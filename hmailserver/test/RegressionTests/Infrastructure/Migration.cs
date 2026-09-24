// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Threading;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.ExternalAccounts;
using RegressionTests.Shared;

namespace RegressionTests.Infrastructure
{
   /// <summary>
   ///    Migration jobs (docs/Migration.md, "Five hundred mailboxes: migration
   ///    jobs"): many mailboxes copied from another server, a job per mailbox,
   ///    throttled, resumed after an interruption and reported on.
   ///
   ///    The source is this server itself. Every test makes a domain source.test
   ///    beside the suite's example.test, fills its accounts over IMAP with
   ///    APPEND - folders, flags, a keyword, an internal date, a Message-ID of
   ///    each message's own - and points a migration at 127.0.0.1:143 without
   ///    TLS, so the job speaks to a real IMAP server and files into real
   ///    accounts, and everything asserted is read back from the target over IMAP
   ///    or REST, never from the migration's own answer alone. The contacts and
   ///    calendars come from the source's own CardDAV and CalDAV on the web
   ///    services listener, plain HTTP on 127.0.0.1, which is the one place a
   ///    migration may read DAV without TLS. What a real server would rarely do
   ///    on cue - refuse a logon for now, refuse one folder for good and another
   ///    for now, refuse a message, lose one between the search and the fetch,
   ///    list other people's mailboxes - a ScriptedImapServer does.
   ///
   ///    A Google Workspace job's sign-in is exercised as far as it happens on
   ///    this server: the link, spent when it is opened, the provider's callback
   ///    with the state and the cookie, the code exchange and the refresh against
   ///    a local token endpoint, a newer link voiding an older flow, a sign-in as
   ///    the wrong address, a busy token endpoint retried and a refused grant
   ///    failing the job. What is NOT exercised end to end, because a Google or
   ///    Microsoft 365 migration talks only to the provider's own servers
   ///    (imap.gmail.com, outlook.office365.com, their DAV and Graph) and the
   ///    bench has no tenant: the XOAUTH2 login with the token, Google's CardDAV
   ///    and CalDAV, and Microsoft Graph. MigrationImapClientTester pins the
   ///    XOAUTH2 initial response, and MigrationSourcesTester the conversions
   ///    from Graph's JSON to a vCard and an iCalendar event, the provider table
   ///    and the URLs; Utilities.RunTestSuite runs both.
   ///
   ///    Ports: the REST listener on 9956 (or the suite's own), the web services
   ///    listener on 9957, 9958 where nothing listens (a source without DAV, and
   ///    an IMAP server that is not there), 9959 for the scripted IMAP server and
   ///    9960 for the token endpoint. Every migration a test makes is paused and
   ///    deleted in TearDown, the per-host ceiling a test changes is put back to 4,
   ///    and the import sign-in settings a test sets are put back.
   /// </summary>
   [TestFixture]
   public class Migration : TestFixtureBase
   {
      private static int RestPort = 9956;
      private const int WebServicesPort = 9957;
      private const int NoDavPort = 9958;
      private const int ScriptedPort = 9959;
      private const int TokenPort = 9960;

      private const string AdminPassword = "testar";
      private const string SourcePassword = "Source-Pass-1";
      private const string TargetPassword = "Target-Pass-1";
      private const string InternalDate = "17-Jul-1996 02:44:25 -0700";
      private const string Namespaces = "* NAMESPACE ((\"\" \"/\")) ((\"Other Users/\" \"/\")) ((\"Shared/\" \"/\"))";

      private Domain _source;
      private readonly List<string> _keyIds = new List<string>();
      private bool _perHostChanged;
      private bool _signInChanged;

      [SetUp]
      public void StartListeners()
      {
         _settings.SetAdministratorPassword(AdminPassword);
         _source = SingletonProvider<TestSetup>.Instance.AddDomain("source.test");

         IniFileSetting.Write("CalDavRedirectUrl", "");
         IniFileSetting.Write("CardDavRedirectUrl", "");
         IniFileSetting.Write("WebServicesBindAddress", "127.0.0.1");
         IniFileSetting.Write("WebServicesHttpPort", WebServicesPort.ToString());
         IniFileSetting.Write("WebServicesHttpsPort", "0");

         RestPort = RestListener.Start(RestPort);
         IniFileSetting.Write("RestApiCertificateFile", "");
         IniFileSetting.Write("RestApiPrivateKeyFile", "");

         _application.Reinitialize();

         (int status, string body) probe = Admin("GET", "/api/v1/migrations");
         Assert.AreEqual(200, probe.status, "The REST listener did not answer GET /api/v1/migrations. Body: " + probe.body);

         // A migration a failed run left behind would run into this test's accounts.
         RemoveEveryMigration();
      }

      [TearDown]
      public void StopListeners()
      {
         // Each part put back on its own, so that one failing leaves the others
         // done: a per-host ceiling of one, or a registration with a local token
         // endpoint, would outlive the fixture in the database.
         Exception first = null;
         try
         {
            RemoveEveryMigration();
         }
         catch (Exception e)
         {
            first = e;
         }

         try
         {
            foreach (string id in _keyIds)
               Admin("DELETE", "/api/v1/apikeys/" + id);
            _keyIds.Clear();

            if (_perHostChanged)
            {
               Admin("PUT", "/api/v1/settings", "{\"migration_connections_per_host\":4}");
               _perHostChanged = false;
            }

            if (_signInChanged)
            {
               ClearSignInSettings();
               _signInChanged = false;
            }
         }
         catch (Exception e)
         {
            first = first ?? e;
         }
         finally
         {
            IniFileSetting.Write("WebServicesHttpPort", "0");
            RestListener.Stop();
            _application.Reinitialize();
         }

         if (first != null)
            throw first;
      }

      // ------------------------------------------------------------ the tests

      [Test]
      [Description("Three mailboxes are copied whole - folders into their special-use counterparts, created subscribed, every message byte for byte with its flags, a keyword and its internal date, nothing on the source marked read - and a fourth with the wrong password fails with the source's refusal; the report and the summary CSV say both, and a job added to the finished migration runs it again.")]
      public void ThreeMailboxesAreCopiedAndAWrongPasswordFailsItsJob()
      {
         Source("anna");
         Source("bert");
         Source("carla");
         Source("dora");
         Target("anna");
         Target("bert");
         Target("carla");
         Target("dora");

         // anna: an INBOX with read, flagged, answered and keyword'd mail, a Sent
         // folder, and a folder two levels down. bert and carla: an INBOX each.
         Seed("anna@source.test", "INBOX", 30, "anna-inbox",
            i => i <= 10 ? "\\Seen" : (i <= 15 ? "\\Flagged \\Answered $Work" : ""));
         Seed("anna@source.test", "Sent", 5, "anna-sent", i => "\\Seen");
         CreateFolder("anna@source.test", SourcePassword, "Projects", null);
         Seed("anna@source.test", "Projects.2025", 4, "anna-2025", null);
         Seed("bert@source.test", "INBOX", 12, "bert-inbox", null);
         Seed("carla@source.test", "INBOX", 8, "carla-inbox", null);

         // anna's account here has its Sent folder already, under another name:
         // the source's Sent is filed into it, not beside it.
         CreateFolder("anna@example.test", TargetPassword, "Sent Items", null);

         List<Flagged> sourceBefore = FlagsAndDates("anna@source.test", SourcePassword, "INBOX");

         long migration = CreateMigration("Three and a wrong password", "");
         AddJobs(migration,
            "sourceuser,secret,target\r\n" +
            "anna@source.test," + SourcePassword + ",anna@example.test\r\n" +
            "bert@source.test," + SourcePassword + ",bert@example.test\r\n" +
            "carla@source.test," + SourcePassword + ",carla@example.test\r\n" +
            "dora@source.test,not-her-password,dora@example.test\r\n", 4);
         Start(migration);

         List<JsonElement> jobs = WaitForJobs(migration, all => all.All(Settled), "every job to settle");

         JsonElement dora = jobs.Single(j => Text(j, "source_user") == "dora@source.test");
         Assert.AreEqual("failed", Text(dora, "state"), Describe(dora));
         StringAssert.Contains("refused the logon", Text(dora, "last_error"),
            "A wrong password fails the job at once, with the source's own answer.");

         foreach (string user in new[] { "anna", "bert", "carla" })
         {
            JsonElement job = jobs.Single(j => Text(j, "source_user") == user + "@source.test");
            Assert.AreEqual("done", Text(job, "state"), Describe(job));
         }

         Assert.AreEqual(30, CountIn("anna@example.test", TargetPassword, "INBOX"));
         Assert.AreEqual(5, CountIn("anna@example.test", TargetPassword, "Sent Items"), "\\Sent goes to the account's own Sent folder.");
         Assert.AreEqual(-1, CountIn("anna@example.test", TargetPassword, "Sent"), "No second Sent folder is made beside the account's own.");
         Assert.AreEqual(4, CountIn("anna@example.test", TargetPassword, "Projects.2025"), "The hierarchy is kept.");
         Assert.AreEqual(12, CountIn("bert@example.test", TargetPassword, "INBOX"));
         Assert.AreEqual(8, CountIn("carla@example.test", TargetPassword, "INBOX"));
         Assert.AreEqual(0, CountIn("dora@example.test", TargetPassword, "INBOX"));
         StringAssert.Contains("Projects.2025", Subscribed("anna@example.test", TargetPassword), "A folder made by the copy is subscribed.");

         // The flags, the keyword and the internal date of every message, in
         // order, as the source holds them; and the source untouched by the read.
         List<Flagged> copied = FlagsAndDates("anna@example.test", TargetPassword, "INBOX");
         List<Flagged> sourceAfter = FlagsAndDates("anna@source.test", SourcePassword, "INBOX");
         Assert.AreEqual(30, copied.Count);
         for (int i = 0; i < 30; i++)
         {
            Assert.AreEqual(sourceBefore[i].Flags, copied[i].Flags, "The flags of message " + (i + 1));
            Assert.AreEqual(sourceBefore[i].Date, copied[i].Date, "The internal date of message " + (i + 1));
            Assert.AreEqual(sourceBefore[i].Flags, sourceAfter[i].Flags, "The source's message " + (i + 1) + " was changed by the copy.");
         }
         StringAssert.Contains("$Work", copied[10].Flags, "A keyword comes with the message.");
         StringAssert.DoesNotContain("\\Seen", copied[20].Flags, "An unread message stays unread.");
         CollectionAssert.AreEquivalent(MessageIds("anna@source.test", SourcePassword, "INBOX"), MessageIds("anna@example.test", TargetPassword, "INBOX"));

         // Byte for byte: every message's size and whole text, in order.
         Assert.AreEqual(Bodies("anna@source.test", SourcePassword, "INBOX"), Bodies("anna@example.test", TargetPassword, "INBOX"),
            "A message is not stored as the source held it.");

         // The report: per folder, what the source held and what was copied.
         JsonElement anna = jobs.Single(j => Text(j, "source_user") == "anna@source.test");
         JsonElement report = Report(migration, Number(anna, "id"));
         JsonElement sent = Folder(report, "Sent");
         Assert.AreEqual("Sent Items", Text(sent, "target"));
         Assert.AreEqual("\\Sent", Text(sent, "special_use"));
         Assert.AreEqual(5, Number(sent, "source_messages"));
         Assert.AreEqual(5, Number(sent, "copied"));
         Assert.AreEqual(30, Number(Folder(report, "INBOX"), "copied"));
         Assert.AreEqual(39, Number(report, "messages_copied"));

         // The summary: a header and a row per job, the failed one with its error.
         (int status, string body) summary = Admin("GET", "/api/v1/migrations/" + migration + "/summary.csv");
         Assert.AreEqual(200, summary.status, summary.body);
         string[] lines = summary.body.Split(new[] { "\r\n" }, StringSplitOptions.RemoveEmptyEntries);
         Assert.AreEqual(5, lines.Length, summary.body);
         StringAssert.StartsWith("job,source_user,target,state,phase", lines[0]);
         string doraLine = lines.Single(l => l.Contains("dora@source.test"));
         StringAssert.Contains(",failed,", doraLine);
         StringAssert.Contains("refused the logon", doraLine);
         Assert.AreEqual(3, lines.Count(l => l.Contains(",done,")), summary.body);
         StringAssert.DoesNotContain(SourcePassword, summary.body, "A secret is never answered.");
         StringAssert.DoesNotContain(SourcePassword, Admin("GET", "/api/v1/migrations/" + migration + "/jobs").body, "A secret is never answered.");

         // Finished, and given another mailbox: it runs again for it.
         WaitForMigration(migration, "finished");
         Source("erin");
         Target("erin");
         Seed("erin@source.test", "INBOX", 3, "erin-inbox", null);
         AddJobs(migration, Row("erin"), 1);
         Assert.AreEqual("running", Text(Get("/api/v1/migrations/" + migration), "state"), "A finished migration given a job did not run again.");
         WaitForJobs(migration, all => all.All(Settled), "the added job");
         Assert.AreEqual("done", Text(Of(Jobs(migration), "erin@source.test"), "state"));
         Assert.AreEqual(3, CountIn("erin@example.test", TargetPassword, "INBOX"));
      }

      [Test]
      [Description("A job interrupted by a restart of the servers is stopped in the middle of its folder and goes on from its resume marks: every message arrives once, and none is read twice.")]
      public void AJobInterruptedByARestartGoesOnFromItsMarks()
      {
         Source("long");
         Target("long");
         Seed("long@source.test", "INBOX", 150, "long", i => i % 3 == 0 ? "\\Seen" : "");

         // Ten messages a second: fifteen seconds for the folder, so the restart
         // lands in the middle of it.
         long migration = CreateMigration("Interrupted", ",\"concurrency\":1,\"messages_per_minute\":600");
         AddJobs(migration, "long@source.test," + SourcePassword + ",long@example.test", 1);
         Start(migration);

         // The first mark is written after fifty messages.
         JsonElement before = WaitForJobs(migration, all => Number(all[0], "messages_copied") >= 50, "the first resume mark")[0];
         Assert.AreEqual("running", Text(before, "state"), Describe(before));
         Assert.Less(Number(before, "messages_copied"), 150, "The job finished before the restart; the test proves nothing.");

         _application.Reinitialize();

         // The stop stopped it, rather than waiting for it to finish: straight
         // after the restart it is queued or running again, short of the end.
         JsonElement requeued = Jobs(migration)[0];
         CollectionAssert.Contains(new[] { "queued", "running" }, Text(requeued, "state"), Describe(requeued));
         Assert.Less(Number(requeued, "messages_copied"), 150, "The stop waited for the job to finish rather than stopping it.");

         JsonElement after = WaitForJobs(migration, all => all.All(Settled), "the job to finish after the restart")[0];
         Assert.AreEqual("done", Text(after, "state"), Describe(after));
         Assert.AreEqual(150, Number(after, "messages_copied"));

         List<string> ids = MessageIds("long@example.test", TargetPassword, "INBOX");
         Assert.AreEqual(150, ids.Count, "Every message, once.");
         Assert.AreEqual(150, ids.Distinct().Count(), "A message copied twice.");

         // The stop wrote the mark where the job was, so the second run read on
         // from it: nothing was fetched again to be found a duplicate.
         JsonElement inbox = Folder(Report(migration, Number(after, "id")), "INBOX");
         Assert.AreEqual(150, Number(inbox, "copied"));
         Assert.AreEqual(0, Reason(inbox, "duplicate"), "The second run started before the mark: the marks did not advance.");
      }

      [Test]
      [Description("What a crash leaves - messages copied past the last mark the row holds - is found again by Message-ID and not copied twice.")]
      public void CopiesPastTheMarkAreFoundAgainAfterACrash()
      {
         Source("crash");
         Target("crash");
         Seed("crash@source.test", "INBOX", 100, "crash", null);

         long migration = CreateMigration("Crashed", ",\"concurrency\":1,\"messages_per_minute\":600");
         AddJobs(migration, "crash@source.test," + SourcePassword + ",crash@example.test", 1);
         Start(migration);
         WaitForJobs(migration, all => Number(all[0], "messages_copied") >= 50, "the first resume mark");

         Pause(migration);
         JsonElement paused = WaitForJobs(migration, all => Text(all[0], "state") == "paused", "the job to pause")[0];
         long copiedBefore = CountIn("crash@example.test", TargetPassword, "INBOX");
         Assert.Less(copiedBefore, 100, "The job finished before the pause; the test proves nothing.");

         // A crash between two marks: the row names a UID twenty messages behind
         // what is already in the account.
         _application.Database.ExecuteSQL("update hm_migrationfolders set mflastuid = mflastuid - 20 where mfjobid = " + Number(paused, "id"));

         Resume(migration);
         JsonElement done = WaitForJobs(migration, all => all.All(Settled), "the job to finish")[0];
         Assert.AreEqual("done", Text(done, "state"), Describe(done));

         List<string> ids = MessageIds("crash@example.test", TargetPassword, "INBOX");
         Assert.AreEqual(100, ids.Count);
         Assert.AreEqual(100, ids.Distinct().Count(), "A message copied past the mark was copied again.");

         JsonElement inbox = Folder(Report(migration, Number(done, "id")), "INBOX");
         Assert.AreEqual(20, Reason(inbox, "duplicate"), "The twenty fetched again are found, and only those.");
         Assert.AreEqual(100, Number(inbox, "copied"));
      }

      [Test]
      [Description("A folder the source renumbered (its UIDVALIDITY changed) is taken again from its first message, and what was copied before is recognised by its Message-ID rather than copied twice.")]
      public void ARenumberedFolderIsTakenAgainWithoutCopyingAnythingTwice()
      {
         Source("renumber");
         Target("renumber");
         Seed("renumber@source.test", "INBOX", 80, "renumber", null);

         long migration = CreateMigration("Renumbered", ",\"concurrency\":1,\"messages_per_minute\":600");
         AddJobs(migration, "renumber@source.test," + SourcePassword + ",renumber@example.test", 1);
         Start(migration);
         WaitForJobs(migration, all => Number(all[0], "messages_copied") >= 50, "the first resume mark");

         Pause(migration);
         JsonElement paused = WaitForJobs(migration, all => Text(all[0], "state") == "paused", "the job to pause")[0];
         long copiedBefore = CountIn("renumber@example.test", TargetPassword, "INBOX");
         Assert.Less(copiedBefore, 80, "The job finished before the pause; the test proves nothing.");
         Assert.GreaterOrEqual(copiedBefore, 50);

         // What a source does when it rebuilds a folder: the same messages under
         // a new UIDVALIDITY. The mark now names a validity the folder no longer has.
         _application.Database.ExecuteSQL("update hm_migrationfolders set mfuidvalidity = '1' where mfjobid = " + Number(paused, "id"));

         Resume(migration);
         JsonElement done = WaitForJobs(migration, all => all.All(Settled), "the job to finish")[0];
         Assert.AreEqual("done", Text(done, "state"), Describe(done));

         List<string> ids = MessageIds("renumber@example.test", TargetPassword, "INBOX");
         Assert.AreEqual(80, ids.Count);
         Assert.AreEqual(80, ids.Distinct().Count(), "A message copied twice after the renumbering.");

         JsonElement inbox = Folder(Report(migration, Number(done, "id")), "INBOX");
         Assert.AreEqual(1, Number(inbox, "uidvalidity_restarts"));
         Assert.AreEqual(copiedBefore, Reason(inbox, "duplicate"), "Every message copied before the renumbering is found again, and only those.");
         Assert.AreEqual(80, Number(inbox, "copied"));
      }

      [Test]
      [Description("An \\All folder, taken last, brings only the mail no other folder had - after a pause as well, when the folders finished before it are done - and a \\Flagged folder is a view and is not copied; the report says both.")]
      public void AnAllFolderBringsOnlyWhatNoOtherFolderHadAfterAPause()
      {
         Source("labels");
         Target("labels");

         // The INBOX, a folder of its own, a starred view of two INBOX messages,
         // and All: everything the other two hold and three archived under no
         // folder - Gmail's shape, on this server.
         Seed("labels@source.test", "INBOX", 10, "inbox", null);
         Seed("labels@source.test", "Work", 5, "work", null);
         CreateFolder("labels@source.test", SourcePassword, "Starred", "\\Flagged");
         Seed("labels@source.test", "Starred", 2, "inbox", null, 1, false);
         CreateFolder("labels@source.test", SourcePassword, "All", "\\All");
         Seed("labels@source.test", "All", 10, "inbox", null, 1, false);
         Seed("labels@source.test", "All", 5, "work", null, 1, false);
         Seed("labels@source.test", "All", 3, "archived", null, 1, false);

         // Two messages a second: the INBOX takes five seconds, and the pause
         // lands once it is done and before All is reached.
         long migration = CreateMigration("Labels", ",\"concurrency\":1,\"messages_per_minute\":120");
         AddJobs(migration, "labels@source.test," + SourcePassword + ",labels@example.test", 1);
         Start(migration);
         WaitForJobs(migration, all => Number(all[0], "folders_done") >= 1, "the INBOX to be done");

         Pause(migration);
         JsonElement paused = WaitForJobs(migration, all => Text(all[0], "state") == "paused", "the job to pause")[0];
         Assert.Less(Number(paused, "folders_done"), 3, "The job reached All before the pause; the test proves nothing.");

         Resume(migration);
         JsonElement done = WaitForJobs(migration, all => all.All(Settled), "the job to finish")[0];
         Assert.AreEqual("done", Text(done, "state"), Describe(done));

         Assert.AreEqual(10, CountIn("labels@example.test", TargetPassword, "INBOX"));
         Assert.AreEqual(5, CountIn("labels@example.test", TargetPassword, "Work"));
         Assert.AreEqual(3, CountIn("labels@example.test", TargetPassword, "All"),
            "All brought again what a folder finished before the pause holds: its keys were not read back.");
         Assert.AreEqual(-1, CountIn("labels@example.test", TargetPassword, "Starred"), "A \\Flagged view is not a folder of its own here.");

         var everything = new List<string>();
         foreach (string folder in new[] { "INBOX", "Work", "All" })
            everything.AddRange(MessageIds("labels@example.test", TargetPassword, folder));
         Assert.AreEqual(18, everything.Count);
         Assert.AreEqual(18, everything.Distinct().Count(), "A message arrived twice.");

         JsonElement report = Report(migration, Number(done, "id"));
         JsonElement all = Folder(report, "All");
         Assert.AreEqual(15, Reason(all, "another_folder"));
         StringAssert.Contains("copied once", Text(all, "note"), "The report says why All brought three of eighteen.");
         StringAssert.Contains("view of messages", Text(Folder(report, "Starred"), "note"));
      }

      [Test]
      [Description("Pause stops a running job and nothing more arrives until Resume; a migration with a job running is not deleted; Cancel stops one and keeps what it copied, and a retry of it goes on from its marks; Retry with the password mended runs a job that failed.")]
      public void PauseResumeCancelAndRetry()
      {
         Source("slow");
         Source("stopped");
         Source("mended");
         Target("slow");
         Target("stopped");
         Target("mended");
         Seed("slow@source.test", "INBOX", 150, "slow", null);
         Seed("stopped@source.test", "INBOX", 80, "stopped", null);
         Seed("mended@source.test", "INBOX", 5, "mended", null);

         long migration = CreateMigration("Stop and go", ",\"concurrency\":3,\"messages_per_minute\":600");
         AddJobs(migration,
            "slow@source.test," + SourcePassword + ",slow@example.test\r\n" +
            "stopped@source.test," + SourcePassword + ",stopped@example.test\r\n" +
            "mended@source.test,the-old-password,mended@example.test\r\n", 3);
         Start(migration);

         // Cancel: the job stops, and what it copied stays.
         JsonElement stopped = WaitForJobs(migration, all => Number(Of(all, "stopped@source.test"), "messages_copied") >= 50, "stopped's first mark")
            .Single(j => Text(j, "source_user") == "stopped@source.test");
         (int status, string body) cancel = Admin("POST", "/api/v1/migrations/" + migration + "/jobs/" + Number(stopped, "id") + "/cancel", "{}");
         Assert.AreEqual(200, cancel.status, cancel.body);
         WaitForJobs(migration, all => Text(Of(all, "stopped@source.test"), "state") == "cancelled", "the cancel");
         long kept = CountIn("stopped@example.test", TargetPassword, "INBOX");
         Assert.GreaterOrEqual(kept, 50, "What the cancelled job copied stays.");
         Thread.Sleep(1500);
         Assert.AreEqual(kept, CountIn("stopped@example.test", TargetPassword, "INBOX"), "A cancelled job copied on.");

         // A migration with a job running is not deleted.
         (int status, string body) refusedDelete = Admin("DELETE", "/api/v1/migrations/" + migration);
         Assert.AreEqual(409, refusedDelete.status, refusedDelete.body);
         StringAssert.Contains("running", refusedDelete.body);

         // Pause: nothing arrives while it is paused.
         Pause(migration);
         WaitForJobs(migration, all => Text(Of(all, "slow@source.test"), "state") == "paused", "the pause");
         long atPause = CountIn("slow@example.test", TargetPassword, "INBOX");
         Assert.Less(atPause, 150, "The job finished before the pause; the test proves nothing.");
         Thread.Sleep(2000);
         Assert.AreEqual(atPause, CountIn("slow@example.test", TargetPassword, "INBOX"), "A paused job copied on.");
         Assert.AreEqual("paused", Text(Get("/api/v1/migrations/" + migration), "state"));

         // Resume: it goes on from its mark to the end, nothing twice.
         Resume(migration);
         WaitForJobs(migration, all => Settled(Of(all, "slow@source.test")), "slow to finish");
         Assert.AreEqual("done", Text(Of(Jobs(migration), "slow@source.test"), "state"));
         List<string> ids = MessageIds("slow@example.test", TargetPassword, "INBOX");
         Assert.AreEqual(150, ids.Count);
         Assert.AreEqual(150, ids.Distinct().Count());

         // Retry: the wrong password failed its job at once; mended, it runs.
         JsonElement mended = Of(Jobs(migration), "mended@source.test");
         Assert.AreEqual("failed", Text(mended, "state"), Describe(mended));
         Assert.AreEqual(0, CountIn("mended@example.test", TargetPassword, "INBOX"));
         (int status, string body) retry = Admin("POST", "/api/v1/migrations/" + migration + "/jobs/" + Number(mended, "id") + "/retry",
            "{\"secret\":\"" + SourcePassword + "\"}");
         Assert.AreEqual(200, retry.status, retry.body);
         // Queued, or already started by the scheduler between the retry and its answer.
         CollectionAssert.Contains(new[] { "queued", "running" }, Text(Json(retry.body), "state"));
         WaitForJobs(migration, all => Settled(Of(all, "mended@source.test")), "the retried job to finish");
         Assert.AreEqual("done", Text(Of(Jobs(migration), "mended@source.test"), "state"));
         Assert.AreEqual(5, CountIn("mended@example.test", TargetPassword, "INBOX"));

         // A cancelled job stays cancelled through all of that, until it is
         // retried: then it goes on from its marks, nothing twice.
         Assert.AreEqual("cancelled", Text(Of(Jobs(migration), "stopped@source.test"), "state"));
         (int status, string body) again = Admin("POST", "/api/v1/migrations/" + migration + "/jobs/" + Number(stopped, "id") + "/retry", "{}");
         Assert.AreEqual(200, again.status, again.body);
         WaitForJobs(migration, all => Settled(Of(all, "stopped@source.test")), "the cancelled job, retried");
         Assert.AreEqual("done", Text(Of(Jobs(migration), "stopped@source.test"), "state"));
         List<string> stoppedIds = MessageIds("stopped@example.test", TargetPassword, "INBOX");
         Assert.AreEqual(80, stoppedIds.Count);
         Assert.AreEqual(80, stoppedIds.Distinct().Count(), "The retried job copied again what it had copied before it was cancelled.");

         // Resume is for a paused migration, and this one is not.
         Assert.AreEqual(409, Admin("POST", "/api/v1/migrations/" + migration + "/resume", "{}").status, "Resume of a migration that is not paused.");
      }

      [Test]
      [Description("A source is tried again for what time mends - a logon it cannot take just now, a folder busy - and leaves what it never will: a folder it says does not exist, a message it will not give, a message gone, other people's mailboxes. The job goes on, and the report and its errors say what was left.")]
      public void AScriptedSourceIsRetriedForWhatTimeMendsAndLeavesWhatItNeverWill()
      {
         Target("scripted");

         var inbox = new ScriptedImapServer.RemoteMailbox("INBOX");
         for (int uid = 101; uid <= 104; uid++)
            inbox.Add(new ScriptedImapServer.RemoteMessage(uid, ScriptedText("inbox-" + uid)));
         var gone = new ScriptedImapServer.RemoteMailbox("Gone");
         gone.Add(new ScriptedImapServer.RemoteMessage(201, ScriptedText("gone-201")));
         var busy = new ScriptedImapServer.RemoteMailbox("Busy");
         busy.Add(new ScriptedImapServer.RemoteMessage(301, ScriptedText("busy-301")));
         busy.Add(new ScriptedImapServer.RemoteMessage(302, ScriptedText("busy-302")));
         var others = new ScriptedImapServer.RemoteMailbox("Other Users/bob/INBOX");
         others.Add(new ScriptedImapServer.RemoteMessage(401, ScriptedText("bob-401")));
         var shared = new ScriptedImapServer.RemoteMailbox("Shared/Team");
         shared.Add(new ScriptedImapServer.RemoteMessage(501, ScriptedText("team-501")));
         var mailboxes = new List<ScriptedImapServer.RemoteMailbox> { inbox, gone, busy, others, shared };

         long migration = CreateMigration("Scripted", ",\"concurrency\":1", NoDavPort, ScriptedPort);
         AddJobs(migration, "someone@scripted.test,far-side-secret,scripted@example.test", 1);

         // 1. The source cannot log anybody on just now: a retry, not a failure.
         var first = new ScriptedImapServer(ScriptedPort, mailboxes) { LoginAnswer = "NO [UNAVAILABLE] Temporary authentication failure" };
         first.StartListen();
         Start(migration);
         JsonElement job = WaitForJobs(migration, all => Text(all[0], "state") == "paused", "the job to pause after the logon it could not have")[0];
         first.WaitForCompletion();
         Assert.AreEqual(1, Number(job, "attempts"), Describe(job));
         StringAssert.Contains("could not log the job on just now", Text(job, "last_error"));
         StringAssert.Contains("UNAVAILABLE", Text(job, "last_error"));
         Assert.AreEqual(1, Report(migration, Number(job, "id")).GetProperty("errors").GetArrayLength());

         // 2. Logged on: Gone refused for good, Busy for now, one message
         // refused, one vanished, and the other users' and shared mailboxes left.
         var second = new ScriptedImapServer(ScriptedPort, mailboxes) { NamespaceLine = Namespaces };
         second.ExamineAnswers["Gone"] = "NO [NONEXISTENT] Unknown Mailbox";
         second.ExamineAnswers["Busy"] = "NO [UNAVAILABLE] Mailbox is busy";
         second.RefuseFetchOf.Add(102);
         second.VanishedUids.Add(103);
         second.StartListen();
         Retry(migration, Number(job, "id"), null);
         job = WaitForJobs(migration, all => Text(all[0], "state") == "paused", "the job to pause at the busy folder")[0];
         second.WaitForCompletion();
         StringAssert.Contains("Mailbox is busy", Text(job, "last_error"));
         CollectionAssert.AreEqual(new[] { "INBOX", "Gone", "Busy" }, second.ExaminedMailboxes, "Other people's mailboxes are not opened.");
         Assert.AreEqual(3, second.FetchedUids.Count(uid => uid == 102), "A refused message is asked for three times.");
         Assert.AreEqual(2, CountIn("scripted@example.test", TargetPassword, "INBOX"));

         JsonElement report = Report(migration, Number(job, "id"));
         JsonElement inboxTally = Folder(report, "INBOX");
         Assert.AreEqual(2, Number(inboxTally, "copied"));
         Assert.AreEqual(1, Reason(inboxTally, "refused"));
         Assert.AreEqual(1, Reason(inboxTally, "gone"));
         StringAssert.Contains("NONEXISTENT", Text(Folder(report, "Gone"), "note"));
         StringAssert.Contains("another user's", Text(Folder(report, "Other Users/bob/INBOX"), "note"));
         StringAssert.Contains("another user's", Text(Folder(report, "Shared/Team"), "note"));
         string errors = report.GetProperty("errors").ToString();
         StringAssert.Contains("Gone", errors, "The folder left uncopied is among the errors, not only in a note.");
         StringAssert.Contains("UID 102", errors, "The message left uncopied is among the errors.");

         // 3. Busy opens now; what was done is not opened again.
         var third = new ScriptedImapServer(ScriptedPort, mailboxes) { NamespaceLine = Namespaces };
         third.StartListen();
         Retry(migration, Number(job, "id"), null);
         job = WaitForJobs(migration, all => all.All(Settled), "the job to finish")[0];
         third.WaitForCompletion();
         Assert.AreEqual("done", Text(job, "state"), Describe(job));
         CollectionAssert.AreEqual(new[] { "Busy" }, third.ExaminedMailboxes, "A folder finished, or left for good, is not opened again.");
         Assert.AreEqual(2, CountIn("scripted@example.test", TargetPassword, "Busy"));
         Assert.AreEqual(0, CountIn("scripted@example.test", TargetPassword, "Gone"), "Something was copied from a folder the source would not open.");
      }

      [Test]
      [Description("A source nothing answers at is tried again after a minute, the error kept in the report, and after twenty-four attempts in a row the job fails, saying so.")]
      public void ASourceNothingAnswersAtIsTriedAgainAndFailsAfterTwentyFourAttempts()
      {
         Target("unreached");
         long migration = CreateMigration("Nobody there", ",\"concurrency\":1", NoDavPort, NoDavPort);
         AddJobs(migration, "unreached@far.test,a-password,unreached@example.test", 1);
         Start(migration);

         JsonElement job = WaitForJobs(migration, all => Text(all[0], "state") == "paused", "the first failure")[0];
         Assert.AreEqual(1, Number(job, "attempts"), Describe(job));
         long wait = Number(job, "next_attempt") - DateTimeOffset.UtcNow.ToUnixTimeSeconds();
         Assert.GreaterOrEqual(wait, 40L, "The first retry is a minute away.");
         Assert.LessOrEqual(wait, 61L, "The first retry is a minute away.");
         Assert.IsFalse(string.IsNullOrEmpty(Text(job, "last_error")));
         JsonElement errors = Report(migration, Number(job, "id")).GetProperty("errors");
         Assert.AreEqual(1, errors.GetArrayLength());
         Assert.AreEqual(Text(job, "last_error"), Text(errors[0], "error"));

         // The twenty-fourth attempt, due now.
         _application.Database.ExecuteSQL("update hm_migrationjobs set mjattempts = 23, mjnextattempt = 1 where mjid = " + Number(job, "id"));
         job = WaitForJobs(migration, all => Text(all[0], "state") == "failed", "the twenty-fourth attempt")[0];
         Assert.AreEqual(24, Number(job, "attempts"));
         StringAssert.StartsWith("Gave up after 24 attempts", Text(job, "last_error"));
      }

      [Test]
      [Description("A message over the target's own size limit is skipped and counted, and the job goes on.")]
      public void AMessageOverTheTargetsLimitIsSkipped()
      {
         Source("big");
         Target("big");
         Seed("big@source.test", "INBOX", 1, "small", null);
         Seed("big@source.test", "INBOX", 1, "large", null, 1, false, 6000);

         // example.test takes two kilobytes a message.
         _domain.MaxMessageSize = 2;
         _domain.Save();

         long migration = CreateMigration("Too large", "");
         AddJobs(migration, Row("big"), 1);
         Start(migration);
         JsonElement job = WaitForJobs(migration, all => all.All(Settled), "the job")[0];
         Assert.AreEqual("done", Text(job, "state"), Describe(job));
         Assert.AreEqual(1, CountIn("big@example.test", TargetPassword, "INBOX"));
         Assert.AreEqual(1, Reason(Folder(Report(migration, Number(job, "id")), "INBOX"), "too_large"));
      }

      [Test]
      [Description("An account whose quota fills pauses its job with the reason rather than skipping mail, and the job goes on once the quota is raised.")]
      public void AFullQuotaPausesTheJobUntilItIsRaised()
      {
         Source("full");
         Account target = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "full@example.test", TargetPassword, 1);
         Seed("full@source.test", "INBOX", 4, "full", null, 1, true, 400 * 1024);

         long migration = CreateMigration("Over quota", "");
         AddJobs(migration, Row("full"), 1);
         Start(migration);
         JsonElement job = WaitForJobs(migration, all => Text(all[0], "state") == "paused", "the job to pause at the quota")[0];
         StringAssert.Contains("quota", Text(job, "last_error"));
         int atQuota = CountIn("full@example.test", TargetPassword, "INBOX");
         Assert.Less(atQuota, 4);

         target.MaxSize = 20;
         target.Save();
         Retry(migration, Number(job, "id"), null);
         job = WaitForJobs(migration, all => all.All(Settled), "the job to finish")[0];
         Assert.AreEqual("done", Text(job, "state"), Describe(job));
         Assert.AreEqual(4, CountIn("full@example.test", TargetPassword, "INBOX"));
         Assert.AreEqual(4, MessageIds("full@example.test", TargetPassword, "INBOX").Distinct().Count());
      }

      [Test]
      [Description("A migration runs no more jobs at once than its concurrency, and every migration from one host together no more than MigrationConnectionsPerHost: counted as running jobs and as IMAP sessions at the source.")]
      public void ConcurrencyAndTheCeilingPerHostHoldTheSessions()
      {
         // The setting is checked as the others are.
         (int status, string body) refused = Admin("PUT", "/api/v1/settings", "{\"migration_connections_per_host\":0}");
         Assert.AreEqual(400, refused.status, refused.body);
         StringAssert.Contains("MigrationConnectionsPerHost must be between 1 and 64", refused.body);

         var sources = new[] { "one", "two", "three", "four", "five", "six", "seven" };
         foreach (string name in sources)
         {
            Source(name);
            Target(name);
            Seed(name + "@source.test", "INBOX", 10, name, null);
         }
         CustomAsserts.AssertSessionCount(eSessionType.eSTIMAP, 0);

         // One migration, concurrency 1: three jobs, one after another.
         long single = CreateMigration("One at a time", ",\"concurrency\":1,\"messages_per_minute\":300");
         AddJobs(single, string.Join("\r\n", sources.Take(3).Select(Row)), 3);
         Start(single);
         (int maxRunning, int maxSessions) first = Watch(new[] { single });
         Assert.AreEqual(1, first.maxRunning, "Concurrency 1 ran another number of jobs at once.");
         Assert.AreEqual(1, first.maxSessions, "Concurrency 1 held another number of IMAP sessions at the source (0 means the sampling saw none).");
         Assert.IsTrue(Jobs(single).All(j => Text(j, "state") == "done"));

         // Two migrations from the same host, each allowed four, under a ceiling of one.
         (int status, string body) perHost = Admin("PUT", "/api/v1/settings", "{\"migration_connections_per_host\":1}");
         Assert.AreEqual(200, perHost.status, perHost.body);
         _perHostChanged = true;
         StringAssert.Contains("\"migration_connections_per_host\":1", Admin("GET", "/api/v1/settings").body);

         CustomAsserts.AssertSessionCount(eSessionType.eSTIMAP, 0);
         long left = CreateMigration("Left", ",\"concurrency\":4,\"messages_per_minute\":300");
         long right = CreateMigration("Right", ",\"concurrency\":4,\"messages_per_minute\":300");
         AddJobs(left, string.Join("\r\n", sources.Skip(3).Take(2).Select(Row)), 2);
         AddJobs(right, string.Join("\r\n", sources.Skip(5).Take(2).Select(Row)), 2);
         Start(left);
         Start(right);
         (int maxRunning, int maxSessions) shared = Watch(new[] { left, right });
         Assert.AreEqual(1, shared.maxRunning, "Two migrations from one host ran another number of jobs at once than the ceiling.");
         Assert.AreEqual(1, shared.maxSessions, "Two migrations from one host held another number of IMAP sessions than the ceiling.");
         Assert.IsTrue(Jobs(left).Concat(Jobs(right)).All(j => Text(j, "state") == "done"));

         foreach (string name in sources)
            Assert.AreEqual(10, CountIn(name + "@example.test", TargetPassword, "INBOX"), name);
      }

      [Test]
      [Description("The contacts and the events come with the mail, from the source's own CardDAV and CalDAV, a contact the account holds already counted rather than copied twice; a source without DAV has the phase skipped with its reason, and the job is done.")]
      public void ContactsAndCalendarsComeFromTheSourcesDavAndASourceWithoutItSkipsThePhase()
      {
         Source("dav");
         Target("dav");
         Source("nodav");
         Target("nodav");
         Seed("dav@source.test", "INBOX", 3, "dav", null);
         Seed("nodav@source.test", "INBOX", 3, "nodav", null);

         (int status, string body) erin = AsAccount("dav@source.test", SourcePassword, "POST", "/api/v1/me/contacts",
            "{\"name\":\"Erin Example\",\"address\":\"erin@elsewhere.test\"}");
         Assert.AreEqual(201, erin.status, erin.body);
         (int status, string body) frank = AsAccount("dav@source.test", SourcePassword, "POST", "/api/v1/me/contacts",
            "{\"name\":\"Frank Example\",\"address\":\"frank@elsewhere.test\"}");
         Assert.AreEqual(201, frank.status, frank.body);

         // The target knows Erin already: one address once, the address book's rule.
         (int status, string body) known = AsAccount("dav@example.test", TargetPassword, "POST", "/api/v1/me/contacts",
            "{\"name\":\"Erin\",\"address\":\"erin@elsewhere.test\"}");
         Assert.AreEqual(201, known.status, known.body);

         string ics = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//Migration test//EN\r\n" +
                      "BEGIN:VEVENT\r\nUID:migration-event-0001\r\nDTSTAMP:20260101T000000Z\r\n" +
                      "DTSTART:20260710T090000Z\r\nDTEND:20260710T100000Z\r\nSUMMARY:Migration planning\r\n" +
                      "END:VEVENT\r\nEND:VCALENDAR\r\n";
         (int status, string body) put = Http(WebServicesPort, "PUT", "/dav/calendars/dav@source.test/calendar/migration-event-0001.ics",
            Basic("dav@source.test", SourcePassword), ics, "text/calendar; charset=utf-8", "X-Forwarded-Proto: https\r\n");
         Assert.AreEqual(201, put.status, "The source's event could not be written. Body: " + put.body);

         long withDav = CreateMigration("With DAV", "");
         AddJobs(withDav, "dav@source.test," + SourcePassword + ",dav@example.test", 1);
         long withoutDav = CreateMigration("Without DAV", "", NoDavPort);
         AddJobs(withoutDav, "nodav@source.test," + SourcePassword + ",nodav@example.test", 1);
         Start(withDav);
         Start(withoutDav);

         JsonElement davJob = WaitForJobs(withDav, all => all.All(Settled), "the DAV job")[0];
         JsonElement noDavJob = WaitForJobs(withoutDav, all => all.All(Settled), "the job without DAV")[0];
         Assert.AreEqual("done", Text(davJob, "state"), Describe(davJob));
         Assert.AreEqual("done", Text(noDavJob, "state"), "A phase that cannot run never fails the job. " + Describe(noDavJob));

         // What the target account now holds, read as its owner reads it.
         (int status, string body) contacts = AsAccount("dav@example.test", TargetPassword, "GET", "/api/v1/me/contacts", null);
         Assert.AreEqual(200, contacts.status, contacts.body);
         StringAssert.Contains("erin@elsewhere.test", contacts.body);
         StringAssert.Contains("frank@elsewhere.test", contacts.body);
         (int status, string body) events = AsAccount("dav@example.test", TargetPassword, "GET",
            "/api/v1/me/calendar/events?start=20260701T000000Z&end=20260801T000000Z", null);
         Assert.AreEqual(200, events.status, events.body);
         StringAssert.Contains("Migration planning", events.body);
         Assert.AreEqual(3, CountIn("dav@example.test", TargetPassword, "INBOX"));

         JsonElement davReport = Report(withDav, Number(davJob, "id"));
         JsonElement contactsTally = davReport.GetProperty("contacts");
         Assert.AreEqual("copied", Text(contactsTally, "state"), davReport.ToString());
         Assert.AreEqual(1, Number(contactsTally, "copied"));
         Assert.AreEqual(1, Reason(contactsTally, "already_there"));
         Assert.AreEqual("copied", Text(davReport.GetProperty("calendars"), "state"), davReport.ToString());
         Assert.AreEqual(1, Number(davReport.GetProperty("calendars"), "copied"));

         // The source without DAV: the mail copied, both phases skipped, and why.
         Assert.AreEqual(3, CountIn("nodav@example.test", TargetPassword, "INBOX"));
         JsonElement noDavReport = Report(withoutDav, Number(noDavJob, "id"));
         foreach (string phase in new[] { "contacts", "calendars" })
         {
            JsonElement tally = noDavReport.GetProperty(phase);
            Assert.AreEqual("skipped", Text(tally, "state"), noDavReport.ToString());
            StringAssert.Contains("could not be reached", Text(tally, "reason"));
         }
         (int status, string body) none = AsAccount("nodav@example.test", TargetPassword, "GET", "/api/v1/me/contacts", null);
         StringAssert.DoesNotContain("elsewhere.test", none.body);
      }

      [Test]
      [Description("Jobs are added from CSV - a heading skipped, a byte order mark ignored, RFC 4180 quoting - and a request with one line wrong is refused whole, naming the line, with nothing added; a mailbox another migration is copying there now is refused, and a password in the target's place is not repeated back.")]
      public void JobsAreAddedFromCsvAndAWrongLineRefusesTheWholeRequestNamingIt()
      {
         Target("anna");
         Target("bert");
         long migration = CreateMigration("From a spreadsheet", "");

         JsonElement added = AddJobs(migration,
            "﻿sourceuser,secret,target\n" +
            "anna@source.test,first-secret,anna@example.test\n" +
            "\"bert, the second\",\"with \"\"quotes\"\", and a comma\",bert@example.test\n", 2);
         Assert.IsFalse(added.GetProperty("jobs").EnumerateArray().Any(j => j.TryGetProperty("signin_url", out _)),
            "An IMAP source's job has no sign-in link.");
         List<JsonElement> jobs = Jobs(migration);
         CollectionAssert.AreEquivalent(new[] { "anna@source.test", "bert, the second" }, jobs.Select(j => Text(j, "source_user")),
            "A byte order mark became part of the heading, or of the first source user.");
         Assert.IsTrue(jobs.All(j => Text(j, "state") == "queued"));

         AssertRefusedLine(migration, "carla@source.test,pw,anna@example.test\ndora@source.test,pw\n", 2,
            "a row is sourceuser,secret,target - three fields - and this one has 2");
         AssertRefusedLine(migration, "carla@source.test,pw,anna@example.test\r\n\r\nerin@source.test,pw,nobody@example.test\r\n", 3,
            "the target nobody@example.test is not an account of this server");
         AssertRefusedLine(migration, "\"carla@source.test,pw,anna@example.test", 1, "a quoted field that is not closed");
         AssertRefusedLine(migration, "carla@source.test,,anna@example.test", 1, "needs the source password as the secret");
         AssertRefusedLine(migration, "anna@source.test,again,anna@example.test", 1, "already a job of this migration");

         // The columns the wrong way round: the password is in the target's place.
         (int status, string body) swapped = Admin("POST", "/api/v1/migrations/" + migration + "/jobs", "carla@source.test,anna@example.test,Swapped-Secret-9");
         Assert.AreEqual(400, swapped.status, swapped.body);
         StringAssert.Contains("line 1: the third field, the target, is not an address", swapped.body);
         StringAssert.DoesNotContain("Swapped-Secret-9", swapped.body, "A password was repeated back in the refusal.");

         Assert.AreEqual(2, Jobs(migration).Count, "A refused request added a job.");

         // Another migration of the same mailbox into the same account, while the
         // first has it queued: refused, since the two would each copy it.
         long twin = CreateMigration("The same mailbox again", "");
         (int status, string body) twice = Admin("POST", "/api/v1/migrations/" + twin + "/jobs", "anna@source.test,first-secret,anna@example.test");
         Assert.AreEqual(409, twice.status, twice.body);
         StringAssert.Contains("line 1: that mailbox is being copied into that account already", twice.body);
         Assert.AreEqual(0, Jobs(twin).Count);

         // One job as JSON is the same thing.
         (int status, string body) one = Admin("POST", "/api/v1/migrations/" + migration + "/jobs",
            "{\"source_user\":\"carla@source.test\",\"secret\":\"third-secret\",\"target\":\"anna@example.test\"}");
         Assert.AreEqual(201, one.status, one.body);
         Assert.AreEqual(3, Jobs(migration).Count);

         string listing = Admin("GET", "/api/v1/migrations/" + migration + "/jobs").body;
         foreach (string secret in new[] { "first-secret", "and a comma", "third-secret" })
            StringAssert.DoesNotContain(secret, listing, "A secret is never answered.");
      }

      [Test]
      [Description("Reading migrations needs view_accounts and changing them manage_accounts: a read-only key and an auditor read and are refused every write; a key confined to a domain cannot create a migration, does not see one with no jobs, sees only the migrations into its domain and is refused a job into another.")]
      public void AReadOnlyKeyAndAnAuditorAreRefusedAndAConfinedKeyStaysInItsDomain()
      {
         Target("anna");
         Domain other = SingletonProvider<TestSetup>.Instance.AddDomain("other.test");
         SingletonProvider<TestSetup>.Instance.AddAccount(other, "olga@other.test", TargetPassword);

         long ours = CreateMigration("Into example.test", "");
         long job = Number(AddJobs(ours, "anna@source.test,pw,anna@example.test", 1).GetProperty("jobs")[0], "id");
         long theirs = CreateMigration("Into other.test", "");
         AddJobs(theirs, "olga@source.test,pw,olga@other.test", 1);
         long empty = CreateMigration("Nobody's yet", "");

         string readOnly = CreateKey("migration readonly", "{\"label\":\"migration readonly\",\"scope\":\"readonly\"}");
         string auditor = CreateKey("migration auditor", "{\"label\":\"migration auditor\",\"role\":\"auditor\"}");
         string confined = CreateKey("migration confined", "{\"label\":\"migration confined\",\"scope\":\"full\",\"domains\":\"example.test\"}");

         string jobPath = "/api/v1/migrations/" + ours + "/jobs/" + job;
         foreach (string key in new[] { readOnly, auditor })
         {
            Assert.AreEqual(200, WithKey(key, "GET", "/api/v1/migrations").status);
            Assert.AreEqual(200, WithKey(key, "GET", "/api/v1/migrations/" + ours + "/jobs").status);
            Assert.AreEqual(200, WithKey(key, "GET", jobPath + "/report").status);
            Assert.AreEqual(403, WithKey(key, "POST", "/api/v1/migrations", "{\"name\":\"not allowed\",\"host\":\"imap.x.test\"}").status);
            Assert.AreEqual(403, WithKey(key, "POST", "/api/v1/migrations/" + ours + "/jobs", "bert@source.test,pw,anna@example.test").status);
            Assert.AreEqual(403, WithKey(key, "POST", "/api/v1/migrations/" + ours + "/start", "{}").status);
            Assert.AreEqual(403, WithKey(key, "POST", "/api/v1/migrations/" + ours + "/pause", "{}").status);
            Assert.AreEqual(403, WithKey(key, "POST", "/api/v1/migrations/" + ours + "/resume", "{}").status);
            Assert.AreEqual(403, WithKey(key, "POST", jobPath + "/retry", "{}").status);
            Assert.AreEqual(403, WithKey(key, "POST", jobPath + "/cancel", "{}").status);
            Assert.AreEqual(403, WithKey(key, "POST", jobPath + "/signin", "{}").status);
            Assert.AreEqual(403, WithKey(key, "DELETE", "/api/v1/migrations/" + ours).status);
         }
         Assert.AreEqual(1, Jobs(ours).Count, "A refused key added a job.");
         Assert.AreEqual("new", Text(Get("/api/v1/migrations/" + ours), "state"), "A refused key started the migration.");
         Assert.AreEqual("queued", Text(Jobs(ours)[0], "state"), "A refused key cancelled the job.");

         (int status, string body) created = WithKey(confined, "POST", "/api/v1/migrations", "{\"name\":\"Mine\",\"host\":\"imap.x.test\"}");
         Assert.AreEqual(403, created.status, created.body);
         StringAssert.Contains("confined to named domains cannot create a migration", created.body);

         (int status, string body) seen = WithKey(confined, "GET", "/api/v1/migrations");
         Assert.AreEqual(200, seen.status, seen.body);
         List<long> ids = Json(seen.body).GetProperty("migrations").EnumerateArray().Select(m => Number(m, "id")).ToList();
         CollectionAssert.Contains(ids, ours);
         CollectionAssert.DoesNotContain(ids, theirs, "A key confined to example.test saw a migration into other.test.");
         CollectionAssert.DoesNotContain(ids, empty, "A key confined to a domain saw a migration with no jobs, which is no domain's.");
         Assert.AreEqual(404, WithKey(confined, "GET", "/api/v1/migrations/" + theirs).status);
         Assert.AreEqual(404, WithKey(confined, "POST", "/api/v1/migrations/" + empty + "/jobs", "bert@source.test,pw,anna@example.test").status);

         (int status, string body) outside = WithKey(confined, "POST", "/api/v1/migrations/" + ours + "/jobs",
            "bert@source.test,pw,anna@example.test\r\nolga2@source.test,pw,olga@other.test\r\n");
         Assert.AreEqual(403, outside.status, outside.body);
         StringAssert.Contains("line 2", outside.body);
         Assert.AreEqual(1, Jobs(ours).Count, "The refused request added the line before the one refused.");

         (int status, string body) inside = WithKey(confined, "POST", "/api/v1/migrations/" + ours + "/jobs", "bert@source.test,pw,anna@example.test");
         Assert.AreEqual(201, inside.status, inside.body);
      }

      [Test]
      [Description("The audit trail names who created a migration, added a job - by its mailbox and target, the secret withheld - cancelled it while it ran, and deleted the migration.")]
      public void TheAuditTrailNamesWhoMadeAddedCancelledAndDeleted()
      {
         Source("audited");
         Target("audited");
         Seed("audited@source.test", "INBOX", 60, "audited", null);

         long migration = CreateMigration("Audited migration", ",\"concurrency\":1,\"messages_per_minute\":300");
         long job = Number(AddJobs(migration, "audited@source.test," + SourcePassword + ",audited@example.test", 1).GetProperty("jobs")[0], "id");
         Start(migration);
         WaitForJobs(migration, all => Text(all[0], "state") == "running", "the job to run");

         (int status, string body) cancel = Admin("POST", "/api/v1/migrations/" + migration + "/jobs/" + job + "/cancel", "{}");
         Assert.AreEqual(200, cancel.status, cancel.body);
         WaitForJobs(migration, all => Text(all[0], "state") == "cancelled", "the cancel");
         WaitForMigration(migration, "finished");
         Assert.AreEqual(200, Admin("DELETE", "/api/v1/migrations/" + migration).status);

         List<JsonElement> trail = Json(Admin("GET", "/api/v1/audit?limit=500").body).GetProperty("entries").EnumerateArray().ToList();
         const string jobName = "audited@source.test -> audited@example.test";

         JsonElement made = trail.First(e => Text(e, "object_type") == "migration" && Text(e, "object") == "Audited migration" && Text(e, "action") == "created");
         Assert.AreEqual("REST", Text(made, "interface"));
         StringAssert.Contains("administrator", Text(made, "actor").ToLowerInvariant());

         JsonElement added = trail.First(e => Text(e, "object_type") == "migration job" && Text(e, "object") == jobName && Text(e, "action") == "created");
         StringAssert.Contains("(changed)", Text(added, "detail"), "The secret is recorded as changed.");
         StringAssert.DoesNotContain(SourcePassword, Text(added, "detail"), "The secret reached the trail.");

         JsonElement cancelled = trail.First(e => Text(e, "object_type") == "migration job" && Text(e, "object") == jobName && Text(e, "action") == "cancelled");
         Assert.AreEqual("REST", Text(cancelled, "interface"));
         StringAssert.Contains("while it was running", Text(cancelled, "detail"));

         Assert.IsTrue(trail.Any(e => Text(e, "object_type") == "migration" && Text(e, "action") == "deleted"), "The delete is not in the trail.");
      }

      [Test]
      [Description("Deleting the account a job copies into cancels its unfinished jobs, with the reason, and leaves the others.")]
      public void DeletingTheTargetAccountCancelsItsUnfinishedJobs()
      {
         Account leaving = Target("leaving");
         Target("staying");
         long migration = CreateMigration("Into an account that goes", "");
         AddJobs(migration, "leaving@source.test,pw,leaving@example.test\r\nstaying@source.test,pw,staying@example.test", 2);

         _domain.Accounts.DeleteByDBID(leaving.ID);

         List<JsonElement> jobs = Jobs(migration);
         JsonElement gone = Of(jobs, "leaving@source.test");
         Assert.AreEqual("cancelled", Text(gone, "state"), Describe(gone));
         Assert.AreEqual("The target account was deleted.", Text(gone, "last_error"));
         Assert.AreEqual("queued", Text(Of(jobs, "staying@source.test"), "state"));
      }

      [Test]
      [Description("A Google Workspace migration takes the provider's own server and refuses another, a job may carry a refresh token, and on a server not registered with Google a job without one, or a sign-in link, is refused with what is missing.")]
      public void AGoogleMigrationSaysWhatItNeedsOnAServerNotRegisteredWithGoogle()
      {
         Target("anna");

         // Whatever an earlier fixture left, this server is not registered.
         ClearSignInSettings();
         _signInChanged = true;

         (int status, string body) google = Admin("POST", "/api/v1/migrations", "{\"name\":\"Workspace\",\"kind\":\"google\"}");
         Assert.AreEqual(201, google.status, google.body);
         JsonElement made = Json(google.body);
         Assert.AreEqual("google", Text(made, "kind"));
         Assert.AreEqual("imap.gmail.com", Text(made, "host"));
         Assert.AreEqual(993, Number(made, "port"));
         Assert.AreEqual("tls", Text(made, "security"));
         long id = Number(made, "id");

         (int status, string body) microsoft = Admin("POST", "/api/v1/migrations", "{\"name\":\"Tenant\",\"kind\":\"microsoft365\"}");
         Assert.AreEqual(201, microsoft.status, microsoft.body);
         Assert.AreEqual("outlook.office365.com", Text(Json(microsoft.body), "host"));

         // The owner's token goes to the provider's own server, over TLS, and
         // nowhere else.
         (int status, string body) elsewhere = Admin("POST", "/api/v1/migrations", "{\"name\":\"Elsewhere\",\"kind\":\"google\",\"host\":\"imap.elsewhere.test\"}");
         Assert.AreEqual(400, elsewhere.status, elsewhere.body);
         StringAssert.Contains("host must be imap.gmail.com", elsewhere.body);
         (int status, string body) clear = Admin("POST", "/api/v1/migrations", "{\"name\":\"Clear\",\"kind\":\"google\",\"security\":\"none\"}");
         Assert.AreEqual(400, clear.status, clear.body);
         StringAssert.Contains("security must be tls", clear.body);
         (int status, string body) port = Admin("POST", "/api/v1/migrations", "{\"name\":\"Port\",\"kind\":\"microsoft365\",\"port\":143}");
         Assert.AreEqual(400, port.status, port.body);
         StringAssert.Contains("port must be 993", port.body);

         AssertRefusedLine(id, "someone@workspace.test,,anna@example.test", 1, "no sign-in link can be made");

         JsonElement added = AddJobs(id, "someone@workspace.test,1//a-refresh-token-an-administrator-holds,anna@example.test", 1);
         JsonElement job = added.GetProperty("jobs")[0];
         Assert.IsTrue(job.GetProperty("signs_in").GetBoolean());
         Assert.IsFalse(job.GetProperty("awaiting_signin").GetBoolean(), "A job given its refresh token waits for nobody.");
         Assert.IsFalse(job.TryGetProperty("signin_url", out _));

         (int status, string body) link = Admin("POST", "/api/v1/migrations/" + id + "/jobs/" + Number(job, "id") + "/signin", "{}");
         Assert.AreEqual(503, link.status, link.body);
         StringAssert.Contains("not set up on this server", link.body);

         // An IMAP migration's job signs in with its password, never with a link.
         long imap = CreateMigration("Passwords", "");
         JsonElement imapJob = AddJobs(imap, "anna@source.test,pw,anna@example.test", 1).GetProperty("jobs")[0];
         Assert.AreEqual(409, Admin("POST", "/api/v1/migrations/" + imap + "/jobs/" + Number(imapJob, "id") + "/signin", "{}").status);

         // DAV over plain HTTP is for this machine only.
         (int status, string body) plain = Admin("POST", "/api/v1/migrations", "{\"name\":\"Cleartext\",\"host\":\"imap.far.test\",\"dav_url\":\"http://dav.far.test/\"}");
         Assert.AreEqual(400, plain.status, plain.body);
         StringAssert.Contains("plain http only to this machine", plain.body);
      }

      [Test]
      [Description("A Google Workspace job's owner signs in through a link that works once: a newer link voids the flow an older one began, a sign-in as another address keeps nothing and gives the link back, and a cancelled job signed in again is queued. The provider's token endpoint busy is tried again; a grant it refuses fails the job at once.")]
      public void AnOwnersSignInIsKeptForTheirJobOnceAndABusyTokenEndpointIsTriedAgain()
      {
         Target("anna");

         using (var provider = new FakeHttpEndpoint(TokenPort, 200, TokenAnswer("someone@workspace.test", "refresh-1"), "application/json"))
         {
            _signInChanged = true;
            (int status, string body) registered = Admin("PUT", "/api/v1/settings",
               "{\"import_oauth_redirect_base_url\":\"https://mail.example.test\",\"import_oauth_google_client_id\":\"client-g\",\"import_oauth_google_client_secret\":\"secret-g\"," +
               "\"import_oauth_google_token_url\":\"" + provider.UrlFor("/token") + "\"}");
            Assert.AreEqual(200, registered.status, registered.body);

            (int status, string body) google = Admin("POST", "/api/v1/migrations", "{\"name\":\"Workspace\",\"kind\":\"google\"}");
            Assert.AreEqual(201, google.status, google.body);
            long migration = Number(Json(google.body), "id");
            JsonElement added = AddJobs(migration, "someone@workspace.test,,anna@example.test", 1).GetProperty("jobs")[0];
            long job = Number(added, "id");
            Assert.IsTrue(added.GetProperty("awaiting_signin").GetBoolean());
            string firstLink = LinkPath(Text(added, "signin_url"));

            // A key that is not the link's opens nothing.
            Assert.AreEqual(404, Raw("GET", Regex.Replace(firstLink, "key=[0-9a-f]+", "key=" + new string('0', 64)), null, null, null).Status);

            // The owner opens it: to Google, asking for the mail, the contacts and
            // the calendars, with the cookie the callback checks. Spent by opening.
            var opened = Open(firstLink);
            StringAssert.StartsWith("https://accounts.google.com/", opened.location);
            StringAssert.Contains("carddav", Uri.UnescapeDataString(opened.location));
            Assert.AreEqual(404, Raw("GET", firstLink, null, null, null).Status, "A link opened once opened again.");

            // A newer link, and the flow the old one began is refused at its end.
            string secondLink = NewLink(migration, job);
            Response stale = Callback(opened.state, opened.nonce);
            Assert.AreEqual(409, stale.Status, stale.Body);
            StringAssert.Contains("replaced", stale.Body);
            Assert.IsTrue(Of(Jobs(migration), "someone@workspace.test").GetProperty("awaiting_signin").GetBoolean());

            // Signed in as somebody else: nothing kept, and the link works again.
            provider.SetResponse(200, TokenAnswer("other@workspace.test", "refresh-other"));
            var wrong = Open(secondLink);
            Response wrongAnswer = Callback(wrong.state, wrong.nonce);
            Assert.AreEqual(409, wrongAnswer.Status, wrongAnswer.Body);
            StringAssert.Contains("Signed in as another address", wrongAnswer.Body);
            Assert.IsTrue(Of(Jobs(migration), "someone@workspace.test").GetProperty("awaiting_signin").GetBoolean());

            provider.SetResponse(200, TokenAnswer("someone@workspace.test", "refresh-1"));
            var right = Open(secondLink);
            Response rightAnswer = Callback(right.state, right.nonce);
            Assert.AreEqual(200, rightAnswer.Status, rightAnswer.Body);
            StringAssert.Contains("Your mailbox will be copied", rightAnswer.Body);
            StringAssert.Contains("grant_type=authorization_code", Encoding.ASCII.GetString(provider.RequestBodies.Last()));
            Assert.IsFalse(Of(Jobs(migration), "someone@workspace.test").GetProperty("awaiting_signin").GetBoolean());
            Assert.AreEqual(404, Raw("GET", secondLink, null, null, null).Status, "A link that signed in works again.");

            // Cancelled, then signed in for again: queued, to run when started.
            Assert.AreEqual(200, Admin("POST", "/api/v1/migrations/" + migration + "/jobs/" + job + "/cancel", "{}").status);
            Assert.AreEqual("cancelled", Text(Of(Jobs(migration), "someone@workspace.test"), "state"));
            provider.SetResponse(200, TokenAnswer("someone@workspace.test", "refresh-2"));
            var again = Open(NewLink(migration, job));
            Assert.AreEqual(200, Callback(again.state, again.nonce).Status);
            JsonElement requeued = Of(Jobs(migration), "someone@workspace.test");
            Assert.AreEqual("queued", Text(requeued, "state"), "A cancelled job signed in for again was not queued.");
            Assert.AreEqual(0, Number(requeued, "attempts"));

            // The provider busy: a retry, with Google's own servers never reached.
            provider.SetResponse(503, "<html><body>Service unavailable</body></html>", "text/html");
            Start(migration);
            JsonElement busy = WaitForJobs(migration, all => Text(all[0], "state") == "paused", "the job to pause at the busy token endpoint")[0];
            Assert.AreEqual(1, Number(busy, "attempts"), Describe(busy));
            StringAssert.Contains("503", Text(busy, "last_error"));
            StringAssert.Contains("grant_type=refresh_token&refresh_token=refresh-2", Encoding.ASCII.GetString(provider.RequestBodies.Last()),
               "The refresh token the last sign-in gave is the one used.");

            // A grant the provider refuses: failed at once, with its words.
            provider.SetResponse(400, "{\"error\":\"invalid_grant\",\"error_description\":\"Token has been expired or revoked.\"}", "application/json");
            Retry(migration, job, null);
            JsonElement refused = WaitForJobs(migration, all => Text(all[0], "state") == "failed", "the refused grant")[0];
            StringAssert.Contains("expired or revoked", Text(refused, "last_error"));
            Assert.AreEqual(0, Number(refused, "attempts"), "A refused grant is not a retry.");
         }
      }

      // ------------------------------------------------------------ the steps

      private Account Source(string local)
      {
         return SingletonProvider<TestSetup>.Instance.AddAccount(_source, local + "@source.test", SourcePassword);
      }

      private Account Target(string local)
      {
         return SingletonProvider<TestSetup>.Instance.AddAccount(_domain, local + "@example.test", TargetPassword);
      }

      private static string Row(string name)
      {
         return name + "@source.test," + SourcePassword + "," + name + "@example.test";
      }

      private static string MessageText(string messageId, string subject, int bodyBytes)
      {
         string body = "The body of " + subject + ".\r\n";
         if (bodyBytes > body.Length)
         {
            var padded = new StringBuilder(body);
            while (padded.Length < bodyBytes)
               padded.Append("Line of padding to make the message as large as the test needs it.\r\n");
            body = padded.ToString();
         }

         return "From: someone@elsewhere.test\r\n" +
                "To: owner@source.test\r\n" +
                "Subject: " + subject + "\r\n" +
                "Message-ID: <" + messageId + ">\r\n" +
                "Date: Wed, 17 Jul 1996 02:44:25 -0700\r\n" +
                "\r\n" +
                body;
      }

      private static string ScriptedText(string name)
      {
         return "From: someone@scripted.test\r\nSubject: " + name + "\r\nMessage-ID: <" + name + "@scripted.test>\r\n\r\nThe body of " + name + ".\r\n";
      }

      // Messages appended to a folder of a source account, numbered from first,
      // each with a Message-ID of its own made of the prefix and its number - so
      // the same prefix and number in two folders is the same message - and the
      // flags the function gives its number. The folder is made first unless
      // create is false; bodyBytes pads each message to about that size.
      private static void Seed(string address, string folder, int count, string prefix, Func<int, string> flags,
                               int first = 1, bool create = true, int bodyBytes = 0)
      {
         var imap = new ImapClientSimulator();
         Assert.IsTrue(imap.ConnectAndLogon(address, SourcePassword), "Could not log on as " + address);
         try
         {
            if (create && !folder.Equals("INBOX", StringComparison.OrdinalIgnoreCase))
               Assert.IsTrue(imap.CreateFolder(folder), "Could not create " + folder + " in " + address);

            for (int i = first; i < first + count; i++)
            {
               string text = MessageText(prefix + "-" + i + "@source.test", prefix + " " + i, bodyBytes);
               string flagList = flags == null ? "" : flags(i);
               string command = "B" + i.ToString("D4") + " APPEND \"" + folder + "\" (" + flagList + ") \"" + InternalDate + "\" {" +
                                Encoding.ASCII.GetByteCount(text) + "}";
               string answer = imap.SendSingleCommandWithLiteral(command, text);
               StringAssert.Contains("B" + i.ToString("D4") + " OK", answer, "APPEND " + i + " to " + folder + " of " + address);
            }
         }
         finally
         {
            imap.Disconnect();
         }
      }

      // A folder made, with a special use when one is given (RFC 6154's CREATE ... USE).
      private static void CreateFolder(string address, string password, string folder, string use)
      {
         var imap = new ImapClientSimulator();
         Assert.IsTrue(imap.ConnectAndLogon(address, password), "Could not log on as " + address);
         try
         {
            if (use == null)
            {
               Assert.IsTrue(imap.CreateFolder(folder), "Could not create " + folder + " in " + address);
               return;
            }

            string answer = imap.SendSingleCommand("C1 CREATE \"" + folder + "\" (USE (" + use + "))");
            StringAssert.StartsWith("C1 OK", answer.Substring(answer.LastIndexOf("C1 ", StringComparison.Ordinal)),
               "Could not create " + folder + " as " + use + " in " + address + ": " + answer);
         }
         finally
         {
            imap.Disconnect();
         }
      }

      /// <summary>How many messages a folder holds, or -1 when there is no such folder.</summary>
      private static int CountIn(string address, string password, string folder)
      {
         var imap = new ImapClientSimulator();
         Assert.IsTrue(imap.ConnectAndLogon(address, password), "Could not log on as " + address);
         try
         {
            string text;
            if (!imap.SelectFolder(folder, out text))
               return -1;

            Match match = Regex.Match(text, @"\* (\d+) EXISTS");
            Assert.IsTrue(match.Success, "SELECT " + folder + " answered without EXISTS: " + text);
            return int.Parse(match.Groups[1].Value);
         }
         finally
         {
            imap.Disconnect();
         }
      }

      private static string Subscribed(string address, string password)
      {
         var imap = new ImapClientSimulator();
         Assert.IsTrue(imap.ConnectAndLogon(address, password), "Could not log on as " + address);
         try
         {
            return imap.LSUB();
         }
         finally
         {
            imap.Disconnect();
         }
      }

      private sealed class Flagged
      {
         public string Flags;
         public string Date;
      }

      // Every message's flags (sorted, \Recent left out) and internal date, in
      // sequence order. EXAMINE, so reading them changes nothing.
      private static List<Flagged> FlagsAndDates(string address, string password, string folder)
      {
         var imap = new ImapClientSimulator();
         Assert.IsTrue(imap.ConnectAndLogon(address, password), "Could not log on as " + address);
         try
         {
            imap.ExamineFolder(folder);
            string answer = imap.Fetch("1:* (FLAGS INTERNALDATE)");
            var list = new List<Flagged>();
            foreach (Match line in Regex.Matches(answer, @"\* (\d+) FETCH \(([^\r\n]*)\)"))
            {
               string items = line.Groups[2].Value;
               Match flags = Regex.Match(items, @"FLAGS \(([^)]*)\)");
               Match date = Regex.Match(items, "INTERNALDATE \"([^\"]*)\"");
               Assert.IsTrue(flags.Success && date.Success, "A FETCH line without FLAGS or INTERNALDATE: " + line.Value);
               list.Add(new Flagged
               {
                  Flags = string.Join(" ", flags.Groups[1].Value.Split(new[] { ' ' }, StringSplitOptions.RemoveEmptyEntries)
                     .Where(f => !f.Equals("\\Recent", StringComparison.OrdinalIgnoreCase)).OrderBy(f => f, StringComparer.Ordinal)),
                  Date = date.Groups[1].Value
               });
            }
            return list;
         }
         finally
         {
            imap.Disconnect();
         }
      }

      // Every message's size and whole text, in sequence order, as FETCH answers
      // them: the tagged completion left out.
      private static string Bodies(string address, string password, string folder)
      {
         var imap = new ImapClientSimulator();
         Assert.IsTrue(imap.ConnectAndLogon(address, password), "Could not log on as " + address);
         try
         {
            imap.ExamineFolder(folder);
            string answer = imap.Fetch("1:* (RFC822.SIZE BODY.PEEK[])");
            int tagged = answer.LastIndexOf("\r\nA17 ", StringComparison.Ordinal);
            return tagged >= 0 ? answer.Substring(0, tagged) : answer;
         }
         finally
         {
            imap.Disconnect();
         }
      }

      // The Message-ID of every message in a folder.
      private static List<string> MessageIds(string address, string password, string folder)
      {
         var imap = new ImapClientSimulator();
         Assert.IsTrue(imap.ConnectAndLogon(address, password), "Could not log on as " + address);
         try
         {
            imap.ExamineFolder(folder);
            string answer = imap.Fetch("1:* (BODY.PEEK[HEADER.FIELDS (MESSAGE-ID)])");
            return Regex.Matches(answer, @"Message-ID:\s*(<[^>\r\n]+>)", RegexOptions.IgnoreCase)
               .Cast<Match>().Select(m => m.Groups[1].Value).ToList();
         }
         finally
         {
            imap.Disconnect();
         }
      }

      private static long CreateMigration(string name, string extra, int davPort = WebServicesPort, int imapPort = 143)
      {
         string body = "{\"name\":\"" + name + "\",\"host\":\"127.0.0.1\",\"port\":" + imapPort + ",\"security\":\"none\"," +
                       "\"dav_url\":\"http://127.0.0.1:" + davPort + "/\"" + extra + "}";
         (int status, string body) created = Admin("POST", "/api/v1/migrations", body);
         Assert.AreEqual(201, created.status, body + " -> " + created.body);
         JsonElement migration = Json(created.body);
         Assert.AreEqual("new", Text(migration, "state"));
         return Number(migration, "id");
      }

      private static JsonElement AddJobs(long migration, string rows, int expected)
      {
         (int status, string body) added = Admin("POST", "/api/v1/migrations/" + migration + "/jobs", rows);
         Assert.AreEqual(201, added.status, rows + " -> " + added.body);
         JsonElement answer = Json(added.body);
         Assert.AreEqual(expected, Number(answer, "created"), added.body);
         return answer;
      }

      private static void AssertRefusedLine(long migration, string rows, int line, string reason)
      {
         int before = Jobs(migration).Count;
         (int status, string body) refused = Admin("POST", "/api/v1/migrations/" + migration + "/jobs", rows);
         Assert.AreEqual(400, refused.status, rows + " -> " + refused.body);
         JsonElement answer = Json(refused.body);
         Assert.AreEqual(line, Number(answer, "line"), refused.body);
         StringAssert.StartsWith("line " + line + ": ", Text(answer, "error"));
         StringAssert.Contains(reason, Text(answer, "error"));
         Assert.AreEqual(before, Jobs(migration).Count, "The refused request added a job: " + rows);
      }

      private static void Start(long migration)
      {
         (int status, string body) started = Admin("POST", "/api/v1/migrations/" + migration + "/start", "{}");
         Assert.AreEqual(200, started.status, started.body);
         Assert.AreEqual("running", Text(Json(started.body), "state"));
      }

      private static void Pause(long migration)
      {
         (int status, string body) paused = Admin("POST", "/api/v1/migrations/" + migration + "/pause", "{}");
         Assert.AreEqual(200, paused.status, paused.body);
      }

      private static void Resume(long migration)
      {
         // Refused with 409 while a job is still stopping; that is seconds.
         DateTime until = DateTime.Now.AddSeconds(30);
         while (true)
         {
            (int status, string body) resumed = Admin("POST", "/api/v1/migrations/" + migration + "/resume", "{}");
            if (resumed.status == 200)
               return;
            if (resumed.status != 409 || DateTime.Now > until)
               Assert.Fail("Resume was refused: " + resumed.status + " " + resumed.body);
            Thread.Sleep(250);
         }
      }

      private static void Retry(long migration, long job, string secret)
      {
         string body = secret == null ? "{}" : "{\"secret\":\"" + secret + "\"}";
         (int status, string body) retried = Admin("POST", "/api/v1/migrations/" + migration + "/jobs/" + job + "/retry", body);
         Assert.AreEqual(200, retried.status, retried.body);
      }

      private static void WaitForMigration(long migration, string state)
      {
         DateTime deadline = DateTime.Now.AddSeconds(60);
         string now = Text(Get("/api/v1/migrations/" + migration), "state");
         while (now != state)
         {
            if (DateTime.Now > deadline)
               Assert.Fail("The migration is " + now + ", and did not become " + state + ".");
            Thread.Sleep(200);
            now = Text(Get("/api/v1/migrations/" + migration), "state");
         }
      }

      private static List<JsonElement> Jobs(long migration)
      {
         return Get("/api/v1/migrations/" + migration + "/jobs").GetProperty("jobs").EnumerateArray().ToList();
      }

      private static JsonElement Of(List<JsonElement> jobs, string sourceUser)
      {
         return jobs.Single(j => Text(j, "source_user") == sourceUser);
      }

      private static bool Settled(JsonElement job)
      {
         string state = Text(job, "state");
         return state == "done" || state == "failed" || state == "cancelled";
      }

      private static string Describe(JsonElement job)
      {
         return Text(job, "source_user") + ": " + Text(job, "state") + ", " + Number(job, "messages_copied") + " of " +
                Number(job, "messages_total") + ", last error '" + Text(job, "last_error") + "'";
      }

      private static List<JsonElement> WaitForJobs(long migration, Func<List<JsonElement>, bool> until, string what, int seconds = 180)
      {
         DateTime deadline = DateTime.Now.AddSeconds(seconds);
         List<JsonElement> jobs = Jobs(migration);
         while (!until(jobs))
         {
            if (DateTime.Now > deadline)
               Assert.Fail("Waited " + seconds + " seconds for " + what + ". The jobs: " + string.Join("; ", jobs.Select(Describe)));
            Thread.Sleep(200);
            jobs = Jobs(migration);
         }
         return jobs;
      }

      // Until every job of the migrations has settled: the most jobs the server
      // said were running at once, and the most IMAP sessions the source held.
      private (int maxRunning, int maxSessions) Watch(long[] migrations)
      {
         int maxRunning = 0;
         int maxSessions = 0;
         DateTime deadline = DateTime.Now.AddSeconds(180);
         while (true)
         {
            int sessions = _application.Status.get_SessionCount(eSessionType.eSTIMAP);
            List<JsonElement> listed = Json(Admin("GET", "/api/v1/migrations").body).GetProperty("migrations").EnumerateArray()
               .Where(m => migrations.Contains(Number(m, "id"))).ToList();
            int running = listed.Sum(m => (int) Number(m.GetProperty("jobs"), "running"));

            maxRunning = Math.Max(maxRunning, running);
            maxSessions = Math.Max(maxSessions, sessions);

            if (listed.All(m => Number(m.GetProperty("jobs"), "queued") + Number(m.GetProperty("jobs"), "running") +
                                Number(m.GetProperty("jobs"), "paused") == 0))
               return (maxRunning, maxSessions);

            if (DateTime.Now > deadline)
               Assert.Fail("The migrations did not finish in three minutes.");
            Thread.Sleep(100);
         }
      }

      private static JsonElement Report(long migration, long job)
      {
         return Get("/api/v1/migrations/" + migration + "/jobs/" + job + "/report");
      }

      private static JsonElement Folder(JsonElement report, string source)
      {
         foreach (JsonElement folder in report.GetProperty("folders").EnumerateArray())
         {
            if (Text(folder, "source") == source)
               return folder;
         }

         Assert.Fail("The report has no folder " + source + ": " + report);
         return default(JsonElement);
      }

      private static long Reason(JsonElement tally, string reason)
      {
         JsonElement count;
         return tally.GetProperty("skipped_reasons").TryGetProperty(reason, out count) ? count.GetInt64() : 0;
      }

      private void RemoveEveryMigration()
      {
         (int status, string body) listed = Admin("GET", "/api/v1/migrations");
         if (listed.status != 200)
            return;

         var stuck = new List<string>();
         foreach (JsonElement migration in Json(listed.body).GetProperty("migrations").EnumerateArray())
         {
            long id = Number(migration, "id");
            if (Text(migration, "state") == "running")
               Admin("POST", "/api/v1/migrations/" + id + "/pause", "{}");

            // Refused while a job of it is still stopping.
            DateTime until = DateTime.Now.AddSeconds(60);
            while (true)
            {
               (int status, string body) deleted = Admin("DELETE", "/api/v1/migrations/" + id);
               if (deleted.status == 200 || deleted.status == 404)
                  break;
               if (DateTime.Now > until)
               {
                  stuck.Add(id + ": " + deleted.status + " " + deleted.body);
                  break;
               }
               Thread.Sleep(250);
            }
         }

         if (stuck.Count > 0)
            Assert.Fail("Migrations could not be deleted: " + string.Join("; ", stuck));
      }

      private string CreateKey(string label, string body)
      {
         (int status, string created) = Admin("POST", "/api/v1/apikeys", body);
         Assert.AreEqual(201, status, "POST /api/v1/apikeys (" + label + "): " + created);
         JsonElement key = Json(created);
         JsonElement id = key.GetProperty("id");
         _keyIds.Add(id.ValueKind == JsonValueKind.String ? id.GetString() : id.GetRawText());
         return Text(key, "key");
      }

      // ------------------------------------------------------------- the sign-in

      private static void ClearSignInSettings()
      {
         Admin("PUT", "/api/v1/settings",
            "{\"import_oauth_redirect_base_url\":\"\",\"import_oauth_google_client_id\":\"\",\"import_oauth_google_client_secret\":\"\"," +
            "\"import_oauth_google_authorize_url\":\"https://accounts.google.com/o/oauth2/v2/auth\",\"import_oauth_google_token_url\":\"https://oauth2.googleapis.com/token\"," +
            "\"import_oauth_microsoft_client_id\":\"\",\"import_oauth_microsoft_client_secret\":\"\"}");
      }

      // What the provider's token endpoint answers: a refresh token and an
      // id_token naming who signed in.
      private static string TokenAnswer(string address, string refresh)
      {
         string claims = "{\"sub\":\"1\",\"email\":\"" + address + "\"}";
         string idToken = "eyJhbGciOiJub25lIn0." + Base64Url(Encoding.UTF8.GetBytes(claims)) + ".sig";
         return "{\"access_token\":\"access-for-" + refresh + "\",\"token_type\":\"Bearer\",\"expires_in\":3600," +
                "\"refresh_token\":\"" + refresh + "\",\"id_token\":\"" + idToken + "\",\"scope\":\"https://mail.google.com/ https://www.googleapis.com/auth/carddav https://www.googleapis.com/auth/calendar\"}";
      }

      private static string Base64Url(byte[] bytes)
      {
         return Convert.ToBase64String(bytes).TrimEnd('=').Replace('+', '-').Replace('/', '_');
      }

      // The path and query of a sign-in link, to open on this listener.
      private static string LinkPath(string url)
      {
         int at = url.IndexOf("/portal/oauth/migration", StringComparison.Ordinal);
         Assert.GreaterOrEqual(at, 0, "Not a sign-in link: " + url);
         return url.Substring(at);
      }

      private static string NewLink(long migration, long job)
      {
         (int status, string body) made = Admin("POST", "/api/v1/migrations/" + migration + "/jobs/" + job + "/signin", "{}");
         Assert.AreEqual(200, made.status, made.body);
         return LinkPath(Text(Json(made.body), "signin_url"));
      }

      // A link opened: where it sends the browser, the flow's state and the cookie.
      private static (string location, string state, string nonce) Open(string path)
      {
         Response opened = Raw("GET", path, null, null, null);
         Assert.AreEqual(302, opened.Status, opened.Headers + opened.Body);
         string location = HeaderLine(opened.Headers, "Location");
         string cookie = HeaderLine(opened.Headers, "Set-Cookie");
         StringAssert.StartsWith("hmimport=", cookie);
         return (location, Regex.Match(location, "[?&]state=([^&]*)").Groups[1].Value, Regex.Match(cookie, "^hmimport=([^;]+)").Groups[1].Value);
      }

      private static Response Callback(string state, string nonce)
      {
         return Raw("GET", "/portal/oauth/callback?state=" + state + "&code=a-code", null, null, "hmimport=" + nonce);
      }

      private static string HeaderLine(string headers, string name)
      {
         Match match = Regex.Match(headers, "^" + name + ":\\s*(.*)$", RegexOptions.Multiline | RegexOptions.IgnoreCase);
         return match.Success ? match.Groups[1].Value.TrimEnd('\r') : "";
      }

      // ------------------------------------------------------------- HTTP

      private static JsonElement Get(string path)
      {
         (int status, string body) answer = Admin("GET", path);
         Assert.AreEqual(200, answer.status, "GET " + path + ": " + answer.body);
         return Json(answer.body);
      }

      private static JsonElement Json(string body)
      {
         using (JsonDocument document = JsonDocument.Parse(body))
            return document.RootElement.Clone();
      }

      private static string Text(JsonElement element, string name)
      {
         return element.GetProperty(name).GetString();
      }

      private static long Number(JsonElement element, string name)
      {
         return element.GetProperty(name).GetInt64();
      }

      private static string Basic(string user, string password)
      {
         return "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(user + ":" + password));
      }

      private static (int status, string body) Admin(string method, string path, string body = null)
      {
         return RestRequest.Send("127.0.0.1", RestPort, method, path, Basic("Administrator", AdminPassword), body);
      }

      private static (int status, string body) WithKey(string key, string method, string path, string body = null)
      {
         return RestRequest.Send("127.0.0.1", RestPort, method, path, "Bearer " + key, body);
      }

      private static (int status, string body) AsAccount(string address, string password, string method, string path, string body)
      {
         return RestRequest.Send("127.0.0.1", RestPort, method, path, Basic(address, password), body);
      }

      private sealed class Response
      {
         public int Status;
         public string Headers = "";
         public string Body = "";
      }

      // A raw exchange with the REST listener, for the headers RestRequest does
      // not return and the cookie it does not send.
      private static Response Raw(string method, string path, string authorization, string body, string cookie)
      {
         using (var client = new TcpClient())
         {
            client.Connect("127.0.0.1", RestPort);
            using (NetworkStream stream = client.GetStream())
            {
               byte[] payload = Encoding.UTF8.GetBytes(body ?? "");
               var head = new StringBuilder();
               head.Append(method + " " + path + " HTTP/1.0\r\n");
               head.Append("Host: 127.0.0.1\r\n");
               if (authorization != null)
                  head.Append("Authorization: " + authorization + "\r\n");
               if (cookie != null)
                  head.Append("Cookie: " + cookie + "\r\n");
               if (body != null)
               {
                  head.Append("Content-Type: application/json\r\n");
                  head.Append("Content-Length: " + payload.Length + "\r\n");
               }
               head.Append("Connection: close\r\n\r\n");

               byte[] headBytes = Encoding.ASCII.GetBytes(head.ToString());
               stream.Write(headBytes, 0, headBytes.Length);
               if (body != null)
                  stream.Write(payload, 0, payload.Length);

               stream.ReadTimeout = 30000;
               var memory = new MemoryStream();
               stream.CopyTo(memory);
               string text = Encoding.UTF8.GetString(memory.ToArray());

               int split = text.IndexOf("\r\n\r\n", StringComparison.Ordinal);
               var response = new Response();
               string headers = split >= 0 ? text.Substring(0, split) : text;
               response.Status = int.Parse(Regex.Match(headers, "^HTTP/1\\.[01] (\\d{3})").Groups[1].Value);
               response.Headers = headers;
               response.Body = split >= 0 ? text.Substring(split + 4) : "";
               return response;
            }
         }
      }

      // One HTTP/1.0 request with a body of any type to another listener - the
      // CalDAV PUT, which RestRequest (JSON only) does not send.
      private static (int status, string body) Http(int port, string method, string path, string authorization, string body,
                                                    string contentType, string extraHeaders)
      {
         using (var client = new TcpClient())
         {
            client.Connect("127.0.0.1", port);
            using (NetworkStream stream = client.GetStream())
            using (var memory = new MemoryStream())
            {
               byte[] bytes = body == null ? new byte[0] : Encoding.UTF8.GetBytes(body);
               var request = new StringBuilder();
               request.Append(method + " " + path + " HTTP/1.0\r\n");
               request.Append("Host: 127.0.0.1:" + port + "\r\n");
               if (authorization != null)
                  request.Append("Authorization: " + authorization + "\r\n");
               if (body != null)
               {
                  request.Append("Content-Type: " + contentType + "\r\n");
                  request.Append("Content-Length: " + bytes.Length + "\r\n");
               }
               if (!string.IsNullOrEmpty(extraHeaders))
                  request.Append(extraHeaders);
               request.Append("Connection: close\r\n\r\n");

               byte[] head = Encoding.ASCII.GetBytes(request.ToString());
               stream.Write(head, 0, head.Length);
               stream.Write(bytes, 0, bytes.Length);
               stream.CopyTo(memory);

               string text = Encoding.UTF8.GetString(memory.ToArray());
               int split = text.IndexOf("\r\n\r\n", StringComparison.Ordinal);
               string statusLine = text.Substring(0, Math.Max(0, text.IndexOf("\r\n", StringComparison.Ordinal)));
               string[] parts = statusLine.Split(' ');
               int status = parts.Length > 1 ? int.Parse(parts[1]) : 0;
               return (status, split >= 0 ? text.Substring(split + 4) : "");
            }
         }
      }
   }
}
