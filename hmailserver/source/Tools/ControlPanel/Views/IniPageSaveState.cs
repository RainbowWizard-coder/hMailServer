// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Windows;
using hMailServer.ControlPanel.Services;
using InlineNotice = hMailServer.ControlPanel.Views.Scaffold.InlineNotice;
using StatusPill = hMailServer.ControlPanel.Views.Scaffold.StatusPill;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// The save state of an hMailServer.INI settings page - the feature settings pages,
   /// Directory authentication and Directory synchronisation - in the pill beside its
   /// title and the notice under its header, by the one model every settings page
   /// keeps (<see cref="SettingsPageStatus"/>, review findings F120 and F218).
   ///
   /// The three pages said it three ways: the feature settings pages wore a grey
   /// Editing pill as they opened, never marked an edit, and kept a green Saved
   /// through the edits after it, though the service had not taken the save; the two
   /// directory pages had no pill, and said Saved, and Could not save, in grey 12-pixel
   /// text in a footer - or in a message box. Now the pill follows the editors
   /// (<see cref="Follow"/>): nothing while they hold what was read, Unsaved changes
   /// from the first edit, and after a save the state the save settled in, Saved,
   /// Reinitialise to apply or Restart to apply, until the next edit. The sentence that
   /// goes with a state is a notice at its level, with room for one action, and a save
   /// that wrote nothing says why in a Critical notice.
   /// </summary>
   internal sealed class IniPageSaveState
   {
      private readonly StatusPill pill_;
      private readonly InlineNotice notice_;
      private readonly Func<bool> hasUnsavedChanges_;
      private SettingsPageState settled_ = SettingsPageState.Clean;

      /// <param name="pill">The pill in the page header's Status slot.</param>
      /// <param name="notice">The notice under the header, collapsed while there is nothing to say.</param>
      /// <param name="hasUnsavedChanges">The page's own answer: whether an editor differs from what was read or saved.</param>
      public IniPageSaveState(StatusPill pill, InlineNotice notice, Func<bool> hasUnsavedChanges)
      {
         pill_ = pill;
         notice_ = notice;
         hasUnsavedChanges_ = hasUnsavedChanges;
         Show_(SettingsPageState.Clean);
      }

      /// <summary>The state the pill shows now.</summary>
      public SettingsPageState State { get; private set; }

      /// <summary>
      /// After a read, a save or a step of a restart: the state the page rests in until
      /// the next edit, and the sentence that goes with it - at the state's level unless
      /// <paramref name="level"/> says otherwise, with <paramref name="action"/> as the
      /// notice's one action. A null sentence hides the notice.
      /// </summary>
      public void Settle(SettingsPageState state, string sentence, StatusLevel? level = null, object action = null)
      {
         settled_ = state;
         Say_(sentence, level ?? SettingsPageStatus.LevelFor(state), action);
         Follow();
      }

      /// <summary>
      /// After an edit: Unsaved changes while an editor differs from what was read or
      /// saved, and the settled state again once none does. The notice is left alone:
      /// what a save or a read said stays true after the first keystroke.
      /// </summary>
      public void Follow()
         => Show_(SettingsPageStatus.Follow(settled_, hasUnsavedChanges_?.Invoke() == true));

      /// <summary>
      /// A save that wrote nothing, or not everything: why, in a Critical notice under
      /// the header - never in grey caption text or a message box. The pill keeps
      /// following the editors, which still hold what was not saved.
      /// </summary>
      public void Refuse(string sentence)
      {
         Say_(sentence, StatusLevel.Critical, null);
         Follow();
      }

      private void Show_(SettingsPageState state)
      {
         State = state;
         if (pill_ == null)
            return;

         pill_.Level = SettingsPageStatus.LevelFor(state);
         pill_.Text = SettingsPageStatus.WordFor(state);
         pill_.Visibility = SettingsPageStatus.ShowsPill(state) ? Visibility.Visible : Visibility.Collapsed;
      }

      private void Say_(string sentence, StatusLevel level, object action)
      {
         if (notice_ == null)
            return;

         notice_.Content = action;
         if (string.IsNullOrEmpty(sentence))
         {
            notice_.Visibility = Visibility.Collapsed;
            return;
         }

         notice_.Level = level;
         notice_.Text = sentence;
         notice_.Visibility = Visibility.Visible;
      }
   }
}
