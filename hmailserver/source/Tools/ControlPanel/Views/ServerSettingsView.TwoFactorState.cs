// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Windows;
using System.Windows.Controls;
using hMailServer.ControlPanel.Services;
using hMailServer.ControlPanel.Views.Scaffold;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   public partial class ServerSettingsView
   {
      /// <summary>
      /// Whether a second factor is on, as the first row of its card on the
      /// Administrative access page's Two-factor tab: a pill (On, at Good; Off, at
      /// the level the card gives it), a sentence saying what that means, and the
      /// button that changes it, worded by the state - Set up while it is off, Turn
      /// off while it is on. Review finding F240: both cards offered "Set up or turn
      /// off …" and said nothing of the state until a dialog had been opened and
      /// closed, and then as a line of green or red under the button, so a factor
      /// legitimately off read as a failure.
      ///
      /// Read every time the tab is built and again after the dialog closes. A
      /// state that cannot be read - the link to the service lost - is said as Not
      /// read, and the button waits for the page's Reload: neither of its words
      /// would be known to be the right one.
      /// </summary>
      private sealed class TwoFactorStateRow : ComSetting, INotPersisted
      {
         /// <summary>Whether the factor is on; may throw when it cannot be read.</summary>
         public Func<bool> ReadOn;

         /// <summary>The pill's level while the factor is off.</summary>
         public StatusLevel OffLevel = StatusLevel.Normal;

         public string OnSentence;
         public string OffSentence;
         public string SetUpCaption;
         public string TurnOffCaption;

         /// <summary>Opens the dialog that sets the factor up or turns it off.</summary>
         public Action Open;

         // The editor last built: made anew by every BuildEditor, as
         // NeverBanNameRows' are, since the page builds its editors again on every
         // visit and every Reload.
         private StatusPill pill_;
         private TextBlock sentence_;
         private Wpf.Ui.Controls.Button button_;

         public override bool WantsInitialValue => false;

         // The card's title names the factor; the pill and the sentence say its
         // state, so a caption above them would repeat the title.
         protected override bool CaptionOnRow => false;

         protected override FrameworkElement BuildEditor(object value)
         {
            pill_ = new StatusPill { VerticalAlignment = VerticalAlignment.Center };
            sentence_ = new TextBlock
            {
               TextWrapping = TextWrapping.Wrap,
               VerticalAlignment = VerticalAlignment.Center,
               Margin = new Thickness(DesignTokens.Space.Sm, 0, 0, 0)
            };
            sentence_.SetResourceReference(FrameworkElement.StyleProperty, "TextBody");

            var state = new Grid();
            state.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            state.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            Grid.SetColumn(sentence_, 1);
            state.Children.Add(pill_);
            state.Children.Add(sentence_);

            button_ = new Wpf.Ui.Controls.Button
            {
               Appearance = Wpf.Ui.Controls.ControlAppearance.Secondary,
               HorizontalAlignment = HorizontalAlignment.Left,
               Margin = new Thickness(0, DesignTokens.Space.Md, 0, 0)
            };
            SetAid(button_, "action-" + Slug(SetUpCaption));
            button_.Click += (_, _) =>
            {
               Open?.Invoke();
               Show_();
            };

            var panel = new StackPanel();
            panel.Children.Add(state);
            panel.Children.Add(button_);
            Show_();
            return panel;
         }

         // Writes nothing: each dialog stores its own change.
         public override object ReadEditor() => null;

         public override void Write(object owner, string property)
         {
         }

         /// <summary>The pill, the sentence and the button's words for the state the factor is in now.</summary>
         private void Show_()
         {
            bool? on;
            try
            {
               on = ReadOn();
            }
            catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
            {
               on = null;
            }

            pill_.Level = on == true ? StatusLevel.Good : on == false ? OffLevel : StatusLevel.Normal;
            pill_.Text = on == true ? L("On") : on == false ? L("Off") : L("Not read");
            sentence_.Text = on == true ? OnSentence
               : on == false ? OffSentence
               : L("Whether it is on could not be read from the server.");
            button_.Content = on == true ? TurnOffCaption : SetUpCaption;
            button_.IsEnabled = on != null;
         }
      }
   }
}
