// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>What an administrator answered when asked about a page's unsaved changes.</summary>
   public enum UnsavedAnswer
   {
      /// <summary>Save them, as the page's own Save does, and go on only if every one was saved.</summary>
      Save,

      /// <summary>Let them go: the page reads what the server holds the next time it is opened.</summary>
      Discard,

      /// <summary>Stay on the page with them, the safe answer, and the one Escape and Enter give.</summary>
      Stay
   }

   /// <summary>
   /// Whether a page with unsaved changes may be left, and the words that ask.
   ///
   /// The shell keeps a page once it has been built and every settings page reads
   /// the server again as it is opened, so an edit left behind by a click in the
   /// sidebar, a Related link, a palette result, closing the window or Reload was
   /// gone without a word when the page came back - and the pill beside its title
   /// had said "Unsaved changes" the whole time. Every way of leaving a page now asks
   /// first (MainWindow), every Reload asks before it reads again, and this is the
   /// part of that decision that needs no window: the questions, and what each
   /// answer leads to, held by UnsavedChangesTests.
   /// </summary>
   public static class UnsavedChanges
   {
      /// <summary>The question asked before a page with unsaved changes is left.</summary>
      public static string LeaveQuestion(string pageTitle) => F("You have unsaved changes on {0}.", pageTitle);

      /// <summary>The question a settings page's Reload asks while it holds unsaved changes.</summary>
      public static string ReloadQuestion => L("Discard your unsaved changes and read the settings again?");

      /// <summary>The question the event scripts page's Reload from disk asks while the script holds unsaved changes.</summary>
      public static string ReloadScriptQuestion => L("Discard your unsaved changes and read the script from disk again?");

      /// <summary>The question the IP ranges page asks before it shows another range's permissions over changed ones.</summary>
      public static string PermissionsQuestion(string rangeName) => F("The permissions of {0} have unsaved changes.", rangeName);

      /// <summary>
      /// Whether the page may be left: at once when nothing on it is unsaved, without
      /// asking; after Discard; after Save only when the save wrote everything - a
      /// save that failed anywhere leaves the page on screen with its own sentence
      /// saying what - and never after Stay.
      /// </summary>
      /// <param name="hasUnsavedChanges">Whether the page holds changes that have not reached the server.</param>
      /// <param name="ask">Asks the administrator; called only when there is something to ask about.</param>
      /// <param name="save">Saves the page; true when every change was saved.</param>
      public static bool MayLeave(bool hasUnsavedChanges, Func<UnsavedAnswer> ask, Func<bool> save)
      {
         if (!hasUnsavedChanges)
            return true;

         switch (ask())
         {
            case UnsavedAnswer.Discard:
               return true;
            case UnsavedAnswer.Save:
               return save();
            default:
               return false;
         }
      }
   }

   /// <summary>
   /// What a page's editors held when they were last read from the server or saved
   /// to it, so that the page can tell whether what is on screen now differs. Each
   /// editor is one string, in the order the page lists them; a password box is
   /// given as whether anything was typed in it, never as what was, so no secret is
   /// kept here.
   /// </summary>
   public sealed class EditorBaseline
   {
      private string[] values_;

      /// <summary>Whether the editors have been read since the page was made.</summary>
      public bool IsTaken => values_ != null;

      /// <summary>Records the editors as they are now: just read, or just saved.</summary>
      public void Take(IEnumerable<string> values)
         => values_ = (values ?? Enumerable.Empty<string>()).Select(value => value ?? "").ToArray();

      /// <summary>Forgets the record, so that nothing is unsaved until the next <see cref="Take"/>.</summary>
      public void Forget() => values_ = null;

      /// <summary>
      /// True when the editors hold something other than what was recorded. Before
      /// anything was recorded there is nothing to differ from, and nothing is unsaved.
      /// </summary>
      public bool Differs(IEnumerable<string> values)
      {
         if (values_ == null)
            return false;

         string[] now = (values ?? Enumerable.Empty<string>()).Select(value => value ?? "").ToArray();
         return !now.SequenceEqual(values_, StringComparer.Ordinal);
      }
   }
}
