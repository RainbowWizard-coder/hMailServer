// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The virus scanning overview: the page that answers "will an infected message
   /// actually be caught here".
   ///
   /// The anti-virus settings page is a correct editor for each value and cannot
   /// answer that, because the answer is the interaction of four of them. Every
   /// behaviour pinned below was read out of the server rather than assumed, and
   /// every one of them is a way the configuration silently does less than it looks
   /// like it does:
   ///
   ///   - VirusScanner::ScanFile_ reports a scanner error through ErrorManager at
   ///     Medium and carries on. When every enabled scanner errors, the result is
   ///     NoVirusFound - so a scanner switched on with a missing host or executable
   ///     delivers mail exactly as though it had been examined and found clean;
   ///   - VirusScanner::Scan skips the message when `limit > 0 && messageKB > limit`,
   ///     so 0 is "no limit" and any positive number is a hole. That is the opposite
   ///     way round from every other size limit in the product;
   ///   - SMTPDeliverer::HandleInfectedMessage_ only sends the notifications on the
   ///     DELETE branch, so choosing "strip attachments" silently disables both
   ///     notification settings while they still read as on;
   ///   - SMTPDeliverer::RunVirusProtection_ blocks attachments BEFORE it checks the
   ///     virus-scan flag or whether any scanner is enabled, so attachment blocking
   ///     is independent of all of it;
   ///   - and a message only reaches the scanners at all when its virus-scan flag is
   ///     set, which for POP3-fetched mail comes from the fetch account rather than
   ///     from anything on the anti-virus page.
   /// </summary>
   public class VirusPipelineTests
   {
      /// <summary>
      /// hMailServer's own defaults, from DBScripts\CreateTables*.sql: every scanner
      /// off, ClamAV pre-filled with localhost:3310 but not enabled, avaction 0
      /// (delete), avmaxmsgsize 0, both notifications off, and - the interesting one
      /// - enableattachmentblocking 0 with roughly twenty *.bat/*.exe/... patterns
      /// seeded into hm_blocked_attachments beside it.
      /// </summary>
      private static VirusPipelineConfig Defaults()
      {
         return new VirusPipelineConfig
         {
            ClamAvEnabled = false,
            ClamAvHost = "localhost",
            ClamAvPort = 3310,
            ClamWinEnabled = false,
            CustomScannerEnabled = false,
            MaxScanKilobytes = 0,
            Action = VirusAction.DeleteMessage,
            NotifySender = false,
            NotifyRecipient = false,
            AttachmentBlockingEnabled = false,
            BlockedAttachmentPatterns = 20
         };
      }

      /// <summary>A server that is actually scanning: clamd reachable, delete on find.</summary>
      private static VirusPipelineConfig Scanning()
      {
         VirusPipelineConfig config = Defaults();
         config.ClamAvEnabled = true;
         config.AttachmentBlockingEnabled = true;
         return config;
      }

      private static bool HasNote(VirusPipelineConfig config, StatusLevel level, string fragment) =>
         VirusPipeline.Notes(config).Any(n => n.Level == level && n.Text.Contains(fragment));

      // ---- the shipped default ------------------------------------------------

      [Fact]
      public void AStockInstallationScansNothingAndSaysSo()
      {
         VirusPipelineConfig config = Defaults();

         Assert.Equal(0, VirusPipeline.CountUsable(config));

         // The single most important sentence on the page, for the configuration
         // the largest number of servers are actually in.
         Assert.Contains("No message is scanned for viruses", VirusPipeline.Verdict(config));
         Assert.Equal(StatusLevel.Critical, VirusPipeline.WorstLevel(config));
         Assert.True(HasNote(config, StatusLevel.Critical, "Nothing examines incoming mail"));
      }

      [Fact]
      public void PatternsConfiguredWithBlockingOffAreReportedAsUnapplied()
      {
         // The shipped state, and it looks like protection from the settings page:
         // twenty file-name patterns are listed on Blocked attachments and not one
         // of them is applied, because the switch that turns the feature on is
         // seeded off.
         VirusPipelineConfig config = Defaults();

         Assert.True(HasNote(config, StatusLevel.Information, "configured but attachment blocking is switched off"));
      }

      // ---- enabled but unable to run ------------------------------------------

      [Fact]
      public void AnEnabledScannerWithNoHostIsReportedAsUnableToRun()
      {
         VirusPipelineConfig config = Defaults();
         config.ClamAvEnabled = true;
         config.ClamAvHost = "";

         VirusScannerEntry clamAv = VirusPipeline.Scanners(config).Single(s => s.Key == "clamav");

         Assert.True(clamAv.Enabled);
         Assert.False(clamAv.Usable);
         Assert.Equal("On, but cannot run", clamAv.StateText);
         Assert.Equal(0, VirusPipeline.CountUsable(config));

         // The consequence, not just the fact - this is the sentence that explains
         // why a broken scanner is worse than no scanner.
         Assert.True(HasNote(config, StatusLevel.Critical, "delivered as though it had been examined and found clean"));
      }

      // AVFailAction 1 holds what no scanner could examine: the page must not say
      // such a message is delivered as though clean, which is the default's story.
      [Fact]
      public void WithUnscannedMailHeld_ABrokenScannerIsNotSaidToDeliverAsClean()
      {
         VirusPipelineConfig config = Defaults();
         config.ClamAvEnabled = true;
         config.ClamAvHost = "";
         config.HoldUnscanned = true;

         Assert.False(HasNote(config, StatusLevel.Critical, "found clean"));
         Assert.True(HasNote(config, StatusLevel.Critical, "held and scanned again"));
         Assert.Contains("held and scanned again", VirusPipeline.WhenNoScannerAnswers(config));
         Assert.DoesNotContain("found clean", VirusPipeline.WhenNoScannerAnswers(config));
      }

      [Fact]
      public void ByDefault_WhatNoScannerExaminedIsDeliveredAsThoughClean()
      {
         Assert.Contains("examined and found clean", VirusPipeline.WhenNoScannerAnswers(Defaults()));
      }

      [Fact]
      public void APortOfZeroCountsAsUnableToRun()
      {
         VirusPipelineConfig config = Defaults();
         config.ClamAvEnabled = true;
         config.ClamAvPort = 0;

         Assert.False(VirusPipeline.Scanners(config).Single(s => s.Key == "clamav").Usable);
         Assert.Equal(0, VirusPipeline.CountUsable(config));
      }

      [Fact]
      public void ClamWinNeedsBothTheExecutableAndTheDatabaseFolder()
      {
         // ClamWinVirusScanner builds `--database="<folder>"` into the command line,
         // so a missing folder is not a scanner running with a default - it is a
         // scanner invoked with an empty argument.
         VirusPipelineConfig config = Defaults();
         config.ClamWinEnabled = true;
         config.ClamWinExecutable = @"C:\Program Files\ClamWin\bin\clamscan.exe";
         config.ClamWinDatabaseFolder = "";

         VirusScannerEntry clamWin = VirusPipeline.Scanners(config).Single(s => s.Key == "clamwin");

         Assert.False(clamWin.Usable);
         Assert.Contains("signature database folder", clamWin.Problem);
      }

      [Fact]
      public void ACustomScannerReturnValueOfZeroIsNotTreatedAsMissing()
      {
         // The return code that means "infected" is a number, and 0 is a legitimate
         // choice of number. Only the executable is required.
         VirusPipelineConfig config = Defaults();
         config.CustomScannerEnabled = true;
         config.CustomScannerExecutable = @"C:\scanner.exe";
         config.CustomScannerVirusReturnValue = 0;

         Assert.True(VirusPipeline.Scanners(config).Single(s => s.Key == "custom").Usable);
      }

      // ---- the order --------------------------------------------------------

      [Fact]
      public void ScannersAreListedInTheOrderTheServerTriesThem()
      {
         // VirusScanner::ScanFile_ asks ClamWin, then the custom scanner, then
         // ClamAV. The page states an order, so the order has to be the real one.
         string[] keys = VirusPipeline.Scanners(Defaults()).Select(s => s.Key).ToArray();

         Assert.Equal(new[] { "clamwin", "custom", "clamav" }, keys);
         Assert.Equal(new[] { 1, 2, 3 }, VirusPipeline.Scanners(Defaults()).Select(s => s.Order).ToArray());
      }

      // ---- the size limit, which is backwards -------------------------------

      [Fact]
      public void ASizeLimitOfZeroMeansEverythingIsScanned()
      {
         VirusPipelineConfig config = Scanning();
         config.MaxScanKilobytes = 0;

         Assert.Contains("Every message is scanned", VirusPipeline.Verdict(config));
         Assert.False(HasNote(config, StatusLevel.Warning, "are not scanned"));
      }

      [Fact]
      public void APositiveSizeLimitIsReportedAsAGapRatherThanAsProtection()
      {
         VirusPipelineConfig config = Scanning();
         config.MaxScanKilobytes = 1024;

         // The verdict says "within the size limit"; the Warning under it says the
         // limit and what becomes of a larger message (review finding F265).
         Assert.Contains("within the size limit", VirusPipeline.Verdict(config));
         Assert.DoesNotContain("1,024 KB", VirusPipeline.Verdict(config));

         // Named as a warning, and the note has to say the message is delivered
         // rather than refused - that is the part an administrator gets wrong.
         Assert.True(HasNote(config, StatusLevel.Warning, "are not scanned"));
         Assert.True(HasNote(config, StatusLevel.Warning, "delivered, not refused"));
      }

      [Fact]
      public void TheSizeLimitIsShownInMegabytesOnceKilobytesStopBeingReadable()
      {
         Assert.Equal("512 KB", VirusPipeline.Kilobytes(512));
         Assert.Equal("10,240 KB (10 MB)", VirusPipeline.Kilobytes(10240));
      }

      // ---- what happens on a find -------------------------------------------

      [Fact]
      public void StrippingAttachmentsSaysTheMessageIsStillDelivered()
      {
         VirusPipelineConfig config = Scanning();
         config.Action = VirusAction.StripAttachments;

         Assert.Contains("still delivered", VirusPipeline.Verdict(config));
         Assert.True(HasNote(config, StatusLevel.Warning, "still receives the message body"));
      }

      [Fact]
      public void TheNotificationsAreReportedAsInertWhenTheActionIsToStrip()
      {
         // HandleInfectedMessage_ sends the notifications only on the delete branch.
         // Both switches still read as on in the settings page, so the overview has
         // to be the thing that says they do nothing.
         VirusPipelineConfig config = Scanning();
         config.Action = VirusAction.StripAttachments;
         config.NotifySender = true;
         config.NotifyRecipient = true;

         Assert.Contains("Neither notification setting applies", VirusPipeline.ActionSummary(config));

         // And the backscatter warning must NOT fire, because nothing is sent.
         Assert.False(HasNote(config, StatusLevel.Warning, "backscatter"));
      }

      [Fact]
      public void NotifyingTheSenderOfAVirusIsCalledOutAsBackscatter()
      {
         VirusPipelineConfig config = Scanning();
         config.Action = VirusAction.DeleteMessage;
         config.NotifySender = true;

         Assert.Contains("the envelope sender", VirusPipeline.ActionSummary(config));
         Assert.True(HasNote(config, StatusLevel.Warning, "backscatter"));
      }

      [Fact]
      public void DeletingWithNoNotificationsSaysNobodyIsTold()
      {
         VirusPipelineConfig config = Scanning();

         Assert.Contains("nobody is told", VirusPipeline.ActionSummary(config));
      }

      // ---- the ways round the whole page --------------------------------------

      [Fact]
      public void FetchAccountsWithScanningOffAreReportedBecauseTheSwitchIsElsewhere()
      {
         VirusPipelineConfig config = Scanning();
         config.FetchAccountsTotal = 3;
         config.FetchAccountsWithScanningOff = 2;

         Assert.True(HasNote(config, StatusLevel.Warning, "delivered unscanned no matter what is set here"));
      }

      [Fact]
      public void ATruncatedFetchAccountWalkSaysAtLeastRatherThanStatingATotal()
      {
         // The walk is one COM round trip per account and is capped so the page stays
         // responsive on a large server. A capped count presented as a total would be
         // a number that reads as reassuring precisely when it is least reliable.
         VirusPipelineConfig config = Scanning();
         config.FetchAccountsTotal = 1000;
         config.FetchAccountsWithScanningOff = 2;
         config.FetchAccountScanIncomplete = true;

         Assert.True(HasNote(config, StatusLevel.Warning, "At least 2 external POP3 fetch accounts have"));
         Assert.True(HasNote(config, StatusLevel.Warning, "there may be others"));

         config.FetchAccountScanIncomplete = false;

         Assert.False(HasNote(config, StatusLevel.Warning, "At least 2"));
         Assert.False(HasNote(config, StatusLevel.Warning, "there may be others"));
      }

      [Fact]
      public void AttachmentBlockingOnWithNoPatternsIsReportedAsDoingNothing()
      {
         VirusPipelineConfig config = Scanning();
         config.BlockedAttachmentPatterns = 0;

         Assert.True(HasNote(config, StatusLevel.Warning, "no patterns to block"));
      }

      [Fact]
      public void BlockingStillCountsWhenNoScannerCanRun()
      {
         // RunVirusProtection_ blocks attachments before it looks at the scanners at
         // all, so "no scanner" and "nothing at all" are different states and the
         // page must not collapse them.
         VirusPipelineConfig config = Defaults();
         config.AttachmentBlockingEnabled = true;

         Assert.Contains("Attachment blocking is still stripping", VirusPipeline.Verdict(config));
         Assert.False(HasNote(config, StatusLevel.Critical, "Nothing examines incoming mail"));
         Assert.True(HasNote(config, StatusLevel.Critical, "No virus scanner can run"));
      }

      /// <summary>
      /// Attachment blocking switched on with no pattern strips nothing, so with no
      /// scanner that can run nothing examines incoming mail - as the verdict says,
      /// "no attachment is blocked", and the Warning that blocking does nothing at
      /// all - where the Critical note said that attachment blocking still strips
      /// the file-name patterns on its own page.
      /// </summary>
      [Fact]
      public void NoScannerAndBlockingWithNoPatterns_IsNothingExaminingIncomingMail()
      {
         VirusPipelineConfig config = Defaults();
         config.AttachmentBlockingEnabled = true;
         config.BlockedAttachmentPatterns = 0;

         Assert.Contains("no attachment is blocked", VirusPipeline.Verdict(config));
         Assert.True(HasNote(config, StatusLevel.Critical, "Nothing examines incoming mail. No scanner can run and attachment blocking has no patterns"));
         Assert.False(HasNote(config, StatusLevel.Critical, "still strips"));
         Assert.False(HasNote(config, StatusLevel.Critical, "attachment blocking is off"));
         Assert.True(HasNote(config, StatusLevel.Warning, "no patterns to block"));
         Assert.Single(VirusPipeline.Notes(config), n => n.Level == StatusLevel.Critical);
      }

      // ---- the shared vocabulary ---------------------------------------------

      [Fact]
      public void EveryNoteCarriesTextAndAUsableLevel()
      {
         // The page renders the level through StatusSemantics, which has a
         // presentation for every level; a note with no text would render as a bare
         // severity word and say nothing.
         var configs = new List<VirusPipelineConfig>
         {
            Defaults(),
            Scanning(),
            new() { ClamAvEnabled = true, ClamAvHost = "", MaxScanKilobytes = 500, NotifySender = true }
         };

         foreach (VirusPipelineConfig config in configs)
         {
            foreach (VirusPipelineNote note in VirusPipeline.Notes(config))
            {
               Assert.False(string.IsNullOrWhiteSpace(note.Text));
               Assert.NotNull(StatusSemantics.For(note.Level));
            }
         }
      }

      [Fact]
      public void ANullConfigurationIsSurvivedRatherThanThrown()
      {
         // ReadConfig hands back whatever it managed to read when COM fails part-way,
         // and the page draws it. Nothing here may throw on the degraded case.
         Assert.Empty(VirusPipeline.Scanners(null));
         Assert.Empty(VirusPipeline.Notes(null));
         Assert.Equal(0, VirusPipeline.CountUsable(null));
         Assert.Equal(StatusLevel.Normal, VirusPipeline.WorstLevel(null));
         Assert.False(string.IsNullOrWhiteSpace(VirusPipeline.Verdict(null)));
      }

      // ---- values that could not be read ---------------------------------------
      //
      // A value the server did not answer is the type's default in the snapshot:
      // a scanner read as off, a host read as blank, a size limit read as 0. The
      // virus overview drew those as the configuration when the link was lost -
      // "No message is scanned for viruses. Everything is delivered as it arrives."

      [Fact]
      public void AnUnreadScanner_IsNotRead_AndTheVerdictIsNotGiven()
      {
         VirusPipelineConfig config = Scanning();
         config.ClamAvHost = "";
         config.Unread.Add(nameof(config.ClamAvHost));

         VirusScannerEntry clamAv = VirusPipeline.Scanners(config).Single(s => s.Key == "clamav");
         Assert.True(clamAv.Unread);
         Assert.False(clamAv.Enabled);
         Assert.False(clamAv.Usable);
         Assert.Null(clamAv.Problem);
         Assert.Equal("Not read", clamAv.StateText);

         Assert.False(VirusPipeline.ScannersRead(config));
         Assert.Contains("could not be read", VirusPipeline.Verdict(config));

         // Neither "cannot run" nor "nothing examines incoming mail" is said of a
         // scanner that was not read.
         Assert.DoesNotContain(VirusPipeline.Notes(config), n => n.Level == StatusLevel.Critical);
      }

      [Fact]
      public void AnUnreadAction_LeavesWhatHappensToAnInfectedMessageUnsaid()
      {
         VirusPipelineConfig config = Scanning();
         config.NotifySender = true;
         config.Unread.Add(nameof(config.Action));

         Assert.Contains("could not be read", VirusPipeline.ActionSummary(config));
         Assert.DoesNotContain(VirusPipeline.Notes(config), n => n.Text.Contains("backscatter"));
      }

      [Fact]
      public void AConfigurationReadInFull_HasNothingUnread()
      {
         VirusPipelineConfig config = Scanning();
         Assert.True(VirusPipeline.ScannersRead(config));
         Assert.DoesNotContain(VirusPipeline.Scanners(config), s => s.Unread);
         Assert.DoesNotContain("could not be read", VirusPipeline.Verdict(config));
      }

      // ---- whole sentences, one per case ----------------------------------------
      //
      // A sentence put together from a counted noun ("2 scanners"), a pronoun
      // ("them is") and a template could not be translated: the pieces are
      // translated apart, and Czech read "z ten neběží", Japanese put a Latin "s"
      // on its noun. Each case is now one text in the catalogue, and these pin the
      // English of each so that no piece can come back.

      /// <summary>Scanning() with the custom scanner able to run as well: two usable scanners.</summary>
      private static VirusPipelineConfig TwoScanners()
      {
         VirusPipelineConfig config = Scanning();
         config.CustomScannerEnabled = true;
         config.CustomScannerExecutable = @"C:\scan\scan.exe";
         config.CustomScannerVirusReturnValue = 1;
         return config;
      }

      /// <summary>
      /// The verdict names the scanners that can run, in the order the server
      /// tries them, where it counted them; and with a size limit it says "within
      /// the size limit" and no more, where it said the limit and what becomes of
      /// a larger message, which the Warning under it and the next card's first
      /// line say too (review finding F265). A whole sentence for each count.
      /// </summary>
      [Fact]
      public void TheVerdictNamesTheScannersInTurn_InOneWholeSentencePerCount()
      {
         VirusPipelineConfig config = Scanning();
         Assert.Equal("Every message is scanned by one scanner: ClamAV. If a virus is found, the whole message is deleted and never delivered.",
            VirusPipeline.Verdict(config));

         config.Action = VirusAction.StripAttachments;
         Assert.Equal("Every message is scanned by one scanner: ClamAV. If a virus is found, the attachments are stripped and the message is still delivered.",
            VirusPipeline.Verdict(config));

         config = TwoScanners();
         Assert.Equal(2, VirusPipeline.CountUsable(config));
         Assert.Equal("Every message is scanned by two scanners in turn: Custom scanner, then ClamAV. If a virus is found, the whole message is deleted and never delivered.",
            VirusPipeline.Verdict(config));

         config.MaxScanKilobytes = 512;
         Assert.Equal("Messages within the size limit are scanned by two scanners in turn: Custom scanner, then ClamAV. If a virus is found, the whole message is deleted and never delivered.",
            VirusPipeline.Verdict(config));
         Assert.True(HasNote(config, StatusLevel.Warning, "Messages larger than 512 KB are not scanned."));

         config = Scanning();
         config.MaxScanKilobytes = 512;
         Assert.Equal("Messages within the size limit are scanned by one scanner: ClamAV. If a virus is found, the whole message is deleted and never delivered.",
            VirusPipeline.Verdict(config));

         config = TwoScanners();
         config.ClamWinEnabled = true;
         config.ClamWinExecutable = @"C:\ClamWin\clamscan.exe";
         config.ClamWinDatabaseFolder = @"C:\ClamWin\db";
         Assert.Equal("Every message is scanned by three scanners in turn: ClamWin, then Custom scanner, then ClamAV. If a virus is found, the whole message is deleted and never delivered.",
            VirusPipeline.Verdict(config));
         config.MaxScanKilobytes = 512;
         Assert.Equal("Messages within the size limit are scanned by three scanners in turn: ClamWin, then Custom scanner, then ClamAV. If a virus is found, the whole message is deleted and never delivered.",
            VirusPipeline.Verdict(config));

         // A scanner switched on that cannot run is not named.
         config = Scanning();
         config.CustomScannerEnabled = true;
         Assert.Equal("Every message is scanned by one scanner: ClamAV. If a virus is found, the whole message is deleted and never delivered.",
            VirusPipeline.Verdict(config));
      }

      [Fact]
      public void TheVerdictCountsTheBlockedPatternsInOneWholeSentence()
      {
         VirusPipelineConfig config = Defaults();
         config.AttachmentBlockingEnabled = true;
         Assert.Equal("No message is scanned for viruses. Attachment blocking is still stripping 20 file-name patterns, but nothing examines the contents of a message.",
            VirusPipeline.Verdict(config));

         config.BlockedAttachmentPatterns = 1;
         Assert.Equal("No message is scanned for viruses. Attachment blocking is still stripping one file-name pattern, but nothing examines the contents of a message.",
            VirusPipeline.Verdict(config));
      }

      [Fact]
      public void TheActionSummaryNamesWhoIsToldInOneWholeSentence()
      {
         VirusPipelineConfig config = Scanning();
         config.NotifyRecipient = true;
         Assert.Equal("The message is deleted and a notice is sent to every recipient.", VirusPipeline.ActionSummary(config));

         config.NotifySender = true;
         Assert.Equal("The message is deleted and a notice is sent to every recipient and to the envelope sender.", VirusPipeline.ActionSummary(config));

         config.NotifyRecipient = false;
         Assert.Equal("The message is deleted and a notice is sent to the envelope sender.", VirusPipeline.ActionSummary(config));
      }

      [Fact]
      public void TheFetchAccountNoteIsOneWholeSentencePerCountAndPerFloor()
      {
         VirusPipelineConfig config = Scanning();
         config.FetchAccountsTotal = 3;

         config.FetchAccountsWithScanningOff = 1;
         Assert.True(HasNote(config, StatusLevel.Warning, "One external POP3 fetch account has anti-virus turned off, so mail collected by it is delivered unscanned no matter what is set here. The switch is on the fetch account, not on this page."));

         config.FetchAccountsWithScanningOff = 2;
         Assert.True(HasNote(config, StatusLevel.Warning, "2 external POP3 fetch accounts have anti-virus turned off, so mail collected by them is delivered unscanned no matter what is set here. The switch is on the fetch account, not on this page."));

         config.FetchAccountScanIncomplete = true;
         Assert.True(HasNote(config, StatusLevel.Warning, "At least 2 external POP3 fetch accounts have anti-virus turned off, so mail collected by them is delivered unscanned no matter what is set here. The switch is on the fetch account, not on this page. There are more accounts on this server than this page walks, so there may be others."));

         config.FetchAccountsWithScanningOff = 1;
         Assert.True(HasNote(config, StatusLevel.Warning, "At least one external POP3 fetch account has anti-virus turned off, so mail collected by it is delivered unscanned no matter what is set here. The switch is on the fetch account, not on this page. There are more accounts on this server than this page walks, so there may be others."));
      }

      [Fact]
      public void TheUnappliedPatternsNoteIsOneWholeSentenceForOneAndForSeveral()
      {
         VirusPipelineConfig config = Defaults();
         Assert.True(HasNote(config, StatusLevel.Information, "20 blocked-attachment patterns are configured but attachment blocking is switched off, so none of them is applied."));

         config.BlockedAttachmentPatterns = 1;
         Assert.True(HasNote(config, StatusLevel.Information, "One blocked-attachment pattern is configured but attachment blocking is switched off, so it is not applied."));
      }

      [Fact]
      public void TheScannersInTurnNoteCountsThemInOneWholeSentence()
      {
         Assert.True(HasNote(TwoScanners(), StatusLevel.Information, "Every message goes through 2 scanners in turn, and each is asked about the whole message"));
         Assert.False(HasNote(Scanning(), StatusLevel.Information, "in turn, and each is asked"));
      }

      [Fact]
      public void NoVerdictOrNoteBeginsOrEndsInTheMiddleOfASentence()
      {
         // A text that starts with a lower-case word or ends without a full stop is
         // a piece of a sentence, which is what the catalogue must never hold.
         VirusPipelineConfig blocking = Defaults();
         blocking.AttachmentBlockingEnabled = true;
         VirusPipelineConfig gaps = TwoScanners();
         gaps.MaxScanKilobytes = 512;
         gaps.Action = VirusAction.StripAttachments;
         gaps.FetchAccountsWithScanningOff = 2;
         gaps.FetchAccountScanIncomplete = true;
         gaps.ClamWinEnabled = true;
         VirusPipelineConfig notices = Scanning();
         notices.NotifySender = true;
         notices.FetchAccountsWithScanningOff = 1;

         foreach (VirusPipelineConfig config in new[] { Defaults(), Scanning(), blocking, gaps, notices })
         {
            var texts = new List<string> { VirusPipeline.Verdict(config), VirusPipeline.ActionSummary(config) };
            texts.AddRange(VirusPipeline.Notes(config).Select(n => n.Text));
            foreach (string text in texts.Where(t => t.Length > 0))
            {
               Assert.True(char.IsUpper(text[0]) || char.IsDigit(text[0]), text);
               Assert.EndsWith(".", text);
            }
         }
      }

      // ---- the verdict's level, and the problems' pages ------------------------

      /// <summary>
      /// The verdict is shown at the level of what it says: "No message is scanned
      /// for viruses... Everything is delivered as it arrives." was plain body text.
      /// At the Warning level and above the notes say it, each with the page that
      /// puts it right, and the verdict is the card's lead sentence.
      /// </summary>
      [Fact]
      public void TheVerdict_HasTheLevelOfWhatItSays()
      {
         Assert.Equal(StatusLevel.Critical, VirusPipeline.VerdictLevel(Defaults()));
         Assert.True(VirusPipeline.NotesSayTheVerdict(Defaults()));

         VirusPipelineConfig scanning = Scanning();
         Assert.Equal(StatusLevel.Good, VirusPipeline.VerdictLevel(scanning));
         Assert.False(VirusPipeline.NotesSayTheVerdict(scanning));

         scanning.MaxScanKilobytes = 10240;
         Assert.Equal(StatusLevel.Warning, VirusPipeline.VerdictLevel(scanning));
         Assert.True(VirusPipeline.NotesSayTheVerdict(scanning));

         scanning.MaxScanKilobytes = 0;
         scanning.Action = VirusAction.StripAttachments;
         Assert.Equal(StatusLevel.Warning, VirusPipeline.VerdictLevel(scanning));

         scanning.Unread.Add(nameof(scanning.Action));
         Assert.Equal(StatusLevel.Information, VirusPipeline.VerdictLevel(scanning));
         Assert.False(VirusPipeline.NotesSayTheVerdict(scanning));
         Assert.False(VirusPipeline.NotesSayTheVerdict(null));
      }

      /// <summary>
      /// A verdict at Warning or above is always said by a note at its level, so
      /// that the count of problems beside the title - the notes at Warning and
      /// above - is never "No problems found" beside it, and the overview draws
      /// no notice of the verdict's own at that level (NotesSayTheVerdict):
      /// "Messages up to 10,240 KB (10 MB) are scanned by one scanner; anything
      /// larger is delivered without being scanned at all..." was a third Warning,
      /// with the same Open Anti-virus settings, above the two notes that say it,
      /// beside "2 problems".
      /// </summary>
      [Fact]
      public void AVerdictAtWarningOrAbove_IsSaidByANoteAtItsLevel()
      {
         var configs = new List<VirusPipelineConfig> { Defaults(), Scanning() };

         VirusPipelineConfig blockingOnly = Defaults();
         blockingOnly.AttachmentBlockingEnabled = true;
         configs.Add(blockingOnly);

         VirusPipelineConfig limited = Scanning();
         limited.MaxScanKilobytes = 10240;
         configs.Add(limited);

         VirusPipelineConfig stripping = Scanning();
         stripping.Action = VirusAction.StripAttachments;
         configs.Add(stripping);

         VirusPipelineConfig noPatterns = Scanning();
         noPatterns.BlockedAttachmentPatterns = 0;
         configs.Add(noPatterns);

         VirusPipelineConfig noScannerNoPatterns = Defaults();
         noScannerNoPatterns.AttachmentBlockingEnabled = true;
         noScannerNoPatterns.BlockedAttachmentPatterns = 0;
         configs.Add(noScannerNoPatterns);

         foreach (VirusPipelineConfig config in configs)
         {
            StatusLevel verdict = VirusPipeline.VerdictLevel(config);
            Assert.Equal(verdict >= StatusLevel.Warning, VirusPipeline.NotesSayTheVerdict(config));
            if (verdict >= StatusLevel.Warning)
               Assert.Contains(VirusPipeline.Notes(config), n => n.Level >= verdict);
         }
      }

      /// <summary>
      /// The attachment sandbox, which the overview did not mention (review
      /// finding F265): off as shipped; on, what its verdicts do, by the two
      /// settings that decide it; on with an address empty, a Warning, on the card
      /// and among the problems, in the external setup checklist's words; and
      /// nothing said of a value not read.
      /// </summary>
      [Fact]
      public void TheSandbox_SaysWhetherItIsOnAndWhatItsVerdictsDo()
      {
         VirusPipelineConfig config = Scanning();
         (StatusLevel level, string state, string sentence) = VirusPipeline.Sandbox(config);
         Assert.Equal((StatusLevel.Normal, "Off"), (level, state));
         Assert.Equal("The sandbox is off, which is the default: nothing is held and nothing is submitted.", sentence);
         Assert.DoesNotContain(VirusPipeline.Notes(config), n => n.Text.Contains("sandbox", StringComparison.OrdinalIgnoreCase));

         config.SandboxEnabled = true;
         config.SandboxSubmitUrl = "https://sandbox.example/submit";
         config.SandboxVerdictUrl = "";
         (level, state, sentence) = VirusPipeline.Sandbox(config);
         Assert.Equal((StatusLevel.Warning, "On, but cannot run"), (level, state));
         Assert.Equal("The sandbox is switched on, but its submit or verdict address is empty.", sentence);
         VirusPipelineNote inert = VirusPipeline.Notes(config).Single(n => n.Text.StartsWith("The sandbox is ON but an address is empty", StringComparison.Ordinal));
         Assert.Equal((StatusLevel.Warning, "antivirus"), (inert.Level, inert.Page));

         config.SandboxVerdictUrl = "https://sandbox.example/verdict";
         Assert.DoesNotContain(VirusPipeline.Notes(config), n => n.Text.Contains("sandbox", StringComparison.OrdinalIgnoreCase));
         (level, state, sentence) = VirusPipeline.Sandbox(config);
         Assert.Equal((StatusLevel.Good, "On"), (level, state));
         Assert.Equal("Mail from outside with an attachment of a submitted type waits for the sandbox: a clean verdict delivers it, a malicious one returns it to its sender while quarantining is off, and no verdict in time delivers it with a header saying so.", sentence);

         config.SandboxTimeoutReturns = true;
         Assert.Equal("Mail from outside with an attachment of a submitted type waits for the sandbox: a clean verdict delivers it, a malicious one returns it to its sender while quarantining is off, and no verdict in time returns it too.",
            VirusPipeline.Sandbox(config).Sentence);

         config.QuarantineEnabled = true;
         Assert.Equal("Mail from outside with an attachment of a submitted type waits for the sandbox: a clean verdict delivers it, a malicious one quarantines it, and no verdict in time returns it to its sender.",
            VirusPipeline.Sandbox(config).Sentence);

         config.SandboxTimeoutReturns = false;
         Assert.Equal("Mail from outside with an attachment of a submitted type waits for the sandbox: a clean verdict delivers it, a malicious one quarantines it, and no verdict in time delivers it with a header saying so.",
            VirusPipeline.Sandbox(config).Sentence);

         // What the verdicts do, not read: the sentence that does not say.
         config.Unread.Add(nameof(config.QuarantineEnabled));
         Assert.Equal("Mail from outside with an attachment of a submitted type waits for the sandbox's verdict before it is delivered.",
            VirusPipeline.Sandbox(config).Sentence);

         // The switch not read: nothing is said, and no note.
         config.Unread.Add(nameof(config.SandboxEnabled));
         config.SandboxVerdictUrl = "";
         (level, state, sentence) = VirusPipeline.Sandbox(config);
         Assert.Equal((StatusLevel.Normal, "Not read", "These settings could not be read."), (level, state, sentence));
         Assert.DoesNotContain(VirusPipeline.Notes(config), n => n.Text.Contains("sandbox", StringComparison.OrdinalIgnoreCase));

         Assert.Equal("Not read", VirusPipeline.Sandbox(null).State);
      }

      /// <summary>
      /// Each note names the page that puts it right: attachment blocking with no
      /// patterns on Blocked attachments, fetch accounts on Domains, the rest on
      /// Anti-virus settings.
      /// </summary>
      [Fact]
      public void EachNote_NamesThePageThatPutsItRight()
      {
         VirusPipelineConfig config = Scanning();
         config.BlockedAttachmentPatterns = 0;
         config.FetchAccountsTotal = 3;
         config.FetchAccountsWithScanningOff = 1;
         config.MaxScanKilobytes = 10240;

         IReadOnlyList<VirusPipelineNote> notes = VirusPipeline.Notes(config);
         Assert.Equal("blockedattachments", notes.Single(n => n.Text.StartsWith("Attachment blocking is switched on with no patterns", StringComparison.Ordinal)).Page);
         Assert.Equal("domains", notes.Single(n => n.Text.Contains("fetch account", StringComparison.Ordinal)).Page);
         Assert.Equal("antivirus", notes.Single(n => n.Text.StartsWith("Messages larger than", StringComparison.Ordinal)).Page);
         Assert.All(notes, n => Assert.NotNull(NavigationMap.Find(n.Page)));
      }
   }
}
