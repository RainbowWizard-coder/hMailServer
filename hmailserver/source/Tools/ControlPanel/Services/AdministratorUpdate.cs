// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// An administrator's install of an update: what the Control Panel decides,
   /// asks for and reads (docs/ServiceAccount.md, "The live update").
   ///
   /// A service that runs as NT SERVICE\hMailServer cannot run an installer, so the
   /// Control Panel on the server installs the update itself: it asks Windows to
   /// run {app}\Bin\hMailServer.exe /InstallUpdate elevated (the stage, which
   /// copies the installer into a folder only administrators can write and checks
   /// it there), asks the service over COM to take the backup and the rollback
   /// image, tells the stage when the service is ready, asks the service to drain,
   /// and closes with <see cref="GoExitCode"/> so that the Updater the stage handed
   /// over to runs the installer - and starts this program again afterwards.
   ///
   /// Nothing here is trusted by anything elevated: the stage re-derives every path
   /// and re-checks every file. This is the Control Panel's half - which of those
   /// steps applies, the stage's command line, and the words for what the stage,
   /// the service and the Updater say - kept free of WPF and COM so that it can be
   /// tested.
   /// </summary>
   public static class AdministratorUpdate
   {
      /// <summary>"HMUP": the exit code that tells the Updater to install. Anything
      /// else is a cancellation. AdminUpdate::GoExitCode in the server.</summary>
      public const int GoExitCode = 0x484D5550;

      /// <summary>The stage's exit codes (AdminUpdate::StageExit).</summary>
      public static class StageExit
      {
         public const int HandedOver = 0;
         public const int Arguments = 2;
         public const int NotElevated = 3;
         public const int Permissions = 4;
         public const int Busy = 5;
         public const int Refused = 6;
         public const int Parent = 9;
         public const int TrustFile = 10;
         public const int DiskSpace = 11;
         public const int NameTaken = 12;
         public const int Cancelled = 13;
         public const int Failed = 14;
      }

      /// <summary>What a click on Install update does.</summary>
      public enum Decision
      {
         /// <summary>The service installs updates itself (LocalSystem), or the server is older than 6.3.6 and says what to do: Status.InstallUpdate, as before.</summary>
         ServiceInstalls,
         /// <summary>An administrator's install, from this Control Panel.</summary>
         Administrator,
         /// <summary>The server is another computer: its administrator installs there.</summary>
         Remote,
         /// <summary>No verified installer is waiting.</summary>
         NotReady,
         /// <summary>An install is already under way: being prepared (6), ready (7) or installing (4).</summary>
         UnderWay,
         /// <summary>The release on offer is not a version this can put on a command line.</summary>
         BadVersion
      }

      private static readonly Regex ReleaseVersion_ = new Regex(@"^[0-9]{1,9}(\.[0-9]{1,9}){1,3}(-[A-Za-z0-9.]{1,20})?$", RegexOptions.CultureInvariant);
      private static readonly Regex WorkId_ = new Regex("^[0-9a-f]{32}$", RegexOptions.CultureInvariant);

      /// <summary>
      /// The decision, from what the server said: needsAdministrator is
      /// Status.UpdateNeedsAdministrator, null when the server is older than 6.3.6
      /// and has no such property; state is Status.UpdateState.
      /// </summary>
      public static Decision Decide(bool isLocal, bool? needsAdministrator, int state, string version)
      {
         if (needsAdministrator != true)
            return Decision.ServiceInstalls;
         if (!isLocal)
            return Decision.Remote;
         // A second administrator - or a second click - while an install is
         // prepared, ready or running: not "nothing to install" (the smoke VM, 2
         // October 2026, said "No verified installer is waiting" then).
         if (state is 4 or 6 or 7)
            return Decision.UnderWay;
         if (state != 3)
            return Decision.NotReady;
         if (!IsReleaseVersion(version))
            return Decision.BadVersion;
         return Decision.Administrator;
      }

      public static bool IsReleaseVersion(string version) => version != null && ReleaseVersion_.IsMatch(version);

      public static bool IsWorkId(string id) => id != null && WorkId_.IsMatch(id);

      /// <summary>A new work id: 32 lowercase hexadecimal characters, random.</summary>
      public static string NewWorkId() => Convert.ToHexString(RandomNumberGenerator.GetBytes(16)).ToLowerInvariant();

      /// <summary>
      /// The stage's arguments: the release, the work folder's name, this process's
      /// id and the service's wait, each to its pattern, and the verb last. No path
      /// and nothing secret: the stage takes neither from its command line.
      /// </summary>
      public static string StageArguments(string version, string workId, int controlPanelProcessId, int waitSeconds)
      {
         if (!IsReleaseVersion(version))
            throw new ArgumentException("not a release version", nameof(version)); // no-loc: a programming error
         if (!IsWorkId(workId))
            throw new ArgumentException("not a work id", nameof(workId)); // no-loc: a programming error
         if (controlPanelProcessId <= 0)
            throw new ArgumentException("not a process id", nameof(controlPanelProcessId)); // no-loc: a programming error
         int wait = Math.Clamp(waitSeconds, 30, 3600);
         return string.Format(CultureInfo.InvariantCulture, "--update-version {0} --update-work {1} --control-panel {2} --update-wait {3} /InstallUpdate", // no-loc
            version, workId, controlPanelProcessId, wait);
      }

      /// <summary>The installation's folders, from the service's registered program ({app}\Bin\hMailServer.exe).</summary>
      public static bool InstallationFrom(string serviceProgram, out string app)
      {
         app = null;
         if (string.IsNullOrWhiteSpace(serviceProgram))
            return false;
         string bin = Path.GetDirectoryName(serviceProgram);
         if (string.IsNullOrEmpty(bin) || !string.Equals(Path.GetFileName(bin), "Bin", StringComparison.OrdinalIgnoreCase))
            return false;
         app = Path.GetDirectoryName(bin);
         return !string.IsNullOrEmpty(app);
      }

      public static string WorkFolder(string app, string workId) => Path.Combine(app, "AdminUpdate", workId);

      /// <summary>The files the install needs in the installation, which a damaged or partial one may lack.</summary>
      public static IEnumerable<string> RequiredFiles(string app)
      {
         yield return Path.Combine(app, "Bin", "hMailServer.exe");
         yield return Path.Combine(app, "Bin", "hMailServer.Updater.exe");
         yield return Path.Combine(app, "ControlPanel", "hMailCP.exe");
      }

      /// <summary>status.txt and outcome.txt's key=value lines. The first line of an outcome is not one.</summary>
      public static Dictionary<string, string> ParseKeyValues(string text)
      {
         var values = new Dictionary<string, string>(StringComparer.Ordinal);
         if (string.IsNullOrEmpty(text))
            return values;
         foreach (string raw in text.Split('\n'))
         {
            string line = raw.TrimEnd('\r');
            int equals = line.IndexOf('=');
            if (equals <= 0)
               continue;
            values[line.Substring(0, equals)] = line.Substring(equals + 1);
         }
         return values;
      }

      /// <summary>What the Updater wrote: the status, the release and the detail of
      /// its first line, and the key=value lines after it.</summary>
      public sealed class Outcome
      {
         public string Status { get; init; } = "";
         public string Version { get; init; } = "";
         public string Detail { get; init; } = "";
         public string Id { get; init; } = "";
         public string By { get; init; } = "";
         public string From { get; init; } = "";
         public string Finished { get; init; } = "";
         public string InstallLog { get; init; } = "";
         public string RollbackLog { get; init; } = "";
         public string Installer { get; init; } = "";
         public string RollbackImage { get; init; } = "";
      }

      private static readonly HashSet<string> Statuses_ = new HashSet<string>(StringComparer.Ordinal)
      {
         "ok", "failed", "rolled-back", "rollback-failed", "no-rollback", "upgraded-incomplete", "cancelled", "refused" // no-loc: the Updater's words
      };

      /// <summary>outcome.txt read; null when it is not one the Updater writes.</summary>
      public static Outcome ParseOutcome(string text)
      {
         if (string.IsNullOrEmpty(text))
            return null;
         string first = text.Split('\n')[0].TrimEnd('\r').Trim();
         int space = first.IndexOf(' ');
         if (space <= 0)
            return null;
         string status = first.Substring(0, space);
         if (!Statuses_.Contains(status))
            return null;
         string rest = first.Substring(space + 1);
         int second = rest.IndexOf(' ');
         int newline = text.IndexOf('\n');
         Dictionary<string, string> values = ParseKeyValues(newline < 0 ? "" : text.Substring(newline + 1));
         string Get(string key) => values.TryGetValue(key, out string value) ? value : "";
         return new Outcome
         {
            Status = status,
            Version = second > 0 ? rest.Substring(0, second) : rest,
            Detail = second > 0 ? rest.Substring(second + 1) : "",
            Id = Get("id"),
            By = Get("by"),
            From = Get("from"),
            Finished = Get("finished"),
            InstallLog = Get("install-log"),
            RollbackLog = Get("rollback-log"),
            Installer = Get("installer"),
            RollbackImage = Get("rollback-image")
         };
      }

      /// <summary>Whether an outcome is a good one, for the notice's level.</summary>
      public static StatusLevel LevelOf(Outcome outcome)
      {
         switch (outcome?.Status)
         {
            case "ok": return StatusLevel.Good;
            case "rollback-failed":
            case "no-rollback":
            // The database was changed and the install did not complete: nothing
            // was put back, and the service may not be running.
            case "upgraded-incomplete": return StatusLevel.Critical;
            default: return StatusLevel.Warning;
         }
      }

      /// <summary>The sentence for an outcome: O-ok and the rest of the design's texts.</summary>
      public static string OutcomeText(Outcome outcome)
      {
         if (outcome == null)
            return L("The install's outcome could not be read.");
         string log = string.IsNullOrEmpty(outcome.InstallLog) ? "-" : outcome.InstallLog;
         switch (outcome.Status)
         {
            case "ok":
               return F("hMailServer has been updated from {0} to {1}, and the service is running. Installed by {2} at {3}.", outcome.From, outcome.Version, outcome.By, outcome.Finished);
            case "failed":
               return F("hMailServer {0} was not installed: {1}. hMailServer {2} is still running, unchanged. The installer's log: {3}", outcome.Version, outcome.Detail, outcome.From, log);
            case "rolled-back":
               return F("hMailServer {0} did not start, so {1} was put back and is running: {2}. The installer's log: {3}", outcome.Version, outcome.From, outcome.Detail, log);
            case "no-rollback":
               return F("hMailServer {0} did not start, and there was no earlier installer to put back: {1}. Reinstall hMailServer {2} from its release page as an administrator. The installer's log: {3}", outcome.Version, outcome.Detail, outcome.From, log);
            case "rollback-failed":
               return F("hMailServer {0} did not start, and putting {1} back failed too: {2}. Run {3} as an administrator to reinstall {1}. The logs: {4}", outcome.Version, outcome.From, outcome.Detail,
                  string.IsNullOrEmpty(outcome.RollbackImage) ? "-" : outcome.RollbackImage, log + (string.IsNullOrEmpty(outcome.RollbackLog) ? "" : ", " + outcome.RollbackLog));
            case "upgraded-incomplete":
               // The installer's database step changed the database, or could not
               // show that it left it unchanged (exit code 11, or its log said so), and
               // a step did not complete: the Updater puts nothing back, since the
               // earlier version may refuse that database. The detail says which.
               // The first sentence stands alone, as the sign-in banner shows it.
               return F("hMailServer {0} is installed and its database step changed the database, or may have, but the install did not complete, so {1} was not put back: it may not run on that database. What did not complete: {2}. The installer's log: {3}",
                  outcome.Version, outcome.From, outcome.Detail, log);
            case "cancelled":
               return F("The install of hMailServer {0} was cancelled: {1}", outcome.Version, outcome.Detail);
            default:
               return F("hMailServer {0} was not installed: {1}. Nothing was changed.", outcome.Version, outcome.Detail);
         }
      }

      /// <summary>B-signin: the banner of the sign-in page after an install, when
      /// the Control Panel could not sign in again by itself.</summary>
      public static string SignInBanner(Outcome outcome) => F("hMailServer was updated: {0} Sign in to see the server.", FirstSentence(OutcomeText(outcome)));

      /// <summary>The text up to its first full stop: ". ", or the ideographic "。"
      /// the Japanese and Chinese catalogues end a sentence with - without which the
      /// banner in those languages ran on to the first ". " in the Updater's English
      /// detail, or to the end.</summary>
      public static string FirstSentence(string text)
      {
         if (string.IsNullOrEmpty(text))
            return "";
         int end = text.IndexOf(". ", StringComparison.Ordinal);
         int ideographic = text.IndexOf('。');
         if (ideographic > 0 && (end < 0 || ideographic < end))
            return text.Substring(0, ideographic + 1);
         return end > 0 ? text.Substring(0, end + 1) : text;
      }

      /// <summary>Why the stage stopped, from its exit code and what status.txt said.</summary>
      public static string StageStoppedText(int exitCode, IReadOnlyDictionary<string, string> status, string version)
      {
         status ??= new Dictionary<string, string>();
         string Get(string key) => status.TryGetValue(key, out string value) ? value : "";

         if (exitCode == StageExit.Refused || Get("state") == "refused")
         {
            if (Get("revocation") == "offline")
               return F("hMailServer {0} was not installed: the revocation server {1} could not be reached, so whether the installer's certificate is revoked could not be checked. Nothing was changed. Click Install update again when it can be reached, or install hMailServer {0} by hand.",
                  version, string.IsNullOrEmpty(Get("crl-host")) ? L("named in its certificate") : Get("crl-host"));
            // A copy that could not be made (a link on the way, a file too large)
            // leaves nothing to inspect, and the stage names no folder for it.
            if (string.IsNullOrEmpty(Get("kept")))
               return F("hMailServer {0} was not installed: the installer did not pass the check - {1}. Nothing was changed and the server runs as it did. Download it again.",
                  version, Clause(Get("reason")));
            return F("hMailServer {0} was not installed: the installer did not pass the check - {1}. Nothing was changed and the server runs as it did. Download it again; if the check fails again, the copy is kept in {2} for inspection.",
               version, Clause(Get("reason")), Get("kept"));
         }

         string why;
         switch (exitCode)
         {
            case StageExit.Arguments: why = L("the program Windows was asked to run is not the one the hMailServer service is registered with, or was given something it does not take"); break;
            case StageExit.NotElevated: why = L("Windows did not run it as an administrator"); break;
            case StageExit.Permissions: why = L("the permissions of the installation changed after the check; click Install update again to see which"); break;
            case StageExit.Busy: why = L("another administrator's install of an update is under way on this server"); break;
            case StageExit.Parent: why = L("Windows did not start it from this Control Panel"); break;
            case StageExit.TrustFile: why = L("a trust file hMailServer.INI names may be changed by someone other than the administrators"); break;
            case StageExit.DiskSpace: why = L("there is less than 1 GB free on the installation's drive"); break;
            case StageExit.NameTaken: why = L("its work folder was already there"); break;
            case StageExit.Cancelled: return L("The update was stopped; nothing was installed.");
            default:
               why = string.IsNullOrEmpty(Get("reason")) ? F("it ended with code {0}", exitCode) : Clause(Get("reason"));
               break;
         }
         return F("Install update stopped before anything was changed: {0}.", why);
      }

      /// <summary>
      /// A reason the stage wrote, as a clause inside a sentence that ends it: without
      /// the full stop of its own that some carry - the version check's and the
      /// Sigstore verifier's sentences end in one - so that the sentence does not end
      /// in two.
      /// </summary>
      public static string Clause(string reason) => (reason ?? "").TrimEnd().TrimEnd('.').TrimEnd();

      /// <summary>
      /// Why the install did not start when Windows would not start the elevated
      /// stage: the prompt declined (ERROR_CANCELLED), or Windows' own reason - for
      /// a standard user whose requests to elevate are denied by policy, "This
      /// program is blocked by group policy" (ERROR_ACCESS_DISABLED_BY_POLICY, as
      /// on the smoke VM on 1 October 2026) - followed by what that means here.
      /// </summary>
      public static string ElevationFailedText(int error)
      {
         const int cancelled = 1223;
         if (error == cancelled)
            return L("The Windows prompt was declined, so nothing was installed and nothing on the server was changed.");
         return Sentences(F("Windows could not start the install: {0}", new System.ComponentModel.Win32Exception(error).Message.TrimEnd()), L("Nothing was changed."));
      }

      /// <summary>
      /// Whether the install window's timer may take the install a step further:
      /// not once it has finished, and not while it asks the administrator a
      /// question (stop the update? install without a backup?), whose modal message
      /// loop goes on running the timer - the install would otherwise reach the
      /// installer, or be cancelled, under the question.
      /// </summary>
      public static bool InstallMayAdvance(bool finished, bool askingTheAdministrator) => !finished && !askingTheAdministrator;

      /// <summary>Install update clicked while an install is already under way on the
      /// server: the stage's own refusal of a second one, said before Windows is asked.</summary>
      public static string UnderWayText() =>
         F("Install update stopped before anything was changed: {0}.", L("another administrator's install of an update is under way on this server"));

      /// <summary>R-remote: a Control Panel connected to another computer.</summary>
      public static string RemoteText(string host, string account) =>
         F("This Control Panel is connected to {0}, not to the computer it runs on. The hMailServer service on {0} runs as {1}, which cannot run an installer, so updates there are installed by an administrator at that server: open the hMailServer Control Panel on {0} itself and click Install update there.",
            host, string.IsNullOrEmpty(account) ? "NT SERVICE\\hMailServer" : account); // no-loc: an account name

      /// <summary>R-files: a file the install needs is missing from the installation.</summary>
      public static string MissingFileText(string file, string version, string releaseUrl) =>
         F("This installation has no {0}, so it cannot install the update itself. Download hMailServer {1} from {2}, check that Get-AuthenticodeSignature reports it Valid and signed by Progressive Robot Ltd, and run it as an administrator.",
            file, version, string.IsNullOrEmpty(releaseUrl) ? "https://gitlab.com/hmailserver/hmailserver/-/releases" : releaseUrl); // no-loc: the project's release page

      /// <summary>R-copy: this Control Panel is not the installation's own.</summary>
      public static string NotInstalledCopyText(string own, string expected) =>
         F("This Control Panel runs from {0}, not from the installation's own Control Panel ({1}). Open the Control Panel there and click Install update.", own, expected);

      /// <summary>
      /// The longest an administrator's install can be under way: the Updater's
      /// wait for this program to close (15 minutes), the installer's and the
      /// rollback image's hour each, and the service's wait after each. A mark of
      /// one in progress older than this is left by an Updater that did not
      /// finish - a power cut, a forced sign-out - and it would otherwise refuse
      /// every install after it, since only an administrator can remove it.
      /// </summary>
      public static readonly TimeSpan InProgressLifetime = TimeSpan.FromHours(5);

      /// <summary>
      /// Whether AdminUpdate\in-progress, written at writtenUtc, says an install is
      /// under way now. A stale one is not believed: the stage, which holds the
      /// installation's busy file for as long as an install lives, refuses a
      /// second install itself (exit 5) and the Updater writes the mark afresh.
      /// </summary>
      public static bool InProgressMarkIsLive(DateTime writtenUtc, DateTime nowUtc) =>
         nowUtc - writtenUtc <= InProgressLifetime && writtenUtc - nowUtc <= TimeSpan.FromMinutes(5);

      /// <summary>How often, and how many times, the Updates page of a Control Panel
      /// opened again after an install asks the service again while it still says
      /// the install is under way.</summary>
      public static readonly TimeSpan SettleInterval = TimeSpan.FromSeconds(2);
      public const int SettleAttempts = 30;

      /// <summary>
      /// Whether the Updates page of a Control Panel the Updater opened again should
      /// ask the service again in a moment. An install that ended without stopping
      /// the service - refused, cancelled, or failed before the installer stopped
      /// anything - leaves the service saying "installing" (UpdateState 4) until it
      /// has read the outcome, which it looks for every five seconds and believes
      /// when it has seen it twice; the Control Panel is opened again the moment the
      /// outcome is written, so its first look can come first, and would otherwise
      /// show "Installing" with Install update off under an outcome that says
      /// nothing was installed (seen on the smoke VM on 1 October 2026). Only after
      /// an install (an outcome to show), only while the state is 4, and at most
      /// SettleAttempts times.
      /// </summary>
      public static bool ShouldAskAgainAfterInstall(bool reopenedAfterInstall, int updateState, int attemptsSoFar) =>
         reopenedAfterInstall && updateState == 4 && attemptsSoFar < SettleAttempts;

      /// <summary>R-busy: another administrator's install is under way.</summary>
      public static string BusyText(IReadOnlyDictionary<string, string> inProgress)
      {
         string Get(string key) => inProgress != null && inProgress.TryGetValue(key, out string value) ? value : "?";
         return F("An update is already being installed on this server: hMailServer {0}, started at {1} by {2}.", Get("version"), Get("started"), Get("by"));
      }

      /// <summary>R-perm: the permission rule's finding, with the command that removes it.</summary>
      public static string PermissionText(InstallationPermissions.Finding finding)
      {
         if (!string.IsNullOrEmpty(finding.Reason))
            return F("Install update stopped before anything was changed: the permissions of {0} could not be checked ({1}).", finding.Object, finding.Reason);
         string text = F("Install update stopped before anything was changed: {0} can be changed by {1} ({2}). A program run from there as an administrator could be one that account put there. Remove that permission - from a Command Prompt run as administrator: {3} - and click Install update again.",
            finding.Object, finding.Principal, finding.Right, finding.Fix);
         if (finding.Inherited && !string.IsNullOrEmpty(finding.InheritedFrom))
            text = Sentences(text, F("The permission is inherited from {0}; the command stops the folder inheriting it.", finding.InheritedFrom));
         return text;
      }

      /// <summary>
      /// The service's preparation document (Status.UpdatePreparation) read into
      /// what the progress window shows: the step, the backup and the rollback image.
      /// </summary>
      public sealed class Preparation
      {
         public string Step { get; init; } = "";
         public bool BackupTaken { get; init; }
         public bool BackupRunning { get; init; }
         public string BackupSkipped { get; init; } = "";
         public string BackupFailed { get; init; } = "";
         public string BackupPath { get; init; } = "";
         public string RollbackVersion { get; init; } = "";
         public bool RollbackReady { get; init; }
         public string RollbackReason { get; init; } = "";
         public string Error { get; init; } = "";
      }

      public static Preparation ParsePreparation(string json)
      {
         if (string.IsNullOrWhiteSpace(json))
            return null;
         try
         {
            using var document = System.Text.Json.JsonDocument.Parse(json);
            var root = document.RootElement;
            string Text(System.Text.Json.JsonElement element, string name) =>
               element.ValueKind == System.Text.Json.JsonValueKind.Object && element.TryGetProperty(name, out var value) && value.ValueKind == System.Text.Json.JsonValueKind.String ? value.GetString() : "";
            bool Flag(System.Text.Json.JsonElement element, string name) =>
               element.ValueKind == System.Text.Json.JsonValueKind.Object && element.TryGetProperty(name, out var value) && value.ValueKind == System.Text.Json.JsonValueKind.True;
            var backup = root.TryGetProperty("backup", out var b) ? b : default;
            var rollback = root.TryGetProperty("rollback", out var r) ? r : default;
            return new Preparation
            {
               Step = Text(root, "step"),
               BackupTaken = Flag(backup, "taken"),
               BackupRunning = Flag(backup, "running"),
               BackupSkipped = Text(backup, "skipped"),
               BackupFailed = Text(backup, "failed"),
               BackupPath = Text(backup, "path"),
               RollbackVersion = Text(rollback, "version"),
               RollbackReady = Flag(rollback, "ready"),
               RollbackReason = Text(rollback, "reason"),
               Error = Text(root, "error")
            };
         }
         catch (System.Text.Json.JsonException)
         {
            return null;
         }
      }

      /// <summary>What the backup step says, once it is decided.</summary>
      public static string BackupText(Preparation preparation)
      {
         if (preparation == null)
            return "";
         if (preparation.BackupTaken)
            return F("Backed up to {0}.", preparation.BackupPath);
         if (preparation.BackupRunning)
            return F("Backing up to {0}.", preparation.BackupPath);
         switch (preparation.BackupSkipped)
         {
            case "chosen": return L("No backup first: you chose to install without one.");
            case "off": return L("No backup first: 'Back up to the configured destination before an unattended apply' is off.");
            case "no-destination": return L("No backup first: no backup destination is set (Backup & restore). A rollback puts the previous program back, not the database.");
            default: return "";
         }
      }

      /// <summary>Q-backup: the question after a backup failed.</summary>
      public static string BackupFailedQuestion(Preparation preparation, string version, string running) =>
         F("The backup before the update failed: {0}. See the backup log. Install hMailServer {1} without a backup? A rollback would put {2} back, but not the database as it was.",
            preparation?.BackupFailed ?? "", version, running);

      /// <summary>What the rollback step says, once it is decided.</summary>
      public static string RollbackText(Preparation preparation, string version)
      {
         if (preparation == null || string.IsNullOrEmpty(preparation.RollbackVersion))
            return "";
         return preparation.RollbackReady
            ? F("hMailServer {0} is ready to be put back if needed.", preparation.RollbackVersion)
            : F("None: {0}. If {1} does not start, it will need reinstalling by hand.", preparation.RollbackReason, version);
      }

      /// <summary>The Common Name of an X.500 subject ("CN=Progressive Robot Ltd, O=...").</summary>
      public static string CommonNameOf(string subject)
      {
         if (string.IsNullOrEmpty(subject))
            return "";
         foreach (string part in subject.Split(','))
         {
            string trimmed = part.Trim();
            if (trimmed.StartsWith("CN=", StringComparison.OrdinalIgnoreCase))
               return trimmed.Substring(3);
         }
         return subject;
      }

      /// <summary>Reads a small text file another process may be writing, shared.</summary>
      public static string ReadShared(string path, int most = 64 * 1024)
      {
         try
         {
            using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            if (stream.Length > most)
               return null;
            using var reader = new StreamReader(stream, Encoding.UTF8);
            return reader.ReadToEnd();
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            return null;
         }
      }
   }
}
