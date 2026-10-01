// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Offline in the webmail, Gmail's offline mail: the page keeps the bodies and
// attachments of the last N days in the browser's own storage (IndexedDB),
// reads and searches them without a connection, and this is the server's
// half of that - which is small on purpose, because the mail lives in the
// browser and nowhere else:
//
//    GET /api/v1/me/offline            {days_max, days, since}: the ceiling the
//                                      administrator set (ReaderOfflineDaysMax)
//                                      and the reader's own choice under it - the
//                                      offline_days preference, clamped - with the
//                                      moment the window opens at, in the clock the
//                                      listings' received is written in.
//    PUT /api/v1/me/offline            {days}: the reader's choice, 0 to days_max.
//                                      0 keeps nothing beyond what the page always
//                                      kept: the inbox listing and the last opened
//                                      messages.
//    GET /api/v1/me/offline/messages   ?after_id=&limit=: every message received
//                                      within the window, in every folder the account
//                                      may read, by id from the oldest up - the ids
//                                      grow with time - paged by after_id. Only what
//                                      the page needs to decide what to fetch and
//                                      what to drop: the id, the folder, the size,
//                                      received and the flags. No file is read and no
//                                      message is copied, so a page of the walk costs
//                                      the server one pass over the folders it already
//                                      holds in memory, reading a few fields of each,
//                                      and the rows of that page; the page fetches
//                                      each body through GET /api/v1/me/messages/{id}
//                                      as it always has.
//
// The window is measured against the message's received time (the createtime
// column, the server's local clock), which is what the folder listings show and
// sort by, and not against the Date header, which a sender writes.
//
// The choice is read from hm_accountprefs, and a read that fails is answered
// as the failure it is (500), never as "nothing chosen": the page acts on
// nothing chosen by emptying what the browser keeps, and a database that
// was unreachable for a moment is no reason to.

#include "StdAfx.h"
#include "RestApiServer.h"
#include "HttpServer.h"
#include "JsonDocument.h"
#include "Unicode.h"
#include "../Application/Configuration.h"
#include "../Application/EffectiveSetting.h"
#include "../BO/Account.h"
#include "../BO/IMAPFolder.h"
#include "../BO/IMAPFolders.h"
#include "../BO/Message.h"
#include "../BO/Messages.h"
#include "../../IMAP/IMAPConfiguration.h"
#include "../../IMAP/IMAPFolderContainer.h"

#include <algorithm>
#include <ctime>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      const int OfflineDaysCeiling = 365;
      const int OfflineMessagesPerPage = 500;
      const wchar_t *OfflineDaysPreference = _T("offline_days");

      // The reader's choice, held within the administrator's ceiling. A
      // preference above a ceiling that was lowered later counts as the
      // ceiling; one that is not a number counts as nothing.
      int EffectiveDays(std::shared_ptr<const Account> account, const String &preference, int &ceiling)
      {
         // The ceiling is a setting with levels: the account's own, its
         // domain's, or the server's (docs/SettingLevels.md).
         ceiling = EffectiveSetting::ResolveInt(_T("reader_offline_days_max"), std::shared_ptr<const Domain>(), account);
         if (ceiling < 0)
            ceiling = 0;
         if (ceiling > OfflineDaysCeiling)
            ceiling = OfflineDaysCeiling;

         String text = preference;
         text.TrimLeft();
         text.TrimRight();
         if (text.IsEmpty())
            return 0;

         int days = 0;
         for (int i = 0; i < text.GetLength(); i++)
         {
            if (text[i] < '0' || text[i] > '9' || days > OfflineDaysCeiling)
               return 0;
            days = days * 10 + (text[i] - '0');
         }

         return days > ceiling ? ceiling : days;
      }

      // The moment the window opens at, as the server writes a received time:
      // that many days before now on the local clock, carried through mktime
      // so that a month boundary and summer time come out right, as the
      // search's older_than: does it.
      String WindowStart(int days)
      {
         const std::time_t now = std::time(nullptr);
         struct tm moment;
#ifdef HM_PLATFORM_POSIX
         if (::localtime_r(&now, &moment) == nullptr)
            return String();
#else
         if (localtime_s(&moment, &now) != 0)
            return String();
#endif

         moment.tm_mday -= days;
         moment.tm_isdst = -1;
         if (mktime(&moment) == (std::time_t) -1)
            return String();

         char text[32];
         if (strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &moment) == 0)
            return String();

         return String(text);
      }

      // The window's start is digits, dashes, a space and colons: nothing a
      // JSON string needs escaped.
      AnsiString WindowJson(int ceiling, int days)
      {
         AnsiString since = days > 0 ? AnsiString(WindowStart(days)) : AnsiString();
         AnsiString json;
         json.Format("{\"days_max\":%d,\"days\":%d,\"since\":\"%hs\"}", ceiling, days, since.c_str());
         return json;
      }

      AnsiString FlagsJson(const std::shared_ptr<Message> &message)
      {
         // The keywords are IMAP atoms - no quote, no backslash, nothing above
         // ASCII - so they go into the JSON as they are.
         AnsiString keywords;
         std::vector<String> list = message->GetKeywordList();
         for (size_t i = 0; i < list.size(); i++)
         {
            if (i > 0)
               keywords += ",";
            keywords += "\"" + AnsiString(list[i]) + "\"";
         }
         AnsiString flags;
         flags.Format("{\"seen\":%hs,\"flagged\":%hs,\"answered\":%hs,\"draft\":%hs,\"deleted\":%hs,\"keywords\":[%hs]}",
            message->GetFlagSeen() ? "true" : "false",
            message->GetFlagFlagged() ? "true" : "false",
            message->GetFlagAnswered() ? "true" : "false",
            message->GetFlagDraft() ? "true" : "false",
            message->GetFlagDeleted() ? "true" : "false",
            keywords.c_str());
         return flags;
      }

      // A bare number in the body: {"days": 14} or {"days": "14"}.
      bool ReadDays(const AnsiString &requestBody, int &days)
      {
         days = -1;
         JsonValue document;
         std::string parseError;
         if (!JsonValue::Parse(std::string(requestBody.c_str(), requestBody.size()), document, parseError) || !document.IsObject())
            return false;

         const JsonValue *member = document.Get("days");
         if (!member)
            return false;

         if (member->IsNumber())
         {
            double value = member->AsNumber();
            if (value < 0 || value > OfflineDaysCeiling || value != (double) (int) value)
               return false;
            days = (int) value;
            return true;
         }

         if (!member->IsString())
            return false;

         std::string text = member->AsString();
         if (text.empty() || text.size() > 3)
            return false;
         int value = 0;
         for (size_t i = 0; i < text.size(); i++)
         {
            if (text[i] < '0' || text[i] > '9')
               return false;
            value = value * 10 + (text[i] - '0');
         }
         days = value;
         return true;
      }
   }

   AnsiString
   RestApiServer::OpenApiOfflinePaths_()
   {
      static const char *paths =
         ",\"/api/v1/me/offline\":{"
         "\"get\":{\"summary\":\"How much mail the signed-in account may keep in its browser for reading offline\",\"description\":\"{days_max,days,since}: days_max is the ceiling the administrator set (ReaderOfflineDaysMax; 0 offers none), days the reader's own choice under it (the offline_days preference, clamped to the ceiling; 0 until they choose), and since the moment the window opens at, in the form the listings' received is written in - empty when days is 0. The webmail keeps the bodies and attachments of the messages received since then in the browser's own storage, reads and searches them without a connection, and removes them at sign-out; nothing is kept on the server for it.\",\"responses\":{\"200\":{\"description\":\"The window\"}}},"
         "\"put\":{\"summary\":\"Choose how many days to keep\",\"description\":\"Body: {days}, 0 to days_max. Writes the offline_days preference and answers as GET does. 0 turns it off: the webmail then keeps only the inbox listing and the last opened messages, as it always has. The choice is one of the account's hundred preferences: with all hundred taken by other keys it is refused as PUT /api/v1/me/preferences refuses the hundred-and-first.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"properties\":{\"days\":{\"type\":\"integer\"}}}}}},\"responses\":{\"200\":{\"description\":\"The window\"},\"400\":{\"description\":\"days is not a whole number between 0 and days_max, or the account already holds a hundred preferences (at most 100 preferences)\"},\"500\":{\"description\":\"The choice could not be read or saved: a failed read is never answered as nothing chosen, since the webmail empties its store on that\"}}}},"
         "\"/api/v1/me/offline/messages\":{\"get\":{\"summary\":\"The messages within the offline window\",\"description\":\"{days,since,count,bytes,messages:[{id,folder_id,uid,size,received,flags}],more,next_after_id}: every message received since the window opened, in every folder the account may read, by id from the oldest up; after_id continues from the id last seen and limit is at most 500. count and bytes are the whole window's, whatever the page, so the webmail can say whether it will fit in the browser's storage. Only the fields the webmail needs to decide what to fetch, what to drop and which flags changed: it reads each body through GET /api/v1/me/messages/{id}. No file is read and no message is copied: a page is one pass over the folders in memory. Empty, with days 0, when the reader keeps nothing.\",\"parameters\":[{\"name\":\"after_id\",\"in\":\"query\",\"schema\":{\"type\":\"integer\"}},{\"name\":\"limit\",\"in\":\"query\",\"schema\":{\"type\":\"integer\"}}],\"responses\":{\"200\":{\"description\":\"The page\"},\"500\":{\"description\":\"The choice could not be read\"}}}}";
      return AnsiString(paths);
   }

   HttpResponse
   RestApiServer::HandleMeOffline_(const Caller &caller, RouteKind kind, const AnsiString &query, const AnsiString &requestBody)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      // A read that failed is not a reader who chose nothing: it is answered
      // as a failure, so the page leaves its store as it is.
      String preference;
      if (!ReadPreference_(account->GetID(), OfflineDaysPreference, preference))
         return BuildResponse_(500, "{\"error\":\"the choice could not be read\"}");

      int ceiling = 0;
      int days = EffectiveDays(account, preference, ceiling);

      if (kind == RouteMeOffline)
         return BuildResponse_(200, WindowJson(ceiling, days));

      if (kind == RouteMeOfflinePut)
      {
         int wanted = 0;
         if (!ReadDays(requestBody, wanted) || wanted > ceiling)
         {
            AnsiString error;
            error.Format("{\"error\":\"days is a whole number between 0 and %d\",\"days_max\":%d}", ceiling, ceiling);
            return BuildResponse_(400, error);
         }

         String text;
         text.Format(_T("%d"), wanted);
         bool full = false;
         if (!WritePreference_(account->GetID(), OfflineDaysPreference, text, full))
         {
            // The cap is the reader's to answer - the same words the
            // preferences route uses for it - and a failed write the server's.
            if (full)
               return BuildResponse_(400, "{\"error\":\"at most 100 preferences\"}");
            return BuildResponse_(500, "{\"error\":\"the choice could not be saved\"}");
         }

         return BuildResponse_(200, WindowJson(ceiling, wanted));
      }

      if (kind != RouteMeOfflineMessages)
         return BuildResponse_(404, "{\"error\":\"not found\"}");

      const String since = days > 0 ? WindowStart(days) : String();
      if (days > 0 && since.IsEmpty())
         return BuildResponse_(500, "{\"error\":\"the window could not be worked out\"}");

      __int64 afterId = 0;
      AnsiString afterText = QueryParameter_(query, "after_id");
      if (!afterText.IsEmpty())
      {
         afterId = _atoi64(afterText.c_str());
         if (afterId < 0)
            afterId = 0;
      }

      int limit = OfflineMessagesPerPage;
      AnsiString limitText = QueryParameter_(query, "limit");
      if (!limitText.IsEmpty())
      {
         limit = atoi(limitText.c_str());
         if (limit < 1 || limit > OfflineMessagesPerPage)
            limit = OfflineMessagesPerPage;
      }

      // What a row of the page carries: the fields, read once, and not the
      // message they were read from.
      struct Row
      {
         __int64 id = 0;
         __int64 folder_id = 0;
         unsigned int uid = 0;
         int size = 0;
         String received;
         AnsiString flags;
      };
      const auto laterId = [](const Row &a, const Row &b) { return a.id < b.id; };

      // The candidates for this page: the limit + 1 smallest ids past after_id,
      // held as a heap with the largest on top so that what a smaller id
      // displaces is what the next page will list, and the one beyond the
      // page is how "more" is known. So a walk of a mailbox holds at most
      // 501 rows at a time, however many messages the window has.
      std::vector<Row> rows;
      int count = 0;
      __int64 bytes = 0;

      if (days > 0)
      {
         String delimiter = Configuration::Instance()->GetIMAPConfiguration()->GetHierarchyDelimiter();
         std::vector<std::pair<std::shared_ptr<IMAPFolder>, String>> folders;
         CollectReadableFolders_(account, IMAPFolderContainer::Instance()->GetFoldersForAccount(account->GetID()), String(), delimiter, folders, 0);

         for (size_t f = 0; f < folders.size(); f++)
         {
            std::shared_ptr<Messages> messages = folders[f].first->GetMessages();
            if (!messages)
               continue;

            // The collection's own objects, shared with every IMAP session on
            // the mailbox, reached through the pointers a snapshot takes under
            // its lock: not a copy of each message, which GetCopy constructs
            // and which a walk of a mailbox would pay for once per page.
            // Nothing here is written, and what is read - the id, the size,
            // the received time and the flags - is what the folder counts
            // read the same way.
            std::vector<std::shared_ptr<Message>> snapshot = messages->GetSnapshot();
            for (size_t i = 0; i < snapshot.size(); i++)
            {
               const std::shared_ptr<Message> &message = snapshot[i];
               if (!message || message->GetFlagDeleted())
                  continue;
               // The received time and the window's start share one fixed
               // form, YYYY-MM-DD HH:MM:SS, so the text compares as the time does.
               if (message->GetCreateTime().Compare(since) < 0)
                  continue;

               count++;
               bytes += message->GetSize();
               if (message->GetID() <= afterId)
                  continue;
               // With the heap full, an id at or past its top cannot be on
               // this page, and its flags are not worth writing out.
               if ((int) rows.size() > limit && message->GetID() >= rows.front().id)
                  continue;

               Row row;
               row.id = message->GetID();
               row.folder_id = folders[f].first->GetID();
               row.uid = message->GetUID();
               row.size = message->GetSize();
               row.received = message->GetCreateTime();
               row.flags = FlagsJson(message);
               rows.push_back(row);
               std::push_heap(rows.begin(), rows.end(), laterId);
               if ((int) rows.size() > limit + 1)
               {
                  std::pop_heap(rows.begin(), rows.end(), laterId);
                  rows.pop_back();
               }
            }
         }
      }

      std::sort(rows.begin(), rows.end(), laterId);

      const bool more = (int) rows.size() > limit;
      if (more)
         rows.resize(limit);

      AnsiString json;
      json.Format("{\"days\":%d,\"since\":\"%hs\",\"count\":%d,\"bytes\":%I64d,\"messages\":[",
         days, JsonEscape_(Utf8_(since)).c_str(), count, bytes);

      __int64 lastId = 0;
      for (size_t i = 0; i < rows.size(); i++)
      {
         const Row &row = rows[i];
         if (i > 0)
            json += ",";

         AnsiString entry;
         entry.Format("{\"id\":%I64d,\"folder_id\":%I64d,\"uid\":%u,\"size\":%d,\"received\":\"%hs\",\"flags\":%hs}",
            row.id,
            row.folder_id,
            row.uid,
            row.size,
            JsonEscape_(Utf8_(row.received)).c_str(),
            row.flags.c_str());
         json += entry;
         lastId = row.id;
      }

      AnsiString tail;
      tail.Format("],\"more\":%hs,\"next_after_id\":%I64d}", more ? "true" : "false", more ? lastId : (__int64) 0);
      json += tail;

      return BuildResponse_(200, json);
   }
}
