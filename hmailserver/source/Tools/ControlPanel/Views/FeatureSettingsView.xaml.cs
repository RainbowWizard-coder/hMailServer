// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
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
   /// resolver and the public web services listener; and the Updates page, whose
   /// update policy is one of its cards, under a card of the page's own - the
   /// version and the three update steps (FeatureSettingsView.Updates.cs).
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
         Hardening,
         Authentication,
         Dns,
         WebServices,
         Updates
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

            return Field = new FieldRow { Label = caption, Hint = hint, Content = content, Applies = applies };
         }

         /// <summary>The row <see cref="Row"/> built, where a value a save refuses is said.</summary>
         public FieldRow Field { get; private set; }

         /// <summary>
         /// The row of a value this Control Panel cannot show as the server's - a key of a
         /// section of hMailServer.ini that no COM property carries, where the file here is
         /// not the server's or may not be read: the caption and a neutral Not read pill,
         /// as ServerSettingsView's UnreadRow_ draws it, and no editor, so that a save
         /// neither writes nor counts it. The card says why once (SendingLimitsNotice_).
         /// </summary>
         protected FieldRow NotReadRow()
         {
            var pill = new StatusPill
            {
               Level = StatusLevel.Normal,
               Text = L("Not read"),
               HorizontalAlignment = HorizontalAlignment.Left
            };
            AutomationProperties.SetAutomationId(pill, "setting-not-read");
            return Row(pill, Label);
         }

         /// <summary>
         /// The key of the switch - a BoolSetting on the same page - that this setting
         /// counts only while it is on; null for a setting that counts whatever is
         /// switched. The editor follows the switch (review finding F197).
         /// </summary>
         public string DependsOn;

         /// <summary>What <see cref="CreateEditor"/> returned, which follows the switch named by <see cref="DependsOn"/>.</summary>
         public FrameworkElement Built;

         /// <summary>The switch named by <see cref="DependsOn"/>, once the page has found it; null otherwise.</summary>
         public BoolSetting Master;

         /// <summary>
         /// Whether the switch this setting counts under is off: the row is dimmed and
         /// out of the tab order, so a message on it could be neither reached nor
         /// acted on (review finding F197).
         /// </summary>
         public bool SwitchedOff => Master?.Box != null && Master.Box.IsChecked != true;

         /// <summary>
         /// Why the value in the editor is not one the server takes, or null: a number
         /// outside the range the server reads it in, an address that is not one. Asked
         /// only of a value that was edited - one read from the file is shown as it is
         /// and, unchanged, not written.
         /// </summary>
         protected virtual string Check() => null;

         /// <summary>
         /// Puts <see cref="Check"/>'s answer on the row, or takes an old one off it;
         /// true when the value may be saved. A save asks every row before it writes
         /// any (review finding F220). A row behind a switch that is off is not held
         /// against the save, since it cannot be reached to correct it: the save keeps
         /// what the server has for a value it would not take (<see cref="KeptAsStored"/>),
         /// as the domain dialog keeps a number behind a switch that is off (F197).
         /// </summary>
         public bool Validate()
         {
            string error = Refusal;
            if (Field != null)
               Field.Error = error;
            return error == null;
         }

         /// <summary>
         /// Why a save would refuse the row as it stands, or null: <see cref="Check"/>'s
         /// answer for an edited value whose switch is on. What <see cref="Validate"/> puts
         /// on the row, and what the page's refusal stands on until it is put right.
         /// </summary>
         public string Refusal => IsChanged && !SwitchedOff ? Check() : null;

         /// <summary>
         /// Whether a save writes nothing of this row and puts its editor back to what
         /// the file holds: an edit the server would not take, behind a switch that is
         /// off, which <see cref="Validate"/> lets through. Put back, and not left in
         /// the box, so that the box says what the server has, and a later save with
         /// the switch on does not write what nobody was asked about.
         /// </summary>
         protected bool KeptAsStored => SwitchedOff && IsChanged && Check() != null;

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

         /// <summary>
         /// The width of a number on these pages, as ServerSettingsView gives its own
         /// (review finding F139): a count of seconds or a port is a few digits, and a
         /// box the width of the column says it expects a sentence. Free text, a path,
         /// a secret and a list take the width of the form column, as there.
         /// </summary>
         protected const double NumberWidth = 180;

         /// <summary>The width of an address - a bind address, the DNS override: an IPv6 address with room to spare (F220).</summary>
         protected const double AddressWidth = 260;

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

         /// <summary>The switch, which the settings that depend on it follow.</summary>
         public CheckBox Box => box_;

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

         /// <summary>
         /// What the server uses for the key held empty, where its reader puts a value of
         /// its own in the place of an empty one (IniFeatureStore.HeldEmptyPlaceholder);
         /// null where the server uses the empty value.
         /// </summary>
         public string EmptyMeans;

         /// <summary>An address, 260 pixels wide; free text otherwise, the width of the form column.</summary>
         public bool Address;

         /// <summary>What a save asks of the value - why it is not one the server takes, or null - where it asks anything.</summary>
         public Func<string, string> Rule;

         private Wpf.Ui.Controls.TextBox box_;

         public override FrameworkElement CreateEditor(IniFeatureStore store)
         {
            // The value the server holds: a key it holds empty is shown empty, and said
            // to be, where it was shown as the default - which the server does not use
            // for it (IniFeatureStore.ReadHeld; release review finding CP-4).
            string current = store.ReadHeld(Key, Default, out bool heldEmpty);
            Loaded = (current ?? "").Trim();
            box_ = new Wpf.Ui.Controls.TextBox
            {
               Text = current,
               PlaceholderText = heldEmpty ? IniFeatureStore.HeldEmptyPlaceholder(EmptyMeans) : ExamplePlaceholder.Shown(Placeholder),
               HorizontalAlignment = Address ? HorizontalAlignment.Left : HorizontalAlignment.Stretch
            };
            if (Address)
               box_.Width = AddressWidth;
            Describe(box_, Key);
            box_.TextChanged += (s, e) => { if (Field != null) Field.Error = null; };
            return Row(box_, Label);
         }

         protected override string Check() => Rule?.Invoke(box_?.Text?.Trim() ?? "");

         public override void Save(IniFeatureStore store)
         {
            if (KeptAsStored)
            {
               box_.Text = Loaded ?? "";
               return;
            }

            WriteIfChanged(store, box_.Text.Trim());
         }

         public override string LiveValue => box_?.Text;

         public override void OnEditorChanged(Action handler)
         {
            if (box_ != null && handler != null)
               box_.TextChanged += (s, e) => handler();
         }
      }

      /// <summary>
      /// A whole number: a box 180 pixels wide, as ServerSettingsView's numbers are,
      /// and the range the server reads the value in, which a save holds it to with a
      /// message on the field (review findings F220 and F139). These were 520-pixel
      /// boxes of free text whose save wrote whatever was typed - a port of 70000, a
      /// PBKDF2 work factor the server then reports and replaces with its own.
      ///
      /// A text box, as the LDAP and Directory synchronisation pages' numbers are, and
      /// not a NumberBox: a NumberBox takes its text in as the keyboard leaves it -
      /// which pressing Save with the mouse does, before Save asks the row - rounding
      /// a fraction ("1.5" became 2) and putting back the value before for text it
      /// cannot read ("30s", "10 000"), so that the save wrote a number other than
      /// the one typed, or the old one, without a word; and with bounds it pulls a
      /// value to the nearest one. The text stays as typed until the save asks
      /// whether it is a whole number in the range (Services/SettingNumberRange), and
      /// says on the field when it is not. A value the file holds that is not a
      /// number is shown as it is and, unchanged, not written. A key of another
      /// section - [SendingLimits] - is read and written there, and only while the
      /// file here is the server's and may be read: otherwise the row is Not read and a
      /// save leaves it, and the card says why (<see cref="SendingLimitsNotice_"/>;
      /// release review part B, CPB-2).
      /// </summary>
      private class NumberSetting : Setting
      {
         public string Default = "0";
         public string Placeholder = "";

         /// <summary>The lowest and the highest value the server reads the key in.</summary>
         public long Minimum;
         public long Maximum = int.MaxValue;

         /// <summary>0 is taken as well, outside the range: the server's built-in value (the password hash work factors).</summary>
         public bool ZeroIsBuiltIn;

         /// <summary>The section of hMailServer.INI the key is in, where it is not [Settings].</summary>
         public string Section;

         protected override bool InSettingsSection => Section == null;

         private Wpf.Ui.Controls.TextBox box_;

         public override FrameworkElement CreateEditor(IniFeatureStore store)
         {
            box_ = null;
            if (Section != null && !FileSectionIsTheServers_(store))
               return NotReadRow();

            // A [Settings] number the server holds empty is 0 to it, whatever the
            // default (IniFeatureStore.ReadNumber; release review finding CP-4).
            string raw = (Section == null
               ? store.ReadNumber(Key, Default)
               : IniDirect.ReadValue(store.IniPath, Section, Key, Default)) ?? "";
            raw = raw.Trim();

            box_ = new Wpf.Ui.Controls.TextBox
            {
               Text = raw,
               Width = NumberWidth,
               HorizontalAlignment = HorizontalAlignment.Left,
               PlaceholderText = ExamplePlaceholder.Shown(Placeholder)
            };

            // No input method: with a Japanese or Chinese one on, a digit typed arrives
            // as a full-width one, which the server does not read as a digit and the
            // save refuses.
            InputMethod.SetIsInputMethodEnabled(box_, false);

            Loaded = SettingNumberRange.TryParse(raw, out long current)
               ? current.ToString(CultureInfo.InvariantCulture)
               : raw;

            Describe(box_, Key);
            box_.TextChanged += (s, e) => { if (Field != null) Field.Error = null; };
            return Row(box_, Label);
         }

         public override void Save(IniFeatureStore store)
         {
            // A row marked Not read has no editor, and is left as the server has it.
            if (box_ == null)
               return;

            if (KeptAsStored)
            {
               box_.Text = Loaded ?? "";
               return;
            }

            string value = LiveValue;
            if (Section == null)
            {
               WriteIfChanged(store, value);
               return;
            }

            if (Loaded != null && string.Equals(value, Loaded, StringComparison.Ordinal))
               return;

            IniDirect.WriteValue(store.IniPath, Section, Key, value);
            Loaded = value;
         }

         /// <summary>
         /// The number as hMailServer.INI spells it, read from the text as typed, or
         /// the text itself when it is not a whole number - which the save refuses.
         /// </summary>
         public override string LiveValue
         {
            get
            {
               if (box_ == null)
                  return null;

               string text = (box_.Text ?? "").Trim();
               return SettingNumberRange.TryParse(text, out long value)
                  ? value.ToString(CultureInfo.InvariantCulture)
                  : text;
            }
         }

         protected override string Check() => SettingNumberRange.Error(LiveValue, Minimum, Maximum, ZeroIsBuiltIn);

         public override void OnEditorChanged(Action handler)
         {
            if (box_ != null && handler != null)
               box_.TextChanged += (s, e) => handler();
         }
      }

      /// <summary>
      /// One of a few words, as a list: a setting the server reads as a word, such as
      /// the MTA-STS policy mode, which was a free-text box captioned "(enforce /
      /// testing / none)" beside a list that existed in the same file for numbers
      /// (review finding F220). Matched without regard to case, as the server reads
      /// it; a word the list does not have is offered as itself, after the choices,
      /// and written back unchanged (Services/ChoiceOptions).
      /// </summary>
      private class WordSetting : Setting
      {
         public string Default = "";
         public (string Value, string Label)[] Options;

         /// <summary>
         /// The choice the server uses for the key held empty, where its reader puts one of
         /// its own in the place of an empty value - MtaStsPolicyMode's "enforce"
         /// (WebServicesServer, which serves anything but enforce, testing and none as
         /// enforce); null where the server uses the empty value.
         /// </summary>
         public string EmptyMeans;
         private ComboBox combo_;

         public override FrameworkElement CreateEditor(IniFeatureStore store)
         {
            string current = (store.ReadHeld(Key, Default, out bool heldEmpty) ?? "").Trim();
            combo_ = new ComboBox { HorizontalAlignment = HorizontalAlignment.Stretch };

            ComboBoxItem chosen = null;
            foreach ((string value, string label) in Options)
            {
               var item = new ComboBoxItem { Content = label, Tag = value };
               combo_.Items.Add(item);
               if (chosen == null && string.Equals(value, current, StringComparison.OrdinalIgnoreCase))
                  chosen = item;
            }

            if (chosen == null && current.Length > 0)
            {
               chosen = new ComboBoxItem { Content = ChoiceOptions.UnknownLabel(current), Tag = current };
               combo_.Items.Add(chosen);
            }

            // A key the server holds empty is the choice the server then uses, where its
            // reader puts one in its place (EmptyMeans), and else is offered as itself and
            // written back so, as a word the list does not know is - not as the first
            // choice (CP-4). Saved unchanged, the choice writes nothing.
            if (chosen == null && heldEmpty && EmptyMeans != null)
            {
               foreach (ComboBoxItem item in combo_.Items)
               {
                  if (string.Equals((string)item.Tag, EmptyMeans, StringComparison.OrdinalIgnoreCase))
                  {
                     chosen = item;
                     break;
                  }
               }
            }

            if (chosen == null && heldEmpty)
            {
               chosen = new ComboBoxItem { Content = IniFeatureStore.HeldEmptyPlaceholder(EmptyMeans), Tag = "" };
               combo_.Items.Add(chosen);
            }

            combo_.SelectedItem = chosen ?? (combo_.Items.Count > 0 ? combo_.Items[0] : null);
            Loaded = LiveValue;

            Describe(combo_, Key);
            return Row(combo_, Label);
         }

         public override void Save(IniFeatureStore store)
         {
            if (LiveValue is string value)
               WriteIfChanged(store, value);
         }

         public override string LiveValue => combo_?.SelectedItem is ComboBoxItem item ? (string)item.Tag : null;

         public override void OnEditorChanged(Action handler)
         {
            if (combo_ != null && handler != null)
               combo_.SelectionChanged += (s, e) => handler();
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
            var row = new Grid { HorizontalAlignment = HorizontalAlignment.Stretch };
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

            string current = store.Read(Key, Default);
            Loaded = (current ?? "").Trim();
            box_ = new Wpf.Ui.Controls.TextBox
            {
               Text = current,
               PlaceholderText = ExamplePlaceholder.Shown(Placeholder),
               HorizontalAlignment = HorizontalAlignment.Stretch
            };
            Describe(box_, Key);
            Grid.SetColumn(box_, 0);
            row.Children.Add(box_);

            // Captioned Browse…, as the dialogs' pickers are, where it was a
            // 40-pixel "…" (review finding F324); no Alt key, as a card holds
            // several - the REST API card's certificate and private key.
            var browse = new Wpf.Ui.Controls.Button
            {
               Content = L("Browse…"),
               Margin = new Thickness(DesignTokens.Space.Sm, 0, 0, 0),
               VerticalAlignment = VerticalAlignment.Bottom
            };
            SetAid(browse, Key + "Browse"); // no-loc
            // Named by its caption and then the setting it fills, so that the
            // card's two are told apart; the sentence is its help text and tool tip.
            string setting = LabelInName.Shown(AccessibleName ?? Label ?? L("this setting"));
            ControlHelp.Explain(browse, PickFolder ? F("Browse for a folder for {0}", setting) : F("Browse for a file for {0}", setting));
            ControlHelp.NameLink(browse, L("Browse…"), setting);
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
            combo_ = new ComboBox { HorizontalAlignment = HorizontalAlignment.Stretch };
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

         // Nothing while the removal is ticked: a blank box then removes the stored
         // secret, and "leave blank to keep it" in the box said the opposite of the
         // box ticked under it.
         private string Placeholder_()
            => RemovesStored ? ""
               : hasStored_ ? L("A secret is configured - leave blank to keep it")
               : string.IsNullOrEmpty(Hint) ? L("Enter a secret")
               : Hint;

         // A PasswordBox's placeholder is not part of its accessible name, and
         // whether a secret is already stored is the one thing this control
         // conveys that its label does not - leaving the field blank keeps the
         // existing value, so a listener who cannot see the placeholder has no
         // way to know whether there is one. The row's hint carries it, under
         // the editor and in its help text alike - except while the removal is
         // ticked, when a blank box does not keep it: the hint said that it did,
         // and so did the help text a screen reader reads on the field.
         private string Hint_()
            => !hasStored_ || RemovesStored ? Blurb
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
               return Arrange_(Row(box_, Label, hint: hint));

            var row = new Grid { HorizontalAlignment = HorizontalAlignment.Stretch };
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

            // Ticked or unticked, the field above says what a blank box does now.
            remove_.Checked += (s, e) => ShowWhatABlankBoxDoes_();
            remove_.Unchecked += (s, e) => ShowWhatABlankBoxDoes_();

            var panel = new StackPanel();
            panel.SetResourceReference(MarginProperty, "AppFieldGap");
            panel.Children.Add(row);
            panel.Children.Add(remove_);

            if (!string.IsNullOrEmpty(Warning))
            {
               warning_ = new InlineNotice { Level = StatusLevel.Warning, Text = Warning, HorizontalAlignment = HorizontalAlignment.Stretch, Margin = new Thickness(0) };
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
            remove_.IsChecked = false;
            remove_.Visibility = stored ? Visibility.Visible : Visibility.Collapsed;
            ShowWhatABlankBoxDoes_();

            // The field's gap is the panel's; inside it, the small step between the
            // parts that are on screen.
            row_.Margin = new Thickness(0, 0, 0, stored || warning_ != null ? DesignTokens.Space.Sm : 0);
            remove_.Margin = new Thickness(0, 0, 0, warning_ != null ? DesignTokens.Space.Sm : 0);
         }

         /// <summary>
         /// What a blank box does at the save, in the placeholder, the row's hint and
         /// the field's help text: keeps the stored secret, or removes it while the box
         /// under it is ticked - when the placeholder is empty and the hint says what
         /// the setting is and no more. The help text is the row's, as FieldRow makes
         /// it: the hint, then when a save applies.
         /// </summary>
         private void ShowWhatABlankBoxDoes_()
         {
            box_.PlaceholderText = Placeholder_();
            row_.Hint = Hint_();
            AutomationProperties.SetHelpText(box_, Sentences(row_.Hint, row_.Applies, row_.Error));
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

         /// <summary>
         /// Writes one value, and throws with Windows' reason when the file refused it: the
         /// answer was dropped, so that a file this user may read and not write was counted
         /// saved (release review part B, CPB-2).
         /// </summary>
         public static void WriteValue(string iniPath, string section, string key, string value)
         {
            if (string.IsNullOrEmpty(iniPath))
               throw new InvalidOperationException("hMailServer.ini was not found on this machine.");
            ProfileApi.WriteStringChecked(section, key, value, iniPath);
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
               throw new InvalidOperationException("hMailServer.ini was not found on this machine.");
            ProfileApi.WriteSectionLinesChecked(section, lines, iniPath);
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
            box_ = null;
            loaded_ = null;
            if (!FileSectionIsTheServers_(store))
               return NotReadRow();

            loaded_ = Normalize(IniDirect.ReadSectionLines(store.IniPath, Section));

            box_ = new Wpf.Ui.Controls.TextBox
            {
               Text = loaded_,
               PlaceholderText = ExamplePlaceholder.Shown(Placeholder),
               MinHeight = 88,
               AcceptsReturn = true,
               TextWrapping = TextWrapping.NoWrap,
               VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
               HorizontalAlignment = HorizontalAlignment.Stretch
            };
            Describe(box_, Key);
            return Row(box_, Label);
         }

         public override void Save(IniFeatureStore store)
         {
            // A row marked Not read has no editor, and is left as the server has it.
            if (box_ == null)
               return;

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
         private readonly string tab_;

         /// <summary>Creates a signpost row pointing at the page that owns a setting.</summary>
         /// <param name="page">Nav key of the page that owns the setting.</param>
         /// <param name="caption">What is over there, in the administrator's words.</param>
         /// <param name="tab">
         /// The English header of the page's tab that holds it, marked with N(), or
         /// null: the row then names the tab and its button opens the page on it
         /// (MainWindow.NavigateToTab), where it opened the page on its first tab.
         /// </param>
         public ElsewhereSetting(string page, string caption, string tab = null)
         {
            page_ = page;
            tab_ = tab;
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
            if (!string.IsNullOrEmpty(tab_))
               destination += " > " + L(tab_);

            var row = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 0, 0, DesignTokens.Space.Sm) };

            var text = new TextBlock
            {
               Text = Label + "  →  " + destination,
               VerticalAlignment = VerticalAlignment.Center,
               MaxWidth = 420
            };
            text.SetResourceReference(StyleProperty, "TextBody");
            row.Children.Add(text);

            // The button names the page it opens, with no ellipsis: it opens a page
            // and asks nothing (review finding F305); it said "Open…" on every row.
            string pageTitle = L(NavigationMap.TitleOf(page_));
            var button = new Wpf.Ui.Controls.Button   // per setting: no access key, the rows are reached with the arrow keys
            {
               Content = F("Open {0}", pageTitle),
               Appearance = Wpf.Ui.Controls.ControlAppearance.Transparent,
               Margin = new Thickness(DesignTokens.Space.Sm, 0, 0, 0),
               VerticalAlignment = VerticalAlignment.Center,
               ToolTip = F("Open {0}", destination)
            };
            ButtonStyles.CompactPageLink(button);
            AutomationProperties.SetName(button, F("Open {0}, which now has {1}", pageTitle, Label));
            SetAid(button, "elsewhere-" + page_);
            button.Click += (s, e) => (Application.Current?.MainWindow as MainWindow)?.NavigateToTab(page_, tab_);
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

         /// <summary>
         /// The navigation key of the page that shows more of what the notice says, or
         /// null: the notice then carries Open and that page's title
         /// (Services/NoticeActions) - the ACME certificate's state, which the SSL
         /// certificates page lists with the others (review finding F237).
         /// </summary>
         public string Page;
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

         /// <summary>
         /// Whether the notice is the card's first row, above the fields, where the
         /// card's one live fact is read before the requests under it - the ACME
         /// certificate's state, which was below the card's seven fields and so below
         /// the fold at 1280 by 800 (review finding F237). A warning about the fields
         /// is under them, as every other is.
         /// </summary>
         public bool First;
      }

      private class CardDef
      {
         public string Title;
         public string Blurb;
         public List<Setting> Settings = new();
         public List<WarningDef> Warnings = new();

         /// <summary>
         /// The tab the card is drawn on, translated, or null for a page without tabs.
         /// A section whose cards name tabs is drawn as ServerSettingsView draws its
         /// pages - the header and the notice above a tab strip, each tab scrolling on
         /// its own (<see cref="FrameTabs_"/>) - its tabs in the order their first cards
         /// come.
         /// </summary>
         public string Tab;
      }

      // The master switches the settings under them follow (Setting.DependsOn).
      private const string AcmeSwitch = "AcmeEnabled"; // no-loc: INI keys
      private const string OAuth2Switch = "OAuth2Enabled"; // no-loc

      private readonly Section section_;
      private readonly IniFeatureStore store_ = new();
      private List<CardDef> cards_;

      // Why this build of the page read hMailServer.ini on this computer in place
      // of the server, and the notice that says so; null when every read the build
      // made was answered. Taken when the page is built, and what Save_ refuses
      // on: the store's own FallbackCause is live - a warning's read on an editor
      // change sets it - and a save that refused on that after a build that had
      // read cleanly did nothing and said nothing. The notice is kept in words,
      // since putting a lost link into words has the session repair it, which
      // must not happen again for an old failure.
      private Exception readFromFile_;
      private string readFromFileNotice_;

      // The notice's Try again while the values are the file's: one button, kept from
      // one build to the next, so that the keyboard stays on it when the server still
      // does not answer (review finding F179).
      private Wpf.Ui.Controls.Button tryAgain_;

      /// <summary>One built warning: the definition and the notice it drives.</summary>
      private class WarningRow
      {
         public WarningDef Def;
         public InlineNotice Notice;

         /// <summary>The notice's Open button, made once for its page and kept across the warning's reruns.</summary>
         public UIElement Action;
         public string ActionPage;
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
               Text = L("The Control Panel is connected to another host, so the account its service runs as cannot be read from here. Open the Control Panel on the server itself to see it.")
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

         // Switched on with no host name, nothing can be issued: the server asks the
         // CA for a certificate for the names listed, and with none it asks for
         // nothing. Said first, and alone - where the page went on to say that none
         // had been issued yet and that, if that persisted, the CA could not reach
         // this server, which sent the reader to the network (review finding F237).
         if (!AcmeHostNames.Any(LiveText_("AcmeDomains", "")))
         {
            return new WarningState
            {
               Level = StatusLevel.Critical,
               Text = L("No certificate can be issued without a host name: add at least one to the host names below.")
            };
         }

         if (!ServerSession.IsLocalSession)
         {
            return new WarningState
            {
               Level = StatusLevel.Information,
               Text = L("The issued certificate is a file on the server's disk, and the Control Panel is connected to another host, so its expiry cannot be read from here."),
               Page = "certs"
            };
         }

         string folder = AcmeCertificateFolder_();

         if (folder.Length == 0)
         {
            return new WarningState
            {
               Level = StatusLevel.Information,
               Text = L("Where the issued certificate would be written could not be determined, because neither the output folder above nor the server's data folder ([Directories] DataFolder in hMailServer.ini) could be read.")
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
         string expires = ServerTimeText.Date(health.ExpiresOn.Value);

         if (days < 0)
         {
            return new WarningState
            {
               Page = "certs",
               Level = StatusLevel.Critical,
               Text = PluralF(-days,
                  "The issued certificate expired on {0}, {1} day ago, and renewal has not replaced it. Clients are refusing this server's TLS. Check the error log for the last renewal attempt: the usual cause is that the CA can no longer reach this server on the http-01 challenge port.",
                  "The issued certificate expired on {0}, {1} days ago, and renewal has not replaced it. Clients are refusing this server's TLS. Check the error log for the last renewal attempt: the usual cause is that the CA can no longer reach this server on the http-01 challenge port.",
                  expires, -days)
            };
         }

         if (days <= CertificateInspector.ExpiryWarningDays)
         {
            return new WarningState
            {
               Page = "certs",
               Level = StatusLevel.Warning,
               Text = PluralF(days,
                  "The issued certificate expires on {0}, in {1} day. The renewal task runs inside this window, so this is only a problem if the number stops falling - if it does, renewal is failing and the error log says why.",
                  "The issued certificate expires on {0}, in {1} days. The renewal task runs inside this window, so this is only a problem if the number stops falling - if it does, renewal is failing and the error log says why.",
                  expires, days)
            };
         }

         return new WarningState
         {
            Page = "certs",
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
               Text = F("OAuth2 is switched on but no client can sign in with a token, because it is missing {0}. Signature verification cannot run at all, so every token is rejected and the log records only a failed sign-in - which reads as \"the password is wrong\" rather than as a configuration gap.", string.Join("; ", blocking))
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
               Text = Sentences(F("Tokens are accepted, but {0}: the server applies each of those only when you have set it, and blank is the default. That is wider than it looks. Any token your identity provider signs with this key is accepted - including one it issued to a different application entirely, which is enough to sign in as whichever mailbox that token names. Set both to the exact values your provider puts in its tokens.", unchecked_),
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

      public FeatureSettingsView(Section section)
      {
         InitializeComponent();
         saveState_ = new IniPageSaveState(StatePill, StatusNotice, () => HasUnsavedChanges);
         section_ = section;
         UpdateCard.Visibility = IsUpdatesPage_ ? Visibility.Visible : Visibility.Collapsed;
         BuildDefinition();
         if (cards_.Any(card => card.Tab != null))
            FrameTabs_();
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

         // The Updates page's own card reads the server's update state as the page opens.
         EnterUpdates_();
      }

      public void OnLeave() => LeaveUpdates_();

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
               Header.PageKey = "security";
               Header.Subtitle = L("Encryption to other servers, the signatures this server adds and checks, the results it records, forwarded mail and the reports it sends.");
               cards_.Add(new CardDef
               {
                  Title = L("DANE & DNSSEC"),
                  Tab = L("Encryption"),
                  Blurb = L("Validates recipient TLSA records with in-process DNSSEC and blocks delivery over forged chains (RFC 7672)."),
                  Settings =
                  {
                     new BoolSetting { Key = "DaneEnforcementEnabled", Default = true, Label = L("Honour recipient DANE/TLSA records when sending") },
                     new BoolSetting { Key = "DnssecValidationEnabled", Default = true, Label = L("Validate DNSSEC for DANE and SPF/DKIM/DMARC lookups") },
                     new TextSetting
                     {
                        // A caption, with the format and an example under it, where the
                        // caption was the format string (review finding F311).
                        Key = "DnssecTrustAnchors",
                        Label = L("Trust anchors (DS records, separated by semicolons)"),
                        Blurb = L("Each is a root DS record - key tag, algorithm, digest type and the digest in hex - such as 20326 8 2 E06D44B80B8F1D39A95C0B0D7C65D08458E880409BBC683457104237C7F8EC8D. Set, they replace the built-in root anchors; empty uses those.")
                     }
                  }
               });
               cards_.Add(new CardDef
               {
                  Title = L("MTA-STS"),
                  Tab = L("Encryption"),
                  Blurb = L("Discovers and enforces recipient MTA-STS policies before delivering over TLS (RFC 8461). To publish a policy for your own domains, see the Web services & autoconfiguration page."),
                  Settings =
                  {
                     new BoolSetting { Key = "MtaStsEnabled", Default = true, Label = L("Honour recipient MTA-STS policies when sending") }
                  }
               });
               cards_.Add(new CardDef
               {
                  Title = L("ARC sealing"),
                  Tab = L("Signing"),
                  // The prerequisite sentence is stated rather than checked here:
                  // whether a hosted domain has a DKIM selector and key file lives
                  // in the database, behind the COM API, and this page works from
                  // hMailServer.INI alone - it must keep telling the truth when no
                  // COM session exists. The Setup checklist page holds the live
                  // check and walks the actual domains.
                  Blurb = L("Adds ARC seals to forwarded mail so downstream servers can trust original authentication results (RFC 8617). Sealing borrows the forwarding domain's DKIM selector and private key, so it needs at least one hosted domain with DKIM signing configured - without that the switch reads enabled and seals nothing, with only a debug log line to say so."),
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
                  Tab = L("Signing"),
                  Settings =
                  {
                     new ElsewhereSetting("antispam", L("ARC inbound filtering and its trusted sealers"), N("Sender auth"))
                  },
                  // These two are database settings rather than ini values, so they are not
                  // editable on this page - which is for hMailServer.INI. They ARE editable,
                  // on the Anti-spam page, and this card points there. It used to say they
                  // could not be edited at all, which stopped being true the moment the COM
                  // properties landed; a card that describes a limitation the product no
                  // longer has is the same defect as one that claims a feature it lacks.
                  Blurb = L("The counterpart to ARC sealing: on inbound mail, a valid ARC chain from a trusted sealer can offset the DMARC failure score for a message whose DMARC pass was destroyed by forwarding (RFC 8617). It is configured on the Anti-spam settings page's Sender auth tab, because it is stored with the other anti-spam settings rather than in hMailServer.ini.")
               });
               cards_.Add(new CardDef
               {
                  Title = L("DKIM signature timestamps"),
                  Tab = L("Signing"),
                  Blurb = L("When a DKIM signature was made, and when it stops being one a verifier should honour (RFC 6376 3.5)."),
                  Settings =
                  {
                     new NumberSetting
                     {
                        Key = "DKIMSignatureValiditySeconds", Minimum = 0,
                        Default = "0",
                        Placeholder = "604800",
                        Label = L("Validity window for the signatures this server makes (seconds; 0 = no expiry)"),
                        Blurb = L("0 emits no expiry at all, which is the safe default: an expiry is a promise about mail already in flight, and a window shorter than the delay a retry, a greylist or a mailing list adds costs the message its DKIM pass and its DMARC alignment at the far end. 604,800 is a week, the usual choice for a sender who wants one.")
                     },
                     new BoolSetting
                     {
                        Key = "DKIMEnforceSignatureExpiry",
                        Default = true,
                        Label = L("Refuse signatures on incoming mail whose expiry has passed"),
                        Blurb = L("The expiry sits inside the bytes the signature covers, so it is the sending domain's own instruction rather than something a third party can add. Turning this off means a captured signed message can be replayed indefinitely.")
                     },
                     new NumberSetting
                     {
                        Key = "DKIMExpiryClockSkewSeconds", Minimum = 0,
                        Default = "300",
                        Label = L("Clock-drift tolerance when checking an expiry (seconds)"),
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
                  Tab = L("Signing"),
                  Blurb = L("Oversigning (RFC 6376 5.4) lists a header field name in the signature's h= tag once more often than the field occurs, which makes adding another one - a second From:, an injected Subject: - break the signature. Off by default: oversigning a field that a mailing list or forwarder legitimately adds costs those messages their DKIM pass."),
                  Settings =
                  {
                     new TextSetting
                     {
                        Key = "DkimOversignHeaders",
                        Label = L("Header fields to oversign in outbound DKIM signatures (comma separated; empty = off)"),
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
                  Tab = L("Signing"),
                  Blurb = L("Records the verdicts this server itself reached about each inbound message - SPF, DKIM and DMARC - as trace headers on the delivered message, for downstream filters and diagnostics (RFC 8601, RFC 7208)."),
                  Settings =
                  {
                     new BoolSetting
                     {
                        Key = "AuthenticationResultsEnabled",
                        Default = false,
                        Label = L("Write an Authentication-Results header on inbound mail"),
                        Blurb = L("Each accepted inbound message gets one Authentication-Results header (RFC 8601) carrying the SPF, DKIM and DMARC verdicts this server reached about it. Only checks that actually ran are reported - which checks run is configured on the Anti-spam settings page - and a message on which no check ran gets no header.")
                     },
                     new BoolSetting
                     {
                        Key = "ReceivedSpfHeaderEnabled",
                        Default = false,
                        Label = L("Write a Received-SPF header on inbound mail"),
                        Blurb = L("Records the SPF verdict for each accepted inbound message as a Received-SPF header (RFC 7208 section 9.1). The header is only written when the SPF check actually ran, so 'Check SPF' must be enabled on the Anti-spam settings page for it to appear.")
                     },
                     new TextSetting
                     {
                        Key = "AuthenticationResultsIdentity",
                        Label = L("Identity the results are written under (empty = this computer's name)"),
                        Placeholder = "mail.example.com",
                        Blurb = L("The authserv-id: the first token of every Authentication-Results header this server writes, always lower-cased, and the name a downstream filter checks before trusting the verdicts. The same name decides which arriving Authentication-Results headers are treated as forged: one claiming this identity is removed, while one naming any other identity is left completely untouched.")
                     }
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
                  Tab = L("Forwarding"),
                  Blurb = L("SRS rewrites the envelope sender into one this server can vouch for and can undo on the way back, so bounces still reach the original sender; BATV tags the envelope sender of outbound mail so a forged bounce - one for a message this server never sent - can be told apart from a real one. Both use a server-wide secret and do nothing at all until one is set."),
                  Settings =
                  {
                     new BoolSetting
                     {
                        Key = "RewriteEnvelopeFromWhenForwarding",
                        Default = false,
                        Label = L("Rewrite the envelope sender when forwarding (the simple fallback, no secret needed)"),
                        Blurb = L("That makes SPF pass at the next hop, at the cost of the original sender's address: a bounce comes back to the forwarding mailbox instead of to whoever wrote the message. SRS below does the same job without losing the return path, and while SRS is enabled this setting is not consulted at all.")
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
               cards_.Add(new CardDef
               {
                  Title = L("TLS reporting (TLS-RPT)"),
                  Tab = L("Reports"),
                  Blurb = L("Sends daily aggregate reports about TLS connection failures to recipient domains (RFC 8460)."),
                  Settings =
                  {
                     // Named "TLS report..." rather than "Report..." since DMARC
                     // reporting joined this page: two identically-labelled fields
                     // on one page are ambiguous to anyone who cannot see which
                     // card they are in, and AccessibleNamesTests fails on it.
                     new TextSetting { Key = "TlsRptFromAddress", Label = L("TLS report sender address (empty = off)"), Placeholder = "tlsrpt@example.com" },
                     new TextSetting { Key = "TlsRptOrganizationName", Default = "hMailServer", Label = L("Organisation name in TLS reports") }
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
                  Tab = L("Reports"),
                  Blurb = L("Sends the daily aggregate reports that domains ask for with a rua= tag in their DMARC record (RFC 7489) - who sent mail claiming to be them, and whether it passed. Reports only go to an address outside the policy domain when that address's own DNS says it wants them, so a domain cannot use its DMARC record to aim this server's reports at somebody else."),
                  Settings =
                  {
                     new TextSetting { Key = "DmarcRptFromAddress", Label = L("DMARC report sender address (empty = off)"), Placeholder = "dmarc@example.com" },
                     new TextSetting { Key = "DmarcRptOrganizationName", Default = "hMailServer", Label = L("Organisation name in DMARC reports") }
                  }
               });
               break;

            case Section.Automation:
               Header.PageKey = "acme";
               Header.Subtitle = L("Built-in Let's Encrypt integration: certificates are issued, renewed, assigned to TLS ports and hot-reloaded automatically.");
               cards_.Add(new CardDef
               {
                  Title = L("ACME (Let's Encrypt)"),
                  Blurb = L("Issued certificates are stored in Data\\ACME and assigned to TLS ports without a restart. Key reuse keeps published DANE TLSA records valid across renewals."),
                  Settings =
                  {
                     new BoolSetting { Key = "AcmeEnabled", Default = false, Label = L("Issue and renew certificates automatically") },
                     new TextSetting { Key = "AcmeContactEmail", DependsOn = AcmeSwitch, Label = L("Contact email address (CA expiry notices)"), Placeholder = "admin@example.com" },
                     new TextSetting { Key = "AcmeDomains", DependsOn = AcmeSwitch, Label = L("Host names for the certificate (comma separated)"), Placeholder = "mail.example.com, mta-sts.example.com" }, // no-loc
                     new TextSetting { Key = "AcmeDirectoryUrl", DependsOn = AcmeSwitch, Default = "https://acme-v02.api.letsencrypt.org/directory", Label = L("ACME directory URL") }, // no-loc: a URL
                     new NumberSetting { Key = "AcmeHttpPort", DependsOn = AcmeSwitch, Minimum = 1, Maximum = 65535, Default = "80", Label = L("Port for http-01 challenges") },
                     new PathSetting { Key = "AcmeCertificateDirectory", PickFolder = true, Label = L("Certificate output folder (empty = Data\\ACME)") },
                     new BoolSetting { Key = "AcmeReuseKey", DependsOn = AcmeSwitch, Default = true, Label = L("Reuse the private key across renewals (keeps DANE TLSA records valid)") }
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
                        First = true,
                        Compute = ComputeAcmeCertificateState_
                     }
                  }
               });
               break;

            // The Updates page (review finding F272): the update policy, moved here
            // from API & monitoring, under the page's own card - the version, the
            // update check's verdict and the three steps (FeatureSettingsView.Updates.cs),
            // which were on Server status. Its settings are hMailServer.ini's, as the
            // rest of this view's are; the subtitle is the page's navigation
            // description (PageHeader.PageKey).
            case Section.Updates:
               Header.PageKey = "updates";
               cards_.Add(new CardDef
               {
                  // Every key the live update reads is on this card. That is checked
                  // rather than maintained by hand: build/check-ini-coverage.py fails
                  // when a setting the server reads has no editor anywhere in the
                  // Control Panel, which is how the five trust settings below were
                  // found missing after the feature had shipped. The card above it has
                  // the buttons (FeatureSettingsView.Updates.cs); the two were a page
                  // apart, on Server status and API & monitoring, until the Updates
                  // page (review finding F272). Everything is off until the check is
                  // turned on, and the check sends nothing but the request.
                  Title = L("How this server updates"),
                  Blurb = L("The server can notice a new release, verify it against the release's own Sigstore signature, and apply it - by a click on Install update above, or on its own inside a window you set. Off until you turn the check on; the check sends nothing but the request."),
                  Settings =
                  {
                     // As often as the hours below say, which the server honours from 1 up -
                     // it raises a lower value to 1 and caps nothing (IniFileSettings.cpp,
                     // UpdateChecker::NextCheckUnix, in 64-bit seconds) - where the switch said
                     // "once a day" beside a box that took 1 to 168 and refused the monthly
                     // check the server runs (release review finding CP-5).
                     new BoolSetting { Key = "UpdateCheckEnabled", Default = false, Label = L("Check for a new release") },
                     new NumberSetting { Key = "UpdateCheckHours", Minimum = 1, Default = "24", Label = L("Hours between checks (at least 1; 24 = once a day)") },
                     // Anything but prerelease is the stable feed (UpdateChecker::IsPreReleaseChannel), an empty value too.
                     new TextSetting { Key = "UpdateChannel", Default = "stable", EmptyMeans = "stable", Label = L("Channel: stable, or prerelease to run ahead on a test machine") },
                     new TextSetting { Key = "UpdateFeedUrl", Label = L("Update feed URL (empty = the built-in feed)"), Blurb = L("Set it to use a mirror.") },
                     new TextSetting { Key = "HttpProxy", Label = L("Forward proxy for this server's web requests (host:port; empty = direct)"), Blurb = L("The update feed and its downloads, JWKS and token introspection all go through it. Proxy credentials are not supported."), Placeholder = "proxy.example.net:3128" },
                     new BoolSetting { Key = "UpdateAutoDownload", Default = false, Label = L("Download and verify the installer as soon as a release is found") },
                     new TextSetting
                     {
                        Key = "UpdateWindow",
                        Label = L("Unattended window (empty = never apply on its own)"),
                        Placeholder = "Sun 03:00", // no-loc
                        Blurb = Sentences(L("For example 03:00, Sun 03:00 or Sat,Sun 02:00-05:00."), L("Inside the window the scheduled task applies a verified installer without a click: the service stops, files and schema upgrade, the service starts, and the outcome goes to the log, the Windows event log and the card above."))
                     },
                     new BoolSetting { Key = "UpdateBackupBeforeApply", Default = true, Label = L("Back up to the configured destination before an unattended apply (no destination or a failed backup means no apply)") },
                     new BoolSetting
                     {
                        Key = "UpdateRequireAuthenticode",
                        Default = false,
                        Label = L("Also require a valid Authenticode signature on the installer"),
                        Blurb = L("This project's installers have been Authenticode-signed since 6.3.1, by Progressive Robot Ltd through Azure Artifact Signing; the releases before 6.3.1 carry no signature, and this refuses them. It checks, when an installer is downloaded, that Windows trusts the signature - WinVerifyTrust against this machine's trusted roots, with no revocation check - and not who made it: that is the job of the Sigstore signature, which every release carries and which is checked either way.")
                     },
                     new NumberSetting { Key = "UpdateServiceWaitSeconds", Minimum = 5, Maximum = 86400, Default = "180", Label = L("Seconds to wait for the service after an apply before rolling back") },
                     new TextSetting
                     {
                        Key = "UpdateSourceRepository",
                        Label = L("Repository the signing certificate must name (empty = this project; - = do not check)"),
                        Placeholder = "https://gitlab.example.net/mail/hmailserver", // no-loc
                        Blurb = L("The last five settings on this card are for a site that builds its own releases and signs them with its own Sigstore, or that mirrors this project's. Leave every one of them empty and the server trusts only an installer signed by one of this project's two release workflows - the GitHub one for the releases up to 6.3.3, the GitLab one for the releases after - and recorded in the public transparency log.")
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
               break;

            // "Advanced INI settings" filed by storage mechanism, which is the one
            // thing an administrator never knows and never needs to. Everything on
            // it that belonged to a feature with a page of its own has gone to that
            // page; what is left is genuinely server-wide, so the page is now named
            // for that rather than for the file the values happen to live in. The
            // nav key stays "hardening" and the old titles stay as search aliases,
            // so every existing link and bookmark still lands here.
            case Section.Hardening:
               Header.PageKey = "hardening";
               Header.Subtitle = L("Server-wide ceilings, durability and abuse controls that belong to no single protocol or feature. The defaults are safe; change these only with a specific reason.");
               cards_.Add(new CardDef
               {
                  Title = L("Timeouts and queue bounds"),
                  Blurb = L("Server-wide ceilings that keep one slow operation from holding a resource forever. Note that 0 does not mean the same thing for all of them - each label says."),
                  Settings =
                  {
                     new NumberSetting
                     {
                        Key = "FinalizationTimeout", Minimum = 0,
                        Default = "240",
                        Label = L("Message finalisation deadline (seconds; 0 = no time limit)"),
                        Blurb = L("How long the server will go on finalising an accepted message before answering 451 and asking the sender to retry, rather than holding the connection open indefinitely.")
                     },
                     new NumberSetting { Key = "DNSQueryTimeout", Minimum = 0, Maximum = 86400, Default = "10", Label = L("DNS query timeout (seconds; 0 = no time limit)") },
                     new NumberSetting { Key = "ClientSessionCeiling", Minimum = 0, Default = "1800", Label = L("Absolute lifetime of one client session (seconds; 0 = no time limit)") },
                     new NumberSetting { Key = "DBConnectionAcquireTimeout", Minimum = 0, Default = "60", Label = L("Wait for a free database connection (seconds; 0 = wait for ever)") },
                     // At most 4,294,967 seconds: the watchdog is a 32-bit count of
                     // milliseconds, which a longer timeout wrapped to a few hundred - every
                     // event script then ended at once. The server clamps to it as well
                     // (ScriptServer::WatchdogMilliseconds; release review finding CP-5).
                     new NumberSetting { Key = "ScriptTimeout", Minimum = 0, Maximum = 4294967, Default = "60", Label = L("Event script execution timeout (seconds; 0 = no time limit)") },
                     new NumberSetting { Key = "ExternalProcessTimeout", Minimum = 0, Default = "300", Label = L("External process timeout (seconds; 0 = no time limit)"), Blurb = L("For example a command-line virus scanner.") },
                     new NumberSetting { Key = "AsyncQueueStallThreshold", Minimum = 0, Default = "120", Label = L("Report the async work queue as stalled after (seconds; 0 = never)") },
                     new NumberSetting { Key = "AsyncQueueReservedThreads", Minimum = 0, Default = "2", Label = L("Threads reserved so the async queue cannot be starved (0 = none)") }
                  }
               });
               // Scanner timeouts moved to the scanner they configure: SpamAssassin
               // on the Anti-spam page and ClamAV on the Anti-virus page. The
               // resolver settings moved to Network > DNS resolver.
               cards_.Add(new CardDef
               {
                  Title = L("Received headers"),
                  Blurb = L("Submission identity handling and the diagnostic headers added to received mail. (Where SMTP AUTH is offered is on the Authentication page.)"),
                  Settings =
                  {
                     new TextSetting { Key = "AuthUserReplacementIP", Address = true, Label = L("Replace the client IP for authenticated users (empty = keep the real IP)"), Placeholder = "127.0.0.1" },
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
                  Blurb = L("What happens to a TCP connection this server refuses before any protocol conversation starts: one from an address no IP range allows, or one over that range's connection limit. Both of those are configured on the IP ranges page."),
                  Settings =
                  {
                     new NumberSetting
                     {
                        Key = "BlockedIPHoldSeconds", Minimum = 0,
                        Default = "0",
                        Label = L("Hold a refused connection open before dropping it (seconds; 0 = drop immediately)"),
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
                     new BoolSetting { Key = "MessageStoreConsistencyCheck", Default = false, Label = L("Periodically cross-check message rows against files on disk"), Blurb = L("Read-only: it writes a report when the two diverge.") }
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
                  Blurb = L("Ceilings on what one authenticated account may submit over a rolling period - the brake on a compromised account being used to spam. Counted per account at SMTP submission: messages and envelope recipients separately, and a message refused by the limit gets a temporary error so a real client retries later."),
                  // Why the rows are Not read, where they are: the server keeps them in
                  // its own hMailServer.ini, which no COM property reaches (CPB-2).
                  Warnings = { new WarningDef { Aid = "sendinglimits-not-read", First = true, Compute = () => SendingLimitsNotice_(store_) } },
                  Settings =
                  {
                     new NumberSetting
                     {
                        Key = "MaxMessagesPerAccountPerPeriod", Minimum = 0,
                        Section = "SendingLimits",
                        Default = "0",
                        Label = L("Maximum messages per account per period (0 = no limit)"),
                        Placeholder = "500"
                     },
                     new NumberSetting
                     {
                        Key = "MaxRecipientsPerAccountPerPeriod", Minimum = 0,
                        Section = "SendingLimits",
                        Default = "0",
                        Label = L("Maximum recipients per account per period (0 = no limit)"),
                        Placeholder = "2000",
                        Blurb = L("Recipients are the stricter measure: one message to two thousand addresses is two thousand recipients.")
                     },
                     new NumberSetting
                     {
                        Key = "PeriodHours", Minimum = 1, Maximum = 168,
                        Section = "SendingLimits",
                        Default = "24",
                        Label = L("Period length (hours, 1 to 168)"),
                        Blurb = L("The default is 24, and 168 is a week.")
                     },
                     new NumberSetting
                     {
                        Key = "StateSaveIntervalSeconds", Minimum = 1, Maximum = 3600,
                        Section = "SendingLimits",
                        Default = "10",
                        Label = L("Save the counters to disk every (seconds, 1 to 3,600)"),
                        Blurb = Sentences(L("How much sending history a crash can forget."), L("The default is 10."))
                     },
                     new SectionLinesSetting
                     {
                        Key = "SendingLimitsOverrides",
                        Section = "SendingLimitsOverrides",
                        Label = L("Per-address overrides, one per line: address=messages:recipients[:hours]"),
                        Placeholder = "newsletter@example.com=5000:50000\nceo@example.com=0:0",
                        Blurb = L("An override replaces the global ceilings for that address; 0:0 exempts it entirely. Without the optional :hours the override uses the global period.")
                     }
                  }
               });
               cards_.Add(new CardDef
               {
                  Title = L("Submission rate limits"),
                  Blurb = L("Throttle abusive senders submitting to this server. Limits are per minute; 0 disables the limit."),
                  Settings =
                  {
                     new NumberSetting { Key = "MaxSubmissionsPerIPPerMinute", Minimum = 0, Default = "0", Label = L("Maximum authenticated submissions per client IP per minute (0 = no limit)"), Placeholder = "60" }
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
                  Blurb = L("Bounces and virus notifications are sent from mailer-daemon@<domain>. This overrides the <domain> part."),
                  Settings =
                  {
                     new TextSetting { Key = "DaemonAddressDomain", Label = L("Domain for the mailer-daemon sender address (empty = the host name)"), Blurb = L("Without a host name, the message's local domain is used, and without that, the machine name."), Placeholder = "example.com" }
                  }
               });
               cards_.Add(new CardDef
               {
                  Title = L("Low-level tuning"),
                  Blurb = L("Specialist knobs - leave at the defaults unless you have a specific reason."),
                  Settings =
                  {
                     new NumberSetting { Key = "SMTPDMaxSizeDrop", Minimum = 0, Default = "0", Label = L("Drop inbound messages larger than this mid-transfer (KB; 0 = off)") },
                     // SAMoveVsCopy went to Anti-spam > SpamAssassin: it configures
                     // how a message reaches that scanner, and belongs with the host,
                     // port and timeouts that configure the rest of the same handoff.
                     new NumberSetting { Key = "LoadHeaderReadSize", Minimum = 64, Maximum = 50000, Default = "4000", Label = L("Header read chunk size (bytes)") },
                     new NumberSetting { Key = "LoadBodyReadSize", Minimum = 64, Maximum = 50000, Default = "4000", Label = L("Body read chunk size (bytes)") }
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
                  Blurb = L("Chooses the envelope hMailServer puts around the secrets it stores for its own use: machine-scoped Windows DPAPI, which cannot be decrypted on any other machine, or the legacy Blowfish scheme, which can. Leave it on."),
                  Settings =
                  {
                     new BoolSetting
                     {
                        Key = "ProtectStoredSecretsWithDPAPI",
                        Default = true,
                        Label = L("Protect stored secrets with Windows DPAPI"),
                        Blurb = L("It governs two kinds of stored secret. Only the password pepper is always stored as you typed it, so hMailServer.ini should still be readable only by Administrators and by the account the service runs as.")
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
                        Blurb = L("Runs hMailServer.exe /GrantServiceAccess as an administrator, after Windows asks: NT SERVICE\\hMailServer is given Modify on the program folder's own Data, Logs, Temp and Database folders and on the folders an administrator has approved, and Read on the program folder - never hMailServer.ini. It can be run again at any time, and removes nobody else's permission.")
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
                              Text = L("What an account of its own changes. hMailServer.ini is read-only to it, on purpose: the administrator password, its second factor, the folders and the database are changed as the Control Panel says when you try, and settings are kept in the database as always. A folder set later outside the program folder - a backup or archive folder - is reached once Grant access approves it. An account other than NT SERVICE\\hMailServer is set in services.msc and needs the \"Log on as a service\" right; an external database with Windows authentication needs a login for it. A service that cannot log on reports error 1069 in the Windows event log and does not start.")
                           };
                        }
                     }
                  }
               });
               cards_.Add(new CardDef
               {
                  Title = L("Settings that used to be on this page"),
                  Blurb = L("Each of these now sits with the rest of its own feature. Nothing was removed and no value changed; only the page it is edited on."),
                  Settings =
                  {
                     new ElsewhereSetting("antispam", L("Greylisting record expiration, and SpamAssassin move-vs-copy")),
                     new ElsewhereSetting("protocols", L("IMAP search time and size limits, and the eight per-protocol idle timeouts")),
                     new ElsewhereSetting("tls", L("TLS session tickets, session cache size, resumption lifetime and ticket-key rotation")),
                     new ElsewhereSetting("delivery", L("MX hosts tried per attempt, times the attempt number")),
                     new ElsewhereSetting("performance", L("Maximum parallel external POP3 fetch threads")),
                     new ElsewhereSetting("security", L("SRS, BATV and the plain envelope-sender rewrite for forwarded mail"), N("Forwarding")),
                     new ElsewhereSetting("relays", L("The PROXY protocol and XCLIENT, with which a front-end proxy passes on the real client address"))
                  }
               });
               break;

            case Section.Authentication:
               Header.PageKey = "authentication";
               Header.Subtitle = L("How mailbox users prove who they are: how their passwords are stored, where SMTP AUTH is offered, and tokens from an external identity provider.");

               // OAuth2 is the last card (the owner's decision of 9 October 2026,
               // review finding F238): the page opened on its fifteen fields, live
               // while OAuth2 is off, with password storage and SMTP AUTH - what every
               // server uses - below the fold. Defined here and added after the others.
               var oauth2 = new CardDef
               {
                  Title = L("OAuth2 / external identity provider"),
                  Blurb = L("Accept OAuth2 / OpenID Connect bearer tokens (XOAUTH2) from an external identity provider for IMAP, POP3 and SMTP submission, validated against the issuer's signing key. The mailbox named by the token still has to exist as a local account."),
                  Settings =
                  {
                     new BoolSetting { Key = "OAuth2Enabled", Default = false, Label = L("Accept OAuth2 bearer tokens (XOAUTH2)"), Blurb = L("While this is off the server uses none of the settings below, so they cannot be changed until it is on.") },
                     new BoolSetting { Key = "OAuth2RequireTLS", DependsOn = OAuth2Switch, Default = true, Label = L("Require TLS for token authentication") },
                     new TextSetting { Key = "OAuth2Issuer", DependsOn = OAuth2Switch, Label = L("Expected token issuer (iss)"), Placeholder = "https://login.microsoftonline.com/<tenant>/v2.0" }, // no-loc: an example issuer, the same in every language
                     new TextSetting { Key = "OAuth2Audience", DependsOn = OAuth2Switch, Label = L("Expected audience (aud)"), Blurb = L("Usually the client ID of this server's registration with the identity provider.") },
                     new TextSetting { Key = "OAuth2AllowedAlgorithms", DependsOn = OAuth2Switch, Default = "RS256", Label = L("Allowed signing algorithms (comma separated)"), Placeholder = "RS256, ES256" }, // no-loc
                     // Empty is "email" to the validator (OAuth2TokenValidator::LoadConfig).
                     new TextSetting { Key = "OAuth2UsernameClaim", DependsOn = OAuth2Switch, Default = "email", EmptyMeans = "email", Label = L("Claim that holds the mailbox address") },
                     new PathSetting { Key = "OAuth2PublicKeyFile", DependsOn = OAuth2Switch, FileFilter = L("PEM/key files (*.pem;*.crt;*.cer;*.key;*.pub)|*.pem;*.crt;*.cer;*.key;*.pub|All files (*.*)|*.*"), Label = L("RSA/EC public key file (PEM, for RS*/ES* tokens)") },
                     new SecretSetting { Key = "OAuth2HmacSecret", DependsOn = OAuth2Switch, Label = L("Shared HMAC secret (only for HS256/384/512 tokens)"), Hint = L("Only needed for HS* algorithms") },
                     // The two round trips to the provider, both off unless set: its
                     // published keys instead of a hand-copied PEM file, and a live
                     // revocation check after a token verifies.
                     new TextSetting { Key = "OAuth2JwksUrl", DependsOn = OAuth2Switch, Label = L("JWK Set URL (empty = the key file only)"), Blurb = L("Where the provider publishes its signing keys."), Placeholder = "https://login.microsoftonline.com/<tenant>/discovery/v2.0/keys" }, // no-loc: an example URL, the same in every language
                     new NumberSetting { Key = "OAuth2JwksCacheSeconds", DependsOn = OAuth2Switch, Minimum = 10, Default = "3600", Label = L("Seconds to keep the JWK Set before re-fetching (a token naming an unknown key ID re-fetches at once)") },
                     new TextSetting { Key = "OAuth2IntrospectionUrl", DependsOn = OAuth2Switch, Label = L("Token introspection endpoint (empty = no revocation check)"), Blurb = L("The provider's RFC 7662 endpoint, which says whether a token is still valid."), Placeholder = "https://idp.example.com/oauth2/introspect" }, // no-loc: an example URL, the same in every language
                     new TextSetting { Key = "OAuth2IntrospectionClientId", DependsOn = OAuth2Switch, Label = L("Client ID this server introspects as") },
                     new SecretSetting { Key = "OAuth2IntrospectionClientSecret", DependsOn = OAuth2Switch, Label = L("Client secret for introspection"), Hint = L("Issued by the provider together with the client ID") },
                     new NumberSetting { Key = "OAuth2IntrospectionCacheSeconds", DependsOn = OAuth2Switch, Minimum = 0, Default = "300", Label = L("Seconds an introspection verdict is reused (never past the token's own expiry)") },
                     new BoolSetting { Key = "OAuth2IntrospectionFailOpen", DependsOn = OAuth2Switch, Default = false, Label = L("Accept a token when the introspection endpoint cannot answer (availability over the revocation check)") }
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
               };
               cards_.Add(new CardDef
               {
                  Title = L("Password storage"),
                  Blurb = L("How account passwords are hashed, how much work a new hash costs, and the weakest stored hash still allowed to sign in. Existing passwords keep their current hash until they are next changed - or, when a work factor was raised, until they are next used to sign in."),
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
                        Label = L("Reject sign-ins using a weaker stored hash than"),
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
                     new NumberSetting { Key = "PasswordHashIterations", Minimum = 10000, Maximum = 10000000, ZeroIsBuiltIn = true, Default = "0", Label = L("PBKDF2 iterations for new hashes (10,000 to 10,000,000; 0 = the built-in 210,000)") },
                     new NumberSetting { Key = "PasswordHashMemoryKB", Minimum = 4096, Maximum = 1048576, ZeroIsBuiltIn = true, Default = "0", Label = L("Argon2id memory for new hashes (KiB, 4,096 to 1,048,576; 0 = the built-in 19,456)") },
                     new NumberSetting { Key = "PasswordHashTimeCost", Minimum = 1, Maximum = 20, ZeroIsBuiltIn = true, Default = "0", Label = L("Argon2id passes for new hashes (1 to 20; 0 = the built-in 2)") }
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
               cards_.Add(oauth2);
               break;

            case Section.Dns:
               Header.PageKey = "dns";
               Header.Subtitle = L("How this server resolves MX, PTR, SPF, DKIM, DMARC and block list lookups.");
               cards_.Add(new CardDef
               {
                  Title = L("Name servers"),
                  Blurb = L("Which resolver hMailServer queries. The override takes a single IPv4 address and is used on port 53; DNSSEC validation and its trust anchors are on the Transport security page."),
                  Settings =
                  {
                     new TextSetting { Key = "DNSServer", Address = true, Rule = SettingAddressRule.DnsOverride, Label = L("Override DNS server (empty = the name servers Windows uses)"), Placeholder = "1.1.1.1" }
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
                  Blurb = L("Whether lookups may be answered from the Windows DNS resolver cache. Off, every lookup goes to the network."),
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
               Header.PageKey = "webservices";
               Header.Subtitle = L("The built-in HTTP listener that serves mail-client autoconfiguration and MTA-STS policies for your local domains.");
               cards_.Add(new CardDef
               {
                  Title = L("Listener"),
                  Blurb = L("Nothing below is served until a port is set here. The server answers on either port, but an MTA-STS policy is only valid over HTTPS and Outlook only accepts autodiscover over HTTPS - so in practice set the HTTPS port and a certificate covering the host names clients use."),
                  Settings =
                  {
                     new NumberSetting { Key = "WebServicesHttpPort", Minimum = 0, Maximum = 65535, Default = "0", Label = L("HTTP port (0 = off)"), Blurb = L("80 is the port clients expect.") },
                     new NumberSetting { Key = "WebServicesHttpsPort", Minimum = 0, Maximum = 65535, Default = "0", Label = L("HTTPS port (0 = off)"), Blurb = L("443 is the port clients expect.") },
                     new TextSetting { Key = "WebServicesBindAddress", Address = true, Default = "0.0.0.0", Label = L("Bind address") },
                     new TextSetting { Key = "WebServicesTrustedProxies", Label = L("Trusted proxies whose X-Forwarded-Proto: https is believed (comma separated)"), Blurb = L("Addresses; the loopback address is always trusted.") },
                     new PathSetting { Key = "WebServicesCertificateFile", FileFilter = L("PEM/certificate files (*.pem;*.crt;*.cer)|*.pem;*.crt;*.cer|All files (*.*)|*.*"), Label = L("TLS certificate file (PEM, optional)"), Blurb = L("Left empty, the ACME certificate is used.") },
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
                     new WordSetting
                     {
                        Key = "MtaStsPolicyMode",
                        Default = "enforce",
                        EmptyMeans = "enforce",
                        Label = L("MTA-STS policy mode"),
                        Options = new (string, string)[]
                        {
                           ("enforce", L("enforce - senders that honour MTA-STS deliver only over valid TLS to the hosts listed")),
                           ("testing", L("testing - senders deliver as before and report what would have failed")),
                           ("none", L("none - no policy: senders that cached one stop applying it"))
                        }
                     },
                     new NumberSetting { Key = "MtaStsPolicyMaxAge", Minimum = 86400, Maximum = 31557600, Default = "604800", Label = L("Policy max age (seconds, 86,400 to 31,557,600)"), Blurb = L("The default, 604,800, is a week; 86,400 is a day and 31,557,600 a year, the longest RFC 8461 allows.") },
                     new TextSetting { Key = "MtaStsPolicyMx", Label = L("Policy MX host patterns (empty = derive from each domain's MX)"), Placeholder = "mail.example.com, *.example.com" } // no-loc
                  }
               });
               cards_.Add(new CardDef
               {
                  // The server serves both under /dav/ on this listener, with no switch to
                  // turn them off (WebServicesServer.cpp), and an empty setting sends the
                  // discovery URL there (BuiltInDavUrl_): the card said hMailServer
                  // implemented neither and that empty answered 404 (release review
                  // finding CPV-1). Only a value that is set and unusable answers 404.
                  Title = L("Calendar and contacts discovery (CalDAV / CardDAV)"),
                  Blurb = L("hMailServer serves CalDAV and CardDAV itself, under /dav/ on this listener, over HTTPS, to each account by its mail address and password. These settings send the well-known discovery URLs (RFC 6764) to another server instead, for calendars and contacts kept there; empty, they lead to this server's own /dav/, so a client configured with a user's mail address finds their calendar without being told a second address."),
                  Settings =
                  {
                     new TextSetting
                     {
                        Key = "CalDavRedirectUrl",
                        Label = L("Redirect /.well-known/caldav to (empty = this server's own /dav/)"),
                        Placeholder = "https://calendar.example.com/dav/",
                        Blurb = L("Must be an absolute URL. A relative one is refused and the discovery URL answers 404 instead, with the reason reported once in the error log.")
                     },
                     new TextSetting
                     {
                        Key = "CardDavRedirectUrl",
                        Label = L("Redirect /.well-known/carddav to (empty = this server's own /dav/)"),
                        Placeholder = "https://contacts.example.com/dav/",
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
         int shownTab = ClearTabs_();
         warningRows_.Clear();
         store_.ForgetFallback();
         readFromFile_ = null;
         readFromFileNotice_ = null;

         AssignAccessibleNames();

         if (!store_.IsAvailable)
         {
            SetState_(SettingsPageState.Unavailable,
               L("hMailServer.ini was not found on this machine. These settings can only be edited on the server itself."));
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
               fields.Children.Add(setting.Built = setting.CreateEditor(store_));

            if (fields.Children.Count > 0 && fields.Children[fields.Children.Count - 1] is FrameworkElement last)
               last.Margin = new Thickness(0);

            foreach (WarningDef def in card.Warnings)
            {
               // A warning that is the card's first row goes above its fields, with
               // the gap under it a field row keeps (WarningDef.First).
               if (def.First)
               {
                  var first = (FrameworkElement)BuildWarningRow_(def);
                  first.Margin = new Thickness(0, 0, 0, DesignTokens.Space.Md);
                  fields.Children.Insert(0, first);
               }
               else
                  fields.Children.Add(BuildWarningRow_(def));
            }

            var section = new SettingsSection
            {
               Heading = card.Title,
               Description = card.Blurb,
               Content = fields,
               Margin = new Thickness(0)
            };
            var surface = new Card { Content = section };
            surface.SetResourceReference(MarginProperty, "AppCardGap");
            PanelFor_(card).Children.Add(surface);
         }

         // A setting that counts only while a switch is on follows the switch, once
         // both exist (review finding F197). A message a save left on it goes as the
         // switch goes off, with the row it can no longer be corrected in.
         foreach (Setting setting in cards_.SelectMany(card => card.Settings).Where(setting => setting.DependsOn != null))
         {
            if (FindSetting_(setting.DependsOn) is BoolSetting master && master.Box != null && setting.Built != null)
            {
               setting.Master = master;
               DialogFields.EnabledBy(master.Box, setting.Built);
               master.Box.Unchecked += (s, e) => { if (setting.Field != null) setting.Field.Error = null; };
            }
         }

         ShowTab_(shownTab);

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

         PathText.Visibility = Visibility.Visible;

         // The server could not be asked, and the values on screen are the ones in
         // hMailServer.ini on this computer: the server's cache of its settings
         // store, which may not be what it runs. Save waits for the server, which
         // every write needs - where the page looked as it always did, Save and
         // all. The pill and the notice say it in the state its cause makes
         // (SettingsPageStatus.ForFallback), at that state's level, which is the
         // cause's (ComErrorText.LevelFor): a lost link is Connection lost at
         // Critical, as the shell's pill, the Dashboard and the setup checklist say
         // it, and an older server without GetIniSetting, which answered and is
         // connected, is Read from file at Warning.
         if (store_.FallbackCause != null)
         {
            readFromFile_ = store_.FallbackCause;
            // The reason last: DescribeComError's sentences are written to end a
            // message, and one ends in the runtime's text in brackets with no full
            // stop, so a sentence after it ran on from it.
            readFromFileNotice_ = F("The server could not be asked for these settings, so the page shows the values in hMailServer.ini on this computer, which may differ from what the server runs, and nothing can be saved until the server answers. Why: {0}",
               ServerSession.DescribeComError(readFromFile_));
            SayReadFromFile_();
            return;
         }

         // Since schema 6042 the settings store is the database and the file its
         // cache: the page edits the store. A card that keeps its settings in a
         // section of the file of their own - the per-account sending limits - says
         // so in its own words, and the line names the file for it.
         // The file is named only where it is the server's: a workstation's is the
         // workstation's, and the rows kept in it are Not read (CPB-2).
         PathText.Text = cards_.SelectMany(card => card.Settings).Any(setting => setting is NumberSetting { Section: not null } or SectionLinesSetting)
            && FileSectionIsTheServers_(store_)
            ? F("Stored in the server's settings database; the settings a card says are kept in hMailServer.ini are in {0}.", store_.IniPath)
            : L("Stored in the server's settings database.");
         SaveButton.IsEnabled = true;
         SetState_(SettingsPageState.Clean, null);
      }

      /// <summary>
      /// The sending limits card's word for its rows that are Not read here
      /// (<see cref="Setting.NotReadRow"/>), or null when the file here is the server's and
      /// may be read: the server reads [SendingLimits] and [SendingLimitsOverrides] from
      /// its own file alone (RateLimiter), and no COM property carries them. A Control
      /// Panel installed on a workstation has a hMailServer.ini of its own, and the rows
      /// showed and saved that one's values as the server's: a brake of 500 messages read
      /// 0, "no limit", and an edit of it said every setting on the page was in effect
      /// (release review part B, CPB-2).
      /// </summary>
      private static WarningState SendingLimitsNotice_(IniFeatureStore store)
      {
         // (FileSectionIsTheServers_'s reasons, in words.)
         string denied = null;
         switch (store?.LocalSectionAccess(out denied))
         {
            case IniFeatureStore.FileSectionAccess.OtherComputer:
               return new WarningState
               {
                  Level = StatusLevel.Warning,
                  Text = L("The settings marked Not read are sending limits the server keeps in the hMailServer.ini on its own computer, and no COM property carries them; this Control Panel is signed in to a server on another computer, and the file here is this computer's own. Start the Control Panel on the server's computer to see and change them; saving leaves them as they are on the server.")
               };
            case IniFeatureStore.FileSectionAccess.Denied:
               return new WarningState
               {
                  Level = StatusLevel.Warning,
                  Text = F("The settings marked Not read are sending limits the server keeps in hMailServer.ini, which could not be read: {0} No COM property carries them, and from 6.3.6 the file belongs to the administrators. Close the Control Panel and start it with Run as administrator to see and change them; saving leaves them as they are on the server.", denied)
               };
            default:
               return null;
         }
      }

      /// <summary>
      /// Whether a section of the file other than [Settings] - [SendingLimits],
      /// [SendingLimitsOverrides] - is the server's here (IniFeatureStore.LocalSectionAccess):
      /// the server reads those keys from its own file alone (RateLimiter), and no COM
      /// property carries them (release review part B, CPB-2).
      /// </summary>
      private static bool FileSectionIsTheServers_(IniFeatureStore store)
         => store != null && store.LocalSectionAccess(out _) == IniFeatureStore.FileSectionAccess.Readable;

      /// <summary>The tab strip of a tabbed section (<see cref="FrameTabs_"/>), or null.</summary>
      private TabControl cardTabs_;

      /// <summary>Each tab's column of cards, by its header, for this build of the page.</summary>
      private readonly Dictionary<string, StackPanel> tabPanels_ = new(StringComparer.Ordinal);

      // The markup's panel of the notice and the line that names the store (round
      // the CardsPanel a tabbed section leaves empty), the place under the header it
      // starts in and goes back to while no tab is built, and a place above the
      // first card of each tab, in the strip's order (PlaceTabHead_).
      private Panel tabHead_;
      private ContentControl tabHeadHome_;
      private readonly List<ContentControl> tabHeadSlots_ = new();

      /// <summary>
      /// Below this height of the page, a tabbed section shows its subtitle as the
      /// title's tool tip (<see cref="FitTabbedHeader_"/>): the height at which
      /// ServerSettingsView's pages do (review finding F290) - a 760 by 520 window
      /// gives a page some 395 pixels, 1280 by 800 some 675.
      /// </summary>
      internal const double TabbedSubtitleToolTipBelow = 600;

      /// <summary>
      /// Draws a section whose cards name tabs as ServerSettingsView draws its pages:
      /// the header with Reload and Save changes, the notice and the line that names
      /// the store above a tab strip, and each tab's cards scrolling under it, so that
      /// Save is in view however long a tab is. Transport security was one scroll of
      /// ten cards under a header that scrolled away with Save (review finding F228).
      /// The markup is the untabbed pages' frame, a scroll viewer round the header and
      /// the cards; this takes the header and the panel under it out of it, into a
      /// grid with the strip below, and puts the grid in its place.
      ///
      /// Only the header and the strip stay pinned, as on ServerSettingsView's pages
      /// (review finding F290): the notice and the line that names the store go into
      /// the tab on screen, above its first card, and scroll away with the settings
      /// (<see cref="PlaceTabHead_"/>), and in a window under 600 pixels of page the
      /// subtitle is the title's tool tip (<see cref="FitTabbedHeader_"/>). Pinned, the
      /// title, a three-line German subtitle, the notice, the line and the strip left
      /// Transport security's cards some 140 pixels at 760 by 520, and with the notice
      /// of a page read from the file some 40 - one card's heading.
      /// </summary>
      private void FrameTabs_()
      {
         if (cardTabs_ != null || Header.Parent is not Panel outer || CardsPanel.Parent is not Panel inner)
            return;

         outer.Children.Remove(Header);
         outer.Children.Remove(inner);

         var frame = new Grid();
         frame.SetResourceReference(MarginProperty, "AppPagePadding");
         frame.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
         frame.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
         frame.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });

         Grid.SetRow(Header, 0);
         frame.Children.Add(Header);
         tabHead_ = inner;
         tabHeadHome_ = new ContentControl { Focusable = false, IsTabStop = false, Content = inner };
         Grid.SetRow(tabHeadHome_, 1);
         frame.Children.Add(tabHeadHome_);

         cardTabs_ = new TabControl
         {
            Background = Brushes.Transparent,
            BorderThickness = new Thickness(0),
            Padding = new Thickness(0, DesignTokens.Space.Sm, 0, 0)
         };
         AutomationProperties.SetAutomationId(cardTabs_, "feature-settings-tabs");
         Grid.SetRow(cardTabs_, 2);
         frame.Children.Add(cardTabs_);

         // The strip's own changes only: a list on a tab raises SelectionChanged too,
         // and it bubbles here.
         cardTabs_.SelectionChanged += (s, e) =>
         {
            if (ReferenceEquals(e.OriginalSource, cardTabs_))
               PlaceTabHead_();
         };
         SizeChanged += (s, e) => FitTabbedHeader_(e.NewSize.Height);

         Content = frame;
      }

      /// <summary>
      /// Puts the notice and the line that names the store above the first card of
      /// the tab on screen, where they scroll with its settings, or under the header
      /// while no tab is built - as on a page that cannot read hMailServer.ini, whose
      /// notice is all it shows (review finding F290).
      /// </summary>
      private void PlaceTabHead_()
      {
         if (tabHead_ == null || tabHeadHome_ == null)
            return;

         int at = cardTabs_?.SelectedIndex ?? -1;
         ContentControl target = at >= 0 && at < tabHeadSlots_.Count ? tabHeadSlots_[at] : tabHeadHome_;
         if (ReferenceEquals(tabHead_.Parent, target))
            return;

         if (tabHead_.Parent is ContentControl from)
            from.Content = null;
         target.Content = tabHead_;
      }

      /// <summary>
      /// Below <see cref="TabbedSubtitleToolTipBelow"/> of page, the subtitle is the
      /// tool tip of the title's row rather than a line under it, and above it a line
      /// again; it stays the header's help text either way, so a screen reader hears
      /// it as before (review finding F290). A page of tabs pins its header above the
      /// strip, and in a short window the lines it pins are the settings' room.
      /// </summary>
      private void FitTabbedHeader_(double pageHeight)
      {
         if (cardTabs_ == null || string.IsNullOrEmpty(Header.Subtitle))
            return;

         Header.ApplyTemplate();
         if (Header.Template?.FindName("PART_Subtitle", Header) is not FrameworkElement subtitle
             || Header.Template.FindName("PART_Title", Header) is not FrameworkElement title
             || VisualTreeHelper.GetParent(title) is not FrameworkElement titleRow)
            return;

         if (pageHeight > 0 && pageHeight < TabbedSubtitleToolTipBelow)
         {
            subtitle.Visibility = Visibility.Collapsed;
            titleRow.ToolTip = Header.Subtitle;
         }
         else
         {
            subtitle.ClearValue(VisibilityProperty);
            titleRow.ClearValue(ToolTipProperty);
         }
      }

      /// <summary>Empties the tab strip for a build of the page; the tab that was showing, or -1.</summary>
      private int ClearTabs_()
      {
         tabPanels_.Clear();
         if (cardTabs_ == null)
            return -1;

         int shown = cardTabs_.SelectedIndex;

         // The notice back under the header while the tabs that held it go.
         tabHeadSlots_.Clear();
         PlaceTabHead_();
         cardTabs_.Items.Clear();
         return shown;
      }

      /// <summary>
      /// The column a card goes in: its tab's, made the first time the tab is asked
      /// for, or the page's own on a page without tabs. A tab's column is the form's
      /// width and keeps clear of the tab's scroll bar, as ServerSettingsView's are.
      /// </summary>
      private Panel PanelFor_(CardDef card)
      {
         if (cardTabs_ == null || card.Tab == null)
            return CardsPanel;

         if (tabPanels_.TryGetValue(card.Tab, out StackPanel panel))
            return panel;

         // As wide as the form width allows whatever its cards hold, so that the cards of
         // every tab end at one edge (FormColumnPanel; review finding F295).
         panel = new FormColumnPanel
         {
            MaxWidth = DesignTokens.FormWidth,
            HorizontalAlignment = HorizontalAlignment.Left,
            Margin = new Thickness(0, DesignTokens.Space.Xs, DesignTokens.Space.Md, DesignTokens.Space.Xs)
         };
         tabPanels_[card.Tab] = panel;

         // The notice's place in this tab, above everything (PlaceTabHead_).
         var headSlot = new ContentControl { Focusable = false, IsTabStop = false };
         panel.Children.Add(headSlot);
         tabHeadSlots_.Add(headSlot);

         cardTabs_.Items.Add(new TabItem
         {
            Header = card.Tab,
            Content = new ScrollViewer { VerticalScrollBarVisibility = ScrollBarVisibility.Auto, Content = panel }
         });
         return panel;
      }

      /// <summary>
      /// After a build, the tab that was showing before it - a Reload or a return to
      /// the page keeps the reader where they were - or the first.
      /// </summary>
      private void ShowTab_(int shown)
      {
         if (cardTabs_ == null || cardTabs_.Items.Count == 0)
            return;

         cardTabs_.SelectedIndex = shown >= 0 && shown < cardTabs_.Items.Count ? shown : 0;
         PlaceTabHead_();
      }

      /// <summary>An editor changed: the warnings that read it, and the pill.</summary>
      private void EditorChanged_()
      {
         RefreshWarnings_();
         saveState_.Follow();
      }

      /// <summary>
      /// What the page says while this build's values are the file's
      /// (<see cref="readFromFile_"/>): the line under the header names the file,
      /// Save is held, and the pill and the notice say why, in the state the cause
      /// makes and at its level. Said when the page is built, and again when a save
      /// is refused for it.
      /// </summary>
      private void SayReadFromFile_()
      {
         PathText.Text = F("Read from {0} on this computer.", store_.IniPath);
         SaveButton.IsEnabled = false;
         tryAgain_ ??= ReadFailure.TryAgainButton(() => Reload_Click(this, null));
         SetState_(SettingsPageStatus.ForFallback(readFromFile_), readFromFileNotice_, null, tryAgain_);
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
      private void SetState_(SettingsPageState state, string message, StatusLevel? noticeLevel = null, object action = null)
         => saveState_.Settle(state, message, noticeLevel, action);

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
            if (state.Page == null)
               row.Notice.Content = null;
            else
            {
               if (row.Action == null || row.ActionPage != state.Page)
               {
                  row.Action = NoticeActions.OpenPage(state.Page, (row.Def.Aid ?? "warning") + "-open-" + state.Page);
                  row.ActionPage = state.Page;
               }
               row.Notice.Content = row.Action;
            }
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
         EnterUpdates_();
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

         // Values this build read from the file when the server could not be asked
         // are not written back over what the server holds, and the refusal is said:
         // the question before leaving the page, whose Save comes here, stays on it
         // under the notice that says why. A read that fell back after a build that
         // read cleanly - a warning's, on an editor change - does not stop the save,
         // whose write then says why it failed if the server is still not there.
         if (readFromFile_ != null)
         {
            SayReadFromFile_();
            return false;
         }

         // A secret given a new value and ticked for removal both: refused before
         // anything on the page is written, in the notice under the header - until the
         // value is cleared or the box unticked, as the refusal asks, though another
         // edit is still unsaved: the refusal names this secret, and stayed saying the
         // box was ticked under a box unticked.
         SecretSetting conflict = cards_.SelectMany(card => card.Settings).OfType<SecretSetting>().FirstOrDefault(secret => secret.Conflicts);
         if (conflict != null)
         {
            saveState_.Refuse(SecretSetting.ConflictSentence(conflict.Label), () => conflict.Conflicts);
            return false;
         }

         // A number outside the range the server reads it in, an address that is not
         // one: said on every field at fault, the keyboard put in the first, and
         // nothing on the page written (review finding F220) - where the save wrote
         // whatever was typed. Not a field behind a switch that is off, which cannot
         // be reached: its value is kept as the server has it (Setting.KeptAsStored).
         List<Setting> refused = cards_.SelectMany(card => card.Settings).Where(setting => !setting.Validate()).ToList();
         if (refused.Count > 0)
         {
            // Withdrawn as the reader puts the marked values right, as every refusal a
            // page makes before it writes is (SaveStateNotice): asked again as the
            // editors change.
            saveState_.Refuse(L("Nothing was saved: correct what is marked below, then save again."),
               () => cards_.SelectMany(card => card.Settings).Any(setting => setting.Refusal != null));
            DialogFields.ShowError(refused[0].Field, refused[0].Field.Error);
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
            // A failure, not a refusal: the settings before the one that failed are written.
            saveState_.Fail(F("Could not save: {0}", ServerSession.DescribeComError(ex)));
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

         if (AskRestart_(waiting == null ? null : waitingForRestart))
            RestartService();

         return true;
      }

      /// <summary>
      /// Whether to restart the service now, after a save whose settings wait for it:
      /// Restart now or Later, and Later is what Enter gives, because a restart drops
      /// every open session and a reflexive Enter after Save must not do that.
      /// </summary>
      private static bool AskRestart_(int? waiting)
         => MessageBox.Confirm(Confirmations.RestartAfterSave(waiting), L("Control Panel"), L("_Restart now"),
            cancelByDefault: true, decline: L("_Later"));

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
      /// pending bar shares (Services/ServiceRestart). A restart that left the service
      /// stopped says so, at Critical, and offers the restart again, whose press starts
      /// it (release review finding CP-1): it said "Could not restart the service" over
      /// a timeout, with no way on from the page.
      /// </summary>
      private async void RestartService()
      {
         SetState_(SettingsPageState.Restarting, L("Restarting the hMailServer service\u2026"), StatusLevel.Information);

         (ServiceRestartOutcome outcome, string reason) = await ServiceRestart.RunAsync();
         switch (outcome)
         {
            case ServiceRestartOutcome.NotRestarted:
               SetState_(SettingsPageState.RestartFailed, F("Could not restart the service: {0}", reason));
               return;
            case ServiceRestartOutcome.Stopped:
               SetState_(SettingsPageState.RestartFailed, reason, StatusLevel.Critical,
                  saveState_.RestartAction("feature-restart-service",
                     L("The hMailServer service is not running, so no mail is received or delivered until it is started. Start it now?")));
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
