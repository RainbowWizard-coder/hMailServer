// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Text;
using System.Text.RegularExpressions;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;
using hMailServer;

namespace RegressionTests.Rules
{
   /// <summary>
   ///    Outbound content policy: the rule conditions that ask what is in a
   ///    message and where it is going - a card number or an IBAN in the
   ///    readable text, an attachment's type by its bytes, a recipient this
   ///    server does not host, the account-number pattern of the setting - and
   ///    the three answers: refuse at DATA naming the rule, quarantine, or let
   ///    it go and tell the sender.
   ///
   ///    An SMTP simulator on this fixture's own ports stands in for the world
   ///    outside, reached through the route to dummy-example.com, so what
   ///    leaves is what the simulator received and nothing else.
   /// </summary>
   [TestFixture]
   public class OutboundContentPolicy : TestFixtureBase
   {
      private const string AdminPassword = "testar";
      private const string Outside = "partner@dummy-example.com";

      // The ports allocated to this fixture: the simulators, a route to a port
      // nothing listens on (for a message that must never be attempted), and
      // the REST listener.
      private const int RefusalSimulatorPort = 9636;
      private const int UnattendedRoutePort = 9637;
      private static int RestPort = 9638;
      private const int NoticeSimulatorPort = 9639;
      private const int OutsideSenderSimulatorPort = 9640;

      // The predefined fields and the action by their IDL numbers rather than
      // their names: this project compiles against whatever hMailServer is
      // registered on the bench, which may predate these members, and the
      // numbers are what the server reads either way.
      private const int SensitiveContent = 10;
      private const int AttachmentType = 11;
      private const int ExternalRecipient = 12;
      private const int Content = 13;
      private const int WarnSender = 15;

      private static Rule NewRule(hMailServer.Rules rules, string name, bool allCriteria = true)
      {
         Rule rule = rules.Add();
         rule.Name = name;
         rule.Active = true;
         rule.UseAND = allCriteria;
         return rule;
      }

      private static void AddCriterion(Rule rule, int field, eRuleMatchType match, string value)
      {
         RuleCriteria criterion = rule.Criterias.Add();
         criterion.UsePredefined = true;
         criterion.PredefinedField = (eRulePredefinedField) field;
         criterion.MatchType = match;
         criterion.MatchValue = value;
         criterion.Save();
      }

      private static void AddHeaderCriterion(Rule rule, string header, eRuleMatchType match, string value)
      {
         RuleCriteria criterion = rule.Criterias.Add();
         criterion.UsePredefined = false;
         criterion.HeaderField = header;
         criterion.MatchType = match;
         criterion.MatchValue = value;
         criterion.Save();
      }

      private static void AddAction(Rule rule, int type, string value = null, string header = null)
      {
         RuleAction action = rule.Actions.Add();
         action.Type = (eRuleActionType) type;
         if (value != null)
            action.Value = value;
         if (header != null)
            action.HeaderName = header;
         action.Save();
      }

      private static void ExternalAndSensitive(Rule rule, string detector)
      {
         AddCriterion(rule, ExternalRecipient, eRuleMatchType.eMTContains, "@");
         AddCriterion(rule, SensitiveContent, eRuleMatchType.eMTContains, detector);
      }

      [TearDown]
      public void ForgetTheRules()
      {
         hMailServer.Rules global = _application.Rules;
         while (global.Count > 0)
            global[0].Delete();
      }

      [Test]
      [Description("A card number on its way out is refused at DATA in the rule's words; the same text to a local mailbox is delivered, and a clean message leaves")]
      public void ACardNumberToAnOutsideAddressIsRefusedAtData()
      {
         Account sender = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "cards@example.test", "test");
         Account colleague = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "colleague@example.test", "test");

         Rule rule = NewRule(_application.Rules, "No card numbers leave");
         ExternalAndSensitive(rule, "card_number");
         AddAction(rule, (int) eRuleActionType.eRAReject, value: "Card numbers may not be sent outside the organisation.");
         rule.Save();

         using (var server = new SmtpServerSimulator(1, RefusalSimulatorPort))
         {
            server.AddRecipientResult(new Dictionary<string, int> { { Outside, 250 } });
            server.StartListen();
            TestSetup.AddRoutePointingAtLocalhost(1, RefusalSimulatorPort, false);

            var refused = Assert.Throws<DeliveryFailedException>(() =>
               new SmtpClientSimulator().Send(sender.Address, Outside, "Payment", "Please charge 4111 1111 1111 1111, expiry 12/28."));
            StringAssert.Contains("550", refused.Message, "Refused before acceptance. Got: " + refused.Message);
            StringAssert.Contains("Card numbers may not be sent outside the organisation.", refused.Message, "In the rule's words. Got: " + refused.Message);

            // No external recipient, so the same text stays a message between colleagues.
            SmtpClientSimulator.StaticSend(sender.Address, colleague.Address, "Payment", "Please charge 4111 1111 1111 1111, expiry 12/28.");
            Pop3ClientSimulator.AssertMessageCount(colleague.Address, "test", 1);

            // A number that fails Luhn is not a card, and the message leaves.
            new SmtpClientSimulator().Send(sender.Address, Outside, "Order", "Reference 4111 1111 1111 1112, thanks.");
            server.WaitForCompletion();
            StringAssert.Contains("Reference 4111 1111 1111 1112", server.MessageData, "The clean message reached the outside.");
         }
      }

      [Test]
      [Description("An IBAN in a text attachment on its way out is quarantined under the rule's name rather than delivered")]
      public void AnIbanInATextAttachmentIsQuarantined()
      {
         _settings.SetAdministratorPassword(AdminPassword);
         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();
         try
         {
            Account sender = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "treasury@example.test", "test");

            Rule rule = NewRule(_application.Rules, "Hold bank details");
            ExternalAndSensitive(rule, "iban");
            AddAction(rule, (int) eRuleActionType.eRAQuarantine);
            rule.Save();

            // Nothing listens on this port: a message the rule holds is never
            // attempted, and one it does not hold would fail loudly here.
            TestSetup.AddRoutePointingAtLocalhost(1, UnattendedRoutePort, false);

            string attachment = Convert.ToBase64String(Encoding.ASCII.GetBytes("Beneficiary: Example Ltd\r\nIBAN: GB82 WEST 1234 5698 7654 32\r\n"));
            SmtpClientSimulator.StaticSendRaw(sender.Address, Outside, WithAttachment(sender.Address, Outside, "Bank details", "details.txt", "text/plain", attachment));
            CustomAsserts.AssertRecipientsInDeliveryQueue(0);

            string credentials = Convert.ToBase64String(Encoding.ASCII.GetBytes("Administrator:" + AdminPassword));
            (int status, string body) held = RestRequest.Send("127.0.0.1", RestPort, "GET", "/api/v1/quarantine", "Basic " + credentials, null);
            ClassicAssert.AreEqual(200, held.status, held.body);
            StringAssert.Contains("Bank details", held.body, "The quarantine holds it.");
            StringAssert.Contains("Rule: Hold bank details", held.body, "Under the rule's name.");
         }
         finally
         {
            RestListener.Stop();
         }
      }

      [Test]
      [Description("A message a rule held and a person released is not held again by the same rule and reaches the outside once, and its sender, told when it was held, is not told a second time")]
      public void AReleasedMessageIsNotHeldAgainAndTheSenderIsToldOnce()
      {
         _settings.SetAdministratorPassword(AdminPassword);
         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();
         string credentials = Convert.ToBase64String(Encoding.ASCII.GetBytes("Administrator:" + AdminPassword));
         try
         {
            Account sender = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "reviewed@example.test", "test");

            Rule rule = NewRule(_application.Rules, "Review bank details");
            ExternalAndSensitive(rule, "iban");
            AddAction(rule, WarnSender, value: "Held for review.");
            AddAction(rule, (int) eRuleActionType.eRAQuarantine);
            rule.Save();

            // Nothing listens here while the message is held; the simulator
            // takes the port only once the release is what is being watched.
            TestSetup.AddRoutePointingAtLocalhost(1, UnattendedRoutePort, false);

            SmtpClientSimulator.StaticSend(sender.Address, Outside, "Bank details for review", "Pay to GB82 WEST 1234 5698 7654 32 please.");
            CustomAsserts.AssertRecipientsInDeliveryQueue(0);

            // Held under the rule's name, and the sender told once.
            (int status, string body) held = RestRequest.Send("127.0.0.1", RestPort, "GET", "/api/v1/quarantine", "Basic " + credentials, null);
            ClassicAssert.AreEqual(200, held.status, held.body);
            Match entry = Regex.Match(held.body, "\\{\"id\":(\\d+),\"sender\":\"" + Regex.Escape(sender.Address) + "\",\"recipients\":\"" + Regex.Escape(Outside) + "\",\"subject\":\"Bank details for review\",\"reason\":\"Rule: Review bank details\"");
            Assert.IsTrue(entry.Success, "Held under the rule's name: " + held.body);
            Pop3ClientSimulator.AssertMessageCount(sender.Address, "test", 1);

            using (var server = new SmtpServerSimulator(1, UnattendedRoutePort))
            {
               server.AddRecipientResult(new Dictionary<string, int> { { Outside, 250 } });
               server.StartListen();

               // A person releases it: the deliverer runs the rule again, the
               // rule matches again, and the release mark is what stops the
               // hold and the second notice.
               (int status, string body) released = RestRequest.Send("127.0.0.1", RestPort, "POST", "/api/v1/quarantine/" + entry.Groups[1].Value + "/release", "Basic " + credentials, null);
               ClassicAssert.AreEqual(200, released.status, released.body);

               server.WaitForCompletion();
               StringAssert.Contains("GB82 WEST 1234 5698 7654 32", server.MessageData, "The released message reached the outside.");
            }

            CustomAsserts.AssertRecipientsInDeliveryQueue(0);
            (int status, string body) after = RestRequest.Send("127.0.0.1", RestPort, "GET", "/api/v1/quarantine", "Basic " + credentials, null);
            StringAssert.DoesNotContain("\"id\":" + entry.Groups[1].Value + ",", after.body, "The entry is gone, and no new one took its place: " + after.body);
            StringAssert.DoesNotContain("\"subject\":\"Bank details for review\"", after.body, "Not held a second time: " + after.body);
            Pop3ClientSimulator.AssertMessageCount(sender.Address, "test", 1);
         }
         finally
         {
            RestListener.Stop();
         }
      }

      [Test]
      [Description("A recipient that greylists - 451 on the first attempt, the common case - means one notice to the sender, not one per delivery attempt")]
      public void AGreylistedRecipientMeansOneNoticeNotTwo()
      {
         Account sender = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "greylisted@example.test", "test");

         Rule rule = NewRule(_application.Rules, "Card numbers by mail");
         ExternalAndSensitive(rule, "card_number");
         AddAction(rule, WarnSender, value: "Please use the payment portal for card details.");
         rule.Save();

         using (var server = new SmtpServerSimulator(2, NoticeSimulatorPort))
         {
            server.AddRecipientResult(new Dictionary<string, int> { { Outside, 451 } });
            server.AddRecipientResult(new Dictionary<string, int> { { Outside, 250 } });
            server.StartListen();
            TestSetup.AddRoutePointingAtLocalhost(2, NoticeSimulatorPort, false);

            new SmtpClientSimulator().Send(sender.Address, Outside, "Payment", "Card 5500 0000 0000 0004 please.");

            // The first attempt is answered 451 and the message is rescheduled;
            // forcing the queue is the retry, on the simulator's second
            // connection, which accepts it. The rules run on both passes.
            CustomAsserts.AssertRecipientsInDeliveryQueue(0);
            server.WaitForCompletion();
            StringAssert.Contains("5500 0000 0000 0004", server.MessageData, "The message left on the second attempt.");
         }

         string notice = Pop3ClientSimulator.AssertGetFirstMessageText(sender.Address, "test");
         StringAssert.Contains("Subject: Policy notice: Card numbers by mail", notice);
         Pop3ClientSimulator.AssertMessageCount(sender.Address, "test", 1);
      }

      [Test]
      [Description("A rule that blocks speaks to the webmail: a card number to an outside address sent through POST /api/v1/me/messages is refused with the rule's text before anything is queued, and the same text to a colleague goes")]
      public void ABlockedMessageFromTheWebmailIsRefusedWithTheRulesText()
      {
         _settings.SetAdministratorPassword(AdminPassword);
         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();
         try
         {
            Account sender = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "webmail@example.test", "test");
            Account colleague = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "colleague3@example.test", "test");

            Rule rule = NewRule(_application.Rules, "No card numbers leave");
            ExternalAndSensitive(rule, "card_number");
            AddAction(rule, (int) eRuleActionType.eRAReject, value: "Card numbers may not be sent outside the organisation.");
            rule.Save();

            // Nothing listens here: a message the route was handed would fail loudly.
            TestSetup.AddRoutePointingAtLocalhost(1, UnattendedRoutePort, false);

            string me = "Basic " + Convert.ToBase64String(Encoding.ASCII.GetBytes(sender.Address + ":test"));
            (int status, string body) refused = RestRequest.Send("127.0.0.1", RestPort, "POST", "/api/v1/me/messages", me,
               "{\"to\":\"" + Outside + "\",\"subject\":\"Payment\",\"text\":\"Please charge 4111 1111 1111 1111, expiry 12/28.\"}");
            ClassicAssert.AreEqual(403, refused.status, refused.body);
            StringAssert.Contains("Card numbers may not be sent outside the organisation.", refused.body, "In the rule's words.");
            StringAssert.Contains("\"code\":\"policy_rejected\"", refused.body);
            CustomAsserts.AssertRecipientsInDeliveryQueue(0, false);

            // No external recipient, so the same text between colleagues goes.
            (int status, string body) sent = RestRequest.Send("127.0.0.1", RestPort, "POST", "/api/v1/me/messages", me,
               "{\"to\":\"" + colleague.Address + "\",\"subject\":\"Payment\",\"text\":\"Please charge 4111 1111 1111 1111, expiry 12/28.\"}");
            ClassicAssert.AreEqual(201, sent.status, sent.body);
            Pop3ClientSimulator.AssertMessageCount(colleague.Address, "test", 1);
         }
         finally
         {
            RestListener.Stop();
         }
      }

      [Test]
      [Description("A reject that meets a message after acceptance - here through a marker an earlier rule writes, which the DATA-time check cannot see - drops it, and a sender this server hosts is told it was not sent")]
      public void ARejectMetAfterAcceptanceTellsAHostedSender()
      {
         Account sender = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "marked@example.test", "test");

         Rule marker = NewRule(_application.Rules, "Mark board papers");
         AddCriterion(marker, (int) eRulePredefinedField.eFTSubject, eRuleMatchType.eMTContains, "Board papers");
         AddAction(marker, (int) eRuleActionType.eRASetHeaderValue, value: "yes", header: "X-Board-Papers");
         marker.Save();

         Rule block = NewRule(_application.Rules, "Board papers stay inside");
         AddHeaderCriterion(block, "X-Board-Papers", eRuleMatchType.eMTContains, "yes");
         AddCriterion(block, ExternalRecipient, eRuleMatchType.eMTContains, "@");
         AddAction(block, (int) eRuleActionType.eRAReject, value: "Board papers are not sent outside.");
         block.Save();

         using (var server = new SmtpServerSimulator(1, OutsideSenderSimulatorPort))
         {
            server.AddRecipientResult(new Dictionary<string, int> { { Outside, 250 } });
            server.StartListen();
            TestSetup.AddRoutePointingAtLocalhost(1, OutsideSenderSimulatorPort, false);

            // Accepted at DATA, where the marker is not yet on the message;
            // dropped in the deliverer, where the marker rule has run first.
            new SmtpClientSimulator().Send(sender.Address, Outside, "Board papers for Thursday", "Attached.");
            CustomAsserts.AssertRecipientsInDeliveryQueue(0);

            ClassicAssert.AreEqual("", server.MessageData ?? "", "The message did not leave.");
         }

         string notice = Pop3ClientSimulator.AssertGetFirstMessageText(sender.Address, "test");
         StringAssert.Contains("Subject: Policy notice: Board papers stay inside", notice);
         StringAssert.Contains("X-hMailServer-Policy-Outcome: not-sent", notice);
         StringAssert.Contains("was not sent", notice);
         StringAssert.Contains("Board papers are not sent outside.", notice);
         Pop3ClientSimulator.AssertMessageCount(sender.Address, "test", 1);
      }

      [Test]
      [Description("An attachment is typed by its bytes: a program called report.txt is caught as an executable, and a real text file with the same name is not")]
      public void AnExecutableRenamedAsTextIsCaughtByItsBytes()
      {
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "typed@example.test", "test");

         Rule rule = NewRule(_application.Rules, "No programs");
         AddCriterion(rule, AttachmentType, eRuleMatchType.eMTContains, "executable");
         AddAction(rule, (int) eRuleActionType.eRADeleteEmail);
         rule.Save();

         // The first bytes of a Windows program: MZ, then the DOS header.
         byte[] program = { 0x4D, 0x5A, 0x90, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00 };
         string disguised = Convert.ToBase64String(program);
         string genuine = Convert.ToBase64String(Encoding.ASCII.GetBytes("Quarterly report: all is well.\r\n"));

         SmtpClientSimulator.StaticSendRaw("sender@dummy-example.com", account.Address, WithAttachment("sender@dummy-example.com", account.Address, "the program", "report.txt", "text/plain", disguised));
         SmtpClientSimulator.StaticSendRaw("sender@dummy-example.com", account.Address, WithAttachment("sender@dummy-example.com", account.Address, "the report", "report.txt", "text/plain", genuine));
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 1);
         StringAssert.Contains("Subject: the report", Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test"));
      }

      [Test]
      [Description("Warn the sender: the message leaves, and the sender is mailed a notice naming the rule, with the action's text, marked auto-generated")]
      public void TheSenderIsWarnedAndTheMessageStillGoes()
      {
         Account sender = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "warned@example.test", "test");

         Rule rule = NewRule(_application.Rules, "Card numbers by mail");
         ExternalAndSensitive(rule, "card_number");
         AddAction(rule, WarnSender, value: "Please use the payment portal for card details.");
         rule.Save();

         using (var server = new SmtpServerSimulator(1, NoticeSimulatorPort))
         {
            server.AddRecipientResult(new Dictionary<string, int> { { Outside, 250 } });
            server.StartListen();
            TestSetup.AddRoutePointingAtLocalhost(1, NoticeSimulatorPort, false);

            new SmtpClientSimulator().Send(sender.Address, Outside, "Payment", "Card 5500 0000 0000 0004 please.");
            server.WaitForCompletion();
            StringAssert.Contains("5500 0000 0000 0004", server.MessageData, "The message itself was not held.");
         }

         string notice = Pop3ClientSimulator.AssertGetFirstMessageText(sender.Address, "test");
         StringAssert.Contains("Subject: Policy notice: Card numbers by mail", notice);
         StringAssert.Contains("X-hMailServer-Policy: Card numbers by mail", notice);
         StringAssert.Contains("Auto-Submitted: auto-generated", notice);
         StringAssert.Contains("From: postmaster@" + _domain.Name, notice);
         StringAssert.Contains("Please use the payment portal for card details.", notice);
         StringAssert.Contains("matched the outbound content policy \"Card numbers by mail\"", notice);
      }

      [Test]
      [Description("No notice goes to a sender this server does not host: the message from outside is delivered and nothing is sent back")]
      public void AnOutsideSenderIsNotWarned()
      {
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "inbound@example.test", "test");

         Rule rule = NewRule(_application.Rules, "Any card number");
         AddCriterion(rule, SensitiveContent, eRuleMatchType.eMTContains, "card_number");
         AddAction(rule, WarnSender, value: "This would be backscatter.");
         rule.Save();

         using (var server = new SmtpServerSimulator(1, OutsideSenderSimulatorPort))
         {
            server.AddRecipientResult(new Dictionary<string, int> { { "sender@dummy-example.com", 250 } });
            server.StartListen();
            TestSetup.AddRoutePointingAtLocalhost(1, OutsideSenderSimulatorPort, false);

            SmtpClientSimulator.StaticSend("sender@dummy-example.com", account.Address, "Payment", "Card 4111 1111 1111 1111.");
            Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 1);
            CustomAsserts.AssertRecipientsInDeliveryQueue(0);

            // A notice would have gone to dummy-example.com, which is the simulator.
            ClassicAssert.AreEqual("", server.MessageData ?? "", "Nothing was sent back to the outside sender.");
         }
      }

      [Test]
      [Description("A message marked internal does not leave: the Sensitivity header with an external recipient is refused, the refusal names the rule, and the same message to a colleague is delivered")]
      public void AMessageMarkedInternalDoesNotLeave()
      {
         Account sender = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "internal@example.test", "test");
         Account colleague = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "colleague2@example.test", "test");

         Rule rule = NewRule(_application.Rules, "Internal only");
         AddHeaderCriterion(rule, "Sensitivity", eRuleMatchType.eMTContains, "Company-Confidential");
         AddCriterion(rule, ExternalRecipient, eRuleMatchType.eMTContains, "@");
         AddAction(rule, (int) eRuleActionType.eRAReject);
         rule.Save();

         TestSetup.AddRoutePointingAtLocalhost(1, UnattendedRoutePort, false);

         var refused = Assert.Throws<DeliveryFailedException>(() =>
            new SmtpClientSimulator().SendRaw(sender.Address, Outside, MarkedInternal(sender.Address, Outside)));
         StringAssert.Contains("550", refused.Message, refused.Message);
         StringAssert.Contains("Message refused by the rule \"Internal only\"", refused.Message, "The refusal names the rule when the action has no text of its own. Got: " + refused.Message);

         SmtpClientSimulator.StaticSendRaw(sender.Address, colleague.Address, MarkedInternal(sender.Address, colleague.Address));
         Pop3ClientSimulator.AssertMessageCount(colleague.Address, "test", 1);
      }

      [Test]
      [Description("The account-number detector is the dlp_account_number_pattern setting, refused unless it compiles; the content condition reads a text attachment the rule's own pattern is matched against")]
      public void TheAccountNumberPatternIsASettingAndContentReadsAttachments()
      {
         _settings.SetAdministratorPassword(AdminPassword);
         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();
         string credentials = Convert.ToBase64String(Encoding.ASCII.GetBytes("Administrator:" + AdminPassword));
         try
         {
            (int status, string body) bad = RestRequest.Send("127.0.0.1", RestPort, "PUT", "/api/v1/settings", "Basic " + credentials, "{\"dlp_account_number_pattern\":\"(\"}");
            ClassicAssert.AreEqual(400, bad.status, "A pattern that does not compile is refused: " + bad.body);

            (int status, string body) set = RestRequest.Send("127.0.0.1", RestPort, "PUT", "/api/v1/settings", "Basic " + credentials, "{\"dlp_account_number_pattern\":\"ACC-[0-9]{6}\"}");
            ClassicAssert.AreEqual(200, set.status, set.body);

            // One mailbox per message, so each is read once and nothing is deleted between them.
            Account first = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "accounts1@example.test", "test");
            Account second = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "accounts2@example.test", "test");
            Account third = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "accounts3@example.test", "test");

            Rule detector = NewRule(_application.Rules, "Account numbers");
            AddCriterion(detector, SensitiveContent, eRuleMatchType.eMTContains, "account_number");
            AddAction(detector, (int) eRuleActionType.eRASetHeaderValue, value: "account", header: "X-Policy-Found");
            detector.Save();

            Rule own = NewRule(_application.Rules, "Project code in a file");
            AddCriterion(own, Content, eRuleMatchType.eMTContains, "PRJ-2026");
            AddAction(own, (int) eRuleActionType.eRASetHeaderValue, value: "project", header: "X-Policy-Project");
            own.Save();

            SmtpClientSimulator.StaticSend("sender@dummy-example.com", first.Address, "one", "Please settle ACC-123456 today.");
            string found = Pop3ClientSimulator.AssertGetFirstMessageText(first.Address, "test");
            StringAssert.Contains("X-Policy-Found: account", found, "The setting's pattern is the detector.");
            StringAssert.DoesNotContain("X-Policy-Project", found);

            string csv = Convert.ToBase64String(Encoding.ASCII.GetBytes("code,amount\r\nPRJ-2026,100\r\n"));
            SmtpClientSimulator.StaticSendRaw("sender@dummy-example.com", second.Address, WithAttachment("sender@dummy-example.com", second.Address, "two", "costs.csv", "application/octet-stream", csv));
            string attached = Pop3ClientSimulator.AssertGetFirstMessageText(second.Address, "test");
            StringAssert.Contains("X-Policy-Project: project", attached, "The content condition read the attachment, whatever its declared type.");
            StringAssert.DoesNotContain("X-Policy-Found", attached, "No account number anywhere in it: the detector did not fire.");

            SmtpClientSimulator.StaticSend("sender@dummy-example.com", third.Address, "three", "Please settle ACC-12 today.");
            string tooShort = Pop3ClientSimulator.AssertGetFirstMessageText(third.Address, "test");
            StringAssert.DoesNotContain("X-Policy-Found", tooShort, "A number the pattern does not match is not an account number.");

            // Excel's "Unicode Text": UTF-16 with its byte-order mark, a NUL in
            // every other byte, read through the mark rather than called binary.
            Account fourth = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "accounts4@example.test", "test");
            byte[] mark = Encoding.Unicode.GetPreamble();
            byte[] units = Encoding.Unicode.GetBytes("code\tamount\r\nPRJ-2026\t100\r\n");
            byte[] unicodeText = new byte[mark.Length + units.Length];
            Buffer.BlockCopy(mark, 0, unicodeText, 0, mark.Length);
            Buffer.BlockCopy(units, 0, unicodeText, mark.Length, units.Length);
            SmtpClientSimulator.StaticSendRaw("sender@dummy-example.com", fourth.Address, WithAttachment("sender@dummy-example.com", fourth.Address, "four", "costs.txt", "application/octet-stream", Convert.ToBase64String(unicodeText)));
            string unicode = Pop3ClientSimulator.AssertGetFirstMessageText(fourth.Address, "test");
            StringAssert.Contains("X-Policy-Project: project", unicode, "The content condition read a UTF-16 attachment through its mark.");
         }
         finally
         {
            RestRequest.Send("127.0.0.1", RestPort, "PUT", "/api/v1/settings", "Basic " + credentials, "{\"dlp_account_number_pattern\":\"\"}");
            RestListener.Stop();
         }
      }

      private static string MarkedInternal(string from, string to)
      {
         return "From: " + from + "\r\n" +
                "To: " + to + "\r\n" +
                "Subject: Board minutes\r\n" +
                "Sensitivity: Company-Confidential\r\n" +
                "\r\n" +
                "Not for circulation.\r\n";
      }

      private static string WithAttachment(string from, string to, string subject, string fileName, string contentType, string base64)
      {
         return "From: " + from + "\r\n" +
                "To: " + to + "\r\n" +
                "Subject: " + subject + "\r\n" +
                "MIME-Version: 1.0\r\n" +
                "Content-Type: multipart/mixed; boundary=\"part\"\r\n" +
                "\r\n" +
                "--part\r\n" +
                "Content-Type: text/plain\r\n" +
                "\r\n" +
                "See the attachment " + fileName + ".\r\n" +
                "--part\r\n" +
                "Content-Type: " + contentType + "; name=\"" + fileName + "\"\r\n" +
                "Content-Disposition: attachment; filename=\"" + fileName + "\"\r\n" +
                "Content-Transfer-Encoding: base64\r\n" +
                "\r\n" +
                base64 + "\r\n" +
                "--part--\r\n";
      }
   }
}
