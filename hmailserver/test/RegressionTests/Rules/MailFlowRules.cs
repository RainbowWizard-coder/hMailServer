// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Text;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;
using hMailServer;

namespace RegressionTests.Rules
{
   /// <summary>
   ///    Mail flow rules: a domain's own rules, run for its mail after the
   ///    server's; a condition on the attachment name; and the four actions a
   ///    transport rule has that these did not - add a disclaimer, reject with a
   ///    text (before the message is accepted, where the sender is told), require
   ///    TLS for the delivery, and quarantine.
   /// </summary>
   [TestFixture]
   public class MailFlowRules : TestFixtureBase
   {
      private const string AdminPassword = "testar";
      private static int RestPort = 9598;

      private static Rule NewRule(hMailServer.Rules rules, string name, eRulePredefinedField field, eRuleMatchType match, string value)
      {
         Rule rule = rules.Add();
         rule.Name = name;
         rule.Active = true;
         RuleCriteria criterion = rule.Criterias.Add();
         criterion.UsePredefined = true;
         criterion.PredefinedField = field;
         criterion.MatchType = match;
         criterion.MatchValue = value;
         criterion.Save();
         return rule;
      }

      private static void AddAction(Rule rule, eRuleActionType type, string value = null, string body = null, string header = null)
      {
         RuleAction action = rule.Actions.Add();
         action.Type = type;
         if (value != null)
            action.Value = value;
         if (body != null)
            action.Body = body;
         if (header != null)
            action.HeaderName = header;
         action.Save();
      }

      [TearDown]
      public void ForgetTheRules()
      {
         hMailServer.Rules global = _application.Rules;
         while (global.Count > 0)
            global[0].Delete();
      }

      [Test]
      [Description("A domain's rules run for the domain's mail alone: a header set by a rule of the first domain is on a message to it and not on one to a second domain")]
      public void ADomainsRulesRunForItsMailAlone()
      {
         Domain second = SingletonProvider<TestSetup>.Instance.AddDomain("second.test");
         Account first = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "flow@example.test", "test");
         Account other = SingletonProvider<TestSetup>.Instance.AddAccount(second, "flow@second.test", "test");

         Rule rule = NewRule(_domain.Rules, "Domain header", eRulePredefinedField.eFTSubject, eRuleMatchType.eMTContains, "flow");
         AddAction(rule, eRuleActionType.eRASetHeaderValue, value: "yes", header: "X-Domain-Rule");
         rule.Save();

         ClassicAssert.AreEqual(1, _domain.Rules.Count, "The rule is the domain's.");
         ClassicAssert.AreEqual(0, _application.Rules.Count, "And not the server's.");
         ClassicAssert.AreEqual(0, second.Rules.Count, "And not the second domain's.");

         SmtpClientSimulator.StaticSend("sender@dummy-example.com", first.Address, "flow one", "Body.");
         SmtpClientSimulator.StaticSend("sender@dummy-example.com", other.Address, "flow two", "Body.");
         string mine = Pop3ClientSimulator.AssertGetFirstMessageText(first.Address, "test");
         string theirs = Pop3ClientSimulator.AssertGetFirstMessageText(other.Address, "test");

         StringAssert.Contains("X-Domain-Rule: yes", mine, "The domain's rule ran for its own mail.");
         StringAssert.DoesNotContain("X-Domain-Rule", theirs, "And not for another domain's.");

         rule.Delete();
         ClassicAssert.AreEqual(0, _domain.Rules.Count);
      }

      [Test]
      [Description("A recipient domain's delete acts on its own recipients: a message to the first domain alone is deleted by its rule, and the same message to both domains reaches the second domain's mailbox and not the first's")]
      public void ARecipientDomainsDeleteTakesOnlyItsOwnRecipientsOff()
      {
         Domain second = SingletonProvider<TestSetup>.Instance.AddDomain("second.test");
         Account first = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "scope@example.test", "test");
         Account other = SingletonProvider<TestSetup>.Instance.AddAccount(second, "scope@second.test", "test");

         Rule rule = NewRule(_domain.Rules, "Drop it here", eRulePredefinedField.eFTSubject, eRuleMatchType.eMTContains, "scoped");
         AddAction(rule, eRuleActionType.eRADeleteEmail);
         rule.Save();

         // The negative control: to the first domain alone, the rule deletes it.
         SmtpClientSimulator.StaticSend("sender@dummy-example.com", first.Address, "scoped alone", "Body.");
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         Pop3ClientSimulator.AssertMessageCount(first.Address, "test", 0);

         // To both: until 26 September 2026 the first domain's delete deleted the
         // message for the second domain's recipient as well.
         SmtpClientSimulator.StaticSend("sender@dummy-example.com", new List<string> { first.Address, other.Address }, "scoped both", "Body.");
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         Pop3ClientSimulator.AssertMessageCount(other.Address, "test", 1);
         Pop3ClientSimulator.AssertMessageCount(first.Address, "test", 0);
      }

      [Test]
      [Description("A domain's rules are not chosen by an envelope sender nobody vouched for: a message from outside claiming the first domain as its sender, to the second domain, does not get the first domain's header; to the first domain's own mailbox it does")]
      public void AnUnvouchedSenderDoesNotChooseItsDomainsRules()
      {
         Domain second = SingletonProvider<TestSetup>.Instance.AddDomain("second.test");
         Account first = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "claimed@example.test", "test");
         Account other = SingletonProvider<TestSetup>.Instance.AddAccount(second, "target@second.test", "test");

         Rule rule = NewRule(_domain.Rules, "Sender header", eRulePredefinedField.eFTSubject, eRuleMatchType.eMTContains, "claimed");
         AddAction(rule, eRuleActionType.eRASetHeaderValue, value: "yes", header: "X-First-Domain-Rule");
         rule.Save();

         // The negative control: to the first domain's own mailbox its rule runs.
         SmtpClientSimulator.StaticSend(first.Address, first.Address, "claimed one", "Body.");
         StringAssert.Contains("X-First-Domain-Rule: yes", Pop3ClientSimulator.AssertGetFirstMessageText(first.Address, "test"));

         // Until 26 September 2026 the unauthenticated MAIL FROM chose the first
         // domain's rules for the second domain's mail.
         SmtpClientSimulator.StaticSend(first.Address, other.Address, "claimed two", "Body.");
         StringAssert.DoesNotContain("X-First-Domain-Rule", Pop3ClientSimulator.AssertGetFirstMessageText(other.Address, "test"),
            "An envelope sender nobody vouched for chose its domain's rules for another domain's mail.");
      }

      [Test]
      [Description("A recipient domain's reject on a message that also goes to another domain takes its own recipients off and tells a hosted sender so: the message is accepted, the other domain's recipient gets it, the rejecting domain's does not, and the sender is mailed a notice naming that recipient alone")]
      public void ARecipientDomainsRejectTellsAHostedSenderWhoWasLeftOut()
      {
         Domain second = SingletonProvider<TestSetup>.Instance.AddDomain("second.test");
         Account refused = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "refusing@example.test", "test");
         Account colleague = SingletonProvider<TestSetup>.Instance.AddAccount(second, "colleague@second.test", "test");
         Account writer = SingletonProvider<TestSetup>.Instance.AddAccount(second, "writer@second.test", "test");

         Rule rule = NewRule(_domain.Rules, "No quotes here", eRulePredefinedField.eFTSubject, eRuleMatchType.eMTContains, "quote");
         AddAction(rule, eRuleActionType.eRAReject, value: "Quotes go to sales@example.test.");
         rule.Save();

         // The message is accepted: the SMTP session has one answer for every
         // recipient, and the rule is example.test's say over its own alone.
         SmtpClientSimulator.StaticSend(writer.Address, new List<string> { refused.Address, colleague.Address }, "Your quote", "Attached.");
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         Pop3ClientSimulator.AssertMessageCount(colleague.Address, "test", 1);
         Pop3ClientSimulator.AssertMessageCount(refused.Address, "test", 0);

         // The hosted sender is told who did not get it. Until the scoped
         // reject told nobody, the recipient was dropped without a word.
         string notice = Decoded_(Pop3ClientSimulator.AssertGetFirstMessageText(writer.Address, "test"));
         StringAssert.Contains("Subject: Policy notice: No quotes here", notice);
         StringAssert.Contains("was not delivered to " + refused.Address + ":", notice);
         StringAssert.DoesNotContain(colleague.Address, notice, "The notice named a recipient the rule did not refuse.");
         StringAssert.Contains("Quotes go to sales@example.test.", notice);
         StringAssert.Contains("its other recipients are not affected", notice);
      }

      /// <summary>A notice as a reader sees it: quoted-printable soft breaks and escapes undone.</summary>
      private static string Decoded_(string raw)
      {
         string joined = raw.Replace("=\r\n", "").Replace("=\n", "");
         return System.Text.RegularExpressions.Regex.Replace(joined, "=([0-9A-F]{2})",
            match => ((char) Convert.ToInt32(match.Groups[1].Value, 16)).ToString());
      }

      [Test]
      [Description("An attachment's file name is a condition: a rule on *.exe deletes the message that carries one and leaves the one that carries a text file")]
      public void AnAttachmentNameIsACondition()
      {
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "attached@example.test", "test");

         Rule rule = NewRule(_application.Rules, "No executables", eRulePredefinedField.eFTAttachmentName, eRuleMatchType.eMTWildcard, "*.exe");
         AddAction(rule, eRuleActionType.eRADeleteEmail);
         rule.Save();

         SmtpClientSimulator.StaticSendRaw("sender@dummy-example.com", account.Address, WithAttachment(account.Address, "setup.exe", "a program"));
         SmtpClientSimulator.StaticSendRaw("sender@dummy-example.com", account.Address, WithAttachment(account.Address, "notes.txt", "some notes"));
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 1);
         StringAssert.Contains("notes.txt", Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test"));
      }

      [Test]
      [Description("A rule adds a disclaimer: the text is appended to the message's body when the rule matches, and the message says which rule put it there")]
      public void ADisclaimerIsAddedByARule()
      {
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "disclaimed@example.test", "test");

         Rule rule = NewRule(_application.Rules, "Legal footer", eRulePredefinedField.eFTSubject, eRuleMatchType.eMTContains, "contract");
         AddAction(rule, eRuleActionType.eRAAddDisclaimer, body: "This message is confidential and may be privileged.");
         rule.Save();

         SmtpClientSimulator.StaticSend("sender@dummy-example.com", account.Address, "the contract", "Please find the draft below.");
         string message = Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");

         StringAssert.Contains("This message is confidential and may be privileged.", message);
         StringAssert.Contains("X-hMailServer-Disclaimer: rule Legal footer", message);
      }

      [Test]
      [Description("A rule rejects with a text: the SMTP session refuses the message with 550 and the rule's words before accepting it, and a message the rule does not match is delivered")]
      public void AMessageIsRejectedWithTheRulesText()
      {
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "rejected@example.test", "test");

         Rule rule = NewRule(_application.Rules, "No invoices here", eRulePredefinedField.eFTSubject, eRuleMatchType.eMTContains, "invoice");
         AddAction(rule, eRuleActionType.eRAReject, value: "Invoices go to accounts@example.test, not here.");
         rule.Save();

         var refused = Assert.Throws<DeliveryFailedException>(() =>
            new SmtpClientSimulator().Send("sender@dummy-example.com", account.Address, "Your invoice", "Attached."));
         StringAssert.Contains("550", refused.Message, "Refused, not accepted and dropped. Got: " + refused.Message);
         StringAssert.Contains("Invoices go to accounts@example.test, not here.", refused.Message, "In the rule's words. Got: " + refused.Message);

         SmtpClientSimulator.StaticSend("sender@dummy-example.com", account.Address, "Lunch", "Tomorrow?");
         Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 1);
      }

      [Test]
      [Description("A rule quarantines: the message is held in the quarantine under the rule's name rather than delivered")]
      public void AMessageIsQuarantinedByARule()
      {
         _settings.SetAdministratorPassword(AdminPassword);
         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();
         try
         {
            Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "held@example.test", "test");

            Rule rule = NewRule(_application.Rules, "Hold for review", eRulePredefinedField.eFTSubject, eRuleMatchType.eMTContains, "review me");
            AddAction(rule, eRuleActionType.eRAQuarantine);
            rule.Save();

            SmtpClientSimulator.StaticSend("sender@dummy-example.com", account.Address, "Please review me", "The body under review.");
            CustomAsserts.AssertRecipientsInDeliveryQueue(0);
            Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 0);

            string credentials = Convert.ToBase64String(Encoding.ASCII.GetBytes("Administrator:" + AdminPassword));
            (int status, string body) held = RestRequest.Send("127.0.0.1", RestPort, "GET", "/api/v1/quarantine", "Basic " + credentials, null);
            ClassicAssert.AreEqual(200, held.status, held.body);
            StringAssert.Contains("Please review me", held.body, "The quarantine holds it.");
            StringAssert.Contains("Rule: Hold for review", held.body, "Under the rule's name.");
         }
         finally
         {
            RestListener.Stop();
         }
      }

      [Test]
      [Description("A rule requires TLS: a delivery to a server that offers no STARTTLS is deferred naming the rule rather than sent in the clear")]
      public void ARuleRequiresTlsForTheDelivery()
      {
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "careful@example.test", "test");

         Rule rule = NewRule(_application.Rules, "Encrypt to partners", eRulePredefinedField.eFTSubject, eRuleMatchType.eMTContains, "secret");
         AddAction(rule, eRuleActionType.eRARequireTLS);
         rule.Save();

         int port = TestSetup.GetNextFreePort();
         using (var server = new SmtpServerSimulator(1, port))
         {
            var results = new System.Collections.Generic.Dictionary<string, int> { { "test@dummy-example.com", 250 } };
            server.AddRecipientResult(results);
            server.StartListen();
            TestSetup.AddRoutePointingAtLocalhost(1, port, true, eConnectionSecurity.eCSNone);

            LogHandler.DeleteCurrentDefaultLog();
            new SmtpClientSimulator().Send(account.Address, "test@dummy-example.com", "a secret", "Not in the clear.");
            CustomAsserts.AssertRecipientsInDeliveryQueue(0);

            // The route tries once, so the refusal comes back to the sender at once,
            // naming the rule - which is where the reason is written, not the log.
            // The report's text part is quoted-printable, wrapped with soft breaks.
            string bounce = Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test").Replace("=\r\n", "").Replace("=20", " ");
            StringAssert.Contains("the rule Encrypt to partners requires TLS", bounce);

            ClassicAssert.IsFalse((server.MessageData ?? "").Contains("Not in the clear."), "The message was not handed over without TLS.");
         }
      }

      private static string WithAttachment(string to, string fileName, string subject)
      {
         return "From: sender@dummy-example.com\r\n" +
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
                "Content-Type: application/octet-stream; name=\"" + fileName + "\"\r\n" +
                "Content-Disposition: attachment; filename=\"" + fileName + "\"\r\n" +
                "Content-Transfer-Encoding: base64\r\n" +
                "\r\n" +
                "SGVsbG8=\r\n" +
                "--part--\r\n";
      }
   }
}
