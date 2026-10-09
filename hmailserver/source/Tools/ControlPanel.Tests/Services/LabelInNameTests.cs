// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// A captioned control's accessible name starts with its caption (WCAG 2.5.3,
   /// Label in Name; review findings F017, F127, F202). Some thirty buttons and
   /// boxes had their name replaced by a sentence - Refresh was "Re-check the
   /// external prerequisites", Preview "Show what a synchronisation would do,
   /// changing nothing", Members… "Edit the members of the selected group", a
   /// dialog's Note box "Why it is listed" - so a screen reader said words nobody
   /// could see and "click Refresh" in voice control reached nothing. The sentence
   /// is the control's help text and tool tip now (Views/ControlHelp.Explain).
   ///
   /// The source is read, as the mnemonics checker reads it: in C#, a control
   /// made with Content = L("caption") or captioned by a FieldRow (or a helper
   /// that builds one), then given AutomationProperties.SetName; in XAML, an
   /// element with both a caption and an AutomationProperties.Name. A name may say
   /// more after the caption - "Copy host of the MX record" - never something else.
   /// </summary>
   public class LabelInNameTests
   {
      private const string Literal = "\"((?:[^\"\\\\]|\\\\.)*)\"";

      private static readonly Regex[] Captioned =
      {
         new(@"\b(\w+)\s*=\s*new\s+[\w.]*(?:Button|CheckBox|RadioButton|ToggleButton|HyperlinkButton)\b[^;]*?Content\s*=\s*L\(" + Literal + @"\)", RegexOptions.Singleline),
         new(@"\b(\w+)\s*=\s*\w*Button\w*\(\s*L\(" + Literal + @"\)"),
      };

      private static readonly Regex[] Fields =
      {
         new(@"(?:FormField_|Field|Captioned|LabelledBox_|LabelledCombo_|Label)\(\s*L\(" + Literal + @"\)\s*,\s*(\w+)"),
         new(@"new FieldRow\s*\{[^}]*?Label\s*=\s*L\(" + Literal + @"\)[^}]*?Content\s*=\s*(\w+)", RegexOptions.Singleline),
      };

      private static readonly Regex SetName = new(@"AutomationProperties\.SetName\(\s*(\w+)\s*,\s*(?:L|F)\(" + Literal);

      private static readonly Regex XamlElement = new(@"<[\w:.]+\b[^<>]*?>", RegexOptions.Singleline);
      private static readonly Regex XamlContent = new(@"\bContent=""\{loc:L '((?:[^'\\]|\\.)*)'\}""");
      private static readonly Regex XamlName = new(@"AutomationProperties\.Name=""\{loc:L '((?:[^'\\]|\\.)*)'\}""");

      [Fact]
      public void EveryCaptionedControl_IsNamedByItsCaption()
      {
         string root = ControlPanelDirectory();
         var bad = new List<string>();

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
               if (captions.TryGetValue(m.Groups[1].Value, out string caption) && !StartsWithCaption(m.Groups[2].Value, caption))
                  bad.Add(Path.GetFileName(file) + ": \"" + caption + "\" is named \"" + m.Groups[2].Value + "\"");
            }
         }

         foreach (string file in Directory.EnumerateFiles(root, "*.xaml", SearchOption.AllDirectories).Where(Ours))
         {
            foreach (Match element in XamlElement.Matches(File.ReadAllText(file)))
            {
               Match caption = XamlContent.Match(element.Value);
               Match name = XamlName.Match(element.Value);
               if (caption.Success && name.Success && !StartsWithCaption(name.Groups[1].Value, caption.Groups[1].Value))
                  bad.Add(Path.GetFileName(file) + ": \"" + caption.Groups[1].Value + "\" is named \"" + name.Groups[1].Value + "\"");
            }
         }

         Assert.True(bad.Count == 0, "A control's name must start with its caption; put the explanation in its help text (ControlHelp.Explain):\n" + string.Join("\n", bad));
      }

      /// <summary>The rule itself, on the captions that broke it and on the shapes that keep it.</summary>
      [Theory]
      [InlineData("Refresh", "_Refresh", true)]
      [InlineData("Copy host of the MX record", "Copy host", true)]
      [InlineData("Account address", "Acc_ount", true)]
      [InlineData("Setup checklist", "Setup _checklist…", true)]
      [InlineData("Re-check the external prerequisites", "_Refresh", false)]
      [InlineData("Show what a synchronisation would do, changing nothing", "_Preview", false)]
      [InlineData("Why it is listed", "N_ote", false)]
      [InlineData("Add distribution list", "Add li_st", false)]
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
