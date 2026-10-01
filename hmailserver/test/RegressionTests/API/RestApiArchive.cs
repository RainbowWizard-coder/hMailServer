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
using RegressionTests.Infrastructure;
using RegressionTests.Shared;
using hMailServer;

namespace RegressionTests.API
{
   /// <summary>
   ///    The archive index over REST: search, one entry, hold and release, with
   ///    the domain scope decided from the query and from the row rather than from
   ///    the route - a domain-restricted key searches one of its own domains and
   ///    touches only rows in them.
   /// </summary>
   [TestFixture]
   public class RestApiArchive : TestFixtureBase
   {
      // The port the listener answers on: this one on the Windows bench, the
      // suite's own where RestListener finds one already on.
      private static int RestPort = 9098;
      private const string AdminPassword = "testar";
      private string archiveRoot_;

      private void WriteSetting(string key, string value)
      {
         IniFileSetting.Write(key, value);
      }

      private static (int status, string body) Http(string method, string path, string authorization = null, string requestBody = null)
      {
         if (authorization == null)
            authorization = "Basic " + Convert.ToBase64String(Encoding.ASCII.GetBytes("Administrator:" + AdminPassword));

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
               return (statusCode, separator >= 0 ? raw.Substring(separator + 4) : "");
            }
         }
      }

      private static string Extract(string json, string key)
      {
         string needle = "\"" + key + "\":\"";
         int at = json.IndexOf(needle, StringComparison.Ordinal);
         Assert.IsTrue(at >= 0, "No '" + key + "' in: " + json);
         int start = at + needle.Length;
         return json.Substring(start, json.IndexOf('"', start) - start);
      }

      // Rows outlive the fixtures that wrote them - the archive outlives its accounts
      // by design - so every count here is scoped to a subject no other test or run
      // has used.
      private static string Unique(string prefix)
      {
         return prefix + " " + Guid.NewGuid().ToString("N").Substring(0, 8);
      }

      private static int Count(string json, string needle)
      {
         int count = 0;
         for (int at = json.IndexOf(needle, StringComparison.Ordinal); at >= 0; at = json.IndexOf(needle, at + 1, StringComparison.Ordinal))
            count++;
         return count;
      }

      private static long FirstId(string json)
      {
         const string needle = "\"id\":";
         int at = json.IndexOf(needle, StringComparison.Ordinal);
         Assert.IsTrue(at >= 0, "No entry in: " + json);
         int start = at + needle.Length;
         return long.Parse(json.Substring(start, json.IndexOf(',', start) - start));
      }

      [SetUp]
      public void StartRestApiAndTheArchive()
      {
         archiveRoot_ = Paths.Combine(Path.GetTempPath(), "hmail-rest-archive-" + Guid.NewGuid().ToString("N"));
         Directory.CreateDirectory(archiveRoot_);

         _settings.SetAdministratorPassword(AdminPassword);
         RestPort = RestListener.Start(RestPort);
         WriteSetting("ArchiveDir", archiveRoot_);
         _application.Reinitialize();

         (int status, string body) probe = Http("GET", "/api/v1/status");
         Assert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);
      }

      [TearDown]
      public void StopRestApiAndRemoveTheArchive()
      {
         RestListener.Stop();
         WriteSetting("ArchiveDir", "");
         _application.Reinitialize();
         try
         {
            Directory.Delete(archiveRoot_, true);
         }
         catch (IOException)
         {
            // Deliberately ignored: best effort only, and the outcome of the surrounding operation does not depend on this succeeding.
         }
      }

      private void ArchiveOneMessage(string subject)
      {
         ArchiveMessages(subject);
      }

      // The two accounts once, however many messages: an account added twice is
      // refused as a duplicate.
      private void ArchiveMessages(params string[] subjects)
      {
         Account alice = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "alice@example.test", "test");
         Account bob = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "bob@example.test", "test");
         foreach (string subject in subjects)
            SmtpClientSimulator.StaticSend(alice.Address, bob.Address, subject, "Body text.");
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
      }

      [Test]
      [Description("The archive is searched over REST by domain, subject and mailbox; one entry is read by id; an unknown id is 404; the OpenAPI document names the routes.")]
      public void TheArchiveIsSearchedOverRest()
      {
         string needle = Unique("REST archive needle");
         ArchiveOneMessage(needle);

         (int status, string body) = Http("GET", "/api/v1/archive?domain=example.test&subject=" + Uri.EscapeDataString(needle));
         Assert.AreEqual(200, status, body);
         Assert.AreEqual(2, Count(body, "\"id\":"), body);
         StringAssert.Contains("\"subject\":\"" + needle + "\"", body);

         (int subjectStatus, string bySubject) = Http("GET", "/api/v1/archive?subject=" + Uri.EscapeDataString(needle.Substring(5)) + "&mailbox=bob");
         Assert.AreEqual(200, subjectStatus, bySubject);
         Assert.AreEqual(1, Count(bySubject, "\"id\":"), "the %20 in the query is decoded and mailbox narrows to bob: " + bySubject);
         StringAssert.Contains("\"direction\":\"received\"", bySubject);

         long id = FirstId(bySubject);
         (int oneStatus, string one) = Http("GET", "/api/v1/archive/" + id);
         Assert.AreEqual(200, oneStatus, one);
         StringAssert.Contains("\"id\":" + id + ",", one);
         StringAssert.Contains("\"mailbox\":\"bob\"", one);

         Assert.AreEqual(404, Http("GET", "/api/v1/archive/987654321").status);

         (int docStatus, string doc) = Http("GET", "/api/v1/openapi.json");
         Assert.AreEqual(200, docStatus);
         foreach (string path in new[] { "/api/v1/archive", "/api/v1/archive/{id}", "/api/v1/archive/{id}/hold" })
            StringAssert.Contains("\"" + path + "\"", doc, "The OpenAPI document must describe " + path);
      }

      [Test]
      [Description("A search of the archive index and one entry read are each on the audit trail - the search with what was asked for and how many entries it answered, the entry by who wrote to whom and never its subject - and the same search again inside the minute is the row already written. Until 6.3.6 neither was: the index is the senders, recipients and subjects of everybody's archived mail.")]
      public void ASearchAndAnEntryReadAreOnTheAuditTrail()
      {
         string needle = Unique("Audited search");
         ArchiveOneMessage(needle);

         string search = "/api/v1/archive?domain=example.test&mailbox=bob&subject=" + Uri.EscapeDataString(needle);
         (int status, string body) = Http("GET", search);
         Assert.AreEqual(200, status, body);
         Assert.AreEqual(1, Count(body, "\"id\":"), body);
         long id = FirstId(body);

         // The Deck's archive page searches again after every hold it places: the
         // same words from the same credential inside the minute are one row.
         Assert.AreEqual(200, Http("GET", search).status);

         Assert.AreEqual(200, Http("GET", "/api/v1/archive/" + id).status);
         Assert.AreEqual(404, Http("GET", "/api/v1/archive/987654321").status, "An entry that is not there was not read.");

         (int searchesStatus, string searches) = Http("GET", "/api/v1/audit?object_type=archive&action=searched&limit=1000");
         Assert.AreEqual(200, searchesStatus, searches);
         List<Dictionary<string, object>> searchRows = MiniJson.List(MiniJson.Object(searches), "entries")
            .Cast<Dictionary<string, object>>()
            .Where(row => MiniJson.Text(row, "detail").Contains(needle))
            .ToList();

         Assert.AreEqual(1, searchRows.Count, "The search twice inside the minute is one row: " + searches);
         Assert.AreEqual("bob@example.test", MiniJson.Text(searchRows[0], "object"), "The row names whose archive was searched.");
         // The words asked for and the count answered, then the role the
         // credential read under, as every row of the trail ends.
         Assert.AreEqual("searched for domain example.test, mailbox bob, subject containing " + needle + "; 1 entry answered [role: serveradmin]",
            MiniJson.Text(searchRows[0], "detail"));
         Assert.AreEqual("administrator", MiniJson.Text(searchRows[0], "actor"));
         Assert.AreEqual("REST", MiniJson.Text(searchRows[0], "interface"));
         Assert.AreEqual("127.0.0.1", MiniJson.Text(searchRows[0], "address"));

         (int entriesStatus, string entries) = Http("GET", "/api/v1/audit?object_type=archive+entry&action=read&limit=1000");
         Assert.AreEqual(200, entriesStatus, entries);
         List<Dictionary<string, object>> entryRows = MiniJson.List(MiniJson.Object(entries), "entries")
            .Cast<Dictionary<string, object>>()
            .Where(row => MiniJson.Text(row, "object") == "archiveid " + id)
            .ToList();

         Assert.AreEqual(1, entryRows.Count, entries);
         Assert.AreEqual("its index entry read: the copy kept for bob@example.test, from alice@example.test to bob@example.test [role: serveradmin]",
            MiniJson.Text(entryRows[0], "detail"));
         StringAssert.DoesNotContain("\"object\":\"archiveid 987654321\"", entries, "A refused read is not recorded.");
         StringAssert.DoesNotContain(needle, entries, "The entry is named by who wrote to whom; its subject would make the trail a copy of the mail.");
      }

      [Test]
      [Description("A hold is placed and lifted over REST, read back in the entry, and refused for an unknown id and for a read-only key.")]
      public void HoldsArePlacedAndLiftedOverRest()
      {
         string needle = Unique("Hold me");
         ArchiveOneMessage(needle);
         (int status, string body) = Http("GET", "/api/v1/archive?domain=example.test&mailbox=bob&subject=" + Uri.EscapeDataString(needle));
         Assert.AreEqual(200, status, body);
         long id = FirstId(body);

         Assert.AreEqual(200, Http("POST", "/api/v1/archive/" + id + "/hold").status);
         StringAssert.Contains("\"hold\":true", Http("GET", "/api/v1/archive/" + id).body);
         StringAssert.Contains("\"id\":" + id + ",", Http("GET", "/api/v1/archive?domain=example.test&hold=1").body, "hold=1 lists the held copies");

         Assert.AreEqual(200, Http("DELETE", "/api/v1/archive/" + id + "/hold").status);
         StringAssert.Contains("\"hold\":false", Http("GET", "/api/v1/archive/" + id).body);
         Assert.AreEqual(404, Http("POST", "/api/v1/archive/987654321/hold").status);

         (int created, string key) = Http("POST", "/api/v1/apikeys", null, "{\"label\":\"rest archive - readonly\",\"scope\":\"readonly\"}");
         Assert.AreEqual(201, created, key);
         string token = Extract(key, "key");
         Assert.AreEqual(403, Http("POST", "/api/v1/archive/" + id + "/hold", "Bearer " + token).status, "a read-only key cannot place a hold");
         Assert.AreEqual(200, Http("GET", "/api/v1/archive/" + id, "Bearer " + token).status, "but it can read");
      }

      [Test]
      [Description("An erasure with the archive included sweeps the person's archive tree file by file: a copy under legal hold keeps its file and its row, and the rest go; the answer counts the held copy once, for its file and its row together.")]
      public void AHeldCopySurvivesAnErasure()
      {
         // The index rows earlier runs left naming bob outlive their files, and a held
         // one among them would be counted here: their holds are released first, so
         // that this run's erasure removes them rather than counting them.
         (int status, string body) stale = Http("GET", "/api/v1/archive?domain=example.test&hold=1&recipient=" + Uri.EscapeDataString("bob@example.test"));
         ClassicAssert.AreEqual(200, stale.status, stale.body);
         foreach (System.Text.RegularExpressions.Match row in System.Text.RegularExpressions.Regex.Matches(stale.body, "\"id\":([0-9]+),"))
            Http("DELETE", "/api/v1/archive/" + row.Groups[1].Value + "/hold");

         string held = Unique("Hold me");
         string swept = Unique("Sweep me");
         ArchiveMessages(held, swept);

         (int status, string body) = Http("GET", "/api/v1/archive?domain=example.test&mailbox=bob&subject=" + Uri.EscapeDataString(held));
         Assert.AreEqual(200, status, body);
         long heldId = FirstId(body);
         Assert.AreEqual(200, Http("POST", "/api/v1/archive/" + heldId + "/hold").status);

         string bobsTree = Paths.Combine(archiveRoot_, "example.test", "bob");
         Assert.IsTrue(Directory.Exists(bobsTree), "Bob's archive tree is where the archiver writes it: " + bobsTree);
         Assert.AreEqual(2, Directory.GetFiles(bobsTree, "*", SearchOption.AllDirectories).Length, "Two received copies before the erasure.");

         (int status, string body) erased = Http("POST", "/api/v1/addresses/erase", null, "{\"address\":\"bob@example.test\",\"include_archive\":true}");
         ClassicAssert.AreEqual(200, erased.status, erased.body);
         Dictionary<string, object> answer = MiniJson.Object(erased.body);
         ClassicAssert.Greater(MiniJson.Number(answer, "removed"), 0);
         ClassicAssert.AreEqual(1, MiniJson.Number(answer, "held"), "One copy is held, and its file and its row are one copy: " + erased.body);

         Assert.AreEqual(1, Directory.GetFiles(bobsTree, "*", SearchOption.AllDirectories).Length, "The held copy's file stays; the other went.");
         StringAssert.Contains("\"hold\":true", Http("GET", "/api/v1/archive/" + heldId).body, "The held row stays too.");
         StringAssert.DoesNotContain(swept, Http("GET", "/api/v1/archive?domain=example.test&mailbox=bob").body, "The swept copy's row went with its file.");

         // Released, so that the row this run leaves is not held in the next.
         ClassicAssert.AreEqual(200, Http("DELETE", "/api/v1/archive/" + heldId + "/hold").status);
      }

      [Test]
      [Description("A domain-restricted key must name one of its domains to search, sees only rows in them, and cannot touch a row of another domain or an Inbound copy.")]
      public void ADomainRestrictedKeyStaysInItsDomains()
      {
         string needle = Unique("Scoped");
         ArchiveOneMessage(needle);

         (int created, string key) = Http("POST", "/api/v1/apikeys", null, "{\"label\":\"rest archive - domain\",\"scope\":\"full\",\"domains\":\"example.test\"}");
         Assert.AreEqual(201, created, key);
         string bearer = "Bearer " + Extract(key, "key");

         Assert.AreEqual(403, Http("GET", "/api/v1/archive", bearer).status, "no domain named");
         Assert.AreEqual(403, Http("GET", "/api/v1/archive?domain=other.test", bearer).status, "another domain");

         (int status, string body) = Http("GET", "/api/v1/archive?domain=example.test&subject=" + Uri.EscapeDataString(needle), bearer);
         Assert.AreEqual(200, status, body);
         Assert.AreEqual(2, Count(body, "\"id\":"), body);

         long id = FirstId(body);
         Assert.AreEqual(200, Http("POST", "/api/v1/archive/" + id + "/hold", bearer).status, "its own domain's row can be held");
         Assert.AreEqual(200, Http("DELETE", "/api/v1/archive/" + id + "/hold", bearer).status);

         // An Inbound copy (from an outside sender) belongs to no domain: not the key's.
         string outside = Unique("From outside");
         SmtpClientSimulator.StaticSend("outsider@elsewhere.test", "bob@example.test", outside, "Body text.");
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         (int allStatus, string all) = Http("GET", "/api/v1/archive?subject=" + Uri.EscapeDataString(outside));
         Assert.AreEqual(200, allStatus, all);
         StringAssert.Contains("\"direction\":\"inbound\"", all);
         long inboundId = 0;
         foreach (string entry in all.Split(new[] { "{\"id\":" }, StringSplitOptions.RemoveEmptyEntries)
                                     .Where(entry => entry.Contains("\"direction\":\"inbound\"")))
            inboundId = long.Parse(entry.Substring(0, entry.IndexOf(',')));
         Assert.IsTrue(inboundId > 0, "the Inbound copy has a row: " + all);
         Assert.AreEqual(403, Http("GET", "/api/v1/archive/" + inboundId, bearer).status, "the Inbound copy is nobody's domain");
         Assert.AreEqual(200, Http("GET", "/api/v1/archive/" + inboundId).status, "the administrator sees it");
      }
   }
}
