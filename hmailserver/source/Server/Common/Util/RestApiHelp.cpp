// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The documentation, served. See RestApiServer.h and HelpPages.h.
//
//    GET /api/v1/help           every page: its name, its title, its size in
//                               bytes and its headings; the wiki base URL the
//                               "Open on the wiki" links are made against, and
//                               the build tag
//    GET /api/v1/help/{page}    one page's markdown, text/markdown
//    GET /portal-markdown.js    the webmail's markdown renderer
//
// Both routes are unauthenticated, as /deck-lang/ is: the text is the
// repository's own operator documentation, hmailserver/docs/*.md, public in
// the repository and on the wiki, and nothing else - no setting's value, no
// name of anything on this server - so a credential would protect nothing and
// would keep the Deck's sign-in card from offering help. What the pages hold is
// what build/generate-help-pages.py embedded when the server was built; the
// wiki link is the only part that is this server's own.
//
// {page} is a file name without its extension and without a path: Backup for
// Backup.md, matched without regard to case because a link somebody typed is
// as good as one the pane made. Anything carrying a separator, a dot or a
// character outside letters and digits is not a page and is refused with the
// same 404 as a page that is not there, so a request that tries the path for
// a file learns nothing. A page's answer
// carries Cache-Control for a day and an ETag from the build tag, and a
// request whose If-None-Match names that tag is answered 304, so a browser
// that holds the page asks a few bytes' worth on each open until the
// documentation changes in a build. The list is the one answer that is not
// build-constant - the wiki setting rides in it - so it is answered no-cache
// with an ETag of the build tag and a digest of the wiki value, and 304 to an
// If-None-Match naming that: a change to the setting reaches a browser on its
// next load rather than a day later, and a shared cache holds nothing stale.
#include "StdAfx.h"
#include "RestApiServer.h"
#include "HelpPages.h"
#include "HttpCompression.h"
#include "DavSupport.h"
#include "../Application/Configuration.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   extern const char *PortalHeaders;
   extern const char *const PortalMarkdownScriptPieces[];

   namespace
   {
      // One compressed copy per distinct answer - a page, or the list - held
      // by its bytes, as the Deck's page and catalogues are. The pages number
      // some seventy and the list is one more; the capacity holds them all.
      HttpCompressedCache helpCache_(128);

      // A page name is letters and digits: what the file names are, and the
      // whole of what a request may say. A dot would name an extension, a
      // separator a directory, and neither is a page.
      bool WellFormedPageName_(const AnsiString &name)
      {
         if (name.IsEmpty() || name.GetLength() > 64)
            return false;

         for (int i = 0; i < name.GetLength(); i++)
         {
            const char c = name[i];
            const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
            const bool digit = c >= '0' && c <= '9';
            if (!letter && !digit)
               return false;
         }

         return true;
      }

      const HelpPage *FindHelpPage_(const AnsiString &name)
      {
         if (!WellFormedPageName_(name))
            return nullptr;

         for (const HelpPage *page = HelpPages; page->name; page++)
         {
            if (name.CompareNoCase(page->name) == 0)
               return page;
         }

         return nullptr;
      }

      AnsiString JoinHelpPage_(const HelpPage &page)
      {
         AnsiString text;
         for (const char *const *piece = page.pieces; *piece; piece++)
            text += *piece;
         return text;
      }

      // The first level-one heading, which is what a document is called; the
      // file name when it has none.
      AnsiString HelpPageTitle_(const HelpPage &page)
      {
         for (const HelpHeading *heading = page.headings; heading->text; heading++)
         {
            if (heading->level == 1)
               return heading->text;
         }

         return page.name;
      }

      AnsiString HelpPageETag_(const HelpPage &page)
      {
         return AnsiString("\"") + HelpPagesTag + "-" + page.name + "\"";
      }

      // Whether an If-None-Match names this tag: the tag itself, or a list
      // that carries it, a weak marker allowed on either. Anything else - an
      // empty header, a star - is a fresh request.
      bool MatchesETag_(const AnsiString &ifNoneMatch, const AnsiString &etag)
      {
         if (ifNoneMatch.IsEmpty())
            return false;

         std::vector<AnsiString> candidates = StringParser::SplitString(ifNoneMatch, ",");
         for (size_t i = 0; i < candidates.size(); i++)
         {
            AnsiString candidate = candidates[i];
            candidate.Trim();
            if (candidate.StartsWith("W/"))
               candidate = candidate.Mid(2);
            if (candidate == etag)
               return true;
         }

         return false;
      }

      AnsiString Number_(int value)
      {
         AnsiString text;
         text.Format("%d", value);
         return text;
      }

      // A page is the same bytes until the next build: a day's cache. The list
      // carries the wiki setting: revalidated on every use.
      const char *HelpPageHeaders_ = "X-Content-Type-Options: nosniff\r\n"
                                     "Cache-Control: public, max-age=86400\r\n";
      const char *HelpListHeaders_ = "X-Content-Type-Options: nosniff\r\n"
                                     "Cache-Control: no-cache\r\n";
   }

   HttpResponse
   RestApiServer::HandleHelpList_(const AnsiString &request)
   {
      // The ETag names the build and the wiki value both, since the body is
      // a function of both: two settings are two tags, and the tag of a list
      // a browser holds stops matching the moment the setting changes.
      const AnsiString wiki = Utf8_(Configuration::Instance()->GetHelpWikiBaseUrl());
      const AnsiString etag = AnsiString("\"") + HelpPagesTag + "-" + Dav::Sha256Hex(wiki).Left(16) + "\"";

      if (MatchesETag_(GetHeader_(request, "if-none-match"), etag))
      {
         HttpResponse unchanged;
         unchanged.status = 304;
         unchanged.content_type = "application/json; charset=utf-8";
         unchanged.extra_headers = AnsiString(HelpListHeaders_) + "ETag: " + etag + "\r\n";
         return unchanged;
      }

      AnsiString body = "{\"wiki\":\"" + JsonEscape_(wiki) +
                        "\",\"tag\":\"" + AnsiString(HelpPagesTag) + "\",\"pages\":[";

      bool firstPage = true;
      for (const HelpPage *page = HelpPages; page->name; page++)
      {
         if (!firstPage)
            body += ",";
         firstPage = false;

         body += "{\"name\":\"" + JsonEscape_(page->name) + "\",\"title\":\"" + JsonEscape_(HelpPageTitle_(*page)) + "\",\"bytes\":";
         body += Number_((int) page->bytes) + ",\"headings\":[";

         bool firstHeading = true;
         for (const HelpHeading *heading = page->headings; heading->text; heading++)
         {
            if (!firstHeading)
               body += ",";
            firstHeading = false;

            body += "{\"text\":\"" + JsonEscape_(heading->text) + "\",\"level\":" + Number_(heading->level) +
                    ",\"line\":" + Number_(heading->line) + "}";
         }

         body += "]}";
      }

      body += "]}";

      HttpResponse response = BuildResponse_(200, body);
      helpCache_.Fill(response);
      response.extra_headers = AnsiString(HelpListHeaders_) + "ETag: " + etag + "\r\n";
      return response;
   }

   HttpResponse
   RestApiServer::HandleHelpPage_(const AnsiString &name, const AnsiString &request)
   {
      const HelpPage *page = FindHelpPage_(name);
      if (!page)
         return BuildResponse_(404, "{\"error\":\"no such page\"}");

      const AnsiString etag = HelpPageETag_(*page);

      HttpResponse response;
      response.content_type = "text/markdown; charset=utf-8";
      response.extra_headers = AnsiString(HelpPageHeaders_) + "ETag: " + etag + "\r\n";

      if (MatchesETag_(GetHeader_(request, "if-none-match"), etag))
      {
         response.status = 304;
         return response;
      }

      response.status = 200;
      response.body = JoinHelpPage_(*page);
      helpCache_.Fill(response);
      return response;
   }

   // The webmail's markdown renderer, PortalMarkdown.js beside Portal.js,
   // joined from its pieces and compressed on first use, as the S/MIME module
   // is. The Deck carries a copy of the same renderer inline, because its page
   // allows no script but its own inline one.
   HttpResponse
   RestApiServer::HandlePortalMarkdownScript_()
   {
      static HttpStaticBody script;

      HttpResponse response;
      response.content_type = "text/javascript; charset=utf-8";
      script.Fill(response, PortalMarkdownScriptPieces);
      response.extra_headers = PortalHeaders;
      return response;
   }

   AnsiString
   RestApiServer::OpenApiHelpPaths_()
   {
      return
         ",\"/api/v1/help\":{\"get\":{\"summary\":\"The documentation carried in the binary, listed\","
         "\"description\":\"Unauthenticated, as /deck-lang/ is: the text is the repository's own operator documentation (hmailserver/docs/*.md), public in the repository and on the wiki, and nothing of this server's own but the wiki base URL. {wiki, tag, pages:[{name, title, bytes, headings:[{text, level, line}]}]}. wiki is the HelpWikiBaseUrl setting, empty when the administrator hid the link; tag is the build tag the pages' ETags are made from; each page's name is what /api/v1/help/{page} takes, its title its first heading, its headings the level-one and level-two ones with the line each starts on, counted from 0. Answered Cache-Control: no-cache, since the wiki value rides in it, with an ETag of the build tag and a digest of that value, and 304 to an If-None-Match naming it.\","
         "\"responses\":{\"200\":{\"description\":\"The list\"},\"304\":{\"description\":\"The browser's copy is current\"}}}},"
         "\"/api/v1/help/{page}\":{\"get\":{\"summary\":\"One page of the documentation, as markdown\","
         "\"description\":\"Unauthenticated. page is a file name without its extension and without a path - Backup for Backup.md - matched without regard to case. text/markdown; charset=utf-8, with Cache-Control for a day and an ETag from the build tag; a request whose If-None-Match names the tag is answered 304. A name carrying a dot, a separator or anything outside letters and digits, and a name that is not a page, are one and the same 404.\","
         "\"parameters\":[{\"name\":\"page\",\"in\":\"path\",\"required\":true,\"schema\":{\"type\":\"string\"}}],"
         "\"responses\":{\"200\":{\"description\":\"The page's markdown\"},\"304\":{\"description\":\"The browser's copy is current\"},\"404\":{\"description\":\"No such page\"}}}}";
   }
}
