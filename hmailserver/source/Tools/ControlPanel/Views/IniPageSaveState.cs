// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Threading;
using hMailServer.ControlPanel.Services;
using InlineNotice = hMailServer.ControlPanel.Views.Scaffold.InlineNotice;
using MessageBox = hMailServer.ControlPanel.Views.Dialogs;
using StatusPill = hMailServer.ControlPanel.Views.Scaffold.StatusPill;
using static hMailServer.ControlPanel.Services.Loc;

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
   /// that wrote nothing says why in a Critical notice - withdrawn, when it was refused
   /// before anything was written, once the reader puts its cause right or takes the
   /// edits back (Services/SaveStateNotice).
   /// </summary>
   internal sealed class IniPageSaveState
   {
      private readonly StatusPill pill_;
      private readonly InlineNotice notice_;
      private readonly Func<bool> hasUnsavedChanges_;
      private readonly Func<IInputElement> keyboardHome_;
      private readonly SaveStateNotice sentence_ = new();
      private SettingsPageState settled_ = SettingsPageState.Clean;

      /// <param name="pill">The pill in the page header's Status slot.</param>
      /// <param name="notice">The notice under the header, collapsed while there is nothing to say.</param>
      /// <param name="hasUnsavedChanges">The page's own answer: whether an editor differs from what was read or saved.</param>
      /// <param name="keyboardHome">
      /// Where the keyboard goes when the notice's action had it and goes away - its
      /// Restart the service…, pressed, as the restart begins: the page's Save changes,
      /// the header's control above the notice. Null for a page whose notice carries no
      /// action.
      /// </param>
      public IniPageSaveState(StatusPill pill, InlineNotice notice, Func<bool> hasUnsavedChanges, Func<IInputElement> keyboardHome = null)
      {
         pill_ = pill;
         notice_ = notice;
         hasUnsavedChanges_ = hasUnsavedChanges;
         keyboardHome_ = keyboardHome;
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
         sentence_.Settle(sentence, level ?? SettingsPageStatus.LevelFor(state), action);
         Say_();
         Follow();
      }

      /// <summary>
      /// After an edit: Unsaved changes while an editor differs from what was read or
      /// saved, and the settled state again once none does. The notice is left alone -
      /// what a save or a read said stays true after the first keystroke - but for a
      /// refusal made before anything was written, which goes once its cause is put
      /// right or the editors hold what was read or saved again, the settled sentence
      /// coming back in its place.
      /// </summary>
      public void Follow()
      {
         bool unsaved = hasUnsavedChanges_?.Invoke() == true;
         if (sentence_.Follow(unsaved))
            Say_();
         Show_(SettingsPageStatus.Follow(settled_, unsaved));
      }

      /// <summary>
      /// A save refused before it wrote anything: why, in a Critical notice under the
      /// header - never in grey caption text or a message box - until the next read or
      /// save, until <paramref name="stillRefused"/> answers false after an edit - the
      /// reader did what the refusal asks, though another edit is still unsaved - or
      /// until the edits are all taken back. The pill keeps following the editors,
      /// which still hold what was not saved. The notice is brought into view, with the
      /// page header above it where both fit: the save the question before leaving a
      /// page makes is refused with the page scrolled wherever the edit was, and a
      /// refusal a message box put in front of the reader must not wait above the fold.
      /// </summary>
      /// <param name="sentence">Why the save was refused.</param>
      /// <param name="stillRefused">
      /// The refusal's own condition, asked again on every edit <see cref="Follow"/>
      /// sees: whether the editors still hold what the save was refused for.
      /// </param>
      public void Refuse(string sentence, Func<bool> stillRefused = null)
      {
         sentence_.Refuse(sentence, stillRefused);
         SayRefusal_();
      }

      /// <summary>
      /// A save that failed as it wrote, and may have written part of what it was asked
      /// to: why, as <see cref="Refuse"/> says it, until the next read or save - kept
      /// when the edits are taken back by hand, since the file may hold some of them.
      /// </summary>
      public void Fail(string sentence)
      {
         sentence_.Fail(sentence);
         SayRefusal_();
      }

      private void SayRefusal_()
      {
         Say_();
         Follow();

         if (notice_ == null || notice_.Visibility != Visibility.Visible)
            return;

         notice_.UpdateLayout();
         ScrollViewer scroll = null;
         for (DependencyObject at = VisualTreeHelper.GetParent(notice_); at != null && scroll == null; at = VisualTreeHelper.GetParent(at))
            scroll = at as ScrollViewer;

         // From the top of the page down to the notice, so the header's pill and
         // actions show with it, where that fits in the window; the notice alone where
         // it does not. One request: a second is measured from the offset the first
         // has not applied yet, and undoes it.
         double above = 0;
         if (scroll?.Content is UIElement page && page.IsAncestorOf(notice_))
            above = Math.Max(0, notice_.TranslatePoint(new Point(0, 0), page).Y);
         if (above > 0 && above + notice_.ActualHeight <= scroll.ViewportHeight)
            notice_.BringIntoView(new Rect(0, -above, Math.Max(1, notice_.ActualWidth), notice_.ActualHeight + above));
         else
            notice_.BringIntoView();
      }

      /// <summary>
      /// The notice's action for a saved change that waits for a service restart:
      /// Restart the service…, the window's pending bar's restart, which asks
      /// <paramref name="question"/> first, with Cancel the answer Enter gives, and
      /// settles the page as the restart goes (Services/ServiceRestart). Null when the
      /// server is not the one on this machine, which a restart from here cannot reach.
      /// </summary>
      public Wpf.Ui.Controls.Button RestartAction(string automationId, string question)
      {
         if (!ServiceRestart.Available)
            return null;

         var button = new Wpf.Ui.Controls.Button
         {
            Content = L("Restart the service…"),
            Appearance = Wpf.Ui.Controls.ControlAppearance.Secondary,
            VerticalAlignment = VerticalAlignment.Center
         };
         AutomationProperties.SetAutomationId(button, automationId);
         button.Click += async (s, e) => await RestartAsync_(automationId, question);
         return button;
      }

      /// <summary>Asks, restarts the service, and says how it went.</summary>
      private async Task RestartAsync_(string automationId, string question)
      {
         if (!MessageBox.Confirm(question, L("Control Panel"), L("_Restart the service"), cancelByDefault: true))
            return;

         Settle(SettingsPageState.Restarting, L("Restarting the hMailServer service\u2026"), StatusLevel.Information);
         (ServiceRestartOutcome outcome, string reason) = await ServiceRestart.RunAsync();
         switch (outcome)
         {
            case ServiceRestartOutcome.NotRestarted:
               Settle(SettingsPageState.RestartFailed, F("Could not restart the service: {0}", reason), action: RestartAction(automationId, question));
               break;
            case ServiceRestartOutcome.NotReattached:
               Settle(SettingsPageState.NotConnected,
                  F("The service was restarted but the Control Panel could not reconnect to it: {0}\n\nIt will keep trying as you use the application.", reason));
               break;
            default:
               Settle(SettingsPageState.Applied, L("Service restarted - settings are live."));
               break;
         }
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

      /// <summary>
      /// Draws what the notice says now (<see cref="sentence_"/>). The keyboard, when it
      /// was on an action the notice drops - its Restart the service…, pressed, as the
      /// restart begins - goes to the action that replaces it where there is one, and
      /// otherwise to the page's keyboard home: it went to the window, so that Tab
      /// started again from the window's first control and a screen reader said
      /// nothing of where the keyboard was.
      /// </summary>
      private void Say_()
      {
         if (notice_ == null)
            return;

         bool keyboardOnAction = notice_.Content is UIElement before && HoldsKeyboard_(before);
         object action = sentence_.Action;
         bool actionGoes = !ReferenceEquals(notice_.Content, action);

         notice_.Content = action;
         if (sentence_.Sentence == null)
            notice_.Visibility = Visibility.Collapsed;
         else
         {
            notice_.Level = sentence_.Level;
            notice_.Text = sentence_.Sentence;
            notice_.Visibility = Visibility.Visible;
         }

         if (keyboardOnAction && (actionGoes || notice_.Visibility != Visibility.Visible))
            KeepKeyboard_(notice_.Visibility == Visibility.Visible ? action as UIElement : null);
      }

      /// <summary>
      /// Whether <paramref name="element"/> has the keyboard, or is what its window gives
      /// the keyboard back to as it is activated again: the question Restart the
      /// service… asks first is a window of its own, and the page's window takes the
      /// keyboard back after it closes, to the element its focus scope remembers.
      /// </summary>
      private static bool HoldsKeyboard_(UIElement element)
      {
         if (element.IsKeyboardFocusWithin)
            return true;

         DependencyObject scope = FocusManager.GetFocusScope(element);
         return scope != null && ReferenceEquals(FocusManager.GetFocusedElement(scope), element);
      }

      /// <summary>
      /// Puts the keyboard on <paramref name="next"/> once it is laid out, or on the
      /// page's keyboard home where there is no next action or it cannot take it -
      /// through Focus, which a window that is not active yet keeps as the element it
      /// gives the keyboard to when it is.
      /// </summary>
      private void KeepKeyboard_(UIElement next)
      {
         IInputElement home = keyboardHome_?.Invoke();
         if (next == null)
         {
            home?.Focus();
            return;
         }

         // The action became the notice's content a moment ago, and is not in the
         // visual tree until the layout that follows.
         notice_.Dispatcher.BeginInvoke(DispatcherPriority.Loaded, new Action(() =>
         {
            if (next.IsVisible && next.IsEnabled && next.Focusable && next.Focus())
               return;
            home?.Focus();
         }));
      }
   }
}
