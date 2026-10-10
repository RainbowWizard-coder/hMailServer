// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The collection editor's grid headers were the dialog's captions, which are
   /// sentences: the greylisting bypass list's SPF, DKIM and DMARC columns were
   /// all headed "Requires…" at 80 pixels, the allow and block list's "Block
   /// rather than allow" read "Block rath…" and its expiry date was kept off the
   /// grid, the domain dialog's Names tab carried an example in its header, and
   /// the account dialog's External tab left the server, its one star column,
   /// about 40 pixels ("Ser…") beside five fixed ones. Every test here fails
   /// against those specifications: a narrow column whose caption is longer than
   /// a few words has a header of its own (FieldSpec.GridLabel), the editor keeps
   /// the caption as the header's tool tip and help text, a Bool's cells can be
   /// words of their own, and a value that applies only while another is true is
   /// shown as empty while it is not.
   /// </summary>
   public class CollectionGridHeaderTests
   {
      // A header this wide, or wider, has room for a sentence; narrower than it,
      // a caption of more than this many characters is cut.
      private const double NarrowColumn = 130;
      private const int ShortCaption = 14;

      private static readonly Regex FieldSpecs = new(@"new FieldSpec\s*\{(?<body>[^{}]*)\}", RegexOptions.Singleline);
      private static readonly Regex Setting = new(@"\b(?<name>Prop|Label|GridLabel|GridWidth|ShowInGrid)\s*=\s*(?:L\(""(?<text>(?:[^""\\]|\\.)*)""\)|""(?<text>(?:[^""\\]|\\.)*)""|(?<text>[\w.]+))");

      /// <summary>
      /// Every field the grid shows in a narrow column, whose caption is longer
      /// than a few words, has a header of its own. The rule, so that the next
      /// sentence-long caption over a narrow column is caught here rather than in
      /// a render.
      /// </summary>
      [Fact]
      public void EveryNarrowColumnWithASentenceForACaption_HasAHeaderOfItsOwn()
      {
         var offenders = new List<string>();
         foreach (Dictionary<string, string> field in Fields())
         {
            if (field.TryGetValue("ShowInGrid", out string shown) && shown == "false")
               continue;
            if (!field.TryGetValue("GridWidth", out string widthText) || !double.TryParse(widthText, NumberStyles.Float, CultureInfo.InvariantCulture, out double width) || width >= NarrowColumn)
               continue;
            if (!field.TryGetValue("Label", out string label) || label.Length <= ShortCaption)
               continue;
            if (!field.ContainsKey("GridLabel"))
               offenders.Add(field.GetValueOrDefault("Prop", "?") + ": '" + label + "' over " + width + " pixels");
         }

         Assert.True(offenders.Count == 0, "A column narrower than " + NarrowColumn + " pixels is headed by a caption longer than " + ShortCaption + " characters, which the column cuts: " + string.Join("; ", offenders));
      }

      [Fact]
      public void TheBypassList_IsHeadedByTheResultsNames_AndReadsRequiredOrADash()
      {
         string specs = Specs();
         foreach (string result in new[] { "SPF", "DKIM", "DMARC" })
         {
            Assert.Matches(new Regex(@"Prop = ""Require" + result + @""".*GridLabel = L\(""" + result + @"""\).*GridTrue = L\(""Required""\).*GridFalse = ""—"".*GridWidth = 72"), specs);
         }
      }

      [Fact]
      public void TheAllowAndBlockList_HasAnActionColumn_AndOneExpiresColumn_ShowingTheDateOrNever()
      {
         string fields = WithheldRowTests.MemberText(Specs(), "private static List<FieldSpec> SenderListFields(double valueWidth)");
         Assert.True(fields != null, "SenderListFields is gone, so the server's and a domain's lists can differ.");
         Assert.Contains("Prop = \"Blocked\", Label = L(\"Block rather than allow\"), GridLabel = L(\"Action\"), GridTrue = L(\"Block\"), GridFalse = L(\"Allow\")", fields, StringComparison.Ordinal);
         Assert.Matches(new Regex(@"Prop = ""Expires"".*ShowInGrid = false"), fields);
         Assert.Matches(new Regex(@"Prop = ""ExpiresTime"".*GridLabel = L\(""Expires""\), GridOnlyWhen = ""Expires"", GridEmpty = L\(""Never""\), GridWidth = 150"), fields);

         // Both lists are the one set of fields.
         Assert.Equal(2, Regex.Matches(Specs(), @"Fields = SenderListFields\(\d+\)").Count);
      }

      /// <summary>
      /// The domain dialog's list fits its tab: the grid has 564 pixels there, under
      /// the dialog's frame, the card's padding and the vertical bar the grid always
      /// keeps, and an address column of 240 beside Action, Reason's floor and Expires
      /// passed them by 36, so the grid scrolled sideways under a bar in every
      /// language at the only width the dialog has.
      /// </summary>
      [Fact]
      public void TheDomainDialogsAllowAndBlockList_FitsItsTab()
      {
         Match domain = Regex.Match(Specs(), @"DomainSenderList\(string domainName\)[\s\S]*?Fields = SenderListFields\((\d+)\)");
         Assert.True(domain.Success, "DomainSenderList no longer builds its fields with SenderListFields.");
         const double action = 90, reasonFloor = 120, expires = 150, tabGrid = 564;
         double address = double.Parse(domain.Groups[1].Value, CultureInfo.InvariantCulture);
         Assert.True(address + action + reasonFloor + expires <= tabGrid,
            "The domain dialog's allow and block list is " + (address + action + reasonFloor + expires - tabGrid) + " pixels wider than its tab, so the grid scrolls sideways.");
         Assert.True(address >= 160, "The address column is narrower than its widest header, Address or domain in Japanese.");
      }

      [Fact]
      public void TheNamesTab_IsHeadedAliasDomain_AndTheExternalTab_KeepsTheServerColumn()
      {
         string specs = Specs();
         Assert.Matches(new Regex(@"Prop = ""AliasName"", Label = L\(""Alias domain name""\), GridLabel = L\(""Alias domain""\), Default = """", Hint = F\(""Such as \{0\}"", ""example\.net""\)"), specs);
         Assert.Matches(new Regex(@"Prop = ""ServerType"".*ShowInGrid = false"), specs);
         // The connection security is off the grid as well, so that the server has
         // the room (F199, ExternalAccountsGridTests).
         Assert.Matches(new Regex(@"Prop = ""ConnectionSecurity"",\s*Label = L\(""Connection security""\),[^}]*ShowInGrid = false"), specs);
      }

      /// <summary>
      /// Every column keeps its width, so that a grid wider than its card scrolls
      /// sideways rather than squeezing its columns, which cut the remote domains
      /// grid's headers at every width their fields named; and a grid that
      /// scrolls sideways keeps its vertical bar. A field's width is its pixels at
      /// 100 per cent, raised with the Windows text size as its words are, where at
      /// 150 per cent a fixed width cut them short.
      /// </summary>
      [Fact]
      public void EveryColumn_KeepsItsWidth_AndTheGridScrollsSideways()
      {
         string editor = ReadControlPanelFile(Path.Join("Views", "CollectionEditorView.cs"));
         Assert.Contains(": new DataGridLength(TextScaleFactor.Scaled(f.GridWidth)),", editor, StringComparison.Ordinal);
         Assert.Contains("MinWidth = TextScaleFactor.Scaled(double.IsNaN(f.GridWidth) ? StarColumnFloor : f.GridWidth)", editor, StringComparison.Ordinal);
         Assert.Contains("private const double StarColumnFloor = 120;", editor, StringComparison.Ordinal);
         Assert.Contains("ScrollViewer.SetVerticalScrollBarVisibility(grid_, ScrollBarVisibility.Visible);", editor, StringComparison.Ordinal);
      }

      [Fact]
      public void TheEditor_HeadsAColumnByItsGridLabel_AndKeepsTheCaptionAsItsToolTip()
      {
         string editor = ReadControlPanelFile(Path.Join("Views", "CollectionEditorView.cs"));
         Assert.Contains("Header = f.GridLabel ?? f.Label,", editor, StringComparison.Ordinal);
         Assert.Contains("GridStyles.HeaderCaption(col, f.Label);", editor, StringComparison.Ordinal);
         Assert.Contains("new CellConverter(f.GridTrue, f.GridFalse, f.GridEmpty)", editor, StringComparison.Ordinal);
         Assert.Contains("new GatedCellConverter(f.GridTrue, f.GridFalse, f.GridEmpty)", editor, StringComparison.Ordinal);

         string header = WithheldRowTests.MemberText(ReadControlPanelFile(Path.Join("Services", "GridStyles.cs")), "public static void HeaderCaption(DataGridColumn column, string caption)");
         Assert.True(header != null, "GridStyles.HeaderCaption is gone.");
         Assert.Contains("FrameworkElement.ToolTipProperty, caption", header, StringComparison.Ordinal);
         Assert.Contains("AutomationProperties.HelpTextProperty, caption", header, StringComparison.Ordinal);
      }

      // ---- helpers -------------------------------------------------------------------------

      private static IEnumerable<Dictionary<string, string>> Fields()
      {
         foreach (Match spec in FieldSpecs.Matches(Specs()))
         {
            var field = new Dictionary<string, string>(StringComparer.Ordinal);
            foreach (Match setting in Setting.Matches(spec.Groups["body"].Value))
               field[setting.Groups["name"].Value] = setting.Groups["text"].Value;
            yield return field;
         }
      }

      private static string Specs() => ReadControlPanelFile(Path.Join("Views", "CollectionSpecs.cs"));

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
