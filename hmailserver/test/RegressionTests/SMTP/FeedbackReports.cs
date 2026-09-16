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
   ///    Complaints read: the feedback loops. A provider posts an RFC 5965 abuse
   ///    report to the address the operator registered with it when one of its
   ///    readers presses the button that says this is spam.
   ///
   ///    The whole of what makes a report believable here is that it arrived at the
   ///    configured mailbox, so most of these tests are about reports that must NOT
   ///    count: one to an ordinary mailbox, one about a domain this server does not
   ///    host, the same report a second time, a not-spam report, and one that is not
   ///    a report at all. Each is followed by - or preceded by - the genuine one and
   ///    required to count, so every "nothing happened" here is shown to be a test
   ///    that could see something happen. And every report, counted or not, is
   ///    delivered to the mailbox unchanged, which each test reads it out of.
   /// </summary>
   [TestFixture]
   public class FeedbackReports : TestFixtureBase
   {
      private static int RestPort = 9554;

      private const string Password = "test";
      private const string Reader = "reader@" + BounceTestSupport.RemoteDomain;

      private static int sequence_;

      private Account mailbox_;

      private string Mailbox
      {
         get { return mailbox_.Address; }
      }

      private string DomainPath
      {
         get { return "/api/v1/domains/" + _domain.Name; }
      }

      private Account NewSender()
      {
         int sequence = Interlocked.Increment(ref sequence_);
         return SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "complaint-sender-" + sequence + "@" + _domain.Name, Password);
      }

      private string NewMessageId()
      {
         return "complained-" + Guid.NewGuid().ToString("N") + "@" + _domain.Name;
      }

      [SetUp]
      public void StartRestApiAndNameAMailbox()
      {
         _settings.SetAdministratorPassword(BounceTestSupport.AdminPassword);

         RestPort = RestListener.Start(RestPort);
         IniFileSetting.Write("RestApiCertificateFile", "");
         IniFileSetting.Write("RestApiPrivateKeyFile", "");

         _application.Reinitialize();

         (int status, string body) probe = BounceTestSupport.Http(RestPort, "GET", "/api/v1/status");
         Assert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);

         mailbox_ = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "fbl@" + _domain.Name, Password);

         ComplaintTestSupport.ComSetComplaintProcessingEnabled(_settings, true);
         ComplaintTestSupport.ComSetComplaintMailbox(_settings, Mailbox);

         // The shipped default is 0 - record and count, list nobody - which is the
         // observe-only setting an operator starts with, and RestApiComplaints proves
         // that it is what a fresh server has. This fixture is about what the feature
         // does when it is allowed to act, so it says so rather than relying on a
         // default; AThresholdOfZeroListsNobody puts it back to 0 for itself.
         ComplaintTestSupport.ComSetComplaintSuppressionThreshold(_settings, 1);
      }

      [TearDown]
      public void RestoreSettingsAndStopRestApi()
      {
         try
         {
            ComplaintTestSupport.RestoreDefaults(_settings);
            BounceTestSupport.RestoreDefaults(_settings);
         }
         finally
         {
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      [Test]
      [Description("A report delivered to the complaint mailbox is parsed, recorded against the sending domain and the sending account with every field the row asks for, and the message is still delivered unchanged.")]
      public void AReportToTheMailboxIsParsedAndRecorded()
      {
         Account sender = NewSender();
         string messageId = NewMessageId();

         InjectReport(ComplaintTestSupport.ReportText(Mailbox, "abuse", sender.Address, Reader, messageId, "Our newsletter"));

         string item = BounceTestSupport.AssertItemFor(Read(DomainPath + "/complaints"), Reader);

         Assert.AreEqual(sender.Address.ToLowerInvariant(), BounceTestSupport.Text(item, "sender"), item);
         Assert.AreEqual((long) sender.ID, BounceTestSupport.Number(item, "account_id"), item);
         Assert.AreEqual("abuse", BounceTestSupport.Text(item, "feedback_type"), item);
         Assert.IsTrue(BounceTestSupport.Flag(item, "counts"), item);
         Assert.AreEqual("SomeGenerator/1.0", BounceTestSupport.Text(item, "provider"), item);
         Assert.AreEqual("192.0.2.1", BounceTestSupport.Text(item, "source_ip"), item);
         Assert.AreEqual(messageId.ToLowerInvariant(), BounceTestSupport.Text(item, "message_id"), item);
         Assert.AreEqual("Our newsletter", BounceTestSupport.Text(item, "subject"), item);
         // The date the provider gave, read as a Date field is read - the zone the
         // value names is applied and the server's own is not, so this is the same
         // on every bench.
         Assert.AreEqual("2026-09-15 09:00:00", BounceTestSupport.Text(item, "arrival"), item);
      }

      [Test]
      [Description("The address whose reader complained is put on the sending domain's suppression list, with a source of complaint and 5.7.1 rather than a dead-mailbox code, and is counted as a complaint beside the bounce counts without touching one of them.")]
      public void TheComplainedOfAddressIsSuppressed()
      {
         Account sender = NewSender();

         InjectReport(ComplaintTestSupport.ReportText(Mailbox, "abuse", sender.Address, Reader, NewMessageId()));

         string entry = BounceTestSupport.AssertItemFor(Read(DomainPath + "/suppressions"), Reader);
         Assert.AreEqual("complaint", BounceTestSupport.Text(entry, "source"), entry);
         Assert.AreEqual("none", BounceTestSupport.Text(entry, "category"), entry);
         StringAssert.Contains("abuse", BounceTestSupport.Text(entry, "diagnostic"), entry);

         string count = BounceTestSupport.AssertItemFor(Read(DomainPath + "/bounces"), Reader);
         Assert.AreEqual(1, BounceTestSupport.Number(count, "complaints"), count);
         Assert.AreEqual(0, BounceTestSupport.Number(count, "hard"), count);
         Assert.AreEqual(0, BounceTestSupport.Number(count, "consecutive_hard"), count);
         Assert.IsTrue(BounceTestSupport.Flag(count, "suppressed"), count);

         // The list is the bounces' list, so the refusal a submission meets is the
         // one that list gives - and its reason is the complaint's, not a bounce's.
         // Bounce processing is OFF here, on purpose: a list the feedback loops are
         // filling has to be applied, or the feature does nothing while looking as
         // though it works.
         Assert.IsFalse(BounceTestSupport.ComBounceProcessingEnabled(_settings),
            "This test is about the list applying with bounce processing off.");

         string reply = BounceTestSupport.RcptReply(sender.Address, Reader);
         StringAssert.StartsWith("550", reply, "A complained-of address was not refused at RCPT TO.");
         StringAssert.Contains("5.7.1", reply, reply);
         StringAssert.Contains("reported mail to it as spam", reply, reply);
      }

      [Test]
      [Description("A message to the complaint mailbox that is not a readable feedback report records nothing, says so in the application log, and is delivered to the mailbox unchanged - and the genuine report after it counts, so the test can fail.")]
      public void AMalformedReportIsRefusedWithoutLosingTheMessage()
      {
         Account sender = NewSender();

         string delivered = InjectRaw(ComplaintTestSupport.MalformedReportText(Mailbox));
         StringAssert.Contains("this report does not say who or about what", delivered,
            "The message that was not a readable report was not delivered to the mailbox unchanged.");

         Assert.IsNull(BounceTestSupport.ItemFor(Read(DomainPath + "/complaints"), Reader),
            "A message that is not a readable feedback report was recorded as a complaint.");

         BounceTestSupport.AssertLogContains("is not an RFC 5965 feedback report this server can read",
            "The server did not say that the message to the complaint mailbox was not a report it could read.");

         // The negative control: the genuine one, to the same mailbox, is recorded.
         InjectReport(ComplaintTestSupport.ReportText(Mailbox, "abuse", sender.Address, Reader, NewMessageId()));
         Assert.IsNotNull(BounceTestSupport.ItemFor(Read(DomainPath + "/complaints"), Reader),
            "The genuine report after the malformed one was not recorded either, so this test could not have failed.");
      }

      [Test]
      [Description("A report delivered to an ordinary mailbox is not read at all, so a stranger who can mail one user cannot stop this server sending to an address of that user's domain.")]
      public void AReportToAnOrdinaryMailboxIsNotRead()
      {
         Account sender = NewSender();
         Account ordinary = NewSender();

         string delivered = InjectRaw(ComplaintTestSupport.ReportText(ordinary.Address, "abuse", sender.Address, Reader, NewMessageId()),
                                      ordinary.Address);
         StringAssert.Contains("report-type=feedback-report", delivered, "The report was not delivered to the ordinary mailbox unchanged.");

         Assert.IsNull(BounceTestSupport.ItemFor(Read(DomainPath + "/complaints"), Reader),
            "A feedback report delivered to an ordinary mailbox was recorded as a complaint.");
         Assert.IsNull(BounceTestSupport.ItemFor(Read(DomainPath + "/suppressions"), Reader),
            "A feedback report delivered to an ordinary mailbox listed an address.");

         // The negative control: the same report to the complaint mailbox is read.
         InjectReport(ComplaintTestSupport.ReportText(Mailbox, "abuse", sender.Address, Reader, NewMessageId()));
         Assert.IsNotNull(BounceTestSupport.ItemFor(Read(DomainPath + "/complaints"), Reader),
            "The same report to the complaint mailbox was not recorded either, so this test could not have failed.");
      }

      [Test]
      [Description("A report about mail from a domain this server does not host records nothing, so no report from outside can create a row for a domain that is not the operator's.")]
      public void AReportAboutAnotherOperatorsDomainIsNotRecorded()
      {
         Account sender = NewSender();

         InjectRaw(ComplaintTestSupport.ReportText(Mailbox, "abuse", "somebody@" + BounceTestSupport.RemoteDomain, Reader, NewMessageId()));

         Assert.IsNull(BounceTestSupport.ItemFor(Read(DomainPath + "/complaints"), Reader),
            "A report about a domain this server does not host was recorded against a domain it does.");

         BounceTestSupport.AssertLogContains("which is not a domain this server hosts",
            "The server did not say that the report named a domain it does not host.");

         // The negative control: the same report about this domain's own sender.
         InjectReport(ComplaintTestSupport.ReportText(Mailbox, "abuse", sender.Address, Reader, NewMessageId()));
         Assert.IsNotNull(BounceTestSupport.ItemFor(Read(DomainPath + "/complaints"), Reader),
            "The report about this domain's own sender was not recorded either, so this test could not have failed.");
      }

      [Test]
      [Description("The same report sent twice is one complaint: the second is recognised by its key and counted once, so a provider that resends does not push an address over the threshold on its own.")]
      public void TheSameReportTwiceIsOneComplaint()
      {
         Account sender = NewSender();
         string messageId = NewMessageId();

         ComplaintTestSupport.ComSetComplaintSuppressionThreshold(_settings, 2);

         string report = ComplaintTestSupport.ReportText(Mailbox, "abuse", sender.Address, Reader, messageId);

         InjectReport(report);
         InjectReport(report);

         string count = BounceTestSupport.AssertItemFor(Read(DomainPath + "/bounces"), Reader);
         Assert.AreEqual(1, BounceTestSupport.Number(count, "complaints"), count);
         Assert.IsFalse(BounceTestSupport.Flag(count, "suppressed"), count);

         Assert.AreEqual(1, BounceTestSupport.Number(Read(DomainPath + "/complaints"), "total"),
            "The same report twice was recorded twice.");

         // A second, different message complained about does reach the threshold.
         InjectReport(ComplaintTestSupport.ReportText(Mailbox, "abuse", sender.Address, Reader, NewMessageId()));

         count = BounceTestSupport.AssertItemFor(Read(DomainPath + "/bounces"), Reader);
         Assert.AreEqual(2, BounceTestSupport.Number(count, "complaints"), count);
         Assert.IsTrue(BounceTestSupport.Flag(count, "suppressed"), count);
      }

      [Test]
      [Description("A not-spam report is recorded and counts towards nothing: it is the reader saying the opposite, so it lists nobody and does not raise the rate.")]
      public void ANotSpamReportCountsTowardsNothing()
      {
         Account sender = NewSender();

         InjectReport(ComplaintTestSupport.ReportText(Mailbox, "not-spam", sender.Address, Reader, NewMessageId()));

         string item = BounceTestSupport.AssertItemFor(Read(DomainPath + "/complaints"), Reader);
         Assert.AreEqual("not-spam", BounceTestSupport.Text(item, "feedback_type"), item);
         Assert.IsFalse(BounceTestSupport.Flag(item, "counts"), item);

         Assert.IsNull(BounceTestSupport.ItemFor(Read(DomainPath + "/suppressions"), Reader),
            "A not-spam report listed the reader's address.");
         Assert.AreEqual(0, BounceTestSupport.Number(Read(DomainPath + "/complaints"), "complaints"),
            "A not-spam report was counted towards the complaint rate.");
      }

      [Test]
      [Description("A report that redacts the reader - no Original-Rcpt-To and no To: in the message it wraps - is still a complaint: it is recorded against the domain and counts towards the rate, and it lists NOBODY, which is asserted of the whole list rather than of one address the report never named.")]
      public void ARedactedReportCountsForTheDomainAndListsNobody()
      {
         Account sender = NewSender();

         InjectReport(ComplaintTestSupport.ReportText(Mailbox, "abuse", sender.Address, null, NewMessageId()));

         string document = Read(DomainPath + "/complaints");
         Assert.AreEqual(1, BounceTestSupport.Number(document, "total"), document);
         Assert.AreEqual(1, BounceTestSupport.Number(document, "complaints"), document);
         StringAssert.Contains("\"address\":\"\"", document, "The recorded complaint named a reader the report did not.");

         // The whole list, not one address: the point of the test is that NOTHING was
         // listed, and naming an address here would pass whatever the server did with
         // the report - which is how this test used to pass while the server listed
         // the fallback address the fixture had quietly left in the wrapped message.
         string list = Read(DomainPath + "/suppressions");
         Assert.AreEqual(0, BounceTestSupport.Number(list, "total"), list);
         StringAssert.Contains("\"items\":[]", list, list);
      }

      [Test]
      [Description("A report with no Original-Rcpt-To but with a To: in the message it wraps names that address: the documented fallback, told apart from a fully redacted report so that neither path can pass for the other.")]
      public void TheWrappedToIsUsedWhenTheFieldIsMissing()
      {
         Account sender = NewSender();

         InjectReport(ComplaintTestSupport.ReportWithRcptToOnlyInTheWrappedMessage(
            Mailbox, sender.Address, Reader, NewMessageId()));

         string item = BounceTestSupport.AssertItemFor(Read(DomainPath + "/complaints"), Reader);
         Assert.AreEqual("abuse", BounceTestSupport.Text(item, "feedback_type"), item);

         Assert.IsNotNull(BounceTestSupport.ItemFor(Read(DomainPath + "/suppressions"), Reader),
            "The address the wrapped message was addressed to was not listed.");
      }

      [Test]
      [Description("With bounce processing off and the feedback loops on, the list is still applied - including to an entry a bounce made. That is a change to shipped bounce behaviour and it is what BounceSuppression.md and the bounces settings description now say.")]
      public void TheListIsAppliedWithBounceProcessingOffAndComplaintsOn()
      {
         Account sender = NewSender();

         // An entry a BOUNCE made, which needs bounce processing on to be written.
         BounceTestSupport.ComSetBounceProcessingEnabled(_settings, true);
         BounceTestSupport.DeliverThroughSimulator(sender.Address, Reader, 550, "550 5.1.1 User unknown");
         Pop3ClientSimulator.AssertGetFirstMessageText(sender.Address, Password);

         Assert.IsNotNull(BounceTestSupport.ItemFor(Read(DomainPath + "/suppressions"), Reader),
            "The hard bounce did not put the address on the list, so this test proves nothing.");

         // Now bounce processing off, feedback loops on - the state this branch
         // changed. The entry is still enforced.
         BounceTestSupport.ComSetBounceProcessingEnabled(_settings, false);

         string list = Read(DomainPath + "/suppressions");
         Assert.IsFalse(BounceTestSupport.Flag(list, "processing_enabled"), list);
         Assert.IsTrue(BounceTestSupport.Flag(list, "list_applies"),
            "The document did not say the list still applies, which is what the page reads.");

         string reply = BounceTestSupport.RcptReply(sender.Address, Reader);
         StringAssert.StartsWith("550", reply, "A listed address was accepted with bounce processing off and complaints on.");

         // The negative control: with BOTH off nothing is refused.
         ComplaintTestSupport.ComSetComplaintProcessingEnabled(_settings, false);

         string open = Read(DomainPath + "/suppressions");
         Assert.IsFalse(BounceTestSupport.Flag(open, "list_applies"), open);
         StringAssert.StartsWith("250", BounceTestSupport.RcptReply(sender.Address, Reader),
            "With both switches off the list was applied all the same.");
      }

      [Test]
      [Description("With complaint processing off, a report to the mailbox is delivered and nothing is read, queried or written - which is what an upgraded server does until an administrator switches it on.")]
      public void WithProcessingOffNothingIsRead()
      {
         Account sender = NewSender();

         ComplaintTestSupport.ComSetComplaintProcessingEnabled(_settings, false);

         InjectRaw(ComplaintTestSupport.ReportText(Mailbox, "abuse", sender.Address, Reader, NewMessageId()));

         Assert.IsNull(BounceTestSupport.ItemFor(Read(DomainPath + "/complaints"), Reader),
            "A report was recorded with complaint processing off.");

         ComplaintTestSupport.ComSetComplaintProcessingEnabled(_settings, true);

         InjectReport(ComplaintTestSupport.ReportText(Mailbox, "abuse", sender.Address, Reader, NewMessageId()));
         Assert.IsNotNull(BounceTestSupport.ItemFor(Read(DomainPath + "/complaints"), Reader),
            "The report after processing was switched on was not recorded either, so this test could not have failed.");
      }

      [Test]
      [Description("With the threshold at 0 a complaint is recorded and counted and nobody is listed, which is the observe-only setting an operator starts with.")]
      public void AThresholdOfZeroListsNobody()
      {
         Account sender = NewSender();

         ComplaintTestSupport.ComSetComplaintSuppressionThreshold(_settings, 0);

         InjectReport(ComplaintTestSupport.ReportText(Mailbox, "abuse", sender.Address, Reader, NewMessageId()));

         string count = BounceTestSupport.AssertItemFor(Read(DomainPath + "/bounces"), Reader);
         Assert.AreEqual(1, BounceTestSupport.Number(count, "complaints"), count);
         Assert.IsFalse(BounceTestSupport.Flag(count, "suppressed"), count);

         Assert.IsNull(BounceTestSupport.ItemFor(Read(DomainPath + "/suppressions"), Reader),
            "A complaint listed an address with the threshold at 0.");
      }

      [Test]
      [Description("The rate's denominator is the messages a remote accepted, counted per sending domain: a delivered message and then a complaint about it leave the domain with one complaint in one message, which is too little to judge and says so rather than reporting a rate of a million.")]
      public void TheRateCountsWhatWasDeliveredAndWhatWasComplainedAbout()
      {
         Account sender = NewSender();
         string messageId = NewMessageId();

         DeliverOne(sender.Address, Reader, messageId);

         InjectReport(ComplaintTestSupport.ReportText(Mailbox, "abuse", sender.Address, Reader, messageId));

         string document = Read(DomainPath + "/complaints");
         Assert.AreEqual(1, BounceTestSupport.Number(document, "complaints"), document);
         Assert.IsTrue(BounceTestSupport.Number(document, "sent") >= 1, document);
         Assert.AreEqual(7, BounceTestSupport.Number(document, "window_days"), document);
         Assert.IsFalse(BounceTestSupport.Flag(document, "rate_judged"),
            "A domain that has sent a handful of messages was judged on its complaint rate.");
         Assert.AreEqual(0, BounceTestSupport.Number(document, "rate_ppm"),
            "An unjudged domain reported a rate rather than 0.");
      }

      // ---- helpers -----------------------------------------------------------

      /// <summary>
      ///    One message from this server's sender to the address the report will name,
      ///    delivered through a simulator that accepts it - which is what the rate's
      ///    denominator counts.
      /// </summary>
      private static void DeliverOne(string sender, string recipient, string messageId)
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
         }

         CustomAsserts.AssertRecipientsInDeliveryQueue(0, true);
      }

      /// <summary>
      ///    A report posted to the complaint mailbox as a provider posts one - over
      ///    plain SMTP, with the provider's own envelope sender - and read out of the
      ///    mailbox again, which every report must be delivered to, counted or not.
      /// </summary>
      private string InjectReport(string text)
      {
         string delivered = InjectRaw(text);
         StringAssert.Contains("report-type=feedback-report", delivered, "The report was not delivered to the mailbox unchanged.");
         return delivered;
      }

      /// <summary>
      ///    The same, without asserting anything about what arrived: for the messages
      ///    that are not reports.
      /// </summary>
      private string InjectRaw(string text, string to = null)
      {
         string recipient = to ?? Mailbox;

         SmtpClientSimulator.StaticSendRaw("fbl@provider.test", recipient, text);

         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         return Pop3ClientSimulator.AssertGetFirstMessageText(recipient, Password);
      }

      private static string Read(string path)
      {
         return BounceTestSupport.Read(RestPort, path);
      }
   }
}
