// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Windows.Threading;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// A search that asks the server, run once typing pauses rather than on Enter
   /// alone or on every keystroke (review finding F210): the suppression list's
   /// and the audit trail's boxes are queries to the server - a list too long to
   /// read whole - so they cannot filter the rows already here as the other list
   /// pages do, and they waited for Enter, which nothing on the page said. Each
   /// change of the text restarts the wait; Enter runs the search at once.
   /// </summary>
   public sealed class SearchDelay
   {
      /// <summary>How long typing has to pause before the search runs.</summary>
      public const int DelayMilliseconds = 300;

      private readonly DispatcherTimer timer_;
      private readonly Action search_;

      public SearchDelay(Action search)
      {
         search_ = search;
         timer_ = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(DelayMilliseconds) };
         timer_.Tick += (_, _) => Now();
      }

      /// <summary>The text changed: the search runs once it has stayed the same for the delay.</summary>
      public void Restart()
      {
         timer_.Stop();
         timer_.Start();
      }

      /// <summary>Runs the search now - Enter, or Refresh - and drops the one that was waiting.</summary>
      public void Now()
      {
         timer_.Stop();
         search_?.Invoke();
      }

      /// <summary>Drops the search that was waiting, as the page is left.</summary>
      public void Cancel() => timer_.Stop();
   }
}
