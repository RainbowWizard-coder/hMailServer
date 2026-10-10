// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// One configuration check that found something - an IP range that relays for
   /// anybody, no host name, bounces refused, localhost banned - with its level,
   /// its sentence and the navigation key of the page that puts it right. The
   /// Server status page shows each as a notice; the Dashboard's attention card
   /// shows them above the setup steps (review finding F242), where they never
   /// reached it, less any the checklist holds as an open step. ConfigurationChecks
   /// reads them; this half is plain data, so that a test can hold the order and
   /// the card's arithmetic.
   /// </summary>
   public sealed class ConfigurationWarning
   {
      public StatusLevel Level { get; init; }

      public string Text { get; init; }

      /// <summary>The navigation key of the page that puts it right, or null.</summary>
      public string Page { get; init; }

      /// <summary>
      /// The subject this warning shares with an item of the setup checklist
      /// (SetupItem.Key) - <see cref="DashboardAttention.HostNameKey"/> for no host
      /// name - or null for a warning the checklist does not hold. While that item is
      /// an open step, the Dashboard's attention card lists the step alone and
      /// counts it once (DashboardAttention.WithoutOpenSteps); Server status, which
      /// shows no steps, keeps the warning.
      /// </summary>
      public string Key { get; init; }
   }

   /// <summary>
   /// What the Dashboard's attention card lists, and in what order (review finding
   /// F242): the configuration warnings first, the most severe first, then the next
   /// step of the setup checklist and the two after it, then one line that says how
   /// many more the checklist holds and opens it. The card listed every open item of
   /// the checklist - eleven to fourteen rows on a new server - which pushed the
   /// figures to the foot of the first screen and both charts below it, and the
   /// checklist is one click away.
   /// </summary>
   public static class DashboardAttention
   {
      /// <summary>How many of the setup checklist's open steps the card lists: the next one and two more.</summary>
      public const int StepsShown = 3;

      /// <summary>
      /// The subject of the empty host name, which ConfigurationChecks warns of and
      /// the setup checklist's host-name item (ExternalSetupChecks.HostNameItem)
      /// holds as a step: the key that ties the warning to the step.
      /// </summary>
      public const string HostNameKey = "hostname";

      /// <summary>How many open steps the card lists, of <paramref name="openSteps"/>.</summary>
      public static int StepsToShow(int openSteps) => Math.Max(0, Math.Min(openSteps, StepsShown));

      /// <summary>How many open steps the card leaves to the checklist's line.</summary>
      public static int StepsLeftToTheChecklist(int openSteps) => Math.Max(0, openSteps - StepsShown);

      /// <summary>
      /// The warnings, Critical first, then Warning, then the rest, each level in the
      /// order the checks found them, so that an open relay is the first line of the
      /// card whatever the server lists before it.
      /// </summary>
      public static IReadOnlyList<ConfigurationWarning> MostSevereFirst(IEnumerable<ConfigurationWarning> warnings)
         => (warnings ?? Enumerable.Empty<ConfigurationWarning>())
            .Where(w => w != null)
            .Select((w, i) => (w, i))
            .OrderByDescending(x => Rank_(x.w.Level))
            .ThenBy(x => x.i)
            .Select(x => x.w)
            .ToList();

      /// <summary>
      /// The warnings the card lists and counts beside the open steps: all but those
      /// whose subject (<see cref="ConfigurationWarning.Key"/>) is also the subject
      /// of an open step, since each of those says what a step of the checklist
      /// says. An empty host name was the warning "No host name is set..." and the
      /// step "The host name - the name this server gives other servers" on every
      /// new server, and was counted twice, so that the card said 15 things where
      /// the checklist said 14. The order is kept; a step that is done, or cannot be
      /// told, takes nothing away.
      /// </summary>
      public static IReadOnlyList<ConfigurationWarning> WithoutOpenSteps(
         IEnumerable<ConfigurationWarning> warnings, IEnumerable<string> openStepKeys)
      {
         var open = new HashSet<string>(
            (openStepKeys ?? Enumerable.Empty<string>()).Where(key => !string.IsNullOrEmpty(key)), StringComparer.Ordinal);
         return (warnings ?? Enumerable.Empty<ConfigurationWarning>())
            .Where(w => w != null && (string.IsNullOrEmpty(w.Key) || !open.Contains(w.Key)))
            .ToList();
      }

      /// <summary>
      /// The card's level: Critical when a warning is, otherwise the level the
      /// checklist's own state gives it - a warning of the configuration counts as
      /// something that needs action.
      /// </summary>
      public static StatusLevel CardLevel(StatusLevel checklistLevel, IEnumerable<ConfigurationWarning> warnings)
      {
         StatusLevel worst = checklistLevel;
         foreach (ConfigurationWarning warning in warnings ?? Enumerable.Empty<ConfigurationWarning>())
         {
            if (warning != null && Rank_(warning.Level) > Rank_(worst))
               worst = warning.Level;
         }
         return worst;
      }

      private static int Rank_(StatusLevel level) => level switch
      {
         StatusLevel.Critical => 4,
         StatusLevel.Warning => 3,
         StatusLevel.Information => 2,
         StatusLevel.Good => 1,
         _ => 0
      };
   }
}
