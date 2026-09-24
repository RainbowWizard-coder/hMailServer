// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The signed-in account's deleted items still within the recovery window
// (RecoverableStore.h):
//
//    GET    /api/v1/me/recoverable                every one, newest first
//    POST   /api/v1/me/recoverable/{id}/restore   back into the folder it came from
//    DELETE /api/v1/me/recoverable/{id}           gone for good, now

#include "StdAfx.h"
#include "RestApiServer.h"
#include "HttpServer.h"
#include "RecoverableStore.h"
#include "DiscoveryHolds.h"
#include "../BO/Account.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   AnsiString
   RestApiServer::OpenApiRecoverablePaths_()
   {
      static const char *paths =
         ",\"/api/v1/me/recoverable\":{\"get\":{\"summary\":\"Deleted items the signed-in account can still recover\",\"description\":\"{items:[{id,folder,folder_id,subject,from,size,seen,deleted}],days,held}: every message deleted within the last RecoverDeletedDays days - by IMAP EXPUNGE, UID EXPUNGE or CLOSE, a POP3 DELE at QUIT, the webmail's delete or its emptying of a folder, or the account's own retention choices - newest first. deleted is seconds since 1970. A message moved to another folder is not a deleted one. days is 0, and items empty, when the server keeps nothing - unless held is true: a legal hold covers the account (docs/Discovery.md), and every message it deletes is kept here whatever days says, until the hold is released.\",\"responses\":{\"200\":{\"description\":\"The items\"}}}},"
         "\"/api/v1/me/recoverable/{id}/restore\":{\"post\":{\"summary\":\"Put a deleted item back\",\"description\":\"Into the folder it was deleted from; when that folder is gone, the one of the same path, and otherwise the INBOX. Its flags come back with it; it is a new message with a new UID. The item leaves the list.\",\"responses\":{\"200\":{\"description\":\"folder_id and message_id of the message put back\"},\"404\":{\"description\":\"Not one of this account's, or already gone\"}}}},"
         "\"/api/v1/me/recoverable/{id}\":{\"delete\":{\"summary\":\"Delete an item for good now\",\"responses\":{\"200\":{\"description\":\"Gone\"},\"404\":{\"description\":\"Not one of this account's\"},\"409\":{\"description\":\"A legal hold covers the account, so its deleted items are kept until it is released\"}}}}";
      return AnsiString(paths);
   }

   HttpResponse
   RestApiServer::HandleMeRecoverable_(const Caller &caller, RouteKind kind, __int64 id)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      if (kind == RouteMeRecoverable)
      {
         std::vector<RecoverableStore::Entry> entries;
         if (!RecoverableStore::List(account->GetID(), entries))
            return BuildResponse_(500, "{\"error\":\"the deleted items could not be read\"}");

         AnsiString body = "{\"items\":[";
         for (size_t i = 0; i < entries.size(); i++)
         {
            const RecoverableStore::Entry &entry = entries[i];
            AnsiString numbers;
            numbers.Format("{\"id\":%I64d,\"folder_id\":%I64d,\"size\":%d,\"seen\":%hs,\"deleted\":%I64d,",
               entry.id, entry.folder_id, entry.size, (entry.flags & 1) ? "true" : "false", entry.deleted);
            body += (i ? "," : "") + numbers;
            body += "\"folder\":\"" + JsonEscape_(Utf8_(entry.folder)) + "\"";
            body += ",\"subject\":\"" + JsonEscape_(Utf8_(entry.subject)) + "\"";
            body += ",\"from\":\"" + JsonEscape_(Utf8_(entry.from)) + "\"}";
         }
         // held: a legal hold covers the account, so what is listed stays - a
         // copy of every message it deletes is kept, whatever days says, and
         // none of them can be deleted for good (docs/Discovery.md).
         AnsiString tail;
         tail.Format("],\"days\":%d,\"held\":%hs}", RecoverableStore::Days(account->GetID()),
                     DiscoveryHolds::IsAccountHeld(account->GetID()) ? "true" : "false");
         return BuildResponse_(200, body + tail);
      }

      if (kind == RouteMeRecoverableRestore)
      {
         __int64 folderId = 0;
         __int64 messageId = 0;
         String problem;
         if (!RecoverableStore::Restore(account, id, folderId, messageId, problem))
         {
            const int status = problem == _T("not found") || problem == _T("the kept copy is gone") ? 404 : 500;
            return BuildResponse_(status, "{\"error\":\"" + JsonEscape_(Utf8_(problem)) + "\"}");
         }

         LOG_APPLICATION("RestApi: " + account->GetAddress() + " recovered a deleted message.");

         AnsiString body;
         body.Format("{\"folder_id\":%I64d,\"message_id\":%I64d}", folderId, messageId);
         return BuildResponse_(200, body);
      }

      if (kind == RouteMeRecoverableDelete)
      {
         if (DiscoveryHolds::IsAccountHeld(account->GetID()))
            return BuildResponse_(409, "{\"error\":\"" + JsonEscape_(Utf8_(DiscoveryHolds::PurgeRefusal())) + "\"}");

         if (!RecoverableStore::Purge(account->GetID(), id))
            return BuildResponse_(404, "{\"error\":\"not one of your deleted items\"}");
         return BuildResponse_(200, "{\"deleted\":true}");
      }

      return BuildResponse_(404, "{\"error\":\"not found\"}");
   }
}
