// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.Domains
{
   /// <summary>
   ///    One value, three levels (docs/SettingLevels.md): a setting a domain or an
   ///    account may hold its own value of is resolved in one place - the account's
   ///    own value, else its domain's, else the server's - and every read site asks
   ///    that one place. Each test here proves what the server then DID, not only
   ///    what it answered, and each has its negative control: a sibling account of
   ///    the same domain with no value of its own, which must behave exactly as an
   ///    account did before the levels existed.
   ///
   ///    Two things the row's brief put one way are done the way the code has
   ///    always worked, and the tests follow the code:
   ///
   ///      * max_message_size_kb has always been the SENDING domain's limit (the
   ///        envelope sender's, at MAIL FROM), so the account's level is the
   ///        sending account's: MAIL FROM an account with 200 KB of its own and
   ///        SIZE=300 KB is refused while its sibling's is taken.
   ///      * the session policies govern the browser sessions (the webmail, the
   ///        portal, the Deck), not IMAP, so the idle timeout is proved on a
   ///        session cookie, as RestApiDevices proves the domain's.
   ///
   ///    Ports: the REST listener on 10040, the range this row was allocated
   ///    (10040-10049). SMTP and POP3 are the suite's own. Every override a test
   ///    sets is cleared in the teardown through the interface that clears it -
   ///    so the overlay's cache is told - whether or not the test reached its end;
   ///    so is every server value a test changed.
   /// </summary>
   [TestFixture]
   public class SettingLevels : TestFixtureBase
   {
      private static int RestPort = 10040;
      private const string AdminPassword = "testar";
      private const string Password = "Levels-Passw0rd!";
      private const string Pest = "pest@example.com";
      private const string Stranger = "stranger@outside-levels.test";
      private const string ExternalHeader = "X-hMailServer-External";

      // What each test changed, put back latest first, each on its own.
      private readonly List<Action> _putBack = new List<Action>();

      [SetUp]
      public void StartRestApi()
      {
         _putBack.Clear();
         _settings.SetAdministratorPassword(AdminPassword);

         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();

         (int status, string body) probe = Admin("GET", "/api/v1/status", null);
         ClassicAssert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);
      }

      [TearDown]
      public void PutTheLevelsBack()
      {
         try
         {
            for (int i = _putBack.Count - 1; i >= 0; i--)
               Guarded_(_putBack[i]);
            _putBack.Clear();
         }
         finally
         {
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      // ---- max_message_size_kb ------------------------------------------------

      [Test]
      [Description("max_message_size_kb at three levels: the server 1000 KB, the domain 500, one account 200. MAIL FROM that account with SIZE=300 KB is refused with the size sentence and its own 200; MAIL FROM its sibling, which has no value of its own, is taken, and at 600 KB refused with the domain's 500. An account's value above the server's is capped by it (the server's value is a ceiling, the rule the domain's has always had). Cleared, the account takes the domain's again. The answers' _effective and _source say the same at every step.")]
      public void TheMaximumMessageSizeIsTheAccountsThenTheDomainsUnderTheServersCeiling()
      {
         Account small = AddAccount("small");
         Account sibling = AddAccount("sibling");

         int serverBefore = _settings.MaxMessageSize;
         _putBack.Add(() => _settings.MaxMessageSize = serverBefore);
         _settings.MaxMessageSize = 1000;
         _domain.MaxMessageSize = 500;
         _domain.Save();

         // Before any account has a value of its own, both are the domain's - as
         // they were before the levels.
         AssertLevel(AccountEntry(small), "max_message_size_kb", "null", "500", "domain", "small, before");
         StringAssert.StartsWith("250", MailFrom(small.Address, 300), "Under the domain's 500, 300 KB is taken.");

         PutAccount(small, "{\"max_message_size_kb\":200}");
         AssertLevel(AccountEntry(small), "max_message_size_kb", "200", "200", "account", "small, its own");
         AssertLevel(AccountEntry(sibling), "max_message_size_kb", "null", "500", "domain", "the sibling");
         AssertLevel(DomainEntry(_domain.Name), "max_message_size_kb", null, "500", "domain", "the domain");
         ClassicAssert.AreEqual("200", small.get_SettingOverride("max_message_size_kb"), "COM reads the same row.");

         string refused = MailFrom(small.Address, 300);
         StringAssert.StartsWith("552", refused, "The account's own 200 KB refuses a 300 KB message.");
         StringAssert.Contains("exceeds fixed maximum message size", refused);
         StringAssert.Contains("Max size: 200 KB", refused);

         // The negative control: the sibling is the domain's.
         StringAssert.StartsWith("250", MailFrom(sibling.Address, 300), "The sibling has no value of its own and takes the domain's 500.");
         string siblingRefused = MailFrom(sibling.Address, 600);
         StringAssert.StartsWith("552", siblingRefused, "And the domain's 500 is still in force for it.");
         StringAssert.Contains("Max size: 500 KB", siblingRefused);

         // The server's value is a ceiling no level below exceeds.
         PutAccount(small, "{\"max_message_size_kb\":5000}");
         AssertLevel(AccountEntry(small), "max_message_size_kb", "5000", "1000", "server", "small, above the server's");
         string capped = MailFrom(small.Address, 1100);
         StringAssert.StartsWith("552", capped, "5000 of its own does not lift the server's 1000.");
         StringAssert.Contains("Max size: 1000 KB", capped);
         StringAssert.StartsWith("250", MailFrom(small.Address, 900), "Under the server's 1000 and over the domain's 500: the account's own 5000 did loosen the domain's.");

         // Cleared: the domain's again.
         PutAccount(small, "{\"max_message_size_kb\":null}");
         AssertLevel(AccountEntry(small), "max_message_size_kb", "null", "500", "domain", "small, cleared");
         ClassicAssert.AreEqual("", small.get_SettingOverride("max_message_size_kb"), "Null over REST removed the row.");
         StringAssert.StartsWith("250", MailFrom(small.Address, 300), "Cleared, the account takes the domain's 500 again.");
      }

      // ---- quarantine_digest_hour ---------------------------------------------

      [Test]
      [Description("quarantine_digest_hour: the domain at 7, one account at 9. The digest pass run as the scheduler would at nine sends the account's digest and not its sibling's, though both have something held; at seven the sibling's and not the account's, though something new is held for it too. Cleared, the account's goes at seven.")]
      public void TheDigestGoesAtTheAccountsOwnHourAndItsSiblingsAtTheDomains()
      {
         IniFileSetting.Write("QuarantineEnabled", "1");
         _putBack.Add(() => IniFileSetting.Delete("QuarantineEnabled"));
         _application.Reinitialize();

         EmptyTheQuarantine();
         _putBack.Add(EmptyTheQuarantine);

         _settings.QuarantineDigestBaseUrl = "http://127.0.0.1:" + RestPort;
         _putBack.Add(() => _settings.QuarantineDigestBaseUrl = "");

         _domain.QuarantineDigest = true;
         _domain.QuarantineDigestHour = 7;
         _domain.Save();

         Account night = AddAccount("night");
         Account day = AddAccount("day");

         PutAccount(night, "{\"quarantine_digest_hour\":9}");
         AssertLevel(AccountEntry(night), "quarantine_digest_hour", "9", "9", "account", "night");
         AssertLevel(AccountEntry(day), "quarantine_digest_hour", "null", "7", "domain", "day");
         AssertLevel(AccountEntry(day), "quarantine_digest", "null", "true", "domain", "day's switch");

         Hold(night, "Night 1");
         Hold(day, "Day 1");

         ClassicAssert.AreEqual(1, _settings.AntiSpam.SendQuarantineDigestsAtHour(9),
            "At nine: the account whose own hour it is, and nobody else.");
         Pop3ClientSimulator.AssertMessageCount(night.Address, Password, 1);
         Pop3ClientSimulator.AssertMessageCount(day.Address, Password, 0, TimeSpan.FromSeconds(2));

         Hold(night, "Night 2");

         ClassicAssert.AreEqual(1, _settings.AntiSpam.SendQuarantineDigestsAtHour(7),
            "At seven: the domain's hour - the sibling, and not the account with an hour of its own, though something new is held for it.");
         Pop3ClientSimulator.AssertMessageCount(day.Address, Password, 1);
         Pop3ClientSimulator.AssertMessageCount(night.Address, Password, 1, TimeSpan.FromSeconds(2));

         PutAccount(night, "{\"quarantine_digest_hour\":null}");
         AssertLevel(AccountEntry(night), "quarantine_digest_hour", "null", "7", "domain", "night, cleared");

         ClassicAssert.AreEqual(1, _settings.AntiSpam.SendQuarantineDigestsAtHour(7),
            "Cleared, the account takes the domain's seven, and what was held for it since goes.");
         Pop3ClientSimulator.AssertMessageCount(night.Address, Password, 2);
      }

      // ---- the external-sender tag ------------------------------------------

      [Test]
      [Description("external_tag_subject and external_tag_header: the domain on, one account off - its mail from outside is not tagged while its sibling's, which inherits, is; the account cleared is tagged again. Then the other way: the domain off, the account's header on - it is stamped and its sibling is not.")]
      public void TheExternalTagIsTheRecipientAccountsOwnWhereItHasOne()
      {
         _domain.ExternalTagSubject = true;
         _domain.ExternalTagHeader = true;
         _domain.Save();

         Account quiet = AddAccount("quiet");
         Account loud = AddAccount("loud");

         PutAccount(quiet, "{\"external_tag_subject\":false,\"external_tag_header\":false}");
         AssertLevel(AccountEntry(quiet), "external_tag_header", "false", "false", "account", "quiet");
         AssertLevel(AccountEntry(loud), "external_tag_header", "null", "true", "domain", "loud");

         SmtpClientSimulator.StaticSend(Stranger, quiet.Address, "Invoice", "Body");
         string toQuiet = Pop3ClientSimulator.AssertGetFirstMessageText(quiet.Address, Password);
         StringAssert.Contains("Subject: Invoice", toQuiet, "The account said no tag for its own mail.");
         StringAssert.DoesNotContain("[EXTERNAL]", toQuiet);
         StringAssert.DoesNotContain(ExternalHeader, toQuiet);

         // The negative control: the sibling inherits the domain's on.
         SmtpClientSimulator.StaticSend(Stranger, loud.Address, "Invoice", "Body");
         string toLoud = Pop3ClientSimulator.AssertGetFirstMessageText(loud.Address, Password);
         StringAssert.Contains("Subject: [EXTERNAL] Invoice", toLoud, "The sibling has no value of its own and is tagged as the domain says.");
         StringAssert.Contains(ExternalHeader, toLoud);

         PutAccount(quiet, "{\"external_tag_subject\":null,\"external_tag_header\":null}");
         AssertLevel(AccountEntry(quiet), "external_tag_header", "null", "true", "domain", "quiet, cleared");
         SmtpClientSimulator.StaticSend(Stranger, quiet.Address, "Receipt", "Body");
         string cleared = Pop3ClientSimulator.AssertGetFirstMessageText(quiet.Address, Password);
         StringAssert.Contains("Subject: [EXTERNAL] Receipt", cleared, "Cleared, the account is tagged as its domain says.");
         StringAssert.Contains(ExternalHeader, cleared);

         // The other way round: the domain off, the account's own header on.
         _domain.ExternalTagSubject = false;
         _domain.ExternalTagHeader = false;
         _domain.Save();
         PutAccount(quiet, "{\"external_tag_header\":true}");

         SmtpClientSimulator.StaticSend(Stranger, quiet.Address, "Statement", "Body");
         string stamped = Pop3ClientSimulator.AssertGetFirstMessageText(quiet.Address, Password);
         StringAssert.Contains(ExternalHeader, stamped, "The account's own on, under a domain that is off.");
         StringAssert.Contains("Subject: Statement", stamped, "Only the header was the account's; the subject is the domain's off.");

         SmtpClientSimulator.StaticSend(Stranger, loud.Address, "Statement", "Body");
         string unstamped = Pop3ClientSimulator.AssertGetFirstMessageText(loud.Address, Password);
         StringAssert.DoesNotContain(ExternalHeader, unstamped, "The sibling is the domain's off.");
      }

      // ---- session_idle_seconds ----------------------------------------------

      [Test]
      [Description("session_idle_seconds: the domain 600, one account 1. The account's browser session announces 1 and is refused after two idle seconds; its sibling's announces 600 and still works. Cleared, the account's next session announces the domain's 600 and outlives the same wait.")]
      public void TheIdleTimeoutIsTheAccountsOwnWhereItHasOne()
      {
         Account brief = AddAccount("brief");
         Account patient = AddAccount("patient");

         ClassicAssert.IsTrue(_application.Utilities.SetDomainSessionPolicy(_domain.Name, 600, 0, 0, 0));
         PutAccount(brief, "{\"session_idle_seconds\":1}");

         AssertLevel(AccountEntry(brief), "session_idle_seconds", "1", "1", "account", "brief");
         AssertLevel(AccountEntry(patient), "session_idle_seconds", "null", "600", "domain", "patient");

         (string cookie, long idle) briefSession = SignIn(brief);
         (string cookie, long idle) patientSession = SignIn(patient);
         ClassicAssert.AreEqual(1, briefSession.idle, "The account's own idle timeout is what its sign-in announces.");
         ClassicAssert.AreEqual(600, patientSession.idle, "The sibling's is the domain's.");

         ClassicAssert.AreEqual(200, WithCookie(briefSession.cookie).status, "A fresh session works.");
         ClassicAssert.AreEqual(200, WithCookie(patientSession.cookie).status, "A fresh session works.");

         Thread.Sleep(2200);

         ClassicAssert.AreEqual(401, WithCookie(briefSession.cookie).status, "Idle for longer than the account's own second, the session is refused.");
         ClassicAssert.AreEqual(200, WithCookie(patientSession.cookie).status, "The sibling's session, under the domain's 600, still works.");

         PutAccount(brief, "{\"session_idle_seconds\":null}");
         AssertLevel(AccountEntry(brief), "session_idle_seconds", "null", "600", "domain", "brief, cleared");

         (string cookie, long idle) again = SignIn(brief);
         ClassicAssert.AreEqual(600, again.idle, "Cleared, the account's sessions are the domain's.");
         Thread.Sleep(2200);
         ClassicAssert.AreEqual(200, WithCookie(again.cookie).status, "And they outlive the wait that ended the last one.");
      }

      // ---- recall_days ------------------------------------------------------

      [Test]
      [Description("recall_days at three levels through the overlay: the server 1, the domain 30, the sender 2. A message sent two and a bit days ago is outside the server's day (404), inside the domain's thirty (recalled), and - for the next message - outside the sender's own two (404) though the domain says thirty; cleared, the sender takes the domain's thirty and the message is recalled.")]
      public void TheRecallRouteHonoursTheSendersOwnDays()
      {
         Account sender = AddAccount("recall-sender");
         Account reader = AddAccount("recall-reader");

         int serverBefore = ServerRecallDays();
         _putBack.Add(() => Admin("PUT", "/api/v1/settings", "{\"recall_days\":" + serverBefore + "}"));
         ClassicAssert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"recall_days\":1}").status);

         SendAuthenticated(sender, reader.Address, "levels-recall-1@levels.test", "First");
         SendAuthenticated(sender, reader.Address, "levels-recall-2@levels.test", "Second");
         Pop3ClientSimulator.AssertMessageCount(reader.Address, Password, 2);

         // Two and a bit days old, as RestApiMessageRecall backdates.
         _application.Database.ExecuteSQL("update hm_recall set recallcreated = recallcreated - 200000 where recallmsgid like 'levels-recall-%@levels.test'");

         string me = Basic(sender.Address, Password);

         AssertLevel(AccountEntry(sender), "recall_days", "null", "1", "server", "sender, before");
         (int status, string body) outside = As(me, "POST", "/api/v1/me/recall", "{\"message_id\":\"<levels-recall-1@levels.test>\"}");
         ClassicAssert.AreEqual(404, outside.status, "Under the server's one day the message is out of reach. " + outside.body);

         PutDomain(_domain.Name, "{\"active\":true,\"recall_days\":30}");
         AssertLevel(DomainEntry(_domain.Name), "recall_days", "30", "30", "domain", "the domain");
         AssertLevel(AccountEntry(sender), "recall_days", "null", "30", "domain", "sender, under the domain's");
         (int status, string body) inside = As(me, "POST", "/api/v1/me/recall", "{\"message_id\":\"<levels-recall-1@levels.test>\"}");
         ClassicAssert.AreEqual(200, inside.status, inside.body);
         StringAssert.Contains("\"recalled\":1", inside.body, "The domain's thirty days reach it.");
         Pop3ClientSimulator.AssertMessageCount(reader.Address, Password, 1);

         PutAccount(sender, "{\"recall_days\":2}");
         AssertLevel(AccountEntry(sender), "recall_days", "2", "2", "account", "sender, its own");
         (int status, string body) own = As(me, "POST", "/api/v1/me/recall", "{\"message_id\":\"<levels-recall-2@levels.test>\"}");
         ClassicAssert.AreEqual(404, own.status, "The sender's own two days, though its domain says thirty. " + own.body);
         Pop3ClientSimulator.AssertMessageCount(reader.Address, Password, 1, TimeSpan.FromSeconds(2));

         PutAccount(sender, "{\"recall_days\":null}");
         AssertLevel(AccountEntry(sender), "recall_days", "null", "30", "domain", "sender, cleared");
         (int status, string body) back = As(me, "POST", "/api/v1/me/recall", "{\"message_id\":\"<levels-recall-2@levels.test>\"}");
         ClassicAssert.AreEqual(200, back.status, back.body);
         StringAssert.Contains("\"recalled\":1", back.body, "Cleared, the domain's thirty again.");
         Pop3ClientSimulator.AssertMessageCount(reader.Address, Password, 0);
      }

      // ---- the overlay's own rules -----------------------------------------

      [Test]
      [Description("The overlay's rows go with their owner: deleting an account deletes its rows and deleting a domain deletes the domain's, each proved by the probe - an insert of the same (scope, owner, name), which the unique index refuses while the row is there and takes once it is gone. A sibling's row stays.")]
      public void AnOwnersRowsAreDeletedWithIt()
      {
         Account leaving = AddAccount("leaving");
         Account staying = AddAccount("staying");
         leaving.set_SettingOverride("recall_days", "5");
         staying.set_SettingOverride("recall_days", "6");
         _putBack.Add(() => staying.set_SettingOverride("recall_days", ""));
         long leavingId = leaving.ID;
         long stayingId = staying.ID;

         Domain other = SingletonProvider<TestSetup>.Instance.AddDomain("levels-cascade.test");
         other.set_SettingOverride("recall_days", "9");
         long otherId = other.ID;

         // The probe's own control: while the row is there, the insert is refused.
         ClassicAssert.IsFalse(ProbeInsert(2, leavingId), "The account's row is there before the account is deleted.");
         ClassicAssert.IsFalse(ProbeInsert(1, otherId), "The domain's row is there before the domain is deleted.");

         leaving.Delete();
         other.Delete();

         ClassicAssert.IsTrue(ProbeInsert(2, leavingId), "Deleting the account deleted its row.");
         ClassicAssert.IsTrue(ProbeInsert(1, otherId), "Deleting the domain deleted its row.");
         ClassicAssert.IsFalse(ProbeInsert(2, stayingId), "The sibling's row is not the deleted account's and stays.");
         ClassicAssert.AreEqual("6", staying.get_SettingOverride("recall_days"));

         _application.Database.ExecuteSQL("delete from hm_settingoverrides where soname = 'recall_days' and ((soscope = 2 and soownerid = " + leavingId + ") or (soscope = 1 and soownerid = " + otherId + "))");
      }

      [Test]
      [Description("The XML backup carries the overlay's rows under the domain and the account: a backup, every domain deleted, a restore - and the domain's and the account's own values are back, with a sibling that had none still inheriting.")]
      public void TheBackupCarriesTheOverlay()
      {
         string address = "kept@" + _domain.Name;
         string siblingAddress = "plain@" + _domain.Name;
         Account kept = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, address, Password);
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, siblingAddress, Password);

         _domain.set_SettingOverride("recall_days", "30");
         kept.set_SettingOverride("recall_days", "5");
         kept.set_SettingOverride("quarantine_digest_hour", "9");
         kept.set_SettingOverride("max_message_size_kb", "200");

         string directory = Path.Combine(Path.GetTempPath(), "hm-levels-" + TestSetup.UniqueString());
         Directory.CreateDirectory(directory);
         _putBack.Add(() => Directory.Delete(directory, true));

         RunBackup(directory);

         while (_application.Domains.Count > 0)
            _application.Domains[0].Delete();

         Restore(directory);

         Domain restored = _application.Domains.get_ItemByName(_domain.Name);
         Account restoredKept = restored.Accounts.get_ItemByAddress(address);
         Account restoredSibling = restored.Accounts.get_ItemByAddress(siblingAddress);

         ClassicAssert.AreEqual("30", restored.get_SettingOverride("recall_days"), "The domain's own value came back.");
         ClassicAssert.AreEqual("5", restoredKept.get_SettingOverride("recall_days"), "The account's own values came back.");
         ClassicAssert.AreEqual("9", restoredKept.get_SettingOverride("quarantine_digest_hour"));
         ClassicAssert.AreEqual("200", restoredKept.get_SettingOverride("max_message_size_kb"));
         ClassicAssert.AreEqual("account", restoredKept.get_EffectiveSettingSource("recall_days"));

         // The negative control: nothing was invented for the account that had nothing.
         ClassicAssert.AreEqual("", restoredSibling.get_SettingOverride("recall_days"), "The sibling had no value of its own and has none now.");
         ClassicAssert.AreEqual("30", restoredSibling.get_EffectiveSetting("recall_days"));
         ClassicAssert.AreEqual("domain", restoredSibling.get_EffectiveSettingSource("recall_days"));
      }

      [Test]
      [Description("COM and REST agree, and refuse alike: a domain's and an account's own values round-trip over COM with EffectiveSetting and EffectiveSettingSource, an empty string or a REST null makes a level inherit, a name that is the server's alone is refused on both doors, a value the setting refuses is refused on both with its own sentence and nothing stored, a level held in a field of its own is refused over the overlay, _effective and _source are never taken, and the server's own routes say which keys have levels.")]
      public void ComAndRestRoundTripAndRefuseAlike()
      {
         Account mine = AddAccount("mine");
         Account theirs = AddAccount("theirs");
         int server = ServerRecallDays();

         _domain.set_SettingOverride("recall_days", "30");
         _putBack.Add(() => _domain.set_SettingOverride("recall_days", ""));
         ClassicAssert.AreEqual("30", _domain.get_SettingOverride("recall_days"));
         ClassicAssert.AreEqual("30", _domain.get_EffectiveSetting("recall_days"));
         ClassicAssert.AreEqual("domain", _domain.get_EffectiveSettingSource("recall_days"));

         mine.set_SettingOverride("recall_days", "5");
         _putBack.Add(() => mine.set_SettingOverride("recall_days", ""));
         ClassicAssert.AreEqual("5", mine.get_SettingOverride("recall_days"));
         ClassicAssert.AreEqual("5", mine.get_EffectiveSetting("recall_days"));
         ClassicAssert.AreEqual("account", mine.get_EffectiveSettingSource("recall_days"));
         ClassicAssert.AreEqual("", theirs.get_SettingOverride("recall_days"), "The sibling has none of its own.");
         ClassicAssert.AreEqual("30", theirs.get_EffectiveSetting("recall_days"));
         ClassicAssert.AreEqual("domain", theirs.get_EffectiveSettingSource("recall_days"));
         AssertLevel(AccountEntry(mine), "recall_days", "5", "5", "account", "REST reads what COM wrote");

         // REST null and COM's empty string are the same "take the level above".
         PutAccount(mine, "{\"recall_days\":null}");
         ClassicAssert.AreEqual("", mine.get_SettingOverride("recall_days"));
         ClassicAssert.AreEqual("domain", mine.get_EffectiveSettingSource("recall_days"));
         _domain.set_SettingOverride("recall_days", "");
         ClassicAssert.AreEqual(server.ToString(), mine.get_EffectiveSetting("recall_days"), "Neither level holds one: the server's.");
         ClassicAssert.AreEqual("server", mine.get_EffectiveSettingSource("recall_days"));

         // A name that is the server's alone, on both doors.
         COMException comServerOnly = Assert.Throws<COMException>(() => mine.set_SettingOverride("max_delivery_threads", "5"));
         StringAssert.Contains("GET /api/v1/settings/levels", comServerOnly.Message);
         Assert.Throws<COMException>(() => _domain.set_SettingOverride("max_delivery_threads", "5"));
         Assert.Throws<COMException>(() => mine.get_EffectiveSetting("max_delivery_threads"));
         ClassicAssert.AreEqual(400, Admin("PUT", "/api/v1/accounts/" + mine.Address, "{\"max_delivery_threads\":5}").status);
         ClassicAssert.AreEqual(400, Admin("PUT", "/api/v1/domains/" + _domain.Name, "{\"active\":true,\"max_delivery_threads\":5}").status);

         // A value the setting refuses, with its own sentence, and nothing stored.
         COMException comRange = Assert.Throws<COMException>(() => mine.set_SettingOverride("recall_days", "0"));
         StringAssert.Contains("recall_days must be between 1 and 365", comRange.Message);
         (int status, string body) restRange = Admin("PUT", "/api/v1/accounts/" + mine.Address, "{\"recall_days\":366}");
         ClassicAssert.AreEqual(400, restRange.status, restRange.body);
         StringAssert.Contains("recall_days must be between 1 and 365", restRange.body);
         ClassicAssert.AreEqual("", mine.get_SettingOverride("recall_days"), "A refused value stored nothing.");

         // A level held in a field of its own is set there, not in the overlay.
         COMException column = Assert.Throws<COMException>(() => _domain.set_SettingOverride("max_message_size_kb", "300"));
         StringAssert.Contains("own field", column.Message);

         // What the answer says is in effect is never taken.
         (int status, string body) readOnly = Admin("PUT", "/api/v1/accounts/" + mine.Address, "{\"recall_days_effective\":7}");
         ClassicAssert.AreEqual(400, readOnly.status, readOnly.body);
         StringAssert.Contains("never taken", readOnly.body);

         // The server's routes say which keys have levels, and the list says what they are.
         (int status, string body) settings = Admin("GET", "/api/v1/settings", null);
         ClassicAssert.AreEqual(200, settings.status, settings.body);
         StringAssert.Contains("\"recall_days_levels\":\"account\"", settings.body);
         StringAssert.DoesNotContain("\"max_delivery_threads_levels\"", settings.body, "A server-only key says nothing of levels.");
         (int status, string body) levels = Admin("GET", "/api/v1/settings/levels", null);
         ClassicAssert.AreEqual(200, levels.status, levels.body);
         StringAssert.Contains("\"name\":\"recall_days\"", levels.body);
         StringAssert.Contains("\"name\":\"quarantine_digest_hour\"", levels.body);
         StringAssert.DoesNotContain("\"name\":\"max_delivery_threads\"", levels.body);
      }

      [Test]
      [Description("A key restricted to one domain may set its own domain's and its own accounts' values and not another domain's, and may not set a value that is the server administrator's (a limit or a ceiling) even on its own; the server-wide list of the settings with levels is refused to it.")]
      public void ADomainRestrictedKeySetsItsOwnDomainsLevelsAndNoOthers()
      {
         Account own = AddAccount("own");
         Domain elsewhere = SingletonProvider<TestSetup>.Instance.AddDomain("levels-elsewhere.test");
         Account foreign = SingletonProvider<TestSetup>.Instance.AddAccount(elsewhere, "foreign@levels-elsewhere.test", Password);
         _putBack.Add(() => own.set_SettingOverride("recall_days", ""));
         _putBack.Add(() => _domain.set_SettingOverride("recall_days", ""));

         (int status, string body) created = Admin("POST", "/api/v1/apikeys",
            "{\"label\":\"levels - here\",\"scope\":\"full\",\"domains\":\"" + _domain.Name + "\"}");
         ClassicAssert.AreEqual(201, created.status, "POST /api/v1/apikeys must create a key. Body: " + created.body);
         JsonElement key = Parse(created.body);
         string keyId = key.GetProperty("id").ToString();
         string bearer = "Bearer " + key.GetProperty("key").GetString();
         _putBack.Add(() => Admin("DELETE", "/api/v1/apikeys/" + keyId, null));

         (int status, string body) ownAccount = As(bearer, "PUT", "/api/v1/accounts/" + own.Address, "{\"recall_days\":7}");
         ClassicAssert.AreEqual(200, ownAccount.status, ownAccount.body);
         ClassicAssert.AreEqual("7", own.get_SettingOverride("recall_days"), "Its own domain's account took it.");

         (int status, string body) ownDomain = As(bearer, "PUT", "/api/v1/domains/" + _domain.Name, "{\"active\":true,\"recall_days\":9}");
         ClassicAssert.AreEqual(200, ownDomain.status, ownDomain.body);
         ClassicAssert.AreEqual("9", _domain.get_SettingOverride("recall_days"), "Its own domain took it.");

         // The negative control: another domain's, through the same key.
         ClassicAssert.AreEqual(403, As(bearer, "PUT", "/api/v1/accounts/" + foreign.Address, "{\"recall_days\":7}").status);
         ClassicAssert.AreEqual(403, As(bearer, "PUT", "/api/v1/domains/" + elsewhere.Name, "{\"active\":true,\"recall_days\":9}").status);
         ClassicAssert.AreEqual("", foreign.get_SettingOverride("recall_days"), "Another domain's account was not touched.");
         ClassicAssert.AreEqual("", elsewhere.get_SettingOverride("recall_days"), "Another domain was not touched.");

         // A limit is the server administrator's, even on its own account.
         (int status, string body) limit = As(bearer, "PUT", "/api/v1/accounts/" + own.Address, "{\"max_message_size_kb\":50000}");
         ClassicAssert.AreEqual(403, limit.status, limit.body);
         ClassicAssert.AreEqual("", own.get_SettingOverride("max_message_size_kb"), "The refused limit stored nothing.");

         ClassicAssert.AreEqual(403, As(bearer, "GET", "/api/v1/settings/levels", null).status, "The list is server-wide.");
      }

      [Test]
      [Description("build/check-setting-levels.py, which holds every read of a setting with levels to the resolver, passes on this checkout - and it passes only because its negative control (a new function reading a domain's digest hour straight, and a read site with its resolver call taken out, each in a temporary copy) is refused on the same run. Ignored where there is no Python or no checkout.")]
      public void TheCheckerRefusesAReadSiteWithoutTheResolver()
      {
         string python = FindPython();
         if (python == null)
            Assert.Ignore("No python.exe on the PATH, so the checker cannot run here.");

         string checker = RepositoryFile(Path.Combine("build", "check-setting-levels.py"));

         var start = new ProcessStartInfo(python, "\"" + checker + "\"")
         {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8,
         };
         start.EnvironmentVariables["PYTHONIOENCODING"] = "utf-8";

         using (Process process = Process.Start(start))
         {
            Task<string> output = process.StandardOutput.ReadToEndAsync();
            Task<string> error = process.StandardError.ReadToEndAsync();
            ClassicAssert.IsTrue(process.WaitForExit(120 * 1000), "The checker did not finish within two minutes.");
            process.WaitForExit();

            string printed = output.Result + error.Result;
            ClassicAssert.AreEqual(0, process.ExitCode, printed);
            StringAssert.Contains("the negative control", printed);
            StringAssert.Contains("is refused", printed);
            StringAssert.DoesNotContain("self-test:", printed, "The negative control did not hold.");
         }
      }

      // ---- helpers -------------------------------------------------------------

      private static void Guarded_(Action step)
      {
         try
         {
            step();
         }
         catch (Exception e) when (!ExceptionPolicy.IsFatal(e))
         {
            Console.WriteLine("SettingLevels teardown: " + e.Message);
         }
      }

      private Account AddAccount(string localPart)
      {
         return SingletonProvider<TestSetup>.Instance.AddAccount(_domain, localPart + "@" + _domain.Name, Password);
      }

      private static string Basic(string user, string password)
      {
         return "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(user + ":" + password));
      }

      private static (int status, string body) Admin(string method, string path, string requestBody)
      {
         return RestRequest.Send("127.0.0.1", RestPort, method, path, Basic("Administrator", AdminPassword), requestBody);
      }

      private static (int status, string body) As(string authorization, string method, string path, string requestBody)
      {
         return RestRequest.Send("127.0.0.1", RestPort, method, path, authorization, requestBody);
      }

      private static JsonElement Parse(string body)
      {
         return JsonDocument.Parse(body).RootElement.Clone();
      }

      private static JsonElement AccountEntry(Account account)
      {
         (int status, string body) answer = Admin("GET", "/api/v1/accounts/" + account.Address, null);
         ClassicAssert.AreEqual(200, answer.status, "GET the account: " + answer.body);
         return Parse(answer.body);
      }

      private static JsonElement DomainEntry(string name)
      {
         (int status, string body) answer = Admin("GET", "/api/v1/domains/" + name, null);
         ClassicAssert.AreEqual(200, answer.status, "GET the domain: " + answer.body);
         return Parse(answer.body);
      }

      /// <summary>
      ///    PUT /api/v1/accounts/{address} as the administrator, and every name in the
      ///    body cleared again in the teardown.
      /// </summary>
      private void PutAccount(Account account, string json)
      {
         foreach (JsonProperty member in Parse(json).EnumerateObject())
         {
            string name = member.Name;
            _putBack.Add(() => account.set_SettingOverride(name, ""));
         }

         (int status, string body) answer = Admin("PUT", "/api/v1/accounts/" + account.Address, json);
         ClassicAssert.AreEqual(200, answer.status, "PUT the account " + json + ": " + answer.body);
      }

      /// <summary>The same for a domain's own values that the overlay holds.</summary>
      private void PutDomain(string name, string json)
      {
         Domain domain = _application.Domains.get_ItemByName(name);
         foreach (JsonProperty member in Parse(json).EnumerateObject())
         {
            if (member.Name == "active")
               continue;
            string setting = member.Name;
            _putBack.Add(() => domain.set_SettingOverride(setting, ""));
         }

         (int status, string body) answer = Admin("PUT", "/api/v1/domains/" + name, json);
         ClassicAssert.AreEqual(200, answer.status, "PUT the domain " + json + ": " + answer.body);
      }

      /// <summary>
      ///    The level's own value (as JSON, or null to leave it unasked: a domain's
      ///    field keeps the key it always had), what is in effect and where it came from.
      /// </summary>
      private static void AssertLevel(JsonElement entry, string name, string own, string effective, string source, string context)
      {
         if (own != null)
            ClassicAssert.AreEqual(own, entry.GetProperty(name).GetRawText(), context + ": " + name + ", the level's own value");
         ClassicAssert.AreEqual(effective, entry.GetProperty(name + "_effective").GetRawText(), context + ": " + name + "_effective");
         ClassicAssert.AreEqual(source, entry.GetProperty(name + "_source").GetString(), context + ": " + name + "_source");
      }

      private static int ServerRecallDays()
      {
         (int status, string body) settings = Admin("GET", "/api/v1/settings", null);
         ClassicAssert.AreEqual(200, settings.status, settings.body);
         Match match = Regex.Match(settings.body, "\"recall_days\":(\\d+)");
         ClassicAssert.IsTrue(match.Success, "No recall_days in GET /api/v1/settings: " + settings.body);
         return int.Parse(match.Groups[1].Value);
      }

      /// <summary>MAIL FROM the address declaring the size, on a session of its own; the answer.</summary>
      private static string MailFrom(string sender, int kilobytes)
      {
         using (var socket = new TcpConnection())
         {
            ClassicAssert.IsTrue(socket.Connect(25));
            ClassicAssert.IsTrue(socket.Receive().StartsWith("220"));
            socket.Send("EHLO example.test\r\n");
            string ehlo = socket.ReadUntil("250 HELP");
            ClassicAssert.IsTrue(ehlo.Contains("250"), "EHLO was not accepted. Got: " + ehlo);
            string answer = socket.SendAndReceive("MAIL FROM:<" + sender + "> SIZE=" + (kilobytes * 1024) + "\r\n");
            socket.Send("QUIT\r\n");
            return answer;
         }
      }

      private static void SendAuthenticated(Account from, string to, string messageId, string subject)
      {
         new SmtpClientSimulator().SendRaw(from.Address, Password, from.Address, to,
            "From: " + from.Address + "\r\nTo: " + to + "\r\nSubject: " + subject + "\r\nMessage-ID: <" + messageId + ">\r\n\r\n" + subject + ", sent over authenticated SMTP.\r\n");
      }

      /// <summary>
      ///    Whether an overlay row of this scope and owner could be inserted: false
      ///    while one is there (the unique index refuses it), true once it is gone -
      ///    in which case the probe's own row is left for the caller to delete.
      /// </summary>
      private bool ProbeInsert(int scope, long ownerId)
      {
         try
         {
            _application.Database.ExecuteSQL("insert into hm_settingoverrides (soscope, soownerid, soname, sovalue) values (" +
                                             scope + ", " + ownerId + ", 'recall_days', '5')");
            return true;
         }
         catch (COMException)
         {
            // The refusal is reported to the error log as well; that line is the
            // probe's, not a fault.
            LogHandler.ReadAndDeleteErrorLog();
            return false;
         }
      }

      // The quarantine digest's own way of holding a message: the account blocks
      // a sender and the sender's mail is kept (QuarantineDigests).
      private void Hold(Account account, string subject)
      {
         (int status, string body) blocked = As(Basic(account.Address, Password), "POST", "/api/v1/me/sender-list",
            "{\"value\":\"" + Pest + "\",\"blocked\":true}");
         ClassicAssert.IsTrue(blocked.status == 201 || blocked.status == 200, blocked.body);

         var quarantine = _settings.AntiSpam.Quarantine;
         quarantine.Refresh();
         int before = quarantine.Count;

         new SmtpClientSimulator().Send(Pest, account.Address, subject, "Nothing wrong with this message but its sender.");
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         DateTime until = DateTime.UtcNow.AddSeconds(30);
         while (true)
         {
            quarantine.Refresh();
            if (quarantine.Count == before + 1 || DateTime.UtcNow > until)
               break;
            Thread.Sleep(100);
         }

         ClassicAssert.AreEqual(before + 1, quarantine.Count, "The message was held.");
      }

      private void EmptyTheQuarantine()
      {
         var quarantine = _settings.AntiSpam.Quarantine;
         quarantine.Refresh();

         while (quarantine.Count > 0)
         {
            quarantine.DeleteByDBID(quarantine[0].ID);
            quarantine.Refresh();
         }
      }

      // ---- a browser session -------------------------------------------------

      /// <summary>POST /api/v1/session as the account: the cookie and the idle timeout it announced.</summary>
      private static (string cookie, long idle) SignIn(Account account)
      {
         (int status, string setCookie, string body) started = Raw("POST", "/api/v1/session", Basic(account.Address, Password), "User-Agent: Levels/1.0\r\n");
         ClassicAssert.AreEqual(201, started.status, started.body);

         string cookie = started.setCookie;
         int start = cookie.IndexOf('=') + 1;
         int end = cookie.IndexOf(';', start);
         cookie = end > start ? cookie.Substring(start, end - start) : cookie.Substring(start);

         return (cookie, Parse(started.body).GetProperty("idle_seconds").GetInt64());
      }

      private static (int status, string body) WithCookie(string cookie)
      {
         (int status, string setCookie, string body) answer = Raw("GET", "/api/v1/me", null, "Cookie: hmailsession=" + cookie + "\r\n");
         return (answer.status, answer.body);
      }

      /// <summary>One HTTP/1.0 request with extra header lines; the status, the Set-Cookie header and the body.</summary>
      private static (int status, string setCookie, string body) Raw(string method, string path, string authorization, string extraHeaders)
      {
         using (var client = new TcpClient())
         {
            Exception last = null;
            for (int attempt = 0; attempt < 25; attempt++)
            {
               try
               {
                  client.Connect("127.0.0.1", RestPort);
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
               head.Append(method + " " + path + " HTTP/1.0\r\n");
               head.Append("Host: 127.0.0.1\r\n");
               if (authorization != null)
                  head.Append("Authorization: " + authorization + "\r\n");
               if (extraHeaders != null)
                  head.Append(extraHeaders);
               head.Append("Connection: close\r\n\r\n");

               byte[] headBytes = Encoding.ASCII.GetBytes(head.ToString());
               stream.Write(headBytes, 0, headBytes.Length);

               byte[] buffer = new byte[8192];
               int read;
               while ((read = stream.Read(buffer, 0, buffer.Length)) > 0)
                  memory.Write(buffer, 0, read);

               string raw = Encoding.UTF8.GetString(memory.ToArray());
               int separator = raw.IndexOf("\r\n\r\n", StringComparison.Ordinal);
               string headers = separator >= 0 ? raw.Substring(0, separator) : raw;
               string body = separator >= 0 ? raw.Substring(separator + 4) : "";

               string[] lines = headers.Split(new[] { "\r\n" }, StringSplitOptions.None);
               int status = 0;
               string[] first = lines[0].Split(' ');
               if (first.Length >= 2)
                  int.TryParse(first[1], out status);

               string setCookie = "";
               for (int i = 1; i < lines.Length; i++)
               {
                  if (lines[i].StartsWith("Set-Cookie:", StringComparison.OrdinalIgnoreCase))
                     setCookie = lines[i].Substring("Set-Cookie:".Length).Trim();
               }

               return (status, setCookie, body);
            }
         }
      }

      // ---- a backup and a restore (BackupAccountStores' way) -------------------

      private void RunBackup(string directory)
      {
         var backup = _application.Settings.Backup;
         backup.BackupDomains = true;
         backup.BackupMessages = false;
         backup.BackupSettings = false;
         backup.CompressDestinationFiles = true;
         backup.Destination = directory;

         CustomAsserts.AssertDeleteFile(backup.LogFile);

         _application.BackupManager.StartBackup();

         for (int attempt = 0; attempt < 120; attempt++)
         {
            try
            {
               string log = TestSetup.ReadExistingTextFile(backup.LogFile);

               if (log.Contains("BACKUP ERROR:"))
                  Assert.Fail("The backup failed. Log: " + log);

               if (log.Contains("Backup completed successfully"))
                  return;
            }
            catch (Exception sharing) when (!ExceptionPolicy.IsFatal(sharing))
            {
               // The server is writing the file; read it again in a moment.
            }

            Thread.Sleep(250);
         }

         Assert.Fail("The backup did not finish within 30 seconds.");
      }

      private void Restore(string directory)
      {
         string startTime = _application.Status.StartTime;

         // Status.StartTime has one-second resolution; step past the second.
         Thread.Sleep(1100);

         FileInfo[] archives = new DirectoryInfo(directory).GetFiles("*.7z");
         ClassicAssert.AreEqual(1, archives.Length, "Expected exactly one archive in " + directory);

         var backup = _application.BackupManager.LoadBackup(archives[0].FullName);
         backup.RestoreDomains = true;
         backup.RestoreMessages = false;
         backup.RestoreSettings = false;
         backup.StartRestore();

         for (int attempt = 0; attempt < 600; attempt++)
         {
            try
            {
               string now = _application.Status.StartTime;
               if (now.Length > 0 && now != startTime)
               {
                  Thread.Sleep(500);
                  return;
               }
            }
            catch (Exception talking) when (!ExceptionPolicy.IsFatal(talking))
            {
               // The COM server is reinitialising.
            }

            Thread.Sleep(100);
         }

         Assert.Fail("The restore did not finish within a minute.");
      }

      // ---- the checkout ------------------------------------------------------

      // python.exe from the PATH, skipping the Microsoft Store's stand-in.
      private static string FindPython()
      {
         foreach (string entry in (Environment.GetEnvironmentVariable("PATH") ?? "").Split(Path.PathSeparator))
         {
            string directory = entry.Trim().Trim('"');
            if (directory.Length == 0 || directory.IndexOf("WindowsApps", StringComparison.OrdinalIgnoreCase) >= 0)
               continue;

            try
            {
               string candidate = Path.Combine(directory, "python.exe");
               if (File.Exists(candidate))
                  return candidate;
            }
            catch (ArgumentException)
            {
               // A PATH entry with a character a path cannot hold.
            }
         }

         return null;
      }

      // A file of the checkout, found by walking up from the test binaries.
      private static string RepositoryFile(string relativePath)
      {
         for (DirectoryInfo directory = new DirectoryInfo(TestContext.CurrentContext.TestDirectory); directory != null; directory = directory.Parent)
         {
            string candidate = Path.Combine(directory.FullName, relativePath);
            if (File.Exists(candidate))
               return candidate;
         }

         Assert.Ignore("These binaries are not inside a checkout, so " + relativePath + " cannot be found.");
         return null;
      }
   }
}
