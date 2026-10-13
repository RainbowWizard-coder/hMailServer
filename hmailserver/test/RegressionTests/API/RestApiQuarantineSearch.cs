// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.API
{
   /// <summary>
   ///    The quarantine's search over REST (review finding F261): GET
   ///    /api/v1/quarantine?q= looks through every held message - not only the
   ///    newest thousand the list answers - in the sender, the recipients, the
   ///    subject and the reason, in any case and as the text is written, and pages
   ///    newest first; GET /api/v1/me/quarantine?q= does the same for one account's
   ///    own, and never looks in the recipients.
   ///
   ///    Every message here is held the way a real one is - a link the fixture's
   ///    own URI list names scores 10 over a delete threshold of 5 with
   ///    QuarantineEnabled on - and the list is made and taken back, and the
   ///    quarantine read and emptied, over REST alone, so that the fixture runs on
   ///    the Linux server as it does on Windows, against PostgreSQL there and SQL
   ///    Server Compact here: the two databases whose LIKE treats case and the
   ///    bracket differently. (AntiSpam.Quarantine's recipe turns the SURBL test
   ///    point's list on through the COM collection, which the Linux suite's
   ///    stand-in does not carry: every test would be skipped there.)
   /// </summary>
   [TestFixture]
   public class RestApiQuarantineSearch : TestFixtureBase
   {
      // The port the listener answers on: this one on the Windows bench (the range
      // batch53's quarantine search was given), the suite's own where RestListener
      // finds one already on.
      private static int RestPort = 47580;
      private const string AdminPassword = "testar";
      private const string UserPassword = "Quarantine-Search-1!";

      // The fixture's own URI list: the zone it is asked under, which the suite's
      // DNS serves, the one domain it lists, and the reason a hold records.
      private const string Zone = "quarantine-search.test";
      private const string ListedDomain = "held-for-search.test";
      private const string ListReason = "Listed by the quarantine search test list";
      private const string SpamBody =
         "This is a test message with a listed url: -> http://" + ListedDomain + "/ <-";

      // Whether the set-up got as far as the REST API answering, which is what the
      // tear-down may then ask.
      private bool restUp_;

      // Two accounts, the second with an underscore in it, which a LIKE reads as
      // any one character unless it is escaped.
      private string Ada => "qsearch-ada@" + _domain.Name;
      private string Bob => "qsearch_bob@" + _domain.Name;

      [OneTimeSetUp]
      public void ServeTheListedDomain()
      {
         SuiteDns.Zone.WithA(ListedDomain + "." + Zone, "127.0.0.2");
      }

      [OneTimeTearDown]
      public void RestoreTheSuiteDns()
      {
         SuiteDns.Reset();
      }

      [SetUp]
      public void StartRestApiAndHoldSpam()
      {
         _settings.SetAdministratorPassword(AdminPassword);

         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, Ada, UserPassword);
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, Bob, UserPassword);

         var antiSpam = _application.Settings.AntiSpam;
         antiSpam.SpamMarkThreshold = 1;
         antiSpam.SpamDeleteThreshold = 5;

         RestPort = RestListener.Start(RestPort);
         ServerIniFile.SetSetting("QuarantineEnabled", "1");
         _application.Reinitialize();

         (int status, string body) probe = Http("GET", "/api/v1/status");
         Assert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);
         restUp_ = true;

         TakeBackOurList();
         (int status, string body) created = Http("POST", "/api/v1/surbl-servers", null,
            "{\"dns_host\":\"" + Zone + "\",\"expected_result\":\"\",\"reject_message\":\"" + ListReason + "\",\"score\":10,\"active\":true}");
         Assert.AreEqual(201, created.status, created.body);

         EmptyTheQuarantine();
      }

      // Takes back what the set-up made, and asks the REST API nothing when it
      // never answered, so that a set-up that failed - or was skipped - is what
      // the test reports, not a refused connection here.
      [TearDown]
      public void StopRestApi()
      {
         try
         {
            if (restUp_)
            {
               EmptyTheQuarantine();
               TakeBackOurList();
            }
         }
         finally
         {
            restUp_ = false;
            ServerIniFile.SetSetting("QuarantineEnabled", null);
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      // ------------------------------------------------------------- helpers

      private static string AdminHeader()
      {
         return BasicHeader("Administrator", AdminPassword);
      }

      private static string BasicHeader(string user, string password)
      {
         return "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(user + ":" + password));
      }

      private static (int status, string body) Http(string method, string path, string authorization = null, string requestBody = null)
      {
         using (var client = new TcpClient())
         {
            Exception last = null;
            for (int attempt = 0; attempt < 25; attempt++)
            {
               try
               {
                  client.Connect(TestPorts.HostAddress, RestPort);
                  last = null;
                  break;
               }
               catch (SocketException ex)
               {
                  last = ex;
                  Thread.Sleep(200);
               }
            }

            if (last != null)
               throw last;

            using (NetworkStream stream = client.GetStream())
            using (var memory = new MemoryStream())
            {
               byte[] content = requestBody == null ? new byte[0] : Encoding.UTF8.GetBytes(requestBody);
               byte[] request = Encoding.ASCII.GetBytes(
                  method + " " + path + " HTTP/1.0\r\n" +
                  "Host: 127.0.0.1\r\n" +
                  "Authorization: " + (authorization ?? AdminHeader()) + "\r\n" +
                  (requestBody == null ? "" : "Content-Type: application/json\r\nContent-Length: " + content.Length + "\r\n") +
                  "Connection: close\r\n\r\n");

               stream.Write(request, 0, request.Length);
               stream.Write(content, 0, content.Length);

               byte[] buffer = new byte[4096];
               int read;
               while ((read = stream.Read(buffer, 0, buffer.Length)) > 0)
                  memory.Write(buffer, 0, read);

               string raw = Encoding.UTF8.GetString(memory.ToArray());

               int statusCode = 0;
               string[] lines = raw.Split(new[] { "\r\n" }, StringSplitOptions.None);
               if (lines.Length > 0)
               {
                  string[] parts = lines[0].Split(' ');
                  if (parts.Length >= 2)
                     int.TryParse(parts[1], out statusCode);
               }

               int separator = raw.IndexOf("\r\n\r\n", StringComparison.Ordinal);
               return (statusCode, separator >= 0 ? raw.Substring(separator + 4) : "");
            }
         }
      }

      // GET /api/v1/quarantine?... as an object: the page and its numbers, with the
      // answer as it came under "body" for the failure messages.
      private static Dictionary<string, object> Page(string query, string authorization = null, string path = "/api/v1/quarantine")
      {
         (int status, string body) = Http("GET", path + "?" + query, authorization);
         Assert.AreEqual(200, status, path + "?" + query + " answered " + body);
         Dictionary<string, object> page = MiniJson.Object(body);
         page["body"] = body;
         return page;
      }

      private static List<Dictionary<string, object>> Messages(Dictionary<string, object> page)
      {
         return MiniJson.List(page, "messages").Cast<Dictionary<string, object>>().ToList();
      }

      private static List<string> Subjects(Dictionary<string, object> page)
      {
         return Messages(page).Select(m => MiniJson.Text(m, "subject")).ToList();
      }

      private static int Number(Dictionary<string, object> page, string key)
      {
         Assert.IsTrue(page.ContainsKey(key), "The answer has no " + key + ": " + MiniJson.Text(page, "body"));
         return (int) MiniJson.Number(page, key);
      }

      private static string Q(string text)
      {
         return "q=" + Uri.EscapeDataString(text);
      }

      // Every held message, over REST: the list is the newest thousand, and the
      // tests hold a handful.
      private static void EmptyTheQuarantine()
      {
         for (int round = 0; round < 10; round++)
         {
            (int status, string body) = Http("GET", "/api/v1/quarantine");
            Assert.AreEqual(200, status, body);

            List<long> ids = ((List<object>) MiniJson.Parse(body)).Cast<Dictionary<string, object>>()
               .Select(m => MiniJson.Number(m, "id")).ToList();
            if (ids.Count == 0)
               return;

            foreach (long id in ids)
               Http("DELETE", "/api/v1/quarantine/" + id);
         }

         Assert.Fail("The quarantine could not be emptied.");
      }

      // Deletes every URI list on the fixture's zone: the set-up's, and one an
      // interrupted run left behind.
      private static void TakeBackOurList()
      {
         (int status, string body) = Http("GET", "/api/v1/surbl-servers");
         Assert.AreEqual(200, status, body);

         object parsed = MiniJson.Parse(body);
         var items = parsed as List<object> ?? MiniJson.List((Dictionary<string, object>) parsed, "items");

         foreach (Dictionary<string, object> item in items.Cast<Dictionary<string, object>>())
         {
            if (string.Equals(MiniJson.Text(item, "dns_host"), Zone, StringComparison.OrdinalIgnoreCase))
               Http("DELETE", "/api/v1/surbl-servers/" + MiniJson.Number(item, "id"));
         }
      }

      // Sends one spam message and asserts it was held: the reply to the final dot
      // is 250, where a refusal is 550.
      private static void Hold(string sender, string subject, params string[] recipients)
      {
         var socket = new TcpConnection();
         Assert.IsTrue(socket.Connect(25), "Could not connect to SMTP.");
         socket.ReadUntil("220");
         socket.Send("HELO qsearch.example\r\n");
         socket.ReadUntil("250");
         socket.Send("MAIL FROM:<" + sender + ">\r\n");
         socket.ReadUntil("\r\n");
         foreach (string recipient in recipients)
         {
            socket.Send("RCPT TO:<" + recipient + ">\r\n");
            socket.ReadUntil("\r\n");
         }
         socket.Send("DATA\r\n");
         socket.ReadUntil("\r\n");
         socket.Send("From: " + sender + "\r\n" +
                     "To: " + string.Join(", ", recipients) + "\r\n" +
                     "Subject: " + subject + "\r\n" +
                     "\r\n" +
                     SpamBody + "\r\n" +
                     ".\r\n");
         string reply = socket.ReadUntil("\r\n");
         socket.Send("QUIT\r\n");
         socket.Disconnect();
         StringAssert.StartsWith("250", reply, "A quarantined message is accepted, not refused: " + subject);
      }

      // ---------------------------------------------------------------- tests

      [Test]
      [Description("q is looked for in the sender, the recipients, the subject and the reason, in any case")]
      public void TheSearchLooksInTheSenderTheRecipientsTheSubjectAndTheReason()
      {
         Hold("a_b@spam-sender.test", "Invoice 7", Ada);
         Hold("axb@spam-sender.test", "Monthly statement", Bob);
         Hold("parcels@spam-sender.test", "Your parcel", Ada, Bob);

         // The sender, its underscore as the character it is: axb@ is not found.
         Assert.AreEqual(new[] { "Invoice 7" }, Subjects(Page(Q("A_B@SPAM"))).ToArray());

         // The recipients, in any case: both messages held for Bob, newest first.
         Assert.AreEqual(new[] { "Your parcel", "Monthly statement" }, Subjects(Page(Q(Bob.ToUpperInvariant()))).ToArray());

         // The subject.
         Assert.AreEqual(new[] { "Invoice 7" }, Subjects(Page(Q("invoice"))).ToArray());

         // The reason, read off the list and asked for in the other case.
         (int status, string listed) = Http("GET", "/api/v1/quarantine");
         Assert.AreEqual(200, status, listed);
         string reason = MiniJson.Text((Dictionary<string, object>) ((List<object>) MiniJson.Parse(listed))[0], "reason");
         Assert.IsTrue(reason.Length >= 6, "The hold recorded no reason: " + listed);
         string part = reason.Substring(0, 6);
         string flipped = new string(part.Select(c => char.IsUpper(c) ? char.ToLowerInvariant(c) : char.ToUpperInvariant(c)).ToArray());
         Dictionary<string, object> byReason = Page(Q(flipped));
         Assert.AreEqual(3, Number(byReason, "total"), "Every hold of the same verdict has the reason: " + MiniJson.Text(byReason, "body"));

         // Nothing anywhere.
         Dictionary<string, object> none = Page(Q("nowhere-to-be-found"));
         Assert.AreEqual(0, Number(none, "total"));
         Assert.AreEqual(0, Messages(none).Count);
      }

      [Test]
      [Description("q is matched as it is written: %, _, [, a quote, a backslash and a slash are the characters they are on every database")]
      public void TheTextIsMatchedAsItIsWritten()
      {
         Hold("promo@spam-sender.test", "100% off", Ada);
         Hold("promo@spam-sender.test", "100 percent off", Ada);
         Hold("promo@spam-sender.test", "[promo] sale", Ada);
         Hold("promo@spam-sender.test", "p", Ada);
         Hold("promo@spam-sender.test", "O'Brien's report", Ada);
         Hold("promo@spam-sender.test", "back\\slash and a/b", Ada);

         Assert.AreEqual(new[] { "100% off" }, Subjects(Page(Q("100%"))).ToArray(), "% found every subject with 100 in it.");
         Assert.AreEqual(new[] { "100% off" }, Subjects(Page(Q("%"))).ToArray());
         // A bracket opens a character class on SQL Server and Compact: [promo] would
         // find the subject "p".
         Assert.AreEqual(new[] { "[promo] sale" }, Subjects(Page(Q("[promo]"))).ToArray());
         Assert.AreEqual(new[] { "O'Brien's report" }, Subjects(Page(Q("o'brien"))).ToArray());
         Assert.AreEqual(new[] { "back\\slash and a/b" }, Subjects(Page(Q("\\"))).ToArray());
         Assert.AreEqual(new[] { "back\\slash and a/b" }, Subjects(Page(Q("a/b"))).ToArray());
      }

      [Test]
      [Description("Capitals outside ASCII are matched in any case, as the database folds them: on PostgreSQL its lower() folds the text as it folds the column, where the server's own lowering left them capitals and found nothing, not even the text itself")]
      public void CapitalsOutsideAsciiAreMatchedInAnyCase()
      {
         Hold("transfers@spam-sender.test", Encoded("Überweisung erhalten"), Ada);
         Hold("transfers@spam-sender.test", Encoded("Счёт на оплату"), Ada);
         Hold("transfers@spam-sender.test", "Ueberweisung, spelt out", Ada);

         foreach (string text in new[] { "Überweisung", "ÜBERWEISUNG", "überweisung" })
         {
            Dictionary<string, object> found = Page(Q(text));
            Assert.AreEqual(1, Number(found, "total"), text + ": " + MiniJson.Text(found, "body"));
            Assert.AreEqual(new[] { "Überweisung erhalten" }, Subjects(found).ToArray(), text);
         }

         Dictionary<string, object> cyrillic = Page(Q("СЧЁТ"));
         Assert.AreEqual(new[] { "Счёт на оплату" }, Subjects(cyrillic).ToArray(), MiniJson.Text(cyrillic, "body"));

         // The account's own search, and the address it is confined to, are lowered
         // the same way.
         Dictionary<string, object> own = Page(Q("ÜBERWEISUNG"), BasicHeader(Ada, UserPassword), "/api/v1/me/quarantine");
         Assert.AreEqual(1, Number(own, "total"), MiniJson.Text(own, "body"));
         Assert.AreEqual(new[] { "Überweisung erhalten" }, Subjects(own).ToArray());
      }

      // A header value outside ASCII as a sender writes it: an RFC 2047 encoded word,
      // which the store decodes when it records the subject.
      private static string Encoded(string text)
      {
         return "=?utf-8?B?" + Convert.ToBase64String(Encoding.UTF8.GetBytes(text)) + "?=";
      }

      [Test]
      [Description("A search pages newest first, counting every match, with held beside it; no q and a page is every message")]
      public void ASearchPagesNewestFirstAndCountsEveryMatch()
      {
         Hold("pages@spam-sender.test", "Page one", Ada);
         Hold("pages@spam-sender.test", "Page two", Ada);
         Hold("pages@spam-sender.test", "Page three", Ada);
         Hold("other@spam-sender.test", "Something else", Ada);

         Dictionary<string, object> first = Page(Q("page") + "&limit=2");
         Assert.AreEqual(3, Number(first, "total"));
         Assert.AreEqual(4, Number(first, "held"));
         Assert.AreEqual(0, Number(first, "offset"));
         Assert.AreEqual(2, Number(first, "count"));
         Assert.AreEqual("page", MiniJson.Text(first, "query"));
         Assert.AreEqual(new[] { "Page three", "Page two" }, Subjects(first).ToArray());

         Dictionary<string, object> second = Page(Q("page") + "&limit=2&offset=2");
         Assert.AreEqual(3, Number(second, "total"));
         Assert.AreEqual(new[] { "Page one" }, Subjects(second).ToArray());

         Dictionary<string, object> beyond = Page(Q("page") + "&offset=3");
         Assert.AreEqual(3, Number(beyond, "total"));
         Assert.AreEqual(0, Number(beyond, "count"));
         Assert.AreEqual(0, Messages(beyond).Count);

         // No words, a page: every message, and held and total are one number.
         Dictionary<string, object> every = Page("offset=1&limit=2");
         Assert.AreEqual(4, Number(every, "total"));
         Assert.AreEqual(4, Number(every, "held"));
         Assert.AreEqual(new[] { "Page three", "Page two" }, Subjects(every).ToArray());

         // Each message as the list writes it.
         Dictionary<string, object> one = Messages(first)[0];
         foreach (string key in new[] { "id", "sender", "recipients", "subject", "reason", "score", "size", "created", "helo", "sender_domain", "helo_domain" })
            Assert.IsTrue(one.ContainsKey(key), "A message found has no " + key + ": " + MiniJson.Text(first, "body"));
         Assert.AreEqual("pages@spam-sender.test", MiniJson.Text(one, "sender"));
         Assert.AreEqual("spam-sender.test", MiniJson.Text(one, "sender_domain"));
      }

      [Test]
      [Description("With none of q, offset and limit the list is the bare array it always was, and an empty q is none")]
      public void WithNoSearchTheListIsTheArrayItWas()
      {
         Hold("array@spam-sender.test", "Still an array", Ada);

         foreach (string path in new[] { "/api/v1/quarantine", "/api/v1/quarantine?q=", "/api/v1/quarantine?q=+++" })
         {
            (int status, string body) = Http("GET", path);
            Assert.AreEqual(200, status, body);
            StringAssert.StartsWith("[", body.TrimStart(), path + " is not the array: " + body);
            StringAssert.Contains("\"subject\":\"Still an array\"", body);
         }
      }

      [Test]
      [Description("A q longer than 255 characters, and an offset or a limit that is not a whole number in its range, are refused with 400")]
      public void WhatIsOutOfBoundsIsRefused()
      {
         string longest = new string('x', 255);
         Assert.AreEqual(200, Http("GET", "/api/v1/quarantine?" + Q(longest)).status, "255 characters is a search.");
         Assert.AreEqual(400, Http("GET", "/api/v1/quarantine?" + Q(longest + "y")).status, "256 characters is refused, not cut.");

         foreach (string query in new[] { "limit=0", "limit=1001", "limit=ten", "limit=-1", "offset=-1", "offset=x", "offset=1000000000", "limit=2.5" })
         {
            (int status, string body) = Http("GET", "/api/v1/quarantine?" + query);
            Assert.AreEqual(400, status, query + " answered " + body);
         }

         Assert.AreEqual(200, Http("GET", "/api/v1/quarantine?limit=1000&offset=999999999").status);
         Assert.AreEqual(400, Http("GET", "/api/v1/me/quarantine?" + Q(longest + "y"), BasicHeader(Ada, UserPassword)).status);
         Assert.AreEqual(400, Http("GET", "/api/v1/me/quarantine?limit=0", BasicHeader(Ada, UserPassword)).status);
      }

      [Test]
      [Description("A search is not on the audit trail, as the list is not - the quarantine is one of the server's working lists - while reading a held message's headers is")]
      public void ASearchIsAWorkingListAsTheListIs()
      {
         string words = "unrecorded " + Guid.NewGuid().ToString("N").Substring(0, 8);
         Hold("audit@spam-sender.test", "Held for the " + words, Ada);

         Dictionary<string, object> found = Page(Q(words));
         Assert.AreEqual(1, Number(found, "total"));
         long id = MiniJson.Number(Messages(found)[0], "id");

         (int status, string trail) = Http("GET", "/api/v1/audit?limit=1000");
         Assert.AreEqual(200, status, trail);
         List<Dictionary<string, object>> rows = MiniJson.List(MiniJson.Object(trail), "entries").Cast<Dictionary<string, object>>().ToList();
         Assert.IsFalse(rows.Any(row => (MiniJson.Text(row, "detail") ?? "").Contains(words) || (MiniJson.Text(row, "object") ?? "").Contains(words)),
            "A search of the quarantine was recorded: " + trail);

         // The control: the message itself, read, is recorded - so the trail is on and
         // REST reads are reaching it.
         Assert.AreEqual(200, Http("GET", "/api/v1/quarantine/" + id + "/headers").status);
         (int afterStatus, string after) = Http("GET", "/api/v1/audit?object_type=quarantined+message&action=read&limit=1000");
         Assert.AreEqual(200, afterStatus, after);
         Assert.IsTrue(MiniJson.List(MiniJson.Object(after), "entries").Cast<Dictionary<string, object>>().Any(row => MiniJson.Text(row, "object") == id.ToString()),
            "The headers read was not recorded, so the trail proves nothing about the search: " + after);
      }

      [Test]
      [Description("An account's own search finds only its own held messages, never looks in the recipients, and counts only its own")]
      public void AnAccountsSearchFindsItsOwnAndNeverLooksInTheRecipients()
      {
         Hold("billing@spam-sender.test", "Ada's invoice", Ada);
         Hold("billing@spam-sender.test", "Shared invoice", Ada, Bob);
         Hold("billing@spam-sender.test", "Bob's invoice", Bob);

         string ada = BasicHeader(Ada, UserPassword);
         string bob = BasicHeader(Bob, UserPassword);

         Dictionary<string, object> adas = Page(Q("INVOICE"), ada, "/api/v1/me/quarantine");
         Assert.AreEqual(new[] { "Shared invoice", "Ada's invoice" }, Subjects(adas).ToArray());
         Assert.AreEqual(2, Number(adas, "total"), "total counts the account's own, never anybody else's.");
         Assert.IsTrue(Messages(adas).All(m => !m.ContainsKey("recipients")), "The other recipients are not the account's to learn.");

         // Bob is a recipient of the shared message, but a search of the recipients
         // would tell Ada who else it was for.
         Dictionary<string, object> byRecipient = Page(Q(Bob), ada, "/api/v1/me/quarantine");
         Assert.AreEqual(0, Number(byRecipient, "total"), MiniJson.Text(byRecipient, "body"));

         Dictionary<string, object> bobs = Page(Q("invoice") + "&limit=1", bob, "/api/v1/me/quarantine");
         Assert.AreEqual(2, Number(bobs, "total"));
         Assert.AreEqual(new[] { "Bob's invoice" }, Subjects(bobs).ToArray());

         Dictionary<string, object> bobsSecond = Page(Q("invoice") + "&limit=1&offset=1", bob, "/api/v1/me/quarantine");
         Assert.AreEqual(new[] { "Shared invoice" }, Subjects(bobsSecond).ToArray());

         // Without q: every message held for the account, and only those.
         Dictionary<string, object> every = Page("", ada, "/api/v1/me/quarantine");
         Assert.AreEqual(2, Number(every, "total"));
         Assert.AreEqual(new[] { "Shared invoice", "Ada's invoice" }, Subjects(every).ToArray());
         Assert.IsTrue(MiniJson.Flag(every, "enabled"), MiniJson.Text(every, "body"));
      }

      [Test]
      [Description("The OpenAPI document describes q, offset and limit on both lists")]
      public void TheOpenApiDocumentDescribesTheSearch()
      {
         (int status, string body) = Http("GET", "/api/v1/openapi.json");
         Assert.AreEqual(200, status, body);

         Dictionary<string, object> paths = (Dictionary<string, object>) MiniJson.Object(body)["paths"];
         foreach (string path in new[] { "/api/v1/quarantine", "/api/v1/me/quarantine" })
         {
            var get = (Dictionary<string, object>) ((Dictionary<string, object>) paths[path])["get"];
            List<string> names = MiniJson.List(get, "parameters").Cast<Dictionary<string, object>>().Select(p => MiniJson.Text(p, "name")).ToList();
            Assert.AreEqual(new[] { "q", "offset", "limit" }, names.ToArray(), path);
         }
      }
   }
}
