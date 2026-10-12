// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// What a screen reader says for a list's row (review findings F070, F285): a
   /// row class without a ToString of its own is announced by its class name, and
   /// a row of bare values says nothing without its columns. Services/RowSpeech is
   /// the sentence; the pages' row classes call it, which their source is read for.
   /// </summary>
   public class RowSpeechTests
   {
      [Fact]
      public void ARow_IsItsCellsThatHaveSomething_JoinedWithCommas()
      {
         Assert.Equal("serveradmin, Everything, on every domain", RowSpeech.Of("serveradmin", "Everything", null, "  ", "on every domain"));
         Assert.Equal("", RowSpeech.Of());
         Assert.Equal("", RowSpeech.Of(null, ""));
      }

      [Fact]
      public void ALabelledCell_SaysItsColumn_AndAnEmptyOneSaysNothing()
      {
         Assert.Equal("Active: Yes", RowSpeech.Labelled("Active", "Yes"));
         Assert.Equal("Yes", RowSpeech.Labelled("", "Yes"));
         Assert.Equal("", RowSpeech.Labelled("Score", ""));
         Assert.Equal("zen.spamhaus.org, Active: Yes, Score: 3", RowSpeech.Of("zen.spamhaus.org", RowSpeech.Labelled("Active", "Yes"), RowSpeech.Labelled("Score", "3")));
      }

      /// <summary>
      /// The administrator roles page: each card is titled and each grid named for
      /// what it holds, and each row says its role, or its account, role and
      /// domains (review finding F070).
      /// </summary>
      [Fact]
      public void TheRolesPage_NamesItsCardsGridsAndRows()
      {
         string page = Read(Path.Join("Views", "RolesView.cs"));

         // The roles card says what a built-in role and an account without a role are (review finding F085),
         // and holds the grid under the Control Deck's button (F089).
         Assert.Matches(new Regex(@"new Card\s*\{\s*Title = L\(""Roles""\),[\s\S]{0,800}?Content = rolesContent\s*\}"), page);
         Assert.Contains("rolesContent.Children.Add(roleGrid_);", page, StringComparison.Ordinal);
         Assert.Contains("new Card { Title = L(\"Who holds a role\"), Content = held", page, StringComparison.Ordinal);
         Assert.Contains("AutomationProperties.SetName(roleGrid_, L(\"Roles\"));", page, StringComparison.Ordinal);
         Assert.Contains("AutomationProperties.SetName(assignmentGrid_, L(\"Who holds a role\"));", page, StringComparison.Ordinal);
         Assert.Matches(new Regex(@"class RoleRow[\s\S]{0,900}?public override string ToString\(\) => RowSpeech\.Of\(Name, Purpose\);"), page);
         Assert.Matches(new Regex(@"class AssignmentRow[\s\S]{0,500}?public override string ToString\(\) => RowSpeech\.Of\(Address, Role, Domains\);"), page);
      }

      /// <summary>
      /// Every class a page binds a grid's or a list's columns to (nameof(Row.Cell))
      /// says itself: without a ToString of its own a row is announced by its class
      /// name, which is what the audit trail's, the suppression list's, the message
      /// trace's, journaling's and eDiscovery's rows said (review finding F285).
      /// </summary>
      [Fact]
      public void EveryGridRowClass_SaysItself()
      {
         string root = ControlPanelDirectory();
         string all = string.Join(Environment.NewLine, Directory.EnumerateFiles(Path.Join(root, "Views"), "*.cs", SearchOption.AllDirectories)
            .Concat(Directory.EnumerateFiles(Path.Join(root, "Services"), "*.cs", SearchOption.AllDirectories))
            .Select(File.ReadAllText));
         var bad = new System.Collections.Generic.List<string>();
         int checkedRows = 0;

         foreach (string file in Directory.EnumerateFiles(Path.Join(root, "Views"), "*.cs", SearchOption.AllDirectories))
         {
            string source = File.ReadAllText(file);
            if (!source.Contains("DataGrid", StringComparison.Ordinal) && !source.Contains("ListView", StringComparison.Ordinal) && !source.Contains("ListBox", StringComparison.Ordinal))
               continue;

            foreach (string row in Regex.Matches(source, @"nameof\((\w+)\.\w+\)").Select(m => m.Groups[1].Value).Distinct())
            {
               Match declared = Regex.Match(all, @"\bclass " + row + @"\b[^{;]*\{");
               if (!declared.Success)
                  continue;   // an enum or a type of another assembly

               checkedRows++;
               if (!Body_(all, declared.Index + declared.Length).Contains("override string ToString", StringComparison.Ordinal))
                  bad.Add(Path.GetFileName(file) + ": " + row);
            }
         }

         Assert.True(checkedRows >= 15, "Only " + checkedRows + " row classes were found; the scan needs updating.");
         Assert.True(bad.Count == 0, "A grid row a screen reader announces by its class name: " + string.Join(", ", bad));
      }

      [Fact]
      public void TheServicesRows_SayThemselves()
      {
         Assert.Equal("mail.example.net, our partner's relay, Added (server time): " + ServerTimeText.Cell("2026-10-01"),
            new NeverBanNameRow { Pattern = "mail.example.net", Note = "our partner's relay", Created = "2026-10-01" }.ToString());
         Assert.Equal("D:/Data/example.com/ada/1A.eml, ada@example.com",
            new MessageStoreConsistencyEntry { ExpectedPath = "D:/Data/example.com/ada/1A.eml", Account = "ada@example.com" }.ToString());
      }

      /// <summary>
      /// A collection page's row is its cells as the grid shows them, the first
      /// cell of words bare and every other with its column's name, where it was
      /// the first three raw values with no column at all.
      /// </summary>
      [Fact]
      public void ACollectionRow_SaysItsCellsAsTheGridShowsThem()
      {
         string editor = Read(Path.Join("Views", "CollectionEditorView.cs"));
         Assert.Contains("var row = new Row { Id = TryGetId(item), Speech = Speak_ };", editor, StringComparison.Ordinal);
         Assert.Contains("=> Speech?.Invoke(this) ??", editor, StringComparison.Ordinal);
         Assert.Matches(new Regex(@"private string Speak_\(Row row\)[\s\S]{0,900}?RowSpeech\.Labelled\(MnemonicText\.Strip\(f\.GridLabel \?\? f\.Label \?\? """"\), text\)"), editor);
         Assert.Matches(new Regex(@"private static string CellWords_\(FieldSpec f, Row row\)[\s\S]{0,1500}?return option\.Label;"), editor);
      }

      private static string Body_(string source, int open)
      {
         int depth = 1;
         int i = open;
         while (depth > 0 && i < source.Length)
         {
            if (source[i] == '{')
               depth++;
            else if (source[i] == '}')
               depth--;
            i++;
         }

         return source.Substring(open, i - open);
      }

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

      internal static string Read(string relative)
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
