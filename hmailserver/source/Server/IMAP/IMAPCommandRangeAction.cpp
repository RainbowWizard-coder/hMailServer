// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#include "stdafx.h"
#include "IMAPCommandRangeAction.h"
#include "IMAPConnection.h"
#include "IMAPFolderView.h"
#include "../Common/BO/Messages.h"
#include "../Common/BO/Message.h"
#include "../Common/BO/IMAPFolder.h"
#include "../Common/BO/ACLPermission.h"

#include "IMAPStore.h"
#include "IMAPListLookup.h"
#include "../Common/AntiSpam/SpamAssassin/SpamAssassinLearner.h"
#include "../Common/Util/Parsing/StringParser.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <set>
#include <stdexcept>
#include <string>


#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // A UID from a client's set. UIDs are unsigned 32-bit numbers, and one
      // above INT_MAX - "1:4294967295", the whole range, is a set clients send -
      // went through _ttoi, which cannot hold it. Anything past the largest UID
      // there can be is that UID.
      unsigned int ParseSetUid_(const String &text)
      {
         const __int64 value = _ttoi64(text);

         if (value <= 0)
            return 0;

         if (value > (__int64) 0xFFFFFFFFu)
            return 0xFFFFFFFFu;

         return (unsigned int) value;
      }

      // The sequence numbers of a set already taken, as disjoint runs, first to
      // last. The parts of first..last not taken yet are added to parts, in
      // order, and all of it is taken.
      void TakeUntaken_(std::map<int, int> &taken, int first, int last, std::vector<std::pair<int, int> > &parts)
      {
         int cursor = first;
         int mergedFirst = first;
         int mergedLast = last;

         // The run that holds first, or ends just before it, if there is one.
         auto iter = taken.upper_bound(first);
         if (iter != taken.begin())
         {
            auto before = std::prev(iter);
            if ((*before).second >= first - 1)
               iter = before;
         }

         // Every run that overlaps first..last or touches it.
         while (iter != taken.end() && (*iter).first <= last + 1)
         {
            if ((*iter).first > cursor)
               parts.push_back(std::make_pair(cursor, std::min(last, (*iter).first - 1)));

            cursor = std::max(cursor, (*iter).second + 1);
            mergedFirst = std::min(mergedFirst, (*iter).first);
            mergedLast = std::max(mergedLast, (*iter).second);
            iter = taken.erase(iter);
         }

         if (cursor <= last)
            parts.push_back(std::make_pair(cursor, last));

         taken[mergedFirst] = mergedLast;
      }
   }

   IMAPCommandRangeAction::IMAPCommandRangeAction() :
      is_uid_(false),
      uidplus_dest_uidvalidity_(0)
   {

   }

   IMAPCommandRangeAction::~IMAPCommandRangeAction()
   {

   }

   void
   IMAPCommandRangeAction::SetIsUID(bool bIsUID)
   {
      is_uid_ = bIsUID;
   }

   bool
   IMAPCommandRangeAction::GetIsUID()
   {
      return is_uid_;
   }

   IMAPResult
   IMAPCommandRangeAction::DoForMails(std::shared_ptr<IMAPConnection> pConnection, const String &sMailNos, std::shared_ptr<IMAPCommandArgument> pArgument)
   {
      std::shared_ptr<IMAPFolderView> view = pConnection->GetCurrentFolderView();
      std::shared_ptr<IMAPFolder> pCurFolder = pConnection->GetCurrentFolder();

      if (!view || !pCurFolder)
         return IMAPResult(IMAPResult::ResultNo, "No folder selected.");

      // RFC 5182 (SEARCHRES): "$" references the result saved by "SEARCH RETURN (SAVE)".
      // Expanded here, the single chokepoint for FETCH/STORE/COPY/MOVE sequence-sets, so
      // every consumer sees a concrete set. The saved result is stored as UIDs; for a
      // sequence-number command they are mapped to the positions THIS SESSION knows them
      // by. An empty saved result expands to "0", which matches nothing.
      String sExpandedMailNos = sMailNos;
      if (sExpandedMailNos.Find(_T("$")) >= 0)
      {
         const std::vector<__int64> &savedUids = pConnection->GetSavedSearchResult();
         std::vector<String> tokens;

         for (__int64 uid : savedUids)
         {
            if (is_uid_)
            {
               String s;
               s.Format(_T("%I64d"), uid);
               tokens.push_back(s);
               continue;
            }

            int sequence = 0;
            IMAPViewEntry entry;

            if (view->GetEntryByUID((unsigned int) uid, sequence, entry))
            {
               String s;
               s.Format(_T("%d"), sequence);
               tokens.push_back(s);
            }
         }

         String sSubstitution = tokens.empty() ? String(_T("0")) : StringParser::JoinVector(tokens, _T(","));

         // "$" stands where a message number would, as a whole element of the
         // set (RFC 5182: seq-last-command = "$"), and it stands in once: the set
         // is a set, and ResolveTargets_ takes each message once. It used to be
         // replaced as text wherever it stood, once per "$", so "$,$,$,..." -
         // a hundred thousand of them in one command line - copied the saved
         // result a hundred thousand times, gigabytes for a saved result of a
         // few thousand messages, before a message was looked at. An element
         // that holds a "$" and is not one - "1:$", "$$" - is no set at all.
         if (!IMAPListLookup::ExpandSavedResult(sMailNos, sSubstitution, sExpandedMailNos))
            return IMAPResult(IMAPResult::ResultBad, "Invalid message set: \"$\" stands alone for the saved result.");
      }

      // Resolved against this session's view, so the numbers mean the messages they meant
      // when the client was told about them, whatever other sessions have done to the
      // folder since (RFC 3501 2.3.1.2; upstream #458).
      std::vector<std::pair<int, IMAPViewEntry> > targets = ResolveTargets_(view, sExpandedMailNos);

      if (targets.empty())
         return IMAPResult();

      std::set<__int64> message_ids;
      for (const std::pair<int, IMAPViewEntry> &target : targets)
         message_ids.insert(target.second.message_id);

      std::shared_ptr<Messages> messages = pCurFolder->GetMessages();

      std::map<__int64, std::shared_ptr<Message> > resolved = UsesLiveMessages()
         ? messages->GetItemsByIds(message_ids)
         : messages->GetCopyByIds(message_ids);

      // A message in this session's view but no longer in the folder was expunged by
      // another session, and this client has not been told - it is told the next time an
      // untagged EXPUNGE may be sent. What the command does about it depends on how it
      // was addressed: the UID variants ignore it (RFC 3501 6.4.8), the others by policy.
      MissingMessagePolicy policy = is_uid_ ? MissingMessagePolicy::Ignore : GetMissingMessagePolicy();

      bool any_missing = false;

      for (const std::pair<int, IMAPViewEntry> &target : targets)
      {
         if (resolved.find(target.second.message_id) != resolved.end())
            continue;

         view->MarkVanished(target.second.message_id);
         any_missing = true;
      }

      if (any_missing && policy == MissingMessagePolicy::FailBeforeActing)
         return IMAPResult(IMAPResult::ResultNo, "[EXPUNGEISSUED] Some of the messages no longer exist.");

      for (const std::pair<int, IMAPViewEntry> &target : targets)
      {
         auto iter = resolved.find(target.second.message_id);

         if (iter == resolved.end())
            continue;

         IMAPResult result = DoAction(pConnection, target.first, (*iter).second, pArgument);

         if (result.GetResult() != IMAPResult::ResultOK)
            return result;
      }

      if (any_missing && policy == MissingMessagePolicy::ReportAfterActing)
         return IMAPResult(IMAPResult::ResultNo, "[EXPUNGEISSUED] Some of the messages no longer exist.");

      return IMAPResult();
   }

   std::vector<std::pair<int, IMAPViewEntry> >
   IMAPCommandRangeAction::ResolveTargets_(std::shared_ptr<IMAPFolderView> view, const String &sMailNos)
   {
      std::vector<std::pair<int, IMAPViewEntry> > targets;

      const unsigned int highestUid = view->GetHighestUID();
      const int messageCount = view->GetMessageCount();

      std::vector<String> sSplitted = StringParser::SplitString(sMailNos, ",");

      // Each message once, in the order the set first names it, the set being a
      // set: "1:*,1:*" is every message, not every message twice. Each element
      // used to put every message it names into targets, so a set of a few
      // hundred thousand "1:*" over a mailbox of thousands made billions of
      // them - memory the server ran out of - and FETCH answered, STORE stored
      // and COPY copied each message as many times as it was named. Each
      // element is now a run of sequence numbers, and only what no earlier
      // element took is taken, a step that costs what the set's length costs.
      std::map<int, int> taken;
      std::vector<std::pair<int, int> > runs;

      for (String sCur : sSplitted)
      {
         long lColonPos = sCur.Find(_T(":"));

         String sFirstPart = lColonPos >= 0 ? sCur.Mid(0, lColonPos) : sCur;
         String sSecondPart = lColonPos >= 0 ? sCur.Mid(lColonPos + 1) : sCur;

         bool firstIsStar = sFirstPart == _T("*");
         bool secondIsStar = sSecondPart == _T("*");

         if (is_uid_)
         {
            // RFC 3501: "*" is the largest UID in use - on either side of the colon, and
            // a range is valid in either order, so "*:1" is the whole mailbox and a bare
            // "*" is the last message. An empty mailbox has no largest UID, so "*" must
            // match nothing there.
            //
            // It is the largest UID on the RIGHT of the colon too. It used to be left
            // unbounded there, which made "<uidnext>:*" the empty range from UIDNEXT
            // upwards - but RFC 3501 6.4.8 is explicit that "a UID range of 559:*
            // always includes the UID of the last message in the mailbox, even if 559
            // is higher than any assigned UID value", and clients rely on it: that is
            // how they ask for "anything new" and learn the last UID when nothing is.
            // The view holds exactly the messages this session knows, and highestUid
            // is the view's own, so no message the view took in is left out by it.
            if ((firstIsStar || secondIsStar) && highestUid == 0)
               continue;

            if (lColonPos >= 0)
            {
               unsigned int startUid = firstIsStar ? highestUid : ParseSetUid_(sFirstPart);
               unsigned int endUid = secondIsStar ? highestUid : ParseSetUid_(sSecondPart);

               if (endUid < startUid)
                  std::swap(startUid, endUid);

               int firstSequence = 0;
               int lastSequence = -1;

               if (view->GetSequenceRangeByUIDRange(startUid, endUid, firstSequence, lastSequence))
                  TakeUntaken_(taken, firstSequence, lastSequence, runs);
            }
            else
            {
               unsigned int uid = firstIsStar ? highestUid : ParseSetUid_(sCur);

               if (uid == 0)
                  continue;

               int sequence = 0;
               IMAPViewEntry entry;

               if (view->GetEntryByUID(uid, sequence, entry))
                  TakeUntaken_(taken, sequence, sequence, runs);
            }
         }
         else
         {
            // Sequence numbers: "*" is the highest one in this session's view, on either
            // side of the colon, and a range is valid in either order.
            if (lColonPos >= 0)
            {
               int startIndex = firstIsStar ? messageCount : _ttoi(sFirstPart);
               int endIndex = secondIsStar ? messageCount : _ttoi(sSecondPart);

               if (endIndex < startIndex)
                  std::swap(startIndex, endIndex);

               // What GetEntriesBySequenceRange makes of the two.
               if (startIndex < 1)
                  startIndex = 1;

               if (endIndex < 0 || endIndex > messageCount)
                  endIndex = messageCount;

               if (startIndex <= endIndex)
                  TakeUntaken_(taken, startIndex, endIndex, runs);
            }
            else
            {
               int sequence = firstIsStar ? messageCount : _ttoi(sCur);

               if (sequence <= 0)
                  continue;

               IMAPViewEntry entry;

               if (view->GetEntryBySequence(sequence, entry))
                  TakeUntaken_(taken, sequence, sequence, runs);
            }
         }
      }

      for (const std::pair<int, int> &run : runs)
      {
         for (const std::pair<int, IMAPViewEntry> &entry : view->GetEntriesBySequenceRange(run.first, run.second))
            targets.push_back(entry);
      }

      return targets;
   }

   void
   IMAPCommandRangeAction::RecordCopyUid(unsigned int sourceUid, unsigned int destUid, unsigned int destUidValidity)
   {
      uidplus_source_uids_.push_back(sourceUid);
      uidplus_dest_uids_.push_back(destUid);
      uidplus_dest_uidvalidity_ = destUidValidity;
   }

   void
   IMAPCommandRangeAction::RecordCopyUidIfReadable(std::shared_ptr<IMAPConnection> pConnection, std::shared_ptr<IMAPFolder> folder, unsigned int sourceUid, unsigned int destUid)
   {
      if (!pConnection->CheckPermission(folder, ACLPermission::PermissionRead))
         return;

      RecordCopyUid(sourceUid, destUid, (unsigned int) folder->GetCreationTime().ToInt());
   }

   void
   IMAPCommandRangeAction::ForgetCopyUids()
   {
      uidplus_source_uids_.clear();
      uidplus_dest_uids_.clear();
      uidplus_dest_uidvalidity_ = 0;
   }

   void
   IMAPCommandRangeAction::HoldLesson(std::shared_ptr<Message> copy, std::shared_ptr<const Account> destinationOwner, bool spam)
   {
      Lesson_ lesson;
      lesson.copy = copy;
      lesson.destination_owner = destinationOwner;
      lesson.spam = spam;
      lessons_.push_back(lesson);
   }

   void
   IMAPCommandRangeAction::GiveLessons()
   {
      for (const Lesson_ &lesson : lessons_)
         SpamAssassinLearner::Teach(lesson.copy, lesson.destination_owner, lesson.spam);

      lessons_.clear();
   }

   void
   IMAPCommandRangeAction::ForgetLessons()
   {
      lessons_.clear();
   }

   void
   IMAPCommandRangeAction::KeepOnlySettableFlags(std::shared_ptr<IMAPConnection> pConnection, std::shared_ptr<IMAPFolder> folder, std::shared_ptr<Message> message)
   {
      if (!pConnection->CheckPermission(folder, ACLPermission::PermissionWriteSeen))
         message->SetFlagSeen(false);

      if (!pConnection->CheckPermission(folder, ACLPermission::PermissionWriteDeleted))
         message->SetFlagDeleted(false);

      if (!pConnection->CheckPermission(folder, ACLPermission::PermissionWriteOthers))
      {
         message->SetFlagDraft(false);
         message->SetFlagAnswered(false);
         message->SetFlagFlagged(false);
         message->SetKeywords(String());
      }
   }

   String
   IMAPCommandRangeAction::JoinUids_(const std::vector<unsigned int> &uids)
   {
      String result;

      for (size_t i = 0; i < uids.size(); i++)
      {
         if (i > 0)
            result += _T(",");

         String temp;
         temp.Format(_T("%u"), uids[i]);
         result += temp;
      }

      return result;
   }

   String
   IMAPCommandRangeAction::GetUIDPlusResponseCode()
   {
      if (uidplus_source_uids_.empty() || uidplus_dest_uids_.empty())
         return _T("");

      String validity;
      validity.Format(_T("%u"), uidplus_dest_uidvalidity_);

      String result = _T("[COPYUID ") + validity + _T(" ") + JoinUids_(uidplus_source_uids_) +
                      _T(" ") + JoinUids_(uidplus_dest_uids_) + _T("] ");

      return result;
   }

   namespace
   {
      // The view's lookups as 6.3.5 made them, by reading every entry: what the
      // ones by halving are held to.
      bool OldEntryByUID_(const std::vector<std::pair<int, IMAPViewEntry> > &all, unsigned int uid, int &sequence, IMAPViewEntry &entry)
      {
         for (const std::pair<int, IMAPViewEntry> &one : all)
         {
            if (one.second.uid != uid)
               continue;

            sequence = one.first;
            entry = one.second;
            return true;
         }

         sequence = 0;
         return false;
      }

      std::vector<std::pair<int, IMAPViewEntry> > OldEntriesByUIDRange_(const std::vector<std::pair<int, IMAPViewEntry> > &all, unsigned int first, unsigned int last)
      {
         std::vector<std::pair<int, IMAPViewEntry> > result;

         for (const std::pair<int, IMAPViewEntry> &one : all)
         {
            if (one.second.uid < first || one.second.uid > last)
               continue;

            result.push_back(one);
         }

         return result;
      }

      unsigned int OldParseSetUid_(const String &text)
      {
         const __int64 value = _ttoi64(text);

         if (value <= 0)
            return 0;

         if (value > (__int64) 0xFFFFFFFFu)
            return 0xFFFFFFFFu;

         return (unsigned int) value;
      }

      // ResolveTargets_ as 6.3.5 had it, word for word but for the lookups
      // above, and then each message once, where the set first names it: the
      // targets the one that takes runs is held to.
      std::vector<std::pair<int, IMAPViewEntry> > OldResolveTargets_(std::shared_ptr<IMAPFolderView> view, const String &sMailNos, bool is_uid_)
      {
         std::vector<std::pair<int, IMAPViewEntry> > targets;
         const std::vector<std::pair<int, IMAPViewEntry> > all = view->GetAllEntries();

         const unsigned int highestUid = view->GetHighestUID();
         const int messageCount = view->GetMessageCount();

         std::vector<String> sSplitted = StringParser::SplitString(sMailNos, ",");

         for (String sCur : sSplitted)
         {
            long lColonPos = sCur.Find(_T(":"));

            String sFirstPart = lColonPos >= 0 ? sCur.Mid(0, lColonPos) : sCur;
            String sSecondPart = lColonPos >= 0 ? sCur.Mid(lColonPos + 1) : sCur;

            bool firstIsStar = sFirstPart == _T("*");
            bool secondIsStar = sSecondPart == _T("*");

            if (is_uid_)
            {
               if ((firstIsStar || secondIsStar) && highestUid == 0)
                  continue;

               if (lColonPos >= 0)
               {
                  unsigned int startUid = firstIsStar ? highestUid : OldParseSetUid_(sFirstPart);
                  unsigned int endUid = secondIsStar ? highestUid : OldParseSetUid_(sSecondPart);

                  if (endUid < startUid)
                     std::swap(startUid, endUid);

                  for (const std::pair<int, IMAPViewEntry> &entry : OldEntriesByUIDRange_(all, startUid, endUid))
                     targets.push_back(entry);
               }
               else
               {
                  unsigned int uid = firstIsStar ? highestUid : OldParseSetUid_(sCur);

                  if (uid == 0)
                     continue;

                  int sequence = 0;
                  IMAPViewEntry entry;

                  if (OldEntryByUID_(all, uid, sequence, entry))
                     targets.push_back(std::make_pair(sequence, entry));
               }
            }
            else
            {
               if (lColonPos >= 0)
               {
                  int startIndex = firstIsStar ? messageCount : _ttoi(sFirstPart);
                  int endIndex = secondIsStar ? messageCount : _ttoi(sSecondPart);

                  if (endIndex < startIndex)
                     std::swap(startIndex, endIndex);

                  for (const std::pair<int, IMAPViewEntry> &entry : view->GetEntriesBySequenceRange(startIndex, endIndex))
                     targets.push_back(entry);
               }
               else
               {
                  int sequence = firstIsStar ? messageCount : _ttoi(sCur);

                  if (sequence <= 0)
                     continue;

                  IMAPViewEntry entry;

                  if (view->GetEntryBySequence(sequence, entry))
                     targets.push_back(std::make_pair(sequence, entry));
               }
            }
         }

         std::vector<std::pair<int, IMAPViewEntry> > once;
         std::set<int> seen;
         for (const std::pair<int, IMAPViewEntry> &target : targets)
         {
            if (seen.insert(target.first).second)
               once.push_back(target);
         }

         return once;
      }

      bool SameTargets_(const std::vector<std::pair<int, IMAPViewEntry> > &a, const std::vector<std::pair<int, IMAPViewEntry> > &b)
      {
         if (a.size() != b.size())
            return false;

         for (size_t i = 0; i < a.size(); i++)
         {
            if (a[i].first != b[i].first || a[i].second.message_id != b[i].second.message_id || a[i].second.uid != b[i].second.uid)
               return false;
         }

         return true;
      }

      // xorshift64*: the same cases on every run and every platform.
      unsigned long long NextSetRandom_(unsigned long long &state)
      {
         state ^= state >> 12;
         state ^= state << 25;
         state ^= state >> 27;
         return state * 2685821657736338717ULL;
      }

      std::shared_ptr<IMAPFolderView> MakeView_(const std::vector<unsigned int> &uids)
      {
         std::shared_ptr<Messages> messages = std::shared_ptr<Messages>(new Messages(1, 1));

         for (size_t i = 0; i < uids.size(); i++)
         {
            std::shared_ptr<Message> message = std::shared_ptr<Message>(new Message(false));
            message->SetID(1000 + (__int64) i);
            message->SetUID(uids[i]);
            messages->AddItem(message);
         }

         std::shared_ptr<IMAPFolderView> view = std::shared_ptr<IMAPFolderView>(new IMAPFolderView(1, 1));
         view->Initialize(messages);
         return view;
      }

      String SetNumber_(unsigned long long &state, unsigned int largest)
      {
         switch (NextSetRandom_(state) % 12)
         {
         case 0: return _T("*");
         case 1: return _T("4294967295");
         case 2: return _T("99999999999");
         case 3: return _T("0");
         default: break;
         }

         String number;
         number.Format(_T("%u"), (unsigned int) (NextSetRandom_(state) % (largest + 4)));
         return number;
      }
   }

   void
   IMAPSequenceSetTester::Test(int cases)
   {
      // The time first: 50,000 UIDs named one at a time over a view of 50,000
      // messages, each found by halving. 6.3.5 read the whole view for each.
      {
         std::vector<unsigned int> uids;
         for (unsigned int uid = 1; uid <= 50000; uid++)
            uids.push_back(uid * 2);

         std::shared_ptr<IMAPFolderView> view = MakeView_(uids);

         String set;
         for (unsigned int uid = 1; uid <= 50000; uid++)
         {
            String element;
            if (uid % 2 == 0)
               element.Format(_T("%u:%u,"), uid * 2, uid * 2);
            else
               element.Format(_T("%u,"), uid * 2);
            set += element;
         }
         set += _T("1:*,1:*,1:*");

         IMAPStore store;
         store.SetIsUID(true);
         IMAPCommandRangeAction &action = store;

         std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
         std::vector<std::pair<int, IMAPViewEntry> > targets = action.ResolveTargets_(view, set);
         const double milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();

         if (milliseconds > HM_TEST_TIME_BOUND(2000))
            throw std::logic_error("A UID set of 50,000 elements over 50,000 messages took " + std::to_string((long long) milliseconds) + " ms");

         if (targets.size() != 50000 || targets[0].first != 1 || targets[49999].second.uid != 100000)
            HM_TEST_FAILED();
      }

      // Then generated views and sets, by UID and by sequence number: the
      // targets 6.3.5 resolved, each message once.
      unsigned long long state = 0xBB67AE8584CAA73BULL;

      for (int number = 0; number < cases; number++)
      {
         std::vector<unsigned int> uids;
         const int count = (int) (NextSetRandom_(state) % 26);
         unsigned int uid = (unsigned int) (NextSetRandom_(state) % 3);
         for (int i = 0; i < count; i++)
         {
            uid += 1 + (unsigned int) (NextSetRandom_(state) % 3);
            uids.push_back(uid);
         }

         std::shared_ptr<IMAPFolderView> view = MakeView_(uids);
         const std::vector<std::pair<int, IMAPViewEntry> > all = view->GetAllEntries();
         const bool byUid = NextSetRandom_(state) % 2 == 0;
         const unsigned int largest = byUid ? uid : (unsigned int) count;

         // The view's lookups, against reading every entry.
         for (int probe = 0; probe < 4; probe++)
         {
            const unsigned int first = (unsigned int) (NextSetRandom_(state) % (largest + 4));
            const unsigned int last = (unsigned int) (NextSetRandom_(state) % (largest + 4));

            int sequence = -5;
            int oldSequence = -5;
            IMAPViewEntry entry;
            IMAPViewEntry oldEntry;
            const bool found = view->GetEntryByUID(first, sequence, entry);
            const bool oldFound = OldEntryByUID_(all, first, oldSequence, oldEntry);

            if (found != oldFound || sequence != oldSequence || (found && entry.message_id != oldEntry.message_id))
               throw std::logic_error("GetEntryByUID answered other than 6.3.5's in case " + std::to_string(number));

            if (!SameTargets_(view->GetEntriesByUIDRange(first, last), OldEntriesByUIDRange_(all, first, last)))
               throw std::logic_error("GetEntriesByUIDRange answered other than 6.3.5's in case " + std::to_string(number));
         }

         String set;
         const int elements = 1 + (int) (NextSetRandom_(state) % 8);
         for (int i = 0; i < elements; i++)
         {
            if (i > 0)
               set += _T(",");

            set += SetNumber_(state, largest);

            if (NextSetRandom_(state) % 2 == 0)
            {
               set += _T(":");
               set += SetNumber_(state, largest);
            }
         }

         IMAPStore store;
         store.SetIsUID(byUid);
         IMAPCommandRangeAction &action = store;

         if (!SameTargets_(action.ResolveTargets_(view, set), OldResolveTargets_(view, set, byUid)))
            throw std::logic_error("The messages of a set were other than 6.3.5's, each once, in case " + std::to_string(number));
      }

      // A SEARCH key's set, read once into runs (IMAPListLookup::ToRuns): the time
      // for 100,000 elements asked about 50,000 messages, which IsItemInList
      // answered by reading the whole set for each...
      {
         std::vector<String> elements;
         for (int i = 0; i < 100000; i++)
         {
            String element;
            if (i % 3 == 0)
               element.Format(_T("%d:%d"), i * 7 + 3, i * 7 + 1);
            else
               element.Format(_T("%d"), i * 7);
            elements.push_back(element);
         }

         std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
         const IMAPListLookup::Runs runs = IMAPListLookup::ToRuns(elements, 50000);
         int found = 0;
         for (__int64 item = 1; item <= 50000; item++)
         {
            if (IMAPListLookup::IsItemInRuns(runs, item))
               found++;
         }
         const double milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();

         int expected = 0;
         for (__int64 item = 1; item <= 50000; item++)
         {
            // 7i alone where i is not a multiple of 3; 7i+1 to 7i+3 where it is.
            if ((item % 7 == 0 && item % 21 != 0) || (item % 21 >= 1 && item % 21 <= 3))
               expected++;
         }

         if (found != expected)
            HM_TEST_FAILED();

         if (milliseconds > HM_TEST_TIME_BOUND(2000))
            throw std::logic_error("A set of 100,000 elements asked about 50,000 messages took " + std::to_string((long long) milliseconds) + " ms");
      }

      // ...and what it answers, against IsItemInList, over generated sets that
      // hold what a client can send: "*", reversed ranges, 0, numbers past 32
      // bits, a minus sign, letters, empty elements.
      for (int number = 0; number < cases; number++)
      {
         const unsigned int largest = (unsigned int) (NextSetRandom_(state) % 30);
         const __int64 maxItem = (__int64) largest;

         std::vector<String> elements;
         const int count = (int) (NextSetRandom_(state) % 10);
         for (int i = 0; i < count; i++)
         {
            String element;
            switch (NextSetRandom_(state) % 10)
            {
            case 0: element = _T(""); break;
            case 1: element = _T("-3"); break;
            case 2: element = _T("x7"); break;
            default:
               element = SetNumber_(state, largest);
               if (NextSetRandom_(state) % 2 == 0)
               {
                  element += _T(":");
                  element += SetNumber_(state, largest);
               }
               break;
            }
            elements.push_back(element);
         }

         const IMAPListLookup::Runs runs = IMAPListLookup::ToRuns(elements, maxItem);

         static const __int64 extremes[] = { -4, -3, 0, 4294967295LL, 99999999999LL, 4294967296LL };
         for (__int64 item = -1; item <= (__int64) largest + 5; item++)
         {
            if (IMAPListLookup::IsItemInRuns(runs, item) != IMAPListLookup::IsItemInList(elements, item, maxItem))
               throw std::logic_error("A SEARCH set's runs answered other than its elements in case " + std::to_string(number));
         }
         for (__int64 item : extremes)
         {
            if (IMAPListLookup::IsItemInRuns(runs, item) != IMAPListLookup::IsItemInList(elements, item, maxItem))
               throw std::logic_error("A SEARCH set's runs answered other than its elements in case " + std::to_string(number));
         }

         // A UID set's ranges, merged, against reading every range.
         IMAPListLookup::UidRanges ranges;
         const int rangeCount = (int) (NextSetRandom_(state) % 8);
         for (int i = 0; i < rangeCount; i++)
         {
            unsigned int first = (unsigned int) (NextSetRandom_(state) % (largest + 4));
            unsigned int last = NextSetRandom_(state) % 6 == 0 ? 0xFFFFFFFFu : (unsigned int) (NextSetRandom_(state) % (largest + 4));
            if (last < first)
               std::swap(first, last);
            ranges.push_back(std::make_pair(first, last));
         }

         const IMAPListLookup::UidRanges merged = IMAPListLookup::MergeUidRanges(ranges);
         for (unsigned int uid = 0; uid <= largest + 5; uid++)
         {
            bool inAny = false;
            for (const std::pair<unsigned int, unsigned int> &range : ranges)
               inAny = inAny || (uid >= range.first && uid <= range.second);

            if (IMAPListLookup::IsInUidRanges(merged, uid) != inAny)
               throw std::logic_error("A UID set's merged ranges answered other than its ranges in case " + std::to_string(number));
         }
      }

      // "$" stands in for the saved result once, where it stands alone.
      {
         struct Expansion
         {
            const wchar_t *set;
            bool valid;
            const wchar_t *expanded;
         };

         static const Expansion expansions[] =
         {
            { L"$", true, L"1,4:6" },
            { L"$,$,$", true, L"1,4:6" },
            { L"3,$,1,$", true, L"3,1,4:6,1" },
            { L"2:3", true, L"2:3" },
            { L"1:$", false, L"" },
            { L"$$", false, L"" },
            { L"$:*", false, L"" },
         };

         for (const Expansion &one : expansions)
         {
            String expanded = _T("untouched");
            const bool valid = IMAPListLookup::ExpandSavedResult(one.set, _T("1,4:6"), expanded);

            if (valid != one.valid || (valid && expanded != one.expanded) || (!valid && expanded != _T("untouched")))
               throw std::logic_error("\"$\" was expanded other than once, where it stands alone");
         }

         // A hundred thousand "$" over a saved result of 5,000: one copy of it.
         String set;
         for (int i = 0; i < 100000; i++)
            set += i == 0 ? _T("$") : _T(",$");

         std::vector<String> saved;
         for (int i = 1; i <= 5000; i++)
         {
            String uid;
            uid.Format(_T("%d"), i * 2);
            saved.push_back(uid);
         }
         const String substitution = StringParser::JoinVector(saved, _T(","));

         std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
         String expanded;
         if (!IMAPListLookup::ExpandSavedResult(set, substitution, expanded) || expanded != substitution)
            HM_TEST_FAILED();
         const double milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();

         if (milliseconds > HM_TEST_TIME_BOUND(2000))
            throw std::logic_error("A set of 100,000 \"$\" took " + std::to_string((long long) milliseconds) + " ms to expand");
      }
   }
}
