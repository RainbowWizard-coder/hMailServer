// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Text;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;
using hMailServer;

namespace RegressionTests.SMTP
{
   /// <summary>
   ///    Journaling and dual delivery (Common/Util/Journaling.h): a copy of a
   ///    domain's mail, or of one account's, sent to an address.
   ///
   ///    What is proved here is the wiring, on real SMTP deliveries: that a rule
   ///    is evaluated once per accepted message where the recipients are known,
   ///    that the copy is the shape the rule asked for, that the mailbox the
   ///    message was addressed to is untouched either way, and that the two
   ///    things that must stop a copy stop one - a rule that is off, and a
   ///    journal message this server itself wrote coming back in.
   ///
   ///    And, as much to the point, that one thing which must NOT stop a copy
   ///    does not: an X-hMailServer-Journal header the sender wrote. The name is
   ///    published in the documentation, so if the guard took the header at its
   ///    word, anybody under an outbound compliance rule could keep their mail out
   ///    of the archive with one line in their client. The header this server
   ///    writes carries a mark the process chose at random and nobody else has
   ///    seen; the test takes that value out of a report the server really made
   ///    and shows that it stops a copy while an invented one does not.
   ///
   ///    The rules are written over REST rather than over COM on purpose: the COM
   ///    methods are new, and the suite's interop assembly is built from the type
   ///    library the machine has registered, so a fixture that named them would
   ///    not compile until every bench had been re-registered. The REST routes and
   ///    the COM methods go through one function on the server
   ///    (Journaling::SaveRule), so what is proved through one is true of both.
   ///
   ///    The journal mailbox is usually in a SECOND hosted domain, which is the
   ///    ordinary arrangement; one test below deliberately puts it INSIDE the
   ///    journalled domain, because that configuration used to match and then
   ///    throw the copy away, leaving an empty archive and a rule that every
   ///    surface still showed as enabled.
   /// </summary>
   [TestFixture]
   public class Journaling : TestFixtureBase
   {
      private const string JournalHeader = "X-hMailServer-Journal";
      private const string AdminPassword = "testar";
      private const string Password = "Journal-Passw0rd!";

      // The fixture's own port, from the range this row was allocated (9716-9725),
      // so that it never contends with another fixture's listener.
      private static int RestPort = 9716;

      private Domain _journalDomain;
      private Account _journalBox;
      private readonly List<long> _rules = new List<long>();

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(AdminPassword);
         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();
         Assert.AreEqual(200, Admin("GET", "/api/v1/status", null).status, "REST API did not answer /api/v1/status.");

         _journalDomain = SingletonProvider<TestSetup>.Instance.AddDomain("journals-" + TestSetup.UniqueString() + ".test");
         _journalBox = SingletonProvider<TestSetup>.Instance.AddAccount(_journalDomain, "vault@" + _journalDomain.Name, Password);
      }

      [TearDown]
      public void StopRestApi()
      {
         try
         {
            // Every rule this fixture wrote, taken away: a rule left behind would
            // copy the mail of every fixture that ran after it, which is the one
            // way this feature could break a suite that has nothing to do with it.
            foreach (long id in _rules)
               Admin("DELETE", "/api/v1/journal-rules/" + id, null);

            _rules.Clear();

            // And the journal domain this test made. The suite's baseline
            // removes it before the next test, but nothing removed the LAST
            // test's, and a bench with two domains fails the pre-flight of the
            // gate after it.
            if (_journalDomain != null)
            {
               _journalDomain.Delete();
               _journalDomain = null;
            }
         }
         finally
         {
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      /// <summary>Writes a rule and answers its id, remembering it for the teardown.</summary>
      private long AddRule(string scope, string target, string journalAddress, string direction, string mode, bool enabled = true)
      {
         string body = "{\"scope\":\"" + scope + "\",\"target\":\"" + target + "\",\"journal_address\":\"" + journalAddress +
                       "\",\"direction\":\"" + direction + "\",\"mode\":\"" + mode + "\",\"enabled\":" + (enabled ? "true" : "false") + "}";

         var answer = Admin("POST", "/api/v1/journal-rules", body);
         Assert.AreEqual(201, answer.status, "The journal rule was refused: " + answer.body);

         long id = IdOf(answer.body);
         _rules.Add(id);
         return id;
      }

      private static long IdOf(string json)
      {
         int at = json.IndexOf("\"id\":", StringComparison.Ordinal);
         Assert.Greater(at, -1, "The answer carried no id: " + json);

         int from = at + 5;
         int to = from;
         while (to < json.Length && (char.IsDigit(json[to]) || json[to] == '-'))
            to++;

         return long.Parse(json.Substring(from, to - from));
      }

      [Test]
      [Description("With no rule, nothing is copied anywhere: the feature is inert until somebody writes a rule.")]
      public void NothingIsJournalledWithoutARule()
      {
         var reader = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "reader@" + _domain.Name, Password);

         SmtpClientSimulator.StaticSend("stranger@outside-journal.test", reader.Address, "Quarterly figures", "The body");

         string delivered = Pop3ClientSimulator.AssertGetFirstMessageText(reader.Address, Password);
         StringAssert.DoesNotContain(JournalHeader, delivered, "The recipient's own copy is never marked.");

         Pop3ClientSimulator.AssertMessageCount(_journalBox.Address, Password, 0);
      }

      [Test]
      [Description("An inbound rule on a domain produces one envelope report per message to the journal address, with the original attached and the envelope right, and leaves the recipient's own copy alone.")]
      public void ADomainRuleReportsEveryArrivingMessage()
      {
         var reader = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "reader@" + _domain.Name, Password);
         var colleague = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "colleague@" + _domain.Name, Password);

         AddRule("domain", _domain.Name, _journalBox.Address, "inbound", "report");

         // Two envelope recipients and no To header naming the second: the whole
         // reason the report exists beside the attached message is that the
         // envelope is where the message actually went.
         string raw = "From: stranger@outside-journal.test\r\n" +
                      "To: " + reader.Address + "\r\n" +
                      "Subject: Quarterly figures\r\n" +
                      "Message-ID: <journal-one@outside-journal.test>\r\n" +
                      "\r\n" +
                      "The body of the journalled message.\r\n";

         // One RCPT TO per address. A comma-joined string would be one RCPT TO of
         // something that is not an address, which the server answers 550 to.
         var client = new SmtpClientSimulator();
         client.SendRaw("stranger@outside-journal.test",
            new List<string> { reader.Address, colleague.Address }, raw);

         string readersCopy = Pop3ClientSimulator.AssertGetFirstMessageText(reader.Address, Password);
         StringAssert.DoesNotContain(JournalHeader, readersCopy, "The recipient's own copy must not be marked.");
         StringAssert.Contains("Subject: Quarterly figures", readersCopy);

         Pop3ClientSimulator.AssertGetFirstMessageText(colleague.Address, Password);

         // One report, not one per recipient: the rule matched once.
         Pop3ClientSimulator.AssertMessageCount(_journalBox.Address, Password, 1);

         string report = Pop3ClientSimulator.AssertGetFirstMessageText(_journalBox.Address, Password);

         StringAssert.Contains(JournalHeader, report, "Every journal message carries the header the evaluation reads first.");
         StringAssert.Contains("Content-Type: multipart/mixed", report, "A report is the envelope and the message, in two parts.");
         StringAssert.Contains("Sender: stranger@outside-journal.test", report, "The envelope sender belongs in the report.");
         StringAssert.Contains("Subject: Quarterly figures", report);
         StringAssert.Contains("Message-Id: <journal-one@outside-journal.test>", report);
         StringAssert.Contains("Recipient: " + reader.Address, report, "Each envelope recipient gets a line.");
         StringAssert.Contains("Recipient: " + colleague.Address, report,
            "The recipient no header names is the one the envelope report exists for.");
         StringAssert.Contains("Content-Type: message/rfc822", report, "The original is attached as a message.");
         StringAssert.Contains("The body of the journalled message.", report, "The original goes through unaltered.");
      }

      [Test]
      [Description("An outbound rule for an account copies what that account sends over an authenticated session, and nothing it merely receives.")]
      public void AnOutboundRuleReportsWhatTheAccountSends()
      {
         var sender = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "sender@" + _domain.Name, Password);
         var reader = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "reader@" + _domain.Name, Password);

         AddRule("account", sender.Address, _journalBox.Address, "outbound", "report");

         // Mail ARRIVING for the account is not what an outbound rule covers.
         SmtpClientSimulator.StaticSend("stranger@outside-journal.test", sender.Address, "Incoming", "Body");
         Pop3ClientSimulator.AssertGetFirstMessageText(sender.Address, Password);
         Pop3ClientSimulator.AssertMessageCount(_journalBox.Address, Password, 0);

         // What it sends, signed in, is.
         var client = new SmtpClientSimulator();
         client.SendRaw(sender.Address, Password, sender.Address, reader.Address,
            "From: " + sender.Address + "\r\nTo: " + reader.Address + "\r\nSubject: Outgoing note\r\n\r\nSent by the account.\r\n");

         Pop3ClientSimulator.AssertGetFirstMessageText(reader.Address, Password);

         Pop3ClientSimulator.AssertMessageCount(_journalBox.Address, Password, 1);

         string report = Pop3ClientSimulator.AssertGetFirstMessageText(_journalBox.Address, Password);
         StringAssert.Contains("Sender: " + sender.Address, report);
         StringAssert.Contains("Recipient: " + reader.Address, report);
         StringAssert.Contains("Sent by the account.", report, "The message the account sent is attached to the report.");
      }

      [Test]
      [Description("Copy mode sends the message itself - Google's dual delivery - rather than a report about it: the same headers and body, with one header added and no wrapper.")]
      public void CopyModeSendsTheMessageItself()
      {
         var reader = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "reader@" + _domain.Name, Password);

         AddRule("account", reader.Address, _journalBox.Address, "inbound", "copy");

         var client = new SmtpClientSimulator();
         client.SendRaw("stranger@outside-journal.test", reader.Address,
            "From: stranger@outside-journal.test\r\nTo: " + reader.Address +
            "\r\nSubject: Migrate me\r\nX-Original-Marker: kept\r\n\r\nThe body a second system wants.\r\n");

         Pop3ClientSimulator.AssertGetFirstMessageText(reader.Address, Password);

         Pop3ClientSimulator.AssertMessageCount(_journalBox.Address, Password, 1);

         string copy = Pop3ClientSimulator.AssertGetFirstMessageText(_journalBox.Address, Password);

         StringAssert.Contains(JournalHeader, copy, "A copy is marked too, so that it is never journalled again.");
         StringAssert.Contains("Subject: Migrate me", copy, "A copy keeps the message's own subject.");
         StringAssert.Contains("X-Original-Marker: kept", copy, "A copy keeps every header the message had.");
         StringAssert.Contains("The body a second system wants.", copy);
         StringAssert.DoesNotContain("Content-Type: multipart/mixed", copy, "A copy is the message, not a report wrapping it.");
         StringAssert.DoesNotContain("Recipient: " + reader.Address, copy, "A copy carries no envelope report.");
      }

      [Test]
      [Description("A copy-mode journal copy of mail from outside whose From names the hosted test domain, and that passed DMARC for it when it arrived, is sent on by a journal mailbox here that forwards it to an address outside: the copy carries what DMARC passed for when the mail arrived, as every copy of it does (docs/SendAs.md, the envelope a copy is sent on with).")]
      public void AJournalMailboxForwardsACopyOfMailThatPassedDmarc()
      {
         var reader = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "reader@" + _domain.Name, Password);
         AddRule("account", reader.Address, _journalBox.Address, "inbound", "copy");

         _journalBox.ForwardEnabled = true;
         _journalBox.ForwardAddress = "external@example.com";
         _journalBox.ForwardKeepOriginal = true;
         _journalBox.Save();

         int markBefore = (int) _settings.AntiSpam.SpamMarkThreshold;
         int deleteBefore = (int) _settings.AntiSpam.SpamDeleteThreshold;
         bool dmarcBefore = _settings.AntiSpam.DMARCEnabled;
         Route route = null;
         try
         {
            _settings.AntiSpam.SpamMarkThreshold = 100;
            _settings.AntiSpam.SpamDeleteThreshold = 1000;
            _settings.AntiSpam.DMARCEnabled = true;
            _application.Reinitialize();

            // DMARC passes for example.test on arrival: SPF for the loopback at its
            // subdomain bounces.example.test, which aligns with the From (SuiteDns).
            SuiteDns.Zone
               .WithTxt("_dmarc.example.test", "v=DMARC1; p=none")
               .WithTxt("bounces.example.test", "v=spf1 ip4:127.0.0.1 -all");

            int port = TestSetup.GetNextFreePort();
            using (var server = new SmtpServerSimulator(1, port))
            {
               server.SecondsToWaitBeforeTerminate = 60;
               server.AddRecipientResult(new Dictionary<string, int> { { "external@example.com", 250 } });
               server.StartListen();

               route = _settings.Routes.Add();
               route.DomainName = "example.com";
               route.TargetSMTPHost = "localhost";
               route.TargetSMTPPort = port;
               route.NumberOfTries = 1;
               route.MinutesBetweenTry = 5;
               route.Save();

               new SmtpClientSimulator().SendRaw("bounce@bounces.example.test", reader.Address,
                  "From: The CEO <ceo@example.test>\r\nTo: " + reader.Address + "\r\nSubject: Journalled and forwarded\r\n\r\nPlease read.\r\n");

               CustomAsserts.AssertRecipientsInDeliveryQueue(0);

               Pop3ClientSimulator.AssertMessageCount(_journalBox.Address, Password, 1);

               // The envelope sender in MAIL FROM, between its angle brackets: the journal
               // copy's, the postmaster of the rule's domain.
               string mailFrom = server.NumberOfConnectedClients > 0 ? (server.MailFromCommand ?? "") : "";
               int start = mailFrom.IndexOf('<');
               int end = mailFrom.LastIndexOf('>');
               string envelope = start >= 0 && end > start ? mailFrom.Substring(start + 1, end - start - 1).ToLowerInvariant() : "";

               Assert.IsNotEmpty(envelope,
                  "The journal mailbox's forward of a copy of mail that passed DMARC for example.test when it arrived was not sent on.");
               Assert.IsTrue(envelope.EndsWith("@example.test"),
                  "The journal mailbox's forward should keep the journal copy's envelope, at the rule's domain: " + mailFrom);
            }
         }
         finally
         {
            SuiteDns.Reset();
            if (route != null)
            {
               try { _settings.Routes.DeleteByDBID(route.ID); } catch (Exception) { }
            }
            _settings.AntiSpam.SpamMarkThreshold = markBefore;
            _settings.AntiSpam.SpamDeleteThreshold = deleteBefore;
            _settings.AntiSpam.DMARCEnabled = dmarcBefore;
            _application.Reinitialize();
         }
      }

      [Test]
      [Description("The journal header a sender writes decides nothing: the copy is made anyway, and only the mark this server minted stops one.")]
      public void OnlyTheServersOwnJournalHeaderStopsACopy()
      {
         var reader = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "reader@" + _domain.Name, Password);

         AddRule("account", reader.Address, _journalBox.Address, "inbound", "report");

         // A sender writing the header themselves. The name is in the manual, so
         // this is the thing anybody under a compliance rule would try first: if
         // it worked, the archive would be optional for whoever read the manual.
         var forging = new SmtpClientSimulator();
         forging.SendRaw("stranger@outside-journal.test", reader.Address,
            JournalHeader + ": 1\r\nFrom: stranger@outside-journal.test\r\nTo: " + reader.Address +
            "\r\nSubject: Not in the archive please\r\n\r\nWith a journal header the sender wrote.\r\n");

         Pop3ClientSimulator.AssertGetFirstMessageText(reader.Address, Password);

         Pop3ClientSimulator.AssertMessageCount(_journalBox.Address, Password, 1);

         string report = Pop3ClientSimulator.AssertGetFirstMessageText(_journalBox.Address, Password);
         StringAssert.Contains("Subject: Not in the archive please", report,
            "A header the sender wrote must not keep a message out of the archive.");

         // And the value this server put on its own report IS honoured. Taken out
         // of the report above rather than invented here, because the whole point
         // is that it cannot be invented: it carries a mark the server process
         // chose at random and told nobody.
         string mark = HeaderValue(report, JournalHeader);
         Assert.IsTrue(mark.Length > 0, "The report this server wrote carries no journal header: " + report);
         StringAssert.Contains(".", mark, "The journal header carries the rule id and a mark, not the id alone: " + mark);

         var returning = new SmtpClientSimulator();
         returning.SendRaw("archive@outside-journal.test", reader.Address,
            JournalHeader + ": " + mark + "\r\nFrom: archive@outside-journal.test\r\nTo: " + reader.Address +
            "\r\nSubject: A report coming back\r\n\r\nA journal report relayed back in.\r\n");

         // The mailbox still gets its mail: the guard stops the COPY, not the
         // delivery. Reading a message through the POP3 simulator deletes it, so
         // the forged one is gone and this is the one message in the mailbox.
         string cameBack = Pop3ClientSimulator.AssertGetFirstMessageText(reader.Address, Password);
         StringAssert.Contains("Subject: A report coming back", cameBack, "The message carrying this server's mark is delivered like any other.");

         // The queue empty before the count below, so that "nothing" is not
         // merely "the second report has not been delivered yet".
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         // No second report: the one report was read - and so deleted - above,
         // and the message carrying this server's own mark was not journalled
         // again.
         Pop3ClientSimulator.AssertMessageCount(_journalBox.Address, Password, 0);
      }

      [Test]
      [Description("A domain rule may send its copies to a mailbox in the domain it journals: the copy is made, once, and nothing cascades.")]
      public void ADomainMayBeJournalledIntoAMailboxOfItsOwn()
      {
         var reader = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "reader@" + _domain.Name, Password);
         var insideVault = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "inside-vault@" + _domain.Name, Password);

         // The journal mailbox is itself covered by the rule it serves, which is
         // the most ordinary way to write one of these and used to match and then
         // silently throw the copy away, leaving an empty archive behind a rule
         // every surface showed as enabled.
         AddRule("domain", _domain.Name, insideVault.Address, "inbound", "report");

         SmtpClientSimulator.StaticSend("stranger@outside-journal.test", reader.Address, "Round and round", "Body");

         string delivered = Pop3ClientSimulator.AssertGetFirstMessageText(reader.Address, Password);
         StringAssert.Contains("Subject: Round and round", delivered, "The message itself is delivered as it always was.");

         // Once. The report is delivered to the vault through the queue and never
         // passes the evaluation point, so it does not journal itself; a second
         // message would mean it had. The queue is drained first, so that "one"
         // is not "the second has not arrived yet", and the count comes before
         // the read, because reading through the POP3 simulator deletes.
         Pop3ClientSimulator.AssertMessageCount(insideVault.Address, Password, 1);
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         Pop3ClientSimulator.AssertMessageCount(insideVault.Address, Password, 1);

         string report = Pop3ClientSimulator.AssertGetFirstMessageText(insideVault.Address, Password);
         StringAssert.Contains("Recipient: " + reader.Address, report, "The copy is made, into a mailbox of the journalled domain.");
      }

      /// <summary>The value of the first header of that name in a message, or an empty string.</summary>
      private static string HeaderValue(string message, string name)
      {
         foreach (string line in message.Replace("\r\n", "\n").Split('\n'))
         {
            if (line.Length == 0)
               break;

            if (line.StartsWith(name + ":", StringComparison.OrdinalIgnoreCase))
               return line.Substring(name.Length + 1).Trim();
         }

         return "";
      }

      [Test]
      [Description("A rule that is switched off copies nothing, and the message is delivered exactly as it would be with no rule at all.")]
      public void ARuleThatIsOffCopiesNothing()
      {
         var reader = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "reader@" + _domain.Name, Password);

         long id = AddRule("account", reader.Address, _journalBox.Address, "inbound", "report", enabled: false);

         SmtpClientSimulator.StaticSend("stranger@outside-journal.test", reader.Address, "While it is off", "Body");

         Pop3ClientSimulator.AssertGetFirstMessageText(reader.Address, Password);
         Pop3ClientSimulator.AssertMessageCount(_journalBox.Address, Password, 0);

         // And switched on again it copies, which is what makes the assertion
         // above about the switch rather than about the rule being wrong.
         Assert.AreEqual(200, Admin("PUT", "/api/v1/journal-rules/" + id, "{\"enabled\":true}").status);

         SmtpClientSimulator.StaticSend("stranger@outside-journal.test", reader.Address, "Once it is on", "Body");

         Pop3ClientSimulator.AssertGetFirstMessageText(reader.Address, Password);
         Pop3ClientSimulator.AssertMessageCount(_journalBox.Address, Password, 1);
      }

      [Test]
      [Description("The rules are administrator-only and refuse what they cannot honour: a target this server does not host, a word outside the three vocabularies, and a rule with nowhere to send the copy.")]
      public void ARuleIsCheckedBeforeItIsWritten()
      {
         var refusedTarget = Admin("POST", "/api/v1/journal-rules",
            "{\"scope\":\"domain\",\"target\":\"not-hosted-here.test\",\"journal_address\":\"" + _journalBox.Address + "\"}");
         Assert.AreEqual(400, refusedTarget.status, "A domain this server does not host cannot be journalled.");
         StringAssert.Contains("not-hosted-here.test", refusedTarget.body, "The refusal names the domain.");

         var refusedWord = Admin("POST", "/api/v1/journal-rules",
            "{\"scope\":\"domain\",\"target\":\"" + _domain.Name + "\",\"journal_address\":\"" + _journalBox.Address +
            "\",\"direction\":\"sideways\"}");
         Assert.AreEqual(400, refusedWord.status, "A direction outside the three words is refused.");

         var refusedAddress = Admin("POST", "/api/v1/journal-rules",
            "{\"scope\":\"domain\",\"target\":\"" + _domain.Name + "\",\"journal_address\":\"\"}");
         Assert.AreEqual(400, refusedAddress.status, "A rule with nowhere to send the copy is refused.");

         var refusedShape = Admin("POST", "/api/v1/journal-rules",
            "{\"scope\":\"domain\",\"target\":\"" + _domain.Name + "\",\"journal_address\":\"not-an-address\"}");
         Assert.AreEqual(400, refusedShape.status, "A journal address that is not an address is refused.");

         // A line break in the journal address. It used to pass - only a space was
         // looked for - and the value is written into a header of a message this
         // server generates and handed to the parser that builds an outbound
         // RCPT TO, so a rule carrying one is a header injection with the
         // administrator's own permission on it.
         var refusedBreak = Admin("POST", "/api/v1/journal-rules",
            "{\"scope\":\"domain\",\"target\":\"" + _domain.Name +
            "\",\"journal_address\":\"vault\\r\\nX-Injected:yes\\r\\nx@outside-journal.test\"}");
         Assert.AreEqual(400, refusedBreak.status, "A journal address carrying a line break is refused: " + refusedBreak.body);

         // An account journalled into itself: every message twice in the mailbox
         // and nothing archived anywhere. A DOMAIN rule into a mailbox of that
         // domain is a different thing, is ordinary, and is allowed - see
         // ADomainMayBeJournalledIntoAMailboxOfItsOwn.
         var reader = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "reader@" + _domain.Name, Password);

         var refusedSelf = Admin("POST", "/api/v1/journal-rules",
            "{\"scope\":\"account\",\"target\":\"" + reader.Address + "\",\"journal_address\":\"" + reader.Address + "\"}");
         Assert.AreEqual(400, refusedSelf.status, "An account cannot be journalled into itself.");

         // Nothing was written by any of them.
         var listing = Admin("GET", "/api/v1/journal-rules", null);
         Assert.AreEqual(200, listing.status);
         Assert.AreEqual("[]", listing.body.Trim(), "A refused rule leaves no row: " + listing.body);
      }

      [Test]
      [Description("An id no rule carries is a 404, even one that would wrap onto a real id in thirty-two bits: a PUT or a DELETE must not reach a different rule.")]
      public void AnIdNoRuleCarriesReachesNoRule()
      {
         var reader = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "reader@" + _domain.Name, Password);

         long id = AddRule("account", reader.Address, _journalBox.Address, "inbound", "report");

         // The same id with a 1 in the thirty-third bit. Truncated to thirty-two
         // bits - which is what the id used to be narrowed to twice on its way to
         // the query - this names the rule just written, so a DELETE of it
         // destroyed that rule and answered {"deleted":true}.
         long wrapping = id + 4294967296L;

         var missing = Admin("DELETE", "/api/v1/journal-rules/" + wrapping, null);
         Assert.AreEqual(404, missing.status, "An id no rule carries answered: " + missing.body);

         var missingUpdate = Admin("PUT", "/api/v1/journal-rules/" + wrapping, "{\"enabled\":false}");
         Assert.AreEqual(404, missingUpdate.status, "An id no rule carries was updated: " + missingUpdate.body);

         // And the rule that id would have wrapped onto is untouched.
         var listing = Admin("GET", "/api/v1/journal-rules", null);
         Assert.AreEqual(200, listing.status);
         StringAssert.Contains("\"id\":" + id, listing.body, "The rule was destroyed by a request for another id: " + listing.body);
         StringAssert.Contains("\"enabled\":true", listing.body, "The rule was changed by a request for another id: " + listing.body);
      }

      private static (int status, string body) Admin(string method, string path, string requestBody)
      {
         string authorization = "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes("Administrator:" + AdminPassword));
         return RestRequest.Send("127.0.0.1", RestPort, method, path, authorization, requestBody);
      }
   }
}
