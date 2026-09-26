// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using System.Net;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
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
   ///    Mail in, to a webhook (docs/InboundWebhooks.md): an address, or a whole
   ///    domain, whose mail this server posts to a URL.
   ///
   ///    Every message here is sent over SMTP, because only a real message proves
   ///    that the recipient path takes a routed address, the delivery path takes
   ///    it off the message and the pass posts what the message store kept. What
   ///    is asserted is what a receiver sees: the request, its fields as a
   ///    SendGrid receiver would read them, the signature recomputed from the two
   ///    headers and the raw body, and whether the mailbox the address has got the
   ///    message too.
   ///
   ///    No test asserts on the number POST /run answers: a drain is posted the
   ///    moment a message is queued and one pass runs at a time, so "delivered" is
   ///    whichever pass got there first. The queue's states and the requests the
   ///    endpoint got are what is asserted.
   ///
   ///    This file runs in the Windows suite and the Linux one. The tests that need
   ///    the COM shape of a distribution list, or an event script to hold a message
   ///    between its acceptance and its delivery, are in InboundWebhooksLists.cs,
   ///    which only the Windows suite compiles.
   ///
   ///    Ports 10180 to 10189: the REST listener on 10180, the endpoint on 10181, a
   ///    second endpoint on 10182.
   /// </summary>
   [TestFixture]
   public partial class InboundWebhooks : TestFixtureBase
   {
      private static int RestPort = 10180;
      private const int EndpointPort = 10181;
      private const int SecondEndpointPort = 10182;

      private const string AdminPassword = "testar";
      private const string RoutesPath = "/api/v1/inbound-webhooks";
      private const string Secret = "an-inbound-secret-nobody-else-has";
      private const string NewSecret = "the-secret-it-was-changed-to";
      private const string AccountPassword = "secret-password";

      private static readonly Encoding Latin1 = Encoding.GetEncoding("ISO-8859-1");

      private static string EndpointUrl
      {
         get { return "http://127.0.0.1:" + EndpointPort + "/inbound"; }
      }

      private static string SecondEndpointUrl
      {
         get { return "http://127.0.0.1:" + SecondEndpointPort + "/second"; }
      }

      private bool _changed;
      private string _domainName;
      private readonly List<string> _keys = new List<string>();
      private readonly List<string> _roles = new List<string>();

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

         RemoveEveryRoute();

         Http("PUT", "/api/v1/settings", "{\"webhook_max_attempts\":5,\"webhook_retention_days\":7,\"webhooks_enabled\":true}");
      }

      [TearDown]
      public void StopRestApi()
      {
         if (!_changed)
            return;

         RunEach(
            RemoveEveryRoute,
            () => Assert.AreEqual(200, Http("PUT", "/api/v1/settings", "{\"webhook_max_attempts\":5,\"webhook_retention_days\":7,\"webhooks_enabled\":true}").status),
            () =>
            {
               foreach (string id in _keys)
                  Assert.AreEqual(200, Http("DELETE", "/api/v1/apikeys/" + id).status, "API key " + id + " was not removed.");
               _keys.Clear();

               foreach (string role in _roles)
                  Assert.AreEqual(200, Http("DELETE", "/api/v1/roles/" + role).status, "Role " + role + " was not removed.");
               _roles.Clear();
            },
            RestListener.Stop,
            () => _application.Reinitialize());
      }

      [Test]
      [Description("A message sent to a routed address arrives at the endpoint as SendGrid's form - the fields decoded, the attachment a file, the envelope - signed so that a receiver recomputes the signature, posted once, and the mailbox the address has receives nothing.")]
      public void AMessageArrivesAsSendGridsFormAndTheMailboxGetsNothing()
      {
         string address = "support@" + _domainName;
         SingletonProvider<TestSetup>.Instance.AddAccount(CurrentDomain(), address, AccountPassword);

         using (var endpoint = new FakeHttpEndpoint(IPAddress.Loopback, EndpointPort, 200, "{}", "application/json"))
         {
            Create(address);

            byte[] attachment = AttachmentBytes();
            SmtpClientSimulator.StaticSendRaw("outsider@example.net", address, MessageWithAttachment(address, "My order has not arrived", attachment));

            List<string> requests = WaitForRequests(endpoint, 1);
            Assert.AreEqual(1, requests.Count, "The endpoint was called " + requests.Count + " time(s).");

            string request = requests[0];
            byte[] body = endpoint.RequestBodies[0];

            StringAssert.StartsWith("POST /inbound ", request);
            StringAssert.Contains("X-hMailServer-Event: message.inbound", request, request);
            StringAssert.Contains("X-hMailServer-Attempt: 1", request, request);
            StringAssert.Contains("Content-Type: multipart/form-data; boundary=", request, request);

            string timestamp = Header(request, "X-hMailServer-Timestamp");
            string signature = Header(request, "X-hMailServer-Signature");
            Assert.IsNotEmpty(timestamp, "A signature with no timestamp can be replayed for ever.");
            Assert.AreEqual("sha256=" + Hmac(Secret, timestamp, body), signature,
               "The outbound webhooks' recipe: HMAC-SHA256 over the timestamp, a full stop and the raw body.");

            Dictionary<string, FormPart> form = ParseForm(request, body);

            StringAssert.Contains("outsider@example.net", form["from"].Text);
            Assert.AreEqual(address, form["to"].Text);
            Assert.AreEqual("My order has not arrived", form["subject"].Text);
            StringAssert.Contains("The parcel was due on Monday.", form["text"].Text);
            StringAssert.Contains("<b>Monday</b>", form["html"].Text);
            StringAssert.Contains("Subject: My order has not arrived", form["headers"].Text);
            StringAssert.Contains("\"to\":[\"" + address + "\"]", form["envelope"].Text, form["envelope"].Text);
            StringAssert.Contains("\"from\":\"outsider@example.net\"", form["envelope"].Text, form["envelope"].Text);
            Assert.AreEqual("1", form["attachments"].Text);
            StringAssert.Contains("\"filename\":\"receipt.pdf\"", form["attachment-info"].Text, form["attachment-info"].Text);
            StringAssert.Contains("\"type\":\"application/pdf\"", form["attachment-info"].Text, form["attachment-info"].Text);
            Assert.AreEqual("receipt.pdf", form["attachment1"].FileName);
            CollectionAssert.AreEqual(attachment, form["attachment1"].Bytes, "The attachment is the file the sender attached, decoded.");
            Assert.AreEqual(address, form["route"].Text);
            Assert.IsTrue(form.ContainsKey("spam"), "The spam verdict travels, whatever it is.");
            Assert.IsFalse(form.ContainsKey("email"), "The raw message goes only where the route asks for it.");

            string queue = RunUntil(RoutesPath + "/deliveries?limit=50", "\"state\":\"delivered\"");
            Assert.AreEqual(1, Count(queue, "\"state\":\"delivered\""), queue);
            StringAssert.Contains("\"message_kept\":false", queue, "A delivered message's copy is not kept: " + queue);

            Pop3ClientSimulator.AssertMessageCount(address, AccountPassword, 0);

            // Once, however many passes have run since: a delivered row is never
            // posted again.
            Assert.AreEqual(1, endpoint.Requests.Count, "The message was posted more than once.");
         }
      }

      [Test]
      [Description("A secret changed is the one the next message is signed under, and not the old one; a change that sends an empty secret keeps the one that is set, and the next message is signed under it still.")]
      public void AChangedSecretSignsTheNextMessageAndAnEmptyOneKeepsIt()
      {
         string address = "rotate@" + _domainName;
         string path = RoutesPath + "/" + Uri.EscapeDataString(address);

         using (var endpoint = new FakeHttpEndpoint(IPAddress.Loopback, EndpointPort, 200, "{}", "application/json"))
         {
            Create(address);

            (int status, string body) changed = Http("PUT", path, "{\"secret\":\"" + NewSecret + "\"}");
            Assert.AreEqual(200, changed.status, changed.body);

            SmtpClientSimulator.StaticSend("outsider@example.net", address, "First", "A body.");
            List<string> requests = WaitForRequests(endpoint, 1);
            Assert.AreEqual(1, requests.Count);

            string timestamp = Header(requests[0], "X-hMailServer-Timestamp");
            string signature = Header(requests[0], "X-hMailServer-Signature");
            Assert.AreEqual("sha256=" + Hmac(NewSecret, timestamp, endpoint.RequestBodies[0]), signature, "Signed under the secret it was changed to.");
            Assert.AreNotEqual("sha256=" + Hmac(Secret, timestamp, endpoint.RequestBodies[0]), signature, "Not under the one it was made with.");

            (int status, string body) kept = Http("PUT", path, "{\"secret\":\"\",\"enabled\":true}");
            Assert.AreEqual(200, kept.status, kept.body);
            StringAssert.Contains("\"secret_set\":true", kept.body, kept.body);

            SmtpClientSimulator.StaticSend("outsider@example.net", address, "Second", "Another body.");
            requests = WaitForRequests(endpoint, 2);
            Assert.AreEqual(2, requests.Count);

            timestamp = Header(requests[1], "X-hMailServer-Timestamp");
            Assert.AreEqual("sha256=" + Hmac(NewSecret, timestamp, endpoint.RequestBodies[1]), Header(requests[1], "X-hMailServer-Signature"),
               "An empty secret in a change keeps the one that is set.");
         }
      }

      [Test]
      [Description("A route that delivers as well posts the message and delivers it to the mailbox the address has.")]
      public void ARouteThatDeliversAsWellDoesBoth()
      {
         string address = "orders@" + _domainName;
         SingletonProvider<TestSetup>.Instance.AddAccount(CurrentDomain(), address, AccountPassword);

         using (var endpoint = new FakeHttpEndpoint(IPAddress.Loopback, EndpointPort, 200, "{}", "application/json"))
         {
            Create(address, "\"deliver\":true");

            SmtpClientSimulator.StaticSend("outsider@example.net", address, "An order", "Two of the blue ones, please.");

            Assert.AreEqual(1, WaitForRequests(endpoint, 1).Count);
            Pop3ClientSimulator.AssertMessageCount(address, AccountPassword, 1);
         }
      }

      [Test]
      [Description("A domain route takes an address no account has - RCPT TO is accepted for it - and posts it.")]
      public void ADomainRouteTakesAnAddressNobodyHas()
      {
         using (var endpoint = new FakeHttpEndpoint(IPAddress.Loopback, EndpointPort, 200, "{}", "application/json"))
         {
            Create(_domainName);

            string address = "nobody-has-this@" + _domainName;
            SmtpClientSimulator.StaticSend("outsider@example.net", address, "To nobody", "A body.");

            List<string> requests = WaitForRequests(endpoint, 1);
            Assert.AreEqual(1, requests.Count);

            Dictionary<string, FormPart> form = ParseForm(requests[0], endpoint.RequestBodies[0]);
            StringAssert.Contains("\"to\":[\"" + address + "\"]", form["envelope"].Text, form["envelope"].Text);
            Assert.AreEqual(_domainName, form["route"].Text);
         }
      }

      [Test]
      [Description("An address route switched off leaves its address to the domain's route, which is on: RCPT TO takes the address for the domain's route, and the delivery path posts it there - it is neither returned as an unknown address nor held for the route that is off.")]
      public void AnAddressRouteSwitchedOffLeavesItsAddressToTheDomainsRoute()
      {
         string sender = "desk-sender@" + _domainName;
         SingletonProvider<TestSetup>.Instance.AddAccount(CurrentDomain(), sender, AccountPassword);

         string address = "desk@" + _domainName;

         using (var endpoint = new FakeHttpEndpoint(IPAddress.Loopback, EndpointPort, 200, "{}", "application/json"))
         {
            Create(address);
            Create(_domainName);

            (int status, string body) off = Http("PUT", RoutesPath + "/" + Uri.EscapeDataString(address), "{\"enabled\":false}");
            Assert.AreEqual(200, off.status, off.body);

            SmtpClientSimulator.StaticSend(sender, address, "To the desk", "A body.");

            List<string> requests = WaitForRequests(endpoint, 1);
            Assert.AreEqual(1, requests.Count, "The domain's route did not post it.");

            Dictionary<string, FormPart> form = ParseForm(requests[0], endpoint.RequestBodies[0]);
            Assert.AreEqual(_domainName, form["route"].Text, "Posted by the domain's route, the one that is on.");
            StringAssert.Contains("\"to\":[\"" + address + "\"]", form["envelope"].Text, form["envelope"].Text);

            // And nothing came back to its sender as an unknown address.
            Pop3ClientSimulator.AssertMessageCount(sender, AccountPassword, 0);
         }
      }

      [Test]
      [Description("A recipient written with a plus tag is found by the address before the tag, and the tag is kept in what is posted.")]
      public void APlusTagIsFoundByItsAddressAndKept()
      {
         Domain domain = CurrentDomain();
         domain.PlusAddressingEnabled = true;
         domain.PlusAddressingCharacter = "+";
         domain.Save();

         using (var endpoint = new FakeHttpEndpoint(IPAddress.Loopback, EndpointPort, 200, "{}", "application/json"))
         {
            string routed = "reply@" + _domainName;
            Create(routed);

            string tagged = "reply+ticket4711@" + _domainName;
            SmtpClientSimulator.StaticSend("outsider@example.net", tagged, "Re: your ticket", "Thanks.");

            List<string> requests = WaitForRequests(endpoint, 1);
            Assert.AreEqual(1, requests.Count);

            Dictionary<string, FormPart> form = ParseForm(requests[0], endpoint.RequestBodies[0]);
            StringAssert.Contains(tagged, form["envelope"].Text, "The tag is what the application reads: " + form["envelope"].Text);
            Assert.AreEqual(routed, form["route"].Text);
         }
      }

      [Test]
      [Description("The JSON form carries the parsed parts, the verdicts, the attachment in base64 and, asked for, the raw message in base64 - signed the same way.")]
      public void TheJsonFormCarriesTheAttachmentAndTheRawMessage()
      {
         string address = "json@" + _domainName;

         using (var endpoint = new FakeHttpEndpoint(IPAddress.Loopback, EndpointPort, 200, "{}", "application/json"))
         {
            Create(address, "\"format\":\"json\",\"include_raw\":true");

            byte[] attachment = AttachmentBytes();
            SmtpClientSimulator.StaticSendRaw("outsider@example.net", address, MessageWithAttachment(address, "A JSON one", attachment));

            List<string> requests = WaitForRequests(endpoint, 1);
            Assert.AreEqual(1, requests.Count);

            byte[] body = endpoint.RequestBodies[0];
            StringAssert.Contains("Content-Type: application/json", requests[0], requests[0]);
            Assert.AreEqual("sha256=" + Hmac(Secret, Header(requests[0], "X-hMailServer-Timestamp"), body),
               Header(requests[0], "X-hMailServer-Signature"));

            using (JsonDocument document = JsonDocument.Parse(Encoding.UTF8.GetString(body)))
            {
               JsonElement root = document.RootElement;

               Assert.AreEqual("message.inbound", root.GetProperty("event").GetString());
               Assert.AreEqual(address, root.GetProperty("route").GetString());
               Assert.AreEqual("A JSON one", root.GetProperty("subject").GetString());
               StringAssert.Contains("The parcel was due on Monday.", root.GetProperty("text").GetString());
               Assert.AreEqual(address, root.GetProperty("envelope").GetProperty("to")[0].GetString());
               Assert.IsTrue(root.GetProperty("verdicts").TryGetProperty("spam", out _), "The spam verdict travels, whatever it is.");

               JsonElement first = root.GetProperty("attachments")[0];
               Assert.AreEqual("receipt.pdf", first.GetProperty("filename").GetString());
               Assert.AreEqual(attachment.Length, first.GetProperty("size").GetInt32());
               CollectionAssert.AreEqual(attachment, Convert.FromBase64String(first.GetProperty("content").GetString()));

               string raw = Encoding.UTF8.GetString(Convert.FromBase64String(root.GetProperty("raw").GetString()));
               StringAssert.Contains("Subject: A JSON one", raw, "The raw message is the message as it arrived.");
            }
         }
      }

      [Test]
      [Description("A route that names attachments only lists them - name, type and size - and carries none of their bytes.")]
      public void NamesOnlyLeavesTheBytesOut()
      {
         string address = "names@" + _domainName;

         using (var endpoint = new FakeHttpEndpoint(IPAddress.Loopback, EndpointPort, 200, "{}", "application/json"))
         {
            Create(address, "\"attachments\":\"names\"");

            byte[] attachment = AttachmentBytes();
            SmtpClientSimulator.StaticSendRaw("outsider@example.net", address, MessageWithAttachment(address, "Names only", attachment));

            List<string> requests = WaitForRequests(endpoint, 1);
            Assert.AreEqual(1, requests.Count);

            Dictionary<string, FormPart> form = ParseForm(requests[0], endpoint.RequestBodies[0]);
            Assert.AreEqual("1", form["attachments"].Text);
            StringAssert.Contains("\"filename\":\"receipt.pdf\"", form["attachment-info"].Text);
            StringAssert.Contains("\"size\":" + attachment.Length, form["attachment-info"].Text);
            Assert.IsFalse(form.ContainsKey("attachment1"), "The file itself is left out.");
         }
      }

      [Test]
      [Description("A route that leaves attachments out lists none and says how many it left out; the form carries the raw message as email when asked, cc, the delivery queue's id and the connecting address.")]
      public void NoneLeavesTheAttachmentsOutAndSaysHowMany()
      {
         string address = "none@" + _domainName;

         using (var endpoint = new FakeHttpEndpoint(IPAddress.Loopback, EndpointPort, 200, "{}", "application/json"))
         {
            Create(address, "\"attachments\":\"none\",\"include_raw\":true");

            string message = MessageWithAttachment(address, "None of them", AttachmentBytes())
               .Replace("To: " + address + "\r\n", "To: " + address + "\r\nCc: someone.else@example.net\r\n");
            SmtpClientSimulator.StaticSendRaw("outsider@example.net", address, message);

            List<string> requests = WaitForRequests(endpoint, 1);
            Assert.AreEqual(1, requests.Count);

            Dictionary<string, FormPart> form = ParseForm(requests[0], endpoint.RequestBodies[0]);
            Assert.AreEqual("0", form["attachments"].Text, "None carried.");
            Assert.AreEqual("1", form["attachments_left_out"].Text, "And how many were left out.");
            Assert.IsFalse(form.ContainsKey("attachment-info"), "None listed either.");
            Assert.IsFalse(form.ContainsKey("attachment1"));
            StringAssert.Contains("Subject: None of them", form["email"].Text, "The message as it arrived, as email.");
            StringAssert.Contains("someone.else@example.net", form["cc"].Text);
            Assert.IsTrue(Regex.IsMatch(form["queue_id"].Text, "^[0-9]+$"), "queue_id: " + form["queue_id"].Text);
            Assert.IsTrue(IPAddress.TryParse(form["sender_ip"].Text, out _), "sender_ip: " + form["sender_ip"].Text);
         }
      }

      [Test]
      [Description("The text fields arrive decoded to UTF-8 whatever the message was written in; a verdict a sender wrote into the message - an Authentication-Results field in this server's name, a reason score - never travels as this server's; and an attachment's name with a backslash in it cannot leave a quoted-string open.")]
      public void TheFieldsAreDecodedAndOnlyThisServersVerdictsTravel()
      {
         string address = "decoded@" + _domainName;

         using (var endpoint = new FakeHttpEndpoint(IPAddress.Loopback, EndpointPort, 200, "{}", "application/json"))
         {
            Create(address);

            // The authserv-id this server writes (AuthenticationResults::GetAuthservId,
            // the machine's name while none is configured), so that the forgery is
            // one the server would be fooled by if it read a field it did not write.
            string authservId = Environment.MachineName.ToLowerInvariant();

            var message = new StringBuilder();
            message.Append("From: Someone <outsider@example.net>\r\n");
            message.Append("To: " + address + "\r\n");
            message.Append("Subject: =?utf-8?B?" + Convert.ToBase64String(Encoding.UTF8.GetBytes("Gr\u00fc\u00dfe aus K\u00f6ln")) + "?=\r\n");
            message.Append("Authentication-Results: " + authservId + "; spf=pass smtp.mailfrom=example.net; dkim=pass header.d=example.net; dmarc=pass header.from=example.net\r\n");
            message.Append("X-hMailServer-Reason-Score: 7\r\n");
            message.Append("MIME-Version: 1.0\r\n");
            message.Append("Content-Type: multipart/mixed; boundary=\"outer-boundary\"\r\n");
            message.Append("\r\n");
            message.Append("--outer-boundary\r\n");
            message.Append("Content-Type: text/plain; charset=iso-8859-1\r\n");
            message.Append("Content-Transfer-Encoding: quoted-printable\r\n");
            message.Append("\r\n");
            message.Append("Un caf=E9, s'il vous pla=EEt.\r\n");
            message.Append("--outer-boundary\r\n");
            message.Append("Content-Type: text/plain; name=\"C:\\temp\\notes.txt\"\r\n");
            message.Append("Content-Disposition: attachment; filename=\"C:\\temp\\notes.txt\"\r\n");
            message.Append("\r\n");
            message.Append("Some notes.\r\n");
            message.Append("--outer-boundary--\r\n");

            SmtpClientSimulator.StaticSendRaw("outsider@example.net", address, message.ToString());

            List<string> requests = WaitForRequests(endpoint, 1);
            Assert.AreEqual(1, requests.Count);

            Dictionary<string, FormPart> form = ParseForm(requests[0], endpoint.RequestBodies[0]);

            Assert.AreEqual("Gr\u00fc\u00dfe aus K\u00f6ln", form["subject"].Text, "An encoded word, decoded to UTF-8.");
            StringAssert.Contains("Un caf\u00e9, s'il vous pla\u00eet.", form["text"].Text, "Latin-1, decoded to UTF-8.");
            StringAssert.Contains("UTF-8", form["charsets"].Text);

            // Written by the sender, in this server's name: none of it is this
            // server's verdict (AuthenticationResultsEnabled is off, as it ships,
            // so the server wrote no field at all), and a score is carried only for
            // a message the server itself classified as spam.
            Assert.IsFalse(form.ContainsKey("SPF"), "SPF: " + (form.ContainsKey("SPF") ? form["SPF"].Text : ""));
            Assert.IsFalse(form.ContainsKey("dkim"), "dkim");
            Assert.IsFalse(form.ContainsKey("dmarc"), "dmarc");
            Assert.IsFalse(form.ContainsKey("spam_score"), "spam_score");

            // The part's own header is well formed: its filename has no
            // backslash left in it to escape the closing quote.
            Assert.IsTrue(form.ContainsKey("attachment1"), "The attachment is carried.");
            Assert.IsFalse(form["attachment1"].FileName.Contains("\\"), "filename=\"" + form["attachment1"].FileName + "\"");
            StringAssert.Contains("Some notes.", form["attachment1"].Text);
         }
      }

      [Test]
      [Description("An endpoint that refuses is retried on the outbound webhooks' schedule and the message is then kept: it can be downloaded, and tried again once the endpoint is put right - from no attempts, with the same body every time - and each of those is on the audit trail. A delivery is removed with DELETE.")]
      public void ARefusedMessageIsKeptDownloadedAndRetried()
      {
         Http("PUT", "/api/v1/settings", "{\"webhook_max_attempts\":2}");

         string address = "kept@" + _domainName;

         using (var endpoint = new FakeHttpEndpoint(IPAddress.Loopback, EndpointPort, 500, "{}", "application/json"))
         {
            Create(address);

            SmtpClientSimulator.StaticSend("outsider@example.net", address, "Keep this one", "A body worth keeping.");

            string kept = RunUntil(RoutesPath + "/deliveries?limit=50", "\"state\":\"kept\"");
            StringAssert.Contains("\"attempts\":2", kept, kept);
            StringAssert.Contains("\"message_kept\":true", kept, kept);
            StringAssert.Contains("answered 500", kept, "The last failure is on the row: " + kept);

            CustomAsserts.AssertReportedError("HM7401", "is kept");

            string id = Extract(kept, "id");

            (int status, string message) = Http("GET", RoutesPath + "/deliveries/" + id + "/message");
            Assert.AreEqual(200, status, message);
            StringAssert.Contains("Subject: Keep this one", message, "The message as it arrived.");

            int before = endpoint.Requests.Count;
            endpoint.SetResponse(200, "{}");

            (int retried, string retryBody) = Http("POST", RoutesPath + "/deliveries/" + id + "/retry", "{}");
            Assert.AreEqual(200, retried, retryBody);

            string delivered = RunUntil(RoutesPath + "/deliveries?limit=50", "\"state\":\"delivered\"");
            StringAssert.Contains("\"message_kept\":false", delivered, delivered);

            List<string> requests = WaitForRequests(endpoint, before + 1);
            StringAssert.Contains("X-hMailServer-Attempt: 1", requests[requests.Count - 1],
               "A retry by hand starts the attempts again from none.");

            // Every attempt posted the same bytes: the body is built from the copy
            // alone, its boundary from the message and its time from when it was
            // taken. Only the timestamp header and the signature over it change.
            List<byte[]> bodies = endpoint.RequestBodies;
            Assert.IsTrue(bodies.Count >= 3, bodies.Count + " request(s)");
            CollectionAssert.AreEqual(bodies[0], bodies[bodies.Count - 1], "A retry is the same body.");

            (int gone, string goneBody) = Http("GET", RoutesPath + "/deliveries/" + id + "/message");
            Assert.AreEqual(404, gone, "A delivered message's copy is not kept: " + goneBody);

            List<JsonElement> events = Audit("inbound webhook delivery");
            Assert.IsTrue(events.Any(e => e.GetProperty("action").GetString() == "downloaded"), "The download is on the audit trail: " + Describe(events));
            Assert.IsTrue(events.Any(e => e.GetProperty("action").GetString() == "retried"), "The retry is on the audit trail: " + Describe(events));

            (int removed, string removedBody) = Http("DELETE", RoutesPath + "/deliveries/" + id);
            Assert.AreEqual(200, removed, removedBody);
            StringAssert.DoesNotContain("\"id\":" + id + ",", Read(RoutesPath + "/deliveries?limit=50"), "A delivery removed is gone from the queue.");
            Assert.IsTrue(Audit("inbound webhook delivery").Any(e => e.GetProperty("action").GetString() == "removed"), "The removal is on the audit trail.");
         }
      }

      [Test]
      [Description("A route that returns what it could not post sends the sender the delivery-status report every failed delivery sends, 4.4.7, without the endpoint's answer or its address; the returned delivery keeps no message.")]
      public void AReturningRouteSendsTheSenderAReport()
      {
         Http("PUT", "/api/v1/settings", "{\"webhook_max_attempts\":1}");

         string sender = "sender@" + _domainName;
         SingletonProvider<TestSetup>.Instance.AddAccount(CurrentDomain(), sender, AccountPassword);

         string address = "returns@" + _domainName;

         using (var endpoint = new FakeHttpEndpoint(IPAddress.Loopback, EndpointPort, 503, "{\"detail\":\"internal-endpoint-words\"}", "application/json"))
         {
            Create(address, "\"on_failure\":\"bounce\"");

            SmtpClientSimulator.StaticSend(sender, address, "Return this one", "A body.");

            string bounced = RunUntil(RoutesPath + "/deliveries?limit=50", "\"state\":\"bounced\"");
            StringAssert.Contains("\"message_kept\":false", bounced, "A returned message's copy is gone: " + bounced);

            CustomAsserts.AssertReportedError("HM7402", "returned to its sender");

            string report = Pop3ClientSimulator.AssertGetFirstMessageText(sender, AccountPassword);
            StringAssert.Contains("4.4.7", report, report);
            StringAssert.Contains(address, report, report);

            // What the endpoint answered and where it is are not the sender's
            // business: not its body, not the failure this server recorded ("the
            // endpoint answered 503"), not its URL. The report's copy of the
            // original's headers names 127.0.0.1 as the sending host, so the
            // endpoint is looked for by its port and its path.
            StringAssert.DoesNotContain("internal-endpoint-words", report, report);
            StringAssert.DoesNotContain("answered 503", report, report);
            StringAssert.DoesNotContain(":" + EndpointPort, report, report);
            StringAssert.DoesNotContain("/inbound", report, report);
         }
      }

      [Test]
      [Description("A sender who asked, with DSN NOTIFY=NEVER, not to be told of a failure is not told when a returning route gives up: the delivery is returned, and no report is sent.")]
      public void AReturnedMessageIsNotReportedToASenderWhoAskedNotToBeTold()
      {
         Http("PUT", "/api/v1/settings", "{\"webhook_max_attempts\":1}");

         string sender = "quiet-sender@" + _domainName;
         SingletonProvider<TestSetup>.Instance.AddAccount(CurrentDomain(), sender, AccountPassword);

         string address = "returns-quietly@" + _domainName;

         using (var endpoint = new FakeHttpEndpoint(IPAddress.Loopback, EndpointPort, 503, "{}", "application/json"))
         {
            Create(address, "\"on_failure\":\"bounce\"");

            SendWithNotify(sender, address, "NEVER", "Do not tell me", "A body.");

            RunUntil(RoutesPath + "/deliveries?limit=50", "\"state\":\"bounced\"");
            CustomAsserts.AssertReportedError("HM7402", "not to be told");

            // AReturningRouteSendsTheSenderAReport is the same message without the
            // NOTIFY, and its sender is sent the report.
            Pop3ClientSimulator.AssertMessageCount(sender, AccountPassword, 0);
         }
      }

      [Test]
      [Description("A message larger than a returning route's ceiling is not posted: it is returned at once, 5.3.4.")]
      public void AMessageOverTheCeilingIsReturnedAtOnce()
      {
         string sender = "big-sender@" + _domainName;
         SingletonProvider<TestSetup>.Instance.AddAccount(CurrentDomain(), sender, AccountPassword);

         string address = "small@" + _domainName;

         using (var endpoint = new FakeHttpEndpoint(IPAddress.Loopback, EndpointPort, 200, "{}", "application/json"))
         {
            Create(address, "\"on_failure\":\"bounce\",\"max_size_kb\":1");

            SmtpClientSimulator.StaticSend(sender, address, "Too large", string.Join("\r\n", Enumerable.Repeat(new string('x', 70), 60)));

            string report = Pop3ClientSimulator.AssertGetFirstMessageText(sender, AccountPassword);
            StringAssert.Contains("5.3.4", report, report);

            Assert.AreEqual(0, endpoint.Requests.Count, "A message over the ceiling is never posted.");
         }
      }

      [Test]
      [Description("A returning route that delivers as well still delivers a message over its ceiling to the mailbox: only the post is refused, and a sender whose message reached the mailbox is not told it failed.")]
      public void ARouteThatDeliversAsWellStillDeliversAMessageOverItsCeiling()
      {
         string sender = "big-sender2@" + _domainName;
         SingletonProvider<TestSetup>.Instance.AddAccount(CurrentDomain(), sender, AccountPassword);

         string address = "small-but-delivered@" + _domainName;
         SingletonProvider<TestSetup>.Instance.AddAccount(CurrentDomain(), address, AccountPassword);

         using (var endpoint = new FakeHttpEndpoint(IPAddress.Loopback, EndpointPort, 200, "{}", "application/json"))
         {
            Create(address, "\"on_failure\":\"bounce\",\"max_size_kb\":1,\"deliver\":true");

            SmtpClientSimulator.StaticSend(sender, address, "Too large to post", string.Join("\r\n", Enumerable.Repeat(new string('x', 70), 60)));

            Pop3ClientSimulator.AssertMessageCount(address, AccountPassword, 1);
            Pop3ClientSimulator.AssertMessageCount(sender, AccountPassword, 0);
            Assert.AreEqual(0, endpoint.Requests.Count, "A message over the ceiling is never posted.");
         }
      }

      [Test]
      [Description("An endpoint that accepts with a 2xx and a long answer - longer than the 16 KB of it that is read - has accepted: the message is posted once and delivered, not taken for no answer and posted again.")]
      public void ALongAnswerThatAcceptsIsAnAcceptance()
      {
         string address = "chatty@" + _domainName;
         string longAnswer = "<html><body>" + string.Join("", Enumerable.Repeat("<p>Thank you for your message. It has been received.</p>", 800)) + "</body></html>";
         Assert.IsTrue(longAnswer.Length > 40000);

         using (var endpoint = new FakeHttpEndpoint(IPAddress.Loopback, EndpointPort, 200, longAnswer, "text/html"))
         {
            Create(address);

            SmtpClientSimulator.StaticSend("outsider@example.net", address, "Say thank you", "A body.");

            string delivered = RunUntil(RoutesPath + "/deliveries?limit=50", "\"state\":\"delivered\"");
            StringAssert.Contains("\"attempts\":1", delivered, delivered);
            Assert.AreEqual(1, endpoint.Requests.Count, "Posted once.");
         }
      }

      [Test]
      [Description("An endpoint that takes the connection and never answers holds up neither the mail this server accepts - SMTP answers every message while the pass waits on it - nor another route, whose message is posted after one wait on the silent endpoint, not after its whole queue.")]
      public void ASilentEndpointHoldsUpNeitherMailNorAnotherRoute()
      {
         const int stallSeconds = 10;

         string silent = "silent@" + _domainName;
         string other = "other@" + _domainName;

         // More messages than the asynchronous queue has threads: a drain that
         // waited for the running pass would take a thread each, and the end of
         // DATA of the messages after them would wait with them.
         int threads = _application.Settings.MaxAsynchronousThreads;
         int messages = Math.Min(Math.Max(threads, 15) + 5, 40);

         using (var endpoint = new FakeHttpEndpoint(IPAddress.Loopback, EndpointPort, 200, "{}", "application/json"))
         using (var second = new FakeHttpEndpoint(IPAddress.Loopback, SecondEndpointPort, 200, "{}", "application/json"))
         {
            endpoint.SetStall("/inbound", stallSeconds);

            Create(silent);
            Create(other, null, SecondEndpointUrl);

            double slowest = 0;
            for (int i = 0; i < messages; i++)
            {
               Stopwatch watch = Stopwatch.StartNew();
               SmtpClientSimulator.StaticSend("outsider@example.net", silent, "Message " + i, "A body.");
               slowest = Math.Max(slowest, watch.Elapsed.TotalSeconds);
            }

            Assert.IsTrue(slowest < stallSeconds / 2.0,
               "A message to the silent route waited " + slowest.ToString("0.0") + " seconds for its 250: the passes' drains held the threads SMTP finishes a message on.");

            SmtpClientSimulator.StaticSend("outsider@example.net", other, "To the other route", "A body.");

            Assert.IsTrue(WaitUntil(() => second.Requests.Count >= 1, 4 * stallSeconds + 10),
               "The other route's message was not posted while the silent endpoint held its queue.");

            Assert.AreEqual(other, ParseForm(second.Requests[0], second.RequestBodies[0])["route"].Text);
         }
      }

      [Test]
      [Description("A route switched off with more due deliveries than a pass reads holds up no other route: its rows are not read while it is off, and another route's message is posted at once.")]
      public void ASwitchedOffRoutesQueueHoldsUpNoOtherRoute()
      {
         // One attempt, so that each message is kept at once and can be made due
         // again by hand while its route is off.
         Http("PUT", "/api/v1/settings", "{\"webhook_max_attempts\":1}");

         const int rows = 55;   // more than the 50 due rows one pass reads

         string held = "held@" + _domainName;
         string other = "unheld@" + _domainName;
         string heldPath = RoutesPath + "/" + Uri.EscapeDataString(held);

         using (var endpoint = new FakeHttpEndpoint(IPAddress.Loopback, EndpointPort, 500, "{}", "application/json"))
         using (var second = new FakeHttpEndpoint(IPAddress.Loopback, SecondEndpointPort, 200, "{}", "application/json"))
         {
            Create(held);

            for (int i = 0; i < rows; i++)
               SmtpClientSimulator.StaticSend("outsider@example.net", held, "Held " + i, "A body.");

            string kept = "";
            Assert.IsTrue(WaitUntil(() =>
            {
               Http("POST", RoutesPath + "/run", "{}");
               kept = Read(RoutesPath + "/deliveries?state=kept&limit=1000&route=" + Uri.EscapeDataString(held));
               return Count(kept, "\"state\":\"kept\"") >= rows;
            }, 120), "Not every message was kept: " + Count(kept, "\"state\":\"kept\""));

            CustomAsserts.AssertReportedError("HM7401", "is kept");

            (int status, string body) off = Http("PUT", heldPath, "{\"enabled\":false}");
            Assert.AreEqual(200, off.status, off.body);

            // Every kept row made due again, while its route is off.
            foreach (Match id in Regex.Matches(kept, "\"id\":([0-9]+)"))
            {
               (int retried, string retryBody) = Http("POST", RoutesPath + "/deliveries/" + id.Groups[1].Value + "/retry", "{}");
               Assert.AreEqual(200, retried, retryBody);
            }

            StringAssert.Contains("\"pending\":" + rows, Read(heldPath), "Every row due, and held while the route is off.");

            Create(other, null, SecondEndpointUrl);
            SmtpClientSimulator.StaticSend("outsider@example.net", other, "Not held", "A body.");

            Assert.IsTrue(WaitUntil(() =>
            {
               Http("POST", RoutesPath + "/run", "{}");
               return second.Requests.Count >= 1;
            }, 30), "The other route's message stood behind the rows of a route that is off.");

            StringAssert.Contains("\"pending\":" + rows, Read(heldPath), "The route that is off posted nothing.");
         }
      }

      [Test]
      [Description("A route is refused for a domain this server does not host, plain http off this machine, an empty secret, a URL with a space or a line break in it, a size ceiling past 81920 KB, a name that exists, and - for the server's administrator too - a link-local or cloud metadata address, an IPv4 one carried in IPv6 in each of its forms included.")]
      public void TheRoutesRefuseWhatTheyMust()
      {
         (int status, string body) notHosted = Http("POST", RoutesPath,
            "{\"address\":\"someone@not-hosted.example\",\"url\":\"" + EndpointUrl + "\",\"secret\":\"" + Secret + "\"}");
         Assert.AreEqual(400, notHosted.status, notHosted.body);
         StringAssert.Contains("does not host", notHosted.body);

         (int status, string body) clear = Http("POST", RoutesPath,
            "{\"address\":\"a@" + _domainName + "\",\"url\":\"http://receiver.example/hook\",\"secret\":\"" + Secret + "\"}");
         Assert.AreEqual(400, clear.status, clear.body);
         StringAssert.Contains("https", clear.body);

         (int status, string body) noSecret = Http("POST", RoutesPath,
            "{\"address\":\"a@" + _domainName + "\",\"url\":\"" + EndpointUrl + "\",\"secret\":\"\"}");
         Assert.AreEqual(400, noSecret.status, noSecret.body);
         StringAssert.Contains("secret", noSecret.body);

         // A space or a line break in the path would end the request line and
         // start a header of the URL's own making.
         foreach (string url in new[] { EndpointUrl + "?a=b c", EndpointUrl + "\\r\\nX-Injected: 1", EndpointUrl + "\\tx" })
         {
            (int status, string body) control = Http("POST", RoutesPath,
               "{\"address\":\"a@" + _domainName + "\",\"url\":\"" + url + "\",\"secret\":\"" + Secret + "\"}");
            Assert.AreEqual(400, control.status, url + ": " + control.body);
            StringAssert.Contains("control character", control.body, url + ": " + control.body);
         }

         // A character outside ASCII has no one spelling in a request line, and is
         // refused on every platform: read as a signed char it was refused on Linux
         // as a control character, and passed on Windows.
         (int status, string body) accented = Http("POST", RoutesPath,
            "{\"address\":\"a@" + _domainName + "\",\"url\":\"" + EndpointUrl + "/caf\u00e9\",\"secret\":\"" + Secret + "\"}");
         Assert.AreEqual(400, accented.status, accented.body);
         StringAssert.Contains("printable ASCII", accented.body);

         // No more than the 80 MB the message parser takes.
         (int status, string body) tooLarge = Http("POST", RoutesPath,
            "{\"address\":\"a@" + _domainName + "\",\"url\":\"" + EndpointUrl + "\",\"secret\":\"" + Secret + "\",\"max_size_kb\":81921}");
         Assert.AreEqual(400, tooLarge.status, tooLarge.body);
         StringAssert.Contains("81920", tooLarge.body);

         foreach (string url in new[] { "https://169.254.169.254/latest/meta-data", "https://[fe80::1]/hook", "https://100.100.100.200/hook",
                                        "https://168.63.129.16/hook", "https://192.0.0.192/hook", "https://[fd00:ec2::254]/hook",
                                        "https://[::ffff:169.254.169.254]/hook", "https://[::169.254.169.254]/hook",
                                        "https://[64:ff9b::a9fe:a9fe]/hook", "https://[2002:a9fe:a9fe::1]/hook" })
         {
            (int status, string body) metadata = Http("POST", RoutesPath,
               "{\"address\":\"a@" + _domainName + "\",\"url\":\"" + url + "\",\"secret\":\"" + Secret + "\"}");
            Assert.AreEqual(400, metadata.status, url + ": " + metadata.body);
            StringAssert.Contains("link-local or cloud metadata", metadata.body, url + ": " + metadata.body);
         }

         Create("a@" + _domainName, "\"max_size_kb\":81920");

         (int status, string body) twice = Http("POST", RoutesPath,
            "{\"address\":\"a@" + _domainName + "\",\"url\":\"" + EndpointUrl + "\",\"secret\":\"" + Secret + "\"}");
         Assert.AreEqual(409, twice.status, twice.body);

         (int status, string body) unknown = Http("POST", RoutesPath,
            "{\"address\":\"b@" + _domainName + "\",\"url\":\"" + EndpointUrl + "\",\"secret\":\"" + Secret + "\",\"forward_to\":\"x\"}");
         Assert.AreEqual(400, unknown.status, unknown.body);

         string listing = Read(RoutesPath);
         StringAssert.Contains("\"private_network\":true", listing, "The administrator's route may reach this server's own network: " + listing);
         StringAssert.Contains("\"max_size_kb\":81920", listing, "81920 KB is the most a route takes, and taken: " + listing);
      }

      [Test]
      [Description("A shared secret is never handed back; the answer says only whether one is set, and an empty one in a change keeps the one there is. A route switched off takes no new mail: an address that has nothing else is refused at RCPT TO.")]
      public void TheSecretIsNeverReadBack()
      {
         string address = "secret@" + _domainName;

         using (var endpoint = new FakeHttpEndpoint(IPAddress.Loopback, EndpointPort, 200, "{}", "application/json"))
         {
            Create(address);

            string listing = Read(RoutesPath);
            StringAssert.DoesNotContain(Secret, listing);
            StringAssert.Contains("\"secret_set\":true", listing, listing);

            // On: RCPT TO takes the address, which has no mailbox.
            SmtpClientSimulator.StaticSend("outsider@example.net", address, "While on", "A body.");
            Assert.AreEqual(1, WaitForRequests(endpoint, 1).Count);

            (int status, string body) unchanged = Http("PUT", RoutesPath + "/" + Uri.EscapeDataString(address), "{\"enabled\":false,\"secret\":\"\"}");
            Assert.AreEqual(200, unchanged.status, unchanged.body);
            StringAssert.Contains("\"secret_set\":true", unchanged.body, unchanged.body);
            StringAssert.Contains("\"enabled\":false", unchanged.body, unchanged.body);
            StringAssert.DoesNotContain(Secret, unchanged.body);

            // Off: the address is what it is with no route, which is nobody's.
            Assert.Throws<DeliveryFailedException>(() => SmtpClientSimulator.StaticSend("outsider@example.net", address, "While off", "A body."),
               "A route switched off still took an address nobody has.");
            Assert.AreEqual(1, endpoint.Requests.Count, "Nothing was posted for the refused message.");
         }
      }

      [Test]
      [Description("An API key whose role holds manage_inbound_webhooks, confined to one domain, routes that domain's addresses to a public address and nothing else: its own network is refused without saying where a name leads, another domain is refused, it is listed its own routes and not another domain's, another domain's delivery is the same 404 as none, and it cannot run the pass - which the same role unconfined can. A route it saves is held to public addresses from then on, and its address cannot change. A reader without the permission is answered the URL's scheme and host alone, and a kept message only with view_mailbox.")]
      public void AKeyRoutesItsOwnDomainToAPublicAddressOnly()
      {
         string otherDomain = "inbound-other.test";
         SingletonProvider<TestSetup>.Instance.AddDomain(otherDomain);

         using (var endpoint = new FakeHttpEndpoint(IPAddress.Loopback, EndpointPort, 200, "{}", "application/json"))
         {
            Create("someone@" + otherDomain);

            // A delivery of the other domain, for the confined key to be refused.
            SmtpClientSimulator.StaticSend("outsider@example.net", "someone@" + otherDomain, "Elsewhere", "A body.");
            string otherQueue = RunUntil(RoutesPath + "/deliveries?limit=50&route=" + Uri.EscapeDataString("someone@" + otherDomain), "\"state\":\"delivered\"");
            string otherId = Extract(otherQueue, "id");

            MakeRole("inboundtest-app", "view_configuration,manage_inbound_webhooks");
            string key = MakeKey("inbound app", null, "inboundtest-app", _domainName);

            (int status, string body) loopback = AsKey(key, "POST", RoutesPath,
               "{\"address\":\"app@" + _domainName + "\",\"url\":\"" + EndpointUrl + "\",\"secret\":\"" + Secret + "\"}");
            Assert.AreEqual(400, loopback.status, loopback.body);
            StringAssert.Contains("public address", loopback.body, loopback.body);

            (int status, string body) privateRange = AsKey(key, "POST", RoutesPath,
               "{\"address\":\"app@" + _domainName + "\",\"url\":\"https://10.20.30.40/hook\",\"secret\":\"" + Secret + "\"}");
            Assert.AreEqual(400, privateRange.status, privateRange.body);
            StringAssert.Contains("public address", privateRange.body, privateRange.body);

            // A name that leads inside: refused, without saying where it leads.
            (int status, string body) byName = AsKey(key, "POST", RoutesPath,
               "{\"address\":\"app@" + _domainName + "\",\"url\":\"https://localhost/hook\",\"secret\":\"" + Secret + "\"}");
            Assert.AreEqual(400, byName.status, byName.body);
            StringAssert.Contains("public address", byName.body, byName.body);
            StringAssert.DoesNotContain("127.0.0.1", byName.body, "Where a name resolves is not told to a key: " + byName.body);
            StringAssert.DoesNotContain("::1", byName.body, byName.body);

            // A public literal, so that nothing is resolved; no message is ever sent
            // to this route, so nothing is ever posted to it.
            (int status, string body) made = AsKey(key, "POST", RoutesPath,
               "{\"address\":\"app@" + _domainName + "\",\"url\":\"https://8.8.8.8/inbound\",\"secret\":\"" + Secret + "\"}");
            Assert.AreEqual(201, made.status, made.body);
            StringAssert.Contains("\"private_network\":false", made.body, made.body);
            StringAssert.Contains("https://8.8.8.8/inbound", made.body, "A credential that may change the route is answered its URL whole: " + made.body);

            (int status, string body) elsewhere = AsKey(key, "POST", RoutesPath,
               "{\"address\":\"app@" + otherDomain + "\",\"url\":\"https://8.8.8.8/inbound\",\"secret\":\"" + Secret + "\"}");
            Assert.AreEqual(403, elsewhere.status, elsewhere.body);

            (int status, string body) listing = AsKey(key, "GET", RoutesPath, null);
            Assert.AreEqual(200, listing.status, listing.body);
            StringAssert.Contains("app@" + _domainName, listing.body, listing.body);
            StringAssert.DoesNotContain(otherDomain, listing.body, "A confined key is listed its own domains' routes: " + listing.body);

            (int status, string body) other = AsKey(key, "GET", RoutesPath + "/" + Uri.EscapeDataString("someone@" + otherDomain), null);
            Assert.AreEqual(403, other.status, other.body);

            (int status, string body) queue = AsKey(key, "GET", RoutesPath + "/deliveries?limit=50", null);
            Assert.AreEqual(200, queue.status, queue.body);
            StringAssert.DoesNotContain(otherDomain, queue.body, "A confined key is read its own routes' deliveries: " + queue.body);

            // Another domain's delivery is the same 404 as none - and is left as
            // it is.
            (int status, string body) otherRetry = AsKey(key, "POST", RoutesPath + "/deliveries/" + otherId + "/retry", "{}");
            Assert.AreEqual(404, otherRetry.status, "Another domain's delivery is the same 404 as none: " + otherRetry.body);
            (int status, string body) otherRemove = AsKey(key, "DELETE", RoutesPath + "/deliveries/" + otherId, null);
            Assert.AreEqual(404, otherRemove.status, otherRemove.body);
            StringAssert.Contains("\"id\":" + otherId + ",", Read(RoutesPath + "/deliveries?limit=50"), "Another domain's delivery was removed by a key confined elsewhere.");

            (int status, string body) run = AsKey(key, "POST", RoutesPath + "/run", "{}");
            Assert.AreEqual(403, run.status, run.body);

            // The same role reaching every domain may run the pass: the refusal is
            // the confinement's, not the role's.
            string everywhere = MakeKey("inbound app everywhere", null, "inboundtest-app");
            (int status, string body) runEverywhere = AsKey(everywhere, "POST", RoutesPath + "/run", "{}");
            Assert.AreEqual(200, runEverywhere.status, runEverywhere.body);

            // A route the administrator made may reach this server's network; the
            // key re-saving it holds it to public addresses from then on, and
            // cannot move it to another address.
            string desk = "desk2@" + _domainName;
            Create(desk);
            StringAssert.Contains("\"private_network\":true", Read(RoutesPath + "/" + Uri.EscapeDataString(desk)));

            (int status, string body) resaved = AsKey(key, "PUT", RoutesPath + "/" + Uri.EscapeDataString(desk), "{\"url\":\"https://8.8.8.8/desk\"}");
            Assert.AreEqual(200, resaved.status, resaved.body);
            StringAssert.Contains("\"private_network\":false", resaved.body, resaved.body);

            (int status, string body) moved = AsKey(key, "PUT", RoutesPath + "/" + Uri.EscapeDataString(desk), "{\"address\":\"desk3@" + _domainName + "\"}");
            Assert.AreEqual(400, moved.status, moved.body);

            // A reader who may not change the routes: the URL's scheme and host,
            // never its path, where a receiver's token lives; and a kept message
            // only beside view_mailbox.
            MakeRole("inboundtest-reader", "view_configuration");
            string reader = MakeKey("inbound reader", null, "inboundtest-reader");

            (int status, string body) read = AsKey(reader, "GET", RoutesPath, null);
            Assert.AreEqual(200, read.status, read.body);
            StringAssert.Contains("https://8.8.8.8/...", read.body, read.body);
            StringAssert.DoesNotContain("8.8.8.8/inbound", read.body, "A reader is not answered the path: " + read.body);
            StringAssert.DoesNotContain("10181/inbound", read.body, read.body);

            (int status, string body) readMessage = AsKey(reader, "GET", RoutesPath + "/deliveries/" + otherId + "/message", null);
            Assert.AreEqual(403, readMessage.status, "A kept message is the mail itself, and needs view_mailbox: " + readMessage.body);

            string auditor = MakeKey("inbound auditor", "readonly", null);

            (int status, string body) auditorRead = AsKey(auditor, "GET", RoutesPath, null);
            Assert.AreEqual(200, auditorRead.status, auditorRead.body);

            (int status, string body) write = AsKey(auditor, "POST", RoutesPath,
               "{\"address\":\"auditor@" + _domainName + "\",\"url\":\"https://8.8.8.8/inbound\",\"secret\":\"" + Secret + "\"}");
            Assert.AreEqual(403, write.status, write.body);
         }
      }

      [Test]
      [Description("A route made and removed is on the audit trail under its address both times - the removal too - and its URL is recorded as changed, never by its value.")]
      public void TheAuditTrailNamesARouteByItsAddressAndNeverItsUrl()
      {
         string address = "audited@" + _domainName;

         Create(address);
         Assert.AreEqual(200, Http("DELETE", RoutesPath + "/" + Uri.EscapeDataString(address)).status);

         List<JsonElement> entries = Audit("inbound webhook");
         string all = Describe(entries);

         Assert.IsTrue(entries.Any(e => e.GetProperty("action").GetString() == "created" && e.GetProperty("object").GetString().Contains(address)),
            "The route made, under its address: " + all);
         Assert.IsTrue(entries.Any(e => e.GetProperty("action").GetString() == "deleted" && e.GetProperty("object").GetString().Contains(address)),
            "The route removed, under its address and not a row number: " + all);
         StringAssert.DoesNotContain(EndpointPort + "/inbound", all, "The URL is never recorded by its value: " + all);
         StringAssert.DoesNotContain(Secret, all);
      }

      [Test]
      [Description("An erasure of an address takes the deliveries it sent - with the messages they kept - out of the inbound webhooks' queue.")]
      public void AnErasureTakesWhatTheAddressSentOutOfTheQueue()
      {
         Http("PUT", "/api/v1/settings", "{\"webhook_max_attempts\":1}");

         string erased = "forget-me@example.net";
         string address = "erasure@" + _domainName;

         using (var endpoint = new FakeHttpEndpoint(IPAddress.Loopback, EndpointPort, 500, "{}", "application/json"))
         {
            Create(address);

            SmtpClientSimulator.StaticSend(erased, address, "Forget me", "A body.");

            string kept = RunUntil(RoutesPath + "/deliveries?limit=50", "\"state\":\"kept\"");
            CustomAsserts.AssertReportedError("HM7401", "is kept");
            string id = Extract(kept, "id");
            StringAssert.Contains(erased, kept, kept);

            (int status, string body) erasure = Http("POST", "/api/v1/addresses/erase", "{\"address\":\"" + erased + "\",\"include_archive\":false}");
            Assert.AreEqual(200, erasure.status, erasure.body);

            string after = Read(RoutesPath + "/deliveries?limit=50");
            StringAssert.DoesNotContain(erased, after, "The erased address is still in the queue: " + after);

            (int gone, string goneBody) = Http("GET", RoutesPath + "/deliveries/" + id + "/message");
            Assert.AreEqual(404, gone, "The message the delivery kept went with it: " + goneBody);
         }
      }

      [Test]
      [Description("A domain removed takes its routes with it, so that a domain made again under the name finds none.")]
      public void ARouteGoesWithItsDomain()
      {
         string doomed = "inbound-doomed.test";
         Domain domain = SingletonProvider<TestSetup>.Instance.AddDomain(doomed);

         Create("someone@" + doomed);
         Create(doomed);
         StringAssert.Contains(doomed, Read(RoutesPath));

         domain.Delete();

         StringAssert.DoesNotContain(doomed, Read(RoutesPath), "A route outlived its domain.");
      }

      [Test]
      [Description("WebhooksEnabled, the outbound webhooks' master switch, does not silence an inbound route.")]
      public void TheOutboundSwitchDoesNotSilenceARoute()
      {
         string address = "unsilenced@" + _domainName;

         using (var endpoint = new FakeHttpEndpoint(IPAddress.Loopback, EndpointPort, 200, "{}", "application/json"))
         {
            Create(address);

            Assert.AreEqual(200, Http("PUT", "/api/v1/settings", "{\"webhooks_enabled\":false}").status);

            SmtpClientSimulator.StaticSend("outsider@example.net", address, "Still posted", "A body.");
            Assert.AreEqual(1, WaitForRequests(endpoint, 1).Count, "WebhooksEnabled off silenced an inbound route.");
         }
      }

      // ----------------------------------------------------------------- helpers

      // The test domain as the server has it now: the COM object SetUp was given
      // belongs to the configuration the reinitialise replaced.
      private Domain CurrentDomain()
      {
         return _application.Domains.get_ItemByName(_domainName);
      }

      private sealed class FormPart
      {
         public string Headers = "";
         public string FileName = "";
         public byte[] Bytes = new byte[0];

         public string Text
         {
            get { return Encoding.UTF8.GetString(Bytes); }
         }
      }

      // multipart/form-data, read as a receiver reads it: split at the boundary
      // the Content-Type names, each part's name and filename out of its
      // Content-Disposition. Latin-1 maps each byte to one character, so the
      // bytes of a part come back unchanged.
      private static Dictionary<string, FormPart> ParseForm(string request, byte[] body)
      {
         string contentType = Header(request, "Content-Type");
         int at = contentType.IndexOf("boundary=", StringComparison.Ordinal);
         Assert.IsTrue(at > 0, "No boundary in: " + contentType);

         string delimiter = "--" + contentType.Substring(at + 9).Trim();
         string text = Latin1.GetString(body);

         var parts = new Dictionary<string, FormPart>();

         int position = text.IndexOf(delimiter, StringComparison.Ordinal);
         while (position >= 0)
         {
            int start = position + delimiter.Length;
            if (text.Length >= start + 2 && text.Substring(start, 2) == "--")
               break;

            start += 2;
            int headerEnd = text.IndexOf("\r\n\r\n", start, StringComparison.Ordinal);
            Assert.IsTrue(headerEnd > 0, "A part with no end to its headers.");

            int next = text.IndexOf("\r\n" + delimiter, headerEnd + 4, StringComparison.Ordinal);
            Assert.IsTrue(next > 0, "A part that is never closed.");

            var part = new FormPart();
            part.Headers = text.Substring(start, headerEnd - start);
            part.Bytes = Latin1.GetBytes(text.Substring(headerEnd + 4, next - headerEnd - 4));

            Match name = Regex.Match(part.Headers, "name=\"([^\"]*)\"");
            Match fileName = Regex.Match(part.Headers, "filename=\"([^\"]*)\"");
            if (fileName.Success)
               part.FileName = fileName.Groups[1].Value;

            Assert.IsTrue(name.Success, "A part with no name: " + part.Headers);
            parts[name.Groups[1].Value] = part;

            position = next + 2;
         }

         return parts;
      }

      private static byte[] AttachmentBytes()
      {
         // Bytes a text conversion would damage: a high byte, a NUL and a CR.
         return Encoding.ASCII.GetBytes("%PDF-1.4\n%").Concat(new byte[] { 0xC7, 0xEC, 0x8F, 0xA2, 0x00, 0x0D, 0x0A })
            .Concat(Encoding.ASCII.GetBytes("1 0 obj\n<</Type/Catalog>>\nendobj\n%%EOF\n")).ToArray();
      }

      private static string MessageWithAttachment(string to, string subject, byte[] attachment)
      {
         var message = new StringBuilder();
         message.Append("From: Someone <outsider@example.net>\r\n");
         message.Append("To: " + to + "\r\n");
         message.Append("Subject: " + subject + "\r\n");
         message.Append("MIME-Version: 1.0\r\n");
         message.Append("Content-Type: multipart/mixed; boundary=\"outer-boundary\"\r\n");
         message.Append("\r\n");
         message.Append("--outer-boundary\r\n");
         message.Append("Content-Type: multipart/alternative; boundary=\"inner-boundary\"\r\n");
         message.Append("\r\n");
         message.Append("--inner-boundary\r\n");
         message.Append("Content-Type: text/plain; charset=utf-8\r\n");
         message.Append("\r\n");
         message.Append("The parcel was due on Monday.\r\n");
         message.Append("--inner-boundary\r\n");
         message.Append("Content-Type: text/html; charset=utf-8\r\n");
         message.Append("\r\n");
         message.Append("<p>The parcel was due on <b>Monday</b>.</p>\r\n");
         message.Append("--inner-boundary--\r\n");
         message.Append("--outer-boundary\r\n");
         message.Append("Content-Type: application/pdf; name=\"receipt.pdf\"\r\n");
         message.Append("Content-Disposition: attachment; filename=\"receipt.pdf\"\r\n");
         message.Append("Content-Transfer-Encoding: base64\r\n");
         message.Append("\r\n");
         message.Append(Convert.ToBase64String(attachment) + "\r\n");
         message.Append("--outer-boundary--\r\n");
         return message.ToString();
      }

      // One message over SMTP with a DSN NOTIFY on its one RCPT TO (RFC 3461).
      private static void SendWithNotify(string from, string to, string notify, string subject, string body)
      {
         using (var connection = new TcpConnection())
         {
            Assert.IsTrue(connection.Connect(TestPorts.Smtp), "No SMTP service on port " + TestPorts.Smtp);
            StringAssert.StartsWith("220", connection.Receive());

            connection.Send("EHLO example.net\r\n");
            connection.ReadUntil("250 HELP");

            StringAssert.StartsWith("250", connection.SendAndReceive("MAIL FROM:<" + from + ">\r\n"));
            StringAssert.StartsWith("250", connection.SendAndReceive("RCPT TO:<" + to + "> NOTIFY=" + notify + "\r\n"));
            StringAssert.StartsWith("354", connection.SendAndReceive("DATA\r\n"));

            string accepted = connection.SendAndReceive("From: " + from + "\r\nTo: " + to + "\r\nSubject: " + subject + "\r\n\r\n" + body + "\r\n.\r\n");
            StringAssert.StartsWith("250", accepted);

            connection.Send("QUIT\r\n");
            connection.Disconnect();
         }
      }

      private void Create(string address, string extra = null, string url = null)
      {
         string body = "{\"address\":\"" + address + "\",\"url\":\"" + (url ?? EndpointUrl) + "\",\"secret\":\"" + Secret + "\"" +
                       (string.IsNullOrEmpty(extra) ? "" : "," + extra) + "}";

         (int status, string body) answer = Http("POST", RoutesPath, body);
         Assert.AreEqual(201, answer.status, answer.body);
      }

      // Every route removed, each removal checked: a route of the test domain
      // left behind would take every later fixture's mail for that domain. The
      // domain's own removal takes its routes with it as well (ARouteGoesWithItsDomain),
      // which is what PerformBasicSetup does between fixtures.
      private void RemoveEveryRoute()
      {
         (int status, string body) listing = Http("GET", RoutesPath);
         Assert.AreEqual(200, listing.status, "The routes could not be listed to be removed: " + listing.body);

         foreach (Match match in Regex.Matches(listing.body, "\"address\":\"([^\"]+)\""))
         {
            (int status, string body) removed = Http("DELETE", RoutesPath + "/" + Uri.EscapeDataString(match.Groups[1].Value));
            Assert.AreEqual(200, removed.status, "The route for " + match.Groups[1].Value + " was not removed: " + removed.body);
         }
      }

      private void MakeRole(string name, string permissions)
      {
         (int status, string body) role = Http("POST", "/api/v1/roles",
            "{\"name\":\"" + name + "\",\"description\":\"inbound webhook fixture\",\"permissions\":\"" + permissions + "\"}");
         Assert.AreEqual(201, role.status, role.body);
         _roles.Add(name);
      }

      private string MakeKey(string label, string scope, string role, string domains = null)
      {
         var body = new StringBuilder("{\"label\":\"" + label + "\"");
         if (scope != null)
            body.Append(",\"scope\":\"" + scope + "\"");
         if (role != null)
            body.Append(",\"role\":\"" + role + "\"");
         if (domains != null)
            body.Append(",\"domains\":\"" + domains + "\"");
         body.Append("}");

         (int status, string answer) created = Http("POST", "/api/v1/apikeys", body.ToString());
         Assert.AreEqual(201, created.status, created.answer);

         // Keys go before the roles they hold, which is the order they are made in.
         _keys.Insert(0, Extract(created.answer, "id"));

         Match key = Regex.Match(created.answer, "\"key\"\\s*:\\s*\"([^\"]+)\"");
         Assert.IsTrue(key.Success, "No key in: " + created.answer);
         return key.Groups[1].Value;
      }

      // The audit trail's entries of one object type, newest first.
      private static List<JsonElement> Audit(string objectType)
      {
         (int status, string body) answer = Http("GET", "/api/v1/audit?limit=200&object_type=" + Uri.EscapeDataString(objectType));
         Assert.AreEqual(200, answer.status, answer.body);

         using (JsonDocument document = JsonDocument.Parse(answer.body))
         {
            return document.RootElement.GetProperty("entries").EnumerateArray()
               .Where(e => e.GetProperty("object_type").GetString() == objectType)
               .Select(e => e.Clone()).ToList();
         }
      }

      private static string Describe(List<JsonElement> entries)
      {
         return string.Join(" | ", entries.Select(e => e.GetRawText()));
      }

      // Runs a pass until the listing at path says what is wanted, and answers it.
      private string RunUntil(string path, string wanted)
      {
         string last = "";

         for (int attempt = 0; attempt < 40; attempt++)
         {
            Http("POST", RoutesPath + "/run", "{}");

            last = Read(path);
            if (last.Contains(wanted))
               return last;

            Thread.Sleep(250);
         }

         Assert.Fail("Never " + wanted + ": " + last);
         return last;
      }

      private static bool WaitUntil(Func<bool> condition, int seconds)
      {
         DateTime until = DateTime.UtcNow.AddSeconds(seconds);

         while (DateTime.UtcNow < until)
         {
            if (condition())
               return true;

            Thread.Sleep(250);
         }

         return condition();
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

      private static (int status, string body) AsKey(string key, string method, string path, string requestBody)
      {
         return RestRequest.Send("127.0.0.1", RestPort, method, path, "Bearer " + key, requestBody);
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

      // Waits for the drain, and runs a pass while it waits: the first attempt is
      // made as soon as the message is queued, and a pass asked for here finds it
      // due if the drain has not got to it yet.
      private List<string> WaitForRequests(FakeHttpEndpoint endpoint, int wanted)
      {
         for (int attempt = 0; attempt < 100; attempt++)
         {
            List<string> requests = endpoint.Requests;
            if (requests.Count >= wanted)
               return requests;

            if (attempt % 10 == 9)
               Http("POST", RoutesPath + "/run", "{}");

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

      private static string Hmac(string key, string timestamp, byte[] body)
      {
         byte[] signed = Encoding.ASCII.GetBytes(timestamp + ".").Concat(body).ToArray();

         using (var hmac = new HMACSHA256(Encoding.UTF8.GetBytes(key)))
         {
            byte[] hash = hmac.ComputeHash(signed);
            var text = new StringBuilder();
            foreach (byte value in hash)
               text.Append(value.ToString("x2"));

            return text.ToString();
         }
      }
   }
}
