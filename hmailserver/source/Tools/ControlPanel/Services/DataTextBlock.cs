// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Windows.Controls;
using System.Windows.Documents;
using System.Windows.Media;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>The WPF half of <see cref="DataText"/>: a sentence's runs, its data in the monospaced face.</summary>
   public static class DataTextBlock
   {
      /// <summary>
      /// Fills <paramref name="block"/> with a translated sentence whose {0} is
      /// data: the words around it in the block's own face, the data in the
      /// monospaced one. A sentence without {0} is shown as it is, the data after it.
      /// </summary>
      public static void Fill(TextBlock block, string template, string data, bool breakAfterColons = false)
      {
         if (block == null)
            return;

         template ??= "";
         string shown = breakAfterColons ? DataText.BreakAfterColons(data) : data ?? "";
         block.Inlines.Clear();
         int at = template.IndexOf("{0}", StringComparison.Ordinal);
         string before = at < 0 ? template + " " : template.Substring(0, at);
         string after = at < 0 ? "" : template.Substring(at + 3);
         if (before.Length > 0)
            block.Inlines.Add(new Run(before));
         block.Inlines.Add(new Run(shown) { FontFamily = new FontFamily(Typography.MonoFontFamily) });
         if (after.Length > 0)
            block.Inlines.Add(new Run(after));
      }
   }
}
