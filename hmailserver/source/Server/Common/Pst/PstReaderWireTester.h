// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// What the PST reader's process and the service say to each other
// (PstReaderWire.h), held to its own rules: every record written and read back
// field for field; a frame read whole from a stream, and a stream that ends
// between two frames told from one that ends inside one; and what the service
// must refuse without allocating what a reply claims - a header of the wrong
// magic, a frame past the protocol's ceiling, a header that claims 300 MB and is
// followed by ten bytes, a reply cut at every length, a count larger than its
// payload, bytes after the last field, a status, an outcome or a role the
// protocol does not have. And each status the reader can answer with thrown in
// the service as the type the reader threw, so that the job's catches take a
// failure in the reader's process as they took one in the service.

#pragma once

namespace HM
{
   class PstReaderWireTester
   {
   public:
      void Test();
   };
}
