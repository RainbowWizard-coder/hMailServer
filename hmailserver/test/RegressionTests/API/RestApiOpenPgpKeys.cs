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
   ///    And trust made visible (schema 6081): that a DIFFERENT key arriving
   ///    for an address already held is filed as a change naming the key it
   ///    replaced, and stays unaccepted until the account says otherwise; that
   ///    the "verified in person" mark is the account's own word, said with its
   ///    own session and never an app password's; and that the mark is never
   ///    inherited by the key that replaced a verified one.
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

      [Test]
      [Description("A correspondent's key is remembered from the first time it is seen: the same key seen again keeps its first_seen and carries no change, and a DIFFERENT key arriving for the same address is filed as a change - changed_from names the key it replaced, accepted is false - until the person accepts it. A key for another address is no change at all.")]
      public void TheFirstKeySeenIsRememberedAndADifferentOneIsAChange()
      {
         (int status, string body) first = Send("PUT", "/api/v1/me/openpgp/keys/correspondents", _me,
            "{\"public_key\":\"" + OpenPgpTestSupport.RingPeerPublicBase64 + "\",\"source\":\"autocrypt\"}");
         Assert.AreEqual(201, first.status, first.body);
         StringAssert.Contains("\"verified\":false", first.body);
         StringAssert.Contains("\"verified_at\":0", first.body);
         StringAssert.DoesNotContain("changed_from", first.body, "The first key an address has replaced nothing.");
         long firstSeen = long.Parse(Regex.Match(first.body, "\"first_seen\":(\\d+)").Groups[1].Value);

         // The same key again, brought by something else: not a change, and the
         // first-seen fingerprint does not move.
         (int status, string body) again = Send("PUT", "/api/v1/me/openpgp/keys/correspondents", _me,
            "{\"public_key\":\"" + OpenPgpTestSupport.RingPeerPublicBase64 + "\",\"source\":\"wkd\"}");
         Assert.AreEqual(200, again.status, again.body);
         StringAssert.DoesNotContain("changed_from", again.body, "The same key seen again is not a change.");
         Assert.AreEqual(firstSeen, long.Parse(Regex.Match(again.body, "\"first_seen\":(\\d+)").Groups[1].Value));

         // A different key for the SAME address: Joe Doe's key filed under bob's
         // address by the request, which is what a lookup that answered another
         // key, an import over the one held, or a kept Autocrypt key does.
         (int status, string body) changed = Send("PUT", "/api/v1/me/openpgp/keys/correspondents", _me,
            "{\"public_key\":\"" + OpenPgpTestSupport.OtherKeyBase64 + "\",\"address\":\"" + OpenPgpTestSupport.RingPeerAddress + "\",\"source\":\"imported\"}");
         Assert.AreEqual(201, changed.status, changed.body);
         StringAssert.Contains("\"fingerprint\":\"" + OpenPgpTestSupport.OtherKeyFingerprint + "\"", changed.body);
         StringAssert.Contains("\"changed_from\":\"" + OpenPgpTestSupport.RingPeerFingerprint + "\"", changed.body, "The change names the key it replaced.");
         StringAssert.Contains("\"accepted\":false", changed.body);
         Assert.IsTrue(Regex.IsMatch(changed.body, "\"changed_at\":1[0-9]{9}"), changed.body);
         long changedId = long.Parse(Regex.Match(changed.body, "\"id\":(\\d+)").Groups[1].Value);

         // The key it replaced is untouched: still the first key seen, no change on it.
         string bobRow = RowOf(Send("GET", "/api/v1/me/openpgp/keys", _me, null).body, OpenPgpTestSupport.RingPeerFingerprint);
         StringAssert.DoesNotContain("changed_from", bobRow);
         StringAssert.Contains("\"first_seen\":" + firstSeen, bobRow);

         // Negative control: a key for ANOTHER address replaced nobody's.
         (int status, string body) other = Send("PUT", "/api/v1/me/openpgp/keys/correspondents", _me,
            "{\"public_key\":\"" + OpenPgpTestSupport.SuiteKeyBase64 + "\",\"source\":\"imported\"}");
         Assert.AreEqual(201, other.status, other.body);
         StringAssert.DoesNotContain("changed_from", other.body);

         // Accepting keeps the change on record and turns accepted true.
         (int status, string body) accepted = Send("PUT", "/api/v1/me/openpgp/keys/" + changedId + "/accepted", _me, "{\"accepted\":true}");
         Assert.AreEqual(200, accepted.status, accepted.body);
         StringAssert.Contains("\"changed_from\":\"" + OpenPgpTestSupport.RingPeerFingerprint + "\"", accepted.body);
         StringAssert.Contains("\"accepted\":true", accepted.body);
         StringAssert.Contains("\"accepted\":true", RowOf(Send("GET", "/api/v1/me/openpgp/keys", _me, null).body, OpenPgpTestSupport.OtherKeyFingerprint));

         // A key that replaced nothing has no change to accept; a value that is
         // not a boolean is refused; a row that is not there is 404.
         long bobId = long.Parse(Regex.Match(bobRow, "\"id\":(\\d+)").Groups[1].Value);
         (int status, string body) nothingToAccept = Send("PUT", "/api/v1/me/openpgp/keys/" + bobId + "/accepted", _me, "{\"accepted\":true}");
         Assert.AreEqual(400, nothingToAccept.status, nothingToAccept.body);
         StringAssert.Contains("did not replace", nothingToAccept.body);
         Assert.AreEqual(400, Send("PUT", "/api/v1/me/openpgp/keys/" + changedId + "/accepted", _me, "{\"accepted\":\"yes\"}").status);
         Assert.AreEqual(404, Send("PUT", "/api/v1/me/openpgp/keys/999999/accepted", _me, "{\"accepted\":true}").status);
      }

      [Test]
      [Description("Marking a key verified in person is the account's own word, said with its own session: an app password is refused and changes nothing, another account's credential finds no such key, a key of the account's own has nothing to verify, and the mark comes off again the same way it went on.")]
      public void MarkingAKeyVerifiedNeedsTheAccountsOwnSession()
      {
         (int status, string body) stored = Send("PUT", "/api/v1/me/openpgp/keys/correspondents", _me,
            "{\"public_key\":\"" + OpenPgpTestSupport.RingPeerPublicBase64 + "\",\"source\":\"imported\"}");
         Assert.AreEqual(201, stored.status, stored.body);
         long id = long.Parse(Regex.Match(stored.body, "\"id\":(\\d+)").Groups[1].Value);

         (int status, string body) marked = Send("PUT", "/api/v1/me/openpgp/keys/" + id + "/verified", _me, "{\"verified\":true}");
         Assert.AreEqual(200, marked.status, marked.body);
         StringAssert.Contains("\"verified\":true", marked.body);
         Assert.IsTrue(Regex.IsMatch(marked.body, "\"verified_at\":1[0-9]{9}"), marked.body);
         StringAssert.DoesNotContain("changed_from", marked.body, "A key that replaced nothing carries no change, marked or not.");
         StringAssert.Contains("\"verified\":true", RowOf(Send("GET", "/api/v1/me/openpgp/keys", _me, null).body, OpenPgpTestSupport.RingPeerFingerprint));

         // Off again the same way.
         (int status, string body) unmarked = Send("PUT", "/api/v1/me/openpgp/keys/" + id + "/verified", _me, "{\"verified\":false}");
         Assert.AreEqual(200, unmarked.status, unmarked.body);
         StringAssert.Contains("\"verified\":false", unmarked.body);
         StringAssert.Contains("\"verified_at\":0", unmarked.body);

         // An app password is a credential for a program that reads and sends
         // mail, and a program has no way of knowing that a fingerprint was
         // read back to its owner: refused, and the key stays unverified. The
         // same credential still reads the ring - only the writes are refused.
         (int status, string body) minted = Send("POST", "/api/v1/me/app-passwords", _me, "{\"name\":\"Phone\",\"password\":\"" + Password + "\"}");
         Assert.AreEqual(201, minted.status, minted.body);
         string asProgram = Basic(_account.Address, Regex.Match(minted.body, "\"password\":\"([^\"]+)\"").Groups[1].Value);
         (int status, string body) refused = Send("PUT", "/api/v1/me/openpgp/keys/" + id + "/verified", asProgram, "{\"verified\":true}");
         Assert.AreEqual(403, refused.status, refused.body);
         StringAssert.Contains("app password", refused.body);
         Assert.AreEqual(200, Send("GET", "/api/v1/me/openpgp/keys", asProgram, null).status);
         StringAssert.Contains("\"verified\":false", RowOf(Send("GET", "/api/v1/me/openpgp/keys", _me, null).body, OpenPgpTestSupport.RingPeerFingerprint));

         // And the four writes to the ring itself are refused on it too, or
         // the refusal above would be decorative: forget the row and store the
         // key again and the mark is gone, or a change made to look like a
         // first sighting. Nothing in the ring moved.
         string before = Send("GET", "/api/v1/me/openpgp/keys", _me, null).body;
         (int status, string body) storeRefused = Send("PUT", "/api/v1/me/openpgp/keys/correspondents", asProgram,
            "{\"public_key\":\"" + OpenPgpTestSupport.SuiteKeyBase64 + "\",\"source\":\"imported\"}");
         Assert.AreEqual(403, storeRefused.status, storeRefused.body);
         StringAssert.Contains("app password", storeRefused.body);
         Assert.AreEqual(403, Send("PUT", "/api/v1/me/openpgp/keys/own", asProgram,
            "{\"public_key\":\"" + OpenPgpTestSupport.RingOwnPublicBase64 + "\","
            + "\"secret_key\":\"" + Escaped(OpenPgpTestSupport.RingOwnSecretArmoured) + "\",\"source\":\"generated\"}").status);
         (int status, string body) forgetRefused = Send("DELETE", "/api/v1/me/openpgp/keys/correspondents/" + OpenPgpTestSupport.RingPeerFingerprint, asProgram, null);
         Assert.AreEqual(403, forgetRefused.status, forgetRefused.body);
         Assert.AreEqual(403, Send("DELETE", "/api/v1/me/openpgp/keys/own/" + OpenPgpTestSupport.RingOwnFingerprint, asProgram, null).status,
            "Refused before the row is looked for: a program does not learn from a 404 whether the key is there.");
         Assert.AreEqual(before, Send("GET", "/api/v1/me/openpgp/keys", _me, null).body, "The ring is exactly as it was.");

         // Another account's credential: the row id is this account's, so over
         // there it is no key at all - and nothing here changed.
         Account other = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "pgpring-other@" + _domain.Name, Password);
         (int status, string body) theirs = Send("PUT", "/api/v1/me/openpgp/keys/" + id + "/verified", Basic(other.Address, Password), "{\"verified\":true}");
         Assert.AreEqual(404, theirs.status, theirs.body);
         StringAssert.Contains("\"verified\":false", RowOf(Send("GET", "/api/v1/me/openpgp/keys", _me, null).body, OpenPgpTestSupport.RingPeerFingerprint));

         // A key of the account's own is nobody's correspondent.
         (int status, string body) own = Send("PUT", "/api/v1/me/openpgp/keys/own", _me,
            "{\"public_key\":\"" + OpenPgpTestSupport.RingOwnPublicBase64 + "\","
            + "\"secret_key\":\"" + Escaped(OpenPgpTestSupport.RingOwnSecretArmoured) + "\",\"source\":\"generated\"}");
         Assert.AreEqual(201, own.status, own.body);
         long ownId = long.Parse(Regex.Match(own.body, "\"id\":(\\d+)").Groups[1].Value);
         (int status, string body) notACorrespondent = Send("PUT", "/api/v1/me/openpgp/keys/" + ownId + "/verified", _me, "{\"verified\":true}");
         Assert.AreEqual(400, notACorrespondent.status, notACorrespondent.body);
         StringAssert.Contains("nothing to verify", notACorrespondent.body);

         // Not a boolean, and no such row.
         Assert.AreEqual(400, Send("PUT", "/api/v1/me/openpgp/keys/" + id + "/verified", _me, "{\"verified\":1}").status);
         Assert.AreEqual(404, Send("PUT", "/api/v1/me/openpgp/keys/999999/verified", _me, "{\"verified\":true}").status);
      }

      [Test]
      [Description("The mark is never inherited: a different key arriving for a verified correspondent's address is a new, unverified row filed as a change, while the same key seen again keeps its mark; and verifying the new key accepts the change in the same act, a fingerprint read back to its owner being the stronger of the two.")]
      public void TheMarkIsClearedWhenTheKeyChanges()
      {
         (int status, string body) stored = Send("PUT", "/api/v1/me/openpgp/keys/correspondents", _me,
            "{\"public_key\":\"" + OpenPgpTestSupport.RingPeerPublicBase64 + "\",\"source\":\"imported\"}");
         Assert.AreEqual(201, stored.status, stored.body);
         long id = long.Parse(Regex.Match(stored.body, "\"id\":(\\d+)").Groups[1].Value);
         (int status, string body) marked = Send("PUT", "/api/v1/me/openpgp/keys/" + id + "/verified", _me, "{\"verified\":true}");
         Assert.AreEqual(200, marked.status, marked.body);
         string verifiedAt = Regex.Match(marked.body, "\"verified_at\":(\\d+)").Groups[1].Value;

         // Negative control: the same key seen again keeps the mark, and when it was made.
         (int status, string body) again = Send("PUT", "/api/v1/me/openpgp/keys/correspondents", _me,
            "{\"public_key\":\"" + OpenPgpTestSupport.RingPeerPublicBase64 + "\",\"source\":\"autocrypt\"}");
         Assert.AreEqual(200, again.status, again.body);
         StringAssert.Contains("\"verified\":true", again.body, "Seeing the same key again does not clear the mark.");
         StringAssert.Contains("\"verified_at\":" + verifiedAt, again.body);

         // A different key for the address: unverified, and filed as a change.
         (int status, string body) changed = Send("PUT", "/api/v1/me/openpgp/keys/correspondents", _me,
            "{\"public_key\":\"" + OpenPgpTestSupport.OtherKeyBase64 + "\",\"address\":\"" + OpenPgpTestSupport.RingPeerAddress + "\",\"source\":\"imported\"}");
         Assert.AreEqual(201, changed.status, changed.body);
         StringAssert.Contains("\"verified\":false", changed.body, "A new key is not verified because the old one was.");
         StringAssert.Contains("\"verified_at\":0", changed.body);
         StringAssert.Contains("\"changed_from\":\"" + OpenPgpTestSupport.RingPeerFingerprint + "\"", changed.body);
         StringAssert.Contains("\"accepted\":false", changed.body);
         long changedId = long.Parse(Regex.Match(changed.body, "\"id\":(\\d+)").Groups[1].Value);

         // The key the person verified is still the key the person verified.
         StringAssert.Contains("\"verified\":true", RowOf(Send("GET", "/api/v1/me/openpgp/keys", _me, null).body, OpenPgpTestSupport.RingPeerFingerprint));

         // Verifying the new key - both fingerprints compared with its owner -
         // accepts the change in the same act.
         (int status, string body) verified = Send("PUT", "/api/v1/me/openpgp/keys/" + changedId + "/verified", _me, "{\"verified\":true}");
         Assert.AreEqual(200, verified.status, verified.body);
         StringAssert.Contains("\"verified\":true", verified.body);
         StringAssert.Contains("\"accepted\":true", verified.body);
         StringAssert.Contains("\"changed_from\":\"" + OpenPgpTestSupport.RingPeerFingerprint + "\"", verified.body, "The change stays on record.");

         // And unmarking it leaves the acceptance as it was.
         (int status, string body) unmarked = Send("PUT", "/api/v1/me/openpgp/keys/" + changedId + "/verified", _me, "{\"verified\":false}");
         Assert.AreEqual(200, unmarked.status, unmarked.body);
         StringAssert.Contains("\"verified\":false", unmarked.body);
         StringAssert.Contains("\"accepted\":true", unmarked.body);
      }

      [Test]
      [Description("A correspondent's key filed under an address the account holds its OWN key for is a change of the own key, not a first sighting: the page seals every message to the sender's own key, so a stranger's key under that address in silence would have every message sealed to the stranger. And a correspondent's key that carries no address in its user ids, with none in the request, is refused rather than filed anywhere.")]
      public void AKeyForTheAccountsOwnAddressIsAChangeOfTheOwnKey()
      {
         // Negative control: with no own key for the address, a correspondent's
         // key under it is the first key the address has had.
         (int status, string body) first = Send("PUT", "/api/v1/me/openpgp/keys/correspondents", _me,
            "{\"public_key\":\"" + OpenPgpTestSupport.RingPeerPublicBase64 + "\",\"address\":\"" + OpenPgpTestSupport.RingOwnAddress + "\",\"source\":\"autocrypt\"}");
         Assert.AreEqual(201, first.status, first.body);
         StringAssert.DoesNotContain("changed_from", first.body);
         Assert.AreEqual(200, Send("DELETE", "/api/v1/me/openpgp/keys/correspondents/" + OpenPgpTestSupport.RingPeerFingerprint, _me, null).status);

         Assert.AreEqual(201, Send("PUT", "/api/v1/me/openpgp/keys/own", _me,
            "{\"public_key\":\"" + OpenPgpTestSupport.RingOwnPublicBase64 + "\","
            + "\"secret_key\":\"" + Escaped(OpenPgpTestSupport.RingOwnSecretArmoured) + "\",\"source\":\"generated\"}").status);

         // The same correspondent's key under the same address, now that the
         // account holds its own key for it: a change of the OWN key, pending.
         (int status, string body) spoof = Send("PUT", "/api/v1/me/openpgp/keys/correspondents", _me,
            "{\"public_key\":\"" + OpenPgpTestSupport.RingPeerPublicBase64 + "\",\"address\":\"" + OpenPgpTestSupport.RingOwnAddress + "\",\"source\":\"autocrypt\"}");
         Assert.AreEqual(201, spoof.status, spoof.body);
         StringAssert.Contains("\"changed_from\":\"" + OpenPgpTestSupport.RingOwnFingerprint + "\"", spoof.body, "A change of the account's own key.");
         StringAssert.Contains("\"accepted\":false", spoof.body);
         // The own key is untouched by it.
         StringAssert.DoesNotContain("changed_from", RowOf(Send("GET", "/api/v1/me/openpgp/keys", _me, null).body, OpenPgpTestSupport.RingOwnFingerprint));

         // A key whose only user id is a name - bob's key with its user id
         // rewritten to "Stranger" - is refused when the request names no
         // address: there is nowhere to file it, and the page never fills the
         // account's own address in for a correspondent's key.
         (int status, string body) nowhere = Send("PUT", "/api/v1/me/openpgp/keys/correspondents", _me,
            "{\"public_key\":\"" + OpenPgpTestSupport.NoAddressKeyBase64 + "\",\"source\":\"autocrypt\"}");
         Assert.AreEqual(400, nowhere.status, nowhere.body);
         StringAssert.Contains("address", nowhere.body);
      }

      [Test]
      [Description("A key comes back to the address it was filed under, whatever address the request names: the address is the ring's, like first_seen, so a re-store cannot move a row - and its verified mark and its history - to an address they were never about.")]
      public void AKeyComesBackToTheAddressItWasFiledUnder()
      {
         (int status, string body) stored = Send("PUT", "/api/v1/me/openpgp/keys/correspondents", _me,
            "{\"public_key\":\"" + OpenPgpTestSupport.RingPeerPublicBase64 + "\",\"source\":\"imported\"}");
         Assert.AreEqual(201, stored.status, stored.body);
         StringAssert.Contains("\"address\":\"" + OpenPgpTestSupport.RingPeerAddress + "\"", stored.body);
         long id = long.Parse(Regex.Match(stored.body, "\"id\":(\\d+)").Groups[1].Value);
         Assert.AreEqual(200, Send("PUT", "/api/v1/me/openpgp/keys/" + id + "/verified", _me, "{\"verified\":true}").status);

         (int status, string body) again = Send("PUT", "/api/v1/me/openpgp/keys/correspondents", _me,
            "{\"public_key\":\"" + OpenPgpTestSupport.RingPeerPublicBase64 + "\",\"address\":\"carol@fixtures.test\",\"source\":\"wkd\"}");
         Assert.AreEqual(200, again.status, again.body);
         StringAssert.Contains("\"address\":\"" + OpenPgpTestSupport.RingPeerAddress + "\"", again.body, "Filed where it was.");
         StringAssert.DoesNotContain("carol@fixtures.test", again.body);
         StringAssert.Contains("\"verified\":true", again.body, "The mark stays with the row, which stayed where it was.");
         StringAssert.Contains("\"source\":\"wkd\"", again.body, "What the request may change, it changed.");

         string listed = Send("GET", "/api/v1/me/openpgp/keys", _me, null).body;
         Assert.AreEqual(1, Regex.Matches(listed, OpenPgpTestSupport.RingPeerFingerprint).Count, "One row, not a moved one and a copy.");
         StringAssert.DoesNotContain("carol@fixtures.test", listed);
      }

      [Test]
      [Description("Which key a new arrival is a change OF: the address's newest settled key while one is there, and a pending key only when no settled one is left - so that removing the old key does not make the next arrival look like the first.")]
      public void APendingKeyCountsWhenNoSettledOneIsLeft()
      {
         // A, settled; B, a change of A, pending.
         Assert.AreEqual(201, Send("PUT", "/api/v1/me/openpgp/keys/correspondents", _me,
            "{\"public_key\":\"" + OpenPgpTestSupport.RingPeerPublicBase64 + "\",\"source\":\"imported\"}").status);
         (int status, string body) b = Send("PUT", "/api/v1/me/openpgp/keys/correspondents", _me,
            "{\"public_key\":\"" + OpenPgpTestSupport.OtherKeyBase64 + "\",\"address\":\"" + OpenPgpTestSupport.RingPeerAddress + "\",\"source\":\"imported\"}");
         Assert.AreEqual(201, b.status, b.body);
         StringAssert.Contains("\"changed_from\":\"" + OpenPgpTestSupport.RingPeerFingerprint + "\"", b.body);

         // Negative control: with A still settled, a third key C is a change of A, not of the pending B.
         (int status, string body) c = Send("PUT", "/api/v1/me/openpgp/keys/correspondents", _me,
            "{\"public_key\":\"" + OpenPgpTestSupport.SuiteKeyBase64 + "\",\"address\":\"" + OpenPgpTestSupport.RingPeerAddress + "\",\"source\":\"imported\"}");
         Assert.AreEqual(201, c.status, c.body);
         StringAssert.Contains("\"changed_from\":\"" + OpenPgpTestSupport.RingPeerFingerprint + "\"", c.body, "The settled key is preferred over a pending one.");
         Assert.AreEqual(200, Send("DELETE", "/api/v1/me/openpgp/keys/correspondents/" + OpenPgpTestSupport.SuiteKeyFingerprint, _me, null).status);

         // A removed: B, pending, is all that is left for the address, and C
         // arriving now is a change of B - not a first sighting.
         Assert.AreEqual(200, Send("DELETE", "/api/v1/me/openpgp/keys/correspondents/" + OpenPgpTestSupport.RingPeerFingerprint, _me, null).status);
         (int status, string body) cAgain = Send("PUT", "/api/v1/me/openpgp/keys/correspondents", _me,
            "{\"public_key\":\"" + OpenPgpTestSupport.SuiteKeyBase64 + "\",\"address\":\"" + OpenPgpTestSupport.RingPeerAddress + "\",\"source\":\"imported\"}");
         Assert.AreEqual(201, cAgain.status, cAgain.body);
         StringAssert.Contains("\"changed_from\":\"" + OpenPgpTestSupport.OtherKeyFingerprint + "\"", cAgain.body, "A pending key counts when no settled one is left.");
         StringAssert.Contains("\"accepted\":false", cAgain.body);
      }

      // ---- helpers ---------------------------------------------------------

      /// <summary>
      ///    One row of the listing, by its fingerprint: the object around the
      ///    fingerprint's field. A row nests nothing, so the nearest braces are its.
      /// </summary>
      private static string RowOf(string listing, string fingerprint)
      {
         int at = listing.IndexOf("\"fingerprint\":\"" + fingerprint + "\"", StringComparison.Ordinal);
         Assert.IsTrue(at >= 0, "No row with " + fingerprint + " in " + listing);
         int start = listing.LastIndexOf('{', at);
         int end = listing.IndexOf('}', at);
         return listing.Substring(start, end - start + 1);
      }

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
