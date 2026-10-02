// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Reflection;
using System.Runtime.ExceptionServices;
using System.Text;
using System.Text.RegularExpressions;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.API
{
   /// <summary>
   ///    The password breach check over REST: its four settings in the server-wide group,
   ///    the refusal - with the reason word a page translates - on every route that
   ///    chooses a password (the account holder's own change, the account update, the
   ///    account create, the help desk's reset, a chosen app password), the endpoint
   ///    test route, and the OpenAPI document that describes them.
   ///
   ///    The corpus is FakeBreachCorpus on 127.0.0.1:9551 and nothing reaches the
   ///    internet. Every refusal is paired with the same route accepting a password the
   ///    corpus does not hold, and every "nothing changed" is read back rather than
   ///    assumed.
   /// </summary>
   [TestFixture]
   public class RestApiBreachCheck : TestFixtureBase
   {
      // The port the listener answers on: this one on the Windows bench (shared with
      // the other fixtures that start their own listener and stop it after), the
      // suite's own where RestListener finds one already on.
      private static int RestPort = 9104;
      private const string AdminPassword = "testar";
      private const string UserPassword = "Original-Passw0rd-9551";

      private Account _account;

      // Whether SetUp got as far as changing anything. NUnit runs a TearDown whose
      // SetUp threw - Assert.Ignore included - and one that skipped before this was set
      // has nothing to put back, and no listener to put it back through.
      private bool _changed;

      private string Address
      {
         get { return "breachself@" + _domain.Name; }
      }

      [SetUp]
      public void StartRestApi()
      {
         _changed = false;

         if (TestPorts.Host != "127.0.0.1" && !string.Equals(TestPorts.Host, "localhost", StringComparison.OrdinalIgnoreCase))
            Assert.Ignore("The fake breach corpus listens on this machine's loopback address, and the server under test is on " + TestPorts.Host + ".");

         ClassicAssert.IsTrue(string.IsNullOrEmpty(ServerIniFile.GetSetting("HttpProxy")),
            "HttpProxy is set on this bench, and a proxy on this machine would be asked for the fake corpus on loopback. Clear it and run again.");

         _changed = true;

         _settings.SetAdministratorPassword(AdminPassword);

         _account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, Address, UserPassword);

         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();

         (int status, string body) probe = Http("GET", "/api/v1/status", AdminHeader());
         Assert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);

         RestoreDefaults();
      }

      [TearDown]
      public void StopRestApi()
      {
         if (!_changed)
            return;

         // Over COM rather than through the listener, which may be the thing that did
         // not come up; and each restore runs whether or not the one before it threw.
         RunEach(
            RestoreDefaultsOverCom,
            () => ServerIniFile.SetSetting("PasswordPolicyMinimumLength", null),
            RestListener.Stop,
            () => _application.Reinitialize());
      }

      /// <summary>Runs every step, and then throws the first exception any of them threw.</summary>
      private static void RunEach(params Action[] steps)
      {
         Exception first = null;
         foreach (Action step in steps)
         {
            try
            {
               step();
            }
            catch (Exception ex)
            {
               if (first == null)
                  first = ex;
            }
         }

         if (first != null)
            ExceptionDispatchInfo.Capture(first).Throw();
      }

      /// <summary>The four settings at their defaults, by late binding on the COM Settings object.</summary>
      private void RestoreDefaultsOverCom()
      {
         SetOverCom("BreachCheckEnabled", false);
         SetOverCom("BreachCheckUrl", "https://api.pwnedpasswords.com/range/");
         SetOverCom("BreachCheckMinimumCount", 1);
         SetOverCom("BreachCheckUnreachableAction", 0);
      }

      private void SetOverCom(string property, object value)
      {
         object settings = _settings;
         try
         {
            settings.GetType().InvokeMember(property, BindingFlags.SetProperty, null, settings, new[] { value });
         }
         catch (TargetInvocationException ex) when (ex.InnerException != null)
         {
            ExceptionDispatchInfo.Capture(ex.InnerException).Throw();
         }
      }

      private void RestoreDefaults()
      {
         (int status, string body) reset = Http("PUT", "/api/v1/settings", AdminHeader(),
            "{\"breach_check_enabled\":false,\"breach_check_url\":\"https://api.pwnedpasswords.com/range/\",\"breach_check_minimum_count\":1,\"breach_check_unreachable_action\":\"allow\"}");
         Assert.AreEqual(200, reset.status, "The breach check settings could not be put back. Body: " + reset.body);
      }

      private static void Enable(string action = "allow")
      {
         (int status, string body) on = Http("PUT", "/api/v1/settings", AdminHeader(),
            "{\"breach_check_enabled\":true,\"breach_check_url\":\"" + FakeBreachCorpus.Url + "\",\"breach_check_minimum_count\":1,\"breach_check_unreachable_action\":\"" + action + "\"}");
         Assert.AreEqual(200, on.status, "Body: " + on.body);
      }

      private static void AssertReason(string body, string reason)
      {
         StringAssert.Contains("\"reason\":\"" + reason + "\"", body, "The refusal must carry the reason a page translates. Body: " + body);
         StringAssert.Contains("\"error\":\"", body, "...beside the sentence. Body: " + body);
      }

      private static string ErrorLogWhenItSays(string code)
      {
         string log = "";
         RetryHelper.TryAction(TimeSpan.FromSeconds(10), () =>
         {
            log = LogHandler.ReadErrorLog();
            RetryableAssert.StringContains(code, log);
         });
         LogHandler.ClearErrorLogUntilSettled();
         return log;
      }

      // ---- the tests -------------------------------------------------------------

      [Test]
      [Description("GET /api/v1/settings carries the four keys at their defaults, and a PUT of all four reads back, over REST and over COM")]
      public void TheFourSettingsRoundTripOverRest()
      {
         (int status, string body) before = Http("GET", "/api/v1/settings", AdminHeader());
         Assert.AreEqual(200, before.status, before.body);
         StringAssert.Contains("\"breach_check_enabled\":false", before.body);
         StringAssert.Contains("\"breach_check_url\":\"https://api.pwnedpasswords.com/range/\"", before.body);
         StringAssert.Contains("\"breach_check_minimum_count\":1", before.body);
         StringAssert.Contains("\"breach_check_unreachable_action\":\"allow\"", before.body);

         (int status, string body) put = Http("PUT", "/api/v1/settings", AdminHeader(),
            "{\"breach_check_enabled\":true,\"breach_check_url\":\"https://mirror.example/p/{prefix}.txt\",\"breach_check_minimum_count\":5,\"breach_check_unreachable_action\":\"refuse\"}");
         Assert.AreEqual(200, put.status, put.body);

         (int status, string body) after = Http("GET", "/api/v1/settings", AdminHeader());
         StringAssert.Contains("\"breach_check_enabled\":true", after.body);
         StringAssert.Contains("\"breach_check_url\":\"https://mirror.example/p/{prefix}.txt\"", after.body);
         StringAssert.Contains("\"breach_check_minimum_count\":5", after.body);
         StringAssert.Contains("\"breach_check_unreachable_action\":\"refuse\"", after.body);

         // The same store the Control Panel reads.
         object settings = _settings;
         ClassicAssert.AreEqual(5, Convert.ToInt32(settings.GetType().InvokeMember("BreachCheckMinimumCount", BindingFlags.GetProperty, null, settings, null)));
         ClassicAssert.AreEqual(1, Convert.ToInt32(settings.GetType().InvokeMember("BreachCheckUnreachableAction", BindingFlags.GetProperty, null, settings, null)));
      }

      [Test]
      [Description("An endpoint the check would not use is a 400 naming why, and nothing else in the same PUT is applied")]
      public void AnInvalidUrlIsRefusedAndNothingInThePutIsApplied()
      {
         (int status, string body) refused = Http("PUT", "/api/v1/settings", AdminHeader(),
            "{\"breach_check_minimum_count\":7,\"breach_check_url\":\"http://example.com/range/\"}");
         Assert.AreEqual(400, refused.status, refused.body);
         StringAssert.Contains("Plain http is accepted only to this machine", refused.body);

         (int status, string body) zero = Http("PUT", "/api/v1/settings", AdminHeader(), "{\"breach_check_minimum_count\":0}");
         Assert.AreEqual(400, zero.status, zero.body);

         (int status, string body) unknownWord = Http("PUT", "/api/v1/settings", AdminHeader(), "{\"breach_check_unreachable_action\":\"ignore\"}");
         Assert.AreEqual(400, unknownWord.status, unknownWord.body);

         (int status, string body) after = Http("GET", "/api/v1/settings", AdminHeader());
         StringAssert.Contains("\"breach_check_minimum_count\":1", after.body, "The minimum in the refused PUT must not have been applied.");
         StringAssert.Contains("\"breach_check_url\":\"https://api.pwnedpasswords.com/range/\"", after.body);
         StringAssert.Contains("\"breach_check_unreachable_action\":\"allow\"", after.body);
      }

      [Test]
      [Description("POST /api/v1/me/password refuses a password in the corpus with 400 and reason breached; the old password still signs in")]
      public void SelfServiceRefusesABreachedPasswordWithAReason()
      {
         using (var corpus = new FakeBreachCorpus())
         {
            corpus.Add("password", 10434004);
            Enable();

            (int status, string body) refused = Http("POST", "/api/v1/me/password", UserHeader(UserPassword),
               "{\"current\":\"" + UserPassword + "\",\"new\":\"password\"}");
            Assert.AreEqual(400, refused.status, refused.body);
            AssertReason(refused.body, "breached");

            Assert.AreEqual(200, Http("GET", "/api/v1/me", UserHeader(UserPassword)).status, "The password must be unchanged.");

            // Negative control: the same route with a password the corpus does not hold.
            const string fresh = "A-Password-Nobody-Leaked-9551";
            (int status, string body) changed = Http("POST", "/api/v1/me/password", UserHeader(UserPassword),
               "{\"current\":\"" + UserPassword + "\",\"new\":\"" + fresh + "\"}");
            Assert.AreEqual(200, changed.status, changed.body);
            Assert.AreEqual(200, Http("GET", "/api/v1/me", UserHeader(fresh)).status, "The new password must sign in.");
            Assert.AreEqual(2, corpus.Requests.Count);
         }
      }

      [Test]
      [Description("With the action set to refuse and nothing listening, POST /api/v1/me/password is 503 with reason breach_check_unavailable")]
      public void SelfServiceUnavailableRefusalIs503()
      {
         Enable("refuse");

         (int status, string body) refused = Http("POST", "/api/v1/me/password", UserHeader(UserPassword),
            "{\"current\":\"" + UserPassword + "\",\"new\":\"Unreachable-Refused-9551\"}");
         Assert.AreEqual(503, refused.status, refused.body);
         AssertReason(refused.body, "breach_check_unavailable");

         ErrorLogWhenItSays("HM6601");

         Assert.AreEqual(200, Http("GET", "/api/v1/me", UserHeader(UserPassword)).status, "The password must be unchanged.");
      }

      [Test]
      [Description("PUT /api/v1/accounts/{address} refuses a password in the corpus with the reason, and takes one it does not hold")]
      public void AccountUpdateRefusesABreachedPassword()
      {
         using (var corpus = new FakeBreachCorpus())
         {
            corpus.Add("password", 10434004);
            Enable();

            (int status, string body) refused = Http("PUT", "/api/v1/accounts/" + Address, AdminHeader(), "{\"password\":\"password\"}");
            Assert.AreEqual(400, refused.status, refused.body);
            AssertReason(refused.body, "breached");

            var still = new ImapClientSimulator();
            ClassicAssert.IsTrue(still.ConnectAndLogon(Address, UserPassword), "The refused update must leave the password as it was.");
            still.Disconnect();

            const string fresh = "Updated-Nobody-Leaked-9551";
            (int status, string body) updated = Http("PUT", "/api/v1/accounts/" + Address, AdminHeader(), "{\"password\":\"" + fresh + "\"}");
            Assert.AreEqual(200, updated.status, updated.body);

            var imap = new ImapClientSimulator();
            ClassicAssert.IsTrue(imap.ConnectAndLogon(Address, fresh), "The accepted password must be in force.");
            imap.Disconnect();
         }
      }

      [Test]
      [Description("POST /api/v1/accounts/{address}/password, the help desk's reset, judges a password outside ASCII as the characters sent: the breach check finds a leaked one and the policy counts its characters, not its bytes")]
      public void AccountPasswordResetJudgesAPasswordOutsideAsciiAsItsCharacters()
      {
         string route = "/api/v1/accounts/" + Address + "/password";

         // 18 characters, 22 bytes of UTF-8. The route read the body one byte to a
         // character (String(AnsiString)), which made it 22 characters: the breach
         // check asked about the SHA-1 of their UTF-8 (prefix AB4DF), which the
         // corpus does not hold, and a minimum of 20 characters was met. The stored
         // hash was the same either way (Unicode::ToUtf8Canonical), so these two
         // are where the difference is seen.
         const string accented = "p\u00e4ssw\u00f6rd-2026-cr\u00e9\u00e9";
         const string fresh = "Nobody-Leaked-fr\u00fch-9551";

         using (var corpus = new FakeBreachCorpus())
         {
            corpus.Add(accented, 3);
            Enable();

            (int status, string body) refused = Http("POST", route, AdminHeader(), "{\"password\":\"" + accented + "\"}");
            Assert.AreEqual(400, refused.status, refused.body);
            AssertReason(refused.body, "breached");
            Assert.AreEqual(1, corpus.Requests.Count);
            StringAssert.StartsWith("GET /range/" + FakeBreachCorpus.Sha1HexUtf8(accented).Substring(0, 5) + " ", corpus.Requests[0],
               "The check must ask about the SHA-1 of the characters' UTF-8.");
            Assert.AreEqual(200, Http("GET", "/api/v1/me", UserHeader(UserPassword)).status, "The refused reset must leave the password as it was.");

            // Negative control: the same route, and a password outside ASCII the corpus does not hold.
            (int status, string body) accepted = Http("POST", route, AdminHeader(), "{\"password\":\"" + fresh + "\"}");
            Assert.AreEqual(200, accepted.status, accepted.body);
            Assert.AreEqual(2, corpus.Requests.Count);
            Assert.AreEqual(200, Http("GET", "/api/v1/me", UserHeader(fresh)).status, "The reset password must sign in.");
         }

         RestoreDefaults();

         ServerIniFile.SetSetting("PasswordPolicyMinimumLength", "20");
         _application.Reinitialize();

         (int status, string body) tooShort = Http("POST", route, AdminHeader(), "{\"password\":\"" + accented + "\"}");
         Assert.AreEqual(400, tooShort.status, tooShort.body);
         StringAssert.Contains("20", tooShort.body, "Eighteen characters are fewer than twenty, whatever their UTF-8 counts.");
         Assert.AreEqual(200, Http("GET", "/api/v1/me", UserHeader(fresh)).status, "The refused reset must leave the password as it was.");

         ServerIniFile.SetSetting("PasswordPolicyMinimumLength", null);
         _application.Reinitialize();

         // Neither rule on: the reset takes it, and it signs in.
         (int status, string body) reset = Http("POST", route, AdminHeader(), "{\"password\":\"" + accented + "\"}");
         Assert.AreEqual(200, reset.status, reset.body);
         Assert.AreEqual(200, Http("GET", "/api/v1/me", UserHeader(accented)).status, "The reset password must sign in.");
      }

      [Test]
      [Description("POST /api/v1/domains/{domain}/accounts applies the breach check and the password policy, and stores a non-ASCII password as the characters sent")]
      public void AccountCreateAppliesThePolicyAndTheBreachCheck()
      {
         string domainPath = "/api/v1/domains/" + _domain.Name + "/accounts";

         // A password outside ASCII: 18 characters, 22 bytes of UTF-8.
         const string accented = "p\u00e4ssw\u00f6rd-2026-cr\u00e9\u00e9";
         string accentedAddress = "breachutf8@" + _domain.Name;

         using (var corpus = new FakeBreachCorpus())
         {
            corpus.Add("password", 10434004);
            corpus.Add(accented, 3);
            Enable();

            string leakedAddress = "breachcreate@" + _domain.Name;
            (int status, string body) refused = Http("POST", domainPath, AdminHeader(),
               "{\"address\":\"" + leakedAddress + "\",\"password\":\"password\"}");
            Assert.AreEqual(400, refused.status, refused.body);
            AssertReason(refused.body, "breached");
            Assert.AreEqual(404, Http("GET", "/api/v1/accounts/" + leakedAddress, AdminHeader()).status, "A refused create must create nothing.");

            (int status, string body) accepted = Http("POST", domainPath, AdminHeader(),
               "{\"address\":\"" + leakedAddress + "\",\"password\":\"Created-Nobody-Leaked-9551\"}");
            Assert.AreEqual(201, accepted.status, accepted.body);

            // The route hands the check the characters sent. The corpus holds the SHA-1
            // of their UTF-8; a route that read the body one byte to a character, as
            // String(AnsiString) did, would have asked about the UTF-8 of those 22
            // characters, found nothing, and made the account. The stored hash cannot
            // show that difference (see the end of this test), so this is where it is seen.
            (int status, string body) accentedRefused = Http("POST", domainPath, AdminHeader(),
               "{\"address\":\"" + accentedAddress + "\",\"password\":\"" + accented + "\"}");
            Assert.AreEqual(400, accentedRefused.status, accentedRefused.body);
            AssertReason(accentedRefused.body, "breached");
            Assert.AreEqual(404, Http("GET", "/api/v1/accounts/" + accentedAddress, AdminHeader()).status, "A refused create must create nothing.");
         }

         RestoreDefaults();

         // The policy, which this route never applied before.
         ServerIniFile.SetSetting("PasswordPolicyMinimumLength", "16");
         _application.Reinitialize();

         string shortAddress = "breachshort@" + _domain.Name;
         (int status, string body) tooShort = Http("POST", domainPath, AdminHeader(),
            "{\"address\":\"" + shortAddress + "\",\"password\":\"Short-1!\"}");
         Assert.AreEqual(400, tooShort.status, tooShort.body);
         StringAssert.Contains("16", tooShort.body, "The refusal names the requirement.");
         Assert.AreEqual(404, Http("GET", "/api/v1/accounts/" + shortAddress, AdminHeader()).status, "A refused create must create nothing.");

         ServerIniFile.SetSetting("PasswordPolicyMinimumLength", null);
         _application.Reinitialize();

         // The password outside ASCII, with neither rule on: stored as UTF-8, so the
         // same characters validate. Last, because Account.ValidatePassword has no REST
         // route: against the Linux suite's shim it skips the test here, after
         // everything above has run.
         (int status, string body) created = Http("POST", domainPath, AdminHeader(),
            "{\"address\":\"" + accentedAddress + "\",\"password\":\"" + accented + "\"}");
         Assert.AreEqual(201, created.status, created.body);
         Account createdAccount = _domain.Accounts.get_ItemByAddress(accentedAddress);
         ClassicAssert.IsTrue(createdAccount.ValidatePassword(accented), "The account must validate the exact characters it was created with.");
         ClassicAssert.IsFalse(createdAccount.ValidatePassword("p\u00e4ssw\u00f6rd-2026-cr\u00e9\u00e8"),
            "Negative control: a password one accented letter different must not validate.");

         // Not a negative control: the password's UTF-8 read one byte to a character -
         // the Latin-1 reading of those bytes, and for these letters the Windows-1252
         // one too. Since 6.3.4 every password is hashed over Unicode::ToUtf8Canonical,
         // which takes a String holding UTF-8 widened byte by byte - as IMAP LOGIN,
         // POP3 PASS and a REST Basic credential hand a password over - as those bytes,
         // so that such a sign-in opens an account whose password was set as text
         // (Crypt.cpp, docs/SecretsAtRest.md; ClassTester requires the same). The form
         // is the same password by design, and so a route that stored it would store
         // the same hash: the breach check above is what tells the two routes apart.
         ClassicAssert.IsTrue(createdAccount.ValidatePassword("p\u00c3\u00a4ssw\u00c3\u00b6rd-2026-cr\u00c3\u00a9\u00c3\u00a9"),
            "The password's UTF-8 widened one byte to a character is the same password (Unicode::ToUtf8Canonical).");
      }

      [Test]
      [Description("A chosen app password over the administrator's route is checked; a generated one asks nothing")]
      public void AdministratorAppPasswordChosenIsChecked()
      {
         using (var corpus = new FakeBreachCorpus())
         {
            corpus.Add("correct horse battery staple", 1200);
            Enable();

            string route = "/api/v1/accounts/" + Address + "/app-passwords";

            (int status, string body) refused = Http("POST", route, AdminHeader(), "{\"name\":\"Chosen\",\"password\":\"correct horse battery staple\"}");
            Assert.AreEqual(400, refused.status, refused.body);
            AssertReason(refused.body, "breached");
            Assert.AreEqual(1, corpus.Requests.Count);

            (int status, string body) generated = Http("POST", route, AdminHeader(), "{\"name\":\"Generated\"}");
            Assert.AreEqual(201, generated.status, generated.body);
            Assert.AreEqual(1, corpus.Requests.Count, "A generated app password must not be asked about.");

            (int status, string body) chosen = Http("POST", route, AdminHeader(), "{\"name\":\"Chosen\",\"password\":\"an app password nobody has 9551\"}");
            Assert.AreEqual(201, chosen.status, chosen.body);
            Assert.AreEqual(2, corpus.Requests.Count);
         }
      }

      [Test]
      [Description("POST /api/v1/settings/breach-check/test reports what the endpoint said, and never its body; an unusable URL is a 400; a domain-restricted key is refused")]
      public void TheTestRouteReportsWhatTheEndpointSaid()
      {
         using (var corpus = new FakeBreachCorpus())
         {
            corpus.AddHash("00000" + "ABCDEF0123456789ABCDEF0123456789ABC", 2);

            (int status, string body) reached = Http("POST", "/api/v1/settings/breach-check/test", AdminHeader(),
               "{\"url\":\"" + FakeBreachCorpus.Url + "\"}");
            Assert.AreEqual(200, reached.status, reached.body);
            StringAssert.Contains("\"reachable\":true", reached.body);
            StringAssert.Contains("\"status\":200", reached.body);
            StringAssert.Contains("\"entries\":4", reached.body);
            StringAssert.Contains("\"padded\":true", reached.body);
            StringAssert.IsMatch("\"milliseconds\":[0-9]+", reached.body);
            StringAssert.DoesNotContain("ABCDEF0123456789", reached.body, "The endpoint's answer must never be returned.");

            ClassicAssert.AreEqual(1, corpus.Requests.Count);
            StringAssert.StartsWith("GET /range/00000 HTTP/1.0\r\n", corpus.Requests[0]);
            StringAssert.Contains("\r\nAdd-Padding: true\r\n", corpus.Requests[0]);

            // No body: the configured URL, which is the fake once it is set.
            Enable();
            (int status, string body) configured = Http("POST", "/api/v1/settings/breach-check/test", AdminHeader(), null);
            Assert.AreEqual(200, configured.status, configured.body);
            StringAssert.Contains("\"reachable\":true", configured.body);
            ClassicAssert.AreEqual(2, corpus.Requests.Count);
         }

         (int status, string body) unreachable = Http("POST", "/api/v1/settings/breach-check/test", AdminHeader(),
            "{\"url\":\"" + FakeBreachCorpus.Url + "\"}");
         Assert.AreEqual(200, unreachable.status, unreachable.body);
         StringAssert.Contains("\"reachable\":false", unreachable.body);
         StringAssert.Contains("\"error\":\"", unreachable.body);

         (int status, string body) invalid = Http("POST", "/api/v1/settings/breach-check/test", AdminHeader(), "{\"url\":\"http://example.com/range/\"}");
         Assert.AreEqual(400, invalid.status, invalid.body);

         // A key restricted to a domain may not make the server fetch anything.
         (int status, string body) key = Http("POST", "/api/v1/apikeys", AdminHeader(),
            "{\"label\":\"regression - breach check test\",\"scope\":\"full\",\"domains\":\"" + _domain.Name + "\"}");
         Assert.AreEqual(201, key.status, key.body);
         Match token = Regex.Match(key.body, "\"key\":\"([^\"]+)\"");
         Assert.IsTrue(token.Success, key.body);
         Match id = Regex.Match(key.body, "\"id\":\"?([^\",}]+)");
         try
         {
            (int status, string body) scoped = Http("POST", "/api/v1/settings/breach-check/test", "Bearer " + token.Groups[1].Value, "{\"url\":\"" + FakeBreachCorpus.Url + "\"}");
            Assert.AreEqual(403, scoped.status, scoped.body);
         }
         finally
         {
            if (id.Success)
               Http("DELETE", "/api/v1/apikeys/" + id.Groups[1].Value, AdminHeader());
         }
      }

      [Test]
      [Description("A test asked by any credential but the administrator password reaches a public address only: a full-scope API key's test of an endpoint on this machine - named in the body, or configured - is answered reachable false and the endpoint is never asked, while the administrator password's test of the same endpoint is answered as before. Until 6.3.6 the key's test reached the server's own network and reported what answered there and how fast.")]
      public void AKeysTestReachesPublicAddressesOnly()
      {
         (int status, string body) key = Http("POST", "/api/v1/apikeys", AdminHeader(),
            "{\"label\":\"regression - breach check test, public\",\"scope\":\"full\"}");
         Assert.AreEqual(201, key.status, key.body);
         Match token = Regex.Match(key.body, "\"key\":\"([^\"]+)\"");
         Assert.IsTrue(token.Success, key.body);
         Match id = Regex.Match(key.body, "\"id\":\"?([^\",}]+)");

         try
         {
            using (var corpus = new FakeBreachCorpus())
            {
               corpus.AddHash("00000" + "ABCDEF0123456789ABCDEF0123456789ABC", 2);

               (int status, string body) named = Http("POST", "/api/v1/settings/breach-check/test", "Bearer " + token.Groups[1].Value,
                  "{\"url\":\"" + FakeBreachCorpus.Url + "\"}");
               Assert.AreEqual(200, named.status, named.body);
               StringAssert.Contains("\"reachable\":false", named.body, "A key's test of this machine is not made.");
               StringAssert.Contains("\"status\":0", named.body);
               ClassicAssert.AreEqual(0, corpus.Requests.Count, "The endpoint on this machine was never asked for a key.");

               // The configured endpoint is held to the same rule: the caller is what decides.
               Enable();
               (int status, string body) configured = Http("POST", "/api/v1/settings/breach-check/test", "Bearer " + token.Groups[1].Value, null);
               Assert.AreEqual(200, configured.status, configured.body);
               StringAssert.Contains("\"reachable\":false", configured.body);
               ClassicAssert.AreEqual(0, corpus.Requests.Count, "The configured endpoint on this machine was never asked for a key either.");

               // The administrator password tests it as it always did.
               (int status, string body) byAdministrator = Http("POST", "/api/v1/settings/breach-check/test", AdminHeader(), null);
               Assert.AreEqual(200, byAdministrator.status, byAdministrator.body);
               StringAssert.Contains("\"reachable\":true", byAdministrator.body);
               ClassicAssert.AreEqual(1, corpus.Requests.Count);
            }
         }
         finally
         {
            if (id.Success)
               Http("DELETE", "/api/v1/apikeys/" + id.Groups[1].Value, AdminHeader());
         }
      }

      [Test]
      [Description("The OpenAPI document names the four keys, both reason words and the test route")]
      public void TheOpenApiDocumentNamesTheKeysTheReasonsAndTheRoute()
      {
         (int status, string body) document = Http("GET", "/api/v1/openapi.json", AdminHeader());
         Assert.AreEqual(200, document.status);

         foreach (string needle in new[]
                  {
                     "breach_check_enabled", "breach_check_url", "breach_check_minimum_count", "breach_check_unreachable_action",
                     "reason: breached", "reason: breach_check_unavailable", "\"/api/v1/settings/breach-check/test\""
                  })
            StringAssert.Contains(needle, document.body, "The OpenAPI document must describe " + needle);
      }

      // ---- HTTP ------------------------------------------------------------------

      private static string AdminHeader()
      {
         return "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes("Administrator:" + AdminPassword));
      }

      private string UserHeader(string password)
      {
         return "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(Address + ":" + password));
      }

      private static (int status, string body) Http(string method, string path, string authorization, string requestBody = null)
      {
         return RestRequest.Send("127.0.0.1", RestPort, method, path, authorization, requestBody);
      }
   }
}
