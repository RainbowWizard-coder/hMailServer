// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;

namespace RegressionTests.API
{
   /// <summary>
   ///    Spam traps over REST: the collection under its domain, a trap taken back at
   ///    its own address, the record of what has addressed one, and the four settings
   ///    in the server-wide group.
   ///
   ///    Everything here is made over the API and torn down over it, and the one
   ///    message is sent with a raw socket, so nothing depends on COM - which is why
   ///    the Linux suite links this fixture.
   ///
   ///    What this fixture puts on the server's allow and block list, it takes back
   ///    by name: the list is shared with whoever else uses this server, and a test
   ///    that empties it cannot put back what was on it.
   ///
   ///    The ban is switched off for these tests (spam_trap_ban_minutes 0) and tested
   ///    in AntiSpam/SpamTraps instead: the suite talks to this server from 127.0.0.1,
   ///    so a ban earned here would refuse the tests that come after it. What is left
   ///    is still the whole of the rest - the refusal, the record, the blocked sender
   ///    and the routes.
   /// </summary>
   [TestFixture]
   public class RestApiSpamTraps : TestFixtureBase
   {
      private static int RestPort = 9582;
      private const string AdminPassword = "testar";
      private const string DomainPrefix = "resttrap-";

      // The sender these tests make the server block, so the tear-down can take
      // that entry back without touching anybody else's.
      private const string SpammerDomain = "rest-spammer.example";

      private string banMinutesBefore_;
      private string blockDaysBefore_;

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(AdminPassword);

         RestPort = RestListener.Start(RestPort);

         _application.Reinitialize();

         (int status, string body) probe = Http("GET", "/api/v1/status");
         ClassicAssert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);

         Dictionary<string, object> settings = Settings();
         banMinutesBefore_ = Number(settings, "spam_trap_ban_minutes");
         blockDaysBefore_ = Number(settings, "spam_trap_block_days");

         PutSettings("{\"spam_trap_ban_minutes\":0,\"spam_trap_block_days\":1}");

         // The sender's domain authorises the address the suite connects from: a
         // trap hit blocks an envelope sender only when its SPF passes
         // (SpamTrapMonitor::OnHit), since the envelope is otherwise just a claim.
         SuiteDns.Zone.WithTxt(SpammerDomain, "v=spf1 ip4:127.0.0.1 -all");
      }

      [TearDown]
      public void StopRestApi()
      {
         try
         {
            Http("DELETE", "/api/v1/spam-traps/hits");
            RemoveTestDomains();
            RemoveOurBlockedSenders();
            SuiteDns.Reset();

            PutSettings("{\"spam_trap_ban_minutes\":" + banMinutesBefore_ +
                        ",\"spam_trap_block_days\":" + blockDaysBefore_ + "}");
         }
         finally
         {
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      // ------------------------------------------------------------------ the routes

      [Test]
      [Description("A trap is declared under its domain, listed there, and taken back at its own address - the shape an alias has.")]
      public void ATrapIsDeclaredListedAndTakenBack()
      {
         string domain = NewDomain();

         (int status, string body) created = Http("POST", TrapsPath(domain), "{\"address\":\"never-was-real\"}");
         ClassicAssert.AreEqual(201, created.status, created.body);

         Dictionary<string, object> trap = MiniJson.Object(created.body);
         ClassicAssert.AreEqual("never-was-real@" + domain, MiniJson.Text(trap, "address"), created.body);
         ClassicAssert.AreEqual("never-was-real", MiniJson.Text(trap, "local_part"), created.body);
         ClassicAssert.IsTrue(MiniJson.Number(trap, "id") > 0, created.body);
         ClassicAssert.IsTrue(MiniJson.Text(trap, "created").Length > 0, created.body);

         (int status, string body) listed = Http("GET", TrapsPath(domain));
         ClassicAssert.AreEqual(200, listed.status, listed.body);
         ClassicAssert.AreEqual(1, MiniJson.List(MiniJson.Object(listed.body), "spam_traps").Count, listed.body);

         // Asking twice for the same state is that state, not an error: a
         // configuration tool applying a list of traps must not fail on its
         // second run.
         (int status, string body) again = Http("POST", TrapsPath(domain), "{\"address\":\"never-was-real@" + domain + "\"}");
         ClassicAssert.AreEqual(200, again.status, again.body);

         (int status, string body) removed = Http("DELETE", "/api/v1/spam-traps/never-was-real@" + domain);
         ClassicAssert.AreEqual(200, removed.status, removed.body);

         (int status, string body) empty = Http("GET", TrapsPath(domain));
         ClassicAssert.AreEqual(0, MiniJson.List(MiniJson.Object(empty.body), "spam_traps").Count, empty.body);

         (int status, string body) missing = Http("DELETE", "/api/v1/spam-traps/never-was-real@" + domain);
         ClassicAssert.AreEqual(404, missing.status, missing.body);
      }

      [Test]
      [Description("A trap is taken back at a percent-encoded address as well, which is what every client that builds this path sends.")]
      public void ATrapIsTakenBackAtAnEncodedAddress()
      {
         string domain = NewDomain();

         (int status, string body) created = Http("POST", TrapsPath(domain), "{\"address\":\"never-was-real\"}");
         ClassicAssert.AreEqual(201, created.status, created.body);

         // The Deck sends encodeURIComponent(address) and the PowerShell module
         // sends [uri]::EscapeDataString(address); both turn the @ into %40. A
         // route that does not decode its identifier answers 404 for a trap that is
         // there, and neither surface can remove one at all.
         (int status, string body) removed = Http("DELETE", "/api/v1/spam-traps/never-was-real%40" + domain);
         ClassicAssert.AreEqual(200, removed.status, removed.body);

         (int status, string body) empty = Http("GET", TrapsPath(domain));
         ClassicAssert.AreEqual(0, MiniJson.List(MiniJson.Object(empty.body), "spam_traps").Count, empty.body);

         (int status, string body) missing = Http("DELETE", "/api/v1/spam-traps/never-was-real%40" + domain);
         ClassicAssert.AreEqual(404, missing.status, missing.body);
      }

      [Test]
      [Description("An address a mailbox already has cannot be trapped, and an address that is a trap cannot be given to a mailbox: both directions of the same rule.")]
      public void ARealAddressAndATrapAddressRefuseEachOther()
      {
         string domain = NewDomain();

         (int status, string body) account = Http("POST", "/api/v1/domains/" + domain + "/accounts",
            "{\"address\":\"real-person@" + domain + "\",\"password\":\"Rest-Traps-1!\"}");
         ClassicAssert.AreEqual(201, account.status, account.body);

         (int status, string body) refused = Http("POST", TrapsPath(domain), "{\"address\":\"real-person\"}");
         ClassicAssert.AreEqual(409, refused.status, refused.body);
         StringAssert.Contains("already an account", refused.body, refused.body);

         (int status, string body) trap = Http("POST", TrapsPath(domain), "{\"address\":\"never-was-real\"}");
         ClassicAssert.AreEqual(201, trap.status, trap.body);

         (int status, string body) refusedAccount = Http("POST", "/api/v1/domains/" + domain + "/accounts",
            "{\"address\":\"never-was-real@" + domain + "\",\"password\":\"Rest-Traps-1!\"}");
         ClassicAssert.AreEqual(400, refusedAccount.status, refusedAccount.body);
         StringAssert.Contains("spam trap", refusedAccount.body, refusedAccount.body);
      }

      [Test]
      [Description("A trap has to be an address in the domain it is declared on, and the body takes no other field.")]
      public void TheCreateJudgesWhatItIsGiven()
      {
         string domain = NewDomain();

         (int status, string body) elsewhere = Http("POST", TrapsPath(domain), "{\"address\":\"someone@somewhere-else.example\"}");
         ClassicAssert.AreEqual(400, elsewhere.status, elsewhere.body);
         StringAssert.Contains("must be an address in", elsewhere.body, elsewhere.body);

         (int status, string body) unknown = Http("POST", TrapsPath(domain), "{\"address\":\"never\",\"reason\":\"because\"}");
         ClassicAssert.AreEqual(400, unknown.status, unknown.body);
         StringAssert.Contains("unknown field", unknown.body, unknown.body);

         (int status, string body) nothing = Http("POST", TrapsPath(domain), "{}");
         ClassicAssert.AreEqual(400, nothing.status, nothing.body);

         (int status, string body) noDomain = Http("POST", "/api/v1/domains/no-such-domain.example/spam-traps", "{\"address\":\"never\"}");
         ClassicAssert.AreEqual(404, noDomain.status, noDomain.body);
      }

      // ------------------------------------------------------------------- the record

      [Test]
      [Description("A message to a trap is refused as an unknown address, recorded with what was done, and its sender is put on the server's block list.")]
      public void AMessageToATrapIsRecordedAndItsSenderBlocked()
      {
         string domain = NewDomain();

         (int status, string body) trap = Http("POST", TrapsPath(domain), "{\"address\":\"never-was-real\"}");
         ClassicAssert.AreEqual(201, trap.status, trap.body);

         string reply = RcptReply("harvester@" + SpammerDomain, "never-was-real@" + domain);
         StringAssert.Contains("550", reply, reply);
         StringAssert.Contains("Unknown user", reply, reply);

         (int status, string body) hits = Http("GET", "/api/v1/spam-traps/hits");
         ClassicAssert.AreEqual(200, hits.status, hits.body);

         Dictionary<string, object> document = MiniJson.Object(hits.body);
         List<object> items = MiniJson.List(document, "items");

         ClassicAssert.AreEqual(1, items.Count, hits.body);

         var hit = (Dictionary<string, object>) items[0];
         ClassicAssert.AreEqual("never-was-real@" + domain, Text(hit, "address"), hits.body);
         ClassicAssert.AreEqual(domain, Text(hit, "domain"), hits.body);
         ClassicAssert.AreEqual("harvester@" + SpammerDomain, Text(hit, "sender"), hits.body);
         ClassicAssert.IsTrue(Text(hit, "ip_address").Length > 0, hits.body);
         ClassicAssert.IsTrue(Text(hit, "note").Length > 0, "The hit says nothing about what was done: " + hits.body);
         StringAssert.Contains("hits_since_start", hits.body, hits.body);

         // The same hit under its own domain, and nothing about the rest of the
         // server in that answer.
         (int status, string body) mine = Http("GET", TrapsPath(domain) + "/hits");
         ClassicAssert.AreEqual(200, mine.status, mine.body);
         ClassicAssert.AreEqual(1, MiniJson.List(MiniJson.Object(mine.body), "items").Count, mine.body);
         StringAssert.DoesNotContain("hits_since_start", mine.body, mine.body);

         // The sender, on the server's list, with an expiry.
         (int status, string body) senders = Http("GET", "/api/v1/sender-list");
         ClassicAssert.AreEqual(200, senders.status, senders.body);
         StringAssert.Contains("harvester@" + SpammerDomain, senders.body, senders.body);
         StringAssert.Contains("\"blocked\":true", senders.body, senders.body);

         // Clearing the record removes rows and says how many.
         (int status, string body) cleared = Http("DELETE", "/api/v1/spam-traps/hits");
         ClassicAssert.AreEqual(200, cleared.status, cleared.body);
         StringAssert.Contains("\"removed\":1", cleared.body, cleared.body);

         (int status, string body) afterwards = Http("GET", "/api/v1/spam-traps/hits");
         ClassicAssert.AreEqual(0, MiniJson.List(MiniJson.Object(afterwards.body), "items").Count, afterwards.body);

         // And what the hit did is still done: clearing a record undoes nothing.
         (int status, string body) stillBlocked = Http("GET", "/api/v1/sender-list");
         StringAssert.Contains("harvester@" + SpammerDomain, stillBlocked.body, stillBlocked.body);
      }

      [Test]
      [Description("Clearing the record of what has addressed the traps is on the audit trail, with the detail saying that the bans and the blocked senders stay - hm_spamtraphits is traffic, so no statement of the delete is recorded, and this is the one act whose whole effect is to remove evidence.")]
      public void ClearingTheRecordIsOnTheAuditTrail()
      {
         string domain = NewDomain();

         (int status, string body) trap = Http("POST", TrapsPath(domain), "{\"address\":\"never-was-real\"}");
         ClassicAssert.AreEqual(201, trap.status, trap.body);

         RcptReply("harvester@" + SpammerDomain, "never-was-real@" + domain);

         (int status, string body) cleared = Http("DELETE", "/api/v1/spam-traps/hits");
         ClassicAssert.AreEqual(200, cleared.status, cleared.body);

         // The newest row of the trail, not a count of them: the trail is
         // append-only and shared with every fixture that ran before this one, and
         // the rows come back newest first.
         (int status, string body) trail = Http("GET", "/api/v1/audit?action=cleared&limit=1");
         ClassicAssert.AreEqual(200, trail.status, trail.body);

         List<object> entries = MiniJson.List(MiniJson.Object(trail.body), "entries");
         ClassicAssert.AreEqual(1, entries.Count,
            "Destroying the record of what walked into the traps left no trace on the audit trail: " + trail.body);

         var row = (Dictionary<string, object>) entries[0];
         ClassicAssert.AreEqual("spam trap hits", Text(row, "object_type"), trail.body);
         ClassicAssert.AreEqual("every recorded hit", Text(row, "object"), trail.body);
         StringAssert.Contains("the bans and the blocked senders it made stay", Text(row, "detail"), trail.body);
      }

      [Test]
      [Description("The limit on the hit listing is a number between 1 and 1000, and anything else is refused rather than guessed at.")]
      public void TheListingJudgesItsLimit()
      {
         (int status, string body) tooMany = Http("GET", "/api/v1/spam-traps/hits?limit=5000");
         ClassicAssert.AreEqual(400, tooMany.status, tooMany.body);

         (int status, string body) fine = Http("GET", "/api/v1/spam-traps/hits?limit=5");
         ClassicAssert.AreEqual(200, fine.status, fine.body);
      }

      // ----------------------------------------------------------------- the settings

      [Test]
      [Description("The four settings are in the server-wide group, and each refuses a value outside its range in the sentence the Control Panel shows.")]
      public void TheSettingsAreInTheServerGroup()
      {
         Dictionary<string, object> settings = Settings();

         foreach (string key in new[] { "spam_trap_ban_minutes", "spam_trap_block_days",
                                        "spam_trap_hit_retention_days", "spam_trap_teach_spamassassin" })
         {
            ClassicAssert.IsTrue(settings.ContainsKey(key), key + " is not in GET /api/v1/settings.");
         }

         (int status, string body) refused = Http("PUT", "/api/v1/settings", "{\"spam_trap_hit_retention_days\":0}");
         ClassicAssert.AreEqual(400, refused.status, refused.body);
         StringAssert.Contains("SpamTrapHitRetentionDays", refused.body, refused.body);

         (int status, string body) refusedBan = Http("PUT", "/api/v1/settings", "{\"spam_trap_ban_minutes\":-5}");
         ClassicAssert.AreEqual(400, refusedBan.status, refusedBan.body);

         // Nothing was applied by the refused request.
         ClassicAssert.AreEqual("0", Number(Settings(), "spam_trap_ban_minutes"));
      }

      [Test]
      [Description("The OpenAPI document the Deck builds its forms from carries the trap paths, and is still JSON.")]
      public void TheOpenApiDocumentCarriesTheRoutes()
      {
         (int status, string body) document = Http("GET", "/api/v1/openapi.json");
         ClassicAssert.AreEqual(200, document.status, document.body);

         Dictionary<string, object> parsed = MiniJson.Object(document.body);
         ClassicAssert.IsNotNull(parsed, "The OpenAPI document is not a JSON object.");

         foreach (string path in new[] { "/api/v1/domains/{domain}/spam-traps",
                                         "/api/v1/spam-traps/{address}",
                                         "/api/v1/domains/{domain}/spam-traps/hits",
                                         "/api/v1/spam-traps/hits" })
         {
            StringAssert.Contains(path, document.body, path + " is not in the OpenAPI document.");
         }
      }

      // ---------------------------------------------------------------------- helpers

      private static string TrapsPath(string domain)
      {
         return "/api/v1/domains/" + domain + "/spam-traps";
      }

      private static string NewDomain()
      {
         string name = DomainPrefix + Guid.NewGuid().ToString("N").Substring(0, 8) + ".test";
         (int status, string body) = Http("POST", "/api/v1/domains", "{\"name\":\"" + name + "\",\"active\":true}");
         ClassicAssert.AreEqual(201, status, body);
         return name;
      }

      private static void RemoveTestDomains()
      {
         (int status, string body) = Http("GET", "/api/v1/domains");

         if (status != 200)
            return;

         object parsed = MiniJson.Parse(body);
         IEnumerable<object> entries = parsed as List<object> ??
                                       (IEnumerable<object>) MiniJson.List((Dictionary<string, object>) parsed, "domains");

         foreach (object entry in entries)
         {
            var domain = entry as Dictionary<string, object>;
            object name;

            if (domain == null || !domain.TryGetValue("name", out name) || name == null)
               continue;

            if (name.ToString().StartsWith(DomainPrefix, StringComparison.OrdinalIgnoreCase))
               Http("DELETE", "/api/v1/domains/" + name);
         }
      }

      /// <summary>
      ///    The entries this fixture's own trap hits made, and only those: the
      ///    sender it uses, and anything whose reason names a spam trap. The rest of
      ///    the server's list is somebody else's.
      /// </summary>
      private static void RemoveOurBlockedSenders()
      {
         (int status, string body) = Http("GET", "/api/v1/sender-list");

         if (status != 200)
            return;

         foreach (object entry in MiniJson.List(MiniJson.Object(body), "items"))
         {
            var item = entry as Dictionary<string, object>;
            object id;

            if (item == null || !item.TryGetValue("id", out id) || id == null)
               continue;

            if (Text(item, "value").IndexOf(SpammerDomain, StringComparison.OrdinalIgnoreCase) < 0 &&
                Text(item, "reason").IndexOf("the spam trap", StringComparison.OrdinalIgnoreCase) < 0)
               continue;

            Http("DELETE", "/api/v1/sender-list/" + Convert.ToInt64(id));
         }
      }

      private static Dictionary<string, object> Settings()
      {
         (int status, string body) = Http("GET", "/api/v1/settings");
         ClassicAssert.AreEqual(200, status, body);
         return MiniJson.Object(body);
      }

      private static void PutSettings(string body)
      {
         (int status, string answer) = Http("PUT", "/api/v1/settings", body);
         ClassicAssert.AreEqual(200, status, answer);
      }

      private static string Number(Dictionary<string, object> settings, string key)
      {
         object value;

         if (!settings.TryGetValue(key, out value) || value == null)
            return "0";

         return Convert.ToInt64(value).ToString();
      }

      private static string Text(Dictionary<string, object> row, string field)
      {
         object value;
         return row.TryGetValue(field, out value) && value != null ? value.ToString() : string.Empty;
      }

      /// <summary>One RCPT TO over a raw socket, and the reply to it.</summary>
      private static string RcptReply(string sender, string recipient)
      {
         var socket = new TcpConnection();
         ClassicAssert.IsTrue(socket.Connect(25), "Could not connect to SMTP.");

         try
         {
            socket.ReadUntil("220");

            socket.Send("HELO rest-traps\r\n");
            socket.ReadUntil("250");

            socket.Send("MAIL FROM:<" + sender + ">\r\n");
            socket.ReadUntil("250");

            socket.Send("RCPT TO:<" + recipient + ">\r\n");
            string reply = socket.Receive();

            socket.Send("QUIT\r\n");

            return reply;
         }
         finally
         {
            socket.Disconnect();
         }
      }

      private static (int status, string body) Http(string method, string path, string requestBody = null)
      {
         string credentials = Convert.ToBase64String(Encoding.ASCII.GetBytes("Administrator:" + AdminPassword));
         return Http(method, path, "Basic " + credentials, requestBody);
      }

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
               byte[] bodyBytes = requestBody == null ? new byte[0] : Encoding.UTF8.GetBytes(requestBody);

               var head = new StringBuilder();
               head.Append(method + " " + path + " HTTP/1.0\r\n");
               head.Append("Host: 127.0.0.1\r\n");

               if (authorization != null)
                  head.Append("Authorization: " + authorization + "\r\n");

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
