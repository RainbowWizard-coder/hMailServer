// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The organisation's directory (DirectoryStore.h) over REST. Two surfaces, and
// the difference between them is the whole of the row's "the administrator or
// the person sets":
//
//    GET    /api/v1/me/directory?q=&limit=    every account and list of the
//                                             READER'S OWN domain, opted-out
//                                             and inactive accounts left out
//    GET    /api/v1/me/directory/me           the reader's own entry, and
//                                             whether they may change it
//    PUT    /api/v1/me/directory/me           the reader's own title, department,
//                                             telephone numbers, photo and opt-out
//    GET    /api/v1/me/directory/me/photo    the reader's own photo, listed or not
//    GET    /api/v1/me/directory/{id}/photo   one entry's photo, as the image it is
//
//    GET    /api/v1/accounts/{address}/directory   the administrator's view of
//                                                  one account's entry, the note
//                                                  included
//    PUT    /api/v1/accounts/{address}/directory   the administrator sets any
//                                                  field of it, the opt-out too
//    GET    /api/v1/accounts/{address}/directory/photo   the photo, for the
//                                                        administrator's page
//
// The reader's routes never take a domain. The domain is the one the signed-in
// account is in, read from the account and not from the request, so there is no
// spelling of a request that reads another domain's names - which is the whole
// of the boundary on a server that hosts domains for people who have nothing to
// do with one another.
//
// The person's PUT is refused every field but the opt-out when
// DirectoryUserEditable is off. The opt-out is never refused: it is a person
// saying "leave me out of the list", and a switch that could take that away
// would make the row's promise of an opt-out untrue.
//
// A photo goes up as base64 in the body and comes back from a route of its own,
// because a listing that carried a hundred photos inline would be megabytes on
// every keystroke of the completion. What is stored is judged by the bytes -
// JPEG, PNG or GIF, at most 100 KB decoded - and never by a media type the
// sender named.

#include "StdAfx.h"
#include "RestApiServer.h"
#include "HttpServer.h"
#include "DirectoryStore.h"
#include "JsonDocument.h"
#include "AuditTrail.h"
#include "Unicode.h"
#include "Encoding/Base64.h"
#include "../Application/Logger.h"
#include "../Cache/CacheContainer.h"
#include "../BO/Account.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      const int DirectoryListDefaultLimit = 200;
      const int DirectoryListMaximumLimit = 1000;

      AnsiString Int64Text(__int64 value)
      {
         AnsiString text;
         text.Format("%I64d", value);
         return text;
      }

      String Wide(const std::string &text)
      {
         String out;
         Unicode::MultiByteToWide(AnsiString(text), out);
         return out;
      }

      String DomainOf(const String &address)
      {
         const int at = address.Find(_T("@"));
         if (at < 0)
            return String();
         String domain = address.Mid(at + 1);
         domain.ToLower();
         return domain;
      }

      String Trim(const String &value)
      {
         String trimmed = value;
         trimmed.TrimLeft();
         trimmed.TrimRight();
         return trimmed;
      }

      bool Contains(const String &haystack, const String &lowerNeedle)
      {
         if (lowerNeedle.IsEmpty())
            return true;
         String lower = haystack;
         lower.ToLower();
         return lower.Find(lowerNeedle) >= 0;
      }
   }

   // One line of the directory. The note is the administrator's and is never
   // here: a reader asking who somebody is has no business with what the
   // administrator wrote about them.
   AnsiString
   RestApiServer::DirectoryEntryJson_(const DirectoryEntry &entry)
   {
      AnsiString json = "{\"id\":" + Int64Text(entry.id) +
         ",\"kind\":\"" + AnsiString(entry.list ? "list" : "person") +
         "\",\"name\":\"" + JsonEscape_(Utf8_(entry.name)) +
         "\",\"address\":\"" + JsonEscape_(Utf8_(entry.address)) +
         "\",\"title\":\"" + JsonEscape_(Utf8_(entry.record.title)) +
         "\",\"department\":\"" + JsonEscape_(Utf8_(entry.record.department)) +
         "\",\"phone\":\"" + JsonEscape_(Utf8_(entry.record.phone)) +
         "\",\"mobile\":\"" + JsonEscape_(Utf8_(entry.record.mobile)) +
         "\",\"office\":\"" + JsonEscape_(Utf8_(entry.record.office)) +
         "\",\"has_photo\":" + (entry.hasPhoto ? "true" : "false");

      if (entry.hasPhoto)
         json += ",\"photo_url\":\"/api/v1/me/directory/" + Int64Text(entry.id) + "/photo\"";

      json += "}";
      return json;
   }

   // The stored record of one account, for the two routes that answer one
   // entry. withNote is the administrator's view.
   AnsiString
   RestApiServer::DirectoryRecordJson_(const DirectoryRecord &record, bool withNote)
   {
      AnsiString json = "{\"title\":\"" + JsonEscape_(Utf8_(record.title)) +
         "\",\"department\":\"" + JsonEscape_(Utf8_(record.department)) +
         "\",\"phone\":\"" + JsonEscape_(Utf8_(record.phone)) +
         "\",\"mobile\":\"" + JsonEscape_(Utf8_(record.mobile)) +
         "\",\"office\":\"" + JsonEscape_(Utf8_(record.office)) +
         "\",\"opt_out\":" + (record.optOut ? "true" : "false") +
         ",\"has_photo\":" + (record.photoType.IsEmpty() ? "false" : "true") +
         ",\"photo_type\":\"" + JsonEscape_(Utf8_(record.photoType)) +
         "\",\"updated\":\"" + JsonEscape_(Utf8_(record.updated)) + "\"";

      if (withNote)
         json += ",\"note\":\"" + JsonEscape_(Utf8_(record.note)) + "\"";

      json += "}";
      return json;
   }

   // The fields of a PUT, applied onto a record. False, with problem, when a
   // member is of the wrong type, too long for its column, or a photo this
   // store will not keep. Only the members present are touched, so a page that
   // sends one field changes one field.
   bool
   RestApiServer::ReadDirectoryBody_(const AnsiString &requestBody, bool allowNote, bool allowOptOut, bool allowFields,
                                     DirectoryRecord &record, String &problem)
   {
      problem = _T("");

      JsonValue body;
      std::string parseError;
      std::string text(requestBody.c_str(), (size_t) requestBody.GetLength());
      if (!JsonValue::Parse(text, body, parseError) || !body.IsObject())
      {
         problem = _T("the body must be a JSON object");
         return false;
      }

      static const char *const known[] = { "title", "department", "phone", "mobile", "office", "photo", "opt_out", "note" };
      for (size_t i = 0; i < body.Members().size(); i++)
      {
         const std::string &key = body.Members()[i].first;
         bool recognised = false;
         for (size_t k = 0; !recognised && k < sizeof(known) / sizeof(known[0]); k++)
            recognised = key == known[k];

         if (!recognised)
         {
            problem = _T("this route takes title, department, phone, mobile, office, photo and opt_out; ") + Wide(key) + _T(" is none of them");
            return false;
         }

         if (key == "note" && !allowNote)
         {
            problem = _T("note is the administrator's; it is set at /api/v1/accounts/{address}/directory");
            return false;
         }

         if (key == "opt_out" && !allowOptOut)
         {
            problem = _T("opt_out cannot be set here");
            return false;
         }

         if (key != "opt_out" && key != "note" && !allowFields)
         {
            problem = _T("the administrator of this server sets the directory entries; you can still take yourself out of the directory with opt_out");
            return false;
         }
      }

      struct Field
      {
         const char *key;
         String *value;
         int maximum;
      };

      const Field fields[] =
      {
         { "title", &record.title, DirectoryStore::MaximumTitleLength },
         { "department", &record.department, DirectoryStore::MaximumDepartmentLength },
         { "phone", &record.phone, DirectoryStore::MaximumPhoneLength },
         { "mobile", &record.mobile, DirectoryStore::MaximumPhoneLength },
         { "office", &record.office, DirectoryStore::MaximumOfficeLength },
         { "note", &record.note, DirectoryStore::MaximumNoteLength },
      };

      for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
      {
         const JsonValue *member = body.Get(fields[i].key);
         if (!member)
            continue;

         if (member->IsNull())
         {
            *fields[i].value = String();
            continue;
         }

         if (!member->IsString())
         {
            problem.Format(_T("%hs must be a string"), fields[i].key);
            return false;
         }

         String value = Trim(Wide(member->AsString()));
         if (value.GetLength() > fields[i].maximum)
         {
            problem.Format(_T("%hs is at most %d characters"), fields[i].key, fields[i].maximum);
            return false;
         }

         // A line break in a field a page draws on one line is never what was
         // meant, and it is how a name is smuggled into a listing's layout.
         if (value.Find(_T("\r")) >= 0 || value.Find(_T("\n")) >= 0)
         {
            problem.Format(_T("%hs is one line"), fields[i].key);
            return false;
         }

         *fields[i].value = value;
      }

      if (const JsonValue *member = body.Get("opt_out"))
      {
         if (!member->IsBool())
         {
            problem = _T("opt_out must be true or false");
            return false;
         }
         record.optOut = member->AsBool();
      }

      if (const JsonValue *member = body.Get("photo"))
      {
         if (member->IsNull() || (member->IsString() && member->AsString().empty()))
         {
            record.photo = String();
            record.photoType = String();
         }
         else if (!member->IsString())
         {
            problem = _T("photo is the image as base64, or null to remove it");
            return false;
         }
         else
         {
            AnsiString encoded(member->AsString());
            AnsiString clean;
            for (int i = 0; i < encoded.GetLength(); i++)
            {
               const char c = encoded.GetAt(i);
               if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
                  clean += c;
            }

            // A base64 string this long cannot decode to an image this store
            // keeps, and refusing it before decoding is what keeps a request
            // from spending a megabyte of memory to be refused.
            if (clean.GetLength() > (DirectoryStore::MaximumPhotoBytes / 3 + 1) * 4 + 16)
            {
               problem.Format(_T("the photo is larger than the %d bytes a directory photo may be"), DirectoryStore::MaximumPhotoBytes);
               return false;
            }

            AnsiString bytes = Base64::Decode(clean.c_str(), (int) clean.GetLength());
            AnsiString contentType;
            if (!DirectoryStore::ReadPhoto(bytes, contentType, problem))
               return false;

            record.photo = Wide(std::string(clean.c_str(), (size_t) clean.GetLength()));
            record.photoType = Wide(std::string(contentType.c_str(), (size_t) contentType.GetLength()));
         }
      }

      return true;
   }

   AnsiString
   RestApiServer::OpenApiDirectoryPaths_()
   {
      static const char *paths =
         ",\"/api/v1/me/directory\":{\"get\":{\"summary\":\"The organisation's directory: every account and list of the reader's own domain\",\"description\":\"{enabled,editable,domain,entries:[{id,kind,name,address,title,department,phone,mobile,office,has_photo,photo_url}],count}. kind is person or list. The domain is the signed-in account's own and is never taken from the request, so no spelling of this route reads another domain's names; an account that is not active, and one whose owner has opted out, is not in it. q= narrows to the entries whose name, address, title or department contains the text, case-insensitively - which is what the To, Cc and Bcc completion asks - and limit= caps the answer at 200 by default, 1000 at most. With DirectoryEnabled off, enabled is false and entries is empty. editable says whether this reader may set their own entry.\",\"responses\":{\"200\":{\"description\":\"The directory\"}}}},"
         "\"/api/v1/me/directory/me\":{\"get\":{\"summary\":\"The reader's own directory entry\",\"description\":\"{enabled,editable,entry:{title,department,phone,mobile,office,opt_out,has_photo,photo_type,updated}}. The administrator's note is not here; it is answered to an administrator at /api/v1/accounts/{address}/directory alone.\",\"responses\":{\"200\":{\"description\":\"The entry\"}}},"
         "\"put\":{\"summary\":\"The reader sets their own directory entry\",\"description\":\"{title,department,phone,mobile,office,photo,opt_out}, any subset; a member sent null is cleared. photo is the image bytes as base64 - a JPEG, a PNG or a GIF of at most 100 KB decoded, judged by the bytes and not by any media type - and null removes it. With DirectoryUserEditable off, every member but opt_out is refused 403 and the opt-out still goes through, because leaving the list is the person's to decide. note is never taken here.\",\"responses\":{\"200\":{\"description\":\"The entry as it now is\"},\"400\":{\"description\":\"A member of the wrong type, too long for its column, or a photo this store will not keep\"},\"403\":{\"description\":\"DirectoryUserEditable is off and a member other than opt_out was sent\"}}}},"
         "\"/api/v1/me/directory/me/photo\":{\"get\":{\"summary\":\"The reader's own directory photo\",\"description\":\"Answered whether or not the reader is listed - a person who has opted out still has to be able to see the photo they chose - and 404 when they have none.\",\"responses\":{\"200\":{\"description\":\"The image\"},\"404\":{\"description\":\"No photo\"}}}},"
         "\"/api/v1/me/directory/{id}/photo\":{\"get\":{\"summary\":\"One directory entry's photo\",\"description\":\"The image as it is, under X-Content-Type-Options: nosniff and a sandbox policy. The id is the account id the listing answers. An entry of another domain, an inactive account and one that has opted out are all 404 alike, so this route says nothing the listing does not.\",\"responses\":{\"200\":{\"description\":\"The image\"},\"404\":{\"description\":\"No such entry in your directory, or it has no photo\"}}}},"
         "\"/api/v1/accounts/{address}/directory\":{\"get\":{\"summary\":\"An account's directory entry, administered\",\"description\":\"{address,entry:{title,department,phone,mobile,office,opt_out,has_photo,photo_type,updated,note}}. The note is the administrator's own and is answered here and nowhere else.\",\"responses\":{\"200\":{\"description\":\"The entry\"},\"404\":{\"description\":\"No such account\"}}},"
         "\"put\":{\"summary\":\"Set an account's directory entry\",\"description\":\"{title,department,phone,mobile,office,photo,opt_out,note}, any subset; a member sent null is cleared. The same widths and the same photo rule as the reader's own route, and the administrator may set the opt-out whether or not DirectoryUserEditable is on. A change to opt_out is recorded on the audit trail as directory-opt-out-changed, naming the account and which way it went: the rest of the entry is not audited, but putting somebody who asked to be left out back into their domain's listing is a decision of theirs reversed by another hand.\",\"responses\":{\"200\":{\"description\":\"The entry as it now is\"},\"400\":{\"description\":\"A member of the wrong type, too long for its column, or a photo this store will not keep\"},\"404\":{\"description\":\"No such account\"}}}},"
         "\"/api/v1/accounts/{address}/directory/photo\":{\"get\":{\"summary\":\"An account's directory photo\",\"description\":\"The image as it is, for an administrator's page. 404 when the account has none.\",\"responses\":{\"200\":{\"description\":\"The image\"},\"404\":{\"description\":\"No photo, or no such account\"}}}}";
      return AnsiString(paths);
   }

   HttpResponse
   RestApiServer::HandleMeDirectory_(const Caller &caller, RouteKind kind, __int64 id, const AnsiString &query, const AnsiString &requestBody)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      const bool enabled = DirectoryStore::Enabled();
      const bool editable = DirectoryStore::UserEditable(account);

      if (kind == RouteMeDirectory)
      {
         AnsiString head = AnsiString("{\"enabled\":") + (enabled ? "true" : "false") +
            ",\"editable\":" + (editable ? "true" : "false") +
            ",\"domain\":\"" + JsonEscape_(Utf8_(DomainOf(account->GetAddress()))) + "\",\"entries\":[";

         if (!enabled)
            return BuildResponse_(200, head + "],\"count\":0}");

         std::vector<DirectoryEntry> entries;
         if (!DirectoryStore::List(account->GetDomainID(), entries))
            return BuildResponse_(500, "{\"error\":\"the directory could not be read\"}");

         // What the query string holds is UTF-8, percent-decoded to bytes by
         // QueryParameter_, and it is turned into a wide string as UTF-8 and
         // not with the machine's code page. String(AnsiString) would decode
         // it with CP_ACP, which makes "Müller" into "MÃ¼ller" and leaves the
         // one colleague whose name is hardest to type impossible to find -
         // the completion asks this route on every keystroke.
         String needle;
         Unicode::MultiByteToWide(QueryParameter_(query, "q"), needle);
         needle.ToLower();

         int limit = DirectoryListDefaultLimit;
         AnsiString limitText = QueryParameter_(query, "limit");
         if (!limitText.IsEmpty())
         {
            limit = atoi(limitText.c_str());
            if (limit < 1)
               limit = 1;
            if (limit > DirectoryListMaximumLimit)
               limit = DirectoryListMaximumLimit;
         }

         AnsiString body = head;
         int shown = 0;
         int matched = 0;
         for (size_t i = 0; i < entries.size(); i++)
         {
            const DirectoryEntry &entry = entries[i];
            if (!needle.IsEmpty() &&
                !Contains(entry.name, needle) && !Contains(entry.address, needle) &&
                !Contains(entry.record.title, needle) && !Contains(entry.record.department, needle))
               continue;

            matched++;
            if (shown >= limit)
               continue;

            body += (shown ? "," : "") + DirectoryEntryJson_(entry);
            shown++;
         }

         AnsiString tail;
         tail.Format("],\"count\":%d}", matched);
         return BuildResponse_(200, body + tail);
      }

      if (kind == RouteMeDirectoryOwn)
      {
         DirectoryRecord record;
         if (!DirectoryStore::Get(account->GetID(), record))
            return BuildResponse_(500, "{\"error\":\"the directory entry could not be read\"}");

         AnsiString body = AnsiString("{\"enabled\":") + (enabled ? "true" : "false") +
            ",\"editable\":" + (editable ? "true" : "false") +
            ",\"entry\":" + DirectoryRecordJson_(record, false) + "}";
         return BuildResponse_(200, body);
      }

      if (kind == RouteMeDirectoryOwnPut)
      {
         DirectoryRecord record;
         if (!DirectoryStore::Get(account->GetID(), record))
            return BuildResponse_(500, "{\"error\":\"the directory entry could not be read\"}");

         record.accountId = account->GetID();

         String problem;
         if (!ReadDirectoryBody_(requestBody, false, true, editable, record, problem))
         {
            // The one refusal that is about authority rather than about the
            // body: the fields are the administrator's on this server.
            const int status = problem.Find(_T("administrator of this server")) >= 0 ? 403 : 400;
            return BuildResponse_(status, "{\"error\":\"" + JsonEscape_(Utf8_(problem)) + "\"}");
         }

         if (!DirectoryStore::Save(record))
            return BuildResponse_(500, "{\"error\":\"the directory entry could not be saved\"}");

         LOG_APPLICATION("RestApi: " + account->GetAddress() + " changed their directory entry.");

         DirectoryRecord saved;
         DirectoryStore::Get(account->GetID(), saved);
         AnsiString body = AnsiString("{\"enabled\":") + (enabled ? "true" : "false") +
            ",\"editable\":" + (editable ? "true" : "false") +
            ",\"entry\":" + DirectoryRecordJson_(saved, false) + "}";
         return BuildResponse_(200, body);
      }

      if (kind == RouteMeDirectoryOwnPhoto)
      {
         AnsiString ownBytes;
         AnsiString ownType;
         if (!DirectoryStore::Photo(account->GetID(), ownBytes, ownType))
            return BuildResponse_(404, "{\"error\":\"you have no directory photo\"}");

         HttpResponse own;
         own.status = 200;
         own.content_type = ownType;
         own.body = ownBytes;
         own.extra_headers =
            "X-Content-Type-Options: nosniff\r\n"
            "Content-Security-Policy: sandbox\r\n"
            "Cache-Control: private, max-age=60\r\n";
         return own;
      }

      if (kind == RouteMeDirectoryPhoto)
      {
         if (!enabled)
            return BuildResponse_(404, "{\"error\":\"no such directory entry\"}");

         // Scoped by the reader's own domain in the query itself, so an
         // opted-out account, an inactive one and one of another domain are
         // the same 404 - and one photo costs one query. Listing the whole
         // domain to decide whether one id is in it made a page of N people
         // cost N listings of N: the webmail asks this route once per card it
         // draws.
         AnsiString bytes;
         AnsiString contentType;
         if (!DirectoryStore::ListedPhoto(account->GetDomainID(), id, bytes, contentType))
            return BuildResponse_(404, "{\"error\":\"no such directory entry, or it has no photo\"}");

         HttpResponse response;
         response.status = 200;
         response.content_type = contentType;
         response.body = bytes;
         response.extra_headers =
            "X-Content-Type-Options: nosniff\r\n"
            "Content-Security-Policy: sandbox\r\n"
            "Cache-Control: private, max-age=3600\r\n";
         return response;
      }

      return BuildResponse_(404, "{\"error\":\"not found\"}");
   }

   HttpResponse
   RestApiServer::HandleAccountDirectory_(const Caller &caller, const String &address, RouteKind kind, const AnsiString &requestBody)
   {
      std::shared_ptr<const Account> account = CacheContainer::Instance()->GetAccount(address);
      if (!account)
         return BuildResponse_(404, "{\"error\":\"no such account\"}");

      DirectoryRecord record;
      if (!DirectoryStore::Get(account->GetID(), record))
         return BuildResponse_(500, "{\"error\":\"the directory entry could not be read\"}");

      record.accountId = account->GetID();

      if (kind == RouteAccountDirectoryPhoto)
      {
         AnsiString bytes;
         AnsiString contentType;
         if (!DirectoryStore::Photo(account->GetID(), bytes, contentType))
            return BuildResponse_(404, "{\"error\":\"this account has no directory photo\"}");

         HttpResponse photo;
         photo.status = 200;
         photo.content_type = contentType;
         photo.body = bytes;
         photo.extra_headers =
            "X-Content-Type-Options: nosniff\r\n"
            "Content-Security-Policy: sandbox\r\n"
            "Cache-Control: private, max-age=60\r\n";
         return photo;
      }

      if (kind == RouteAccountDirectoryGet)
      {
         AnsiString body = "{\"address\":\"" + JsonEscape_(Utf8_(account->GetAddress())) +
            "\",\"entry\":" + DirectoryRecordJson_(record, true) + "}";
         return BuildResponse_(200, body);
      }

      // The administrator sets any field, the note and the opt-out included:
      // DirectoryUserEditable is about the person's own hands, not about the
      // administrator's.
      const bool optOutBefore = record.optOut;

      String problem;
      if (!ReadDirectoryBody_(requestBody, true, true, true, record, problem))
         return BuildResponse_(400, "{\"error\":\"" + JsonEscape_(Utf8_(problem)) + "\"}");

      if (!DirectoryStore::Save(record))
         return BuildResponse_(500, "{\"error\":\"the directory entry could not be saved\"}");

      // hm_directory is not an audited table: a title and a telephone number
      // are written by the person themselves as often as by an administrator
      // and decide nothing about what the server does with mail. The opt-out
      // is not that. Clearing somebody else's puts a person who had asked to
      // be left out back into the listing, the completion, the page and the
      // collection of their domain, and that is a decision of theirs being
      // reversed by somebody else - so it is recorded under the actor the
      // dispatch installed, and setting one is recorded beside it.
      if (record.optOut != optOutBefore)
      {
         AuditTrail::Instance()->RecordEvent(_T("account"), account->GetAddress(), _T("directory-opt-out-changed"),
            record.optOut ? _T("the account was taken out of the organisation's directory")
                          : _T("the account's opt-out from the organisation's directory was cleared; it is listed again"));
      }

      LOG_APPLICATION("REST API: the directory entry of " + account->GetAddress() + " was changed by " + String(caller.identity) + ".");

      DirectoryRecord saved;
      DirectoryStore::Get(account->GetID(), saved);
      AnsiString body = "{\"address\":\"" + JsonEscape_(Utf8_(account->GetAddress())) +
         "\",\"entry\":" + DirectoryRecordJson_(saved, true) + "}";
      return BuildResponse_(200, body);
   }
}
