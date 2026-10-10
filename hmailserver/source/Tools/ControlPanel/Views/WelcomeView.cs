// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Text.Json;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using hMailServer.ControlPanel.Services;
using hMailServer.ControlPanel.Views.Scaffold;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>Landing page shown after connecting.</summary>
   public class WelcomeView : UserControl, IPageLifecycle, IRefreshablePage
   {
      private readonly PageHeader header_ = new() { PageKey = "welcome" };

      // The offer to be walked through the server. It is a notice rather than a
      // tour that starts itself: a first-run walk that seizes the screen of
      // somebody who already knows the product is the reason first-run walks
      // have a bad name. It is taken off the page once the walk is finished.
      // At the Normal level, as the sample domain's offer is: an offer is not a
      // fact worth the Information word and its diamond (review finding F307).
      private readonly InlineNotice tourOffer_ = new()
      {
         Level = StatusLevel.Normal,
         Visibility = Visibility.Collapsed,
         Margin = new Thickness(0, 0, 0, 16)
      };

      // The sample domain (the server's Common/Application/SampleDomain.h): one
      // button that fills a bare server with something to look at, and one that
      // takes it away again and says what went. A card of its own under the
      // intents, titled A sample domain, with its button in the card's footer
      // (review finding F275): it was a second notice above the intents, four
      // lines that began "Nothing on this server to look at yet?" on a server of
      // four domains and eleven accounts, and arrived after the first paint, so
      // that the intents moved under the pointer. The walk's is the one notice
      // above the intents. The card is drawn as the page is - its title, what the
      // sample is and its button, not yet pressable - so that the read only fills
      // it in.
      //
      // Four shapes. No sample: the offer. Ours: what is in it and the button that
      // removes it. A domain named example.test that this server did not make:
      // neither button, and the sentence that says why - the refusal the whole
      // feature turns on. A server that could not read its domain table: neither
      // button either, because it has just said it does not know.
      //
      // The password is in none of them. It is minted by the server when the
      // sample is made, shown in the dialog that reports the creation, and never
      // stored, so there is nothing for this card to read back.
      private readonly Card sample_ = new()
      {
         Title = L("A sample domain"),
         Margin = new Thickness(0, 0, 12, 24)
      };

      // What the read found, at its level: Good for the sample that is here,
      // Warning for a domain of that name that is not the sample and for a
      // domain table that could not be read; nothing while there is no sample.
      private readonly InlineNotice sampleNotice_ = new()
      {
         Visibility = Visibility.Collapsed,
         Margin = new Thickness(0)
      };

      private readonly Wpf.Ui.Controls.Button sampleAction_ = new()
      {
         Appearance = Wpf.Ui.Controls.ControlAppearance.Secondary,
         Content = MakeSampleCaption_(),
         HorizontalAlignment = HorizontalAlignment.Left,
         IsEnabled = false
      };

      // The domain the server makes the sample under (SampleDomain.h), until its
      // read names it.
      private const string SampleDomainName = "example.test"; // no-loc: a domain name

      // What the last read of the server said, so that the button knows which of
      // the two calls it is. Empty until the page has been entered.
      private string sampleState_ = "";
      private bool sampleBusy_;

      // The "Or browse by area" tiles: the groups of the tree the credential signed in
      // sees (Services/WelcomeAreas, review finding F276), where six tiles written
      // into this page named areas the sidebar does not have. Three to a row, each row
      // as tall as its tallest tile: a uniform grid made every row as tall as the
      // tallest tile of all.
      private readonly Grid areas_ = new()
      {
         ColumnDefinitions =
         {
            new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) },
            new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) },
            new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) }
         }
      };

      public WelcomeView()
      {
         var panel = new StackPanel { MaxWidth = DesignTokens.OverviewWidth, HorizontalAlignment = HorizontalAlignment.Left };

         // "Show me around" in the header's actions as well as in the notice
         // below, because the notice goes away for good once the walk is done
         // and the entry point must not go with it. The tour's light bulb, as on
         // the pages a tour visits: the question mark is the help pane's (review
         // finding F024).
         var showMe = new Wpf.Ui.Controls.Button
         {
            Content = L("Sho_w me around"),
            Appearance = Wpf.Ui.Controls.ControlAppearance.Secondary,
            Icon = new Wpf.Ui.Controls.SymbolIcon(Wpf.Ui.Controls.SymbolRegular.Lightbulb24)
         };
         System.Windows.Automation.AutomationProperties.SetAutomationId(showMe, "welcome-show-me");
         showMe.Click += (s, e) => StartWalk_();
         header_.Actions = showMe;

         // The subtitle says which server this is, once connected (OnEnter).
         panel.Children.Add(header_);

         tourOffer_.Text = L("New to hMailServer? A short walk sets up a domain, a mailbox and a listener that offers TLS, and ends on the dashboard.");
         var start = new Wpf.Ui.Controls.Button
         {
            Content = L("_Start the walk"),
            Appearance = Wpf.Ui.Controls.ControlAppearance.Primary
         };
         System.Windows.Automation.AutomationProperties.SetAutomationId(start, "welcome-start-walk");
         start.Click += (s, e) => StartWalk_();
         tourOffer_.Content = start;
         panel.Children.Add(tourOffer_);

         System.Windows.Automation.AutomationProperties.SetAutomationId(sampleAction_, "welcome-sample-domain");
         System.Windows.Automation.AutomationProperties.SetAutomationId(sample_, "welcome-sample-card");
         sampleAction_.Click += (s, e) => SampleClicked_();
         sample_.Description = SampleDescription_(SampleDomainName);
         sample_.Content = sampleNotice_;
         sample_.Footer = sampleAction_;

         panel.Children.Add(Text(L("Start with what you want to do, browse by area below, or press Ctrl+K to search every page and setting."),
            "TextSecondary", new Thickness(0, 0, 0, 16)));

         // Keyed on intent, in the administrator's words, and short enough to scan:
         // the reasons this application gets opened, the setup checklist first and
         // then the one that had no route at all - mail that has stalled. The list
         // lives in WelcomeIntents so a test can hold every entry to a page that
         // exists, and the right number of them.
         // Twelve pixels past the right and the bottom, which the rows' own right and
         // bottom margins take back, so that the right column ends where the notices
         // above it do, and not 12 pixels short of them (review finding F307).
         var intents = new System.Windows.Controls.Primitives.UniformGrid { Columns = 2, Margin = new Thickness(0, 0, -DesignTokens.Space.Md, -DesignTokens.Space.Md) };
         foreach (WelcomeIntent intent in WelcomeIntents.Entries)
            intents.Children.Add(IntentRow(intent));
         panel.Children.Add(new SettingsSection { Heading = L("What do you want to do?"), Content = intents });

         // The sample domain, under the intents, which come first.
         panel.Children.Add(sample_);

         // One tile per group of the sidebar the signed-in credential sees, filled
         // as the page is entered (ShowAreas_). Past the right and the bottom by the
         // tiles' own margins, as the intents are.
         areas_.Margin = new Thickness(0, 0, -DesignTokens.Space.Md, -DesignTokens.Space.Md);
         panel.Children.Add(new SettingsSection { Heading = L("Or browse by area"), Content = areas_, Margin = new Thickness(0) });

         var scroll = new ScrollViewer { Content = panel, VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
         scroll.SetResourceReference(PaddingProperty, "AppPagePadding");
         Content = scroll;
      }

      private static TextBlock Text(string text, string style, Thickness margin)
      {
         var block = new TextBlock { Text = text, Margin = margin };
         block.SetResourceReference(StyleProperty, style);
         return block;
      }

      /// <summary>
      /// One intent as a wide, low button: the task in bold, what the page offers
      /// under it. Left-aligned text, because these are read as a list rather than
      /// glanced at as tiles.
      /// </summary>
      private static Wpf.Ui.Controls.Button IntentRow(WelcomeIntent intent)
      {
         var stack = new StackPanel();
         stack.Children.Add(Text(L(intent.Heading), "TextBodyStrong", new Thickness(0, 0, 0, 4)));
         stack.Children.Add(Text(L(intent.Blurb), "TextCaption", new Thickness(0)));

         // A chevron at the row's end, in the tertiary text: with a heading and a
         // caption alone, nothing at rest said the row could be pressed (review
         // finding F307).
         var row = new Grid();
         row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
         row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
         row.Children.Add(stack);
         var chevron = new Wpf.Ui.Controls.SymbolIcon(Wpf.Ui.Controls.SymbolRegular.ChevronRight20)
         {
            FontSize = 16,
            Margin = new Thickness(DesignTokens.Space.Md, 0, 0, 0),
            VerticalAlignment = VerticalAlignment.Center
         };
         chevron.SetResourceReference(ForegroundProperty, "TextFillColorTertiaryBrush");
         Grid.SetColumn(chevron, 1);
         row.Children.Add(chevron);

         var btn = new Wpf.Ui.Controls.Button
         {
            Content = row,
            Appearance = Wpf.Ui.Controls.ControlAppearance.Secondary,
            Margin = new Thickness(0, 0, 12, 12),
            Padding = new Thickness(16, 12, 16, 12),
            HorizontalAlignment = HorizontalAlignment.Stretch,
            HorizontalContentAlignment = HorizontalAlignment.Stretch,
            // Stretched to the row's height: centred, a two-line tile sat lower
            // than the three-line one beside it and the two columns never lined up.
            VerticalAlignment = VerticalAlignment.Stretch,
            VerticalContentAlignment = VerticalAlignment.Top,
            Cursor = System.Windows.Input.Cursors.Hand
         };
         System.Windows.Automation.AutomationProperties.SetName(btn, L(intent.Heading));
         // The blurb that helps a reader choose, which the name alone lost.
         System.Windows.Automation.AutomationProperties.SetHelpText(btn, L(intent.Blurb));
         btn.Click += (s, e) => (Application.Current.MainWindow as MainWindow)?.NavigateTo(intent.Page);
         return btn;
      }

      /// <summary>
      /// The area tiles for the credential signed in: one per group of the tree it
      /// sees, in the sidebar's order (Services/WelcomeAreas). Built on every entry,
      /// since a sign-in as another role sees another tree.
      /// </summary>
      private void ShowAreas_()
      {
         ServerSession session = ServerSession.Current;
         Func<string, bool> mayUse = session == null || session.Permissions == null ? null : session.May;

         areas_.Children.Clear();
         areas_.RowDefinitions.Clear();

         IReadOnlyList<WelcomeArea> areas = WelcomeAreas.For(mayUse);
         int columns = areas_.ColumnDefinitions.Count;
         for (int i = 0; i < areas.Count; i++)
         {
            if (i % columns == 0)
               areas_.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });

            Wpf.Ui.Controls.Button tile = Tile(areas[i]);
            Grid.SetRow(tile, i / columns);
            Grid.SetColumn(tile, i % columns);
            areas_.Children.Add(tile);
         }
      }

      /// <summary>
      /// One area as a tile: the group's glyph, its title and its purpose, as the
      /// sidebar has them, opening the group's first page - which shows the group in
      /// the sidebar - and saying which page that is in its tool tip and to a screen
      /// reader.
      /// </summary>
      private static Wpf.Ui.Controls.Button Tile(WelcomeArea area)
      {
         string heading = L(area.Title);
         string opens = F("Opens {0}, the first page of this area.", L(area.PageTitle));

         var stack = new StackPanel();

         // Centred across the WHOLE tile, which needs the stack below to be
         // stretched: with the stack sized to its content, "centre" meant the centre
         // of the widest line of text, so every tile put its icon somewhere
         // different - above the middle of "Domains & accounts" in one and much
         // further left above "Live logs" in another. Six tiles, six positions, none
         // of them chosen. Reported as issue #30. An icon name the glyph set does not
         // have draws no glyph, as the sidebar does.
         if (Enum.TryParse(area.Icon, out Wpf.Ui.Controls.SymbolRegular symbol))
         {
            var icn = new Wpf.Ui.Controls.SymbolIcon
            {
               Symbol = symbol,
               FontSize = 24,
               Margin = new Thickness(0, 0, 0, 12),
               HorizontalAlignment = HorizontalAlignment.Center
            };
            // By key, never by a held brush: the brand brush is republished on every
            // theme change, and under High Contrast it is the system highlight.
            icn.SetResourceReference(ForegroundProperty, "AppBrandBrush");
            stack.Children.Add(icn);
         }

         stack.Children.Add(Text(heading, "TextBodyStrong", new Thickness(0, 0, 0, 4)));
         stack.Children.Add(Text(L(area.Purpose), "TextCaption", new Thickness(0)));

         var btn = new Wpf.Ui.Controls.Button
         {
            Content = stack,
            Appearance = Wpf.Ui.Controls.ControlAppearance.Secondary,
            Margin = new Thickness(0, 0, 12, 12),
            Padding = new Thickness(16, 16, 16, 16),
            HorizontalAlignment = HorizontalAlignment.Stretch,
            // Stretch rather than Left: the content stack then spans the tile, which
            // is what lets the icon centre on the TILE and leaves the two text
            // blocks left-aligned exactly where they already were (a TextBlock in a
            // stretched vertical StackPanel still draws its text from the left).
            HorizontalContentAlignment = HorizontalAlignment.Stretch,
            // As tall as the row, which is as tall as its tallest tile.
            VerticalAlignment = VerticalAlignment.Stretch,
            VerticalContentAlignment = VerticalAlignment.Top,
            MinHeight = 128,
            Cursor = System.Windows.Input.Cursors.Hand
         };
         btn.ToolTip = opens;
         System.Windows.Automation.AutomationProperties.SetName(btn, heading);
         System.Windows.Automation.AutomationProperties.SetHelpText(btn, opens);
         btn.Click += (s, e) => (Application.Current.MainWindow as MainWindow)?.NavigateTo(area.Page);
         return btn;
      }

      public void OnEnter()
      {
         try
         {
            header_.Subtitle = F("Connected to hMailServer {0} on {1}.", (string)ServerSession.Current.Application.Version, ServerSession.Current.Host);
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            header_.Subtitle = "";
         }

         var shell = Application.Current.MainWindow as MainWindow;
         tourOffer_.Visibility = shell != null && !shell.HasFinishedFirstRun ? Visibility.Visible : Visibility.Collapsed;

         ShowAreas_();

         LoadSample_();
      }

      void IRefreshablePage.Refresh() => OnEnter();

      private static void StartWalk_()
         => (Application.Current.MainWindow as MainWindow)?.StartTour(TourCatalog.FirstRun);

      // ---- the sample domain ----------------------------------------------------

      /// <summary>
      /// Reads Utilities.GetSampleDomain off the window thread and draws the notice
      /// from it. Off the thread because the call reads the domain table, and this
      /// is the first page the application shows: a server whose database is slow
      /// must not make the Welcome page slow to appear. A read that fails leaves the
      /// notice hidden - the page has plenty else on it, and an error about a
      /// feature nobody has asked for yet is noise.
      /// </summary>
      private void LoadSample_()
      {
         Task.Run(() =>
         {
            string json;
            try
            {
               json = (string)ServerSession.Current.Application.Utilities.GetSampleDomain();
            }
            catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
            {
               json = null;
            }

            Dispatcher.BeginInvoke(new Action(() => ShowSample_(json)));
         });
      }

      /// <summary>The offer's button, before the read lands and after it finds no sample: one caption, one key.</summary>
      private static string MakeSampleCaption_() => L("Make a sample _domain");

      /// <summary>What the sample is, whatever the read finds: the card's description.</summary>
      private static string SampleDescription_(string domain)
         => F("Makes {0} with three mailboxes, a distribution list, a rule and some delivered mail, so that every page has something in it. Remove it before this server takes real mail.", domain);

      private void ShowSample_(string json)
      {
         sampleState_ = "";
         if (string.IsNullOrEmpty(json))
         {
            sample_.Visibility = Visibility.Collapsed;
            return;
         }

         string domain, list;
         var accounts = new List<string>();
         try
         {
            using JsonDocument document = JsonDocument.Parse(json);
            JsonElement root = document.RootElement;
            sampleState_ = root.GetProperty("state").GetString() ?? "";
            domain = root.GetProperty("domain").GetString() ?? "";
            list = root.GetProperty("list").GetString() ?? "";
            foreach (JsonElement address in root.GetProperty("accounts").EnumerateArray())
               accounts.Add(address.GetString() ?? "");
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            sample_.Visibility = Visibility.Collapsed;
            return;
         }

         sample_.Description = SampleDescription_(domain.Length > 0 ? domain : SampleDomainName);
         sampleNotice_.Content = null;
         sampleAction_.IsEnabled = !sampleBusy_;

         if (sampleState_ == "present")
         {
            // The password is said in the dialog that reports the sample's
            // creation, and only there; here, how to be given another.
            sampleNotice_.Show(StatusLevel.Good, F("{0} is here with the mailboxes {1}, the list {2} and a rule that files invoices. Its password was shown once, when it was made; remove the sample and make it again to be given another.",
               domain, string.Join(", ", accounts), list));
            sampleAction_.Content = L("Re_move the sample domain");
            sampleAction_.Visibility = Visibility.Visible;
         }
         else if (sampleState_ == "foreign")
         {
            sampleNotice_.Show(StatusLevel.Warning, F("A domain named {0} is already on this server, and it is not the sample domain this page makes, so no sample can be made here and this page will not remove it. If you do not want that domain, delete it on the Domains page.", domain));
            sampleNotice_.Content = NoticeActions.OpenPage("domains", "welcome-sample-open-domains"); // no-loc: a page key and an id
            sampleAction_.Visibility = Visibility.Collapsed;
         }
         else if (sampleState_ == "unknown")
         {
            // The server could not read its domain table. Offering either button
            // here would be offering to act on an answer it has just said it does
            // not have.
            sampleNotice_.Show(StatusLevel.Warning, L("This server could not read its domain table, so it cannot say whether the sample domain is here. Nothing is offered until it can."));
            sampleAction_.Visibility = Visibility.Collapsed;
         }
         else
         {
            // The offer: what the sample is is the card's description, and the
            // password sentence is the creation dialog's.
            sampleNotice_.Hide();
            sampleAction_.Content = MakeSampleCaption_();
            sampleAction_.Visibility = Visibility.Visible;
         }

         // A card with nothing to offer and nothing to say has no footer.
         sample_.Footer = sampleAction_.Visibility == Visibility.Visible ? sampleAction_ : null;
         sample_.Visibility = Visibility.Visible;
      }

      /// <summary>
      /// Makes the sample or removes it, off the window thread as the read is: the
      /// creation writes a domain, three mailboxes, a list, a rule and eight messages
      /// into the delivery queue, and the removal deletes all of it. The refusals -
      /// example.test already here, or here and not the sample's - come back as the
      /// server's own sentence on a COM error, which is shown rather than replaced
      /// with one of this page's that could drift from it.
      /// </summary>
      private void SampleClicked_()
      {
         if (sampleBusy_)
            return;

         // The question's button is the page's own, which names the act with the verb
         // each catalogue asks it with: under the list's Remove, French, Spanish,
         // Russian and Czech asked their word for delete over their word for remove.
         bool removing = sampleState_ == "present";
         if (removing && !Dialogs.Confirm(L("Remove the sample domain with every mailbox, message and rule in it? Anything added to it goes too."),
                L("Control Panel"), L("Re_move the sample domain"), destructive: true))
            return;

         sampleBusy_ = true;
         sampleAction_.IsEnabled = false;

         Task.Run(() =>
         {
            string json = null;
            string failure = null;
            try
            {
               json = removing
                  ? (string)ServerSession.Current.Application.Utilities.RemoveSampleDomain()
                  : (string)ServerSession.Current.Application.Utilities.CreateSampleDomain();
            }
            catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
            {
               failure = ServerSession.DescribeComError(fatalCheck);
            }

            Dispatcher.BeginInvoke(new Action(() => SampleDone_(removing, json, failure)));
         });
      }

      private void SampleDone_(bool removing, string json, string failure)
      {
         sampleBusy_ = false;
         sampleAction_.IsEnabled = true;

         // The server's refusal, or what went wrong, after a prefix that says which
         // of the two failed: DescribeComError's sentence is written to follow one.
         if (failure != null)
         {
            Dialogs.Warn(removing
                  ? F("Could not remove the sample domain: {0}", failure)
                  : F("Could not make the sample domain: {0}", failure),
               L("Control Panel"));
            LoadSample_();
            return;
         }

         try
         {
            using JsonDocument document = JsonDocument.Parse(json ?? "");
            JsonElement root = document.RootElement;
            string domain = root.GetProperty("domain").GetString() ?? "";

            if (removing)
               // Four counts: each after its noun as a label, which no language has to
               // decline (Loc.Plural chooses one form for one count).
               Dialogs.Info(F("{0} was removed, with everything in it - mailboxes: {1}, distribution lists: {2}, rules: {3}, messages: {4}.",
                  domain, root.GetProperty("accounts").GetInt64(), root.GetProperty("lists").GetInt64(),
                  root.GetProperty("rules").GetInt64(), root.GetProperty("messages").GetInt64()), L("Control Panel"));
            else
               // The password is in this dialog and nowhere else, ever: the server
               // mints it here and keeps only the hash, so this box is the one
               // moment anybody can be told it. TryGetProperty rather than
               // GetProperty, because a server older than this panel answers no
               // password at all, and losing the whole report over that would be
               // worse than an empty slot.
               // Two counts are two sentences, the first placed whole in the second.
               Dialogs.Info(PluralF(root.GetProperty("messages").GetInt64(),
                  "{0} {1} message is on its way into them and arrives in a few seconds. All of the mailboxes sign in with the password {2}, which is shown here once - write it down, because this server did not keep it.",
                  "{0} {1} messages are on their way into them and arrive in a few seconds. All of the mailboxes sign in with the password {2}, which is shown here once - write it down, because this server did not keep it.",
                  PluralF(root.GetProperty("accounts_created").GetInt64(), "{0} was made with {1} mailbox.", "{0} was made with {1} mailboxes.",
                     domain, root.GetProperty("accounts_created").GetInt64()),
                  root.GetProperty("messages").GetInt64(),
                  root.TryGetProperty("password", out JsonElement minted) ? minted.GetString() ?? "" : ""), L("Control Panel"));
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            // The work was done; only the report could not be read.
         }

         LoadSample_();
      }

      public void OnLeave()
      {
      }
   }
}
