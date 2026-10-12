// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using hMailServer.ControlPanel.Services;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// One value, three levels (docs/SettingLevels.md), embedded in the domain and
   /// the account dialogs: every setting a domain or an account may hold its own
   /// value of, with what is in effect beside it and where that came from - the
   /// server, the domain or the account - which is the only honest answer to "why
   /// is this account not doing what the domain says".
   ///
   /// A setting is ticked when this level holds a value of its own and unticked
   /// when it takes the level above; Save levels now writes what changed over COM
   /// (Domain.SettingOverride, Account.SettingOverride - an empty value is "take
   /// the level above"), at once and not with the dialog's Save, and reads every
   /// line back. A setting whose level at this dialog is a field of its own - the
   /// domain's maximum message size, its digest hour, the account's retention - is
   /// shown with what is in effect and edited where it always was, on another tab.
   /// Against a server older than schema 6088 the members are unknown, and the tab
   /// says so rather than offering an editor that cannot read back.
   /// </summary>
   public class SettingLevelsPanel : UserControl
   {
      private sealed class Row
      {
         public SettingLevel Level;
         public CheckBox Own;
         public TextBox Value;
         public TextBlock Effect;
         public bool LoadedOwn;
         public string LoadedValue;
      }

      private readonly string domainName_;
      private readonly string address_;
      private readonly bool forAccount_;
      private readonly List<Row> rows_ = new();

      // What the tab has to say, as a notice at its level, where it was one bold
      // text block for everything: a failed read is Critical, with Try again
      // (Services/ReadFailure); the notes about an older server and a dialog that
      // must be saved first are Information; a save says Saved or what was refused.
      private readonly Scaffold.InlineNotice status_ = DialogFields.Notice();
      private readonly Button save_;
      private readonly StackPanel list_ = new();

      /// <param name="forAccount">The account's levels (address names it; empty while it is new), or the domain's.</param>
      public SettingLevelsPanel(string domainName, string address, bool forAccount)
      {
         domainName_ = domainName;
         address_ = address;
         forAccount_ = forAccount;

         // Secondary: the panel is the Levels tab of the account and the domain
         // dialogs, whose Save is the one Primary button (review finding F195).
         save_ = new Wpf.Ui.Controls.Button
         {
            Content = L("Save _levels now"),
            Appearance = Wpf.Ui.Controls.ControlAppearance.Secondary,
            Margin = new Thickness(0, 12, 0, 0),
            MinWidth = 120
         };
         save_.Click += (_, _) => SaveLevels();

         Build();
      }

      private bool ForAccount => forAccount_;

      private void Build()
      {
         var root = new StackPanel { Margin = new Thickness(4, 8, 4, 4) };

         var hint = new TextBlock
         {
            Text = ForAccount
               ? L("A value this account holds of its own wins over its domain's and the server's. Tick a setting to give the account a value of its own, untick it to take its domain's, and save the levels: they are written at once, not with the dialog's Save.")
               : L("A value this domain holds of its own wins over the server's for every account of it that holds none. Tick a setting to give the domain a value of its own, untick it to take the server's, and save the levels: they are written at once, not with the dialog's Save. A setting kept in a field of its own is edited on that field's tab and shown here with what is in effect."),
            TextWrapping = TextWrapping.Wrap,
            Margin = new Thickness(0, 0, 0, 12)
         };
         hint.SetResourceReference(ForegroundProperty, "TextFillColorSecondaryBrush");
         root.Children.Add(hint);

         status_.Margin = new Thickness(0, 0, 0, 12);
         root.Children.Add(status_);
         root.Children.Add(list_);
         root.Children.Add(save_);

         Content = root;
      }

      /// <summary>Reads every setting's own value, what is in effect and where it came from.</summary>
      public void Reload()
      {
         list_.Children.Clear();
         rows_.Clear();

         // A message of the tab's own goes with the read it was about. A failed
         // read stays until a read works, and keeps its Try again button - and the
         // keyboard on it - when the read fails again (ReadFailure).
         if (!ReadFailure.IsShown(status_))
            status_.Hide();

         if (string.IsNullOrEmpty(domainName_) || (ForAccount && string.IsNullOrEmpty(address_)))
         {
            ReadFailure.Clear(status_);
            status_.Show(StatusLevel.Information, L("Save it and reopen it: a level's own value belongs to something that exists."));
            save_.IsEnabled = false;
            return;
         }

         dynamic owner = null;
         try
         {
            owner = OpenOwner();

            foreach (SettingLevel level in SettingLevels.All)
            {
               bool field = ForAccount ? level.AccountField : level.DomainField;

               string own = field ? "" : (string)owner.SettingOverride[level.Name];
               string effective = (string)owner.EffectiveSetting[level.Name];
               string source = (string)owner.EffectiveSettingSource[level.Name];

               list_.Children.Add(MakeRow(level, field, own, effective, source));
            }

            save_.IsEnabled = true;

            // The read worked: a failure it was tried again for goes, and the
            // keyboard, if it was on Try again, goes to Save levels now.
            ReadFailure.Clear(status_, save_);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            list_.Children.Clear();
            rows_.Clear();
            save_.IsEnabled = false;

            // An older server has no levels at all: that is a fact about it, not a
            // failure, and is said as information. Anything else is a read that
            // failed, said at Critical with Try again, where it was bare bold text
            // - "The input string ... was not in a correct format." - with Save
            // levels now greyed and no way to read again.
            if (SettingApplies.IsMissingMember(ex))
            {
               ReadFailure.Clear(status_);
               status_.Show(StatusLevel.Information, L("This server has no setting levels: it is older than schema 6088, and every setting here is the server's or the domain's own field."));
            }
            else
            {
               ReadFailure.Show(status_, null, null, L("Could not read the setting levels: {0}"), ex, Reload, save_);
            }
         }
         finally
         {
            if (owner != null)
               ServerSession.Release((object)owner);
         }
      }

      private FrameworkElement MakeRow(SettingLevel level, bool field, string own, string effective, string source)
      {
         var panel = new StackPanel { Margin = new Thickness(0, 0, 0, 10) };

         var effect = new TextBlock { TextWrapping = TextWrapping.Wrap, Text = SettingLevels.InEffect(effective, source, level.Type) ?? "" };
         effect.SetResourceReference(FrameworkElement.StyleProperty, "TextCaption");

         if (field)
         {
            // The name, from data: a setting's name is the server's word and
            // carries no access key.
            var name = new TextBlock { Text = level.Name, FontFamily = new System.Windows.Media.FontFamily(Typography.MonoFontFamily), FontSize = Typography.Mono };
            panel.Children.Add(name);
            panel.Children.Add(effect);
            var where = new TextBlock { Text = L("Edited on this dialog's other tabs."), TextWrapping = TextWrapping.Wrap };
            where.SetResourceReference(FrameworkElement.StyleProperty, "TextCaption");
            panel.Children.Add(where);
            return panel;
         }

         bool has = !string.IsNullOrEmpty(own);

         // The name is the box's caption, from data, and a CheckBox reads the first
         // underscore of a caption as its access key: "max_message_size_kb" showed as
         // "maxmessage_size_kb", with Alt+M on it, and every setting's name has one.
         var check = new CheckBox { Content = MnemonicText.Escape(level.Name), IsChecked = has };
         var value = new TextBox
         {
            Text = has ? own : "",
            IsEnabled = has,
            MaxWidth = 420,
            HorizontalAlignment = HorizontalAlignment.Left,
            MinWidth = 160,
            // The box's own value and what it does, under its caption (review finding F294).
            Margin = new Thickness(DesignTokens.CheckBoxCaptionInset, 2, 0, 2)
         };
         AutomationProperties.SetName(value, F("Own value of {0}", level.Name));
         check.Checked += (_, _) => value.IsEnabled = true;
         check.Unchecked += (_, _) => value.IsEnabled = false;

         panel.Children.Add(check);
         panel.Children.Add(value);
         effect.Margin = new Thickness(DesignTokens.CheckBoxCaptionInset, 0, 0, 0);
         panel.Children.Add(effect);

         rows_.Add(new Row { Level = level, Own = check, Value = value, Effect = effect, LoadedOwn = has, LoadedValue = has ? own : "" });
         return panel;
      }

      private void SaveLevels()
      {
         dynamic owner = null;
         var refused = new List<string>();

         try
         {
            owner = OpenOwner();

            foreach (Row row in rows_)
            {
               bool own = row.Own.IsChecked == true;
               string typed = SettingLevels.ForServer(row.Value.Text, row.Level.Type);

               // Only what changed is written: every write is a row of its own.
               if (own == row.LoadedOwn && (!own || typed == row.LoadedValue))
                  continue;

               if (own && string.IsNullOrEmpty(typed))
               {
                  refused.Add(F("{0}: {1}", row.Level.Name, L("a value of its own is needed, or untick it to take the level above.")));
                  continue;
               }

               try
               {
                  owner.SettingOverride[row.Level.Name] = own ? typed : "";
               }
               catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
               {
                  refused.Add(F("{0}: {1}", row.Level.Name, ServerSession.DescribeComError(ex)));
               }
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            // The domain or account could not be opened to write to, so nothing was
            // saved: said after what failed, as the read's failure is, where the
            // Critical notice carried DescribeComError's sentence alone.
            refused.Add(F("Could not save the setting levels: {0}", ServerSession.DescribeComError(ex)));
         }
         finally
         {
            if (owner != null)
               ServerSession.Release((object)owner);
         }

         Reload();

         // A read that failed after the save says so, with Try again, and is not
         // written over: the levels on the tab are not the ones just saved. What the
         // save did is said before it - what it could not write, "Could not save the
         // setting levels:" and why when the domain or account could not be opened,
         // or that it wrote everything - where the read's failure was all the tab
         // said: under a lost link the save's own failure and the read's are one
         // event, and the typed values went from the tab with nothing to say that the
         // save had not happened.
         if (ReadFailure.IsShown(status_))
         {
            string saved = refused.Count == 0
               ? L("The setting levels were saved.")
               : string.Join(Environment.NewLine, refused);
            status_.Show(StatusLevel.Critical, saved + Environment.NewLine + status_.Text);
            return;
         }

         if (refused.Count == 0)
            status_.Show(StatusLevel.Good, L("Saved. What is in effect is shown beside each setting."));
         else
            status_.Show(StatusLevel.Critical, string.Join(Environment.NewLine, refused));
      }

      private dynamic OpenOwner()
      {
         dynamic domains = ServerSession.Current.Application.Domains;
         dynamic domain = domains.ItemByName[domainName_];
         ServerSession.Release((object)domains);

         if (!ForAccount)
            return domain;

         dynamic accounts = domain.Accounts;
         dynamic account = accounts.ItemByAddress[address_];
         ServerSession.Release((object)accounts);
         ServerSession.Release((object)domain);
         return account;
      }
   }
}
