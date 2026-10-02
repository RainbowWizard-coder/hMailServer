// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.CodeDom.Compiler;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;
using hMailServer;

namespace RegressionTests.API
{
   /// <summary>
   ///    The unattended part: the last of the roadmap's live update. With
   ///    UpdateCheckEnabled=1 the scheduled task reads the feed when it is due, fetches
   ///    and verifies the installer when UpdateAutoDownload says so, and applies it -
   ///    after a backup, when UpdateBackupBeforeApply says so - when UpdateWindow is
   ///    open. Every step here is the same code a click runs; what is tested is that
   ///    the task takes them on its own, and only when it may.
   /// </summary>
   [TestFixture]
   public class UpdateSchedule : TestFixtureBase
   {
      // The port the listener answers on: this one on the Windows bench, the
      // suite's own where RestListener finds one already on.
      private static int RestPort = 9104;
      private const string AdminPassword = "testar";
      private const string LatestPath = "/repos/hmailserver/hmailserver/releases/latest";

      private static string _quiet, _quietRollback, _marker;

      private FakeHttpEndpoint _feed;
      private FakeSigstore _sigstore;
      private string _updatesDirectory;
      private string _backupDirectory;
      private string _previousBackupDestination;

      [SetUp]
      public void StartFeed()
      {
         // Every test here cleans the Updates folder, which only SYSTEM, the
         // Administrators and the service may change.
         RequireElevation("the Updates folder admits changes from SYSTEM, the Administrators and the service alone, and these tests clean it.");

         CompileQuietInstallers(Newer, Running);
         File.Delete(_marker);

         _sigstore = new FakeSigstore();
         _updatesDirectory = Paths.Combine(_settings.Directories.DataDirectory, "Updates");
         CleanUpdates();
         // A directory of the fixture's own for each test: the second part is this
         // prefix and a bare hex guid, so it can never be a rooted path.
         _backupDirectory = Paths.Combine(Path.GetTempPath(), "hm-update-backup-" + Guid.NewGuid().ToString("N"));
         _previousBackupDestination = _settings.Backup.Destination;

         _feed = new FakeHttpEndpoint(200, "{}");
         ServeRelease(_quiet);

         _settings.SetAdministratorPassword(AdminPassword);
         WriteSetting("UpdateCheckEnabled", "0");
         WriteSetting("UpdateChannel", "stable");
         WriteSetting("UpdateFeedUrl", _feed.UrlFor(LatestPath));
         WriteSetting("UpdateTrustRootsFile", _sigstore.WriteTrustRootsFile());
         WriteSetting("UpdateLogPublicKeyFile", _sigstore.WriteLogKeyFile());
         WriteSetting("UpdateRequireAuthenticode", "0");
         WriteSetting("UpdateServiceWaitSeconds", "8");
         WriteSetting("UpdateAutoDownload", "0");
         WriteSetting("UpdateWindow", "");
         WriteSetting("UpdateBackupBeforeApply", "1");
         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();
      }

      [TearDown]
      public void StopFeed()
      {
         // An unelevated run set nothing up (RequireElevation).
         if (!Elevated)
            return;

         // Every installer here is the quiet one, so an outcome is 'ok': let the
         // reinitialize below report it, which also takes the state out of
         // 'installing' for the next test.
         bool outcomePending = File.Exists(OutcomePath);
         WriteSetting("UpdateCheckEnabled", "0");
         WriteSetting("UpdateFeedUrl", "");
         WriteSetting("UpdateTrustRootsFile", "");
         WriteSetting("UpdateLogPublicKeyFile", "");
         WriteSetting("UpdateRequireAuthenticode", "0");
         WriteSetting("UpdateServiceWaitSeconds", "180");
         WriteSetting("UpdateAutoDownload", "0");
         WriteSetting("UpdateWindow", "");
         WriteSetting("UpdateBackupBeforeApply", "1");
         RestListener.Stop();
         _settings.Backup.Destination = _previousBackupDestination;
         _application.Reinitialize();
         if (outcomePending)
            WaitUntil(() => File.Exists(OutcomePath + ".reported"), 20, "The outcome was not reported.");
         _feed.Dispose();
         _sigstore.Dispose();
         CleanUpdates();
         if (Directory.Exists(_backupDirectory))
            Directory.Delete(_backupDirectory, true);
      }

      [Test]
      public void TheWindowIsReportedAsWrittenAndAsRead()
      {
         // Every day, an hour from the start.
         WriteSetting("UpdateWindow", "03:00");
         _application.Reinitialize();
         string body = Http("GET", "/api/v1/update").body;
         StringAssert.Contains("\"window\":{\"text\":\"every day 03:00-04:00\",\"open\":" + (IsBetween(3, 0, 4, 0) ? "true" : "false") + ",\"error\":\"\"}", body);
         StringAssert.Contains("\"autoDownload\":false", body);
         StringAssert.Contains("\"backupBeforeApply\":true", body);

         // Days and a range, in any case and order; read back Sunday-first, as the week is counted.
         WriteSetting("UpdateWindow", "sat, Sun 02:00-05:00");
         _application.Reinitialize();
         body = Http("GET", "/api/v1/update").body;
         StringAssert.Contains("\"window\":{\"text\":\"Sun,Sat 02:00-05:00\"", body);

         // Not a day, not a time: said, not guessed.
         WriteSetting("UpdateWindow", "Someday 03:00");
         _application.Reinitialize();
         body = Http("GET", "/api/v1/update").body;
         StringAssert.Contains("\"window\":{\"text\":\"Someday 03:00\",\"open\":false,\"error\":\"UpdateWindow: 'someday' is not a day", body);

         WriteSetting("UpdateWindow", "25:00");
         _application.Reinitialize();
         body = Http("GET", "/api/v1/update").body;
         StringAssert.Contains("\"error\":\"UpdateWindow: '25:00' is not a time; use HH:MM.\"", body);

         // Empty: never.
         WriteSetting("UpdateWindow", "");
         _application.Reinitialize();
         body = Http("GET", "/api/v1/update").body;
         StringAssert.Contains("\"window\":{\"text\":\"\",\"open\":false,\"error\":\"\"}", body);
      }

      [Test]
      public void AnUpdateIsAppliedUnattendedInsideTheWindow()
      {
         WriteSetting("UpdateCheckEnabled", "1");
         WriteSetting("UpdateAutoDownload", "1");
         WriteSetting("UpdateBackupBeforeApply", "0");
         WriteSetting("UpdateWindow", WindowAroundNow());
         _application.Reinitialize();

         // The startup task: the check finds the release, the download verifies it,
         // the window is open, the helper is started and the quiet installer runs.
         string outcome = WaitForOutcome(OutcomePath, 90);
         StringAssert.StartsWith("ok " + Newer, outcome);

         string log = LogHandler.ReadCurrentDefaultLog();
         StringAssert.Contains("Update check: hMailServer " + Newer + " is available", log);
         StringAssert.Contains("Update: hMailServer " + Newer + " downloaded to", log);
         StringAssert.Contains("is open and hMailServer " + Newer + " is verified; applying it unattended", log);
         // The token in the installer's environment, not on its command line.
         string ran = File.ReadAllText(_marker);
         StringAssert.Contains("quiet /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS /LOG=", ran);
         StringAssert.IsMatch(@"\[HM_UPGRADE_TOKEN=[0-9a-f]{64}\]", ran, "The token is given in the installer's environment.");
         StringAssert.DoesNotContain("/upgradetoken", ran);
         Assert.AreEqual(1, CountRequests("GET " + LatestPath), "The feed was read once by the startup task.");
      }

      [Test]
      public void NothingIsAppliedOutsideTheWindow()
      {
         WriteSetting("UpdateCheckEnabled", "1");
         WriteSetting("UpdateAutoDownload", "1");
         WriteSetting("UpdateBackupBeforeApply", "0");
         WriteSetting("UpdateWindow", WindowAwayFromNow());
         _application.Reinitialize();

         // The task checks and downloads, and stops there.
         var status = _application.Status;
         WaitUntil(() => status.UpdateState == 3, 60, "The task did not download and verify the installer. State: " + status.UpdateState + " " + status.UpdateLastError);
         Thread.Sleep(3000);
         Assert.IsFalse(File.Exists(OutcomePath), "Nothing may be applied outside the window.");
         Assert.AreEqual(3, status.UpdateState);
         StringAssert.DoesNotContain("applying it unattended", LogHandler.ReadCurrentDefaultLog());
      }

      [Test]
      public void TheDownloadIsNotAutomaticUnlessAsked()
      {
         WriteSetting("UpdateCheckEnabled", "1");
         WriteSetting("UpdateAutoDownload", "0");
         WriteSetting("UpdateBackupBeforeApply", "0");
         WriteSetting("UpdateWindow", WindowAroundNow());
         _application.Reinitialize();

         var status = _application.Status;
         WaitUntil(() => status.UpdateState == 2, 30, "The task did not check.");
         Thread.Sleep(3000);
         Assert.AreEqual(2, status.UpdateState, "Known, not fetched: UpdateAutoDownload is off.");
         Assert.AreEqual(0, CountRequests("GET /download/"), "Nothing was fetched.");
         Assert.IsFalse(File.Exists(OutcomePath));
      }

      [Test]
      public void TheBackupComesFirstAndItsAbsenceStopsTheApply()
      {
         WriteSetting("UpdateCheckEnabled", "1");
         WriteSetting("UpdateAutoDownload", "1");
         WriteSetting("UpdateBackupBeforeApply", "1");
         WriteSetting("UpdateWindow", WindowAroundNow());
         _settings.Backup.Destination = "";
         _application.Reinitialize();

         // Verified and waiting, but no destination to back up to: not applied, and
         // the log says what to do about it.
         var status = _application.Status;
         WaitUntil(() => status.UpdateState == 3, 60, "The task did not download and verify the installer.");
         WaitUntil(() => LogHandler.ReadCurrentDefaultLog().Contains("UpdateBackupBeforeApply is on and no backup destination is configured"), 20,
            "The task did not say why it stopped.");
         Thread.Sleep(2000);
         Assert.IsFalse(File.Exists(OutcomePath), "No backup, no apply.");

         // With a destination the backup runs, completes, and the apply follows.
         Directory.CreateDirectory(_backupDirectory);
         _settings.Backup.Destination = _backupDirectory;
         _settings.Backup.BackupSettings = true;
         _settings.Backup.BackupDomains = false;
         _settings.Backup.BackupMessages = false;
         _application.Reinitialize();

         string outcome = WaitForOutcome(OutcomePath, 120);
         StringAssert.StartsWith("ok " + Newer, outcome);
         string log = LogHandler.ReadCurrentDefaultLog();
         StringAssert.Contains("Update: backing up before applying hMailServer " + Newer, log);
         StringAssert.Contains("Update: the backup completed; applying hMailServer " + Newer, log);
         Assert.IsNotEmpty(Directory.GetFileSystemEntries(_backupDirectory), "The backup was written to the destination.");
      }

      [Test]
      public void TheLastApplysOutcomeOutlivesTheStartUpCheck()
      {
         // The Updates folder made by the server, with its own permissions, as an
         // apply leaves it: a download makes it.
         var status = _application.Status;
         Assert.IsTrue(status.CheckForUpdate(), status.UpdateLastError);
         Assert.IsTrue(status.DownloadUpdate(), status.UpdateLastError);

         // The outcome the helper writes while the service is down, after an
         // install. The start-up task reports it, then runs the check it is due; on
         // 6.3.4 and 6.3.5 that check, a second later, replaced the whole status and
         // dropped the outcome, and Status.UpdateApplyOutcome, the REST route and the
         // Control Panel said nothing of the update (#10's test, 29 September 2026).
         File.WriteAllText(OutcomePath, "ok " + Running + " the installer succeeded and the service is running" + Environment.NewLine);
         const string checkSaid = "Update check: hMailServer ";
         int checks = Occurrences(LogHandler.ReadCurrentDefaultLog(), checkSaid);
         WriteSetting("UpdateCheckEnabled", "1");
         _application.Reinitialize();

         WaitUntil(() => File.Exists(OutcomePath + ".reported"), 30, "The outcome was not reported at start-up.");
         WaitUntil(() => Occurrences(LogHandler.ReadCurrentDefaultLog(), checkSaid) > checks, 30, "The start-up check did not run.");
         StringAssert.Contains("Update applied: hMailServer " + Running + " is installed", LogHandler.ReadCurrentDefaultLog());
         status = _application.Status;
         StringAssert.StartsWith("ok " + Running + ": the installer succeeded", status.UpdateApplyOutcome,
            "The check that followed the start kept the outcome it had just been told.");
         (int code, string body) = Http("GET", "/api/v1/update");
         Assert.AreEqual(200, code, body);
         StringAssert.Contains("\"apply\":{\"status\":\"ok\",\"version\":\"" + Running + "\",\"detail\":\"the installer succeeded", body);

         // And every check after it, one that succeeds and one that fails.
         Assert.IsTrue(status.CheckForUpdate(), status.UpdateLastError);
         StringAssert.StartsWith("ok " + Running + ": ", status.UpdateApplyOutcome);
         _feed.SetResponse(500, "{\"message\":\"boom\"}");
         Assert.IsFalse(status.CheckForUpdate());
         StringAssert.StartsWith("ok " + Running + ": ", status.UpdateApplyOutcome);
      }

      [Test]
      public void AVerifiedInstallerIsKeptAcrossARestartAndNotFetchedAgain()
      {
         // A service that may not install - one running as NT SERVICE\hMailServer -
         // with UpdateAutoDownload=1 downloads and verifies, and leaves the install to
         // an administrator. On 6.3.4 and 6.3.5 it fetched the whole installer again
         // every time it started, since the verdict lives in the process (#10's test,
         // 29 September 2026). Outside the window here, so nothing is applied.
         WriteSetting("UpdateCheckEnabled", "1");
         WriteSetting("UpdateAutoDownload", "1");
         WriteSetting("UpdateBackupBeforeApply", "0");
         WriteSetting("UpdateWindow", WindowAwayFromNow());
         _application.Reinitialize();

         var status = _application.Status;
         WaitUntil(() => status.UpdateState == 3, 60, "The task did not download and verify the installer. State: " + status.UpdateState + " " + status.UpdateLastError);
         string installer = "GET /download/hMailServer-" + Newer + "-x64.exe ";
         Assert.AreEqual(1, CountRequests(installer), "One download.");
         string path = status.UpdateInstallerPath;
         byte[] verified = File.ReadAllBytes(path);
         DateTime written = File.GetLastWriteTimeUtc(path);

         // A new process: its status starts empty, and its start-up check finds the
         // release again.
         RestartServerAndReacquireCom();
         status = _application.Status;
         WaitUntil(() => status.UpdateState == 3, 60, "After the restart the installer was not verified. State: " + status.UpdateState + " " + status.UpdateLastError);
         Assert.AreEqual(1, CountRequests(installer), "The installer verified before the restart is verified again where it is, not fetched again.");
         Assert.AreEqual(path, status.UpdateInstallerPath);
         Assert.AreEqual(written, File.GetLastWriteTimeUtc(path), "The file is the one kept.");
         Assert.AreEqual(FakeSigstore.ReleaseSigning(Newer).Identity, status.UpdateSignerIdentity, "And its signer is read from its bundle again.");
         StringAssert.Contains("Update: hMailServer " + Newer + " is already in " + path + " and verified again: signed by " + FakeSigstore.ReleaseSigning(Newer).Identity,
            LogHandler.ReadCurrentDefaultLog());

         // A kept copy that is no longer the file that was signed is deleted and
         // fetched again.
         byte[] changed = (byte[]) verified.Clone();
         changed[changed.Length / 2] ^= 0xFF;
         File.WriteAllBytes(path, changed);
         RestartServerAndReacquireCom();
         status = _application.Status;
         WaitUntil(() => status.UpdateState == 3, 60, "After the second restart the installer was not fetched and verified. State: " + status.UpdateState + " " + status.UpdateLastError);
         Assert.AreEqual(2, CountRequests(installer), "A kept copy that does not verify is fetched again.");
         Assert.AreEqual(verified, File.ReadAllBytes(path), "The release's own bytes are back.");
         StringAssert.Contains("Update: the copy of hMailServer " + Newer + " kept in " + path + " did not verify again, so it is deleted and fetched again: ",
            LogHandler.ReadCurrentDefaultLog());
      }

      [Test]
      public void AKeptInstallerOutlivesAStartWhoseFirstCheckFails()
      {
         // The task sweeps the Updates folder after each check, keeping the files of
         // the running version and of the one on offer. A start whose first check
         // fails knows of none on offer, and the sweep took the installer the process
         // before had downloaded and verified - the copy kept across the restart to
         // be verified again where it is - so the next check that succeeded fetched
         // it again. The first check on a new machine failed that way on 29 September
         // 2026 (#10's test). Nothing is swept until a check says what is on offer.
         WriteSetting("UpdateCheckEnabled", "1");
         WriteSetting("UpdateAutoDownload", "1");
         WriteSetting("UpdateBackupBeforeApply", "0");
         WriteSetting("UpdateWindow", WindowAwayFromNow());
         _application.Reinitialize();

         var status = _application.Status;
         WaitUntil(() => status.UpdateState == 3, 60, "The task did not download and verify the installer. State: " + status.UpdateState + " " + status.UpdateLastError);
         string installer = "GET /download/hMailServer-" + Newer + "-x64.exe ";
         Assert.AreEqual(1, CountRequests(installer), "One download.");
         string path = status.UpdateInstallerPath;

         // The feed silent while the service starts again: its start-up check fails,
         // and the sweep runs after it, in the same run of the task.
         const string failedSaid = "Update check failed: ";
         int failures = Occurrences(LogHandler.ReadCurrentDefaultLog(), failedSaid);
         _feed.SetStall(LatestPath, 1);
         RestartServerAndReacquireCom();
         status = _application.Status;
         WaitUntil(() => Occurrences(LogHandler.ReadCurrentDefaultLog(), failedSaid) > failures, 60,
            "The start-up check did not fail on the silent feed.");
         Thread.Sleep(3000);
         Assert.IsTrue(File.Exists(path), "The kept installer outlives a start whose first check failed.");
         Assert.IsTrue(File.Exists(path + ".cosign.bundle"), "And its bundle.");

         // The next check that succeeds finds the release again, and the download
         // verifies the kept copy where it is.
         _feed.SetStall(LatestPath, 0);
         Assert.IsTrue(status.CheckForUpdate(), status.UpdateLastError);
         Assert.IsTrue(status.DownloadUpdate(), status.UpdateLastError);
         Assert.AreEqual(1, CountRequests(installer), "The kept installer is verified again, not fetched again.");
         StringAssert.Contains("Update: hMailServer " + Newer + " is already in " + path + " and verified again", LogHandler.ReadCurrentDefaultLog());
      }

      // ---- the fixture ---------------------------------------------------------------

      // The quiet program twice: as the newer release's installer and as the running
      // version's, the rollback image. Each carries the version resource a real
      // installer does (Inno Setup writes <version>.0 from VersionInfoVersion), and
      // the server holds each file to the release it is fetched as.
      private static void CompileQuietInstallers(string newer, string running)
      {
         if (_quiet != null)
            return;
         string directory = Paths.Combine(Path.GetTempPath(), "hm-fake-installers");
         Directory.CreateDirectory(directory);
         _marker = Paths.Combine(directory, "ran-schedule.log");
         _quiet = CompileQuiet(Paths.Combine(directory, "quiet-schedule.exe"), newer);
         _quietRollback = CompileQuiet(Paths.Combine(directory, "quiet-schedule-rollback.exe"), running);
      }

      private static string CompileQuiet(string path, string version)
      {
         string source =
            "using System;\nusing System.IO;\n" +
            "[assembly: System.Reflection.AssemblyFileVersion(\"" + version + ".0\")]\n" +
            "class FakeInstaller\n{\n   static int Main(string[] args)\n   {\n" +
            "      File.AppendAllText(@\"" + _marker + "\", \"quiet \" + string.Join(\" \", args) + " +
            "(string.IsNullOrEmpty(Environment.GetEnvironmentVariable(\"HM_UPGRADE_TOKEN\")) ? \"\" : \" [HM_UPGRADE_TOKEN=\" + Environment.GetEnvironmentVariable(\"HM_UPGRADE_TOKEN\") + \"]\") + " +
            "Environment.NewLine);\n" +
            "      return 0;\n   }\n}\n";
         using (CodeDomProvider provider = CodeDomProvider.CreateProvider("CSharp"))
         {
            var parameters = new CompilerParameters {GenerateExecutable = true, OutputAssembly = path, GenerateInMemory = false};
            parameters.ReferencedAssemblies.Add("System.dll");
            CompilerResults results = provider.CompileAssemblyFromSource(parameters, source);
            if (results.Errors.HasErrors)
               Assert.Fail("The fake installer did not compile: " + results.Errors[0]);
         }
         return path;
      }

      private string Running
      {
         get { return ((string) _application.Version).Split('-')[0]; }
      }

      private string Newer
      {
         get
         {
            string[] parts = Running.Split('.');
            return parts[0] + "." + parts[1] + "." + (int.Parse(parts[2]) + 1);
         }
      }

      private string OutcomePath
      {
         get { return Paths.Combine(_updatesDirectory, "last-apply.txt"); }
      }

      // "HH:MM-HH:MM" around now, every day: an hour either side, which never
      // crosses a day boundary in a way the window cannot express.
      private static string WindowAroundNow()
      {
         DateTime now = DateTime.Now;
         return now.AddHours(-1).ToString("HH:mm", CultureInfo.InvariantCulture) + "-" + now.AddHours(1).ToString("HH:mm", CultureInfo.InvariantCulture);
      }

      private static string WindowAwayFromNow()
      {
         DateTime now = DateTime.Now;
         return now.AddHours(3).ToString("HH:mm", CultureInfo.InvariantCulture) + "-" + now.AddHours(4).ToString("HH:mm", CultureInfo.InvariantCulture);
      }

      private static bool IsBetween(int fromHour, int fromMinute, int toHour, int toMinute)
      {
         int minutes = DateTime.Now.Hour * 60 + DateTime.Now.Minute;
         return minutes >= fromHour * 60 + fromMinute && minutes < toHour * 60 + toMinute;
      }

      // Each release signed as the project signs it (FakeSigstore.ReleaseSigning):
      // the newer one on GitLab for its own tag, the running one by whichever signed
      // that version.
      private void ServeRelease(string installerExe)
      {
         byte[] installer = File.ReadAllBytes(installerExe);
         byte[] rollback = File.ReadAllBytes(_quietRollback);
         string name = "hMailServer-" + Newer + "-x64.exe";
         string rollbackName = "hMailServer-" + Running + "-x64.exe";

         _feed.ClearRoutes();
         _feed.SetResponse("/download/" + name, 200, installer);
         _feed.SetResponse("/download/" + name + ".cosign.bundle", 200,
            Encoding.UTF8.GetBytes(_sigstore.Bundle(installer, FakeSigstore.ReleaseSigning(Newer))), "application/json");
         _feed.SetResponse(200, Release(Newer, name, installer));
         // The quiet program built as the running version, so the apply has a rollback image.
         _feed.SetResponse("/download/" + rollbackName, 200, rollback);
         _feed.SetResponse("/download/" + rollbackName + ".cosign.bundle", 200,
            Encoding.UTF8.GetBytes(_sigstore.Bundle(rollback, FakeSigstore.ReleaseSigning(Running))), "application/json");
         _feed.SetResponse("/repos/hmailserver/hmailserver/releases/tags/v" + Running, 200, Encoding.UTF8.GetBytes(Release(Running, rollbackName, rollback)), "application/json");
      }

      private string Release(string version, string name, byte[] installer)
      {
         string download = _feed.UrlFor("/download/");
         string digest;
         using (var sha = SHA256.Create())
            digest = "sha256:" + FakeSigstore.Hex(sha.ComputeHash(installer));
         return "{\"html_url\":\"https://gitlab.com/hmailserver/hmailserver/-/releases/v" + version + "\"," +
                "\"tag_name\":\"v" + version + "\",\"name\":\"hMailServer " + version + "\",\"draft\":false,\"prerelease\":false," +
                "\"published_at\":\"2026-09-12T10:00:00Z\",\"assets\":[" +
                "{\"name\":\"" + name + "\",\"size\":" + installer.Length + ",\"digest\":\"" + digest + "\",\"browser_download_url\":\"" + download + name + "\"}," +
                "{\"name\":\"" + name + ".cosign.bundle\",\"size\":1,\"browser_download_url\":\"" + download + name + ".cosign.bundle\"}" +
                "]}";
      }

      private int CountRequests(string fragment)
      {
         return _feed.Requests.Count(request => request.Contains(fragment));
      }

      private static int Occurrences(string text, string what)
      {
         int count = 0;
         for (int at = text.IndexOf(what, StringComparison.Ordinal); at >= 0; at = text.IndexOf(what, at + what.Length, StringComparison.Ordinal))
            count++;
         return count;
      }

      private static string WaitForOutcome(string path, int seconds)
      {
         WaitUntil(() => File.Exists(path), seconds, "The helper wrote no outcome to " + path + " within " + seconds + " seconds.");
         Thread.Sleep(200);
         return File.ReadAllText(path);
      }

      private static void WaitUntil(Func<bool> condition, int seconds, string message)
      {
         DateTime deadline = DateTime.Now.AddSeconds(seconds);
         while (!condition())
         {
            if (DateTime.Now > deadline)
               Assert.Fail(message);
            Thread.Sleep(250);
         }
      }

      private void CleanUpdates()
      {
         if (!Directory.Exists(_updatesDirectory))
            return;
         foreach (string file in Directory.GetFiles(_updatesDirectory, "*", SearchOption.AllDirectories))
         {
            try
            {
               File.Delete(file);
            }
            catch (IOException ex)
            {
               // A file the server still has open - the installer it is downloading or
               // applying. The next run's clean-up gets it, and a leftover under Updates
               // costs the following fixture nothing, so it is traced and not failed on.
               Trace.WriteLine("UpdateSchedule: " + file + " could not be deleted: " + ex.Message);
            }
            catch (UnauthorizedAccessException ex)
            {
               // apply-token carries a DACL naming SYSTEM, Administrators and the
               // service account only; the suite is none of those. The server
               // revokes it after an apply, and one never redeemed expires.
               Trace.WriteLine("UpdateSchedule: " + file + " is not the suite's to delete: " + ex.Message);
            }
         }
      }

      private static void WriteSetting(string key, string value)
      {
         IniFileSetting.Write(key, value);
      }

      private static (int code, string body) Http(string method, string path)
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
               string request =
                  method + " " + path + " HTTP/1.0\r\n" +
                  "Host: 127.0.0.1\r\n" +
                  "Authorization: Basic " + Convert.ToBase64String(Encoding.ASCII.GetBytes("Administrator:" + AdminPassword)) + "\r\n" +
                  "Connection: close\r\n\r\n";
               byte[] bytes = Encoding.ASCII.GetBytes(request);
               stream.Write(bytes, 0, bytes.Length);

               byte[] buffer = new byte[4096];
               int read;
               while ((read = stream.Read(buffer, 0, buffer.Length)) > 0)
                  memory.Write(buffer, 0, read);

               string raw = Encoding.UTF8.GetString(memory.ToArray());
               int code = 0;
               Match status = Regex.Match(raw, "^HTTP/1\\.[01] (\\d{3})");
               if (status.Success)
                  code = int.Parse(status.Groups[1].Value);
               int separator = raw.IndexOf("\r\n\r\n", StringComparison.Ordinal);
               return (code, separator >= 0 ? raw.Substring(separator + 4) : "");
            }
         }
      }
   }
}
