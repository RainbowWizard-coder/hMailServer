// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Threading;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.SMTP
{
   /// <summary>
   ///    The second way a bounce arrives: an RFC 3464 delivery report, sent back to
   ///    one of this server's senders some time after a remote server accepted the
   ///    message and then failed to deliver it.
   ///
   ///    This is the half that invites forgery. Anybody can send a multipart/report
   ///    naming any address, and a server that believed them would let a stranger
   ///    put a colleague's correspondent on a suppression list with one message. So
   ///    most of these tests are about reports that must NOT count: one for a
   ///    message this server never sent, one naming a recipient the message never
   ///    went to, the same genuine report a second time, a report that only says
   ///    the message is delayed, one submitted over an authenticated session, and
   ///    this server's own non-delivery report arriving in the sender's mailbox. Each
   ///    of those is followed by the genuine report and required to count, so every
   ///    "nothing happened" here is shown to be a test that could see something
   ///    happen. And every report, counted or not, is delivered to the sender
   ///    unchanged.
   /// </summary>
   [TestFixture]
   public class BounceReports : TestFixtureBase
   {
      private static int RestPort = 9553;

      private const string Password = "test";
      private const string Remote = "gone@" + BounceTestSupport.RemoteDomain;

      private static int senderSequence_;

      private Account NewSender()
      {
         int sequence = Interlocked.Increment(ref senderSequence_);
         return SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "report-sender-" + sequence + "@" + _domain.Name, Password);
      }

      private string DomainPath
      {
         get { return "/api/v1/domains/" + _domain.Name; }
      }

      private string NewMessageId()
      {
         return "original-" + Guid.NewGuid().ToString("N") + "@" + _domain.Name;
      }

      [SetUp]
      public void StartRestApiAndEnableProcessing()
      {
         _settings.SetAdministratorPassword(BounceTestSupport.AdminPassword);

         RestPort = RestListener.Start(RestPort);
         IniFileSetting.Write("RestApiCertificateFile", "");
         IniFileSetting.Write("RestApiPrivateKeyFile", "");

         _application.Reinitialize();

         (int status, string body) probe = BounceTestSupport.Http(RestPort, "GET", "/api/v1/status");
         Assert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);

         BounceTestSupport.ComSetBounceProcessingEnabled(_settings, true);
      }

      [TearDown]
      public void RestoreSettingsAndStopRestApi()
      {
         try
         {
            BounceTestSupport.RestoreDefaults(_settings);
         }
         finally
         {
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      [Test]
      [Description("A genuine report - a null sender, over SMTP, for a message a remote accepted from this server, naming the recipient it went to - is counted as that recipient's bounce, lists it with a source of report, and is still delivered to the sender.")]
      public void AGenuineDeliveryReportIsCounted()
      {
         Account sender = NewSender();
         string messageId = NewMessageId();

         DeliverOriginal(sender.Address, Remote, messageId);

         InjectReport(sender.Address, Remote, "failed", "5.1.1", messageId);

         string row = BounceTestSupport.AssertItemFor(Read(DomainPath + "/bounces"), Remote);
         Assert.AreEqual(1, BounceTestSupport.Number(row, "hard"), row);

         string item = BounceTestSupport.AssertItemFor(Read(DomainPath + "/suppressions"), Remote);
         Assert.AreEqual("report", BounceTestSupport.Text(item, "source"), item);
         Assert.AreEqual("hard", BounceTestSupport.Text(item, "category"), item);
         Assert.AreEqual("5.1.1", BounceTestSupport.Text(item, "status"), item);
         StringAssert.Contains("User unknown", BounceTestSupport.Text(item, "diagnostic"), item);
         Assert.AreEqual("mx." + BounceTestSupport.RemoteDomain, BounceTestSupport.Text(item, "remote_mta"), item);
      }

      [Test]
      [Description("A report quoting a Message-ID this server never sent is delivered and ignored - and the genuine one after it counts, so the test can fail.")]
      public void AForgedReportForAMessageNeverSentIsIgnored()
      {
         Account sender = NewSender();
         string messageId = NewMessageId();

         DeliverOriginal(sender.Address, Remote, messageId);

         InjectReport(sender.Address, Remote, "failed", "5.1.1", NewMessageId());

         Assert.AreEqual(0, BounceTestSupport.Number(Read(DomainPath + "/bounces"), "total"),
            "A report for a message this server never sent was counted. That is the forged backscatter the register exists to refuse.");
         Assert.AreEqual(0, BounceTestSupport.Number(Read(DomainPath + "/suppressions"), "total"));

         InjectReport(sender.Address, Remote, "failed", "5.1.1", messageId);

         Assert.AreEqual(1, BounceTestSupport.Number(BounceTestSupport.AssertItemFor(Read(DomainPath + "/bounces"), Remote), "hard"),
            "The genuine report did not count, so the first half of this test proved nothing.");
      }

      [Test]
      [Description("The right Message-ID with a Final-Recipient the message never went to is ignored, and does not use up the key the genuine report needs.")]
      public void AReportNamingARecipientTheMessageWasNotSentToIsIgnored()
      {
         Account sender = NewSender();
         string messageId = NewMessageId();
         string bystander = "bystander@" + BounceTestSupport.RemoteDomain;

         DeliverOriginal(sender.Address, Remote, messageId);

         InjectReport(sender.Address, bystander, "failed", "5.1.1", messageId);

         Assert.AreEqual(0, BounceTestSupport.Number(Read(DomainPath + "/bounces"), "total"),
            "A report naming an address the message never went to was counted.");

         InjectReport(sender.Address, Remote, "failed", "5.1.1", messageId);

         string counts = Read(DomainPath + "/bounces");
         Assert.AreEqual(1, BounceTestSupport.Number(BounceTestSupport.AssertItemFor(counts, Remote), "hard"), counts);
         Assert.IsNull(BounceTestSupport.ItemFor(counts, bystander), counts);
      }

      [Test]
      [Description("The same genuine report twice counts once: a matching key is consumed.")]
      public void TheSameReportTwiceCountsOnce()
      {
         BounceTestSupport.ComSetBounceHardBounceThreshold(_settings, 2);

         Account sender = NewSender();
         string messageId = NewMessageId();

         DeliverOriginal(sender.Address, Remote, messageId);

         InjectReport(sender.Address, Remote, "failed", "5.1.1", messageId);
         InjectReport(sender.Address, Remote, "failed", "5.1.1", messageId);

         string row = BounceTestSupport.AssertItemFor(Read(DomainPath + "/bounces"), Remote);
         Assert.AreEqual(1, BounceTestSupport.Number(row, "hard"), "A replayed report was counted again: " + row);
         Assert.AreEqual(0, BounceTestSupport.Number(Read(DomainPath + "/suppressions"), "total"),
            "A threshold of two was reached by replaying one report.");
      }

      [Test]
      [Description("Action: delayed is not a bounce: nothing is counted, and the key is still there for the failure that follows.")]
      public void ADelayedReportIsNotABounce()
      {
         Account sender = NewSender();
         string messageId = NewMessageId();

         DeliverOriginal(sender.Address, Remote, messageId);

         InjectReport(sender.Address, Remote, "delayed", "4.4.7", messageId);

         Assert.AreEqual(0, BounceTestSupport.Number(Read(DomainPath + "/bounces"), "total"),
            "A report that the message is delayed was counted as a bounce.");

         InjectReport(sender.Address, Remote, "failed", "5.1.1", messageId);

         Assert.AreEqual(1, BounceTestSupport.Number(BounceTestSupport.AssertItemFor(Read(DomainPath + "/bounces"), Remote), "hard"));
      }

      [Test]
      [Description("With BounceReadDeliveryReports off a delivery records no key, so a report about it matches nothing even once reading is back on; with it on the same sequence counts.")]
      public void ReadingReportsOffIgnoresThemAndRecordsNoKeys()
      {
         Account sender = NewSender();

         BounceTestSupport.ComSetBounceReadDeliveryReports(_settings, false);

         string unrecorded = NewMessageId();
         DeliverOriginal(sender.Address, Remote, unrecorded);

         // Reading is switched back on before the report arrives, so the one reason
         // left for ignoring it is that no key was written while it was off.
         BounceTestSupport.ComSetBounceReadDeliveryReports(_settings, true);

         InjectReport(sender.Address, Remote, "failed", "5.1.1", unrecorded);

         Assert.AreEqual(0, BounceTestSupport.Number(Read(DomainPath + "/bounces"), "total"),
            "A key was recorded for a delivery made while reading reports was off.");

         string recorded = NewMessageId();
         DeliverOriginal(sender.Address, Remote, recorded);
         InjectReport(sender.Address, Remote, "failed", "5.1.1", recorded);

         Assert.AreEqual(1, BounceTestSupport.Number(BounceTestSupport.AssertItemFor(Read(DomainPath + "/bounces"), Remote), "hard"));
      }

      [Test]
      [Description("This server's own non-delivery report, arriving in the sender's mailbox with a null sender and quoting a message whose report key really exists, is not read as a second bounce - and the genuine report after it still finds that key and counts.")]
      public void OurOwnNonDeliveryReportIsNotCountedTwice()
      {
         BounceTestSupport.ComSetBounceHardBounceThreshold(_settings, 2);

         Account sender = NewSender();
         string messageId = NewMessageId();

         // First the message is accepted, which writes the key for this Message-ID,
         // recipient and sender. Without it this server's own report could match
         // nothing, and the test would pass with the check it is about removed.
         DeliverOriginal(sender.Address, Remote, messageId);

         // Then the same message - the same Message-ID, sender and recipient - is
         // refused with the remote's own 5.1.1. That is one hard bounce, and it makes
         // this server write its own report, which quotes the Message-ID and names
         // the recipient: everything the key needs.
         DeliverWithMessageId(sender.Address, Remote, messageId, 550,
            "5.1.1 <" + Remote + ">: Recipient address rejected: User unknown");

         string ownReport = Pop3ClientSimulator.AssertGetFirstMessageText(sender.Address, Password);
         StringAssert.Contains("report-type=delivery-status", ownReport, "The sender did not receive this server's own report.");
         StringAssert.Contains("Final-Recipient: rfc822; " + Remote, ownReport, ownReport);
         StringAssert.Contains("Message-ID: <" + messageId + ">", ownReport,
            "This server's own report does not quote the original Message-ID, so it could not have matched the key and the test proves nothing.");

         string row = BounceTestSupport.AssertItemFor(Read(DomainPath + "/bounces"), Remote);
         Assert.AreEqual(1, BounceTestSupport.Number(row, "hard"), "This server's own report was counted as a second bounce: " + row);

         // The key was not used up by the report that was ignored: the genuine one
         // finds it.
         InjectReport(sender.Address, Remote, "failed", "5.1.1", messageId);

         Assert.AreEqual(2, BounceTestSupport.Number(BounceTestSupport.AssertItemFor(Read(DomainPath + "/bounces"), Remote), "hard"),
            "The genuine report did not count, so the key was gone - consumed by this server's own report, which must never be read.");
      }

      [Test]
      [Description("A threshold above one is reached through reports: an acceptance sets the run of hard bounces aside rather than ending it, and the report that the message failed gives it back.")]
      public void AThresholdAboveOneIsReachedThroughReports()
      {
         BounceTestSupport.ComSetBounceHardBounceThreshold(_settings, 2);

         Account sender = NewSender();

         string first = NewMessageId();
         DeliverOriginal(sender.Address, Remote, first);
         InjectReport(sender.Address, Remote, "failed", "5.1.1", first);

         string row = BounceTestSupport.AssertItemFor(Read(DomainPath + "/bounces"), Remote);
         Assert.AreEqual(1, BounceTestSupport.Number(row, "consecutive_hard"), row);
         Assert.AreEqual(0, BounceTestSupport.Number(Read(DomainPath + "/suppressions"), "total"), "One hard bounce is below a threshold of two.");

         // The next message is accepted too - which is all a server behind a smart
         // host ever sees - and its report is the second hard bounce in a row.
         string second = NewMessageId();
         DeliverOriginal(sender.Address, Remote, second);
         InjectReport(sender.Address, Remote, "failed", "5.1.1", second);

         row = BounceTestSupport.AssertItemFor(Read(DomainPath + "/bounces"), Remote);
         Assert.AreEqual(2, BounceTestSupport.Number(row, "hard"), row);
         Assert.AreEqual(2, BounceTestSupport.Number(row, "consecutive_hard"),
            "The second message's acceptance ended the run its own report belonged to: " + row);

         string item = BounceTestSupport.AssertItemFor(Read(DomainPath + "/suppressions"), Remote);
         Assert.AreEqual("report", BounceTestSupport.Text(item, "source"), item);
      }

      [Test]
      [Description("A message that was accepted and never reported is a delivery: it ends the run, so the hard bounce reported after it is one in a row, not two - and two reported in a row after that do list the address.")]
      public void AnAcceptanceNoReportContradictsEndsTheRun()
      {
         BounceTestSupport.ComSetBounceHardBounceThreshold(_settings, 2);

         Account sender = NewSender();

         string first = NewMessageId();
         DeliverOriginal(sender.Address, Remote, first);
         InjectReport(sender.Address, Remote, "failed", "5.1.1", first);

         // Delivered, and no report ever comes for it.
         DeliverOriginal(sender.Address, Remote, NewMessageId());

         string third = NewMessageId();
         DeliverOriginal(sender.Address, Remote, third);
         InjectReport(sender.Address, Remote, "failed", "5.1.1", third);

         string row = BounceTestSupport.AssertItemFor(Read(DomainPath + "/bounces"), Remote);
         Assert.AreEqual(2, BounceTestSupport.Number(row, "hard"), row);
         Assert.AreEqual(1, BounceTestSupport.Number(row, "consecutive_hard"),
            "A message delivered between two hard bounces did not end the run: " + row);
         Assert.AreEqual(0, BounceTestSupport.Number(Read(DomainPath + "/suppressions"), "total"),
            "An address a message was delivered to between two hard bounces was listed at a threshold of two.");

         // The control: two in a row from here are two.
         string fourth = NewMessageId();
         DeliverOriginal(sender.Address, Remote, fourth);
         InjectReport(sender.Address, Remote, "failed", "5.1.1", fourth);

         Assert.AreEqual(2, BounceTestSupport.Number(BounceTestSupport.AssertItemFor(Read(DomainPath + "/bounces"), Remote), "consecutive_hard"));
         BounceTestSupport.AssertItemFor(Read(DomainPath + "/suppressions"), Remote);
      }

      [Test]
      [Description("Two messages accepted before either report arrives: the first report carries its bounce forward onto the second message, and the second report brings both into the run.")]
      public void ReportsForMessagesSentBeforeEitherReportArrivesStillAddUp()
      {
         BounceTestSupport.ComSetBounceHardBounceThreshold(_settings, 2);

         Account sender = NewSender();

         string first = NewMessageId();
         string second = NewMessageId();
         DeliverOriginal(sender.Address, Remote, first);
         DeliverOriginal(sender.Address, Remote, second);

         InjectReport(sender.Address, Remote, "failed", "5.1.1", first);

         string row = BounceTestSupport.AssertItemFor(Read(DomainPath + "/bounces"), Remote);
         Assert.AreEqual(1, BounceTestSupport.Number(row, "hard"), row);
         Assert.AreEqual(0, BounceTestSupport.Number(Read(DomainPath + "/suppressions"), "total"),
            "The first report listed the address although a later message to it had been accepted and might have been delivered.");

         InjectReport(sender.Address, Remote, "failed", "5.1.1", second);

         row = BounceTestSupport.AssertItemFor(Read(DomainPath + "/bounces"), Remote);
         Assert.AreEqual(2, BounceTestSupport.Number(row, "consecutive_hard"),
            "The first report's bounce was not carried forward onto the second message: " + row);
         BounceTestSupport.AssertItemFor(Read(DomainPath + "/suppressions"), Remote);
      }

      [Test]
      [Description("An entry removed by hand starts the run again for a report still on its way, too: the run that report's message set aside is forgotten, so it adds one - and two in a row after that list the address again.")]
      public void RemovingAnEntryForgetsTheRunAPendingReportWouldGiveBack()
      {
         BounceTestSupport.ComSetBounceHardBounceThreshold(_settings, 2);

         Account sender = NewSender();

         string first = NewMessageId();
         DeliverOriginal(sender.Address, Remote, first);
         InjectReport(sender.Address, Remote, "failed", "5.1.1", first);

         // Accepted with a run of one set aside, and then the administrator lists
         // and removes the address before that message's report comes.
         string second = NewMessageId();
         DeliverOriginal(sender.Address, Remote, second);

         Assert.AreEqual(201, BounceTestSupport.Http(RestPort, "POST", DomainPath + "/suppressions", "{\"address\":\"" + Remote + "\"}").status);
         Assert.AreEqual(200, BounceTestSupport.Http(RestPort, "DELETE", DomainPath + "/suppressions/" + Uri.EscapeDataString(Remote)).status);

         InjectReport(sender.Address, Remote, "failed", "5.1.1", second);

         Assert.AreEqual(1, BounceTestSupport.Number(BounceTestSupport.AssertItemFor(Read(DomainPath + "/bounces"), Remote), "consecutive_hard"),
            "A report about a message accepted before the removal gave back the run as it stood before the removal.");
         Assert.AreEqual(0, BounceTestSupport.Number(Read(DomainPath + "/suppressions"), "total"),
            "The address was listed again by one hard bounce after it was removed, at a threshold of two.");

         string third = NewMessageId();
         DeliverOriginal(sender.Address, Remote, third);
         InjectReport(sender.Address, Remote, "failed", "5.1.1", third);

         BounceTestSupport.AssertItemFor(Read(DomainPath + "/suppressions"), Remote);
      }

      [Test]
      [Description("A report whose Status says 5.1.1 beside a bare '550 <address>' - which is how this server's own reports, and any MTA that derives its code the same way, write one - is counted as other and never lists the address; a report carrying the remote's own 5.1.1 does.")]
      public void AReportWhoseStatusWasDerivedFromABareReplyIsNotTrusted()
      {
         Account sender = NewSender();

         string first = NewMessageId();
         DeliverOriginal(sender.Address, Remote, first);
         InjectReport(sender.Address, Remote, "failed", "5.1.1", first, "smtp; 550 " + Remote);

         string row = BounceTestSupport.AssertItemFor(Read(DomainPath + "/bounces"), Remote);
         Assert.AreEqual(1, BounceTestSupport.Number(row, "other"), row);
         Assert.AreEqual(0, BounceTestSupport.Number(row, "hard"), row);
         Assert.AreEqual(0, BounceTestSupport.Number(Read(DomainPath + "/suppressions"), "total"),
            "A Status a reporting server derived from a bare 550 was trusted as a dead mailbox.");

         string second = NewMessageId();
         DeliverOriginal(sender.Address, Remote, second);
         InjectReport(sender.Address, Remote, "failed", "5.1.1", second);

         Assert.AreEqual(1, BounceTestSupport.Number(BounceTestSupport.AssertItemFor(Read(DomainPath + "/bounces"), Remote), "hard"),
            "The report with the remote's own 5.1.1 was not counted, so the first half of this test proved nothing.");
         BounceTestSupport.AssertItemFor(Read(DomainPath + "/suppressions"), Remote);
      }

      [Test]
      [Description("A null-sender message addressed to two of this server's addresses is not read as a report - a report goes to one envelope sender - and the same report to the sender alone counts.")]
      public void AReportAddressedToTwoLocalAddressesIsNotRead()
      {
         Account sender = NewSender();
         Account bystander = NewSender();
         string messageId = NewMessageId();

         DeliverOriginal(sender.Address, Remote, messageId);

         SendNullSenderToTwo(sender.Address, bystander.Address, ReportText(sender.Address, Remote, "failed", "5.1.1", messageId));
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         Pop3ClientSimulator.AssertGetFirstMessageText(sender.Address, Password);
         Pop3ClientSimulator.AssertGetFirstMessageText(bystander.Address, Password);

         Assert.AreEqual(0, BounceTestSupport.Number(Read(DomainPath + "/bounces"), "total"),
            "A null-sender message to two local addresses was read as a delivery report.");

         InjectReport(sender.Address, Remote, "failed", "5.1.1", messageId);

         Assert.AreEqual(1, BounceTestSupport.Number(BounceTestSupport.AssertItemFor(Read(DomainPath + "/bounces"), Remote), "hard"),
            "The same report to the sender alone did not count, so the first half of this test proved nothing.");
      }

      [Test]
      [Description("A report submitted over an authenticated session - which a remote MTA's never is - is ignored; the same report over plain SMTP counts.")]
      public void AReportSentOverAnAuthenticatedSessionIsIgnored()
      {
         Account sender = NewSender();
         string messageId = NewMessageId();

         DeliverOriginal(sender.Address, Remote, messageId);

         new SmtpClientSimulator().SendRaw(sender.Address, Password, "", sender.Address,
            ReportText(sender.Address, Remote, "failed", "5.1.1", messageId));
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         Pop3ClientSimulator.AssertGetFirstMessageText(sender.Address, Password);

         Assert.AreEqual(0, BounceTestSupport.Number(Read(DomainPath + "/bounces"), "total"),
            "A report a signed-in user submitted was believed.");

         InjectReport(sender.Address, Remote, "failed", "5.1.1", messageId);

         Assert.AreEqual(1, BounceTestSupport.Number(BounceTestSupport.AssertItemFor(Read(DomainPath + "/bounces"), Remote), "hard"),
            "The key was used up by the report that was ignored, or the genuine report was not read.");
      }

      // ---- helpers -----------------------------------------------------------

      /// <summary>
      ///    One message from sender to recipient, carrying this Message-ID, delivered
      ///    through this server to a simulator that accepts it - which is when the
      ///    server writes the key a later report has to match.
      /// </summary>
      private static void DeliverOriginal(string sender, string recipient, string messageId)
      {
         int port = TestSetup.GetNextFreePort();

         using (var server = new SmtpServerSimulator(1, port))
         {
            server.AddRecipientResult(new Dictionary<string, int> { [recipient] = 250 });
            server.StartListen();
            BounceTestSupport.RouteTo(port);

            SmtpClientSimulator.StaticSendRaw(sender, recipient,
               "From: " + sender + "\r\n" +
               "To: " + recipient + "\r\n" +
               "Subject: The original\r\n" +
               "Message-ID: <" + messageId + ">\r\n" +
               "\r\n" +
               "Body text.\r\n");

            server.WaitForCompletion();

            StringAssert.Contains("Message-ID: <" + messageId + ">", server.MessageData,
               "The remote server did not receive the message with the Message-ID the report will quote.");
         }

         CustomAsserts.AssertRecipientsInDeliveryQueue(0, true);
      }

      /// <summary>
      ///    The same message as DeliverOriginal - this Message-ID - delivered to a
      ///    simulator that answers RCPT TO with this code and reply text. Nothing is
      ///    asserted about what the simulator received, since a refused recipient
      ///    gets no DATA.
      /// </summary>
      private static void DeliverWithMessageId(string sender, string recipient, string messageId, int code, string replyText)
      {
         int port = TestSetup.GetNextFreePort();

         using (var server = new SmtpServerSimulator(1, port))
         {
            server.AddRecipientResult(new Dictionary<string, int> { [recipient] = code });
            server.RecipientReplyTexts[recipient] = replyText;
            server.StartListen();
            BounceTestSupport.RouteTo(port);

            SmtpClientSimulator.StaticSendRaw(sender, recipient,
               "From: " + sender + "\r\n" +
               "To: " + recipient + "\r\n" +
               "Subject: The original\r\n" +
               "Message-ID: <" + messageId + ">\r\n" +
               "\r\n" +
               "Body text.\r\n");

            server.WaitForCompletion();
         }

         CustomAsserts.AssertRecipientsInDeliveryQueue(0, true);
      }

      /// <summary>
      ///    A report, sent over plain SMTP with a null envelope sender as a remote MTA
      ///    sends one, and taken out of the sender's mailbox again once it has been
      ///    delivered - which every report must be, counted or not. diagnosticCode,
      ///    when given, is the Diagnostic-Code field's value in place of the remote's
      ///    own 550 5.1.1.
      /// </summary>
      private static void InjectReport(string reportTo, string finalRecipient, string action, string status, string originalMessageId,
                                       string diagnosticCode = null)
      {
         SmtpClientSimulator.StaticSendRaw("", reportTo, ReportText(reportTo, finalRecipient, action, status, originalMessageId, diagnosticCode));

         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         string delivered = Pop3ClientSimulator.AssertGetFirstMessageText(reportTo, Password);
         StringAssert.Contains("report-type=delivery-status", delivered, "The report was not delivered to the sender unchanged.");
         StringAssert.Contains("Final-Recipient: rfc822; " + finalRecipient, delivered);
      }

      /// <summary>
      ///    One message with a null envelope sender and two RCPT TOs, over plain SMTP.
      /// </summary>
      private static void SendNullSenderToTwo(string firstRecipient, string secondRecipient, string text)
      {
         using (var socket = new TcpConnection())
         {
            Assert.IsTrue(socket.Connect(25), "Could not connect to the SMTP server.");
            StringAssert.StartsWith("220", socket.Receive());

            StringAssert.StartsWith("250", socket.SendAndReceive("HELO example.test\r\n"));
            StringAssert.StartsWith("250", socket.SendAndReceive("MAIL FROM:<>\r\n"));
            StringAssert.StartsWith("250", socket.SendAndReceive("RCPT TO:<" + firstRecipient + ">\r\n"));
            StringAssert.StartsWith("250", socket.SendAndReceive("RCPT TO:<" + secondRecipient + ">\r\n"));
            StringAssert.StartsWith("354", socket.SendAndReceive("DATA\r\n"));

            socket.Send(text);
            if (!text.EndsWith("\r\n"))
               socket.Send("\r\n");

            StringAssert.StartsWith("250", socket.SendAndReceive(".\r\n"));
            socket.SendAndReceive("QUIT\r\n");
         }
      }

      /// <summary>
      ///    A Postfix-shaped RFC 3464 report: the human-readable part, the
      ///    message/delivery-status part with one recipient group, and the original
      ///    message's headers as text/rfc822-headers.
      /// </summary>
      private static string ReportText(string reportTo, string finalRecipient, string action, string status, string originalMessageId,
                                       string diagnosticCode = null)
      {
         const string Boundary = "report-boundary-4F1A2B";
         string reportingMta = "mx." + BounceTestSupport.RemoteDomain;

         string diagnostic = action != "failed"
            ? ""
            : "Diagnostic-Code: " + (diagnosticCode ?? ("smtp; 550 5.1.1 <" + finalRecipient + ">: Recipient address rejected: User unknown")) + "\r\n";

         return
            "From: Mail Delivery System <MAILER-DAEMON@" + reportingMta + ">\r\n" +
            "To: " + reportTo + "\r\n" +
            "Subject: Undelivered Mail Returned to Sender\r\n" +
            "Auto-Submitted: auto-replied\r\n" +
            "MIME-Version: 1.0\r\n" +
            "Content-Type: multipart/report; report-type=delivery-status;\r\n" +
            "\tboundary=\"" + Boundary + "\"\r\n" +
            "\r\n" +
            "This is a MIME-encapsulated message.\r\n" +
            "\r\n" +
            "--" + Boundary + "\r\n" +
            "Content-Description: Notification\r\n" +
            "Content-Type: text/plain; charset=us-ascii\r\n" +
            "\r\n" +
            "Your message could not be delivered to one or more recipients.\r\n" +
            "\r\n" +
            "--" + Boundary + "\r\n" +
            "Content-Description: Delivery report\r\n" +
            "Content-Type: message/delivery-status\r\n" +
            "\r\n" +
            "Reporting-MTA: dns; " + reportingMta + "\r\n" +
            "\r\n" +
            "Final-Recipient: rfc822; " + finalRecipient + "\r\n" +
            "Action: " + action + "\r\n" +
            "Status: " + status + "\r\n" +
            "Remote-MTA: dns; " + reportingMta + "\r\n" +
            diagnostic +
            "\r\n" +
            "--" + Boundary + "\r\n" +
            "Content-Description: Undelivered Message Headers\r\n" +
            "Content-Type: text/rfc822-headers\r\n" +
            "\r\n" +
            "From: " + reportTo + "\r\n" +
            "To: " + finalRecipient + "\r\n" +
            "Subject: The original\r\n" +
            "Message-ID: <" + originalMessageId + ">\r\n" +
            "\r\n" +
            "--" + Boundary + "--\r\n";
      }

      private static string Read(string path)
      {
         return BounceTestSupport.Read(RestPort, path);
      }
   }
}
