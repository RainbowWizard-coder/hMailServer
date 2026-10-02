// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
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
   ///    Web Push: a phone is told about new mail with the webmail's page closed.
   ///
   ///    The browser is stood in for here by a key pair the server's own
   ///    Utilities.GenerateWebPushKeyPair makes and by a FakeHttpEndpoint that
   ///    plays the push service, so what is asserted is what a real push service
   ///    would receive: the TTL and Content-Encoding headers RFC 8030 and RFC 8291
   ///    require, an Authorization header whose VAPID token verifies under the
   ///    public key it names and whose audience is the push service's origin, and
   ///    a body that opens with the subscription's own private key to exactly the
   ///    JSON the documentation promises - and nothing the person did not allow.
   ///
   ///    The tests that matter most are about NOT sending: quiet hours, a sender
   ///    the device is not for, the feature switched off. A notification that
   ///    arrives at two in the morning is the one that gets the feature turned off
   ///    for good.
   /// </summary>
   [TestFixture]
   public class RestApiWebPush : TestFixtureBase
   {
      private static int RestPort = 9590;
      private const string AdminPassword = "testar";
      private const string UserPassword = "Push-Passw0rd!";
      private const string MePath = "/api/v1/me/push";
      private const string SettingsPath = "/api/v1/settings/push";

      private Account _account;
      private Account _sender;

      private string Address
      {
         get { return _account.Address; }
      }

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(AdminPassword);
         _account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "push@" + _domain.Name, UserPassword);
         _sender = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "from@" + _domain.Name, UserPassword);

         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();

         (int status, string body) probe = Http("GET", "/api/v1/status", AdminHeader());
         Assert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);

         // On, with a fresh key pair, so that the tests below start from a server
         // that can send - to the FakeHttpEndpoint on 127.0.0.1, which is plain
         // http to this machine's loopback and taken only while the suite's own
         // switch is on (loopback_http_for_tests). TearDown puts all three back.
         (int status, string body) on = Http("PUT", SettingsPath, AdminHeader(), "{\"enabled\":true,\"loopback_http_for_tests\":true}");
         Assert.AreEqual(200, on.status, on.body);

         (int status, string body) pair = Http("POST", SettingsPath + "/vapid", AdminHeader(), "{}");
         Assert.AreEqual(200, pair.status, pair.body);
         StringAssert.Contains("\"vapid_public_key\":\"B", pair.body,
            "A VAPID public key is a 65-octet uncompressed P-256 point, which base64url always starts with B. Body: " + pair.body);
      }

      [TearDown]
      public void StopRestApi()
      {
         // Off, and the key pair cleared - a setting left behind here would let every
         // fixture after this one queue pushes for a service that is not there.
         Http("PUT", SettingsPath, AdminHeader(), "{\"enabled\":false,\"loopback_http_for_tests\":false,\"vapid_private_key\":\"\"}");

         RestListener.Stop();
         _application.Reinitialize();
      }

      [Test]
      [Description("A device subscribes and is listed by its name and its push service's origin, never by its endpoint or keys; removing it removes it.")]
      public void ASubscriptionIsStoredAndRemoved()
      {
         using (var service = new FakeHttpEndpoint(201, ""))
         {
            Device device = Device.Make(this, service.UrlFor("/push/first-device"));

            long id = Subscribe(device, "\"device\":\"Pixel in the kitchen\"");

            string listing = Read(MePath, UserHeader());
            StringAssert.Contains("\"enabled\":true", listing);
            StringAssert.Contains("\"device\":\"Pixel in the kitchen\"", listing);
            StringAssert.Contains("\"service\":\"http://127.0.0.1:" + service.Port + "\"", listing,
               "A row is recognised by the push service's origin, which is all of the endpoint a person needs. Body: " + listing);
            StringAssert.Contains("\"state\":\"active\"", listing);
            StringAssert.Contains("\"detail\":1", listing, "The sender, and not the subject, is what a device is told by default.");

            StringAssert.DoesNotContain("first-device", listing,
               "The endpoint is the capability to notify the device and must never come back out. Body: " + listing);
            StringAssert.DoesNotContain(device.PublicKey, listing, "The p256dh key came back in a listing.");
            StringAssert.DoesNotContain(device.Auth, listing, "The auth secret came back in a listing.");

            Assert.AreEqual(1, PushSubscriptionCountOverCom(), "Account.PushSubscriptionCount does not see the row.");

            // The same endpoint again is the same device, not a second one.
            long again = Subscribe(device, "\"device\":\"Pixel, renamed\"");
            Assert.AreEqual(id, again, "A device subscribing again under the same endpoint must replace its own row.");
            StringAssert.Contains("\"device\":\"Pixel, renamed\"", Read(MePath, UserHeader()));
            Assert.AreEqual(1, Count(Read(MePath, UserHeader()), "\"device\":"), "One endpoint became two rows.");

            (int status, string body) removed = Http("DELETE", MePath + "/subscriptions/" + id, UserHeader());
            Assert.AreEqual(200, removed.status, removed.body);
            StringAssert.Contains("\"removed\":true", removed.body);
            StringAssert.Contains("\"subscriptions\":[]", Read(MePath, UserHeader()));

            (int status, string body) gone = Http("DELETE", MePath + "/subscriptions/" + id, UserHeader());
            Assert.AreEqual(404, gone.status, gone.body);
         }
      }

      [Test]
      [Description("A key that is not the shape RFC 8291 needs is refused before it is stored, as is a choice outside its range.")]
      public void AMalformedSubscriptionIsRefused()
      {
         using (var service = new FakeHttpEndpoint(201, ""))
         {
            Device device = Device.Make(this, service.UrlFor("/push/bad"));

            (int status, string body) shortKey = Http("POST", MePath + "/subscriptions", UserHeader(),
               "{\"endpoint\":\"" + device.Endpoint + "\",\"keys\":{\"p256dh\":\"AAAA\",\"auth\":\"" + device.Auth + "\"},\"device\":\"x\"}");
            Assert.AreEqual(400, shortKey.status, shortKey.body);
            StringAssert.Contains("p256dh", shortKey.body);

            (int status, string body) badQuiet = Http("POST", MePath + "/subscriptions", UserHeader(),
               device.SubscriptionJson("\"device\":\"x\",\"quiet_from\":1500,\"quiet_to\":60"));
            Assert.AreEqual(400, badQuiet.status, badQuiet.body);
            StringAssert.Contains("quiet hours", badQuiet.body);

            (int status, string body) notHttps = Http("POST", MePath + "/subscriptions", UserHeader(),
               "{\"endpoint\":\"http://push.example.net/anything\",\"keys\":{\"p256dh\":\"" + device.PublicKey + "\",\"auth\":\"" + device.Auth + "\"},\"device\":\"x\"}");
            Assert.AreEqual(400, notHttps.status, notHttps.body);
            StringAssert.Contains("https", notHttps.body,
               "Plain http is accepted to a loopback address only - the token in the Authorization header must not cross a network in the clear.");

            StringAssert.Contains("\"subscriptions\":[]", Read(MePath, UserHeader()), "A refused subscription was stored.");
         }
      }

      [Test]
      [Description("An endpoint outside printable ASCII is refused, and one percent-encoded as a browser gives it is taken. The endpoint's hash, which the unique index and every lookup are on, was made over AnsiString(endpoint): two endpoints of one length that differed only in a letter above U+00FF shared it, so the second replaced the first.")]
      public void AnEndpointOutsidePrintableAsciiIsRefused()
      {
         using (var service = new FakeHttpEndpoint(201, ""))
         {
            // The same path as a browser serialises it: percent-encoded UTF-8.
            Device encoded = Device.Make(this, service.UrlFor("/push/%D0%BA%D0%BB%D1%8E%D1%87"));
            Subscribe(encoded, "\"device\":\"percent-encoded\"");

            string[] refusedEndpoints =
            {
               "https://push.example.net/\u043a\u043b\u044e\u0447-\u0430",
               "https://push.example.net/\u043a\u043b\u044e\u0447-\u0431",
               "https://push.example.net/caf\u00e9",
               "https://push.example.net/a b"
            };

            for (int i = 0; i < refusedEndpoints.Length; i++)
            {
               Device device = Device.Make(this, refusedEndpoints[i]);
               (int status, string body) refused = Http("POST", MePath + "/subscriptions", UserHeader(),
                  device.SubscriptionJson("\"device\":\"refused-" + i + "\""));
               Assert.AreEqual(400, refused.status, refusedEndpoints[i] + ": " + refused.body);
               StringAssert.Contains("printable ASCII", refused.body, refused.body);
            }

            string listed = Read(MePath, UserHeader());
            StringAssert.Contains("\"device\":\"percent-encoded\"", listed, listed);
            StringAssert.DoesNotContain("refused-", listed, "A refused subscription was stored: " + listed);
         }
      }

      [Test]
      [Description("A push carries the headers RFC 8030 and RFC 8291 require, a VAPID token that verifies under the key it names for the push service's origin, and a body that opens to exactly the promised JSON.")]
      public void APushCarriesTheRightHeadersAndAVerifiableSignature()
      {
         using (var service = new FakeHttpEndpoint(201, ""))
         {
            Device device = Device.Make(this, service.UrlFor("/push/signed"));
            long id = Subscribe(device, "\"device\":\"Signed\"");

            (int status, string body) sent = Http("POST", MePath + "/test", UserHeader(), "{\"id\":" + id + "}");
            Assert.AreEqual(200, sent.status, sent.body);
            StringAssert.Contains("\"sent\":true", sent.body);

            string request = OneRequest(service);
            Assert.IsNotNull(request, "The push service was never called.");

            StringAssert.StartsWith("POST /push/signed HTTP/1.", request, request);
            StringAssert.Contains("Content-Encoding: aes128gcm", request, "RFC 8291: the body is aes128gcm and the header says so.");
            StringAssert.Contains("Content-Type: application/octet-stream", request, request);
            Assert.AreEqual("86400", Header(request, "TTL"), "The TTL header carries WebPushTimeToLiveSeconds, 86400 by default.");
            Assert.AreEqual("normal", Header(request, "Urgency"));

            string authorization = Header(request, "Authorization");
            StringAssert.StartsWith("vapid t=", authorization, "RFC 8292: 'vapid t=<token>, k=<key>'. Got: " + authorization);
            Match parts = Regex.Match(authorization, @"^vapid t=([^,\s]+),\s*k=(\S+)$");
            Assert.IsTrue(parts.Success, "The Authorization header is not the RFC 8292 form: " + authorization);

            string token = parts.Groups[1].Value;
            string key = parts.Groups[2].Value;

            string serverKey = Extract(Read(MePath, UserHeader()), "vapid_public_key");
            Assert.AreEqual(serverKey, key, "The k= parameter must be the server's own public key, which is the one the browser subscribed with.");

            string[] jwt = token.Split('.');
            Assert.AreEqual(3, jwt.Length, "A JWT is three parts: " + token);

            string header = Encoding.UTF8.GetString(Base64Url.Decode(jwt[0]));
            string claims = Encoding.UTF8.GetString(Base64Url.Decode(jwt[1]));
            StringAssert.Contains("\"alg\":\"ES256\"", header, header);
            StringAssert.Contains("\"aud\":\"http://127.0.0.1:" + service.Port + "\"", claims,
               "The aud claim is the origin of the endpoint and nothing more: " + claims);
            StringAssert.Contains("\"sub\":\"mailto:", claims, "A push service must be able to reach a person: " + claims);

            long expiry = long.Parse(Extract(claims, "exp"));
            long now = DateTimeOffset.UtcNow.ToUnixTimeSeconds();
            Assert.IsTrue(expiry > now && expiry <= now + 24 * 3600, "RFC 8292 bounds exp at 24 hours from now. exp=" + expiry + " now=" + now);

            Assert.IsTrue(VerifyEs256(jwt[0] + "." + jwt[1], Base64Url.Decode(jwt[2]), Base64Url.Decode(key)),
               "The VAPID token's signature does not verify under the public key its own header names.");

            byte[] body = OneBody(service);
            Assert.IsNotNull(body, "The push service received no body.");
            Assert.IsTrue(body.Length > 21 + 16, "An aes128gcm body is a header of at least 21 octets and a record with a 16-octet tag; got " + body.Length);

            string plain = Decrypt(body, device);
            StringAssert.Contains("\"v\":1", plain, plain);
            StringAssert.Contains("\"kind\":\"mail\"", plain, plain);
            StringAssert.Contains("\"test\":true", plain, "A test push says it is a test, so the worker can word it as one: " + plain);
            StringAssert.DoesNotContain("\"from\"", plain, "A test push is about no message and names nobody: " + plain);
            StringAssert.DoesNotContain("\"subject\"", plain, plain);

            StringAssert.Contains("\"last_sent\":", Read(MePath, UserHeader()));
            Assert.AreNotEqual("0", Extract(Read(MePath, UserHeader()), "last_sent"), "A delivered push marks the row.");
         }
      }

      [Test]
      [Description("New mail is announced with exactly what the device is allowed: a count alone at level 0, the sender at 1, the sender and the subject at 2.")]
      public void NewMailIsAnnouncedWithWhatTheDeviceAllows()
      {
         using (var service = new FakeHttpEndpoint(201, ""))
         {
            Device device = Device.Make(this, service.UrlFor("/push/mail"));
            long id = Subscribe(device, "\"device\":\"Count only\",\"detail\":0");

            SmtpClientSimulator.StaticSend(_sender.Address, Address, "The first subject", "Hello.");
            Pop3ClientSimulator.AssertMessageCount(Address, UserPassword, 1);

            byte[] first = OneBody(service);
            Assert.IsNotNull(first, "Mail arrived and no push was sent.");
            string countOnly = Decrypt(first, device);
            StringAssert.Contains("\"count\":1", countOnly, countOnly);
            StringAssert.Contains("\"folder\":\"INBOX\"", countOnly, countOnly);
            StringAssert.DoesNotContain("\"from\"", countOnly, "At level 0 a push carries a count and names nobody: " + countOnly);
            StringAssert.DoesNotContain("The first subject", countOnly, "At level 0 the subject must not leave the server: " + countOnly);
            StringAssert.DoesNotContain("\"test\"", countOnly, countOnly);

            (int status, string body) changed = Http("PUT", MePath + "/subscriptions/" + id, UserHeader(), "{\"detail\":2}");
            Assert.AreEqual(200, changed.status, changed.body);
            StringAssert.Contains("\"detail\":2", changed.body);

            SmtpClientSimulator.StaticSend(_sender.Address, Address, "Lunch on Thursday?", "Hello again.");
            Pop3ClientSimulator.AssertMessageCount(Address, UserPassword, 2);

            byte[] second = NthBody(service, 1);
            Assert.IsNotNull(second, "The second message was delivered and no push followed it.");
            string full = Decrypt(second, device);
            StringAssert.Contains("\"from\":\"" + _sender.Address + "\"", full, full);
            StringAssert.Contains("\"subject\":\"Lunch on Thursday?\"", full, full);

            // Nothing else: the promise in docs/WebPush.md is that these are the only
            // fields, and a body is not "the message" in any form.
            foreach (string key in KeysOf(full))
               Assert.Contains(key, new[] { "v", "kind", "count", "at", "folder", "from", "name", "subject" },
                  "The payload carries a field the documentation does not list: " + key + " in " + full);
         }
      }

      [Test]
      [Description("A push service answering 410 means the subscription is gone: the row is removed rather than retried.")]
      public void A410RemovesTheSubscription()
      {
         using (var service = new FakeHttpEndpoint(201, ""))
         {
            service.SetResponse("/push/gone", 410, new byte[0], "text/plain");

            Device device = Device.Make(this, service.UrlFor("/push/gone"));
            long id = Subscribe(device, "\"device\":\"Expired\"");

            (int status, string body) sent = Http("POST", MePath + "/test", UserHeader(), "{\"id\":" + id + "}");
            Assert.AreEqual(502, sent.status, sent.body);
            StringAssert.Contains("gone", sent.body, sent.body);

            Assert.IsNotNull(OneRequest(service), "The push service was never asked.");
            StringAssert.Contains("\"subscriptions\":[]", Read(MePath, UserHeader()),
               "A subscription the push service says is gone must be removed, not kept and retried.");
            Assert.AreEqual(0, PushSubscriptionCountOverCom());
         }
      }

      [Test]
      [Description("An endpoint that answers with an error is a failure with a next attempt, not a delivery and not a removal.")]
      public void AnEndpointThatFailsIsRetriedLater()
      {
         using (var service = new FakeHttpEndpoint(500, "no"))
         {
            Device device = Device.Make(this, service.UrlFor("/push/failing"));
            long id = Subscribe(device, "\"device\":\"Failing\"");

            (int status, string body) sent = Http("POST", MePath + "/test", UserHeader(), "{\"id\":" + id + "}");
            Assert.AreEqual(502, sent.status, sent.body);

            string listing = Read(MePath, UserHeader());
            StringAssert.Contains("\"device\":\"Failing\"", listing, "A 500 is not 'gone'; the row must stay. Body: " + listing);
            StringAssert.Contains("\"state\":\"active\"", listing, listing);
            StringAssert.Contains("\"failures\":1", listing, listing);
            StringAssert.Contains("answered 500", listing, "The reason is shown to the person: " + listing);
         }
      }

      /// <summary>
      ///    OUT-3. A push endpoint is whatever the person hands over, and the push went
      ///    out with HttpsClient::Request, which reaches any address - an internal
      ///    https host, a metadata address - and the test route answered the
      ///    transport's reason (refused, timed out, a TLS error) word for word: a
      ///    scanner of the network behind the server. An https endpoint is now
      ///    reached only at a public address. Against the old code the listener below
      ///    was connected to, and the answer named the failure; now nothing connects
      ///    and the answer says only that the service could not be reached.
      /// </summary>
      [Test]
      [Description("An https push endpoint at an address inside the network is never connected to, and the reason is not shown to the person.")]
      public void APushEndpointInsideTheNetworkIsNeverReached()
      {
         var listener = new System.Net.Sockets.TcpListener(System.Net.IPAddress.Loopback, 0);
         listener.Start();
         try
         {
            int port = ((System.Net.IPEndPoint) listener.LocalEndpoint).Port;
            Device device = Device.Make(this, "https://127.0.0.1:" + port + "/push/inside");
            long id = Subscribe(device, "\"device\":\"Inside\"");

            (int status, string body) sent = Http("POST", MePath + "/test", UserHeader(), "{\"id\":" + id + "}");
            Assert.AreEqual(502, sent.status, sent.body);
            StringAssert.Contains("could not be reached", sent.body);
            StringAssert.DoesNotContain("127.0.0.1", sent.body, "The answer names the address it was refused for.");
            Assert.IsFalse(listener.Pending(), "The server connected to an https push endpoint on an internal address.");

            string listing = Read(MePath, UserHeader());
            StringAssert.Contains("could not be reached", listing, "The failure is recorded for the device, in the same words.");
            StringAssert.DoesNotContain("not a public address", listing, "The transport's reason reached the person's listing: " + listing);
         }
         finally
         {
            listener.Stop();
         }
      }

      /// <summary>
      ///    Until 6.3.6 a push went straight to the push service whatever HttpProxy
      ///    said, so a server that reaches the web only through its proxy notified
      ///    no device. With HttpProxy set, the proxy is now asked for a tunnel to the
      ///    address the endpoint's host was judged by. The proxy here refuses every
      ///    tunnel, so nothing leaves the machine: 192.88.99.1 is a public address
      ///    that is never connected to. Against the old code the proxy was asked for
      ///    nothing.
      /// </summary>
      [Test]
      [Description("With HttpProxy set, a push to an https endpoint at a public address goes through the proxy, which is asked for a tunnel to that address, and the person is told only that the service could not be reached.")]
      public void APushGoesThroughTheProxy()
      {
         Device device = Device.Make(this, "https://192.88.99.1/push/proxied");
         long id = Subscribe(device, "\"device\":\"Behind a proxy\"");

         using (var proxy = new FakeHttpProxy(true))
         {
            IniFileSetting.Write("HttpProxy", proxy.Address);
            _application.Reinitialize();
            try
            {
               (int status, string body) sent = Http("POST", MePath + "/test", UserHeader(), "{\"id\":" + id + "}");
               Assert.AreEqual(502, sent.status, sent.body);
               StringAssert.Contains("could not be reached", sent.body);
               StringAssert.DoesNotContain("192.88.99.1", sent.body, "The transport's reason reached the person.");
               CollectionAssert.AreEqual(new[] { "192.88.99.1:443" }, proxy.Targets,
                  "The proxy is asked for one tunnel, to the address the endpoint's host was judged by.");
            }
            finally
            {
               // Removed before the proxy is, so that no retry of it goes out
               // without one.
               Http("DELETE", MePath + "/subscriptions/" + id, UserHeader());
               IniFileSetting.Delete("HttpProxy");
               _application.Reinitialize();
            }
         }
      }

      /// <summary>
      ///    OUT-3. Plain http to this machine's loopback is the push fixture's endpoint,
      ///    and a push is sent to it with HttpsClient::Request, which reaches the
      ///    loopback - so it is taken only from a request made on this machine and
      ///    through no proxy. A reverse proxy on the same machine makes every reader's
      ///    request come from the loopback; one that says it forwarded the request
      ///    (X-Forwarded-For, Forwarded) is a reader somewhere else. Against the old
      ///    code both subscriptions below were 201.
      /// </summary>
      [Test]
      [Description("A plain-http loopback push endpoint is refused from a request a proxy forwarded, and taken from one made on this machine.")]
      public void ALoopbackPushEndpointIsTakenFromThisMachineAlone()
      {
         using (var service = new FakeHttpEndpoint(201, ""))
         {
            Device forwarded = Device.Make(this, service.UrlFor("/push/forwarded"));
            (int status, string body) viaProxy = RestRequest.Send("127.0.0.1", RestPort, "POST", MePath + "/subscriptions", UserHeader(),
               forwarded.SubscriptionJson("\"device\":\"Forwarded\""), "X-Forwarded-For: 203.0.113.9");
            Assert.AreEqual(400, viaProxy.status, viaProxy.body);
            StringAssert.Contains("https address", viaProxy.body);

            (int status, string body) viaForwarded = RestRequest.Send("127.0.0.1", RestPort, "POST", MePath + "/subscriptions", UserHeader(),
               forwarded.SubscriptionJson("\"device\":\"Forwarded\""), "Forwarded: for=203.0.113.9");
            Assert.AreEqual(400, viaForwarded.status, viaForwarded.body);
            StringAssert.Contains("\"subscriptions\":[]", Read(MePath, UserHeader()), "Nothing is stored when the endpoint is refused.");

            Subscribe(forwarded, "\"device\":\"Here\"");
         }
      }

      /// <summary>
      ///    OUT-3, what was left: a reverse proxy on this machine that forwards
      ///    without Forwarded, X-Forwarded-For or X-Real-IP (nginx's proxy_pass adds
      ///    none) makes every reader's request one from this machine, so the header
      ///    check alone still took a reader's http://127.0.0.1:&lt;any port&gt;/ and
      ///    POSTed to it. Plain http to loopback is now taken only while the suite's
      ///    own switch is on, which is off on every server in the field; and a device
      ///    saved while it was on is not reached once it is off. Against the old code
      ///    the first subscription below was a 201, and the last push reached the
      ///    endpoint.
      /// </summary>
      [Test]
      [Description("With the test switch off, a plain-http loopback push endpoint is refused even from this machine, and one already stored is not reached.")]
      public void ALoopbackPushEndpointNeedsTheTestSwitch()
      {
         using (var service = new FakeHttpEndpoint(201, ""))
         {
            (int status, string body) off = Http("PUT", SettingsPath, AdminHeader(), "{\"loopback_http_for_tests\":false}");
            Assert.AreEqual(200, off.status, off.body);

            Device refused = Device.Make(this, service.UrlFor("/push/refused"));
            (int status, string body) made = Http("POST", MePath + "/subscriptions", UserHeader(), refused.SubscriptionJson("\"device\":\"Refused\""));
            Assert.AreEqual(400, made.status, "A loopback endpoint from this machine with the switch off: " + made.body);
            StringAssert.Contains("https address", made.body);
            StringAssert.Contains("\"subscriptions\":[]", Read(MePath, UserHeader()), "Nothing is stored when the endpoint is refused.");

            // Stored while the switch was on, then the switch goes off.
            (int status, string body) on = Http("PUT", SettingsPath, AdminHeader(), "{\"loopback_http_for_tests\":true}");
            Assert.AreEqual(200, on.status, on.body);
            long id = Subscribe(Device.Make(this, service.UrlFor("/push/stored")), "\"device\":\"Stored\"");
            off = Http("PUT", SettingsPath, AdminHeader(), "{\"loopback_http_for_tests\":false}");
            Assert.AreEqual(200, off.status, off.body);

            (int status, string body) sent = Http("POST", MePath + "/test", UserHeader(), "{\"id\":" + id + "}");
            Assert.AreEqual(502, sent.status, sent.body);
            StringAssert.Contains("could not be reached", sent.body);
            Assert.AreEqual(0, service.CountRequests("POST", "/push/stored"), "A loopback endpoint was reached with the switch off.");
         }
      }

      /// <summary>
      ///    OUT-2. The REST listener has four workers, and a request that waits on
      ///    another server - a test push, a brand logo not yet known, a one-click
      ///    unsubscribe, a WKD lookup - held one for as long as that server took.
      ///    One reader keeping four such requests in flight against a tarpit stalled
      ///    the webmail, the portal and the Deck for everybody. Now an account waits
      ///    on one other server at a time and all callers together on two. Against the
      ///    old code the same account's second request and the third account's were
      ///    admitted - and answered 200 by the quick push service - instead of 429,
      ///    while the two stalled ones each held a worker for twenty seconds.
      /// </summary>
      [Test]
      [Description("An account waits on one other server at a time, and all callers together on half the REST workers at most; the rest are answered 429 at once.")]
      public void RequestsThatReachOutTakeTurns()
      {
         Account second = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "push-second@" + _domain.Name, UserPassword);
         Account third = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "push-third@" + _domain.Name, UserPassword);
         string secondHeader = BasicHeader(second.Address, UserPassword);
         string thirdHeader = BasicHeader(third.Address, UserPassword);

         using (var stalled = new FakeHttpEndpoint(201, ""))
         using (var quick = new FakeHttpEndpoint(201, ""))
         {
            stalled.SetStall("/push/slow", 15);

            long mine = Subscribe(Device.Make(this, stalled.UrlFor("/push/slow")), "\"device\":\"Slow\"");
            long mineQuick = Subscribe(Device.Make(this, quick.UrlFor("/push/mine")), "\"device\":\"Quick\"");

            (int status, string body) madeSecond = Http("POST", MePath + "/subscriptions", secondHeader,
               Device.Make(this, stalled.UrlFor("/push/slow")).SubscriptionJson("\"device\":\"Slow too\""));
            Assert.AreEqual(201, madeSecond.status, madeSecond.body);
            long secondsSlow = long.Parse(Extract(madeSecond.body, "id"));

            (int status, string body) madeThird = Http("POST", MePath + "/subscriptions", thirdHeader,
               Device.Make(this, quick.UrlFor("/push/third")).SubscriptionJson("\"device\":\"Quick too\""));
            Assert.AreEqual(201, madeThird.status, madeThird.body);
            long thirdsQuick = long.Parse(Extract(madeThird.body, "id"));

            var first = System.Threading.Tasks.Task.Run(() => Http("POST", MePath + "/test", UserHeader(), "{\"id\":" + mine + "}"));
            Thread.Sleep(1000);
            var other = System.Threading.Tasks.Task.Run(() => Http("POST", MePath + "/test", secondHeader, "{\"id\":" + secondsSlow + "}"));
            Thread.Sleep(1000);

            (int status, string body) again = Http("POST", MePath + "/test", UserHeader(), "{\"id\":" + mineQuick + "}");
            Assert.AreEqual(429, again.status, "The same account, while it waits on one server: " + again.body);
            StringAssert.Contains("reaching_out_busy", again.body);

            (int status, string body) full = Http("POST", MePath + "/test", thirdHeader, "{\"id\":" + thirdsQuick + "}");
            Assert.AreEqual(429, full.status, "A third account, while two wait on other servers: " + full.body);

            Assert.AreEqual(200, Http("GET", MePath, UserHeader()).status, "A request that reaches nowhere is answered meanwhile.");

            Assert.AreEqual(502, first.Result.status, first.Result.body);
            Assert.AreEqual(502, other.Result.status, other.Result.body);

            (int status, string body) afterwards = Http("POST", MePath + "/test", thirdHeader, "{\"id\":" + thirdsQuick + "}");
            Assert.AreEqual(200, afterwards.status, "Their turns over, the third account is admitted: " + afterwards.body);
         }
      }

      [Test]
      [Description("Inside the device's quiet hours, judged on the device's own clock, mail is not announced; outside them it is.")]
      public void QuietHoursAreHonoured()
      {
         using (var service = new FakeHttpEndpoint(201, ""))
         {
            // A window around now on a device whose clock is three hours ahead of
            // UTC: the offset has to be applied for "now" to fall inside it.
            const int offset = 180;
            int deviceMinute = (int) ((DateTime.UtcNow.TimeOfDay.TotalMinutes + offset) % 1440);
            int from = (deviceMinute + 1440 - 30) % 1440;
            int to = (deviceMinute + 30) % 1440;

            Device device = Device.Make(this, service.UrlFor("/push/quiet"));
            long id = Subscribe(device, "\"device\":\"Asleep\",\"detail\":2,\"quiet_from\":" + from + ",\"quiet_to\":" + to + ",\"quiet_offset\":" + offset);

            SmtpClientSimulator.StaticSend(_sender.Address, Address, "In the night", "Shh.");
            Pop3ClientSimulator.AssertMessageCount(Address, UserPassword, 1);
            AssertNothingArrives(service, "A push was sent inside the device's quiet hours.");

            // The same window read on the server's clock would be sixty minutes off:
            // the offset is what makes it the device's hours and not the server's.
            (int status, string body) awake = Http("PUT", MePath + "/subscriptions/" + id, UserHeader(), "{\"quiet_from\":-1,\"quiet_to\":-1}");
            Assert.AreEqual(200, awake.status, awake.body);

            SmtpClientSimulator.StaticSend(_sender.Address, Address, "In the morning", "Good morning.");
            Pop3ClientSimulator.AssertMessageCount(Address, UserPassword, 2);

            Assert.IsNotNull(OneRequest(service), "With the quiet hours removed the next message must be announced.");
            StringAssert.Contains("In the morning", Decrypt(OneBody(service), device), "The push that arrived is about the wrong message.");
         }
      }

      [Test]
      [Description("A device that is only for certain senders is told about those and no other; the list takes addresses and @domain suffixes.")]
      public void OnlyTheChosenSendersAreAnnounced()
      {
         using (var service = new FakeHttpEndpoint(201, ""))
         {
            Account other = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "other@" + _domain.Name, UserPassword);

            Device device = Device.Make(this, service.UrlFor("/push/senders"));
            Subscribe(device, "\"device\":\"Work only\",\"detail\":1,\"senders\":\"" + _sender.Address + ", @nowhere.test\"");

            SmtpClientSimulator.StaticSend(other.Address, Address, "Not from the list", "Hi.");
            Pop3ClientSimulator.AssertMessageCount(Address, UserPassword, 1);
            AssertNothingArrives(service, "A sender outside the device's list was announced.");

            SmtpClientSimulator.StaticSend(_sender.Address, Address, "From the list", "Hi.");
            Pop3ClientSimulator.AssertMessageCount(Address, UserPassword, 2);

            Assert.IsNotNull(OneRequest(service), "A sender on the device's list was not announced.");
            StringAssert.Contains("\"from\":\"" + _sender.Address + "\"", Decrypt(OneBody(service), device));
         }
      }

      [Test]
      [Description("With Web Push switched off nothing is sent to a device that subscribed while it was on, and nothing new can subscribe.")]
      public void TheFeatureOffMeansNothingIsSent()
      {
         using (var service = new FakeHttpEndpoint(201, ""))
         {
            Device device = Device.Make(this, service.UrlFor("/push/off"));
            long id = Subscribe(device, "\"device\":\"Subscribed while on\"");

            (int status, string body) off = Http("PUT", SettingsPath, AdminHeader(), "{\"enabled\":false}");
            Assert.AreEqual(200, off.status, off.body);
            StringAssert.Contains("\"enabled\":false", Read(MePath, UserHeader()), "The page is told the feature is off.");

            SmtpClientSimulator.StaticSend(_sender.Address, Address, "While off", "Nobody's phone should buzz.");
            Pop3ClientSimulator.AssertMessageCount(Address, UserPassword, 1);
            AssertNothingArrives(service, "Web Push is off and a push was sent.");

            (int status, string body) test = Http("POST", MePath + "/test", UserHeader(), "{\"id\":" + id + "}");
            Assert.AreEqual(502, test.status, "Even a test push is refused while the feature is off. Body: " + test.body);
            AssertNothingArrives(service, "Web Push is off and a test push reached the service.");

            Device another = Device.Make(this, service.UrlFor("/push/off-2"));
            (int status, string body) refused = Http("POST", MePath + "/subscriptions", UserHeader(), another.SubscriptionJson("\"device\":\"Too late\""));
            Assert.AreEqual(409, refused.status, refused.body);

            // The row made while it was on is still there: switching off is not
            // forgetting everybody's choices.
            StringAssert.Contains("\"device\":\"Subscribed while on\"", Read(MePath, UserHeader()));

            (int status, string body) on = Http("PUT", SettingsPath, AdminHeader(), "{\"enabled\":true}");
            Assert.AreEqual(200, on.status, on.body);

            SmtpClientSimulator.StaticSend(_sender.Address, Address, "On again", "Buzz.");
            Pop3ClientSimulator.AssertMessageCount(Address, UserPassword, 2);
            Assert.IsNotNull(OneRequest(service), "Switched on again, the next message must be announced.");
         }
      }

      [Test]
      [Description("The settings group emits the public key and never the private one, accepts a private key and derives the public half from it, and refuses one that is not a P-256 scalar.")]
      public void ThePrivateKeyIsWriteOnlyAndThePublicKeyIsDerived()
      {
         string group = Read(SettingsPath, AdminHeader());
         StringAssert.Contains("\"vapid_public_key\":\"B", group, group);
         StringAssert.DoesNotContain("vapid_private_key", group, "The private key must never be emitted by the settings group: " + group);
         StringAssert.Contains("\"max_devices_per_account\":10", group, group);

         // A pair made by the browser-side helper is a P-256 pair too, so its private
         // half is a valid VAPID key: setting it must derive exactly its public half.
         dynamic utilities = _application.Utilities;
         string pair = utilities.GenerateWebPushKeyPair();
         string privateKey = Extract(pair, "private_key");
         string publicKey = Extract(pair, "public_key");

         (int status, string body) carried = Http("PUT", SettingsPath, AdminHeader(), "{\"vapid_private_key\":\"" + privateKey + "\"}");
         Assert.AreEqual(200, carried.status, carried.body);
         Assert.AreEqual(publicKey, Extract(Read(SettingsPath, AdminHeader()), "vapid_public_key"),
            "Setting the private key must derive the public half, so the two can never disagree.");
         StringAssert.DoesNotContain(privateKey, carried.body, "The private key was echoed back by the PUT.");

         (int status, string body) refused = Http("PUT", SettingsPath, AdminHeader(), "{\"vapid_private_key\":\"not-a-key\"}");
         Assert.AreEqual(400, refused.status, refused.body);

         (int status, string body) cleared = Http("PUT", SettingsPath, AdminHeader(), "{\"vapid_private_key\":\"\"}");
         Assert.AreEqual(200, cleared.status, cleared.body);
         Assert.AreEqual("", Extract(Read(SettingsPath, AdminHeader()), "vapid_public_key"), "Clearing the private key clears the public one with it.");

         using (var service = new FakeHttpEndpoint(201, ""))
         {
            Device device = Device.Make(this, service.UrlFor("/push/no-key"));
            (int status, string body) noKey = Http("POST", MePath + "/subscriptions", UserHeader(), device.SubscriptionJson("\"device\":\"x\""));
            Assert.AreEqual(409, noKey.status, "With no key pair a subscription could never be sent to, and must be refused now rather than fail later. Body: " + noKey.body);
         }
      }

      [Test]
      [Description("An administrator sees how many devices an account has and can stop every one of them, which is recorded in the audit trail; the endpoint and keys are not in that listing either.")]
      public void AnAdministratorSeesTheCountAndStopsEveryDevice()
      {
         using (var service = new FakeHttpEndpoint(201, ""))
         {
            Device phone = Device.Make(this, service.UrlFor("/push/phone"));
            Device laptop = Device.Make(this, service.UrlFor("/push/laptop"));
            Subscribe(phone, "\"device\":\"Phone\"");
            Subscribe(laptop, "\"device\":\"Laptop\"");

            string adminPath = "/api/v1/accounts/" + Uri.EscapeDataString(Address) + "/push";

            string seen = Read(adminPath, AdminHeader());
            StringAssert.Contains("\"count\":2", seen, seen);
            StringAssert.Contains("\"device\":\"Phone\"", seen);
            StringAssert.DoesNotContain("/push/phone", seen, "The administrator's listing carried an endpoint: " + seen);
            StringAssert.DoesNotContain(phone.PublicKey, seen, "The administrator's listing carried a key.");

            (int status, string body) notMine = Http("GET", adminPath, UserHeader());
            Assert.AreEqual(403, notMine.status, "An account credential is not an administrator's. Body: " + notMine.body);

            (int status, string body) stopped = Http("DELETE", adminPath, AdminHeader());
            Assert.AreEqual(200, stopped.status, stopped.body);
            StringAssert.Contains("\"removed\":2", stopped.body, stopped.body);

            StringAssert.Contains("\"subscriptions\":[]", Read(MePath, UserHeader()), "The person's own listing still shows a device.");
            Assert.AreEqual(0, PushSubscriptionCountOverCom());

            string trail = Read("/api/v1/audit?object_type=account&action=push-subscriptions-removed&limit=20", AdminHeader());
            StringAssert.Contains(Address, trail, "Stopping somebody's notifications is an administrative change and must be in the audit trail: " + trail);
            StringAssert.Contains("2 device", trail, trail);

            (int status, string body) nobody = Http("DELETE", "/api/v1/accounts/nobody%40" + _domain.Name + "/push", AdminHeader());
            Assert.AreEqual(404, nobody.status, nobody.body);
         }
      }

      // ---- the browser's half -------------------------------------------------

      /// <summary>
      ///    What pushManager.subscribe hands a page: an endpoint at the push service,
      ///    a P-256 public key and sixteen random octets. The private half stays
      ///    here, as it stays in the browser, and is what opens a push body.
      /// </summary>
      private sealed class Device
      {
         public string Endpoint;
         public string PublicKey;
         public string PrivateKey;
         public string Auth;

         public static Device Make(RestApiWebPush fixture, string endpoint)
         {
            dynamic utilities = fixture._application.Utilities;
            string pair = utilities.GenerateWebPushKeyPair();

            var auth = new byte[16];
            using (var random = RandomNumberGenerator.Create())
               random.GetBytes(auth);

            return new Device
            {
               Endpoint = endpoint,
               PublicKey = Extract(pair, "public_key"),
               PrivateKey = Extract(pair, "private_key"),
               Auth = Base64Url.Encode(auth)
            };
         }

         public string SubscriptionJson(string extra)
         {
            return "{\"endpoint\":\"" + Endpoint + "\",\"keys\":{\"p256dh\":\"" + PublicKey + "\",\"auth\":\"" + Auth + "\"}" +
                   (string.IsNullOrEmpty(extra) ? "" : "," + extra) + "}";
         }
      }

      private long Subscribe(Device device, string extra)
      {
         (int status, string body) made = Http("POST", MePath + "/subscriptions", UserHeader(), device.SubscriptionJson(extra));
         Assert.AreEqual(201, made.status, made.body);
         StringAssert.DoesNotContain(device.PublicKey, made.body, "The answer to a subscription echoed a key back.");
         return long.Parse(Extract(made.body, "id"));
      }

      private string Decrypt(byte[] body, Device device)
      {
         dynamic utilities = _application.Utilities;
         return utilities.WebPushDecrypt(Base64Url.Encode(body), device.PrivateKey, device.Auth);
      }

      private int PushSubscriptionCountOverCom()
      {
         // dynamic because the test project's interop may predate the property.
         dynamic account = _account;
         return (int) account.PushSubscriptionCount;
      }

      /// <summary>
      ///    ES256 as RFC 7518 defines it for a JWS: the signature is r and s, each
      ///    thirty-two octets, which is the form ECDsa.VerifyData takes.
      /// </summary>
      private static bool VerifyEs256(string signingInput, byte[] signature, byte[] publicKey)
      {
         Assert.AreEqual(65, publicKey.Length, "A VAPID public key is a 65-octet uncompressed point.");
         Assert.AreEqual(4, publicKey[0], "An uncompressed point starts with 0x04.");
         Assert.AreEqual(64, signature.Length, "An ES256 signature is r || s, 64 octets.");

         var parameters = new ECParameters
         {
            Curve = ECCurve.NamedCurves.nistP256,
            Q = new ECPoint { X = Slice(publicKey, 1, 32), Y = Slice(publicKey, 33, 32) }
         };

         using (ECDsa ecdsa = ECDsa.Create(parameters))
            return ecdsa.VerifyData(Encoding.ASCII.GetBytes(signingInput), signature, HashAlgorithmName.SHA256);
      }

      private static byte[] Slice(byte[] data, int at, int length)
      {
         var part = new byte[length];
         Array.Copy(data, at, part, 0, length);
         return part;
      }

      private static class Base64Url
      {
         public static string Encode(byte[] data)
         {
            return Convert.ToBase64String(data).TrimEnd('=').Replace('+', '-').Replace('/', '_');
         }

         public static byte[] Decode(string text)
         {
            string padded = text.Replace('-', '+').Replace('_', '/');
            while (padded.Length % 4 != 0)
               padded += "=";
            return Convert.FromBase64String(padded);
         }
      }

      // ---- helpers -----------------------------------------------------------

      private static string AdminHeader()
      {
         return BasicHeader("Administrator", AdminPassword);
      }

      private string UserHeader()
      {
         return BasicHeader(Address, UserPassword);
      }

      private static string BasicHeader(string user, string password)
      {
         return "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(user + ":" + password));
      }

      private static (int status, string body) Http(string method, string path, string authorization, string requestBody = null)
      {
         return RestRequest.Send("127.0.0.1", RestPort, method, path, authorization, requestBody);
      }

      private static string Read(string path, string authorization)
      {
         (int status, string body) answer = Http("GET", path, authorization);
         Assert.AreEqual(200, answer.status, answer.body);
         return answer.body;
      }

      private static string Extract(string json, string key)
      {
         Match match = Regex.Match(json, "\"" + key + "\"\\s*:\\s*\"?([^\",}]*)\"?");
         Assert.IsTrue(match.Success, "No '" + key + "' in: " + json);
         return match.Groups[1].Value;
      }

      private static IEnumerable<string> KeysOf(string json)
      {
         foreach (Match match in Regex.Matches(json, "\"([a-z_]+)\"\\s*:"))
            yield return match.Groups[1].Value;
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

      // A delivery notes the message and posts a drain to the work queue at once,
      // so a push arrives within a second or two; ten seconds is the ceiling.
      private static string OneRequest(FakeHttpEndpoint service)
      {
         for (int attempt = 0; attempt < 100; attempt++)
         {
            List<string> requests = service.Requests;
            if (requests.Count > 0)
               return requests[0];

            Thread.Sleep(100);
         }

         return null;
      }

      private static byte[] OneBody(FakeHttpEndpoint service)
      {
         return NthBody(service, 0);
      }

      private static byte[] NthBody(FakeHttpEndpoint service, int index)
      {
         for (int attempt = 0; attempt < 100; attempt++)
         {
            List<byte[]> bodies = service.RequestBodies;
            if (bodies.Count > index)
               return bodies[index];

            Thread.Sleep(100);
         }

         return null;
      }

      // The negative: the message has been delivered (the caller waited for it), the
      // drain runs at once, and three seconds is long enough for it to have done
      // whatever it was going to do.
      private static void AssertNothingArrives(FakeHttpEndpoint service, string why)
      {
         int before = service.Requests.Count;
         Thread.Sleep(3000);
         Assert.AreEqual(before, service.Requests.Count, why + " Requests: " + string.Join("\n----\n", service.Requests));
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
   }
}
