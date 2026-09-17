// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Text;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.SMTP
{
   /// <summary>
   ///    Telling an account holder their mailbox is filling up, before it fills.
   ///
   ///    Nothing did, so the first anyone heard of a full mailbox was mail that
   ///    stopped arriving - and since the refusal now happens during the sending
   ///    server's SMTP conversation, the person it happens to is the last to find out.
   ///
   ///    What is worth testing here is not that a notice is sent. It is that exactly
   ///    ONE is. A warning on every delivery while a mailbox sits above the threshold
   ///    would be worse than none: dozens of identical notices, each consuming a
   ///    little of the space they are complaining about, in a mailbox already short of
   ///    room. The implementation avoids that by treating the threshold as an event
   ///    rather than a state - below before this message, at or above after it - so
   ///    the second delivery in each test is the assertion that matters.
   /// </summary>
   [TestFixture]
   public class QuotaWarning : TestFixtureBase
   {
      private const string Mailbox = "quotawarn@example.test";

      /// <summary>
      ///    A body of roughly the requested size, in lines short enough that no SMTP
      ///    line-length limit is involved in what is being measured.
      /// </summary>
      private static string BodyOfAbout(int kilobytes)
      {
         var body = new StringBuilder();
         string line = new string('x', 900) + "\r\n";

         while (body.Length < kilobytes * 1024)
            body.Append(line);

         return body.ToString();
      }

      private static int MessagesIn(string address)
      {
         return new Pop3ClientSimulator().GetMessageCount(address, "test");
      }

      [Test]
      [Description("The delivery that takes a mailbox past the threshold produces exactly one warning, and the next delivery produces none - the warning is an event, not a state")]
      public void CrossingTheThresholdWarnsOnceAndOnlyOnce()
      {
         // 1 MB quota with the warning at 50%, so two 300 KB messages straddle the
         // threshold: the first lands under it, the second crosses it.
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, Mailbox, "test");
         account.MaxSize = 1;
         account.Save();

         try
         {
            ServerIniFile.SetSetting("QuotaWarningPercent", "50");
            RestartServerAndReacquireCom();

            // Under the threshold: 300 KB of a 1024 KB quota is 29%.
            SmtpClientSimulator.StaticSend("sender@example.test", Mailbox, "first", BodyOfAbout(300));
            Pop3ClientSimulator.AssertMessageCount(Mailbox, "test", 1);

            ClassicAssert.AreEqual(1, MessagesIn(Mailbox),
               "The first message is well under the threshold, so nothing should have been sent about it.");

            // Across it: about 58%.
            SmtpClientSimulator.StaticSend("sender@example.test", Mailbox, "second", BodyOfAbout(300));
            Pop3ClientSimulator.AssertMessageCount(Mailbox, "test", 3);

            // Read message 3 without popping the mailbox: the counts below are part of
            // the assertion, so nothing here may consume a message.
            var pop3 = new Pop3ClientSimulator();
            ClassicAssert.IsTrue(pop3.ConnectAndLogon(Mailbox, "test"));
            string warning = pop3.RETR(3);
            pop3.Disconnect();

            StringAssert.Contains("mailbox is almost full", warning.ToLower(),
               "The account holder should have been told their mailbox is filling up. Got: " + warning);
            StringAssert.Contains(Mailbox, warning,
               "The notice should name the mailbox it is about. Got: " + warning);
            StringAssert.Contains("postmaster@example.test", warning,
               "The notice comes from the postmaster of the recipient's own domain, because that is " +
               "who can do something about it. Got: " + warning);

            // The assertion the whole design exists for. A third delivery leaves the
            // mailbox still over the threshold, and a naive "is it over?" check would
            // send another notice here - and another for every message after that.
            SmtpClientSimulator.StaticSend("sender@example.test", Mailbox, "third", BodyOfAbout(50));
            Pop3ClientSimulator.AssertMessageCount(Mailbox, "test", 4);

            ClassicAssert.AreEqual(4, MessagesIn(Mailbox),
               "Only the crossing warns. A mailbox that is still over the threshold must not be " +
               "warned about again on every delivery - that would fill it faster than the mail does.");
         }
         finally
         {
            ServerIniFile.SetSetting("QuotaWarningPercent", null);
            RestartServerAndReacquireCom();
         }
      }

      private void ResetPolicy_()
      {
         _domain.QuotaWarningPercent = -1;
         _domain.QuotaWarningSubject = "";
         _domain.QuotaWarningText = "";
         _domain.QuotaGracePercent = -1;
         _domain.QuotaSendStopPercent = -1;
         _domain.Save();
         _settings.QuotaGracePercent = 0;
         _settings.QuotaSendStopPercent = 0;
      }

      [Test]
      [Description("A domain sets its own threshold and its own words: the warning arrives at the domain's percentage rather than the server's, with the domain's subject and text and the macros filled")]
      public void ADomainSetsItsOwnThresholdAndWords()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, Mailbox, "test");
         account.MaxSize = 1;
         account.Save();

         _domain.QuotaWarningPercent = 25;
         _domain.QuotaWarningSubject = "Postfach fast voll";
         _domain.QuotaWarningText = "Hallo %MACRO_MAILBOX%, %MACRO_PERCENT% Prozent von %MACRO_QUOTA% MB sind belegt.";
         _domain.Save();
         try
         {
            // 300 KB of 1024 is 29%: past the domain's 25, well under the server's 90.
            SmtpClientSimulator.StaticSend("sender@example.test", Mailbox, "first", BodyOfAbout(300));
            Pop3ClientSimulator.AssertMessageCount(Mailbox, "test", 2);

            var pop3 = new Pop3ClientSimulator();
            ClassicAssert.IsTrue(pop3.ConnectAndLogon(Mailbox, "test"));
            string warning = pop3.RETR(2);
            pop3.Disconnect();

            StringAssert.Contains("Subject: Postfach fast voll", warning, "The domain's subject. Got: " + warning);
            StringAssert.Contains("Hallo " + Mailbox + ", ", warning, "The domain's text with the mailbox filled in. Got: " + warning);
            StringAssert.IsMatch("(29|30) Prozent von 1 MB sind belegt", warning, "The used share and the limit filled in. Got: " + warning);
         }
         finally
         {
            ResetPolicy_();
         }
      }

      [Test]
      [Description("A grace past the limit: incoming mail is still accepted while the mailbox is over its limit by less than the grace, and refused at delivery once it is not - and a domain with no grace refuses at the limit itself")]
      public void TheGraceAcceptsIncomingMailPastTheLimit()
      {
         var graced = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, Mailbox, "test");
         graced.MaxSize = 1;
         graced.Save();

         var strict = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "quotastrict@example.test", "test");
         strict.MaxSize = 1;
         strict.Save();

         _domain.QuotaGracePercent = 50;
         _domain.Save();
         try
         {
            // 600 KB, then 600 KB more: 1.17 MB in a 1 MB mailbox, which the grace
            // (up to 1.5 MB) admits. The second delivery also crosses 90%, so the
            // warning is the third message.
            SmtpClientSimulator.StaticSend("sender@example.test", Mailbox, "first", BodyOfAbout(600));
            Pop3ClientSimulator.AssertMessageCount(Mailbox, "test", 1);
            SmtpClientSimulator.StaticSend("sender@example.test", Mailbox, "second", BodyOfAbout(600));
            Pop3ClientSimulator.AssertMessageCount(Mailbox, "test", 3);

            // 600 KB more would make 1.76 MB, past the grace: refused at delivery,
            // and the mailbox stays as it was.
            SmtpClientSimulator.StaticSend("sender@example.test", Mailbox, "third", BodyOfAbout(600));
            CustomAsserts.AssertRecipientsInDeliveryQueue(0);
            Pop3ClientSimulator.AssertMessageCount(Mailbox, "test", 3);

            // The same two messages into a mailbox whose domain allows no grace:
            // the second is refused at the limit, as it always was.
            _domain.QuotaGracePercent = 0;
            _domain.Save();
            SmtpClientSimulator.StaticSend("sender@example.test", strict.Address, "first", BodyOfAbout(600));
            Pop3ClientSimulator.AssertMessageCount(strict.Address, "test", 1);
            SmtpClientSimulator.StaticSend("sender@example.test", strict.Address, "second", BodyOfAbout(600));
            CustomAsserts.AssertRecipientsInDeliveryQueue(0);
            Pop3ClientSimulator.AssertMessageCount(strict.Address, "test", 1);
         }
         finally
         {
            ResetPolicy_();
         }
      }

      [Test]
      [Description("Sending stops before receiving does: past the domain's send-stop share the account's authenticated MAIL FROM is refused with the reason, while mail to it still arrives; the server's value applies where the domain has none, and 0 never stops anyone")]
      public void SendingStopsBeforeReceivingDoes()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, Mailbox, "test");
         account.MaxSize = 1;
         account.Save();

         _domain.QuotaSendStopPercent = 50;
         _domain.Save();
         try
         {
            // 600 KB of 1024 is 58%: past the domain's 50.
            SmtpClientSimulator.StaticSend("sender@example.test", Mailbox, "first", BodyOfAbout(600));
            Pop3ClientSimulator.AssertMessageCount(Mailbox, "test", 1);

            var refused = Assert.Throws<DeliveryFailedException>(() =>
               new SmtpClientSimulator().Send(false, Mailbox, "test", Mailbox, Mailbox, "out", "body", out string _));
            StringAssert.Contains("552", refused.Message, "A hard refusal of the sender, not a retry-later. Got: " + refused.Message);
            StringAssert.Contains("Sending is stopped", refused.Message, "The reason, so the owner knows what to do. Got: " + refused.Message);
            StringAssert.Contains("50%", refused.Message, "The domain's own point. Got: " + refused.Message);

            // Receiving goes on: this is not a full mailbox.
            SmtpClientSimulator.StaticSend("sender@example.test", Mailbox, "second", BodyOfAbout(50));
            Pop3ClientSimulator.AssertMessageCount(Mailbox, "test", 2);

            // The server's value where the domain has none, and 0 stops nobody.
            _domain.QuotaSendStopPercent = -1;
            _domain.Save();
            _settings.QuotaSendStopPercent = 40;
            refused = Assert.Throws<DeliveryFailedException>(() =>
               new SmtpClientSimulator().Send(false, Mailbox, "test", Mailbox, Mailbox, "out", "body", out string _));
            StringAssert.Contains("40%", refused.Message, "The server's point. Got: " + refused.Message);

            _settings.QuotaSendStopPercent = 0;
            new SmtpClientSimulator().Send(false, Mailbox, "test", Mailbox, Mailbox, "out", "body", out string _);
            Pop3ClientSimulator.AssertMessageCount(Mailbox, "test", 3);
         }
         finally
         {
            ResetPolicy_();
         }
      }

      [Test]
      [Description("QuotaWarningPercent=0 sends nothing at all, so an operator who does not want the server writing to their users can say so")]
      public void TheWarningCanBeSwitchedOff()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, Mailbox, "test");
         account.MaxSize = 1;
         account.Save();

         try
         {
            ServerIniFile.SetSetting("QuotaWarningPercent", "0");
            RestartServerAndReacquireCom();

            // The same two messages that produced a warning above.
            SmtpClientSimulator.StaticSend("sender@example.test", Mailbox, "first", BodyOfAbout(300));
            SmtpClientSimulator.StaticSend("sender@example.test", Mailbox, "second", BodyOfAbout(300));

            Pop3ClientSimulator.AssertMessageCount(Mailbox, "test", 2);

            ClassicAssert.AreEqual(2, MessagesIn(Mailbox),
               "With the warning disabled the mailbox should hold the two delivered messages and " +
               "nothing else.");
         }
         finally
         {
            ServerIniFile.SetSetting("QuotaWarningPercent", null);
            RestartServerAndReacquireCom();
         }
      }
   }
}
