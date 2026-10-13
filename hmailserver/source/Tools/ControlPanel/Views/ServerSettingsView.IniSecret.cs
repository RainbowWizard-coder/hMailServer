// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using hMailServer.ControlPanel.Services;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   public partial class ServerSettingsView
   {
      /// <summary>
      /// A secret kept in the settings store under a [Settings] key - the metrics
      /// listener's bearer token and HTTP Basic password, which came here with the
      /// Monitoring &amp; alerts page from the INI-only view (review finding F273) -
      /// written as that view writes one: the value is never shown, the box says
      /// whether one is stored, a blank box keeps it, a box under it removes it with
      /// the save, and Generate fills in a strong random value where the secret's
      /// only requirement is randomness. Ticking the removal empties the box and
      /// holds it, so that a typed value and a removal cannot both be asked of one
      /// save. A password row of the page, never one the environment withholds
      /// (ComSetting.IsWithheld): an environment that sets it shows the row
      /// disabled, with its mark.
      /// </summary>
      private sealed class IniSecret : ComPassword, IIniSetting
      {
         public IniFeatureStore IniStore;

         /// <summary>Adds Generate, which fills the box with <see cref="PasswordGenerator"/>'s value.</summary>
         public bool OfferGenerate;

         /// <summary>What the empty box says while no secret is stored, where "Enter a secret" would say less.</summary>
         public string Hint;

         private PasswordField box_;
         private CheckBox remove_;
         private bool hasStored_;

         public override bool WantsInitialValue => false;

         // The box's own words, since a password box shows no value: whether a secret
         // is stored, which a blank box keeps, and nothing while the removal is ticked.
         private string Placeholder_()
            => remove_?.IsChecked == true ? ""
               : hasStored_ ? L("A secret is configured - leave blank to keep it")
               : string.IsNullOrEmpty(Hint) ? L("Enter a secret")
               : Hint;

         protected override FrameworkElement BuildEditor(object value)
         {
            // Asked of the server when there is a session, and of the file only when
            // there is none (IniFeatureStore.Read): a remote Control Panel knows
            // whether a secret is stored as well as one on the server does.
            hasStored_ = IniStore != null && IniStore.SettingsReachable && !string.IsNullOrEmpty(IniStore.Read(Path, "").Trim());

            box_ = new PasswordField { FontSize = Typography.Body, HorizontalAlignment = HorizontalAlignment.Stretch };
            Describe(box_, Path);
            Watch(box_);

            var line = new Grid();
            line.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            line.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            Grid.SetColumn(box_, 0);
            line.Children.Add(box_);

            if (OfferGenerate)
            {
               var generate = new Wpf.Ui.Controls.Button   // per setting: no access key, a card holds two
               {
                  Content = L("Generate"),
                  Margin = new Thickness(DesignTokens.Space.Sm, 0, 0, 0),
                  VerticalAlignment = VerticalAlignment.Bottom,
                  ToolTip = L("Fill in a strong random secret")
               };
               SetAid(generate, Path + "Generate"); // no-loc
               // The same word on every secret that offers it, so the name says which.
               AutomationProperties.SetName(generate, F("Generate a random value for {0}", AccessibleName ?? Label ?? L("this secret")));
               generate.Click += (s, e) => box_.Password = PasswordGenerator.Generate(32);
               Grid.SetColumn(generate, 1);
               line.Children.Add(generate);
            }

            remove_ = new CheckBox   // per setting: no access key, the rows are reached with the arrow keys
            {
               Content = L("Remove the stored secret when saving"),
               Margin = new Thickness(0, DesignTokens.Space.Sm, 0, 0),
               FontSize = Typography.Body,
               Visibility = hasStored_ ? Visibility.Visible : Visibility.Collapsed
            };
            SetAid(remove_, Path + "Remove"); // no-loc
            AutomationProperties.SetName(remove_, F("Remove the stored secret when saving - {0}", AccessibleName ?? Label ?? L("this secret")));
            remove_.Checked += (s, e) =>
            {
               box_.Password = "";
               box_.IsEnabled = false;
               ShowWhatABlankBoxDoes_();
            };
            remove_.Unchecked += (s, e) =>
            {
               box_.IsEnabled = true;
               ShowWhatABlankBoxDoes_();
            };
            Watch(remove_);

            ShowWhatABlankBoxDoes_();

            var panel = new StackPanel();
            panel.Children.Add(line);
            panel.Children.Add(remove_);
            return panel;
         }

         /// <summary>
         /// The placeholder, and the box's help text, which a screen reader reads and
         /// which carries what the placeholder says - a password box's placeholder is
         /// not part of its name - before the row's note.
         /// </summary>
         private void ShowWhatABlankBoxDoes_()
         {
            box_.PlaceholderText = Placeholder_();
            AutomationProperties.SetHelpText(box_, Sentences(Placeholder_(), Blurb));
         }

         /// <summary>
         /// The secret is set, for the page's warnings: stored and not to be removed,
         /// or typed now.
         /// </summary>
         public bool IsConfigured
            => box_ == null ? hasStored_
               : remove_?.IsChecked == true ? false
               : !string.IsNullOrEmpty(box_.Password) || hasStored_;

         /// <summary>What the save is asked to do: remove the secret, store what was typed, or nothing.</summary>
         public override object ReadEditor()
            => remove_?.IsChecked == true ? RemoveMark : box_?.Password ?? "";

         // A value no secret is: the removal, as the page's change tracking sees it.
         private const string RemoveMark = "\u0000remove";

         /// <summary>The box and the removal, for the page's warnings to follow (WireMetricsWarnings_).</summary>
         public PasswordField Box => box_;

         public CheckBox RemoveBox => remove_;

         public bool SaveToIni()
         {
            if (IniStore == null || box_ == null)
               return false;

            if (remove_?.IsChecked == true)
            {
               IniStore.Write(Path, "");
               hasStored_ = false;
               remove_.IsChecked = false;
               remove_.Visibility = Visibility.Collapsed;
               ShowWhatABlankBoxDoes_();
               return true;
            }

            string typed = box_.Password;
            if (string.IsNullOrEmpty(typed))
               return false;   // blank keeps the stored secret

            IniStore.Write(Path, typed);
            hasStored_ = true;
            remove_.Visibility = Visibility.Visible;
            ShowWhatABlankBoxDoes_();
            return true;
         }
      }
   }
}
