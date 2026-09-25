// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The PST reader held to vectors from Microsoft's specifications and to files
// built here, byte by byte, in memory: the CRC and the encode tables, compressed
// RTF ([MS-OXRTFCP]'s own examples, a flipped byte refused, a stored body whose
// size says more than it holds), HTML carried in RTF, the header's refusals, the
// data trees and subnode trees, the heap and its B-trees, a property context and
// a table - and, for each, what the reader must refuse without looping or
// allocating what the file claims: a B-tree that points back at itself, a tree
// that says it holds 4 GB, a heap reference past its page, a row index that
// claims more rows than asked for. Then the rebuild's pieces and the import's
// rules for names, keywords and dates. Nothing here comes from a real PST.

#pragma once

namespace HM
{
   class PstTester
   {
   public:
      void Test();
   };
}
