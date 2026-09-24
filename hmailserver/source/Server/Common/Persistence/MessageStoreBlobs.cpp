// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"

#include "MessageStoreBlobs.h"
#include "MessageStore.h"
#include "MessageStoreMaintenance.h"

#include "../SQL/DALRecordset.h"
#include "../SQL/DatabaseConnectionManager.h"

#include <ctime>
#include <map>
#include <set>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // Everything below is read and written under blob_lock.
      boost::recursive_mutex blob_lock;

      // How many DeletionHolds live, and whether one asked for the counts to be
      // rebuilt when the last ends.
      int deletion_holds = 0;
      bool recount_when_released = false;

      // While a hold lives: every count added since the first began, by blob -
      // what a recount in the service adds to what its walk found, since a name
      // counted while the walk reads may be one it never saw - and every blob
      // whose count was lowered, which the end of the hold looks at again.
      std::map<AnsiString, __int64> held_increments;
      std::set<AnsiString> held_lowered;

      // The database manager, or null while the application has none.
      std::shared_ptr<DatabaseConnectionManager> Db()
      {
         return Application::Instance()->GetDBManager();
      }

      void ReportBlobError(const String &where, const String &text)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7352, where, text);
      }
   }

   String
   MessageStoreBlobs::Root()
   {
      return FileUtilities::Combine(IniFileSettings::Instance()->GetDataDirectory(), _T("Blobs"));
   }

   String
   MessageStoreBlobs::PathOf(const AnsiString &sha256)
   {
      const String hex = sha256;
      return FileUtilities::Combine(FileUtilities::Combine(Root(), hex.Mid(0, 2)), hex);
   }

   bool
   MessageStoreBlobs::Exists(const AnsiString &sha256)
   {
      return MessageStore::Backend().Exists(PathOf(sha256));
   }

   bool
   MessageStoreBlobs::Read(const MessageStoreReference &reference, std::string &bytes, String &problem)
   {
      bytes.clear();

      const String path = PathOf(reference.sha256);

      // The length first, from the backend, so that a blob of the wrong size
      // costs a stat rather than a read of all of it.
      unsigned __int64 size = 0;
      if (!MessageStore::Backend().Size(path, size))
      {
         problem = "the blob " + String(reference.sha256) + " it references is missing or cannot be read";
         return false;
      }

      if (size != reference.length)
      {
         problem.Format(_T("the blob %s it references is %I64u bytes where %I64u are named"),
            String(reference.sha256).c_str(), size, reference.length);
         return false;
      }

      AnsiString contents;
      if (!MessageStore::Backend().ReadAll(path, contents, 0))
      {
         problem = "the blob " + String(reference.sha256) + " it references is missing or cannot be read";
         return false;
      }

      if ((unsigned __int64) contents.size() != reference.length)
      {
         problem.Format(_T("the blob %s it references is %I64u bytes where %I64u are named"),
            String(reference.sha256).c_str(), (unsigned __int64) contents.size(), reference.length);
         return false;
      }

      if (MessageStoreFormat::Hex(MessageStoreFormat::Sha256(contents.data(), contents.size())) != reference.sha256)
      {
         problem = "the blob " + String(reference.sha256) + " it references does not match its own name";
         return false;
      }

      bytes.assign(contents.data(), contents.size());
      return true;
   }

   bool
   MessageStoreBlobs::AddOne_(const AnsiString &sha256, unsigned __int64 length, String &problem)
   {
      std::shared_ptr<DatabaseConnectionManager> db = Db();
      if (!db)
      {
         problem = _T("the database is not open");
         return false;
      }

      SQLCommand select("select sbrefs from hm_storeblobs where sbsha256 = @SHA256");
      select.AddParameter("@SHA256", String(sha256));

      std::shared_ptr<DALRecordset> recordset = db->OpenRecordset(select);
      if (!recordset)
      {
         problem = _T("hm_storeblobs could not be read");
         return false;
      }

      if (!recordset->IsEOF())
      {
         SQLCommand update("update hm_storeblobs set sbrefs = sbrefs + 1 where sbsha256 = @SHA256");
         update.AddParameter("@SHA256", String(sha256));
         if (!db->Execute(update))
         {
            problem = _T("hm_storeblobs could not be updated");
            return false;
         }
      }
      else
      {
         SQLStatement statement(SQLStatement::STInsert, "hm_storeblobs");
         statement.AddColumn("sbsha256", String(sha256));
         statement.AddColumnInt64("sbsize", (__int64) length);
         statement.AddColumn("sbrefs", 1);
         statement.AddColumnInt64("sbcreated", (__int64) time(nullptr));
         statement.SetIdentityColumn("sbid");

         __int64 made = 0;
         if (!db->Execute(statement, &made))
         {
            problem = _T("a row could not be added to hm_storeblobs");
            return false;
         }
      }

      // Counted while a hold lives: a recount in the service adds it to what its
      // walk finds, since the name it is for may not be one the walk saw.
      if (deletion_holds > 0)
         held_increments[sha256]++;

      return true;
   }

   void
   MessageStoreBlobs::ReleaseOne_(const AnsiString &sha256)
   {
      std::shared_ptr<DatabaseConnectionManager> db = Db();
      if (!db)
         return;

      SQLCommand update("update hm_storeblobs set sbrefs = sbrefs - 1 where sbsha256 = @SHA256 and sbrefs > 0");
      update.AddParameter("@SHA256", String(sha256));
      if (!db->Execute(update))
      {
         // Left counted: a blob kept too long is what the recount is for.
         ReportBlobError(_T("MessageStoreBlobs::Release"), "The count of the blob " + String(sha256) + " could not be lowered; it is kept, and a recount (a restore, or hmailserver --recount-store) corrects the count.");
         return;
      }

      // Held: the count may reach none, and the blob and its row are kept; the
      // end of the hold looks at it again.
      if (deletion_holds > 0)
      {
         held_lowered.insert(sha256);
         return;
      }

      DeleteIfNone_(sha256);
   }

   void
   MessageStoreBlobs::DeleteIfNone_(const AnsiString &sha256)
   {
      std::shared_ptr<DatabaseConnectionManager> db = Db();
      if (!db)
         return;

      SQLCommand select("select sbrefs from hm_storeblobs where sbsha256 = @SHA256");
      select.AddParameter("@SHA256", String(sha256));
      std::shared_ptr<DALRecordset> recordset = db->OpenRecordset(select);
      if (!recordset || recordset->IsEOF() || recordset->GetLongValue("sbrefs") > 0)
         return;

      SQLCommand remove("delete from hm_storeblobs where sbsha256 = @SHA256 and sbrefs <= 0");
      remove.AddParameter("@SHA256", String(sha256));
      if (!db->Execute(remove))
         return;

      // The file only when the row is gone: a delete that matched nothing - the
      // count raised again between the two statements - leaves a row, and the
      // part it counts stays.
      std::shared_ptr<DALRecordset> after = db->OpenRecordset(select);
      if (!after || !after->IsEOF())
         return;

      MessageStore::Backend().Remove(PathOf(sha256));
   }

   bool
   MessageStoreBlobs::Keep(const std::string &original, const std::vector<MessageStoreReference> &references, String &problem)
   {
      boost::recursive_mutex::scoped_lock lock(blob_lock);

      std::vector<MessageStoreReference> counted;

      for (size_t i = 0; i < references.size(); i++)
      {
         const MessageStoreReference &reference = references[i];
         const String path = PathOf(reference.sha256);

         // The same bytes are already here: the whole point - when they are the
         // same bytes. What is there is checked against its name before it is
         // counted for a message whose own copy is about to be replaced by a
         // reference to it, and written again from this message's bytes, which
         // hash to the name, when it is not: a blob damaged on disk, or one of
         // the right length and the wrong bytes, would otherwise take this
         // message down with the others that share it.
         bool writeIt = !MessageStore::Backend().Exists(path);
         if (!writeIt)
         {
            std::string existing;
            String damage;
            if (!Read(reference, existing, damage))
            {
               ReportBlobError(_T("MessageStoreBlobs::Keep"), "The blob " + path + " was not the part its name says (" + damage + "); it is written again from a message that carries the part.");
               writeIt = true;
            }
         }

         if (writeIt)
         {
            const AnsiString bytes(original.data() + (size_t) reference.offset, (size_t) reference.length);
            if (!MessageStore::Backend().WriteAtomically(path, bytes))
            {
               problem = "the blob " + String(reference.sha256) + " could not be written to " + path;
               Release(counted);
               return false;
            }
         }

         if (!AddOne_(reference.sha256, reference.length, problem))
         {
            Release(counted);
            return false;
         }

         counted.push_back(reference);
      }

      return true;
   }

   bool
   MessageStoreBlobs::AddReferences(const std::vector<MessageStoreReference> &references, String &problem)
   {
      boost::recursive_mutex::scoped_lock lock(blob_lock);

      std::vector<MessageStoreReference> counted;
      for (size_t i = 0; i < references.size(); i++)
      {
         if (!Exists(references[i].sha256))
         {
            problem = "the blob " + String(references[i].sha256) + " it references is missing";
            Release(counted);
            return false;
         }

         if (!AddOne_(references[i].sha256, references[i].length, problem))
         {
            Release(counted);
            return false;
         }

         counted.push_back(references[i]);
      }

      return true;
   }

   void
   MessageStoreBlobs::Release(const std::vector<MessageStoreReference> &references)
   {
      boost::recursive_mutex::scoped_lock lock(blob_lock);

      for (size_t i = 0; i < references.size(); i++)
         ReleaseOne_(references[i].sha256);
   }

   bool
   MessageStoreBlobs::Adopt(const String &file, const AnsiString &sha256, unsigned __int64 length, String &problem)
   {
      boost::recursive_mutex::scoped_lock lock(blob_lock);

      if (Exists(sha256))
         return true;

      AnsiString bytes;
      if (!MessageStore::Backend().ReadAll(file, bytes, 0))
      {
         problem = "the blob " + String(sha256) + " could not be read from " + file;
         return false;
      }

      if ((unsigned __int64) bytes.size() != length ||
          MessageStoreFormat::Hex(MessageStoreFormat::Sha256(bytes.data(), bytes.size())) != sha256)
      {
         problem = "the blob " + String(sha256) + " in " + file + " is not the part its name says";
         return false;
      }

      if (!MessageStore::Backend().WriteAtomically(PathOf(sha256), bytes))
      {
         problem = "the blob " + String(sha256) + " could not be written into the store";
         return false;
      }

      return true;
   }

   MessageStoreBlobs::DeletionHold::DeletionHold(bool recountWhenDone)
   {
      boost::recursive_mutex::scoped_lock lock(blob_lock);

      // The first hold starts the record the end of the last one reads.
      if (deletion_holds++ == 0)
      {
         held_increments.clear();
         held_lowered.clear();
         recount_when_released = false;
      }

      if (recountWhenDone)
         recount_when_released = true;
   }

   MessageStoreBlobs::DeletionHold::~DeletionHold()
   {
      bool recount = false;
      {
         boost::recursive_mutex::scoped_lock lock(blob_lock);

         if (deletion_holds > 1)
         {
            deletion_holds--;
            return;
         }

         recount = recount_when_released;
      }

      // The last hold, still in force while the counts are rebuilt: a count
      // the restore took to none stays none-and-kept until the recount has
      // raised it, and nothing it names is deleted in between. The recount only
      // raises (SetCounts), so a name counted while it reads keeps its count. A
      // failure is reported and leaves the counts as they are; then nothing
      // below is deleted either, since a count left at none may be a name the
      // recount did not reach.
      bool recounted = !recount;
      if (recount)
      {
         try
         {
            String summary;
            if (!MessageStoreMaintenance::Recount(summary, false))
               ReportBlobError(_T("MessageStoreBlobs::DeletionHold"), "The blob counts could not be rebuilt after the restore: " + summary + " No blob was deleted; run hmailserver --recount-store with the service stopped to rebuild them.");
            else
            {
               LOG_APPLICATION("Message store: " + summary);
               recounted = true;
            }
         }
         catch (...)
         {
            ReportBlobError(_T("MessageStoreBlobs::DeletionHold"), _T("The blob counts could not be rebuilt after the restore. No blob was deleted; run hmailserver --recount-store with the service stopped to rebuild them."));
         }
      }

      boost::recursive_mutex::scoped_lock lock(blob_lock);

      // Another hold began while this one recounted: it ends the record.
      if (--deletion_holds > 0)
         return;

      std::set<AnsiString> lowered;
      lowered.swap(held_lowered);
      held_increments.clear();
      recount_when_released = false;

      // What the hold kept that nothing needs now: each blob whose count it saw
      // lowered and that is none still, deleted as it would have been.
      if (!recounted)
         return;

      for (auto sha256 = lowered.begin(); sha256 != lowered.end(); ++sha256)
         DeleteIfNone_(*sha256);
   }

   bool
   MessageStoreBlobs::IsHeld()
   {
      boost::recursive_mutex::scoped_lock lock(blob_lock);
      return deletion_holds > 0;
   }

   bool
   MessageStoreBlobs::Any()
   {
      std::shared_ptr<DatabaseConnectionManager> db = Db();
      if (!db)
         return false;

      SQLCommand select("select count(*) as blobcount from hm_storeblobs");
      std::shared_ptr<DALRecordset> recordset = db->OpenRecordset(select);
      if (!recordset || recordset->IsEOF())
         return false;

      return recordset->GetLongValue("blobcount") > 0;
   }

   bool
   MessageStoreBlobs::SetCounts(const std::map<AnsiString, std::pair<unsigned __int64, __int64> > &counts, bool exact, String &summary)
   {
      boost::recursive_mutex::scoped_lock lock(blob_lock);

      std::shared_ptr<DatabaseConnectionManager> db = Db();
      if (!db)
      {
         summary = _T("the database is not open.");
         return false;
      }

      std::map<AnsiString, __int64> existing;
      if (!List(existing))
      {
         summary = _T("hm_storeblobs could not be read.");
         return false;
      }

      int changed = 0;
      int added = 0;
      int removed = 0;

      for (auto row = existing.begin(); row != existing.end(); ++row)
      {
         auto counted = counts.find(row->first);

         // In the service a count is never lowered and a row never removed:
         // names are counted while the walk reads, before their rows exist, and
         // a count too low is the one mistake this table is not allowed. What
         // the walk found is raised by every count added since the first hold
         // began; too high is the safe side.
         if (!exact)
         {
            __int64 target = counted == counts.end() ? 0 : counted->second.second;
            auto added_since = held_increments.find(row->first);
            if (added_since != held_increments.end())
               target += added_since->second;

            if (target <= row->second)
               continue;

            SQLCommand update("update hm_storeblobs set sbrefs = @REFS where sbsha256 = @SHA256");
            update.AddParameter("@REFS", target);
            update.AddParameter("@SHA256", String(row->first));
            if (!db->Execute(update))
            {
               summary = _T("a count in hm_storeblobs could not be set.");
               return false;
            }
            changed++;
            continue;
         }

         if (counted == counts.end())
         {
            SQLCommand remove("delete from hm_storeblobs where sbsha256 = @SHA256");
            remove.AddParameter("@SHA256", String(row->first));
            if (!db->Execute(remove))
            {
               summary = _T("a row of hm_storeblobs could not be removed.");
               return false;
            }
            removed++;
            continue;
         }

         if (counted->second.second != row->second)
         {
            SQLCommand update("update hm_storeblobs set sbrefs = @REFS where sbsha256 = @SHA256");
            update.AddParameter("@REFS", counted->second.second);
            update.AddParameter("@SHA256", String(row->first));
            if (!db->Execute(update))
            {
               summary = _T("a count in hm_storeblobs could not be set.");
               return false;
            }
            changed++;
         }
      }

      for (auto count = counts.begin(); count != counts.end(); ++count)
      {
         if (existing.find(count->first) != existing.end())
            continue;

         __int64 refs = count->second.second;
         if (!exact)
         {
            auto added_since = held_increments.find(count->first);
            if (added_since != held_increments.end())
               refs += added_since->second;
         }

         SQLStatement statement(SQLStatement::STInsert, "hm_storeblobs");
         statement.AddColumn("sbsha256", String(count->first));
         statement.AddColumnInt64("sbsize", (__int64) count->second.first);
         statement.AddColumnInt64("sbrefs", refs);
         statement.AddColumnInt64("sbcreated", (__int64) time(nullptr));
         statement.SetIdentityColumn("sbid");

         __int64 made = 0;
         if (!db->Execute(statement, &made))
         {
            summary = _T("a row could not be added to hm_storeblobs.");
            return false;
         }
         added++;
      }

      if (exact)
         summary.Format(_T("the blob counts were rebuilt from the stored files: %d blob(s) referenced, %d count(s) corrected, %d row(s) added and %d removed for blobs nothing references (their files are left for the consistency check to report)."),
            (int) counts.size(), changed, added, removed);
      else
         summary.Format(_T("the blob counts were raised to what the stored files reference: %d blob(s) referenced, %d count(s) raised and %d row(s) added; none was lowered, which only hmailserver --recount-store does, with the service stopped."),
            (int) counts.size(), changed, added);
      return true;
   }

   bool
   MessageStoreBlobs::List(std::map<AnsiString, __int64> &counts)
   {
      counts.clear();

      std::shared_ptr<DatabaseConnectionManager> db = Db();
      if (!db)
         return false;

      SQLCommand select("select sbsha256, sbrefs from hm_storeblobs");
      std::shared_ptr<DALRecordset> recordset = db->OpenRecordset(select);
      if (!recordset)
         return false;

      while (!recordset->IsEOF())
      {
         counts[AnsiString(recordset->GetStringValue("sbsha256"))] = (__int64) recordset->GetLongValue("sbrefs");
         recordset->MoveNext();
      }

      return true;
   }

   bool
   MessageStoreBlobs::GetTotals(Totals &totals)
   {
      totals = Totals();

      std::shared_ptr<DatabaseConnectionManager> db = Db();
      if (!db)
         return false;

      SQLCommand select("select sbsize, sbrefs from hm_storeblobs");
      std::shared_ptr<DALRecordset> recordset = db->OpenRecordset(select);
      if (!recordset)
         return false;

      while (!recordset->IsEOF())
      {
         totals.blobs++;
         totals.bytes += recordset->GetInt64Value("sbsize");
         totals.references += (__int64) recordset->GetLongValue("sbrefs");
         recordset->MoveNext();
      }

      return true;
   }
}
