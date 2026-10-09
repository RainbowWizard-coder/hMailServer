// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Input;
using hMailServer.ControlPanel.Services;
using hMailServer.ControlPanel.Views.Scaffold;
using System.Linq;
using MessageBox = hMailServer.ControlPanel.Views.Dialogs;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// Data-driven settings pages for the hMailServer.INI feature switches:
   /// transport security (DANE, DNSSEC, MTA-STS, ARC, TLS-RPT), automatic
   /// certificates (ACME), integrations (REST API, Prometheus metrics,
   /// ManageSieve), authentication (OAuth2, password storage), the DNS
   /// resolver and the public web services listener.
   ///
   /// On the scaffold since the fourth wave: the header is a PageHeader with a
   /// StatusPill for where the page stands (SettingsPageStatus, kept by
   /// IniPageSaveState: none while the editors hold what was read, Unsaved changes
   /// from the first edit, and what a save settled in until the next), each card is a
   /// Card holding a SettingsSection of FieldRows, a computed warning is an
   /// InlineNotice, and the state's sentence is one too. The definition below -
   /// every Key and Label - is what build/generate-settings-index.ps1 and the
   /// search-index test read, and is untouched by that.
   /// </summary>
   public partial class FeatureSettingsView : UserControl, IPageLifecycle
   {
      public enum Section
      {
         Security,
         Automation,
         Integration,
         Hardening,
         Authentication,
         Dns,
         WebServices
      }

      private abstract class Setting
      {
         public string Key;
         public string Label;

         /// <summary>
         /// A caption printed under the editor and attached to it as accessible
         /// help text - for the statements in <see cref="SettingClaims"/> about
         /// what the server actually does with the value. Named to match
         /// ServerSettingsView, so the two settings views read the same way.
         /// </summary>
         public string Blurb;

         /// <summary>
         /// What a screen reader should call this editor. Assigned by
         /// <see cref="AssignAccessibleNames"/> before the editors are built,
         /// because the answer depends on the whole page - see
         /// <see cref="AccessibleNames"/>.
         /// </summary>
         public string AccessibleName;

         public abstract FrameworkElement CreateEditor(IniFeatureStore store);
         public abstract void Save(IniFeatureStore store);

         /// <summary>
         /// The value currently sitting in the editor, as the INI would spell it
         /// ("1"/"0" for booleans), or null before the editor has been built.
         /// The computed warnings read this so they react while the administrator
         /// is still typing, instead of only after a save and a page reload.
         /// </summary>
         public virtual string LiveValue => null;

         /// <summary>
         /// What the editor held as the page was built, or as it was last saved -
         /// what <see cref="IsChanged"/> compares against. The editor's own value
         /// rather than what the file holds, so that a value the editor cannot show
         /// as it was stored (a choice the list does not have, a number written
         /// with spaces) is not taken for an edit nobody made.
         /// </summary>
         private string shown_;

         /// <summary>Records the editor's value as the one on record: just built, or just saved.</summary>
         public void MarkShown() => shown_ = LiveValue;

         /// <summary>
         /// Whether the editor holds something other than what it was filled with or
         /// last saved. A row with no editor value - a link to another page, a button
         /// - is never changed.
         /// </summary>
         public virtual bool IsChanged => LiveValue != null && !string.Equals(LiveValue, shown_, StringComparison.Ordinal);

         /// <summary>
         /// Calls <paramref name="handler"/> whenever the editor's value changes.
         /// No-op for editors that never take part in a computed warning.
         /// </summary>
         public virtual void OnEditorChanged(Action handler)
         {
         }

         protected static void SetAid(FrameworkElement element, string id)
         {
            if (element != null && !string.IsNullOrEmpty(id))
               AutomationProperties.SetAutomationId(element, id);
         }

         /// <summary>
         /// The AutomationId plus the accessible name.
         ///
         /// An AutomationId is for test automation and is never spoken. Every
         /// editor on this page is labelled by a separate TextBlock above it and
         /// WPF does not connect the two, so before this each of the 83 INI
         /// settings here was announced as an unnamed "edit" - including the
         /// password pepper, whose label carries the warning that changing it
         /// invalidates every stored password.
         /// </summary>
         protected void Describe(FrameworkElement element, string id)
         {
            SetAid(element, id);

            if (element != null && !string.IsNullOrEmpty(AccessibleName))
               AutomationProperties.SetName(element, AccessibleName);
         }

         /// <summary>
         /// The editor in its row: the caption above it, <see cref="Blurb"/> (or
         /// <paramref name="hint"/>) under it and attached to the editor as help
         /// text. Both, for the reason given on ServerSettingsView's Annotate: a
         /// caption sitting loose in the panel is reached only after the editor,
         /// and a note saying the server does less than the control suggests has
         /// to be heard with it. The row attaches it to a bare editor itself; an
         /// editor inside a grid - a path beside its browse button - is passed as
         /// <paramref name="editor"/> and told here, since the row sees the grid.
         /// </summary>
         protected FieldRow Row(FrameworkElement content, string caption, FrameworkElement editor = null, string hint = null)
         {
            hint ??= Blurb;
            if (string.IsNullOrEmpty(hint))
               hint = null;

            if (editor != null && hint != null)
               AutomationProperties.SetHelpText(editor, hint);

            // When a save of this key reaches the running server, as the server
            // marks it (Services/SettingApplies); nothing for a row kept in another
            // section, which the mark does not cover.
            string applies = InSettingsSection ? SettingApplies.Caption(IniFeatureStore.AppliesFor(Key)) : null;

            // A key the server's environment sets is shown and not edited: the
            // server refuses the change, which the next start would undo
            // (Services/EnvironmentSettings). Its mark takes the applies mark's place,
            // and the editor keeps the value it read, which a save does not write
            // because it has not changed.
            string environment = InSettingsSection ? EnvironmentSettings.Caption(IniFeatureStore.EnvironmentForKey(Key)) : null;
            if (environment != null)
            {
               content.IsEnabled = false;
               applies = environment;
            }

            // The live update's trust is read by the server from hMailServer.INI
            // alone, never from the database, and a change over COM is refused
            // (docs/UpdateFeed.md): the process an update is checked for may not
            // choose what it is checked against. Shown, then, and not edited; the
            // editor keeps the value it read, which a save does not write because
            // it has not changed.
            if (InSettingsSection && Key != null && TrustAnchorKeys.Contains(Key))
            {
               content.IsEnabled = false;
               applies = L("Set in hMailServer.ini only");
            }

            return new FieldRow { Label = caption, Hint = hint, Content = content, Applies = applies };
         }

         /// <summary>
         /// Whether the row's Key is a [Settings] key - the settings store, which the
         /// server marks with when a save applies. False for the rows kept in another
         /// section of the file.
         /// </summary>
         protected virtual bool InSettingsSection => true;

         /// <summary>
         /// The live update's trust (IniSettingStore::IsTrustAnchorName on the
         /// server): the five [Settings] keys it reads from hMailServer.INI alone.
         /// </summary>
         private static readonly HashSet<string> TrustAnchorKeys = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
         {
            "UpdateSigningIdentity", // no-loc
            "UpdateSigningIssuer", // no-loc
            "UpdateSourceRepository", // no-loc
            "UpdateTrustRootsFile", // no-loc
            "UpdateLogPublicKeyFile" // no-loc
         };

         /// <summary>The width of a text editor on these pages: one column, whatever the value.</summary>
         protected const double EditorWidth = 520;

         /// <summary>
         /// The value the editor was filled with, as the INI spells it - what a save
         /// compares against, so that a page of eighty fields with one changed writes
         /// one. Every write publishes on the server (docs/SettingsApply.md), and a
         /// save that wrote every field unchanged was eighty reloads of the whole
         /// store for one value; it also stored every default it had merely shown,
         /// turning each into a stored value the next release's default could not
         /// reach.
         /// </summary>
         protected string Loaded;

         /// <summary>Writes the value when it is not what the editor was filled with.</summary>
         protected void WriteIfChanged(IniFeatureStore store, string value)
         {
            if (Loaded != null && string.Equals(value, Loaded, StringComparison.Ordinal))
               return;

            store.Write(Key, value);
            Loaded = value;
         }
      }

      private class BoolSetting : Setting
      {
         public bool Default;
         private CheckBox box_;

         public override FrameworkElement CreateEditor(IniFeatureStore store)
         {
            bool current = store.ReadBool(Key, Default);
            Loaded = current ? "1" : "0";
            box_ = new CheckBox { Content = Label, IsChecked = current };
            SetAid(box_, Key);

            // A checkbox names itself from its Content, so it needs an override
            // only when the resolved name differs - i.e. when the same wording
            // appears on another card and has been qualified with the card title.
            if (!string.IsNullOrEmpty(AccessibleName) &&
                !string.Equals(AccessibleName, Label, StringComparison.Ordinal))
            {
               AutomationProperties.SetName(box_, AccessibleName);
            }

            // The check box is its own caption, so the row has none.
            return Row(box_, null);
         }

         public override void Save(IniFeatureStore store)
            => WriteIfChanged(store, box_.IsChecked is true ? "1" : "0");

         public override string LiveValue
            => box_ == null ? null : (box_.IsChecked is true ? "1" : "0");

         public override void OnEditorChanged(Action handler)
         {
            if (box_ == null || handler == null)
               return;
            box_.Checked += (s, e) => handler();
            box_.Unchecked += (s, e) => handler();
         }
      }

      private class TextSetting : Setting
      {
         public string Default = "";
         public string Placeholder = "";
         private Wpf.Ui.Controls.TextBox box_;

         public override FrameworkElement CreateEditor(IniFeatureStore store)
         {
            string current = store.Read(Key, Default);
            Loaded = (current ?? "").Trim();
            box_ = new Wpf.Ui.Controls.TextBox
            {
               Text = current,
               PlaceholderText = Placeholder,
               Width = EditorWidth,
               HorizontalAlignment = HorizontalAlignment.Left
            };
            Describe(box_, Key);
            return Row(box_, Label);
         }

         public override void Save(IniFeatureStore store)
            => WriteIfChanged(store, box_.Text.Trim());

         public override string LiveValue => box_?.Text;

         public override void OnEditorChanged(Action handler)
         {
            if (box_ != null && handler != null)
               box_.TextChanged += (s, e) => handler();
         }
      }

      /// <summary>
      /// A path field: a text box plus a "..." button that opens a file or folder
      /// picker (<see cref="PickFolder"/>) and writes the chosen path back into the box.
      /// </summary>
      private class PathSetting : Setting
      {
         public readonly string Default = "";
         public string Placeholder = "";
         public bool PickFolder;
         public string FileFilter = L("All files (*.*)|*.*");
         private Wpf.Ui.Controls.TextBox box_;

         public override FrameworkElement CreateEditor(IniFeatureStore store)
         {
            var row = new Grid { Width = EditorWidth, HorizontalAlignment = HorizontalAlignment.Left };
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

            string current = store.Read(Key, Default);
            Loaded = (current ?? "").Trim();
            box_ = new Wpf.Ui.Controls.TextBox
            {
               Text = current,
               PlaceholderText = Placeholder,
               HorizontalAlignment = HorizontalAlignment.Stretch
            };
            Describe(box_, Key);
            Grid.SetColumn(box_, 0);
            row.Children.Add(box_);

            var browse = new Wpf.Ui.Controls.Button
            {
               Content = "\u2026",
               MinWidth = 40,
               Margin = new Thickness(DesignTokens.Space.Sm, 0, 0, 0),
               VerticalAlignment = VerticalAlignment.Bottom,
               ToolTip = PickFolder ? L("Browse for a folder") : L("Browse for a file")
            };
            SetAid(browse, Key + "Browse"); // no-loc
            // The content is a single ellipsis, so without this the certificate and
            // private-key browse buttons on the REST API card are announced as two
            // identical "\u2026" and there is no way to tell which one is which.
            AutomationProperties.SetName(browse,
               PickFolder
                  ? F("Browse for a folder for {0}", AccessibleName ?? Label ?? L("this setting"))
                  : F("Browse for a file for {0}", AccessibleName ?? Label ?? L("this setting")));
            browse.Click += (s, e) =>
            {
               string picked = PickFolder
                  ? PathPicker.PickFolder(box_.Text)
                  : PathPicker.PickFile(box_.Text, FileFilter);
               if (picked != null)
                  box_.Text = picked;
            };
            Grid.SetColumn(browse, 1);
            row.Children.Add(browse);

            return Row(row, Label, box_);
         }

         public override void Save(IniFeatureStore store)
            => WriteIfChanged(store, box_.Text.Trim());

         public override string LiveValue => box_?.Text;

         public override void OnEditorChanged(Action handler)
         {
            if (box_ != null && handler != null)
               box_.TextChanged += (s, e) => handler();
         }
      }

      private class ChoiceSetting : Setting
      {
         public int Default;
         public (int Value, string Label)[] Options;
         private ComboBox combo_;

         public override FrameworkElement CreateEditor(IniFeatureStore store)
         {
            combo_ = new ComboBox { Width = EditorWidth, HorizontalAlignment = HorizontalAlignment.Left };
            if (!int.TryParse(store.Read(Key, Default.ToString()), out int current))
               current = Default;
            Loaded = current.ToString();
            foreach ((int value, string label) in Options)
            {
               var item = new ComboBoxItem { Content = label, Tag = value };
               combo_.Items.Add(item);
               if (value == current)
                  combo_.SelectedItem = item;
            }
            if (combo_.SelectedItem == null && combo_.Items.Count > 0)
               combo_.SelectedIndex = 0;

            Describe(combo_, Key);
            return Row(combo_, Label);
         }

         public override void Save(IniFeatureStore store)
         {
            int value = combo_.SelectedItem is ComboBoxItem cbi ? (int)cbi.Tag : Default;
            WriteIfChanged(store, value.ToString());
         }

         public override string LiveValue
            => combo_?.SelectedItem is ComboBoxItem cbi ? ((int)cbi.Tag).ToString() : null;

         public override void OnEditorChanged(Action handler)
         {
            if (combo_ != null && handler != null)
               combo_.SelectionChanged += (s, e) => handler();
         }
      }

      /// <summary>
      /// A write-only secret field. The current value is never shown (it may be a
      /// DPAPI-protected blob); a placeholder indicates whether one is already set,
      /// and the value is only written when the admin types a new one — so leaving
      /// the field blank keeps the existing secret untouched. While one is stored, a
      /// box under the field removes it with the page's save, which nothing could
      /// before (review finding F239): blank kept it, and nothing cleared it.
      /// </summary>
      private class SecretSetting : Setting
      {
         /// <summary>
         /// Placeholder text shown when no secret is configured yet. It was called
         /// Note; renamed because the base class now carries the note that says what
         /// the server does with the value, and two members of that name would have
         /// been one silently shadowing the other.
         /// </summary>
         public string Hint = L("");

         /// <summary>
         /// Adds a "Generate" button that fills the box with a strong random value
         /// from <see cref="PasswordGenerator"/>. For the server-wide secrets (SRS,
         /// BATV, the metrics bearer token) whose only requirement is randomness:
         /// without the button, "enter a secret" quietly invites a weak one.
         /// </summary>
         public bool OfferGenerate;

         /// <summary>
         /// A warning that stays under the field, whatever is typed: for a secret
         /// whose change does harm of its own - the password pepper's.
         /// </summary>
         public string Warning;

         private hMailServer.ControlPanel.Views.PasswordField box_;
         private bool hasStored_;
         private FieldRow row_;
         private CheckBox remove_;
         private InlineNotice warning_;

         /// <summary>
         /// True when a secret is effectively configured: one is stored in the INI,
         /// or the administrator has typed one into the editor. What the computed
         /// warnings ask, since a blank box keeps the stored value - unless the box
         /// that removes it is ticked.
         /// </summary>
         public bool IsConfigured(IniFeatureStore store)
         {
            if (box_ != null && RemovesStored && !Conflicts)
               return false;
            if (box_ != null && !string.IsNullOrEmpty(box_.Password))
               return true;
            if (box_ != null)
               return hasStored_;
            return !string.IsNullOrEmpty(store.Read(Key, "").Trim());
         }

         /// <summary>Whether the page's save removes the stored secret.</summary>
         public bool RemovesStored => remove_?.IsChecked == true;

         /// <summary>
         /// A new value typed since the page was read or saved, and the removal of the
         /// stored one ticked, both: which was meant is not the page's to guess, and
         /// the save refuses, as the Directory authentication page's does for its
         /// service account password. The value a save just wrote, still in the box
         /// to be copied - a generated token, say - is not a new one.
         /// </summary>
         public bool Conflicts => RemovesStored && base.IsChanged && !string.IsNullOrEmpty(box_?.Password);

         /// <summary>The removal is a change the page has not saved, as a typed value is.</summary>
         public override bool IsChanged => base.IsChanged || RemovesStored;

         private string Placeholder_()
            => hasStored_
               ? L("A secret is configured — leave blank to keep it")
               : (string.IsNullOrEmpty(Hint) ? L("Enter a secret") : Hint);

         // A PasswordBox's placeholder is not part of its accessible name, and
         // whether a secret is already stored is the one thing this control
         // conveys that its label does not - leaving the field blank keeps the
         // existing value, so a listener who cannot see the placeholder has no
         // way to know whether there is one. The row's hint carries it, under
         // the editor and in its help text alike.
         private string Hint_()
            => !hasStored_ ? Blurb
               : Sentences(Placeholder_(), Blurb);

         public override FrameworkElement CreateEditor(IniFeatureStore store)
         {
            hasStored_ = !string.IsNullOrEmpty(store.Read(Key, "").Trim());

            box_ = new hMailServer.ControlPanel.Views.PasswordField
            {
               PlaceholderText = Placeholder_(),
               HorizontalAlignment = HorizontalAlignment.Stretch
            };
            Describe(box_, Key);
            string hint = Hint_();

            if (!OfferGenerate)
            {
               box_.Width = EditorWidth;
               box_.HorizontalAlignment = HorizontalAlignment.Left;
               return Arrange_(Row(box_, Label, hint: hint));
            }

            var row = new Grid { Width = EditorWidth, HorizontalAlignment = HorizontalAlignment.Left };
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

            Grid.SetColumn(box_, 0);
            row.Children.Add(box_);

            var generate = new Wpf.Ui.Controls.Button   // per setting: no access key, the rows are reached with the arrow keys
            {
               Content = L("Generate"),
               Margin = new Thickness(DesignTokens.Space.Sm, 0, 0, 0),
               VerticalAlignment = VerticalAlignment.Bottom,
               ToolTip = L("Fill in a strong random secret")
            };
            SetAid(generate, Key + "Generate"); // no-loc
            // The visible content is the same word on every secret that offers
            // it, so the accessible name says which secret this one fills.
            AutomationProperties.SetName(generate,
               F("Generate a random value for {0}", AccessibleName ?? Label ?? L("this secret")));
            generate.Click += (s, e) => box_.Password = PasswordGenerator.Generate(32);
            Grid.SetColumn(generate, 1);
            row.Children.Add(generate);

            return Arrange_(Row(row, Label, box_, hint));
         }

         /// <summary>
         /// The row; under it the box that removes the stored secret, shown while one
         /// is stored; and <see cref="Warning"/> as a notice close under them when
         /// there is one - a notice and not a hint, because it is a warning to be
         /// seen before anything is typed, in the level's colour, mark and word. The
         /// panel keeps the field's gap below it, so a card's last field gives it up
         /// as a lone row does.
         /// </summary>
         private FrameworkElement Arrange_(FieldRow row)
         {
            row_ = row;

            remove_ = new CheckBox   // per setting: no access key, the rows are reached with the arrow keys
            {
               Content = L("Remove the stored secret when saving")
            };
            SetAid(remove_, Key + "Remove"); // no-loc
            // The visible words are the same under every secret, so the accessible
            // name, which starts with them, says which secret this one removes.
            AutomationProperties.SetName(remove_,
               F("Remove the stored secret when saving - {0}", AccessibleName ?? Label ?? L("this secret")));

            var panel = new StackPanel();
            panel.SetResourceReference(MarginProperty, "AppFieldGap");
            panel.Children.Add(row);
            panel.Children.Add(remove_);

            if (!string.IsNullOrEmpty(Warning))
            {
               warning_ = new InlineNotice { Level = StatusLevel.Warning, Text = Warning, MaxWidth = EditorWidth, HorizontalAlignment = HorizontalAlignment.Left, Margin = new Thickness(0) };
               AutomationProperties.SetAutomationId(warning_, Key + "Warning"); // no-loc
               panel.Children.Add(warning_);
            }

            ShowStored_(hasStored_);
            return panel;
         }

         /// <summary>
         /// Whether a secret is stored, in the placeholder, the hint and the removal
         /// box - as the page is built, and again after a save that stored one or
         /// removed it.
         /// </summary>
         private void ShowStored_(bool stored)
         {
            hasStored_ = stored;
            box_.PlaceholderText = Placeholder_();
            row_.Hint = Hint_();
            remove_.IsChecked = false;
            remove_.Visibility = stored ? Visibility.Visible : Visibility.Collapsed;
            AutomationProperties.SetHelpText(box_, row_.Hint ?? "");

            // The field's gap is the panel's; inside it, the small step between the
            // parts that are on screen.
            row_.Margin = new Thickness(0, 0, 0, stored || warning_ != null ? DesignTokens.Space.Sm : 0);
            remove_.Margin = new Thickness(0, 0, 0, warning_ != null ? DesignTokens.Space.Sm : 0);
         }

         public override void Save(IniFeatureStore store)
         {
            string entered = box_.Password;

            // The page refuses a save with both before it writes anything (Save_);
            // a value that reaches here with both is not guessed at.
            if (Conflicts)
               throw new InvalidOperationException(ConflictSentence(Label));

            if (RemovesStored)
            {
               store.Write(Key, "");
               box_.Password = "";
               ShowStored_(false);
               return;
            }

            // A value is written when it is not the one a save just wrote and the box
            // still holds, which stays there to be copied, as it always has.
            if (!string.IsNullOrEmpty(entered) && base.IsChanged)
            {
               store.Write(Key, entered);
               ShowStored_(true);
            }
            // Blank = keep the existing secret.
         }

         /// <summary>What a save with a new value and the removal both ticked says.</summary>
         public static string ConflictSentence(string label)
            => F("A new value has been typed for \"{0}\", and \"Remove the stored secret when saving\" is ticked under it. Clear the value or untick the box - guessing which was meant is not this page's decision to make.", label ?? "");

         public override string LiveValue => box_?.Password;

         public override void OnEditorChanged(Action handler)
         {
            // PasswordChanged, which WPF-UI 4.3 raises for a typed character and for a
            // Password set in code - Generate's - alike. Not TextChanged, though the box
            // is a TextBox: a Password set in code rewrites the mask without raising it,
            // so a generated secret left the pill saying nothing over an unsaved change,
            // and the warnings that ask whether a secret is set unrefreshed.
            if (box_ != null && handler != null)
               box_.PasswordChanged += (s, e) => handler();

            // The removal box is an edit of its own: the pill, and the warnings that
            // ask whether a secret is configured, follow it.
            if (remove_ != null && handler != null)
            {
               remove_.Checked += (s, e) => handler();
               remove_.Unchecked += (s, e) => handler();
            }
         }
      }

      /// <summary>
      /// Direct reads and writes of hMailServer.INI sections other than [Settings],
      /// which is all <see cref="IniFeatureStore"/> speaks. Used by the sending-limit
      /// editors ([SendingLimits] / [SendingLimitsOverrides]) and by the computed
      /// warnings that need [Directories] DataFolder to locate the ACME certificate.
      /// </summary>
      private static class IniDirect
      {
         public static string ReadValue(string iniPath, string section, string key, string defaultValue)
         {
            if (string.IsNullOrEmpty(iniPath))
               return defaultValue;
            return ProfileApi.ReadString(section, key, defaultValue, iniPath, 4096);
         }

         public static void WriteValue(string iniPath, string section, string key, string value)
         {
            if (string.IsNullOrEmpty(iniPath))
               throw new InvalidOperationException("hMailServer.INI was not found on this machine.");
            ProfileApi.WriteString(section, key, value, iniPath);
         }

         /// <summary>The key=value lines of one section, in file order.</summary>
         public static List<string> ReadSectionLines(string iniPath, string section)
         {
            if (string.IsNullOrEmpty(iniPath))
               return new List<string>();
            return ProfileApi.ReadSectionLines(section, iniPath);
         }

         /// <summary>Replaces one section's lines wholesale (empty list clears it).</summary>
         public static void WriteSectionLines(string iniPath, string section, IReadOnlyList<string> lines)
         {
            if (string.IsNullOrEmpty(iniPath))
               throw new InvalidOperationException("hMailServer.INI was not found on this machine.");
            ProfileApi.WriteSectionLines(section, lines, iniPath);
         }
      }

      /// <summary>
      /// A text setting stored in an hMailServer.INI section other than [Settings].
      /// Same editor as <see cref="TextSetting"/>; only the section differs.
      /// </summary>
      private class SectionTextSetting : Setting
      {
         public string Section;

         protected override bool InSettingsSection => false;
         public string Default = "";
         public string Placeholder = "";
         private Wpf.Ui.Controls.TextBox box_;

         public override FrameworkElement CreateEditor(IniFeatureStore store)
         {
            box_ = new Wpf.Ui.Controls.TextBox
            {
               Text = IniDirect.ReadValue(store.IniPath, Section, Key, Default),
               PlaceholderText = Placeholder,
               Width = EditorWidth,
               HorizontalAlignment = HorizontalAlignment.Left
            };
            Describe(box_, Key);
            return Row(box_, Label);
         }

         public override void Save(IniFeatureStore store)
            => IniDirect.WriteValue(store.IniPath, Section, Key, box_.Text.Trim());

         public override string LiveValue => box_?.Text;

         public override void OnEditorChanged(Action handler)
         {
            if (box_ != null && handler != null)
               box_.TextChanged += (s, e) => handler();
         }
      }

      /// <summary>
      /// Edits a whole INI section as lines, one entry per line. Built for
      /// [SendingLimitsOverrides], whose entries are per-address values rather than
      /// fixed keys. Reads the section back on every build (so it never misreports
      /// its own state) and only rewrites the section when the text was actually
      /// changed, so merely opening and saving the page cannot disturb the file.
      /// </summary>
      private class SectionLinesSetting : Setting
      {
         public string Section;

         protected override bool InSettingsSection => false;
         public string Placeholder = "";
         private Wpf.Ui.Controls.TextBox box_;
         private string loaded_;

         private static string Normalize(IEnumerable<string> lines)
            => string.Join("\n", lines.Select(l => l.Trim()).Where(l => l.Length > 0));

         public override FrameworkElement CreateEditor(IniFeatureStore store)
         {
            loaded_ = Normalize(IniDirect.ReadSectionLines(store.IniPath, Section));

            box_ = new Wpf.Ui.Controls.TextBox
            {
               Text = loaded_,
               PlaceholderText = Placeholder,
               Width = EditorWidth,
               MinHeight = 88,
               AcceptsReturn = true,
               TextWrapping = TextWrapping.NoWrap,
               VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
               HorizontalAlignment = HorizontalAlignment.Left
            };
            Describe(box_, Key);
            return Row(box_, Label);
         }

         public override void Save(IniFeatureStore store)
         {
            string current = Normalize(box_.Text.Split('\n'));
            if (current == loaded_)
               return;

            IniDirect.WriteSectionLines(store.IniPath, Section,
               current.Length == 0 ? new string[0] : current.Split('\n'));
            loaded_ = current;
         }

         public override string LiveValue => box_?.Text;

         public override void OnEditorChanged(Action handler)
         {
            if (box_ != null && handler != null)
               box_.TextChanged += (s, e) => handler();
         }
      }

      /// <summary>
      /// A row that names something edited on another page, and goes there.
      ///
      /// Two uses, both the same shape. A setting that has MOVED is a link an
      /// administrator has already followed once and a note in somebody's runbook,
      /// and neither survives a silent relocation: the page opens, the setting is
      /// not on it, and the reasonable conclusion is that the feature was removed.
      /// And a setting that has always been elsewhere - the API keys, beside the
      /// listener they authenticate - is worth naming where it will be looked for
      /// rather than only where it lives.
      ///
      /// Deliberately not persisted, and deliberately carries no Key, so that the
      /// settings-index generator cannot index it: the Ctrl+K palette must send
      /// somebody searching for "session tickets" to the page that owns the
      /// setting, not to a signpost pointing at it.
      /// </summary>
      private class ElsewhereSetting : Setting
      {
         private readonly string page_;

         /// <summary>Creates a signpost row pointing at the page that owns a setting.</summary>
         /// <param name="page">Nav key of the page that owns the setting.</param>
         /// <param name="caption">What is over there, in the administrator's words.</param>
         public ElsewhereSetting(string page, string caption)
         {
            page_ = page;
            Label = caption;
         }

         public override FrameworkElement CreateEditor(IniFeatureStore store)
         {
            // LocationOf gives the full "group > page" trail and returns "" - not
            // null - for a key it does not know, so an empty result has to fall
            // back to the title rather than be printed as a blank destination.
            string destination = NavigationMap.LocationOf(page_);
            if (string.IsNullOrEmpty(destination))
               destination = NavigationMap.TitleOf(page_);

            var row = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 0, 0, DesignTokens.Space.Sm) };

            var text = new TextBlock
            {
               Text = Label + "  →  " + destination,
               VerticalAlignment = VerticalAlignment.Center,
               MaxWidth = 420
            };
            text.SetResourceReference(StyleProperty, "TextBody");
            row.Children.Add(text);

            var button = new Wpf.Ui.Controls.Button   // per setting: no access key, the rows are reached with the arrow keys
            {
               Content = L("Open…"),
               Appearance = Wpf.Ui.Controls.ControlAppearance.Transparent,
               Padding = new Thickness(DesignTokens.Space.Sm, DesignTokens.Space.Xs, DesignTokens.Space.Sm, DesignTokens.Space.Xs),
               Margin = new Thickness(DesignTokens.Space.Sm, 0, 0, 0),
               VerticalAlignment = VerticalAlignment.Center,
               Cursor = Cursors.Hand,
               ToolTip = F("Open {0}", destination)
            };
            button.SetResourceReference(FontSizeProperty, "AppFontSizeCaption");
            AutomationProperties.SetName(button, F("Open {0}, which now has {1}", destination, Label));
            SetAid(button, "elsewhere-" + page_);
            button.Click += (s, e) => (Application.Current?.MainWindow as MainWindow)?.NavigateTo(page_);
            row.Children.Add(button);

            return row;
         }

         /// <summary>Nothing to save: this row edits nothing.</summary>
         public override void Save(IniFeatureStore store)
         {
         }
      }

      /// <summary>
      /// A button on the Windows service account card that runs hMailServer.exe as
      /// an administrator, after the elevation prompt, and shows what it said
      /// (docs/ServiceAccount.md): Grant access, and the two moves. The account is
      /// passed on /Register's command line, the only place /Register takes it from
      /// - never ServiceAccountName, which the service and hMailServer
      /// administrators can write. The move to NT SERVICE\hMailServer is made only
      /// after a grant that ended cleanly, and the grant runs with --confirm: a
      /// folder or key file the configuration names outside the service's folders
      /// is asked about, by the elevated program itself, before anything is
      /// approved. It carries no Key - it edits nothing, saves nothing, and the
      /// settings index has nothing to find in it - and works only on the server
      /// itself, whose folders these are.
      /// </summary>
      private class ServiceAccountActionSetting : Setting
      {
         public enum Action
         {
            Grant,
            MoveToOwnAccount,
            MoveToLocalSystem
         }

         public Action Does;
         public string ButtonText;
         public string AutomationId;

         protected override bool InSettingsSection => false;

         public override FrameworkElement CreateEditor(IniFeatureStore store)
         {
            string executable = ServerExecutable_(store);
            bool here = ServerSession.IsLocalSession && executable != null;

            var button = new Wpf.Ui.Controls.Button   // per setting: no access key, the rows are reached with the arrow keys
            {
               Content = ButtonText,
               IsEnabled = here,
               HorizontalAlignment = HorizontalAlignment.Left
            };
            SetAid(button, AutomationId);
            AutomationProperties.SetName(button, Label);

            button.Click += async (s, e) =>
            {
               button.IsEnabled = false;
               try
               {
                  (int exitCode, string said, string error) outcome = await Task.Run(() => RunElevated_(executable, Does));
                  if (outcome.error != null)
                     MessageBox.Show(F("hMailServer.exe could not be run: {0}", outcome.error), L("Control Panel"), MessageBoxButton.OK, MessageBoxImage.Warning);
                  else if (outcome.exitCode != 0)
                     MessageBox.Show(F("hMailServer.exe /GrantServiceAccess did not finish its work (exit code {0}):\n\n{1}", outcome.exitCode, outcome.said), L("Control Panel"), MessageBoxButton.OK, MessageBoxImage.Warning);
                  else if (Does == Action.Grant)
                     MessageBox.Show(F("hMailServer.exe /GrantServiceAccess finished:\n\n{0}", outcome.said), L("Control Panel"), MessageBoxButton.OK, MessageBoxImage.Information);
                  else
                  {
                     string account = Does == Action.MoveToOwnAccount ? "NT SERVICE\\hMailServer" : "LocalSystem"; // no-loc
                     MessageBox.Show(F("The service is registered to log on as {0} from its next start. Restart the hMailServer service - in services.msc, or with net stop hMailServer and net start hMailServer - for it to take effect.\n\n{1}", account, outcome.said),
                        L("Control Panel"), MessageBoxButton.OK, MessageBoxImage.Information);
                  }
               }
               finally
               {
                  button.IsEnabled = true;
               }
            };

            return Row(button, Label, button, here ? Blurb : L("Open the Control Panel on the server itself to change the account the service runs as or grant it its folders."));
         }

         /// <summary>Nothing to save: this row edits nothing.</summary>
         public override void Save(IniFeatureStore store)
         {
         }

         /// <summary>
         /// hMailServer.exe beside hMailServer.INI, where the installer puts both;
         /// else the service's own registration; null when neither names one that
         /// is there.
         /// </summary>
         private static string ServerExecutable_(IniFeatureStore store)
         {
            if (store.IsAvailable)
            {
               string beside = System.IO.Path.Combine(System.IO.Path.GetDirectoryName(store.IniPath) ?? "", "hMailServer.exe"); // no-loc
               if (System.IO.File.Exists(beside))
                  return beside;
            }

            WindowsServiceInfo service = WindowsServiceInfo.Query();
            string registered = service.Exists ? WindowsServiceInfo.ExecutableFrom(service.PathName) : null;
            return registered != null && System.IO.File.Exists(registered) ? registered : null;
         }

         /// <summary>
         /// The command line cmd.exe runs, elevated. The move to the virtual
         /// account is two commands joined by &amp;&amp;, so that /Register runs
         /// only when the grant ended in 0 - every folder reached, or confirmed.
         /// </summary>
         private static string CommandFor_(string executable, Action action, string report)
         {
            string exe = "\"" + executable + "\""; // no-loc
            string into = " > \"" + report + "\" 2>&1"; // no-loc
            string onto = " >> \"" + report + "\" 2>&1"; // no-loc
            switch (action)
            {
               case Action.MoveToOwnAccount:
                  return exe + " --confirm /GrantServiceAccess" + into + " && " + exe + " --service-account virtual /Register" + onto; // no-loc
               case Action.MoveToLocalSystem:
                  return exe + " --service-account localsystem /Register" + into; // no-loc
               default:
                  return exe + " --confirm /GrantServiceAccess" + into; // no-loc
            }
         }

         /// <summary>
         /// Runs the command elevated, its lines into a file of this session's own,
         /// and hands back its exit code and what it said - or why it could not be
         /// run. The elevated process is cmd.exe so that the verb's output has
         /// somewhere to go: a program started through the elevation prompt has no
         /// handles this process can read.
         /// </summary>
         private static (int exitCode, string said, string error) RunElevated_(string executable, Action action)
         {
            string report = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "hMailServer-service-account-" + Guid.NewGuid().ToString("N") + ".txt"); // no-loc
            try
            {
               var start = new System.Diagnostics.ProcessStartInfo
               {
                  FileName = System.IO.Path.Combine(Environment.SystemDirectory, "cmd.exe"), // no-loc
                  // /d, as ServiceRestart.TryRestart: no Command Processor AutoRun in the
                  // elevated cmd.exe.
                  Arguments = "/d /s /c \"" + CommandFor_(executable, action, report) + "\"", // no-loc
                  UseShellExecute = true,
                  Verb = "runas", // no-loc
                  WindowStyle = System.Diagnostics.ProcessWindowStyle.Hidden
               };

               int exitCode;
               using (var process = System.Diagnostics.Process.Start(start))
               {
                  if (process == null)
                     return (-1, "", null);
                  process.WaitForExit();
                  exitCode = process.ExitCode;
               }

               string said = System.IO.File.Exists(report) ? System.IO.File.ReadAllText(report, System.Text.Encoding.UTF8).Trim() : "";
               return (exitCode, said, null);
            }
            catch (System.ComponentModel.Win32Exception ex) when (ex.NativeErrorCode == 1223)
            {
               return (-1, "", L("the elevation prompt was cancelled."));
            }
            catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
            {
               return (-1, "", ex.GetBaseException().Message);
            }
            finally
            {
               try
               {
                  if (System.IO.File.Exists(report))
                     System.IO.File.Delete(report);
               }
               catch (Exception ex) when (ex is System.IO.IOException || ex is UnauthorizedAccessException)
               {
                  // A report left in this session's temp folder costs nothing.
               }
            }
         }
      }

      /// <summary>What one computed warning has decided to say, or null for nothing.</summary>
      private class WarningState
      {
         public StatusLevel Level;
         public string Text;
      }

      /// <summary>
      /// A computed warning printed inside a card, under the settings it is about.
      /// <see cref="Compute"/> runs against the live editors (falling back to the
      /// INI for values on other pages) and returns null when there is nothing to
      /// say. Each carries a colour, a shape AND a word via StatusSemantics, so the
      /// meaning survives greyscale, colour blindness and High Contrast.
      /// </summary>
      private class WarningDef
      {
         /// <summary>AutomationId of the warning row, so tests can find it.</summary>
         public string Aid;

         public Func<WarningState> Compute;
      }

      private class CardDef
      {
         public string Title;
         public string Blurb;
         public List<Setting> Settings = new();
         public List<WarningDef> Warnings = new();
      }

      private readonly Section section_;
      private readonly IniFeatureStore store_ = new();
      private List<CardDef> cards_;

      /// <summary>One built warning: the definition and the notice it drives.</summary>
      private class WarningRow
      {
         public WarningDef Def;
         public InlineNotice Notice;
      }

      private readonly List<WarningRow> warningRows_ = new();

      /// <summary>The pill and the notice under the header, by the one save-state model.</summary>
      private readonly IniPageSaveState saveState_;

      // ---- inputs for the computed warnings -----------------------------------
      //
      // Each reads the live editor when the setting is on this page (so a warning
      // reacts while the administrator types) and falls back to the INI file for
      // settings that live on another page. The fallback default must match the
      // server's own default in IniFileSettings.cpp, or the warning would reason
      // about a configuration the server does not run.

      private Setting FindSetting_(string key)
      {
         if (cards_ == null)
            return null;

         return cards_
            .SelectMany(card => card.Settings)
            .FirstOrDefault(setting => string.Equals(setting.Key, key, StringComparison.OrdinalIgnoreCase));
      }

      private string LiveText_(string key, string fallbackDefault)
      {
         Setting setting = FindSetting_(key);
         string live = setting?.LiveValue;
         return live ?? store_.Read(key, fallbackDefault);
      }

      private bool LiveBool_(string key, bool fallbackDefault)
      {
         Setting setting = FindSetting_(key);
         string live = setting?.LiveValue;
         return live != null ? live == "1" : store_.ReadBool(key, fallbackDefault);
      }

      private int LiveInt_(string key, int fallbackDefault)
      {
         return int.TryParse(LiveText_(key, fallbackDefault.ToString()).Trim(), out int value)
            ? value
            : fallbackDefault;
      }

      /// <summary>
      /// Whether a write-only secret is effectively set: typed into the editor, or
      /// already stored in the INI (a blank box keeps the stored value).
      /// </summary>
      private bool SecretConfigured_(string key)
      {
         if (FindSetting_(key) is SecretSetting secret)
            return secret.IsConfigured(store_);

         return !string.IsNullOrEmpty(store_.Read(key, "").Trim());
      }

      /// <summary>
      /// The service account line: what Windows is running the service as.
      ///
      /// This is the "show the state, not the switch" rule applied to the one
      /// setting on the card whose effect is not the server's to deliver. Only the
      /// Service Control Manager knows the truth, so the truth is what is reported,
      /// in every case including the ones where it cannot be determined. There is
      /// nothing to compare it with any more: ServiceAccountName, which /Register
      /// used to read, is not read (docs/ServiceAccount.md), and the account is
      /// changed with the buttons below.
      /// </summary>
      private WarningState ComputeServiceAccountState_()
      {
         // Reading the local SCM only describes this machine, and the settings
         // being edited belong to whichever host the session is connected to.
         if (!ServerSession.IsLocalSession)
         {
            return new WarningState
            {
               Level = StatusLevel.Information,
               Text = L("The Control Panel is connected to another host, so the account ITS service runs as cannot be read from here. Open the Control Panel on the server itself to see it.")
            };
         }

         WindowsServiceInfo service = serviceInfo_ ??= WindowsServiceInfo.Query();

         if (!service.Exists)
         {
            return new WarningState
            {
               Level = StatusLevel.Information,
               Text = service.Error != null
                  ? F("The Service Control Manager could not be queried, so the account the service runs as is unknown: {0}", service.Error)
                  : L("Windows does not report an hMailServer service on this machine, so there is no account to show or move yet.")
            };
         }

         bool localSystem = string.Equals(
            WindowsServiceInfo.Canonical(service.StartName, Environment.MachineName), "localsystem",
            StringComparison.OrdinalIgnoreCase);

         return new WarningState
         {
            Level = localSystem ? StatusLevel.Information : StatusLevel.Good,
            Text = localSystem
               ? F("The service is running as {0}, the most privileged account on the machine: a flaw reachable through SMTP, IMAP or POP3 would have full control of the computer. Move to NT SERVICE\\hMailServer below contains that.", WindowsServiceInfo.DescribeAccount(service.StartName))
               : F("The service is running as {0}, which is not LocalSystem, so it is already contained.", service.StartName)
         };
      }

      /// <summary>
      /// Queried once per page instance. The SCM lookup goes through WMI, which is
      /// slow enough that running it on every keystroke - which is what a computed
      /// warning does - would be felt while typing.
      /// </summary>
      private WindowsServiceInfo serviceInfo_;

      /// <summary>[Directories] DataFolder, or "" when it cannot be read.</summary>
      private string DataFolder_()
         => IniDirect.ReadValue(store_.IniPath, "Directories", "DataFolder", "").Trim();

      /// <summary>
      /// Where the server looks for an issued ACME certificate: the configured
      /// output folder, else Data\ACME - the exact fallback in
      /// AcmeClient::GetCertificateDirectory. "" when neither can be determined.
      /// </summary>
      private string AcmeCertificateFolder_()
      {
         string configured = store_.Read("AcmeCertificateDirectory", "").Trim();
         if (configured.Length > 0)
            return configured;

         string dataFolder = DataFolder_();
         return dataFolder.Length > 0 ? dataFolder + "\\ACME" : "";
      }

      /// <summary>True when both halves of the issued ACME pair exist on disk.</summary>
      private bool AcmeCertificateExists_()
      {
         string folder = AcmeCertificateFolder_();
         return folder.Length > 0
            && System.IO.File.Exists(folder + "\\fullchain.pem")
            && System.IO.File.Exists(folder + "\\privkey.pem");
      }

      /// <summary>
      /// The state of the certificate ACME has actually issued.
      ///
      /// Every other thing on the ACME card is a request - a switch, an e-mail
      /// address, a list of host names - and none of them says whether an issued
      /// certificate exists, when it expires, or whether the renewal that is meant
      /// to be automatic has in fact happened. An expired automatic certificate is
      /// silent by construction: nothing on the page changes, and the first symptom
      /// is clients refusing to connect. So the switch is joined by the one fact
      /// that answers the question, read from the file on disk.
      /// </summary>
      private WarningState ComputeAcmeCertificateState_()
      {
         if (!LiveBool_("AcmeEnabled", false))
            return null;

         if (!ServerSession.IsLocalSession)
         {
            return new WarningState
            {
               Level = StatusLevel.Information,
               Text = L("The issued certificate is a file on the server's disk, and the Control Panel is connected to another host, so its expiry cannot be read from here.")
            };
         }

         string folder = AcmeCertificateFolder_();

         if (folder.Length == 0)
         {
            return new WarningState
            {
               Level = StatusLevel.Information,
               Text = L("Where the issued certificate would be written could not be determined, because neither the output folder above nor [Directories] DataFolder in hMailServer.INI could be read.")
            };
         }

         if (!AcmeCertificateExists_())
         {
            return new WarningState
            {
               Level = StatusLevel.Warning,
               Text = F("ACME is switched on but no certificate has been issued yet - fullchain.pem and privkey.pem are not both in {0}. Issue happens on the server's own schedule, so this is normal for a few minutes after enabling it; if it persists, the CA could not reach this server, and the Setup checklist page checks exactly that.", folder)
            };
         }

         // Cached against the folder it was read for. A computed warning re-runs on
         // every keystroke in every editor on the page, and Inspect opens and parses
         // two PEM files - doing that per character typed into the contact address
         // would be felt. Keying it on the folder means a change to the output
         // folder still re-reads, which is the only edit on this card that can
         // change the answer.
         if (acmeHealth_ == null || acmeHealthFolder_ != folder)
         {
            acmeHealth_ = CertificateInspector.Inspect(
               folder + "\\fullchain.pem", folder + "\\privkey.pem", filesReadableHere: true);
            acmeHealthFolder_ = folder;
         }

         CertificateHealth health = acmeHealth_;

         if (health.ExpiresOn == null || health.DaysRemaining == null)
         {
            return new WarningState
            {
               Level = StatusLevel.Information,
               Text = F("A certificate has been issued into {0}, but its expiry could not be read: {1}", folder,
                  health.CertificateFile?.Detail ?? L("the file could not be parsed as a certificate."))
            };
         }

         int days = health.DaysRemaining.Value;
         string expires = health.ExpiresOn.Value.ToString("d MMMM yyyy", System.Globalization.CultureInfo.CurrentCulture); // no-loc

         if (days < 0)
         {
            return new WarningState
            {
               Level = StatusLevel.Critical,
               Text = PluralF(-days,
                  "The issued certificate EXPIRED on {0}, {1} day ago, and renewal has not replaced it. Clients are refusing this server's TLS. Check the error log for the last renewal attempt: the usual cause is that the CA can no longer reach this server on the http-01 challenge port.",
                  "The issued certificate EXPIRED on {0}, {1} days ago, and renewal has not replaced it. Clients are refusing this server's TLS. Check the error log for the last renewal attempt: the usual cause is that the CA can no longer reach this server on the http-01 challenge port.",
                  expires, -days)
            };
         }

         if (days <= CertificateInspector.ExpiryWarningDays)
         {
            return new WarningState
            {
               Level = StatusLevel.Warning,
               Text = PluralF(days,
                  "The issued certificate expires on {0}, in {1} day. The renewal task runs inside this window, so this is only a problem if the number stops falling - if it does, renewal is failing and the error log says why.",
                  "The issued certificate expires on {0}, in {1} days. The renewal task runs inside this window, so this is only a problem if the number stops falling - if it does, renewal is failing and the error log says why.",
                  expires, days)
            };
         }

         return new WarningState
         {
            Level = StatusLevel.Good,
            Text = PluralF(days,
               "A certificate is issued and valid until {0}, {1} day from now. Renewal starts automatically inside the last {2} days.",
               "A certificate is issued and valid until {0}, {1} days from now. Renewal starts automatically inside the last {2} days.",
               expires, days, CertificateInspector.ExpiryWarningDays)
         };
      }

      /// <summary>The inspected ACME pair, and the folder it was inspected in.</summary>
      private CertificateHealth acmeHealth_;
      private string acmeHealthFolder_;

      /// <summary>
      /// Whether the ManageSieve listener will actually offer STARTTLS.
      ///
      /// It has no certificate setting of its own: ManageSieveServer::
      /// FindTlsCertificate_ borrows one from a TLS-capable IMAP, POP3 or SMTP
      /// port, in that order, and when there is none it starts anyway and offers
      /// plain text only, saying so once in the application log. So an
      /// administrator can configure the port, save, restart, connect - and hand
      /// their mailbox password across the network in the clear, with nothing
      /// anywhere in the GUI having suggested otherwise.
      /// </summary>
      private WarningState ComputeManageSieveTlsState_()
      {
         int port = LiveInt_("ManageSieveServerPort", 0);
         if (port <= 0)
            return null;

         string bind = LiveText_("ManageSieveServerBindAddress", "127.0.0.1").Trim();
         bool loopbackOnly = bind.StartsWith("127.") || bind == "::1";

         // Read once per page instance, including when the answer is "could not
         // tell": ??= alone would retry two COM collection walks on every keystroke
         // for as long as the session stayed broken.
         if (!tlsPortCertificateRead_)
         {
            tlsPortCertificate_ = FindManageSieveCertificateName_();
            tlsPortCertificateRead_ = true;
         }

         string certificate = tlsPortCertificate_;

         if (certificate == null)
         {
            return new WarningState
            {
               Level = StatusLevel.Information,
               Text = L("Whether STARTTLS will be offered could not be determined: the TCP/IP ports and certificates could not be read. This listener borrows its certificate from a TLS-capable IMAP, POP3 or SMTP port and has none of its own.")
            };
         }

         if (certificate.Length > 0)
         {
            // Deliberately "expected to be", not "will be". Everything checkable
            // from here has been checked - the certificate row exists and names
            // both files - but the listener only truly gets STARTTLS if
            // SslContextInitializer::InitServer can LOAD them at start-up, and a
            // file that has been moved, is unreadable by the service account, or
            // carries an encrypted key with no passphrase fails there. That
            // happens on the server, at start-up, and cannot be established by
            // reading configuration.
            return new WarningState
            {
               Level = StatusLevel.Good,
               Text = F("STARTTLS is expected to be offered, using the certificate '{0}' borrowed from a TLS-capable mailbox port - this listener has no certificate setting of its own, by design: the client editing Sieve filters is the client reading the mailbox, so the certificate it already trusts is the right one to present. Whether the files actually load is decided when the service starts; if they do not, the listener runs in plain text and says so in the application log, and the SSL certificates page checks the files themselves.", certificate)
            };
         }

         string missing = L("No TLS-capable IMAP, POP3 or SMTP port has a certificate assigned, so this listener will offer plain text only - ManageSieve authenticates with the mailbox password, which would then cross the network in the clear.");

         return new WarningState
         {
            Level = loopbackOnly ? StatusLevel.Information : StatusLevel.Warning,
            Text = loopbackOnly
               ? Sentences(missing, F("It is bound to {0}, which keeps it on this machine - that is the supported way to run it without TLS. Assign a certificate to a mailbox port to offer STARTTLS.", bind))
               : Sentences(missing, F("It is bound to {0}, so those passwords cross the network. Either assign a certificate to a TLS-capable mailbox port on the TCP/IP ports page, bind this listener to 127.0.0.1, or put it behind a TLS terminator.", bind))
         };
      }

      /// <summary>
      /// The name of the certificate ManageSieve would borrow: "" when no
      /// TLS-capable mailbox port has one, or null when the question could not be
      /// asked. Cached per page instance - it is two COM collection walks, and a
      /// computed warning runs on every keystroke.
      ///
      /// The preference order is IMAP, then POP3, then SMTP, matching
      /// ManageSieveServer::FindTlsCertificate_. Reporting a different one would be
      /// worse than reporting none: an administrator would check the wrong
      /// certificate's expiry and host names.
      /// </summary>
      private string tlsPortCertificate_;
      private bool tlsPortCertificateRead_;

      private string FindManageSieveCertificateName_()
      {
         // Usable, not merely referenced. FindTlsCertificate_ skips a port whose
         // SSLCertificateID resolves to no certificate row, and skips one whose
         // certificate has an empty certificate or private-key path - in both cases
         // it KEEPS LOOKING rather than giving up.
         //
         // The first of those is reachable in one step: deleting a certificate
         // issues only "delete from hm_sslcertificates" and never clears
         // portsslcertificateid on the ports referencing it, so a dangling id is
         // the ordinary result of removing a certificate that was in use. Matching
         // on id alone reported "STARTTLS will be offered, using the certificate
         // '#7'" for a certificate that no longer exists, while the server was
         // logging that it had none and running the listener in plain text - with
         // ManageSieve carrying mailbox passwords.
         var usableCertificateNames = new Dictionary<int, string>();

         dynamic certificates = null;
         dynamic ports = null;

         try
         {
            certificates = ServerSession.Current.Application.Settings.SSLCertificates;
            int certificateCount = (int)certificates.Count;
            for (int i = 0; i < certificateCount; i++)
            {
               dynamic certificate = certificates.Item[i];
               try
               {
                  string certificateFile = ((string)certificate.CertificateFile ?? "").Trim();
                  string privateKeyFile = ((string)certificate.PrivateKeyFile ?? "").Trim();

                  if (certificateFile.Length == 0 || privateKeyFile.Length == 0)
                     continue;

                  usableCertificateNames[(int)certificate.ID] = (string)certificate.Name ?? "";
               }
               finally
               {
                  ServerSession.Release((object)certificate);
               }
            }

            // Per protocol, the FIRST port the server would accept - which is not
            // the first port of that protocol. The server walks the ports in order
            // and skips the unusable ones, so keeping the first referenced id and
            // then finding it unusable would name a different certificate from the
            // one actually borrowed.
            var certificateIdByProtocol = new Dictionary<int, int>();

            ports = ServerSession.Current.Application.Settings.TCPIPPorts;
            int portCount = (int)ports.Count;
            for (int i = 0; i < portCount; i++)
            {
               dynamic port = ports.Item[i];
               try
               {
                  int security = (int)port.ConnectionSecurity;
                  int certificateId = (int)port.SSLCertificateID;

                  // Only a port that will actually perform a handshake has a
                  // certificate to lend: security 0 is plaintext.
                  if (security <= 0 || certificateId <= 0)
                     continue;

                  if (!usableCertificateNames.ContainsKey(certificateId))
                     continue;

                  int protocol = (int)port.Protocol;
                  if (!certificateIdByProtocol.ContainsKey(protocol))
                     certificateIdByProtocol[protocol] = certificateId;
               }
               finally
               {
                  ServerSession.Release((object)port);
               }
            }

            // The first protocol, in this order of preference, that lends a certificate.
            int? lendingCertificateId = new[] { ServerSession.SessionImap, ServerSession.SessionPop3, ServerSession.SessionSmtp }
               .Where(certificateIdByProtocol.ContainsKey)
               .Select(protocol => (int?)certificateIdByProtocol[protocol])
               .FirstOrDefault();
            if (lendingCertificateId.HasValue)
            {
               string name = usableCertificateNames[lendingCertificateId.Value];
               return name.Length > 0 ? name : "#" + lendingCertificateId.Value;
            }
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            return null;
         }
         finally
         {
            ServerSession.Release((object)ports);
            ServerSession.Release((object)certificates);
         }

         return "";
      }

      /// <summary>
      /// What an OAuth2 token actually has to satisfy to be accepted here.
      ///
      /// The first version of this method got the issuer and audience exactly
      /// backwards, and in the dangerous direction. It treated a blank
      /// OAuth2Issuer or OAuth2Audience as a fatal gap and told the administrator
      /// that "no client can log on with a token". OAuth2TokenValidator.cpp does
      /// the opposite: both checks sit behind `if (!config.issuer.IsEmpty())` and
      /// `if (!config.audience.IsEmpty())`, so a blank value means "do not check
      /// this at all". Blank is also the shipped default.
      ///
      /// So the configuration that warning fired on is not inert - it is
      /// PERMISSIVE. With both blank, any correctly-signed, unexpired token
      /// carrying the username claim is accepted, including one the identity
      /// provider minted for a completely different application. Describing that
      /// as a lockout hid the real risk behind an invented one, and an
      /// administrator acting on it would have typed a guess into a working
      /// configuration - where any value that does not exactly equal the token's
      /// own iss (or appear in its aud) then rejects every token there is.
      ///
      /// What genuinely stops every token, verified against the same file: no key
      /// material for the algorithms allowed (signature verification cannot run),
      /// and no username claim (the server has nothing to resolve to a mailbox).
      /// Those are the only two treated as blocking.
      /// </summary>
      private WarningState ComputeOAuth2State_()
      {
         if (!LiveBool_("OAuth2Enabled", false))
            return null;

         // The server implements exactly three algorithms, and nothing else. Reading
         // ValidateWithConfig: HS256, RS256 and - since 19 August 2026 - ES256 have
         // verifiers, and every other value falls into the final else and is refused
         // as unsupported. ES256 used to be refused by name, because JWS carries an
         // ECDSA signature as a raw R||S pair while OpenSSL wants X9.62 DER; the
         // transcode now exists, so this page must stop reporting it as unusable.
         //
         // A family test - "does the list contain RS" - is still not good enough:
         // RS512 and PS256 pass it and are refused by the server, and the page would
         // have called that configuration complete. The list is matched against the
         // three names that exist.
         string algorithmSetting = LiveText_("OAuth2AllowedAlgorithms", "RS256").Trim();

         var named = algorithmSetting
            .Split(new[] { ',', ' ', ';' }, StringSplitOptions.RemoveEmptyEntries)
            .Select(a => a.Trim().ToUpperInvariant())
            .Where(a => a.Length > 0)
            .ToList();

         bool allowsRs256 = named.Contains("RS256");
         bool allowsHs256 = named.Contains("HS256");
         bool allowsEs256 = named.Contains("ES256");
         List<string> unusable = named.Where(a => a != "RS256" && a != "HS256" && a != "ES256").ToList();

         var blocking = new List<string>();

         // RS256 and ES256 share OAuth2PublicKeyFile - one holds an RSA key, the other
         // an EC one, and the server checks that the key is actually the kind the
         // algorithm names rather than letting a mismatch surface as a signature
         // failure. Either being allowed makes the file mandatory.
         if ((allowsRs256 || allowsEs256) && LiveText_("OAuth2PublicKeyFile", "").Trim().Length == 0)
            blocking.Add(L("the issuer's public key file, which RS256 and ES256 tokens are verified against"));

         if (allowsHs256 && !SecretConfigured_("OAuth2HmacSecret"))
            blocking.Add(L("the shared HMAC secret, which HS256 tokens are verified against"));

         if (!allowsRs256 && !allowsHs256 && !allowsEs256)
         {
            blocking.Add(F("any algorithm this server can verify - it implements RS256, ES256 and HS256 only, and \"{0}\" names none of them, so every token is refused whatever key material is installed", algorithmSetting));
         }

         if (blocking.Count > 0)
         {
            return new WarningState
            {
               Level = StatusLevel.Warning,
               Text = F("OAuth2 is switched on but no client can log on with a token, because it is missing {0}. Signature verification cannot run at all, so every token is rejected and the log records only a failed logon - which reads as \"the password is wrong\" rather than as a configuration gap.", string.Join("; ", blocking))
            };
         }

         // A list that mixes a usable algorithm with unusable ones is not fatal -
         // tokens signed with the usable one still work - but a client the
         // identity provider configured for one of the others fails with nothing
         // on this page to explain why, so it is named.
         if (unusable.Count > 0)
         {
            return new WarningState
            {
               Level = StatusLevel.Warning,
               Text = F("Tokens signed with {0} are refused: this server implements RS256, ES256 and HS256 only. Remove what the server cannot verify from the list, so a client offering it is refused for a reason the log makes plain rather than appearing to be allowed.", JoinOr(unusable))
            };
         }

         // Signature verification works. What is left is how much a valid signature
         // is allowed to prove, and that is where the blanks matter.
         bool noIssuer = LiveText_("OAuth2Issuer", "").Trim().Length == 0;
         bool noAudience = LiveText_("OAuth2Audience", "").Trim().Length == 0;

         if (noIssuer || noAudience)
         {
            string unchecked_ = noIssuer && noAudience
               ? L("neither the issuer (iss) nor the audience (aud) is checked")
               : noIssuer ? L("the issuer (iss) is not checked") : L("the audience (aud) is not checked");

            return new WarningState
            {
               Level = StatusLevel.Warning,
               Text = Sentences(F("Tokens are accepted, but {0}: the server applies each of those only when you have set it, and blank is the default. That is wider than it looks. Any token your identity provider signs with this key is accepted - including one it issued to a different application entirely, which is enough to log in as whichever mailbox that token names. Set both to the exact values your provider puts in its tokens.", unchecked_),
                  LiveBool_("OAuth2RequireTLS", true)
                     ? null
                     : L("Tokens are also accepted over unencrypted connections, so a recorded one can be replayed until it expires."))
            };
         }

         if (!LiveBool_("OAuth2RequireTLS", true))
         {
            return new WarningState
            {
               Level = StatusLevel.Warning,
               Text = L("The configuration is complete, but tokens are accepted over connections that are not encrypted. A bearer token is a credential in plain text: anyone who records the connection can replay it until it expires. Turn TLS back on unless something in front of this server is already terminating it.")
            };
         }

         return new WarningState
         {
            Level = StatusLevel.Good,
            Text = L("Key material is present for the algorithms allowed, and both the issuer and the audience are checked, so a token has to have been minted by your provider for this server specifically. The mailbox it names still has to exist as a local account - a valid token for an address this server does not host is refused.")
         };
      }

      /// <summary>
      /// Strict dotted-quad parse, mirroring the inet_pton call in
      /// MetricsServer::Start - IPAddress.TryParse would accept "127.1" and IPv6,
      /// both of which the listener itself refuses.
      /// </summary>
      private static bool TryParseIpv4_(string text, out byte firstOctet)
      {
         firstOctet = 0;
         if (string.IsNullOrWhiteSpace(text))
            return false;

         string[] parts = text.Trim().Split('.');
         if (parts.Length != 4)
            return false;

         var octets = new byte[4];
         for (int i = 0; i < 4; i++)
         {
            if (parts[i].Length == 0 || parts[i].Length > 3 || !parts[i].All(char.IsDigit))
               return false;
            if (!byte.TryParse(parts[i], out octets[i]))
               return false;
         }

         firstOctet = octets[0];
         return true;
      }

      public FeatureSettingsView(Section section)
      {
         InitializeComponent();
         saveState_ = new IniPageSaveState(StatePill, StatusNotice, () => HasUnsavedChanges);
         section_ = section;
         BuildDefinition();
         BuildUi();
      }

      public void OnEnter()
      {
         // The page instance is cached by MainWindow, so re-read the INI from disk
         // on every navigation. This keeps the editors in sync with values changed
         // elsewhere (a prior save, another tool, or a hand edit) instead of showing
         // stale data captured when the page was first constructed.
         //
         // The same reasoning applies to what the computed warnings read from
         // outside the INI, which is cached per page instance so that it is not
         // re-read on every keystroke: the certificate a mailbox port lends to
         // ManageSieve, the account the service runs as, and the ACME pair on disk
         // can all have changed since this page was last open, and a warning
         // reasoning about a stale one is worse than no warning at all.
         ForgetExternalState_();

         // Every field's mark in one call (IniFeatureStore.LoadMarks), before the
         // rows that show them are built.
         IniFeatureStore.LoadMarks();

         BuildDefinition();
         BuildUi();
      }

      public void OnLeave()
      {
      }

      /// <summary>
      /// Whether any editor holds something other than what it was filled with or
      /// last saved. OnEnter reads the file again, so such an edit is gone when the
      /// page is next opened.
      /// </summary>
      public bool HasUnsavedChanges
         => store_.IsAvailable && cards_ != null && cards_.SelectMany(card => card.Settings).Any(setting => setting.IsChanged);

      /// <summary>The page's own Save; true when every write was made.</summary>
      public bool SaveChanges() => Save_();

      private void BuildDefinition()
      {
         cards_ = new List<CardDef>();

         switch (section_)
         {
            case Section.Security:
               Header.Title = L("Transport security");
               Header.Subtitle = L("Outbound mail authentication and encryption policies (hMailServer.INI).");
               cards_.Add(new CardDef
               {
                  Title = L("DANE & DNSSEC"),
                  Blurb = L("Validates recipient TLSA records with in-process DNSSEC and blocks delivery over forged chains (RFC 7672)."),
                  Settings =
                  {
                     new BoolSetting { Key = "DaneEnforcementEnabled", Default = true, Label = L("Honor recipient DANE/TLSA records when sending") },
                     new BoolSetting { Key = "DnssecValidationEnabled", Default = true, Label = L("Validate DNSSEC for DANE and SPF/DKIM/DMARC lookups") },
                     new TextSetting { Key = "DnssecTrustAnchors", Label = L("Trust anchor override (tag alg digesttype hex; ...)"), Placeholder = L("Leave empty for the built-in root anchors") }
                  }
               });
               cards_.Add(new CardDef
               {
                  Title = L("MTA-STS"),
                  Blurb = L("Discovers and enforces recipient MTA-STS policies before delivering over TLS (RFC 8461). To publish a policy for your own domains, see the Web services & autoconfiguration page."),
                  Settings =
                  {
                     new BoolSetting { Key = "MtaStsEnabled", Default = true, Label = L("Honor recipient MTA-STS policies when sending") }
                  }
               });
               cards_.Add(new CardDef
               {
                  Title = L("ARC sealing"),
                  // The prerequisite sentence is stated rather than checked here:
                  // whether a hosted domain has a DKIM selector and key file lives
                  // in the database, behind the COM API, and this page works from
                  // hMailServer.INI alone - it must keep telling the truth when no
                  // COM session exists. The Setup checklist page holds the live
                  // check and walks the actual domains.
                  Blurb = L("Adds ARC seals to forwarded mail so downstream servers can trust original authentication results (RFC 8617). Sealing borrows the forwarding domain's DKIM selector and private key, so it needs at least one hosted domain with DKIM signing configured - without that the switch reads enabled and seals nothing, with only a debug log line to say so. The Setup checklist page checks your domains for this."),
                  Settings =
                  {
                     new BoolSetting { Key = "ArcSealingEnabled", Default = false, Label = L("Seal forwarded messages with the domain's DKIM key") }
                  }
               });
               cards_.Add(new CardDef
               {
                  // An information-only card, deliberately without editors. The two
                  // values it describes live in the hm_settings database table (the
                  // anti-spam PropertySet, seeded at database version 6010), which
                  // nothing in this Control Panel can reach: IniFeatureStore reads
                  // and writes only hMailServer.INI, and the COM AntiSpam interface
                  // has no accessor for them, so the COM-path pattern the other
                  // anti-spam settings use cannot reach them either. An editor here
                  // that wrote INI keys of the same names would look configured and
                  // change nothing - the exact defect this page's blurbs exist to
                  // prevent - so until a real accessor exists, the card only tells
                  // the truth about where the switch is and what it needs.
                  Title = L("ARC inbound filtering"),
                  // These two are database settings rather than ini values, so they are not
                  // editable on this page - which is for hMailServer.INI. They ARE editable,
                  // on the Anti-spam page, and this card points there. It used to say they
                  // could not be edited at all, which stopped being true the moment the COM
                  // properties landed; a card that describes a limitation the product no
                  // longer has is the same defect as one that claims a feature it lacks.
                  Blurb = L("The counterpart to ARC sealing: on inbound mail, a valid ARC chain from a trusted sealer can offset the DMARC failure score for a message whose DMARC pass was destroyed by forwarding (RFC 8617). It is configured on the Anti-spam page, under sender authentication, because it is stored with the other anti-spam settings rather than in hMailServer.INI. With the trusted-sealer list empty the feature does nothing at all, by design: anyone can fabricate an entire ARC chain and seal it with keys published in their own DNS, and it will validate perfectly, so a passing chain proves nothing unless you already trust the sealer. The list is not an option of the feature - it is the feature. The offset never exceeds the DMARC failure score, and applies only while DMARC scoring is enabled on the Anti-spam page.")
               });
               cards_.Add(new CardDef
               {
                  Title = L("DKIM signature timestamps"),
                  Blurb = L("When a DKIM signature was made, and when it stops being one a verifier should honour (RFC 6376 3.5)."),
                  Settings =
                  {
                     new TextSetting
                     {
                        Key = "DKIMSignatureValiditySeconds",
                        Default = "0",
                        Placeholder = "604800",
                        Label = L("Validity window in seconds for signatures we produce (0 = no expiry)"),
                        Blurb = L("0 emits no expiry at all, which is the safe default: an expiry is a promise about mail already in flight, and a window shorter than the delay a retry, a greylist or a mailing list adds costs the message its DKIM pass and its DMARC alignment at the far end. 604800 is a week, the usual choice for a sender who wants one. A signing timestamp is always sent regardless of this value.")
                     },
                     new BoolSetting
                     {
                        Key = "DKIMEnforceSignatureExpiry",
                        Default = true,
                        Label = L("Refuse signatures on incoming mail whose expiry has passed"),
                        Blurb = L("The expiry sits inside the bytes the signature covers, so it is the sending domain's own instruction rather than something a third party can add. Turning this off means a captured signed message can be replayed indefinitely.")
                     },
                     new TextSetting
                     {
                        Key = "DKIMExpiryClockSkewSeconds",
                        Default = "300",
                        Placeholder = "300",
                        Label = L("Clock-drift tolerance in seconds when checking an expiry"),
                        Blurb = L("Allows for this server's clock differing from the signer's. Without it, a clock running a few minutes fast turns other people's valid mail into DKIM failures.")
                     }
                  }
               });
               cards_.Add(new CardDef
               {
                  // Defaults verified against IniFileSettings.cpp (empty = off) and
                  // DKIM::InitializeOversigning_ for the length cap, the invalid-name
                  // handling and the automatic From.
                  Title = L("DKIM oversigning"),
                  Blurb = L("Oversigning (RFC 6376 5.4) lists a header field name in the signature's h= tag once more often than the field occurs, which makes ADDING another one - a second From:, an injected Subject: - break the signature. Off by default: oversigning a field that a mailing list or forwarder legitimately adds costs those messages their DKIM pass."),
                  Settings =
                  {
                     new TextSetting
                     {
                        Key = "DkimOversignHeaders",
                        Label = L("Header fields to oversign in outbound DKIM signatures (comma separated, empty = off)"),
                        Placeholder = "From, Subject, Reply-To", // no-loc
                        Blurb = L("From is included automatically whenever this list is non-empty - a prepended second From: is the attack oversigning exists for. Names must be printable ASCII without a colon; an invalid name is dropped with an error log entry, and a value longer than 256 characters is ignored entirely, also with an error entry, because h= has to fit on one unfolded line.")
                     }
                  }
               });
               cards_.Add(new CardDef
               {
                  // No mention of SMTP AUTH verdicts here on purpose: the server's
                  // results carrier has an auth= slot, but nothing feeds it yet, so
                  // advertising it would be a capability claim with nothing behind it.
                  Title = L("Authentication results on inbound mail"),
                  Blurb = L("Records the verdicts this server itself reached about each inbound message - SPF, DKIM and DMARC - as trace headers on the delivered message, for downstream filters and diagnostics (RFC 8601, RFC 7208)."),
                  Settings =
                  {
                     new BoolSetting
                     {
                        Key = "AuthenticationResultsEnabled",
                        Default = false,
                        Label = L("Write an Authentication-Results header on inbound mail"),
                        Blurb = L("Each accepted inbound message gets one Authentication-Results header (RFC 8601) carrying the SPF, DKIM and DMARC verdicts this server reached about it. Only checks that actually ran are reported - which checks run is configured on the Anti-spam page - and a message on which no check ran gets no header. An arriving message that already carries an Authentication-Results header claiming this server's own identity has that header removed first, so a sender cannot have a verdict written in this server's name believed downstream (RFC 8601 section 5).")
                     },
                     new BoolSetting
                     {
                        Key = "ReceivedSpfHeaderEnabled",
                        Default = false,
                        Label = L("Write a Received-SPF header on inbound mail"),
                        Blurb = L("Records the SPF verdict for each accepted inbound message as a Received-SPF header (RFC 7208 section 9.1). The header is only written when the SPF check actually ran, so 'Check SPF' must be enabled on the Anti-spam page for it to appear.")
                     },
                     new TextSetting
                     {
                        Key = "AuthenticationResultsIdentity",
                        Label = L("Identity the results are written under (empty = this computer's name)"),
                        Placeholder = "mail.yourdomain.com",
                        Blurb = L("The authserv-id: the first token of every Authentication-Results header this server writes, always lower-cased, and the name a downstream filter checks before trusting the verdicts. The same name decides which arriving Authentication-Results headers are treated as forged: one claiming this identity is removed, while one naming any other identity is left completely untouched.")
                     }
                  }
               });
               cards_.Add(new CardDef
               {
                  Title = L("TLS reporting (TLS-RPT)"),
                  Blurb = L("Sends daily aggregate reports about TLS connection failures to recipient domains (RFC 8460)."),
                  Settings =
                  {
                     // Named "TLS report..." rather than "Report..." since DMARC
                     // reporting joined this page: two identically-labelled fields
                     // on one page are ambiguous to anyone who cannot see which
                     // card they are in, and AccessibleNamesTests fails on it.
                     new TextSetting { Key = "TlsRptFromAddress", Label = L("TLS report sender address (empty = disabled)"), Placeholder = "tlsrpt@yourdomain.com" },
                     new TextSetting { Key = "TlsRptOrganizationName", Default = "hMailServer", Label = L("Organization name in TLS reports") }
                  }
               });
               // The same shape as TLS reporting above and deliberately beside it:
               // both are reports this server sends to other domains about their
               // own mail, both are inert until a sender address is set, and an
               // administrator who has just switched one on is the person most
               // likely to want the other.
               cards_.Add(new CardDef
               {
                  Title = L("DMARC aggregate reporting (rua)"),
                  Blurb = L("Sends the daily aggregate reports that domains ask for with a rua= tag in their DMARC record (RFC 7489) - who sent mail claiming to be them, and whether it passed. Reports only go to an address outside the policy domain when that address's own DNS says it wants them, so a domain cannot use its DMARC record to aim this server's reports at somebody else."),
                  Settings =
                  {
                     new TextSetting { Key = "DmarcRptFromAddress", Label = L("DMARC report sender address (empty = disabled)"), Placeholder = "dmarc@yourdomain.com" },
                     new TextSetting { Key = "DmarcRptOrganizationName", Default = "hMailServer", Label = L("Organization name in DMARC reports") }
                  }
               });
               // Moved here from the catch-all INI page. This is the same subject as
               // the rest of this page - whether mail this server sends still passes
               // the recipient's authentication checks - and forwarding is the single
               // most common way a domain's SPF record starts failing for mail it
               // genuinely sent. The plain rewrite fallback comes with it, because
               // SRS silently replaces it and neither one can be judged alone.
               cards_.Add(new CardDef
               {
                  Title = L("Forwarded mail & bounce protection (SRS / BATV)"),
                  Blurb = L("Forwarding a message keeps the original envelope sender, so the next hop checks SPF for a domain that never authorised this server and the message fails. These are the two answers. SRS rewrites the envelope sender into one this server can vouch for and can undo on the way back, so bounces still reach the original sender; BATV tags the envelope sender of outbound mail so a forged bounce - one for a message this server never sent - can be told apart from a real one. Both use a server-wide secret and do nothing at all until one is set."),
                  Settings =
                  {
                     new BoolSetting
                     {
                        Key = "RewriteEnvelopeFromWhenForwarding",
                        Default = false,
                        Label = L("Rewrite the envelope sender when forwarding (the simple fallback, no secret needed)"),
                        Blurb = L("Replaces the envelope sender with the forwarding account's own address. That makes SPF pass at the next hop, at the cost of the original sender's address: a bounce comes back to the forwarding mailbox instead of to whoever wrote the message. SRS below does the same job without losing the return path, and while SRS is enabled this setting is not consulted at all.")
                     },
                     new BoolSetting { Key = "SRSEnabled", Default = false, Label = L("Enable Sender Rewriting Scheme (SRS) on forwarded mail") },
                     new SecretSetting { Key = "SRSSecret", OfferGenerate = true, Label = L("SRS signing secret"), Hint = L("A random server-wide secret") },
                     new BoolSetting { Key = "BATVEnabled", Default = false, Label = L("Tag outbound envelope senders with BATV and validate returning bounces") },
                     new SecretSetting { Key = "BATVSecret", OfferGenerate = true, Label = L("BATV signing secret"), Hint = L("A random server-wide secret") }
                  },
                  Warnings =
                  {
                     // Verified in SMTPForwarding.cpp: SRS::Forward returns nothing
                     // when the secret is empty, and the RewriteEnvelopeFrom...
                     // fallback sits in an "else if" behind SRSEnabled - so SRS-on
                     // with no secret rewrites nothing AND suppresses the fallback.
                     new WarningDef
                     {
                        Aid = "SrsSecretMissingWarning",
                        Compute = () =>
                        {
                           if (!LiveBool_("SRSEnabled", false) || SecretConfigured_("SRSSecret"))
                              return null;
                           return new WarningState
                           {
                              Level = StatusLevel.Warning,
                              Text = L("SRS is switched on but no secret is set, so no envelope sender is rewritten - and while SRS is on, the plain rewrite fallback at the top of this card is skipped as well. This is strictly worse than switching SRS off. Generate or enter a secret.")
                           };
                        }
                     },
                     new WarningDef
                     {
                        Aid = "BatvSecretMissingWarning",
                        Compute = () =>
                        {
                           if (!LiveBool_("BATVEnabled", false) || SecretConfigured_("BATVSecret"))
                              return null;
                           return new WarningState
                           {
                              Level = StatusLevel.Warning,
                              Text = L("BATV is switched on but no secret is set, so outbound senders are not tagged and returning bounces are not validated - the switch reads enabled while it does nothing. Generate or enter a secret.")
                           };
                        }
                     }
                  }
               });
               break;

            case Section.Automation:
               Header.Title = L("Automatic certificates (ACME)");
               Header.Subtitle = L("Built-in Let's Encrypt integration: certificates are issued, renewed, assigned to TLS ports and hot-reloaded automatically.");
               cards_.Add(new CardDef
               {
                  Title = L("ACME (Let's Encrypt)"),
                  Blurb = L("Issued certificates are stored in Data\\ACME and assigned to TLS ports without a restart. Key reuse keeps published DANE TLSA records valid across renewals."),
                  Settings =
                  {
                     new BoolSetting { Key = "AcmeEnabled", Default = false, Label = L("Issue and renew certificates automatically") },
                     new TextSetting { Key = "AcmeContactEmail", Label = L("Contact e-mail (CA expiry notices)"), Placeholder = "admin@yourdomain.com" },
                     new TextSetting { Key = "AcmeDomains", Label = L("Host names for the certificate (comma separated)"), Placeholder = "mail.yourdomain.com, mta-sts.yourdomain.com" }, // no-loc
                     new TextSetting { Key = "AcmeDirectoryUrl", Default = "https://acme-v02.api.letsencrypt.org/directory", Label = L("ACME directory URL") },
                     new TextSetting { Key = "AcmeHttpPort", Default = "80", Label = L("Port for http-01 challenges") },
                     new PathSetting { Key = "AcmeCertificateDirectory", PickFolder = true, Label = L("Certificate output folder (empty = Data\\ACME)"), Placeholder = L("Falls back to Data\\ACME") },
                     new BoolSetting { Key = "AcmeReuseKey", Default = true, Label = L("Reuse the private key across renewals (keeps DANE TLSA records valid)") }
                  },
                  Warnings =
                  {
                     // The whole point of automatic certificates is that nobody has
                     // to think about them, which is also why a renewal that stops
                     // working is silent: every control on this card still reads
                     // exactly as it did. The expiry of the file on disk is the one
                     // fact that distinguishes "working" from "was working".
                     new WarningDef
                     {
                        Aid = "AcmeCertificateState",
                        Compute = ComputeAcmeCertificateState_
                     }
                  }
               });
               break;

            case Section.Integration:
               Header.Title = L("API & monitoring");
               Header.Subtitle = L("REST administration API, Prometheus metrics and remote script management. The public web services listener (autoconfiguration, MTA-STS hosting) is on the Web services & autoconfiguration page; OAuth2 token authentication is on the Authentication page.");
               cards_.Add(new CardDef
               {
                  Title = L("REST administration API + Web Control Deck"),
                  Blurb = L("JSON API under /api/v1 plus the browser-based Control Deck at the listener root. Authenticated with the administrator password, or with a scoped API key - a key can be read-only, limited to named domains and source addresses, given an expiry and revoked on its own, none of which the administrator password can. Keys are managed on the REST API keys page. TLS is required unless the listener is bound to 127.0.0.1."),
                  Settings =
                  {
                     new ElsewhereSetting("apikeys", L("Creating and revoking API keys")),
                     new TextSetting { Key = "RestApiPort", Default = "0", Label = L("Port (0 = disabled)"), Placeholder = "8045" },
                     new TextSetting { Key = "RestApiBindAddress", Default = "127.0.0.1", Label = L("Bind address") },
                     new PathSetting { Key = "RestApiCertificateFile", FileFilter = L("PEM/certificate files (*.pem;*.crt;*.cer)|*.pem;*.crt;*.cer|All files (*.*)|*.*"), Label = L("TLS certificate file (PEM, optional)"), Placeholder = L("Falls back to the ACME certificate") },
                     new PathSetting { Key = "RestApiPrivateKeyFile", FileFilter = L("PEM/key files (*.pem;*.key)|*.pem;*.key|All files (*.*)|*.*"), Label = L("TLS private key file (PEM, optional)") }
                  }
               });
               cards_.Add(new CardDef
               {
                  Title = L("Monitoring"),
                  // This blurb has been wrong in BOTH directions. It once advertised
                  // an OpenTelemetry metrics export that did not exist - an
                  // administrator pointed a collector here, got nothing, and no error
                  // explained why. Then the metrics and logs exporters shipped on
                  // 21 Aug 2026 and the corrected wording immediately understated the
                  // truth instead. SettingClaimsTests scans this file: the withdrawn
                  // overclaim's wording must not come back verbatim, and the claim on
                  // each Otel*Endpoint setting is pinned separately. Keep this blurb
                  // in step with what Application::StartServers actually starts.
                  Blurb = L("Prometheus metrics (/metrics), OpenTelemetry export of traces, metrics and logs (one endpoint per signal, each off until set), a slow-query log, and JSON-structured log output for log aggregators (on the Logging page)."),
                  Settings =
                  {
                     new TextSetting { Key = "MetricsServerPort", Default = "0", Label = L("Metrics port (0 = disabled)"), Placeholder = "9090" },
                     new TextSetting
                     {
                        Key = "MetricsHistoryDays",
                        Default = "7",
                        Label = L("Keep metric history for (days; 0 = do not record)"),
                        Placeholder = "7",
                        Blurb = L("Once a minute the server writes one row per metric - sessions, messages processed, delivered, deferred and bounced, spam and viruses, authentication and TLS outcomes, missing message files - to hm_metricsamples, and keeps them this many days. It is what the dashboard's 24 hours, 7 days and 30 days views and GET /api/v1/metrics/history read. A week of minute-resolution history is about a hundred and forty thousand small rows. 0 turns the sampler off.")
                     },
                     new TextSetting
                     {
                        Key = "MetricsServerBindAddress",
                        Default = "127.0.0.1",
                        Label = L("Metrics bind address"),
                        Blurb = L("An IPv4 address, and the credential gate: anywhere in 127.0.0.0/8 the endpoints are open to this machine without authentication, exactly as before. On any other address /metrics answers 503 until a credential below is set - the exposition includes queue depth, session counts and version numbers, so it must not be network-readable unauthenticated.")
                     },
                     // Access control and TLS for the exposition. All five default to
                     // empty (verified in IniFileSettings.cpp), which on a loopback
                     // bind is the old behaviour exactly: plain unauthenticated HTTP.
                     new SecretSetting
                     {
                        Key = "MetricsServerAuthToken",
                        OfferGenerate = true,
                        Label = L("Bearer token for /metrics (empty = none)"),
                        Hint = L("A random token Prometheus will present on every scrape"),
                        Blurb = L("Presented as \"Authorization: Bearer ...\" - in Prometheus, the scrape config's bearer_token. Leading and trailing spaces are trimmed. The health probes /livez, /readyz and /healthz never require it, so load balancers keep working.")
                     },
                     new TextSetting
                     {
                        Key = "MetricsServerAuthUsername",
                        Label = L("HTTP Basic user name for /metrics (empty = Basic off)"),
                        Placeholder = "metrics",
                        Blurb = L("The alternative to the bearer token, for scrapers that only speak HTTP Basic. The user name and the password must BOTH be set: with only one of them the server logs a warning at startup and behaves as if neither were set.")
                     },
                     new SecretSetting
                     {
                        Key = "MetricsServerAuthPassword",
                        OfferGenerate = true,
                        Label = L("HTTP Basic password for /metrics"),
                        Hint = L("Only used together with the user name above"),
                        Blurb = L("The user name is trimmed of surrounding spaces; the password deliberately is not, because whitespace is legitimate inside a password.")
                     },
                     new PathSetting
                     {
                        Key = "MetricsServerCertificateFile",
                        FileFilter = L("PEM/certificate files (*.pem;*.crt;*.cer)|*.pem;*.crt;*.cer|All files (*.*)|*.*"),
                        Label = L("TLS certificate file for the metrics listener (PEM)"),
                        Placeholder = L("Leave both empty for plain HTTP")
                     },
                     new PathSetting
                     {
                        Key = "MetricsServerPrivateKeyFile",
                        FileFilter = L("PEM/key files (*.pem;*.key)|*.pem;*.key|All files (*.*)|*.*"),
                        Label = L("TLS private key file for the metrics listener (PEM)"),
                        Blurb = L("Certificate and key must BOTH be set to serve HTTPS; with only one of them TLS is NOT enabled and the server logs it. Unlike the REST API listener there is no fall-back to the ACME certificate here. If TLS is configured but cannot be prepared (an unreadable file, a key that does not match), the health probes stay on plain HTTP and /metrics answers 503 rather than serving the exposition in the clear.")
                     },
                     // JsonLogging moved to the Logging page, with the other log settings.
                     new TextSetting
                     {
                        Key = "OtelEndpoint",
                        Label = L("OpenTelemetry OTLP endpoint for traces (empty = disabled)"),
                        Placeholder = "http://localhost:4318",
                        Blurb = L(SettingClaims.NoteFor("OtelEndpoint"))
                     },
                     new TextSetting
                     {
                        Key = "OtelMetricsEndpoint",
                        Label = L("OpenTelemetry OTLP endpoint for metrics (empty = disabled)"),
                        Placeholder = "http://localhost:4318",
                        Blurb = L(SettingClaims.NoteFor("OtelMetricsEndpoint"))
                     },
                     new TextSetting
                     {
                        Key = "OtelLogsEndpoint",
                        Label = L("OpenTelemetry OTLP endpoint for logs (empty = disabled)"),
                        Placeholder = "http://localhost:4318",
                        Blurb = L(SettingClaims.NoteFor("OtelLogsEndpoint"))
                     },
                     new TextSetting
                     {
                        Key = "OtelMetricsInterval",
                        Default = "60",
                        Label = L("Seconds between metric pushes"),
                        Placeholder = "60",
                        Blurb = L("Clamped to 5-3600 by the exporter. Only used when the metrics endpoint above is set.")
                     },
                     new TextSetting { Key = "OtelServiceName", Default = "hmailserver", Label = L("OpenTelemetry service name") },
                     new TextSetting { Key = "SlowQueryLogMilliseconds", Default = "0", Label = L("Log database queries slower than N ms (0 = off)"), Placeholder = "250" }
                  },
                  Warnings =
                  {
                     // Behaviour verified in MetricsServer::Start: an invalid bind
                     // address is refused outright; a non-loopback bind without a
                     // credential serves 503 on /metrics; half a Basic pair and
                     // half a TLS pair are each discarded with a log line; and a
                     // credential without TLS crosses the network in clear text.
                     new WarningDef
                     {
                        Aid = "MetricsBindAddressInvalidWarning",
                        Compute = () =>
                        {
                           if (LiveInt_("MetricsServerPort", 0) <= 0)
                              return null;
                           if (TryParseIpv4_(LiveText_("MetricsServerBindAddress", "127.0.0.1"), out _))
                              return null;
                           return new WarningState
                           {
                              Level = StatusLevel.Warning,
                              Text = L("The metrics bind address is not a plain IPv4 address (host names and IPv6 are not accepted here), so the metrics listener will not start at all.")
                           };
                        }
                     },
                     new WarningDef
                     {
                        Aid = "MetricsNeedsCredentialWarning",
                        Compute = () =>
                        {
                           if (LiveInt_("MetricsServerPort", 0) <= 0)
                              return null;
                           if (!TryParseIpv4_(LiveText_("MetricsServerBindAddress", "127.0.0.1"), out byte firstOctet))
                              return null;
                           if (firstOctet == 127)
                              return null;

                           bool tokenSet = SecretConfigured_("MetricsServerAuthToken");
                           bool basicSet = LiveText_("MetricsServerAuthUsername", "").Trim().Length > 0
                                        && SecretConfigured_("MetricsServerAuthPassword");
                           if (tokenSet || basicSet)
                              return null;

                           return new WarningState
                           {
                              Level = StatusLevel.Warning,
                              Text = L("The bind address is not a loopback address and no credential is set, so /metrics answers 503 (Service Unavailable) to every scrape. Set a bearer token, or both HTTP Basic fields, or bind to 127.0.0.1. The health probes keep answering either way.")
                           };
                        }
                     },
                     new WarningDef
                     {
                        Aid = "MetricsHalfBasicWarning",
                        Compute = () =>
                        {
                           if (LiveInt_("MetricsServerPort", 0) <= 0)
                              return null;

                           bool userSet = LiveText_("MetricsServerAuthUsername", "").Trim().Length > 0;
                           bool passSet = SecretConfigured_("MetricsServerAuthPassword");
                           if (userSet == passSet)
                              return null;

                           return new WarningState
                           {
                              Level = StatusLevel.Warning,
                              Text = F("HTTP Basic needs both the user name and the password. Only {0} is set, so Basic authentication is NOT enabled - the server logs this and behaves as if neither were set.",
                                 userSet ? L("the user name") : L("the password"))
                           };
                        }
                     },
                     new WarningDef
                     {
                        Aid = "MetricsHalfTlsWarning",
                        Compute = () =>
                        {
                           if (LiveInt_("MetricsServerPort", 0) <= 0)
                              return null;

                           bool certSet = LiveText_("MetricsServerCertificateFile", "").Trim().Length > 0;
                           bool keySet = LiveText_("MetricsServerPrivateKeyFile", "").Trim().Length > 0;
                           if (certSet == keySet)
                              return null;

                           return new WarningState
                           {
                              Level = StatusLevel.Warning,
                              Text = F("TLS for the metrics listener needs both the certificate and the private key. Only {0} is set, so TLS is NOT enabled and scrapes stay plain HTTP.",
                                 certSet ? L("the certificate") : L("the private key"))
                           };
                        }
                     },
                     new WarningDef
                     {
                        Aid = "MetricsClearTextCredentialWarning",
                        Compute = () =>
                        {
                           if (LiveInt_("MetricsServerPort", 0) <= 0)
                              return null;
                           if (!TryParseIpv4_(LiveText_("MetricsServerBindAddress", "127.0.0.1"), out byte firstOctet))
                              return null;
                           if (firstOctet == 127)
                              return null;

                           bool tokenSet = SecretConfigured_("MetricsServerAuthToken");
                           bool basicSet = LiveText_("MetricsServerAuthUsername", "").Trim().Length > 0
                                        && SecretConfigured_("MetricsServerAuthPassword");
                           if (!tokenSet && !basicSet)
                              return null;

                           bool tlsSet = LiveText_("MetricsServerCertificateFile", "").Trim().Length > 0
                                      && LiveText_("MetricsServerPrivateKeyFile", "").Trim().Length > 0;
                           if (tlsSet)
                              return null;

                           return new WarningState
                           {
                              Level = StatusLevel.Warning,
                              Text = L("A credential is set but the listener has no TLS, so the credential crosses the network in clear text on every scrape and can be replayed by anyone on the path. Set the certificate and key files, or bind to 127.0.0.1. The server still starts - it logs this same warning.")
                           };
                        }
                     }
                  }
               });
               cards_.Add(new CardDef
               {
                  // Every key the live update reads is on this card. That is checked
                  // rather than maintained by hand: build/check-ini-coverage.py fails
                  // when a setting the server reads has no editor anywhere in the
                  // Control Panel, which is how the five trust settings below were
                  // found missing after the feature had shipped. The Status page has
                  // the buttons. Everything is off until the check is turned on, and
                  // the check sends nothing but the request.
                  Title = L("Updates"),
                  Blurb = L("The server can notice a new release, verify it against the release's own Sigstore signature, and apply it - by a click on the Status page, or on its own inside a window you set. Off until you turn the check on; the check sends nothing but the request."),
                  Settings =
                  {
                     new BoolSetting { Key = "UpdateCheckEnabled", Default = false, Label = L("Check for a new release once a day (UpdateCheckEnabled)") },
                     new TextSetting { Key = "UpdateCheckHours", Default = "24", Label = L("Hours between checks (1 to 168)"), Placeholder = "24" },
                     new TextSetting { Key = "UpdateChannel", Default = "stable", Label = L("Channel: stable, or prerelease to run ahead on a test machine"), Placeholder = "stable" },
                     new TextSetting { Key = "UpdateFeedUrl", Label = L("Update feed URL (empty = the built-in feed; set for a mirror)"), Placeholder = "https://updates.progressiverobot.com/releases.json" },
                     new TextSetting { Key = "HttpProxy", Label = L("Forward proxy for every web request this server makes - the feed, its downloads, JWKS, token introspection (host:port; empty = direct; no proxy credentials)"), Placeholder = "proxy.example.net:3128" },
                     new BoolSetting { Key = "UpdateAutoDownload", Default = false, Label = L("Download and verify the installer as soon as a release is found") },
                     new TextSetting
                     {
                        Key = "UpdateWindow",
                        Label = L("Unattended window (empty = never apply on its own; e.g. 03:00, Sun 03:00, Sat,Sun 02:00-05:00)"),
                        Placeholder = "Sun 03:00", // no-loc
                        Blurb = L("Inside the window the scheduled task applies a verified installer without a click: the service stops, files and schema upgrade, the service starts, and the outcome goes to the log, the Windows event log and the Status page. A service that does not come back is rolled back to the previous installer, unless the new version has already changed the database.")
                     },
                     new BoolSetting { Key = "UpdateBackupBeforeApply", Default = true, Label = L("Back up to the configured destination before an unattended apply (no destination or a failed backup means no apply)") },
                     new BoolSetting
                     {
                        Key = "UpdateRequireAuthenticode",
                        Default = false,
                        Label = L("Also require a valid Authenticode signature on the installer"),
                        Blurb = L("This project's installers have been Authenticode-signed since 6.3.1, by Progressive Robot Ltd through Azure Artifact Signing; the releases before 6.3.1 carry no signature, and this refuses them. It checks, when an installer is downloaded, that Windows trusts the signature - WinVerifyTrust against this machine's trusted roots, with no revocation check - and not who made it: that is the job of the Sigstore signature, which every release carries and which is checked either way.")
                     },
                     new TextSetting { Key = "UpdateServiceWaitSeconds", Default = "180", Label = L("Seconds to wait for the service after an apply before rolling back"), Placeholder = "180" },
                     new TextSetting
                     {
                        Key = "UpdateSourceRepository",
                        Label = L("Repository the signing certificate must name (empty = this project; - = do not check)"),
                        Placeholder = "https://gitlab.example.net/mail/hmailserver", // no-loc
                        Blurb = L("The last five settings on this card are for a site that builds its own releases and signs them with its own Sigstore, or that mirrors this project's. Leave every one of them empty and the server trusts only an installer signed by one of this project's two release workflows - the GitHub one for the releases up to 6.3.3, the GitLab one for the releases after - and recorded in the public transparency log. Setting any of them narrows or replaces that for both, and getting one wrong means either refusing genuine releases or trusting somebody else's.")
                     },
                     new TextSetting
                     {
                        Key = "UpdateSigningIdentity",
                        Label = L("Signing identity the certificate must begin with (empty = this project's two release workflows)"),
                        Placeholder = "https://gitlab.example.net/mail/hmailserver//.gitlab-ci.yml@refs/tags/v" // no-loc
                     },
                     new TextSetting
                     {
                        Key = "UpdateSigningIssuer",
                        Label = L("Identity provider that issued the signing certificate (empty = GitHub's or GitLab's)"),
                        Placeholder = "https://gitlab.example.net" // no-loc
                     },
                     new TextSetting
                     {
                        Key = "UpdateTrustRootsFile",
                        Label = L("Certificate authority file for a private Sigstore (PEM; empty = the public Fulcio roots)"),
                        Placeholder = "C:\\hMailServer\\Data\\fulcio-roots.pem" // no-loc
                     },
                     new TextSetting
                     {
                        Key = "UpdateLogPublicKeyFile",
                        Label = L("Transparency log's public key for a private Sigstore (PEM; empty = the public Rekor key)"),
                        Placeholder = "C:\\hMailServer\\Data\\rekor.pub" // no-loc
                     }
                  }
               });
               cards_.Add(new CardDef
               {
                  // Verified in WindowsEventLog.cpp: the sink hooks ErrorManager, not
                  // the Logger, so it fires independently of the log mask - which is
                  // the whole case for shipping it on. A healthy server writes zero
                  // events, and a per-id throttle (5 per 10 minutes) answers floods.
                  Title = L("Windows Event Log"),
                  Blurb = L("Writes the conditions an operator alerts on - database unavailable, a listener that would not start, a crash, a failed backup, the disk floor - to the Windows Application log, where Event Viewer, monitoring agents and SIEM collectors can see them. Stable event ids (the table lives in WindowsEventLog.h); a healthy server writes nothing at all. Protocol traffic and routine logging never go here - a mail server that logs every session to the Application log gets itself uninstalled."),
                  Settings =
                  {
                     new BoolSetting { Key = "WindowsEventLogEnabled", Default = true, Label = L("Write operational events to the Windows Application log") },
                     new ChoiceSetting
                     {
                        Key = "WindowsEventLogLevel",
                        Default = 2,
                        Label = L("Minimum severity that becomes an event"),
                        Options = new[]
                        {
                           (1, L("Critical only")),
                           (2, L("Critical and High (default - a healthy server writes nothing)")),
                           (3, L("Critical, High and Medium")),
                           (4, L("Everything ErrorManager reports"))
                        }
                     }
                  }
               });
               cards_.Add(new CardDef
               {
                  Title = L("ManageSieve (RFC 5804)"),
                  Blurb = L("Lets mail clients upload and manage per-account Sieve filter scripts over TCP. Authentication is SASL PLAIN against the account database, so the mailbox password crosses this connection. STARTTLS is offered only when a TLS-capable mailbox port has a certificate to borrow - the status below says whether it will be."),
                  Settings =
                  {
                     new TextSetting { Key = "ManageSieveServerPort", Default = "0", Label = L("ManageSieve port (0 = disabled)"), Placeholder = "4190" },
                     new TextSetting { Key = "ManageSieveServerBindAddress", Default = "127.0.0.1", Label = L("ManageSieve bind address") }
                  },
                  Warnings =
                  {
                     // This listener starts whether or not it can offer TLS, and
                     // records the difference in one application-log line at
                     // start-up. Nothing else anywhere said which of the two an
                     // administrator was about to get, and the answer decides
                     // whether mailbox passwords cross the network in the clear.
                     new WarningDef
                     {
                        Aid = "ManageSieveTlsState",
                        Compute = ComputeManageSieveTlsState_
                     }
                  }
               });
               cards_.Add(new CardDef
               {
                  Title = L("Operability"),
                  // Log retention lives on the Logging page, next to the other
                  // logging settings, so there is exactly one editor per key.
                  Blurb = L("Graceful shutdown behaviour for unattended / clustered operation. (Log retention is on the Logging page.)"),
                  Settings =
                  {
                     new TextSetting { Key = "ShutdownDrainSeconds", Default = "0", Label = L("On stop, wait up to N seconds for active sessions to finish (0 = stop immediately)"), Placeholder = "30" }
                  }
               });
               // Monitoring and integration that the server keeps in its database
               // rather than here, named where an administrator looks for it.
               cards_.Add(new CardDef
               {
                  Title = L("On other pages"),
                  Settings =
                  {
                     new ElsewhereSetting("logging", L("The SNMP agent, alerts, outbound webhooks and the audit trail")),
                     new ElsewhereSetting("webmail", L("What the webmail and the Control Deck offer: sign-in, sessions, Web Push"))
                  }
               });
               break;

            // "Advanced INI settings" filed by storage mechanism, which is the one
            // thing an administrator never knows and never needs to. Everything on
            // it that belonged to a feature with a page of its own has gone to that
            // page; what is left is genuinely server-wide, so the page is now named
            // for that rather than for the file the values happen to live in. The
            // nav key stays "hardening" and the old titles stay as search aliases,
            // so every existing link and bookmark still lands here.
            case Section.Hardening:
               Header.Title = L("Server limits & expert settings");
               Header.Subtitle = L("Server-wide ceilings, durability and abuse controls that belong to no single protocol or feature. The defaults are safe; change these only with a specific reason.");
               cards_.Add(new CardDef
               {
                  Title = L("Timeouts and queue bounds"),
                  Blurb = L("Server-wide ceilings that keep one slow operation from holding a resource forever. The defaults are deliberate; each one is here because there is a diagnosable situation in which it is the right thing to change. Note that 0 does not mean the same thing for all of them - each label says."),
                  Settings =
                  {
                     new TextSetting
                     {
                        Key = "FinalizationTimeout",
                        Default = "240",
                        Label = L("Message finalization deadline (seconds; 0 = no deadline)"),
                        Blurb = L("How long the server will go on finalizing an accepted message before answering 451 and asking the sender to retry, rather than holding the connection open indefinitely.")
                     },
                     new TextSetting { Key = "DNSQueryTimeout", Default = "10", Label = L("DNS query timeout (seconds; 0 = no bound)") },
                     new TextSetting { Key = "ClientSessionCeiling", Default = "1800", Label = L("Absolute lifetime of one client session (seconds)") },
                     new TextSetting { Key = "DBConnectionAcquireTimeout", Default = "60", Label = L("Wait for a free database connection (seconds)") },
                     new TextSetting { Key = "ScriptTimeout", Default = "60", Label = L("Event script execution timeout (seconds)") },
                     new TextSetting { Key = "ExternalProcessTimeout", Default = "300", Label = L("External process timeout, e.g. a command-line virus scanner (seconds)") },
                     new TextSetting { Key = "AsyncQueueStallThreshold", Default = "120", Label = L("Report the async work queue as stalled after (seconds)") },
                     new TextSetting { Key = "AsyncQueueReservedThreads", Default = "2", Label = L("Threads reserved so the async queue cannot be starved") }
                  }
               });
               // Scanner timeouts moved to the scanner they configure: SpamAssassin
               // on the Anti-spam page and ClamAV on the Anti-virus page. The
               // resolver settings moved to Network > DNS resolver.
               cards_.Add(new CardDef
               {
                  // Both of these let a peer tell this server where a connection
                  // "really" came from, which is why the wording leads with the
                  // trust list rather than the on/off switch. Verified in
                  // TCPServer::HandleAccept and SMTPConnection::XClientPermitted_:
                  // every trust decision is made against the real TCP peer, never
                  // against an address a header supplied, so chained proxies cannot
                  // bootstrap trust.
                  Title = L("Front-end proxies (PROXY protocol and XCLIENT)"),
                  Blurb = L("Put a load balancer, a TLS terminator or a Postfix relay in front of the SMTP listener and every connection appears to come from IT - which silently breaks DNSBL checks, SPF, greylisting, auto-ban and the IP range rules all at once, while everything still reports success. These let a named upstream pass on the real client address. Both are off by default and trust nobody until an address is listed, because a peer that can rewrite its own source address has defeated every IP-based control on this server. The upstream has to be configured to send it or nothing here changes: HAProxy needs send-proxy or send-proxy-v2 on its server line, and a Postfix relay needs XCLIENT enabled towards this host. hMailServer cannot check that from here - and note that once an address is listed as a PROXY protocol proxy the header becomes REQUIRED from it, so a proxy that does not send one will have its connections dropped."),
                  Settings =
                  {
                     new BoolSetting { Key = "SMTPProxyProtocolEnabled", Default = false, Label = L("Accept the PROXY protocol (v1 and v2) on the SMTP listener") },
                     new TextSetting
                     {
                        Key = "SMTPProxyProtocolTrustedIPs",
                        Label = L("Proxies allowed to send a PROXY header (comma-separated addresses or CIDR ranges)"),
                        Placeholder = "10.0.0.5, 192.168.10.0/24",
                        Blurb = L("Empty means nobody, which is the safe default. Matched against the real TCP peer. An entry that does not parse matches nothing rather than everything, and is reported in the error log once per run.")
                     },
                     new BoolSetting { Key = "SMTPXClientEnabled", Default = false, Label = L("Accept the XCLIENT command (Postfix)") },
                     new TextSetting
                     {
                        Key = "SMTPXClientTrustedIPs",
                        Label = L("Upstreams allowed to use XCLIENT (comma-separated addresses or CIDR ranges)"),
                        Placeholder = "10.0.0.6",
                        Blurb = L("Empty means nobody. XCLIENT is not even advertised in EHLO to an upstream that is not listed here, so an attacker learns nothing about the deployment by asking.")
                     }
                  }
               });
               cards_.Add(new CardDef
               {
                  Title = L("Received headers"),
                  Blurb = L("Submission identity handling and the diagnostic headers added to received mail. (Where SMTP AUTH is offered is on the Authentication page.)"),
                  Settings =
                  {
                     new TextSetting { Key = "AuthUserReplacementIP", Label = L("Replace the client IP for authenticated users (empty = keep the real IP)"), Placeholder = "e.g. 127.0.0.1" },
                     new BoolSetting { Key = "AddXAuthUserHeader", Default = false, Label = L("Add an X-AuthUser header with the authenticated account") },
                     new BoolSetting { Key = "AddXAuthUserIP", Default = true, Label = L("Include the client IP in the X-AuthUser header") },
                     new BoolSetting { Key = "AddXOriginalRcptTo", Default = false, Label = L("Add an X-OriginalRcptTo header") }
                  }
               });
               cards_.Add(new CardDef
               {
                  // Verified in TCPServer.cpp: the hold applies where a session could
                  // not be created at all - a non-matching IP range or the connection
                  // limit for that range - and nowhere else. It is not an auto-ban
                  // setting, and the blurb says which page each of those is on so
                  // nobody comes here looking for one and changes this instead.
                  Title = L("Refused connections"),
                  Blurb = L("What happens to a TCP connection this server refuses before any protocol conversation starts: one from an address no IP range allows, or one over that range's connection limit. Both of those are configured on the IP ranges page. Locking out an address that keeps failing to log on is a different mechanism entirely and is on the Auto-ban page."),
                  Settings =
                  {
                     new TextSetting
                     {
                        Key = "BlockedIPHoldSeconds",
                        Default = "0",
                        Label = L("Hold a refused connection open before dropping it (seconds, 0 = drop immediately)"),
                        Blurb = L("Anti-pounding: a host that reconnects the instant it is dropped can do so thousands of times a minute, and holding the socket open slows it to one connection per interval without costing this server a thread. The connection is held by a timer, so it consumes nothing while it waits.")
                     }
                  }
               });
               cards_.Add(new CardDef
               {
                  Title = L("Message store durability"),
                  Blurb = L("Crash-durability barrier and an optional integrity scan for the on-disk message store. The defaults preserve the previous behaviour; enabling fsync adds a small per-message cost."),
                  Settings =
                  {
                     new BoolSetting { Key = "MessageStoreFsync", Default = false, Label = L("Flush each received message to disk before it is acknowledged (durable, slower)") },
                     new BoolSetting { Key = "MessageStoreConsistencyCheck", Default = false, Label = L("Periodically cross-check message rows against files on disk (read-only; writes a report on divergence)") }
                  }
               });
               cards_.Add(new CardDef
               {
                  // Defaults and the reload behaviour verified in RateLimiter.cpp:
                  // LoadSettings_ reads [SendingLimits]/[SendingLimitsOverrides]
                  // directly and MaybeRefreshSettings_ stats the INI every couple
                  // of seconds, so unlike everything else on this page these apply
                  // without a service restart.
                  Title = L("Per-account sending limits"),
                  Blurb = L("Ceilings on what one authenticated account may submit over a rolling period - the brake on a compromised account being used to spam. Counted per account at SMTP submission: messages and envelope recipients separately, and a message refused by the limit gets a temporary error so a real client retries later. Unlike the rest of this page these live in the [SendingLimits] and [SendingLimitsOverrides] sections of hMailServer.INI and are re-read within a couple of seconds of the file changing - saving here applies them WITHOUT a service restart. Counters survive a restart via a state file in the data directory."),
                  Settings =
                  {
                     new SectionTextSetting
                     {
                        Key = "MaxMessagesPerAccountPerPeriod",
                        Section = "SendingLimits",
                        Default = "0",
                        Label = L("Max messages per account per period (0 = no limit)"),
                        Placeholder = "500"
                     },
                     new SectionTextSetting
                     {
                        Key = "MaxRecipientsPerAccountPerPeriod",
                        Section = "SendingLimits",
                        Default = "0",
                        Label = L("Max recipients per account per period (0 = no limit)"),
                        Placeholder = "2000",
                        Blurb = L("Recipients are the stricter measure: one message to two thousand addresses is two thousand recipients.")
                     },
                     new SectionTextSetting
                     {
                        Key = "PeriodHours",
                        Section = "SendingLimits",
                        Default = "24",
                        Label = L("Period length in hours (default 24)"),
                        Blurb = L("Clamped to 1-168 by the server: a value outside that range is not an error, it is quietly pulled to the nearest bound.")
                     },
                     new SectionTextSetting
                     {
                        Key = "StateSaveIntervalSeconds",
                        Section = "SendingLimits",
                        Default = "10",
                        Label = L("Save the counters to disk every N seconds (default 10)"),
                        Blurb = L("How much sending history a crash can forget. Clamped to 1-3600; a value below 1 falls back to the default of 10.")
                     },
                     new SectionLinesSetting
                     {
                        Key = "SendingLimitsOverrides",
                        Section = "SendingLimitsOverrides",
                        Label = L("Per-address overrides, one per line: address=messages:recipients[:hours]"),
                        Placeholder = "newsletter@yourdomain.com=5000:50000\nceo@yourdomain.com=0:0",
                        Blurb = L("An override replaces the global ceilings for that address; 0:0 exempts it entirely. Without the optional :hours the override uses the global period. A malformed line is ignored with a log entry and the global limit still applies to that account. Comment lines in this INI section are not shown here and are dropped if the list is saved.")
                     }
                  }
               });
               cards_.Add(new CardDef
               {
                  Title = L("Submission rate limits"),
                  Blurb = L("Throttle abusive senders submitting to this server. Limits are per minute; 0 disables the limit. (Throttling your own outbound rate to a destination is on the Delivery of e-mail page.)"),
                  Settings =
                  {
                     new TextSetting { Key = "MaxSubmissionsPerIPPerMinute", Default = "0", Label = L("Max authenticated submissions per client IP per minute (0 = unlimited)"), Placeholder = "60" }
                  }
               });
               // Logging detail moved to the Logging page, indexer cadence to
               // Performance > Indexing and message archiving to Advanced &
               // scripting (next to the mirroring address), so each setting sits
               // with the feature it configures rather than on this catch-all page.
               cards_.Add(new CardDef
               {
                  Title = L("Server-generated mail"),
                  // The server builds mailer-daemon@<domain> from this and puts it
                  // in the From: header of bounces and virus notices; it has no
                  // effect on how or where mail is delivered.
                  Blurb = L("Bounces and virus notifications are sent from mailer-daemon@<domain>. This overrides the <domain> part. Left empty, the server uses its own host name, then the local domain the message involves, then this computer's name. It is not a delivery setting and does not change where mail is routed."),
                  Settings =
                  {
                     new TextSetting { Key = "DaemonAddressDomain", Label = L("Domain for the mailer-daemon sender address (empty = host name, else the message's local domain, else the machine name)"), Placeholder = "yourdomain.com" }
                  }
               });
               cards_.Add(new CardDef
               {
                  Title = L("Low-level tuning"),
                  Blurb = L("Specialist knobs — leave at the defaults unless you have a specific reason."),
                  Settings =
                  {
                     new TextSetting { Key = "SMTPDMaxSizeDrop", Default = "0", Label = L("Drop oversized inbound messages above N bytes mid-transfer (0 = off)") },
                     // SAMoveVsCopy went to Anti-spam > SpamAssassin: it configures
                     // how a message reaches that scanner, and belongs with the host,
                     // port and timeouts that configure the rest of the same handoff.
                     new TextSetting { Key = "LoadHeaderReadSize", Default = "4000", Label = L("Header read chunk size (bytes)") },
                     new TextSetting { Key = "LoadBodyReadSize", Default = "4000", Label = L("Body read chunk size (bytes)") }
                  }
               });
               cards_.Add(new CardDef
               {
                  // The blurb this replaces claimed that "sensitive values in
                  // hMailServer.INI (database password, OAuth/SRS/BATV secrets,
                  // password pepper) are encrypted with Windows DPAPI on the next
                  // service start". Read against the source, that was wrong three
                  // times over: nothing rewrites hMailServer.INI at start-up at
                  // all; Crypt::ProtectSecret is called only from Property::Save,
                  // PersistentRoute, PersistentFetchAccount and
                  // PersistentSSLCertificate, all of which write to the DATABASE;
                  // and OAuth2HmacSecret, SRSSecret, BATVSecret and PasswordPepper
                  // are each read with a plain ReadIniSettingString_, so this
                  // switch has never touched one of them. The database password is
                  // the only INI value it governs, and only at the moment it is
                  // set, through IniFileSettings::SetPassword. Since 6.3.4 it
                  // governs a second kind as well: the [Settings] secrets
                  // (IniSettingStore::IsProtectedName - SRSSecret, BATVSecret,
                  // the OAuth2 and metrics secrets, ServiceAccountPassword,
                  // GssapiServicePassword) and an account's second factor are
                  // sealed under the stored-secrets key while it is on, and
                  // opened back at the next start when it is off
                  // (docs/SecretsAtRest.md). PasswordPepper alone stays as
                  // written, so the field's blurb names both kinds and the pepper.
                  Title = L("Stored secret protection"),
                  // "Turn it off to restore a backup onto a different machine" was
                  // the one reason this card gave, and it was misdirection: no
                  // backed-up secret is in a DPAPI envelope. Configuration::XMLStore
                  // writes the relayer password in clear (PropertySet decrypts at
                  // Refresh), and Route, FetchAccount and SSLCertificate hard-code
                  // Blowfish in both directions precisely so that backups restore
                  // onto replacement hardware; restore then re-envelopes under the
                  // destination machine's own setting. So a cross-machine restore
                  // works with this left on, and following the old advice downgraded
                  // every future secret write to a key that ships in the source.
                  Blurb = L("Chooses the envelope hMailServer puts around the secrets it stores for its own use: machine-scoped Windows DPAPI, which cannot be decrypted on any other machine, or the legacy Blowfish scheme, which can. Leave it on. It does not affect backup and restore - nothing in a backup archive is DPAPI-protected, and a restore re-protects each secret under the destination machine's own setting - so a backup taken here restores onto another machine with this switched on."),
                  Settings =
                  {
                     new BoolSetting
                     {
                        Key = "ProtectStoredSecretsWithDPAPI",
                        Default = true,
                        Label = L("Protect stored secrets with Windows DPAPI"),
                        Blurb = L("It governs two kinds of stored secret. Those bound to this machine - the database password in hMailServer.INI, the SMTP relayer password, and the passwords, tokens and passphrases of routes, external fetch accounts, SSL certificates and the rest kept in the database - are put in a Windows DPAPI envelope when they are next saved, or in the legacy Blowfish scheme while this is switched off. The secrets in the [Settings] section - the SRS and BATV secrets, the OAuth2 secrets, the metrics bearer token and HTTP Basic password, the Windows service account password and the GSSAPI service password - and each account's second factor are sealed under the installation's stored-secrets key once it has one, at the next start and at every save; switched off, the next start stores them as written again. Only the password pepper is always stored as you typed it, so hMailServer.INI should still be readable only by Administrators and by the account the service runs as.")
                     }
                  }
               });
               cards_.Add(new CardDef
               {
                  // The clearest case on this page of the rule that a setting whose
                  // effect happens outside the program has to say so. The account
                  // is the Service Control Manager's, so the card reads it from
                  // there, and changes it with buttons that run the registration
                  // itself, elevated, with the account on its command line - the
                  // only place /Register takes it from (docs/ServiceAccount.md).
                  Title = L("Windows service account"),
                  Blurb = L("Which Windows account the hMailServer service logs on as. A new installation uses NT SERVICE\\hMailServer, a virtual account of its own that needs no password and reaches only the folders it is granted; an installation upgraded from an earlier version keeps LocalSystem, the most privileged account on the machine, until it is moved - and under LocalSystem a flaw reachable through SMTP, IMAP or POP3 would have full control of the computer."),
                  Settings =
                  {
                     // No ServiceAccountName or ServiceAccountPassword editor any
                     // more. /Register took the account from them; it takes it
                     // from its command line now, which these buttons pass, and
                     // an account with a password is set in services.msc
                     // (docs/ServiceAccount.md). The keys are still read, and a
                     // stored password is raised on the Setup checklist page.
                     new ServiceAccountActionSetting
                     {
                        Does = ServiceAccountActionSetting.Action.Grant,
                        ButtonText = L("Grant access…"),
                        AutomationId = "ServiceAccountGrant",
                        Label = L("Folders the service account needs"),
                        Blurb = L("Runs hMailServer.exe /GrantServiceAccess as an administrator, after Windows asks: NT SERVICE\\hMailServer is given Modify on the program folder's own Data, Logs, Temp and Database folders and on the folders an administrator has approved, and Read on the program folder - never hMailServer.INI. A folder the configuration names elsewhere, such as a backup or archive folder set later, is approved only after a question that names it; key files outside those folders are listed. It can be run again at any time, and removes nobody else's permission.")
                     },
                     new ServiceAccountActionSetting
                     {
                        Does = ServiceAccountActionSetting.Action.MoveToOwnAccount,
                        ButtonText = L("Move to NT SERVICE\\hMailServer…"),
                        AutomationId = "ServiceAccountMoveToOwn",
                        Label = L("Run the service as its own account"),
                        Blurb = L("Grants the folders as Grant access does - asking about any folder or key file the configuration names outside them - and registers the service as NT SERVICE\\hMailServer only if that ends cleanly. The service keeps its old account until it is restarted.")
                     },
                     new ServiceAccountActionSetting
                     {
                        Does = ServiceAccountActionSetting.Action.MoveToLocalSystem,
                        ButtonText = L("Move to LocalSystem…"),
                        AutomationId = "ServiceAccountMoveToLocalSystem",
                        Label = L("Run the service as LocalSystem"),
                        Blurb = L("Registers the service as LocalSystem again, which installing a live update from the server or writing firewall rules for the auto-ban needs. The service keeps its old account until it is restarted.")
                     }
                  },
                  Warnings =
                  {
                     new WarningDef
                     {
                        Aid = "ServiceAccountStatus",
                        Compute = ComputeServiceAccountState_
                     },
                     new WarningDef
                     {
                        Aid = "ServiceAccountGrants",
                        Compute = () =>
                        {
                           // Only while the service runs as an account of its own:
                           // for LocalSystem none of it applies, and a permanent
                           // notice about work that is not needed is noise.
                           if (!ServerSession.IsLocalSession)
                              return null;
                           WindowsServiceInfo service = serviceInfo_ ??= WindowsServiceInfo.Query();
                           if (!service.Exists || string.Equals(WindowsServiceInfo.Canonical(service.StartName, Environment.MachineName), "localsystem", StringComparison.OrdinalIgnoreCase))
                              return null;

                           return new WarningState
                           {
                              Level = StatusLevel.Information,
                              Text = L("What an account of its own changes. hMailServer.INI is read-only to it, on purpose: the administrator password, its second factor, the folders and the database are changed as the Control Panel says when you try, and settings are kept in the database as always. A folder set later outside the program folder - a backup or archive folder - is reached once Grant access approves it. An account other than NT SERVICE\\hMailServer is set in services.msc and needs the \"Log on as a service\" right; an external database with Windows authentication needs a login for it. A service that cannot log on reports error 1069 in the Windows event log and does not start.")
                           };
                        }
                     }
                  }
               });
               cards_.Add(new CardDef
               {
                  Title = L("Settings that used to be on this page"),
                  Blurb = L("This page used to collect every hMailServer.INI value without a home, which meant it was filed by where the value is stored rather than by what it does - and where a value is stored is the one thing you never need to know. Each of these now sits with the rest of its own feature. Nothing was removed and no value changed; only the page it is edited on."),
                  Settings =
                  {
                     new ElsewhereSetting("antispam", L("Greylisting record expiration, and SpamAssassin move-vs-copy")),
                     new ElsewhereSetting("protocols", L("IMAP search time and size limits, and the eight per-protocol idle timeouts")),
                     new ElsewhereSetting("tls", L("TLS session tickets, session cache size, resumption lifetime and ticket-key rotation")),
                     new ElsewhereSetting("delivery", L("Extra delivery attempts per additional MX host")),
                     new ElsewhereSetting("performance", L("Max parallel external POP3 fetch threads")),
                     new ElsewhereSetting("security", L("SRS, BATV and the plain envelope-sender rewrite for forwarded mail"))
                  }
               });
               break;

            case Section.Authentication:
               Header.Title = L("Authentication");
               Header.Subtitle = L("How mailbox users prove who they are: external identity providers, how passwords are stored, and where SMTP AUTH is offered (hMailServer.INI).");
               cards_.Add(new CardDef
               {
                  Title = L("OAuth2 / external identity provider"),
                  Blurb = L("Accept OAuth2 / OpenID Connect bearer tokens (XOAUTH2) from an external identity provider for IMAP, POP3 and SMTP submission, validated against the issuer's signing key. The mailbox named by the token still has to exist as a local account."),
                  Settings =
                  {
                     new BoolSetting { Key = "OAuth2Enabled", Default = false, Label = L("Accept OAuth2 bearer tokens (XOAUTH2)") },
                     new BoolSetting { Key = "OAuth2RequireTLS", Default = true, Label = L("Require TLS for token authentication") },
                     new TextSetting { Key = "OAuth2Issuer", Label = L("Expected token issuer (iss)"), Placeholder = "https://login.microsoftonline.com/<tenant>/v2.0" },
                     new TextSetting { Key = "OAuth2Audience", Label = L("Expected audience (aud)"), Placeholder = L("your application / client id") },
                     new TextSetting { Key = "OAuth2AllowedAlgorithms", Default = "RS256", Label = L("Allowed signing algorithms (comma separated)"), Placeholder = "RS256, ES256" }, // no-loc
                     new TextSetting { Key = "OAuth2UsernameClaim", Default = "email", Label = L("Claim that holds the mailbox address"), Placeholder = "email" },
                     new PathSetting { Key = "OAuth2PublicKeyFile", FileFilter = L("PEM/key files (*.pem;*.crt;*.cer;*.key;*.pub)|*.pem;*.crt;*.cer;*.key;*.pub|All files (*.*)|*.*"), Label = L("RSA/EC public key file (PEM, for RS*/ES* tokens)"), Placeholder = L("Path to the issuer's public key") },
                     new SecretSetting { Key = "OAuth2HmacSecret", Label = L("Shared HMAC secret (only for HS256/384/512 tokens)"), Hint = L("Only needed for HS* algorithms") },
                     // The two round trips to the provider, both off unless set: its
                     // published keys instead of a hand-copied PEM file, and a live
                     // revocation check after a token verifies.
                     new TextSetting { Key = "OAuth2JwksUrl", Label = L("JWK Set URL (the provider's published signing keys; empty = the key file only)"), Placeholder = "https://login.microsoftonline.com/<tenant>/discovery/v2.0/keys" },
                     new TextSetting { Key = "OAuth2JwksCacheSeconds", Default = "3600", Label = L("Seconds to keep the JWK Set before re-fetching (a token naming an unknown key id re-fetches at once)") },
                     new TextSetting { Key = "OAuth2IntrospectionUrl", Label = L("Token introspection endpoint (RFC 7662; empty = no revocation check)"), Placeholder = "https://idp.example.com/oauth2/introspect" },
                     new TextSetting { Key = "OAuth2IntrospectionClientId", Label = L("Client id this server introspects as") },
                     new SecretSetting { Key = "OAuth2IntrospectionClientSecret", Label = L("Client secret for introspection"), Hint = L("Issued by the provider together with the client id") },
                     new TextSetting { Key = "OAuth2IntrospectionCacheSeconds", Default = "300", Label = L("Seconds an introspection verdict is reused (never past the token's own expiry)") },
                     new BoolSetting { Key = "OAuth2IntrospectionFailOpen", Default = false, Label = L("Accept a token when the introspection endpoint cannot answer (availability over the revocation check)") }
                  },
                  Warnings =
                  {
                     // Seven settings that only work as a set, and no way to see
                     // whether the set is complete. Every one of them missing
                     // produces the same symptom - a token that is rejected and a
                     // log line saying only that a logon failed - so an incomplete
                     // configuration reads to the administrator, and to the user
                     // on the phone, as "the password is wrong".
                     new WarningDef
                     {
                        Aid = "OAuth2CoherenceState",
                        Compute = ComputeOAuth2State_
                     }
                  }
               });
               cards_.Add(new CardDef
               {
                  Title = L("Password storage"),
                  Blurb = L("How account passwords are hashed, how much work a new hash costs, and the weakest stored hash still allowed to log on. Existing passwords keep their current hash until they are next changed - or, when a work factor was raised, until they are next used to log on."),
                  Settings =
                  {
                     new ChoiceSetting
                     {
                        Key = "PreferredHashAlgorithm",
                        Default = 4,
                        Label = L("Password hash for new or changed account passwords"),
                        Options = new (int, string)[]
                        {
                           (5, L("Argon2id (memory-hard, recommended)")),
                           (7, L("scrypt (memory-hard, RFC 7914)")),
                           (4, L("PBKDF2 (default)")),
                           (3, L("SHA-256")),
                           (2, L("MD5 (legacy)")),
                           (1, L("Blowfish (legacy)"))
                        }
                     },
                     new ChoiceSetting
                     {
                        Key = "MinimumAcceptedHashAlgorithm",
                        Default = 0,
                        Label = L("Reject logins using a weaker stored hash than"),
                        Options = new (int, string)[]
                        {
                           (0, L("Accept any stored hash")),
                           (3, L("SHA-256 or stronger")),
                           (4, L("PBKDF2 or stronger")),
                           (5, L("Argon2id or scrypt only"))
                        }
                     },
                     // A plain caption and a warning under the field that says what a
                     // change does, and a question at Save (PepperChange): the caption
                     // that shouted about it was the only safeguard (review finding F169).
                     new SecretSetting { Key = "PasswordPepper", Label = L("Password pepper"), Hint = L("Server-wide secret mixed into password hashes"), Warning = PepperChange.Warning },
                     new TextSetting { Key = "PasswordHashIterations", Default = "0", Placeholder = "0 = 210,000", Label = L("PBKDF2 iterations for new hashes (0 = the built-in 210,000; 10,000 to 10,000,000)") },
                     new TextSetting { Key = "PasswordHashMemoryKB", Default = "0", Placeholder = "0 = 19,456", Label = L("Argon2id memory for new hashes, in KiB (0 = the built-in 19,456; 4,096 to 1,048,576)") },
                     new TextSetting { Key = "PasswordHashTimeCost", Default = "0", Placeholder = "0 = 2", Label = L("Argon2id passes for new hashes (0 = the built-in 2; 1 to 20)") }
                  }
               });
               cards_.Add(new CardDef
               {
                  Title = L("SMTP authentication"),
                  Blurb = L("AUTH is normally offered on every SMTP port. List the local TCP ports where it should not be, for example a port 25 that only accepts inbound mail from other servers."),
                  Settings =
                  {
                     new TextSetting { Key = "DisableAUTHList", Label = L("Do not offer AUTH on these local TCP ports (comma separated)"), Placeholder = "25" }
                  }
               });
               // How people sign in to the browser pages is the Webmail page's: those
               // settings are kept in the server's database, not in this file.
               cards_.Add(new CardDef
               {
                  Title = L("Sign-in to the webmail and the Control Deck"),
                  Settings =
                  {
                     new ElsewhereSetting("webmail", L("Passkeys, single sign-on with OpenID Connect or SAML, and account recovery"))
                  }
               });
               break;

            case Section.Dns:
               Header.Title = L("DNS resolver");
               Header.Subtitle = L("How this server resolves MX, PTR, SPF, DKIM, DMARC and block list lookups (hMailServer.INI).");
               cards_.Add(new CardDef
               {
                  Title = L("Name servers"),
                  Blurb = L("Which resolver hMailServer queries. Leave empty to use the name servers Windows is configured with. The override takes a single IPv4 address and is used on port 53; DNSSEC validation and its trust anchors are on the Transport security page."),
                  Settings =
                  {
                     new TextSetting { Key = "DNSServer", Label = L("Override DNS server (empty = the name servers Windows uses)"), Placeholder = "1.1.1.1" }
                  }
               });
               cards_.Add(new CardDef
               {
                  Title = L("DNS cache"),
                  // Corrected 2026-08-15: the previous text described an in-process cache that
                  // keeps answers "in memory for their TTL". There is no such cache anywhere in
                  // the server. The setting toggles DNS_QUERY_BYPASS_CACHE on the DnsQueryEx
                  // call - i.e. whether WINDOWS' resolver cache is consulted - and a custom DNS
                  // server forces the bypass regardless, because the system cache would answer
                  // from the wrong resolver.
                  Blurb = L("Whether lookups may be answered from the Windows DNS resolver cache. Off, every lookup goes to the network. Ignored when an override DNS server is set above — those lookups always bypass the system cache, because its answers would come from the wrong resolver."),
                  Settings =
                  {
                     new BoolSetting { Key = "UseDNSCache", Default = true, Label = L("Answer from the Windows DNS resolver cache when possible") }
                  }
               });
               cards_.Add(new CardDef
               {
                  Title = L("DNS block list checks"),
                  Blurb = L("When during the SMTP conversation block list lookups happen. Which block lists are asked is on the DNS block lists (DNSBL) page."),
                  Settings =
                  {
                     new BoolSetting { Key = "DNSBLChecksAfterMailFrom", Default = true, Label = L("Run DNSBL checks after MAIL FROM (rather than at connect)") }
                  }
               });
               break;

            case Section.WebServices:
               Header.Title = L("Web services & client autoconfiguration");
               Header.Subtitle = L("The built-in HTTP listener that serves mail-client autoconfiguration and MTA-STS policies for your local domains (hMailServer.INI).");
               cards_.Add(new CardDef
               {
                  Title = L("Listener"),
                  Blurb = L("Nothing below is served until a port is set here. The server answers on either port, but an MTA-STS policy is only valid over HTTPS and Outlook only accepts autodiscover over HTTPS — so in practice set the HTTPS port and a certificate covering the host names clients use."),
                  Settings =
                  {
                     new TextSetting { Key = "WebServicesHttpPort", Default = "0", Label = L("HTTP port (80 to enable, 0 = disabled)") },
                     new TextSetting { Key = "WebServicesHttpsPort", Default = "0", Label = L("HTTPS port (443 to enable, 0 = disabled)") },
                     new TextSetting { Key = "WebServicesBindAddress", Default = "0.0.0.0", Label = L("Bind address") },
                     new TextSetting { Key = "WebServicesTrustedProxies", Label = L("Trusted proxies whose X-Forwarded-Proto: https is believed (addresses, comma-separated; loopback always is)") },
                     new PathSetting { Key = "WebServicesCertificateFile", FileFilter = L("PEM/certificate files (*.pem;*.crt;*.cer)|*.pem;*.crt;*.cer|All files (*.*)|*.*"), Label = L("TLS certificate file (PEM, optional)"), Placeholder = L("Falls back to the ACME certificate") },
                     new PathSetting { Key = "WebServicesPrivateKeyFile", FileFilter = L("PEM/key files (*.pem;*.key)|*.pem;*.key|All files (*.*)|*.*"), Label = L("TLS private key file (PEM, optional)") }
                  }
               });
               cards_.Add(new CardDef
               {
                  Title = L("Client autoconfiguration (autoconfig & autodiscover)"),
                  Blurb = L("Hands mail clients the right host names, ports and security settings automatically: Thunderbird-style autoconfig and Outlook autodiscover, for every local domain. Point autoconfig.<domain> and autodiscover.<domain> at this server in DNS."),
                  Settings =
                  {
                     new BoolSetting { Key = "AutoconfigEnabled", Default = true, Label = L("Thunderbird autoconfig + Outlook autodiscover") },
                     new TextSetting { Key = "AutoconfigClientHost", Label = L("Host name clients connect to (empty = server host name)") }
                  }
               });
               cards_.Add(new CardDef
               {
                  Title = L("MTA-STS policy hosting"),
                  Blurb = L("Publishes https://mta-sts.<domain>/.well-known/mta-sts.txt so other servers require TLS when they deliver to you. Honouring other domains' policies is on the Transport security page."),
                  Settings =
                  {
                     new BoolSetting { Key = "MtaStsHostingEnabled", Default = true, Label = L("Serve MTA-STS policies for local domains") },
                     new TextSetting { Key = "MtaStsPolicyMode", Default = "enforce", Label = L("MTA-STS policy mode (enforce / testing / none)") },
                     new TextSetting { Key = "MtaStsPolicyMaxAge", Default = "604800", Label = L("Policy max age (seconds; default 604800 = 7 days)") },
                     new TextSetting { Key = "MtaStsPolicyMx", Label = L("Policy MX host patterns (empty = derive from each domain's MX)"), Placeholder = "mail.yourdomain.com, *.yourdomain.com" } // no-loc
                  }
               });
               cards_.Add(new CardDef
               {
                  Title = L("Calendar and contacts discovery (CalDAV / CardDAV)"),
                  Blurb = L("hMailServer does NOT implement CalDAV or CardDAV. These settings only answer the well-known discovery URLs (RFC 6764) with a redirect to the server that does, so a client configured with a user's mail address finds their calendar without being told a second address. Leave both empty unless you run such a server: empty means the discovery URLs answer 404, which is the honest response when there is nothing to point at. They need the web services listener above to be running, like everything else on this page."),
                  Settings =
                  {
                     new TextSetting
                     {
                        Key = "CalDavRedirectUrl",
                        Label = L("Redirect /.well-known/caldav to (empty = answer 404)"),
                        Placeholder = "https://calendar.yourdomain.com/dav/",
                        Blurb = L("Must be an absolute URL. A relative one is refused and the discovery URL answers 404 instead, with the reason reported once in the error log.")
                     },
                     new TextSetting
                     {
                        Key = "CardDavRedirectUrl",
                        Label = L("Redirect /.well-known/carddav to (empty = answer 404)"),
                        Placeholder = "https://contacts.yourdomain.com/dav/",
                        Blurb = L("Must be an absolute URL, as above.")
                     }
                  }
               });
               break;
         }
      }

      /// <summary>
      /// Works out what a screen reader should call each editor, before any of them
      /// are built.
      ///
      /// No label on these seven pages currently repeats within its own page - that
      /// was counted rather than assumed - so nothing here is qualified today. It
      /// still goes through the same resolver as the COM settings pages: the naming
      /// rule must not be one thing on one settings page and another on the next,
      /// and a card added to this page tomorrow that repeats a label somewhere else
      /// on it will be handled without anybody having to remember to.
      /// <see cref="AccessibleNames"/> holds the decision.
      /// </summary>
      private void AssignAccessibleNames()
      {
         var settings = new List<Setting>();
         var editors = new List<LabelledEditor>();

         foreach (CardDef card in cards_)
         {
            foreach (Setting setting in card.Settings)
            {
               settings.Add(setting);
               editors.Add(new LabelledEditor(setting.Label, card.Title, setting.Key));
            }
         }

         IReadOnlyList<string> names = AccessibleNames.Resolve(editors);
         for (int i = 0; i < settings.Count; i++)
            settings[i].AccessibleName = names[i];
      }

      private void BuildUi()
      {
         CardsPanel.Children.Clear();
         warningRows_.Clear();

         AssignAccessibleNames();

         if (!store_.IsAvailable)
         {
            SetState_(SettingsPageState.Unavailable,
               L("hMailServer.INI was not found on this machine. These settings can only be edited on the server itself."));
            PathText.Visibility = Visibility.Collapsed;
            SaveButton.IsEnabled = false;
            return;
         }

         foreach (CardDef card in cards_)
         {
            // One card, one section: the card is the surface, the section the
            // level-2 heading a screen reader walks the page by, the rows the
            // fields, and a computed warning a notice under the fields it is
            // about. The last row gives up its gap so the card's own padding
            // closes the card.
            var fields = new StackPanel();
            foreach (Setting setting in card.Settings)
               fields.Children.Add(setting.CreateEditor(store_));

            if (fields.Children.Count > 0 && fields.Children[fields.Children.Count - 1] is FrameworkElement last)
               last.Margin = new Thickness(0);

            foreach (WarningDef def in card.Warnings)
               fields.Children.Add(BuildWarningRow_(def));

            var section = new SettingsSection
            {
               Heading = card.Title,
               Description = card.Blurb,
               Content = fields,
               Margin = new Thickness(0)
            };
            var surface = new Card { Content = section };
            surface.SetResourceReference(MarginProperty, "AppCardGap");
            CardsPanel.Children.Add(surface);
         }

         // Wire every editor to the warnings AFTER all the editors exist, because
         // a warning may read a setting on a later card than its own - and to the
         // pill, which says Unsaved changes from the first edit.
         foreach (CardDef card in cards_)
            foreach (Setting setting in card.Settings)
               setting.OnEditorChanged(EditorChanged_);

         // What every editor was filled with, which is what an unsaved change is
         // measured from.
         foreach (Setting setting in cards_.SelectMany(card => card.Settings))
            setting.MarkShown();

         RefreshWarnings_();

         PathText.Text = F("Editing {0}", store_.IniPath);
         PathText.Visibility = Visibility.Visible;
         SaveButton.IsEnabled = true;
         SetState_(SettingsPageState.Clean, null);
      }

      /// <summary>An editor changed: the warnings that read it, and the pill.</summary>
      private void EditorChanged_()
      {
         RefreshWarnings_();
         saveState_.Follow();
      }

      /// <summary>
      /// One (initially collapsed) computed warning: an InlineNotice, which carries
      /// the same three channels - colour, shape and word - as the dashboard and
      /// Spam overview badges, so none of the three is load-bearing alone.
      /// </summary>
      private FrameworkElement BuildWarningRow_(WarningDef def)
      {
         var notice = new InlineNotice
         {
            Visibility = Visibility.Collapsed,
            Margin = new Thickness(0, DesignTokens.Space.Md, 0, 0)
         };

         if (!string.IsNullOrEmpty(def.Aid))
            AutomationProperties.SetAutomationId(notice, def.Aid);

         warningRows_.Add(new WarningRow { Def = def, Notice = notice });
         return notice;
      }

      /// <summary>
      /// Where the page stands after a read, a save or a step of a restart, in the
      /// header's pill - until the next edit, which the pill follows - and, when
      /// there is a sentence to go with it, in the notice under the header. The
      /// notice takes the state's level unless the caller says otherwise: a restart
      /// in progress is a warning on the pill (what is on screen is not yet what
      /// runs) and information in the sentence.
      /// </summary>
      private void SetState_(SettingsPageState state, string message, StatusLevel? noticeLevel = null)
         => saveState_.Settle(state, message, noticeLevel);

      /// <summary>
      /// Re-evaluates every computed warning on the page against the editors as
      /// they stand. Runs once after the page is built and again on every editor
      /// change, so ticking a switch that makes a configuration inert says so
      /// immediately - not after a save, a restart and a support thread.
      /// </summary>
      private void RefreshWarnings_()
      {
         foreach (WarningRow row in warningRows_)
         {
            WarningState state;
            try
            {
               state = row.Def.Compute();
            }
            catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
            {
               // A warning must never take the page down; a state it cannot
               // evaluate is a state it does not report.
               state = null;
            }

            if (state == null)
            {
               row.Notice.Visibility = Visibility.Collapsed;
               continue;
            }

            row.Notice.Level = state.Level;
            row.Notice.Text = state.Text;
            row.Notice.Visibility = Visibility.Visible;
         }
      }

      private void Reload_Click(object sender, RoutedEventArgs e)
      {
         // Reading again replaces every editor, so an unsaved edit is asked about
         // first, Cancel being the answer Enter gives.
         if (HasUnsavedChanges &&
             !MessageBox.Confirm(UnsavedChanges.ReloadQuestion, L("Control Panel"), L("_Discard"), destructive: true, cancelByDefault: true))
            return;

         // Reload means reload, including the state read from outside the INI.
         // Without this the button re-read the file but kept the cached Service
         // Control Manager snapshot, so an administrator who ran the registration
         // command in the other window and pressed Reload - the obvious thing to
         // do, and what the status line invites - was shown the pre-registration
         // account and told the change had not been applied.
         ForgetExternalState_();

         BuildDefinition();
         BuildUi();
      }

      /// <summary>
      /// Drops everything the computed warnings cache from outside hMailServer.INI.
      ///
      /// Each is cached per page instance because a computed warning re-runs on
      /// every keystroke and these are expensive - a WMI query, two COM collection
      /// walks, and parsing two PEM files. That makes them stale by construction,
      /// so both entry points that mean "show me the current state" clear them.
      /// </summary>
      private void ForgetExternalState_()
      {
         serviceInfo_ = null;
         tlsPortCertificate_ = null;
         tlsPortCertificateRead_ = false;
         acmeHealth_ = null;
         acmeHealthFolder_ = null;
      }

      private void Save_Click(object sender, RoutedEventArgs e) => Save_();

      /// <summary>
      /// Writes every editor whose value is not what it was filled with, and says on
      /// the pill and under the header when the change takes effect. True when every
      /// write was made - which is what the question before leaving a page with
      /// unsaved changes waits for before it goes on.
      /// </summary>
      private bool Save_()
      {
         if (!store_.IsAvailable)
            return false;

         // A secret given a new value and ticked for removal both: refused before
         // anything on the page is written, in the notice under the header.
         SecretSetting conflict = cards_.SelectMany(card => card.Settings).OfType<SecretSetting>().FirstOrDefault(secret => secret.Conflicts);
         if (conflict != null)
         {
            saveState_.Refuse(SecretSetting.ConflictSentence(conflict.Label));
            return false;
         }

         // A new password pepper stops passwords working: asked about before anything
         // on the page is written, and a No writes nothing.
         if (!ConfirmPepperChange_())
            return false;

         try
         {
            foreach (CardDef card in cards_)
               foreach (Setting setting in card.Settings)
                  setting.Save(store_);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            saveState_.Refuse(F("Could not save: {0}", ex.Message));
            return false;
         }

         // Every editor now holds what the file does.
         foreach (Setting setting in cards_.SelectMany(card => card.Settings))
            setting.MarkShown();

         // The server published what it stored (docs/SettingsApply.md): a key marked
         // now is in effect already. What is not is what the server lists as pending
         // among this page's keys - a key marked reinitialize waits for the window's
         // Apply now, and only a key marked restart needs the service restarted. One
         // read of that list for the whole save; the window's bar reuses it.
         string time = DateTime.Now.ToLongTimeString();
         IReadOnlyList<string> pending = IniFeatureStore.PendingNames();

         // Null is a server that cannot say - one from before a save could publish,
         // whose [Settings] keys all apply at its next start, or one that did not
         // answer.
         List<string> waiting = null;
         int waitingForRestart = 0;
         if (pending != null)
         {
            var pageKeys = new HashSet<string>(
               cards_.SelectMany(card => card.Settings).Select(setting => setting.Key).Where(key => !string.IsNullOrEmpty(key)),
               StringComparer.OrdinalIgnoreCase);
            waiting = pending.Where(pageKeys.Contains).ToList();
            waitingForRestart = waiting.Count(key => IniFeatureStore.AppliesFor(key) == SettingApplies.Restart);
         }

         // The state is the one SettingsPageStatus.AfterSave gives, the rule its tests
         // hold, where this method chose it with an if-chain of its own beside it.
         SettingsPageState state = SettingsPageStatus.AfterSave(waiting?.Count, waitingForRestart);

         if (state == SettingsPageState.Saved)
         {
            SetState_(state, F("Saved {0} - every setting on this page is in effect.", time));
            return true;
         }

         if (state == SettingsPageState.WaitsForReinitialise)
         {
            SetState_(state, PluralF(waiting.Count,
               "Saved {0} - {1} setting waits for a reinitialise; Apply now, in the bar at the foot of the window, takes it.",
               "Saved {0} - {1} settings wait for a reinitialise; Apply now, in the bar at the foot of the window, takes them.",
               time, waiting.Count));
            return true;
         }

         // Some of the page's keys wait for a restart, or the server cannot say what
         // waits and applies them all at its next start: never that the save took effect.
         SetState_(state, F("Saved {0} - restart the service to apply.", time));

         string question = waiting == null
            ? L("Settings saved. The hMailServer service must be restarted for the changes to take effect.\n\nRestart it now?")
            : Plural(waitingForRestart,
               "Settings saved. {0} of them takes effect only when the hMailServer service restarts.\n\nRestart it now?",
               "Settings saved. {0} of them take effect only when the hMailServer service restarts.\n\nRestart it now?");
         if (MessageBox.Show(question, L("Control Panel"), MessageBoxButton.YesNo, MessageBoxImage.Question) == MessageBoxResult.Yes)
            RestartService();

         return true;
      }

      /// <summary>
      /// Asks before a save writes a new password pepper on a server with accounts
      /// (Services/PepperChange), with the number of accounts when they can be
      /// counted and Cancel the answer Enter gives. True when there is nothing to ask
      /// about or the change was chosen.
      /// </summary>
      private bool ConfirmPepperChange_()
      {
         if (FindSetting_(PepperChange.Key) is not SecretSetting pepper || !pepper.IsChanged)
            return true;

         // A pepper removed is a pepper changed: every password peppered with it stops
         // working as surely as with a new one.
         string stored = store_.Read(PepperChange.Key, "");
         if (pepper.RemovesStored)
         {
            int? holders = CountAccounts_();
            return !PepperChange.RemovalAsksFirst(stored, holders)
               || MessageBox.Confirm(PepperChange.Question(holders), L("Control Panel"), L("_Change the pepper"),
                  destructive: true, cancelByDefault: true);
         }

         string typed = pepper.LiveValue;
         if (string.IsNullOrEmpty(typed) || string.Equals(typed, stored ?? "", StringComparison.Ordinal))
            return true;

         int? accounts = CountAccounts_();
         if (!PepperChange.AsksFirst(typed, stored, accounts))
            return true;

         return MessageBox.Confirm(PepperChange.Question(accounts), L("Control Panel"), L("_Change the pepper"),
            destructive: true, cancelByDefault: true);
      }

      /// <summary>
      /// How many accounts the server has, across its domains; null when they could
      /// not be counted, which the pepper's question takes as some.
      /// </summary>
      private static int? CountAccounts_()
      {
         dynamic domains = null;
         try
         {
            domains = ServerSession.Current?.Application?.Domains;
            if (domains == null)
               return null;

            int total = 0;
            int count = (int)domains.Count;
            for (int i = 0; i < count; i++)
            {
               dynamic domain = null;
               dynamic accounts = null;
               try
               {
                  domain = domains.Item[i];
                  accounts = domain.Accounts;
                  total += (int)accounts.Count;
               }
               finally
               {
                  ServerSession.Release((object)accounts);
                  ServerSession.Release((object)domain);
               }
            }

            return total;
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            return null;
         }
         finally
         {
            ServerSession.Release((object)domains);
         }
      }

      /// <summary>
      /// Restarts the service and says how it went, through the restart the window's
      /// pending bar shares (Services/ServiceRestart).
      /// </summary>
      private async void RestartService()
      {
         SetState_(SettingsPageState.Restarting, L("Restarting the hMailServer service..."), StatusLevel.Information);

         (ServiceRestartOutcome outcome, string reason) = await ServiceRestart.RunAsync();
         switch (outcome)
         {
            case ServiceRestartOutcome.NotRestarted:
               SetState_(SettingsPageState.RestartFailed, F("Could not restart the service: {0}", reason));
               return;
            case ServiceRestartOutcome.NotReattached:
               SetState_(SettingsPageState.NotConnected,
                  F("The service was restarted but the Control Panel could not reconnect to it: {0}\n\nIt will keep trying as you use the application.", reason));
               return;
            default:
               SetState_(SettingsPageState.Applied, L("Service restarted - settings are live."));
               return;
         }
      }
   }
}
