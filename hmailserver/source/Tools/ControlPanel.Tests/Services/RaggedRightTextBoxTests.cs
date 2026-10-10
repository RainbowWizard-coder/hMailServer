// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Text.RegularExpressions;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// A text box that wraps sets its lines ragged right (review findings F026, F299).
   /// WPF-UI's styles stretch a text box's content, HorizontalContentAlignment
   /// Stretch, which WPF's text box reads as justified text: each wrapped line of a
   /// signature, an auto-reply, a rule's reply or disclaimer, a list's footer, a
   /// server message or an LDAP filter was spread to the box's width as it was
   /// typed, and the domain's DKIM record wrote "Value:", "v=DKIM1;" and "k=rsa;" at
   /// the box's left, middle and right above the key. The implicit styles of both
   /// text boxes, WPF's and WPF-UI's, set a box that wraps to the left, and leave a
   /// one-line box, and those inside WPF-UI's own controls, as WPF-UI draws them.
   /// </summary>
   public class RaggedRightTextBoxTests
   {
      /// <summary>
      /// Each style is based on WPF-UI's by its key: {StaticResource {x:Type TextBox}}
      /// inside the dictionary that defines the implicit style finds the entry being
      /// defined and yields null, a box in the classic theme (SharedTemplatesTests).
      /// </summary>
      [Theory]
      [InlineData("TextBox", "DefaultTextBoxStyle")]
      [InlineData("ui:TextBox", "DefaultUiTextBoxStyle")]
      public void ATextBoxThatWrapsIsRaggedRight(string type, string wpfUiKey)
      {
         string app = Read("App.xaml");
         Match style = Regex.Match(app,
            @"<Style TargetType=""\{x:Type " + Regex.Escape(type) + @"\}"" BasedOn=""\{StaticResource " + wpfUiKey + @"\}"">(.*?)</Style>",
            RegexOptions.Singleline);
         Assert.True(style.Success, "App.xaml has no implicit style for " + type + " based on WPF-UI's " + wpfUiKey + ".");

         // A trigger for each way of wrapping, and nothing else: a one-line box keeps
         // WPF-UI's alignment, and the style sets no other property of any box.
         string body = style.Groups[1].Value;
         Assert.Equal(2, Regex.Matches(body, @"<Setter ").Count);
         foreach (string wrapping in new[] { "Wrap", "WrapWithOverflow" })
         {
            Match trigger = Regex.Match(body,
               @"<Trigger Property=""TextWrapping"" Value=""" + wrapping + @""">\s*<Setter Property=""HorizontalContentAlignment"" Value=""Left"" />\s*</Trigger>");
            Assert.True(trigger.Success, type + " does not set a box that wraps (" + wrapping + ") to the left.");
         }
      }

      /// <summary>
      /// The style reaches a box only while nothing set the alignment on the box
      /// itself: a local HorizontalContentAlignment beats a style's trigger. No text
      /// box that wraps sets one, so every such box is ragged right; a one-line box
      /// may, as the two-factor code's box centres its six digits.
      /// </summary>
      [Fact]
      public void NoTextBoxThatWrapsSetsItsOwnContentAlignment()
      {
         string controlPanel = ContrastTokensTests.FindControlPanelDirectory();
         Assert.True(controlPanel != null, "The Control Panel's sources were not found above the test binaries.");
         var offending = new List<string>();
         int boxes = 0;
         foreach (string file in Directory.GetFiles(Path.Join(controlPanel, "Views"), "*.*", SearchOption.AllDirectories))
         {
            if (!file.EndsWith(".cs", StringComparison.Ordinal) && !file.EndsWith(".xaml", StringComparison.Ordinal))
               continue;
            string source = File.ReadAllText(file);

            // "new TextBox { ... }", and "TextBox name = new() { ... }" or "TextBox Make() => new() { ... }".
            var inCode = new Regex(@"(?:new (?:Wpf\.Ui\.Controls\.)?TextBox\b[^;{]*|\bTextBox\s+\w+(?:\([^)]*\))?\s*=>?\s*new\(\)\s*)\{(?<body>[^{}]*)\}");
            foreach (Match box in inCode.Matches(source))
            {
               boxes++;
               if (WrapsAndAligns(box.Groups["body"].Value))
                  offending.Add(Path.GetFileName(file));
            }

            foreach (Match box in Regex.Matches(source, @"<(?:ui:)?TextBox\b[^>]*>"))
            {
               boxes++;
               if (WrapsAndAligns(box.Value))
                  offending.Add(Path.GetFileName(file));
            }
         }

         Assert.True(boxes >= 40, "Fewer text boxes than the Control Panel builds: " + boxes);
         Assert.True(offending.Count == 0, "A text box that wraps sets its own HorizontalContentAlignment: " + string.Join(", ", offending));
      }

      private static bool WrapsAndAligns(string box)
         => box.Contains("HorizontalContentAlignment", StringComparison.Ordinal)
            && Regex.IsMatch(box, @"TextWrapping(?:\.|="")Wrap");

      private static string Read(params string[] parts)
      {
         string controlPanel = ContrastTokensTests.FindControlPanelDirectory();
         Assert.True(controlPanel != null, "The Control Panel's sources were not found above the test binaries.");
         return File.ReadAllText(Path.Join(controlPanel, Path.Join(parts)));
      }
   }
}
