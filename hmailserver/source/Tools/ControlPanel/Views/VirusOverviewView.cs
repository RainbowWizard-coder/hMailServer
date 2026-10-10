// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Documents;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Shapes;
using System.Windows.Threading;
using hMailServer.ControlPanel.Services;

// The Control Panel has its own Typography (the type scale, in Services) and
// System.Windows.Documents declares one too. That import is needed here for Run and
// Inlines, so the reference is aliased to the one meant rather than dropping the import.
using Typography = hMailServer.ControlPanel.Services.Typography;
using Card = hMailServer.ControlPanel.Views.Scaffold.Card;
using InlineNotice = hMailServer.ControlPanel.Views.Scaffold.InlineNotice;
using LoadingState = hMailServer.ControlPanel.Views.Scaffold.LoadingState;
using PageHeader = hMailServer.ControlPanel.Views.Scaffold.PageHeader;
using StatusPill = hMailServer.ControlPanel.Views.Scaffold.StatusPill;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// One page that answers "will an infected message actually be caught here".
   ///
   /// The anti-virus settings page is a correct editor and cannot answer that,
   /// because the answer is not a setting. It is the interaction of four of them:
   /// which scanners are switched on, whether each has the configuration it needs to
   /// run at all, the size above which messages are quietly skipped, and what the
   /// server then does with a message a scanner has condemned.
   ///
   /// The failure this page exists to make visible is the one the roadmap keeps
   /// finding elsewhere: **a configuration that looks enabled and is inert**. A
   /// scanner switched on with a missing host or a missing executable does not
   /// refuse mail and does not disable itself. `VirusScanner::ScanFile_` reports the
   /// error at Medium and moves on, and when every scanner has errored the verdict
   /// is `NoVirusFound` — indistinguishable, from the message's point of view, from
   /// having been examined and found clean. Nothing in the settings page shows that,
   /// and the only trace is an error log nobody reads until afterwards.
   ///
   /// Read-only on purpose. It is a diagnosis, not a second editor: every row links
   /// to the page that owns the setting, so there is still exactly one place to
   /// change each value — which is also why this page cannot drift out of step.
   ///
   /// The judgement all lives in <see cref="VirusPipeline"/>, which has no WPF and is
   /// tested directly; this file reads the COM settings into a snapshot and draws it.
   /// </summary>
   public class VirusOverviewView : UserControl, IPageLifecycle
   {
      private readonly StackPanel body_ = new();

      // A read that failed is a warning in the flow of the page, not a grey line
      // at the bottom: on a page whose verdicts are the product, an incomplete
      // read has to be seen before the verdicts are believed.
      private readonly InlineNotice readFailure_ = new()
      {
         Level = StatusLevel.Warning,
         Visibility = Visibility.Collapsed
      };

      // Where the values came from - a provenance footnote, the quietest text.
      private readonly TextBlock status_ = new();

      // The page's title, whose status is the count of problems found.
      private readonly PageHeader header_;

      public VirusOverviewView()
      {
         var page = new StackPanel { MaxWidth = 1000, HorizontalAlignment = HorizontalAlignment.Left };

         var refresh = new Wpf.Ui.Controls.Button { Content = L("_Refresh") };
         ControlHelp.Explain(refresh, L("Re-read the anti-virus configuration from the server"));
         System.Windows.Automation.AutomationProperties.SetAutomationId(refresh, "virus-overview-refresh");
         refresh.Click += (s, e) => Reload();

         page.Children.Add(header_ = new PageHeader
         {
            PageKey = "virusoverview",
            Subtitle = L("Which scanners can actually run, what they are asked to look at, and what happens to a message one of them condemns. Nothing on this page can be edited - each section has a button to the page that owns its settings."),
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

      // A count of the fetch accounts still running is of a page that was left.
      public void OnLeave() => StopCounting_();

      // ---- the fetch-account walk, off the UI thread -------------------------

      // Which read the page is drawing: a walk that ends after the page was read
      // again, or left, belongs to a read nobody is looking at.
      private int generation_;

      // The walk running, to stop it when the page is read again or left.
      private CancellationTokenSource walk_;

      // True from a read until its walk has ended: the What gets scanned card says
      // the accounts are being counted, and the title's pill does not say "No
      // problems found" of notes still to come.
      private bool counting_;

      private void StopCounting_()
      {
         generation_++;
         walk_?.Cancel();
         walk_ = null;
         counting_ = false;
      }

      // ---- reading the configuration -----------------------------------------

      private int failedReads_;
      private string firstError_;

      // Whether the anti-virus settings object could be opened at all. When it
      // could not, every value in the snapshot is a default, and a page drawn
      // from defaults says "No message is scanned" of a server it never asked.
      private bool configRead_;

      /// <summary>
      /// Reads the anti-virus settings into a snapshot.
      ///
      /// Every property goes through <see cref="Read{T}"/> so that one value the
      /// server cannot give us - an older build, a COM error - costs that value and
      /// its row rather than the whole page. The count of failures is shown at the
      /// bottom instead of being swallowed, because a page that quietly shows a
      /// blank host for a scanner it could not read would be reporting a critical
      /// fault that is not there.
      /// </summary>
      private VirusPipelineConfig ReadConfig()
      {
         failedReads_ = 0;
         firstError_ = null;
         configRead_ = false;

         var config = new VirusPipelineConfig();
         dynamic settings = null;
         dynamic antiVirus = null;

         try
         {
            settings = ServerSession.Current.Application.Settings;
            antiVirus = settings.AntiVirus;
            configRead_ = true;

            config.ClamAvEnabled = Read(config, nameof(config.ClamAvEnabled), () => (bool)antiVirus.ClamAVEnabled);
            config.ClamAvHost = Read(config, nameof(config.ClamAvHost), () => (string)antiVirus.ClamAVHost) ?? "";
            config.ClamAvPort = Read(config, nameof(config.ClamAvPort), () => (int)antiVirus.ClamAVPort);

            config.ClamWinEnabled = Read(config, nameof(config.ClamWinEnabled), () => (bool)antiVirus.ClamWinEnabled);
            config.ClamWinExecutable = Read(config, nameof(config.ClamWinExecutable), () => (string)antiVirus.ClamWinExecutable) ?? "";
            config.ClamWinDatabaseFolder = Read(config, nameof(config.ClamWinDatabaseFolder), () => (string)antiVirus.ClamWinDBFolder) ?? "";

            config.CustomScannerEnabled = Read(config, nameof(config.CustomScannerEnabled), () => (bool)antiVirus.CustomScannerEnabled);
            config.CustomScannerExecutable = Read(config, nameof(config.CustomScannerExecutable), () => (string)antiVirus.CustomScannerExecutable) ?? "";
            config.CustomScannerVirusReturnValue = Read(config, nameof(config.CustomScannerVirusReturnValue), () => (int)antiVirus.CustomScannerReturnValue);

            config.MaxScanKilobytes = Read(config, nameof(config.MaxScanKilobytes), () => (int)antiVirus.MaximumMessageSize);

            // eAntivirusAction: 0 = delete the message, 1 = strip the attachments.
            config.Action = Read(config, nameof(config.Action), () => (int)antiVirus.Action) == 1
               ? VirusAction.StripAttachments
               : VirusAction.DeleteMessage;

            // AVFailAction is a [Settings] key, not a COM property: read where every
            // INI row of the Anti-virus page reads it (IniFeatureStore - the file on
            // this machine, or the server's copy over COM).
            config.HoldUnscanned = Read(config, nameof(config.HoldUnscanned), () => new IniFeatureStore().Read("AVFailAction", "0").Trim() == "1");

            config.NotifySender = Read(config, nameof(config.NotifySender), () => (bool)antiVirus.NotifySender);
            config.NotifyRecipient = Read(config, nameof(config.NotifyRecipient), () => (bool)antiVirus.NotifyReceiver);

            config.AttachmentBlockingEnabled = Read(config, nameof(config.AttachmentBlockingEnabled), () => (bool)antiVirus.EnableAttachmentBlocking);
            config.BlockedAttachmentPatterns = CountCollection(config, nameof(config.BlockedAttachmentPatterns), () => antiVirus.BlockedAttachments);

            // The attachment sandbox's settings are the server's, not the anti-virus
            // object's. QuarantineEnabled is a [Settings] key, read as AVFailAction
            // is, and decides where a malicious verdict sends a message.
            config.SandboxEnabled = Read(config, nameof(config.SandboxEnabled), () => (bool)settings.SandboxEnabled);
            config.SandboxSubmitUrl = Read(config, nameof(config.SandboxSubmitUrl), () => (string)settings.SandboxSubmitUrl) ?? "";
            config.SandboxVerdictUrl = Read(config, nameof(config.SandboxVerdictUrl), () => (string)settings.SandboxVerdictUrl) ?? "";
            config.SandboxTimeoutReturns = Read(config, nameof(config.SandboxTimeoutReturns), () => (int)settings.SandboxTimeoutAction == 1);
            config.QuarantineEnabled = Read(config, nameof(config.QuarantineEnabled), () => new IniFeatureStore().ReadBool("QuarantineEnabled", false));

            // The fetch accounts are counted after the page is drawn, off the UI
            // thread (CountFetchAccounts_).
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            failedReads_++;
            firstError_ ??= ServerSession.DescribeComError(ex);
         }
         finally
         {
            ServerSession.Release((object)antiVirus);
            ServerSession.Release((object)settings);
         }

         return config;
      }

      /// <summary>
      /// One value, recorded against its property when it could not be read, so
      /// that the row it feeds says "Not read" rather than showing the default.
      /// </summary>
      private T Read<T>(VirusPipelineConfig config, string property, Func<T> read)
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

      private int CountCollection(VirusPipelineConfig config, string property, Func<object> open)
      {
         dynamic collection = null;
         try
         {
            collection = open();
            return (int)collection.Count;
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            failedReads_++;
            firstError_ ??= ServerSession.DescribeComError(ex);
            if (property != null)
               config.Unread.Add(property);
            return 0;
         }
         finally
         {
            ServerSession.Release((object)collection);
         }
      }

      /// <summary>
      /// Counts external POP3 fetch accounts, and how many of them have their own
      /// anti-virus switch off.
      ///
      /// Worth the walk across every domain and account, because this is the one
      /// way mail reaches a mailbox with none of the settings on this page applied:
      /// SMTPDeliverer only scans a message whose virus-scan flag is set, and for a
      /// fetched message that flag comes from the fetch account rather than from
      /// anything here.
      ///
      /// Off the UI thread, after the cards are drawn from the quick reads: fetch
      /// accounts hang off individual accounts and there is no server-wide
      /// collection of them, so this is one COM round trip per account, and the
      /// page was read whole on the UI thread - a large installation's window
      /// stopped answering until the last account had been read, with nothing on
      /// screen to say why (review finding F265). Bounded still: past the cap the
      /// count is marked incomplete and the note says "at least", because a number
      /// presented as a total when it is a floor is worse than no number. Touches
      /// nothing of the page's: what it found, or the exception it stopped at, is
      /// its result, and the page takes it in on the UI thread.
      /// </summary>
      private const int MaxAccountsWalked = 1000;

      /// <summary>What the walk found: the counts, whether it stopped at the cap, or the failure it stopped at.</summary>
      private readonly record struct FetchAccountCount(int Total, int ScanningOff, bool Incomplete, Exception Failure);

      private static FetchAccountCount CountFetchAccounts_(CancellationToken cancel)
      {
         int accountsWalked = 0, total = 0, scanningOff = 0;
         dynamic domains = null;

         try
         {
            domains = ServerSession.Current.Application.Domains;
            int domainCount = (int)domains.Count;

            for (int d = 0; d < domainCount; d++)
            {
               dynamic domain = null;
               dynamic accounts = null;

               try
               {
                  domain = domains.Item[d];
                  accounts = domain.Accounts;
                  int accountCount = (int)accounts.Count;

                  for (int a = 0; a < accountCount; a++)
                  {
                     // The page was read again or left: what is counted is for nobody.
                     if (cancel.IsCancellationRequested)
                        return new FetchAccountCount(total, scanningOff, true, null);

                     if (accountsWalked >= MaxAccountsWalked)
                        return new FetchAccountCount(total, scanningOff, true, null);

                     accountsWalked++;

                     dynamic account = null;
                     dynamic fetchAccounts = null;

                     try
                     {
                        account = accounts.Item[a];
                        fetchAccounts = account.FetchAccounts;
                        int fetchCount = (int)fetchAccounts.Count;

                        for (int f = 0; f < fetchCount; f++)
                        {
                           dynamic fetchAccount = null;

                           try
                           {
                              fetchAccount = fetchAccounts.Item[f];
                              total++;

                              if (!(bool)fetchAccount.UseAntiVirus)
                                 scanningOff++;
                           }
                           finally
                           {
                              ServerSession.Release((object)fetchAccount);
                           }
                        }
                     }
                     finally
                     {
                        ServerSession.Release((object)fetchAccounts);
                        ServerSession.Release((object)account);
                     }
                  }
               }
               finally
               {
                  ServerSession.Release((object)accounts);
                  ServerSession.Release((object)domain);
               }
            }

            return new FetchAccountCount(total, scanningOff, false, null);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            return new FetchAccountCount(total, scanningOff, false, ex);
         }
         finally
         {
            ServerSession.Release((object)domains);
         }
      }

      /// <summary>
      /// Runs the walk for the read <paramref name="generation"/> drew, and draws the
      /// page again with what it found - or, when it failed, with one failed read
      /// more and no fetch-account note, as the walk on the UI thread did. A walk
      /// that ends after the page was read again or left changes nothing.
      /// </summary>
      private async void CountFetchAccountsThenDraw_(VirusPipelineConfig config, int generation)
      {
         using var cancel = new CancellationTokenSource();
         walk_ = cancel;

         FetchAccountCount count;
         try
         {
            count = await Task.Run(() => CountFetchAccounts_(cancel.Token));
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            count = new FetchAccountCount(0, 0, false, ex);
         }

         if (generation != generation_ || cancel.IsCancellationRequested)
            return;

         walk_ = null;
         counting_ = false;

         if (count.Failure != null)
         {
            // Counted as one failed read rather than several: the page still has
            // everything else, and the fetch-account note simply does not appear.
            failedReads_++;
            firstError_ ??= ServerSession.DescribeComError(count.Failure);
            config.Unread.Add(nameof(config.FetchAccountsWithScanningOff));
         }
         else
         {
            config.FetchAccountsTotal = count.Total;
            config.FetchAccountsWithScanningOff = count.ScanningOff;
            config.FetchAccountScanIncomplete = count.Incomplete;
         }

         Draw_(config);
      }

      // ---- drawing ------------------------------------------------------------

      /// <summary>
      /// Reads the configuration and draws it, then counts the fetch accounts off
      /// the UI thread and draws the page again with what the count found.
      /// </summary>
      private void Reload()
      {
         // A count still running is of the read this one replaces.
         StopCounting_();

         VirusPipelineConfig config = ReadConfig();

         // Nothing could be read: no cards, since every value in them would be a
         // default drawn as a finding, and one Critical notice with Try again
         // (Services/ReadFailure).
         if (!configRead_)
         {
            body_.Children.Clear();
            ReadFailure.Show(readFailure_, null, null, L("The anti-virus configuration could not be read: "), firstError_, Reload);
            header_.Status = null;
            status_.Text = "";
            return;
         }

         counting_ = true;
         Draw_(config);
         CountFetchAccountsThenDraw_(config, generation_);
      }

      /// <summary>
      /// Draws the cards from <paramref name="config"/> - while the fetch accounts
      /// are counted, and again once they are. The keyboard stays where it was: a
      /// control of the cards that had it is found again in the new cards by its
      /// automation id and its place among those that share it, where the count
      /// ending a few seconds after the page opened would take it out of a link
      /// the reader had tabbed to.
      /// </summary>
      private void Draw_(VirusPipelineConfig config)
      {
         (string id, int nth) = FocusedPlace_();

         body_.Children.Clear();
         ReadFailure.Clear(readFailure_);

         // The problems - Warning and above - under the verdict, each with the
         // page that puts it right; the rest at the foot, where all of them were.
         IReadOnlyList<VirusPipelineNote> notes = VirusPipeline.Notes(config);
         List<VirusPipelineNote> problems = OverviewNotices.Problems(notes, n => n.Level);
         List<VirusPipelineNote> worthKnowing = OverviewNotices.WorthKnowing(notes, n => n.Level);

         // "No problems found" only where every value was read: the notes that
         // follow from a value not read are left out, and the count said so in
         // green beside the Warning that says the sections may be incomplete.
         // Nor while the fetch accounts are counted: a fetch account with
         // scanning off is a problem the count has not reached yet.
         header_.Status = OverviewNotices.ProblemsPill(problems.Count, problems.Count == 0 ? StatusLevel.Good : problems[0].Level,
            !counting_ && failedReads_ == 0 && VirusPipeline.VerdictLevel(config) != StatusLevel.Information);

         body_.Children.Add(VerdictCard(config, problems));
         body_.Children.Add(ScannersCard(config));
         body_.Children.Add(WhatIsScannedCard(config));
         body_.Children.Add(InfectedCard(config));
         body_.Children.Add(AttachmentsCard(config));
         body_.Children.Add(SandboxCard(config));

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

         if (id != null)
            Dispatcher.BeginInvoke(DispatcherPriority.Loaded, new Action(() => Refocus_(id, nth)));
      }

      /// <summary>
      /// The automation id of the control in the cards that has the keyboard, and
      /// its place among the cards' controls of that id; no id when the keyboard
      /// is elsewhere.
      /// </summary>
      private (string Id, int Nth) FocusedPlace_()
      {
         if (Keyboard.FocusedElement is not FrameworkElement focused || !body_.IsAncestorOf(focused))
            return (null, 0);

         string id = AutomationProperties.GetAutomationId(focused);
         if (string.IsNullOrEmpty(id))
            return (null, 0);

         List<FrameworkElement> same = WithId_(body_, id);
         return (id, Math.Max(0, same.IndexOf(focused)));
      }

      private void Refocus_(string id, int nth)
      {
         List<FrameworkElement> same = WithId_(body_, id);
         if (same.Count > 0)
            same[Math.Min(nth, same.Count - 1)].Focus();
      }

      /// <summary>The elements under <paramref name="root"/> with automation id <paramref name="id"/>, in the order of the visual tree.</summary>
      private static List<FrameworkElement> WithId_(DependencyObject root, string id)
      {
         var found = new List<FrameworkElement>();
         var stack = new Stack<DependencyObject>();
         stack.Push(root);
         while (stack.Count > 0)
         {
            DependencyObject node = stack.Pop();
            if (node is FrameworkElement element && string.Equals(AutomationProperties.GetAutomationId(element), id, StringComparison.Ordinal))
               found.Add(element);

            int children = VisualTreeHelper.GetChildrenCount(node);
            for (int at = children - 1; at >= 0; at--)
               stack.Push(VisualTreeHelper.GetChild(node, at));
         }

         return found;
      }

      /// <summary>One section of the page. Named Section and not Card because the
      /// component it builds is called that.</summary>
      private static Card Section(string title, out StackPanel content)
      {
         content = new StackPanel();
         var card = new Card { Title = title, Content = content };
         card.SetResourceReference(MarginProperty, "AppCardGap");
         return card;
      }

      /// <summary>
      /// The verdict, then every problem, worst first, each with the page that
      /// puts it right. A verdict that is Good, or that cannot be said, is a
      /// notice at its level; one at the Warning level or above is said by the
      /// notes at its level, and is the card's lead sentence, at no level and
      /// with no action - it still says how many scanners examine a message and
      /// what becomes of an infected one - so that it is not said twice and the
      /// notices at the Warning level and above are the problems the count beside
      /// the title counts.
      /// </summary>
      private static Card VerdictCard(VirusPipelineConfig config, IReadOnlyList<VirusPipelineNote> problems)
      {
         Card card = Section(L("What happens to an infected message"), out StackPanel content);

         if (!VirusPipeline.NotesSayTheVerdict(config))
            content.Children.Add(OverviewNotices.Notice(VirusPipeline.VerdictLevel(config), VirusPipeline.Verdict(config), null, "virus-overview-verdict"));
         else
            content.Children.Add(OverviewNotices.Lead(VirusPipeline.Verdict(config), "virus-overview-verdict"));

         foreach (VirusPipelineNote problem in problems)
            content.Children.Add(OverviewNotices.Notice(problem.Level, problem.Text, problem.Page, "virus-overview-problem"));

         content.Children.Add(Paragraph(VirusPipeline.WhenNoScannerAnswers(config), Typography.Caption));

         return card;
      }

      private Card ScannersCard(VirusPipelineConfig config)
      {
         Card card = Section(L("Scanners, in the order the server tries them"), out StackPanel content);

         var grid = new Grid();
         grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });                       // order
         grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });   // name + detail
         grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });                       // state
         grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });                       // link

         int row = 0;
         foreach (VirusScannerEntry scanner in VirusPipeline.Scanners(config))
         {
            grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            AddScannerRow(grid, row, scanner);
            row++;
         }

         content.Children.Add(grid);

         content.Children.Add(Paragraph(L("The first scanner to find something ends the scan, so the order is also the cost: a slow scanner in front of a fast one is paid for on every clean message."),
            Typography.Caption));

         return card;
      }

      private void AddScannerRow(Grid grid, int row, VirusScannerEntry scanner)
      {
         var order = new TextBlock
         {
            Text = scanner.Order.ToString(),
            Margin = new Thickness(0, 6, 10, 6),
            VerticalAlignment = VerticalAlignment.Top
         };
         order.SetResourceReference(StyleProperty, "TextCaptionTertiary");
         Grid.SetRow(order, row);
         Grid.SetColumn(order, 0);
         grid.Children.Add(order);

         var text = new StackPanel { Margin = new Thickness(0, 6, 12, 6) };

         var name = new TextBlock { Text = scanner.Name, TextWrapping = TextWrapping.Wrap };
         // Dimmed as well as labelled, because the label is what the reader who
         // cannot see the dimming gets. An enabled-but-broken scanner is NOT
         // dimmed - it is the one that most needs the eye drawn to it.
         name.SetResourceReference(StyleProperty, scanner.Enabled ? "TextBodyStrong" : "TextSecondary");

         // The whole row as the accessible name of the one element in it with an
         // automation peer. A TextBlock has one and a Panel does not, so putting it
         // on the surrounding StackPanel - the obvious place - would set a property
         // nothing ever reads, and the columns would be announced as unrelated
         // fragments for the listener to reassemble.
         System.Windows.Automation.AutomationProperties.SetName(name,
            scanner.Name + ", " + scanner.StateText + ". "
            + (scanner.Problem ?? scanner.Detail));

         text.Children.Add(name);

         var detail = new TextBlock { Text = scanner.Problem ?? scanner.Detail, TextWrapping = TextWrapping.Wrap };
         detail.SetResourceReference(StyleProperty, "TextCaption");

         // The problem in the critical level's fill-text shade: the danger
         // colour itself is a mark's, and as words on the dark card it
         // measured 4.1:1.
         if (scanner.Problem != null)
            detail.SetResourceReference(TextBlock.ForegroundProperty, StatusSemantics.For(StatusLevel.Critical).FillTextBrushKey);

         text.Children.Add(detail);
         Grid.SetRow(text, row);
         Grid.SetColumn(text, 1);
         grid.Children.Add(text);

         // The state as a pill: the word, its shape and its colour together, so
         // none of the three is load-bearing alone. "On, but cannot run" is a
         // third state and is deliberately not collapsed into either "On" or
         // "Off": it is the state the settings page cannot show and the whole
         // reason for this one.
         var state = new StatusPill
         {
            // "Not read" is the neutral grey, as Off is: it says nothing, which is
            // exactly what is known.
            Level = scanner.Unread || !scanner.Enabled ? StatusLevel.Normal
               : scanner.Usable ? StatusLevel.Good
               : StatusLevel.Critical,
            Text = scanner.StateText,
            Margin = new Thickness(0, 6, 14, 6),
            VerticalAlignment = VerticalAlignment.Top,
            // Its own word's width at the column's start: the column is as wide as
            // its longest word, "On, but cannot run", which an Off stretched to.
            HorizontalAlignment = HorizontalAlignment.Left
         };

         Grid.SetRow(state, row);
         Grid.SetColumn(state, 2);
         grid.Children.Add(state);

         FrameworkElement link = PageLink(scanner.Page, L("Settings…"),
            F("Open {0}, which owns the {1} settings", L(NavigationMap.TitleOf(scanner.Page)), scanner.Name));
         link.Margin = new Thickness(0, 4, 0, 4);
         link.VerticalAlignment = VerticalAlignment.Top;
         Grid.SetRow(link, row);
         Grid.SetColumn(link, 3);
         grid.Children.Add(link);
      }

      private Card WhatIsScannedCard(VirusPipelineConfig config)
      {
         Card card = Section(L("What gets scanned"), out StackPanel content);

         content.Children.Add(Paragraph(
            !config.WasRead(nameof(config.MaxScanKilobytes))
               ? L("The size limit could not be read.")
               : config.MaxScanKilobytes > 0
               ? F("Messages up to {0}. A larger message is delivered without being scanned — it is not refused, and nothing is logged.", VirusPipeline.Kilobytes(config.MaxScanKilobytes))
               : L("Every message, whatever its size. The size limit is 0, which here means \"no limit\"."),
            Typography.Body));

         content.Children.Add(Paragraph(L("Each message is scanned twice over: once as the whole file on disk, and then once per attachment, each written out to the temporary folder and handed to the scanner on its own. The second pass is what catches an attachment inside a structure the scanner does not decode for itself. If the message cannot be parsed as MIME the per-attachment pass is skipped and an error is logged."),
            Typography.Caption));

         // Counted after the page is drawn: the wait is said where the count goes.
         if (counting_)
         {
            var counting = new LoadingState { Text = L("Counting external accounts…"), Padding = new Thickness(0, 8, 0, 12) };
            AutomationProperties.SetAutomationId(counting, "virus-overview-counting");
            content.Children.Add(counting);
         }
         else if (config.WasRead(nameof(config.FetchAccountsWithScanningOff)) && config.FetchAccountsTotal > 0)
         {
            content.Children.Add(Paragraph(Sentences(PluralF(config.FetchAccountsWithScanningOff,
                  "Mail collected from external POP3 accounts is scanned only when that fetch account says so — {0} of {1} currently has anti-virus switched off. That switch is on the account, not on this page.",
                  "Mail collected from external POP3 accounts is scanned only when that fetch account says so — {0} of {1} currently have anti-virus switched off. That switch is on the account, not on this page.",
                  config.FetchAccountsWithScanningOff, config.FetchAccountsTotal),
               config.FetchAccountScanIncomplete
                  ? F("These are counts of the first {0} accounts only; this page stops there rather than making you wait for the rest.", MaxAccountsWalked)
                  : null),
               Typography.Caption));
         }

         content.Children.Add(PageLink("antivirus", L("Size limit and scanners…"),
            L("Open Anti-virus settings, which owns the maximum message size to scan")));

         return card;
      }

      private static Card InfectedCard(VirusPipelineConfig config)
      {
         Card card = Section(L("When a scanner finds something"), out StackPanel content);

         content.Children.Add(Paragraph(VirusPipeline.ActionSummary(config), Typography.Body));

         return card;
      }

      private Card AttachmentsCard(VirusPipelineConfig config)
      {
         Card card = Section(L("Attachment blocking, which is separate"), out StackPanel content);

         content.Children.Add(Paragraph(
            !config.WasRead(nameof(config.AttachmentBlockingEnabled), nameof(config.BlockedAttachmentPatterns))
               ? L("The attachment blocking settings could not be read.")
               : config.AttachmentBlockingEnabled
               ? Plural(config.BlockedAttachmentPatterns,
                  "{0} pattern is stripped from every message by file name, whatever the scanners say.",
                  "{0} patterns are stripped from every message by file name, whatever the scanners say.")
               : Plural(config.BlockedAttachmentPatterns,
                  "Switched off. {0} pattern is configured and it is not applied.",
                  "Switched off. {0} patterns are configured and none is applied."),
            Typography.Body));

         content.Children.Add(Paragraph(L("This runs before virus scanning and independently of it: it applies even with every scanner switched off, and it applies to messages the size limit above would skip. A matched attachment is replaced by a short text file explaining the removal rather than being deleted outright, so the recipient can tell something was taken out."),
            Typography.Caption));

         content.Children.Add(PageLink("blockedattachments", L("Patterns…"),
            L("Open Blocked attachments, which owns the list of file-name patterns")));

         return card;
      }

      /// <summary>
      /// The attachment sandbox: whether it is on, as the scanners' rows say it,
      /// and what it does to a message - the other thing that holds or quarantines
      /// a message for what it carries, which the page did not mention (review
      /// finding F265). The judgement is VirusPipeline.Sandbox's.
      /// </summary>
      private static Card SandboxCard(VirusPipelineConfig config)
      {
         Card card = Section(L("Attachments opened in a sandbox"), out StackPanel content);
         (StatusLevel level, string state, string sentence) = VirusPipeline.Sandbox(config);

         var grid = new Grid();
         grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });   // what it does
         grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });                       // state
         grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });                       // link

         var text = new TextBlock { Text = sentence, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 6, 12, 6) };
         text.SetResourceReference(StyleProperty, "TextBody");
         AutomationProperties.SetAutomationId(text, "virus-overview-sandbox");
         grid.Children.Add(text);

         var pill = new StatusPill
         {
            Level = level,
            Text = state,
            Margin = new Thickness(0, 6, 14, 6),
            VerticalAlignment = VerticalAlignment.Top
         };
         AutomationProperties.SetAutomationId(pill, "virus-overview-sandbox-state");
         Grid.SetColumn(pill, 1);
         grid.Children.Add(pill);

         // On the tab that holds them, Sandbox, where the page opens on its first.
         FrameworkElement link = PageLink("antivirus", L("Settings…"),
            F("Open {0}, which owns the {1} settings", L(NavigationMap.TitleOf("antivirus")), L("Sandbox")), N("Sandbox"));
         link.Margin = new Thickness(0, 4, 0, 4);
         link.VerticalAlignment = VerticalAlignment.Top;
         Grid.SetColumn(link, 2);
         grid.Children.Add(link);

         content.Children.Add(grid);
         return card;
      }

      private static Card NotesCard(IReadOnlyList<VirusPipelineNote> notes)
      {
         Card card = Section(L("Worth knowing about this configuration"), out StackPanel content);

         // One notice per note, at the note's own level: the component draws the
         // colour, the shape and the severity word, and puts the word first in
         // its accessible name, which is what this card used to build by hand.
         foreach (VirusPipelineNote note in notes)
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
      /// The whole page is read-only, so these links are the only way out of it, and
      /// they are the reason it can stay read-only: every value shown here is one
      /// click from the single place that edits it. With <paramref name="tab"/>, the
      /// English header of the page's tab that holds the settings, it opens that tab
      /// (MainWindow.NavigateToTab).
      /// </summary>
      private static FrameworkElement PageLink(string page, string caption, string accessibleName, string tab = null)
      {
         var button = new Wpf.Ui.Controls.Button
         {
            Content = caption,
            Appearance = Wpf.Ui.Controls.ControlAppearance.Transparent,
            FontSize = Typography.Caption,
            Padding = new Thickness(8, 3, 8, 3),
            VerticalAlignment = VerticalAlignment.Center,
            Cursor = System.Windows.Input.Cursors.Hand,
            ToolTip = accessibleName,
            HorizontalAlignment = HorizontalAlignment.Left
         };
         ControlHelp.NameLink(button, caption, accessibleName);
         System.Windows.Automation.AutomationProperties.SetAutomationId(button,
            "virus-overview-open-" + page + (tab == null ? "" : "-" + tab.ToLowerInvariant()));
         button.Click += (s, e) => (Application.Current?.MainWindow as MainWindow)?.NavigateToTab(page, tab);
         return button;
      }
   }
}
