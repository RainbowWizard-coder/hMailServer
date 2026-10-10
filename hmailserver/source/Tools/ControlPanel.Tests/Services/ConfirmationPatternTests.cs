// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Xml.Linq;
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
         ("Views/TcpIpPortsView.xaml.cs", "private void OfferReinitialize_()", "Reinitialise _now", "_Later"),
         ("Views/FeatureSettingsView.Updates.cs", "private async void RunUpdateAction_(string action)", "_Install the update", "_Later"),
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

      /// <summary>
      /// A question's act and the button that declines it have Alt keys of their own
      /// in every language. Where two buttons share one, WPF's AccessKeyManager only
      /// moves the keyboard between them and presses neither, so the key of both the
      /// act and the decline stops working: Czech gave Apply now and Later both Alt+P,
      /// and Ukrainian gave them, and Reinitialise now and Later, both Alt+З. The two
      /// captions meet only in the dialog, at run time, so check-mnemonics.py, which
      /// reads each page's own captions, cannot see them; this reads every question
      /// whose act and decline are both a caption of the catalogue.
      /// </summary>
      [Fact]
      public void EachQuestionsActAndDecline_HaveAltKeysOfTheirOwn_InEveryLanguage()
      {
         var pairs = new List<(string Where, string Action, string Decline)>();
         foreach ((string relative, string source) in SourceFiles())
         {
            foreach (string call in Calls(source, "Confirm("))
            {
               if (!call.Contains("decline:", StringComparison.Ordinal))
                  continue;

               Match decline = Regex.Match(call, @"decline:\s*L\(""((?:[^""\\]|\\.)*)""\)");
               Assert.True(decline.Success, relative + ": a question names its decline with something other than one caption, which this cannot check: " + call.Split('\n')[0].Trim());

               // The act is the third argument: Confirm(text, caption, action, ...).
               List<string> arguments = Arguments(call);
               Match action = arguments.Count > 2 ? Regex.Match(arguments[2], @"^L\(""((?:[^""\\]|\\.)*)""\)$") : Match.Empty;
               Assert.True(action.Success, relative + ": a question with a decline of its own names its act with something other than one caption, which this cannot check: " + call.Split('\n')[0].Trim());
               pairs.Add((relative, Regex.Unescape(action.Groups[1].Value), Regex.Unescape(decline.Groups[1].Value)));
            }
         }

         // The two that clashed are among those read, so that the scan cannot go blind.
         Assert.Contains(pairs, pair => pair.Action == "_Apply now" && pair.Decline == "_Later");
         Assert.Contains(pairs, pair => pair.Action == "Reinitialise _now" && pair.Decline == "_Later");

         string resources = Path.GetDirectoryName(ControlPanelFile(Path.Join("Resources", "Strings.resx")));
         var offenders = new List<string>();
         int catalogues = 0;
         foreach (string path in Directory.EnumerateFiles(resources, "Strings.*.resx"))
         {
            string tag = Path.GetFileNameWithoutExtension(path).Substring("Strings.".Length);
            var catalogue = XDocument.Load(path).Root.Elements("data")
               .ToDictionary(d => (string)d.Attribute("name"), d => (string)d.Element("value") ?? "", StringComparer.Ordinal);

            foreach ((string where, string action, string decline) in pairs)
            {
               // A text the catalogue leaves empty is shown in English.
               string act = catalogue.TryGetValue(action, out string a) && a.Length > 0 ? a : action;
               string no = catalogue.TryGetValue(decline, out string d) && d.Length > 0 ? d : decline;
               char? actKey = AccessKeyOf_(act), noKey = AccessKeyOf_(no);
               if (actKey != null && actKey == noKey)
                  offenders.Add(tag + ", " + where + ": \"" + act + "\" and \"" + no + "\" are both Alt+" + actKey);
            }

            catalogues++;
         }

         Assert.Equal(17, catalogues);
         foreach ((string where, string action, string decline) in pairs)
         {
            if (AccessKeyOf_(action) is char key && key == AccessKeyOf_(decline))
               offenders.Add("en, " + where + ": \"" + action + "\" and \"" + decline + "\" are both Alt+" + key);
         }

         Assert.True(offenders.Count == 0,
            "A question's act and its decline share an Alt key, so neither key presses its button; move one of them in the catalogue:\n" + string.Join("\n", offenders.Distinct()));
      }

      // The languages of VerbOfTheAct_'s columns, English first.
      private static readonly string[] VerbLanguages_ =
      {
         "en", "cs", "da", "de", "es", "fi", "fr", "it", "ja", "nb", "nl", "pl", "pt-BR", "ru", "sv", "tr", "uk", "zh-Hans"
      };

      // Each act's verb in each language, as the stem its caption and its question
      // share (lower case; forms of one verb that share no stem are given apart by
      // "|"), in VerbLanguages_'s order.
      private static readonly Dictionary<string, string[]> VerbOfTheAct_ = new(StringComparer.Ordinal)
      {
         ["Re_move"] = new[] { "remove", "odebr", "fjern", "entfern", "quit", "poist", "retir", "rimuov", "削除", "fjern", "verwijder", "usu", "remov", "убра", "ta bort", "kaldır", "вилуч", "移除" },
         ["Re_move stored passphrase"] = new[] { "remove", "odstran", "fjern", "entfern", "quit", "poist", "retir", "rimuov", "削除", "fjern", "verwijder", "usu", "remov", "убра", "ta bort", "kaldır", "вилуч", "移除" },
         ["_Remove message"] = new[] { "remove", "odebr", "fjern", "entfern", "quit", "poist", "retir", "rimuov", "削除", "fjern", "verwijder", "usu", "remov", "удал", "ta bort", "kaldır", "вилуч", "移除" },
         ["Re_move the sample domain"] = new[] { "remove", "odstran", "fjern", "entfern", "elimin", "poist", "supprim", "rimuov", "削除", "fjern", "verwijder", "usu", "remov", "удал", "ta bort", "kaldır", "вилуч", "移除" },
         ["_Take the role away"] = new[] { "take", "odebr", "fjern", "entfern", "quit", "poist", "retir", "togli", "取り消", "fjern", "verwijder", "odbierz|odebra", "remov", "сня", "ta bort", "kaldır", "зня", "收回" },
         ["_Clear"] = new[] { "clear", "vymaz", "ryd", "lösch", "borr", "tyhjen", "effac", "cancell", "クリア", "tøm", "wissen", "wyczy", "limp", "очист", "rensa", "temizle", "очист", "清除" },
         ["_Clear the recovery address"] = new[] { "clear", "smaz", "slet", "lösch", "borr", "poist", "effac", "cancell", "消去", "slett", "wissen", "usu", "apag", "удал", "radera", "sil", "видал", "清除" },
         ["Clear the recovery co_des"] = new[] { "clear", "smaz", "slet", "lösch", "borr", "poist", "effac", "cancell", "消去", "slett", "wissen", "usu", "apag", "удал", "radera", "sil", "видал", "清除" },
      };

      /// <summary>
      /// A question asks with the verb its button acts with, in English and in every
      /// language (review findings F185 and F076): the button says what the question
      /// asks, and Remove, which takes an entry out of a list the thing outlives, is not
      /// Delete, which destroys it. Russian asked "Удалить" (delete) over the button
      /// "Убрать" (remove), German "entziehen" over "Rolle entfernen", the English
      /// suppression list "Take ... off" over Remove, and the recovery tab "Remove" and
      /// "End" over Clear: one act asked, another answered. Every question whose act is
      /// one of VerbOfTheAct_'s captions is held to the forms of that act's verb, which
      /// its caption must carry too, so that a caption translated anew cannot leave the
      /// table behind unseen. The Delete buttons are not held to it: Czech asks with
      /// odstranit and Spanish with eliminar beside the Smazat and Borrar of their
      /// button, synonyms both.
      /// </summary>
      [Fact]
      public void EachQuestion_AsksWithTheVerbOnItsButton_InEveryLanguage()
      {
         string confirmations = ReadControlPanelFile(Path.Join("Services", "Confirmations.cs"));
         var questions = new List<(string Where, string Action, string Question)>();
         var unread = new List<string>();
         foreach ((string relative, string source) in SourceFiles())
         {
            foreach (string call in Calls(source, "Confirm("))
            {
               // The act is the third argument: Confirm(text, caption, action, ...).
               List<string> arguments = Arguments(call);
               Match action = arguments.Count > 2 ? Regex.Match(arguments[2], @"^L\(""((?:[^""\\]|\\.)*)""\)$") : Match.Empty;
               string act = action.Success ? Regex.Unescape(action.Groups[1].Value) : null;
               if (act == null || !VerbOfTheAct_.ContainsKey(act))
                  continue;

               List<string> texts = MarkedTexts_(arguments[0]);
               Match helper = Regex.Match(arguments[0], @"^Confirmations\.(\w+)");
               if (texts.Count == 0 && helper.Success)
                  texts = MarkedTexts_(ConfirmationsMember_(confirmations, helper.Groups[1].Value));
               if (texts.Count == 0)
                  unread.Add(relative + ": " + call.Split('\n')[0].Trim());
               questions.AddRange(texts.Select(text => (relative, act, text)));
            }
         }

         Assert.True(unread.Count == 0, "A question with one of these acts is asked with a text this cannot read, so its verb goes unchecked:\n" + string.Join("\n", unread));

         // Each act is read, and the questions that asked otherwise are among those read,
         // so that the scan cannot go blind.
         foreach (string act in VerbOfTheAct_.Keys)
            Assert.Contains(questions, question => question.Action == act);
         Assert.Contains(questions, question => question.Question.StartsWith("Remove {0} from {1}?", StringComparison.Ordinal));
         Assert.Contains(questions, question => question.Question.StartsWith("Remove {0} from the suppression list of {1}?", StringComparison.Ordinal));
         Assert.Contains(questions, question => question.Question.StartsWith("Take the role {0} away from {1}?", StringComparison.Ordinal));
         Assert.Contains(questions, question => question.Question.StartsWith("Clear this account's recovery address?", StringComparison.Ordinal));
         Assert.Contains(questions, question => question.Question.StartsWith("Clear this account's one-time recovery codes?", StringComparison.Ordinal));

         string resources = Path.GetDirectoryName(ControlPanelFile(Path.Join("Resources", "Strings.resx")));
         var catalogues = new Dictionary<string, Dictionary<string, string>>(StringComparer.Ordinal) { ["en"] = null };
         foreach (string path in Directory.EnumerateFiles(resources, "Strings.*.resx"))
         {
            catalogues[Path.GetFileNameWithoutExtension(path).Substring("Strings.".Length)] = XDocument.Load(path).Root.Elements("data")
               .ToDictionary(d => (string)d.Attribute("name"), d => (string)d.Element("value") ?? "", StringComparer.Ordinal);
         }

         Assert.Equal(VerbLanguages_.Length, catalogues.Count);
         var offenders = new List<string>();
         foreach ((string tag, Dictionary<string, string> catalogue) in catalogues)
         {
            int column = Array.IndexOf(VerbLanguages_, tag);
            Assert.True(column >= 0, "Strings." + tag + ".resx has no column in VerbOfTheAct_.");
            foreach ((string where, string action, string question) in questions)
            {
               string act = Regex.Replace(Translated_(catalogue, action), @"\s*\(_.\)", "").Replace("_", "", StringComparison.Ordinal).ToLowerInvariant();
               string asked = QuestionOf_(Translated_(catalogue, question)).ToLowerInvariant();
               string[] forms = VerbOfTheAct_[action][column].Split('|');
               if (!forms.Any(form => act.Contains(form, StringComparison.Ordinal)))
                  offenders.Add(tag + ": the button \"" + act + "\" carries none of " + string.Join(", ", forms) + "; give VerbOfTheAct_ the forms of its verb");
               else if (!forms.Any(form => asked.Contains(form, StringComparison.Ordinal)))
                  offenders.Add(tag + ", " + where + ": \"" + asked + "\" over the button \"" + act + "\"");
            }
         }

         Assert.True(offenders.Count == 0,
            "A question asks with another verb than its button acts with; ask with the button's verb:\n" + string.Join("\n", offenders.Distinct()));
      }

      /// <summary>The texts an expression marks: L("..."), F("...", ...), Plural(n, "...", "...") and PluralF(n, "...", "...", ...).</summary>
      private static List<string> MarkedTexts_(string expression)
      {
         const string text = @"""((?:[^""\\]|\\.)*)""";
         var texts = Regex.Matches(expression, @"\b(?:L|F)\(\s*" + text)
            .Select(match => Regex.Unescape(match.Groups[1].Value)).ToList();
         foreach (Match plural in Regex.Matches(expression, @"\bPluralF?\([^,]+,\s*" + text + @"\s*,\s*" + text))
         {
            texts.Add(Regex.Unescape(plural.Groups[1].Value));
            texts.Add(Regex.Unescape(plural.Groups[2].Value));
         }

         return texts;
      }

      /// <summary>A member of Services/Confirmations, from its declaration to the next comment or member.</summary>
      private static string ConfirmationsMember_(string confirmations, string name)
      {
         Match declaration = Regex.Match(confirmations, @"public static string " + name + @"\b");
         Assert.True(declaration.Success, "Confirmations." + name + " is gone.");
         Match next = Regex.Match(confirmations.Substring(declaration.Index + declaration.Length), @"\n\s*(?:public |//)");
         return confirmations.Substring(declaration.Index, declaration.Length + (next.Success ? next.Index : confirmations.Length - declaration.Index - declaration.Length));
      }

      // A text the catalogue leaves empty is shown in English; English has no catalogue.
      private static string Translated_(Dictionary<string, string> catalogue, string text)
         => catalogue != null && catalogue.TryGetValue(text, out string value) && value.Length > 0 ? value : text;

      // The question a confirmation asks: its text to the first question mark, or to the
      // full stop a Japanese question ends with.
      private static readonly char[] QuestionEnds_ = { '?', '？', '。' };

      private static string QuestionOf_(string text)
      {
         int end = text.IndexOfAny(QuestionEnds_);
         return end < 0 ? text : text.Substring(0, end);
      }

      /// <summary>
      /// The key a caption's Alt key is, as WPF's AccessText reads it: the character
      /// after the first underscore that is not doubled, upper-cased as
      /// AccessKeyManager compares keys; null where there is none.
      /// </summary>
      private static char? AccessKeyOf_(string caption)
      {
         for (int i = 0; i < caption.Length - 1; i++)
         {
            if (caption[i] != '_')
               continue;
            if (caption[i + 1] == '_')
            {
               i++;
               continue;
            }

            return char.ToUpperInvariant(caption[i + 1]);
         }

         return null;
      }

      /// <summary>A call's arguments, each trimmed, split at the commas outside any parenthesis, bracket, brace or string.</summary>
      private static List<string> Arguments(string call)
      {
         var arguments = new List<string>();
         int open = call.IndexOf('(');
         int depth = 0;
         int start = open + 1;
         bool inString = false;
         for (int i = open; i < call.Length; i++)
         {
            char c = call[i];
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
            else if (c is '(' or '[' or '{')
               depth++;
            else if (c is ')' or ']' or '}')
            {
               if (--depth == 0)
               {
                  arguments.Add(call.Substring(start, i - start).Trim());
                  break;
               }
            }
            else if (c == ',' && depth == 1)
            {
               arguments.Add(call.Substring(start, i - start).Trim());
               start = i + 1;
            }
         }

         return arguments;
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
