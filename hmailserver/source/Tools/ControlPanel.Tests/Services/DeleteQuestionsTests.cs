// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Linq;
using System.Xml.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// What the Control Panel's delete and remove questions say (Services/Confirmations):
   /// each names the thing acted on and what goes with it, and each page asks its own.
   /// </summary>
   public class DeleteQuestionsTests
   {
      // ---- Domains & accounts, public folders (F065) ------------------------------------

      [Fact]
      public void TheAccountsQuestion_SaysTheMailboxAndItsMessagesGo()
      {
         string question = Confirmations.DeleteAccount("user00@example.com");
         Assert.Contains("user00@example.com", question, StringComparison.Ordinal);
         Assert.Contains("mailbox and every message", question, StringComparison.Ordinal);
         Assert.Contains("no undo", question, StringComparison.Ordinal);
      }

      [Fact]
      public void TheAliasAndListQuestions_SayMailToThemStops_AndTheMembersAreKept()
      {
         Assert.Contains("no longer delivered", Confirmations.DeleteAlias("sales@example.com"), StringComparison.Ordinal);
         string list = Confirmations.DeleteDistributionList("everyone@example.com");
         Assert.Contains("everyone@example.com", list, StringComparison.Ordinal);
         Assert.Contains("members' mailboxes are not affected", list, StringComparison.Ordinal);
      }

      [Fact]
      public void TheDomainsQuestion_IsOneWholeSentencePerCount()
      {
         string none = Confirmations.DeleteDomain("example.com", 0);
         string one = Confirmations.DeleteDomain("example.com", 1);
         string many = Confirmations.DeleteDomain("example.com", 12);

         Assert.Equal(3, new[] { none, one, many }.Distinct().Count());
         Assert.All(new[] { none, one, many }, text => Assert.Contains("example.com", text, StringComparison.Ordinal));
         Assert.Contains("no accounts", none, StringComparison.Ordinal);
         Assert.Contains("one account", one, StringComparison.Ordinal);
         Assert.Contains("12 accounts", many, StringComparison.Ordinal);
      }

      [Fact]
      public void ThePublicFoldersQuestion_NamesTheFolderUnquoted()
      {
         string question = Confirmations.DeletePublicFolder("Sales");
         Assert.Contains("public folder Sales and every message", question, StringComparison.Ordinal);
         Assert.DoesNotContain("'", question, StringComparison.Ordinal);
      }

      // ---- SSL certificates, IP ranges, directory synchronisation (F128) ---------------

      [Fact]
      public void TheCertificatesQuestion_NamesThePortsThatPresentIt()
      {
         string unread = Confirmations.DeleteCertificate("legacy.example.com", null);
         string none = Confirmations.DeleteCertificate("legacy.example.com", Array.Empty<string>());
         string one = Confirmations.DeleteCertificate("legacy.example.com", new[] { "SMTP 465" });
         string two = Confirmations.DeleteCertificate("legacy.example.com", new[] { "SMTP 465", "IMAP 993" });

         Assert.Equal(4, new[] { unread, none, one, two }.Distinct().Count());
         Assert.All(new[] { unread, none, one, two }, text => Assert.Contains("legacy.example.com", text, StringComparison.Ordinal));
         Assert.Contains("No TCP/IP port presents it", none, StringComparison.Ordinal);
         Assert.Contains("SMTP 465", one, StringComparison.Ordinal);
         Assert.Contains("does not start again", one, StringComparison.Ordinal);
         Assert.Contains("SMTP 465, IMAP 993", two, StringComparison.Ordinal);
      }

      [Fact]
      public void TheCertificatesPage_CountsTheTlsPortsThatNameTheCertificate()
      {
         string page = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "SslCertificatesView.xaml.cs"));
         string delete = WithheldRowTests.MemberText(page, "private void Delete_Click(object sender, RoutedEventArgs e)");
         Assert.True(delete != null, "SslCertificatesView.Delete_Click is gone.");
         Assert.Contains("Confirmations.DeleteCertificate(row.Name, PortsPresenting_(row.Id))", delete, StringComparison.Ordinal);
         Assert.Contains("L(\"_Delete certificate\")", delete, StringComparison.Ordinal);

         string ports = WithheldRowTests.MemberText(page, "private static List<string> PortsPresenting_(int certificateId)");
         Assert.True(ports != null, "SslCertificatesView.PortsPresenting_ is gone.");
         Assert.Contains("(int)port.ConnectionSecurity > 0", ports, StringComparison.Ordinal);
         Assert.Contains("(int)port.SSLCertificateID == certificateId", ports, StringComparison.Ordinal);
         Assert.Contains("return null;", ports, StringComparison.Ordinal);
      }

      [Fact]
      public void TheRangesQuestion_NamesItsAddresses_AndWhereTheyGo()
      {
         string question = Confirmations.DeleteIpRange("Branch office", "10.1.0.0", "10.1.0.255");
         Assert.Contains("Branch office (10.1.0.0 - 10.1.0.255)", question, StringComparison.Ordinal);
         Assert.Contains("next range that covers them", question, StringComparison.Ordinal);
      }

      [Fact]
      public void TheDirectorySyncQuestion_RepeatsThePreviewsCounts()
      {
         string all = Confirmations.DirectorySyncApply("", 4, 2, 0, 7, false);
         Assert.StartsWith("Apply the preview to every domain linked to a directory?", all, StringComparison.Ordinal);
         Assert.Contains("Mailboxes created: 4. Mailboxes updated: 2. Directory entries skipped: 7.", all, StringComparison.Ordinal);
         Assert.DoesNotContain("inactive", all, StringComparison.Ordinal);
         Assert.EndsWith("No message is deleted and no account is removed.", all, StringComparison.Ordinal);

         string one = Confirmations.DirectorySyncApply("example.com", 4, 2, 3, 7, true);
         Assert.StartsWith("Apply the preview to the domain example.com?", one, StringComparison.Ordinal);
         Assert.Contains("Accounts marked inactive: 3.", one, StringComparison.Ordinal);

         string page = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "DirectorySyncView.cs"));
         string apply = WithheldRowTests.MemberText(page, "private async Task ApplyAsync_()");
         Assert.True(apply != null, "DirectorySyncView.ApplyAsync_ is gone.");
         Assert.Contains("Confirmations.DirectorySyncApply(", apply, StringComparison.Ordinal);
         Assert.Contains("previewReport_", apply, StringComparison.Ordinal);
         Assert.Contains("L(\"_Apply\")", apply, StringComparison.Ordinal);
      }

      // ---- Administrator roles (F069) ------------------------------------------------------

      [Fact]
      public void TheRolesQuestion_NamesTheRoleAndTheAccount()
      {
         string question = Confirmations.TakeRoleAway("helpdesk", "helen@example.test");
         Assert.Contains("helpdesk", question, StringComparison.Ordinal);
         Assert.Contains("helen@example.test", question, StringComparison.Ordinal);
         Assert.Contains("admin level", question, StringComparison.Ordinal);
      }

      [Fact]
      public void TakeTheRoleAway_ActsOnTheChosenRowAlone_AsksFirst_AndIsADangerTextButtonGatedByTheList()
      {
         string page = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "RolesView.cs"));

         string unassign = WithheldRowTests.MemberText(page, "private void Unassign_()");
         Assert.True(unassign != null, "RolesView.Unassign_ is gone.");
         Assert.DoesNotContain("address_.Text", unassign, StringComparison.Ordinal);
         int asks = unassign.IndexOf("Confirmations.TakeRoleAway(row.Role, row.Address)", StringComparison.Ordinal);
         int writes = unassign.IndexOf("Write_(row.Address, \"\", \"\")", StringComparison.Ordinal);
         Assert.True(asks >= 0 && writes > asks, "Take the role away writes without asking first.");
         Assert.Contains("destructive: true", unassign, StringComparison.Ordinal);

         Assert.Contains("ButtonStyles.DangerText(take_);", page, StringComparison.Ordinal);
         Assert.Contains("SelectionGate.Bind(assignmentGrid_, take_);", page, StringComparison.Ordinal);

         // A read that failed or worked enables Give the role alone: the gate is
         // the selection's, and ReadFailure enabling Take the role away would
         // override it with nothing chosen.
         Assert.DoesNotContain("Reload_, give_, take_", page, StringComparison.Ordinal);
         Assert.DoesNotContain("ReadFailure.Clear(readNotice_, give_, take_)", page, StringComparison.Ordinal);
      }

      // ---- Acts that ran without a question (F184) ------------------------------------------

      [Fact]
      public void TheFolderPermissionsQuestion_NamesWhomTheEntryIsFor_AndTheFolder()
      {
         string account = Confirmations.RemoveFolderPermission(0, "jane@example.com", "Sales");
         string group = Confirmations.RemoveFolderPermission(1, "Sales team", "Sales");
         string anyone = Confirmations.RemoveFolderPermission(2, "", "Sales");

         Assert.Equal(3, new[] { account, group, anyone }.Distinct().Count());
         Assert.Contains("jane@example.com", account, StringComparison.Ordinal);
         Assert.Contains("the group Sales team", group, StringComparison.Ordinal);
         Assert.Contains("anyone", anyone, StringComparison.Ordinal);
         Assert.All(new[] { account, group, anyone }, text => Assert.Contains("the folder Sales", text, StringComparison.Ordinal));
      }

      [Fact]
      public void TheReleaseQuestion_NamesTheGroupAndCounts()
      {
         string[] questions =
         {
            Confirmations.ReleaseQuarantined(1, "mass-mail.example"),
            Confirmations.ReleaseQuarantined(5, "mass-mail.example"),
            Confirmations.ReleaseQuarantined(1, null),
            Confirmations.ReleaseQuarantined(7, null)
         };

         Assert.Equal(4, questions.Distinct().Count());
         Assert.Contains("the 5 messages from mass-mail.example", questions[1], StringComparison.Ordinal);
         Assert.Contains("the 7 checked messages", questions[3], StringComparison.Ordinal);
         Assert.All(questions, text => Assert.Contains("delivered at once", text, StringComparison.Ordinal));
      }

      /// <summary>
      /// The quarantine's delete of one message names it by its subject and its
      /// sender, as the queue's question names its message, where it asked "Delete
      /// this message permanently?" of a row's own delete and of a group or a checked
      /// set that held one.
      /// </summary>
      [Fact]
      public void TheQuarantinesOneMessageQuestion_NamesTheMessageBySubjectAndSender()
      {
         string named = Confirmations.DeleteQuarantined("Invoice 42", "billing@mass-mail.example");
         Assert.Contains("the message “Invoice 42” from billing@mass-mail.example permanently?", named, StringComparison.Ordinal);
         Assert.Contains("no other copy", named, StringComparison.Ordinal);

         string bare = Confirmations.DeleteQuarantined("  ", "billing@mass-mail.example");
         Assert.Contains("the message with no subject from billing@mass-mail.example permanently?", bare, StringComparison.Ordinal);
         Assert.Contains("from <> permanently?", Confirmations.DeleteQuarantined("Undelivered mail", ""), StringComparison.Ordinal);

         string page = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "QuarantineView.cs"));
         string ask = WithheldRowTests.MemberText(page, "private bool ConfirmDelete_(IReadOnlyList<int> ids, string groupTitle)");
         Assert.True(ask != null, "QuarantineView.ConfirmDelete_ is gone.");
         Assert.Contains("review_.MessagesWith(ids)", ask, StringComparison.Ordinal);
         Assert.Contains("Confirmations.DeleteQuarantined(one.Subject, one.Sender)", ask, StringComparison.Ordinal);
         Assert.Contains("if (!ConfirmDelete_(ids, groupTitle))", page, StringComparison.Ordinal);
      }

      /// <summary>
      /// Every language quotes the quarantined message's subject with the marks its
      /// sendout question quotes a subject with.
      /// </summary>
      [Fact]
      public void TheQuarantinesSubject_IsQuoted_AsEachLanguageQuotesTheSendoutsSubject()
      {
         const string Sendout = "Send “{0}” to every active account matching “{1}”?";
         const string Delete = "Delete the message “{0}” from {1} permanently?\n\nThe sender was told it was accepted, so nothing will retry and there is no other copy.";

         string resources = Path.GetDirectoryName(ConfirmationPatternTests.ControlPanelFile(Path.Join("Resources", "Strings.resx")));
         int catalogues = 0;
         foreach (string path in Directory.EnumerateFiles(resources, "Strings.*.resx"))
         {
            string tag = Path.GetFileNameWithoutExtension(path).Substring("Strings.".Length);
            var catalogue = XDocument.Load(path).Root.Elements("data")
               .ToDictionary(d => (string)d.Attribute("name"), d => (string)d.Element("value") ?? "", StringComparer.Ordinal);

            Assert.True(catalogue.TryGetValue(Sendout, out string sendout) && sendout.Length > 0, tag + " does not translate the sendout question.");
            int at = sendout.IndexOf("{0}", StringComparison.Ordinal);
            string opening = sendout.Substring(sendout[at - 1] == ' ' ? at - 2 : at - 1, sendout[at - 1] == ' ' ? 2 : 1);
            string closing = sendout.Substring(at + 3, sendout[at + 3] == ' ' ? 2 : 1);

            Assert.True(catalogue.TryGetValue(Delete, out string delete) && delete.Length > 0, tag + " does not translate the quarantine's question for one message.");
            Assert.True(delete.Contains(opening + "{0}" + closing, StringComparison.Ordinal),
               tag + ": the subject is not quoted " + opening + "…" + closing + " in \"" + delete + "\".");
            catalogues++;
         }

         Assert.Equal(17, catalogues);
      }

      [Fact]
      public void TheSweepQuestions_NameTheRetentionWindow()
      {
         Assert.NotEqual(Confirmations.DeleteExpiredQuarantine(1), Confirmations.DeleteExpiredQuarantine(30));
         Assert.Contains("longer than 30 days", Confirmations.DeleteExpiredQuarantine(30), StringComparison.Ordinal);
         Assert.Contains("longer than a day", Confirmations.DeleteExpiredQuarantine(1), StringComparison.Ordinal);
         Assert.Contains("older than 90 days", Confirmations.DeleteExpiredTrace(90), StringComparison.Ordinal);
         Assert.Contains("older than a day", Confirmations.DeleteExpiredTrace(1), StringComparison.Ordinal);
         Assert.Contains("delayed again", Confirmations.ClearGreylisting, StringComparison.Ordinal);
      }

      [Fact]
      public void TheSendoutQuestion_NamesTheSubject_TheCountAndTheWildcard()
      {
         string uncounted = Confirmations.Sendout("Maintenance tonight", null, "*@example.com");
         string one = Confirmations.Sendout("Maintenance tonight", 1, "*@example.com");
         string many = Confirmations.Sendout("Maintenance tonight", 12, "*@example.com");

         Assert.Equal(3, new[] { uncounted, one, many }.Distinct().Count());
         Assert.All(new[] { uncounted, one, many }, text =>
         {
            Assert.Contains("Maintenance tonight", text, StringComparison.Ordinal);
            Assert.Contains("*@example.com", text, StringComparison.Ordinal);
         });
         Assert.Contains("the 12 active accounts", many, StringComparison.Ordinal);
         Assert.Contains("*@example.com", Confirmations.SendoutToNobody("*@example.com"), StringComparison.Ordinal);

         // Quoted as the subject is: a ? in a wildcard stands for any one character,
         // and "matching *?" ran the pattern into the question's own mark.
         Assert.All(new[] { uncounted, one, many }, text =>
            Assert.EndsWith("matching “*@example.com”?", text, StringComparison.Ordinal));
         Assert.Contains("matches “*@example.com”,", Confirmations.SendoutToNobody("*@example.com"), StringComparison.Ordinal);
      }

      /// <summary>
      /// Every language quotes the sendout's wildcard with the marks its own question
      /// quotes the subject with, so that no translation leaves it bare against the
      /// question's mark.
      /// </summary>
      [Fact]
      public void TheSendoutsWildcard_IsQuoted_AsEachLanguageQuotesTheSubject()
      {
         (string Key, string Wildcard)[] questions =
         {
            ("Send “{0}” to every active account matching “{1}”?", "{1}"),
            ("Send “{0}” to the one active account matching “{1}”?", "{1}"),
            ("Send “{0}” to the {1} active accounts matching “{2}”?", "{2}"),
            ("No active account in an active domain matches “{0}”, so nobody would receive the message.", "{0}"),
         };

         string resources = Path.GetDirectoryName(ConfirmationPatternTests.ControlPanelFile(Path.Join("Resources", "Strings.resx")));
         int catalogues = 0;
         foreach (string path in Directory.EnumerateFiles(resources, "Strings.*.resx"))
         {
            string tag = Path.GetFileNameWithoutExtension(path).Substring("Strings.".Length);
            var catalogue = XDocument.Load(path).Root.Elements("data")
               .ToDictionary(d => (string)d.Attribute("name"), d => (string)d.Element("value") ?? "", StringComparer.Ordinal);

            // The subject's marks, from the first question: the character either side
            // of {0}, with the space French sets inside its guillemets.
            Assert.True(catalogue.TryGetValue(questions[0].Key, out string first) && first.Length > 0, tag + " does not translate the sendout question.");
            int at = first.IndexOf("{0}", StringComparison.Ordinal);
            Assert.True(at > 0 && at + 3 < first.Length, tag + ": the subject is not quoted in \"" + first + "\".");
            string opening = first.Substring(first[at - 1] == ' ' ? at - 2 : at - 1, first[at - 1] == ' ' ? 2 : 1);
            string closing = first.Substring(at + 3, first[at + 3] == ' ' ? 2 : 1);
            Assert.False(char.IsLetterOrDigit(opening.Trim()[0]) || char.IsLetterOrDigit(closing.Trim()[0]), tag + ": the subject is not quoted in \"" + first + "\".");

            foreach ((string key, string wildcard) in questions)
            {
               Assert.True(catalogue.TryGetValue(key, out string value) && value.Length > 0, tag + " does not translate \"" + key + "\".");
               Assert.True(value.Contains(opening + wildcard + closing, StringComparison.Ordinal),
                  tag + ": the wildcard " + wildcard + " is not quoted " + opening + "…" + closing + " in \"" + value + "\".");
            }

            catalogues++;
         }

         Assert.Equal(17, catalogues);
      }

      // The server's own cases for StringParser::WildcardMatch, which the sendout's
      // count has to agree with, and its case-insensitive form.
      [Theory]
      [InlineData("Test", "Test", true)]
      [InlineData("", "", true)]
      [InlineData("Test*", "Testar", true)]
      [InlineData("Test*", "Test", true)]
      [InlineData("Test*", "Te", false)]
      [InlineData("*two*", "one-two-three", true)]
      [InlineData("*two*", "one-three", false)]
      [InlineData("?two?", "one-two-three", false)]
      [InlineData("?two?", "-two-", true)]
      [InlineData("?", "A", true)]
      [InlineData("?", "AA", false)]
      [InlineData("*", "A", true)]
      [InlineData("*", "", true)]
      [InlineData("?", "", false)]
      [InlineData("??語", "標準語", true)]
      [InlineData("?語", "標準語", false)]
      [InlineData("test@*", "test@example.test", true)]
      [InlineData("test@test.co.*", "test@test.co.uk", true)]
      [InlineData("test@test.co.*", "test@example.test", false)]
      [InlineData("test@test.co.*", "test@test.co", false)]
      [InlineData("TEST@*", "test@Example.test", true)]   // WildcardMatchNoCase
      [InlineData("a.b@example.com", "axb@example.com", false)] // a dot is a dot
      [InlineData("a+b@example.com", "a+b@example.com", true)]
      public void TheSendoutsWildcard_MatchesAsTheServerMatches(string wildcard, string address, bool matches)
         => Assert.Equal(matches, SendoutAudience.Matches(wildcard, address));

      [Theory]
      [InlineData(true, true, true)]
      [InlineData(false, true, false)]   // an inactive account is not sent to
      [InlineData(true, false, false)]   // nor one of an inactive domain
      public void TheSendout_ReachesActiveAccountsOfActiveDomains(bool accountActive, bool domainActive, bool reaches)
         => Assert.Equal(reaches, SendoutAudience.Reaches("*", "a@example.com", accountActive, domainActive));

      [Theory]
      [InlineData(null, false)]
      [InlineData("", false)]
      [InlineData(" \r\n\t", false)]
      [InlineData("The server restarts at ten.", true)]
      public void AnEmptyMessage_IsNotSent(string body, bool sent)
         => Assert.Equal(sent, SendoutAudience.HasBody(body));

      [Fact]
      public void EachSendoutPress_StartsWithNoOutcomeOnThePage()
      {
         // The warning that the wildcard reached nobody stayed through the next press
         // and a cancelled question, under a form that now reached some accounts.
         string send = WithheldRowTests.MemberText(ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "UtilityViews.cs")), "private void Send()");
         Assert.True(send != null, "SendoutView.Send is gone.");
         int clears = send.IndexOf("status_.Visibility = Visibility.Collapsed;", StringComparison.Ordinal);
         Assert.True(clears >= 0, "SendoutView.Send does not hide the last outcome.");
         Assert.DoesNotContain("return;", send.Substring(0, clears), StringComparison.Ordinal);
         Assert.True(clears < send.IndexOf("Confirmations.SendoutToNobody(", StringComparison.Ordinal), "SendoutView.Send hides its outcome only after the warning.");
         Assert.True(clears < send.IndexOf("MessageBox.Confirm(", StringComparison.Ordinal), "SendoutView.Send hides its outcome only after the question.");

         // The message field's refusal too, before any return: a press that stopped at
         // the From address or the subject left "Write the message first" beside the
         // message typed since.
         int clearsField = send.IndexOf("bodyRow_.Error = null;", StringComparison.Ordinal);
         Assert.True(clearsField >= 0, "SendoutView.Send does not clear the message field's refusal.");
         Assert.DoesNotContain("return;", send.Substring(0, clearsField), StringComparison.Ordinal);
      }

      [Theory]
      [InlineData("FolderPermissionsDialog.cs", "private void DeleteSelected()", "Confirmations.RemoveFolderPermission(", "perms.DeleteByDBID(id)")]
      [InlineData("QuarantineView.cs", "private void SweepExpired()", "Confirmations.DeleteExpiredQuarantine(days)", "quarantine.DeleteExpired()")]
      [InlineData("MessageTraceView.cs", "private void SweepExpired()", "Confirmations.DeleteExpiredTrace(days)", "trace.DeleteExpired()")]
      [InlineData("UtilityViews.cs", "private void Send()", "Confirmations.Sendout(", "utilities.EmailAllAccounts(")]
      public void EachIrreversibleAct_AsksBeforeItActs(string file, string member, string question, string act)
      {
         string text = WithheldRowTests.MemberText(ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", file)), member);
         Assert.True(text != null, file + ": " + member + " is gone.");
         int asks = text.IndexOf(question, StringComparison.Ordinal);
         int acts = text.IndexOf(act, StringComparison.Ordinal);
         Assert.True(asks >= 0 && acts > asks, file + ": " + member + " acts without asking first.");
      }

      [Fact]
      public void TheSweeps_AskNothingAtARetentionOfNought_AndCannotBePressedThere()
      {
         foreach (string file in new[] { "QuarantineView.cs", "MessageTraceView.cs" })
         {
            string page = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", file));
            string sweep = WithheldRowTests.MemberText(page, "private void SweepExpired()");
            Assert.Contains("if (days <= 0)", sweep, StringComparison.Ordinal);
            string state = WithheldRowTests.MemberText(page, "private void ShowSweepState_(int days)");
            Assert.True(state != null, file + ": ShowSweepState_ is gone.");
            Assert.Contains("removeExpired_.IsEnabled = days > 0;", state, StringComparison.Ordinal);
         }
      }

      [Fact]
      public void AGroupsOrTheCheckedMessagesRelease_AsksFirst_WithCancelOnEnter()
      {
         string page = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "QuarantineView.cs"));
         string act = WithheldRowTests.MemberText(page, "private void Act_(IReadOnlyList<int> ids, string action, string groupTitle = null, bool checkedSet = false)");
         Assert.True(act != null, "QuarantineView.Act_ is gone.");
         Assert.Contains("(checkedSet || groupTitle != null || ids.Count > 1) && !ConfirmRelease_(ids.Count, groupTitle)", act, StringComparison.Ordinal);
         Assert.Contains("Act_(review_.CheckedIds, \"release\", checkedSet: true)", page, StringComparison.Ordinal);
         string ask = WithheldRowTests.MemberText(page, "private static bool ConfirmRelease_(int count, string groupTitle)");
         Assert.Contains("cancelByDefault: true", ask, StringComparison.Ordinal);
      }

      [Fact]
      public void ClearingTheGreylistingRecords_AsksFirst()
      {
         string page = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "ServerSettingsView.xaml.cs"));
         int clear = page.IndexOf("a.ClearGreyListingTriplets()", StringComparison.Ordinal);
         int ask = page.IndexOf("Confirmations.ClearGreylisting", StringComparison.Ordinal);
         Assert.True(ask >= 0 && clear > ask && clear - ask < 600, "Clear greylisting triplets clears without asking first.");

         // A Cancel answers the press, as every other settings action that asks
         // first does: with no text the result line kept the last press's
         // "Greylisting triplets cleared.", so that a cancelled press read as a
         // second clear.
         string question = ConfirmationPatternTests.Calls(page.Substring(ask - 40, clear - ask + 40), "MessageBox.Confirm(").Single();
         string onCancel = page.Substring(page.IndexOf(question, StringComparison.Ordinal) + question.Length, 80);
         Assert.Matches(@"^\)\s*return \(true, L\(""Nothing was changed\.""\)\);", onCancel);
      }

      // ---- Account two-factor (F200) ---------------------------------------------------------

      [Fact]
      public void TheTwoFactorQuestions_NameTheAccount_AndWhatStopsWorking()
      {
         string again = Confirmations.EnrolAgain("user00@example.com");
         Assert.Contains("user00@example.com", again, StringComparison.Ordinal);
         Assert.Contains("replaces the current one", again, StringComparison.Ordinal);
         Assert.Contains("stop working", again, StringComparison.Ordinal);

         string remove = Confirmations.RemoveSecondFactor("user00@example.com");
         Assert.Contains("user00@example.com", remove, StringComparison.Ordinal);
         Assert.Contains("password signs in to mail clients again", remove, StringComparison.Ordinal);
      }

      [Fact]
      public void EnrolAgain_SaysItReplacesTheSecret_IsSecondary_AndAsksFirst()
      {
         string panel = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "AccountTwoFactorPanel.cs"));

         string state = WithheldRowTests.MemberText(panel, "private void ShowEnrolState_(bool enrolled)");
         Assert.True(state != null, "AccountTwoFactorPanel.ShowEnrolState_ is gone.");
         Assert.Contains("L(\"_Enrol again (replaces the current secret)\")", state, StringComparison.Ordinal);
         Assert.Contains("enrolled ? Wpf.Ui.Controls.ControlAppearance.Secondary : Wpf.Ui.Controls.ControlAppearance.Primary", state, StringComparison.Ordinal);

         string reload = WithheldRowTests.MemberText(panel, "public void Reload()");
         Assert.Contains("ShowEnrolState_(enabled);", reload, StringComparison.Ordinal);

         string enrol = WithheldRowTests.MemberText(panel, "private void Enrol()");
         int asks = enrol.IndexOf("Confirmations.EnrolAgain(address_)", StringComparison.Ordinal);
         int enrols = enrol.IndexOf("account.EnrolTOTP()", StringComparison.Ordinal);
         Assert.True(asks >= 0 && enrols > asks, "Enrol again replaces the secret without asking first.");
         Assert.Contains("enrolled_ &&", enrol, StringComparison.Ordinal);

         string disable = WithheldRowTests.MemberText(panel, "private void Disable()");
         int asksRemove = disable.IndexOf("Confirmations.RemoveSecondFactor(address_)", StringComparison.Ordinal);
         int removes = disable.IndexOf("account.DisableTOTP()", StringComparison.Ordinal);
         Assert.True(asksRemove >= 0 && removes > asksRemove, "Remove takes the second factor away without asking first.");
      }

      [Theory]
      [InlineData("DomainsView.xaml.cs", "private void DeleteAccount_(string address)", "Confirmations.DeleteAccount(address)", "L(\"_Delete account\")")]
      [InlineData("DomainsView.xaml.cs", "private void DeleteAlias_(string name)", "Confirmations.DeleteAlias(name)", "L(\"_Delete alias\")")]
      [InlineData("DomainsView.xaml.cs", "private void DeleteDistList_(string address)", "Confirmations.DeleteDistributionList(address)", "L(\"_Delete list\")")]
      [InlineData("DomainsView.xaml.cs", "private void DeleteDomain_(string name)", "Confirmations.DeleteDomain(name, accountCount)", "L(\"_Delete domain\")")]
      [InlineData("PublicFoldersView.cs", "private void DeleteFolder()", "Confirmations.DeletePublicFolder(name)", "L(\"_Delete folder\")")]
      [InlineData("IPRangesView.xaml.cs", "private void Delete_Click(object sender, RoutedEventArgs e)", "Confirmations.DeleteIpRange(row.Name, row.LowerIP, row.UpperIP)", "L(\"_Delete range\")")]
      public void EachDelete_AsksItsOwnQuestion_WithTheActOnItsButton(string file, string member, string question, string action)
      {
         string text = WithheldRowTests.MemberText(ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", file)), member);
         Assert.True(text != null, file + ": " + member + " is gone.");
         Assert.Contains(question, text, StringComparison.Ordinal);
         Assert.Contains(action, text, StringComparison.Ordinal);
         Assert.Contains("destructive: true", text, StringComparison.Ordinal);
      }
   }
}
