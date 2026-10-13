// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The parsers of an IMAP client's text and the String functions they lean on,
// as a program of its own for the POSIX build.
//
// On Windows the same testers run inside the shipping server: ClassTester::
// DoTests calls them, and the regression suite reaches it through
// Utilities.RunTestSuite. That route does not exist here, so the CMake build
// makes this instead and linux-build.sh runs it with every other *-tests
// program.
//
// What it holds them to: what each made up to 6.3.5 - the code of then is kept
// in the testers and given the same generated cases, ten times as many here as
// on Windows - and the time each takes, a pass over what it reads where 6.3.5's
// time grew with the square of it. String's FindNoCase, ContainsNoCase and
// ReplaceNoCase, wide and narrow; the items of a FETCH; the base subject SORT
// and THREAD read; the messages of a message set, each once; and the keywords
// a STORE keeps; a FETCH's HEADER.FIELDS sections, each message's header
// read once against the names asked; the bodies a FETCH's part numbers
// name, each part list read and each encapsulated message parsed once for a
// message's sections; the criteria a SEARCH, SORT or THREAD is parsed into,
// each named again beside itself left out - against a removal that compares
// every pair, with what they match before and after; and the bytes a FETCH
// answers for a message's parts, held to the bytes generated messages were
// written with, some of them read back from a file of its own in the
// system's temporary directory. Nothing here touches the network, the
// configuration or a database.
//
// One line a tester, and a non-zero exit on the first failure.

#include <cstdio>
#include <functional>
#include <stdexcept>

#include "../Common/Util/Parsing/StringParser.h"
#include "../IMAP/IMAPFetchParser.h"
#include "../IMAP/IMAPFetch.h"
#include "../IMAP/IMAPThread.h"
#include "../IMAP/IMAPCommandRangeAction.h"
#include "../IMAP/IMAPStore.h"
#include "../IMAP/IMAPSearchParser.h"

namespace
{
   bool Run_(const char *what, const std::function<void()> &test)
   {
      try
      {
         test();
      }
      catch (const std::exception &failure)
      {
         printf("FAIL  %s: %s\n", what, failure.what());
         return false;
      }

      printf("ok    %s\n", what);
      return true;
   }
}

int
main(int /*argc*/, char * /*argv*/[])
{
   const int cases = 200000;

   bool passed =
      Run_("String's case-ignoring search and replace: as 6.3.5's, in one pass", [cases]() { HM::StringNoCaseTester().Test(cases); }) &&
      Run_("the items of a FETCH: as 6.3.5's, in one pass", [cases]() { HM::IMAPFetchParserTester().Test(cases); }) &&
      Run_("the base subject: as 6.3.5's, in one pass", [cases]() { HM::IMAPThreadSubjectTester().Test(cases); }) &&
      Run_("a message set: 6.3.5's messages, each once, the view's UIDs found by halving; SEARCH and UID sets by halving; \"$\" once", [cases]() { HM::IMAPSequenceSetTester().Test(cases / 4); }) &&
      Run_("a STORE's keywords: as 6.3.5 kept them, in one pass", [cases]() { HM::IMAPStoreKeywordsTester().Test(cases); }) &&
      Run_("a FETCH's HEADER.FIELDS sections: as 6.3.5's, each header read once", [cases]() { HM::IMAPFetchHeaderFieldsTester().Test(cases / 4); }) &&
      Run_("the bodies a FETCH's part numbers name: as 6.3.5's, each part list read and each encapsulated message parsed once a message", [cases]() { HM::IMAPFetchSectionsTester().Test(cases / 20); }) &&
      Run_("the criteria of a SEARCH, SORT or THREAD: each named again beside itself left out, nothing else, what they match the same", [cases]() { HM::IMAPSearchParserTester().Test(cases / 10); }) &&
      Run_("the bytes of a FETCH's parts: each part's MIME header and body as the message was written, and a structure a client can read", [cases]() { HM::IMAPFetchPartBytesTester().Test(cases / 40); });

   return passed ? 0 : 1;
}
