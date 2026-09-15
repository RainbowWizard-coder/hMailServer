// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Runtime.InteropServices;
using System.Text.RegularExpressions;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.API
{
   /// <summary>
   ///    The suppression list's administration surface: the settings group, the
   ///    list read, added to and removed from by hand, the counts, classify and the
   ///    sweep over REST, and the same documents over COM, which is what the
   ///    Control Panel reads.
   ///
   ///    What is held here beside the round trips is who may do what. A list is a
   ///    domain's, so a key issued for one domain reads and changes that domain's
   ///    list and gets 403 on another's, on the settings that apply to every
   ///    domain and on the sweep that removes entries from all of them; a read-only
   ///    key reads and changes nothing. And an address this server hosts is
   ///    refused, because the list is never checked for one and an entry for it
   ///    would say something untrue.
   /// </summary>
   [TestFixture]
   public class RestApiBounces : TestFixtureBase
   {
      private static int RestPort = 9553;

      private const string SettingsPath = "/api/v1/settings/bounces";
      private const string Remote = "gone@" + BounceTestSupport.RemoteDomain;

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
      [Description("GET and PUT /api/v1/settings/bounces are the eight Settings properties: written over REST and read back over COM, and a value out of range - or a word that is not an action - is refused naming the key, with nothing in the body applied.")]
      public void TheSettingsGroupRoundTripsAndRefusesOutOfRange()
      {
         (int status, string body) written = Http("PUT", SettingsPath,
            "{\"processing_enabled\":true,\"hard_bounce_threshold\":3,\"mailbox_full_is_hard\":true,\"suppression_action\":\"drop\"," +
            "\"suppression_expiry_days\":30,\"read_delivery_reports\":false,\"count_retention_days\":45,\"report_window_days\":14}");
         Assert.AreEqual(200, written.status, written.body);

         Assert.IsTrue(BounceTestSupport.ComBounceProcessingEnabled(_settings));
         Assert.AreEqual(3, BounceTestSupport.ComBounceHardBounceThreshold(_settings));
         Assert.IsTrue(BounceTestSupport.ComBounceMailboxFullIsHard(_settings));
         Assert.AreEqual(1, BounceTestSupport.ComBounceSuppressionAction(_settings), "drop is 1");
         Assert.AreEqual(30, BounceTestSupport.ComBounceSuppressionExpiryDays(_settings));
         Assert.IsFalse(BounceTestSupport.ComBounceReadDeliveryReports(_settings));
         Assert.AreEqual(45, BounceTestSupport.ComBounceCountRetentionDays(_settings));
         Assert.AreEqual(14, BounceTestSupport.ComBounceReportWindowDays(_settings));

         string read = Read(SettingsPath);
         Assert.IsTrue(BounceTestSupport.Flag(read, "processing_enabled"), read);
         Assert.AreEqual(3, BounceTestSupport.Number(read, "hard_bounce_threshold"), read);
         Assert.AreEqual("drop", BounceTestSupport.Text(read, "suppression_action"), read);
         Assert.AreEqual(30, BounceTestSupport.Number(read, "suppression_expiry_days"), read);
         Assert.IsFalse(BounceTestSupport.Flag(read, "read_delivery_reports"), read);
         Assert.AreEqual(45, BounceTestSupport.Number(read, "count_retention_days"), read);
         Assert.AreEqual(14, BounceTestSupport.Number(read, "report_window_days"), read);

         // All or nothing: the acceptable key beside the refused one is not applied.
         (int status, string body) threshold = Http("PUT", SettingsPath, "{\"count_retention_days\":10,\"hard_bounce_threshold\":0}");
         Assert.AreEqual(400, threshold.status, threshold.body);
         StringAssert.Contains("hard_bounce_threshold", threshold.body);
         Assert.AreEqual(45, BounceTestSupport.ComBounceCountRetentionDays(_settings), "A refused body changed a setting.");
         Assert.AreEqual(3, BounceTestSupport.ComBounceHardBounceThreshold(_settings));

         (int status, string body) window = Http("PUT", SettingsPath, "{\"report_window_days\":31}");
         Assert.AreEqual(400, window.status, window.body);
         StringAssert.Contains("report_window_days", window.body);

         (int status, string body) expiry = Http("PUT", SettingsPath, "{\"suppression_expiry_days\":3651}");
         Assert.AreEqual(400, expiry.status, expiry.body);
         StringAssert.Contains("suppression_expiry_days", expiry.body);

         (int status, string body) action = Http("PUT", SettingsPath, "{\"suppression_action\":\"sometimes\"}");
         Assert.AreEqual(400, action.status, action.body);
         StringAssert.Contains("suppression_action", action.body);
         Assert.AreEqual(1, BounceTestSupport.ComBounceSuppressionAction(_settings));

         // COM refuses the same ranges rather than clamping them.
         Assert.Throws<COMException>(() => BounceTestSupport.ComSetBounceHardBounceThreshold(_settings, 0));
         Assert.Throws<COMException>(() => BounceTestSupport.ComSetBounceSuppressionAction(_settings, 3));
         Assert.Throws<COMException>(() => BounceTestSupport.ComSetBounceReportWindowDays(_settings, 0));
         Assert.AreEqual(3, BounceTestSupport.ComBounceHardBounceThreshold(_settings));
      }

      [Test]
      [Description("An address is put on a domain's list by hand and taken off again: 201 with the entry, 409 when listed already, 400 for an invalid address, a hosted one or an expiry out of range, 404 for an unknown domain, a search that finds it, and DELETE by address - encoded or not - then 404.")]
      public void ManualAddListAndRemove()
      {
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "hosted@" + _domain.Name, "test");

         (int status, string body) created = Http("POST", DomainPath + "/suppressions",
            "{\"address\":\"Gone@" + BounceTestSupport.RemoteDomain + "\",\"note\":\"Left the company in August.\",\"expires_days\":30}");
         Assert.AreEqual(201, created.status, created.body);
         Assert.AreEqual(Remote, BounceTestSupport.Text(created.body, "address"), "Addresses are stored lower-cased: " + created.body);
         Assert.AreEqual("manual", BounceTestSupport.Text(created.body, "source"), created.body);
         Assert.AreEqual("Left the company in August.", BounceTestSupport.Text(created.body, "diagnostic"), created.body);
         Assert.AreNotEqual("", BounceTestSupport.Text(created.body, "expires"), "An expiry in days is stored as a time: " + created.body);

         Assert.AreEqual(409, Http("POST", DomainPath + "/suppressions", "{\"address\":\"" + Remote + "\"}").status);

         (int status, string body) hosted = Http("POST", DomainPath + "/suppressions", "{\"address\":\"hosted@" + _domain.Name + "\"}");
         Assert.AreEqual(400, hosted.status, hosted.body);
         StringAssert.Contains("hosted", hosted.body);

         Assert.AreEqual(400, Http("POST", DomainPath + "/suppressions", "{\"address\":\"not an address\"}").status);
         Assert.AreEqual(400, Http("POST", DomainPath + "/suppressions", "{\"note\":\"no address\"}").status);
         Assert.AreEqual(400, Http("POST", DomainPath + "/suppressions",
            "{\"address\":\"later@" + BounceTestSupport.RemoteDomain + "\",\"expires_days\":3651}").status);
         Assert.AreEqual(404, Http("POST", "/api/v1/domains/no-such-domain.test/suppressions", "{\"address\":\"" + Remote + "\"}").status);
         Assert.AreEqual(404, Http("GET", "/api/v1/domains/no-such-domain.test/suppressions").status);

         Assert.AreEqual(201, Http("POST", DomainPath + "/suppressions", "{\"address\":\"other@" + BounceTestSupport.RemoteDomain + "\"}").status);

         string all = Read(DomainPath + "/suppressions");
         Assert.AreEqual(2, BounceTestSupport.Number(all, "total"), all);

         string searched = Read(DomainPath + "/suppressions?search=gone");
         Assert.AreEqual(1, BounceTestSupport.Number(searched, "total"), searched);
         BounceTestSupport.AssertItemFor(searched, Remote);
         Assert.IsNull(BounceTestSupport.ItemFor(searched, "other@" + BounceTestSupport.RemoteDomain), searched);

         string paged = Read(DomainPath + "/suppressions?limit=1&offset=1");
         Assert.AreEqual(2, BounceTestSupport.Number(paged, "total"), paged);
         Assert.AreEqual(1, Regex.Matches(paged, "\"address\":").Count, "A page of one holds one item: " + paged);

         (int status, string body) removed = Http("DELETE", DomainPath + "/suppressions/" + Uri.EscapeDataString(Remote));
         Assert.AreEqual(200, removed.status, removed.body);
         Assert.AreEqual(404, Http("DELETE", DomainPath + "/suppressions/" + Remote).status);

         Assert.AreEqual(200, Http("DELETE", DomainPath + "/suppressions/other@" + BounceTestSupport.RemoteDomain).status);
         Assert.AreEqual(0, BounceTestSupport.Number(Read(DomainPath + "/suppressions"), "total"));
      }

      [Test]
      [Description("A key issued for one domain reads and changes that domain's list only - 403 on another domain's, on the settings group and on the sweep - and a read-only key can read the list but not add to it or remove from it.")]
      public void ADomainScopedKeyReachesOnlyItsOwnDomainsList()
      {
         const string OtherDomain = "bounces-other.test";
         SingletonProvider<TestSetup>.Instance.AddDomain(OtherDomain);

         Assert.AreEqual(201, Http("POST", DomainPath + "/suppressions", "{\"address\":\"" + Remote + "\"}").status);

         (string id, string token) scoped = CreateKey("regression - bounces domain scoped", "full", _domain.Name);
         (string id, string token) readOnly = CreateKey("regression - bounces read-only", "readonly", null);

         try
         {
            Assert.AreEqual(200, Bearer("GET", DomainPath + "/suppressions", scoped.token).status);
            Assert.AreEqual(200, Bearer("GET", DomainPath + "/bounces", scoped.token).status);

            (int status, string body) own = Bearer("POST", DomainPath + "/suppressions", scoped.token,
               "{\"address\":\"scoped@" + BounceTestSupport.RemoteDomain + "\"}");
            Assert.AreEqual(201, own.status, own.body);
            Assert.AreEqual(200, Bearer("DELETE", DomainPath + "/suppressions/scoped@" + BounceTestSupport.RemoteDomain, scoped.token).status);

            Assert.AreEqual(403, Bearer("GET", "/api/v1/domains/" + OtherDomain + "/suppressions", scoped.token).status,
               "A key for one domain read another domain's list.");
            Assert.AreEqual(403, Bearer("GET", "/api/v1/domains/" + OtherDomain + "/bounces", scoped.token).status);
            Assert.AreEqual(403, Bearer("POST", "/api/v1/domains/" + OtherDomain + "/suppressions", scoped.token,
               "{\"address\":\"planted@" + BounceTestSupport.RemoteDomain + "\"}").status);
            Assert.AreEqual(403, Bearer("DELETE", "/api/v1/domains/" + OtherDomain + "/suppressions/" + Remote, scoped.token).status);
            Assert.AreEqual(0, BounceTestSupport.Number(Read("/api/v1/domains/" + OtherDomain + "/suppressions"), "total"),
               "A refused POST put something on the other domain's list.");

            Assert.AreEqual(403, Bearer("GET", SettingsPath, scoped.token).status, "The bounce settings apply to every domain.");
            Assert.AreEqual(403, Bearer("PUT", SettingsPath, scoped.token, "{\"processing_enabled\":true}").status);
            Assert.AreEqual(403, Bearer("POST", "/api/v1/bounces/expire", scoped.token, "{}").status, "The sweep removes entries from every domain.");
            Assert.AreEqual(200, Bearer("POST", "/api/v1/bounces/classify", scoped.token, "{\"status\":\"5.1.1\"}").status,
               "Classify reads and writes nothing, so it is anybody's.");

            Assert.AreEqual(200, Bearer("GET", DomainPath + "/suppressions", readOnly.token).status);
            Assert.AreEqual(403, Bearer("POST", DomainPath + "/suppressions", readOnly.token,
               "{\"address\":\"readonly@" + BounceTestSupport.RemoteDomain + "\"}").status);
            Assert.AreEqual(403, Bearer("DELETE", DomainPath + "/suppressions/" + Uri.EscapeDataString(Remote), readOnly.token).status);
            BounceTestSupport.AssertItemFor(Read(DomainPath + "/suppressions"), Remote);
         }
         finally
         {
            Http("DELETE", "/api/v1/apikeys/" + scoped.id);
            Http("DELETE", "/api/v1/apikeys/" + readOnly.id);
         }
      }

      [Test]
      [Description("POST /api/v1/bounces/classify answers how a status or a reply would be read - a bare 550 as other and not trusted - and writes nothing.")]
      public void ClassifyAnswersWithoutTouchingAnything()
      {
         string hard = Classify("{\"status\":\"550 5.1.1 <gone@example.com>: User unknown\"}");
         Assert.AreEqual("hard", BounceTestSupport.Text(hard, "category"), hard);
         Assert.IsTrue(BounceTestSupport.Flag(hard, "trusted"), hard);
         Assert.IsTrue(BounceTestSupport.Flag(hard, "suppresses"), hard);

         string reportStatus = Classify("{\"status\":\"5.1.10\",\"diagnostic\":\"smtp; 550 5.1.10 RESOLVER.ADR.RecipientNotFound\"}");
         Assert.AreEqual("hard", BounceTestSupport.Text(reportStatus, "category"), reportStatus);

         string full = Classify("{\"status\":\"552 5.2.2 Mailbox full\"}");
         Assert.AreEqual("mailbox_full", BounceTestSupport.Text(full, "category"), full);
         Assert.IsFalse(BounceTestSupport.Flag(full, "suppresses"), "A full mailbox does not suppress by default: " + full);

         string policy = Classify("{\"status\":\"550 5.7.1 Message rejected as spam\"}");
         Assert.AreEqual("policy", BounceTestSupport.Text(policy, "category"), policy);
         Assert.IsFalse(BounceTestSupport.Flag(policy, "suppresses"), policy);

         string bare = Classify("{\"status\":\"550 <gone@example.com>\"}");
         Assert.AreEqual("other", BounceTestSupport.Text(bare, "category"), bare);
         Assert.IsFalse(BounceTestSupport.Flag(bare, "trusted"), bare);
         Assert.AreEqual("5.1.1", BounceTestSupport.Text(bare, "status"),
            "The code delivery would have recorded is shown, and is still not believed: " + bare);

         string greylisted = Classify("{\"status\":\"451 4.7.1 Greylisted, try again later\"}");
         Assert.AreEqual("soft", BounceTestSupport.Text(greylisted, "category"), greylisted);

         Assert.AreEqual(400, Http("POST", "/api/v1/bounces/classify", "{}").status);
         Assert.AreEqual(400, Http("POST", "/api/v1/bounces/classify", "not json").status);

         Assert.AreEqual(0, BounceTestSupport.Number(Read(DomainPath + "/bounces"), "total"));
         Assert.AreEqual(0, BounceTestSupport.Number(Read(DomainPath + "/suppressions"), "total"));
      }

      [Test]
      [Description("The Control Panel's COM methods answer the same documents the routes do, and add, remove, classify and sweep the same way.")]
      public void TheComMethodsAnswerWhatTheRoutesAnswer()
      {
         Utilities utilities = _application.Utilities;

         Assert.IsTrue(BounceTestSupport.ComAddSuppression(utilities, _domain.Name, Remote, "Added over COM.", 0));
         Assert.IsFalse(BounceTestSupport.ComAddSuppression(utilities, _domain.Name, Remote, "Twice.", 0), "Listed already answers False.");
         Assert.Throws<COMException>(() => BounceTestSupport.ComAddSuppression(utilities, _domain.Name, "not an address", "", 0));
         Assert.Throws<COMException>(() => BounceTestSupport.ComAddSuppression(utilities, _domain.Name, "later@" + BounceTestSupport.RemoteDomain, "", 3651));

         string overCom = BounceTestSupport.ComGetSuppressionList(utilities, _domain.Name, "", 100);
         string item = BounceTestSupport.AssertItemFor(overCom, Remote);
         Assert.AreEqual("manual", BounceTestSupport.Text(item, "source"), item);
         Assert.AreEqual("Added over COM.", BounceTestSupport.Text(item, "diagnostic"), item);

         string overRest = Read(DomainPath + "/suppressions");
         Assert.AreEqual(BounceTestSupport.AssertItemFor(overRest, Remote), item, "The two surfaces answer the same entry.");

         StringAssert.Contains("\"total\":0", BounceTestSupport.ComGetBounceCounts(utilities, _domain.Name, "", 100));

         string classified = BounceTestSupport.ComClassifyBounce(utilities, "550 5.1.1 User unknown", "");
         Assert.AreEqual("hard", BounceTestSupport.Text(classified, "category"), classified);

         Assert.IsTrue(BounceTestSupport.ComRemoveSuppression(utilities, _domain.Name, Remote));
         Assert.IsFalse(BounceTestSupport.ComRemoveSuppression(utilities, _domain.Name, Remote), "Not listed answers False.");

         Assert.GreaterOrEqual(BounceTestSupport.ComRunBounceMaintenance(utilities), 0);
      }

      [Test]
      [Description("The OpenAPI document describes the six routes and the settings group, which is what the Deck draws its forms from.")]
      public void TheOpenApiDocumentDescribesTheRoutes()
      {
         string document = Read("/api/v1/openapi.json");

         foreach (string path in new[]
                  {
                     "\"/api/v1/domains/{domain}/suppressions\"",
                     "\"/api/v1/domains/{domain}/suppressions/{address}\"",
                     "\"/api/v1/domains/{domain}/bounces\"",
                     "\"/api/v1/bounces/classify\"",
                     "\"/api/v1/bounces/expire\"",
                     "\"/api/v1/settings/bounces\""
                  })
         {
            StringAssert.Contains(path, document, "The OpenAPI document does not describe " + path + ".");
         }

         int at = document.IndexOf("\"/api/v1/settings/bounces\"", StringComparison.Ordinal);
         string entry = document.Substring(at, Math.Min(6000, document.Length - at));
         StringAssert.Contains("suppression_action", entry);
         StringAssert.Contains("report_window_days", entry);
      }

      // ---- helpers -----------------------------------------------------------

      private string Classify(string body)
      {
         (int status, string answer) = Http("POST", "/api/v1/bounces/classify", body);
         Assert.AreEqual(200, status, answer);
         return answer;
      }

      private static (string id, string token) CreateKey(string label, string scope, string domains)
      {
         string body = "{\"label\":\"" + label + "\",\"scope\":\"" + scope + "\"" +
                       (domains == null ? "" : ",\"domains\":\"" + domains + "\"") + "}";

         (int status, string answer) = Http("POST", "/api/v1/apikeys", body);
         Assert.AreEqual(201, status, "POST /api/v1/apikeys must create a key. Body: " + answer);

         return (BounceTestSupport.Text(answer, "id"), BounceTestSupport.Text(answer, "key"));
      }

      private static (int status, string body) Bearer(string method, string path, string token, string requestBody = null)
      {
         return RestRequest.Send("127.0.0.1", RestPort, method, path, "Bearer " + token, requestBody);
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
