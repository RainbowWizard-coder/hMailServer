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
   /// The Control Panel asks before an act in one way (review findings F185 and
   /// F066): Views/Dialogs.Confirm, with the thing acted on named in the question,
   /// the act named on the button and Enter on Cancel whenever the act deletes,
   /// revokes or replaces something, or stops the server's services. Confirmations
   /// came in several shapes - Yes and No, Enter answering Yes on some and No on
   /// others, OK and Cancel, a red Yes on a merge that removes nothing, "Delete this
   /// security group?" naming no group, one recovery panel still on the grey Win32
   /// box - and these hold every page to the one shape, so that a new page cannot
   /// bring an old one back.
   /// </summary>
   public class ConfirmationPatternTests
   {
      private static readonly string[] YesNoShapes =
      {
         "MessageBoxButton.YesNo", "MessageBoxButton.YesNoCancel", "MessageBoxButton.OKCancel"
      };

      [Fact]
      public void NoPage_AsksWithYesAndNo_OrOkAndCancel()
      {
         var offenders = new List<string>();
         foreach ((string relative, string source) in SourceFiles())
         {
            // The themed box keeps Win32's button sets for its Show overloads,
            // which are what it replaced; nothing else may ask with them.
            if (relative == Path.Join("Views", "Dialogs.cs"))
               continue;

            string[] lines = source.Split('\n');
            for (int i = 0; i < lines.Length; i++)
            {
               if (YesNoShapes.Any(shape => lines[i].Contains(shape, StringComparison.Ordinal)))
                  offenders.Add(relative + ":" + (i + 1) + ": " + lines[i].Trim());
            }
         }

         Assert.True(offenders.Count == 0,
            "A page asks with Yes and No or OK and Cancel; ask with Dialogs.Confirm, naming the act on its button:\n" + string.Join("\n", offenders));
      }

      [Fact]
      public void EveryPageThatAsks_AsksThroughTheThemedDialogs()
      {
         var offenders = new List<string>();
         foreach ((string relative, string source) in SourceFiles())
         {
            if (source.Contains("System.Windows.MessageBox.Show(", StringComparison.Ordinal))
               offenders.Add(relative + ": calls the Win32 box by name");

            bool asks = source.Contains("MessageBox.Show(", StringComparison.Ordinal) || source.Contains("MessageBox.Confirm(", StringComparison.Ordinal);
            if (asks && !source.Contains("using MessageBox = hMailServer.ControlPanel.Views.Dialogs;", StringComparison.Ordinal))
               offenders.Add(relative + ": MessageBox is not Views.Dialogs here, so it is the grey Win32 box");
         }

         Assert.True(offenders.Count == 0, string.Join("\n", offenders));
      }

      [Fact]
      public void NoConfirmation_AnswersWithYesOrOk()
      {
         var offenders = new List<string>();
         foreach ((string relative, string source) in SourceFiles())
         {
            foreach (string call in Calls(source, "Confirm("))
            {
               if (Regex.IsMatch(call, @"L\(""(Yes|No|OK|_Yes|_No|_OK)""\)"))
                  offenders.Add(relative + ": " + call.Split('\n')[0].Trim());
            }
         }

         Assert.True(offenders.Count == 0, "A confirmation's button says Yes or OK rather than what it does:\n" + string.Join("\n", offenders));
      }

      [Theory]
      [InlineData(true, null, true)]     // a destructive act never takes Enter unless its caller says so
      [InlineData(false, null, false)]   // an ordinary one does
      [InlineData(true, false, false)]   // as asked
      [InlineData(false, true, true)]    // an act that cannot be taken back, though it destroys nothing: a release, a restart
      [InlineData(true, true, true)]
      public void CancelTakesEnter_ForADestructiveAct_UnlessAsked(bool destructive, bool? cancelByDefault, bool cancelTakesEnter)
         => Assert.Equal(cancelTakesEnter, Confirmations.CancelTakesEnter(destructive, cancelByDefault));

      [Fact]
      public void TheConfirmation_GivesEnterWhereConfirmationsSays()
      {
         string dialogs = ReadControlPanelFile(Path.Join("Views", "Dialogs.cs"));
         string build = WithheldRowTests.MemberText(dialogs, "private static MessageDialog ConfirmDialog_(");
         Assert.True(build != null, "Dialogs.ConfirmDialog_ is gone.");
         Assert.Contains("Confirmations.CancelTakesEnter(destructive, cancelByDefault)", build, StringComparison.Ordinal);
         Assert.Contains("IsDefault: !cancelTakesEnter", build, StringComparison.Ordinal);
         Assert.Contains("IsDefault: cancelTakesEnter", build, StringComparison.Ordinal);
         Assert.Contains("decline ?? L(\"Cancel\")", build, StringComparison.Ordinal);
      }

      [Fact]
      public void EveryCollectionThatDeletes_NamesTheEntryInItsQuestion()
      {
         string specs = ReadControlPanelFile(Path.Join("Views", "CollectionSpecs.cs"));
         string[] blocks = specs.Split("new CollectionSpec", StringSplitOptions.None).Skip(1).ToArray();
         Assert.NotEmpty(blocks);

         foreach (string block in blocks)
         {
            Match title = Regex.Match(block, @"Title = L\(""([^""]+)""\)");
            string name = title.Success ? title.Groups[1].Value : block.Substring(0, Math.Min(60, block.Length));
            if (block.Contains("CanDelete = false", StringComparison.Ordinal))
               continue;

            Match prompt = Regex.Match(block, @"DeletePrompt = N\(""([^""]+)""\)");
            Assert.True(prompt.Success, name + ": the spec asks no question of its own before a delete.");
            Assert.Contains("{0}", prompt.Groups[1].Value, StringComparison.Ordinal);
            Assert.StartsWith("Delete the ", prompt.Groups[1].Value, StringComparison.Ordinal);

            Match props = Regex.Match(block, @"DeleteNameProps = new\[\] \{([^}]*)\}");
            Assert.True(props.Success, name + ": the spec does not say which value names an entry.");
            foreach (Match prop in Regex.Matches(props.Groups[1].Value, @"""(\w+)"""))
               Assert.True(specs.Contains("Prop = \"" + prop.Groups[1].Value + "\"", StringComparison.Ordinal),
                  name + ": DeleteNameProps names " + prop.Groups[1].Value + ", which is not a field.");
         }

         string editor = ReadControlPanelFile(Path.Join("Views", "CollectionEditorView.cs"));
         string delete = WithheldRowTests.MemberText(editor, "private void DeleteSelected()");
         Assert.True(delete != null, "CollectionEditorView.DeleteSelected is gone.");
         Assert.Contains("F(spec_.DeletePrompt, DeleteName_(row))", delete, StringComparison.Ordinal);
         Assert.Contains("destructive: true", delete, StringComparison.Ordinal);
         Assert.DoesNotContain("ItemNoun", delete, StringComparison.Ordinal);
      }

      [Fact]
      public void TheQueuesQuestion_NamesTheMessage_ItsSenderAndItsRecipients()
      {
         string question = Confirmations.RemoveQueuedMessage("1042", "newsletter@example.com", "jane.doe@partner.example.net");
         Assert.Contains("1042", question, StringComparison.Ordinal);
         Assert.Contains("newsletter@example.com", question, StringComparison.Ordinal);
         Assert.Contains("jane.doe@partner.example.net", question, StringComparison.Ordinal);
         Assert.Contains("without a bounce", question, StringComparison.Ordinal);
      }

      [Theory]
      [InlineData(true)]
      [InlineData(false)]
      public void TheRevokeQuestion_NamesTheKey(bool expired)
         => Assert.Contains("deploy-bot", Confirmations.RevokeApiKey("deploy-bot", expired), StringComparison.Ordinal);

      [Fact]
      public void TheRestartQuestion_IsOneSentencePerCase_AndSaysWhatARestartDrops()
      {
         string unknown = Confirmations.RestartAfterSave(null);
         string one = Confirmations.RestartAfterSave(1);
         string two = Confirmations.RestartAfterSave(2);

         Assert.Equal(3, new[] { unknown, one, two }.Distinct().Count());
         Assert.Contains("2 of them", two, StringComparison.Ordinal);
         Assert.All(new[] { unknown, one, two }, text => Assert.Contains("drops every open session", text, StringComparison.Ordinal));
      }

      [Fact]
      public void TheRestartAfterASave_IsAskedWithLaterOnEnter()
      {
         string page = ReadControlPanelFile(Path.Join("Views", "FeatureSettingsView.xaml.cs"));
         string ask = WithheldRowTests.MemberText(page, "private static bool AskRestart_(int? waiting)");
         Assert.True(ask != null, "FeatureSettingsView.AskRestart_ is gone.");
         Assert.Contains("L(\"_Restart now\")", ask, StringComparison.Ordinal);
         Assert.Contains("decline: L(\"_Later\")", ask, StringComparison.Ordinal);
         Assert.Contains("cancelByDefault: true", ask, StringComparison.Ordinal);
      }

      // Each question before an act that stops the server's services, so that every
      // open session drops: the file, the member that asks, the act's button and the
      // decline's, empty for Cancel. The restart after a save is the test above.
      private static readonly (string File, string Member, string Action, string Decline)[] StopsTheServer_ =
      {
         ("MainWindow.xaml.cs", "private void ApplyPending_Click(object sender, RoutedEventArgs e)", "_Apply now", "_Later"),
         ("Views/TcpIpPortsView.xaml.cs", "private void OfferReinitialize_()", "_Reinitialise now", "_Later"),
         ("Views/StatusView.xaml.cs", "private void RunUpdateAction_(string action)", "_Install the update", "_Later"),
         ("Views/StatusView.xaml.cs", "private void PauseResume_Click(object sender, RoutedEventArgs e)", "_Pause the mail server", ""),
      };

      public static TheoryData<string, string, string, string> StopsTheServer
      {
         get
         {
            var rows = new TheoryData<string, string, string, string>();
            foreach ((string file, string member, string action, string decline) in StopsTheServer_)
               rows.Add(file, member, action, decline);
            return rows;
         }
      }

      /// <summary>
      /// An act that stops the server's services gives Enter to the decline whether
      /// the person pressed for it or it is offered after a save (docs/ControlPanelDesign.md),
      /// where the window's Apply now, which reinitialises the server, took Enter while
      /// the TCP/IP ports page's offer of the same reinitialise gave it to Later, and the
      /// Status page's Install the update and Pause took it too.
      /// </summary>
      [Theory]
      [MemberData(nameof(StopsTheServer))]
      public void AnActThatStopsTheServersServices_GivesEnterToTheDecline(string file, string member, string action, string decline)
      {
         string text = WithheldRowTests.MemberText(ReadControlPanelFile(file.Replace('/', Path.DirectorySeparatorChar)), member);
         Assert.True(text != null, file + ": " + member + " is gone.");
         string ask = Calls(text, "MessageBox.Confirm(").SingleOrDefault(call => call.Contains("L(\"" + action + "\")", StringComparison.Ordinal));
         Assert.True(ask != null, file + ": " + member + " does not ask with " + action + ".");
         Assert.Contains("cancelByDefault: true", ask, StringComparison.Ordinal);
         if (decline.Length == 0)
            Assert.DoesNotContain("decline:", ask, StringComparison.Ordinal);
         else
            Assert.Contains("decline: L(\"" + decline + "\")", ask, StringComparison.Ordinal);
      }

      /// <summary>
      /// Every page that reinitialises the server, installs an update or pauses it is
      /// one of the questions above, so that a new one is held to the same Enter.
      /// </summary>
      [Fact]
      public void EveryPageThatStopsTheServersServices_IsHeldToItsEnter()
      {
         var held = new HashSet<string>(StopsTheServer_.Select(row => row.File.Replace('/', Path.DirectorySeparatorChar)), StringComparer.Ordinal);
         var stops = new Regex(@"\.Reinitialize\(\)|\.InstallUpdate\(\)|\bapp\.Stop\(\)");
         var offenders = SourceFiles().Where(source => stops.IsMatch(source.Source) && !held.Contains(source.Relative))
            .Select(source => source.Relative).ToList();
         Assert.True(offenders.Count == 0,
            "A page stops the server's services without being held to giving Enter to the decline; add its question to StopsTheServer:\n" + string.Join("\n", offenders));
      }

      [Fact]
      public void AMergeIsNotDrawnAsDestructive_AReplaceIs()
      {
         string page = ReadControlPanelFile(Path.Join("Views", "BackupView.xaml.cs"));
         Assert.Contains("MessageBox.Confirm(question, L(\"Control Panel\"), L(\"_Replace mailbox\"), destructive: true)", page, StringComparison.Ordinal);
         Assert.Contains("MessageBox.Confirm(question, L(\"Control Panel\"), L(\"_Merge\"))", page, StringComparison.Ordinal);
      }

      /// <summary>Each call of the method named, from the name to its closing parenthesis.</summary>
      internal static IEnumerable<string> Calls(string source, string name)
      {
         int at = 0;
         while ((at = source.IndexOf(name, at, StringComparison.Ordinal)) >= 0)
         {
            int open = at + name.Length - 1;
            int depth = 0;
            bool inString = false;
            int end = -1;
            for (int i = open; i < source.Length; i++)
            {
               char c = source[i];
               if (inString)
               {
                  if (c == '\\')
                     i++;
                  else if (c == '"')
                     inString = false;
                  continue;
               }

               if (c == '"')
                  inString = true;
               else if (c == '(')
                  depth++;
               else if (c == ')' && --depth == 0)
               {
                  end = i;
                  break;
               }
            }

            if (end < 0)
               yield break;

            yield return source.Substring(at, end + 1 - at);
            at = end + 1;
         }
      }

      /// <summary>Every C# source of the Control Panel, by its path under ControlPanel.</summary>
      internal static IEnumerable<(string Relative, string Source)> SourceFiles()
      {
         string root = Path.GetDirectoryName(ControlPanelFile("App.xaml.cs"));
         foreach (string path in Directory.EnumerateFiles(root, "*.cs", SearchOption.AllDirectories))
         {
            string relative = Path.GetRelativePath(root, path);
            string first = relative.Split(Path.DirectorySeparatorChar)[0];
            if (first is "bin" or "obj")
               continue;

            yield return (relative, File.ReadAllText(path));
         }
      }

      internal static string ReadControlPanelFile(string relative) => File.ReadAllText(ControlPanelFile(relative));

      internal static string ControlPanelFile(string relative)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relative);
            if (File.Exists(candidate))
               return candidate;
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/" + relative.Replace('\\', '/') + " could not be found from the test binaries.");
         return null;
      }
   }
}
