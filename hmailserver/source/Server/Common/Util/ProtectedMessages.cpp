// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "ProtectedMessages.h"
#include "ConfidentialMessages.h"
#include "Unicode.h"
#include "Utilities.h"
#include "FileUtilities.h"
#include "FileInfo.h"
#include "Time.h"
#include "Encoding/Base64.h"
#include "Parsing/StringParser.h"
#include "../Application/Application.h"
#include "../Application/Configuration.h"
#include "../Application/ErrorManager.h"
#include "../Application/IniFileSettings.h"
#include "../BO/Account.h"
#include "../BO/Domain.h"
#include "../BO/Message.h"
#include "../BO/MessageData.h"
#include "../BO/MessageRecipients.h"
#include "../Cache/CacheContainer.h"
#include "../Persistence/PersistentMessage.h"
#include "../Persistence/MessageStore.h"
#include "../SQL/DALRecordset.h"
#include "../SQL/SQLCommand.h"
#include "../SQL/SQLStatement.h"
#include "../TCPIP/IPAddress.h"
#include "../../SMTP/RecipientParser.h"

#include <openssl/rand.h>

#include <boost/filesystem.hpp>
#include <ctime>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <string>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      const int TokenBytes = 16;
      const __int64 KeepRowsSeconds = 30LL * 24 * 60 * 60;
      const __int64 UnfinishedSeconds = 24 * 60 * 60;
      const time_t HourSeconds = 60 * 60;
      const int RequestsPerAddressPerHour = 120;
      const size_t MapCeiling = 10000;

      const char *Columns = "pmid, pmaccountid, pmtoken, pmrecipient, pmhint, pmfile, pmsize, pmstored, pmcomplete, pmcreated, pmexpires, pmopened, pmrevoked, pmreplyto";

      // The ceiling on the two routes that take no credential, per address.
      struct Budget
      {
         int count = 0;
         time_t since = 0;
      };
      std::mutex budgets_mutex;
      std::map<AnsiString, Budget> budgets;

      // One chunk is appended at a time, whatever two requests race for.
      std::mutex append_mutex;

      String Root()
      {
         return FileUtilities::Combine(IniFileSettings::Instance()->GetDataDirectory(), _T("Protected"));
      }

      AnsiString Utf8(const String &text)
      {
         AnsiString bytes;
         Unicode::WideToMultiByte(text, bytes);
         return bytes;
      }

      String Lower(const String &text)
      {
         String lower = text;
         lower.Trim();
         lower.ToLower();
         return lower;
      }

      String Clip(const String &text, int length)
      {
         String clipped = text;
         for (int i = 0; i < clipped.GetLength(); i++)
         {
            if (clipped[i] < 0x20)
               clipped.SetAt(i, _T(' '));
         }
         clipped.Trim();
         return clipped.GetLength() > length ? clipped.Mid(0, length) : clipped;
      }

      // Nothing that ends or folds a header line.
      AnsiString HeaderSafe(const String &text)
      {
         AnsiString bytes = Utf8(text);
         AnsiString safe;
         for (int i = 0; i < bytes.GetLength(); i++)
         {
            const unsigned char c = (unsigned char) bytes[i];
            if (c >= 0x20 && c != 0x7f)
               safe += (char) c;
         }
         return safe;
      }

      String HtmlText(const String &text)
      {
         String out;
         for (int i = 0; i < text.GetLength(); i++)
         {
            wchar_t c = text[i];
            if (c == '&') out += _T("&amp;");
            else if (c == '<') out += _T("&lt;");
            else if (c == '>') out += _T("&gt;");
            else if (c == '"') out += _T("&quot;");
            else out += c;
         }
         return out;
      }

      String UtcText(__int64 seconds)
      {
         struct tm parts;
         memset(&parts, 0, sizeof(parts));
#ifdef HM_PLATFORM_POSIX
         time_t value = (time_t) seconds;
         if (::gmtime_r(&value, &parts) == nullptr)
            return String();
#else
         __time64_t value = seconds;
         if (_gmtime64_s(&parts, &value) != 0)
            return String();
#endif
         static const String months[] = { _T("January"), _T("February"), _T("March"), _T("April"), _T("May"), _T("June"), _T("July"), _T("August"), _T("September"), _T("October"), _T("November"), _T("December") };
         String text;
         text.Format(_T("%d %s %04d, %02d:%02d UTC"), parts.tm_mday, months[parts.tm_mon % 12].c_str(), parts.tm_year + 1900, parts.tm_hour, parts.tm_min);
         return text;
      }

      // 128 bits from the system's random source as 22 characters of
      // base64url: the alphabet a URL carries without escaping, no padding.
      AnsiString RandomToken()
      {
         unsigned char buffer[TokenBytes];
         if (RAND_bytes(buffer, sizeof(buffer)) != 1)
            return AnsiString();
         AnsiString encoded = Base64::Encode((const char *) buffer, sizeof(buffer));
         AnsiString out;
         for (int i = 0; i < encoded.GetLength(); i++)
         {
            const char c = encoded[i];
            if (c == '+') out += '-';
            else if (c == '/') out += '_';
            else if (c == '=') break;
            else out += c;
         }
         return out;
      }

      ProtectedMessages::Entry ReadEntry(std::shared_ptr<DALRecordset> recordset)
      {
         ProtectedMessages::Entry entry;
         entry.id = recordset->GetInt64Value("pmid");
         entry.account_id = recordset->GetInt64Value("pmaccountid");
         entry.token = AnsiString(recordset->GetStringValue("pmtoken"));
         entry.recipient = recordset->GetStringValue("pmrecipient");
         entry.hint = recordset->GetStringValue("pmhint");
         entry.file = recordset->GetStringValue("pmfile");
         entry.size = recordset->GetInt64Value("pmsize");
         entry.stored = recordset->GetInt64Value("pmstored");
         entry.complete = recordset->GetLongValue("pmcomplete") != 0;
         entry.created = recordset->GetInt64Value("pmcreated");
         entry.expires = recordset->GetInt64Value("pmexpires");
         entry.opened = recordset->GetInt64Value("pmopened");
         entry.revoked = recordset->GetLongValue("pmrevoked") != 0;
         entry.reply_to = AnsiString(recordset->GetStringValue("pmreplyto"));
         return entry;
      }

      // Whether the account exists, decided by a query that answered: 1 when
      // it does, 0 when the database says it does not, -1 when the database
      // did not answer - which is never taken for "gone".
      int AccountExists(__int64 accountId)
      {
         SQLCommand command("select accountid from hm_accounts where accountid = @ID");
         command.AddParameter("@ID", accountId);
         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (!recordset)
            return -1;
         return recordset->IsEOF() ? 0 : 1;
      }

      // The file, deleted; true when it is gone. A delete that fails - the
      // file open in a reader's fetch - is reported once and left for the next
      // sweep, which selects the row again for as long as it still names it.
      bool DeleteStored(__int64 id, const String &file, const char *where)
      {
         const String path = FileUtilities::Combine(Root(), file);
         if (!FileUtilities::Exists(path) || FileUtilities::DeleteFile(path))
            return true;

         String text;
         text.Format(_T("The ciphertext of the password-protected message with row %I64d could not be deleted; it will be tried again on the next sweep."), id);
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7063, where, text);
         return false;
      }

      void ClearFileColumn(__int64 id)
      {
         SQLCommand clear("update hm_protectedmessages set pmfile = @EMPTY where pmid = @ID");
         clear.AddParameter("@EMPTY", String());
         clear.AddParameter("@ID", id);
         Application::Instance()->GetDBManager()->Execute(clear);
      }

      // RFC 4880 section 6.1: the checksum the armour's "=" line carries.
      unsigned int Crc24(const unsigned char *data, size_t length)
      {
         unsigned int crc = 0xB704CE;
         for (size_t i = 0; i < length; i++)
         {
            crc ^= ((unsigned int) data[i]) << 16;
            for (int bit = 0; bit < 8; bit++)
            {
               crc <<= 1;
               if (crc & 0x1000000)
                  crc ^= 0x1864CFB;
            }
         }
         return crc & 0xFFFFFF;
      }

      // The header of one packet: its tag and where its body is. False for a
      // partial body length, which frames a stream and which the page never
      // writes; the caller has by then seen the packets that matter.
      bool ReadPacketHeader(const unsigned char *data, size_t length, size_t at, int &tag, size_t &bodyStart, size_t &bodyLength)
      {
         if (at >= length)
            return false;
         const unsigned char header = data[at];
         if (!(header & 0x80))
            return false;
         size_t cursor = at + 1;
         if (header & 0x40)
         {
            tag = header & 0x3f;
            if (cursor >= length)
               return false;
            const unsigned char first = data[cursor++];
            if (first < 192)
               bodyLength = first;
            else if (first < 224)
            {
               if (cursor >= length)
                  return false;
               bodyLength = ((size_t) (first - 192) << 8) + data[cursor++] + 192;
            }
            else if (first == 255)
            {
               if (cursor + 4 > length)
                  return false;
               bodyLength = ((size_t) data[cursor] << 24) | ((size_t) data[cursor + 1] << 16) | ((size_t) data[cursor + 2] << 8) | data[cursor + 3];
               cursor += 4;
            }
            else
               return false;
         }
         else
         {
            tag = (header >> 2) & 0x0f;
            const int kind = header & 0x03;
            if (kind == 0)
            {
               if (cursor >= length)
                  return false;
               bodyLength = data[cursor++];
            }
            else if (kind == 1)
            {
               if (cursor + 2 > length)
                  return false;
               bodyLength = ((size_t) data[cursor] << 8) | data[cursor + 1];
               cursor += 2;
            }
            else if (kind == 2)
            {
               if (cursor + 4 > length)
                  return false;
               bodyLength = ((size_t) data[cursor] << 24) | ((size_t) data[cursor + 1] << 16) | ((size_t) data[cursor + 2] << 8) | data[cursor + 3];
               cursor += 4;
            }
            else
               bodyLength = length - cursor;
         }
         bodyStart = cursor;
         return bodyStart + bodyLength <= length;
      }
   }

   int
   ProtectedMessages::DefaultDays()
   {
      return Configuration::Instance()->GetProtectedMessageDays();
   }

   String
   ProtectedMessages::BaseUrl()
   {
      return ConfidentialMessages::BaseUrl();
   }

   AnsiString
   ProtectedMessages::LinkFor(const AnsiString &token)
   {
      return Utf8(BaseUrl()) + "/portal/protected/" + token;
   }

   bool
   ProtectedMessages::IsToken(const AnsiString &value)
   {
      if (value.GetLength() != TokenLength)
         return false;
      for (int i = 0; i < value.GetLength(); i++)
      {
         const char c = value[i];
         const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_';
         if (!ok)
            return false;
      }
      return true;
   }

   bool
   ProtectedMessages::Create(__int64 accountId, const String &recipient, const String &hint, __int64 size, __int64 expires,
                             const AnsiString &replyTo, Entry &made)
   {
      made = Entry();
      made.token = RandomToken();
      if (made.token.IsEmpty())
         return false;

      String directory;
      directory.Format(_T("%I64d"), accountId);
      made.account_id = accountId;
      made.recipient = Clip(Lower(recipient), 255);
      made.hint = Clip(hint, HintMaximum);
      made.file = FileUtilities::Combine(directory, String(made.token) + _T(".asc"));
      made.size = size;
      made.stored = 0;
      made.complete = false;
      made.created = (__int64) time(nullptr);
      made.expires = expires;
      made.reply_to = replyTo;

      SQLStatement statement(SQLStatement::STInsert, "hm_protectedmessages");
      statement.AddColumnInt64("pmaccountid", made.account_id);
      statement.AddColumn("pmtoken", String(made.token));
      statement.AddColumn("pmrecipient", made.recipient);
      statement.AddColumn("pmhint", made.hint);
      statement.AddColumn("pmfile", made.file);
      statement.AddColumnInt64("pmsize", made.size);
      statement.AddColumnInt64("pmstored", 0);
      statement.AddColumn("pmcomplete", (long) 0);
      statement.AddColumnInt64("pmcreated", made.created);
      statement.AddColumnInt64("pmexpires", made.expires);
      statement.AddColumnInt64("pmopened", 0);
      statement.AddColumn("pmrevoked", (long) 0);
      statement.AddColumn("pmreplyto", String(made.reply_to));
      statement.SetIdentityColumn("pmid");

      __int64 id = 0;
      if (!Application::Instance()->GetDBManager()->Execute(statement, &id) || id <= 0)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7060, "ProtectedMessages::Create",
            "The record of a password-protected message could not be written.");
         return false;
      }
      made.id = id;
      return true;
   }

   ProtectedMessages::AppendResult
   ProtectedMessages::Append(const Entry &entry, const AnsiString &bytes, Entry &updated)
   {
      std::lock_guard<std::mutex> guard(append_mutex);

      const String path = PathOf(entry);
      if (path.IsEmpty())
         return Failed;
      String accountDirectory;
      accountDirectory.Format(_T("%I64d"), entry.account_id);
      const String folder = FileUtilities::Combine(Root(), accountDirectory);
      if (!FileUtilities::DirectoryExists(folder) && !FileUtilities::CreateDirectory(folder))
         return Failed;

      // The file is the truth about where the upload has reached. The route
      // read the row outside this lock; a second chunk for the same offset
      // that also passed there meets the bytes the first one wrote here.
      unsigned __int64 onDisk = 0;
      if (FileUtilities::Exists(path) && !FileUtilities::FileSize64(path, onDisk))
         return Failed;
      if ((__int64) onDisk != entry.stored)
      {
         // The row is brought up to the file, so that the next chunk - sent
         // at the size this answers - passes the route's own check as well;
         // otherwise a row left behind by a crash between the write and its
         // update would answer one 409 from the row and another from the file
         // for ever.
         SQLCommand agree("update hm_protectedmessages set pmstored = @STORED where pmid = @ID");
         agree.AddParameter("@STORED", (__int64) onDisk);
         agree.AddParameter("@ID", entry.id);
         Application::Instance()->GetDBManager()->Execute(agree);
         updated = entry;
         updated.stored = (__int64) onDisk;
         return Behind;
      }

      {
         // A narrow path on POSIX: libstdc++'s streams take no wide one.
#ifdef HM_PLATFORM_POSIX
         const AnsiString narrowPath(path);
         std::ofstream out(narrowPath.c_str(), std::ios::binary | std::ios::app);
#else
         std::ofstream out(path.c_str(), std::ios::binary | std::ios::app);
#endif
         if (!out)
            return Failed;
         out.write(bytes.c_str(), bytes.GetLength());
         out.close();
         if (out.fail())
            return Failed;
      }

      updated = entry;
      updated.stored = entry.stored + bytes.GetLength();

      SQLCommand update("update hm_protectedmessages set pmstored = @STORED where pmid = @ID");
      update.AddParameter("@STORED", updated.stored);
      update.AddParameter("@ID", entry.id);
      return Application::Instance()->GetDBManager()->Execute(update) ? Appended : Failed;
   }

   bool
   ProtectedMessages::IsSymmetricMessage(const AnsiString &text, AnsiString &problem)
   {
      const std::string source(text.c_str(), (size_t) text.GetLength());
      const std::string beginLine = "-----BEGIN PGP MESSAGE-----";
      const std::string endLine = "-----END PGP MESSAGE-----";

      const size_t begin = source.find(beginLine);
      if (begin == std::string::npos || source.find_first_not_of(" \t\r\n") != begin)
      {
         problem = "not an OpenPGP message: the ciphertext begins with -----BEGIN PGP MESSAGE-----";
         return false;
      }
      const size_t end = source.find(endLine, begin);
      if (end == std::string::npos)
      {
         problem = "not an OpenPGP message: the armour has no end line";
         return false;
      }
      if (source.find_first_not_of(" \t\r\n", end + endLine.size()) != std::string::npos)
      {
         problem = "not an OpenPGP message: there is something after the armour";
         return false;
      }

      // The armour headers run to the first blank line; the radix-64 follows,
      // and the line beginning "=" is the checksum.
      std::string body = source.substr(begin + beginLine.size(), end - begin - beginLine.size());
      size_t blank = body.find("\n\n");
      size_t blankCrlf = body.find("\r\n\r\n");
      size_t dataAt = 0;
      if (blankCrlf != std::string::npos && (blank == std::string::npos || blankCrlf < blank))
         dataAt = blankCrlf + 4;
      else if (blank != std::string::npos)
         dataAt = blank + 2;

      std::string data;
      std::string checksum;
      size_t at = dataAt;
      while (at < body.size())
      {
         size_t lineEnd = body.find('\n', at);
         if (lineEnd == std::string::npos)
            lineEnd = body.size();
         std::string line = body.substr(at, lineEnd - at);
         at = lineEnd + 1;
         while (!line.empty() && (line[line.size() - 1] == '\r' || line[line.size() - 1] == ' ' || line[line.size() - 1] == '\t'))
            line.erase(line.size() - 1);
         size_t first = line.find_first_not_of(" \t");
         if (first == std::string::npos)
            continue;
         line = line.substr(first);
         if (line[0] == '=')
         {
            checksum = line.substr(1);
            continue;
         }
         for (size_t i = 0; i < line.size(); i++)
         {
            const char c = line[i];
            const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=';
            if (!ok)
            {
               problem = "not an OpenPGP message: the armour carries a character that is not radix-64";
               return false;
            }
         }
         data += line;
      }
      if (data.empty())
      {
         problem = "not an OpenPGP message: the armour is empty";
         return false;
      }

      const AnsiString decoded = Base64::Decode(data.c_str(), (int) data.size());
      if (decoded.IsEmpty())
      {
         problem = "not an OpenPGP message: the radix-64 does not decode";
         return false;
      }
      const unsigned char *bytes = (const unsigned char *) decoded.c_str();
      const size_t length = (size_t) decoded.GetLength();

      if (!checksum.empty())
      {
         const AnsiString given = Base64::Decode(checksum.c_str(), (int) checksum.size());
         const unsigned int sum = Crc24(bytes, length);
         if (given.GetLength() != 3 ||
             (unsigned char) given[0] != ((sum >> 16) & 0xff) || (unsigned char) given[1] != ((sum >> 8) & 0xff) || (unsigned char) given[2] != (sum & 0xff))
         {
            problem = "not an OpenPGP message: the armour's checksum does not match its contents";
            return false;
         }
      }

      // The packets: exactly the shape the page writes and nothing else. One
      // or more symmetric-key encrypted session keys first - which is what
      // makes this a message to a password and not to a key or to nobody, and
      // each is a few dozen bytes, never more - and then ONE integrity-
      // protected data packet that runs to the end. A literal packet between
      // them would be a plaintext filed under the name of ciphertext, and a
      // packet after the body would be bytes nobody decrypts; both are
      // refused by name rather than passed over.
      const size_t SessionKeyMaximum = 128;
      int sessionKeys = 0;
      bool bodySeen = false;
      size_t cursor = 0;
      while (cursor < length)
      {
         int tag = 0;
         size_t bodyStart = 0;
         size_t bodyLength = 0;
         if (!ReadPacketHeader(bytes, length, cursor, tag, bodyStart, bodyLength))
         {
            problem = sessionKeys == 0
               ? "not an OpenPGP message: the first packet is not one"
               : "not a message to a password: a packet's length runs past the end of the message, or is one this route does not file";
            return false;
         }
         if (bodySeen)
         {
            problem = "not a message to a password: something follows the encrypted body";
            return false;
         }
         if (tag == 3)
         {
            if (bodyLength > SessionKeyMaximum)
            {
               problem = "not a message to a password: the session-key packet is larger than one can be";
               return false;
            }
            sessionKeys++;
         }
         else if (tag == 1)
         {
            problem = sessionKeys == 0
               ? "the message is encrypted to a key, not to a password; this route files messages to a password"
               : "the message is encrypted to a key as well as to a password; this route files messages to a password alone";
            return false;
         }
         else if (tag == 9)
         {
            problem = "the message carries no integrity protection (a symmetrically encrypted data packet without a modification detection code)";
            return false;
         }
         else if (tag == 18)
         {
            if (sessionKeys == 0)
            {
               problem = "not a message to a password: the first packet is not a symmetric-key encrypted session key";
               return false;
            }
            if (bodyStart + bodyLength != length)
            {
               problem = "not a message to a password: something follows the encrypted body";
               return false;
            }
            bodySeen = true;
         }
         else
         {
            AnsiString sentence;
            sentence.Format("not a message to a password: packet %d has no place in one (a session key, then the encrypted body, and nothing else)", tag);
            problem = sentence;
            return false;
         }
         cursor = bodyStart + bodyLength;
      }
      if (!bodySeen)
      {
         problem = "not a message to a password: no integrity-protected data packet follows the session key";
         return false;
      }
      return true;
   }

   bool
   ProtectedMessages::Complete(const Entry &entry, Entry &completed, AnsiString &problem)
   {
      AnsiString text;
      if (!ReadFile(entry, text))
      {
         problem = "the ciphertext could not be read back";
         return false;
      }
      if ((__int64) text.GetLength() != entry.size)
      {
         problem = "the bytes stored are not the size that was declared";
         Discard(entry);
         return false;
      }
      if (!IsSymmetricMessage(text, problem))
      {
         Discard(entry);
         return false;
      }

      SQLCommand update("update hm_protectedmessages set pmcomplete = 1 where pmid = @ID");
      update.AddParameter("@ID", entry.id);
      if (!Application::Instance()->GetDBManager()->Execute(update))
      {
         problem = "the record could not be marked complete";
         return false;
      }
      completed = entry;
      completed.complete = true;
      return true;
   }

   bool
   ProtectedMessages::ReadFile(const Entry &entry, AnsiString &text)
   {
      const String path = PathOf(entry);
      if (path.IsEmpty())
         return false;
#ifdef HM_PLATFORM_POSIX
      const AnsiString narrowPath(path);
      std::ifstream in(narrowPath.c_str(), std::ios::binary);
#else
      std::ifstream in(path.c_str(), std::ios::binary);
#endif
      if (!in)
         return false;
      std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      text = AnsiString(bytes);
      return true;
   }

   bool
   ProtectedMessages::FindByToken(const AnsiString &token, Entry &entry)
   {
      if (!IsToken(token))
         return false;
      SQLCommand command(String(_T("select ")) + String(Columns) + _T(" from hm_protectedmessages where pmtoken = @TOKEN"));
      command.AddParameter("@TOKEN", String(token));
      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return false;
      entry = ReadEntry(recordset);
      // A database that compares without regard to case would answer a token
      // that differs from the row's in case alone; the token is the link.
      return entry.token == token;
   }

   bool
   ProtectedMessages::FindById(__int64 accountId, __int64 id, Entry &entry)
   {
      SQLCommand command(String(_T("select ")) + String(Columns) + _T(" from hm_protectedmessages where pmid = @ID and pmaccountid = @ACCOUNTID"));
      command.AddParameter("@ID", id);
      command.AddParameter("@ACCOUNTID", accountId);
      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return false;
      entry = ReadEntry(recordset);
      return true;
   }

   bool
   ProtectedMessages::List(__int64 accountId, std::vector<Entry> &entries)
   {
      entries.clear();
      SQLCommand command(String(_T("select ")) + String(Columns) + _T(" from hm_protectedmessages where pmaccountid = @ACCOUNTID order by pmid desc"));
      command.AddParameter("@ACCOUNTID", accountId);
      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7061, "ProtectedMessages::List",
            "The password-protected messages of an account could not be read.");
         return false;
      }
      while (!recordset->IsEOF())
      {
         entries.push_back(ReadEntry(recordset));
         recordset->MoveNext();
      }
      return true;
   }

   String
   ProtectedMessages::PathOf(const Entry &entry)
   {
      return entry.file.IsEmpty() ? String() : FileUtilities::Combine(Root(), entry.file);
   }

   int
   ProtectedMessages::CountLive(__int64 accountId)
   {
      SQLCommand command("select count(*) as livecount from hm_protectedmessages where pmaccountid = @ACCOUNTID and pmexpires > @NOW and pmrevoked = 0");
      command.AddParameter("@ACCOUNTID", accountId);
      command.AddParameter("@NOW", (__int64) time(nullptr));
      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return -1;
      return (int) recordset->GetInt64Value("livecount");
   }

   __int64
   ProtectedMessages::HeldBytes(__int64 accountId)
   {
      SQLCommand command("select pmsize from hm_protectedmessages where pmaccountid = @ACCOUNTID and pmexpires > @NOW and pmrevoked = 0");
      command.AddParameter("@ACCOUNTID", accountId);
      command.AddParameter("@NOW", (__int64) time(nullptr));
      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return -1;
      __int64 total = 0;
      while (!recordset->IsEOF())
      {
         total += recordset->GetInt64Value("pmsize");
         recordset->MoveNext();
      }
      return total;
   }

   int
   ProtectedMessages::CountReplies(const AnsiString &originalToken)
   {
      SQLCommand command("select count(*) as replycount from hm_protectedmessages where pmreplyto = @TOKEN and pmrevoked = 0");
      command.AddParameter("@TOKEN", String(originalToken));
      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return -1;
      return (int) recordset->GetInt64Value("replycount");
   }

   void
   ProtectedMessages::Discard(const Entry &entry)
   {
      if (entry.id <= 0)
         return;
      SQLCommand remove("delete from hm_protectedmessages where pmid = @ID");
      remove.AddParameter("@ID", entry.id);
      Application::Instance()->GetDBManager()->Execute(remove);

      const String path = PathOf(entry);
      if (!path.IsEmpty() && FileUtilities::Exists(path))
         FileUtilities::DeleteFile(path);
   }

   bool
   ProtectedMessages::Revoke(__int64 accountId, __int64 id)
   {
      Entry entry;
      if (!FindById(accountId, id, entry))
         return false;

      // The row is what the fetch refuses on, so it goes first; the file's
      // column is cleared only once the file is gone, so the sweep - which
      // selects every withdrawn row still naming a file - retries a delete
      // that fails here.
      SQLCommand update("update hm_protectedmessages set pmrevoked = 1 where pmid = @ID and pmaccountid = @ACCOUNTID");
      update.AddParameter("@ID", id);
      update.AddParameter("@ACCOUNTID", accountId);
      if (!Application::Instance()->GetDBManager()->Execute(update))
         return false;

      if (!entry.file.IsEmpty() && DeleteStored(entry.id, entry.file, "ProtectedMessages::Revoke"))
         ClearFileColumn(entry.id);
      return true;
   }

   void
   ProtectedMessages::MarkOpened(const Entry &entry)
   {
      if (entry.opened != 0)
         return;
      SQLCommand command("update hm_protectedmessages set pmopened = @NOW where pmid = @ID and pmopened = 0");
      command.AddParameter("@NOW", (__int64) time(nullptr));
      command.AddParameter("@ID", entry.id);
      Application::Instance()->GetDBManager()->Execute(command);
   }

   bool
   ProtectedMessages::WriteNotice(std::shared_ptr<Message> message, const String &fileName, const String &fromHeader, const String &toAddress,
                                  const String &senderName, const Entry &entry, const AnsiString &link)
   {
      FileUtilities::DeleteFile(fileName);

      const String when = UtcText(entry.expires);
      const String url = String(link);
      String text;
      text += senderName + _T(" sent you a password-protected message.\r\n\r\n");
      text += _T("It was encrypted in their browser and can be read only with the password they gave you, on this page until ") + when + _T(":\r\n\r\n");
      text += url + _T("\r\n\r\n");
      if (!entry.hint.IsEmpty())
         text += _T("About the password: ") + entry.hint + _T("\r\n\r\n");
      text += _T("Enter the password on that page and your browser decrypts the message; you can write a reply there, which goes back the same way. ");
      text += _T("Nobody who does not have the password can read it, not even the mail server that keeps it.\r\n");

      String html;
      html += _T("<p><b>") + HtmlText(senderName) + _T("</b> sent you a password-protected message.</p>");
      html += _T("<p>It was encrypted in their browser and can be read only with the password they gave you, on this page until ") + HtmlText(when) + _T(".</p>");
      html += _T("<p><a href=\"") + HtmlText(url) + _T("\">Read the message</a></p>");
      if (!entry.hint.IsEmpty())
         html += _T("<p>About the password: ") + HtmlText(entry.hint) + _T("</p>");
      html += _T("<p style=\"color:#555\">Enter the password on that page and your browser decrypts the message; you can write a reply there, which goes back the same way. Nobody who does not have the password can read it, not even the mail server that keeps it.</p>");

      MessageData data;
      data.LoadFromMessage(fileName, message);
      data.SetCharset(_T("utf-8"));
      data.SetFrom(fromHeader);
      data.SetTo(toAddress);
      data.SetSubject(_T("A password-protected message from ") + senderName);
      data.SetBody(text);
      data.SetHTMLBody(html);
      data.SetSentTime(Time::GetCurrentMimeDate());
      data.GenerateMessageID();
      data.SetFieldValue(_T("X-hMailServer-Protected"), String(entry.token));

      return data.Write(fileName);
   }

   bool
   ProtectedMessages::SendReplyNotice(const Entry &original, const Entry &reply, const AnsiString &link)
   {
      std::shared_ptr<const Account> account = CacheContainer::Instance()->GetAccount(original.account_id);
      if (!account)
         return false;
      const String to = account->GetAddress();
      const String domainName = StringParser::ExtractDomain(to);
      std::shared_ptr<const Domain> domain = domainName.IsEmpty() ? std::shared_ptr<const Domain>() : CacheContainer::Instance()->GetDomain(domainName);
      String from = domain ? domain->GetPostmaster() : String();
      from.Trim();
      if (from.IsEmpty() || from.Find(_T("@")) <= 0)
         from = _T("postmaster@") + domainName;

      AnsiString content;
      content += "From: <" + HeaderSafe(from) + ">\r\n";
      content += "To: <" + HeaderSafe(to) + ">\r\n";
      content += "Subject: A reply to your password-protected message\r\n";
      content += "Date: " + AnsiString(Time::GetCurrentMimeDate()) + "\r\n";
      content += "Message-ID: " + HeaderSafe(Utilities::GenerateMessageID()) + "\r\n";
      content += "Auto-Submitted: auto-generated\r\n";
      content += "X-Auto-Response-Suppress: All\r\n";
      content += "X-hMailServer-Protected: " + reply.token + "\r\n";
      content += "MIME-Version: 1.0\r\n";
      content += "Content-Type: text/plain; charset=\"utf-8\"\r\n";
      content += "Content-Transfer-Encoding: 8bit\r\n";
      content += "\r\n";
      content += HeaderSafe(reply.recipient) + " replied to the password-protected message you sent on " + HeaderSafe(UtcText(original.created)) + ".\r\n";
      content += "\r\n";
      content += "The reply was encrypted in their browser under the password you set, and it opens with that password here, until " + HeaderSafe(UtcText(reply.expires)) + ":\r\n";
      content += "\r\n";
      content += link + "\r\n";

      std::shared_ptr<Message> message = std::shared_ptr<Message>(new Message());
      message->SetState(Message::Delivering);
      message->SetFromAddress(_T(""));

      const String fileName = MessageStore::WorkingFile(message);
      if (!FileUtilities::WriteToFile(fileName, content))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7064, "ProtectedMessages::SendReplyNotice",
            "The notice of a reply to a password-protected message could not be written to the queue for " + to + ".");
         return false;
      }
      message->SetSize(MessageStore::FileSize(fileName));

      bool recipientOk = false;
      RecipientParser recipientParser;
      recipientParser.CreateMessageRecipientList(to, message->GetRecipients(), recipientOk);

      if (message->GetRecipients()->GetCount() == 0 || !PersistentMessage::SaveObject(message))
      {
         FileUtilities::DeleteFile(fileName);
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7064, "ProtectedMessages::SendReplyNotice",
            "The notice of a reply to a password-protected message could not be queued for " + to + ".");
         return false;
      }

      Application::Instance()->SubmitPendingEmail();
      return true;
   }

   bool
   ProtectedMessages::AllowUnauthenticated(const IPAddress &peer)
   {
      const AnsiString key = AnsiString(peer.ToString());
      // A process on this machine is not a scraper - and the regression suite
      // is one, fetching from loopback in every run - so loopback is exempt,
      // exactly as it is from the failed-logon count.
      if (key == "127.0.0.1" || key == "::1")
         return true;

      const time_t now = time(nullptr);
      std::lock_guard<std::mutex> guard(budgets_mutex);
      for (auto it = budgets.begin(); it != budgets.end();)
         it = now - it->second.since >= HourSeconds ? budgets.erase(it) : std::next(it);
      if (budgets.size() > MapCeiling)
         budgets.clear();
      Budget &budget = budgets[key];
      if (budget.since == 0 || now - budget.since >= HourSeconds)
      {
         budget.since = now;
         budget.count = 0;
      }
      if (budget.count >= RequestsPerAddressPerHour)
         return false;
      budget.count++;
      return true;
   }

   void
   ProtectedMessages::ForgetBudgets()
   {
      std::lock_guard<std::mutex> guard(budgets_mutex);
      budgets.clear();
   }

   int
   ProtectedMessages::Sweep(bool orphanDirectories)
   {
      const __int64 now = (__int64) time(nullptr);

      struct Lapsed
      {
         __int64 id;
         String file;
         bool remove_row;
      };
      std::vector<Lapsed> lapsed;

      // No recordset is the database not answering, and a sweep that cannot
      // read the rows must not act on the files: nothing below runs.
      SQLCommand command("select pmid, pmfile, pmexpires, pmrevoked, pmcomplete, pmcreated from hm_protectedmessages where pmexpires <= @NOW or pmrevoked = 1 or (pmcomplete = 0 and pmcreated <= @UNFINISHED)");
      command.AddParameter("@NOW", now);
      command.AddParameter("@UNFINISHED", now - UnfinishedSeconds);
      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return 0;
      while (!recordset->IsEOF())
      {
         Lapsed item;
         item.id = recordset->GetInt64Value("pmid");
         item.file = recordset->GetStringValue("pmfile");
         const bool unfinished = recordset->GetLongValue("pmcomplete") == 0;
         // A record never finished is a row nobody can reach: it goes with its
         // file. A finished row stays thirty days past its expiry so that the
         // sender's list still says what became of it.
         item.remove_row = unfinished || recordset->GetInt64Value("pmexpires") + KeepRowsSeconds <= now;
         if (!item.file.IsEmpty() || item.remove_row)
            lapsed.push_back(item);
         recordset->MoveNext();
      }

      int ended = 0;
      for (const Lapsed &item : lapsed)
      {
         if (!item.file.IsEmpty() && DeleteStored(item.id, item.file, "ProtectedMessages::Sweep"))
         {
            ClearFileColumn(item.id);
            ended++;
         }

         if (item.remove_row && (item.file.IsEmpty() || !FileUtilities::Exists(FileUtilities::Combine(Root(), item.file))))
         {
            SQLCommand remove("delete from hm_protectedmessages where pmid = @ID");
            remove.AddParameter("@ID", item.id);
            Application::Instance()->GetDBManager()->Execute(remove);
         }
      }

      // A deleted account's rows went with it; its directory is removed here,
      // from the retention task only. Only a real directory named by a number
      // is ever looked at, and a symlink never - and "gone" is what the
      // database said when asked, never a lookup that came back empty because
      // the database was away: an outage must not delete every account's files.
      const String root = Root();
      if (orphanDirectories && FileUtilities::DirectoryExists(root))
      {
         boost::system::error_code error_code;
         const boost::filesystem::path rootPath(root.c_str());
         std::vector<boost::filesystem::path> gone;
         for (boost::filesystem::directory_iterator item(rootPath, error_code);
              item != boost::filesystem::directory_iterator(); item.increment(error_code))
         {
            if (error_code)
               break;
            const boost::filesystem::path current = item->path();
            if (boost::filesystem::is_symlink(current, error_code) || !boost::filesystem::is_directory(current, error_code))
               continue;
            const std::string name = current.filename().string();
            if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos || name.size() > 18)
               continue;
            const __int64 accountId = std::stoll(name);
            if (accountId > 0 && AccountExists(accountId) == 0)
               gone.push_back(current);
         }
         for (const boost::filesystem::path &directory : gone)
            boost::filesystem::remove_all(directory, error_code);
      }

      if (ended > 0)
      {
         String text;
         text.Format(_T("ProtectedMessages: %d password-protected message(s) deleted, expired, withdrawn or never finished."), ended);
         LOG_APPLICATION(text);
      }
      return ended;
   }
}
