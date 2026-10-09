// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Review finding F226: the Rules page's scope list sat uncaptioned among the
   /// actions, reading "The server" between the search box and Move up - only a
   /// screen reader heard "Rules of" - and after a domain was chosen the title stayed
   /// "Global rules" and the subtitle "4 rule(s), evaluated top to bottom." The page
   /// is titled Rules, as the navigation map names it, the list is captioned "Show
   /// rules for" in the toolbar's filters slot, and the subtitle names whose rules it
   /// counts. Scanned rather than rendered: this assembly has no WPF.
   /// </summary>
   public class RulesScopeTests
   {
      [Fact]
      public void TheScopeList_IsCaptionedInTheFilters_AndThePageIsTitledRules()
      {
         string xaml = ReadControlPanelFile(Path.Join("Views", "RulesView.xaml"));
         Assert.Contains("Title=\"{loc:L 'Rules'}\"", xaml, StringComparison.Ordinal);
         Assert.DoesNotContain("'Global rules'", xaml, StringComparison.Ordinal);

         // Captioned, and wrapping with the actions as one: in the toolbar's filters
         // slot it left the actions too little room to wrap into at 760 pixels.
         int panel = xaml.IndexOf("<StackPanel x:Name=\"ScopePanel\"", StringComparison.Ordinal);
         int scope = xaml.IndexOf("x:Name=\"ScopeBox\"", StringComparison.Ordinal);
         int up = xaml.IndexOf("x:Name=\"MoveUpButton\"", StringComparison.Ordinal);
         Assert.True(panel > 0 && scope > panel && up > scope, "The scope list is not a captioned group ahead of the actions.");
         Assert.Contains("Content=\"{loc:L 'Show rules _for'}\" Target=\"{Binding ElementName=ScopeBox}\"", xaml, StringComparison.Ordinal);
         Assert.Contains("AutomationProperties.LabeledBy=\"{Binding ElementName=ScopeLabel}\"", xaml, StringComparison.Ordinal);
      }

      [Fact]
      public void TheSubtitle_NamesTheScopeItCounts()
      {
         string code = ReadControlPanelFile(Path.Join("Views", "RulesView.xaml.cs"));
         Assert.DoesNotContain("\"{0} rule(s)", code, StringComparison.Ordinal);

         string subtitle = WithheldRowTests.MemberText(code, "private string Subtitle_(int count)");
         Assert.True(subtitle != null, "RulesView.Subtitle_ is gone.");
         Assert.Contains("{0} rules of the server, evaluated top to bottom.", subtitle, StringComparison.Ordinal);
         Assert.Contains("{0} rules of {1}, evaluated top to bottom.", subtitle, StringComparison.Ordinal);
         Assert.Contains("scopeDomain_", subtitle, StringComparison.Ordinal);

         string changed = WithheldRowTests.MemberText(code, "private void Scope_Changed(object sender, SelectionChangedEventArgs e)");
         Assert.True(changed != null, "RulesView.Scope_Changed is gone.");
         Assert.Contains("scopeDomain_ = null;", changed, StringComparison.Ordinal);
         Assert.Contains("scopeDomain_ = domainName;", changed, StringComparison.Ordinal);
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
