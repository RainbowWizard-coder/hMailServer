// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Data;
using hMailServer.ControlPanel.Views.Scaffold;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// A XAML list page's search, its count and its empty state, for the rows it
   /// read (docs/ControlPanelDesign.md, List pages; review findings F288 and
   /// F210): the routes, TCP/IP ports, IP ranges, SSL certificates, rules and
   /// delivery queue pages call <see cref="Show"/> after every read that worked
   /// and every keystroke in their search. It filters the list
   /// (<see cref="ListSearch"/>), and then the pill says "Shown: 2 of 40" while a
   /// search is typed in and the list's own count otherwise, and where the
   /// search hides every row the empty state says that no entry has what was
   /// typed, without its action - the way to add a first entry, which a list
   /// that has entries does not need - and gives the action back once the search
   /// is cleared (<see cref="ListSearchCount"/>). Those pages filtered their rows
   /// and left the pill at the whole list's count, beside a blank card where the
   /// search hid every row.
   /// </summary>
   public static class ListSearchState
   {
      // The empty state's own action, kept while a search that hides every row
      // takes it away. Held by the empty state, so that it goes with its page.
      private static readonly ConditionalWeakTable<EmptyState, HeldAction> Actions = new();

      private sealed class HeldAction
      {
         public object Action;
      }

      /// <summary>
      /// Filters <paramref name="list"/> by <paramref name="query"/> and says what
      /// it shows: the pill's text - <paramref name="count"/> of the whole list's
      /// number where no search is typed in - and the empty state, whose sentence
      /// for an empty list is <paramref name="emptyText"/>. Only where the last
      /// read worked, which the pill says by being shown: a read that failed hides
      /// it, and its notice says what happened, which neither a count nor an empty
      /// state may contradict (Services/ReadFailure).
      /// </summary>
      public static void Show(ItemsControl list, string query, StatusPill pill, Func<int, string> count,
         EmptyState empty, string emptyText)
      {
         if (list == null)
            return;

         ListSearch.Apply(list, query);

         if (pill == null || count == null || pill.Visibility != Visibility.Visible)
            return;

         int total = Total_(list.ItemsSource);
         int shown = Shown_(list.ItemsSource, total);
         pill.Text = ListSearchCount.PillText(query, shown, total, count(total));

         if (empty == null)
            return;

         HeldAction held = Actions.GetValue(empty, e => new HeldAction { Action = e.Action });
         if (ListSearchCount.NothingMatches(query, shown, total))
         {
            empty.Action = null;
            empty.Text = ListSearchCount.NothingMatchesText(query);
            empty.Visibility = Visibility.Visible;
            return;
         }

         empty.Action = held.Action;
         StatusText.Show(empty, null, total, null, emptyText);
      }

      // How many rows the page read.
      private static int Total_(IEnumerable source)
      {
         if (source == null)
            return 0;

         if (source is ICollection collection)
            return collection.Count;

         int n = 0;
         foreach (object _ in source)
            n++;
         return n;
      }

      // How many of them the search lets through: the rows of the list's view,
      // which ListSearch filtered.
      private static int Shown_(IEnumerable source, int total)
      {
         ICollectionView view = source == null ? null : CollectionViewSource.GetDefaultView(source);
         if (view == null)
            return total;

         int n = 0;
         foreach (object _ in view)
            n++;
         return n;
      }
   }
}
