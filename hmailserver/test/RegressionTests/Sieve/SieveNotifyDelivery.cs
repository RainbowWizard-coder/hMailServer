// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Reflection;
using System.Threading;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;
using hMailServer;

namespace RegressionTests.Sieve
{
   /// <summary>
   ///    enotify (RFC 5435) with the mailto method (RFC 5436), asserted on the
   ///    NOTIFICATION ARRIVING in the target mailbox - a real generated message,
   ///    read back with its headers, because the loop markers on it (null return
   ///    path, Auto-Submitted: auto-notified) are what make the feature safe to
   ///    have and are exactly what a summary-token assertion cannot see.
   ///
   ///    The loop control is the test that matters: a message that is ITSELF
   ///    auto-submitted must not be notified about, or two servers notifying each
   ///    other's contact addresses ping-pong for ever.
   /// </summary>
   [TestFixture]
   public class SieveNotifyDelivery : TestFixtureBase
   {
      private const string Password = "secret";

      private static int accountSequence_;

      // Unique per account created by this fixture, however the tests are ordered or parallelised.
      private static int NextAccountSequence()
      {
         return Interlocked.Increment(ref accountSequence_);
      }

      private static void SetScript(Account account, string script)
      {
         account.GetType().InvokeMember(
            "SieveScript", BindingFlags.SetProperty, null, account, new object[] { script });
      }

      private void NewPair(out Account recipient, out Account watcher)
      {
         int sequence = NextAccountSequence();
         recipient = SingletonProvider<TestSetup>.Instance.AddAccount(
            _domain, "sieve-ntf-rcpt-" + sequence + "@example.test", Password);
         watcher = SingletonProvider<TestSetup>.Instance.AddAccount(
            _domain, "sieve-ntf-watch-" + sequence + "@example.test", Password);

         SetScript(recipient,
            "require \"enotify\";\r\n" +
            "notify :message \"Something arrived\" \"mailto:" + watcher.Address + "\";\r\n");
      }

      [Test]
      [Description("The notification arrives in the target mailbox, with the loop markers on it.")]
      public void TheNotificationArrivesWithLoopMarkers()
      {
         Account recipient, watcher;
         NewPair(out recipient, out watcher);

         SmtpClientSimulator.StaticSend("sieve-ntf-sender@example.test", recipient.Address, "Original subject", "Body.");
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         // The triggering message delivered normally...
         IMAPFolder recipientInbox = recipient.IMAPFolders.get_ItemByName("INBOX");
         CustomAsserts.AssertFolderMessageCount(recipientInbox, 1);

         // ...and the watcher received the notification.
         string notification = Pop3ClientSimulator.AssertGetFirstMessageText(watcher.Address, Password);

         StringAssert.Contains("Something arrived", notification,
            "The ':message' subject is not on the notification.");
         StringAssert.Contains("Auto-Submitted: auto-notified", notification,
            "The notification lacks Auto-Submitted: auto-notified (RFC 5436 2.7.1) - the marker that " +
            "stops the next auto-responder answering it. Without it this feature is a loop generator.");
         StringAssert.Contains("Return-Path: <>", notification,
            "The notification's envelope sender is not null, so a bounce of it can start an exchange.");
         StringAssert.Contains("sieve-ntf-sender@example.test", notification,
            "The notification body does not say who the original message was from.");
      }

      /// <summary>
      ///    The loop control. The triggering message carries Auto-Submitted, so
      ///    notifying about it is exactly the first half of a ping-pong between
      ///    two servers' contact addresses. It must be delivered normally and
      ///    notified about NOT AT ALL.
      /// </summary>
      [Test]
      [Description("A message that is itself auto-submitted is not notified about - the loop control.")]
      public void AnAutoSubmittedMessageIsNotNotifiedAbout()
      {
         Account recipient, watcher;
         NewPair(out recipient, out watcher);

         string raw =
            "From: robot@example.test\r\n" +
            "To: " + recipient.Address + "\r\n" +
            "Subject: Automated report\r\n" +
            "Auto-Submitted: auto-generated\r\n" +
            "\r\n" +
            "Nightly job output.\r\n";

         var smtp = new SmtpClientSimulator();
         smtp.SendRaw("robot@example.test", recipient.Address, raw);
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         IMAPFolder recipientInbox = recipient.IMAPFolders.get_ItemByName("INBOX");
         CustomAsserts.AssertFolderMessageCount(recipientInbox, 1);

         IMAPFolder watcherInbox = watcher.IMAPFolders.get_ItemByName("INBOX");
         CustomAsserts.AssertFolderMessageCount(watcherInbox, 0);
      }

      private static void SendWithLoopCount(Account recipient, string subject, string loopCount)
      {
         string raw =
            "From: sieve-ntf-sender@example.test\r\n" +
            "To: " + recipient.Address + "\r\n" +
            "Subject: " + subject + "\r\n" +
            "X-hMailServer-LoopCount: " + loopCount + "\r\n" +
            "\r\n" +
            "Body.\r\n";

         var smtp = new SmtpClientSimulator();
         smtp.SendRaw("sieve-ntf-sender@example.test", recipient.Address, raw);
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
      }

      /// <summary>
      ///    The rule loop limit. A notification is generated mail, as a vacation reply
      ///    is, and carries the triggering message's X-hMailServer-LoopCount + 1; a
      ///    message that has already passed through RuleLoopLimit generated messages
      ///    is in a cycle, and is delivered without one. Until 6 October 2026
      ///    SieveNotifyResponder sent a notification whatever the count, where
      ///    SieveVacationResponder stopped at the limit.
      ///
      ///    Against the unfixed server the watcher's inbox holds a notification after
      ///    the first message.
      /// </summary>
      [Test]
      [Description("No notification once a message has been through the configured number of generated hops.")]
      public void NoNotificationOnceTheRuleLoopLimitIsReached()
      {
         // Stated rather than assumed, as Rules.cs does: the shipped default is 5,
         // and a fixture that ran earlier could have left it anywhere.
         _settings.RuleLoopLimit = 5;

         Account recipient, watcher;
         NewPair(out recipient, out watcher);

         IMAPFolder recipientInbox = recipient.IMAPFolders.get_ItemByName("INBOX");
         IMAPFolder watcherInbox = watcher.IMAPFolders.get_ItemByName("INBOX");

         SendWithLoopCount(recipient, "At the limit", "5");
         CustomAsserts.AssertFolderMessageCount(recipientInbox, 1);
         CustomAsserts.AssertFolderMessageCount(watcherInbox, 0);

         // One hop below it is notified about, so the check is on the number and not
         // on the header being there, and the notification carries the count + 1.
         SendWithLoopCount(recipient, "Below the limit", "4");
         CustomAsserts.AssertFolderMessageCount(recipientInbox, 2);

         string notification = Pop3ClientSimulator.AssertGetFirstMessageText(watcher.Address, Password);
         StringAssert.Contains("X-hMailServer-LoopCount: 5", notification,
            "The notification does not carry the triggering message's count + 1.");
      }

      /// <summary>
      ///    The header comes with the message, from whoever sent it, and was read with
      ///    _wtoi (_ttoi, which is (int) wcstol, on Linux). "-5" passed the limit, so
      ///    a forward of the notification was bounded by nothing; 2147483647 made the
      ///    notification's count + 1 a signed overflow; 99999999999 is 2147483647 to
      ///    _wtoi; and 2147483648 and 4294967295 are INT_MIN and -1 on Linux. What is
      ///    not a count up to the limit now reads as the limit
      ///    (RuleGuard::ReadLoopCount): the message is delivered, and nobody notified.
      ///
      ///    Against the unfixed server every one of them is notified about, since the
      ///    notification consulted no limit, and the test fails at the first.
      /// </summary>
      [Test]
      [Description("An X-hMailServer-LoopCount that is not a count reads as the rule loop limit: no notification.")]
      public void ALoopCountThatIsNoCountIsReadAsTheLimit()
      {
         _settings.RuleLoopLimit = 5;

         Account recipient, watcher;
         NewPair(out recipient, out watcher);

         IMAPFolder recipientInbox = recipient.IMAPFolders.get_ItemByName("INBOX");
         IMAPFolder watcherInbox = watcher.IMAPFolders.get_ItemByName("INBOX");

         string[] values = { "2147483647", "2147483648", "4294967295", "99999999999", "-5", "-2147483648", "12abc", "+1", "" };

         int delivered = 0;
         foreach (string value in values)
         {
            SendWithLoopCount(recipient, "Loop count '" + value + "'", value);

            delivered++;
            CustomAsserts.AssertFolderMessageCount(recipientInbox, delivered);
            Assert.AreEqual(0, watcherInbox.Messages.Count,
               "A message carrying X-hMailServer-LoopCount: '" + value + "' was notified about.");
         }

         // The control: a count below the limit is notified about, carrying + 1.
         SendWithLoopCount(recipient, "A count", "1");
         CustomAsserts.AssertFolderMessageCount(recipientInbox, delivered + 1);

         string notification = Pop3ClientSimulator.AssertGetFirstMessageText(watcher.Address, Password);
         StringAssert.Contains("X-hMailServer-LoopCount: 2", notification,
            "The notification of a message carrying 1 does not carry 2.");
      }

      [Test]
      [Description("valid_notify_method steers between the supported and unsupported method.")]
      public void ValidNotifyMethodAnswersHonestly()
      {
         int sequence = NextAccountSequence();
         Account recipient = SingletonProvider<TestSetup>.Instance.AddAccount(
            _domain, "sieve-ntf-rcpt-" + sequence + "@example.test", Password);
         recipient.IMAPFolders.Add("MailtoOk");
         recipient.IMAPFolders.Add("XmppOk");

         SetScript(recipient,
            "require [\"enotify\", \"fileinto\"];\r\n" +
            "if valid_notify_method \"mailto:x@example.test\" {\r\n" +
            "  fileinto \"MailtoOk\";\r\n" +
            "}\r\n" +
            "if valid_notify_method \"xmpp:x@example.test\" {\r\n" +
            "  fileinto \"XmppOk\";\r\n" +
            "}\r\n");

         SmtpClientSimulator.StaticSend("sieve-ntf-sender@example.test", recipient.Address, "Probe", "Body.");
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         IMAPFolder mailtoOk = recipient.IMAPFolders.get_ItemByName("MailtoOk");
         CustomAsserts.AssertFolderMessageCount(mailtoOk, 1);

         IMAPFolder xmppOk = recipient.IMAPFolders.get_ItemByName("XmppOk");
         CustomAsserts.AssertFolderMessageCount(xmppOk, 0);
      }
   }
}
