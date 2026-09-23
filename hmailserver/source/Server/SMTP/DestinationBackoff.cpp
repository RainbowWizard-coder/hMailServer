// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "DestinationBackoff.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // RFC 3463 codes a remote uses to say it is deferring this sender for
      // now. 4.4.5 is the code this server's own shaping uses, so a peer that
      // shapes the same way is read the same way.
      const TCHAR *SlowDownStatuses[] =
      {
         _T("4.7.0"), _T("4.7.28"), _T("4.7.500"), _T("4.7.650"), _T("4.3.2"), _T("4.4.5"), nullptr
      };

      // Phrases in the lower-cased text of a 4xx. The table is fixed; see the
      // header for why it is not editable. It is documented in
      // docs/OutboundShaping.md.
      const TCHAR *SlowDownPhrases[] =
      {
         _T("rate limit"), _T("rate-limit"), _T("ratelimit"), _T("too many"), _T("too much"),
         _T("unusual rate"), _T("unexpected volume"), _T("temporarily deferred"), _T("temporarily rate"),
         _T("throttl"), _T("try again later"), _T("server busy"), _T("back off"), _T("slow down"), nullptr
      };
   }

   DestinationBackoff::DestinationBackoff()
   {

   }

   DestinationBackoff::~DestinationBackoff()
   {

   }

   bool
   DestinationBackoff::IsSlowDownReply(int replyCode, const String &enhancedStatus, const String &text)
   {
      // 421: the service is closing the transmission channel. Whatever the
      // words, the remote is not taking mail from this session now.
      if (replyCode == 421)
         return true;

      // Only a transient reply can be a request to wait. A 5xx is a refusal.
      if (replyCode < 400 || replyCode > 499)
         return false;

      // Not a signal, whatever the text: 4.7.1 (greylisting and policy
      // refusals answer it, and it is about this message, not the
      // destination) and 4.2.x (one mailbox).
      if (enhancedStatus == _T("4.7.1") || enhancedStatus.StartsWith(_T("4.2.")))
         return false;

      for (int i = 0; SlowDownStatuses[i] != nullptr; i++)
      {
         if (enhancedStatus == SlowDownStatuses[i])
            return true;
      }

      String lower = text;
      lower.ToLower();

      for (int i = 0; SlowDownPhrases[i] != nullptr; i++)
      {
         if (lower.Find(SlowDownPhrases[i]) >= 0)
            return true;
      }

      return false;
   }

   DestinationBackoff::Verdict
   DestinationBackoff::Ask(const String &destination, time_t now, Entry &entry, int &deferMinutes)
   {
      deferMinutes = 0;

      const String key = Key_(destination);
      if (key.IsEmpty())
         return Connect;

      boost::lock_guard<boost::recursive_mutex> guard(mutex_);

      auto existing = entries_.find(key);
      if (existing == entries_.end())
         return Connect;

      Entry &found = existing->second;

      if (now < found.until)
      {
         // The window is open. Held until it ends, and counted.
         const time_t remaining = found.until - now;
         deferMinutes = (int) ((remaining + 59) / 60);
         if (deferMinutes < 1)
            deferMinutes = 1;

         found.deferred++;
         entry = found;
         return Wait;
      }

      if (found.probing && now - found.probe_started <= ProbeStaleSeconds)
      {
         // Somebody else is finding out. One minute, not counted: this is
         // not the window holding the delivery, it is the probe's turn.
         deferMinutes = 1;
         entry = found;
         return Wait;
      }

      found.probing = true;
      found.probe_started = now;
      entry = found;
      return Probe;
   }

   bool
   DestinationBackoff::ReportSlowDown(const String &destination, bool wasProbe, const String &replyText, const String &host,
                                      int baseMinutes, int maxMinutes, time_t now, Entry &entry)
   {
      const String key = Key_(destination);
      if (key.IsEmpty() || baseMinutes <= 0)
         return false;

      if (maxMinutes < baseMinutes)
         maxMinutes = baseMinutes;

      boost::lock_guard<boost::recursive_mutex> guard(mutex_);

      auto existing = entries_.find(key);

      if (existing == entries_.end())
      {
         Entry made;
         made.destination = key;
         made.since = now;
         made.window_minutes = baseMinutes;
         made.until = now + (time_t) baseMinutes * 60;
         made.reply = Sanitize_(replyText);
         made.host = host;

         entries_[key] = made;
         entry = made;
         return true;
      }

      Entry &found = existing->second;

      if (!wasProbe)
      {
         // A delivery that was already talking to the destination when the
         // window opened. The window it opened stands as it is.
         entry = found;
         return false;
      }

      int window = found.window_minutes > 0 ? found.window_minutes * 2 : baseMinutes;
      if (window > maxMinutes)
         window = maxMinutes;

      found.since = now;
      found.window_minutes = window;
      found.until = now + (time_t) window * 60;
      found.reply = Sanitize_(replyText);
      found.host = host;
      found.probing = false;
      found.probe_started = 0;

      entry = found;
      return true;
   }

   void
   DestinationBackoff::ProbeEnded(const String &destination)
   {
      const String key = Key_(destination);

      boost::lock_guard<boost::recursive_mutex> guard(mutex_);

      auto existing = entries_.find(key);
      if (existing == entries_.end())
         return;

      // Still marked probing means no slow-down reply came back to the probe:
      // the destination is open again. A probe that was answered with one has
      // already had its entry doubled and its flag cleared, and stays.
      if (existing->second.probing)
         entries_.erase(existing);
   }

   std::vector<DestinationBackoff::Entry>
   DestinationBackoff::List() const
   {
      boost::lock_guard<boost::recursive_mutex> guard(mutex_);

      std::vector<Entry> result;
      for (const auto &pair : entries_)
         result.push_back(pair.second);

      return result;
   }

   bool
   DestinationBackoff::Clear(const String &destination)
   {
      const String key = Key_(destination);

      boost::lock_guard<boost::recursive_mutex> guard(mutex_);

      auto existing = entries_.find(key);
      if (existing == entries_.end())
         return false;

      entries_.erase(existing);
      return true;
   }

   void
   DestinationBackoff::Clear()
   {
      boost::lock_guard<boost::recursive_mutex> guard(mutex_);
      entries_.clear();
   }

   String
   DestinationBackoff::FormatTime(time_t value)
   {
      struct tm moment;

#ifdef HM_PLATFORM_POSIX
      if (::localtime_r(&value, &moment) == nullptr)
         return String();
#else
      if (localtime_s(&moment, &value) != 0)
         return String();
#endif

      String text;
      text.Format(_T("%d-%.02d-%.02d %.02d:%.02d:%.02d"),
         moment.tm_year + 1900, moment.tm_mon + 1, moment.tm_mday, moment.tm_hour, moment.tm_min, moment.tm_sec);
      return text;
   }

   String
   DestinationBackoff::Key_(const String &destination)
   {
      String key = destination;
      key.Trim();
      key.ToLower();
      return key;
   }

   String
   DestinationBackoff::Sanitize_(const String &reply)
   {
      // One line of at most 200 characters, with nothing a log line or a JSON
      // string would trip on. The reply is the remote's words and is shown to
      // the administrator as such.
      String cleaned;

      for (int i = 0; i < reply.GetLength() && cleaned.GetLength() < 200; i++)
      {
         const TCHAR c = reply.GetAt(i);

         if (c == '\r' || c == '\n' || c == '\t')
         {
            if (!cleaned.IsEmpty() && cleaned.GetAt(cleaned.GetLength() - 1) != ' ')
               cleaned += _T(" ");
            continue;
         }

         if (c < 32 || c == 127)
            continue;

         cleaned += c;
      }

      cleaned.Trim();
      return cleaned;
   }
}
