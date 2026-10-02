// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// See NeverBanNameList.h.

#include "StdAfx.h"

#include "NeverBanNameList.h"

#include "../Application/Application.h"
#include "../BO/NeverBanName.h"
#include "../BO/NeverBanNames.h"
#include "../BO/SecurityRange.h"
#include "../BO/SecurityRanges.h"
#include "../Persistence/PersistenceMode.h"
#include "../Persistence/PersistentNeverBanName.h"
#include "../Persistence/PersistentSecurityRange.h"
#include "../TCPIP/DNSResolver.h"
#include "../TCPIP/IPAddress.h"
#include "../Threading/Task.h"
#include "../Threading/WorkQueue.h"
#include "Parsing/StringParser.h"
#include "Time.h"
#include "Unicode.h"

#include <atomic>
#include <ctime>
#include <map>
#include <mutex>
#include <stdexcept>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // ---- the rows
      //
      // The patterns as last read, normalised. A handful an administrator typed,
      // walked whole for every decision.
      std::mutex rows_mutex;
      bool need_refresh = true;
      std::vector<String> row_patterns;
      time_t last_read = 0;

      // A row another node added to a shared database is in force here after at
      // most this long; a row added here is in force at once.
      const int RereadSeconds = 300;

      // A table that cannot be read is retried after a minute and reported at
      // most once an hour, as DomainPolicyList does: the ban path asks for the
      // rows, and a database that is refusing must not have every ban queue
      // behind one more failing query, or write an error line each time.
      time_t last_refresh_failure = 0;
      time_t last_refresh_report = 0;

      // ---- the verdicts
      //
      // Lock order: rows_mutex before verdict_mutex, never the other way round.
      std::mutex verdict_mutex;

      struct Kept
      {
         NeverBanNameList::Verdict verdict;
         time_t expires = 0;
         bool used = false;
      };

      std::map<String, Kept> kept;

      // Bumped whenever the rows change, so that a verdict reached under rows
      // that are no longer the list is never kept and never acted on.
      std::atomic<unsigned> generation(0);

      const int KeepSparedSeconds = 3600;
      const int KeepRefusedSeconds = 600;

      // Every SMTP connection from an address the list has rows for leaves a
      // verdict, so the map is held to a size: expired entries go first, and if
      // that is not enough, everything goes - a verdict that is forgotten is
      // only asked again.
      const size_t MaxKept = 4096;

      // ---- the bans waiting for an answer
      //
      // A ban whose reverse lookup was not answered is announced as any ban is
      // and asked about again by the expiry pass, once a minute, at most three
      // times: a resolver that was down for a minute must not leave a listed
      // provider banned for the whole ban.
      std::mutex unverified_mutex;

      struct Unverified
      {
         int attempts = 0;
         time_t due = 0;
      };

      std::map<String, Unverified> unverified;

      const int MaxAttempts = 3;
      const size_t MaxUnverified = 256;

      // The name lookup queue is shared with every SMTP session's own reverse
      // lookup, which degrades to "Unknown" in the Received header when the
      // queue is saturated. A flood of bans must not do that to it: past this
      // many verifications in flight, a ban is announced at once and left to
      // the expiry pass's retries.
      std::atomic<int> in_flight(0);
      const int MaxInFlight = 16;

      const wchar_t *RangeNamePrefix = L"Auto-ban: ";

      boost::asio::ip::address Canonical(const IPAddress &address)
      {
         boost::asio::ip::address value = address.GetAddress();

         if (value.is_v6() && value.to_v6().is_v4_mapped())
            return boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, value.to_v6());

         return value;
      }

      String CanonicalText(const IPAddress &address)
      {
         return String(Canonical(address).to_string().c_str());
      }

      AnsiString JsonText(const String &value)
      {
         AnsiString utf8;
         Unicode::WideToMultiByte(value, utf8);

         AnsiString out;

         for (int i = 0; i < utf8.GetLength(); i++)
         {
            const unsigned char character = (unsigned char) utf8[i];
            char single[2] = { (char) character, 0 };

            switch (character)
            {
            case '"':
               out += "\\\"";
               break;
            case '\\':
               out += "\\\\";
               break;
            case '\n':
               out += "\\n";
               break;
            case '\r':
               out += "\\r";
               break;
            case '\t':
               out += "\\t";
               break;
            default:
               if (character < 0x20)
               {
                  AnsiString escaped;
                  escaped.Format("\\u%04x", (int) character);
                  out += escaped;
               }
               else
               {
                  out += single;
               }
               break;
            }
         }

         return out;
      }

      AnsiString JsonQuoted(const String &value)
      {
         return "\"" + JsonText(value) + "\"";
      }

      const char *JsonFlag(bool value)
      {
         return value ? "true" : "false";
      }

      // A name from somebody's reverse zone, fit for a log line: printable ASCII
      // and no quote, at most 255 characters. The reverse zone's holder chooses
      // these bytes.
      String Printable(const String &name)
      {
         String out;
         const int length = name.GetLength() > 255 ? 255 : name.GetLength();

         for (int i = 0; i < length; i++)
         {
            const wchar_t c = name.GetAt(i);
            out += (c < 0x21 || c > 0x7E || c == L'"' || c == L'\\') ? L'?' : c;
         }

         return out;
      }
   }

   // A ban written while nothing was known about its address: resolved here, on
   // the name lookup queue, and then lifted or announced.
   //
   // Made only after in_flight has been raised for it, and gives that back when
   // it is destroyed rather than when it has run: a task the queue drops unrun -
   // the servers stopped with it still waiting, or a queue that refused it - is
   // destroyed all the same, so that the count of verifications in flight never
   // keeps one that will not finish, and sixteen such never leave every later
   // ban announced unverified.
   class NeverBanVerifyTask : public Task
   {
   public:
      NeverBanVerifyTask(const IPAddress &address, const std::function<void()> &announce) :
         Task("NeverBanNameList::VerifyBan"),
         address_(address),
         announce_(announce)
      {
      }

      virtual ~NeverBanVerifyTask()
      {
         in_flight--;
      }

      virtual void DoWork()
      {
         bool announced = false;

         try
         {
            const unsigned started = NeverBanNameList::Generation_();
            const NeverBanNameList::Verdict verdict = NeverBanNameList::Resolve_(address_);

            NeverBanNameList::Keep_(address_, verdict, started);

            // A verdict reached under rows that changed while it was looked up is
            // not acted on: the ban stands, and is announced, as it would have
            // been with no list.
            if (verdict.spared && started == NeverBanNameList::Generation_())
            {
               NeverBanNameList::Lift_(address_, verdict);
            }
            else
            {
               announced = true;
               announce_();

               if (!verdict.definitive)
                  NeverBanNameList::Unverified_(address_, 0);
            }
         }
         catch (...)
         {
            // The ban is in the table whatever happened here; what must not be
            // lost is the rest of it.
            if (!announced)
               announce_();
         }
      }

   private:
      IPAddress address_;
      std::function<void()> announce_;
   };

   // A ban written for an address already known to be spared - a writer raced
   // the session's own lookup - taken back off the writer's thread: the lift
   // reads every range and tells the firewall, which a spam trap's RCPT TO must
   // not wait for.
   class NeverBanLiftTask : public Task
   {
   public:
      NeverBanLiftTask(const IPAddress &address, const NeverBanNameList::Verdict &verdict) :
         Task("NeverBanNameList::Lift"),
         address_(address),
         verdict_(verdict)
      {
      }

      virtual void DoWork()
      {
         try
         {
            NeverBanNameList::Lift_(address_, verdict_);
         }
         catch (...)
         {
            // The range stays, and expires on its own; the address is not
            // announced, so nothing below the server was told about it.
            LOG_DEBUG(_T("NeverBanNameList - the ban of ") + CanonicalText(address_) + _T(" could not be lifted; it expires on its own."));
         }
      }

   private:
      IPAddress address_;
      NeverBanNameList::Verdict verdict_;
   };

   // A ban announced while its address's reverse lookup was not answered, asked
   // about again by the expiry pass. Gives its place in in_flight back when it is
   // destroyed, run or not, as NeverBanVerifyTask does.
   class NeverBanRetryTask : public Task
   {
   public:
      NeverBanRetryTask(const IPAddress &address, int attempts) :
         Task("NeverBanNameList::RetryUnverified"),
         address_(address),
         attempts_(attempts)
      {
      }

      virtual ~NeverBanRetryTask()
      {
         in_flight--;
      }

      virtual void DoWork()
      {
         try
         {
            const unsigned started = NeverBanNameList::Generation_();
            const NeverBanNameList::Verdict verdict = NeverBanNameList::Resolve_(address_);

            if (!verdict.definitive)
            {
               if (attempts_ + 1 < MaxAttempts)
               {
                  NeverBanNameList::Unverified_(address_, attempts_ + 1);
               }
               else
               {
                  LOG_DEBUG(_T("NeverBanNameList - the reverse lookup of ") + CanonicalText(address_) +
                            _T(" was not answered after ") + StringParser::IntToString(MaxAttempts + 1) +
                            _T(" attempts; its ban stands."));
               }
            }
            else
            {
               NeverBanNameList::Keep_(address_, verdict, started);

               if (verdict.spared && started == NeverBanNameList::Generation_())
                  NeverBanNameList::Lift_(address_, verdict);
            }
         }
         catch (...)
         {
            // The ban was announced when it was placed and stands until it expires.
            LOG_DEBUG(_T("NeverBanNameList - the ban of ") + CanonicalText(address_) + _T(" could not be asked about again; it stands."));
         }
      }

   private:
      IPAddress address_;
      int attempts_;
   };

   // ---- the rows -------------------------------------------------------------

   void
   NeverBanNameList::SetNeedRefresh()
   {
      {
         std::lock_guard<std::mutex> guard(rows_mutex);
         need_refresh = true;
      }

      Forget_();
   }

   bool
   NeverBanNameList::Refresh_()
   {
      // Called with rows_mutex held.
      NeverBanNames all;

      const time_t now = time(nullptr);

      if (!all.Refresh())
      {
         // Keep what is here: replacing the rows with none would ban a listed
         // provider for as long as the database was unwell.
         last_refresh_failure = now;

         if (last_refresh_report == 0 || now - last_refresh_report > 3600)
         {
            last_refresh_report = now;
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7640, "NeverBanNameList::Refresh_",
               "The never-ban list by reverse name could not be read. The rows last read are still in force; a row added or removed since is not in effect yet.");
         }

         return false;
      }

      std::vector<String> fresh;

      for (std::shared_ptr<NeverBanName> row : all.GetSnapshot())
      {
         if (row && !row->GetPattern().IsEmpty())
            fresh.push_back(row->GetPattern());
      }

      // A periodic re-read that finds the rows changed - another node, or a
      // statement run against the table by hand - forgets every verdict, as a
      // change made here does.
      const bool changed = fresh != row_patterns;

      row_patterns.swap(fresh);
      need_refresh = false;
      last_read = now;
      last_refresh_failure = 0;

      if (changed)
         Forget_();

      return true;
   }

   bool
   NeverBanNameList::EnsureRows_(std::vector<String> &patterns)
   {
      std::lock_guard<std::mutex> guard(rows_mutex);

      const time_t now = time(nullptr);

      const bool due = need_refresh || last_read == 0 || now - last_read >= RereadSeconds;
      const bool backingOff = last_refresh_failure != 0 && now - last_refresh_failure < 60;

      if (due && !backingOff)
         Refresh_();

      patterns = row_patterns;

      return !patterns.empty();
   }

   bool
   NeverBanNameList::HasEntries()
   {
      std::vector<String> patterns;
      return EnsureRows_(patterns);
   }

   std::vector<String>
   NeverBanNameList::Patterns()
   {
      std::vector<String> patterns;
      EnsureRows_(patterns);
      return patterns;
   }

   AnsiString
   NeverBanNameList::Document(bool &readable)
   {
      NeverBanNames all;
      readable = all.Refresh();

      AnsiString json = "{\"items\":[";
      int count = 0;

      if (readable)
      {
         for (std::shared_ptr<NeverBanName> row : all.GetSnapshot())
         {
            if (!row)
               continue;

            AnsiString item;
            item.Format("%s{\"id\":%I64d,\"pattern\":\"%s\",\"note\":\"%s\",\"created\":\"%s\"}",
               count > 0 ? "," : "",
               row->GetID(),
               JsonText(row->GetPattern()).c_str(),
               JsonText(row->GetNote()).c_str(),
               JsonText(Time::GetTimeStampFromDateTime(row->GetCreated())).c_str());

            json += item;
            count++;
         }
      }

      json += "]}";

      return json;
   }

   bool
   NeverBanNameList::Add(const String &pattern, const String &note, __int64 &id, String &refusal)
   {
      id = 0;
      refusal.Empty();

      String text = note;
      text.Trim();

      std::shared_ptr<NeverBanName> row(new NeverBanName);
      row->SetPattern(pattern);
      row->SetNote(text);

      String error;
      if (!PersistentNeverBanName::SaveObject(row, error, PersistenceModeNormal))
      {
         refusal = error.IsEmpty() ? String(_T("the entry could not be saved; see the error log")) : error;
         return false;
      }

      id = row->GetID();

      LOG_APPLICATION(_T("Never-ban list: ") + row->GetPattern() +
                      _T(" added. An address whose reverse name it covers, and which that name resolves back to, is not auto-banned."));

      return true;
   }

   bool
   NeverBanNameList::Remove(__int64 id, bool &found)
   {
      found = false;

      NeverBanNames all;
      if (!all.Refresh())
         return false;

      std::shared_ptr<NeverBanName> row = all.GetItemByDBID(id);
      if (!row)
         return true;

      found = true;

      if (!PersistentNeverBanName::DeleteObject(row))
         return false;

      LOG_APPLICATION(_T("Never-ban list: ") + row->GetPattern() + _T(" removed."));

      return true;
   }

   // ---- the verdicts ----------------------------------------------------------

   unsigned
   NeverBanNameList::Generation_()
   {
      return generation.load();
   }

   String
   NeverBanNameList::Key_(const IPAddress &address)
   {
      return CanonicalText(address);
   }

   void
   NeverBanNameList::Forget_()
   {
      std::lock_guard<std::mutex> guard(verdict_mutex);
      generation++;
      kept.clear();
   }

   size_t
   NeverBanNameList::KnownCount_()
   {
      std::lock_guard<std::mutex> guard(verdict_mutex);
      return kept.size();
   }

   void
   NeverBanNameList::Keep_(const IPAddress &address, const Verdict &verdict, unsigned startedUnder)
   {
      if (!verdict.definitive)
         return;

      std::lock_guard<std::mutex> guard(verdict_mutex);

      if (startedUnder != generation.load())
         return;

      const time_t now = time(nullptr);

      if (kept.size() >= MaxKept)
      {
         for (auto it = kept.begin(); it != kept.end();)
         {
            if (it->second.expires <= now)
               it = kept.erase(it);
            else
               ++it;
         }

         if (kept.size() >= MaxKept)
            kept.clear();
      }

      Kept entry;
      entry.verdict = verdict;
      entry.verdict.first_use = false;
      entry.expires = now + (verdict.spared ? KeepSparedSeconds : KeepRefusedSeconds);
      entry.used = false;

      kept[Key_(address)] = entry;
   }

   bool
   NeverBanNameList::Known_(const IPAddress &address, Verdict &verdict, bool markUsed)
   {
      std::lock_guard<std::mutex> guard(verdict_mutex);

      auto it = kept.find(Key_(address));
      if (it == kept.end())
         return false;

      if (it->second.expires <= time(nullptr))
      {
         kept.erase(it);
         return false;
      }

      verdict = it->second.verdict;
      verdict.first_use = !it->second.used;

      if (markUsed)
         it->second.used = true;

      return true;
   }

   // ---- the ban writers ---------------------------------------------------------

   bool
   NeverBanNameList::IsSpared(const IPAddress &address, Verdict &verdict)
   {
      Verdict known;
      if (!Known_(address, known, true))
         return false;

      if (!known.spared)
         return false;

      verdict = known;
      return true;
   }

   void
   NeverBanNameList::VerifyBan(const IPAddress &address, const std::function<void()> &announce)
   {
      std::vector<String> patterns;
      if (!EnsureRows_(patterns))
      {
         announce();
         return;
      }

      std::shared_ptr<WorkQueue> queue = Application::Instance()->GetNameLookupWorkQueue();

      Verdict known;
      if (Known_(address, known, false))
      {
         // A ban written although the address is known to be spared: the
         // session's own lookup finished between the writer's IsSpared and the
         // write. Taken back, and never announced - off this thread when there
         // is a queue to take it.
         if (known.spared)
         {
            if (queue)
               queue->AddTask(std::shared_ptr<Task>(new NeverBanLiftTask(address, known)));
            else
               Lift_(address, known);
            return;
         }

         announce();
         return;
      }

      if (!queue || in_flight.load() >= MaxInFlight)
      {
         announce();
         Unverified_(address, 0);
         return;
      }

      in_flight++;
      queue->AddTask(std::shared_ptr<Task>(new NeverBanVerifyTask(address, announce)), WorkQueue::TaskMayBlock);
   }

   void
   NeverBanNameList::Learn(const IPAddress &address, const std::vector<String> &reverseNames, bool answered)
   {
      std::vector<String> patterns;
      if (!EnsureRows_(patterns))
         return;

      Verdict known;
      if (Known_(address, known, false))
         return;

      const unsigned started = Generation_();

      Verdict verdict = Evaluate(address, reverseNames, answered, patterns, &NeverBanNameList::RealForwardLookup_);

      Keep_(address, verdict, started);

      if (verdict.spared)
      {
         LOG_DEBUG(_T("NeverBanNameList - ") + CanonicalText(address) + _T(" is spared from auto-ban: ") + verdict.reason);
      }
   }

   void
   NeverBanNameList::Unverified_(const IPAddress &address, int attempts)
   {
      std::lock_guard<std::mutex> guard(unverified_mutex);

      if (unverified.size() >= MaxUnverified && unverified.find(Key_(address)) == unverified.end())
      {
         LOG_DEBUG(_T("NeverBanNameList - too many bans are waiting for a reverse lookup; the ban of ") + CanonicalText(address) +
                   _T(" stands without being asked about again."));
         return;
      }

      Unverified entry;
      entry.attempts = attempts;
      entry.due = time(nullptr) + 60;

      unverified[Key_(address)] = entry;
   }

   void
   NeverBanNameList::RetryUnverified()
   {
      std::vector<std::pair<String, int> > due;

      {
         std::lock_guard<std::mutex> guard(unverified_mutex);

         if (unverified.empty())
            return;

         const time_t now = time(nullptr);

         for (auto it = unverified.begin(); it != unverified.end();)
         {
            if (it->second.due <= now)
            {
               due.push_back(std::make_pair(it->first, it->second.attempts));
               it = unverified.erase(it);
            }
            else
               ++it;
         }
      }

      if (due.empty())
         return;

      std::shared_ptr<WorkQueue> queue = Application::Instance()->GetNameLookupWorkQueue();

      for (const std::pair<String, int> &entry : due)
      {
         IPAddress address;
         if (!address.TryParse(AnsiString(entry.first), false))
            continue;

         if (!queue || in_flight.load() >= MaxInFlight)
         {
            // Not an attempt: put back for the next minute as it was.
            Unverified_(address, entry.second);
            continue;
         }

         in_flight++;
         queue->AddTask(std::shared_ptr<Task>(new NeverBanRetryTask(address, entry.second)), WorkQueue::TaskMayBlock);
      }
   }

   bool
   NeverBanNameList::RealForwardLookup_(const String &name, bool ipv6, std::vector<String> &addresses)
   {
      // The resource-record numbers of RFC 1035 and RFC 3596, as SPFDnsResolver
      // writes them: the numbers on the wire, the same on every platform.
      const int ResourceTypeA = 0x0001;
      const int ResourceTypeAAAA = 0x001c;

      DNSResolver resolver;
      std::vector<AnsiString> values;

      if (!resolver.GetRecordsOfType(AnsiString(name), ipv6 ? ResourceTypeAAAA : ResourceTypeA, values))
         return false;

      for (const AnsiString &value : values)
         addresses.push_back(String(value));

      return true;
   }

   NeverBanNameList::Verdict
   NeverBanNameList::Resolve_(const IPAddress &address)
   {
      std::vector<String> patterns;
      if (!EnsureRows_(patterns))
      {
         Verdict empty;
         empty.definitive = true;
         empty.reason = _T("the never-ban list is empty");
         return empty;
      }

      std::vector<String> names;
      DNSResolver resolver;
      const bool answered = resolver.GetPTRRecords(CanonicalText(address), names);

      return Evaluate(address, names, answered, patterns, &NeverBanNameList::RealForwardLookup_);
   }

   bool
   NeverBanNameList::Lift_(const IPAddress &address, const Verdict &verdict)
   {
      const boost::asio::ip::address target = Canonical(address);
      const String text = CanonicalText(address);

      SecurityRanges ranges;
      if (!ranges.Refresh())
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7641, "NeverBanNameList::Lift_",
            "The auto-ban of " + text + " could not be lifted although the never-ban list spares it (" + verdict.reason +
            "): the IP ranges could not be read. Delete its \"Auto-ban:\" range on the IP ranges page to lift it now.");
         return false;
      }

      int lifted = 0;
      bool failed = false;

      for (std::shared_ptr<SecurityRange> range : ranges.GetSnapshot())
      {
         // Only what an auto-ban writes: a single address, expiring, under the
         // name prefix AutoBanFirewall mirrors. A range an administrator wrote
         // is theirs, whatever it covers.
         if (!range || !range->GetExpires())
            continue;

         if (!range->GetName().StartsWith(RangeNamePrefix))
            continue;

         if (Canonical(range->GetLowerIP()) != target || Canonical(range->GetUpperIP()) != target)
            continue;

         // Deleting the range tells the firewall (PersistentSecurityRange::DeleteObject).
         if (PersistentSecurityRange::DeleteObject(range))
            lifted++;
         else
            failed = true;
      }

      if (failed)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7641, "NeverBanNameList::Lift_",
            "The auto-ban of " + text + " could not be lifted although the never-ban list spares it (" + verdict.reason +
            "): its range could not be deleted. Delete its \"Auto-ban:\" range on the IP ranges page to lift it now.");
      }

      // Not the shape the packaged fail2ban filter matches ("is banned for"),
      // on purpose: nothing below the server is to act on this address.
      if (lifted > 0)
      {
         LOG_APPLICATION(_T("Auto-ban: ") + text + _T(" was not banned: ") + verdict.reason + _T("; the ban written for it was lifted."));
      }

      return lifted > 0;
   }

   // ---- diagnosis -------------------------------------------------------------

   bool
   NeverBanNameList::Check(const String &addressText, AnsiString &document, String &refusal)
   {
      String text = addressText;
      text.Trim();

      IPAddress address;
      if (text.IsEmpty() || !address.TryParse(AnsiString(text), false))
      {
         refusal = _T("address must be an IPv4 or IPv6 address, such as 192.0.2.10 or 2001:db8::25");
         return false;
      }

      std::vector<String> patterns;
      EnsureRows_(patterns);

      std::vector<String> names;
      DNSResolver resolver;
      const bool answered = resolver.GetPTRRecords(CanonicalText(address), names);

      // Each name's forward lookup made once, for the verdict and for the list
      // below alike: a check is answered while a person or a REST client waits.
      std::map<String, std::pair<bool, std::vector<String> > > looked;
      ForwardLookup once = [&looked](const String &name, bool ipv6, std::vector<String> &addresses) -> bool
      {
         auto it = looked.find(name);
         if (it == looked.end())
         {
            std::vector<String> found;
            const bool ok = RealForwardLookup_(name, ipv6, found);
            it = looked.insert(std::make_pair(name, std::make_pair(ok, found))).first;
         }

         addresses = it->second.second;
         return it->second.first;
      };

      Verdict verdict;
      if (patterns.empty())
      {
         verdict.definitive = true;
         verdict.reason = _T("the never-ban list is empty");
      }
      else
      {
         verdict = Evaluate(address, names, answered, patterns, once);
      }

      // Every reverse name, with what the list makes of it - including whether it
      // resolves back to the address, looked up whether a row covers it or not,
      // since that is what tells an administrator whether a row WOULD work.
      const boost::asio::ip::address canonical = Canonical(address);
      const bool ipv6 = canonical.is_v6();

      AnsiString list = "[";
      int count = 0;
      int lookups = 0;

      for (const String &raw : names)
      {
         if (count >= MaxReverseNames)
            break;

         const String name = NormaliseName(raw);
         const bool usable = IsUsableReverseName(name);
         const String pattern = usable ? CoveringPattern(name, patterns) : String();

         AnsiString confirmed = "null";
         if (usable && (looked.count(name) > 0 || lookups < MaxForwardLookups))
         {
            if (looked.count(name) == 0)
               lookups++;
            std::vector<String> forward;
            if (once(name, ipv6, forward))
            {
               bool same = false;
               for (const String &answer : forward)
               {
                  if (SameAddress(address, answer))
                     same = true;
               }
               confirmed = same ? "true" : "false";
            }
         }

         AnsiString item;
         item.Format("%s{\"name\":%s,\"usable\":%s,\"pattern\":%s,\"resolves_back\":%s}",
            count > 0 ? "," : "",
            JsonQuoted(name).c_str(),
            JsonFlag(usable),
            pattern.IsEmpty() ? "null" : JsonQuoted(pattern).c_str(),
            confirmed.c_str());

         list += item;
         count++;
      }

      list += "]";

      document.Format("{\"address\":%s,\"reverse_names\":%s,\"answered\":%s,\"spared\":%s,\"name\":%s,\"pattern\":%s,\"reason\":%s}",
         JsonQuoted(CanonicalText(address)).c_str(),
         list.c_str(),
         JsonFlag(answered),
         JsonFlag(verdict.spared),
         verdict.name.IsEmpty() ? "null" : JsonQuoted(verdict.name).c_str(),
         verdict.pattern.IsEmpty() ? "null" : JsonQuoted(verdict.pattern).c_str(),
         JsonQuoted(verdict.reason).c_str());

      return true;
   }

   // ---- the rules ---------------------------------------------------------------

   String
   NeverBanNameList::NormaliseName(const String &name)
   {
      String value = name;
      value.Trim();
      value.ToLower();

      while (value.GetLength() > 0 && value.EndsWith(_T(".")))
         value = value.Left(value.GetLength() - 1);

      return value;
   }

   bool
   NeverBanNameList::IsUsableReverseName(const String &normalisedName)
   {
      if (normalisedName.IsEmpty() || normalisedName.GetLength() > 255)
         return false;

      // A host name is printable ASCII. Anything else - a control character, a
      // quote, a byte a reverse zone's holder put there - is matched by nothing
      // and never reaches a log line as it came.
      for (int i = 0; i < normalisedName.GetLength(); i++)
      {
         const wchar_t c = normalisedName.GetAt(i);
         if (c < 0x21 || c > 0x7E || c == L'"' || c == L'\\')
            return false;
      }

      // A reverse zone that answers with an address written as a name - some do
      // - names nobody.
      if (StringParser::IsValidIPAddress(normalisedName))
         return false;

      if (normalisedName.StartsWith(_T(".")) || normalisedName.Find(_T("..")) >= 0)
         return false;

      if (normalisedName.Find(_T(" ")) >= 0 || normalisedName.Find(_T("@")) >= 0 ||
          normalisedName.Find(_T("[")) >= 0 || normalisedName.Find(_T("*")) >= 0)
         return false;

      // localhost, a top-level domain, co.uk: a name nobody registers, so no
      // pattern may spare it and none can.
      if (NeverBanName::IsPublicSuffix(normalisedName))
         return false;

      return true;
   }

   String
   NeverBanNameList::CoveringPattern(const String &normalisedName, const std::vector<String> &patterns)
   {
      String best;
      bool bestIsSuffix = false;

      for (const String &pattern : patterns)
      {
         if (!NeverBanName::PatternCovers(pattern, normalisedName))
            continue;

         const bool suffix = pattern.StartsWith(_T("."));

         if (best.IsEmpty() ||
             (bestIsSuffix && !suffix) ||
             (bestIsSuffix == suffix && pattern.GetLength() > best.GetLength()))
         {
            best = pattern;
            bestIsSuffix = suffix;
         }
      }

      return best;
   }

   bool
   NeverBanNameList::SameAddress(const IPAddress &address, const String &answer)
   {
      String text = answer;
      text.Trim();

      IPAddress parsed;
      if (text.IsEmpty() || !parsed.TryParse(AnsiString(text), false))
         return false;

      return Canonical(address) == Canonical(parsed);
   }

   NeverBanNameList::Verdict
   NeverBanNameList::Evaluate(const IPAddress &address, const std::vector<String> &reverseNames, bool answered,
                              const std::vector<String> &patterns, const ForwardLookup &forward)
   {
      Verdict verdict;

      if (patterns.empty())
      {
         verdict.definitive = true;
         verdict.reason = _T("the never-ban list is empty");
         return verdict;
      }

      if (!answered)
      {
         verdict.definitive = false;
         verdict.reason = _T("its reverse lookup was not answered");
         return verdict;
      }

      if (reverseNames.empty())
      {
         verdict.definitive = true;
         verdict.reason = _T("it has no reverse name");
         return verdict;
      }

      const bool ipv6 = Canonical(address).is_v6();

      bool allAnswered = true;
      int lookups = 0;
      int looked = 0;

      String firstUsable;
      String unusable;
      String nearName;
      String nearPattern;
      bool nearUnanswered = false;

      for (const String &raw : reverseNames)
      {
         if (looked >= MaxReverseNames)
            break;

         looked++;

         const String name = NormaliseName(raw);

         if (!IsUsableReverseName(name))
         {
            if (unusable.IsEmpty())
               unusable = name;
            continue;
         }

         if (firstUsable.IsEmpty())
            firstUsable = name;

         const String pattern = CoveringPattern(name, patterns);
         if (pattern.IsEmpty())
            continue;

         if (lookups >= MaxForwardLookups)
         {
            allAnswered = false;
            continue;
         }

         lookups++;

         std::vector<String> addresses;
         if (!forward || !forward(name, ipv6, addresses))
         {
            allAnswered = false;

            if (nearName.IsEmpty())
            {
               nearName = name;
               nearPattern = pattern;
               nearUnanswered = true;
            }

            continue;
         }

         for (const String &answer : addresses)
         {
            if (!SameAddress(address, answer))
               continue;

            verdict.spared = true;
            verdict.definitive = true;
            verdict.name = name;
            verdict.pattern = pattern;
            verdict.reason = _T("its reverse name ") + name + _T(" resolves back to it and is covered by ") + pattern +
                             _T(" on the never-ban list");
            return verdict;
         }

         if (nearName.IsEmpty() || nearUnanswered)
         {
            nearName = name;
            nearPattern = pattern;
            nearUnanswered = false;
         }
      }

      verdict.definitive = allAnswered;
      verdict.name = nearName.IsEmpty() ? firstUsable : nearName;
      verdict.pattern = nearPattern;

      if (!nearName.IsEmpty() && !nearUnanswered)
      {
         verdict.reason = _T("its reverse name ") + nearName + _T(" is covered by ") + nearPattern +
                          _T(" on the never-ban list but does not resolve back to it, so anybody could have written it");
      }
      else if (!nearName.IsEmpty())
      {
         verdict.reason = _T("its reverse name ") + nearName + _T(" is covered by ") + nearPattern +
                          _T(" on the never-ban list but its forward lookup was not answered");
      }
      else if (!firstUsable.IsEmpty())
      {
         verdict.reason = _T("its reverse name ") + firstUsable + _T(" is not covered by the never-ban list");
      }
      else
      {
         verdict.reason = _T("its reverse name ") + Printable(unusable) + _T(" is an address, a public suffix or not a host name, which the never-ban list never matches");
      }

      return verdict;
   }

   // ---- the tester ----------------------------------------------------------------

   namespace
   {
      void Expect(bool condition, const char *what)
      {
         if (!condition)
            throw std::logic_error(what);
      }

      IPAddress Address(const char *text)
      {
         IPAddress address;
         if (!address.TryParse(AnsiString(text), false))
            throw std::logic_error("NeverBanNameListTester: a test address did not parse.");
         return address;
      }

      // A forward zone of the tester's own: name -> addresses, and the names
      // whose lookups fail.
      struct FakeZone
      {
         std::map<String, std::vector<String> > a;
         std::map<String, std::vector<String> > aaaa;
         std::vector<String> failing;
         int asked = 0;

         NeverBanNameList::ForwardLookup Lookup()
         {
            return [this](const String &name, bool ipv6, std::vector<String> &out) -> bool
            {
               asked++;

               for (const String &bad : failing)
               {
                  if (bad == name)
                     return false;
               }

               const std::map<String, std::vector<String> > &zone = ipv6 ? aaaa : a;
               auto it = zone.find(name);
               if (it != zone.end())
                  out = it->second;
               return true;
            };
         }
      };
   }

   void
   NeverBanNameListTester::Test()
   {
      // ---- patterns: what may be listed
      String refusal;

      Expect(NeverBanName::ValidatePattern(_T("mail.example.net"), refusal), "A whole host name was refused as a pattern.");
      Expect(NeverBanName::ValidatePattern(_T(".example.net"), refusal), "A suffix was refused as a pattern.");
      Expect(NeverBanName::ValidatePattern(_T(" .Example.NET. "), refusal), "A suffix in another case, with spaces and the root's dot, was refused.");
      Expect(NeverBanName::ValidatePattern(_T(".outbound.protection.outlook.com"), refusal), "A deep suffix was refused.");

      Expect(!NeverBanName::ValidatePattern(_T(""), refusal), "An empty pattern was accepted.");
      Expect(!NeverBanName::ValidatePattern(_T("."), refusal), "A lone dot was accepted.");
      Expect(!NeverBanName::ValidatePattern(_T(".com"), refusal), "The public suffix .com was accepted.");
      Expect(!NeverBanName::ValidatePattern(_T("co.uk"), refusal), "The public suffix co.uk was accepted.");
      Expect(!NeverBanName::ValidatePattern(_T(".github.io"), refusal), "The private-section suffix .github.io was accepted.");
      Expect(!NeverBanName::ValidatePattern(_T("localhost"), refusal), "A single label was accepted.");
      Expect(NeverBanName::ValidatePattern(_T("example.co.uk"), refusal), "A registrable domain under co.uk was refused - the negative control of the suffix rule.");
      Expect(!NeverBanName::ValidatePattern(_T("*.example.net"), refusal), "A star wildcard was accepted.");
      Expect(refusal.Find(_T(".example.net")) >= 0, "The star's refusal does not say how to write the wildcard.");
      Expect(!NeverBanName::ValidatePattern(_T("192.0.2.1"), refusal), "An IPv4 address was accepted as a name.");
      Expect(!NeverBanName::ValidatePattern(_T("2001:db8::1"), refusal), "An IPv6 address was accepted as a name.");
      Expect(!NeverBanName::ValidatePattern(_T("[192.0.2.1]"), refusal), "An address literal was accepted.");
      Expect(!NeverBanName::ValidatePattern(_T("192.0.2.0/24"), refusal), "A network was accepted.");
      Expect(!NeverBanName::ValidatePattern(_T("postmaster@example.net"), refusal), "An email address was accepted.");
      Expect(!NeverBanName::ValidatePattern(_T("mail example.net"), refusal), "A pattern with a space was accepted.");
      Expect(!NeverBanName::ValidatePattern(_T("..example.net"), refusal), "A pattern with an empty label was accepted.");
      Expect(!NeverBanName::ValidatePattern(_T("-bad.example.net"), refusal), "A label starting with a hyphen was accepted.");

      Expect(NeverBanName::NormalisePattern(_T(" .Example.NET.. ")) == _T(".example.net"), "A pattern was not normalised.");

      // ---- covering: the domain list's rules, at a label boundary
      Expect(NeverBanName::PatternCovers(_T(".example.net"), _T("mail5.outbound.example.net")), "A suffix did not cover a name beneath it.");
      Expect(NeverBanName::PatternCovers(_T(".example.net"), _T("example.net")), "A suffix did not cover its own apex.");
      Expect(!NeverBanName::PatternCovers(_T(".example.net"), _T("notexample.net")), "A suffix covered a name across a label boundary.");
      Expect(!NeverBanName::PatternCovers(_T(".example.net"), _T("example.net.attacker.test")), "A suffix covered a name that merely contains it.");
      Expect(NeverBanName::PatternCovers(_T("mail.example.net"), _T("mail.example.net")), "A whole name did not cover itself.");
      Expect(!NeverBanName::PatternCovers(_T("mail.example.net"), _T("x.mail.example.net")), "A whole name covered a name beneath it.");
      Expect(!NeverBanName::PatternCovers(_T("example.net"), _T("mail.example.net")), "A whole domain covered a host beneath it - that is what the leading dot is for.");

      // ---- reverse names that may be matched
      Expect(NeverBanNameList::NormaliseName(_T(" Mail.Example.NET. ")) == _T("mail.example.net"), "A reverse name was not normalised.");
      Expect(NeverBanNameList::IsUsableReverseName(_T("mail.example.net")), "An ordinary reverse name was not usable.");
      Expect(!NeverBanNameList::IsUsableReverseName(_T("")), "An empty reverse name was usable.");
      Expect(!NeverBanNameList::IsUsableReverseName(_T("localhost")), "localhost was usable.");
      Expect(!NeverBanNameList::IsUsableReverseName(_T("co.uk")), "A public suffix was usable.");
      Expect(!NeverBanNameList::IsUsableReverseName(_T("192.0.2.9")), "An address written as a reverse name was usable.");
      Expect(!NeverBanNameList::IsUsableReverseName(_T("mail..example.net")), "A reverse name with an empty label was usable.");
      Expect(!NeverBanNameList::IsUsableReverseName(_T("mail\r\nforged.example.net")), "A reverse name with a line break was usable.");
      Expect(!NeverBanNameList::IsUsableReverseName(_T("mail\"x.example.net")), "A reverse name with a quote was usable.");
      Expect(NeverBanNameList::IsUsableReverseName(_T("mail_relay.example.net")), "A reverse name with an underscore, which real reverse zones carry, was not usable.");

      // ---- the most specific pattern
      std::vector<String> patterns;
      patterns.push_back(_T(".example.net"));
      patterns.push_back(_T(".outbound.example.net"));
      patterns.push_back(_T("mail5.outbound.example.net"));

      Expect(NeverBanNameList::CoveringPattern(_T("mail5.outbound.example.net"), patterns) == _T("mail5.outbound.example.net"), "A whole name did not beat the suffixes covering it.");
      Expect(NeverBanNameList::CoveringPattern(_T("mail6.outbound.example.net"), patterns) == _T(".outbound.example.net"), "The longer suffix did not win.");
      Expect(NeverBanNameList::CoveringPattern(_T("www.example.net"), patterns) == _T(".example.net"), "The only covering suffix was not chosen.");
      Expect(NeverBanNameList::CoveringPattern(_T("mail.example.org"), patterns).IsEmpty(), "A pattern was found for a name nothing covers.");

      // ---- the same address
      const IPAddress v4 = Address("203.0.113.14");
      const IPAddress v6 = Address("2001:db8::25");
      const IPAddress mapped = Address("::ffff:203.0.113.14");

      Expect(NeverBanNameList::SameAddress(v4, _T("203.0.113.14")), "An address was not its own forward answer.");
      Expect(!NeverBanNameList::SameAddress(v4, _T("203.0.113.15")), "A neighbour was taken for the address.");
      Expect(NeverBanNameList::SameAddress(v6, _T("2001:DB8:0:0::25")), "An IPv6 address in another spelling was not the same.");
      Expect(!NeverBanNameList::SameAddress(v6, _T("2001:db8::26")), "Another IPv6 address was taken for the address.");
      Expect(NeverBanNameList::SameAddress(mapped, _T("203.0.113.14")), "A mapped IPv4 address was not its IPv4 answer.");
      Expect(!NeverBanNameList::SameAddress(v4, _T("not an address")), "Text that is not an address matched.");

      // ---- the decision
      FakeZone zone;
      zone.a[_T("mail5.outbound.example.net")].push_back(_T("203.0.113.14"));
      zone.a[_T("spoofed.example.net")].push_back(_T("198.51.100.1"));
      zone.aaaa[_T("mx.example.net")].push_back(_T("2001:db8::25"));
      zone.failing.push_back(_T("broken.example.net"));

      std::vector<String> list;
      list.push_back(_T(".example.net"));

      std::vector<String> names;
      names.push_back(_T("MAIL5.outbound.example.net."));

      NeverBanNameList::Verdict verdict = NeverBanNameList::Evaluate(v4, names, true, list, zone.Lookup());
      Expect(verdict.spared && verdict.definitive, "A covered, forward-confirmed reverse name did not spare its address.");
      Expect(verdict.name == _T("mail5.outbound.example.net") && verdict.pattern == _T(".example.net"), "The verdict did not name the reverse name and the pattern.");

      // Negative controls of the same case, one change each.
      std::vector<String> otherList;
      otherList.push_back(_T(".example.org"));
      verdict = NeverBanNameList::Evaluate(v4, names, true, otherList, zone.Lookup());
      Expect(!verdict.spared && verdict.definitive, "An address was spared by a list that does not cover its reverse name.");

      verdict = NeverBanNameList::Evaluate(Address("203.0.113.99"), names, true, list, zone.Lookup());
      Expect(!verdict.spared && verdict.definitive, "A covered reverse name that resolves to another address spared this one.");
      Expect(verdict.reason.Find(_T("does not resolve back")) >= 0, "The refusal does not say the name did not resolve back.");

      std::vector<String> spoofed;
      spoofed.push_back(_T("spoofed.example.net"));
      verdict = NeverBanNameList::Evaluate(v4, spoofed, true, list, zone.Lookup());
      Expect(!verdict.spared, "A reverse name its forward zone does not confirm spared the address.");

      std::vector<String> broken;
      broken.push_back(_T("broken.example.net"));
      verdict = NeverBanNameList::Evaluate(v4, broken, true, list, zone.Lookup());
      Expect(!verdict.spared && !verdict.definitive, "A forward lookup that failed was taken as an answer.");

      verdict = NeverBanNameList::Evaluate(v4, names, false, list, zone.Lookup());
      Expect(!verdict.spared && !verdict.definitive, "A reverse lookup that failed was taken as an answer.");

      verdict = NeverBanNameList::Evaluate(v4, std::vector<String>(), true, list, zone.Lookup());
      Expect(!verdict.spared && verdict.definitive, "An address with no reverse name was spared, or its absence not taken as an answer.");

      verdict = NeverBanNameList::Evaluate(v4, names, true, std::vector<String>(), zone.Lookup());
      Expect(!verdict.spared, "An empty list spared an address.");

      // An address written as its own reverse name, and a bare suffix, are never
      // looked up at all.
      std::vector<String> literal;
      literal.push_back(_T("203.0.113.14"));
      literal.push_back(_T("net"));
      zone.asked = 0;
      verdict = NeverBanNameList::Evaluate(v4, literal, true, list, zone.Lookup());
      Expect(!verdict.spared && zone.asked == 0, "An address or a public suffix given as a reverse name was looked up or matched.");

      // A reverse name its zone's holder filled with a line break reaches the
      // reason - which goes into a log line - only as printable text.
      std::vector<String> hostile;
      hostile.push_back(_T("x\r\n\"Auto-ban: 192.0.2.1 is banned for 60 minutes"));
      verdict = NeverBanNameList::Evaluate(v4, hostile, true, list, zone.Lookup());
      Expect(!verdict.spared && verdict.reason.Find(_T("\r")) < 0 && verdict.reason.Find(_T("\n")) < 0 && verdict.reason.Find(_T("\"")) < 0,
             "A reverse name's line break or quote reached the verdict's reason as it came.");

      // Several reverse names: the one that qualifies spares, wherever it is.
      std::vector<String> several;
      several.push_back(_T("customer-14.isp.example"));
      several.push_back(_T("spoofed.example.net"));
      several.push_back(_T("mail5.outbound.example.net"));
      verdict = NeverBanNameList::Evaluate(v4, several, true, list, zone.Lookup());
      Expect(verdict.spared && verdict.name == _T("mail5.outbound.example.net"), "The qualifying reverse name among several did not spare the address.");

      // IPv6: AAAA, not A.
      std::vector<String> v6names;
      v6names.push_back(_T("mx.example.net"));
      verdict = NeverBanNameList::Evaluate(v6, v6names, true, list, zone.Lookup());
      Expect(verdict.spared, "An IPv6 address confirmed by its AAAA record was not spared.");
      zone.aaaa.clear();
      zone.a[_T("mx.example.net")].push_back(_T("2001:db8::25"));
      verdict = NeverBanNameList::Evaluate(v6, v6names, true, list, zone.Lookup());
      Expect(!verdict.spared, "An IPv6 address was confirmed by an A lookup.");

      // A mapped IPv4 peer is confirmed by A.
      verdict = NeverBanNameList::Evaluate(mapped, names, true, list, zone.Lookup());
      Expect(verdict.spared, "A mapped IPv4 peer was not confirmed by its A record.");

      // The forward lookups one verdict may cost are bounded.
      std::vector<String> many;
      for (int i = 0; i < 20; i++)
         many.push_back(_T("host") + StringParser::IntToString(i) + _T(".example.net"));
      zone.asked = 0;
      verdict = NeverBanNameList::Evaluate(v4, many, true, list, zone.Lookup());
      Expect(zone.asked == NeverBanNameList::MaxForwardLookups, "The forward lookups of one verdict were not bounded.");
      Expect(!verdict.spared && !verdict.definitive, "A verdict that stopped looking was taken as definitive.");

      // ---- the verdict cache, with no database and no DNS
      NeverBanNameList::Forget_();
      Expect(NeverBanNameList::KnownCount_() == 0, "Forget_ left a verdict.");

      NeverBanNameList::Verdict spared;
      spared.spared = true;
      spared.definitive = true;
      spared.name = _T("mail5.outbound.example.net");
      spared.pattern = _T(".example.net");

      const IPAddress cached = Address("192.0.2.200");
      NeverBanNameList::Keep_(cached, spared, NeverBanNameList::Generation_());

      NeverBanNameList::Verdict read;
      Expect(NeverBanNameList::IsSpared(cached, read) && read.first_use, "A kept spared verdict was not read, or not as its first use.");
      Expect(NeverBanNameList::IsSpared(cached, read) && !read.first_use, "A kept verdict's second read was called its first.");
      Expect(!NeverBanNameList::IsSpared(Address("192.0.2.201"), read), "An address nothing is known about was spared.");

      NeverBanNameList::Verdict undecided = spared;
      undecided.definitive = false;
      const IPAddress notKept = Address("192.0.2.202");
      NeverBanNameList::Keep_(notKept, undecided, NeverBanNameList::Generation_());
      Expect(!NeverBanNameList::IsSpared(notKept, read), "A verdict that was not definitive was kept.");

      const unsigned before = NeverBanNameList::Generation_();
      NeverBanNameList::Forget_();
      Expect(!NeverBanNameList::IsSpared(cached, read), "A verdict survived the rows changing.");

      NeverBanNameList::Keep_(cached, spared, before);
      Expect(!NeverBanNameList::IsSpared(cached, read), "A verdict reached under rows that have since changed was kept.");

      NeverBanNameList::Verdict refused;
      refused.definitive = true;
      NeverBanNameList::Keep_(cached, refused, NeverBanNameList::Generation_());
      Expect(!NeverBanNameList::IsSpared(cached, read), "A kept verdict that does not spare was read as sparing.");

      NeverBanNameList::Forget_();

      // ---- a verification or a retry the queue drops unrun gives its place among
      // those in flight back, as VerifyBan and RetryUnverified raise it before
      // they hand one over; one that kept it would, sixteen times over, leave
      // every later ban announced unverified. Nothing is announced for it.
      {
         const int before = in_flight.load();
         bool announced = false;

         in_flight++;
         {
            std::shared_ptr<Task> dropped(new NeverBanVerifyTask(Address("192.0.2.210"), [&announced]() { announced = true; }));
            Expect(in_flight.load() == before + 1, "A verification waiting in the queue was not counted in flight - the negative control.");
         }
         Expect(in_flight.load() == before, "A verification the queue dropped unrun kept its place among those in flight.");
         Expect(!announced, "A verification the queue dropped unrun announced its ban.");

         in_flight++;
         {
            std::shared_ptr<Task> dropped(new NeverBanRetryTask(Address("192.0.2.211"), 0));
            Expect(in_flight.load() == before + 1, "A retry waiting in the queue was not counted in flight - the negative control.");
         }
         Expect(in_flight.load() == before, "A retry the queue dropped unrun kept its place among those in flight.");
      }
   }
}
