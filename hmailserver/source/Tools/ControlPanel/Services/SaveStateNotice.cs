// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// What the notice under an hMailServer.INI settings page's header says, beside the
   /// pill of the one save-state model (<see cref="SettingsPageStatus"/>): the sentence
   /// of the state the page settled in after its last read, save or restart, at its
   /// level and with its one action - or, over it, why a save was refused or failed.
   ///
   /// A refusal made before anything was written - a port out of range, a new secret
   /// typed with the removal of the stored one ticked - is about the edits the save was
   /// asked to write, and is withdrawn once none is left: when the editors hold what was
   /// read or saved again, the notice goes back to the settled sentence. It stayed,
   /// Critical, saying "Clear the value or untick the box" under a page whose pill said
   /// that nothing was unsaved, over a value cleared and a box unticked, until the next
   /// save. A save that failed part of the way through keeps its sentence however the
   /// editors are set afterwards: some of its settings may have been written, and the
   /// notice is the one place that says the file may not hold what the page shows.
   ///
   /// WPF-free, the action held as an object the page made, so that a test can hold the
   /// sequences a page goes through (Views/IniPageSaveState draws it).
   /// </summary>
   public sealed class SaveStateNotice
   {
      private string settledSentence_;
      private StatusLevel settledLevel_ = StatusLevel.Information;
      private object settledAction_;

      // Set while the notice shows a refusal made before anything was written.
      private bool refusalBeforeWriting_;

      /// <summary>The sentence the notice shows now; null when it shows none and is collapsed.</summary>
      public string Sentence { get; private set; }

      /// <summary>The level the notice shows now.</summary>
      public StatusLevel Level { get; private set; } = StatusLevel.Information;

      /// <summary>The notice's one action now, or null.</summary>
      public object Action { get; private set; }

      /// <summary>Whether the notice shows a refusal or a failure of a save, rather than the settled sentence.</summary>
      public bool ShowsRefusal { get; private set; }

      /// <summary>
      /// After a read, a save or a step of a restart: the sentence that goes with the
      /// state the page settled in, which replaces any refusal.
      /// </summary>
      public void Settle(string sentence, StatusLevel level, object action)
      {
         settledSentence_ = string.IsNullOrEmpty(sentence) ? null : sentence;
         settledLevel_ = level;
         settledAction_ = action;
         refusalBeforeWriting_ = false;
         ShowSettled_();
      }

      /// <summary>
      /// A save refused before it wrote anything: why, at Critical, until the next read
      /// or save - or until no edit is left for it to be about (<see cref="Follow"/>).
      /// </summary>
      public void Refuse(string sentence)
      {
         Show_(sentence, StatusLevel.Critical, null, refusal: true);
         refusalBeforeWriting_ = true;
      }

      /// <summary>
      /// A save that failed as it wrote, which may have written part of what it was
      /// asked to: why, at Critical, until the next read or save.
      /// </summary>
      public void Fail(string sentence)
      {
         Show_(sentence, StatusLevel.Critical, null, refusal: true);
         refusalBeforeWriting_ = false;
      }

      /// <summary>
      /// After an edit: a refusal made before anything was written is withdrawn once
      /// the editors hold what was read or saved, and the settled sentence shows
      /// again. True when what the notice shows changed.
      /// </summary>
      /// <param name="hasUnsavedChanges">Whether an editor still differs from what was read or saved.</param>
      public bool Follow(bool hasUnsavedChanges)
      {
         if (!refusalBeforeWriting_ || hasUnsavedChanges)
            return false;

         refusalBeforeWriting_ = false;
         ShowSettled_();
         return true;
      }

      private void ShowSettled_() => Show_(settledSentence_, settledLevel_, settledAction_, refusal: false);

      private void Show_(string sentence, StatusLevel level, object action, bool refusal)
      {
         Sentence = string.IsNullOrEmpty(sentence) ? null : sentence;
         Level = level;
         Action = Sentence == null ? null : action;
         ShowsRefusal = refusal && Sentence != null;
      }
   }
}
