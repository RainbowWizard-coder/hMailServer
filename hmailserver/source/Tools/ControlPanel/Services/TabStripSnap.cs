// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// Where an overflowing tab strip scrolls to, by whole headers (review findings
   /// F193 and F107). Plain numbers, so that a test holds every case; the strip
   /// itself is <see cref="TabStripScrolling"/>.
   ///
   /// The strip scrolled by pixels - 48 a wheel notch, four fifths of its width a
   /// chevron click, and Windows' least distance to bring the selected header in
   /// - and so began and ended mid-word: the domain dialog's strip read
   /// "… ing and disclaimer" at its left edge, the account dialog's "… al", and
   /// "Tagg" and "Ex" at their right. A step here lands with a header's left edge
   /// at the strip's left edge, so the first header shown is whole; the one step
   /// that cannot, the last, which ends where the strip ends, leaves the header
   /// cut at the left, and <see cref="CutAtLeft"/> and <see cref="CutAtRight"/>
   /// say where a header is cut, so that the strip fades that edge and the cut
   /// header reads as one that goes on under the chevron.
   ///
   /// A header is its left edge and its width in the strip's own coordinates, in
   /// the order the headers are laid out; the offset is the strip's horizontal
   /// scroll offset, the viewport its visible width and the scrollable width how
   /// far it can scroll (ScrollViewer.ScrollableWidth).
   /// </summary>
   public static class TabStripSnap
   {
      /// <summary>
      /// Sub-pixel slack: offsets are doubles, and a scroll viewer can land a
      /// fraction short of where it was asked to go.
      /// </summary>
      public const double Epsilon = 0.5;

      /// <summary>How wide the fade is at an edge where a header is cut.</summary>
      public const double FadeWidth = 24;

      /// <summary>
      /// The offset one step forward: the left edge of the first header that is
      /// not wholly shown at the right, put at the left, or - for a header wider
      /// than the strip, or where there is none - one header further than now.
      /// Never past the end of the strip.
      /// </summary>
      public static double Forward(IReadOnlyList<(double Left, double Width)> headers, double offset, double viewport, double scrollable)
      {
         if (headers == null || headers.Count == 0 || scrollable <= Epsilon)
            return Clamp(offset, scrollable);

         double right = offset + viewport;
         foreach ((double left, double width) in headers)
         {
            // The first header the right edge cuts or does not reach.
            if (left + width > right + Epsilon && left > offset + Epsilon)
               return Clamp(left, scrollable);
         }

         // Every header that is not wholly shown starts at or before the left
         // edge: one header on, whatever its width.
         return Clamp(NextEdge_(headers, offset) ?? scrollable, scrollable);
      }

      /// <summary>
      /// The offset one step back: the strip moved right by up to a viewport, its
      /// left edge on a header's, so that the header that was first shown is the
      /// last whole one; one header back where that header is wider than the
      /// strip. Never before the start.
      /// </summary>
      public static double Back(IReadOnlyList<(double Left, double Width)> headers, double offset, double viewport, double scrollable)
      {
         if (headers == null || headers.Count == 0 || offset <= Epsilon)
            return 0;

         // The first header the left edge cuts or that starts at it: what is to
         // stay in view at the right after the step.
         double keepRight = offset;
         foreach ((double left, double width) in headers)
         {
            if (left + width > offset + Epsilon)
            {
               keepRight = left > offset - Epsilon ? left : left + width;
               break;
            }
         }

         // The earliest header edge from which everything up to keepRight fits.
         foreach ((double left, double _) in headers)
         {
            if (left < offset - Epsilon && keepRight - left <= viewport + Epsilon)
               return Clamp(left, scrollable);
         }

         return Clamp(PreviousEdge_(headers, offset) ?? 0, scrollable);
      }

      /// <summary>
      /// The offset one wheel notch on: the next header's left edge in the
      /// direction of the notch (a positive <paramref name="direction"/> forward).
      /// </summary>
      public static double Notch(IReadOnlyList<(double Left, double Width)> headers, double offset, double scrollable, int direction)
      {
         if (headers == null || headers.Count == 0)
            return Clamp(offset, scrollable);

         double? edge = direction > 0 ? NextEdge_(headers, offset) : PreviousEdge_(headers, offset);
         return Clamp(edge ?? (direction > 0 ? scrollable : 0), scrollable);
      }

      /// <summary>
      /// The offset that shows header <paramref name="index"/> whole, moving the
      /// strip as little as a whole-header step allows: left of the strip, its left
      /// edge at the strip's; right of it, the first header edge from which it
      /// fits; already whole, the offset as it is - moved on to the next header's
      /// edge where the strip's left edge cuts a header before it, as Windows' own
      /// least-distance scroll to the selected header leaves it.
      /// </summary>
      public static double Reveal(IReadOnlyList<(double Left, double Width)> headers, int index, double offset, double viewport, double scrollable)
      {
         if (headers == null || index < 0 || index >= headers.Count)
            return Clamp(offset, scrollable);

         (double left, double width) = headers[index];
         if (left < offset - Epsilon)
            return Clamp(left, scrollable);

         double right = left + width;
         if (right <= offset + viewport + Epsilon)
         {
            // Whole: past a header the left edge cuts, while this one stays whole.
            double? next = CutAtLeft(headers, offset) ? NextEdge_(headers, offset) : null;
            return Clamp(next is { } edge && edge <= left + Epsilon ? edge : offset, scrollable);
         }

         foreach ((double edge, double _) in headers)
         {
            if (edge > offset + Epsilon && right - edge <= viewport + Epsilon)
               return Clamp(edge, scrollable);
         }

         // Wider than the strip: its own left edge.
         return Clamp(left, scrollable);
      }

      /// <summary>Whether the strip's left edge cuts a header at this offset.</summary>
      public static bool CutAtLeft(IReadOnlyList<(double Left, double Width)> headers, double offset)
      {
         if (headers == null || offset <= Epsilon)
            return false;

         foreach ((double left, double width) in headers)
         {
            if (left < offset - Epsilon && left + width > offset + Epsilon)
               return true;
         }

         return false;
      }

      /// <summary>Whether the strip's right edge cuts a header at this offset.</summary>
      public static bool CutAtRight(IReadOnlyList<(double Left, double Width)> headers, double offset, double viewport)
      {
         if (headers == null)
            return false;

         double right = offset + viewport;
         foreach ((double left, double width) in headers)
         {
            if (left < right - Epsilon && left + width > right + Epsilon)
               return true;
         }

         return false;
      }

      /// <summary>Whether a header is out of sight or cut at the strip's right: what the right chevron is for.</summary>
      public static bool MoreAfter(IReadOnlyList<(double Left, double Width)> headers, double offset, double viewport)
      {
         if (headers == null || headers.Count == 0)
            return false;

         (double left, double width) = headers[headers.Count - 1];
         return left + width > offset + viewport + Epsilon;
      }

      /// <summary>Whether a header is out of sight or cut at the strip's left: what the left chevron is for.</summary>
      public static bool MoreBefore(IReadOnlyList<(double Left, double Width)> headers, double offset)
         => headers != null && headers.Count > 0 && headers[0].Left < offset - Epsilon;

      private static double? NextEdge_(IReadOnlyList<(double Left, double Width)> headers, double offset)
      {
         foreach ((double left, double _) in headers)
         {
            if (left > offset + Epsilon)
               return left;
         }

         return null;
      }

      private static double? PreviousEdge_(IReadOnlyList<(double Left, double Width)> headers, double offset)
      {
         double? found = null;
         foreach ((double left, double _) in headers)
         {
            if (left < offset - Epsilon)
               found = left;
         }

         return found;
      }

      private static double Clamp(double offset, double scrollable)
         => Math.Max(0, Math.Min(offset, Math.Max(0, scrollable)));
   }
}
