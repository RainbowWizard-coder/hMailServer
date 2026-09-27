// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// A settings row whose value a file of the server's environment supplied is
   /// withheld: the row is an empty text box in place of its own editor, and a save
   /// sends what was typed over it (ComSetting.WithheldEntry). A REST row that read
   /// its own editor on a save did so from an editor that was never built - the list
   /// of a choice row threw a NullReferenceException out of every save of the page,
   /// and the box of a certificate row answered empty, so what was pasted over the
   /// withheld value was dropped and counted as saved - and a row given its own
   /// editor again went on answering from the withheld box of an earlier build.
   ///
   /// Scanned rather than rendered, as PageSplitTests is: the rows are WPF controls
   /// nested in the page, and building one needs a dispatcher and a live COM session.
   /// Each test here fails against the tree before those fixes.
   /// </summary>
   public class WithheldRowTests
   {
      private static readonly string[] EditorReads = { "ReadEditor(", "SelectedValue", "Combo", "combo_", "box_", "TryGetChange(" };

      [Fact]
      public void ARestRowThatCanBeWithheld_LooksAtWhatWasTypedBeforeItReadsAnEditor()
      {
         string source = ReadServerSettingsView();
         Assert.False(source == null, "ServerSettingsView.xaml.cs could not be found from the test binaries.");

         // The kinds of row that are never withheld, from ComSetting.IsWithheld.
         Match never = Regex.Match(source, @"private bool IsWithheld\s*\{\s*get\s*\{\s*if \((.*?)\)\s*return false;");
         Assert.True(never.Success, "ComSetting.IsWithheld is gone, or no longer starts with the rows that are never withheld.");
         var neverWithheld = new HashSet<string>(Regex.Matches(never.Groups[1].Value, @"this is (\w+)").Select(m => m.Groups[1].Value), StringComparer.Ordinal);
         Assert.Contains("ComPassword", neverWithheld);

         var checkedRows = new List<string>();
         foreach (Match row in Regex.Matches(source, @"\n      private sealed class (Rest\w+) : ([^\r\n{]*IRestSetting[^\r\n{]*)"))
         {
            string name = row.Groups[1].Value;
            IEnumerable<string> kinds = row.Groups[2].Value.Split(',').Select(kind => kind.Trim()).Append(name);
            if (kinds.Any(neverWithheld.Contains))
               continue;

            string body = ClassBody(source, "class " + name);
            Assert.True(body != null, name + " has no body the scan can find.");

            foreach (string member in new[] { "public bool TryGetChange(out object value)", "public void Accepted()" })
            {
               string text = MemberText(body, member);
               Assert.True(text != null, name + " has no " + member + ".");

               int read = EditorReads.Select(token => text.IndexOf(token, StringComparison.Ordinal)).Where(at => at >= 0).DefaultIfEmpty(-1).Min();
               if (read < 0)
                  continue;

               int withheld = text.IndexOf("WithheldEntry", StringComparison.Ordinal);
               Assert.True(withheld >= 0 && withheld < read,
                  name + "." + member + " reads an editor before it looks at WithheldEntry; a withheld row has none of its own:\n" + text);
            }

            checkedRows.Add(name);
         }

         foreach (string expected in new[] { "RestBool", "RestNumber", "RestText", "RestChoice" })
            Assert.Contains(expected, checkedRows);
      }

      [Fact]
      public void ARowBuiltWithItsOwnEditor_ForgetsTheBoxItHadWhenItWasWithheld()
      {
         string source = ReadServerSettingsView();
         Assert.False(source == null, "ServerSettingsView.xaml.cs could not be found from the test binaries.");

         string comSetting = ClassBody(source, "abstract class ComSetting") ?? ClassBody(source, "class ComSetting");
         Assert.True(comSetting != null, "ComSetting is gone from ServerSettingsView.");

         // CreateRow's own-editor branch goes through a method that lets the withheld
         // box go before it builds the editor.
         Match branch = Regex.Match(comSetting, @"IsWithheld \? WithheldEditor\(\) : (\w+)\(value\)");
         Assert.True(branch.Success, "CreateRow no longer chooses between the withheld box and the row's own editor.");

         string own = MemberText(comSetting, "FrameworkElement " + branch.Groups[1].Value + "(object value)");
         Assert.True(own != null, "CreateRow builds the row's own editor with " + branch.Groups[1].Value + ", which does not forget the withheld box.");
         int forgets = own.IndexOf("withheld_ = null", StringComparison.Ordinal);
         int builds = own.IndexOf("BuildEditor(", StringComparison.Ordinal);
         Assert.True(forgets >= 0 && builds > forgets, own);
      }

      /// <summary>
      /// A member of a class body, from its signature to the end of its expression or
      /// of its block - or of its declaration, for one with no body; null when the
      /// body has no such member.
      /// </summary>
      private static string MemberText(string body, string signature)
      {
         int at = body.IndexOf(signature, StringComparison.Ordinal);
         if (at < 0)
            return null;

         int start = at + signature.Length;
         int arrow = body.IndexOf("=>", start, StringComparison.Ordinal);
         int brace = body.IndexOf('{', start);
         int semicolon = body.IndexOf(';', start);
         if (semicolon >= 0 && (arrow < 0 || semicolon < arrow) && (brace < 0 || semicolon < brace))
            return body.Substring(start, semicolon + 1 - start);

         if (brace < 0 || (arrow >= 0 && arrow < brace))
         {
            int end = arrow < 0 ? -1 : body.IndexOf(';', arrow);
            return end < 0 ? null : body.Substring(start, end + 1 - start);
         }

         int depth = 0;
         for (int i = brace; i < body.Length; i++)
         {
            if (body[i] == '{')
               depth++;
            else if (body[i] == '}' && --depth == 0)
               return body.Substring(start, i + 1 - start);
         }

         return null;
      }

      /// <summary>The body of one nested class, or null (PageSplitTests' shape).</summary>
      private static string ClassBody(string source, string declaration)
      {
         Match match = Regex.Match(source, Regex.Escape(declaration) + @"\b[^\{]*\{(.*?)\n      \}", RegexOptions.Singleline);
         return match.Success ? match.Groups[1].Value : null;
      }

      private static string ReadServerSettingsView()
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);

         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", "Views", "ServerSettingsView.xaml.cs");
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);

            directory = directory.Parent;
         }

         return null;
      }
   }
}
