// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Diagnostics;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using hMailServer.ControlPanel.Services;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// "Open the Control Deck": a button beside the texts that send the reader there -
   /// Discovery's cases, the alert rules and the webhook subscriptions, the spam-trap
   /// addresses, a compromised account's held messages, the migration jobs, the REST
   /// API's own card, the dialogs' notes on a room's booking policy, a domain's reports
   /// received and webhook subscriptions and a list's members' delivery, and the setup
   /// checklist's sandbox finding - which named the Deck with no button and no address
   /// (review finding F281; the owner's decision of 9 October 2026). It opens the REST
   /// listener's root in the browser, where the Deck is served; while the Deck cannot
   /// be opened from here it is disabled, and says why in its tool tip and to a screen
   /// reader, and beside it in a card, a dialog or the checklist (Services/ControlDeck).
   /// </summary>
   internal static class ControlDeckLink
   {
      /// <summary>
      /// The Deck's address from the server's settings, as the Discovery page and the
      /// REST settings client find the listener; null with the reason when it cannot
      /// be opened from here.
      /// </summary>
      public static Uri Find(out string whyNot) => Find(null, out whyNot);

      /// <summary>
      /// <see cref="Find(out string)"/>, with <paramref name="whyOffHere"/> the reason a
      /// page that holds the REST API's port gives while it is off (null for the
      /// sentence that names the API &amp; monitoring page). The settings saved and not
      /// yet applied are asked of the server, so that the button is not pressable
      /// while hMailServer.ini names an address nothing listens on yet
      /// (ControlDeck.ListenerSettings).
      /// </summary>
      public static Uri Find(string whyOffHere, out string whyNot)
      {
         try
         {
            var ini = new IniFeatureStore();

            // The ACME certificate the listener falls back to when it has none of its
            // own: looked for on this machine's disk when it is the server's.
            bool? acmeCertificate = null;
            if (ServerSession.IsLocalSession)
            {
               string dataFolder = ini.IniPath == null ? "" : ProfileApi.ReadString("Directories", "DataFolder", "", ini.IniPath, 4096); // no-loc
               string acmeFolder = DiscoveryReport.AcmeCertificateFolder(ini.Read("AcmeCertificateDirectory", ""), dataFolder); // no-loc
               acmeCertificate = DiscoveryReport.AcmeCertificateIn(acmeFolder, System.IO.File.Exists);
            }

            return ControlDeck.Address(
               ini.Read("RestApiPort", "0"), ini.Read("RestApiBindAddress", "127.0.0.1"), // no-loc
               ini.Read("RestApiCertificateFile", ""), ini.Read("RestApiPrivateKeyFile", ""), // no-loc
               acmeCertificate, ServerSession.IsLocalSession, ServerSession.Current?.Host,
               IniFeatureStore.PendingNames(), whyOffHere, out whyNot);
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            whyNot = Sentences(L("Where the Control Deck is could not be read."), ServerSession.DescribeComError(fatalCheck));
            return null;
         }
      }

      /// <summary>
      /// The button, read for where the Deck is now (<see cref="Update"/>):
      /// <paramref name="caption"/> is its words - with an Alt key in a page's header,
      /// without one on a card, whose row buttons carry none - and
      /// <paramref name="whyOffHere"/> the reason it gives on a page that holds the
      /// REST API's port while the port is 0, or null (<see cref="Find(string, out string)"/>).
      /// </summary>
      public static Wpf.Ui.Controls.Button Button(string caption, string whyOffHere = null)
      {
         var button = new Wpf.Ui.Controls.Button
         {
            Content = caption,
            Appearance = Wpf.Ui.Controls.ControlAppearance.Secondary,
            Icon = new Wpf.Ui.Controls.SymbolIcon(Wpf.Ui.Controls.SymbolRegular.Open24),
            VerticalAlignment = VerticalAlignment.Center
         };

         // The reason is in the tool tip of the disabled button too, and is its
         // accessible help text: a disabled button that says nothing reads as broken.
         ToolTipService.SetShowOnDisabled(button, true);
         AutomationProperties.SetAutomationId(button, "open-control-deck");
         button.Click += (s, e) => Open_(button.Tag as Uri);
         Update(button, whyOffHere);
         return button;
      }

      /// <summary>
      /// Reads where the Deck is again and sets the button by it: pressable, saying
      /// the address it opens, or disabled, saying why. Returns the reason, or null.
      /// A page that keeps its button calls this as it is entered, since the REST API
      /// may have been turned on or off since.
      /// </summary>
      public static string Update(Wpf.Ui.Controls.Button button, string whyOffHere = null)
      {
         Uri deck = Find(whyOffHere, out string whyNot);
         string help = deck != null ? F("Opens {0} in the web browser.", deck.AbsoluteUri) : whyNot;

         button.Tag = deck;
         button.IsEnabled = deck != null;
         button.ToolTip = help;
         AutomationProperties.SetHelpText(button, help);
         return deck != null ? null : whyNot;
      }

      /// <summary>
      /// The button as a card's row, with the reason beside it while it is disabled,
      /// wrapping in a narrow card. The row is made as its page is built, which is as
      /// the page is entered. <paramref name="whyOffHere"/> is as for <see cref="Button"/>.
      /// </summary>
      public static FrameworkElement Row(string caption, string whyOffHere = null)
      {
         Wpf.Ui.Controls.Button button = Button(caption, whyOffHere);
         button.Margin = new Thickness(0, 4, 8, 0);

         var row = new WrapPanel();
         row.Children.Add(button);

         if (button.Tag == null)
         {
            var reason = new TextBlock
            {
               Text = AutomationProperties.GetHelpText(button),
               TextWrapping = TextWrapping.Wrap,
               MaxWidth = 420,
               Margin = new Thickness(0, 4, 0, 0),
               VerticalAlignment = VerticalAlignment.Center
            };
            reason.SetResourceReference(FrameworkElement.StyleProperty, "TextCaption");
            row.Children.Add(reason);
         }

         return row;
      }

      /// <summary>
      /// The button as a dialog's row, under the note that sends the reader to the
      /// Deck - an account's booking policy, a domain's reports received and webhook
      /// subscriptions, a list's members' delivery - which named it with no button
      /// (found re-checking eeff3c210). Tucked under its note, which keeps a gap below
      /// it, and keeping that gap before what follows.
      /// </summary>
      public static FrameworkElement DialogRow()
      {
         FrameworkElement row = Row(L("Open the Control Deck"));
         row.Margin = new Thickness(0, -DesignTokens.Space.Sm, 0, DesignTokens.Space.Md);
         return row;
      }

      private static void Open_(Uri deck)
      {
         if (deck == null)
            return;

         try
         {
            Process.Start(new ProcessStartInfo(deck.AbsoluteUri) { UseShellExecute = true });
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            Dialogs.Warn(F("Could not open a browser: {0}\n\n{1}", ex.Message, deck.AbsoluteUri), L("Control Panel"));
         }
      }
   }
}
