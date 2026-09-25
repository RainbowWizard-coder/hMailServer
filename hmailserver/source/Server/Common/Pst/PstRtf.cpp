// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// See PstRtf.h.

#include "StdAfx.h"

#include "PstRtf.h"
#include "PstCodePages.h"
#include "PstFormat.h"

#include <cstdlib>
#include <cstring>
#include <vector>

namespace HM
{
   namespace
   {
      const uint32_t TypeLzfu = 0x75465A4C;   // "LZFu"
      const uint32_t TypeMela = 0x414C454D;   // "MELA"

      // The destinations whose text is never the body: tables, pictures,
      // document information, headers and footers.
      const char *const SkippedDestinations[] =
      {
         "fonttbl", "colortbl", "stylesheet", "info", "pict", "object", "header", "footer", "headerl", "headerr",
         "headerf", "footerl", "footerr", "footerf", "xmlnstbl", "listtable", "listoverridetable", "rsidtbl",
         "generator", "themedata", "colorschememapping", "latentstyles", "datastore", "filetbl", "revtbl",
         "pgdsctbl", "fldinst", "operator", "author", "title", "subject", "company", "userprops"
      };

      bool Skipped(const std::string &word)
      {
         for (size_t i = 0; i < sizeof(SkippedDestinations) / sizeof(SkippedDestinations[0]); i++)
         {
            if (word == SkippedDestinations[i])
               return true;
         }
         return false;
      }

      bool Letter(unsigned char c)
      {
         return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
      }

      int HexDigit(unsigned char c)
      {
         if (c >= '0' && c <= '9') return c - '0';
         if (c >= 'a' && c <= 'f') return c - 'a' + 10;
         if (c >= 'A' && c <= 'F') return c - 'A' + 10;
         return -1;
      }

      struct GroupState
      {
         bool suppressed;   // inside \htmlrtf: RTF only, not the HTML
         bool skipped;      // inside a destination that is not the body
         bool htmltag;      // inside {\*\htmltag}: the HTML itself
         int unicodeSkip;   // \ucN
      };

      // The groups deeper than this are read as text, not as groups: a real
      // body nests a few dozen deep.
      const size_t MaximumGroupDepth = 4096;
   }

   const std::string &
   PstRtf::Preload()
   {
      static const std::string preload =
         "{\\rtf1\\ansi\\mac\\deff0\\deftab720{\\fonttbl;}{\\f0\\fnil \\froman \\fswiss \\fmodern \\fscript \\fdecor MS Sans SerifSymbolArialTimes New RomanCourier"
         "{\\colortbl\\red0\\green0\\blue0\r\n\\par \\pard\\plain\\f0\\fs20\\b\\i\\u\\tab\\tx";
      return preload;
   }

   bool
   PstRtf::Decompress(const std::string &compressed, size_t limit, std::string &rtf, std::string &problem)
   {
      rtf.clear();

      // [MS-OXRTFCP] 2.1.3.1: COMPSIZE (what follows it), RAWSIZE, COMPTYPE, CRC.
      if (compressed.size() < 16)
      {
         problem = "the compressed RTF body is shorter than its header";
         return false;
      }

      const uint32_t compressedSize = PstFormat::U32(compressed, 0, "the RTF body's header");
      const uint32_t rawSize = PstFormat::U32(compressed, 4, "the RTF body's header");
      const uint32_t type = PstFormat::U32(compressed, 8, "the RTF body's header");
      const uint32_t crc = PstFormat::U32(compressed, 12, "the RTF body's header");

      const size_t want = rawSize < limit ? (size_t) rawSize : limit;

      if (type == TypeMela)
      {
         // Stored as it is: RAWSIZE bytes, or as many as there are.
         const size_t present = compressed.size() - 16;
         rtf = compressed.substr(16, want < present ? want : present);
         return true;
      }

      if (type != TypeLzfu)
      {
         problem = "the RTF body is compressed in a way [MS-OXRTFCP] does not define";
         return false;
      }

      if (compressedSize < 12 || (uint64_t) compressedSize + 4 > compressed.size())
      {
         problem = "the compressed RTF body is cut short";
         return false;
      }

      const unsigned char *input = (const unsigned char *) compressed.data() + 16;
      const size_t inputSize = compressedSize - 12;

      if (PstFormat::Crc(input, inputSize) != crc)
      {
         problem = "the compressed RTF body is damaged (its checksum does not match)";
         return false;
      }

      // [MS-OXRTFCP] 2.2.2: a 4,096-byte dictionary that starts with the
      // preload, written at every byte produced; a control byte whose bits,
      // lowest first, say whether each next item is a literal byte or a
      // two-byte reference - twelve bits of offset, four of length less two.
      // A reference to the write position ends the stream.
      unsigned char dictionary[4096];
      memset(dictionary, 0, sizeof(dictionary));
      const std::string &preload = Preload();
      memcpy(dictionary, preload.data(), preload.size());
      size_t write = preload.size();

      // RAWSIZE is the file's claim; what the input can make is at most 17
      // bytes for every 2 of a reference, and a control byte per eight items -
      // under nine times its length. The room reserved is the smaller of the
      // two, so that a few bytes claiming 4 GB reserve a few dozen.
      const uint64_t canMake = (uint64_t) inputSize * 9;
      rtf.reserve(canMake < want ? (size_t) canMake : want);
      size_t at = 0;
      while (at < inputSize && rtf.size() < want)
      {
         const unsigned char control = input[at++];
         for (int bit = 0; bit < 8 && at < inputSize && rtf.size() < want; bit++)
         {
            if ((control & (1 << bit)) == 0)
            {
               const unsigned char c = input[at++];
               rtf += (char) c;
               dictionary[write] = c;
               write = (write + 1) % 4096;
               continue;
            }

            if (at + 1 >= inputSize)
            {
               problem = "the compressed RTF body is cut short";
               return false;
            }

            const unsigned int reference = ((unsigned int) input[at] << 8) | input[at + 1];
            at += 2;
            const size_t offset = reference >> 4;
            const size_t length = (reference & 0x0F) + 2;

            if (offset == write)
               return true;

            for (size_t k = 0; k < length && rtf.size() < want; k++)
            {
               const unsigned char c = dictionary[(offset + k) % 4096];
               rtf += (char) c;
               dictionary[write] = c;
               write = (write + 1) % 4096;
            }
         }
      }

      return true;
   }

   std::string
   PstRtf::CompressLiterals(const std::string &rtf)
   {
      std::string body;
      size_t write = Preload().size();
      size_t i = 0;
      bool ended = false;
      while (!ended)
      {
         const size_t controlAt = body.size();
         body += '\0';
         unsigned char control = 0;
         for (int bit = 0; bit < 8; bit++)
         {
            if (i >= rtf.size())
            {
               control |= (unsigned char) (1 << bit);
               body += (char) ((write >> 4) & 0xFF);
               body += (char) ((write << 4) & 0xF0);
               ended = true;
               break;
            }
            body += rtf[i++];
            write = (write + 1) % 4096;
         }
         body[controlAt] = (char) control;
      }

      std::string out;
      const uint32_t header[4] = { (uint32_t) body.size() + 12, (uint32_t) rtf.size(), TypeLzfu, PstFormat::Crc(body) };
      for (int k = 0; k < 4; k++)
      {
         for (int b = 0; b < 4; b++)
            out += (char) ((header[k] >> (8 * b)) & 0xFF);
      }
      return out + body;
   }

   PstRtf::Kind
   PstRtf::KindOf(const std::string &rtf)
   {
      // [MS-OXRTFEX] 2.1.3.1.1: the marker is in the document's header, before
      // any text; the first kilobyte holds it in any file Outlook writes.
      const std::string head = rtf.substr(0, rtf.size() < 1024 ? rtf.size() : 1024);
      if (head.find("\\fromhtml1") != std::string::npos)
         return KindHtml;
      if (head.find("\\fromtext") != std::string::npos)
         return KindText;
      return KindRtf;
   }

   std::string
   PstRtf::Deencapsulate(const std::string &rtf, bool html)
   {
      std::string out;
      std::string pending;       // \'hh bytes, decoded together in the code page
      int codePage = 1252;
      uint32_t highSurrogate = 0;

      std::vector<GroupState> stack;
      GroupState state = { false, false, false, 1 };
      int skipCharacters = 0;
      bool firstInGroup = false;
      bool starred = false;

      auto flush = [&]()
      {
         if (pending.empty())
            return;
         std::string utf8;
         if (!PstCodePages::Decode(pending, codePage, utf8) && !PstCodePages::Decode(pending, 1252, utf8))
            utf8.clear();
         out += utf8;
         pending.clear();
      };

      auto visible = [&]() -> bool
      {
         return !state.skipped && (state.htmltag || !state.suppressed || !html);
      };

      auto text = [&](const char *value)
      {
         flush();
         out += value;
      };

      const size_t n = rtf.size();
      size_t i = 0;
      while (i < n)
      {
         const unsigned char c = (unsigned char) rtf[i];

         if (c == '{')
         {
            if (stack.size() < MaximumGroupDepth)
               stack.push_back(state);
            i++;
            firstInGroup = true;
            starred = false;
            continue;
         }

         if (c == '}')
         {
            if (!stack.empty())
            {
               state = stack.back();
               stack.pop_back();
            }
            i++;
            firstInGroup = false;
            starred = false;
            continue;
         }

         if (c == '\\')
         {
            i++;
            if (i >= n)
               break;

            const unsigned char next = (unsigned char) rtf[i];
            if (Letter(next))
            {
               size_t end = i;
               while (end < n && end - i < 32 && Letter((unsigned char) rtf[end]))
                  end++;
               const std::string word = rtf.substr(i, end - i);

               bool hasParameter = false;
               long parameter = 0;
               size_t k = end;
               if (k < n && (rtf[k] == '-' || (rtf[k] >= '0' && rtf[k] <= '9')))
               {
                  size_t m = k + 1;
                  while (m < n && m - k < 11 && rtf[m] >= '0' && rtf[m] <= '9')
                     m++;
                  parameter = strtol(rtf.substr(k, m - k).c_str(), nullptr, 10);
                  hasParameter = true;
                  k = m;
               }
               if (k < n && rtf[k] == ' ')
                  k++;
               i = k;

               const bool wasFirst = firstInGroup;
               firstInGroup = false;

               if (skipCharacters > 0)
               {
                  skipCharacters--;
                  continue;
               }

               if (wasFirst || starred)
               {
                  if (starred && word == "htmltag")
                  {
                     state.htmltag = true;
                     state.suppressed = false;
                     starred = false;
                     continue;
                  }
                  if (starred || Skipped(word))
                  {
                     state.skipped = true;
                     starred = false;
                     continue;
                  }
               }

               if (word == "htmlrtf")
               {
                  state.suppressed = !hasParameter || parameter != 0;
                  continue;
               }
               if (word == "ansicpg" && hasParameter && parameter > 0)
               {
                  flush();
                  codePage = (int) parameter;
                  continue;
               }
               if (word == "uc" && hasParameter && parameter >= 0 && parameter < 16)
               {
                  state.unicodeSkip = (int) parameter;
                  continue;
               }
               if (word == "u" && hasParameter)
               {
                  if (visible())
                  {
                     flush();
                     uint32_t unit = (uint32_t) (parameter < 0 ? parameter + 65536 : parameter) & 0xFFFF;
                     if (unit >= 0xD800 && unit <= 0xDBFF)
                        highSurrogate = unit;
                     else if (unit >= 0xDC00 && unit <= 0xDFFF && highSurrogate != 0)
                     {
                        PstFormat::AppendUtf8(out, 0x10000 + ((highSurrogate - 0xD800) << 10) + (unit - 0xDC00));
                        highSurrogate = 0;
                     }
                     else
                     {
                        highSurrogate = 0;
                        PstFormat::AppendUtf8(out, unit);
                     }
                  }
                  skipCharacters = state.unicodeSkip;
                  continue;
               }

               if (!visible())
                  continue;

               if (word == "par" || word == "line")
                  text("\r\n");
               else if (word == "tab")
                  text("\t");
               else if (word == "lquote")
                  text("\xE2\x80\x98");
               else if (word == "rquote")
                  text("\xE2\x80\x99");
               else if (word == "ldblquote")
                  text("\xE2\x80\x9C");
               else if (word == "rdblquote")
                  text("\xE2\x80\x9D");
               else if (word == "bullet")
                  text("\xE2\x80\xA2");
               else if (word == "endash")
                  text("\xE2\x80\x93");
               else if (word == "emdash")
                  text("\xE2\x80\x94");
               continue;
            }

            // A control symbol.
            i++;
            firstInGroup = false;

            if (next == '*')
            {
               starred = true;
               continue;
            }

            if (next == '\'')
            {
               int high = i < n ? HexDigit((unsigned char) rtf[i]) : -1;
               int low = i + 1 < n ? HexDigit((unsigned char) rtf[i + 1]) : -1;
               i += 2;
               if (skipCharacters > 0)
               {
                  skipCharacters--;
                  continue;
               }
               if (visible() && high >= 0 && low >= 0)
                  pending += (char) (high * 16 + low);
               continue;
            }

            if (skipCharacters > 0)
            {
               skipCharacters--;
               continue;
            }

            if (!visible())
               continue;

            if (next == '\\' || next == '{' || next == '}')
               pending += (char) next;
            else if (next == '~')
               text("\xC2\xA0");
            else if (next == '\r' || next == '\n')
               text("\r\n");
            continue;
         }

         if (c == '\r' || c == '\n')
         {
            i++;
            continue;
         }

         firstInGroup = false;
         if (skipCharacters > 0)
         {
            skipCharacters--;
            i++;
            continue;
         }

         if (visible())
            pending += (char) c;
         i++;
      }

      flush();
      return out;
   }
}
