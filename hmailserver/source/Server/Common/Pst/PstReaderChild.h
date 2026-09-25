// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The PST reader as a process of its own (docs/Migration.md, "Outlook data files
// (PST)"): `hMailServer.exe --pst-read <handle>` on Windows, `hmailserver
// --pst-read <descriptor>` on Linux. Only the service starts it
// (PstReaderProcess.h); it is not a command for an administrator.
//
// It is given the file, never a name: the handle or descriptor the service
// opened, and checked against the domain's import directory by the final path of
// that very handle, inherited into this process and named on the command line -
// so nothing can swap the file between the check and the read. It reads its
// requests from its standard input and writes its replies to its standard
// output, a frame each (PstReaderWire.h): it says hello, and then answers each
// request with this server's own PST reader (PstStore, PstMime, PstItems) under
// the same limits and the same work budget the service kept when the reader ran
// inside it. It ends when its input ends.
//
// It is dispatched before anything else in either program's main: no crash
// oracle, no configuration read, no database, no log, no COM registration.
// Memory running short in it is its own ceiling reached, the same for that
// message every time, and it answers that as too large, not as the server
// short of memory.
// What it needs is the file and the two pipes. A defect in the reader that ends
// the process ends this process; the service sees its output end, takes that as
// the reader having failed on what it was asked, and starts another.
//
// THE TEST HOOK. The open request may name NIDs (PstReaderProcess.h says where
// the service gets them): a request for one of them ends this process at once,
// as a crash in the reader would, so that the regression fixture can show the
// service surviving one. The service names none unless an administrator has put
// the hook's file beside the PST, and reports it (HM7553) whenever it does.

#pragma once

#include <string>

namespace HM
{
   class PstReaderChild
   {
   public:
      // The whole process: the handle or descriptor as the command line gives
      // it, in decimal. The exit code is 0 when the input ended, anything else
      // when the reader could not go on.
      static int Run(const std::string &native);

      // The exit code of a reader that ended itself for the test hook.
      static const int EndedByTestHook = 86;
   };
}
