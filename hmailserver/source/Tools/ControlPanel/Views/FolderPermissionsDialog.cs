// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using hMailServer.ControlPanel.Services;
using hMailServer.ControlPanel.Views.Scaffold;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>Small reusable single-line text prompt, on the standard frame through <see cref="DialogFields.PromptText"/>.</summary>
   internal static class InputDialog
   {
      public static string Prompt(Window owner, string title, string prompt, string initial = "")
         => DialogFields.PromptText(owner, title, prompt, initial);
   }

   /// <summary>
   /// ACL permission editor for one public IMAP folder. Lists the access-control
   /// entries (user / group / anyone) and lets each be added, edited or removed.
   /// On the standard frame: the folder is the heading, the three buttons that
   /// act on the list sit under it, what went wrong is a notice above it, and
   /// Close is alone in the footer.
   /// </summary>
   public class FolderPermissionsDialog : FluentDialogWindow
   {
      // eACLPermission bit flags. The label is the check box's caption in the entry
      // dialog; a note is the sentence under the box for a right that does more
      // than its name says. The right's name in an entry's row is RightWords'.
      private static readonly (string Label, int Bit, string Note)[] Flags =
      {
         (L("Lookup (folder is visible)"), 1, null),
         (L("Read messages"), 2, null),
         (L("Keep seen/unseen state"), 4, null),
         (L("Set flags"), 8, null),
         (L("Insert / append messages"), 16, null),
         (L("Post"), 32, L("Post also lets the account send as this mailbox's address over SMTP (Send-As), when 'An authenticated session may only send as an address its account owns or has been granted' is on, on the Protocols page's SMTP tab.")),
         (L("Create sub-folders"), 64, null),
         (L("Delete folder"), 128, null),
         (L("Delete messages"), 256, null),
         (L("Expunge"), 512, null),
         (L("Administer (manage ACL)"), 1024, null)
      };

      // Each right's short name in an entry's row, by its bit, catalogued whole
      // (review finding F204): the row took the first word of each caption, so
      // Delete folder and Delete messages both read "Delete", and in German the
      // rights were "Ordner", "Nachrichten" and "Gelesen/Ungelesen-Status". The
      // names are RFC 4314's rights.
      private static readonly (int Bit, string Word)[] RightWords =
      {
         (1, L("Lookup")),
         (2, L("Read")),
         (4, L("Seen")),
         (8, L("Flags")),
         (16, L("Insert")),
         (32, L("Post")),
         (64, L("Create")),
         (128, L("Delete folder")),
         (256, L("Delete messages")),
         (512, L("Expunge")),
         (1024, L("Administer"))
      };

      private static readonly (int Value, string Label)[] Types =
      {
         (0, L("Account (user)")), (1, L("Group")), (2, L("Anyone"))
      };

      private readonly string folderName_;

      // The names from the top folder down to this one: a folder is found under its
      // parent (OpenFolder), where it was looked for at the top alone and a
      // sub-folder's permissions could not be read (review finding F206).
      private readonly IReadOnlyList<string> folderPath_;
      // On a card with the compact inset, as the public folders list is: the
      // rows take the application's list row style (the selection fill and the
      // brand pill, in place of WPF-UI's solid accent band), and the card is the
      // list's frame.
      private readonly ListBox list_ = new() { Height = 260, BorderThickness = new Thickness(0), Background = System.Windows.Media.Brushes.Transparent };
      private readonly List<int> ids_ = new();

      // Whom each entry is for, beside its id: its type (account, group, anyone)
      // and the account's address or the group's name, for the question asked
      // before it is removed.
      private readonly List<(int Type, string Subject)> subjects_ = new();
      private readonly InlineNotice notice_ = DialogFields.Notice();

      public FolderPermissionsDialog(Window owner, string folderName)
         : this(owner, new[] { folderName }, folderName)
      {
      }

      /// <summary>The permissions of the public folder at <paramref name="folderPath"/>, named <paramref name="folderName"/> - its full name, Sales.Leads.</summary>
      public FolderPermissionsDialog(Window owner, IReadOnlyList<string> folderPath, string folderName)
      {
         folderName_ = folderName;
         folderPath_ = folderPath;
         Owner = owner;
         Title = F("Permissions - {0}", folderName);

         var body = new StackPanel();
         body.Children.Add(notice_);

         AutomationProperties.SetName(list_, F("Access-control entries for {0}", folderName));
         body.Children.Add(new Card
         {
            Padding = new Thickness(DesignTokens.Space.Sm),
            Margin = new Thickness(0, 0, 0, DesignTokens.Space.Md),
            Content = list_
         });

         var actions = new StackPanel { Orientation = Orientation.Horizontal };
         var add = new Wpf.Ui.Controls.Button { Content = L("_Add"), Appearance = Wpf.Ui.Controls.ControlAppearance.Primary, Margin = new Thickness(0, 0, DesignTokens.Space.Sm, 0), MinWidth = 88 };
         add.Click += (s, e) => AddOrEdit(-1);
         var edit = new Wpf.Ui.Controls.Button { Content = L("_Edit"), Margin = new Thickness(0, 0, DesignTokens.Space.Sm, 0), MinWidth = 88 };
         edit.Click += (s, e) => { if (list_.SelectedIndex >= 0) AddOrEdit(ids_[list_.SelectedIndex]); };
         // Remove, not Delete: it takes an entry out of a list the folder outlives,
         // as the group members' and the distribution lists' Remove do; red words on
         // a secondary button, not the filled red of a confirmation's own answer;
         // and Edit and Remove act on the chosen entry, so they can be pressed only
         // while there is one, where a press with none did nothing (review findings
         // F183, F076 and F184).
         var del = new Wpf.Ui.Controls.Button { Content = L("Re_move"), MinWidth = 88 };
         ButtonStyles.DangerText(del);
         del.Click += (s, e) => DeleteSelected();
         SelectionGate.Bind(list_, edit, del);
         actions.Children.Add(add);
         actions.Children.Add(edit);
         actions.Children.Add(del);
         body.Children.Add(actions);

         // Escape only. "Add" is deliberately not the default button: Enter with a
         // row selected in the list must not open the add dialog, and the list's own
         // double-click already covers "open the thing I am looking at".
         var close = new Wpf.Ui.Controls.Button { Content = L("Close"), MinWidth = 88 };
         close.Click += (s, e) => Close();

         // Headed by the window's title, the kind and then the name, where the
         // heading was the bare name and the kind only in a title the frame does
         // not show (review finding F293).
         UseFrame(Title, body, null, close, width: 520);
         list_.MouseDoubleClick += (s, e) => { if (list_.SelectedIndex >= 0) AddOrEdit(ids_[list_.SelectedIndex]); };
         Loaded += (s, e) => Reload();
      }

      /// <summary>
      /// The folder, reached from the top through each folder's SubFolders, as
      /// PublicFoldersView.OpenFolderAt reaches it. Walked here rather than through
      /// that method so that this file says what the folder is to a reader of it
      /// alone, build/check-deck-parity.py's census of what the program writes.
      /// </summary>
      private dynamic OpenFolder(dynamic folders)
      {
         dynamic folder = folders.ItemByName[folderPath_[0]];
         for (int i = 1; i < folderPath_.Count; i++)
         {
            dynamic subFolders = folder.SubFolders;
            try
            {
               dynamic next = subFolders.ItemByName[folderPath_[i]];
               ServerSession.Release((object)folder);
               folder = next;
            }
            finally
            {
               ServerSession.Release((object)subFolders);
            }
         }

         return folder;
      }

      private void Reload()
      {
         list_.Items.Clear();
         ids_.Clear();
         subjects_.Clear();
         dynamic folders = ServerSession.Current.Application.Settings.PublicFolders;
         try
         {
            dynamic folder = OpenFolder(folders);
            dynamic perms = folder.Permissions;
            int count = (int)perms.Count;
            for (int i = 0; i < count; i++)
            {
               dynamic p = perms.Item[i];
               ids_.Add((int)p.ID);
               int type = (int)p.PermissionType;
               subjects_.Add((type, type switch { 0 => SafeAccount(p), 1 => (string)SafeGroupName(p) ?? F("(group #{0})", (int)p.PermissionGroupID), _ => "" }));
               list_.Items.Add(Describe(p));
               ServerSession.Release(p);
            }
            ServerSession.Release(perms);
            ServerSession.Release(folder);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            notice_.Show(StatusLevel.Critical, F("Could not load permissions: {0}", ServerSession.DescribeComError(ex)));
         }
         finally
         {
            ServerSession.Release(folders);
         }
      }

      private static string Describe(dynamic p)
      {
         int type = (int)p.PermissionType;
         string subject = type switch
         {
            0 => SafeAccount(p),
            1 => SafeGroup(p),
            _ => L("Anyone")
         };
         int value = (int)p.Value;
         var rights = new List<string>();
         foreach ((int bit, string word) in RightWords)
            if ((value & bit) != 0)
               rights.Add(word);
         return subject + " - " + (rights.Count == 0 ? L("no rights") : string.Join(", ", rights));
      }

      private static string SafeAccount(dynamic p)
      {
         try { dynamic a = p.Account; string addr = (string)a.Address; ServerSession.Release(a); return addr; }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck)) { return F("(account #{0})", (int)p.PermissionAccountID); }
      }

      private static string SafeGroup(dynamic p)
      {
         string name = SafeGroupName(p);
         return name != null ? F("Group: {0}", name) : F("(group #{0})", (int)p.PermissionGroupID);
      }

      // The group's name alone, or null when it cannot be read: the list says
      // "Group: " before it in the reader's language, and the entry dialog's box
      // takes the name as it is.
      private static string SafeGroupName(dynamic p)
      {
         try { dynamic g = p.Group; string n = (string)g.Name; ServerSession.Release(g); return n; }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck)) { return null; }
      }

      /// <summary>
      /// Removes the chosen entry after asking, naming whom it is for and the folder,
      /// with Cancel the answer Enter gives: the rights it gives end at once, and it
      /// was removed at the press (review finding F184).
      /// </summary>
      private void DeleteSelected()
      {
         int index = list_.SelectedIndex;
         if (index < 0 || index >= ids_.Count)
            return;
         int id = ids_[index];
         (int type, string subject) = subjects_[index];

         if (!Dialogs.Confirm(Confirmations.RemoveFolderPermission(type, subject, folderName_), L("Control Panel"), L("Re_move"), destructive: true))
            return;

         notice_.Hide();
         dynamic folders = ServerSession.Current.Application.Settings.PublicFolders;
         try
         {
            dynamic folder = OpenFolder(folders);
            dynamic perms = folder.Permissions;
            perms.DeleteByDBID(id);
            ServerSession.Release(perms);
            ServerSession.Release(folder);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            notice_.Show(StatusLevel.Critical, F("Could not delete the permission: {0}", ServerSession.DescribeComError(ex)));
         }
         finally
         {
            ServerSession.Release(folders);
         }
         Reload();
      }

      private void AddOrEdit(int existingId)
      {
         // The entry's account or group is resolved in the dialog, which says on
         // its box a name the server has none of and stays open with what was
         // typed, where it was resolved after the dialog had closed and said in
         // this dialog's notice with the entry gone (review finding F205).
         var dlg = new PermissionEditDialog(this, Types, Flags)
         {
            Resolve = (type, subject) => type == 0 ? ResolveAccountId(subject) : ResolveGroupId(subject),
            Suggest = SubjectsOf_
         };

         // Pre-fill when editing.
         if (existingId >= 0)
         {
            dynamic folders = ServerSession.Current.Application.Settings.PublicFolders;
            try
            {
               dynamic folder = OpenFolder(folders);
               dynamic perms = folder.Permissions;
               dynamic p = perms.ItemByDBID(existingId);
               int type = (int)p.PermissionType;
               string subject = type == 0 ? SafeAccount(p) : type == 1 ? (string)SafeGroupName(p) ?? "" : "";
               dlg.Initialize(type, subject, (int)p.Value);
               ServerSession.Release(p);
               ServerSession.Release(perms);
               ServerSession.Release(folder);
            }
            catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck)) { /* Deliberately ignored: best effort only, and the outcome of the surrounding operation does not depend on this succeeding. */ }
            finally { ServerSession.Release(folders); }
         }

         if (dlg.ShowDialog() != true)
            return;

         notice_.Hide();
         int subjectId = dlg.SubjectId;

         dynamic folders2 = ServerSession.Current.Application.Settings.PublicFolders;
         try
         {
            dynamic folder = OpenFolder(folders2);
            dynamic perms = folder.Permissions;
            dynamic p = existingId >= 0 ? perms.ItemByDBID(existingId) : perms.Add();
            p.PermissionType = dlg.SelectedType;
            if (dlg.SelectedType == 0) p.PermissionAccountID = subjectId;
            else if (dlg.SelectedType == 1) p.PermissionGroupID = subjectId;
            p.Value = dlg.Value;
            p.Save();
            ServerSession.Release(p);
            ServerSession.Release(perms);
            ServerSession.Release(folder);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            notice_.Show(StatusLevel.Critical, F("Could not save the permission: {0}", ServerSession.DescribeComError(ex)));
         }
         finally
         {
            ServerSession.Release(folders2);
         }
         Reload();
      }

      /// <summary>
      /// What the entry dialog's box offers to pick from: the server's account
      /// addresses - up to a thousand, read domain by domain - or its groups'
      /// names, in name order. What cannot be read is left out; the box still
      /// takes what is typed.
      /// </summary>
      private static IReadOnlyList<string> SubjectsOf_(int type)
      {
         const int most = 1000;
         var names = new List<string>();
         try
         {
            if (type == 1)
            {
               dynamic groups = ServerSession.Current.Application.Settings.Groups;
               try
               {
                  int count = (int)groups.Count;
                  for (int i = 0; i < count; i++)
                  {
                     dynamic g = groups.Item[i];
                     names.Add((string)g.Name);
                     ServerSession.Release(g);
                  }
               }
               finally { ServerSession.Release(groups); }
            }
            else if (type == 0)
            {
               dynamic domains = ServerSession.Current.Application.Domains;
               try
               {
                  int domainCount = (int)domains.Count;
                  for (int d = 0; d < domainCount && names.Count < most; d++)
                  {
                     dynamic domain = domains.Item[d];
                     dynamic accounts = domain.Accounts;
                     int count = (int)accounts.Count;
                     for (int i = 0; i < count && names.Count < most; i++)
                     {
                        dynamic account = accounts.Item[i];
                        names.Add((string)account.Address);
                        ServerSession.Release(account);
                     }
                     ServerSession.Release(accounts);
                     ServerSession.Release(domain);
                  }
               }
               finally { ServerSession.Release(domains); }
            }
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck)) { /* Deliberately ignored: the list is an offer only, and the box takes what is typed. */ }

         names.RemoveAll(string.IsNullOrEmpty);
         names.Sort(StringComparer.OrdinalIgnoreCase);
         return names;
      }

      private static int ResolveAccountId(string address)
      {
         if (string.IsNullOrWhiteSpace(address) || !address.Contains('@'))
            return 0;
         string domainName = address.Split('@')[1];
         dynamic domains = ServerSession.Current.Application.Domains;
         try
         {
            dynamic domain = domains.ItemByName[domainName];
            dynamic accounts = domain.Accounts;
            dynamic account = accounts.ItemByAddress[address];
            int id = (int)account.ID;
            ServerSession.Release(account);
            ServerSession.Release(accounts);
            ServerSession.Release(domain);
            return id;
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck)) { return 0; }
         finally { ServerSession.Release(domains); }
      }

      private static int ResolveGroupId(string name)
      {
         dynamic groups = ServerSession.Current.Application.Settings.Groups;
         try
         {
            int count = (int)groups.Count;
            for (int i = 0; i < count; i++)
            {
               dynamic g = groups.Item[i];
               bool match = string.Equals((string)g.Name, name, StringComparison.OrdinalIgnoreCase);
               int id = match ? (int)g.ID : 0;
               ServerSession.Release(g);
               if (match)
                  return id;
            }
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck)) { /* Deliberately ignored: best effort only, and the outcome of the surrounding operation does not depend on this succeeding. */ }
         finally { ServerSession.Release(groups); }
         return 0;
      }
   }

   /// <summary>
   /// Add/edit dialog for a single ACL entry, on the standard frame. The account
   /// or group is picked from the server's or typed, and resolved here: a name the
   /// server has none of is said on its box and the dialog stays open (review
   /// finding F205). Read only, Read and write and Full control tick the rights in
   /// one press, and the eleven rights are in two columns.
   /// </summary>
   internal class PermissionEditDialog : FluentDialogWindow
   {
      private readonly ComboBox typeCombo_ = new();

      // Typed, or picked from the server's accounts or groups (Suggest), the list
      // filling in as the first letters are typed.
      private readonly ComboBox subject_ = new() { IsEditable = true, IsTextSearchEnabled = true };
      private readonly FieldRow subjectRow_;
      private readonly List<(CheckBox Box, int Bit)> flagBoxes_ = new();
      private readonly Dictionary<int, IReadOnlyList<string>> offered_ = new();

      public int SelectedType { get; private set; }
      public string Subject { get; private set; }
      public int Value { get; private set; }

      /// <summary>The id of the account or group the dialog resolved Subject to; 0 for Anyone, or where no Resolve is set.</summary>
      public int SubjectId { get; private set; }

      /// <summary>The id of the account (type 0) or group (type 1) a name is, or 0 where the server has none.</summary>
      public Func<int, string, int> Resolve { get; set; }

      /// <summary>The account addresses (type 0) or group names (type 1) the box offers.</summary>
      public Func<int, IReadOnlyList<string>> Suggest { get; set; }

      public PermissionEditDialog(Window owner, (int Value, string Label)[] types, (string Label, int Bit, string Note)[] flags)
      {
         Owner = owner;
         Title = L("Access-control entry");

         var body = new StackPanel();
         foreach ((int value, string label) in types)
            typeCombo_.Items.Add(new ComboBoxItem { Content = label, Tag = value });
         typeCombo_.SelectedIndex = 0;
         typeCombo_.SelectionChanged += (s, e) => UpdateSubjectState();
         body.Children.Add(Label(L("Applies _to"), typeCombo_));

         subjectRow_ = Label(L("_Account address"), subject_);
         body.Children.Add(subjectRow_);

         body.Children.Add(DialogFields.Caption(L("Permissions")));

         // The three sets of rights an entry is usually given, each one press that
         // ticks its rights and clears the rest (Services/FolderRightPresets).
         var presets = new WrapPanel { Margin = new Thickness(0, 0, 0, DesignTokens.Space.Sm) };
         presets.Children.Add(Preset_(new Wpf.Ui.Controls.Button { Content = L("_Read only") }, FolderRightPresets.ReadOnly,
            L("Ticks Lookup, Read messages and Keep seen/unseen state, and clears the rest.")));
         presets.Children.Add(Preset_(new Wpf.Ui.Controls.Button { Content = L("Read and _write") }, FolderRightPresets.ReadAndWrite,
            L("Ticks the read-only rights and Set flags, Insert / append messages, Delete messages and Expunge, and clears the rest.")));
         presets.Children.Add(Preset_(new Wpf.Ui.Controls.Button { Content = L("_Full control") }, FolderRightPresets.FullControl,
            L("Ticks every right, Administer included.")));
         body.Children.Add(presets);

         // Two columns, row by row, where one column of eleven at a 40-pixel pitch
         // ran down the dialog.
         var columns = new Grid();
         columns.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
         columns.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
         for (int i = 0; i < flags.Length; i++)
         {
            (string label, int bit, string note) = flags[i];
            if (i % 2 == 0)
               columns.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });

            var cell = new StackPanel { Margin = new Thickness(0, 0, i % 2 == 0 ? DesignTokens.Space.Md : 0, 0) };
            Grid.SetRow(cell, i / 2);
            Grid.SetColumn(cell, i % 2);
            columns.Children.Add(cell);

            var cb = new CheckBox { Content = label, Margin = new Thickness(0, DesignTokens.Space.Xs, 0, DesignTokens.Space.Xs) };
            flagBoxes_.Add((cb, bit));
            cell.Children.Add(cb);
            // Under the box, not in its caption: "Post - and send as this mailbox's
            // address over SMTP (Send-As, when SmtpAuthenticatedSenderCheck is on)"
            // was wider than the dialog. The box is told it as help text, so that a
            // screen reader reading the box hears it as it did when the caption said it.
            if (note != null)
            {
               cell.Children.Add(DialogFields.Note(note));
               AutomationProperties.SetHelpText(cb, note);
            }
         }
         body.Children.Add(columns);

         // Enter saves, Escape cancels.
         var ok = new Wpf.Ui.Controls.Button { Content = L("_Save"), Appearance = Wpf.Ui.Controls.ControlAppearance.Primary, MinWidth = 88 };
         ok.Click += (s, e) => Commit();
         var cancel = new Wpf.Ui.Controls.Button { Content = L("Cancel"), MinWidth = 88 };
         cancel.Click += (s, e) => Close();

         UseFrame(L("Access-control entry"), body, ok, cancel, width: 520);

         // Room for the account or group box's message, while the box is shown:
         // an entry for Anyone has nothing to refuse, and the dialog shrinks with
         // the box as it did.
         KeepRoomForFieldErrors(1, subjectRow_);
         UpdateSubjectState();

         // The offer is read once the page has said where from (Suggest).
         Loaded += (_, _) => Offer_();
      }

      private Wpf.Ui.Controls.Button Preset_(Wpf.Ui.Controls.Button button, int rights, string help)
      {
         button.Margin = new Thickness(0, 0, DesignTokens.Space.Sm, DesignTokens.Space.Xs);
         AutomationProperties.SetHelpText(button, help);
         button.ToolTip = help;
         button.Click += (_, _) =>
         {
            foreach ((CheckBox box, int bit) in flagBoxes_)
               box.IsChecked = (rights & bit) != 0;
         };
         return button;
      }

      /// <summary>The server's accounts or groups in the box's list, for the entry type chosen, read once a type.</summary>
      private void Offer_()
      {
         int type = typeCombo_.SelectedItem is ComboBoxItem cbi ? (int)cbi.Tag : 0;
         if (Suggest == null || type == 2)
            return;

         if (!offered_.TryGetValue(type, out IReadOnlyList<string> names))
            offered_[type] = names = Suggest(type) ?? Array.Empty<string>();

         // The text typed stays: clearing the list would clear it.
         string typed = subject_.Text;
         subject_.Items.Clear();
         foreach (string name in names)
            subject_.Items.Add(name);
         subject_.Text = typed;
      }

      public void Initialize(int type, string subject, int value)
      {
         foreach (ComboBoxItem item in typeCombo_.Items)
            if ((int)item.Tag == type)
               typeCombo_.SelectedItem = item;
         subject_.Text = subject ?? "";
         foreach ((CheckBox box, int bit) in flagBoxes_)
            box.IsChecked = (value & bit) != 0;
         UpdateSubjectState();
      }

      private void UpdateSubjectState()
      {
         int type = typeCombo_.SelectedItem is ComboBoxItem cbi ? (int)cbi.Tag : 0;
         bool needsSubject = type != 2; // Anyone needs no subject
         subjectRow_.Label = type == 1 ? L("_Group name") : L("_Account address");
         subjectRow_.Visibility = needsSubject ? Visibility.Visible : Visibility.Collapsed;
         subjectRow_.Error = null;

         // The caption above this box changes with the entry type, so its accessible
         // name has to change with it. The row names the editor once, from the
         // caption it was built with; a group entry would otherwise announce
         // itself as "Account address".
         AutomationProperties.SetName(subject_, MnemonicText.Strip(subjectRow_.Label));

         if (IsLoaded)
            Offer_();
      }

      private void Commit()
      {
         SelectedType = typeCombo_.SelectedItem is ComboBoxItem cbi ? (int)cbi.Tag : 0;
         Subject = (subject_.Text ?? "").Trim();
         int value = 0;
         foreach ((CheckBox box, int bit) in flagBoxes_)
            if (box.IsChecked is true)
               value |= bit;
         Value = value;

         // The account or group resolved here, on its box, before the dialog
         // closes: a name the server has none of was said by the folder's dialog
         // after this one had closed, with the entry typed gone.
         DialogFields.ClearErrors(subjectRow_);
         SubjectId = 0;
         if (SelectedType != 2)
         {
            if (Subject.Length == 0)
            {
               DialogFields.ShowError(subjectRow_, SelectedType == 1 ? L("Enter the group's name.") : L("Enter the account's address."));
               return;
            }

            if (Resolve != null)
            {
               SubjectId = Resolve(SelectedType, Subject);
               if (SubjectId == 0)
               {
                  DialogFields.ShowError(subjectRow_, SelectedType == 1
                     ? F("No group found named '{0}'.", Subject)
                     : F("No account found with address '{0}'.", Subject));
                  return;
               }
            }
         }

         DialogResult = true;
         Close();
      }

      /// <summary>
      /// A caption and its editor as one row, which names the editor to UI
      /// Automation. The eleven permission checkboxes are already named by their
      /// own Content.
      /// </summary>
      private static FieldRow Label(string text, FrameworkElement editor) => DialogFields.Field(text, editor);
   }
}
