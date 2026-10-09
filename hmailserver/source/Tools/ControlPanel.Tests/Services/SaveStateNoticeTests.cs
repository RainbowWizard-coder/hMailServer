// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The notice under an hMailServer.INI settings page's header (Services/SaveStateNotice,
   /// which Views/IniPageSaveState draws): the settled sentence of the last read, save or
   /// restart, a refusal over it, and the refusal withdrawn - when nothing was written -
   /// once the reader puts its cause right, though another edit is still unsaved, or takes
   /// every edit back. The refusal stayed until the next save while any edit was left, so
   /// that the Authentication page said, Critical, "... is ticked under it. Clear the value
   /// or untick the box" over the box unticked as it asked, and Directory authentication
   /// "Port must be between 0 and 65535." over 636; and before that, until the next save
   /// whatever the editors held, over a value cleared and a box unticked beside no pill.
   /// </summary>
   public class SaveStateNoticeTests
   {
      private const string Saved = "Saved 10:00:00 - every setting on this page is in effect.";
      private const string Refused = "A new value has been typed for \"Shared HMAC secret\", and \"Remove the stored secret when saving\" is ticked under it.";
      private const string Failed = "Could not save: access denied.";

      [Fact]
      public void ARefusalBeforeWriting_GoesWhenItsCauseIsPutRight_ThoughAnotherEditIsLeft()
      {
         bool ticked = true;
         var notice = new SaveStateNotice();
         notice.Settle(Saved, StatusLevel.Good, null);

         notice.Refuse(Refused, () => ticked);
         Assert.Equal(Refused, notice.Sentence);
         Assert.Equal(StatusLevel.Critical, notice.Level);
         Assert.True(notice.ShowsRefusal);

         // An edit that has nothing to do with it - another field typed in - leaves the
         // refusal: the value and the box it names are still both there.
         Assert.False(notice.Follow(hasUnsavedChanges: true));
         Assert.Equal(Refused, notice.Sentence);
         Assert.True(notice.ShowsRefusal);

         // The box unticked, as the refusal asks, with the value still typed and the
         // other edit still unsaved: what it says is no longer so, and the notice says
         // again what the last save said, at its level.
         ticked = false;
         Assert.True(notice.Follow(hasUnsavedChanges: true));
         Assert.Equal(Saved, notice.Sentence);
         Assert.Equal(StatusLevel.Good, notice.Level);
         Assert.False(notice.ShowsRefusal);

         // And it is withdrawn once: the box ticked again does not bring it back - the
         // next save says it, if the reader asks for one.
         ticked = true;
         Assert.False(notice.Follow(hasUnsavedChanges: true));
         Assert.Equal(Saved, notice.Sentence);
         Assert.False(notice.ShowsRefusal);
      }

      [Fact]
      public void ARefusalBeforeWriting_GoesWithTheLastEdit_WhateverItsConditionSays()
      {
         // Every edit taken back: nothing is left for the refusal to be about, whether or
         // not it named a condition to ask again.
         var notice = new SaveStateNotice();
         notice.Settle(Saved, StatusLevel.Good, null);
         notice.Refuse(Refused, () => true);
         Assert.True(notice.Follow(hasUnsavedChanges: false));
         Assert.Equal(Saved, notice.Sentence);

         // A refusal that names no condition stands while an edit is left, and goes with
         // the last.
         notice.Refuse(Refused);
         Assert.False(notice.Follow(hasUnsavedChanges: true));
         Assert.Equal(Refused, notice.Sentence);
         Assert.True(notice.Follow(hasUnsavedChanges: false));
         Assert.Equal(Saved, notice.Sentence);
      }

      [Fact]
      public void ARefusalOnAPageThatHadNothingToSay_LeavesNoNotice_WhenWithdrawn()
      {
         var notice = new SaveStateNotice();
         notice.Settle(null, StatusLevel.Normal, null);
         notice.Refuse(Refused);

         Assert.True(notice.Follow(hasUnsavedChanges: false));
         Assert.Null(notice.Sentence);
         Assert.Null(notice.Action);

         // The same when its cause is put right with another edit left.
         notice.Refuse("Port must be between 0 and 65535.", () => false);
         Assert.True(notice.Follow(hasUnsavedChanges: true));
         Assert.Null(notice.Sentence);
         Assert.Null(notice.Action);
      }

      [Fact]
      public void TheSettledAction_ComesBackWithItsSentence()
      {
         bool portOutOfRange = true;
         var restart = new object();
         var notice = new SaveStateNotice();
         notice.Settle("Saved 10:00:00 - the new schedule takes effect when the service restarts.", StatusLevel.Information, restart);
         Assert.Same(restart, notice.Action);

         // A refusal carries no action: Restart the service… beside "Port must be
         // between 0 and 65535." would restart over values that were never written.
         notice.Refuse("Port must be between 0 and 65535.", () => portOutOfRange);
         Assert.Null(notice.Action);

         // The port put right, another edit still unsaved.
         portOutOfRange = false;
         Assert.True(notice.Follow(hasUnsavedChanges: true));
         Assert.Same(restart, notice.Action);
         Assert.Equal(StatusLevel.Information, notice.Level);
      }

      [Fact]
      public void AFailedWrite_StaysWhenTheEditsAreTakenBack()
      {
         // Some of the settings may have been written before the one that failed, so the
         // file may not hold what the page shows: the notice is the one place that says so.
         var notice = new SaveStateNotice();
         notice.Settle(Saved, StatusLevel.Good, null);
         notice.Fail(Failed);

         Assert.False(notice.Follow(hasUnsavedChanges: true));
         Assert.False(notice.Follow(hasUnsavedChanges: false));
         Assert.Equal(Failed, notice.Sentence);
         Assert.Equal(StatusLevel.Critical, notice.Level);
         Assert.True(notice.ShowsRefusal);
      }

      [Fact]
      public void AReadOrASave_ReplacesARefusal_AndIsNotWithdrawn()
      {
         var notice = new SaveStateNotice();
         notice.Settle(Saved, StatusLevel.Good, null);
         notice.Refuse(Refused, () => false);

         notice.Settle("Saved 10:05:00 - every setting on this page is in effect.", StatusLevel.Good, null);
         Assert.False(notice.ShowsRefusal);
         Assert.False(notice.Follow(hasUnsavedChanges: true));
         Assert.False(notice.Follow(hasUnsavedChanges: false));
         Assert.Equal("Saved 10:05:00 - every setting on this page is in effect.", notice.Sentence);

         // A failure after a refusal is a failure, kept: the refusal's condition, put
         // right since, does not withdraw it.
         notice.Refuse(Refused, () => false);
         notice.Fail(Failed);
         Assert.False(notice.Follow(hasUnsavedChanges: true));
         Assert.False(notice.Follow(hasUnsavedChanges: false));
         Assert.Equal(Failed, notice.Sentence);
      }
   }
}
