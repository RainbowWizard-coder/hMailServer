// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// A new password checked against the published breach corpus, by the range query
// of the k-anonymity model.
//
// The password is hashed here with SHA-1 over its UTF-8 bytes, and the first five
// hexadecimal characters of that hash are all that is sent: GET <endpoint>/<prefix>.
// The endpoint answers with every suffix in the corpus that shares the prefix, each
// with the number of times it has been seen, and the comparison with the other
// thirty-five characters is made on this machine. The request asks for padding
// (Add-Padding: true), which makes the endpoint add entries with a count of zero so
// that the size of the answer says nothing about which prefix was asked for either.
// Neither the password, nor the hash, nor the suffix ever leaves the server, and
// none of them is written to any log.
//
// Applied where a password is CHOSEN - by the account holder, or by an administrator
// for them or for the server's own administrator credential - and after the local
// rules (PasswordPolicy, PasswordHistory::IsReuse), so that a password those refuse
// costs no request. It is before PasswordHistory::Record at every site, because a
// refused change must not leave a history row behind. Never at logon: a password
// that works today keeps working, as with the policy.
//
// Off by default, because it sends something off the server. The endpoint is a
// setting so an organisation can run its own mirror of the corpus; it must be https,
// or plain http to this machine. What happens when the endpoint cannot be used is a
// setting too, and the default lets the change through - with an entry in the error
// log either way, since a security control that silently did nothing is the worst
// outcome of the three.
//
// Evaluate() is pure - configuration, password and a fetcher in, verdict out - so
// the rules are pinned by PasswordBreachCheckTester without a network or a
// configured server, in the shape PasswordPolicy::Evaluate uses.

#pragma once

#include <functional>

namespace HM
{
   struct BreachCheckConfig
   {
      bool enabled = false;
      String url;
      long minimum_count = 1;

      // 0: let the change through; 1: refuse it. Both are logged.
      int unreachable_action = 0;
   };

   class PasswordBreachCheck
   {
   public:
      enum Verdict
      {
         Acceptable,
         Breached,
         Unavailable
      };

      enum UnreachableAction
      {
         UnreachableAllow = 0,
         UnreachableRefuse = 1
      };

      // The range endpoint of the public corpus.
      static const wchar_t *DefaultUrl;

      // The two sentences a refusal carries, shown to whoever was choosing the
      // password. The Control Panel, the Deck and the webmail translate them; the
      // first two by their English text, the last two by the reason word below.
      static const wchar_t *BreachedSentence;
      static const wchar_t *UnavailableSentence;

      // The machine-readable reason a REST refusal carries beside its sentence.
      static const char *BreachedReason;
      static const char *UnavailableReason;

      // How long one request may take, in seconds, and the most of an answer read.
      static const int RequestTimeoutSeconds = 8;
      static const size_t MaxResponseBytes = 256 * 1024;

      // SHA-1 of the password's UTF-8 bytes, as forty upper-case hexadecimal
      // characters. Never through AnsiString(String), which is the ANSI code page:
      // every password outside ASCII would then be hashed as other bytes and miss.
      static bool Sha1HexUpper(const String &password, AnsiString &hex40);

      // Whether url may be used as the range endpoint. On false, out_reason is a
      // sentence for the administrator that does not repeat the URL's path.
      static bool ValidateEndpoint(const String &url, String &out_reason);

      // The URL requested for one prefix: {prefix} replaced where the endpoint has
      // it, and otherwise the prefix appended to the path.
      static AnsiString RangeUrl(const AnsiString &url, const AnsiString &prefix5);

      // Reads a range answer - lines of SUFFIX:COUNT - for one suffix. False when the
      // body is not such an answer at all. out_count is 0 when the suffix is absent;
      // out_padded is true when any entry has a count of zero; out_entries is the
      // number of lines.
      static bool ParseRange(const AnsiString &body, const AnsiString &suffix35, long &out_count, bool &out_padded, int &out_entries);

      // Fetches one URL. False with error set when no answer was obtained.
      typedef std::function<bool(const AnsiString &url, int &status, AnsiString &body, String &error)> Fetcher;

      // The verdict for one password. out_error explains Unavailable, and never
      // names the password, the hash or the request path; out_error_code is the
      // HM error number that explains it better than "unreachable" (6602 a stored
      // endpoint that is not acceptable, 6603 a password that could not be
      // hashed), or 0.
      static Verdict Evaluate(const BreachCheckConfig &config, const String &password, const Fetcher &fetch,
                              String &out_error, int &out_error_code);

      // The outbound HTTPS client, with the padding header, the timeout and the cap.
      static Fetcher NetworkFetcher();

      // The configured check. Disabled when the server's settings are not loaded -
      // the command-line modes, and the moments before the database is open.
      static BreachCheckConfig GetConfiguredCheck();

      // For the call sites: reads the configuration, evaluates, logs, and answers
      // whether the change may go ahead. On false, out_reason is the sentence to
      // show, out_code the REST reason word and out_http_status 400 (found) or 503
      // (could not be checked, and the server is set to refuse). who names the
      // credential in the log - an address, "an app password", "the administrator".
      static bool IsAcceptable(const String &who, const String &password, String &out_reason,
                               AnsiString &out_code, int &out_http_status);

      // The administrator's test: asks the endpoint for the prefix 00000, which
      // belongs to no password in particular, and reports what came back. Empty url
      // means the configured one. False when the endpoint could not be used, with
      // out_error set; out_status is the HTTP status, or 0 when none arrived.
      //
      // publicOnly: the endpoint is reached only at a public address, over https,
      // as HttpsClient::RequestPublicThroughProxy reaches one - for a test asked by
      // a credential other than the server administrator's (the REST route), whose
      // answer reports the status and the time of whatever the URL reached.
      static bool Probe(const String &url, int &out_status, int &out_entries, bool &out_padded,
                        int &out_milliseconds, String &out_error, bool publicOnly = false);

   private:
      static bool IsLoopbackHost_(const AnsiString &host);

      // NetworkFetcher held to public addresses (Probe, publicOnly).
      static Fetcher PublicFetcher_();

      // Why an answer with no entries in it is not a verdict, for the error log.
      static String EmptyAnswerReason_();
   };

   class PasswordBreachCheckTester
   {
   public:
      void Test();

   private:
      void TestHashing_();
      void TestEndpointValidation_();
      void TestRangeUrl_();
      void TestParseRange_();
      void TestEvaluate_();
   };
}
