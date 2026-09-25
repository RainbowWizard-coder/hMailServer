// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Threading;
using System.Linq;
using NUnit.Framework;
using RegressionTests.Shared;

namespace RegressionTests.Infrastructure
{
   /// <summary>
   ///    Whether a restore is safe, rather than whether it works. BackupRestore
   ///    covers the happy path - back up a populated server, delete it, put it back -
   ///    and everything here is about what happens when the archive is not what the
   ///    restore assumes it is.
   ///
   ///    Every test in this fixture is about data loss, and every one of them fails
   ///    against the server as it was:
   ///
   ///      * TestRestoringDomainsFromAnArchiveWithNoneIsRefused - the worst of them.
   ///        The restore dropped every domain, then loaded domains from a &lt;Domains&gt;
   ///        element that was not there, which Collection::XMLLoad cannot tell from a
   ///        server that had no domains at backup time - so it reported "Restore
   ///        completed successfully" over an empty server. Before the fix the domain
   ///        count is zero and no refusal is logged.
   ///
   ///      * TestRestoringMessagesFromAnArchiveWithNoneIsRefused - same deletion,
   ///        then a null dereference on GetChildAttr("DataFiles", "Format") for an
   ///        archive taken with BackupMessages off. Before the fix this leaves the
   ///        server with no domains and trips the crash oracle; the guard that was
   ///        supposed to catch it ran forty lines after the pointer was used.
   ///
   ///      * TestRestoringSettingsFromAnArchiveWithNoneIsRefused - the same shape
   ///        applied to Configuration::XMLLoad, which deletes the SSL certificates,
   ///        TCP/IP ports, IP ranges, global rules, blocked attachments and anti-spam
   ///        lists before discovering there is nothing to put back.
   ///
   ///      * TestATruncatedArchiveCannotBeOpened - LoadBackup ignored the result of
   ///        the extraction and handed back a Backup object describing an archive it
   ///        had failed to read, with S_OK and nothing in any log.
   ///
   ///      * TestAnArchiveFromANewerVersionIsRefused - the Version attribute has been
   ///        written into every archive since 2010 and nothing had ever read it.
   ///
   ///      * TestAnArchiveWithAnEntryThatClimbsOutIsRefused, ...WithAnAbsoluteEntry...
   ///        and ...WithALink... - archives crafted with the bundled 7za (rn, -spf)
   ///        and a tar with a symbolic link. Each opened, and the restore handed
   ///        every entry to the archiver to place as it saw fit; now the entries
   ///        are listed first and the archive refused (HM7540). Each test opens
   ///        the same archive without the entry first, so the refusal is the
   ///        entry's.
   ///
   ///      * TestSecondBackupSucceedsWithAStagingFolderAlreadyPresent - not a restore
   ///        at all. FileUtilities::CopyDirectory copies with the throwing overload of
   ///        boost::filesystem::copy_file and no copy_options, so a destination file
   ///        that already exists throws; the exception is rethrown by
   ///        ExceptionHandler::Run, by WorkQueue::ExecuteTask and by
   ///        WorkQueue::IoServiceRunWorker, and an exception escaping a boost::thread
   ///        function calls std::terminate. With CompressDestinationFiles off nothing
   ///        ever removes the staged DataBackup folder, so before the fix the second
   ///        backup an installation takes kills the service - which is why this test
   ///        ends by asking the server a question over COM.
   /// </summary>
   [TestFixture]
   public class BackupRestoreSafety : TestFixtureBase
   {
      private string _backupDirectory;

      private string _previousDestination;
      private bool _previousBackupDomains;
      private bool _previousBackupMessages;
      private bool _previousBackupSettings;
      private bool _previousCompress;

      // Named differently from the base class's SetUp deliberately. A [SetUp] with
      // the same name hides the base one, which is how a fixture loses
      // PerformBasicSetup - and this one needs it for the domain it backs up and for
      // the clean-error-log assertion it makes on the way in.
      [SetUp]
      public void SetUpBackupRestoreSafety()
      {
         _backupDirectory = Paths.Combine(Path.GetTempPath(), "hMailBackupSafety-" + TestSetup.UniqueString());
         Directory.CreateDirectory(_backupDirectory);

         var backup = _application.Settings.Backup;

         _previousDestination = backup.Destination;
         _previousBackupDomains = backup.BackupDomains;
         _previousBackupMessages = backup.BackupMessages;
         _previousBackupSettings = backup.BackupSettings;
         _previousCompress = backup.CompressDestinationFiles;

         backup.Destination = _backupDirectory;

         CustomAsserts.AssertDeleteFile(backup.LogFile);
      }

      [TearDown]
      public void TearDownBackupRestoreSafety()
      {
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

         // Safety net. Every refused restore is reported as a critical error
         // (HM5014), which is correct, and each test consumes its own - but if an
         // assertion threw before that happened, the error this test provoked on
         // purpose would fail whichever fixture runs next and hide the real failure.
         LogHandler.DeleteErrorLog();
      }

      private void ConfigureBackup(bool domains, bool messages, bool settings, bool compress)
      {
         var backup = _application.Settings.Backup;

         backup.BackupDomains = domains;
         backup.BackupMessages = messages;
         backup.BackupSettings = settings;
         backup.CompressDestinationFiles = compress;
      }

      private string ReadBackupLog()
      {
         string logFile = _application.Settings.Backup.LogFile;

         try
         {
            if (!File.Exists(logFile))
               return "(no backup log)";

            // FileShare.ReadWrite: the server holds this log open, so an exclusive
            // open would fail rather than read.
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

      // Polls without using an NUnit assertion, so that a read taken while the server
      // is mid-write cannot record a failure against a test that then succeeds.
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

      private void RunBackupAndWaitForSuccess()
      {
         _application.BackupManager.StartBackup();

         Assert.IsTrue(WaitForBackupLog("Backup completed successfully", 300),
            "The backup did not complete. Backup log: " + ReadBackupLog());
      }

      private string SingleArchive()
      {
         var archives = new List<string>();

         foreach (string file in Directory.GetFiles(_backupDirectory, "*.7z"))
            archives.Add(file);

         Assert.AreEqual(1, archives.Count,
            "Expected exactly one archive in " + _backupDirectory + " but found " + archives.Count + ".");

         return archives[0];
      }

      // The archive an administrator would be handed by a failed copy: the beginning
      // of a real one. 7-Zip keeps the index at the end of the file, so this is
      // readable as a file and unreadable as an archive - which is the case the whole
      // restore path has to survive.
      private string CreateTruncatedArchive()
      {
         string source = SingleArchive();
         string truncated = Paths.Combine(_backupDirectory, "truncated.7z");

         byte[] head = new byte[512];
         int read;

         using (var stream = File.OpenRead(source))
            read = stream.Read(head, 0, head.Length);

         Assert.Greater(read, 0, "The archive to truncate was empty.");

         using (var stream = File.Create(truncated))
            stream.Write(head, 0, read);

         return truncated;
      }

      private string Locate7Za()
      {
         string programDirectory = _application.Settings.Directories.ProgramDirectory;

         // Same two candidates the scheduled-backup fixture uses for hMailServer.ini:
         // a registered install resolves to {InstallLocation}\Bin, while a developer
         // build that is not registered runs from the directory it was built into.
         string[] candidates =
         {
            Paths.Combine(programDirectory, "7za.exe"),
            Paths.Combine(programDirectory, "Bin", "7za.exe"),
         };

         string found = candidates.FirstOrDefault(File.Exists);
         if (found == null)
            Assert.Fail("Could not find 7za.exe next to the server. Looked in " + string.Join(" and ", candidates) + ".");

         return found;
      }

      // Builds an archive that claims to have been written by a much later
      // hMailServer. Only the index is needed: the version is refused before anything
      // else in the archive is looked at, which is the property being tested.
      private string CreateArchiveClaimingVersion(string version)
      {
         string stagingDirectory = Paths.Combine(_backupDirectory, "forged");
         Directory.CreateDirectory(stagingDirectory);

         // Mode 11 is domains + settings + compression, which is what a real archive
         // taken with BackupMessages off says. UTF-16 with a BOM because that is what
         // FileUtilities::WriteToFile(.., true) produces and what
         // ReadCompleteTextFile expects to detect.
         string xml = "<Backup><BackupInformation Mode=\"11\" Version=\"" + version + "\"/></Backup>";

         // settings-store-exempt: writes the forged archive's backup index, not hMailServer.ini.
         File.WriteAllText(Paths.Combine(stagingDirectory, "hMailServerBackup.xml"), xml, Encoding.Unicode);

         string archive = Paths.Combine(_backupDirectory, "forged.7z");

         // Run with the staging directory as the working directory and pass a bare
         // file name, so the entry is stored as "hMailServerBackup.xml" and not under
         // a path - which is how the server's own AddFile stores it and what the
         // restore's wildcard matches.
         var startInfo = new ProcessStartInfo(Locate7Za(), "a \"" + archive + "\" hMailServerBackup.xml -t7z -mx1")
         {
            WorkingDirectory = stagingDirectory,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
         };

         using (var process = Process.Start(startInfo))
         {
            Assert.IsNotNull(process, "7za.exe could not be started.");

            string output = process.StandardOutput.ReadToEnd();
            process.WaitForExit();

            Assert.AreEqual(0, process.ExitCode, "7za.exe could not build the test archive: " + output);
         }

         Assert.IsTrue(File.Exists(archive), "7za.exe reported success but " + archive + " does not exist.");

         return archive;
      }

      // Runs the bundled 7za - the archiver the restore itself runs - in a
      // directory of this test's, and fails the test if it fails.
      private void Run7Za(string arguments, string workingDirectory)
      {
         var startInfo = new ProcessStartInfo(Locate7Za(), arguments)
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

      // The index of an archive this server would open and restore: Mode 11, as
      // an archive taken with BackupMessages off says, and a version older than
      // the running one. UTF-16 with a BOM, as the executer writes it.
      private string WriteRestorableIndex(string stagingDirectory)
      {
         Directory.CreateDirectory(stagingDirectory);

         string index = Paths.Combine(stagingDirectory, "hMailServerBackup.xml");

         // settings-store-exempt: writes a crafted archive's backup index, not hMailServer.ini.
         File.WriteAllText(index, "<Backup><BackupInformation Mode=\"11\" Version=\"6.0.0-B1\"/></Backup>", Encoding.Unicode);

         return index;
      }

      // A 7z archive holding that index alone, built with the bundled 7za from a
      // staging directory of its own. It opens: the tests below check that first,
      // so that what refuses the archive afterwards is the entry they add to it.
      private string CreateRestorableArchive(string name, out string stagingDirectory)
      {
         stagingDirectory = Paths.Combine(_backupDirectory, name + "-staging");
         WriteRestorableIndex(stagingDirectory);

         string archive = Paths.Combine(_backupDirectory, name + ".7z");
         Run7Za("a \"" + archive + "\" hMailServerBackup.xml -t7z -mx1", stagingDirectory);

         return archive;
      }

      // One ustar header: the fields a reader uses, octal numbers, the checksum
      // over the header with its own field read as spaces.
      private static byte[] TarHeader(string name, char type, long size, string linkName)
      {
         var header = new byte[512];

         void Put(int offset, int length, string text)
         {
            byte[] bytes = Encoding.ASCII.GetBytes(text);
            Array.Copy(bytes, 0, header, offset, Math.Min(bytes.Length, length));
         }

         Put(0, 100, name);
         Put(100, 8, "0000777");
         Put(108, 8, "0000000");
         Put(116, 8, "0000000");
         Put(124, 12, Convert.ToString(size, 8).PadLeft(11, '0'));
         Put(136, 12, "00000000000");
         Put(148, 8, "        ");
         header[156] = (byte) type;
         Put(157, 100, linkName ?? string.Empty);
         Put(257, 6, "ustar");
         Put(263, 2, "00");

         int checksum = 0;
         foreach (byte b in header)
            checksum += b;

         Put(148, 8, Convert.ToString(checksum, 8).PadLeft(6, '0') + "\0 ");
         return header;
      }

      // A tar a restore can be pointed at like any archive - 7za opens tar as well
      // as 7z - holding the index and, when linkTarget is given, a symbolic link
      // beside it. Written by hand because a tar's link needs no privilege to
      // make, where a Windows symbolic link to store in a 7z archive does.
      private string CreateTarBackup(string name, string linkTarget)
      {
         string stagingDirectory = Paths.Combine(_backupDirectory, name + "-staging");
         byte[] index = File.ReadAllBytes(WriteRestorableIndex(stagingDirectory));

         string archive = Paths.Combine(_backupDirectory, name + ".tar");

         using (var stream = File.Create(archive))
         {
            byte[] header = TarHeader("hMailServerBackup.xml", '0', index.Length, null);
            stream.Write(header, 0, header.Length);
            stream.Write(index, 0, index.Length);

            int padding = (512 - index.Length % 512) % 512;
            stream.Write(new byte[padding], 0, padding);

            if (linkTarget != null)
            {
               header = TarHeader("DataBackup", '2', 0, linkTarget);
               stream.Write(header, 0, header.Length);
            }

            stream.Write(new byte[1024], 0, 1024);
         }

         return archive;
      }

      // LoadBackup, which every restore begins with, refusing the archive: no
      // Backup object, the reason in the backup log, and HM7540 in the ERROR log.
      private void AssertArchiveIsRefused(string archive, string expectedInLog)
      {
         CustomAsserts.AssertDeleteFile(_application.Settings.Backup.LogFile);

         // No assertion inside the try: Assert's own exception would be caught
         // as the refusal (see TestATruncatedArchiveCannotBeOpened).
         Exception thrown = null;
         try
         {
            var backup = _application.BackupManager.LoadBackup(archive);
            Console.WriteLine("LoadBackup returned an object for a crafted archive: " + backup);
         }
         catch (Exception e) when (!ExceptionPolicy.IsFatal(e))
         {
            thrown = e;
         }

         Assert.IsNotNull(thrown, archive + " should have been refused. Backup log: " + ReadBackupLog());

         Assert.IsTrue(WaitForBackupLog(expectedInLog, 30),
            "The backup log should name the entry the archive was refused for. Backup log: " + ReadBackupLog());

         // Unlike an archive that cannot be read, which is an ordinary mistake, an
         // archive built to write where a restore must not is reported.
         CustomAsserts.AssertReportedError("HM7540", "Nothing was extracted from it");
      }

      private void AssertTestDomainIsIntact(string accountAddress)
      {
         Assert.AreEqual(1, _application.Domains.Count,
            "The refused restore deleted domains. " + _application.Domains.Count + " domain(s) remain.");

         var domain = _application.Domains[0];
         Assert.AreEqual("example.test", domain.Name);

         // Enumerated rather than looked up by address: get_ItemByAddress raises a COM
         // error when there is no such account, and the failure this is watching for -
         // the account having been deleted - deserves the message below rather than an
         // interop exception from inside an assertion.
         var accounts = domain.Accounts;
         bool found = false;

         for (int index = 0; index < accounts.Count; index++)
         {
            if (string.Equals(accounts[index].Address, accountAddress, StringComparison.OrdinalIgnoreCase))
               found = true;
         }

         Assert.IsTrue(found,
            accountAddress + " was deleted by a restore that was supposed to have been refused. " +
            accounts.Count + " account(s) remain.");
      }

      [Test]
      [Description("A restore of domains from an archive that contains no domains is refused, instead of deleting every domain and reporting success.")]
      public void TestRestoringDomainsFromAnArchiveWithNoneIsRefused()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "safety-domains@example.test", "test");
         Assert.IsNotNull(account);

         // Settings only. There is no way to ask for a backup of nothing, so the
         // archive under test is a perfectly ordinary one that happens not to contain
         // the thing the restore is about to ask for.
         ConfigureBackup(false, false, true, true);
         RunBackupAndWaitForSuccess();

         string archive = SingleArchive();
         CustomAsserts.AssertDeleteFile(_application.Settings.Backup.LogFile);

         var backup = _application.BackupManager.LoadBackup(archive);
         Assert.IsNotNull(backup);

         // The guard that makes this test safe to run at all. The restore below is
         // only allowed to start if the archive really has no domains in it, which is
         // the exact condition the server is supposed to refuse - so a bug in the
         // refusal cannot be reached by way of a bug in this test's setup.
         Assert.IsFalse(backup.ContainsDomains,
            "The archive was expected to contain no domains, so this test cannot check what it is here to check.");

         backup.RestoreDomains = true;
         backup.RestoreSettings = false;
         backup.RestoreMessages = false;
         backup.StartRestore();

         Assert.IsTrue(WaitForBackupLog("does not contain any domains", 60),
            "The restore should have been refused because the archive contains no domains. Backup log: " + ReadBackupLog());

         AssertTestDomainIsIntact("safety-domains@example.test");

         // A refused restore is a failed restore and is reported as HM5014, which has
         // to be consumed here: PerformBasicSetup in the next fixture asserts the
         // error log is empty.
         CustomAsserts.AssertReportedError("HM5014");
      }

      [Test]
      [Description("A restore of messages from an archive that contains no message store is refused, instead of deleting every domain and then dereferencing null.")]
      public void TestRestoringMessagesFromAnArchiveWithNoneIsRefused()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "safety-messages@example.test", "test");
         Assert.IsNotNull(account);

         // Domains and settings, no messages - and no messages means no DataFiles
         // element in the index, which is the null that used to be dereferenced.
         ConfigureBackup(true, false, true, true);
         RunBackupAndWaitForSuccess();

         string archive = SingleArchive();
         CustomAsserts.AssertDeleteFile(_application.Settings.Backup.LogFile);

         var backup = _application.BackupManager.LoadBackup(archive);
         Assert.IsNotNull(backup);

         Assert.IsFalse(backup.ContainsMessages,
            "The archive was expected to contain no messages, so this test cannot check what it is here to check.");

         backup.RestoreDomains = true;
         backup.RestoreSettings = false;
         backup.RestoreMessages = true;
         backup.StartRestore();

         Assert.IsTrue(WaitForBackupLog("does not contain any messages", 60),
            "The restore should have been refused because the archive contains no messages. Backup log: " + ReadBackupLog());

         AssertTestDomainIsIntact("safety-messages@example.test");

         CustomAsserts.AssertReportedError("HM5014");
      }

      [Test]
      [Description("A restore of settings from an archive that contains no settings is refused, instead of emptying every settings collection and reporting success.")]
      public void TestRestoringSettingsFromAnArchiveWithNoneIsRefused()
      {
         // Something in a settings collection that is easy to look for afterwards.
         // PerformBasicSetup clears the SSL certificates before every test, so this
         // cannot leak into another fixture.
         var certificate = _application.Settings.SSLCertificates.Add();
         certificate.Name = "backup-safety";
         certificate.CertificateFile = "backup-safety.crt";
         certificate.PrivateKeyFile = "backup-safety.key";
         certificate.Save();

         ConfigureBackup(true, false, false, true);
         RunBackupAndWaitForSuccess();

         string archive = SingleArchive();
         CustomAsserts.AssertDeleteFile(_application.Settings.Backup.LogFile);

         var backup = _application.BackupManager.LoadBackup(archive);
         Assert.IsNotNull(backup);

         Assert.IsFalse(backup.ContainsSettings,
            "The archive was expected to contain no settings, so this test cannot check what it is here to check.");

         // Messages are asked for as well as settings, and the archive has neither.
         // That is on purpose: the settings check is the one under test, and the
         // messages check standing behind it means a bug in the settings check cannot
         // reach Configuration::XMLLoad - anything refused in the preparation phase
         // stops the whole restore before its first deletion. The assertion on the
         // message text is what keeps the test honest about which check fired.
         backup.RestoreDomains = true;
         backup.RestoreSettings = true;
         backup.RestoreMessages = true;
         backup.StartRestore();

         Assert.IsTrue(WaitForBackupLog("does not contain the server settings", 60),
            "The restore should have been refused because the archive contains no settings. Backup log: " + ReadBackupLog());

         var certificates = _application.Settings.SSLCertificates;
         bool found = false;

         for (int index = 0; index < certificates.Count; index++)
         {
            if (certificates[index].Name == "backup-safety")
               found = true;
         }

         Assert.IsTrue(found,
            "The SSL certificate was deleted by a restore that was supposed to have been refused. " +
            certificates.Count + " certificate(s) remain.");

         CustomAsserts.AssertReportedError("HM5014");
      }

      [Test]
      [Description("A truncated archive cannot be opened, rather than opening as a backup that contains nothing.")]
      public void TestATruncatedArchiveCannotBeOpened()
      {
         ConfigureBackup(true, false, true, true);
         RunBackupAndWaitForSuccess();

         string truncated = CreateTruncatedArchive();
         CustomAsserts.AssertDeleteFile(_application.Settings.Backup.LogFile);

         // Assert.Catch rather than Assert.Throws<COMException>: what matters is that
         // the call fails, not which exception type the interop layer picks for
         // DISP_E_BADINDEX.
         // Deliberately no assertion inside the delegate. Assert.Catch treats an
         // AssertionException like any other exception, so an assertion in here would
         // let the test pass on the strength of its own failure.
         Exception thrown = null;
         try
         {
            var backup = _application.BackupManager.LoadBackup(truncated);
            Console.WriteLine("LoadBackup returned an object for a truncated archive: " + backup);
         }
         catch (Exception e) when (!ExceptionPolicy.IsFatal(e))
         {
            thrown = e;
         }

         Assert.IsNotNull(thrown,
            "Opening a truncated archive should have failed. Backup log: " + ReadBackupLog());

         Assert.IsTrue(WaitForBackupLog("could not be read", 30),
            "The backup log should say the archive could not be read. Backup log: " + ReadBackupLog());

         // Pointing a restore at a file that is not a backup is an ordinary mistake,
         // not a defect in the server, so it must not write to the ERROR log - a
         // server whose ERROR log fills up with operator typos has an ERROR log that
         // means nothing.
         CustomAsserts.AssertNoReportedError();
      }

      [Test]
      [Description("An archive written by a newer hMailServer is refused rather than restored with whatever this version happens to understand.")]
      public void TestAnArchiveFromANewerVersionIsRefused()
      {
         string archive = CreateArchiveClaimingVersion("99.0.0-B1");

         CustomAsserts.AssertDeleteFile(_application.Settings.Backup.LogFile);

         Exception thrown = null;
         try
         {
            var backup = _application.BackupManager.LoadBackup(archive);
            Console.WriteLine("LoadBackup returned an object for a newer archive: " + backup);
         }
         catch (Exception e) when (!ExceptionPolicy.IsFatal(e))
         {
            thrown = e;
         }

         Assert.IsNotNull(thrown,
            "An archive from hMailServer 99.0.0 should not have opened. Backup log: " + ReadBackupLog());

         Assert.IsTrue(WaitForBackupLog("newer than", 30),
            "The backup log should say the archive is from a newer version. Backup log: " + ReadBackupLog());

         CustomAsserts.AssertNoReportedError();
      }

      [Test]
      [Description("An archive with an entry named ..\\something is refused before anything in it is extracted.")]
      public void TestAnArchiveWithAnEntryThatClimbsOutIsRefused()
      {
         string staging;
         string archive = CreateRestorableArchive("climbing", out staging);

         // The control: the same archive before the entry is added opens.
         Assert.IsNotNull(_application.BackupManager.LoadBackup(archive),
            "The archive holding only a sound index should have opened. Backup log: " + ReadBackupLog());

         // What 7za rn makes: an entry whose stored name climbs out of wherever the
         // archive is extracted - here, out of the restore's temporary directory
         // into the server's temp directory itself.
         string escapeName = "escaped-" + TestSetup.UniqueString() + ".txt";
         // settings-store-exempt: writes a file to put in a crafted archive, not hMailServer.ini.
         File.WriteAllText(Paths.Combine(staging, escapeName), "written outside the target");
         Run7Za("a \"" + archive + "\" \"" + escapeName + "\" -t7z -mx1", staging);
         Run7Za("rn \"" + archive + "\" \"" + escapeName + "\" \"..\\" + escapeName + "\"", staging);

         string escaped = Paths.Combine(_application.Settings.Directories.TempDirectory, escapeName);

         AssertArchiveIsRefused(archive, "..\\" + escapeName + " - a path that leads out of the directory it is extracted to");

         Assert.IsFalse(File.Exists(escaped), escaped + " was written by a restore that was refused.");
      }

      [Test]
      [Description("An archive with an entry stored under an absolute path is refused before anything in it is extracted.")]
      public void TestAnArchiveWithAnAbsoluteEntryIsRefused()
      {
         string staging;
         string archive = CreateRestorableArchive("absolute", out staging);

         Assert.IsNotNull(_application.BackupManager.LoadBackup(archive),
            "The archive holding only a sound index should have opened. Backup log: " + ReadBackupLog());

         // What 7za -spf makes: the entry keeps its whole path, drive and all.
         string absolute = Paths.Combine(staging, "absolute-" + TestSetup.UniqueString() + ".txt");
         // settings-store-exempt: writes a file to put in a crafted archive, not hMailServer.ini.
         File.WriteAllText(absolute, "stored under its full path");
         Run7Za("a -spf \"" + archive + "\" \"" + absolute + "\" -t7z -mx1", staging);

         // The file name and not the whole path: the temp directory's may be
         // written in its short form on the bench, and 7za keeps what it was given.
         AssertArchiveIsRefused(archive, Path.GetFileName(absolute) + " - a path that leads out of the directory it is extracted to");
      }

      [Test]
      [Description("An archive holding a symbolic link is refused before anything in it is extracted.")]
      public void TestAnArchiveWithALinkIsRefused()
      {
         // The control: a tar holding the index alone opens, so what refuses the
         // second is its link and not its format.
         Assert.IsNotNull(_application.BackupManager.LoadBackup(CreateTarBackup("unlinked", null)),
            "A tar holding only a sound index should have opened. Backup log: " + ReadBackupLog());

         // DataBackup as a link to the Windows directory: a restore that staged the
         // message store through it would write the store's files there.
         string archive = CreateTarBackup("linked", "C:/Windows");

         AssertArchiveIsRefused(archive, "DataBackup - a link to C:/Windows");
      }

      [Test]
      [Description("A second backup into a destination that still holds the staged message store succeeds, instead of terminating the service.")]
      public void TestSecondBackupSucceedsWithAStagingFolderAlreadyPresent()
      {
         // Compression off is what makes this the ordinary case rather than a
         // contrived one: the staged DataBackup folder *is* the backup in that mode,
         // so nothing ever deletes it and every run after the first copies onto files
         // that are already there.
         ConfigureBackup(true, true, true, false);

         RunBackupAndWaitForSuccess();

         string stagedStore = Paths.Combine(_backupDirectory, "DataBackup");
         Assert.IsTrue(Directory.Exists(stagedStore),
            "An uncompressed backup should leave the message store in " + stagedStore + ".");

         CustomAsserts.AssertDeleteFile(_application.Settings.Backup.LogFile);

         RunBackupAndWaitForSuccess();

         // The service answering at all is half of what this test checks: before the
         // fix the copy above threw, the exception escaped the work-queue thread and
         // the process was terminated, so this call is what tells a failed run apart
         // from a dead server.
         Assert.AreEqual("example.test", _application.Domains[0].Name);

         CustomAsserts.AssertNoReportedError();
      }
   }
}
