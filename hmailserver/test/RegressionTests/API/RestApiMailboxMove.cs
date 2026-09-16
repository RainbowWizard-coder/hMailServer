// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Net.Sockets;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;
using hMailServer;

namespace RegressionTests.API
{
   /// <summary>
   ///    Renaming a mailbox and moving one to another domain over REST:
   ///    GET and POST /api/v1/accounts/{address}/move.
   ///
   ///    The server-side operation is tested through COM in
   ///    Domains/MailboxMove.cs, where the store, the folder tree and the rows
   ///    keyed on the account are all readable. What this fixture is for is what
   ///    only the route decides: which refusal is which status, that the summary
   ///    is the document the OpenAPI entry describes, that the move is ONE entry
   ///    on the audit trail naming both addresses, and - the one worth the most -
   ///    that a credential confined to one domain cannot move somebody's mailbox
   ///    INTO a domain it has no authority over. The path is scoped by
   ///    Authorize_; the domain in the body is not, and is judged by the handler.
   /// </summary>
   [TestFixture]
   public class RestApiMailboxMove : TestFixtureBase
   {
      // The port the listener answers on: this one on the Windows bench, the
      // suite's own where RestListener finds one already on.
      private static int RestPort = 9138;
      private const string AdminPassword = "testar";
      private const string UserPassword = "test-Pass-1";
      private const string SecondDomain = "restmove.test";

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(AdminPassword);

         RestPort = RestListener.Start(RestPort);

         _application.Reinitialize();

         (int status, string body) probe = Http("GET", "/api/v1/status");
         Assert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);
      }

      [TearDown]
      public void StopRestApi()
      {
         try
         {
            Domain other = _application.Domains.get_ItemByName(SecondDomain);
            if (other != null)
               _application.Domains.DeleteByDBID(other.ID);
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
         }

         RestListener.Stop();
         _application.Reinitialize();
      }

      private Account Seeded(string address)
      {
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, address, UserPassword);

         SmtpClientSimulator.StaticSend("outside@example.com", address, "Seed", "Creates the mailbox.");
         Pop3ClientSimulator.AssertMessageCount(address, UserPassword, 1);

         return account;
      }

      /// <summary>
      ///    The POP3 lock is released when the session's socket closes, after the
      ///    client has read the reply to QUIT, so a fixture that reads a mailbox
      ///    and then moves it has a race to wait out. The GET beside the move is
      ///    exactly what says whether the wait is over.
      /// </summary>
      private static void WaitUntilMovable(string address)
      {
         DateTime until = DateTime.Now.AddSeconds(30);
         string body = "";

         while (DateTime.Now < until)
         {
            (int status, string answer) = Http("GET", MovePath(address));
            body = answer;

            if (status == 200 && answer.Contains("\"in_use\":false"))
               return;

            Thread.Sleep(200);
         }

         Assert.Fail("The mailbox was still in use after 30 seconds: " + body);
      }

      // The address goes in the path as it is written, which is what every
      // other account fixture does and what the server takes: "@" is a legal
      // character in a path segment, and an address is not a query value. A
      // client that percent-encodes it is served too - the listener decodes a
      // path segment before it routes - and one test below sends the encoded
      // form on purpose, because the Control Deck and the PowerShell module do.
      private static string MovePath(string address)
      {
         return "/api/v1/accounts/" + address + "/move";
      }

      [Test]
      [Description("The route moves a mailbox and answers the summary its OpenAPI entry describes; the account is at the new address over COM, its mail is still readable, and the old address is an alias that delivers.")]
      public void TheRouteMovesAMailboxAndTheSummarySaysWhatItCarried()
      {
         Account account = Seeded("rest-from@example.test");
         long accountId = account.ID;

         WaitUntilMovable("rest-from@example.test");

         (int status, string body) moved = Http("POST", MovePath("rest-from@example.test"),
            "{\"new_address\":\"rest-to@example.test\"}");

         Assert.AreEqual(200, moved.status, moved.body);
         StringAssert.Contains("\"old_address\":\"rest-from@example.test\"", moved.body, moved.body);
         StringAssert.Contains("\"new_address\":\"rest-to@example.test\"", moved.body, moved.body);
         StringAssert.Contains("\"alias_kept\":true", moved.body, "keep_alias defaults to true: " + moved.body);
         StringAssert.Contains("\"messages\":1", moved.body, moved.body);

         Account after = _domain.Accounts.get_ItemByAddress("rest-to@example.test");
         Assert.AreEqual(accountId, (long) after.ID, "The same account row, at the new address.");

         Pop3ClientSimulator.AssertMessageCount("rest-to@example.test", UserPassword, 1);

         Assert.AreEqual("rest-to@example.test", _domain.Aliases.get_ItemByName("rest-from@example.test").Value);
      }

      [Test]
      [Description("keep_alias false leaves nothing behind, and the route says so in its answer.")]
      public void KeepAliasFalseLeavesNothingBehind()
      {
         Seeded("rest-drop-from@example.test");
         WaitUntilMovable("rest-drop-from@example.test");

         (int status, string body) moved = Http("POST", MovePath("rest-drop-from@example.test"),
            "{\"new_address\":\"rest-drop-to@example.test\",\"keep_alias\":false}");

         Assert.AreEqual(200, moved.status, moved.body);
         StringAssert.Contains("\"alias_kept\":false", moved.body, moved.body);
         Assert.AreEqual(0, _domain.Aliases.Count, "Nothing is left behind when the switch is off.");
      }

      [Test]
      [Description("The GET says whether a move would be refused now and what the mailbox has been through: never moved before the first move, and a complete journal row naming both addresses after it.")]
      public void TheGetSaysWhetherAMoveWouldBeRefusedAndWhatTheMailboxHasBeenThrough()
      {
         Seeded("rest-status@example.test");
         WaitUntilMovable("rest-status@example.test");

         (int status, string body) before = Http("GET", MovePath("rest-status@example.test"));
         Assert.AreEqual(200, before.status, before.body);
         StringAssert.Contains("\"in_use\":false", before.body, before.body);
         StringAssert.Contains("\"last_move\":null", before.body, "A mailbox that has never moved has no journal row.");

         Assert.AreEqual(200, Http("POST", MovePath("rest-status@example.test"),
            "{\"new_address\":\"rest-status2@example.test\"}").status);

         (int status, string body) after = Http("GET", MovePath("rest-status2@example.test"));
         Assert.AreEqual(200, after.status, after.body);
         StringAssert.Contains("\"old_address\":\"rest-status@example.test\"", after.body, after.body);
         StringAssert.Contains("\"complete\":true", after.body, after.body);

         Assert.AreEqual(404, Http("GET", MovePath("nobody@example.test")).status);

         // The same mailbox asked for with the address percent-encoded, which
         // is what a browser's encodeURIComponent and PowerShell's
         // EscapeDataString produce and therefore what the Control Deck and the
         // module send. Without the listener decoding a path segment,
         // "rest-status2%40example.test" is an address no account has and every
         // one of those calls is a 404 against an account that is there.
         (int status, string body) encoded = Http("GET",
            "/api/v1/accounts/" + Uri.EscapeDataString("rest-status2@example.test") + "/move");
         Assert.AreEqual(200, encoded.status, "A percent-encoded address names the same account: " + encoded.body);
         StringAssert.Contains("\"address\":\"rest-status2@example.test\"", encoded.body, encoded.body);
      }

      [Test]
      [Description("A refusal about the request is 400, one about the state of the server is 409, and an address no account has is 404 - so a client can tell 'try again later' from 'this will never work'. None of them changes anything.")]
      public void RefusalsCarryTheirOwnStatus()
      {
         Seeded("rest-refused@example.test");
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "rest-occupied@example.test", UserPassword);
         WaitUntilMovable("rest-refused@example.test");

         (int status, string body) missing = Http("POST", MovePath("rest-refused@example.test"), "{}");
         Assert.AreEqual(400, missing.status, missing.body);
         StringAssert.Contains("new_address is required", missing.body, missing.body);

         (int status, string body) nowhere = Http("POST", MovePath("rest-refused@example.test"),
            "{\"new_address\":\"rest-refused@nowhere.invalid\"}");
         Assert.AreEqual(400, nowhere.status, nowhere.body);
         StringAssert.Contains("does not hold the domain nowhere.invalid", nowhere.body, nowhere.body);

         (int status, string body) taken = Http("POST", MovePath("rest-refused@example.test"),
            "{\"new_address\":\"rest-occupied@example.test\"}");
         Assert.AreEqual(409, taken.status, taken.body);
         StringAssert.Contains("already exists", taken.body, taken.body);

         Assert.AreEqual(404, Http("POST", MovePath("nobody@example.test"),
            "{\"new_address\":\"somebody@example.test\"}").status);

         // Nothing changed.
         Assert.AreEqual(2, _domain.Accounts.Count);
         Assert.IsNotNull(_domain.Accounts.get_ItemByAddress("rest-refused@example.test"));
         Pop3ClientSimulator.AssertMessageCount("rest-refused@example.test", UserPassword, 1);
      }

      [Test]
      [Description("One entry on the audit trail, naming both addresses: the rows a move writes are one change, so the trail carries 'account moved' once rather than an account update, an alias created and a row per list membership.")]
      public void TheMoveIsOneEntryOnTheAuditTrailNamingBothAddresses()
      {
         Seeded("rest-audited@example.test");
         WaitUntilMovable("rest-audited@example.test");

         Assert.AreEqual(200, Http("POST", MovePath("rest-audited@example.test"),
            "{\"new_address\":\"rest-audited2@example.test\"}").status);

         // The whole trail rather than a filtered slice: the table is not
         // emptied between tests (emptying it needs a direct statement, which
         // the Linux shim cannot make), so every assertion here is about a row
         // this move would have written and no other.
         //
         // In particular it is NOT that the old address appears nowhere. This
         // fixture creates its own account over COM, hm_accounts is an audited
         // table, and so an "account created" entry naming the address the
         // mailbox left is in the trail before the move is asked for. What is
         // being tested is that the MOVE wrote one row and not four.
         (int status, string body) trail = Http("GET", "/api/v1/audit?limit=200");
         Assert.AreEqual(200, trail.status, trail.body);

         Assert.AreEqual(1, Regex.Matches(trail.body, "\"object\":\"rest-audited2@example.test\",\"action\":\"moved\"").Count,
            "One entry for the whole move: " + trail.body);

         // The three rows the move writes that the statement chokepoint would
         // otherwise have recorded beside it, each asked for by name.
         Assert.AreEqual(0, Regex.Matches(trail.body, "\"object\":\"rest-audited2@example.test\",\"action\":\"updated\"").Count,
            "The account row the move wrote is part of the move, not a second change: " + trail.body);

         Assert.AreEqual(0, Regex.Matches(trail.body, "\"object_type\":\"alias\",\"object\":\"rest-audited@example.test\"").Count,
            "The alias the move left behind is part of the move, not a second change: " + trail.body);

         int at = trail.body.IndexOf("\"object\":\"rest-audited2@example.test\",\"action\":\"moved\"", StringComparison.Ordinal);
         int from = trail.body.LastIndexOf('{', at);
         int to = trail.body.IndexOf('}', at);
         string entry = trail.body.Substring(from, to - from + 1);

         StringAssert.Contains("\"object_type\":\"account\"", entry, entry);
         StringAssert.Contains("rest-audited@example.test", entry, "The one entry names the address the mailbox left.");
      }

      [Test]
      [Description("A key confined to one domain may move a mailbox inside it, and is refused a move OUT of it - the address in the path is scoped by the authorisation, the one in the body is judged by the handler, and without that second check a key issued for one domain could move somebody's whole mailbox into a domain it does not administer.")]
      public void ADomainRestrictedKeyIsRefusedTheDomainTheMailboxWouldMoveTo()
      {
         Seeded("rest-scoped@example.test");
         SingletonProvider<TestSetup>.Instance.AddDomain(SecondDomain);
         WaitUntilMovable("rest-scoped@example.test");

         (string id, string key) confined = CreateKey("mailbox-move-confined", "full", _domain.Name);

         // Into a domain it does not administer: refused, and nothing changed.
         (int status, string body) outward = Bearer("POST", MovePath("rest-scoped@example.test"), confined.key,
            "{\"new_address\":\"rest-scoped@" + SecondDomain + "\"}");
         Assert.AreEqual(403, outward.status, outward.body);
         StringAssert.Contains("may not administer accounts in the domain", outward.body, outward.body);
         Assert.IsNotNull(_domain.Accounts.get_ItemByAddress("rest-scoped@example.test"));

         // Inside its own domain: allowed.
         (int status, string body) inward = Bearer("POST", MovePath("rest-scoped@example.test"), confined.key,
            "{\"new_address\":\"rest-scoped2@example.test\"}");
         Assert.AreEqual(200, inward.status, inward.body);
      }

      [Test]
      [Description("The OpenAPI document describes both methods of the path, and says the two things a caller cannot learn anywhere else: which permission it needs in both domains, and that the archive and the message trace are deliberately not rewritten.")]
      public void OpenApiDescribesTheMoveRoute()
      {
         (int status, string body) = Http("GET", "/api/v1/openapi.json");
         Assert.AreEqual(200, status, body);

         StringAssert.Contains("\"/api/v1/accounts/{address}/move\":{", body);

         int at = body.IndexOf("\"/api/v1/accounts/{address}/move\":{", StringComparison.Ordinal);
         string entry = body.Substring(at, Math.Min(4000, body.Length - at));

         StringAssert.Contains("manage_accounts in the address's domain AND in the domain new_address names", entry, entry);
         StringAssert.Contains("archive and the message trace are deliberately NOT rewritten", entry, entry);
      }

      private static (string id, string key) CreateKey(string label, string scope, string domains)
      {
         string body = "{\"label\":\"" + label + "\",\"scope\":\"" + scope + "\"" +
                       (domains == null ? "" : ",\"domains\":\"" + domains + "\"") + "}";
         (int status, string created) = Http("POST", "/api/v1/apikeys", body);
         Assert.AreEqual(201, status, "POST /api/v1/apikeys must create a key. Body: " + created);
         return (Extract(created, "id"), Extract(created, "key"));
      }

      // The value of a top-level JSON property, string or number, enough for
      // the bodies this API returns.
      private static string Extract(string json, string key)
      {
         Match match = Regex.Match(json, "\"" + key + "\"\\s*:\\s*\"?([^\",}]*)\"?");
         Assert.IsTrue(match.Success, "No '" + key + "' in: " + json);
         return match.Groups[1].Value;
      }

      private static (int status, string body) Http(string method, string path, string requestBody = null)
      {
         string credentials = Convert.ToBase64String(Encoding.ASCII.GetBytes("Administrator:" + AdminPassword));
         return Http(method, path, "Basic " + credentials, requestBody);
      }

      private static (int status, string body) Bearer(string method, string path, string token, string requestBody = null)
      {
         return Http(method, path, "Bearer " + token, requestBody);
      }

      // Issues one HTTP/1.0 request against the REST listener and returns the
      // parsed status code and body. The connect is retried briefly to absorb the
      // listener bind race right after a reinitialize.
      private static (int status, string body) Http(string method, string path, string authorization, string requestBody)
      {
         using (var client = new TcpClient())
         {
            Exception last = null;
            for (int attempt = 0; attempt < 25; attempt++)
            {
               try
               {
                  client.Connect("127.0.0.1", RestPort);
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
               var headers = new StringBuilder();
               headers.Append(method + " " + path + " HTTP/1.0\r\n");
               headers.Append("Host: 127.0.0.1\r\n");
               if (authorization != null)
                  headers.Append("Authorization: " + authorization + "\r\n");

               byte[] bodyBytes = requestBody == null ? new byte[0] : Encoding.UTF8.GetBytes(requestBody);
               if (requestBody != null)
               {
                  headers.Append("Content-Type: application/json\r\n");
                  headers.Append("Content-Length: " + bodyBytes.Length + "\r\n");
               }

               headers.Append("Connection: close\r\n\r\n");

               byte[] headerBytes = Encoding.ASCII.GetBytes(headers.ToString());
               stream.Write(headerBytes, 0, headerBytes.Length);
               if (bodyBytes.Length > 0)
                  stream.Write(bodyBytes, 0, bodyBytes.Length);

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
               string body = separator >= 0 ? raw.Substring(separator + 4) : "";

               return (statusCode, body);
            }
         }
      }
   }
}
