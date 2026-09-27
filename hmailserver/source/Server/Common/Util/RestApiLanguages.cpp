// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The webmail in the reader's language. English is the page; a catalogue
// maps every text of it to one language. GET /portal-lang/<code>.json
// answers a catalogue to anyone (the sign-in page has no account); the page
// itself carries the catalogue for the reader's Accept-Language so the first
// paint is already translated; the script fetches another when the reader
// chooses one (kept in the browser, and in the account's preferences once
// signed in).
//
// A catalogue is revalidated on every use (no-cache, an ETag of its digest,
// 304 to an If-None-Match naming it). It was cached for a day, and the page it
// translates is never cached: after an upgrade a reader who had chosen a
// language got the new page with yesterday's catalogue, and every text the
// upgrade added stayed English for a day - a screen reader read them out in
// the page's voice (the NVDA pass of Roadmap2 row 43 met exactly that).

#include "StdAfx.h"
#include "RestApiServer.h"
#include "HttpServer.h"
#include "HttpCompression.h"
#include "PortalLanguages.h"
#include "DavSupport.h"
#include <algorithm>
#include <cstdlib>
#include <map>
#include <mutex>
#include <vector>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      std::mutex catalogues_mutex;
      std::map<AnsiString, AnsiString> catalogues;

      AnsiString Lower(AnsiString value)
      {
         value.ToLower();
         return value;
      }

      // The code as the table spells it, for a tag in any case; empty when
      // there is no such language.
      AnsiString Known(const AnsiString &tag)
      {
         const AnsiString lowered = Lower(tag);
         for (const PortalLanguage *language = PortalLanguages; language->code; language++)
         {
            if (Lower(AnsiString(language->code)) == lowered)
               return AnsiString(language->code);
         }
         return AnsiString();
      }

      // A tag's language: the exact code, else the primary subtag, with the
      // regional ones mapped to the catalogue there is (pt to pt-BR, zh to
      // zh-Hans, no and nn to nb). English is the page itself: empty.
      AnsiString Match(const AnsiString &tag)
      {
         const AnsiString exact = Known(tag);
         if (!exact.IsEmpty())
            return exact;
         AnsiString primary = Lower(tag);
         int dash = primary.Find("-");
         if (dash > 0)
            primary = primary.Mid(0, dash);
         if (primary == "en")
            return AnsiString();
         if (primary == "pt")
            return Known("pt-BR");
         if (primary == "zh")
            return Known("zh-Hans");
         if (primary == "no" || primary == "nn")
            return Known("nb");
         return Known(primary);
      }
   }

   AnsiString
   RestApiServer::PortalLanguageJson_(const AnsiString &code)
   {
      const AnsiString known = Known(code);
      if (known.IsEmpty())
         return AnsiString();
      std::lock_guard<std::mutex> guard(catalogues_mutex);
      std::map<AnsiString, AnsiString>::iterator it = catalogues.find(known);
      if (it != catalogues.end())
         return it->second;
      AnsiString joined;
      for (const PortalLanguage *language = PortalLanguages; language->code; language++)
      {
         if (AnsiString(language->code) != known)
            continue;
         for (const char *const *piece = language->pieces; *piece; piece++)
            joined += *piece;
      }
      catalogues[known] = joined;
      return joined;
   }

   // Accept-Language (RFC 9110): tags with weights, the heaviest first; the
   // first that is a language here wins, and English anywhere before it means
   // the page as it is. Empty when nothing matches.
   AnsiString
   RestApiServer::NegotiatePortalLanguage_(const AnsiString &request)
   {
      const AnsiString lowered = Lower(request);
      int headersEnd = lowered.Find("\r\n\r\n");
      if (headersEnd < 0)
         headersEnd = lowered.GetLength();
      const int at = lowered.Find("\r\naccept-language:");
      if (at < 0 || at > headersEnd)
         return AnsiString();
      const int lineStart = at + 18;
      int lineEnd = lowered.Find("\r\n", lineStart);
      if (lineEnd < 0)
         lineEnd = lowered.GetLength();
      const AnsiString value = request.Mid(lineStart, lineEnd - lineStart);

      std::vector<std::pair<double, AnsiString> > choices;
      int position = 0;
      while (position <= value.GetLength())
      {
         const int comma = value.Find(",", position);
         AnsiString item = comma < 0 ? value.Mid(position) : value.Mid(position, comma - position);
         position = comma < 0 ? value.GetLength() + 1 : comma + 1;
         double weight = 1.0;
         const int semicolon = item.Find(";");
         if (semicolon >= 0)
         {
            const AnsiString parameters = Lower(item.Mid(semicolon + 1));
            item = item.Mid(0, semicolon);
            const int q = parameters.Find("q=");
            if (q >= 0)
               weight = atof(parameters.Mid(q + 2).c_str());
         }
         item.TrimLeft();
         item.TrimRight();
         if (item.IsEmpty() || weight <= 0 || item == "*")
            continue;
         choices.push_back(std::make_pair(-weight, item));
      }
      std::stable_sort(choices.begin(), choices.end(),
         [](const std::pair<double, AnsiString> &a, const std::pair<double, AnsiString> &b) { return a.first < b.first; });
      for (size_t i = 0; i < choices.size(); i++)
      {
         const AnsiString tag = Lower(choices[i].second);
         if (tag == "en" || tag.StartsWith("en-"))
            return AnsiString();
         const AnsiString match = Match(choices[i].second);
         if (!match.IsEmpty())
            return match;
      }
      return AnsiString();
   }

   HttpResponse
   RestApiServer::HandlePortalLanguage_(const AnsiString &code, const AnsiString &request)
   {
      const AnsiString json = PortalLanguageJson_(code);
      if (json.IsEmpty())
         return BuildResponse_(404, "{\"error\":\"no such language\"}");
      // The tag is the catalogue's own digest: it changes when the texts do,
      // in whatever build, and not otherwise.
      const AnsiString etag = AnsiString("\"lang-") + Known(code) + "-" + Dav::Sha256Hex(json).Left(16) + "\"";
      const AnsiString headers = AnsiString("Cache-Control: no-cache\r\nX-Content-Type-Options: nosniff\r\nETag: ") + etag + "\r\n";
      std::vector<AnsiString> named = StringParser::SplitString(GetHeader_(request, "if-none-match"), ",");
      for (size_t i = 0; i < named.size(); i++)
      {
         AnsiString candidate = named[i];
         candidate.Trim();
         if (candidate.StartsWith("W/"))
            candidate = candidate.Mid(2);
         if (candidate == etag)
         {
            HttpResponse unchanged;
            unchanged.status = 304;
            unchanged.content_type = "application/json; charset=utf-8";
            unchanged.extra_headers = headers;
            return unchanged;
         }
      }
      HttpResponse response;
      response.status = 200;
      response.content_type = "application/json; charset=utf-8";
      response.body = json;
      // Compressed once per language rather than once per request; the cache
      // is RestApiServer.cpp's, shared with the page.
      PortalPageCache().Fill(response);
      response.extra_headers = headers;
      return response;
   }
}
