// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;
using hMailServer;

namespace RegressionTests.AntiSpam
{
   /// <summary>
   ///    The quarantine review (GitLab issue 1): the HELO name stored with a hold,
   ///    the one request that acts on many held messages, whitelisting and
   ///    blacklisting from the queue into the two lists that already exist, the
   ///    headers and a preview read without releasing, and the expired sweep over
   ///    REST.
   ///
   ///    The members this added to QuarantinedMessage and Quarantine are reached
   ///    late-bound (dynamic), so that the fixture does not depend on a type library
   ///    registered after the schema step - the same reason the Sieve fixtures give.
   ///
   ///    Every name a test lists is under .test or is co.uk, which no other fixture
   ///    sends from or says HELO as, and TearDown removes every entry a test wrote,
   ///    from both lists: a black domain left behind would refuse somebody else's
   ///    mail at MAIL FROM.
   /// </summary>
   [TestFixture]
   public class QuarantineReview : TestFixtureBase
   {
      private const string SurblTestPoint = "surbl-org-permanent-test-point.com.multi.surbl.org";
      private const string SpamLink = "http://surbl-org-permanent-test-point.com/";
      private const string AdminPassword = "testar";

      // A port of this fixture's own, as every REST fixture has one.
      private static int RestPort = 10387;

      // What the tests list, and nothing else; TearDown takes each of these off
      // both lists whether or not the test that wrote it finished.
      private static readonly string[] Listed =
      {
         "allow-me@review-sender.test",
         "block-me@review-sender.test",
         ".review-helo-block.test",
         ".review-sender-allow.test",
         ".co.uk",
         "held-at-delivery@review-sender.test",
         // What blocking a HELO of 192.0.2.77 by its domain wrote before an
         // address was no name: removed should a regression write it again.
         ".2.77"
      };

      private Account _recipient;

      [OneTimeSetUp]
      public void ServeTheNamesThisFixtureNeeds()
      {
         // The verdict has to be real, and must not depend on a live blacklist.
         SuiteDns.Zone.WithA(SurblTestPoint, "127.0.0.2");
      }

      [OneTimeTearDown]
      public void RestoreTheDefaults()
      {
         SuiteDns.Reset();

         ServerIniFile.SetSetting("QuarantineEnabled", null);
         ServerIniFile.SetSetting("QuarantineRetentionDays", null);
         RestartServerAndReacquireCom();
      }

      [SetUp]
      public new void SetUp()
      {
         _settings.SetAdministratorPassword(AdminPassword);
         RestPort = RestListener.Start(RestPort);

         _recipient = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "reviewed@example.test", "test");

         var antiSpam = _application.Settings.AntiSpam;
         antiSpam.SpamMarkThreshold = 1;
         antiSpam.SpamDeleteThreshold = 5;

         var surbl = antiSpam.SURBLServers[0];
         surbl.Active = true;
         surbl.Score = 10;
         surbl.Save();

         ServerIniFile.SetSetting("QuarantineEnabled", "1");
         _application.Reinitialize();

         (int status, string body) probe = Rest("GET", "/api/v1/status");
         Assert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);

         RemoveWhatTheTestsListed();
         EmptyTheQuarantine();
      }

      [TearDown]
      public new void TearDown()
      {
         try
         {
            RemoveWhatTheTestsListed();
            EmptyTheQuarantine();
         }
         finally
         {
            ServerIniFile.SetSetting("QuarantineEnabled", null);
            ServerIniFile.SetSetting("QuarantineRetentionDays", null);

            var surbl = _application.Settings.AntiSpam.SURBLServers[0];
            surbl.Active = false;
            surbl.Save();

            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      // ------------------------------------------------------------------ helpers

      private static (int status, string body) Rest(string method, string path, string requestBody = null)
      {
         string credentials = Convert.ToBase64String(Encoding.ASCII.GetBytes("Administrator:" + AdminPassword));
         return RestRequest.Send("127.0.0.1", RestPort, method, path, "Basic " + credentials, requestBody);
      }

      private static (int status, string body) WithKey(string token, string method, string path, string requestBody = null)
      {
         return RestRequest.Send("127.0.0.1", RestPort, method, path, "Bearer " + token, requestBody);
      }

      private void EmptyTheQuarantine()
      {
         var quarantine = _application.Settings.AntiSpam.Quarantine;
         quarantine.Refresh();

         while (quarantine.Count > 0)
         {
            quarantine.DeleteByDBID(quarantine[0].ID);
            quarantine.Refresh();
         }
      }

      /// <summary>Every entry a test here wrote, off the server's allow and block list and the domain white and black list.</summary>
      private static void RemoveWhatTheTestsListed()
      {
         (int status, string body) senders = Rest("GET", "/api/v1/sender-list");
         if (senders.status == 200)
         {
            foreach (object item in MiniJson.List(MiniJson.Object(senders.body), "items"))
            {
               var entry = (Dictionary<string, object>) item;
               if (Listed.Contains(MiniJson.Text(entry, "value")))
                  Rest("DELETE", "/api/v1/sender-list/" + MiniJson.Number(entry, "id"));
            }
         }

         (int status, string body) domains = Rest("GET", "/api/v1/domain-policy-list");
         if (domains.status == 200)
         {
            foreach (object item in MiniJson.List(MiniJson.Object(domains.body), "items"))
            {
               var entry = (Dictionary<string, object>) item;
               if (Listed.Contains(MiniJson.Text(entry, "pattern")))
                  Rest("DELETE", "/api/v1/domain-policy-list/" + MiniJson.Number(entry, "id"));
            }
         }
      }

      /// <summary>
      ///    One SMTP transaction, answered with the reply to the final dot. greeting is
      ///    the whole HELO or EHLO line the client sends; headers may carry lines of the
      ///    client's own, a Received header among them, before the ones written here.
      /// </summary>
      private string Send(string greeting, string from, string subject, string body, string contentType = null, string headers = "")
      {
         var socket = new TcpConnection();
         ClassicAssert.IsTrue(socket.Connect(25), "Could not connect to SMTP.");
         socket.ReadUntil("220");

         socket.Send(greeting + "\r\n");
         socket.ReadUntil(greeting.StartsWith("EHLO", StringComparison.OrdinalIgnoreCase) ? "250 " : "250");

         socket.Send("MAIL FROM:<" + from + ">\r\n");
         string mailFrom = socket.ReadUntil("\r\n");
         if (!mailFrom.StartsWith("250"))
         {
            socket.Send("QUIT\r\n");
            socket.Disconnect();
            return mailFrom;
         }

         socket.Send("RCPT TO:<" + _recipient.Address + ">\r\n");
         socket.ReadUntil("\r\n");

         socket.Send("DATA\r\n");
         socket.ReadUntil("\r\n");

         // The null sender's report still names somebody in its header.
         socket.Send(headers +
                     "From: " + (from.Length == 0 ? "mailer-daemon@review-sender.test" : from) + "\r\n" +
                     "To: " + _recipient.Address + "\r\n" +
                     "Subject: " + subject + "\r\n" +
                     (contentType != null ? "Content-Type: " + contentType + "\r\n" : "") +
                     "\r\n" +
                     body + "\r\n" +
                     ".\r\n");

         string reply = socket.ReadUntil("\r\n");

         socket.Send("QUIT\r\n");
         socket.Disconnect();

         return reply;
      }

      private string SendSpam(string greeting, string from, string subject, string text = "Buy now")
      {
         string reply = Send(greeting, from, subject, text + " -> " + SpamLink + " <-");
         StringAssert.StartsWith("250", reply, "The spam must be accepted and held. Got: " + reply);
         return reply;
      }

      /// <summary>What GET /api/v1/quarantine says about the held message with this subject, or null.</summary>
      private static Dictionary<string, object> Held(string subject)
      {
         (int status, string body) list = Rest("GET", "/api/v1/quarantine");
         Assert.AreEqual(200, list.status, list.body);

         foreach (object item in (List<object>) MiniJson.Parse(list.body))
         {
            var entry = (Dictionary<string, object>) item;
            if (MiniJson.Text(entry, "subject") == subject)
               return entry;
         }

         return null;
      }

      private static long HeldId(string subject)
      {
         Dictionary<string, object> entry = Held(subject);
         Assert.IsNotNull(entry, "Nothing with the subject '" + subject + "' is held.");
         return MiniJson.Number(entry, "id");
      }

      private static Dictionary<string, object> Bulk(string requestBody, int expectedStatus = 200)
      {
         (int status, string body) answer = Rest("POST", "/api/v1/quarantine/bulk", requestBody);
         Assert.AreEqual(expectedStatus, answer.status, "POST /api/v1/quarantine/bulk " + requestBody + " answered: " + answer.body);
         return MiniJson.Object(answer.body);
      }

      private static Dictionary<string, object> ResultFor(Dictionary<string, object> answer, long id)
      {
         return MiniJson.List(answer, "results").Cast<Dictionary<string, object>>().Single(r => MiniJson.Number(r, "id") == id);
      }

      private static Dictionary<string, object> SenderListEntry(string value)
      {
         (int status, string body) list = Rest("GET", "/api/v1/sender-list");
         Assert.AreEqual(200, list.status, list.body);
         return MiniJson.List(MiniJson.Object(list.body), "items").Cast<Dictionary<string, object>>().FirstOrDefault(e => MiniJson.Text(e, "value") == value);
      }

      private static Dictionary<string, object> PolicyEntry(string pattern, string matchOn)
      {
         (int status, string body) list = Rest("GET", "/api/v1/domain-policy-list");
         Assert.AreEqual(200, list.status, list.body);
         return MiniJson.List(MiniJson.Object(list.body), "items").Cast<Dictionary<string, object>>()
            .FirstOrDefault(e => MiniJson.Text(e, "pattern") == pattern && MiniJson.Text(e, "match_on") == matchOn);
      }

      /// <summary>Waits for a message to reach the quarantine through the delivery queue.</summary>
      private static Dictionary<string, object> WaitUntilHeld(string subject)
      {
         for (int attempt = 0; attempt < 60; attempt++)
         {
            Dictionary<string, object> entry = Held(subject);
            if (entry != null)
               return entry;

            Thread.Sleep(250);
         }

         Assert.Fail("The message '" + subject + "' was never held.");
         return null;
      }

      // -------------------------------------------------------------------- tests

      [Test]
      [Description("The HELO and the EHLO name a client gives are stored with its held message, as it gave them, with the organizations' domains the pages group by")]
      public void TheHeloTheClientGaveIsStoredWithTheHold()
      {
         SendSpam("HELO client-one.review-helo.test", "outsider@mail.review-sender.test", "Held after HELO");
         SendSpam("EHLO Client-Two.Review-Helo.test", "outsider@review-sender.test", "Held after EHLO");

         Dictionary<string, object> first = Held("Held after HELO");
         Assert.IsNotNull(first);
         Assert.AreEqual("client-one.review-helo.test", MiniJson.Text(first, "helo"));
         Assert.AreEqual("review-helo.test", MiniJson.Text(first, "helo_domain"),
            "The group is the organization's domain, the registrable one: the host's name is not.");
         Assert.AreEqual("review-sender.test", MiniJson.Text(first, "sender_domain"));

         Dictionary<string, object> second = Held("Held after EHLO");
         Assert.AreEqual("Client-Two.Review-Helo.test", MiniJson.Text(second, "helo"), "Stored as the client gave it.");
         Assert.AreEqual("review-helo.test", MiniJson.Text(second, "helo_domain"), "Grouped without regard to case.");

         // And over COM, late-bound.
         dynamic quarantine = _application.Settings.AntiSpam.Quarantine;
         quarantine.Refresh();
         dynamic item = quarantine.ItemByDBID[(int) MiniJson.Number(first, "id")];
         Assert.AreEqual("client-one.review-helo.test", (string) item.HeloName);
         Assert.AreEqual("review-helo.test", (string) item.HeloDomain);
         Assert.AreEqual("review-sender.test", (string) item.SenderDomain);
      }

      [Test]
      [Description("A message held at delivery takes its HELO from the Received header this server wrote, never from one the client wrote itself")]
      public void AHoldAtDeliveryTakesTheHeloFromThisServersOwnReceivedHeader()
      {
         // The server's block list holds a blocked sender's copy at delivery, long
         // after the SMTP session has gone.
         (int status, string body) blocked = Rest("POST", "/api/v1/sender-list", "{\"value\":\"held-at-delivery@review-sender.test\",\"blocked\":true}");
         Assert.AreEqual(201, blocked.status, blocked.body);

         string reply = Send("HELO delivery-client.review-helo.test", "held-at-delivery@review-sender.test", "Held at delivery", "Not spam at all.",
            headers: "Received: from forged-helo.review-forged.test (forged [192.0.2.1]) by some.other.host with ESMTP id 1; Sun, 27 Sep 2026 10:00:00 +0000\r\n");
         StringAssert.StartsWith("250", reply);

         Dictionary<string, object> held = WaitUntilHeld("Held at delivery");
         StringAssert.Contains("block list", MiniJson.Text(held, "reason"),
            "The message must have been held at delivery, by the list, and not during the SMTP session.");
         Assert.AreEqual("delivery-client.review-helo.test", MiniJson.Text(held, "helo"),
            "The name this server recorded in its own Received header, not the one the client's own header claimed.");
      }

      [Test]
      [Description("One request releases several held messages and answers for each, an unknown id among them")]
      public void OneRequestReleasesSeveralAndAnswersForEach()
      {
         SendSpam("HELO bulk.review-helo.test", "outsider@review-sender.test", "Bulk one");
         SendSpam("HELO bulk.review-helo.test", "outsider@review-sender.test", "Bulk two");

         long one = HeldId("Bulk one");
         long two = HeldId("Bulk two");

         Dictionary<string, object> answer = Bulk("{\"ids\":[" + one + "," + two + "," + two + ",999999999],\"action\":\"release\"}");

         Assert.AreEqual(2, MiniJson.Number(answer, "succeeded"));
         Assert.AreEqual(1, MiniJson.Number(answer, "failed"));
         Assert.AreEqual(3, MiniJson.List(answer, "results").Count, "An id named twice is acted on once.");
         Assert.AreEqual("released", MiniJson.Text(ResultFor(answer, one), "outcome"));
         Assert.IsTrue(MiniJson.Flag(ResultFor(answer, two), "ok"));
         Assert.IsFalse(MiniJson.Flag(ResultFor(answer, 999999999), "ok"));
         StringAssert.Contains("No quarantined message", MiniJson.Text(ResultFor(answer, 999999999), "error"));

         Pop3ClientSimulator.AssertMessageCount(_recipient.Address, "test", 2);
         Assert.IsNull(Held("Bulk one"));
         Assert.IsNull(Held("Bulk two"));
      }

      [Test]
      [Description("A bulk request that is not one is refused whole, and changes nothing")]
      public void ARequestThatIsNotOneIsRefusedAndChangesNothing()
      {
         SendSpam("HELO refused.review-helo.test", "outsider@review-sender.test", "Still held");
         long id = HeldId("Still held");

         string[] refused =
         {
            "",
            "[]",
            "{\"action\":\"release\"}",
            "{\"ids\":[],\"action\":\"release\"}",
            "{\"ids\":[" + id + "],\"action\":\"whitelist\"}",
            "{\"ids\":[" + id + "],\"action\":\"release\",\"list\":\"address\"}",
            "{\"ids\":[" + id + "],\"action\":\"allow\",\"domain_from\":\"helo\"}",
            "{\"ids\":[" + id + "],\"action\":\"delete\",\"dry_run\":\"yes\"}",
            "{\"ids\":[\"" + id + "\"],\"action\":\"delete\"}",
            "{\"ids\":[" + id + "],\"action\":\"delete\",\"everything\":true}"
         };

         foreach (string body in refused)
         {
            (int status, string answer) = Rest("POST", "/api/v1/quarantine/bulk", body);
            Assert.AreEqual(400, status, "This body must be refused: " + body + " Answer: " + answer);
         }

         Assert.IsNotNull(Held("Still held"), "A refused request must leave the message where it was.");
         Pop3ClientSimulator.AssertMessageCount(_recipient.Address, "test", 0);
      }

      [Test]
      [Description("Whitelist by address writes the sender on the server's allow list and then releases; the same sender again is left as it is")]
      public void WhitelistingTheAddressWritesTheServersAllowListThenReleases()
      {
         SendSpam("HELO allow.review-helo.test", "allow-me@review-sender.test", "Allow me one");
         SendSpam("HELO allow.review-helo.test", "allow-me@review-sender.test", "Allow me two");

         long one = HeldId("Allow me one");
         long two = HeldId("Allow me two");

         // A dry run writes nothing and acts on nothing.
         Dictionary<string, object> dry = Bulk("{\"ids\":[" + one + "],\"action\":\"allow\",\"dry_run\":true}");
         Assert.AreEqual("add", MiniJson.Text((Dictionary<string, object>) MiniJson.List(dry, "entries")[0], "outcome"));
         Assert.IsNull(SenderListEntry("allow-me@review-sender.test"), "A dry run wrote an entry.");
         Assert.IsNotNull(Held("Allow me one"), "A dry run released a message.");

         Dictionary<string, object> answer = Bulk("{\"ids\":[" + one + "," + two + "],\"action\":\"allow\"}");

         List<object> entries = MiniJson.List(answer, "entries");
         Assert.AreEqual(1, entries.Count, "Two messages from one sender make one entry.");
         var entry = (Dictionary<string, object>) entries[0];
         Assert.AreEqual("allow-me@review-sender.test", MiniJson.Text(entry, "value"));
         Assert.AreEqual("address", MiniJson.Text(entry, "list"));
         Assert.AreEqual("added", MiniJson.Text(entry, "outcome"));
         Assert.AreEqual(2, MiniJson.Number(answer, "succeeded"));

         Dictionary<string, object> listed = SenderListEntry("allow-me@review-sender.test");
         Assert.IsNotNull(listed, "The sender must be on the server's allow and block list.");
         Assert.IsFalse(MiniJson.Flag(listed, "blocked"));
         Assert.AreEqual("server", MiniJson.Text(listed, "scope"));
         Assert.IsNull(PolicyEntry(".review-sender.test", "envelope"), "The address choice must not write the domain list.");

         Pop3ClientSimulator.AssertMessageCount(_recipient.Address, "test", 2);

         // The same sender held again is already allowed; nothing is written twice.
         SendSpam("HELO allow.review-helo.test", "allow-me@review-sender.test", "Allow me three");
         Dictionary<string, object> again = Bulk("{\"ids\":[" + HeldId("Allow me three") + "],\"action\":\"allow\",\"list\":\"address\"}");
         Assert.AreEqual("unchanged", MiniJson.Text((Dictionary<string, object>) MiniJson.List(again, "entries")[0], "outcome"));
      }

      [Test]
      [Description("Blacklist by address writes the block entry and deletes; the null sender cannot be listed and is left held")]
      public void BlacklistingTheAddressWritesTheBlockEntryThenDeletes()
      {
         SendSpam("HELO block.review-helo.test", "block-me@review-sender.test", "Block me");
         SendSpam("HELO block.review-helo.test", "", "From the null sender");

         long blocked = HeldId("Block me");
         long bounce = HeldId("From the null sender");

         Dictionary<string, object> answer = Bulk("{\"ids\":[" + blocked + "," + bounce + "],\"action\":\"block\",\"list\":\"address\"}");

         Assert.AreEqual("deleted", MiniJson.Text(ResultFor(answer, blocked), "outcome"));
         Assert.IsFalse(MiniJson.Flag(ResultFor(answer, bounce), "ok"));
         StringAssert.Contains("null sender", MiniJson.Text(ResultFor(answer, bounce), "error"));

         Dictionary<string, object> listed = SenderListEntry("block-me@review-sender.test");
         Assert.IsNotNull(listed);
         Assert.IsTrue(MiniJson.Flag(listed, "blocked"));

         Assert.IsNull(Held("Block me"), "Blacklist deletes.");
         Assert.IsNotNull(Held("From the null sender"), "A message no entry can be made for is left where it is.");
         Pop3ClientSimulator.AssertMessageCount(_recipient.Address, "test", 0);
      }

      [Test]
      [Description("Blacklist by the HELO name's domain writes a black row matched on the HELO on the domain list, which then refuses that HELO at MAIL FROM")]
      public void BlacklistingTheHeloDomainWritesTheDomainListAndItRefuses()
      {
         SendSpam("HELO mx1.review-helo-block.test", "outsider@review-sender.test", "Block the HELO");
         long id = HeldId("Block the HELO");

         Dictionary<string, object> answer = Bulk("{\"ids\":[" + id + "],\"action\":\"block\",\"list\":\"domain\",\"domain_from\":\"helo\"}");

         var entry = (Dictionary<string, object>) MiniJson.List(answer, "entries")[0];
         Assert.AreEqual(".review-helo-block.test", MiniJson.Text(entry, "value"),
            "The organization's domain and everything beneath it.");
         Assert.AreEqual("helo", MiniJson.Text(entry, "match_on"));

         Dictionary<string, object> row = PolicyEntry(".review-helo-block.test", "helo");
         Assert.IsNotNull(row, "The row must be on the domain white and black list.");
         Assert.AreEqual("black", MiniJson.Text(row, "action"));
         Assert.IsNull(SenderListEntry("outsider@review-sender.test"), "The domain choice must not write the server's allow and block list.");
         Assert.IsNull(Held("Block the HELO"));

         // Another host of the same organization is refused before anything else runs.
         string reply = Send("HELO mx2.review-helo-block.test", "someone@review-sender.test", "Refused", "hello");
         StringAssert.StartsWith("550", reply, "A black HELO domain is refused at MAIL FROM. Got: " + reply);
      }

      [Test]
      [Description("Whitelist by the sender's domain writes a white row matched on the envelope, and its next message is no longer held on the score")]
      public void WhitelistingTheSenderDomainWritesAWhiteRowAndTheNextIsNotHeld()
      {
         SendSpam("HELO mx.review-helo.test", "news@bounces.review-sender-allow.test", "White the domain");
         long id = HeldId("White the domain");

         Dictionary<string, object> answer = Bulk("{\"ids\":[" + id + "],\"action\":\"allow\",\"list\":\"domain\"}");
         Assert.AreEqual(1, MiniJson.Number(answer, "succeeded"));

         Dictionary<string, object> row = PolicyEntry(".review-sender-allow.test", "envelope");
         Assert.IsNotNull(row);
         Assert.AreEqual("white", MiniJson.Text(row, "action"));

         Pop3ClientSimulator.AssertMessageCount(_recipient.Address, "test", 1);

         // The next one from anywhere beneath the organization is not held on the
         // score: white is not the accept gate's to refuse.
         SendSpam("HELO mx.review-helo.test", "offers@review-sender-allow.test", "White again");
         Assert.IsNull(Held("White again"), "A white domain's message must not be held on the score.");
         Pop3ClientSimulator.AssertMessageCount(_recipient.Address, "test", 2);
      }

      [Test]
      [Description("A HELO that is an address without brackets, stored as the client gave it, has no HELO domain: blocking by the HELO domain writes nothing for it and leaves it held")]
      public void AHeloThatIsAnAddressHasNoDomainToList()
      {
         SendSpam("HELO 192.0.2.77", "outsider@review-sender.test", "Address HELO");

         Dictionary<string, object> held = Held("Address HELO");
         Assert.IsNotNull(held);
         Assert.AreEqual("192.0.2.77", MiniJson.Text(held, "helo"), "Stored as the client gave it.");
         Assert.AreEqual("", MiniJson.Text(held, "helo_domain"),
            "An address has no organization's domain; the Public Suffix List's implicit rule would have made it 2.77.");

         long id = MiniJson.Number(held, "id");
         Dictionary<string, object> answer = Bulk("{\"ids\":[" + id + "],\"action\":\"block\",\"list\":\"domain\",\"domain_from\":\"helo\"}");

         Assert.AreEqual(0, MiniJson.List(answer, "entries").Count, "No entry can be made for an address.");
         Assert.IsFalse(MiniJson.Flag(ResultFor(answer, id), "ok"));
         StringAssert.Contains("address", MiniJson.Text(ResultFor(answer, id), "error"));
         Assert.IsNull(PolicyEntry(".2.77", "helo"),
            "Nothing may be written on the domain list: .2.77 would refuse every client whose HELO is an address ending in .2.77.");
         Assert.IsNotNull(Held("Address HELO"), "A message no entry can be made for is left where it is.");
      }

      [Test]
      [Description("A domain that is itself a public suffix is named in a dry run, refused with 409 and nothing changed, and written only when the request confirms it")]
      public void APublicSuffixIsWrittenOnlyWhenConfirmed()
      {
         SendSpam("HELO co.uk", "outsider@review-sender.test", "Public suffix HELO");
         long id = HeldId("Public suffix HELO");

         string request = "{\"ids\":[" + id + "],\"action\":\"block\",\"list\":\"domain\",\"domain_from\":\"helo\"";

         Dictionary<string, object> dry = Bulk(request + ",\"dry_run\":true}");
         var planned = (Dictionary<string, object>) MiniJson.List(dry, "entries")[0];
         Assert.AreEqual(".co.uk", MiniJson.Text(planned, "value"));
         Assert.IsTrue(MiniJson.Flag(planned, "public_suffix"));

         (int status, string body) refused = Rest("POST", "/api/v1/quarantine/bulk", request + "}");
         Assert.AreEqual(409, refused.status, refused.body);
         StringAssert.Contains("public suffix", refused.body);
         Assert.IsNull(PolicyEntry(".co.uk", "helo"), "Nothing may be written without the confirmation.");
         Assert.IsNotNull(Held("Public suffix HELO"), "Nothing may be deleted without the confirmation.");

         Dictionary<string, object> confirmed = Bulk(request + ",\"confirm_public_suffix\":true}");
         Assert.AreEqual(1, MiniJson.Number(confirmed, "succeeded"));
         Assert.IsNotNull(PolicyEntry(".co.uk", "helo"));
         Assert.IsNull(Held("Public suffix HELO"));
      }

      [Test]
      [Description("The headers and a plain-text preview are read without releasing; an HTML-only message is previewed as text")]
      public void HeadersAndAPreviewAreReadWithoutReleasing()
      {
         SendSpam("HELO preview.review-helo.test", "outsider@review-sender.test", "Preview subject 77", "Plain body words for the preview");

         string html = "<html><head><style>.x { color: red }</style></head><body><p>Html body words</p> <a href=\"" + SpamLink + "\">link</a></body></html>";
         StringAssert.StartsWith("250", Send("HELO preview.review-helo.test", "outsider@review-sender.test", "Html only", html, "text/html; charset=utf-8"));

         long plain = HeldId("Preview subject 77");
         long htmlOnly = HeldId("Html only");

         (int status, string body) headers = Rest("GET", "/api/v1/quarantine/" + plain + "/headers");
         Assert.AreEqual(200, headers.status, headers.body);
         Dictionary<string, object> read = MiniJson.Object(headers.body);
         StringAssert.Contains("Subject: Preview subject 77", MiniJson.Text(read, "headers"));
         StringAssert.Contains("Received: from preview.review-helo.test", MiniJson.Text(read, "headers"));
         Assert.IsFalse(MiniJson.Flag(read, "truncated"));
         Assert.AreEqual(65536, MiniJson.Number(read, "limit"));

         (int status, string body) preview = Rest("GET", "/api/v1/quarantine/" + plain + "/preview");
         Assert.AreEqual(200, preview.status, preview.body);
         Dictionary<string, object> text = MiniJson.Object(preview.body);
         StringAssert.Contains("Plain body words for the preview", MiniJson.Text(text, "text"));
         Assert.IsFalse(MiniJson.Flag(text, "from_html"));

         (int status, string body) htmlPreview = Rest("GET", "/api/v1/quarantine/" + htmlOnly + "/preview");
         Assert.AreEqual(200, htmlPreview.status, htmlPreview.body);
         Dictionary<string, object> fromHtml = MiniJson.Object(htmlPreview.body);
         Assert.IsTrue(MiniJson.Flag(fromHtml, "from_html"));
         StringAssert.Contains("Html body words", MiniJson.Text(fromHtml, "text"));
         StringAssert.DoesNotContain("<p>", MiniJson.Text(fromHtml, "text"), "The preview is text: the markup is taken out.");
         StringAssert.DoesNotContain("color", MiniJson.Text(fromHtml, "text"), "A style sheet is not text a person reads.");

         Assert.AreEqual(404, Rest("GET", "/api/v1/quarantine/999999999/headers").status);
         Assert.AreEqual(404, Rest("GET", "/api/v1/quarantine/999999999/preview").status);

         // Over COM, late-bound, the same.
         dynamic quarantine = _application.Settings.AntiSpam.Quarantine;
         dynamic item = quarantine.ItemByDBID[(int) plain];
         StringAssert.Contains("Subject: Preview subject 77", (string) item.ReadHeaders());
         StringAssert.Contains("Plain body words for the preview", (string) item.ReadPreview());

         Assert.IsNotNull(Held("Preview subject 77"), "Reading a held message must not release it.");
         Pop3ClientSimulator.AssertMessageCount(_recipient.Address, "test", 0);
      }

      [Test]
      [Description("The expired sweep runs over REST: past QuarantineRetentionDays it deletes, and with 0 days it keeps everything")]
      public void TheExpiredSweepRunsOverRest()
      {
         int old = SingletonProvider<TestSetup>.Instance.GetApp().Database.ExecuteSQLWithReturn(
            "insert into hm_quarantine (quarantinefilename, quarantinesender, quarantinerecipients, quarantinesubject, quarantinereason, quarantinescore, quarantinesize, quarantinecreated) " +
            "values ('review-sweep-no-such-file.eml', 'old@review-sender.test', 'reviewed@example.test', 'Held long ago', 'old', 12, 345, DATEADD(day, -30, GETDATE()))");

         try
         {
            ServerIniFile.SetSetting("QuarantineRetentionDays", "0");
            _application.Reinitialize();

            (int status, string body) kept = Rest("POST", "/api/v1/quarantine/expire", "{}");
            Assert.AreEqual(200, kept.status, kept.body);
            Assert.AreEqual(0, MiniJson.Number(MiniJson.Object(kept.body), "removed"), "0 days keeps everything.");
            Assert.AreEqual(0, MiniJson.Number(MiniJson.Object(kept.body), "retention_days"));
            Assert.IsNotNull(Held("Held long ago"));

            ServerIniFile.SetSetting("QuarantineRetentionDays", "7");
            _application.Reinitialize();

            (int status, string body) swept = Rest("POST", "/api/v1/quarantine/expire", "{}");
            Assert.AreEqual(200, swept.status, swept.body);
            Assert.GreaterOrEqual(MiniJson.Number(MiniJson.Object(swept.body), "removed"), 1);
            Assert.AreEqual(7, MiniJson.Number(MiniJson.Object(swept.body), "retention_days"));
            Assert.IsNull(Held("Held long ago"), "What is past the window must be gone.");
         }
         finally
         {
            SingletonProvider<TestSetup>.Instance.GetApp().Database.ExecuteSQL("delete from hm_quarantine where quarantineid = " + old);
         }
      }

      [Test]
      [Description("The new routes ask the permissions they document: allow and block need manage_settings beside manage_quarantine, the headers view_mailbox, and a confined key reaches none of them")]
      public void TheNewRoutesAskTheirPermissions()
      {
         SendSpam("HELO keys.review-helo.test", "allow-me@review-sender.test", "Asked of keys");
         long id = HeldId("Asked of keys");

         var revoke = new List<string>();
         try
         {
            (string id, string token) Key(string fields)
            {
               (int status, string body) created = Rest("POST", "/api/v1/apikeys", "{\"label\":\"quarantine review " + revoke.Count + "\"," + fields + "}");
               Assert.AreEqual(201, created.status, created.body);
               Dictionary<string, object> key = MiniJson.Object(created.body);
               revoke.Add(MiniJson.Text(key, "id"));
               return (MiniJson.Text(key, "id"), MiniJson.Text(key, "key"));
            }

            // The domain administrator's role holds manage_quarantine and not manage_settings.
            (string id, string token) domainAdmin = Key("\"role\":\"domainadmin\"");
            (int status, string body) allow = WithKey(domainAdmin.token, "POST", "/api/v1/quarantine/bulk", "{\"ids\":[" + id + "],\"action\":\"allow\"}");
            Assert.AreEqual(403, allow.status, "Allow writes the server's list, which is manage_settings'. Body: " + allow.body);
            Assert.IsNull(SenderListEntry("allow-me@review-sender.test"));
            Assert.AreEqual(200, WithKey(domainAdmin.token, "GET", "/api/v1/quarantine/" + id + "/headers").status);

            // A read-only key is the auditor: it reads a held message and changes nothing.
            (string id, string token) readOnly = Key("\"scope\":\"readonly\"");
            Assert.AreEqual(200, WithKey(readOnly.token, "GET", "/api/v1/quarantine/" + id + "/preview").status);
            Assert.AreEqual(403, WithKey(readOnly.token, "POST", "/api/v1/quarantine/bulk", "{\"ids\":[" + id + "],\"action\":\"release\"}").status);
            Assert.AreEqual(403, WithKey(readOnly.token, "POST", "/api/v1/quarantine/expire", "{}").status);

            // A key confined to named domains reaches none of the quarantine.
            (string id, string token) confined = Key("\"scope\":\"full\",\"domains\":\"example.test\"");
            Assert.AreEqual(403, WithKey(confined.token, "GET", "/api/v1/quarantine/" + id + "/headers").status);
            Assert.AreEqual(403, WithKey(confined.token, "POST", "/api/v1/quarantine/bulk", "{\"ids\":[" + id + "],\"action\":\"release\"}").status);

            Assert.IsNotNull(Held("Asked of keys"), "Nothing refused may have acted.");

            // And the domain administrator may release.
            (int status, string body) release = WithKey(domainAdmin.token, "POST", "/api/v1/quarantine/bulk", "{\"ids\":[" + id + "],\"action\":\"release\"}");
            Assert.AreEqual(200, release.status, release.body);
            Pop3ClientSimulator.AssertMessageCount(_recipient.Address, "test", 1);
         }
         finally
         {
            foreach (string key in revoke)
               Rest("DELETE", "/api/v1/apikeys/" + key);
         }
      }

      [Test]
      [Description("Quarantine.Bulk over COM takes and answers the REST route's JSON")]
      public void TheComBulkMethodTakesTheSameRequest()
      {
         SendSpam("HELO com.review-helo.test", "outsider@review-sender.test", "Over COM");
         long id = HeldId("Over COM");

         dynamic quarantine = _application.Settings.AntiSpam.Quarantine;
         string answer = (string) quarantine.Bulk("{\"ids\":[" + id + "],\"action\":\"delete\"}");

         Dictionary<string, object> read = MiniJson.Object(answer);
         Assert.AreEqual(1, MiniJson.Number(read, "succeeded"), answer);
         Assert.IsNull(Held("Over COM"));

         Assert.Throws<System.Runtime.InteropServices.COMException>(() => quarantine.Bulk("{\"ids\":[1],\"action\":\"nuke\"}"),
            "A request that is not one is refused over COM too.");
      }
   }
}
