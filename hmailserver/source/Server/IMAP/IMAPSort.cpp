// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#include "StdAfx.h"

#include "IMAPSort.h"
#include "IMAPSortParser.h"
#include "IMAPConnection.h"
#include "IMAPAddressList.h"
#include "IMAPThread.h"

#include "../Common/Persistence/PersistentMessageMetaData.h"
#include "../Common/BO/Message.h"
#include "../Common/Mime/Mime.h"
#include "../Common/Util/Time.h"
#include "../Common/Util/VariantDateTime.h"
#include "../Common/Persistence/PersistentMessage.h"
#include "../Common/Persistence/MessageStore.h"

#include <algorithm>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // A date as a key that sorts as text: "YYYY-MM-DD HH:MM:SS", every field
      // zero-padded, or "" for none.
      String DateSortKey_(const DateTime &date)
      {
         if (date.GetStatus() != DateTime::valid)
            return String();

         return Time::GetTimeStampFromDateTime(date);
      }

      // The internal date - INTERNALDATE, "arrival" - as a key.
      String ArrivalSortKey_(std::shared_ptr<Message> message)
      {
         return DateSortKey_(Time::GetDateFromSystemDate(message->GetCreateTime()));
      }

      // RFC 5256 section 2.1 compares with the i;ascii-casemap collation: a-z
      // are folded to A-Z and nothing else is, so "é" and "É" stay distinct
      // whatever the locale's ToUpper would make of them.
      String AsciiCaseMap_(const String &value)
      {
         String folded;

         for (int i = 0; i < value.GetLength(); i++)
         {
            wchar_t c = value.GetAt(i);

            if (c >= 'a' && c <= 'z')
               c = (wchar_t) (c - 'a' + 'A');

            folded += c;
         }

         return folded;
      }

      bool IsAscii_(const String &value)
      {
         for (int i = 0; i < value.GetLength(); i++)
         {
            if (value.GetAt(i) > 0x7F)
               return false;
         }

         return true;
      }

      // Two keys compared. ASCII keys - every date, and most subjects and
      // addresses - by the i;ascii-casemap collation, ordinal once folded. A key
      // beyond ASCII, under any charset but US-ASCII, by the system's own
      // case-insensitive comparison, as SORT has always ordered it: RFC 5256's
      // collation leaves "é" and "É" apart and puts every Greek or Turkish
      // letter after "Z", which reads as no order at all to the people whose
      // letters they are (MessageIndexing.TestMetaDataSortSubjectGreek and
      // Turkish hold it).
      int CompareKeys_(const String &left, const String &right, bool usAscii)
      {
         if (usAscii || (IsAscii_(left) && IsAscii_(right)))
            return left.Compare(right);

#ifdef HM_PLATFORM_POSIX
         return ::wcscasecmp(left.c_str(), right.c_str());
#else
         const int result = CompareStringW(LOCALE_SYSTEM_DEFAULT, NORM_IGNORECASE, left.c_str(), -1, right.c_str(), -1);
         if (result == 0)
            return left.Compare(right);
         return result - CSTR_EQUAL;
#endif
      }

      // One message's keys, one per sort criterion, and the sequence number that
      // breaks the last tie.
      struct SortEntry
      {
         std::pair<int, std::shared_ptr<Message> > message;
         std::vector<String> keys;
      };
   }

   IMAPSort::IMAPSort(void)
   {
   }

   IMAPSort::~IMAPSort(void)
   {
   }

   bool
   IMAPSort::Sort(std::shared_ptr<IMAPConnection> pConnection, std::vector<std::pair<int, std::shared_ptr<Message> > > &vecMessages, String character_set, std::shared_ptr<IMAPSortParser> pParser,
                  const std::function<bool()> &abortRequested)
   {
      // RFC 5256 section 3. Every criterion in the list takes part: the first
      // decides, a tie goes to the second, and so on, and when every criterion
      // ties "the order in which the two messages appear in the mailbox (that is,
      // by sequence number)" decides. REVERSE "only reverses a single criterion,
      // and does not affect the implicit "sequence number" sort criterion".
      //
      // What this replaces sorted by the LAST criterion alone - "(ARRIVAL REVERSE
      // SIZE)" was a size sort - with std::sort, which leaves ties in no defined
      // order, and applied REVERSE by reversing the finished list, which reversed
      // the ties along with it. imaptest's sort tests found all three.
      //
      // The keys are also the RFC's rather than the raw headers': SUBJECT is the
      // base subject (2.1), FROM, TO and CC the mailbox part of the first address
      // - "[IMAP] addr-mailbox of the first "From" address" - not the header text
      // with a leading quote trimmed off, and DATE the sent date with INTERNALDATE
      // standing in when there is none (2.2). Strings compare with a-z folded to
      // A-Z and nothing else (AsciiCaseMap_), which is the i;ascii-casemap
      // collation RFC 5256 prescribes whatever the charset.

      // A criterion for a key named before - REVERSE or not, in any case, or
      // one this server does not know after another it does not know - is left
      // out: wherever it would be asked, the first has already found the two
      // messages equal on that key, so it can only find them equal again, and
      // the order is the same without it. Each used to be kept, a key made of it
      // for every message and compared at every step of the sort: "SORT (SIZE
      // SIZE SIZE ...)" or "(X1 X2 X3 ...)", a hundred thousand of them in one
      // command line, made a hundred thousand keys per message of the mailbox -
      // gigabytes for a few thousand - and the time ceiling is asked only before
      // a header is read, which neither needs.
      std::vector<std::pair<bool,String> > vecSortTypes;
      {
         std::set<SortField> named;

         for (const std::pair<bool, String> &sortType : pParser->GetSortTypes())
         {
            if (named.insert(GetSortField_(sortType.second)).second)
               vecSortTypes.push_back(sortType);
         }
      }

      // The command layer refuses an empty list; this stays because the cost of
      // being certain is one comparison.
      if (vecSortTypes.empty() || vecMessages.empty())
         return true;

      std::vector<SortField> fields;
      std::vector<bool> ascending;

      for (const std::pair<bool, String> &sortType : vecSortTypes)
      {
         fields.push_back(GetSortField_(sortType.second));
         ascending.push_back(sortType.first);
      }

      // With message indexing on, the indexed header values stand in for the
      // file read, as before.
      std::map<SortField, std::map<__int64, String> > databaseMetaData;

      if (Configuration::Instance()->GetMessageIndexing())
      {
         const int accountID = (int) vecMessages[0].second->GetAccountID();
         const int folderID = (int) vecMessages[0].second->GetFolderID();

         for (size_t i = 0; i < fields.size(); i++)
         {
            SortField field = fields[i];

            if (field != From && field != Subject && field != To && field != CC && field != Date)
               continue;

            if (databaseMetaData.find(field) != databaseMetaData.end())
               continue;

            PersistentMessageMetaData messageMetaData;
            messageMetaData.GetMetaData(accountID, folderID, vecSortTypes[i].second, databaseMetaData[field]);
         }
      }

      std::vector<SortEntry> entries;
      entries.reserve(vecMessages.size());

      for (const std::pair<int, std::shared_ptr<Message> > &messagePair : vecMessages)
      {
         SortEntry entry;
         entry.message = messagePair;

         std::shared_ptr<Message> message = messagePair.second;

         // The header, read once for this message and only when a criterion needs
         // what the index does not hold.
         bool headerLoaded = false;
         MimeHeader oHeader;

         for (size_t i = 0; i < fields.size(); i++)
         {
            SortField field = fields[i];
            String key;

            if (field == Arrival)
            {
               key = ArrivalSortKey_(message);
            }
            else if (field == Size)
            {
               // Zero-padded, so the sizes sort as text.
               key.Format(_T("%010d"), message->GetSize());
            }
            else if (field == Unknown)
            {
               // Every message ties on a criterion nobody knows.
            }
            else
            {
               String fieldValue;
               bool fromIndex = false;

               auto indexed = databaseMetaData.find(field);
               if (indexed != databaseMetaData.end())
               {
                  auto value = indexed->second.find(message->GetID());
                  if (value != indexed->second.end())
                  {
                     fieldValue = value->second;
                     fromIndex = true;
                  }
               }

               AnsiString rawValue;

               if (!fromIndex)
               {
                  if (!headerLoaded)
                  {
                     // Asked BEFORE the read, so the caller's ceiling stops the next
                     // file rather than being consulted once every file is read.
                     if (abortRequested && abortRequested())
                        return false;

                     AnsiString sHeader = MessageStore::LoadHeader(pConnection->GetAccountOwningCurrentFolder(), message);
                     oHeader.Load(sHeader, sHeader.GetLength());
                     headerLoaded = true;
                  }

                  const String &fieldName = vecSortTypes[i].second;
                  const char *raw = oHeader.GetRawFieldValue(fieldName);

                  if (raw)
                     rawValue = raw;

                  fieldValue = oHeader.GetUnicodeFieldValue(AnsiString(fieldName));
               }
               else
               {
                  rawValue = AnsiString(fieldValue);
               }

               if (field == Subject)
               {
                  key = IMAPThread::GetBaseSubject(fieldValue);
               }
               else if (field == From || field == To || field == CC)
               {
                  key = String(IMAPAddressList::GetFirstMailbox(rawValue).c_str());
               }
               else if (field == Date)
               {
                  // The index holds the sent date as a date already; a header holds
                  // it as text. Either way INTERNALDATE stands in when it cannot be
                  // read.
                  DateTime sentDate = fromIndex
                     ? Time::GetDateFromSystemDate(fieldValue)
                     : Time::GetDateTimeFromMimeHeader(fieldValue);

                  key = DateSortKey_(sentDate);

                  if (key.IsEmpty() || fieldValue.IsEmpty())
                     key = ArrivalSortKey_(message);
               }

               key = AsciiCaseMap_(key);
            }

            entry.keys.push_back(key);
         }

         entries.push_back(entry);
      }

      const bool usAscii = character_set.CompareNoCase(_T("US-ASCII")) == 0;

      std::stable_sort(entries.begin(), entries.end(),
         [&ascending, usAscii](const SortEntry &left, const SortEntry &right)
         {
            for (size_t i = 0; i < ascending.size(); i++)
            {
               const int compare = CompareKeys_(left.keys[i], right.keys[i], usAscii);

               if (compare == 0)
                  continue;

               return ascending[i] ? compare < 0 : compare > 0;
            }

            return left.message.first < right.message.first;
         });

      for (size_t i = 0; i < entries.size(); i++)
         vecMessages[i] = entries[i].message;

      return true;
   }

   IMAPSort::SortField
   IMAPSort::GetSortField_(AnsiString sHeaderField)
   {
      SortField sortField;

      sHeaderField.MakeUpper();

      if (sHeaderField == "SUBJECT")
         sortField = Subject;
      else if (sHeaderField == "CC")
         sortField = CC;
      else if (sHeaderField == "DATE")
         sortField = Date;
      else if (sHeaderField == "FROM")
         sortField = From;
      else if (sHeaderField == "TO")
         sortField = To;
      else if (sHeaderField == "ARRIVAL")
         sortField = Arrival;
      else if (sHeaderField == "SIZE")
         sortField = Size;
      else
         sortField = Unknown;

      return sortField;
   }
}
