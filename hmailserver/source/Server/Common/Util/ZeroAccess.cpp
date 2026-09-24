// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "ZeroAccess.h"
#include "OpenPgpKey.h"
#include "OpenPgpRing.h"
#include "OpenPgpWriter.h"
#include "File.h"
#include "FileUtilities.h"
#include "ByteBuffer.h"
#include "Unicode.h"
#include "Encoding/Base64.h"
#include "../Application/Application.h"
#include "../Application/Configuration.h"
#include "../Application/EffectiveSetting.h"
#include "../Application/ErrorManager.h"
#include "../Application/Logger.h"
#include "../BO/Account.h"
#include "../BO/Message.h"
#include "../Persistence/PersistentAccount.h"
#include "../Persistence/PersistentMessage.h"
#include "../Persistence/MessageStore.h"
#include "../SQL/SQLCommand.h"
#include "../SQL/SQLStatement.h"
#include "../SQL/DALRecordset.h"

#include <ctime>
#include <fstream>
#include <string>
#include <vector>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      const char *Columns = "zaid, zaaccountid, zafingerprint, zasalt, zapasses, zamemory, zalanes, zasecret, zarecovery, zaenabledat";

      __int64 Now()
      {
         return (__int64) time(nullptr);
      }

      AnsiString Utf8(const String &value)
      {
         AnsiString out;
         Unicode::WideToMultiByte(value, out);
         return out;
      }

      AnsiString Escape(const AnsiString &value)
      {
         AnsiString out;
         for (int i = 0; i < value.GetLength(); i++)
         {
            unsigned char c = (unsigned char) value[i];
            if (c == '"') out += "\\\"";
            else if (c == '\\') out += "\\\\";
            else if (c == '\n') out += "\\n";
            else if (c == '\r') out += "\\r";
            else if (c == '\t') out += "\\t";
            else if (c < 0x20) { char buffer[8]; sprintf_s(buffer, sizeof(buffer), "\\u%04x", c); out += buffer; }
            else out += (char) c;
         }
         return out;
      }

      void ReadRow(std::shared_ptr<DALRecordset> recordset, ZeroAccess::Row &row)
      {
         row.id = recordset->GetInt64Value("zaid");
         row.account_id = recordset->GetInt64Value("zaaccountid");
         row.fingerprint = AnsiString(recordset->GetStringValue("zafingerprint"));
         row.salt = AnsiString(recordset->GetStringValue("zasalt"));
         row.passes = (int) recordset->GetLongValue("zapasses");
         row.memory_kib = (int) recordset->GetLongValue("zamemory");
         row.lanes = (int) recordset->GetLongValue("zalanes");
         row.password_wrapped = AnsiString(recordset->GetStringValue("zasecret"));
         row.recovery_wrapped = AnsiString(recordset->GetStringValue("zarecovery"));
         row.enabled_at = recordset->GetInt64Value("zaenabledat");
      }

      bool IsBase64Text(const AnsiString &text)
      {
         if (text.IsEmpty() || text.GetLength() % 4 != 0)
            return false;
         for (int i = 0; i < text.GetLength(); i++)
         {
            const char c = text[i];
            const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=';
            if (!ok)
               return false;
         }
         return true;
      }

      // One wrapped copy: an armoured secret key, every secret packet in it
      // encrypted, and the same key as the ring row named.
      bool CheckWrapped(const AnsiString &wrapped, const AnsiString &fingerprint, const char *what, AnsiString &problem)
      {
         if (wrapped.IsEmpty())
         {
            problem.Format("%hs: the armoured OpenPGP secret key, sealed in the browser, is required", what);
            return false;
         }
         if (wrapped.GetLength() > OpenPgpRing::SecretMaximum)
         {
            problem.Format("%hs: at most 512 KB of armour", what);
            return false;
         }
         const OpenPgpKey::SecretKeyFacts facts = OpenPgpKey::ReadSecretKey(wrapped, (size_t) OpenPgpRing::SecretMaximum);
         if (!facts.ok)
         {
            problem.Format("%hs: %hs", what, facts.error.c_str());
            return false;
         }
         if (facts.fingerprint != fingerprint)
         {
            problem.Format("%hs: not the key named by fingerprint", what);
            return false;
         }
         return true;
      }

      // A message that arrived under S/MIME: an enveloped-data body, which
      // is under a key already, as a PGP/MIME message is.
      bool IsSmimeEnveloped(const AnsiString &message)
      {
         const int blank = message.Find("\r\n\r\n");
         AnsiString header = blank < 0 ? message : message.Mid(0, blank);
         header.MakeLower();
         header.Replace("\r\n ", " ");
         header.Replace("\r\n\t", " ");
         int at = header.StartsWith("content-type:") ? 0 : header.Find("\ncontent-type:");
         if (at < 0)
            return false;
         if (at > 0)
            at += 1;
         const int lineEnd = header.Find("\n", at);
         const AnsiString field = lineEnd < 0 ? header.Mid(at) : header.Mid(at, lineEnd - at);
         return (field.Find("application/pkcs7-mime") >= 0 || field.Find("application/x-pkcs7-mime") >= 0) && field.Find("signed-data") < 0;
      }

      bool ReadWhole(const String &fileName, AnsiString &out)
      {
         File file;
         if (!file.Open(fileName, File::OTReadOnly))
            return false;
         std::shared_ptr<ByteBuffer> buffer = file.ReadFile();
         file.Close();
         if (!buffer)
            return false;
         out = AnsiString(std::string((const char *) buffer->GetBuffer(), buffer->GetSize()));
         return true;
      }
   }

   void
   ZeroAccess::Scrub(const String &fileName)
   {
      const __int64 length = FileUtilities::FileSize(fileName);
      if (length <= 0)
         return;

      // In place, without truncating: a file opened to be rewritten keeps
      // its blocks, which is what overwriting them means. Truncating first
      // would free them and put the zeros somewhere else.
#ifdef HM_PLATFORM_POSIX
      const AnsiString narrow = fileName.c_str();
      std::fstream over(narrow.c_str(), std::ios::in | std::ios::out | std::ios::binary);
#else
      std::fstream over(fileName.c_str(), std::ios::in | std::ios::out | std::ios::binary);
#endif
      if (!over)
         return;
      std::vector<char> zeros((size_t) length, '\0');
      over.write(&zeros[0], (std::streamsize) zeros.size());
      over.flush();
      over.close();
   }

   bool
   ZeroAccess::SealStoredCopy(std::shared_ptr<const Account> account, std::shared_ptr<Message> copy, const char *what)
   {
      if (!account || !copy || !account->GetZeroAccess())
         return true;

      const String fileName = MessageStore::WorkingFile(account, copy);
      AnsiString problem;
      if (SealFile(account, fileName, problem))
      {
         copy->SetSize(MessageStore::FileSize(fileName));
         return true;
      }

      Scrub(fileName);
      FileUtilities::DeleteFile(fileName);

      String errorMessage;
      errorMessage.Format(_T("The %s for the zero-access mailbox %s could not be encrypted to its key and was not kept: %s. The file has been removed rather than stored in the clear."),
         String(what).c_str(), String(account->GetAddress()).c_str(), String(problem).c_str());
      ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7071, "ZeroAccess::SealStoredCopy", errorMessage);
      return false;
   }

   const char *
   ZeroAccess::KeptHeaders()
   {
      return "every header field of the message as it arrived - From, To, Cc, Date, Subject, Message-ID, the Received trace and the server's own fields among them - except the Content-* fields and MIME-Version, which described the body that is now under the key";
   }

   bool
   ZeroAccess::Allowed(std::shared_ptr<const Account> account)
   {
      // A setting with levels (Application/EffectiveSetting.h): the account's
      // own where an administrator gave it one, its domain's, the server's.
      return EffectiveSetting::ResolveBool(_T("zero_access_allowed"), std::shared_ptr<const Domain>(), account);
   }

   bool
   ZeroAccess::Get(__int64 accountId, Row &row)
   {
      SQLCommand command(String(_T("select ")) + String(Columns) + _T(" from hm_zeroaccess where zaaccountid = @ACCOUNTID"));
      command.AddParameter("@ACCOUNTID", accountId);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return false;

      ReadRow(recordset, row);
      return true;
   }

   bool
   ZeroAccess::IsMailboxKey(__int64 accountId, const AnsiString &fingerprint)
   {
      Row row;
      if (!Get(accountId, row))
         return false;
      return row.fingerprint == fingerprint;
   }

   bool
   ZeroAccess::Validate(__int64 accountId, const Row &row, bool requireRecovery, AnsiString &problem, int &status)
   {
      status = 400;

      AnsiString lowered = row.fingerprint;
      lowered.MakeLower();
      if (!OpenPgpRing::IsFingerprint(lowered))
      {
         problem = "fingerprint: the fingerprint of one of your own keys in the ring, lower-case hex";
         return false;
      }

      OpenPgpRing::Row ringRow;
      if (!OpenPgpRing::Find(accountId, OpenPgpRing::KindOwn, lowered, ringRow))
      {
         problem = "fingerprint: no key of your own with that fingerprint is in the ring";
         status = 404;
         return false;
      }
      if (ringRow.revoked)
      {
         problem = "that key is revoked; a revoked key encrypts nothing";
         return false;
      }

      // The one thing the server does with the key is encrypt to it, so a key
      // it cannot encrypt to is refused at the switch-on rather than at the
      // first delivery - and a key that EXPIRES is refused there too, because
      // from the day it expired every delivery would be refused and the
      // password copy could not be sealed again, with nothing on the page
      // having said so. The re-wrap does not ask: a key that has gone bad
      // since must never stand between a person and their password change.
      if (requireRecovery)
      {
         const OpenPgpWriter::EncryptionKey usable = OpenPgpWriter::ChooseEncryptionKeyBase64(ringRow.public_key, Now());
         if (!usable.ok)
         {
            problem = "this server cannot encrypt to that key: " + usable.error;
            return false;
         }
         if (usable.expires != 0)
         {
            problem = "that key expires, and a mailbox key must not, because every delivery would be refused from the day it did: choose a key that does not expire - the key page offers Never - or extend this one first";
            return false;
         }
      }

      if (!IsBase64Text(row.salt) || Base64::Decode(row.salt.c_str(), row.salt.GetLength()).GetLength() != SaltBytes)
      {
         problem = "salt: base64 of 16 random octets";
         return false;
      }
      if (row.passes < MinimumPasses || row.passes > MaximumPasses)
      {
         problem.Format("passes: %d to %d", MinimumPasses, MaximumPasses);
         return false;
      }
      if (row.memory_kib < MinimumMemoryKiB || row.memory_kib > MaximumMemoryKiB)
      {
         problem.Format("memory_kib: %d to %d", MinimumMemoryKiB, MaximumMemoryKiB);
         return false;
      }
      if (row.lanes < MinimumLanes || row.lanes > MaximumLanes)
      {
         problem.Format("lanes: %d to %d", MinimumLanes, MaximumLanes);
         return false;
      }

      if (!CheckWrapped(row.password_wrapped, lowered, "password_wrapped", problem))
         return false;
      if (requireRecovery || !row.recovery_wrapped.IsEmpty())
      {
         if (!CheckWrapped(row.recovery_wrapped, lowered, "recovery_wrapped", problem))
            return false;
      }

      status = 200;
      return true;
   }

   bool
   ZeroAccess::SetFlag_(std::shared_ptr<const Account> account, bool on, AnsiString &problem)
   {
      std::shared_ptr<Account> mutableAccount = std::shared_ptr<Account>(new Account());
      if (!PersistentAccount::ReadObject(mutableAccount, account->GetID()) || mutableAccount->GetID() == 0)
      {
         problem = "the account could not be read";
         return false;
      }

      mutableAccount->SetZeroAccess(on);

      String saveError;
      if (!PersistentAccount::SaveObject(mutableAccount, saveError, PersistenceModeNormal))
      {
         problem = saveError.IsEmpty() ? AnsiString("the account could not be saved") : Utf8(saveError);
         return false;
      }

      return true;
   }

   bool
   ZeroAccess::Delete_(__int64 accountId)
   {
      SQLCommand command("delete from hm_zeroaccess where zaaccountid = @ACCOUNTID");
      command.AddParameter("@ACCOUNTID", accountId);
      return Application::Instance()->GetDBManager()->Execute(command);
   }

   bool
   ZeroAccess::Enable(std::shared_ptr<const Account> account, Row &row, AnsiString &problem, int &status)
   {
      status = 500;

      Row existing;
      if (Get(account->GetID(), existing) || account->GetZeroAccess())
      {
         problem = "zero access is already on for this mailbox; switch it off first";
         status = 409;
         return false;
      }

      if (!Validate(account->GetID(), row, true, problem, status))
         return false;

      row.fingerprint.MakeLower();
      row.account_id = account->GetID();
      row.enabled_at = Now();

      SQLStatement statement;
      statement.SetTable("hm_zeroaccess");
      statement.SetStatementType(SQLStatement::STInsert);
      statement.SetIdentityColumn("zaid");
      statement.AddColumnInt64("zaaccountid", row.account_id);
      statement.AddColumn("zafingerprint", String(row.fingerprint));
      statement.AddColumn("zasalt", String(row.salt));
      statement.AddColumn("zapasses", (long) row.passes);
      statement.AddColumn("zamemory", (long) row.memory_kib);
      statement.AddColumn("zalanes", (long) row.lanes);
      statement.AddColumn("zasecret", String(row.password_wrapped));
      statement.AddColumn("zarecovery", String(row.recovery_wrapped));
      statement.AddColumnInt64("zaenabledat", row.enabled_at);

      __int64 id = 0;
      if (!Application::Instance()->GetDBManager()->Execute(statement, &id) || id == 0)
      {
         problem = "the mailbox key could not be stored";
         status = 500;
         return false;
      }
      row.id = id;

      // The flag last: a flag with no row beside it would refuse every
      // delivery, and a row with no flag is a key nothing acts on.
      if (!SetFlag_(account, true, problem))
      {
         Delete_(account->GetID());
         status = 500;
         return false;
      }

      LOG_APPLICATION("Zero access: " + account->GetAddress() + " switched its mailbox to zero access, on key " + String(row.fingerprint) + ". From now on every message delivered to it is encrypted to that key, and this server cannot read it.");

      status = 201;
      return true;
   }

   bool
   ZeroAccess::Rewrap(std::shared_ptr<const Account> account, const Row &fresh, AnsiString &problem, int &status)
   {
      Row existing;
      if (!Get(account->GetID(), existing))
      {
         problem = "zero access is not on for this mailbox";
         status = 404;
         return false;
      }

      // The key stays the key: a re-wrap changes what it is sealed under,
      // never which key the mailbox stands on.
      Row candidate = fresh;
      candidate.fingerprint = existing.fingerprint;
      if (!candidate.recovery_wrapped.IsEmpty() && candidate.recovery_wrapped != existing.recovery_wrapped)
      {
         problem = "recovery_wrapped: the recovery copy is not replaced by a re-wrap; switch zero access off and on to make a new recovery key";
         status = 400;
         return false;
      }
      candidate.recovery_wrapped.Empty();
      if (!Validate(account->GetID(), candidate, false, problem, status))
         return false;

      SQLCommand command("update hm_zeroaccess set zasalt = @SALT, zapasses = @PASSES, zamemory = @MEMORY, zalanes = @LANES, zasecret = @SECRET where zaaccountid = @ACCOUNTID");
      command.AddParameter("@SALT", String(candidate.salt));
      command.AddParameter("@PASSES", candidate.passes);
      command.AddParameter("@MEMORY", candidate.memory_kib);
      command.AddParameter("@LANES", candidate.lanes);
      command.AddParameter("@SECRET", String(candidate.password_wrapped));
      command.AddParameter("@ACCOUNTID", account->GetID());
      if (!Application::Instance()->GetDBManager()->Execute(command))
      {
         problem = "the mailbox key could not be stored";
         status = 500;
         return false;
      }

      status = 200;
      return true;
   }

   bool
   ZeroAccess::Disable(std::shared_ptr<const Account> account, AnsiString &problem)
   {
      Row existing;
      const bool hadRow = Get(account->GetID(), existing);
      if (!hadRow && !account->GetZeroAccess())
      {
         problem = "zero access is not on for this mailbox";
         return false;
      }

      // The flag first, so that a delivery in flight is never refused for a
      // row that is gone; then the row.
      if (!SetFlag_(account, false, problem))
         return false;
      if (hadRow && !Delete_(account->GetID()))
      {
         problem = "the mailbox key row could not be removed";
         return false;
      }

      LOG_APPLICATION("Zero access: " + account->GetAddress() + " switched its mailbox back to ordinary delivery. What was stored encrypted stays encrypted; new mail is stored as it arrives.");
      return true;
   }

   AnsiString
   ZeroAccess::RowJson(const Row &row)
   {
      AnsiString json;
      json.Format("{\"enabled\":true,\"fingerprint\":\"%hs\",\"salt\":\"%hs\",\"argon2\":{\"passes\":%d,\"memory_kib\":%d,\"lanes\":%d},"
                  "\"password_wrapped\":\"%hs\",\"recovery_wrapped\":\"%hs\",\"enabled_at\":%I64d}",
                  row.fingerprint.c_str(), Escape(row.salt).c_str(), row.passes, row.memory_kib, row.lanes,
                  Escape(row.password_wrapped).c_str(), Escape(row.recovery_wrapped).c_str(), row.enabled_at);
      return json;
   }

   bool
   ZeroAccess::SealFile(std::shared_ptr<const Account> account, const String &fileName, AnsiString &problem)
   {
      Row row;
      if (!Get(account->GetID(), row))
      {
         problem = "the mailbox has zero access on and no key row beside it";
         return false;
      }

      OpenPgpRing::Row ringRow;
      if (!OpenPgpRing::Find(account->GetID(), OpenPgpRing::KindOwn, row.fingerprint, ringRow))
      {
         problem = "the mailbox's key " + row.fingerprint + " is no longer in the ring";
         return false;
      }

      const OpenPgpWriter::EncryptionKey key = OpenPgpWriter::ChooseEncryptionKeyBase64(ringRow.public_key, Now());
      if (!key.ok)
      {
         problem = "the mailbox's key cannot be encrypted to: " + key.error;
         return false;
      }

      AnsiString original;
      if (!ReadWhole(fileName, original))
      {
         problem = "the message file could not be read";
         return false;
      }

      // Already sealed - a message the page composed with OpenPGP and sent to
      // itself, a correspondent's PGP/MIME message, or an S/MIME envelope -
      // is stored as it is: it is under a key already, and a second layer
      // would make it a message no client shows, since the outer layer names
      // no key the person's mail client has. What arrives encrypted stays as
      // it arrived.
      if (OpenPgpWriter::IsPgpMimeEncrypted(original) || IsSmimeEnveloped(original))
         return true;

      AnsiString sealed;
      if (!OpenPgpWriter::SealMessage(key, original, sealed, problem))
         return false;

      // The sealed message goes beside the original first, so that a failure
      // at any step leaves the original whole; then the original's bytes are
      // overwritten with zeros - the most a program can do about a plaintext
      // that was on the disk, and the filesystem decides whether it is enough
      // - and the sealed file takes its name.
      const String sealedName = fileName + _T(".sealed");
      {
         File out;
         if (!out.Open(sealedName, File::OTCreate) || !out.Write(sealed) || !out.FlushToDisk())
         {
            out.Close();
            FileUtilities::DeleteFile(sealedName);
            problem = "the sealed message could not be written";
            return false;
         }
         out.Close();
      }

      Scrub(fileName);
      std::fill(original.begin(), original.end(), '\0');

      if (!FileUtilities::DeleteFile(fileName) || !FileUtilities::Move(sealedName, fileName))
      {
         FileUtilities::DeleteFile(sealedName);
         problem = "the sealed message could not take the message's place";
         return false;
      }

      return true;
   }
}
