// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
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
      private Account _carl;

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
            // The passphrase one test sets, and the ini key another sets, taken
            // back whatever the test did; the write-only passphrase is cleared
            // by an empty one.
            dynamic backupSettings = _application.Settings.Backup;
            backupSettings.EncryptionPassphrase = "";
            IniFileSetting.Delete("BackupMessagesDBOnly");

            RestListener.Stop();
            _application.Reinitialize();
         }
         finally
         {
            // Every failure a test here provokes on purpose is reported as
            // HM5014 and consumed by the test; this is the net for a test that
            // threw before it could, so that the next fixture's clean-log check
            // does not fail on this one's doing.
            LogHandler.DeleteErrorLog();

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
      ///    copies of second and third in Projects.Alpha; bob has two of his own;
      ///    carl has nothing at all.
      /// </summary>
      private void Populate()
      {
         _ann = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ann@" + _domain.Name, Password);
         _bob = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "bob@" + _domain.Name, Password);
         _carl = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "carl@" + _domain.Name, Password);

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

      private string TakeBackup(bool encrypted = false)
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
            Assert.AreEqual(encrypted, newest.GetProperty("encrypted").GetBoolean(), "The listing reads whether an archive is encrypted from its own header: " + listed.body);
            Assert.Greater(newest.GetProperty("size").GetInt64(), 0L, "The archive's size is read from the file: " + listed.body);
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

      /// <summary>The listing with an encrypted archive's passphrase in the one header a GET has for it.</summary>
      private (int status, string body) AdminWithPassphrase(string path, string passphrase)
         => RestRequest.Send("127.0.0.1", RestPort, "GET", path, Basic("Administrator", AdminPassword), null, "X-hMailServer-Backup-Passphrase: " + passphrase);

      private string BackupLog()
      {
         try
         {
            return TestSetup.ReadExistingTextFile(_application.Settings.Backup.LogFile);
         }
         catch (IOException)
         {
            return "";
         }
      }

      /// <summary>
      ///    A copy of the archive under a name the listing recognises, with its
      ///    index edited: the way to make an archive say what the backup never
      ///    would - a file the store lacks, a name the executer never writes,
      ///    five hundred rows too many.
      /// </summary>
      private string EditedArchive(string archive, Func<string, string> edit)
      {
         const string copyName = "HMBackup 2000-01-01 000000.7z";
         string copy = Path.Combine(_backupDir, copyName);
         File.Copy(Path.Combine(_backupDir, archive), copy, true);

         string staging = Path.Combine(_backupDir, "edited");
         Directory.CreateDirectory(staging);
         Run7Za("e \"" + copy + "\" hMailServerBackup.xml -o\"" + staging + "\" -y", staging);
         string index = Path.Combine(staging, "hMailServerBackup.xml");
         Assert.IsTrue(File.Exists(index), "7za.exe did not extract the backup's index.");

         // settings-store-exempt: reads and rewrites the backup archive's index, not hMailServer.ini.
         string xml = edit(File.ReadAllText(index));
         // UTF-16 with a byte-order mark, which is what the server writes the index in.
         // settings-store-exempt: writes the backup archive's index, not hMailServer.ini.
         File.WriteAllText(index, xml, Encoding.Unicode);
         File.SetLastWriteTimeUtc(index, DateTime.UtcNow.AddMinutes(1));
         Run7Za("u \"" + copy + "\" hMailServerBackup.xml", staging);
         Directory.Delete(staging, true);

         return copyName;
      }

      private void Run7Za(string arguments, string workingDirectory)
      {
         string programDirectory = _application.Settings.Directories.ProgramDirectory;
         string[] candidates =
         {
            Path.Combine(programDirectory, "7za.exe"),
            Path.Combine(programDirectory, "Bin", "7za.exe"),
         };
         string sevenZip = candidates.FirstOrDefault(File.Exists);
         if (sevenZip == null)
            Assert.Fail("Could not find 7za.exe next to the server. Looked in " + string.Join(" and ", candidates) + ".");

         var startInfo = new ProcessStartInfo(sevenZip, arguments)
         {
            WorkingDirectory = workingDirectory,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
         };

         using (var process = Process.Start(startInfo))
         {
            Assert.IsNotNull(process, "7za.exe could not be started.");
            string output = process.StandardOutput.ReadToEnd();
            process.WaitForExit();
            Assert.AreEqual(0, process.ExitCode, "7za.exe " + arguments + " failed: " + output);
         }
      }

      /// <summary>The index from ann's account element on: where her rows are edited.</summary>
      private static int AnnsAccountAt(string xml, string address)
      {
         int at = xml.IndexOf("<Account Name=\"" + address + "\"", StringComparison.OrdinalIgnoreCase);
         Assert.Greater(at, 0, "The index has no account element for " + address);
         return at;
      }

      private static string ReplaceNthFilenameAfter(string xml, int from, int nth, string replacement)
      {
         var filename = new Regex("Filename=\"[^\"]*\"");
         Match match = filename.Match(xml, from);
         for (int i = 0; i < nth; i++)
         {
            Assert.IsTrue(match.Success, "The index has fewer Filename attributes than expected after " + from);
            match = match.NextMatch();
         }
         Assert.IsTrue(match.Success, "The index has fewer Filename attributes than expected after " + from);
         return xml.Substring(0, match.Index) + "Filename=\"" + replacement + "\"" + xml.Substring(match.Index + match.Length);
      }

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

         // Over REST the archive is a NAME and nothing else: a path percent-encodes
         // its separators, the route does not decode them, and what arrives is a
         // name the destination lacks - 404, and nothing opened. (The outside-path
         // refusal itself is proved over COM, in TheComRoundTrip, which is the
         // one interface that takes a path.)
         string outside = Path.Combine(Path.GetTempPath(), "hm-outside-" + TestSetup.UniqueString() + ".7z");
         File.Copy(Path.Combine(_backupDir, archive), outside);
         try
         {
            (int status, string body) refused = Admin("POST", MailboxRoute(outside, _ann.Address, "/restore"), "{\"mode\":\"merge\"}");
            Assert.AreEqual(404, refused.status, refused.body);
            StringAssert.Contains("no archive", refused.body);
            Assert.AreEqual(404, Admin("GET", MailboxRoute(outside, _ann.Address), null).status);
         }
         finally
         {
            File.Delete(outside);
         }

         // One message takes no mode, and replace of a mailbox the archive holds
         // nothing for would only empty it.
         (int status, string body) modeAndMessage = Admin("POST", MailboxRoute(archive, _ann.Address, "/restore"), "{\"mode\":\"merge\",\"message\":1}");
         Assert.AreEqual(400, modeAndMessage.status, modeAndMessage.body);
         StringAssert.Contains("does not apply to a single message", modeAndMessage.body);
         (int status, string body) emptyReplace = Admin("POST", MailboxRoute(archive, _carl.Address, "/restore"), "{\"mode\":\"replace\"}");
         Assert.AreEqual(400, emptyReplace.status, emptyReplace.body);
         StringAssert.Contains("would only empty the mailbox", emptyReplace.body);
         Assert.AreEqual(0, Count(_carl.Address, "INBOX"));

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
            string contents = BackupLog();
            if (contents.IndexOf("Backup completed successfully", StringComparison.Ordinal) != contents.LastIndexOf("Backup completed successfully", StringComparison.Ordinal))
               break;

            Thread.Sleep(250);
         }

         // The job the refused request registered reads failed with that reason,
         // not queued: it is the one numbered just before the next job started.
         JsonElement next = RestoreAndWait(archive, _ann.Address, "{\"mode\":\"merge\"}");
         int refusedJob = next.GetProperty("job").GetInt32() - 1;
         (int status, string body) phantom = Admin("GET", "/api/v1/backup/restore-mailbox/" + refusedJob, null);
         Assert.AreEqual(200, phantom.status, phantom.body);
         using (JsonDocument document = JsonDocument.Parse(phantom.body))
         {
            Assert.AreEqual("failed", document.RootElement.GetProperty("state").GetString(), phantom.body);
            StringAssert.Contains("already running", document.RootElement.GetProperty("error").GetString());
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
         string selected;
         Assert.IsTrue(session.SelectFolder("INBOX", out selected));
         Match uidNextMatch = Regex.Match(selected, @"UIDNEXT (\d+)");
         Assert.IsTrue(uidNextMatch.Success, "SELECT did not answer UIDNEXT: " + selected);
         int uidNextBefore = int.Parse(uidNextMatch.Groups[1].Value);
         Assert.AreEqual(2, session.GetMessageCount("INBOX"));

         JsonElement outcome = RestoreAndWait(archive, _ann.Address, "{\"mode\":\"merge\"}");
         AssertOutcome(outcome, folders: 3, added: 1, skipped: 4, failed: 0);

         // The session that had the folder selected the whole time.
         string noop = session.SendSingleCommand("NOOP");
         StringAssert.Contains("3 EXISTS", noop, "The selected session must be told about the addition: " + noop);

         // A fresh UID, above what the folder had promised: a client with a
         // cached folder sees a new message, never a reused number.
         string uidSearch = session.SendSingleCommand("UID SEARCH SUBJECT \"first\"");
         Match uidMatch = Regex.Match(uidSearch, @"\* SEARCH (\d+)");
         Assert.IsTrue(uidMatch.Success, "UID SEARCH did not find the restored message: " + uidSearch);
         Assert.GreaterOrEqual(int.Parse(uidMatch.Groups[1].Value), uidNextBefore, "The restored message's UID must be at or above the folder's earlier UIDNEXT: " + uidSearch);
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
      [Description("An index naming a file the store lacks, and a row whose file name is not the backup's own: the listing leaves the unsound row out and counts it, a merge counts both as failed with the log lines and goes on, the request is on the audit trail, and a replace from the same archive ends failed with the BACKUP ERROR line because it emptied more than it could put back.")]
      public void AJobThatFailsAfterQueuingIsReportedAndAReplaceThatCannotRefillFails()
      {
         Populate();
         string archive = TakeBackup();

         string edited = EditedArchive(archive, xml =>
         {
            int at = AnnsAccountAt(xml, _ann.Address);
            xml = ReplaceNthFilenameAfter(xml, at, 0, "{00000000-0000-0000-0000-000000000000}.eml");
            return ReplaceNthFilenameAfter(xml, at, 1, "..\\evil.eml");
         });

         // The listing: four rows it can take and one it cannot.
         (int status, string body) listed = Admin("GET", MailboxRoute(edited, _ann.Address), null);
         Assert.AreEqual(200, listed.status, listed.body);
         using (JsonDocument document = JsonDocument.Parse(listed.body))
         {
            Assert.AreEqual(4, document.RootElement.GetProperty("messages").GetInt32(), listed.body);
            Assert.AreEqual(1, document.RootElement.GetProperty("unrestorable").GetInt32(), listed.body);
         }
         StringAssert.DoesNotContain("evil", listed.body, "A name the backup never writes must not be listed.");

         // Merge: the missing file and the unsound row are failed, the three that
         // are still in the mailbox are skipped, nothing is written under a name
         // the backup never made.
         JsonElement merged = RestoreAndWait(edited, _ann.Address, "{\"mode\":\"merge\"}");
         AssertOutcome(merged, folders: 3, added: 0, skipped: 3, failed: 2);
         Assert.AreEqual(3, Count(_ann.Address, "INBOX"));
         string log = BackupLog();
         StringAssert.Contains("whose file the archive does not hold", log);
         StringAssert.Contains("not one this server's backup writes", log);
         Assert.IsFalse(File.Exists(Path.Combine(_application.Settings.Directories.DataDirectory, "evil.eml")), "Nothing may be written from an unsound name.");

         // The request is on the audit trail, by whoever asked.
         (int status, string body) trail = Admin("GET", "/api/v1/audit?action=mailbox%20restore%20started&limit=1", null);
         Assert.AreEqual(200, trail.status, trail.body);
         StringAssert.Contains(_ann.Address, trail.body, "The mailbox restore request must be on the audit trail: " + trail.body);
         StringAssert.Contains("merging", trail.body, trail.body);

         // Replace: the folders are emptied, two rows cannot come back, and that
         // is a failed job with the whole restore's report.
         JsonElement replaced = RestoreAndWait(edited, _ann.Address, "{\"mode\":\"replace\"}");
         Assert.AreEqual("failed", replaced.GetProperty("state").GetString(), replaced.GetRawText());
         Assert.AreEqual(3, replaced.GetProperty("added").GetInt32(), replaced.GetRawText());
         Assert.AreEqual(2, replaced.GetProperty("failed").GetInt32(), replaced.GetRawText());
         StringAssert.Contains("could not put back 2", replaced.GetProperty("error").GetString());
         Assert.AreEqual(1, Count(_ann.Address, "INBOX"), "Only the sound INBOX row came back.");
         Assert.AreEqual(2, Count(_ann.Address, "Projects.Alpha"));
         StringAssert.Contains("BACKUP ERROR: Mailbox restore", BackupLog());
         CustomAsserts.AssertReportedError("HM5014");
      }

      [Test]
      [Description("An encrypted archive: the listing refuses without the passphrase and with a wrong one in the server's own words, answers with the right one in the header, and the restore takes the passphrase in its body.")]
      public void AnEncryptedArchiveNeedsItsPassphraseInTheHeaderAndTheBody()
      {
         const string passphrase = "correct horse battery staple";
         Populate();

         dynamic backupSettings = _application.Settings.Backup;
         backupSettings.EncryptionPassphrase = passphrase;
         string archive = TakeBackup(encrypted: true);

         (int status, string body) bare = Admin("GET", MailboxRoute(archive, _ann.Address), null);
         Assert.AreEqual(400, bare.status, bare.body);
         StringAssert.Contains("is encrypted, and no passphrase was given", bare.body);

         (int status, string body) wrong = AdminWithPassphrase(MailboxRoute(archive, _ann.Address), "not the passphrase");
         Assert.AreEqual(400, wrong.status, wrong.body);
         StringAssert.Contains("could not be opened with the passphrase given", wrong.body);

         (int status, string body) right = AdminWithPassphrase(MailboxRoute(archive, _ann.Address), passphrase);
         Assert.AreEqual(200, right.status, right.body);
         using (JsonDocument document = JsonDocument.Parse(right.body))
         {
            Assert.AreEqual(5, document.RootElement.GetProperty("messages").GetInt32(), right.body);
            Assert.IsTrue(document.RootElement.GetProperty("subjects").GetBoolean(), "Subjects come from the staged files, opened with the passphrase: " + right.body);
         }

         DeleteBySubject(_ann.Address, "INBOX", "second");
         Assert.AreEqual(400, Admin("POST", MailboxRoute(archive, _ann.Address, "/restore"), "{\"mode\":\"merge\"}").status);
         Assert.AreEqual(400, Admin("POST", MailboxRoute(archive, _ann.Address, "/restore"), "{\"mode\":\"merge\",\"passphrase\":\"not the passphrase\"}").status);
         Assert.AreEqual(2, Count(_ann.Address, "INBOX"), "A refused restore changes nothing.");

         JsonElement outcome = RestoreAndWait(archive, _ann.Address, "{\"mode\":\"merge\",\"passphrase\":\"" + passphrase + "\"}");
         AssertOutcome(outcome, folders: 3, added: 1, skipped: 4, failed: 0);
         Assert.AreEqual(3, Count(_ann.Address, "INBOX"));
      }

      [Test]
      [Description("An archive taken with BackupMessagesDBOnly holds rows and no files: the listing reads no subjects and says why, and a restore from it fails with the reason rather than emptying or writing anything.")]
      public void AnArchiveWithRowsAndNoFilesCannotRestoreAMailbox()
      {
         IniFileSetting.Write("BackupMessagesDBOnly", "1");
         _application.Reinitialize();

         Populate();
         string archive = TakeBackup();

         (int status, string body) listed = Admin("GET", MailboxRoute(archive, _ann.Address), null);
         Assert.AreEqual(200, listed.status, listed.body);
         using (JsonDocument document = JsonDocument.Parse(listed.body))
         {
            Assert.AreEqual(5, document.RootElement.GetProperty("messages").GetInt32(), listed.body);
            Assert.IsFalse(document.RootElement.GetProperty("subjects").GetBoolean(), listed.body);
            StringAssert.Contains("no message files", document.RootElement.GetProperty("subjects_note").GetString());
         }

         DeleteBySubject(_ann.Address, "INBOX", "second");
         JsonElement outcome = RestoreAndWait(archive, _ann.Address, "{\"mode\":\"replace\"}");
         Assert.AreEqual("failed", outcome.GetProperty("state").GetString(), outcome.GetRawText());
         StringAssert.Contains("BackupMessagesDBOnly", outcome.GetProperty("error").GetString());
         Assert.AreEqual(2, Count(_ann.Address, "INBOX"), "Nothing was emptied: the refusal came before the deletion.");
         CustomAsserts.AssertReportedError("HM5014");
      }

      [Test]
      [Description("Subjects are read for a listing of at most 500 messages: an index with more is listed without them and says so, and narrowing to one folder brings them back.")]
      public void SubjectsAreWithheldPastTheCeiling()
      {
         Populate();
         string archive = TakeBackup();

         string edited = EditedArchive(archive, xml =>
         {
            int at = AnnsAccountAt(xml, _ann.Address);
            Match first = new Regex("<Message\\b[^>]*/>").Match(xml, at);
            Assert.IsTrue(first.Success, "The index has no message element for the account.");
            var copies = new StringBuilder();
            for (int i = 0; i < 500; i++)
               copies.Append(Regex.Replace(first.Value, "ID=\"\\d+\"", "ID=\"" + (900001 + i) + "\""));
            return xml.Substring(0, first.Index + first.Length) + copies + xml.Substring(first.Index + first.Length);
         });

         (int status, string body) listed = Admin("GET", MailboxRoute(edited, _ann.Address), null);
         Assert.AreEqual(200, listed.status, listed.body);
         using (JsonDocument document = JsonDocument.Parse(listed.body))
         {
            Assert.AreEqual(505, document.RootElement.GetProperty("messages").GetInt32(), "messages: " + document.RootElement.GetProperty("messages").GetRawText());
            Assert.IsFalse(document.RootElement.GetProperty("subjects").GetBoolean(), "Past the ceiling no subject is read.");
            StringAssert.Contains("at most 500", document.RootElement.GetProperty("subjects_note").GetString());
         }

         (int status, string body) narrowed = Admin("GET", MailboxRoute(edited, _ann.Address) + "?folder=Projects.Alpha", null);
         Assert.AreEqual(200, narrowed.status, narrowed.body);
         using (JsonDocument document = JsonDocument.Parse(narrowed.body))
         {
            Assert.AreEqual(2, document.RootElement.GetProperty("messages").GetInt32(), narrowed.body);
            Assert.IsTrue(document.RootElement.GetProperty("subjects").GetBoolean(), "One folder under the ceiling is listed with subjects: " + narrowed.body);
         }
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
