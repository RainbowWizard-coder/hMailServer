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
   };
}