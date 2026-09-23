// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "DestinationBackoffTester.h"
#include "DestinationBackoff.h"

#include <stdexcept>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      void Expect(bool condition, const char *message)
      {
         if (!condition)
            throw std::logic_error(message);
      }

      struct Reply
      {
         int code;
         const TCHAR *enhanced;
         const TCHAR *text;
      };

      // Replies that ask the sender to wait, in the shape the delivery client
      // records them: the code, the enhanced status it parsed (or the class's
      // undefined code when the remote sent none), the whole line.
      const Reply SlowDownReplies[] =
      {
         { 421, _T("4.7.0"),   _T("421 4.7.0 [TSS04] Messages temporarily deferred due to unexpected volume") },
         { 451, _T("4.7.650"), _T("451 4.7.650 The mail server has been temporarily rate limited") },
         { 450, _T("4.7.28"),  _T("450 4.7.28 Our system has detected an unusual rate of unsolicited mail") },
         { 421, _T("4.0.0"),   _T("421 Service not available, closing transmission channel") },
         { 452, _T("4.3.2"),   _T("452 4.3.2 System not accepting network messages") },
         { 451, _T("4.4.5"),   _T("451 4.4.5 Mail system congestion") },
         { 451, _T("4.0.0"),   _T("451 Too many messages from your address") },
         { 450, _T("4.0.0"),   _T("450 Requested action aborted: try again later") },
         { 451, _T("4.0.0"),   _T("451 Server busy, please slow down") },
         { 451, _T("4.7.500"), _T("451 4.7.500 Server busy. Please try again later") },
         { 451, _T("4.0.0"),   _T("451 Throttling in effect for your address") },
      };

      // Replies that are not that signal, whatever they say.
      const Reply OtherReplies[] =
      {
         { 550, _T("5.1.1"),   _T("550 5.1.1 User unknown") },
         { 451, _T("4.7.1"),   _T("451 4.7.1 Greylisted, please try again later") },
         { 451, _T("4.7.1"),   _T("451 4.7.1 Too many recipients for a new sender") },
         { 450, _T("4.2.1"),   _T("450 4.2.1 Mailbox busy") },
         { 452, _T("4.2.2"),   _T("452 4.2.2 Mailbox full, too many messages") },
         { 250, _T(""),        _T("250 OK") },
         { 554, _T("5.7.1"),   _T("554 5.7.1 Rate limit exceeded, you are blocked") },
         { 552, _T("5.2.2"),   _T("552 Too many messages in the mailbox") },
         { 451, _T("4.3.0"),   _T("451 Temporary local problem") },
         { 450, _T("4.2.0"),   _T("450 Requested mail action not taken: mailbox unavailable") },
         { 0,   _T("4.4.1"),   _T("") },
      };
   }

   void
   DestinationBackoffTester::Test()
   {
      DestinationBackoff::Instance()->Clear();

      try
      {
         TestSignals_();
         TestWindowAndProbe_();
         TestCeilingAndStaleProbe_();
         TestListingAndClearing_();
      }
      catch (...)
      {
         DestinationBackoff::Instance()->Clear();
         throw;
      }

      DestinationBackoff::Instance()->Clear();
   }

   void
   DestinationBackoffTester::TestSignals_()
   {
      for (const Reply &reply : SlowDownReplies)
      {
         Expect(DestinationBackoff::IsSlowDownReply(reply.code, reply.enhanced, reply.text),
            "A reply that asks the sender to slow down was not read as one.");
      }

      for (const Reply &reply : OtherReplies)
      {
         Expect(!DestinationBackoff::IsSlowDownReply(reply.code, reply.enhanced, reply.text),
            "A reply that is not a request to slow down was read as one.");
      }

      // The exclusions win over the text: greylisting's 4.7.1 and one
      // mailbox's 4.2.x are never the destination's word.
      Expect(!DestinationBackoff::IsSlowDownReply(451, _T("4.7.1"), _T("451 4.7.1 rate limit, try again later")),
         "A 4.7.1 reply was read as a slow-down because of its text.");
      Expect(!DestinationBackoff::IsSlowDownReply(450, _T("4.2.1"), _T("450 4.2.1 too many messages for this user")),
         "A 4.2.x reply was read as a slow-down because of its text.");

      // And case does not matter in the text.
      Expect(DestinationBackoff::IsSlowDownReply(451, _T("4.0.0"), _T("451 RATE LIMIT EXCEEDED")),
         "The phrase table is case-sensitive.");
   }

   void
   DestinationBackoffTester::TestWindowAndProbe_()
   {
      DestinationBackoff *backoff = DestinationBackoff::Instance();
      backoff->Clear();

      const time_t t0 = 1000000;
      DestinationBackoff::Entry entry;
      int deferMinutes = 0;

      Expect(backoff->Ask(_T("slow.example"), t0, entry, deferMinutes) == DestinationBackoff::Connect,
         "A destination with no window was not open.");

      // The first slow-down reply opens a window of the base.
      Expect(backoff->ReportSlowDown(_T("Slow.Example"), false, _T("421 4.7.0 too\r\nmany"), _T("mx.slow.example"), 2, 60, t0, entry),
         "The first slow-down reply did not open a window.");
      Expect(entry.window_minutes == 2 && entry.since == t0 && entry.until == t0 + 120 && !entry.probing,
         "The first window is not the base, from now.");
      Expect(entry.destination == _T("slow.example"), "The destination was not lower-cased.");
      Expect(entry.reply == _T("421 4.7.0 too many"), "The reply's line break was not folded to a space.");
      Expect(entry.host == _T("mx.slow.example"), "The host was not kept.");

      // Held for the rest of the window, rounded up to whole minutes, counted.
      Expect(backoff->Ask(_T("slow.example"), t0 + 30, entry, deferMinutes) == DestinationBackoff::Wait && deferMinutes == 2,
         "A delivery 30 seconds into a two-minute window was not held for two minutes.");
      Expect(backoff->Ask(_T("SLOW.example"), t0 + 119, entry, deferMinutes) == DestinationBackoff::Wait && deferMinutes == 1,
         "A delivery a second before the window ends was not held for one minute.");
      Expect(entry.deferred == 2, "The window did not count the deliveries it held.");

      // Another destination is untouched.
      Expect(backoff->Ask(_T("other.example"), t0 + 30, entry, deferMinutes) == DestinationBackoff::Connect,
         "A window for one destination held another.");

      // A slow-down reply from a delivery that was in flight when the window
      // opened changes nothing.
      Expect(!backoff->ReportSlowDown(_T("slow.example"), false, _T("451 4.7.650 rate limited"), _T("mx2.slow.example"), 2, 60, t0 + 40, entry),
         "A slow-down reply from a delivery that was not the probe changed the window.");
      Expect(entry.window_minutes == 2 && entry.until == t0 + 120 && entry.reply == _T("421 4.7.0 too many"),
         "The window was altered by a delivery that was not the probe.");

      // When the window has passed, the first delivery is the probe; the next
      // waits a minute, not counted.
      Expect(backoff->Ask(_T("slow.example"), t0 + 120, entry, deferMinutes) == DestinationBackoff::Probe && entry.probing,
         "The first delivery after the window did not become the probe.");
      Expect(backoff->Ask(_T("slow.example"), t0 + 121, entry, deferMinutes) == DestinationBackoff::Wait && deferMinutes == 1 && entry.deferred == 2,
         "A delivery during the probe was not held for a minute uncounted.");

      // The probe answered with a slow-down reply: the window doubles, from now.
      Expect(backoff->ReportSlowDown(_T("slow.example"), true, _T("451 4.7.650 rate limited"), _T("mx2.slow.example"), 2, 60, t0 + 130, entry),
         "A slow-down reply to the probe did not double the window.");
      Expect(entry.window_minutes == 4 && entry.since == t0 + 130 && entry.until == t0 + 130 + 240 && !entry.probing,
         "The doubled window is not four minutes from the probe's reply.");
      Expect(entry.reply == _T("451 4.7.650 rate limited") && entry.host == _T("mx2.slow.example"),
         "The doubled window does not carry the probe's reply.");
      Expect(backoff->Ask(_T("slow.example"), t0 + 131, entry, deferMinutes) == DestinationBackoff::Wait && deferMinutes == 4,
         "The doubled window did not hold the next delivery.");

      // ProbeEnded on an entry whose probe was answered does nothing.
      backoff->ProbeEnded(_T("slow.example"));
      Expect(backoff->List().size() == 1, "ProbeEnded removed a window that a slow-down reply had just doubled.");

      // The probe delivered, or failed with anything else: the entry goes.
      Expect(backoff->Ask(_T("slow.example"), t0 + 130 + 240, entry, deferMinutes) == DestinationBackoff::Probe,
         "The delivery after the doubled window did not become the probe.");
      backoff->ProbeEnded(_T("slow.example"));
      Expect(backoff->List().empty(), "A probe that was not answered with a slow-down reply did not clear the entry.");
      Expect(backoff->Ask(_T("slow.example"), t0 + 130 + 241, entry, deferMinutes) == DestinationBackoff::Connect,
         "The destination was not open after the probe cleared it.");

      backoff->Clear();
   }

   void
   DestinationBackoffTester::TestCeilingAndStaleProbe_()
   {
      DestinationBackoff *backoff = DestinationBackoff::Instance();
      backoff->Clear();

      const time_t t0 = 2000000;
      DestinationBackoff::Entry entry;
      int deferMinutes = 0;

      // Base 2, ceiling 5: 2, 4, 5, 5.
      backoff->ReportSlowDown(_T("capped.example"), false, _T("421 busy"), _T("mx"), 2, 5, t0, entry);
      Expect(entry.window_minutes == 2, "The first window is not the base.");

      time_t now = t0;
      const int expected[] = { 4, 5, 5 };
      for (int i = 0; i < 3; i++)
      {
         now = entry.until;
         Expect(backoff->Ask(_T("capped.example"), now, entry, deferMinutes) == DestinationBackoff::Probe, "No probe at the end of the window.");
         backoff->ReportSlowDown(_T("capped.example"), true, _T("421 busy"), _T("mx"), 2, 5, now, entry);
         Expect(entry.window_minutes == expected[i], "The window did not double to the ceiling and stop there.");
      }

      // A ceiling below the base reads as the base.
      backoff->Clear();
      backoff->ReportSlowDown(_T("capped.example"), false, _T("421 busy"), _T("mx"), 10, 3, t0, entry);
      Expect(entry.window_minutes == 10, "A ceiling below the base shrank the first window.");

      // A probe nobody ended - a delivery thread that died with it - goes stale
      // and the next delivery takes it over, rather than the destination
      // staying closed for good.
      backoff->Clear();
      backoff->ReportSlowDown(_T("stale.example"), false, _T("421 busy"), _T("mx"), 1, 60, t0, entry);
      Expect(backoff->Ask(_T("stale.example"), t0 + 60, entry, deferMinutes) == DestinationBackoff::Probe, "No probe at the end of the window.");
      Expect(backoff->Ask(_T("stale.example"), t0 + 61, entry, deferMinutes) == DestinationBackoff::Wait, "A second delivery probed beside the first.");
      Expect(backoff->Ask(_T("stale.example"), t0 + 60 + DestinationBackoff::ProbeStaleSeconds + 1, entry, deferMinutes) == DestinationBackoff::Probe,
         "A stale probe was not taken over.");

      backoff->Clear();
   }

   void
   DestinationBackoffTester::TestListingAndClearing_()
   {
      DestinationBackoff *backoff = DestinationBackoff::Instance();
      backoff->Clear();

      const time_t t0 = 3000000;
      DestinationBackoff::Entry entry;

      // A reply of more than 200 characters, with control characters in it,
      // is kept to one clean line of 200.
      String longReply = _T("451 4.7.650 ");
      for (int i = 0; i < 30; i++)
         longReply += _T("rate limited ");
      longReply += _T("\x01\x02end");

      backoff->ReportSlowDown(_T("a.example"), false, longReply, _T("mx.a"), 2, 60, t0, entry);
      Expect(entry.reply.GetLength() <= 200 && entry.reply.GetLength() >= 190 &&
             entry.reply.Find(_T("\x01")) < 0 && entry.reply.Find(_T("\r")) < 0 && entry.reply.Find(_T("end")) < 0,
         "The stored reply was not cut to 200 clean characters.");

      backoff->ReportSlowDown(_T("b.example"), false, _T("421 busy"), _T("mx.b"), 2, 60, t0, entry);

      std::vector<DestinationBackoff::Entry> listed = backoff->List();
      Expect(listed.size() == 2, "The listing does not hold both windows.");
      Expect(listed[0].destination == _T("a.example") && listed[1].destination == _T("b.example"), "The listing is not by destination.");

      Expect(backoff->Clear(_T("A.EXAMPLE")), "Clearing one window by name, in another case, failed.");
      Expect(!backoff->Clear(_T("a.example")), "Clearing a window that is not there answered true.");
      Expect(backoff->List().size() == 1 && backoff->List()[0].destination == _T("b.example"), "Clearing one window took the other.");

      backoff->Clear();
      Expect(backoff->List().empty(), "Clear did not forget every window.");

      Expect(DestinationBackoff::FormatTime(t0).GetLength() == 19, "FormatTime is not YYYY-MM-DD HH:MM:SS.");
   }
}
