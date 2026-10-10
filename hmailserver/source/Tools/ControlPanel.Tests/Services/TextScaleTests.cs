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
         Assert.Contains("foreach (ResourceDictionary merged in dictionary.MergedDictionaries)", scale, StringComparison.Ordinal);
         Assert.Contains("double scaled = TextScaleFactor.Scaled(baseSize, factor);", scale, StringComparison.Ordinal);
         Assert.Contains("SystemEvents.UserPreferenceChanged +=", scale, StringComparison.Ordinal);
      }

      /// <summary>
      /// The text sizes of the shared styles and the converted pages are the ramp's tokens
      /// taken by DynamicResource, which TextScale sets: a number, or a token taken by
      /// StaticResource, is a size the text size never reaches. An icon keeps its number.
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
         }
      }

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
            foreach (Match m in Regex.Matches(Read("", file), @"\{DynamicResource (AppFontSize\w+)\}"))
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
