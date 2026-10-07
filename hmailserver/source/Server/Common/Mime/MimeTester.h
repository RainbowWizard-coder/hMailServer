// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   class MimeTester
   {
   public:
      MimeTester(void);
      ~MimeTester(void);

      void Test();

      // How a decoded header field value is unfolded: as 6.3.5 did, over generated
      // values, and in one pass. Test runs it; so does report-parser-tests on POSIX,
      // where nothing runs Test. Throws std::logic_error on a failure.
      static void TestFieldUnfolding();

      // How the field coder folds a value it writes: with no line of white space
      // alone, and so that a value set as COM sets it is checked by Message.Save as
      // it is written. Needs MimeEnvironment::AutoFolding, which the server sets.
      // Test runs it; so does report-parser-tests on POSIX. Throws std::logic_error
      // on a failure.
      static void TestFieldFolding();
   };
}