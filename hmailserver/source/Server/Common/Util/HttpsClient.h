// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <vector>

namespace HM
{
   // One HTTP request, synchronously, with the response read to the end.
   //
   // The third such client in this tree (AcmeClient::Transact_ and
   // OutboundOAuth2TokenClient::HttpsPost_ are the other two) and the one meant to be
   // shared from here on: the JWKS fetch and token introspection both need a verified
   // HTTPS GET or POST with a bounded response, and nothing about that is specific to
   // either. HTTP/1.0 with Connection: close, so the end of the response is the end of
   // the stream and no chunked or keep-alive handling is needed.
   //
   // https is verified against the machine's root store with the host name checked
   // (CertificateVerifier), exactly as the two older clients do. Plain http is accepted
   // only to a loopback address: a JWKS document or an introspection verdict fetched in
   // the clear from across a network would let whoever is on that network sign tokens
   // or revoke them, while a service on the same machine - or the fixtures - is a
   // different matter.
   class HttpsClient
   {
   public:

      struct Response
      {
         Response() : status_code(0), truncated(false) {}

         int status_code;
         AnsiString headers;   // the raw header block, for callers that need one
         AnsiString body;

         // Set only for a caller that asked for the head of a long answer
         // (keep_head_of_long_answer): the answer ran past max_response_bytes,
         // and body is what was read up to there.
         bool truncated;
      };

      // method is GET or POST. extra_headers are complete lines without the CRLF.
      // content_type and body are used for POST. Returns false, with error set, when
      // no response was obtained at all; an HTTP error status is a true return with the
      // status in the response, for the caller to judge.
      //
      // timeout_seconds bounds the whole request - the name lookup, the connection, a
      // proxy's CONNECT, the TLS handshake, sending, and reading the answer to its end -
      // on every platform, not each step of it: an endpoint that accepts and then
      // trickles its answer is given up on at the deadline like one that never answers.
      //
      // HttpProxy, when set, carries the request - except to an address on this machine
      // (localhost, 127.0.0.0/8, ::1), which is reached directly unless the proxy is on
      // this machine too, since a proxy anywhere else would reach its own loopback.
      static bool Request(const AnsiString &method, const AnsiString &url, const std::vector<AnsiString> &extra_headers,
                          const AnsiString &content_type, const AnsiString &body, Response &response, String &error,
                          int timeout_seconds = 20, size_t max_response_bytes = 1024 * 1024);

      // Request, for a URL an outsider chose (a List-Unsubscribe address): the host
      // is reached only if it is public, judged by the ADDRESSES it resolves to and
      // not by its name - loopback, RFC 1918, 169.254/16, 100.64/10, ::1, fc00::/7,
      // fe80::/10, multicast, unspecified and the reserved blocks are refused, an
      // IPv4 address carried inside an IPv6 one judged as the IPv4 - and the
      // connection is made to the addresses that passed, so a second lookup cannot
      // answer differently. Not through HttpProxy: the proxy would resolve the name
      // itself, out of sight of this. Shorter deadline and cap than Request's, since
      // the caller is a REST worker with a request of its own to answer. Otherwise
      // the server could be made to reach what only it can reach.
      //
      // keep_head_of_long_answer is for a caller that judges an answer by its
      // status alone (an inbound webhook's POST): an answer longer than
      // max_response_bytes is read that far, its status line and headers parsed
      // from what was read, and returned with truncated set - rather than taken
      // for no answer at all, which would post the same message again to an
      // endpoint that took it and said so at length.
      static bool RequestPublic(const AnsiString &method, const AnsiString &url, const std::vector<AnsiString> &extra_headers,
                                const AnsiString &content_type, const AnsiString &body, Response &response, String &error,
                                int timeout_seconds = 10, size_t max_response_bytes = 32 * 1024,
                                bool keep_head_of_long_answer = false);

      // Request, for an endpoint the administrator named that may be on this
      // server's own network - an inbound webhook's (InboundWebhooks.h) - but
      // never on a link-local address: loopback and the private ranges are
      // reached, while 169.254/16 and fe80::/10, where the clouds answer their
      // instance metadata, and the metadata addresses outside them
      // (100.100.100.200, 168.63.129.16, 192.0.0.192, fd00:ec2::254) are refused.
      // Judged by the ADDRESSES the name resolves to, as RequestPublic judges, an
      // IPv4 address carried inside an IPv6 one judged as the IPv4, and the
      // connection is made to the addresses that passed, so a second lookup
      // cannot answer differently. Not through HttpProxy, which would resolve the
      // name itself, out of sight of this. Like Request, it follows no redirect:
      // a 3xx is an answer for the caller to judge, never a second connection.
      // keep_head_of_long_answer as for RequestPublic.
      static bool RequestNotLinkLocal(const AnsiString &method, const AnsiString &url, const std::vector<AnsiString> &extra_headers,
                                      const AnsiString &content_type, const AnsiString &body, Response &response, String &error,
                                      int timeout_seconds = 20, size_t max_response_bytes = 1024 * 1024,
                                      bool keep_head_of_long_answer = false);

      // Request, for the message store's object storage (docs/ObjectStorage.md),
      // whose endpoint may be a MinIO or a Ceph gateway behind a CA of the
      // organisation's own. With ca_file empty it is Request exactly. With
      // ca_file naming a PEM file, the certificate is verified against the
      // certificates in that file (and, on Linux, the system's bundle; on
      // Windows the file alone) rather than the machine's root store, the host
      // name - or the address, for an endpoint written as one - and the TLS
      // server usage checked as ever.
      // Verification is never off, and there is no switch that turns it off.
      static bool RequestTrusting(const AnsiString &method, const AnsiString &url, const std::vector<AnsiString> &extra_headers,
                                  const AnsiString &content_type, const AnsiString &body, Response &response, String &error,
                                  int timeout_seconds, size_t max_response_bytes, const String &ca_file);

      // Whether a host - a name, or an address written as one - leads to no
      // link-local or metadata address, judged as RequestNotLinkLocal judges:
      // every address the name resolves to within five seconds. resolved says
      // whether it resolved at all; a name that does not resolve is not refused
      // here (true, resolved false), since the check that holds is the one made
      // at each connection. False with error set when an address is refused.
      static bool HostIsNotLinkLocal(const AnsiString &host, bool &resolved, String &error);

      // GET url to a file, following up to five redirects (to https, or to plain http
      // on a loopback address, as Request accepts), the body streamed to the file
      // rather than held. True when a 200 response was written to path in full and
      // within max_bytes; otherwise false with error set and nothing left at path.
      // status_code is the last response's, or 0 when no response was obtained.
      // timeout_seconds bounds each step - the lookup and connection, the handshake,
      // sending, each read - rather than the whole, which a large file on a slow link
      // would outlast without anything being wrong.
      static bool Download(const AnsiString &url, const String &path, size_t max_bytes, int &status_code, String &error,
                           int timeout_seconds = 60);

      // http(s)://host[:port]/path -> parts. Port defaults to 443 or 80; path to "/".
      static bool ParseUrl(const AnsiString &url, bool &https, AnsiString &host, AnsiString &port, AnsiString &path);

      // localhost, or a literal address that is loopback (127.0.0.0/8, ::1). A name
      // that merely starts with 127. is not.
      static bool IsLoopbackHost(const AnsiString &host);

      // Whether a host - a name, or an address written as one - leads only to
      // public addresses, judged as RequestPublic judges them: every address the
      // name resolves to within five seconds, none loopback, private, link-local
      // or reserved. False with error set otherwise, and when it does not resolve.
      // For a connection made later by something else (a fetch account), this is
      // the check at the moment it is asked, not a guarantee about the next lookup.
      static bool HostIsPublic(const AnsiString &host, String &error);

      // application/x-www-form-urlencoded, one value.
      static AnsiString FormEncode(const AnsiString &value);

   private:
      static bool Request_(const AnsiString &method, const AnsiString &url, const std::vector<AnsiString> &extra_headers,
                           const AnsiString &content_type, const AnsiString &body, Response &response, String &error,
                           int timeout_seconds, size_t max_response_bytes, bool public_only, bool not_link_local,
                           bool keep_head_of_long_answer = false, const String *ca_file = nullptr);
   };
}
