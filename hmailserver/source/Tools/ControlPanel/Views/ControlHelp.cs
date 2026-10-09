// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Windows;
using System.Windows.Automation;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// What a control does, said beside its name rather than in place of it.
   ///
   /// A button's accessible name is the words on it - "Refresh", "Preview",
   /// "Delete selected" - which WPF takes from its caption, Alt key and all. Some
   /// thirty buttons had their name replaced by a sentence that explained them:
   /// Refresh was "Re-check the external prerequisites", Preview was "Show what
   /// a synchronisation would do, changing nothing", Members... was "Edit the
   /// members of the selected group". A screen reader then said words nobody
   /// could see, and somebody using voice control who said "click Refresh" or
   /// "click Preview" reached nothing (WCAG 2.5.3, Label in Name; review
   /// findings F017, F127, F202). The sentence is the control's help text now,
   /// which a screen reader says after the name, and its tool tip where it had
   /// none, so a sighted reader can read it too. LabelInNameTests holds every
   /// captioned control to a name that starts with its caption.
   /// </summary>
   public static class ControlHelp
   {
      public static void Explain(FrameworkElement control, string explanation)
      {
         if (control == null || string.IsNullOrEmpty(explanation))
            return;

         AutomationProperties.SetHelpText(control, explanation);
         control.ToolTip ??= explanation;
      }

      /// <summary>
      /// Names a link whose caption says less than where it goes - one of ten
      /// "Settings…" on the spam filtering overview - by its caption and then its
      /// destination (Services/LabelInName.LinkName), so that the ten are told
      /// apart and each still starts with the words on it; the destination is
      /// its tool tip.
      /// </summary>
      public static void NameLink(FrameworkElement control, string caption, string destination)
      {
         if (control == null)
            return;

         AutomationProperties.SetName(control, hMailServer.ControlPanel.Services.LabelInName.LinkName(caption, destination));
         if (!string.IsNullOrEmpty(destination))
            control.ToolTip ??= destination;
      }
   }
}
