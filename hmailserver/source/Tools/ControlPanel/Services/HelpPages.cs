// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Text;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The operator documentation this program carries: hmailserver/docs/*.md,
   /// embedded straight from the repository by the project file as
   /// Help.&lt;Name&gt;.md (README.md and DeckParity.md left out, as the server's
   /// copy leaves them out), so the Control Panel - which talks COM only and
   /// must work with no REST listener - has the same text the server serves
   /// at /api/v1/help, and cannot be behind it: nothing is copied into this
   /// tree to go stale. The resources are read from the assembly this type
   /// was compiled into: the program's own, and ControlPanel.Core's, which
   /// carries the same resource group so that HelpMapTests can hold the
   /// copy to the directory - the names, the two exclusions, the bytes.
   /// </summary>
   public static class HelpPages
   {
      private const string Prefix = "Help.";
      private const string Suffix = ".md";

      private static readonly Lazy<IReadOnlyList<string>> names_ = new Lazy<IReadOnlyList<string>>(() =>
         typeof(HelpPages).Assembly.GetManifestResourceNames()
            .Where(n => n.StartsWith(Prefix, StringComparison.Ordinal) && n.EndsWith(Suffix, StringComparison.Ordinal))
            .Select(n => n.Substring(Prefix.Length, n.Length - Prefix.Length - Suffix.Length))
            .OrderBy(n => n, StringComparer.Ordinal)
            .ToList());

      /// <summary>Every document's name without the extension, in ordinal file-name order, as the server lists them.</summary>
      public static IReadOnlyList<string> Names => names_.Value;

      /// <summary>Whether a document of this name is carried; without regard to case.</summary>
      public static bool Has(string name)
         => Find(name) != null;

      /// <summary>The document's name as it is spelled, or null.</summary>
      public static string Find(string name)
         => string.IsNullOrEmpty(name) ? null : Names.FirstOrDefault(n => string.Equals(n, name, StringComparison.OrdinalIgnoreCase));

      /// <summary>The document's text with LF line endings, or null when there is no such document.</summary>
      public static string Read(string name)
      {
         string spelled = Find(name);
         if (spelled == null)
            return null;

         using Stream stream = typeof(HelpPages).Assembly.GetManifestResourceStream(Prefix + spelled + Suffix);
         if (stream == null)
            return null;

         using var reader = new StreamReader(stream, Encoding.UTF8, true);
         return reader.ReadToEnd().Replace("\r\n", "\n");
      }

      /// <summary>
      /// The wiki base URL as the pane may use it: the value when it is an
      /// absolute http or https URL, trimmed, and empty otherwise. The same rule
      /// the server's two setters apply; applied again here because the value
      /// reaches ShellExecute and the server is not the only way a row is
      /// written. Empty hides the link.
      /// </summary>
      public static string WikiBaseUrlOrEmpty(string value)
      {
         string trimmed = (value ?? "").Trim();
         if (trimmed.StartsWith("http://", StringComparison.OrdinalIgnoreCase) ||
             trimmed.StartsWith("https://", StringComparison.OrdinalIgnoreCase))
            return trimmed;
         return "";
      }

      /// <summary>
      /// The page of the project's wiki made from the document - Backup-and-Restore
      /// for Backup, since the wiki names its pages its own way - or null when no
      /// page was (<see cref="HelpWikiPages"/>, generated from
      /// build/help-wiki-pages.txt, the table the server answers the Deck and the
      /// webmail with); without regard to case, as <see cref="Find"/> is.
      /// </summary>
      public static string WikiPageOf(string name)
         => !string.IsNullOrEmpty(name) && HelpWikiPages.ByDocument.TryGetValue(name, out string page) ? page : null;

      /// <summary>
      /// What "Open on the wiki" opens for the document: the wiki base URL with the
      /// project wiki's page for the document added, whatever the base - an
      /// administrator's own wiki gets the same names - or null - no link - when the
      /// base is empty or not a web address (<see cref="WikiBaseUrlOrEmpty"/>) or the
      /// project's wiki has no page made from the document. The page's name, never
      /// the document's: the wiki has no page called Backup, and up to October 2026,
      /// while the link was made of the document's name, it opened GitLab's "page
      /// does not exist" for every document but two.
      /// </summary>
      public static string WikiUrl(string baseUrl, string name)
      {
         string usable = WikiBaseUrlOrEmpty(baseUrl);
         string page = WikiPageOf(name);
         return usable.Length == 0 || page == null ? null : usable + page;
      }

      /// <summary>The document's title: its first level-one heading, or its name.</summary>
      public static string TitleOf(string name)
      {
         string text = Read(name);
         if (text == null)
            return name;

         foreach (MdHeading heading in MarkdownBlocks.Headings(text))
         {
            if (heading.Level == 1)
               return heading.Text;
         }
         return Find(name) ?? name;
      }
   }
}
