// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Xml.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The Windows "Text size" setting honoured (review finding F317): nothing read
   /// UISettings.TextScaleFactor or its registry value, so raising Text size to 125-150
   /// per cent changed nothing, and fixed widths would have clipped once it did. These
   /// hold the factor, the type ramp at it, the start-up that puts it in force, every
   /// shared style and the converted pages taking their text sizes from the scaled
   /// tokens, the grid floors and dialogs measured at it, and the certificates grid's
   /// fixed widths gone. Each fails against the tree as it was.
   /// </summary>
   public class TextScaleTests : IDisposable
   {
      private static readonly XNamespace Wpf = "http://schemas.microsoft.com/winfx/2006/xaml/presentation";
      private static readonly XNamespace X = "http://schemas.microsoft.com/winfx/2006/xaml";

      public void Dispose() => TextScaleFactor.Use(1.0);

      [Theory]
      [InlineData(100, 1.0)]
      [InlineData(125, 1.25)]
      [InlineData(150, 1.5)]
      [InlineData(225, 2.25)]
      [InlineData(300, 2.25)]
      [InlineData(50, 1.0)]
      [InlineData(0, 1.0)]
      public void TheRegistrysPercentage_IsTheFactor_HeldToWhatWindowsOffers(int percent, double factor)
         => Assert.Equal(factor, TextScaleFactor.FromRegistry(percent));

      [Fact]
      public void NoValue_OrOneThatIsNoWholeNumber_IsOneHundredPerCent()
      {
         Assert.Equal(1.0, TextScaleFactor.FromRegistry(null));
         Assert.Equal(1.0, TextScaleFactor.FromRegistry("big"));
         Assert.Equal(1.25, TextScaleFactor.FromRegistry(" 125 "));
         Assert.Equal(1.0, TextScaleFactor.Clamp(double.NaN));
      }

      [Theory]
      [InlineData(14, 1.5, 21)]
      [InlineData(12, 1.25, 15)]
      [InlineData(13, 1.25, 16.5)]
      [InlineData(28, 1.0, 28)]
      public void ASize_IsScaledToTheHalfPixel(double size, double factor, double expected)
         => Assert.Equal(expected, TextScaleFactor.Scaled(size, factor));

      [Fact]
      public void TheTypeRamp_IsAtTheTextSizeInForce()
      {
         Assert.Equal(12, Typography.Caption);
         Assert.Equal(14, Typography.Body);
         Assert.Equal(28, Typography.Title);

         TextScaleFactor.Use(1.5);
         Assert.Equal(18, Typography.Caption);
         Assert.Equal(21, Typography.Body);
         Assert.Equal(30, Typography.DialogTitle);
         Assert.Equal(24, Typography.DocumentHeading);
         Assert.Equal(42, Typography.Title);

         string ramp = Read("Services", "Typography.cs");
         Assert.DoesNotMatch(@"public const double", ramp);
      }

      [Fact]
      public void TheTextSize_IsPutInForceAtStartUp_BeforeTheFirstWindow()
      {
         string app = Read("", "App.xaml.cs");
         string startup = WithheldRowTests.MemberText(app, "protected override void OnStartup(StartupEventArgs e)");
         Assert.True(startup != null, "App.OnStartup is gone.");
         Assert.Contains("Services.TextScale.Install();", startup, StringComparison.Ordinal);
         Assert.True(startup.IndexOf("Services.ThemeTokens.Initialize();", StringComparison.Ordinal) < startup.IndexOf("Services.TextScale.Install();", StringComparison.Ordinal));

         string scale = Read("Services", "TextScale.cs");
         Assert.Contains(@"private const string KeyPath = @""Software\Microsoft\Accessibility"";", scale, StringComparison.Ordinal);
         Assert.Contains("private const string ValueName = \"TextScaleFactor\";", scale, StringComparison.Ordinal);
         // Every font-size resource of every merged dictionary, an icon's excepted, from the size it first held.
         Assert.Contains("name.Contains(\"FontSize\", StringComparison.Ordinal) && !name.Contains(\"Icon\", StringComparison.Ordinal)", scale, StringComparison.Ordinal);
         // And the line height of text set in lines, which rises with its text.
         Assert.Contains("|| name.StartsWith(\"AppLineHeight\", StringComparison.Ordinal)", scale, StringComparison.Ordinal);
         Assert.Contains("foreach (ResourceDictionary merged in dictionary.MergedDictionaries)", scale, StringComparison.Ordinal);
         Assert.Contains("double scaled = TextScaleFactor.Scaled(baseSize, factor);", scale, StringComparison.Ordinal);
         Assert.Contains("SystemEvents.UserPreferenceChanged +=", scale, StringComparison.Ordinal);
      }

      /// <summary>
      /// The text sizes of the shared styles and the converted pages are the ramp's tokens
      /// taken by DynamicResource, which TextScale sets: a number, or a token taken by
      /// StaticResource, is a size the text size never reaches. An icon keeps its number.
      /// A line height is a token too: Live logs' lines kept 17 while their text rose to
      /// 18 at 150 per cent, and each line ran into the next.
      /// </summary>
      [Theory]
      [InlineData("Views\\Scaffold\\Tokens.xaml")]
      [InlineData("Views\\Scaffold\\Scaffold.xaml")]
      [InlineData("App.xaml")]
      [InlineData("MainWindow.xaml")]
      [InlineData("Views\\DomainsView.xaml")]
      [InlineData("Views\\ConnectView.xaml")]
      [InlineData("Views\\DashboardView.xaml")]
      [InlineData("Views\\LogsView.xaml")]
      public void TheTextSizes_AreTheScaledTokens(string file)
      {
         string text = Read("", file.Replace('\\', Path.DirectorySeparatorChar));
         Assert.DoesNotContain("{StaticResource AppFontSize", text, StringComparison.Ordinal);
         Assert.DoesNotMatch(@"<Setter Property=""(?:TextElement\.)?FontSize"" Value=""\d", text);

         XElement root = XDocument.Parse(text).Root;
         foreach (XElement e in root.DescendantsAndSelf())
         {
            foreach (XAttribute attribute in e.Attributes().Where(a => a.Name.LocalName is "FontSize" or "TextElement.FontSize"))
            {
               if (e.Name.LocalName == "SymbolIcon" || (string)e.Attribute("Text") == "hM")
                  continue;
               Assert.True(attribute.Value.StartsWith("{DynamicResource AppFontSize", StringComparison.Ordinal),
                  file + ": " + e.Name.LocalName + " sets its text at " + attribute.Value + ", which the Windows text size does not reach.");
            }

            foreach (XAttribute attribute in e.Attributes().Where(a => a.Name.LocalName == "LineHeight"))
            {
               Assert.True(attribute.Value.StartsWith("{DynamicResource AppLineHeight", StringComparison.Ordinal),
                  file + ": " + e.Name.LocalName + " sets its lines " + attribute.Value + " apart, which the Windows text size does not reach.");
            }
         }
      }

      /// <summary>
      /// No text size set in code is a number of its own: text set in code takes its size
      /// from Typography, the ramp at the Windows text size. The help pane's headings were
      /// 20 and 16 while the text under them followed the setting, so that at 150 per cent
      /// a heading stood smaller than its body of 21. A value is a number of its own where
      /// a digit is left once the ramp's own TextScaleFactor.Scaled and the comparisons
      /// that choose a rung are taken out. An icon's size is no text, and the help
      /// document's rule is an empty line two pixels high under its border.
      /// </summary>
      [Fact]
      public void NoTextSizeSetInCode_IsANumberOfItsOwn()
      {
         string root = ContrastTokensTests.FindControlPanelDirectory();
         Assert.True(root != null, "The ControlPanel folder could not be found from the test binaries.");

         var found = new List<string>();
         foreach (string path in Directory.EnumerateFiles(root, "*.cs", SearchOption.AllDirectories))
         {
            string file = Path.GetRelativePath(root, path).Replace('\\', '/');
            if (file.StartsWith("bin/", StringComparison.Ordinal) || file.StartsWith("obj/", StringComparison.Ordinal))
               continue;

            string code = File.ReadAllText(path);
            foreach (Match m in Regex.Matches(code, @"\bFontSize\s*=(?!=)"))
            {
               string value = ValueAfter_(code, m.Index + m.Length);
               string type = EnclosingType_(code, m.Index);
               bool icon = type != null && (type == "SymbolIcon" || type.EndsWith(".SymbolIcon", StringComparison.Ordinal));
               bool rule = file == "Services/MarkdownFlow.cs" && value == "2" && type == "Paragraph";
               if (IsANumberOfItsOwn_(value) && !icon && !rule)
                  found.Add(file + " line " + LineOf_(code, m.Index) + ": FontSize = " + value);
            }

            foreach (Match m in Regex.Matches(code, @"SetValue\(\s*(?:([\w.]+)\.)?FontSizeProperty\s*,"))
            {
               string value = ValueAfter_(code, m.Index + m.Length);
               string owner = m.Groups[1].Value;
               bool icon = owner == "SymbolIcon" || owner.EndsWith(".SymbolIcon", StringComparison.Ordinal);
               if (IsANumberOfItsOwn_(value) && !icon)
                  found.Add(file + " line " + LineOf_(code, m.Index) + ": " + m.Value + " " + value);
            }
         }

         Assert.True(found.Count == 0, "Text sizes set in code as numbers of their own, which the Windows text size does not reach: " + string.Join("; ", found));

         // The help pane's headings are on the ramp: Subtitle, the document's heading, Body.
         string headings = Read("Services", "MarkdownFlow.cs");
         Assert.Contains("FontSize = heading.Level == 1 ? Typography.DialogTitle : heading.Level == 2 ? Typography.DocumentHeading : Typography.Body,", headings, StringComparison.Ordinal);
         TextScaleFactor.Use(1.5);
         Assert.Equal(24, Typography.DocumentHeading);
         Assert.True(Typography.DocumentHeading > Typography.Body && Typography.DialogTitle > Typography.DocumentHeading);
      }

      /// <summary>The expression after "=" or a call's comma, to the comma, semicolon or bracket that ends it.</summary>
      private static string ValueAfter_(string code, int start)
      {
         int depth = 0;
         int i = start;
         for (; i < code.Length; i++)
         {
            char c = code[i];
            if (c is '(' or '[' or '{')
               depth++;
            else if (c is ')' or ']' or '}')
            {
               if (depth == 0)
                  break;
               depth--;
            }
            else if ((c == ',' || c == ';') && depth == 0)
               break;
         }

         return code.Substring(start, i - start).Trim();
      }

      /// <summary>The type an object initialiser around <paramref name="at"/> builds - "SymbolIcon" for new SymbolIcon { ... } - or null.</summary>
      private static string EnclosingType_(string code, int at)
      {
         int depth = 0;
         for (int i = at - 1; i >= 0; i--)
         {
            char c = code[i];
            if (c is ')' or ']' or '}')
               depth++;
            else if (c is '(' or '[')
            {
               if (depth == 0)
                  return null;
               depth--;
            }
            else if (c == '{')
            {
               if (depth == 0)
               {
                  Match m = Regex.Match(code.Substring(0, i), @"new\s+([\w.]+)\s*(?:\([^()]*\))?\s*$");
                  return m.Success ? m.Groups[1].Value : null;
               }

               depth--;
            }
         }

         return null;
      }

      /// <summary>Whether a size is a number of its own: a digit left once the ramp's TextScaleFactor.Scaled and a rung's comparisons are taken out.</summary>
      private static bool IsANumberOfItsOwn_(string value)
      {
         string rest = Regex.Replace(value, @"TextScaleFactor\.Scaled\([^()]*\)", "");
         rest = Regex.Replace(rest, @"[!=]=\s*\d+", "");
         return Regex.IsMatch(rest, @"(?<![\w.])\d");
      }

      private static int LineOf_(string code, int index) => code.Take(index).Count(c => c == '\n') + 1;

      /// <summary>
      /// The three WPF-UI styles that hold the size their dictionary had when it was
      /// read - an AccessText's (every caption with an Alt key), a list box's and a rich
      /// text box's - take the controls' size by DynamicResource, which TextScale sets:
      /// at 150 per cent "Add domain" stayed at 14 inside a button whose font was 21.
      /// </summary>
      [Theory]
      [InlineData("AccessText", "DefaultAccessTextStyle")]
      [InlineData("ListBox", "DefaultListBoxStyle")]
      [InlineData("RichTextBox", "DefaultRichTextBoxStyle")]
      public void TheWpfUiStylesThatHoldASize_TakeTheScaledOne(string type, string basedOn)
      {
         XElement style = XDocument.Parse(Read("", "App.xaml")).Root.Descendants(Wpf + "Style")
            .SingleOrDefault(s => (string)s.Attribute("TargetType") == type && s.Attribute(X + "Key") == null);
         Assert.True(style != null, "App.xaml has no implicit " + type + " style.");
         Assert.Equal("{StaticResource " + basedOn + "}", (string)style.Attribute("BasedOn"));
         Assert.Contains(style.Elements(Wpf + "Setter"), s => (string)s.Attribute("Property") == "FontSize"
            && (string)s.Attribute("Value") == "{DynamicResource ControlContentThemeFontSize}");
      }

      [Fact]
      public void EveryTokenATextTakes_IsOneTheTokensDefine()
      {
         string tokens = Read("Views", "Scaffold", "Tokens.xaml");
         foreach (string file in new[] { "App.xaml", "MainWindow.xaml", Path.Join("Views", "DomainsView.xaml"), Path.Join("Views", "ConnectView.xaml"),
            Path.Join("Views", "DashboardView.xaml"), Path.Join("Views", "LogsView.xaml"), Path.Join("Views", "Scaffold", "Scaffold.xaml") })
         {
            foreach (Match m in Regex.Matches(Read("", file), @"\{DynamicResource (AppFontSize\w+|AppLineHeight\w+)\}"))
               Assert.Contains("x:Key=\"" + m.Groups[1].Value + "\"", tokens, StringComparison.Ordinal);
         }
      }

      [Fact]
      public void GridFloors_AreMeasuredInTheScaledFont_AndDialogsWidenWithTheirText()
      {
         string grids = Read("Services", "GridStyles.cs");
         Assert.Contains("if (set.Value is DynamicResourceExtension reference && Application.Current?.TryFindResource(reference.ResourceKey) is T resolved)", grids, StringComparison.Ordinal);
         Assert.Contains("return fallback is double size ? (T)(object)TextScaleFactor.Scaled(size) : fallback;", grids, StringComparison.Ordinal);

         string dialog = WithheldRowTests.MemberText(Read("Views", "FluentDialogWindow.cs"), "public void SizeToContentWithin(double width)");
         Assert.Contains("width * TextScaleFactor.Factor", dialog, StringComparison.Ordinal);
         Assert.Contains("SystemParameters.WorkArea.Width", dialog, StringComparison.Ordinal);

         foreach (string file in new[] { "AdministratorTwoFactorDialog.cs", "TotpPromptDialog.cs", "TotpSetupDialog.cs" })
         {
            string code = Read("Views", file);
            Assert.Contains("FontSize = Typography.Title,", code, StringComparison.Ordinal);
            Assert.Contains("MinWidth = 200,", code, StringComparison.Ordinal);
            Assert.DoesNotContain("FontSize = 28,", code, StringComparison.Ordinal);
         }
      }

      [Fact]
      public void TheCertificatesGrid_HasNoFixedWidthOfWords()
      {
         XElement grid = XDocument.Parse(Read("Views", "SslCertificatesView.xaml")).Root.Descendants(Wpf + "DataGrid")
            .Single(g => (string)g.Attribute(X + "Name") == "CertGrid");
         foreach (XElement column in grid.Element(Wpf + "DataGrid.Columns").Elements())
         {
            string width = (string)column.Attribute("Width");
            Assert.True(width == "Auto" || width.EndsWith('*'), (string)column.Attribute("Header") + " is " + width + " pixels wide, which larger text cuts short.");
         }

         // Their floors, set for the words at 100 per cent, rise with the text size.
         Assert.Contains("column.MinWidth = TextScaleFactor.Scaled(column.MinWidth);", Read("Views", "SslCertificatesView.xaml.cs"), StringComparison.Ordinal);
      }

      /// <summary>
      /// No grid column of words in a page's XAML is a fixed number of pixels wide, which
      /// larger text cuts short; a column of numbers (GridNumberText) keeps its pixels. The
      /// IP ranges' addresses and permissions were 150 and 70 pixels. The TCP/IP ports and
      /// routes grids are review finding F322's, whose columns that page floors at their
      /// widest words in its code (TcpIpPortsView.FloorColumns_).
      /// </summary>
      [Fact]
      public void NoColumnOfWordsInXaml_IsAFixedWidth()
      {
         string root = ContrastTokensTests.FindControlPanelDirectory();
         Assert.True(root != null, "The ControlPanel folder could not be found from the test binaries.");

         var found = new List<string>();
         foreach (string path in Directory.EnumerateFiles(Path.Join(root, "Views"), "*.xaml", SearchOption.AllDirectories))
         {
            string file = Path.GetFileName(path);
            if (file is "TcpIpPortsView.xaml" or "RoutesView.xaml")
               continue;

            foreach (XElement column in XDocument.Parse(File.ReadAllText(path)).Root.Descendants()
               .Where(e => e.Name.LocalName is "DataGridTextColumn" or "DataGridTemplateColumn" or "DataGridCheckBoxColumn"))
            {
               string width = (string)column.Attribute("Width");
               bool pixels = width != null && double.TryParse(width, NumberStyles.Float, CultureInfo.InvariantCulture, out _);
               bool number = (string)column.Attribute("ElementStyle") == "{StaticResource GridNumberText}";
               if (pixels && !number)
                  found.Add(file + ": " + ((string)column.Attribute("Header") ?? (string)column.Attribute("Binding")) + " is " + width + " pixels wide");
            }
         }

         Assert.True(found.Count == 0, "Columns of words at a fixed width, which larger text cuts short: " + string.Join("; ", found));
      }

      /// <summary>
      /// No grid column built in code is a fixed number of pixels wide, which larger text
      /// cuts short: the allowed and blocked domains' Added, at 150, read "9/28/2026 8:18 P…"
      /// at 150 per cent, and the suppression list's dates, at 150, were cut mid-glyph. A
      /// width is a share, or Auto over a floor, or measured from its words (GridStyles.CellWidth) or
      /// raised with the text (TextScaleFactor.Scaled). The quarantine's tick box and its
      /// four icon buttons hold no words, and its Held is measured from its header and its
      /// widest time (SizeFixedColumns_); the roles' columns of words are measured from
      /// their headers and their widest words (FixedWidth_).
      /// </summary>
      [Fact]
      public void NoColumnBuiltInCode_IsAFixedWidth()
      {
         string root = ContrastTokensTests.FindControlPanelDirectory();
         Assert.True(root != null, "The ControlPanel folder could not be found from the test binaries.");

         var allowed = new HashSet<string>(StringComparer.Ordinal)
         {
            "Views/QuarantineView.cs: CheckWidth",
            "Views/QuarantineView.cs: ActionsWidth",
            "Views/QuarantineView.cs: Math.Ceiling(held)",
            "Views/RolesView.cs: floor",
         };

         var found = new List<string>();
         foreach (string path in Directory.EnumerateFiles(root, "*.cs", SearchOption.AllDirectories))
         {
            string file = Path.GetRelativePath(root, path).Replace('\\', '/');
            if (file.StartsWith("bin/", StringComparison.Ordinal) || file.StartsWith("obj/", StringComparison.Ordinal))
               continue;

            string code = File.ReadAllText(path);
            foreach (Match m in Regex.Matches(code, @"new DataGridLength\("))
            {
               string width = ValueAfter_(code, m.Index + m.Length);
               string rest = code.Substring(m.Index + m.Length + width.Length).TrimStart();
               bool share = rest.StartsWith(",", StringComparison.Ordinal);
               // A header's width alone is no measure of the words under it: Added was
               // Math.Max(150, GridStyles.HeaderWidth(added)) and its dates were cut.
               bool measured = width.Contains("TextScaleFactor.Scaled(", StringComparison.Ordinal) || width.Contains("GridStyles.CellWidth(", StringComparison.Ordinal);
               if (!share && !measured && !allowed.Contains(file + ": " + width))
                  found.Add(file + " line " + LineOf_(code, m.Index) + ": new DataGridLength(" + width + ")");
            }
         }

         Assert.True(found.Count == 0, "Columns at a fixed width, which larger text cuts short: " + string.Join("; ", found));
      }

      /// <summary>
      /// The columns that were fixed widths of words are floored at their widest words,
      /// measured in the grid's font at the Windows text size, whenever their rows are read:
      /// the allowed and blocked domains' Added over every entry, and the IP ranges'
      /// addresses (the widest IPv4 address and the ranges' own) and their permissions'
      /// Yes and No.
      /// </summary>
      [Fact]
      public void TheColumnsThatWereFixed_AreFlooredAtTheirWidestWords()
      {
         string policy = Read("Views", "DomainPolicyListView.cs");
         string added = WithheldRowTests.MemberText(policy, "private void FloorAdded_()");
         Assert.True(added != null, "DomainPolicyListView.FloorAdded_ is gone.");
         Assert.Contains("foreach (EntryView entry in all_)", added, StringComparison.Ordinal);
         Assert.Contains("floor = Math.Max(floor, GridStyles.CellWidth(entry.Added));", added, StringComparison.Ordinal);
         Assert.Matches(@"entries_\.ItemsSource = shown;\s+FloorAdded_\(\);", policy);
         Assert.Contains("Width = DataGridLength.Auto,", WithheldRowTests.MemberText(policy, "private void BuildColumns_()"), StringComparison.Ordinal);

         string ranges = Read("Views", "IPRangesView.xaml.cs");
         string floors = WithheldRowTests.MemberText(ranges, "private void FloorColumns_(IEnumerable<RangeRow> rows)");
         Assert.True(floors != null, "IPRangesView.FloorColumns_ is gone.");
         Assert.Contains("Floor_(column, GridStyles.CellWidth(\"255.255.255.255\"));", floors, StringComparison.Ordinal);
         Assert.Contains("Floor_(LowerColumn, GridStyles.CellWidth(row.LowerIP));", floors, StringComparison.Ordinal);
         Assert.Contains("Floor_(UpperColumn, GridStyles.CellWidth(row.UpperIP));", floors, StringComparison.Ordinal);
         Assert.Contains("Floor_(column, GridStyles.CellWidth(L(\"Yes\")));", floors, StringComparison.Ordinal);
         Assert.Contains("Floor_(column, GridStyles.CellWidth(L(\"No\")));", floors, StringComparison.Ordinal);
         Assert.Contains("RangeGrid.ItemsSource = rows;", ranges, StringComparison.Ordinal);
         Assert.Contains("FloorColumns_(rows);", ranges, StringComparison.Ordinal);
      }

      /// <summary>A token's size at 100 per cent, as Tokens.xaml holds it.</summary>
      internal static string TokenSize(string key)
      {
         string tokens = Read("Views", "Scaffold", "Tokens.xaml");
         Match m = Regex.Match(tokens, @"x:Key=""" + Regex.Escape(key) + @""">([\d.]+)<");
         Assert.True(m.Success, "Tokens.xaml has no size " + key + ".");
         return m.Groups[1].Value;
      }

      private static string Read(params string[] parts) => HostedAddressTests.ReadControlPanelFile(Path.Join(parts));
   }
}
