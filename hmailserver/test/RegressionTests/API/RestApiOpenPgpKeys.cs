// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Text;
using System.Text.RegularExpressions;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;
using hMailServer;

namespace RegressionTests.API
{
   /// <summary>
   ///    The webmail's OpenPGP key ring (hm_pgpring, OpenPgpRing.h,
   ///    RestApiOpenPgpKeys.cpp): the account's own keys with their secret halves,
   ///    its correspondents' public keys, the lookup this server makes on the
   ///    page's behalf, and the Autocrypt header a send may carry.
   ///
   ///    The CRYPTOGRAPHY is not under test here - it is the page's, and
   ///    build/check-portal-openpgp.js holds it to GnuPG in both directions. What
   ///    is under test is what this server does with what the page hands it: that
   ///    every field of a row is read out of the KEY rather than taken from the
   ///    request, that a secret key with no passphrase on it is refused rather
   ///    than stored, that the two halves of a row have to be one key, that a
   ///    correspondent's row remembers where it came from and when it was first
   ///    seen, and that the lookup route answers a key without keeping it.
   ///
   ///    Every key below was made by GnuPG (OpenPgpTestSupport, and
   ///    build/make-openpgp-fixtures.sh beside it).
   /// </summary>
   [TestFixture]
   public class RestApiOpenPgpKeys : TestFixtureBase
   {
      private static int RestPort = 9766;
      private const int KeyserverPort = 9767;
      private const string AdminPassword = "testar";
      private const string Password = "OpenPGP-Passw0rd!";

      private Account _account;
      private string _me;

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(AdminPassword);
         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();
         Assert.AreEqual(200, Admin("GET", "/api/v1/status", null).status, "REST API did not answer /api/v1/status.");

         _account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "pgpring@" + _domain.Name, Password);
         _me = Basic(_account.Address, Password);
      }

      [TearDown]
      public void StopRestApi()
      {
         try
         {
            // Whatever a test switched, put back: a later fixture that found the
            // page's OpenPGP switched off, or a keyserver pointed at a port
            // nothing is listening on, would fail for a reason nothing names.
            Admin("PUT", "/api/v1/settings/openpgp",
               "{\"portal_enabled\":true,\"lookup_wkd\":true,\"lookup_keyserver\":false,\"keyserver_url\":\"https://keys.openpgp.org\",\"lookup_timeout\":10}");
         }
         finally
         {
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      [Test]
      [Description("An account with no keys is answered an empty ring, with the limits and what lookup the administrator allows.")]
      public void TheRingIsEmptyUntilSomethingIsPutInIt()
      {
         (int status, string body) listed = Send("GET", "/api/v1/me/openpgp/keys", _me, null);
         Assert.AreEqual(200, listed.status, listed.body);
         StringAssert.Contains("\"own\":[]", listed.body);
         StringAssert.Contains("\"correspondents\":[]", listed.body);
         StringAssert.Contains("\"limits\":{\"own\":20,\"correspondents\":500}", listed.body);
         StringAssert.Contains("\"lookup\":{\"wkd\":true,\"keyserver\":false}", listed.body);
      }

      [Test]
      [Description("A key of the account's own is stored with its secret half, and every field of the row - the fingerprint above all - is read out of the key rather than taken from the request.")]
      public void AKeyOfTheAccountsOwnIsStoredWithItsSecretHalf()
      {
         // A fingerprint and an algorithm the client got wrong on purpose: the
         // row must say what the KEY says, not what the request said, or a key
         // could be filed under somebody else's fingerprint and encrypted to.
         (int status, string body) stored = Send("PUT", "/api/v1/me/openpgp/keys/own", _me,
            "{\"public_key\":\"" + OpenPgpTestSupport.RingOwnPublicBase64 + "\","
            + "\"secret_key\":\"" + Escaped(OpenPgpTestSupport.RingOwnSecretArmoured) + "\","
            + "\"address\":\"" + OpenPgpTestSupport.RingOwnAddress + "\",\"name\":\"Alice\",\"source\":\"imported\","
            + "\"fingerprint\":\"" + new string('0', 40) + "\",\"algorithm\":\"RSA 8192\",\"created\":1}");
         Assert.AreEqual(201, stored.status, stored.body);
         StringAssert.Contains("\"fingerprint\":\"" + OpenPgpTestSupport.RingOwnFingerprint + "\"", stored.body);
         StringAssert.DoesNotContain(new string('0', 40), stored.body, "The fingerprint is the key's, not the request's.");
         StringAssert.Contains("\"key_id\":\"" + OpenPgpTestSupport.RingOwnFingerprint.Substring(24) + "\"", stored.body);
         StringAssert.Contains("\"algorithm\":\"EdDSA\"", stored.body, "The algorithm is the one the key packet names.");
         StringAssert.Contains("\"source\":\"imported\"", stored.body);
         StringAssert.Contains("\"revoked\":false", stored.body);
         Assert.IsTrue(Regex.IsMatch(stored.body, "\"created\":1[0-9]{9}"), "The creation time is the key's: " + stored.body);

         (int status, string body) listed = Send("GET", "/api/v1/me/openpgp/keys", _me, null);
         Assert.AreEqual(200, listed.status, listed.body);
         StringAssert.Contains("\"fingerprint\":\"" + OpenPgpTestSupport.RingOwnFingerprint + "\"", listed.body);
         // The secret half comes back exactly as it was stored. This server
         // cannot open it, so nothing here would fail if it came back changed -
         // and the account would simply never read its own mail again.
         StringAssert.Contains("-----BEGIN PGP PRIVATE KEY BLOCK-----", listed.body);
         StringAssert.Contains("\"secret_key\":\"" + Escaped(OpenPgpTestSupport.RingOwnSecretArmoured) + "\"", listed.body);

         // Put again: the same key replaces its row rather than making a second.
         (int status, string body) again = Send("PUT", "/api/v1/me/openpgp/keys/own", _me,
            "{\"public_key\":\"" + OpenPgpTestSupport.RingOwnPublicBase64 + "\","
            + "\"secret_key\":\"" + Escaped(OpenPgpTestSupport.RingOwnSecretArmoured) + "\",\"source\":\"imported\"}");
         Assert.AreEqual(200, again.status, again.body);
         Assert.AreEqual(1, Regex.Matches(Send("GET", "/api/v1/me/openpgp/keys", _me, null).body,
            OpenPgpTestSupport.RingOwnFingerprint).Count, "One row, replaced rather than added.");
      }

      [Test]
      [Description("A secret key with no passphrase on it is refused rather than stored: the encryption happens in the browser, so this is the one place that can insist on it.")]
      public void ASecretKeyInTheClearIsRefused()
      {
         (int status, string body) refused = Send("PUT", "/api/v1/me/openpgp/keys/own", _me,
            "{\"public_key\":\"" + OpenPgpTestSupport.RingOwnPublicBase64 + "\","
            + "\"secret_key\":\"" + Escaped(OpenPgpTestSupport.RingNakedSecretArmoured) + "\",\"source\":\"imported\"}");
         Assert.AreEqual(400, refused.status, refused.body);
         StringAssert.Contains("NOT encrypted", refused.body);
         StringAssert.Contains("\"own\":[]", Send("GET", "/api/v1/me/openpgp/keys", _me, null).body);
      }

      [Test]
      [Description("The public half and the secret half of one row have to be the same key, and a key of the account's own needs a secret half at all.")]
      public void TheTwoHalvesOfARowHaveToBeOneKey()
      {
         (int status, string body) mismatched = Send("PUT", "/api/v1/me/openpgp/keys/own", _me,
            "{\"public_key\":\"" + OpenPgpTestSupport.RingOwnPublicBase64 + "\","
            + "\"secret_key\":\"" + Escaped(OpenPgpTestSupport.RingPeerSecretArmoured) + "\",\"source\":\"imported\"}");
         Assert.AreEqual(400, mismatched.status, mismatched.body);
         StringAssert.Contains("not the same key", mismatched.body);

         (int status, string body) halfOnly = Send("PUT", "/api/v1/me/openpgp/keys/own", _me,
            "{\"public_key\":\"" + OpenPgpTestSupport.RingOwnPublicBase64 + "\",\"source\":\"imported\"}");
         Assert.AreEqual(400, halfOnly.status, halfOnly.body);
         StringAssert.Contains("secret_key", halfOnly.body);

         (int status, string body) notAKey = Send("PUT", "/api/v1/me/openpgp/keys/own", _me,
            "{\"public_key\":\"" + OpenPgpTestSupport.MalformedKeyBase64 + "\","
            + "\"secret_key\":\"" + Escaped(OpenPgpTestSupport.RingOwnSecretArmoured) + "\",\"source\":\"imported\"}");
         Assert.AreEqual(400, notAKey.status, notAKey.body);

         // And the key ring refuses a SECRET key as a correspondent's outright:
         // a correspondent's key is a public key and there is no secret half.
         (int status, string body) peerSecret = Send("PUT", "/api/v1/me/openpgp/keys/correspondents", _me,
            "{\"public_key\":\"" + OpenPgpTestSupport.RingPeerPublicBase64 + "\","
            + "\"secret_key\":\"" + Escaped(OpenPgpTestSupport.RingPeerSecretArmoured) + "\",\"source\":\"imported\"}");
         Assert.AreEqual(400, peerSecret.status, peerSecret.body);
         StringAssert.Contains("there is no secret half", peerSecret.body);
      }

      [Test]
      [Description("A correspondent's key remembers where it came from and when it was first seen; a second sighting moves last_seen and leaves first_seen alone, and it never carries a secret half.")]
      public void ACorrespondentsKeyRemembersWhereItCameFrom()
      {
         (int status, string body) learned = Send("PUT", "/api/v1/me/openpgp/keys/correspondents", _me,
            "{\"public_key\":\"" + OpenPgpTestSupport.RingPeerPublicBase64 + "\",\"source\":\"autocrypt\",\"prefer_encrypt\":\"mutual\"}");
         Assert.AreEqual(201, learned.status, learned.body);
         StringAssert.Contains("\"fingerprint\":\"" + OpenPgpTestSupport.RingPeerFingerprint + "\"", learned.body);
         // The address is the key's own user id, since the request named none.
         StringAssert.Contains("\"address\":\"" + OpenPgpTestSupport.RingPeerAddress + "\"", learned.body);
         StringAssert.Contains("\"source\":\"autocrypt\"", learned.body);
         StringAssert.Contains("\"prefer_encrypt\":\"mutual\"", learned.body);
         StringAssert.DoesNotContain("\"secret_key\"", learned.body, "A correspondent's row has no secret half at all.");
         long firstSeen = long.Parse(Regex.Match(learned.body, "\"first_seen\":(\\d+)").Groups[1].Value);
         Assert.Greater(firstSeen, 1700000000L, learned.body);

         System.Threading.Thread.Sleep(1100);
         (int status, string body) again = Send("PUT", "/api/v1/me/openpgp/keys/correspondents", _me,
            "{\"public_key\":\"" + OpenPgpTestSupport.RingPeerPublicBase64 + "\",\"source\":\"wkd\"}");
         Assert.AreEqual(200, again.status, again.body);
         Assert.AreEqual(firstSeen, long.Parse(Regex.Match(again.body, "\"first_seen\":(\\d+)").Groups[1].Value),
            "First seen is when it was FIRST seen, whatever brought it back.");
         Assert.Greater(long.Parse(Regex.Match(again.body, "\"last_seen\":(\\d+)").Groups[1].Value), firstSeen,
            "Last seen moves: " + again.body);
         StringAssert.Contains("\"source\":\"wkd\"", again.body);

         (int status, string body) badSource = Send("PUT", "/api/v1/me/openpgp/keys/correspondents", _me,
            "{\"public_key\":\"" + OpenPgpTestSupport.RingPeerPublicBase64 + "\",\"source\":\"somewhere\"}");
         Assert.AreEqual(400, badSource.status, badSource.body);
      }

      [Test]
      [Description("A key is removed by its fingerprint, from the list it is in and no other, and removing one that is not there is a 404.")]
      public void AKeyIsRemovedByItsFingerprint()
      {
         Assert.AreEqual(201, Send("PUT", "/api/v1/me/openpgp/keys/own", _me,
            "{\"public_key\":\"" + OpenPgpTestSupport.RingOwnPublicBase64 + "\","
            + "\"secret_key\":\"" + Escaped(OpenPgpTestSupport.RingOwnSecretArmoured) + "\",\"source\":\"generated\"}").status);
         Assert.AreEqual(201, Send("PUT", "/api/v1/me/openpgp/keys/correspondents", _me,
            "{\"public_key\":\"" + OpenPgpTestSupport.RingPeerPublicBase64 + "\",\"source\":\"imported\"}").status);

         // The same fingerprint in the other list is not this row.
         Assert.AreEqual(404, Send("DELETE", "/api/v1/me/openpgp/keys/correspondents/" + OpenPgpTestSupport.RingOwnFingerprint, _me, null).status);
         Assert.AreEqual(404, Send("DELETE", "/api/v1/me/openpgp/keys/own/" + new string('e', 40), _me, null).status);
         Assert.AreEqual(404, Send("DELETE", "/api/v1/me/openpgp/keys/own/not-a-fingerprint", _me, null).status);

         Assert.AreEqual(200, Send("DELETE", "/api/v1/me/openpgp/keys/own/" + OpenPgpTestSupport.RingOwnFingerprint, _me, null).status);
         Assert.AreEqual(404, Send("DELETE", "/api/v1/me/openpgp/keys/own/" + OpenPgpTestSupport.RingOwnFingerprint, _me, null).status);

         string listed = Send("GET", "/api/v1/me/openpgp/keys", _me, null).body;
         StringAssert.Contains("\"own\":[]", listed);
         StringAssert.Contains("\"fingerprint\":\"" + OpenPgpTestSupport.RingPeerFingerprint + "\"", listed);
      }

      [Test]
      [Description("The lookup route fetches a correspondent's key on the page's behalf, answers it with where it was found, and keeps nothing: the person decides whether to keep it.")]
      public void TheLookupFetchesAKeyAndKeepsNothing()
      {
         using (var keyserver = new FakeHttpEndpoint(KeyserverPort, 404, "", "text/plain"))
         {
            keyserver.SetResponse("/vks/v1/by-email/" + Uri.EscapeDataString(OpenPgpTestSupport.RingPeerAddress),
               200, Encoding.ASCII.GetBytes(OpenPgpTestSupport.Armoured(OpenPgpTestSupport.RingPeerPublicBase64)),
               "application/pgp-keys");
            Assert.AreEqual(200, Admin("PUT", "/api/v1/settings/openpgp",
               "{\"lookup_wkd\":false,\"lookup_keyserver\":true,\"keyserver_url\":\"http://127.0.0.1:" + KeyserverPort + "\"}").status);

            (int status, string body) found = Send("GET", "/api/v1/me/openpgp/lookup?address=" + OpenPgpTestSupport.RingPeerAddress, _me, null);
            Assert.AreEqual(200, found.status, found.body);
            StringAssert.Contains("\"found\":true", found.body);
            StringAssert.Contains("\"source\":\"keyserver\"", found.body);
            StringAssert.Contains("\"fingerprint\":\"" + OpenPgpTestSupport.RingPeerFingerprint + "\"", found.body);
            StringAssert.Contains("\"address\":\"" + OpenPgpTestSupport.RingPeerAddress + "\"", found.body);
            Assert.IsTrue(keyserver.Requests.Count >= 1, "The server fetched it.");
            StringAssert.Contains("/vks/v1/by-email/", keyserver.Requests[0]);

            // Nothing was kept: the page decides, with the fingerprint in front
            // of the person, because a key nobody looked at is a key nobody
            // has any reason to trust.
            StringAssert.Contains("\"correspondents\":[]", Send("GET", "/api/v1/me/openpgp/keys", _me, null).body);

            // An address the keyserver has no key for: found false with a
            // sentence, which is the ordinary case rather than a failure.
            (int status, string body) missing = Send("GET", "/api/v1/me/openpgp/lookup?address=nobody@fixtures.test", _me, null);
            Assert.AreEqual(200, missing.status, missing.body);
            StringAssert.Contains("\"found\":false", missing.body);
            StringAssert.Contains("\"status\":404", missing.body);

            Assert.AreEqual(400, Send("GET", "/api/v1/me/openpgp/lookup?address=not-an-address", _me, null).status);
            Assert.AreEqual(400, Send("GET", "/api/v1/me/openpgp/lookup", _me, null).status);
         }
      }

      [Test]
      [Description("With both lookups switched off the route refuses by name rather than quietly finding nothing, and a keyserver address that is neither https nor on this machine is refused before it is stored.")]
      public void TheLookupIsRefusedWhenTheAdministratorAllowsNone()
      {
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings/openpgp", "{\"lookup_wkd\":false,\"lookup_keyserver\":false}").status);
         (int status, string body) refused = Send("GET", "/api/v1/me/openpgp/lookup?address=" + OpenPgpTestSupport.RingPeerAddress, _me, null);
         Assert.AreEqual(403, refused.status, refused.body);
         StringAssert.Contains("lookup_unavailable", refused.body);

         (int status, string body) plain = Admin("PUT", "/api/v1/settings/openpgp", "{\"keyserver_url\":\"http://keys.example.org\"}");
         Assert.AreEqual(400, plain.status, plain.body);
         StringAssert.Contains("keyserver_url", plain.body);
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings/openpgp", "{\"keyserver_url\":\"http://127.0.0.1:" + KeyserverPort + "\"}").status,
            "A keyserver on this machine is reachable in the clear, because there is nothing between it and this server.");
      }

      [Test]
      [Description("The address the lookup is asked for goes into a URL this server sends, and the validator admits a quoted local part with a control character in it. One is refused before any request is made, so a caller cannot write into the request line of a fetch this server makes.")]
      public void TheLookupRefusesAnAddressWithAControlCharacterInIt()
      {
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings/openpgp", "{\"lookup_wkd\":true}").status);
         // "a<CR><LF>X-Foo: bar"@example.test, percent-encoded as a browser would send it.
         (int status, string body) refused = Send("GET", "/api/v1/me/openpgp/lookup?address=%22a%0d%0aX-Foo:%20bar%22%40example.test", _me, null);
         Assert.AreEqual(400, refused.status, refused.body);
         StringAssert.Contains("address", refused.body);
      }

      [Test]
      [Description("One switch decides whether the webmail offers OpenPGP at all: with it off the ring, the lookup and the module are refused, and the keys already stored are untouched.")]
      public void OneSwitchDecidesWhetherTheWebmailOffersOpenPgpAtAll()
      {
         Assert.AreEqual(201, Send("PUT", "/api/v1/me/openpgp/keys/correspondents", _me,
            "{\"public_key\":\"" + OpenPgpTestSupport.RingPeerPublicBase64 + "\",\"source\":\"imported\"}").status);

         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings/openpgp", "{\"portal_enabled\":false}").status);
         (int status, string body) off = Send("GET", "/api/v1/me/openpgp/keys", _me, null);
         Assert.AreEqual(403, off.status, off.body);
         StringAssert.Contains("openpgp_unavailable", off.body);
         Assert.AreEqual(403, Send("PUT", "/api/v1/me/openpgp/keys/correspondents", _me,
            "{\"public_key\":\"" + OpenPgpTestSupport.RingPeerPublicBase64 + "\",\"source\":\"imported\"}").status);
         Assert.AreEqual(403, Send("DELETE", "/api/v1/me/openpgp/keys/correspondents/" + OpenPgpTestSupport.RingPeerFingerprint, _me, null).status);
         Assert.AreEqual(403, Send("GET", "/api/v1/me/openpgp/lookup?address=" + OpenPgpTestSupport.RingPeerAddress, _me, null).status);

         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings/openpgp", "{\"portal_enabled\":true}").status);
         StringAssert.Contains("\"fingerprint\":\"" + OpenPgpTestSupport.RingPeerFingerprint + "\"",
            Send("GET", "/api/v1/me/openpgp/keys", _me, null).body,
            "The row was never touched by the switch.");
      }

      [Test]
      [Description("A send may carry an Autocrypt header, which is written above the message as an RFC 5322 header; a value that would break a line into a header of its own is refused before anything is written.")]
      public void AnAutocryptHeaderIsWrittenOnTheMessageAndCannotInjectAnother()
      {
         string header = "addr=" + _account.Address + "; prefer-encrypt=mutual; keydata=" + OpenPgpTestSupport.RingOwnPublicBase64.Substring(0, 60);
         (int status, string body) sent = Send("POST", "/api/v1/me/messages", _me,
            "{\"to\":\"" + _account.Address + "\",\"subject\":\"With a key\",\"text\":\"Nothing secret.\","
            + "\"autocrypt\":\"" + header + "\"}");
         Assert.AreEqual(201, sent.status, sent.body);

         string message = Pop3ClientSimulator.AssertGetFirstMessageText(_account.Address, Password);
         StringAssert.Contains("Autocrypt: addr=" + _account.Address, message);
         StringAssert.Contains("prefer-encrypt=mutual", message);
         StringAssert.Contains("Nothing secret.", message.Replace("=\r\n", ""));

         // A bare newline in a header value a client chose is header injection.
         (int status, string body) injected = Send("POST", "/api/v1/me/messages", _me,
            "{\"to\":\"" + _account.Address + "\",\"subject\":\"Injected\",\"text\":\"x\","
            + "\"autocrypt\":\"addr=a@b.test\\r\\nBcc: somebody@elsewhere.test\"}");
         Assert.AreEqual(400, injected.status, injected.body);
         StringAssert.Contains("autocrypt", injected.body);

         // And a value that is not an Autocrypt header at all.
         Assert.AreEqual(400, Send("POST", "/api/v1/me/messages", _me,
            "{\"to\":\"" + _account.Address + "\",\"subject\":\"Wrong\",\"text\":\"x\",\"autocrypt\":\"keydata=AAAA\"}").status);
      }

      [Test]
      [Description("The page's OpenPGP module is served at its own path, unauthenticated as the page's other scripts are, and is the file the checker runs.")]
      public void TheModuleIsServedAtItsOwnPath()
      {
         (int status, string body) script = Send("GET", "/portal-openpgp.js", null, null);
         Assert.AreEqual(200, script.status, script.body.Substring(0, Math.Min(200, script.body.Length)));
         StringAssert.Contains("root.hmOpenPgp = factory", script.body);
         StringAssert.Contains("readPackets", script.body);
         Assert.Greater(script.body.Length, 50000, "The whole module, not a stub.");

         // And it is not served at all where the webmail's OpenPGP is switched
         // off: an installation that wants none of this is not handing a
         // hundred kilobytes of cryptography to whoever asks.
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings/openpgp", "{\"portal_enabled\":false}").status);
         Assert.AreEqual(404, Send("GET", "/portal-openpgp.js", null, null).status);
      }

      [Test]
      [Description("The ring is the account's own: another account's credentials see their own ring and never this one's.")]
      public void TheRingBelongsToTheAccountThatSignedIn()
      {
         Account other = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "pgpring-other@" + _domain.Name, Password);
         Assert.AreEqual(201, Send("PUT", "/api/v1/me/openpgp/keys/correspondents", _me,
            "{\"public_key\":\"" + OpenPgpTestSupport.RingPeerPublicBase64 + "\",\"source\":\"imported\"}").status);

         string theirs = Send("GET", "/api/v1/me/openpgp/keys", Basic(other.Address, Password), null).body;
         StringAssert.Contains("\"correspondents\":[]", theirs);
         Assert.AreEqual(404, Send("DELETE", "/api/v1/me/openpgp/keys/correspondents/" + OpenPgpTestSupport.RingPeerFingerprint,
            Basic(other.Address, Password), null).status);
         StringAssert.Contains("\"fingerprint\":\"" + OpenPgpTestSupport.RingPeerFingerprint + "\"",
            Send("GET", "/api/v1/me/openpgp/keys", _me, null).body);
      }

      // ---- helpers ---------------------------------------------------------

      /// <summary>An armoured block as one JSON string: its newlines escaped.</summary>
      private static string Escaped(string armoured)
      {
         return armoured.Replace("\\", "\\\\").Replace("\"", "\\\"").Replace("\r", "\\r").Replace("\n", "\\n");
      }

      private static string Basic(string user, string password)
      {
         return "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(user + ":" + password));
      }

      private static (int status, string body) Admin(string method, string path, string requestBody)
      {
         return Send(method, path, Basic("Administrator", AdminPassword), requestBody);
      }

      private static (int status, string body) Send(string method, string path, string authorization, string requestBody)
      {
         return RestRequest.Send("127.0.0.1", RestPort, method, path, authorization, requestBody);
      }
   }
}
