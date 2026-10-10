// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Xml.Linq;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// One button order in the Control Panel's rows of buttons - a page header's
   /// actions, a toolbar's and a card's footer (review finding F289): the secondary
   /// and destructive buttons first, then Refresh or Reload, then the Primary - or
   /// the act drawn in the filled Danger appearance - at the right edge. The Primary
   /// came first on the REST API keys page, the suppression list and the audit
   /// trail, in the middle of the delivery queue's and the event scripts' rows and
   /// last on the settings pages, and the backup page's cards changed their order
   /// from one to the next. A dialog's footer keeps the Windows order, its primary
   /// button before Cancel (DialogFrame), and is not one of these rows.
   /// </summary>
   public class ButtonOrderTests
   {
      private static readonly XNamespace Ui = "http://schemas.lepo.co/wpfui/2022/xaml";
      private static readonly XNamespace Scaffold = "clr-namespace:hMailServer.ControlPanel.Views.Scaffold";

      /// <summary>In XAML, a filled button is the last button of every panel in a header's actions, a toolbar's or a card's footer.</summary>
      [Fact]
      public void InXaml_TheFilledButtonIsLastInItsRow()
      {
         var slots = new[] { Scaffold + "PageHeader.Actions", Scaffold + "Toolbar.Actions", Scaffold + "Card.Footer" };
         var wrong = new List<string>();
         foreach (string file in Directory.EnumerateFiles(ControlPanelDirectory("Views"), "*.xaml", SearchOption.AllDirectories))
         {
            XElement root = XDocument.Parse(File.ReadAllText(file)).Root;
            foreach (XElement panel in root.Descendants().Where(e => slots.Contains(e.Name)).SelectMany(s => s.DescendantsAndSelf()))
            {
               List<XElement> buttons = panel.Elements(Ui + "Button").ToList();
               for (int i = 0; i < buttons.Count - 1; i++)
               {
                  if ((string)buttons[i].Attribute("Appearance") is "Primary" or "Danger")
                     wrong.Add(Path.GetFileName(file) + ": " + (string)buttons[i].Attribute("Content") + " before " + (string)buttons[^1].Attribute("Content"));
               }
            }
         }

         Assert.True(wrong.Count == 0, "A Primary before another button: " + string.Join("; ", wrong));
      }

      /// <summary>
      /// In code, a panel given to a header's or a toolbar's Actions or a card's
      /// Footer adds its Primary last: every button of such a panel made with the
      /// Primary appearance is the panel's last Children.Add.
      /// </summary>
      [Fact]
      public void InCode_ThePrimaryIsAddedLastToItsRow()
      {
         var primary = new Regex(@"(\w+) = (?:MakeButton\w*|Button_|new [\w.]*Button)\b[^;]*?ControlAppearance\.Primary", RegexOptions.Singleline);
         var slot = new Regex(@"\b(?:Actions|Footer) = (\w+)\b");
         var declaration = new Regex(@"var (\w+) = new (?:StackPanel|WrapPanel)\b");
         var wrong = new List<string>();

         foreach (string file in Directory.EnumerateFiles(ControlPanelDirectory("Views"), "*.cs", SearchOption.AllDirectories))
         {
            string text = File.ReadAllText(file);
            var primaries = new HashSet<string>(primary.Matches(text).Select(m => m.Groups[1].Value));
            var slots = new HashSet<string>(slot.Matches(text).Select(m => m.Groups[1].Value));
            foreach (Match panel in declaration.Matches(text))
            {
               string name = panel.Groups[1].Value;
               if (!slots.Contains(name))
                  continue;

               Match next = declaration.Match(text, panel.Index + panel.Length);
               string scope = text.Substring(panel.Index, (next.Success ? next.Index : text.Length) - panel.Index);
               List<string> added = Regex.Matches(scope, Regex.Escape(name) + @"\.Children\.Add\(([^;]*?)\);").Select(m => m.Groups[1].Value.Trim()).ToList();
               for (int i = 0; i < added.Count - 1; i++)
               {
                  if (primaries.Contains(added[i]) || added[i].Contains("ControlAppearance.Primary", StringComparison.Ordinal))
                     wrong.Add(Path.GetFileName(file) + ": " + name + " adds " + added[i] + " before " + added[^1]);
               }
            }
         }

         Assert.True(wrong.Count == 0, "A Primary before another button: " + string.Join("; ", wrong));
      }

      /// <summary>The pages the finding named, as they are now.</summary>
      [Fact]
      public void TheNamedPages_PutThePrimaryLast()
      {
         string keys = ReadControlPanelFile(Path.Join("Views", "ApiKeysView.cs"));
         Assert.True(keys.IndexOf("actions.Children.Add(reload);", StringComparison.Ordinal) < keys.IndexOf("actions.Children.Add(create);", StringComparison.Ordinal),
            "The REST API keys page's Create a key… is not after Refresh.");

         string scripts = ReadControlPanelFile(Path.Join("Views", "ScriptsView.cs"));
         Assert.True(scripts.IndexOf("actions.Children.Add(reload);", StringComparison.Ordinal) < scripts.IndexOf("actions.Children.Add(save);", StringComparison.Ordinal),
            "The event scripts' Save & reload is not after Reload from disk.");

         string queue = ReadControlPanelFile(Path.Join("Views", "QueueView.xaml"));
         Assert.True(queue.IndexOf("x:Name=\"RemoveButton\"", StringComparison.Ordinal) < queue.IndexOf("x:Name=\"RetryButton\"", StringComparison.Ordinal),
            "The delivery queue's Deliver now is not after Remove.");

         string backup = ReadControlPanelFile(Path.Join("Views", "BackupView.xaml"));
         Assert.True(backup.IndexOf("x:Name=\"MailboxRestoreStatusButton\"", StringComparison.Ordinal) < backup.IndexOf("'Restore the mailbo_x'", StringComparison.Ordinal),
            "Backup & restore's Restore the mailbox is not after Re-read the outcome.");
      }

      private static string ControlPanelDirectory(string relative)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relative);
            if (Directory.Exists(candidate))
               return candidate;
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/" + relative + " could not be found from the test binaries.");
         return null;
      }

      private static string ReadControlPanelFile(string relative)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relative);
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/" + relative.Replace('\\', '/') + " could not be found from the test binaries.");
         return null;
      }
   }
}
