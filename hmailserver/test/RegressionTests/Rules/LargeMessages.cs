// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
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
   ///    committed. The SMTP session takes no message over 20 MB by default, so
   ///    the fixture raises the limit; PerformBasicSetup puts it back.
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

      // The header of the one message of an account's folder, after asserting that
      // the generated body follows it byte for byte.
      private static string AssertStoredBodyWhole(Account account, string folder = "Inbox")
      {
         return AssertStoredBodiesWhole(account, 1, folder)[0];
      }

      // The headers of the messages an account's folder holds once it holds count
      // of them, each read from its file - a byte for each character, as the
      // message is ASCII - after asserting that the generated body follows it byte
      // for byte. Waited for longer than CustomAsserts waits for a message: each
      // here is 81 MB, written on the disk once for every recipient and copy.
      private static List<string> AssertStoredBodiesWhole(Account account, int count, string folder = "Inbox")
      {
         IMAPFolder stored = account.IMAPFolders.get_ItemByName(folder);

         DateTime until = DateTime.UtcNow.AddSeconds(120);
         while (stored.Messages.Count < count && DateTime.UtcNow < until)
            System.Threading.Thread.Sleep(250);

         ClassicAssert.AreEqual(count, stored.Messages.Count, "The number of messages in " + account.Address + "'s " + folder + ".");

         var headers = new List<string>();
         for (int i = 0; i < count; i++)
         {
            string text = Encoding.GetEncoding(28591).GetString(File.ReadAllBytes(stored.Messages[i].Filename));
            headers.Add(AssertBodyWhole(text, "Message " + (i + 1) + " in " + account.Address + "'s " + folder));
         }
         return headers;
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

      [Test]
      [Description("A Sieve redirect of a message over 80 MB raises its rule-loop count and carries its whole body. Until the large-message row the redirected copy's count could not be written (HM6010).")]
      public void ASieveRedirectOfALargeMessageCarriesItsLoopCountAndItsWholeBody()
      {
         Account source = AddAccount("large-sieve@example.test");
         Account target = AddAccount("large-sieve-target@example.test");

         // Late-bound, as the Sieve fixtures set a script (Sieve/SieveDelivery.cs).
         source.GetType().InvokeMember("SieveScript", BindingFlags.SetProperty, null, source,
            new object[] { "redirect \"" + target.Address + "\";\r\n" });

         SmtpClientSimulator.StaticSendRaw(Correspondent, source.Address, LargeMessage(Correspondent, source.Address, "large-sieve-redirect"));

         string redirected = AssertStoredBodyWhole(target);
         StringAssert.Contains("X-hMailServer-LoopCount: 1\r\n", redirected, "The redirected copy of the large message does not carry its loop count.");
         CustomAsserts.AssertNoReportedError();
      }

      [Test]
      [Description("An account's rule copies a message over 80 MB: the copy carries X-CopyRule and its rule-loop count raised, the original neither, and both their whole body. Until the large-message row no rule ran on such a message (HM6040), so no copy was made.")]
      public void ARuleCopiesALargeMessageWithItsLoopCountAndItsWholeBody()
      {
         Account account = AddAccount("large-copier@example.test");

         // The size, and a loop count under 1, so that the copy - which carries 1 -
         // is not copied again (as Rules.cs's copy tests do it).
         Rule rule = NewRule(account.Rules, "Copy large", eRulePredefinedField.eFTMessageSize, eRuleMatchType.eMTGreaterThan, "0");
         rule.UseAND = true;
         RuleCriteria once = rule.Criterias.Add();
         once.UsePredefined = false;
         once.HeaderField = "X-hMailServer-LoopCount";
         once.MatchType = eRuleMatchType.eMTLessThan;
         once.MatchValue = "1";
         once.Save();
         AddAction(rule, eRuleActionType.eRACreateCopy);
         rule.Save();

         SmtpClientSimulator.StaticSendRaw(Correspondent, account.Address, LargeMessage(Correspondent, account.Address, "large-rule-copy"));

         List<string> headers = AssertStoredBodiesWhole(account, 2);
         string copy = headers.Find(header => header.Contains("X-CopyRule: Copy large\r\n"));
         string original = headers.Find(header => !header.Contains("X-CopyRule"));
         ClassicAssert.IsNotNull(copy, "Neither message is the rule's copy: " + string.Join(" | ", headers));
         ClassicAssert.IsNotNull(original, "Neither message is the original: " + string.Join(" | ", headers));
         StringAssert.Contains("X-hMailServer-LoopCount: 1\r\n", copy, "The rule's copy of the large message does not carry its loop count.");
         StringAssert.DoesNotContain("X-hMailServer-LoopCount", original, "The original was given a loop count.");
         CustomAsserts.AssertNoReportedError();
      }

      [Test]
      [Description("A rule that rejects refuses a message over 80 MB at the end of DATA, with 550 and the rule's words, on the header it matched; nothing is delivered. Until the large-message row the SMTP session could not read the header, accepted the message, and no rule ran on it at delivery (HM6040).")]
      public void ARejectRuleRefusesALargeMessageAtTheEndOfData()
      {
         Account account = AddAccount("large-rejected@example.test");

         Rule rule = NewRule(_application.Rules, "No large invoices", eRulePredefinedField.eFTSubject, eRuleMatchType.eMTContains, "large-invoice");
         AddAction(rule, eRuleActionType.eRAReject, value: "Large invoices go to accounts@example.test.");
         rule.Save();

         var refused = Assert.Throws<DeliveryFailedException>(() =>
            SmtpClientSimulator.StaticSendRaw(Correspondent, account.Address, LargeMessage(Correspondent, account.Address, "large-invoice")));
         StringAssert.Contains("550", refused.Message, "Refused, not accepted. Got: " + refused.Message);
         StringAssert.Contains("Large invoices go to accounts@example.test.", refused.Message, "In the rule's words. Got: " + refused.Message);

         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         Pop3ClientSimulator.AssertMessageCount(account.Address, Password, 0);
         CustomAsserts.AssertNoReportedError();
      }

      [Test]
      [Description("A rule's script function is not run on a message over 80 MB - a script reads the message through the COM API's Message, which does not read one of that size, so its Subject and its other fields would read as empty and its Save would be refused (HM6010) - and the log says so; the rule's other action is taken. The same function runs on a message under the limit. Until the large-message row no rule ran on such a message at all (HM6040).")]
      public void ARulesScriptFunctionIsNotRunOnALargeMessageAndTheLogSaysWhy()
      {
         Account small = AddAccount("small-scripted@example.test");
         Account large = AddAccount("large-scripted@example.test");

         _settings.Scripting.Enabled = true;
         File.WriteAllText(_settings.Scripting.CurrentScriptFile,
            "Sub MarkLargeMessage(message)" + Environment.NewLine +
            " Call EventLog.Write(\"MarkLargeMessage ran on [\" & message.Subject & \"]\")" + Environment.NewLine +
            " message.HeaderValue(\"X-Large-Script\") = \"ran\"" + Environment.NewLine +
            " message.Save" + Environment.NewLine +
            "End Sub");
         _settings.Scripting.Reload();

         Rule rule = NewRule(_application.Rules, "Large script", eRulePredefinedField.eFTSubject, eRuleMatchType.eMTContains, "large-script-rule");
         RuleAction function = rule.Actions.Add();
         function.Type = eRuleActionType.eRARunScriptFunction;
         function.ScriptFunction = "MarkLargeMessage";
         function.Save();
         AddAction(rule, eRuleActionType.eRASetHeaderValue, value: "taken", header: "X-Large-Rule-Action");
         rule.Save();

         // The control: under the limit the function runs and its change is saved.
         LogHandler.DeleteEventLog();
         SmtpClientSimulator.StaticSend(Correspondent, small.Address, "small large-script-rule", "A message the function can read.");
         string smallMessage = Pop3ClientSimulator.AssertGetFirstMessageText(small.Address, Password);
         StringAssert.Contains("X-Large-Script: ran", smallMessage, "The function did not run on a message under the limit, so its absence below would prove nothing.");
         StringAssert.Contains("MarkLargeMessage ran on [small large-script-rule]", LogHandler.ReadEventLog());

         LogHandler.DeleteEventLog();
         SmtpClientSimulator.StaticSendRaw(Correspondent, large.Address, LargeMessage(Correspondent, large.Address, "large-script-rule"));

         string header = AssertStoredBodyWhole(large);
         StringAssert.Contains("X-Large-Rule-Action: taken", header, "The rule's other action was not taken on the large message.");
         StringAssert.DoesNotContain("X-Large-Script", header, "The script function ran on the large message.");
         ClassicAssert.IsTrue(LogHandler.DefaultLogContains("so the script function MarkLargeMessage of rule \"Large script\" was not run"),
            "The log does not say why the script function was not run: " + LogHandler.ReadCurrentDefaultLog());
         StringAssert.DoesNotContain("MarkLargeMessage ran", LogHandler.ReadEventLog(), "The script function ran on the large message.");
         CustomAsserts.AssertNoReportedError();
      }

      [Test]
      [Description("A message over 80 MB that the SMTP session marks as spam carries the spam headers and the subject's tag, written with its header alone and the body copied behind it byte for byte, and an account with spam filtering off gets its copy without them; its sender's domain's signature is not appended, and the log says so. Until the large-message row the headers' write was refused (HM6010), the signature was left off without a word, and the account's copy could not be rid of them (HM6351).")]
      public void ALargeMessageMarkedAsSpamCarriesTheSpamHeaders()
      {
         hMailServer.AntiSpam antiSpam = _settings.AntiSpam;
         bool addHeaderSpam = antiSpam.AddHeaderSpam;
         bool addHeaderReason = antiSpam.AddHeaderReason;
         bool prependSubject = antiSpam.PrependSubject;
         string prependSubjectText = antiSpam.PrependSubjectText;

         try
         {
            _domain.SignatureEnabled = true;
            _domain.SignaturePlainText = "The domain's signature.";
            _domain.AddSignaturesToLocalMail = true;
            _domain.Save();

            Account sender = AddAccount("large-spam-sender@example.test");
            Account reader = AddAccount("large-spam-reader@example.test");
            Account optedOut = AddAccount("large-spam-optout@example.test");
            optedOut.AntiSpamEnabled = false;
            optedOut.Save();

            // Marked, not refused, by a test the session runs before the message:
            // example.test has no MX in the suite's zone (SuiteDns).
            antiSpam.SpamMarkThreshold = 5;
            antiSpam.SpamDeleteThreshold = 100;
            antiSpam.UseMXChecks = true;
            antiSpam.UseMXChecksScore = 5;
            antiSpam.AddHeaderSpam = true;
            antiSpam.AddHeaderReason = true;
            antiSpam.PrependSubject = true;
            antiSpam.PrependSubjectText = "[SPAM]";

            // Not signed in, from an address that is an account here: the session
            // tests it for spam, and the sender's domain signs it.
            new SmtpClientSimulator().SendRaw(sender.Address, new List<string> { reader.Address, optedOut.Address },
               LargeMessage(sender.Address, reader.Address, "large-spam"));

            string marked = AssertStoredBodyWhole(reader);
            StringAssert.Contains("X-hMailServer-Spam: YES\r\n", marked, "The large message marked as spam does not carry the spam header.");
            StringAssert.Contains("Subject: [SPAM] large-spam\r\n", marked, "The large message marked as spam does not carry the subject's tag.");
            StringAssert.Contains("X-hMailServer-Reason-Score: 5\r\n", marked, "The large message marked as spam does not carry its score.");

            string unmarked = AssertStoredBodyWhole(optedOut);
            StringAssert.DoesNotContain("X-hMailServer-Spam", unmarked, "The copy of an account with spam filtering off kept the spam header.");
            StringAssert.DoesNotContain("X-hMailServer-Reason", unmarked, "The copy of an account with spam filtering off kept the reasons.");
            StringAssert.Contains("Subject: large-spam\r\n", unmarked, "The copy of an account with spam filtering off kept the subject's tag.");

            ClassicAssert.IsTrue(LogHandler.DefaultLogContains("so the signature was not appended"),
               "The log does not say why the signature was not appended: " + LogHandler.ReadCurrentDefaultLog());
            CustomAsserts.AssertNoReportedError();
         }
         finally
         {
            antiSpam.AddHeaderSpam = addHeaderSpam;
            antiSpam.AddHeaderReason = addHeaderReason;
            antiSpam.PrependSubject = prependSubject;
            antiSpam.PrependSubjectText = prependSubjectText;
         }
      }
   }
}
