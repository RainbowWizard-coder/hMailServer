// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.Rules
{
   /// <summary>
   ///    A message larger than the 80 MB the server parses a message whole at
   ///    (MessageData::MaxParseBytes; upstream hMailServer #683 and #686).
   ///
   ///    Until the large-message row such a message's load failed on every path,
   ///    and whatever needed its header went without: no rule ran on it at all
   ///    (HM6040); a forward neither read nor raised its rule-loop count, so two
   ///    accounts forwarding to each other passed it back and forth for ever with
   ///    an HM6010 at every hop; a bounce read neither the count nor
   ///    Auto-Submitted; and the signature and the disclaimers were left off
   ///    without a word. Those paths now read the header alone and write it back
   ///    with the body copied behind it byte for byte. A rule's criterion that
   ///    needs the body declines, and the signature and the disclaimers are not
   ///    appended, each with a line in the log that says why
   ///    (hmailserver/docs/MailFlowRules.md, DomainTransforms.md).
   ///
   ///    Every message here is generated, just over the limit, and none is
   ///    committed. The server takes no message over 20 MB by default, so the
   ///    fixture raises the limit; PerformBasicSetup puts it back.
   /// </summary>
   [TestFixture]
   public class LargeMessages : TestFixtureBase
   {
      private const string Password = "test";

      // Pre-allocated to this fixture (47140-47159): the listeners the outbound
      // tests' routes point at.
      private const int DisclaimerCapturePort = 47140;
      private const int BounceCapturePort = 47141;

      private const string Correspondent = "correspondent@example.com";
      private const string LastLine = "The last line of a message too large to be read whole.";

      private static readonly Lazy<string> Body = new Lazy<string>(MakeBody);

      // Just over the 80 MB a message is parsed whole at, in numbered lines no
      // client dot-stuffs, ending in a line no other line is: a body cut short,
      // or with a block out of place, compares unequal.
      private static string MakeBody()
      {
         const int length = 81 * 1024 * 1024;
         var body = new StringBuilder(length + 256);
         for (int line = 0; body.Length < length; line++)
            body.Append("Line ").Append(line.ToString("D8")).Append(" of a message too large to be read whole, which nothing reads.\r\n");
         body.Append(LastLine).Append("\r\n");
         return body.ToString();
      }

      private static string LargeMessage(string from, string to, string subject, string extraHeaders = "")
      {
         return "From: " + from + "\r\n" +
                "To: " + to + "\r\n" +
                "Subject: " + subject + "\r\n" +
                extraHeaders +
                "MIME-Version: 1.0\r\n" +
                "Content-Type: text/plain; charset=us-ascii\r\n" +
                "\r\n" +
                Body.Value;
      }

      // The header of a message as stored or received, after asserting that what
      // follows it is the generated body byte for byte. Compared without printing
      // either, which would put 81 MB in the test's output.
      private static string AssertBodyWhole(string message, string what)
      {
         int end = message.IndexOf("\r\n\r\n", StringComparison.Ordinal);
         ClassicAssert.Greater(end, 0, what + " has no end of its header.");

         string header = message.Substring(0, end + 4);
         int bodyLength = message.Length - (end + 4);
         ClassicAssert.AreEqual(Body.Value.Length, bodyLength, what + ": the body is not the length it was sent at. Header: " + header);
         ClassicAssert.IsTrue(string.CompareOrdinal(message, end + 4, Body.Value, 0, Body.Value.Length) == 0,
            what + ": the body is not the one sent, byte for byte. Header: " + header);
         return header;
      }

      // The first message of an account's folder, read from its file - a byte for
      // each character, as the message is ASCII - whole.
      private static string AssertStoredBodyWhole(Account account, string folder = "Inbox")
      {
         Message stored = CustomAsserts.AssertGetFirstMessage(account, folder);
         string text = Encoding.GetEncoding(28591).GetString(File.ReadAllBytes(stored.Filename));
         return AssertBodyWhole(text, "The message in " + account.Address + "'s " + folder);
      }

      private Account AddAccount(string address)
      {
         return SingletonProvider<TestSetup>.Instance.AddAccount(_domain, address, Password);
      }

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

      // One message sent outwards by a signed-in account, to a domain a route
      // points at a listener of this test's own that refuses its RCPT TO (550), so
      // that the message fails at once and its sender is sent a report - or, marked
      // Auto-Submitted, is not. No message of 81 MB is sent across the listener,
      // which takes what it is sent a line at a time and would not finish.
      private void SendOutwardsToARefusal(Account sender, int port, string domainName, string recipient, string message)
      {
         using (var capture = new SmtpServerSimulator(1, port))
         {
            capture.AddRecipientResult(new Dictionary<string, int> { { recipient, 550 } });
            capture.StartListen();

            var route = _settings.Routes.Add();

            try
            {
               route.DomainName = domainName;
               route.TargetSMTPHost = "localhost";
               route.TargetSMTPPort = port;
               route.NumberOfTries = 1;
               route.MinutesBetweenTry = 5;
               route.Save();

               new SmtpClientSimulator().SendRaw(sender.Address, Password, sender.Address, recipient, message);
               _application.SubmitEMail();

               capture.WaitForCompletion();
            }
            finally
            {
               if (route.ID > 0)
                  _settings.Routes.DeleteByDBID(route.ID);
            }
         }
      }

      [SetUp]
      public void AllowLargeMessages()
      {
         // 100 MB, in KB: the default of 20 MB refuses every message here.
         _settings.MaxMessageSize = 100 * 1024;
      }

      [TearDown]
      public void ForgetTheRules()
      {
         hMailServer.Rules global = _application.Rules;
         while (global.Count > 0)
            global[0].Delete();
      }

      [Test]
      [Description("A rule's header criterion and its set-header action apply to a message over 80 MB: the header is set and the body is the one sent, byte for byte. Until the large-message row no rule ran on such a message at all (HM6040).")]
      public void AHeaderRuleSetsAHeaderOnALargeMessage()
      {
         Account account = AddAccount("large-rule@example.test");

         Rule rule = NewRule(_application.Rules, "Large header", eRulePredefinedField.eFTSubject, eRuleMatchType.eMTContains, "large-header-rule");
         AddAction(rule, eRuleActionType.eRASetHeaderValue, value: "applied", header: "X-Large-Rule");
         rule.Save();

         SmtpClientSimulator.StaticSendRaw(Correspondent, account.Address, LargeMessage(Correspondent, account.Address, "large-header-rule"));

         string header = AssertStoredBodyWhole(account);
         StringAssert.Contains("X-Large-Rule: applied", header, "The rule's header was not set on the large message.");
         CustomAsserts.AssertNoReportedError();
      }

      [Test]
      [Description("A body criterion does not match a message over 80 MB, whose body is not read, whichever way round it is phrased - 'does not contain' would otherwise match every such message - and the log says so: the rule that would delete a body without the word delivers it.")]
      public void ABodyCriterionDoesNotMatchALargeMessageAndTheLogSaysWhy()
      {
         Account account = AddAccount("large-body@example.test");

         Rule rule = NewRule(_application.Rules, "Large body", eRulePredefinedField.eFTBody, eRuleMatchType.eMTNotContains, "approved");
         AddAction(rule, eRuleActionType.eRADeleteEmail);
         rule.Save();

         SmtpClientSimulator.StaticSendRaw(Correspondent, account.Address, LargeMessage(Correspondent, account.Address, "large-body-rule"));

         AssertStoredBodyWhole(account);
         ClassicAssert.IsTrue(LogHandler.DefaultLogContains("the criterion of rule \"Large body\" that needs its body or its attachments could not be evaluated and did not match"),
            "The log does not say why the body criterion declined: " + LogHandler.ReadCurrentDefaultLog());
         CustomAsserts.AssertNoReportedError();
      }

      [Test]
      [Description("A forward of a message over 80 MB raises its rule-loop count and carries its whole body. Until the large-message row the forwarded copy's count could not be written (HM6010), so a forwarding loop of such a message never ended.")]
      public void AForwardOfALargeMessageCarriesItsLoopCountAndItsWholeBody()
      {
         Account forwarder = AddAccount("large-forwarder@example.test");
         Account target = AddAccount("large-target@example.test");
         forwarder.ForwardEnabled = true;
         forwarder.ForwardAddress = target.Address;
         forwarder.ForwardKeepOriginal = true;
         forwarder.Save();

         SmtpClientSimulator.StaticSendRaw(Correspondent, forwarder.Address, LargeMessage(Correspondent, forwarder.Address, "large-forward"));

         string forwarded = AssertStoredBodyWhole(target);
         StringAssert.Contains("X-hMailServer-LoopCount: 1\r\n", forwarded, "The forwarded copy of the large message does not carry its loop count.");
         AssertStoredBodyWhole(forwarder);
         CustomAsserts.AssertNoReportedError();
      }

      [Test]
      [Description("An account's rule forwards a message over 80 MB on its size, with its loop count raised and its whole body. Until the large-message row no rule ran on it (HM6040), so it was not forwarded at all.")]
      public void ARuleForwardsALargeMessageWithItsLoopCountAndItsWholeBody()
      {
         Account account = AddAccount("large-rule-forwarder@example.test");
         Account target = AddAccount("large-rule-target@example.test");

         Rule rule = NewRule(account.Rules, "Forward large", eRulePredefinedField.eFTMessageSize, eRuleMatchType.eMTGreaterThan, "0");
         RuleAction forward = rule.Actions.Add();
         forward.Type = eRuleActionType.eRAForwardEmail;
         forward.To = target.Address;
         forward.Save();
         rule.Save();

         SmtpClientSimulator.StaticSendRaw(Correspondent, account.Address, LargeMessage(Correspondent, account.Address, "large-rule-forward"));

         string forwarded = AssertStoredBodyWhole(target);
         StringAssert.Contains("X-hMailServer-LoopCount: 1\r\n", forwarded, "The rule's forwarded copy of the large message does not carry its loop count.");
         AssertStoredBodyWhole(account);
         CustomAsserts.AssertNoReportedError();
      }

      [Test]
      [Description("A message over 80 MB whose rule-loop count has reached the limit is not forwarded (HM4333). Until the large-message row its count was not read, and it was forwarded at any count.")]
      public void ALargeMessageAtTheLoopLimitIsNotForwarded()
      {
         Account forwarder = AddAccount("large-looping@example.test");
         Account target = AddAccount("large-loop-target@example.test");
         forwarder.ForwardEnabled = true;
         forwarder.ForwardAddress = target.Address;
         forwarder.ForwardKeepOriginal = true;
         forwarder.Save();

         string atTheLimit = "X-hMailServer-LoopCount: " + _settings.RuleLoopLimit + "\r\n";
         SmtpClientSimulator.StaticSendRaw(Correspondent, forwarder.Address, LargeMessage(Correspondent, forwarder.Address, "large-loop", atTheLimit));

         AssertStoredBodyWhole(forwarder);
         CustomAsserts.AssertReportedError("HM4333", "Maximum forward loop count reached");
         Pop3ClientSimulator.AssertMessageCount(target.Address, Password, 0);
      }

      [Test]
      [Description("A rule's disclaimer is not appended to a message over 80 MB, whose text part is not read, and the log says so; the message is delivered as it was sent. Until the large-message row no rule ran on it (HM6040), and nothing said why it had no footer.")]
      public void ARulesDisclaimerIsNotAppendedToALargeMessageAndTheLogSaysWhy()
      {
         Account account = AddAccount("large-footer@example.test");

         Rule rule = NewRule(_application.Rules, "Large footer", eRulePredefinedField.eFTSubject, eRuleMatchType.eMTContains, "large-footer-rule");
         AddAction(rule, eRuleActionType.eRAAddDisclaimer, body: "This message is confidential and may be privileged.");
         rule.Save();

         SmtpClientSimulator.StaticSendRaw(Correspondent, account.Address, LargeMessage(Correspondent, account.Address, "large-footer-rule"));

         string header = AssertStoredBodyWhole(account);
         StringAssert.DoesNotContain("X-hMailServer-Disclaimer", header, "A large message was stamped as footed.");
         ClassicAssert.IsTrue(LogHandler.DefaultLogContains("so the disclaimer of rule Large footer was not appended"),
            "The log does not say why the rule's disclaimer was not appended: " + LogHandler.ReadCurrentDefaultLog());
         CustomAsserts.AssertNoReportedError();
      }

      [Test]
      [Description("The domain's disclaimer is not appended to a message over 80 MB leaving the organisation, and the log says so; the header it left with, which the report of its refusal carries, has no footer's stamp. Until the large-message row it was left off without a word.")]
      public void TheDomainsDisclaimerIsNotAppendedToALargeMessageAndTheLogSaysWhy()
      {
         Account sender = AddAccount("large-sender@example.test");

         _domain.DisclaimerEnabled = true;
         _domain.DisclaimerPlainText = "Confidential: this note may be privileged.";
         _domain.DisclaimerHTML = "<p>Confidential in HTML: may be privileged.</p>";
         _domain.Save();

         // The partner's listener refuses the recipient. The footer is decided when
         // the message is submitted, before any delivery, and 81 MB is not sent to a
         // test listener, which takes what it is sent a line at a time and would not
         // finish; the report of the refusal carries the header the message left with.
         const string partner = "partner@outside-large.test";
         SendOutwardsToARefusal(sender, DisclaimerCapturePort, "outside-large.test", partner,
            LargeMessage(sender.Address, partner, "large-domain-disclaimer"));

         ClassicAssert.IsTrue(LogHandler.DefaultLogContains("so the domain's disclaimer was not appended"),
            "The log does not say why the domain's disclaimer was not appended: " + LogHandler.ReadCurrentDefaultLog());

         string report = Pop3ClientSimulator.AssertGetFirstMessageText(sender.Address, Password);
         StringAssert.Contains("Subject: large-domain-disclaimer", report, "The report of the refusal does not carry the large message's header.");
         StringAssert.DoesNotContain("X-hMailServer-Disclaimer", report, "A large message was stamped as footed.");
         CustomAsserts.AssertNoReportedError();
      }

      [Test]
      [Description("The signature is not appended to a message over 80 MB, and the log says so; the message is delivered as it was sent. Until the large-message row it was left off without a word.")]
      public void TheSignatureIsNotAppendedToALargeMessageAndTheLogSaysWhy()
      {
         _domain.SignatureEnabled = true;
         _domain.SignaturePlainText = "The domain's signature.";
         _domain.AddSignaturesToLocalMail = true;
         _domain.Save();

         Account sender = AddAccount("large-signed@example.test");
         Account recipient = AddAccount("large-reader@example.test");

         new SmtpClientSimulator().SendRaw(sender.Address, Password, sender.Address, recipient.Address,
            LargeMessage(sender.Address, recipient.Address, "large-signature"));

         AssertStoredBodyWhole(recipient);
         ClassicAssert.IsTrue(LogHandler.DefaultLogContains("so the signature was not appended"),
            "The log does not say why the signature was not appended: " + LogHandler.ReadCurrentDefaultLog());
         CustomAsserts.AssertNoReportedError();
      }

      [Test]
      [Description("A message over 80 MB marked Auto-Submitted that cannot be delivered is not bounced (HM4404). Until the large-message row its header was not read, and its sender was sent a bounce of it.")]
      public void ALargeAutoSubmittedMessageThatFailsIsNotBounced()
      {
         Account sender = AddAccount("large-auto@example.test");

         const string nobody = "nobody@outside-bounce.test";
         SendOutwardsToARefusal(sender, BounceCapturePort, "outside-bounce.test", nobody,
            LargeMessage(sender.Address, nobody, "large-bounce", "Auto-Submitted: auto-replied\r\n"));

         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         CustomAsserts.AssertReportedError("HM4404", "Auto-Submitted header");
         Pop3ClientSimulator.AssertMessageCount(sender.Address, Password, 0);
      }
   }
}
