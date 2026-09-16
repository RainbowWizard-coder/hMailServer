// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Net;
using System.Security.Cryptography;
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
   ///    Outbound webhooks: five things that happen to mail, each reaching a
   ///    subscribed endpoint as a signed POST.
   ///
   ///    What is held here is what a receiver's author has to be able to rely on.
   ///    The signature is recomputed from the two headers and the raw body exactly
   ///    as a receiver would recompute it, and a DIFFERENT secret is shown to
   ///    produce a different one - a signature that cannot fail is not a
   ///    signature. A subscription confined to one domain is shown to hear
   ///    nothing from another, because a scope that quietly does not apply is
   ///    worse than no scope. An endpoint that is not there is retried and then
   ///    dead-lettered with its body kept, rather than retried for ever. The
   ///    ceiling is shown to DROP rather than queue, which is the whole of "a
   ///    busy server cannot be turned into a flood". And the queue is shown to
   ///    survive a real restart of the service: the endpoint is not listening
   ///    when the events are raised, the service is restarted, and the endpoint
   ///    is then brought up on the port it was always going to be on and gets
   ///    what it missed.
   ///
   ///    One test sends a real message over SMTP rather than using the test
   ///    route, because the route proves the plumbing and only a real message
   ///    proves that the acceptance path raises anything at all.
   ///
   ///    No test asserts on the number POST /run answers. The scheduled pass runs
   ///    once a minute on its own thread, one pass runs at a time, and a /run that
   ///    waited for the scheduled pass finds what it left - so "delivered" is
   ///    whichever pass got there first. What is asserted is what a receiver can
   ///    see: the requests the endpoint got and the states the queue is in.
   /// </summary>
   [TestFixture]
   public class RestApiWebhooks : TestFixtureBase
   {
      private static int RestPort = 9138;

      // A port of this fixture's own, so that the address a subscription stores -
      // and the address nothing is listening on while the restart test runs - is
      // known before any endpoint exists.
      private const int EndpointPort = 9590;

      private const string AdminPassword = "testar";
      private const string WebhooksPath = "/api/v1/webhooks";
      private const string Secret = "a-shared-secret-nobody-else-has";
      private const string OtherSecret = "not-the-secret-it-was-signed-with";

      private static string EndpointUrl
      {
         get { return "http://127.0.0.1:" + EndpointPort + "/hook"; }
      }

      private bool _changed;

      [SetUp]
      public void StartRestApi()
      {
         _changed = false;
         _domainName = _domain.Name;

         if (TestPorts.Host != "127.0.0.1" && !string.Equals(TestPorts.Host, "localhost", StringComparison.OrdinalIgnoreCase))
            Assert.Ignore("The fake endpoint listens on this machine's loopback address, and the server under test is on " + TestPorts.Host + ".");

         _changed = true;

         _settings.SetAdministratorPassword(AdminPassword);

         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();

         (int status, string body) probe = Http("GET", "/api/v1/status");
         Assert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);

         // Removing a subscription takes its queue with it through the foreign
         // key, so this is the whole of the clean-up - and it is done over the
         // API rather than in SQL, so the fixture runs against a server that is
         // not this machine.
         RemoveEverySubscription();

         Http("PUT", "/api/v1/settings",
            "{\"webhooks_enabled\":true,\"webhook_max_attempts\":5,\"webhook_max_per_minute\":60,\"webhook_retention_days\":7}");
      }

      [TearDown]
      public void StopRestApi()
      {
         if (!_changed)
            return;

         RunEach(
            RemoveEverySubscription,
            () => Http("PUT", "/api/v1/settings",
               "{\"webhooks_enabled\":true,\"webhook_max_attempts\":5,\"webhook_max_per_minute\":60,\"webhook_retention_days\":7}"),
            () => SetDomainWebhooks(true, false),
            RestListener.Stop,
            () => _application.Reinitialize());
      }

      [Test]
      [Description("The five events the roadmap names are the five the server offers, so a client can draw the choice rather than hard-coding it.")]
      public void TheFiveEventsAreOffered()
      {
         string listing = Read(WebhooksPath);

         foreach (string name in new[] { "message.received", "message.delivered", "message.bounced",
                                         "message.quarantined", "account.locked" })
            StringAssert.Contains("\"" + name + "\"", listing, listing);
      }

      [Test]
      [Description("A subscription is refused without an endpoint, without a secret, and with an endpoint this server would reach in the clear across a network.")]
      public void ASubscriptionNeedsAnEndpointAndASecret()
      {
         (int status, string body) noUrl = Http("POST", WebhooksPath, "{\"name\":\"suite\",\"url\":\"\",\"secret\":\"" + Secret + "\"}");
         Assert.AreEqual(400, noUrl.status, noUrl.body);
         StringAssert.Contains("endpoint", noUrl.body);

         (int status, string body) noSecret = Http("POST", WebhooksPath,
            "{\"name\":\"suite\",\"url\":\"" + EndpointUrl + "\",\"secret\":\"\"}");
         Assert.AreEqual(400, noSecret.status, noSecret.body);
         StringAssert.Contains("secret", noSecret.body);

         (int status, string body) clear = Http("POST", WebhooksPath,
            "{\"name\":\"suite\",\"url\":\"http://receiver.example/hook\",\"secret\":\"" + Secret + "\"}");
         Assert.AreEqual(400, clear.status, clear.body);
         StringAssert.Contains("https", clear.body,
            "A body that says who wrote to whom must not go across a network in the clear.");

         (int status, string body) badName = Http("POST", WebhooksPath,
            "{\"name\":\"not a name\",\"url\":\"" + EndpointUrl + "\",\"secret\":\"" + Secret + "\"}");
         Assert.AreEqual(400, badName.status, badName.body);
      }

      [Test]
      [Description("A shared secret is never handed back; the answer says only whether one is set, and an empty one in a change keeps the one that is there.")]
      public void ASecretIsNeverHandedBack()
      {
         Create("suite");

         string listing = Read(WebhooksPath);
         StringAssert.DoesNotContain(Secret, listing, "A reader of the page must not be able to take away what they could sign with.");
         StringAssert.Contains("\"secret_set\":true", listing, listing);

         (int status, string body) unchanged = Http("PUT", WebhooksPath + "/suite", "{\"enabled\":false,\"secret\":\"\"}");
         Assert.AreEqual(200, unchanged.status, unchanged.body);
         StringAssert.Contains("\"secret_set\":true", unchanged.body,
            "Switching a subscription off must not clear what it signs with.");
      }

      [Test]
      [Description("Every one of the five events arrives with a timestamp and an HMAC-SHA256 signature over 'timestamp.body' that a receiver can recompute - and a different secret produces a different signature.")]
      public void EachEventArrivesSignedAndAWrongSecretIsDetectable()
      {
         using (var endpoint = new FakeHttpEndpoint(IPAddress.Loopback, EndpointPort, 200, "{}", "application/json"))
         {
            Create("suite");

            string[] events =
            {
               "message.received", "message.delivered", "message.bounced",
               "message.quarantined", "account.locked"
            };

            foreach (string name in events)
               Assert.AreEqual("1", Extract(RaiseTest(name, _domainName), "queued"), name + " was queued for nobody.");

            Run();

            List<string> requests = WaitForRequests(endpoint, events.Length);
            Assert.AreEqual(events.Length, requests.Count, "The endpoint was called " + requests.Count + " time(s).");

            string queue = Read(WebhooksPath + "/deliveries?limit=50");
            Assert.AreEqual(events.Length, Count(queue, "\"state\":\"delivered\""),
               "Every event that was queued should be recorded as delivered, whichever pass delivered it: " + queue);

            for (int i = 0; i < events.Length; i++)
            {
               string request = requests[i];
               string timestamp = Header(request, "X-hMailServer-Timestamp");
               string signature = Header(request, "X-hMailServer-Signature");
               string sent = BodyOf(request);

               Assert.IsNotEmpty(timestamp, "A signature with no timestamp can be replayed for ever.");
               StringAssert.StartsWith("sha256=", signature, signature);

               Assert.AreEqual("sha256=" + Hmac(Secret, timestamp + "." + sent), signature,
                  "The recipe in docs/Webhooks.md is HMAC-SHA256 over the timestamp, a full stop, and the raw body.");

               Assert.AreNotEqual("sha256=" + Hmac(OtherSecret, timestamp + "." + sent), signature,
                  "A signature that any secret reproduces is not a signature.");

               StringAssert.Contains("\"event\":\"" + events[i] + "\"", sent, sent);
               StringAssert.Contains("X-hMailServer-Event: " + events[i], request, request);
               StringAssert.Contains("X-hMailServer-Delivery: ", request, request);
               StringAssert.Contains("X-hMailServer-Attempt: 1", request, request);
               StringAssert.Contains("Content-Type: application/json", request, request);
               StringAssert.Contains("\"subscription\":\"suite\"", sent, sent);
               StringAssert.Contains("\"server\":\"", sent, sent);
            }
         }
      }

      [Test]
      [Description("A body carries the envelope and never the message: no body, no headers, and no subject unless the domain and the subscription both allow one.")]
      public void ABodyCarriesNoSubjectUnlessTheDomainAndTheSubscriptionAgree()
      {
         using (var endpoint = new FakeHttpEndpoint(IPAddress.Loopback, EndpointPort, 200, "{}", "application/json"))
         {
            // The subscription asks for subjects; the domain has not agreed.
            Create("suite", "\"include_subject\":true");

            RaiseTest("message.received", _domainName, "\"subject\":\"The quarterly figures\"");
            Run();

            List<string> first = WaitForRequests(endpoint, 1);
            StringAssert.DoesNotContain("quarterly", BodyOf(first[0]),
               "A subject line is what a message is about; the domain has not said it may travel.");

            SetDomainWebhooks(true, true);

            RaiseTest("message.received", _domainName, "\"subject\":\"The quarterly figures\"");
            Run();

            List<string> second = WaitForRequests(endpoint, 2);
            StringAssert.Contains("The quarterly figures", BodyOf(second[1]),
               "With both switches on the subject travels.");
         }
      }

      [Test]
      [Description("A subscription confined to one domain hears nothing about another, because a scope that quietly does not apply is worse than no scope.")]
      public void ASubscriptionConfinedToOneDomainHearsNothingFromAnother()
      {
         Create("suite", "\"domains\":\"" + _domainName + "\"");

         Assert.AreEqual("1", Extract(RaiseTest("message.received", _domainName), "queued"));
         Assert.AreEqual("0", Extract(RaiseTest("message.received", "somewhere-else.example"), "queued"),
            "The subscription names one domain, and the event was about another.");

         string queue = Read(WebhooksPath + "/deliveries?limit=50");
         Assert.AreEqual(1, Count(queue, "\"event\":\"message.received\""), queue);
      }

      [Test]
      [Description("An event nothing subscribes to is not an error: the answer says it was queued for nobody rather than pretending it was sent.")]
      public void AnEventNobodyWantsIsQueuedForNobody()
      {
         Create("suite", "\"events\":\"message.bounced\"");

         Assert.AreEqual("0", Extract(RaiseTest("account.locked", _domainName), "queued"));
         Assert.AreEqual("1", Extract(RaiseTest("message.bounced", _domainName), "queued"));
      }

      [Test]
      [Description("A domain with webhooks switched off raises nothing, whatever the subscriptions ask for.")]
      public void ADomainCanTakeItselfOutOfIt()
      {
         Create("suite");

         SetDomainWebhooks(false, false);

         Assert.AreEqual("0", Extract(RaiseTest("message.received", _domainName), "queued"),
            "The domain's own switch is off, so nothing about it is raised.");

         SetDomainWebhooks(true, false);

         Assert.AreEqual("1", Extract(RaiseTest("message.received", _domainName), "queued"));
      }

      [Test]
      [Description("Past its ceiling a subscription's events are dropped and counted rather than queued, so a busy hour cannot become a flood an hour later.")]
      public void TheCeilingDropsRatherThanQueues()
      {
         Create("suite", "\"max_per_minute\":1");

         // The ceiling is counted per wall-clock minute (epoch / 60), so three
         // raises that straddle :59 to :00 would be two minutes' worth and the
         // second would be queued with nothing wrong. Wait for a fresh minute
         // when this one is nearly over.
         if (DateTime.UtcNow.Second > 55)
            Thread.Sleep(TimeSpan.FromSeconds(61 - DateTime.UtcNow.Second));

         Assert.AreEqual("1", Extract(RaiseTest("message.received", _domainName), "queued"));
         Assert.AreEqual("0", Extract(RaiseTest("message.received", _domainName), "queued"),
            "The second event in the same minute is past the ceiling of one.");
         Assert.AreEqual("0", Extract(RaiseTest("message.received", _domainName), "queued"));

         string listing = Read(WebhooksPath + "/suite");
         Assert.AreEqual(2, Extract(listing, "dropped").ToInt(), listing);
         Assert.AreEqual(1, Extract(listing, "pending").ToInt(), listing);
      }

      [Test]
      [Description("An endpoint that is not answering is retried and then dead-lettered, with the body and the reason kept rather than lost.")]
      public void AFailingEndpointIsRetriedAndThenDeadLettered()
      {
         Http("PUT", "/api/v1/settings", "{\"webhook_max_attempts\":2}");

         Create("suite");
         RaiseTest("message.bounced", _domainName);

         Run();

         string afterOne = Read(WebhooksPath + "/deliveries?limit=50");
         StringAssert.Contains("\"state\":\"pending\"", afterOne, "One failure is a retry, not a verdict: " + afterOne);
         StringAssert.Contains("\"attempts\":1", afterOne, "Nothing is listening on that port, so the one attempt failed: " + afterOne);

         Run();

         string queue = Read(WebhooksPath + "/deliveries?limit=50");
         StringAssert.Contains("\"state\":\"dead-lettered\"", queue, queue);
         StringAssert.Contains("\"attempts\":2", queue, queue);
         StringAssert.Contains("message.bounced", queue,
            "The body is kept, so an administrator can see what the receiver would have got.");

         // The administrator is told once, in the error log, that an endpoint of
         // theirs is not answering - which is the point, and which would otherwise
         // fail the next test's SetUp as an error nobody expected.
         CustomAsserts.AssertReportedError("HM6971", "dead-lettered");
      }

      [Test]
      [Description("The queue survives a restart of the service: events raised while nothing was listening are delivered after the server has been stopped and started.")]
      public void TheQueueSurvivesARestart()
      {
         Create("suite");

         RaiseTest("message.received", _domainName);
         RaiseTest("message.bounced", _domainName);

         // One pass with nothing listening. The first row gets a failure recorded
         // against it; the second is left untouched for the next pass, because an
         // endpoint that gave no answer at all is not tried again in the same pass.
         // Both are still pending, and both are what the restart must carry over.
         Run();

         // A real stop and start of the service on the Windows bench. Against the
         // Linux server the shim makes this a reinitialise, which proves less -
         // the process is the same one - but still proves that the rows are read
         // from the database rather than held in memory by whatever raised them.
         RestartServerAndReacquireCom();

         (int status, string body) probe = Http("GET", "/api/v1/status");
         Assert.AreEqual(200, probe.status, "The REST listener did not come back after the restart. Body: " + probe.body);

         string queue = Read(WebhooksPath + "/deliveries?limit=50");
         Assert.AreEqual(2, Count(queue, "\"state\":\"pending\""), queue);

         using (var endpoint = new FakeHttpEndpoint(IPAddress.Loopback, EndpointPort, 200, "{}", "application/json"))
         {
            Run();

            List<string> requests = WaitForRequests(endpoint, 2);
            Assert.AreEqual(2, requests.Count, "What was owed before the restart is owed after it.");
            StringAssert.Contains("message.received", BodyOf(requests[0]) + BodyOf(requests[1]));
            StringAssert.Contains("message.bounced", BodyOf(requests[0]) + BodyOf(requests[1]));

            string delivered = Read(WebhooksPath + "/deliveries?limit=50");
            Assert.AreEqual(2, Count(delivered, "\"state\":\"delivered\""), delivered);
         }
      }

      [Test]
      [Description("A message accepted over SMTP raises message.received from the path that owns it, with the envelope and the queue id and no subject.")]
      public void AMessageAcceptedOverSmtpRaisesTheEvent()
      {
         using (var endpoint = new FakeHttpEndpoint(IPAddress.Loopback, EndpointPort, 200, "{}", "application/json"))
         {
            string address = "hookwatch@" + _domainName;
            Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, address, "secret-password");

            Create("suite", "\"events\":\"message.received\"");

            SmtpClientSimulator.StaticSend("outsider@example.net", address, "A message for the hook", "The body nobody sees.");

            Pop3ClientSimulator.AssertMessageCount(address, "secret-password", 1);

            Run();

            List<string> requests = WaitForRequests(endpoint, 1);
            string sent = BodyOf(requests[0]);

            StringAssert.Contains("\"event\":\"message.received\"", sent, sent);
            StringAssert.Contains("\"sender\":\"outsider@example.net\"", sent, sent);
            StringAssert.Contains("\"recipient\":\"" + address + "\"", sent, sent);
            StringAssert.Contains("\"domain\":\"" + _domainName + "\"", sent, sent);
            StringAssert.Contains("\"queue_id\":", sent, sent);
            StringAssert.DoesNotContain("A message for the hook", sent,
               "The subject travels only where the domain and the subscription both allow it.");
            StringAssert.DoesNotContain("The body nobody sees", sent,
               "A webhook is a notification, not a copy of the message.");

            account.Delete();
         }
      }

      [Test]
      [Description("A setting outside its range is refused over REST with the sentence COM uses, not clamped by the setter without a word.")]
      public void ARangeIsRefusedNotClamped()
      {
         (int status, string body) attempts = Http("PUT", "/api/v1/settings", "{\"webhook_max_attempts\":25}");
         Assert.AreEqual(400, attempts.status, attempts.body);
         StringAssert.Contains("between 1 and 20", attempts.body, attempts.body);

         (int status, string body) perMinute = Http("PUT", "/api/v1/settings", "{\"webhook_max_per_minute\":0}");
         Assert.AreEqual(400, perMinute.status, perMinute.body);
         StringAssert.Contains("between 1 and 100000", perMinute.body, perMinute.body);

         (int status, string body) retention = Http("PUT", "/api/v1/settings", "{\"webhook_retention_days\":-1}");
         Assert.AreEqual(400, retention.status, retention.body);
         StringAssert.Contains("3650", retention.body, retention.body);

         string settings = Read("/api/v1/settings");
         StringAssert.Contains("\"webhook_max_attempts\":5", settings, "A refused change changes nothing: " + settings);
      }

      [Test]
      [Description("Deleting a subscription takes its queue with it, because a delivery to an endpoint that no longer exists is a request nobody wants made.")]
      public void DeletingASubscriptionTakesItsQueueWithIt()
      {
         Create("suite");
         RaiseTest("message.received", _domainName);

         StringAssert.Contains("\"webhook\":\"suite\"", Read(WebhooksPath + "/deliveries?limit=50"));

         (int status, string body) removed = Http("DELETE", WebhooksPath + "/suite");
         Assert.AreEqual(200, removed.status, removed.body);

         string queue = Read(WebhooksPath + "/deliveries?limit=50");
         StringAssert.DoesNotContain("\"webhook\":\"suite\"", queue, queue);

         Assert.AreEqual(404, Http("GET", WebhooksPath + "/suite").status);
         Assert.AreEqual(404, Http("DELETE", WebhooksPath + "/suite").status);
      }

      [Test]
      [Description("Every route is refused to an API key of any scope, with a 401, because a subscription holds a signing secret and the address this server will POST to.")]
      public void NoApiKeyReachesThem()
      {
         string key = CreateApiKey();

         foreach (string path in new[] { WebhooksPath, WebhooksPath + "/deliveries" })
         {
            (int status, string body) answer = RestRequest.Send("127.0.0.1", RestPort, "GET", path, "Bearer " + key, null);
            Assert.AreEqual(401, answer.status, path + " answered " + answer.status + ": " + answer.body);
         }

         (int status, string body) create = RestRequest.Send("127.0.0.1", RestPort, "POST", WebhooksPath, "Bearer " + key,
            "{\"name\":\"key\",\"url\":\"" + EndpointUrl + "\",\"secret\":\"" + Secret + "\"}");
         Assert.AreEqual(401, create.status, create.body);
      }

      // ----------------------------------------------------------------- helpers

      private string CreateApiKey()
      {
         (int status, string body) answer = Http("POST", "/api/v1/apikeys", "{\"label\":\"webhook-suite\"}");
         Assert.AreEqual(201, answer.status, answer.body);

         Match match = Regex.Match(answer.body, "\"key\"\\s*:\\s*\"([^\"]+)\"");
         Assert.IsTrue(match.Success, "No key in: " + answer.body);

         return match.Groups[1].Value;
      }

      private void Create(string name, string extra = null)
      {
         string body = "{\"name\":\"" + name + "\",\"url\":\"" + EndpointUrl + "\",\"secret\":\"" + Secret + "\"" +
                       (string.IsNullOrEmpty(extra) ? "" : "," + extra) + "}";

         (int status, string body) answer = Http("POST", WebhooksPath, body);
         Assert.AreEqual(201, answer.status, answer.body);
      }

      // The domain's name rather than the Domain object: TheQueueSurvivesARestart
      // restarts the service, after which the COM proxy the fixture was given
      // in SetUp answers nothing, and TearDown still has to put the domain back.
      private string _domainName;

      private void RemoveEverySubscription()
      {
         (int status, string body) listing = Http("GET", WebhooksPath);
         if (listing.status != 200)
            return;

         foreach (Match match in Regex.Matches(listing.body, "\"name\":\"([^\"]+)\""))
            Http("DELETE", WebhooksPath + "/" + match.Groups[1].Value);
      }

      private void SetDomainWebhooks(bool enabled, bool subjects)
      {
         (int status, string body) answer = Http("PUT", "/api/v1/domains/" + _domainName,
            "{\"active\":true,\"webhooks_enabled\":" + (enabled ? "true" : "false") +
            ",\"webhook_subjects\":" + (subjects ? "true" : "false") + "}");
         Assert.AreEqual(200, answer.status, answer.body);
      }

      private string RaiseTest(string name, string domain, string extra = null)
      {
         string body = "{\"event\":\"" + name + "\",\"domain\":\"" + domain + "\",\"account\":\"someone@" + domain + "\"" +
                       (string.IsNullOrEmpty(extra) ? "" : "," + extra) + "}";

         (int status, string body) answer = Http("POST", WebhooksPath + "/test", body);
         Assert.AreEqual(200, answer.status, answer.body);
         return answer.body;
      }

      private string Run()
      {
         (int status, string body) answer = Http("POST", WebhooksPath + "/run", "{}");
         Assert.AreEqual(200, answer.status, answer.body);
         return answer.body;
      }

      private static string Read(string path)
      {
         (int status, string body) answer = Http("GET", path);
         Assert.AreEqual(200, answer.status, answer.body);
         return answer.body;
      }

      private static (int status, string body) Http(string method, string path, string requestBody = null)
      {
         string credentials = Convert.ToBase64String(Encoding.ASCII.GetBytes("Administrator:" + AdminPassword));
         return RestRequest.Send("127.0.0.1", RestPort, method, path, "Basic " + credentials, requestBody);
      }

      /// <summary>Runs every step, and then throws the first exception any of them threw.</summary>
      private static void RunEach(params Action[] steps)
      {
         Exception first = null;
         foreach (Action step in steps)
         {
            try
            {
               step();
            }
            catch (Exception ex)
            {
               if (first == null)
                  first = ex;
            }
         }

         if (first != null)
            throw first;
      }

      // FakeHttpEndpoint keeps each request as its raw text - request line, headers
      // and body - which is exactly what a real receiver has to work from when it
      // verifies a signature.
      private static List<string> WaitForRequests(FakeHttpEndpoint endpoint, int wanted)
      {
         for (int attempt = 0; attempt < 100; attempt++)
         {
            List<string> requests = endpoint.Requests;
            if (requests.Count >= wanted)
               return requests;

            Thread.Sleep(100);
         }

         return endpoint.Requests;
      }

      private static string Header(string request, string name)
      {
         foreach (string line in request.Split(new[] { "\r\n" }, StringSplitOptions.None))
         {
            if (line.StartsWith(name + ":", StringComparison.OrdinalIgnoreCase))
               return line.Substring(name.Length + 1).Trim();
         }

         return "";
      }

      private static string BodyOf(string request)
      {
         int at = request.IndexOf("\r\n\r\n", StringComparison.Ordinal);
         return at >= 0 ? request.Substring(at + 4) : "";
      }

      private static string Extract(string json, string key)
      {
         Match match = Regex.Match(json, "\"" + key + "\"\\s*:\\s*\"?([^\",}]*)\"?");
         Assert.IsTrue(match.Success, "No '" + key + "' in: " + json);
         return match.Groups[1].Value;
      }

      private static int Count(string text, string needle)
      {
         int count = 0;
         int at = 0;
         while ((at = text.IndexOf(needle, at, StringComparison.Ordinal)) >= 0)
         {
            count++;
            at += needle.Length;
         }

         return count;
      }

      private static string Hmac(string key, string data)
      {
         using (var hmac = new HMACSHA256(Encoding.UTF8.GetBytes(key)))
         {
            byte[] hash = hmac.ComputeHash(Encoding.UTF8.GetBytes(data));
            var text = new StringBuilder();
            foreach (byte value in hash)
               text.Append(value.ToString("x2"));

            return text.ToString();
         }
      }
   }

   internal static class WebhookNumberExtensions
   {
      /// <summary>A number a JSON answer carried, as one. 0 when it is not one.</summary>
      public static int ToInt(this string value)
      {
         int number;
         return int.TryParse(value, out number) ? number : 0;
      }
   }
}
