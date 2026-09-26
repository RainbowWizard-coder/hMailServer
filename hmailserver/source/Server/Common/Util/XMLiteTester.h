// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   // XMLite, the parser that reads a backup's index (BackupRestorer), on the
   // malformed indexes the triage of the semgrep-sast findings named on 26
   // September 2026: a text that stops inside a tag name, just after "<?",
   // "<!--" or "<![CDATA[", inside an attribute's quotes or after an escape
   // character, or after an XML declaration with no element; and entity
   // references without their ";" ("x&amp", "x&lt", "&ampX"). Each must end in
   // a parse error or in the text as written, and where a parse stops must lie
   // within the text. A well-formed document with an XML declaration, a
   // comment, a CDATA section and every entity parses as it should, and so
   // does an element with processing instructions inside it.
   //
   // Each text is copied into an allocation of exactly its characters and its
   // terminator, so that built with AddressSanitizer a read past the end is the
   // overflow it is; built without, a read past the end shows only where it
   // changes a result.
   //
   // It runs inside the service with the other class tests (ClassTester, which
   // Utilities.RunTestSuite reaches). It needs no database, no setting and no
   // file, and it puts nothing in the error log: the malformed texts are parsed
   // as XDoc::Load parses its root element, without the HM4229 report that
   // would fail the regression test after this one. It throws std::logic_error
   // naming the first thing that is wrong.
   class XMLiteTester
   {
   public:
      void Test();

   private:
      void TestMalformed_();
      void TestEntities_();
      void TestWellFormed_();
   };
}
