// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The Control Panel's half of an administrator's install of an update
   /// (AdministratorUpdate): which click does what, the stage's command line - the
   /// release, the work id and nothing else, never a path or a secret - the go code,
   /// and the words for everything the stage, the service and the Updater say.
   /// </summary>
   [Collection("Loc")]
   public class AdministratorUpdateTests : IDisposable
   {
      public void Dispose() => Loc.Reset();

      [Fact]
      public void TheDecisionFollowsTheServerTheConnectionAndTheState()
      {
         // LocalSystem, or a server before 6.3.6 (null): the service's own install.
         Assert.Equal(AdministratorUpdate.Decision.ServiceInstalls, AdministratorUpdate.Decide(true, false, 3, "6.3.7"));
         Assert.Equal(AdministratorUpdate.Decision.ServiceInstalls, AdministratorUpdate.Decide(false, false, 3, "6.3.7"));
         Assert.Equal(AdministratorUpdate.Decision.ServiceInstalls, AdministratorUpdate.Decide(true, null, 3, "6.3.7"));
         Assert.Equal(AdministratorUpdate.Decision.ServiceInstalls, AdministratorUpdate.Decide(false, null, 3, "6.3.7"));
         // An administrator is needed: here, or at the server.
         Assert.Equal(AdministratorUpdate.Decision.Administrator, AdministratorUpdate.Decide(true, true, 3, "6.3.7"));
         Assert.Equal(AdministratorUpdate.Decision.Remote, AdministratorUpdate.Decide(false, true, 3, "6.3.7"));
         Assert.Equal(AdministratorUpdate.Decision.Remote, AdministratorUpdate.Decide(false, true, 2, "6.3.7"));
         foreach (int state in new[] { 0, 1, 2, 4, 5, 6, 7 })
            Assert.Equal(AdministratorUpdate.Decision.NotReady, AdministratorUpdate.Decide(true, true, state, "6.3.7"));
         Assert.Equal(AdministratorUpdate.Decision.BadVersion, AdministratorUpdate.Decide(true, true, 3, "6.3.7 & calc"));
         Assert.Equal(AdministratorUpdate.Decision.BadVersion, AdministratorUpdate.Decide(true, true, 3, ""));
         Assert.Equal(AdministratorUpdate.Decision.BadVersion, AdministratorUpdate.Decide(true, true, 3, null));
      }

      [Theory]
      [InlineData("6.3.7", true)]
      [InlineData("6.3.7.1", true)]
      [InlineData("10.0", true)]
      [InlineData("6.3.7-rc1", true)]
      [InlineData("6", false)]
      [InlineData("6.3.7.1.2", false)]
      [InlineData("v6.3.7", false)]
      [InlineData("6.3.7 & calc", false)]
      [InlineData("6.3.7\" /x", false)]
      [InlineData("6.3.7-a_b", false)]
      [InlineData("6.3.7-", false)]
      [InlineData("6.3.7-abcdefghijklmnopqrstu", false)]
      [InlineData("..\\..\\x", false)]
      public void TheReleaseVersionPatternIsTheStages(string version, bool accepted)
      {
         Assert.Equal(accepted, AdministratorUpdate.IsReleaseVersion(version));
      }

      [Fact]
      public void TheWorkIdIs32LowercaseHexadecimalCharactersAndRandom()
      {
         string a = AdministratorUpdate.NewWorkId();
         string b = AdministratorUpdate.NewWorkId();
         Assert.True(AdministratorUpdate.IsWorkId(a));
         Assert.NotEqual(a, b);
         Assert.False(AdministratorUpdate.IsWorkId(a.ToUpperInvariant().Replace("0", "A") + "X"));
         Assert.False(AdministratorUpdate.IsWorkId(a.Substring(1)));
         Assert.False(AdministratorUpdate.IsWorkId("..\\" + a.Substring(3)));
      }

      [Fact]
      public void TheStageIsGivenTheReleaseTheIdThisProcessAndTheWaitAndNothingElse()
      {
         string id = "0123456789abcdef0123456789abcdef";
         string arguments = AdministratorUpdate.StageArguments("6.3.7", id, 4242, 180);
         Assert.Equal("--update-version 6.3.7 --update-work 0123456789abcdef0123456789abcdef --control-panel 4242 --update-wait 180 /InstallUpdate", arguments);
         Assert.DoesNotContain("\\", arguments);
         Assert.DoesNotContain("\"", arguments);
         Assert.DoesNotContain("token", arguments, StringComparison.OrdinalIgnoreCase);
         Assert.DoesNotContain("password", arguments, StringComparison.OrdinalIgnoreCase);

         // The wait is held to what the stage takes.
         Assert.Contains("--update-wait 30 ", AdministratorUpdate.StageArguments("6.3.7", id, 1, 5));
         Assert.Contains("--update-wait 3600 ", AdministratorUpdate.StageArguments("6.3.7", id, 1, 99999));

         // Nothing that is not its pattern reaches a command line an administrator's
         // prompt runs.
         Assert.Throws<ArgumentException>(() => AdministratorUpdate.StageArguments("6.3.7 & calc", id, 1, 180));
         Assert.Throws<ArgumentException>(() => AdministratorUpdate.StageArguments("6.3.7", "C:\\Windows", 1, 180));
         Assert.Throws<ArgumentException>(() => AdministratorUpdate.StageArguments("6.3.7", id, 0, 180));
      }

      [Fact]
      public void TheGoCodeIsHMUP()
      {
         Assert.Equal(0x484D5550, AdministratorUpdate.GoExitCode);
         Assert.Equal("HMUP", new string(new[] { (char)((AdministratorUpdate.GoExitCode >> 24) & 0xFF), (char)((AdministratorUpdate.GoExitCode >> 16) & 0xFF),
            (char)((AdministratorUpdate.GoExitCode >> 8) & 0xFF), (char)(AdministratorUpdate.GoExitCode & 0xFF) }));
      }

      [Fact]
      public void TheInstallationIsTheFolderAboveTheServicesBin()
      {
         Assert.True(AdministratorUpdate.InstallationFrom("C:\\Program Files\\hMailServer\\Bin\\hMailServer.exe", out string app));
         Assert.Equal("C:\\Program Files\\hMailServer", app);
         Assert.False(AdministratorUpdate.InstallationFrom("C:\\dev\\x64\\Release\\hMailServer.exe", out _));
         Assert.False(AdministratorUpdate.InstallationFrom("", out _));
         Assert.Equal(Path.Combine("C:\\hm", "AdminUpdate", "0123456789abcdef0123456789abcdef"), AdministratorUpdate.WorkFolder("C:\\hm", "0123456789abcdef0123456789abcdef"));
      }

      [Fact]
      public void TheStatusFileIsReadKeyByKey()
      {
         Dictionary<string, string> status = AdministratorUpdate.ParseKeyValues("state=installer-verified\r\nsigner=CN=Progressive Robot Ltd, O=Progressive Robot Ltd\r\nready-event=1234\r\nbroken line\r\n=x\r\n");
         Assert.Equal("installer-verified", status["state"]);
         Assert.Equal("CN=Progressive Robot Ltd, O=Progressive Robot Ltd", status["signer"]);
         Assert.Equal("1234", status["ready-event"]);
         Assert.Equal(3, status.Count);
         Assert.Empty(AdministratorUpdate.ParseKeyValues(null));
         Assert.Equal("Progressive Robot Ltd", AdministratorUpdate.CommonNameOf("CN=Progressive Robot Ltd, O=Progressive Robot Ltd, C=GB"));
      }

      private const string Keys = "\r\nid=0123456789abcdef0123456789abcdef\r\nby=HOST\\admin\r\nfrom=6.3.6\r\nfinished=2026-10-20T10:00:00Z\r\ninstall-log=C:\\hm\\AdminUpdate\\x\\install-6.3.7.log\r\nrollback-log=C:\\hm\\AdminUpdate\\x\\rollback-6.3.6.log\r\ninstaller=C:\\hm\\AdminUpdate\\x\\hMailServer-6.3.7-x64.exe\r\nrollback-image=C:\\hm\\AdminUpdate\\x\\rollback\\hMailServer-6.3.6-x64.exe\r\n";

      [Theory]
      [InlineData("ok", "hMailServer has been updated from 6.3.6 to 6.3.7, and the service is running. Installed by HOST\\admin at 2026-10-20T10:00:00Z.")]
      [InlineData("failed", "hMailServer 6.3.7 was not installed: the detail. hMailServer 6.3.6 is still running, unchanged.")]
      [InlineData("rolled-back", "hMailServer 6.3.7 did not start, so 6.3.6 was put back and is running: the detail.")]
      [InlineData("no-rollback", "hMailServer 6.3.7 did not start, and there was no earlier installer to put back: the detail.")]
      [InlineData("rollback-failed", "hMailServer 6.3.7 did not start, and putting 6.3.6 back failed too: the detail. Run C:\\hm\\AdminUpdate\\x\\rollback\\hMailServer-6.3.6-x64.exe as an administrator")]
      [InlineData("cancelled", "The install of hMailServer 6.3.7 was cancelled: the detail")]
      [InlineData("refused", "hMailServer 6.3.7 was not installed: the detail. Nothing was changed.")]
      public void EachOutcomeHasItsSentence(string status, string expected)
      {
         AdministratorUpdate.Outcome outcome = AdministratorUpdate.ParseOutcome(status + " 6.3.7 the detail" + Keys);
         Assert.NotNull(outcome);
         Assert.Equal(status, outcome.Status);
         Assert.Equal("6.3.7", outcome.Version);
         Assert.Equal("the detail", outcome.Detail);
         Assert.Equal("0123456789abcdef0123456789abcdef", outcome.Id);
         Assert.Equal("HOST\\admin", outcome.By);
         Assert.Contains(expected, AdministratorUpdate.OutcomeText(outcome));
         Assert.Equal(status == "ok" ? StatusLevel.Good : status is "rollback-failed" or "no-rollback" ? StatusLevel.Critical : StatusLevel.Warning,
            AdministratorUpdate.LevelOf(outcome));
         Assert.StartsWith("hMailServer was updated: ", AdministratorUpdate.SignInBanner(outcome));
      }

      [Theory]
      [InlineData(null)]
      [InlineData("")]
      [InlineData("installed 6.3.7 x")]
      [InlineData("ok")]
      [InlineData("\r\nid=x")]
      public void AnythingElseIsNoOutcome(string text)
      {
         Assert.Null(AdministratorUpdate.ParseOutcome(text));
         Assert.Equal("The install's outcome could not be read.", AdministratorUpdate.OutcomeText(null));
      }

      [Fact]
      public void EachStageExitHasItsReason()
      {
         var none = new Dictionary<string, string>();
         foreach (int code in new[] { 2, 3, 4, 5, 9, 10, 11, 12, 14, 99 })
            Assert.StartsWith("Install update stopped before anything was changed: ", AdministratorUpdate.StageStoppedText(code, none, "6.3.7"));
         Assert.Contains("administrator", AdministratorUpdate.StageStoppedText(3, none, "6.3.7"));
         Assert.Contains("code 99", AdministratorUpdate.StageStoppedText(99, none, "6.3.7"));
         Assert.Equal("The update was stopped; nothing was installed.", AdministratorUpdate.StageStoppedText(13, none, "6.3.7"));

         // A refusal names the check and where the copy is kept; a revocation server
         // that could not be reached is told apart from a revoked certificate.
         var refused = new Dictionary<string, string> { ["state"] = "refused", ["reason"] = "it has been changed since it was signed (TRUST_E_BAD_DIGEST)", ["kept"] = "C:\\hm\\AdminUpdate\\x" };
         string text = AdministratorUpdate.StageStoppedText(6, refused, "6.3.7");
         Assert.Contains("TRUST_E_BAD_DIGEST", text);
         Assert.Contains("C:\\hm\\AdminUpdate\\x", text);
         var offline = new Dictionary<string, string> { ["state"] = "refused", ["revocation"] = "offline", ["crl-host"] = "www.microsoft.com" };
         string offlineText = AdministratorUpdate.StageStoppedText(6, offline, "6.3.7");
         Assert.Contains("www.microsoft.com could not be reached", offlineText);
         Assert.DoesNotContain("revoked.", offlineText);
      }

      [Fact]
      public void AControlPanelOpenedAgainAsksAgainWhileTheServiceStillSaysInstalling()
      {
         // Opened again after an install that ended without stopping the service,
         // the Status page asks again while the service still says 4 (installing),
         // until it has read the outcome; never on an ordinary opening, never for
         // another state, and not for ever.
         Assert.True(AdministratorUpdate.ShouldAskAgainAfterInstall(true, 4, 0));
         Assert.True(AdministratorUpdate.ShouldAskAgainAfterInstall(true, 4, AdministratorUpdate.SettleAttempts - 1));
         Assert.False(AdministratorUpdate.ShouldAskAgainAfterInstall(true, 4, AdministratorUpdate.SettleAttempts));
         Assert.False(AdministratorUpdate.ShouldAskAgainAfterInstall(false, 4, 0));
         foreach (int state in new[] { 0, 1, 2, 3, 5, 6, 7 })
            Assert.False(AdministratorUpdate.ShouldAskAgainAfterInstall(true, state, 0));
         // Long enough to outlast the service's look for the outcome: every five
         // seconds, believed when seen twice.
         Assert.True(AdministratorUpdate.SettleInterval.TotalSeconds * AdministratorUpdate.SettleAttempts >= 30);
      }

      [Fact]
      public void AReasonThatEndsInAFullStopDoesNotEndTheSentenceTwice()
      {
         // The stage's reasons are sentences of their own where they come from the
         // version check and the Sigstore verifier, as the smoke VM's refusals of
         // 1 October 2026 showed: "...the release it was offered as.. Nothing was
         // changed". The window reads them as a clause.
         var version = new Dictionary<string, string> { ["state"] = "refused", ["reason"] = "The installer says it is version 6.3.6.0, not 6.3.7, the release it was offered as.", ["kept"] = "C:\\hm\\AdminUpdate\\x" };
         string text = AdministratorUpdate.StageStoppedText(6, version, "6.3.7");
         Assert.Contains("the release it was offered as. Nothing was changed", text);
         Assert.DoesNotContain("..", text);

         var failed = new Dictionary<string, string> { ["reason"] = "the copy could not be made. " };
         string failedText = AdministratorUpdate.StageStoppedText(14, failed, "6.3.7");
         Assert.Equal("Install update stopped before anything was changed: the copy could not be made.", failedText);

         Assert.Equal("signed (TRUST_E_BAD_DIGEST)", AdministratorUpdate.Clause("signed (TRUST_E_BAD_DIGEST)"));
         Assert.Equal("", AdministratorUpdate.Clause(null));
      }

      [Fact]
      public void ThePreparationDocumentIsReadAsTheServiceWritesIt()
      {
         // UpdateInstaller's PreparationDocument_, field for field.
         const string json = "{\"step\":\"rollback\",\"backup\":{\"taken\":true,\"running\":false,\"skipped\":\"\",\"failed\":\"\",\"path\":\"D:\\\\Backup\"},\"rollback\":{\"version\":\"6.3.6\",\"ready\":false,\"reason\":\"release 6.3.6 carries no x64 installer\"},\"error\":\"\"}";
         AdministratorUpdate.Preparation preparation = AdministratorUpdate.ParsePreparation(json);
         Assert.Equal("rollback", preparation.Step);
         Assert.True(preparation.BackupTaken);
         Assert.Equal("D:\\Backup", preparation.BackupPath);
         Assert.Equal("6.3.6", preparation.RollbackVersion);
         Assert.False(preparation.RollbackReady);
         Assert.Equal("Backed up to D:\\Backup.", AdministratorUpdate.BackupText(preparation));
         Assert.Contains("release 6.3.6 carries no x64 installer", AdministratorUpdate.RollbackText(preparation, "6.3.7"));

         Assert.Null(AdministratorUpdate.ParsePreparation(""));
         Assert.Null(AdministratorUpdate.ParsePreparation("{not json"));

         Assert.Contains("chose", AdministratorUpdate.BackupText(new AdministratorUpdate.Preparation { BackupSkipped = "chosen" }));
         Assert.Contains("UpdateBackupBeforeApply", AdministratorUpdate.BackupText(new AdministratorUpdate.Preparation { BackupSkipped = "off" }));
         Assert.Contains("no backup destination", AdministratorUpdate.BackupText(new AdministratorUpdate.Preparation { BackupSkipped = "no-destination" }));
         Assert.Contains("the disk is full", AdministratorUpdate.BackupFailedQuestion(new AdministratorUpdate.Preparation { BackupFailed = "the disk is full" }, "6.3.7", "6.3.6"));
      }

      [Fact]
      public void TheRefusalsNameWhatToDo()
      {
         Assert.Contains("open the hMailServer Control Panel on mail.example.com itself", AdministratorUpdate.RemoteText("mail.example.com", "NT SERVICE\\hMailServer"));
         Assert.Contains("Get-AuthenticodeSignature", AdministratorUpdate.MissingFileText("C:\\hm\\Bin\\hMailServer.Updater.exe", "6.3.7", "https://gitlab.com/hmailserver/hmailserver/-/releases/v6.3.7"));
         var finding = new InstallationPermissions.Finding
         {
            Failed = true,
            Object = "D:\\hMailServer",
            Principal = "NT AUTHORITY\\Authenticated Users",
            Right = "write data / create files",
            Inherited = true,
            InheritedFrom = "D:\\",
            Fix = "icacls \"D:\\hMailServer\" /inheritance:d && icacls \"D:\\hMailServer\" /remove:g *S-1-5-11"
         };
         string text = AdministratorUpdate.PermissionText(finding);
         Assert.Contains("icacls \"D:\\hMailServer\" /inheritance:d", text);
         Assert.Contains("inherited from D:\\", text);
         Assert.Contains("hMailServer 6.3.7, started at 2026-10-20T10:00:00Z by HOST\\admin",
            AdministratorUpdate.BusyText(new Dictionary<string, string> { ["version"] = "6.3.7", ["started"] = "2026-10-20T10:00:00Z", ["by"] = "HOST\\admin" }));
      }

      [Fact]
      public void AnInProgressMarkIsBelievedOnlyForAsLongAsAnInstallCanLast()
      {
         var now = new DateTime(2026, 10, 20, 12, 0, 0, DateTimeKind.Utc);
         Assert.True(AdministratorUpdate.InProgressMarkIsLive(now, now));
         Assert.True(AdministratorUpdate.InProgressMarkIsLive(now - TimeSpan.FromHours(4), now), "an install with its rollback can take four hours");
         Assert.True(AdministratorUpdate.InProgressMarkIsLive(now - AdministratorUpdate.InProgressLifetime, now));
         // Left by an Updater that did not finish: not a reason to refuse every install after it.
         Assert.False(AdministratorUpdate.InProgressMarkIsLive(now - AdministratorUpdate.InProgressLifetime - TimeSpan.FromSeconds(1), now));
         Assert.False(AdministratorUpdate.InProgressMarkIsLive(now - TimeSpan.FromDays(3), now));
         // A clock a little ahead is the same install; a mark from the far future is not one.
         Assert.True(AdministratorUpdate.InProgressMarkIsLive(now + TimeSpan.FromMinutes(1), now));
         Assert.False(AdministratorUpdate.InProgressMarkIsLive(now + TimeSpan.FromDays(1), now));
      }
   }
}
