// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Linq;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using hMailServer.ControlPanel.Services;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// A section of these pages shown as cards on a page of another kind - a page that
   /// holds the settings of its subject since review finding F273 moved them there:
   /// the Rules page's Rule limits, Incoming relays' front-end proxies. The cards are
   /// this view's, with every row type, read and save it has (COM, the settings store,
   /// the REST API); what is not drawn is the page's frame: no header, no padding, no
   /// gap and no width of its own - the host page has those, and the cards are as wide
   /// as the host's and as far from them as its cards are from each other; no Reload,
   /// the host's Refresh reads the cards again when they hold nothing unsaved; no
   /// scroll viewer of its own, which would take the mouse wheel from the host's; and
   /// its Save, named by the host (<see cref="SaveCaption"/>) so that its access key is
   /// chosen in the host's scope, with the pill beside it, are the last card's own
   /// actions, in its footer under the hairline (<see cref="PlaceEmbeddedFooter_"/>).
   /// The host asks it the page questions - <see cref="IPageLifecycle.HasUnsavedChanges"/>,
   /// SaveChanges, RevealSetting - and Ctrl+S finds its Save by <see cref="PageCommands"/>.
   /// </summary>
   public partial class ServerSettingsView
   {
      private readonly bool embedded_;

      // The Save and the pill, and the card whose footer holds them (PlaceEmbeddedFooter_).
      private StackPanel embeddedFooter_;
      private Scaffold.Card embeddedFooterCard_;

      /// <summary>The section as cards for another page (see the class's remarks).</summary>
      public ServerSettingsView(Section section, bool embedded) : this(section)
      {
         embedded_ = embedded;
         if (!embedded)
            return;

         if (Content is Grid frame)
         {
            frame.Margin = new Thickness(0);
            frame.RowDefinitions[2].Height = GridLength.Auto;

            // The gap above the cards is the host's, as between any two of its cards.
            SettingsTabs.Padding = new Thickness(0);

            Header.Visibility = Visibility.Collapsed;
            Header.Actions = null;
            Header.Status = null;

            // The host's Refresh reads the cards again; a Reload of the cards alone
            // beside it would be a second way to do the same.
            foreach (Wpf.Ui.Controls.Button reload in HeaderActions.Children.OfType<Wpf.Ui.Controls.Button>()
               .Where(button => AutomationProperties.GetAutomationId(button) == "settings-reload").ToList())
               HeaderActions.Children.Remove(reload);

            var footer = new StackPanel { Orientation = Orientation.Horizontal };
            footer.Children.Add(HeaderActions);
            StatePill.Margin = new Thickness(DesignTokens.Space.Sm, 0, 0, 0);
            StatePill.VerticalAlignment = VerticalAlignment.Center;
            footer.Children.Add(StatePill);
            embeddedFooter_ = footer;
         }

         // Its caption is the host's, so Ctrl+S knows it by its command and not by "Save changes".
         PageCommands.SetCommand(SaveButton, PageCommand.Save);
      }

      /// <summary>The embedded section's Save's caption, with its access key, as the host chooses it in its own scope.</summary>
      public string SaveCaption
      {
         get => SaveButton.Content as string;
         set => SaveButton.Content = value;
      }

      /// <summary>Whether the section is shown as cards on another page.</summary>
      public bool IsEmbedded => embedded_;

      /// <summary>
      /// Puts the Save and the pill in the footer of the last card, under its hairline,
      /// where every card of a page has its own actions: under the cards, outside them,
      /// the Save's access key was the page's, and on a page whose cards answer their
      /// own keys (AccessKeyScope.ByCard, the backup page) a caption outside every card
      /// may take no letter any card's caption takes - in English three were left. As
      /// the cards are drawn, and each time they are drawn anew: out of the card the
      /// drawing replaced before into the new one, an element being drawn in one place.
      /// </summary>
      private void PlaceEmbeddedFooter_()
      {
         if (embeddedFooter_ == null)
            return;

         Scaffold.Card last = SettingsTabs.Items.OfType<TabItem>()
            .Select(tab => tab.Content as Panel)
            .Where(panel => panel != null)
            .SelectMany(panel => panel.Children.OfType<Scaffold.Card>())
            .LastOrDefault();
         if (ReferenceEquals(last, embeddedFooterCard_))
            return;

         if (embeddedFooterCard_ != null)
            embeddedFooterCard_.Footer = null;
         embeddedFooterCard_ = last;
         if (last != null)
            last.Footer = embeddedFooter_;
      }

      /// <summary>
      /// Reads the cards again for the host's Refresh, unless they hold an edit that has
      /// not been saved, which a Refresh of the host's list must not throw away.
      /// </summary>
      public void RefreshUnlessEdited()
      {
         if (!HasUnsavedChanges)
            OnEnter();
      }
   }
}
