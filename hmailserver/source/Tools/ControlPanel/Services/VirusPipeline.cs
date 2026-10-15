// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// One scanner, as the server will actually run it.
   ///
   /// "Configured" and "will run" are not the same thing here, and the gap between
   /// them is the reason this page exists: a scanner that is switched on with a
   /// piece of its configuration missing does not fail the message, it reports an
   /// error to the log and the message is delivered as though it were clean.
   /// </summary>
   public sealed class VirusScannerEntry
   {
      internal VirusScannerEntry(string key, int order, string name, bool enabled, bool usable,
         string detail, string problem, string page, bool unread = false, string shortName = null)
      {
         Key = key;
         Order = order;
         Name = name;
         ShortName = shortName ?? name;
         Unread = unread;
         // A scanner whose settings could not be read is neither on nor off, and
         // neither usable nor broken: the row says "Not read", and nothing that
         // follows from the scanner is said.
         Enabled = !unread && enabled;
         Usable = !unread && usable;
         Detail = unread ? L("These settings could not be read.") : detail;
         Problem = unread ? null : problem;
         Page = page;
      }

      /// <summary>Stable identifier, so a test can name a scanner without matching its wording.</summary>
      public string Key { get; }

      /// <summary>
      /// Position in the order VirusScanner::ScanFile_ tries them, 1-based. The
      /// order is fixed in the source - ClamWin, then the custom scanner, then
      /// ClamAV - and it matters because the first scanner to find something ends
      /// the scan, so a slow scanner in front of a fast one costs every message.
      /// </summary>
      public int Order { get; }

      public string Name { get; }

      /// <summary>
      /// The name in a sentence that lists the scanners - ClamWin, Custom scanner,
      /// ClamAV - where <see cref="Name"/> says what each is as well, in brackets
      /// that a list of three would run together (review finding F265).
      /// </summary>
      public string ShortName { get; }

      /// <summary>Whether the administrator has switched it on.</summary>
      public bool Enabled { get; }

      /// <summary>
      /// Whether it can actually run: switched on AND with the settings it needs.
      /// An enabled scanner that is not usable is the dangerous state, because
      /// nothing about the configuration looks wrong from the settings page.
      /// </summary>
      public bool Usable { get; }

      /// <summary>Where it scans, in one line - the host and port, or the executable.</summary>
      public string Detail { get; }

      /// <summary>What is missing, when <see cref="Usable"/> is false and <see cref="Enabled"/> is true. Otherwise null.</summary>
      public string Problem { get; }

      /// <summary>Navigation key of the page that owns these settings.</summary>
      public string Page { get; }

      /// <summary>
      /// Whether the scanner's settings could not be read, so that neither
      /// <see cref="Enabled"/> nor <see cref="Usable"/> is a fact about the server.
      /// </summary>
      public bool Unread { get; }

      /// <summary>"On", "Off", "On, but cannot run" or "Not read" - the word a reader gets when the colour is not available.</summary>
      public string StateText =>
         Unread ? L("Not read") : !Enabled ? L("Off") : Usable ? L("On") : L("On, but cannot run");
   }

   /// <summary>
   /// Something worth saying about the virus configuration as a whole, carrying a
   /// <see cref="StatusLevel"/> so the page presents it with the shared colour,
   /// shape and severity word rather than inventing a third vocabulary.
   /// </summary>
   public sealed class VirusPipelineNote
   {
      internal VirusPipelineNote(StatusLevel level, string text, string page = null)
      {
         Level = level;
         Text = text;
         Page = page ?? VirusPipeline.NotesPage;
      }

      public StatusLevel Level { get; }

      /// <summary>The whole note, in a sentence or two. Never empty.</summary>
      public string Text { get; }

      /// <summary>
      /// The navigation key of the page that puts the note right, which a note at
      /// the Warning or Critical level opens from the overview: Anti-virus
      /// settings unless the note names another.
      /// </summary>
      public string Page { get; }
   }

   /// <summary>What the server does with a message a scanner has condemned.</summary>
   public enum VirusAction
   {
      /// <summary>eAntivirusAction.hDeleteEmail - the message is not delivered.</summary>
      DeleteMessage = 0,

      /// <summary>eAntivirusAction.hDeleteAttachments - the attachments are stripped and the message IS delivered.</summary>
      StripAttachments = 1
   }

   /// <summary>
   /// A snapshot of the anti-virus configuration, read from the COM settings tree.
   ///
   /// Plain properties with no behaviour, so <see cref="VirusPipeline"/> can be
   /// tested without a server: every interesting case - a scanner enabled with no
   /// host, a size limit that silently skips large messages, notifications sent to
   /// a forged sender - is a state a live server can be in and a tedious one to
   /// produce against a real one.
   /// </summary>
   public sealed class VirusPipelineConfig
   {
      public bool ClamAvEnabled { get; set; }
      public string ClamAvHost { get; set; } = "";
      public int ClamAvPort { get; set; }

      public bool ClamWinEnabled { get; set; }
      public string ClamWinExecutable { get; set; } = "";
      public string ClamWinDatabaseFolder { get; set; } = "";

      public bool CustomScannerEnabled { get; set; }
      public string CustomScannerExecutable { get; set; } = "";
      public int CustomScannerVirusReturnValue { get; set; }

      /// <summary>
      /// AntiVirus.MaximumMessageSize, in KB. **0 means no limit** - the comparison
      /// in VirusScanner::Scan is `limit > 0 && messageKB > limit`, so zero is the
      /// safe value here and any positive number is a hole. That is the opposite
      /// way round from most size limits, which is exactly why it is worth a page.
      /// </summary>
      public int MaxScanKilobytes { get; set; }

      public VirusAction Action { get; set; }
      public bool NotifySender { get; set; }
      public bool NotifyRecipient { get; set; }

      public bool AttachmentBlockingEnabled { get; set; }
      public int BlockedAttachmentPatterns { get; set; }

      /// <summary>External POP3 fetch accounts that have their own anti-virus switch turned off.</summary>
      public int FetchAccountsWithScanningOff { get; set; }

      /// <summary>
      /// AVFailAction ([Settings] in hMailServer.INI) is 1: a message no scanner could
      /// examine is held in the queue and scanned again, and returned to its sender
      /// when the holds run out (SMTPDeliverer), instead of delivered as though it
      /// had been found clean. 0, the shipped default, delivers it.
      /// </summary>
      public bool HoldUnscanned { get; set; }

      /// <summary>How many external POP3 fetch accounts exist at all.</summary>
      public int FetchAccountsTotal { get; set; }

      /// <summary>
      /// True when the fetch-account walk gave up before it had seen every account.
      ///
      /// There is no server-wide collection of fetch accounts - they hang off each
      /// account - so counting them means walking every account on the server, and a
      /// page that does that unbounded stops responding on a large installation. The
      /// walk is capped, and when it is capped the numbers above are a floor rather
      /// than a total, which the wording has to reflect: a page that says "2 accounts"
      /// when it means "at least 2 of the first thousand" is worse than one that
      /// admits it stopped looking.
      /// </summary>
      public bool FetchAccountScanIncomplete { get; set; }

      /// <summary>
      /// Settings.SandboxEnabled: attachments of a submitted type in mail from
      /// outside are sent to a sandbox and the message held for its verdict
      /// (docs/Sandbox.md) - after the anti-virus scan and before the rules.
      /// </summary>
      public bool SandboxEnabled { get; set; }

      /// <summary>Settings.SandboxSubmitUrl. Empty holds and submits nothing, whatever the switch says.</summary>
      public string SandboxSubmitUrl { get; set; } = "";

      /// <summary>Settings.SandboxVerdictUrl. Empty holds and submits nothing, whatever the switch says.</summary>
      public string SandboxVerdictUrl { get; set; } = "";

      /// <summary>
      /// Settings.SandboxTimeoutAction is 1: a hold the sandbox did not answer for in
      /// time, or answered error, is returned to its sender (5.7.1); 0, the default,
      /// delivers it with a header saying so.
      /// </summary>
      public bool SandboxTimeoutReturns { get; set; }

      /// <summary>
      /// QuarantineEnabled ([Settings]): a malicious verdict quarantines the message.
      /// Off, as shipped, it is returned to its sender with 5.7.1 instead, since the
      /// quarantine's digest and release are off with it (docs/Sandbox.md).
      /// </summary>
      public bool QuarantineEnabled { get; set; }

      /// <summary>
      /// The properties that could not be read, by name, kept by the page that
      /// fills this snapshot. A value that could not be read is the type's
      /// default, which here is a real configuration - a threshold of 0, a check
      /// that is off - so nothing that follows from one may be said as a fact:
      /// the row is "Not read", the verdict is left out, and so is every note
      /// that depends on it.
      /// </summary>
      public HashSet<string> Unread { get; } = new(StringComparer.Ordinal);

      /// <summary>Whether every one of these properties was read.</summary>
      public bool WasRead(params string[] properties)
      {
         foreach (string property in properties)
         {
            if (Unread.Contains(property))
               return false;
         }

         return true;
      }
   }

   /// <summary>
   /// What the anti-virus configuration will actually do to a message, derived from
   /// the settings rather than from what the settings are called.
   ///
   /// No WPF here on purpose: every judgement this page makes is a pure function of
   /// a <see cref="VirusPipelineConfig"/> and is tested directly. The view reads the
   /// COM settings into the snapshot and draws what it is told.
   ///
   /// The behaviour being described lives in `VirusScanner::ScanFile_`,
   /// `VirusScanner::Scan` and `SMTPDeliverer::RunVirusProtection_`, and the one
   /// thing worth carrying away from all three is this: **a scanner that errors is
   /// not a message that fails**. ScanFile_ reports the error through ErrorManager
   /// at Medium and carries on to the next scanner; if every scanner errors, the
   /// result is `NoVirusFound` and the message is delivered exactly as if it had
   /// been examined and found clean. So "enabled" is never the question. "Can it
   /// run" is.
   /// </summary>
   public static class VirusPipeline
   {
      /// <summary>
      /// The concurrency cap in VirusScanner::MaxRunningScanners. Named here so the
      /// page can state it rather than describing "a limit".
      /// </summary>
      public const int MaxConcurrentScans = 10;

      /// <summary>
      /// Every scanner, in the order VirusScanner::ScanFile_ tries them.
      /// </summary>
      public static IReadOnlyList<VirusScannerEntry> Scanners(VirusPipelineConfig config)
      {
         var scanners = new List<VirusScannerEntry>();

         if (config == null)
            return scanners;

         // A scanner whose settings could not be read is "Not read", not off and
         // not broken: the snapshot holds a blank host or a false switch for a
         // value the server did not answer, and a blank drawn as a setting would
         // call a working scanner unusable.
         bool Unread(params string[] properties) => !config.WasRead(properties);

         // 1. ClamWin. Two paths, and both are needed: ClamWinVirusScanner passes
         //    the database folder to the executable on the command line.
         bool clamWinUsable = !IsBlank(config.ClamWinExecutable) && !IsBlank(config.ClamWinDatabaseFolder);
         scanners.Add(new VirusScannerEntry(
            "clamwin", 1, L("ClamWin"),
            config.ClamWinEnabled,
            config.ClamWinEnabled && clamWinUsable,
            IsBlank(config.ClamWinExecutable) ? L("No executable set") : config.ClamWinExecutable,
            !config.ClamWinEnabled ? null
               : IsBlank(config.ClamWinExecutable) && IsBlank(config.ClamWinDatabaseFolder)
                  ? L("Neither the executable nor the signature database folder is set.")
                  : IsBlank(config.ClamWinExecutable) ? L("No executable path is set.")
                  : IsBlank(config.ClamWinDatabaseFolder) ? L("No signature database folder is set.")
                  : null,
            "antivirus",
            Unread(nameof(config.ClamWinEnabled), nameof(config.ClamWinExecutable), nameof(config.ClamWinDatabaseFolder)),
            "ClamWin")); // no-loc: the product's name

         // 2. The custom scanner. Only the executable is required; the return value
         //    that means "infected" is a number and 0 is a legitimate choice, so it
         //    is never treated as missing.
         scanners.Add(new VirusScannerEntry(
            "custom", 2, L("Custom scanner"),
            config.CustomScannerEnabled,
            config.CustomScannerEnabled && !IsBlank(config.CustomScannerExecutable),
            IsBlank(config.CustomScannerExecutable)
               ? L("No executable set")
               : F("{0} - exit code {1} means infected", config.CustomScannerExecutable, config.CustomScannerVirusReturnValue),
            config.CustomScannerEnabled && IsBlank(config.CustomScannerExecutable)
               ? L("No executable path is set.")
               : null,
            "antivirus",
            Unread(nameof(config.CustomScannerEnabled), nameof(config.CustomScannerExecutable), nameof(config.CustomScannerVirusReturnValue)),
            L("Custom scanner")));

         // 3. ClamAV over the network (clamd INSTREAM). A port of 0 is not a
         //    default, it is an address nothing listens on.
         bool clamAvUsable = !IsBlank(config.ClamAvHost) && config.ClamAvPort > 0 && config.ClamAvPort <= 65535;
         scanners.Add(new VirusScannerEntry(
            "clamav", 3, L("ClamAV (clamd)"),
            config.ClamAvEnabled,
            config.ClamAvEnabled && clamAvUsable,
            IsBlank(config.ClamAvHost)
               ? L("No host set")
               : config.ClamAvHost + ":" + config.ClamAvPort,
            !config.ClamAvEnabled ? null
               : IsBlank(config.ClamAvHost) ? L("No host name or address is set.")
               : config.ClamAvPort <= 0 ? L("The port is 0, which nothing can listen on.")
               : config.ClamAvPort > 65535 ? L("The port is outside the range 1-65535.")
               : null,
            "antivirus",
            Unread(nameof(config.ClamAvEnabled), nameof(config.ClamAvHost), nameof(config.ClamAvPort)),
            "ClamAV")); // no-loc: the product's name

         return scanners;
      }

      /// <summary>Whether every scanner's settings were read, without which no verdict can be given.</summary>
      public static bool ScannersRead(VirusPipelineConfig config)
         => config != null && !Scanners(config).Any(scanner => scanner.Unread);

      /// <summary>The page a note is about unless it names another.</summary>
      public const string NotesPage = "antivirus";

      /// <summary>
      /// The level the verdict is shown at, from what it says: Critical when no
      /// scanner can run; a Warning when a larger message is delivered unscanned
      /// or an infected one is delivered with its attachments stripped; Good
      /// otherwise; Information when it cannot be said. "No message is scanned
      /// for viruses... Everything is delivered as it arrives." was plain body
      /// text with no level.
      /// </summary>
      public static StatusLevel VerdictLevel(VirusPipelineConfig config)
      {
         if (config == null || !ScannersRead(config) || !config.WasRead(nameof(config.Action), nameof(config.MaxScanKilobytes),
                nameof(config.AttachmentBlockingEnabled), nameof(config.BlockedAttachmentPatterns)))
            return StatusLevel.Information;

         if (CountUsable(config) == 0)
            return StatusLevel.Critical;

         return config.MaxScanKilobytes > 0 || config.Action == VirusAction.StripAttachments ? StatusLevel.Warning : StatusLevel.Good;
      }

      /// <summary>
      /// Whether the notes say what the verdict says: at the Warning level and
      /// above - no scanner that can run, a larger message delivered unscanned,
      /// an infected one delivered with its attachments stripped - a note at the
      /// verdict's level says it, with the page that puts it right. The overview
      /// then draws the verdict as the card's lead sentence, at no level and with
      /// no action - it still says what happens to every message, "scanned by
      /// one scanner", which no note does - and those notes are the only notices
      /// at the Warning level and above, as the count beside the title counts
      /// them: as a notice of its own the verdict stood above the notes that said
      /// it, at their level and with the same Open Anti-virus settings, one
      /// notice more than the count. A verdict that is Good, or that cannot be
      /// said, has no note to say it and is the notice.
      /// </summary>
      public static bool NotesSayTheVerdict(VirusPipelineConfig config)
         => VerdictLevel(config) >= StatusLevel.Warning;

      /// <summary>
      /// One sentence answering "what will happen to an infected message", written
      /// for somebody who has just arrived at the page.
      /// </summary>
      public static string Verdict(VirusPipelineConfig config)
      {
         if (config == null)
            return L("The anti-virus configuration could not be read.");

         // A verdict from a value the server did not answer - a scanner read as
         // off, a size limit read as 0 - is not given.
         if (!ScannersRead(config) || !config.WasRead(nameof(config.Action), nameof(config.MaxScanKilobytes),
                nameof(config.AttachmentBlockingEnabled), nameof(config.BlockedAttachmentPatterns)))
            return L("What happens to an infected message cannot be said: some of the settings could not be read.");

         int usable = CountUsable(config);

         // Every sentence here is one whole text for one scanner or pattern and
         // one for several: a counted noun translated apart from its sentence
         // took a Latin "s" in Japanese and could not be declined in Polish.
         if (usable == 0)
         {
            if (!config.AttachmentBlockingEnabled || config.BlockedAttachmentPatterns <= 0)
               return L("No message is scanned for viruses, and no attachment is blocked. Everything is delivered as it arrives.");

            return config.BlockedAttachmentPatterns == 1
               ? L("No message is scanned for viruses. Attachment blocking is still stripping one file-name pattern, but nothing examines the contents of a message.")
               : Plural(config.BlockedAttachmentPatterns,
                  "No message is scanned for viruses. Attachment blocking is still stripping {0} file-name pattern, but nothing examines the contents of a message.",
                  "No message is scanned for viruses. Attachment blocking is still stripping {0} file-name patterns, but nothing examines the contents of a message.");
         }

         // The scanners by name, in the order the server tries them, where the
         // verdict counted them - "scanned by 2 scanners" sent the reader to the
         // card below to find out which. With a size limit the sentence says
         // "within the size limit" and no more: the limit and what becomes of a
         // larger message are the Warning under the verdict, and the next card's
         // first line, which the verdict said a third time (review finding F265).
         // A sentence for each count, the names after a colon, so that no
         // language has to decline a name it cannot.
         List<string> names = Scanners(config).Where(scanner => scanner.Usable).Select(scanner => scanner.ShortName).ToList();
         bool limited = config.MaxScanKilobytes > 0;
         string scanned = names.Count switch
         {
            1 => limited
               ? F("Messages within the size limit are scanned by one scanner: {0}.", names[0])
               : F("Every message is scanned by one scanner: {0}.", names[0]),
            2 => limited
               ? F("Messages within the size limit are scanned by two scanners in turn: {0}, then {1}.", names[0], names[1])
               : F("Every message is scanned by two scanners in turn: {0}, then {1}.", names[0], names[1]),
            _ => limited
               ? F("Messages within the size limit are scanned by three scanners in turn: {0}, then {1}, then {2}.", names[0], names[1], names[2])
               : F("Every message is scanned by three scanners in turn: {0}, then {1}, then {2}.", names[0], names[1], names[2])
         };

         // The first sentence is whole, and the second is written whole in the
         // text that places it, so that a language with no space between
         // sentences can join them its own way.
         return config.Action == VirusAction.DeleteMessage
            ? F("{0} If a virus is found, the whole message is deleted and never delivered.", scanned)
            : F("{0} If a virus is found, the attachments are stripped and the message is still delivered.", scanned);
      }

      /// <summary>
      /// The attachment sandbox (docs/Sandbox.md) as the overview's card says it:
      /// the word on its pill, the pill's level and one sentence. The sandbox is
      /// the other thing that holds or quarantines a message for what it carries,
      /// and the overview did not mention it (review finding F265). Off is the
      /// default and says nothing is held; switched on with an address empty is
      /// the state that looks on and holds nothing - the server refuses to save
      /// it so, but an address emptied over COM while it was on leaves it so.
      /// Nothing is said of a value not read.
      /// </summary>
      public static (StatusLevel Level, string State, string Sentence) Sandbox(VirusPipelineConfig config)
      {
         if (config == null || !config.WasRead(nameof(config.SandboxEnabled), nameof(config.SandboxSubmitUrl), nameof(config.SandboxVerdictUrl)))
            return (StatusLevel.Normal, L("Not read"), L("These settings could not be read."));

         if (!config.SandboxEnabled)
            return (StatusLevel.Normal, L("Off"), L("The sandbox is off, which is the default: nothing is held and nothing is submitted."));

         if (IsBlank(config.SandboxSubmitUrl) || IsBlank(config.SandboxVerdictUrl))
            return (StatusLevel.Warning, L("On, but cannot run"), L("The sandbox is switched on, but its submit or verdict address is empty."));

         // What the verdicts do turns on two more settings; not read, the sentence
         // is the one that does not say.
         if (!config.WasRead(nameof(config.SandboxTimeoutReturns), nameof(config.QuarantineEnabled)))
            return (StatusLevel.Good, L("On"), L("Mail from outside with an attachment of a submitted type waits for the sandbox's verdict before it is delivered."));

         // A whole sentence for each pair: a malicious verdict quarantines the
         // message, or returns it while quarantining is off; no verdict in time
         // delivers it with a header, or returns it (SandboxTimeoutAction).
         string sentence = (config.QuarantineEnabled, config.SandboxTimeoutReturns) switch
         {
            (true, false) => L("Mail from outside with an attachment of a submitted type waits for the sandbox: a clean verdict delivers it, a malicious one quarantines it, and no verdict in time delivers it with a header saying so."),
            (true, true) => L("Mail from outside with an attachment of a submitted type waits for the sandbox: a clean verdict delivers it, a malicious one quarantines it, and no verdict in time returns it to its sender."),
            (false, false) => L("Mail from outside with an attachment of a submitted type waits for the sandbox: a clean verdict delivers it, a malicious one returns it to its sender while quarantining is off, and no verdict in time delivers it with a header saying so."),
            (false, true) => L("Mail from outside with an attachment of a submitted type waits for the sandbox: a clean verdict delivers it, a malicious one returns it to its sender while quarantining is off, and no verdict in time returns it too.")
         };
         return (StatusLevel.Good, L("On"), sentence);
      }

      /// <summary>
      /// What the server does about an infected message, in the words of the two
      /// settings that decide it.
      /// </summary>
      public static string ActionSummary(VirusPipelineConfig config)
      {
         if (config == null)
            return "";

         if (!config.WasRead(nameof(config.Action), nameof(config.NotifySender), nameof(config.NotifyRecipient)))
            return L("What the server does with an infected message could not be read.");

         if (config.Action == VirusAction.StripAttachments)
         {
            // Deliberately says what does NOT happen: this action is the one people
            // choose expecting the notifications below it to apply, and they do not.
            return L("The attachments are replaced and the message is delivered. Neither notification setting applies to this action - the server only sends the \"message deleted\" notice when the action is to delete the message.");
         }

         if (!config.NotifySender && !config.NotifyRecipient)
            return L("The message is deleted and nobody is told. The deletion is recorded in the application log.");

         if (config.NotifyRecipient && config.NotifySender)
            return L("The message is deleted and a notice is sent to every recipient and to the envelope sender.");

         return config.NotifyRecipient
            ? L("The message is deleted and a notice is sent to every recipient.")
            : L("The message is deleted and a notice is sent to the envelope sender.");
      }

      /// <summary>
      /// What becomes of a message when no scanner can answer - delivered as though
      /// clean, the shipped default, or held and returned (AVFailAction 1) - and why
      /// the page reports whether a scanner can run.
      /// </summary>
      public static string WhenNoScannerAnswers(VirusPipelineConfig config)
         => config != null && !config.WasRead(nameof(config.HoldUnscanned))
            ? L("Whether a message no scanner could examine is held or delivered could not be read.")
            : config != null && config.HoldUnscanned
            ? L("A scanner that cannot run does not refuse the message. The error is written to the log and the scan continues to the next scanner; when none of them can answer, the message is held and scanned again, and returned to its sender when the holds run out (If a message cannot be scanned, on the Anti-virus settings page). That is why this page reports whether each scanner can run rather than whether it is switched on.")
            : L("A scanner that cannot run does not refuse the message. The error is written to the log and the scan continues to the next scanner; when none of them can answer, the message is treated exactly as if it had been examined and found clean. That is why this page reports whether each scanner can run rather than whether it is switched on.");

      /// <summary>
      /// Everything about this configuration that an administrator would want told
      /// to them rather than left to be discovered from a log.
      /// </summary>
      public static IReadOnlyList<VirusPipelineNote> Notes(VirusPipelineConfig config)
      {
         var notes = new List<VirusPipelineNote>();

         if (config == null)
            return notes;

         IReadOnlyList<VirusScannerEntry> scanners = Scanners(config);
         int usable = CountUsable(config);

         // Nothing that follows from a value that could not be read is said: a
         // scanner the server did not answer for counts as neither usable nor
         // not, and a switch read as off is not off.
         bool scannersRead = ScannersRead(config);
         bool blocking = config.WasRead(nameof(config.AttachmentBlockingEnabled), nameof(config.BlockedAttachmentPatterns));
         bool action = config.WasRead(nameof(config.Action), nameof(config.NotifySender), nameof(config.NotifyRecipient));

         // ---- cannot work ------------------------------------------------------

         foreach (VirusScannerEntry scanner in scanners.Where(s => s.Enabled && !s.Usable))
         {
            notes.Add(new VirusPipelineNote(StatusLevel.Critical, config.HoldUnscanned
               ? F("{0} is switched on but cannot run. {1} Every scan it is asked for fails, and while no other scanner answers, each message is held and scanned again, and returned to its sender when the holds run out.", scanner.Name, scanner.Problem)
               : F("{0} is switched on but cannot run. {1} Every scan it is asked for fails, and a failed scan is not a failed message: the error is logged and the message is delivered as though it had been examined and found clean.", scanner.Name, scanner.Problem)));
         }

         // Blocking switched on with no pattern strips nothing, as the verdict says
         // ("no attachment is blocked") and the Warning below: with no scanner
         // that can run, nothing examines incoming mail, where the note said that
         // attachment blocking still strips the patterns on its page.
         if (scannersRead && blocking && usable == 0 && (!config.AttachmentBlockingEnabled || config.BlockedAttachmentPatterns <= 0))
         {
            notes.Add(new VirusPipelineNote(StatusLevel.Critical, config.AttachmentBlockingEnabled
               ? L("Nothing examines incoming mail. No scanner can run and attachment blocking has no patterns, so an infected message is delivered to the mailbox exactly as it arrived.")
               : L("Nothing examines incoming mail. No scanner can run and attachment blocking is off, so an infected message is delivered to the mailbox exactly as it arrived.")));
         }
         else if (scannersRead && blocking && usable == 0)
         {
            notes.Add(new VirusPipelineNote(StatusLevel.Critical,
               L("No virus scanner can run. Attachment blocking still strips the file-name patterns on its own page, but nothing looks inside a message or inside an archive.")));
         }

         // ---- works, but leaves a gap -----------------------------------------

         if (scannersRead && usable > 0 && config.WasRead(nameof(config.MaxScanKilobytes)) && config.MaxScanKilobytes > 0)
         {
            notes.Add(new VirusPipelineNote(StatusLevel.Warning,
               F("Messages larger than {0} are not scanned. They are delivered, not refused, and nothing is written to the log when it happens - so this limit is invisible in normal operation. 0 means \"no limit\" and is the setting that closes the gap.", Kilobytes(config.MaxScanKilobytes))));
         }

         if (scannersRead && action && usable > 0 && config.Action == VirusAction.StripAttachments)
         {
            notes.Add(new VirusPipelineNote(StatusLevel.Warning,
               L("Infected messages are delivered with their attachments stripped rather than deleted. The recipient still receives the message body, which for most malicious mail is the part carrying the instructions telling them what to do next.")));
         }

         if (scannersRead && action && usable > 0 && config.Action == VirusAction.DeleteMessage && config.NotifySender)
         {
            notes.Add(new VirusPipelineNote(StatusLevel.Warning,
               L("A notice is sent to the envelope sender of every infected message. Virus senders forge that address almost without exception, so this mostly delivers mail to people who did not send anything - which is backscatter, and it is what gets a server onto a block list.")));
         }

         // The sandbox switched on with an address empty: on the page that sets it,
         // and in the words of the external setup checklist, which says the same.
         if (Sandbox(config).Level == StatusLevel.Warning)
         {
            notes.Add(new VirusPipelineNote(StatusLevel.Warning,
               L("The sandbox is on but an address is empty, so nothing is held and nothing is submitted until both are set - the application log says so at every start. Set both addresses, or turn it off.")));
         }

         if (blocking && config.AttachmentBlockingEnabled && config.BlockedAttachmentPatterns == 0)
         {
            notes.Add(new VirusPipelineNote(StatusLevel.Warning,
               L("Attachment blocking is switched on with no patterns to block, so it does nothing at all."),
               "blockedattachments"));
         }

         if (config.WasRead(nameof(config.FetchAccountsWithScanningOff)) && config.FetchAccountsWithScanningOff > 0)
         {
            // One whole text per case: the count is exact or a floor (the walk
            // stopped before the last account), and there is one account or
            // several. "At least " before a counted noun and " it is"/" them is"
            // after it were three pieces no translator could agree.
            int off = config.FetchAccountsWithScanningOff;
            string text;
            if (config.FetchAccountScanIncomplete)
            {
               text = off == 1
                  ? L("At least one external POP3 fetch account has anti-virus turned off, so mail collected by it is delivered unscanned no matter what is set here. The switch is on the fetch account, not on this page. There are more accounts on this server than this page walks, so there may be others.")
                  : Plural(off,
                     "At least {0} external POP3 fetch account has anti-virus turned off, so mail collected by it is delivered unscanned no matter what is set here. The switch is on the fetch account, not on this page. There are more accounts on this server than this page walks, so there may be others.",
                     "At least {0} external POP3 fetch accounts have anti-virus turned off, so mail collected by them is delivered unscanned no matter what is set here. The switch is on the fetch account, not on this page. There are more accounts on this server than this page walks, so there may be others.");
            }
            else
            {
               text = off == 1
                  ? L("One external POP3 fetch account has anti-virus turned off, so mail collected by it is delivered unscanned no matter what is set here. The switch is on the fetch account, not on this page.")
                  : Plural(off,
                     "{0} external POP3 fetch account has anti-virus turned off, so mail collected by it is delivered unscanned no matter what is set here. The switch is on the fetch account, not on this page.",
                     "{0} external POP3 fetch accounts have anti-virus turned off, so mail collected by them is delivered unscanned no matter what is set here. The switch is on the fetch account, not on this page.");
            }
            // The switch is on each fetch account, on the Domains page.
            notes.Add(new VirusPipelineNote(StatusLevel.Warning, text, "domains"));
         }

         // ---- worth knowing ----------------------------------------------------

         if (blocking && !config.AttachmentBlockingEnabled && config.BlockedAttachmentPatterns > 0)
         {
            notes.Add(new VirusPipelineNote(StatusLevel.Information,
               config.BlockedAttachmentPatterns == 1
                  ? L("One blocked-attachment pattern is configured but attachment blocking is switched off, so it is not applied.")
                  : Plural(config.BlockedAttachmentPatterns,
                     "{0} blocked-attachment pattern is configured but attachment blocking is switched off, so it is not applied.",
                     "{0} blocked-attachment patterns are configured but attachment blocking is switched off, so none of them is applied.")));
         }

         // The scan runs at delivery, on the delivery thread, after the message has been
         // accepted (SMTPDeliverer::RunVirusProtection_, from PreprocessMessage_), not in
         // the task that answers the sender's "250 OK": the note said the sender waited
         // for it (release review finding CPV-3). Its cost is delivery delay.
         if (scannersRead && usable > 1)
         {
            notes.Add(new VirusPipelineNote(StatusLevel.Information,
               Plural(usable,
                  "Every message goes through {0} scanner in turn, and each is asked about the whole message and then about each attachment separately. That is thorough and it is not free: the scan runs on the delivery thread after the message has been accepted, so its cost is delivery delay, not the sender's wait.",
                  "Every message goes through {0} scanners in turn, and each is asked about the whole message and then about each attachment separately. That is thorough and it is not free: the scan runs on the delivery thread after the message has been accepted, so its cost is delivery delay, not the sender's wait.")));
         }

         if (scannersRead && config.ClamWinEnabled && config.ClamAvEnabled)
         {
            notes.Add(new VirusPipelineNote(StatusLevel.Information,
               L("ClamWin and ClamAV both use the same signature database. Running both scans every message twice against the same definitions.")));
         }

         if (scannersRead && usable > 0)
         {
            notes.Add(new VirusPipelineNote(StatusLevel.Information,
               F("At most {0} scans run at once. Beyond that, delivery threads wait - and after 60 seconds of waiting the server gives up waiting and scans anyway, so a slow scanner shows up as slow mail rather than as unscanned mail.", MaxConcurrentScans)));
         }

         return notes;
      }

      /// <summary>The single worst note, for a one-line summary. Normal when there is nothing to say.</summary>
      public static StatusLevel WorstLevel(VirusPipelineConfig config)
      {
         return Notes(config).Aggregate(StatusLevel.Normal, (worst, note) => note.Level > worst ? note.Level : worst);
      }

      /// <summary>How many scanners are both switched on and able to run.</summary>
      public static int CountUsable(VirusPipelineConfig config)
         => Scanners(config).Count(scanner => scanner.Usable);

      private static bool IsBlank(string value) => string.IsNullOrWhiteSpace(value);

      /// <summary>
      /// The size limit in the units the setting is stored in, with a megabyte
      /// figure beside it once it is large enough for KB to stop being readable.
      /// </summary>
      public static string Kilobytes(int kilobytes)
      {
         if (kilobytes >= 1024)
            return F("{0} KB ({1} MB)", kilobytes.ToString("N0"), Math.Round(kilobytes / 1024.0, 1));

         return F("{0} KB", kilobytes.ToString("N0"));
      }
   }
}
