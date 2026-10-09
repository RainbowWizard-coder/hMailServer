// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Automation.Peers;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// Says a sentence to a screen reader without moving the keyboard: a UI
   /// Automation notification raised on an element of the window it concerns.
   ///
   /// A live region (Services/Announce) says what an element shows when it
   /// changes; this says what has no element of its own to show it - the row the
   /// palette's arrow keys moved to while the keyboard stays in its search box,
   /// the range a dashboard chart now covers. The tour card's sentence is said
   /// the same way. Each kind of sentence has its own activity id, so that a
   /// reader replaces the last one of that kind rather than queueing them, which
   /// is what arrowing through a list needs: the row arrived at, not every row
   /// passed on the way.
   /// </summary>
   public static class ScreenReaderNotice
   {
      /// <summary>
      /// Says <paramref name="sentence"/> on <paramref name="source"/>'s behalf.
      /// Nothing is raised when no automation client is listening, and nothing
      /// here throws: a sentence that failed because nothing was listening would
      /// be worse than one that is quiet.
      /// </summary>
      public static void Say(UIElement source, string sentence, string activityId)
      {
         if (source == null || string.IsNullOrEmpty(sentence))
            return;

         try
         {
            AutomationPeer peer = UIElementAutomationPeer.FromElement(source) ?? UIElementAutomationPeer.CreatePeerForElement(source);
            peer?.RaiseNotificationEvent(AutomationNotificationKind.ActionCompleted,
               AutomationNotificationProcessing.MostRecent, sentence, activityId ?? "hMailServerNotice");
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            // No peer could be made, or the client went away. The screen still shows it.
         }
      }
   }
}
