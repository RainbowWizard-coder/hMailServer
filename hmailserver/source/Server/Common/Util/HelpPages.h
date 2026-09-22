// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The operator documentation, carried in the binary. hmailserver/docs/*.md is
// what the wiki's reference pages are written from and what CI holds; the
// wiki itself lives in another repository and cannot be built in, so what a
// server with no route to the Internet serves is this: the same text,
// embedded by build/generate-help-pages.py as HelpPagesData.cpp, in the
// arrangement the webmail's page uses (pieces of four thousand characters,
// joined when served). The three help panes - the Control Deck's and the
// webmail's, over GET /api/v1/help and /api/v1/help/{page} in
// RestApiHelp.cpp; the Control Panel's from its own embedded copy over no
// route at all - map each of their pages to one of these documents and one of
// its headings; build/check-help-map.py holds the three maps to the documents.

#pragma once

namespace HM
{
   // One heading of a page: its text, its level (1 or 2: ATX # and ##, or
   // setext under ==== and ----) and the line of the page it starts on,
   // counted from 0, so a pane can scroll to it.
   struct HelpHeading
   {
      const char *text;
      int level;
      int line;
   };

   // One page: the file name without its extension (Backup for Backup.md),
   // the byte length of its text (UTF-8, LF line endings), the pieces of that
   // text ended by a null pointer, and its headings ended by a null text.
   struct HelpPage
   {
      const char *name;
      unsigned int bytes;
      const char *const *pieces;
      const HelpHeading *headings;
   };

   // Every page, in file-name order, ended by a null name.
   extern const HelpPage HelpPages[];

   // The build tag: the first sixteen hex digits of the SHA-256 of every
   // page's text in table order. The routes serve it as the ETag, so a
   // browser that holds a page is answered 304 until the text changes.
   extern const char *const HelpPagesTag;
}
