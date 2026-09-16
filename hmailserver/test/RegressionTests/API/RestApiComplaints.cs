// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Runtime.InteropServices;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.API
{
   /// <summary>
   ///    The feedback loops' administration surface: the settings group, a domain's
   ///    complaints, every domain's rate and the sweep over REST, and the same
   ///    documents over COM, which is what the Control Panel reads.
   ///
   ///    What is held here beside the round trips is who may do what. A domain's
   ///    complaints are that domain's, so a key issued for one domain reads that
   ///    domain's and gets 403 on another's, on the settings that apply to every
   ///    domain, on the rates that are every domain's, and on the sweep that prunes
   ///    all of them; a read-only key reads and changes nothing. And the complaint
   ///    mailbox is refused unless every address in it is one this server hosts,
   ///    because a feedback loop is registered against a mailbox here and a list
   ///    naming an address somewhere else would read no report and never say so.
   /// </summary>
   [TestFixture]
   public class RestApiComplaints : TestFixtureBase
   {
      private static int RestPort = 9555;

      private const string SettingsPath = "/api/v1/settings/complaints";
      private const string RatesPath = "/api/v1/complaints/rates";
      private const string ExpirePath = "/api/v1/complaints/expire";

      private string DomainPath
      {
         get { return "/api/v1/domains/" + _domain.Name; }
      }

      private string Mailbox
      {
         get { return "fbl@" + _domain.Name; }
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

         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, Mailbox, "test");
      }

      [TearDown]
      public void RestoreSettingsAndStopRestApi()
      {
         try
         {
            ComplaintTestSupport.RestoreDefaults(_settings);
         }
         finally
         {
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      [Test]
      [Description("GET and PUT /api/v1/settings/complaints are the four Settings properties: written over REST and read back over COM, and a value out of range is refused naming the key, with nothing in the body applied.")]
      public void TheSettingsGroupRoundTripsAndRefusesOutOfRange()
      {
         (int status, string body) written = Http("PUT", SettingsPath,
            "{\"processing_enabled\":true,\"mailbox\":\"" + Mailbox + "\",\"suppression_threshold\":3,\"retention_days\":45}");
         Assert.AreEqual(200, written.status, written.body);

         Assert.IsTrue(ComplaintTestSupport.ComComplaintProcessingEnabled(_settings));
         Assert.AreEqual(Mailbox, ComplaintTestSupport.ComComplaintMailbox(_settings));
         Assert.AreEqual(3, ComplaintTestSupport.ComComplaintSuppressionThreshold(_settings));
         Assert.AreEqual(45, ComplaintTestSupport.ComComplaintRetentionDays(_settings));

         string read = Read(SettingsPath);
         Assert.IsTrue(BounceTestSupport.Flag(read, "processing_enabled"), read);
         Assert.AreEqual(Mailbox, BounceTestSupport.Text(read, "mailbox"), read);
         Assert.AreEqual(3, BounceTestSupport.Number(read, "suppression_threshold"), read);
         Assert.AreEqual(45, BounceTestSupport.Number(read, "retention_days"), read);

         // All or nothing: the acceptable key beside the refused one is not applied.
         (int status, string body) threshold = Http("PUT", SettingsPath, "{\"retention_days\":10,\"suppression_threshold\":101}");
         Assert.AreEqual(400, threshold.status, threshold.body);
         StringAssert.Contains("suppression_threshold", threshold.body);
         Assert.AreEqual(45, ComplaintTestSupport.ComComplaintRetentionDays(_settings), "A refused body changed a setting.");

         (int status, string body) retention = Http("PUT", SettingsPath, "{\"retention_days\":3651}");
         Assert.AreEqual(400, retention.status, retention.body);
         StringAssert.Contains("retention_days", retention.body);

         // COM refuses the same ranges rather than clamping them.
         Assert.Throws<COMException>(() => ComplaintTestSupport.ComSetComplaintSuppressionThreshold(_settings, 101));
         Assert.Throws<COMException>(() => ComplaintTestSupport.ComSetComplaintRetentionDays(_settings, 3651));
      }

      [Test]
      [Description("The complaint mailbox has to be an address this server hosts, over REST and over COM in the same sentence, because a provider delivers its reports to it; an empty value is the default and means no report is ever read.")]
      public void TheMailboxMustBeAnAddressThisServerHosts()
      {
         // The shipped defaults, before anything in this fixture writes one: the
         // feature off, no mailbox, and the threshold at 0 - record and count, list
         // nobody - which is what docs/Complaints.md tells an operator to start with.
         // A default of 1 would mean one forged report to a discoverable address
         // refusing whatever address it named, for the whole domain.
         Assert.AreEqual(0, ComplaintTestSupport.ComComplaintSuppressionThreshold(_settings),
            "The shipped complaint threshold is not the observe-only 0 the page recommends.");

         (int status, string body) outside = Http("PUT", SettingsPath, "{\"mailbox\":\"fbl@" + BounceTestSupport.RemoteDomain + "\"}");
         Assert.AreEqual(400, outside.status, outside.body);
         StringAssert.Contains("hosts", outside.body, outside.body);
         Assert.AreEqual("", ComplaintTestSupport.ComComplaintMailbox(_settings), "A refused mailbox was stored anyway.");

         (int status, string body) nonsense = Http("PUT", SettingsPath, "{\"mailbox\":\"not an address\"}");
         Assert.AreEqual(400, nonsense.status, nonsense.body);

         Assert.AreEqual(200, Http("PUT", SettingsPath, "{\"mailbox\":\"" + Mailbox + "\"}").status);
         Assert.AreEqual(200, Http("PUT", SettingsPath, "{\"mailbox\":\"\"}").status, "An empty mailbox is the default and is accepted.");
         Assert.AreEqual("", ComplaintTestSupport.ComComplaintMailbox(_settings));

         Assert.Throws<COMException>(() => ComplaintTestSupport.ComSetComplaintMailbox(_settings, "fbl@" + BounceTestSupport.RemoteDomain));
         Assert.Throws<COMException>(() => ComplaintTestSupport.ComSetComplaintMailbox(_settings, "not an address"));
      }

      [Test]
      [Description("The listing answers for a domain that has had no complaint - the rate, the settings in force and an empty list, rather than an error - and 404 for a domain that does not exist.")]
      public void TheListingAnswersForADomainWithNoComplaints()
      {
         Assert.AreEqual(200, Http("PUT", SettingsPath, "{\"processing_enabled\":true,\"mailbox\":\"" + Mailbox + "\"}").status);

         string document = Read(DomainPath + "/complaints");

         Assert.AreEqual(_domain.Name, BounceTestSupport.Text(document, "domain"), document);
         Assert.IsTrue(BounceTestSupport.Flag(document, "processing_enabled"), document);
         Assert.IsTrue(BounceTestSupport.Flag(document, "mailbox_configured"), document);

         // A key confined to one domain reads this document, and the mailbox address
         // is the one thing that lets a report be forged. It belongs in the
         // server-wide settings group and nowhere else, and so does the threshold.
         StringAssert.DoesNotContain(Mailbox, document, "The per-domain document gave away the complaint mailbox address.");
         StringAssert.DoesNotContain("suppression_threshold", document, document);
         Assert.AreEqual(0, BounceTestSupport.Number(document, "total"), document);
         Assert.AreEqual(7, BounceTestSupport.Number(document, "window_days"), document);
         Assert.AreEqual(0, BounceTestSupport.Number(document, "rate_ppm"), document);
         Assert.IsFalse(BounceTestSupport.Flag(document, "rate_judged"), document);
         StringAssert.Contains("\"items\":[]", document, document);

         Assert.AreEqual(404, Http("GET", "/api/v1/domains/no-such-domain.test/complaints").status);
      }

      [Test]
      [Description("The rates route answers every hosted domain's window, floor and figures, and the sweep answers what it pruned; both are the documents the COM methods return.")]
      public void TheRatesAndTheSweepAnswer()
      {
         string rates = Read(RatesPath);
         Assert.AreEqual(7, BounceTestSupport.Number(rates, "window_days"), rates);
         Assert.AreEqual(100, BounceTestSupport.Number(rates, "minimum_volume"), rates);
         StringAssert.Contains("\"domains\":[", rates, rates);

         (int status, string body) swept = Http("POST", ExpirePath);
         Assert.AreEqual(200, swept.status, swept.body);
         Assert.IsTrue(BounceTestSupport.Number(swept.body, "complaints_pruned") >= 0, swept.body);

         Utilities utilities = _application.Utilities;

         string overCom = ComplaintTestSupport.ComGetComplaintRates(utilities);
         Assert.AreEqual(BounceTestSupport.Number(rates, "minimum_volume"), BounceTestSupport.Number(overCom, "minimum_volume"),
            "The two surfaces answer the same rates document.");

         string listOverCom = ComplaintTestSupport.ComGetComplaints(utilities, _domain.Name, "", 100);
         Assert.AreEqual(_domain.Name, BounceTestSupport.Text(listOverCom, "domain"), listOverCom);
         Assert.AreEqual(0, BounceTestSupport.Number(listOverCom, "total"), listOverCom);

         Assert.Throws<COMException>(() => ComplaintTestSupport.ComGetComplaints(utilities, "no-such-domain.test", "", 100));
      }

      [Test]
      [Description("A key issued for one domain reads that domain's complaints and is refused another's, the settings, the rates and the sweep; a read-only key reads and changes nothing.")]
      public void AKeyIsConfinedToItsDomainAndItsScope()
      {
         const string OtherDomain = "complaints-other.test";
         SingletonProvider<TestSetup>.Instance.AddDomain(OtherDomain);

         (string id, string token) scoped = CreateKey("regression - complaints domain scoped", "full", _domain.Name);
         (string id, string token) readOnly = CreateKey("regression - complaints read-only", "readonly", null);

         try
         {
            Assert.AreEqual(200, Bearer("GET", DomainPath + "/complaints", scoped.token).status);

            Assert.AreEqual(403, Bearer("GET", "/api/v1/domains/" + OtherDomain + "/complaints", scoped.token).status,
               "A key for one domain read another domain's complaints.");
            Assert.AreEqual(403, Bearer("GET", SettingsPath, scoped.token).status, "The complaint settings apply to every domain.");
            Assert.AreEqual(403, Bearer("PUT", SettingsPath, scoped.token, "{\"processing_enabled\":true}").status);
            Assert.AreEqual(403, Bearer("GET", RatesPath, scoped.token).status, "The rates are every domain's.");
            Assert.AreEqual(403, Bearer("POST", ExpirePath, scoped.token, "{}").status, "The sweep prunes every domain's rows.");

            Assert.AreEqual(200, Bearer("GET", DomainPath + "/complaints", readOnly.token).status);
            Assert.AreEqual(403, Bearer("POST", ExpirePath, readOnly.token, "{}").status);
            Assert.AreEqual(403, Bearer("PUT", SettingsPath, readOnly.token, "{\"processing_enabled\":true}").status);
         }
         finally
         {
            Http("DELETE", "/api/v1/apikeys/" + scoped.id);
            Http("DELETE", "/api/v1/apikeys/" + readOnly.id);
         }
      }

      [Test]
      [Description("The OpenAPI document describes the three routes and the settings group, which is what the Deck draws its forms from.")]
      public void TheOpenApiDocumentDescribesTheRoutes()
      {
         string document = Read("/api/v1/openapi.json");

         foreach (string path in new[]
                  {
                     "\"/api/v1/domains/{domain}/complaints\"",
                     "\"/api/v1/complaints/rates\"",
                     "\"/api/v1/complaints/expire\"",
                     "\"/api/v1/settings/complaints\""
                  })
         {
            StringAssert.Contains(path, document, "The OpenAPI document does not describe " + path + ".");
         }

         int at = document.IndexOf("\"/api/v1/settings/complaints\"", StringComparison.Ordinal);
         string entry = document.Substring(at, Math.Min(6000, document.Length - at));
         StringAssert.Contains("suppression_threshold", entry);
         StringAssert.Contains("retention_days", entry);
      }

      [Test]
      [Description("The complaints.rate.high alert rule is seeded by the schema step, on, with the threshold in parts per million that the providers call too high.")]
      public void TheAlertRuleIsSeeded()
      {
         string rules = Read("/api/v1/alerts/rules");

         int at = rules.IndexOf("\"complaints.rate.high\"", StringComparison.Ordinal);
         Assert.IsTrue(at >= 0, "The complaints.rate.high alert rule was not seeded: " + rules);

         int start = rules.LastIndexOf('{', at);
         int end = rules.IndexOf('}', at);
         string rule = rules.Substring(start, end - start + 1);

         Assert.IsTrue(BounceTestSupport.Flag(rule, "enabled"), rule);
         Assert.AreEqual(1000, BounceTestSupport.Number(rule, "threshold"), rule);
      }

      // ---- helpers -----------------------------------------------------------

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
