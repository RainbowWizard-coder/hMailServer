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
   /// A captioned control's accessible name starts with its caption (WCAG 2.5.3,
   /// Label in Name; review findings F017, F127, F202), in every language. Some
   /// thirty buttons and boxes had their name replaced by a sentence - Refresh was
   /// "Re-check the external prerequisites", Preview "Show what a synchronisation
   /// would do, changing nothing", Members… "Edit the members of the selected
   /// group", a dialog's Note box "Why it is listed" - so a screen reader said
   /// words nobody could see and "click Refresh" in voice control reached
   /// nothing. The sentence is the control's help text and tool tip now
   /// (Views/ControlHelp.Explain).
   ///
   /// A name and a caption are two catalogue entries, translated apart, so the
   /// rule is held in each of the seventeen catalogues as well as in English:
   /// checked in English alone, it let 79 pairs through - the journaling page's
   /// Note box was "Merknad" under the caption Notat in Norwegian, a Browse… was
   /// "Nach einer CA-Zertifikatbündeldatei suchen" in German, a Revoke "API キー
   /// {0} を取り消す" under 取り消し in Japanese.
   ///
   /// The source is read, as the mnemonics checker reads it: in C#, a control
   /// made with Content = L("caption"), captioned by a FieldRow (or a helper that
   /// builds one) or by a Label whose Target it is, then given
   /// AutomationProperties.SetName; in XAML, an element with both a caption and
   /// an AutomationProperties.Name, a FieldRow whose editor carries a Name, and a
   /// caption that is the Mnemonic.Target of a named element. A name may say more
   /// after the caption - "Copy host of the MX record" - never something else.
   /// </summary>
   public class LabelInNameTests
   {
      private const string Literal = "\"((?:[^\"\\\\]|\\\\.)*)\"";
      private const string XamlLiteral = @"'((?:[^'\\]|\\.)*)'";

      private static readonly Regex[] Captioned =
      {
         new(@"\b(\w+)\s*=\s*new\s+[\w.]*(?:Button|CheckBox|RadioButton|ToggleButton|HyperlinkButton)\b[^;]*?Content\s*=\s*L\(" + Literal + @"\)", RegexOptions.Singleline),
         new(@"\b(\w+)\s*=\s*\w*Button\w*\(\s*L\(" + Literal + @"\)"),
      };

      private static readonly Regex[] Fields =
      {
         new(@"(?:FormField_|Field|Captioned|LabelledBox_|LabelledCombo_|Label)\(\s*L\(" + Literal + @"\)\s*,\s*(\w+)"),
         new(@"new FieldRow\s*\{[^}]*?Label\s*=\s*L\(" + Literal + @"\)[^}]*?Content\s*=\s*(\w+)", RegexOptions.Singleline),
         new(@"new\s+Label\s*\{[^}]*?Content\s*=\s*L\(" + Literal + @"\)[^}]*?Target\s*=\s*(\w+)", RegexOptions.Singleline),
      };

      private static readonly Regex SetName = new(@"AutomationProperties\.SetName\(\s*(\w+)\s*,\s*(?:L|F)\(" + Literal);

      private static readonly Regex XamlElement = new(@"<[\w:.]+\b[^<>]*?>", RegexOptions.Singleline);
      private static readonly Regex XamlContent = new(@"\bContent=""\{loc:L " + XamlLiteral + @"\}""");
      private static readonly Regex XamlName = new(@"AutomationProperties\.Name=""\{loc:L " + XamlLiteral + @"\}""");
      private static readonly Regex XamlRow = new(@"<scaffold:FieldRow\b([^>]*)>(.*?)</scaffold:FieldRow>", RegexOptions.Singleline);
      private static readonly Regex XamlLabel = new(@"\bLabel=""\{loc:L " + XamlLiteral + @"\}""");
      private static readonly Regex XamlTarget = new(@"<TextBlock\b[^>]*?Text=""\{loc:L " + XamlLiteral + @"\}""[^>]*?Mnemonic\.Target=""\{Binding ElementName=(\w+)\}""", RegexOptions.Singleline);

      [Fact]
      public void EveryCaptionedControl_IsNamedByItsCaption()
      {
         List<string> bad = Pairs_()
            .Where(p => !StartsWithCaption(Unescape_(p.Name), Unescape_(p.Caption)))
            .Select(p => p.File + ": \"" + p.Caption + "\" is named \"" + p.Name + "\"")
            .ToList();

         Assert.True(Pairs_().Count >= 5, "The captioned controls with names of their own were not found.");
         Assert.True(bad.Count == 0, "A control's name must start with its caption; put the explanation in its help text (ControlHelp.Explain):\n" + string.Join("\n", bad));
      }

      /// <summary>
      /// The same in every catalogue: the translation of each name starts with the
      /// translation of its caption, compared with that language's case rules.
      /// </summary>
      [Fact]
      public void EveryCaptionedControl_IsNamedByItsCaption_InEveryLanguage()
      {
         var pairs = Pairs_();
         var bad = new List<string>();
         int catalogues = 0;

         foreach (string file in Directory.EnumerateFiles(Path.Join(ControlPanelDirectory(), "Resources"), "Strings.*.resx"))
         {
            string tag = Path.GetFileName(file)["Strings.".Length..^".resx".Length];
            var culture = CultureInfo.GetCultureInfo(tag);
            Dictionary<string, string> catalogue = XDocument.Load(file).Root.Elements("data")
               .ToDictionary(d => (string)d.Attribute("name"), d => (string)d.Element("value") ?? "", StringComparer.Ordinal);
            catalogues++;

            foreach ((string where, string caption, string name) in pairs)
            {
               if (!catalogue.TryGetValue(Unescape_(caption), out string shown) || !catalogue.TryGetValue(Unescape_(name), out string said)
                  || shown.Length == 0 || said.Length == 0)
                  continue;
               if (!LabelInName.StartsWithCaption(said, shown, culture))
                  bad.Add(tag + " " + where + ": \"" + shown + "\" is named \"" + said + "\" (\"" + caption + "\", \"" + name + "\")");
            }
         }

         Assert.Equal(17, catalogues);
         Assert.True(bad.Count == 0, "In every language a control's name starts with its caption: a name that says more starts with the caption's words and then says it, as \"Host kopieren ({0}-Eintrag)\" does:\n" + string.Join("\n", bad));
      }

      /// <summary>
      /// A FieldRow names the box of a row of a box and a button beside it - a file
      /// and its Browse… - and not the panel around the two, so that the box needs
      /// no name of its own, which was a second string to translate.
      /// </summary>
      [Fact]
      public void AFieldRow_NamesTheBoxOfARowOfABoxAndAButton()
      {
         string row = File.ReadAllText(Path.Join(ControlPanelDirectory(), "Views", "Scaffold", "FieldRow.cs"));
         Assert.Matches(new Regex(@"if \(Mnemonic\.Editor\(editor, reachable: false\) is FrameworkElement inner\)\s*editor = inner;[\s\S]{0,400}?AutomationProperties\.SetName\(editor,"), row);
      }

      /// <summary>Every (where, caption, name) the source gives a captioned control.</summary>
      private static List<(string File, string Caption, string Name)> Pairs_()
      {
         string root = ControlPanelDirectory();
         var pairs = new List<(string File, string Caption, string Name)>();

         foreach (string file in Directory.EnumerateFiles(root, "*.cs", SearchOption.AllDirectories).Where(Ours))
         {
            string source = File.ReadAllText(file);
            var captions = new Dictionary<string, string>(StringComparer.Ordinal);
            foreach (Regex rx in Captioned)
            {
               foreach (Match m in rx.Matches(source))
                  captions[m.Groups[1].Value] = m.Groups[2].Value;
            }

            foreach (Regex rx in Fields)
            {
               foreach (Match m in rx.Matches(source))
                  captions.TryAdd(m.Groups[2].Value, m.Groups[1].Value);
            }

            foreach (Match m in SetName.Matches(source))
            {
               if (captions.TryGetValue(m.Groups[1].Value, out string caption))
                  pairs.Add((Path.GetFileName(file), caption, m.Groups[2].Value));
            }
         }

         foreach (string file in Directory.EnumerateFiles(root, "*.xaml", SearchOption.AllDirectories).Where(Ours))
         {
            string source = File.ReadAllText(file);
            foreach (Match element in XamlElement.Matches(source))
            {
               Match caption = XamlContent.Match(element.Value);
               Match name = XamlName.Match(element.Value);
               if (caption.Success && name.Success)
                  pairs.Add((Path.GetFileName(file), caption.Groups[1].Value, name.Groups[1].Value));
            }

            // A FieldRow's editor: its first element that is not a button beside it.
            foreach (Match row in XamlRow.Matches(source))
            {
               Match label = XamlLabel.Match(row.Groups[1].Value);
               Match name = XamlElement.Matches(row.Groups[2].Value).Cast<Match>()
                  .Where(e => !Regex.IsMatch(e.Value, @"^<[\w:.]*Button\b"))
                  .Select(e => XamlName.Match(e.Value)).FirstOrDefault(n => n.Success);
               if (label.Success && name != null)
                  pairs.Add((Path.GetFileName(file), label.Groups[1].Value, name.Groups[1].Value));
            }

            foreach (Match target in XamlTarget.Matches(source))
            {
               Match element = Regex.Match(source, @"<[\w:.]+\b[^<>]*?x:Name=""" + target.Groups[2].Value + @"""[^<>]*?>", RegexOptions.Singleline);
               Match name = element.Success ? XamlName.Match(element.Value) : Match.Empty;
               if (name.Success)
                  pairs.Add((Path.GetFileName(file), target.Groups[1].Value, name.Groups[1].Value));
            }
         }

         return pairs;
      }

      /// <summary>A C# literal's text, as the catalogue keys it.</summary>
      private static string Unescape_(string literal) => literal.Replace("\\\"", "\"").Replace("\\\\", "\\");


      /// <summary>The rule itself, on the captions that broke it and on the shapes that keep it.</summary>
      [Theory]
      [InlineData("Refresh", "_Refresh", true)]
      [InlineData("Copy host of the MX record", "Copy host", true)]
      [InlineData("Account address", "Acc_ount", true)]
      [InlineData("Setup checklist", "Setup _checklist…", true)]
      [InlineData("Re-check the external prerequisites", "_Refresh", false)]
      [InlineData("Show what a synchronisation would do, changing nothing", "_Preview", false)]
      [InlineData("Why it is listed", "N_ote", false)]
      [InlineData("Add distribution list", "Add lis_t", false)]
      [InlineData("SSL/TLS: Open SSL/TLS, which owns the protocol versions and cipher lists", "SSL/TLS →", true)]
      public void TheRule(string name, string caption, bool keeps)
      {
         Assert.Equal(keeps, StartsWithCaption(name, caption));
      }

      /// <summary>
      /// A link among several whose captions say less than where they go - ten
      /// "Settings…" on the spam filtering overview - is named by its caption and
      /// then its destination, so the ten are told apart and each starts with the
      /// words on it; one whose destination starts with its caption is named by
      /// the destination alone.
      /// </summary>
      [Theory]
      [InlineData("Settings…", "Open Blocked senders, which owns the Blocked senders settings", "Settings: Open Blocked senders, which owns the Blocked senders settings")]
      [InlineData("Open Domains", "Open Domains, which owns the settings for: DKIM signing", "Open Domains, which owns the settings for: DKIM signing")]
      [InlineData("Host name setting…", "Open Protocols, whose SMTP tab has the Host name", "Host name setting: Open Protocols, whose SMTP tab has the Host name")]
      [InlineData("Pr_operties…", "", "Properties")]
      // An overview's link to a page ends in an arrow, which is no word of its name:
      // it was "TCP/IP ports →: Open TCP/IP ports, ...".
      [InlineData("TCP/IP ports →", "Open TCP/IP ports, which owns the per-listener security setting", "TCP/IP ports: Open TCP/IP ports, which owns the per-listener security setting")]
      [InlineData("Anti-virus settings →", "Open Anti-virus settings, which owns the ClamAV settings", "Anti-virus settings: Open Anti-virus settings, which owns the ClamAV settings")]
      [InlineData("Next ›", "", "Next")]
      public void ALink_IsNamedByItsCaptionThenWhereItGoes(string caption, string destination, string name)
      {
         Assert.Equal(name, LabelInName.LinkName(caption, destination));
         Assert.True(LabelInName.StartsWithCaption(LabelInName.LinkName(caption, destination), caption));
      }

      [Theory]
      [InlineData("ExternalSetupView.cs")]
      [InlineData("LdapSettingsView.cs")]
      [InlineData("SpamOverviewView.cs")]
      [InlineData("VirusOverviewView.cs")]
      [InlineData("TlsOverviewView.cs")]
      [InlineData("DnsRecordsView.cs")]
      [InlineData("ApiKeysView.cs")]
      public void EveryPageLink_IsNamedByItsCaption(string file)
      {
         string source = File.ReadAllText(Path.Join(ControlPanelDirectory(), "Views", file));
         Assert.Contains("ControlHelp.NameLink(button, caption, accessibleName);", source, StringComparison.Ordinal);
         Assert.DoesNotContain("SetName(button, accessibleName)", source, StringComparison.Ordinal);
      }

      /// <summary>The chart cards' Show table and Show chart say the chart after the caption.</summary>
      [Fact]
      public void TheChartViewButton_IsNamedByItsCaptionThenItsChart()
      {
         string card = File.ReadAllText(Path.Join(ControlPanelDirectory(), "Views", "AccessibleChartCard.cs"));
         Assert.Contains("F(\"Show chart of {0}\", title)", card, StringComparison.Ordinal);
         Assert.Contains("F(\"Show table of {0}\", title)", card, StringComparison.Ordinal);
      }

      [Fact]
      public void ControlHelp_PutsTheSentenceInTheHelpTextAndTheToolTip()
      {
         string help = File.ReadAllText(Path.Join(ControlPanelDirectory(), "Views", "ControlHelp.cs"));
         Assert.Contains("AutomationProperties.SetHelpText(control, explanation);", help, StringComparison.Ordinal);
         Assert.Contains("control.ToolTip ??= explanation;", help, StringComparison.Ordinal);
         Assert.Contains("AutomationProperties.SetName(control, hMailServer.ControlPanel.Services.LabelInName.LinkName(caption, destination));", help, StringComparison.Ordinal);
      }

      private static bool StartsWithCaption(string name, string caption) => LabelInName.StartsWithCaption(name, caption);

      private static bool Ours(string path)
         => !path.Contains(Path.DirectorySeparatorChar + "bin" + Path.DirectorySeparatorChar, StringComparison.Ordinal)
            && !path.Contains(Path.DirectorySeparatorChar + "obj" + Path.DirectorySeparatorChar, StringComparison.Ordinal);

      private static string ControlPanelDirectory()
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel");
            if (File.Exists(Path.Join(candidate, "MainWindow.xaml")))
               return candidate;
            directory = directory.Parent;
         }

         Assert.Fail("The ControlPanel sources could not be found from the test binaries.");
         return null;
      }
   }
}
