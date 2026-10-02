// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Text;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.API
{
   /// <summary>
   ///    A JSON number past the range of the field it is written into. The REST
   ///    routes read numbers through JsonValue, whose AsInt64 converted a number
   ///    with a fraction or an exponent from its double - undefined behaviour in
   ///    C++ for 1e20 - and whose callers then narrowed the answer to an int, which
   ///    keeps the low 32 bits: 4294967301 became 5. A field held to an int's range
   ///    now reaches its own range check as the large number it is, and is refused
   ///    by it or kept at the largest int, never wrapped to a small value that
   ///    passes. JsonValueTester holds the conversion itself to its cases; this is
   ///    what a caller of the routes sees, on both servers.
   /// </summary>
   [TestFixture]
   public class RestApiNumberRanges : TestFixtureBase
   {
      private static int RestPort = 9652;
      private const string AdminPassword = "testar";
      private const string RulePath = "/api/v1/alerts/rules/test.state";

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(AdminPassword);

         RestPort = RestListener.Start(RestPort);

         _application.Reinitialize();

         (int status, string body) probe = Http("GET", "/api/v1/status");
         ClassicAssert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);
      }

      [TearDown]
      public void StopRestApi()
      {
         try
         {
            // RestApiAlerts' own test condition, left off as that fixture leaves it.
            Http("PUT", RulePath, "{\"enabled\":false,\"threshold\":0,\"cooldown_minutes\":0,\"webhook\":\"\",\"webhook_secret\":\"\",\"actions\":1}");
         }
         finally
         {
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      [Test]
      [Description("An alert rule's cool-down of 4294967301 minutes is refused as past a week, not taken as the 5 its low 32 bits make; a threshold of 4294967297 is kept as the largest int, not as 1.")]
      public void AnAlertRuleNumberPastAnIntIsNotWrapped()
      {
         (int status, string body) made = Http("PUT", RulePath, "{\"enabled\":false,\"digest\":false,\"actions\":1,\"cooldown_minutes\":0,\"threshold\":3}");
         Assert.IsTrue(made.status == 200 || made.status == 201, "The rule could not be written: " + made.body);

         (int status, string body) cooldown = Http("PUT", RulePath, "{\"cooldown_minutes\":4294967301}");
         Assert.AreEqual(400, cooldown.status, "A cool-down of 4294967301 minutes was taken: " + cooldown.body);
         StringAssert.Contains("cool-down", cooldown.body);

         (int status, string body) negative = Http("PUT", RulePath, "{\"cooldown_minutes\":-4294967291}");
         Assert.AreEqual(400, negative.status, "A cool-down of -4294967291 minutes was taken: " + negative.body);

         (int status, string body) exponent = Http("PUT", RulePath, "{\"cooldown_minutes\":1e20}");
         Assert.AreEqual(400, exponent.status, "A cool-down of 1e20 minutes was taken: " + exponent.body);

         (int status, string body) threshold = Http("PUT", RulePath, "{\"threshold\":4294967297}");
         Assert.AreEqual(200, threshold.status, threshold.body);
         StringAssert.Contains("\"threshold\":2147483647", threshold.body, "A threshold of 4294967297 was not held at the largest int: " + threshold.body);
         StringAssert.Contains("\"cooldown_minutes\":0", threshold.body, "A refused cool-down changed the rule: " + threshold.body);

         // What fits is what it was.
         (int status, string body) ordinary = Http("PUT", RulePath, "{\"threshold\":25,\"cooldown_minutes\":60}");
         Assert.AreEqual(200, ordinary.status, ordinary.body);
         StringAssert.Contains("\"threshold\":25", ordinary.body);
         StringAssert.Contains("\"cooldown_minutes\":60", ordinary.body);
      }

      private static (int status, string body) Http(string method, string path, string requestBody = null)
      {
         string credentials = Convert.ToBase64String(Encoding.ASCII.GetBytes("Administrator:" + AdminPassword));
         return RestRequest.Send("127.0.0.1", RestPort, method, path, "Basic " + credentials, requestBody);
      }
   }
}
