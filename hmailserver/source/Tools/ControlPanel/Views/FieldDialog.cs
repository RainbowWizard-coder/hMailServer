// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using Wpf.Ui.Controls;
using Button = Wpf.Ui.Controls.Button;
using TextBox = Wpf.Ui.Controls.TextBox;
using hMailServer.ControlPanel.Services;
using hMailServer.ControlPanel.Views.Scaffold;
using System.Linq;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// Modal property editor generated from a
   /// <see cref="CollectionEditorView.CollectionSpec"/>. Produces a
   /// dictionary of property =&gt; value on OK.
   ///
   /// This is the editor behind every collection page - SURBL servers, DNS
   /// blacklists, the two white lists, blocked attachments, incoming relays,
   /// groups, server messages - so an accessibility defect here is one defect in
   /// perhaps a dozen places. Until this pass there was one: each field's wording
   /// was a TextBlock placed above the control with nothing connecting the two, so
   /// every box in every one of those dialogs was announced as an unnamed "edit".
   /// Every field is a <see cref="FieldRow"/> now, on the standard frame.
   ///
   /// The write happens while the dialog is open, through the save the page
   /// passes in: a refusal of the server is said on the field it names, or on the
   /// first field, and the dialog keeps everything typed, where every committer
   /// returned true, the dialog closed, and the page's write after it reported a
   /// refusal in a message box with all that was typed gone (review finding
   /// F209). Every caption carries an Alt key, assigned here, since the specs'
   /// captions carry none; a number box is as wide as a number, not the dialog.
   /// </summary>
   internal sealed class FieldDialog : FluentDialogWindow
   {
      public Dictionary<string, object> Result { get; } = new();

      private readonly List<Func<bool>> committers_ = new();

      // Every field's row, so that a refusal on one is cleared before the next try.
      private readonly List<FieldRow> rows_ = new();

      // The rows in the order the fields are laid out, with their captions and
      // properties, for the field a refusal of the server names.
      private readonly List<CollectionEditorView.FieldSpec> laidOut_ = new();

      // The page's write of the values: null when it saved, or the refusal to say.
      private readonly Func<Dictionary<string, object>, Refusal> save_;

      /// <summary>
      /// Why the page's write refused the values: the sentence the dialog says, and
      /// the property of the field it is said on, where the page knows it - or
      /// null, and the dialog says it on the field the sentence names, or on its
      /// first field.
      /// </summary>
      public sealed record Refusal(string Text, string Prop = null);

      public FieldDialog(CollectionEditorView.CollectionSpec spec, CollectionEditorView.Row existing, Window owner)
         : this(spec, existing, owner, null)
      {
      }

      /// <param name="save">
      /// Writes the values to the server while the dialog is open: null when they
      /// are saved and the dialog may close, or the sentence that says why not,
      /// which the dialog says on the field it names and stays open. Null closes
      /// the dialog on the press, the values in <see cref="Result"/>.
      /// </param>
      public FieldDialog(CollectionEditorView.CollectionSpec spec, CollectionEditorView.Row existing, Window owner,
                         Func<Dictionary<string, object>, Refusal> save)
      {
         save_ = save;
         Owner = owner;
         Title = L(existing == null ? spec.AddTitle : spec.EditTitle);
         AutomationProperties.SetName(this, Title);

         var panel = new StackPanel();

         // The tick boxes that lead a spec - Active, Enabled, the grid's first
         // column - go after the fields that say what the entry is, so that an
         // Add dialog opens on the host and not on a tick (review finding F174):
         // on the specs that ask for it, which are the short ones. An external
         // account's Enabled and a remote domain policy's Active are the master
         // switch of a dialog that scrolls, and stay first, in sight.
         List<CollectionEditorView.FieldSpec> fields = spec.Fields.Where(f => f.Prop != "ID").ToList();
         if (spec.TickBoxesLast)
            fields = FieldDialogRules.TickBoxesAfterTheRest(fields, f => f.Kind == CollectionEditorView.FieldKind.Bool);

         // Resolved for the whole dialog at once, the same way the settings pages
         // do it, so that two fields with the same wording are told apart by the
         // object they belong to rather than announced identically.
         IReadOnlyList<string> names = AccessibleNames.Resolve(
            fields.Select(f => new LabelledEditor(f.Label, spec.ItemNoun, f.Prop)).ToList());

         // The button says what it does: Add on an Add dialog, Save on an Edit one.
         var ok = new Button { Content = existing == null ? L("_Add") : L("_Save"), Appearance = ControlAppearance.Primary, MinWidth = 88 };

         // An Alt key on every caption, none of them the button's: the specs'
         // captions carry none, and in Japanese no field of these dialogs had one.
         string okCaption = (string)ok.Content;
         List<string> captions = FieldDialogRules.WithAccessKeys(
            fields.Select(f => f.Label).ToList(), fields.Select(f => f.Prop).ToList(),
            FieldDialogRules.KeysOf(new[] { okCaption, L("Cancel") }), FieldDialogRules.MarksKeyAfter(okCaption));

         for (int i = 0; i < fields.Count; i++)
         {
            CollectionEditorView.FieldSpec f = fields[i];

            object current = existing != null && existing.Values.TryGetValue(f.Prop, out object v)
               ? v
               : f.Default;

            FieldRow row = BuildField(f, captions[i], current, names[i]);
            rows_.Add(row);
            laidOut_.Add(f);
            panel.Children.Add(row);
         }

         ok.Click += (_, _) =>
         {
            DialogFields.ClearErrors(rows_.ToArray());
            Result.Clear();

            // Every committer runs, so that every field refused is marked on
            // this press and not one a press; the keyboard goes to the first.
            if (!FieldDialogRules.AllAccept(committers_))
            {
               FieldRow first = rows_.FirstOrDefault(r => r.Error != null);
               if (first != null)
                  DialogFields.ShowError(first, first.Error);
               return;
            }

            // The write, with the dialog open: a refusal stays here, on its field.
            Refusal refusal = save_?.Invoke(Result);
            if (refusal != null)
            {
               DialogFields.ShowError(RowFor_(refusal), refusal.Text);
               return;
            }

            DialogResult = true;
            Close();
         };
         var cancel = new Button { Content = L("Cancel"), MinWidth = 88 };
         cancel.Click += (_, _) => Close();

         UseFrame(Title, panel, ok, cancel, width: 460);

         // Room under the fields for the messages a press can bring - one for each
         // required field, or the one a refusal of the server says - so that Add
         // and Cancel stay where they are.
         KeepRoomForFieldErrors(Math.Max(1, fields.Count(f => f.Required)));
      }

      /// <summary>
      /// The row a refusal of the server is said on: the field the page named, the
      /// field whose caption or property the refusal names, or the first field that
      /// is not a tick box.
      /// </summary>
      private FieldRow RowFor_(Refusal refusal)
      {
         int named = refusal.Prop != null
            ? laidOut_.FindIndex(f => f.Prop == refusal.Prop)
            : FieldDialogRules.FieldNamedIn(refusal.Text, laidOut_.Select(f => f.Label).ToList(), laidOut_.Select(f => f.Prop).ToList());
         if (named >= 0)
            return rows_[named];

         for (int i = 0; i < laidOut_.Count; i++)
         {
            if (laidOut_[i].Kind != CollectionEditorView.FieldKind.Bool)
               return rows_[i];
         }

         return rows_[0];
      }

      private FieldRow BuildField(CollectionEditorView.FieldSpec f, string caption, object current, string accessibleName)
      {
         string prop = f.Prop;

         // What the server says about the object: shown, selectable to copy (a
         // fingerprint is read out to a partner), and never sent back.
         if (f.ReadOnly)
         {
            var shown = new TextBox
            {
               Text = Convert.ToString(current) ?? "",
               IsReadOnly = true,
               TextWrapping = TextWrapping.Wrap
            };
            Describe(shown, prop, accessibleName);
            return DialogFields.Field(caption, shown, f.Hint);
         }

         switch (f.Kind)
         {
            case CollectionEditorView.FieldKind.Bool:
               {
                  // The caption wraps: the dialog is 460 wide, and a caption that says
                  // what the option does ("Deliver each message to the accounts its
                  // headers name (a mailbox shared by several people)") was cut off at
                  // its edge. AccessText keeps the access key a string caption had.
                  var words = new AccessText { Text = caption, TextWrapping = TextWrapping.Wrap };
                  var box = new CheckBox { Content = words, IsChecked = current is bool b && b };
                  // Named by its caption, or by the qualified name when that tells it
                  // from an identically worded field elsewhere in the dialog. The
                  // caption is no longer a string Content, so the name is set.
                  if (!string.Equals(accessibleName, f.Label, StringComparison.Ordinal))
                     Describe(box, prop, accessibleName);
                  else
                  {
                     AutomationProperties.SetAutomationId(box, prop);
                     AutomationProperties.SetName(box, f.Label);
                  }
                  committers_.Add(() => { Result[prop] = box.IsChecked is true; return true; });
                  return DialogFields.Field(null, box, f.Hint);
               }
            case CollectionEditorView.FieldKind.Combo:
               {
                  var combo = new ComboBox();
                  int sel = current is int ci ? ci : Convert.ToInt32(current ?? 0);
                  foreach ((int Value, string Label) opt in f.Options)
                  {
                     var item = new ComboBoxItem { Content = opt.Label, Tag = opt.Value };
                     combo.Items.Add(item);
                     if (opt.Value == sel)
                        combo.SelectedItem = item;
                  }
                  if (combo.SelectedItem == null && combo.Items.Count > 0)
                     combo.SelectedIndex = 0;
                  Describe(combo, prop, accessibleName);
                  committers_.Add(() =>
                  {
                     Result[prop] = combo.SelectedItem is ComboBoxItem cbi ? (int)cbi.Tag : 0;
                     return true;
                  });
                  return DialogFields.Field(caption, combo, f.Hint);
               }
            case CollectionEditorView.FieldKind.Multiline:
               {
                  var box = new TextBox
                  {
                     Text = Convert.ToString(current) ?? "",
                     AcceptsReturn = true,
                     TextWrapping = TextWrapping.Wrap,
                     MinLines = 4,
                     MaxLines = 10
                  };
                  Describe(box, prop, accessibleName);
                  committers_.Add(() =>
                  {
                     // Left empty, a write-only value is not sent, so the one
                     // the server holds stays. Only an EMPTY box: white space
                     // is sent, and the server, which trims the value, takes
                     // it for "remove" - as the REST API and the Deck do.
                     if (f.WriteOnly && box.Text.Length == 0)
                        return true;
                     Result[prop] = box.Text;
                     return true;
                  });
                  return DialogFields.Field(caption, box, f.Hint);
               }
            case CollectionEditorView.FieldKind.Password:
               {
                  var box = new hMailServer.ControlPanel.Views.PasswordField { Password = Convert.ToString(current) ?? "" };
                  Describe(box, prop, accessibleName);
                  committers_.Add(() => { Result[prop] = box.Password; return true; });
                  return DialogFields.Field(caption, box, f.Hint);
               }
            case CollectionEditorView.FieldKind.Number:
               {
                  double cur = 0;
                  try { cur = Convert.ToDouble(current ?? 0); } catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck)) { cur = 0; }
                  // As wide as a number and its spin buttons, at the start of the
                  // row: across the dialog, 412 pixels held a one-to-three digit score.
                  var box = new Wpf.Ui.Controls.NumberBox
                  {
                     Value = cur,
                     MaxDecimalPlaces = 0,
                     SmallChange = 1,
                     LargeChange = 10,
                     Width = 160,
                     HorizontalAlignment = HorizontalAlignment.Left
                  };
                  Describe(box, prop, accessibleName);
                  committers_.Add(() => { Result[prop] = (int)(box.Value ?? 0); return true; });
                  return DialogFields.Field(caption, box, f.Hint);
               }
            default:
               {
                  var box = new TextBox { Text = Convert.ToString(current) ?? "" };
                  Describe(box, prop, accessibleName);
                  FieldRow row = DialogFields.Field(caption, box, f.Hint);
                  committers_.Add(() =>
                  {
                     // A field the entry is useless without is refused empty, on
                     // the field (review finding F174). Marked here; the keyboard
                     // is put in the first field refused once every committer has
                     // run, by the button's handler.
                     if (f.Required && string.IsNullOrWhiteSpace(box.Text))
                     {
                        row.Error = FieldDialogRules.RequiredMessage(f.Label);
                        return false;
                     }
                     Result[prop] = box.Text;
                     return true;
                  });
                  return row;
               }
         }
      }

      /// <summary>
      /// Names the control and gives it a stable automation id. The id is the
      /// property being edited, which is what a UI-automation test would look for;
      /// the name is what a screen reader says, and until this pass there was none.
      /// </summary>
      private static void Describe(FrameworkElement element, string prop, string accessibleName)
      {
         if (element == null)
            return;

         if (!string.IsNullOrEmpty(prop))
            AutomationProperties.SetAutomationId(element, prop);

         if (!string.IsNullOrEmpty(accessibleName))
            AutomationProperties.SetName(element, accessibleName);
      }
   }
}
