// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "DomainPolicyList.h"

#include "../BO/DomainPolicyEntries.h"
#include "../Persistence/PersistentDomainPolicyEntry.h"
#include "../Util/Time.h"
#include "../Util/Unicode.h"
#include "../Util/Parsing/StringParser.h"

#include <ctime>
#include <mutex>
#include <vector>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      std::mutex list_mutex;
      bool need_refresh = true;

      // The rows as last read. Small by construction; walked whole for every
      // decision, which for a handful of rows is cheaper than any index.
      std::vector<std::shared_ptr<DomainPolicyEntry> > entries;

      // A table that cannot be read is reported at most once an hour. Without
      // the ceiling one unreadable table would write an error line per MAIL
      // FROM, which is the shape of failure that fills a disk while somebody is
      // already dealing with a database problem.
      time_t last_refresh_report = 0;

      // When the last read was attempted and failed. The retry backs off with
      // it: Decide() calls Refresh_() under the list mutex, so a database that
      // is refusing would otherwise have every MAIL FROM of every session queue
      // behind one more failing query.
      time_t last_refresh_failure = 0;

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

      // Whether candidate is a more specific match than the one held: a whole
      // domain beats a suffix, then the longer pattern wins, then black wins a
      // dead heat, so that a white and a black row covering exactly the same
      // name refuse rather than accept. The whole-domain test comes FIRST
      // because `.example.com` is one character longer than `example.com` and a
      // length test would let the suffix beat the exact row it exists beside -
      // which is the pair the rule was written for.
      bool Beats(const std::shared_ptr<DomainPolicyEntry> &candidate, const std::shared_ptr<DomainPolicyEntry> &held)
      {
         if (!held)
            return true;

         if (candidate->IsSuffix() != held->IsSuffix())
            return !candidate->IsSuffix();

         const int candidateLength = candidate->GetPattern().GetLength();
         const int heldLength = held->GetPattern().GetLength();

         if (candidateLength != heldLength)
            return candidateLength > heldLength;

         return candidate->GetAction() == DomainPolicyEntry::ActionBlack &&
                held->GetAction() != DomainPolicyEntry::ActionBlack;
      }
   }

   void
   DomainPolicyList::SetNeedRefresh()
   {
      std::lock_guard<std::mutex> guard(list_mutex);
      need_refresh = true;
   }

   bool
   DomainPolicyList::Refresh_()
   {
      DomainPolicyEntries all;

      if (!all.Refresh())
      {
         // Keep what is here and leave the flag set so the next session retries.
         // Replacing the list with an empty one would switch every black entry
         // off for as long as the database was unwell.
         const time_t now = time(nullptr);
         last_refresh_failure = now;

         if (last_refresh_report == 0 || now - last_refresh_report > 3600)
         {
            last_refresh_report = now;
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6960, "DomainPolicyList::Refresh_",
               "The domain policy list could not be read. The rows last read are still in force; a row added or removed since is not in effect yet.");
         }

         return false;
      }

      std::vector<std::shared_ptr<DomainPolicyEntry> > fresh;

      for (std::shared_ptr<DomainPolicyEntry> entry : all.GetSnapshot())
      {
         if (entry && !entry->GetPattern().IsEmpty())
            fresh.push_back(entry);
      }

      entries.swap(fresh);
      need_refresh = false;

      return true;
   }

   String
   DomainPolicyList::NormaliseName(const String &name)
   {
      String value = name;
      value.Trim();
      value.ToLower();

      // An address literal is not a name; no pattern covers it.
      if (value.StartsWith(_T("[")))
         return String();

      while (value.GetLength() > 0 && value.EndsWith(_T(".")))
         value = value.Left(value.GetLength() - 1);

      return value;
   }

   DomainPolicyList::Decision
   DomainPolicyList::Decide(const String &envelopeSender, const String &heloHost)
   {
      Decision decision;

      std::vector<std::shared_ptr<DomainPolicyEntry> > rows;

      {
         std::lock_guard<std::mutex> guard(list_mutex);

         // A minute between attempts once one has failed. The rows last read
         // stay in force meanwhile, which is why waiting is safe.
         if (need_refresh && (last_refresh_failure == 0 || time(nullptr) - last_refresh_failure >= 60))
            Refresh_();

         // The common case, and it must cost nothing: an empty list is one flag
         // test and one size test away from the SMTP session.
         if (entries.empty())
            return decision;

         rows = entries;
      }

      const String envelopeDomain = envelopeSender.IsEmpty() ? String() : NormaliseName(StringParser::ExtractDomain(envelopeSender));
      const String helo = NormaliseName(heloHost);

      std::shared_ptr<DomainPolicyEntry> best;
      String bestName;
      DomainPolicyEntry::MatchOn bestKey = DomainPolicyEntry::MatchBoth;

      for (const std::shared_ptr<DomainPolicyEntry> &row : rows)
      {
         const DomainPolicyEntry::MatchOn key = row->GetMatchOn();

         String matched;
         DomainPolicyEntry::MatchOn matchedKey = key;

         if ((key == DomainPolicyEntry::MatchEnvelope || key == DomainPolicyEntry::MatchBoth) && row->Covers(envelopeDomain))
         {
            matched = envelopeDomain;
            matchedKey = DomainPolicyEntry::MatchEnvelope;
         }
         else if ((key == DomainPolicyEntry::MatchHelo || key == DomainPolicyEntry::MatchBoth) && row->Covers(helo))
         {
            matched = helo;
            matchedKey = DomainPolicyEntry::MatchHelo;
         }
         else
         {
            continue;
         }

         if (Beats(row, best))
         {
            best = row;
            bestName = matched;
            bestKey = matchedKey;
         }
      }

      if (!best)
         return decision;

      decision.verdict = best->GetAction() == DomainPolicyEntry::ActionBlack ? VerdictBlack : VerdictWhite;
      decision.pattern = best->GetPattern();
      decision.matchedName = bestName;
      decision.key = bestKey;

      return decision;
   }

   AnsiString
   DomainPolicyList::Document(bool &readable)
   {
      DomainPolicyEntries all;
      readable = all.Refresh();

      AnsiString json = "{\"items\":[";
      int count = 0;

      if (readable)
      {
         for (std::shared_ptr<DomainPolicyEntry> entry : all.GetSnapshot())
         {
            if (!entry)
               continue;

            AnsiString item;
            item.Format("%s{\"id\":%I64d,\"pattern\":\"%s\",\"match_on\":\"%s\",\"action\":\"%s\",\"note\":\"%s\",\"created\":\"%s\"}",
               count > 0 ? "," : "",
               entry->GetID(),
               JsonText(entry->GetPattern()).c_str(),
               JsonText(DomainPolicyEntry::MatchOnWord(entry->GetMatchOn())).c_str(),
               JsonText(DomainPolicyEntry::ActionWord(entry->GetAction())).c_str(),
               JsonText(entry->GetNote()).c_str(),
               JsonText(Time::GetTimeStampFromDateTime(entry->GetCreated())).c_str());

            json += item;
            count++;
         }
      }

      json += "]}";

      return json;
   }

   bool
   DomainPolicyList::Add(const String &pattern, const String &matchOnWord, const String &actionWord,
                         const String &note, __int64 &id, String &refusal)
   {
      id = 0;
      refusal.Empty();

      DomainPolicyEntry::MatchOn matchOn = DomainPolicyEntry::MatchBoth;
      if (!DomainPolicyEntry::ReadMatchOn(matchOnWord, matchOn))
      {
         refusal = _T("match_on must be envelope, helo or both");
         return false;
      }

      DomainPolicyEntry::Action action = DomainPolicyEntry::ActionWhite;
      if (!DomainPolicyEntry::ReadAction(actionWord, action))
      {
         refusal = _T("action must be white or black");
         return false;
      }

      std::shared_ptr<DomainPolicyEntry> entry(new DomainPolicyEntry);
      entry->SetPattern(pattern);
      entry->SetMatchOn(matchOn);
      entry->SetAction(action);
      entry->SetNote(note);

      String error;
      if (!PersistentDomainPolicyEntry::SaveObject(entry, error, PersistenceModeNormal))
      {
         refusal = error.IsEmpty() ? String(_T("the entry could not be saved; see the error log")) : error;
         return false;
      }

      id = entry->GetID();

      LOG_APPLICATION(_T("Domain policy list: ") + entry->GetPattern() + _T(" (") +
                      DomainPolicyEntry::MatchOnWord(matchOn) + _T(") is now ") + DomainPolicyEntry::ActionWord(action) + _T("."));

      return true;
   }

   bool
   DomainPolicyList::Remove(__int64 id, bool &found)
   {
      found = false;

      DomainPolicyEntries all;
      if (!all.Refresh())
         return false;

      std::shared_ptr<DomainPolicyEntry> entry = all.GetItemByDBID(id);
      if (!entry)
         return true;

      found = true;

      if (!PersistentDomainPolicyEntry::DeleteObject(entry))
         return false;

      LOG_APPLICATION(_T("Domain policy list: ") + entry->GetPattern() + _T(" (") +
                      DomainPolicyEntry::MatchOnWord(entry->GetMatchOn()) + _T(") removed."));

      return true;
   }
}
