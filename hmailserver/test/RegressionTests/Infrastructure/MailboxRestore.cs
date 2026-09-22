// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Text;
using System.Text.Json;
using System.Threading;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;

namespace RegressionTests.Infrastructure
{
   /// <summary>
   ///    One mailbox, or one message, put back from a backup archive without the
   ///    rest of the server (Common/Application/MailboxRestore): the four REST
   ///    routes, the COM methods and what they do to a live mailbox.
   ///
   ///    The shape of every test: a domain with two accounts, folders and
   ///    messages with flags is backed up by the server's own backup into a
   ///    destination of this fixture's own; something is then deleted from, or
   ///    delivered to, one account; and the restore is judged by what the
   ///    mailbox holds afterwards, read back over IMAP, beside the counts the
   ///    job reports. Every test carries its negative control - the other
   ///    account untouched, the message that was still there not duplicated,
   ///    the request that is refused changing nothing.
   ///
   ///    The listener is the fixture's own, on a port from the range this row
   ///    was allocated (9846-9855).
   /// </summary>
   [TestFixture]
   public class MailboxRestore : TestFixtureBase
   {
      private static int RestPort = 9846;
      private const string AdminPassword = "testar";
      private const string Password = "Restore-Passw0rd!";

      // A word that appears in one message's body and nowhere else, for the
      // search.
      private const string Needle = "Quenchable";

      private string _backupDir;
      private Account _ann;
      private Account _bob;

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(AdminPassword);

         _backupDir = Path.Combine(Path.GetTempPath(), "hm-mailbox-restore-" + TestSetup.UniqueString());
         Directory.CreateDirectory(_backupDir);

         if (File.Exists(_application.Settings.Backup.LogFile))
            File.Delete(_application.Settings.Backup.LogFile);

         var backup = _application.Settings.Backup;
         backup.BackupDomains = true;
         backup.BackupMessages = true;
         backup.BackupSettings = false;
         backup.CompressDestinationFiles = true;
         backup.Destination = _backupDir;

         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();

         (int status, string body) probe = Admin("GET", "/api/v1/status", null);
         Assert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);
      }

      [TearDown]
      public void StopRestApi()
      {
         try
         {
            RestListener.Stop();
            _application.Reinitialize();
         }
         finally
         {
            try
            {
               if (Directory.Exists(_backupDir))
                  Directory.Delete(_backupDir, true);
            }
            catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
            {
               // The service may still hold an archive open for a moment; the
               // temp directory is not worth failing a test over.
            }
         }
      }

      // ---- the mailbox and the archive ----------------------------------------

      /// <summary>
      ///    ann has first (flagged, with a label), second and third in INBOX, and
      ///    copies of second and third in Projects.Alpha; bob has two of his own.
      /// </summary>
      private void Populate()
      {
         _ann = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ann@" + _domain.Name, Password);
         _bob = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "bob@" + _domain.Name, Password);

         var smtp = new SmtpClientSimulator();

         smtp.Send("sender@elsewhere.test", _ann.Address, "first", "The first message, " + Needle + " in its body.");
         Pop3ClientSimulator.AssertMessageCount(_ann.Address, Password, 1);
         smtp.Send("sender@elsewhere.test", _ann.Address, "second", "The second message.");
         Pop3ClientSimulator.AssertMessageCount(_ann.Address, Password, 2);
         smtp.Send("sender@elsewhere.test", _ann.Address, "third", "The third message.");
         Pop3ClientSimulator.AssertMessageCount(_ann.Address, Password, 3);

         smtp.Send("sender@elsewhere.test", _bob.Address, "bob one", "Bob's first.");
         Pop3ClientSimulator.AssertMessageCount(_bob.Address, Password, 1);
         smtp.Send("sender@elsewhere.test", _bob.Address, "bob two", "Bob's second.");
         Pop3ClientSimulator.AssertMessageCount(_bob.Address, Password, 2);

         var imap = new ImapClientSimulator();
         Assert.IsTrue(imap.ConnectAndLogon(_ann.Address, Password));
         Assert.IsTrue(imap.CreateFolder("Projects"));
         Assert.IsTrue(imap.CreateFolder("Projects.Alpha"));
         Assert.IsTrue(imap.SelectFolder("INBOX"));
         Assert.IsTrue(imap.SetFlagOnMessage(1, true, "\\Flagged"));
         Assert.IsTrue(imap.SetFlagOnMessage(1, true, "$label1"));
         Assert.IsTrue(imap.Copy(2, "Projects.Alpha"));
         Assert.IsTrue(imap.Copy(3, "Projects.Alpha"));
         imap.Disconnect();

         ImapClientSimulator.AssertMessageCount(_ann.Address, Password, "Projects.Alpha", 2);
      }

      private string TakeBackup()
      {
         _application.BackupManager.StartBackup();

         for (int i = 0; i < 240; i++)
         {
            try
            {
               string contents = TestSetup.ReadExistingTextFile(_application.Settings.Backup.LogFile);
               if (contents.IndexOf("Backup completed successfully", StringComparison.Ordinal) >= 0)
                  break;
               Assert.IsTrue(contents.IndexOf("BACKUP ERROR:", StringComparison.Ordinal) < 0, "The backup failed: " + contents);
            }
            catch (IOException)
            {
               // The log is being written.
            }

            Thread.Sleep(250);
         }

         (int status, string body) listed = Admin("GET", "/api/v1/backup/archives", null);
         Assert.AreEqual(200, listed.status, listed.body);

         using (JsonDocument document = JsonDocument.Parse(listed.body))
         {
            JsonElement archives = document.RootElement.GetProperty("archives");
            Assert.AreEqual(JsonValueKind.Array, archives.ValueKind, listed.body);
            Assert.Greater(archives.GetArrayLength(), 0, "The backup wrote no archive the listing can see: " + listed.body);

            JsonElement newest = archives[0];
            StringAssert.StartsWith("HMBackup ", newest.GetProperty("name").GetString(), listed.body);
            Assert.IsFalse(newest.GetProperty("encrypted").GetBoolean(), "No passphrase is set, so the archive must not read as encrypted.");
            Assert.AreEqual(_backupDir.TrimEnd('\\'), document.RootElement.GetProperty("destination").GetString());

            return newest.GetProperty("name").GetString();
         }
      }

      // ---- helpers ----------------------------------------------------------------

      private static string MailboxRoute(string archive, string address, string suffix = "")
         => "/api/v1/backup/" + Uri.EscapeDataString(archive) + "/mailboxes/" + Uri.EscapeDataString(address) + suffix;

      private (int status, string body) Admin(string method, string path, string requestBody)
         => RestRequest.Send("127.0.0.1", RestPort, method, path, Basic("Administrator", AdminPassword), requestBody);

      private (int status, string body) As(string address, string method, string path, string requestBody)
         => RestRequest.Send("127.0.0.1", RestPort, method, path, Basic(address, Password), requestBody);

      private static string Basic(string user, string password)
         => "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(user + ":" + password));

      /// <summary>Starts a restore and waits for its job to finish; the outcome document.</summary>
      private JsonElement RestoreAndWait(string archive, string address, string body)
      {
         (int status, string answer) started = Admin("POST", MailboxRoute(archive, address, "/restore"), body);
         Assert.AreEqual(202, started.status, started.answer);

         int job;
         using (JsonDocument document = JsonDocument.Parse(started.answer))
         {
            job = document.RootElement.GetProperty("job").GetInt32();
            Assert.AreEqual("queued", document.RootElement.GetProperty("state").GetString());
            Assert.AreEqual("/api/v1/backup/restore-mailbox/" + job, document.RootElement.GetProperty("status").GetString());
         }

         return WaitForJob(job);
      }

      private JsonElement WaitForJob(int job)
      {
         for (int i = 0; i < 400; i++)
         {
            (int status, string body) polled = Admin("GET", "/api/v1/backup/restore-mailbox/" + job, null);
            Assert.AreEqual(200, polled.status, polled.body);

            JsonElement outcome = JsonDocument.Parse(polled.body).RootElement.Clone();
            string state = outcome.GetProperty("state").GetString();

            if (state == "done" || state == "failed")
               return outcome;

            Thread.Sleep(250);
         }

         Assert.Fail("The mailbox restore job " + job + " did not finish.");
         return default;
      }

      private static void AssertOutcome(JsonElement outcome, int folders, int added, int skipped, int failed)
      {
         string text = outcome.GetRawText();
         Assert.AreEqual("done", outcome.GetProperty("state").GetString(), text);
         Assert.AreEqual(folders, outcome.GetProperty("folders").GetInt32(), "folders: " + text);
         Assert.AreEqual(added, outcome.GetProperty("added").GetInt32(), "added: " + text);
         Assert.AreEqual(skipped, outcome.GetProperty("skipped").GetInt32(), "skipped: " + text);
         Assert.AreEqual(failed, outcome.GetProperty("failed").GetInt32(), "failed: " + text);
      }

      private static int Count(string address, string folder)
      {
         var imap = new ImapClientSimulator();
         Assert.IsTrue(imap.ConnectAndLogon(address, Password));
         int count = imap.GetMessageCount(folder);
         imap.Disconnect();
         return count;
      }

      /// <summary>The sequence numbers SEARCH answers for a subject, as text ("" when none).</summary>
      private static string Search(string address, string folder, string criteria)
      {
         var imap = new ImapClientSimulator();
         Assert.IsTrue(imap.ConnectAndLogon(address, Password));
         Assert.IsTrue(imap.SelectFolder(folder));
         string found = imap.Search(criteria).Trim();
         imap.Disconnect();
         return found;
      }

      private static void DeleteBySubject(string address, string folder, string subject)
      {
         var imap = new ImapClientSimulator();
         Assert.IsTrue(imap.ConnectAndLogon(address, Password));
         Assert.IsTrue(imap.SelectFolder(folder));
         string found = imap.Search("SUBJECT \"" + subject + "\"").Trim();
         Assert.IsNotEmpty(found, "No message with the subject " + subject + " to delete.");
         Assert.IsTrue(imap.SetFlagOnMessage(int.Parse(found.Split(' ')[0]), true, "\\Deleted"));
         Assert.IsTrue(imap.Expunge());
         imap.Disconnect();
      }

      private static long MessageIdInListing(string listing, string subject)
      {
         using (JsonDocument document = JsonDocument.Parse(listing))
         {
            foreach (JsonElement folder in document.RootElement.GetProperty("folders").EnumerateArray())
            {
               foreach (JsonElement message in folder.GetProperty("messages").EnumerateArray())
               {
                  if (message.GetProperty("subject").GetString() == subject)
                     return message.GetProperty("id").GetInt64();
               }
            }
         }

         Assert.Fail("The listing has no message with the subject " + subject + ": " + listing);
         return 0;
      }

      // ---- the tests ----------------------------------------------------------------

      [Test]
      [Description("Merge brings back two deleted messages and a deleted folder with their flags and keyword, leaves the other account and the message that was still there alone, and merging the same archive again adds nothing.")]
      public void MergeBringsBackWhatWasDeletedAndLeavesTheRestAlone()
      {
         Populate();
         string archive = TakeBackup();

         DeleteBySubject(_ann.Address, "INBOX", "second");
         DeleteBySubject(_ann.Address, "INBOX", "third");

         var imap = new ImapClientSimulator();
         Assert.IsTrue(imap.ConnectAndLogon(_ann.Address, Password));
         Assert.IsTrue(imap.DeleteFolder("Projects.Alpha"));
         imap.Disconnect();

         // The state before: the negative control for every count below.
         Assert.AreEqual(1, Count(_ann.Address, "INBOX"));
         Assert.AreEqual(2, Count(_bob.Address, "INBOX"));

         JsonElement outcome = RestoreAndWait(archive, _ann.Address, "{\"mode\":\"merge\"}");

         // INBOX, Projects and Projects.Alpha were written to; second and third
         // came back to INBOX and to Alpha, first was still there.
         AssertOutcome(outcome, folders: 3, added: 4, skipped: 1, failed: 0);
         Assert.AreEqual("merge", outcome.GetProperty("mode").GetString());

         Assert.AreEqual(3, Count(_ann.Address, "INBOX"));
         Assert.AreEqual(2, Count(_ann.Address, "Projects.Alpha"));
         Assert.AreEqual(2, Count(_bob.Address, "INBOX"), "The other account must not be touched.");

         // The flags and the keyword came back with the message that was still
         // there untouched, and the restored ones as the archive held them.
         imap = new ImapClientSimulator();
         Assert.IsTrue(imap.ConnectAndLogon(_ann.Address, Password));
         Assert.IsTrue(imap.SelectFolder("INBOX"));
         string first = imap.Search("SUBJECT \"first\"").Trim();
         string flags = imap.GetFlags(int.Parse(first));
         StringAssert.Contains("\\Flagged", flags);
         StringAssert.Contains("$label1", flags);
         string second = imap.Search("SUBJECT \"second\"").Trim();
         Assert.IsNotEmpty(second, "second must be back in INBOX.");
         StringAssert.DoesNotContain("\\Flagged", imap.GetFlags(int.Parse(second)), "second was never flagged.");
         imap.Disconnect();

         // Merged again: every file is now in the live mailbox, so nothing is
         // added and nothing is duplicated.
         JsonElement again = RestoreAndWait(archive, _ann.Address, "{\"mode\":\"merge\"}");
         AssertOutcome(again, folders: 3, added: 0, skipped: 5, failed: 0);
         Assert.AreEqual(3, Count(_ann.Address, "INBOX"));
         Assert.AreEqual(2, Count(_ann.Address, "Projects.Alpha"));
      }

      [Test]
      [Description("Replace after new mail has arrived empties the live folders and refills them, so the mailbox is the archive's and the new mail is gone; the other account is untouched.")]
      public void ReplaceMakesTheMailboxTheArchivesAndTheNewMailIsGone()
      {
         Populate();
         string archive = TakeBackup();

         new SmtpClientSimulator().Send("sender@elsewhere.test", _ann.Address, "arrived after", "Came after the backup.");
         Pop3ClientSimulator.AssertMessageCount(_ann.Address, Password, 4);
         DeleteBySubject(_ann.Address, "INBOX", "first");
         Assert.AreEqual(3, Count(_ann.Address, "INBOX"));

         JsonElement outcome = RestoreAndWait(archive, _ann.Address, "{\"mode\":\"replace\"}");

         // Every archived message is added, since every live folder was emptied first.
         AssertOutcome(outcome, folders: 3, added: 5, skipped: 0, failed: 0);
         Assert.AreEqual("replace", outcome.GetProperty("mode").GetString());

         Assert.AreEqual(3, Count(_ann.Address, "INBOX"));
         Assert.AreEqual(2, Count(_ann.Address, "Projects.Alpha"));
         Assert.IsEmpty(Search(_ann.Address, "INBOX", "SUBJECT \"arrived after\""), "Mail that arrived since the backup must be gone after replace.");
         Assert.IsNotEmpty(Search(_ann.Address, "INBOX", "SUBJECT \"first\""), "first must be back.");
         Assert.AreEqual(2, Count(_bob.Address, "INBOX"), "The other account must not be touched.");
      }

      [Test]
      [Description("One message is put back into the folder it was in, and another into a named folder that is created for it; the listing names each message by an id the restore takes, with its folder, subject, sender, date and size.")]
      public void OneMessageIntoItsOwnFolderAndIntoANamedOne()
      {
         Populate();
         string archive = TakeBackup();

         (int status, string body) listed = Admin("GET", MailboxRoute(archive, _ann.Address), null);
         Assert.AreEqual(200, listed.status, listed.body);

         using (JsonDocument document = JsonDocument.Parse(listed.body))
         {
            JsonElement root = document.RootElement;
            Assert.AreEqual(_ann.Address, root.GetProperty("address").GetString());
            StringAssert.EndsWith(archive, root.GetProperty("archive").GetString());
            Assert.AreEqual(5, root.GetProperty("messages").GetInt32(), listed.body);
            Assert.IsTrue(root.GetProperty("subjects").GetBoolean(), "Subjects are read for a small listing: " + listed.body);
            Assert.AreEqual(3, root.GetProperty("folders").GetArrayLength(), listed.body);

            JsonElement inbox = root.GetProperty("folders")[0];
            Assert.AreEqual("INBOX", inbox.GetProperty("path").GetString());
            JsonElement message = inbox.GetProperty("messages")[0];
            foreach (string field in new[] { "id", "folder", "subject", "from", "date", "size", "flags", "keywords" })
               Assert.IsTrue(message.TryGetProperty(field, out _), "The listing's message lacks " + field + ": " + listed.body);
            Assert.AreEqual("sender@elsewhere.test", message.GetProperty("from").GetString());
            Assert.Greater(message.GetProperty("size").GetInt32(), 0);
         }

         // The folder filter narrows the listing to one folder.
         (int status, string body) filtered = Admin("GET", MailboxRoute(archive, _ann.Address) + "?folder=Projects.Alpha", null);
         Assert.AreEqual(200, filtered.status, filtered.body);
         using (JsonDocument document = JsonDocument.Parse(filtered.body))
         {
            Assert.AreEqual(1, document.RootElement.GetProperty("folders").GetArrayLength(), filtered.body);
            Assert.AreEqual("Projects.Alpha", document.RootElement.GetProperty("folders")[0].GetProperty("path").GetString());
         }

         long secondId = MessageIdInListing(listed.body, "second");
         long thirdId = MessageIdInListing(listed.body, "third");

         DeleteBySubject(_ann.Address, "INBOX", "second");
         DeleteBySubject(_ann.Address, "INBOX", "third");
         Assert.AreEqual(1, Count(_ann.Address, "INBOX"));

         // Into the folder it was in.
         JsonElement own = RestoreAndWait(archive, _ann.Address, "{\"message\":" + secondId + "}");
         AssertOutcome(own, folders: 1, added: 1, skipped: 0, failed: 0);
         Assert.AreEqual("message", own.GetProperty("mode").GetString());
         Assert.AreEqual(secondId, own.GetProperty("message").GetInt64());
         Assert.AreEqual(2, Count(_ann.Address, "INBOX"));
         Assert.IsNotEmpty(Search(_ann.Address, "INBOX", "SUBJECT \"second\""));

         // Into a named folder, which is made for it.
         JsonElement named = RestoreAndWait(archive, _ann.Address, "{\"message\":" + thirdId + ",\"folder\":\"Restored\"}");
         AssertOutcome(named, folders: 1, added: 1, skipped: 0, failed: 0);
         Assert.AreEqual(1, Count(_ann.Address, "Restored"));
         Assert.IsNotEmpty(Search(_ann.Address, "Restored", "SUBJECT \"third\""));
         Assert.AreEqual(2, Count(_ann.Address, "INBOX"), "third went to Restored, not to INBOX.");

         // The same message again is still in the mailbox - in Restored - so it
         // is skipped rather than duplicated, wherever it is asked for.
         JsonElement again = RestoreAndWait(archive, _ann.Address, "{\"message\":" + thirdId + "}");
         AssertOutcome(again, folders: 1, added: 0, skipped: 1, failed: 0);
         Assert.AreEqual(2, Count(_ann.Address, "INBOX"));
      }

      [Test]
      [Description("An account the archive does not hold is 404, an account this server does not have is 404, a path outside the backup destination is 400, an archive that is not in the destination is 404, a mode that is not merge or replace is 400, an unknown message is 404, and a restore while a backup runs is 409 - each changing nothing.")]
      public void RefusalsChangeNothing()
      {
         Populate();
         string archive = TakeBackup();

         // Nobody of that name in the archive, and nobody of that name here.
         Assert.AreEqual(404, Admin("GET", MailboxRoute(archive, "nobody@" + _domain.Name), null).status);
         Assert.AreEqual(404, Admin("POST", MailboxRoute(archive, "nobody@" + _domain.Name, "/restore"), "{\"mode\":\"merge\"}").status);

         // In the archive, but deleted here since: not recreated.
         _bob.Delete();
         (int status, string body) gone = Admin("POST", MailboxRoute(archive, "bob@" + _domain.Name, "/restore"), "{\"mode\":\"merge\"}");
         Assert.AreEqual(404, gone.status, gone.body);
         StringAssert.Contains("no account", gone.body);

         // A path outside the destination, whatever it points at.
         string outside = Path.Combine(Path.GetTempPath(), "hm-outside-" + TestSetup.UniqueString() + ".7z");
         File.Copy(Path.Combine(_backupDir, archive), outside);
         try
         {
            (int status, string body) refused = Admin("POST", MailboxRoute(outside, _ann.Address, "/restore"), "{\"mode\":\"merge\"}");
            Assert.AreEqual(400, refused.status, refused.body);
            StringAssert.Contains("not inside the backup destination", refused.body);
            Assert.AreEqual(400, Admin("GET", MailboxRoute(outside, _ann.Address), null).status);
         }
         finally
         {
            File.Delete(outside);
         }

         // A name the destination does not hold.
         Assert.AreEqual(404, Admin("POST", MailboxRoute("HMBackup 2000-01-01 000000.7z", _ann.Address, "/restore"), "{\"mode\":\"merge\"}").status);

         // The request itself.
         (int status, string body) badMode = Admin("POST", MailboxRoute(archive, _ann.Address, "/restore"), "{\"mode\":\"overwrite\"}");
         Assert.AreEqual(400, badMode.status, badMode.body);
         Assert.AreEqual(400, Admin("POST", MailboxRoute(archive, _ann.Address, "/restore"), "{\"file\":\"x\"}").status);
         Assert.AreEqual(404, Admin("POST", MailboxRoute(archive, _ann.Address, "/restore"), "{\"message\":987654321}").status);
         Assert.AreEqual(400, Admin("POST", MailboxRoute(archive, _ann.Address, "/restore"), "{\"folder\":\"Restored\"}").status,
            "folder without message names nowhere for a whole mailbox to go.");
         Assert.AreEqual(404, Admin("GET", "/api/v1/backup/restore-mailbox/987654", null).status);

         // Nothing changed.
         Assert.AreEqual(3, Count(_ann.Address, "INBOX"));

         // A backup holds the one run slot; a restore asked for meanwhile is 409.
         _application.BackupManager.StartBackup();
         (int status, string body) busy = Admin("POST", MailboxRoute(archive, _ann.Address, "/restore"), "{\"mode\":\"merge\"}");
         Assert.AreEqual(409, busy.status, busy.body);
         StringAssert.Contains("already running", busy.body);

         // Let the backup finish before the next test empties the destination.
         for (int i = 0; i < 240; i++)
         {
            try
            {
               string contents = TestSetup.ReadExistingTextFile(_application.Settings.Backup.LogFile);
               if (contents.IndexOf("Backup completed successfully", StringComparison.Ordinal) != contents.LastIndexOf("Backup completed successfully", StringComparison.Ordinal))
                  break;
            }
            catch (IOException)
            {
            }

            Thread.Sleep(250);
         }
      }

      [Test]
      [Description("A search finds a restored message by a word in its body, and an IMAP session with the folder selected is told about the additions on its next command.")]
      public void TheSearchFindsARestoredMessageAndASelectedSessionSeesIt()
      {
         Populate();
         string archive = TakeBackup();

         DeleteBySubject(_ann.Address, "INBOX", "first");
         Assert.IsEmpty(Search(_ann.Address, "INBOX", "TEXT \"" + Needle + "\""), "The negative control: the word is gone with the message.");

         var session = new ImapClientSimulator();
         Assert.IsTrue(session.ConnectAndLogon(_ann.Address, Password));
         Assert.IsTrue(session.SelectFolder("INBOX"));
         Assert.AreEqual(2, session.GetMessageCount("INBOX"));

         JsonElement outcome = RestoreAndWait(archive, _ann.Address, "{\"mode\":\"merge\"}");
         AssertOutcome(outcome, folders: 3, added: 1, skipped: 4, failed: 0);

         // The session that had the folder selected the whole time.
         string noop = session.SendSingleCommand("NOOP");
         StringAssert.Contains("3 EXISTS", noop, "The selected session must be told about the addition: " + noop);
         session.Disconnect();

         Assert.IsNotEmpty(Search(_ann.Address, "INBOX", "TEXT \"" + Needle + "\""), "The search must find the restored message.");
      }

      [Test]
      [Description("The COM round trip: DescribeMailbox lists the archive's messages as the REST route does, RestoreMailbox and RestoreMessage queue jobs whose outcome MailboxRestoreStatus answers, and an archive outside the destination is refused with the reason.")]
      public void TheComRoundTrip()
      {
         Populate();
         string archive = TakeBackup();

         DeleteBySubject(_ann.Address, "INBOX", "second");
         DeleteBySubject(_ann.Address, "INBOX", "third");
         Assert.AreEqual(1, Count(_ann.Address, "INBOX"));

         dynamic manager = _application.BackupManager;

         string listing = (string) manager.DescribeMailbox(archive, _ann.Address, "");
         using (JsonDocument document = JsonDocument.Parse(listing))
         {
            Assert.AreEqual(5, document.RootElement.GetProperty("messages").GetInt32(), listing);
            Assert.AreEqual(_ann.Address, document.RootElement.GetProperty("address").GetString());
         }
         long thirdId = MessageIdInListing(listing, "third");

         int job = (int) manager.RestoreMessage(archive, _ann.Address, thirdId, "", "");
         Assert.Greater(job, 0);
         JsonElement one = WaitForJob(job);
         AssertOutcome(one, folders: 1, added: 1, skipped: 0, failed: 0);
         Assert.AreEqual(2, Count(_ann.Address, "INBOX"));

         string status = (string) manager.MailboxRestoreStatus(job);
         using (JsonDocument document = JsonDocument.Parse(status))
         {
            Assert.AreEqual("done", document.RootElement.GetProperty("state").GetString(), status);
            Assert.AreEqual(job, document.RootElement.GetProperty("job").GetInt32());
         }

         int merge = (int) manager.RestoreMailbox(archive, _ann.Address, "merge", "");
         JsonElement whole = WaitForJob(merge);
         AssertOutcome(whole, folders: 3, added: 1, skipped: 4, failed: 0);
         Assert.AreEqual(3, Count(_ann.Address, "INBOX"));

         // Refused with the reason, before anything is touched.
         string outside = Path.Combine(Path.GetTempPath(), "hm-outside-" + TestSetup.UniqueString() + ".7z");
         File.Copy(Path.Combine(_backupDir, archive), outside);
         try
         {
            var refused = Assert.Throws<System.Runtime.InteropServices.COMException>(() => manager.RestoreMailbox(outside, _ann.Address, "merge", ""));
            StringAssert.Contains("not inside the backup destination", refused.Message);
         }
         finally
         {
            File.Delete(outside);
         }

         Assert.Throws<System.Runtime.InteropServices.COMException>(() => manager.RestoreMailbox(archive, _ann.Address, "overwrite", ""));
         Assert.Throws<System.Runtime.InteropServices.COMException>(() => manager.MailboxRestoreStatus(987654));
         Assert.AreEqual(3, Count(_ann.Address, "INBOX"));
      }

      [Test]
      [Description("An auditor may read the archives and a mailbox out of one and may not restore; a help desk may read neither; a server administrator may do all of it.")]
      public void TheRolesAreTheRestoresOwn()
      {
         Populate();
         string archive = TakeBackup();

         Account audrey = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "audrey@" + _domain.Name, Password);
         Account helen = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "helen@" + _domain.Name, Password);
         ((dynamic) _application).AssignRole(audrey.Address, "auditor", "");
         ((dynamic) _application).AssignRole(helen.Address, "helpdesk", "");

         Assert.AreEqual(200, As(audrey.Address, "GET", "/api/v1/backup/archives", null).status);
         Assert.AreEqual(200, As(audrey.Address, "GET", MailboxRoute(archive, _ann.Address), null).status);
         (int status, string body) refused = As(audrey.Address, "POST", MailboxRoute(archive, _ann.Address, "/restore"), "{\"mode\":\"merge\"}");
         Assert.AreEqual(403, refused.status, refused.body);

         Assert.AreEqual(403, As(helen.Address, "GET", MailboxRoute(archive, _ann.Address), null).status, "A help desk does not read mailboxes.");
         Assert.AreEqual(403, As(helen.Address, "POST", MailboxRoute(archive, _ann.Address, "/restore"), "{\"mode\":\"merge\"}").status);

         // Nothing was changed by the refusals, and the administrator still may.
         Assert.AreEqual(3, Count(_ann.Address, "INBOX"));
         JsonElement outcome = RestoreAndWait(archive, _ann.Address, "{\"mode\":\"merge\"}");
         AssertOutcome(outcome, folders: 3, added: 0, skipped: 5, failed: 0);
      }
   }
}
