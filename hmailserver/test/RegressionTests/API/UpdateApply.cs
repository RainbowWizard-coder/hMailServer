// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.CodeDom.Compiler;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Threading;
using Microsoft.Win32;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;
using hMailServer;

namespace RegressionTests.API
{
   /// <summary>
   ///    The apply: the third part of the roadmap's live update. The server hands the
   ///    verified installer to hMailServer.Updater, which runs it, waits for the service
   ///    to come back, rolls back if it does not, and writes an outcome the service
   ///    reports at its next start. The "installers" here are programs the fixture
   ///    compiles on the spot - one that does nothing, one that stops the service, one
   ///    that starts it, one that fails - served from the fake feed and signed by the
   ///    suite's own Sigstore, so the whole path runs: download, verify, token, helper,
   ///    installer, wait, rollback, outcome, report.
   /// </summary>
   [TestFixture]
   public class UpdateApply : TestFixtureBase
   {
      // The port the listener answers on: this one on the Windows bench, the
      // suite's own where RestListener finds one already on.
      private static int RestPort = 9104;
      private const string AdminPassword = "testar";
      private const string LatestPath = "/repos/hmailserver/hmailserver/releases/latest";

      private static string _quiet, _stopper, _starter, _failing, _loud, _ager, _later, _incomplete, _putback, _upgraded, _upgradedStopper, _marker;

      private FakeHttpEndpoint _feed;
      private FakeSigstore _sigstore;
      private string _updatesDirectory;

      [SetUp]
      public void StartFeed()
      {
         // Every test here cleans the Updates folder, and some plant files in it,
         // which only SYSTEM, the Administrators and the service may change.
         RequireElevation("the Updates folder admits changes from SYSTEM, the Administrators and the service alone, and these tests clean it and plant files in it.");

         CompileFakeInstallers(Newer, Running);
         File.Delete(_marker);

         _sigstore = new FakeSigstore();
         _updatesDirectory = Paths.Combine(_settings.Directories.DataDirectory, "Updates");
         CleanUpdates();
         if (TokenListed())
            RestartServerAndReacquireCom();

         _feed = new FakeHttpEndpoint(200, "{}");
         ServeRelease(_quiet, _starter);

         _settings.SetAdministratorPassword(AdminPassword);
         WriteSetting("UpdateCheckEnabled", "0");
         WriteSetting("UpdateChannel", "stable");
         WriteSetting("UpdateFeedUrl", _feed.UrlFor(LatestPath));
         WriteSetting("UpdateTrustRootsFile", _sigstore.WriteTrustRootsFile());
         WriteSetting("UpdateLogPublicKeyFile", _sigstore.WriteLogKeyFile());
         WriteSetting("UpdateRequireAuthenticode", "0");
         WriteSetting("UpdateServiceWaitSeconds", "8");
         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();
      }

      [TearDown]
      public void StopFeed()
      {
         // An unelevated run set nothing up (RequireElevation).
         if (!Elevated)
            return;

         // An outcome the test did not ask to have reported must not be reported by
         // the reinitialize below: a non-ok one is an ERROR, and the next test's
         // setup would fail on it.
         if (File.Exists(OutcomePath))
            File.Delete(OutcomePath);

         // The apply drains the server before it starts the helper, and a helper
         // whose installer never stopped the service - every fake here but the
         // stopper - leaves that drain on until the server's own watcher ends it.
         // Left here, explicitly, so that one test's apply never blinds the next
         // ("already draining"), whatever the watcher's timing.
         LeaveTheDrainIfDraining();
         WriteSetting("UpdateCheckEnabled", "0");
         WriteSetting("UpdateFeedUrl", "");
         WriteSetting("UpdateTrustRootsFile", "");
         WriteSetting("UpdateLogPublicKeyFile", "");
         WriteSetting("UpdateRequireAuthenticode", "0");
         WriteSetting("UpdateServiceWaitSeconds", "180");
         RestListener.Stop();
         _application.Reinitialize();
         _feed.Dispose();
         _sigstore.Dispose();
         CleanUpdates();
         if (TokenListed())
            RestartServerAndReacquireCom();
      }

      [Test]
      public void AVerifiedInstallerIsHandedToTheHelperWhichRunsItSilently()
      {
         var status = Downloaded();

         Assert.IsTrue(status.InstallUpdate(), "The helper must start. Error: " + status.UpdateLastError);
         Assert.AreEqual(4, status.UpdateState, "State 4 is 'installing'.");

         // No download while it installs: it would put another file where the
         // helper is about to run this one. Refused, and the state left installing.
         Assert.IsFalse(status.DownloadUpdate(), "A download while an update is being installed must be refused.");
         StringAssert.Contains("hMailServer " + Newer + " is being installed; nothing is downloaded until its outcome is known.", status.UpdateLastError);
         Assert.AreEqual(4, status.UpdateState, "The refused download leaves the state 'installing'.");
         StringAssert.Contains("Update: hMailServer " + Newer + " is being applied by " + Paths.Combine(_updatesDirectory, "hMailServer.Updater.exe"),
            LogHandler.ReadCurrentDefaultLog());

         string outcome = WaitForOutcome(OutcomePath, 60);
         StringAssert.StartsWith("ok " + Newer + " the installer succeeded and the service is running", outcome);

         // What the installer was run with: silent, no restart, a log, and the token
         // in its environment - never on its command line, which Inno Setup writes
         // into the installer's log.
         string ran = File.ReadAllText(_marker);
         StringAssert.Contains("quiet /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS /LOG=", ran);
         StringAssert.DoesNotContain("/upgradetoken", ran, "A 6.3.6 or later installer is given the token in its environment.");
         StringAssert.Contains("/LOG=" + Paths.Combine(_updatesDirectory, "install-" + Newer + ".log"), ran);
         StringAssert.DoesNotContain("starter", ran, "The service never stopped, so nothing was rolled back.");

         Assert.IsTrue(File.Exists(Paths.Combine(_updatesDirectory, "hMailServer.Updater.exe")), "The helper runs from a copy outside Bin.");
         Assert.IsTrue(File.Exists(Paths.Combine(_updatesDirectory, "apply-" + Newer + ".log")), "The helper keeps a log.");
         string rollback = Paths.Combine(_updatesDirectory, "rollback", "hMailServer-" + Running + "-x64.exe");
         Assert.IsTrue(File.Exists(rollback), "The running version's installer was fetched as the rollback image: " + rollback);
         Assert.IsTrue(File.Exists(rollback + ".cosign.bundle"));
         StringAssert.Contains("Update: the rollback image for hMailServer " + Running + " is " + rollback + ", verified.", LogHandler.ReadCurrentDefaultLog());

         // The token the installer was given authenticates the administrator once.
         string token = Regex.Match(ran, @"quiet .*\[HM_UPGRADE_TOKEN=([0-9a-f]{64})\]").Groups[1].Value;
         Assert.AreEqual(64, token.Length, "A 32-byte token, hex: " + ran);

         // The helper's log has the installer's command line, says where the token
         // went, and does not carry it.
         string applyLog = File.ReadAllText(Paths.Combine(_updatesDirectory, "apply-" + Newer + ".log"));
         StringAssert.Contains(" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS /LOG=", applyLog);
         StringAssert.Contains("the token is passed in its environment", applyLog);
         StringAssert.DoesNotContain(token, applyLog, "The helper's log must not carry the token it passed.");

         Assert.IsNotNull(new Application().Authenticate("Administrator", "token:" + token), "The token is the administrator, once.");
         Assert.IsNull(new Application().Authenticate("Administrator", "token:" + token), "And only once.");
         Assert.IsFalse(TokenListed(), "A redeemed token is gone.");
         StringAssert.Contains("Update apply token redeemed", LogHandler.ReadCurrentDefaultLog());

         // The outcome is reported when the service next starts; a reinitialize runs
         // the same startup task.
         _application.Reinitialize();
         WaitUntil(() => File.Exists(OutcomePath + ".reported"), 30, "The outcome was not reported at startup.");
         StringAssert.Contains("Update applied: hMailServer " + Newer + " is installed", LogHandler.ReadCurrentDefaultLog());
         status = _application.Status;
         StringAssert.StartsWith("ok " + Newer + ": the installer succeeded", status.UpdateApplyOutcome);
         (int code, string body) = Http("GET", "/api/v1/update");
         Assert.AreEqual(200, code, body);
         StringAssert.Contains("\"apply\":{\"status\":\"ok\",\"version\":\"" + Newer + "\",\"detail\":\"the installer succeeded", body);
      }

      [Test]
      public void AServiceThatDoesNotComeBackIsRolledBack()
      {
         // Captured now: the service is about to stop and start again, and the
         // fixture's COM proxy will not survive it.
         string newer = Newer;
         string running = Running;

         ServeRelease(_stopper, _starter);
         var status = Downloaded();
         Assert.IsTrue(status.InstallUpdate(), status.UpdateLastError);

         // The helper: runs the stopper, the service stops; waits UpdateServiceWaitSeconds;
         // runs the rollback image, which starts the service; the service's startup task
         // reports the outcome and renames the file.
         WaitUntil(() => File.Exists(OutcomePath + ".reported"), 120, "The rollback did not complete and get reported.");
         string outcome = File.ReadAllText(OutcomePath + ".reported");
         StringAssert.StartsWith("rolled-back " + newer + " the installer succeeded but the service was not running after 8 seconds", outcome);
         StringAssert.Contains("hMailServer " + running + " was reinstalled and the service is running", outcome);

         string ran = File.ReadAllText(_marker);
         StringAssert.Contains("stopper /VERYSILENT", ran);
         StringAssert.Contains("starter /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS /LOG=", ran);
         StringAssert.DoesNotContain("starter /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS /upgradetoken", ran,
            "The rollback runs without the token: the version it restores already matches the schema.");
         Assert.IsFalse(Regex.IsMatch(ran, @"starter [^\r\n]*HM_UPGRADE_TOKEN"), "Nor in its environment.");

         // The service was restarted behind the harness's back; put the harness back
         // in step with it before anything else goes through COM.
         RestartServerAndReacquireCom();

         StringAssert.Contains("Update to hMailServer " + newer + " did not succeed (rolled-back)", LogHandler.ReadCurrentDefaultLog());
         StringAssert.Contains("Update to hMailServer " + newer + " did not succeed (rolled-back)", LogHandler.ReadErrorLog(),
            "A rollback is an ERROR the administrator must see.");
         Console.WriteLine("hMailServer error log (expected for this fixture):");
         Console.WriteLine(LogHandler.ReadErrorLog());
         LogHandler.ClearErrorLogUntilSettled();
      }

      [Test]
      public void AnInstallerThatFailsWhileTheServiceStaysUpIsNotRolledBack()
      {
         ServeRelease(_failing, _starter);
         var status = Downloaded();
         Assert.IsTrue(status.InstallUpdate(), status.UpdateLastError);

         string outcome = WaitForOutcome(OutcomePath, 60);
         StringAssert.StartsWith("failed " + Newer + " the installer exited with 1 but the service is running", outcome);
         StringAssert.DoesNotContain("starter", File.ReadAllText(_marker), "The service never stopped: no rollback.");

         // Reported as an ERROR at the next start, which a reinitialize stands in for.
         _application.Reinitialize();
         WaitUntil(() => File.Exists(OutcomePath + ".reported"), 30, "The outcome was not reported at startup.");
         StringAssert.Contains("Update to hMailServer " + Newer + " did not succeed (failed)", LogHandler.ReadErrorLog());
         Console.WriteLine("hMailServer error log (expected for this fixture):");
         Console.WriteLine(LogHandler.ReadErrorLog());
         LogHandler.ClearErrorLogUntilSettled();
      }

      [Test]
      [Description("An installer whose steps after its files did not complete (exit code 10) is rolled back, though the service it left runs")]
      public void AnInstallerWhoseStepsAfterItsFilesDidNotCompleteIsRolledBack()
      {
         // Until 6.3.6 the installer exited 0 when its database upgrade failed, and
         // the helper, finding the service running, reported the update installed
         // (the installer smoke VM, 1 October 2026). Exit code 1 with the service
         // running is still not rolled back (AnInstallerThatFailsWhileTheServiceStaysUpIsNotRolledBack).
         ServeRelease(_incomplete, _putback);
         var status = Downloaded();
         Assert.IsTrue(status.InstallUpdate(), status.UpdateLastError);

         string outcome = WaitForOutcome(OutcomePath, 60);
         StringAssert.StartsWith("rolled-back " + Newer + " the installer exited with 10: a step after its files", outcome);
         StringAssert.Contains("hMailServer " + Running + " was reinstalled and the service is running", outcome);
         // What its log says went wrong, without the lines that only name an error.
         StringAssert.Contains("The hMailServer database could not be created or upgraded (exit code 2).", outcome);
         StringAssert.DoesNotContain("WSAGetLastError", outcome);
         StringAssert.DoesNotContain("0 failed", outcome);
         string ran = File.ReadAllText(_marker);
         StringAssert.Contains("incomplete /VERYSILENT", ran);
         StringAssert.Contains("putback /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS /LOG=", ran, "The rollback image did not run.");
         // The installer is told which version is put back, so that it stops before
         // its database step when a step before it fails; the rollback image is not.
         Assert.IsTrue(Regex.IsMatch(ran, @"incomplete [^\r\n]*\[HM_UPDATE_ROLLBACK_VERSION=" + Regex.Escape(Running) + @"\]"),
            "The installer was not given the version the helper puts back.");
         Assert.IsFalse(Regex.IsMatch(ran, @"putback [^\r\n]*HM_UPDATE_ROLLBACK_VERSION"), "The rollback image was given a version to put back.");

         _application.Reinitialize();
         WaitUntil(() => File.Exists(OutcomePath + ".reported"), 30, "The outcome was not reported at startup.");
         StringAssert.Contains("Update to hMailServer " + Newer + " did not succeed (rolled-back)", LogHandler.ReadErrorLog());
         Console.WriteLine("hMailServer error log (expected for this fixture):");
         Console.WriteLine(LogHandler.ReadErrorLog());
         LogHandler.ClearErrorLogUntilSettled();
      }

      [Test]
      [Description("An installer whose database step changed the database and which then did not complete (exit code 11) is not rolled back: the earlier version may not run on that database")]
      public void AnInstallerThatUpgradedTheDatabaseIsNeverRolledBack()
      {
         // Since 36ef1a144 the helper put the earlier version back on exit code 10,
         // which the installer gave for any step after its files - also after its
         // database step had moved the schema. The earlier version then met a newer
         // database, its own installer failed, and the server refused the database:
         // no mail. The installer now gives 11 once the database has changed, and the
         // helper puts nothing back on it.
         ServeRelease(_upgraded, _putback);
         var status = Downloaded();
         Assert.IsTrue(status.InstallUpdate(), status.UpdateLastError);

         string outcome = WaitForOutcome(OutcomePath, 60);
         StringAssert.StartsWith("upgraded-incomplete " + Newer + " the installer exited with 11: a step did not complete after its database step had changed the database", outcome);
         StringAssert.Contains("Database step changed the database: it was at version 6105 before the step and is at version 6106", outcome);
         StringAssert.Contains("hMailServer " + Running + " was not put back, since it may not run on that database; the service is running", outcome);
         StringAssert.Contains("The hMailServer service was installed but did not start.", outcome);
         // The step that did not complete is named without the last file copied.
         StringAssert.DoesNotContain("Dest filename", outcome);
         StringAssert.Contains("restore the backup taken before the update", outcome);
         StringAssert.DoesNotContain("putback", File.ReadAllText(_marker), "The rollback image ran.");

         _application.Reinitialize();
         WaitUntil(() => File.Exists(OutcomePath + ".reported"), 30, "The outcome was not reported at startup.");
         StringAssert.Contains("Update to hMailServer " + Newer + " did not complete after its database step had changed the database, or may have, so the earlier version was not put back (upgraded-incomplete)",
            LogHandler.ReadErrorLog());
         StringAssert.Contains("Code: HM5481", LogHandler.ReadErrorLog(), "An upgraded database not put back has an error of its own.");
         StringAssert.DoesNotContain("did not succeed", LogHandler.ReadErrorLog());
         Console.WriteLine("hMailServer error log (expected for this fixture):");
         Console.WriteLine(LogHandler.ReadErrorLog());
         LogHandler.ClearErrorLogUntilSettled();
      }

      [Test]
      [Description("An installer that ended with 0 after its database step changed the database, and whose service did not come back, is not rolled back either")]
      public void AServiceThatDoesNotComeBackOnAnUpgradedDatabaseIsNotRolledBack()
      {
         string newer = Newer;
         string running = Running;

         ServeRelease(_upgradedStopper, _starter);
         var status = Downloaded();
         Assert.IsTrue(status.InstallUpdate(), status.UpdateLastError);

         // The installer's log says its database step began and not that it left the
         // database unchanged: the helper does not run the rollback image, which would
         // put a version on the database that refuses it.
         string outcome = WaitForOutcome(OutcomePath, 60);
         StringAssert.StartsWith("upgraded-incomplete " + newer + " the installer succeeded but the service was not running after 8 seconds", outcome);
         StringAssert.Contains("hMailServer " + running + " was not put back", outcome);
         StringAssert.DoesNotContain("starter", File.ReadAllText(_marker), "The rollback image ran.");

         // Bring it back ourselves; its startup reports the outcome.
         RestartServerAndReacquireCom();
         WaitUntil(() => File.Exists(OutcomePath + ".reported"), 30, "The outcome was not reported at startup.");
         StringAssert.Contains("Code: HM5481", LogHandler.ReadErrorLog());
         Console.WriteLine("hMailServer error log (expected for this fixture):");
         Console.WriteLine(LogHandler.ReadErrorLog());
         LogHandler.ClearErrorLogUntilSettled();
      }

      [Test]
      [Description("An installer that fails is reported with what its own log says went wrong, not only its exit code")]
      public void AFailedInstallerIsReportedWithWhatItsLogSays()
      {
         ServeRelease(_loud, _starter);
         var status = Downloaded();
         Assert.IsTrue(status.InstallUpdate(), status.UpdateLastError);
         string outcome = WaitForOutcome(OutcomePath, 60);
         StringAssert.StartsWith("failed " + Newer + " the installer exited with 5 but the service is running; the installer's log says: ", outcome);
         StringAssert.Contains("Dest filename: C:\\Program Files\\hMailServer\\ControlPanel\\hMailCP.exe", outcome);
         StringAssert.Contains("The process cannot access the file because it is being used by another process.", outcome);
         StringAssert.Contains("User chose Abort.", outcome);
         StringAssert.DoesNotContain("starter", File.ReadAllText(_marker), "The service never stopped: no rollback.");
         _application.Reinitialize();
         WaitUntil(() => File.Exists(OutcomePath + ".reported"), 30, "The outcome was not reported at startup.");
         StringAssert.Contains("Update to hMailServer " + Newer + " did not succeed (failed)", LogHandler.ReadErrorLog());
         Console.WriteLine("hMailServer error log (expected for this fixture):");
         Console.WriteLine(LogHandler.ReadErrorLog());
         LogHandler.ClearErrorLogUntilSettled();
      }

      [Test]
      [Description("A program running from under the installation - a Control Panel left open - is ended before the installer runs, since a silent installer aborts at the first file it cannot replace")]
      public void AProgramRunningFromTheInstallationIsEndedBeforeTheInstallerRuns()
      {
         // A copy of cmd.exe under the server's own directory tree stands in for the
         // Control Panel: its image is under the installation, so its files are ones
         // the installer would replace.
         string standIn = Paths.Combine(InstallationRoot(), "in-use-test", "hMailCP.exe");
         Directory.CreateDirectory(Path.GetDirectoryName(standIn));
         File.Copy(Paths.Combine(Environment.SystemDirectory, "cmd.exe"), standIn, true);
         var start = new ProcessStartInfo(standIn, "/c ping -n 120 127.0.0.1 >nul") {UseShellExecute = false, CreateNoWindow = true};
         using (Process process = Process.Start(start))
         {
            try
            {
               var status = Downloaded();
               Assert.IsTrue(status.InstallUpdate(), status.UpdateLastError);
               string outcome = WaitForOutcome(OutcomePath, 60);
               StringAssert.StartsWith("ok " + Newer, outcome);
               Assert.IsTrue(process.WaitForExit(30000), "The stand-in kept running: the helper did not end it.");
               string applyLog = File.ReadAllText(Paths.Combine(_updatesDirectory, "apply-" + Newer + ".log")).ToLowerInvariant();
               StringAssert.Contains(("Ended hMailCP.exe (process " + process.Id + "), running from " + standIn).ToLowerInvariant(), applyLog);
               StringAssert.Contains("1 program(s) running from it ended before the installer runs", applyLog);
            }
            finally
            {
               if (!process.HasExited)
                  process.Kill();
               process.WaitForExit(10000);
            }
         }
         Directory.Delete(Path.GetDirectoryName(standIn), true);
      }

      // The installation the helper is told about: the directory above the service's
      // Bin, read from the service's own registration, as the server derives it.
      private static string InstallationRoot()
      {
         using (RegistryKey key = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Services\hMailServer"))
         {
            string image = (string) key.GetValue("ImagePath");
            image = image.StartsWith("\"") ? image.Substring(1, image.IndexOf('"', 1) - 1) : image.Split(' ')[0];
            return Path.GetDirectoryName(Path.GetDirectoryName(image));
         }
      }

      [Test]
      public void NoRollbackImageMeansTheOutcomeSaysSo()
      {
         string newer = Newer;
         string running = Running;

         ServeRelease(_stopper, null);
         var status = Downloaded();
         Assert.IsTrue(status.InstallUpdate(), status.UpdateLastError);
         StringAssert.Contains("Update: no rollback image for hMailServer " + running + " could be had (the release feed answered HTTP 404",
            LogHandler.ReadCurrentDefaultLog());

         // The service stops and nothing brings it back; the helper says so.
         string outcome = WaitForOutcome(OutcomePath, 60);
         StringAssert.StartsWith("no-rollback " + newer + " the installer succeeded but the service was not running after 8 seconds", outcome);
         StringAssert.Contains("no rollback image was available", outcome);

         // Bring it back ourselves; its startup reports the outcome as an error.
         RestartServerAndReacquireCom();
         WaitUntil(() => File.Exists(OutcomePath + ".reported"), 30, "The outcome was not reported at startup.");
         StringAssert.Contains("Update to hMailServer " + newer + " did not succeed (no-rollback)", LogHandler.ReadErrorLog());
         Console.WriteLine("hMailServer error log (expected for this fixture):");
         Console.WriteLine(LogHandler.ReadErrorLog());
         LogHandler.ClearErrorLogUntilSettled();
      }

      [Test]
      public void NothingIsAppliedWithoutAVerifiedDownload()
      {
         var status = _application.Status;
         Assert.IsTrue(status.CheckForUpdate(), status.UpdateLastError);
         Assert.AreEqual(2, status.UpdateState);

         Assert.IsFalse(status.InstallUpdate(), "Nothing has been downloaded and verified.");
         StringAssert.Contains("No verified installer is waiting", status.UpdateLastError);
         Assert.IsFalse(File.Exists(OutcomePath));
         Assert.IsFalse(TokenListed(), "No token is issued for an apply that did not start.");
      }

      [Test]
      public void AnInstallerChangedSinceItWasVerifiedIsRefusedAndDeleted()
      {
         var status = Downloaded();
         string path = status.UpdateInstallerPath;
         byte[] bytes = File.ReadAllBytes(path);
         bytes[100] ^= 0xFF;
         File.WriteAllBytes(path, bytes);

         Assert.IsFalse(status.InstallUpdate(), "A file that no longer matches its bundle must not run.");
         StringAssert.Contains("The installer no longer verifies against its bundle and has been deleted", status.UpdateLastError);
         Assert.IsFalse(File.Exists(path));
         Assert.IsFalse(File.Exists(OutcomePath));
      }

      [Test]
      public void AnInstallerWhoseBundleIsSwappedForAnotherReleasesIsRefusedAtTheApply()
      {
         // Verified at the download, then the bundle beside it replaced by one the
         // same pipeline made for another tag: the apply verifies once more, against
         // the release it installs, and refuses.
         var status = Downloaded();
         string path = status.UpdateInstallerPath;
         string another = BumpVersion(Newer);
         File.WriteAllText(path + ".cosign.bundle",
            _sigstore.Bundle(File.ReadAllBytes(path), FakeSigstore.ReleaseSigning(another)), new UTF8Encoding(false));

         Assert.IsFalse(status.InstallUpdate(), "A file signed for another release must not run as this one.");
         StringAssert.Contains("The installer no longer verifies against its bundle and has been deleted: The certificate was made for the tag v" + another +
            ", not for v" + Newer, status.UpdateLastError);
         Assert.IsFalse(File.Exists(path));
         Assert.IsFalse(File.Exists(OutcomePath));
      }

      [Test]
      public void AnInstallerReplacedByAnotherReleasesIsRefusedAtTheApply()
      {
         // Verified at the download, then the installer replaced by another
         // release's, with a bundle that verifies it as this one - a certificate the
         // signature check accepts. The program says it is the later release, so
         // the apply, reading its version resource once more, refuses it.
         var status = Downloaded();
         string path = status.UpdateInstallerPath;
         byte[] later = File.ReadAllBytes(_later);
         File.WriteAllBytes(path, later);
         File.WriteAllText(path + ".cosign.bundle", _sigstore.Bundle(later, FakeSigstore.ReleaseSigning(Newer)), new UTF8Encoding(false));

         Assert.IsFalse(status.InstallUpdate(), "Another release's installer must not run as this one.");
         StringAssert.Contains("The installer is not the release it was downloaded as and has been deleted: The installer says it is version " +
            BumpVersion(Newer) + ".0, not " + Newer, status.UpdateLastError);
         Assert.IsFalse(File.Exists(path));
         Assert.IsFalse(File.Exists(OutcomePath));
      }

      [Test]
      [Description("The helper runs the installer only when it has the SHA-256 the server read at its last check, through a handle it holds while the installer runs: a file replaced between the server's check and the helper's hold is not run.")]
      public void AnInstallerReplacedAfterTheLastCheckIsNotRun()
      {
         var status = Downloaded();
         string path = status.UpdateInstallerPath;
         Assert.IsTrue(status.InstallUpdate(), status.UpdateLastError);

         // The helper waits two seconds before it holds the installer, for the
         // service to finish the call that started it; the swap is made in them.
         File.WriteAllBytes(path, File.ReadAllBytes(_later));

         string outcome = WaitForOutcome(OutcomePath, 60);
         StringAssert.StartsWith("failed " + Newer + " the installer was not run: ", outcome);
         StringAssert.Contains(" is not the file the server checked: its SHA-256 is ", outcome);
         Assert.IsFalse(File.Exists(_marker), "Nothing may have run: " + (File.Exists(_marker) ? File.ReadAllText(_marker) : ""));

         // The service never stopped, so the server reads the outcome itself, leaves
         // the drain and reports it as the failure it is.
         WaitUntil(() => File.Exists(OutcomePath + ".reported"), 30, "The outcome was not reported while the service was still running.");
         StringAssert.Contains("Update to hMailServer " + Newer + " did not succeed (failed)", LogHandler.ReadErrorLog());
         LogHandler.ClearErrorLogUntilSettled();
      }

      [Test]
      public void ACachedRollbackImageThatSaysItIsAnotherVersionIsFetchedAgain()
      {
         // A rollback image left from an earlier apply, whose bundle verifies it as
         // the running version but whose program says it is the newer one. It is not
         // the running version's, so it is deleted and the image fetched again from
         // the feed.
         string running = Running;
         string rollbackName = "hMailServer-" + running + "-x64.exe";
         var status = Downloaded();
         string rollbackDirectory = Paths.Combine(_updatesDirectory, "rollback");
         Directory.CreateDirectory(rollbackDirectory);
         string cached = Paths.Combine(rollbackDirectory, rollbackName);
         byte[] wrong = File.ReadAllBytes(_quiet);
         File.WriteAllBytes(cached, wrong);
         File.WriteAllText(cached + ".cosign.bundle", _sigstore.Bundle(wrong, FakeSigstore.ReleaseSigning(running)), new UTF8Encoding(false));

         Assert.IsTrue(status.InstallUpdate(), status.UpdateLastError);
         Assert.AreEqual(1, _feed.CountRequests("GET", "/download/" + rollbackName), "The cached image was refused, so the feed's was fetched.");
         Assert.AreEqual(File.ReadAllBytes(_starter), File.ReadAllBytes(cached), "The rollback image is now the running version's own.");
         StringAssert.Contains("Update: the rollback image for hMailServer " + running + " is " + cached + ", verified.", LogHandler.ReadCurrentDefaultLog());
         StringAssert.StartsWith("ok ", WaitForOutcome(OutcomePath, 60));
      }

      [Test]
      public void ARollbackImageSignedForAnotherReleaseIsNotUsed()
      {
         // The feed's entry for the running version carries a file signed for the
         // next tag. It is not the running version's, so there is no rollback image,
         // and the apply goes ahead without one and says so.
         string running = Running;
         string another = BumpVersion(running);
         ServeRelease(_quiet, _starter, FakeSigstore.ReleaseSigning(another));
         var status = Downloaded();
         Assert.IsTrue(status.InstallUpdate(), status.UpdateLastError);
         StringAssert.Contains("Update: no rollback image for hMailServer " + running + " could be had (", LogHandler.ReadCurrentDefaultLog());
         StringAssert.Contains("made for the tag v" + another + ", not for v" + running, LogHandler.ReadCurrentDefaultLog());
         Assert.IsFalse(File.Exists(Paths.Combine(_updatesDirectory, "rollback", "hMailServer-" + running + "-x64.exe")), "Nothing that failed is kept.");
         StringAssert.StartsWith("ok ", WaitForOutcome(OutcomePath, 60));
      }

      [Test]
      public void ARollbackImageThatSaysItIsAnotherVersionIsNotUsed()
      {
         // The running version's entry carries the newer release's installer,
         // signed as the running version: the signature is in order, but the
         // program says it is the newer version, so it is not the running one's.
         string running = Running;
         ServeRelease(_quiet, _quiet);
         var status = Downloaded();
         Assert.IsTrue(status.InstallUpdate(), status.UpdateLastError);
         StringAssert.Contains("Update: no rollback image for hMailServer " + running + " could be had (", LogHandler.ReadCurrentDefaultLog());
         StringAssert.Contains("The installer says it is version " + Newer + ".0, not " + running + ", the release it was offered as.", LogHandler.ReadCurrentDefaultLog());
         StringAssert.StartsWith("ok ", WaitForOutcome(OutcomePath, 60));
      }

      [Test]
      public void TheTokenExpiresAfterAnHour()
      {
         // The "installer" here ages the token file by two hours: it runs as the
         // service account, which may touch the file, where the suite may not.
         ServeRelease(_ager, _starter);
         var status = Downloaded();
         Assert.IsTrue(status.InstallUpdate(), status.UpdateLastError);
         WaitForOutcome(OutcomePath, 60);

         Assert.IsTrue(TokenListed(), "The ager never redeemed the token.");
         string token = Regex.Match(File.ReadAllText(_marker), @"ager .*?\[HM_UPGRADE_TOKEN=([0-9a-f]{64})\]").Groups[1].Value;
         Assert.AreEqual(64, token.Length, "The helper handed the installer a 32-byte token.");

         Assert.IsNull(new Application().Authenticate("Administrator", "token:" + token), "An hour is the token's life.");
         Assert.IsFalse(TokenListed(), "A stale token is removed when it is presented.");
         StringAssert.Contains("Update apply token refused: it had expired", LogHandler.ReadCurrentDefaultLog());

         // The first apply drained the server and its "installer" never stopped
         // the service, so the drain is on until the server's watcher sees the
         // outcome and ends it; a second apply before that is refused as "already
         // draining". Wait for the server to notice, as an operator would.
         WaitUntil(() => !Draining(), 30, "The server did not leave the first apply's drain after the helper finished without stopping it.");

         // And a wrong token, right shape, is refused without touching a fresh one.
         ServeRelease(_quiet, _starter);
         Assert.IsTrue(status.CheckForUpdate() && status.DownloadUpdate() && status.InstallUpdate(), status.UpdateLastError);
         WaitForOutcome(OutcomePath, 60);
         Assert.IsNull(new Application().Authenticate("Administrator", "token:" + new string('0', 64)));
         Assert.IsTrue(TokenListed(), "A wrong guess does not burn the token.");
      }

      [Test]
      [Description("A helper that finishes without stopping the service - an installer that failed while the service stayed up - would leave the server drained for ever; the server watches for the outcome, leaves the drain itself, records the 'left' as the server, and reports the outcome then rather than at the next start.")]
      public void AHelperThatFinishesWithoutStoppingTheServiceHasTheDrainLeftForIt()
      {
         ServeRelease(_failing, _starter);
         var status = Downloaded();
         Assert.IsTrue(status.InstallUpdate(), status.UpdateLastError);
         Assert.IsTrue(Draining(), "The apply drains the server before it starts the helper.");

         string outcome = WaitForOutcome(OutcomePath, 60);
         StringAssert.StartsWith("failed " + Newer + " the installer exited with 1 but the service is running", outcome);

         // Within the watcher's poll - five seconds, the file seen twice - the
         // server leaves the drain by itself, with no reinitialize and no DELETE.
         WaitUntil(() => !Draining(), 30, "The server did not leave the drain after the helper finished without stopping it.");
         Assert.AreEqual(3, (int) _application.ServerState, "The server is not Running after leaving the drain.");
         WaitUntil(() => File.Exists(OutcomePath + ".reported"), 30, "The outcome was not reported while the service was still running.");

         string log = LogHandler.ReadCurrentDefaultLog();
         StringAssert.Contains("Update: the helper has finished while this service is still running", log);
         StringAssert.Contains("Drain: left by the server. The update helper finished without stopping the service", log);
         StringAssert.Contains("Update to hMailServer " + Newer + " did not succeed (failed)", LogHandler.ReadErrorLog(),
            "The failure is reported now, as an ERROR, not at the next start.");

         // The 'left' is on the audit trail under the server's own name.
         (int code, string body) trail = Http("GET", "/api/v1/audit?limit=50");
         Assert.AreEqual(200, trail.code, trail.body);
         string body = trail.body;
         using (JsonDocument document = JsonDocument.Parse(body))
         {
            JsonElement left = document.RootElement.GetProperty("entries").EnumerateArray()
               .Where(e => e.GetProperty("object_type").GetString() == "server" && e.GetProperty("object").GetString() == "drain" && e.GetProperty("action").GetString() == "left")
               .OrderBy(e => e.GetProperty("id").GetInt64())
               .Select(e => e.Clone())
               .LastOrDefault();
            Assert.AreNotEqual(JsonValueKind.Undefined, left.ValueKind, "No 'left' on the audit trail: " + body);
            Assert.AreEqual("server", left.GetProperty("actor").GetString(), "The 'left' is not the server's own.");
            Assert.AreEqual("system", left.GetProperty("actor_type").GetString());
            StringAssert.Contains("finished without stopping the service", left.GetProperty("detail").GetString());
         }

         Console.WriteLine("hMailServer error log (expected for this fixture):");
         Console.WriteLine(LogHandler.ReadErrorLog());
         LogHandler.ClearErrorLogUntilSettled();
      }

      // Whether the server is in maintenance mode, read over the route the Deck
      // reads it from: GET /api/v1/server/drain answers draining true or false.
      private static bool Draining()
      {
         (int code, string body) = Http("GET", "/api/v1/server/drain");
         return code == 200 && body.Contains("\"draining\":true");
      }

      private static void LeaveTheDrainIfDraining()
      {
         try
         {
            if (Draining())
               Http("DELETE", "/api/v1/server/drain");
         }
         catch (Exception e) when (!ExceptionPolicy.IsFatal(e))
         {
            // A test that restarted the service behind the harness's back may have
            // left the listener elsewhere; the reinitialize below ends a drain too.
         }
      }

      [Test]
      public void AnApplyCanBeRunOverRest()
      {
         Downloaded();
         (int code, string body) = Http("POST", "/api/v1/update/install");
         Assert.AreEqual(200, code, body);
         StringAssert.Contains("\"state\":4", body);
         StringAssert.Contains("\"stateName\":\"installing\"", body);
         string outcome = WaitForOutcome(OutcomePath, 60);
         StringAssert.StartsWith("ok " + Newer, outcome);
      }

      // ---- an administrator's install (6.3.6) -----------------------------------------
      //
      // A service that is not an administrator (NT SERVICE\hMailServer) cannot run an
      // installer; an administrator installs from the Control Panel at the server, which
      // asks Windows for permission and checks the installer itself (Util/AdminUpdate.h).
      // The bench runs the service as LocalSystem, which installs as it always did: the
      // first test holds that path shut there. The second runs only where the service is
      // registered as another account - the installer smoke VM - and holds what the
      // service says there.

      [Test]
      public void ALocalSystemServiceInstallsItselfAndRefusesTheAdministratorsPath()
      {
         string account = ServiceLogonAccount();
         if (account == null || !IsLocalSystem(account))
            Assert.Ignore("The hMailServer service is registered as " + (account ?? "an account sc.exe did not report") + ", not LocalSystem.");

         var status = Downloaded();
         Assert.IsFalse(status.UpdateNeedsAdministrator, "LocalSystem may run an installer.");
         Assert.AreEqual("", status.UpdateInstallAdvice, "There is nothing to advise where the service installs.");

         Assert.IsFalse(status.PrepareUpdateForAdministrator(Newer, false), "LocalSystem installs itself; there is no administrator's install to prepare.");
         StringAssert.Contains("This service installs updates itself", status.UpdateLastError);
         Assert.AreEqual(3, status.UpdateState, "A refused preparation leaves the verified installer waiting.");
         Assert.AreEqual("", status.UpdatePreparation);

         Assert.IsFalse(status.BeginUpdateForAdministrator(Newer), "Nor one to begin.");
         StringAssert.Contains("This service installs updates itself", status.UpdateLastError);
         Assert.AreEqual(3, status.UpdateState);
         status.CancelUpdateForAdministrator("the test cancelled it");
         Assert.AreEqual(3, status.UpdateState, "A cancel with nothing prepared changes nothing.");
         Assert.IsFalse(TokenListed(), "No token is issued for an install that was refused.");

         (int code, string body) = Http("GET", "/api/v1/update");
         Assert.AreEqual(200, code, body);
         StringAssert.Contains("\"installedBy\":\"service\"", body);
         StringAssert.Contains("\"installAdvice\":\"\"", body);
         StringAssert.Contains("\"preparation\":null", body);
      }

      [Test]
      public void AServiceThatIsNotAnAdministratorLeavesTheInstallToOne()
      {
         string account = ServiceLogonAccount();
         if (account == null || IsLocalSystem(account))
            Assert.Ignore("The hMailServer service is registered as " + (account ?? "an account sc.exe did not report") +
                          "; this runs where it is another account, such as NT SERVICE\\hMailServer on the installer smoke VM.");

         var status = Downloaded();
         Assert.IsTrue(status.UpdateNeedsAdministrator, account + " is not an administrator and cannot run an installer.");
         string advice = status.UpdateInstallAdvice;
         StringAssert.Contains("cannot run an installer, so updates are installed by an administrator at the server", advice);
         StringAssert.Contains("hMailServer " + Newer, advice);

         // Install update over COM is not an error there: the installer stays verified
         // and waiting (state 3, not 5), and UpdateLastError says who installs it.
         Assert.IsFalse(status.InstallUpdate(), "The service cannot run the installer.");
         Assert.AreEqual(3, status.UpdateState, "The verified installer is still waiting for the administrator.");
         Assert.AreEqual(advice, status.UpdateLastError);
         Assert.IsFalse(File.Exists(OutcomePath));
         Assert.IsFalse(TokenListed(), "No token is issued for an install the service cannot run.");

         // Over REST the same, as a 409 with a code the Deck reads.
         (int code, string body) = Http("POST", "/api/v1/update/install");
         Assert.AreEqual(409, code, body);
         StringAssert.Contains("\"code\":\"update_install_by_administrator\"", body);
         (code, body) = Http("GET", "/api/v1/update");
         Assert.AreEqual(200, code, body);
         StringAssert.Contains("\"installedBy\":\"administrator\"", body);
         StringAssert.Contains("\"state\":3", body);

         // A begin with nothing prepared is refused and changes nothing.
         Assert.IsFalse(status.BeginUpdateForAdministrator(Newer));
         StringAssert.Contains("is not prepared for an administrator's install", status.UpdateLastError);
         Assert.AreEqual(3, status.UpdateState);
         Assert.IsFalse(TokenListed());
      }

      // The account the hMailServer service is registered to log on as (sc.exe qc);
      // null where that cannot be read.
      private static string ServiceLogonAccount()
      {
         try
         {
            var start = new ProcessStartInfo(Path.Combine(Environment.SystemDirectory, "sc.exe"), "qc hMailServer")
            {
               UseShellExecute = false,
               RedirectStandardOutput = true,
               CreateNoWindow = true
            };
            using (Process process = Process.Start(start))
            {
               string output = process.StandardOutput.ReadToEnd();
               process.WaitForExit(10000);
               Match match = Regex.Match(output, @"SERVICE_START_NAME\s*:\s*(\S.*?)\s*$", RegexOptions.Multiline);
               return match.Success ? match.Groups[1].Value : null;
            }
         }
         catch (Exception)
         {
            return null;
         }
      }

      private static bool IsLocalSystem(string account)
      {
         return string.Equals(account, "LocalSystem", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(account, @".\LocalSystem", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(account, @"NT AUTHORITY\SYSTEM", StringComparison.OrdinalIgnoreCase);
      }

      // ---- the fixture's installers ---------------------------------------------------

      // Each fake carries the version resource a real installer does (Inno Setup
      // writes <version>.0 from VersionInfoVersion), which the server holds to the
      // release it fetches the file as: the installers say the newer version, and
      // the starter - always the rollback image here - the running one.
      private static void CompileFakeInstallers(string newer, string running)
      {
         if (_quiet != null)
            return;

         string directory = Paths.Combine(Path.GetTempPath(), "hm-fake-installers");
         Directory.CreateDirectory(directory);
         _marker = Paths.Combine(directory, "ran.log");

         _quiet = Compile(directory, "quiet", "", 0, newer);
         _stopper = Compile(directory, "stopper",
            "using (var s = new System.ServiceProcess.ServiceController(\"hMailServer\")) { s.Stop(); s.WaitForStatus(System.ServiceProcess.ServiceControllerStatus.Stopped, TimeSpan.FromSeconds(90)); }", 0, newer);
         _starter = Compile(directory, "starter",
            "using (var s = new System.ServiceProcess.ServiceController(\"hMailServer\")) { s.Start(); s.WaitForStatus(System.ServiceProcess.ServiceControllerStatus.Running, TimeSpan.FromSeconds(90)); }", 0, running);
         _failing = Compile(directory, "failing", "", 1, newer);
         // Fails the way the real one did on 14 September 2026 - exit code 5, and a
         // log in Inno Setup's shape saying which file it could not replace and why.
         _loud = Compile(directory, "loud",
            @"foreach (string a in args) if (a.StartsWith(""/LOG="")) File.WriteAllText(a.Substring(5).Trim('""'), ""2026-09-14 01:38:20.001   Dest filename: C:\\Program Files\\hMailServer\\ControlPanel\\hMailCP.exe\r\n2026-09-14 01:38:20.002   Message box (Abort/Retry/Ignore):\r\n   C:\\Program Files\\hMailServer\\ControlPanel\\hMailCP.exe\r\n\r\nAn error occurred while trying to replace the existing file:\r\nDeleteFile failed; code 32.\r\nThe process cannot access the file because it is being used by another process.\r\n2026-09-14 01:38:20.003   User chose Abort.\r\n"");", 5, newer);
         // Runs as the service account, so it can do what the suite cannot: age the
         // token file, which only SYSTEM, Administrators and that account may touch.
         _ager = Compile(directory, "ager",
            "File.SetLastWriteTimeUtc(Path.Combine(AppDomain.CurrentDomain.BaseDirectory, \"apply-token\"), DateTime.UtcNow.AddHours(-2));", 0, newer);
         // The quiet program built as the release after the newer one: another
         // release's installer, which is never run.
         _later = Compile(directory, "later", "", 0, BumpVersion(newer));
         // An installer whose files are in place but whose steps after them - the
         // database upgrade above all - did not complete: exit code 10, the
         // service left running (hMailServerInnoExtension.iss, GetCustomSetupExitCode).
         // Its log as the real one wrote it on the smoke VM on 2 October 2026: lines
         // that name an error without being one, then the database step's failure.
         _incomplete = Compile(directory, "incomplete",
            @"foreach (string a in args) if (a.StartsWith(""/LOG="")) File.WriteAllText(a.Substring(5).Trim('""'), ""2026-10-02 09:08:58.001   Function and DLL name: WSAGetLastError@ws2_32.dll\r\n2026-10-02 09:09:10.002   Done: 0 granted, 12 already there, 0 failed, 0 approved, 0 to check.\r\n2026-10-02 09:09:12.003   Defaulting to OK for suppressed message box (OK):\r\n                          The hMailServer database could not be created or upgraded (exit code 2).The hMailServer service may not work until the database has been upgraded.\r\n2026-10-02 09:09:12.004   CurStepChanged raised an exception.\r\n"");", 10, newer);
         // A rollback image that starts the service only where it is not running,
         // as the real one leaves it running.
         _putback = Compile(directory, "putback",
            "using (var s = new System.ServiceProcess.ServiceController(\"hMailServer\")) { if (s.Status != System.ServiceProcess.ServiceControllerStatus.Running) { s.Start(); s.WaitForStatus(System.ServiceProcess.ServiceControllerStatus.Running, TimeSpan.FromSeconds(90)); } }", 0, running);
         // An installer whose database step changed the database and whose step
         // after it - the service's start - did not complete: exit code 11, the
         // service left running, and a log in the real one's shape, with the lines
         // the installer writes about its database step (BeginDatabaseStep and
         // EndDatabaseStep in hMailServerInnoExtension.iss).
         _upgraded = Compile(directory, "upgraded",
            @"foreach (string a in args) if (a.StartsWith(""/LOG="")) File.WriteAllText(a.Substring(5).Trim('""'), ""2026-10-02 12:00:00.001   Installation process succeeded.\r\n2026-10-02 12:00:01.002   Database step begun: the database was at version 6105 before it.\r\n2026-10-02 12:00:09.003   Database step changed the database: it was at version 6105 before the step and is at version 6106.\r\n2026-10-02 12:00:12.004   Defaulting to OK for suppressed message box (OK):\r\n                          The hMailServer service was installed but did not start.\r\n2026-10-02 12:00:12.005   CurStepChanged raised an exception.\r\n"");", 11, newer);
         // An installer that ended with 0 after its database step began and did not
         // say it left the database unchanged, and whose service does not come back:
         // the case exit code 11 cannot cover - the service failing after setup has
         // finished - and that the helper must not roll back either.
         _upgradedStopper = Compile(directory, "upgradedstopper",
            @"foreach (string a in args) if (a.StartsWith(""/LOG="")) File.WriteAllText(a.Substring(5).Trim('""'), ""2026-10-02 12:00:01.002   Database step begun: the database was at version 6105 before it.\r\n2026-10-02 12:00:09.003   Database step changed the database: it was at version 6105 before the step and is at version 6106.\r\n""); " +
            "using (var s = new System.ServiceProcess.ServiceController(\"hMailServer\")) { s.Stop(); s.WaitForStatus(System.ServiceProcess.ServiceControllerStatus.Stopped, TimeSpan.FromSeconds(90)); }", 0, newer);
      }

      // A console program that records its name and arguments, does what it is told to
      // the service, and exits with the code it was given: an installer as far as the
      // helper can tell.
      private static string Compile(string directory, string name, string action, int exitCode, string version)
      {
         string source =
            "using System;\n" +
            "using System.IO;\n" +
            "[assembly: System.Reflection.AssemblyFileVersion(\"" + version + ".0\")]\n" +
            "class FakeInstaller\n{\n" +
            "   static int Main(string[] args)\n   {\n" +
            // The token the helper gave in the environment, where a 6.3.6 or later
            // installer takes it, and the version it puts back if the installer does
            // not complete, recorded after the arguments.
            "      File.AppendAllText(@\"" + _marker + "\", \"" + name + " \" + string.Join(\" \", args) + " +
            "(string.IsNullOrEmpty(Environment.GetEnvironmentVariable(\"HM_UPGRADE_TOKEN\")) ? \"\" : \" [HM_UPGRADE_TOKEN=\" + Environment.GetEnvironmentVariable(\"HM_UPGRADE_TOKEN\") + \"]\") + " +
            "(string.IsNullOrEmpty(Environment.GetEnvironmentVariable(\"HM_UPDATE_ROLLBACK_VERSION\")) ? \"\" : \" [HM_UPDATE_ROLLBACK_VERSION=\" + Environment.GetEnvironmentVariable(\"HM_UPDATE_ROLLBACK_VERSION\") + \"]\") + " +
            "Environment.NewLine);\n" +
            "      " + action + "\n" +
            "      return " + exitCode + ";\n" +
            "   }\n}\n";

         // The name is one of the words CompileFakeInstallers passes - "quiet",
         // "stopper", "starter", "failing", "loud", "ager", "later", "incomplete", "putback",
         // "upgraded", "upgradedstopper" - so what is appended here is a file
         // name, never a rooted path that would displace the directory.
         string path = Paths.Combine(directory, name + ".exe");
         using (CodeDomProvider provider = CodeDomProvider.CreateProvider("CSharp"))
         {
            var parameters = new CompilerParameters {GenerateExecutable = true, OutputAssembly = path, GenerateInMemory = false};
            parameters.ReferencedAssemblies.Add("System.dll");
            parameters.ReferencedAssemblies.Add("System.ServiceProcess.dll");
            CompilerResults results = provider.CompileAssemblyFromSource(parameters, source);
            if (results.Errors.HasErrors)
            {
               var errors = new StringBuilder();
               foreach (CompilerError error in results.Errors)
                  errors.AppendLine(error.ToString());
               Assert.Fail("The fake installer '" + name + "' did not compile:\n" + errors);
            }
         }
         return path;
      }

      // ---- the release -----------------------------------------------------------------

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

      private static string BumpVersion(string version)
      {
         string[] parts = version.Split('.');
         return parts[0] + "." + parts[1] + "." + (int.Parse(parts[2]) + 1);
      }

      private string OutcomePath
      {
         get { return Paths.Combine(_updatesDirectory, "last-apply.txt"); }
      }

      private Status Downloaded()
      {
         var status = _application.Status;
         Assert.IsTrue(status.CheckForUpdate(), "The feed must be readable. Error: " + status.UpdateLastError);
         Assert.AreEqual(2, status.UpdateState, "The fixture's release must be newer than the running version.");
         Assert.IsTrue(status.DownloadUpdate(), "The installer must download and verify. Error: " + status.UpdateLastError);
         Assert.AreEqual(3, status.UpdateState);
         return status;
      }

      // The newer release's installer is the program at installerExe; the running
      // version's release - the rollback image - is the program at rollbackExe, or
      // absent from the feed when null.
      //
      // Each release is signed as the project signs it (FakeSigstore.ReleaseSigning):
      // the newer one on GitLab for its own tag, the running one by whichever signed
      // that version - the GitHub workflow for 6.3.3. rollbackSigning replaces the
      // rollback image's.
      private void ServeRelease(string installerExe, string rollbackExe, FakeSigstore.Options rollbackSigning = null)
      {
         byte[] installer = File.ReadAllBytes(installerExe);
         string name = "hMailServer-" + Newer + "-x64.exe";

         _feed.ClearRoutes();
         _feed.SetResponse("/download/" + name, 200, installer);
         _feed.SetResponse("/download/" + name + ".cosign.bundle", 200,
            Encoding.UTF8.GetBytes(_sigstore.Bundle(installer, FakeSigstore.ReleaseSigning(Newer))), "application/json");
         _feed.SetResponse(200, Release(Newer, name, installer));

         if (rollbackExe != null)
         {
            byte[] rollback = File.ReadAllBytes(rollbackExe);
            string rollbackName = "hMailServer-" + Running + "-x64.exe";
            _feed.SetResponse("/download/" + rollbackName, 200, rollback);
            _feed.SetResponse("/download/" + rollbackName + ".cosign.bundle", 200,
               Encoding.UTF8.GetBytes(_sigstore.Bundle(rollback, rollbackSigning ?? FakeSigstore.ReleaseSigning(Running))), "application/json");
            _feed.SetResponse("/repos/hmailserver/hmailserver/releases/tags/v" + Running, 200, Encoding.UTF8.GetBytes(Release(Running, rollbackName, rollback)), "application/json");
         }
         else
            _feed.SetResponse("/repos/hmailserver/hmailserver/releases/tags/v" + Running, 404, Encoding.UTF8.GetBytes("{\"message\":\"Not Found\"}"), "application/json");
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

      private static string WaitForOutcome(string path, int seconds)
      {
         WaitUntil(() => File.Exists(path), seconds, "The helper wrote no outcome to " + path + " within " + seconds + " seconds.");
         // Written in one go, but read it once it is complete.
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

      // apply-token is written with a DACL of SYSTEM, Administrators and the
      // service account. An unelevated process is none of those, and File.Exists
      // then answers false whether or not the file is there, because it cannot read
      // the attributes - so presence is read off the directory listing, which works
      // however this runs. An elevated run's CleanUpdates deletes the file, which
      // revokes the token as the restart below would.
      private bool TokenListed()
      {
         return Directory.Exists(_updatesDirectory) &&
                Directory.GetFiles(_updatesDirectory).Any(f => Path.GetFileName(f) == "apply-token");
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
            catch (IOException)
            {
               // The helper of the previous test may still hold its log open for a moment.
            }
            catch (UnauthorizedAccessException)
            {
               // apply-token is written by the service with a DACL naming SYSTEM,
               // the Administrators group and the service account only - which is
               // the point of it, since it authenticates as the administrator for
               // an hour. This fixture runs elevated, so it can; should it not, it
               // does not need to: the server revokes it when the apply finishes,
               // and one never redeemed expires in an hour.
            }
         }
      }

      // ---- plumbing --------------------------------------------------------------------

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

   /// <summary>
   ///    hMailServer.Updater on its own, started as the server starts it but with no
   ///    server behind it: a service name no service has, and every file in a folder
   ///    of the test's own. The helper runs a file - the installer, the rollback
   ///    image - only when it has the SHA-256 the server gave for it, read through a
   ///    handle the helper keeps open while the file runs, so that nothing can
   ///    change the file between the check and the run. Needs no elevation.
   /// </summary>
   [TestFixture]
   public class UpdateHelperDigest : TestFixtureBase
   {
      private const string NoSuchService = "hMailServerNoSuchServiceForTheHelperTest";

      private string _directory;
      private string _marker;
      private string _installer;
      private string _rollback;

      [SetUp]
      public void MakePrograms()
      {
         Assert.IsTrue(File.Exists(HelperPath()), "hMailServer.Updater.exe is not beside the service: " + HelperPath());

         // A folder of this test's own, named by a bare hex guid under the temporary
         // folder, so that nothing appended to it can be a rooted path.
         _directory = Paths.Combine(Path.GetTempPath(), "hm-helper-digest-" + Guid.NewGuid().ToString("N"));
         Directory.CreateDirectory(_directory);
         _marker = Paths.Combine(_directory, "ran.log");
         _installer = Compile("installer", "System.Threading.Thread.Sleep(4000);");
         _rollback = Compile("rollback", "");
      }

      [TearDown]
      public void RemovePrograms()
      {
         try
         {
            Directory.Delete(_directory, true);
         }
         catch (IOException)
         {
            // A program the helper started may be a moment from exiting.
         }
         catch (UnauthorizedAccessException)
         {
            // The same, reported as a refusal: a running program's file.
         }
      }

      [Test]
      public void AnInstallerWithTheServersDigestIsRunWhileHeld()
      {
         // The control: the same program started directly, with nothing holding
         // it, can be renamed while it runs - Windows opens a program's file so
         // that it may be - so a refusal below is the helper's doing.
         using (Process direct = Process.Start(new ProcessStartInfo(_installer) {UseShellExecute = false, CreateNoWindow = true}))
         {
            WaitUntil(() => File.Exists(_marker), 30, "The program did not start.");
            try
            {
               File.Move(_installer, _installer + ".moved");
               File.Move(_installer + ".moved", _installer);
            }
            catch (IOException e)
            {
               direct.WaitForExit(30000);
               Assert.Inconclusive("A running program cannot be renamed here even with nothing holding it, so the helper's hold would show nothing: " + e.Message);
            }
            Assert.IsTrue(direct.WaitForExit(30000), "The program did not finish.");
         }
         File.Delete(_marker);

         Process helper = StartHelper("--installer", _installer, "--installer-sha256", Sha256(_installer));
         try
         {
            // The installer sleeps four seconds: while it runs, the helper's handle
            // keeps it from being renamed, which a running program alone does not.
            WaitUntil(() => File.Exists(_marker), 30, "The helper did not run the installer.");
            Assert.Throws<IOException>(() => File.Move(_installer, _installer + ".moved"), "The installer could be renamed while it ran.");
         }
         finally
         {
            Assert.IsTrue(helper.WaitForExit(60000), "The helper did not finish.");
            helper.Dispose();
         }

         StringAssert.StartsWith("installer ", File.ReadAllText(_marker));
         StringAssert.StartsWith("no-rollback 9.9.9 the installer succeeded but the service was not running", Outcome());
      }

      [Test]
      public void AnInstallerWithAnotherDigestIsNotRun()
      {
         string other = Sha256(_rollback);
         RunHelper("--installer", _installer, "--installer-sha256", other);

         string outcome = Outcome();
         StringAssert.StartsWith("failed 9.9.9 the installer was not run: ", outcome);
         StringAssert.Contains("is not the file the server checked: its SHA-256 is " + Sha256(_installer) + ", not " + other, outcome);
         Assert.IsFalse(File.Exists(_marker), "The installer ran.");
      }

      [Test]
      public void AnInstallerWithNoDigestIsNotRun()
      {
         RunHelper("--installer", _installer);

         StringAssert.StartsWith("failed 9.9.9 the installer was not run: the server gave no SHA-256 for ", Outcome());
         Assert.IsFalse(File.Exists(_marker), "The installer ran.");
      }

      [Test]
      public void ARollbackImageWithAnotherDigestIsNotRun()
      {
         // The installer runs, the service - there is none - is not running after
         // it, and the rollback image is held to its digest before it runs.
         RunHelper("--installer", _installer, "--installer-sha256", Sha256(_installer),
                   "--rollback", _rollback, "--rollback-sha256", Sha256(_installer), "--rollback-version", "9.9.8");

         StringAssert.Contains("; the rollback image was not run: ", Outcome());
         StringAssert.StartsWith("rollback-failed 9.9.9 ", Outcome());
         string ran = File.ReadAllText(_marker);
         StringAssert.Contains("installer ", ran);
         StringAssert.DoesNotContain("rollback ", ran, "The rollback image ran.");
         // Checked before the installer runs, too: an installer told of a version to
         // put back stops before its database step when a step before it fails,
         // which only a rollback that can run makes good.
         StringAssert.DoesNotContain("HM_UPDATE_ROLLBACK_VERSION", ran, "The installer was told of a rollback image that cannot be run.");
      }

      [Test]
      public void ARollbackImageWithTheServersDigestIsRun()
      {
         RunHelper("--installer", _installer, "--installer-sha256", Sha256(_installer),
                   "--rollback", _rollback, "--rollback-sha256", Sha256(_rollback), "--rollback-version", "9.9.8");

         string ran = File.ReadAllText(_marker);
         StringAssert.Contains("rollback ", ran, "The rollback image did not run.");
         StringAssert.DoesNotContain("the rollback image was not run", Outcome());
         Assert.IsTrue(Regex.IsMatch(ran, @"installer [^\r\n]* \[HM_UPDATE_ROLLBACK_VERSION=9\.9\.8\]"), "The installer was not told of the version put back.");
         Assert.IsFalse(Regex.IsMatch(ran, @"rollback [^\r\n]*HM_UPDATE_ROLLBACK_VERSION"), "The rollback image was told of a version to put back.");
      }

      [Test]
      public void TheRollbackImageIsHeldWhileTheInstallerRuns()
      {
         // From before the installer runs until the rollback image has run: what the
         // installer is told it can count on is held the way the installer itself is.
         Process helper = StartHelper("--installer", _installer, "--installer-sha256", Sha256(_installer),
                                      "--rollback", _rollback, "--rollback-sha256", Sha256(_rollback), "--rollback-version", "9.9.8");
         try
         {
            WaitUntil(() => File.Exists(_marker), 30, "The helper did not run the installer.");
            Assert.Throws<IOException>(() => File.Move(_rollback, _rollback + ".moved"), "The rollback image could be renamed while the installer ran.");
            Assert.Throws<IOException>(() => File.AppendAllText(_rollback, "x"), "The rollback image could be changed while the installer ran.");
         }
         finally
         {
            Assert.IsTrue(helper.WaitForExit(60000), "The helper did not finish.");
            helper.Dispose();
         }

         StringAssert.Contains("rollback ", File.ReadAllText(_marker), "The rollback image did not run.");
      }

      private string Outcome()
      {
         return File.ReadAllText(Paths.Combine(_directory, "outcome.txt"));
      }

      private void RunHelper(params string[] arguments)
      {
         using (Process helper = StartHelper(arguments))
            Assert.IsTrue(helper.WaitForExit(60000), "The helper did not finish.");
      }

      private Process StartHelper(params string[] arguments)
      {
         var all = new List<string>
         {
            "--version", "9.9.9",
            "--outcome", Paths.Combine(_directory, "outcome.txt"),
            "--log", Paths.Combine(_directory, "helper.log"),
            "--service", NoSuchService,
            "--wait", "0"
         };
         all.AddRange(arguments);
         var start = new ProcessStartInfo(HelperPath(), string.Join(" ", all.Select(a => "\"" + a + "\"")))
         {
            UseShellExecute = false,
            CreateNoWindow = true
         };
         return Process.Start(start);
      }

      // The helper beside the service's own binary, as HelperSourcePath finds it.
      private static string HelperPath()
      {
         using (RegistryKey key = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Services\hMailServer"))
         {
            string image = (string) key.GetValue("ImagePath");
            image = image.StartsWith("\"") ? image.Substring(1, image.IndexOf('"', 1) - 1) : image.Split(' ')[0];
            return Paths.Combine(Path.GetDirectoryName(image), "hMailServer.Updater.exe");
         }
      }

      private static string Sha256(string path)
      {
         using (var sha = SHA256.Create())
            return FakeSigstore.Hex(sha.ComputeHash(File.ReadAllBytes(path)));
      }

      // A console program that records its name, its arguments and the version the
      // helper told it is put back (HM_UPDATE_ROLLBACK_VERSION), and exits 0 - an
      // installer, as far as the helper can tell - after doing what action says.
      private string Compile(string name, string action)
      {
         string source =
            "using System;\n" +
            "using System.IO;\n" +
            "class FakeInstaller\n{\n" +
            "   static int Main(string[] args)\n   {\n" +
            "      File.AppendAllText(@\"" + _marker + "\", \"" + name + " \" + string.Join(\" \", args) + " +
            "(string.IsNullOrEmpty(Environment.GetEnvironmentVariable(\"HM_UPDATE_ROLLBACK_VERSION\")) ? \"\" : \" [HM_UPDATE_ROLLBACK_VERSION=\" + Environment.GetEnvironmentVariable(\"HM_UPDATE_ROLLBACK_VERSION\") + \"]\") + " +
            "Environment.NewLine);\n" +
            "      " + action + "\n" +
            "      return 0;\n" +
            "   }\n}\n";

         // name is "installer" or "rollback", so this is a file name under the
         // test's folder, never a rooted path.
         string path = Paths.Combine(_directory, name + ".exe");
         using (CodeDomProvider provider = CodeDomProvider.CreateProvider("CSharp"))
         {
            var parameters = new CompilerParameters {GenerateExecutable = true, OutputAssembly = path, GenerateInMemory = false};
            parameters.ReferencedAssemblies.Add("System.dll");
            CompilerResults results = provider.CompileAssemblyFromSource(parameters, source);
            if (results.Errors.HasErrors)
            {
               var errors = new StringBuilder();
               foreach (CompilerError error in results.Errors)
                  errors.AppendLine(error.ToString());
               Assert.Fail("The fake installer '" + name + "' did not compile:\n" + errors);
            }
         }
         return path;
      }

      private static void WaitUntil(Func<bool> condition, int seconds, string message)
      {
         DateTime deadline = DateTime.Now.AddSeconds(seconds);
         while (!condition())
         {
            if (DateTime.Now > deadline)
               Assert.Fail(message);
            Thread.Sleep(100);
         }
      }
   }
}
