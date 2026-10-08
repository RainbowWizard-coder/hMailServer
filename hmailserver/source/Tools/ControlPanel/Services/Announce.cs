// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Automation.Peers;
using System.Windows.Threading;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// Says a live region's new text to a screen reader.
   ///
   /// Marking an element <c>AutomationProperties.LiveSetting="Polite"</c> (or
   /// Assertive) is a promise, not an announcement: a reader says the region's
   /// name when the element raises <see cref="AutomationEvents.LiveRegionChanged"/>,
   /// and nothing in this program raised it. Every notice, loading state, empty
   /// state, field error and result line declared itself a live region and was
   /// silent - "Released 3 messages", "Deleted.", "Could not load…" and every Save
   /// or Test outcome reached a sighted administrator and nobody else.
   ///
   /// <see cref="Request"/> is what a component calls when its text changed or it
   /// came into view. The event goes once per dispatcher turn and element, at
   /// Loaded priority - after the layout that shows the element, so that a text
   /// set while the element was collapsed and the Visible that follows in the same
   /// handler are one announcement, not two and not none - and only while the
   /// element is visible, is a live region and has a name to say. Nothing is
   /// raised when no automation client is listening, which is most of the time,
   /// and nothing here throws: a notice that failed because nothing was listening
   /// would be worse than one that is quiet.
   /// </summary>
   public static class Announce
   {
      // Set while an announcement is queued for the element, so that several
      // changes in one handler raise one event.
      private static readonly DependencyProperty PendingProperty = DependencyProperty.RegisterAttached(
         "AnnouncePending", typeof(bool), typeof(Announce), new PropertyMetadata(false));

      /// <summary>
      /// Queues the announcement of <paramref name="element"/>'s current name, as
      /// a live region. Harmless on an element that is not one, is collapsed, or
      /// has nothing to say: those are decided when the queue runs, not now.
      /// </summary>
      public static void Request(UIElement element)
      {
         if (element == null || (bool)element.GetValue(PendingProperty))
            return;

         element.SetValue(PendingProperty, true);
         element.Dispatcher.BeginInvoke(DispatcherPriority.Loaded, new Action(() => Raise_(element)));
      }

      private static void Raise_(UIElement element)
      {
         element.SetValue(PendingProperty, false);

         try
         {
            if (!element.IsVisible || AutomationProperties.GetLiveSetting(element) == AutomationLiveSetting.Off)
               return;

            if (!AutomationPeer.ListenerExists(AutomationEvents.LiveRegionChanged))
               return;

            AutomationPeer peer = UIElementAutomationPeer.FromElement(element) ?? UIElementAutomationPeer.CreatePeerForElement(element);
            if (peer == null || string.IsNullOrEmpty(peer.GetName()))
               return;

            peer.RaiseAutomationEvent(AutomationEvents.LiveRegionChanged);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            // No peer could be made, or the automation client went away between
            // the check and the event. The text is still on screen.
         }
      }
   }
}
