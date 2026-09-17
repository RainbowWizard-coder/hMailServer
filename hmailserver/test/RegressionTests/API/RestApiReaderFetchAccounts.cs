// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Text;
using System.Threading;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;
using hMailServer;

namespace RegressionTests.API
{
   /// <summary>
   ///    The other accounts a reader adds in the webmail: /api/v1/me/fetch-accounts,
   ///    the administrator's fetch accounts narrowed to what a reader may set. A
   ///    host on a private address is refused until an administrator allows it; a
   ///    field that would switch a filter off or read recipients out of the
   ///    collected mail is refused by name, and the account saved has the filters
   ///    on whatever was sent; an interval under 15 minutes and a fourth account
   ///    are refused; an account another reader owns is not found; an account an
   ///    administrator set up with what a reader cannot set is seen and not
   ///    changed; and a collection started from the page brings the mail in.
   /// </summary>
   [TestFixture]
   public class RestApiReaderFetchAccounts : TestFixtureBase
   {
      private static int RestPort = 9614;
      private const string AdminPassword = "testar";
      private const string ReaderPassword = "Reader-Fetch-Passw0rd!";

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(AdminPassword);
         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();

         Assert.AreEqual(200, Admin("GET", "/api/v1/status", null).status, "REST API did not answer /api/v1/status.");
      }

      [TearDown]
      public void StopRestApi()
      {
         try
         {
            Admin("PUT", "/api/v1/settings", "{\"reader_fetch_accounts_max\":3,\"reader_fetch_accounts_private_addresses\":false}");
         }
         finally
         {
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      [Test]
      [Description("A reader's other account: a private host refused until allowed, an administrator's field refused by name and the filters forced on, the interval floor, the ceiling, another reader's and an administrator's account kept out of reach, and a collection from the page that brings the mail in.")]
      public void AReaderCollectsFromAnotherAccountWithinTheServersLimits()
      {
         Account reader = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "reader-fetch@" + _domain.Name, ReaderPassword);
         Account neighbour = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "neighbour-fetch@" + _domain.Name, ReaderPassword);
         string me = Basic(reader.Address, ReaderPassword);

         var messages = new List<string>
         {
            "From: old-friend@dummy-example.com\r\nTo: reader@old-provider.test\r\nSubject: Collected from the old provider\r\n\r\nHello from before.\r\n"
         };
         int port = TestSetup.GetNextFreePort();
         string body = "{\"name\":\"Old provider\",\"server_address\":\"localhost\",\"port\":" + port +
                       ",\"server_type\":\"pop3\",\"username\":\"old\",\"password\":\"old-password\",\"connection_security\":\"none\",\"days_to_keep_messages\":-1}";

         // A private host, before an administrator allows one.
         (int status, string body) refused = Send("POST", "/api/v1/me/fetch-accounts", me, body);
         Assert.AreEqual(400, refused.status, refused.body);
         StringAssert.Contains("not a public address", refused.body);
         Assert.AreEqual(0, reader.FetchAccounts.Count, "Nothing is saved when the host is refused.");

         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"reader_fetch_accounts_private_addresses\":true}").status);

         // What only an administrator sets.
         (int status, string body) routed = Send("POST", "/api/v1/me/fetch-accounts", me,
            body.Substring(0, body.Length - 1) + ",\"process_mime_recipients\":true}");
         Assert.AreEqual(400, routed.status, routed.body);
         StringAssert.Contains("process_mime_recipients", routed.body);
         (int status, string body) filterOff = Send("POST", "/api/v1/me/fetch-accounts", me,
            body.Substring(0, body.Length - 1) + ",\"use_antispam\":false}");
         Assert.AreEqual(400, filterOff.status, filterOff.body);

         // Too often.
         (int status, string body) tooOften = Send("POST", "/api/v1/me/fetch-accounts", me,
            body.Substring(0, body.Length - 1) + ",\"minutes_between_fetch\":5}");
         Assert.AreEqual(400, tooOften.status, tooOften.body);
         StringAssert.Contains("at least 15", tooOften.body);
         Assert.AreEqual(0, reader.FetchAccounts.Count);

         using (var pop3Server = new Pop3ServerSimulator(1, port, messages))
         {
            pop3Server.StartListen();

            (int status, string body) created = Send("POST", "/api/v1/me/fetch-accounts", me, body);
            Assert.AreEqual(201, created.status, created.body);
            StringAssert.Contains("\"use_antispam\":true", created.body);
            StringAssert.Contains("\"use_antivirus\":true", created.body);
            StringAssert.Contains("\"process_mime_recipients\":false", created.body);
            StringAssert.Contains("\"enable_route_recipients\":false", created.body);
            string id = NumberAfter(created.body, "\"id\":");

            FetchAccount viaCom = reader.FetchAccounts.get_Item(0);
            Assert.IsTrue(viaCom.UseAntiSpam, "The filters are on as COM reads the account.");
            Assert.IsTrue(viaCom.UseAntiVirus);
            Assert.IsFalse(viaCom.ProcessMIMERecipients);

            // Another reader cannot see or collect it.
            string neighbourAuth = Basic(neighbour.Address, ReaderPassword);
            Assert.AreEqual(404, Send("GET", "/api/v1/me/fetch-accounts/" + id, neighbourAuth, null).status);
            Assert.AreEqual(404, Send("POST", "/api/v1/me/fetch-accounts/" + id + "/download", neighbourAuth, null).status);
            StringAssert.DoesNotContain("Old provider", Send("GET", "/api/v1/me/fetch-accounts", neighbourAuth, null).body);

            // Collected now, from the page's button.
            (int status, string body) queued = Send("POST", "/api/v1/me/fetch-accounts/" + id + "/download", me, null);
            Assert.AreEqual(202, queued.status, queued.body);
            pop3Server.WaitForCompletion();
            Pop3ClientSimulator.AssertMessageCount(reader.Address, ReaderPassword, 1);
            StringAssert.Contains("Collected from the old provider", Pop3ClientSimulator.AssertGetFirstMessageText(reader.Address, ReaderPassword));

            DateTime deadline = DateTime.UtcNow.AddSeconds(30);
            while (DateTime.UtcNow < deadline && Send("GET", "/api/v1/me/fetch-accounts/" + id, me, null).body.Contains("\"locked\":true"))
               Thread.Sleep(100);

            // Paused, and back.
            (int status, string body) paused = Send("PUT", "/api/v1/me/fetch-accounts/" + id, me, "{\"enabled\":false}");
            Assert.AreEqual(200, paused.status, paused.body);
            StringAssert.Contains("\"enabled\":false", paused.body);
            StringAssert.Contains("\"use_antispam\":true", paused.body, "A change keeps the filters on.");

            // The ceiling: one more is refused once the reader has as many as allowed.
            Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"reader_fetch_accounts_max\":1}").status);
            (int status, string body) second = Send("POST", "/api/v1/me/fetch-accounts", me, body.Replace("Old provider", "A second one"));
            Assert.AreEqual(403, second.status, second.body);
            StringAssert.Contains("at most 1", second.body);

            // Not offered at all.
            Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"reader_fetch_accounts_max\":0}").status);
            Assert.AreEqual(403, Send("GET", "/api/v1/me/fetch-accounts", me, null).status);
            Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"reader_fetch_accounts_max\":3}").status);

            Assert.AreEqual(200, Send("DELETE", "/api/v1/me/fetch-accounts/" + id, me, null).status);
            Assert.AreEqual(0, reader.FetchAccounts.Count);
         }

         // An administrator's account, set up to read recipients from the message: seen, not changed or removed.
         FetchAccount administrators = reader.FetchAccounts.Add();
         administrators.Name = "Catch-all";
         administrators.ServerAddress = "localhost";
         administrators.Port = 9;
         administrators.MinutesBetweenFetch = 60;
         administrators.ProcessMIMERecipients = true;
         administrators.Enabled = false;
         administrators.Save();

         (int status, string body) listed = Send("GET", "/api/v1/me/fetch-accounts", me, null);
         StringAssert.Contains("Catch-all", listed.body);
         Assert.AreEqual(403, Send("PUT", "/api/v1/me/fetch-accounts/" + administrators.ID, me, "{\"name\":\"Mine now\"}").status);
         Assert.AreEqual(403, Send("DELETE", "/api/v1/me/fetch-accounts/" + administrators.ID, me, null).status);
         Assert.AreEqual("Catch-all", reader.FetchAccounts.get_ItemByDBID(administrators.ID).Name);
      }

      private static string Basic(string user, string password)
      {
         return "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(user + ":" + password));
      }

      private static (int status, string body) Admin(string method, string path, string requestBody)
      {
         return Send(method, path, Basic("Administrator", AdminPassword), requestBody);
      }

      private static (int status, string body) Send(string method, string path, string authorization, string requestBody)
      {
         return RestRequest.Send("127.0.0.1", RestPort, method, path, authorization, requestBody);
      }

      private static string NumberAfter(string body, string marker)
      {
         int at = body.IndexOf(marker, StringComparison.Ordinal);
         Assert.IsTrue(at >= 0, "Missing " + marker + " in: " + body);
         int start = at + marker.Length, end = start;
         while (end < body.Length && char.IsDigit(body[end]))
            end++;
         return body.Substring(start, end - start);
      }
   }
}
