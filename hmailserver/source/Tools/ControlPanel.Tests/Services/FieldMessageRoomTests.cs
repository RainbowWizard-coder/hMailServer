// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The room a dialog keeps under its body for its fields' messages, taken again
   /// as the body changes (review finding F213; FluentDialogWindow.KeepRoomForFieldErrors).
   /// The floor was measured once, when the dialog loaded: the access-control entry
   /// switched to Anyone collapsed its account box and stayed as tall as with it,
   /// 520 by 671 either way, about 110 pixels of blank above Save.
   /// </summary>
   public class FieldMessageRoomTests
   {
      // A body of 400 pixels in a window whose chrome is 150, capped at 900, with
      // room for one message line of 20.
      private const double Room = 20;
      private const double Cap = 900;
      private const double Chrome = 150;

      [Fact]
      public void TheFloor_IsTheBodyAndTheRoom()
      {
         (double floor, double grows) = FieldMessageRoom.Take(400, 0, Room, Cap, Chrome, true);
         Assert.Equal(420, floor);
         Assert.Equal(20, grows);
      }

      [Fact]
      public void AMessage_GrowsIntoTheRoom_AndTheFloorStays()
      {
         // The message's 20 pixels are in the body's height and taken out again.
         (double floor, _) = FieldMessageRoom.Take(420, 20, Room, Cap, Chrome, true);
         Assert.Equal(420, floor);
      }

      [Fact]
      public void ARowCollapsed_LowersTheFloor_ByTheRow()
      {
         // The account box's row, 48 pixels, collapsed for Anyone.
         (double before, _) = FieldMessageRoom.Take(400, 0, Room, Cap, Chrome, true);
         (double after, _) = FieldMessageRoom.Take(352, 0, Room, Cap, Chrome, true);
         Assert.Equal(48, before - after);
      }

      [Fact]
      public void NoRoom_WhileTheRowsThatCanBeRefusedAreCollapsed()
      {
         (double floor, double grows) = FieldMessageRoom.Take(352, 0, Room, Cap, Chrome, false);
         Assert.Equal(0, floor);
         Assert.Equal(0, grows);
      }

      [Fact]
      public void AtTheCap_TheRoomIsWhatTheWindowCanStillGrow()
      {
         // 150 + 740 = 890: ten pixels to the cap of 900.
         (double floor, double grows) = FieldMessageRoom.Take(740, 0, Room, Cap, Chrome, true);
         Assert.Equal(10, grows);
         Assert.Equal(750, floor);

         // Past it, the body scrolls and there is no floor.
         Assert.Equal((0.0, 0.0), FieldMessageRoom.Take(800, 0, Room, Cap, Chrome, true));
      }

      [Fact]
      public void MessagesOneUnderAnother_TakeTheSumOfTheirHeights()
      {
         Assert.Equal(56, FieldMessageRoom.Covered(new[] { (100.0, 120.0), (200.0, 236.0) }));
      }

      [Fact]
      public void MessagesBesideEachOther_TakeTheTallestOnce()
      {
         Assert.Equal(36, FieldMessageRoom.Covered(new[] { (100.0, 120.0), (100.0, 136.0) }));
         Assert.Equal(40, FieldMessageRoom.Covered(new[] { (100.0, 136.0), (120.0, 140.0) }));
      }

      [Fact]
      public void NoMessages_CoverNothing()
      {
         Assert.Equal(0, FieldMessageRoom.Covered(System.Array.Empty<(double, double)>()));
      }
   }
}
