// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
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
            string bounce = Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");
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
