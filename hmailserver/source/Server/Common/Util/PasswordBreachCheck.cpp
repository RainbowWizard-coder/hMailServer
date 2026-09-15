// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "PasswordBreachCheck.h"

#include "HttpsClient.h"
#include "Unicode.h"
#include "Hashing/HashCreator.h"
#include "../Application/Configuration.h"
#include "../Application/PropertySet.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   const wchar_t *PasswordBreachCheck::DefaultUrl = L"https://api.pwnedpasswords.com/range/";

   const wchar_t *PasswordBreachCheck::BreachedSentence =
      L"This password appears in a published data breach, so it would be among the first tried against this account. Choose a different one.";

   const wchar_t *PasswordBreachCheck::UnavailableSentence =
      L"The password could not be checked against published data breaches just now, and this server is set to refuse a change it cannot check. Try again later.";

   const char *PasswordBreachCheck::BreachedReason = "breached";
   const char *PasswordBreachCheck::UnavailableReason = "breach_check_unavailable";

   namespace
   {
      const char *PrefixToken = "{prefix}";

      bool IsHex(char c)
      {
         return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
      }

      int CountOccurrences(const std::string &text, const std::string &needle)
      {
         int count = 0;
         for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size()))
            count++;
         return count;
      }
   }

   bool
   PasswordBreachCheck::Sha1HexUpper(const String &password, AnsiString &hex40)
   {
      hex40 = "";

      AnsiString utf8;
      if (!Unicode::WideToMultiByte(password, utf8))
         return false;

      // A copy, because the hasher takes a non-const pointer; an empty password is
      // hashed like any other string rather than read through a null.
      std::vector<unsigned char> bytes(utf8.c_str(), utf8.c_str() + utf8.GetLength());
      unsigned char none = 0;

      HashCreator hasher(HashCreator::SHA1);
      AnsiString hex = hasher.GenerateHashNoSalt(bytes.empty() ? &none : &bytes[0], (int) bytes.size(), HashCreator::hex);
      hex.MakeUpper();

      if (hex.GetLength() != 40)
         return false;

      hex40 = hex;
      return true;
   }

   String
   PasswordBreachCheck::EmptyAnswerReason_()
   {
      return _T("the endpoint answered with no entries at all, which no range of the corpus is (a mirror answers a prefix it holds nothing under with padding entries)");
   }

   bool
   PasswordBreachCheck::IsLoopbackHost_(const AnsiString &host)
   {
      // Parsed as an address, never matched as text: "127.example.com" starts with
      // "127." and is somebody else's machine. HttpsClient::IsLoopbackHost answers
      // exactly that way since this check arrived, and it is asked here rather than
      // copied so the endpoint accepted is the endpoint the client will reach.
      return HttpsClient::IsLoopbackHost(host);
   }

   bool
   PasswordBreachCheck::ValidateEndpoint(const String &url, String &out_reason)
   {
      out_reason = "";

      if (url.IsEmpty())
      {
         out_reason = _T("The breach check endpoint is empty. The public corpus is at https://api.pwnedpasswords.com/range/.");
         return false;
      }

      if (url.GetLength() > 2048)
      {
         out_reason = _T("The breach check endpoint is longer than 2048 characters.");
         return false;
      }

      for (int i = 0; i < url.GetLength(); i++)
      {
         const wchar_t c = url[i];
         if (c <= 0x20 || c >= 0x7F)
         {
            out_reason = _T("The breach check endpoint contains a space, a control character or a character outside ASCII; percent-encode anything that is not ASCII.");
            return false;
         }
      }

      // ASCII from here, so the narrowing is exact.
      const std::string text = AnsiString(url).c_str();

      if (text.find('#') != std::string::npos)
      {
         out_reason = _T("The breach check endpoint must not contain a fragment (#).");
         return false;
      }

      std::string lower = text;
      std::transform(lower.begin(), lower.end(), lower.begin(), [](char c) { return (char) tolower((unsigned char) c); });

      const bool https = lower.compare(0, 8, "https://") == 0;
      if (!https && lower.compare(0, 7, "http://") != 0)
      {
         out_reason = _T("The breach check endpoint must be an https URL, such as https://api.pwnedpasswords.com/range/.");
         return false;
      }

      // The scheme is compared without case above and must be sent as written below,
      // where HttpsClient compares it with case; so it must also be written in lower case.
      if (text.compare(0, https ? 8 : 7, https ? "https://" : "http://") != 0)
      {
         out_reason = _T("The breach check endpoint's scheme must be written in lower case (https://).");
         return false;
      }

      const size_t authorityStart = https ? 8 : 7;
      const size_t pathStart = text.find('/', authorityStart);
      const std::string authority = text.substr(authorityStart, pathStart == std::string::npos ? std::string::npos : pathStart - authorityStart);

      if (authority.find('@') != std::string::npos)
      {
         out_reason = _T("The breach check endpoint must not carry a user name or a password.");
         return false;
      }

      if (authority.find_first_of("?{}") != std::string::npos)
      {
         out_reason = _T("The breach check endpoint's host is not a host name, an IPv4 address or a bracketed IPv6 address.");
         return false;
      }

      bool parsedHttps = false;
      AnsiString host, port, path;
      if (!HttpsClient::ParseUrl(AnsiString(text.c_str()), parsedHttps, host, port, path))
      {
         out_reason = _T("The breach check endpoint is not an https URL with a host and an optional port.");
         return false;
      }

      for (int i = 0; i < host.GetLength(); i++)
      {
         const char c = host.c_str()[i];
         if (!(isalnum((unsigned char) c) || c == '.' || c == '-' || c == ':'))
         {
            out_reason = _T("The breach check endpoint's host is not a host name, an IPv4 address or a bracketed IPv6 address.");
            return false;
         }
      }

      const long portNumber = atol(port.c_str());
      if (port.GetLength() > 5 || portNumber < 1 || portNumber > 65535)
      {
         out_reason = _T("The breach check endpoint's port must be between 1 and 65535.");
         return false;
      }

      const int tokens = CountOccurrences(text, PrefixToken);
      if (tokens > 1)
      {
         out_reason = _T("The breach check endpoint may contain {prefix} once at most.");
         return false;
      }

      // A brace that is not part of the one token would be sent as it is written,
      // which no server reads as the prefix.
      std::string withoutToken = text;
      if (tokens == 1)
         withoutToken.erase(withoutToken.find(PrefixToken), strlen(PrefixToken));
      if (withoutToken.find_first_of("{}") != std::string::npos)
      {
         out_reason = _T("The breach check endpoint may contain braces only as {prefix}.");
         return false;
      }

      if (tokens == 0 && text.find('?') != std::string::npos)
      {
         out_reason = _T("The breach check endpoint may have a query string only when {prefix} says where the prefix goes; otherwise the prefix is appended to the path.");
         return false;
      }

      if (!https && !IsLoopbackHost_(host))
      {
         out_reason = _T("Plain http is accepted only to this machine (localhost, 127.0.0.0/8 or ::1). Use https for any other host.");
         return false;
      }

      return true;
   }

   AnsiString
   PasswordBreachCheck::RangeUrl(const AnsiString &url, const AnsiString &prefix5)
   {
      std::string text = url.c_str();

      const size_t token = text.find(PrefixToken);
      if (token != std::string::npos)
      {
         text.replace(token, strlen(PrefixToken), prefix5.c_str());
         return AnsiString(text.c_str());
      }

      if (text.empty() || text[text.size() - 1] != '/')
      {
         // http://host is http://host/ plus the prefix, and so is http://host/range.
         text += "/";
      }

      text += prefix5.c_str();
      return AnsiString(text.c_str());
   }

   bool
   PasswordBreachCheck::ParseRange(const AnsiString &body, const AnsiString &suffix35, long &out_count, bool &out_padded, int &out_entries)
   {
      out_count = 0;
      out_padded = false;
      out_entries = 0;

      const std::string text(body.c_str(), (size_t) body.GetLength());
      size_t start = 0;

      while (start <= text.size())
      {
         size_t end = text.find('\n', start);
         if (end == std::string::npos)
            end = text.size();

         std::string line = text.substr(start, end - start);
         start = end + 1;

         // CR and spaces are the only decoration a range answer carries.
         line.erase(std::remove_if(line.begin(), line.end(), [](char c) { return c == '\r' || c == ' ' || c == '\t'; }), line.end());

         if (line.empty())
         {
            if (end == text.size())
               break;
            continue;
         }

         // Exactly thirty-five hexadecimal characters, a colon, and a count. Anything
         // else - an HTML page from a captive portal, a JSON error, a truncated line -
         // means this was not a range answer, and "not found in it" would be a lie.
         if (line.size() < 37 || line[35] != ':')
            return false;

         for (size_t i = 0; i < 35; i++)
         {
            if (!IsHex(line[i]))
               return false;
         }

         const std::string digits = line.substr(36);
         if (digits.empty() || digits.size() > 12 || digits.find_first_not_of("0123456789") != std::string::npos)
            return false;

         long long parsed = 0;
         for (char d : digits)
            parsed = parsed * 10 + (d - '0');
         const long count = parsed > LONG_MAX ? LONG_MAX : (long) parsed;

         out_entries++;

         if (count == 0)
            out_padded = true;

         if (_strnicmp(line.c_str(), suffix35.c_str(), 35) == 0 && suffix35.GetLength() == 35)
            out_count = count;

         if (end == text.size())
            break;
      }

      return true;
   }

   PasswordBreachCheck::Verdict
   PasswordBreachCheck::Evaluate(const BreachCheckConfig &config, const String &password, const Fetcher &fetch,
                                 String &out_error, int &out_error_code)
   {
      out_error = "";
      out_error_code = 0;

      if (!config.enabled)
         return Acceptable;

      // Nothing to look up. Every call site refuses an empty password before this.
      if (password.IsEmpty())
         return Acceptable;

      String urlProblem;
      if (!ValidateEndpoint(config.url, urlProblem))
      {
         out_error = _T("the configured endpoint cannot be used: ") + urlProblem;
         out_error_code = 6602;
         return Unavailable;
      }

      AnsiString hex;
      if (!Sha1HexUpper(password, hex))
      {
         out_error = _T("the password could not be converted to UTF-8 and hashed");
         out_error_code = 6603;
         return Unavailable;
      }

      const AnsiString prefix = hex.Left(5);
      const AnsiString suffix = hex.Mid(5);

      int status = 0;
      AnsiString body;
      String fetchError;
      if (!fetch(RangeUrl(AnsiString(config.url), prefix), status, body, fetchError))
      {
         out_error = fetchError.IsEmpty() ? String(_T("no answer was obtained")) : fetchError;
         return Unavailable;
      }

      if (status != 200)
      {
         out_error.Format(_T("the endpoint answered with HTTP status %d"), status);
         return Unavailable;
      }

      long count = 0;
      bool padded = false;
      int entries = 0;
      if (!ParseRange(body, suffix, count, padded, entries))
      {
         out_error = _T("the endpoint's answer was not a list of hash suffixes and counts");
         return Unavailable;
      }

      // An answer with nothing in it is not "not found". Every prefix of the public
      // corpus has hundreds of suffixes, and padding was asked for besides, so an empty
      // 200 is a mirror serving empty files or a proxy answering for an endpoint it
      // could not reach - which, read as a verdict, would switch the check off without
      // a word in any log. A mirror of a smaller corpus answers a prefix it holds
      // nothing under with padding entries, which count.
      if (entries == 0)
      {
         out_error = EmptyAnswerReason_();
         return Unavailable;
      }

      const long minimum = config.minimum_count < 1 ? 1 : config.minimum_count;
      return count >= minimum ? Breached : Acceptable;
   }

   PasswordBreachCheck::Fetcher
   PasswordBreachCheck::NetworkFetcher()
   {
      return [](const AnsiString &url, int &status, AnsiString &body, String &error) -> bool
      {
         std::vector<AnsiString> headers;
         headers.push_back("Add-Padding: true");

         HttpsClient::Response response;
         if (!HttpsClient::Request("GET", url, headers, "", "", response, error, RequestTimeoutSeconds, MaxResponseBytes))
            return false;

         status = response.status_code;
         body = response.body;
         return true;
      };
   }

   BreachCheckConfig
   PasswordBreachCheck::GetConfiguredCheck()
   {
      BreachCheckConfig config;

      Configuration *configuration = Configuration::Instance();
      if (!configuration || !configuration->GetSettings())
         return config;

      config.enabled = configuration->GetBreachCheckEnabled();
      config.url = configuration->GetBreachCheckUrl();
      config.minimum_count = configuration->GetBreachCheckMinimumCount();
      config.unreachable_action = configuration->GetBreachCheckUnreachableAction();

      return config;
   }

   bool
   PasswordBreachCheck::IsAcceptable(const String &who, const String &password, String &out_reason,
                                     AnsiString &out_code, int &out_http_status)
   {
      out_reason = "";
      out_code = "";
      out_http_status = 0;

      const BreachCheckConfig config = GetConfiguredCheck();
      if (!config.enabled)
         return true;

      Verdict verdict = Unavailable;
      String error;
      int errorCode = 0;

      try
      {
         verdict = Evaluate(config, password, NetworkFetcher(), error, errorCode);
      }
      catch (...)
      {
         verdict = Unavailable;
         error = _T("an unexpected error ended the check");
         errorCode = 6604;
      }

      if (verdict == Acceptable)
         return true;

      if (verdict == Breached)
      {
         // Who, and nothing about the password: not the hash, not its prefix, not the
         // count, which together with the prefix would narrow it to a handful.
         String message = _T("Password breach check: a new password for ") + who +
                          _T(" was refused; it appears in the breach corpus.");
         LOG_APPLICATION(message);

         out_reason = BreachedSentence;
         out_code = BreachedReason;
         out_http_status = 400;
         return false;
      }

      const bool refuse = config.unreachable_action == UnreachableRefuse;
      const int code = errorCode != 0 ? errorCode : (refuse ? 6601 : 6600);

      String description = _T("The password breach check could not be completed for ") + who + _T(" (") + error + _T("), so ") +
         (refuse ? String(_T("the change was refused")) : String(_T("the new password was accepted without it"))) +
         _T(", as BreachCheckUnreachableAction says.");

      ErrorManager::Instance()->ReportError(ErrorManager::Medium, code, "PasswordBreachCheck::IsAcceptable", description);

      if (!refuse)
         return true;

      out_reason = UnavailableSentence;
      out_code = UnavailableReason;
      out_http_status = 503;
      return false;
   }

   bool
   PasswordBreachCheck::Probe(const String &url, int &out_status, int &out_entries, bool &out_padded,
                              int &out_milliseconds, String &out_error)
   {
      out_status = 0;
      out_entries = 0;
      out_padded = false;
      out_milliseconds = 0;
      out_error = "";

      String endpoint = url;
      if (endpoint.IsEmpty())
         endpoint = GetConfiguredCheck().url;

      if (!ValidateEndpoint(endpoint, out_error))
         return false;

      const auto started = std::chrono::steady_clock::now();

      int status = 0;
      AnsiString body;
      String fetchError;
      bool answered = false;

      try
      {
         // 00000 is a prefix, not a password: the probe tells the endpoint nothing.
         answered = NetworkFetcher()(RangeUrl(AnsiString(endpoint), "00000"), status, body, fetchError);
      }
      catch (...)
      {
         fetchError = _T("an unexpected error ended the request");
      }

      out_milliseconds = (int) std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
      out_status = answered ? status : 0;

      if (!answered)
      {
         out_error = fetchError.IsEmpty() ? String(_T("no answer was obtained")) : fetchError;
         return false;
      }

      if (status != 200)
      {
         out_error.Format(_T("the endpoint answered with HTTP status %d"), status);
         return false;
      }

      long count = 0;
      if (!ParseRange(body, "00000000000000000000000000000000000", count, out_padded, out_entries))
      {
         out_error = _T("the endpoint's answer was not a list of hash suffixes and counts");
         return false;
      }

      // As Evaluate reads it: an empty answer is an endpoint the check could not use.
      if (out_entries == 0)
      {
         out_error = EmptyAnswerReason_();
         return false;
      }

      return true;
   }

   // ------------------------------------------------------------------------
   // The in-process tests, run by ClassTester::DoTests.
   // ------------------------------------------------------------------------

   void
   PasswordBreachCheckTester::Test()
   {
      TestHashing_();
      TestEndpointValidation_();
      TestRangeUrl_();
      TestParseRange_();
      TestEvaluate_();
   }

   void
   PasswordBreachCheckTester::TestHashing_()
   {
      AnsiString hex;

      // The published vector: SHA-1("password").
      if (!PasswordBreachCheck::Sha1HexUpper(_T("password"), hex) || hex != "5BAA61E4C9B93F3F0682250B6CF8331B7EE68FD8")
         throw std::logic_error("PasswordBreachCheck::Sha1HexUpper did not produce SHA-1(\"password\") in upper case.");

      // UTF-8, not the ANSI code page and not UTF-16: p-a-umlaut-s-s-w-o-umlaut-r-d-2026.
      if (!PasswordBreachCheck::Sha1HexUpper(L"p\u00e4ssw\u00f6rd-2026", hex) || hex != "7DF5AB98EEEBE93B14A0AB70F4284204AC981A4B")
         throw std::logic_error("PasswordBreachCheck::Sha1HexUpper did not hash a non-ASCII password over its UTF-8 bytes.");

      if (hex == "6C5D660DACA251FD5140ACCDE9F22F03A0A476EF" || hex == "C0CBAF78F9AA1186C3F926CF166E53A52E12E48D")
         throw std::logic_error("PasswordBreachCheck::Sha1HexUpper hashed the Windows-1252 or UTF-16 bytes.");
   }

   void
   PasswordBreachCheckTester::TestEndpointValidation_()
   {
      const wchar_t *accepted[] =
      {
         L"https://api.pwnedpasswords.com/range/",
         L"https://api.pwnedpasswords.com/range",
         L"https://mirror.example/range/",
         L"https://mirror.example:8443/p/{prefix}.txt",
         L"https://mirror.example/lookup?prefix={prefix}",
         L"http://127.0.0.1:9551/range/",
         L"http://127.9.9.9/range/",
         L"http://localhost:9551/range/",
         L"http://[::1]:9551/range/"
      };

      const wchar_t *refused[] =
      {
         L"",
         L"http://example.com/range/",
         L"http://127.evil.example/range/",
         L"http://127.0.0.1.evil.example/range/",
         L"http://10.0.0.1/range/",
         L"https://user@mirror.example/range/",
         L"https://user:pass@mirror.example/range/",
         L"https://mirror.example/range/#x",
         L"https://mirror.example/range/?a=1",
         L"https://mirror.example/{prefix}/{prefix}",
         L"https://mirror.example/{prefx}",
         L"ftp://mirror.example/range/",
         L"HTTPS://mirror.example/range/",
         L"https://mirror example/range/",
         L"https://mirror.example:0/range/",
         L"https://mirror.example:70000/range/",
         L"https:///range/",
         L"https://m\u00e9rror.example/range/"
      };

      String reason;

      for (const wchar_t *url : accepted)
      {
         if (!PasswordBreachCheck::ValidateEndpoint(url, reason))
            throw std::logic_error("PasswordBreachCheck::ValidateEndpoint refused an endpoint it must accept.");
      }

      for (const wchar_t *url : refused)
      {
         if (PasswordBreachCheck::ValidateEndpoint(url, reason) || reason.IsEmpty())
            throw std::logic_error("PasswordBreachCheck::ValidateEndpoint accepted an endpoint it must refuse, or refused one without a reason.");
      }

      // The reason never repeats the path, which is where a mirror's secret would be.
      PasswordBreachCheck::ValidateEndpoint(L"http://example.com/secret-path-9551/", reason);
      if (reason.Find(_T("secret-path-9551")) >= 0)
         throw std::logic_error("PasswordBreachCheck::ValidateEndpoint repeated the URL's path in its reason.");
   }

   void
   PasswordBreachCheckTester::TestRangeUrl_()
   {
      if (PasswordBreachCheck::RangeUrl("https://api.pwnedpasswords.com/range/", "5BAA6") != "https://api.pwnedpasswords.com/range/5BAA6")
         throw std::logic_error("PasswordBreachCheck::RangeUrl did not append the prefix to a path ending in a slash.");

      if (PasswordBreachCheck::RangeUrl("https://mirror.example/range", "5BAA6") != "https://mirror.example/range/5BAA6")
         throw std::logic_error("PasswordBreachCheck::RangeUrl did not add the slash before the prefix.");

      if (PasswordBreachCheck::RangeUrl("https://mirror.example/p/{prefix}.txt", "5BAA6") != "https://mirror.example/p/5BAA6.txt")
         throw std::logic_error("PasswordBreachCheck::RangeUrl did not put the prefix where {prefix} is.");
   }

   void
   PasswordBreachCheckTester::TestParseRange_()
   {
      const AnsiString suffix = "1E4C9B93F3F0682250B6CF8331B7EE68FD8";
      long count = -1;
      bool padded = true;
      int entries = -1;

      AnsiString crlf = "0018A45C4D1DEF81644B54AB7F969B88D65:1\r\n1E4C9B93F3F0682250B6CF8331B7EE68FD8:10434004\r\n00D4F6E8FA6EECAD2A3AA415EEC418D38EC:0\r\n";
      if (!PasswordBreachCheck::ParseRange(crlf, suffix, count, padded, entries) || count != 10434004 || !padded || entries != 3)
         throw std::logic_error("PasswordBreachCheck::ParseRange misread a CRLF answer with a padding entry.");

      AnsiString lowerLf = "0018a45c4d1def81644b54ab7f969b88d65:2\n1e4c9b93f3f0682250b6cf8331b7ee68fd8:7";
      if (!PasswordBreachCheck::ParseRange(lowerLf, suffix, count, padded, entries) || count != 7 || padded || entries != 2)
         throw std::logic_error("PasswordBreachCheck::ParseRange misread a lower-case LF answer without a final line break.");

      AnsiString absent = "0018A45C4D1DEF81644B54AB7F969B88D65:1\r\n";
      if (!PasswordBreachCheck::ParseRange(absent, suffix, count, padded, entries) || count != 0)
         throw std::logic_error("PasswordBreachCheck::ParseRange found a suffix that is not there.");

      if (!PasswordBreachCheck::ParseRange("", suffix, count, padded, entries) || count != 0 || entries != 0)
         throw std::logic_error("PasswordBreachCheck::ParseRange did not read an empty body as an answer with nothing in it.");

      if (PasswordBreachCheck::ParseRange("<html><body>Sign in to the network</body></html>", suffix, count, padded, entries))
         throw std::logic_error("PasswordBreachCheck::ParseRange read an HTML page as a range answer.");

      if (PasswordBreachCheck::ParseRange("0018A45C4D1DEF81644B54AB7F969B88D65:1\r\n1E4C9B93F3F0682250B6CF8331B7EE68FD:3\r\n", suffix, count, padded, entries))
         throw std::logic_error("PasswordBreachCheck::ParseRange accepted a truncated line.");

      if (PasswordBreachCheck::ParseRange("0018A45C4D1DEF81644B54AB7F969B88D65:x\r\n", suffix, count, padded, entries))
         throw std::logic_error("PasswordBreachCheck::ParseRange accepted a count that is not a number.");
   }

   void
   PasswordBreachCheckTester::TestEvaluate_()
   {
      BreachCheckConfig config;
      config.enabled = true;
      config.url = _T("https://mirror.example/range/");
      config.minimum_count = 1;

      std::vector<AnsiString> requested;
      AnsiString answer = "1E4C9B93F3F0682250B6CF8331B7EE68FD8:3\r\n0018A45C4D1DEF81644B54AB7F969B88D65:0\r\n";
      int answerStatus = 200;
      bool reachable = true;

      PasswordBreachCheck::Fetcher fake = [&](const AnsiString &url, int &status, AnsiString &body, String &error) -> bool
      {
         requested.push_back(url);
         if (!reachable)
         {
            error = _T("The request to mirror.example failed: refused");
            return false;
         }
         status = answerStatus;
         body = answer;
         return true;
      };

      String error;
      int code = 0;

      // Off: nothing is asked.
      BreachCheckConfig off = config;
      off.enabled = false;
      if (PasswordBreachCheck::Evaluate(off, _T("password"), fake, error, code) != PasswordBreachCheck::Acceptable || !requested.empty())
         throw std::logic_error("PasswordBreachCheck::Evaluate made a request while the check is off.");

      // What leaves is the five-character prefix, and nothing else.
      if (PasswordBreachCheck::Evaluate(config, _T("password"), fake, error, code) != PasswordBreachCheck::Breached)
         throw std::logic_error("PasswordBreachCheck::Evaluate did not find a suffix the answer carries.");
      if (requested.size() != 1 || requested[0] != "https://mirror.example/range/5BAA6")
         throw std::logic_error("PasswordBreachCheck::Evaluate requested something other than the range for 5BAA6.");

      // The minimum count is a boundary: three sightings refuse at three, not at four.
      config.minimum_count = 3;
      if (PasswordBreachCheck::Evaluate(config, _T("password"), fake, error, code) != PasswordBreachCheck::Breached)
         throw std::logic_error("PasswordBreachCheck::Evaluate accepted a password seen exactly the minimum number of times.");
      config.minimum_count = 4;
      if (PasswordBreachCheck::Evaluate(config, _T("password"), fake, error, code) != PasswordBreachCheck::Acceptable)
         throw std::logic_error("PasswordBreachCheck::Evaluate refused a password seen fewer times than the minimum.");
      config.minimum_count = 0;
      if (PasswordBreachCheck::Evaluate(config, _T("password"), fake, error, code) != PasswordBreachCheck::Breached)
         throw std::logic_error("PasswordBreachCheck::Evaluate treated a minimum below one as anything but one.");
      config.minimum_count = 1;

      // A padding entry has a count of zero and can never match.
      answer = "1E4C9B93F3F0682250B6CF8331B7EE68FD8:0\r\n";
      if (PasswordBreachCheck::Evaluate(config, _T("password"), fake, error, code) != PasswordBreachCheck::Acceptable)
         throw std::logic_error("PasswordBreachCheck::Evaluate refused a password found only as a padding entry.");

      // Anything that is not an answer is not "not found".
      answer = "<html>captive portal</html>";
      if (PasswordBreachCheck::Evaluate(config, _T("password"), fake, error, code) != PasswordBreachCheck::Unavailable || error.IsEmpty())
         throw std::logic_error("PasswordBreachCheck::Evaluate read a page that is not a range answer as a verdict.");

      // An empty 200 is not "not found" either: an endpoint serving empty files, or a
      // proxy answering for one it could not reach, would otherwise switch the check
      // off without a word in any log.
      answer = "";
      if (PasswordBreachCheck::Evaluate(config, _T("password"), fake, error, code) != PasswordBreachCheck::Unavailable || error.IsEmpty() || code != 0)
         throw std::logic_error("PasswordBreachCheck::Evaluate read an empty answer as a verdict.");
      if (error.Find(_T("password")) >= 0 || error.Find(_T("5BAA6")) >= 0)
         throw std::logic_error("PasswordBreachCheck::Evaluate put the password or its hash in the empty-answer error.");

      // ...while an answer holding nothing but padding is an answer, and a good one.
      answer = "0018A45C4D1DEF81644B54AB7F969B88D65:0\r\n";
      if (PasswordBreachCheck::Evaluate(config, _T("password"), fake, error, code) != PasswordBreachCheck::Acceptable)
         throw std::logic_error("PasswordBreachCheck::Evaluate did not take an answer of padding alone as a verdict.");

      answer = "";
      answerStatus = 503;
      if (PasswordBreachCheck::Evaluate(config, _T("password"), fake, error, code) != PasswordBreachCheck::Unavailable)
         throw std::logic_error("PasswordBreachCheck::Evaluate read an HTTP error status as a verdict.");
      answerStatus = 200;

      reachable = false;
      if (PasswordBreachCheck::Evaluate(config, _T("password"), fake, error, code) != PasswordBreachCheck::Unavailable || code != 0)
         throw std::logic_error("PasswordBreachCheck::Evaluate did not report an endpoint that gave no answer as unavailable.");
      reachable = true;

      // An endpoint that is not acceptable is never asked.
      requested.clear();
      BreachCheckConfig plainHttp = config;
      plainHttp.url = _T("http://example.com/range/");
      if (PasswordBreachCheck::Evaluate(plainHttp, _T("password"), fake, error, code) != PasswordBreachCheck::Unavailable || code != 6602 || !requested.empty())
         throw std::logic_error("PasswordBreachCheck::Evaluate used an endpoint that is not acceptable.");

      // The error text names neither the password nor any part of its hash.
      if (error.Find(_T("password")) >= 0 || error.Find(_T("5BAA6")) >= 0)
         throw std::logic_error("PasswordBreachCheck::Evaluate put the password or its hash in the error text.");
   }
}
