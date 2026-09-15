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

         _settings.BounceProcessingEnabled = true;
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
         _settings.BounceHardBounceThreshold = 2;

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

         _settings.BounceReadDeliveryReports = false;

         string unrecorded = NewMessageId();
         DeliverOriginal(sender.Address, Remote, unrecorded);

         // Reading is switched back on before the report arrives, so the one reason
         // left for ignoring it is that no key was written while it was off.
         _settings.BounceReadDeliveryReports = true;

         InjectReport(sender.Address, Remote, "failed", "5.1.1", unrecorded);

         Assert.AreEqual(0, BounceTestSupport.Number(Read(DomainPath + "/bounces"), "total"),
            "A key was recorded for a delivery made while reading reports was off.");

         string recorded = NewMessageId();
         DeliverOriginal(sender.Address, Remote, recorded);
         InjectReport(sender.Address, Remote, "failed", "5.1.1", recorded);

         Assert.AreEqual(1, BounceTestSupport.Number(BounceTestSupport.AssertItemFor(Read(DomainPath + "/bounces"), Remote), "hard"));
      }

      [Test]
      [Description("This server's own non-delivery report, arriving in the sender's mailbox with a null sender, is not read as a second bounce.")]
      public void OurOwnNonDeliveryReportIsNotCountedTwice()
      {
         _settings.BounceHardBounceThreshold = 2;

         Account sender = NewSender();

         BounceTestSupport.DeliverThroughSimulator(sender.Address, Remote, 550,
            "5.1.1 <" + Remote + ">: Recipient address rejected: User unknown");

         string report = Pop3ClientSimulator.AssertGetFirstMessageText(sender.Address, Password);
         StringAssert.Contains("report-type=delivery-status", report, "The sender received this server's own report.");

         string row = BounceTestSupport.AssertItemFor(Read(DomainPath + "/bounces"), Remote);
         Assert.AreEqual(1, BounceTestSupport.Number(row, "hard"), "This server's own report was counted as a second bounce: " + row);
         Assert.AreEqual(0, BounceTestSupport.Number(Read(DomainPath + "/suppressions"), "total"));
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
      ///    A report, sent over plain SMTP with a null envelope sender as a remote MTA
      ///    sends one, and taken out of the sender's mailbox again once it has been
      ///    delivered - which every report must be, counted or not.
      /// </summary>
      private static void InjectReport(string reportTo, string finalRecipient, string action, string status, string originalMessageId)
      {
         SmtpClientSimulator.StaticSendRaw("", reportTo, ReportText(reportTo, finalRecipient, action, status, originalMessageId));

         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         string delivered = Pop3ClientSimulator.AssertGetFirstMessageText(reportTo, Password);
         StringAssert.Contains("report-type=delivery-status", delivered, "The report was not delivered to the sender unchanged.");
         StringAssert.Contains("Final-Recipient: rfc822; " + finalRecipient, delivered);
      }

      /// <summary>
      ///    A Postfix-shaped RFC 3464 report: the human-readable part, the
      ///    message/delivery-status part with one recipient group, and the original
      ///    message's headers as text/rfc822-headers.
      /// </summary>
      private static string ReportText(string reportTo, string finalRecipient, string action, string status, string originalMessageId)
      {
         const string Boundary = "report-boundary-4F1A2B";
         string reportingMta = "mx." + BounceTestSupport.RemoteDomain;

         string diagnostic = action == "failed"
            ? "Diagnostic-Code: smtp; 550 5.1.1 <" + finalRecipient + ">: Recipient address rejected: User unknown\r\n"
            : "";

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
