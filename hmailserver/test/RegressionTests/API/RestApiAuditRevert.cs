// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Text;
using System.Text.Json;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;
using hMailServer;

namespace RegressionTests.API
{
   /// <summary>
   ///    A setting put back from its audit row: POST /api/v1/audit/{id}/revert and
   ///    Utilities.RevertAuditedChange, which write the value the row says the setting
   ///    had through the same setter the change came through, so the put-back is one
   ///    more row in the trail made by whoever asked - and which refuse, with the
   ///    reason, a row that is not a setting's, a secret's, or one whose value the
   ///    trail cut. The trail is also read and verified over COM here, for the Control
   ///    Panel's page, which reads what the Deck reads.
   /// </summary>
   [TestFixture]
   public class RestApiAuditRevert : TestFixtureBase
   {
      private static int RestPort = 9596;
      private const string AdminPassword = "testar";
      private const string AuditPath = "/api/v1/audit";

      private string _hostName;

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(AdminPassword);
         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();

         (int status, string body) probe = Http("GET", "/api/v1/status");
         ClassicAssert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);

         _hostName = Setting("host_name");
      }

      [TearDown]
      public void StopRestApi()
      {
         try
         {
            if (_hostName != null && Setting("host_name") != _hostName)
               Http("PUT", "/api/v1/settings", "{\"host_name\":" + JsonSerializer.Serialize(_hostName) + "}");
         }
         finally
         {
            RestListener.Stop();
         }
      }

      [Test]
      [Description("A setting changed over REST is put back from its audit row: the value it had returns, the put-back is itself recorded as an update by whoever asked, and the answer names the setting and the value.")]
      public void ASettingIsPutBackFromItsRow()
      {
         const string changed = "audit-revert.example";
         string before = _hostName;
         ClassicAssert.AreNotEqual(changed, before);

         ClassicAssert.AreEqual(200, Http("PUT", "/api/v1/settings", "{\"host_name\":\"" + changed + "\"}").status);
         ClassicAssert.AreEqual(changed, Setting("host_name"));

         (long id, string setting) row = NewestSettingRowWith(changed);
         ClassicAssert.Greater(row.id, 0, "The change was recorded.");

         (int status, string body) reverted = Http("POST", AuditPath + "/" + row.id + "/revert", "{}");
         ClassicAssert.AreEqual(200, reverted.status, reverted.body);
         using (JsonDocument document = JsonDocument.Parse(reverted.body))
         {
            ClassicAssert.IsTrue(document.RootElement.GetProperty("reverted").GetBoolean());
            ClassicAssert.AreEqual(row.id, document.RootElement.GetProperty("id").GetInt64());
            ClassicAssert.AreEqual(row.setting, document.RootElement.GetProperty("setting").GetString());
            ClassicAssert.AreEqual(before, document.RootElement.GetProperty("value").GetString());
         }

         ClassicAssert.AreEqual(before, Setting("host_name"), "The value it had is back, and the running server reads it.");

         // The put-back is a change of its own, newest in the trail, from the
         // changed value back to the old one, over REST.
         (int status, string body) trail = Http("GET", AuditPath + "?object_type=setting&limit=5");
         string newest = FirstEntry(trail.body);
         StringAssert.Contains("\"object\":\"" + row.setting + "\"", newest);
         StringAssert.Contains(changed + " -> " + before, newest);
         StringAssert.Contains("\"interface\":\"REST\"", newest);
         StringAssert.Contains("\"action\":\"updated\"", newest);
      }

      [Test]
      [Description("A row that is not a setting's change, an unknown row, and a body-less request: refused with the reason, 404 for the row that is not there, and nothing changed.")]
      public void RowsThatCannotBePutBackAreRefused()
      {
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "audit-revert@example.test", "test");

         (int status, string body) trail = Http("GET", AuditPath + "?object_type=account&limit=5");
         long accountRow = 0;
         using (JsonDocument document = JsonDocument.Parse(trail.body))
         {
            foreach (JsonElement entry in document.RootElement.GetProperty("entries").EnumerateArray())
            {
               if (entry.GetProperty("object").GetString() == account.Address)
               {
                  accountRow = entry.GetProperty("id").GetInt64();
                  break;
               }
            }
         }
         ClassicAssert.Greater(accountRow, 0, "The account's creation was recorded: " + trail.body);

         (int status, string body) refused = Http("POST", AuditPath + "/" + accountRow + "/revert", "{}");
         ClassicAssert.AreEqual(409, refused.status, refused.body);
         StringAssert.Contains("Only a setting's change can be put back", refused.body);

         ClassicAssert.AreEqual(404, Http("POST", AuditPath + "/999999999/revert", "{}").status);
         ClassicAssert.IsNotNull(_domain.Accounts.get_ItemByAddress(account.Address), "Nothing was changed by a refusal.");
      }

      [Test]
      [Description("Over COM: the trail is read as the Deck reads it, the chain verifies intact, and a setting's row is put back with the value answered.")]
      public void TheTrailIsReadVerifiedAndPutBackOverCom()
      {
         const string changed = "com-revert.example";
         string before = _hostName;

         ClassicAssert.AreEqual(200, Http("PUT", "/api/v1/settings", "{\"host_name\":\"" + changed + "\"}").status);
         (long id, string setting) row = NewestSettingRowWith(changed);

         string listing = _application.Utilities.GetAuditTrail("", 20, 0);
         using (JsonDocument document = JsonDocument.Parse(listing))
         {
            ClassicAssert.Greater(document.RootElement.GetProperty("total").GetInt32(), 0);
            ClassicAssert.AreEqual(0, document.RootElement.GetProperty("offset").GetInt32());
            JsonElement first = document.RootElement.GetProperty("entries")[0];
            ClassicAssert.AreEqual(row.id, first.GetProperty("id").GetInt64(), "Newest first, as the Deck reads it.");
            ClassicAssert.AreEqual(row.setting, first.GetProperty("object").GetString());
            StringAssert.Contains(changed, first.GetProperty("detail").GetString());
         }

         using (JsonDocument document = JsonDocument.Parse(_application.Utilities.VerifyAuditTrail()))
         {
            ClassicAssert.IsTrue(document.RootElement.GetProperty("intact").GetBoolean(), document.RootElement.ToString());
            ClassicAssert.Greater(document.RootElement.GetProperty("rows_checked").GetInt64(), 0);
         }

         string restored = _application.Utilities.RevertAuditedChange((int) row.id);
         ClassicAssert.AreEqual(before, restored);
         ClassicAssert.AreEqual(before, Setting("host_name"));

         var refused = Assert.Throws<System.Runtime.InteropServices.COMException>(() => _application.Utilities.RevertAuditedChange(999999999));
         StringAssert.Contains("No audit row", refused.Message);
      }

      [Test]
      [Description("A switch changed over REST is put back into the flag the server reads: COM and REST read the value it had, a message delivered afterwards shows it, and the put-back is recorded as the switch's own change. Until 29 September 2026 the put-back wrote \"true\" into the setting's text column, which nothing reads, answered that the switch was back, and left it off.")]
      public void ASwitchIsPutBackIntoTheFlagTheServerReads()
      {
         const string property = "adddeliveredtoheader";
         bool original = _settings.AddDeliveredToHeader;

         try
         {
            // On, then switched off over REST: the row says "true -> false".
            _settings.AddDeliveredToHeader = true;
            ClassicAssert.AreEqual(200, Http("PUT", "/api/v1/settings", "{\"add_delivered_to_header\":false}").status);
            ClassicAssert.IsFalse(_settings.AddDeliveredToHeader);

            long row = NewestRowFor(property, "true -> false");

            (int status, string body) reverted = Http("POST", AuditPath + "/" + row + "/revert", "{}");
            ClassicAssert.AreEqual(200, reverted.status, reverted.body);
            using (JsonDocument document = JsonDocument.Parse(reverted.body))
               ClassicAssert.AreEqual("true", document.RootElement.GetProperty("value").GetString());

            ClassicAssert.IsTrue(_settings.AddDeliveredToHeader, "COM reads the switch as it was before the change: on.");
            ClassicAssert.IsTrue(SettingFlag("add_delivered_to_header"), "REST reads the switch as it was before the change: on.");

            // The put-back is the switch's own change, from off to on.
            string newest = FirstEntry(Http("GET", AuditPath + "?object_type=setting&limit=5").body);
            StringAssert.Contains("\"object\":\"" + property + "\"", newest);
            StringAssert.Contains("false -> true", newest);

            // And the server does what the switch says: local delivery adds the field.
            Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "audit-revert-switch@example.test", "test");
            SmtpClientSimulator.StaticSend("sender@example.test", account.Address, "Put back", "The switch is on again.");
            string message = Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");
            StringAssert.Contains("Delivered-To: " + account.Address, message);
         }
         finally
         {
            _settings.AddDeliveredToHeader = original;
         }
      }

      [Test]
      [Description("A number is put back as a number, over COM; a text whose values hold \" -> \" is put back whole, split where the new side is the setting's value now; and a row that cannot be split so is refused with nothing changed. Until 29 September 2026 the first arrow was taken, so \"before -> after\" changed to \"plain\" was put back as \"before\".")]
      public void ANumberAndATextHoldingAnArrowArePutBackWhole()
      {
         int originalPort = _settings.SMTPRelayerPort;
         string originalWelcome = _settings.WelcomeIMAP;

         try
         {
            // A number, changed over REST and put back over COM.
            _settings.SMTPRelayerPort = 2525;
            ClassicAssert.AreEqual(200, Http("PUT", "/api/v1/settings", "{\"smtp_relayer_port\":2626}").status);
            ClassicAssert.AreEqual(2626, _settings.SMTPRelayerPort);

            long portRow = NewestRowFor("smtprelayerport", "2525 -> 2626");
            ClassicAssert.AreEqual("2525", _application.Utilities.RevertAuditedChange((int) portRow));
            ClassicAssert.AreEqual(2525, _settings.SMTPRelayerPort);
            StringAssert.Contains("\"smtp_relayer_port\":2525,", Http("GET", "/api/v1/settings").body);

            // A text holding the arrow: the row reads "before -> after -> plain".
            const string arrowed = "before -> after";
            _settings.WelcomeIMAP = arrowed;
            ClassicAssert.AreEqual(200, Http("PUT", "/api/v1/settings", "{\"welcome_imap\":\"plain\"}").status);
            ClassicAssert.AreEqual("plain", _settings.WelcomeIMAP);

            long textRow = NewestRowFor("welcomeimap", arrowed + " -> plain");

            (int status, string body) reverted = Http("POST", AuditPath + "/" + textRow + "/revert", "{}");
            ClassicAssert.AreEqual(200, reverted.status, reverted.body);
            using (JsonDocument document = JsonDocument.Parse(reverted.body))
               ClassicAssert.AreEqual(arrowed, document.RootElement.GetProperty("value").GetString());
            ClassicAssert.AreEqual(arrowed, _settings.WelcomeIMAP, "The whole value it had is back.");

            // The same row again: the setting now holds neither of its possible new
            // sides, so the row does not say where the old value ends.
            (int status, string body) refused = Http("POST", AuditPath + "/" + textRow + "/revert", "{}");
            ClassicAssert.AreEqual(409, refused.status, refused.body);
            StringAssert.Contains("does not say where", refused.body);
            ClassicAssert.AreEqual(arrowed, _settings.WelcomeIMAP, "Nothing was changed by the refusal.");
         }
         finally
         {
            _settings.SMTPRelayerPort = originalPort;
            _settings.WelcomeIMAP = originalWelcome;
         }
      }

      [Test]
      [Description("A row whose values hold \" -> \" is not divided by the value the setting has now once the setting has changed again: \"first\" changed to \"b -> c\" and then to \"c\" leaves the row \"first -> b -> c\", whose second arrow leaves \"c\" as the new side, and the row is refused rather than \"first -> b\" written back - a value the setting never had.")]
      public void AnArrowedRowIsRefusedOnceTheSettingHasChangedSince()
      {
         string originalWelcome = _settings.WelcomeIMAP;

         try
         {
            _settings.WelcomeIMAP = "first";
            ClassicAssert.AreEqual(200, Http("PUT", "/api/v1/settings", "{\"welcome_imap\":\"b -> c\"}").status);
            long firstRow = NewestRowFor("welcomeimap", "first -> b -> c");

            ClassicAssert.AreEqual(200, Http("PUT", "/api/v1/settings", "{\"welcome_imap\":\"c\"}").status);
            ClassicAssert.AreEqual("c", _settings.WelcomeIMAP);

            (int status, string body) refused = Http("POST", AuditPath + "/" + firstRow + "/revert", "{}");
            ClassicAssert.AreEqual(409, refused.status, refused.body);
            StringAssert.Contains("changed since", refused.body);
            ClassicAssert.AreEqual("c", _settings.WelcomeIMAP, "Nothing was written by the refusal.");
         }
         finally
         {
            _settings.WelcomeIMAP = originalWelcome;
         }
      }

      // ----------------------------------------------------------------- helpers

      private static string Setting(string key)
      {
         (int status, string body) answer = Http("GET", "/api/v1/settings");
         ClassicAssert.AreEqual(200, answer.status, answer.body);
         using (JsonDocument document = JsonDocument.Parse(answer.body))
            return document.RootElement.GetProperty(key).GetString();
      }

      private static bool SettingFlag(string key)
      {
         (int status, string body) answer = Http("GET", "/api/v1/settings");
         ClassicAssert.AreEqual(200, answer.status, answer.body);
         using (JsonDocument document = JsonDocument.Parse(answer.body))
            return document.RootElement.GetProperty(key).GetBoolean();
      }

      // The newest row for the setting whose detail holds the text.
      private static long NewestRowFor(string setting, string detailText)
      {
         (int status, string body) trail = Http("GET", AuditPath + "?object_type=setting&limit=20");
         ClassicAssert.AreEqual(200, trail.status, trail.body);
         using (JsonDocument document = JsonDocument.Parse(trail.body))
         {
            foreach (JsonElement entry in document.RootElement.GetProperty("entries").EnumerateArray())
            {
               if (entry.GetProperty("object").GetString() == setting &&
                   (entry.GetProperty("detail").GetString() ?? "").Contains(detailText))
                  return entry.GetProperty("id").GetInt64();
            }
         }
         Assert.Fail("No row records " + setting + " as " + detailText + ": " + trail.body);
         return 0;
      }

      private static (long id, string setting) NewestSettingRowWith(string value)
      {
         (int status, string body) trail = Http("GET", AuditPath + "?object_type=setting&limit=20");
         ClassicAssert.AreEqual(200, trail.status, trail.body);
         using (JsonDocument document = JsonDocument.Parse(trail.body))
         {
            foreach (JsonElement entry in document.RootElement.GetProperty("entries").EnumerateArray())
            {
               string detail = entry.GetProperty("detail").GetString() ?? "";
               if (detail.Contains(" -> " + value))
                  return (entry.GetProperty("id").GetInt64(), entry.GetProperty("object").GetString());
            }
         }
         Assert.Fail("No setting row records a change to " + value + ": " + trail.body);
         return (0, "");
      }

      private static string FirstEntry(string trailJson)
      {
         using (JsonDocument document = JsonDocument.Parse(trailJson))
         {
            JsonElement entries = document.RootElement.GetProperty("entries");
            return entries.GetArrayLength() > 0 ? entries[0].GetRawText() : "";
         }
      }

      private static (int status, string body) Http(string method, string path, string requestBody = null)
      {
         string credentials = Convert.ToBase64String(Encoding.ASCII.GetBytes("Administrator:" + AdminPassword));
         return RestRequest.Send("127.0.0.1", RestPort, method, path, "Basic " + credentials, requestBody);
      }
   }
}
