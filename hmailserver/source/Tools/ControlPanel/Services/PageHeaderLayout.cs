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
      /// Below this height of the page, a page that pins its header and a tab strip
      /// above its tabs (the settings pages ServerSettingsView builds) shows its
      /// subtitle as the title's tool tip, so that the room is the settings' (review
      /// finding F290): a 760 by 520 window gives a page some 395 pixels, and the
      /// title, a two-line subtitle, a two-line notice and the strip took all but 140.
      /// </summary>
      public const double SubtitleInToolTipBelow = 600;

      /// <summary>Whether a page this high shows its subtitle as the title's tool tip.</summary>
      public static bool SubtitleInToolTip(double pageHeight)
         => pageHeight > 0 && pageHeight < SubtitleInToolTipBelow;

      /// <summary>
      /// Whether the actions go under the subtitle: when the header's width, less
      /// the actions' (with their gap), cannot hold the title whole. The subtitle no
      /// longer shares the title's column, so it is not measured against them.
      /// </summary>
      public static bool ActionsGoUnder(double width, double actionsWidth, double titleWidth)
         => actionsWidth > 0 && width - actionsWidth < titleWidth;
   }
}
