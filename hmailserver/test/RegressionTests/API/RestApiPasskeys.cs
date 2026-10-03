// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Net.Sockets;
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
   ///    Passkeys (WebAuthn.h, PasskeyStore.h, RestApiPasskeys.cpp): an account enrols a
   ///    credential and signs in with it instead of its password, the administrator does
   ///    the same for the Control Deck, and the four things that must not work do not -
   ///    an assertion collected at another origin, a signature counter that has not moved
   ///    on (a cloned authenticator), a challenge answered twice, and a registration
   ///    without the owner's own password.
   ///
   ///    The authenticator is in this file. A real one is a phone or a security key; what
   ///    it does is arithmetic this fixture can do - build the client data and the
   ///    authenticator data exactly as the specification says, and sign
   ///    authenticatorData || SHA-256(clientDataJSON) with a P-256 key it keeps - so no
   ///    browser and no device are needed to exercise the server's half.
   ///
   ///    The relying party id here is 127.0.0.1, which a browser would refuse (a relying
   ///    party id is a domain name). That is a property of the browser and not of this
   ///    server: what the server does is hash whatever host the page was served on and
   ///    compare it, and the loopback address is the host these tests are served on.
   /// </summary>
   [TestFixture]
   public class RestApiPasskeys : TestFixtureBase
   {
      private static int RestPort = 9736;
      private const string AdminPassword = "testar";
      private const string Password = "Passkey-Passw0rd!";
      private const string Host = "127.0.0.1";
      private const string Origin = "http://127.0.0.1";

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(AdminPassword);
         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();
         Assert.AreEqual(200, Admin("GET", "/api/v1/status", null).status, "REST API did not answer /api/v1/status.");
      }

      [TearDown]
      public void StopRestApi()
      {
         try
         {
            Admin("PUT", "/api/v1/settings",
               "{\"passkeys_enabled\":true,\"passkey_relying_party\":\"\",\"passkey_allowed_origins\":\"\"}");
         }
         finally
         {
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      [Test]
      [Description("An account registers a passkey, signs in with it in place of its password, and removes it; " +
                   "a registration without the account's own password is refused, and the public key is never emitted.")]
      public void AnAccountRegistersAPasskeyAndSignsInWithIt()
      {
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "passkey@" + _domain.Name, Password);
         string me = Basic(account.Address, Password);

         using (var authenticator = new SoftwareAuthenticator())
         {
            // Minting a credential is proven by the person: no password, no passkey.
            (int status, string body) refused = Send("POST", "/api/v1/passkeys", me,
               "{\"name\":\"Phone\",\"credential\":{}}");
            Assert.AreEqual(400, refused.status, refused.body);
            StringAssert.Contains("password is required", refused.body);

            string id = Register(authenticator, me, "Work phone", Password);

            (int status, string body) listed = Send("GET", "/api/v1/passkeys", me, null);
            Assert.AreEqual(200, listed.status, listed.body);
            StringAssert.Contains("\"name\":\"Work phone\"", listed.body);
            StringAssert.Contains("\"rp_id\":\"127.0.0.1\"", listed.body);
            StringAssert.Contains("\"last_used\":0", listed.body);
            StringAssert.DoesNotContain("public_key", listed.body, "The public key is not part of the answer.");
            StringAssert.DoesNotContain(authenticator.PublicKeyBase64Url, listed.body);

            // The sign-in: a challenge, an assertion over it, and the session a
            // password would have started.
            // Read back from the database rather than from the snapshot this
            // fixture has held since AddAccount: the Basic-authenticated calls
            // above have already stamped it, and what is being checked is that
            // the PASSKEY sign-in stamps it too.
            DateTime lastLogonBefore = Convert.ToDateTime(_domain.Accounts.get_ItemByAddress(account.Address).LastLogonTime);
            Thread.Sleep(1100);
            string cookie = SignIn(authenticator);
            (int status, string body) whoami = Send("GET", "/api/v1/me", null, null, "Cookie: " + cookie);
            Assert.AreEqual(200, whoami.status, whoami.body);
            StringAssert.Contains(account.Address, whoami.body);

            // The last-logon stamp moves on, as it does for every other way in.
            // A person who only ever signs in with a passkey must not look to
            // the administrator like somebody who has never signed in at all.
            Assert.AreNotEqual(lastLogonBefore,
               Convert.ToDateTime(_domain.Accounts.get_ItemByAddress(account.Address).LastLogonTime),
               "A passkey sign-in is a logon, and is stamped as one.");

            // Using it is recorded: the counter moves on and the last-used stamp
            // is no longer zero.
            (int status, string body) afterUse = Send("GET", "/api/v1/passkeys", me, null);
            StringAssert.DoesNotContain("\"last_used\":0", afterUse.body, "A passkey that has signed somebody in has been used.");

            // The account's device list names the credential it was.
            StringAssert.Contains("\"credential\":\"passkey\"", Send("GET", "/api/v1/me/devices", me, null).body);

            // Removing one is answered while passkeys are switched off, and that
            // is the point: an administrator who turns them off because a device
            // was lost is at the moment its owner most wants that credential
            // gone. Only the two routes that would ADD one are behind the switch.
            Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"passkeys_enabled\":false}").status);
            Assert.AreEqual(403, Send("POST", "/api/v1/passkeys/options", me, "{}").status,
               "Adding one is behind the switch.");
            Assert.AreEqual(200, Send("DELETE", "/api/v1/passkeys/" + id, me, null).status,
               "Taking a way in away does not depend on the feature being on.");
            Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"passkeys_enabled\":true}").status);

            StringAssert.Contains("\"passkeys\":[]", Send("GET", "/api/v1/passkeys", me, null).body);
            Assert.AreEqual(404, Send("DELETE", "/api/v1/passkeys/" + id, me, null).status, "It is gone.");
         }
      }

      [Test]
      [Description("Registering a passkey proves the second factor as making an app password does: the one-time code, " +
                   "or one of the account's recovery codes in its place. The code is spent by the registration it proved - " +
                   "not by a refusal that came after it was matched, and never twice.")]
      public void ARecoveryCodeStandsInForTheOneTimeCodeWhenRegisteringOne()
      {
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "passkey-codes@" + _domain.Name, Password);
         string me = Basic(account.Address, Password);

         (int status, string body) made = Send("POST", "/api/v1/me/recovery/codes", me, "{\"password\":\"" + Password + "\"}");
         Assert.AreEqual(201, made.status, made.body);
         MatchCollection codes = Regex.Matches(made.body, "\"([2-9A-HJ-NP-TV-Z]{5}-[2-9A-HJ-NP-TV-Z]{5})\"");
         Assert.AreEqual(10, codes.Count, made.body);

         account.EnrolTOTP();
         account.Save();
         try
         {
            // Signed in with the first code, as somebody whose authenticator is
            // gone would be; what is left to check is the Security page's form.
            (int status, string body, string headers) session = SendRaw("POST", "/api/v1/session", me, null,
               "X-hMailServer-Recovery-Code: " + codes[0].Groups[1].Value);
            Assert.AreEqual(201, session.status, session.body);
            Match cookie = Regex.Match(session.headers, @"Set-Cookie: (hmailsession=[^;]+);");
            Assert.IsTrue(cookie.Success, "The sign-in set no session cookie: " + session.headers);
            string signedIn = "Cookie: " + cookie.Groups[1].Value;
            StringAssert.Contains("\"unused\":9", Send("GET", "/api/v1/me/recovery", null, null, signedIn).body);

            using (var authenticator = new SoftwareAuthenticator())
            {
               (int status, string body) options = Send("POST", "/api/v1/passkeys/options", null, "{}", signedIn);
               Assert.AreEqual(200, options.status, options.body);
               string challenge = Value(options.body, "challenge");

               // The password alone: the 401 names the header, as making an app
               // password does, so the page knows to ask for the code.
               (int status, string body, string headers) noCode = SendRaw("POST", "/api/v1/passkeys", null,
                  authenticator.Registration(challenge, Origin, Host, "Tablet", Password), signedIn);
               Assert.AreEqual(401, noCode.status, noCode.body);
               StringAssert.Contains("X-hMailServer-OTP: required", noCode.headers);

               // A code that is not one of the account's changes nothing.
               Assert.AreEqual(401, Send("POST", "/api/v1/passkeys", null,
                  authenticator.Registration(challenge, Origin, Host, "Tablet", Password),
                  signedIn + "\r\nX-hMailServer-Recovery-Code: ZZZZZ-ZZZZZ").status);

               // A refusal AFTER the code was matched spends nothing: the name
               // is checked once the password and the second factor are proved,
               // and a registration with no name is refused there.
               string second = codes[1].Groups[1].Value;
               (int status, string body) unnamed = Send("POST", "/api/v1/passkeys", null,
                  authenticator.Registration(challenge, Origin, Host, "", Password),
                  signedIn + "\r\nX-hMailServer-Recovery-Code: " + second);
               Assert.AreEqual(400, unnamed.status, unnamed.body);
               StringAssert.Contains("name is required", unnamed.body);
               StringAssert.Contains("\"unused\":9", Send("GET", "/api/v1/me/recovery", null, null, signedIn).body,
                  "A refusal does not cost a code.");

               // The same code registers the passkey, and is spent by that.
               (int status, string body) registered = Send("POST", "/api/v1/passkeys", null,
                  authenticator.Registration(challenge, Origin, Host, "Tablet", Password),
                  signedIn + "\r\nX-hMailServer-Recovery-Code: " + second);
               Assert.AreEqual(201, registered.status, registered.body);
               StringAssert.Contains("\"name\":\"Tablet\"", Send("GET", "/api/v1/passkeys", null, null, signedIn).body);
               StringAssert.Contains("\"unused\":8", Send("GET", "/api/v1/me/recovery", null, null, signedIn).body,
                  "The registration it proved spent the code.");

               // And not twice: a fresh challenge, the spent code, no passkey.
               (int status, string body) again = Send("POST", "/api/v1/passkeys/options", null, "{}", signedIn);
               Assert.AreEqual(200, again.status, again.body);
               Assert.AreEqual(401, Send("POST", "/api/v1/passkeys", null,
                  authenticator.Registration(Value(again.body, "challenge"), Origin, Host, "Tablet again", Password),
                  signedIn + "\r\nX-hMailServer-Recovery-Code: " + second).status,
                  "A spent code proves nothing.");
               StringAssert.DoesNotContain("Tablet again", Send("GET", "/api/v1/passkeys", null, null, signedIn).body);
            }
         }
         finally
         {
            account.DisableTOTP();
            account.Save();
         }
      }

      [Test]
      [Description("What must not sign anybody in: an assertion collected at another origin, one whose signature " +
                   "counter has not moved on (a cloned authenticator), one answering a challenge that was already " +
                   "spent, and any of them at all when passkeys are switched off.")]
      public void AnAssertionFromTheWrongOriginOrACloneIsRefused()
      {
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "passkey-refuse@" + _domain.Name, Password);
         string me = Basic(account.Address, Password);

         using (var authenticator = new SoftwareAuthenticator())
         {
            Register(authenticator, me, "Security key", Password);

            // Another origin. The signature is perfectly good and the challenge is
            // this server's; what is wrong is where the browser collected it.
            (string challenge, string cookie) elsewhereAt = Challenge();
            (int status, string body) wrongOrigin = Send("POST", "/api/v1/session", null,
               authenticator.Assertion(elsewhereAt.challenge, "https://evil.example", Host, 0), "Cookie: " + elsewhereAt.cookie);
            Assert.AreEqual(401, wrongOrigin.status, wrongOrigin.body);

            // A challenge works once: the same assertion sent again is refused,
            // because the challenge it answers is no longer waiting.
            (string challenge, string cookie) once = Challenge();
            string assertion = authenticator.Assertion(once.challenge, Origin, Host, 0);
            Assert.AreEqual(201, Send("POST", "/api/v1/session", null, assertion, "Cookie: " + once.cookie).status);
            Assert.AreEqual(401, Send("POST", "/api/v1/session", null, assertion, "Cookie: " + once.cookie).status,
               "A replayed assertion answers a challenge that has been spent.");

            // A clone: an authenticator whose counter has not moved past what the
            // server last saw. The signature verifies; the counter is what refuses it.
            (string challenge, string cookie) cloned = Challenge();
            uint behind = authenticator.SignCount - 1;
            (int status, string body) clone = Send("POST", "/api/v1/session", null,
               authenticator.Assertion(cloned.challenge, Origin, Host, behind), "Cookie: " + cloned.cookie);
            Assert.AreEqual(401, clone.status, clone.body);

            // An authenticator that saw the person but did not verify them - a
            // security key touched without its PIN. Everything else about the
            // assertion is perfect. It is refused because the one-time code is
            // skipped on a passkey sign-in on exactly the strength of that
            // verification, so a credential enrolled as two factors must not be
            // able to assert as one.
            (string challenge, string cookie) touchOnly = Challenge();
            (int status, string body) unverified = Send("POST", "/api/v1/session", null,
               authenticator.Assertion(touchOnly.challenge, Origin, Host, 0, 0x01), "Cookie: " + touchOnly.cookie);
            Assert.AreEqual(401, unverified.status, unverified.body);

            // A body-authenticated session without the header this server's own
            // pages send. A passkey assertion is a credential a cross-site form
            // could carry - no Authorization header to set, no cookie of ours to
            // send - so the header is what says the post came from this page.
            (string challenge, string cookie) noHeader = Challenge();
            Assert.AreEqual(401, Send("POST", "/api/v1/session", null,
               authenticator.Assertion(noHeader.challenge, Origin, Host, 0), "Cookie: " + noHeader.cookie, false).status,
               "A session minted from the request body needs the header a cross-site form cannot set.");

            // Switched off, nothing at all.
            Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"passkeys_enabled\":false}").status);
            Assert.AreEqual(403, Send("POST", "/api/v1/passkeys/challenge", null, "{}").status);
            Assert.AreEqual(401, Send("POST", "/api/v1/session", null,
               authenticator.Assertion("Y2hhbGxlbmdl", Origin, Host, 0), null).status);
            Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"passkeys_enabled\":true}").status);
         }
      }

      [Test]
      [Description("With passkey_relying_party set to a parent domain, a page at any host under it can have the " +
                   "browser collect a passkey of that domain, so the origin must be one passkey_allowed_origins " +
                   "names - never merely the host the request's own Host header names. Without the list nothing is " +
                   "registered or taken; with it, an assertion collected at a sibling host is refused although its " +
                   "signature, challenge and relying party are all good, and one collected at a named origin signs in.")]
      public void AParentDomainRelyingPartyTakesOnlyTheOriginsItNames()
      {
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "passkey-parent@" + _domain.Name, Password);
         string me = Basic(account.Address, Password);
         const string Parent = "example.test";
         const string Ours = "https://mail.example.test";

         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"passkey_relying_party\":\"" + Parent + "\"}").status);

         // A relying party and no list: nothing is this server's own origin yet, and both
         // challenge routes say what to set.
         (int status, string body) noList = Send("POST", "/api/v1/passkeys/options", me, "{}");
         Assert.AreEqual(400, noList.status, noList.body);
         StringAssert.Contains("passkey_allowed_origins", noList.body);
         Assert.AreEqual(400, Send("POST", "/api/v1/passkeys/challenge", null, "{}").status);

         // A list that is not a list of origins is refused.
         Assert.AreEqual(400, Admin("PUT", "/api/v1/settings", "{\"passkey_allowed_origins\":\"mail.example.test\"}").status);
         Assert.AreEqual(400, Admin("PUT", "/api/v1/settings", "{\"passkey_allowed_origins\":\"https://mail.example.test/portal\"}").status);
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"passkey_allowed_origins\":\"" + Ours + ", https://webmail.example.test\"}").status);

         using (var authenticator = new SoftwareAuthenticator())
         {
            // Registered at the named origin, bound to the parent domain.
            (int status, string body) options = Send("POST", "/api/v1/passkeys/options", me, "{}", null, true, "mail.example.test");
            Assert.AreEqual(200, options.status, options.body);
            StringAssert.Contains("\"id\":\"" + Parent + "\"", options.body);
            (int status, string body) made = Send("POST", "/api/v1/passkeys", me,
               authenticator.Registration(Value(options.body, "challenge"), Ours, Parent, "Phone", Password), null, true, "mail.example.test");
            Assert.AreEqual(201, made.status, made.body);

            // The attack: a page on a sibling host asks the browser for a passkey of the
            // parent domain - which the browser allows - and the client posts the
            // assertion here. Signature, challenge, relying party hash and counter are
            // all good; the origin is not this server's. The Host carried is one the
            // server answers to (a configured origin's host), so that what this
            // isolates is the passkey origin check and not the Host-header check added
            // in 6.3.6 - that check (a rebinding page reaching this address under the
            // sibling's own Host) is covered by the server-hardening fixtures; here the
            // assertion's own origin is the sibling, which the origin check refuses.
            (string challenge, string cookie) sibling = Challenge();
            (int status, string body) phished = Send("POST", "/api/v1/session", null,
               authenticator.Assertion(sibling.challenge, "https://shop.example.test", Parent, 0), "Cookie: " + sibling.cookie,
               true, "mail.example.test");
            Assert.AreEqual(401, phished.status, "An assertion collected at a sibling host signed somebody in: " + phished.body);

            // The negative control: the same authenticator at an origin the list names.
            (string challenge, string cookie) ours = Challenge();
            (int status, string body) signedIn = Send("POST", "/api/v1/session", null,
               authenticator.Assertion(ours.challenge, "https://webmail.example.test", Parent, 0), "Cookie: " + ours.cookie,
               true, "webmail.example.test");
            Assert.AreEqual(201, signedIn.status, signedIn.body);
            StringAssert.Contains(account.Address, signedIn.body);
         }
      }

      [Test]
      [Description("The administrator registers a passkey from the Control Deck while signed in with the " +
                   "administrator password, and signs in with it afterwards; the account's passkeys and the " +
                   "administrator's are not each other's.")]
      public void TheAdministratorRegistersAPasskeyAndSignsInWithIt()
      {
         string administrator = Basic("Administrator", AdminPassword);

         using (var authenticator = new SoftwareAuthenticator())
         {
            (int status, string body) noPassword = Send("POST", "/api/v1/passkeys", administrator,
               "{\"name\":\"Console\",\"credential\":{}}");
            Assert.AreEqual(400, noPassword.status, noPassword.body);

            string id = Register(authenticator, administrator, "Console laptop", AdminPassword);
            StringAssert.Contains("\"name\":\"Console laptop\"", Send("GET", "/api/v1/passkeys", administrator, null).body);

            // An account does not see it, and cannot remove it.
            Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "passkey-other@" + _domain.Name, Password);
            string me = Basic(account.Address, Password);
            StringAssert.Contains("\"passkeys\":[]", Send("GET", "/api/v1/passkeys", me, null).body);
            Assert.AreEqual(404, Send("DELETE", "/api/v1/passkeys/" + id, me, null).status,
               "The administrator's passkey is not an account's to remove.");

            // The Deck's sign-in: the session it starts is the administrator's.
            string cookie = SignIn(authenticator);
            (int status, string body) session = Send("GET", "/api/v1/session", null, null, "Cookie: " + cookie);
            Assert.AreEqual(200, session.status, session.body);
            StringAssert.Contains("administrator", session.body);
            Assert.AreEqual(200, Send("GET", "/api/v1/status", null, null, "Cookie: " + cookie).status,
               "The administrator's session reaches the administration API.");

            Assert.AreEqual(200, Send("DELETE", "/api/v1/passkeys/" + id, administrator, null).status);
         }
      }

      [Test]
      [Description("Sign-in challenges asked for in their thousands, each from a /24 of its own as a rotating proxy pool gives them, push nobody's out: a person's challenge, asked for first, still signs them in with their passkey. Nothing of a challenge is kept on the server - the hmpasskey cookie carries it, sealed - so there is no table to fill. Until 6.3.6 a table of a thousand dropped its oldest whoever held it; as 6.3.6 was first written a table of 4,096 held each source to its share, which 4,200 sources from as many /24s still beat.")]
      public void ChallengesFromThousandsOfSourcesPushNobodysOut()
      {
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "passkey-flood@" + _domain.Name, Password);
         string me = Basic(account.Address, Password);

         using (var authenticator = new SoftwareAuthenticator())
         {
            Register(authenticator, me, "Phone", Password);

            (string challenge, string cookie) person = Challenge("X-Forwarded-For: 198.51.100.4");
            StringAssert.DoesNotContain(person.challenge, person.cookie, "The cookie shows the challenge it holds.");

            for (int i = 0; i < 4200; i++)
               Challenge("X-Forwarded-For: 10." + (i / 256) + "." + (i % 256) + ".1");

            (int status, string body, string headers) session = SendRaw("POST", "/api/v1/session", null,
               authenticator.Assertion(person.challenge, Origin, Host, 0), "Cookie: " + person.cookie);
            Assert.AreEqual(201, session.status, "The person's challenge still answers after the flood: " + session.body);
         }
      }

      [Test]
      [Description("The administrator password asked again at a passkey registration is held to the same failed-sign-in throttle as the password itself: from an administrator session that did not need the password, a wrong one is refused 403, and the next guess inside the delay 429 with Retry-After before it is looked at. Until 6.3.6 the throttle read only an Authorization: Basic header.")]
      public void TheAdministratorPasswordAskedAtARegistrationIsThrottled()
      {
         (int status, string body, string headers) signedIn = SendRaw("POST", "/api/v1/session", Basic("Administrator", AdminPassword), null, null);
         Assert.AreEqual(201, signedIn.status, signedIn.body);
         Match cookie = Regex.Match(signedIn.headers, @"Set-Cookie: (hmailsession=[^;]+);");
         Assert.IsTrue(cookie.Success, signedIn.headers);

         string wrongPassword = "{\"name\":\"Console\",\"password\":\"" + AdminPassword + "-wrong\",\"credential\":{}}";

         (int status, string body, string headers) first = SendRaw("POST", "/api/v1/passkeys", null, wrongPassword, "Cookie: " + cookie.Groups[1].Value);
         Assert.AreEqual(403, first.status, first.body);
         StringAssert.Contains("the password is not correct", first.body);

         (int status, string body, string headers) second = SendRaw("POST", "/api/v1/passkeys", null, wrongPassword, "Cookie: " + cookie.Groups[1].Value);
         Assert.AreEqual(429, second.status, "A second guess inside the delay is refused before it is looked at: " + second.body);
         Match retryAfter = Regex.Match(second.headers, @"Retry-After: (\d+)");
         Assert.IsTrue(retryAfter.Success, second.headers);

         // The delay passes, and the right password clears it for the fixtures after.
         Thread.Sleep((int.Parse(retryAfter.Groups[1].Value) + 1) * 1000);
         Assert.AreEqual(200, Admin("GET", "/api/v1/status", null).status, "The right password is taken once the delay has passed.");
      }

      // ---------------------------------------------------------------- helpers

      /// <summary>
      ///    Registers the authenticator's credential for whatever credential the
      ///    Authorization header stands for, and answers the passkey's id.
      /// </summary>
      private static string Register(SoftwareAuthenticator authenticator, string authorization, string name, string password)
      {
         (int status, string body) options = Send("POST", "/api/v1/passkeys/options", authorization, "{}");
         Assert.AreEqual(200, options.status, options.body);
         StringAssert.Contains("\"id\":\"127.0.0.1\"", options.body, "The relying party is the host the page was served on.");
         string challenge = Value(options.body, "challenge");

         string registration = authenticator.Registration(challenge, Origin, Host, name, password);
         (int status, string body) made = Send("POST", "/api/v1/passkeys", authorization, registration);
         Assert.AreEqual(201, made.status, made.body);
         return Regex.Match(made.body, "\"id\":(\\d+)").Groups[1].Value;
      }

      /// <summary>
      ///    A sign-in challenge and the cookie it was handed out with. extraHeader is
      ///    one more header line - an X-Forwarded-For, which a request from this
      ///    machine is believed for as a reverse proxy's.
      /// </summary>
      private static (string challenge, string cookie) Challenge(string extraHeader = null)
      {
         (int status, string body, string headers) answer = SendRaw("POST", "/api/v1/passkeys/challenge", null, "{}", extraHeader);
         Assert.AreEqual(200, answer.status, answer.body);
         Match cookie = Regex.Match(answer.headers, @"Set-Cookie: (hmpasskey=[^;]+);");
         Assert.IsTrue(cookie.Success, "The challenge was handed out without its cookie: " + answer.headers);
         return (Value(answer.body, "challenge"), cookie.Groups[1].Value);
      }

      /// <summary>Signs in with the authenticator and answers the session cookie.</summary>
      private static string SignIn(SoftwareAuthenticator authenticator)
      {
         (string challenge, string cookie) offered = Challenge();
         (int status, string body, string headers) session = SendRaw("POST", "/api/v1/session", null,
            authenticator.Assertion(offered.challenge, Origin, Host, 0), "Cookie: " + offered.cookie);
         Assert.AreEqual(201, session.status, session.body);
         Match cookie = Regex.Match(session.headers, @"Set-Cookie: (hmailsession=[^;]+);");
         Assert.IsTrue(cookie.Success, "The passkey sign-in set no session cookie: " + session.headers);
         return cookie.Groups[1].Value;
      }

      private static string Value(string json, string key)
      {
         Match match = Regex.Match(json, "\"" + key + "\":\"([^\"]*)\"");
         Assert.IsTrue(match.Success, "no " + key + " in " + json);
         return match.Groups[1].Value;
      }

      private static string Basic(string user, string password)
      {
         return "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(user + ":" + password));
      }

      private static (int status, string body) Admin(string method, string path, string requestBody)
      {
         return Send(method, path, Basic("Administrator", AdminPassword), requestBody);
      }

      private static (int status, string body) Send(string method, string path, string authorization, string requestBody,
                                                    string extraHeader = null, bool requestedWith = true,
                                                    string hostHeader = null)
      {
         (int status, string body, string headers) answer = SendRaw(method, path, authorization, requestBody, extraHeader,
            requestedWith, hostHeader);
         return (answer.status, answer.body);
      }

      /// <summary>
      ///    One HTTP/1.0 request with the response HEADERS as well as its body: this
      ///    fixture needs the Set-Cookie lines, which RestRequest drops. Written here
      ///    rather than added to the shared helper, as the note in RestRequest invites.
      ///
      ///    Every write carries X-Requested-With, as both of this server's own pages
      ///    do. It matters most on POST /api/v1/session: a passkey assertion is a
      ///    credential read out of the request BODY, which is the one credential a
      ///    cross-site form could carry, so the server takes the header as the proof
      ///    that the post came from its own page. requestedWith: false is how the
      ///    check below sends one without it.
      /// </summary>
      private static (int status, string body, string headers) SendRaw(string method, string path, string authorization,
                                                                       string requestBody, string extraHeader,
                                                                       bool requestedWith = true, string hostHeader = null)
      {
         using (var client = new TcpClient())
         {
            Exception last = null;
            for (int attempt = 0; attempt < 25; attempt++)
            {
               try
               {
                  client.Connect(Host, RestPort);
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
               headers.Append("Host: " + (hostHeader ?? Host) + "\r\n");
               if (authorization != null)
                  headers.Append("Authorization: " + authorization + "\r\n");
               if (requestedWith && method != "GET")
                  headers.Append("X-Requested-With: hMailServer\r\n");

               byte[] bodyBytes = requestBody == null ? new byte[0] : Encoding.UTF8.GetBytes(requestBody);
               if (requestBody != null)
               {
                  headers.Append("Content-Type: application/json\r\n");
                  headers.Append("Content-Length: " + bodyBytes.Length + "\r\n");
               }

               if (!string.IsNullOrEmpty(extraHeader))
                  headers.Append(extraHeader + "\r\n");

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
               string[] lines = raw.Split(new[] {"\r\n"}, StringSplitOptions.None);
               if (lines.Length > 0)
               {
                  string[] parts = lines[0].Split(' ');
                  if (parts.Length >= 2)
                     int.TryParse(parts[1], out statusCode);
               }

               int separator = raw.IndexOf("\r\n\r\n", StringComparison.Ordinal);
               return (statusCode,
                       separator >= 0 ? raw.Substring(separator + 4) : "",
                       separator >= 0 ? raw.Substring(0, separator) : raw);
            }
         }
      }

      /// <summary>
      ///    A WebAuthn authenticator, in this process. It holds one P-256 key pair and
      ///    one credential id, and does exactly what a phone or a security key does:
      ///    writes the client data the browser would collect, writes the authenticator
      ///    data with the relying party hash, the flags and the counter, and signs
      ///    authenticatorData || SHA-256(clientDataJSON).
      /// </summary>
      private sealed class SoftwareAuthenticator : IDisposable
      {
         private readonly ECDsa _key = ECDsa.Create(ECCurve.NamedCurves.nistP256);
         private readonly byte[] _credentialId;
         private readonly byte[] _aaguid = new byte[16];

         public SoftwareAuthenticator()
         {
            _credentialId = new byte[32];
            using (var random = RandomNumberGenerator.Create())
               random.GetBytes(_credentialId);
            SignCount = 5;
         }

         /// <summary>What the authenticator will report next, and has reported last.</summary>
         public uint SignCount { get; private set; }

         public string CredentialIdBase64Url
         {
            get { return Base64Url(_credentialId); }
         }

         public string PublicKeyBase64Url
         {
            get { return Base64Url(CoseKey()); }
         }

         /// <summary>The body POST /api/v1/passkeys takes.</summary>
         public string Registration(string challenge, string origin, string rpId, string name, string password)
         {
            byte[] clientData = ClientData("webauthn.create", challenge, origin);
            byte[] authenticatorData = AuthenticatorData(rpId, 0x45, SignCount, true);

            byte[] attestation = Cbor.Map(new[]
            {
               new KeyValuePair<byte[], byte[]>(Cbor.Text("fmt"), Cbor.Text("none")),
               new KeyValuePair<byte[], byte[]>(Cbor.Text("attStmt"), Cbor.Map(new KeyValuePair<byte[], byte[]>[0])),
               new KeyValuePair<byte[], byte[]>(Cbor.Text("authData"), Cbor.Bytes(authenticatorData))
            });

            return "{\"name\":\"" + name + "\",\"password\":\"" + password + "\",\"credential\":{\"id\":\"" +
                   CredentialIdBase64Url + "\",\"response\":{\"clientDataJSON\":\"" + Base64Url(clientData) +
                   "\",\"attestationObject\":\"" + Base64Url(attestation) + "\",\"transports\":[\"internal\"]}}}";
         }

         /// <summary>
         ///    The body POST /api/v1/session takes in place of a password. counter is the
         ///    number to report; 0 means "the next one", which is what a real
         ///    authenticator does. flags is the authenticator data's flags byte: 0x05 is
         ///    user present and user verified, which is what an authenticator that asked
         ///    for a PIN or a fingerprint reports, and 0x01 is a touch alone.
         /// </summary>
         public string Assertion(string challenge, string origin, string rpId, uint counter, byte flags = 0x05)
         {
            if (counter == 0)
            {
               SignCount += 1;
               counter = SignCount;
            }

            byte[] clientData = ClientData("webauthn.get", challenge, origin);
            byte[] authenticatorData = AuthenticatorData(rpId, flags, counter, false);

            byte[] signed = Concat(authenticatorData, Sha256(clientData));
            byte[] raw = _key.SignData(signed, HashAlgorithmName.SHA256);

            return "{\"passkey\":{\"id\":\"" + CredentialIdBase64Url + "\",\"response\":{\"clientDataJSON\":\"" +
                   Base64Url(clientData) + "\",\"authenticatorData\":\"" + Base64Url(authenticatorData) +
                   "\",\"signature\":\"" + Base64Url(Der(raw)) + "\",\"userHandle\":\"\"}}}";
         }

         private static byte[] ClientData(string type, string challenge, string origin)
         {
            return Encoding.UTF8.GetBytes("{\"type\":\"" + type + "\",\"challenge\":\"" + challenge +
                                          "\",\"origin\":\"" + origin + "\",\"crossOrigin\":false}");
         }

         private byte[] AuthenticatorData(string rpId, byte flags, uint counter, bool attested)
         {
            var data = new List<byte>();
            data.AddRange(Sha256(Encoding.UTF8.GetBytes(rpId)));
            data.Add(flags);
            data.Add((byte) (counter >> 24));
            data.Add((byte) (counter >> 16));
            data.Add((byte) (counter >> 8));
            data.Add((byte) counter);

            if (attested)
            {
               data.AddRange(_aaguid);
               data.Add((byte) (_credentialId.Length >> 8));
               data.Add((byte) _credentialId.Length);
               data.AddRange(_credentialId);
               data.AddRange(CoseKey());
            }

            return data.ToArray();
         }

         private byte[] CoseKey()
         {
            ECParameters parameters = _key.ExportParameters(false);
            return Cbor.Map(new[]
            {
               new KeyValuePair<byte[], byte[]>(Cbor.Integer(1), Cbor.Integer(2)),      // kty: EC2
               new KeyValuePair<byte[], byte[]>(Cbor.Integer(3), Cbor.Integer(-7)),     // alg: ES256
               new KeyValuePair<byte[], byte[]>(Cbor.Integer(-1), Cbor.Integer(1)),     // crv: P-256
               new KeyValuePair<byte[], byte[]>(Cbor.Integer(-2), Cbor.Bytes(parameters.Q.X)),
               new KeyValuePair<byte[], byte[]>(Cbor.Integer(-3), Cbor.Bytes(parameters.Q.Y))
            });
         }

         /// <summary>
         ///    .NET signs P-256 into the IEEE P1363 R||S pair, which is what JWS uses;
         ///    WebAuthn carries the X9.62 DER an authenticator emits, so the pair is
         ///    wrapped here rather than in the server.
         /// </summary>
         private static byte[] Der(byte[] raw)
         {
            byte[] r = DerInteger(raw, 0, raw.Length / 2);
            byte[] s = DerInteger(raw, raw.Length / 2, raw.Length / 2);
            var der = new List<byte> {0x30};
            int length = r.Length + s.Length;
            if (length < 128)
            {
               der.Add((byte) length);
            }
            else
            {
               der.Add(0x81);
               der.Add((byte) length);
            }
            der.AddRange(r);
            der.AddRange(s);
            return der.ToArray();
         }

         private static byte[] DerInteger(byte[] source, int offset, int count)
         {
            int start = offset;
            int end = offset + count;
            while (start < end - 1 && source[start] == 0)
               start++;

            bool pad = (source[start] & 0x80) != 0;
            var value = new List<byte> {0x02, (byte) (end - start + (pad ? 1 : 0))};
            if (pad)
               value.Add(0);
            for (int i = start; i < end; i++)
               value.Add(source[i]);
            return value.ToArray();
         }

         private static byte[] Sha256(byte[] data)
         {
            using (var sha = SHA256.Create())
               return sha.ComputeHash(data);
         }

         private static byte[] Concat(byte[] first, byte[] second)
         {
            var both = new byte[first.Length + second.Length];
            Buffer.BlockCopy(first, 0, both, 0, first.Length);
            Buffer.BlockCopy(second, 0, both, first.Length, second.Length);
            return both;
         }

         private static string Base64Url(byte[] data)
         {
            return Convert.ToBase64String(data).TrimEnd('=').Replace('+', '-').Replace('/', '_');
         }

         public void Dispose()
         {
            _key.Dispose();
         }
      }

      /// <summary>
      ///    Just enough CBOR to write what an authenticator writes: unsigned and negative
      ///    integers, byte and text strings, and definite-length maps.
      /// </summary>
      private static class Cbor
      {
         public static byte[] Integer(long value)
         {
            return value >= 0 ? Head(0, (ulong) value) : Head(1, (ulong) (-value - 1));
         }

         public static byte[] Bytes(byte[] data)
         {
            return Join(Head(2, (ulong) data.Length), data);
         }

         public static byte[] Text(string text)
         {
            byte[] raw = Encoding.UTF8.GetBytes(text);
            return Join(Head(3, (ulong) raw.Length), raw);
         }

         public static byte[] Map(KeyValuePair<byte[], byte[]>[] members)
         {
            var all = new List<byte>(Head(5, (ulong) members.Length));
            foreach (KeyValuePair<byte[], byte[]> member in members)
            {
               all.AddRange(member.Key);
               all.AddRange(member.Value);
            }
            return all.ToArray();
         }

         private static byte[] Head(int major, ulong argument)
         {
            if (argument < 24)
               return new[] {(byte) ((major << 5) | (int) argument)};
            if (argument < 256)
               return new[] {(byte) ((major << 5) | 24), (byte) argument};
            if (argument < 65536)
               return new[] {(byte) ((major << 5) | 25), (byte) (argument >> 8), (byte) argument};
            return new[]
            {
               (byte) ((major << 5) | 26), (byte) (argument >> 24), (byte) (argument >> 16),
               (byte) (argument >> 8), (byte) argument
            };
         }

         private static byte[] Join(byte[] first, byte[] second)
         {
            var both = new byte[first.Length + second.Length];
            Buffer.BlockCopy(first, 0, both, 0, first.Length);
            Buffer.BlockCopy(second, 0, both, first.Length, second.Length);
            return both;
         }
      }
   }
}
