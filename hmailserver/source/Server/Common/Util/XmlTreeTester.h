// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   // XmlTree.h, the backup index's XML, held to what it must do:
   //
   //   * write, byte for byte, the text every earlier hMailServer wrote for the
   //     same tree - the line breaks and tabs, the space after each attribute,
   //     the five references and nothing else escaped;
   //   * read back every character it writes, in an attribute and as text;
   //   * read what earlier versions' reader read - a truncated index as far as
   //     it goes, text before the first tag, the lenient forms - and refuse what
   //     it refused, with the problem it names: a text that stops inside a name,
   //     just after "<?", "<!--" or "<![CDATA[", inside an attribute's quotes, or
   //     after an XML declaration with no element; an entity reference without
   //     its ";" kept as written; and nesting beyond XDoc::MaximumDepth refused
   //     at any depth, a hundred thousand levels among them, without exhausting
   //     the stack.
   //
   // It runs inside the service with the other class tests (ClassTester, which
   // Utilities.RunTestSuite reaches), and on Linux as xml-tree-tests. It needs no
   // database, no setting and no file, and it puts nothing in the error log: the
   // malformed texts go to XDoc::Parse, which does not report, not XDoc::Load,
   // whose HM4229 would fail the regression test after this one. It throws
   // std::logic_error naming the first thing that is wrong.
   class XmlTreeTester
   {
   public:
      void Test();

   private:
      void TestWriting_();
      void TestRoundTrip_();
      void TestReading_();
      void TestRefusals_();
      void TestEntities_();
      void TestDepth_();
      void TestEditing_();
   };
}
