// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
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
   /// Add action, App passwords' Create, Two-factor's Enrol a second factor, the DKIM
   /// tab's Switch now and the route's Add address - so that a tab could show two blue
   /// buttons, each looking like the way out of the dialog.
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

      [Theory]
      [InlineData("AppPasswordsPanel.cs")]
      [InlineData("AccountTwoFactorPanel.cs")]
      [InlineData("AccountRecoveryPanel.cs")]
      public void APanelOnTheAccountDialogsTabs_HasNoPrimaryButton(string file)
         => Assert.DoesNotContain("ControlAppearance.Primary", ReadView(file), StringComparison.Ordinal);

      [Fact]
      public void TheListEditorsAdd_IsPrimaryOnAPageAndSecondaryOnADialogsTab()
      {
         string source = ReadView("CollectionEditorView.cs");
         Assert.Contains("addButton_ = MakeButton(L(\"_Add\"), embedded_ ? ControlAppearance.Secondary : ControlAppearance.Primary,", source, StringComparison.Ordinal);
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
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", "Views", file);
            if (File.Exists(candidate))
               return File.ReadAllText(candidate).Replace("\r\n", "\n");
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/Views/" + file + " could not be found from the test binaries.");
         return null;
      }
   }
}
