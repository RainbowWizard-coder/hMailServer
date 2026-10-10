// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// A dialog with a Save has one Primary button, its Save (review finding F195). The
   /// editors the domain, account and route dialogs embed on their tabs drew Primary
   /// buttons of their own beside it: the list editor's Add on the Names and Allow and
   /// block list tabs and the External tab, the Rules tab's Add rule, Add criterion and
   /// Add action, App passwords' Create, Two-factor's Enrol a second factor, the Levels
   /// tab's Save levels now, the DKIM tab's Switch now and the route's Add address - so
   /// that a tab could show two blue buttons, each looking like the way out of the dialog.
   /// </summary>
   public class EmbeddedButtonWeightTests
   {
      [Theory]
      [InlineData("AccountDialog.cs")]
      [InlineData("DomainDialog.cs")]
      [InlineData("RouteDialog.cs")]
      [InlineData("IPRangeDialog.cs")]
      [InlineData("DistributionListDialog.cs")]
      [InlineData("TcpIpPortDialog.cs")]
      public void ADialogWithASave_HasThatSaveAsItsOnlyPrimaryButton(string file)
      {
         string[] primaries = ReadView(file).Split('\n')
            .Where(line => line.Contains("ControlAppearance.Primary", StringComparison.Ordinal)).ToArray();

         Assert.True(primaries.Length == 1 && primaries[0].Contains("L(\"_Save\")", StringComparison.Ordinal),
            file + " draws a Primary button besides its Save:\n" + string.Join("\n", primaries.Select(l => l.Trim())));
      }

      /// <summary>
      /// The panels the account and the domain dialogs put on their tabs draw no Primary
      /// button: the account's App passwords, Two-factor and Recovery, and the Levels tab
      /// both dialogs build from SettingLevelsPanel.
      /// </summary>
      [Theory]
      [InlineData("AppPasswordsPanel.cs")]
      [InlineData("AccountTwoFactorPanel.cs")]
      [InlineData("AccountRecoveryPanel.cs")]
      [InlineData("SettingLevelsPanel.cs")]
      public void APanelOnTheAccountOrDomainDialogsTabs_HasNoPrimaryButton(string file)
         => Assert.DoesNotContain("ControlAppearance.Primary", ReadView(file), StringComparison.Ordinal);

      [Fact]
      public void TheListEditorsAdd_IsPrimaryOnAPageAndSecondaryOnADialogsTab()
      {
         string source = ReadView("CollectionEditorView.cs");
         Assert.Contains("addButton_ = MakeButton(L(\"_Add\"), onDialogTab_ ? ControlAppearance.Secondary : ControlAppearance.Primary,", source, StringComparison.Ordinal);
      }

      /// <summary>
      /// Add's weight is the dialog's tab's, not the editor's embedded form's: the
      /// Groups page hosts the editor embedded, without its chrome, and drew its Add
      /// as Secondary when the weight followed that, leaving the page no Primary
      /// button. The four factories built for a dialog's tab say so, every editor a
      /// dialog builds is one of them, no page's is, and the Groups page's does not.
      /// </summary>
      [Fact]
      public void OnlyTheEditorsBuiltForADialogsTab_DrawTheirAddAsSecondary()
      {
         string specs = ReadView("CollectionSpecs.cs");
         var forDialogTab = new HashSet<string>(StringComparer.Ordinal);
         MatchCollection factories = Regex.Matches(specs, @"public static \w+ (\w+)\([^)]*\)\s*=>");
         for (int i = 0; i < factories.Count; i++)
         {
            int end = i + 1 < factories.Count ? factories[i + 1].Index : specs.Length;
            if (specs.Substring(factories[i].Index, end - factories[i].Index).Contains(SavedAsMadeNoteTests.DialogTabEnd, StringComparison.Ordinal))
               forDialogTab.Add(factories[i].Groups[1].Value);
         }

         Assert.Equal(new[] { "AccountRules", "DomainAliases", "DomainSenderList", "FetchAccounts" }, forDialogTab.OrderBy(n => n, StringComparer.Ordinal));

         var problems = new List<string>();
         int tabs = 0;
         foreach (string dialog in Directory.GetFiles(ViewsDirectory(), "*Dialog.cs"))
         {
            foreach (Match call in Regex.Matches(File.ReadAllText(dialog), @"CollectionSpecs\.(\w+)\("))
            {
               tabs++;
               if (!forDialogTab.Contains(call.Groups[1].Value))
                  problems.Add(Path.GetFileName(dialog) + " puts CollectionSpecs." + call.Groups[1].Value + " on a tab, and it is not built for a dialog's tab.");
            }
         }

         Assert.True(tabs >= 3, "The domain and account dialogs no longer build their list tabs from CollectionSpecs as this test reads them.");

         string window = SaveStateModelTests.ReadControlPanelFile("MainWindow.xaml.cs");
         MatchCollection pages = Regex.Matches(window, @"pageFactories_\[""\w+""\]\s*=\s*\(\)\s*=>\s*CollectionSpecs\.(\w+)\(");
         Assert.True(pages.Count > 5, "MainWindow no longer makes its list pages from CollectionSpecs as this test reads it.");
         foreach (Match page in pages)
         {
            if (forDialogTab.Contains(page.Groups[1].Value))
               problems.Add("The page CollectionSpecs." + page.Groups[1].Value + " makes is built as a dialog's tab, so its Add is not Primary.");
         }

         Assert.True(problems.Count == 0, string.Join("\n", problems));

         // The Groups page: the editor in its page mode, as every other list page
         // (review finding F112), so that Add is the page's Primary.
         Assert.Contains("editor_ = new CollectionEditorView(spec);", specs, StringComparison.Ordinal);
         Assert.Single(Regex.Matches(specs, @"new CollectionEditorView\("));
      }

      [Fact]
      public void TheRulesTab_DrawsItsAddsAsSecondaryButtons_AndDropsThePagePadding()
      {
         string source = ReadView("RulesView.xaml.cs");
         int start = source.IndexOf("public void ConfigureForRules(", StringComparison.Ordinal);
         Assert.True(start >= 0, "RulesView.xaml.cs no longer has ConfigureForRules.");
         int end = source.IndexOf("private dynamic OpenRules()", start, StringComparison.Ordinal);
         string configure = source.Substring(start, end - start);

         foreach (string button in new[] { "AddRuleButton", "AddCriterionButton", "AddActionButton" })
            Assert.Contains(button + ".Appearance = Wpf.Ui.Controls.ControlAppearance.Secondary;", configure, StringComparison.Ordinal);
         Assert.Matches(new Regex(@"page\.Margin = new Thickness\(0, DesignTokens\.Space\.Md, 0, 0\);"), configure);

         // On the page itself the three stay as the XAML draws them.
         string xaml = ReadView("RulesView.xaml");
         foreach (string button in new[] { "AddRuleButton", "AddCriterionButton", "AddActionButton" })
            Assert.Matches(new Regex("x:Name=\"" + button + "\"[^>]*Appearance=\"Primary\""), xaml);
      }

      private static string ReadView(string file)
         => File.ReadAllText(Path.Join(ViewsDirectory(), file)).Replace("\r\n", "\n");

      private static string ViewsDirectory()
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", "Views");
            if (File.Exists(Path.Join(candidate, "CollectionEditorView.cs")))
               return candidate;
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/Views could not be found from the test binaries.");
         return null;
      }
   }
}
