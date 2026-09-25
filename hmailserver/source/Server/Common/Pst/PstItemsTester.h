// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// What the PST reader's extension reads, held to files and structures built
// here byte by byte from Microsoft's specifications: an ANSI file's NDB layer
// ([MS-PST]'s 32-bit header, pages, blocks, data trees and subnode trees, and a
// table whose rows fill an ANSI block exactly), with the damage it must refuse;
// the recurrence blob of [MS-OXOCAL] 2.2.1.44, cut short at every length and
// with every count and field it must refuse; the time zone structures and the
// conversions across a clock change, north and south; the global object id;
// and the iCalendar and vCard written from an appointment, a task and a contact
// - parsed, summarised and expanded by this server's own ICalendar and VCard
// as CalDAV and CardDAV would.

#pragma once

namespace HM
{
   class PstItemsTester
   {
   public:
      void Test();
   };
}
