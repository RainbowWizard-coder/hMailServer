// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Text;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;

namespace RegressionTests.API
{
   /// <summary>
   ///    The two switches of GitLab issue 51 over REST: refuse_helo_not_fqdn and
   ///    refuse_mail_from_domain_not_fqdn in PUT /api/v1/settings/antispam, read and
   ///    written each on its own, the same values COM reads, a value that is not a
   ///    boolean refused with nothing changed, and both described in the OpenAPI
   ///    document the Control Deck builds its Anti-spam form from. What the
   ///    switches DO to a session is AntiSpam/FqdnRefusal's; this is the resource.
   ///
   ///    Everything here goes over the API, and what it changes it puts back, so the
   ///    Linux suite links this fixture.
   /// </summary>
   [TestFixture]
   public class RestApiFqdnRefusal : TestFixtureBase
   {
      private static int RestPort = 9681;
      private const string AdminPassword = "testar";

      private string settingsBefore_;

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(AdminPassword);

         RestPort = RestListener.Start(RestPort);

         _application.Reinitialize();

         (int status, string body) probe = Http("GET", "/api/v1/status");
         ClassicAssert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);

         Dictionary<string, object> settings = AntiSpamSettings();
         settingsBefore_ = "{\"refuse_helo_not_fqdn\":" + (MiniJson.Flag(settings, "refuse_helo_not_fqdn") ? "true" : "false") +
                           ",\"refuse_mail_from_domain_not_fqdn\":" + (MiniJson.Flag(settings, "refuse_mail_from_domain_not_fqdn") ? "true" : "false") + "}";
      }

      [TearDown]
      public void StopRestApi()
      {
         try
         {
            Http("PUT", "/api/v1/settings/antispam", settingsBefore_);
         }
         finally
         {
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      [Test]
      [Description("Both switches are in the anti-spam group, each written and read back on its own, and COM reads what REST wrote.")]
      public void TheTwoSwitchesAreWrittenAndReadBackEachOnItsOwn()
      {
         Dictionary<string, object> settings = AntiSpamSettings();
         ClassicAssert.IsTrue(settings.ContainsKey("refuse_helo_not_fqdn"), "refuse_helo_not_fqdn is in the group");
         ClassicAssert.IsTrue(settings.ContainsKey("refuse_mail_from_domain_not_fqdn"), "refuse_mail_from_domain_not_fqdn is in the group");

         dynamic antiSpam = _settings.AntiSpam;

         (int status, string body) heloOnly = Http("PUT", "/api/v1/settings/antispam",
            "{\"refuse_helo_not_fqdn\":true,\"refuse_mail_from_domain_not_fqdn\":false}");
         ClassicAssert.AreEqual(200, heloOnly.status, heloOnly.body);

         settings = AntiSpamSettings();
         ClassicAssert.IsTrue(MiniJson.Flag(settings, "refuse_helo_not_fqdn"));
         ClassicAssert.IsFalse(MiniJson.Flag(settings, "refuse_mail_from_domain_not_fqdn"));
         ClassicAssert.IsTrue((bool) antiSpam.RefuseHeloNotFqdn, "COM reads what REST wrote");
         ClassicAssert.IsFalse((bool) antiSpam.RefuseMailFromDomainNotFqdn, "COM reads what REST wrote");

         (int status, string body) senderOnly = Http("PUT", "/api/v1/settings/antispam",
            "{\"refuse_helo_not_fqdn\":false,\"refuse_mail_from_domain_not_fqdn\":true}");
         ClassicAssert.AreEqual(200, senderOnly.status, senderOnly.body);

         settings = AntiSpamSettings();
         ClassicAssert.IsFalse(MiniJson.Flag(settings, "refuse_helo_not_fqdn"));
         ClassicAssert.IsTrue(MiniJson.Flag(settings, "refuse_mail_from_domain_not_fqdn"));
         ClassicAssert.IsFalse((bool) antiSpam.RefuseHeloNotFqdn, "COM reads what REST wrote");
         ClassicAssert.IsTrue((bool) antiSpam.RefuseMailFromDomainNotFqdn, "COM reads what REST wrote");
      }

      [Test]
      [Description("A value that is not true or false is refused with 400 and the sentence naming the key, and changes nothing.")]
      public void AValueThatIsNotABooleanIsRefused()
      {
         (int status, string body) off = Http("PUT", "/api/v1/settings/antispam",
            "{\"refuse_helo_not_fqdn\":false,\"refuse_mail_from_domain_not_fqdn\":false}");
         ClassicAssert.AreEqual(200, off.status, off.body);

         (int status, string body) word = Http("PUT", "/api/v1/settings/antispam", "{\"refuse_helo_not_fqdn\":\"yes\"}");
         ClassicAssert.AreEqual(400, word.status, word.body);
         StringAssert.Contains("refuse_helo_not_fqdn must be true or false", word.body, word.body);

         (int status, string body) number = Http("PUT", "/api/v1/settings/antispam", "{\"refuse_mail_from_domain_not_fqdn\":1}");
         ClassicAssert.AreEqual(400, number.status, number.body);
         StringAssert.Contains("refuse_mail_from_domain_not_fqdn must be true or false", number.body, number.body);

         Dictionary<string, object> settings = AntiSpamSettings();
         ClassicAssert.IsFalse(MiniJson.Flag(settings, "refuse_helo_not_fqdn"), "A refused write changed nothing");
         ClassicAssert.IsFalse(MiniJson.Flag(settings, "refuse_mail_from_domain_not_fqdn"), "A refused write changed nothing");
      }

      [Test]
      [Description("The OpenAPI document describes both as booleans of the anti-spam group's PUT, which is what the Deck draws its form from.")]
      public void TheSwitchesAreInTheOpenApiDocument()
      {
         (int status, string body) spec = Http("GET", "/api/v1/openapi.json");
         ClassicAssert.AreEqual(200, spec.status, spec.body.Length > 400 ? spec.body.Substring(0, 400) : spec.body);

         ClassicAssert.IsTrue(spec.body.Contains("\"refuse_helo_not_fqdn\":{\"type\":\"boolean\",\"description\":\"Whether a session whose HELO/EHLO name is not a fully qualified domain name"),
            "refuse_helo_not_fqdn is described as a boolean");
         ClassicAssert.IsTrue(spec.body.Contains("\"refuse_mail_from_domain_not_fqdn\":{\"type\":\"boolean\",\"description\":\"Whether a session whose envelope sender's domain is not a fully qualified domain name"),
            "refuse_mail_from_domain_not_fqdn is described as a boolean");
      }

      // ------------------------------------------------------------------ helpers

      private static Dictionary<string, object> AntiSpamSettings()
      {
         (int status, string body) = Http("GET", "/api/v1/settings/antispam");
         ClassicAssert.AreEqual(200, status, body);
         return MiniJson.Object(body);
      }

      private static (int status, string body) Http(string method, string path, string requestBody = null)
      {
         string credentials = Convert.ToBase64String(Encoding.ASCII.GetBytes("Administrator:" + AdminPassword));
         return RestRequest.Send("127.0.0.1", RestPort, method, path, "Basic " + credentials, requestBody);
      }
   }
}
