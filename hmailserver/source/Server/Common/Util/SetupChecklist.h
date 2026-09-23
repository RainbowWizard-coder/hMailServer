// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The first-hour checklist: what a new server still needs done, judged by the
// server itself rather than by whoever is reading the documentation.
//
// Nine items, the ones every installation has to get past before it is a
// mail server anybody can use: a profile chosen for the kind of server it is
// (on a young server; docs/Profiles.md), a domain, an account in it, TLS on the port
// clients submit through, an SPF record, DKIM signing with its record
// published, a DMARC record, a backup destination, and a second factor on the
// administrator password. Each is judged from what the server can see - its
// own configuration, the files it would open, and a DNS lookup made the way a
// receiving server makes it - and reported as one of four states:
//
//   done            the server found the thing in place;
//   action_needed   it is missing or wrong, and the finding says what;
//   cannot_tell     the server could not find out (a DNS lookup failed, or the
//                   caller asked for none), which says nothing either way;
//   not_needed      there is nothing to do, because nothing depends on it yet.
//
// Every finding carries a code and its arguments beside an English sentence,
// so that the Control Deck and the Control Panel can say the same thing in
// their own languages while a script reads the code. The document is what
// GET /api/v1/setup/checklist answers and what Utilities.GetSetupChecklist
// returns over COM; SetupChecklist.cpp says what each code means.
#pragma once

namespace HM
{
   class SetupChecklist
   {
   public:
      // The whole list as one JSON document. With lookUpDns, the SPF, DKIM and
      // DMARC records of every active domain are looked up, which costs the
      // resolver's timeout for each record that is missing - seconds, on a server
      // in its first hour, which is the server this is for. Without it the three
      // record items say which hosts to look up and leave the lookup to the
      // caller, which is what the Control Panel does off its window thread.
      static AnsiString Json(bool lookUpDns);
   };
}
