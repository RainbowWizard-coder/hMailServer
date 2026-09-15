// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Text;
using System.Runtime.InteropServices;
using System.Threading;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.SMTP
{
   /// <summary>
   ///    Bounces read, and a suppression list: a refusal met while delivering is
   ///    classified against RFC 3463, counted per address and per sending domain,
   ///    and an address that has hard-bounced past the threshold is put on that
   ///    domain's list, which the next send is checked against.
   ///
   ///    The tests that matter most are the ones about NOT listing. A suppression
   ///    list that puts a live address on it is worse than none - the sender stops
   ///    reaching somebody and nothing tells them - so a full mailbox, a policy
   ///    refusal and, above all, a bare "550 &lt;address&gt;" (which this server's own
   ///    delivery status notification rightly records as 5.1.1) are each provoked
   ///    and each required to leave the list empty, beside the one reply that must
   ///    fill it. And the first test runs the same hard refusal with processing off,
   ///    which is the negative control for everything else here: it proves these
   ///    tests can see nothing happen.
   /// </summary>
   [TestFixture]
   public class BounceSuppression : TestFixtureBase
   {
      private static int RestPort = 9553;

      private const string Password = "test";
      private const string Remote = "gone@" + BounceTestSupport.RemoteDomain;
      private const string HardReply = "5.1.1 <" + Remote + ">: Recipient address rejected: User unknown";

      private static int senderSequence_;

      private Account NewSender()
      {
         int sequence = Interlocked.Increment(ref senderSequence_);
         return SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "bounce-sender-" + sequence + "@" + _domain.Name, Password);
      }

      private string DomainPath
      {
         get { return "/api/v1/domains/" + _domain.Name; }
      }

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(BounceTestSupport.AdminPassword);

         RestPort = RestListener.Start(RestPort);
         IniFileSetting.Write("RestApiCertificateFile", "");
         IniFileSetting.Write("RestApiPrivateKeyFile", "");

         _application.Reinitialize();

         (int status, string body) probe = Http("GET", "/api/v1/status");
         Assert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);
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
      [Description("Off by default: the eight settings read back as shipped, and a remote's own 5.1.1 with processing off counts nothing, lists nothing and refuses nothing.")]
      public void BounceSuppressionIsOffByDefault()
      {
         Assert.IsFalse(_settings.BounceProcessingEnabled, "Bounce processing ships off.");
         Assert.AreEqual(1, _settings.BounceHardBounceThreshold);
         Assert.IsFalse(_settings.BounceMailboxFullIsHard);
         Assert.AreEqual(0, _settings.BounceSuppressionAction, "refuse");
         Assert.AreEqual(0, _settings.BounceSuppressionExpiryDays);
         Assert.IsTrue(_settings.BounceReadDeliveryReports);
         Assert.AreEqual(90, _settings.BounceCountRetentionDays);
         Assert.AreEqual(7, _settings.BounceReportWindowDays);

         Account sender = NewSender();

         BounceTestSupport.DeliverThroughSimulator(sender.Address, Remote, 550, HardReply);

         Assert.AreEqual(0, BounceTestSupport.Number(Read(DomainPath + "/bounces"), "total"),
            "With processing off, a refusal must not even be counted.");
         Assert.AreEqual(0, BounceTestSupport.Number(Read(DomainPath + "/suppressions"), "total"),
            "With processing off, nothing is put on the list.");

         // And the next send is not refused: it reaches the remote server again.
         Assert.AreEqual(1, BounceTestSupport.DeliverThroughSimulator(sender.Address, Remote, 550, HardReply),
            "The second message did not reach the remote server, so something refused it with processing off.");
      }

      [Test]
      [Description("A remote's own 550 5.1.1 at RCPT TO puts the address on the sending domain's list, with the category, the status, the remote's words and host, and a source of bounce - and the sender still gets the usual report.")]
      public void AHardBounceAtRcptPutsTheAddressOnTheSendingDomainsList()
      {
         _settings.BounceProcessingEnabled = true;
         Account sender = NewSender();

         BounceTestSupport.DeliverThroughSimulator(sender.Address, Remote, 550, HardReply);

         string report = Pop3ClientSimulator.AssertGetFirstMessageText(sender.Address, Password);
         StringAssert.Contains("Status: 5.1.1", report, "The non-delivery report must be unchanged by the list.");

         string list = Read(DomainPath + "/suppressions");
         Assert.AreEqual(1, BounceTestSupport.Number(list, "total"), list);
         Assert.IsTrue(BounceTestSupport.Flag(list, "processing_enabled"), list);
         Assert.AreEqual("refuse", BounceTestSupport.Text(list, "action"), list);

         string item = BounceTestSupport.AssertItemFor(list, Remote);
         Assert.AreEqual("bounce", BounceTestSupport.Text(item, "source"), item);
         Assert.AreEqual("hard", BounceTestSupport.Text(item, "category"), item);
         Assert.AreEqual("5.1.1", BounceTestSupport.Text(item, "status"), item);
         StringAssert.Contains("User unknown", BounceTestSupport.Text(item, "diagnostic"), "The entry keeps the remote's own words.");
         Assert.AreEqual("127.0.0.1", BounceTestSupport.Text(item, "remote_mta"), item);
         Assert.AreEqual("", BounceTestSupport.Text(item, "expires"), "With no expiry set, an entry has none.");
         Assert.IsFalse(BounceTestSupport.Flag(item, "expired"), item);

         string counts = Read(DomainPath + "/bounces");
         string row = BounceTestSupport.AssertItemFor(counts, Remote);
         Assert.AreEqual(1, BounceTestSupport.Number(row, "hard"), row);
         Assert.AreEqual(1, BounceTestSupport.Number(row, "consecutive_hard"), row);
         Assert.AreEqual("5.1.1", BounceTestSupport.Text(row, "last_status"), row);
         Assert.IsTrue(BounceTestSupport.Flag(row, "suppressed"), row);
      }

      [Test]
      [Description("After a hard bounce, the same domain's next submission to that address is refused at RCPT TO with the entry's own status and a sentence that names the address, the list and why.")]
      public void TheNextSubmissionIsRefusedWithAClearReason()
      {
         _settings.BounceProcessingEnabled = true;
         Account sender = NewSender();

         BounceTestSupport.DeliverThroughSimulator(sender.Address, Remote, 550, HardReply);
         Pop3ClientSimulator.AssertGetFirstMessageText(sender.Address, Password);

         string reply = BounceTestSupport.RcptReply(sender.Address, Remote);

         StringAssert.StartsWith("550 5.1.1 ", reply,
            "An EHLO session gets the entry's own enhanced status, which is the status the remote refused with.");
         StringAssert.Contains(Remote, reply);
         StringAssert.Contains("suppression list of " + _domain.Name, reply);
         StringAssert.Contains("bounced permanently", reply);
         StringAssert.Contains("administrator can remove it", reply);

         // Refused at RCPT, so nothing was queued and no second report was written.
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         Pop3ClientSimulator.AssertMessageCount(sender.Address, Password, 0);
      }

      [Test]
      [Description("The webmail's send route refuses a listed recipient with 400 and the code the page translates, queues nothing - and still sends to an address that is not listed.")]
      public void TheWebmailSendRouteRefusesASuppressedRecipient()
      {
         _settings.BounceProcessingEnabled = true;
         Account sender = NewSender();

         (int status, string body) added = Http("POST", DomainPath + "/suppressions", "{\"address\":\"" + Remote + "\",\"note\":\"Left the company.\"}");
         Assert.AreEqual(201, added.status, added.body);

         string account = "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(sender.Address + ":" + Password));

         (int status, string body) refused = RestRequest.Send("127.0.0.1", RestPort, "POST", "/api/v1/me/messages", account,
            "{\"to\":\"" + Remote + "\",\"subject\":\"From the webmail\",\"text\":\"Body.\"}");

         Assert.AreEqual(400, refused.status, refused.body);
         Assert.AreEqual("recipient_suppressed", BounceTestSupport.Text(refused.body, "code"), refused.body);
         Assert.AreEqual(Remote, BounceTestSupport.Text(refused.body, "address"), refused.body);
         StringAssert.Contains("suppression list", refused.body);

         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         // The control: the same route and the same sender reach an address that is
         // not listed, so the 400 above was the list and not the route.
         const string Alive = "alive@" + BounceTestSupport.RemoteDomain;
         int port = TestSetup.GetNextFreePort();

         using (var server = new SmtpServerSimulator(1, port))
         {
            server.AddRecipientResult(new Dictionary<string, int> { [Alive] = 250 });
            server.StartListen();
            BounceTestSupport.RouteTo(port);

            (int status, string body) sent = RestRequest.Send("127.0.0.1", RestPort, "POST", "/api/v1/me/messages", account,
               "{\"to\":\"" + Alive + "\",\"subject\":\"From the webmail\",\"text\":\"Body.\"}");
            Assert.AreEqual(201, sent.status, sent.body);

            server.WaitForCompletion();
            Assert.AreEqual(1, server.RcptTosReceived);

            CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         }
      }

      [Test]
      [Description("The list is the sending domain's: a sender in another hosted domain still reaches an address the first domain has listed.")]
      public void AnotherDomainsSenderIsNotRefused()
      {
         _settings.BounceProcessingEnabled = true;
         Account sender = NewSender();

         Domain second = SingletonProvider<TestSetup>.Instance.AddDomain("bounces-second.test");
         Account otherSender = SingletonProvider<TestSetup>.Instance.AddAccount(second, "sender@bounces-second.test", Password);

         Assert.AreEqual(201, Http("POST", DomainPath + "/suppressions", "{\"address\":\"" + Remote + "\"}").status);

         StringAssert.StartsWith("550", BounceTestSupport.RcptReply(sender.Address, Remote),
            "The domain that listed the address refuses it.");

         Assert.AreEqual(1, BounceTestSupport.DeliverThroughSimulator(otherSender.Address, Remote, 250, null),
            "A sender at another domain was stopped by a list that is not its domain's.");

         Assert.AreEqual(0, BounceTestSupport.Number(Read("/api/v1/domains/bounces-second.test/suppressions"), "total"));
      }

      [Test]
      [Description("An entry removed by hand lets mail through again, starts the run of hard bounces again, and is in the audit trail with its actor - where the entry the server itself made is not.")]
      public void RemovingTheEntryByHandLetsMailThroughAgain()
      {
         _settings.BounceProcessingEnabled = true;
         Account sender = NewSender();

         _application.Database.ExecuteSQL("delete from hm_audit");

         BounceTestSupport.DeliverThroughSimulator(sender.Address, Remote, 550, HardReply);
         BounceTestSupport.AssertItemFor(Read(DomainPath + "/suppressions"), Remote);

         (int status, string body) removed = Http("DELETE", DomainPath + "/suppressions/" + Uri.EscapeDataString(Remote));
         Assert.AreEqual(200, removed.status, removed.body);
         StringAssert.Contains("\"removed\":true", removed.body);

         Assert.AreEqual(404, Http("DELETE", DomainPath + "/suppressions/" + Remote).status,
            "An address that is not listed is not found.");

         Assert.AreEqual(1, BounceTestSupport.DeliverThroughSimulator(sender.Address, Remote, 250, null),
            "After the entry was removed, the next message did not reach the remote server.");

         string row = BounceTestSupport.AssertItemFor(Read(DomainPath + "/bounces"), Remote);
         Assert.AreEqual(0, BounceTestSupport.Number(row, "consecutive_hard"), row);
         Assert.AreEqual(1, BounceTestSupport.Number(row, "hard"), "The history is kept: " + row);
         Assert.AreNotEqual("", BounceTestSupport.Text(row, "last_delivered"), row);
         Assert.IsFalse(BounceTestSupport.Flag(row, "suppressed"), row);

         string trail = Read("/api/v1/audit?object_type=suppression+list+entry&limit=50");
         StringAssert.Contains("\"action\":\"deleted\"", trail, trail);
         StringAssert.Contains("\"object\":\"" + Remote + "\"", trail, trail);
         StringAssert.Contains("\"interface\":\"REST\"", trail, trail);
         StringAssert.Contains("\"actor\":\"administrator\"", trail, trail);
         StringAssert.DoesNotContain("\"action\":\"created\"", trail,
            "The entry delivery made has no actor and must not be recorded as an administrative change: " + trail);
      }

      [Test]
      [Description("A full mailbox (552 5.2.2) is counted and does not list the address - until BounceMailboxFullIsHard says it should.")]
      public void AMailboxFullRefusalIsCountedButDoesNotSuppress()
      {
         _settings.BounceProcessingEnabled = true;
         Account sender = NewSender();

         const string FullReply = "5.2.2 <" + Remote + ">: Mailbox full";

         BounceTestSupport.DeliverThroughSimulator(sender.Address, Remote, 552, FullReply);

         string row = BounceTestSupport.AssertItemFor(Read(DomainPath + "/bounces"), Remote);
         Assert.AreEqual(1, BounceTestSupport.Number(row, "mailbox_full"), row);
         Assert.AreEqual(0, BounceTestSupport.Number(row, "hard"), row);
         Assert.AreEqual(0, BounceTestSupport.Number(row, "consecutive_hard"), row);
         Assert.AreEqual(0, BounceTestSupport.Number(Read(DomainPath + "/suppressions"), "total"),
            "A full mailbox is usually emptied; it must not list the address by default.");

         _settings.BounceMailboxFullIsHard = true;

         BounceTestSupport.DeliverThroughSimulator(sender.Address, Remote, 552, FullReply);

         string item = BounceTestSupport.AssertItemFor(Read(DomainPath + "/suppressions"), Remote);
         Assert.AreEqual("mailbox_full", BounceTestSupport.Text(item, "category"), item);
         Assert.AreEqual("5.2.2", BounceTestSupport.Text(item, "status"), item);
      }

      [Test]
      [Description("A policy refusal (5.7.1) is counted and never lists the address, even at a threshold of one.")]
      public void APolicyRefusalNeverSuppresses()
      {
         _settings.BounceProcessingEnabled = true;
         _settings.BounceMailboxFullIsHard = true;
         Account sender = NewSender();

         BounceTestSupport.DeliverThroughSimulator(sender.Address, Remote, 550,
            "5.7.1 <" + Remote + ">: Service unavailable; client host blocked using a blocklist");

         string row = BounceTestSupport.AssertItemFor(Read(DomainPath + "/bounces"), Remote);
         Assert.AreEqual(1, BounceTestSupport.Number(row, "policy"), row);
         Assert.AreEqual(0, BounceTestSupport.Number(row, "consecutive_hard"), row);
         Assert.AreEqual(0, BounceTestSupport.Number(Read(DomainPath + "/suppressions"), "total"),
            "A refusal on policy says nothing about whether the address exists.");
      }

      [Test]
      [Description("A bare '550 <address>' - which the sender's report still calls 5.1.1 - is not the remote's own code, so it is counted as other and never lists the address.")]
      public void ABareReplyCodeIsNotTrustedAsADeadAddress()
      {
         _settings.BounceProcessingEnabled = true;
         Account sender = NewSender();

         // No reply text: the simulator answers "550 gone@dummy-example.com".
         BounceTestSupport.DeliverThroughSimulator(sender.Address, Remote, 550, null);

         string report = Pop3ClientSimulator.AssertGetFirstMessageText(sender.Address, Password);
         StringAssert.Contains("Status: 5.1.1", report,
            "The report's own code is unchanged; this test is about what the list makes of it.");

         string row = BounceTestSupport.AssertItemFor(Read(DomainPath + "/bounces"), Remote);
         Assert.AreEqual(1, BounceTestSupport.Number(row, "other"), row);
         Assert.AreEqual(0, BounceTestSupport.Number(row, "hard"), row);
         Assert.AreEqual(0, BounceTestSupport.Number(Read(DomainPath + "/suppressions"), "total"),
            "A code this server derived from three digits was trusted as a dead mailbox.");
      }

      [Test]
      [Description("The threshold counts hard bounces in a row: a delivery between them starts the run again.")]
      public void TheThresholdCountsConsecutiveHardBouncesOnly()
      {
         _settings.BounceProcessingEnabled = true;
         _settings.BounceHardBounceThreshold = 2;
         Account sender = NewSender();

         BounceTestSupport.DeliverThroughSimulator(sender.Address, Remote, 550, HardReply);
         Assert.AreEqual(0, BounceTestSupport.Number(Read(DomainPath + "/suppressions"), "total"), "One hard bounce is below a threshold of two.");

         BounceTestSupport.DeliverThroughSimulator(sender.Address, Remote, 250, null);
         Assert.AreEqual(0, BounceTestSupport.Number(BounceTestSupport.AssertItemFor(Read(DomainPath + "/bounces"), Remote), "consecutive_hard"),
            "A delivery must start the run again.");

         BounceTestSupport.DeliverThroughSimulator(sender.Address, Remote, 550, HardReply);
         Assert.AreEqual(0, BounceTestSupport.Number(Read(DomainPath + "/suppressions"), "total"),
            "Two hard bounces with a delivery between them are not two in a row.");

         BounceTestSupport.DeliverThroughSimulator(sender.Address, Remote, 550, HardReply);

         string row = BounceTestSupport.AssertItemFor(Read(DomainPath + "/bounces"), Remote);
         Assert.AreEqual(3, BounceTestSupport.Number(row, "hard"), row);
         Assert.AreEqual(2, BounceTestSupport.Number(row, "consecutive_hard"), row);
         BounceTestSupport.AssertItemFor(Read(DomainPath + "/suppressions"), Remote);
      }

      [Test]
      [Description("With the action drop, a listed recipient is accepted at RCPT TO and removed at delivery: nothing reaches the remote server and no report reaches the sender.")]
      public void DropAcceptsAndSilentlyDropsTheRecipient()
      {
         _settings.BounceProcessingEnabled = true;
         _settings.BounceSuppressionAction = 1;
         Account sender = NewSender();

         Assert.AreEqual(201, Http("POST", DomainPath + "/suppressions", "{\"address\":\"" + Remote + "\"}").status);

         int port = TestSetup.GetNextFreePort();

         using (var server = new SmtpServerSimulator(1, port))
         {
            server.AddRecipientResult(new Dictionary<string, int> { [Remote] = 250 });
            server.StartListen();
            BounceTestSupport.RouteTo(port);

            // Accepted: this throws if RCPT TO is refused.
            SmtpClientSimulator.StaticSend(sender.Address, Remote, "Dropped", "Body text.");

            CustomAsserts.AssertRecipientsInDeliveryQueue(0);

            Assert.AreEqual(0, server.RcptTosReceived, "A dropped recipient reached the remote server.");
         }

         Pop3ClientSimulator.AssertMessageCount(sender.Address, Password, 0);

         string log = LogHandler.ReadCurrentDefaultLog();
         StringAssert.Contains("BounceSuppressionAction is drop", log, "A drop is silent to the sender, not to the log.");
      }

      [Test]
      [Description("With the action observe, a listed recipient is delivered and the log says it was listed.")]
      public void ObserveSendsAndOnlyRecords()
      {
         _settings.BounceProcessingEnabled = true;
         _settings.BounceSuppressionAction = 2;
         Account sender = NewSender();

         Assert.AreEqual(201, Http("POST", DomainPath + "/suppressions", "{\"address\":\"" + Remote + "\"}").status);

         Assert.AreEqual(1, BounceTestSupport.DeliverThroughSimulator(sender.Address, Remote, 250, null),
            "Observe must deliver.");

         StringAssert.Contains("BounceSuppressionAction is observe", LogHandler.ReadCurrentDefaultLog());
         Pop3ClientSimulator.AssertMessageCount(sender.Address, Password, 0);
      }

      [Test]
      [Description("Mail that was not submitted straight to the address - here an alias that forwards to it - is accepted at RCPT TO and returned to the sender at delivery in an RFC 3464 report.")]
      public void MailNotSubmittedDirectlyIsReturnedAtDelivery()
      {
         _settings.BounceProcessingEnabled = true;
         Account sender = NewSender();

         string forwarder = "forwarder@" + _domain.Name;
         SingletonProvider<TestSetup>.Instance.AddAlias(_domain, forwarder, Remote);

         Assert.AreEqual(201, Http("POST", DomainPath + "/suppressions", "{\"address\":\"" + Remote + "\"}").status);

         int port = TestSetup.GetNextFreePort();

         using (var server = new SmtpServerSimulator(1, port))
         {
            server.AddRecipientResult(new Dictionary<string, int> { [Remote] = 250 });
            server.StartListen();
            BounceTestSupport.RouteTo(port);

            SmtpClientSimulator.StaticSend(sender.Address, forwarder, "Through an alias", "Body text.");

            CustomAsserts.AssertRecipientsInDeliveryQueue(0);

            Assert.AreEqual(0, server.RcptTosReceived, "The listed address was reached through the alias.");
         }

         string report = Pop3ClientSimulator.AssertGetFirstMessageText(sender.Address, Password);
         StringAssert.Contains("multipart/report", report);
         StringAssert.Contains("Final-Recipient: rfc822; " + Remote, report);
         StringAssert.Contains("Status: 5.7.1", report, "A hand-made entry refuses with 5.7.1.");
         StringAssert.Contains("suppression list", report);
      }

      [Test]
      [Description("An entry whose expiry has passed stops applying at once, and the sweep removes it and says so.")]
      public void ExpiredEntriesStopApplyingAndAreSwept()
      {
         _settings.BounceProcessingEnabled = true;
         Account sender = NewSender();

         Assert.AreEqual(201, Http("POST", DomainPath + "/suppressions", "{\"address\":\"" + Remote + "\"}").status);
         StringAssert.StartsWith("550", BounceTestSupport.RcptReply(sender.Address, Remote), "The control: listed, refused.");

         _application.Database.ExecuteSQL("update hm_bouncesuppressions set bsexpires = '2000-01-01 00:00:00'");

         string item = BounceTestSupport.AssertItemFor(Read(DomainPath + "/suppressions"), Remote);
         Assert.IsTrue(BounceTestSupport.Flag(item, "expired"), item);

         StringAssert.StartsWith("250", BounceTestSupport.RcptReply(sender.Address, Remote),
            "An expired entry must stop applying before the sweep removes it.");

         (int status, string body) swept = Http("POST", "/api/v1/bounces/expire", "{}");
         Assert.AreEqual(200, swept.status, swept.body);
         Assert.AreEqual(1, BounceTestSupport.Number(swept.body, "suppressions_expired"), swept.body);

         Assert.AreEqual(0, BounceTestSupport.Number(Read(DomainPath + "/suppressions"), "total"));
      }

      [Test]
      [Description("Deleting a domain with a count, an entry and a report key succeeds, and each table still refuses a row for a domain that does not exist: all three cascade from hm_domains.")]
      public void DeletingTheDomainRemovesItsRows()
      {
         _settings.BounceProcessingEnabled = true;

         const string Name = "bounces-deleted.test";
         Domain doomed = SingletonProvider<TestSetup>.Instance.AddDomain(Name);
         Account sender = SingletonProvider<TestSetup>.Instance.AddAccount(doomed, "sender@" + Name, Password);

         // A row in each of the three tables: a key from a delivery, a count from a
         // refusal, and an entry by hand.
         BounceTestSupport.DeliverThroughSimulator(sender.Address, "alive@" + BounceTestSupport.RemoteDomain, 250, null);
         BounceTestSupport.DeliverThroughSimulator(sender.Address, Remote, 550, HardReply);
         BounceTestSupport.AssertItemFor(Read("/api/v1/domains/" + Name + "/suppressions"), Remote);

         Assert.AreEqual(201, Http("POST", "/api/v1/domains/" + Name + "/suppressions",
            "{\"address\":\"other@" + BounceTestSupport.RemoteDomain + "\"}").status);

         int doomedId = doomed.ID;

         // With rows in all three tables, the delete can only succeed if every one of
         // them either has no constraint at all or cascades.
         _application.Domains.DeleteByDBID(doomedId);

         // And each still has its constraint: a row naming the deleted domain is
         // refused by the database. Together the two leave only the cascade - read
         // off the database's behaviour, so it holds on whichever of the four the
         // bench runs, without reading a table the COM API cannot return.
         string[] orphans =
         {
            "insert into hm_bouncesuppressions (bsdomainid, bsaddress, bssource, bscategory, bsstatus, bsdiagnostic, bsremotemta, bscreated, bsexpires) " +
               "values (" + doomedId + ", 'orphan@" + BounceTestSupport.RemoteDomain + "', 2, 0, '', '', '', '2026-01-01 00:00:00', '')",
            "insert into hm_bouncecounts (bcdomainid, bcaddress, bchard, bcsoft, bcmailboxfull, bcpolicy, bcother, bcconsecutivehard, bclaststatus, bclastcategory, bclastbounce, bclastdelivered) " +
               "values (" + doomedId + ", 'orphan@" + BounceTestSupport.RemoteDomain + "', 0, 0, 0, 0, 0, 0, '', 0, '', '')",
            "insert into hm_bouncereportkeys (brkdomainid, brkkey, brksent) values (" + doomedId + ", 'orphan', '2026-01-01 00:00:00')"
         };

         foreach (string orphan in orphans)
         {
            Assert.Throws<COMException>(() => _application.Database.ExecuteSQL(orphan),
               "A row naming a domain that does not exist was accepted, so this table has no foreign key to hm_domains: " + orphan);
         }

         // The refusals are reported to the error log by the connection that saw
         // them; those lines are this test's, not a fault.
         LogHandler.ReadAndDeleteErrorLog();
      }

      [Test]
      [Description("Utilities.EraseAddressTraces removes an address's counts and entries in every domain, and counts them in what it answers.")]
      public void EraseAddressTracesRemovesBounceRows()
      {
         _settings.BounceProcessingEnabled = true;
         Account sender = NewSender();

         BounceTestSupport.DeliverThroughSimulator(sender.Address, Remote, 550, HardReply);
         BounceTestSupport.AssertItemFor(Read(DomainPath + "/suppressions"), Remote);
         BounceTestSupport.AssertItemFor(Read(DomainPath + "/bounces"), Remote);

         int removed = _application.Utilities.EraseAddressTraces(Remote, false);
         Assert.GreaterOrEqual(removed, 2, "The count row and the entry are two of the things removed.");

         Assert.AreEqual(0, BounceTestSupport.Number(Read(DomainPath + "/suppressions"), "total"));
         Assert.AreEqual(0, BounceTestSupport.Number(Read(DomainPath + "/bounces"), "total"));
      }

      private static (int status, string body) Http(string method, string path, string requestBody = null)
      {
         return BounceTestSupport.Http(RestPort, method, path, requestBody);
      }

      private static string Read(string path)
      {
         return BounceTestSupport.Read(RestPort, path);
      }
   }
}
