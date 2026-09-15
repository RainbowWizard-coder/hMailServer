// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
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
   ///    DKIM keys that rotate, over REST: GET /api/v1/domains/{domain}/dkim/rotation,
   ///    the five operations beneath it, POST /api/v1/dkim/rotation/run with a clock the
   ///    caller moves, and the policy as five keys of PUT /api/v1/domains/{domain}.
   ///
   ///    Every domain here is made over the API and starts with no key at all, so the
   ///    whole walk is the server's own - generated, published, seen, switched, kept
   ///    and retired - and nothing depends on a key file on the machine running the
   ///    tests, which is why the Linux suite links this fixture. The records are
   ///    served by the suite's DNS zone from the values the status document names.
   ///
   ///    The authorisation test is the one to read twice: a rotation route left out of
   ///    Authorize_'s domain list would be allowed for a key issued for any domain.
   /// </summary>
   [TestFixture]
   public class RestApiDkimRotation : TestFixtureBase
   {
      private static int RestPort = 9552;
      private const string AdminPassword = "testar";
      private const string DomainPrefix = "restrot-";

      private const int Hour = 3600;
      private const int Day = 86400;

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
            SuiteDns.Reset();

            hMailServer.Domains domains = _application.Domains;
            for (int i = domains.Count - 1; i >= 0; i--)
            {
               Domain domain = domains[i];
               if (domain.Name.StartsWith(DomainPrefix, StringComparison.OrdinalIgnoreCase))
                  domain.Delete();
            }
         }
         finally
         {
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      // ------------------------------------------------------------------ helpers

      private static string NewDomain()
      {
         string name = DomainPrefix + Guid.NewGuid().ToString("N").Substring(0, 8) + ".test";
         (int status, string body) = Http("POST", "/api/v1/domains", "{\"name\":\"" + name + "\",\"active\":true}");
         Assert.AreEqual(201, status, body);
         return name;
      }

      private static string RotationPath(string domain, string action = "")
      {
         return "/api/v1/domains/" + domain + "/dkim/rotation" + action;
      }

      private static Dictionary<string, object> Status(string domain)
      {
         (int status, string body) = Http("GET", RotationPath(domain));
         Assert.AreEqual(200, status, body);
         return MiniJson.Object(body);
      }

      private static Dictionary<string, object> Start(string domain, string selector)
      {
         (int status, string body) = Http("POST", RotationPath(domain, "/start"), "{\"selector\":\"" + selector + "\"}");
         Assert.AreEqual(201, status, body);
         return MiniJson.Object(body);
      }

      private static Dictionary<string, object> RunAhead(long secondsAhead)
      {
         (int status, string body) = Http("POST", "/api/v1/dkim/rotation/run", "{\"seconds_ahead\":" + secondsAhead + "}");
         Assert.AreEqual(200, status, body);
         return MiniJson.Object(body);
      }

      private static void Publish(Dictionary<string, object> status, string slot)
      {
         List<object> records = MiniJson.List(status, slot + ".records");
         Assert.IsTrue(records.Count > 0, "No record to publish for " + slot + ".");

         foreach (Dictionary<string, object> record in records.Cast<Dictionary<string, object>>())
            SuiteDns.Zone.WithTxt((string) record["host"], (string) record["value"]);
      }

      private static Dictionary<string, object> DomainEntry(string domain)
      {
         (int status, string body) = Http("GET", "/api/v1/domains");
         Assert.AreEqual(200, status, body);

         object parsed = MiniJson.Parse(body);
         IEnumerable<object> list = parsed as List<object> ??
                                    (IEnumerable<object>) MiniJson.List((Dictionary<string, object>) parsed, "domains");

         foreach (Dictionary<string, object> entry in list.Cast<Dictionary<string, object>>())
         {
            if (string.Equals((string) entry["name"], domain, StringComparison.OrdinalIgnoreCase))
               return entry;
         }

         Assert.Fail("The domain " + domain + " is not in the listing: " + body);
         return null;
      }

      // ------------------------------------------------------------------ the walk

      [Test]
      [Description("A rotation walks through over REST with the clock moved: awaiting_dns, propagating, switched; a second rotation keeps the first selector for the retention period and then retires it, and the domain listing follows every step.")]
      public void TheRotationWalksThroughOverRestWithTheClockMoved()
      {
         string domain = NewDomain();

         Dictionary<string, object> started = Start(domain, "walk-a");
         Assert.AreEqual("awaiting_dns", MiniJson.Text(started, "phase"));
         Assert.AreEqual("walk-a", MiniJson.Text(started, "next.selector"));
         Assert.AreEqual("walk-a", (string) DomainEntry(domain)["dkim_secondary_selector"]);

         Publish(started, "next");

         Dictionary<string, object> summary = RunAhead(0);
         Assert.IsTrue(MiniJson.Number(summary, "domains") >= 1, "The run did not look at the domain: " + string.Join(",", summary.Keys));
         Assert.AreEqual("propagating", MiniJson.Text(Status(domain), "phase"));

         RunAhead(72 * Hour + Hour);
         Dictionary<string, object> switched = Status(domain);
         Assert.AreEqual("walk-a", MiniJson.Text(switched, "current.selector"));
         Assert.IsNull(MiniJson.At(switched, "next"));
         Assert.AreEqual("walk-a", (string) DomainEntry(domain)["dkim_selector"], "The listing does not show the new selector.");
         Assert.AreEqual("", (string) DomainEntry(domain)["dkim_secondary_selector"]);

         Dictionary<string, object> second = Start(domain, "walk-b");
         Publish(second, "next");
         RunAhead(80 * Hour);
         RunAhead(153 * Hour);

         Dictionary<string, object> keeping = Status(domain);
         Assert.AreEqual("walk-b", MiniJson.Text(keeping, "current.selector"));
         Assert.AreEqual("walk-a", MiniJson.Text(keeping, "retiring.selector"));
         Assert.AreEqual("idle", MiniJson.Text(keeping, "phase"));
         Assert.AreEqual("keep", (string) MiniJson.List(keeping, "retiring.records").Cast<Dictionary<string, object>>().First()["action"]);

         RunAhead(153 * Hour + 10 * Day);
         Dictionary<string, object> retired = Status(domain);
         Assert.IsNull(MiniJson.At(retired, "retiring"));
         Assert.AreEqual("walk-a", MiniJson.Text(retired, "retired.selector"));
         Assert.AreEqual("off", MiniJson.Text(retired, "phase"));
         Assert.AreEqual("remove", (string) MiniJson.List(retired, "retired.records").Cast<Dictionary<string, object>>().First()["action"]);
      }

      // ------------------------------------------------------------------ authorisation

      [Test]
      [Description("A key issued for another domain is refused all six domain routes and the run; a read-only key may read the status and nothing else.")]
      public void RotationRoutesAreScopedToTheKeysDomains()
      {
         string domain = NewDomain();

         (string scopedId, string scopedKey) = CreateKey("restrot - scoped", "full", "example.test");
         (string readOnlyId, string readOnlyKey) = CreateKey("restrot - readonly", "readonly", null);

         try
         {
            var routes = new[]
            {
               ("GET", RotationPath(domain), (string) null),
               ("POST", RotationPath(domain, "/start"), "{}"),
               ("POST", RotationPath(domain, "/check"), "{}"),
               ("POST", RotationPath(domain, "/switch"), "{}"),
               ("POST", RotationPath(domain, "/retire"), "{}"),
               ("DELETE", RotationPath(domain, "/next"), (string) null),
               ("POST", "/api/v1/dkim/rotation/run", "{}"),
            };

            foreach (var route in routes)
            {
               (int status, string body) refused = Bearer(route.Item1, route.Item2, scopedKey, route.Item3);
               Assert.AreEqual(403, refused.status, "A key for another domain reached " + route.Item1 + " " + route.Item2 + ": " + refused.body);
            }

            Assert.IsNull(MiniJson.At(Status(domain), "next"), "A refused start staged a key.");

            // The key's own domain answers.
            Assert.AreEqual(200, Bearer("GET", RotationPath("example.test"), scopedKey).status);

            Assert.AreEqual(200, Bearer("GET", RotationPath(domain), readOnlyKey).status, "A read-only key may read the status.");
            foreach (var route in routes.Skip(1))
            {
               (int status, string body) refused = Bearer(route.Item1, route.Item2, readOnlyKey, route.Item3);
               Assert.AreEqual(403, refused.status, "A read-only key reached " + route.Item1 + " " + route.Item2 + ": " + refused.body);
            }

            Assert.IsNull(MiniJson.At(Status(domain), "next"), "A read-only key staged a key.");
         }
         finally
         {
            Http("DELETE", "/api/v1/apikeys/" + scopedId);
            Http("DELETE", "/api/v1/apikeys/" + readOnlyId);
         }
      }

      // ------------------------------------------------------------------ refusals

      [Test]
      [Description("Every operation refuses what it cannot do, with the status the API documents: a bad selector 400, a second start 409, a switch with nothing staged 409, before propagation 409 and with force 200, with force and no record 409, a clock moved backwards 400, an unknown domain 404.")]
      public void RotationRoutesRefuseWhatTheyCannotDo()
      {
         string domain = NewDomain();

         foreach (string bad in new[] { "../x", "a_b", "A/B" })
         {
            (int status, string body) refused = Http("POST", RotationPath(domain, "/start"), "{\"selector\":\"" + bad + "\"}");
            Assert.AreEqual(400, refused.status, bad + ": " + refused.body);
         }

         Assert.AreEqual(400, Http("POST", RotationPath(domain, "/start"), "{\"selector\":5}").status);
         Assert.AreEqual(400, Http("POST", RotationPath(domain, "/start"), "{\"size\":2048}").status, "An unknown field is refused by name.");

         (int nothingStatus, string nothingBody) = Http("POST", RotationPath(domain, "/switch"), "{}");
         Assert.AreEqual(409, nothingStatus, nothingBody);
         Assert.AreEqual("nothing_staged", MiniJson.Text(MiniJson.Object(nothingBody), "reason"));

         Assert.AreEqual(409, Http("POST", RotationPath(domain, "/retire"), "{}").status, "Nothing is retiring.");
         Assert.AreEqual(409, Http("DELETE", RotationPath(domain, "/next")).status, "Nothing is staged to cancel.");

         Dictionary<string, object> started = Start(domain, "refuse-a");
         Assert.AreEqual(409, Http("POST", RotationPath(domain, "/start"), "{\"selector\":\"refuse-b\"}").status, "A second start while one is staged.");

         Publish(started, "next");
         RunAhead(0);

         (int earlyStatus, string earlyBody) = Http("POST", RotationPath(domain, "/switch"), "{}");
         Assert.AreEqual(409, earlyStatus, earlyBody);
         Dictionary<string, object> early = MiniJson.Object(earlyBody);
         Assert.AreEqual("propagation", MiniJson.Text(early, "reason"));
         Assert.AreEqual("propagating", MiniJson.Text(early, "status.phase"), "A 409 carries the status document.");

         (int forcedStatus, string forcedBody) = Http("POST", RotationPath(domain, "/switch"), "{\"force\":true}");
         Assert.AreEqual(200, forcedStatus, forcedBody);
         Assert.AreEqual("refuse-a", MiniJson.Text(MiniJson.Object(forcedBody), "current.selector"));

         // Force skips the period and never the check.
         Start(domain, "refuse-c");
         (int blindStatus, string blindBody) = Http("POST", RotationPath(domain, "/switch"), "{\"force\":true}");
         Assert.AreEqual(409, blindStatus, blindBody);
         Assert.AreEqual("record_not_found", MiniJson.Text(MiniJson.Object(blindBody), "reason"));
         Assert.AreEqual("refuse-a", (string) DomainEntry(domain)["dkim_selector"], "A forced switch moved signing to a key whose record was never published.");

         Assert.AreEqual(200, Http("DELETE", RotationPath(domain, "/next")).status);
         Assert.AreEqual("", (string) DomainEntry(domain)["dkim_secondary_selector"]);

         Assert.AreEqual(400, Http("POST", "/api/v1/dkim/rotation/run", "{\"seconds_ahead\":-1}").status);
         Assert.AreEqual(400, Http("POST", "/api/v1/dkim/rotation/run", "{\"seconds_ahead\":\"soon\"}").status);
         Assert.AreEqual(400, Http("POST", "/api/v1/dkim/rotation/run", "{\"seconds_ahead\":316224001}").status);

         Assert.AreEqual(404, Http("GET", RotationPath("no-such-domain.test")).status);
         Assert.AreEqual(404, Http("POST", RotationPath("no-such-domain.test", "/start"), "{}").status);
      }

      // ------------------------------------------------------------------ the policy

      [Test]
      [Description("The policy is written through PUT /api/v1/domains/{domain} and shown by the listing; a value out of range is a 400 naming the key and changes nothing.")]
      public void ThePolicyIsWrittenThroughTheDomainAndRefusedOutOfRange()
      {
         string domain = NewDomain();

         Dictionary<string, object> shipped = DomainEntry(domain);
         Assert.AreEqual(false, shipped["dkim_rotation_enabled"]);
         Assert.AreEqual(180.0, shipped["dkim_rotation_interval_days"]);
         Assert.AreEqual(72.0, shipped["dkim_rotation_propagation_hours"]);
         Assert.AreEqual(7.0, shipped["dkim_rotation_retention_days"]);
         Assert.AreEqual(2048.0, shipped["dkim_rotation_key_bits"]);

         (int status, string body) = Http("PUT", "/api/v1/domains/" + domain,
            "{\"active\":true,\"dkim_rotation_interval_days\":90,\"dkim_rotation_propagation_hours\":48," +
            "\"dkim_rotation_retention_days\":14,\"dkim_rotation_key_bits\":3072}");
         Assert.AreEqual(200, status, body);

         Dictionary<string, object> written = DomainEntry(domain);
         Assert.AreEqual(90.0, written["dkim_rotation_interval_days"]);
         Assert.AreEqual(48.0, written["dkim_rotation_propagation_hours"]);
         Assert.AreEqual(14.0, written["dkim_rotation_retention_days"]);
         Assert.AreEqual(3072.0, written["dkim_rotation_key_bits"]);

         Dictionary<string, object> policy = Status(domain);
         Assert.AreEqual(90, MiniJson.Number(policy, "policy.interval_days"), "The status reads the policy the domain holds.");

         foreach (string refused in new[]
                  {
                     "\"dkim_rotation_interval_days\":0", "\"dkim_rotation_propagation_hours\":721",
                     "\"dkim_rotation_retention_days\":0", "\"dkim_rotation_key_bits\":1024", "\"dkim_rotation_key_bits\":3000",
                     "\"dkim_rotation_enabled\":\"yes\""
                  })
         {
            (int refusedStatus, string refusedBody) = Http("PUT", "/api/v1/domains/" + domain, "{\"active\":true," + refused + "}");
            Assert.AreEqual(400, refusedStatus, refused + ": " + refusedBody);
            string key = refused.Split(':')[0].Trim('"');
            StringAssert.Contains(key, refusedBody, "The refusal names the key.");
         }

         Dictionary<string, object> unchanged = DomainEntry(domain);
         Assert.AreEqual(90.0, unchanged["dkim_rotation_interval_days"]);
         Assert.AreEqual(48.0, unchanged["dkim_rotation_propagation_hours"]);
         Assert.AreEqual(14.0, unchanged["dkim_rotation_retention_days"]);
         Assert.AreEqual(3072.0, unchanged["dkim_rotation_key_bits"]);
         Assert.AreEqual(false, unchanged["dkim_rotation_enabled"]);
      }

      [Test]
      [Description("A PUT that replays a selector from before the last switch - an old export applied again - is refused with 409 while the rotation manages the domain, and the selector the rotation chose stands; the negative control is the same PUT on a domain no rotation manages.")]
      public void AStaleConfigurationApplyIsRefusedNotSilentlyReverted()
      {
         string domain = NewDomain();

         Dictionary<string, object> first = Start(domain, "stale-a");
         Publish(first, "next");
         RunAhead(0);
         RunAhead(73 * Hour);

         Dictionary<string, object> second = Start(domain, "stale-b");
         Publish(second, "next");
         RunAhead(80 * Hour);
         RunAhead(153 * Hour);
         Assert.AreEqual("stale-b", (string) DomainEntry(domain)["dkim_selector"]);

         (int status, string body) = Http("PUT", "/api/v1/domains/" + domain, "{\"active\":true,\"dkim_selector\":\"stale-a\"}");
         Assert.AreEqual(409, status, body);
         StringAssert.Contains("managed by its key rotation", body);
         Assert.AreEqual("stale-b", (string) DomainEntry(domain)["dkim_selector"], "A stale apply put the old selector back.");

         // An unrelated change to the same domain is fine.
         Assert.AreEqual(200, Http("PUT", "/api/v1/domains/" + domain, "{\"active\":true,\"postmaster\":\"postmaster@" + domain + "\"}").status);

         // The control: a domain no rotation manages takes the same kind of PUT.
         string unmanaged = NewDomain();
         (int freeStatus, string freeBody) = Http("PUT", "/api/v1/domains/" + unmanaged, "{\"active\":true,\"dkim_selector\":\"stale-a\"}");
         Assert.AreEqual(200, freeStatus, freeBody);
      }

      // ------------------------------------------------------------------ the document, the alert, the trail

      [Test]
      [Description("openapi.json is JSON and describes the seven rotation routes and the five policy keys.")]
      public void TheOpenApiDocumentDescribesTheRotationRoutes()
      {
         (int status, string body) = Http("GET", "/api/v1/openapi.json");
         Assert.AreEqual(200, status);

         Dictionary<string, object> document = MiniJson.Object(body);
         var paths = (Dictionary<string, object>) document["paths"];

         foreach (string path in new[]
                  {
                     "/api/v1/domains/{domain}/dkim/rotation", "/api/v1/domains/{domain}/dkim/rotation/start",
                     "/api/v1/domains/{domain}/dkim/rotation/check", "/api/v1/domains/{domain}/dkim/rotation/switch",
                     "/api/v1/domains/{domain}/dkim/rotation/retire", "/api/v1/domains/{domain}/dkim/rotation/next",
                     "/api/v1/dkim/rotation/run"
                  })
         {
            Assert.IsTrue(paths.ContainsKey(path), "openapi.json does not describe " + path);
         }

         foreach (string key in new[] { "dkim_rotation_enabled", "dkim_rotation_interval_days", "dkim_rotation_propagation_hours",
                                        "dkim_rotation_retention_days", "dkim_rotation_key_bits" })
            StringAssert.Contains(key, body);
      }

      [Test]
      [Description("A next key whose record has not been seen for the rule's threshold raises dkim.rotation.stalled, and publishing the record clears it.")]
      public void AStalledRotationRaisesItsAlertAndClearsWhenTheRecordAppears()
      {
         // Seeded by the schema step, so 200; a database that somehow lacks the row
         // gets it created, which is 201. Either way the rule is on at seven days.
         int ruleStatus = Http("PUT", "/api/v1/alerts/rules/dkim.rotation.stalled", "{\"enabled\":true,\"threshold\":7}").status;
         Assert.IsTrue(ruleStatus == 200 || ruleStatus == 201, "The dkim.rotation.stalled rule could not be written: " + ruleStatus);
         Http("POST", "/api/v1/alerts/run", "{}");

         string domain = NewDomain();
         Dictionary<string, object> started = Start(domain, "stalled");

         RunAhead(6 * Day);
         Http("POST", "/api/v1/alerts/run", "{}");
         Assert.AreNotEqual("raised", NewestStalledState(), "Raised before the threshold.");

         RunAhead(8 * Day);
         Http("POST", "/api/v1/alerts/run", "{}");
         Assert.AreEqual("raised", NewestStalledState(), "A next key unseen for eight days did not raise the alert.");

         Publish(started, "next");
         RunAhead(8 * Day + Hour);
         Http("POST", "/api/v1/alerts/run", "{}");
         Assert.AreEqual("cleared", NewestStalledState(), "Publishing the record did not clear the alert.");
      }

      private static string NewestStalledState()
      {
         (int status, string body) = Http("GET", "/api/v1/alerts/events?limit=500");
         Assert.AreEqual(200, status, body);

         foreach (Dictionary<string, object> entry in MiniJson.List(MiniJson.Object(body), "events").Cast<Dictionary<string, object>>())
         {
            if ((string) entry["condition"] == "dkim.rotation.stalled")
               return (string) entry["state"];
         }

         return null;
      }

      [Test]
      [Description("A switch moves the domain's selectors through a change the audit trail records, with the key file columns withheld as (changed).")]
      public void ARotationSwitchIsRecordedInTheAuditTrail()
      {
         string domain = NewDomain();

         Dictionary<string, object> started = Start(domain, "audited");
         Publish(started, "next");
         RunAhead(0);
         RunAhead(73 * Hour);
         Assert.AreEqual("audited", (string) DomainEntry(domain)["dkim_selector"]);

         (int status, string body) = Http("GET", "/api/v1/audit?object_type=domain&limit=200");
         Assert.AreEqual(200, status, body);

         var rows = MiniJson.List(MiniJson.Object(body), "entries").Cast<Dictionary<string, object>>()
                            .Where(row => row.Values.OfType<string>().Any(v => v.Contains("domaindkimselector") && v.Contains("audited")))
                            .ToList();

         Assert.IsTrue(rows.Count > 0, "No audit row records the switch: " + body);
         string detail = string.Join(" ", rows[0].Values.OfType<string>());
         StringAssert.Contains("(changed)", detail, "The key file columns must be withheld.");
         StringAssert.DoesNotContain(".pem", detail, "A key file path reached the audit trail.");
      }

      // ------------------------------------------------------------------ HTTP

      private static (string id, string key) CreateKey(string label, string scope, string domains)
      {
         string body = "{\"label\":\"" + label + "\",\"scope\":\"" + scope + "\"" +
                       (domains == null ? "" : ",\"domains\":\"" + domains + "\"") + "}";
         (int status, string created) = Http("POST", "/api/v1/apikeys", body);
         Assert.AreEqual(201, status, "POST /api/v1/apikeys must create a key. Body: " + created);
         return (Extract(created, "id"), Extract(created, "key"));
      }

      private static string Extract(string json, string key)
      {
         Match match = Regex.Match(json, "\"" + key + "\"\\s*:\\s*\"([^\"]*)\"");
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
