// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// A back-off per destination, opened by the destination's own words.
//
// A large provider that is deferring this server says so in its reply - a 421
// at the banner, a 4.7.0 or 4.7.650 at MAIL FROM, "too many messages" in the
// text - and until this existed the answer was to keep connecting: every
// message to that provider was offered, deferred, and retried on the ordinary
// schedule, each attempt spending the message's retry budget and each one
// another reason for the provider to keep saying no. What this holds is the
// answer an MTA is expected to give: stop talking to that destination for a
// while, then send ONE delivery to find out, and double the wait if the answer
// is the same. Every other destination is untouched, which is the whole point
// of keying it by destination rather than pausing the queue.
//
// The destination is what ExternalDelivery::DeliverToSingleDomain_ delivers
// to: the recipient domain of an MX delivery, or the host of a route or a
// relay, lower-cased. A provider's MX hosts share its rate limiting, so the
// domain is the right grain; a smart host is one destination whatever it
// relays for.
//
// The signal is fixed, and deliberately narrow (IsSlowDownReply): a 5xx is a
// refusal, not a request to slow down; 4.7.1 is what greylisting and policy
// refusals answer, and a back-off on it would hold the whole destination after
// every first contact; 4.2.x is one mailbox; a connection that was refused or
// timed out said nothing, and neither did a DNS failure or this server's own
// deferrals. What it reads is the reply code, the enhanced status the remote
// sent, and a short table of phrases in the text.
//
// In memory, per process, under one mutex, as RemoteDomainThrottle is: there is
// one delivery queue in one process, and a restart forgets every window, which
// costs at most one more slow-down reply per destination. The caller's clock
// is passed in rather than read here, so that the self-test can move it.

#pragma once

#include "../Common/Util/Singleton.h"

#include <ctime>
#include <map>
#include <vector>

namespace HM
{
   class DestinationBackoff : public Singleton<DestinationBackoff>
   {
   public:
      struct Entry
      {
         Entry() : since(0), until(0), window_minutes(0), deferred(0), probing(false), probe_started(0) {}

         String destination;     // the key, lower-cased
         time_t since;           // when the current window opened
         time_t until;           // when it ends
         int window_minutes;     // its length; doubles on every slow-down reply to a probe
         String reply;           // the remote's reply, at most 200 characters, control characters stripped
         String host;            // the host that answered it
         int deferred;           // how many deliveries this entry has held back
         bool probing;           // the window has passed and one delivery is finding out
         time_t probe_started;
      };

      // What Ask answers.
      enum Verdict
      {
         Connect = 0,     // no back-off for this destination: deliver as usual
         Probe = 1,       // the window has passed and this delivery is the one that finds out
         Wait = 2         // deferred: the window is open, or another delivery is probing
      };

      DestinationBackoff();
      virtual ~DestinationBackoff();

      // Whether a reply asks this server to slow down. replyCode is the
      // three-digit code, enhancedStatus the RFC 3463 code the remote sent (or
      // the one derived from the reply code, as MessageRecipient carries it),
      // text the whole reply.
      static bool IsSlowDownReply(int replyCode, const String &enhancedStatus, const String &text);

      // Asked before a connection is opened. On Wait, deferMinutes is how long
      // the caller should hold the batch (the rest of the window, or one minute
      // while another delivery probes) and entry is a copy of what is known.
      // On Probe the caller MUST end the probe - ProbeEnded, or ReportSlowDown
      // - or the destination stays marked probing until the probe goes stale.
      Verdict Ask(const String &destination, time_t now, Entry &entry, int &deferMinutes);

      // A slow-down reply came back from the destination. With no entry, a
      // window of baseMinutes opens; when this delivery was the probe, the
      // window doubles up to maxMinutes and the probe ends; a reply from a
      // delivery that was already in flight when a window opened changes
      // nothing. Returns true when a window was opened or doubled, and the
      // entry as it now stands.
      bool ReportSlowDown(const String &destination, bool wasProbe, const String &replyText, const String &host,
                          int baseMinutes, int maxMinutes, time_t now, Entry &entry);

      // The probe ended with anything but a slow-down reply - a delivery, a
      // 5xx, a plain 4xx, a connection failure. The destination is open again.
      void ProbeEnded(const String &destination);

      // Every entry, for the listing. A copy, read under the lock.
      std::vector<Entry> List() const;

      // Forgets one destination's window. False when there was none.
      bool Clear(const String &destination);

      // Forgets every window. For the self-tests only.
      void Clear();

      // "YYYY-MM-DD HH:MM:SS" on the local clock, as the log writes times.
      static String FormatTime(time_t value);

      // How long a probe may run before another delivery may take it over: a
      // delivery thread that died with the probe must not close a destination
      // for good.
      static const int ProbeStaleSeconds = 15 * 60;

   private:

      static String Key_(const String &destination);
      static String Sanitize_(const String &reply);

      mutable boost::recursive_mutex mutex_;
      std::map<String, Entry> entries_;
   };
}
