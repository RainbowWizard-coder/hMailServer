// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Attachments opened somewhere safe. See Sandbox.h and docs/Sandbox.md.

#include "stdafx.h"
#include "Sandbox.h"

#include <openssl/sha.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <ctime>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../BO/Message.h"
#include "../BO/MessageData.h"
#include "../BO/MessageRecipient.h"
#include "../BO/MessageRecipients.h"
#include "../Mime/Mime.h"
#include "../Persistence/PersistentMessage.h"
#include "../Persistence/MessageStore.h"
#include "../Application/ExceptionHandler.h"
#include "../Application/IniFileSettings.h"
#include "../AntiSpam/QuarantineStore.h"
#include "../SQL/DALConnection.h"
#include "../SQL/DALRecordset.h"
#include "../SQL/DatabaseConnectionManager.h"
#include "../Util/Charset.h"
#include "../Util/Crypt.h"
#include "../Util/FileUtilities.h"
#include "../Util/GUIDCreator.h"
#include "../Util/HttpsClient.h"
#include "../Util/JsonDocument.h"
#include "../Util/LegacyBlowfish.h"
#include "../Util/MessageTrace.h"
#include "../Util/MessageUtilities.h"
#include "../Util/TraceHeaderWriter.h"
#include "../../SMTP/DeliveryQueue.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   const TCHAR *Sandbox::DefaultTypes =
      _T("exe,dll,scr,js,jse,vbs,wsf,hta,msi,bat,cmd,ps1,jar,lnk,iso,img,docm,xlsm,pptm,doc,xls,ppt,rtf,pdf,zip,7z,rar");

   namespace
   {
      // A file that is too large to submit, or could not be read, is recorded
      // beside the submitted ones of a held message so that the header can say
      // so when the hold ends. Not a state the poller ever acts on.
      const int FileNotSubmittable = 3;

      // The start of every quarantine reason this unit writes, which is what
      // QuarantineStore's release recognises it by.
      const TCHAR *const QuarantineReasonPrefix = _T("Sandbox: ");

      const size_t MaxResponseBytes = 64 * 1024;

      // Each request's own ceiling - connect, TLS, send and the whole answer
      // (HttpsClient keeps one deadline across them) - and never longer than
      // what is left of the pass's budget.
      const int SubmitTimeoutSeconds = 60;
      const int VerdictTimeoutSeconds = 15;

      // How long one pass may spend on HTTP. Past it no request is started; the
      // held messages the pass did not reach come first at the next one. The
      // timeouts are decided before any request, so a sandbox that accepts
      // connections and never answers cannot stretch a hold past its timeout
      // by more than one pass.
      const int PassBudgetSeconds = 60;

      // The longest submission id stored (hm_sandboxholds.shsubmission).
      const int MaxSubmissionLength = 1000;

      // A hold that cannot be recorded, or a message whose data cannot be read,
      // is tried again a minute later - this many times in a row, and then
      // decided by SandboxTimeoutAction rather than tried for ever.
      const int MaxAttempts = 5;

      // Forwarded messages are opened this many levels deep; the level past it
      // is submitted whole, whatever its type.
      const int MaxForwardedDepth = 5;

      // The longest secret the legacy Blowfish scheme stores, which is what a
      // protected setting is written with while ProtectStoredSecretsWithDPAPI
      // is 0.
      const int LegacySecretMaxLength = LegacyBlowfish::MaxPlainLength;

      // The largest message file MessageData parses (MessageData.cpp).
      const unsigned __int64 MaxParsedBytes = 1024 * 1024 * 80;

      // How often the expired verdicts are swept, and how often a poller that
      // keeps failing says so.
      const __int64 SweepIntervalSeconds = 600;
      const __int64 FailureReportIntervalSeconds = 3600;

      // The poller's thread. A pointer that is never freed at exit, because a
      // thread object destroyed while its thread still runs ends the process.
      boost::mutex poller_mutex;
      boost::thread *poller_thread = nullptr;

      // Set when the deliverer holds a message, so that the poller submits its
      // files within a second rather than at the next poll.
      std::atomic<bool> submit_soon(false);

      // Whether this start has said that the switch is on with an address
      // empty. Once per start, and again only after it was put right.
      std::atomic<bool> inactive_reported(false);

      // The poller's own bookkeeping, read and written on its thread alone.
      __int64 last_sweep = 0;
      __int64 last_poll_failure_report = 0;
      __int64 resume_after = 0;

      // Attempts per message at recording a hold or reading its data.
      boost::mutex attempts_mutex;
      std::map<__int64, int> attempts;

      __int64 NowSeconds()
      {
         return (__int64) time(nullptr);
      }

      String ClipText(const String &value, int maximum)
      {
         if (value.GetLength() <= maximum)
            return value;

         return value.Mid(0, maximum);
      }

      // One line, printable: what a sandbox wrote in its detail and what a
      // sender called a file both reach the application log and a header.
      String OneLine(const String &value)
      {
         String result;

         for (int i = 0; i < value.GetLength(); i++)
         {
            const wchar_t c = value.GetAt(i);
            result += (c < 32 || c == 127) ? (wchar_t) ' ' : c;
         }

         result.Trim();
         return result;
      }

      // Text every database this server runs on stores as it is: a character
      // outside the Basic Multilingual Plane - an emoji in a file name - is a
      // question mark, because MySQL's utf8 (these tables' charset, as every
      // table's) is three bytes a character and refuses the fourth in strict
      // mode, which would fail the hold's insert every time.
      String Storable(const String &value, int maximum)
      {
         String result;

         for (int i = 0; i < value.GetLength(); i++)
         {
            const unsigned int c = (unsigned int) value.GetAt(i);

            if (c >= 0xD800 && c <= 0xDBFF)
            {
               // A high surrogate: the pair is one character outside the plane.
               const unsigned int next = i + 1 < value.GetLength() ? (unsigned int) value.GetAt(i + 1) : 0;
               if (next >= 0xDC00 && next <= 0xDFFF)
                  i++;
               result += (wchar_t) '?';
            }
            else if ((c >= 0xDC00 && c <= 0xDFFF) || c > 0xFFFF)
            {
               result += (wchar_t) '?';
            }
            else
            {
               result += (wchar_t) c;
            }
         }

         return ClipText(result, maximum);
      }

      // A header field value: ASCII, one line, at most 900 characters. A file
      // name outside ASCII keeps its shape with question marks, since the
      // header is for a person reading the source and not for a program.
      AnsiString HeaderSafe(const String &value)
      {
         AnsiString result;

         for (int i = 0; i < value.GetLength() && result.GetLength() < 900; i++)
         {
            const wchar_t c = value.GetAt(i);

            if (c < 32 || c == 127)
               result += ' ';
            else if (c > 126)
               result += '?';
            else
               result += (char) c;
         }

         return result;
      }

      AnsiString ToUtf8(const String &value)
      {
         return Charset::ToMultiByte(value, "utf-8");
      }

      String FromUtf8(const std::string &value)
      {
         AnsiString bytes;
         bytes.assign(value.data(), value.size());
         return Charset::ToWideChar(bytes, "utf-8");
      }

      AnsiString Sha256Hex(const AnsiString &bytes)
      {
         unsigned char digest[SHA256_DIGEST_LENGTH] = {};
         SHA256(reinterpret_cast<const unsigned char *>(bytes.data()), static_cast<size_t>(bytes.size()), digest);

         static const char *hex = "0123456789abcdef";
         AnsiString result;
         for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
         {
            result += hex[digest[i] >> 4];
            result += hex[digest[i] & 0x0f];
         }
         return result;
      }

      // The database manager, or null while the application has none - between
      // a stop's CloseDatabase and the next start. Every statement below asks
      // for it through here, so none of them is run on a manager that is gone.
      std::shared_ptr<DatabaseConnectionManager> Db()
      {
         return Application::Instance()->GetDBManager();
      }

      std::shared_ptr<DALRecordset> QueryRows(const SQLCommand &command)
      {
         std::shared_ptr<DatabaseConnectionManager> db = Db();
         if (!db)
            return std::shared_ptr<DALRecordset>();

         return db->OpenRecordset(command);
      }

      bool RunStatement(const SQLCommand &command)
      {
         std::shared_ptr<DatabaseConnectionManager> db = Db();
         if (!db)
            return false;

         return db->Execute(command);
      }

      void ReportSandboxError(int code, const String &source, const String &text)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, code, source, text);
      }

      // Whether the sandbox is on AND named: with either address empty nothing
      // is held and nothing is submitted, whatever the switch says. Saving the
      // switch on with an address empty is refused on every surface; this is
      // for a configuration that has it anyway.
      bool Active()
      {
         Configuration *configuration = Configuration::Instance();
         return configuration->GetSandboxEnabled() &&
                !configuration->GetSandboxSubmitUrl().IsEmpty() &&
                !configuration->GetSandboxVerdictUrl().IsEmpty();
      }

      // One application-log line per start while the switch is on and an
      // address is empty, saying that nothing is held and why.
      void ReportInactive()
      {
         Configuration *configuration = Configuration::Instance();
         const bool submitEmpty = configuration->GetSandboxSubmitUrl().IsEmpty();
         const bool verdictEmpty = configuration->GetSandboxVerdictUrl().IsEmpty();

         if (!configuration->GetSandboxEnabled() || (!submitEmpty && !verdictEmpty))
         {
            inactive_reported = false;
            return;
         }

         if (inactive_reported.exchange(true))
            return;

         LOG_APPLICATION(Formatter::Format(_T("Sandbox - SandboxEnabled is on, but {0} empty, so nothing is held and nothing is submitted until both addresses are set."),
            String(submitEmpty && verdictEmpty ? _T("SandboxSubmitUrl and SandboxVerdictUrl are") : submitEmpty ? _T("SandboxSubmitUrl is") : _T("SandboxVerdictUrl is"))));
      }

      // ---- the settings --------------------------------------------------

      const wchar_t *const TopLevelTypes[] =
      {
         L"application", L"audio", L"font", L"image", L"message", L"model", L"multipart", L"text", L"video"
      };

      bool IsExtensionCharacter(wchar_t c)
      {
         return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
      }

      // RFC 6838 4.2's restricted name characters.
      bool IsMediaTypeCharacter(wchar_t c)
      {
         return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                c == '!' || c == '#' || c == '$' || c == '&' || c == '-' || c == '^' || c == '_' || c == '.' || c == '+';
      }

      // The list, split on commas, semicolons and white space. Read from the
      // stored text, which every interface has already canonicalised.
      std::vector<String> TypeList(const String &text)
      {
         String list = text;
         list.Replace(_T(";"), _T(","));
         list.Replace(_T(" "), _T(","));
         list.Replace(_T("\t"), _T(","));
         list.Replace(_T("\r"), _T(","));
         list.Replace(_T("\n"), _T(","));

         std::vector<String> types;
         for (String entry : StringParser::SplitString(list, _T(",")))
         {
            entry.Trim();
            entry.ToLower();
            if (!entry.IsEmpty())
               types.push_back(entry);
         }

         return types;
      }

      // ---- the parts of a message ------------------------------------------

      // One attachment, with every name a mail client may save it under: the
      // Content-Disposition filename, the Content-Type name - a client that
      // prefers the one the other ignores saves the file under a different
      // extension - and the name this server gives a part that has neither.
      struct Part
      {
         Part() : forced(false) {}

         String file_name;
         std::vector<String> names;
         AnsiString content_type;
         std::shared_ptr<MimeBody> body;

         // A forwarded message past MaxForwardedDepth: not opened, and so
         // submitted whole whatever its type.
         bool forced;
      };

      void CollectParts(std::shared_ptr<MimeBody> root, int depth, std::vector<Part> &parts)
      {
         if (!root)
            return;

         MimeBody::BodyList list;
         root->GetAttachmentList(root, list);

         for (std::shared_ptr<MimeBody> body : list)
         {
            if (!body)
               continue;

            Part part;
            part.body = body;
            part.file_name = body->GetUnicodeFilename();

            const char *contentType = body->GetContentType();
            part.content_type = contentType ? contentType : "";

            const AnsiString dispositionName = body->GetParameter(CMimeConst::ContentDisposition(), CMimeConst::Filename());
            if (!dispositionName.IsEmpty())
               part.names.push_back(MimeHeader::GetUnicodeFieldValue(CMimeConst::ContentDisposition(), dispositionName));

            const AnsiString typeName = body->GetParameter(CMimeConst::ContentType(), CMimeConst::Name());
            if (!typeName.IsEmpty())
               part.names.push_back(MimeHeader::GetUnicodeFieldValue(CMimeConst::ContentType(), typeName));

            part.names.push_back(part.file_name);

            // A forwarded message is one attachment to the MIME walk, named
            // after its subject; what it carries is attached as much as what
            // the outer message carries, so it is opened and its own parts
            // judged too.
            if (body->IsEncapsulatedRFC822Message())
            {
               if (depth < MaxForwardedDepth)
                  CollectParts(body->LoadEncapsulatedMessage(), depth + 1, parts);
               else
                  part.forced = true;
            }

            parts.push_back(part);
         }
      }

      // A name's extension as Windows will save the file: trailing dots and
      // white space are dropped by Win32 path normalisation and by the save
      // dialogs of browsers and mail clients, so "invoice.exe." is saved as
      // invoice.exe and must be judged as one.
      String ExtensionOf(const String &name)
      {
         String saved = name;
         saved.TrimRight(_T(". \t\r\n"));

         const int dot = saved.ReverseFind(_T('.'));
         if (dot < 0 || dot + 1 >= saved.GetLength())
            return String();

         String extension = saved.Mid(dot + 1);
         extension.Trim();
         extension.ToLower();
         return extension;
      }

      // Whether a part is of a submitted type: any of its names' last
      // extension, or its media type without parameters.
      bool IsListed(const std::vector<String> &types, const Part &part)
      {
         if (part.forced)
            return true;

         std::vector<String> extensions;
         for (const String &name : part.names)
         {
            const String extension = ExtensionOf(name);
            if (!extension.IsEmpty())
               extensions.push_back(extension);
         }

         String mediaType = String(part.content_type);
         const int semicolon = mediaType.Find(_T(";"));
         if (semicolon >= 0)
            mediaType = mediaType.Mid(0, semicolon);
         mediaType.Trim();
         mediaType.ToLower();

         for (const String &type : types)
         {
            for (const String &extension : extensions)
            {
               if (type == extension)
                  return true;
            }

            if (!mediaType.IsEmpty() && type == mediaType)
               return true;
         }

         return false;
      }

      // ---- the rows ------------------------------------------------------

      Sandbox::File ReadFileRow(std::shared_ptr<DALRecordset> recordset)
      {
         Sandbox::File file;
         file.id = recordset->GetInt64Value("shid");
         file.message_id = recordset->GetInt64Value("shmessageid");
         file.sha256 = AnsiString(recordset->GetStringValue("shsha256"));
         file.filename = recordset->GetStringValue("shfilename");
         file.size = recordset->GetInt64Value("shsize");
         file.submission = recordset->GetStringValue("shsubmission");
         file.held = recordset->GetInt64Value("shheld");
         file.state = (int) recordset->GetLongValue("shstate");
         file.verdict = recordset->GetStringValue("shverdict");
         file.score = (int) recordset->GetLongValue("shscore");
         file.detail = recordset->GetStringValue("shdetail");
         file.errors = (int) recordset->GetLongValue("sherrors");
         file.decision = (int) recordset->GetLongValue("shdecision");
         return file;
      }

      // False when the database did not answer; an empty list is a message
      // with no hold.
      bool ReadFiles(__int64 messageId, std::vector<Sandbox::File> &files)
      {
         files.clear();

         SQLCommand command("select * from hm_sandboxholds where shmessageid = @MESSAGEID order by shid");
         command.AddParameter("@MESSAGEID", messageId);

         std::shared_ptr<DALRecordset> recordset = QueryRows(command);
         if (!recordset)
            return false;

         while (!recordset->IsEOF())
         {
            files.push_back(ReadFileRow(recordset));
            recordset->MoveNext();
         }

         return true;
      }

      bool InsertFile(const Sandbox::File &file)
      {
         std::shared_ptr<DatabaseConnectionManager> db = Db();
         if (!db)
            return false;

         SQLStatement statement(SQLStatement::STInsert, "hm_sandboxholds");
         statement.AddColumnInt64("shmessageid", file.message_id);
         statement.AddColumn("shsha256", String(file.sha256));
         statement.AddColumn("shfilename", Storable(file.filename, 255));
         statement.AddColumnInt64("shsize", file.size);
         statement.AddColumn("shsubmission", Storable(file.submission, MaxSubmissionLength));
         statement.AddColumnInt64("shheld", file.held);
         statement.AddColumnInt64("shstate", (__int64) file.state);
         statement.AddColumn("shverdict", ClipText(file.verdict, 16));
         statement.AddColumnInt64("shscore", (__int64) file.score);
         statement.AddColumn("shdetail", Storable(file.detail, 255));
         statement.AddColumnInt64("sherrors", (__int64) file.errors);
         statement.AddColumnInt64("shdecision", (__int64) file.decision);
         statement.SetIdentityColumn("shid");

         __int64 made = 0;
         return db->Execute(statement, &made) && made > 0;
      }

      bool UpdateFile(const Sandbox::File &file)
      {
         SQLCommand command("update hm_sandboxholds set shsubmission = @SUBMISSION, shstate = @STATE, shverdict = @VERDICT, shscore = @SCORE, shdetail = @DETAIL, sherrors = @ERRORS where shid = @ID");
         command.AddParameter("@SUBMISSION", Storable(file.submission, MaxSubmissionLength));
         command.AddParameter("@STATE", file.state);
         command.AddParameter("@VERDICT", ClipText(file.verdict, 16));
         command.AddParameter("@SCORE", file.score);
         command.AddParameter("@DETAIL", Storable(file.detail, 255));
         command.AddParameter("@ERRORS", file.errors);
         command.AddParameter("@ID", file.id);
         return RunStatement(command);
      }

      bool DeleteFiles(__int64 messageId)
      {
         SQLCommand command("delete from hm_sandboxholds where shmessageid = @MESSAGEID");
         command.AddParameter("@MESSAGEID", messageId);
         return RunStatement(command);
      }

      // Records a decision on a hold that has none yet. A hold another hand
      // decided in the meantime - the poller's verdict, an administrator's
      // release, the timeout - keeps that decision: the statement matches only
      // an undecided row, and the caller reads back which decision stands.
      bool Decide(__int64 messageId, int decision)
      {
         SQLCommand command("update hm_sandboxholds set shdecision = @DECISION where shmessageid = @MESSAGEID and shdecision = 0");
         command.AddParameter("@DECISION", decision);
         command.AddParameter("@MESSAGEID", messageId);
         return RunStatement(command);
      }

      // The decision the rows carry now; -1 when there are none or they could
      // not be read.
      int DecisionOf(__int64 messageId)
      {
         std::vector<Sandbox::File> files;
         if (!ReadFiles(messageId, files) || files.empty())
            return -1;

         return files[0].decision;
      }

      // The message's hm_messages.messagetype. answered is false when the
      // database did not answer, and type is -1 when the row is gone: the two
      // must never be confused, or a database that blinks deletes a hold.
      int MessageType(__int64 messageId, bool &answered)
      {
         answered = false;

         SQLCommand command("select messagetype from hm_messages where messageid = @MESSAGEID");
         command.AddParameter("@MESSAGEID", messageId);

         std::shared_ptr<DALRecordset> recordset = QueryRows(command);
         if (!recordset)
            return -1;

         answered = true;

         if (recordset->IsEOF())
            return -1;

         return (int) recordset->GetLongValue("messagetype");
      }

      // Into the hold: out of the delivery queue, unlocked, and only while the
      // row is still an ordinary queued message.
      bool HoldMessage(__int64 messageId)
      {
         SQLCommand command("update hm_messages set messagetype = 5, messagelocked = 0 where messageid = @MESSAGEID and messagetype = 1");
         command.AddParameter("@MESSAGEID", messageId);
         if (!RunStatement(command))
            return false;

         bool answered = false;
         return MessageType(messageId, answered) == Message::HeldForSandbox;
      }

      // Back into the delivery queue, due now, and the deliverer woken: the
      // statement the compromised-account detection's release is written as.
      // Only a row still held for the sandbox: one the compromised-account
      // detection moved to review (messagetype 4) stays there.
      bool ReleaseMessage(__int64 messageId)
      {
         SQLCommand command("update hm_messages set messagetype = 1, messagelocked = 0, messagenexttrytime = '1901-01-01 00:00:00' where messageid = @MESSAGEID and messagetype = 5");
         command.AddParameter("@MESSAGEID", messageId);
         if (!RunStatement(command))
            return false;

         Application::Instance()->SubmitPendingEmail();
         return true;
      }

      // Kept in the queue for a minute rather than held or delivered. Not
      // counted as a try, so the attempts are counted here instead.
      void Defer(std::shared_ptr<Message> message)
      {
         PersistentMessage::SetNextTryTime(message->GetID(), false, 1);
         PersistentMessage::UnlockObject(message);
      }

      int CountAttempt(__int64 messageId)
      {
         boost::lock_guard<boost::mutex> guard(attempts_mutex);

         // Bounded: a message that is gone leaves its count behind.
         if (attempts.size() > 10000)
            attempts.clear();

         return ++attempts[messageId];
      }

      void ForgetAttempts(__int64 messageId)
      {
         boost::lock_guard<boost::mutex> guard(attempts_mutex);
         attempts.erase(messageId);
      }

      // ---- the verdict cache ---------------------------------------------

      struct Remembered
      {
         Remembered() : score(0) {}

         String verdict;
         int score;
         String detail;
      };

      bool LookUpVerdict(const AnsiString &sha256, Remembered &remembered)
      {
         if (Configuration::Instance()->GetSandboxCacheHours() <= 0)
            return false;

         SQLCommand command("select svverdict, svscore, svdetail, svexpires from hm_sandboxverdicts where svsha256 = @SHA256");
         command.AddParameter("@SHA256", String(sha256));

         std::shared_ptr<DALRecordset> recordset = QueryRows(command);
         if (!recordset || recordset->IsEOF())
            return false;

         if (recordset->GetInt64Value("svexpires") <= NowSeconds())
            return false;

         remembered.verdict = recordset->GetStringValue("svverdict");
         remembered.score = (int) recordset->GetLongValue("svscore");
         remembered.detail = recordset->GetStringValue("svdetail");
         return remembered.verdict == _T("clean") || remembered.verdict == _T("malicious");
      }

      // An update of the row the hash already has, or an insert when it has
      // none. Two pollers can answer the same file - two servers on one
      // database, each polling - and the one that inserts second is refused
      // by the unique index on svsha256: that refusal is expected and not
      // reported, and the row the first wrote stands, with the same answer.
      void RememberVerdict(const AnsiString &sha256, const String &verdict, int score, const String &detail)
      {
         const int hours = Configuration::Instance()->GetSandboxCacheHours();
         if (hours <= 0 || sha256.IsEmpty())
            return;

         std::shared_ptr<DatabaseConnectionManager> db = Db();
         if (!db)
            return;

         const __int64 now = NowSeconds();

         SQLCommand existing("select svid from hm_sandboxverdicts where svsha256 = @SHA256");
         existing.AddParameter("@SHA256", String(sha256));
         std::shared_ptr<DALRecordset> recordset = db->OpenRecordset(existing);
         if (!recordset)
            return;

         if (!recordset->IsEOF())
         {
            SQLCommand update("update hm_sandboxverdicts set svverdict = @VERDICT, svscore = @SCORE, svdetail = @DETAIL, svanswered = @ANSWERED, svexpires = @EXPIRES where svsha256 = @SHA256");
            update.AddParameter("@VERDICT", verdict);
            update.AddParameter("@SCORE", score);
            update.AddParameter("@DETAIL", Storable(detail, 255));
            update.AddParameter("@ANSWERED", now);
            update.AddParameter("@EXPIRES", now + (__int64) hours * 3600);
            update.AddParameter("@SHA256", String(sha256));
            db->Execute(update);
            return;
         }

         SQLStatement statement(SQLStatement::STInsert, "hm_sandboxverdicts");
         statement.AddColumn("svsha256", String(sha256));
         statement.AddColumn("svverdict", verdict);
         statement.AddColumnInt64("svscore", (__int64) score);
         statement.AddColumn("svdetail", Storable(detail, 255));
         statement.AddColumnInt64("svanswered", now);
         statement.AddColumnInt64("svexpires", now + (__int64) hours * 3600);
         statement.SetIdentityColumn("svid");

         __int64 made = 0;
         db->Execute(statement, &made, DALConnection::DALErrorInSQL);
      }

      void SweepVerdicts()
      {
         const __int64 now = NowSeconds();
         if (last_sweep != 0 && now - last_sweep < SweepIntervalSeconds && now >= last_sweep)
            return;

         last_sweep = now;

         SQLCommand command("delete from hm_sandboxverdicts where svexpires <= @NOW");
         command.AddParameter("@NOW", now);
         RunStatement(command);
      }

      // ---- the verdicts --------------------------------------------------

      // What an answered file means. A suspicious answer at or above the
      // threshold is malicious; below it, clean. An error is neither.
      String EffectiveVerdict(const String &verdict, int score)
      {
         if (verdict == _T("suspicious"))
            return score >= Configuration::Instance()->GetSandboxScoreThreshold() ? String(_T("malicious")) : String(_T("clean"));

         return verdict;
      }

      bool IsMalicious(const Sandbox::File &file)
      {
         return file.state == Sandbox::FileAnswered && EffectiveVerdict(file.verdict, file.score) == _T("malicious");
      }

      int CountedFiles(const std::vector<Sandbox::File> &files)
      {
         int count = 0;
         for (const Sandbox::File &file : files)
         {
            if (file.state != FileNotSubmittable)
               count++;
         }
         return count;
      }

      // "not submitted: a.zip, b.iso (larger than 20480 KB, or unreadable)", or nothing.
      String NotSubmittedNote(const std::vector<String> &names)
      {
         if (names.empty())
            return String();

         String list;
         for (const String &name : names)
            list += (list.IsEmpty() ? _T("") : _T(", ")) + OneLine(name);

         return Formatter::Format(_T("not submitted: {0} (larger than {1} KB, or unreadable)"), list, Configuration::Instance()->GetSandboxMaxSizeKB());
      }

      String NotSubmittedNote(const std::vector<Sandbox::File> &files)
      {
         std::vector<String> names;
         for (const Sandbox::File &file : files)
         {
            if (file.state == FileNotSubmittable && !file.filename.IsEmpty())
               names.push_back(file.filename);
         }

         return NotSubmittedNote(names);
      }

      // Whether a header block carries a field of this name whose value starts
      // so (case aside). The header's own lines, not its folded continuations.
      bool HeaderHasField(const AnsiString &header, const AnsiString &name, const AnsiString &valueStart)
      {
         const AnsiString prefix = name + ":";

         for (AnsiString line : StringParser::SplitString(header, "\r\n"))
         {
            if (!line.StartsWith(prefix))
               continue;

            AnsiString value = line.Mid(prefix.GetLength());
            value.Trim();
            if (valueStart.IsEmpty() || value.StartsWith(valueStart))
               return true;
         }

         return false;
      }

      bool WriteHeader(std::shared_ptr<Message> message, const String &value)
      {
         const String fileName = MessageStore::WorkingFile(message);

         std::vector<std::pair<AnsiString, AnsiString> > fields;
         fields.push_back(std::make_pair(AnsiString("X-hMailServer-Sandbox"), HeaderSafe(value)));

         TraceHeaderWriter writer;
         if (!writer.Write(fileName, message, fields))
         {
            ReportSandboxError(7273, _T("Sandbox::WriteHeader"),
               Formatter::Format(_T("The X-hMailServer-Sandbox header could not be written to message {0} ({1}). The message is delivered without it."), message->GetID(), value));
            return false;
         }

         message->SetSize((int) MessageStore::FileSize(fileName));
         return true;
      }

      // A sender's own X-hMailServer-Sandbox, removed as the message enters
      // the server's hands - its first delivery pass, before anything is
      // examined - so that the field a reader sees is only ever this
      // server's, examined or not. Read cheaply first: only a message that
      // carries the field is parsed and written again.
      void RemoveSendersHeader(std::shared_ptr<Message> message)
      {
         const String fileName = MessageStore::WorkingFile(message);
         const AnsiString header = PersistentMessage::LoadHeader(fileName, false);
         if (!HeaderHasField(header, "X-hMailServer-Sandbox", ""))
            return;

         MessageData data;
         if (!data.LoadFromMessage(fileName, message))
         {
            LOG_APPLICATION(Formatter::Format(_T("Sandbox - Message {0} arrived carrying an X-hMailServer-Sandbox header of its own, which could not be removed: the message could not be read."), message->GetID()));
            return;
         }

         data.DeleteField("X-hMailServer-Sandbox");

         if (!data.Write(fileName))
         {
            LOG_APPLICATION(Formatter::Format(_T("Sandbox - Message {0} arrived carrying an X-hMailServer-Sandbox header of its own, which could not be removed: the message could not be written."), message->GetID()));
            return;
         }

         message->SetSize((int) MessageStore::FileSize(fileName));

         LOG_DEBUG(Formatter::Format(_T("Sandbox - Message {0}: the X-hMailServer-Sandbox header it arrived with was removed."), message->GetID()));
      }

      String Recipients(std::shared_ptr<Message> message)
      {
         return ClipText(message->GetRecipients()->GetCommaSeperatedRecipientList(), 255);
      }

      void RecordTrace(std::shared_ptr<Message> message, const wchar_t *eventName, const String &detail)
      {
         MessageTrace::Record(message->GetID(), eventName, message->GetFromAddress(), Recipients(message),
                              MessageUtilities::GetSendersIP(message), 0, Storable(detail, 255));
      }

      AnsiString Bearer()
      {
         const String token = Configuration::Instance()->GetSandboxToken();
         if (token.IsEmpty())
            return AnsiString();

         return "Authorization: Bearer " + ToUtf8(token);
      }

      // A multipart/form-data field, appended byte for byte.
      void AppendField(AnsiString &body, const AnsiString &boundary, const char *name, const AnsiString &value)
      {
         body.append("--");
         body.append(boundary);
         body.append("\r\nContent-Disposition: form-data; name=\"");
         body.append(name);
         body.append("\"\r\n\r\n");
         body.append(value);
         body.append("\r\n");
      }

      // The file name as a quoted-string's content: ASCII, with nothing that
      // would end the parameter.
      AnsiString QuotedFileName(const String &name)
      {
         AnsiString result;
         for (int i = 0; i < name.GetLength() && result.GetLength() < 200; i++)
         {
            const wchar_t c = name.GetAt(i);
            if (c < 32 || c > 126 || c == '"' || c == '\\')
               result += '_';
            else
               result += (char) c;
         }

         return result.IsEmpty() ? AnsiString("attachment") : result;
      }

      // The id the submit answered: {"id":"..."} or {"id":123}; empty when the
      // body names none, which is polled by the SHA-256 instead. False, with the
      // problem, for an id longer than this server stores: every verdict
      // request would ask for an id the sandbox never gave.
      bool SubmissionId(const AnsiString &body, String &submission, String &problem)
      {
         submission = String();

         JsonValue json;
         std::string error;
         if (!JsonValue::Parse(std::string(body.data(), body.size()), json, error) || !json.IsObject())
            return true;

         const JsonValue *id = json.Get("id");
         if (!id)
            return true;

         if (id->IsString())
            submission = OneLine(FromUtf8(id->AsString()));
         else if (id->IsNumber())
            submission = StringParser::IntToString(id->AsInt64());

         if (submission.GetLength() > MaxSubmissionLength)
         {
            problem = Formatter::Format(_T("the sandbox answered an id of {0} characters, longer than the {1} this server stores"), submission.GetLength(), MaxSubmissionLength);
            submission = String();
            return false;
         }

         return true;
      }

      // What is left of a pass's budget, for one request: never more than the
      // request's own ceiling, never less than five seconds.
      int RequestTimeout(int ceiling, __int64 deadline)
      {
         __int64 remaining = deadline - NowSeconds();
         if (remaining < 5)
            remaining = 5;

         return remaining < (__int64) ceiling ? (int) remaining : ceiling;
      }

      bool SubmitFile(__int64 messageId, const String &fileName, const AnsiString &content, const AnsiString &sha256, int timeoutSeconds,
                      String &submission, String &problem)
      {
         const String url = Configuration::Instance()->GetSandboxSubmitUrl();
         if (url.IsEmpty())
         {
            problem = _T("SandboxSubmitUrl is empty");
            return false;
         }

         String guid = GUIDCreator::GetGUID();
         guid.Replace(_T("{"), _T(""));
         guid.Replace(_T("}"), _T(""));
         guid.Replace(_T("-"), _T(""));
         const AnsiString boundary = "hMailServerSandbox" + AnsiString(guid);

         AnsiString body;
         AppendField(body, boundary, "filename", ToUtf8(fileName));
         AppendField(body, boundary, "sha256", sha256);
         AppendField(body, boundary, "message_id", AnsiString(StringParser::IntToString(messageId)));
         body.append("--");
         body.append(boundary);
         body.append("\r\nContent-Disposition: form-data; name=\"file\"; filename=\"");
         body.append(QuotedFileName(fileName));
         body.append("\"\r\nContent-Type: application/octet-stream\r\n\r\n");
         body.append(content);
         body.append("\r\n--");
         body.append(boundary);
         body.append("--\r\n");

         std::vector<AnsiString> headers;
         const AnsiString bearer = Bearer();
         if (!bearer.IsEmpty())
            headers.push_back(bearer);

         HttpsClient::Response response;
         String error;
         if (!HttpsClient::Request("POST", AnsiString(url), headers, "multipart/form-data; boundary=" + boundary, body, response, error,
                                   timeoutSeconds, MaxResponseBytes))
         {
            problem = error;
            return false;
         }

         if (response.status_code < 200 || response.status_code > 299)
         {
            problem = Formatter::Format(_T("the sandbox answered HTTP {0}"), response.status_code);
            return false;
         }

         return SubmissionId(response.body, submission, problem);
      }

      struct Answer
      {
         Answer() : done(false), score(0) {}

         bool done;
         String verdict;
         int score;
         String detail;
      };

      bool AskVerdict(const Sandbox::File &file, int timeoutSeconds, Answer &answer, String &problem)
      {
         const String url = Configuration::Instance()->GetSandboxVerdictUrl();
         if (url.IsEmpty())
         {
            problem = _T("SandboxVerdictUrl is empty");
            return false;
         }

         AnsiString target = AnsiString(url);

         // The submission's id, or - when the submit answered none - the hash,
         // which is what a service that names its jobs by hash polls by.
         const AnsiString id = file.submission.IsEmpty() ? file.sha256 : HttpsClient::FormEncode(ToUtf8(file.submission));

         if (target.Find("{id}") >= 0 || target.Find("{sha256}") >= 0)
         {
            target.Replace("{id}", id.c_str());
            target.Replace("{sha256}", file.sha256.c_str());
         }
         else
         {
            target += target.Find("?") >= 0 ? "&" : "?";
            if (!file.submission.IsEmpty())
               target += "id=" + id;
            else
               target += "sha256=" + file.sha256;
         }

         std::vector<AnsiString> headers;
         const AnsiString bearer = Bearer();
         if (!bearer.IsEmpty())
            headers.push_back(bearer);

         HttpsClient::Response response;
         String error;
         if (!HttpsClient::Request("GET", target, headers, "", "", response, error, timeoutSeconds, MaxResponseBytes))
         {
            problem = error;
            return false;
         }

         if (response.status_code != 200)
         {
            problem = Formatter::Format(_T("the sandbox answered HTTP {0}"), response.status_code);
            return false;
         }

         JsonValue json;
         std::string parseError;
         if (!JsonValue::Parse(std::string(response.body.data(), response.body.size()), json, parseError) || !json.IsObject())
         {
            problem = _T("the sandbox's answer is not a JSON object");
            return false;
         }

         const std::string state = json.GetString("state");
         if (state == "pending")
         {
            answer.done = false;
            return true;
         }

         if (state != "done")
         {
            problem = _T("the sandbox's answer has a state that is neither pending nor done");
            return false;
         }

         String verdict = FromUtf8(json.GetString("verdict"));
         verdict.Trim();
         verdict.ToLower();
         if (verdict != _T("clean") && verdict != _T("malicious") && verdict != _T("suspicious") && verdict != _T("error"))
         {
            problem = _T("the sandbox's verdict is not clean, malicious, suspicious or error");
            return false;
         }

         const String detail = ClipText(OneLine(FromUtf8(json.GetString("detail"))), 255);

         // The score, when it is a number. A suspicious answer is only as good
         // as its score: without a numeric one - absent, null, or the digits in
         // a string - it is an error, never clean, and never remembered, since
         // the threshold has nothing to judge.
         const JsonValue *scoreValue = json.Get("score");
         __int64 score = 0;
         if (scoreValue && scoreValue->IsNumber())
         {
            score = scoreValue->AsInt64();
         }
         else if (verdict == _T("suspicious"))
         {
            answer.done = true;
            answer.verdict = _T("error");
            answer.score = 0;
            answer.detail = ClipText(String(_T("answered suspicious without a numeric score")) + (detail.IsEmpty() ? String() : String(_T(": ")) + detail), 255);
            return true;
         }

         if (score < 0) score = 0;
         if (score > 100) score = 100;

         answer.done = true;
         answer.verdict = verdict;
         answer.score = (int) score;
         answer.detail = detail;
         return true;
      }

      // The files of a queued message the poller is to submit, by SHA-256,
      // from one read of the message whatever their number.
      bool LoadContents(__int64 messageId, const std::set<AnsiString> &wanted, std::map<AnsiString, AnsiString> &contents)
      {
         contents.clear();

         std::shared_ptr<Message> message = std::shared_ptr<Message>(new Message());
         if (!PersistentMessage::ReadObject(message, messageId))
            return false;

         std::shared_ptr<MessageData> data = std::shared_ptr<MessageData>(new MessageData());
         if (!data->LoadFromMessage(MessageStore::WorkingFile(message), message))
            return false;

         std::vector<Part> parts;
         CollectParts(data->GetMimeMessage(), 0, parts);

         for (Part &part : parts)
         {
            AnsiString bytes;
            if (!part.body || !part.body->GetDecodedContent(bytes))
               continue;

            const AnsiString sha256 = Sha256Hex(bytes);
            if (wanted.find(sha256) != wanted.end())
               contents[sha256] = bytes;
         }

         return true;
      }

      // The file the quarantine is to name: the first one the decision rests on.
      const Sandbox::File *ReasonFile(const std::vector<Sandbox::File> &files, int decision)
      {
         for (const Sandbox::File &file : files)
         {
            if (decision == Sandbox::DecisionMalicious && IsMalicious(file))
               return &file;

            if (decision == Sandbox::DecisionError && file.state == Sandbox::FileAnswered && file.verdict == _T("error"))
               return &file;
         }

         // The threshold moved between the poller's decision and now: the first
         // file with any answer that is not clean.
         for (const Sandbox::File &file : files)
         {
            if (file.state == Sandbox::FileAnswered && file.verdict != _T("clean"))
               return &file;
         }

         return nullptr;
      }

      String Summary(int count, __int64 seconds)
      {
         return Formatter::Format(_T("{0} file(s), {1}s"), count, seconds < 0 ? (__int64) 0 : seconds);
      }

      String WithNote(const String &value, const String &note)
      {
         return note.IsEmpty() ? value : value + _T("; ") + note;
      }

      // ---- the ends of a hold, and of an examination -------------------------

      // A message the sandbox found malicious: quarantined with the verdict as
      // the reason - or, while the quarantine is off (QuarantineEnabled 0, as
      // shipped), returned to its sender with 5.7.1, since a store no digest
      // reads and no release is offered from is not a place to leave it. The
      // header goes on the quarantined copy once: a quarantine that fails is
      // tried again at the next poll, and must not add a field each time.
      Sandbox::Result Malicious(std::shared_ptr<Message> message, const String &headerValue, const String &reason, int score)
      {
         Sandbox::Result result;

         if (!QuarantineStore::GetEnabled())
         {
            result.outcome = Sandbox::OutcomeBounce;
            result.reason = ClipText(_T("The sandbox this server submits attachments to found this message malicious: ") + reason +
                                     _T(". The quarantine on this server is off, so the message is returned rather than held for release."), 500);

            LOG_APPLICATION(Formatter::Format(_T("Sandbox - Message {0}: malicious, and QuarantineEnabled is 0, so it is neither delivered nor quarantined. {1}"), message->GetID(), reason));
            return result;
         }

         const AnsiString header = PersistentMessage::LoadHeader(MessageStore::WorkingFile(message), false);
         if (!HeaderHasField(header, "X-hMailServer-Sandbox", "malicious"))
            WriteHeader(message, headerValue);

         result.outcome = Sandbox::OutcomeQuarantine;
         result.score = score;
         result.reason = ClipText(String(QuarantineReasonPrefix) + reason, 255);
         return result;
      }

      // A message the sandbox could not examine and will not: more files than
      // SandboxMaxFiles, data that cannot be read, a hold that cannot be
      // recorded. Nothing about it is exempt; SandboxTimeoutAction decides it
      // as it decides a hold that timed out, with the header saying why.
      Sandbox::Result WithoutVerdict(std::shared_ptr<Message> message, const String &why)
      {
         Sandbox::Result result;
         const __int64 messageId = message->GetID();

         if (Configuration::Instance()->GetSandboxTimeoutAction() == 1)
         {
            result.outcome = Sandbox::OutcomeBounce;
            result.reason = ClipText(_T("The sandbox this server submits attachments to could not examine this message: ") + why +
                                     _T(". This server is configured to return such a message rather than deliver it."), 500);

            LOG_APPLICATION(Formatter::Format(_T("Sandbox - Message {0}: not examined - {1}; SandboxTimeoutAction is 1, so it is not delivered."), messageId, why));
            return result;
         }

         WriteHeader(message, _T("not examined: ") + why);

         LOG_APPLICATION(Formatter::Format(_T("Sandbox - Message {0}: not examined - {1}; SandboxTimeoutAction is 0, so it is delivered with a header saying so."), messageId, why));
         return result;
      }

      // A step that failed and is tried again a minute later, until the
      // attempts run out and SandboxTimeoutAction decides the message. The
      // message is kept in the queue meanwhile, undelivered.
      Sandbox::Result TryAgainLater(std::shared_ptr<Message> message, int code, const String &sentence, const String &why, bool forgetRows)
      {
         const __int64 messageId = message->GetID();

         if (CountAttempt(messageId) >= MaxAttempts)
         {
            ForgetAttempts(messageId);
            if (forgetRows)
               DeleteFiles(messageId);

            ReportSandboxError(code, _T("Sandbox::Examine"),
               Formatter::Format(_T("{0} This was attempt {1} in a row, the last: SandboxTimeoutAction decides the message."), sentence, MaxAttempts));
            return WithoutVerdict(message, why);
         }

         ReportSandboxError(code, _T("Sandbox::Examine"),
            sentence + _T(" It stays in the delivery queue, undelivered, and is tried again in a minute."));

         Defer(message);

         Sandbox::Result result;
         result.outcome = Sandbox::OutcomeHeld;
         return result;
      }

      // ---- the decision a returning message carries -------------------------

      Sandbox::Result Decided(std::shared_ptr<Message> message, const std::vector<Sandbox::File> &files)
      {
         Sandbox::Result result;

         const __int64 messageId = message->GetID();
         const int decision = files[0].decision;
         const __int64 seconds = files[0].held > 0 ? NowSeconds() - files[0].held : 0;
         const int count = CountedFiles(files);
         const String note = NotSubmittedNote(files);

         switch (decision)
         {
         case Sandbox::DecisionPending:
         {
            // Back in the queue while undecided: a hold whose type change did
            // not happen, or a message the compromised-account review released
            // while its files were still out. Held again, never submitted again.
            if (HoldMessage(messageId))
            {
               ForgetAttempts(messageId);
               submit_soon = true;
               result.outcome = Sandbox::OutcomeHeld;
               return result;
            }

            return TryAgainLater(message, 7271,
               Formatter::Format(_T("Message {0} could not be moved to the sandbox hold."), messageId),
               _T("the hold could not be recorded"), true);
         }

         case Sandbox::DecisionClean:
         {
            WriteHeader(message, WithNote(_T("clean (") + Summary(count, seconds) + _T(")"), note));
            DeleteFiles(messageId);

            LOG_APPLICATION(Formatter::Format(_T("Sandbox - Message {0}: every submitted file was answered clean ({1}); it is delivered."), messageId, Summary(count, seconds)));
            return result;
         }

         case Sandbox::DecisionMalicious:
         {
            const Sandbox::File *reason = ReasonFile(files, decision);
            const String fileName = reason ? OneLine(reason->filename) : String(_T("a file"));
            const String detail = reason ? reason->detail : String();
            const int score = reason ? reason->score : 0;
            const String verdict = reason ? reason->verdict : String(_T("malicious"));

            result = Malicious(message,
               Formatter::Format(_T("malicious ({0}): {1} {2} (score {3})"), Summary(count, seconds), fileName, verdict, score),
               Formatter::Format(_T("{0} is {1} (score {2}): {3}"), fileName, verdict, score, detail),
               score);

            // Returned: the rows go now. Quarantined: they go when the quarantine
            // has it (the deliverer's Forget), and stay while it is tried again.
            if (result.outcome == Sandbox::OutcomeBounce)
               DeleteFiles(messageId);

            return result;
         }

         case Sandbox::DecisionError:
         case Sandbox::DecisionTimedOut:
         {
            const Sandbox::File *reason = decision == Sandbox::DecisionError ? ReasonFile(files, decision) : nullptr;
            const String what = decision == Sandbox::DecisionTimedOut
               ? Formatter::Format(_T("timed out after {0} minute(s)"), Configuration::Instance()->GetSandboxTimeoutMinutes())
               : Formatter::Format(_T("error on {0}: {1}"), reason ? OneLine(reason->filename) : String(_T("a file")), reason ? reason->detail : String());

            DeleteFiles(messageId);

            if (Configuration::Instance()->GetSandboxTimeoutAction() == 1)
            {
               result.outcome = Sandbox::OutcomeBounce;
               result.reason = ClipText(_T("The sandbox this server submits attachments to did not give a verdict on this message: ") + what +
                                    _T(". This server is configured to return such a message rather than deliver it."), 500);

               LOG_APPLICATION(Formatter::Format(_T("Sandbox - Message {0}: {1} ({2}); SandboxTimeoutAction is 1, so it is not delivered."), messageId, what, Summary(count, seconds)));
               return result;
            }

            WriteHeader(message, WithNote(what + _T(" (") + Summary(count, seconds) + _T(")"), note));

            LOG_APPLICATION(Formatter::Format(_T("Sandbox - Message {0}: {1} ({2}); SandboxTimeoutAction is 0, so it is delivered with a header saying so."), messageId, what, Summary(count, seconds)));
            return result;
         }

         case Sandbox::DecisionReleased:
         {
            WriteHeader(message, WithNote(_T("released by administrator (") + Summary(count, seconds) + _T(")"), note));
            DeleteFiles(messageId);

            LOG_APPLICATION(Formatter::Format(_T("Sandbox - Message {0}: released by an administrator without the sandbox's verdict ({1}); it is delivered."), messageId, Summary(count, seconds)));
            return result;
         }

         case Sandbox::DecisionTurnedOff:
         default:
         {
            WriteHeader(message, WithNote(_T("not examined: the sandbox was turned off, or an address emptied, while the message was held (") + Summary(count, seconds) + _T(")"), note));
            DeleteFiles(messageId);

            LOG_APPLICATION(Formatter::Format(_T("Sandbox - Message {0}: the sandbox was turned off, or an address emptied, while it was held ({1}); it is delivered without a verdict."), messageId, Summary(count, seconds)));
            return result;
         }
         }
      }

      // ---- a message seen for the first time --------------------------------

      struct Candidate
      {
         Candidate() : size(0), cached(false), score(0) {}

         String file_name;
         AnsiString sha256;
         __int64 size;
         bool cached;
         String verdict;
         int score;
         String detail;
      };

      Sandbox::Result Examined(std::shared_ptr<Message> message)
      {
         Sandbox::Result result;

         const __int64 messageId = message->GetID();

         const std::vector<String> types = TypeList(Configuration::Instance()->GetSandboxTypes());
         if (types.empty())
            return result;

         // A message that cannot be read is not delivered unexamined. Over the
         // size the MIME parser takes it never will be read, and is decided at
         // once; otherwise the file may be held by another process for a moment
         // (an on-access scanner), and it is tried again a minute later, a few
         // times, before SandboxTimeoutAction decides it.
         const String fileName = MessageStore::WorkingFile(message);
         std::shared_ptr<MessageData> data = std::shared_ptr<MessageData>(new MessageData());
         if (!data->LoadFromMessage(fileName, message))
         {
            unsigned __int64 bytes = 0;
            if (FileUtilities::FileSize64(fileName, bytes) && bytes > MaxParsedBytes)
               return WithoutVerdict(message, _T("the message is larger than 80 MB, which this server does not parse"));

            return TryAgainLater(message, 7270,
               Formatter::Format(_T("Message {0} could not be read, so its attachments could not be examined."), messageId),
               _T("the message could not be read"), false);
         }

         std::vector<Part> parts;
         CollectParts(data->GetMimeMessage(), 0, parts);

         const __int64 maxBytes = (__int64) Configuration::Instance()->GetSandboxMaxSizeKB() * 1024;

         std::vector<Candidate> candidates;
         std::vector<String> notSubmittable;

         for (Part &part : parts)
         {
            if (!IsListed(types, part))
               continue;

            const String name = part.file_name.IsEmpty() ? String(_T("(unnamed)")) : part.file_name;

            AnsiString content;
            if (!part.body || !part.body->GetDecodedContent(content) || (__int64) content.size() > maxBytes)
            {
               notSubmittable.push_back(name);
               continue;
            }

            const AnsiString sha256 = Sha256Hex(content);

            // The same file twice in one message is submitted once.
            bool seen = false;
            for (const Candidate &candidate : candidates)
            {
               if (candidate.sha256 == sha256)
                  seen = true;
            }
            if (seen)
               continue;

            Candidate candidate;
            candidate.file_name = name;
            candidate.sha256 = sha256;
            candidate.size = (__int64) content.size();
            candidates.push_back(candidate);
         }

         const String note = NotSubmittedNote(notSubmittable);

         if (candidates.empty())
         {
            if (!notSubmittable.empty())
            {
               WriteHeader(message, note);
               LOG_APPLICATION(Formatter::Format(_T("Sandbox - Message {0}: {1}; it is delivered without a verdict."), messageId, note));
            }

            return result;
         }

         // The cache: a file answered within SandboxCacheHours is not submitted
         // again, and a remembered malicious one ends the matter at once.
         int toSubmit = 0;
         for (Candidate &candidate : candidates)
         {
            Remembered remembered;
            if (LookUpVerdict(candidate.sha256, remembered))
            {
               candidate.cached = true;
               candidate.verdict = remembered.verdict;
               candidate.score = remembered.score;
               candidate.detail = remembered.detail;

               if (remembered.verdict == _T("malicious"))
               {
                  const String what = Formatter::Format(_T("{0} was answered malicious before (score {1}): {2}"), OneLine(candidate.file_name), remembered.score, remembered.detail);

                  RecordTrace(message, MessageTrace::EventSandboxVerdict,
                     Formatter::Format(_T("{0} sha256 {1}: malicious, remembered (score {2}) {3}"), candidate.file_name, String(candidate.sha256), remembered.score, remembered.detail));

                  result = Malicious(message,
                     Formatter::Format(_T("malicious (remembered verdict): {0} (score {1})"), OneLine(candidate.file_name), remembered.score),
                     what, remembered.score);

                  if (result.outcome == Sandbox::OutcomeQuarantine)
                  {
                     LOG_APPLICATION(Formatter::Format(_T("Sandbox - Message {0}: {1}; it is quarantined without a submission."), messageId, what));
                  }

                  return result;
               }
            }
            else
            {
               toSubmit++;
            }
         }

         if (toSubmit == 0)
         {
            WriteHeader(message, WithNote(_T("clean (") + Summary((int) candidates.size(), 0) + _T(")"), note));
            LOG_APPLICATION(Formatter::Format(_T("Sandbox - Message {0}: every file of a submitted type was answered clean before ({1} file(s)); it is delivered without a submission."), messageId, (int) candidates.size()));
            return result;
         }

         // One message may send at most SandboxMaxFiles files to the sandbox:
         // each is a submission against the administrator's quota and a request
         // the poller makes at every pass. One over it is not examined, and its
         // action says what becomes of it.
         const int maxFiles = Configuration::Instance()->GetSandboxMaxFiles();
         if (toSubmit > maxFiles)
            return WithoutVerdict(message, Formatter::Format(_T("{0} files to submit, more than SandboxMaxFiles ({1})"), toSubmit, maxFiles));

         // Held, with every file recorded as not yet submitted: the poller
         // submits them, off this delivery thread, within a second.
         const __int64 held = NowSeconds();
         std::vector<Sandbox::File> files;

         for (const Candidate &candidate : candidates)
         {
            Sandbox::File file;
            file.message_id = messageId;
            file.sha256 = candidate.sha256;
            file.filename = candidate.file_name;
            file.size = candidate.size;
            file.held = held;
            file.decision = Sandbox::DecisionPending;

            if (candidate.cached)
            {
               file.state = Sandbox::FileAnswered;
               file.verdict = candidate.verdict;
               file.score = candidate.score;
               file.detail = candidate.detail;
            }
            else
            {
               file.state = Sandbox::FileNotSubmitted;
            }

            files.push_back(file);
         }

         for (const String &name : notSubmittable)
         {
            Sandbox::File file;
            file.message_id = messageId;
            file.filename = name;
            file.held = held;
            file.state = FileNotSubmittable;
            file.decision = Sandbox::DecisionPending;
            files.push_back(file);
         }

         for (const Sandbox::File &file : files)
         {
            if (!InsertFile(file))
            {
               DeleteFiles(messageId);
               return TryAgainLater(message, 7270,
                  Formatter::Format(_T("The sandbox hold of message {0} could not be recorded."), messageId),
                  _T("the hold could not be recorded"), false);
            }
         }

         if (!HoldMessage(messageId))
         {
            return TryAgainLater(message, 7271,
               Formatter::Format(_T("Message {0} could not be moved to the sandbox hold."), messageId),
               _T("the hold could not be recorded"), true);
         }

         ForgetAttempts(messageId);
         submit_soon = true;

         LOG_APPLICATION(Formatter::Format(_T("Sandbox - Message {0}: held for the sandbox's verdict on {1} file(s); it waits at most {2} minute(s)."),
            messageId, CountedFiles(files), Configuration::Instance()->GetSandboxTimeoutMinutes()));

         result.outcome = Sandbox::OutcomeHeld;
         return result;
      }

      // ---- the poller's work on one message ---------------------------------

      std::shared_ptr<Message> TraceMessage(std::shared_ptr<Message> &message, __int64 messageId)
      {
         if (!message)
         {
            message = std::shared_ptr<Message>(new Message());
            if (!PersistentMessage::ReadObject(message, messageId))
               message.reset();
         }

         return message;
      }

      // Submits what has not been submitted and, unless submitOnly, asks for
      // what has, then decides the hold when the answers allow. No request is
      // started past the deadline.
      void PollMessage(__int64 messageId, std::vector<Sandbox::File> &files, __int64 deadline, bool submitOnly)
      {
         // Read again: the deliverer, an administrator or another server may
         // have acted since the pass read the holds.
         bool answered = false;
         if (MessageType(messageId, answered) != Message::HeldForSandbox)
            return;

         std::shared_ptr<Message> message;

         std::set<AnsiString> wanted;
         for (const Sandbox::File &file : files)
         {
            if (file.state == Sandbox::FileNotSubmitted)
               wanted.insert(file.sha256);
         }

         std::map<AnsiString, AnsiString> contents;
         const bool loaded = wanted.empty() ? true : LoadContents(messageId, wanted, contents);

         for (Sandbox::File &file : files)
         {
            boost::this_thread::interruption_point();

            if (file.state == Sandbox::FileNotSubmitted)
            {
               if (NowSeconds() >= deadline)
                  return;

               String submission;
               String problem;

               std::map<AnsiString, AnsiString>::const_iterator content = contents.find(file.sha256);
               if (!loaded)
                  problem = _T("the queued message could not be read");
               else if (content == contents.end())
                  problem = _T("the attachment was not found in the queued message");
               else if (SubmitFile(messageId, file.filename, content->second, file.sha256, RequestTimeout(SubmitTimeoutSeconds, deadline), submission, problem))
               {
                  file.state = Sandbox::FileSubmitted;
                  file.submission = submission;
                  UpdateFile(file);

                  LOG_APPLICATION(Formatter::Format(_T("Sandbox - Message {0}: {1} ({2} bytes, sha256 {3}) submitted to the sandbox, which answered id \"{4}\"."),
                     messageId, OneLine(file.filename), file.size, String(file.sha256), submission));

                  if (TraceMessage(message, messageId))
                     RecordTrace(message, MessageTrace::EventSandboxSubmitted, Formatter::Format(_T("{0} sha256 {1} id {2}"), file.filename, String(file.sha256), submission));
                  continue;
               }

               file.errors++;
               UpdateFile(file);

               // One line when a file's submits begin to fail, and the rest at
               // the debug level: a sandbox that is down is a condition.
               if (file.errors == 1)
               {
                  LOG_APPLICATION(Formatter::Format(_T("Sandbox - Message {0}: {1} (sha256 {2}) could not be submitted to the sandbox: {3}. It is tried again at every poll until the hold times out."),
                     messageId, OneLine(file.filename), String(file.sha256), problem));
               }
               else
               {
                  LOG_DEBUG(Formatter::Format(_T("Sandbox - Message {0}: {1} could not be submitted to the sandbox ({2} failed attempt(s)): {3}"), messageId, OneLine(file.filename), file.errors, problem));
               }
               continue;
            }

            if (submitOnly || file.state != Sandbox::FileSubmitted)
               continue;

            if (NowSeconds() >= deadline)
               return;

            Answer answer;
            String problem;
            if (!AskVerdict(file, RequestTimeout(VerdictTimeoutSeconds, deadline), answer, problem))
            {
               file.errors++;
               UpdateFile(file);

               if (file.errors == 1)
               {
                  LOG_APPLICATION(Formatter::Format(_T("Sandbox - Message {0}: the verdict on {1} could not be asked for: {2}. It is asked again at every poll until the hold times out."), messageId, OneLine(file.filename), problem));
               }
               else
               {
                  LOG_DEBUG(Formatter::Format(_T("Sandbox - Message {0}: the verdict on {1} could not be asked for ({2} failed attempt(s)): {3}"), messageId, OneLine(file.filename), file.errors, problem));
               }
               continue;
            }

            if (!answer.done)
               continue;

            file.state = Sandbox::FileAnswered;
            file.verdict = answer.verdict;
            file.score = answer.score;
            file.detail = answer.detail;
            UpdateFile(file);

            const String effective = EffectiveVerdict(answer.verdict, answer.score);
            if (effective == _T("clean") || effective == _T("malicious"))
               RememberVerdict(file.sha256, effective, answer.score, answer.detail);

            LOG_APPLICATION(Formatter::Format(_T("Sandbox - Message {0}: the sandbox's verdict on {1} (sha256 {2}) is {3}, score {4}."),
               messageId, OneLine(file.filename), String(file.sha256), answer.verdict, answer.score));

            if (TraceMessage(message, messageId))
               RecordTrace(message, MessageTrace::EventSandboxVerdict, Formatter::Format(_T("{0}: {1} (score {2}) {3}"), file.filename, answer.verdict, answer.score, answer.detail));
         }

         if (submitOnly)
            return;

         // The decision, once the files allow one. A malicious file ends the
         // hold at once. Otherwise nothing is decided until EVERY file has an
         // answer: an error on one file is not a reason to stop waiting for the
         // others, or a payload beside a file the sandbox fails on quickly
         // would be delivered within one poll, never having had its own
         // verdict. With every answer in, an error on any file decides the hold
         // by SandboxTimeoutAction, and all clean delivers it.
         bool anyMalicious = false;
         bool anyError = false;
         bool allAnswered = true;
         for (const Sandbox::File &file : files)
         {
            if (file.state == FileNotSubmittable)
               continue;

            if (file.state != Sandbox::FileAnswered)
            {
               allAnswered = false;
               continue;
            }

            if (IsMalicious(file))
               anyMalicious = true;
            else if (file.verdict == _T("error"))
               anyError = true;
         }

         int decision = Sandbox::DecisionPending;
         if (anyMalicious)
            decision = Sandbox::DecisionMalicious;
         else if (allAnswered)
            decision = anyError ? Sandbox::DecisionError : Sandbox::DecisionClean;

         if (decision == Sandbox::DecisionPending)
            return;

         if (!Decide(messageId, decision))
         {
            ReportSandboxError(7276, _T("Sandbox::PollOnce"),
               Formatter::Format(_T("The sandbox's decision on message {0} ({1}) could not be recorded; it stays held and the next poll tries again."), messageId, String(Sandbox::DecisionWord(decision))));
            return;
         }

         // Whichever decision stands - this one, or an administrator's release
         // that came first - the message goes back to the queue with it.
         const int standing = DecisionOf(messageId);
         if (standing == Sandbox::DecisionPending)
            return;

         if (!ReleaseMessage(messageId))
            return;

         if (standing == decision)
         {
            LOG_APPLICATION(Formatter::Format(_T("Sandbox - Message {0}: decided {1}; it goes back to the delivery queue."), messageId, String(Sandbox::DecisionWord(decision))));
         }
      }

      // One pass. A full pass decides, first and without a request, every hold
      // that is decided, turned off or timed out, then spends at most
      // PassBudgetSeconds on requests, starting after the message the last one
      // stopped at. A submit-only pass - run at once when a message is held -
      // submits the files not yet submitted and asks nothing.
      void Pass(bool submitOnly)
      {
         if (!Db())
            return;

         ReportInactive();

         const bool active = Active();
         const __int64 now = NowSeconds();

         if (submitOnly && !active)
            return;

         // Every row, by message.
         std::map<__int64, std::vector<Sandbox::File> > holds;
         {
            SQLCommand command("select * from hm_sandboxholds order by shmessageid, shid");
            std::shared_ptr<DALRecordset> recordset = QueryRows(command);
            if (!recordset)
               return;

            while (!recordset->IsEOF())
            {
               Sandbox::File file = ReadFileRow(recordset);
               holds[file.message_id].push_back(file);
               recordset->MoveNext();
            }
         }

         // A held message with no rows at all - the rows removed by hand - goes
         // back to the queue, where the deliverer examines it again.
         if (!submitOnly)
         {
            SQLCommand command("select messageid from hm_messages where messagetype = 5");
            std::shared_ptr<DALRecordset> recordset = QueryRows(command);
            std::vector<__int64> orphans;
            while (recordset && !recordset->IsEOF())
            {
               const __int64 id = recordset->GetInt64Value("messageid");
               if (holds.find(id) == holds.end())
                  orphans.push_back(id);
               recordset->MoveNext();
            }

            for (__int64 id : orphans)
            {
               if (ReleaseMessage(id))
               {
                  LOG_APPLICATION(Formatter::Format(_T("Sandbox - Message {0} was held with no record of its submissions; it goes back to the delivery queue and is examined again."), id));
               }
            }
         }

         const int timeoutMinutes = Configuration::Instance()->GetSandboxTimeoutMinutes();

         // Phase one, with no request: what can be decided without the sandbox.
         std::vector<__int64> work;

         for (auto &entry : holds)
         {
            boost::this_thread::interruption_point();

            const __int64 messageId = entry.first;
            std::vector<Sandbox::File> &files = entry.second;

            bool answered = false;
            const int type = MessageType(messageId, answered);
            if (!answered)
               continue;

            if (type == -1)
            {
               // The message is gone - delivered, discarded or deleted - and its
               // rows with it.
               if (!submitOnly)
                  DeleteFiles(messageId);
               continue;
            }

            // In the deliverer's hands, or held for the compromised-account
            // review, which takes precedence and which no decision here reaches.
            if (type != Message::HeldForSandbox)
               continue;

            const bool timedOut = files[0].held > 0 && now - files[0].held >= (__int64) timeoutMinutes * 60;

            if (submitOnly)
            {
               bool unsent = false;
               for (const Sandbox::File &file : files)
               {
                  if (file.state == Sandbox::FileNotSubmitted)
                     unsent = true;
               }

               if (files[0].decision == Sandbox::DecisionPending && !timedOut && unsent)
                  work.push_back(messageId);
               continue;
            }

            if (files[0].decision != Sandbox::DecisionPending)
            {
               // Decided, and the release did not happen.
               ReleaseMessage(messageId);
               continue;
            }

            if (!active)
            {
               if (Decide(messageId, Sandbox::DecisionTurnedOff) && DecisionOf(messageId) == Sandbox::DecisionTurnedOff && ReleaseMessage(messageId))
               {
                  LOG_APPLICATION(Formatter::Format(_T("Sandbox - Message {0}: the sandbox was turned off, or an address emptied, while it was held; it goes back to the delivery queue without a verdict."), messageId));
               }
               continue;
            }

            if (timedOut)
            {
               if (Decide(messageId, Sandbox::DecisionTimedOut) && DecisionOf(messageId) == Sandbox::DecisionTimedOut && ReleaseMessage(messageId))
               {
                  LOG_APPLICATION(Formatter::Format(_T("Sandbox - Message {0}: no verdict on every file within {1} minute(s); the hold has timed out and it goes back to the delivery queue."), messageId, timeoutMinutes));
               }
               continue;
            }

            work.push_back(messageId);
         }

         // Phase two: the requests, within the budget, the messages the last
         // full pass did not reach first.
         const __int64 deadline = NowSeconds() + PassBudgetSeconds;
         const __int64 after = submitOnly ? 0 : resume_after;
         std::stable_partition(work.begin(), work.end(), [after](__int64 id) { return id > after; });

         size_t reached = 0;
         for (__int64 messageId : work)
         {
            boost::this_thread::interruption_point();

            if (NowSeconds() >= deadline)
               break;

            PollMessage(messageId, holds[messageId], deadline, submitOnly);
            reached++;

            if (!submitOnly)
               resume_after = messageId;
         }

         if (!submitOnly && reached == work.size())
            resume_after = 0;

         if (reached < work.size())
         {
            LOG_DEBUG(Formatter::Format(_T("Sandbox - the pass reached {0} of {1} held message(s) within its {2} seconds; the rest come first at the next."), (int) reached, (int) work.size(), PassBudgetSeconds));
         }

         if (!submitOnly)
            SweepVerdicts();
      }
   }

   Sandbox::Result
   Sandbox::Examine(std::shared_ptr<Message> message)
   {
      Result result;

      try
      {
         if (!message || message->GetID() <= 0)
            return result;

         // A decision the poller or an administrator recorded is acted on
         // whether or not the sandbox is still switched on: it is the end of a
         // hold that began while it was.
         std::vector<File> files;
         if (!ReadFiles(message->GetID(), files))
         {
            if (!Active())
               return result;

            return TryAgainLater(message, 7270,
               Formatter::Format(_T("Whether message {0} is held for the sandbox could not be read."), message->GetID()),
               _T("whether it is held could not be read"), false);
         }

         if (!files.empty())
            return Decided(message, files);

         // Examined once, on the first pass, as the DKIM signature and the
         // mirror copy are: a message a remote deferred comes round again with
         // a try counted (or, deferred by this server's own shaping, with
         // FlagFirstPassDone), and was delivered to its local recipients - or
         // decided by a sandbox - on the pass before. Its rows went with that
         // decision, so without this it would be submitted and held again.
         const bool firstPass = message->GetNoOfRetries() == 0 && !message->GetFirstPassDone();

         // A person released it from the quarantine the sandbox's own verdict
         // put it in: the remembered malicious verdict would put it straight
         // back, and the X-hMailServer-Sandbox it carries is this server's.
         // Any other release - the spam quarantine's above all, which holds a
         // message at SMTP time, before the deliverer ever sees it - is not
         // exempt, and is examined here like new mail.
         const bool releasedFromVerdict = message->GetReleasedFromSandboxQuarantine();

         // On the first pass, before anything else, the field a sender wrote
         // under this server's name goes, whether or not the message is
         // examined: a reader should never meet an X-hMailServer-Sandbox this
         // server did not write.
         if (firstPass && !releasedFromVerdict)
            RemoveSendersHeader(message);

         if (!Active() || !firstPass || releasedFromVerdict)
            return result;

         // What this server's own signed-in users submit - an authenticated
         // SMTP session, the webmail and what is sent through its code - is
         // theirs to send, and is not uploaded to a third party unless the
         // administrator asks for it.
         if (!Configuration::Instance()->GetSandboxInternalMail() && message->GetSubmittedBySignedInUser())
            return result;

         return Examined(message);
      }
      catch (...)
      {
         ReportSandboxError(7275, _T("Sandbox::Examine"),
            _T("An unexpected error ended the sandbox's examination of a message. The message is delivered without a verdict, as a message the virus scanner could not examine is by default."));
         return Result();
      }
   }

   bool
   Sandbox::Rehold(std::shared_ptr<Message> message)
   {
      if (!message)
         return false;

      if (HoldMessage(message->GetID()))
         return true;

      Defer(message);
      return false;
   }

   void
   Sandbox::Forget(__int64 messageId)
   {
      DeleteFiles(messageId);
      ForgetAttempts(messageId);
   }

   bool
   Sandbox::ReadHeld(__int64 messageId, Held &held)
   {
      held = Held();

      SQLCommand command("select messageid, messagefrom, messagecreatetime, messagelocked, messagecurnooftries from hm_messages where messageid = @MESSAGEID and messagetype = 5");
      command.AddParameter("@MESSAGEID", messageId);

      std::shared_ptr<DALRecordset> recordset = QueryRows(command);
      if (!recordset || recordset->IsEOF())
         return false;

      held.message_id = recordset->GetInt64Value("messageid");
      held.from = recordset->GetStringValue("messagefrom");
      held.created = recordset->GetStringValue("messagecreatetime");
      held.locked = recordset->GetLongValue("messagelocked") == 1;
      held.tries = (int) recordset->GetLongValue("messagecurnooftries");

      SQLCommand recipients("select recipientaddress from hm_messagerecipients where recipientmessageid = @MESSAGEID");
      recipients.AddParameter("@MESSAGEID", messageId);

      std::shared_ptr<DALRecordset> addresses = QueryRows(recipients);
      while (addresses && !addresses->IsEOF())
      {
         held.recipients += (held.recipients.IsEmpty() ? _T("") : _T(",")) + addresses->GetStringValue("recipientaddress");
         addresses->MoveNext();
      }

      ReadFiles(messageId, held.files);

      if (!held.files.empty())
      {
         held.held = held.files[0].held;
         held.decision = held.files[0].decision;
      }

      return true;
   }

   bool
   Sandbox::ListHeld(int offset, int count, std::vector<Held> &held, int &total)
   {
      held.clear();
      total = 0;

      if (offset < 0)
         offset = 0;

      // The page's messages, and how many there are in all.
      SQLCommand command("select messageid, messagefrom, messagecreatetime, messagelocked, messagecurnooftries from hm_messages where messagetype = 5 order by messageid");

      std::shared_ptr<DALRecordset> recordset = QueryRows(command);
      if (!recordset)
         return false;

      while (!recordset->IsEOF())
      {
         if (total >= offset && (int) held.size() < count)
         {
            Held entry;
            entry.message_id = recordset->GetInt64Value("messageid");
            entry.from = recordset->GetStringValue("messagefrom");
            entry.created = recordset->GetStringValue("messagecreatetime");
            entry.locked = recordset->GetLongValue("messagelocked") == 1;
            entry.tries = (int) recordset->GetLongValue("messagecurnooftries");
            held.push_back(entry);
         }

         total++;
         recordset->MoveNext();
      }

      if (held.empty())
         return true;

      std::map<__int64, size_t> index;
      for (size_t i = 0; i < held.size(); i++)
         index[held[i].message_id] = i;

      const __int64 first = held.front().message_id;
      const __int64 last = held.back().message_id;

      // The page's recipients, in one query.
      SQLCommand recipients("select r.recipientmessageid, r.recipientaddress from hm_messagerecipients r inner join hm_messages m on m.messageid = r.recipientmessageid where m.messagetype = 5 and r.recipientmessageid >= @FIRST and r.recipientmessageid <= @LAST order by r.recipientid");
      recipients.AddParameter("@FIRST", first);
      recipients.AddParameter("@LAST", last);

      std::shared_ptr<DALRecordset> addresses = QueryRows(recipients);
      while (addresses && !addresses->IsEOF())
      {
         std::map<__int64, size_t>::const_iterator found = index.find(addresses->GetInt64Value("recipientmessageid"));
         if (found != index.end())
         {
            Held &entry = held[found->second];
            entry.recipients += (entry.recipients.IsEmpty() ? _T("") : _T(",")) + addresses->GetStringValue("recipientaddress");
         }
         addresses->MoveNext();
      }

      // And their files, in one more.
      SQLCommand rows("select * from hm_sandboxholds where shmessageid >= @FIRST and shmessageid <= @LAST order by shmessageid, shid");
      rows.AddParameter("@FIRST", first);
      rows.AddParameter("@LAST", last);

      std::shared_ptr<DALRecordset> fileRows = QueryRows(rows);
      while (fileRows && !fileRows->IsEOF())
      {
         const File file = ReadFileRow(fileRows);
         std::map<__int64, size_t>::const_iterator found = index.find(file.message_id);
         if (found != index.end())
            held[found->second].files.push_back(file);
         fileRows->MoveNext();
      }

      for (Held &entry : held)
      {
         if (!entry.files.empty())
         {
            entry.held = entry.files[0].held;
            entry.decision = entry.files[0].decision;
         }
      }

      return true;
   }

   String
   Sandbox::Describe(const Held &held)
   {
      String names;
      for (const File &file : held.files)
      {
         if (file.filename.IsEmpty())
            continue;
         names += (names.IsEmpty() ? _T("") : _T(", ")) + file.filename;
      }

      return _T("from ") + held.from + _T(" to ") + held.recipients + _T(", held for the sandbox's verdict on: ") +
             (names.IsEmpty() ? String(_T("(no file recorded)")) : names);
   }

   bool
   Sandbox::Release(__int64 messageId, String &problem)
   {
      bool answered = false;
      if (MessageType(messageId, answered) != Message::HeldForSandbox)
      {
         problem = answered ? _T("not held") : _T("the delivery queue could not be read");
         return false;
      }

      std::vector<File> files;
      if (!ReadFiles(messageId, files))
      {
         problem = _T("the hold could not be read");
         return false;
      }

      if (files.empty())
      {
         // A held message with no record of its hold (rows removed by hand) is
         // given one, so that the deliverer sees the decision and does not
         // submit it again.
         File marker;
         marker.message_id = messageId;
         marker.held = NowSeconds();
         marker.state = FileNotSubmittable;
         marker.decision = DecisionReleased;
         if (!InsertFile(marker))
         {
            problem = _T("the release could not be recorded");
            return false;
         }
      }
      else if (!Decide(messageId, DecisionReleased))
      {
         problem = _T("the release could not be recorded");
         return false;
      }

      // The decision that stands says what this release did. Decide matches
      // only an undecided hold, so a verdict, the timeout or the switch that
      // got there first keeps its decision, and the message goes on as that
      // decision says - quarantined, returned or delivered with its header -
      // and not as "released by administrator".
      const int standing = DecisionOf(messageId);
      if (standing != DecisionReleased)
      {
         if (standing != DecisionPending && standing != -1)
            ReleaseMessage(messageId);

         problem = _T("decided");
         return false;
      }

      if (!ReleaseMessage(messageId))
      {
         problem = _T("the message could not be put back in the delivery queue");
         return false;
      }

      return true;
   }

   bool
   Sandbox::Discard(__int64 messageId, String &problem)
   {
      bool answered = false;
      if (MessageType(messageId, answered) != Message::HeldForSandbox)
      {
         problem = answered ? _T("not held") : _T("the delivery queue could not be read");
         return false;
      }

      // The queue's own removal: read, lock, delete the row and the file.
      DeliveryQueue::Remove(messageId);

      if (MessageType(messageId, answered) != -1 || !answered)
      {
         problem = _T("the message could not be removed; it is still held");
         return false;
      }

      DeleteFiles(messageId);
      ForgetAttempts(messageId);
      return true;
   }

   // ---- the poller -----------------------------------------------------------

   void
   Sandbox::PollOnce()
   {
      Pass(false);
   }

   void
   Sandbox::WorkerLoop_()
   {
      int waited = 0;

      while (true)
      {
         boost::this_thread::sleep_for(boost::chrono::seconds(1));
         waited++;

         int period = 30;
         try
         {
            period = Configuration::Instance()->GetSandboxPollSeconds();
         }
         catch (...)
         {
         }

         // A message just held has its files submitted now, not at the next
         // poll; the verdicts are asked for at the poll's own pace.
         const bool full = waited >= period;
         const bool soon = submit_soon.exchange(false);
         if (!full && !soon)
            continue;

         if (full)
            waited = 0;

         try
         {
            Pass(!full);
         }
         catch (const boost::thread_interrupted &)
         {
            throw;
         }
         catch (...)
         {
            // Contained, so the thread goes on polling; said at most once an
            // hour, because a failure that repeats every poll is one condition.
            const __int64 now = NowSeconds();
            if (last_poll_failure_report == 0 || now - last_poll_failure_report >= FailureReportIntervalSeconds || now < last_poll_failure_report)
            {
               last_poll_failure_report = now;
               ReportSandboxError(7274, _T("Sandbox::WorkerLoop_"),
                  _T("A pass of the sandbox's poller ended in an unexpected error. Held messages wait for the next pass; the hold's timeout still applies."));
            }
         }
      }
   }

   void
   Sandbox::WorkerFunc_()
   {
      boost::function<void()> func = &Sandbox::WorkerLoop_;
      ExceptionHandler::Run("Sandbox poller", func);
      LOG_DEBUG("Sandbox poller stopped.");
   }

   void
   Sandbox::Start()
   {
      boost::lock_guard<boost::mutex> guard(poller_mutex);

      inactive_reported = false;

      if (poller_thread)
      {
         // Never two pollers in one process: they would submit the same file
         // twice. A thread still running - a Start with no Stop before it -
         // goes on as it is.
         if (poller_thread->joinable() && !poller_thread->timed_join(boost::posix_time::milliseconds(1)))
            return;

         delete poller_thread;
         poller_thread = nullptr;
      }

      poller_thread = new boost::thread(&Sandbox::WorkerFunc_);
   }

   void
   Sandbox::Stop(const std::function<void()> &progress)
   {
      boost::lock_guard<boost::mutex> guard(poller_mutex);

      if (!poller_thread || !poller_thread->joinable())
         return;

      poller_thread->interrupt();

      // Joined, however long the request in flight takes to end - at most its
      // own timeout, a minute for a submit - so that nothing it does afterwards
      // reaches a database manager a Reinitialize has reset, or a delivery
      // manager that has stopped, and so that a Start after this one never
      // finds it still running.
      while (!poller_thread->timed_join(boost::posix_time::seconds(1)))
      {
         if (progress)
            progress();
      }
   }

   // ---- the checks every surface shares ----------------------------------------

   String
   Sandbox::UrlProblem(const String &url, const String &settingName)
   {
      if (url.IsEmpty())
         return String();

      if (url.GetLength() > 2048)
         return settingName + _T(" is longer than 2048 characters.");

      for (int i = 0; i < url.GetLength(); i++)
      {
         const wchar_t c = url.GetAt(i);
         if (c <= 0x20 || c >= 0x7F)
            return settingName + _T(" contains a space, a control character or a character outside ASCII; percent-encode anything that is not ASCII.");
      }

      // ASCII from here, so the narrowing is exact.
      const AnsiString text = AnsiString(url);

      if (text.Find("#") >= 0)
         return settingName + _T(" must not contain a fragment (#).");

      const bool https = text.Left(8) == "https://";
      const bool http = text.Left(7) == "http://";
      if (!https && !http)
         return settingName + _T(" must be an https URL, written in lower case - or plain http to this machine (127.0.0.1, ::1 or localhost).");

      const int authorityStart = https ? 8 : 7;
      const int pathStart = text.Find("/", static_cast<size_t>(authorityStart));
      const AnsiString authority = pathStart < 0 ? text.Mid(authorityStart) : text.Mid(authorityStart, pathStart - authorityStart);
      if (authority.Find("@") >= 0)
         return settingName + _T(" must not carry a user name or a password.");

      bool parsedHttps = false;
      AnsiString host, port, path;
      if (!HttpsClient::ParseUrl(text, parsedHttps, host, port, path) || host.IsEmpty())
         return settingName + _T(" is not an http or https URL with a host.");

      const long portNumber = atol(port.c_str());
      if (port.GetLength() > 5 || portNumber < 1 || portNumber > 65535)
         return settingName + _T("'s port must be between 1 and 65535.");

      if (!https && !HttpsClient::IsLoopbackHost(host))
         return settingName + _T(" must be an https URL: plain http is accepted only to this machine (127.0.0.1, ::1 or localhost), since an attachment and its verdict sent in the clear across a network could be read and changed on the way.");

      return String();
   }

   String
   Sandbox::TypesProblem(const String &types, const String &settingName, String &canonical)
   {
      canonical = String();

      String list = types;
      list.Replace(_T(";"), _T(","));
      list.Replace(_T(" "), _T(","));
      list.Replace(_T("\t"), _T(","));
      list.Replace(_T("\r"), _T(","));
      list.Replace(_T("\n"), _T(","));

      std::vector<String> accepted;

      for (String entry : StringParser::SplitString(list, _T(",")))
      {
         entry.Trim();
         entry.ToLower();
         if (entry.IsEmpty())
            continue;

         if (entry.Left(2) == _T("*."))
            entry = entry.Mid(2);
         else if (entry.Left(1) == _T("."))
            entry = entry.Mid(1);

         const String refusal = settingName + _T(": \"") + entry +
            _T("\" is neither an extension (letters and digits, such as exe) nor a media type (such as application/pdf).");

         const int slash = entry.Find(_T("/"));
         if (slash >= 0)
         {
            const String top = entry.Mid(0, slash);
            const String sub = entry.Mid(slash + 1);

            bool known = false;
            for (const wchar_t *name : TopLevelTypes)
            {
               if (top == name)
                  known = true;
            }

            if (!known || sub.IsEmpty() || sub.GetLength() > 100)
               return refusal;

            for (int i = 0; i < sub.GetLength(); i++)
            {
               if (!IsMediaTypeCharacter(sub.GetAt(i)))
                  return refusal;
            }
         }
         else
         {
            if (entry.IsEmpty() || entry.GetLength() > 16)
               return refusal;

            for (int i = 0; i < entry.GetLength(); i++)
            {
               if (!IsExtensionCharacter(entry.GetAt(i)))
                  return refusal;
            }
         }

         bool duplicate = false;
         for (const String &existing : accepted)
         {
            if (existing == entry)
               duplicate = true;
         }

         if (!duplicate)
            accepted.push_back(entry);
      }

      String joined;
      for (const String &entry : accepted)
         joined += (joined.IsEmpty() ? _T("") : _T(",")) + entry;

      if (joined.GetLength() > 1000)
         return settingName + _T(" is longer than 1000 characters once written out.");

      canonical = joined;
      return String();
   }

   String
   Sandbox::SwitchProblem(bool enabled, const String &submitUrl, const String &verdictUrl, const String &settingName)
   {
      if (!enabled)
         return String();

      String submit = submitUrl;
      submit.Trim();
      String verdict = verdictUrl;
      verdict.Trim();

      if (!submit.IsEmpty() && !verdict.IsEmpty())
         return String();

      const String which = submit.IsEmpty() && verdict.IsEmpty() ? String(_T("both addresses are"))
                         : submit.IsEmpty() ? String(_T("the submit address is"))
                         : String(_T("the verdict address is"));

      return settingName + _T(" cannot be on while ") + which +
             _T(" empty: a message would be held with nowhere to send its files or to ask for their verdict. Set both addresses first.");
   }

   String
   Sandbox::TokenProblem(const String &token, const String &settingName)
   {
      if (token.IsEmpty())
         return String();

      if (token.GetLength() > 2048)
         return settingName + _T(" is longer than 2048 characters.");

      // The token goes into an HTTP header as it is: a line break would end the
      // header and begin another, and a space or a character outside ASCII is
      // not a bearer token (RFC 6750 2.1).
      for (int i = 0; i < token.GetLength(); i++)
      {
         const wchar_t c = token.GetAt(i);
         if (c <= 0x20 || c >= 0x7F)
            return settingName + _T(" may hold only visible ASCII characters - no space, tab or line break, and nothing outside ASCII - since it is sent in an Authorization header.");
      }

      // What the store would hold. The legacy scheme, used while
      // ProtectStoredSecretsWithDPAPI is 0, holds 248 characters and refuses
      // more; the platform's store holds more, and its envelope has to fit in
      // hm_settings.settingstring's 4000.
      if (!IniFileSettings::Instance()->GetProtectStoredSecretsWithDPAPI() && token.GetLength() > LegacySecretMaxLength)
      {
         return Formatter::Format(_T("{0} is longer than {1} characters, the most this server stores while ProtectStoredSecretsWithDPAPI is 0."),
            settingName, LegacySecretMaxLength);
      }

      const String stored = Crypt::Instance()->ProtectSecret(token);
      if (stored.IsEmpty())
         return settingName + _T(" could not be stored protected; the error log says why.");

      if (stored.GetLength() > 4000)
      {
         return Formatter::Format(_T("{0} is too long to store: protected, it takes {1} characters, and the store holds 4000."),
            settingName, stored.GetLength());
      }

      return String();
   }

   bool
   Sandbox::IsSandboxQuarantineReason(const String &reason)
   {
      const String prefix = QuarantineReasonPrefix;
      return reason.GetLength() > prefix.GetLength() && reason.Left(prefix.GetLength()) == prefix;
   }

   const char *
   Sandbox::DecisionWord(int decision)
   {
      switch (decision)
      {
      case DecisionClean: return "clean";
      case DecisionMalicious: return "malicious";
      case DecisionError: return "error";
      case DecisionTimedOut: return "timed_out";
      case DecisionReleased: return "released";
      case DecisionTurnedOff: return "turned_off";
      default: return "pending";
      }
   }

   const char *
   Sandbox::FileStateWord(int state)
   {
      switch (state)
      {
      case FileNotSubmitted: return "not_submitted";
      case FileSubmitted: return "submitted";
      case FileAnswered: return "answered";
      default: return "not_submittable";
      }
   }
}
