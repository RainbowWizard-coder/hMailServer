// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"

#include "MessageStoreObjectClient.h"

#include "../Util/HttpsClient.h"
#include "../Util/ObjectStoreSignature.h"
#include "../Util/Strings/Formatter.h"
#include "../Util/Unicode.h"

#include <openssl/evp.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <thread>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      typedef ObjectStoreSignature Sig;

      // The Azure API version every request names: Put Block List with a
      // Content-MD5 for the whole blob, and Copy Blob, as proved against
      // Azurite (docs/ObjectStorage.md).
      const char *AzureVersion = "2021-08-06";

      // How long a request's answer may be when it is not an object's bytes: a
      // listing's page, an error's XML.
      const size_t AnswerBytes = 8 * 1024 * 1024;

      // What a read to the end asks for first, and at a time after that.
      const size_t FirstReadBytes = 1024 * 1024;
      const size_t WindowReadBytes = 8 * 1024 * 1024;

      // How long the store is left alone after a request it did not answer,
      // and how long a bucket that answered is taken to be there.
      const long long PauseMilliseconds = 10 * 1000;
      const long long ConfirmedMilliseconds = 60 * 1000;

      thread_local String last_problem;

      std::atomic<long long> last_unreachable_report(0);
      std::atomic<long long> last_refused_report(0);
      std::atomic<long long> last_failed_report(0);

      long long NowMs()
      {
         return (long long) std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
      }

      // Once a minute at most, across every thread.
      bool ReportDue(std::atomic<long long> &last)
      {
         const long long now = NowMs();
         long long previous = last.load();
         if (previous != 0 && now - previous < 60 * 1000)
            return false;

         return last.compare_exchange_strong(previous, now);
      }

      // The whole request, bounded: 15 seconds, 2 more for every MB it moves,
      // 120 at most.
      int TimeoutFor(size_t bytes)
      {
         const size_t seconds = 15 + 2 * (bytes / (1024 * 1024));
         return seconds > 120 ? 120 : (int) seconds;
      }

      std::string TrimmedText(const std::string &text)
      {
         const size_t first = text.find_first_not_of(" \t\r\n");
         if (first == std::string::npos)
            return std::string();
         const size_t last = text.find_last_not_of(" \t\r\n");
         return text.substr(first, last - first + 1);
      }

      // Text that goes into a request line or a header as it is - an endpoint,
      // a bucket, an account, a region, a SAS: printable ASCII and no space,
      // so that a value with a CR, an LF or a space in it cannot add a header
      // or split the request line. A '#' only where it is allowed.
      bool PlainToken(const std::string &text, bool allowHash)
      {
         for (size_t i = 0; i < text.size(); i++)
         {
            const unsigned char c = (unsigned char) text[i];
            if (c <= 0x20 || c >= 0x7F || (!allowHash && c == '#'))
               return false;
         }
         return true;
      }

      std::string LowerAsciiOf(const std::string &text)
      {
         std::string lower = text;
         for (size_t i = 0; i < lower.size(); i++)
         {
            if (lower[i] >= 'A' && lower[i] <= 'Z')
               lower[i] = (char) (lower[i] - 'A' + 'a');
         }
         return lower;
      }

      std::string Unquoted(const std::string &etag)
      {
         std::string value = TrimmedText(etag);
         if (value.size() >= 2 && value[0] == '"' && value[value.size() - 1] == '"')
            value = value.substr(1, value.size() - 2);
         return value;
      }

      unsigned __int64 DecimalValueOf(const std::string &text)
      {
         const std::string value = TrimmedText(text);
         unsigned __int64 number = 0;
         for (size_t i = 0; i < value.size() && value[i] >= '0' && value[i] <= '9'; i++)
            number = number * 10 + (unsigned __int64) (value[i] - '0');
         return number;
      }

      // Seconds since 1970 of a civil date in UTC, without the C library's
      // time zone: the days-from-civil arithmetic.
      __int64 UtcSeconds(int year, int month, int day, int hour, int minute, int second)
      {
         year -= month <= 2 ? 1 : 0;
         const __int64 era = (year >= 0 ? year : year - 399) / 400;
         const __int64 yearOfEra = year - era * 400;
         const __int64 dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
         const __int64 dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
         const __int64 days = era * 146097 + dayOfEra - 719468;
         return days * 86400 + hour * 3600 + minute * 60 + second;
      }

      // The digits of text from at, count of them exactly; -1 when they are
      // not all digits.
      int DigitsAt(const std::string &text, size_t at, size_t count)
      {
         if (at + count > text.size())
            return -1;

         int value = 0;
         for (size_t i = at; i < at + count; i++)
         {
            if (text[i] < '0' || text[i] > '9')
               return -1;
            value = value * 10 + (text[i] - '0');
         }
         return value;
      }

      // S3's 2026-09-25T05:12:52.000Z. 0 when it is not that.
      __int64 IsoTimeOf(const std::string &text)
      {
         const int year = DigitsAt(text, 0, 4);
         const int month = DigitsAt(text, 5, 2);
         const int day = DigitsAt(text, 8, 2);
         const int hour = DigitsAt(text, 11, 2);
         const int minute = DigitsAt(text, 14, 2);
         const int second = DigitsAt(text, 17, 2);
         if (year < 0 || month < 1 || month > 12 || day < 1 || hour < 0 || minute < 0 || second < 0 ||
             text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' || text[16] != ':')
            return 0;

         return UtcSeconds(year, month, day, hour, minute, second);
      }

      // Azure's Fri, 25 Sep 2026 05:12:52 GMT. 0 when it is not that.
      __int64 Rfc1123TimeOf(const std::string &text)
      {
         static const char *months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

         // "Fri, 25 Sep 2026 05:12:52 GMT": the day at 5, the month at 8, the
         // year at 12, the time at 17.
         if (text.size() < 25 || text[3] != ',')
            return 0;

         const int day = DigitsAt(text, 5, 2);
         const int year = DigitsAt(text, 12, 4);
         const int hour = DigitsAt(text, 17, 2);
         const int minute = DigitsAt(text, 20, 2);
         const int second = DigitsAt(text, 23, 2);
         if (day < 1 || year < 0 || hour < 0 || minute < 0 || second < 0)
            return 0;

         const std::string monthName = text.substr(8, 3);
         for (int month = 0; month < 12; month++)
         {
            if (monthName == months[month])
               return UtcSeconds(year, month + 1, day, hour, minute, second);
         }
         return 0;
      }

      std::string QueryString(const Sig::Pairs &query)
      {
         std::string text;
         for (size_t i = 0; i < query.size(); i++)
         {
            if (i > 0)
               text += '&';
            text += Sig::UriEncode(query[i].first, true) + "=" + Sig::UriEncode(query[i].second, true);
         }
         return text;
      }

      std::string DecimalOf(unsigned __int64 value)
      {
         char text[32];
         snprintf(text, sizeof(text), "%llu", (unsigned long long) value);
         return std::string(text);
      }

      // The MD5 of what a put reads, built up part by part.
      class RunningMd5
      {
      public:
         RunningMd5() : context_(EVP_MD_CTX_new())
         {
            if (context_ && EVP_DigestInit_ex(context_, EVP_md5(), nullptr) != 1)
            {
               EVP_MD_CTX_free(context_);
               context_ = nullptr;
            }
         }

         ~RunningMd5()
         {
            if (context_)
               EVP_MD_CTX_free(context_);
         }

         void Add(const std::string &bytes)
         {
            if (context_ && !bytes.empty())
               EVP_DigestUpdate(context_, bytes.data(), bytes.size());
         }

         std::string Finish()
         {
            unsigned char digest[EVP_MAX_MD_SIZE];
            unsigned int length = 0;
            if (!context_ || EVP_DigestFinal_ex(context_, digest, &length) != 1)
               return std::string();
            return std::string((const char *) digest, length);
         }

         RunningMd5(const RunningMd5 &) = delete;
         RunningMd5 &operator=(const RunningMd5 &) = delete;

      private:
         EVP_MD_CTX *context_;
      };

      String WideOfUtf8(const std::string &utf8)
      {
         String wide;
         Unicode::MultiByteToWide(AnsiString(utf8.c_str()), wide);
         return wide;
      }
   }

   // ---- The base: sending, retrying, pausing, reporting ------------------------

   bool
   MessageStoreObjectClient::Settings::operator==(const Settings &other) const
   {
      return service == other.service && endpoint == other.endpoint && region == other.region && bucket == other.bucket &&
             addressing == other.addressing && access_key == other.access_key && secret == other.secret && sas == other.sas &&
             ca_file == other.ca_file;
   }

   MessageStoreObjectClient::MessageStoreObjectClient() :
      paused_until_(0),
      confirmed_until_(0)
   {
   }

   bool
   MessageStoreObjectClient::Paused() const
   {
      return NowMs() < paused_until_.load();
   }

   MessageStoreObjectClient::Outcome
   MessageStoreObjectClient::Read(const std::string &key, unsigned __int64 offset, unsigned __int64 length,
                                  std::string &bytes, unsigned __int64 &total, const std::string &ifMatch, std::string *etag)
   {
      bytes.clear();
      total = 0;
      if (etag)
         etag->clear();

      std::string firstEtag;
      Outcome outcome = ReadRange_(key, offset, length > 0 ? length : (unsigned __int64) FirstReadBytes, ifMatch, bytes, total, firstEtag);
      if (outcome != OutcomeDone)
         return outcome;

      if (etag)
         *etag = firstEtag;

      if (length > 0)
         return OutcomeDone;

      // To the end, a window at a time, each held to the first one's ETag.
      const std::string match = !ifMatch.empty() ? ifMatch : firstEtag;
      while (offset + (unsigned __int64) bytes.size() < total)
      {
         std::string window;
         std::string windowEtag;
         unsigned __int64 windowTotal = 0;
         outcome = ReadRange_(key, offset + (unsigned __int64) bytes.size(), (unsigned __int64) WindowReadBytes, match, window, windowTotal, windowEtag);
         if (outcome != OutcomeDone)
            return outcome == OutcomeAbsent ? OutcomeFailed : outcome;

         if (window.empty() || windowTotal != total)
         {
            SetProblem_(WideOfUtf8(key) + _T(": the object changed while it was read"));
            return OutcomeFailed;
         }

         bytes.append(window);
      }

      return OutcomeDone;
   }

   MessageStoreObjectClient::Outcome
   MessageStoreObjectClient::RangeAnswer_(const Builder &build, const std::string &what, unsigned __int64 offset, unsigned __int64 length,
                                          std::string &bytes, unsigned __int64 &total, std::string &etag)
   {
      bytes.clear();
      total = 0;
      etag.clear();

      const size_t expected = (size_t) length;
      Builder sized = [build, expected](Request &request)
      {
         if (!build(request))
            return false;
         request.max_response = expected + 65536;
         request.transfer_bytes = expected;
         return true;
      };

      Answer answer;
      const Outcome outcome = Send_(sized, what, answer);
      if (outcome == OutcomeAbsent)
         return Absent_();

      const std::string range = HeaderOf_(answer.headers, "Content-Range");
      const size_t slash = range.rfind('/');

      if (outcome == OutcomeFailed && answer.status == 416)
      {
         // At or past the end: nothing, and the size from bytes */total.
         total = slash == std::string::npos ? 0 : DecimalValueOf(range.substr(slash + 1));
         return OutcomeDone;
      }

      if (outcome != OutcomeDone)
         return outcome;

      etag = Unquoted(HeaderOf_(answer.headers, "ETag"));

      if (answer.status == 206)
      {
         bytes.swap(answer.body);
         total = slash != std::string::npos ? DecimalValueOf(range.substr(slash + 1)) : offset + (unsigned __int64) bytes.size();
         return OutcomeDone;
      }

      // A 200: the whole object, the range not honoured. The window is cut
      // from it, and the whole is the total.
      total = (unsigned __int64) answer.body.size();
      if (offset < total)
      {
         const unsigned __int64 left = total - offset;
         bytes = answer.body.substr((size_t) offset, (size_t) (length < left ? length : left));
      }
      return OutcomeDone;
   }

   String
   MessageStoreObjectClient::LastProblem()
   {
      return last_problem;
   }

   void
   MessageStoreObjectClient::SetProblem_(const String &problem)
   {
      last_problem = problem;
   }

   std::string
   MessageStoreObjectClient::ExpectedS3Etag(const std::vector<std::string> &partMd5s, const std::string &wholeMd5)
   {
      if (partMd5s.size() <= 1)
         return Sig::HexOf(wholeMd5);

      std::string joined;
      for (size_t i = 0; i < partMd5s.size(); i++)
         joined += partMd5s[i];

      return Sig::HexOf(Sig::Md5Of(joined.data(), joined.size())) + "-" + DecimalOf((unsigned __int64) partMd5s.size());
   }

   std::string
   MessageStoreObjectClient::HeaderOf_(const std::string &headers, const char *name)
   {
      const size_t nameLength = strlen(name);
      size_t position = 0;
      while (position < headers.size())
      {
         size_t end = headers.find("\r\n", position);
         if (end == std::string::npos)
            end = headers.size();

         const std::string line = headers.substr(position, end - position);
         position = end + 2;

         const size_t colon = line.find(':');
         if (colon != nameLength)
            continue;

         bool same = true;
         for (size_t i = 0; i < nameLength && same; i++)
         {
            char a = line[i];
            char b = name[i];
            if (a >= 'A' && a <= 'Z')
               a = (char) (a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z')
               b = (char) (b - 'A' + 'a');
            same = a == b;
         }

         if (same)
            return TrimmedText(line.substr(colon + 1));
      }

      return std::string();
   }

   std::string
   MessageStoreObjectClient::XmlUnescape_(const std::string &text)
   {
      std::string out;
      for (size_t i = 0; i < text.size(); i++)
      {
         if (text[i] != '&')
         {
            out += text[i];
            continue;
         }

         const size_t end = text.find(';', i);
         if (end == std::string::npos)
         {
            out += text[i];
            continue;
         }

         const std::string entity = text.substr(i + 1, end - i - 1);
         if (entity == "amp")
            out += '&';
         else if (entity == "lt")
            out += '<';
         else if (entity == "gt")
            out += '>';
         else if (entity == "quot")
            out += '"';
         else if (entity == "apos")
            out += '\'';
         else if (entity.size() > 1 && entity[0] == '#')
         {
            const unsigned long code = entity[1] == 'x' || entity[1] == 'X' ? strtoul(entity.c_str() + 2, nullptr, 16) : strtoul(entity.c_str() + 1, nullptr, 10);

            // As UTF-8.
            if (code < 0x80)
               out += (char) code;
            else if (code < 0x800)
            {
               out += (char) (0xC0 | (code >> 6));
               out += (char) (0x80 | (code & 0x3F));
            }
            else if (code < 0x10000)
            {
               out += (char) (0xE0 | (code >> 12));
               out += (char) (0x80 | ((code >> 6) & 0x3F));
               out += (char) (0x80 | (code & 0x3F));
            }
            else
            {
               out += (char) (0xF0 | (code >> 18));
               out += (char) (0x80 | ((code >> 12) & 0x3F));
               out += (char) (0x80 | ((code >> 6) & 0x3F));
               out += (char) (0x80 | (code & 0x3F));
            }
         }
         else
         {
            out += text.substr(i, end - i + 1);
         }

         i = end;
      }
      return out;
   }

   void
   MessageStoreObjectClient::XmlValues_(const std::string &xml, const char *element, std::vector<std::string> &values)
   {
      values.clear();

      const std::string open = std::string("<") + element + ">";
      const std::string close = std::string("</") + element + ">";

      size_t position = 0;
      for (;;)
      {
         const size_t start = xml.find(open, position);
         if (start == std::string::npos)
            break;

         const size_t end = xml.find(close, start + open.size());
         if (end == std::string::npos)
            break;

         values.push_back(xml.substr(start + open.size(), end - start - open.size()));
         position = end + close.size();
      }
   }

   bool
   MessageStoreObjectClient::ConfirmBucket_()
   {
      if (NowMs() < confirmed_until_.load())
         return true;

      if (Probe() != OutcomeDone)
         return false;

      confirmed_until_ = NowMs() + ConfirmedMilliseconds;
      return true;
   }

   MessageStoreObjectClient::Outcome
   MessageStoreObjectClient::Absent_()
   {
      // A 404 is an object that is not there only while the bucket is: S3
      // answers a HEAD in a bucket that does not exist with the same 404 as a
      // HEAD of a key that does not, and a message taken for missing on that
      // answer would be replaced by a placeholder.
      return ConfirmBucket_() ? OutcomeAbsent : OutcomeUnavailable;
   }

   MessageStoreObjectClient::Outcome
   MessageStoreObjectClient::NoBucket_(const std::string &bucket)
   {
      // A put answered 404, or the bucket's own HEAD did: the bucket or the
      // container is not there. Not an object to be taken for missing, and not
      // something a retry mends - every message waits in the queue until the
      // administrator puts it back or names the right one.
      const String said = _T("there is no bucket or container ") + WideOfUtf8(bucket) + _T(" at the endpoint (404)");
      paused_until_ = NowMs() + PauseMilliseconds;
      SetProblem_(said);
      if (ReportDue(last_failed_report))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::High, 7474, "MessageStoreObjectClient",
            "The object store refused a request: " + said + ". Messages are not lost meanwhile: a delivery waits in the queue, and IMAP and POP3 are told to try again. Said once a minute at most.");
      }
      return OutcomeUnavailable;
   }

   MessageStoreObjectClient::Outcome
   MessageStoreObjectClient::Send_(const Builder &build, const std::string &what, Answer &answer)
   {
      answer = Answer();

      if (NowMs() < paused_until_.load())
      {
         SetProblem_(WideOfUtf8(what) + _T(": the object store did not answer a moment ago, and is left alone for ten seconds before it is asked again"));
         return OutcomeUnavailable;
      }

      String problem;
      for (int attempt = 0; attempt < 3; attempt++)
      {
         if (attempt > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(attempt == 1 ? 200 : 800));

         Request request;
         if (!build(request))
         {
            SetProblem_(WideOfUtf8(what) + _T(": the request could not be made from the object store's settings"));
            return OutcomeFailed;
         }

         // HttpsClient asks for JSON unless told otherwise; a store answers
         // XML or bytes. Neither signature covers Accept.
         request.headers.push_back(AnsiString("Accept: */*"));

         const size_t moved = request.transfer_bytes > (size_t) request.body.GetLength() ? request.transfer_bytes : (size_t) request.body.GetLength();

         HttpsClient::Response response;
         String error;
         if (!HttpsClient::RequestTrusting(request.method, request.url, request.headers, request.content_type, request.body, response, error,
                                           TimeoutFor(moved), request.max_response, ca_file_))
         {
            problem = WideOfUtf8(what) + _T(": ") + error;
            continue;
         }

         answer.status = response.status_code;
         answer.headers.assign(response.headers.c_str(), (size_t) response.headers.GetLength());
         answer.body.assign(response.body.data(), response.body.size());

         if (answer.status >= 200 && answer.status < 300)
         {
            // A connection that ended early leaves a body shorter than the
            // length the store gave: tried again, and never taken for the
            // object. A HEAD's length is the object's, with no body.
            const std::string length = HeaderOf_(answer.headers, "Content-Length");
            if (std::string(request.method.c_str()) != "HEAD" && !length.empty() &&
                DecimalValueOf(length) != (unsigned __int64) answer.body.size())
            {
               problem = WideOfUtf8(what) + _T(": the answer ended before the length the store gave");
               continue;
            }

            return OutcomeDone;
         }

         if (answer.status == 404)
         {
            SetProblem_(WideOfUtf8(what) + _T(": not found (404)"));
            return OutcomeAbsent;
         }

         // A range past the end - the first bytes of an empty object - is the
         // reader's to answer (Read), and nothing to report; so is an If-Match
         // that did not hold - the object rewritten in the middle of a read.
         if (answer.status == 416)
         {
            SetProblem_(WideOfUtf8(what) + _T(": the range is past the end (416)"));
            return OutcomeFailed;
         }

         if (answer.status == 412)
         {
            SetProblem_(WideOfUtf8(what) + _T(": the object changed while it was read (412)"));
            return OutcomeFailed;
         }

         // The store's own word for it, when it gave one: S3's <Code>, Azure's
         // x-ms-error-code.
         std::vector<std::string> codes;
         XmlValues_(answer.body, "Code", codes);
         std::string code = codes.empty() ? HeaderOf_(answer.headers, "x-ms-error-code") : codes[0];
         const String said = Formatter::Format("{0}: HTTP {1}{2}", WideOfUtf8(what), answer.status,
                                              code.empty() ? String() : String(_T(" ")) + WideOfUtf8(code));

         if (answer.status == 401 || answer.status == 403)
         {
            // Not something a retry mends, and not an absent object either: the
            // store is refusing this server, which is the administrator's to put
            // right. Every message waits meanwhile.
            paused_until_ = NowMs() + PauseMilliseconds;
            SetProblem_(said);
            if (ReportDue(last_refused_report))
            {
               ErrorManager::Instance()->ReportError(ErrorManager::High, 7471, "MessageStoreObjectClient",
                  "The object store refused this server's credentials: " + said + ". Check the access key and secret (or the account key, or the SAS and its permissions and expiry) in the message store's settings. Messages are not lost meanwhile: a delivery waits in the queue, and IMAP and POP3 are told to try again. Said once a minute at most.");
            }
            return OutcomeUnavailable;
         }

         if (answer.status == 408 || answer.status == 429 || answer.status >= 500)
         {
            problem = said;
            continue;
         }

         SetProblem_(said);
         if (ReportDue(last_failed_report))
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7474, "MessageStoreObjectClient",
               "The object store refused a request: " + said + ". Said once a minute at most.");
         }
         return OutcomeFailed;
      }

      // Three tries, none answered: the store is left alone for a while.
      paused_until_ = NowMs() + PauseMilliseconds;
      SetProblem_(problem);
      if (ReportDue(last_unreachable_report))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::High, 7470, "MessageStoreObjectClient",
            "The object store could not be reached: " + problem + ". Messages are not lost meanwhile: a delivery waits in the queue and is tried again, IMAP answers NO [UNAVAILABLE] and POP3 -ERR [SYS/TEMP]. Said once a minute at most.");
      }
      return OutcomeUnavailable;
   }

   namespace
   {
      // ---- S3 -------------------------------------------------------------------

      class S3ObjectClient : public MessageStoreObjectClient
      {
      public:

         bool Configure(const Settings &settings, String &problem)
         {
            settings_ = settings;
            ca_file_ = settings.ca_file;

            region_ = settings.region.empty() ? std::string("us-east-1") : settings.region;

            std::string endpoint = settings.endpoint.empty() ? "https://s3." + region_ + ".amazonaws.com" : settings.endpoint;
            while (endpoint.size() > 1 && endpoint[endpoint.size() - 1] == '/')
               endpoint.erase(endpoint.size() - 1);

            bool https = false;
            AnsiString host;
            AnsiString port;
            AnsiString path;
            if (!HttpsClient::ParseUrl(AnsiString(endpoint.c_str()), https, host, port, path))
            {
               problem = _T("the endpoint is not an https URL with a host");
               return false;
            }

            https_ = https;
            host_ = host.c_str();
            port_ = port.c_str();
            base_path_ = path.c_str();
            while (!base_path_.empty() && base_path_[base_path_.size() - 1] == '/')
               base_path_.erase(base_path_.size() - 1);

            if (settings.bucket.empty())
            {
               problem = _T("no bucket is named");
               return false;
            }

            if (settings.access_key.empty() || settings.secret.empty())
            {
               problem = _T("the access key id and the secret access key are both needed");
               return false;
            }

            if (!PlainToken(endpoint, false) || !PlainToken(settings.bucket, false) || !PlainToken(region_, false) ||
                !PlainToken(settings.access_key, false))
            {
               problem = _T("the endpoint, the bucket, the region and the access key id may hold printable ASCII only - no space, no control character, no '#'");
               return false;
            }

            // Virtual-hosted for AWS's own endpoint, where path-style is on its
            // way out; path-style for a named endpoint - MinIO, Ceph - unless
            // asked for otherwise.
            virtual_ = settings.addressing == "virtual" || (settings.addressing != "path" && settings.endpoint.empty());

            if (virtual_ && https_ && settings.bucket.find('.') != std::string::npos)
            {
               problem = _T("a bucket whose name has a dot cannot be reached virtual-hosted over https - the certificate's wildcard covers one label; set the addressing to path");
               return false;
            }

            return true;
         }

         Outcome Head(const std::string &key, Properties &properties) override
         {
            properties = Properties();

            Answer answer;
            const Outcome outcome = Send_(Builder_("HEAD", ObjectPath_(key), Sig::Pairs(), Sig::Pairs(), std::string(), std::string()), "HEAD " + key, answer);
            if (outcome == OutcomeAbsent)
               return Absent_();
            if (outcome != OutcomeDone)
               return outcome;

            properties.size = DecimalValueOf(HeaderOf_(answer.headers, "Content-Length"));
            properties.etag = Unquoted(HeaderOf_(answer.headers, "ETag"));
            return OutcomeDone;
         }

         Outcome ReadRange_(const std::string &key, unsigned __int64 offset, unsigned __int64 length, const std::string &ifMatch,
                            std::string &bytes, unsigned __int64 &total, std::string &etag) override
         {
            Sig::Pairs unsignedHeaders;
            unsignedHeaders.push_back(std::make_pair(std::string("Range"), "bytes=" + DecimalOf(offset) + "-" + DecimalOf(offset + length - 1)));
            if (!ifMatch.empty())
               unsignedHeaders.push_back(std::make_pair(std::string("If-Match"), "\"" + ifMatch + "\""));

            Builder build = Builder_("GET", ObjectPath_(key), Sig::Pairs(), Sig::Pairs(), std::string(), std::string(), unsignedHeaders);
            return RangeAnswer_(build, "GET " + key, offset, length, bytes, total, etag);
         }

         Outcome Put(const std::string &key, unsigned __int64 size, const Source &source, std::string &md5) override
         {
            md5.clear();

            if (size <= (unsigned __int64) PartBytes)
            {
               std::string bytes;
               if (size > 0 && !source(0, (size_t) size, bytes))
               {
                  SetProblem_(_T("what was to be put could not be read"));
                  return OutcomeFailed;
               }

               md5 = Sig::Md5Of(bytes.data(), bytes.size());

               Answer answer;
               const Outcome outcome = Send_(Builder_("PUT", ObjectPath_(key), Sig::Pairs(), Sig::Pairs(), bytes, "application/octet-stream"), "PUT " + key, answer);
               return outcome == OutcomeAbsent ? NoBucket_(settings_.bucket) : outcome;
            }

            // A multipart upload, aborted when it cannot be finished so that
            // nothing of it is kept.
            Sig::Pairs uploads;
            uploads.push_back(std::make_pair(std::string("uploads"), std::string()));

            Answer answer;
            Outcome outcome = Send_(Builder_("POST", ObjectPath_(key), uploads, Sig::Pairs(), std::string(), std::string()), "POST " + key + "?uploads", answer);
            if (outcome != OutcomeDone)
               return outcome == OutcomeAbsent ? NoBucket_(settings_.bucket) : outcome;

            std::vector<std::string> ids;
            XmlValues_(answer.body, "UploadId", ids);
            if (ids.empty() || ids[0].empty())
            {
               SetProblem_(_T("the store began a multipart upload without saying which"));
               return OutcomeFailed;
            }
            const std::string uploadId = XmlUnescape_(ids[0]);

            RunningMd5 whole;
            std::vector<std::string> partMd5s;
            std::string completion = "<CompleteMultipartUpload>";
            int part = 1;
            for (unsigned __int64 offset = 0; offset < size; offset += (unsigned __int64) PartBytes, part++)
            {
               const size_t length = (size_t) (size - offset < (unsigned __int64) PartBytes ? size - offset : (unsigned __int64) PartBytes);

               std::string bytes;
               if (!source(offset, length, bytes) || bytes.size() != length)
               {
                  SetProblem_(_T("what was to be put could not be read"));
                  Abort_(key, uploadId);
                  return OutcomeFailed;
               }
               whole.Add(bytes);
               partMd5s.push_back(Sig::Md5Of(bytes.data(), bytes.size()));

               Sig::Pairs query;
               query.push_back(std::make_pair(std::string("partNumber"), DecimalOf((unsigned __int64) part)));
               query.push_back(std::make_pair(std::string("uploadId"), uploadId));

               outcome = Send_(Builder_("PUT", ObjectPath_(key), query, Sig::Pairs(), bytes, "application/octet-stream"), "PUT " + key + " part " + DecimalOf((unsigned __int64) part), answer);
               if (outcome != OutcomeDone)
               {
                  Abort_(key, uploadId);
                  return outcome == OutcomeAbsent ? OutcomeFailed : outcome;
               }

               completion += "<Part><PartNumber>" + DecimalOf((unsigned __int64) part) + "</PartNumber><ETag>" + HeaderOf_(answer.headers, "ETag") + "</ETag></Part>";
            }
            completion += "</CompleteMultipartUpload>";

            // The completion is small and its work is not: S3 assembles the
            // parts before it answers, so its deadline is the whole upload's.
            Sig::Pairs complete;
            complete.push_back(std::make_pair(std::string("uploadId"), uploadId));
            Builder completeBuild = Builder_("POST", ObjectPath_(key), complete, Sig::Pairs(), completion, "application/xml");
            const size_t completing = (size_t) size;
            Builder completeSized = [completeBuild, completing](Request &request)
            {
               if (!completeBuild(request))
                  return false;
               request.transfer_bytes = completing;
               return true;
            };
            outcome = Send_(completeSized, "POST " + key + "?uploadId", answer);

            // S3 can answer 200 and put the failure in the body.
            if (outcome == OutcomeDone && answer.body.find("<Error>") != std::string::npos)
            {
               SetProblem_(_T("the store answered the multipart upload's completion with an error"));
               outcome = OutcomeFailed;
            }

            md5 = whole.Finish();

            // NoSuchUpload to a completion tried again: the first try finished
            // the upload and its answer was lost. The object is taken for this
            // put when its ETag is the one these parts make.
            if (outcome == OutcomeAbsent)
            {
               Properties there;
               if (Head(key, there) == OutcomeDone && there.size == size && LowerAsciiOf(there.etag) == ExpectedS3Etag(partMd5s, md5))
                  return OutcomeDone;
            }

            if (outcome != OutcomeDone)
            {
               md5.clear();
               Abort_(key, uploadId);
               return outcome == OutcomeAbsent ? OutcomeFailed : outcome;
            }

            return OutcomeDone;
         }

         Outcome Copy(const std::string &from, const std::string &to) override
         {
            Sig::Pairs signedHeaders;
            signedHeaders.push_back(std::make_pair(std::string("x-amz-copy-source"), Sig::UriEncode("/" + settings_.bucket + "/" + from, false)));

            Answer answer;
            const Outcome outcome = Send_(Builder_("PUT", ObjectPath_(to), Sig::Pairs(), signedHeaders, std::string(), std::string()), "COPY " + from + " to " + to, answer);
            if (outcome == OutcomeAbsent)
               return Absent_();
            if (outcome != OutcomeDone)
               return outcome;

            // S3 can answer 200 and put the failure in the body.
            if (answer.body.find("<Error>") != std::string::npos)
            {
               SetProblem_(_T("the store answered the copy with an error"));
               return OutcomeFailed;
            }

            return OutcomeDone;
         }

         Outcome Remove(const std::string &key) override
         {
            Answer answer;
            const Outcome outcome = Send_(Builder_("DELETE", ObjectPath_(key), Sig::Pairs(), Sig::Pairs(), std::string(), std::string()), "DELETE " + key, answer);
            if (outcome == OutcomeAbsent)
               return Absent_() == OutcomeAbsent ? OutcomeDone : OutcomeUnavailable;
            return outcome;
         }

         Outcome List(const std::string &prefix, std::vector<Listed> &objects) override
         {
            objects.clear();

            std::string token;
            for (int page = 0; page < 100000; page++)
            {
               Sig::Pairs query;
               query.push_back(std::make_pair(std::string("list-type"), std::string("2")));
               query.push_back(std::make_pair(std::string("prefix"), prefix));
               if (!token.empty())
                  query.push_back(std::make_pair(std::string("continuation-token"), token));

               Builder build = Builder_("GET", BucketPath_(), query, Sig::Pairs(), std::string(), std::string());
               Builder sized = [build](Request &request)
               {
                  if (!build(request))
                     return false;
                  request.max_response = AnswerBytes;
                  return true;
               };

               Answer answer;
               const Outcome outcome = Send_(sized, "LIST " + prefix, answer);
               if (outcome != OutcomeDone)
                  return outcome == OutcomeAbsent ? OutcomeUnavailable : outcome;

               std::vector<std::string> contents;
               XmlValues_(answer.body, "Contents", contents);
               for (size_t i = 0; i < contents.size(); i++)
               {
                  std::vector<std::string> keys;
                  std::vector<std::string> sizes;
                  std::vector<std::string> modified;
                  XmlValues_(contents[i], "Key", keys);
                  XmlValues_(contents[i], "Size", sizes);
                  XmlValues_(contents[i], "LastModified", modified);
                  if (keys.empty())
                     continue;

                  Listed listed;
                  listed.key = XmlUnescape_(keys[0]);
                  listed.size = sizes.empty() ? 0 : DecimalValueOf(sizes[0]);
                  listed.modified = modified.empty() ? 0 : IsoTimeOf(modified[0]);
                  objects.push_back(listed);
               }

               std::vector<std::string> truncated;
               std::vector<std::string> next;
               XmlValues_(answer.body, "IsTruncated", truncated);
               XmlValues_(answer.body, "NextContinuationToken", next);
               if (truncated.empty() || TrimmedText(truncated[0]) != "true" || next.empty())
                  return OutcomeDone;

               token = XmlUnescape_(next[0]);
            }

            return OutcomeDone;
         }

         Outcome Probe() override
         {
            Answer answer;
            const Outcome outcome = Send_(Builder_("HEAD", BucketPath_(), Sig::Pairs(), Sig::Pairs(), std::string(), std::string()), "HEAD bucket " + settings_.bucket, answer);
            if (outcome == OutcomeAbsent)
               return NoBucket_(settings_.bucket);
            return outcome;
         }

      private:

         std::string Host_() const
         {
            return virtual_ ? settings_.bucket + "." + host_ : host_;
         }

         // host[:port] as the Host header carries it, which is what is signed.
         std::string Authority_() const
         {
            std::string authority = Host_();
            if (authority.find(':') != std::string::npos)
               authority = "[" + authority + "]";
            if (port_ != (https_ ? "443" : "80"))
               authority += ":" + port_;
            return authority;
         }

         std::string BucketPath_() const
         {
            return virtual_ ? base_path_ + "/" : base_path_ + "/" + settings_.bucket;
         }

         std::string ObjectPath_(const std::string &key) const
         {
            return virtual_ ? base_path_ + "/" + key : base_path_ + "/" + settings_.bucket + "/" + key;
         }

         void Abort_(const std::string &key, const std::string &uploadId)
         {
            // What the abort answers is not asked: a part it could not remove is
            // the store's to expire under the bucket's lifecycle rule
            // (docs/ObjectStorage.md). The failure being reported is the put's.
            const String kept = LastProblem();

            Sig::Pairs query;
            query.push_back(std::make_pair(std::string("uploadId"), uploadId));
            Answer answer;
            Send_(Builder_("DELETE", ObjectPath_(key), query, Sig::Pairs(), std::string(), std::string()), "DELETE " + key + "?uploadId", answer);

            SetProblem_(kept);
         }

         // A request, signed afresh for each try: x-amz-date is the moment of
         // the try.
         Builder Builder_(const std::string &method, const std::string &path, const Sig::Pairs &query, const Sig::Pairs &signedHeaders,
                          const std::string &body, const std::string &contentType, const Sig::Pairs &unsignedHeaders = Sig::Pairs()) const
         {
            const std::string authority = Authority_();
            const std::string scheme = https_ ? "https://" : "http://";
            const std::string region = region_;
            const std::string accessKey = settings_.access_key;
            const std::string secret = settings_.secret;
            const std::string payloadHash = Sig::Sha256HexOf(body);

            return [=](Request &request)
            {
               const std::string amzDate = Sig::AmzDate(time(nullptr));

               Sig::V4Request v4;
               v4.method = method;
               v4.path = path;
               v4.query = query;
               v4.payload_hash = payloadHash;
               v4.headers.push_back(std::make_pair(std::string("host"), authority));
               v4.headers.push_back(std::make_pair(std::string("x-amz-content-sha256"), payloadHash));
               v4.headers.push_back(std::make_pair(std::string("x-amz-date"), amzDate));
               for (size_t i = 0; i < signedHeaders.size(); i++)
                  v4.headers.push_back(signedHeaders[i]);

               const Sig::V4Signed signature = Sig::SignV4(v4, amzDate, region, "s3", accessKey, secret);

               std::string url = scheme + authority + Sig::UriEncode(path, false);
               if (!query.empty())
                  url += "?" + QueryString(query);

               request.method = method.c_str();
               request.url = url.c_str();
               request.headers.clear();
               request.headers.push_back(AnsiString(("x-amz-content-sha256: " + payloadHash).c_str()));
               request.headers.push_back(AnsiString(("x-amz-date: " + amzDate).c_str()));
               for (size_t i = 0; i < signedHeaders.size(); i++)
                  request.headers.push_back(AnsiString((signedHeaders[i].first + ": " + signedHeaders[i].second).c_str()));
               for (size_t i = 0; i < unsignedHeaders.size(); i++)
                  request.headers.push_back(AnsiString((unsignedHeaders[i].first + ": " + unsignedHeaders[i].second).c_str()));
               request.headers.push_back(AnsiString(("Authorization: " + signature.authorization).c_str()));

               // HttpsClient frames a body; a PUT without one still says how
               // long it is, which S3 requires.
               if (body.empty() && method == "PUT")
                  request.headers.push_back(AnsiString("Content-Length: 0"));

               request.content_type = body.empty() ? AnsiString() : AnsiString(contentType.c_str());
               request.body = AnsiString(body.data(), body.size());
               return true;
            };
         }

         Settings settings_;
         std::string region_;
         bool https_ = true;
         std::string host_;
         std::string port_;
         std::string base_path_;
         bool virtual_ = false;
      };

      // ---- Azure Blob Storage ---------------------------------------------------

      class AzureBlobObjectClient : public MessageStoreObjectClient
      {
      public:

         bool Configure(const Settings &settings, String &problem)
         {
            settings_ = settings;
            ca_file_ = settings.ca_file;

            if (settings.access_key.empty())
            {
               problem = _T("the storage account's name is needed");
               return false;
            }

            if (settings.bucket.empty())
            {
               problem = _T("no container is named");
               return false;
            }

            sas_ = settings.sas;
            while (!sas_.empty() && sas_[0] == '?')
               sas_.erase(0, 1);

            if (!PlainToken(sas_, false) || !PlainToken(settings.access_key, false) || !PlainToken(settings.bucket, false) ||
                !PlainToken(settings.endpoint, false))
            {
               problem = _T("the endpoint, the account, the container and the SAS may hold printable ASCII only - no space, no control character, no '#'");
               return false;
            }

            if (sas_.empty())
            {
               std::string key;
               if (settings.secret.empty() || !Sig::FromBase64(settings.secret, key))
               {
                  problem = _T("the account key is needed, and is base64 - or a SAS in its place");
                  return false;
               }
            }

            std::string endpoint = settings.endpoint.empty() ? "https://" + settings.access_key + ".blob.core.windows.net" : settings.endpoint;
            while (endpoint.size() > 1 && endpoint[endpoint.size() - 1] == '/')
               endpoint.erase(endpoint.size() - 1);

            bool https = false;
            AnsiString host;
            AnsiString port;
            AnsiString path;
            if (!HttpsClient::ParseUrl(AnsiString(endpoint.c_str()), https, host, port, path))
            {
               problem = _T("the endpoint is not an https URL with a host");
               return false;
            }

            https_ = https;
            host_ = host.c_str();
            port_ = port.c_str();

            // The emulator's account is in the path (https://host:10000/devstoreaccount1).
            base_path_ = path.c_str();
            while (!base_path_.empty() && base_path_[base_path_.size() - 1] == '/')
               base_path_.erase(base_path_.size() - 1);

            return true;
         }

         Outcome Head(const std::string &key, Properties &properties) override
         {
            properties = Properties();

            Answer answer;
            const Outcome outcome = Send_(Builder_("HEAD", BlobPath_(key), Sig::Pairs(), Sig::Pairs(), std::string(), std::string()), "HEAD " + key, answer);
            if (outcome == OutcomeAbsent)
               return Absent_();
            if (outcome != OutcomeDone)
               return outcome;

            properties.size = DecimalValueOf(HeaderOf_(answer.headers, "Content-Length"));
            properties.etag = Unquoted(HeaderOf_(answer.headers, "ETag"));

            std::string md5;
            if (Sig::FromBase64(HeaderOf_(answer.headers, "Content-MD5"), md5) && md5.size() == 16)
               properties.content_md5 = md5;

            return OutcomeDone;
         }

         Outcome ReadRange_(const std::string &key, unsigned __int64 offset, unsigned __int64 length, const std::string &ifMatch,
                            std::string &bytes, unsigned __int64 &total, std::string &etag) override
         {
            // If-Match is one of the standard headers Shared Key signs
            // (ObjectStoreSignature::SharedKeyStringToSign).
            Sig::Pairs headers;
            headers.push_back(std::make_pair(std::string("x-ms-range"), "bytes=" + DecimalOf(offset) + "-" + DecimalOf(offset + length - 1)));
            if (!ifMatch.empty())
               headers.push_back(std::make_pair(std::string("If-Match"), "\"" + ifMatch + "\""));

            Builder build = Builder_("GET", BlobPath_(key), Sig::Pairs(), headers, std::string(), std::string());
            return RangeAnswer_(build, "GET " + key, offset, length, bytes, total, etag);
         }

         Outcome Put(const std::string &key, unsigned __int64 size, const Source &source, std::string &md5) override
         {
            md5.clear();

            if (size <= (unsigned __int64) PartBytes)
            {
               std::string bytes;
               if (size > 0 && !source(0, (size_t) size, bytes))
               {
                  SetProblem_(_T("what was to be put could not be read"));
                  return OutcomeFailed;
               }

               md5 = Sig::Md5Of(bytes.data(), bytes.size());

               Sig::Pairs headers;
               headers.push_back(std::make_pair(std::string("x-ms-blob-type"), std::string("BlockBlob")));
               headers.push_back(std::make_pair(std::string("x-ms-blob-content-md5"), Sig::Base64Of(md5)));

               Answer answer;
               const Outcome outcome = Send_(Builder_("PUT", BlobPath_(key), Sig::Pairs(), headers, bytes, "application/octet-stream"), "PUT " + key, answer);
               return outcome == OutcomeAbsent ? NoBucket_(settings_.bucket) : outcome;
            }

            // Blocks, then the list that commits them. Blocks never committed
            // are Azure's to expire (a week); nothing is under the name until
            // the list is put.
            RunningMd5 whole;
            std::string list = "<?xml version=\"1.0\" encoding=\"utf-8\"?><BlockList>";
            int block = 0;
            for (unsigned __int64 offset = 0; offset < size; offset += (unsigned __int64) PartBytes, block++)
            {
               const size_t length = (size_t) (size - offset < (unsigned __int64) PartBytes ? size - offset : (unsigned __int64) PartBytes);

               std::string bytes;
               if (!source(offset, length, bytes) || bytes.size() != length)
               {
                  SetProblem_(_T("what was to be put could not be read"));
                  return OutcomeFailed;
               }
               whole.Add(bytes);

               // Every id of a blob the same length.
               char id[32];
               snprintf(id, sizeof(id), "hmsblock-%06d", block);
               const std::string blockId = Sig::Base64Of(std::string(id));

               Sig::Pairs query;
               query.push_back(std::make_pair(std::string("comp"), std::string("block")));
               query.push_back(std::make_pair(std::string("blockid"), blockId));

               Answer answer;
               const Outcome outcome = Send_(Builder_("PUT", BlobPath_(key), query, Sig::Pairs(), bytes, "application/octet-stream"), "PUT " + key + " block " + DecimalOf((unsigned __int64) block), answer);
               if (outcome != OutcomeDone)
                  return outcome == OutcomeAbsent ? NoBucket_(settings_.bucket) : outcome;

               list += "<Latest>" + blockId + "</Latest>";
            }
            list += "</BlockList>";

            md5 = whole.Finish();

            Sig::Pairs query;
            query.push_back(std::make_pair(std::string("comp"), std::string("blocklist")));
            Sig::Pairs headers;
            headers.push_back(std::make_pair(std::string("x-ms-blob-content-md5"), Sig::Base64Of(md5)));

            Answer answer;
            const Outcome outcome = Send_(Builder_("PUT", BlobPath_(key), query, headers, list, "application/xml"), "PUT " + key + "?comp=blocklist", answer);
            return outcome == OutcomeAbsent ? NoBucket_(settings_.bucket) : outcome;
         }

         Outcome Copy(const std::string &from, const std::string &to) override
         {
            // The store's own copy only onto a name that holds nothing: a Copy
            // Blob that ends pending or failed leaves its destination empty, so
            // one onto a name that holds an object - or one the store could not
            // say about - is read and put again instead, and Put Blob and Put
            // Block List replace a blob whole or not at all. And always with a
            // SAS, whose source would need a signature of its own.
            bool storeCopy = sas_.empty();
            if (storeCopy)
            {
               Properties there;
               const Outcome existing = Head(to, there);
               if (existing == OutcomeUnavailable)
                  return existing;
               storeCopy = existing == OutcomeAbsent;
            }

            if (!storeCopy)
            {
               std::string bytes;
               unsigned __int64 total = 0;
               Outcome outcome = Read(from, 0, 0, bytes, total);
               if (outcome != OutcomeDone)
                  return outcome;

               std::string md5;
               const Source source = [&bytes](unsigned __int64 offset, size_t length, std::string &part)
               {
                  if (offset > (unsigned __int64) bytes.size() || (unsigned __int64) length > (unsigned __int64) bytes.size() - offset)
                     return false;
                  part.assign(bytes.data() + (size_t) offset, length);
                  return true;
               };
               return Put(to, (unsigned __int64) bytes.size(), source, md5);
            }

            Sig::Pairs headers;
            headers.push_back(std::make_pair(std::string("x-ms-copy-source"), Scheme_() + Authority_() + Sig::UriEncode(BlobPath_(from), false)));

            Answer answer;
            Outcome outcome = Send_(Builder_("PUT", BlobPath_(to), Sig::Pairs(), headers, std::string(), std::string()), "COPY " + from + " to " + to, answer);
            if (outcome == OutcomeAbsent)
               return Absent_();
            if (outcome != OutcomeDone)
               return outcome;

            // Within one account a copy is done by the time it is answered;
            // when it is not, it is waited for - five seconds at most - and a
            // copy that has not finished by then is aborted, and the name,
            // which held nothing before it, emptied again.
            const std::string copyId = HeaderOf_(answer.headers, "x-ms-copy-id");
            std::string status = HeaderOf_(answer.headers, "x-ms-copy-status");
            for (int poll = 0; poll < 20 && status == "pending"; poll++)
            {
               std::this_thread::sleep_for(std::chrono::milliseconds(250));
               outcome = Send_(Builder_("HEAD", BlobPath_(to), Sig::Pairs(), Sig::Pairs(), std::string(), std::string()), "HEAD " + to, answer);
               if (outcome != OutcomeDone)
                  return outcome == OutcomeAbsent ? OutcomeFailed : outcome;
               status = HeaderOf_(answer.headers, "x-ms-copy-status");
            }

            if (status == "success" || status.empty())
               return OutcomeDone;

            const String unfinished = _T("the store's copy did not finish: ") + WideOfUtf8(status);
            if (status == "pending" && !copyId.empty())
            {
               Sig::Pairs abortQuery;
               abortQuery.push_back(std::make_pair(std::string("comp"), std::string("copy")));
               abortQuery.push_back(std::make_pair(std::string("copyid"), copyId));
               Sig::Pairs abortHeaders;
               abortHeaders.push_back(std::make_pair(std::string("x-ms-copy-action"), std::string("abort")));
               Answer aborted;
               Send_(Builder_("PUT", BlobPath_(to), abortQuery, abortHeaders, std::string(), std::string()), "ABORT COPY " + to, aborted);
            }
            Remove(to);
            SetProblem_(unfinished);
            return OutcomeFailed;
         }

         Outcome Remove(const std::string &key) override
         {
            Answer answer;
            const Outcome outcome = Send_(Builder_("DELETE", BlobPath_(key), Sig::Pairs(), Sig::Pairs(), std::string(), std::string()), "DELETE " + key, answer);
            if (outcome == OutcomeAbsent)
               return Absent_() == OutcomeAbsent ? OutcomeDone : OutcomeUnavailable;
            return outcome;
         }

         Outcome List(const std::string &prefix, std::vector<Listed> &objects) override
         {
            objects.clear();

            std::string marker;
            for (int page = 0; page < 100000; page++)
            {
               Sig::Pairs query;
               query.push_back(std::make_pair(std::string("restype"), std::string("container")));
               query.push_back(std::make_pair(std::string("comp"), std::string("list")));
               query.push_back(std::make_pair(std::string("prefix"), prefix));
               if (!marker.empty())
                  query.push_back(std::make_pair(std::string("marker"), marker));

               Builder build = Builder_("GET", ContainerPath_(), query, Sig::Pairs(), std::string(), std::string());
               Builder sized = [build](Request &request)
               {
                  if (!build(request))
                     return false;
                  request.max_response = AnswerBytes;
                  return true;
               };

               Answer answer;
               const Outcome outcome = Send_(sized, "LIST " + prefix, answer);
               if (outcome != OutcomeDone)
                  return outcome == OutcomeAbsent ? OutcomeUnavailable : outcome;

               std::vector<std::string> blobs;
               XmlValues_(answer.body, "Blob", blobs);
               for (size_t i = 0; i < blobs.size(); i++)
               {
                  std::vector<std::string> names;
                  std::vector<std::string> sizes;
                  std::vector<std::string> modified;
                  XmlValues_(blobs[i], "Name", names);
                  XmlValues_(blobs[i], "Content-Length", sizes);
                  XmlValues_(blobs[i], "Last-Modified", modified);
                  if (names.empty())
                     continue;

                  Listed listed;
                  listed.key = XmlUnescape_(names[0]);
                  listed.size = sizes.empty() ? 0 : DecimalValueOf(sizes[0]);
                  listed.modified = modified.empty() ? 0 : Rfc1123TimeOf(TrimmedText(modified[0]));
                  objects.push_back(listed);
               }

               std::vector<std::string> next;
               XmlValues_(answer.body, "NextMarker", next);
               if (next.empty() || TrimmedText(next[0]).empty())
                  return OutcomeDone;

               marker = XmlUnescape_(TrimmedText(next[0]));
            }

            return OutcomeDone;
         }

         Outcome Probe() override
         {
            Sig::Pairs query;
            query.push_back(std::make_pair(std::string("restype"), std::string("container")));

            Answer answer;
            const Outcome outcome = Send_(Builder_("HEAD", ContainerPath_(), query, Sig::Pairs(), std::string(), std::string()), "HEAD container " + settings_.bucket, answer);
            if (outcome == OutcomeAbsent)
               return NoBucket_(settings_.bucket);
            return outcome;
         }

      private:

         std::string Scheme_() const
         {
            return https_ ? "https://" : "http://";
         }

         std::string Authority_() const
         {
            std::string authority = host_;
            if (authority.find(':') != std::string::npos)
               authority = "[" + authority + "]";
            if (port_ != (https_ ? "443" : "80"))
               authority += ":" + port_;
            return authority;
         }

         std::string ContainerPath_() const
         {
            return base_path_ + "/" + settings_.bucket;
         }

         std::string BlobPath_(const std::string &key) const
         {
            return base_path_ + "/" + settings_.bucket + "/" + key;
         }

         // A request, dated and signed afresh for each try - or carrying the
         // SAS, which needs no signature of this server's.
         Builder Builder_(const std::string &method, const std::string &path, const Sig::Pairs &query, const Sig::Pairs &msHeaders,
                          const std::string &body, const std::string &contentType) const
         {
            const std::string url = Scheme_() + Authority_() + Sig::UriEncode(path, false);
            const std::string sas = sas_;
            const std::string account = settings_.access_key;
            const std::string key = settings_.secret;

            return [=](Request &request)
            {
               Sig::Pairs headers;
               headers.push_back(std::make_pair(std::string("x-ms-date"), Sig::Rfc1123Date(time(nullptr))));
               headers.push_back(std::make_pair(std::string("x-ms-version"), std::string(AzureVersion)));
               for (size_t i = 0; i < msHeaders.size(); i++)
                  headers.push_back(msHeaders[i]);

               std::string target = url;
               if (!query.empty())
                  target += "?" + QueryString(query);
               if (!sas.empty())
                  target += (query.empty() ? "?" : "&") + sas;

               request.method = method.c_str();
               request.url = target.c_str();
               request.headers.clear();
               for (size_t i = 0; i < headers.size(); i++)
                  request.headers.push_back(AnsiString((headers[i].first + ": " + headers[i].second).c_str()));

               if (sas.empty())
               {
                  Sig::SharedKeyRequest signing;
                  signing.method = method;
                  signing.path = path;
                  signing.query = query;
                  signing.headers = headers;
                  signing.content_length = (unsigned long long) body.size();
                  signing.content_type = body.empty() ? std::string() : contentType;

                  std::string authorization;
                  if (!Sig::SignSharedKey(signing, account, key, authorization))
                     return false;
                  request.headers.push_back(AnsiString(("Authorization: " + authorization).c_str()));
               }

               if (body.empty() && method == "PUT")
                  request.headers.push_back(AnsiString("Content-Length: 0"));

               request.content_type = body.empty() ? AnsiString() : AnsiString(contentType.c_str());
               request.body = AnsiString(body.data(), body.size());
               return true;
            };
         }

         Settings settings_;
         std::string sas_;
         bool https_ = true;
         std::string host_;
         std::string port_;
         std::string base_path_;
      };
   }

   std::shared_ptr<MessageStoreObjectClient>
   MessageStoreObjectClient::Create(const Settings &settings, String &problem)
   {
      if (settings.service == ServiceS3)
      {
         std::shared_ptr<S3ObjectClient> client = std::make_shared<S3ObjectClient>();
         if (!client->Configure(settings, problem))
            return std::shared_ptr<MessageStoreObjectClient>();
         return client;
      }

      if (settings.service == ServiceAzure)
      {
         std::shared_ptr<AzureBlobObjectClient> client = std::make_shared<AzureBlobObjectClient>();
         if (!client->Configure(settings, problem))
            return std::shared_ptr<MessageStoreObjectClient>();
         return client;
      }

      problem = _T("no object store is named (MessageStoreObjectService is s3 or azure)");
      return std::shared_ptr<MessageStoreObjectClient>();
   }
}
