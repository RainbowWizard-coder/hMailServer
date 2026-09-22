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
   /// tree to go stale.
   /// </summary>
   public static class HelpPages
   {
      private const string Prefix = "Help.";
      private const string Suffix = ".md";

      private static readonly Lazy<IReadOnlyList<string>> names_ = new Lazy<IReadOnlyList<string>>(() =>
         Assembly.GetExecutingAssembly().GetManifestResourceNames()
            .Where(n => n.StartsWith(Prefix, StringComparison.Ordinal) && n.EndsWith(Suffix, StringComparison.Ordinal))
            .Select(n => n.Substring(Prefix.Length, n.Length - Prefix.Length - Suffix.Length))
            .OrderBy(n => n, StringComparer.OrdinalIgnoreCase)
            .ToList());

      /// <summary>Every document's name without the extension, in file-name order.</summary>
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

         using Stream stream = Assembly.GetExecutingAssembly().GetManifestResourceStream(Prefix + spelled + Suffix);
         if (stream == null)
            return null;

         using var reader = new StreamReader(stream, Encoding.UTF8, true);
         return reader.ReadToEnd().Replace("\r\n", "\n");
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
