// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Reflection;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Threading;
using hMailServer.ControlPanel.Services;
using TextBox = Wpf.Ui.Controls.TextBox;
using System.Linq;
using MessageBox = hMailServer.ControlPanel.Views.Dialogs;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// Tabbed, data-driven settings pages backed by the COM Settings object.
   /// Mirrors the classic Administrator layout: each section is a TabControl
   /// whose tabs group related cards. Property paths are dotted
   /// ("AntiSpam.SpamMarkThreshold") and resolved against app.Settings.
   ///
   /// The definition - which tabs, which cards, which settings on each - is the
   /// ten Build&lt;Section&gt; methods below and is the page's whole content; what
   /// turns it into controls is one path, not one per row type. Every row builds
   /// its editor and nothing else, and <see cref="ComSetting.CreateRow"/> wraps it
   /// in the scaffold's <see cref="Scaffold.FieldRow"/>, which draws the caption
   /// with its Alt key, the note under the editor and any validation message, and
   /// gives the editor its accessible name and help text. That is why a row type
   /// can no longer silently drop its note, as the COM-backed check box did for
   /// years: the note is no longer something each row type has to remember to
   /// print. A card is the scaffold's <see cref="Scaffold.Card"/> for the surface
   /// holding a <see cref="Scaffold.SettingsSection"/> for the heading, the
   /// description and the fields - a level-2 heading, so a reader can walk a page
   /// of a hundred and fifty settings section by section.
   /// </summary>
   public partial class ServerSettingsView : UserControl, IPageLifecycle
   {
      public enum Section
      {
         Protocols,
         Delivery,
         AntiSpam,
         AntiVirus,

         /// <summary>
         /// TLS versions, ciphers and remote-certificate verification. Auto-ban
         /// used to be a third tab here; it is <see cref="AutoBan"/> now, because
         /// brute-force lockout and transport encryption share nothing but the
         /// letters SSL.
         /// </summary>
         Tls,
         AutoBan,
         Logging,
         Performance,
         Advanced,
         AdminAccess,

         /// <summary>
         /// The webmail and the other browser pages people sign in to: how they sign
         /// in, how long a session lasts, what a person may add for themselves,
         /// OpenPGP, Web Push and importing from another provider. Nearly all of it is
         /// kept by the server with no COM property and reached over the REST API,
         /// which is the listener that serves the webmail (Services/RestSettings).
         /// </summary>
         Webmail
      }

      // ---- editor model ------------------------------------------------------

      private abstract class ComSetting
      {
         public string Path;   // dotted path under app.Settings
         public string Label;

         /// <summary>
         /// The argument of an indexed COM property - Settings.CountryPolicy("smtp") -
         /// or null for an ordinary one. Read and written with it.
         /// </summary>
         public string Index;

         /// <summary>
         /// Where the server's environment says the value came from - variable or
         /// file - for a row read over the REST API, which answers it beside the value
         /// (&lt;key&gt;_environment); null otherwise, and for every COM row, which the
         /// page ties to the environment by its property's name instead.
         /// </summary>
         public string RestEnvironment;

         /// <summary>
         /// When a save of this row reaches the running server, for a row whose store
         /// does not say so itself: one of SettingApplies' three marks, or null for
         /// none. The SNMP agent's settings, read when the agent starts, are restart.
         /// </summary>
         public string AppliesMark;

         /// <summary>
         /// What a screen reader should call this editor. Assigned by
         /// <see cref="AssignAccessibleNames"/> before the editors are built,
         /// because the answer depends on the whole page - see
         /// <see cref="AccessibleNames"/>.
         /// </summary>
         public string AccessibleName;

         /// <summary>
         /// A caption printed under the editor and attached to it as accessible
         /// help text. Some rows already carried one; the honest statements in
         /// <see cref="SettingClaims"/> about what the server actually does with a
         /// value are the reason it is now on every row type rather than on two of
         /// them.
         /// </summary>
         public string Blurb;

         /// <summary>
         /// Raised when the editor's value changes, so the page can say it holds
         /// unsaved changes. Wired by <see cref="Watch"/>, which each row type
         /// calls on the control it creates; the rows that write nothing - a
         /// button, a live statistic, the read-only inert row - do not call it,
         /// because looking at one changes nothing to save.
         /// </summary>
         public Action Changed;

         public virtual bool WantsInitialValue => true;

         /// <summary>
         /// The editor, and nothing else: no caption above it, no note under it,
         /// no margin. Those belong to the <see cref="Scaffold.FieldRow"/> that
         /// <see cref="CreateRow"/> puts around it for every row type at once.
         /// </summary>
         protected abstract FrameworkElement BuildEditor(object value);

         /// <summary>
         /// False on the two row types that are their own caption: a check box's
         /// Content is its label, and a row that printed it above as well would
         /// say the same words twice.
         /// </summary>
         protected virtual bool CaptionOnRow => true;

         /// <summary>
         /// One field of the form: the caption with its Alt key, the editor, the
         /// note under it. The editor keeps the accessible name
         /// <see cref="Describe"/> gave it - that one is resolved across the whole
         /// page and can be more exact than the caption - and takes its help text
         /// from the note.
         /// </summary>
         public Scaffold.FieldRow CreateRow(object value)
            => new()
            {
               Label = CaptionOnRow ? Label : null,
               Hint = Blurb,
               Content = HeldByEnvironment(IsWithheld ? WithheldEditor() : BuildEditor(value)),
               Applies = EnvironmentCaption ?? AppliesCaption
            };

         /// <summary>
         /// Whether this row's value came from a file of the server's environment -
         /// a Secret's - which the server withholds from every answer until an
         /// administrator writes another (Services/EnvironmentSettings.ParseFromFile).
         /// A password row never shows its value anyway, and a row that persists
         /// nothing has none.
         /// </summary>
         private bool IsWithheld
         {
            get
            {
               if (this is INotPersisted || this is SectionIniNumber || this is ComPassword || string.IsNullOrEmpty(Path))
                  return false;

               if (RestEnvironment == EnvironmentSettings.File)
                  return true;

               return this is IIniSetting ? IniFeatureStore.WithheldKey(Path) : IniFeatureStore.WithheldComPath(Path);
            }
         }

         /// <summary>
         /// The editor of a withheld row: an empty box that says why it is empty. A
         /// save writes nothing from it unless something was typed
         /// (<see cref="WithheldEntry"/>), so the value the file supplied is neither
         /// shown nor overwritten with a blank.
         /// </summary>
         private FrameworkElement WithheldEditor()
            => withheld_ = TextEditor("", EnvironmentSettings.WithheldPlaceholder());

         private TextBox withheld_;

         /// <summary>
         /// Null for a row that is not withheld; otherwise what was typed over the
         /// withheld value, empty when nothing was - and then nothing is written.
         /// </summary>
         protected string WithheldEntry => withheld_ == null ? null : (withheld_.Text ?? "").Trim();

         /// <summary>
         /// A setting the server's environment sets is shown and not edited: the
         /// server refuses the change, which the next start would undo
         /// (Services/EnvironmentSettings). Its mark takes the applies mark's place
         /// (CreateRow), and the editor, disabled, keeps the value it read - which a
         /// save writing it back unchanged leaves alone.
         /// </summary>
         private FrameworkElement HeldByEnvironment(FrameworkElement editor)
         {
            if (EnvironmentCaption != null)
               editor.IsEnabled = false;
            return editor;
         }

         /// <summary>
         /// "Set by the environment" when the server's environment sets this row's
         /// setting: a row kept in the settings store by its key, a COM row by its
         /// property's name. Nothing for a line of live state, a [Database] row, or a
         /// setting the environment does not set.
         /// </summary>
         protected virtual string EnvironmentCaption
         {
            get
            {
               if (this is INotPersisted || this is SectionIniNumber)
                  return null;

               if (RestEnvironment != null)
                  return EnvironmentSettings.Caption(RestEnvironment);

               string source = this is IIniSetting
                  ? IniFeatureStore.EnvironmentForKey(Path)
                  : IniFeatureStore.EnvironmentForComPath(Path);

               return EnvironmentSettings.Caption(source);
            }
         }

         /// <summary>
         /// When a save of this row reaches the running server, for a row kept in
         /// hMailServer.ini (Services/SettingApplies): the server's mark for a
         /// [Settings] key, and "After a restart" for a [Database] one, which is read
         /// once before the database is open. Nothing for a COM row.
         /// </summary>
         protected virtual string AppliesCaption
            => AppliesMark != null
               ? SettingApplies.Caption(AppliesMark)
               : this is IIniSetting
                  ? SettingApplies.Caption(this is SectionIniNumber ? SettingApplies.Restart : IniFeatureStore.AppliesFor(Path))
                  : null;

         public abstract object ReadEditor();

         /// <summary>
         /// A withheld [Settings] row's save: what was typed over it, as typed, and
         /// nothing when nothing was.
         /// </summary>
         protected bool SaveWithheld(IniFeatureStore store, string key)
         {
            if (store == null || !store.IsAvailable || string.IsNullOrEmpty(WithheldEntry))
               return false;

            store.Write(key, WithheldEntry);
            return true;
         }

         // Give the interactive editor control a stable AutomationId so
         // UI-automation can address it reliably.
         protected static void SetAid(FrameworkElement element, string id)
         {
            if (element != null && !string.IsNullOrEmpty(id))
               System.Windows.Automation.AutomationProperties.SetAutomationId(element, id);
         }

         /// <summary>
         /// The AutomationId plus the accessible name.
         ///
         /// An AutomationId is not spoken; it exists for test automation. Every
         /// editor on these pages is labelled by a separate TextBlock above it, and
         /// WPF does not connect the two, so before this each of the ~150 settings
         /// on these pages was announced as a bare "edit" or "combo box" with no
         /// name at all. Checkboxes were the only exception, because their label is
         /// their Content.
         /// </summary>
         protected void Describe(FrameworkElement element, string id)
         {
            SetAid(element, id);

            if (element != null && !string.IsNullOrEmpty(AccessibleName))
               System.Windows.Automation.AutomationProperties.SetName(element, AccessibleName);
         }

         /// <summary>
         /// Hooks the editor's change so the page's pill can say what is on screen
         /// is not what the server holds.
         ///
         /// One switch rather than an event per row type, and ordered innermost
         /// first on purpose: a WPF-UI NumberBox and a PasswordField are both
         /// TextBoxes by inheritance, so a plain TextBox case above them would
         /// swallow both and the two rows would never report a change.
         /// </summary>
         protected void Watch(FrameworkElement editor)
         {
            switch (editor)
            {
               case CheckBox box:
                  box.Checked += (s, e) => Changed?.Invoke();
                  box.Unchecked += (s, e) => Changed?.Invoke();
                  break;
               case Wpf.Ui.Controls.NumberBox number:
                  number.ValueChanged += (s, e) => Changed?.Invoke();
                  break;
               case PasswordField password:
                  password.PasswordChanged += (s, e) => Changed?.Invoke();
                  break;
               case System.Windows.Controls.TextBox text:
                  text.TextChanged += (s, e) => Changed?.Invoke();
                  break;
               case ComboBox combo:
                  combo.SelectionChanged += (s, e) => Changed?.Invoke();
                  break;
            }
         }

         /// <summary>
         /// The width of every numeric editor on these pages. A count of seconds
         /// or of connections is four or five digits wide, and a box stretched to
         /// the column would say it expects a sentence.
         /// </summary>
         protected const double NumberWidth = 180;

         /// <summary>
         /// The up/down box the four numeric row types share, described, watched
         /// and sized. <paramref name="fallback"/> is what is shown when the value
         /// is not a number at all, which for an INI row is its documented default
         /// and for a COM row is zero.
         /// </summary>
         protected Wpf.Ui.Controls.NumberBox NumberEditor(object value, int minimum, int fallback, int? maximum = null)
         {
            double current;
            try { current = Convert.ToDouble(value); } catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck)) { current = fallback; }

            var box = new Wpf.Ui.Controls.NumberBox
            {
               Value = current,
               Minimum = minimum,
               MaxDecimalPlaces = 0,
               SmallChange = 1,
               LargeChange = 10,
               FontSize = Typography.Body,
               Width = NumberWidth,
               HorizontalAlignment = HorizontalAlignment.Left
            };

            // The server's own ceiling, where it has one: a value past it is refused
            // on save, so the box does not offer it.
            if (maximum.HasValue)
               box.Maximum = maximum.Value;

            Describe(box, Path);
            Watch(box);
            return box;
         }

         /// <summary>
         /// The text box the free-text rows share. It takes the width of the
         /// field column rather than a typed-in one, so the editors line up down
         /// their right edge as well as their left and a long path is readable in
         /// the box that holds it.
         /// </summary>
         protected TextBox TextEditor(string text, string placeholder = null)
         {
            var box = new TextBox
            {
               Text = text ?? "",
               FontSize = Typography.Body,
               HorizontalAlignment = HorizontalAlignment.Stretch
            };

            if (!string.IsNullOrEmpty(placeholder))
               box.PlaceholderText = placeholder;

            Describe(box, Path);
            Watch(box);
            return box;
         }

         /// <summary>
         /// A text editor with a "…" picker beside it: the box takes what is left
         /// of the column and the button its own width.
         /// </summary>
         protected FrameworkElement WithBrowse(TextBox box, bool folder, string filter)
         {
            var row = new Grid();
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            Grid.SetColumn(box, 0);
            row.Children.Add(box);

            var browse = new Wpf.Ui.Controls.Button
            {
               Content = "…",
               MinWidth = 40,
               Margin = new Thickness(DesignTokens.Space.Sm, 0, 0, 0),
               VerticalAlignment = VerticalAlignment.Bottom,
               ToolTip = folder ? L("Browse for a folder") : L("Browse for a file")
            };
            SetAid(browse, Path + "Browse"); // no-loc

            // The button's content is a single ellipsis character, so its content
            // names it "…" and a listener is told nothing at all about which of the
            // several browse buttons on the page they are on.
            System.Windows.Automation.AutomationProperties.SetName(browse,
               folder
                  ? F("Browse for a folder for {0}", AccessibleName ?? Label ?? L("this setting"))
                  : F("Browse for a file for {0}", AccessibleName ?? Label ?? L("this setting")));

            browse.Click += (s, e) =>
            {
               string picked = folder
                  ? Services.PathPicker.PickFolder(box.Text)
                  : Services.PathPicker.PickFile(box.Text, filter);
               if (picked != null)
                  box.Text = picked;
            };
            Grid.SetColumn(browse, 1);
            row.Children.Add(browse);
            return row;
         }

         /// <summary>
         /// The check box the two yes/no row types share. Its Content is its
         /// caption, which is why those rows put no caption on the field row.
         /// </summary>
         protected CheckBox CheckEditor(bool value)
         {
            var box = new CheckBox { Content = Label, IsChecked = value, FontSize = Typography.Body };
            SetAid(box, Path);

            // A checkbox is named by its Content, so it needs an override only
            // when the resolved name differs from the label - which happens when
            // the same wording appears on more than one card and has been
            // qualified with the card title.
            if (!string.IsNullOrEmpty(AccessibleName) &&
                !string.Equals(AccessibleName, Label, StringComparison.Ordinal))
            {
               System.Windows.Automation.AutomationProperties.SetName(box, AccessibleName);
            }

            Watch(box);
            return box;
         }

         /// <summary>The list editor the two combo row types share.</summary>
         protected ComboBox ComboEditor()
            => new() { HorizontalAlignment = HorizontalAlignment.Stretch, FontSize = Typography.Body };

         protected static string Slug(string text)
         {
            if (string.IsNullOrEmpty(text))
               return "";
            var sb = new System.Text.StringBuilder(text.Length);
            foreach (char c in text.ToLowerInvariant())
               sb.Append(char.IsLetterOrDigit(c) ? c : '-');
            return sb.ToString();
         }

         public virtual void Write(object owner, string property)
         {
            // A withheld row writes what was typed over it, as the setting's own type,
            // and nothing when nothing was.
            if (WithheldEntry != null)
            {
               if (WithheldEntry.Length == 0)
                  return;

               object typed = WithheldEntry;
               if (this is ComBool)
                  typed = WithheldEntry == "1" || string.Equals(WithheldEntry, "true", StringComparison.OrdinalIgnoreCase); // no-loc
               else if (this is ComCombo)
                  typed = int.Parse(WithheldEntry, System.Globalization.CultureInfo.InvariantCulture);
               else if (this is ComText text && text.Numeric)
                  typed = (int)(long.Parse(WithheldEntry, System.Globalization.CultureInfo.InvariantCulture) * text.Divisor);

               Store(owner, property, typed);
               return;
            }

            object value = ReadEditor();
            if (value is long n)
               Store(owner, property, (int)n);
            else
               Store(owner, property, value);
         }

         /// <summary>Writes the property, with its argument when it is an indexed one.</summary>
         protected void Store(object owner, string property, object value)
         {
            if (Index == null)
               SetProperty(owner, property, value);
            else
               owner.GetType().InvokeMember(property, BindingFlags.SetProperty, null, owner, new[] { Index, value });
         }

         /// <summary>Reads the property, with its argument when it is an indexed one.</summary>
         public object Fetch(object owner, string property)
            => Index == null
               ? GetProperty(owner, property)
               : owner.GetType().InvokeMember(property, BindingFlags.GetProperty, null, owner, new object[] { Index });
      }

      private class ComBool : ComSetting
      {
         private CheckBox box_;

         /// <summary>The created checkbox (for cross-field dependency wiring).</summary>
         public CheckBox Box => box_;

         // Blurb was once silently dropped on this row type: every other editor
         // wrapped its control and printed the note, and this one returned the
         // bare checkbox, so a note written on a COM-backed checkbox appeared
         // nowhere and no build warning said so. It cannot happen again on any row
         // type, because printing the note is no longer something a row type does:
         // CreateRow puts every editor in a FieldRow whose Hint is the Blurb.
         protected override bool CaptionOnRow => false;

         protected override FrameworkElement BuildEditor(object value)
            => box_ = CheckEditor(value is bool b && b);

         public override object ReadEditor() => box_.IsChecked is true;
      }

      private class ComText : ComSetting
      {
         public bool Numeric;
         public int Divisor = 1;   // numeric display scaling (e.g. hours stored, days shown)
         public bool BrowseFile;   // show a "..." file picker next to the box
         public bool BrowseFolder; // show a "..." folder picker next to the box
         public string FileFilter = L("All files (*.*)|*.*");

         /// <summary>The lowest and highest value a numeric row offers: the server's own bounds, where it has them.</summary>
         public int Minimum;
         public int? Maximum;

         /// <summary>Shown in an empty free-text box: an example of what goes there, never a value.</summary>
         public string Placeholder;

         private TextBox box_;
         private Wpf.Ui.Controls.NumberBox number_;

         /// <summary>Current text in the editor (for live test buttons).</summary>
         public string CurrentText
            => number_ != null
               ? ((long)(number_.Value ?? 0)).ToString()
               : (box_?.Text?.Trim() ?? "");

         /// <summary>Overwrites the editor text (for preset pickers / auto-detect).</summary>
         public void SetText(string text)
         {
            if (number_ != null)
               number_.Value = double.TryParse(text, out double d) ? d : 0;
            else if (box_ != null)
               box_.Text = text ?? "";
         }
         protected override FrameworkElement BuildEditor(object value)
         {
            object shown = value;
            if (Numeric && Divisor > 1 && value != null)
            {
               try { shown = Convert.ToInt64(value) / Divisor; } catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck)) { shown = value; }
            }

            // Numeric settings get an up/down NumberBox; everything else a text box.
            if (Numeric)
               return number_ = NumberEditor(shown, Minimum, Minimum, Maximum);

            box_ = TextEditor(Convert.ToString(shown), Placeholder);
            return BrowseFile || BrowseFolder ? WithBrowse(box_, BrowseFolder, FileFilter) : box_;
         }

         public override object ReadEditor()
         {
            if (Numeric)
            {
               long number = number_ != null ? (long)(number_.Value ?? 0) : 0L;
               return number * Divisor;
            }
            return box_.Text.Trim();
         }
      }

      /// <summary>
      /// Marker for settings stored in hMailServer.ini rather than in the COM
      /// settings tree, so the save path can route them to the INI store.
      /// </summary>
      private interface IIniSetting
      {
         /// <summary>
         /// Writes the row's value when it is not what the editor was filled with,
         /// and says whether it wrote. Every write publishes on the server
         /// (docs/SettingsApply.md): a page of a hundred rows saved with one changed
         /// used to be a hundred reloads of the whole store, and stored every default
         /// it had merely shown.
         /// </summary>
         bool SaveToIni();
      }

      /// <summary>
      /// A yes/no setting stored in hMailServer.ini. Same purpose as IniNumber:
      /// it lets an INI-backed switch sit on the page where admins look for it.
      /// </summary>
      private class IniBool : ComSetting, IIniSetting
      {
         public bool Default;
         public IniFeatureStore IniStore;
         private CheckBox box_;

         /// <summary>The created checkbox (for cross-field dependency wiring).</summary>
         public CheckBox Box => box_;

         public override bool WantsInitialValue => false;

         /// <summary>The caption is the check box's own Content; see ComBool.</summary>
         protected override bool CaptionOnRow => false;

         protected override FrameworkElement BuildEditor(object value)
         {
            bool current = Default;
            if (IniStore != null && IniStore.IsAvailable)
               current = IniStore.ReadBool(Path, Default);

            loaded_ = current;
            return box_ = CheckEditor(current);
         }

         private bool? loaded_;

         public override object ReadEditor() => box_?.IsChecked is true;

         public bool SaveToIni()
         {
            if (WithheldEntry != null)
               return SaveWithheld(IniStore, Path);

            if (IniStore == null || !IniStore.IsAvailable || box_ == null)
               return false;

            bool value = box_.IsChecked is true;
            if (loaded_ == value)
               return false;

            IniStore.WriteBool(Path, value);
            loaded_ = value;
            return true;
         }
      }

      /// <summary>
      /// A numeric setting stored in hMailServer.ini rather than in the COM
      /// settings tree. It exists so an INI-backed knob can sit on the page
      /// where users look for it (log retention belongs with the other logging
      /// settings, not on an unrelated page). Path is the INI key name.
      /// </summary>
      private class IniNumber : ComSetting, IIniSetting
      {
         public int Default;

         /// <summary>
         /// Lowest value the box will accept. Zero for almost everything, because
         /// almost every numeric INI value is a count or a number of seconds. It is
         /// settable because a few are not: the TLS session cache size uses a
         /// negative value to mean "no server-side cache at all", and a box that
         /// silently refused to go below zero would hide that option entirely.
         /// </summary>
         public int MinimumValue;

         private Wpf.Ui.Controls.NumberBox number_;

         // The value comes from the INI file, not from a COM property.
         public override bool WantsInitialValue => false;

         protected override FrameworkElement BuildEditor(object value)
         {
            int current = Default;
            if (IniStore != null && IniStore.IsAvailable && !int.TryParse(ReadRaw(), out current))
               current = Default;

            loaded_ = ((long)current).ToString();
            return number_ = NumberEditor(current, MinimumValue, Default);
         }

         private string loaded_;

         public override object ReadEditor() => (long)(number_?.Value ?? Default);

         /// <summary>Persists to hMailServer.ini when changed. Save_Click calls this
         /// instead of the COM write path.</summary>
         public bool SaveToIni()
         {
            if (WithheldEntry != null)
               return SaveWithheld(IniStore, Path);

            if (IniStore == null || !IniStore.IsAvailable || number_ == null)
               return false;

            string value = ((long)(number_.Value ?? Default)).ToString();
            if (value == loaded_)
               return false;

            WriteRaw(value);
            loaded_ = value;
            return true;
         }

         /// <summary>Where the value is read from and written to - [Settings], unless a subclass says otherwise.</summary>
         protected virtual string ReadRaw() => IniStore.Read(Path, Default.ToString());

         protected virtual void WriteRaw(string value) => IniStore.Write(Path, value);

         /// <summary>Shared store, assigned by the view when the page is built.</summary>
         public IniFeatureStore IniStore;
      }

      /// <summary>
      /// A numeric setting in a named INI section other than [Settings]. The same
      /// row as <see cref="IniNumber"/> - the same box, the same default, the same
      /// minimum - pointed at a different section, which is the whole difference
      /// between the two and now the only thing written here.
      ///
      /// The database tuning values live in [Database]. Editing them through
      /// IniNumber would write keys of the right name into [Settings], where the
      /// server never looks - the value would appear saved, read back correctly on
      /// the next visit, and do nothing at all.
      /// </summary>
      private class SectionIniNumber : IniNumber
      {
         public string Section;

         protected override string ReadRaw() => IniStore.ReadFrom(Section, Path, Default.ToString());

         protected override void WriteRaw(string value) => IniStore.WriteTo(Section, Path, value);
      }

      /// <summary>
      /// A free-text setting stored in hMailServer.ini. Same purpose as IniBool
      /// and IniNumber: it puts an INI-backed value on the page that owns the
      /// feature it configures (the archive folder belongs next to the mirroring
      /// address, not on a catch-all page). Path is the INI key name.
      /// </summary>
      private class IniText : ComSetting, IIniSetting
      {
         public string Placeholder = "";
         public bool BrowseFolder;
         public IniFeatureStore IniStore;

         /// <summary>
         /// What the SERVER uses when the key is absent from the ini - not a
         /// suggestion, and not necessarily empty. Without it this editor showed a
         /// blank box for a key the server defaults to a real value, which is a
         /// false statement about the running configuration; and because saving a
         /// page writes every field on it, the blank was then written back and the
         /// default silently lost. Both OAuth2 host lists default to a Microsoft
         /// host, so that is exactly the shape it bit.
         /// </summary>
         public string Default = "";

         private TextBox box_;

         public override bool WantsInitialValue => false;

         protected override FrameworkElement BuildEditor(object value)
         {
            string current = Default;
            if (IniStore != null && IniStore.IsAvailable)
               current = IniStore.Read(Path, Default);

            loaded_ = (current ?? "").Trim();
            box_ = TextEditor(current, Placeholder);
            return BrowseFolder ? WithBrowse(box_, folder: true, null) : box_;
         }

         private string loaded_;

         public override object ReadEditor() => box_?.Text?.Trim() ?? "";

         public bool SaveToIni()
         {
            if (WithheldEntry != null)
               return SaveWithheld(IniStore, Path);

            if (IniStore == null || !IniStore.IsAvailable || box_ == null)
               return false;

            string value = box_.Text.Trim();
            if (value == loaded_)
               return false;

            IniStore.Write(Path, value);
            loaded_ = value;
            return true;
         }
      }

      /// <summary>
      /// One of a few numbered choices stored in hMailServer.ini: the words of
      /// each choice in a list, where a number box asked the reader to remember
      /// what 0 and 1 meant. A value the list does not know - written by hand, or
      /// by a newer server - is offered as itself and kept, never replaced by the
      /// first choice on a save.
      /// </summary>
      private class IniChoice : ComSetting, IIniSetting
      {
         public int Default;
         public (int Value, string Label)[] Options;
         public IniFeatureStore IniStore;
         private ComboBox combo_;
         private int? loaded_;

         public override bool WantsInitialValue => false;

         protected override FrameworkElement BuildEditor(object value)
         {
            int current = Default;
            if (IniStore != null && IniStore.IsAvailable && !int.TryParse(IniStore.Read(Path, Default.ToString()), out current))
               current = Default;

            loaded_ = current;
            combo_ = ComboEditor();
            foreach ((int v, string l) in ChoiceOptions.WithUnknown(Options, current))
            {
               var item = new ComboBoxItem { Content = l, Tag = v };
               combo_.Items.Add(item);
               if (v == current)
                  combo_.SelectedItem = item;
            }
            Describe(combo_, Path);
            Watch(combo_);
            return combo_;
         }

         public override object ReadEditor()
            => combo_?.SelectedItem is ComboBoxItem cbi ? (int)cbi.Tag : loaded_ ?? Default;

         public bool SaveToIni()
         {
            if (WithheldEntry != null)
               return SaveWithheld(IniStore, Path);

            if (IniStore == null || !IniStore.IsAvailable || combo_ == null)
               return false;

            int value = (int)ReadEditor();
            if (loaded_ == value)
               return false;

            IniStore.Write(Path, value.ToString(System.Globalization.CultureInfo.InvariantCulture));
            loaded_ = value;
            return true;
         }
      }

      private class ComCombo : ComSetting
      {
         public (int Value, string Label)[] Options;
         private ComboBox combo_;

         /// <summary>The created combo (for cross-field dependency wiring).</summary>
         public ComboBox Combo => combo_;

         /// <summary>The value currently selected, or 0 before the editor is built.</summary>
         public int SelectedValue
            => combo_?.SelectedItem is ComboBoxItem cbi ? (int)cbi.Tag : 0;

         /// <summary>
         /// How the value the server holds is named when it is none of Options: null
         /// names it by its number (ChoiceOptions.WithUnknown). A row reached over
         /// the REST API, which answers a word, names it by the word.
         /// </summary>
         protected virtual string UnknownText => null;

         protected override FrameworkElement BuildEditor(object value)
         {
            combo_ = ComboEditor();
            int sel;
            try { sel = Convert.ToInt32(value); } catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck)) { sel = Options.Length > 0 ? Options[0].Value : 0; }

            // A value that is none of the choices is offered as itself, so that the
            // page - which saves every row - writes back what it read rather than
            // the first choice (Services/ChoiceOptions).
            foreach ((int v, string l) in ChoiceOptions.WithUnknown(Options, sel, UnknownText))
            {
               var item = new ComboBoxItem { Content = l, Tag = v };
               combo_.Items.Add(item);
               if (v == sel)
                  combo_.SelectedItem = item;
            }
            Describe(combo_, Path);
            Watch(combo_);
            return combo_;
         }

         public override object ReadEditor()
            => combo_.SelectedItem is ComboBoxItem cbi ? (int)cbi.Tag : 0;
      }

      private class ComPassword : ComSetting
      {
         public string MethodName;   // e.g. SetSMTPRelayerPassword
         private hMailServer.ControlPanel.Views.PasswordField box_;
         public override bool WantsInitialValue => false;

         protected override FrameworkElement BuildEditor(object value)
         {
            box_ = new PasswordField
            {
               FontSize = Typography.Body,
               HorizontalAlignment = HorizontalAlignment.Stretch
            };
            Describe(box_, Path);
            Watch(box_);
            return box_;
         }

         public override object ReadEditor() => box_?.Password ?? "";

         /// <summary>Empties the box once what was typed has been stored.</summary>
         public void Clear()
         {
            if (box_ != null)
               box_.Password = "";
         }

         public override void Write(object owner, string property)
         {
            string pw = box_.Password;
            if (string.IsNullOrEmpty(pw))
               return;   // leave the current password unchanged
            owner.GetType().InvokeMember(MethodName, BindingFlags.InvokeMethod, null, owner, new object[] { pw });
         }
      }

      /// <summary>
      /// A read-only line of live server state, next to the settings that produce it.
      ///
      /// Not an editor and not a button: some things an administrator needs from a
      /// settings page are neither. The cache TTLs are only adjustable with
      /// judgement if the hit rate they produce is visible, and a page of four
      /// numbers with no feedback is a page you can only tune by guessing.
      /// </summary>
      private class ComStat : ComSetting, INotPersisted
      {
         /// <summary>Produces the text, or throws - which is reported in place of it.</summary>
         public Func<string> Read;

         public override bool WantsInitialValue => false;

         protected override FrameworkElement BuildEditor(object value)
         {
            string text;
            try
            {
               text = Read();
            }
            catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
            {
               text = F("This could not be read from the server: {0}", ServerSession.DescribeComError(ex));
            }

            var line = new TextBlock
            {
               Text = text,
               FontSize = Typography.Body,
               TextWrapping = TextWrapping.Wrap
            };
            Describe(line, Path);
            return line;
         }

         /// <summary>Nothing to read back: this row edits nothing.</summary>
         public override object ReadEditor() => null;

         /// <summary>Nothing to write. Marked so Save_Click does not count it.</summary>
         public override void Write(object owner, string property)
         {
         }
      }

      /// <summary>
      /// A non-persistent action button (e.g. "Test connection") with a result line.
      ///
      /// Marked INotPersisted, which it was not. Nothing was ever written wrongly -
      /// Write below is a no-op override, which is what made the omission harmless -
      /// but Save's skip test is "INotPersisted, or an empty Path", and the comment
      /// on that second test ("buttons and preset pickers persist nothing and name
      /// no property") is not true of these buttons: several carry a Path so they
      /// sit beside the switch they act on - "Clear greylisting triplets" names
      /// AntiSpam.GreyListingEnabled, "Test SpamAssassin connection" names
      /// AntiSpam.SpamAssassinEnabled, and the index and repair actions name theirs.
      ///
      /// So each of those fell through to the no-op Write and then incremented the
      /// counter, and "Saved 18 settings" counted buttons among the settings. That
      /// sentence has to be true - it is the only confirmation an administrator
      /// gets that a save did anything - so the row is excluded by the marker that
      /// means what it says, rather than by a Path test that describes these rows
      /// incorrectly.
      /// </summary>
      private class ComAction : ComSetting, INotPersisted
      {
         public string ButtonText;
         public Func<(bool ok, string text)> Action;
         private System.Windows.Controls.TextBlock result_;
         public override bool WantsInitialValue => false;

         protected override FrameworkElement BuildEditor(object value)
         {
            var panel = new StackPanel();
            var btn = new Wpf.Ui.Controls.Button
            {
               Content = ButtonText,
               Appearance = Wpf.Ui.Controls.ControlAppearance.Secondary,
               HorizontalAlignment = HorizontalAlignment.Left
            };
            SetAid(btn, "action-" + Slug(ButtonText));

            // Collapsed until there is a result: an empty line still takes its
            // margin and its line height, which pushed the note under the button
            // away from the button it belongs to.
            result_ = new System.Windows.Controls.TextBlock
            {
               FontSize = Typography.Caption,
               Margin = new Thickness(0, DesignTokens.Space.Sm, 0, 0),
               TextWrapping = TextWrapping.Wrap,
               Visibility = Visibility.Collapsed
            };

            // A polite live region, so the outcome of "Test ClamAV connection" is
            // announced rather than only coloured. Colour is the only other channel
            // this line has - green for success, red for failure - so without this a
            // screen-reader user pressed the button and was told nothing whatsoever,
            // and there is no way to discover the answer by moving around, because
            // the text did not exist before the click.
            //
            // Polite rather than assertive, and safe to make a live region here in a
            // way the dashboard's summary line is not (see AccessibleChartCard): this
            // text changes only when the user presses the button, so an announcement
            // is the answer to something they just asked.
            System.Windows.Automation.AutomationProperties.SetLiveSetting(
               result_, System.Windows.Automation.AutomationLiveSetting.Polite);
            System.Windows.Automation.AutomationProperties.SetName(result_,
               F("{0} result", ButtonText ?? L("Test")));

            btn.Click += (s, e) =>
            {
               try
               {
                  (bool ok, string text) r = Action();
                  Report_(r.text, r.ok);
               }
               catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
               {
                  Report_(F("Test failed: {0}", ex.Message), false);
               }
            };

            panel.Children.Add(btn);
            panel.Children.Add(result_);
            return panel;
         }

         /// <summary>
         /// The outcome, in its level's colour by resource key rather than in a
         /// brush held here: ThemeTokens republishes the status brushes on every
         /// theme change, and a line holding a resolved brush would keep the colour
         /// of the theme it was pressed under. The words say which it is as well,
         /// which is what carries the outcome in High Contrast and to an eye that
         /// cannot separate the two colours.
         /// </summary>
         private void Report_(string text, bool ok)
         {
            result_.Text = text;
            result_.Visibility = Visibility.Visible;
            result_.SetResourceReference(System.Windows.Controls.TextBlock.ForegroundProperty,
               ok ? "AppSuccessBrush" : "AppDangerBrush");
         }

         public override object ReadEditor() => null;
         public override void Write(object owner, string property) { }
      }

      /// <summary>
      /// A non-persistent preset picker that fills other <see cref="ComText"/>
      /// editors when a preset is chosen (e.g. fill the custom-scanner command line
      /// and infected return value for a known AV engine).
      /// </summary>
      private class ComPreset : ComSetting
      {
         public (string Name, string Exe, int ReturnValue)[] Presets;
         public ComText ExeTarget;
         public ComText ReturnTarget;
         public override bool WantsInitialValue => false;

         protected override FrameworkElement BuildEditor(object value)
         {
            ComboBox combo = ComboEditor();
            combo.Items.Add(new ComboBoxItem { Content = L("Choose a preset\u2026"), Tag = -1 });
            for (int i = 0; i < Presets.Length; i++)
               combo.Items.Add(new ComboBoxItem { Content = Presets[i].Name, Tag = i });
            combo.SelectedIndex = 0;

            combo.SelectionChanged += (s, e) =>
            {
               if (combo.SelectedItem is ComboBoxItem cbi && cbi.Tag is int idx && idx >= 0 && idx < Presets.Length)
               {
                  (string Name, string Exe, int ReturnValue) p = Presets[idx];
                  ExeTarget?.SetText(p.Exe);
                  ReturnTarget?.SetText(p.ReturnValue.ToString());
               }
            };

            // Not watched: choosing a preset saves nothing of its own. What it
            // fills in is watched, so the page says it has unsaved changes because
            // the command line changed, which is what actually happened.
            Describe(combo, "preset-" + Slug(Label));
            return combo;
         }

         public override object ReadEditor() => null;
         public override void Write(object owner, string property) { }
      }

      /// <summary>
      /// Marker for a row that writes nothing, so <see cref="Save_Click"/> does not
      /// count it among the settings it saved.
      /// </summary>
      private interface INotPersisted
      {
      }

      /// <summary>
      /// A setting the server stores and never reads - see
      /// <see cref="SettingClaims"/> for how one is established, and
      /// WorkerThreadPriority for the one this exists for.
      ///
      /// The value is shown and cannot be changed. Both halves are deliberate. An
      /// editable control is the failure being fixed: an administrator raises a
      /// number, saves, gets no error, and believes something happened. Removing
      /// the row instead would hide a value somebody may well have set years ago
      /// and be relying on, and would leave them with no way to find out that it
      /// never did anything. The row also stays in the generated search index, so
      /// Ctrl+K still finds the setting by name and lands the reader on the note
      /// that explains it, rather than on nothing at all.
      /// </summary>
      private class ComInert : ComSetting, INotPersisted
      {
         protected override FrameworkElement BuildEditor(object value)
         {
            // Read-only rather than disabled: a disabled control is skipped by the
            // keyboard and, in most screen readers, not reachable at all - so
            // disabling it would hide the value and the explanation from precisely
            // the reader who cannot see the greyed-out styling either. Read-only
            // keeps it in the tab order and announced, and still refuses the edit.
            // Not watched, either: nothing here can become unsaved.
            var box = new TextBox
            {
               Text = Convert.ToString(value) ?? "",
               FontSize = Typography.Body,
               Width = NumberWidth,
               HorizontalAlignment = HorizontalAlignment.Left,
               IsReadOnly = true
            };
            Describe(box, Path);
            return box;
         }

         public override object ReadEditor() => null;

         public override void Write(object owner, string property)
         {
         }
      }

      // ---- settings reached over the REST API ------------------------------------

      /// <summary>
      /// A setting the server keeps in hm_settings and answers over its REST API's
      /// settings groups, but over no COM property (Services/RestSettings). Path is
      /// the setting's own hm_settings name - what the settings search finds it by,
      /// what ties it to a value the server's environment sets, and what
      /// build/check-db-settings-coverage.py looks for - and Key is its name in the
      /// group, which is what is read and sent.
      ///
      /// The page reads each group once when it is built and writes each group once
      /// when it is saved, with only the keys that changed, as the server asks of a
      /// client that reads a group and writes it back. A group that cannot be reached
      /// - the REST API off, a second factor not given, a listener this machine cannot
      /// reach - is said once on each card that has its rows, in place of them, and
      /// nothing of it is written: a row that could not read its value is not offered,
      /// because it would show a default as though the server held it.
      /// </summary>
      private interface IRestSetting
      {
         /// <summary>The group's route, such as /api/v1/settings.</summary>
         string Group { get; }

         /// <summary>The setting's key in the group, such as alerts_enabled.</summary>
         string Key { get; }

         /// <summary>What the group answered, as the editor is filled with it.</summary>
         object ToEditor(object answered);

         /// <summary>The group's value, remembered as what the editor started from.</summary>
         void Loaded(object answered);

         /// <summary>The value to send when the editor no longer holds what it started from.</summary>
         bool TryGetChange(out object value);

         /// <summary>The save went through: what is on screen is what the server holds now.</summary>
         void Accepted();
      }

      /// <summary>A yes/no setting reached over the REST API.</summary>
      private sealed class RestBool : ComBool, IRestSetting
      {
         public string Group = RestSettings.ServerGroup;
         public string Key;
         private bool? loaded_;

         string IRestSetting.Group => Group;
         string IRestSetting.Key => Key;

         public object ToEditor(object answered) => answered;

         public void Loaded(object answered) => loaded_ = answered as bool?;

         // A withheld row has a text box, not this check box, and sends what was typed
         // (RestSettings.FlagChange).
         public bool TryGetChange(out object value)
            => RestSettings.FlagChange(WithheldEntry, WithheldEntry == null && ReadEditor() is true, loaded_, out value);

         public void Accepted()
         {
            if (WithheldEntry == null)
               loaded_ = ReadEditor() is true;
         }
      }

      /// <summary>
      /// A whole number reached over the REST API, offered between the bounds the
      /// server's own check puts on it (Minimum and Maximum).
      /// </summary>
      private sealed class RestNumber : ComText, IRestSetting
      {
         public string Group = RestSettings.ServerGroup;
         public string Key;
         private long? loaded_;

         public RestNumber() => Numeric = true;

         string IRestSetting.Group => Group;
         string IRestSetting.Key => Key;

         public object ToEditor(object answered) => answered;

         public void Loaded(object answered) => loaded_ = answered is long number ? number : null;

         // What was typed over a withheld value is sent even when it is no number, so
         // that the server refuses it and the save says so (RestSettings.NumberChange).
         public bool TryGetChange(out object value)
         {
            long now = WithheldEntry == null && ReadEditor() is long number ? number : 0;
            return RestSettings.NumberChange(WithheldEntry, now, loaded_, out value);
         }

         public void Accepted()
         {
            if (WithheldEntry == null)
               loaded_ = ReadEditor() is long number ? number : null;
         }
      }

      /// <summary>Free text reached over the REST API.</summary>
      private sealed class RestText : ComText, IRestSetting
      {
         public string Group = RestSettings.ServerGroup;
         public string Key;
         private string loaded_;

         string IRestSetting.Group => Group;
         string IRestSetting.Key => Key;

         public object ToEditor(object answered) => answered as string ?? "";

         public void Loaded(object answered) => loaded_ = ((answered as string) ?? "").Trim();

         public bool TryGetChange(out object value)
         {
            if (WithheldEntry != null)
            {
               value = WithheldEntry;
               return WithheldEntry.Length > 0;
            }

            string now = (ReadEditor() as string ?? "").Trim();
            value = now;
            return !string.Equals(loaded_, now, StringComparison.Ordinal);
         }

         public void Accepted()
         {
            if (WithheldEntry == null)
               loaded_ = (ReadEditor() as string ?? "").Trim();
         }
      }

      /// <summary>
      /// A secret reached over the REST API: a password, a client secret, a key. The
      /// server takes one and never answers it back, so the box starts empty and a
      /// save sends it only when something was typed - an empty box keeps what the
      /// server holds, as every password row on these pages does.
      /// </summary>
      private sealed class RestSecret : ComPassword, IRestSetting
      {
         public string Group = RestSettings.ServerGroup;
         public string Key;

         string IRestSetting.Group => Group;
         string IRestSetting.Key => Key;

         public object ToEditor(object answered) => null;

         public void Loaded(object answered)
         {
         }

         public bool TryGetChange(out object value)
         {
            string typed = ReadEditor() as string ?? "";
            value = typed;
            return typed.Length > 0;
         }

         public void Accepted() => Clear();
      }

      /// <summary>
      /// Text the server takes over the REST API and never answers back, though it is
      /// not a secret - a certificate in PEM, which is several lines and is pasted, so
      /// the box takes line breaks. It starts empty; a save sends it only when
      /// something was pasted, and an empty box keeps what the server holds.
      /// </summary>
      private sealed class RestPaste : ComSetting, IRestSetting
      {
         public string Group = RestSettings.ServerGroup;
         public string Key;
         private TextBox box_;

         string IRestSetting.Group => Group;
         string IRestSetting.Key => Key;

         public override bool WantsInitialValue => false;

         public object ToEditor(object answered) => null;

         public void Loaded(object answered)
         {
         }

         protected override FrameworkElement BuildEditor(object value)
         {
            box_ = TextEditor("");
            box_.AcceptsReturn = true;
            box_.TextWrapping = TextWrapping.Wrap;
            box_.MinHeight = 96;
            box_.MaxHeight = 220;
            box_.VerticalScrollBarVisibility = ScrollBarVisibility.Auto;
            box_.FontFamily = new System.Windows.Media.FontFamily(Typography.MonoFontFamily);
            return box_;
         }

         public override object ReadEditor() => box_?.Text?.Trim() ?? "";

         public bool TryGetChange(out object value)
         {
            string pasted = ReadEditor() as string ?? "";
            value = pasted;
            return pasted.Length > 0;
         }

         public void Accepted()
         {
            if (box_ != null)
               box_.Text = "";
         }

         public override void Write(object owner, string property)
         {
         }
      }

      /// <summary>
      /// One of a few words reached over the REST API, which answers and takes the
      /// word (off, watch, enforce) rather than the number behind it. Words is in the
      /// order of Options: the word of the option at the same place.
      /// </summary>
      private sealed class RestChoice : ComCombo, IRestSetting
      {
         public string Group = RestSettings.ServerGroup;
         public string Key;
         public string[] Words;
         private string loaded_;

         // The word the server answered when it is none of Words: offered as itself
         // (ComCombo.UnknownText), under a number no option has, and never sent.
         private string unknown_;
         private const int UnknownValue = int.MinValue;

         string IRestSetting.Group => Group;
         string IRestSetting.Key => Key;

         protected override string UnknownText => unknown_;

         public object ToEditor(object answered)
         {
            int at = Array.IndexOf(Words, answered as string);
            if (at >= 0)
               return Options[at].Value;

            // Shown as the first choice, a word this Control Panel does not know
            // was sent back as that choice by the next save of the page.
            unknown_ = Convert.ToString(answered, System.Globalization.CultureInfo.InvariantCulture) ?? "";
            return UnknownValue;
         }

         public void Loaded(object answered) => loaded_ = answered as string;

         // A withheld row is a text box, and its list was never built: what was typed
         // is sent, and the list is not read (RestSettings.ChoiceChange).
         public bool TryGetChange(out object value)
            => RestSettings.ChoiceChange(WithheldEntry, Combo == null ? null : (int?)SelectedValue,
               Array.ConvertAll(Options, option => option.Value), Words, loaded_, out value);

         public void Accepted()
         {
            if (WithheldEntry == null && TryGetChange(out object word))
               loaded_ = word as string;
         }
      }

      /// <summary>
      /// A line the server answers over the REST API and nobody sets - what a
      /// certificate that was pasted holds, for one. Read-only, and written by nothing.
      /// </summary>
      private sealed class RestStat : ComSetting, IRestSetting, INotPersisted
      {
         public string Group = RestSettings.ServerGroup;
         public string Key;

         /// <summary>What the line says when the server answers nothing for it.</summary>
         public string WhenEmpty;

         string IRestSetting.Group => Group;
         string IRestSetting.Key => Key;

         public object ToEditor(object answered) => answered;

         public void Loaded(object answered)
         {
         }

         public bool TryGetChange(out object value)
         {
            value = null;
            return false;
         }

         public void Accepted()
         {
         }

         protected override FrameworkElement BuildEditor(object value)
         {
            string text = Convert.ToString(value, System.Globalization.CultureInfo.InvariantCulture);
            var line = new TextBlock
            {
               Text = string.IsNullOrWhiteSpace(text) ? WhenEmpty ?? "" : text,
               FontSize = Typography.Body,
               TextWrapping = TextWrapping.Wrap
            };
            Describe(line, Path);
            return line;
         }

         public override object ReadEditor() => null;

         public override void Write(object owner, string property)
         {
         }
      }

      private class CardDef
      {
         public string Title;
         public string Blurb;
         public readonly List<ComSetting> Settings = new();
      }

      private class TabDef
      {
         public string Header;
         public readonly List<CardDef> Cards = new();
      }

      // ---- enum option tables ------------------------------------------------

      private static readonly (int, string)[] ConnSecurity =
      {
         (0, L("None")), (1, L("SSL/TLS")), (2, L("STARTTLS (optional)")), (3, L("STARTTLS (required)"))
      };

      private static readonly (int, string)[] AntivirusAction =
      {
         (0, L("Delete entire e-mail")), (1, L("Delete infected attachments only"))
      };

      // AVFailAction ([Settings] in hMailServer.INI): what a message no scanner could
      // scan becomes - AntiVirusConfiguration's 0 (deliver) and 1 (hold, retry, return).
      private static readonly (int, string)[] AvFailAction =
      {
         (0, L("Deliver it (default)")), (1, L("Hold it and scan it again, then return it to the sender"))
      };

      // Settings.SandboxTimeoutAction: what a hold the sandbox did not answer for ends in.
      private static readonly (int, string)[] SandboxTimeoutAction =
      {
         (0, L("Deliver it with a header saying so")), (1, L("Return it to the sender (5.7.1)"))
      };

      private readonly Section section_;
      private List<TabDef> tabs_;
      private string diag_;
      private int failedReads_;
      private Action afterBuildUi_;

      /// <summary>
      /// What the pill beside the page title says: whether what is on screen has
      /// reached the server. <see cref="ServerSettingsPageStatus"/> - which is WPF-free
      /// and tested - decides the word and the severity for each state.
      /// </summary>
      private ServerSettingsPageState state_ = ServerSettingsPageState.Clean;

      /// <summary>
      /// True while the editors are being filled from the server, so that filling
      /// them is not mistaken for someone typing in them.
      /// </summary>
      private bool building_;

      /// <summary>
      /// How wide the field column is allowed to grow. A settings form stretched
      /// to a wide window puts a caption and its editor a screen apart, which
      /// reads as two separate things; a column is what a form is.
      /// </summary>


      // Most settings on these pages live in the COM settings tree, but a few
      // (log retention) live in hMailServer.ini; IniNumber rows use this store.
      private readonly IniFeatureStore iniStore_ = new IniFeatureStore();

      /// <summary>One REST settings group as this page last read it: its values, or why they could not be read.</summary>
      private sealed class RestGroup
      {
         public IReadOnlyDictionary<string, System.Text.Json.JsonElement> Values;
         public string Refusal;
      }

      // The REST settings groups this page's rows are in, read when the page is built.
      private readonly Dictionary<string, RestGroup> restGroups_ = new(StringComparer.Ordinal);

      // Set by Reload: a one-time code the administrator declined to give is asked
      // for again, once, rather than on every page opened.
      private bool restAskAgain_;

      public ServerSettingsView(Section section)
      {
         InitializeComponent();
         section_ = section;
         BuildDefinition();
      }

      public void OnEnter()
      {
         // Every field's mark in one call (IniFeatureStore.LoadMarks), before the
         // rows that show them are built.
         IniFeatureStore.LoadMarks();
         BuildUi();
      }

      public void OnLeave()
      {
      }

      // ---- definitions -------------------------------------------------------

      /// <summary>
      /// The tab with this header, creating it the first time it is asked for.
      ///
      /// GET-or-create, which it was not. Every call used to append a new tab, so
      /// two cards written as Tab(L("General")).Cards.Add(...) - plainly meaning "put
      /// this on the General tab" - produced TWO tabs both called General, each
      /// holding one card. The anti-spam page shipped with General, General, Sender
      /// auth, Host checks, Greylisting, SpamAssassin, SpamAssassin, and the only way
      /// to find the quarantine settings was to notice that the second identically
      /// named tab was not the same as the first.
      ///
      /// tabs_ is rebuilt per section, so a header reused by a different section is
      /// unaffected - the match is only ever within the page being built. Ordinal
      /// comparison: these headers are literals in this file, and a culture-sensitive
      /// match would be a slower way to get the same answer.
      /// </summary>
      private TabDef Tab(string header)
      {
         TabDef existing = tabs_.FirstOrDefault(
            tab => string.Equals(tab.Header, header, StringComparison.Ordinal));

         if (existing != null)
            return existing;

         var created = new TabDef { Header = header };
         tabs_.Add(created);
         return created;
      }

      private static CardDef Card(string title, string blurb = null)
         => new() { Title = title, Blurb = blurb };

      /// <summary>
      /// Adapts an option table from <see cref="SettingClaims"/> to the array
      /// <see cref="ComCombo"/> takes. The tables live there rather than here
      /// because the wording of an option is a claim about what the server can
      /// produce, and a claim buried in a view literal is one no test can see.
      /// </summary>
      private static (int Value, string Label)[] ToOptions(IReadOnlyList<(int Value, string Label)> options)
      {
         var result = new (int Value, string Label)[options.Count];
         for (int i = 0; i < options.Count; i++)
            result[i] = (options[i].Value, L(options[i].Label));
         return result;
      }

      private void BuildDefinition()
      {
         tabs_ = new List<TabDef>();

         switch (section_)
         {
            case Section.Protocols: BuildProtocols(); break;
            case Section.Delivery: BuildDelivery(); break;
            case Section.AntiSpam: BuildAntiSpam(); break;
            case Section.AntiVirus: BuildAntiVirus(); break;
            case Section.Tls: BuildTls(); break;
            case Section.AutoBan: BuildAutoBan(); break;
            case Section.Logging: BuildLogging(); break;
            case Section.Performance: BuildPerformance(); break;
            case Section.Advanced: BuildAdvanced(); break;
            case Section.AdminAccess: BuildAdminAccess(); break;
            case Section.Webmail: BuildWebmail(); break;
         }
      }

      private void BuildProtocols()
      {
         Header.Title = L("Protocols");
         Header.Subtitle = L("Which services this server runs, connection limits and greetings.");

         var services = Card(L("Services"),
            L("Enable or disable the protocol servers. Changes apply after pressing Save. ManageSieve (for managing Sieve scripts) is enabled on the API & monitoring page; OAuth2 token authentication is on the Authentication page."));
         services.Settings.Add(new ComBool { Path = "ServiceSMTP", Label = L("SMTP server") });
         services.Settings.Add(new ComBool { Path = "ServiceIMAP", Label = L("IMAP server") });
         services.Settings.Add(new ComBool { Path = "ServicePOP3", Label = L("POP3 server") });
         Tab(L("Services")).Cards.Add(services);

         var smtp = Card(L("SMTP"));
         smtp.Settings.Add(new ComText { Path = "HostName", Label = L("Host name (HELO/EHLO greeting)") });
         smtp.Settings.Add(new ComText { Path = "MaxSMTPConnections", Label = L("Max simultaneous connections (0 = unlimited)"), Numeric = true });
         smtp.Settings.Add(new ComText { Path = "MaxMessageSize", Label = L("Max message size (KB, 0 = unlimited)"), Numeric = true });
         smtp.Settings.Add(new ComText { Path = "MaxSMTPRecipientsInBatch", Label = L("Max recipients per message"), Numeric = true });
         smtp.Settings.Add(new ComText { Path = "WelcomeSMTP", Label = L("Welcome banner (empty = default)") });
         smtp.Settings.Add(new ComBool { Path = "AllowSMTPAuthPlain", Label = L("Allow plain-text authentication (AUTH PLAIN/LOGIN)") });
         smtp.Settings.Add(new ComBool { Path = "DenyMailFromNull", Label = L("Reject empty sender addresses (MAIL FROM:<>)") });
         smtp.Settings.Add(new IniBool
         {
            Path = "RejectFullMailboxAtRcpt",
            Label = L("Refuse a full mailbox during the conversation (452 4.2.2)"),
            Blurb = L("On by default, and the one switch here whose default is not the cautious one. With it off, a message for a mailbox that is already over quota is accepted and then answered with a non-delivery report addressed to the envelope sender - which the traffic that fills mailboxes fastest has forged. That report then goes from this server to somebody who sent nothing, which is backscatter, and it is this server's reputation that pays for it. Refusing at RCPT TO with a temporary 452 instead leaves the message in the sending server's queue, so a legitimate sender's mail waits for the mailbox to be emptied and is eventually reported by the machine that knows who the sender really is."),
            IniStore = iniStore_
         });
         smtp.Settings.Add(new IniBool
         {
            Path = "SmtpAuthenticatedSenderCheck",
            Label = L("An authenticated session may only send as an address its account owns or has been granted"),
            Default = false,
            Blurb = L("With this on, MAIL FROM in an authenticated session must be the account's own address, an alias that resolves to it, or another mailbox whose owner has granted the account the post (p) right on their INBOX - which is how Send-As for a shared mailbox is granted, from any IMAP client's ACL editor (SETACL) or over the COM API. Anything else is refused with 550 5.7.1 before a byte of the message is accepted, so one compromised or careless account can no longer send as every other. Off by default because nothing constrained this before and an installation may depend on that; the null sender is always allowed. Unauthenticated sessions are unaffected."),
            IniStore = iniStore_
         });
         smtp.Settings.Add(new IniNumber
         {
            Path = "QuotaWarningPercent",
            Label = L("Warn the mailbox owner at this percentage of their quota (0 = never)"),
            Default = 90,
            Blurb = L("The delivery that takes a mailbox past this percentage produces one notice to the person whose mailbox it is, from their own domain's postmaster. ONE, not one per message: the server treats the threshold as something the mailbox CROSSES rather than a state it is in, so a mailbox sitting at 95% is not warned about again on every delivery - which would fill it faster than the mail does. Emptying the mailbox and filling it again warns again, which is the behaviour anyone would expect and is free here rather than something that had to be remembered and expired. On by default, because the alternative is what this server did before: say nothing at all until mail starts being refused, by which point the person it happens to is the last to know."),
            IniStore = iniStore_
         });
         smtp.Settings.Add(new ComText
         {
            Path = "QuotaGracePercent",
            Label = L("Still accept incoming mail past a mailbox's limit by (% of the limit; 0 = not at all)"),
            Numeric = true,
            Blurb = L("A grace for incoming mail: a mailbox over its limit by less than this still receives, so a person away for a week does not lose mail because a large attachment arrived on Monday. The owner's own saves and copies stop at the limit itself. Each domain may set its own. Applies at once.")
         });
         smtp.Settings.Add(new ComText
         {
            Path = "QuotaSendStopPercent",
            Label = L("Stop an account sending at (% of its limit; 0 = never)"),
            Numeric = true,
            Blurb = L("The account stops sending before it stops receiving: the owner sees the refusal in their own client, with the reason, while mail to them still arrives up to the limit and the grace past it. Each domain may set its own. Applies at once.")
         });
         smtp.Settings.Add(new IniNumber
         {
            Path = "ArchiveRetentionDays",
            Label = L("Delete archived messages after (days; 0 = keep for ever)"),
            Default = 0,
            Blurb = L("ArchiveDir is a raw copy of every message that passes through the server, and until now nothing ever removed anything from it - so on a busy server it is a directory that only grows, and the administrator who finds out is the one whose disk filled up. This prunes it. 0 keeps everything, and that is the default on purpose: an archive is usually kept for a legal or contractual reason, and a server that started deleting from one on upgrade would be destroying exactly what it was told to keep. The sweep only ever removes .eml files, so anything else you keep in the archive is left alone, and it runs at startup and every twelve hours after."),
            IniStore = iniStore_
         });

         smtp.Settings.Add(new IniBool
         {
            Path = "MetricsPerDomainEnabled",
            Label = L("Expose per-domain message counters on /metrics"),
            Blurb = L("Every counter the metrics endpoint exposes is server-wide, which is what stops metrics becoming reporting: you can see how much mail the server handled, never how much any one domain did. This adds a count of messages sent and received per hosted domain. Off by default, and that is a cardinality decision rather than a cautious one - every label value is a separate time series in whatever scrapes this, so an operator hosting several thousand domains should choose to pay for that rather than find out. Only domains this server hosts are ever labelled, so no amount of mail from strangers can create series."),
            IniStore = iniStore_
         });
         smtp.Settings.Add(new ComBool { Path = "AllowIncorrectLineEndings", Label = L("Allow incorrect line endings") });
         smtp.Settings.Add(new ComBool { Path = "DisconnectInvalidClients", Label = L("Disconnect clients sending too many invalid commands") });
         smtp.Settings.Add(new ComText { Path = "MaxNumberOfInvalidCommands", Label = L("Invalid command limit"), Numeric = true });
         Tab(L("SMTP")).Cards.Add(smtp);

         var imap = Card(L("IMAP"));
         imap.Settings.Add(new ComText { Path = "MaxIMAPConnections", Label = L("Max simultaneous connections (0 = unlimited)"), Numeric = true });
         imap.Settings.Add(new ComText { Path = "WelcomeIMAP", Label = L("Welcome banner (empty = default)") });
         imap.Settings.Add(new ComBool { Path = "IMAPIdleEnabled", Label = L("IDLE (push mail)") });
         imap.Settings.Add(new ComBool { Path = "IMAPQuotaEnabled", Label = L("QUOTA") });
         imap.Settings.Add(new ComBool { Path = "IMAPSortEnabled", Label = L("SORT") });
         imap.Settings.Add(new ComBool { Path = "IMAPACLEnabled", Label = L("ACL (shared folder permissions)") });
         imap.Settings.Add(new IniBool
         {
            Path = "IMAPCompressionEnabled",
            Label = L("COMPRESS=DEFLATE (RFC 4978)"),
            Default = true,
            Blurb = L("Lets a client ask for the rest of the session to be compressed, which is worth most on a slow or metered link where a large mailbox listing is mostly repeated text. On by default; a client that does not ask is unaffected."),
            IniStore = iniStore_
         });
         imap.Settings.Add(new ComBool { Path = "CreateDefaultSpecialUseFoldersEnabled", Label = L("Create Drafts, Sent, Trash and Junk for new accounts") });
         imap.Settings.Add(new ComBool { Path = "IMAPSASLPlainEnabled", Label = L("Allow SASL PLAIN authentication") });
         imap.Settings.Add(new ComBool { Path = "IMAPSASLInitialResponseEnabled", Label = L("Allow SASL initial client response") });
         imap.Settings.Add(new ComText { Path = "IMAPPublicFolderName", Label = L("Public folder name") });
         imap.Settings.Add(new ComText { Path = "IMAPMasterUser", Label = L("Master user (empty = disabled)") });
         imap.Settings.Add(new ComText { Path = "IMAPHierarchyDelimiter", Label = L("Folder hierarchy delimiter") });
         Tab(L("IMAP")).Cards.Add(imap);

         // These two are INI values rather than COM ones, and they used to be on
         // the catch-all INI page for that reason alone - which put the answer to
         // "IMAP search stopped working" on a page nobody opens looking for IMAP.
         // Storage mechanism is not a subject heading; the search limits belong
         // beside the SORT switch they interact with.
         var imapSearch = Card(L("IMAP search limits"),
            L("Ceilings on a single IMAP SEARCH or SORT, measured from the start of the search, so that one client searching a very large mailbox cannot occupy a connection indefinitely. If users with big mailboxes report searches failing or returning nothing, these are the two to raise. Either half can be turned off on its own by setting it to 0."));
         imapSearch.Settings.Add(new IniNumber
         {
            Path = "IMAPSearchTimeout",
            Label = L("Maximum time for one IMAP search (seconds; 0 = no limit)"),
            Default = 60,
            Blurb = L("The search stops when this is reached and the client is told the search failed, rather than being given a partial result it would mistake for a complete one."),
            IniStore = iniStore_
         });
         imapSearch.Settings.Add(new IniNumber
         {
            Path = "IMAPSearchMaxMegabytes",
            Label = L("Maximum message content read for one IMAP search (MB; 0 = no limit)"),
            Default = 2048,
            Blurb = L("A separate ceiling from the time limit, because a search that reads enormous amounts of message content is expensive even when it finishes quickly."),
            IniStore = iniStore_
         });
         Tab(L("IMAP")).Cards.Add(imapSearch);
         // Its own card rather than a row in "IMAP": the setting is about how much
         // history the server keeps, and the cost of getting it wrong is a full
         // resync for every client, which reads as "IMAP is slow" rather than as
         // anything to do with this value.
         var history = Card(L("Change history for synchronising clients"),
            L("QRESYNC and CONDSTORE clients ask what vanished while they were away instead of re-listing the folder. The server answers from a table of expunge records, pruned to the newest N twelve-hourly and once at service start. A client whose last sync is older than the retained history gets a full resync - never a wrong answer."));
         history.Settings.Add(new IniNumber
         {
            Path = "IMAPExpungeRetentionRecords",
            Label = L("Expunge records to keep (0 = keep everything, which also skips the prune at start-up)"),
            Default = 5000,
            MinimumValue = 0,
            IniStore = iniStore_
         });
         Tab(L("IMAP")).Cards.Add(history);

         var pop3 = Card(L("POP3"));
         pop3.Settings.Add(new ComText { Path = "MaxPOP3Connections", Label = L("Max simultaneous connections (0 = unlimited)"), Numeric = true });
         pop3.Settings.Add(new ComText { Path = "WelcomePOP3", Label = L("Welcome banner (empty = default)") });
         Tab(L("POP3")).Cards.Add(pop3);

         // RFC 2449 LOGIN-DELAY. On the POP3 page rather than the ini page because
         // that is where the question is asked, and next to the connection ceiling it
         // is the gentler alternative to.
         var pop3Polling = Card(L("How often a client may check for mail"),
            L("POP3 has no way for the server to tell a client that mail has arrived, so clients that want to look responsive poll - and some poll every few seconds. Each poll is a new connection, a TLS handshake and a password verification, which with Argon2id is deliberately expensive, so a handful of eager clients can cost more than the mail does. Setting a delay tells clients the interval in the server's capability list and refuses logins that arrive early with a code the client understands as \"slow down\" rather than as a wrong password."));
         pop3Polling.Settings.Add(new IniNumber
         {
            Path = "Pop3LoginDelaySeconds",
            Label = L("Minimum seconds between logins to one account (0 = no limit)"),
            Default = 0,
            Blurb = L("Counted per account from the last login that was allowed, so a refused attempt does not push the next one further away. It is advertised only while it is set, because announcing a limit that is not enforced is worse than announcing nothing."),
            IniStore = iniStore_
         });
         Tab(L("POP3")).Cards.Add(pop3Polling);

         // The idle timeouts follow the same reasoning as the IMAP search limits
         // above: they are per-protocol settings that sat on the INI page because
         // of where they are stored. They get a tab rather than being split three
         // ways across the protocol tabs because the server/client distinction is
         // the thing that confuses people, and it only reads clearly when all
         // eight are in one place.
         var idle = Card(L("Idle timeouts"),
            L("How long a connection may sit idle before it is closed, per protocol, in seconds. \"Server\" is hMailServer accepting a connection from a mail client or another mail server; \"client\" is hMailServer connecting out to deliver mail or to fetch it from an external POP3 account. The effective timeout moves between the minimum and the maximum with server load, so a busy server drops idle connections sooner."));
         idle.Settings.Add(new IniNumber { Path = "SMTPDMinTimeout", Label = L("SMTP server minimum timeout (s)"), Default = 10, IniStore = iniStore_ });
         idle.Settings.Add(new IniNumber { Path = "SMTPDMaxTimeout", Label = L("SMTP server maximum timeout (s)"), Default = 1800, IniStore = iniStore_ });
         idle.Settings.Add(new IniNumber { Path = "SMTPCMinTimeout", Label = L("SMTP client minimum timeout (s)"), Default = 30, IniStore = iniStore_ });
         idle.Settings.Add(new IniNumber { Path = "SMTPCMaxTimeout", Label = L("SMTP client maximum timeout (s)"), Default = 600, IniStore = iniStore_ });
         idle.Settings.Add(new IniNumber { Path = "POP3DMinTimeout", Label = L("POP3 server minimum timeout (s)"), Default = 10, IniStore = iniStore_ });
         idle.Settings.Add(new IniNumber { Path = "POP3DMaxTimeout", Label = L("POP3 server maximum timeout (s)"), Default = 600, IniStore = iniStore_ });
         idle.Settings.Add(new IniNumber { Path = "POP3CMinTimeout", Label = L("POP3 client minimum timeout (s)"), Default = 30, IniStore = iniStore_ });
         idle.Settings.Add(new IniNumber { Path = "POP3CMaxTimeout", Label = L("POP3 client maximum timeout (s)"), Default = 900, IniStore = iniStore_ });
         Tab(L("Timeouts")).Cards.Add(idle);

         // On the IMAP tab because that is what it repairs, and because somebody
         // arrives at this needing it - after a restored database, or after
         // clients start showing the wrong message - rather than browsing for it.
         var repair = Card(L("Repair"),
            L("Maintenance for the IMAP folder state. Nothing here changes a message; the folder UID counter is bookkeeping that IMAP clients rely on to tell one message from another."));
         repair.Settings.Add(new ComAction
         {
            Path = "ServiceIMAP",
            ButtonText = L("Recalculate folder UID counters"),
            Blurb = L("Each IMAP folder keeps a counter for the next message UID it will issue. If that counter falls behind the messages already in the folder - which a restored or hand-edited database can do - the folder issues a UID it has already used, and a client that caches by UID shows one message where another should be. This moves every counter up to the highest UID its folder actually holds. It never moves one down, so it cannot cause the fault it repairs."),
            Action = () => RecalculateFolderUids_()
         });
         Tab(L("IMAP")).Cards.Add(repair);
      }

      private void BuildDelivery()
      {
         Header.Title = L("Delivery of e-mail");
         Header.Subtitle = L("Outbound delivery behavior, retries and smart-host relaying.");

         var del = Card(L("Delivery of e-mail"),
            L("Outbound delivery, retries and throttling. Keeping forwarded mail SPF-aligned (SRS) and bounce tagging (BATV) are on the Transport security page."));
         del.Settings.Add(new ComText { Path = "SMTPNoOfTries", Label = L("Number of delivery retries"), Numeric = true });
         del.Settings.Add(new ComText { Path = "SMTPMinutesBetweenTry", Label = L("Minutes between retries"), Numeric = true });
         del.Settings.Add(new ComText { Path = "MaxNumberOfMXHosts", Label = L("Max MX hosts to try (0 = all)"), Numeric = true });
         del.Settings.Add(new IniBool
         {
            Path = "OutboundPipelining",
            Label = L("Use PIPELINING when the receiving server offers it"),
            Default = true,
            Blurb = L("Sends the commands of a transaction without waiting for each answer, which removes a round trip per recipient - the difference is largest on a slow link or to a distant server. Used only with a server that advertises it."),
            IniStore = iniStore_
         });
         del.Settings.Add(new IniBool
         {
            Path = "OutboundChunking",
            Label = L("Use CHUNKING (BDAT) when the receiving server offers it"),
            Default = true,
            Blurb = L("Sends the message as counted chunks rather than dot-stuffed DATA. It is also what carries a binary message onward without rewriting it: a message accepted as BINARYMIME can only be relayed as it arrived to a server that offers both this and BINARYMIME."),
            IniStore = iniStore_
         });
         del.Settings.Add(new IniBool
         {
            Path = "DeliveryHardLinks",
            Label = L("Hard link each local recipient's copy instead of copying the file"),
            Default = false,
            Blurb = L("A message to ten local mailboxes is written once and linked ten times rather than copied ten times, which is the difference between one disk write and ten for a large message to a distribution list. Needs the data directory on NTFS; where a link cannot be made the server copies the file and says so in the SMTP log."),
            IniStore = iniStore_
         });
         // Directly modifies the retry count three rows up, and only makes sense
         // read together with the MX host limit immediately above it, so it moved
         // here from the catch-all INI page.
         del.Settings.Add(new IniNumber
         {
            Path = "MXTriesFactor",
            Label = L("Extra delivery attempts per additional MX host (0 = default)"),
            Default = 0,
            Blurb = L("A recipient domain with several MX hosts is worth more attempts than one with a single host, because a retry can land on a different server. This multiplies the retry count above by the number of MX hosts actually tried."),
            IniStore = iniStore_
         });
         del.Settings.Add(new ComText { Path = "SMTPDeliveryBindToIP", Label = L("Bind outbound connections to IP (empty = any)") });
         del.Settings.Add(new ComCombo { Path = "SMTPConnectionSecurity", Label = L("Outbound delivery security (after MX lookup)"), Options = ConnSecurity });
         del.Settings.Add(new ComBool { Path = "AddDeliveredToHeader", Label = L("Add Delivered-To header") });
         // Quick retries override "Minutes between retries" above for the first N
         // attempts, so the two have to be visible together to make sense.
         del.Settings.Add(new IniNumber { Path = "QuickRetries", Label = L("Quick early retries before the normal schedule (0 = off)"), Default = 0, IniStore = iniStore_ });
         del.Settings.Add(new IniNumber { Path = "QuickRetriesMinutes", Label = L("Minutes between quick retries"), Default = 6, IniStore = iniStore_ });
         del.Settings.Add(new IniNumber { Path = "QueueRandomnessMinutes", Label = L("Random jitter added to retry times (minutes, 0 = off)"), Default = 0, IniStore = iniStore_ });
         del.Settings.Add(new IniNumber
         {
            Path = "MaxOutboundPerDestinationPerMinute",
            Label = L("Max outbound messages per destination domain per minute (0 = unlimited)"),
            Default = 0,
            Blurb = L("Throttling your own outbound rate to a domain that rate-limits you. A deferred message is retried a minute later without spending the retry budget above."),
            IniStore = iniStore_
         });
         // Outbound shaping, beside the fixed per-destination rate it completes:
         // the back-off a destination's own slow-down reply opens, and the two
         // connection ceilings. All four are hm_settings rows reached over COM.
         del.Settings.Add(new ComText
         {
            Path = "OutboundBackoffMinutes",
            Label = L("Minutes to hold a destination that asked this server to slow down (0 = off)"),
            Numeric = true,
            Blurb = L("A 421, a rate-limit code or words to that effect from a remote server opens a window in which nothing is sent to that destination; when it ends one delivery probes it, and the same answer doubles the window. A deferral this server chooses does not count against the retries above.")
         });
         del.Settings.Add(new ComText
         {
            Path = "OutboundBackoffMaxMinutes",
            Label = L("Longest such window, in minutes"),
            Numeric = true
         });
         del.Settings.Add(new ComText
         {
            Path = "MaxOutboundConnectionsPerDestination",
            Label = L("Max simultaneous connections to one destination without a remote domain policy (0 = unlimited)"),
            Numeric = true
         });
         del.Settings.Add(new ComText
         {
            Path = "MaxOutboundConnectionsPerTenant",
            Label = L("Max deliveries in flight for one sending domain (0 = unlimited)"),
            Numeric = true,
            Blurb = L("A share of connections, never more than the delivery threads: the accounts of one hosted domain, or all relayed mail together, cannot hold every delivery thread. The per-account sending limit is the volume control; this is not one.")
         });
         Tab(L("Delivery")).Cards.Add(del);

         // Bounces read, and the suppression list: what happens after a delivery
         // gives up, so it sits under the retries that decide when that is. The
         // list itself is a page of its own under Monitoring.
         var bounces = Card(L("Bounces and the suppression list"),
            L("Reads the refusals and delivery reports that come back for this server's senders, counts them per address, and stops sending to an address that has bounced for good. The list is per sending domain, and it is on the Suppression list page."));
         bounces.Settings.Add(new ComBool
         {
            Path = "BounceProcessingEnabled",
            Label = L("Classify bounces and keep a suppression list"),
            Blurb = L(SettingClaims.NoteFor("BounceProcessingEnabled"))
         });
         bounces.Settings.Add(new ComText
         {
            Path = "BounceHardBounceThreshold",
            Label = L("Hard bounces in a row before an address is listed (1 to 100)"),
            Numeric = true,
            Blurb = L("A delivery between two hard bounces starts the count again. A refusal this server cannot trust as a dead mailbox - a bare 550 with no enhanced code, a policy refusal, its own connection failures - never counts.")
         });
         bounces.Settings.Add(new ComBool
         {
            Path = "BounceMailboxFullIsHard",
            Label = L("Count a full mailbox as a hard bounce"),
            Blurb = L("Off by default: a full mailbox is usually emptied, and listing it would stop mail to somebody who reads it next week.")
         });
         bounces.Settings.Add(new ComCombo
         {
            Path = "BounceSuppressionAction",
            Label = L("What happens to a listed recipient"),
            Options = ToOptions(SettingClaims.BounceSuppressionActionOptions),
            Blurb = L(SettingClaims.NoteFor("BounceSuppressionAction"))
         });
         bounces.Settings.Add(new ComText
         {
            Path = "BounceSuppressionExpiryDays",
            Label = L("Days before an entry made by a bounce expires (0 = never)"),
            Numeric = true,
            Blurb = L("Measured from when the entry was made, so lowering it applies to the entries already there. An entry added by hand keeps its own expiry.")
         });
         bounces.Settings.Add(new ComBool
         {
            Path = "BounceReadDeliveryReports",
            Label = L("Read delivery reports that arrive for this server's senders"),
            Blurb = L(SettingClaims.NoteFor("BounceReadDeliveryReports"))
         });
         bounces.Settings.Add(new ComText
         {
            Path = "BounceCountRetentionDays",
            Label = L("Days bounce counts are kept (0 = keep)"),
            Numeric = true
         });
         bounces.Settings.Add(new ComText
         {
            Path = "BounceReportWindowDays",
            Label = L("Days after sending that a delivery report is still matched (1 to 30)"),
            Numeric = true
         });
         Tab(L("Delivery")).Cards.Add(bounces);

         var relay = Card(L("SMTP relayer (smart host)"),
            L("Route all outbound mail through another SMTP server instead of delivering directly. For failover, separate several hosts with a vertical bar: the next host is tried when one cannot be reached (all hosts share the port, security and credentials below)."));
         relay.Settings.Add(new ComText { Path = "SMTPRelayer", Label = L("Relay host name (empty = direct delivery; use host1|host2 for failover)") });
         relay.Settings.Add(new ComText { Path = "SMTPRelayerPort", Label = L("Port"), Numeric = true });
         relay.Settings.Add(new ComCombo { Path = "SMTPRelayerConnectionSecurity", Label = L("Connection security"), Options = ConnSecurity });
         relay.Settings.Add(new ComBool { Path = "SMTPRelayerRequiresAuthentication", Label = L("Relay requires authentication") });
         relay.Settings.Add(new ComText { Path = "SMTPRelayerUsername", Label = L("User name") });
         relay.Settings.Add(new ComPassword { Path = "SetSMTPRelayerPassword", MethodName = "SetSMTPRelayerPassword", Label = L("Password (leave empty to keep current)") });
         Tab(L("Relayer")).Cards.Add(relay);

         var oauth = Card(L("OAuth2 for the relay (Microsoft 365)"),
            L("Microsoft 365 turns Basic authentication for SMTP AUTH off at the end of December 2026. When the relay host below is listed here, hMailServer authenticates with an OAuth2 bearer token (XOAUTH2) instead of the password - fetched from your tenant's token endpoint with the client credentials of an app registration that has the SMTP.SendAsApp permission. The user name above stays: it is the mailbox being relayed as."));
         oauth.Settings.Add(new IniText
         {
            Path = "OutboundOAuth2Hosts",
            Label = L("Relay hosts that use OAuth2 (comma separated; blank this to use none)"),
            Default = "smtp.office365.com",
            Placeholder = "smtp.office365.com",
            Blurb = L("Only destinations named here get a bearer token; every other relay keeps password authentication. This is deliberate - a token presented to the wrong relay is a leaked token."),
            IniStore = iniStore_
         });
         oauth.Settings.Add(new IniText
         {
            Path = "OutboundOAuth2TokenUrl",
            Label = L("Token endpoint (https)"),
            Placeholder = "https://login.microsoftonline.com/<tenant-id>/oauth2/v2.0/token",
            IniStore = iniStore_
         });
         oauth.Settings.Add(new IniText
         {
            Path = "OutboundOAuth2ClientId",
            Label = L("Application (client) ID"),
            IniStore = iniStore_
         });
         oauth.Settings.Add(new IniText
         {
            Path = "OutboundOAuth2ClientSecret",
            Label = L("Client secret"),
            Blurb = L("Stored in hMailServer.ini on the server. The token it buys is cached in memory until 80% of its lifetime has passed and refetched automatically."),
            IniStore = iniStore_
         });
         oauth.Settings.Add(new IniText
         {
            Path = "OutboundOAuth2Scope",
            Label = L("Scope (empty = the Microsoft default)"),
            Placeholder = "https://outlook.office365.com/.default",
            IniStore = iniStore_
         });
         oauth.Settings.Add(new IniText
         {
            Path = "OutboundOAuth2FixedToken",
            Label = L("Fixed token (advanced; used verbatim instead of fetching)"),
            Blurb = L("For tokens obtained outside this server. Leave empty in normal use - fixed tokens expire and nothing here will refresh them."),
            IniStore = iniStore_
         });
         // The collecting half of the same Microsoft 365 story, and it was the one
         // setting of the pair with no field: an administrator could configure
         // sending through M365 here and had to hand-edit the ini to configure
         // fetching from it, which is the same credential problem in the other
         // direction.
         oauth.Settings.Add(new IniText
         {
            Path = "FetchOAuth2Hosts",
            Label = L("External-account hosts to FETCH from with OAuth2 (comma separated; blank this to use none)"),
            Default = "outlook.office365.com",
            Placeholder = "outlook.office365.com",
            Blurb = L("External accounts whose server is named here log in with a bearer token from the same token endpoint above; every other external account keeps USER/PASS. Collecting from Microsoft 365 with a password has not been possible since 2022."),
            IniStore = iniStore_
         });
         Tab(L("Relayer")).Cards.Add(oauth);

         var rules = Card(L("Rules"));
         rules.Settings.Add(new ComText { Path = "RuleLoopLimit", Label = L("Rule loop limit"), Numeric = true });
         Tab(L("Rules")).Cards.Add(rules);

         // The one detector of the sensitive-content rule condition that has to be
         // told what to look for (docs/OutboundContentPolicy.md). Beside the rules
         // that use it; kept by the server with no COM property.
         var sensitive = Card(L("Sensitive content in rules"),
            L("The sensitive-content condition of a rule looks for card numbers, IBANs and, when a pattern is set here, this organisation's own account numbers."));
         sensitive.Settings.Add(new RestText
         {
            Path = "DlpAccountNumberPattern",
            Label = L("Account-number pattern (a regular expression; empty = none)"),
            Key = "dlp_account_number_pattern",
            Placeholder = "ACC-\\d{8}", // no-loc
            Blurb = L("Perl syntax, searched in a message's text and its text attachments, up to 2000 characters. Empty, the default, is an account-number detector that never fires. A pattern the server cannot compile is refused.")
         });
         Tab(L("Rules")).Cards.Add(sensitive);

         // What comes back after delivery besides bounces: the providers' abuse
         // reports and the reports other servers write about this one's domains.
         var complaints = Card(L("Complaints from mailbox providers (feedback loops)"),
            L("Reads the abuse reports (RFC 5965) that providers send when a reader presses the spam button, counts them per address, and can put an address that complains on its sending domain's suppression list. Nothing arrives until the complaint mailbox is registered with each provider's feedback loop - that is done at the provider, not here."));
         complaints.Settings.Add(new ComBool
         {
            Path = "ComplaintProcessingEnabled",
            Label = L("Read complaints sent to the complaint mailbox"),
            Blurb = L("Off by default. With it off nothing is read, counted or written.")
         });
         complaints.Settings.Add(new ComText
         {
            Path = "ComplaintMailbox",
            Label = L("Complaint mailbox (comma separated for several)"),
            Placeholder = "fbl@example.com", // no-loc
            Blurb = L("The address the feedback loops are registered against, which must be one this server hosts. Only a report delivered here is read, because arriving where the provider was told to send it is what tells a report from a forgery. Empty, the default, reads nothing.")
         });
         complaints.Settings.Add(new ComText
         {
            Path = "ComplaintSuppressionThreshold",
            Label = L("Complaints before an address is suppressed (0 = never; up to 100)"),
            Numeric = true,
            Maximum = 100,
            Blurb = L("0 by default: every report is recorded and counted and nobody is listed. Read some real entries first; 1 is then the usual value.")
         });
         complaints.Settings.Add(new ComText
         {
            Path = "ComplaintRetentionDays",
            Label = L("Days a complaint is kept (0 = for ever; up to 3650)"),
            Numeric = true,
            Maximum = 3650,
            Blurb = L("180 by default. A suppression list entry a complaint made keeps its own expiry, set with the bounces.")
         });
         Tab(L("Feedback")).Cards.Add(complaints);

         var domainReports = Card(L("Reports your domains receive"),
            L("The DMARC aggregate and TLS-RPT reports other servers send about your domains, read from each domain's report mailbox - which is set in the domain's dialog."));
         domainReports.Settings.Add(new RestNumber
         {
            Path = "DomainReportRetentionDays",
            Label = L("Days a received report is kept (0 = for ever; up to 3650)"),
            Key = "domain_report_retention_days",
            Maximum = 3650,
            Blurb = L("365 by default.")
         });
         Tab(L("Feedback")).Cards.Add(domainReports);

         // Whether this server's own sending addresses are on the block lists its
         // recipients consult - the outbound half of reputation.
         var reputation = Card(L("Block lists and this server's own addresses"),
            L("Once a day the server asks the DNS block lists below whether an address it sends from is listed, and raises the reputation.listed alert when one is. It checks the delivery bind address and the addresses the global rules bind; a server behind NAT has to be told its public address here, because nothing on the machine can find it."));
         reputation.Settings.Add(new ComBool
         {
            Path = "ReputationWatchEnabled",
            Label = L("Check this server's sending addresses against block lists every day"),
            Blurb = L("On in a new installation and off after an upgrade. With no address to check it looks nothing up.")
         });
         reputation.Settings.Add(new ComText
         {
            Path = "ReputationWatchAddresses",
            Label = L("Other addresses to check (commas or spaces)"),
            Placeholder = "203.0.113.25", // no-loc
            Blurb = L("The public address other servers see, when it is not an address of this machine.")
         });
         reputation.Settings.Add(new ComText
         {
            Path = "ReputationWatchLists",
            Label = L("Block lists to ask"),
            Blurb = L("Each as [ipv4:|ipv6:]zone[=codes], separated by commas or spaces, such as ipv4:zen.spamhaus.org. By default four lists, asked for IPv4: Spamhaus ZEN, SpamCop, PSBL and Mailspike.")
         });
         Tab(L("Reputation")).Cards.Add(reputation);

         // The distribution lists switched to managed in the list dialog
         // (docs/MailingLists.md). Kept by the server with no COM property.
         var lists = Card(L("Managed mailing lists"),
            L("For the distribution lists switched to managed in the list dialog: where the links in their mail point, and when a member whose mail bounces stops being sent it. The member stays on the list, and the list's owner is told once."));
         lists.Settings.Add(new RestText
         {
            Path = "ListBaseUrl",
            Label = L("Base address of the list pages (empty = no links)"),
            Key = "list_base_url",
            Placeholder = "https://lists.example.com", // no-loc
            Blurb = L("The address the REST API is reached at from outside; the subscribe, confirm, unsubscribe and archive pages are under it. Only an https address gives one-click unsubscribe (RFC 8058). Empty, the default, sends mailto addresses only.")
         });
         lists.Settings.Add(new RestNumber
         {
            Path = "ListHardBouncesToUnsubscribe",
            Label = L("Hard bounces before a member is no longer sent mail (1 to 100)"),
            Key = "list_hard_bounces_to_unsubscribe",
            Minimum = 1,
            Maximum = 100,
            Blurb = L("1 by default.")
         });
         lists.Settings.Add(new RestNumber
         {
            Path = "ListSoftBouncesToUnsubscribe",
            Label = L("Soft bounces within the window before a member is no longer sent mail (1 to 100)"),
            Key = "list_soft_bounces_to_unsubscribe",
            Minimum = 1,
            Maximum = 100,
            Blurb = L("5 by default. A delivery that succeeds does not clear the count; the window does.")
         });
         lists.Settings.Add(new RestNumber
         {
            Path = "ListBounceWindowDays",
            Label = L("Days the soft bounces are counted within (1 to 365)"),
            Key = "list_bounce_window_days",
            Minimum = 1,
            Maximum = 365,
            Blurb = L("30 by default.")
         });
         Tab(L("Mailing lists")).Cards.Add(lists);
      }

      private void BuildAntiSpam()
      {
         Header.Title = L("Anti-spam");
         Header.Subtitle = L("Score-based spam filtering: SPF, DKIM, DMARC, host checks, greylisting and SpamAssassin.");

         var general = Card(L("Thresholds & actions"));
         general.Settings.Add(new ComText { Path = "AntiSpam.SpamMarkThreshold", Label = L("Spam mark threshold (score)"), Numeric = true });
         general.Settings.Add(new ComText { Path = "AntiSpam.SpamDeleteThreshold", Label = L("Spam delete threshold (score)"), Numeric = true });
         general.Settings.Add(new ComBool { Path = "AntiSpam.AddHeaderSpam", Label = L("Add X-hMailServer-Spam header") });
         general.Settings.Add(new ComBool { Path = "AntiSpam.AddHeaderReason", Label = L("Add X-hMailServer-Reason header") });
         general.Settings.Add(new ComBool { Path = "AntiSpam.PrependSubject", Label = L("Prepend text to subject") });
         general.Settings.Add(new ComText { Path = "AntiSpam.PrependSubjectText", Label = L("Subject prefix") });
         general.Settings.Add(new ComText { Path = "AntiSpam.MaximumMessageSize", Label = L("Max message size to spam-scan (KB, 0 = unlimited)"), Numeric = true });
         Tab(L("General")).Cards.Add(general);

         // Not a score: this one costs the sender time rather than points, which is
         // why it is a card of its own rather than a row in the thresholds.
         var tarpit = Card(L("Recipient tarpit"),
            L("Slows a session that names more recipients than a legitimate message needs. Past the count, every further RCPT TO in an unauthenticated session waits the delay before it is answered - a dictionary attack or a spam run names hundreds of addresses and pays for each one, while a sending MTA names a handful and never reaches the count. Authenticated sessions and IP ranges with spam protection off are exempt. The wait is a timer on the connection, not a thread asleep. Takes effect at once; stored in hMailServer.ini as SmtpTarpitCount and SmtpTarpitDelaySeconds."));
         tarpit.Settings.Add(new ComText { Path = "AntiSpam.TarpitCount", Label = L("Recipients a message may name before the delay starts (0 = off)"), Numeric = true });
         tarpit.Settings.Add(new ComText { Path = "AntiSpam.TarpitDelay", Label = L("Delay for each further recipient (seconds, at most 30; 0 = off)"), Numeric = true });
         Tab(L("General")).Cards.Add(tarpit);

         // Directly under the delete threshold, because this setting changes what that
         // threshold DOES rather than adding a feature beside it.
         var quarantine = Card(L("Quarantine instead of refusing"),
            L("Changes what happens at the delete threshold. Off, a message over it is refused during the SMTP conversation with a 550 and the sender is told. On, it is accepted with a 250 and held for review instead - the sender believes it was delivered and will not retry, which is what makes a false positive recoverable without bouncing to a return path that is probably forged, and also means the review queue becomes the only place that message exists. Applies to checks that run after the message body has been received; a verdict reached before that (a blacklisted connecting IP, a bad HELO) has no message to hold and stays a refusal."));
         quarantine.Settings.Add(new IniBool
         {
            Path = "QuarantineEnabled",
            Label = L("Hold messages for review instead of refusing them"),
            Blurb = L("Off by default. Turning it on means the server starts storing mail it considers spam, so watch the retention window below."),
            IniStore = iniStore_
         });
         quarantine.Settings.Add(new IniNumber
         {
            Path = "QuarantineRetentionDays",
            Label = L("Delete quarantined messages after (days; 0 = never)"),
            Default = 30,
            Blurb = L("0 is a real answer, not a disabled one - it keeps everything, which is what an administrator collecting evidence wants. Everything else is a promise to the disk."),
            IniStore = iniStore_
         });
         Tab(L("General")).Cards.Add(quarantine);

         var auth = Card(L("Sender authentication"));
         auth.Settings.Add(new ComBool { Path = "AntiSpam.UseSPF", Label = L("Check SPF") });
         auth.Settings.Add(new ComText { Path = "AntiSpam.UseSPFScore", Label = L("SPF failure score"), Numeric = true });
         // SPF's other results, scored on their own (discussion #286). Fail keeps
         // the score above; these three are 0 by default, and 0 is "not scored"
         // as every result but Fail always was. Neutral and None are never scored.
         auth.Settings.Add(new ComText
         {
            Path = "AntiSpam.UseSPFSoftFailScore",
            Label = L("SPF softfail score (0 = not scored)"),
            Numeric = true,
            Blurb = L("~all: the domain says the client is probably not authorised and asks that the message be accepted anyway. Many spray sources live behind it. A score at or above the delete threshold refuses the message during the SMTP conversation as a fail does; a softfail is never a pass for the greylisting bypass.")
         });
         auth.Settings.Add(new ComText
         {
            Path = "AntiSpam.UseSPFTempErrorScore",
            Label = L("SPF temperror score (0 = not scored)"),
            Numeric = true,
            Blurb = L("The check did not finish - DNS timed out or answered SERVFAIL, or the lookup limit was reached - and the record may be fine. Scoring it like a fail drops mail on resolver blips; the switch below is the other answer.")
         });
         auth.Settings.Add(new ComBool
         {
            Path = "AntiSpam.SPFTempErrorDefer",
            Label = L("Answer 451 to an SPF temperror instead of accepting it"),
            Blurb = L("RFC 7208's own answer to a check that could not be completed: try again when DNS works. The transaction is deferred with 451 4.4.3 and the temperror score above is ignored, because a deferred message is never accepted. Off, a temperror is accepted and scored. A sender on the domain white list is never deferred.")
         });
         auth.Settings.Add(new ComText
         {
            Path = "AntiSpam.UseSPFPermErrorScore",
            Label = L("SPF permerror score (0 = not scored)"),
            Numeric = true,
            Blurb = L("The check finished and the domain's published record is unusable: two records, a bad mechanism, a redirect loop, too many lookups. A retry will not fix it, so it is never deferred; strict sites treat it as a fail and lenient sites ignore it.")
         });
         auth.Settings.Add(new IniNumber
         {
            Path = "SpfVoidLookupLimit",
            Label = L("Void SPF lookups allowed before permerror (0 = no limit)"),
            Blurb = L("RFC 7208 4.6.4. A \"void lookup\" is a term of a sender's SPF policy - an a:, mx: or exists: - whose DNS query comes back with nothing at all. The separate limit of ten DNS-querying terms bounds what a policy can ask this server to look up; it does not bound what a policy can waste, and ten terms naming hosts that do not exist cost a full resolution each for a record that can never match. Two is what the RFC says. It also catches the ordinary case: a policy still listing hosts decommissioned years ago, which its owner would rather be told about. Raising it is safe; 0 switches the limit off, which is the setting for a correspondent whose policy is wrong in exactly this way and whose mail you would rather take."),
            IniStore = iniStore_
         });
         auth.Settings.Add(new ComBool { Path = "AntiSpam.DKIMVerificationEnabled", Label = L("Verify DKIM signatures") });
         auth.Settings.Add(new ComText { Path = "AntiSpam.DKIMVerificationFailureScore", Label = L("DKIM failure score"), Numeric = true });
         auth.Settings.Add(new IniBool
         {
            Path = "DkimAcceptSha1",
            Label = L("Accept DKIM signatures that use rsa-sha1"),
            Blurb = L("Off, and off is what RFC 8301 requires: it removed rsa-sha1 from DKIM outright, so a signer must not use it and a verifier must treat a signature that does as invalid. A DKIM signature is an assertion of identity and DMARC alignment is built on it, and SHA-1 collisions have been practical since 2020 - a signature somebody can forge is worth less than no signature at all, because it carries the signer's domain past an aligned DMARC check. Turning this on restores both halves: signatures using it verify again, and a domain configured to sign with it does so instead of being upgraded to rsa-sha256. Keys shorter than 1024 bits are refused regardless, and this setting does not affect that."),
            IniStore = iniStore_
         });
         auth.Settings.Add(new ComBool { Path = "AntiSpam.DMARCEnabled", Label = L("Evaluate DMARC policies") });
         auth.Settings.Add(new ComText { Path = "AntiSpam.DMARCFailureScore", Label = L("DMARC failure score"), Numeric = true });
         auth.Settings.Add(new IniBool
         {
            Path = "DmarcTreeWalkEnabled",
            Label = L("Find organizational domains by DNS tree walk (RFC 9989)"),
            Blurb = L("How the server decides that mail.example.com and example.com belong to the same organization, which is what DMARC's default relaxed alignment compares. The old answer was the Public Suffix List - a file of every registry's delegation rules, compiled in and refreshed at build time. DMARCbis replaced it with a walk up the DNS asking each level whether it is an organizational boundary, so a domain owner can state where their boundary is instead of petitioning a list. Turning this off falls back to the compiled list, which is also what happens for any single lookup the walk cannot complete because DNS was unavailable - a resolver outage must not quietly turn relaxed alignment into strict. Costs up to eight DNS queries per domain, cached for five minutes."),
            IniStore = iniStore_
         });
         auth.Settings.Add(new IniNumber
         {
            Path = "DmarcRptSchemaVersion",
            Label = L("Aggregate-report schema this server sends: 1 = RFC 7489 Appendix C, 2 = RFC 9990"),
            Default = 1,
            MinimumValue = 1,
            Blurb = L("Which spelling of the DMARC aggregate report goes out to the domains this server reports on. 1 is what every report consumer deployed today parses; 2 is DMARCbis, for the day the consumers have caught up. Any other value is reported as an error and treated as 1, because a typo must not decide what goes on the wire."),
            IniStore = iniStore_
         });
         auth.Settings.Add(new ComBool
         {
            Path = "AntiSpam.ArcFilteringEnabled",
            Label = L("Use ARC results from trusted forwarders to offset a DMARC failure"),
            Blurb = L("Forwarding breaks SPF, because the envelope sender changes, and often breaks DKIM, because the body is modified - so a mailing list or a forwarded mailbox can turn a message that passed DMARC at the sender into one that fails it here. A valid ARC chain (RFC 8617) from a forwarder you trust carries the original result, and this cancels exactly the DMARC failure score, never more. It needs 'Evaluate DMARC policies' above to be on, since it only ever offsets a penalty that test adds.")
         });
         auth.Settings.Add(new ComText
         {
            Path = "AntiSpam.ArcTrustedSealers",
            Label = L("Trusted ARC sealer domains"),
            Blurb = L("Required. With this empty the setting above does nothing at all, and that is deliberate rather than an oversight: anyone can fabricate a whole ARC chain and seal it with a key they publish in their own DNS, and it will validate perfectly, so a passing chain proves nothing unless you already trust the sealer. This list is not an option of the feature - it is the feature. Name the exact domains whose seals you honour (the d= of their ARC-Seal), separated by commas, semicolons or spaces; matching is exact, so a suffix of a trusted name is not trusted.")
         });
         Tab(L("Sender auth")).Cards.Add(auth);

         var host = Card(L("Connecting host checks"));
         host.Settings.Add(new ComBool { Path = "AntiSpam.CheckHostInHelo", Label = L("Check host in HELO") });
         host.Settings.Add(new ComText { Path = "AntiSpam.CheckHostInHeloScore", Label = L("HELO check score"), Numeric = true });
         host.Settings.Add(new ComBool { Path = "AntiSpam.CheckPTR", Label = L("Check PTR record") });
         host.Settings.Add(new ComText { Path = "AntiSpam.CheckPTRScore", Label = L("PTR check score"), Numeric = true });
         host.Settings.Add(new ComBool { Path = "AntiSpam.UseMXChecks", Label = L("Check sender MX records") });
         host.Settings.Add(new ComText { Path = "AntiSpam.UseMXChecksScore", Label = L("MX check score"), Numeric = true });
         Tab(L("Host checks")).Cards.Add(host);

         // Not a score, like the tarpit above: a null sender that names a second
         // recipient is a protocol lie, not "maybe spam", and it is answered with
         // a refusal and optionally a ban rather than points.
         var nullSender = Card(L("Null sender with several recipients"),
            L("MAIL FROM:<> is the reverse-path of a delivery report, and a report has exactly one recipient: the address that sent the message it is about. A null sender that goes on to name a second recipient is backscatter, a forged report or a probe. Off, the server accepts it as it always has. On, the second and every later RCPT TO is answered 550 and the transaction's DATA is refused as well; a null sender with one recipient is never touched. Unauthenticated sessions of an IP range that runs the spam tests only, and never a sender on the anti-spam white list. The domain white and black list is a page of its own under Anti-spam."));
         nullSender.Settings.Add(new ComBool
         {
            Path = "AntiSpam.RejectMultiRcptNullSender",
            Label = L("Refuse a null sender that names more than one recipient"),
            Blurb = L("Off by default. Operators who drop this in a script today can delete the script when this is on.")
         });
         nullSender.Settings.Add(new ComBool
         {
            Path = "AntiSpam.BanMultiRcptNullSender",
            Label = L("Also ban the connecting address for a while"),
            Blurb = L("The same expiring IP range a run of failed logons creates, lifted by the same sweep and mirrored into the firewall by the same reconciliation; never for an address on AutoBanNeverBan or an incoming relay. Off by default, so one mis-sent report with two recipients does not ban a real mail server. Does nothing while the refusal above is off.")
         });
         nullSender.Settings.Add(new ComText { Path = "AntiSpam.BanMultiRcptNullSenderHours", Label = L("Ban for (hours, 1 to 8760)"), Numeric = true });
         Tab(L("Host checks")).Cards.Add(nullSender);

         var grey = Card(L("Greylisting"), L("Temporarily rejects mail from unknown senders; legitimate servers retry and pass."));
         grey.Settings.Add(new ComBool { Path = "AntiSpam.GreyListingEnabled", Label = L("Enable greylisting") });
         grey.Settings.Add(new ComText { Path = "AntiSpam.GreyListingInitialDelay", Label = L("Initial delay (minutes)"), Numeric = true });
         grey.Settings.Add(new ComText { Path = "AntiSpam.GreyListingInitialDelete", Label = L("Delete unconfirmed after (days)"), Numeric = true, Divisor = 24 });
         grey.Settings.Add(new ComText { Path = "AntiSpam.GreyListingFinalDelete", Label = L("Delete confirmed after (days)"), Numeric = true, Divisor = 24 });
         grey.Settings.Add(new ComBool { Path = "AntiSpam.BypassGreylistingOnMailFromMX", Label = L("Bypass when sender matches MX") });
         grey.Settings.Add(new ComBool { Path = "AntiSpam.BypassGreylistingOnSPFSuccess", Label = L("Bypass on SPF success") });
         // The hardened greylisting of 18 September 2026 (discussion #282): each
         // off or inert by default, so a server that never touches them runs
         // classic greylisting. A server older than these has no such properties
         // and the rows say so as every other COM row does.
         grey.Settings.Add(new ComBool
         {
            Path = "AntiSpam.GreyListingSlideOnEarlyRetry",
            Label = L("Move the window on a retry that comes too soon"),
            Blurb = L("Each retry before the initial delay has passed starts the delay again from that moment, so a sender that retries on a timer rather than from a queue never reaches the window. Off by default: a well-behaved server that retries once too quickly is delayed a little longer with it on.")
         });
         grey.Settings.Add(new ComText
         {
            Path = "AntiSpam.GreyListingAbuseThreshold",
            Label = L("Refuse with 550 after this many retries before the window opens (0 never refuses)"),
            Numeric = true,
            Blurb = L("A triplet retried too soon this many times is answered 550 instead of 451 for the lock below, which is what stops a sender hammering the same message every few seconds. A misconfigured retry loop is stranded until the lock passes, after which the triplet starts over as a first attempt. The operator who asked for it uses 20.")
         });
         grey.Settings.Add(new ComText
         {
            Path = "AntiSpam.GreyListingAbuseLockMinutes",
            Label = L("How long that refusal lasts (minutes)"),
            Numeric = true,
            Blurb = L("60 by default; means nothing while the threshold above is 0.")
         });
         // The last two greylisting settings are stored in hMailServer.INI rather
         // than in the settings database, and they used to be on the catch-all INI
         // page because of it - so half of greylisting was configured here and half
         // somewhere an administrator would only find by accident. Both apply when
         // they are saved, as the COM rows above do; the mark under each says so.
         grey.Settings.Add(new IniBool
         {
            Path = "GreylistingEnabledDuringRecordExpiration",
            Label = L("Keep greylisting active during the record-expiration window"),
            Default = true,
            Blurb = L("A triplet that has been seen before but has not yet been confirmed sits in a window before its record expires. Turning this off lets mail through during that window instead of greylisting it again, which is gentler on senders that retry slowly and weaker against a sender that retries once."),
            IniStore = iniStore_
         });
         grey.Settings.Add(new IniNumber
         {
            Path = "GreylistingRecordExpirationInterval",
            Label = L("Record expiration interval, minutes"),
            Default = 240,
            Blurb = L("How long that window lasts. Four hours by default."),
            IniStore = iniStore_
         });
         grey.Settings.Add(new ComAction
         {
            Path = "AntiSpam.GreyListingEnabled",
            ButtonText = L("Clear greylisting triplets"),
            Action = () =>
            {
               dynamic a = ServerSession.Current.Application.Settings.AntiSpam;
               try { a.ClearGreyListingTriplets(); return (true, L("Greylisting triplets cleared.")); }
               finally { ServerSession.Release((object)a); }
            }
         });
         Tab(L("Greylisting")).Cards.Add(grey);

         var sa = Card(L("SpamAssassin"));
         sa.Settings.Add(new ComBool { Path = "AntiSpam.SpamAssassinEnabled", Label = L("Use SpamAssassin") });
         var saHost = new ComText { Path = "AntiSpam.SpamAssassinHost", Label = L("Host") };
         var saPort = new ComText { Path = "AntiSpam.SpamAssassinPort", Label = L("Port"), Numeric = true };
         sa.Settings.Add(saHost);
         sa.Settings.Add(saPort);
         sa.Settings.Add(new ComBool { Path = "AntiSpam.SpamAssassinMergeScore", Label = L("Merge SpamAssassin score into hMailServer score") });
         sa.Settings.Add(new ComText { Path = "AntiSpam.SpamAssassinScore", Label = L("Score when not merging"), Numeric = true });
         sa.Settings.Add(new ComAction
         {
            Path = "AntiSpam.SpamAssassinEnabled",
            ButtonText = L("Test SpamAssassin connection"),
            Action = () =>
            {
               string host = saHost.CurrentText;
               int.TryParse(saPort.CurrentText, out int port);
               return TestSpamAssassin(host, port);
            }
         });
         // Timeouts belong with the scanner they apply to: an admin whose mail is
         // backing up looks at the SpamAssassin tab, not at a hardening page.
         sa.Settings.Add(new IniNumber { Path = "SAMinTimeout", Label = L("Minimum timeout (seconds)"), Default = 30, IniStore = iniStore_ });
         sa.Settings.Add(new IniNumber
         {
            Path = "SAMaxTimeout",
            Label = L("Maximum timeout (seconds)"),
            Default = 90,
            Blurb = L("The effective timeout moves between these values with server load."),
            IniStore = iniStore_
         });
         sa.Settings.Add(new IniBool
         {
            Path = "SAMoveVsCopy",
            Label = L("Move the message file to SpamAssassin instead of copying it"),
            Default = false,
            Blurb = L("How the message reaches SpamAssassin when it runs on this machine: a move saves writing a second copy of every message to disk, which matters on a busy server. It only works when SpamAssassin reads from the same volume; against a remote spamd it is the wrong choice."),
            IniStore = iniStore_
         });
         // The User: header of the spamd request. spamd applies per-user preferences to
         // the user a request names and its global configuration to a request that names
         // nobody; until these two existed every scan named nobody.
         sa.Settings.Add(new IniText
         {
            Path = "SpamAssassinUser",
            Label = L("Profile to scan as (spamd User: header; empty = spamd's global configuration)"),
            Blurb = L("The name spamd looks up its per-user preferences for - a user_prefs file or a row in its SQL preference store. Leave empty unless spamd has been set up for it: a spamd told to change to a user it cannot find may refuse the scan, and an unscanned message is delivered as if SpamAssassin were unreachable."),
            IniStore = iniStore_
         });
         sa.Settings.Add(new IniBool
         {
            Path = "SpamAssassinUserFromRecipient",
            Label = L("Scan a message with a single recipient as that recipient"),
            Default = false,
            Blurb = L("Sends the recipient's address as the User: header, so spamd applies that mailbox's own preferences - the shape a virtual-user spamd (--virtual-config-dir with %u or %d) expects. A scan runs once per message, not once per recipient, so a message to several people cannot be scanned under any one of their preferences and uses the profile above instead."),
            IniStore = iniStore_
         });
         sa.Settings.Add(new IniBool
         {
            Path = "SpamAssassinLearnOnMove",
            Label = L("Teach SpamAssassin when a user files a message as spam or takes it back out"),
            Default = false,
            Blurb = L("Moving a message into the folder designated Junk reports it to sa-learn as spam, and moving one back out reports it as ham, so the filter learns from what people already do rather than from a separate ritual nobody performs. Off by default: it runs sa-learn per move, and a user who files newsletters in Junk is teaching the filter that newsletters are spam for everyone the scanner serves."),
            IniStore = iniStore_
         });
         Tab(L("SpamAssassin")).Cards.Add(sa);

         var hook = Card(L("External filtering engine (rspamd and anything else that speaks HTTP)"),
            L("Hands every accepted message to an engine over HTTP and uses the score it returns. The message is the request body and the envelope, connecting address and HELO travel as request headers, which is what rspamd's own check endpoint already expects - so pointing this at rspamd needs no adapter, and pointing it at something you wrote yourself needs only a program that answers JSON."));
         hook.Settings.Add(new IniText
         {
            Path = "FilterHookUrl",
            Label = L("Engine URL (empty = no external engine)"),
            Blurb = L("For example http://127.0.0.1:11333/checkv2. Plain HTTP only for now, so the engine should be on this machine or on a network you trust - the whole message is sent to it."),
            IniStore = iniStore_
         });
         hook.Settings.Add(new IniNumber
         {
            Path = "FilterHookTimeoutSeconds",
            Label = L("Timeout for the whole exchange (seconds)"),
            Default = 10,
            Blurb = L("This runs while the sending server waits for its answer to DATA, so the number is a promise about the worst case rather than a hint. It bounds the connection as well as the reply: an engine whose machine has gone away would otherwise cost the operating system's own TCP timeout on every message."),
            IniStore = iniStore_
         });
         hook.Settings.Add(new IniBool
         {
            Path = "FilterHookFailClosed",
            Label = L("Treat an unanswered check as spam"),
            Blurb = L("Off by default, and this is the decision worth thinking about. Off, an engine that stops answering lets spam through until somebody notices. On, it stops the mail instead - and a message deferred while nobody is watching is a message that eventually bounces. Off is the recoverable failure, which is why it is the default."),
            IniStore = iniStore_
         });
         hook.Settings.Add(new IniNumber
         {
            Path = "FilterHookRejectScore",
            Label = L("Score applied when the engine says \"reject\""),
            Default = 100,
            Blurb = L("The engine's verdict arrives as a score so that it lands alongside SPF, DKIM and DMARC and your existing thresholds decide what happens - rather than a second, parallel notion of spam that has to be reconciled with the first. This is what its strongest verdict is worth."),
            IniStore = iniStore_
         });
         hook.Settings.Add(new IniNumber
         {
            Path = "FilterHookMaxMessageSizeKB",
            Label = L("Do not send messages larger than (KB; 0 = no limit)"),
            Default = 10240,
            Blurb = L("A message above this is passed without being sent to the engine. Posting a fifty-megabyte attachment across the network to be told it is not spam costs more than the answer is worth. 0 removes the ceiling, which is reasonable for an engine on this machine."),
            IniStore = iniStore_
         });
         Tab(L("SpamAssassin")).Cards.Add(hook);

         // What happens to a server that writes to a spam trap (docs/SpamTraps.md).
         // The traps themselves are declared per domain, in the Control Deck or over
         // COM and REST; nothing here does anything until a domain has one.
         var traps = Card(L("Spam traps"),
            L("A spam trap is an address in one of your domains that nobody uses, so whoever writes to it is sending spam. These settings say what happens to a server that does. The trap addresses are declared per domain in the Control Deck; until a domain has one, nothing here does anything."));
         traps.Settings.Add(new ComText
         {
            Path = "SpamTrapBanMinutes",
            Label = L("Minutes the sending address is banned (0 = no ban; up to 525600)"),
            Numeric = true,
            Maximum = 525600,
            Blurb = L("1440 by default. The ban is an expiring IP range, as an auto-ban is. An authenticated session, the anti-spam white list, an incoming relay and an address on the never-ban list are never banned.")
         });
         traps.Settings.Add(new ComText
         {
            Path = "SpamTrapBlockDays",
            Label = L("Days the envelope sender stays blocked (0 = no block; up to 3650)"),
            Numeric = true,
            Maximum = 3650,
            Blurb = L("30 by default: an ordinary block at server scope, which a nearer allow entry still overrules. The null sender, a sender in a domain this server hosts and a sender on the allow list are never blocked.")
         });
         traps.Settings.Add(new ComText
         {
            Path = "SpamTrapHitRetentionDays",
            Label = L("Days the record of trap hits is kept (1 to 3650)"),
            Numeric = true,
            Minimum = 1,
            Maximum = 3650,
            Blurb = L("90 by default. A ban or a block a hit made is not removed with its record.")
         });
         traps.Settings.Add(new ComBool
         {
            Path = "SpamTrapTeachSpamAssassin",
            Label = L("Teach SpamAssassin the other messages of a session that wrote to a trap"),
            Blurb = L("Off by default. Does nothing unless SpamAssassin is enabled and its spamd allows TELL.")
         });
         Tab(L("Spam traps")).Cards.Add(traps);
      }

      private void BuildAntiVirus()
      {
         Header.Title = L("Anti-virus");
         Header.Subtitle = L("Virus scanning and attachment blocking of received messages.");

         var general = Card(L("Action & notifications"));
         general.Settings.Add(new ComCombo { Path = "AntiVirus.Action", Label = L("When a virus is found"), Options = AntivirusAction });
         general.Settings.Add(new ComBool { Path = "AntiVirus.NotifySender", Label = L("Notify sender") });
         general.Settings.Add(new ComBool { Path = "AntiVirus.NotifyReceiver", Label = L("Notify receiver") });
         general.Settings.Add(new ComText { Path = "AntiVirus.MaximumMessageSize", Label = L("Max message size to virus-scan (KB, 0 = unlimited)"), Numeric = true });
         general.Settings.Add(new ComBool { Path = "AntiVirus.EnableAttachmentBlocking", Label = L("Enable attachment blocking (manage list on the Blocked attachments page)") });
         Tab(L("General")).Cards.Add(general);

         // The posture question the rest of this page cannot answer: what happens
         // to a message when the scanner is switched on and cannot run. Until
         // AVFailAction existed the answer was "delivered, with one line in the
         // error log", which is indistinguishable from "scanned and found clean" -
         // so it belongs here, next to the scanners it applies to, rather than in
         // an ini file the people who care about it will never open.
         var failure = Card(L("When a scanner cannot run"),
            L("A scan that errors, times out or cannot reach its engine is not a clean verdict - nobody looked. The shipped behaviour is to deliver the message anyway, which is what this server has always done; holding it instead is the stricter choice and the one to make deliberately."));
         failure.Settings.Add(new IniChoice
         {
            Path = "AVFailAction",
            Label = L("If a message cannot be scanned"),
            Default = 0,
            Options = AvFailAction,
            Blurb = L("Delivering it is the shipped default and what this server has always done. Holding it never delivers unscanned mail: it is scanned again at the interval below, and returned to the sender after the number of holds below."),
            IniStore = iniStore_
         });
         failure.Settings.Add(new IniNumber
         {
            Path = "AVFailRetryMinutes",
            Label = L("Minutes between scan attempts while held"),
            Default = 15,
            IniStore = iniStore_
         });
         failure.Settings.Add(new IniNumber
         {
            Path = "AVFailMaxHolds",
            Label = L("How many times to hold before returning it to the sender"),
            Default = 16,
            Blurb = L("16 holds at 15 minutes is about four hours. 0 means never queue unscanned mail at all - tell the sender immediately. A held message is delivered as soon as any scanner answers; the count is kept in memory, so a service restart gives a held message a fresh budget."),
            IniStore = iniStore_
         });
         Tab(L("General")).Cards.Add(failure);

         var clamav = Card(L("ClamAV (network daemon)"));
         clamav.Settings.Add(new ComBool { Path = "AntiVirus.ClamAVEnabled", Label = L("Scan with clamd") });
         var clamHost = new ComText { Path = "AntiVirus.ClamAVHost", Label = L("Host") };
         var clamPort = new ComText { Path = "AntiVirus.ClamAVPort", Label = L("Port"), Numeric = true };
         clamav.Settings.Add(clamHost);
         clamav.Settings.Add(clamPort);
         clamav.Settings.Add(new ComAction
         {
            ButtonText = L("Test ClamAV connection"),
            Action = () => TestClamAv(clamHost.CurrentText, ParsePort(clamPort.CurrentText))
         });
         // See the SpamAssassin tab: scanner timeouts live with their scanner.
         clamav.Settings.Add(new IniNumber { Path = "ClamMinTimeout", Label = L("Minimum timeout (seconds)"), Default = 15, IniStore = iniStore_ });
         clamav.Settings.Add(new IniNumber
         {
            Path = "ClamMaxTimeout",
            Label = L("Maximum timeout (seconds)"),
            Default = 90,
            Blurb = L("The effective timeout moves between these values with server load."),
            IniStore = iniStore_
         });
         Tab(L("ClamAV")).Cards.Add(clamav);

         var clamwin = Card(L("ClamWin (local executable)"));
         clamwin.Settings.Add(new ComBool { Path = "AntiVirus.ClamWinEnabled", Label = L("Scan with ClamWin") });
         var clamWinExe = new ComText { Path = "AntiVirus.ClamWinExecutable", Label = L("clamscan.exe path"), BrowseFile = true, FileFilter = "Programs (*.exe)|*.exe|All files (*.*)|*.*" };
         var clamWinDb = new ComText { Path = "AntiVirus.ClamWinDBFolder", Label = L("Database folder"), BrowseFolder = true };
         clamwin.Settings.Add(clamWinExe);
         clamwin.Settings.Add(clamWinDb);
         clamwin.Settings.Add(new ComAction
         {
            ButtonText = L("Auto-detect ClamWin"),
            Action = () => AutoDetectClamWin(clamWinExe, clamWinDb)
         });
         clamwin.Settings.Add(new ComAction
         {
            ButtonText = L("Test ClamWin scanner"),
            Action = () => TestClamWin(clamWinExe.CurrentText, clamWinDb.CurrentText)
         });
         Tab(L("ClamWin")).Cards.Add(clamwin);

         var custom = Card(L("Custom scanner"), L("Run an external command; a configured return value indicates an infected message. Use %FILE% where the file to scan should be passed on the command line. For engines without a CLI (HTTP/API, DLP, SIEM), use an OnAcceptMessage handler on the Event scripts page instead."));
         custom.Settings.Add(new ComBool { Path = "AntiVirus.CustomScannerEnabled", Label = L("Use a custom virus scanner") });
         var customExe = new ComText { Path = "AntiVirus.CustomScannerExecutable", Label = L("Command line (use %FILE% for the scanned file)") };
         var customReturn = new ComText { Path = "AntiVirus.CustomScannerReturnValue", Label = L("Return value for infected"), Numeric = true };
         custom.Settings.Add(new ComPreset
         {
            Label = L("Preset engine (fills the command line and return value below \u2013 adjust the path/exit code for your version)"),
            ExeTarget = customExe,
            ReturnTarget = customReturn,
            Presets = CustomScannerPresets
         });
         custom.Settings.Add(customExe);
         custom.Settings.Add(customReturn);
         custom.Settings.Add(new ComAction
         {
            ButtonText = L("Test custom scanner"),
            Action = () => TestCustomScanner(customExe.CurrentText)
         });
         Tab(L("Custom")).Cards.Add(custom);

         // Attachments opened somewhere safe (docs/Sandbox.md). Inert until an
         // administrator names a sandbox, which is a service outside this server,
         // so the card says so; the setup checklist says it too. The two addresses
         // come BEFORE the switch, because a save writes the rows in this order
         // and the server refuses the switch on while an address is empty:
         // setting both and turning it on in one save has to write the addresses
         // first. Turning it off and clearing them works the other way round,
         // since an address emptied while the switch is on is accepted and holds
         // nothing until the switch goes off after it.
         var sandbox = Card(L("Attachments opened in a sandbox"),
            L("A file of a submitted type in a message from outside is sent to a sandbox you run or subscribe to, and the message waits - in no mailbox and not in the quarantine - for its verdict: clean is delivered, malicious is quarantined. Nothing is held until both addresses are set and this is on."));
         sandbox.Settings.Add(new ComText { Path = "SandboxSubmitUrl", Label = L("Submit address (POST)") });
         sandbox.Settings.Add(new ComText { Path = "SandboxVerdictUrl", Label = L("Verdict address (GET)") });
         sandbox.Settings.Add(new ComBool { Path = "SandboxEnabled", Label = L("Hold attachments for the sandbox's verdict") });
         sandbox.Settings.Add(new ComPassword { Path = "SetSandboxToken", MethodName = "SetSandboxToken", Label = L("Bearer token (leave empty to keep current)") });

         // An empty box keeps the token, as it keeps the relayer password; the
         // token is the one secret here whose absence is a state of its own (no
         // Authorization header at all), so removing it is a button of its own.
         sandbox.Settings.Add(new ComAction
         {
            ButtonText = L("Remove the token"),
            Action = () =>
            {
               if (!Dialogs.Confirm(L("Remove the sandbox's bearer token? Requests to the sandbox are then sent without one."), L("Sandbox"), L("_Remove"), destructive: true))
                  return (true, L("Nothing was changed."));

               dynamic settings = ServerSession.Current.Application.Settings;
               try
               {
                  settings.SetSandboxToken("");
                  return (true, L("The token was removed; requests to the sandbox now carry none."));
               }
               finally
               {
                  ServerSession.Release((object)settings);
               }
            }
         });

         sandbox.Settings.Add(new ComText { Path = "SandboxTypes", Label = L("File types submitted (extensions and media types, comma separated)") });
         sandbox.Settings.Add(new ComText { Path = "SandboxMaxSizeKB", Label = L("Largest attachment submitted (KB)"), Numeric = true });
         sandbox.Settings.Add(new ComText { Path = "SandboxMaxFiles", Label = L("Most files one message may submit"), Numeric = true });
         sandbox.Settings.Add(new ComText { Path = "SandboxTimeoutMinutes", Label = L("Minutes a message waits for a verdict"), Numeric = true });
         sandbox.Settings.Add(new ComCombo { Path = "SandboxTimeoutAction", Label = L("When the wait ends or the sandbox answers error"), Options = SandboxTimeoutAction });
         sandbox.Settings.Add(new ComText { Path = "SandboxPollSeconds", Label = L("Seconds between verdict checks"), Numeric = true });
         sandbox.Settings.Add(new ComText { Path = "SandboxScoreThreshold", Label = L("Score at which suspicious counts as malicious (0-100)"), Numeric = true });
         sandbox.Settings.Add(new ComText { Path = "SandboxCacheHours", Label = L("Hours a verdict is remembered (0 = never)"), Numeric = true });
         sandbox.Settings.Add(new ComBool { Path = "SandboxInternalMail", Label = L("Also hold what this server's own signed-in users send, and upload its files") });
         Tab(L("Sandbox")).Cards.Add(sandbox);
      }

      private void BuildTls()
      {
         Header.Title = L("SSL/TLS");
         Header.Subtitle = L("Which TLS versions and ciphers this server negotiates, for its own listeners and for the connections it makes when delivering. Certificates are on the SSL certificates page; DANE and MTA-STS are on Transport security; brute-force lockout is on Auto-ban.");

         var ver = Card(L("Protocol versions"), L("TLS 1.2 and 1.3 are the recommended baseline; older versions exist only for legacy clients."));
         var tls10 = new ComBool { Path = "TlsVersion10Enabled", Label = L("TLS 1.0 (legacy)") };
         var tls11 = new ComBool { Path = "TlsVersion11Enabled", Label = L("TLS 1.1 (legacy)") };
         var tls12 = new ComBool { Path = "TlsVersion12Enabled", Label = L("TLS 1.2") };
         var tls13 = new ComBool { Path = "TlsVersion13Enabled", Label = L("TLS 1.3") };
         ver.Settings.Add(tls10);
         ver.Settings.Add(tls11);
         ver.Settings.Add(tls12);
         ver.Settings.Add(tls13);
         Tab(L("Protocol versions")).Cards.Add(ver);

         var ciph = Card(L("Ciphers & verification"));
         ciph.Settings.Add(new IniText
         {
            Path = "TlsKeyExchangeGroups",
            Label = L("Key exchange groups, in order of preference (empty = the classical fallback secp384r1:x25519:secp256r1)"),
            Blurb = L("The groups this server will agree a TLS key with, in OpenSSL's list form. As shipped it offers the two post-quantum hybrids first and falls back to X25519 and the NIST curves for a peer that has neither, so nothing changes for an older client. An empty list, or one OpenSSL rejects or that would leave no group enabled, is not applied: the server logs it and uses the classical fallback, so a mistake here cannot take TLS down - but it does silently give up the hybrids, which is why the shipped list is shown rather than a blank. Change it against a scan rather than in the dark."),
            // What the server uses when the key is absent - shown, so that saving
            // the page writes the truth back rather than a blank that would drop
            // the post-quantum hybrids to the classical fallback.
            Default = "X25519MLKEM768:SecP256r1MLKEM768:X25519:secp384r1:secp256r1",
            IniStore = iniStore_
         });
         var preferServer = new ComBool { Path = "TlsOptionPreferServerCiphersEnabled", Label = L("Prefer server cipher order") };
         var chacha = new ComBool { Path = "TlsOptionPrioritizeChaChaEnabled", Label = L("Prioritize ChaCha20-Poly1305 when the client prefers it (needs TLS 1.2 or 1.3)") };
         // Labelled "TLS 1.2 and below" rather than just "Cipher list", because that is
         // what it governs and the old label implied otherwise. OpenSSL keeps the TLS
         // 1.3 suites in a separate list that SSL_CTX_set_cipher_list does not touch, so
         // removing a cipher here and confirming it with a TLS 1.2 scan proved nothing
         // about the version most clients actually negotiate.
         ciph.Settings.Add(new ComText
         {
            Path = "SslCipherList",
            Label = L("Cipher list for TLS 1.2 and below (OpenSSL format)"),
            Blurb = L("The single value AEAD-ONLY (case-insensitive) is a named preset rather than an OpenSSL cipher string: forward-secret AEAD suites only (ECDHE or DHE with AES-GCM or ChaCha20-Poly1305), excluding every CBC-mode suite - the Lucky13 padding-oracle family - and static-RSA key exchange. The cost: TLS 1.2 clients that only speak CBC suites cannot connect, and TLS 1.0/1.1 are left with no usable cipher at all, so do not enable those protocols alongside it. TLS 1.3 is unaffected - its suites are AEAD by construction and are configured separately below. A misspelled preset name is rejected and reported, not silently ignored.")
         });
         ciph.Settings.Add(new IniText
         {
            Path = "TlsCipherSuites13",
            Label = L("TLS 1.3 cipher suites (empty = OpenSSL defaults)"),
            Placeholder = "TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256:TLS_AES_128_GCM_SHA256",
            Blurb = L("TLS 1.3 has its own suite list, its own names and its own setter, so the cipher list above does not restrict it. Colon separated, most preferred first, using the RFC 8446 names (TLS_AES_256_GCM_SHA384, TLS_CHACHA20_POLY1305_SHA256, TLS_AES_128_GCM_SHA256, TLS_AES_128_CCM_SHA256, TLS_AES_128_CCM_8_SHA256) - not the OpenSSL cipher-list names. Leave empty to keep OpenSSL's defaults; a name this build does not recognise is skipped and reported rather than failing the whole list."),
            IniStore = iniStore_
         });
         ciph.Settings.Add(preferServer);
         ciph.Settings.Add(chacha);
         ciph.Settings.Add(new ComBool { Path = "VerifyRemoteSslCertificate", Label = L("Verify remote certificates when delivering") });
         Tab(L("Ciphers")).Cards.Add(ciph);

         // Resumption governs every SSL/TLS and STARTTLS listener this server runs,
         // so it belongs on the TLS page and nowhere else; it sat on the catch-all
         // INI page only because that is where it is stored. Every claim below is
         // from SslContextInitializer::SetSessionResumption_, which follows the
         // house rule that a default value makes no OpenSSL call at all - so the
         // card can honestly promise that the defaults change nothing.
         var resume = Card(L("Session resumption"),
            L("Session caching and session tickets let a returning client skip the full handshake. At the defaults these four settings make no OpenSSL call at all, so resumption keeps OpenSSL's stock behaviour - they are here for administrators who need to bound how long a resumption secret stays useful."));
         resume.Settings.Add(new IniBool
         {
            Path = "TlsSessionTicketsEnabled",
            Label = L("Issue TLS session tickets so clients can resume sessions"),
            Default = true,
            Blurb = L("Turning this off stops session tickets on every TLS version: TLS 1.2 and older clients fall back to the server-side session cache, which never leaves this process, and TLS 1.3 clients are sent no ticket at all. For an administrator whose policy is that nothing derived from a long-lived key ever goes on the wire. The only cost is resumption efficiency - every client can still connect with a full handshake."),
            IniStore = iniStore_
         });
         resume.Settings.Add(new IniNumber
         {
            Path = "TlsSessionCacheSize",
            Label = L("Server-side session cache size (0 = OpenSSL's default cap of 20480; negative = no cache)"),
            Default = 0,
            // The one numeric INI setting on any page where a negative value is
            // meaningful rather than a mistake, which is why IniNumber grew a
            // minimum at all.
            MinimumValue = -1,
            Blurb = L("Each cached session costs server memory, and a client that churns handshakes grows the cache - this cap is the defence. A positive value replaces OpenSSL's default cap of 20480 sessions. -1 turns the server-side cache off entirely, which stops session-ID resumption but leaves tickets - which cost the server no memory - unaffected. 0 leaves OpenSSL alone."),
            IniStore = iniStore_
         });
         resume.Settings.Add(new IniNumber
         {
            Path = "TlsSessionTimeoutSeconds",
            Label = L("Resumption lifetime for cached sessions and tickets, seconds (0 = OpenSSL's default of 300)"),
            Default = 0,
            Blurb = L("How long a session stays resumable, for cached sessions and tickets alike. It bounds how long a leaked resumption secret stays useful: a ticket recorded off the wire, or a session read out of a memory dump, stops working once it expires."),
            IniStore = iniStore_
         });
         resume.Settings.Add(new IniNumber
         {
            Path = "TlsTicketKeyRotationSeconds",
            Label = L("Rotate the session-ticket key every N seconds (0 = OpenSSL's single non-rotating key)"),
            Default = 0,
            Blurb = L("What rotation defends: OpenSSL's default ticket key is generated once, when the listener starts, and is never rotated, so every ticket that listener issues for the life of the process is sealed under the same key - and that key, recovered later, decrypts every ticket ever recorded. With an interval set, a captured ticket is useless after at most two intervals. 86400 is a day. Only meaningful while session tickets are enabled above."),
            IniStore = iniStore_
         });
         Tab(L("Session resumption")).Cards.Add(resume);

         // ChaCha prioritization only takes effect when the server chooses the
         // cipher order and a modern TLS version is enabled. Reflect that
         // dependency live in the UI instead of letting it silently no-op.
         afterBuildUi_ = () => WireChaChaDependency(preferServer, chacha, tls12, tls13);
      }

      /// <summary>
      /// Brute-force lockout, which used to be the third tab of the SSL/TLS page.
      ///
      /// The two subjects shared a page and nothing else: you arrive at one of them
      /// because somebody is guessing passwords and at the other because a client
      /// cannot negotiate a cipher, and whichever you came for, half the page was
      /// noise. The roadmap's words for the old title were "an admission that the
      /// page is a bucket".
      ///
      /// The three notes below are the reason this is worth more than a move. Every
      /// one of them is a way the feature silently does nothing, and none of them
      /// was visible anywhere in the interface:
      ///
      ///   - AccountLogon::RegisterFailedLogin returns immediately when
      ///     GetAutoBanLogonEnabled() is false OR MaxInvalidLogonAttempts is 0, so
      ///     a limit of zero turns auto-ban off with the switch still on;
      ///   - with AutoBanMinutes at 0 it disconnects the client and creates no
      ///     range at all, so nothing is blocked;
      ///   - the "within" value is not a sliding window evaluated at logon time. It
      ///     is how long a failure record survives: RemoveExpiredRecords (every
      ///     minute) deletes failures older than it, and only while auto-ban is
      ///     enabled.
      /// </summary>
      private void BuildAutoBan()
      {
         Header.Title = L("Auto-ban");
         Header.Subtitle = L("Automatic lockout of an address that keeps failing to log on. The ban itself is an expiring IP range, so it is listed on the IP ranges page.");

         var ban = Card(L("Auto-ban"),
            L("Counted per connecting IP address across every protocol that authenticates - SMTP AUTH, POP3, IMAP, ManageSieve and the REST API all feed the same counter. On reaching the limit the server clears that address's counted failures and creates an IP range named \"Auto-ban: <user>\" at priority 100 covering that one address, which expires on its own."));
         ban.Settings.Add(new ComBool
         {
            Path = "AutoBanOnLogonFailure",
            Label = L("Enable auto-ban"),
            Blurb = L("Also turned off by a limit of 0 below, whatever this box says.")
         });
         ban.Settings.Add(new ComText
         {
            Path = "MaxInvalidLogonAttempts",
            Label = L("Max invalid logon attempts"),
            Numeric = true,
            Blurb = L("0 disables auto-ban entirely, even with the box above ticked.")
         });
         ban.Settings.Add(new ComText
         {
            Path = "MaxInvalidLogonAttemptsWithin",
            Label = L("...within (minutes)"),
            Numeric = true,
            Blurb = L("How long a counted failure is kept, rather than a sliding window: a housekeeping pass running once a minute deletes failure records older than this - and only while auto-ban is enabled.")
         });
         ban.Settings.Add(new ComText
         {
            Path = "AutoBanMinutes",
            Label = L("Ban duration (minutes)"),
            Numeric = true,
            Blurb = L("0 means the connection is dropped but no range is created, so the address is not actually banned.")
         });

         // What a ban does below the server. Three INI keys, read where the ban is
         // placed, so a save applies them; the checker in CI (build/check-ini-coverage.py) requires every key the
         // server reads to have an editor here, which is also why they are
         // findable from the palette.
         var below = Card(L("Below the server: the firewall"),
            L("An auto-ban is enforced by the server's own listeners: the address is accepted by the operating system, handed to the server and dropped. These settings push the ban down into the operating system's firewall, hand it to a program of yours, and name the addresses that must never be banned at all."));
         below.Settings.Add(new IniBool
         {
            Path = "AutoBanFirewall",
            Label = L("Also block a banned address in the operating system's firewall"),
            Default = false,
            Blurb = L("Windows: an inbound block rule per banned address in Windows Defender Firewall, TCP only and scoped to the ports this server listens on, in a rule group named hMailServer auto-ban; removed when the ban expires or the IP range is deleted. Linux: the packaged autoban-hook keeps an nftables set with a timeout, and the service needs CAP_NET_ADMIN for it - the hook says exactly how to grant it. List the addresses you administer from under never-ban first."),
            IniStore = iniStore_
         });
         below.Settings.Add(new IniText
         {
            Path = "AutoBanCommand",
            Label = L("Program to run when an address is banned or unbanned (empty = none)"),
            Blurb = L("Run as <command> ban <address> <minutes> <ports> when a ban is created and <command> unban <address> when the range goes, on Windows and on Linux alike - for a cloud firewall, a router or a SIEM. Quote the program's path if it contains spaces; the server appends the arguments. A non-zero exit or a program that cannot start is reported once in the error log."),
            IniStore = iniStore_
         });
         below.Settings.Add(new IniText
         {
            Path = "AutoBanNeverBan",
            Label = L("Never auto-ban these addresses (comma separated; CIDR blocks allowed)"),
            Blurb = L("Failed logons from a listed address are not counted at all, so it can never be banned. Put the addresses you administer from here before turning the firewall option on: a ban that reaches the firewall locks that address out of every mail port on this machine. Example: 192.0.2.10, 203.0.113.0/24, 2001:db8::/32. An entry that is not an address is reported by name in the error log and ignored."),
            IniStore = iniStore_
         });

         ban.Settings.Add(new ComAction
         {
            Path = "AutoBanOnLogonFailure",
            ButtonText = L("Clear logon-failure list"),
            Action = () =>
            {
               dynamic s = ServerSession.Current.Application.Settings;
               try
               {
                  s.ClearLogonFailureList();
                  // Deliberately explicit about what this does NOT do. It calls
                  // Configuration::ClearOldLogonFailures, i.e. ClearOldFailures(-1)
                  // on the failure table; the ban is a separate IP range and is
                  // untouched. Somebody pressing this to release a locked-out user
                  // and getting "cleared" would otherwise reasonably conclude the
                  // lockout was lifted.
                  return (true, L("Counted logon failures cleared. Addresses already banned stay banned until their \"Auto-ban:\" range expires - delete it on the IP ranges page to release one now."));
               }
               finally { ServerSession.Release((object)s); }
            }
         });
         Tab(L("Auto-ban")).Cards.Add(ban);
         Tab(L("Auto-ban")).Cards.Add(below);

         // The other half of the same subject, and the reason it is on this page:
         // auto-ban counts per ADDRESS, so a distributed attack that spends a few
         // guesses per address against one mailbox never trips it. This counts per
         // NAME, which is the one thing such an attack cannot vary. An
         // administrator reading a page called "Auto-ban" is entitled to find both.
         var lockout = Card(L("Per-name lockout"),
            L("Counted per user name rather than per address, across every protocol that authenticates, so a botnet spreading its guesses over thousands of addresses still locks the mailbox it is guessing at. A locked name is refused with the ordinary invalid-credentials reply and cannot be unlocked by typing the right password - the lock is checked first - so the ceiling is worth setting with the cost in mind. Failures against a name that does not exist lock that name too, deliberately: doing otherwise would let an attacker use the lockout to discover which accounts are real."));
         lockout.Settings.Add(new IniNumber
         {
            Path = "AccountLockoutThreshold",
            Label = L("Lock a name after this many failures (0 = off)"),
            Default = 0,
            Blurb = L("0 is the shipped default and disables the whole mechanism."),
            IniStore = iniStore_
         });
         lockout.Settings.Add(new IniNumber
         {
            Path = "AccountLockoutWindowMinutes",
            Label = L("...counted within (minutes)"),
            Default = 30,
            IniStore = iniStore_
         });
         lockout.Settings.Add(new IniNumber
         {
            Path = "AccountLockoutMinutes",
            Label = L("Lockout duration (minutes)"),
            Default = 30,
            Blurb = L("A successful logon clears the name's counters, and attempts made while it is locked are not counted - so an attacker cannot hold a mailbox locked indefinitely with a trickle of guesses."),
            IniStore = iniStore_
         });
         Tab(L("Auto-ban")).Cards.Add(lockout);

         // The third measure on the same subject: auto-ban and the lockout decide
         // WHETHER a guess is answered; this decides HOW SOON, and it applies from
         // the first wrong password rather than after a threshold.
         var tarpit = Card(L("Logon tarpit"),
            L("Holds the refusal of a wrong password for a while before sending it: the first failure on a connection waits this many seconds, the second twice as long, and so on up to 30 seconds, on SMTP, POP3 and IMAP alike. A correct password is never delayed. The wait is a timer on the connection, not a thread asleep, so a flood of failed logons costs the server nothing but the sockets they arrive on - the reason the old TarpitDelay was a stub for a decade is that the naive version is a self-inflicted outage."));
         tarpit.Settings.Add(new IniNumber
         {
            Path = "LogonTarpitSeconds",
            Label = L("Delay per failed logon, multiplied by the failures so far on that connection (seconds; 0 = off)"),
            Default = 0,
            Blurb = L("0 is the shipped default. 2 is a reasonable start: a mistyped password costs a person two seconds once, and a password-spraying client two, four, six... seconds per guess."),
            IniStore = iniStore_
         });
         Tab(L("Auto-ban")).Cards.Add(tarpit);

         // Country blocking (docs/CountryBlocking.md): who may connect at all, by
         // where the address is. A domain's own policy, applied at sign-in, is in the
         // domain's dialog; these are the server's, applied when a connection is
         // accepted. Inert until a database file is named.
         var countries = Card(L("Country blocking"),
            L("Refuse, or allow only, connections from the countries you name, by the country a MaxMind database places the address in. This server ships no database and downloads none: until you supply the file, nothing is judged by country. A policy is empty (off), allow:GB,IE or block:CN,RU."));
         countries.Settings.Add(new ComText
         {
            Path = "GeoIpDatabaseFile",
            Label = L("MaxMind country database (.mmdb)"),
            BrowseFile = true,
            FileFilter = L("MaxMind databases (*.mmdb)|*.mmdb|All files (*.*)|*.*"),
            Blurb = L("Such as GeoLite2-Country.mmdb, kept up to date by you. Empty, the default, judges nothing by country whatever the policies say.")
         });
         countries.Settings.Add(new ComText
         {
            Path = "GeoIpDatabaseMaxAgeDays",
            Label = L("Days before the database is reported as stale (0 = never)"),
            Numeric = true,
            Maximum = 3650,
            Blurb = L("45 by default. A stale database is still used; the application log says it is stale.")
         });
         countries.Settings.Add(new ComText
         {
            Path = "CountryPolicy",
            Label = L("SMTP connections"),
            Index = "smtp", // no-loc
            Placeholder = "block:CN,RU", // no-loc
            Blurb = L("Applied when a connection is accepted, before the greeting. An allow list refuses an address whose country the database does not know; a block list lets it in.")
         });
         countries.Settings.Add(new ComText
         {
            Path = "CountryPolicy",
            Label = L("IMAP connections"),
            Index = "imap", // no-loc
            Placeholder = "block:CN,RU" // no-loc
         });
         countries.Settings.Add(new ComText
         {
            Path = "CountryPolicy",
            Label = L("POP3 connections"),
            Index = "pop3", // no-loc
            Placeholder = "block:CN,RU" // no-loc
         });
         countries.Settings.Add(new ComText
         {
            Path = "CountryPolicy",
            Label = L("Webmail, REST API and web services connections"),
            Index = "http", // no-loc
            Placeholder = "block:CN,RU", // no-loc
            Blurb = L("The REST API's listener and the web services listener share this one.")
         });
         countries.Settings.Add(new ComText
         {
            Path = "CountryBlockingExemptRanges",
            Label = L("Never judged by country (addresses and CIDR ranges, comma separated)"),
            Blurb = L("The loopback address and the private networks by default. Emptying it has your own network judged like anybody else's, which a strict allow list will refuse - the webmail and the Control Deck included.")
         });
         Tab(L("Countries")).Cards.Add(countries);

         // Compromised-account detection (docs/CompromisedAccounts.md): the other
         // half of a stolen password - not the guessing, which auto-ban and the
         // lockout meet, but what an account does once somebody else has it.
         // Kept by the server with no COM property.
         var compromise = Card(L("Compromised accounts"),
            L("Scores each account's submissions for the shape of a takeover: far more mail than its busiest hour, mostly to addresses it has never written to, at an hour it never sends, from a network it never used. Watch raises the account.compromised alert; enforce also holds the message for review, holds the account's other queued mail and locks the account until an administrator unlocks it. Held messages are reviewed in the Control Deck."));
         compromise.Settings.Add(new RestChoice
         {
            Path = "CompromiseDetectionMode",
            Label = L("What a takeover-shaped submission does"),
            Key = "compromise_detection_mode",
            Options = new (int, string)[]
            {
               (0, L("Off: record and judge nothing")),
               (1, L("Watch: raise the alert only (default)")),
               (2, L("Enforce: hold the mail and lock the account"))
            },
            Words = new[] { "off", "watch", "enforce" } // no-loc
         });
         compromise.Settings.Add(new RestNumber
         {
            Path = "CompromiseBaselineDays",
            Label = L("Days of submissions that make an account's baseline (7 to 365)"),
            Key = "compromise_baseline_days",
            Minimum = 7,
            Maximum = 365,
            Blurb = L("30 by default. The record of whom an account wrote to is kept as long.")
         });
         compromise.Settings.Add(new RestNumber
         {
            Path = "CompromiseMinimumHistory",
            Label = L("Submissions before an account's baseline counts (1 to 10000)"),
            Key = "compromise_minimum_history",
            Minimum = 1,
            Maximum = 10000,
            Blurb = L("20 by default. Below it only the two floors are judged.")
         });
         compromise.Settings.Add(new RestNumber
         {
            Path = "CompromiseVolumeFactor",
            Label = L("Volume signal: times the account's busiest hour (2 to 100)"),
            Key = "compromise_volume_factor",
            Minimum = 2,
            Maximum = 100,
            Blurb = L("4 by default. The last hour's messages have to exceed this many times the busiest hour of the baseline, and reach the floor below.")
         });
         compromise.Settings.Add(new RestNumber
         {
            Path = "CompromiseVolumeFloor",
            Label = L("Fewest messages in an hour that can be a volume signal (1 to 100000)"),
            Key = "compromise_volume_floor",
            Minimum = 1,
            Maximum = 100000,
            Blurb = L("30 by default. For an account with no baseline yet, this floor is the whole test.")
         });
         compromise.Settings.Add(new RestNumber
         {
            Path = "CompromiseRecipientFloor",
            Label = L("Fewest recipients in an hour that can be a recipients signal (1 to 100000)"),
            Key = "compromise_recipient_floor",
            Minimum = 1,
            Maximum = 100000,
            Blurb = L("20 by default.")
         });
         compromise.Settings.Add(new RestNumber
         {
            Path = "CompromiseNewRecipientShare",
            Label = L("Recipients signal: percentage never written to before (1 to 100)"),
            Key = "compromise_new_recipient_share",
            Minimum = 1,
            Maximum = 100,
            Blurb = L("80 by default.")
         });
         compromise.Settings.Add(new RestNumber
         {
            Path = "CompromiseScoreToLock",
            Label = L("Score that counts as a takeover (1 to 6)"),
            Key = "compromise_score_to_lock",
            Minimum = 1,
            Maximum = 6,
            Blurb = L("3 by default. Volume and recipients score 2 each; an hour of the day the account has never sent at and a network it has never sent from score 1 each.")
         });
         compromise.Settings.Add(new RestNumber
         {
            Path = "CompromiseHeldDays",
            Label = L("Days a held message waits for review (1 to 90)"),
            Key = "compromise_held_days",
            Minimum = 1,
            Maximum = 90,
            Blurb = L("7 by default. After that the message is discarded.")
         });
         Tab(L("Compromised accounts")).Cards.Add(compromise);
      }

      private void BuildLogging()
      {
         Header.Title = L("Logging");
         Header.Subtitle = L("What the server writes to its log files (viewable on the Live logs page).");

         // One tab holding the three cards: calling Tab() again would add a second
         // tab with the same header rather than reuse this one.
         TabDef logging = Tab(L("Logging"));

         var log = Card(L("Log categories"));
         log.Settings.Add(new ComBool { Path = "Logging.Enabled", Label = L("Logging enabled") });
         log.Settings.Add(new ComBool { Path = "Logging.LogApplication", Label = L("Application events") });
         log.Settings.Add(new ComBool { Path = "Logging.LogSMTP", Label = L("SMTP conversations") });
         log.Settings.Add(new ComBool { Path = "Logging.LogIMAP", Label = L("IMAP conversations") });
         log.Settings.Add(new ComBool { Path = "Logging.LogPOP3", Label = L("POP3 conversations") });
         log.Settings.Add(new ComBool { Path = "Logging.LogTCPIP", Label = L("TCP/IP activity") });
         log.Settings.Add(new ComBool { Path = "Logging.LogDebug", Label = L("Debug messages") });
         log.Settings.Add(new ComBool { Path = "Logging.AWStatsEnabled", Label = L("AWStats-compatible log") });
         log.Settings.Add(new ComBool { Path = "Logging.KeepFilesOpen", Label = L("Keep log files open (performance)") });
         log.Settings.Add(new ComCombo
         {
            Path = "Logging.Device",
            Label = L("Log destination"),
            Options = ToOptions(SettingClaims.LogDeviceOptions),
            Blurb = L(SettingClaims.NoteFor("Logging.Device"))
         });

         // The two log-rendering controls are wired together below, because the
         // server has a precedence between them that nothing in the interface used
         // to show: Logger::Render_ tests the line-format setting first, so choosing
         // NCSA makes the JSON switch inert. Two ways of formatting a log line on
         // one card, one of which silently wins, is the same defect class as a
         // setting that does nothing at all.
         //
         // The option wording itself lives in SettingClaims, and the claim the old
         // wording made is pinned there by a test that scans this file for it.
         var format = new ComCombo
         {
            Path = "Logging.LogFormat",
            Label = L("Log line format"),
            Options = ToOptions(SettingClaims.LogFormatOptions),
            Blurb = L(SettingClaims.NoteFor("Logging.LogFormat"))
         };
         var json = new IniBool
         {
            Path = "JsonLogging",
            Label = L("Write logs as JSON lines"),
            Default = false,
            Blurb = L("Machine-readable output for log shippers."),
            IniStore = iniStore_
         };
         log.Settings.Add(format);
         log.Settings.Add(json);
         afterBuildUi_ = () => WireLogFormatDependency(format, json);
         logging.Cards.Add(log);

         // syslog, next to the destination it is an alternative reading of. It is
         // the first question a Linux administrator asks about any daemon, and the
         // answer until now was "log files, or an OTLP collector" - neither of
         // which is what their existing collector reads.
         var syslog = Card(L("syslog (RFC 5424)"),
            L("A copy of every log entry, as RFC 5424, to a syslog collector - rsyslog, syslog-ng, a SIEM - beside the log files rather than instead of them. On Linux with no collector named, a server running under systemd writes to the journal, where journalctl can read it."));

         syslog.Settings.Add(new ComBool
         {
            Path = "Logging.SyslogEnabled",
            Label = L("Send log entries to syslog"),
            Blurb = L(SettingClaims.NoteFor("Logging.SyslogEnabled"))
         });
         syslog.Settings.Add(new ComText
         {
            Path = "Logging.SyslogHost",
            Label = L("Collector host name or address"),
            Blurb = L(SettingClaims.NoteFor("Logging.SyslogHost"))
         });
         syslog.Settings.Add(new ComText
         {
            Path = "Logging.SyslogPort",
            Label = L("Collector port"),
            Numeric = true,
            Blurb = L("514 is the registered port for UDP and for plain TCP; 6514 is the registered port for syslog over TLS.")
         });
         syslog.Settings.Add(new ComCombo
         {
            Path = "Logging.SyslogTransport",
            Label = L("How entries reach the collector"),
            Options = ToOptions(SettingClaims.SyslogTransportOptions),
            Blurb = L(SettingClaims.NoteFor("Logging.SyslogTransport"))
         });
         syslog.Settings.Add(new ComText
         {
            Path = "Logging.SyslogFacility",
            Label = L("Facility (0 to 23)"),
            Numeric = true,
            Blurb = L("2 is mail, which is where a mail server's lines belong and what a collector's mail.* rule already matches. 16 to 23 are local0 to local7, for a site that files its own applications there. 0 is kernel and should not be used.")
         });
         syslog.Settings.Add(new ComCombo
         {
            Path = "Logging.SyslogMinimumSeverity",
            Label = L("Send entries at least this severe"),
            Options = ToOptions(SettingClaims.SyslogSeverityOptions),
            Blurb = L("Error entries carry the severity the server itself recorded: critical, error, warning or notice. Everything else is informational, except TCP/IP and debug, which are debug.")
         });

         // Which categories are sent, as switches rather than as a number, for the
         // same reason the log mask above is: it is one stored integer either way,
         // and nobody should have to add 2, 4, 16 and 64 to configure a log.
         // Errors are not on the list on purpose - the error log is written
         // whatever the log mask says, and this mirrors that.
         // Worded "... to syslog" rather than repeating the six captions of the
         // Log categories card above. Two rows on one page with the same caption
         // read identically in the Ctrl+K palette, which lists a setting by its
         // label and its page and not by its card - and the palette is how most
         // administrators find a setting at all.
         syslog.Settings.Add(new ComBool { Path = "Logging.SyslogLogSMTP", Label = L("SMTP conversations to syslog") });
         syslog.Settings.Add(new ComBool { Path = "Logging.SyslogLogIMAP", Label = L("IMAP conversations to syslog") });
         syslog.Settings.Add(new ComBool { Path = "Logging.SyslogLogPOP3", Label = L("POP3 conversations to syslog") });
         syslog.Settings.Add(new ComBool { Path = "Logging.SyslogLogApplication", Label = L("Application events to syslog") });
         syslog.Settings.Add(new ComBool { Path = "Logging.SyslogLogTCPIP", Label = L("TCP/IP activity to syslog") });
         syslog.Settings.Add(new ComBool
         {
            Path = "Logging.SyslogLogDebug",
            Label = L("Debug messages to syslog"),
            Blurb = L("Errors are always sent, whichever of these are off - the error log is written even when logging is disabled, and this follows it.")
         });

         logging.Cards.Add(syslog);

         // How much is written, next to what is written and how long it is kept -
         // an admin dealing with log volume should not have to find three pages.
         var detail = Card(L("Log detail"),
            L("How much detail each entry carries. Lowering the level is the other half of controlling log volume."));
         detail.Settings.Add(new IniNumber
         {
            Path = "LogLevel",
            Label = L("Log level (3 or above = full detail; 2 or lower = quieter)"),
            Default = 9,
            Blurb = L("There is only one step, at 2. From 3 upwards (the default is 9) every enabled category is logged in full. At 2 or lower the server drops IMAP FETCH and STATUS responses from the log and shortens lines longer than the limit below to their first and last characters. Turning on Debug messages above restores full detail whatever the level."),
            IniStore = iniStore_
         });
         detail.Settings.Add(new IniNumber
         {
            Path = "MaxLogLineLen",
            Label = L("Maximum characters per log line (minimum 100; only applied at log level 2 or lower)"),
            Default = 500,
            IniStore = iniStore_
         });
         detail.Settings.Add(new IniBool
         {
            Path = "SepSvcLogs",
            Label = L("Write a separate log file per service component"),
            Default = false,
            IniStore = iniStore_
         });
         logging.Cards.Add(detail);

         var retention = Card(L("Log retention"),
            L("Housekeeping for the log folder, so logs do not accumulate indefinitely."));
         retention.Settings.Add(new IniNumber
         {
            Path = "LogDeleteDays",
            Label = L("Delete logs older than N days (0 = keep everything)"),
            Default = 0,
            Blurb = L("Checked shortly after the service starts and every six hours after that. Only hMailServer's own dated log files (hmailserver_*.log and ERROR_hmailserver_*.log) are removed; nothing else in the log folder is touched."),
            IniStore = iniStore_
         });
         logging.Cards.Add(retention);

         // Beside log retention rather than in a section of its own: the two answer the
         // same operational question - how much history is kept, and for how long.
         var trace = Card(L("Message trace"),
            L("Records a queryable row for every delivery, refusal and quarantine, so \"what happened to the message Jane sent at 14:20\" can be answered by searching instead of grepping logs. The events are the same ones the AWStats journal has always seen; this gives them somewhere to be asked questions. Off by default, because it stores sender and recipient addresses - which makes it a record of who corresponds with whom."));
         trace.Settings.Add(new IniBool
         {
            Path = "MessageTraceEnabled",
            Label = L("Record a queryable message trace"),
            Blurb = L("Independent of the AWStats log above - either can be on without the other."),
            IniStore = iniStore_
         });
         trace.Settings.Add(new IniNumber
         {
            Path = "MessageTraceRetentionDays",
            Label = L("Delete trace events after (days; 0 = never)"),
            Default = 30,
            Blurb = L("For a record of who corresponds with whom, keeping it for ever should be a decision rather than a default - which is why 0 is offered as an option instead of being one."),
            IniStore = iniStore_
         });
         logging.Cards.Add(trace);

         // The classic Administrator's Logging pane showed the log directory and gave
         // you a button to open it. The Control Panel had neither, so the first
         // question anybody asks on this page - "where are these files?" - could only
         // be answered by opening hMailServer.INI by hand, and the live-logs page
         // shows contents rather than a location.
         //
         // The path is read rather than assumed, and the blurb says so when it cannot
         // be: the Control Panel can be run from a different machine to the server, in
         // which case there is no INI to read and a button that opened "the log folder"
         // would open the wrong machine's idea of one.
         string logFolder = iniStore_ != null && iniStore_.IsAvailable
            ? iniStore_.GetLogFolder()
            : null;

         var logFiles = Card(L("Log files"),
            L("Where the server writes the logs configured above."));

         logFiles.Settings.Add(new ComAction
         {
            Label = L("Log folder"),
            ButtonText = L("Open log folder"),
            Blurb = string.IsNullOrWhiteSpace(logFolder)
               ? L("Read from [Directories] LogFolder in hMailServer.INI, which is not readable from this machine - the Control Panel is running somewhere other than the server.")
               : logFolder,
            Action = () => OpenLogFolder(logFolder)
         });

         logging.Cards.Add(logFiles);

         // The audit trail (docs/AlertsAndAuditTrail.md): read on the Audit trail
         // page, switched and kept here. Kept by the server with no COM property.
         var audit = Card(L("Audit trail"),
            L("Every administrative change - from this Control Panel, the Control Deck, the REST API or a script - recorded in a hash-chained trail, so that a row removed or altered afterwards shows. It is read on the Audit trail page."));
         audit.Settings.Add(new RestBool
         {
            Path = "AuditTrailEnabled",
            Label = L("Record administrative changes in the audit trail"),
            Key = "audit_trail_enabled",
            Blurb = L("On by default.")
         });
         audit.Settings.Add(new RestNumber
         {
            Path = "AuditRetentionDays",
            Label = L("Days of audit trail to keep (0 = everything)"),
            Key = "audit_retention_days",
            Blurb = L("0, keep everything, by default. Removing the oldest rows starts the chain again at the oldest row kept.")
         });
         Tab(L("Audit trail")).Cards.Add(audit);

         // Alerts (docs/AlertsAndAuditTrail.md): the conditions themselves are the
         // alert rules, which are the Control Deck's; the switch, where they go and
         // how many are the server's settings, kept with no COM property.
         var alerts = Card(L("Alerts"),
            L("Mail to an administrator when something needs attention, such as the disk filling, a work queue stalling, a certificate about to expire or a backup that failed. Which conditions are watched is set by the alert rules in the Control Deck; nothing is mailed until both addresses below are set."));
         alerts.Settings.Add(new RestBool
         {
            Path = "AlertsEnabled",
            Label = L("Watch for alert conditions"),
            Key = "alerts_enabled",
            Blurb = L("On by default.")
         });
         alerts.Settings.Add(new RestText
         {
            Path = "AlertRecipient",
            Label = L("Send alerts and the daily digest to"),
            Key = "alert_recipient",
            Placeholder = "postmaster@example.com", // no-loc
            Blurb = L("Empty, the default, mails nothing.")
         });
         alerts.Settings.Add(new RestText
         {
            Path = "AlertSenderAddress",
            Label = L("Send alerts from"),
            Key = "alert_sender_address",
            Placeholder = "hmailserver@example.com", // no-loc
            Blurb = L("Empty, the default, mails nothing.")
         });
         alerts.Settings.Add(new RestNumber
         {
            Path = "AlertMaxPerHour",
            Label = L("Alerts sent one at a time in an hour, at most"),
            Key = "alert_max_per_hour",
            Minimum = 1,
            Blurb = L("20 by default. Past it the rest wait for the digest.")
         });
         alerts.Settings.Add(new RestBool
         {
            Path = "AlertDigestEnabled",
            Label = L("Send the conditions marked for the digest in one daily message"),
            Key = "alert_digest_enabled",
            Blurb = L("On by default.")
         });
         alerts.Settings.Add(new RestNumber
         {
            Path = "AlertDigestHour",
            Label = L("Hour the digest is sent (UTC, 0 to 23)"),
            Key = "alert_digest_hour",
            Maximum = 23,
            Blurb = L("7 by default.")
         });
         alerts.Settings.Add(new RestNumber
         {
            Path = "AlertWebhookMaxAttempts",
            Label = L("Attempts at an alert's webhook before it is given up (1 to 20)"),
            Key = "alert_webhook_max_attempts",
            Minimum = 1,
            Maximum = 20,
            Blurb = L("5 by default.")
         });
         Tab(L("Alerts & webhooks")).Cards.Add(alerts);

         // Outbound webhooks (docs/Webhooks.md): the subscriptions are the Control
         // Deck's and the REST API's; these four are the server's.
         var webhooks = Card(L("Outbound webhooks"),
            L("Events - a message received, delivered, bounced or quarantined, an account locked - posted to the receivers subscribed in the Control Deck or over the REST API. Nothing is sent until a subscription exists."));
         webhooks.Settings.Add(new ComBool
         {
            Path = "WebhooksEnabled",
            Label = L("Send webhook events"),
            Blurb = L("On by default. Off silences every receiver at once and keeps the subscriptions; inbound webhooks are not affected.")
         });
         webhooks.Settings.Add(new ComText
         {
            Path = "WebhookMaxAttempts",
            Label = L("Attempts before a delivery is dead-lettered (1 to 20)"),
            Numeric = true,
            Minimum = 1,
            Maximum = 20,
            Blurb = L("5 by default. The waits between them are none, then 1, 5, 15 and 60 minutes.")
         });
         webhooks.Settings.Add(new ComText
         {
            Path = "WebhookMaxPerMinute",
            Label = L("Events a minute for a subscription with no limit of its own (1 to 100000)"),
            Numeric = true,
            Minimum = 1,
            Maximum = 100000,
            Blurb = L("60 by default. Events past it are dropped and counted, not queued.")
         });
         webhooks.Settings.Add(new ComText
         {
            Path = "WebhookRetentionDays",
            Label = L("Days delivered and dead-lettered deliveries are kept (0 = everything; up to 3650)"),
            Numeric = true,
            Maximum = 3650,
            Blurb = L("7 by default.")
         });
         Tab(L("Alerts & webhooks")).Cards.Add(webhooks);

         // The SNMP agent (docs/Snmp.md). Read when the agent starts, so each is
         // marked as waiting for a restart; kept by the server with no COM property.
         var snmp = Card(L("SNMP agent"),
            L("A read-only SNMP agent for a monitoring system: SNMPv3 with authentication and encryption, and SNMPv2c only if you switch it on. It answers nobody until the managers are listed, and the port has to be open to them in any firewall between."));
         snmp.Settings.Add(new RestBool
         {
            Path = "SnmpEnabled",
            Label = L("Run the SNMP agent"),
            Key = "snmp_enabled",
            AppliesMark = SettingApplies.Restart,
            Blurb = L("Off by default.")
         });
         snmp.Settings.Add(new RestNumber
         {
            Path = "SnmpPort",
            Label = L("UDP port (1 to 65535)"),
            Key = "snmp_port",
            Minimum = 1,
            Maximum = 65535,
            AppliesMark = SettingApplies.Restart,
            Blurb = L("161 by default.")
         });
         snmp.Settings.Add(new RestText
         {
            Path = "SnmpBindAddress",
            Label = L("Listen on this address (empty = every address)"),
            Key = "snmp_bind_address",
            Placeholder = "127.0.0.1", // no-loc
            AppliesMark = SettingApplies.Restart,
            Blurb = L("One address of this machine: 127.0.0.1 when the manager runs here, otherwise an address on a management network.")
         });
         snmp.Settings.Add(new RestText
         {
            Path = "SnmpAllowedAddresses",
            Label = L("Managers allowed to ask (addresses or CIDR ranges, comma separated)"),
            Key = "snmp_allowed_addresses",
            Placeholder = "192.0.2.10, 192.0.2.11", // no-loc
            AppliesMark = SettingApplies.Restart,
            Blurb = L("Empty, the default, answers nobody. Name the managers one by one: UDP does not prove where a request came from.")
         });
         snmp.Settings.Add(new RestText
         {
            Path = "SnmpV3User",
            Label = L("SNMPv3 user name (empty = none)"),
            Key = "snmp_v3_user",
            AppliesMark = SettingApplies.Restart,
            Blurb = L("Up to 32 visible ASCII characters. Authentication is HMAC-SHA-256 and privacy AES-128.")
         });
         snmp.Settings.Add(new RestSecret
         {
            Path = "SnmpV3AuthPassword",
            Label = L("SNMPv3 authentication password (leave empty to keep current)"),
            Key = "snmp_v3_auth_password",
            AppliesMark = SettingApplies.Restart,
            Blurb = L("8 to 64 ASCII characters.")
         });
         snmp.Settings.Add(new RestSecret
         {
            Path = "SnmpV3PrivPassword",
            Label = L("SNMPv3 privacy password (leave empty to keep current)"),
            Key = "snmp_v3_priv_password",
            AppliesMark = SettingApplies.Restart,
            Blurb = L("8 to 64 ASCII characters.")
         });
         snmp.Settings.Add(new RestBool
         {
            Path = "SnmpV2cEnabled",
            Label = L("Also answer SNMPv2c"),
            Key = "snmp_v2c_enabled",
            AppliesMark = SettingApplies.Restart,
            Blurb = L("Off by default. Its community crosses the network in the clear, so anyone who can see the traffic can read what the agent serves.")
         });
         snmp.Settings.Add(new RestSecret
         {
            Path = "SnmpV2cCommunity",
            Label = L("SNMPv2c community (leave empty to keep current)"),
            Key = "snmp_v2c_community",
            AppliesMark = SettingApplies.Restart,
            Blurb = L("Up to 64 visible ASCII characters.")
         });
         snmp.Settings.Add(new RestText
         {
            Path = "SnmpContact",
            Label = L("Contact (sysContact)"),
            Key = "snmp_contact",
            AppliesMark = SettingApplies.Restart,
            Blurb = L("Who to ask about this server: one line of printable ASCII, up to 255 characters.")
         });
         snmp.Settings.Add(new RestText
         {
            Path = "SnmpLocation",
            Label = L("Location (sysLocation)"),
            Key = "snmp_location",
            AppliesMark = SettingApplies.Restart,
            Blurb = L("Where this server is: one line of printable ASCII, up to 255 characters.")
         });
         Tab(L("SNMP")).Cards.Add(snmp);
      }

      /// <summary>
      /// Opens the log folder in Explorer, and says why it could not rather than
      /// failing silently - the two ordinary reasons are a Control Panel running away
      /// from the server, and a LogFolder that names a directory which is not there.
      /// </summary>
      private static (bool ok, string text) OpenLogFolder(string path)
      {
         if (string.IsNullOrWhiteSpace(path))
            return (false, L("No log folder could be read from hMailServer.INI on this machine."));

         if (!System.IO.Directory.Exists(path))
            return (false, F("The configured log folder does not exist on this machine: {0}", path));

         try
         {
            using var process = System.Diagnostics.Process.Start(
               new System.Diagnostics.ProcessStartInfo(path) { UseShellExecute = true });

            return (true, F("Opened {0}", path));
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            return (false, F("Could not open the folder: {0}", ex.Message));
         }
      }

      private void BuildPerformance()
      {
         Header.Title = L("Performance");
         Header.Subtitle = L("Thread pools, in-memory caches and message indexing.");

         var threads = Card(L("Threads"), L("Thread pool sizing. Defaults suit most installations; raise for very busy servers."));
         threads.Settings.Add(new ComText { Path = "MaxDeliveryThreads", Label = L("Max delivery threads"), Numeric = true });
         threads.Settings.Add(new ComText { Path = "MaxAsynchronousThreads", Label = L("Max asynchronous task threads"), Numeric = true });
         threads.Settings.Add(new ComText { Path = "TCPIPThreads", Label = L("TCP/IP threads"), Numeric = true });
         // The fourth thread pool, and the only one that was not on this card:
         // it is an INI value rather than a COM one, which is not a reason for an
         // administrator sizing thread pools to have to look somewhere else.
         threads.Settings.Add(new IniNumber
         {
            Path = "MaxNumberOfExternalFetchThreads",
            Label = L("Max parallel external POP3 fetch threads"),
            Default = 15,
            Blurb = L("How many external accounts, configured under a domain's accounts as external POP3 downloads, are collected at the same time. Each one holds a connection to somebody else's server for as long as the download takes."),
            IniStore = iniStore_
         });

         // Not a NumberBox any more. The server writes this value to the settings
         // table and reads it back only through the COM getter: there is no
         // SetThreadPriority call anywhere in the tree, so nothing has ever acted on
         // it. The label says so as well as the note, because the label is what the
         // Ctrl+K palette shows.
         threads.Settings.Add(new ComInert
         {
            Path = "WorkerThreadPriority",
            Label = L("Worker thread priority (stored, but the server does not use it)"),
            Blurb = L(SettingClaims.NoteFor("WorkerThreadPriority"))
         });
         Tab(L("Threads")).Cards.Add(threads);

         var cache = Card(L("Cache"), L("Caches domain/account/alias lookups in memory to reduce database round-trips. TTL in seconds."));
         cache.Settings.Add(new ComBool { Path = "Cache.Enabled", Label = L("Enable caching") });
         cache.Settings.Add(new ComText { Path = "Cache.DomainCacheTTL", Label = L("Domain cache TTL (seconds)"), Numeric = true });
         cache.Settings.Add(new ComText { Path = "Cache.AccountCacheTTL", Label = L("Account cache TTL (seconds)"), Numeric = true });
         cache.Settings.Add(new ComText { Path = "Cache.AliasCacheTTL", Label = L("Alias cache TTL (seconds)"), Numeric = true });
         cache.Settings.Add(new ComText { Path = "Cache.DistributionListCacheTTL", Label = L("Distribution-list cache TTL (seconds)"), Numeric = true });
         // The four max-size limits are the one thing on this card that does NOT
         // survive a restart. Verified in InterfaceCache/CacheContainer: the
         // setters adjust the live Cache<T> and nothing else - no property row,
         // no OnPropertyChanged arm - and the constructor hard-codes 10 MB at
         // startup. Kept editable because the server honours the value
         // immediately and the getter reads the live value back; labelled and
         // noted as session-only because the label is what the Ctrl+K palette
         // shows, and an administrator must be told before they type, not after
         // the next restart quietly discards it.
         cache.Settings.Add(new ComText { Path = "Cache.DomainCacheMaxSizeKb", Label = L("Domain cache max size (KB, resets at service restart)"), Numeric = true, Blurb = L(SettingClaims.NoteFor("Cache.DomainCacheMaxSizeKb")) });
         cache.Settings.Add(new ComText { Path = "Cache.AccountCacheMaxSizeKb", Label = L("Account cache max size (KB, resets at service restart)"), Numeric = true, Blurb = L(SettingClaims.NoteFor("Cache.AccountCacheMaxSizeKb")) });
         cache.Settings.Add(new ComText { Path = "Cache.AliasCacheMaxSizeKb", Label = L("Alias cache max size (KB, resets at service restart)"), Numeric = true, Blurb = L(SettingClaims.NoteFor("Cache.AliasCacheMaxSizeKb")) });
         cache.Settings.Add(new ComText { Path = "Cache.DistributionListCacheMaxSizeKb", Label = L("Distribution-list cache max size (KB, resets at service restart)"), Numeric = true, Blurb = L(SettingClaims.NoteFor("Cache.DistributionListCacheMaxSizeKb")) });
         Tab(L("Cache")).Cards.Add(cache);

         // Four TTLs and four size caps, and until now no way to see what any of
         // them achieved. The server has counted hits and misses per cache all
         // along and exposes the rate over COM; without it on the page, the only
         // way to tune a TTL was to change it and hope. Read-only, and read fresh
         // on every visit to this page.
         var cacheStats = Card(L("How the caches are performing"),
            L("Live, from the running server. The hit rate is the share of lookups answered from memory since the counters last reset - which they do when the service restarts, when a cache is switched off, and when its TTL is changed, so a rate right after any of those describes a very short sample. A lookup for something that does not exist counts as a miss and caches nothing, so a server being probed for addresses it does not host shows a low rate without anything being wrong with the cache."));
         cacheStats.Settings.Add(new ComStat
         {
            Path = "Cache.HitRates",
            Label = L("Hit rate and memory in use, per cache"),
            Read = () => DescribeCaches_(),
            Blurb = L("A low rate on a busy server usually means the TTL is shorter than the interval between lookups of the same object; a rate of 0% with no memory in use means nothing has been looked up yet, which is not the same thing. Memory in use is measured against the size caps above, and a cache at its cap is evicting entries it would otherwise have kept.")
         });
         Tab(L("Cache")).Cards.Add(cacheStats);

         var index = Card(L("Message indexing"), L("Builds a search index so IMAP SEARCH and the web client are faster."));
         index.Settings.Add(new ComBool { Path = "MessageIndexing.Enabled", Label = L("Enable message indexing") });
         // Tuning lives with the feature it tunes: a tab holding a single on/off
         // switch reads as "there is nothing to adjust here".
         index.Settings.Add(new IniNumber { Path = "IndexerFullMinutes", Label = L("Full re-index interval (minutes)"), Default = 720, IniStore = iniStore_ });
         index.Settings.Add(new IniNumber { Path = "IndexerFullLimit", Label = L("Messages per full-index pass"), Default = 25000, IniStore = iniStore_ });
         index.Settings.Add(new IniNumber { Path = "IndexerQuickLimit", Label = L("Messages per quick-index pass"), Default = 1000, IniStore = iniStore_ });
         // The full-text term index is a second, optional layer behind the switch
         // above: it needs message indexing on to run at all, and the server
         // refuses nothing when it is off - SEARCH BODY/TEXT simply reads every
         // message instead of the narrowed set. These four keys had no editor and
         // no README entry, so the feature was invisible to anyone who did not
         // read IniFileSettings.cpp.
         index.Settings.Add(new IniBool
         {
            Path = "IndexerFullText",
            Label = L("Full-text term index for IMAP SEARCH BODY and TEXT"),
            Default = false,
            Blurb = L("Off by default because it costs a term table of a few kilobytes per message and a backfill pass over every message already delivered - an administrator's decision, never an upgrade's. Results are identical on and off: the index only narrows which messages the substring scan has to read. Requires message indexing (above) to be enabled; on its own this indexes nothing."),
            IniStore = iniStore_
         });
         index.Settings.Add(new IniNumber
         {
            Path = "IndexerFullTextBatchSize",
            Label = L("Messages one backfill pass reads before pausing (1-100000)"),
            Default = 250,
            MinimumValue = 1,
            IniStore = iniStore_
         });
         index.Settings.Add(new IniNumber
         {
            Path = "IndexerFullTextMinTokenLength",
            Label = L("Shortest search string the index answers for (3-64; shorter strings fall back to the scan)"),
            Default = 3,
            MinimumValue = 3,
            IniStore = iniStore_
         });
         index.Settings.Add(new IniNumber
         {
            Path = "IndexerFullTextMaxTokensPerMessage",
            Label = L("Distinct terms per message before it is always scanned instead (64-1000000)"),
            Default = 2048,
            MinimumValue = 64,
            IniStore = iniStore_
         });

         // The two actions on MessageIndexing existed over COM and nowhere else, so
         // "search finds nothing" or "search finds messages that were deleted" had
         // no answer short of writing a script. Both report the counts they moved
         // rather than only "done": the whole question an administrator has here is
         // whether the index matches the mail, and a number answers it.
         index.Settings.Add(new ComAction
         {
            Path = "MessageIndexing.Enabled",
            ButtonText = L("Index now, and show how far behind it is"),
            Blurb = L("Wakes the indexer instead of waiting for its next pass, and reports how many of this server's messages are currently indexed. The pass itself runs in the background and is bounded by the limits above, so on a large backlog it makes progress rather than finishing. With indexing switched off there is no indexer to wake, and this says so rather than reporting success."),
            Action = () => RunIndexNow_()
         });
         index.Settings.Add(new ComAction
         {
            Path = "MessageIndexing.Enabled",
            ButtonText = L("Discard the index and rebuild it"),
            Blurb = L("Empties the index and asks the indexer to rebuild it. No mail is touched - the index is derived data - but until the rebuild catches up, IMAP SEARCH finds less than it should. Worth doing when the index has more entries than the server has messages, which means entries for mail that no longer exists."),
            Action = () => RebuildIndex_()
         });
         Tab(L("Indexing")).Cards.Add(index);

         // The message store's form (docs/MessageStore.md): the switch, the size a
         // part must reach to be held once, and - because a switch that says it
         // saves space is a claim - what the store holds and saves, measured by the
         // server itself (Settings.MessageStoreStatus, GET /api/v1/store).
         var store = Card(L("Message store"),
            L("Whether new mail is stored compressed, with the large attachments it shares with other messages kept once. Clients see every message exactly as it arrived. Turning it off stores new mail as it comes and leaves the stored messages readable; hmailserver --compress-store rewrites the messages already there, and --decompress puts them back."));
         store.Settings.Add(new ComBool { Path = "MessageStoreCompression", Label = L("Store new mail compressed, with large parts held once") });
         store.Settings.Add(new ComText { Path = "MessageStoreDedupMinKB", Label = L("Smallest part held once (KB; 0 holds nothing once)"), Numeric = true });
         store.Settings.Add(new ComStat
         {
            Path = "MessageStoreStatus",
            Label = L("What the store holds, and what it saves"),
            Read = () => DescribeStore_(),
            Blurb = L("Measured by the server on its own time, and at most a quarter of an hour old. A file with several names is counted once.")
         });
         Tab(L("Message store")).Cards.Add(store);

         // Messages kept in an object store rather than the data directory
         // (docs/ObjectStorage.md). Kept by the server with no COM property; the
         // bucket and its credentials are made at the provider, not here.
         var objects = Card(L("Object storage (S3 or Azure)"),
            L("Keep messages in an S3-compatible store (AWS S3, MinIO, Wasabi, Backblaze B2, Ceph) or in Azure Blob Storage instead of the data directory. The bucket or container has to exist already and be reachable from this server. Copy what is stored now with hmailserver --copy-store-to-object-storage before switching new messages to the object store."));
         objects.Settings.Add(new RestChoice
         {
            Path = "MessageStoreBackend",
            Label = L("Where new messages are stored"),
            Key = "message_store_backend",
            Options = new (int, string)[]
            {
               (0, L("In the data directory (default)")),
               (1, L("In the object store"))
            },
            Words = new[] { "file", "object" }, // no-loc
            Blurb = L("A message not on the local disk is looked for in the object store while one is named, so switching back hides nothing. Taken within five seconds.")
         });
         objects.Settings.Add(new RestChoice
         {
            Path = "MessageStoreObjectService",
            Label = L("Object store"),
            Key = "message_store_object_service",
            Options = new (int, string)[]
            {
               (0, L("None (default)")),
               (1, L("S3 or S3-compatible")),
               (2, L("Azure Blob Storage"))
            },
            Words = new[] { "", "s3", "azure" } // no-loc
         });
         objects.Settings.Add(new RestText
         {
            Path = "MessageStoreObjectEndpoint",
            Label = L("Endpoint (empty = the service's own)"),
            Key = "message_store_object_endpoint",
            Placeholder = "https://minio.example.com:9000", // no-loc
            Blurb = L("https, with its certificate verified; plain http only to this machine. Empty uses AWS's regional endpoint, or the storage account's own for Azure.")
         });
         objects.Settings.Add(new RestText
         {
            Path = "MessageStoreObjectRegion",
            Label = L("Region (S3)"),
            Key = "message_store_object_region",
            Blurb = L("us-east-1 by default, which MinIO and Ceph take unless they were set up with another.")
         });
         objects.Settings.Add(new RestText
         {
            Path = "MessageStoreObjectBucket",
            Label = L("Bucket (S3) or container (Azure)"),
            Key = "message_store_object_bucket",
            Blurb = L("It has to exist: this server makes none.")
         });
         objects.Settings.Add(new RestChoice
         {
            Path = "MessageStoreObjectAddressing",
            Label = L("How a request names the bucket (S3)"),
            Key = "message_store_object_addressing",
            Options = new (int, string)[]
            {
               (0, L("Automatic: virtual host for AWS, path for another endpoint (default)")),
               (1, L("Path: https://endpoint/bucket/key")),
               (2, L("Virtual host: https://bucket.endpoint/key"))
            },
            Words = new[] { "auto", "path", "virtual" } // no-loc
         });
         objects.Settings.Add(new RestText
         {
            Path = "MessageStoreObjectPrefix",
            Label = L("Key prefix (empty = none)"),
            Key = "message_store_object_prefix",
            Placeholder = "mail/", // no-loc
            Blurb = L("Lets one bucket hold more than one server's store. Change it only with the store copied under the new prefix: what is under the old one is not looked for.")
         });
         objects.Settings.Add(new RestText
         {
            Path = "MessageStoreObjectAccessKey",
            Label = L("Access key ID (S3) or storage account name (Azure)"),
            Key = "message_store_object_access_key"
         });
         objects.Settings.Add(new RestSecret
         {
            Path = "MessageStoreObjectSecret",
            Label = L("Secret access key (S3) or account key (Azure) (leave empty to keep current)"),
            Key = "message_store_object_secret",
            Blurb = L("Stored protected and never shown again.")
         });
         objects.Settings.Add(new RestSecret
         {
            Path = "MessageStoreObjectSasToken",
            Label = L("Shared access signature, instead of the account key (Azure) (leave empty to keep current)"),
            Key = "message_store_object_sas_token",
            Blurb = L("The query string the Azure portal gives, beginning sv=. Stored protected and never shown again.")
         });
         objects.Settings.Add(new RestText
         {
            Path = "MessageStoreObjectCaFile",
            Label = L("CA certificate for a private endpoint (PEM; empty = the machine's roots)"),
            Key = "message_store_object_ca_file",
            BrowseFile = true,
            FileFilter = L("Certificates (*.pem;*.crt;*.cer)|*.pem;*.crt;*.cer|All files (*.*)|*.*"),
            Blurb = L("For a MinIO or Ceph gateway behind a CA of your own. The host name is checked either way.")
         });
         objects.Settings.Add(new RestNumber
         {
            Path = "MessageStoreObjectCacheMB",
            Label = L("Read cache in MB (0 = none; up to 4096)"),
            Key = "message_store_object_cache_mb",
            Maximum = 4096,
            Blurb = L("32 by default. What was read a moment ago is kept for half a minute, so that one IMAP FETCH is one request and not five.")
         });
         Tab(L("Message store")).Cards.Add(objects);

         // Mailbox encryption at rest (docs/EncryptionAtRest.md): the one server-wide
         // setting of it. Kept by the server with no COM property.
         var atRest = Card(L("Mailboxes encrypted at rest"),
            L("An account encrypted at rest can be read by the server only while its mailbox key is open, which the account's password opens at sign-in."));
         atRest.Settings.Add(new RestNumber
         {
            Path = "MailboxKeyIdleMinutes",
            Label = L("Minutes a mailbox key stays open after its last use (1 to 1440)"),
            Key = "mailbox_key_idle_minutes",
            Minimum = 1,
            Maximum = 1440,
            Blurb = L("15 by default, counted once no IMAP or POP3 session holds the key. After that the mail is readable again only after the next sign-in with the password.")
         });
         Tab(L("Message store")).Cards.Add(atRest);

         // The three [Database] values that are tuning rather than connection detail.
         //
         // Everything else in that section - server, database name, credentials, type
         // and provider - decides whether the server starts at all, and is owned by
         // the setup wizard. These three change how it behaves once connected, and
         // they had no editor anywhere: an administrator whose server logs
         // "connection pool exhausted" had to hand-edit the file, and a mail server
         // whose database is briefly unreachable at boot had no way to lengthen the
         // retry window without doing the same.
         //
         // Defaults and the SQL CE override are read from IniFileSettings.cpp lines
         // 135-145: five connections, six attempts, five seconds between them, and
         // no_of_dbconnections_ forced to 1 when the type is MSSQLCE regardless of
         // what the file says.
         //
         // Written out here rather than in a helper because the settings-index
         // generator only reads the body of a Build<Section>() method; rows built
         // anywhere else compile, render and save, and are invisible to Ctrl+K.
         string databaseType = iniStore_.IsAvailable
            ? iniStore_.ReadFrom("Database", "Type", "").Trim()
            : "";

         bool builtInDatabase = string.Equals(databaseType, "MSSQLCE", StringComparison.OrdinalIgnoreCase);

         // Named rather than described: an administrator who is about to raise the
         // pool size needs to know it will be ignored BEFORE they raise it, and the
         // only honest way to know is to read the configured type.
         string poolNote =
            L("How many database connections the server keeps open and shares between its worker threads. Raising it helps only when threads are visibly waiting for a connection - see the database connection acquire timeout on the Server limits & expert settings page, which is what they are waiting on.");

         if (builtInDatabase)
         {
            poolNote += L("  THIS SERVER USES THE BUILT-IN DATABASE (MSSQLCE), where the value is forced to 1 whatever is set here. That is deliberate: SQL Server Compact does not behave reliably under concurrent connections. Changing it below will have no effect until the server is moved to MySQL, MSSQL or PostgreSQL.");
         }

         var database = Card(L("Database connections"),
            L("How the server uses the database it is already configured for. Where that database IS - the server, name, credentials and type - is set up by the installation wizard and is deliberately not editable here, because a wrong value there stops the server from starting rather than making it slower. Applies after a service restart.")
            + (databaseType.Length > 0 ? F("  Configured database type: {0}.", databaseType) : ""));

         database.Settings.Add(new SectionIniNumber
         {
            Section = "Database", // no-loc
            Path = "NumberOfConnections",
            Label = L("Connections in the pool") + (builtInDatabase ? L(" (ignored: this server uses the built-in database)") : ""),
            Default = 5,
            MinimumValue = 1,
            Blurb = poolNote,
            IniStore = iniStore_
         });

         database.Settings.Add(new SectionIniNumber
         {
            Section = "Database", // no-loc
            Path = "ConnectionAttempts",
            Label = L("Attempts to reach the database at start-up"),
            Default = 6,
            MinimumValue = 1,
            Blurb = L("A mail server usually starts with the rest of the machine, and on a machine where the database service starts second, the first few attempts fail. Together with the delay below this is the whole window: six attempts five seconds apart is half a minute. Lengthen it rather than delaying the service if the database is on another host that is slower to come up."),
            IniStore = iniStore_
         });

         database.Settings.Add(new SectionIniNumber
         {
            Section = "Database", // no-loc
            Path = "ConnectionAttemptsDelay",
            Label = L("Seconds between those attempts"),
            Default = 5,
            MinimumValue = 1,
            IniStore = iniStore_
         });
         database.Settings.Add(new IniNumber
         {
            Path = "DatabaseStatementTimeout",
            Label = L("Seconds a single SQL statement may run (0 = no limit)"),
            Default = 30,
            MinimumValue = 0,
            Blurb = L("What stops one statement blocked on a lock from holding a worker thread for ever. Honoured at connect time by MySQL/MariaDB and PostgreSQL; MS SQL and SQL CE start at ADO's own 30 seconds and pick this value up on any connection that has run an upgrade or maintenance script. Those scripts run under a 30-minute per-statement ceiling of their own regardless of this value."),
            IniStore = iniStore_
         });

         Tab(L("Database")).Cards.Add(database);
      }

      private void BuildAdvanced()
      {
         Header.Title = L("Advanced");
         Header.Subtitle = L("Server-wide defaults, keeping copies of mail and the scripting engine.");

         var general = Card(L("General"),
            L("The administrator password and two-factor authentication are on the Administrative access page."));
         general.Settings.Add(new ComText { Path = "DefaultDomain", Label = L("Default domain (for unqualified logons)") });
         general.Settings.Add(new ComBool { Path = "IPv6PreferredEnabled", Label = L("Prefer IPv6 when delivering") });

         // The help panes' "Open on the wiki" link, in all three surfaces. The
         // server refuses a value that is not empty and not an absolute http or
         // https URL, in the same words the Deck shows.
         general.Settings.Add(new ComText
         {
            Path = "HelpWikiBaseUrl",
            Label = L("Wiki the help pane's \"Open on the wiki\" link points at (empty hides the link)"),
            Blurb = L("The documentation the help pane shows is the server's own copy; this is where the same page is on the wiki, with the document's name added. As shipped, the project's GitLab wiki.")
         });

         // The old label said "legacy COM admin tools", which was closer to the
         // truth than most of this page but still let an administrator believe it
         // would translate something they can see. It cannot: the server hands the
         // value straight back over COM and translates nothing itself, and this
         // Control Panel has no resx at all, so the only consumer is a third-party
         // tool. Left editable rather than made inert for exactly that reason - a
         // script may be reading it.
         general.Settings.Add(new ComText
         {
            Path = "UserInterfaceLanguage",
            Label = L("Administrator UI language (third-party COM tools only)"),
            Blurb = L(SettingClaims.NoteFor("UserInterfaceLanguage"))
         });
         Tab(L("General")).Cards.Add(general);

         // Mirroring and archiving are the two ways of keeping a copy of every
         // message, so they belong on one tab. Archiving lived on the catch-all
         // INI page, where an admin looking for "keep a copy" never found it.
         TabDef copies = Tab(L("Copies of mail"));

         var mirror = Card(L("Mirroring"), L("Sends a copy of every message passing through the server to one address (compliance archiving)."));
         mirror.Settings.Add(new ComText { Path = "MirrorEMailAddress", Label = L("Mirror address (empty = disabled)") });
         copies.Cards.Add(mirror);

         var archive = Card(L("Message archiving"),
            L("Keeps a copy of every message received over SMTP in a folder tree, in addition to delivering it as normal - it does not divert or hold back mail. Mail from a local sender is filed under <domain>\\<mailbox>, mail from elsewhere under Inbound, and messages with no envelope sender (bounces and delivery reports) under Error, plus a copy in the folder of each local recipient."));
         archive.Settings.Add(new IniText
         {
            Path = "ArchiveDir",
            Label = L("Archive folder (empty = archiving off)"),
            Placeholder = @"D:\MailArchive",
            BrowseFolder = true,
            IniStore = iniStore_
         });
         archive.Settings.Add(new IniText
         {
            Path = "ArchiveDomains",
            Label = L("Only archive mail for these domains (comma-separated; empty = every message)"),
            Placeholder = "example.com, example.org", // no-loc
            Blurb = L("An archive is usually kept for a legal or contractual reason, and that reason usually applies to particular domains rather than to every domain this server hosts. With a list here a message is archived only when its local sender or one of its recipients belongs to a listed domain, and only the copies for listed domains are made - a listed recipient still gets their copy when the sender's domain is not listed. Matched without regard to case."),
            IniStore = iniStore_
         });
         archive.Settings.Add(new IniBool
         {
            Path = "ArchiveHardLinks",
            Label = L("Hard-link each recipient's copy instead of copying it"),
            Default = false,
            Blurb = L("Every local recipient's copy becomes another name for the same file inside the archive folder, so a message to ten mailboxes costs one copy rather than ten. Needs an NTFS archive folder; if the link cannot be created the server copies the file and says so in the SMTP log."),
            IniStore = iniStore_
         });
         copies.Cards.Add(archive);

         // The two disk-space keys govern the message-store volume, which is also
         // what archiving fills, so they sit here rather than under Database.
         var disk = Card(L("Disk space"),
            L("What the server does as the message-store volume fills. Both values are absolute megabytes rather than percentages, because what decides whether the next message fits is how many bytes are left, not what fraction of the volume they are."));
         disk.Settings.Add(new IniNumber
         {
            Path = "DiskSpaceWarningThresholdMB",
            Label = L("Warn in the application log and Windows event log below this much free space (MB; 0 = never)"),
            Default = 1024,
            MinimumValue = 0,
            Blurb = L("Where the administrator is told, well before anything is refused. 1024 MB is roughly fifty more maximum-size messages."),
            IniStore = iniStore_
         });
         disk.Settings.Add(new IniNumber
         {
            Path = "MinimumFreeDiskSpaceMB",
            Label = L("Refuse mail with a temporary error below this much free space (MB; 0 = never)"),
            Default = 100,
            MinimumValue = 0,
            Blurb = L("Biased upwards on purpose: refusing early costs delivery latency and nothing else, because the refusal is temporary and a sending server retries for days, while a volume that reaches zero costs a database that will not open and a service that will not start."),
            IniStore = iniStore_
         });
         copies.Cards.Add(disk);

         var script = Card(L("Scripting engine"), L("Runs event scripts (OnAcceptMessage, OnDeliveryStart...) from the Events folder; the script itself is edited on the Event scripts page. The engine reloads when you save."));
         script.Settings.Add(new ComBool { Path = "Scripting.Enabled", Label = L("Enable server-side event scripts") });
         script.Settings.Add(new ComText { Path = "Scripting.Language", Label = L("Language (VBScript, JScript or JavaScript)") });
         script.Settings.Add(new IniText
         {
            Path = "ScriptAllowedObjects",
            Label = L("COM objects a script may create (* = any, as shipped; empty = none beyond hMailServer's own; ProgIDs or CLSIDs, comma separated)"),
            Blurb = L("An event script runs inside the server with the service account's rights, so what it may instantiate is a security decision rather than a convenience. As shipped this is *, which allows any object, so that upgrading never breaks a script an installation already runs - it is the setting to narrow, not the safe state. Name only what a script of yours actually needs - a scripting file system object, an HTTP client - and everything else is refused with the object's name in the error log; empty refuses everything beyond hMailServer's own objects."),
            // Absent means "*", so that a key appearing in an upgrade cannot break
            // scripts an installation already runs; the editor shows that, because
            // a blank here saved back would mean "none" and break them itself.
            Default = "*",
            IniStore = iniStore_
         });
         Tab(L("Scripting")).Cards.Add(script);

         // Migrations from another server and PST imports (docs/Migration.md): the
         // jobs are started in the Control Deck or over the REST API; these two are
         // the server's. Kept with no COM property.
         var migration = Card(L("Migrations"),
            L("Mailbox migrations from another server, and imports of Outlook data files (PST). The jobs are started in the Control Deck or over the REST API."));
         migration.Settings.Add(new RestNumber
         {
            Path = "MigrationConnectionsPerHost",
            Label = L("Connections to one source host, across every running migration (1 to 64)"),
            Key = "migration_connections_per_host",
            Minimum = 1,
            Maximum = 64,
            Blurb = L("4 by default. A provider counts sessions per client address, and too many at once gets an address throttled for the rest of the day.")
         });
         migration.Settings.Add(new RestText
         {
            Path = "PstImportDirectory",
            Label = L("Folder PST files are copied into for import (empty = no PST import)"),
            Key = "pst_import_directory",
            BrowseFolder = true,
            Blurb = L("An absolute path on the server. A PST job names a file in the subfolder for the target account's domain, such as example.com\\archive.pst, and nowhere else.")
         });
         Tab(L("Migrations")).Cards.Add(migration);
      }

      /// <summary>
      /// The webmail and the other pages people sign in to in a browser - the
      /// self-service portal and the Control Deck. Until 6.3.5 none of this was in
      /// the Control Panel: nearly every one of these settings is kept by the server
      /// with no COM property, and is reached over the REST API, which is also the
      /// listener that serves the webmail - so a server whose webmail works is a
      /// server this page can read. The few with a COM property (the session
      /// policies, OpenPGP's limits, Web Push's switch and contact) are written over
      /// COM, as every other page's are.
      /// </summary>
      private void BuildWebmail()
      {
         Header.Title = L("Webmail");
         Header.Subtitle = L("The pages people sign in to in a browser - the webmail, the self-service portal and the Control Deck: how they sign in, how long a session lasts, what people may do, OpenPGP, notifications and importing. Most of these are reached over the REST API, which also serves the webmail.");

         // ---- Sign-in -------------------------------------------------------

         var passkeys = Card(L("Passkeys"),
            L("People may enrol a passkey (WebAuthn) and sign in with it, in the webmail and in the Control Deck. Nothing changes until somebody enrols one: until then they sign in with their password."));
         passkeys.Settings.Add(new RestBool
         {
            Path = "PasskeysEnabled",
            Label = L("Offer passkeys"),
            Key = "passkeys_enabled",
            Blurb = L("On by default. Off refuses every passkey sign-in; the passkeys already enrolled stay until their owners remove them.")
         });
         passkeys.Settings.Add(new RestText
         {
            Path = "PasskeyRelyingParty",
            Label = L("Relying party (empty = the host the page is on)"),
            Key = "passkey_relying_party",
            Placeholder = "example.com", // no-loc
            Blurb = L("A host name, not an address. Set a parent domain when the webmail and the Control Deck are reached at several names, so that one passkey works at all of them.")
         });
         passkeys.Settings.Add(new RestText
         {
            Path = "PasskeyAllowedOrigins",
            Label = L("Origins passkeys may be used at (comma separated)"),
            Key = "passkey_allowed_origins",
            Placeholder = "https://mail.example.com, https://webmail.example.com", // no-loc
            Blurb = L("Each as the browser writes it: https, the host and any port. Empty, the default, is right while the relying party is empty; once a relying party is set, no passkey is registered or taken until this names the origins.")
         });
         Tab(L("Sign-in")).Cards.Add(passkeys);

         var oidc = Card(L("Single sign-on with OpenID Connect"),
            L("Sign people in with your identity provider - Entra ID, Okta, Keycloak, Google. Register this server with the provider first, as a web client whose redirect address is the base address below followed by /portal/oidc/callback. Single sign-on is off until the issuer is set."));
         oidc.Settings.Add(new RestText
         {
            Path = "OidcIssuer",
            Label = L("Issuer (empty = no single sign-on)"),
            Key = "oidc_issuer",
            Placeholder = "https://login.example.com", // no-loc
            Blurb = L("Its discovery document is read from the issuer followed by /.well-known/openid-configuration.")
         });
         oidc.Settings.Add(new RestText
         {
            Path = "OidcClientId",
            Label = L("Client ID"),
            Key = "oidc_client_id"
         });
         oidc.Settings.Add(new RestSecret
         {
            Path = "OidcClientSecret",
            Label = L("Client secret (leave empty to keep current)"),
            Key = "oidc_client_secret",
            Blurb = L("None is needed for a public client, which PKCE makes safe.")
         });
         oidc.Settings.Add(new RestText
         {
            Path = "OidcScopes",
            Label = L("Scopes (space separated)"),
            Key = "oidc_scopes",
            Blurb = L("openid email by default, which is what the sign-in needs: the verified email names the account.")
         });
         oidc.Settings.Add(new RestText
         {
            Path = "OidcRedirectBaseUrl",
            Label = L("Base address of the webmail, as the provider reaches it"),
            Key = "oidc_redirect_base_url",
            Placeholder = "https://mail.example.com", // no-loc
            Blurb = L("SAML sign-in is under it too. Empty uses the import sign-in's base address, on the Import tab.")
         });
         oidc.Settings.Add(new RestBool
         {
            Path = "OidcAutoProvision",
            Label = L("Make a mailbox at the first sign-in of a verified address that has none"),
            Key = "oidc_auto_provision",
            Blurb = L("Off by default: an address this server does not know is refused. A mailbox made this way has no password of its own and is in a domain the server already has.")
         });
         oidc.Settings.Add(new RestText
         {
            Path = "OidcAdministrators",
            Label = L("Addresses that sign in to the Control Deck as the administrator through OpenID Connect (comma separated)"),
            Key = "oidc_administrators",
            Blurb = L("Empty, the default: single sign-on reaches the webmail only, and the Control Deck still asks for the administrator password.")
         });
         Tab(L("Sign-in")).Cards.Add(oidc);

         var saml = Card(L("Single sign-on with SAML"),
            L("Sign people in with a SAML identity provider - Entra ID, Okta, ADFS. Give the provider this server's metadata, which is the webmail's base address followed by /saml/metadata. SAML is off until the provider's entity ID, sign-in address and certificate are all set."));
         saml.Settings.Add(new RestText
         {
            Path = "SamlIdpEntityId",
            Label = L("Identity provider's entity ID (empty = no SAML sign-in)"),
            Key = "saml_idp_entity_id",
            Blurb = L("Exactly as its metadata writes it: the Microsoft Entra Identifier, Okta's Identity Provider Issuer, the ADFS Federation Service Identifier.")
         });
         saml.Settings.Add(new RestText
         {
            Path = "SamlIdpSsoUrl",
            Label = L("Identity provider's sign-in address"),
            Key = "saml_idp_sso_url",
            Placeholder = "https://idp.example.com/sso", // no-loc
            Blurb = L("For the HTTP-Redirect binding: Entra ID's Login URL, Okta's Identity Provider Single Sign-On URL.")
         });
         saml.Settings.Add(new RestPaste
         {
            Path = "SamlIdpCertificate",
            Label = L("Identity provider's signing certificate, in PEM (leave empty to keep current)"),
            Key = "saml_idp_certificate",
            Blurb = L("Paste the certificate, or two while the provider rolls its key over. It is never shown again: the line below says what is held.")
         });
         saml.Settings.Add(new RestStat
         {
            Path = "SamlIdpCertificateFingerprints",
            Label = L("Certificates held"),
            Key = "saml_idp_certificate_fingerprints",
            WhenEmpty = L("None.")
         });
         saml.Settings.Add(new RestText
         {
            Path = "SamlSpEntityId",
            Label = L("This server's entity ID (empty = the base address followed by /saml)"),
            Key = "saml_sp_entity_id",
            Blurb = L("What the provider knows this server as: Entra ID's Identifier, Okta's Audience URI.")
         });
         saml.Settings.Add(new RestText
         {
            Path = "SamlEmailAttribute",
            Label = L("Attribute the address is read from"),
            Key = "saml_email_attribute",
            Blurb = L("Used when the NameID is not an email address. By default the emailaddress claim that Entra ID and ADFS send.")
         });
         saml.Settings.Add(new RestBool
         {
            Path = "SamlRequireSignedAssertion",
            Label = L("Require the assertion itself to be signed"),
            Key = "saml_require_signed_assertion",
            Blurb = L("On by default, which is how Entra ID and ADFS sign. Off accepts a signature over the whole response.")
         });
         saml.Settings.Add(new RestBool
         {
            Path = "SamlAutoProvision",
            Label = L("Make a mailbox at the first sign-in of an address that has none"),
            Key = "saml_auto_provision",
            Blurb = L("Off by default.")
         });
         saml.Settings.Add(new RestText
         {
            Path = "SamlAdministrators",
            Label = L("Addresses that sign in to the Control Deck as the administrator through SAML (comma separated)"),
            Key = "saml_administrators",
            Blurb = L("Empty, the default: SAML reaches the webmail only.")
         });
         saml.Settings.Add(new RestText
         {
            Path = "SamlButtonLabel",
            Label = L("Words on the SAML sign-in button"),
            Key = "saml_button_label",
            Blurb = L("One line of up to 60 characters. The default, Sign in with SAML, is shown in each reader's language; anything else as written.")
         });
         Tab(L("Sign-in")).Cards.Add(saml);

         var recovery = Card(L("Account recovery"),
            L("People may set a recovery address and one-time recovery codes in the webmail, and reset a forgotten password with them. A reset link can be sent only when the server knows its own public address: the base address for messages that expire, the quarantine digest's or the import sign-in's."));
         recovery.Settings.Add(new RestBool
         {
            Path = "AccountRecoveryEnabled",
            Label = L("Let people recover a forgotten password"),
            Key = "account_recovery_enabled",
            Blurb = L("On by default, and inert until somebody sets a recovery address.")
         });
         recovery.Settings.Add(new RestNumber
         {
            Path = "RecoveryResetLinkMinutes",
            Label = L("Minutes a password-reset link works (5 to 1440)"),
            Key = "recovery_reset_link_minutes",
            Minimum = 5,
            Maximum = 1440,
            Blurb = L("30 by default.")
         });
         recovery.Settings.Add(new RestNumber
         {
            Path = "RecoveryVerifyLinkHours",
            Label = L("Hours the link that proves a recovery address works (1 to 168)"),
            Key = "recovery_verify_link_hours",
            Minimum = 1,
            Maximum = 168,
            Blurb = L("24 by default.")
         });
         Tab(L("Sign-in")).Cards.Add(recovery);

         // ---- Sessions ------------------------------------------------------

         var sessions = Card(L("Browser sessions"),
            L("How long a signed-in session of the webmail, the self-service portal or the Control Deck lasts. A domain or an account can hold its own value, on the Levels tab of its dialog."));
         sessions.Settings.Add(new ComText
         {
            Path = "SessionIdleSeconds",
            Label = L("Seconds a session may go unused (0 = the server's own 1800)"),
            Numeric = true,
            Maximum = 2592000,
            Blurb = L("At most 2592000, thirty days.")
         });
         sessions.Settings.Add(new ComText
         {
            Path = "SessionLifetimeSeconds",
            Label = L("Seconds a session may last at all (0 = the server's own 43200)"),
            Numeric = true,
            Maximum = 2592000,
            Blurb = L("43200 is twelve hours. At most 2592000, thirty days.")
         });
         sessions.Settings.Add(new ComBool
         {
            Path = "SessionOnePerDevice",
            Label = L("Signing in again from the same device ends the earlier session"),
            Blurb = L("Off by default. Useful on shared machines.")
         });
         sessions.Settings.Add(new ComBool
         {
            Path = "SessionBindToAddress",
            Label = L("Refuse a session from any address but the one it began on"),
            Blurb = L("Off by default. A phone moving between wi-fi and a mobile network loses its session while this is on.")
         });
         Tab(L("Browser sessions")).Cards.Add(sessions);

         // ---- What people may do --------------------------------------------

         var reader = Card(L("What people may do in the webmail"),
            L("The ceilings on what a person may add for themselves in the webmail."));
         reader.Settings.Add(new RestNumber
         {
            Path = "ReaderFetchAccountsMax",
            Label = L("Other POP3 or IMAP accounts a person may collect from (0 = none; up to 20)"),
            Key = "reader_fetch_accounts_max",
            Maximum = 20,
            Blurb = L("3 by default. The external accounts an administrator adds are not counted.")
         });
         reader.Settings.Add(new RestBool
         {
            Path = "ReaderFetchPrivateHosts",
            Label = L("Let those accounts be on a private address or an unusual port"),
            Key = "reader_fetch_accounts_private_addresses",
            Blurb = L("Off by default: the server connects from inside your network on the person's word. Turn it on to let people collect from an old mail server inside the network.")
         });
         reader.Settings.Add(new RestNumber
         {
            Path = "ReaderMaskedAddressesMax",
            Label = L("Masked addresses a person may make (0 = none; up to 200)"),
            Key = "reader_masked_addresses_max",
            Maximum = 200,
            Blurb = L("20 by default.")
         });
         reader.Settings.Add(new RestNumber
         {
            Path = "ReaderSenderAddressesMax",
            Label = L("Addresses at other providers a person may send as (0 = none; up to 20)"),
            Key = "reader_sender_addresses_max",
            Maximum = 20,
            Blurb = L("5 by default. Each is proved with a code first; mail sent as one may be refused where its domain's DMARC policy is strict.")
         });
         reader.Settings.Add(new RestNumber
         {
            Path = "ReaderOfflineDaysMax",
            Label = L("Days of mail a person may keep in the browser to read offline (0 = none; up to 365)"),
            Key = "reader_offline_days_max",
            Maximum = 365,
            Blurb = L("30 by default.")
         });
         reader.Settings.Add(new RestNumber
         {
            Path = "RecallDays",
            Label = L("Days after sending that mail to this server's accounts may be recalled (1 to 365)"),
            Key = "recall_days",
            Minimum = 1,
            Maximum = 365,
            Blurb = L("14 by default. The unread copies are deleted; the read ones stay.")
         });
         reader.Settings.Add(new RestNumber
         {
            Path = "RecoverDeletedDays",
            Label = L("Days deleted messages are kept for the person to put back (0 = none; up to 365)"),
            Key = "recover_deleted_days",
            Maximum = 365,
            Blurb = L("0 by default. The copies count against no quota.")
         });
         reader.Settings.Add(new RestBool
         {
            Path = "BrandLogosEnabled",
            Label = L("Show senders' brand logos (BIMI)"),
            Key = "brand_logos_enabled",
            Blurb = L("On by default. Only for mail that passed DMARC from a domain whose policy is quarantine or reject, which needs Authentication-Results headers on inbound mail, on the Transport security page.")
         });
         reader.Settings.Add(new RestText
         {
            Path = "ConfidentialBaseUrl",
            Label = L("Base address for the link in a message that expires"),
            Key = "confidential_base_url",
            Placeholder = "https://mail.example.com", // no-loc
            Blurb = L("The address the webmail is reached at from outside. Empty uses the quarantine digest's base address, then the import sign-in's; with none set, the webmail does not offer an expiry.")
         });
         Tab(L("What people may do")).Cards.Add(reader);

         var directory = Card(L("Directory"),
            L("Every active account and enabled list of a domain, shown to the people of that domain: in the webmail, in address completion, and as a read-only CardDAV address book. It never crosses a domain."));
         directory.Settings.Add(new RestBool
         {
            Path = "DirectoryEnabled",
            Label = L("Show each domain's directory to its people"),
            Key = "directory_enabled",
            Blurb = L("On by default. Turn it off where one domain holds people who are not one organisation.")
         });
         directory.Settings.Add(new RestBool
         {
            Path = "DirectoryUserEditable",
            Label = L("Let people edit their own directory entry"),
            Key = "directory_user_editable",
            Blurb = L("On by default: title, department, telephone numbers and photo. Off leaves them to the administrator; a person can always take themselves out of the directory.")
         });
         Tab(L("What people may do")).Cards.Add(directory);

         // ---- Encryption ----------------------------------------------------

         var pgp = Card(L("OpenPGP"),
            L("OpenPGP in the webmail - keys, signing, encrypting and decrypting, all in the browser - and publishing and looking up keys."));
         pgp.Settings.Add(new RestBool
         {
            Path = "OpenPGPPortalEnabled",
            Label = L("Offer OpenPGP in the webmail"),
            Key = "portal_enabled",
            Group = RestSettings.OpenPgpGroup,
            Blurb = L("On by default. Off leaves the keys people already have in the database untouched.")
         });
         pgp.Settings.Add(new ComText
         {
            Path = "OpenPGPKeysPerAccount",
            Label = L("Keys one account may hold (1 to 64)"),
            Numeric = true,
            Minimum = 1,
            Maximum = 64,
            Blurb = L("8 by default, so that a key being replaced and its replacement can both be found.")
         });
         pgp.Settings.Add(new ComText
         {
            Path = "OpenPGPKeyMaximumBytes",
            Label = L("Largest key accepted, in bytes (1024 to 1048576)"),
            Numeric = true,
            Minimum = 1024,
            Maximum = 1048576,
            Blurb = L("65536 by default.")
         });
         pgp.Settings.Add(new ComBool
         {
            Path = "OpenPGPRequireAddressInUserId",
            Label = L("Refuse a key that does not name the account's own address"),
            Blurb = L("On by default.")
         });
         pgp.Settings.Add(new ComBool
         {
            Path = "OpenPGPWebKeyDirectoryEnabled",
            Label = L("Publish keys in the Web Key Directory"),
            Blurb = L("Off by default. Served under /.well-known/openpgpkey by the web services listener; a domain and the account have to allow it too, in their dialogs.")
         });
         pgp.Settings.Add(new RestBool
         {
            Path = "OpenPGPLookupWkd",
            Label = L("Look up a correspondent's key in their domain's Web Key Directory"),
            Key = "lookup_wkd",
            Group = RestSettings.OpenPgpGroup,
            Blurb = L("On by default.")
         });
         pgp.Settings.Add(new RestBool
         {
            Path = "OpenPGPLookupKeyserver",
            Label = L("Ask a keyserver when their domain has no key"),
            Key = "lookup_keyserver",
            Group = RestSettings.OpenPgpGroup,
            Blurb = L("Off by default: the keyserver learns who is about to write to whom.")
         });
         pgp.Settings.Add(new RestText
         {
            Path = "OpenPGPKeyserverUrl",
            Label = L("Keyserver"),
            Key = "keyserver_url",
            Group = RestSettings.OpenPgpGroup,
            Blurb = L("https://keys.openpgp.org by default. An https address, or http to a keyserver on this machine.")
         });
         pgp.Settings.Add(new RestNumber
         {
            Path = "OpenPGPLookupTimeout",
            Label = L("Seconds a key lookup may take (2 to 30)"),
            Key = "lookup_timeout",
            Group = RestSettings.OpenPgpGroup,
            Minimum = 2,
            Maximum = 30,
            Blurb = L("10 by default. The webmail waits on it.")
         });
         pgp.Settings.Add(new RestNumber
         {
            Path = "ProtectedMessageDays",
            Label = L("Days a password-protected message is kept for its reader (1 to 365)"),
            Key = "protected_message_days",
            Group = RestSettings.OpenPgpGroup,
            Minimum = 1,
            Maximum = 365,
            Blurb = L("30 by default, and the most a sender may ask for.")
         });
         pgp.Settings.Add(new RestBool
         {
            Path = "ZeroAccessAllowed",
            Label = L("Let people make their mailbox zero-access"),
            Key = "zero_access_allowed",
            Group = RestSettings.OpenPgpGroup,
            Blurb = L("Off by default. A zero-access mailbox is encrypted to the person's own key: this server can no longer read, search, export or recover its mail, and a forgotten password without the recovery key loses it. Allowing it switches nobody on.")
         });
         Tab(L("Encryption")).Cards.Add(pgp);

         var smime = Card(L("S/MIME certificate authority"),
            L("A domain may issue S/MIME certificates to its people from its own authority."));
         smime.Settings.Add(new RestText
         {
            Path = "SmimeAuthorityBaseUrl",
            Label = L("Base address new certificates name for revocation"),
            Key = "smime_authority_base_url",
            Placeholder = "http://pki.example.com", // no-loc
            Blurb = L("Where clients reach the web services listener; each certificate names its revocation list and its root under it. Empty, the default, names neither, so a client that insists on checking revocation cannot. A certificate already issued keeps what it was issued with.")
         });
         Tab(L("Encryption")).Cards.Add(smime);

         // ---- Notifications -------------------------------------------------

         var push = Card(L("Web Push notifications"),
            L("Notifications of new mail sent to the browsers and phones people allow. The server signs them with its own VAPID key pair, made once with the button below."));
         push.Settings.Add(new ComBool
         {
            Path = "WebPushEnabled",
            Label = L("Send Web Push notifications"),
            Blurb = L("Off by default. With it off nothing is queued and no push service is reached.")
         });
         push.Settings.Add(new ComText
         {
            Path = "WebPushSubject",
            Label = L("Contact for push services (a mailto: or https: address)"),
            Placeholder = "mailto:postmaster@example.com", // no-loc
            Blurb = L("Empty means postmaster at this server's host name.")
         });
         push.Settings.Add(new RestNumber
         {
            Path = "WebPushTimeToLiveSeconds",
            Label = L("Seconds a push service holds a notification for a device that is offline (0 to 2419200)"),
            Key = "time_to_live_seconds",
            Group = RestSettings.PushGroup,
            Maximum = 2419200,
            Blurb = L("86400 by default; 0 delivers at once or not at all.")
         });
         push.Settings.Add(new RestNumber
         {
            Path = "WebPushMaxPerAccount",
            Label = L("Devices one account may have notifications on (1 to 50)"),
            Key = "max_devices_per_account",
            Group = RestSettings.PushGroup,
            Minimum = 1,
            Maximum = 50,
            Blurb = L("10 by default.")
         });
         push.Settings.Add(new RestNumber
         {
            Path = "WebPushMaxAttempts",
            Label = L("Attempts at a device before it is given up (1 to 20)"),
            Key = "max_attempts",
            Group = RestSettings.PushGroup,
            Minimum = 1,
            Maximum = 20,
            Blurb = L("5 by default.")
         });
         push.Settings.Add(new ComStat
         {
            Path = "WebPushVapidPublicKey",
            Label = L("This server's VAPID public key"),
            Read = () =>
            {
               dynamic settings = ServerSession.Current.Application.Settings;
               try
               {
                  string key = (string)settings.WebPushVapidPublicKey;
                  return string.IsNullOrEmpty(key) ? L("None yet: make one with the button below.") : key;
               }
               finally
               {
                  ServerSession.Release((object)settings);
               }
            }
         });
         push.Settings.Add(new ComAction
         {
            ButtonText = L("Make a new VAPID key pair"),
            Action = MakeWebPushKeys_
         });
         push.Settings.Add(new RestSecret
         {
            Path = "WebPushVapidPrivateKey",
            Label = L("VAPID private key, to bring the pair from another server (leave empty to keep current)"),
            Key = "vapid_private_key",
            Group = RestSettings.PushGroup,
            Blurb = L("base64url of the P-256 key; the public half is worked out from it. Every subscription made under the old pair stops working.")
         });
         Tab(L("Notifications")).Cards.Add(push);

         // ---- Import --------------------------------------------------------

         var import = Card(L("Importing from Gmail and Outlook.com with a sign-in"),
            L("Lets a person bring their mail over from Gmail or Outlook.com by signing in there, without giving this server their password. Register this server with Google and with Microsoft first, with the redirect address that is the base address below followed by /portal/oauth/callback. A provider is offered once it has a client ID - and, for Google, a secret."));
         import.Settings.Add(new RestText
         {
            Path = "ImportOAuthRedirectBaseUrl",
            Label = L("Base address of the webmail, as the providers reach it (empty = no import sign-in)"),
            Key = "import_oauth_redirect_base_url",
            Placeholder = "https://mail.example.com" // no-loc
         });
         import.Settings.Add(new RestText
         {
            Path = "ImportOAuthGoogleClientId",
            Label = L("Google client ID"),
            Key = "import_oauth_google_client_id",
            Blurb = L("A Web application client in the Google Cloud console, with the Gmail API's mail scope.")
         });
         import.Settings.Add(new RestSecret
         {
            Path = "ImportOAuthGoogleClientSecret",
            Label = L("Google client secret (leave empty to keep current)"),
            Key = "import_oauth_google_client_secret"
         });
         import.Settings.Add(new RestText
         {
            Path = "ImportOAuthGoogleAuthorizeUrl",
            Label = L("Google authorization endpoint"),
            Key = "import_oauth_google_authorize_url",
            Blurb = L("Google's public one by default.")
         });
         import.Settings.Add(new RestText
         {
            Path = "ImportOAuthGoogleTokenUrl",
            Label = L("Google token endpoint"),
            Key = "import_oauth_google_token_url",
            Blurb = L("Google's public one by default.")
         });
         import.Settings.Add(new RestText
         {
            Path = "ImportOAuthMsClientId",
            Label = L("Microsoft application (client) ID"),
            Key = "import_oauth_microsoft_client_id",
            Blurb = L("Registered in Microsoft Entra with the delegated IMAP.AccessAsUser.All and offline_access permissions, and personal Microsoft accounts allowed for Outlook.com.")
         });
         import.Settings.Add(new RestSecret
         {
            Path = "ImportOAuthMsClientSecret",
            Label = L("Microsoft client secret (leave empty to keep current)"),
            Key = "import_oauth_microsoft_client_secret"
         });
         import.Settings.Add(new RestText
         {
            Path = "ImportOAuthMsAuthorizeUrl",
            Label = L("Microsoft authorization endpoint"),
            Key = "import_oauth_microsoft_authorize_url",
            Blurb = L("The common one by default; a tenant's own or a national cloud's otherwise.")
         });
         import.Settings.Add(new RestText
         {
            Path = "ImportOAuthMsTokenUrl",
            Label = L("Microsoft token endpoint"),
            Key = "import_oauth_microsoft_token_url",
            Blurb = L("Matching the authorization endpoint.")
         });
         Tab(L("Import")).Cards.Add(import);
      }

      /// <summary>
      /// Settings.GenerateWebPushKeys, after asking when a pair exists already: every
      /// subscription made under the old pair stops working at once, and each device
      /// has to be asked again - which is what to do when the private key may have
      /// been copied, and not a thing to press by accident.
      /// </summary>
      private static (bool ok, string text) MakeWebPushKeys_()
      {
         dynamic settings = ServerSession.Current.Application.Settings;
         try
         {
            string existing = (string)settings.WebPushVapidPublicKey;
            if (!string.IsNullOrEmpty(existing) &&
                MessageBox.Show(L("Make a new VAPID key pair?\n\nEvery Web Push subscription made under the present pair stops working at once, and each person has to allow notifications again. Do this when the private key may have been copied."),
                   L("Control Panel"), MessageBoxButton.YesNo, MessageBoxImage.Warning) != MessageBoxResult.Yes)
            {
               return (true, L("Nothing was changed."));
            }

            string made = (string)settings.GenerateWebPushKeys();
            return (true, F("A new key pair was made. The public key is {0}. Open the page again to see it above.", made));
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            return (false, F("The key pair was not made: {0}", L(ServerSentences.MessageOf(ex))));
         }
         finally
         {
            ServerSession.Release((object)settings);
         }
      }

      /// <summary>
      /// Who may administer this server. Both settings existed but had no
      /// reachable home: the password was buried at the bottom of the Advanced
      /// page, and two-factor setup could only be opened from the logon screen,
      /// so nobody already signed in could find either.
      /// </summary>
      private void BuildAdminAccess()
      {
         Header.Title = L("Administrative access");
         Header.Subtitle = L("The credentials used to administer this server.");

         var password = Card(L("Administrator password"),
            L("The main hMailServer administration password. It is used by this Control Panel, the REST API and any script that connects through the COM API, and is stored hashed in hMailServer.ini. Changing it does not affect mailbox passwords."));
         password.Settings.Add(new ComPassword
         {
            Path = "SetAdministratorPassword",
            Label = L("New administrator password (leave empty to keep the current one)"),
            MethodName = "SetAdministratorPassword"
         });
         Tab(L("Password")).Cards.Add(password);

         // The policy applies to MAILBOX passwords, and it sits on the same page as the
         // administrator password because "Password" is where somebody looks for it -
         // not because the two are related. Everything here is off by default, and the
         // card says why that matters rather than leaving it to be discovered.
         var policy = Card(L("Policy for mailbox passwords"),
            L("Applied when a password is CHOSEN - here, in the account editor, by a script or by the API - and never when one is checked at logon. That distinction is the whole design: an account whose password predates the policy keeps working, because locking people out of mailboxes they can open today is a worse outcome than the weak password it would be correcting. Tightening these settings therefore affects the next password each person sets, not the one they have. Everything is off until you turn it on, so an upgrade never starts refusing passwords on its own."));
         policy.Settings.Add(new IniNumber
         {
            Path = "PasswordPolicyMinimumLength",
            Label = L("Minimum length (0 = no minimum)"),
            Default = 0,
            Blurb = L("Length is the requirement that buys the most, and the one people mind least."),
            IniStore = iniStore_
         });
         policy.Settings.Add(new IniBool
         {
            Path = "PasswordPolicyRequireMixedCase",
            Label = L("Require both upper and lower case"),
            IniStore = iniStore_
         });
         policy.Settings.Add(new IniBool
         {
            Path = "PasswordPolicyRequireDigit",
            Label = L("Require at least one digit"),
            IniStore = iniStore_
         });
         policy.Settings.Add(new IniBool
         {
            Path = "PasswordPolicyRequireNonAlphanumeric",
            Label = L("Require at least one character that is not a letter or digit"),
            Blurb = L("Anything that is not a letter or a digit counts, rather than a fixed list of punctuation, so a keyboard layout you do not have cannot make the rule unsatisfiable."),
            IniStore = iniStore_
         });
         policy.Settings.Add(new IniBool
         {
            Path = "PasswordPolicyRejectCommon",
            Label = L("Reject the most commonly used passwords"),
            Blurb = L("A short built-in list - \"password\", \"123456\", \"changeme\" and about thirty more - compared without regard to case. It is deliberately not a \"top 10000\" list, which would need a data file and an update path; it catches the passwords that get typed while setting up a mailbox nobody comes back to."),
            IniStore = iniStore_
         });
         Tab(L("Password")).Cards.Add(policy);

         // The breach check, beside the policy it extends and before the history,
         // because it is another rule about the password chosen rather than about the
         // passwords before it. In hm_settings, unlike the policy above - which is why
         // these rows are COM properties and those are INI rows.
         var breach = Card(L("Passwords found in published breaches"),
            L("Checks a new password against the published breach corpus when it is chosen - here, in the account editor, in the webmail, by a script or over the REST API - and refuses one that has appeared in it. Only the first five characters of the password's SHA-1 hash leave this server; the endpoint answers with every hash that begins with them, padded so that the size of the answer says nothing either, and the comparison is made here. It is never applied at logon, so a password that works today keeps working. Off by default, because it sends something off the server."));
         breach.Settings.Add(new ComBool
         {
            Path = "BreachCheckEnabled",
            Label = L("Refuse a new password found in published data breaches")
         });
         var breachUrl = new ComText
         {
            Path = "BreachCheckUrl",
            Label = L("Range endpoint (https, or http to this machine)"),
            Blurb = L("The public corpus is https://api.pwnedpasswords.com/range/. An organisation that runs its own mirror names it here: the five characters are appended to the path, or put where {prefix} is written. Plain http is accepted only to localhost, 127.0.0.1 or ::1.")
         };
         breach.Settings.Add(breachUrl);
         breach.Settings.Add(new ComText
         {
            Path = "BreachCheckMinimumCount",
            Label = L("Refuse when it has appeared at least this many times"),
            Numeric = true,
            Blurb = L("1 refuses a password that has appeared in the corpus even once.")
         });
         breach.Settings.Add(new ComCombo
         {
            Path = "BreachCheckUnreachableAction",
            Label = L("When the endpoint cannot be reached"),
            Options = ToOptions(SettingClaims.BreachCheckUnreachableOptions),
            Blurb = L("Refusing is stricter, but while the endpoint cannot be reached no password can be changed anywhere, the administrator password included.")
         });
         breach.Settings.Add(new ComAction
         {
            ButtonText = L("Test the endpoint…"),
            Action = () => TestBreachCheckEndpoint(breachUrl.CurrentText)
         });
         Tab(L("Password")).Cards.Add(breach);

         // Separate from the complexity card above, because the two carry very
         // different risks and putting them under one heading would hide that.
         var ageing = Card(L("Reuse and expiry"),
            L("These belong together: expiry without history teaches people to alternate between two passwords, which is worse than not expiring at all. History only ever refuses a CHANGE, at the moment somebody is able to pick something else, so nobody is locked out by it. Expiry refuses a LOGON - read its description before turning it on."));
         ageing.Settings.Add(new IniNumber
         {
            Path = "PasswordPolicyHistoryCount",
            Label = L("Remember this many previous passwords (0 = no history)"),
            Default = 0,
            Blurb = L("The current password counts as the most recent one, so setting a password to itself is refused too - the repeat somebody is most likely to try."),
            IniStore = iniStore_
         });
         ageing.Settings.Add(new IniNumber
         {
            Path = "PasswordPolicyMaximumAgeDays",
            Label = L("Expire passwords after (days; 0 = never)"),
            Default = 0,
            Blurb = L("READ THIS FIRST: IMAP, POP3 and SMTP have no way to change a password, and the self-service portal refuses an expired one at sign-in like every other surface. An expired password is therefore renewed by an administrator - or by the person themselves only if they hold an app password, which still signs them in to the portal, where the expired password is accepted as the current one when setting a new one. Turning this on means someone has to be available for everybody else. Existing app passwords keep working, which is also the one way an affected person can still collect mail; Active Directory accounts are exempt, because their password lives in the directory and expires by its policy, not this one. The clock started when this server was upgraded, so nobody is expired the moment you set this."),
            IniStore = iniStore_
         });
         Tab(L("Password")).Cards.Add(ageing);

         // The one the server enforces, first because it is the one that matters:
         // it protects the credential itself on every surface, not just this tool.
         var serverTwoFactor = Card(L("Second factor on the administrator credential (recommended)"),
            L("A one-time code required in addition to the administrator password, checked by the SERVER - so it applies to every client of that credential: this Control Panel, scripts using the COM API, and the REST API (which reads the code from an X-hMailServer-OTP header). The secret is stored on the server, in hMailServer.ini, machine-protected. It makes the administrator password worthless on its own. Keep the setup key safe: if the authenticator is lost, a local administrator clears AdministratorTotpSecret from hMailServer.ini to recover."));
         serverTwoFactor.Settings.Add(new ComAction
         {
            ButtonText = L("Set up or turn off the server-enforced second factor…"),
            Action = () =>
            {
               new AdministratorTwoFactorDialog(Application.Current?.MainWindow).ShowDialog();
               bool on = false;
               try { on = (bool)ServerSession.Current.Application.AdministratorTOTPEnabled; }
               catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck)) { /* Leave the summary reading "off" if the state cannot be read back. */ }
               return on
                  ? (true, L("A second factor is enforced on the administrator credential, on every client."))
                  : (false, L("No second factor is enforced - the administrator password alone signs in."));
            }
         });
         Tab(L("Two-factor")).Cards.Add(serverTwoFactor);

         var twoFactor = Card(L("Two-factor for this Control Panel only"),
            L("An older, weaker option: a one-time code asked for after the password when signing in to THIS Control Panel, checked by the tool itself after the server has already accepted the password. The secret is stored per machine under HKLM, so enabling or disabling it needs local administrator rights, and it does nothing for the REST API or for scripts using the COM API. Prefer the server-enforced factor above; this one remains for existing setups."));
         twoFactor.Settings.Add(new ComAction
         {
            ButtonText = L("Set up or turn off Control-Panel-only two-factor authentication…"),
            Action = () =>
            {
               new TotpSetupDialog(Application.Current?.MainWindow).ShowDialog();
               return TotpManager.IsConfigured()
                  ? (true, L("Control-Panel-only two-factor authentication is on."))
                  : (false, L("Control-Panel-only two-factor authentication is off."));
            }
         });
         Tab(L("Two-factor")).Cards.Add(twoFactor);
      }

      // ---- COM resolution ----------------------------------------------------

      private static object ResolveOwner(string path, out string property)
      {
         dynamic current = ServerSession.Current.Application.Settings;
         string[] parts = path.Split('.');
         for (int i = 0; i < parts.Length - 1; i++)
            current = GetProperty(current, parts[i]);
         property = parts[^1];
         return current;
      }

      private static object GetProperty(object owner, string name)
         => owner.GetType().InvokeMember(name, BindingFlags.GetProperty, null, owner, null);

      private static void SetProperty(object owner, string name, object value)
         => owner.GetType().InvokeMember(name, BindingFlags.SetProperty, null, owner, new[] { value });

      // ---- UI ----------------------------------------------------------------

      /// <summary>
      /// Works out what a screen reader should call each editor, before any of
      /// them are built.
      ///
      /// It has to be done for the whole page at once, which is why it is not
      /// simply part of building an editor: on the Protocols page "Max simultaneous
      /// connections (0 = unlimited)" and "Welcome banner (empty = default)" each
      /// appear three times, once per protocol card, and only a pass over the page
      /// can tell which labels need qualifying with their card title.
      /// <see cref="AccessibleNames"/> holds the decision.
      /// </summary>
      private void AssignAccessibleNames()
      {
         var settings = new List<ComSetting>();
         var editors = new List<LabelledEditor>();

         foreach (TabDef tab in tabs_)
         {
            foreach (CardDef card in tab.Cards)
            {
               foreach (ComSetting setting in card.Settings)
               {
                  settings.Add(setting);
                  editors.Add(new LabelledEditor(setting.Label, card.Title, setting.Path));
               }
            }
         }

         IReadOnlyList<string> names = AccessibleNames.Resolve(editors);
         for (int i = 0; i < settings.Count; i++)
            settings[i].AccessibleName = names[i];
      }

      private void BuildUi()
      {
         SettingsTabs.Items.Clear();
         diag_ = null;
         failedReads_ = 0;
         building_ = true;

         AssignAccessibleNames();

         // The settings this page reaches over the REST API, read group by group
         // before any editor is built, so that no editor is filled with a default.
         LoadRestGroups_();

         foreach (TabDef tab in tabs_)
         {
            // Cap the settings form to a readable column instead of letting cards
            // stretch the full window width (which left short, left-aligned inputs
            // floating in empty space). The right inset is the room the tab's own
            // scroll bar takes, so that no note runs underneath it.
            var panel = new StackPanel
            {
               MaxWidth = DesignTokens.FormWidth,
               HorizontalAlignment = HorizontalAlignment.Left,
               Margin = new Thickness(0, DesignTokens.Space.Xs, DesignTokens.Space.Md, DesignTokens.Space.Xs)
            };

            foreach (CardDef card in tab.Cards)
            {
               var fields = new StackPanel();

               Scaffold.FieldRow lastRow = null;

               // Why this card's REST rows are not shown, when they are not: said
               // once, at the top of the card, in place of them.
               string restProblem = null;

               foreach (ComSetting setting in card.Settings)
               {
                  object value = null;
                  if (setting is IRestSetting rest)
                  {
                     if (!restGroups_.TryGetValue(rest.Group, out RestGroup group) || group.Values == null)
                     {
                        restProblem ??= group?.Refusal ?? L("These settings could not be read from the server.");
                        continue;
                     }

                     setting.RestEnvironment = RestSettings.ValueOf(group.Values, rest.Key + "_environment") as string; // no-loc

                     if (setting.WantsInitialValue)
                     {
                        // A key the server does not answer is not shown with a
                        // default: an older server, which has no such setting.
                        if (!group.Values.ContainsKey(rest.Key))
                        {
                           failedReads_++;
                           diag_ ??= F("The server does not answer {0} in {1}; it may be older than this Control Panel.", rest.Key, rest.Group);
                           continue;
                        }

                        object answered = RestSettings.ValueOf(group.Values, rest.Key);
                        rest.Loaded(answered);
                        value = rest.ToEditor(answered);
                     }
                  }
                  else if (setting.WantsInitialValue)
                  {
                     try
                     {
                        object owner = ResolveOwner(setting.Path, out string property);
                        value = setting.Fetch(owner, property);
                     }
                     catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
                     {
                        failedReads_++;
                        diag_ ??= ServerSession.DescribeComError(ex);
                        continue;   // value could not be read; skip this editor
                     }
                  }

                  setting.Changed = MarkUnsaved_;
                  lastRow = setting.CreateRow(value);
                  fields.Children.Add(lastRow);
               }

               // The last field's own gap would fall inside the card's padding and
               // make that card taller than the one beside it for no reason.
               if (lastRow != null)
                  lastRow.Margin = new Thickness(0);

               if (restProblem != null)
               {
                  fields.Children.Insert(0, new Scaffold.InlineNotice
                  {
                     Level = StatusLevel.Warning,
                     Text = restProblem,
                     Margin = new Thickness(0, 0, 0, lastRow != null ? DesignTokens.Space.Md : 0)
                  });
               }

               // The card is the surface; the section inside it is the heading, the
               // description and the fields - and a level-2 heading in the
               // automation tree, which is how a screen reader walks a page of a
               // hundred and fifty settings without reading every one of them. Its
               // own section gap is cleared: the card's padding is the gap here.
               panel.Children.Add(new Scaffold.Card
               {
                  Margin = new Thickness(0, 0, 0, DesignTokens.Space.Md),
                  Content = new Scaffold.SettingsSection
                  {
                     Heading = card.Title,
                     Description = card.Blurb,
                     Margin = new Thickness(0),
                     Content = fields
                  }
               });
            }

            var scroll = new ScrollViewer
            {
               VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
               Content = panel
            };
            SettingsTabs.Items.Add(new TabItem { Header = tab.Header, Content = scroll });
         }

         if (SettingsTabs.Items.Count > 0)
            SettingsTabs.SelectedIndex = 0;

         // A page with one tab is a page, not a tab: the strip with a single
         // underlined header is hidden. Collapsing the item hides its header
         // only; the TabControl still presents the selected item's content.
         if (SettingsTabs.Items.Count == 1 && SettingsTabs.Items[0] is TabItem only)
            only.Visibility = Visibility.Collapsed;

         // A page that read cleanly has nothing to say - no pill, no notice. One
         // that could not read some of its values says how many and why, and says
         // it at the top of the page rather than on a line under the fold of a
         // long tab, where it was before.
         ShowState_(ServerSettingsPageState.Clean);

         // And the settings the server's environment sets, every one by its
         // variable: a field is marked where this program can tie it to its setting,
         // and this sentence names them all, so none of them is a surprise at save.
         string readSentence = ServerSettingsPageStatus.ReadSentence(failedReads_, diag_);
         string environmentSentence = EnvironmentSettings.Notice(IniFeatureStore.EnvironmentManaged());
         if (!string.IsNullOrEmpty(readSentence))
            ShowNotice_(string.IsNullOrEmpty(environmentSentence) ? readSentence : readSentence + " " + environmentSentence, StatusLevel.Critical);
         else
            ShowNotice_(environmentSentence, StatusLevel.Information);

         afterBuildUi_?.Invoke();

         // The editors were filled above, and a WPF-UI NumberBox raises
         // ValueChanged while its template is applied - which happens after this
         // method returns. ContextIdle runs below every priority that layout and
         // loading use, so the guard is released only once the page has settled
         // and the page does not announce unsaved changes before anybody has
         // typed anything.
         Dispatcher.BeginInvoke(DispatcherPriority.ContextIdle, new Action(() => building_ = false));
      }

      /// <summary>
      /// An editor changed. The pill is the only thing on the page that says what
      /// is on screen is no longer what the server holds, so it is worth saying at
      /// the first keystroke and not worth saying twice.
      /// </summary>
      private void MarkUnsaved_()
      {
         if (building_ || state_ == ServerSettingsPageState.Unsaved)
            return;

         ShowState_(ServerSettingsPageState.Unsaved);
      }

      /// <summary>
      /// Puts the page's state on the pill beside the title. The notice under the
      /// header is separate on purpose: "27 settings could not be read" is still
      /// true after the first keystroke, and a page that dropped that warning the
      /// moment somebody typed would be hiding it at exactly the point it starts
      /// to matter. Only a reload or a save replaces it.
      /// </summary>
      private void ShowState_(ServerSettingsPageState state)
      {
         state_ = state;

         StatePill.Level = ServerSettingsPageStatus.LevelFor(state);
         StatePill.Text = ServerSettingsPageStatus.WordFor(state);
         StatePill.Visibility = ServerSettingsPageStatus.ShowsPill(state) ? Visibility.Visible : Visibility.Collapsed;
      }

      /// <summary>
      /// The sentence under the header, or nothing when there is nothing to say -
      /// which is the ordinary case, and is why a page that read cleanly shows no
      /// notice at all rather than a line confirming that nothing went wrong.
      /// </summary>
      private void ShowNotice_(string sentence, StatusLevel level)
      {
         if (string.IsNullOrEmpty(sentence))
         {
            StatusNotice.Visibility = Visibility.Collapsed;
            return;
         }

         StatusNotice.Level = level;
         StatusNotice.Text = sentence;
         StatusNotice.Visibility = Visibility.Visible;
      }

      // ---- live test / dependency helpers ------------------------------------

      /// <summary>
      /// Settings.TestBreachCheckEndpoint against what is typed in the endpoint box,
      /// saved or not: the request is for the prefix 00000, which is no password's.
      /// The reason for a failure is the server's, translated when it is a sentence
      /// the catalogue holds.
      /// </summary>
      private static (bool ok, string text) TestBreachCheckEndpoint(string url)
      {
         dynamic settings = ServerSession.Current.Application.Settings;
         try
         {
            object entries = ((object)settings).GetType().InvokeMember(
               "TestBreachCheckEndpoint", BindingFlags.InvokeMethod, null, (object)settings, new object[] { url ?? "" });
            return (true, F("The endpoint answered with {0} entries.", Convert.ToInt32(entries)));
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            return (false, F("The endpoint could not be used: {0}", L(ServerSentences.MessageOf(ex))));
         }
         finally
         {
            ServerSession.Release((object)settings);
         }
      }

      private static (bool ok, string text) TestSpamAssassin(string host, int port)
      {
         if (string.IsNullOrWhiteSpace(host) || port <= 0)
            return (false, L("Enter a host name and port first."));

         dynamic antispam = ServerSession.Current.Application.Settings.AntiSpam;
         try
         {
            object[] args = { host, port, "" };
            object ret = ((object)antispam).GetType().InvokeMember(
               "TestSpamAssassinConnection", BindingFlags.InvokeMethod, null, (object)antispam, args);
            bool ok = ret is bool b && b;
            string msg = args.Length > 2 ? args[2] as string : null;
            if (string.IsNullOrEmpty(msg))
               msg = ok ? L("Connection succeeded.") : L("Connection failed.");
            return (ok, msg);
         }
         finally
         {
            ServerSession.Release((object)antispam);
         }
      }

      // ---- live cache statistics ---------------------------------------------

      /// <summary>
      /// One line per cache: hit rate, memory in use, and its cap.
      ///
      /// Two things are stated rather than left to be inferred, because both would
      /// otherwise be read wrongly. Cache&lt;T&gt;::GetHitRate returns 0 when there
      /// have been no hits at all, which covers both "every lookup missed" and "no
      /// lookup has happened" - so a cache with no memory in use is called out as
      /// idle instead of as failing. And the counters are reset by SetTTL and by
      /// SetEnabled, not only by a restart, so the sample after any settings change
      /// on this page starts from zero.
      /// </summary>
      private static string DescribeCaches_()
      {
         dynamic cache = ServerSession.Current.Application.Settings.Cache;
         try
         {
            if (!(bool)cache.Enabled)
            {
               return L("Caching is switched off, so every domain, account, alias and distribution-list lookup goes to the database. There are no hit rates to report.");
            }

            var lines = new List<string>();

            foreach ((string name, string rateProperty, string sizeProperty, string capProperty) in new[]
            {
               (L("Domains"), "DomainHitRate", "DomainCacheSizeKb", "DomainCacheMaxSizeKb"),
               (L("Accounts"), "AccountHitRate", "AccountCacheSizeKb", "AccountCacheMaxSizeKb"),
               (L("Aliases"), "AliasHitRate", "AliasCacheSizeKb", "AliasCacheMaxSizeKb"),
               (L("Distribution lists"), "DistributionListHitRate", "DistributionListCacheSizeKb", "DistributionListCacheMaxSizeKb")
            })
            {
               int rate = (int)ComProperty_(cache, rateProperty);
               int sizeKb = (int)ComProperty_(cache, sizeProperty);
               int capKb = (int)ComProperty_(cache, capProperty);

               string line = name + ": ";

               if (rate == 0 && sizeKb == 0)
               {
                  // "Nothing is cached" is what this state proves. It is NOT proof
                  // that nothing has been looked up: a lookup that found no such
                  // domain or account counts as a miss and caches nothing, so a
                  // server being probed for addresses it does not host sits here
                  // with plenty of traffic. Saying "no lookup has been made" would
                  // read as "this server is idle", which is the opposite of what
                  // that traffic pattern means.
                  line += L("nothing is cached - either nothing has been looked up since the counters were reset, or the lookups that happened found nothing to cache.");
               }
               else
               {
                  line += F("{0}% of lookups answered from memory, using {1} KB", rate, sizeKb.ToString("N0", System.Globalization.CultureInfo.CurrentCulture));

                  if (capKb > 0)
                  {
                     line += F(" of {0} KB", capKb.ToString("N0", System.Globalization.CultureInfo.CurrentCulture));
                     if (sizeKb >= capKb)
                        line += L(" - at its cap, so entries are being evicted");
                  }

                  line += ".";
               }

               lines.Add(line);
            }

            return string.Join("\r\n", lines);
         }
         finally
         {
            ServerSession.Release((object)cache);
         }
      }

      /// <summary>
      /// Reads one property off a COM object by name. Late-bound because the four
      /// caches differ only by the property name, and writing sixteen dynamic reads
      /// out longhand is where a copy-paste error puts the domain cache's hit rate
      /// beside the account cache's size.
      /// </summary>
      private static object ComProperty_(object owner, string name)
         => owner.GetType().InvokeMember(name, BindingFlags.GetProperty, null, owner, null);

      /// <summary>
      /// The message store's last measurement, from Settings.MessageStoreStatus
      /// (the JSON GET /api/v1/store answers): what the messages are beside what
      /// their files and the parts held once take, so that a saving is read off
      /// the server rather than assumed from the switch.
      /// </summary>
      private static string DescribeStore_()
      {
         dynamic settings = ServerSession.Current.Application.Settings;
         string json = (string)settings.MessageStoreStatus;

         using (System.Text.Json.JsonDocument document = System.Text.Json.JsonDocument.Parse(json))
         {
            System.Text.Json.JsonElement root = document.RootElement;

            long measured = root.GetProperty("measured").GetInt64();
            if (measured == 0)
               return L("The server is measuring the message store; open this page again in a minute for the figures.");

            var culture = System.Globalization.CultureInfo.CurrentCulture;
            string MegaBytes(string name) => (root.GetProperty(name).GetInt64() / 1048576.0).ToString("N1", culture);

            return F("{0} messages, {1} MB as clients see them. Their files take {2} MB and the parts held once {3} MB, which saves {4}%. {5} of the files are compressed. Measured {6}.",
               root.GetProperty("messages").GetInt64().ToString("N0", culture),
               MegaBytes("logical_bytes"),
               MegaBytes("stored_bytes"),
               MegaBytes("blob_bytes"),
               root.GetProperty("saved_percent").GetDouble().ToString("N1", culture),
               root.GetProperty("compressed_files").GetInt64().ToString("N0", culture),
               DateTimeOffset.FromUnixTimeSeconds(measured).LocalDateTime.ToString("g", culture));
         }
      }

      // ---- message index actions ---------------------------------------------
      //
      // MessageIndexing.Index() and .Clear() existed over COM and nowhere else, so
      // the two questions an administrator actually arrives with - "search finds
      // nothing" and "search finds messages that are gone" - had no answer short of
      // writing a script. Both actions report the counts on either side of the run
      // rather than only "done", because the counts ARE the answer: an index behind
      // the mailbox and an index ahead of it are different faults with different
      // fixes, and neither is visible from a success message.

      /// <summary>Total delivered messages and total indexed, or null when unreadable.</summary>
      private static (long Messages, long Indexed)? ReadIndexCounts_()
      {
         dynamic indexing = ServerSession.Current.Application.Settings.MessageIndexing;
         try
         {
            return ((long)(int)indexing.TotalMessageCount, (long)(int)indexing.TotalIndexedCount);
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            return null;
         }
         finally
         {
            ServerSession.Release((object)indexing);
         }
      }

      private static string DescribeIndexCounts_((long Messages, long Indexed)? counts)
      {
         if (counts == null)
            return "";

         long messages = counts.Value.Messages;
         long indexed = counts.Value.Indexed;

         string text = F(" {0} of {1} messages are indexed.",
            indexed.ToString("N0", System.Globalization.CultureInfo.CurrentCulture),
            messages.ToString("N0", System.Globalization.CultureInfo.CurrentCulture));

         if (indexed < messages)
            text += L(" The rest are still waiting; run this again or leave the schedule to catch up.");
         else if (indexed > messages)
            text += L(" There are more index entries than messages, which means entries for mail that no longer exists - discard and rebuild the index to clear them.");

         return text;
      }

      /// <summary>
      /// Whether the indexer is switched on, which decides whether either action
      /// below does anything at all.
      ///
      /// MessageIndexer's worker thread is started only when indexing is enabled
      /// (Application::Start, and Configuration::SetMessageIndexing). IndexNow()
      /// does not index - it sets an event that worker waits on - so with indexing
      /// off, "Index" signals a thread that is not running and reports success
      /// having done nothing. Saying so is the whole point of checking.
      /// </summary>
      private static bool IndexingEnabled_()
      {
         dynamic indexing = ServerSession.Current.Application.Settings.MessageIndexing;
         try
         {
            return (bool)indexing.Enabled;
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            // Unknown rather than off: refusing to act on a read that failed would
            // be worse than letting the attempt report its own error.
            return true;
         }
         finally
         {
            ServerSession.Release((object)indexing);
         }
      }

      private static string IndexingOffNote =>
         L("Message indexing is switched off, so there is no indexer running to ask. Tick \"Enable message indexing\" above and save first - nothing was done.");

      private static (bool ok, string text) RunIndexNow_()
      {
         if (!IndexingEnabled_())
            return (false, IndexingOffNote);

         dynamic indexing = ServerSession.Current.Application.Settings.MessageIndexing;
         try
         {
            indexing.Index();
         }
         finally
         {
            ServerSession.Release((object)indexing);
         }

         // The counts are read after signalling but describe the state BEFORE the
         // pass, because the pass has not happened yet: IndexNow() sets an event and
         // returns, and the worker picks it up. Reporting them as "now" would be a
         // lie that looks like a stuck indexer, so the wording says which they are.
         return (true, L("The indexer has been asked to run; it works in the background and the counts below will not move until it has. As things stand:") + DescribeIndexCounts_(ReadIndexCounts_()));
      }

      private static (bool ok, string text) RebuildIndex_()
      {
         if (!IndexingEnabled_())
            return (false, IndexingOffNote);

         (long Messages, long Indexed)? before = ReadIndexCounts_();

         string warning = L("Discard the search index and rebuild it?\r\n\r\nNo mail is touched - the index is derived from it - but until the rebuild catches up, IMAP SEARCH will find less than it should.");

         if (before != null)
         {
            warning += "\r\n\r\n" + F("{0} index entries will be discarded and rebuilt from {1} messages.",
               before.Value.Indexed.ToString("N0", System.Globalization.CultureInfo.CurrentCulture),
               before.Value.Messages.ToString("N0", System.Globalization.CultureInfo.CurrentCulture));
         }

         if (MessageBox.Show(warning, L("Control Panel"), MessageBoxButton.YesNo, MessageBoxImage.Warning)
             != MessageBoxResult.Yes)
         {
            return (true, L("Nothing was changed."));
         }

         dynamic indexing = ServerSession.Current.Application.Settings.MessageIndexing;
         try
         {
            // Clear() is a DELETE and takes effect immediately; Index() only asks
            // the worker to start rebuilding. So the index really is empty when
            // this returns, and the rebuild really has not happened.
            indexing.Clear();
            indexing.Index();
         }
         finally
         {
            ServerSession.Release((object)indexing);
         }

         return (true, L("The index has been emptied and the indexer asked to rebuild it. The rebuild runs in the background, in passes bounded by the limits above, so on a large mailstore it will take several passes - IMAP SEARCH finds less than it should until it finishes."));
      }

      /// <summary>
      /// Utilities.PerformMaintenance(eUpdateIMAPFolderUID), which had no GUI at all.
      ///
      /// What it repairs, from Maintenance::RecalculateFolderUID_: for every IMAP
      /// folder it sets foldercurrentuid to the highest message UID the folder
      /// actually holds, and only where the stored value is LOWER. A folder whose
      /// counter has fallen behind its messages hands the same UID out twice, and an
      /// IMAP client that caches by UID then shows one message in place of another -
      /// a data-integrity symptom with no obvious cause, and the reason this repair
      /// exists. It cannot lower a counter, so it cannot cause the fault it fixes.
      /// </summary>
      private static (bool ok, string text) RecalculateFolderUids_()
      {
         if (MessageBox.Show(
                L("Recalculate the IMAP folder UID counters?\r\n\r\nFor every folder, the counter is moved up to the highest message UID that folder actually holds. Counters that are already correct or ahead are left alone, and no message is changed.\r\n\r\nRun this if clients report messages appearing under the wrong UID, or after restoring a database."),
                L("Control Panel"), MessageBoxButton.YesNo, MessageBoxImage.Question) != MessageBoxResult.Yes)
         {
            return (true, L("Nothing was changed."));
         }

         dynamic utilities = ServerSession.Current.Application.Utilities;
         try
         {
            // eUpdateIMAPFolderUID = 1. The only operation the enum defines; the
            // server answers "Unknown maintenance operation" for anything else.
            ((object)utilities).GetType().InvokeMember(
               "PerformMaintenance", BindingFlags.InvokeMethod, null, (object)utilities, new object[] { 1 });
         }
         finally
         {
            ServerSession.Release((object)utilities);
         }

         return (true, L("The folder UID counters have been recalculated. Clients that cached the old values will resynchronise on their next connection."));
      }

      // ---- anti-virus scanner test / preset helpers --------------------------

      // Common single-engine custom-scanner command lines. Exit codes vary by
      // product version, so the preset is a starting point the admin can adjust.
      private static readonly (string Name, string Exe, int ReturnValue)[] CustomScannerPresets =
      {
         ("Microsoft Defender (MpCmdRun)", // no-loc: product names and their command lines
            "\"C:\\Program Files\\Windows Defender\\MpCmdRun.exe\" -Scan -ScanType 3 -File \"%FILE%\" -DisableRemediation", 2), // no-loc
         ("Sophos (savscan)", // no-loc
            "\"C:\\Program Files\\Sophos\\Sophos Anti-Virus\\savscan.exe\" -ss -archive \"%FILE%\"", 3), // no-loc
         ("ESET (ecls)", // no-loc
            "\"C:\\Program Files\\ESET\\ESET Security\\ecls.exe\" \"%FILE%\"", 50), // no-loc
         ("Bitdefender (bdscan)", // no-loc
            "\"C:\\Program Files\\Bitdefender\\Endpoint Security\\bdscan.exe\" \"%FILE%\"", 1), // no-loc
         ("Kaspersky (avp.com)", // no-loc
            "\"C:\\Program Files (x86)\\Kaspersky Lab\\Kaspersky Endpoint Security\\avp.com\" SCAN \"%FILE%\"", 2), // no-loc
      };

      private static int ParsePort(string text) => int.TryParse(text, out int p) ? p : 0;

      private static (bool ok, string text) TestClamAv(string host, int port)
      {
         if (string.IsNullOrWhiteSpace(host) || port <= 0)
            return (false, L("Enter a host name and port first."));

         dynamic av = ServerSession.Current.Application.Settings.AntiVirus;
         try
         {
            object[] args = { host, port, "" };
            object ret = ((object)av).GetType().InvokeMember(
               "TestClamAVScanner", BindingFlags.InvokeMethod, null, (object)av, args);
            bool ok = ret is bool b && b;
            string msg = args.Length > 2 ? args[2] as string : null;
            if (string.IsNullOrEmpty(msg))
               msg = ok ? L("Connection succeeded.") : L("Connection failed.");
            return (ok, msg);
         }
         finally
         {
            ServerSession.Release((object)av);
         }
      }

      private static (bool ok, string text) TestClamWin(string executable, string database)
      {
         if (string.IsNullOrWhiteSpace(executable))
            return (false, L("Enter the clamscan.exe path first."));

         dynamic av = ServerSession.Current.Application.Settings.AntiVirus;
         try
         {
            object[] args = { executable, database ?? "", "" };
            object ret = ((object)av).GetType().InvokeMember(
               "TestClamWinScanner", BindingFlags.InvokeMethod, null, (object)av, args);
            bool ok = ret is bool b && b;
            string msg = args.Length > 2 ? args[2] as string : null;
            if (string.IsNullOrEmpty(msg))
               msg = ok ? L("Scanner test succeeded.") : L("Scanner test failed.");
            return (ok, msg);
         }
         finally
         {
            ServerSession.Release((object)av);
         }
      }

      private static (bool ok, string text) TestCustomScanner(string command)
      {
         if (string.IsNullOrWhiteSpace(command))
            return (false, L("Enter the scanner command first."));

         string path = ExtractProgramPath(command);
         if (string.IsNullOrEmpty(path))
            return (false, L("Couldn't determine the executable from the command."));

         if (System.IO.File.Exists(path))
            return (true, F("Executable found: {0}", path));

         return (false, F("Executable not found: {0}", path));
      }

      // Pulls the program path out of a command line that may be quoted and carry
      // arguments / the %FILE% macro.
      private static string ExtractProgramPath(string command)
      {
         command = command.Trim();
         if (command.StartsWith("\""))
         {
            int end = command.IndexOf('"', 1);
            return end > 1 ? command.Substring(1, end - 1) : "";
         }
         int space = command.IndexOf(' ');
         return space < 0 ? command : command.Substring(0, space);
      }

      private static (bool ok, string text) AutoDetectClamWin(ComText exeField, ComText dbField)
      {
         string[] exeCandidates =
         {
            @"C:\Program Files\ClamWin\bin\clamscan.exe", // no-loc
            @"C:\Program Files (x86)\ClamWin\bin\clamscan.exe", // no-loc
         };

         foreach (string candidate in exeCandidates.Where(System.IO.File.Exists))
         {
            exeField.SetText(candidate);
            string db = FindClamWinDatabase();
            if (!string.IsNullOrEmpty(db))
               dbField.SetText(db);
            return (true, F("Found ClamWin at {0}", candidate) +
               (string.IsNullOrEmpty(db) ? "" : F("; database folder {0}", db)));
         }

         return (false, L("ClamWin was not found in the standard install locations."));
      }

      private static string FindClamWinDatabase()
      {
         string[] candidates =
         {
            System.IO.Path.Join(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), @"ClamWin\db"),
            @"C:\ProgramData\ClamWin\db",
         };

         return candidates.FirstOrDefault(System.IO.Directory.Exists) ?? "";
      }


      /// <summary>
      /// Makes the log-format precedence visible.
      ///
      /// Logger::Render_ tests the NCSA format before it looks at JsonLogging, so
      /// with NCSA selected the JSON switch does nothing - and until now the
      /// Logging page offered both, side by side, with no hint that one silently
      /// beat the other. That is the same defect as a setting the server ignores
      /// entirely: the administrator ticks a box, gets no error, and does not get
      /// JSON logs.
      ///
      /// The checkbox is disabled rather than cleared, so the stored INI value is
      /// left alone: a disabled CheckBox keeps its IsChecked state, SaveToIni
      /// writes that state back unchanged, and switching the format away from NCSA
      /// restores the JSON logging the administrator had configured. Clearing it
      /// would silently discard a setting on a page visit.
      /// </summary>
      private static void WireLogFormatDependency(ComCombo format, IniBool json)
      {
         if (format == null || json == null || format.Combo == null || json.Box == null)
            return;

         void Update()
         {
            bool overridden = format.SelectedValue == SettingClaims.LogFormatNcsa;

            json.Box.IsEnabled = !overridden;
            json.Box.ToolTip = overridden ? L(SettingClaims.JsonOverriddenByNcsa) : null;

            // Kept on the checkbox itself as well as in the tool tip: a tool tip
            // needs a hover, and a disabled control that gives no reason for being
            // disabled is indistinguishable from a broken page.
            System.Windows.Automation.AutomationProperties.SetHelpText(json.Box,
               overridden ? L(SettingClaims.JsonOverriddenByNcsa) : "");
         }

         format.Combo.SelectionChanged += (s, e) => Update();
         Update();
      }

      private static void WireChaChaDependency(ComBool preferServer, ComBool chacha, ComBool tls12, ComBool tls13)
      {
         if (preferServer == null || chacha == null || tls12 == null || tls13 == null)
            return;
         if (preferServer.Box == null || chacha.Box == null || tls12.Box == null || tls13.Box == null)
            return;

         void Update()
         {
            bool eligible = preferServer.Box.IsChecked is true
               && (tls12.Box.IsChecked is true || tls13.Box.IsChecked is true);
            chacha.Box.IsEnabled = eligible;
            chacha.Box.ToolTip = eligible
               ? null
               : L("Requires 'Prefer server cipher order' and TLS 1.2 or 1.3 to be enabled.");
         }

         void Handler(object s, RoutedEventArgs e) => Update();
         preferServer.Box.Checked += Handler;
         preferServer.Box.Unchecked += Handler;
         tls12.Box.Checked += Handler;
         tls12.Box.Unchecked += Handler;
         tls13.Box.Checked += Handler;
         tls13.Box.Unchecked += Handler;
         Update();
      }

      private void Reload_Click(object sender, RoutedEventArgs e)
      {
         restAskAgain_ = true;
         BuildUi();
      }

      /// <summary>
      /// Reads every REST settings group this page's rows are in, once, through the
      /// Control Panel's REST session (Services/RestSettingsClient) - signing it in
      /// first, and asking for the administrator's one-time code if the server wants
      /// one. A group that cannot be read keeps the reason, which its cards show.
      /// </summary>
      private void LoadRestGroups_()
      {
         restGroups_.Clear();

         List<string> groups = tabs_.SelectMany(tab => tab.Cards).SelectMany(card => card.Settings)
            .OfType<IRestSetting>().Select(row => row.Group).Distinct(StringComparer.Ordinal).ToList();
         if (groups.Count == 0)
            return;

         ServerSession session = ServerSession.Current;
         RestSettingsClient client = session == null ? null : RestSettingsClient.For(session);

         string refusal = null;
         if (client == null)
            refusal = L("Sign in to a server first.");
         else
            client.Open(AskForCode_, restAskAgain_, out refusal);
         restAskAgain_ = false;

         foreach (string group in groups)
         {
            if (refusal != null)
            {
               restGroups_[group] = new RestGroup { Refusal = refusal };
               continue;
            }

            RestSettingsClient.Reply reply = client.Get(group, AskForCode_);
            if (!reply.Ok)
            {
               restGroups_[group] = new RestGroup { Refusal = client.RefusalFor(reply) };

               // No answer at all - a port a firewall drops, a listener that took the
               // connection and said nothing: the groups after it are not asked, each
               // of which would wait out the same timeout while the page waits, and
               // they say the same.
               if (reply.Status == 0)
                  refusal = restGroups_[group].Refusal;
               continue;
            }

            IReadOnlyDictionary<string, System.Text.Json.JsonElement> values = RestSettings.ParseGroup(reply.Body, out string error);
            restGroups_[group] = values == null ? new RestGroup { Refusal = error } : new RestGroup { Values = values };
         }
      }

      /// <summary>The administrator's one-time code, for the REST session; null when none was given.</summary>
      private string AskForCode_()
      {
         var prompt = new TotpPromptDialog(Window.GetWindow(this) ?? Application.Current?.MainWindow);
         return prompt.ShowDialog() == true ? prompt.Code : null;
      }

      /// <summary>
      /// Writes the changed REST rows, one PUT per group, and counts them as the COM
      /// rows are counted. The server checks every key of a PUT before it applies any
      /// of them, so a refusal leaves the whole group as it was, and it is said with
      /// the server's own sentence.
      ///
      /// waiting counts the rows written here whose change the running server does
      /// not take until a restart or a reinitialize - the SNMP agent's, which it reads
      /// as it starts (AppliesMark) - so that the save does not say "applied
      /// immediately" of a port the agent will go on answering on until then.
      /// </summary>
      private void SaveRest_(List<IRestSetting> rows, ref int saved, ref int failed, ref string firstFailure, ref int waiting)
      {
         if (rows.Count == 0)
            return;

         ServerSession session = ServerSession.Current;
         RestSettingsClient client = session == null ? null : RestSettingsClient.For(session);

         // Why a group had no answer at all: the groups after it are not sent, each of
         // which would wait out the same timeout, and are not saved for the same reason.
         string unanswered = null;

         foreach (IGrouping<string, IRestSetting> group in rows.GroupBy(row => row.Group, StringComparer.Ordinal))
         {
            var changes = new List<KeyValuePair<string, object>>();
            var changed = new List<IRestSetting>();
            foreach (IRestSetting row in group)
            {
               if (row.TryGetChange(out object value))
               {
                  changes.Add(new KeyValuePair<string, object>(row.Key, value));
                  changed.Add(row);
               }
            }

            // A row still holding what it was filled with is what the server holds,
            // and is counted as saved, as an unchanged INI row is.
            int unchanged = group.Count() - changed.Count;
            if (changes.Count == 0)
            {
               saved += unchanged;
               continue;
            }

            RestSettingsClient.Reply reply = unanswered == null ? client?.Put(group.Key, RestSettings.Body(changes), AskForCode_) : null;
            if (reply != null && reply.Ok)
            {
               foreach (IRestSetting row in changed)
               {
                  row.Accepted();
                  if (row is ComSetting { AppliesMark: SettingApplies.Restart or SettingApplies.Reinitialize })
                     waiting++;
               }
               saved += group.Count();
               continue;
            }

            saved += unchanged;
            failed += changed.Count;
            string reason = unanswered ?? (reply == null ? L("Sign in to a server first.") : client.RefusalFor(reply));
            if (reply != null && reply.Status == 0)
               unanswered = reason;
            ComSetting first = changed[0] as ComSetting;
            firstFailure ??= F("\"{0}\" was not saved: {1}", first?.Label ?? changed[0].Key, L(reason));
         }
      }

      private void Save_Click(object sender, RoutedEventArgs e)
      {
         // The writes below are synchronous COM calls, and a page of a hundred and
         // fifty of them over a remote link takes long enough to be worth saying
         // so. Letting the dispatcher paint at Render priority puts the pill on
         // screen before the first write starts; nothing below Render runs, so no
         // input is handled in the middle of a save.
         ShowState_(ServerSettingsPageState.Saving);
         Dispatcher.Invoke(() => { }, DispatcherPriority.Render);

         int saved = 0, failed = 0;

         // The first refusal, in the server's words. A count alone told an
         // administrator whose new password was refused - by the breach check, the
         // endpoint validation, a minimum below one - that "1 setting could not be
         // written" and nothing about which or why.
         string firstFailure = null;

         // The [Settings] keys on this page, to ask the server which of them it is
         // not yet running on - written by this save or by an earlier one. [Database]
         // rows (SectionIniNumber) are not among them: those are read before the
         // database is open, never listed as pending, and a changed one is counted
         // as waiting by itself.
         var iniKeys = new List<string>();
         int databaseRowsChanged = 0;

         // The rows reached over the REST API, written a group at a time after the rest.
         var restRows = new List<IRestSetting>();

         foreach (ComSetting setting in tabs_.SelectMany(tab => tab.Cards).SelectMany(card => card.Settings))
         {
            try
            {
               // INI-backed rows are not in the COM settings tree. A row whose value
               // is what it was filled with is not written (IIniSetting.SaveToIni), and
               // is still what the server holds, so it is counted as saved.
               if (setting is IIniSetting ini)
               {
                  bool wrote = ini.SaveToIni();
                  if (setting is SectionIniNumber)
                  {
                     if (wrote)
                        databaseRowsChanged++;
                  }
                  else if (!string.IsNullOrEmpty(setting.Path))
                  {
                     iniKeys.Add(setting.Path);
                  }
                  saved++;
                  continue;
               }

               // A row for a setting the server never reads writes nothing, and must
               // not be counted among the settings that were saved - "Saved 18
               // settings" has to be true.
               if (setting is INotPersisted)
                  continue;

               // A row reached over the REST API is written with the rest of its
               // group, below - and not at all when its group could not be read,
               // since the card showed no editor for it.
               if (setting is IRestSetting rest)
               {
                  if (restGroups_.TryGetValue(rest.Group, out RestGroup group) && group.Values != null &&
                      (!setting.WantsInitialValue || group.Values.ContainsKey(rest.Key)))
                     restRows.Add(rest);
                  continue;
               }

               // Buttons and preset pickers persist nothing and name no property;
               // resolving one would fail and be counted as a save error.
               if (string.IsNullOrEmpty(setting.Path))
                  continue;

               object owner = ResolveOwner(setting.Path, out string property);
               setting.Write(owner, property);
               saved++;
            }
            catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
            {
               failed++;
               firstFailure ??= F("\"{0}\" was not saved: {1}", setting.Label ?? setting.Path, L(ServerSentences.MessageOf(fatalCheck)));
            }
         }

         int restWaiting = 0;
         SaveRest_(restRows, ref saved, ref failed, ref firstFailure, ref restWaiting);

         // A save publishes (docs/SettingsApply.md): a key marked now is in effect
         // already, and the ones that are not are what the server lists as pending.
         // ServerSettingsPageStatus writes the sentence from that count, so it
         // claims neither a restart nobody needs nor an effect that has not
         // happened. A save that failed anywhere is Critical, because what is on
         // screen is then not what the server holds and the reader has to find out
         // which rows.
         //
         // One read of the pending list for the whole save; the window's bar reuses
         // it. Null - the server cannot say, an older one whose keys all apply at its
         // next start - is not "none", and the sentence then says what it said then.
         int? waiting = databaseRowsChanged + restWaiting;
         if (iniKeys.Count > 0)
         {
            IReadOnlyList<string> pendingList = IniFeatureStore.PendingNames();
            if (pendingList == null)
            {
               waiting = null;
            }
            else
            {
               var pending = new HashSet<string>(pendingList, StringComparer.OrdinalIgnoreCase);
               waiting = databaseRowsChanged + restWaiting + iniKeys.Count(pending.Contains);
            }
         }

         ServerSettingsPageState outcome = failed == 0 ? ServerSettingsPageState.Saved : ServerSettingsPageState.PartlySaved;
         ShowState_(outcome);
         string saveSentence = ServerSettingsPageStatus.SaveSentence(saved, failed, DateTime.Now.ToLongTimeString(), waiting);
         ShowNotice_(
            firstFailure == null ? saveSentence : saveSentence + " " + firstFailure,
            ServerSettingsPageStatus.LevelFor(outcome));

         if (failed == 0)
            Services.Toast.Success(F("Saved {0} settings", saved) + (waiting == null
               ? L(" \u2014 INI settings need a service restart.")
               : waiting > 0 ? F(" \u2014 {0} wait for a reinitialize or a restart.", waiting.Value) : L(" \u2014 applied immediately.")));
         else
            Services.Toast.Failure(F("{0} setting(s) could not be written.", failed), L("Partly saved"));

         // Reload the script engine after scripting changes.
         if (section_ == Section.Advanced)
         {
            try
            {
               dynamic scripting = ServerSession.Current.Application.Settings.Scripting;
               scripting.Reload();
               ServerSession.Release(scripting);
            }
            catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
            {
               // Deliberately ignored: best effort only, and the outcome of the surrounding operation does not depend on this succeeding.
            }
         }
      }
   }
}
