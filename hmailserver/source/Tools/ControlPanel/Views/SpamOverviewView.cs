// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Documents;
using System.Windows.Media;
using System.Windows.Shapes;
using hMailServer.ControlPanel.Services;

// The Control Panel has its own Typography (the type scale, in Services) and
// System.Windows.Documents declares one too. That import is needed here for Run and
// Inlines, so the reference is aliased to the one meant rather than dropping the import.
using Typography = hMailServer.ControlPanel.Services.Typography;
using Card = hMailServer.ControlPanel.Views.Scaffold.Card;
using FieldRow = hMailServer.ControlPanel.Views.Scaffold.FieldRow;
using InlineNotice = hMailServer.ControlPanel.Views.Scaffold.InlineNotice;
using PageHeader = hMailServer.ControlPanel.Views.Scaffold.PageHeader;
using StatusPill = hMailServer.ControlPanel.Views.Scaffold.StatusPill;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// One page that answers "what will my spam filtering do to a message".
   ///
   /// Anti-spam was five pages and none of them could answer that, because the
   /// answer is not on any one of them: the checks and their scores are on
   /// Anti-spam settings, two more checks are lists on their own pages, and what a
   /// score then means is two thresholds whose interaction is documented nowhere.
   /// The roadmap put it as "there is no single page that answers what is my spam
   /// configuration".
   ///
   /// Read-only on purpose. It is a diagnosis, not a sixth editor: every row links
   /// to the page that owns the setting, so there is still exactly one place to
   /// change each value - which is also why this page cannot drift out of step with
   /// them.
   ///
   /// The judgement all lives in <see cref="SpamPipeline"/>, which has no WPF and is
   /// tested directly; this file reads the COM settings into a snapshot and draws
   /// it.
   /// </summary>
   public class SpamOverviewView : UserControl, IPageLifecycle
   {
      // The card width under which "Which list do I need?" gives its four columns up
      // for one block a list.
      private const double GuideTableWidth = 720;

      private readonly StackPanel body_ = new();

      // A read that failed is a warning in the flow of the page, not a grey line
      // at the bottom: it says the rows below may be incomplete, and on a page
      // whose whole purpose is to be believed that has to be loud.
      private readonly InlineNotice readFailure_ = new()
      {
         Level = StatusLevel.Warning,
         Visibility = Visibility.Collapsed
      };

      // Where the values came from - a provenance footnote, the quietest text.
      private readonly TextBlock status_ = new();

      // The page's title, whose status is the count of problems found.
      private readonly PageHeader header_;

      // "Check a sender" (review finding F269), built once and put back on every
      // read, so that what was typed outlives a Refresh. Its answer is cleared by
      // a read: the lists may have changed under it.
      private readonly System.Windows.Controls.TextBox checkAddress_ = new() { MinWidth = 260 };
      private readonly System.Windows.Controls.TextBox checkIp_ = new() { MinWidth = 180 };
      private readonly Wpf.Ui.Controls.Button checkButton_ = new() { Content = L("_Check"), Appearance = Wpf.Ui.Controls.ControlAppearance.Primary };
      private readonly InlineNotice checkNotice_ = new() { Visibility = Visibility.Collapsed, Margin = new Thickness(0, 8, 0, 0) };
      private readonly StackPanel checkResults_ = new() { Margin = new Thickness(0, 8, 0, 0) };
      private Card checkCard_;

      public SpamOverviewView()
      {
         var page = new StackPanel { MaxWidth = DesignTokens.OverviewWidth, HorizontalAlignment = HorizontalAlignment.Left };

         var refresh = new Wpf.Ui.Controls.Button { Content = L("_Refresh") };
         ControlHelp.Explain(refresh, L("Re-read the spam configuration from the server"));
         System.Windows.Automation.AutomationProperties.SetAutomationId(refresh, "spam-overview-refresh");
         refresh.Click += (s, e) => Reload();

         page.Children.Add(header_ = new PageHeader
         {
            PageKey = "spamoverview",
            Subtitle = L("Each spam check in the order the server runs it, what it adds to the score, and what the total does to a message. Nothing here can be edited: each row's link opens the page that owns its setting."),
            Actions = refresh
         });

         page.Children.Add(readFailure_);
         page.Children.Add(body_);

         status_.Margin = new Thickness(0, 12, 0, 0);
         status_.SetResourceReference(StyleProperty, "TextCaptionTertiary");
         page.Children.Add(status_);

         var scroller = new ScrollViewer { Content = page, VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
         scroller.SetResourceReference(PaddingProperty, "AppPagePadding");
         Content = scroller;
      }

      public void OnEnter() => Reload();

      public void OnLeave()
      {
      }

      // ---- reading the configuration -----------------------------------------

      private int failedReads_;
      private string firstError_;

      // Whether the anti-spam settings object could be opened at all. When it
      // could not, every value in the snapshot is a default, and a page drawn
      // from defaults says "both thresholds are 0" of a server it never asked.
      private bool configRead_;

      /// <summary>
      /// Reads the anti-spam settings into a snapshot.
      ///
      /// Every property is read through <see cref="Read{T}"/> so that one property
      /// the server does not have - an older build, a COM error - costs that one
      /// value and its row, rather than the whole page. The count of failures is
      /// shown at the bottom instead of being swallowed: a page that quietly shows
      /// zeroes for settings it could not read would be worse than an error, since
      /// zero thresholds are themselves a finding here.
      /// </summary>
      private SpamPipelineConfig ReadConfig()
      {
         failedReads_ = 0;
         firstError_ = null;
         configRead_ = false;

         var config = new SpamPipelineConfig();
         dynamic settings = null;
         dynamic antiSpam = null;

         try
         {
            settings = ServerSession.Current.Application.Settings;
            antiSpam = settings.AntiSpam;
            configRead_ = true;

            config.MarkThreshold = Read(config, nameof(config.MarkThreshold), () => (int)antiSpam.SpamMarkThreshold);
            config.DeleteThreshold = Read(config, nameof(config.DeleteThreshold), () => (int)antiSpam.SpamDeleteThreshold);
            config.MaxScanKilobytes = Read(config, nameof(config.MaxScanKilobytes), () => (int)antiSpam.MaximumMessageSize);
            config.BlockedSenderCount = Read(config, nameof(config.BlockedSenderCount), () => (int)antiSpam.BlockedSenders.Count);

            config.AddSpamHeader = Read(config, nameof(config.AddSpamHeader), () => (bool)antiSpam.AddHeaderSpam);
            config.AddReasonHeader = Read(config, nameof(config.AddReasonHeader), () => (bool)antiSpam.AddHeaderReason);
            config.PrependSubject = Read(config, nameof(config.PrependSubject), () => (bool)antiSpam.PrependSubject);

            config.CheckHeloHost = Read(config, nameof(config.CheckHeloHost), () => (bool)antiSpam.CheckHostInHelo);
            config.HeloHostScore = Read(config, nameof(config.HeloHostScore), () => (int)antiSpam.CheckHostInHeloScore);
            config.CheckPtr = Read(config, nameof(config.CheckPtr), () => (bool)antiSpam.CheckPTR);
            config.PtrScore = Read(config, nameof(config.PtrScore), () => (int)antiSpam.CheckPTRScore);
            config.CheckSenderMx = Read(config, nameof(config.CheckSenderMx), () => (bool)antiSpam.UseMXChecks);
            config.SenderMxScore = Read(config, nameof(config.SenderMxScore), () => (int)antiSpam.UseMXChecksScore);
            config.CheckSpf = Read(config, nameof(config.CheckSpf), () => (bool)antiSpam.UseSPF);
            config.SpfScore = Read(config, nameof(config.SpfScore), () => (int)antiSpam.UseSPFScore);

            config.VerifyDkim = Read(config, nameof(config.VerifyDkim), () => (bool)antiSpam.DKIMVerificationEnabled);
            config.DkimFailureScore = Read(config, nameof(config.DkimFailureScore), () => (int)antiSpam.DKIMVerificationFailureScore);
            config.EvaluateDmarc = Read(config, nameof(config.EvaluateDmarc), () => (bool)antiSpam.DMARCEnabled);
            config.DmarcFailureScore = Read(config, nameof(config.DmarcFailureScore), () => (int)antiSpam.DMARCFailureScore);

            config.SpamAssassinEnabled = Read(config, nameof(config.SpamAssassinEnabled), () => (bool)antiSpam.SpamAssassinEnabled);
            config.SpamAssassinMergesScore = Read(config, nameof(config.SpamAssassinMergesScore), () => (bool)antiSpam.SpamAssassinMergeScore);
            config.SpamAssassinScore = Read(config, nameof(config.SpamAssassinScore), () => (int)antiSpam.SpamAssassinScore);
            config.SpamAssassinHost = Read(config, nameof(config.SpamAssassinHost), () => (string)antiSpam.SpamAssassinHost) ?? "";

            config.GreylistingEnabled = Read(config, nameof(config.GreylistingEnabled), () => (bool)antiSpam.GreyListingEnabled);
            config.BypassGreylistingOnSpfPass = Read(config, nameof(config.BypassGreylistingOnSpfPass), () => (bool)antiSpam.BypassGreylistingOnSPFSuccess);
            config.BypassGreylistingOnSenderMx = Read(config, nameof(config.BypassGreylistingOnSenderMx), () => (bool)antiSpam.BypassGreylistingOnMailFromMX);

            CountList(config, nameof(config.ActiveDnsBlackLists), () => antiSpam.DNSBlackLists, true, out int dnsblActive, out int dnsblTotal);
            config.ActiveDnsBlackLists = dnsblActive;
            config.TotalDnsBlackLists = dnsblTotal;

            CountList(config, nameof(config.ActiveSurblServers), () => antiSpam.SURBLServers, true, out int surblActive, out int surblTotal);
            config.ActiveSurblServers = surblActive;
            config.TotalSurblServers = surblTotal;

            // No per-entry "active" flag on either white list, so every entry counts.
            CountList(config, nameof(config.WhiteListEntries), () => antiSpam.WhiteListAddresses, false, out _, out int whiteList);
            config.WhiteListEntries = whiteList;

            CountList(config, nameof(config.GreylistWhiteListEntries), () => antiSpam.GreyListingWhiteAddresses, false, out _, out int greyList);
            config.GreylistWhiteListEntries = greyList;

            // Read on its own, as the allow and block list is: a server older than
            // the bypass list has no such collection, and the row then reads 0.
            try
            {
               CountList(config, null, () => antiSpam.GreyListingBypassEntries, false, out _, out int greyBypass);
               config.GreylistBypassEntries = greyBypass;
            }
            catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck)) { /* Deliberately ignored: an older server has no greylisting bypass list. */ }

            // Read on its own: a server older than the allow and block list has no
            // such collection, and the rest of the overview must not fail with it.
            try
            {
               CountList(config, null, () => antiSpam.SenderList, false, out _, out int senderList);
               config.SenderListEntries = senderList;
            }
            catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck)) { /* Deliberately ignored: an older server has no allow and block list, and the row then reads 0 entries. */ }

            // The same, for the domain white and black list, which is a JSON
            // document on AntiSpam rather than a collection.
            try
            {
               config.DomainPolicyListEntries = CountDomainPolicyRows((string)antiSpam.DomainPolicyList);
            }
            catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck)) { /* Deliberately ignored: an older server has no domain white and black list, and the row then reads 0 entries. */ }

            // The external filtering engine is two [Settings] keys, read as the
            // Anti-spam settings page reads them: over COM, from hMailServer.ini
            // where the server does not answer.
            var ini = new IniFeatureStore();
            config.FilterHookUrl = Read(config, nameof(config.FilterHookUrl), () => ini.Read("FilterHookUrl", "")) ?? "";
            config.FilterHookRejectScore = Read(config, nameof(config.FilterHookRejectScore),
               () => int.Parse(ini.Read("FilterHookRejectScore", "100"), System.Globalization.NumberStyles.Integer, System.Globalization.CultureInfo.InvariantCulture));
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            failedReads_++;
            firstError_ ??= ServerSession.DescribeComError(ex);
         }
         finally
         {
            ServerSession.Release((object)antiSpam);
            ServerSession.Release((object)settings);
         }

         return config;
      }

      /// <summary>
      /// One value, recorded against its property when it could not be read, so
      /// that the row it feeds says "Not read" rather than showing the default.
      /// </summary>
      private T Read<T>(SpamPipelineConfig config, string property, Func<T> read)
      {
         try
         {
            return read();
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            failedReads_++;
            firstError_ ??= ServerSession.DescribeComError(ex);
            if (property != null)
               config.Unread.Add(property);
            return default;
         }
      }

      /// <summary>How many rows the domain white and black list's document carries; 0 for a document with none or one that cannot be read.</summary>
      private static int CountDomainPolicyRows(string json)
      {
         if (string.IsNullOrWhiteSpace(json))
            return 0;

         try
         {
            using System.Text.Json.JsonDocument document = System.Text.Json.JsonDocument.Parse(json);

            System.Text.Json.JsonElement items;
            if (document.RootElement.ValueKind == System.Text.Json.JsonValueKind.Object &&
                document.RootElement.TryGetProperty("items", out items) &&
                items.ValueKind == System.Text.Json.JsonValueKind.Array)
               return items.GetArrayLength();
         }
         catch (System.Text.Json.JsonException)
         {
            // A document that cannot be read is a list of nothing here; the page of its own says why.
         }

         return 0;
      }

      /// <summary>
      /// Counts a COM collection, and how many of its entries are active.
      ///
      /// The active count is the part that matters: neither DNSBL nor SURBL has an
      /// enable switch of its own - SpamTestDNSBlackLists::GetIsEnabled returns true
      /// when at least one entry is active - so "3 configured" and "3 active" mean
      /// completely different things and only the second one runs. The two white
      /// lists have no per-entry flag at all, which is what
      /// <paramref name="entriesHaveAnActiveFlag"/> distinguishes.
      /// </summary>
      private void CountList(SpamPipelineConfig config, string property, Func<object> open, bool entriesHaveAnActiveFlag, out int active, out int total)
      {
         active = 0;
         total = 0;

         dynamic collection = null;
         try
         {
            collection = open();
            total = (int)collection.Count;

            if (!entriesHaveAnActiveFlag)
            {
               active = total;
               return;
            }

            for (int i = 0; i < total; i++)
            {
               dynamic item = collection.Item[i];
               try
               {
                  if ((bool)item.Active)
                     active++;
               }
               finally
               {
                  ServerSession.Release((object)item);
               }
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            failedReads_++;
            firstError_ ??= ServerSession.DescribeComError(ex);
            if (property != null)
               config.Unread.Add(property);
         }
         finally
         {
            ServerSession.Release((object)collection);
         }
      }

      // ---- drawing ------------------------------------------------------------

      private void Reload()
      {
         SpamPipelineConfig config = ReadConfig();

         body_.Children.Clear();

         // Nothing could be read: no cards, since every value in them would be a
         // default drawn as a finding, and one Critical notice with Try again
         // (Services/ReadFailure).
         if (!configRead_)
         {
            ReadFailure.Show(readFailure_, null, null, L("The spam configuration could not be read: {0}"), firstError_, Reload);
            header_.Status = null;
            status_.Text = "";
            return;
         }

         ReadFailure.Clear(readFailure_);

         // The problems - Warning and above - under the verdict, each with the
         // page that puts it right; the rest at the foot, where all of them were.
         IReadOnlyList<SpamPipelineNote> notes = SpamPipeline.Notes(config);
         List<SpamPipelineNote> problems = OverviewNotices.Problems(notes, n => n.Level);
         List<SpamPipelineNote> worthKnowing = OverviewNotices.WorthKnowing(notes, n => n.Level);

         // "No problems found" only where every value was read: the notes that
         // follow from a value not read are left out, and the count said so in
         // green beside the Warning that says the sections may be incomplete.
         header_.Status = OverviewNotices.ProblemsPill(problems.Count, problems.Count == 0 ? StatusLevel.Good : problems[0].Level,
            failedReads_ == 0 && SpamPipeline.VerdictLevel(config) != StatusLevel.Information);

         body_.Children.Add(VerdictCard(config, problems));
         body_.Children.Add(ChecksCard(config));
         body_.Children.Add(GreylistingCard(config));
         body_.Children.Add(ListsCard(config));
         body_.Children.Add(CheckCard_());

         if (worthKnowing.Count > 0)
            body_.Children.Add(NotesCard(worthKnowing));

         status_.Text = L("Read from the server. Values are read again every time this page is opened.");

         // Some values could not be read: their rows say "Not read" and the
         // verdicts and notes that depend on them are left out, and this says so
         // above the sections it is about.
         // The reason last, where a sentence after it ran on from the runtime's text
         // in brackets that it may end with.
         readFailure_.Level = StatusLevel.Warning;
         readFailure_.Text = failedReads_ == 0
            ? null
            : failedReads_ == 1
               ? F("Some values could not be read, so the sections below may be incomplete. Why: {0}", firstError_)
               : F("Some values could not be read, so the sections below may be incomplete. The first reason: {0}", firstError_);
         readFailure_.Visibility = failedReads_ == 0 ? Visibility.Collapsed : Visibility.Visible;
      }

      /// <summary>One section of the page. Named Section and not Card because the
      /// component it builds is called that.</summary>
      private static Card Section(string title, out StackPanel content)
      {
         content = new StackPanel();
         var card = new Card { Title = title, Content = content };
         card.SetResourceReference(MarginProperty, "AppCardGap");
         // Each section is a level-2 heading under the page's title, so a screen
         // reader can move between them; the phases inside the checks are level 3.
         System.Windows.Automation.AutomationProperties.SetHeadingLevel(card, System.Windows.Automation.AutomationHeadingLevel.Level2);
         return card;
      }

      /// <summary>
      /// The verdict, then every problem, worst first, each with the page that
      /// puts it right. A verdict that is Good, or that cannot be said, is a
      /// notice at its level; one at the Warning level or above is said by a note
      /// at its level, and is the card's lead sentence, at no level and with no
      /// action, so that it is not said twice and the notices at the Warning level
      /// and above are the problems the count beside the title counts.
      /// </summary>
      private static Card VerdictCard(SpamPipelineConfig config, IReadOnlyList<SpamPipelineNote> problems)
      {
         Card card = Section(L("What happens to a message"), out StackPanel content);

         if (!SpamPipeline.NotesSayTheVerdict(config))
            content.Children.Add(OverviewNotices.Notice(SpamPipeline.VerdictLevel(config), SpamPipeline.Verdict(config), null, "spam-overview-verdict"));
         else
            content.Children.Add(OverviewNotices.Lead(SpamPipeline.Verdict(config), "spam-overview-verdict"));

         foreach (SpamPipelineNote problem in problems)
            content.Children.Add(OverviewNotices.Notice(problem.Level, problem.Text, problem.Page, "spam-overview-problem"));

         // The stop score is the higher threshold: with a threshold unread it
         // would be 0, and the sentence would say the server stops at once. The
         // ceiling sentence tells a check that could not be read from a total
         // that is open-ended, which is the pipeline's to say.
         if (SpamPipeline.ThresholdsRead(config))
         {
            int stop = SpamPipeline.StopScore(config);

            content.Children.Add(Paragraph(Sentences(F("The score is added up across both phases. The server stops testing within a phase as soon as the running total reaches {0} - the higher of the two thresholds - so a check late in the order may never run.", stop),
               SpamPipeline.CeilingSentence(config)),
               Typography.Caption));
         }

         return card;
      }

      private Card ChecksCard(SpamPipelineConfig config)
      {
         Card card = Section(L("Checks, in the order the server runs them"), out StackPanel content);

         foreach (SpamCheckPhase phase in new[] { SpamCheckPhase.BeforeTheBody, SpamCheckPhase.AfterTheBody })
         {
            // The phase as an overline: small, strong and secondary, under the
            // card's title, where it was the card title's own size and weight and
            // the three read as one level (review finding F264).
            var phaseHeading = new TextBlock
            {
               Text = phase == SpamCheckPhase.BeforeTheBody
                  ? L("At RCPT TO, before the body is transferred - a refusal here is a 550")
                  : L("After the body has been received - a refusal here is a 554"),
               TextWrapping = TextWrapping.Wrap,
               FontWeight = FontWeights.SemiBold,
               Margin = new Thickness(0, 12, 0, 4)
            };
            phaseHeading.SetResourceReference(StyleProperty, "TextCaption");
            System.Windows.Automation.AutomationProperties.SetHeadingLevel(phaseHeading, System.Windows.Automation.AutomationHeadingLevel.Level3);
            content.Children.Add(phaseHeading);

            var grid = new Grid();
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });                       // order
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });   // name + detail
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });                       // state
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });                       // score
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });                       // link

            // A header row names the columns: the check, its state, what it adds.
            grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            AddColumnHeader(grid, 1, L("Spam check"), TextAlignment.Left);
            AddColumnHeader(grid, 2, L("State"), TextAlignment.Left);
            AddColumnHeader(grid, 3, L("Adds"), TextAlignment.Right);

            int row = 1;
            foreach (SpamCheck check in SpamPipeline.Checks(config).Where(c => c.Phase == phase))
            {
               grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
               AddChecksRow(grid, row, check, config);
               row++;
            }

            content.Children.Add(grid);
         }

         return card;
      }

      private void AddChecksRow(Grid grid, int row, SpamCheck check, SpamPipelineConfig config)
      {
         var order = new TextBlock
         {
            Text = check.Order.ToString(),
            Margin = new Thickness(0, 6, 10, 6),
            VerticalAlignment = VerticalAlignment.Top
         };
         order.SetResourceReference(StyleProperty, "TextCaptionTertiary");
         Grid.SetRow(order, row);
         Grid.SetColumn(order, 0);
         grid.Children.Add(order);

         var text = new StackPanel { Margin = new Thickness(0, 6, 12, 6) };

         var name = new TextBlock { Text = check.Name, TextWrapping = TextWrapping.Wrap };
         // A disabled check is dimmed as well as labelled "Off", because the
         // label is what the reader who cannot see the dimming gets.
         name.SetResourceReference(StyleProperty, check.Enabled ? "TextBodyStrong" : "TextSecondary");

         // The whole row as the name of the one element in it that has an
         // automation peer. A TextBlock has one and a Panel does not, so putting it
         // on the surrounding StackPanel - the obvious place - would set a property
         // nothing ever reads. The five columns are otherwise announced as five
         // unrelated fragments and the listener has to reassemble the row.
         // "Not read" for a check whose settings the server did not answer: it
         // is neither on nor off, and a score of 0 would be a finding. A check
         // that is on and can change nothing says so at the Warning level.
         (StatusLevel stateLevel, string state) = SpamPipeline.StateOf(config, check);

         System.Windows.Automation.AutomationProperties.SetName(name,
            F("{0}, {1}, score {2}. {3}", check.Name, state, check.ScoreText, check.Detail));

         text.Children.Add(name);

         var detail = new TextBlock { Text = check.Detail, TextWrapping = TextWrapping.Wrap };
         detail.SetResourceReference(StyleProperty, "TextCaption");
         text.Children.Add(detail);
         Grid.SetRow(text, row);
         Grid.SetColumn(text, 1);
         grid.Children.Add(text);

         // "On"/"Off" as a pill: the word, the level's shape and the level's
         // colour together, with Off drawn in the neutral grey that says nothing.
         var pill = new StatusPill
         {
            Level = stateLevel,
            Text = state,
            Margin = new Thickness(0, 6, 14, 6),
            VerticalAlignment = VerticalAlignment.Top
         };
         Grid.SetRow(pill, row);
         Grid.SetColumn(pill, 2);
         grid.Children.Add(pill);

         var score = new TextBlock
         {
            Text = check.ScoreText,
            Margin = new Thickness(0, 6, 14, 6),
            VerticalAlignment = VerticalAlignment.Top,
            TextAlignment = TextAlignment.Right,
            MinWidth = 90
         };
         score.SetResourceReference(StyleProperty, "TextCaption");
         Grid.SetRow(score, row);
         Grid.SetColumn(score, 3);
         grid.Children.Add(score);

         // The link names the page it opens, where every row's read "Settings…".
         FrameworkElement link = PageLink(check.Page, F("{0} →", L(NavigationMap.TitleOf(check.Page))),
            F("Open {0}, which owns the {1} settings", L(NavigationMap.TitleOf(check.Page)), check.Name));
         link.Margin = new Thickness(0, 4, 0, 4);
         link.VerticalAlignment = VerticalAlignment.Top;
         Grid.SetRow(link, row);
         Grid.SetColumn(link, 4);
         grid.Children.Add(link);
      }

      /// <summary>One caption over a column of the checks' grid.</summary>
      private static void AddColumnHeader(Grid grid, int column, string text, TextAlignment alignment)
      {
         var header = new TextBlock
         {
            Text = text,
            TextAlignment = alignment,
            FontWeight = FontWeights.SemiBold,
            Margin = new Thickness(0, 0, column == 3 ? 14 : 12, 2)
         };
         header.SetResourceReference(StyleProperty, "TextCaptionTertiary");
         Grid.SetRow(header, 0);
         Grid.SetColumn(header, column);
         grid.Children.Add(header);
      }

      private Card GreylistingCard(SpamPipelineConfig config)
      {
         Card card = Section(L("Greylisting"), out StackPanel content);

         content.Children.Add(Paragraph(SpamPipeline.GreylistingSummary(config), Typography.Body));
         content.Children.Add(PageLink("antispam", F("{0} →", L(NavigationMap.TitleOf("antispam"))),
            L("Open Anti-spam settings, which owns the greylisting settings")));

         return card;
      }

      /// <summary>
      /// "Which list do I need?": the eight lists that let a sender through or stop
      /// one, in the order a message meets them, each with when it is read, what it
      /// is matched against and what a match does, its page and its count (review
      /// finding F269). The card was "The lists behind the checks", seven names and
      /// counts; three let a sender through, three stop one and two can do either,
      /// and only one of their pages said how it differed from another. The words
      /// are <see cref="SenderListGuide"/>'s, which says where in the server each
      /// comes from.
      /// </summary>
      private Card ListsCard(SpamPipelineConfig config)
      {
         Card card = Section(L("Which list do I need?"), out StackPanel content);
         content.Children.Add(Paragraph(L("Eight lists decide whether a sender is let through or stopped - two of them can do either - each read at its own moment in a message's arrival and matched against its own part of it. In the order a message meets them:"),
            Typography.Caption));

         // The list's names as wide as the widest of them, so that none is cut, and the
         // three answers sharing what is left.
         var grid = new Grid { Margin = new Thickness(0, 4, 0, 0) };
         grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });                         // the list and its count
         grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });     // when it is read
         grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1.2, GridUnitType.Star) });   // what it matches
         grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1.3, GridUnitType.Star) });   // what a match does

         string listHeader = L("List"), whenHeader = L("When it is read"), matchesHeader = L("What it matches"), doesHeader = L("What a match does");
         grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
         AddGuideHeader_(grid, 0, listHeader);
         AddGuideHeader_(grid, 1, whenHeader);
         AddGuideHeader_(grid, 2, matchesHeader);
         AddGuideHeader_(grid, 3, doesHeader);

         // Narrower than four columns can be read at - a window of 760 pixels, or a
         // language whose names are long - each list is a block of its own: its name
         // and count, then the three answers under their headings.
         var blocks = new StackPanel { Margin = new Thickness(0, 4, 0, 0), Visibility = Visibility.Collapsed };
         Grid.SetIsSharedSizeScope(blocks, true);

         int row = 1;
         foreach (SenderListGuideRow list in SenderListGuide.Rows)
         {
            grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            FrameworkElement name = GuideName_(config, list);
            name.Margin = new Thickness(0, 4, 12, 4);
            Grid.SetRow(name, row);
            grid.Children.Add(name);

            AddGuideCell_(grid, row, 1, whenHeader, L(list.When));
            AddGuideCell_(grid, row, 2, matchesHeader, L(list.Matches));
            AddGuideCell_(grid, row, 3, doesHeader, L(list.Does));
            row++;

            blocks.Children.Add(GuideBlock_(config, list, whenHeader, matchesHeader, doesHeader));
         }

         // The card's own width decides, not the window's: the sidebar open or closed
         // gives the same window a different card.
         var host = new Grid();
         host.Children.Add(grid);
         host.Children.Add(blocks);
         host.SizeChanged += (_, e) =>
         {
            bool narrow = e.NewSize.Width < GuideTableWidth;
            grid.Visibility = narrow ? Visibility.Collapsed : Visibility.Visible;
            blocks.Visibility = narrow ? Visibility.Visible : Visibility.Collapsed;
         };
         content.Children.Add(host);
         return card;
      }

      /// <summary>
      /// A list's name as the link to its page, its count under it: "Active: 2 of 3"
      /// for a list whose entries can be switched off, as its own page says it.
      /// </summary>
      private static FrameworkElement GuideName_(SpamPipelineConfig config, SenderListGuideRow list)
      {
         var name = new StackPanel();
         string title = L(NavigationMap.TitleOf(list.Page));
         FrameworkElement link = PageLink(list.Page, F("{0} →", title), F("Open {0}", title));
         link.HorizontalAlignment = HorizontalAlignment.Left;
         name.Children.Add(link);
         var count = new TextBlock { Text = ListCount_(config, list.Page), Margin = new Thickness(8, 0, 0, 0), TextWrapping = TextWrapping.Wrap };
         count.SetResourceReference(StyleProperty, "TextCaptionTertiary");
         name.Children.Add(count);
         return name;
      }

      /// <summary>One list as a block, for a card too narrow for the table: its name and count, then its three answers.</summary>
      private static FrameworkElement GuideBlock_(SpamPipelineConfig config, SenderListGuideRow list, string whenHeader, string matchesHeader, string doesHeader)
      {
         var block = new StackPanel { Margin = new Thickness(0, 4, 0, 10) };
         block.Children.Add(GuideName_(config, list));

         // The headings as wide as the widest of them in every block (the blocks are
         // the size scope), so that the answers start at one place down the card.
         var answers = new Grid { Margin = new Thickness(8, 4, 0, 0) };
         answers.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto, SharedSizeGroup = "SenderListGuideHeading" });
         answers.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
         AddGuideAnswer_(answers, 0, whenHeader, L(list.When));
         AddGuideAnswer_(answers, 1, matchesHeader, L(list.Matches));
         AddGuideAnswer_(answers, 2, doesHeader, L(list.Does));
         block.Children.Add(answers);
         return block;
      }

      private static void AddGuideAnswer_(Grid grid, int row, string header, string text)
      {
         grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
         var heading = new TextBlock { Text = header, FontWeight = FontWeights.SemiBold, TextWrapping = TextWrapping.Wrap, MaxWidth = 180, Margin = new Thickness(0, 2, 12, 2) };
         heading.SetResourceReference(StyleProperty, "TextCaptionTertiary");
         Grid.SetRow(heading, row);
         grid.Children.Add(heading);

         var cell = new TextBlock { Text = text, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 2, 0, 2) };
         cell.SetResourceReference(StyleProperty, "TextCaption");
         System.Windows.Automation.AutomationProperties.SetName(cell, header + ", " + text);
         Grid.SetRow(cell, row);
         Grid.SetColumn(cell, 1);
         grid.Children.Add(cell);
      }

      /// <summary>How many entries a list has, in its own page's words, or Not read.</summary>
      private static string ListCount_(SpamPipelineConfig config, string page)
      {
         switch (page)
         {
            case "dnsbl": // no-loc: page keys
               return Count_(config, nameof(config.ActiveDnsBlackLists), config.ActiveDnsBlackLists, config.TotalDnsBlackLists);
            case "surbl":
               return Count_(config, nameof(config.ActiveSurblServers), config.ActiveSurblServers, config.TotalSurblServers);
            case "spamwhitelist":
               return Count_(config, nameof(config.WhiteListEntries), null, config.WhiteListEntries);
            case "greylistwhitelist":
               return Count_(config, nameof(config.GreylistWhiteListEntries), null, config.GreylistWhiteListEntries);
            case "blockedsenders":
               return Count_(config, nameof(config.BlockedSenderCount), null, config.BlockedSenderCount);
            case "greylistbypass":
               return Count_(config, null, null, config.GreylistBypassEntries);
            case "senderlist":
               return Count_(config, null, null, config.SenderListEntries);
            case "domainpolicylist":
               return Count_(config, null, null, config.DomainPolicyListEntries);
            default:
               return "";
         }
      }

      // The list pages' own words for the count: "Active: 2 of 3" for a list whose
      // entries can be switched off, "3 entries" for one whose cannot; Not read for a
      // count the server did not give, where 0 would be a finding.
      private static string Count_(SpamPipelineConfig config, string property, int? active, int total)
      {
         if (property != null && !config.WasRead(property))
            return L("Not read");
         return active != null
            ? ActiveEntries.PillText(active.Value, total)
            : Plural(total, "{0} entry", "{0} entries");
      }

      private static void AddGuideHeader_(Grid grid, int column, string text)
      {
         var header = new TextBlock { Text = text, FontWeight = FontWeights.SemiBold, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 0, 12, 2) };
         header.SetResourceReference(StyleProperty, "TextCaptionTertiary");
         Grid.SetColumn(header, column);
         grid.Children.Add(header);
      }

      // A cell's accessible name carries its column's header, which a screen reader
      // walking the cells one by one does not otherwise hear.
      private static void AddGuideCell_(Grid grid, int row, int column, string header, string text)
      {
         var cell = new TextBlock { Text = text, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 6, 12, 6) };
         cell.SetResourceReference(StyleProperty, "TextCaption");
         System.Windows.Automation.AutomationProperties.SetName(cell, header + ", " + text);
         Grid.SetRow(cell, row);
         Grid.SetColumn(cell, column);
         grid.Children.Add(cell);
      }

      /// <summary>
      /// "Check a sender": an address, a connecting IP address or both, and what
      /// each list does with them, by the rules the server matches them by
      /// (<see cref="SenderCheck"/>; review finding F269 - nothing said whether a
      /// sender was on any list). The lists are read from the server when Check is
      /// pressed, each on its own, so a list that cannot be read says so and the
      /// others still answer.
      /// </summary>
      private Card CheckCard_()
      {
         if (checkCard_ != null)
         {
            checkResults_.Children.Clear();
            checkNotice_.Visibility = Visibility.Collapsed;
            return checkCard_;
         }

         checkCard_ = Section(L("Check a sender"), out StackPanel content);
         content.Children.Add(Paragraph(L("What each list does with a sender, by the address it gives at MAIL FROM, the address it connects from, or both."), Typography.Caption));
         // What the check cannot know, said where it is asked (release review finding
         // CP-6): the server runs no spam check, no greylisting and no domain policy for
         // a session that signed in (GetDoSpamProtection_, CheckDomainPolicy_), and the
         // allow and block list, read at delivery, still applies to its mail.
         content.Children.Add(Paragraph(L("A sender that signs in (SMTP AUTH) meets none of these lists but the allow and block list, at delivery: the server runs no spam check and does not greylist a session that has signed in."), Typography.Caption));

         var form = new WrapPanel { Orientation = Orientation.Horizontal };
         form.Children.Add(new FieldRow { Label = L("Sender _address"), Content = checkAddress_, Margin = new Thickness(0, 0, 12, 8) });
         form.Children.Add(new FieldRow { Label = L("Connecting _IP address"), Content = checkIp_, Margin = new Thickness(0, 0, 12, 8) });
         checkButton_.VerticalAlignment = VerticalAlignment.Bottom;
         checkButton_.Margin = new Thickness(0, 0, 0, 8);
         System.Windows.Automation.AutomationProperties.SetAutomationId(checkButton_, "spam-overview-check-sender");
         System.Windows.Automation.AutomationProperties.SetAutomationId(checkAddress_, "spam-overview-check-address");
         System.Windows.Automation.AutomationProperties.SetAutomationId(checkIp_, "spam-overview-check-ip");
         checkButton_.Click += (s, e) => RunCheck_();
         form.Children.Add(checkButton_);
         content.Children.Add(form);
         // Enter in either box checks; Escape empties them.
         FormSubmit.Bind(form, checkButton_, checkAddress_, checkIp_);

         content.Children.Add(checkNotice_);
         Grid.SetIsSharedSizeScope(checkResults_, true);
         content.Children.Add(checkResults_);
         return checkCard_;
      }

      private void RunCheck_()
      {
         string address = checkAddress_.Text.Trim();
         string ip = checkIp_.Text.Trim();
         checkResults_.Children.Clear();
         checkNotice_.Visibility = Visibility.Collapsed;

         if (address.Length == 0 && ip.Length == 0)
         {
            SayCheck_(StatusLevel.Information, L("Type the sender's address, the IP address it connects from, or both."));
            return;
         }

         if (ip.Length > 0 && !System.Net.IPAddress.TryParse(ip, out _))
         {
            SayCheck_(StatusLevel.Warning, F("{0} is not an IP address.", ip));
            return;
         }

         dynamic settings = null;
         dynamic antiSpam = null;
         try
         {
            settings = ServerSession.Current.Application.Settings;
            antiSpam = settings.AntiSpam;

            // The connecting address's IP range first, as the server reads it first: a
            // range whose anti-spam is off keeps every list but the allow and block list
            // from being read, and an address no range contains is refused at connect
            // (SenderCheck.ConnectingRange; release review finding CP-6).
            var answers = new List<SenderCheckResult>();
            if (ip.Length > 0 && Range_((object)settings, ip) is SenderCheckResult range)
               answers.Add(range);
            foreach (SenderListGuideRow list in SenderListGuide.Rows)
               answers.Add(Check_((object)antiSpam, list.Page, address, ip));

            // Each list was asked on its own; the rows say what the server does with
            // the sender, where a list it meets first keeps a later one from being read.
            IReadOnlyList<SenderCheckResult> applied = SenderCheck.ApplyPrecedence(answers);
            foreach (SenderCheckResult answer in applied)
               checkResults_.Children.Add(CheckRow_(answer));

            // Above the rows, which list decides, at its level: the notice is a polite
            // live region, so a screen reader, whose keyboard stays in the address box
            // after Enter, hears the answer - where the rows alone said nothing to it.
            (StatusLevel level, string summary) = SenderCheck.Summary(applied);
            SayCheck_(level, summary);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            checkResults_.Children.Clear();
            SayCheck_(StatusLevel.Critical, F("Could not read the lists: {0}", ServerSession.DescribeComError(ex)));
         }
         finally
         {
            ServerSession.Release((object)antiSpam);
            ServerSession.Release((object)settings);
         }
      }

      private void SayCheck_(StatusLevel level, string text)
      {
         checkNotice_.Level = level;
         checkNotice_.Text = text;
         checkNotice_.Visibility = Visibility.Visible;
      }

      /// <summary>
      /// What the IP range the server gives a connection from <paramref name="ip"/> does to
      /// the lists, read from its ranges as the IP ranges page reads them; null where it
      /// does nothing to them, and the reason where the ranges cannot be read.
      /// </summary>
      private static SenderCheckResult Range_(object source, string ip)
      {
         dynamic settings = source;
         try
         {
            return SenderCheck.ConnectingRange(ReadItems_((object)settings.SecurityRanges, (Func<dynamic, SenderCheck.IpRange>)(item =>
               new SenderCheck.IpRange((string)item.Name, (string)item.LowerIP, (string)item.UpperIP, (int)item.Priority, (bool)item.EnableSpamProtection))), ip);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            return SenderCheck.NotRead(SenderCheck.RangesPage, ServerSession.DescribeComError(ex));
         }
      }

      /// <summary>One list's answer, read from the server; a list that cannot be read says why.</summary>
      private static SenderCheckResult Check_(object source, string page, string address, string ip)
      {
         dynamic antiSpam = source;
         try
         {
            switch (page)
            {
               case "spamwhitelist": // no-loc: page keys
                  return SenderCheck.SpamExemptions(ReadItems_((object)antiSpam.WhiteListAddresses, (Func<dynamic, SenderCheck.SpamExemption>)(item =>
                     new SenderCheck.SpamExemption((string)item.LowerIPAddress, (string)item.UpperIPAddress, (string)item.EmailAddress, (string)item.Description))), address, ip);
               case "domainpolicylist":
                  return SenderCheck.DomainPolicy((string)antiSpam.DomainPolicyList, address);
               case "blockedsenders":
                  return SenderCheck.BlockedSenders(ReadItems_((object)antiSpam.BlockedSenders, (Func<dynamic, SenderCheck.BlockedSender>)(item =>
                     new SenderCheck.BlockedSender((string)item.Address, (int)item.Score))), address);
               case "greylistwhitelist":
                  return SenderCheck.GreylistingExemptions(ReadItems_((object)antiSpam.GreyListingWhiteAddresses, (Func<dynamic, string>)(item => (string)item.IPAddress)), ip);
               case "greylistbypass":
                  return SenderCheck.GreylistingBypass(ReadItems_((object)antiSpam.GreyListingBypassEntries, (Func<dynamic, SenderCheck.BypassEntry>)(item =>
                     new SenderCheck.BypassEntry((string)item.Pattern, (int)item.MatchOn, (bool)item.RequireSPF, (bool)item.RequireDKIM, (bool)item.RequireDMARC))), address);
               case "senderlist":
                  return SenderCheck.SenderList(ReadItems_((object)antiSpam.SenderList, (Func<dynamic, SenderCheck.SenderListEntry>)(item =>
                     new SenderCheck.SenderListEntry((string)item.Value, (bool)item.Blocked, (bool)item.Expires, ExpiresAt_(item), (string)item.Reason))), address, DateTime.Now);
               default:
                  // The DNS block lists and SURBL are asked of the network as mail arrives.
                  return SenderCheck.AskedOfTheNetwork(page);
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            return SenderCheck.NotRead(page, ServerSession.DescribeComError(ex));
         }
      }

      // An expiry that is not a date is taken for one that has passed, as the server takes it.
      private static DateTime ExpiresAt_(dynamic item)
      {
         try
         {
            object value = item.ExpiresTime;
            return value is DateTime date ? date : DateTime.MinValue;
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            return DateTime.MinValue;
         }
      }

      /// <summary>Every item of a COM collection, read through <paramref name="read"/>, each released.</summary>
      private static List<T> ReadItems_<T>(object source, Func<dynamic, T> read)
      {
         dynamic collection = source;
         var items = new List<T>();
         try
         {
            int total = (int)collection.Count;
            for (int i = 0; i < total; i++)
            {
               dynamic item = collection.Item[i];
               try
               {
                  items.Add(read(item));
               }
               finally
               {
                  ServerSession.Release((object)item);
               }
            }
         }
         finally
         {
            ServerSession.Release((object)collection);
         }

         return items;
      }

      /// <summary>One list's answer as a row: a pill saying what the list does, the list's page, and the sentence.</summary>
      private static FrameworkElement CheckRow_(SenderCheckResult result)
      {
         (StatusLevel level, string word) = SenderCheck.Pill(result);

         // The pills as wide as the widest of them, in every row (the results panel is
         // the size scope), and the list's page over its sentence beside them, so that
         // neither a pill's word nor a page's name is cut at any width or in any language.
         var row = new Grid { Margin = new Thickness(0, 2, 0, 6) };
         row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto, SharedSizeGroup = "SenderCheckPill" });
         row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });

         var pill = new StatusPill { Level = level, Text = word, Margin = new Thickness(0, 4, 12, 4), VerticalAlignment = VerticalAlignment.Top, HorizontalAlignment = HorizontalAlignment.Left };
         row.Children.Add(pill);

         var answer = new StackPanel();
         string title = L(NavigationMap.TitleOf(result.Page));
         FrameworkElement link = PageLink(result.Page, F("{0} →", title), F("Open {0}", title));
         link.HorizontalAlignment = HorizontalAlignment.Left;
         answer.Children.Add(link);

         var text = new TextBlock { Text = result.Text, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(8, 0, 0, 0) };
         text.SetResourceReference(StyleProperty, "TextBody");
         System.Windows.Automation.AutomationProperties.SetName(text, word + ", " + title + ", " + result.Text);
         answer.Children.Add(text);
         Grid.SetColumn(answer, 1);
         row.Children.Add(answer);
         return row;
      }

      private static Card NotesCard(IReadOnlyList<SpamPipelineNote> notes)
      {
         Card card = Section(L("Worth knowing about this configuration"), out StackPanel content);

         // One notice per note, at the note's own level: the component draws the
         // colour, the shape and the severity word, and puts the word first in
         // its accessible name, which is what this card used to build by hand.
         foreach (SpamPipelineNote note in notes)
            content.Children.Add(new InlineNotice { Level = note.Level, Text = note.Text });

         return card;
      }

      private static TextBlock Paragraph(string text, double size)
      {
         var block = new TextBlock { Text = text, Margin = new Thickness(0, 0, 0, 6) };
         block.SetResourceReference(StyleProperty, size <= Typography.Caption ? "TextCaption" : "TextBody");
         return block;
      }

      /// <summary>
      /// A button that opens the page owning a setting.
      ///
      /// The whole page is read-only, so these links are the only way out of it,
      /// and they are the reason it can stay read-only: every value shown here is
      /// one click from the single place that edits it.
      /// </summary>
      private static FrameworkElement PageLink(string page, string caption, string accessibleName)
      {
         var button = new Wpf.Ui.Controls.Button
         {
            Content = caption,
            Appearance = Wpf.Ui.Controls.ControlAppearance.Transparent,
            VerticalAlignment = VerticalAlignment.Center,
            ToolTip = accessibleName
         };
         ButtonStyles.CompactPageLink(button);
         ControlHelp.NameLink(button, caption, accessibleName);
         System.Windows.Automation.AutomationProperties.SetAutomationId(button, "spam-overview-open-" + page);
         button.Click += (s, e) => (Application.Current?.MainWindow as MainWindow)?.NavigateTo(page);
         return button;
      }
   }
}
