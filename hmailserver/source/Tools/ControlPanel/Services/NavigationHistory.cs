// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>One page of the history: its navigation key, and how it was left.</summary>
   public sealed class HistoryEntry
   {
      public HistoryEntry(string key)
      {
         Key = key;
      }

      /// <summary>The page's navigation key (<see cref="NavigationMap"/>).</summary>
      public string Key { get; }

      /// <summary>How far down the page was scrolled when it was left, in pixels; 0 until it is left.</summary>
      public double ScrollOffset { get; set; }

      /// <summary>The tab its first tab strip was on when it was left, or -1 for a page with none.</summary>
      public int TabIndex { get; set; } = -1;
   }

   /// <summary>
   /// The pages the administrator has been on, in order, for Back and Forward: a
   /// browser's history for one sign-in. Every way to a page - the sidebar, a
   /// Related link, the palette, a Welcome tile, a page's own button, a tour's
   /// step - is a visit, which puts the page after the one on screen and drops
   /// what was ahead of it; Back and Forward move through the list without
   /// changing it. Each entry keeps where its page was scrolled and which tab it
   /// was on when it was left, so that Back returns to the place and not only to
   /// the page.
   ///
   /// The Control Panel replaced the page and recorded nothing until October
   /// 2026: after a jump from the Dashboard's attention list, the setup
   /// checklist, an overview's row, the stalled-mail steps, a Related link or
   /// the palette, the only way back was to find the page again in the tree or
   /// the palette (review finding F013). WPF-free, so that the rules are held by
   /// NavigationHistoryTests; MainWindow.History.cs is the window's half.
   /// </summary>
   public sealed class NavigationHistory
   {
      /// <summary>
      /// How many pages are kept. A browser keeps fifty; a day at the Control
      /// Panel visits fewer pages than a day on the web, and the oldest goes
      /// first, so fifty is more than Back is ever pressed for.
      /// </summary>
      public const int Capacity = 50;

      private readonly List<HistoryEntry> entries_ = new List<HistoryEntry>();
      private int at_ = -1;

      /// <summary>The page on screen, or null before the first visit.</summary>
      public HistoryEntry Current => at_ >= 0 ? entries_[at_] : null;

      /// <summary>Whether there is a page before the one on screen.</summary>
      public bool CanGoBack => at_ > 0;

      /// <summary>Whether there is a page after the one on screen: one Back left.</summary>
      public bool CanGoForward => at_ >= 0 && at_ < entries_.Count - 1;

      /// <summary>The page Back opens, or null.</summary>
      public HistoryEntry PeekBack => CanGoBack ? entries_[at_ - 1] : null;

      /// <summary>The page Forward opens, or null.</summary>
      public HistoryEntry PeekForward => CanGoForward ? entries_[at_ + 1] : null;

      /// <summary>How many pages are kept.</summary>
      public int Count => entries_.Count;

      /// <summary>
      /// A page opened any way but Back and Forward: it goes after the page on
      /// screen, and the pages that were ahead of that one are dropped, as a
      /// browser drops them. The page on screen opened again is no visit, and
      /// neither is an empty key.
      /// </summary>
      public void Visit(string key)
      {
         if (string.IsNullOrEmpty(key))
            return;

         if (Current != null && string.Equals(Current.Key, key, StringComparison.OrdinalIgnoreCase))
            return;

         entries_.RemoveRange(at_ + 1, entries_.Count - at_ - 1);
         entries_.Add(new HistoryEntry(key));
         if (entries_.Count > Capacity)
            entries_.RemoveAt(0);
         at_ = entries_.Count - 1;
      }

      /// <summary>
      /// Keeps how the page on screen is being left - its scroll offset and its
      /// tab - so that Back or Forward to it returns there. Before the step, while
      /// the page is still on screen.
      /// </summary>
      public void Remember(double scrollOffset, int tabIndex)
      {
         HistoryEntry current = Current;
         if (current == null)
            return;

         current.ScrollOffset = double.IsNaN(scrollOffset) || scrollOffset < 0 ? 0 : scrollOffset;
         current.TabIndex = tabIndex < 0 ? -1 : tabIndex;
      }

      /// <summary>Moves to the page before and returns it, or null when there is none (and nothing moves).</summary>
      public HistoryEntry Back()
      {
         if (!CanGoBack)
            return null;

         at_--;
         return entries_[at_];
      }

      /// <summary>Moves to the page after and returns it, or null when there is none (and nothing moves).</summary>
      public HistoryEntry Forward()
      {
         if (!CanGoForward)
            return null;

         at_++;
         return entries_[at_];
      }

      /// <summary>Forgets every page: at sign-in, since the next credential's pages are its own.</summary>
      public void Clear()
      {
         entries_.Clear();
         at_ = -1;
      }
   }
}
