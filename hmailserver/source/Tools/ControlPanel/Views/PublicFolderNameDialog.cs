// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Windows;
using System.Windows.Automation;
using hMailServer.ControlPanel.Services;
using hMailServer.ControlPanel.Views.Scaffold;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// The Public folders page's Add folder: the folder's name, with the hint that a
   /// name with the delimiter in it makes a sub-folder, opened on the selected
   /// folder's name and the delimiter so that what is typed goes under it, and
   /// Create, which can be pressed only once a name is typed (review finding F206).
   /// It was "Folder name:" over a box, with OK and Cancel and no example, and an
   /// empty name was silently ignored. The folder is made while the dialog is open,
   /// so that a name the page or the server refuses is said under the box and what
   /// was typed stays.
   /// </summary>
   public class PublicFolderNameDialog : FluentDialogWindow
   {
      private readonly Wpf.Ui.Controls.TextBox name_ = new();
      private readonly FieldRow nameRow_;
      private readonly Func<string, string> create_;

      /// <param name="owner">The window the dialog belongs to.</param>
      /// <param name="delimiter">The server's hierarchy delimiter, which the hint names.</param>
      /// <param name="prefill">What the box opens with: the selected folder's name and the delimiter, or nothing.</param>
      /// <param name="create">Makes the folder of the name typed; answers null once it is made, or what to say under the box.</param>
      public PublicFolderNameDialog(Window owner, string delimiter, string prefill, Func<string, string> create)
      {
         Owner = owner;
         create_ = create;
         Title = L("New public folder");

         name_.Text = prefill ?? "";
         AutomationProperties.SetAutomationId(name_, "public-folder-name");
         nameRow_ = DialogFields.Field(L("_Folder name"), name_, PublicFolderTree.Hint(delimiter));

         var createButton = new Wpf.Ui.Controls.Button
         {
            Content = L("Cr_eate"),
            Appearance = Wpf.Ui.Controls.ControlAppearance.Primary,
            MinWidth = 88,
            IsEnabled = name_.Text.Trim().Length > 0
         };
         AutomationProperties.SetAutomationId(createButton, "public-folder-create");
         createButton.Click += (_, _) => Create_();
         var cancel = new Wpf.Ui.Controls.Button { Content = L("Cancel"), MinWidth = 88 };
         cancel.Click += (_, _) => Close();

         // Create waits for a name, and a message under the box goes with an edit of it.
         name_.TextChanged += (_, _) =>
         {
            createButton.IsEnabled = name_.Text.Trim().Length > 0;
            nameRow_.Error = null;
         };

         UseFrame(L("New public folder"), nameRow_, createButton, cancel, width: DesignTokens.Dialog.DefaultWidth);
         KeepRoomForFieldErrors(2, nameRow_);

         // The caret after the selected folder's name, so that typing names the
         // folder under it; an empty box has nothing to keep.
         name_.Loaded += (_, _) => name_.CaretIndex = name_.Text.Length;
      }

      /// <summary>The name the folder was made with, once it was.</summary>
      public string CreatedName { get; private set; }

      private void Create_()
      {
         string typed = name_.Text.Trim();
         string refusal = create_?.Invoke(typed);
         if (refusal != null)
         {
            DialogFields.ShowError(nameRow_, refusal);
            return;
         }

         CreatedName = typed;
         DialogResult = true;
         Close();
      }
   }
}
