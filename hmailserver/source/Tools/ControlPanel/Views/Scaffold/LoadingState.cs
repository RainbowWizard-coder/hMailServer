// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Windows;
using System.Windows.Automation;
using System.Windows.Automation.Peers;
using hMailServer.ControlPanel.Services;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views.Scaffold
{
   /// <summary>
   /// What a page shows while its data is on its way: an indeterminate ring
   /// and a line of text, centred in the space the data will take. The text is
   /// a polite live region, so a screen reader hears that a wait has started
   /// without being interrupted by it - the component raises the live-region
   /// event itself (<see cref="Announce"/>) as it is shown and as its text
   /// changes, since the setting alone announces nothing. Swap it for the
   /// content, or for an <see cref="EmptyState"/>, when the load finishes.
   /// </summary>
   public class LoadingState : ScaffoldControl
   {
      static LoadingState()
      {
         DefaultStyleKeyProperty.OverrideMetadata(typeof(LoadingState), new FrameworkPropertyMetadata(typeof(LoadingState)));
      }

      public static readonly DependencyProperty TextProperty = DependencyProperty.Register(
         nameof(Text), typeof(string), typeof(LoadingState), new PropertyMetadata(null, OnTextChanged, NullWhenEmpty));

      public LoadingState()
      {
         // A current value rather than a metadata default, so that the catalogue
         // is consulted at construction - after the language is chosen - and a
         // text set in XAML or code still wins over it.
         SetCurrentValue(TextProperty, L("Loading…"));
         AutomationProperties.SetLiveSetting(this, AutomationLiveSetting.Polite);
         IsVisibleChanged += (s, e) =>
         {
            if ((bool)e.NewValue)
               Announce.Request(this);
         };
      }

      /// <summary>What is being waited for. The default says only that something is.</summary>
      public string Text
      {
         get => (string)GetValue(TextProperty);
         set => SetValue(TextProperty, value);
      }

      protected override AutomationControlType ControlType => AutomationControlType.Text;

      private static void OnTextChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
      {
         AutomationProperties.SetName(d, (string)e.NewValue ?? "");
         Announce.Request((UIElement)d);
      }
   }
}
