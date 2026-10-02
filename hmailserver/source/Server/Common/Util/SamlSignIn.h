// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// SAML sign-in, beside OIDC: the identity providers that still speak only SAML
// 2.0 - an ADFS farm, an Okta or Entra ID application set up the SAML way - as
// the front door to the webmail and the Control Deck.
//
// This is a cryptographic-parser row first and a sign-in row second. SAML's
// answer is an XML document whose truth rests on an XML signature, and an XML
// signature cannot be verified without canonicalising the XML it covers: so
// the reader, the canonicaliser (XmlDocument.h) and the verifier
// (XmlSignature.h) are this server's own, written for this and held to the
// W3C's examples and to .NET's SignedXml, and the sign-in below is the
// smaller part.
//
// WHAT IS BUILT: A SERVICE PROVIDER, THE WEB BROWSER SSO PROFILE
//
//   GET  /portal/saml/start[?deck=1]   an AuthnRequest, deflated (raw DEFLATE,
//                                      RFC 1951) and base64'd into the
//                                      redirect to the identity provider's
//                                      sign-in URL (the HTTP-Redirect
//                                      binding), unsigned.
//   POST /portal/saml/acs              the Response, posted back by the
//                                      browser (the HTTP-POST binding).
//   GET  /portal/saml/finish           the same browser, sent on by the ACS:
//                                      where the sign-in is completed.
//   GET  /saml/metadata                this service provider's metadata, for
//                                      the identity provider to import.
//
// TWO BINDINGS, SERVER-SIDE AND IN THE BROWSER
//
// Every AuthnRequest's ID is kept here, with the RelayState sent beside it and
// a cookie value, for ten minutes and one use. A Response is taken at the ACS
// only when its InResponseTo is one of those IDs, not yet used, and it comes
// back with the same RelayState - so an answer the identity provider sent
// unasked (IdP-initiated sign-in) is refused, and so is one replayed, and one
// meant for another sign-in.
//
// That alone does not close login forgery: somebody who starts a sign-in
// themselves, and keeps the Response their identity provider sends back, can
// have another person's browser post it within the ten minutes - which would
// sign that browser in to the first person's mailbox. So the ACS does not
// finish the sign-in. It keeps what it verified under a one-use token for two
// minutes and sends the browser (303) to /portal/saml/finish, on this server,
// and the finish completes the sign-in only when the browser carries the
// hmsaml cookie the start gave it for that very request. The cookie is
// SameSite=Lax: it does not ride the identity provider's cross-site POST,
// which is why it is not asked for there, and it does ride the top-level GET
// the ACS sends the browser on to, which is a navigation to this server. A
// Response forwarded to another person's browser therefore fails at the finish:
// that browser carries its own cookie or none. The cookie is Secure when the
// start came over TLS, or through the loopback proxy of a plain-HTTP listener
// that says so in X-Forwarded-Proto.
//
// WHAT IS VERIFIED
//
//   * The signature (XmlSignature::Verify), against the certificate or two an
//     administrator stored and never a key the message carries. The Assertion
//     must carry its own signature unless SamlRequireSignedAssertion is off,
//     when a signature over the whole Response is enough. Whichever signed it,
//     the Assertion that is USED is the one re-read from the bytes a signature
//     covered, never one looked up again in the document.
//   * The document's shape: one Assertion, a child of the Response, its ID
//     written once in the whole document; every ds:Signature the Response's or
//     the Assertion's own; an EncryptedAssertion refused with a sentence.
//   * Response: Destination is this server's ACS URL, InResponseTo an issued
//     and unused request ID, its Issuer (when it has one) the identity
//     provider's entity ID, and its status Success.
//   * Assertion (SAML 2.0 core, and the profile's section 4.1.4.3): Issuer is
//     the identity provider's entity ID; a bearer SubjectConfirmation whose
//     data's Recipient is the ACS URL, InResponseTo the same request and
//     NotOnOrAfter in the future; Conditions' NotBefore and NotOnOrAfter,
//     with a minute of skew; an AudienceRestriction naming this service
//     provider's entity ID (every one of them, when there are several); an
//     AuthnStatement; and an ID this server has not taken before - kept in
//     memory until the assertion expires or five thousand newer ones push it
//     out, and forgotten when the listener stops, when every request ID it
//     could answer is forgotten too.
//   * The address: the NameID when its Format is emailAddress, else the
//     attribute SamlEmailAttribute names (Entra's and ADFS's claim URI by
//     default), exactly one value of it. Then OIDC's mapping and provisioning
//     rules exactly (OidcSignIn::MapAddress), with SamlAdministrators and
//     SamlAutoProvision in place of the OIDC switches, and OIDC's ticket: the
//     session is still minted by POST /api/v1/session and nothing else.
//
// WHAT IS NOT BUILT
//
// Single logout; encrypted assertions; signed AuthnRequests (so the metadata
// offers no signing certificate); the artifact binding; attribute queries;
// fetching the identity provider's metadata (an administrator pastes its
// entity ID, sign-in URL and certificate); more than one identity provider.
#pragma once

#include <string>

#include "OidcSignIn.h"

namespace HM
{
   class SamlSignIn
   {
   public:
      // Whether an administrator has named the identity provider's entity ID,
      // its sign-in URL and a certificate, and there is a base URL for the
      // browser to come back to.
      static bool Configured();

      // What the sign-in pages show on the button: SamlButtonLabel, "Sign in
      // with SAML" unless an administrator names it otherwise - its own words,
      // so that the SAML button never reads as the OpenID Connect one does when
      // one identity provider serves both.
      static AnsiString Label();

      // oidc_redirect_base_url, else import_oauth_redirect_base_url, trimmed of
      // a trailing slash: this server's address from outside. Empty when
      // neither is set.
      static AnsiString BaseUrl();

      // <base URL>/portal/saml/acs, and this service provider's entity ID -
      // SamlSpEntityId, else <base URL>/saml. Empty with no base URL.
      static AnsiString AcsUrl();
      static AnsiString ServiceProviderEntityId();

      // Starts a sign-in: url is the identity provider's sign-in URL with the
      // AuthnRequest and the RelayState in its query, and cookie the value the
      // browser must carry back to the finish. deck and source as
      // OidcSignIn::Begin.
      static bool Begin(bool deck, const std::string &source, AnsiString &url, AnsiString &cookie, AnsiString &problem);

      // The ACS: the SAMLResponse and RelayState form fields as the browser
      // posted them, verified. On success what was verified is kept under
      // finishToken, for Finish. result.deck says which page the sign-in was
      // for whenever this server knows it - from the sign-in's own record,
      // never from the Response - success or not; problem is one sentence for
      // the page otherwise.
      static bool Complete(const AnsiString &samlResponse, const AnsiString &relayState,
                           OidcSignIn::Result &result, AnsiString &finishToken, AnsiString &problem);

      // The finish: the token the ACS gave and the hmsaml cookie the browser
      // carried. True with the verified sign-in when the cookie is the one the
      // start gave for that request; the token is spent either way.
      static bool Finish(const AnsiString &finishToken, const AnsiString &cookie,
                         OidcSignIn::Result &result, AnsiString &problem);

      // This service provider's metadata (SAML 2.0 metadata, an
      // EntityDescriptor with one SPSSODescriptor): the entity ID and the ACS
      // URL. Empty with no base URL.
      static AnsiString Metadata();

      // What a pasted certificate setting holds: empty when it is acceptable -
      // empty text, or one or two PEM certificates with an RSA key of 2048 bits
      // or more or an elliptic-curve key of 256 or more, and nothing else - and
      // otherwise the sentence. Private-key material and an over-long paste are
      // refused first (GatewayEncryption::AcceptableKeyText).
      static String CertificateProblem(const String &pem);

      // The certificates as they are stored: each written again as PEM, so
      // that whatever else a tool printed around them - a subject line, bag
      // attributes - is not kept, and a length is a length of certificates.
      // The text as it came when no certificate can be read from it.
      static String NormalisedCertificates(const String &pem);

      // What an hm_settings row holds, in characters: the normalised
      // certificates must fit. A certificate is about 1,200 to 1,500 characters
      // of PEM for a 2048-bit key and about 1,800 to 2,200 for a 4096-bit one,
      // by its subject and extensions, so two fit unless both are long; the
      // refusal names the stored length.
      static const int MaximumStoredCharacters = 4000;

      // The stored certificates as the settings answer them: for each, its
      // SHA-256 fingerprint, its SHA-1 thumbprint (what Entra ID and ADFS show)
      // and when it expires. Never the PEM.
      static String CertificateSummary(const String &pem);

      // The raw DEFLATE (RFC 1951, no zlib header) the redirect binding
      // carries. Public for the tests.
      static bool Deflate(const std::string &bytes, std::string &deflated);

      // An xs:dateTime as SAML writes it, to seconds since the epoch, UTC.
      // False when it is not one. Public for the tests.
      static bool ReadTime(const std::string &text, __int64 &seconds);

      static const int PendingSeconds = 600;
      static const int FinishSeconds = 120;
      static const int ClockSkewSeconds = 60;

      // Forgets the pending sign-ins, the verified ones waiting for their
      // finish and the assertions seen. For the listener's stop and the tests.
      static void Clear();
   };
}
