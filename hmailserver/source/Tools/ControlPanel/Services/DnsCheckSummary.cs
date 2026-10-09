// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Collections.Generic;
using System.Linq;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// What the DNS records page says after it has checked every record of a domain
   /// (review finding F277): one notice at the worst level any check reached, a line
   /// per verdict, worst first, naming the records that reached it -
   /// "Not published: DKIM, TLS-RPT", "Published and correct: MX, SPF, DMARC". The
   /// page had a Check button on each of seven cards and nothing that said how the
   /// domain stood as a whole, so verifying a domain was seven clicks and a scroll
   /// past every card to read the seven lines.
   ///
   /// No WPF here, so the tests hold the grouping and the order.
   /// </summary>
   public static class DnsCheckSummary
   {
      /// <summary>One check's outcome: the record, the level its verdict is at, and the verdict's own word.</summary>
      public sealed record Result(string Record, StatusLevel Level, string Word);

      /// <summary>The worst level any check reached; Normal when there was none.</summary>
      public static StatusLevel Worst(IEnumerable<Result> results)
         => (results ?? Enumerable.Empty<Result>()).Select(r => r.Level).DefaultIfEmpty(StatusLevel.Normal).Max();

      /// <summary>
      /// A line per verdict, "Word: record, record", the worst level first and, at one
      /// level, in the order the page shows the records.
      /// </summary>
      public static IReadOnlyList<string> Lines(IEnumerable<Result> results)
      {
         List<Result> all = (results ?? Enumerable.Empty<Result>()).ToList();
         return all
            .Select((result, index) => (result, index))
            .GroupBy(x => x.result.Word)
            .Select(g => (Word: g.Key, Level: g.Max(x => x.result.Level), First: g.Min(x => x.index), Records: g.Select(x => x.result.Record).ToList()))
            .OrderByDescending(g => g.Level)
            .ThenBy(g => g.First)
            .Select(g => F("{0}: {1}", g.Word, string.Join(", ", g.Records)))
            .ToList();
      }

      /// <summary>The notice's text: what was checked, then a line per verdict.</summary>
      public static string Text(string domain, IEnumerable<Result> results)
         => F("The records of {0}, checked from this machine:", domain) + "\n" + string.Join("\n", Lines(results));
   }
}
