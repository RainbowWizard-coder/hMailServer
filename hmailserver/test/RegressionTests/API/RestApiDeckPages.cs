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

namespace RegressionTests.API
{
   /// <summary>
   ///    What the Control Deck's last pages needed from the REST API and did not
   ///    have: the diagnostic run the desktop program's Diagnostics page makes
   ///    over COM, and the two settings of the quarantine digest only the
   ///    desktop program could write. With these the parity census reads every
   ///    property the Control Panel writes as writable over REST.
   ///
   ///    Every value a write claims to have changed is read back through COM,
   ///    and every refusal is checked to have changed nothing.
   /// </summary>
   [TestFixture]
   public class RestApiDeckPages : TestFixtureBase
   {
      private static int RestPort = 9612;
      private const string AdminPassword = "testar";

      private string _baseUrlBefore;
      private int _linkHoursBefore;

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(AdminPassword);
         _baseUrlBefore = _settings.QuarantineDigestBaseUrl;
         _linkHoursBefore = _settings.QuarantineDigestLinkHours;

         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();

         (int status, string body) probe = Http("GET", "/api/v1/status", null);
         Assert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);
      }

      [TearDown]
      public void StopRestApi()
      {
         try
         {
            _settings.QuarantineDigestBaseUrl = _baseUrlBefore;
            _settings.QuarantineDigestLinkHours = _linkHoursBefore;
         }
         finally
         {
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      [Test]
      [Description("The two quarantine digest settings are on GET and PUT /api/v1/settings: a change is what COM reads afterwards, and a base URL that is not absolute or a link life outside 1 to 720 hours is refused in the Control Panel's own words with nothing changed.")]
      public void TheQuarantineDigestSettingsAreOnTheSettingsRoute()
      {
         (int status, string body) listed = Http("GET", "/api/v1/settings", null);
         Assert.AreEqual(200, listed.status, listed.body);
         StringAssert.Contains("\"quarantine_digest_base_url\":", listed.body);
         StringAssert.Contains("\"quarantine_digest_link_hours\":", listed.body);

         (int status, string body) saved = Http("PUT", "/api/v1/settings",
            "{\"quarantine_digest_base_url\":\"  https://mail.example.test  \",\"quarantine_digest_link_hours\":48}");
         Assert.AreEqual(200, saved.status, saved.body);
         Assert.AreEqual("https://mail.example.test", _settings.QuarantineDigestBaseUrl, "Trimmed, as the COM setter trims it.");
         Assert.AreEqual(48, _settings.QuarantineDigestLinkHours);

         (int status, string body) notAbsolute = Http("PUT", "/api/v1/settings", "{\"quarantine_digest_base_url\":\"mail.example.test\"}");
         Assert.AreEqual(400, notAbsolute.status, notAbsolute.body);
         StringAssert.Contains("begins with http:// or https://", notAbsolute.body);
         Assert.AreEqual("https://mail.example.test", _settings.QuarantineDigestBaseUrl, "A refused value changes nothing.");

         foreach (int hours in new[] { 0, 721 })
         {
            (int status, string body) refused = Http("PUT", "/api/v1/settings", "{\"quarantine_digest_link_hours\":" + hours + "}");
            Assert.AreEqual(400, refused.status, refused.body);
            StringAssert.Contains("between 1 and 720 hours", refused.body);
            Assert.AreEqual(48, _settings.QuarantineDigestLinkHours, "A refused value changes nothing.");
         }

         // Empty is a value: it sends no digest at all.
         Assert.AreEqual(200, Http("PUT", "/api/v1/settings", "{\"quarantine_digest_base_url\":\"\"}").status);
         Assert.AreEqual("", _settings.QuarantineDigestBaseUrl);
      }

      [Test]
      [Description("POST /api/v1/diagnostics/run runs the tests the desktop program's Diagnostics page runs over COM and answers the same results by name, each with its description, whether it passed and what it found; the domains asked about are echoed, and a run without a credential is refused.")]
      public void TheDiagnosticRunIsWhatComRuns()
      {
         string localDomain = _domain.Name;

         (int status, string body) run = Http("POST", "/api/v1/diagnostics/run",
            "{\"local_domain\":\"" + localDomain + "\",\"test_domain\":\"example.com\"}");
         Assert.AreEqual(200, run.status, run.body);

         Dictionary<string, object> answer = MiniJson.Object(run.body);
         Assert.AreEqual(localDomain, answer["local_domain"]);
         Assert.AreEqual("example.com", answer["test_domain"]);

         var results = (List<object>) answer["results"];
         Assert.Greater(results.Count, 0, run.body);

         var restNames = new List<string>();
         foreach (object item in results)
         {
            var result = (Dictionary<string, object>) item;
            Assert.IsInstanceOf<string>(result["name"]);
            Assert.IsInstanceOf<string>(result["description"]);
            Assert.IsInstanceOf<bool>(result["success"]);
            Assert.IsInstanceOf<string>(result["details"]);
            restNames.Add((string) result["name"]);
         }

         hMailServer.Diagnostics diagnostics = _application.Diagnostics;
         diagnostics.LocalDomainName = localDomain;
         diagnostics.TestDomainName = "example.com";
         hMailServer.DiagnosticResults viaCom = diagnostics.PerformTests();
         var comNames = new List<string>();
         for (int i = 0; i < viaCom.Count; i++)
            comNames.Add(viaCom.get_Item(i).Name);

         CollectionAssert.AreEqual(comNames, restNames, "The same tests, in the same order, as the desktop program runs.");

         // The installation paths test is one this bench can judge: it names the ini.
         var paths = (Dictionary<string, object>) results.Find(r => (string) ((Dictionary<string, object>) r)["name"] == "Installation paths");
         Assert.IsNotNull(paths, run.body);
         StringAssert.Contains("hMailServer.ini", (string) paths["details"]);

         Assert.AreEqual(401, Http("POST", "/api/v1/diagnostics/run", "{}", authorization: null).status);
      }

      private static (int status, string body) Http(string method, string path, string requestBody)
      {
         string credentials = Convert.ToBase64String(Encoding.ASCII.GetBytes("Administrator:" + AdminPassword));
         return RestRequest.Send("127.0.0.1", RestPort, method, path, "Basic " + credentials, requestBody);
      }

      private static (int status, string body) Http(string method, string path, string requestBody, string authorization)
      {
         return RestRequest.Send("127.0.0.1", RestPort, method, path, authorization, requestBody);
      }
   }
}
