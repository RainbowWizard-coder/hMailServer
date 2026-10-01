// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <mutex>
#include <string>

namespace HM
{
   // The Content-Security-Policy script-src that lets a page's one inline
   // script run and nothing else: the SHA-256 of that script's text, as a
   // hash source, 'sha256-<base64>'.
   //
   // The Control Deck (<ProgramFolder>/WebAdmin/index.html) is one file with
   // one inline <script> and no other script of any kind - no script file, no
   // event-handler attribute, no javascript: address, no eval. Under
   // script-src 'unsafe-inline' an escaping slip in any of its views of
   // outsider data (a mail header, a log line, a queue entry) would be script
   // in the administrator's session; under the hash only the page's own script
   // runs, whatever lands in the page.
   //
   // The hash is of the text as a browser takes it, which is not the bytes on
   // disk: the HTML parser turns CR LF and a lone CR into LF before it reads a
   // tag, and the browser hashes that text, encoded as UTF-8. The packages
   // install the page with CR LF line ends (.gitattributes), and a hash of
   // those bytes is refused by every engine, the Deck's own script with it.
   //
   // The script is found the way the HTML tokenizer finds it: "<script" in any
   // case followed by white space, "/" or ">"; its attributes read with their
   // quotes, so a ">" inside a quoted value does not end the tag; its text up
   // to the first "</script" followed by white space, "/" or ">". A page that
   // does not hold exactly one such element, or whose element names a src or
   // has no text, has no script to allow: the answer is then 'none', and the
   // page runs nothing rather than anything. Where this reading and a
   // browser's could differ - markup a script cannot be told from without a
   // whole parser, such as "<script" in a comment or an attribute value - the
   // count or the hash comes out different from the browser's and the page's
   // script is refused: the failure is a Deck that does not start, never a
   // script that runs and should not. build/check-deck-script.py refuses such
   // a page before it is shipped.
   class InlineScriptHash
   {
   public:
      // "'sha256-<base64>'" or "'none'", ready for a script-src directive.
      static AnsiString ScriptSource(const AnsiString &page);

      // The text a browser hashes for the page's one inline script, or false
      // when there is not exactly one with text and no src. For the tester.
      static bool ScriptText(const AnsiString &page, std::string &text);
   };

   // ScriptSource for the page last asked about, held with that page's bytes,
   // so the Deck - read from disk on every request, as pageCache_ holds its
   // compressed forms by the same bytes - is hashed once per distinct file
   // rather than once per request. An upgrade that replaces the file is a new
   // page and a new hash, never a stale one.
   class InlineScriptHashCache
   {
   public:
      AnsiString ScriptSource(const AnsiString &page);

      // How many times a hash has been computed. For the tester.
      unsigned long Computed();

   private:
      std::mutex mutex_;
      std::string page_;
      AnsiString source_;
      bool held_ = false;
      unsigned long computed_ = 0;
   };

   // The hash against values computed by another implementation (Python's
   // hashlib), the CR LF trap, the tokenizer's corners and the 'none' cases.
   // Runs inside the service with the other class tests (ClassTester, which
   // Utilities.RunTestSuite reaches) and on Linux as inline-script-hash-tests.
   // No database, setting, file or network. Throws std::logic_error naming the
   // first thing that is wrong.
   class InlineScriptHashTester
   {
   public:
      void Test();
   };
}
