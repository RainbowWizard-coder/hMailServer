// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Xml.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The Allowed and blocked domains page (review finding F266): the Action column
   /// says Allowed or Blocked with what each does in its tool tip, Added is the
   /// reader's date and time marked as the server's, every column is trimmed, an
   /// entry is changed in place over the REST API through Edit…, Enter or a
   /// double-click, and the explanation that stood in a notice is in the subtitle.
   /// </summary>
   public class DomainPolicyEditTests
   {
      [Fact]
      public void TheActionColumn_SaysAWord_AndItsTipWhatItDoes()
      {
         Assert.Equal("Allowed", DomainPolicyWords.Action("white"));
         Assert.Equal("Blocked", DomainPolicyWords.Action("black"));
         Assert.Equal("Allowed: not greylisted, not refused on score", DomainPolicyWords.ActionTip("white"));
         Assert.Equal("Blocked: refused at MAIL FROM", DomainPolicyWords.ActionTip("black"));
      }

      [Fact]
      public void Added_IsTheReadersShortDateAndTime_OrWhatTheServerWrote()
      {
         CultureInfo before = CultureInfo.CurrentCulture;
         try
         {
            CultureInfo.CurrentCulture = CultureInfo.GetCultureInfo("de-DE");
            Assert.Equal("09.10.2026 14:22", DomainPolicyWords.Added("2026-10-09 14:22:05"));
            CultureInfo.CurrentCulture = CultureInfo.GetCultureInfo("en-GB");
            Assert.Equal("09/10/2026 14:22", DomainPolicyWords.Added("2026-10-09 14:22:05"));
         }
         finally
         {
            CultureInfo.CurrentCulture = before;
         }

         Assert.Equal("", DomainPolicyWords.Added(""));
         Assert.Equal("", DomainPolicyWords.Added(null));
         Assert.Equal("yesterday", DomainPolicyWords.Added("yesterday"));
      }

      [Theory]
      [InlineData("envelope", 0)]
      [InlineData("helo", 1)]
      [InlineData("both", 2)]
      public void TheDialogsChoices_AreTheServersWords(string word, int choice)
      {
         Assert.Equal(choice, DomainPolicyWords.MatchOnChoice(word));
         Assert.Equal(word, DomainPolicyWords.MatchOnWord(choice));
         Assert.Equal(0, DomainPolicyWords.ActionChoice("white"));
         Assert.Equal(1, DomainPolicyWords.ActionChoice("black"));
         Assert.Equal("white", DomainPolicyWords.ActionWord(0));
         Assert.Equal("black", DomainPolicyWords.ActionWord(1));
      }

      /// <summary>An entry is changed by PUT /api/v1/domain-policy-list/{id} with all four fields, which keeps its id and its date.</summary>
      [Fact]
      public void AChange_IsAPutOfAllFourFields()
      {
         Assert.Equal("api/v1/domain-policy-list/12", DomainPolicyWords.EntryPath(12));

         using JsonDocument body = JsonDocument.Parse(DomainPolicyWords.ChangeBody(".partner.example", "helo", "black", "Rotating \"hosts\""));
         JsonElement root = body.RootElement;
         Assert.Equal(4, root.EnumerateObject().Count());
         Assert.Equal(".partner.example", root.GetProperty("pattern").GetString());
         Assert.Equal("helo", root.GetProperty("match_on").GetString());
         Assert.Equal("black", root.GetProperty("action").GetString());
         Assert.Equal("Rotating \"hosts\"", root.GetProperty("note").GetString());
      }

      /// <summary>
      /// Why an entry cannot be changed from here is said in this page's words:
      /// the REST API off, or on a loopback address of another machine, with what
      /// to do; anything else in the client's sentence, with what to do meanwhile.
      /// The settings pages' sentences said of this list that no COM property
      /// carries it, and to change "these settings" in the Control Deck.
      /// </summary>
      [Theory]
      [InlineData("0", "0")]
      [InlineData("", "0")]
      [InlineData(" ", "0")]
      [InlineData("off", "off")]
      [InlineData("-1", "-1")]
      [InlineData("65536", "65536")]
      public void AnEntryCannotBeChanged_WhileTheRestApiIsOff(string port, string said)
      {
         string why = DomainPolicyWords.Unchangeable(true, port, "127.0.0.1", true, "The settings pages' sentence.");
         Assert.Equal("An entry is changed in place over the REST API, which is off (its port on the API & monitoring page is " + said + "). Give it a port there and reinitialise the server, or delete the entry and add it again.", why);
         Assert.DoesNotContain("these settings", why, StringComparison.Ordinal);
      }

      [Theory]
      [InlineData("127.0.0.1", "127.0.0.1")]
      [InlineData("", "127.0.0.1")]
      [InlineData("::1", "::1")]
      [InlineData("LocalHost", "LocalHost")]
      public void AnEntryCannotBeChanged_FromAnotherMachine_OverALoopbackListener(string bind, string said)
      {
         Assert.Equal("An entry is changed in place over the REST API, which listens on " + said + " only, out of reach of a Control Panel on another machine. Change the entry in the Control Deck on the server, or delete it and add it again.",
            DomainPolicyWords.Unchangeable(true, "8443", bind, false, "The settings pages' sentence."));
      }

      [Fact]
      public void AnyOtherRefusal_IsTheClientsSentence_AndWhatToDoMeanwhile()
      {
         const string tail = "An entry is changed in place over the REST API; until it can be used from here, delete the entry and add it again instead.";

         // On the server a loopback listener is reached; what went wrong is the client's to say.
         Assert.Equal("The REST API could not be reached: refused. " + tail,
            DomainPolicyWords.Unchangeable(true, "8443", "127.0.0.1", true, "The REST API could not be reached: refused."));
         Assert.Equal("The REST API is not running. " + tail,
            DomainPolicyWords.Unchangeable(true, "8443", "192.0.2.10", false, "The REST API is not running."));

         // No password to sign in with is said whatever the port, as Open checks it first.
         Assert.Equal("This session holds no password. " + tail,
            DomainPolicyWords.Unchangeable(false, "0", "127.0.0.1", false, "This session holds no password."));
      }

      /// <summary>A code not given is asked for again at the next press, and the sentence says which: the page's Edit… or the dialog's Save.</summary>
      [Fact]
      public void ACodeNotGiven_SaysWhichPressAsksAgain()
      {
         Assert.Equal("An entry is changed in place over the REST API, which asks for the administrator's one-time code before it answers, and none was given. Press Edit… to be asked again, or delete the entry and add it again.",
            DomainPolicyWords.CodeNotGiven(saving: false));
         Assert.Equal("Nothing was saved: the REST API asks for the administrator's one-time code before it answers, and none was given. Press Save to be asked again.",
            DomainPolicyWords.CodeNotGiven(saving: true));
         Assert.DoesNotContain("Open the page again", DomainPolicyWords.CodeNotGiven(saving: false), StringComparison.Ordinal);
      }

      [Fact]
      public void ThePage_ChangesAnEntryThroughEdit_EnterAndADoubleClick()
      {
         string page = Page();
         Assert.Contains("var edit = MakeButton(L(\"_Edit…\"), Wpf.Ui.Controls.ControlAppearance.Secondary, (_, _) => EditSelected());", page, StringComparison.Ordinal);
         Assert.Contains("actions.Children.Insert(actions.Children.IndexOf(remove), edit);", page, StringComparison.Ordinal);
         Assert.Contains("SelectionGate.Bind(entries_, edit);", page, StringComparison.Ordinal);
         Assert.Contains("GridKeys.Wire(entries_, edit, null, null);", page, StringComparison.Ordinal);

         string edit = WithheldRowTests.MemberText(page, "private void EditSelected()");
         Assert.True(edit != null, "DomainPolicyListView.EditSelected is gone.");
         Assert.Contains("AddDomainPolicyDialog.Edit(Window.GetWindow(this), selected.Id, selected.Pattern, selected.MatchOnWord, selected.ActionWord, selected.Note)", edit, StringComparison.Ordinal);
         Assert.Contains("Status_(StatusLevel.Good, ListOutcome.Saved(saved, DateTime.Now));", edit, StringComparison.Ordinal);

         // The dialog opens only where the REST API can be used: in a default
         // install, with RestApiPort 0, it opened filled in and refused at Save.
         int check = edit.IndexOf("if (!AddDomainPolicyDialog.CanChange(Window.GetWindow(this), saving: false, out string why))", StringComparison.Ordinal);
         int open = edit.IndexOf("AddDomainPolicyDialog.Edit(", StringComparison.Ordinal);
         Assert.True(check >= 0 && open > check, "EditSelected opens the dialog without asking whether the REST API can be used.");
         Assert.Contains("Status_(StatusLevel.Warning, why);", edit.Substring(check, open - check), StringComparison.Ordinal);

         string change = WithheldRowTests.MemberText(page, "private static bool Change_(Window dialog, int id, string pattern, string matchOn, string action, string note, FieldRow patternRow, InlineNotice notice)");
         Assert.True(change != null, "AddDomainPolicyDialog.Change_ is gone.");
         Assert.Contains("client.Put(DomainPolicyWords.EntryPath(id), DomainPolicyWords.ChangeBody(pattern, matchOn, action, note),", change, StringComparison.Ordinal);
         Assert.Contains("DialogFields.ShowError(patternRow, sentence);", change, StringComparison.Ordinal);
         Assert.Contains("notice.Show(StatusLevel.Critical, client.RefusalFor(reply));", change, StringComparison.Ordinal);
         Assert.Contains("notice.Show(StatusLevel.Warning, DomainPolicyWords.CodeNotGiven(saving: true));", change, StringComparison.Ordinal);

         // Save asks again too, before the PUT: one cancelled code prompt shut
         // every Save for the rest of the session.
         int saveCheck = change.IndexOf("if (!CanChange(dialog, saving: true, out string why))", StringComparison.Ordinal);
         int put = change.IndexOf("client.Put(", StringComparison.Ordinal);
         Assert.True(saveCheck >= 0 && put > saveCheck, "Change_ puts the entry without asking the REST client again.");

         // Both ask for the code again (askAgain), and say a code not given in the page's words.
         string can = WithheldRowTests.MemberText(page, "internal static bool CanChange(Window owner, bool saving, out string why)");
         Assert.True(can != null, "AddDomainPolicyDialog.CanChange is gone.");
         Assert.Contains("client.Open(() => AskForCode_(owner), askAgain: true, out string refusal)", can, StringComparison.Ordinal);
         Assert.Contains("why = DomainPolicyWords.CodeNotGiven(saving);", can, StringComparison.Ordinal);
         Assert.Contains("why = DomainPolicyWords.Unchangeable(session.RestAuthorization() != null,", can, StringComparison.Ordinal);
         Assert.Contains("ini.Read(\"RestApiPort\", \"0\"), ini.Read(\"RestApiBindAddress\", \"127.0.0.1\")", can, StringComparison.Ordinal);
         Assert.DoesNotContain("askAgain: false", page, StringComparison.Ordinal);
      }

      [Fact]
      public void TheColumns_AreTrimmedAndAddedIsTheServersTime()
      {
         string page = Page();
         string columns = WithheldRowTests.MemberText(page, "private void BuildColumns_()");
         Assert.True(columns != null, "DomainPolicyListView.BuildColumns_ is gone.");
         Assert.Contains("Shared_(L(\"Domain or suffix\"), nameof(EntryView.Pattern), 160)", columns, StringComparison.Ordinal);
         Assert.Contains("Fitted_(L(\"Matched on\"), nameof(EntryView.MatchOn))", columns, StringComparison.Ordinal);
         Assert.Contains("ActionColumn_()", columns, StringComparison.Ordinal);
         Assert.Contains("Shared_(L(\"Note\"), nameof(EntryView.Note), 100)", columns, StringComparison.Ordinal);
         Assert.Contains("L(\"Added (server time)\")", columns, StringComparison.Ordinal);
         Assert.Contains("nameof(EntryView.Added)", columns, StringComparison.Ordinal);
         Assert.Contains("GridStyles.Trimmed(", columns, StringComparison.Ordinal);

         string action = WithheldRowTests.MemberText(page, "private static DataGridTemplateColumn ActionColumn_()");
         Assert.True(action != null, "DomainPolicyListView.ActionColumn_ is gone.");
         Assert.Contains("panel.SetBinding(ToolTipProperty, new System.Windows.Data.Binding(nameof(EntryView.ActionTip)));", action, StringComparison.Ordinal);
         Assert.Contains("TextTrimming.CharacterEllipsis", action, StringComparison.Ordinal);
      }

      /// <summary>The explanation is the subtitle's second sentence; the notice that stood on the page for good is gone.</summary>
      [Fact]
      public void TheExplanation_IsInTheSubtitle_NotInANotice()
      {
         string page = Page();
         Assert.Contains("To exempt an address or an IP range from every check use the spam exemptions; to decide per recipient use the allow and block list.", page, StringComparison.Ordinal);
         Assert.DoesNotContain("var explanation = new InlineNotice", page, StringComparison.Ordinal);
         Assert.DoesNotContain("Not the spam exemptions, whose senders", page, StringComparison.Ordinal);
      }

      /// <summary>
      /// The dialog's Add and Save are one button, never on screen together, so
      /// check-mnemonics.py, which would read the two as captions of one scope,
      /// does not read them: here each is held, in English and in every catalogue,
      /// to a key no field caption of the dialog has.
      /// </summary>
      [Fact]
      public void TheDialogsButton_SharesNoKeyWithItsFields_InAnyLanguage()
      {
         string[] fields = { "_Domain or suffix", "_Matched on", "A_ction", "_Note" };
         var catalogues = new Dictionary<string, Dictionary<string, string>> { ["en"] = null };
         foreach (string file in Directory.EnumerateFiles(ResourcesDirectory(), "Strings.*.resx"))
            catalogues[Path.GetFileName(file).Split('.')[1]] = ReadResx(file);

         var bad = new List<string>();
         foreach ((string tag, Dictionary<string, string> catalogue) in catalogues)
         {
            string Caption(string key) => catalogue == null ? key : catalogue[key];
            List<string> keys = fields.Select(f => MnemonicText.Key(Caption(f))?.ToUpperInvariant()).ToList();
            if (keys.Any(k => string.IsNullOrEmpty(k)) || keys.Distinct().Count() != keys.Count)
               bad.Add(tag + ": the fields' keys are " + string.Join(", ", keys));

            foreach (string button in new[] { "_Add", "_Save" })
            {
               string key = MnemonicText.Key(Caption(button))?.ToUpperInvariant();
               if (string.IsNullOrEmpty(key) || keys.Contains(key))
                  bad.Add(tag + ": " + Caption(button) + " shares " + key + " with a field");
            }
         }

         Assert.True(bad.Count == 0, string.Join(Environment.NewLine, bad));
      }

      private static Dictionary<string, string> ReadResx(string file)
      {
         var result = new Dictionary<string, string>(StringComparer.Ordinal);
         foreach (XElement data in XDocument.Load(file).Root.Elements("data"))
            result[(string)data.Attribute("name")] = data.Element("value")?.Value ?? "";
         return result;
      }

      private static string ResourcesDirectory()
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", "Resources");
            if (File.Exists(Path.Join(candidate, "Strings.resx")))
               return candidate;
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/Resources could not be found from the test binaries.");
         return null;
      }

      private static string Page() => ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "DomainPolicyListView.cs"));
   }
}
