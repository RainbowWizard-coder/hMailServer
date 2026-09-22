// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Public web services for mail domains:
//
//   - MTA-STS policy hosting (RFC 8461): serves
//     https://mta-sts.<domain>/.well-known/mta-sts.txt for every local
//     domain, with the mx list derived from the domain's live MX records
//     (overridable with MtaStsPolicyMx).
//   - Client autoconfiguration: Thunderbird-style autoconfig
//     (/mail/config-v1.1.xml), Outlook autodiscover
//     (/autodiscover/autodiscover.xml) and an Apple configuration profile
//     (/email.mobileconfig), all generated from the actual TCP/IP port
//     configuration by GetClientAccessSettings_ - one source of truth for
//     three wire formats. The autodiscover answer also names, as Exchange's
//     <AlternativeMailbox> elements, the mailboxes other accounts have
//     opened to the account asked about - but only to a request that
//     authenticated over HTTPS as that account, since who delegates to whom
//     is not public; see HandleAutodiscover_.
//   - CardDAV (RFC 6352): the account's address book under /dav/, served
//     by CardDavServer over HTTPS and authenticated as the account. See
//     CardDavServer.h for the tree a client walks.
//   - CalDAV / CardDAV service discovery (RFC 6764): /.well-known/carddav
//     redirects to CardDavRedirectUrl when one is set, and to the built-in
//     /dav/ otherwise; /.well-known/caldav redirects to CalDavRedirectUrl.
//     This server implements no CalDAV, so with no target configured that
//     path answers 404 - never a redirect to somewhere that does not speak
//     the protocol.
//   - ACME http-01 challenges (/.well-known/acme-challenge/<token>),
//     served from AcmeChallengeStore so certificate issuance works
//     while this server owns port 80.
//   - security.txt (RFC 9116): serves /.well-known/security.txt for a
//     local domain that has a postmaster address configured. Generated,
//     so the mandatory Expires field is always in the future.
//   - The OpenPGP Web Key Directory (draft-koch-openpgp-webkey-service):
//     /.well-known/openpgpkey/<domain>/hu/<hashed local part> and its
//     policy file (the advanced method, served under openpgpkey.<domain>),
//     and /.well-known/openpgpkey/hu/<hashed local part> and its policy
//     file (the direct method, on the domain itself). Off at the server
//     until OpenPGPWebKeyDirectoryEnabled is set, then off for every
//     domain and every account until each says otherwise; see
//     docs/OpenPGP.md and OpenPgpKeyStore.h. Over HTTPS only, and every
//     refusal is the same 404, so the directory cannot be used to find
//     out who has an account here.
//
// Disabled by default. Enable with WebServicesHttpPort / WebServicesHttpsPort
// in hMailServer.ini. The HTTPS listener uses WebServicesCertificateFile /
// WebServicesPrivateKeyFile, falling back to the ACME certificate.
//
// Several of the features above are enabled by default while the listener
// is not, so ReportUnreachableFeatures() exists to say so at startup. It is
// a static settings-and-logging function on purpose: Application::StartServers
// calls it unconditionally, before and independently of any instance of this
// class existing.

#pragma once

#include <memory>

#include "HttpServer.h"

namespace HM
{
   class WebServicesServer
   {
   public:
      WebServicesServer();
      ~WebServicesServer();

      bool Start(const String &bind_address, int http_port, int https_port,
                 const String &certificate_file, const String &private_key_file);
      void Stop();

      // True if a running instance serves plain HTTP on the given port.
      // Used by the ACME client to decide whether a transient challenge
      // listener is needed.
      static bool IsListeningOnPort(int port);

      // The HTTPS port a running instance actually serves, 0 when there is
      // none. Written when the HTTPS worker starts and cleared by Stop(). It
      // exists so the REST API's /api/v1/srv endpoint can advertise an
      // _autodiscover._tcp record only when something real answers it: the
      // configured WebServicesHttpsPort is not enough, because the HTTPS
      // listener silently stays down when no certificate is available yet.
      static int GetHttpsListenPort();

      // Reports, as LOG_APPLICATION lines, every feature that is switched on
      // but cannot be reached with the web-services configuration as it
      // stands. Two cases: no listener configured at all, and a listener that
      // exists but cannot serve a particular feature (MTA-STS needs HTTPS
      // specifically).
      //
      // MUST be called unconditionally from Application::StartServers -
      // including when both WebServicesHttpPort and WebServicesHttpsPort are 0,
      // which is the shipped default and the single case that most needs
      // reporting. It reads hMailServer.ini and logs, and nothing else: no
      // member state, no socket, no listener, no instance of this class. So it
      // is safe at any point after IniFileSettings exists, whether or not
      // Start() is ever called, and it cannot throw out.
      //
      // Start() calls it as well, with the ports it was actually given, which
      // need not be the ones in the settings. Reporting is keyed on the
      // configuration reported, so the two callers produce one set of lines
      // between them for one configuration, while a later stop/start with a
      // port changed does report the new state instead of staying silent
      // forever after the first call.
      static void ReportUnreachableFeatures();

   private:

      struct ProtocolEndpoint
      {
         int port = 0;
         AnsiString socket_type; // "SSL", "STARTTLS" or "plain"
      };

      // The report above, for the ports a listener is actually being started
      // with rather than the ones in hMailServer.ini. Static, and reachable
      // with no listener and no ports at all, which is how the startup call
      // reaches it.
      static void ReportUnreachableFeatures_(int http_port, int https_port);

      static HttpResponse ProcessRequest_(const HttpRequest &request);

      static HttpResponse BuildResponse_(int status_code, const AnsiString &content_type, const AnsiString &body,
                                       const AnsiString &extra_headers = "");
      static HttpResponse BuildRedirectResponse_(const AnsiString &location);

      // True when the request reached this listener over TLS, or a proxy that
      // terminated TLS on the client's behalf says so in X-Forwarded-Proto.
      static bool RequestArrivedOverHttps_(const AnsiString &request, bool over_tls, const IPAddress &peer);
      // The answer to a configuration profile asked for over plain HTTP: the same
      // URL on the HTTPS listener when one is configured, otherwise 403 with the
      // reason.
      static HttpResponse RefusePlainHttpProfile_(const AnsiString &host, const AnsiString &path, const AnsiString &query);
      static AnsiString GetRequestHost_(const AnsiString &request);
      static AnsiString GetRequestBody_(const AnsiString &request);

      static HttpResponse HandleAcmeChallenge_(const AnsiString &path);
      static HttpResponse HandleMtaStsPolicy_(const AnsiString &host);
      static HttpResponse HandleAutoconfig_(const AnsiString &host, const AnsiString &query);
      // Outlook's POX Autodiscover. The settings are answered to everybody;
      // the <AlternativeMailbox> elements (the mailboxes other accounts have
      // opened to the one asked about) only to a request that authenticated,
      // over HTTPS, as that address. over_https is the answer
      // RequestArrivedOverHttps_ gave for this request.
      static HttpResponse HandleAutodiscover_(const HttpRequest &request, bool over_https);
      static HttpResponse HandleSecurityTxt_(const AnsiString &host);

      // The OpenPGP Web Key Directory. path is what followed
      // /.well-known/openpgpkey/ - either "policy", "hu/<hash>", or a
      // domain followed by one of those. over_https is the answer
      // RequestArrivedOverHttps_ gave for this request.
      static HttpResponse HandleWebKeyDirectory_(const AnsiString &rest, const AnsiString &host, bool over_https);

      // The one answer every refusal gives: 404, the same body, the same
      // headers. A different answer for "no such account" and for "that
      // account publishes no key" would make the directory a way of asking
      // whether an address exists here, which is what a spammer wants from it.
      static HttpResponse WebKeyDirectoryNotFound_();

      // Apple .mobileconfig configuration profile, built from the same
      // GetClientAccessSettings_ data the two XML handlers use.
      static HttpResponse HandleAppleProfile_(const AnsiString &host, const AnsiString &query);

      // RFC 6764 well-known redirect. calendar selects caldav over carddav.
      // built_in_target is where the path points when no other server is
      // configured: this server's own /dav/, which serves both.
      static HttpResponse HandleWellKnownDavRedirect_(bool calendar, const AnsiString &built_in_target);

      // The absolute URL of this server's DAV context path (CardDAV and
      // CalDAV), on the host the client used and over HTTPS: the HTTPS
      // listener's port when the request came over plain HTTP and one is
      // configured. Relative when the request named no host.
      static AnsiString BuiltInDavUrl_(const AnsiString &request, bool over_tls, const IPAddress &peer);

      // The Host header as sent, port included, lower-cased; empty when
      // absent or not printable US-ASCII without spaces. GetRequestHost_
      // strips the port, which a Location header must keep.
      static AnsiString GetRequestHostHeader_(const AnsiString &request);

      // The configured redirect target, if there is a usable one.
      // GetDavRedirectSetting_ answers only "is anything configured", without
      // validating or reporting, so the startup feature report cannot raise an
      // error as a side effect of listing a feature. GetDavRedirectTarget_
      // validates, and is what the request path uses.
      static bool GetDavRedirectSetting_(bool calendar, AnsiString &value);
      static bool GetDavRedirectTarget_(bool calendar, AnsiString &target, bool &configured_but_unusable);

      // Derives the mail domain a client-configuration request is about, from
      // the autoconfig.<domain> host name or the emailaddress parameter, with
      // the client host as the fallback. Shared by all three client
      // configuration formats so they cannot disagree.
      static AnsiString GetQueryEmailAddress_(const AnsiString &query);
      static AnsiString GetRequestedMailDomain_(const AnsiString &host, const AnsiString &query,
                                                const AnsiString &client_host);

      // Resolves the RFC 9116 Contact address for the requested host from
      // the postmaster address of the matching local domain. False when
      // there is nothing publishable, in which case no file is served.
      static bool GetSecurityContact_(const AnsiString &host, AnsiString &contact);

      static bool GetClientAccessSettings_(AnsiString &client_host,
         ProtocolEndpoint &imap, ProtocolEndpoint &pop3, ProtocolEndpoint &smtp);
      static bool GetPolicyMxHosts_(const String &domain, std::vector<AnsiString> &mx_hosts);
      static AnsiString XmlEscape_(const AnsiString &value);

      // A stable UUID derived from a name, so a profile reinstalled on a
      // device replaces the previous one instead of stacking a duplicate
      // account beside it. Deriving it means no state has to be persisted.
      static AnsiString MakeStableUuid_(const AnsiString &seed);

      std::shared_ptr<HttpServer> server_;
      bool running_;
      bool tls_available_;
   };
}
