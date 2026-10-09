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
   /// restart, a refusal over it, and the refusal withdrawn once the edits it refused are
   /// taken back - when nothing was written. The refusal stayed until the next save, so
   /// that the Authentication page said, Critical, "Clear the value or untick the box"
   /// over a value cleared and a box unticked, beside no pill.
   /// </summary>
   public class SaveStateNoticeTests
   {
      private const string Saved = "Saved 10:00:00 - every setting on this page is in effect.";
      private const string Refused = "A new value has been typed for \"Shared HMAC secret\", and \"Remove the stored secret when saving\" is ticked under it.";
      private const string Failed = "Could not save: access denied.";

      [Fact]
      public void ARefusalBeforeWriting_StandsWhileAnEditIsLeft_AndGoesWithTheLast()
      {
         var notice = new SaveStateNotice();
         notice.Settle(Saved, StatusLevel.Good, null);

         notice.Refuse(Refused);
         Assert.Equal(Refused, notice.Sentence);
         Assert.Equal(StatusLevel.Critical, notice.Level);
         Assert.True(notice.ShowsRefusal);

         // One of the two edits taken back: the page still holds an unsaved change, and
         // the refusal is still about it.
         Assert.False(notice.Follow(hasUnsavedChanges: true));
         Assert.Equal(Refused, notice.Sentence);

         // The other: nothing is left for the refusal to be about, and the notice says
         // again what the last save said, at its level.
         Assert.True(notice.Follow(hasUnsavedChanges: false));
         Assert.Equal(Saved, notice.Sentence);
         Assert.Equal(StatusLevel.Good, notice.Level);
         Assert.False(notice.ShowsRefusal);

         // And it is withdrawn once: a later edit does not bring it back.
         Assert.False(notice.Follow(hasUnsavedChanges: true));
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
      }

      [Fact]
      public void TheSettledAction_ComesBackWithItsSentence()
      {
         var restart = new object();
         var notice = new SaveStateNotice();
         notice.Settle("Saved 10:00:00 - the new schedule takes effect when the service restarts.", StatusLevel.Information, restart);
         Assert.Same(restart, notice.Action);

         // A refusal carries no action: Restart the service… beside "the port must be a
         // number" would restart over values that were never written.
         notice.Refuse("Port must be a whole number from 0 to 65535.");
         Assert.Null(notice.Action);

         Assert.True(notice.Follow(hasUnsavedChanges: false));
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
         notice.Refuse(Refused);

         notice.Settle("Saved 10:05:00 - every setting on this page is in effect.", StatusLevel.Good, null);
         Assert.False(notice.ShowsRefusal);
         Assert.False(notice.Follow(hasUnsavedChanges: false));
         Assert.Equal("Saved 10:05:00 - every setting on this page is in effect.", notice.Sentence);

         // A failure after a refusal is a failure: kept.
         notice.Refuse(Refused);
         notice.Fail(Failed);
         Assert.False(notice.Follow(hasUnsavedChanges: false));
         Assert.Equal(Failed, notice.Sentence);
      }
   }
}
