// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Collections.Generic;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.SMTP
{
   /// <summary>
   ///    The scope of the out-of-office reply: domain-wide, and split by sender.
   ///
   ///    Two things the per-account vacation message could not say. First, a whole
   ///    domain can now answer for accounts that have no vacation message of their
   ///    own (a closed office, a decommissioned department) - with the precedence
   ///    rule that an account's own message always wins, so one delivered message
   ///    produces at most ONE auto-reply. Second, the domain reply can say different
   ///    things to different senders: an internal sender (one whose envelope address
   ///    resolves to an account or active alias hosted on this server) can get the
   ///    richer text, everyone else gets the generic one.
   ///
   ///    "Internal" is an honest but weak claim - the envelope sender is whatever
   ///    the submitting client said, so the internal text must never carry secrets -
   ///    and the tests here assert the split, not any stronger guarantee.
   ///
   ///    The suppression rules that keep auto-responders off blacklists are asserted
   ///    against the DOMAIN reply because that is the new path, but they live in
   ///    SMTPVacationMessageCreator and therefore guard the account-level reply
   ///    identically: no reply to a null return path, none to bulk/list/junk
   ///    Precedence, and the reply itself carries Auto-Submitted: auto-replied over
   ///    a null envelope sender.
   /// </summary>
   [TestFixture]
   public class OutOfOfficeScope : TestFixtureBase
   {
      private const string DomainExternalBody = "Your message has been received by our office.";
      private const string DomainExternalSubject = "Message received";
      private const string DomainInternalBody = "The team is at the conference; ask Priya about anything urgent.";
      private const string DomainInternalSubject = "We are at the conference";

      private void EnableDomainReply()
      {
         _domain.VacationMessageIsOn = true;
         _domain.VacationSubject = DomainExternalSubject;
         _domain.VacationMessage = DomainExternalBody;
         _domain.Save();
      }

      [Test]
      [Description("A domain-level reply answers for an account with no vacation message of its own - and only once the feature is switched on.")]
      public void DomainReplyAnswersForAnAccountWithNoOwnMessage()
      {
         var sender = SingletonProvider<TestSetup>.Instance.AddAccount(_domain,
            TestSetup.UniqueString() + "@example.test", "test");
         var closed = SingletonProvider<TestSetup>.Instance.AddAccount(_domain,
            TestSetup.UniqueString() + "@example.test", "test");

         var smtp = new SmtpClientSimulator();

         // The negative control, first: with the domain reply off (the default) and
         // the account owning no vacation message, a delivery produces no reply.
         // Without this phase, the positive phase below could pass against a server
         // that auto-replies unconditionally.
         smtp.Send(sender.Address, closed.Address, "Before", "Body");
         Pop3ClientSimulator.AssertMessageCount(closed.Address, "test", 1);
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         Pop3ClientSimulator.AssertMessageCount(sender.Address, "test", 0);

         EnableDomainReply();

         smtp.Send(sender.Address, closed.Address, "After", "Body");
         Pop3ClientSimulator.AssertMessageCount(closed.Address, "test", 2);
         Pop3ClientSimulator.AssertMessageCount(sender.Address, "test", 1);

         var reply = new Pop3ClientSimulator().GetFirstMessageText(sender.Address, "test");

         StringAssert.Contains(DomainExternalSubject, reply,
            "The reply does not carry the domain's configured subject.");
         StringAssert.Contains(DomainExternalBody, reply,
            "The reply does not carry the domain's configured text.");
         StringAssert.Contains("Auto-Submitted: auto-replied", reply,
            "RFC 3834: the reply must declare itself automatic, or the next auto-responder answers it.");
         StringAssert.Contains("Return-Path: <>", reply,
            "RFC 3834: the reply must travel with a null envelope sender, or a bounce of it starts a loop.");
      }

      [Test]
      [Description("An account's own vacation message wins over the domain's, and exactly one reply is sent.")]
      public void AccountsOwnMessageWinsOverTheDomains()
      {
         var sender = SingletonProvider<TestSetup>.Instance.AddAccount(_domain,
            TestSetup.UniqueString() + "@example.test", "test");
         var away = SingletonProvider<TestSetup>.Instance.AddAccount(_domain,
            TestSetup.UniqueString() + "@example.test", "test");

         EnableDomainReply();

         away.VacationMessageIsOn = true;
         away.VacationSubject = "Back on Monday";
         away.VacationMessage = "I am away until Monday.";
         away.Save();

         new SmtpClientSimulator().Send(sender.Address, away.Address, "Hello", "Body");

         Pop3ClientSimulator.AssertMessageCount(away.Address, "test", 1);
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         // Exactly one reply. Two would mean the domain spoke over the account,
         // doubling the load on every loop guard and letting the two texts
         // contradict each other.
         Pop3ClientSimulator.AssertMessageCount(sender.Address, "test", 1);

         var reply = new Pop3ClientSimulator().GetFirstMessageText(sender.Address, "test");
         StringAssert.Contains("I am away until Monday.", reply,
            "The account's own message must be the one that answers.");
         ClassicAssert.IsFalse(reply.Contains(DomainExternalBody),
            "The domain's text must not appear when the account has a message of its own.");
      }

      [Test]
      [Description("An internal sender gets the internal text; an external sender gets the external text and never the internal one.")]
      public void InternalAndExternalSendersGetDifferentText()
      {
         var colleague = SingletonProvider<TestSetup>.Instance.AddAccount(_domain,
            TestSetup.UniqueString() + "@example.test", "test");
         var closed = SingletonProvider<TestSetup>.Instance.AddAccount(_domain,
            TestSetup.UniqueString() + "@example.test", "test");

         EnableDomainReply();
         _domain.VacationInternalSubject = DomainInternalSubject;
         _domain.VacationInternalMessage = DomainInternalBody;
         _domain.Save();

         // The internal sender: a mailbox hosted here. Its reply is a local
         // delivery and can be read back over POP3.
         new SmtpClientSimulator().Send(colleague.Address, closed.Address, "Internal question", "Body");

         Pop3ClientSimulator.AssertMessageCount(colleague.Address, "test", 1);
         var internalReply = new Pop3ClientSimulator().GetFirstMessageText(colleague.Address, "test");

         StringAssert.Contains(DomainInternalBody, internalReply,
            "A sender who resolves to a local mailbox must receive the internal text.");
         ClassicAssert.IsFalse(internalReply.Contains(DomainExternalBody),
            "The internal reply must not carry the external text as well.");

         // The external sender: an address hosted nowhere on this server. Its reply
         // leaves over SMTP, so a route captures it - which also lets the outbound
         // envelope be inspected, something a stored copy cannot show.
         const string externalSender = "outsider@external-oo.test";

         int capturePort = TestSetup.GetNextFreePort();
         using (var capture = new SmtpServerSimulator(1, capturePort))
         {
            var accepted = new Dictionary<string, int> {{externalSender, 250}};
            capture.AddRecipientResult(accepted);
            capture.StartListen();

            var route = _settings.Routes.Add();
            route.DomainName = "external-oo.test";
            route.TargetSMTPHost = "localhost";
            route.TargetSMTPPort = capturePort;
            route.NumberOfTries = 1;
            route.MinutesBetweenTry = 5;
            route.Save();

            SmtpClientSimulator.StaticSend(externalSender, closed.Address, "External question", "Body");
            _application.SubmitEMail();

            capture.WaitForCompletion();

            StringAssert.Contains(DomainExternalBody, capture.MessageData,
               "A sender who resolves to nothing local must receive the external text.");
            ClassicAssert.IsFalse(capture.MessageData.Contains(DomainInternalBody),
               "The internal text must never leave the installation: it is written for colleagues, " +
               "and the envelope sender is the only thing distinguishing them.");
            StringAssert.Contains("Auto-Submitted: auto-replied", capture.MessageData,
               "The externally delivered reply must declare itself automatic too.");
            StringAssert.Contains("MAIL FROM:<>", capture.MailFromCommand,
               "RFC 3834: the reply must be transmitted with a null envelope sender.");
         }
      }

      [Test]
      [Description("A message with a null return path (MAIL FROM:<>) gets no reply; the same account still answers an ordinary sender.")]
      public void NoReplyToANullReturnPath()
      {
         var closed = SingletonProvider<TestSetup>.Instance.AddAccount(_domain,
            TestSetup.UniqueString() + "@example.test", "test");
         var control = SingletonProvider<TestSetup>.Instance.AddAccount(_domain,
            TestSetup.UniqueString() + "@example.test", "test");

         EnableDomainReply();

         // A bounce: null envelope sender. There is nowhere a reply could even be
         // addressed - which is the point of the rule - so the observable assertions
         // are that the message is delivered, nothing is left queued, and the
         // feature is proven live immediately afterwards on the same account.
         var smtp = new SmtpClientSimulator();
         smtp.Send("", new List<string> {closed.Address}, "Delivery status", "The original message could not be delivered.");

         Pop3ClientSimulator.AssertMessageCount(closed.Address, "test", 1);
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         // The control half: if the feature were inert this send would get no reply
         // and the assertion below would fail, so silence towards the bounce above
         // is meaningful rather than universal.
         smtp.Send(control.Address, closed.Address, "Real question", "Body");
         Pop3ClientSimulator.AssertMessageCount(closed.Address, "test", 2);
         Pop3ClientSimulator.AssertMessageCount(control.Address, "test", 1);
      }

      [Test]
      [Description("A Precedence: bulk message gets no reply, and being refused does not use up the sender's once-per-sender slot.")]
      public void NoReplyToABulkPrecedenceMessage()
      {
         var sender = SingletonProvider<TestSetup>.Instance.AddAccount(_domain,
            TestSetup.UniqueString() + "@example.test", "test");
         var closed = SingletonProvider<TestSetup>.Instance.AddAccount(_domain,
            TestSetup.UniqueString() + "@example.test", "test");

         EnableDomainReply();

         var smtp = new SmtpClientSimulator();

         var bulkMessage =
            "From: " + sender.Address + "\r\n" +
            "To: " + closed.Address + "\r\n" +
            "Subject: Newsletter\r\n" +
            "Precedence: bulk\r\n" +
            "\r\n" +
            "This is bulk mail and must not be answered.\r\n";

         smtp.SendRaw(sender.Address, closed.Address, bulkMessage);

         Pop3ClientSimulator.AssertMessageCount(closed.Address, "test", 1);
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         Pop3ClientSimulator.AssertMessageCount(sender.Address, "test", 0);

         // The same sender now writes an ordinary message and IS answered. This is
         // the negative control for the assertion above - the only difference
         // between the two messages is the Precedence header - and it additionally
         // proves the refusal happened before the once-per-sender memory was
         // written, since a burnt slot would keep this reply from being sent.
         smtp.Send(sender.Address, closed.Address, "Ordinary question", "Body");

         Pop3ClientSimulator.AssertMessageCount(closed.Address, "test", 2);
         Pop3ClientSimulator.AssertMessageCount(sender.Address, "test", 1);

         var reply = new Pop3ClientSimulator().GetFirstMessageText(sender.Address, "test");
         StringAssert.Contains(DomainExternalBody, reply,
            "The ordinary message from the same sender should have been answered.");
      }

      [Test]
      [Description("With the domain's external override on, a stranger gets the domain's generic text while a colleague still gets the account's own message.")]
      public void ExternalOverrideReplacesTheAccountsOwnMessageForStrangersOnly()
      {
         var colleague = SingletonProvider<TestSetup>.Instance.AddAccount(_domain,
            TestSetup.UniqueString() + "@example.test", "test");
         var away = SingletonProvider<TestSetup>.Instance.AddAccount(_domain,
            TestSetup.UniqueString() + "@example.test", "test");

         // Only the override and the external text - the domain-wide fallback
         // switch stays OFF, proving the two policies are independent.
         _domain.VacationExternalOverride = true;
         _domain.VacationSubject = DomainExternalSubject;
         _domain.VacationMessage = DomainExternalBody;
         _domain.Save();

         const string personalBody = "I am at the conference all week; ask Priya about the Delahaye account.";
         away.VacationMessageIsOn = true;
         away.VacationSubject = "At the conference";
         away.VacationMessage = personalBody;
         away.Save();

         // The colleague still gets the personal message.
         new SmtpClientSimulator().Send(colleague.Address, away.Address, "Internal question", "Body");

         Pop3ClientSimulator.AssertMessageCount(colleague.Address, "test", 1);
         var internalReply = new Pop3ClientSimulator().GetFirstMessageText(colleague.Address, "test");
         StringAssert.Contains(personalBody, internalReply,
            "A colleague must still receive the account's own message.");

         // The stranger gets the domain's generic text in its place.
         const string externalSender = "stranger@external-oo.test";

         int capturePort = TestSetup.GetNextFreePort();
         using (var capture = new SmtpServerSimulator(1, capturePort))
         {
            var accepted = new Dictionary<string, int> {{externalSender, 250}};
            capture.AddRecipientResult(accepted);
            capture.StartListen();

            var route = _settings.Routes.Add();
            route.DomainName = "external-oo.test";
            route.TargetSMTPHost = "localhost";
            route.TargetSMTPPort = capturePort;
            route.NumberOfTries = 1;
            route.MinutesBetweenTry = 5;
            route.Save();

            SmtpClientSimulator.StaticSend(externalSender, away.Address, "External question", "Body");
            _application.SubmitEMail();

            capture.WaitForCompletion();

            StringAssert.Contains(DomainExternalBody, capture.MessageData,
               "The stranger should have received the domain's generic external text.");
            ClassicAssert.IsFalse(capture.MessageData.Contains(personalBody),
               "The account's personal message must not reach an external sender when the override is on: " +
               "that text names people and matters, and hiding it from strangers is what the override is FOR.");
         }
      }
      // ---- the policy: the window, the memory, and the account's own internal text

      private hMailServer.Account AddAccount()
      {
         return SingletonProvider<TestSetup>.Instance.AddAccount(_domain, TestSetup.UniqueString() + "@example.test", "test");
      }

      /// <summary>
      ///    Moves every answered-sender stamp back by the given number of days, so
      ///    that a window can be crossed without waiting for it. The stamp is a
      ///    unix time in hm_autoreplies, written by SMTPVacationMessageCreator.
      /// </summary>
      private void AgeTheMemory(int days)
      {
         _application.Database.ExecuteSQL("update hm_autoreplies set arsent = arsent - " + (days * 86400L));
      }

      [Test]
      [Description("The once-per-sender memory is the database's: a sender answered before a restart is not answered again after it, and switching the reply off and on forgets everybody.")]
      public void TheMemorySurvivesARestartAndIsForgottenWhenTheReplyIsSwitchedOff()
      {
         var sender = AddAccount();
         var away = AddAccount();
         away.VacationMessageIsOn = true;
         away.VacationSubject = "Away";
         away.VacationMessage = "Back next week.";
         away.Save();

         new SmtpClientSimulator().Send(sender.Address, away.Address, "One", "Body");
         Pop3ClientSimulator.AssertMessageCount(sender.Address, "test", 1);

         _application.Stop();
         _application.Start();

         new SmtpClientSimulator().Send(sender.Address, away.Address, "Two", "Body");
         Pop3ClientSimulator.AssertMessageCount(away.Address, "test", 2);
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         Pop3ClientSimulator.AssertMessageCount(sender.Address, "test", 1);

         // Off and on again is a new absence, and the memory is the old one's.
         away.VacationMessageIsOn = false;
         away.Save();
         away.VacationMessageIsOn = true;
         away.Save();

         new SmtpClientSimulator().Send(sender.Address, away.Address, "Three", "Body");
         Pop3ClientSimulator.AssertMessageCount(sender.Address, "test", 2);
      }

      [Test]
      [Description("A window in days: the same sender is answered again once the window has passed and not before; 0 keeps to once per absence; the window is 0 to 3650.")]
      public void TheSameSenderIsAnsweredAgainAfterTheWindow()
      {
         var sender = AddAccount();
         var away = AddAccount();
         away.VacationMessageIsOn = true;
         away.VacationSubject = "Away";
         away.VacationMessage = "Back in a month.";
         away.VacationMessageDays = 3;
         away.Save();

         new SmtpClientSimulator().Send(sender.Address, away.Address, "One", "Body");
         Pop3ClientSimulator.AssertMessageCount(sender.Address, "test", 1);

         new SmtpClientSimulator().Send(sender.Address, away.Address, "Two", "Body");
         Pop3ClientSimulator.AssertMessageCount(away.Address, "test", 2);
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         Pop3ClientSimulator.AssertMessageCount(sender.Address, "test", 1);

         AgeTheMemory(4);

         new SmtpClientSimulator().Send(sender.Address, away.Address, "Three", "Body");
         Pop3ClientSimulator.AssertMessageCount(sender.Address, "test", 2);

         // No window: however old the stamp, once is once.
         away.VacationMessageDays = 0;
         away.Save();
         AgeTheMemory(400);

         new SmtpClientSimulator().Send(sender.Address, away.Address, "Four", "Body");
         Pop3ClientSimulator.AssertMessageCount(away.Address, "test", 4);
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         Pop3ClientSimulator.AssertMessageCount(sender.Address, "test", 2);

         var refused = Assert.Throws<System.Runtime.InteropServices.COMException>(() => away.VacationMessageDays = 3651);
         StringAssert.Contains("3650", refused.Message);
      }

      [Test]
      [Description("An account's internal text goes to colleagues; a stranger gets the general text and never the internal one.")]
      public void ColleaguesGetTheAccountsInternalText()
      {
         const string generalBody = "I am away; the office will help.";
         const string internalBody = "Ask Priya in room 12 about anything urgent.";
         const string externalSender = "outsider@external-oo.test";

         var colleague = AddAccount();
         var away = AddAccount();
         away.VacationMessageIsOn = true;
         away.VacationSubject = "Away";
         away.VacationMessage = generalBody;
         away.VacationInternalSubject = "Away - ask Priya";
         away.VacationInternalMessage = internalBody;
         away.Save();

         new SmtpClientSimulator().Send(colleague.Address, away.Address, "Internal question", "Body");
         Pop3ClientSimulator.AssertMessageCount(colleague.Address, "test", 1);
         var internalReply = new Pop3ClientSimulator().GetFirstMessageText(colleague.Address, "test");
         StringAssert.Contains(internalBody, internalReply, "A colleague gets the account's internal text.");
         StringAssert.Contains("Away - ask Priya", internalReply, "And its internal subject.");
         ClassicAssert.IsFalse(internalReply.Contains(generalBody), "The internal reply does not carry the general text as well.");

         int capturePort = TestSetup.GetNextFreePort();
         using (var capture = new SmtpServerSimulator(1, capturePort))
         {
            capture.AddRecipientResult(new Dictionary<string, int> {{externalSender, 250}});
            capture.StartListen();

            var route = _settings.Routes.Add();
            route.DomainName = "external-oo.test";
            route.TargetSMTPHost = "localhost";
            route.TargetSMTPPort = capturePort;
            route.NumberOfTries = 1;
            route.MinutesBetweenTry = 5;
            route.Save();

            SmtpClientSimulator.StaticSend(externalSender, away.Address, "External question", "Body");
            _application.SubmitEMail();
            capture.WaitForCompletion();

            StringAssert.Contains(generalBody, capture.MessageData, "A stranger gets the general text.");
            ClassicAssert.IsFalse(capture.MessageData.Contains(internalBody),
               "The internal text must never leave the installation: the envelope sender is all that tells a colleague from a stranger.");
         }
      }

      [Test]
      [Description("An internal-only reply answers colleagues and is silent to strangers - unless the domain's external override has a text for them, which then answers in its place.")]
      public void AnInternalOnlyReplyIsSilentToStrangers()
      {
         const string personalBody = "At the conference; Priya covers for me.";
         const string externalSender = "outsider@external-oo.test";

         var colleague = AddAccount();
         var away = AddAccount();
         away.VacationMessageIsOn = true;
         away.VacationSubject = "Away";
         away.VacationMessage = personalBody;
         away.VacationMessageInternalOnly = true;
         away.Save();

         new SmtpClientSimulator().Send(colleague.Address, away.Address, "Internal question", "Body");
         Pop3ClientSimulator.AssertMessageCount(colleague.Address, "test", 1);
         StringAssert.Contains(personalBody, new Pop3ClientSimulator().GetFirstMessageText(colleague.Address, "test"));

         int capturePort = TestSetup.GetNextFreePort();
         using (var capture = new SmtpServerSimulator(1, capturePort))
         {
            capture.AddRecipientResult(new Dictionary<string, int> {{externalSender, 250}});
            capture.StartListen();

            var route = _settings.Routes.Add();
            route.DomainName = "external-oo.test";
            route.TargetSMTPHost = "localhost";
            route.TargetSMTPPort = capturePort;
            route.NumberOfTries = 1;
            route.MinutesBetweenTry = 5;
            route.Save();

            // The stranger: delivered to the account, and nothing goes back. The
            // route is there so that a reply, if one were made, would reach the
            // capture rather than fail quietly against a domain nobody resolves.
            SmtpClientSimulator.StaticSend(externalSender, away.Address, "External question", "Body");
            Pop3ClientSimulator.AssertMessageCount(away.Address, "test", 2);
            _application.SubmitEMail();
            CustomAsserts.AssertRecipientsInDeliveryQueue(0);
            ClassicAssert.AreEqual(0, capture.RcptTosReceived, "An internal-only reply must not answer a stranger.");

            // The domain's override speaks for the account to strangers, with the
            // domain's text - the account's own is still never sent outside.
            EnableDomainReply();
            _domain.VacationExternalOverride = true;
            _domain.Save();

            SmtpClientSimulator.StaticSend(externalSender, away.Address, "External question again", "Body");
            _application.SubmitEMail();
            capture.WaitForCompletion();

            StringAssert.Contains(DomainExternalBody, capture.MessageData, "With the override on, the stranger gets the domain's text.");
            ClassicAssert.IsFalse(capture.MessageData.Contains(personalBody), "And never the account's own.");
         }
      }

      [Test]
      [Description("The domain reply has its own window: the same sender is answered again once it has passed.")]
      public void TheDomainReplyHasItsOwnWindow()
      {
         var sender = AddAccount();
         var closed = AddAccount();

         EnableDomainReply();
         _domain.VacationDays = 2;
         _domain.Save();

         new SmtpClientSimulator().Send(sender.Address, closed.Address, "One", "Body");
         Pop3ClientSimulator.AssertMessageCount(sender.Address, "test", 1);

         new SmtpClientSimulator().Send(sender.Address, closed.Address, "Two", "Body");
         Pop3ClientSimulator.AssertMessageCount(closed.Address, "test", 2);
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         Pop3ClientSimulator.AssertMessageCount(sender.Address, "test", 1);

         AgeTheMemory(3);

         new SmtpClientSimulator().Send(sender.Address, closed.Address, "Three", "Body");
         Pop3ClientSimulator.AssertMessageCount(sender.Address, "test", 2);
      }

      private static string WithLoopCount(hMailServer.Account sender, hMailServer.Account recipient, string loopCount)
      {
         return
            "From: " + sender.Address + "\r\n" +
            "To: " + recipient.Address + "\r\n" +
            "Subject: Counted\r\n" +
            "X-hMailServer-LoopCount: " + loopCount + "\r\n" +
            "\r\n" +
            "Body.\r\n";
      }

      /// <summary>
      ///    The reply carries the message's X-hMailServer-LoopCount + 1, and none is sent
      ///    once that count has reached the rule loop limit, as a Sieve vacation reply and
      ///    a rule's reply are held. The reply is a new message, and its count was raised
      ///    from nothing, so every reply carried 1 whatever the message it answered had
      ///    been through: the count, which is what ends a loop of generated messages,
      ///    started again at every out-of-office reply.
      ///
      ///    Against the unfixed server the first reply carries 1, and the message at the
      ///    limit is answered.
      /// </summary>
      [Test]
      [Description("An out-of-office reply carries the message's loop count + 1, and none is sent once the count has reached the rule loop limit.")]
      public void TheReplyCarriesTheLoopCountOnAndStopsAtTheLimit()
      {
         _settings.RuleLoopLimit = 5;

         var below = AddAccount();
         var atTheLimit = AddAccount();
         var away = AddAccount();
         away.VacationMessageIsOn = true;
         away.VacationSubject = "Away";
         away.VacationMessage = "I am away.";
         away.Save();

         var smtp = new SmtpClientSimulator();
         smtp.SendRaw(below.Address, away.Address, WithLoopCount(below, away, "2"));
         Pop3ClientSimulator.AssertMessageCount(below.Address, "test", 1);

         var reply = new Pop3ClientSimulator().GetFirstMessageText(below.Address, "test");
         StringAssert.Contains("X-hMailServer-LoopCount: 3\r\n", reply,
            "The reply to a message carrying 2 does not carry 3. The reply was:\r\n" + reply);

         smtp.SendRaw(atTheLimit.Address, away.Address, WithLoopCount(atTheLimit, away, "5"));
         Pop3ClientSimulator.AssertMessageCount(away.Address, "test", 2);
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         Pop3ClientSimulator.AssertMessageCount(atTheLimit.Address, "test", 0);
      }
   }
}
