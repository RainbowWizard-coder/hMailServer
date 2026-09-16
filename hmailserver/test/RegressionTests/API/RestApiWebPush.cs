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
         // that can send. TearDown puts both back.
         (int status, string body) on = Http("PUT", SettingsPath, AdminHeader(), "{\"enabled\":true}");
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
         Http("PUT", SettingsPath, AdminHeader(), "{\"enabled\":false,\"vapid_private_key\":\"\"}");

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
