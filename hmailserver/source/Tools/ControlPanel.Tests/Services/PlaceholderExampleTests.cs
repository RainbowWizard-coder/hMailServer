// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Xml.Linq;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// A caption names its field and the box shows an example, marked as one, from
   /// example.com or example.net (review finding F303): captions said "Domain (e.g.
   /// partner.com)" and "Header name (e.g. X-Spam-Status)" where other pages put
   /// the example in the box; boxes said yourdomain.com where the rest said
   /// example.com; and an empty DNS override showed 1.1.1.1, which read as a
   /// Cloudflare resolver set on this server.
   /// </summary>
   public class PlaceholderExampleTests
   {
      [Fact]
      public void NoCaptionCarriesItsExample()
      {
         string path = ConfirmationPatternTests.ControlPanelFile(Path.Join("Resources", "Strings.resx"));
         var found = XDocument.Load(path).Root.Elements("data")
            .Select(data => (string)data.Attribute("name"))
            .Where(key => key.Contains("(e.g. ", StringComparison.Ordinal))
            .ToList();
         Assert.True(found.Count == 0, "Captions with their example in parentheses:" + Environment.NewLine + string.Join(Environment.NewLine, found));
      }

      [Fact]
      public void NoPlaceholderSaysYourDomain()
      {
         string views = Path.GetDirectoryName(ConfirmationPatternTests.ControlPanelFile(Path.Join("Views", "UtilityViews.cs")));
         string services = Path.GetDirectoryName(ConfirmationPatternTests.ControlPanelFile(Path.Join("Services", "Loc.cs")));
         var found = Directory.EnumerateFiles(views).Concat(Directory.EnumerateFiles(services))
            .Where(file => file.EndsWith(".cs", StringComparison.Ordinal) || file.EndsWith(".xaml", StringComparison.Ordinal))
            .Where(file => File.ReadAllText(file).Contains("yourdomain", StringComparison.OrdinalIgnoreCase))
            .Select(Path.GetFileName)
            .ToList();
         Assert.True(found.Count == 0, "yourdomain in: " + string.Join(", ", found));
      }

      [Fact]
      public void TheExamplesAreMarkedAsExamples()
      {
         string feature = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "FeatureSettingsView.xaml.cs"));
         // The settings rows hold the bare example and their boxes mark it
         // (ExamplePlaceholder.Shown; PlaceholderExamplesTests).
         Assert.Contains("Label = L(\"Override DNS server (empty = the name servers Windows uses)\"), Placeholder = \"1.1.1.1\"", feature, StringComparison.Ordinal);
         // The three OpenTelemetry endpoints, on Monitoring & alerts since review finding F273.
         string server = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "ServerSettingsView.xaml.cs"));
         Assert.Equal(3, Regex.Matches(server, Regex.Escape("Placeholder = \"http://localhost:4318\"")).Count);
         Assert.Contains("box_ = TextEditor(current, heldEmpty ? L(\"(empty)\") : ExamplePlaceholder.Shown(Placeholder));", server, StringComparison.Ordinal);
         Assert.Contains("PlaceholderText = ExamplePlaceholder.Shown(Placeholder)", feature, StringComparison.Ordinal);
         // A placeholder written as English in every language, "e.g. 127.0.0.1", is gone.
         Assert.DoesNotContain("Placeholder = \"e.g.", feature, StringComparison.Ordinal);

         Assert.Contains("NewDomain.PlaceholderText = F(\"e.g. {0}\", \"partner.example\")", ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "RoutesView.xaml.cs")), StringComparison.Ordinal);
         string ranges = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "IPRangesView.xaml.cs"));
         Assert.Contains("NewLower.PlaceholderText = F(\"e.g. {0}\", \"10.0.0.0\")", ranges, StringComparison.Ordinal);
         Assert.Contains("NewUpper.PlaceholderText = F(\"e.g. {0}\", \"10.0.0.255\")", ranges, StringComparison.Ordinal);
         string utility = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "UtilityViews.cs"));
         Assert.Contains("PlaceholderText = F(\"e.g. {0}\", \"postmaster@example.com\")", utility, StringComparison.Ordinal);
         // The sendout's name explanation is the row's hint, not the box's placeholder.
         Assert.Contains("Label = L(\"From _name\"), Content = fromName_, Hint = L(\"Sender name, as recipients see it\")", utility, StringComparison.Ordinal);
         Assert.Contains("F(\"Such as {0}\", \"X-Spam-Status\")", ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "RuleCriteriaDialog.cs")), StringComparison.Ordinal);
         Assert.Contains("F(\"Such as {0}\", \"INBOX.Archive\")", ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "RuleActionDialog.cs")), StringComparison.Ordinal);
      }
   }
}
