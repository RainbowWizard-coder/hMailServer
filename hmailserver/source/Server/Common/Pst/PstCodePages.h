// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Windows code page identifiers, as a PST names them (PR_INTERNET_CPID for an
// HTML body's bytes, PR_MESSAGE_CODEPAGE for its PT_STRING8 text), with the
// charset name Internet mail gives each and the name iconv knows it by.
//
// One table for both platforms, so that a message reads the same wherever the
// server runs: on Windows the text is decoded with MultiByteToWideChar, on a
// POSIX system with iconv opened on the table's name. A code page that is not
// in the table is decoded on neither, and the message says so (docs/Migration.md
// lists the table).

#pragma once

#include <string>

namespace HM
{
   class PstCodePages
   {
   public:
      // The charset parameter for a code page ("windows-1252"), or empty when
      // the table does not have it.
      static std::string CharsetName(int codePage);

      // Whether the table has it.
      static bool Known(int codePage);

      // bytes in the code page, as UTF-8. False, with nothing written, when the
      // code page is not in the table or cannot be converted here.
      static bool Decode(const std::string &bytes, int codePage, std::string &utf8);

      // The table's rows, for the tester and the documentation: the count, and
      // one row by position (false past the end).
      static size_t Count();
      static bool Row(size_t index, int &codePage, std::string &charset, std::string &iconvName);
   };
}
