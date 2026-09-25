// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// A message's RTF body, as Outlook keeps it: PR_RTF_COMPRESSED, compressed with
// LZFu or stored as it is ("MELA") ([MS-OXRTFCP]), and the HTML or plain text
// Outlook wraps in RTF when that is what the message was (\fromhtml1,
// \fromtext) ([MS-OXRTFEX]).
//
// LZFu's CRC is checked; a stream that fails it gives nothing. MELA's CRC is 0
// by definition and not checked, and its RAWSIZE is taken as at most the bytes
// that follow - a file in the wild says 332 where 320 follow. Nothing is
// produced past RAWSIZE or past the caller's limit.

#pragma once

#include <string>

namespace HM
{
   class PstRtf
   {
   public:
      // PR_RTF_COMPRESSED to RTF. False, with the reason, when it cannot be.
      static bool Decompress(const std::string &compressed, size_t limit, std::string &rtf, std::string &problem);

      enum Kind
      {
         KindRtf = 0,      // a body written in RTF
         KindHtml = 1,     // HTML carried in RTF (\fromhtml1)
         KindText = 2      // plain text carried in RTF (\fromtext)
      };

      static Kind KindOf(const std::string &rtf);

      // The HTML (html true) or the text an RTF carries, as UTF-8: the text of
      // the \*\htmltag groups and of what \htmlrtf does not mark as RTF only,
      // with \'hh read in the document's \ansicpg and \uN as the character it
      // names. Of an RTF body written in RTF, the visible text, roughly.
      static std::string Deencapsulate(const std::string &rtf, bool html);

      // [MS-OXRTFCP] 2.1.2.1: the 207 bytes the dictionary starts with.
      static const std::string &Preload();

      // For the tester: a compressed stream of literals only, which is what a
      // reader must take as readily as a compressed one.
      static std::string CompressLiterals(const std::string &rtf);
   };
}
