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
      private readonly PageHeader header_ = new() { Title = L("Welcome") };

      // The offer to be walked through the server. It is a notice rather than a
      // tour that starts itself: a first-run walk that seizes the screen of
      // somebody who already knows the product is the reason first-run walks
      // have a bad name. It is taken off the page once the walk is finished.
      private readonly InlineNotice tourOffer_ = new()
      {
         Level = StatusLevel.Information,
         Visibility = Visibility.Collapsed,
         Margin = new Thickness(0, 0, 0, 16)
      };

      // The sample domain (the server's Common/Application/SampleDomain.h): one
      // button that fills a bare server with something to look at, and one that
      // takes it away again and says what went. It is a notice beside the walk's,
      // and for the same reason: an offer that a reader who knows the product
      // steps past, rather than a step they have to get out of.
      //
      // Four shapes. No sample: the offer. Ours: what is in it and the button that
      // removes it. A domain named example.test that this server did not make:
      // neither button, and the sentence that says why - the refusal the whole
      // feature turns on. A server that could not read its domain table: neither
      // button either, because it has just said it does not know.
      //
      // The password is in none of them. It is minted by the server when the
      // sample is made, shown in the dialog that reports the creation, and never
      // stored, so there is nothing for this notice to read back.
      private readonly InlineNotice sample_ = new()
      {
         Level = StatusLevel.Information,
         Visibility = Visibility.Collapsed,
         Margin = new Thickness(0, 0, 0, 16)
      };

      private readonly Wpf.Ui.Controls.Button sampleAction_ = new()
      {
         Appearance = Wpf.Ui.Controls.ControlAppearance.Secondary
      };

      // What the last read of the server said, so that the button knows which of
      // the two calls it is. Empty until the page has been entered.
      private string sampleState_ = "";
      private bool sampleBusy_;

      public WelcomeView()
      {
         var panel = new StackPanel { MaxWidth = 980, HorizontalAlignment = HorizontalAlignment.Left };

         // "Show me around" in the header's actions as well as in the notice
         // below, because the notice goes away for good once the walk is done
         // and the entry point must not go with it.
         var showMe = new Wpf.Ui.Controls.Button
         {
            Content = L("Sho_w me around"),
            Appearance = Wpf.Ui.Controls.ControlAppearance.Secondary,
            Icon = new Wpf.Ui.Controls.SymbolIcon(Wpf.Ui.Controls.SymbolRegular.QuestionCircle24)
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
         sampleAction_.Click += (s, e) => SampleClicked_();
         sample_.Content = sampleAction_;
         panel.Children.Add(sample_);

         panel.Children.Add(Text(L("Start with what you want to do, browse by area below, or press Ctrl+K to search every page and setting."),
            "TextSecondary", new Thickness(0, 0, 0, 16)));

         // Keyed on intent, in the administrator's words, and short enough to scan:
         // the twelve reasons this application gets opened, with the one that had no
         // route at all - mail that has stalled - first. The list lives in
         // WelcomeIntents so a test can hold every entry to a page that exists.
         var intents = new System.Windows.Controls.Primitives.UniformGrid { Columns = 2 };
         foreach (WelcomeIntent intent in WelcomeIntents.Entries)
            intents.Children.Add(IntentRow(intent));
         panel.Children.Add(new SettingsSection { Heading = L("What do you want to do?"), Content = intents });

         var tiles = new System.Windows.Controls.Primitives.UniformGrid { Columns = 3 };
         tiles.Children.Add(Tile(Wpf.Ui.Controls.SymbolRegular.Globe24, L("Domains & accounts"),
            L("Add domains, accounts, aliases and distribution lists."), "domains"));
         tiles.Children.Add(Tile(Wpf.Ui.Controls.SymbolRegular.Server24, L("Server settings"),
            L("Protocols, delivery, anti-spam, anti-virus and advanced options."), "protocols"));
         tiles.Children.Add(Tile(Wpf.Ui.Controls.SymbolRegular.DataUsage24, L("Dashboard"),
            L("Live processed-mail, spam and virus counters with charts."), "dashboard"));
         tiles.Children.Add(Tile(Wpf.Ui.Controls.SymbolRegular.DocumentText24, L("Live logs"),
            L("Stream the server log in real time."), "logs"));
         tiles.Children.Add(Tile(Wpf.Ui.Controls.SymbolRegular.Key24, L("Transport security"),
            L("DANE, MTA-STS, ARC and TLS reporting."), "security"));
         tiles.Children.Add(Tile(Wpf.Ui.Controls.SymbolRegular.ArrowSync24, L("Backup & restore"),
            L("Back up or restore your configuration and data."), "backup"));
         panel.Children.Add(new SettingsSection { Heading = L("Or browse by area"), Content = tiles, Margin = new Thickness(0) });

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

         var btn = new Wpf.Ui.Controls.Button
         {
            Content = stack,
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
         btn.Click += (s, e) => (Application.Current.MainWindow as MainWindow)?.NavigateTo(intent.Page);
         return btn;
      }

      private static Wpf.Ui.Controls.Button Tile(Wpf.Ui.Controls.SymbolRegular icon, string heading, string subtitle, string navKey)
      {
         // Centred across the WHOLE tile, which needs the stack below to be
         // stretched: with the stack sized to its content, "centre" meant the centre
         // of the widest line of text, so every tile put its icon somewhere
         // different - above the middle of "Domains & accounts" in one and much
         // further left above "Live logs" in another. Six tiles, six positions, none
         // of them chosen. Reported as issue #30.
         var icn = new Wpf.Ui.Controls.SymbolIcon
         {
            Symbol = icon,
            FontSize = 24,
            Margin = new Thickness(0, 0, 0, 12),
            HorizontalAlignment = HorizontalAlignment.Center
         };
         // By key, never by a held brush: the brand brush is republished on every
         // theme change, and under High Contrast it is the system highlight.
         icn.SetResourceReference(ForegroundProperty, "AppBrandBrush");

         var stack = new StackPanel();
         stack.Children.Add(icn);
         stack.Children.Add(Text(heading, "TextBodyStrong", new Thickness(0, 0, 0, 4)));
         stack.Children.Add(Text(subtitle, "TextCaption", new Thickness(0)));

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
            VerticalContentAlignment = VerticalAlignment.Top,
            MinHeight = 128,
            Cursor = System.Windows.Input.Cursors.Hand
         };
         System.Windows.Automation.AutomationProperties.SetName(btn, heading);
         btn.Click += (s, e) => (Application.Current.MainWindow as MainWindow)?.NavigateTo(navKey);
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

         if (sampleState_ == "present")
         {
            sample_.Level = StatusLevel.Good;
            sample_.Text = F("{0} is here with the mailboxes {1}, the list {2} and one rule that files invoices. The one password for all of them was shown when it was made and was not kept - remove the sample and make it again to be given another. It is left out of the first-hour checklist, because a sample domain is not this server's first domain.",
               domain, string.Join(", ", accounts), list);
            sampleAction_.Content = L("Re_move the sample domain");
            sampleAction_.Visibility = Visibility.Visible;
         }
         else if (sampleState_ == "foreign")
         {
            sample_.Level = StatusLevel.Warning;
            sample_.Text = F("A domain named {0} is already on this server, and it is not the sample domain this page makes, so no sample can be made here and this page will not remove it. If you do not want that domain, delete it on the Domains page.", domain);
            sampleAction_.Visibility = Visibility.Collapsed;
         }
         else if (sampleState_ == "unknown")
         {
            // The server could not read its domain table. Offering either button
            // here would be offering to act on an answer it has just said it does
            // not have.
            sample_.Level = StatusLevel.Warning;
            sample_.Text = L("This server could not read its domain table, so it cannot say whether the sample domain is here. Nothing is offered until it can.");
            sampleAction_.Visibility = Visibility.Collapsed;
         }
         else
         {
            sample_.Level = StatusLevel.Information;
            sample_.Text = F("Nothing on this server to look at yet? This makes {0} with three mailboxes, a distribution list, a rule and a handful of delivered messages, so that every page has something in it while you learn. Every mailbox signs in with one password, which this server mints now and shows once, so write it down - and remove the sample before this server takes real mail.", domain);
            sampleAction_.Content = L("Make a sample _domain");
            sampleAction_.Visibility = Visibility.Visible;
         }

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

         bool removing = sampleState_ == "present";
         if (removing && !Dialogs.Confirm(L("Remove the sample domain with every mailbox, message and rule in it? Anything added to it goes too."),
                L("Control Panel"), L("Re_move"), destructive: true))
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
