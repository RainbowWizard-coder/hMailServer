// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
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
   ///    Single sign-on (OidcSignIn.h, RestApiOidcSignIn.cpp): an organisation's identity
   ///    provider as the front door to the webmail and the Control Deck.
   ///
   ///    The identity provider is in this file. It is a FakeHttpEndpoint serving the three
   ///    documents a real one serves - the discovery document, a JWK Set holding an RSA
   ///    key minted in this process, and a token endpoint answering an id_token signed
   ///    with it - so the whole of the server's half runs: the authorize redirect it
   ///    writes, the code it exchanges, the token it verifies (signature, issuer,
   ///    audience, expiry, nonce), the address it maps and the session it ends up with.
   ///
   ///    The browser is not needed either: the fixture follows the redirects itself,
   ///    which is all a browser does here.
   /// </summary>
   [TestFixture]
   public class RestApiOidcSignIn : TestFixtureBase
   {
      private static int RestPort = 9737;
      private const int IssuerPort = 9738;
      private const string AdminPassword = "testar";
      private const string ClientId = "hmailserver-under-test";
      private const string Host = "127.0.0.1";

      private static string Issuer
      {
         get { return "http://127.0.0.1:" + IssuerPort; }
      }

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
               "{\"oidc_issuer\":\"\",\"oidc_client_id\":\"\",\"oidc_client_secret\":\"\"," +
               "\"oidc_redirect_base_url\":\"\",\"oidc_administrators\":\"\",\"oidc_auto_provision\":false}");
         }
         finally
         {
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      [Test]
      [Description("An address the issuer verified signs in to the webmail: the start redirects to the issuer with " +
                   "PKCE and a nonce, the callback verifies the id_token and hands out a one-use ticket, and the " +
                   "ticket becomes the session a password would have started. The ticket works once.")]
      public void AVerifiedAddressSignsInToTheWebmail()
      {
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "sso@" + _domain.Name, "Sso-Passw0rd!");

         using (var rsa = RSA.Create(2048))
         using (var issuer = new FakeHttpEndpoint(IssuerPort, 404, "{}", "application/json"))
         {
            Publish(issuer, rsa);
            Configure();

            // What the sign-in page offers, asked before anybody has signed in.
            (int status, string body) methods = Send("GET", "/api/v1/signin-methods", null, null);
            Assert.AreEqual(200, methods.status, methods.body);
            StringAssert.Contains("\"oidc\":true", methods.body);
            StringAssert.Contains("\"oidc_label\":\"127.0.0.1:" + IssuerPort + "\"", methods.body);

            Started started = Start(false);
            StringAssert.Contains("code_challenge_method=S256", started.location);
            StringAssert.Contains("client_id=" + ClientId, started.location);
            StringAssert.Contains("scope=openid%20email", started.location);
            // The redirect_uri is one query parameter of the authorization request,
            // so its slashes travel percent-encoded (RFC 6749 section 4.1.1).
            StringAssert.Contains("redirect_uri=http%3A%2F%2F127.0.0.1%3A" + RestPort + "%2Fportal%2Foidc%2Fcallback", started.location);

            issuer.SetResponse("/token", 200,
               Encoding.UTF8.GetBytes("{\"access_token\":\"a\",\"token_type\":\"Bearer\",\"id_token\":\"" +
                  IdToken(rsa, Issuer, ClientId, started.nonce, account.Address, true, 600) + "\"}"),
               "application/json");

            Ticket ticket = Complete(started, "/portal");
            DateTime lastLogonBefore = Convert.ToDateTime(_domain.Accounts.get_ItemByAddress(account.Address).LastLogonTime);

            // The stamp is kept to the second: a sign-in in the same second as
            // the reading above would compare equal to it and prove nothing.
            Thread.Sleep(1100);

            // A ticket posted without the header this server's own pages send.
            // That is the cross-site form: no Authorization header to set, no
            // cookie of ours to send, a body a text/plain form can write. It is
            // refused - and the ticket is not spent by it either, which the
            // sign-in immediately below proves, so a stranger cannot even use it
            // to burn somebody else's ticket.
            Assert.AreEqual(401, Send("POST", "/api/v1/session", null, "{\"sso_ticket\":\"" + ticket.value + "\"}",
               "Cookie: " + ticket.cookie, false).status,
               "A session minted from the request body needs the header a cross-site form cannot set.");

            // The ticket carried to another browser: the link /portal#sso=<ticket>
            // sent to somebody else, whose browser has no hmsso cookie, or has the
            // cookie of a sign-in of its own. This server's own page would post it
            // with the header, so the header proves nothing here; the cookie does.
            // Refused both ways, and the ticket is not spent by either, which the
            // sign-in below proves.
            Assert.AreEqual(401, Send("POST", "/api/v1/session", null, "{\"sso_ticket\":\"" + ticket.value + "\"}", null).status,
               "A ticket presented by a browser that was not given its cookie signs nobody in.");

            Started another = Start(false);
            Token(issuer, rsa, Issuer, ClientId, another.nonce, account.Address, true, 600);
            Ticket anotherTicket = Complete(another, "/portal");
            Assert.AreEqual(401, Send("POST", "/api/v1/session", null, "{\"sso_ticket\":\"" + ticket.value + "\"}",
               "Cookie: " + anotherTicket.cookie).status,
               "A ticket presented with another sign-in's cookie signs nobody in.");

            (int status, string body, string headers) session = SendRaw("POST", "/api/v1/session", null,
               "{\"sso_ticket\":\"" + ticket.value + "\"}", "Cookie: " + ticket.cookie);
            Assert.AreEqual(201, session.status, session.body);
            StringAssert.Contains(account.Address, session.body);

            // Stamped as the logon it is, as a password sign-in is.
            Assert.AreNotEqual(lastLogonBefore,
               Convert.ToDateTime(_domain.Accounts.get_ItemByAddress(account.Address).LastLogonTime),
               "A single sign-on is a logon, and is stamped as one.");

            Match cookie = Regex.Match(session.headers, @"Set-Cookie: (hmailsession=[^;]+);");
            Assert.IsTrue(cookie.Success, "The ticket started no session: " + session.headers);
            (int status, string body) whoami = Send("GET", "/api/v1/me", null, null, "Cookie: " + cookie.Groups[1].Value);
            Assert.AreEqual(200, whoami.status, whoami.body);
            StringAssert.Contains(account.Address, whoami.body);

            // The device list says which credential it was.
            StringAssert.Contains("\"credential\":\"single_sign_on\"",
               Send("GET", "/api/v1/me/devices", Basic(account.Address, "Sso-Passw0rd!"), null).body);

            // The session answer clears the ticket's cookie: it has nothing left to do.
            StringAssert.Contains("Set-Cookie: hmsso=;", session.headers);

            // A ticket is good once.
            Assert.AreEqual(401, Send("POST", "/api/v1/session", null, "{\"sso_ticket\":\"" + ticket.value + "\"}",
               "Cookie: " + ticket.cookie).status,
               "A spent ticket is not a credential.");
         }
      }

      [Test]
      [Description("What the callback refuses: an id_token answering another sign-in (a wrong nonce), one issued " +
                   "for somebody else (a wrong audience), one that has expired, and one whose address this server " +
                   "has no mailbox for - and the state itself, which works once.")]
      public void AnIdTokenThatDoesNotCheckOutIsRefused()
      {
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "sso-refuse@" + _domain.Name, "Sso-Passw0rd!");

         using (var rsa = RSA.Create(2048))
         using (var issuer = new FakeHttpEndpoint(IssuerPort, 404, "{}", "application/json"))
         {
            Publish(issuer, rsa);
            Configure();

            // A nonce from another sign-in.
            Started wrongNonce = Start(false);
            Token(issuer, rsa, Issuer, ClientId, "a-nonce-from-somewhere-else", account.Address, true, 600);
            StringAssert.Contains("sso_error", Callback(wrongNonce).location);

            // Issued for another client.
            Started wrongAudience = Start(false);
            Token(issuer, rsa, Issuer, "somebody-else", wrongAudience.nonce, account.Address, true, 600);
            StringAssert.Contains("sso_error", Callback(wrongAudience).location);

            // Expired.
            Started expired = Start(false);
            Token(issuer, rsa, Issuer, ClientId, expired.nonce, account.Address, true, -600);
            StringAssert.Contains("sso_error", Callback(expired).location);

            // An address the issuer has not verified.
            Started unverified = Start(false);
            Token(issuer, rsa, Issuer, ClientId, unverified.nonce, account.Address, false, 600);
            StringAssert.Contains("sso_error", Callback(unverified).location);

            // A verified address this server has no mailbox for, with provisioning off.
            Started stranger = Start(false);
            Token(issuer, rsa, Issuer, ClientId, stranger.nonce, "nobody-here@" + _domain.Name, true, 600);
            StringAssert.Contains("sso_error", Callback(stranger).location);

            // A state works once: the same callback again finds nothing pending.
            Started spent = Start(false);
            Token(issuer, rsa, Issuer, ClientId, spent.nonce, account.Address, true, 600);
            StringAssert.Contains("sso=", Callback(spent).location);
            StringAssert.Contains("sso_error", Callback(spent).location);
         }
      }

      [Test]
      [Description("With oidc_auto_provision on, a verified address at a domain this server has gets a mailbox at " +
                   "its first sign-in; and an address in oidc_administrators signs in to the Control Deck as the " +
                   "administrator, which an address not in that list does not.")]
      public void ProvisioningAndTheControlDeck()
      {
         string newcomer = "sso-new@" + _domain.Name;
         string chief = "sso-chief@" + _domain.Name;
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, chief, "Sso-Passw0rd!");

         using (var rsa = RSA.Create(2048))
         using (var issuer = new FakeHttpEndpoint(IssuerPort, 404, "{}", "application/json"))
         {
            Publish(issuer, rsa);
            Configure();

            // Off: no mailbox is made.
            Started refused = Start(false);
            Token(issuer, rsa, Issuer, ClientId, refused.nonce, newcomer, true, 600);
            StringAssert.Contains("sso_error", Callback(refused).location);
            Assert.AreEqual(404, Admin("GET", "/api/v1/accounts/" + newcomer, null).status, "No mailbox was made.");

            // On: one is, at a domain this server already has.
            Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"oidc_auto_provision\":true}").status);
            Started provisioned = Start(false);
            Token(issuer, rsa, Issuer, ClientId, provisioned.nonce, newcomer, true, 600);
            Ticket ticket = Complete(provisioned, "/portal");
            Assert.AreEqual(201, Send("POST", "/api/v1/session", null, "{\"sso_ticket\":\"" + ticket.value + "\"}",
               "Cookie: " + ticket.cookie).status);
            Assert.AreEqual(200, Admin("GET", "/api/v1/accounts/" + newcomer, null).status, "The mailbox was made.");

            // A verified address at a domain this server does not have is still refused.
            Started elsewhere = Start(false);
            Token(issuer, rsa, Issuer, ClientId, elsewhere.nonce, "somebody@not-a-domain-here.test", true, 600);
            StringAssert.Contains("sso_error", Callback(elsewhere).location);

            // The Control Deck: the list decides, not the issuer.
            Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"oidc_administrators\":\"" + chief + "\"}").status);

            Started notTheChief = Start(true);
            Token(issuer, rsa, Issuer, ClientId, notTheChief.nonce, newcomer, true, 600);
            (int status, string location) deckRefusal = Callback(notTheChief);
            StringAssert.Contains("sso_error", deckRefusal.location);
            StringAssert.StartsWith("/#", deckRefusal.location, "A Deck sign-in comes back to the Deck.");

            Started asChief = Start(true);
            Token(issuer, rsa, Issuer, ClientId, asChief.nonce, chief, true, 600);
            Ticket deckTicket = Complete(asChief, "/");

            (int status, string body, string headers) session = SendRaw("POST", "/api/v1/session", null,
               "{\"sso_ticket\":\"" + deckTicket.value + "\"}", "Cookie: " + deckTicket.cookie);
            Assert.AreEqual(201, session.status, session.body);
            StringAssert.Contains("\"administrator\":true", session.body);

            Match cookie = Regex.Match(session.headers, @"Set-Cookie: (hmailsession=[^;]+);");
            Assert.IsTrue(cookie.Success, session.headers);
            Assert.AreEqual(200, Send("GET", "/api/v1/status", null, null, "Cookie: " + cookie.Groups[1].Value).status,
               "The Deck's session reaches the administration API.");
         }
      }

      [Test]
      [Description("A domain switched off signs nobody in, however they prove who they are: a single sign-on " +
                   "ticket minted before the switch is refused at POST /api/v1/session, a sign-in finished after " +
                   "it comes back with sso_error, and a browser session started with the password before it ends " +
                   "at its next request - as the password itself is refused. The negative control is each of them " +
                   "working while the domain is on.")]
      public void ASwitchedOffDomainSignsNobodyIn()
      {
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "sso-offboarded@" + _domain.Name, "Sso-Passw0rd!");

         using (var rsa = RSA.Create(2048))
         using (var issuer = new FakeHttpEndpoint(IssuerPort, 404, "{}", "application/json"))
         {
            Publish(issuer, rsa);
            Configure();

            // While the domain is on: a password session, and a ticket not yet spent.
            (int status, string body, string headers) byPassword = SendRaw("POST", "/api/v1/session",
               Basic(account.Address, "Sso-Passw0rd!"), null, null);
            Assert.AreEqual(201, byPassword.status, byPassword.body);
            string passwordCookie = Regex.Match(byPassword.headers, @"Set-Cookie: (hmailsession=[^;]+);").Groups[1].Value;
            Assert.AreEqual(200, Send("GET", "/api/v1/me", null, null, "Cookie: " + passwordCookie).status,
               "The negative control: the session works while the domain is on.");

            Started before = Start(false);
            Token(issuer, rsa, Issuer, ClientId, before.nonce, account.Address, true, 600);
            Ticket minted = Complete(before, "/portal");

            _domain.Active = false;
            _domain.Save();
            try
            {
               Assert.AreEqual(401, Send("POST", "/api/v1/session", Basic(account.Address, "Sso-Passw0rd!"), null).status,
                  "The password is refused for a switched-off domain, as it always was.");

               Assert.AreEqual(401, Send("POST", "/api/v1/session", null, "{\"sso_ticket\":\"" + minted.value + "\"}",
                  "Cookie: " + minted.cookie).status,
                  "A single sign-on into a switched-off domain started a session.");

               Started after = Start(false);
               Token(issuer, rsa, Issuer, ClientId, after.nonce, account.Address, true, 600);
               (int status, string location) refused = Callback(after);
               StringAssert.Contains("sso_error", refused.location, "A sign-in into a switched-off domain was handed a ticket.");

               Assert.AreEqual(401, Send("GET", "/api/v1/me", null, null, "Cookie: " + passwordCookie).status,
                  "A session outlived its domain being switched off.");
            }
            finally
            {
               _domain.Active = true;
               _domain.Save();
            }

            // Ended, not suspended: switching the domain back on does not bring the
            // session back, and a fresh sign-in works again.
            Assert.AreEqual(401, Send("GET", "/api/v1/me", null, null, "Cookie: " + passwordCookie).status);
            Started again = Start(false);
            Token(issuer, rsa, Issuer, ClientId, again.nonce, account.Address, true, 600);
            Ticket fresh = Complete(again, "/portal");
            Assert.AreEqual(201, Send("POST", "/api/v1/session", null, "{\"sso_ticket\":\"" + fresh.value + "\"}",
               "Cookie: " + fresh.cookie).status);
         }
      }

      // ---------------------------------------------------------------- helpers

      private struct Started
      {
         public string location;
         public string state;
         public string nonce;
         public string cookie;
         public bool deck;
      }

      /// <summary>The three documents an identity provider publishes.</summary>
      private static void Publish(FakeHttpEndpoint issuer, RSA rsa)
      {
         issuer.SetResponse("/.well-known/openid-configuration", 200, Encoding.UTF8.GetBytes(
            "{\"issuer\":\"" + Issuer + "\"," +
            "\"authorization_endpoint\":\"" + Issuer + "/authorize\"," +
            "\"token_endpoint\":\"" + Issuer + "/token\"," +
            "\"jwks_uri\":\"" + Issuer + "/jwks\"}"), "application/json");

         RSAParameters parameters = rsa.ExportParameters(false);
         issuer.SetResponse("/jwks", 200, Encoding.UTF8.GetBytes(
            "{\"keys\":[{\"kty\":\"RSA\",\"use\":\"sig\",\"alg\":\"RS256\",\"kid\":\"issuer-1\",\"n\":\"" +
            Base64Url(parameters.Modulus) + "\",\"e\":\"" + Base64Url(parameters.Exponent) + "\"}]}"), "application/json");
      }

      private static void Configure()
      {
         (int status, string body) applied = Admin("PUT", "/api/v1/settings",
            "{\"oidc_issuer\":\"" + Issuer + "\",\"oidc_client_id\":\"" + ClientId + "\"," +
            "\"oidc_redirect_base_url\":\"http://127.0.0.1:" + RestPort + "\"}");
         Assert.AreEqual(200, applied.status, applied.body);
      }

      /// <summary>What the token endpoint will answer next.</summary>
      private static void Token(FakeHttpEndpoint issuer, RSA rsa, string iss, string audience, string nonce,
                                string email, bool verified, long lifetime)
      {
         issuer.SetResponse("/token", 200, Encoding.UTF8.GetBytes(
            "{\"access_token\":\"a\",\"token_type\":\"Bearer\",\"id_token\":\"" +
            IdToken(rsa, iss, audience, nonce, email, verified, lifetime) + "\"}"), "application/json");
      }

      private static string IdToken(RSA rsa, string iss, string audience, string nonce, string email, bool verified,
                                    long lifetime)
      {
         string header = "{\"alg\":\"RS256\",\"typ\":\"JWT\",\"kid\":\"issuer-1\"}";
         long now = (long) (DateTime.UtcNow - new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc)).TotalSeconds;
         string claims = "{\"iss\":\"" + iss + "\",\"aud\":\"" + audience + "\",\"sub\":\"1234\"," +
                         "\"iat\":" + (now - 10) + ",\"exp\":" + (now + lifetime) + ",\"nonce\":\"" + nonce + "\"," +
                         "\"email\":\"" + email + "\",\"email_verified\":" + (verified ? "true" : "false") + "}";
         string signingInput = Base64Url(Encoding.UTF8.GetBytes(header)) + "." + Base64Url(Encoding.UTF8.GetBytes(claims));
         byte[] signature = rsa.SignData(Encoding.ASCII.GetBytes(signingInput), HashAlgorithmName.SHA256,
            RSASignaturePadding.Pkcs1);
         return signingInput + "." + Base64Url(signature);
      }

      /// <summary>GET /portal/oidc/start, and what the redirect and the cookie say.</summary>
      private static Started Start(bool deck)
      {
         (int status, string body, string headers) answer =
            SendRaw("GET", "/portal/oidc/start" + (deck ? "?deck=1" : ""), null, null, null);
         Assert.AreEqual(302, answer.status, answer.headers);

         Match location = Regex.Match(answer.headers, @"Location: (\S+)");
         Assert.IsTrue(location.Success, answer.headers);
         Match cookie = Regex.Match(answer.headers, @"Set-Cookie: (hmoidc=[^;]+);");
         Assert.IsTrue(cookie.Success, "The sign-in was started without its cookie: " + answer.headers);

         var started = new Started
         {
            location = location.Groups[1].Value,
            cookie = cookie.Groups[1].Value,
            deck = deck
         };
         started.state = Regex.Match(started.location, @"[?&]state=([^&]+)").Groups[1].Value;
         started.nonce = Regex.Match(started.location, @"[?&]nonce=([^&]+)").Groups[1].Value;
         Assert.IsNotEmpty(started.state, started.location);
         Assert.IsNotEmpty(started.nonce, started.location);
         return started;
      }

      /// <summary>GET /portal/oidc/callback as the browser would, and where it is sent.</summary>
      private static (int status, string location) Callback(Started started)
      {
         (int status, string body, string headers) answer = SendRaw("GET",
            "/portal/oidc/callback?state=" + started.state + "&code=a-code", null, null, "Cookie: " + started.cookie);
         Assert.AreEqual(302, answer.status, answer.headers);
         Match location = Regex.Match(answer.headers, @"Location: (\S+)");
         Assert.IsTrue(location.Success, answer.headers);
         return (answer.status, location.Groups[1].Value);
      }

      /// <summary>A ticket, and the hmsso cookie that binds it to the browser it was handed to.</summary>
      private struct Ticket
      {
         public string value;
         public string cookie;
      }

      /// <summary>The callback, expected to succeed; answers the one-use ticket and its cookie.</summary>
      private static Ticket Complete(Started started, string page)
      {
         (int status, string body, string headers) answer = SendRaw("GET",
            "/portal/oidc/callback?state=" + started.state + "&code=a-code", null, null, "Cookie: " + started.cookie);
         Assert.AreEqual(302, answer.status, answer.headers);
         string location = Regex.Match(answer.headers, @"Location: (\S+)").Groups[1].Value;
         StringAssert.StartsWith(page + "#sso=", location, "The sign-in did not come back with a ticket.");

         Match cookie = Regex.Match(answer.headers, @"Set-Cookie: (hmsso=[^;]+);([^\r\n]*)");
         Assert.IsTrue(cookie.Success, "The ticket came without the cookie that binds it to this browser: " + answer.headers);
         StringAssert.Contains("HttpOnly", cookie.Groups[2].Value);
         StringAssert.Contains("SameSite=Strict", cookie.Groups[2].Value);
         StringAssert.Contains("Path=/api/v1/session", cookie.Groups[2].Value);

         return new Ticket { value = location.Substring((page + "#sso=").Length), cookie = cookie.Groups[1].Value };
      }

      private static string Base64Url(byte[] data)
      {
         return Convert.ToBase64String(data).TrimEnd('=').Replace('+', '-').Replace('/', '_');
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
                                                    string extraHeader = null, bool requestedWith = true)
      {
         (int status, string body, string headers) answer = SendRaw(method, path, authorization, requestBody, extraHeader, requestedWith);
         return (answer.status, answer.body);
      }

      /// <summary>
      ///    One HTTP/1.0 request with the response headers as well as its body: this
      ///    fixture reads Location and Set-Cookie, which RestRequest drops.
      ///
      ///    Every write carries X-Requested-With, as both of this server's own pages
      ///    do. A single sign-on ticket is a credential read out of the request BODY,
      ///    which is the one credential a cross-site form could carry, so the server
      ///    takes the header as the proof that the post came from its own page;
      ///    requestedWith: false is how the check below sends one without it.
      /// </summary>
      private static (int status, string body, string headers) SendRaw(string method, string path, string authorization,
                                                                       string requestBody, string extraHeader,
                                                                       bool requestedWith = true)
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
               headers.Append("Host: " + Host + "\r\n");
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
   }
}
