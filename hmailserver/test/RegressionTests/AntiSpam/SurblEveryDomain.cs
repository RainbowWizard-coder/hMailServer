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
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;

namespace RegressionTests.AntiSpam
{
   /// <summary>
   ///    The URI blocklists are asked about every domain a message links to, up
   ///    to 200, and a message that links to more scores SurblTooManyDomainsScore
   ///    (SURBL.h, MaxDomainsAsked). Before 6.3.6 was released the lists were
   ///    asked about 48 names drawn from all of them, so that a listed link
   ///    beside N made-up domains was asked about in 48 of N+1 messages: padding
   ///    set the chance, and cost the sender nothing.
   ///
   ///    What these pin, each beside the case that comes out the other way:
   ///
   ///      * a listed link after a hundred made-up domains hidden in the HTML
   ///        part is asked about - all 101 domains are, which the suite's DNS
   ///        zone counts - and the message is tagged, with no score for too many
   ///        domains;
   ///      * a message whose links name 250 domains scores the setting, once, and
   ///        is asked about no more than 256 names; with the setting at 0, or with
   ///        no URI list active, it scores nothing;
   ///      * the setting reads and writes over REST, 5 until it is changed.
   ///
   ///    The URI list is one of the fixture's own, on a zone the suite's DNS
   ///    serves, made and taken back over REST, so that both suites run it.
   /// </summary>
   [TestFixture]
   public class SurblEveryDomain : TestFixtureBase
   {
      private static int RestPort = 9652;
      private const string AdminPassword = "testar";

      // The zone the fixture's list is asked under, and its listed name.
      private const string Zone = "uri-padding.test";

      private long listId_;
      private Dictionary<string, object> antiSpamBefore_;
      private string token_;

      [SetUp]
      public void ArmTheList()
      {
         _settings.SetAdministratorPassword(AdminPassword);
         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();

         (int status, string body) probe = Http("GET", "/api/v1/status");
         ClassicAssert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);

         (int status, string body) settings = Http("GET", "/api/v1/settings/antispam");
         ClassicAssert.AreEqual(200, settings.status, settings.body);
         antiSpamBefore_ = MiniJson.Object(settings.body);

         // The setting itself is put by the tests that need it, so that the first
         // test runs as it is on a server that predates it: its negative control.
         PutAntiSpam("{\"spam_mark_threshold\":5,\"spam_delete_threshold\":100,\"add_header_spam\":true,\"add_header_reason\":true}");

         TakeBackOurList();

         (int status, string body) created = Http("POST", "/api/v1/surbl-servers",
            "{\"dns_host\":\"" + Zone + "\",\"expected_result\":\"\",\"reject_message\":\"Listed by the padding test list\",\"score\":5,\"active\":true}");
         ClassicAssert.AreEqual(201, created.status, created.body);
         listId_ = MiniJson.Number(MiniJson.Object(created.body), "id");

         // Names nobody asked about before, so that no resolver cache answers
         // for the zone: every lookup this test makes reaches it, and is counted.
         token_ = Guid.NewGuid().ToString("N").Substring(0, 8);
      }

      [TearDown]
      public void TakeItBack()
      {
         try
         {
            TakeBackOurList();

            if (antiSpamBefore_ != null)
            {
               string score = antiSpamBefore_.ContainsKey("surbl_too_many_domains_score")
                  ? ",\"surbl_too_many_domains_score\":" + MiniJson.Number(antiSpamBefore_, "surbl_too_many_domains_score")
                  : "";

               PutAntiSpam("{\"spam_mark_threshold\":" + MiniJson.Number(antiSpamBefore_, "spam_mark_threshold") +
                           ",\"spam_delete_threshold\":" + MiniJson.Number(antiSpamBefore_, "spam_delete_threshold") +
                           ",\"add_header_spam\":" + (MiniJson.Flag(antiSpamBefore_, "add_header_spam") ? "true" : "false") +
                           ",\"add_header_reason\":" + (MiniJson.Flag(antiSpamBefore_, "add_header_reason") ? "true" : "false") +
                           score + "}");
            }
         }
         finally
         {
            SuiteDns.Reset();
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      [Test]
      [Description("A listed link after a hundred made-up domains hidden in the HTML part is asked about, with every one of the 101 domains, and tags the message; the message is not scored for too many domains.")]
      public void AListedLinkAfterAHundredMadeUpDomainsIsAskedAbout()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "surbl-hundred@example.test", "test");

         List<string> padding = Enumerable.Range(1, 100).Select(i => "pad" + i + "-" + token_ + ".test").ToList();
         string listed = "listed-" + token_ + ".test";
         SuiteDns.Zone.WithA(listed + "." + Zone, "127.0.0.2");
         SuiteDns.Zone.ClearQueries();

         new SmtpClientSimulator().SendRaw(account.Address, account.Address, MessageWithLinks(account.Address, padding, listed));
         string text = Unfolded(Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test"));

         HashSet<string> asked = AskedUnderZone();
         List<string> notAsked = padding.Concat(new[] { listed }).Where(d => !asked.Contains(d)).ToList();

         ClassicAssert.AreEqual(0, notAsked.Count,
            "Not every domain the message links to was asked about the list: " + notAsked.Count + " of 101 were not, " +
            string.Join(", ", notAsked.Take(5)) + (notAsked.Count > 5 ? ", ..." : "") + ". Before 6.3.6 was released 48 were.");
         ClassicAssert.IsTrue(text.Contains("X-hMailServer-Spam"),
            "The listed link after a hundred made-up domains did not tag the message.");
         ClassicAssert.IsTrue(text.Contains("Listed by the padding test list"), "The list's reason is not in the headers: " + Headers(text));
         ClassicAssert.IsFalse(text.Contains("the URI blocklists are asked about"),
            "A message of 101 domains was scored for too many domains: " + Headers(text));
      }

      [Test]
      [Description("A message whose links name 250 domains is asked about no more than 256 names and scores SurblTooManyDomainsScore once; at 0 it scores nothing, and with no URI list active nothing is scored.")]
      public void AMessageThatLinksToMoreDomainsThanAreAskedScoresForIt()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "surbl-many@example.test", "test");

         List<string> padding = Enumerable.Range(1, 250).Select(i => "many" + i + "-" + token_ + ".test").ToList();
         string message = MessageWithLinks(account.Address, padding, null);

         PutAntiSpam("{\"surbl_too_many_domains_score\":5}");

         // Scored, once, with the reason the header gives.
         SuiteDns.Zone.ClearQueries();
         new SmtpClientSimulator().SendRaw(account.Address, account.Address, message);
         string scored = Unfolded(Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test"));

         ClassicAssert.IsTrue(scored.Contains("The links name 250 domains, more than the 200 the URI blocklists are asked about - (Score: 5)"),
            "A message of 250 domains was not scored for them: " + Headers(scored));
         ClassicAssert.IsTrue(scored.Contains("X-hMailServer-Spam"), "A message scored 5 at a mark threshold of 5 was not tagged.");

         HashSet<string> asked = AskedUnderZone();
         ClassicAssert.AreEqual(200, asked.Count(d => padding.Contains(d)),
            "Not 200 of the message's 250 domains were asked about: " + asked.Count);
         ClassicAssert.LessOrEqual(asked.Count, 256, "More than 256 names were asked of one list for one message.");

         // At 0, nothing.
         PutAntiSpam("{\"surbl_too_many_domains_score\":0}");
         new SmtpClientSimulator().SendRaw(account.Address, account.Address, message);
         string zero = Unfolded(Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test"));
         ClassicAssert.IsFalse(zero.Contains("the URI blocklists are asked about"), "The score at 0 still scored: " + Headers(zero));
         ClassicAssert.IsFalse(zero.Contains("X-hMailServer-Spam"), "The score at 0 still tagged the message.");

         // With no URI list active, the test does not run and nothing is scored.
         PutAntiSpam("{\"surbl_too_many_domains_score\":5}");
         (int status, string body) off = Http("PUT", "/api/v1/surbl-servers/" + listId_, "{\"active\":false}");
         ClassicAssert.AreEqual(200, off.status, off.body);
         new SmtpClientSimulator().SendRaw(account.Address, account.Address, message);
         string inactive = Unfolded(Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test"));
         ClassicAssert.IsFalse(inactive.Contains("the URI blocklists are asked about"),
            "A message was scored for too many domains with no URI list active: " + Headers(inactive));
      }

      [Test]
      [Description("SurblTooManyDomainsScore reads and writes over REST, as surbl_too_many_domains_score.")]
      public void TheScoreReadsAndWritesOverRest()
      {
         PutAntiSpam("{\"surbl_too_many_domains_score\":7}");
         (int status, string body) read = Http("GET", "/api/v1/settings/antispam");
         ClassicAssert.AreEqual(200, read.status, read.body);
         ClassicAssert.AreEqual(7, MiniJson.Number(MiniJson.Object(read.body), "surbl_too_many_domains_score"), read.body);

         (int status, string body) document = Http("GET", "/api/v1/openapi.json");
         ClassicAssert.AreEqual(200, document.status, document.body);
         StringAssert.Contains("surbl_too_many_domains_score", document.body, "The setting is not in the OpenAPI document the Deck builds its form from.");
      }

      // ------------------------------------------------------------------ helpers

      /// <summary>
      ///    A message from and to the address with a text part that links to
      ///    nothing and an HTML part whose anchors to the padding domains are
      ///    hidden - display:none, as a sender hiding them would - followed by a
      ///    visible link to listed, when there is one.
      /// </summary>
      private static string MessageWithLinks(string address, List<string> padding, string listed)
      {
         var html = new StringBuilder();
         html.Append("<html><body><p>Hello.</p><div style=\"display:none\">");
         foreach (string domain in padding)
            html.Append("<a href=\"http://" + domain + "/\"></a>\r\n");
         html.Append("</div>");
         if (listed != null)
            html.Append("<p><a href=\"https://www." + listed + "/offer?id=1\">See the offer</a></p>");
         html.Append("</body></html>\r\n");

         const string boundary = "surbl-padding-boundary";
         return "From: " + address + "\r\n" +
                "To: " + address + "\r\n" +
                "Subject: SURBL padding\r\n" +
                "MIME-Version: 1.0\r\n" +
                "Content-Type: multipart/alternative; boundary=\"" + boundary + "\"\r\n" +
                "\r\n" +
                "--" + boundary + "\r\n" +
                "Content-Type: text/plain; charset=us-ascii\r\n" +
                "\r\n" +
                "Hello.\r\n" +
                "--" + boundary + "\r\n" +
                "Content-Type: text/html; charset=us-ascii\r\n" +
                "\r\n" +
                html +
                "--" + boundary + "--\r\n";
      }

      /// <summary>The names asked under the fixture's zone since the queries were last cleared, less the zone.</summary>
      private static HashSet<string> AskedUnderZone()
      {
         string suffix = "." + Zone;
         var asked = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

         foreach (string query in SuiteDns.Zone.Queries)
         {
            int slash = query.IndexOf('/');
            string name = slash >= 0 ? query.Substring(slash + 1) : query;
            if (name.EndsWith(suffix, StringComparison.OrdinalIgnoreCase))
               asked.Add(name.Substring(0, name.Length - suffix.Length));
         }

         return asked;
      }

      /// <summary>
      ///    The message with its header lines unfolded (RFC 5322 2.2.3): the server
      ///    folds a long X-hMailServer-Reason value at a space, and a phrase looked
      ///    for across the fold - or looked for to be absent - must not depend on
      ///    where the fold fell.
      /// </summary>
      private static string Unfolded(string message)
      {
         return message.Replace("\r\n ", " ").Replace("\r\n\t", "\t");
      }

      private static string Headers(string message)
      {
         int end = message.IndexOf("\r\n\r\n", StringComparison.Ordinal);
         return end >= 0 ? message.Substring(0, end) : message;
      }

      private void TakeBackOurList()
      {
         (int status, string body) lists = Http("GET", "/api/v1/surbl-servers");
         ClassicAssert.AreEqual(200, lists.status, lists.body);

         object parsed = MiniJson.Parse(lists.body);
         var items = parsed as List<object> ?? MiniJson.List((Dictionary<string, object>) parsed, "items");

         foreach (Dictionary<string, object> item in items.Cast<Dictionary<string, object>>())
         {
            if (string.Equals(MiniJson.Text(item, "dns_host"), Zone, StringComparison.OrdinalIgnoreCase))
               Http("DELETE", "/api/v1/surbl-servers/" + MiniJson.Number(item, "id"));
         }
      }

      private static void PutAntiSpam(string body)
      {
         (int status, string answer) = Http("PUT", "/api/v1/settings/antispam", body);
         ClassicAssert.AreEqual(200, status, answer);
      }

      private static (int status, string body) Http(string method, string path, string requestBody = null)
      {
         string credentials = Convert.ToBase64String(Encoding.ASCII.GetBytes("Administrator:" + AdminPassword));

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
               byte[] bodyBytes = requestBody == null ? new byte[0] : Encoding.UTF8.GetBytes(requestBody);

               var head = new StringBuilder();
               head.Append(method + " " + path + " HTTP/1.0\r\n");
               head.Append("Host: 127.0.0.1\r\n");
               head.Append("Authorization: Basic " + credentials + "\r\n");

               if (requestBody != null)
               {
                  head.Append("Content-Type: application/json\r\n");
                  head.Append("Content-Length: " + bodyBytes.Length + "\r\n");
               }

               head.Append("Connection: close\r\n\r\n");

               byte[] headBytes = Encoding.ASCII.GetBytes(head.ToString());
               stream.Write(headBytes, 0, headBytes.Length);

               if (bodyBytes.Length > 0)
                  stream.Write(bodyBytes, 0, bodyBytes.Length);

               byte[] buffer = new byte[8192];
               int read;

               while ((read = stream.Read(buffer, 0, buffer.Length)) > 0)
                  memory.Write(buffer, 0, read);

               string raw = Encoding.UTF8.GetString(memory.ToArray());
               string[] first = raw.Split(new[] { "\r\n" }, StringSplitOptions.None)[0].Split(' ');
               int status = 0;

               if (first.Length >= 2)
                  int.TryParse(first[1], out status);

               int separator = raw.IndexOf("\r\n\r\n", StringComparison.Ordinal);
               return (status, separator >= 0 ? raw.Substring(separator + 4) : "");
            }
         }
      }
   }
}
