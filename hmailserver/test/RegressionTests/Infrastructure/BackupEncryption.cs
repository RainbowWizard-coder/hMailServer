// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;

namespace RegressionTests.Infrastructure
{
   /// <summary>
   ///    The encrypted backup: an archive of every mailbox on the server, written
   ///    to whatever destination an administrator typed - often a share, sometimes
   ///    a cloud mount - and encrypted as it is written.
   ///
   ///    What is proved here, rather than described:
   ///
   ///      * the archive cannot be LISTED without the passphrase. 7-Zip is run
   ///        against it directly, the way somebody who found the file would, and
   ///        the file names are not in its output - which is what -mhe=on buys
   ///        over encrypting the data alone;
   ///      * it restores with the passphrase, mail included;
   ///      * without one, or with a wrong one, every way in refuses - COM, the
   ///        REST route, and so the Deck and hmctl that use it - and refuses
   ///        BEFORE anything on the server is touched, which is the only property
   ///        that matters when the alternative is a server with nothing on it;
   ///      * the passphrase is in no read, no log and no archive. It is write-only
   ///        over COM and over REST by construction (there is no getter to call),
   ///        and this checks the places it could still leak into: the backup log,
   ///        the settings section the archive itself carries, and the group the
   ///        REST API answers;
   ///      * a backup with no passphrase is unchanged, and still restores without
   ///        one;
   ///      * and the one configuration that would write mail in the clear while
   ///        claiming to encrypt it - a passphrase with compression off, which
   ///        copies the message files beside the archive - refuses to run at all.
   ///
   ///    THE BENCH. This fixture clears the backup passphrase in SetUp and again
   ///    in TearDown: it is write-only, so a passphrase that was set here before
   ///    could not be put back, and the suite's bench has none. Every other backup
   ///    setting is snapshotted and restored.
   /// </summary>
   [TestFixture]
   public class BackupEncryption : TestFixtureBase
   {
      // Printable ASCII, no leading or trailing space, well past the twelve
      // characters the server insists on.
      private const string Passphrase = "correct horse battery staple";
      private const string WrongPassphrase = "not the passphrase at all";

      // Free block: 9130, 9131 and 9137 are the alert, remote-domain and
      // account-import fixtures'; this one takes 9142.
      private static int RestPort = 9142;
      private const string AdminPassword = "testar";

      private string _backupDirectory;

      private string _previousDestination;
      private bool _previousBackupDomains;
      private bool _previousBackupMessages;
      private bool _previousBackupSettings;
      private bool _previousCompress;

      [SetUp]
      public void SetUpBackupEncryption()
      {
         // The credential the REST half of this fixture presents. Established
         // here rather than assumed: whatever an earlier fixture left the
         // administrator password as, the Basic header below is one this test
         // made true, so a 401 can only ever mean the route refused it.
         _settings.SetAdministratorPassword(AdminPassword);

         _backupDirectory = Paths.Combine(Path.GetTempPath(), "hMailBackupEncryption-" + TestSetup.UniqueString());
         Directory.CreateDirectory(_backupDirectory);

         var backup = _application.Settings.Backup;

         _previousDestination = backup.Destination;
         _previousBackupDomains = backup.BackupDomains;
         _previousBackupMessages = backup.BackupMessages;
         _previousBackupSettings = backup.BackupSettings;
         _previousCompress = backup.CompressDestinationFiles;

         backup.Destination = _backupDirectory;
         backup.BackupDomains = true;
         backup.BackupMessages = true;
         backup.BackupSettings = true;
         backup.CompressDestinationFiles = true;

         // A known state: no passphrase, whatever the last test left.
         SetPassphrase("");

         CustomAsserts.AssertDeleteFile(backup.LogFile);
      }

      [TearDown]
      public void TearDownBackupEncryption()
      {
         SetPassphrase("");

         var backup = _application.Settings.Backup;

         backup.Destination = _previousDestination;
         backup.BackupDomains = _previousBackupDomains;
         backup.BackupMessages = _previousBackupMessages;
         backup.BackupSettings = _previousBackupSettings;
         backup.CompressDestinationFiles = _previousCompress;

         try
         {
            if (Directory.Exists(_backupDirectory))
               Directory.Delete(_backupDirectory, true);
         }
         catch (IOException)
         {
            // A temporary directory left behind is not worth failing a test over.
         }

         // ONE test here writes to the ERROR log, and it is not any of the refused
         // restores. A refused open is deliberately not an error: BackupManager::
         // LoadBackup logs to the backup log and the application log "and not
         // ReportError", because "pointing this at a file that is not a backup is
         // an ordinary mistake made by hand at a prompt, and a server that writes
         // to its ERROR log because somebody mistyped a path is a server whose
         // ERROR log stops meaning anything" - and COMError::GenerateError, which
         // turns the refusal into the HRESULT the test catches, writes nothing
         // anywhere. That is why the refusal test can assert AssertNoReportedError
         // straight after catching two of them, and it is the assertion that would
         // notice if that ever changed.
         //
         // The one that does is EncryptingWithoutCompression...: it provokes a real
         // failed backup run, which OnBackupFailed reports as HM5014, and the test
         // consumes it with AssertReportedError (which deletes the log in its own
         // finally). This is the safety net for the run where that test throws
         // before it gets there, so the error it provoked on purpose does not fail
         // whichever fixture runs next.
         LogHandler.DeleteErrorLog();
      }

      // ---- the four members this feature adds ------------------------------
      //
      // Reached late-bound, the way QuarantineDigests reaches the settings it
      // needs. The test project carries a COMReference, so it compiles against
      // whichever hMailServer type library is REGISTERED on this machine, and a
      // member added on a branch is not in that library until the registration
      // catches up - while IDispatch answers it as soon as the server binary is
      // rebuilt, which is what these tests run against. Everything older is
      // early-bound as the rest of the suite is.

      private void SetPassphrase(string passphrase)
      {
         dynamic backup = _application.Settings.Backup;
         backup.EncryptionPassphrase = passphrase;
      }

      private bool PassphraseIsSet()
      {
         dynamic backup = _application.Settings.Backup;
         return (bool) backup.EncryptionPassphraseSet;
      }

      private bool ArchiveIsEncrypted(string archive)
      {
         dynamic utilities = _application.Utilities;
         return (bool) utilities.BackupArchiveIsEncrypted(archive);
      }

      private dynamic OpenEncrypted(string archive, string passphrase)
      {
         dynamic manager = _application.BackupManager;
         return manager.LoadEncryptedBackup(archive, passphrase);
      }

      // ---- the bench -------------------------------------------------------

      private static Account DeliverTwoMessages(Domain domain)
      {
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(domain, "encrypted@" + domain.Name, "test");

         SmtpClientSimulator.StaticSend("sender@" + domain.Name, account.Address, "First", "The first message.");
         SmtpClientSimulator.StaticSend("sender@" + domain.Name, account.Address, "Second", "The second message.");
         Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 2);

         return account;
      }

      private string ReadBackupLog()
      {
         string logFile = _application.Settings.Backup.LogFile;

         try
         {
            if (!File.Exists(logFile))
               return "(no backup log)";

            // FileShare.ReadWrite: the server holds this log open.
            using (var stream = new FileStream(logFile, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
            using (var reader = new StreamReader(stream))
            {
               return reader.ReadToEnd();
            }
         }
         catch (IOException exception)
         {
            return "(backup log could not be read: " + exception.Message + ")";
         }
      }

      private bool WaitForBackupLog(string expected, int timeoutSeconds)
      {
         for (int attempt = 0; attempt < timeoutSeconds * 4; attempt++)
         {
            if (ReadBackupLog().Contains(expected))
               return true;

            Thread.Sleep(250);
         }

         return false;
      }

      private string RunBackupAndWaitForSuccess()
      {
         _application.BackupManager.StartBackup();

         Assert.That(WaitForBackupLog("Backup completed successfully", 300),
            "The backup did not complete. Backup log: " + ReadBackupLog());

         return ReadBackupLog();
      }

      private string SingleArchive()
      {
         string[] archives = Directory.GetFiles(_backupDirectory, "*.7z");

         Assert.That(archives.Length, Is.EqualTo(1),
            "Expected exactly one archive in " + _backupDirectory + " but found " + archives.Length + ".");

         return archives[0];
      }

      private void WaitForRestoreCompletion(string lastServerStartTime)
      {
         for (int attempt = 0; attempt < 900; attempt++)
         {
            try
            {
               string startTime = _application.Status.StartTime;

               if (startTime.Length > 0 && startTime != lastServerStartTime)
                  return;
            }
            catch (Exception exception) when (!ExceptionPolicy.IsFatal(exception))
            {
               // The server is reinitializing; the proxy answers again when it is done.
            }

            Thread.Sleep(100);
         }

         Assert.Fail("The restore did not finish. Backup log: " + ReadBackupLog());
      }

      /// <summary>
      ///    7-Zip itself, run against the archive the way somebody who found the
      ///    file would run it. stdinLines is what it reads when it asks for a
      ///    passphrase - nothing at all for "and they did not have it".
      /// </summary>
      private (int exitCode, string output) Run7Za(string arguments, string stdinLines)
      {
         string programDirectory = _application.Settings.Directories.ProgramDirectory;

         // A registered install resolves to {InstallLocation}\Bin, a developer
         // build to the directory it was built into - the two BackupAccountStores
         // looks in.
         string[] candidates =
         {
            Paths.Combine(programDirectory, "7za.exe"),
            Paths.Combine(programDirectory, "Bin", "7za.exe"),
         };

         string sevenZip = candidates.FirstOrDefault(File.Exists);
         if (sevenZip == null)
            Assert.Fail("Could not find 7za.exe next to the server. Looked in " + string.Join(" and ", candidates) + ".");

         var startInfo = new ProcessStartInfo(sevenZip, arguments)
         {
            WorkingDirectory = _backupDirectory,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            RedirectStandardInput = true,
         };

         using (var process = Process.Start(startInfo))
         {
            Assert.That(process, Is.Not.Null, "7za.exe could not be started.");

            if (!string.IsNullOrEmpty(stdinLines))
               process.StandardInput.Write(stdinLines);

            // Closed either way: with nothing written this is the end of the
            // input, which is what makes 7-Zip fail instead of waiting for ever.
            process.StandardInput.Close();

            string output = process.StandardOutput.ReadToEnd() + process.StandardError.ReadToEnd();
            process.WaitForExit();

            return (process.ExitCode, output);
         }
      }

      private static string PassphraseLines(string passphrase)
      {
         string line = passphrase + "\n";
         return line + line + line;
      }

      // ---- the REST listener, for the route the Deck and hmctl use ----------

      private static (int status, string body) Http(string method, string path, string requestBody = null)
      {
         string credentials = Convert.ToBase64String(Encoding.ASCII.GetBytes("Administrator:" + AdminPassword));

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
               catch (SocketException exception)
               {
                  last = exception;
                  Thread.Sleep(200);
               }
            }

            if (last != null)
               throw last;

            using (NetworkStream stream = client.GetStream())
            using (var memory = new MemoryStream())
            {
               var headers = new StringBuilder();
               headers.Append(method + " " + path + " HTTP/1.0\r\n");
               headers.Append("Host: 127.0.0.1\r\n");
               headers.Append("Authorization: Basic " + credentials + "\r\n");

               byte[] bodyBytes = requestBody == null ? new byte[0] : Encoding.UTF8.GetBytes(requestBody);
               if (requestBody != null)
               {
                  headers.Append("Content-Type: application/json\r\n");
                  headers.Append("Content-Length: " + bodyBytes.Length + "\r\n");
               }

               headers.Append("Connection: close\r\n\r\n");

               byte[] headerBytes = Encoding.ASCII.GetBytes(headers.ToString());
               stream.Write(headerBytes, 0, headerBytes.Length);
               if (bodyBytes.Length > 0)
                  stream.Write(bodyBytes, 0, bodyBytes.Length);

               var buffer = new byte[8192];
               int read;
               while ((read = stream.Read(buffer, 0, buffer.Length)) > 0)
                  memory.Write(buffer, 0, read);

               string answer = Encoding.UTF8.GetString(memory.ToArray());
               int separator = answer.IndexOf("\r\n\r\n", StringComparison.Ordinal);

               string head = separator < 0 ? answer : answer.Substring(0, separator);
               string body = separator < 0 ? "" : answer.Substring(separator + 4);

               string[] statusParts = head.Split(new[] { "\r\n" }, StringSplitOptions.None)[0].Split(' ');
               int status = statusParts.Length > 1 && int.TryParse(statusParts[1], out int parsed) ? parsed : 0;

               return (status, body);
            }
         }
      }

      // ---- the tests -------------------------------------------------------

      [Test]
      [Description("An encrypted archive cannot even be listed without its passphrase, and restores with it - mail included")]
      public void AnEncryptedArchiveHidesItsNamesAndRestoresWithItsPassphrase()
      {
         string domainName = _domain.Name;
         Account account = DeliverTwoMessages(_domain);
         string address = account.Address;

         SetPassphrase(Passphrase);
         ClassicAssert.IsTrue(PassphraseIsSet(), "The server should report that a passphrase is set.");

         string log = RunBackupAndWaitForSuccess();
         StringAssert.Contains("This archive will be encrypted", log);
         StringAssert.Contains("The archive is encrypted", log);

         string archive = SingleArchive();
         ClassicAssert.IsTrue(ArchiveIsEncrypted(archive),
            "The archive's own header should say it is encrypted.");

         // THE POINT OF -mhe=on. Whoever finds this file learns nothing from it:
         // not the mailboxes in it, not that there is an hMailServer index in it.
         var listed = Run7Za("l \"" + archive + "\"", null);
         ClassicAssert.AreNotEqual(0, listed.exitCode, "7za listed an encrypted archive without a passphrase: " + listed.output);
         StringAssert.DoesNotContain("hMailServerBackup.xml", listed.output);
         StringAssert.DoesNotContain("DataBackup", listed.output);

         var opened = Run7Za("l \"" + archive + "\"", PassphraseLines(Passphrase));
         ClassicAssert.AreEqual(0, opened.exitCode, "7za could not list the archive with its passphrase: " + opened.output);
         StringAssert.Contains("hMailServerBackup.xml", opened.output);

         // And now the whole point of a backup: the mail comes back.
         _application.Domains.DeleteByDBID(_domain.ID);
         ClassicAssert.AreEqual(0, _application.Domains.Count, "The domain should have been deleted before the restore.");

         string startTime = _application.Status.StartTime;

         dynamic backup = OpenEncrypted(archive, Passphrase);
         ClassicAssert.IsTrue((bool) backup.ContainsDomains);
         ClassicAssert.IsTrue((bool) backup.ContainsMessages);
         ClassicAssert.IsTrue((bool) backup.ContainsSettings);

         backup.RestoreDomains = true;
         backup.RestoreMessages = true;
         backup.RestoreSettings = true;
         backup.StartRestore();

         Assert.That(WaitForBackupLog("Restore completed successfully", 600),
            "The restore did not complete. Backup log: " + ReadBackupLog());
         WaitForRestoreCompletion(startTime);

         Domain restored = _application.Domains.get_ItemByName(domainName);
         ClassicAssert.IsNotNull(restored, "The domain should be back after the restore.");
         ClassicAssert.IsNotNull(restored.Accounts.get_ItemByAddress(address), "The account should be back after the restore.");

         Pop3ClientSimulator.AssertMessageCount(address, "test", 2);

         // The passphrase is not in the archive, so the restore cannot have
         // brought one back: what this server has is the one that was set here.
         ClassicAssert.IsTrue(PassphraseIsSet(),
            "Restoring the settings must not clear the passphrase this server is using.");
      }

      [Test]
      [Description("An encrypted backup never puts the plaintext in the destination: it stages under the temp directory instead")]
      public void AnEncryptedBackupStagesThePlaintextAwayFromTheDestination()
      {
         DeliverTwoMessages(_domain);

         SetPassphrase(Passphrase);

         // WHAT THIS IS ABOUT. Both halves of a backup are written to a file
         // before they are archived: the index, which carries every account and
         // the server's own stored secrets, and the message files, which are
         // copied whole into a DataBackup folder because 7-Zip is handed a
         // directory. Both used to go into the destination and be deleted
         // afterwards - and the destination is the share or the cloud mount the
         // encryption exists because of, where a sync client uploads a file as it
         // appears. So an encrypted run stages under the temp directory instead,
         // and this watches the destination while the run happens to say so.
         //
         // A sampling watcher can MISS - a fast enough run between two samples
         // sees nothing - but it cannot lie: a name in this set was in the
         // destination. The post-run assertion below is the one that always
         // holds, and the two together are what there is, short of instrumenting
         // the server.
         var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
         var stop = new ManualResetEventSlim(false);

         var watcher = new Thread(() =>
         {
            while (!stop.IsSet)
            {
               try
               {
                  foreach (string entry in Directory.GetFileSystemEntries(_backupDirectory))
                  {
                     lock (seen)
                        seen.Add(Path.GetFileName(entry));
                  }
               }
               catch (Exception exception) when (!ExceptionPolicy.IsFatal(exception))
               {
                  // A directory being written into is allowed to refuse a listing.
               }

               Thread.Sleep(5);
            }
         });

         watcher.IsBackground = true;
         watcher.Start();

         try
         {
            RunBackupAndWaitForSuccess();
         }
         finally
         {
            stop.Set();
            watcher.Join(TimeSpan.FromSeconds(10));
         }

         string[] namesSeen;
         lock (seen)
            namesSeen = seen.ToArray();

         ClassicAssert.IsFalse(namesSeen.Contains("hMailServerBackup.xml", StringComparer.OrdinalIgnoreCase),
            "The backup index was written into the destination in the clear. It carries every account and this server's stored secrets, and the destination is the one place an encrypted backup must never put them. Seen there: " + string.Join(", ", namesSeen));
         ClassicAssert.IsFalse(namesSeen.Contains("DataBackup", StringComparer.OrdinalIgnoreCase),
            "The message files were staged in the destination in the clear before being archived. Seen there: " + string.Join(", ", namesSeen));

         // And afterwards nothing but the archive, whichever way the run went.
         string[] left = Directory.GetFileSystemEntries(_backupDirectory);
         ClassicAssert.AreEqual(1, left.Length,
            "The destination should hold the archive and nothing else. It holds: " + string.Join(", ", left.Select(Path.GetFileName)));
         StringAssert.EndsWith(".7z", left[0]);

         ClassicAssert.IsTrue(ArchiveIsEncrypted(SingleArchive()),
            "The archive this staged should still be an encrypted one.");
      }

      [Test]
      [Description("Without the passphrase, or with a wrong one, every way in refuses and the server is untouched")]
      public void TheArchiveIsRefusedWithoutThePassphraseOrWithAWrongOneAndNothingChanges()
      {
         string domainName = _domain.Name;
         Account account = DeliverTwoMessages(_domain);

         SetPassphrase(Passphrase);
         RunBackupAndWaitForSuccess();

         string archive = SingleArchive();

         // No passphrase at all. The archive is not "corrupt" and must not be
         // called corrupt: what it needs is a passphrase, and the sentence says so.
         Exception withoutPassphrase = Assert.Catch(() => _application.BackupManager.LoadBackup(archive));
         StringAssert.Contains("is encrypted", withoutPassphrase.Message);
         StringAssert.Contains("passphrase", withoutPassphrase.Message);

         // A wrong one is a different sentence: the archive was opened with what
         // was given and did not come apart.
         Exception wrongPassphrase = Assert.Catch(() => OpenEncrypted(archive, WrongPassphrase));
         StringAssert.Contains("could not be opened with the passphrase given", wrongPassphrase.Message);

         // Neither of them is an error in the server: a mistyped passphrase is an
         // ordinary thing to do at a prompt, and an ERROR log that fills up with
         // them stops meaning anything.
         CustomAsserts.AssertNoReportedError();

         // Nothing has been touched: the domain, the account and the mail are all
         // still here, which is the property a refusal has to have.
         ClassicAssert.AreEqual(1, _application.Domains.Count);
         Domain domain = _application.Domains.get_ItemByName(domainName);
         ClassicAssert.IsNotNull(domain.Accounts.get_ItemByAddress(account.Address));
         Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 2);

         StringAssert.Contains("Nothing has been changed on this server", ReadBackupLog());
      }

      [Test]
      [Description("A backup taken with no passphrase is unencrypted and still opens and restores without one")]
      public void AnUnencryptedBackupStillOpensWithoutAPassphrase()
      {
         DeliverTwoMessages(_domain);

         string log = RunBackupAndWaitForSuccess();
         StringAssert.Contains("This archive will not be encrypted", log);

         string archive = SingleArchive();
         ClassicAssert.IsFalse(ArchiveIsEncrypted(archive),
            "An archive taken with no passphrase should not read back as encrypted.");

         var listed = Run7Za("l \"" + archive + "\"", null);
         ClassicAssert.AreEqual(0, listed.exitCode, listed.output);
         StringAssert.Contains("hMailServerBackup.xml", listed.output);

         Backup opened = _application.BackupManager.LoadBackup(archive);
         ClassicAssert.IsTrue(opened.ContainsDomains);

         // And a passphrase offered for an archive that has none is not an error
         // either: whoever restores does not have to know which kind it is.
         dynamic openedWithOne = OpenEncrypted(archive, Passphrase);
         ClassicAssert.IsTrue((bool) openedWithOne.ContainsMessages);
      }

      [Test]
      [Description("The passphrase is in no read, no log and no archive")]
      public void ThePassphraseIsNeverReadBackAnywhere()
      {
         DeliverTwoMessages(_domain);

         SetPassphrase(Passphrase);
         string log = RunBackupAndWaitForSuccess();

         // The backup log says the archive is encrypted and never what with.
         StringAssert.DoesNotContain(Passphrase, log);

         // Nor the application and error logs, which the whole run went through.
         StringAssert.DoesNotContain(Passphrase, LogHandler.ReadCurrentDefaultLog());

         // The archive carries the settings section, and the passphrase is the one
         // setting left out of it - a key kept inside the lock it opens is not a
         // key. Read with the passphrase, since that is the only way in.
         string archive = SingleArchive();
         string staging = Paths.Combine(_backupDirectory, "index");
         Directory.CreateDirectory(staging);

         var extracted = Run7Za("e \"" + archive + "\" hMailServerBackup.xml -o\"" + staging + "\" -y", PassphraseLines(Passphrase));
         ClassicAssert.AreEqual(0, extracted.exitCode, extracted.output);

         string index = Paths.Combine(staging, "hMailServerBackup.xml");
         ClassicAssert.IsTrue(File.Exists(index), "7za did not extract the backup's index.");

         string xml = File.ReadAllText(index, Encoding.Unicode);
         StringAssert.Contains("<Backup>", xml);
         StringAssert.DoesNotContain(Passphrase, xml);
         StringAssert.DoesNotContain("backupencryptionpassphrase", xml);

         // And the flag is the only thing anything answers about it.
         ClassicAssert.IsTrue(PassphraseIsSet());

         // THE AUDIT TRAIL, which is the one store that holds this back by a
         // rule rather than by construction. Setting the passphrase is an
         // administrative change and is recorded as one - it goes through
         // PropertySet, which hands the real value and the value before it to
         // AuditTrail::RecordSettingChange - and the only thing between that and
         // a copy of the passphrase in a table the Deck renders is
         // AuditTrail::IsSecretName matching "passphrase" inside the setting's
         // name. Nothing else ties THIS setting to that list, so a later edit to
         // either could write the passphrase into hm_audit with every other test
         // here still passing. Both halves are asserted: the change is recorded,
         // and the value is not.
         RestPort = RestListener.Start(RestPort);
         RestartServerAndReacquireCom();

         try
         {
            (int status, string body) = Http("GET", "/api/v1/audit?object_type=setting&limit=100");
            Assert.That(status, Is.EqualTo(200), body);

            StringAssert.Contains("backupencryptionpassphrase: (changed)", body,
               "Setting the backup passphrase is an administrative change and has to be in the trail. Body: " + body);
            StringAssert.DoesNotContain(Passphrase, body,
               "An audit trail that copies the backup passphrase into a table the Deck renders hands over every archive the server has ever written.");
         }
         finally
         {
            RestListener.Stop();
            RestartServerAndReacquireCom();
         }
      }

      [Test]
      [Description("A passphrase an archive would not survive being moved with is refused, and changes nothing")]
      public void APassphraseTheArchiveWouldNotSurviveIsRefused()
      {
         Exception tooShort = Assert.Catch(() => SetPassphrase("short one"));
         StringAssert.Contains("at least 12 characters", tooShort.Message);

         Exception notAscii = Assert.Catch(() => SetPassphrase("passphrase with a \u00e4 in it"));
         StringAssert.Contains("printable ASCII", notAscii.Message);

         Exception spaced = Assert.Catch(() => SetPassphrase(" leading space here "));
         StringAssert.Contains("begin or end with a space", spaced.Message);

         ClassicAssert.IsFalse(PassphraseIsSet(),
            "A refused passphrase must leave the server with the one it had - here, none.");
      }

      [Test]
      [Description("A passphrase with compression off would write the mail in the clear beside the archive, so the backup refuses to run")]
      public void EncryptingWithoutCompressionRefusesTheBackupRatherThanWritingMailInTheClear()
      {
         DeliverTwoMessages(_domain);

         _application.Settings.Backup.CompressDestinationFiles = false;
         SetPassphrase(Passphrase);

         _application.BackupManager.StartBackup();

         Assert.That(WaitForBackupLog("BACKUP ERROR", 120),
            "The backup should have refused to run. Backup log: " + ReadBackupLog());

         string log = ReadBackupLog();
         StringAssert.Contains("compress destination files", log.ToLowerInvariant());
         StringAssert.Contains("No archive has been written", log);

         // Nothing was written at all: no archive, and no folder of message files
         // sitting beside where one would have been.
         ClassicAssert.AreEqual(0, Directory.GetFiles(_backupDirectory, "*.7z").Length,
            "A refused backup must not leave an archive behind.");
         ClassicAssert.IsFalse(Directory.Exists(Paths.Combine(_backupDirectory, "DataBackup")),
            "A refused backup must not leave the message files in the destination.");

         // The refusal is reported as a failed backup, which is what it is.
         CustomAsserts.AssertReportedError("BACKUP ERROR");
      }

      [Test]
      [Description("The REST route refuses without the passphrase and with a wrong one, and restores with it - which is the only restore a Linux server has")]
      public void TheRestRouteRefusesWithoutThePassphraseAndRestoresWithIt()
      {
         string domainName = _domain.Name;
         Account account = DeliverTwoMessages(_domain);
         string address = account.Address;

         SetPassphrase(Passphrase);
         RunBackupAndWaitForSuccess();
         string archive = SingleArchive();

         RestPort = RestListener.Start(RestPort);
         RestartServerAndReacquireCom();

         try
         {
            // The group says one is set and does not say what it is.
            (int status, string body) = Http("GET", "/api/v1/settings/backup");
            Assert.That(status, Is.EqualTo(200), body);
            StringAssert.Contains("\"encryption_passphrase_set\":true", body);
            StringAssert.DoesNotContain(Passphrase, body);
            StringAssert.DoesNotContain("\"encryption_passphrase\":", body);

            string archiveJson = archive.Replace("\\", "\\\\");

            (int withoutStatus, string withoutBody) = Http("POST", "/api/v1/backup/restore",
               "{\"file\":\"" + archiveJson + "\",\"restore_domains\":true,\"restore_messages\":true}");
            Assert.That(withoutStatus, Is.EqualTo(400), withoutBody);
            StringAssert.Contains("is encrypted", withoutBody);

            (int wrongStatus, string wrongBody) = Http("POST", "/api/v1/backup/restore",
               "{\"file\":\"" + archiveJson + "\",\"passphrase\":\"" + WrongPassphrase + "\",\"restore_domains\":true,\"restore_messages\":true}");
            Assert.That(wrongStatus, Is.EqualTo(400), wrongBody);
            StringAssert.Contains("could not be opened with the passphrase given", wrongBody);

            // Refused, and the server is as it was.
            ClassicAssert.AreEqual(1, _application.Domains.Count);

            // Now the real thing.
            _application.Domains.DeleteByDBID(_application.Domains.get_ItemByName(domainName).ID);
            ClassicAssert.AreEqual(0, _application.Domains.Count);

            string startTime = _application.Status.StartTime;

            (int startedStatus, string startedBody) = Http("POST", "/api/v1/backup/restore",
               "{\"file\":\"" + archiveJson + "\",\"passphrase\":\"" + Passphrase + "\",\"restore_domains\":true,\"restore_messages\":true}");
            Assert.That(startedStatus, Is.EqualTo(202), startedBody);
            StringAssert.Contains("\"encrypted\":true", startedBody);
            StringAssert.DoesNotContain(Passphrase, startedBody);

            Assert.That(WaitForBackupLog("Restore completed successfully", 600),
               "The restore did not complete. Backup log: " + ReadBackupLog());
            WaitForRestoreCompletion(startTime);

            Domain restored = _application.Domains.get_ItemByName(domainName);
            ClassicAssert.IsNotNull(restored, "The domain should be back after the restore.");
            Pop3ClientSimulator.AssertMessageCount(address, "test", 2);
         }
         finally
         {
            RestListener.Stop();
            RestartServerAndReacquireCom();
         }
      }
   }
}
