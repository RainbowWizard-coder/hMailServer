// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Threading;
using hMailServer.ControlPanel.Services;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel
{
   /// <summary>
   /// The breadcrumb's Related links: as many as fit beside the trail, in the
   /// order the navigation map lists them, and the rest in a drop-down at their
   /// end (review finding F271). The bar drew the first three whatever the room,
   /// so a pair the map lists on purpose further down was never drawn. Which
   /// links fit is Services/RelatedLinksFit's rule; this builds every link,
   /// measures them in the window's own font and hides those that do not fit,
   /// again as the window's width changes.
   /// </summary>
   public partial class MainWindow
   {
      private readonly List<(NavNode Page, FrameworkElement Link)> related_ = new();
      private TextBlock relatedCaption_;
      private Wpf.Ui.Controls.Button relatedMore_;
      private bool relatedWired_;

      /// <summary>The Related links to <paramref name="targets"/>, every one built, fitted once the bar is laid out.</summary>
      private void BuildRelated_(IEnumerable<NavNode> targets)
      {
         RelatedHost.Children.Clear();
         related_.Clear();

         if (!relatedWired_)
         {
            relatedWired_ = true;
            BreadcrumbBar.SizeChanged += (s, e) =>
            {
               if (e.WidthChanged)
                  FitRelated_();
            };
         }

         foreach (NavNode target in targets)
            related_.Add((target, RelatedLink(target)));

         if (related_.Count == 0)
            return;

         relatedCaption_ = new TextBlock
         {
            Text = L("Related:"),
            FontSize = Typography.Caption,
            Margin = new Thickness(0, 0, 2, 0),
            VerticalAlignment = VerticalAlignment.Center
         };
         relatedCaption_.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorTertiaryBrush");
         RelatedHost.Children.Add(relatedCaption_);

         foreach ((NavNode _, FrameworkElement link) in related_)
            RelatedHost.Children.Add(link);

         relatedMore_ = new Wpf.Ui.Controls.Button
         {
            Appearance = Wpf.Ui.Controls.ControlAppearance.Transparent,
            FontSize = Typography.Caption,
            Padding = new Thickness(6, 2, 6, 2),
            Margin = new Thickness(2, 0, 0, 0),
            Cursor = System.Windows.Input.Cursors.Hand,
            ToolTip = L("Related pages that do not fit beside the trail"),
            Visibility = Visibility.Collapsed
         };
         AutomationProperties.SetAutomationId(relatedMore_, "breadcrumb-related-more");
         relatedMore_.Click += (s, e) => ShowMoreRelated_();
         RelatedHost.Children.Add(relatedMore_);

         // Once the bar has been laid out with the new trail, whose width is
         // what the links share the bar with.
         Dispatcher.BeginInvoke(DispatcherPriority.Loaded, new Action(FitRelated_));
      }

      /// <summary>
      /// Shows the links that fit beside the trail and puts the rest behind the
      /// drop-down: "More (n)" after the links drawn, or "Related pages (n)" alone
      /// when not even the first fits, the caption then going with the links.
      /// </summary>
      private void FitRelated_()
      {
         if (related_.Count == 0 || relatedMore_ == null || !BreadcrumbBar.IsVisible)
            return;

         var infinite = new Size(double.PositiveInfinity, double.PositiveInfinity);
         var widths = new List<double>();
         foreach ((NavNode _, FrameworkElement link) in related_)
         {
            link.Visibility = Visibility.Visible;
            link.Measure(infinite);
            widths.Add(link.DesiredSize.Width + link.Margin.Left + link.Margin.Right);
         }

         relatedMore_.Content = F("More ({0})", related_.Count);
         relatedMore_.Visibility = Visibility.Visible;
         relatedMore_.Measure(infinite);
         double moreWidth = relatedMore_.DesiredSize.Width + relatedMore_.Margin.Left + relatedMore_.Margin.Right;

         relatedCaption_.Visibility = Visibility.Visible;
         relatedCaption_.Measure(infinite);
         double captionWidth = relatedCaption_.DesiredSize.Width + relatedCaption_.Margin.Left + relatedCaption_.Margin.Right;

         // What the bar leaves the links: its width inside its padding, less Back
         // and Forward, the whole trail on one line, and a gap between them.
         double trail = 0;
         foreach (UIElement part in BreadcrumbHost.Children)
         {
            part.Measure(infinite);
            trail += part.DesiredSize.Width + (part is FrameworkElement f ? f.Margin.Left + f.Margin.Right : 0);
         }
         HistoryButtons.Measure(infinite);
         double history = HistoryButtons.DesiredSize.Width + HistoryButtons.Margin.Left + HistoryButtons.Margin.Right;
         double available = BreadcrumbBar.ActualWidth - BreadcrumbBar.Padding.Left - BreadcrumbBar.Padding.Right
            - history - trail - DesignTokens.Space.Md - captionWidth;

         int shown = RelatedLinksFit.Count(widths, available, moreWidth);
         for (int i = 0; i < related_.Count; i++)
            related_[i].Link.Visibility = i < shown ? Visibility.Visible : Visibility.Collapsed;

         int hidden = related_.Count - shown;
         relatedCaption_.Visibility = shown > 0 ? Visibility.Visible : Visibility.Collapsed;
         relatedMore_.Visibility = hidden > 0 ? Visibility.Visible : Visibility.Collapsed;
         relatedMore_.Content = shown > 0 ? F("More ({0})", hidden) : F("Related pages ({0})", hidden);
      }

      /// <summary>
      /// What the signed-in credential may open, for the Related links: the
      /// session's permissions, or null - every page - for a credential the
      /// server did not describe, as the tree is built (VisibleRoots_).
      /// </summary>
      private static Func<string, bool> SessionMayUse_()
      {
         ServerSession session = ServerSession.Current;
         return session == null || session.Permissions == null ? null : session.May;
      }

      /// <summary>The links that do not fit, as a menu under the drop-down, each opening its page.</summary>
      private void ShowMoreRelated_()
      {
         var menu = new ContextMenu
         {
            PlacementTarget = relatedMore_,
            Placement = System.Windows.Controls.Primitives.PlacementMode.Bottom
         };

         foreach ((NavNode page, FrameworkElement link) in related_)
         {
            if (link.Visibility == Visibility.Visible)
               continue;

            var item = new MenuItem { Header = L(page.Title), ToolTip = string.IsNullOrEmpty(page.Purpose) ? null : L(page.Purpose) };
            AutomationProperties.SetAutomationId(item, "breadcrumb-related-more-" + page.Key);
            string key = page.Key;
            item.Click += (s, e) => NavigateTo(key);
            menu.Items.Add(item);
         }

         if (menu.Items.Count > 0)
            menu.IsOpen = true;
      }
   }
}
