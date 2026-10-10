// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// Where a page header's parts go (Views/Scaffold/PageHeader): the title and its
   /// pill on the first line with the page's actions beside them, the subtitle on a
   /// line of its own under both, and the actions under the subtitle when the title
   /// would not fit beside them whole. Free of WPF, so that the rule is tested
   /// without a window.
   /// </summary>
   public static class PageHeaderLayout
   {
      /// <summary>
      /// Whether the actions go under the subtitle: when the header's width, less
      /// the actions' (with their gap), cannot hold the title whole. The subtitle no
      /// longer shares the title's column, so it is not measured against them.
      /// </summary>
      public static bool ActionsGoUnder(double width, double actionsWidth, double titleWidth)
         => actionsWidth > 0 && width - actionsWidth < titleWidth;
   }
}
