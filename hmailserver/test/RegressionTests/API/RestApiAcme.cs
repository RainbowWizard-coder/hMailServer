// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.API
{
   /// <summary>
   ///    The ACME certificate on demand (GitLab work item 98): GET /api/v1/acme, what
   ///    the server knows of the certificate and its checks, and POST /api/v1/acme/run,
   ///    a check now - with the rules that keep a check from being started by the wrong
   ///    credential or spending a CA's limits.
   ///
   ///    A name added to AcmeDomains used to wait for the certificate's renewal, two
   ///    thirds of its life, before any certificate named it, and nothing could ask for
   ///    a check or say how one went. Now a certificate that does not name every
   ///    configured host is due at once, a save of AcmeDomains with such a host starts a
   ///    check, and the two routes ask and tell.
   ///
   ///    The CA is FakeAcmeCa on the loopback address, or a port nothing listens on. No
   ///    private key is ever written to the certificate directory here, so the REST
   ///    listener - which serves HTTPS with the ACME pair when both halves are there -
   ///    stays the plain one the fixtures talk to, and the Linux suite links this
   ///    fixture. Each test has a CA of its own (a port of its own), because the spacing
   ///    the server keeps between checks belongs to the CA it was learnt from and lives
   ///    for the life of the process: a test that shared a CA with the one before would
   ///    meet that one's cooldown.
   /// </summary>
   [TestFixture]
   public class RestApiAcme : TestFixtureBase
   {
      // 10270 to 10280, 10283 and 10284 are this fixture's: the REST listener on the
      // Windows bench, and a CA per test. TlsAcmeIssuance's CAs are 10281 and 10282.
      private static int RestPort = 10270;
      private const string AdminPassword = "testar";

      // The certificate in use: mail.example.test alone, valid to 2046. Public; its key
      // is not anywhere.
      private const string MailOnlyPem =
         "-----BEGIN CERTIFICATE-----\n" +
         "MIIBZzCCAQ2gAwIBAgICEAAwCgYIKoZIzj0EAwIwHDEaMBgGA1UEAwwRbWFpbC5l\n" +
         "eGFtcGxlLnRlc3QwHhcNMjYxMDAxMDAwMDAwWhcNNDYxMDAxMDAwMDAwWjAcMRow\n" +
         "GAYDVQQDDBFtYWlsLmV4YW1wbGUudGVzdDBZMBMGByqGSM49AgEGCCqGSM49AwEH\n" +
         "A0IABMKpoiWouQBKaqu3kIESVQ1JgfoAZHCmxo+tTipAvMkpDYj6DCgEv8ENeeBS\n" +
         "TNIf/vIrw+tiYD8sG3TF+fZzO5WjPzA9MBwGA1UdEQQVMBOCEW1haWwuZXhhbXBs\n" +
         "ZS50ZXN0MB0GA1UdDgQWBBRAX4sanqJemn9//s4RuNyg7kEVkTAKBggqhkjOPQQD\n" +
         "AgNIADBFAiEA9g42H5I8mJ4c1kxfAj0TP1cwNqwSgpLOSQJiugLRSEICICCUhzTg\n" +
         "XjOgn4XfqQCxcjLZDA0CKGyMuE59bWAXTu4e\n" +
         "-----END CERTIFICATE-----\n";

      // Common name other.example.test, and the names mail.example.test and
      // *.wild.example.test; valid 2026-10-01 to 2046-10-01, serial 0x1004.
      private const string WildcardPem =
         "-----BEGIN CERTIFICATE-----\n" +
         "MIIBfTCCASSgAwIBAgICEAQwCgYIKoZIzj0EAwIwHTEbMBkGA1UEAwwSb3RoZXIu\n" +
         "ZXhhbXBsZS50ZXN0MB4XDTI2MTAwMTAwMDAwMFoXDTQ2MTAwMTAwMDAwMFowHTEb\n" +
         "MBkGA1UEAwwSb3RoZXIuZXhhbXBsZS50ZXN0MFkwEwYHKoZIzj0CAQYIKoZIzj0D\n" +
         "AQcDQgAEwqmiJai5AEpqq7eQgRJVDUmB+gBkcKbGj61OKkC8ySkNiPoMKAS/wQ15\n" +
         "4FJM0h/+8ivD62JgPywbdMX59nM7laNUMFIwMQYDVR0RBCowKIIRbWFpbC5leGFt\n" +
         "cGxlLnRlc3SCEyoud2lsZC5leGFtcGxlLnRlc3QwHQYDVR0OBBYEFEBfixqeol6a\n" +
         "f3/+zhG43KDuQRWRMAoGCCqGSM49BAMCA0cAMEQCIH8i8Q1c1y4FUiVeNrgatu/m\n" +
         "0GrUE11CsLLxAufxmdQWAiAbwwTyXVgAmzx3QMRyKpd65toHxBwzlq3gWiZ0YFRf\n" +
         "2A==\n" +
         "-----END CERTIFICATE-----\n";

      private const long NotBefore2026 = 1790812800; // 2026-10-01 00:00:00 UTC
      private const long NotAfter2046 = 2421964800;  // 2046-10-01 00:00:00 UTC

      private string AcmeDirectory
      {
         get { return Paths.Combine(_application.Settings.Directories.ProgramDirectory, "hm_acme_ondemand_test"); }
      }

      private string CertificateFile
      {
         get { return Paths.Combine(AcmeDirectory, "fullchain.pem"); }
      }

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
      public void StopAcmeAndRestApi()
      {
         try
         {
            // A check still running when ACME is switched off would go on to its end
            // against a directory this teardown is about to remove.
            WaitUntilIdle();

            IniFileSetting.Write("AcmeEnabled", null);
            IniFileSetting.Write("AcmeDomains", null);
            IniFileSetting.Write("AcmeDirectoryUrl", null);
            IniFileSetting.Write("AcmeCertificateDirectory", null);
         }
         finally
         {
            RestListener.Stop();
            _application.Reinitialize();

            try
            {
               if (Directory.Exists(AcmeDirectory))
                  Directory.Delete(AcmeDirectory, true);
            }
            catch (IOException)
            {
               // Deliberately ignored: best effort only, and the outcome of the surrounding operation does not depend on this succeeding.
            }
         }
      }

      // ------------------------------------------------------------------ helpers

      private void WriteCertificate(string pem)
      {
         Directory.CreateDirectory(AcmeDirectory);
         File.WriteAllText(CertificateFile, pem, new UTF8Encoding(false));
      }

      // ACME on, against directoryUrl, for domains; the reinitialise registers the
      // checks and runs the one at start, which this waits for.
      private Dictionary<string, object> EnableAcme(string directoryUrl, string domains)
      {
         long before = LastAttemptId();

         IniFileSetting.Write("AcmeCertificateDirectory", AcmeDirectory);
         IniFileSetting.Write("AcmeDirectoryUrl", directoryUrl);
         IniFileSetting.Write("AcmeDomains", domains);
         IniFileSetting.Write("AcmeEnabled", "1");

         _application.Reinitialize();

         return WaitFor(s => MiniJson.At(s, "running") == null && MiniJson.Number(s, "last_attempt.id") > before &&
                             MiniJson.Text(s, "last_attempt.trigger") == "start",
            "the check at start to finish");
      }

      private static Dictionary<string, object> Status()
      {
         (int status, string body) = Http("GET", "/api/v1/acme");
         Assert.AreEqual(200, status, body);
         return MiniJson.Object(body);
      }

      private static long LastAttemptId()
      {
         return MiniJson.Number(Status(), "last_attempt.id");
      }

      private static Dictionary<string, object> WaitFor(Func<Dictionary<string, object>, bool> condition, string what, int seconds = 60)
      {
         DateTime until = DateTime.UtcNow.AddSeconds(seconds);
         string last = "";

         while (DateTime.UtcNow < until)
         {
            (int status, string body) = Http("GET", "/api/v1/acme");
            if (status == 200)
            {
               last = body;
               Dictionary<string, object> state = MiniJson.Object(body);
               if (condition(state))
                  return state;
            }

            Thread.Sleep(250);
         }

         Assert.Fail("Waited " + seconds + " seconds for " + what + ". Last status: " + last);
         return null;
      }

      private static void WaitUntilIdle()
      {
         DateTime until = DateTime.UtcNow.AddSeconds(90);
         while (DateTime.UtcNow < until)
         {
            // A refused or failed read is not idle: it is the listener restarting,
            // and a check runs at that start.
            (int status, string body) = Http("GET", "/api/v1/acme");
            if (status == 200 && MiniJson.At(MiniJson.Object(body), "running") == null)
               return;
            Thread.Sleep(250);
         }
      }

      private static List<string> Names(Dictionary<string, object> state, string path)
      {
         return MiniJson.List(state, path).Cast<string>().ToList();
      }

      private static (int status, string headers, string body) Run(string body = "{}")
      {
         string credentials = Convert.ToBase64String(Encoding.ASCII.GetBytes("Administrator:" + AdminPassword));
         return RestRequest.Exchange("127.0.0.1", RestPort, "POST", "/api/v1/acme/run", "Basic " + credentials, body, null);
      }

      private static int RetryAfter(string headers)
      {
         Match match = Regex.Match(headers ?? "", @"^Retry-After:\s*(\d+)\s*$", RegexOptions.Multiline | RegexOptions.IgnoreCase);
         Assert.IsTrue(match.Success, "No Retry-After in: " + headers);
         return int.Parse(match.Groups[1].Value);
      }

      private static long Now()
      {
         return (long) (DateTime.UtcNow - new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc)).TotalSeconds;
      }

      // The audit trail's records of a requested check, newest first.
      private static List<Dictionary<string, object>> CheckRequestedRecords()
      {
         (int status, string body) = Http("GET", "/api/v1/audit?object_type=ssl+certificate&action=check+requested&limit=200");
         Assert.AreEqual(200, status, body);
         return MiniJson.List(MiniJson.Object(body), "entries")
            .Cast<Dictionary<string, object>>()
            .Where(row => MiniJson.Text(row, "object_type") == "ssl certificate" && MiniJson.Text(row, "action") == "check requested")
            .ToList();
      }

      private static long NewestCheckRequestedRecord()
      {
         return CheckRequestedRecords().Select(row => MiniJson.Number(row, "id")).DefaultIfEmpty(0).Max();
      }

      // A reinitialise, and the check at start it makes, waited for: the one whose id
      // is after the last check's.
      private Dictionary<string, object> ReinitializeAndWaitForTheCheckAtStart()
      {
         long before = LastAttemptId();

         _application.Reinitialize();

         return WaitFor(s => MiniJson.At(s, "running") == null && MiniJson.Number(s, "last_attempt.id") > before &&
                             MiniJson.Text(s, "last_attempt.trigger") == "start",
            "the check at start after the reinitialise to finish");
      }

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
         return RestRequest.Send("127.0.0.1", RestPort, method, path, "Basic " + credentials, requestBody);
      }

      private static (int status, string body) Bearer(string method, string path, string token, string requestBody = null)
      {
         return RestRequest.Send("127.0.0.1", RestPort, method, path, "Bearer " + token, requestBody);
      }

      // ------------------------------------------------------------------ the status

      [Test]
      [Description("The status names the certificate in use - its names (a wildcard among them), subject, issuer, serial and dates - says it covers the configured hosts, one of them through the wildcard, and that the check at start found nothing due; the CA was not asked for a certificate.")]
      public void TheStatusDescribesTheCertificateAndItsCoverage()
      {
         WriteCertificate(WildcardPem);

         using (var ca = new FakeAcmeCa(10271))
         {
            // Written fully qualified once, and beside it an address no certificate this
            // client orders can name: neither may make the certificate due.
            Dictionary<string, object> state = EnableAcme(ca.DirectoryUrl, "Mail.Example.TEST, x.wild.example.test., 192.0.2.7");

            Assert.IsTrue(MiniJson.Flag(state, "enabled"));
            Assert.AreEqual(ca.DirectoryUrl, MiniJson.Text(state, "directory_url"));
            CollectionAssert.AreEqual(new[] { "mail.example.test", "x.wild.example.test" }, Names(state, "domains"),
               "AcmeDomains is answered as the client reads it: lower case, trimmed, the final dot taken off, an address left out.");
            CollectionAssert.AreEqual(new[] { "192.0.2.7" }, Names(state, "unusable_domains"),
               "The name left out is named.");

            Assert.AreEqual(CertificateFile, MiniJson.Text(state, "certificate.file"));
            CollectionAssert.AreEqual(new[] { "mail.example.test", "*.wild.example.test" }, Names(state, "certificate.names"),
               "The names are the subject alternative names; the common name is not one of them.");
            StringAssert.Contains("other.example.test", MiniJson.Text(state, "certificate.subject"));
            StringAssert.Contains("other.example.test", MiniJson.Text(state, "certificate.issuer"));
            Assert.AreEqual("1004", MiniJson.Text(state, "certificate.serial"));
            Assert.AreEqual(NotBefore2026, MiniJson.Number(state, "certificate.not_before"));
            Assert.AreEqual(NotAfter2046, MiniJson.Number(state, "certificate.not_after"));
            Assert.IsFalse(MiniJson.Flag(state, "certificate.expired"));
            Assert.IsFalse(MiniJson.Flag(state, "certificate.deployed"), "No record names a certificate this fixture wrote.");

            Assert.IsTrue(MiniJson.Flag(state, "covers_domains"), "x.wild.example.test is covered by *.wild.example.test.");
            Assert.AreEqual(0, MiniJson.List(state, "missing_domains").Count);
            Assert.IsFalse(MiniJson.Flag(state, "due"));

            Assert.AreEqual("not_due", MiniJson.Text(state, "last_attempt.outcome"));
            Assert.AreEqual("finished", MiniJson.Text(state, "last_attempt.state"));
            Assert.IsNull(MiniJson.At(state, "last_attempt.error"));

            long next = MiniJson.Number(state, "next_scheduled_check_at");
            Assert.That(next, Is.InRange(Now() + 3400, Now() + 3700), "The hourly check is an hour after the start.");

            Assert.AreEqual(60, MiniJson.Number(state, "requests.interval_seconds"));
            Assert.AreEqual(1200, MiniJson.Number(state, "requests.cooldown_seconds"));
            Assert.IsTrue(MiniJson.Flag(state, "requests.allowed_now"));
            Assert.IsNull(MiniJson.At(state, "rate_limited_until"));

            Assert.AreEqual(0, ca.Count("POST", "/order"), "A certificate that is not due was ordered.");

            // Nothing about the CA account, a key or a nonce is in it.
            (int status, string body) = Http("GET", "/api/v1/acme");
            Assert.AreEqual(200, status);
            StringAssert.DoesNotContain("acct", body);
            StringAssert.DoesNotContain("nonce", body);
            StringAssert.DoesNotContain("PRIVATE", body);
         }
      }

      // ------------------------------------------------------------------ the coverage rule

      [Test]
      [Description("A certificate that does not name a host AcmeDomains lists is due at once, whatever its dates: the check at start tries to issue (reason names), and its failure leaves the certificate in use byte for byte, with no record or key written; a request straight after waits out the twenty minutes after a failure, with a Retry-After.")]
      public void ACertificateMissingAConfiguredNameIsDueAndAFailedAttemptLeavesItInPlace()
      {
         WriteCertificate(MailOnlyPem);
         byte[] before = File.ReadAllBytes(CertificateFile);

         // Nothing listens on 10272: the attempt fails at its first step, the directory.
         Dictionary<string, object> state = EnableAcme("http://127.0.0.1:10272/directory", "mail.example.test,new.example.test");

         Assert.IsFalse(MiniJson.Flag(state, "covers_domains"));
         CollectionAssert.AreEqual(new[] { "new.example.test" }, Names(state, "missing_domains"));
         Assert.IsTrue(MiniJson.Flag(state, "due"));
         Assert.AreEqual("names", MiniJson.Text(state, "due_reason"));

         Assert.AreEqual("failed", MiniJson.Text(state, "last_attempt.outcome"), "A certificate twenty years from its renewal, lacking a configured host, was not tried for.");
         Assert.AreEqual("names", MiniJson.Text(state, "last_attempt.reason"));
         Assert.AreEqual("directory", MiniJson.Text(state, "last_attempt.failed_step"));
         StringAssert.Contains("directory", MiniJson.Text(state, "last_attempt.error"));

         CollectionAssert.AreEqual(before, File.ReadAllBytes(CertificateFile), "A failed attempt changed the certificate in use.");
         Assert.IsFalse(File.Exists(Paths.Combine(AcmeDirectory, "privkey.pem")), "A failed attempt wrote a key.");
         Assert.IsFalse(File.Exists(CertificateFile + ".new"), "A failed attempt left a certificate half-installed.");
         CollectionAssert.AreEqual(new[] { "mail.example.test" }, Names(state, "certificate.names"));

         // The cooldown after a failure, with the time it ends.
         (int status, string headers, string body) = Run();
         Assert.AreEqual(429, status, body);
         Dictionary<string, object> refusal = MiniJson.Object(body);
         Assert.AreEqual("cooldown", MiniJson.Text(refusal, "reason"));
         int retryAfter = RetryAfter(headers);
         Assert.That(retryAfter, Is.InRange(1100, 1200), "The cooldown is twenty minutes from the failure.");
         Assert.AreEqual(retryAfter, MiniJson.Number(refusal, "retry_after_seconds"));

         Dictionary<string, object> after = Status();
         Assert.IsFalse(MiniJson.Flag(after, "requests.allowed_now"));
         Assert.AreEqual("cooldown", MiniJson.Text(after, "requests.waiting_for"));
         Assert.AreEqual(MiniJson.Number(state, "last_attempt.id"), MiniJson.Number(after, "last_attempt.id"), "A refused request started a check.");
      }

      // ------------------------------------------------------------------ one at a time

      [Test]
      [Description("A save of AcmeDomains with a host the certificate lacks starts a check at once; a request while it runs is answered 202 with that check (already_running) and starts no other; when the CA stops answering the order the check fails at the order and the certificate stays.")]
      public void ASaveOfAcmeDomainsStartsACheckAndARequestWhileItRunsGetsThatOne()
      {
         WriteCertificate(MailOnlyPem);
         byte[] before = File.ReadAllBytes(CertificateFile);

         using (var ca = new FakeAcmeCa(10273))
         {
            ca.Mode = FakeAcmeCa.OrderMode.Stall;
            ca.StallSeconds = 8;

            Dictionary<string, object> started = EnableAcme(ca.DirectoryUrl, "mail.example.test");
            Assert.AreEqual("not_due", MiniJson.Text(started, "last_attempt.outcome"), "The certificate names the one host configured.");
            Assert.AreEqual(0, ca.Count("POST", "/order"));

            // The save is the trigger: nothing else asks for a check here.
            IniFileSetting.Write("AcmeDomains", "mail.example.test,new.example.test");

            Dictionary<string, object> running = WaitFor(s => MiniJson.Text(s, "running.trigger") == "settings" &&
                                                              MiniJson.Text(s, "running.step") == "order",
               "the check the save started to reach the order", 30);
            long id = MiniJson.Number(running, "running.id");

            (int status, string headers, string body) = Run();
            Assert.AreEqual(202, status, body);
            Dictionary<string, object> answer = MiniJson.Object(body);
            Assert.IsFalse(MiniJson.Flag(answer, "accepted"));
            Assert.IsTrue(MiniJson.Flag(answer, "already_running"));
            Assert.AreEqual(id, MiniJson.Number(answer, "attempt.id"), "The answer is not the check that is running.");
            Assert.AreEqual("settings", MiniJson.Text(answer, "attempt.trigger"));
            Assert.AreEqual("/api/v1/acme", MiniJson.Text(answer, "status"));

            Dictionary<string, object> done = WaitFor(s => MiniJson.At(s, "running") == null && MiniJson.Number(s, "last_attempt.id") == id,
               "the stalled check to end", 60);
            Assert.AreEqual("failed", MiniJson.Text(done, "last_attempt.outcome"));
            Assert.AreEqual("order", MiniJson.Text(done, "last_attempt.failed_step"));
            Assert.AreEqual(1, ca.Count("POST", "/order"), "A second order was made while the first one ran.");

            CollectionAssert.AreEqual(before, File.ReadAllBytes(CertificateFile), "A failed attempt changed the certificate in use.");
         }
      }

      // ------------------------------------------------------------------ the CA's rate limit

      [Test]
      [Description("A CA's rateLimited answer to the order is reported - the problem's type and detail, and rate_limited_until from its Retry-After - and obeyed: a request is refused 429 until then, with the CA's Retry-After rather than the twenty-minute cooldown.")]
      public void ARateLimitedOrderIsReportedAndObeyed()
      {
         WriteCertificate(MailOnlyPem);

         using (var ca = new FakeAcmeCa(10274))
         {
            ca.Mode = FakeAcmeCa.OrderMode.RateLimited;
            ca.RetryAfter = "7200";

            Dictionary<string, object> state = EnableAcme(ca.DirectoryUrl, "mail.example.test,new.example.test");

            Assert.AreEqual("failed", MiniJson.Text(state, "last_attempt.outcome"));
            Assert.AreEqual("order", MiniJson.Text(state, "last_attempt.failed_step"));
            Assert.AreEqual("urn:ietf:params:acme:error:rateLimited", MiniJson.Text(state, "last_attempt.ca_problem.type"));
            StringAssert.Contains("too many certificates", MiniJson.Text(state, "last_attempt.ca_problem.detail"));
            Assert.That(MiniJson.Number(state, "rate_limited_until"), Is.InRange(Now() + 7000, Now() + 7300));

            (int status, string headers, string body) = Run();
            Assert.AreEqual(429, status, body);
            Assert.AreEqual("rate_limited", MiniJson.Text(MiniJson.Object(body), "reason"));
            Assert.That(RetryAfter(headers), Is.InRange(7000, 7200), "The CA's two hours, not the twenty-minute cooldown.");

            Assert.AreEqual(1, ca.Count("POST", "/order"), "The rate limit was not obeyed.");
         }
      }

      [Test]
      [Description("A CA's refusal of the order that is no rate limit (rejectedIdentifier, 400, with a Retry-After of a day beside it) is reported as the CA's problem, its detail whole with the name it quotes, and holds nothing but the twenty-minute cooldown: rate_limited_until stays null and a request is refused 429 for the cooldown, not for a rate limit.")]
      public void AnotherProblemFromTheCaIsReportedAndIsNoRateLimit()
      {
         WriteCertificate(MailOnlyPem);
         byte[] before = File.ReadAllBytes(CertificateFile);

         using (var ca = new FakeAcmeCa(10283))
         {
            ca.Mode = FakeAcmeCa.OrderMode.Rejected;

            Dictionary<string, object> state = EnableAcme(ca.DirectoryUrl, "mail.example.test,new.example.test");

            Assert.AreEqual("failed", MiniJson.Text(state, "last_attempt.outcome"));
            Assert.AreEqual("order", MiniJson.Text(state, "last_attempt.failed_step"));
            Assert.AreEqual("urn:ietf:params:acme:error:rejectedIdentifier", MiniJson.Text(state, "last_attempt.ca_problem.type"));
            Assert.AreEqual("Error creating new order :: Cannot issue for \"new.example.test\": Domain name does not end with a valid public suffix (TLD)",
               MiniJson.Text(state, "last_attempt.ca_problem.detail"), "The CA's detail is not reported whole.");
            Assert.IsNull(MiniJson.At(state, "rate_limited_until"), "A refusal that is no rate limit was taken for one.");

            (int status, string headers, string body) = Run();
            Assert.AreEqual(429, status, body);
            Assert.AreEqual("cooldown", MiniJson.Text(MiniJson.Object(body), "reason"), "A refusal that is no rate limit held the requests as one.");
            Assert.That(RetryAfter(headers), Is.InRange(1100, 1200), "The twenty-minute cooldown, not the day the CA's Retry-After gave.");

            Dictionary<string, object> after = Status();
            Assert.AreEqual("cooldown", MiniJson.Text(after, "requests.waiting_for"));
            Assert.IsNull(MiniJson.At(after, "rate_limited_until"));

            Assert.AreEqual(1, ca.Count("POST", "/order"));
            CollectionAssert.AreEqual(before, File.ReadAllBytes(CertificateFile), "A failed attempt changed the certificate in use.");
         }
      }

      // ------------------------------------------------------------------ a save while a check runs

      [Test]
      [Description("A save of AcmeDomains made while a check runs - here a requested check that finds the names covered, held where it asks the CA's directory for the renewal window - queues no check beside it, and is looked at again when that check ends: a check with trigger settings and reason names follows it at once, not at the next hour.")]
      public void ASaveWhileACheckRunsIsCheckedWhenThatCheckEnds()
      {
         WriteCertificate(MailOnlyPem);
         byte[] before = File.ReadAllBytes(CertificateFile);

         using (var ca = new FakeAcmeCa(10284))
         {
            Dictionary<string, object> started = EnableAcme(ca.DirectoryUrl, "mail.example.test");
            Assert.AreEqual("not_due", MiniJson.Text(started, "last_attempt.outcome"), "The certificate names the one host configured.");

            // The next directory request - the requested check's, which finds the names
            // covered and asks for the CA's renewal window - is held eight seconds and
            // dropped, after which the check falls back to the dates and finds nothing due.
            ca.HoldNextDirectory(8);

            (int status, string headers, string body) = Run();
            Assert.AreEqual(202, status, body);
            long id = MiniJson.Number(MiniJson.Object(body), "attempt.id");

            DateTime until = DateTime.UtcNow.AddSeconds(30);
            while (ca.DirectoriesHeld == 0 && DateTime.UtcNow < until)
               Thread.Sleep(100);
            Assert.AreEqual(1, ca.DirectoriesHeld, "The requested check did not reach the directory.");

            // The order the check after it makes is held a second and dropped.
            ca.Mode = FakeAcmeCa.OrderMode.Stall;
            ca.StallSeconds = 1;

            IniFileSetting.Write("AcmeDomains", "mail.example.test,new.example.test");

            Dictionary<string, object> during = Status();
            Assert.AreEqual(id, MiniJson.Number(during, "running.id"), "The save queued a check in place of the one running.");
            Assert.AreEqual("request", MiniJson.Text(during, "running.trigger"));

            Dictionary<string, object> after = WaitFor(s => MiniJson.At(s, "running") == null && MiniJson.Number(s, "last_attempt.id") > id,
               "the check after the held one to end", 60);
            Assert.AreEqual("settings", MiniJson.Text(after, "last_attempt.trigger"),
               "The check after the held one is not the save's.");
            Assert.AreEqual("names", MiniJson.Text(after, "last_attempt.reason"));
            Assert.AreEqual("failed", MiniJson.Text(after, "last_attempt.outcome"));
            Assert.AreEqual("order", MiniJson.Text(after, "last_attempt.failed_step"));
            Assert.AreEqual(1, ca.Count("POST", "/order"), "The save's check is not the one order made.");

            CollectionAssert.AreEqual(before, File.ReadAllBytes(CertificateFile), "A failed attempt changed the certificate in use.");
         }
      }

      // ------------------------------------------------------------------ every trigger keeps the spacing

      [Test]
      [Description("The twenty minutes after a failed attempt hold every check, not only a requested one: the check at start that a reinitialise makes finds the certificate due and orders nothing (outcome waiting, failed_step cooldown), asking the CA nothing at all, and a save of AcmeDomains with another host inside those minutes queues no check.")]
      public void TheCooldownHoldsTheCheckAtStartAndASave()
      {
         WriteCertificate(MailOnlyPem);
         byte[] before = File.ReadAllBytes(CertificateFile);

         using (var ca = new FakeAcmeCa(10277))
         {
            // The order is held a second and dropped: a CA that stopped answering.
            ca.Mode = FakeAcmeCa.OrderMode.Stall;
            ca.StallSeconds = 1;

            Dictionary<string, object> failed = EnableAcme(ca.DirectoryUrl, "mail.example.test,new.example.test");
            Assert.AreEqual("failed", MiniJson.Text(failed, "last_attempt.outcome"));
            Assert.AreEqual("order", MiniJson.Text(failed, "last_attempt.failed_step"));
            Assert.AreEqual(1, ca.Count("POST", "/order"));
            int requestsAfterTheFailure = ca.Requests.Count;

            Dictionary<string, object> held = ReinitializeAndWaitForTheCheckAtStart();
            Assert.AreEqual("waiting", MiniJson.Text(held, "last_attempt.outcome"),
               "The check at start, inside the twenty minutes after a failure, did not wait: " + MiniJson.Text(held, "last_attempt.error"));
            Assert.AreEqual("cooldown", MiniJson.Text(held, "last_attempt.failed_step"));
            Assert.AreEqual("names", MiniJson.Text(held, "last_attempt.reason"), "The certificate was due, and held.");
            StringAssert.Contains("twenty minutes", MiniJson.Text(held, "last_attempt.error"));
            Assert.AreEqual(MiniJson.Number(failed, "last_issuance.id"), MiniJson.Number(held, "last_issuance.id"),
               "A check that ordered nothing was taken for the last attempt to issue.");
            Assert.AreEqual(requestsAfterTheFailure, ca.Requests.Count,
               "The check at start asked the CA something inside the cooldown: " + string.Join(", ", ca.Requests.Skip(requestsAfterTheFailure)));

            long heldId = MiniJson.Number(held, "last_attempt.id");

            IniFileSetting.Write("AcmeDomains", "mail.example.test,new.example.test,third.example.test");
            Thread.Sleep(3000);

            Dictionary<string, object> afterSave = Status();
            Assert.IsNull(MiniJson.At(afterSave, "running"), "A save inside the cooldown queued a check.");
            Assert.AreEqual(heldId, MiniJson.Number(afterSave, "last_attempt.id"), "A save inside the cooldown started a check.");
            Assert.AreEqual(1, ca.Count("POST", "/order"), "A save inside the cooldown ordered a certificate.");
            Assert.AreEqual("cooldown", MiniJson.Text(afterSave, "requests.waiting_for"));

            CollectionAssert.AreEqual(before, File.ReadAllBytes(CertificateFile), "The certificate in use was changed.");
         }
      }

      [Test]
      [Description("A CA's rateLimited answer with no Retry-After holds every order for an hour, as RFC 8555 section 6.6 leaves it to the client: rate_limited_until is an hour on, a request is refused for it rather than for the twenty-minute cooldown, and the check at start that a reinitialise makes waits for it (failed_step rate_limited) and orders nothing.")]
      public void ARateLimitWithNoRetryAfterHoldsEveryCheckForAnHour()
      {
         WriteCertificate(MailOnlyPem);

         using (var ca = new FakeAcmeCa(10278))
         {
            ca.Mode = FakeAcmeCa.OrderMode.RateLimited;
            ca.RetryAfter = null;

            Dictionary<string, object> state = EnableAcme(ca.DirectoryUrl, "mail.example.test,new.example.test");
            Assert.AreEqual("failed", MiniJson.Text(state, "last_attempt.outcome"));
            Assert.AreEqual("urn:ietf:params:acme:error:rateLimited", MiniJson.Text(state, "last_attempt.ca_problem.type"));
            long finished = MiniJson.Number(state, "last_attempt.finished_at");
            Assert.That(MiniJson.Number(state, "rate_limited_until"), Is.InRange(finished + 3600, finished + 3601),
               "A rate limit with no Retry-After did not hold for an hour from the failure.");

            (int status, string headers, string body) = Run();
            Assert.AreEqual(429, status, body);
            Assert.AreEqual("rate_limited", MiniJson.Text(MiniJson.Object(body), "reason"), "The hour, not the twenty-minute cooldown.");
            Assert.That(RetryAfter(headers), Is.InRange(3300, 3600));

            Dictionary<string, object> held = ReinitializeAndWaitForTheCheckAtStart();
            Assert.AreEqual("waiting", MiniJson.Text(held, "last_attempt.outcome"),
               "The check at start, inside the CA's rate limit, did not wait: " + MiniJson.Text(held, "last_attempt.error"));
            Assert.AreEqual("rate_limited", MiniJson.Text(held, "last_attempt.failed_step"));
            Assert.AreEqual(1, ca.Count("POST", "/order"), "The check at start ordered inside the CA's rate limit.");
         }
      }

      // ------------------------------------------------------------------ one at a time, whatever started it

      [Test]
      [Description("One check at a time holds across triggers, not only for a request: while a check a save started is held at the order, the check at start that a reinitialise makes does not begin beside it - the CA sees one order - and the held check is the last to finish.")]
      public void TheCheckAtStartDoesNotBeginBesideARunningCheck()
      {
         WriteCertificate(MailOnlyPem);
         byte[] before = File.ReadAllBytes(CertificateFile);

         using (var ca = new FakeAcmeCa(10279))
         {
            // Held longer than the reinitialise takes: its stop waits ten seconds for
            // the maintenance queue's threads and then goes on without one inside an
            // HTTP request, which the stop does not interrupt, so that the held check
            // outlives the stop and the start comes while it runs.
            ca.Mode = FakeAcmeCa.OrderMode.Stall;
            ca.StallSeconds = 25;

            Dictionary<string, object> started = EnableAcme(ca.DirectoryUrl, "mail.example.test");
            Assert.AreEqual("not_due", MiniJson.Text(started, "last_attempt.outcome"), "The certificate names the one host configured.");

            IniFileSetting.Write("AcmeDomains", "mail.example.test,new.example.test");

            Dictionary<string, object> running = WaitFor(s => MiniJson.Text(s, "running.trigger") == "settings" &&
                                                              MiniJson.Text(s, "running.step") == "order",
               "the check the save started to reach the order", 30);
            long id = MiniJson.Number(running, "running.id");

            // The start's check is queued as the servers start again; it finds this one
            // running and does nothing.
            _application.Reinitialize();

            Dictionary<string, object> during = Status();
            Assert.AreEqual(id, MiniJson.Number(during, "running.id"),
               "The check at start took the place of the one running: " + MiniJson.Text(during, "running.trigger"));

            Dictionary<string, object> done = WaitFor(s => MiniJson.At(s, "running") == null && MiniJson.Number(s, "last_attempt.id") >= id,
               "the held check to end", 90);
            Assert.AreEqual(id, MiniJson.Number(done, "last_attempt.id"),
               "A check began beside the held one and ended after it: " + MiniJson.Text(done, "last_attempt.trigger"));
            Assert.AreEqual("settings", MiniJson.Text(done, "last_attempt.trigger"));
            Assert.AreEqual("failed", MiniJson.Text(done, "last_attempt.outcome"));
            Assert.AreEqual("order", MiniJson.Text(done, "last_attempt.failed_step"));
            Assert.AreEqual(1, ca.Count("POST", "/order"), "The check at start ordered beside the check that was running.");

            CollectionAssert.AreEqual(before, File.ReadAllBytes(CertificateFile), "A failed attempt changed the certificate in use.");
         }
      }

      // ------------------------------------------------------------------ a save that adds nothing

      [Test]
      [Description("A save of AcmeDomains that names only hosts the certificate already carries - in another case, fully qualified, reordered, through its wildcard, with a host taken off or beside an address - queues no check, though the spacing would allow one; a save that adds a host it lacks then does.")]
      public void ASaveNamingOnlyHostsTheCertificateCarriesQueuesNoCheck()
      {
         // mail.example.test and *.wild.example.test.
         WriteCertificate(WildcardPem);

         using (var ca = new FakeAcmeCa(10280))
         {
            Dictionary<string, object> started = EnableAcme(ca.DirectoryUrl, "mail.example.test,x.wild.example.test");
            Assert.AreEqual("not_due", MiniJson.Text(started, "last_attempt.outcome"));
            Assert.IsTrue(MiniJson.Flag(started, "covers_domains"));
            Assert.IsTrue(MiniJson.Flag(started, "requests.allowed_now"), "The spacing holds a check, so this test would prove nothing.");
            long startId = MiniJson.Number(started, "last_attempt.id");
            int requestsBefore = ca.Requests.Count;

            foreach (string list in new[] { "MAIL.Example.Test.", "x.wild.example.test,mail.example.test", "y.wild.example.test", "mail.example.test, 192.0.2.7" })
               IniFileSetting.Write("AcmeDomains", list);

            // A check queued by any of them would have run and ended within this.
            Thread.Sleep(3000);

            Dictionary<string, object> after = Status();
            Assert.IsNull(MiniJson.At(after, "running"), "A save of hosts the certificate carries queued a check.");
            Assert.AreEqual(startId, MiniJson.Number(after, "last_attempt.id"),
               "A save of hosts the certificate carries started a check: " + MiniJson.Text(after, "last_attempt.trigger"));
            Assert.AreEqual(requestsBefore, ca.Requests.Count,
               "A save of hosts the certificate carries asked the CA something: " + string.Join(", ", ca.Requests.Skip(requestsBefore)));

            // The control: a host it lacks is a check at once, as the subscription is
            // there to make it.
            ca.Mode = FakeAcmeCa.OrderMode.Stall;
            ca.StallSeconds = 1;
            IniFileSetting.Write("AcmeDomains", "mail.example.test,new.example.test");

            Dictionary<string, object> saved = WaitFor(s => MiniJson.At(s, "running") == null && MiniJson.Number(s, "last_attempt.id") > startId,
               "the check the save of a missing host started to end");
            Assert.AreEqual("settings", MiniJson.Text(saved, "last_attempt.trigger"));
            Assert.AreEqual("names", MiniJson.Text(saved, "last_attempt.reason"));
         }
      }

      // ------------------------------------------------------------------ a request

      [Test]
      [Description("A request is accepted with 202 and the queued check, runs as a check of its own (trigger request), is on the audit trail, and a second request within the minute is refused 429 for the interval, not for a cooldown.")]
      public void ARequestIsAcceptedRecordedAndSpacedAMinuteApart()
      {
         WriteCertificate(MailOnlyPem);

         using (var ca = new FakeAcmeCa(10275))
         {
            EnableAcme(ca.DirectoryUrl, "mail.example.test");

            // The trail is the bench's, kept from run to run, and other tests ask for
            // checks too: only a record after this one is this request's.
            long lastRecord = NewestCheckRequestedRecord();

            (int status, string headers, string body) = Run("");
            Assert.AreEqual(202, status, body);
            Dictionary<string, object> answer = MiniJson.Object(body);
            Assert.IsTrue(MiniJson.Flag(answer, "accepted"));
            Assert.IsFalse(MiniJson.Flag(answer, "already_running"));
            Assert.AreEqual("request", MiniJson.Text(answer, "attempt.trigger"));
            long id = MiniJson.Number(answer, "attempt.id");

            Dictionary<string, object> done = WaitFor(s => MiniJson.Number(s, "last_attempt.id") == id && MiniJson.At(s, "running") == null,
               "the requested check to finish");
            Assert.AreEqual("request", MiniJson.Text(done, "last_attempt.trigger"));
            Assert.AreEqual("not_due", MiniJson.Text(done, "last_attempt.outcome"));
            Assert.AreEqual(0, ca.Count("POST", "/order"), "A check that found nothing due ordered a certificate.");

            List<Dictionary<string, object>> records = CheckRequestedRecords()
               .Where(row => MiniJson.Number(row, "id") > lastRecord)
               .ToList();
            Assert.AreEqual(1, records.Count, "The request is not on the audit trail once, as one record after the last: " +
               string.Join(" | ", records.Select(row => MiniJson.Text(row, "detail"))));
            Dictionary<string, object> record = records[0];
            Assert.AreEqual("ACME (automatic)", MiniJson.Text(record, "object"));
            Assert.AreEqual("administrator", MiniJson.Text(record, "actor"), "The record does not name the credential that asked.");
            Assert.AreEqual("REST", MiniJson.Text(record, "interface"));
            StringAssert.StartsWith("names: mail.example.test; directory: " + ca.DirectoryUrl, MiniJson.Text(record, "detail"),
               "The record does not carry the names and the directory the check is for.");

            (int again, string againHeaders, string againBody) = Run();
            Assert.AreEqual(429, again, againBody);
            Assert.AreEqual("interval", MiniJson.Text(MiniJson.Object(againBody), "reason"));
            Assert.That(RetryAfter(againHeaders), Is.InRange(1, 60));
         }
      }

      // ------------------------------------------------------------------ refusals

      [Test]
      [Description("The run takes no body but {} - a list of names of its own is refused 400 - and is refused 409 while ACME is off, which the status says.")]
      public void TheRunTakesNoNamesAndNeedsAcme()
      {
         (int offStatus, string offHeaders, string offBody) = Run();
         Assert.AreEqual(409, offStatus, offBody);
         Assert.AreEqual("disabled", MiniJson.Text(MiniJson.Object(offBody), "reason"));
         Assert.IsFalse(MiniJson.Flag(Status(), "enabled"));

         WriteCertificate(MailOnlyPem);

         using (var ca = new FakeAcmeCa(10276))
         {
            long before = MiniJson.Number(EnableAcme(ca.DirectoryUrl, "mail.example.test"), "last_attempt.id");

            (int status, string headers, string body) = Run("{\"domains\":[\"attacker.example\"]}");
            Assert.AreEqual(400, status, body);
            Assert.AreEqual(before, LastAttemptId(), "A refused body started a check.");
            Assert.IsNull(MiniJson.At(Status(), "running"));
         }
      }

      // ------------------------------------------------------------------ authorisation

      [Test]
      [Description("A key issued for named domains is refused both routes; a read-only key reads the status and is refused the run; no credential at all is 401.")]
      public void TheRoutesAreTheServerAdministrators()
      {
         (string scopedId, string scopedKey) = CreateKey("acme - scoped", "full", "example.test");
         (string readOnlyId, string readOnlyKey) = CreateKey("acme - readonly", "readonly", null);

         try
         {
            Assert.AreEqual(403, Bearer("GET", "/api/v1/acme", scopedKey).status, "A key for one domain read the server's certificate.");
            Assert.AreEqual(403, Bearer("POST", "/api/v1/acme/run", scopedKey, "{}").status, "A key for one domain asked for a check.");

            Assert.AreEqual(200, Bearer("GET", "/api/v1/acme", readOnlyKey).status, "A read-only key may read the status.");
            Assert.AreEqual(403, Bearer("POST", "/api/v1/acme/run", readOnlyKey, "{}").status, "A read-only key asked for a check.");

            Assert.AreEqual(401, RestRequest.Send("127.0.0.1", RestPort, "GET", "/api/v1/acme", null, null).status);
            Assert.AreEqual(401, RestRequest.Send("127.0.0.1", RestPort, "POST", "/api/v1/acme/run", null, "{}").status);
         }
         finally
         {
            Http("DELETE", "/api/v1/apikeys/" + scopedId);
            Http("DELETE", "/api/v1/apikeys/" + readOnlyId);
         }
      }
   }
}
