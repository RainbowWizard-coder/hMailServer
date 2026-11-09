// Experimental native hMailServer persistence adapter; not installed in production.
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "StdAfx.h"
#include "PersistentSentSubmission.h"
#include "../Util/SentDeduplicationPolicy.h"
#include "../Application/Application.h"
#include "../Application/IniFileSettings.h"
#include "../Application/Logger.h"
#include "../Util/Strings/Formatter.h"
#include <atomic>
#include <ctime>
#include "../SQL/DALConnection.h"
#include "../SQL/DALRecordset.h"
#include "../SQL/DatabaseConnectionManager.h"
#include "../SQL/DatabaseTransaction.h"
#include "../SQL/SQLCommand.h"

namespace HM
{
   namespace
   {
      std::atomic<bool> sentCopyRecovered(false);

      DatabaseConnectionManager *Db()
      {
         return Application::Instance()->GetDBManager().get();
      }

      SQLCommand ForSubmission(const String &query, const String &id, __int64 accountId)
      {
         SQLCommand command(query);
         command.AddParameter("@SID", id);
         command.AddParameter("@ACCOUNT", accountId);
         return command;
      }
   }

   bool PersistentSentSubmission::PrepareAtServerStart()
   {
      sentCopyRecovered.store(false, std::memory_order_release);
      auto settings = IniFileSettings::Instance();
      // The stable operational switch is the only runtime activation gate.
      // Fail closed if the ledger is not pre-provisioned and validated.
      if (!settings->GetSentAutoCopyEnabled())
         return true;

      // Schema provisioning is an explicit offline migration, never performed
      // automatically during service startup. An unavailable/incompatible
      // ledger must not cause a Sent copy to be made without bookkeeping.

      // Fail closed if this database has an incomplete or incompatible ledger.
      // A zero-row SELECT validates all fields used by the adapter without
      // reading mailbox content or changing any row.
      SQLCommand schemaProbe(_T("select sssubmissionid,ssaccountid,ssstate,sscreatedutc,"
                                "ssexpiresutc,ssmessageid,ssmimesha256,ssmimebytes,ssconfirmedutc from hm_sentsubmissions where 1=0"));
      auto schema = Db()->OpenRecordset(schemaProbe);
      if (!schema)
         return false;

      // For the SQL CE deployment, a table with all six columns but without
      // the expected UNIQUE primary key or state/expiry index is NOT ready.
      // Do not attempt automatic repair of an existing/partially migrated table.
      if (settings->GetDatabaseType() == DatabaseSettings::TypeMSSQLCompactEdition)
      {
         SQLCommand keyQuery(_T("select count(*) as constraintcount from INFORMATION_SCHEMA.TABLE_CONSTRAINTS "
                                "where TABLE_NAME='hm_sentsubmissions' and "
                                "CONSTRAINT_NAME='pk_hm_sentsubmissions' and CONSTRAINT_TYPE='PRIMARY KEY'"));
         auto keyRows = Db()->OpenRecordset(keyQuery);
         if (!keyRows || keyRows->IsEOF() || keyRows->GetLongValue("constraintcount") != 1)
            return false;
      }

      // Only the cold-start path may quarantine incomplete records. A Pending
      // row can be left behind after an accepted SMTP submission and before
      // ClaimCopy; no original message file is guaranteed to survive delivery.
      // A CopyClaimed row might already have a physical Sent copy. Neither
      // state is blindly replayed or deleted on restart.
      const __int64 quarantined = QuarantineUncertainClaims();
      if (quarantined < 0)
         return false;

      // Expired confirmed copies are no longer useful for correlation.
      // A failed housekeeping pass does not authorize any extra copy.
      PruneResolved(static_cast<__int64>(time(nullptr)));
      sentCopyRecovered.store(true, std::memory_order_release);
      return true;
   }

   bool PersistentSentSubmission::IsRecoveryReady()
   {
      return sentCopyRecovered.load(std::memory_order_acquire);
   }

   bool PersistentSentSubmission::InsertPending(const String &id, __int64 accountId,
                                                __int64 created, __int64 expires)
   {
      if (id.IsEmpty() || id.GetLength() > 64 || accountId <= 0 || created < 0 ||
          expires <= created || expires - created > 3600)
         return false;
      SQLCommand command(_T("insert into hm_sentsubmissions (sssubmissionid,ssaccountid,ssstate,sscreatedutc,ssexpiresutc,ssmessageid) ")
                         _T("values (@SID,@ACCOUNT,0,@CREATED,@EXPIRES,0)"));
      command.AddParameter("@SID", id);
      command.AddParameter("@ACCOUNT", accountId);
      command.AddParameter("@CREATED", created);
      command.AddParameter("@EXPIRES", expires);
      // The pool can replay INSERT after an answer was lost.  On replay a
      // duplicate key is not evidence of a new submission: read the entire
      // identity back and accept only an exact replay of our own record.
      bool reconnected = false;
      const __int64 affected = Db()->ExecuteCount(command, &reconnected, DALConnection::DALErrorInSQL);
      if (affected == 1)
         return true;
      // When a connection was lost after INSERT committed, a replay may
      // report either 0 (forgiven duplicate) or -1 (failed replay). Only
      // an exact read-back of our fresh, still-Pending row is safe to accept.
      // Never reinterpret an error WITHOUT a confirmed reconnect as success.
      if (!reconnected)
         return false;

      SQLCommand read = ForSubmission(
         _T("select ssstate,sscreatedutc,ssexpiresutc,ssmessageid from hm_sentsubmissions ")
         _T("where sssubmissionid=@SID and ssaccountid=@ACCOUNT"), id, accountId);
      auto existing = Db()->OpenRecordset(read);
      return existing && !existing->IsEOF() &&
         existing->GetLongValue("ssstate") == 0 &&
         existing->GetInt64Value("sscreatedutc") == created &&
         existing->GetInt64Value("ssexpiresutc") == expires &&
         existing->GetInt64Value("ssmessageid") == 0;
   }

   __int64 PersistentSentSubmission::ClaimCopy(const String &id, __int64 accountId)
   {
      if (id.IsEmpty() || accountId <= 0)
         return -1;
      SQLCommand command = ForSubmission(
         _T("update hm_sentsubmissions set ssstate=1 where sssubmissionid=@SID and ssaccountid=@ACCOUNT and ssstate=0"), id, accountId);
      bool reconnected = false;
      const __int64 affected = Db()->ExecuteCount(command, &reconnected);
      // If a successful claim lost its response, the replay matches zero.
      // It is NOT safe to turn the resulting state=1 into permission to
      // write another copy: the first attempt may already be writing one.
      return (reconnected && affected == 0) ? -1 : affected;
   }

   __int64 PersistentSentSubmission::CommitCopy(
      const String &id, __int64 accountId, __int64 messageId,
      const String &mimeSha256, __int64 mimeBytes,
      __int64 confirmedUtcSeconds)
   {
      // SQL CE positional binding follows the physical SQL parameter order.
      // Never confirm an incompletely fingerprinted copy.
      if (id.IsEmpty() || accountId <= 0 || messageId <= 0 ||
          mimeSha256.GetLength() != 64 || mimeBytes <= 0 ||
          confirmedUtcSeconds <= 0)
         return -1;

      for (int i = 0; i < mimeSha256.GetLength(); ++i)
      {
         const wchar_t ch = mimeSha256[i];
         if (!((ch >= L'0' && ch <= L'9') ||
               (ch >= L'a' && ch <= L'f')))
            return -1;
      }

      SQLCommand command(
         _T("update hm_sentsubmissions set ssstate=2, ")
         _T("ssmessageid=@MESSAGEID, ssmimesha256=@SHA256, ")
         _T("ssmimebytes=@BYTES, ssconfirmedutc=@CONFIRMED ")
         _T("where sssubmissionid=@SID and ssaccountid=@ACCOUNT ")
         _T("and ssstate=1"));

      command.AddParameter("@MESSAGEID", messageId);
      command.AddParameter("@SHA256", mimeSha256);
      command.AddParameter("@BYTES", mimeBytes);
      command.AddParameter("@CONFIRMED", confirmedUtcSeconds);
      command.AddParameter("@SID", id);
      command.AddParameter("@ACCOUNT", accountId);

      bool reconnected = false;
      const __int64 affected = Db()->ExecuteCount(command, &reconnected);

      // A reconnect makes the outcome uncertain even if a retry affected
      // a row. Do not treat uncertainty as permission to create another
      // physical Sent copy or correlate an IMAP APPEND.
      if (reconnected)
         return -1;

      return affected == 1 ? 1 : -1;
   }
   // Build57.36: read-only discovery of one confirmed SMTP Sent copy.
   // No row is updated, claimed, deleted or marked as deduplicated here.
   // The final IMAP reconciliation must recheck all conditions atomically.
   bool PersistentSentSubmission::FindUniqueSentCandidate(
      __int64 accountId,
      __int64 folderId,
      const String &mimeSha256,
      __int64 mimeBytes,
      __int64 nowUtcSeconds,
      __int64 &messageId)
   {
      messageId = 0;

      if (!IsRecoveryReady() ||
          accountId <= 0 || folderId <= 0 ||
          mimeBytes <= 0 || nowUtcSeconds <= 0 ||
          mimeSha256.GetLength() != 64)
         return false;

      for (int i = 0; i < mimeSha256.GetLength(); ++i)
      {
         const wchar_t ch = mimeSha256[i];
         if (!((ch >= L'0' && ch <= L'9') ||
               (ch >= L'a' && ch <= L'f')))
            return false;
      }

      const __int64 earliest =
         nowUtcSeconds - SentDeduplication::MatchWindowSeconds;

      if (earliest < 0)
         return false;

      // Include the existing message row in the query.
      // A confirmed ledger record alone is insufficient: its
      // message must still belong to the same account and folder.
      //
      // SQL CE positional parameter order is significant.

      SQLCommand command(
         _T("select count(*) as matchcount, ")
         _T("min(s.ssmessageid) as matchedmessageid ")
         _T("from hm_sentsubmissions s ")
         _T("inner join hm_messages m ")
         _T("on m.messageid=s.ssmessageid ")
         _T("where s.ssaccountid=@ACCOUNT ")
         _T("and m.messageaccountid=@MESSAGEACCOUNT ")
         _T("and m.messagefolderid=@FOLDER ")
         _T("and s.ssstate=2 ")
         _T("and s.ssmimesha256=@SHA256 ")
         _T("and s.ssmimebytes=@BYTES ")
         _T("and s.ssconfirmedutc>=@EARLIEST ")
         _T("and s.ssconfirmedutc<=@NOW"));

      command.AddParameter("@ACCOUNT", accountId);
      command.AddParameter("@MESSAGEACCOUNT", accountId);
      command.AddParameter("@FOLDER", folderId);
      command.AddParameter("@SHA256", mimeSha256);
      command.AddParameter("@BYTES", mimeBytes);
      command.AddParameter("@EARLIEST", earliest);
      command.AddParameter("@NOW", nowUtcSeconds);

      auto rows = Db()->OpenRecordset(command);

      if (!rows || rows->IsEOF())
         return false;

      // Two identical SMTP submissions inside the window are
      // ambiguous and must NOT be collapsed into one APPEND.
      if (rows->GetLongValue("matchcount") != 1)
         return false;

      const __int64 candidate =
         rows->GetInt64Value("matchedmessageid");

      if (candidate <= 0)
         return false;

      messageId = candidate;
      return true;
   }
   // Build57.37: all eligibility conditions are rechecked in the
   // conditional UPDATE. The earlier lookup is never used alone
   // as permission to suppress the IMAP APPEND.
   //
   // This SQL CE statement requires runtime validation against
   // the migrated database schema before any IMAP integration.
   // Build57.44 - native, short transaction.
   // Observation/integration stage: NOT called from IMAP APPEND.
   // SQL CE does not accept the scalar COUNT subquery in the
   // former UPDATE. Discover and conditionally update instead
   // on one DatabaseTransaction connection.
   __int64 PersistentSentSubmission::ClaimUniqueSentCandidate(
      __int64 accountId,
      __int64 folderId,
      const String &mimeSha256,
      __int64 mimeBytes,
      __int64 nowUtcSeconds,
      __int64 &messageId)
   {
      messageId = 0;

      if (!IsRecoveryReady() ||
          accountId <= 0 || folderId <= 0 ||
          mimeBytes <= 0 ||
          nowUtcSeconds < SentDeduplication::MatchWindowSeconds ||
          mimeSha256.GetLength() != 64)
         return 0;

      for (int i = 0; i < mimeSha256.GetLength(); ++i)
      {
         const wchar_t ch = mimeSha256[i];
         if (!((ch >= L'0' && ch <= L'9') ||
               (ch >= L'a' && ch <= L'f')))
            return 0;
      }

      const __int64 earliest =
         nowUtcSeconds - SentDeduplication::MatchWindowSeconds;

      // No MIME, filesystem or mailbox operations inside this scope.
      auto manager = Application::Instance()->GetDBManager();

      DatabaseTransaction transaction(manager);

      // Do not acknowledge a claim through a joined scope: its
      // outer transaction may still roll back.
      if (!transaction.IsOpen() ||
          !transaction.IsTransactional() ||
          transaction.IsJoined())
         return -1;

      SQLCommand find(
         _T("select count(*) as matchcount, ")
         _T("min(s.ssmessageid) as matchedmessageid ")
         _T("from hm_sentsubmissions s ")
         _T("inner join hm_messages m ")
         _T("on m.messageid=s.ssmessageid ")
         _T("where s.ssaccountid=@ACCOUNT ")
         _T("and m.messageaccountid=@MESSAGEACCOUNT ")
         _T("and m.messagefolderid=@FOLDER ")
         _T("and s.ssstate=2 ")
         _T("and s.ssmimesha256=@SHA256 ")
         _T("and s.ssmimebytes=@BYTES ")
         _T("and s.ssconfirmedutc>=@EARLIEST ")
         _T("and s.ssconfirmedutc<=@NOW"));

      find.AddParameter("@ACCOUNT", accountId);
      find.AddParameter("@MESSAGEACCOUNT", accountId);
      find.AddParameter("@FOLDER", folderId);
      find.AddParameter("@SHA256", mimeSha256);
      find.AddParameter("@BYTES", mimeBytes);
      find.AddParameter("@EARLIEST", earliest);
      find.AddParameter("@NOW", nowUtcSeconds);

      __int64 candidateId = 0;

      {
         auto rows = transaction.OpenRecordset(find);

         if (!rows || rows->IsEOF())
            return -1;

         if (rows->GetLongValue("matchcount") != 1)
            return 0;

         candidateId = rows->GetInt64Value("matchedmessageid");
      }

      if (candidateId <= 0)
         return 0;

      SQLCommand update(
         _T("update hm_sentsubmissions set ssstate=3 ")
         _T("where ssmessageid=@MESSAGEID ")
         _T("and ssaccountid=@ACCOUNT ")
         _T("and ssstate=2 ")
         _T("and ssmimesha256=@SHA256 ")
         _T("and ssmimebytes=@BYTES ")
         _T("and ssconfirmedutc>=@EARLIEST ")
         _T("and ssconfirmedutc<=@NOW"));

      update.AddParameter("@MESSAGEID", candidateId);
      update.AddParameter("@ACCOUNT", accountId);
      update.AddParameter("@SHA256", mimeSha256);
      update.AddParameter("@BYTES", mimeBytes);
      update.AddParameter("@EARLIEST", earliest);
      update.AddParameter("@NOW", nowUtcSeconds);

      __int64 affected = -1;

      if (!transaction.Execute(update, nullptr, 0, &affected))
         return -1;

      // Zero means another claim won or eligibility changed.
      // More than one means schema/data ambiguity: roll back.
      if (affected != 1)
         return affected == 0 ? 0 : -1;

      // A failed/unknown COMMIT must never authorize suppression.
      if (!transaction.Commit())
         return -1;

      messageId = candidateId;
      return 1;
   }
   bool PersistentSentSubmission::GetState(const String &id, __int64 accountId,
                                            int &state, __int64 &messageId)
   {
      if (id.IsEmpty() || accountId <= 0)
         return false;
      SQLCommand command = ForSubmission(
         _T("select ssstate,ssmessageid from hm_sentsubmissions where sssubmissionid=@SID and ssaccountid=@ACCOUNT"), id, accountId);
      auto rows = Db()->OpenRecordset(command);
      if (!rows || rows->IsEOF())
         return false;
      state = static_cast<int>(rows->GetLongValue("ssstate"));
      messageId = rows->GetInt64Value("ssmessageid");
      return true;
   }

   __int64 PersistentSentSubmission::QuarantineUncertainClaims()
   {
      // Call only at a controlled single-instance cold start, BEFORE
      // accepting SMTP. Pending has no guaranteed durable source file after
      // delivery. CopyClaimed might already have a physical Sent copy.
      // Quarantine both states, without replay, deletion or mailbox scanning.
      SQLCommand command(_T("update hm_sentsubmissions set ssstate=6 where ssstate in (0,1)"));
      return Db()->ExecuteCount(command);
   }

   __int64 PersistentSentSubmission::MarkNeedsReview(const String &id, __int64 accountId)
   {
      if (id.IsEmpty() || accountId <= 0)
         return -1;
      SQLCommand command = ForSubmission(
         _T("update hm_sentsubmissions set ssstate=6 where sssubmissionid=@SID ")
         _T("and ssaccountid=@ACCOUNT and ssstate in (0,1)"), id, accountId);
      return Db()->ExecuteCount(command);
   }

   __int64 PersistentSentSubmission::PruneResolved(__int64 nowUtcSeconds)
   {
      if (nowUtcSeconds <= 0)
         return -1;
      // ServerCopied is a durable, confirmed copy: its correlation window
      // ends after 60 minutes. Pruning only the ledger cannot remove mail.
      // Pending, CopyClaimed and NeedsReview must survive expiration for review.
      SQLCommand command(_T("delete from hm_sentsubmissions where ssstate in (2,3,5) and ssexpiresutc<=@NOW"));
      command.AddParameter("@NOW", nowUtcSeconds);
      return Db()->ExecuteCount(command);
   }
}
