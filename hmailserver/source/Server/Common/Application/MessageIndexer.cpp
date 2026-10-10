// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "MessageIndexer.h"

#include "../Application/ExceptionHandler.h"
#include "../BO/Message.h"
#include "../BO/MessageData.h"
#include "../BO/MessageMetaData.h"
#include "../Mime/Mime.h"
#include "../Util/Time.h"
#include "../Persistence/PersistentMessageMetaData.h"
#include "../Persistence/PersistentMessage.h"
#include "../Persistence/PersistentMessageIndex.h"
#include "../Util/MailboxKeys.h"
#include "../Util/ServerStatus.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // The reconciliation pass (heal delivered messages the cursor walked
      // past, or whose terms were removed after an edit) runs on this many
      // caught-up loop iterations - about hourly at the one-minute wait. It
      // exists for rare races, so it does not need to be fast, and its
      // anti-join over the message table is not free on a large store.
      const int FullTextReconcileIterations = 60;
   }

   MessageIndexer::MessageIndexer() :
      stop_asked_on_its_thread_(false),
      iIndexRunCount(0),
      full_text_iteration_count_(0),
      full_text_reconcile_active_(false)
   {
      work_ = boost::bind(&MessageIndexer::WorkerFuncInternal, this);
   }

   MessageIndexer::MessageIndexer(const boost::function<void()> &work) :
      work_(work),
      stop_asked_on_its_thread_(false),
      iIndexRunCount(0),
      full_text_iteration_count_(0),
      full_text_reconcile_active_(false)
   {

   }

   MessageIndexer::~MessageIndexer(void)
   {
      // The server's indexer is never destroyed (Singleton); a tester's is, and
      // a thread object let go of while its thread runs leaves the thread with
      // this object gone under it.
      Stop();
   }

   void
   MessageIndexer::Start()
   {
      boost::lock_guard<boost::recursive_mutex> guard(starterMutex_);

      // From StopServers to the next StartServers the database manager may be
      // gone - a Reinitialize closes it after StopServers and makes a new one
      // before StartServers - and the stop has waited for the indexer already.
      // An indexer started meanwhile, by the setting turned on over COM or the
      // REST API, would make its first statement through an empty pointer. So it
      // is left to StartServers, which starts the indexer when the setting is on:
      // Configuration::SetMessageIndexing saves the setting before it calls this,
      // so that a setting saved before StartServers reads it is read there, and
      // one saved after finds the state Starting or Running here. A setting saved
      // while the database is closed is not saved: its save goes through the same
      // empty database manager, as any setting's does then, and never gets here.
      const int state = ServerStatus::Instance()->GetState();
      if (state == ServerStatus::StateStopping || state == ServerStatus::StateStopped)
      {
         LOG_DEBUG("The message indexer is not started while the servers are stopped; it starts with them.");
         return;
      }

      if (workerThread_.joinable())
      {
         // On the indexer's own thread, by something it runs - an OnError script
         // setting MessageIndexing.Enabled: this is the indexer, which is neither
         // joined - boost throws "trying joining itself", as the timed_join below
         // did for such a Start until 10 October 2026, which the script met as a
         // COM error and its setting was not saved - nor started again. A Stop the
         // same thread made before is taken back, and the indexer goes on.
         if (boost::this_thread::get_id() == workerThread_.get_id())
         {
            if (stop_asked_on_its_thread_)
               TakeBackStopOnItsThread_();

            return;
         }

         // Running, and not asked to stop: this is the indexer.
         if (!stop_asked_on_its_thread_ && !workerThread_.timed_join(boost::posix_time::milliseconds(0)))
            return;

         // Asked to stop on its own thread and not yet ended: waited for.
         if (workerThread_.joinable())
            workerThread_.join();
      }

      stop_asked_on_its_thread_ = false;

      LOG_DEBUG("Starting message indexer...");

      std::function<void ()> func = std::bind( &MessageIndexer::WorkerFunc, this );
      workerThread_ = boost::thread(func);

   }

   void
   MessageIndexer::TakeBackStopOnItsThread_()
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // On the indexer's thread, after a Stop on it: the interruption the Stop asked
   // for is pending on this thread, and an interruption point throws it and
   // forgets it. Unless interruptions are turned off on the thread just now, when
   // it stays pending and the thread ends at its next interruption point after
   // all, as the Stop asked.
   //---------------------------------------------------------------------------()
   {
      try
      {
         boost::this_thread::interruption_point();
      }
      catch (boost::thread_interrupted &)
      {
         // The Stop's, taken back.
      }

      if (!boost::this_thread::interruption_requested())
         stop_asked_on_its_thread_ = false;
   }

   void
   MessageIndexer::WorkerFunc()
   {
      // ExceptionHandler::Run reports an exception the pass throws and answers
      // false - on POSIX too, since 10 October 2026 - so that the thread ends
      // here and the process goes on; the next Start starts a new one.
      ExceptionHandler::Run("MessageIndexer", work_);
      LOG_DEBUG("Message indexer stopped.");
   }

   void
   MessageIndexer::WorkerFuncInternal()
   {
      LOG_DEBUG("Message indexer started...");

      PersistentMessageMetaData persistentMetaData;
      persistentMetaData.DeleteOrphanedItems();

      if (IniFileSettings::Instance()->GetIndexerFullTextEnabled())
         PersistentMessageIndex::DeleteOrphanedItems();

      while (true)
      {
         IndexMessages_();

         const bool fullTextCaughtUp = IndexFullTextBatch_();

         full_text_iteration_count_++;

         if (!fullTextCaughtUp)
         {
            // Backfill in progress. A short interruptible pause between
            // bounded passes is the throttle: each pass touches at most one
            // batch, so the database is never held for long, and a service
            // stop lands on either this sleep or the per-message interruption
            // point within a batch. The cursor makes the resume a resume.
            boost::this_thread::sleep_for(boost::chrono::milliseconds(250));
            continue;
         }

         if (IniFileSettings::Instance()->GetIndexerFullTextEnabled())
         {
            // Terms of deleted messages, on the full-index cadence. Stale
            // terms are dead rows, never wrong answers - a search only ever
            // sees messages still in the folder - so this is housekeeping,
            // not correctness, and can be slow.
            int fullMinutes = IniFileSettings::Instance()->GetIndexerFullMinutes();
            if (fullMinutes < 1)
               fullMinutes = 720;

            if ((full_text_iteration_count_ % fullMinutes) == 0)
               PersistentMessageIndex::DeleteOrphanedItems();
         }

         index_now_.WaitFor(boost::chrono::minutes(1));
      }

   }

   void
   MessageIndexer::IndexNow()
   {
      index_now_.Set();
   }

   void
   MessageIndexer::Stop(const std::function<void()> &progress)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Interrupts the thread and waits for it to end. Until 10 October 2026 this
   // interrupted and returned, and the thread went on to its next interruption
   // point - the rest of a message of a batch, a statement - while StopServers
   // returned and a Reinitialize closed the database (Application::ExitInstance):
   // the statement went through the empty database manager, threw (HM4208,
   // "boost unique_lock has no mutex"), and on Linux the exception ended the
   // process. And a Start made while that thread was still ending - the start of
   // the same Reinitialize - found it running and started none, so the indexer
   // stayed stopped until the setting was saved again.
   //---------------------------------------------------------------------------()
   {
      boost::lock_guard<boost::recursive_mutex> guard(starterMutex_);

      if (!workerThread_.joinable())
         return;

      workerThread_.interrupt();

      // Asked on the thread itself, which cannot wait for its own end: it ends
      // at its next interruption point, and Start waits for that.
      if (boost::this_thread::get_id() == workerThread_.get_id())
      {
         stop_asked_on_its_thread_ = true;
         return;
      }

      LOG_DEBUG("Stopping message indexer.");

      while (!workerThread_.timed_join(boost::posix_time::seconds(1)))
      {
         if (progress)
            progress();
      }

      stop_asked_on_its_thread_ = false;
   }

   bool
   MessageIndexer::IndexFullTextBatch_()
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // One bounded pass of the full-text term index. Bounded is the contract:
   // at most one batch of messages is read and indexed per call, so however
   // large the store, no single pass can monopolise the database or the disk.
   // Answers true when there is nothing (more) to do right now.
   //
   // The cursor walk indexes delivered messages in id order and persists the
   // high-water mark after each batch, which is what makes a service stop in
   // the middle of a week-long backfill resume where it left off instead of
   // starting over. Id ranges rather than row counts, with a MIN() jump over
   // empty id space, so the walk needs no ORDER BY/TOP - which every engine
   // spells differently - and re-running a range is idempotent.
   //
   // Any database failure answers true WITHOUT advancing the cursor: the
   // caller then waits the full minute, which is the retry backoff, and
   // nothing is skipped.
   //---------------------------------------------------------------------------()
   {
      if (!IniFileSettings::Instance()->GetIndexerFullTextEnabled())
         return true;

      const int batchSize = IniFileSettings::Instance()->GetIndexerFullTextBatchSize();

      const __int64 frontier = PersistentMessageIndex::GetLatestDeliveredMessageId();
      const __int64 cursor = PersistentMessageIndex::GetBackfillCursor();

      if (cursor < frontier)
      {
         // Jump the cursor over id space with nothing delivered in it, so a
         // store whose ids start in the millions is not walked from zero.
         const __int64 firstAbove = PersistentMessageIndex::GetFirstMessageIdAbove(cursor);
         if (firstAbove == 0 || firstAbove > frontier)
         {
            PersistentMessageIndex::SetBackfillCursor(frontier);
            return true;
         }

         const __int64 low = firstAbove - 1;
         __int64 high = low + batchSize;
         if (high > frontier)
            high = frontier;

         std::vector<PersistentMessageIndex::IndexableMessage> messages;
         if (!PersistentMessageIndex::GetMessagesInRange(low, high, messages))
            return true;

         // A message skipped for a mailbox whose encryption was not known is
         // passed: without the complete-marker it is read at every search, and
         // the reconciliation finds it - so one account the server cannot tell
         // about never holds the backfill up.
         bool skipped = false;
         if (!IndexFullTextMessages_(messages, skipped))
            return true;

         if (!PersistentMessageIndex::SetBackfillCursor(high))
            return true;

         return high >= frontier;
      }

      // Caught up. Occasionally reconcile: a message the cursor passed before
      // its delivery committed, an interrupted batch, or terms deleted after
      // an edit all lack the complete-marker and are scanned on every search
      // until this finds them. Once a reconciliation finds a full batch it
      // stays active across passes rather than waiting for the next hourly
      // gate, so a real backlog heals at backfill speed.
      if (!full_text_reconcile_active_ && (full_text_iteration_count_ % FullTextReconcileIterations) != 0)
         return true;

      std::vector<PersistentMessageIndex::IndexableMessage> unindexed;
      if (!PersistentMessageIndex::GetUnindexedMessages(cursor, batchSize, unindexed))
         return true;

      if (unindexed.empty())
      {
         full_text_reconcile_active_ = false;
         return true;
      }

      bool skipped = false;
      if (!IndexFullTextMessages_(unindexed, skipped))
         return true;

      // What could not be told about is asked again at the next reconciliation,
      // a minute on at the soonest, not at once.
      if (skipped)
      {
         full_text_reconcile_active_ = false;
         return true;
      }

      // A full batch means there may be more to heal; an emptier one means
      // this reconciliation is done.
      full_text_reconcile_active_ = (int) unindexed.size() >= batchSize;
      return !full_text_reconcile_active_;
   }

   bool
   MessageIndexer::IndexFullTextMessages_(const std::vector<PersistentMessageIndex::IndexableMessage> &messages, bool &skipped)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Reads, tokenises and stores the terms for a batch of messages. The text
   // tokenised is EXACTLY the text the searcher scans - the raw header and the
   // body/HTML-body pair selected by MessageData's multipart walk, obtained
   // through the same MessageData the scan itself uses - because the index is
   // only allowed to say "cannot contain", and it can only say that about the
   // string the scan would actually search.
   //---------------------------------------------------------------------------()
   {
      const size_t maxTerms = (size_t) IniFileSettings::Instance()->GetIndexerFullTextMaxTokensPerMessage();

      // The accounts this batch stored words of, and the second look at them:
      // an account switched to encryption at rest while this batch read its
      // messages - the "no key" below read just before the switch-on put it in
      // the keyed set - has what the batch stored of its words dropped again.
      // The switch-on drops the index after it puts the account in the set,
      // and this asks the set after the words were written: one of the two
      // comes after the other, so nothing is left standing (docs/
      // EncryptionAtRest.md, "The index"). Asked of the set in memory, which
      // costs nothing where no account is keyed, and on every way out of this
      // function - a batch that stops part way wrote words too. Not known is
      // not keyed here: a batch that could not tell skipped those messages.
      std::set<__int64> termsWritten;
      skipped = false;
      struct SecondLook
      {
         explicit SecondLook(std::set<__int64> &written) : accounts(written) {}
         // Drops what the index holds of the accounts when it goes: a copy would
         // drop it twice.
         SecondLook(const SecondLook &) = delete;
         SecondLook &operator=(const SecondLook &) = delete;

         std::set<__int64> &accounts;
         ~SecondLook()
         {
            try
            {
               for (__int64 accountId : accounts)
               {
                  if (MailboxKeys::KeyedState(accountId) == MailboxKeys::KeyedYes || MailboxKeys::KeyedState(accountId) == MailboxKeys::KeyedDamaged)
                     MailboxKeys::ForgetIndex(accountId);
               }
            }
            catch (...)
            {
               // A destructor, on the way out of a batch: the next batch's look
               // at the account, or the switch-on's own drop, stands.
            }
         }
      } secondLook(termsWritten);

      for (const PersistentMessageIndex::IndexableMessage &messageToIndex : messages)
      {
         boost::this_thread::interruption_point();

         // Whether the mailbox is encrypted at rest not known just now: the
         // message is left unindexed - neither its words nor the overflow
         // marker, which would stand for good - and read at every search until
         // a later pass can tell (the callers, above).
         const MailboxKeys::Keyed keyed = MailboxKeys::KeyedState(messageToIndex.AccountID);
         if (keyed == MailboxKeys::KeyedUnknown)
         {
            skipped = true;
            continue;
         }

         std::shared_ptr<Message> message = std::shared_ptr<Message>(new Message);
         message->SetID(messageToIndex.MessageID);
         message->SetAccountID(messageToIndex.AccountID);
         message->SetPartialFileName(messageToIndex.FileName);

         std::set<String> terms;
         bool overflowed = false;

         std::shared_ptr<MessageData> messageData = std::shared_ptr<MessageData>(new MessageData());

         // A mailbox encrypted at rest (Util/MailboxKeys.h) has no term of its
         // text stored anywhere: the table is not under the account's key, and
         // a term is the word it is. Every message of such an account gets the
         // overflow marker instead - a candidate for every search, which reads
         // it while the key is open, exactly as the index-less server always
         // searched - whether its key happens to be open now or not; and so does
         // one whose key row holds no key, whose files may be under the key it
         // held.
         if (keyed == MailboxKeys::KeyedYes || keyed == MailboxKeys::KeyedDamaged)
         {
            overflowed = true;
         }
         else if (messageData->LoadFromMessage(messageToIndex.FileName, message))
         {
            // Header first; body and HTML body directly concatenated after
            // it, with nothing in between. SEARCH BODY scans GetBody() +
            // GetHTMLBody() as ONE string, so a token straddling that
            // junction is real and must exist in the index as the merged
            // token. Concatenation can only merge tokens, never split them,
            // and the searcher's needles are substring (LIKE) probes, so a
            // merged token still answers for its halves.
            // The text-bearing attachments as well, in the same concatenation the
            // BODY scan reads: the index may only ever exclude, so it has to know
            // about every piece of text the scan would search.
            //
            // A sealed message needs no special case HERE, and that is a
            // decision rather than an accident - say so, because the obvious
            // "improvement" breaks the index's one invariant.
            //
            // S/MIME and PGP/MIME: GetBody() and GetHTMLBody() walk the MIME
            // tree for a text/plain and a text/html part and a sealed message
            // has neither, so only the header is indexed, which is exactly what
            // the searcher can find in one. Nothing to do.
            //
            // Inline ("armoured in the body") OpenPGP: the message IS a
            // text/plain part, so its base64 armour is tokenised here, and past
            // IndexerFullTextMaxTokensPerMessage the overflow marker makes that
            // message a candidate for every search from then on. That is
            // wasteful and it is still correct, and skipping the armour would
            // NOT be: the index is only ever allowed to say "cannot contain",
            // and the searcher scans GetBody() - armour and all - so an index
            // built from less than the searcher reads would exclude a message
            // the searcher would have matched. If this is ever made cheaper,
            // the searcher has to stop reading the armour in the same commit.
            // See hmailserver/docs/SealedMail.md.
            String searchableText = messageData->GetHeader() + messageData->GetBody() + messageData->GetHTMLBody() + messageData->GetAttachmentText();

            PersistentMessageIndex::TokenizeForIndex(searchableText, maxTerms, terms, overflowed);
         }
         else
         {
            // Unreadable right now - locked by a scanner, missing, or over the
            // MIME parser's size cap. The overflow marker keeps it a candidate
            // for every search, which is exactly what a message the scan also
            // cannot read gets today.
            overflowed = true;
         }

         if (!PersistentMessageIndex::SaveTermsForMessage(messageToIndex.MessageID, messageToIndex.AccountID, terms, overflowed))
         {
            // A message deleted between the listing and this save is not a
            // failure of the index: its row is gone, the terms have no row to
            // hang off, and there is nothing left to index. The insert selects
            // from hm_messages itself since 15 September 2026, so a message
            // that is gone inserts nothing rather than drawing the foreign
            // key's refusal; what is left is the instant between that select
            // and the insert, and this is the answer to it.
            std::shared_ptr<Message> stillThere = std::shared_ptr<Message>(new Message);
            if (!PersistentMessage::ReadObject(stillThere, messageToIndex.MessageID) || stillThere->GetID() == 0)
            {
               LOG_DEBUG(Formatter::Format(_T("MessageIndexer: message {0} was deleted before its terms were saved; nothing to index."), messageToIndex.MessageID));
               continue;
            }

            String errorMessage;
            errorMessage.Format(_T("Failed to save full-text index terms for message %I64d. The batch will be retried."),
               messageToIndex.MessageID);

            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6260, "MessageIndexer::IndexFullTextMessages_", errorMessage);
            return false;
         }

         if (!terms.empty())
            termsWritten.insert(messageToIndex.AccountID);
      }

      return true;
   }

   void
   MessageIndexer::IndexMessages_()
   {
      // Default is 720
      int iIndexerFullMin = IniFileSettings::Instance()->GetIndexerFullMinutes();

      PersistentMessageMetaData persistentMetaData;
      // set quickIndex here so the loop will allow loading of an incomplete index
      bool bDoQuickIndex = true;
      if ((iIndexRunCount > iIndexerFullMin) || (iIndexRunCount == 0)){
        bDoQuickIndex = false;
        iIndexRunCount = 1;
      }

      while (true)
      {

         // added the boolean quickIndex to tell the funciton to use the quick index or the full index
         std::set<std::shared_ptr<PersistentMessageMetaData::MessageInfo> > messagesToIndex = persistentMetaData.GetMessagesToIndex(bDoQuickIndex);
         if (messagesToIndex.size() == 0)
         {
            LOG_DEBUG("No messages to index.");
            iIndexRunCount++;  //JDR
            // No messages to index found.
            return;
         }

         // The accounts this batch wrote the sort's copy of headers for, and
         // the second look at them, as for the words: an account switched to
         // encryption at rest after the query left it in has what this batch
         // wrote dropped again - asked of the keyed set, on every way out of
         // the batch. The query leaves such an account out from then on.
         std::set<__int64> accounts;
         struct SecondLook
         {
            std::set<__int64> &accounts;
            ~SecondLook()
            {
               try
               {
                  for (__int64 accountId : accounts)
                  {
                     if (MailboxKeys::KeyedState(accountId) == MailboxKeys::KeyedYes || MailboxKeys::KeyedState(accountId) == MailboxKeys::KeyedDamaged)
                        MailboxKeys::ForgetIndex(accountId);
                  }
               }
               catch (...)
               {
                  // A destructor, on the way out of a batch.
               }
            }
         } secondLook = { accounts };

         for(std::shared_ptr<PersistentMessageMetaData::MessageInfo> messageToIndex : messagesToIndex)
         {
            boost::this_thread::interruption_point();

            accounts.insert(messageToIndex->AccountID);

            AnsiString headerText = PersistentMessage::LoadHeader(messageToIndex->FileName, false);

            MimeHeader header;
            header.Load(headerText, headerText.GetLength(), true);


            String dateString = header.GetUnicodeFieldValue("Date");
            DateTime date = Time::GetDateTimeFromMimeHeader(dateString);

            String from = header.GetUnicodeFieldValue("From");
            String subject = header.GetUnicodeFieldValue("Subject");
            String cc = header.GetUnicodeFieldValue("CC");
            String to = header.GetUnicodeFieldValue("TO");

            std::shared_ptr<MessageMetaData> metaData = std::shared_ptr<MessageMetaData>(new MessageMetaData);

            metaData->SetAccountID(messageToIndex->AccountID);
            metaData->SetFolderID(messageToIndex->FolderID);
            metaData->SetMessageID(messageToIndex->MessageID);

            metaData->SetDate(date);
            metaData->SetFrom(from);
            metaData->SetSubject(subject);
            metaData->SetCC(cc);
            metaData->SetTo(to);

            if (!persistentMetaData.SaveObject(metaData))
            {
               LOG_DEBUG("Error saving the index.")
               // Error saving. Abort now...
               return;
            }
         }

      }
      iIndexRunCount++;
   }

   namespace
   {
      // The tester's stand-in for the indexer's loop, and what it saw. A pass is
      // the indexer's on its way to the database: a stretch with no interruption
      // point - a statement in flight, the rest of a message - then the next
      // statement, then the interruption point of the next message.
      class StandInPass
      {
      public:
         // What the pass asks of its indexer on its own thread, once, at the end
         // of its next stretch, as an OnError script the indexer runs would by
         // setting MessageIndexing.Enabled.
         enum Ask
         {
            AskNothing = 0,
            // A Start while it runs, no stop asked.
            AskStart = 1,
            // A Stop and then a Start: off and on again.
            AskStopThenStart = 2
         };

         StandInPass() :
            throws_(false),
            indexer_(nullptr),
            runs_(0),
            asks_made_(0),
            passes_after_ask_(0),
            ask_(AskNothing),
            ask_threw_(false),
            stop_returned_(false),
            statement_after_stop_(false)
         {
         }

         explicit StandInPass(bool throws) :
            throws_(throws),
            indexer_(nullptr),
            runs_(0),
            asks_made_(0),
            passes_after_ask_(0),
            ask_(AskNothing),
            ask_threw_(false),
            stop_returned_(false),
            statement_after_stop_(false)
         {
         }

         // The indexer whose thread this runs on, for the asks.
         void SetIndexer(MessageIndexer *indexer) { indexer_ = indexer; }

         // What the indexer's thread runs, once a thread: the runs are the
         // indexers begun. Counted here and not by thread id, which the system
         // gives the next thread once the last has ended.
         void Run()
         {
            {
               std::lock_guard<std::mutex> guard(lock_);
               runs_++;
            }
            changed_.notify_all();

            if (throws_)
               throw std::runtime_error("MessageIndexerTester's stand-in pass failed on purpose.");

            for (;;)
            {
               std::this_thread::sleep_for(std::chrono::milliseconds(300));

               // The next statement. A Stop that has returned has let the
               // database go (Application::ExitInstance).
               if (stop_returned_.load())
                  statement_after_stop_.store(true);

               AskIndexer_();

               boost::this_thread::interruption_point();

               {
                  std::lock_guard<std::mutex> guard(lock_);
                  passes_after_ask_++;
               }
               changed_.notify_all();
            }
         }

         // Whether so many indexers have begun within the time.
         bool WaitForRuns(int count, std::chrono::seconds within)
         {
            std::unique_lock<std::mutex> guard(lock_);
            return changed_.wait_for(guard, within, [this, count]() { return runs_ >= count; });
         }

         int Runs()
         {
            std::lock_guard<std::mutex> guard(lock_);
            return runs_;
         }

         // Asks it of the indexer from the pass's own thread.
         void Asks(Ask ask) { ask_.store(ask); }

         // Whether so many asks have been made within the time.
         bool WaitForAsks(int count, std::chrono::seconds within)
         {
            std::unique_lock<std::mutex> guard(lock_);
            return changed_.wait_for(guard, within, [this, count]() { return asks_made_ >= count; });
         }

         // Whether the pass has gone past so many interruption points since its
         // last ask, within the time: the indexer went on.
         bool WaitForPassesAfterAsk(int count, std::chrono::seconds within)
         {
            std::unique_lock<std::mutex> guard(lock_);
            return changed_.wait_for(guard, within, [this, count]() { return passes_after_ask_ >= count; });
         }

         bool AskThrew() const { return ask_threw_.load(); }

         void StopReturned() { stop_returned_.store(true); }
         bool StatementAfterStop() const { return statement_after_stop_.load(); }

      private:
         void AskIndexer_()
         {
            const int ask = ask_.exchange(AskNothing);
            if (ask == AskNothing)
               return;

            try
            {
               if (ask == AskStart)
               {
                  indexer_->Start();
               }
               else if (ask == AskStopThenStart)
               {
                  indexer_->Stop();
                  indexer_->Start();
               }
            }
            catch (...)
            {
               ask_threw_.store(true);
            }

            {
               std::lock_guard<std::mutex> guard(lock_);
               asks_made_++;
               passes_after_ask_ = 0;
            }
            changed_.notify_all();
         }

         const bool throws_;
         MessageIndexer *indexer_;
         std::mutex lock_;
         std::condition_variable changed_;
         int runs_;
         int asks_made_;
         int passes_after_ask_;
         std::atomic<int> ask_;
         std::atomic<bool> ask_threw_;
         std::atomic<bool> stop_returned_;
         std::atomic<bool> statement_after_stop_;
      };

      // Long enough for a thread to be made and begin, however slowly the tester
      // runs - under an emulator for another processor, as the aarch64 builds run
      // the testers - and never waited out when it passes.
      const std::chrono::seconds StandInWait(60);
   }

   void
   MessageIndexerTester::Test()
   {
      TestStopWaitsForThePassInFlight_();
      TestAStartAfterAStopIsANewIndexer_();
      TestAStartOnItsOwnThread_();
   }

   void
   MessageIndexerTester::TestStopWaitsForThePassInFlight_()
   {
      StandInPass pass;
      MessageIndexer indexer(boost::bind(&StandInPass::Run, &pass));

      indexer.Start();
      if (!pass.WaitForRuns(1, StandInWait))
         throw std::logic_error("MessageIndexerTester: the indexer's thread began no pass within a minute of Start.");

      indexer.Stop();
      pass.StopReturned();

      // Twice the stretch of a pass: a thread still in one when Stop returned
      // has reached its next statement by then.
      std::this_thread::sleep_for(std::chrono::milliseconds(600));

      if (pass.StatementAfterStop())
         throw std::logic_error("MessageIndexerTester: MessageIndexer::Stop returned while the indexer's thread was in a pass, and the pass went on to a statement after the stop - through the database manager a Reinitialize resets once StopServers returns.");
   }

   void
   MessageIndexerTester::TestAStartAfterAStopIsANewIndexer_()
   {
      StandInPass pass;
      MessageIndexer indexer(boost::bind(&StandInPass::Run, &pass));

      indexer.Start();
      if (!pass.WaitForRuns(1, StandInWait))
         throw std::logic_error("MessageIndexerTester: the indexer's thread began no pass within a minute of Start.");

      // A stop and a start straight after it, as a Reinitialize makes them.
      indexer.Stop();
      indexer.Start();

      if (!pass.WaitForRuns(2, StandInWait))
         throw std::logic_error("MessageIndexerTester: a Start straight after a Stop began no indexer - it took the thread the stop had left ending for the running indexer, and that thread then ended.");

      indexer.Stop();
   }

   void
   MessageIndexerTester::TestAStartOnItsOwnThread_()
   {
      StandInPass pass;
      MessageIndexer indexer(boost::bind(&StandInPass::Run, &pass));
      pass.SetIndexer(&indexer);

      indexer.Start();
      if (!pass.WaitForRuns(1, StandInWait))
         throw std::logic_error("MessageIndexerTester: the indexer's thread began no pass within a minute of Start.");

      // An OnError script the indexer runs, turning MessageIndexing on while it
      // runs: nothing to do.
      pass.Asks(StandInPass::AskStart);
      if (!pass.WaitForAsks(1, StandInWait))
         throw std::logic_error("MessageIndexerTester: the stand-in pass made no Start on its own thread within a minute.");

      if (pass.AskThrew())
         throw std::logic_error("MessageIndexerTester: a Start on the indexer's own thread, while it ran, threw - the thread asked to join itself (boost: \"trying joining itself\"), which an OnError script turning MessageIndexing on met as a COM error, its setting not saved.");

      if (!pass.WaitForPassesAfterAsk(1, StandInWait) || pass.Runs() != 1)
         throw std::logic_error("MessageIndexerTester: after a Start on the indexer's own thread the indexer did not go on as it was.");

      // And one turning it off and on again: the Stop is taken back, and the
      // indexer goes on as the indexer.
      pass.Asks(StandInPass::AskStopThenStart);
      if (!pass.WaitForAsks(2, StandInWait))
         throw std::logic_error("MessageIndexerTester: the stand-in pass made no Stop and Start on its own thread within a minute.");

      if (pass.AskThrew())
         throw std::logic_error("MessageIndexerTester: a Stop and a Start on the indexer's own thread threw.");

      if (!pass.WaitForPassesAfterAsk(2, StandInWait))
         throw std::logic_error("MessageIndexerTester: a Stop and then a Start on the indexer's own thread ended the indexer - the Start did not take the Stop back, and with the setting on no indexer ran.");

      if (pass.Runs() != 1)
         throw std::logic_error("MessageIndexerTester: a Stop and then a Start on the indexer's own thread began a second indexer.");

      indexer.Stop();
   }

   void
   MessageIndexerTester::TestAnExceptionOnItsThread()
   {
      StandInPass pass(true);
      MessageIndexer indexer(boost::bind(&StandInPass::Run, &pass));

      indexer.Start();
      if (!pass.WaitForRuns(1, StandInWait))
         throw std::logic_error("MessageIndexerTester: the indexer's thread began no pass within a minute of Start.");

      // The pass threw. The process is still here to go on; the thread that
      // threw ends, and a Start once it has ended begins another.
      const std::chrono::steady_clock::time_point until = std::chrono::steady_clock::now() + StandInWait;
      while (pass.Runs() < 2 && std::chrono::steady_clock::now() < until)
      {
         indexer.Start();
         std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }

      if (pass.Runs() < 2)
         throw std::logic_error("MessageIndexerTester: after an exception on the indexer's thread, no Start within a minute began the indexer again.");

      indexer.Stop();
   }
}
