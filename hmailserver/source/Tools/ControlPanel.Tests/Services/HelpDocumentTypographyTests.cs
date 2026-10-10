// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Text.RegularExpressions;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Every document the help pane shows is set ragged right, unhyphenated, in the
   /// pane's face and size (review finding F026): the documents it renders, the
   /// list of every document (All documentation, and a page with no guide) and the
   /// line that says a document is not in this copy. A FlowDocument justifies by
   /// default, and the list was justified - "Account  recovery:  a  recovery
   /// address  and  one-time" across the pane's 340 pixels - while the documents
   /// beside it were ragged right; the missing document's line was in the viewer's
   /// own face and size. All of them now start from MarkdownFlow.NewDocument, the
   /// one place a pane's FlowDocument is made.
   /// </summary>
   public class HelpDocumentTypographyTests
   {
      [Fact]
      public void TheHelpPaneMakesNoDocumentOfItsOwn()
      {
         string pane = Read("Views", "HelpPane.cs");

         Assert.DoesNotMatch(new Regex(@"new\s+FlowDocument\b"), pane);
         Assert.Equal(2, Regex.Matches(pane, @"MarkdownFlow\.NewDocument\(\)").Count);
         string list = WithheldRowTests.MemberText(pane, "private FlowDocument DocumentList_(IEnumerable<Block> before)");
         Assert.True(list != null, "HelpPane no longer builds its list of documents in DocumentList_.");
         Assert.Contains("MarkdownFlow.NewDocument()", list, StringComparison.Ordinal);
         string document = WithheldRowTests.MemberText(pane, "public void ShowDocument(string name, string heading)");
         Assert.True(document != null, "HelpPane no longer opens a document in ShowDocument.");
         Assert.Contains("MarkdownFlow.NewDocument()", document, StringComparison.Ordinal);
      }

      [Fact]
      public void ADocumentIsRaggedRightAndUnhyphenatedInThePanesFace()
      {
         string flow = Read("Services", "MarkdownFlow.cs");

         Assert.Single(Regex.Matches(flow, @"new\s+FlowDocument\b"));
         string made = WithheldRowTests.MemberText(flow, "public static FlowDocument NewDocument()");
         Assert.True(made != null, "MarkdownFlow no longer makes its documents in NewDocument.");
         Assert.Contains("new FlowDocument", made, StringComparison.Ordinal);
         Assert.Contains("TextAlignment = TextAlignment.Left", made, StringComparison.Ordinal);
         Assert.Contains("IsHyphenationEnabled = false", made, StringComparison.Ordinal);
         Assert.Contains("FontFamily = new FontFamily(Typography.UiFontFamily)", made, StringComparison.Ordinal);
         Assert.Contains("FontSize = Typography.Body", made, StringComparison.Ordinal);
         Assert.Contains("\"TextFillColorPrimaryBrush\"", made, StringComparison.Ordinal);

         string build = WithheldRowTests.MemberText(flow, "public static Built Build(IReadOnlyList<MdBlock> blocks, Action<string, string> openDocument)");
         Assert.True(build != null, "MarkdownFlow no longer builds a document in Build.");
         Assert.Contains("NewDocument()", build, StringComparison.Ordinal);
      }

      private static string Read(params string[] parts)
      {
         string controlPanel = ContrastTokensTests.FindControlPanelDirectory();
         Assert.True(controlPanel != null, "The Control Panel's sources were not found above the test binaries.");
         return File.ReadAllText(Path.Join(controlPanel, Path.Join(parts)));
      }
   }
}
