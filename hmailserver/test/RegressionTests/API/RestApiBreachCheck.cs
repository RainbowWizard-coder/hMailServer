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
   ///    account create, a chosen app password), the endpoint test route, and the
   ///    OpenAPI document that describes them.
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
      [Description("POST /api/v1/domains/{domain}/accounts applies the breach check and the password policy, and stores a non-ASCII password as the characters sent")]
      public void AccountCreateAppliesThePolicyAndTheBreachCheck()
      {
         string domainPath = "/api/v1/domains/" + _domain.Name + "/accounts";

         using (var corpus = new FakeBreachCorpus())
         {
            corpus.Add("password", 10434004);
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

         // A password outside ASCII, with neither rule on: stored as UTF-8, so the same
         // characters validate. Read through String(AnsiString), as it was, it did not.
         // Last, because Account.ValidatePassword has no REST route: against the Linux
         // suite's shim it skips the test here, after everything above has run.
         const string accented = "p\u00e4ssw\u00f6rd-2026-cr\u00e9\u00e9";
         string accentedAddress = "breachutf8@" + _domain.Name;
         (int status, string body) created = Http("POST", domainPath, AdminHeader(),
            "{\"address\":\"" + accentedAddress + "\",\"password\":\"" + accented + "\"}");
         Assert.AreEqual(201, created.status, created.body);
         Account createdAccount = _domain.Accounts.get_ItemByAddress(accentedAddress);
         ClassicAssert.IsTrue(createdAccount.ValidatePassword(accented), "The account must validate the exact characters it was created with.");
         ClassicAssert.IsFalse(createdAccount.ValidatePassword("p\u00c3\u00a4ssw\u00c3\u00b6rd-2026-cr\u00c3\u00a9\u00c3\u00a9"),
            "Negative control: the UTF-8 bytes read as Windows-1252 are a different password.");
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
