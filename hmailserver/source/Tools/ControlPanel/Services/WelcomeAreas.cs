// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>One tile of the Welcome page's "Or browse by area": a group of the sidebar.</summary>
   public sealed class WelcomeArea
   {
      public WelcomeArea(string icon, string title, string purpose, string page, string pageTitle)
      {
         Icon = icon;
         Title = title;
         Purpose = purpose;
         Page = page;
         PageTitle = pageTitle;
      }

      /// <summary>The group's glyph, as the name of a Wpf.Ui SymbolRegular member, or null.</summary>
      public string Icon { get; }

      /// <summary>The group's title, marked with N(): translated where it is shown.</summary>
      public string Title { get; }

      /// <summary>The group's purpose, marked with N().</summary>
      public string Purpose { get; }

      /// <summary>The key of the group's first page this credential may open: where the tile goes.</summary>
      public string Page { get; }

      /// <summary>That page's title, marked with N().</summary>
      public string PageTitle { get; }
   }

   /// <summary>
   /// The Welcome page's areas, from the navigation map (review finding F276). The
   /// tiles were six written into the page: "Domains &amp; accounts" beside the group
   /// "Accounts &amp; domains", opening a page the tree called Domains; "Server
   /// settings", promising protocols, delivery, anti-spam, anti-virus and advanced
   /// options and opening Protocols alone; and Dashboard, the next row of the
   /// sidebar, and Transport security, an expert page. Now there is one tile per group
   /// of the tree the signed-in credential sees, in the sidebar's order, with the
   /// group's glyph, title and purpose, opening the group's first page - so the
   /// tiles follow a renamed or reordered group, and a role that the tree leaves a
   /// group out for has no tile for it either.
   /// </summary>
   public static class WelcomeAreas
   {
      /// <summary>
      /// The areas the credential <paramref name="mayUse"/> describes may open,
      /// every group for a null one (a credential the server did not describe), as
      /// <see cref="NavigationMap.VisibleRoots"/> draws the tree. The top-level
      /// pages - Welcome itself, the Dashboard, About - are rows of the sidebar
      /// above and below the groups, not areas.
      /// </summary>
      public static IReadOnlyList<WelcomeArea> For(Func<string, bool> mayUse)
      {
         var areas = new List<WelcomeArea>();
         foreach (NavNode group in NavigationMap.VisibleRoots(mayUse).Where(node => !node.IsPage))
         {
            NavNode first = group.Children.FirstOrDefault(child => child.IsPage);
            if (first != null)
               areas.Add(new WelcomeArea(group.Icon, group.Title, group.Purpose, first.Key, first.Title));
         }

         return areas;
      }
   }
}
