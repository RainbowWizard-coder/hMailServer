// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Collections.Generic;
using System.Linq;
using System.Windows;
using hMailServer.ControlPanel.Services;
using hMailServer.ControlPanel.Views.Scaffold;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// What the three read-only overviews - transport encryption, spam filtering,
   /// virus scanning - share in saying where a server stands: the verdict as a
   /// notice at its level where no note says it, the problems right under it,
   /// each with the page that puts it right, and their count beside the page's
   /// title.
   ///
   /// The verdict was plain body text with no level, and the Critical and Warning
   /// notes were the last card, several screens down, with no way to the page
   /// that fixes them.
   /// </summary>
   internal static class OverviewNotices
   {
      /// <summary>
      /// A notice at <paramref name="level"/>, with a button that opens
      /// <paramref name="page"/> where one is given (Services/NoticeActions).
      /// </summary>
      public static InlineNotice Notice(StatusLevel level, string text, string page, string automationId)
      {
         var notice = new InlineNotice
         {
            Level = level,
            Text = text,
            Content = page == null ? null : NoticeActions.OpenPage(page, automationId + "-open-" + page)
         };
         System.Windows.Automation.AutomationProperties.SetAutomationId(notice, automationId);
         return notice;
      }

      /// <summary>
      /// The verdict where the notes under it say it at its level: the card's
      /// lead sentence in body text, at no level and with no action, so that the
      /// only notices at the Warning level and above are the problems the count
      /// beside the title counts. It still says what the notes do not - how a
      /// message is scored and what becomes of it, how many scanners examine it.
      /// </summary>
      public static System.Windows.Controls.TextBlock Lead(string text, string automationId)
      {
         var lead = new System.Windows.Controls.TextBlock { Text = text, Margin = new Thickness(0, 0, 0, 8) };
         lead.SetResourceReference(FrameworkElement.StyleProperty, "TextBody");
         System.Windows.Automation.AutomationProperties.SetAutomationId(lead, automationId);
         return lead;
      }

      /// <summary>
      /// The problems among a page's notes - Warning and above - worst first, in
      /// the order the page gave them within a level.
      /// </summary>
      public static List<T> Problems<T>(IEnumerable<T> notes, System.Func<T, StatusLevel> level)
         => notes.Where(n => level(n) >= StatusLevel.Warning).OrderByDescending(level).ToList();

      /// <summary>The rest of a page's notes, below Warning, in the order the page gave them.</summary>
      public static List<T> WorthKnowing<T>(IEnumerable<T> notes, System.Func<T, StatusLevel> level)
         => notes.Where(n => level(n) < StatusLevel.Warning).ToList();

      /// <summary>
      /// The pill beside the page's title: how many problems there are, at the
      /// worst one's level, or that none was found.
      /// </summary>
      public static StatusPill ProblemsPill(int problems, StatusLevel worst)
      {
         var pill = new StatusPill
         {
            Level = problems == 0 ? StatusLevel.Good : worst,
            Text = problems == 0 ? L("No problems found")
               : problems == 1 ? L("1 problem")
               : F("{0} problems", problems)
         };
         System.Windows.Automation.AutomationProperties.SetAutomationId(pill, "overview-problems");
         return pill;
      }
   }
}
