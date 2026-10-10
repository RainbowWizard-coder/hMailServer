// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>One heading of the help pane's list of every document, and the documents under it.</summary>
   public sealed class HelpDocumentSection
   {
      public HelpDocumentSection(string heading, IReadOnlyList<string> names)
      {
         Heading = heading;
         Names = names;
      }

      /// <summary>The heading in English, a catalogue key: a sidebar group's title, Reference, or Building and packaging.</summary>
      public string Heading { get; }

      /// <summary>The documents' names without the extension, by title.</summary>
      public IReadOnlyList<string> Names { get; }
   }

   /// <summary>
   /// The help pane's list of every document, in the sidebar's order: under each
   /// group's title the documents its pages and their tabs open (HelpMap), then
   /// Reference - every other document an administrator reads - and last Building
   /// and packaging, the documents about making and testing hMailServer itself.
   /// Each heading's documents are in title order. The list was every document by
   /// file name, one flat run of some 120 in which ContinuousIntegration, Fuzzing
   /// and Nix stood among the operators' guides (review finding F315).
   /// </summary>
   public static class HelpDocumentList
   {
      /// <summary>The heading of the documents no page opens that are not about building hMailServer.</summary>
      public static readonly string Reference = N("Reference");

      /// <summary>The heading of the documents about building, packaging and testing hMailServer.</summary>
      public static readonly string Building = N("Building and packaging");

      /// <summary>
      /// The documents about making, packaging and testing hMailServer rather than
      /// running it: its pipelines and packages, its build and test environments and
      /// the Control Panel's design. One that a page opens is listed under the page's
      /// group all the same.
      /// </summary>
      public static readonly IReadOnlyList<string> BuildingAndPackaging = new[]
      {
         "Alpine", "Channels", "ContinuousIntegration", "ControlPanelDesign", "Fuzzing", // no-loc: documents' names
         "Nix", "PackagesAppImage", "PackagesDeb", "PackagesRpm", "PackagesWindows", "RegressionEnvironment", // no-loc
         "ReproducibleBuilds", "SourcePackages", "SourceTarball", "TestLab", "ThirdPartyBinaries", "WindowsOnArm", // no-loc
      };

      /// <summary>
      /// The sections for the documents <paramref name="names"/> (without the
      /// extension), each document under one heading only - the first group whose
      /// pages open it - and only the headings with a document. With a
      /// <paramref name="term"/> of two characters or more, only the documents whose
      /// title or text holds it, without regard to case.
      /// </summary>
      public static IReadOnlyList<HelpDocumentSection> Sections(IEnumerable<string> names, Func<string, string> titleOf,
         Func<string, string> textOf = null, string term = null)
      {
         var all = (names ?? Array.Empty<string>()).ToList();
         var byName = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
         foreach (string name in all)
            byName.TryAdd(name, name);

         var placed = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
         var raw = new List<(string Heading, List<string> Names)>();
         foreach (NavNode group in NavigationMap.Groups)
         {
            var docs = new List<string>();
            foreach (NavNode page in PagesOf_(group))
            {
               foreach (string doc in DocumentsOf_(page.Key))
               {
                  string name = doc.EndsWith(".md", StringComparison.OrdinalIgnoreCase) ? doc.Substring(0, doc.Length - 3) : doc;
                  if (byName.TryGetValue(name, out string spelled) && placed.Add(spelled))
                     docs.Add(spelled);
               }
            }
            raw.Add((group.Title, docs));
         }

         var building = new HashSet<string>(BuildingAndPackaging, StringComparer.OrdinalIgnoreCase);
         raw.Add((Reference, all.Where(n => !placed.Contains(n) && !building.Contains(n)).ToList()));
         raw.Add((Building, all.Where(n => !placed.Contains(n) && building.Contains(n)).ToList()));

         string wanted = (term ?? "").Trim();
         bool Matches(string name)
            => wanted.Length < 2
               || (titleOf?.Invoke(name) ?? name).Contains(wanted, StringComparison.CurrentCultureIgnoreCase)
               || name.Contains(wanted, StringComparison.OrdinalIgnoreCase)
               || (textOf?.Invoke(name) ?? "").Contains(wanted, StringComparison.CurrentCultureIgnoreCase);

         return raw
            .Select(s => new HelpDocumentSection(s.Heading,
               s.Names.Where(Matches).OrderBy(n => titleOf?.Invoke(n) ?? n, StringComparer.CurrentCultureIgnoreCase).ToList()))
            .Where(s => s.Names.Count > 0)
            .ToList();
      }

      private static IEnumerable<NavNode> PagesOf_(NavNode node)
      {
         foreach (NavNode child in node.Children)
         {
            if (child.Children.Count == 0)
               yield return child;
            else
               foreach (NavNode page in PagesOf_(child))
                  yield return page;
         }
      }

      /// <summary>The documents a page opens: its own, then its tabs' in the map's order.</summary>
      private static IEnumerable<string> DocumentsOf_(string key)
      {
         HelpTarget own = HelpMap.For(key);
         if (own != null && own.IsGuide)
            yield return own.Doc;
         foreach (string tab in HelpMap.TabsOf(key))
         {
            HelpTarget onTab = HelpMap.For(key, tab);
            if (onTab != null && onTab.IsGuide)
               yield return onTab.Doc;
         }
      }
   }
}
