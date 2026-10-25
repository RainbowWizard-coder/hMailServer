// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#include "stdafx.h"

#include "IMAPFolderView.h"

#include <algorithm>

#include "../Common/BO/Message.h"
#include "../Common/BO/Messages.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   IMAPFolderView::IMAPFolderView(__int64 account_id, __int64 folder_id) :
      highest_uid_seen_(0),
      account_id_(account_id),
      folder_id_(folder_id)
   {

   }

   void
   IMAPFolderView::Initialize(std::shared_ptr<Messages> messages)
   {
      boost::lock_guard<boost::recursive_mutex> guard(mutex_);

      entries_.clear();
      offset_by_message_id_.clear();
      vanished_.clear();
      highest_uid_seen_ = 0;

      AppendNewMessages(messages);
   }

   int
   IMAPFolderView::AppendNewMessages(std::shared_ptr<Messages> messages)
   {
      if (!messages)
         return 0;

      // GetSnapshot copies the pointers under the collection lock. GetCopy would clone
      // every message object, which is a lot of work for data that is not kept.
      std::vector<std::shared_ptr<Message> > snapshot = messages->GetSnapshot();

      boost::lock_guard<boost::recursive_mutex> guard(mutex_);

      int added = 0;

      /*
         Messages::Refresh only appends rows, ordered by UID ascending, so anything with
         a UID above the highest one seen is new and belongs at the end. If Refresh ever
         starts reordering or reloading the collection, this no longer holds - and a
         message whose UID is below the highest seen (a folder whose UID counter was
         reset by the repair tool, say) is not shown until the folder is selected again.
      */
      for (std::shared_ptr<Message> message : snapshot)
      {
         if (!message)
            continue;

         unsigned int uid = message->GetUID();

         if (uid <= highest_uid_seen_)
            continue;

         entries_.push_back(IMAPViewEntry(message->GetID(), uid));
         offset_by_message_id_[message->GetID()] = entries_.size() - 1;

         highest_uid_seen_ = uid;
         added++;
      }

      return added;
   }

   int
   IMAPFolderView::GetMessageCount() const
   {
      boost::lock_guard<boost::recursive_mutex> guard(mutex_);

      return (int) entries_.size();
   }

   unsigned int
   IMAPFolderView::GetHighestUID() const
   {
      boost::lock_guard<boost::recursive_mutex> guard(mutex_);

      return entries_.empty() ? 0 : entries_.back().uid;
   }

   bool
   IMAPFolderView::GetEntryBySequence(int sequence, IMAPViewEntry &entry) const
   {
      boost::lock_guard<boost::recursive_mutex> guard(mutex_);

      if (sequence < 1 || sequence > (int) entries_.size())
         return false;

      entry = entries_[sequence - 1];

      return true;
   }

   namespace
   {
      // entries_ is ordered by UID ascending and holds each UID once: a message
      // goes in at the end only with a UID above every one before it
      // (AppendNewMessages), Initialize starts it again empty, and
      // RemoveMessages keeps the order of what remains. So the messages of a
      // range of UIDs are one run of it, found by halving. They were found by
      // reading the whole view, for each UID and each range of a client's set:
      // "UID FETCH 1,2,3,..." of a few hundred thousand UIDs over a large
      // mailbox took a time that grew with the product of the two.
      bool UidBelow_(const IMAPViewEntry &entry, unsigned int uid)
      {
         return entry.uid < uid;
      }

      bool UidAbove_(unsigned int uid, const IMAPViewEntry &entry)
      {
         return uid < entry.uid;
      }
   }

   bool
   IMAPFolderView::GetEntryByUID(unsigned int uid, int &sequence, IMAPViewEntry &entry) const
   {
      boost::lock_guard<boost::recursive_mutex> guard(mutex_);

      auto iter = std::lower_bound(entries_.begin(), entries_.end(), uid, UidBelow_);

      if (iter == entries_.end() || (*iter).uid != uid)
      {
         sequence = 0;
         return false;
      }

      sequence = (int) (iter - entries_.begin()) + 1;
      entry = *iter;
      return true;
   }

   bool
   IMAPFolderView::GetSequenceRangeByUIDRange(unsigned int first, unsigned int last, int &firstSequence, int &lastSequence) const
   {
      boost::lock_guard<boost::recursive_mutex> guard(mutex_);

      auto begin = std::lower_bound(entries_.begin(), entries_.end(), first, UidBelow_);
      auto end = std::upper_bound(entries_.begin(), entries_.end(), last, UidAbove_);

      if (begin >= end)
      {
         firstSequence = 0;
         lastSequence = -1;
         return false;
      }

      firstSequence = (int) (begin - entries_.begin()) + 1;
      lastSequence = (int) (end - entries_.begin());
      return true;
   }

   bool
   IMAPFolderView::GetSequenceByMessageID(__int64 message_id, int &sequence) const
   {
      boost::lock_guard<boost::recursive_mutex> guard(mutex_);

      auto iter = offset_by_message_id_.find(message_id);

      if (iter == offset_by_message_id_.end())
      {
         sequence = 0;
         return false;
      }

      sequence = (int) (*iter).second + 1;

      return true;
   }

   std::vector<std::pair<int, IMAPViewEntry> >
   IMAPFolderView::GetEntriesBySequenceRange(int first, int last) const
   {
      boost::lock_guard<boost::recursive_mutex> guard(mutex_);

      std::vector<std::pair<int, IMAPViewEntry> > result;

      if (first < 1)
         first = 1;

      if (last < 0 || last > (int) entries_.size())
         last = (int) entries_.size();

      for (int sequence = first; sequence <= last; sequence++)
         result.push_back(std::make_pair(sequence, entries_[sequence - 1]));

      return result;
   }

   std::vector<std::pair<int, IMAPViewEntry> >
   IMAPFolderView::GetEntriesByUIDRange(unsigned int first, unsigned int last) const
   {
      boost::lock_guard<boost::recursive_mutex> guard(mutex_);

      std::vector<std::pair<int, IMAPViewEntry> > result;

      int firstSequence = 0;
      int lastSequence = -1;

      if (!GetSequenceRangeByUIDRange(first, last, firstSequence, lastSequence))
         return result;

      for (int sequence = firstSequence; sequence <= lastSequence; sequence++)
         result.push_back(std::make_pair(sequence, entries_[sequence - 1]));

      return result;
   }

   std::vector<std::pair<int, IMAPViewEntry> >
   IMAPFolderView::GetAllEntries() const
   {
      boost::lock_guard<boost::recursive_mutex> guard(mutex_);

      std::vector<std::pair<int, IMAPViewEntry> > result;

      for (size_t i = 0; i < entries_.size(); i++)
         result.push_back(std::make_pair((int) i + 1, entries_[i]));

      return result;
   }

   std::vector<int>
   IMAPFolderView::RemoveMessages(const std::vector<__int64> &message_ids, std::vector<unsigned int> *removed_uids)
   {
      boost::lock_guard<boost::recursive_mutex> guard(mutex_);

      std::vector<int> sequences;
      std::set<__int64> removed;

      // Collect the numbers before removing anything, so they all refer to the same view.
      for (__int64 message_id : message_ids)
      {
         auto iter = offset_by_message_id_.find(message_id);

         if (iter == offset_by_message_id_.end())
         {
            // The client was never told about this message, so there is nothing to
            // expunge - or it is named twice.
            continue;
         }

         if (!removed.insert(message_id).second)
            continue;

         sequences.push_back((int) (*iter).second + 1);
      }

      if (removed.empty())
         return sequences;

      // Every EXPUNGE shifts the messages after it down one step, so a client applying
      // the responses in order needs each number adjusted for the removals reported
      // before it. Sorted first, which is also the order the UIDs are handed back in.
      std::sort(sequences.begin(), sequences.end());

      if (removed_uids)
      {
         removed_uids->clear();

         for (int sequence : sequences)
            removed_uids->push_back(entries_[sequence - 1].uid);
      }

      for (size_t i = 0; i < sequences.size(); i++)
         sequences[i] -= (int) i;

      std::vector<IMAPViewEntry> remaining;

      for (const IMAPViewEntry &entry : entries_)
      {
         if (removed.find(entry.message_id) == removed.end())
            remaining.push_back(entry);
      }

      entries_ = remaining;

      for (__int64 message_id : removed)
         vanished_.erase(message_id);

      Rebuild_();

      return sequences;
   }

   void
   IMAPFolderView::MarkVanished(__int64 message_id)
   {
      boost::lock_guard<boost::recursive_mutex> guard(mutex_);

      if (offset_by_message_id_.find(message_id) == offset_by_message_id_.end())
         return;

      vanished_.insert(message_id);
   }

   std::vector<__int64>
   IMAPFolderView::TakeVanished()
   {
      boost::lock_guard<boost::recursive_mutex> guard(mutex_);

      std::vector<__int64> result(vanished_.begin(), vanished_.end());

      vanished_.clear();

      return result;
   }

   void
   IMAPFolderView::Rebuild_()
   {
      offset_by_message_id_.clear();

      for (size_t i = 0; i < entries_.size(); i++)
         offset_by_message_id_[entries_[i].message_id] = i;
   }
}
