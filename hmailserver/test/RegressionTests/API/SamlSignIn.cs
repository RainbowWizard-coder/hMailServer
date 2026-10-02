// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.IO.Compression;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Security.Cryptography.Xml;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Xml;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;
using hMailServer;

namespace RegressionTests.API
{
   /// <summary>
   ///    SAML sign-in beside OIDC (SamlSignIn.h, RestApiSamlSignIn.cpp): a SAML 2.0 identity
   ///    provider as the front door to the webmail and the Control Deck.
   ///
   ///    The identity provider is this file. It holds an RSA key and a self-signed
   ///    certificate made here, reads the AuthnRequest out of the redirect the server writes
   ///    (raw DEFLATE, inflated with DeflateStream, which reads exactly that), and answers
   ///    with a Response built as XML and signed by .NET's own SignedXml - exclusive
   ///    canonicalisation, the enveloped-signature transform, RSA-SHA256 - so every Response
   ///    below carries a real XML signature, not a mock of one, and every refusal is a
   ///    refusal of a real signature's document.
   ///
   ///    The browser is the fixture too: it follows the start's redirect with the hmsaml
   ///    cookie the start set, posts the form to the ACS, and follows the ACS on to the
   ///    finish with that cookie - which is all a browser does here. A Started is one
   ///    browser's sign-in; the finish is reached with its own cookie unless a test says
   ///    otherwise.
   ///
   ///    The identity provider's sign-in URL (IdpPort) is never fetched by the server - it
   ///    only sends the browser there - so nothing listens on it.
   /// </summary>
   [TestFixture]
   public class SamlSignIn : TestFixtureBase
   {
      private static int RestPort = 10050;
      private const int IdpPort = 10051;
      private const string AdminPassword = "testar";
      private const string Host = "127.0.0.1";
      private const string IdpEntityId = "https://idp.saml.test/entity";
      private const string EmailClaim = "http://schemas.xmlsoap.org/ws/2005/05/identity/claims/emailaddress";
      private const string EmailFormat = "urn:oasis:names:tc:SAML:1.1:nameid-format:emailAddress";
      private const string PersistentFormat = "urn:oasis:names:tc:SAML:2.0:nameid-format:persistent";
      private const string Protocol = "urn:oasis:names:tc:SAML:2.0:protocol";
      private const string AssertionNs = "urn:oasis:names:tc:SAML:2.0:assertion";
      private const string DsNs = "http://www.w3.org/2000/09/xmldsig#";

      private static string IdpSsoUrl
      {
         get { return "http://127.0.0.1:" + IdpPort + "/sso"; }
      }

      private static string BaseUrl
      {
         get { return "http://127.0.0.1:" + RestPort; }
      }

      private static string AcsUrl
      {
         get { return BaseUrl + "/portal/saml/acs"; }
      }

      private static string SpEntityId
      {
         get { return BaseUrl + "/saml"; }
      }

      private RSACryptoServiceProvider _key;
      private X509Certificate2 _certificate;

      [SetUp]
      public void StartRestApi()
      {
         // The key first: a SetUp that fails after the listener is arranged skips the
         // TearDown that would put it back.
         _key = NewKey(2048);
         _certificate = SelfSigned(_key, "CN=SAML fixture identity provider");

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
            (int status, string body) reset = Admin("PUT", "/api/v1/settings",
               "{\"saml_idp_entity_id\":\"\",\"saml_idp_sso_url\":\"\",\"saml_idp_certificate\":\"\",\"saml_sp_entity_id\":\"\"," +
               "\"saml_email_attribute\":\"" + EmailClaim + "\",\"saml_require_signed_assertion\":true," +
               "\"saml_auto_provision\":false,\"saml_administrators\":\"\",\"saml_button_label\":\"Sign in with SAML\"," +
               "\"oidc_redirect_base_url\":\"\"}");
            Assert.AreEqual(200, reset.status, "The SAML settings were not put back: " + reset.body);
         }
         finally
         {
            _certificate?.Dispose();
            _key?.Dispose();
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      // ------------------------------------------------------------------ the happy path

      [Test]
      [Description("Not set up, the start says so and the pages draw no button. Set up, the start redirects to the " +
                   "identity provider with an AuthnRequest that inflates to this service provider's entity ID and ACS, " +
                   "and sets the hmsaml cookie (Secure only when the request came over TLS or through a loopback proxy " +
                   "that says so); the ACS takes a Response carrying a signed Assertion for an existing account and " +
                   "sends the browser on to the finish, which with the cookie hands out a one-use ticket; the ticket " +
                   "becomes the session a password would have started. The ticket works once, and the same Response " +
                   "posted again is refused.")]
      public void ASignedAssertionSignsAnAccountInToTheWebmail()
      {
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "saml@" + _domain.Name, "Saml-Passw0rd!");

         // Not set up: the pages draw no button, and the start says why.
         (int status, string body) before = Send("GET", "/api/v1/signin-methods", null, null);
         Assert.AreEqual(200, before.status, before.body);
         StringAssert.Contains("\"saml\":false", before.body);
         (int status, string body, string headers) unconfigured = SendRaw("GET", "/portal/saml/start", null, null, null, null);
         Assert.AreEqual(302, unconfigured.status, unconfigured.headers);
         StringAssert.Contains("Location: /portal#sso_error=", unconfigured.headers);
         StringAssert.Contains("not set up", Why(Regex.Match(unconfigured.headers, @"Location: (\S+)").Groups[1].Value));

         Configure();

         (int status, string body) methods = Send("GET", "/api/v1/signin-methods", null, null);
         Assert.AreEqual(200, methods.status, methods.body);
         StringAssert.Contains("\"saml\":true", methods.body);
         StringAssert.Contains("\"saml_label\":\"Sign in with SAML\"", methods.body);

         Started started = Start(false);
         XmlElement request = started.request.DocumentElement;
         Assert.AreEqual("AuthnRequest", request.LocalName);
         Assert.AreEqual(Protocol, request.NamespaceURI);
         Assert.AreEqual("2.0", request.GetAttribute("Version"));
         Assert.AreEqual(AcsUrl, request.GetAttribute("AssertionConsumerServiceURL"));
         Assert.AreEqual(IdpSsoUrl, request.GetAttribute("Destination"));
         Assert.AreEqual("urn:oasis:names:tc:SAML:2.0:bindings:HTTP-POST", request.GetAttribute("ProtocolBinding"));
         Assert.AreEqual(SpEntityId, request.GetElementsByTagName("Issuer", AssertionNs)[0].InnerText);
         StringAssert.StartsWith("_", started.id, "An xs:ID begins with a letter or an underscore.");
         Assert.IsNotEmpty(started.relay);
         Assert.IsNull(request.GetElementsByTagName("Signature", DsNs)[0], "The AuthnRequest is not signed, and the metadata says so.");

         // The cookie: Lax, HttpOnly, for the SAML paths only, and not Secure on plain HTTP...
         StringAssert.Contains("HttpOnly", started.setCookie);
         StringAssert.Contains("SameSite=Lax", started.setCookie);
         StringAssert.Contains("Path=/portal/saml", started.setCookie);
         StringAssert.DoesNotContain("Secure", started.setCookie, "A cookie over plain HTTP was marked Secure.");
         // ...and Secure when the proxy in front of the loopback listener says the browser came over HTTPS.
         (int status, string body, string headers) proxied = SendRaw("GET", "/portal/saml/start", null, null, null, "X-Forwarded-Proto: https");
         Assert.AreEqual(302, proxied.status, proxied.headers);
         StringAssert.IsMatch(@"Set-Cookie: hmsaml=[^\r\n]*; Secure", proxied.headers, "Through a proxy that says https, the cookie is Secure.");

         string response = SignedResponse(new Spec(started, account.Address));
         (int status, string location) acs = Acs(response, started.relay);
         Assert.AreEqual(303, acs.status, "The ACS sends a POST on with a GET.");
         StringAssert.StartsWith("/portal/saml/finish?token=", acs.location, "The ACS does not finish the sign-in itself: " + Why(acs.location));

         (int status, string location) finished = Finish(acs.location, started.cookie);
         Assert.AreEqual(303, finished.status);
         StringAssert.StartsWith("/portal#sso=", finished.location, Why(finished.location));
         string ticket = finished.location.Substring("/portal#sso=".Length);

         // The ticket carried out of this browser - the link /portal#sso=<ticket> sent to
         // somebody else, whose browser this server's own page would post it from, header
         // and all. The hmsaml cookie was checked at the finish in the browser that
         // started the sign-in, so what keeps the ticket there is the hmsso cookie the
         // finish set beside it: without it, or with another sign-in's, it is refused,
         // and it is not spent - the sign-in just below uses it.
         Assert.AreEqual(401, Send("POST", "/api/v1/session", null, "{\"sso_ticket\":\"" + ticket + "\"}", null).status,
            "A ticket presented by a browser that was not given its cookie signed somebody in.");
         Started other = Start(false);
         string otherTicket = SignIn(other, SignedResponse(new Spec(other, account.Address)));
         Assert.AreEqual(401, Send("POST", "/api/v1/session", null, "{\"sso_ticket\":\"" + ticket + "\"}", TicketCookie(otherTicket)).status,
            "A ticket presented with another sign-in's cookie signed somebody in.");

         (int status, string body, string headers) session = SendRaw("POST", "/api/v1/session", null,
            "{\"sso_ticket\":\"" + ticket + "\"}", "application/json", TicketCookie(ticket));
         Assert.AreEqual(201, session.status, session.body);
         StringAssert.Contains(account.Address, session.body);

         Match cookie = Regex.Match(session.headers, @"Set-Cookie: (hmailsession=[^;]+);");
         Assert.IsTrue(cookie.Success, "The ticket started no session: " + session.headers);
         (int status, string body) whoami = Send("GET", "/api/v1/me", null, null, "Cookie: " + cookie.Groups[1].Value);
         Assert.AreEqual(200, whoami.status, whoami.body);
         StringAssert.Contains(account.Address, whoami.body);

         // The device list says which credential it was: single sign-on, as OIDC's.
         StringAssert.Contains("\"credential\":\"single_sign_on\"",
            Send("GET", "/api/v1/me/devices", Basic(account.Address, "Saml-Passw0rd!"), null).body);

         // A ticket is good once.
         Assert.AreEqual(401, Send("POST", "/api/v1/session", null, "{\"sso_ticket\":\"" + ticket + "\"}", TicketCookie(ticket)).status,
            "A spent ticket is not a credential.");

         // The same Response again: its request was answered already.
         (int status, string location) replayed = Acs(response, started.relay);
         StringAssert.StartsWith("/portal#sso_error=", replayed.location, "A replayed Response was taken.");
         StringAssert.Contains("finished already", Why(replayed.location));
      }

      [Test]
      [Description("The finish is the browser binding: a Response verified at the ACS for a sign-in started in one " +
                   "browser, finished in another - with that browser's own cookie, or with none - is refused, and no " +
                   "mailbox is made for it even with provisioning on; the finish token works once. The negative " +
                   "control is each sign-in finished in the browser that started it, which signs in.")]
      public void AResponseCarriedToAnotherBrowserIsRefusedAtTheFinish()
      {
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "saml-browser@" + _domain.Name, "Saml-Passw0rd!");
         Configure();

         // Browser A starts the sign-in and keeps the Response; browser B, which started
         // one of its own, is made to post it.
         Started a = Start(false);
         Started b = Start(false);
         Assert.AreNotEqual(a.cookie, b.cookie, "Two browsers were given one cookie.");

         (int status, string location) acs = Acs(SignedResponse(new Spec(a, account.Address)), a.relay);
         StringAssert.StartsWith("/portal/saml/finish?token=", acs.location, Why(acs.location));

         (int status, string location) inB = Finish(acs.location, b.cookie);
         StringAssert.StartsWith("/portal#sso_error=", inB.location, "A Response finished in another browser signed it in.");
         StringAssert.Contains("started in another browser", Why(inB.location));

         // The token was spent by that attempt: not even browser A can use it now.
         (int status, string location) again = Finish(acs.location, a.cookie);
         StringAssert.StartsWith("/portal#sso_error=", again.location);
         StringAssert.Contains("finished already", Why(again.location));

         // With no cookie at all.
         Started c = Start(false);
         (int status, string location) acsC = Acs(SignedResponse(new Spec(c, account.Address)), c.relay);
         (int status, string location) noCookie = Finish(acsC.location, null);
         StringAssert.StartsWith("/portal#sso_error=", noCookie.location);
         StringAssert.Contains("started in another browser", Why(noCookie.location));

         // The mapping waits for the finish: with provisioning on, a forwarded Response for
         // an address with no mailbox makes none...
         string newcomer = "saml-forwarded@" + _domain.Name;
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"saml_auto_provision\":true}").status);
         Started forwarded = Start(false);
         Started victim = Start(false);
         (int status, string location) acsForwarded = Acs(SignedResponse(new Spec(forwarded, newcomer)), forwarded.relay);
         StringAssert.StartsWith("/portal#sso_error=", Finish(acsForwarded.location, victim.cookie).location);
         Assert.AreEqual(404, Admin("GET", "/api/v1/accounts/" + newcomer, null).status, "A mailbox was made for a Response that ended in the wrong browser.");

         // ...and the negative control: finished in its own browser, it signs in and the
         // mailbox is made.
         Started own = Start(false);
         string ticket = SignIn(own, SignedResponse(new Spec(own, newcomer)));
         Assert.AreEqual(201, Send("POST", "/api/v1/session", null, "{\"sso_ticket\":\"" + ticket + "\"}", TicketCookie(ticket)).status);
         Assert.AreEqual(200, Admin("GET", "/api/v1/accounts/" + newcomer, null).status, "The mailbox was made.");

         // And an existing account's, in its own browser.
         Started d = Start(false);
         Assert.IsNotEmpty(SignIn(d, SignedResponse(new Spec(d, account.Address))));
      }

      [Test]
      [Description("The address named by the attribute saml_email_attribute names when the NameID is not an email " +
                   "address (Entra ID's and ADFS's claim by default), beside its negative controls - the claim under " +
                   "another name, two values of it, and markup inside it or inside an emailAddress NameID; and a " +
                   "Response signed both on the Response and on the Assertion, with an InclusiveNamespaces " +
                   "PrefixList - the way Okta signs - is taken.")]
      public void TheEmailAttributeAndAnOktaShapedResponseAreTaken()
      {
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "saml-claim@" + _domain.Name, "Saml-Passw0rd!");
         Configure();

         // A persistent NameID, and the address in the claim.
         Started byClaim = Start(false);
         Spec claim = new Spec(byClaim, "a1b2c3d4-persistent") { nameIdFormat = PersistentFormat, emailAttribute = account.Address };
         string ticket = SignIn(byClaim, SignedResponse(claim));
         Assert.AreEqual(201, Send("POST", "/api/v1/session", null, "{\"sso_ticket\":\"" + ticket + "\"}", TicketCookie(ticket)).status);

         Refused("the claim under another name", s => SignedResponse(new Spec(s, "a1b2c3d4-persistent")
         {
            nameIdFormat = PersistentFormat, emailAttribute = account.Address, emailAttributeName = "urn:oid:0.9.2342.19200300.100.1.3"
         }), "neither by an emailAddress NameID nor by the attribute");

         Refused("two values of the claim", s => SignedResponse(new Spec(s, "a1b2c3d4-persistent")
         {
            nameIdFormat = PersistentFormat, emailAttribute = account.Address, secondEmailValue = "other@" + _domain.Name
         }), "more than one value");

         Refused("markup inside the claim's value", s => SignedResponse(new Spec(s, "a1b2c3d4-persistent")
         {
            nameIdFormat = PersistentFormat, emailAttribute = account.Address + "<x:b xmlns:x=\"urn:x\"/>"
         }), "holds markup");

         Refused("markup inside an emailAddress NameID", s => SignedResponse(new Spec(s, account.Address + "<x:b xmlns:x=\"urn:x\"/>")),
            "NameID holds markup");

         // Okta: the Assertion signed with PrefixList="xs", then the whole Response signed.
         Started okta = Start(false);
         Spec both = new Spec(okta, account.Address) { prefixList = "xs", signResponse = true };
         string oktaTicket = SignIn(okta, SignedResponse(both));
         Assert.AreEqual(201, Send("POST", "/api/v1/session", null, "{\"sso_ticket\":\"" + oktaTicket + "\"}", TicketCookie(oktaTicket)).status);
      }

      [Test]
      [Description("The Control Deck: an address in saml_administrators signs in as the administrator, one that is not " +
                   "is refused and comes back to the Deck. Provisioning: off refuses an address with no mailbox, on " +
                   "makes the mailbox as OIDC does - at a domain this server has, and not at one it does not.")]
      public void TheControlDeckAndProvisioning()
      {
         string newcomer = "saml-new@" + _domain.Name;
         string chief = "saml-chief@" + _domain.Name;
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, chief, "Saml-Passw0rd!");
         Configure();

         // Off: no mailbox is made.
         Refused("an address with no mailbox, provisioning off", s => SignedResponse(new Spec(s, newcomer)), "no mailbox");
         Assert.AreEqual(404, Admin("GET", "/api/v1/accounts/" + newcomer, null).status, "No mailbox was made.");

         // On: one is, at a domain this server already has.
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"saml_auto_provision\":true}").status);
         Started provisioned = Start(false);
         string ticket = SignIn(provisioned, SignedResponse(new Spec(provisioned, newcomer)));
         Assert.AreEqual(201, Send("POST", "/api/v1/session", null, "{\"sso_ticket\":\"" + ticket + "\"}", TicketCookie(ticket)).status);
         Assert.AreEqual(200, Admin("GET", "/api/v1/accounts/" + newcomer, null).status, "The mailbox was made.");

         // An address at a domain this server does not host is refused, provisioning or not.
         Refused("an address at a domain not hosted, provisioning on", s => SignedResponse(new Spec(s, "someone@saml-not-hosted.test")),
            "domain this server does not have");
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"saml_auto_provision\":false}").status);
         Refused("an address at a domain not hosted, provisioning off", s => SignedResponse(new Spec(s, "someone@saml-not-hosted.test")),
            "no mailbox");

         // The Control Deck: the list decides, not the identity provider.
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"saml_administrators\":\"" + chief + "\"}").status);

         Refused("an address not in saml_administrators, for the Deck", s => SignedResponse(new Spec(s, newcomer)),
            "does not open the Control Deck", true);

         Started asChief = Start(true);
         string deckTicket = SignIn(asChief, SignedResponse(new Spec(asChief, chief)));

         (int status, string body, string headers) session = SendRaw("POST", "/api/v1/session", null,
            "{\"sso_ticket\":\"" + deckTicket + "\"}", "application/json", TicketCookie(deckTicket));
         Assert.AreEqual(201, session.status, session.body);
         StringAssert.Contains("\"administrator\":true", session.body);

         Match cookie = Regex.Match(session.headers, @"Set-Cookie: (hmailsession=[^;]+);");
         Assert.IsTrue(cookie.Success, session.headers);
         Assert.AreEqual(200, Send("GET", "/api/v1/status", null, null, "Cookie: " + cookie.Groups[1].Value).status,
            "The Deck's session reaches the administration API.");
      }

      // ------------------------------------------------------------------ the signature

      [Test]
      [Description("What the signature check refuses, each a Response of its own beside its negative control: no " +
                   "signature; a signature by another key, whose certificate the Response carries in KeyInfo; a SHA-1 " +
                   "digest; an XSLT transform; a Reference with an empty URI; a changed NameID; a Response and an " +
                   "Assertion both signed, with either signature bad; and a signature over the Response alone with the " +
                   "Assertion unsigned, refused while saml_require_signed_assertion is on and taken when it is off; and " +
                   "a rolled-over certificate.")]
      public void SignaturesThatDoNotCheckOutAreRefused()
      {
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "saml-sig@" + _domain.Name, "Saml-Passw0rd!");
         Configure();

         Refused("an unsigned Response", s => ResponseXml(new Spec(s, account.Address)), "is not signed");

         using (RSACryptoServiceProvider stranger = NewKey(2048))
         using (X509Certificate2 strangerCertificate = SelfSigned(stranger, "CN=A stranger"))
         {
            // The key the message carries is not the key: the stranger's own certificate
            // in KeyInfo changes nothing.
            Refused("a Response signed with another key, carrying that key's certificate",
               s => SignedResponse(new Spec(s, account.Address) { assertionKey = stranger, keyInfoCertificate = strangerCertificate }),
               "does not verify against");

            // Both signed, one of them bad.
            Refused("a good Response signature over a bad Assertion signature",
               s => SignedResponse(new Spec(s, account.Address) { assertionKey = stranger, signResponse = true }),
               "Assertion's signature does not verify");
            Refused("a bad Response signature over a good Assertion signature",
               s => SignedResponse(new Spec(s, account.Address) { responseKey = stranger, signResponse = true }),
               "Response's signature does not verify");
         }

         Refused("a SHA-1 digest",
            s => SignedResponse(new Spec(s, account.Address) { digestMethod = "http://www.w3.org/2000/09/xmldsig#sha1" }), "SHA-1");

         Refused("an XSLT transform",
            s => SignedResponse(new Spec(s, account.Address)).Replace(
               "<Transform Algorithm=\"http://www.w3.org/2000/09/xmldsig#enveloped-signature\" />",
               "<Transform Algorithm=\"http://www.w3.org/TR/1999/REC-xslt-19991116\" />"), "XSLT");

         Refused("a Reference with an empty URI",
            s => Regex.Replace(SignedResponse(new Spec(s, account.Address)), "<Reference URI=\"#[^\"]+\">", "<Reference URI=\"\">"),
            "empty URI");

         Refused("a changed NameID under a good signature",
            s => SignedResponse(new Spec(s, account.Address)).Replace(">" + account.Address + "<", ">x" + account.Address + "<"),
            "digest does not match");

         // The Response signed, the Assertion not.
         Refused("a signature over the Response alone while the Assertion's own is required",
            s => SignedResponse(new Spec(s, account.Address) { signAssertion = false, signResponse = true }), "assertion itself is not signed");

         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"saml_require_signed_assertion\":false}").status);
         Started responseOnly = Start(false);
         string ticket = SignIn(responseOnly, SignedResponse(new Spec(responseOnly, account.Address) { signAssertion = false, signResponse = true }));
         Assert.AreEqual(201, Send("POST", "/api/v1/session", null, "{\"sso_ticket\":\"" + ticket + "\"}", TicketCookie(ticket)).status,
            "With saml_require_signed_assertion off, a signature over the Response is enough.");

         // Off does not mean unsigned: neither signed is still refused.
         Refused("an unsigned Response with the switch off", s => ResponseXml(new Spec(s, account.Address)), "is not signed");

         // Rotation: two certificates stored, the new one signing.
         using (RSACryptoServiceProvider next = NewKey(2048))
         using (X509Certificate2 nextCertificate = SelfSigned(next, "CN=SAML fixture identity provider (next)"))
         {
            Assert.AreEqual(200, Admin("PUT", "/api/v1/settings",
               "{\"saml_idp_certificate\":\"" + Json(Pem(_certificate) + Pem(nextCertificate)) + "\"}").status);
            Started rolled = Start(false);
            string rolledTicket = SignIn(rolled, SignedResponse(new Spec(rolled, account.Address) { assertionKey = next }));
            Assert.AreEqual(201, Send("POST", "/api/v1/session", null, "{\"sso_ticket\":\"" + rolledTicket + "\"}", TicketCookie(rolledTicket)).status,
               "The second of two stored certificates verifies.");
         }
      }

      [Test]
      [Description("The document's shape, each refused by the check named, and none of it signing the forgery's " +
                   "address in: the wrapping attack (a signed Assertion moved under an unsigned element and a forged " +
                   "Assertion put where SAML looks, with the same ID and with another) and two Assertions, refused by " +
                   "the count of Assertions; a lone Assertion that is not directly under the Response; a signature " +
                   "that is neither the Response's nor the Assertion's own; the Assertion's ID and the Response's ID " +
                   "each written on a second element. The negative control is the attacker's own Response, unwrapped.")]
      public void TheDocumentsShapeIsHeld()
      {
         Account attacker = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "saml-attacker@" + _domain.Name, "Saml-Passw0rd!");
         Account victim = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "saml-victim@" + _domain.Name, "Saml-Passw0rd!");
         Configure();

         Refused("a wrapped Assertion with a forgery of the same ID", s => Wrap(SignedResponse(new Spec(s, attacker.Address)), victim.Address, false),
            "more than one assertion");
         Refused("a wrapped Assertion with a forgery of another ID", s => Wrap(SignedResponse(new Spec(s, attacker.Address)), victim.Address, true),
            "more than one assertion");

         Refused("two Assertions", s =>
         {
            string second = AssertionXml(new Spec(s, victim.Address));
            return SignedResponse(new Spec(s, attacker.Address)).Replace("</samlp:Response>", second + "</samlp:Response>");
         }, "more than one assertion");

         Refused("a lone Assertion under an Extensions element", s => MoveAssertion(SignedResponse(new Spec(s, attacker.Address))),
            "not directly under the Response");

         Refused("a signature that is neither the Response's nor the Assertion's", s => StraySignature(SignedResponse(new Spec(s, attacker.Address))),
            "neither the Response's nor the Assertion's own");

         Refused("the Assertion's ID written twice", s =>
         {
            Spec spec = new Spec(s, attacker.Address);
            return SignedResponse(spec).Replace("<samlp:Status>",
               "<samlp:Extensions><x:Other xmlns:x=\"urn:example:other\" ID=\"" + spec.assertionId + "\"/></samlp:Extensions><samlp:Status>");
         }, "written on more than one element");

         Refused("the Response's ID written twice", s =>
         {
            Spec spec = new Spec(s, attacker.Address);
            return SignedResponse(spec).Replace("<samlp:Status>",
               "<samlp:Extensions><x:Other xmlns:x=\"urn:example:other\" ID=\"" + spec.responseId + "\"/></samlp:Extensions><samlp:Status>");
         }, "written on more than one element");

         // The negative control: the attacker's own Response, unwrapped, is a good one - so
         // each refusal above was of the shape, not of the signature underneath it.
         Started own = Start(false);
         string ticket = SignIn(own, SignedResponse(new Spec(own, attacker.Address)));
         (int status, string body) session = Send("POST", "/api/v1/session", null, "{\"sso_ticket\":\"" + ticket + "\"}", TicketCookie(ticket));
         Assert.AreEqual(201, session.status, session.body);
         StringAssert.Contains(attacker.Address, session.body);
         StringAssert.DoesNotContain(victim.Address, session.body, "The forgery's address signed in.");
      }

      // ------------------------------------------------------------------ the assertion's rules

      [Test]
      [Description("What the Response's and the assertion's rules refuse, each reaching its own check: a status other " +
                   "than Success with the identity provider's reason; an InResponseTo this server did not issue, one " +
                   "issued for another sign-in, and a signed SubjectConfirmationData answering another sign-in than " +
                   "the Response; a Destination and a Recipient at another host; the Response's Issuer and the " +
                   "Assertion's Issuer each wrong under the other right; a confirmation that is not bearer; an expired " +
                   "confirmation; Conditions not yet valid and expired, and each just inside and just outside the " +
                   "minute of skew; another audience, and a second AudienceRestriction without this server while one " +
                   "with it passes; no AuthnStatement; an IdP-initiated Response; an EncryptedAssertion.")]
      public void AssertionsThatDoNotCheckOutAreRefused()
      {
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "saml-rules@" + _domain.Name, "Saml-Passw0rd!");
         Configure();

         Refused("a status other than Success", s => SignedResponse(new Spec(s, account.Address)
         {
            statusCode = "urn:oasis:names:tc:SAML:2.0:status:Responder", secondStatusCode = "urn:oasis:names:tc:SAML:2.0:status:AuthnFailed"
         }), "did not sign the person in (AuthnFailed)");

         Refused("an InResponseTo this server did not issue",
            s => SignedResponse(new Spec(s, account.Address) { inResponseTo = "_not-issued-here", confirmationInResponseTo = "_not-issued-here" }),
            "not started here");

         // A request issued for another sign-in, posted with this one's RelayState.
         {
            Started first = Start(false);
            Started second = Start(false);
            (int status, string location) crossed = Acs(SignedResponse(new Spec(second, account.Address)), first.relay);
            StringAssert.StartsWith("/portal#sso_error=", crossed.location, "A Response answered with another sign-in's RelayState was taken.");
            StringAssert.Contains("RelayState of another sign-in", Why(crossed.location));
         }

         // The signed SubjectConfirmationData answering another request than the Response
         // it came in: with only the Assertion signed, it is the only signed binding.
         {
            Started a = Start(false);
            Started b = Start(false);
            (int status, string location) mixed = Acs(SignedResponse(new Spec(b, account.Address) { confirmationInResponseTo = a.id }), b.relay);
            StringAssert.StartsWith("/portal#sso_error=", mixed.location);
            StringAssert.Contains("answers another sign-in than the Response it came in", Why(mixed.location));
         }

         Refused("a Destination at another host",
            s => SignedResponse(new Spec(s, account.Address) { destination = "https://elsewhere.example/portal/saml/acs" }), "another address");

         Refused("a Recipient at another host",
            s => SignedResponse(new Spec(s, account.Address) { recipient = "https://elsewhere.example/portal/saml/acs" }), "another recipient");

         Refused("the Response's Issuer wrong, the Assertion's right",
            s => SignedResponse(new Spec(s, account.Address) { responseIssuer = "https://impostor.example/entity" }),
            "the Response was issued by another identity provider");
         Refused("the Assertion's Issuer wrong, the Response's right",
            s => SignedResponse(new Spec(s, account.Address) { assertionIssuer = "https://impostor.example/entity" }),
            "the assertion was issued by another identity provider");

         Refused("a confirmation that is not bearer",
            s => SignedResponse(new Spec(s, account.Address) { confirmationMethod = "urn:oasis:names:tc:SAML:2.0:cm:holder-of-key" }),
            "has no bearer SubjectConfirmation");

         Refused("an expired confirmation", s => SignedResponse(new Spec(s, account.Address) { lifetime = -600 }), "bearer confirmation has expired");

         // Conditions, with the confirmation valid: not yet, and expired, and the minute of
         // skew from both sides.
         Refused("Conditions not valid for five minutes yet",
            s => SignedResponse(new Spec(s, account.Address) { conditionsNotBefore = 300 }), "Conditions are not valid yet");
         Refused("Conditions expired five minutes ago",
            s => SignedResponse(new Spec(s, account.Address) { conditionsNotOnOrAfter = -300 }), "Conditions have expired");
         Refused("Conditions not valid for 150 seconds yet, past the skew",
            s => SignedResponse(new Spec(s, account.Address) { conditionsNotBefore = 150 }), "Conditions are not valid yet");
         Refused("Conditions expired 150 seconds ago, past the skew",
            s => SignedResponse(new Spec(s, account.Address) { conditionsNotOnOrAfter = -150 }), "Conditions have expired");
         {
            Started early = Start(false);
            Assert.IsNotEmpty(SignIn(early, SignedResponse(new Spec(early, account.Address) { conditionsNotBefore = 20 })),
               "Conditions valid in 20 seconds are inside the minute of skew.");
            Started late = Start(false);
            Assert.IsNotEmpty(SignIn(late, SignedResponse(new Spec(late, account.Address) { conditionsNotOnOrAfter = -20 })),
               "Conditions expired 20 seconds ago are inside the minute of skew.");
         }

         Refused("another audience", s => SignedResponse(new Spec(s, account.Address) { audience = "https://another-sp.example/saml" }),
            "another service provider");
         Refused("a second AudienceRestriction without this server",
            s => SignedResponse(new Spec(s, account.Address) { extraRestriction = new[] { "https://another-sp.example/saml" } }),
            "another service provider");
         {
            Started both = Start(false);
            Assert.IsNotEmpty(SignIn(both, SignedResponse(new Spec(both, account.Address)
            {
               extraRestriction = new[] { "https://another-sp.example/saml", SpEntityId }
            })), "Two AudienceRestrictions that both name this server pass.");
         }

         Refused("no AuthnStatement", s => SignedResponse(new Spec(s, account.Address) { noAuthnStatement = true }), "no AuthnStatement");

         Refused("an IdP-initiated Response",
            s => SignedResponse(new Spec(s, account.Address) { inResponseTo = "", confirmationInResponseTo = "" }), "IdP-initiated");

         Refused("an EncryptedAssertion", s => ResponseXml(new Spec(s, account.Address) { encrypted = true }), "encrypted the assertion");
      }

      [Test]
      [Description("The reader and the ACS's ceiling: a Response with a DOCTYPE defining an entity is refused; a signed " +
                   "Response of about 170 KB on the wire - over the listener's ordinary 64 KB - is taken, and bodies of " +
                   "300 KB and 1.1 MB are refused with 413 before anything reads them; and a NameID with a comment " +
                   "inside it (user@domain<!---->.evil) is read as the whole user@domain.evil and does not sign " +
                   "user@domain in.")]
      public void TheReaderTheCeilingAndTheCommentInjection()
      {
         Account victim = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "saml-comment@" + _domain.Name, "Saml-Passw0rd!");
         Configure();

         Refused("a DOCTYPE with an entity",
            s => "<!DOCTYPE samlp:Response [<!ENTITY who \"" + victim.Address + "\">]>" + SignedResponse(new Spec(s, victim.Address)),
            "document type declaration");

         // Over the ordinary 64 KB and under the ACS's 256 KB: taken, which only the route's
         // own ceiling allows.
         {
            Started large = Start(false);
            string padded = SignedResponse(new Spec(large, victim.Address) { padding = 120 * 1024 });
            string body = FormBody(padded, large.relay);
            Assert.Greater(body.Length, 100 * 1024, "The large Response is not over the ordinary cap.");
            Assert.Less(body.Length, 250 * 1024, "The large Response is not under the ACS's ceiling.");
            (int status, string location) acs = Acs(padded, large.relay);
            StringAssert.StartsWith("/portal/saml/finish?token=", acs.location, "A Response of " + body.Length + " bytes was refused: " + Why(acs.location));
            (int status, string location) finished = Finish(acs.location, large.cookie);
            StringAssert.StartsWith("/portal#sso=", finished.location, Why(finished.location));
         }

         // Over the ACS's ceiling: 413, or a reset under the body the listener will not read.
         Assert.AreEqual(413, PostOversize(300 * 1024), "A body of 300 KB was read.");
         Assert.AreEqual(413, PostOversize(1100 * 1024), "A body of 1.1 MB was read.");

         // The comment: the signature covers user@domain.evil, which is what is read.
         Refused("a NameID split by a comment", s => SignedResponse(new Spec(s, victim.Address + "<!---->.evil")), "no mailbox");
      }

      [Test]
      [Description("The tables: an assertion's ID taken once - the same assertion in a Response to a second, fresh " +
                   "sign-in is refused by the replay cache - beside its negative control, the first; and the table of " +
                   "sign-ins waiting for their Response makes room out of the asker's own: one source that asks three " +
                   "hundred times gives up its own oldest while its newest still works, and a person's sign-in from " +
                   "another source, started before them and before three hundred more from as many sources, still " +
                   "finishes. The source behind a proxy on this machine is the address the proxy wrote last in " +
                   "X-Forwarded-For. Until 6.3.6 the table held two hundred and dropped its oldest whoever held it.")]
      public void ReplayAndTheTables()
      {
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "saml-tables@" + _domain.Name, "Saml-Passw0rd!");
         Configure();

         string assertionId = "_a" + Guid.NewGuid().ToString("N");
         Started first = Start(false);
         Assert.IsNotEmpty(SignIn(first, SignedResponse(new Spec(first, account.Address) { assertionId = assertionId })));

         Refused("an assertion already used, in a Response to another sign-in",
            s => SignedResponse(new Spec(s, account.Address) { assertionId = assertionId }), "used to sign in already");

         // A person behind the proxy, then one source asking over and over, then
         // many sources once each.
         Started person = Start(false, "X-Forwarded-For: 203.0.113.9, 198.51.100.4");
         Started oldest = Start(false, "X-Forwarded-For: 203.0.113.9");
         Started newest = oldest;
         for (int i = 0; i < 300; i++)
            newest = Start(false, "X-Forwarded-For: 203.0.113.9");
         for (int i = 0; i < 300; i++)
            Start(false, "X-Forwarded-For: 2001:db8:" + (i + 1).ToString("x") + "::7");

         (int status, string location) evicted = Acs(SignedResponse(new Spec(oldest, account.Address)), oldest.relay);
         StringAssert.StartsWith("/portal#sso_error=", evicted.location, "The source that asked over and over kept its oldest sign-in.");
         StringAssert.Contains("not started here", Why(evicted.location));
         Assert.IsNotEmpty(SignIn(newest, SignedResponse(new Spec(newest, account.Address))), "The newest sign-in did not work.");
         Assert.IsNotEmpty(SignIn(person, SignedResponse(new Spec(person, account.Address))),
            "The person's sign-in from another source did not survive the others' starts.");
      }

      // ------------------------------------------------------------------ the settings and the metadata

      [Test]
      [Description("The settings over REST: the certificate write-only, answered by its fingerprints, and never as PEM - " +
                   "not by GET /api/v1/settings, not by the list of what is changed from stock and not by the audit " +
                   "trail; a private key, text that is not a certificate, three certificates, an RSA key of 1024 bits " +
                   "and an elliptic curve of 160 refused; a certificate pasted with its line breaks dropped or made " +
                   "spaces read, stored and verifying; the sign-in URL held to one line and https or the loopback; the " +
                   "button's own words; and the metadata, 404 until there is a base URL and then this service " +
                   "provider's entity ID and ACS and no KeyDescriptor.")]
      public void SettingsAndMetadata()
      {
         // No base URL: no metadata.
         Assert.AreEqual(404, Send("GET", "/saml/metadata", null, null).status);

         Configure();
         string certificateText = Convert.ToBase64String(_certificate.Export(X509ContentType.Cert)).Substring(0, 60);

         (int status, string body) settings = Admin("GET", "/api/v1/settings", null);
         Assert.AreEqual(200, settings.status, settings.body);
         StringAssert.Contains("\"saml_idp_entity_id\":\"" + IdpEntityId + "\"", settings.body);
         StringAssert.DoesNotContain("BEGIN CERTIFICATE", settings.body, "The certificate was answered back.");
         StringAssert.DoesNotContain(certificateText, settings.body, "The certificate was answered back.");
         StringAssert.DoesNotContain("\"saml_idp_certificate\":", settings.body, "The certificate is write-only.");
         StringAssert.Contains(Sha256Fingerprint(_certificate), settings.body, "The fingerprint is what is answered.");
         StringAssert.Contains(_certificate.Thumbprint.ToUpperInvariant(), settings.body, "The SHA-1 thumbprint is answered too.");

         // Not by the list of what is changed from stock either, which names it as set...
         (int status, string body) changed = Admin("GET", "/api/v1/settings/changed", null);
         Assert.AreEqual(200, changed.status, changed.body);
         StringAssert.Contains("\"name\":\"SamlIdpCertificate\"", changed.body, "The certificate is not listed as changed from stock.");
         StringAssert.DoesNotContain("BEGIN CERTIFICATE", changed.body, "The list of what is changed from stock answered the certificate back.");
         StringAssert.DoesNotContain(certificateText, changed.body, "The list of what is changed from stock answered the certificate back.");

         // ...nor by the audit trail, which records that it changed.
         (int status, string body) audit = Admin("GET", "/api/v1/audit?limit=200&object_type=setting", null);
         Assert.AreEqual(200, audit.status, audit.body);
         StringAssert.Contains("SamlIdpCertificate: (changed)", audit.body, "The audit trail did not record the change as a change.");
         StringAssert.DoesNotContain("BEGIN CERTIFICATE", audit.body, "The audit trail recorded the certificate's text.");
         StringAssert.DoesNotContain(certificateText, audit.body, "The audit trail recorded the certificate's text.");

         // Refusals, each with nothing changed.
         using (RSACryptoServiceProvider weakKey = NewKey(1024))
         using (X509Certificate2 weak = SelfSigned(weakKey, "CN=Weak"))
         {
            AssertRefused("{\"saml_idp_certificate\":\"" + Json(Pem(weak)) + "\"}", "1024 bits");
         }

         using (ECDsa curve = ECDsa.Create(ECCurve.NamedCurves.brainpoolP160r1))
         {
            var request = new CertificateRequest("CN=Small curve", curve, HashAlgorithmName.SHA256);
            using (X509Certificate2 smallCurve = request.CreateSelfSigned(DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddYears(1)))
            {
               AssertRefused("{\"saml_idp_certificate\":\"" + Json(Pem(smallCurve)) + "\"}", "elliptic-curve key of 160 bits");
            }
         }

         AssertRefused("{\"saml_idp_certificate\":\"" + Json(Pem(_certificate) + "-----BEGIN PRIVATE KEY-----\nMIIB\n-----END PRIVATE KEY-----\n") + "\"}",
            "PRIVATE key");
         AssertRefused("{\"saml_idp_certificate\":\"not a certificate at all\"}", "could not be read");
         AssertRefused("{\"saml_idp_certificate\":\"" + Json(Pem(_certificate) + Pem(_certificate) + Pem(_certificate)) + "\"}", "at most two");
         AssertRefused("{\"saml_idp_sso_url\":\"ftp://idp.example/sso\"}", "https://");
         AssertRefused("{\"saml_idp_sso_url\":\"https://idp.example/sso\\r\\nSet-Cookie: x=y\"}", "one line");
         AssertRefused("{\"saml_button_label\":\"" + new string('b', 61) + "\"}", "at most 60 characters");

         // Nothing changed by any of them.
         settings = Admin("GET", "/api/v1/settings", null);
         StringAssert.Contains(Sha256Fingerprint(_certificate), settings.body);
         StringAssert.Contains("\"saml_idp_sso_url\":\"" + IdpSsoUrl + "\"", settings.body);

         // Pasted into the Deck, which draws a write-only setting as a password box: the
         // line breaks dropped (Chrome) or made spaces (Firefox). Both are read, stored as
         // PEM again, and verify a sign-in.
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "saml-pasted@" + _domain.Name, "Saml-Passw0rd!");
         foreach (string pasted in new[] { Pem(_certificate).Replace("\n", ""), Pem(_certificate).Replace("\n", " ") })
         {
            Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"saml_idp_certificate\":\"" + Json(pasted) + "\"}").status);
            StringAssert.Contains(Sha256Fingerprint(_certificate), Admin("GET", "/api/v1/settings", null).body);

            Started started = Start(false);
            string ticket = SignIn(started, SignedResponse(new Spec(started, account.Address)));
            Assert.AreEqual(201, Send("POST", "/api/v1/session", null, "{\"sso_ticket\":\"" + ticket + "\"}", TicketCookie(ticket)).status);
         }

         // A round trip of the switches and the names, the button's words among them.
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings",
            "{\"saml_require_signed_assertion\":false,\"saml_auto_provision\":true,\"saml_sp_entity_id\":\"urn:example:sp\"," +
            "\"saml_email_attribute\":\"mail\",\"saml_administrators\":\"a@example.com\",\"saml_button_label\":\"Contoso single sign-on\"}").status);
         settings = Admin("GET", "/api/v1/settings", null);
         StringAssert.Contains("\"saml_require_signed_assertion\":false", settings.body);
         StringAssert.Contains("\"saml_auto_provision\":true", settings.body);
         StringAssert.Contains("\"saml_sp_entity_id\":\"urn:example:sp\"", settings.body);
         StringAssert.Contains("\"saml_email_attribute\":\"mail\"", settings.body);
         StringAssert.Contains("\"saml_administrators\":\"a@example.com\"", settings.body);
         StringAssert.Contains("\"saml_button_label\":\"Contoso single sign-on\"", settings.body);
         StringAssert.Contains("\"saml_label\":\"Contoso single sign-on\"", Send("GET", "/api/v1/signin-methods", null, null).body,
            "The button's words are the setting's.");

         // The metadata carries the entity ID named and the ACS, and no key.
         (int status, string body, string headers) metadata = SendRaw("GET", "/saml/metadata", null, null, null, null);
         Assert.AreEqual(200, metadata.status, metadata.body);
         StringAssert.Contains("application/samlmetadata+xml", metadata.headers);
         var document = new XmlDocument();
         document.LoadXml(metadata.body);
         Assert.AreEqual("urn:example:sp", document.DocumentElement.GetAttribute("entityID"));
         XmlElement acs = (XmlElement) document.GetElementsByTagName("AssertionConsumerService", "urn:oasis:names:tc:SAML:2.0:metadata")[0];
         Assert.AreEqual(AcsUrl, acs.GetAttribute("Location"));
         Assert.AreEqual("urn:oasis:names:tc:SAML:2.0:bindings:HTTP-POST", acs.GetAttribute("Binding"));
         XmlElement descriptor = (XmlElement) document.GetElementsByTagName("SPSSODescriptor", "urn:oasis:names:tc:SAML:2.0:metadata")[0];
         Assert.AreEqual("false", descriptor.GetAttribute("AuthnRequestsSigned"));
         Assert.AreEqual("false", descriptor.GetAttribute("WantAssertionsSigned"), "The switch is off, and the metadata says so.");
         Assert.IsNull(document.GetElementsByTagName("KeyDescriptor", "urn:oasis:names:tc:SAML:2.0:metadata")[0],
            "The metadata offers a key, and requests are not signed.");

         // Emptied, the entity ID is the base URL's again.
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"saml_sp_entity_id\":\"\"}").status);
         StringAssert.Contains("entityID=\"" + SpEntityId + "\"", Send("GET", "/saml/metadata", null, null).body);
      }

      // ------------------------------------------------------------------ the identity provider

      private struct Started
      {
         public string location;
         public string id;
         public string relay;
         public string cookie;      // "hmsaml=...", what the browser sends back
         public string setCookie;   // the Set-Cookie line as the start wrote it
         public bool deck;
         public XmlDocument request;
      }

      /// <summary>One Response, as the fixture's identity provider writes it.</summary>
      private class Spec
      {
         public string inResponseTo;
         public string confirmationInResponseTo;
         public string nameId;
         public string nameIdFormat = EmailFormat;
         public string emailAttribute;
         public string emailAttributeName = EmailClaim;
         public string secondEmailValue;
         public int padding;
         public string destination = AcsUrl;
         public string recipient = AcsUrl;
         public string audience = SpEntityId;
         public string[] extraRestriction;
         public string responseIssuer = IdpEntityId;
         public string assertionIssuer = IdpEntityId;
         public string statusCode = "urn:oasis:names:tc:SAML:2.0:status:Success";
         public string secondStatusCode;
         public string confirmationMethod = "urn:oasis:names:tc:SAML:2.0:cm:bearer";
         public string responseId = "_r" + Guid.NewGuid().ToString("N");
         public string assertionId = "_a" + Guid.NewGuid().ToString("N");
         public int lifetime = 300;
         public int? conditionsNotBefore;       // seconds from now; -300 when null
         public int? conditionsNotOnOrAfter;    // seconds from now; lifetime when null
         public bool noAuthnStatement;
         public bool signAssertion = true;
         public bool signResponse;
         public bool encrypted;
         public string prefixList;
         public string digestMethod = "http://www.w3.org/2001/04/xmlenc#sha256";
         public RSA assertionKey;
         public RSA responseKey;
         public X509Certificate2 keyInfoCertificate;

         public Spec(Started started, string nameId)
         {
            inResponseTo = started.id;
            confirmationInResponseTo = started.id;
            this.nameId = nameId;
         }
      }

      private static string Instant(DateTime utc)
      {
         return utc.ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ss.fffZ", System.Globalization.CultureInfo.InvariantCulture);
      }

      private static string AssertionXml(Spec spec)
      {
         DateTime now = DateTime.UtcNow;

         string attributes = "";
         if (spec.emailAttribute != null)
         {
            attributes += "<saml:Attribute Name=\"" + spec.emailAttributeName + "\">" +
                          "<saml:AttributeValue xsi:type=\"xs:string\">" + spec.emailAttribute + "</saml:AttributeValue>" +
                          (spec.secondEmailValue != null ? "<saml:AttributeValue xsi:type=\"xs:string\">" + spec.secondEmailValue + "</saml:AttributeValue>" : "") +
                          "</saml:Attribute>";
         }
         if (spec.padding > 0)
         {
            attributes += "<saml:Attribute Name=\"urn:example:padding\"><saml:AttributeValue xsi:type=\"xs:string\">" +
                          new string('p', spec.padding) + "</saml:AttributeValue></saml:Attribute>";
         }
         if (attributes.Length > 0)
            attributes = "<saml:AttributeStatement>" + attributes + "</saml:AttributeStatement>";

         string restrictions = "<saml:AudienceRestriction><saml:Audience>" + spec.audience + "</saml:Audience></saml:AudienceRestriction>";
         if (spec.extraRestriction != null)
         {
            restrictions += "<saml:AudienceRestriction>";
            foreach (string audience in spec.extraRestriction)
               restrictions += "<saml:Audience>" + audience + "</saml:Audience>";
            restrictions += "</saml:AudienceRestriction>";
         }

         DateTime notBefore = now.AddSeconds(spec.conditionsNotBefore ?? -300);
         DateTime notOnOrAfter = now.AddSeconds(spec.conditionsNotOnOrAfter ?? spec.lifetime);

         return
            "<saml:Assertion xmlns:saml=\"" + AssertionNs + "\" xmlns:xs=\"http://www.w3.org/2001/XMLSchema\" " +
            "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" ID=\"" + spec.assertionId + "\" Version=\"2.0\" IssueInstant=\"" + Instant(now) + "\">" +
            "<saml:Issuer>" + spec.assertionIssuer + "</saml:Issuer>" +
            "<saml:Subject><saml:NameID Format=\"" + spec.nameIdFormat + "\">" + spec.nameId + "</saml:NameID>" +
            "<saml:SubjectConfirmation Method=\"" + spec.confirmationMethod + "\">" +
            "<saml:SubjectConfirmationData" +
            (spec.confirmationInResponseTo.Length > 0 ? " InResponseTo=\"" + spec.confirmationInResponseTo + "\"" : "") +
            " NotOnOrAfter=\"" + Instant(now.AddSeconds(spec.lifetime)) + "\" Recipient=\"" + spec.recipient + "\"/>" +
            "</saml:SubjectConfirmation></saml:Subject>" +
            "<saml:Conditions NotBefore=\"" + Instant(notBefore) + "\" NotOnOrAfter=\"" + Instant(notOnOrAfter) + "\">" +
            restrictions + "</saml:Conditions>" +
            (spec.noAuthnStatement ? "" :
               "<saml:AuthnStatement AuthnInstant=\"" + Instant(now) + "\" SessionIndex=\"" + spec.assertionId + "\">" +
               "<saml:AuthnContext><saml:AuthnContextClassRef>urn:oasis:names:tc:SAML:2.0:ac:classes:PasswordProtectedTransport</saml:AuthnContextClassRef>" +
               "</saml:AuthnContext></saml:AuthnStatement>") +
            attributes +
            "</saml:Assertion>";
      }

      /// <summary>The Response unsigned, the assertion inside it as the spec says.</summary>
      private static string ResponseXml(Spec spec)
      {
         string inside = spec.encrypted
            ? "<saml:EncryptedAssertion><xenc:EncryptedData xmlns:xenc=\"http://www.w3.org/2001/04/xmlenc#\" " +
              "Type=\"http://www.w3.org/2001/04/xmlenc#Element\"><xenc:CipherData><xenc:CipherValue>AAAA</xenc:CipherValue>" +
              "</xenc:CipherData></xenc:EncryptedData></saml:EncryptedAssertion>"
            : AssertionXml(spec);

         string status = spec.secondStatusCode == null
            ? "<samlp:StatusCode Value=\"" + spec.statusCode + "\"/>"
            : "<samlp:StatusCode Value=\"" + spec.statusCode + "\"><samlp:StatusCode Value=\"" + spec.secondStatusCode + "\"/></samlp:StatusCode>";

         return
            "<samlp:Response xmlns:samlp=\"" + Protocol + "\" xmlns:saml=\"" + AssertionNs + "\" ID=\"" + spec.responseId + "\" " +
            "Version=\"2.0\" IssueInstant=\"" + Instant(DateTime.UtcNow) + "\" Destination=\"" + spec.destination + "\"" +
            (spec.inResponseTo.Length > 0 ? " InResponseTo=\"" + spec.inResponseTo + "\"" : "") + ">" +
            "<saml:Issuer>" + spec.responseIssuer + "</saml:Issuer>" +
            "<samlp:Status>" + status + "</samlp:Status>" +
            inside +
            "</samlp:Response>";
      }

      /// <summary>
      ///    The Response signed as the spec says: the Assertion first (a signature inside it,
      ///    after its Issuer, where SAML's schema puts it), then the Response over it.
      /// </summary>
      private string SignedResponse(Spec spec)
      {
         var document = new XmlDocument { PreserveWhitespace = true };
         document.LoadXml(ResponseXml(spec));

         if (spec.signAssertion)
            SignElement(document, spec.assertionId, spec.assertionKey ?? _key, spec.keyInfoCertificate ?? _certificate, spec.digestMethod, spec.prefixList);
         if (spec.signResponse)
            SignElement(document, spec.responseId, spec.responseKey ?? _key, spec.keyInfoCertificate ?? _certificate, spec.digestMethod, spec.prefixList);

         return document.OuterXml;
      }

      private static void SignElement(XmlDocument document, string id, RSA key, X509Certificate2 keyInfoCertificate, string digestMethod, string prefixList)
      {
         var signer = new SignedXml(document) { SigningKey = key };
         signer.SignedInfo.CanonicalizationMethod = SignedXml.XmlDsigExcC14NTransformUrl;
         signer.SignedInfo.SignatureMethod = "http://www.w3.org/2001/04/xmldsig-more#rsa-sha256";

         var reference = new Reference("#" + id) { DigestMethod = digestMethod };
         reference.AddTransform(new XmlDsigEnvelopedSignatureTransform());
         reference.AddTransform(prefixList == null ? new XmlDsigExcC14NTransform() : new XmlDsigExcC14NTransform(prefixList));
         signer.AddReference(reference);

         // What an identity provider sends, and what the server ignores: the key is the
         // certificate an administrator stored.
         signer.KeyInfo = new KeyInfo();
         signer.KeyInfo.AddClause(new KeyInfoX509Data(keyInfoCertificate));

         signer.ComputeSignature();

         XmlElement target = (XmlElement) document.SelectSingleNode("//*[@ID='" + id + "']");
         XmlNode issuer = target.FirstChild;
         target.InsertAfter(document.ImportNode(signer.GetXml(), true), issuer);
      }

      private static XmlNamespaceManager Names(XmlDocument document)
      {
         var names = new XmlNamespaceManager(document.NameTable);
         names.AddNamespace("samlp", Protocol);
         names.AddNamespace("saml", AssertionNs);
         names.AddNamespace("ds", DsNs);
         return names;
      }

      /// <summary>
      ///    The wrapping attack on a signed Response: the signed Assertion moved into an
      ///    Extensions element and a forgery naming the victim put where the Assertion was -
      ///    unsigned with the same ID, or with another ID carrying the original's signature.
      /// </summary>
      private static string Wrap(string signedResponse, string victim, bool otherId)
      {
         var document = new XmlDocument { PreserveWhitespace = true };
         document.LoadXml(signedResponse);
         XmlNamespaceManager names = Names(document);

         XmlElement response = document.DocumentElement;
         XmlElement original = (XmlElement) response.SelectSingleNode("saml:Assertion", names);
         XmlElement forged = (XmlElement) original.CloneNode(true);

         forged.SelectSingleNode("saml:Subject/saml:NameID", names).InnerText = victim;
         if (otherId)
            forged.SetAttribute("ID", "_forged" + Guid.NewGuid().ToString("N"));
         else
            forged.RemoveChild(forged.SelectSingleNode("ds:Signature", names));

         XmlElement extensions = document.CreateElement("samlp", "Extensions", Protocol);
         response.InsertAfter(extensions, response.SelectSingleNode("saml:Issuer", names));
         extensions.AppendChild(original);
         response.AppendChild(forged);
         return document.OuterXml;
      }

      /// <summary>The signed Response with its only Assertion moved under an Extensions element.</summary>
      private static string MoveAssertion(string signedResponse)
      {
         var document = new XmlDocument { PreserveWhitespace = true };
         document.LoadXml(signedResponse);
         XmlNamespaceManager names = Names(document);

         XmlElement response = document.DocumentElement;
         XmlElement extensions = document.CreateElement("samlp", "Extensions", Protocol);
         response.InsertAfter(extensions, response.SelectSingleNode("saml:Issuer", names));
         extensions.AppendChild(response.SelectSingleNode("saml:Assertion", names));
         return document.OuterXml;
      }

      /// <summary>The signed Response with a copy of the Assertion's signature under an Extensions element.</summary>
      private static string StraySignature(string signedResponse)
      {
         var document = new XmlDocument { PreserveWhitespace = true };
         document.LoadXml(signedResponse);
         XmlNamespaceManager names = Names(document);

         XmlElement response = document.DocumentElement;
         XmlElement extensions = document.CreateElement("samlp", "Extensions", Protocol);
         response.InsertAfter(extensions, response.SelectSingleNode("saml:Issuer", names));
         extensions.AppendChild(response.SelectSingleNode("saml:Assertion/ds:Signature", names).CloneNode(true));
         return document.OuterXml;
      }

      private static RSACryptoServiceProvider NewKey(int bits)
      {
         return new RSACryptoServiceProvider(bits, new CspParameters(24)) { PersistKeyInCsp = false };
      }

      private static X509Certificate2 SelfSigned(RSA key, string subject)
      {
         var request = new CertificateRequest(subject, key, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
         return request.CreateSelfSigned(DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddYears(2));
      }

      private static string Pem(X509Certificate2 certificate)
      {
         string base64 = Convert.ToBase64String(certificate.Export(X509ContentType.Cert));
         var pem = new StringBuilder("-----BEGIN CERTIFICATE-----\n");
         for (int i = 0; i < base64.Length; i += 64)
            pem.Append(base64.Substring(i, Math.Min(64, base64.Length - i))).Append('\n');
         pem.Append("-----END CERTIFICATE-----\n");
         return pem.ToString();
      }

      private static string Sha256Fingerprint(X509Certificate2 certificate)
      {
         using (SHA256 sha = SHA256.Create())
            return BitConverter.ToString(sha.ComputeHash(certificate.RawData)).Replace("-", ":");
      }

      // ------------------------------------------------------------------ the browser

      private void Configure()
      {
         (int status, string body) applied = Admin("PUT", "/api/v1/settings",
            "{\"saml_idp_entity_id\":\"" + IdpEntityId + "\",\"saml_idp_sso_url\":\"" + IdpSsoUrl + "\"," +
            "\"saml_idp_certificate\":\"" + Json(Pem(_certificate)) + "\"," +
            "\"oidc_redirect_base_url\":\"" + BaseUrl + "\"}");
         Assert.AreEqual(200, applied.status, applied.body);
      }

      /// <summary>
      ///    GET /portal/saml/start as a browser of its own, and what it got back: the
      ///    AuthnRequest the redirect carries, inflated, the RelayState and the cookie.
      /// </summary>
      private static Started Start(bool deck, string extraHeader = null)
      {
         (int status, string body, string headers) answer =
            SendRaw("GET", "/portal/saml/start" + (deck ? "?deck=1" : ""), null, null, null, extraHeader);
         Assert.AreEqual(302, answer.status, answer.headers);

         Match location = Regex.Match(answer.headers, @"Location: (\S+)");
         Assert.IsTrue(location.Success, answer.headers);
         StringAssert.StartsWith(IdpSsoUrl + "?SAMLRequest=", location.Groups[1].Value, "The start did not go to the identity provider.");

         Match setCookie = Regex.Match(answer.headers, @"Set-Cookie: (hmsaml=[^;\r\n]+)[^\r\n]*");
         Assert.IsTrue(setCookie.Success, "The start set no hmsaml cookie: " + answer.headers);

         string encoded = Uri.UnescapeDataString(Regex.Match(location.Groups[1].Value, @"[?&]SAMLRequest=([^&]+)").Groups[1].Value);
         string relay = Uri.UnescapeDataString(Regex.Match(location.Groups[1].Value, @"[?&]RelayState=([^&]+)").Groups[1].Value);

         // The HTTP-Redirect binding: raw DEFLATE, which DeflateStream reads.
         string xml;
         using (var deflated = new MemoryStream(Convert.FromBase64String(encoded)))
         using (var inflater = new DeflateStream(deflated, CompressionMode.Decompress))
         using (var reader = new StreamReader(inflater, Encoding.UTF8))
            xml = reader.ReadToEnd();

         var request = new XmlDocument();
         request.LoadXml(xml);

         return new Started
         {
            location = location.Groups[1].Value,
            id = request.DocumentElement.GetAttribute("ID"),
            relay = relay,
            cookie = setCookie.Groups[1].Value,
            setCookie = setCookie.Groups[0].Value,
            deck = deck,
            request = request
         };
      }

      /// <summary>
      ///    The form the identity provider's page posts: the Response base64'd and then
      ///    form-encoded. Uri.EscapeDataString refuses a string this long on .NET Framework,
      ///    and base64 needs only its three symbols escaped.
      /// </summary>
      private static string FormBody(string responseXml, string relay)
      {
         string base64 = Convert.ToBase64String(Encoding.UTF8.GetBytes(responseXml));
         var escaped = new StringBuilder(base64.Length + base64.Length / 16);
         foreach (char c in base64)
         {
            if (c == '+')
               escaped.Append("%2B");
            else if (c == '/')
               escaped.Append("%2F");
            else if (c == '=')
               escaped.Append("%3D");
            else
               escaped.Append(c);
         }
         return "SAMLResponse=" + escaped + "&RelayState=" + Uri.EscapeDataString(relay);
      }

      /// <summary>The browser posting the Response to the ACS, and where it is sent.</summary>
      private static (int status, string location) Acs(string responseXml, string relay)
      {
         (int status, string body, string headers) answer =
            SendRaw("POST", "/portal/saml/acs", null, FormBody(responseXml, relay), "application/x-www-form-urlencoded", null, false);
         Match location = Regex.Match(answer.headers, @"Location: (\S+)");
         Assert.IsTrue(location.Success, answer.status + " " + answer.headers + " " + answer.body);
         return (answer.status, location.Groups[1].Value);
      }

      /// <summary>A browser following the ACS on to the finish with the cookie it holds (or none).</summary>
      private static (int status, string location) Finish(string finishLocation, string cookie)
      {
         (int status, string body, string headers) answer =
            SendRaw("GET", finishLocation, null, null, null, cookie == null ? null : "Cookie: " + cookie);
         Match location = Regex.Match(answer.headers, @"Location: (\S+)");
         Assert.IsTrue(location.Success, answer.status + " " + answer.headers + " " + answer.body);

         // A ticket comes with the cookie that binds it to this browser, and the
         // session is asked for with both, as the browser would.
         Match ticket = Regex.Match(location.Groups[1].Value, @"#sso=([A-Za-z0-9_-]+)$");
         if (ticket.Success)
         {
            Match bound = Regex.Match(answer.headers, @"Set-Cookie: (hmsso=[^;]+);([^\r\n]*)");
            Assert.IsTrue(bound.Success, "The ticket came without the cookie that binds it to this browser: " + answer.headers);
            StringAssert.Contains("HttpOnly", bound.Groups[2].Value);
            StringAssert.Contains("SameSite=Strict", bound.Groups[2].Value);
            StringAssert.Contains("Path=/api/v1/session", bound.Groups[2].Value);
            TicketCookies[ticket.Groups[1].Value] = bound.Groups[1].Value;
         }

         return (answer.status, location.Groups[1].Value);
      }

      private static readonly System.Collections.Generic.Dictionary<string, string> TicketCookies =
         new System.Collections.Generic.Dictionary<string, string>();

      /// <summary>The Cookie header the browser a ticket was handed to would send with it.</summary>
      private static string TicketCookie(string ticket)
      {
         Assert.IsTrue(TicketCookies.ContainsKey(ticket), "No finish handed out that ticket.");
         return "Cookie: " + TicketCookies[ticket];
      }

      /// <summary>Where the browser that started this sign-in ends: the ACS, and the finish when the ACS sends it there.</summary>
      private static string Outcome(Started started, string responseXml)
      {
         (int status, string location) acs = Acs(responseXml, started.relay);
         if (!acs.location.StartsWith("/portal/saml/finish?", StringComparison.Ordinal))
            return acs.location;
         return Finish(acs.location, started.cookie).location;
      }

      /// <summary>A sign-in expected to succeed; answers the one-use ticket.</summary>
      private static string SignIn(Started started, string responseXml)
      {
         string page = started.deck ? "/" : "/portal";
         string landed = Outcome(started, responseXml);
         StringAssert.StartsWith(page + "#sso=", landed, "The sign-in did not come back with a ticket: " + Why(landed));
         return landed.Substring((page + "#sso=").Length);
      }

      /// <summary>A fresh sign-in whose Response the builder writes, refused with the sentence, on its own page.</summary>
      private void Refused(string what, Func<Started, string> build, string sentence, bool deck = false)
      {
         Started started = Start(deck);
         string landed = Outcome(started, build(started));
         StringAssert.StartsWith((deck ? "/" : "/portal") + "#sso_error=", landed, what + " was taken.");
         StringAssert.Contains(sentence, Why(landed), what + " was refused for another reason.");
      }

      private static string Why(string location)
      {
         int at = location.IndexOf("#sso_error=", StringComparison.Ordinal);
         return at < 0 ? "" : Uri.UnescapeDataString(location.Substring(at + "#sso_error=".Length));
      }

      private static void AssertRefused(string body, string sentence)
      {
         (int status, string body) answer = Admin("PUT", "/api/v1/settings", body);
         Assert.AreEqual(400, answer.status, answer.body);
         StringAssert.Contains(sentence, answer.body);
      }

      private static string Json(string text)
      {
         return text.Replace("\\", "\\\\").Replace("\"", "\\\"").Replace("\r", "\\r").Replace("\n", "\\n");
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
                                                    string extraHeader = null)
      {
         (int status, string body, string headers) answer =
            SendRaw(method, path, authorization, requestBody, requestBody == null ? null : "application/json", extraHeader);
         return (answer.status, answer.body);
      }

      private static TcpClient Connect()
      {
         var client = new TcpClient();
         Exception last = null;
         for (int attempt = 0; attempt < 25; attempt++)
         {
            try
            {
               client.Connect(Host, RestPort);
               return client;
            }
            catch (SocketException ex)
            {
               last = ex;
               Thread.Sleep(200);
            }
         }

         client.Dispose();
         throw last;
      }

      /// <summary>
      ///    A form body larger than the ACS's ceiling, answered 413 - or, since the listener
      ///    closes under a body it will not read, reset while the body is written or the
      ///    answer read. A connection that could not be made at all is not a reset: Connect
      ///    throws, and the test fails.
      /// </summary>
      private static int PostOversize(int bytes)
      {
         string body = "SAMLResponse=" + new string('A', bytes) + "&RelayState=x";
         using (TcpClient client = Connect())
         {
            try
            {
               using (NetworkStream stream = client.GetStream())
               using (var memory = new MemoryStream())
               {
                  byte[] head = Encoding.ASCII.GetBytes("POST /portal/saml/acs HTTP/1.0\r\nHost: " + Host +
                     "\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: " + body.Length + "\r\nConnection: close\r\n\r\n");
                  byte[] payload = Encoding.ASCII.GetBytes(body);
                  stream.Write(head, 0, head.Length);
                  stream.Write(payload, 0, payload.Length);

                  byte[] buffer = new byte[4096];
                  int read;
                  while ((read = stream.Read(buffer, 0, buffer.Length)) > 0)
                     memory.Write(buffer, 0, read);

                  string raw = Encoding.ASCII.GetString(memory.ToArray());
                  string[] parts = raw.Split(new[] { "\r\n" }, StringSplitOptions.None)[0].Split(' ');
                  int status;
                  return parts.Length >= 2 && int.TryParse(parts[1], out status) ? status : 0;
               }
            }
            catch (IOException)
            {
               return 413;
            }
            catch (SocketException)
            {
               return 413;
            }
         }
      }

      /// <summary>
      ///    One HTTP/1.0 request with the response headers as well as its body. Every write
      ///    carries X-Requested-With, as this server's own pages do, except the ACS's form:
      ///    that is posted by the identity provider's page, which sets no header of ours.
      /// </summary>
      private static (int status, string body, string headers) SendRaw(string method, string path, string authorization,
                                                                       string requestBody, string contentType, string extraHeader,
                                                                       bool requestedWith = true)
      {
         using (TcpClient client = Connect())
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
               headers.Append("Content-Type: " + (contentType ?? "application/json") + "\r\n");
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
