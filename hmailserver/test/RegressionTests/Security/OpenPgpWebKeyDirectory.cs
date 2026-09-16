// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;

namespace RegressionTests.Security
{
   /// <summary>
   ///    The OpenPGP Web Key Directory (draft-koch-openpgp-webkey-service) on the
   ///    web services listener: both methods, the policy file, and the 404 that
   ///    tells nobody anything.
   ///
   ///    THE HASH IS THE DRAFT'S. The address is never in the URL: the local part
   ///    is lower-cased, hashed with SHA-1 and encoded in z-base-32, and a client
   ///    that gets it wrong finds nothing. TheDraftsOwnExampleIsTheAddress fetches
   ///    the key of joe.doe@ under iy9q119eutrkn8s1mk4r39qejnbu3n5q - which is the
   ///    worked example in section 3.1 of the draft, computed there and not here.
   ///
   ///    THE 404 IS THE POINT OF THE OTHER HALF. A directory that answered
   ///    differently for an address nobody has and an address whose owner has
   ///    published no key would be a way of asking whether somebody has an account
   ///    here, which is exactly what a spammer wants from it. The answers are
   ///    compared BYTE FOR BYTE - this server's responses carry no Date header, so
   ///    they are deterministic - and every refusal takes the same path: no such
   ///    domain, a domain this server does not host, the server switch off, the
   ///    domain switch off, the account switch off, no key, a hash that is not 32
   ///    characters, and a method other than GET.
   ///
   ///    OVER HTTPS ONLY. A public key fetched over plain HTTP can be replaced on
   ///    the way by anybody on the path, and a replaced key is mail encrypted to
   ///    somebody else. The listener here is the plain one and the requests carry
   ///    X-Forwarded-Proto: https from loopback, which this server trusts as a
   ///    proxy on the same machine; the one test without that header proves the
   ///    refusal.
   /// </summary>
   [TestFixture]
   public class OpenPgpWebKeyDirectory : TestFixtureBase
   {
      private const int WebServicesPort = 9140;

      private Account _account;

      private string Address
      {
         get { return "pgp@" + _domain.Name; }
      }

      [SetUp]
      public void StartWebServices()
      {
         _account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, Address, "Original-Passw0rd!");

         IniFileSetting.Write("WebServicesBindAddress", "127.0.0.1");
         IniFileSetting.Write("WebServicesHttpPort", WebServicesPort.ToString());

         // Reinitialize, not Stop/Start: the web services ports are cached in
         // IniFileSettings, which is only re-read by InitInstance().
         _application.Reinitialize();
      }

      [TearDown]
      public void StopWebServices()
      {
         OpenPgpTestSupport.SetDomainPublishes(_domain, false);
         _domain.Save();
         OpenPgpTestSupport.RestoreDefaults(_settings);

         IniFileSetting.Write("WebServicesHttpPort", "0");
         _application.Reinitialize();
      }

      // ---- what is served ------------------------------------------------------

      [Test]
      [Description("The advanced method serves the key at /.well-known/openpgpkey/<domain>/hu/<hashed local part>, "
                   + "as the binary the draft asks for, with the CORS header it requires")]
      public void TheAdvancedMethodServesTheKey()
      {
         PublishAKey();

         Answer answer = Get("/.well-known/openpgpkey/" + _domain.Name + "/hu/" + OpenPgpTestSupport.HashedPgp + "?l=pgp",
            "openpgpkey." + _domain.Name, true);

         Assert.AreEqual(200, answer.Status, "The key was not served. Raw: " + answer.Raw);
         Assert.AreEqual("application/octet-stream", answer.Header("Content-Type"));
         Assert.AreEqual("*", answer.Header("Access-Control-Allow-Origin"),
            "The draft requires the CORS header on the answer.");

         byte[] expected = Convert.FromBase64String(OpenPgpTestSupport.SuiteKeyBase64);
         Assert.AreEqual(expected.Length, answer.Body.Length, "The key came back a different size. Raw: " + answer.Raw);
         for (int i = 0; i < expected.Length; i++)
            Assert.AreEqual(expected[i], answer.Body[i], "The key came back changed at byte " + i);
      }

      [Test]
      [Description("The answer is the key's bytes, not an armoured block: a client reading a directory answer "
                   + "expects binary")]
      public void TheAnswerIsBinaryAndNotArmoured()
      {
         PublishAKey();

         Answer answer = Get("/.well-known/openpgpkey/" + _domain.Name + "/hu/" + OpenPgpTestSupport.HashedPgp,
            "openpgpkey." + _domain.Name, true);

         Assert.AreEqual(200, answer.Status, "Raw: " + answer.Raw);
         Assert.AreEqual(0x98, answer.Body[0],
            "An OpenPGP public-key packet in the old header format opens with 0x98.");

         string asText = Encoding.ASCII.GetString(answer.Body);
         StringAssert.DoesNotContain("BEGIN PGP", asText, "The directory answer must not be armoured.");
      }

      [Test]
      [Description("The direct method serves the same key on the domain itself, with no domain in the path")]
      public void TheDirectMethodServesTheKey()
      {
         PublishAKey();

         Answer answer = Get("/.well-known/openpgpkey/hu/" + OpenPgpTestSupport.HashedPgp, _domain.Name, true);

         Assert.AreEqual(200, answer.Status, "Raw: " + answer.Raw);
         Assert.AreEqual("application/octet-stream", answer.Header("Content-Type"));
      }

      [Test]
      [Description("The draft's own worked example is the address: Joe.Doe hashes to "
                   + "iy9q119eutrkn8s1mk4r39qejnbu3n5q, and that is where the key of joe.doe@ is found")]
      public void TheDraftsOwnExampleIsTheAddress()
      {
         string address = "joe.doe@" + _domain.Name;
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, address, "Original-Passw0rd!");

         // The key's user id names joe.doe@example.org and not this domain, so it
         // takes the administrator's waiver - which is the case that waiver is for.
         OpenPgpTestSupport.ImportKey(_application, address, OpenPgpTestSupport.OtherKeyBase64, true);

         Account joe = _domain.Accounts.ItemByAddress[address];
         OpenPgpTestSupport.SetAccountPublishes(joe, true);
         joe.Save();

         OpenPgpTestSupport.SetWebKeyDirectoryEnabled(_settings, true);
         OpenPgpTestSupport.SetDomainPublishes(_domain, true);
         _domain.Save();

         Answer answer = Get("/.well-known/openpgpkey/" + _domain.Name + "/hu/" + OpenPgpTestSupport.HashedJoeDoe,
            "openpgpkey." + _domain.Name, true);

         Assert.AreEqual(200, answer.Status,
            "The draft's example hash must be where this server publishes joe.doe@. Raw: " + answer.Raw);
      }

      [Test]
      [Description("The policy file both methods require is served, and is empty on purpose")]
      public void ThePolicyFileIsServedByBothMethods()
      {
         PublishAKey();

         Answer advanced = Get("/.well-known/openpgpkey/" + _domain.Name + "/policy", "openpgpkey." + _domain.Name, true);
         Assert.AreEqual(200, advanced.Status, "Raw: " + advanced.Raw);
         Assert.AreEqual(0, advanced.Body.Length, "The policy file carries no flag this server implements.");

         Answer direct = Get("/.well-known/openpgpkey/policy", _domain.Name, true);
         Assert.AreEqual(200, direct.Status, "Raw: " + direct.Raw);
         Assert.AreEqual(0, direct.Body.Length);
      }

      // ---- what is not served, and what the refusal says ------------------------

      [Test]
      [Description("An address nobody has and an address whose owner has published no key answer identically, "
                   + "byte for byte: the directory cannot be used to find out who has an account here")]
      public void TheRefusalTellsNobodyWhetherTheAccountExists()
      {
         PublishAKey();

         // A real, published account of this domain - with its key removed, so
         // there is nothing to serve for it.
         Assert.IsTrue(OpenPgpTestSupport.RemoveKey(_application, Address, OpenPgpTestSupport.SuiteKeyFingerprint));

         Answer known = Get("/.well-known/openpgpkey/" + _domain.Name + "/hu/" + OpenPgpTestSupport.HashedPgp,
            "openpgpkey." + _domain.Name, true);
         Answer unknown = Get("/.well-known/openpgpkey/" + _domain.Name + "/hu/" + OpenPgpTestSupport.HashedNobody,
            "openpgpkey." + _domain.Name, true);

         Assert.AreEqual(404, known.Status, "Raw: " + known.Raw);
         Assert.AreEqual(404, unknown.Status, "Raw: " + unknown.Raw);
         Assert.AreEqual(unknown.Raw, known.Raw,
            "A 404 for an account that exists must be the same bytes as a 404 for one that does not.");

         // And the CORS header is on the refusal too, so even that cannot be
         // used to tell the two apart.
         Assert.AreEqual("*", known.Header("Access-Control-Allow-Origin"));
      }

      [Test]
      [Description("With the server switch off nothing is served, and the refusal is the same one an unknown "
                   + "address gets")]
      public void NothingIsServedWithTheServerSwitchOff()
      {
         PublishAKey();
         OpenPgpTestSupport.SetWebKeyDirectoryEnabled(_settings, false);

         Answer refused = Get("/.well-known/openpgpkey/" + _domain.Name + "/hu/" + OpenPgpTestSupport.HashedPgp,
            "openpgpkey." + _domain.Name, true);
         Answer unknown = Get("/.well-known/openpgpkey/" + _domain.Name + "/hu/" + OpenPgpTestSupport.HashedNobody,
            "openpgpkey." + _domain.Name, true);

         Assert.AreEqual(404, refused.Status, "Raw: " + refused.Raw);
         Assert.AreEqual(unknown.Raw, refused.Raw);

         // The policy file goes with it: a directory that is switched off has none.
         Answer policy = Get("/.well-known/openpgpkey/" + _domain.Name + "/policy", "openpgpkey." + _domain.Name, true);
         Assert.AreEqual(404, policy.Status, "Raw: " + policy.Raw);
      }

      [Test]
      [Description("With the domain switch off nothing of that domain is served, whatever the account says")]
      public void NothingIsServedWithTheDomainSwitchOff()
      {
         PublishAKey();
         OpenPgpTestSupport.SetDomainPublishes(_domain, false);
         _domain.Save();

         Answer refused = Get("/.well-known/openpgpkey/" + _domain.Name + "/hu/" + OpenPgpTestSupport.HashedPgp,
            "openpgpkey." + _domain.Name, true);

         Assert.AreEqual(404, refused.Status, "Raw: " + refused.Raw);
      }

      [Test]
      [Description("With the account's own switch off its key is not served, though the server and the domain "
                   + "both publish")]
      public void NothingIsServedWithTheAccountSwitchOff()
      {
         PublishAKey();

         Account account = _domain.Accounts.ItemByAddress[Address];
         OpenPgpTestSupport.SetAccountPublishes(account, false);
         account.Save();

         Answer refused = Get("/.well-known/openpgpkey/" + _domain.Name + "/hu/" + OpenPgpTestSupport.HashedPgp,
            "openpgpkey." + _domain.Name, true);

         Assert.AreEqual(404, refused.Status,
            "Holding a key is not the same decision as publishing it. Raw: " + refused.Raw);
      }

      [Test]
      [Description("A request over plain HTTP is refused: a key replaced on the way is mail encrypted to somebody else")]
      public void PlainHttpIsRefused()
      {
         PublishAKey();

         Answer refused = Get("/.well-known/openpgpkey/" + _domain.Name + "/hu/" + OpenPgpTestSupport.HashedPgp,
            "openpgpkey." + _domain.Name, false);

         Assert.AreEqual(404, refused.Status, "Raw: " + refused.Raw);
      }

      [Test]
      [Description("A domain this server does not host, and a hash that is not 32 characters, are the same 404")]
      public void AForeignDomainAndAMalformedHashAreTheSame404()
      {
         PublishAKey();

         Answer unknown = Get("/.well-known/openpgpkey/" + _domain.Name + "/hu/" + OpenPgpTestSupport.HashedNobody,
            "openpgpkey." + _domain.Name, true);

         Answer foreign = Get("/.well-known/openpgpkey/somebody-elses.test/hu/" + OpenPgpTestSupport.HashedPgp,
            "openpgpkey.somebody-elses.test", true);
         Assert.AreEqual(unknown.Raw, foreign.Raw, "A domain this server does not host is the same refusal.");

         Answer short_ = Get("/.well-known/openpgpkey/" + _domain.Name + "/hu/abc", "openpgpkey." + _domain.Name, true);
         Assert.AreEqual(unknown.Raw, short_.Raw, "A hash that is not 32 characters is the same refusal.");
      }

      // ---- the plumbing ---------------------------------------------------------

      /// <summary>
      ///    A key stored for the fixture's account and every switch turned on, which
      ///    is the state each test then takes one thing away from.
      /// </summary>
      private void PublishAKey()
      {
         OpenPgpTestSupport.ImportKey(_application, Address, OpenPgpTestSupport.SuiteKeyBase64, false);

         Account account = _domain.Accounts.ItemByAddress[Address];
         OpenPgpTestSupport.SetAccountPublishes(account, true);
         account.Save();

         OpenPgpTestSupport.SetDomainPublishes(_domain, true);
         _domain.Save();

         OpenPgpTestSupport.SetWebKeyDirectoryEnabled(_settings, true);
      }

      private sealed class Answer
      {
         public int Status;
         public string Raw = "";
         public byte[] Body = new byte[0];
         public readonly System.Collections.Generic.Dictionary<string, string> Headers =
            new System.Collections.Generic.Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);

         public string Header(string name)
         {
            string value;
            return Headers.TryGetValue(name, out value) ? value : "";
         }
      }

      /// <summary>
      ///    One GET against the web services listener. overHttps adds the
      ///    X-Forwarded-Proto a TLS-terminating proxy would, which this server
      ///    honours from loopback.
      ///
      ///    Raw is the whole response with its body as ISO-8859-1, so that the
      ///    identical-refusal assertions compare every byte of two answers rather
      ///    than only their statuses.
      /// </summary>
      private static Answer Get(string path, string host, bool overHttps)
      {
         using (var client = new TcpClient())
         {
            Exception last = null;
            for (int attempt = 0; attempt < 25; attempt++)
            {
               try
               {
                  client.Connect("127.0.0.1", WebServicesPort);
                  last = null;
                  break;
               }
               catch (SocketException ex)
               {
                  last = ex;
                  Thread.Sleep(200);
               }
            }

            if (last != null)
               throw last;

            using (NetworkStream stream = client.GetStream())
            using (var memory = new MemoryStream())
            {
               var head = new StringBuilder();
               head.Append("GET " + path + " HTTP/1.0\r\n");
               head.Append("Host: " + host + "\r\n");
               if (overHttps)
                  head.Append("X-Forwarded-Proto: https\r\n");
               head.Append("Connection: close\r\n\r\n");

               byte[] request = Encoding.ASCII.GetBytes(head.ToString());
               stream.Write(request, 0, request.Length);

               byte[] buffer = new byte[8192];
               int read;
               while ((read = stream.Read(buffer, 0, buffer.Length)) > 0)
                  memory.Write(buffer, 0, read);

               byte[] all = memory.ToArray();

               // Latin-1: every byte is one character and back again, so a binary
               // body survives being compared as text.
               string raw = Encoding.GetEncoding(28591).GetString(all);

               var answer = new Answer { Raw = raw };

               string[] lines = raw.Split(new[] { "\r\n" }, StringSplitOptions.None);
               if (lines.Length > 0)
               {
                  string[] parts = lines[0].Split(' ');
                  if (parts.Length >= 2)
                  {
                     int status;
                     if (int.TryParse(parts[1], out status))
                        answer.Status = status;
                  }
               }

               int separator = raw.IndexOf("\r\n\r\n", StringComparison.Ordinal);
               if (separator >= 0)
               {
                  foreach (string line in raw.Substring(0, separator).Split(new[] { "\r\n" }, StringSplitOptions.None))
                  {
                     int colon = line.IndexOf(':');
                     if (colon > 0)
                        answer.Headers[line.Substring(0, colon).Trim()] = line.Substring(colon + 1).Trim();
                  }

                  int bodyAt = separator + 4;
                  answer.Body = new byte[all.Length - bodyAt];
                  Array.Copy(all, bodyAt, answer.Body, 0, answer.Body.Length);
               }

               return answer;
            }
         }
      }
   }
}
