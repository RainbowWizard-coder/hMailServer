// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// See DomainReports.h.

#include "StdAfx.h"

#include "DomainReports.h"

#include "ByteBuffer.h"
#include "File.h"
#include "FileUtilities.h"
#include "JsonDocument.h"
#include "Unicode.h"
#include "Encoding/Base64.h"
#include "Hashing/HashCreator.h"
#include "../Mime/Mime.h"
#include "../BO/Domain.h"
#include "../BO/Message.h"
#include "../BO/MessageRecipient.h"
#include "../BO/MessageRecipients.h"
#include "../Cache/CacheContainer.h"
#include "../Persistence/PersistentMessage.h"
#include "../Persistence/MessageStore.h"
#include "../Application/Application.h"
#include "../Application/IniFileSettings.h"
#include "../AntiSpam/AuthenticationResults.h"
#include "Parsing/StringParser.h"
#include "../SQL/DALConnection.h"
#include "../SQL/DALRecordset.h"
#include "../SQL/DatabaseConnectionManager.h"
#include "../SQL/DatabaseSettings.h"
#include "../SQL/SQLCommand.h"
#include "../SQL/SQLStatement.h"

// zlib: on Windows the copy vendored under Server/zlib, which hMailServer.exe
// compiles in; on POSIX the distribution's, whose library CMakeLists.txt links,
// so the header and the library are one version (and the source tarball can
// leave Server/zlib out: build/source-exclusions.txt).
#ifdef HM_PLATFORM_POSIX
#include <zlib.h>
#else
#include "../../zlib/zlib.h"
#endif

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // A message to a report mailbox is not read past this: a day of aggregate
      // rows from the largest reporter is under a megabyte compressed.
      const size_t MaxMessageBytes = 32 * 1024 * 1024;

      // What an attachment may inflate to. A gzip of zeros is a thousand to one,
      // so a 16 KB attachment could otherwise ask for 16 MB - and a crafted one
      // for far more.
      const size_t MaxInflatedBytes = 16 * 1024 * 1024;

      const int MaxXmlDepth = 32;
      const int MaxXmlNodes = 400000;

      // A summary reads at most this many records; a page an administrator
      // looks at, not the mail path.
      const int MaxSummaryRecords = 200000;
      const int MaxSummarySources = 200;
      const int MaxTlsRows = 10000;

      // The set of report mailboxes, read from hm_domains at most this often
      // and dropped outright when a domain is saved.
      const time_t IntakeCacheSeconds = 60;
      const time_t IntakeReportIntervalSeconds = 3600;
      const time_t CeilingLogIntervalSeconds = 3600;

      std::mutex intake_mutex_;
      std::vector<std::pair<String, __int64> > intake_cache_;
      time_t intake_read_at_ = 0;
      bool intake_valid_ = false;
      unsigned long intake_generation_ = 0;
      time_t intake_reported_at_ = 0;

      // The mailboxes already logged as naming a domain this server no longer
      // hosts, so each is said once until a domain is saved again.
      std::set<String> intake_warned_;

      // When each domain's ceiling was last logged, under the same mutex.
      std::map<__int64, time_t> ceiling_logged_;

      AnsiString Utf8(const String &value)
      {
         AnsiString utf8;
         Unicode::WideToMultiByte(value, utf8);
         return utf8;
      }

      String FromUtf8(const AnsiString &value)
      {
         String wide;
         if (!Unicode::MultiByteToWide(value, wide))
            return String(value);

         return wide;
      }

      AnsiString Lower(const AnsiString &value)
      {
         AnsiString result = value;
         result.MakeLower();
         return result;
      }

      AnsiString Trimmed(const AnsiString &value)
      {
         AnsiString result = value;
         result.Trim();
         return result;
      }

      String Canonical(const String &address)
      {
         String result = address;
         result.Trim();
         result.ToLower();
         return result;
      }

      // A JSON string body from UTF-8 bytes.
      AnsiString JsonText(const AnsiString &utf8)
      {
         AnsiString result;

         for (int i = 0; i < utf8.GetLength(); i++)
         {
            const char c = utf8.GetAt(i);

            switch (c)
            {
            case '"':  result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
               if ((unsigned char) c < 0x20)
               {
                  char escaped[8];
                  snprintf(escaped, sizeof(escaped), "\\u%04x", (unsigned char) c);
                  result += escaped;
               }
               else
                  result += c;
               break;
            }
         }

         return result;
      }

      AnsiString JsonField(const char *name, const AnsiString &utf8)
      {
         return AnsiString("\"") + name + "\":\"" + JsonText(utf8) + "\"";
      }

      AnsiString JsonField(const char *name, const String &value)
      {
         return JsonField(name, Utf8(value));
      }

      AnsiString JsonNumber(const char *name, __int64 value)
      {
         AnsiString text;
         text.Format("\"%s\":%I64d", name, value);
         return text;
      }

      AnsiString JsonBool(const char *name, bool value)
      {
         return AnsiString("\"") + name + "\":" + (value ? "true" : "false");
      }

      __int64 Now()
      {
         return (__int64) time(nullptr);
      }

      DatabaseConnectionManager *Db()
      {
         return Application::Instance()->GetDBManager().get();
      }

      // A row-limited SELECT for each backend: TOP (n) on SQL Server and Compact,
      // LIMIT n on MySQL and PostgreSQL. The same knowledge PersistentComplaint
      // and AuditTrail apply.
      String SelectWithLimit(const String &columns, const String &from, const String &where, const String &orderBy, int rows)
      {
         String statement;

         switch (IniFileSettings::Instance()->GetDatabaseType())
         {
         case DatabaseSettings::TypeMSSQLServer:
         case DatabaseSettings::TypeMSSQLCompactEdition:
            statement.Format(_T("select top (%d) %s from %s"), rows, columns.c_str(), from.c_str());
            if (!where.IsEmpty())
               statement += _T(" where ") + where;
            if (!orderBy.IsEmpty())
               statement += _T(" order by ") + orderBy;
            break;
         default:
            statement.Format(_T("select %s from %s"), columns.c_str(), from.c_str());
            if (!where.IsEmpty())
               statement += _T(" where ") + where;
            if (!orderBy.IsEmpty())
               statement += _T(" order by ") + orderBy;
            statement += Formatter::Format(_T(" limit {0}"), rows);
            break;
         }

         return statement;
      }

      int CountWhere(const String &table, const String &where, const SQLCommand &parameters)
      {
         SQLCommand command = parameters;
         command.SetQueryString(_T("select count(*) as c from ") + table + _T(" where ") + where);

         std::shared_ptr<DALRecordset> recordset = Db()->OpenRecordset(command);
         if (!recordset)
            return -1;

         if (recordset->IsEOF())
            return 0;

         return (int) recordset->GetLongValue("c");
      }

      int ClampInt(__int64 value)
      {
         if (value < 0)
            return 0;
         if (value > INT_MAX)
            return INT_MAX;
         return (int) value;
      }

      // ------------------------------------------------------------ the calendar
      //
      // Days since 1970 to and from a civil date, in the proleptic Gregorian
      // calendar, with no library clock: the same arithmetic on both platforms,
      // and no gmtime to shim.

      void CivilFromDays(__int64 z, int &year, unsigned &month, unsigned &day)
      {
         z += 719468;
         const __int64 era = (z >= 0 ? z : z - 146096) / 146097;
         const unsigned doe = (unsigned) (z - era * 146097);
         const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
         const __int64 y = (__int64) yoe + era * 400;
         const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
         const unsigned mp = (5 * doy + 2) / 153;
         day = doy - (153 * mp + 2) / 5 + 1;
         month = mp < 10 ? mp + 3 : mp - 9;
         year = (int) (y + (month <= 2 ? 1 : 0));
      }

      __int64 DaysFromCivil(int year, unsigned month, unsigned day)
      {
         year -= month <= 2 ? 1 : 0;
         const __int64 era = (year >= 0 ? year : year - 399) / 400;
         const unsigned yoe = (unsigned) (year - era * 400);
         const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
         const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
         return era * 146097 + (__int64) doe - 719468;
      }

      // The digits at [at, at + count) of text as a number, or -1.
      int Digits(const AnsiString &text, int at, int count)
      {
         if (at < 0 || at + count > text.GetLength())
            return -1;

         int value = 0;
         for (int i = at; i < at + count; i++)
         {
            const char c = text.GetAt(i);
            if (c < '0' || c > '9')
               return -1;
            value = value * 10 + (c - '0');
         }

         return value;
      }

      // ------------------------------------------------------------ the XML

      struct XmlNode
      {
         AnsiString name;
         AnsiString text;
         std::vector<XmlNode> children;
      };

      void AppendUtf8(AnsiString &out, unsigned long code)
      {
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

      // The five predefined entities and numeric references, and nothing else:
      // a reference this reader does not know stays as it was written, which is
      // what keeps a document from expanding into anything.
      AnsiString DecodeEntities(const AnsiString &raw)
      {
         if (raw.Find("&") < 0)
            return raw;

         const std::string &s = raw;
         AnsiString out;
         out.reserve(s.size());

         for (size_t i = 0; i < s.size(); i++)
         {
            if (s[i] != '&')
            {
               out += s[i];
               continue;
            }

            const size_t semicolon = s.find(';', i);
            if (semicolon == std::string::npos || semicolon - i > 10)
            {
               out += '&';
               continue;
            }

            const std::string entity = s.substr(i + 1, semicolon - i - 1);

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
            else if (!entity.empty() && entity[0] == '#')
            {
               const bool hex = entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X');
               const char *digits = entity.c_str() + (hex ? 2 : 1);
               char *end = nullptr;
               const unsigned long code = strtoul(digits, &end, hex ? 16 : 10);

               if (end == digits || *end != 0 || code == 0 || code > 0x10FFFF)
               {
                  out += '&';
                  continue;
               }

               AppendUtf8(out, code);
            }
            else
            {
               out += '&';
               continue;
            }

            i = semicolon;
         }

         return out;
      }

      bool ParseXml(const AnsiString &xml, XmlNode &document, AnsiString &problem)
      {
         const std::string &s = xml;
         const size_t n = s.size();
         size_t i = 0;

         if (n >= 3 && (unsigned char) s[0] == 0xEF && (unsigned char) s[1] == 0xBB && (unsigned char) s[2] == 0xBF)
            i = 3;

         document = XmlNode();

         // The open elements, root first. A pointer on this stack is to a node
         // held in its parent's children, and that vector only grows when a
         // child is added to the node on top - which is never one of these.
         std::vector<XmlNode *> stack;
         stack.push_back(&document);

         int nodes = 0;

         while (i < n)
         {
            if (s[i] != '<')
            {
               size_t j = s.find('<', i);
               if (j == std::string::npos)
                  j = n;

               if (stack.size() > 1)
                  stack.back()->text += DecodeEntities(AnsiString(s.substr(i, j - i)));

               i = j;
               continue;
            }

            if (s.compare(i, 2, "<?") == 0)
            {
               const size_t j = s.find("?>", i);
               if (j == std::string::npos)
               {
                  problem = "an unterminated processing instruction";
                  return false;
               }

               i = j + 2;
               continue;
            }

            if (s.compare(i, 4, "<!--") == 0)
            {
               const size_t j = s.find("-->", i);
               if (j == std::string::npos)
               {
                  problem = "an unterminated comment";
                  return false;
               }

               i = j + 3;
               continue;
            }

            if (s.compare(i, 9, "<![CDATA[") == 0)
            {
               const size_t j = s.find("]]>", i);
               if (j == std::string::npos)
               {
                  problem = "an unterminated CDATA section";
                  return false;
               }

               if (stack.size() > 1)
                  stack.back()->text += AnsiString(s.substr(i + 9, j - i - 9));

               i = j + 3;
               continue;
            }

            if (s.compare(i, 2, "<!") == 0)
            {
               // A document type declaration, with or without an internal subset
               // in brackets. One that declares entities is refused outright:
               // an entity is the one thing in XML that makes a small document
               // large or reads a file, and no report needs one.
               size_t j = i + 2;
               int brackets = 0;

               for (; j < n; j++)
               {
                  if (s[j] == '[')
                     brackets++;
                  else if (s[j] == ']')
                     brackets--;
                  else if (s[j] == '>' && brackets <= 0)
                     break;
               }

               if (j >= n)
               {
                  problem = "an unterminated declaration";
                  return false;
               }

               if (s.substr(i, j - i).find("<!ENTITY") != std::string::npos)
               {
                  problem = "a document type declaration that declares entities";
                  return false;
               }

               i = j + 1;
               continue;
            }

            if (s.compare(i, 2, "</") == 0)
            {
               const size_t j = s.find('>', i);
               if (j == std::string::npos)
               {
                  problem = "an unterminated end tag";
                  return false;
               }

               if (stack.size() > 1)
                  stack.pop_back();

               i = j + 1;
               continue;
            }

            // A start tag: the name, then attributes to the closing bracket,
            // with a bracket inside a quoted value left alone.
            size_t j = i + 1;
            while (j < n && s[j] != '>' && s[j] != '/' && !isspace((unsigned char) s[j]))
               j++;

            if (j == i + 1)
            {
               problem = "an element with no name";
               return false;
            }

            AnsiString name(s.substr(i + 1, j - i - 1));

            // The RFC 9990 form puts every element in a namespace, and a document
            // that prefixes them names the same elements.
            const int colon = name.ReverseFind(':');
            if (colon >= 0)
               name = name.Mid(colon + 1);
            name.MakeLower();

            char quote = 0;
            size_t k = j;
            for (; k < n; k++)
            {
               const char c = s[k];
               if (quote)
               {
                  if (c == quote)
                     quote = 0;
               }
               else if (c == '"' || c == '\'')
                  quote = c;
               else if (c == '>')
                  break;
            }

            if (k >= n)
            {
               problem = "an unterminated start tag";
               return false;
            }

            const bool selfClosing = s[k - 1] == '/';

            if (++nodes > MaxXmlNodes)
            {
               problem = "more elements than a report holds";
               return false;
            }

            if ((int) stack.size() > MaxXmlDepth)
            {
               problem = "elements nested deeper than a report goes";
               return false;
            }

            XmlNode *parent = stack.back();
            parent->children.push_back(XmlNode());
            XmlNode *child = &parent->children.back();
            child->name = name;

            if (!selfClosing)
               stack.push_back(child);

            i = k + 1;
         }

         if (document.children.empty())
         {
            problem = "no element";
            return false;
         }

         return true;
      }

      const XmlNode *Child(const XmlNode *node, const char *name)
      {
         if (!node)
            return nullptr;

         for (const XmlNode &child : node->children)
         {
            if (child.name == name)
               return &child;
         }

         return nullptr;
      }

      AnsiString Text(const XmlNode *node, const char *name)
      {
         const XmlNode *child = Child(node, name);
         if (!child)
            return AnsiString();

         return Trimmed(child->text);
      }

      __int64 Number(const AnsiString &text, __int64 fallback)
      {
         if (text.IsEmpty())
            return fallback;

         char *end = nullptr;
         const long long value = strtoll(text.c_str(), &end, 10);

         if (end == text.c_str())
            return fallback;

         return (__int64) value;
      }

      // ------------------------------------------------------------ the wrappers

      bool Inflate(const char *data, size_t length, int windowBits, AnsiString &out, AnsiString &problem)
      {
         out.clear();

         z_stream stream;
         memset(&stream, 0, sizeof(stream));

         if (inflateInit2(&stream, windowBits) != Z_OK)
         {
            problem = "zlib could not start";
            return false;
         }

         stream.next_in = (Bytef *) data;
         stream.avail_in = (uInt) length;

         char buffer[16384];
         int code = Z_OK;

         while (code != Z_STREAM_END)
         {
            stream.next_out = (Bytef *) buffer;
            stream.avail_out = sizeof(buffer);

            code = inflate(&stream, Z_NO_FLUSH);
            if (code != Z_OK && code != Z_STREAM_END)
            {
               inflateEnd(&stream);
               problem = "the compressed data is damaged";
               return false;
            }

            const size_t produced = sizeof(buffer) - stream.avail_out;

            if (out.size() + produced > MaxInflatedBytes)
            {
               inflateEnd(&stream);
               problem = "the report is larger than 16 MB once inflated";
               return false;
            }

            if (produced > 0)
               out.append(buffer, produced);
            else if (code != Z_STREAM_END && stream.avail_in == 0)
            {
               inflateEnd(&stream);
               problem = "the compressed data ends early";
               return false;
            }
         }

         inflateEnd(&stream);
         return true;
      }

      unsigned Read16(const std::string &s, size_t at)
      {
         return (unsigned) (unsigned char) s[at] | ((unsigned) (unsigned char) s[at + 1] << 8);
      }

      unsigned long Read32(const std::string &s, size_t at)
      {
         return (unsigned long) Read16(s, at) | ((unsigned long) Read16(s, at + 2) << 16);
      }

      // The first file of a zip archive, by its central directory - the one
      // place the sizes are always written, since a writer that streams leaves
      // them out of the local header.
      bool Unzip(const AnsiString &bytes, AnsiString &content, AnsiString &problem)
      {
         const std::string &s = bytes;
         const size_t n = s.size();

         if (n < 22)
         {
            problem = "not a zip archive";
            return false;
         }

         size_t eocd = std::string::npos;
         const size_t lowest = n > 22 + 65535 ? n - 22 - 65535 : 0;

         for (size_t at = n - 22; ; at--)
         {
            if (s.compare(at, 4, "PK\x05\x06") == 0)
            {
               eocd = at;
               break;
            }

            if (at == lowest)
               break;
         }

         if (eocd == std::string::npos)
         {
            problem = "a zip archive with no central directory";
            return false;
         }

         const unsigned entries = Read16(s, eocd + 10);
         const unsigned long directorySize = Read32(s, eocd + 12);
         const unsigned long directoryOffset = Read32(s, eocd + 16);

         if (directoryOffset > n || directorySize > n - directoryOffset)
         {
            problem = "a zip archive whose central directory is outside it";
            return false;
         }

         size_t at = directoryOffset;

         for (unsigned entry = 0; entry < entries && entry < 100; entry++)
         {
            if (at + 46 > n || s.compare(at, 4, "PK\x01\x02") != 0)
            {
               problem = "a zip archive with a damaged central directory";
               return false;
            }

            const unsigned method = Read16(s, at + 10);
            const unsigned long compressedSize = Read32(s, at + 20);
            const unsigned long size = Read32(s, at + 24);
            const unsigned nameLength = Read16(s, at + 28);
            const unsigned extraLength = Read16(s, at + 30);
            const unsigned commentLength = Read16(s, at + 32);
            const unsigned long local = Read32(s, at + 42);

            if (at + 46 + nameLength > n)
            {
               problem = "a zip archive with a damaged central directory";
               return false;
            }

            const std::string name = s.substr(at + 46, nameLength);
            at += 46 + nameLength + extraLength + commentLength;

            if (!name.empty() && name[name.size() - 1] == '/')
               continue;

            if (size > MaxInflatedBytes)
            {
               problem = "the report is larger than 16 MB once inflated";
               return false;
            }

            if (local > n || n - local < 30 || s.compare(local, 4, "PK\x03\x04") != 0)
            {
               problem = "a zip archive with a damaged entry";
               return false;
            }

            const size_t dataAt = local + 30 + Read16(s, local + 26) + Read16(s, local + 28);
            if (dataAt > n || compressedSize > n - dataAt)
            {
               problem = "a zip archive whose entry is outside it";
               return false;
            }

            if (method == 0)
            {
               content = AnsiString(s.substr(dataAt, compressedSize));
               return true;
            }

            if (method == 8)
               return Inflate(s.data() + dataAt, compressedSize, -15, content, problem);

            problem = "a zip entry compressed with a method this server does not read";
            return false;
         }

         problem = "a zip archive with no file in it";
         return false;
      }

      bool LooksLikeReport(const String &contentType, const String &name)
      {
         String type = contentType;
         type.Trim();
         type.ToLower();

         String file = name;
         file.Trim();
         file.ToLower();

         if (type == _T("application/gzip") || type == _T("application/x-gzip") ||
             type == _T("application/zip") || type == _T("application/x-zip-compressed") ||
             type == _T("application/tlsrpt+gzip") || type == _T("application/tlsrpt+json") ||
             type == _T("text/xml") || type == _T("application/xml") || type == _T("application/json"))
            return true;

         // application/octet-stream is what a sender that could not be bothered
         // labels everything, so the name decides for it and for a type this
         // list does not know.
         return file.EndsWith(_T(".xml")) || file.EndsWith(_T(".gz")) || file.EndsWith(_T(".zip")) || file.EndsWith(_T(".json"));
      }

      void Collect(MimeBody &part, int depth, std::vector<DomainReports::Candidate> &out)
      {
         // Ten parts at most: a report is one attachment, and a message built
         // to carry hundreds of small archives would otherwise have each one
         // inflated to its 16 MB before anything refused it.
         if (depth > 3 || (int) out.size() >= DomainReports::MaxCandidatesPerMessage)
            return;

         std::shared_ptr<MimeBody> child = part.FindFirstPart();

         if (!child)
         {
            // A leaf: the message's own body, or one attachment.
            const String type = part.GetCleanContentType();
            String name = part.GetRawFilename();
            if (name.IsEmpty())
               name = String(part.GetName().c_str());

            if (!LooksLikeReport(type, name))
               return;

            DomainReports::Candidate candidate;
            if (!part.GetDecodedContent(candidate.bytes))
               return;

            candidate.name = Utf8(name);
            out.push_back(candidate);
            return;
         }

         for (; child; child = part.FindNextPart())
            Collect(*child, depth + 1, out);
      }

      void Expect(bool condition, const char *message)
      {
         if (!condition)
            throw std::logic_error(message);
      }

      void Put16(AnsiString &out, unsigned value)
      {
         out += (char) (value & 0xFF);
         out += (char) ((value >> 8) & 0xFF);
      }

      void Put32(AnsiString &out, unsigned long value)
      {
         Put16(out, (unsigned) (value & 0xFFFF));
         Put16(out, (unsigned) ((value >> 16) & 0xFFFF));
      }
   }

   // ------------------------------------------------------------------- readers

   AnsiString
   DomainReports::DayOf(__int64 seconds)
   {
      if (seconds < 0)
         seconds = 0;

      int year = 1970;
      unsigned month = 1, day = 1;
      CivilFromDays(seconds / 86400, year, month, day);

      char text[16];
      snprintf(text, sizeof(text), "%04d-%02u-%02u", year, month, day);
      return AnsiString(text);
   }

   __int64
   DomainReports::ParseDateTime(const AnsiString &raw)
   {
      // 2026-09-17T00:00:00Z, with or without a fraction, and with Z or an
      // offset; a date with no time is midnight. Anything else is -1.
      const AnsiString text = Trimmed(raw);

      const int year = Digits(text, 0, 4);
      const int month = Digits(text, 5, 2);
      const int day = Digits(text, 8, 2);

      if (year < 1970 || month < 1 || month > 12 || day < 1 || day > 31 || text.GetAt(4) != '-' || text.GetAt(7) != '-')
         return -1;

      __int64 seconds = DaysFromCivil(year, (unsigned) month, (unsigned) day) * 86400;

      if (text.GetLength() == 10)
         return seconds;

      if (text.GetAt(10) != 'T' && text.GetAt(10) != 't' && text.GetAt(10) != ' ')
         return -1;

      const int hour = Digits(text, 11, 2);
      const int minute = Digits(text, 14, 2);
      const int second = Digits(text, 17, 2);

      if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 60 || text.GetAt(13) != ':' || text.GetAt(16) != ':')
         return -1;

      seconds += hour * 3600 + minute * 60 + second;

      int at = 19;
      if (at < text.GetLength() && text.GetAt(at) == '.')
      {
         at++;
         while (at < text.GetLength() && text.GetAt(at) >= '0' && text.GetAt(at) <= '9')
            at++;
      }

      if (at >= text.GetLength())
         return seconds;

      const char sign = text.GetAt(at);
      if (sign == 'Z' || sign == 'z')
         return seconds;

      if (sign != '+' && sign != '-')
         return -1;

      const int offsetHours = Digits(text, at + 1, 2);
      const int offsetMinutes = text.GetLength() >= at + 6 ? Digits(text, at + 4, 2) : 0;
      if (offsetHours < 0 || offsetMinutes < 0)
         return -1;

      const __int64 offset = (__int64) offsetHours * 3600 + offsetMinutes * 60;
      return sign == '+' ? seconds - offset : seconds + offset;
   }

   String
   DomainReports::DmarcKey(const AnsiString &orgName, const AnsiString &reportId, const AnsiString &policyDomain)
   {
      // What RFC 7489 7.2.1.1 says makes a report unique: the reporter and its
      // id, and the domain in case one reporter reuses ids across domains. The
      // same report delivered twice - a retry, or two nodes reading one mailbox
      // - makes one row.
      const AnsiString material = Lower(Trimmed(orgName)) + "\n" + Trimmed(reportId) + "\n" + Lower(Trimmed(policyDomain));

      HashCreator hasher(HashCreator::SHA256);
      return String(hasher.GenerateHashNoSalt(material, HashCreator::hex));
   }

   String
   DomainReports::TlsKey(const AnsiString &orgName, const AnsiString &reportId, const AnsiString &policyDomain, const AnsiString &policyType)
   {
      const AnsiString material = Lower(Trimmed(orgName)) + "\n" + Trimmed(reportId) + "\n" + Lower(Trimmed(policyDomain)) + "\n" + Lower(Trimmed(policyType));

      HashCreator hasher(HashCreator::SHA256);
      return String(hasher.GenerateHashNoSalt(material, HashCreator::hex));
   }

   bool
   DomainReports::IsHostName(const AnsiString &name)
   {
      // Letters, digits, dots and hyphens, no empty label, at most 253
      // characters: what a DNS name is once it is in ASCII, which a domain in
      // a report always is. No regular expression, since this runs on the
      // delivery thread against whatever a stranger wrote.
      const std::string &s = name;

      if (s.empty() || s.size() > 253 || s[0] == '.' || s[s.size() - 1] == '.')
         return false;

      bool labelEmpty = true;

      for (size_t i = 0; i < s.size(); i++)
      {
         const char c = s[i];

         if (c == '.')
         {
            if (labelEmpty)
               return false;

            labelEmpty = true;
            continue;
         }

         const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
         const bool digit = c >= '0' && c <= '9';

         if (!letter && !digit && c != '-')
            return false;

         labelEmpty = false;
      }

      return !labelEmpty;
   }

   namespace
   {
      std::string TrimmedStd(const std::string &text)
      {
         size_t from = 0;
         while (from < text.size() && (text[from] == ' ' || text[from] == '\t'))
            from++;

         size_t to = text.size();
         while (to > from && (text[to - 1] == ' ' || text[to - 1] == '\t' || text[to - 1] == '\r'))
            to--;

         return text.substr(from, to - from);
      }

      std::string LowerStd(const std::string &text)
      {
         std::string result = text;
         for (size_t i = 0; i < result.size(); i++)
            result[i] = (char) tolower((unsigned char) result[i]);
         return result;
      }

      // The run of [A-Za-z0-9.-] at `at`, which is what a keyword or a domain
      // in an Authentication-Results field is made of.
      std::string TokenAt(const std::string &text, size_t at)
      {
         size_t end = at;
         while (end < text.size())
         {
            const char c = text[end];
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
            if (!ok)
               break;
            end++;
         }

         return text.substr(at, end - at);
      }
   }

   void
   DomainReports::ReadMessageDmarc(const AnsiString &rawMessage, const AnsiString &authservId, AnsiString &result, AnsiString &fromDomain)
   {
      result.clear();
      fromDomain.clear();

      const std::string wanted = LowerStd(TrimmedStd(std::string(authservId)));
      if (wanted.empty())
         return;

      // The header block, split into fields; a line beginning with white space
      // continues the field before it and is unfolded onto it.
      const std::string &raw = rawMessage;
      size_t blockEnd = raw.find("\r\n\r\n");
      if (blockEnd == std::string::npos)
         blockEnd = raw.find("\n\n");
      const std::string headers = raw.substr(0, blockEnd == std::string::npos ? raw.size() : blockEnd);

      std::vector<std::string> fields;
      size_t at = 0;

      while (at < headers.size())
      {
         const size_t eol = headers.find('\n', at);
         std::string line = headers.substr(at, eol == std::string::npos ? std::string::npos : eol - at);
         at = eol == std::string::npos ? headers.size() : eol + 1;

         if (!line.empty() && line[line.size() - 1] == '\r')
            line.erase(line.size() - 1);

         if (!line.empty() && (line[0] == ' ' || line[0] == '\t') && !fields.empty())
            fields.back() += " " + TrimmedStd(line);
         else
            fields.push_back(line);
      }

      for (const std::string &field : fields)
      {
         const size_t colon = field.find(':');
         if (colon == std::string::npos)
            continue;

         if (LowerStd(TrimmedStd(field.substr(0, colon))) != "authentication-results")
            continue;

         // The authserv-id is the first token, possibly quoted, and the methods
         // follow it separated by semicolons. Only a field bearing this server's
         // own id is its verdict; the writer strips a forged one before it adds
         // its own, so the first such field is ours.
         const std::string value = TrimmedStd(field.substr(colon + 1));
         const size_t stop = value.find_first_of("; \t");
         std::string id = value.substr(0, stop);

         if (id.size() >= 2 && id[0] == '"' && id[id.size() - 1] == '"')
            id = id.substr(1, id.size() - 2);

         if (LowerStd(id) != wanted)
            continue;

         std::string rest = stop == std::string::npos ? std::string() : value.substr(stop);
         size_t from = 0;

         while (from <= rest.size())
         {
            const size_t semicolon = rest.find(';', from);
            const std::string clause = TrimmedStd(rest.substr(from, semicolon == std::string::npos ? std::string::npos : semicolon - from));
            from = semicolon == std::string::npos ? rest.size() + 1 : semicolon + 1;

            if (LowerStd(clause.substr(0, 6)) != "dmarc=")
               continue;

            result = AnsiString(LowerStd(TokenAt(clause, 6)).substr(0, 16));

            const size_t header = LowerStd(clause).find("header.from=");
            if (header != std::string::npos)
               fromDomain = AnsiString(LowerStd(TokenAt(clause, header + 12)).substr(0, 255));

            return;
         }

         return;
      }
   }

   bool
   DomainReports::ParseDmarcXml(const AnsiString &xml, DmarcReport &report, AnsiString &problem)
   {
      report = DmarcReport();
      problem = AnsiString();

      XmlNode document;
      if (!ParseXml(xml, document, problem))
         return false;

      const XmlNode *feedback = Child(&document, "feedback");
      if (!feedback)
      {
         problem = "the root element is not feedback";
         return false;
      }

      const XmlNode *metadata = Child(feedback, "report_metadata");
      const XmlNode *policy = Child(feedback, "policy_published");

      if (!metadata || !policy)
      {
         problem = "no report_metadata or no policy_published element";
         return false;
      }

      report.org_name = Text(metadata, "org_name").Left(255);
      report.email = Text(metadata, "email").Left(255);
      report.report_id = Text(metadata, "report_id").Left(255);

      const XmlNode *range = Child(metadata, "date_range");
      report.begin = Number(Text(range, "begin"), 0);
      report.end = Number(Text(range, "end"), 0);

      report.policy_domain = Lower(Text(policy, "domain")).Left(255);
      report.adkim = Lower(Text(policy, "adkim")).Left(8);
      report.aspf = Lower(Text(policy, "aspf")).Left(8);
      report.p = Lower(Text(policy, "p")).Left(16);
      report.sp = Lower(Text(policy, "sp")).Left(16);
      report.pct = ClampInt(Number(Text(policy, "pct"), 100));

      if (report.policy_domain.IsEmpty())
      {
         problem = "policy_published names no domain";
         return false;
      }

      if (!IsHostName(report.policy_domain))
      {
         problem = "policy_published names something that is not a host name";
         return false;
      }

      if (report.report_id.IsEmpty())
      {
         problem = "no report_id";
         return false;
      }

      if (report.begin < 0 || report.end < report.begin)
      {
         problem = "a date_range that ends before it begins";
         return false;
      }

      for (const XmlNode &child : feedback->children)
      {
         if (child.name != "record")
            continue;

         if ((int) report.records.size() >= MaxRecordsPerReport)
         {
            report.dropped_records++;
            continue;
         }

         const XmlNode *row = Child(&child, "row");
         const XmlNode *evaluated = Child(row, "policy_evaluated");
         const XmlNode *identifiers = Child(&child, "identifiers");
         const XmlNode *auth = Child(&child, "auth_results");

         DmarcRecord record;
         record.source_ip = Text(row, "source_ip").Left(45);
         record.count = ClampInt(Number(Text(row, "count"), 0));
         record.disposition = Lower(Text(evaluated, "disposition")).Left(16);
         record.dkim = Lower(Text(evaluated, "dkim")).Left(8);
         record.spf = Lower(Text(evaluated, "spf")).Left(8);
         record.header_from = Lower(Text(identifiers, "header_from")).Left(255);
         record.envelope_from = Lower(Text(identifiers, "envelope_from")).Left(255);

         if (auth)
         {
            for (const XmlNode &result : auth->children)
            {
               AnsiString piece;

               if (result.name == "dkim")
               {
                  piece = "d=" + Lower(Text(&result, "domain")) + " s=" + Text(&result, "selector") + " result=" + Lower(Text(&result, "result"));
                  if (!record.dkim_auth.IsEmpty())
                     record.dkim_auth += "; ";
                  record.dkim_auth += piece;
               }
               else if (result.name == "spf")
               {
                  piece = "d=" + Lower(Text(&result, "domain")) + " scope=" + Lower(Text(&result, "scope")) + " result=" + Lower(Text(&result, "result"));
                  if (!record.spf_auth.IsEmpty())
                     record.spf_auth += "; ";
                  record.spf_auth += piece;
               }
            }
         }

         record.dkim_auth = record.dkim_auth.Left(500);
         record.spf_auth = record.spf_auth.Left(255);

         // A record with no source is not one: nothing in it can be charted.
         if (record.source_ip.IsEmpty())
         {
            report.dropped_records++;
            continue;
         }

         report.records.push_back(record);
      }

      if (report.records.empty())
      {
         problem = "no record with a source";
         return false;
      }

      return true;
   }

   bool
   DomainReports::ParseTlsJson(const AnsiString &json, TlsReport &report, AnsiString &problem)
   {
      report = TlsReport();
      problem = AnsiString();

      JsonValue root;
      std::string error;

      if (!JsonValue::Parse(std::string(json), root, error))
      {
         problem = AnsiString(error);
         return false;
      }

      if (!root.IsObject())
      {
         problem = "not a JSON object";
         return false;
      }

      report.org_name = AnsiString(root.GetString("organization-name")).Left(255);
      report.contact = AnsiString(root.GetString("contact-info")).Left(255);
      report.report_id = AnsiString(root.GetString("report-id")).Left(255);

      const JsonValue *range = root.Get("date-range");
      if (range && range->IsObject())
      {
         report.begin = ParseDateTime(AnsiString(range->GetString("start-datetime")));
         report.end = ParseDateTime(AnsiString(range->GetString("end-datetime")));
      }
      else
         report.begin = report.end = -1;

      if (report.report_id.IsEmpty())
      {
         problem = "no report-id";
         return false;
      }

      if (report.begin < 0 || report.end < report.begin)
      {
         problem = "no readable date-range";
         return false;
      }

      const JsonValue *policies = root.Get("policies");
      if (!policies || !policies->IsArray())
      {
         problem = "no policies";
         return false;
      }

      for (size_t i = 0; i < policies->Size() && i < 100; i++)
      {
         const JsonValue *entry = policies->At(i);
         if (!entry || !entry->IsObject())
            continue;

         const JsonValue *policy = entry->Get("policy");
         const JsonValue *summary = entry->Get("summary");

         TlsPolicyResult result;

         if (policy && policy->IsObject())
         {
            result.policy_type = Lower(AnsiString(policy->GetString("policy-type"))).Left(16);
            result.policy_domain = Lower(AnsiString(policy->GetString("policy-domain"))).Left(255);
         }

         if (summary && summary->IsObject())
         {
            result.successful = ClampInt(summary->GetInt64("total-successful-session-count"));
            result.failed = ClampInt(summary->GetInt64("total-failure-session-count"));
         }

         // The failure details, kept as a compact list of what an operator acts
         // on; the column holds 2000 characters and the list stops before it
         // would not fit, still well formed.
         AnsiString list = "[";
         const JsonValue *failures = entry->Get("failure-details");

         if (failures && failures->IsArray())
         {
            for (size_t f = 0; f < failures->Size() && f < 200; f++)
            {
               const JsonValue *failure = failures->At(f);
               if (!failure || !failure->IsObject())
                  continue;

               AnsiString item = "{" + JsonField("result_type", AnsiString(failure->GetString("result-type")).Left(64)) +
                                 "," + JsonField("sending_mta_ip", AnsiString(failure->GetString("sending-mta-ip")).Left(45)) +
                                 "," + JsonField("receiving_mx_hostname", AnsiString(failure->GetString("receiving-mx-hostname")).Left(255)) +
                                 "," + JsonField("receiving_ip", AnsiString(failure->GetString("receiving-ip")).Left(45)) +
                                 "," + JsonNumber("count", ClampInt(failure->GetInt64("failed-session-count"))) + "}";

               if (list.GetLength() + item.GetLength() + 2 > 1998)
                  break;

               if (list.GetLength() > 1)
                  list += ",";
               list += item;
            }
         }

         list += "]";
         result.failures_json = list;

         // A policy naming no domain, or naming something that is not a host
         // name, is not one this server could match to a domain it hosts.
         if (result.policy_domain.IsEmpty() || !IsHostName(result.policy_domain))
            continue;

         report.policies.push_back(result);
      }

      if (report.policies.empty())
      {
         problem = "no policy names a domain";
         return false;
      }

      return true;
   }

   bool
   DomainReports::Unwrap(const AnsiString &bytes, AnsiString &content, AnsiString &problem)
   {
      content.clear();
      problem = AnsiString();

      const std::string &s = bytes;

      // Told from the bytes rather than the name or the declared type: senders
      // label a gzip as application/octet-stream and a zip as application/gzip.
      if (s.size() >= 2 && (unsigned char) s[0] == 0x1f && (unsigned char) s[1] == 0x8b)
         return Inflate(s.data(), s.size(), 16 + MAX_WBITS, content, problem);

      if (s.size() >= 4 && s.compare(0, 4, "PK\x03\x04") == 0)
         return Unzip(bytes, content, problem);

      if (s.size() > MaxInflatedBytes)
      {
         problem = "the report is larger than 16 MB";
         return false;
      }

      content = bytes;
      return true;
   }

   bool
   DomainReports::FindCandidates(const AnsiString &rawMessage, std::vector<Candidate> &candidates)
   {
      candidates.clear();

      if (rawMessage.IsEmpty() || rawMessage.size() > MaxMessageBytes)
         return false;

      MimeBody message;
      size_t index = 0;
      bool partLoaded = false;

      if (message.Load(rawMessage.c_str(), rawMessage.GetLength(), index, partLoaded) == 0)
         return false;

      Collect(message, 0, candidates);
      return true;
   }

   // ------------------------------------------------------------------- intake

   bool
   DomainReports::ReadIntakes_(std::vector<std::pair<String, __int64> > &intakes)
   {
      intakes.clear();

      if (Db() == nullptr)
         return false;

      SQLCommand command(_T("select domainid, domainreportmailbox from hm_domains where domainreportmailbox <> ''"));

      std::shared_ptr<DALRecordset> recordset = Db()->OpenRecordset(command);
      if (!recordset)
         return false;

      std::vector<std::pair<String, __int64> > named;

      while (!recordset->IsEOF())
      {
         const String address = Canonical(recordset->GetStringValue("domainreportmailbox"));
         if (!address.IsEmpty())
            named.push_back(std::make_pair(address, recordset->GetInt64Value("domainid")));

         recordset->MoveNext();
      }

      // A mailbox in a domain this server no longer hosts reads nothing, since
      // no message can be delivered to it here. A rename or a delete rewrites
      // or clears the column (PersistentDomain), so this is a restore or a
      // hand-edited row - said once per mailbox, until a domain is saved.
      for (const auto &entry : named)
      {
         const String mailboxDomain = entry.first.Find(_T("@")) >= 0 ? StringParser::ExtractDomain(entry.first) : String();
         const bool hosted = !mailboxDomain.IsEmpty() && CacheContainer::Instance()->GetDomain(mailboxDomain);

         if (hosted)
         {
            intakes.push_back(entry);
            continue;
         }

         bool warn = false;

         {
            std::lock_guard<std::mutex> guard(intake_mutex_);
            warn = intake_warned_.insert(entry.first).second;
         }

         if (warn)
         {
            LOG_APPLICATION(Formatter::Format(_T("DomainReports: the report mailbox {0} of domain {1} is in a domain this server does not host, so no report is read for that domain. Name a mailbox in a hosted domain, or clear it."),
               entry.first, entry.second));
         }
      }

      return true;
   }

   void
   DomainReports::ForgetIntakes()
   {
      std::lock_guard<std::mutex> guard(intake_mutex_);
      intake_valid_ = false;
      intake_generation_++;
      intake_warned_.clear();
   }

   bool
   DomainReports::IntakeDomainFor_(const String &address, __int64 &domainID)
   {
      domainID = 0;

      const String wanted = Canonical(address);
      if (wanted.IsEmpty())
         return false;

      const time_t now = time(nullptr);
      unsigned long generation = 0;

      {
         std::lock_guard<std::mutex> guard(intake_mutex_);

         if (intake_valid_ && intake_read_at_ <= now && now - intake_read_at_ < IntakeCacheSeconds)
         {
            for (const auto &intake : intake_cache_)
            {
               if (intake.first == wanted)
               {
                  domainID = intake.second;
                  return true;
               }
            }

            return false;
         }

         generation = intake_generation_;
      }

      // Read with no lock held: this is the delivery thread, and a database
      // that has stopped answering must not park it behind a lock another
      // delivery thread is waiting on as well.
      std::vector<std::pair<String, __int64> > fresh;
      bool read = false;

      try
      {
         read = ReadIntakes_(fresh);
      }
      catch (...)
      {
         read = false;
      }

      if (!read)
      {
         bool report = false;

         {
            std::lock_guard<std::mutex> guard(intake_mutex_);

            if (intake_reported_at_ == 0 || intake_reported_at_ > now || now - intake_reported_at_ >= IntakeReportIntervalSeconds)
            {
               intake_reported_at_ = now;
               report = true;
            }
         }

         // With no list nothing is read, which is the safe way to fail: the
         // message is delivered to the mailbox and can be read again by hand.
         if (report)
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6802, "DomainReports::IntakeDomainFor_",
               "The domains' report mailboxes could not be read from the database, so no DMARC or TLS-RPT report is read until they can be. This is reported at most once an hour while it lasts.");
         }

         return false;
      }

      {
         std::lock_guard<std::mutex> guard(intake_mutex_);

         // Published only if no domain was saved while this was reading.
         if (generation == intake_generation_)
         {
            intake_cache_ = fresh;
            intake_read_at_ = now;
            intake_valid_ = true;
         }
      }

      for (const auto &intake : fresh)
      {
         if (intake.first == wanted)
         {
            domainID = intake.second;
            return true;
         }
      }

      return false;
   }

   bool
   DomainReports::ReadFile_(const String &fileName, AnsiString &raw)
   {
      raw.clear();

      unsigned __int64 size = 0;
      if (!FileUtilities::FileSize64(fileName, size))
         return false;

      if (size == 0 || size > MaxMessageBytes)
         return false;

      File file;
      if (!file.Open(fileName, File::OTReadOnly))
         return false;

      std::shared_ptr<ByteBuffer> buffer = file.ReadFile();
      file.Close();

      if (!buffer || buffer->GetSize() == 0)
         return false;

      raw.append(buffer->GetCharBuffer(), buffer->GetSize());
      return true;
   }

   __int64
   DomainReports::HostedDomainID_(const AnsiString &domainName)
   {
      // The readers refuse a name that is not a host name before this is
      // reached; held here too, because the cache asserts on an address.
      if (!IsHostName(domainName))
         return 0;

      String name = FromUtf8(domainName);
      name.Trim();

      if (name.IsEmpty())
         return 0;

      // A domain that names no report mailbox takes no reports in, wherever
      // they arrive: a report about it that came to a sister domain's mailbox
      // is not recorded, so naming a mailbox on one domain never starts a
      // chart for another whose administrator asked for none.
      std::shared_ptr<const Domain> domain = CacheContainer::Instance()->GetDomain(name);
      if (!domain || domain->GetReportMailbox().IsEmpty())
         return 0;

      return domain->GetID();
   }

   bool
   DomainReports::TakenInLastDay_(__int64 domainID, __int64 &dmarcRecords, __int64 &tlsRows)
   {
      dmarcRecords = 0;
      tlsRows = 0;

      const __int64 since = Now() - 86400;

      // The records are added up from the report rows' own count rather than
      // counted in the records table, so the question costs the domain's rows
      // of one day and not a join over every record it holds.
      SQLCommand records(_T("select coalesce(sum(drrecords), 0) as c from hm_dmarcreports where drdomainid = @DOMAINID and drreceived >= @SINCE"));
      records.AddParameter("@DOMAINID", domainID);
      records.AddParameter("@SINCE", since);

      std::shared_ptr<DALRecordset> recordset = Db()->OpenRecordset(records);
      if (!recordset)
         return false;

      if (!recordset->IsEOF())
      {
         // A sum is a DECIMAL on MySQL and an integer elsewhere - the same
         // distinction PersistentAccount::GetMessageBoxSize makes.
         if (IniFileSettings::Instance()->GetDatabaseType() == DatabaseSettings::TypeMYSQLServer)
            dmarcRecords = (__int64) recordset->GetDoubleValue("c");
         else
            dmarcRecords = recordset->GetInt64Value("c");
      }

      SQLCommand parameters;
      parameters.AddParameter("@DOMAINID", domainID);
      parameters.AddParameter("@SINCE", since);

      const int rows = CountWhere(_T("hm_tlsreports"), _T("trdomainid = @DOMAINID and trreceived >= @SINCE"), parameters);
      if (rows < 0)
         return false;

      tlsRows = rows;
      return true;
   }

   bool
   DomainReports::ShouldLogCeiling_(__int64 domainID)
   {
      const time_t now = time(nullptr);

      std::lock_guard<std::mutex> guard(intake_mutex_);

      std::map<__int64, time_t>::iterator logged = ceiling_logged_.find(domainID);
      if (logged != ceiling_logged_.end() && logged->second <= now && now - logged->second < CeilingLogIntervalSeconds)
         return false;

      ceiling_logged_[domainID] = now;
      return true;
   }

   int
   DomainReports::RecordDmarc_(__int64 domainID, const DmarcReport &report, const AnsiString &source,
                               const AnsiString &messageDmarc, const AnsiString &messageFrom, __int64 &reportRowID)
   {
      reportRowID = 0;

      const String key = DmarcKey(report.org_name, report.report_id, report.policy_domain);

      __int64 messages = 0, passed = 0;
      for (const DmarcRecord &record : report.records)
      {
         messages += record.count;
         if (record.dkim == "pass" || record.spf == "pass")
            passed += record.count;
      }

      SQLStatement statement;
      statement.SetTable("hm_dmarcreports");
      statement.AddColumnInt64("drdomainid", domainID);
      statement.AddColumn("drkey", key);
      statement.AddColumn("drorgname", FromUtf8(report.org_name).Left(255));
      statement.AddColumn("dremail", FromUtf8(report.email).Left(255));
      statement.AddColumn("drreportid", FromUtf8(report.report_id).Left(255));
      statement.AddColumnInt64("drbegin", report.begin);
      statement.AddColumnInt64("drend", report.end);
      statement.AddColumn("drpolicydomain", FromUtf8(report.policy_domain).Left(255));
      statement.AddColumn("dradkim", FromUtf8(report.adkim).Left(8));
      statement.AddColumn("draspf", FromUtf8(report.aspf).Left(8));
      statement.AddColumn("drp", FromUtf8(report.p).Left(16));
      statement.AddColumn("drsp", FromUtf8(report.sp).Left(16));
      statement.AddColumn("drpct", (long) report.pct);
      statement.AddColumn("drmessages", (long) ClampInt(messages));
      statement.AddColumn("drpassed", (long) ClampInt(passed));
      statement.AddColumnInt64("drreceived", Now());
      statement.AddColumn("drsource", FromUtf8(source).Left(255));
      statement.AddColumn("drrecords", (long) report.records.size());
      statement.AddColumn("drmsgdmarc", FromUtf8(messageDmarc).Left(16));
      statement.AddColumn("drmsgfrom", FromUtf8(messageFrom).Left(255));
      statement.SetStatementType(SQLStatement::STInsert);
      statement.SetIdentityColumn("drid");

      // The unique index on the key decides a repeat: the loser is not an error,
      // so none is written, and the key is read back to tell "already recorded"
      // from "could not be written" - the shape PersistentComplaint::Insert has.
      __int64 inserted = 0;
      if (!Db()->Execute(statement, &inserted, DALConnection::DALErrorInSQL) || inserted <= 0)
      {
         SQLCommand command(_T("select drid from hm_dmarcreports where drkey = @KEY"));
         command.AddParameter("@KEY", key);

         std::shared_ptr<DALRecordset> recordset = Db()->OpenRecordset(command);
         if (!recordset)
            return -1;

         return recordset->IsEOF() ? -1 : 0;
      }

      for (const DmarcRecord &record : report.records)
      {
         SQLStatement row;
         row.SetTable("hm_dmarcreportrecords");
         row.AddColumnInt64("rrreportid", inserted);
         row.AddColumn("rrsourceip", FromUtf8(record.source_ip).Left(45));
         row.AddColumn("rrcount", (long) record.count);
         row.AddColumn("rrdisposition", FromUtf8(record.disposition).Left(16));
         row.AddColumn("rrdkim", FromUtf8(record.dkim).Left(8));
         row.AddColumn("rrspf", FromUtf8(record.spf).Left(8));
         row.AddColumn("rrheaderfrom", FromUtf8(record.header_from).Left(255));
         row.AddColumn("rrenvelopefrom", FromUtf8(record.envelope_from).Left(255));
         row.AddColumn("rrdkimauth", FromUtf8(record.dkim_auth).Left(500));
         row.AddColumn("rrspfauth", FromUtf8(record.spf_auth).Left(255));
         row.SetStatementType(SQLStatement::STInsert);
         row.SetIdentityColumn("rrid");

         if (!Db()->Execute(row))
         {
            // Half a report is worse than none: a chart drawn from it would
            // under-count with nothing to say so. The report row goes, and the
            // records written so far with it, and the next delivery of the same
            // report is not a repeat.
            SQLCommand remove(_T("delete from hm_dmarcreports where drid = @ID"));
            remove.AddParameter("@ID", inserted);
            Db()->Execute(remove);
            return -1;
         }
      }

      reportRowID = inserted;
      return 1;
   }

   int
   DomainReports::RecordTls_(__int64 domainID, const TlsReport &report, const TlsPolicyResult &policy, const AnsiString &source)
   {
      const String key = TlsKey(report.org_name, report.report_id, policy.policy_domain, policy.policy_type);

      SQLStatement statement;
      statement.SetTable("hm_tlsreports");
      statement.AddColumnInt64("trdomainid", domainID);
      statement.AddColumn("trkey", key);
      statement.AddColumn("trorgname", FromUtf8(report.org_name).Left(255));
      statement.AddColumn("trcontact", FromUtf8(report.contact).Left(255));
      statement.AddColumn("trreportid", FromUtf8(report.report_id).Left(255));
      statement.AddColumnInt64("trbegin", report.begin);
      statement.AddColumnInt64("trend", report.end);
      statement.AddColumn("trpolicytype", FromUtf8(policy.policy_type).Left(16));
      statement.AddColumn("trpolicydomain", FromUtf8(policy.policy_domain).Left(255));
      statement.AddColumn("trsuccess", (long) policy.successful);
      statement.AddColumn("trfailure", (long) policy.failed);
      statement.AddColumn("trfailures", FromUtf8(policy.failures_json).Left(2000));
      statement.AddColumnInt64("trreceived", Now());
      statement.AddColumn("trsource", FromUtf8(source).Left(255));
      statement.SetStatementType(SQLStatement::STInsert);
      statement.SetIdentityColumn("trid");

      __int64 inserted = 0;
      if (Db()->Execute(statement, &inserted, DALConnection::DALErrorInSQL) && inserted > 0)
         return 1;

      SQLCommand command(_T("select trid from hm_tlsreports where trkey = @KEY"));
      command.AddParameter("@KEY", key);

      std::shared_ptr<DALRecordset> recordset = Db()->OpenRecordset(command);
      if (!recordset)
         return -1;

      return recordset->IsEOF() ? -1 : 0;
   }

   void
   DomainReports::InspectIncomingReport(std::shared_ptr<Message> message)
   {
      if (!message)
         return;

      // The local recipients, by the address the report was addressed to.
      std::vector<String> localAddresses;
      std::set<String> distinct;

      // owned by this call: the recipients of the message this delivery holds
      for (std::shared_ptr<MessageRecipient> recipient : message->GetRecipients()->GetVector())
      {
         if (recipient->GetLocalAccountID() <= 0)
            continue;

         const String original = recipient->GetOriginalAddress().IsEmpty() ? recipient->GetAddress() : recipient->GetOriginalAddress();
         if (distinct.insert(Canonical(original)).second)
            localAddresses.push_back(original);
      }

      // Addressed to a report mailbox and to nothing else, for the reason the
      // complaint mailbox is: a reporter posts to the one address the rua= tag
      // names, and a message carrying a report to that address AND to somebody
      // else is somebody making this server read a report on the way past.
      if (localAddresses.size() != 1)
         return;

      __int64 intakeDomainID = 0;
      if (!IntakeDomainFor_(localAddresses[0], intakeDomainID))
         return;

      const String fileName = MessageStore::WorkingFile(message);

      AnsiString raw;
      if (!ReadFile_(fileName, raw))
      {
         LOG_APPLICATION(Formatter::Format(_T("DomainReports: message {0} to the report mailbox {1} could not be read, or is larger than 32 MB; it is delivered unchanged and nothing is recorded."),
            message->GetID(), localAddresses[0]));
         return;
      }

      std::vector<Candidate> candidates;
      if (!FindCandidates(raw, candidates) || candidates.empty())
      {
         LOG_APPLICATION(Formatter::Format(_T("DomainReports: message {0} to the report mailbox {1} carries no DMARC aggregate or TLS-RPT report this server can read; it is delivered unchanged and nothing is recorded."),
            message->GetID(), localAddresses[0]));
         return;
      }

      // What the inbound path made of the message itself: empty unless this
      // server wrote an Authentication-Results field on it, in which case the
      // dmarc= verdict and the From domain it was about are kept on the row,
      // so a reporter whose messages fail DMARC can be told from one whose
      // messages pass.
      AnsiString messageDmarc, messageFrom;
      ReadMessageDmarc(raw, AuthenticationResults::GetAuthservId(), messageDmarc, messageFrom);

      // The ceilings: what a message may add, and what a domain has taken in
      // over the last day, read once per domain the message turns out to be
      // about. The mailbox address is published, so anybody may post to it,
      // and without these one message could add tens of thousands of rows and
      // a day of them could fill the disk.
      int recordedThisMessage = 0;

      struct Taken
      {
         __int64 dmarcRecords;
         __int64 tlsRows;
      };
      std::map<__int64, Taken> taken;

      auto takenFor = [&taken] (__int64 domainID, Taken *&entry) -> bool
      {
         std::map<__int64, Taken>::iterator found = taken.find(domainID);
         if (found == taken.end())
         {
            Taken fresh;
            if (!TakenInLastDay_(domainID, fresh.dmarcRecords, fresh.tlsRows))
               return false;

            found = taken.insert(std::make_pair(domainID, fresh)).first;
         }

         entry = &found->second;
         return true;
      };

      for (const Candidate &candidate : candidates)
      {
         const String name = FromUtf8(candidate.name);

         AnsiString content;
         AnsiString problem;

         if (!Unwrap(candidate.bytes, content, problem))
         {
            LOG_APPLICATION(Formatter::Format(_T("DomainReports: the attachment {0} of message {1} could not be unwrapped: {2}. It is delivered unchanged and nothing is recorded."),
               name, message->GetID(), FromUtf8(problem)));
            continue;
         }

         // Which kind, from the first character that is not white space.
         size_t at = 0;
         const std::string &text = content;
         if (text.size() >= 3 && (unsigned char) text[0] == 0xEF && (unsigned char) text[1] == 0xBB && (unsigned char) text[2] == 0xBF)
            at = 3;
         while (at < text.size() && isspace((unsigned char) text[at]))
            at++;

         if (at < text.size() && text[at] == '<')
         {
            DmarcReport report;
            if (!ParseDmarcXml(content, report, problem))
            {
               LOG_APPLICATION(Formatter::Format(_T("DomainReports: the attachment {0} of message {1} is not a DMARC aggregate report this server can read: {2}. It is delivered unchanged and nothing is recorded."),
                  name, message->GetID(), FromUtf8(problem)));
               continue;
            }

            const __int64 domainID = HostedDomainID_(report.policy_domain);
            if (domainID <= 0)
            {
               // A report about somebody else's domain is not this server's to
               // record: no row is written, so no report from outside can
               // create one for a domain that is not here - or for one that is
               // here and has named no mailbox.
               LOG_APPLICATION(Formatter::Format(_T("DomainReports: the DMARC aggregate report in message {0} is about {1}, which is not a domain this server hosts with a report mailbox; it is delivered unchanged and nothing is recorded."),
                  message->GetID(), FromUtf8(report.policy_domain)));
               continue;
            }

            const int records = (int) report.records.size();

            if (recordedThisMessage + records > MaxRecordsPerMessage)
            {
               LOG_APPLICATION(Formatter::Format(_T("DomainReports: the DMARC aggregate report in the attachment {0} of message {1} is not recorded: the message has already added {2} record(s), and one message may add at most {3}. It is delivered unchanged."),
                  name, message->GetID(), recordedThisMessage, (int) MaxRecordsPerMessage));
               continue;
            }

            Taken *today = nullptr;
            if (!takenFor(domainID, today))
            {
               ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6804, "DomainReports::InspectIncomingReport",
                  Formatter::Format(_T("What domain {0} has taken in today could not be read from the database, so the DMARC aggregate report from {1} is not recorded; the message is in the report mailbox."),
                     FromUtf8(report.policy_domain), FromUtf8(report.org_name)));
               continue;
            }

            if (today->dmarcRecords + records > MaxRecordsPerDomainPerDay)
            {
               if (ShouldLogCeiling_(domainID))
               {
                  LOG_APPLICATION(Formatter::Format(_T("DomainReports: {0} has taken in {1} DMARC record(s) in the last twenty-four hours, and takes in at most {2}; the report from {3} in message {4} is not recorded, nor any other until the day moves on. A genuine day of reports is far under this, so somebody is posting to the report mailbox on purpose. This is said at most once an hour per domain."),
                     FromUtf8(report.policy_domain), today->dmarcRecords, (int) MaxRecordsPerDomainPerDay, FromUtf8(report.org_name), message->GetID()));
               }
               continue;
            }

            __int64 rowID = 0;
            const int inserted = RecordDmarc_(domainID, report, candidate.name, messageDmarc, messageFrom, rowID);

            if (inserted < 0)
            {
               ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6800, "DomainReports::InspectIncomingReport",
                  Formatter::Format(_T("A DMARC aggregate report from {0} about {1} could not be recorded in the database; the message is in the report mailbox and nothing is charted from it."),
                     FromUtf8(report.org_name), FromUtf8(report.policy_domain)));
               continue;
            }

            if (inserted == 0)
            {
               LOG_DEBUG(Formatter::Format(_T("DomainReports: the DMARC aggregate report in message {0} has already been recorded; it is counted once."),
                  message->GetID()));
               continue;
            }

            recordedThisMessage += records;
            today->dmarcRecords += records;

            __int64 messages = 0, passed = 0;
            for (const DmarcRecord &record : report.records)
            {
               messages += record.count;
               if (record.dkim == "pass" || record.spf == "pass")
                  passed += record.count;
            }

            const String counts = Formatter::Format(_T("{0} record(s), {1} message(s), {2} passing DMARC"),
               (__int64) report.records.size(), messages, passed);

            LOG_APPLICATION(Formatter::Format(_T("DomainReports: a DMARC aggregate report from {0} about {1} covering {2} was recorded: {3}."),
               FromUtf8(report.org_name), FromUtf8(report.policy_domain), FromUtf8(DayOf(report.begin) + " to " + DayOf(report.end)), counts));

            if (report.dropped_records > 0)
            {
               // The schema has nowhere to say "and N more" - a chart drawn from
               // the rows kept would under-count. Saying so here is the only
               // honest option, as the reporter says of its own cap.
               LOG_APPLICATION(Formatter::Format(_T("DomainReports: that report holds more than {0} records, or records with no source; {1} of them were not kept, so the chart under-counts it."),
                  (int) MaxRecordsPerReport, report.dropped_records));
            }
         }
         else if (at < text.size() && text[at] == '{')
         {
            TlsReport report;
            if (!ParseTlsJson(content, report, problem))
            {
               LOG_APPLICATION(Formatter::Format(_T("DomainReports: the attachment {0} of message {1} is not a TLS-RPT report this server can read: {2}. It is delivered unchanged and nothing is recorded."),
                  name, message->GetID(), FromUtf8(problem)));
               continue;
            }

            for (const TlsPolicyResult &policy : report.policies)
            {
               const __int64 domainID = HostedDomainID_(policy.policy_domain);
               if (domainID <= 0)
               {
                  LOG_APPLICATION(Formatter::Format(_T("DomainReports: the TLS-RPT report in message {0} is about {1}, which is not a domain this server hosts with a report mailbox; it is delivered unchanged and nothing is recorded for it."),
                     message->GetID(), FromUtf8(policy.policy_domain)));
                  continue;
               }

               Taken *today = nullptr;
               if (!takenFor(domainID, today))
               {
                  ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6804, "DomainReports::InspectIncomingReport",
                     Formatter::Format(_T("What domain {0} has taken in today could not be read from the database, so the TLS-RPT report from {1} is not recorded; the message is in the report mailbox."),
                        FromUtf8(policy.policy_domain), FromUtf8(report.org_name)));
                  continue;
               }

               if (today->tlsRows + 1 > MaxTlsRowsPerDomainPerDay)
               {
                  if (ShouldLogCeiling_(domainID))
                  {
                     LOG_APPLICATION(Formatter::Format(_T("DomainReports: {0} has taken in {1} TLS-RPT row(s) in the last twenty-four hours, and takes in at most {2}; the report from {3} in message {4} is not recorded, nor any other until the day moves on. This is said at most once an hour per domain."),
                        FromUtf8(policy.policy_domain), today->tlsRows, (int) MaxTlsRowsPerDomainPerDay, FromUtf8(report.org_name), message->GetID()));
                  }
                  continue;
               }

               const int inserted = RecordTls_(domainID, report, policy, candidate.name);

               if (inserted < 0)
               {
                  ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6801, "DomainReports::InspectIncomingReport",
                     Formatter::Format(_T("A TLS-RPT report from {0} about {1} could not be recorded in the database; the message is in the report mailbox and nothing is shown from it."),
                        FromUtf8(report.org_name), FromUtf8(policy.policy_domain)));
                  continue;
               }

               if (inserted == 0)
               {
                  LOG_DEBUG(Formatter::Format(_T("DomainReports: the TLS-RPT report in message {0} has already been recorded; it is counted once."),
                     message->GetID()));
                  continue;
               }

               today->tlsRows++;

               const String sessions = Formatter::Format(_T("{0} session(s) succeeded, {1} failed"), (__int64) policy.successful, (__int64) policy.failed);

               LOG_APPLICATION(Formatter::Format(_T("DomainReports: a TLS-RPT report from {0} about {1} ({2}) covering {3} was recorded: {4}."),
                  FromUtf8(report.org_name), FromUtf8(policy.policy_domain), FromUtf8(policy.policy_type),
                  FromUtf8(DayOf(report.begin) + " to " + DayOf(report.end)), sessions));
            }
         }
         else
         {
            LOG_APPLICATION(Formatter::Format(_T("DomainReports: the attachment {0} of message {1} is neither the XML of a DMARC aggregate report nor the JSON of a TLS-RPT report; it is delivered unchanged and nothing is recorded."),
               name, message->GetID()));
         }
      }
   }

   // ------------------------------------------------------------------- answers

   bool
   DomainReports::SummaryJson(__int64 domainID, int days, AnsiString &json)
   {
      if (days < 1)
         days = 30;
      if (days > 3650)
         days = 3650;

      std::shared_ptr<const Domain> domain = CacheContainer::Instance()->GetDomain(domainID);
      if (!domain)
         return false;

      const __int64 now = Now();
      const __int64 since = now - (__int64) days * 86400;

      // The reports in the window: a report counts when any of its range is
      // inside it, and its records are charted on the day it begins.
      struct ReportRow
      {
         __int64 begin = 0;
         AnsiString org;
      };

      // Per reporter: how many reports and messages, and of the reports how
      // many arrived in a message this server's inbound path evaluated for
      // DMARC at all (checked) and how many of those passed (authenticated) -
      // which is what tells a reporter whose mail is what it says from one
      // whose is not, when AuthenticationResultsEnabled is on.
      struct Reporter
      {
         Reporter() : reports(0), messages(0), checked(0), authenticated(0) { }

         __int64 reports, messages, checked, authenticated;
      };

      std::map<__int64, ReportRow> reports;
      std::map<AnsiString, Reporter> reporters;

      {
         SQLCommand command(SelectWithLimit(_T("drid, drbegin, drorgname, drmessages, drmsgdmarc"), _T("hm_dmarcreports"),
            _T("drdomainid = @DOMAINID and drend >= @SINCE"), _T("drid"), MaxSummaryRecords));
         command.AddParameter("@DOMAINID", domainID);
         command.AddParameter("@SINCE", since);

         std::shared_ptr<DALRecordset> recordset = Db()->OpenRecordset(command);
         if (!recordset)
            return false;

         while (!recordset->IsEOF())
         {
            ReportRow row;
            row.begin = recordset->GetInt64Value("drbegin");
            row.org = Utf8(recordset->GetStringValue("drorgname"));
            reports[recordset->GetInt64Value("drid")] = row;

            Reporter &reporter = reporters[row.org];
            reporter.reports++;
            reporter.messages += recordset->GetInt64Value("drmessages");

            const String verdict = recordset->GetStringValue("drmsgdmarc");
            if (!verdict.IsEmpty())
               reporter.checked++;
            if (verdict == _T("pass"))
               reporter.authenticated++;

            recordset->MoveNext();
         }
      }

      struct Tally
      {
         Tally() : messages(0), passed(0), failed(0), dkim(0), spf(0), quarantined(0), rejected(0) { }

         __int64 messages, passed, failed, dkim, spf, quarantined, rejected;
         std::set<AnsiString> reporters;
         AnsiString header_from;
      };

      Tally total;
      std::map<AnsiString, Tally> bySource;
      std::map<AnsiString, Tally> byDay;

      if (!reports.empty())
      {
         SQLCommand command(SelectWithLimit(
            _T("r.rrreportid as rrreportid, r.rrsourceip as rrsourceip, r.rrcount as rrcount, r.rrdisposition as rrdisposition, r.rrdkim as rrdkim, r.rrspf as rrspf, r.rrheaderfrom as rrheaderfrom"),
            _T("hm_dmarcreportrecords r inner join hm_dmarcreports p on p.drid = r.rrreportid"),
            _T("p.drdomainid = @DOMAINID and p.drend >= @SINCE"), _T("r.rrid"), MaxSummaryRecords));
         command.AddParameter("@DOMAINID", domainID);
         command.AddParameter("@SINCE", since);

         std::shared_ptr<DALRecordset> recordset = Db()->OpenRecordset(command);
         if (!recordset)
            return false;

         while (!recordset->IsEOF())
         {
            const __int64 reportID = recordset->GetInt64Value("rrreportid");
            std::map<__int64, ReportRow>::const_iterator report = reports.find(reportID);

            if (report != reports.end())
            {
               const __int64 count = recordset->GetInt64Value("rrcount");
               const AnsiString ip = Utf8(recordset->GetStringValue("rrsourceip"));
               const AnsiString disposition = Utf8(recordset->GetStringValue("rrdisposition"));
               const bool dkim = recordset->GetStringValue("rrdkim") == _T("pass");
               const bool spf = recordset->GetStringValue("rrspf") == _T("pass");
               const AnsiString headerFrom = Utf8(recordset->GetStringValue("rrheaderfrom"));
               const AnsiString day = DayOf(report->second.begin);

               Tally *tallies[3] = { &total, &bySource[ip], &byDay[day] };
               for (Tally *tally : tallies)
               {
                  tally->messages += count;
                  if (dkim || spf)
                     tally->passed += count;
                  else
                     tally->failed += count;
                  if (dkim)
                     tally->dkim += count;
                  if (spf)
                     tally->spf += count;
                  if (disposition == "quarantine")
                     tally->quarantined += count;
                  else if (disposition == "reject")
                     tally->rejected += count;
                  tally->reporters.insert(report->second.org);
                  if (tally->header_from.IsEmpty())
                     tally->header_from = headerFrom;
               }
            }

            recordset->MoveNext();
         }
      }

      // The sources, most messages first, and the reporters likewise.
      std::vector<std::pair<AnsiString, const Tally *> > sources;
      for (const auto &entry : bySource)
         sources.push_back(std::make_pair(entry.first, &entry.second));

      std::sort(sources.begin(), sources.end(), [] (const std::pair<AnsiString, const Tally *> &a, const std::pair<AnsiString, const Tally *> &b)
      {
         if (a.second->messages != b.second->messages)
            return a.second->messages > b.second->messages;
         return a.first < b.first;
      });

      std::vector<std::pair<AnsiString, Reporter> > reporterList(reporters.begin(), reporters.end());
      std::sort(reporterList.begin(), reporterList.end(), [] (const std::pair<AnsiString, Reporter> &a, const std::pair<AnsiString, Reporter> &b)
      {
         if (a.second.messages != b.second.messages)
            return a.second.messages > b.second.messages;
         return a.first < b.first;
      });

      json = "{" + JsonField("domain", domain->GetName()) + "," +
             JsonNumber("days", days) + "," +
             JsonNumber("since", since) + "," +
             JsonNumber("until", now) + "," +
             JsonField("report_mailbox", domain->GetReportMailbox()) + "," +
             JsonBool("mailbox_configured", !domain->GetReportMailbox().IsEmpty()) + "," +
             JsonNumber("reports", (__int64) reports.size()) + "," +
             JsonNumber("messages", total.messages) + "," +
             JsonNumber("passed", total.passed) + "," +
             JsonNumber("failed", total.failed) + "," +
             JsonNumber("dkim_passed", total.dkim) + "," +
             JsonNumber("spf_passed", total.spf) + "," +
             JsonNumber("quarantined", total.quarantined) + "," +
             JsonNumber("rejected", total.rejected) + ",\"reporters\":[";

      for (size_t i = 0; i < reporterList.size(); i++)
      {
         if (i > 0)
            json += ",";
         json += "{" + JsonField("org", reporterList[i].first) + "," +
                 JsonNumber("reports", reporterList[i].second.reports) + "," +
                 JsonNumber("messages", reporterList[i].second.messages) + "," +
                 JsonNumber("checked", reporterList[i].second.checked) + "," +
                 JsonNumber("authenticated", reporterList[i].second.authenticated) + "}";
      }

      // One entry per day of the window when the window is a year or less, so
      // that the chart shows the days nothing arrived as gaps rather than
      // closing them up; a longer window lists only the days that have rows.
      json += "],\"by_day\":[";

      bool first = true;
      if (days <= 400)
      {
         const __int64 firstDay = since / 86400;
         const __int64 lastDay = now / 86400;

         for (__int64 d = firstDay; d <= lastDay; d++)
         {
            const AnsiString day = DayOf(d * 86400);
            std::map<AnsiString, Tally>::const_iterator found = byDay.find(day);
            const Tally empty;
            const Tally &tally = found == byDay.end() ? empty : found->second;

            if (!first)
               json += ",";
            first = false;

            json += "{" + JsonField("day", day) + "," + JsonNumber("messages", tally.messages) + "," +
                    JsonNumber("passed", tally.passed) + "," + JsonNumber("failed", tally.failed) + "}";
         }
      }
      else
      {
         for (const auto &entry : byDay)
         {
            if (!first)
               json += ",";
            first = false;

            json += "{" + JsonField("day", entry.first) + "," + JsonNumber("messages", entry.second.messages) + "," +
                    JsonNumber("passed", entry.second.passed) + "," + JsonNumber("failed", entry.second.failed) + "}";
         }
      }

      json += "],\"sources\":[";

      for (size_t i = 0; i < sources.size() && (int) i < MaxSummarySources; i++)
      {
         const Tally &tally = *sources[i].second;

         if (i > 0)
            json += ",";

         json += "{" + JsonField("ip", sources[i].first) + "," +
                 JsonNumber("messages", tally.messages) + "," +
                 JsonNumber("passed", tally.passed) + "," +
                 JsonNumber("failed", tally.failed) + "," +
                 JsonNumber("dkim_passed", tally.dkim) + "," +
                 JsonNumber("spf_passed", tally.spf) + "," +
                 JsonNumber("quarantined", tally.quarantined) + "," +
                 JsonNumber("rejected", tally.rejected) + "," +
                 JsonField("header_from", tally.header_from) + "," +
                 JsonNumber("reporters", (__int64) tally.reporters.size()) + "}";
      }

      AnsiString tail;
      tail.Format("],\"sources_total\":%d}", (int) sources.size());
      json += tail;

      return true;
   }

   namespace
   {
      AnsiString ReportItemJson(std::shared_ptr<DALRecordset> recordset)
      {
         AnsiString item = "{" + JsonNumber("id", recordset->GetInt64Value("drid")) + "," +
                           JsonField("org", recordset->GetStringValue("drorgname")) + "," +
                           JsonField("email", recordset->GetStringValue("dremail")) + "," +
                           JsonField("report_id", recordset->GetStringValue("drreportid")) + "," +
                           JsonNumber("begin", recordset->GetInt64Value("drbegin")) + "," +
                           JsonNumber("end", recordset->GetInt64Value("drend")) + "," +
                           JsonField("policy_domain", recordset->GetStringValue("drpolicydomain")) + "," +
                           JsonField("adkim", recordset->GetStringValue("dradkim")) + "," +
                           JsonField("aspf", recordset->GetStringValue("draspf")) + "," +
                           JsonField("p", recordset->GetStringValue("drp")) + "," +
                           JsonField("sp", recordset->GetStringValue("drsp")) + "," +
                           JsonNumber("pct", recordset->GetLongValue("drpct")) + "," +
                           JsonNumber("messages", recordset->GetLongValue("drmessages")) + "," +
                           JsonNumber("passed", recordset->GetLongValue("drpassed")) + "," +
                           JsonNumber("received", recordset->GetInt64Value("drreceived")) + "," +
                           JsonField("source", recordset->GetStringValue("drsource")) + "," +
                           JsonNumber("records", recordset->GetLongValue("drrecords")) + "," +
                           JsonField("message_dmarc", recordset->GetStringValue("drmsgdmarc")) + "," +
                           JsonField("message_from", recordset->GetStringValue("drmsgfrom"));
         return item;
      }

      const wchar_t *ReportColumns =
         L"drid, drorgname, dremail, drreportid, drbegin, drend, drpolicydomain, dradkim, draspf, drp, drsp, drpct, drmessages, drpassed, drreceived, drsource, drrecords, drmsgdmarc, drmsgfrom";
   }

   bool
   DomainReports::ReportsJson(__int64 domainID, int limit, int offset, AnsiString &json)
   {
      if (limit < 1) limit = 1;
      if (limit > 1000) limit = 1000;
      if (offset < 0) offset = 0;

      std::shared_ptr<const Domain> domain = CacheContainer::Instance()->GetDomain(domainID);
      if (!domain)
         return false;

      SQLCommand countParameters;
      countParameters.AddParameter("@DOMAINID", domainID);
      const int total = CountWhere(_T("hm_dmarcreports"), _T("drdomainid = @DOMAINID"), countParameters);
      if (total < 0)
         return false;

      json = "{" + JsonField("domain", domain->GetName()) + "," +
             JsonField("report_mailbox", domain->GetReportMailbox()) + "," +
             JsonBool("mailbox_configured", !domain->GetReportMailbox().IsEmpty()) + "," +
             JsonNumber("total", total) + "," + JsonNumber("offset", offset) + ",\"items\":[";

      if (offset < total)
      {
         SQLCommand command(SelectWithLimit(ReportColumns, _T("hm_dmarcreports"), _T("drdomainid = @DOMAINID"), _T("drid desc"), offset + limit));
         command.AddParameter("@DOMAINID", domainID);

         std::shared_ptr<DALRecordset> recordset = Db()->OpenRecordset(command);
         if (!recordset)
            return false;

         int index = 0, written = 0;
         while (!recordset->IsEOF() && written < limit)
         {
            if (index >= offset)
            {
               if (written > 0)
                  json += ",";
               json += ReportItemJson(recordset) + "}";
               written++;
            }

            index++;
            recordset->MoveNext();
         }
      }

      json += "]}";
      return true;
   }

   bool
   DomainReports::ReportJson(__int64 domainID, __int64 reportID, bool &found, AnsiString &json)
   {
      found = false;

      SQLCommand command(String(_T("select ")) + ReportColumns + _T(" from hm_dmarcreports where drid = @ID and drdomainid = @DOMAINID"));
      command.AddParameter("@ID", reportID);
      command.AddParameter("@DOMAINID", domainID);

      std::shared_ptr<DALRecordset> recordset = Db()->OpenRecordset(command);
      if (!recordset)
         return false;

      if (recordset->IsEOF())
         return true;

      found = true;
      json = ReportItemJson(recordset) + ",\"records\":[";

      SQLCommand records(SelectWithLimit(_T("rrsourceip, rrcount, rrdisposition, rrdkim, rrspf, rrheaderfrom, rrenvelopefrom, rrdkimauth, rrspfauth"),
         _T("hm_dmarcreportrecords"), _T("rrreportid = @ID"), _T("rrid"), MaxRecordsPerReport));
      records.AddParameter("@ID", reportID);

      std::shared_ptr<DALRecordset> rows = Db()->OpenRecordset(records);
      if (!rows)
         return false;

      bool first = true;
      while (!rows->IsEOF())
      {
         if (!first)
            json += ",";
         first = false;

         json += "{" + JsonField("source_ip", rows->GetStringValue("rrsourceip")) + "," +
                 JsonNumber("count", rows->GetLongValue("rrcount")) + "," +
                 JsonField("disposition", rows->GetStringValue("rrdisposition")) + "," +
                 JsonField("dkim", rows->GetStringValue("rrdkim")) + "," +
                 JsonField("spf", rows->GetStringValue("rrspf")) + "," +
                 JsonField("header_from", rows->GetStringValue("rrheaderfrom")) + "," +
                 JsonField("envelope_from", rows->GetStringValue("rrenvelopefrom")) + "," +
                 JsonField("dkim_auth", rows->GetStringValue("rrdkimauth")) + "," +
                 JsonField("spf_auth", rows->GetStringValue("rrspfauth")) + "}";

         rows->MoveNext();
      }

      json += "]}";
      return true;
   }

   bool
   DomainReports::TlsReportsJson(__int64 domainID, int days, int limit, int offset, AnsiString &json)
   {
      if (days < 0) days = 0;
      if (days > 3650) days = 3650;
      if (limit < 1) limit = 1;
      if (limit > 1000) limit = 1000;
      if (offset < 0) offset = 0;

      std::shared_ptr<const Domain> domain = CacheContainer::Instance()->GetDomain(domainID);
      if (!domain)
         return false;

      const __int64 now = Now();
      const __int64 since = days > 0 ? now - (__int64) days * 86400 : 0;

      // Every row of the window, newest first, so the session totals are the
      // window's and not the page's.
      SQLCommand command(SelectWithLimit(
         _T("trid, trorgname, trcontact, trreportid, trbegin, trend, trpolicytype, trpolicydomain, trsuccess, trfailure, trfailures, trreceived, trsource"),
         _T("hm_tlsreports"), _T("trdomainid = @DOMAINID and trend >= @SINCE"), _T("trid desc"), MaxTlsRows));
      command.AddParameter("@DOMAINID", domainID);
      command.AddParameter("@SINCE", since);

      std::shared_ptr<DALRecordset> recordset = Db()->OpenRecordset(command);
      if (!recordset)
         return false;

      std::vector<AnsiString> items;
      __int64 succeeded = 0, failed = 0;

      while (!recordset->IsEOF())
      {
         succeeded += recordset->GetLongValue("trsuccess");
         failed += recordset->GetLongValue("trfailure");

         AnsiString failures = Utf8(recordset->GetStringValue("trfailures"));
         if (failures.IsEmpty() || failures.GetAt(0) != '[')
            failures = "[]";

         items.push_back("{" + JsonNumber("id", recordset->GetInt64Value("trid")) + "," +
                         JsonField("org", recordset->GetStringValue("trorgname")) + "," +
                         JsonField("contact", recordset->GetStringValue("trcontact")) + "," +
                         JsonField("report_id", recordset->GetStringValue("trreportid")) + "," +
                         JsonNumber("begin", recordset->GetInt64Value("trbegin")) + "," +
                         JsonNumber("end", recordset->GetInt64Value("trend")) + "," +
                         JsonField("policy_type", recordset->GetStringValue("trpolicytype")) + "," +
                         JsonField("policy_domain", recordset->GetStringValue("trpolicydomain")) + "," +
                         JsonNumber("successful", recordset->GetLongValue("trsuccess")) + "," +
                         JsonNumber("failed", recordset->GetLongValue("trfailure")) + "," +
                         "\"failures\":" + failures + "," +
                         JsonNumber("received", recordset->GetInt64Value("trreceived")) + "," +
                         JsonField("source", recordset->GetStringValue("trsource")) + "}");

         recordset->MoveNext();
      }

      json = "{" + JsonField("domain", domain->GetName()) + "," +
             JsonNumber("days", days) + "," +
             JsonNumber("since", since) + "," +
             JsonBool("mailbox_configured", !domain->GetReportMailbox().IsEmpty()) + "," +
             JsonNumber("sessions_succeeded", succeeded) + "," +
             JsonNumber("sessions_failed", failed) + "," +
             JsonNumber("total", (__int64) items.size()) + "," +
             JsonNumber("offset", offset) + ",\"items\":[";

      for (size_t i = (size_t) offset, written = 0; i < items.size() && (int) written < limit; i++, written++)
      {
         if (written > 0)
            json += ",";
         json += items[i];
      }

      json += "]}";
      return true;
   }

   int
   DomainReports::Prune(int olderThanDays, int &dmarcPruned, int &tlsPruned, bool &complete)
   {
      dmarcPruned = 0;
      tlsPruned = 0;
      complete = true;

      if (olderThanDays < 0)
         return 0;

      const __int64 cutoff = Now() - (__int64) olderThanDays * 86400;

      // Candidates in one select and then deleted by id, so a pass is bounded
      // whatever the tables hold; a report's records go with it by the cascade.
      // Passes run until one finds nothing, or MaxPrunePasses have run - at
      // which point the answer says it is not complete rather than saying
      // "removed" of rows that are still there.
      struct Table
      {
         const wchar_t *name;
         const wchar_t *id;
         const char *idColumn;
         const wchar_t *received;
         int *count;
      };

      Table tables[2] =
      {
         { L"hm_dmarcreports", L"drid", "drid", L"drreceived", &dmarcPruned },
         { L"hm_tlsreports", L"trid", "trid", L"trreceived", &tlsPruned }
      };

      for (const Table &table : tables)
      {
         bool more = true;

         for (int pass = 0; more && pass < MaxPrunePasses; pass++)
         {
            SQLCommand candidates(SelectWithLimit(table.id, table.name, String(table.received) + _T(" <= @CUTOFF"), table.id, MaxPrunePerPass));
            candidates.AddParameter("@CUTOFF", cutoff);

            std::shared_ptr<DALRecordset> recordset = Db()->OpenRecordset(candidates);
            if (!recordset)
               return -1;

            std::vector<__int64> ids;
            while (!recordset->IsEOF())
            {
               ids.push_back(recordset->GetInt64Value(table.idColumn));
               recordset->MoveNext();
            }

            for (__int64 id : ids)
            {
               SQLCommand remove(String(_T("delete from ")) + table.name + _T(" where ") + table.id + _T(" = @ID"));
               remove.AddParameter("@ID", id);

               if (!Db()->Execute(remove))
                  return -1;

               (*table.count)++;
            }

            more = (int) ids.size() >= MaxPrunePerPass;
         }

         if (more)
            complete = false;
      }

      return dmarcPruned + tlsPruned;
   }

   void
   DomainReports::Sweep()
   {
      Configuration *configuration = Configuration::Instance();
      if (!configuration || !configuration->GetSettings())
         return;

      const int retention = configuration->GetDomainReportRetentionDays();
      if (retention <= 0)
         return;

      int dmarcPruned = 0, tlsPruned = 0;
      bool complete = true;

      if (Prune(retention, dmarcPruned, tlsPruned, complete) < 0)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6803, "DomainReports::Sweep",
            "The sweep of the DMARC and TLS-RPT reports failed part way, so reports older than DomainReportRetentionDays may remain until the next pass.");
         return;
      }

      if (dmarcPruned + tlsPruned > 0)
      {
         LOG_APPLICATION(Formatter::Format(_T("DomainReports: the sweep removed {0} DMARC aggregate report(s) and {1} TLS-RPT report(s) read more than {2} days ago{3}."),
            dmarcPruned, tlsPruned, retention, String(complete ? _T("") : _T("; more remain for the next pass"))));
      }
   }

   // ------------------------------------------------------------------- tester

   namespace
   {
      AnsiString Gzip(const AnsiString &plain)
      {
         z_stream stream;
         memset(&stream, 0, sizeof(stream));

         if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK)
            return AnsiString();

         std::vector<unsigned char> out(deflateBound(&stream, (uLong) plain.size()) + 32);
         stream.next_in = (Bytef *) plain.data();
         stream.avail_in = (uInt) plain.size();
         stream.next_out = out.data();
         stream.avail_out = (uInt) out.size();

         const int code = deflate(&stream, Z_FINISH);
         const size_t produced = out.size() - stream.avail_out;
         deflateEnd(&stream);

         if (code != Z_STREAM_END)
            return AnsiString();

         AnsiString result;
         result.append((const char *) out.data(), produced);
         return result;
      }

      // A zip archive holding one stored file, as a writer that streams would
      // leave it: the local header with no sizes, and the central directory
      // with them.
      AnsiString StoredZip(const AnsiString &name, const AnsiString &bytes)
      {
         AnsiString local;
         Put32(local, 0x04034b50);
         Put16(local, 20);
         Put16(local, 0x0008);       // bit 3: sizes follow the data, not the header
         Put16(local, 0);            // stored
         Put16(local, 0);
         Put16(local, 0);
         Put32(local, 0);
         Put32(local, 0);
         Put32(local, 0);
         Put16(local, (unsigned) name.size());
         Put16(local, 0);
         local += name;
         local += bytes;

         const unsigned long crc = (unsigned long) crc32(0L, (const Bytef *) bytes.data(), (uInt) bytes.size());

         AnsiString directory;
         Put32(directory, 0x02014b50);
         Put16(directory, 20);
         Put16(directory, 20);
         Put16(directory, 0x0008);
         Put16(directory, 0);
         Put16(directory, 0);
         Put16(directory, 0);
         Put32(directory, crc);
         Put32(directory, (unsigned long) bytes.size());
         Put32(directory, (unsigned long) bytes.size());
         Put16(directory, (unsigned) name.size());
         Put16(directory, 0);
         Put16(directory, 0);
         Put16(directory, 0);
         Put16(directory, 0);
         Put32(directory, 0);
         Put32(directory, 0);
         directory += name;

         AnsiString end;
         Put32(end, 0x06054b50);
         Put16(end, 0);
         Put16(end, 0);
         Put16(end, 1);
         Put16(end, 1);
         Put32(end, (unsigned long) directory.size());
         Put32(end, (unsigned long) local.size());
         Put16(end, 0);

         return local + directory + end;
      }

      const char *SampleReport =
         "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
         "<!-- a report as google.com sends one -->\n"
         "<feedback>\n"
         "  <report_metadata>\n"
         "    <org_name>google.com</org_name>\n"
         "    <email>noreply-dmarc-support@google.com</email>\n"
         "    <report_id>12345678901234567890</report_id>\n"
         "    <date_range><begin>1789430400</begin><end>1789516799</end></date_range>\n"
         "  </report_metadata>\n"
         "  <policy_published>\n"
         "    <domain>Example.COM</domain>\n"
         "    <adkim>r</adkim><aspf>r</aspf><p>quarantine</p><sp>reject</sp><pct>100</pct>\n"
         "  </policy_published>\n"
         "  <record>\n"
         "    <row><source_ip>192.0.2.10</source_ip><count>12</count>\n"
         "      <policy_evaluated><disposition>none</disposition><dkim>pass</dkim><spf>pass</spf></policy_evaluated></row>\n"
         "    <identifiers><header_from>example.com</header_from><envelope_from>example.com</envelope_from></identifiers>\n"
         "    <auth_results><dkim><domain>example.com</domain><selector>s1</selector><result>pass</result></dkim>\n"
         "      <spf><domain>example.com</domain><scope>mfrom</scope><result>pass</result></spf></auth_results>\n"
         "  </record>\n"
         "  <record>\n"
         "    <row><source_ip>198.51.100.7</source_ip><count>3</count>\n"
         "      <policy_evaluated><disposition>quarantine</disposition><dkim>fail</dkim><spf>fail</spf></policy_evaluated></row>\n"
         "    <identifiers><header_from>example.com</header_from></identifiers>\n"
         "    <auth_results><spf><domain>forger.test</domain><result>pass</result></spf></auth_results>\n"
         "  </record>\n"
         "</feedback>\n";
   }

   void
   DomainReportsTester::Test()
   {
      typedef DomainReports R;

      // ------------------------------------------------------------ the calendar
      Expect(R::DayOf(0) == "1970-01-01", "The epoch did not read as 1970-01-01.");
      Expect(R::DayOf(1789430400) == "2026-09-15", "A day in 2026 did not read as its date.");
      Expect(R::DayOf(951782400) == "2000-02-29", "A leap day did not read as its date.");
      Expect(R::ParseDateTime("2026-09-15T00:00:00Z") == 1789430400, "An RFC 3339 midnight was not read.");
      Expect(R::ParseDateTime("2026-09-15T23:59:59Z") == 1789516799, "The end of a day was not read.");
      Expect(R::ParseDateTime("2026-09-15T02:00:00+02:00") == 1789430400, "An offset was not applied.");
      Expect(R::ParseDateTime("2026-09-15T00:00:00.5Z") == 1789430400, "A fraction of a second was not skipped.");
      Expect(R::ParseDateTime("2026-09-15") == 1789430400, "A bare date was not read as midnight.");
      Expect(R::ParseDateTime("not a date") == -1, "A date that is not one was not refused.");
      Expect(R::ParseDateTime("2026-13-01T00:00:00Z") == -1, "A thirteenth month was not refused.");

      // ---------------------------------------------------------------- the XML
      R::DmarcReport report;
      AnsiString problem;

      Expect(R::ParseDmarcXml(AnsiString(SampleReport), report, problem), ("A well-formed aggregate report was not read: " + problem).c_str());
      Expect(report.org_name == "google.com", "The reporter was not read.");
      Expect(report.report_id == "12345678901234567890", "The report id was not read.");
      Expect(report.begin == 1789430400 && report.end == 1789516799, "The date range was not read.");
      Expect(report.policy_domain == "example.com", "The policy domain was not lower-cased.");
      Expect(report.p == "quarantine" && report.sp == "reject" && report.pct == 100, "The policy was not read.");
      Expect(report.records.size() == 2, "The two records were not read as two.");
      Expect(report.records[0].source_ip == "192.0.2.10" && report.records[0].count == 12, "The first record's source and count were not read.");
      Expect(report.records[0].dkim == "pass" && report.records[0].spf == "pass" && report.records[0].disposition == "none", "The first record's verdicts were not read.");
      Expect(report.records[0].dkim_auth == "d=example.com s=s1 result=pass", "The DKIM authentication result was not read.");
      Expect(report.records[0].spf_auth == "d=example.com scope=mfrom result=pass", "The SPF authentication result was not read.");
      Expect(report.records[1].disposition == "quarantine" && report.records[1].dkim == "fail", "The second record was not read.");
      Expect(report.records[1].spf_auth == "d=forger.test scope= result=pass", "An SPF result with no scope was not read.");

      // The RFC 9990 form: a namespace on every element, and a prefix.
      R::DmarcReport namespaced;
      Expect(R::ParseDmarcXml(AnsiString(
         "<?xml version=\"1.0\"?><d:feedback xmlns:d=\"urn:ietf:params:xml:ns:dmarc-2.0\"><d:version>1.0</d:version>"
         "<d:report_metadata><d:org_name>Reporter &amp; Co</d:org_name><d:report_id>r1</d:report_id>"
         "<d:date_range><d:begin>1789430400</d:begin><d:end>1789516799</d:end></d:date_range></d:report_metadata>"
         "<d:policy_published><d:domain>example.com</d:domain><d:p>none</d:p></d:policy_published>"
         "<d:record><d:row><d:source_ip>2001:db8::1</d:source_ip><d:count>1</d:count>"
         "<d:policy_evaluated><d:disposition>pass</d:disposition><d:dkim>pass</d:dkim><d:spf>fail</d:spf></d:policy_evaluated></d:row>"
         "<d:identifiers><d:header_from>example.com</d:header_from></d:identifiers></d:record></d:feedback>"), namespaced, problem),
         ("A namespaced report was not read: " + problem).c_str());
      Expect(namespaced.org_name == "Reporter & Co", "An entity in the reporter's name was not decoded.");
      Expect(namespaced.records.size() == 1 && namespaced.records[0].source_ip == "2001:db8::1", "The IPv6 source was not read.");
      Expect(namespaced.pct == 100, "A report with no pct did not read as 100.");

      // A CDATA section, a numeric reference and a self-closing element.
      R::DmarcReport odd;
      Expect(R::ParseDmarcXml(AnsiString(
         "<feedback><report_metadata><org_name><![CDATA[a <b> c]]></org_name><report_id>&#x41;&#66;</report_id>"
         "<date_range><begin>1</begin><end>2</end></date_range><extra/></report_metadata>"
         "<policy_published><domain>example.com</domain></policy_published>"
         "<record><row><source_ip>192.0.2.1</source_ip><count>1</count></row></record></feedback>"), odd, problem),
         ("A report with CDATA and references was not read: " + problem).c_str());
      Expect(odd.org_name == "a <b> c", "A CDATA section was not read as its text.");
      Expect(odd.report_id == "AB", "Numeric character references were not decoded.");

      // What is refused.
      R::DmarcReport refused;
      Expect(!R::ParseDmarcXml(AnsiString("<!DOCTYPE feedback [<!ENTITY x \"y\">]><feedback><report_metadata/></feedback>"), refused, problem) &&
             problem.Find("entities") >= 0, "A document type declaration with an entity was not refused.");
      Expect(!R::ParseDmarcXml(AnsiString("<report><nothing/></report>"), refused, problem), "A document whose root is not feedback was read.");
      Expect(!R::ParseDmarcXml(AnsiString("<feedback><report_metadata><report_id>x</report_id></report_metadata><policy_published><domain>a.test</domain></policy_published></feedback>"), refused, problem),
             "A report with no record was read.");
      Expect(!R::ParseDmarcXml(AnsiString("<feedback><report_metadata><report_id>x</report_id></report_metadata><policy_published></policy_published><record><row><source_ip>1.2.3.4</source_ip></row></record></feedback>"), refused, problem),
             "A report whose policy names no domain was read.");
      Expect(!R::ParseDmarcXml(AnsiString(""), refused, problem), "An empty document was read.");
      Expect(!R::ParseDmarcXml(AnsiString("<feedback><a"), refused, problem), "An unterminated tag was read.");

      // A policy domain that is not a host name is refused before any lookup:
      // the domain cache asserts on an address where a name should be.
      Expect(!R::ParseDmarcXml(AnsiString("<feedback><report_metadata><report_id>x</report_id><date_range><begin>1</begin><end>2</end></date_range></report_metadata>"
                                          "<policy_published><domain>dmarc@example.com</domain></policy_published>"
                                          "<record><row><source_ip>1.2.3.4</source_ip><count>1</count></row></record></feedback>"), refused, problem) &&
             problem.Find("host name") >= 0, "A report whose policy domain is an address was read.");
      Expect(R::IsHostName("example.com") && R::IsHostName("xn--bcher-kva.example") && R::IsHostName("a-b.c"), "A plain host name was refused.");
      Expect(!R::IsHostName("dmarc@example.com") && !R::IsHostName("") && !R::IsHostName(".example.com") && !R::IsHostName("example..com") &&
             !R::IsHostName("example.com.") && !R::IsHostName("exa mple.com") && !R::IsHostName(AnsiString(std::string(254, 'a'))),
             "Something that is not a host name was taken for one.");

      // The cap on records: the 2,001st is counted and not kept.
      {
         AnsiString many = "<feedback><report_metadata><org_name>big.test</org_name><report_id>big</report_id><date_range><begin>1</begin><end>2</end></date_range></report_metadata>"
                           "<policy_published><domain>example.com</domain></policy_published>";
         for (int i = 0; i < R::MaxRecordsPerReport + 1; i++)
         {
            AnsiString record;
            record.Format("<record><row><source_ip>10.0.%d.%d</source_ip><count>1</count></row></record>", i / 256, i % 256);
            many += record;
         }
         many += "</feedback>";

         R::DmarcReport big;
         Expect(R::ParseDmarcXml(many, big, problem), ("A report at the cap was not read: " + problem).c_str());
         Expect((int) big.records.size() == R::MaxRecordsPerReport && big.dropped_records == 1,
                "A report one record past the cap did not keep the cap and count the one past it.");
      }

      // ------------------------------------------------ the carrying message
      {
         AnsiString verdict, from;
         const AnsiString carried =
            "Return-Path: <noreply-dmarc-support@google.com>\r\n"
            "Authentication-Results: mail.example.com;\r\n"
            "\tspf=pass smtp.mailfrom=noreply-dmarc-support@google.com;\r\n"
            "\tdkim=pass header.d=google.com;\r\n"
            "\tdmarc=pass (p=reject) header.from=google.com\r\n"
            "From: noreply-dmarc-support@google.com\r\n"
            "To: dmarc@example.com\r\n"
            "\r\n"
            "Authentication-Results: mail.example.com; dmarc=fail header.from=body.test\r\n";

         R::ReadMessageDmarc(carried, "mail.example.com", verdict, from);
         Expect(verdict == "pass" && from == "google.com", "The dmarc= verdict and header.from= of this server's own field were not read.");

         R::ReadMessageDmarc(carried, "Mail.Example.COM", verdict, from);
         Expect(verdict == "pass", "The authserv-id was compared with case.");

         R::ReadMessageDmarc(carried, "other.example", verdict, from);
         Expect(verdict.IsEmpty() && from.IsEmpty(), "A field bearing another server's authserv-id was read as ours.");

         // A quoted authserv-id, a field with no dmarc= method, and none at all.
         R::ReadMessageDmarc(AnsiString("Authentication-Results: \"mail.example.com\"; dmarc=none header.from=stranger.test\r\nFrom: a@b\r\n\r\n"), "mail.example.com", verdict, from);
         Expect(verdict == "none" && from == "stranger.test", "A quoted authserv-id was not unquoted.");

         R::ReadMessageDmarc(AnsiString("Authentication-Results: mail.example.com; spf=pass smtp.mailfrom=a@b.test\r\n\r\n"), "mail.example.com", verdict, from);
         Expect(verdict.IsEmpty() && from.IsEmpty(), "A field with no dmarc= method produced a verdict.");

         R::ReadMessageDmarc(AnsiString("From: a@b.test\r\nTo: c@d.test\r\n\r\nAuthentication-Results: mail.example.com; dmarc=pass\r\n"), "mail.example.com", verdict, from);
         Expect(verdict.IsEmpty(), "A field in the body was read as a header.");

         // A forged field with our id ahead of ours is the writer's to strip;
         // here the first field bearing the id is the one taken, which is ours
         // once the writer has run.
         R::ReadMessageDmarc(AnsiString("Authentication-Results: mail.example.com; dmarc=fail header.from=x.test\r\nAuthentication-Results: mail.example.com; dmarc=pass header.from=y.test\r\n\r\n"), "mail.example.com", verdict, from);
         Expect(verdict == "fail" && from == "x.test", "The first field bearing the id was not the one taken.");
      }

      // ------------------------------------------------------------ the wrappers
      AnsiString content;
      const AnsiString sample(SampleReport);

      const AnsiString gz = Gzip(sample);
      Expect(!gz.IsEmpty() && (unsigned char) gz.GetAt(0) == 0x1f, "The test's gzip was not made.");
      Expect(R::Unwrap(gz, content, problem) && content == sample, ("A gzip attachment was not inflated: " + problem).c_str());

      const AnsiString zip = StoredZip("google.com!example.com!1789430400!1789516799.xml", sample);
      Expect(R::Unwrap(zip, content, problem) && content == sample, ("A zip attachment with a streamed local header was not read: " + problem).c_str());

      Expect(R::Unwrap(sample, content, problem) && content == sample, "A plain attachment was not passed through.");

      AnsiString damaged = gz;
      damaged = damaged.Left(damaged.GetLength() / 2);
      Expect(!R::Unwrap(damaged, content, problem), "A truncated gzip was read.");

      AnsiString notZip = "PK\x03\x04" + AnsiString(std::string(30, 'x'));
      Expect(!R::Unwrap(notZip, content, problem), "Bytes that only begin like a zip were read.");

      // ------------------------------------------------------------ the message
      std::vector<R::Candidate> candidates;
      const AnsiString base64 = Base64::Encode(gz.c_str(), gz.GetLength());

      Expect(R::FindCandidates(AnsiString(
         "From: noreply-dmarc-support@google.com\r\n"
         "To: dmarc@example.com\r\n"
         "Subject: Report domain: example.com Submitter: google.com Report-ID: 12345678901234567890\r\n"
         "MIME-Version: 1.0\r\n"
         "Content-Type: multipart/mixed; boundary=\"b\"\r\n"
         "\r\n"
         "--b\r\n"
         "Content-Type: text/plain\r\n"
         "\r\n"
         "This is an aggregate report.\r\n"
         "--b\r\n"
         "Content-Type: application/gzip; name=\"google.com!example.com!1789430400!1789516799.xml.gz\"\r\n"
         "Content-Disposition: attachment; filename=\"google.com!example.com!1789430400!1789516799.xml.gz\"\r\n"
         "Content-Transfer-Encoding: base64\r\n"
         "\r\n" + base64 + "\r\n"
         "--b--\r\n"), candidates), "The message did not load.");
      Expect(candidates.size() == 1, "The gzip attachment was not the one candidate; the text part was taken for one.");
      Expect(candidates[0].name == "google.com!example.com!1789430400!1789516799.xml.gz", "The attachment's name was not read.");
      Expect(candidates[0].bytes == gz, "The attachment was not decoded from base64 to the gzip bytes.");

      // The whole message is the archive, as one large reporter sends it.
      Expect(R::FindCandidates(AnsiString(
         "From: dmarc@reporter.test\r\n"
         "To: dmarc@example.com\r\n"
         "Content-Type: application/zip; name=\"report.zip\"\r\n"
         "Content-Transfer-Encoding: base64\r\n"
         "\r\n" + Base64::Encode(zip.c_str(), zip.GetLength()) + "\r\n"), candidates) && candidates.size() == 1 && candidates[0].bytes == zip,
         "A message whose whole body is the archive was not read.");

      Expect(R::FindCandidates(AnsiString("From: a@b.test\r\nTo: c@d.test\r\n\r\nJust a note.\r\n"), candidates) && candidates.empty(),
         "Ordinary mail produced a candidate.");

      // ---------------------------------------------------------------- TLS-RPT
      R::TlsReport tls;
      Expect(R::ParseTlsJson(AnsiString(
         "{\"organization-name\":\"Company-X\",\"date-range\":{\"start-datetime\":\"2026-09-15T00:00:00Z\",\"end-datetime\":\"2026-09-15T23:59:59Z\"},"
         "\"contact-info\":\"sts-reporting@company-x.example\",\"report-id\":\"5065427c-23d3-47ca-b6e0-946ea0e8c4be\","
         "\"policies\":[{\"policy\":{\"policy-type\":\"sts\",\"policy-string\":[\"version: STSv1\",\"mode: enforce\"],\"policy-domain\":\"Example.com\",\"mx-host\":[\"*.mail.example.com\"]},"
         "\"summary\":{\"total-successful-session-count\":5326,\"total-failure-session-count\":303},"
         "\"failure-details\":[{\"result-type\":\"certificate-expired\",\"sending-mta-ip\":\"2001:db8:abcd:0012::1\",\"receiving-mx-hostname\":\"mx1.mail.example.com\",\"receiving-ip\":\"203.0.113.6\",\"failed-session-count\":100},"
         "{\"result-type\":\"starttls-not-supported\",\"sending-mta-ip\":\"2001:db8:abcd:0013::1\",\"receiving-mx-hostname\":\"mx2.mail.example.com\",\"receiving-ip\":\"203.0.113.7\",\"failed-session-count\":200}]}]}"),
         tls, problem), ("A TLS-RPT report was not read: " + problem).c_str());
      Expect(tls.org_name == "Company-X" && tls.report_id == "5065427c-23d3-47ca-b6e0-946ea0e8c4be", "The TLS-RPT metadata was not read.");
      Expect(tls.begin == 1789430400 && tls.end == 1789516799, "The TLS-RPT date range was not read.");
      Expect(tls.policies.size() == 1 && tls.policies[0].policy_domain == "example.com" && tls.policies[0].policy_type == "sts", "The policy was not read.");
      Expect(tls.policies[0].successful == 5326 && tls.policies[0].failed == 303, "The session counts were not read.");
      Expect(tls.policies[0].failures_json.Find("\"result_type\":\"certificate-expired\"") >= 0 && tls.policies[0].failures_json.Find("\"count\":200") >= 0,
             "The failure details were not kept.");

      R::TlsReport refusedTls;
      Expect(!R::ParseTlsJson(AnsiString("{\"organization-name\":\"x\"}"), refusedTls, problem), "A TLS-RPT report with no id or policies was read.");
      Expect(!R::ParseTlsJson(AnsiString("not json"), refusedTls, problem), "Text that is not JSON was read as a TLS-RPT report.");
      Expect(!R::ParseTlsJson(AnsiString("{\"report-id\":\"x\",\"date-range\":{\"start-datetime\":\"2026-09-15T00:00:00Z\",\"end-datetime\":\"2026-09-15T23:59:59Z\"},\"policies\":[{\"policy\":{\"policy-type\":\"sts\"}}]}"), refusedTls, problem),
             "A TLS-RPT report whose policy names no domain was read.");
      Expect(!R::ParseTlsJson(AnsiString("{\"report-id\":\"x\",\"date-range\":{\"start-datetime\":\"2026-09-15T00:00:00Z\",\"end-datetime\":\"2026-09-15T23:59:59Z\"},\"policies\":[{\"policy\":{\"policy-type\":\"sts\",\"policy-domain\":\"tls@example.com\"}}]}"), refusedTls, problem),
             "A TLS-RPT report whose policy domain is an address was read.");

      // --------------------------------------------------------------- the keys
      Expect(R::DmarcKey("google.com", "123", "Example.com") == R::DmarcKey("Google.COM", " 123 ", "example.com"),
             "The same report read twice did not make the same key.");
      Expect(R::DmarcKey("google.com", "123", "example.com") != R::DmarcKey("google.com", "124", "example.com"),
             "Two reports with different ids shared a key.");
      Expect(R::DmarcKey("google.com", "123", "example.com").GetLength() == 64, "The key is not a SHA-256 in hex.");
      Expect(R::TlsKey("x", "1", "example.com", "sts") != R::TlsKey("x", "1", "example.com", "tlsa"),
             "Two policies of one TLS-RPT report shared a key.");
   }
}
