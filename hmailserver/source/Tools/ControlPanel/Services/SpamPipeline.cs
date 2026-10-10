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
   /// Which half of the SMTP conversation a check runs in. The distinction is not
   /// cosmetic: it decides what the check can look at, what the sender is told when
   /// it refuses, and whether the message size limit applies to it.
   /// </summary>
   public enum SpamCheckPhase
   {
      /// <summary>
      /// At RCPT TO, before the body has been transferred. These checks see only
      /// the connection: the connecting address, the HELO name and the envelope
      /// sender. A refusal here is a 550 and the body is never sent.
      /// </summary>
      BeforeTheBody,

      /// <summary>
      /// After the body has been received. These checks read the message, so they
      /// are the ones the "max message size to spam-scan" limit skips. A refusal
      /// here is a 554 - the sender has already paid for the transfer.
      /// </summary>
      AfterTheBody
   }

   /// <summary>One spam check, as the server will actually run it.</summary>
   public sealed class SpamCheck
   {
      internal SpamCheck(string key, int order, string name, SpamCheckPhase phase, bool enabled,
         int? score, string scoreText, string detail, string page, bool unread = false)
      {
         Key = key;
         Order = order;
         Name = name;
         Phase = phase;
         Unread = unread;
         // A check whose settings could not be read is neither on nor off, and
         // has no score to state: the row says "Not read", and nothing that
         // follows from the check is said.
         Enabled = !unread && enabled;
         Score = unread ? null : score;
         ScoreText = unread ? "…" : scoreText; // no-loc: an ellipsis where the score would be
         Detail = detail;
         Page = page;
      }

      /// <summary>
      /// Stable identifier, so a test can name a check without depending on its
      /// wording and the view can key on it without matching a display string.
      /// </summary>
      public string Key { get; }

      /// <summary>
      /// Position in the server's own list, 1-based. SpamTestRunner::LoadSpamTests
      /// pushes the tests in a fixed order and RunSpamTest walks that order, so
      /// this is the order they run in - which matters because the run stops early
      /// (see <see cref="SpamPipeline.StopScore"/>).
      /// </summary>
      public int Order { get; }

      /// <summary>What the check is called in the interface, not the C++ class name.</summary>
      public string Name { get; }

      public SpamCheckPhase Phase { get; }

      /// <summary>
      /// Whether the server will run it. This is the check's own GetIsEnabled, not
      /// a guess: for DNSBL and SURBL there is no on/off switch at all and the
      /// answer is "at least one list entry is active".
      /// </summary>
      public bool Enabled { get; }

      /// <summary>
      /// What a failure adds to the score, or null when the number is not a single
      /// value we can state - a per-entry list score, or SpamAssassin's own score
      /// when merging is on.
      /// </summary>
      public int? Score { get; }

      /// <summary>The score as it should be shown, including the null cases.</summary>
      public string ScoreText { get; }

      /// <summary>
      /// Whether the check's settings could not be read, so that neither
      /// <see cref="Enabled"/> nor <see cref="Score"/> is a fact about the server.
      /// </summary>
      public bool Unread { get; }

      /// <summary>One line: what the check looks at, and anything surprising about it.</summary>
      public string Detail { get; }

      /// <summary>Navigation key of the page that owns the settings behind it.</summary>
      public string Page { get; }
   }

   /// <summary>
   /// Something worth saying about the configuration as a whole, carrying a
   /// <see cref="StatusLevel"/> so the page can present it with the shared colour,
   /// shape and severity word rather than inventing a third vocabulary.
   /// </summary>
   public sealed class SpamPipelineNote
   {
      internal SpamPipelineNote(StatusLevel level, string text, string page = null)
      {
         Level = level;
         Text = text;
         Page = page ?? SpamPipeline.NotesPage;
      }

      public StatusLevel Level { get; }

      /// <summary>The whole note, in one sentence or two. Never empty.</summary>
      public string Text { get; }

      /// <summary>
      /// The navigation key of the page that puts the note right, which a note at
      /// the Warning or Critical level opens from the overview: Anti-spam
      /// settings, which owns the thresholds and every scored check.
      /// </summary>
      public string Page { get; }
   }

   /// <summary>
   /// A snapshot of the anti-spam configuration, read from the COM settings tree.
   ///
   /// Plain properties with no behaviour so that <see cref="SpamPipeline"/> can be
   /// tested without a server: every interesting case here - a threshold of zero,
   /// a delete threshold below the mark threshold, a check that scores nothing - is
   /// a state a live server can be in and a state that is tedious and slow to
   /// produce against one.
   ///
   /// Property names match the COM paths the settings pages use
   /// ("AntiSpam.SpamMarkThreshold" -> MarkThreshold is the one exception, because
   /// "SpamMarkThreshold" on a class already called spam configuration stutters).
   /// </summary>
   public sealed class SpamPipelineConfig
   {
      // Thresholds. Both are compared with > 0 before they are used at all, which
      // is the single most surprising thing in this whole area.
      public int MarkThreshold { get; set; }
      public int DeleteThreshold { get; set; }

      /// <summary>AntiSpam.MaximumMessageSize, in KB. 0 = no limit of our own.</summary>
      public int MaxScanKilobytes { get; set; }

      /// <summary>How many blocked-sender entries exist. The blacklist has no
      /// on/off switch - an empty list IS the off state - so the count is the
      /// enablement.</summary>
      public int BlockedSenderCount { get; set; }

      // What "marked" actually does to the message.
      public bool AddSpamHeader { get; set; }
      public bool AddReasonHeader { get; set; }
      public bool PrependSubject { get; set; }

      // The connection checks.
      public bool CheckHeloHost { get; set; }
      public int HeloHostScore { get; set; }
      public bool CheckPtr { get; set; }
      public int PtrScore { get; set; }
      public bool CheckSenderMx { get; set; }
      public int SenderMxScore { get; set; }
      public bool CheckSpf { get; set; }
      public int SpfScore { get; set; }

      // The message checks.
      public bool VerifyDkim { get; set; }
      public int DkimFailureScore { get; set; }
      public bool EvaluateDmarc { get; set; }
      public int DmarcFailureScore { get; set; }

      public bool SpamAssassinEnabled { get; set; }
      public bool SpamAssassinMergesScore { get; set; }
      public int SpamAssassinScore { get; set; }
      public string SpamAssassinHost { get; set; }

      /// <summary>
      /// The external filtering engine (FilterHookUrl, empty for none) - rspamd or
      /// anything else that answers the hook's HTTP request - and the score an
      /// answer of reject adds (FilterHookRejectScore). SpamTestRunner runs it
      /// last, after SpamAssassin, and it adds the score the engine answers.
      /// </summary>
      public string FilterHookUrl { get; set; } = "";
      public int FilterHookRejectScore { get; set; }

      // The two list-driven checks have no switch of their own.
      public int ActiveDnsBlackLists { get; set; }
      public int TotalDnsBlackLists { get; set; }
      public int ActiveSurblServers { get; set; }
      public int TotalSurblServers { get; set; }

      // Greylisting is not a scoring check; it defers instead. Kept here because
      // the overview has to describe it, and because its two bypasses are the
      // reason an administrator reports that "greylisting is not working".
      public bool GreylistingEnabled { get; set; }
      public bool BypassGreylistingOnSpfPass { get; set; }
      public bool BypassGreylistingOnSenderMx { get; set; }
      public int GreylistWhiteListEntries { get; set; }

      /// <summary>
      /// Greylisting bypass entries: domains let past greylisting on the
      /// authentication results their mail carries. 0 on a server older than
      /// the list.
      /// </summary>
      public int GreylistBypassEntries { get; set; }

      /// <summary>Anti-spam white list entries: senders that skip every check above.</summary>
      public int WhiteListEntries { get; set; }

      /// <summary>
      /// Entries on the server's allow and block list. Not a check the score
      /// runs - the list is read at delivery, per recipient, after every check
      /// here has finished - so it is counted with the lists rather than drawn as
      /// a row of the pipeline, where it would claim a place in an order it does
      /// not have.
      /// </summary>
      public int SenderListEntries { get; set; }

      /// <summary>
      /// Rows on the domain white and black list, white and black together. Not a
      /// check either: a black row refuses at MAIL FROM before any check runs, and
      /// a white row is what lets a sender past greylisting and past the score's
      /// refusal while every check still runs - so it is counted with the lists
      /// and named among the greylisting bypasses.
      /// </summary>
      public int DomainPolicyListEntries { get; set; }

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
   /// What the spam configuration will do to a message, derived from the settings
   /// rather than described by hand.
   ///
   /// WHY THIS EXISTS. Anti-spam was five pages - thresholds and checks, SURBL
   /// servers, DNS blacklists, a white list and a greylisting white list - and none
   /// of them could answer the question an administrator arrives with, because the
   /// answer is not on any one page. It needs the order the checks run in, what each
   /// one contributes, and the two thresholds that turn a total into a verdict.
   ///
   /// Everything asserted here was read out of the server, and three of the findings
   /// are the reason the page is worth building rather than a nicety:
   ///
   ///   - SMTPConnection::DoSpamProtection_ guards both thresholds with "> 0". A
   ///     mark threshold of zero does not mean "mark everything", it means "mark
   ///     nothing" - and nothing in the interface said so.
   ///   - SpamTestRunner::RunSpamTest breaks out of the loop as soon as the running
   ///     total reaches max(delete, mark). Checks later in the fixed order simply do
   ///     not run, so the order is load-bearing; and with both thresholds at zero
   ///     that condition is true immediately, so exactly one check runs per phase.
   ///   - the delete threshold is tested before the mark threshold, so a delete
   ///     threshold at or below the mark threshold means nothing is ever marked and
   ///     delivered - it is refused instead.
   ///
   /// No WPF, for the same reason as <see cref="NavigationMap"/> and
   /// <see cref="AccessibleNames"/>: the statements about the server are the part
   /// worth testing, and they must be testable without a live server and without a
   /// dispatcher.
   /// </summary>
   public static class SpamPipeline
   {
      /// <summary>
      /// The hard ceiling in SpamProtection::RunPostTransmissionTests, in KB: above
      /// it the body checks are skipped whatever the configured limit is, because
      /// MessageData::LoadFromMessage will not load the message.
      /// </summary>
      public const int ParserSizeLimitKilobytes = 1024 * 80;

      /// <summary>
      /// Pages this overview links to. Every spam page in the navigation must be
      /// reachable from here - an overview that leaves one out sends the reader
      /// back to hunting for it, which is the problem it was built to remove.
      /// A test asserts this list against the navigation group, which is how the
      /// quarantine page was caught the moment it was added.
      /// </summary>
      public static IReadOnlyList<string> LinkedPages { get; } = new[]
      {
         "antispam", "blockedsenders", "dnsbl", "quarantine", "surbl", "spamwhitelist", "greylistwhitelist", "greylistbypass", "senderlist", "domainpolicylist"
      };

      /// <summary>
      /// The eleven scoring checks, in the order SpamTestRunner runs them: the
      /// external filtering engine is the last (SpamTestRunner::LoadSpamTests), and
      /// the page left it out, so that with rspamd attached the highest total it
      /// stated was false (review finding F264).
      ///
      /// Greylisting is deliberately not one of them: it defers a message instead
      /// of scoring it, and folding a deferral into a score table would be the same
      /// class of untruth this page exists to expose. See
      /// <see cref="GreylistingSummary"/>.
      /// </summary>
      public static IReadOnlyList<SpamCheck> Checks(SpamPipelineConfig config)
      {
         if (config == null)
            return Array.Empty<SpamCheck>();

         // A check whose settings could not be read is "Not read", not off: the
         // snapshot holds the type's default for a value the server did not
         // answer, and a default drawn as a setting is the lie this page exists
         // to expose.
         bool Unread(params string[] properties) => !config.WasRead(properties);

         var checks = new List<SpamCheck>
         {
            // 1-6: SpamTest::PreTransmission. Run at MAIL FROM by default
            // (DNSBLChecksAfterMailFrom=1), at the first RCPT when that is 0.
            //
            // The blacklist is registered first in SpamTestRunner::LoadSpamTests
            // on purpose: it is the cheapest test, and with the default score a
            // match ends the run before any DNS lookup happens.
            new SpamCheck("blockedsenders", 1, L("Blocked senders"), SpamCheckPhase.BeforeTheBody,
               config.BlockedSenderCount > 0, null,
               config.BlockedSenderCount > 0 ? L("the entry's score") : L("adds nothing"),
               config.BlockedSenderCount > 0
                  ? Plural(config.BlockedSenderCount,
                     "Matches the envelope sender against {0} blocked address or domain; the score comes from the entry that matched.",
                     "Matches the envelope sender against {0} blocked addresses and domains; the score comes from the entry that matched.")
                  : L("No blocked senders are defined, so nothing is matched. The list has no on/off switch - an empty list is the off state."),
               "blockedsenders", Unread(nameof(config.BlockedSenderCount))),

            new SpamCheck("dnsbl", 2, L("DNS block lists (DNSBL)"), SpamCheckPhase.BeforeTheBody,
               config.ActiveDnsBlackLists > 0, null,
               config.ActiveDnsBlackLists > 0 ? L("the entry's score") : L("adds nothing"),
               DescribeList(L("Looks up the connecting address on each active block list; the score comes from the entry that matched"),
                  config.ActiveDnsBlackLists, config.TotalDnsBlackLists),
               "dnsbl", Unread(nameof(config.ActiveDnsBlackLists), nameof(config.TotalDnsBlackLists))),

            new SpamCheck("helo", 3, L("HELO host check"), SpamCheckPhase.BeforeTheBody,
               config.CheckHeloHost, config.HeloHostScore, Score(config.HeloHostScore),
               L("Fails when the name the client gave in HELO/EHLO does not resolve to the address it is connecting from."),
               "antispam", Unread(nameof(config.CheckHeloHost), nameof(config.HeloHostScore))),

            new SpamCheck("ptr", 4, L("PTR record check"), SpamCheckPhase.BeforeTheBody,
               config.CheckPtr, config.PtrScore, Score(config.PtrScore),
               L("Fails when the connecting address has no reverse DNS record."),
               "antispam", Unread(nameof(config.CheckPtr), nameof(config.PtrScore))),

            new SpamCheck("mx", 5, L("Sender MX check"), SpamCheckPhase.BeforeTheBody,
               config.CheckSenderMx, config.SenderMxScore, Score(config.SenderMxScore),
               L("Fails when the envelope sender's domain publishes no MX record."),
               "antispam", Unread(nameof(config.CheckSenderMx), nameof(config.SenderMxScore))),

            new SpamCheck("spf", 6, L("SPF"), SpamCheckPhase.BeforeTheBody,
               config.CheckSpf, config.SpfScore, Score(config.SpfScore),
               L("Checks the sender's SPF policy against the connecting address. A pass can also bypass greylisting."),
               "antispam", Unread(nameof(config.CheckSpf), nameof(config.SpfScore))),

            // 6-9: SpamTest::PostTransmission, run once the body has arrived.
            new SpamCheck("surbl", 7, L("SURBL servers"), SpamCheckPhase.AfterTheBody,
               config.ActiveSurblServers > 0, null,
               config.ActiveSurblServers > 0 ? L("the entry's score") : L("adds nothing"),
               DescribeList(L("Looks up the domains of links found in the body on each active SURBL server"),
                  config.ActiveSurblServers, config.TotalSurblServers),
               "surbl", Unread(nameof(config.ActiveSurblServers), nameof(config.TotalSurblServers))),

            new SpamCheck("dkim", 8, L("DKIM verification"), SpamCheckPhase.AfterTheBody,
               config.VerifyDkim, config.DkimFailureScore, Score(config.DkimFailureScore),
               L("Scores only a permanent failure - a signature that is present and wrong. An unsigned message scores nothing."),
               "antispam", Unread(nameof(config.VerifyDkim), nameof(config.DkimFailureScore))),

            new SpamCheck("dmarc", 9, L("DMARC"), SpamCheckPhase.AfterTheBody,
               config.EvaluateDmarc, config.DmarcFailureScore, Score(config.DmarcFailureScore),
               L("Evaluates the From: domain's DMARC policy, using its own SPF and DKIM results."),
               "antispam", Unread(nameof(config.EvaluateDmarc), nameof(config.DmarcFailureScore))),

            new SpamCheck("spamassassin", 10, "SpamAssassin", SpamCheckPhase.AfterTheBody,
               config.SpamAssassinEnabled,
               config.SpamAssassinMergesScore ? (int?) null : config.SpamAssassinScore,
               config.SpamAssassinMergesScore ? L("SpamAssassin's own score") : Score(config.SpamAssassinScore),
               F("Scores only when SpamAssassin itself tags the message (X-Spam-Status: Yes). {0}", (config.SpamAssassinMergesScore ? L("Merging is on, so its own numeric score is added - which can be any size.") : L("Merging is off, so the fixed score is used however sure SpamAssassin was."))),
               "antispam", Unread(nameof(config.SpamAssassinEnabled), nameof(config.SpamAssassinMergesScore), nameof(config.SpamAssassinScore))),

            // 11: SpamTestFilterHook, after SpamAssassin. It adds the engine's own
            // score, rounded, or FilterHookRejectScore for an answer of reject -
            // and, with FilterHookFailClosed on, for no answer - so its score can
            // be any size, as a merging SpamAssassin's can.
            new SpamCheck("filterhook", 11, L("External filtering engine"), SpamCheckPhase.AfterTheBody,
               !string.IsNullOrWhiteSpace(config.FilterHookUrl), null,
               string.IsNullOrWhiteSpace(config.FilterHookUrl) ? L("adds nothing") : L("the engine's score"),
               string.IsNullOrWhiteSpace(config.FilterHookUrl)
                  ? L("No engine is configured, so no message is sent to one.")
                  : F("Sends the message to the engine at {0} and adds the score it answers; an answer of reject adds {1}.", config.FilterHookUrl, config.FilterHookRejectScore),
               "antispam", Unread(nameof(config.FilterHookUrl), nameof(config.FilterHookRejectScore)))
         };

         return checks;
      }

      /// <summary>The page a note is about unless it names another.</summary>
      public const string NotesPage = "antispam";

      /// <summary>
      /// The level the verdict is shown at, from what it says: Critical when no
      /// score can mark or refuse anything, or when the delete threshold, tested
      /// first, leaves nothing to mark; a Warning when nothing is ever marked;
      /// Good otherwise; Information when the thresholds could not be read. It was
      /// plain body text with no level.
      /// </summary>
      public static StatusLevel VerdictLevel(SpamPipelineConfig config)
      {
         if (!ThresholdsRead(config))
            return StatusLevel.Information;

         bool marks = config.MarkThreshold > 0;
         bool refuses = config.DeleteThreshold > 0;

         if (!marks && !refuses)
            return StatusLevel.Critical;

         if (marks && refuses && config.DeleteThreshold <= config.MarkThreshold)
            return StatusLevel.Critical;

         return marks ? StatusLevel.Good : StatusLevel.Warning;
      }

      /// <summary>
      /// Whether the notes say what the verdict says: at the Warning level and
      /// above - nothing ever marked, nothing done on score at all, a delete
      /// threshold that leaves nothing to mark - a note at the verdict's level
      /// says it, with the page that puts it right. The overview then draws the
      /// verdict as the card's lead sentence, at no level and with no action, and
      /// those notes are the only notices at the Warning level and above, as the
      /// count beside the title counts them: as a notice of its own the verdict
      /// stood above the note that said it, at its level and with the same Open
      /// Anti-spam settings, one notice more than the count. A verdict that is
      /// Good, or that cannot be said, has no note to say it and is the notice.
      /// </summary>
      public static bool NotesSayTheVerdict(SpamPipelineConfig config)
         => VerdictLevel(config) >= StatusLevel.Warning;

      /// <summary>
      /// The state a check's row shows, as a level and a word: Not read and Off
      /// say nothing; a check that is on but can never change the outcome - it
      /// scores 0, or it is SpamAssassin with no host to ask - is a Warning, On,
      /// scores 0 or On, no host, where it showed the same green On as one that
      /// works.
      /// </summary>
      public static (StatusLevel Level, string Text) StateOf(SpamPipelineConfig config, SpamCheck check)
      {
         if (check.Unread)
            return (StatusLevel.Normal, L("Not read"));

         if (!check.Enabled)
            return (StatusLevel.Normal, L("Off"));

         if (check.Key == "spamassassin" && config != null)
         {
            if (config.WasRead(nameof(config.SpamAssassinHost)) && string.IsNullOrWhiteSpace(config.SpamAssassinHost))
               return (StatusLevel.Warning, L("On, no host"));

            if (!config.SpamAssassinMergesScore && config.SpamAssassinScore <= 0)
               return (StatusLevel.Warning, L("On, scores 0"));

            return (StatusLevel.Good, L("On"));
         }

         return check.Score == 0 ? (StatusLevel.Warning, L("On, scores 0")) : (StatusLevel.Good, L("On"));
      }

      /// <summary>
      /// What the Anti-spam settings page refuses on the refuse threshold's field as
      /// the two thresholds are typed in, or null: a refuse threshold at or below a
      /// mark threshold above 0, which is tested first and so refuses every message the
      /// mark would only mark (review finding F263). Either at 0 is a warning, not a
      /// refusal: 0 means never, and is a choice (<see cref="ThresholdWarning"/>).
      /// </summary>
      public static string ThresholdProblem(int mark, int refuse)
         => mark > 0 && refuse > 0 && refuse <= mark
            ? F("The refuse threshold must be above the mark threshold, {0}: at or below it, every message the mark threshold would mark is refused instead.", mark)
            : null;

      /// <summary>What a threshold at 0 means, said under the two on the settings page; null while both are above 0.</summary>
      public static string ThresholdWarning(int mark, int refuse)
         => mark <= 0 && refuse <= 0
            ? L("Both thresholds are 0, so no score marks, refuses or holds any message.")
            : mark <= 0
            ? L("The mark threshold is 0, so no message is marked as spam: 0 means never.")
            : refuse <= 0
            ? L("The refuse threshold is 0, so no message is refused or held on its score: 0 means never.")
            : null;

      /// <summary>
      /// What the total score does to the message, in one sentence, including the
      /// two cases where the answer is "nothing".
      /// </summary>
      public static string Verdict(SpamPipelineConfig config)
      {
         if (config == null)
            return "";

         // A threshold that could not be read is 0 in the snapshot, and 0 is the
         // one value that says "nothing happens": the verdict is not given.
         if (!ThresholdsRead(config))
            return L("What happens to a message cannot be said: the thresholds could not be read.");

         bool marks = config.MarkThreshold > 0;
         bool refuses = config.DeleteThreshold > 0;

         // The server tests the delete threshold first (SMTPConnection), so with
         // it at or below the mark threshold every message that reaches the mark
         // is refused and none is only marked - "10000 or more is marked and
         // delivered; 10000 or more is refused" said both of one message.
         if (marks && refuses && config.DeleteThreshold <= config.MarkThreshold)
         {
            return F("A message scoring {0} or more is refused (550 before the body, 554 after it). None is only marked: the mark threshold, {1}, is not below that.", config.DeleteThreshold, config.MarkThreshold);
         }

         if (marks && refuses)
         {
            return F("A message scoring {0} or more is marked as spam and delivered; {1} or more is refused (550 before the body, 554 after it).", config.MarkThreshold, config.DeleteThreshold);
         }

         if (marks)
            return F("A message scoring {0} or more is marked as spam and delivered. Nothing is refused on score.", config.MarkThreshold);

         if (refuses)
            return F("A message scoring {0} or more is refused (550 before the body, 554 after it). Nothing is marked.", config.DeleteThreshold);

         return L("Nothing at all happens on score: both thresholds are 0, and the server only marks or refuses when the threshold is above 0.");
      }

      /// <summary>
      /// The score at which the server stops testing - max(delete, mark), copied
      /// from SpamProtection::RunPreTransmissionTests, which passes it to
      /// RunSpamTest as the maximum. Counted per phase: the total resets between
      /// the connection checks and the message checks, while the verdict is taken
      /// on the sum of both.
      /// </summary>
      public static int StopScore(SpamPipelineConfig config)
         => config == null ? 0 : Math.Max(config.DeleteThreshold, config.MarkThreshold);

      /// <summary>
      /// The most the enabled checks could add between them, or null when that
      /// cannot be stated: an active DNSBL or SURBL carries its score per list
      /// entry, and a merging SpamAssassin contributes whatever score it produced.
      /// Null means "unknown", never "zero" - claiming a ceiling we cannot compute
      /// is how a page starts lying.
      /// </summary>
      public static int? HighestReachableScore(SpamPipelineConfig config)
      {
         if (config == null)
            return null;

         // A check that could not be read may be on with any score: the ceiling
         // is then unknown, as it is with a per-entry score.
         if (Checks(config).Any(c => c.Unread))
            return null;

         int total = 0;

         foreach (SpamCheck check in Checks(config).Where(c => c.Enabled))
         {
            if (check.Score == null)
               return null;

            total += check.Score.Value;
         }

         return total;
      }

      /// <summary>
      /// What the verdict says of the highest total the enabled checks can reach,
      /// after the stop score: the number; that the total is open-ended, which is a
      /// statement about the configuration; or that it cannot be said, which is a
      /// statement about the read. The two are not the same sentence: a check the
      /// server did not answer leaves <see cref="HighestReachableScore"/> null as
      /// a per-entry score does, and "open-ended" of it would describe a
      /// configuration the page never saw.
      /// </summary>
      public static string CeilingSentence(SpamPipelineConfig config)
      {
         if (config == null)
            return "";

         if (Checks(config).Any(c => c.Unread))
            return L("The highest total reachable cannot be said: a check could not be read.");

         int? ceiling = HighestReachableScore(config);

         return ceiling != null
            ? F("With the checks currently enabled the highest total reachable is {0}.", ceiling.Value)
            : L("The total is open-ended, because a list entry, SpamAssassin's own score or the external engine's can be any size.");
      }

      /// <summary>
      /// Greylisting, described rather than scored. Deliberately says out loud that
      /// the two bypasses exist, because "greylisting is enabled and mail from that
      /// host is not being delayed" is nearly always one of them.
      /// </summary>
      public static string GreylistingSummary(SpamPipelineConfig config)
      {
         if (config == null)
            return "";

         if (!config.WasRead(nameof(config.GreylistingEnabled), nameof(config.BypassGreylistingOnSpfPass),
                nameof(config.BypassGreylistingOnSenderMx), nameof(config.GreylistWhiteListEntries),
                nameof(config.GreylistBypassEntries), nameof(config.DomainPolicyListEntries)))
            return L("The greylisting settings could not be read.");

         if (!config.GreylistingEnabled)
            return L("Off. No message is deferred on a first attempt.");

         var bypasses = new List<string>();
         if (config.BypassGreylistingOnSpfPass)
            bypasses.Add(L("the sender's SPF check passed"));
         if (config.BypassGreylistingOnSenderMx)
            bypasses.Add(L("the connecting address is one of the sender domain's A or MX records"));
         if (config.GreylistWhiteListEntries > 0)
            bypasses.Add(F("the connecting address matches one of the greylisting exemptions (entries: {0})", config.GreylistWhiteListEntries));
         if (config.GreylistBypassEntries > 0)
            bypasses.Add(F("the sender's domain or HELO host is one of the {0} entries on the greylisting bypass list and the message carries the SPF, DKIM or DMARC pass that entry requires", config.GreylistBypassEntries));
         if (config.DomainPolicyListEntries > 0)
            bypasses.Add(L("the sender's domain or HELO name is an allowed domain"));

         string text = L("On. A sender/recipient/address triplet that has not been seen before is answered 451 \"Please try again later.\" at RCPT TO, and accepted when it retries. It runs after the connection checks above and adds no score.");

         // A per-domain switch as well, which is on the domain and not on any of
         // the anti-spam pages - so an administrator whose greylisting "is not
         // working" for one domain has nowhere else to be told this.
         return Sentences(text,
            bypasses.Count > 0 ? F("Skipped when {0}.", Join(bypasses)) : null,
            L("Also skipped for any recipient domain that has greylisting turned off in its own settings."));
      }

      /// <summary>
      /// Everything worth saying about the configuration as a whole: first the
      /// arrangements that cannot work, then the ones that waste a lookup, then the
      /// facts an administrator cannot see anywhere in the interface.
      ///
      /// Ordered by severity so that the page can render them in order and a reader
      /// who stops after the first one has read the worst one.
      /// </summary>
      public static IReadOnlyList<SpamPipelineNote> Notes(SpamPipelineConfig config)
      {
         var notes = new List<SpamPipelineNote>();
         if (config == null)
            return notes;

         // Nothing that follows from a value that could not be read is said: a
         // threshold the server did not answer is 0 here, and 0 would make the
         // worst note on the page.
         bool thresholds = ThresholdsRead(config);
         bool marks = thresholds && config.MarkThreshold > 0;
         bool refuses = thresholds && config.DeleteThreshold > 0;

         // ---- cannot work -----------------------------------------------------

         if (thresholds && !marks && !refuses)
         {
            notes.Add(new SpamPipelineNote(StatusLevel.Critical,
               L("Both thresholds are 0, so no score can mark or refuse anything - the server tests them with \"greater than 0\" before using them. It also stops testing as soon as the running total reaches the higher threshold, which is 0, so exactly one check runs in each phase and its result is discarded.")));
         }
         else if (thresholds && !marks)
         {
            notes.Add(new SpamPipelineNote(StatusLevel.Warning,
               F("Nothing is ever marked as spam: marking needs a mark threshold above 0, and a 0 there means \"never\" rather than \"always\". Messages reaching {0} are still refused.", config.DeleteThreshold)));
         }

         if (marks && refuses && config.DeleteThreshold <= config.MarkThreshold)
         {
            notes.Add(new SpamPipelineNote(StatusLevel.Critical,
               F("The refuse threshold ({0}) is at or below the mark threshold ({1}) and is tested first, so no message is ever marked and delivered - every message that would be marked is refused instead.", config.DeleteThreshold, config.MarkThreshold)));
         }

         int? ceiling = HighestReachableScore(config);
         if (marks && ceiling != null && ceiling.Value < config.MarkThreshold)
         {
            notes.Add(new SpamPipelineNote(StatusLevel.Critical,
               F("The enabled checks can add at most {0} between them, which is below the mark threshold of {1}: no message can reach either threshold, however bad it is.", ceiling.Value, config.MarkThreshold)));
         }

         // ---- runs and cannot change the outcome ------------------------------

         // SpamAssassin is excluded here because it gets a note of its own below:
         // the reason its score is zero is different (merging is off and the fixed
         // score was never set), and so is the fix.
         foreach (SpamCheck check in Checks(config)
            .Where(c => c.Enabled && c.Score == 0 && c.Key != "spamassassin"))
         {
            notes.Add(new SpamPipelineNote(StatusLevel.Warning,
               F("{0} is on but scores 0, so it costs a lookup on every message and can never change the outcome. Give it a score, or turn it off.", check.Name)));
         }

         bool spamAssassin = config.WasRead(nameof(config.SpamAssassinEnabled), nameof(config.SpamAssassinMergesScore), nameof(config.SpamAssassinScore), nameof(config.SpamAssassinHost));

         if (spamAssassin && config.SpamAssassinEnabled && !config.SpamAssassinMergesScore && config.SpamAssassinScore <= 0)
         {
            notes.Add(new SpamPipelineNote(StatusLevel.Warning,
               F("SpamAssassin's verdict is thrown away: with merging off, a message it tags as spam is given the fixed score, which is {0}.", config.SpamAssassinScore)));
         }

         if (spamAssassin && config.SpamAssassinEnabled && string.IsNullOrWhiteSpace(config.SpamAssassinHost))
         {
            notes.Add(new SpamPipelineNote(StatusLevel.Warning,
               L("SpamAssassin is on with no host set, so every message is accepted without a verdict and raises server error 5508.")));
         }

         if (marks && config.WasRead(nameof(config.AddSpamHeader), nameof(config.AddReasonHeader), nameof(config.PrependSubject))
             && !config.AddSpamHeader && !config.AddReasonHeader && !config.PrependSubject)
         {
            notes.Add(new SpamPipelineNote(StatusLevel.Warning,
               L("A marked message is changed in no visible way: no X-hMailServer-Spam header, no reason header and no subject prefix, so a mail client has nothing to file on. The internal spam flag is still set, which suppresses auto-replies and forwarding for accounts configured that way.")));
         }

         // ---- true, invisible, and worth knowing ------------------------------

         int stop = thresholds ? StopScore(config) : 0;
         if (stop > 0)
         {
            foreach (SpamCheckPhase phase in new[] { SpamCheckPhase.BeforeTheBody, SpamCheckPhase.AfterTheBody })
            {
               List<SpamCheck> enabled = Checks(config).Where(c => c.Enabled && c.Phase == phase).ToList();

               // Only interesting when something comes after the check that can
               // reach the stop score on its own - otherwise nothing is skipped.
               for (int i = 0; i < enabled.Count - 1; i++)
               {
                  if (enabled[i].Score == null || enabled[i].Score.Value < stop)
                     continue;

                  notes.Add(new SpamPipelineNote(StatusLevel.Information,
                     F("{0} scores {1} on its own, which reaches the {2} at which the server stops testing - so {3} will not run for a message it fails, and will not appear in the log.", enabled[i].Name, enabled[i].Score.Value, stop, Join(enabled.Skip(i + 1).Select(c => c.Name).ToList()))));
                  break;
               }
            }
         }

         if (!config.WasRead(nameof(config.MaxScanKilobytes)))
         {
            // Said neither way: 0 would be "no limit", and the value is not known.
         }
         else if (config.MaxScanKilobytes > 0)
         {
            // The external engine is a check that needs the body as well, and
            // SpamProtection::RunPostTransmissionTests skips it with the others.
            notes.Add(new SpamPipelineNote(StatusLevel.Information,
               string.IsNullOrWhiteSpace(config.FilterHookUrl)
                  ? F("Messages larger than {0} KB skip every check that needs the body (SURBL, DKIM, DMARC and SpamAssassin). The connection checks still run, and the message is delivered.", config.MaxScanKilobytes)
                  : F("Messages larger than {0} KB skip every check that needs the body (SURBL, DKIM, DMARC, SpamAssassin and the external filtering engine). The connection checks still run, and the message is delivered.", config.MaxScanKilobytes)));
         }
         else
         {
            notes.Add(new SpamPipelineNote(StatusLevel.Information,
               F("There is no scan size limit, but messages over {0} MB still skip the body checks: the MIME parser cannot load them, and scanning without loading them would write an empty body over the message.", (ParserSizeLimitKilobytes / 1024))));
         }

         if (config.WasRead(nameof(config.WhiteListEntries)))
         {
            notes.Add(new SpamPipelineNote(StatusLevel.Information,
               F("None of this applies to an authenticated session - spam protection is skipped entirely for a client that signed in, for connections from an IP range with spam protection turned off, and for the senders and IP ranges in the spam exemptions (entries: {0}).", config.WhiteListEntries)));
         }

         return notes;
      }

      /// <summary>Whether both thresholds were read, without which no verdict can be given.</summary>
      public static bool ThresholdsRead(SpamPipelineConfig config)
         => config != null && config.WasRead(nameof(config.MarkThreshold), nameof(config.DeleteThreshold));

      /// <summary>The single worst note, for a one-line summary. Normal when there is nothing to say.</summary>
      public static StatusLevel WorstLevel(SpamPipelineConfig config)
      {
         return Notes(config).Aggregate(StatusLevel.Normal, (worst, note) => note.Level > worst ? note.Level : worst);
      }

      /// <summary>
      /// What a check adds to the score, as the Adds column says it: "+2", "adds
      /// nothing" for 0, a negative score as it is. The bare number - "2", or "-"
      /// for nothing - stood unlabelled at the end of each row.
      /// </summary>
      private static string Score(int score)
         => score > 0 ? F("+{0}", score)
            : score == 0 ? L("adds nothing")
            : score.ToString(System.Globalization.CultureInfo.CurrentCulture);

      private static string DescribeList(string what, int active, int total)
      {
         if (total == 0)
            return F("{0}. No entries are configured, so the check does not run.", what);

         if (active == 0)
            return PluralF(total, "{0}. {1} entry is configured but not active, so the check does not run.",
               "{0}. {1} entries are configured, none active, so the check does not run.", what, total);

         // The list pages' words for the count, "Active: 2 of 3" (ActiveEntries).
         return F("{0}. Active: {1} of {2}.", what, active, total);
      }

      /// <summary>"a, b and c" - the notes are sentences, so they need the "and".</summary>
      private static string Join(IReadOnlyList<string> parts)
      {
         if (parts == null || parts.Count == 0)
            return "";
         if (parts.Count == 1)
            return parts[0];

         return F("{0} and {1}", string.Join(", ", parts.Take(parts.Count - 1)), parts[parts.Count - 1]);
      }
   }
}
