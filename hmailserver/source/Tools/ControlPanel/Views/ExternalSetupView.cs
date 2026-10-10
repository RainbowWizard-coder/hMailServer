// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Linq;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Documents;
using System.Windows.Shapes;
using hMailServer.ControlPanel.Services;
using Card = hMailServer.ControlPanel.Views.Scaffold.Card;
using InlineNotice = hMailServer.ControlPanel.Views.Scaffold.InlineNotice;
using PageHeader = hMailServer.ControlPanel.Views.Scaffold.PageHeader;
using StatusPill = hMailServer.ControlPanel.Views.Scaffold.StatusPill;

// The Control Panel has its own Typography (the type scale, in Services) and
// System.Windows.Documents declares one too. That import is needed here for Run and
// Inlines, so the reference is aliased to the one meant rather than dropping the import.
using Typography = hMailServer.ControlPanel.Services.Typography;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// One page that answers "what does this server still need done OUTSIDE it".
   ///
   /// Several shipped features are inert until the administrator does something
   /// hMailServer cannot do for them: publish a DNS record, produce a CA bundle,
   /// name the forwarders they trust, register an application with an identity
   /// provider. The server cannot write to anyone's DNS zone, cannot mint
   /// certificates for other people's devices, and cannot know which third
   /// parties the administrator trusts - those calls are the administrator's to
   /// make, on systems the administrator controls. Today nothing collects them
   /// in one place, so a feature can look enabled and quietly do nothing; that
   /// failure shape ("exists but is inert by default") is the one this page
   /// exists to make loud.
   ///
   /// Read-only on purpose, like the spam overview: it is a checklist, not
   /// another editor. Every item links to the page that owns its settings, so
   /// there is still exactly one place to change each value.
   ///
   /// Every item carries one of four states, and the state is always a word in
   /// the text - never only a colour or a shape - so it survives greyscale,
   /// High Contrast and a screen reader alike:
   ///
   ///   Done          - the parts this panel can see are in place.
   ///   Not needed    - the feature is off, so nothing external is required.
   ///   Action needed - something verifiable is missing, and the feature is
   ///                   currently doing less than its settings suggest.
   ///   Cannot tell   - the answer lives somewhere this panel cannot read
   ///                   (the public internet, a remote machine's disk, a
   ///                   non-TXT DNS record, or a database table with no COM
   ///                   property), so the item says exactly what to check and how.
   ///
   /// The checking itself lives in <see cref="ExternalSetupChecks"/> (Services),
   /// shared with the dashboard's "needs attention" summary so the two can never
   /// drift apart and disagree in front of the administrator. This page renders
   /// every item in full; the dashboard card renders the aggregate and the items
   /// that need action, and links here for the rest.
   ///
   /// Configuration is read synchronously from COM and hMailServer.INI, the way
   /// every other page reads it. DNS TXT records (DKIM, MTA-STS discovery) are
   /// then looked up through <see cref="DnsTxtLookup"/> on a background task,
   /// because each lookup can block for the resolver's full timeout, and the
   /// rows update in place when the answers arrive.
   ///
   /// Honesty rule, learned the hard way from fifteen documented overclaims in
   /// this project: nothing on this page asserts a status it did not actually
   /// determine. Where the answer is knowable from COM, the local filesystem or
   /// a TXT lookup it is checked; where it is not, the item says "Cannot tell"
   /// and why, which is itself information the administrator does not have today.
   /// </summary>
   public class ExternalSetupView : UserControl, IPageLifecycle
   {
      private readonly StackPanel body_ = new();

      // The tally, as a notice at the level the tally itself implies: something
      // needing action is a warning, something that cannot be told from here is
      // information, and a clean sheet is good. The counts are still words, so
      // the level is confirmation and never the only carrier.
      private readonly InlineNotice summary_ = new() { Visibility = Visibility.Collapsed };

      // A read that failed is its own notice, above the tally: it says the items
      // below may be incomplete, which is a different statement from the tally,
      // and one the tally depends on.
      private readonly InlineNotice readFailure_ = new()
      {
         Level = StatusLevel.Warning,
         Visibility = Visibility.Collapsed
      };

      // Where the verdicts came from - a provenance footnote under the list,
      // the quietest text on the page.
      private readonly TextBlock status_ = new();

      // The tally's action: the marked next step brought on screen, with the
      // keyboard on its Open button (review finding F016). The summary named the
      // step without leading to it, below four finished cards a returning
      // administrator scrolled past on every visit.
      private readonly Wpf.Ui.Controls.Button goToNext_ = new()
      {
         Content = L("_Go to the next step"),
         Appearance = Wpf.Ui.Controls.ControlAppearance.Secondary
      };

      // The next step's card and the button on it that opens its page, from the
      // last RenderItems; null when nothing needs action.
      private Card nextCard_;
      private FrameworkElement nextLink_;

      // The finished items - Done and Not needed - are one line each, and open to
      // their full text on a click; the ones opened stay open while the page
      // draws them again (the DNS pass draws every item a second time). By title,
      // which names one item.
      private readonly System.Collections.Generic.HashSet<string> opened_ = new(StringComparer.Ordinal);

      public ExternalSetupView()
      {
         var page = new StackPanel { MaxWidth = DesignTokens.OverviewWidth, HorizontalAlignment = HorizontalAlignment.Left };

         var refresh = new Wpf.Ui.Controls.Button { Content = L("_Refresh") };
         ControlHelp.Explain(refresh, L("Re-check the external prerequisites"));
         System.Windows.Automation.AutomationProperties.SetAutomationId(refresh, "external-setup-refresh");
         refresh.Click += (s, e) => Reload();

         page.Children.Add(new PageHeader
         {
            PageKey = "externalsetup",
            Subtitle = L("What this server still needs, in the order a new installation meets it. Each item says whether it is done, the next one to do is marked, and the button beside it opens the page that does it. Where the answer cannot be read from here, the item says what to check instead of guessing."),
            Actions = refresh
         });

         System.Windows.Automation.AutomationProperties.SetAutomationId(goToNext_, "external-setup-go-to-next");
         goToNext_.Click += (s, e) => GoToNextStep_();

         // The failure first: a tally counted from values some of which could not
         // be read was drawn above the notice that said so, and that notice spoke
         // of "the items above" while it sat above them (review finding F040).
         // Collapsed while every value was read, it leaves the tally first.
         page.Children.Add(readFailure_);
         page.Children.Add(summary_);
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

      // ---- running the shared checks ---------------------------------------------

      /// <summary>The current run of the shared checklist. Kept because the DNS
      /// pass updates its items in place, and RenderItems reads them again.</summary>
      private ExternalSetupChecks checks_;

      /// <summary>
      /// Bumped by every Reload. A background DNS pass carries the generation it
      /// was started for and applies nothing if another reload has happened since,
      /// so a slow lookup can never overwrite a newer page with stale verdicts.
      /// </summary>
      private int generation_;

      /// <summary>The COM/INI half of the status line, fixed at build time.</summary>
      private string baseStatus_;

      private void Reload()
      {
         int generation = ++generation_;

         checks_ = ExternalSetupChecks.Run();

         baseStatus_ = L("Checked against the server. Everything is re-checked every time this page is opened.");

         // The level from the cause: a lost link is Critical, as the shell's pill
         // calls it, and values a server that answered could not give are a Warning.
         readFailure_.Level = ComErrorText.LevelFor(checks_.LinkLost);
         // In DNS records' words, which say the same of its cards: what follows
         // first, the reason last, where a sentence after it ran on from the
         // runtime's text in brackets that it may end with; and no count, since one
         // read that failed may be the whole of an item.
         readFailure_.Text = checks_.FailedReads == 0
            ? null
            : checks_.FailedReads == 1
               ? F("Some values could not be read, so the cards below may be incomplete. Why: {0}", checks_.FirstError)
               : F("Some values could not be read, so the cards below may be incomplete. The first reason: {0}", checks_.FirstError);
         readFailure_.Visibility = checks_.FailedReads == 0 ? Visibility.Collapsed : Visibility.Visible;

         RenderItems(checks_.DnsLookupCount > 0
            ? Plural(checks_.DnsLookupCount,
               "{0} DNS record is being looked up in the background; the row marked \"checking\" will update.",
               "{0} DNS records are being looked up in the background; the rows marked \"checking\" will update.")
            : "");

         if (checks_.DnsLookupCount == 0)
            return;

         // The lookups run off the UI thread because each one can block for the
         // resolver's full timeout - and the missing-record case, the slowest,
         // is exactly the case this page exists to surface. The COM objects are
         // never touched from here: everything the probes need was copied into
         // plain strings during the build, so nothing apartment-bound crosses a
         // thread. Results only land if this is still the newest reload.
         ExternalSetupChecks checks = checks_;
         Task.Run(() =>
         {
            checks.ResolveDnsLookups();

            Dispatcher.BeginInvoke(new Action(() =>
            {
               if (generation != generation_)
                  return;

               checks.ApplyDnsResults();
               RenderItems(L("The DNS records were checked through the Windows resolver."));
            }));
         });
      }

      // ---- drawing --------------------------------------------------------------

      private void RenderItems(string dnsNote)
      {
         body_.Children.Clear();

         // The next step is the first item, in the checklist's own order, that
         // needs action: the order is the one a new installation meets them in
         // (docs/SetupChecklist.md), so the first open item is the one to do.
         // While values could not be read there is none, on the tally or on a card:
         // the item it would be is the first of those that could be told, not the
         // first that needs doing, and the card kept its Next step pill and the
         // page's primary button after the tally stopped naming it (F040).
         bool partial = checks_.FailedReads > 0;
         SetupItem next = partial ? null : checks_.Items.FirstOrDefault(i => i.State == SetupItemState.ActionNeeded);

         nextCard_ = null;
         nextLink_ = null;
         int number = 0;
         foreach (SetupItem item in checks_.Items)
         {
            bool isNext = ReferenceEquals(item, next);
            Card card = ItemCard(item, ++number, isNext, out FrameworkElement link);
            body_.Children.Add(card);
            if (isNext)
            {
               nextCard_ = card;
               nextLink_ = link;
            }
         }

         // The tally leads to the step it names.
         summary_.Content = next != null ? goToNext_ : null;

         int action = checks_.Items.Count(i => i.State == SetupItemState.ActionNeeded);
         int unknown = checks_.Items.Count(i => i.State == SetupItemState.CannotTell);
         int done = checks_.Items.Count(i => i.State == SetupItemState.Done);
         int unused = checks_.Items.Count(i => i.State == SetupItemState.NotNeeded);

         summary_.Level = action > 0 ? StatusLevel.Warning
            : unknown > 0 ? StatusLevel.Information
            : StatusLevel.Good;
         // Each state in a sentence of its own, in the words of its pill and of the
         // Dashboard's setup card (Services/SetupTally): "4 items need action. 7 items
         // cannot be checked from here. 4 items are not needed.", where one sentence
         // said "4 need action, 7 cannot be told from here, 0 done, 4 not needed"
         // (review finding F034). While values could not be read, the tally says
         // first how it counts an item that depends on one - every item is counted,
         // and an unread value is a Cannot tell finding of its item - and names no
         // next step (above). It said that only what could be read was counted,
         // which the numbers were not.
         summary_.Text = Sentences(partial ? L("An item that depends on a value that could not be read is counted as one that cannot be told, unless what was read already shows that it needs action.") : null,
            SetupTally.Tally(action, unknown, done, unused, next?.Title));
         summary_.Visibility = Visibility.Visible;

         // Two sentences; each is whole in the catalogue.
         status_.Text = Sentences(baseStatus_, dnsNote);
      }

      private Card ItemCard(SetupItem item, int number, bool isNext, out FrameworkElement openButton)
      {
         var card = new Card();
         card.SetResourceReference(MarginProperty, "AppCardGap");
         openButton = null;

         // A finished item - Done or Not needed - is one line: its number, its
         // pill, its title and its Open button, and the rest on a click (review
         // finding F016). Action needed and Cannot tell keep their whole text,
         // which is what the reader came for.
         bool compact = item.State == SetupItemState.Done || item.State == SetupItemState.NotNeeded;

         var content = new StackPanel();

         // Header: the item's number, the state as a pill - colour, shape and
         // word together, so no single channel is load-bearing - then the title,
         // then the button to the page that does it. The next step carries a
         // second pill that says so, and its button is the primary one on the
         // page: the eye goes to it first, which is where it should go.
         var header = new Grid();
         header.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
         header.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
         header.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
         header.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
         // The compact card's details button, between the title and the Open button.
         header.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

         var numberText = new TextBlock
         {
            Text = number.ToString(System.Globalization.CultureInfo.CurrentCulture) + ".",
            MinWidth = 24,
            Margin = new Thickness(0, 1, 4, 0),
            VerticalAlignment = VerticalAlignment.Top
         };
         numberText.SetResourceReference(StyleProperty, "TextBodyStrong");
         header.Children.Add(numberText);

         var pills = new WrapPanel { Orientation = Orientation.Horizontal, VerticalAlignment = VerticalAlignment.Top, Margin = new Thickness(0, 0, 12, 0) };
         pills.Children.Add(new StatusPill
         {
            Level = ExternalSetupChecks.LevelFor(item.State),
            Text = ExternalSetupChecks.StateWord(item.State)
         });
         if (isNext)
         {
            var nextPill = new StatusPill { Level = StatusLevel.Information, Text = L("Next step"), Margin = new Thickness(6, 0, 0, 0) };
            System.Windows.Automation.AutomationProperties.SetAutomationId(nextPill, "external-setup-next");
            pills.Children.Add(nextPill);
         }
         Grid.SetColumn(pills, 1);
         header.Children.Add(pills);

         var title = new TextBlock { Text = item.Title, TextWrapping = TextWrapping.Wrap };
         title.SetResourceReference(StyleProperty, "TextBodyStrong");

         // The whole item, as one utterance, on the one header element that has
         // an automation peer: a listener hears the number, the state, the
         // subject, what it is for and what to do, instead of unrelated fragments.
         System.Windows.Automation.AutomationProperties.SetName(title,
            number.ToString(System.Globalization.CultureInfo.CurrentCulture) + ". "
            + (isNext ? L("Next step") + ". " : "")
            + ExternalSetupChecks.StateWord(item.State) + ": " + item.Title + ". " + Sentences(item.Purpose, item.Action));
         Grid.SetColumn(title, 2);
         header.Children.Add(title);

         if (item.Page != null)
         {
            // The button names the page it opens. It said "Settings…" on every
            // item, which left the reader to guess where each one went.
            string pageTitle = L(NavigationMap.TitleOf(item.Page));
            FrameworkElement link = PageLink(item.Page, item.Tab, F("Open {0}", pageTitle),
               F("Open {0}, which owns the settings for: {1}", pageTitle, L(item.Title)), isNext);
            link.VerticalAlignment = VerticalAlignment.Top;
            Grid.SetColumn(link, 4);
            header.Children.Add(link);
            openButton = link;
         }

         content.Children.Add(header);

         var purpose = new TextBlock { Text = item.Purpose, Margin = new Thickness(0, 8, 0, 0) };
         purpose.SetResourceReference(StyleProperty, "TextCaption");
         content.Children.Add(purpose);

         foreach (SetupFinding finding in item.Findings)
         {
            StatusPresentation findingPresentation = StatusSemantics.For(ExternalSetupChecks.LevelFor(finding.State));

            var row = new Grid { Margin = new Thickness(0, 8, 0, 0) };
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });

            var findingMark = new Path { Width = 9, Height = 9, Stretch = System.Windows.Media.Stretch.Fill, Margin = new Thickness(0, 4, 8, 0), VerticalAlignment = VerticalAlignment.Top };
            ShapeMarkVisuals.ApplyMark(findingMark, findingPresentation.Shape, findingPresentation.BrushKey);
            row.Children.Add(findingMark);

            // The state word is IN the text (a bold run), so it reaches
            // greyscale printouts and screen readers without any extra plumbing.
            var text = new TextBlock
            {
               FontSize = Typography.Label,
               TextWrapping = TextWrapping.Wrap
            };
            text.Inlines.Add(new Run(ExternalSetupChecks.StateWord(finding.State) + " - ") { FontWeight = FontWeights.SemiBold });
            text.Inlines.Add(new Run(finding.Text));
            Grid.SetColumn(text, 1);
            row.Children.Add(text);

            content.Children.Add(row);

            // Where the finding sends the reader to the Control Deck, the button that
            // opens it, under the finding's text (review finding F281).
            if (finding.NamesControlDeck)
            {
               FrameworkElement deck = ControlDeckLink.Row(L("Open the Control Deck"));
               deck.Margin = new Thickness(17, 2, 0, 0);
               content.Children.Add(deck);
            }
         }

         // The action is only worth space when there is (or may be) something to
         // do. "Not needed" earns a quiet card; the instructions would be noise -
         // and on a done item they read as something still to do ("Add the domain
         // on the Domains page" under "Done - Active domains: 1 of 1").
         if (item.State != SetupItemState.NotNeeded && item.State != SetupItemState.Done)
         {
            var action = new TextBlock { Margin = new Thickness(0, 12, 0, 0) };
            action.SetResourceReference(StyleProperty, "TextCaption");
            action.Inlines.Add(new Run(L("What to do: ")) { FontWeight = FontWeights.SemiBold });
            action.Inlines.Add(new Run(item.Action));
            content.Children.Add(action);
         }

         if (compact)
            FoldDetails_(content, header, title, item, number);

         card.Content = content;
         return card;
      }

      /// <summary>
      /// A finished item's card made one line: everything under its header - what
      /// the item is for and what was found - goes into a panel shown on a click,
      /// on the title or on the chevron beside the Open button, which a keyboard
      /// reaches and a screen reader hears as Show the details of the item. Open
      /// stays on the line. The card stays open while the page draws it again.
      /// </summary>
      private void FoldDetails_(StackPanel content, Grid header, TextBlock title, SetupItem item, int number)
      {
         var details = new StackPanel();
         while (content.Children.Count > 1)
         {
            UIElement part = content.Children[1];
            content.Children.RemoveAt(1);
            details.Children.Add(part);
         }
         content.Children.Add(details);

         var chevron = new Wpf.Ui.Controls.SymbolIcon { FontSize = 16 };
         var toggle = new Wpf.Ui.Controls.Button
         {
            Content = chevron,
            Appearance = Wpf.Ui.Controls.ControlAppearance.Transparent,
            Padding = new Thickness(4),
            MinWidth = 24,
            MinHeight = 24,
            Margin = new Thickness(0, 0, 8, 0),
            VerticalAlignment = VerticalAlignment.Top,
            Cursor = System.Windows.Input.Cursors.Hand
         };
         System.Windows.Automation.AutomationProperties.SetAutomationId(toggle,
            "external-setup-details-" + number.ToString(System.Globalization.CultureInfo.InvariantCulture));
         Grid.SetColumn(toggle, 3);
         header.Children.Add(toggle);

         void Show(bool open)
         {
            details.Visibility = open ? Visibility.Visible : Visibility.Collapsed;
            chevron.Symbol = open ? Wpf.Ui.Controls.SymbolRegular.ChevronUp24 : Wpf.Ui.Controls.SymbolRegular.ChevronDown24;
            toggle.ToolTip = open ? L("Hide the details") : L("Show the details");
            System.Windows.Automation.AutomationProperties.SetName(toggle,
               open ? F("Hide the details of {0}", item.Title) : F("Show the details of {0}", item.Title));
         }

         void Toggle()
         {
            bool open = !opened_.Contains(item.Title);
            if (open)
               opened_.Add(item.Title);
            else
               opened_.Remove(item.Title);
            Show(open);
         }

         toggle.Click += (s, e) => Toggle();
         title.Cursor = System.Windows.Input.Cursors.Hand;
         title.MouseLeftButtonUp += (s, e) => Toggle();
         Show(opened_.Contains(item.Title));
      }

      /// <summary>
      /// The tally's Go to the next step: the marked card brought on screen, and
      /// the keyboard on its Open button once the scroll is laid out, so that
      /// Enter opens the page that does it.
      /// </summary>
      private void GoToNextStep_()
      {
         if (nextCard_ == null)
            return;

         nextCard_.BringIntoView(new Rect(0, 0, nextCard_.ActualWidth, nextCard_.ActualHeight));
         FrameworkElement link = nextLink_;
         Dispatcher.BeginInvoke(new Action(() =>
         {
            if (link != null && link.IsVisible)
               System.Windows.Input.Keyboard.Focus(link);
         }), System.Windows.Threading.DispatcherPriority.Loaded);
      }

      /// <summary>
      /// A button that opens the page owning an item's settings. The page is
      /// read-only, so these links are the only way out of it - which is also
      /// what keeps it read-only: every state shown here is one click from the
      /// single place that changes it. It opens the page on the item's tab where
      /// the item names one (SetupItem.Tab, MainWindow.NavigateToTab): the page's
      /// first tab is often not where the settings are - Transport security's
      /// Encryption, for ARC sealing on Signing and TLS reporting on Reports
      /// (review finding F228).
      /// </summary>
      private static FrameworkElement PageLink(string page, string tab, string caption, string accessibleName, bool primary)
      {
         var button = new Wpf.Ui.Controls.Button
         {
            Content = caption,
            Appearance = primary ? Wpf.Ui.Controls.ControlAppearance.Primary : Wpf.Ui.Controls.ControlAppearance.Secondary,
            VerticalAlignment = VerticalAlignment.Center,
            ToolTip = accessibleName
         };
         ButtonStyles.CompactPageLink(button);
         ControlHelp.NameLink(button, caption, accessibleName);
         System.Windows.Automation.AutomationProperties.SetAutomationId(button, "external-setup-open-" + page);
         button.Click += (s, e) => (Application.Current?.MainWindow as MainWindow)?.NavigateToTab(page, tab);
         return button;
      }
   }
}
