// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.InteropServices;
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
      ///    _wtoi (_ttoi, which is (int) wcstol, on Linux). "-5" gave the notification
      ///    -4, so its forwards had five hops more to go than after a count of 0, and
      ///    "-2147483648" some two thousand million more; 2147483647 made the
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

      // The lines of a message's header that begin a field of this name, as a mail
      // client finds them: up to the first empty line.
      private static List<string> FieldLines(string message, string name)
      {
         var lines = new List<string>();
         foreach (string line in message.Replace("\r\n", "\n").Split('\n'))
         {
            if (line.Length == 0)
               break;

            if (line.StartsWith(name + ":", StringComparison.OrdinalIgnoreCase))
               lines.Add(line);
         }

         return lines;
      }

      /// <summary>
      ///    A notification's header is the server's. The MIME writer wrote a line
      ///    break in a value as it stood, so a ':message' holding line breaks wrote
      ///    "Auto-Submitted: no" and "X-hMailServer-LoopCount: 0" as fields of their
      ///    own, above the notification's own two, and those are what is read first:
      ///    a script that notified its own account was then held neither by the rule
      ///    that a message which is itself auto-submitted is not notified about nor by
      ///    the rule loop limit, and notified about its own notifications for ever. The
      ///    writer now keeps a value to one field (MimeField::AppendAsOneField): the
      ///    one notification carries Auto-Submitted: auto-notified and a count of 1,
      ///    and is not notified about.
      ///
      ///    Against the unfixed server the notification's first Auto-Submitted is
      ///    "no", and the account's mailbox does not stop filling.
      /// </summary>
      [Test]
      [Description("A notification whose :message holds line breaks writes no field of its own, and a script notifying its own account sends one notification.")]
      public void ANotificationsMessageWritesNoFieldOfItsOwn()
      {
         _settings.RuleLoopLimit = 5;

         int sequence = NextAccountSequence();
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(
            _domain, "sieve-ntf-self-" + sequence + "@example.test", Password);

         SetScript(account,
            "require \"enotify\";\r\n" +
            "notify :message \"Note\r\nAuto-Submitted: no\r\nX-hMailServer-LoopCount: 0\" \"mailto:" + account.Address + "\";\r\n");

         SmtpClientSimulator.StaticSend("sieve-ntf-sender@example.test", account.Address, "Trigger", "Body.");
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         IMAPFolder inbox = account.IMAPFolders.get_ItemByName("INBOX");
         CustomAsserts.AssertFolderMessageCount(inbox, 2);

         // Nothing followed the notification: it was not notified about.
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         Assert.AreEqual(2, inbox.Messages.Count, "The notification was notified about.");

         // The two in the order they arrive, which is not fixed.
         string first = Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, Password);
         string second = Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, Password);
         string notification = first.Contains("You have new mail.") ? first : second;

         StringAssert.Contains("You have new mail.", notification, "No notification arrived.");
         CollectionAssert.AreEqual(new[] { "Auto-Submitted: auto-notified" }, FieldLines(notification, "Auto-Submitted"),
            "The notification's Auto-Submitted is not its own alone. The notification was:\r\n" + notification);
         CollectionAssert.AreEqual(new[] { "X-hMailServer-LoopCount: 1" }, FieldLines(notification, "X-hMailServer-LoopCount"),
            "The notification's loop count is not its own alone. The notification was:\r\n" + notification);
         Assert.AreEqual(1, FieldLines(notification, "Subject").Count,
            "The notification's subject is not one field. The notification was:\r\n" + notification);
      }

      // Invoked late-bound, as the SieveScript property is, so that the test does not
      // depend on the registered type library.
      private string CheckSyntax(string script)
      {
         object utilities = _application.Utilities;
         return (string) utilities.GetType().InvokeMember(
            "CheckSieveSyntax", BindingFlags.InvokeMethod, null, utilities, new object[] { script });
      }

      // A server that checks a script when it is stored - the REST API's filter route,
      // which is how the Linux suite stores one - refuses a notify :from that is not an
      // address, so a script stored before that check cannot be made there.
      private static bool RefusedForTheFrom(Exception refused)
      {
         Exception cause = refused is TargetInvocationException && refused.InnerException != null ? refused.InnerException : refused;
         return cause is COMException && cause.Message.Contains("notify :from");
      }

      // The application log's line for a :from that was not used, in the parts that hold
      // no quotation mark, which the log's JSON format would write differently.
      private static void AssertFromNotUsedIsLogged(Account recipient)
      {
         Assert.IsTrue(LogHandler.DefaultLogContains("Sieve notify: the script for " + recipient.Address + " asked for ':from "),
            "The notify :from that was not used is not in the application log.");
         Assert.IsTrue(LogHandler.DefaultLogContains("which is not an address that reaches that account. The notification was sent from " +
               recipient.Address + " instead."),
            "The application log does not say the notification was sent from the account's own address.");
      }

      private static string NotifyScript(string from, string watcherAddress)
      {
         return
            "require \"enotify\";\r\n" +
            "notify :from \"" + from + "\" :message \"Something arrived\" \"mailto:" + watcherAddress + "\";\r\n";
      }

      /// <summary>
      ///    A notification's :from is the notification's From when it reaches the
      ///    account - here an active alias whose value is the account's address - which
      ///    is the rule a vacation reply's :from is held to
      ///    (SieveVacationDelivery.TestFromIsHonouredOnlyForTheUsersOwnAddresses). The
      ///    script is accepted when it is stored.
      /// </summary>
      [Test]
      [Description("A notify :from naming an alias of the account is accepted when stored, and is the notification's From.")]
      public void ANotifyFromThatReachesTheAccountIsTheSender()
      {
         Account recipient, watcher;
         NewPair(out recipient, out watcher);

         string aliasAddress = "sieve-ntf-alias-" + NextAccountSequence() + "@example.test";
         SingletonProvider<TestSetup>.Instance.AddAlias(_domain, aliasAddress, recipient.Address);

         string script = NotifyScript(aliasAddress, watcher.Address);
         Assert.IsEmpty(CheckSyntax(script), "A notify :from naming one of the account's own addresses was refused when stored.");
         SetScript(recipient, script);

         SmtpClientSimulator.StaticSend("sieve-ntf-sender@example.test", recipient.Address, "Original subject", "Body.");
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         string notification = Pop3ClientSimulator.AssertGetFirstMessageText(watcher.Address, Password);
         StringAssert.Contains("Something arrived", notification, "No notification arrived.");
         CollectionAssert.AreEqual(new[] { "From: " + aliasAddress }, FieldLines(notification, "From"),
            "A notify :from naming an alias of the account is not the notification's From. The notification was:\r\n" + notification);
      }

      /// <summary>
      ///    A notify :from that is not an address at all is refused when the script is
      ///    stored, with a sentence that names it, where a vacation :from is refused: no
      ///    address, a display name, and an address followed by a line break and a field
      ///    of its own. An address is accepted, whoever's it is - whether it reaches the
      ///    account is a question about the account, asked at delivery
      ///    (AnotherAccountsAddressAsNotifyFromIsNotTheSender).
      ///
      ///    Against the unfixed server every notify script below is accepted.
      /// </summary>
      [Test]
      [Description("A notify :from that is not an address is refused when the script is stored, as a vacation :from is.")]
      public void ANotifyFromThatIsNotAnAddressIsRefusedWhenStored()
      {
         const string Watcher = "sieve-ntf-watch@example.test";

         foreach (string from in new[] { "not an address", "", "Boss <ceo@example.test>", "sieve-ntf@example.test\r\nX-A: Boss <ceo@example.test>" })
         {
            string error = CheckSyntax(NotifyScript(from, Watcher));
            StringAssert.Contains("notify :from", error,
               "A notify :from of '" + from + "' was not refused, or the refusal does not name it, when the script was stored.");

            // The rule notify's follows.
            Assert.IsNotEmpty(CheckSyntax("require \"vacation\";\r\nvacation :from \"" + from + "\" \"Away.\";\r\n"),
               "A vacation :from of '" + from + "' was accepted when the script was stored, so the two rules no longer agree.");
         }

         // The controls: an address is stored, the account's or another's, and so is a
         // notify without :from.
         Assert.IsEmpty(CheckSyntax(NotifyScript("sieve-ntf@example.test", Watcher)));
         Assert.IsEmpty(CheckSyntax(NotifyScript("somebody-else@example.test", Watcher)));
         Assert.IsEmpty(CheckSyntax("require \"enotify\";\r\nnotify \"mailto:" + Watcher + "\";\r\n"));
      }

      /// <summary>
      ///    A notification's :from was its From whatever it said, so any account with a
      ///    script sent mail through this server as anyone - another account here, or
      ///    any address anywhere, and DKIM-signed for a domain this server signs for.
      ///    It is now held to the rule a vacation reply's :from is: another account's
      ///    address is not one that reaches this account, so the notification comes
      ///    from the account's own address, and the application log says so.
      ///
      ///    Against the unfixed server the notification's From is the other account's.
      /// </summary>
      [Test]
      [Description("A notify :from naming another account is not the notification's From: the account's own address is, and it is logged.")]
      public void AnotherAccountsAddressAsNotifyFromIsNotTheSender()
      {
         Account recipient, watcher;
         NewPair(out recipient, out watcher);

         Account victim = SingletonProvider<TestSetup>.Instance.AddAccount(
            _domain, "sieve-ntf-victim-" + NextAccountSequence() + "@example.test", Password);

         SetScript(recipient, NotifyScript(victim.Address, watcher.Address));

         SmtpClientSimulator.StaticSend("sieve-ntf-sender@example.test", recipient.Address, "Original subject", "Body.");
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         string notification = Pop3ClientSimulator.AssertGetFirstMessageText(watcher.Address, Password);
         StringAssert.Contains("Something arrived", notification, "No notification arrived.");
         CollectionAssert.AreEqual(new[] { "From: " + recipient.Address }, FieldLines(notification, "From"),
            "A notify :from naming another account was not replaced by the account's own address. The notification was:\r\n" + notification);

         AssertFromNotUsedIsLogged(recipient);
      }

      /// <summary>
      ///    A script stored before notify's :from was checked can hold one that is not
      ///    an address. It still runs - refused at delivery, it would file nothing and
      ///    notify nobody - and its notification comes from the account's own address.
      ///
      ///    COM's SieveScript property writes the script as it is given, as a script
      ///    stored before the check was. The REST API's filter route, which is how the
      ///    Linux suite stores a script, checks it and refuses this one, as
      ///    ANotifyFromThatIsNotAnAddressIsRefusedWhenStored asserts, so there the case
      ///    is skipped, with that said.
      ///
      ///    Against the unfixed server the notification's From is "not an address".
      /// </summary>
      [Test]
      [Description("A stored script whose notify :from is not an address runs, and its notification is from the account's own address.")]
      public void AStoredNotifyFromThatIsNotAnAddressIsNotTheSender()
      {
         Account recipient, watcher;
         NewPair(out recipient, out watcher);
         recipient.IMAPFolders.Add("Kept");

         try
         {
            SetScript(recipient,
               "require [\"enotify\", \"fileinto\"];\r\n" +
               "fileinto \"Kept\";\r\n" +
               "notify :from \"not an address\" :message \"Something arrived\" \"mailto:" + watcher.Address + "\";\r\n");
         }
         catch (Exception refused) when (RefusedForTheFrom(refused))
         {
            Assert.Ignore("This server checks a script when it is stored and refused this one, so no script of the kind " +
               "this case runs can be stored here: " + refused.GetBaseException().Message);
         }

         SmtpClientSimulator.StaticSend("sieve-ntf-sender@example.test", recipient.Address, "Original subject", "Body.");
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         IMAPFolder kept = recipient.IMAPFolders.get_ItemByName("Kept");
         CustomAsserts.AssertFolderMessageCount(kept, 1);

         string notification = Pop3ClientSimulator.AssertGetFirstMessageText(watcher.Address, Password);
         StringAssert.Contains("Something arrived", notification, "No notification arrived.");
         CollectionAssert.AreEqual(new[] { "From: " + recipient.Address }, FieldLines(notification, "From"),
            "A stored notify :from that is not an address was not replaced by the account's own address. The notification was:\r\n" + notification);

         AssertFromNotUsedIsLogged(recipient);
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
