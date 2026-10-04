// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "QuarantineReview.h"
#include "QuarantineStore.h"
#include "DomainPolicyList.h"
#include "SenderPattern.h"
#include "DMARC/DMARC.h"
#include "DMARC/PublicSuffixList.h"

#include "../BO/DomainPolicyEntries.h"
#include "../BO/DomainPolicyEntry.h"
#include "../BO/SenderListEntries.h"
#include "../BO/SenderListEntry.h"
#include "../Persistence/PersistenceMode.h"
#include "../Persistence/PersistentDomainPolicyEntry.h"
#include "../Persistence/PersistentSenderListEntry.h"
#include "../Util/JsonDocument.h"
#include "../Util/Unicode.h"
#include "../Util/Parsing/StringParser.h"

#include <chrono>
#include <map>
#include <set>
#include <stdexcept>
#include <string>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // JSON text of a value, as UTF-8 with the escapes RFC 8259 asks for. The
      // same escaping DomainPolicyList's document does; kept here rather than
      // borrowed from the REST server, because COM answers this document too.
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

      AnsiString Quoted(const String &value)
      {
         return AnsiString("\"") + JsonText(value) + AnsiString("\"");
      }

      String FromUtf8(const std::string &utf8)
      {
         String value;
         Unicode::MultiByteToWide(AnsiString(utf8.c_str()), value);
         return value;
      }

      // One list entry the request makes, and the messages that make it.
      struct ReviewEntry
      {
         String value;
         String match_on;
         bool public_suffix = false;
         String outcome;
         String error;
         std::vector<__int64> ids;
      };

      // One message the request names.
      struct ReviewItem
      {
         __int64 id = 0;
         String entry;     // the value of its entry, for allow and block
         String problem;   // why nothing is done to it, when something is wrong
         String outcome;
      };

      const char *const ReasonText = "Added from the quarantine review.";

      // The server's allow and block list: what writing value as allowed or
      // blocked would do, or does. The rule the REST route keeps: a value the
      // list holds is rewritten, not duplicated, and an entry that no longer
      // applies because it expired is brought back as a lasting one.
      bool WriteAddress(const String &value, bool blocked, bool dryRun, String &outcome, String &error)
      {
         __int64 existingId = 0;
         const bool exists = PersistentSenderListEntry::Find(SenderListEntry::ScopeServer, 0, value, existingId);

         SenderListEntries entries(SenderListEntry::ScopeServer, 0);
         if (!entries.Refresh())
         {
            error = "The server's allow and block list could not be read.";
            return false;
         }

         std::shared_ptr<SenderListEntry> entry = exists ? entries.GetItemByDBID(existingId) : std::shared_ptr<SenderListEntry>();

         if (entry)
         {
            if (entry->GetBlocked() == blocked && !entry->GetExpires())
            {
               outcome = "unchanged";
               return true;
            }

            if (dryRun)
            {
               outcome = "change";
               return true;
            }

            entry->SetBlocked(blocked);
            entry->SetExpires(false);
            entry->SetReason(ReasonText);

            if (!PersistentSenderListEntry::SaveObject(entry))
            {
               error = "The entry could not be saved; see the error log.";
               return false;
            }

            outcome = "changed";
         }
         else
         {
            // The ceiling CreateEntry keeps for every list.
            if (entries.GetCount() >= 1000)
            {
               error = "The server's allow and block list already holds a thousand entries; remove some first.";
               return false;
            }

            if (dryRun)
            {
               outcome = "add";
               return true;
            }

            entry = std::shared_ptr<SenderListEntry>(new SenderListEntry());
            entry->SetScope(SenderListEntry::ScopeServer);
            entry->SetOwnerID(0);
            entry->SetValue(value);
            entry->SetBlocked(blocked);
            entry->SetReason(ReasonText);

            if (!PersistentSenderListEntry::SaveObject(entry))
            {
               error = "The entry could not be saved; see the error log.";
               return false;
            }

            outcome = "added";
         }

         LOG_APPLICATION(Formatter::Format("Quarantine review: {0} is now {1} on the server's allow and block list.",
            value, blocked ? String(_T("blocked")) : String(_T("allowed"))));

         return true;
      }

      // The domain white and black list, the same way: a row for the pattern
      // and the key already there is changed to this action, not joined by a
      // second one the persister would refuse.
      bool WriteDomain(const String &pattern, const String &matchOnWord, bool black, bool dryRun, String &outcome, String &error)
      {
         DomainPolicyEntry::MatchOn matchOn = DomainPolicyEntry::MatchEnvelope;
         DomainPolicyEntry::ReadMatchOn(matchOnWord, matchOn);

         const DomainPolicyEntry::Action action = black ? DomainPolicyEntry::ActionBlack : DomainPolicyEntry::ActionWhite;

         const __int64 existingId = PersistentDomainPolicyEntry::Find(pattern, (int) matchOn);

         if (existingId != 0)
         {
            DomainPolicyEntries all;
            if (!all.Refresh())
            {
               error = "The domain white and black list could not be read.";
               return false;
            }

            std::shared_ptr<DomainPolicyEntry> entry = all.GetItemByDBID(existingId);
            if (!entry)
            {
               error = "The domain white and black list changed while it was read; try again.";
               return false;
            }

            if (entry->GetAction() == action)
            {
               outcome = "unchanged";
               return true;
            }

            if (dryRun)
            {
               outcome = "change";
               return true;
            }

            entry->SetAction(action);

            String refusal;
            if (!PersistentDomainPolicyEntry::SaveObject(entry, refusal, PersistenceModeNormal))
            {
               error = refusal.IsEmpty() ? String(_T("The entry could not be saved; see the error log.")) : refusal;
               return false;
            }

            LOG_APPLICATION(_T("Quarantine review: ") + pattern + _T(" (") + matchOnWord + _T(") is now ") +
                            DomainPolicyEntry::ActionWord(action) + _T(" on the domain white and black list."));

            outcome = "changed";
            return true;
         }

         if (dryRun)
         {
            outcome = "add";
            return true;
         }

         __int64 id = 0;
         String refusal;
         if (!DomainPolicyList::Add(pattern, matchOnWord, DomainPolicyEntry::ActionWord(action), ReasonText, id, refusal))
         {
            error = refusal;
            return false;
         }

         outcome = "added";
         return true;
      }

      bool ReadWord(const JsonValue &body, const char *key, String &out, String &error)
      {
         const JsonValue *member = body.Get(key);
         if (!member || member->IsNull())
            return true;

         if (!member->IsString())
         {
            error = String(key) + _T(" must be a string");
            return false;
         }

         out = FromUtf8(member->AsString());
         return true;
      }

      bool ReadFlag(const JsonValue &body, const char *key, bool &out, String &error)
      {
         const JsonValue *member = body.Get(key);
         if (!member || member->IsNull())
            return true;

         if (!member->IsBool())
         {
            error = String(key) + _T(" must be true or false");
            return false;
         }

         out = member->AsBool();
         return true;
      }
   }

   String
   QuarantineReview::ActionWord(Action action)
   {
      switch (action)
      {
      case ActionDelete: return _T("delete");
      case ActionAllow: return _T("allow");
      case ActionBlock: return _T("block");
      default: return _T("release");
      }
   }

   bool
   QuarantineReview::IsPublicSuffix(const String &domain)
   {
      String name = DomainPolicyList::NormaliseName(domain);
      if (name.StartsWith(_T(".")))
         name = name.Mid(1);

      if (name.IsEmpty())
         return false;

      const std::vector<String> labels = StringParser::SplitString(name, _T("."));
      if (labels.empty())
         return false;

      // One label is a suffix by the list's implicit "*" rule: localhost, a
      // top-level domain.
      if (labels.size() == 1)
         return true;

      return PublicSuffixList::GetPublicSuffixLabelCount(labels) >= labels.size();
   }

   String
   QuarantineReview::OrganizationOf_(const String &normalisedName)
   {
      if (normalisedName.IsEmpty())
         return String();

      // An address is not a name. The session stores the HELO as the client
      // gave it, and a client that says HELO 192.0.2.7 would otherwise be
      // grouped under 2.7 - the list's implicit rule takes the last number for
      // a top-level domain - and blocking that group would write .2.7 on the
      // domain list, refusing every client whose HELO is an address ending so.
      if (StringParser::IsValidIPAddress(normalisedName))
         return String();

      return DMARC::GetOrganizationalDomainByPublicSuffixList(normalisedName);
   }

   String
   QuarantineReview::SenderDomain(const String &sender)
   {
      String address = sender;
      address.Trim();

      if (address.IsEmpty() || address.Find(_T("@")) < 0)
         return String();

      return OrganizationOf_(DomainPolicyList::NormaliseName(StringParser::ExtractDomain(address)));
   }

   String
   QuarantineReview::HeloDomain(const String &helo)
   {
      return OrganizationOf_(DomainPolicyList::NormaliseName(helo));
   }

   bool
   QuarantineReview::EntryFor_(const Request &request, const QuarantinedMessage &message, String &value, String &problem)
   {
      value.Empty();

      String sender = message.sender;
      sender.Trim();

      if (request.list == ListAddress)
      {
         if (sender.IsEmpty())
         {
            problem = "The message has the null sender, which no list can name.";
            return false;
         }

         String refusal;
         if (!SenderPattern::Validate(sender, refusal))
         {
            problem = String(_T("The sender cannot be listed: ")) + refusal;
            return false;
         }

         value = SenderPattern::Canonical(sender);
         return true;
      }

      String organization;

      if (request.source == DomainOfSender)
      {
         if (sender.IsEmpty())
         {
            problem = "The message has the null sender, so there is no sender domain to list.";
            return false;
         }

         organization = SenderDomain(sender);
         if (organization.IsEmpty())
         {
            problem = "The sender's address has no domain to list.";
            return false;
         }
      }
      else
      {
         String helo = message.helo;
         helo.Trim();

         if (helo.IsEmpty())
         {
            problem = "No HELO name was recorded for this message: it was held before the server recorded them, or it did not arrive over SMTP.";
            return false;
         }

         organization = HeloDomain(helo);
         if (organization.IsEmpty())
         {
            problem = "The HELO name is an address, which the domain list does not name.";
            return false;
         }
      }

      value = String(_T(".")) + organization;

      String refusal;
      if (!DomainPolicyEntry::ValidatePattern(value, refusal))
      {
         problem = String(_T("The domain cannot be listed: ")) + refusal;
         value.Empty();
         return false;
      }

      return true;
   }

   bool
   QuarantineReview::ParseRequest(const AnsiString &utf8Json, Request &out, String &error)
   {
      out = Request();

      JsonValue body;
      std::string parseError;
      std::string text(utf8Json.c_str(), (size_t) utf8Json.GetLength());

      if (!JsonValue::Parse(text, body, parseError) || !body.IsObject())
      {
         error = "The body must be a JSON object.";
         return false;
      }

      static const char *const known[] = { "ids", "action", "list", "domain_from", "dry_run", "confirm_public_suffix" };

      for (const std::pair<std::string, JsonValue> &member : body.Members())
      {
         bool found = false;
         for (const char *key : known)
            found = found || member.first == key;

         if (!found)
         {
            error = String(_T("unknown field: ")) + FromUtf8(member.first.substr(0, 48));
            return false;
         }
      }

      String actionWord;
      if (!ReadWord(body, "action", actionWord, error))
         return false;

      if (actionWord == _T("release"))
         out.action = ActionRelease;
      else if (actionWord == _T("delete"))
         out.action = ActionDelete;
      else if (actionWord == _T("allow"))
         out.action = ActionAllow;
      else if (actionWord == _T("block"))
         out.action = ActionBlock;
      else
      {
         error = "action is required: release, delete, allow or block";
         return false;
      }

      const JsonValue *ids = body.Get("ids");
      if (!ids || !ids->IsArray() || ids->Size() == 0)
      {
         error = "ids is required: an array of 1 to 1000 quarantine ids";
         return false;
      }

      if (ids->Size() > (size_t) MaxIds)
      {
         error = "ids names at most 1000 messages in one request";
         return false;
      }

      std::set<__int64> seen;
      for (const JsonValue &item : ids->Items())
      {
         const __int64 id = item.AsInt64();

         if (!item.IsNumber() || id <= 0 || (double) id != item.AsNumber())
         {
            error = "every id must be a positive whole number";
            return false;
         }

         // Named twice is acted on once: a release that ran twice would deliver
         // twice, if the store could, and the answer would list the second as a
         // failure the reader did not cause.
         if (seen.insert(id).second)
            out.ids.push_back(id);
      }

      String listWord;
      String fromWord;
      if (!ReadWord(body, "list", listWord, error) ||
          !ReadWord(body, "domain_from", fromWord, error) ||
          !ReadFlag(body, "dry_run", out.dry_run, error) ||
          !ReadFlag(body, "confirm_public_suffix", out.confirm_public_suffix, error))
         return false;

      if (!WritesAList(out.action) && (!listWord.IsEmpty() || !fromWord.IsEmpty()))
      {
         error = "list and domain_from go with allow and block only";
         return false;
      }

      if (listWord.IsEmpty() || listWord == _T("address"))
         out.list = ListAddress;
      else if (listWord == _T("domain"))
         out.list = ListDomain;
      else
      {
         error = "list must be address or domain";
         return false;
      }

      if (!fromWord.IsEmpty() && out.list != ListDomain)
      {
         error = "domain_from goes with list domain only";
         return false;
      }

      if (fromWord.IsEmpty() || fromWord == _T("sender"))
         out.source = DomainOfSender;
      else if (fromWord == _T("helo"))
         out.source = DomainOfHelo;
      else
      {
         error = "domain_from must be sender or helo";
         return false;
      }

      return true;
   }

   QuarantineReview::Outcome
   QuarantineReview::Run(const Request &request, AnsiString &out_json, String &out_error)
   {
      out_json.Empty();
      out_error.Empty();

      if (request.ids.empty() || request.ids.size() > (size_t) MaxIds)
      {
         out_error = "ids names 1 to 1000 messages";
         return OutcomeBadRequest;
      }

      const bool lists = WritesAList(request.action);
      const bool blocking = request.action == ActionBlock;
      const String listWord = !lists ? String() : request.list == ListAddress ? String(_T("address")) : String(_T("domain"));
      const String matchOnWord = lists && request.list == ListDomain
         ? (request.source == DomainOfHelo ? String(_T("helo")) : String(_T("envelope")))
         : String();

      // Every message first, and the entries they make.
      std::vector<ReviewItem> items;
      std::vector<ReviewEntry> entries;
      std::map<String, size_t> entryIndex;

      for (__int64 id : request.ids)
      {
         ReviewItem item;
         item.id = id;

         QuarantinedMessage message;
         if (!QuarantineStore::GetById(id, message))
         {
            item.problem = "No quarantined message with that id.";
         }
         else if (lists)
         {
            String value;
            String problem;

            if (EntryFor_(request, message, value, problem))
            {
               item.entry = value;

               auto at = entryIndex.find(value);
               if (at == entryIndex.end())
               {
                  ReviewEntry entry;
                  entry.value = value;
                  entry.match_on = matchOnWord;
                  entry.public_suffix = request.list == ListDomain && IsPublicSuffix(value);
                  entryIndex[value] = entries.size();
                  entries.push_back(entry);
                  at = entryIndex.find(value);
               }

               entries[at->second].ids.push_back(id);
            }
            else
            {
               item.problem = problem;
            }
         }

         items.push_back(item);
      }

      // A public suffix is written only on purpose.
      std::vector<String> suffixes;
      for (const ReviewEntry &entry : entries)
      {
         if (entry.public_suffix)
            suffixes.push_back(entry.value);
      }

      if (!suffixes.empty() && !request.dry_run && !request.confirm_public_suffix)
      {
         String names;
         AnsiString list;

         for (const String &name : suffixes)
         {
            names += (names.IsEmpty() ? _T("") : _T(", ")) + name;
            list += (list.IsEmpty() ? "" : ",") + Quoted(name);
         }

         out_error = String(_T("Nothing was changed: ")) + names +
            _T(" is a public suffix, and an entry for it covers every domain registered beneath it. ")
            _T("Send confirm_public_suffix to list it anyway.");

         out_json = AnsiString("{\"error\":") + Quoted(out_error) + ",\"public_suffix\":[" + list + "]}";
         return OutcomeNeedsConfirmation;
      }

      // The entries, before any message is acted on.
      for (ReviewEntry &entry : entries)
      {
         const bool written = request.list == ListAddress
            ? WriteAddress(entry.value, blocking, request.dry_run, entry.outcome, entry.error)
            : WriteDomain(entry.value, matchOnWord, blocking, request.dry_run, entry.outcome, entry.error);

         if (!written)
         {
            for (ReviewItem &item : items)
            {
               if (item.problem.IsEmpty() && item.entry == entry.value)
                  item.problem = String(_T("The list entry could not be written, so the message was left where it is: ")) + entry.error;
            }
         }
      }

      // Then the messages.
      const bool releasing = request.action == ActionRelease || request.action == ActionAllow;
      int succeeded = 0;
      int failed = 0;

      for (ReviewItem &item : items)
      {
         if (!item.problem.IsEmpty())
         {
            failed++;
            continue;
         }

         if (request.dry_run)
         {
            item.outcome = releasing ? _T("release") : _T("delete");
            succeeded++;
            continue;
         }

         if (releasing)
         {
            String error;
            if (QuarantineStore::Release(item.id, error))
               item.outcome = _T("released");
            else
               item.problem = error.IsEmpty() ? String(_T("The message could not be released.")) : error;
         }
         else
         {
            if (QuarantineStore::Delete(item.id))
               item.outcome = _T("deleted");
            else
               item.problem = "The quarantined message could not be deleted.";
         }

         if (item.problem.IsEmpty())
            succeeded++;
         else
            failed++;
      }

      AnsiString json = "{\"action\":" + Quoted(ActionWord(request.action)) +
         ",\"list\":" + Quoted(listWord) +
         ",\"domain_from\":" + Quoted(lists && request.list == ListDomain ? (request.source == DomainOfHelo ? String(_T("helo")) : String(_T("sender"))) : String()) +
         ",\"dry_run\":" + AnsiString(request.dry_run ? "true" : "false") +
         ",\"entries\":[";

      for (size_t i = 0; i < entries.size(); i++)
      {
         const ReviewEntry &entry = entries[i];

         AnsiString ids;
         for (size_t j = 0; j < entry.ids.size(); j++)
         {
            AnsiString number;
            number.Format("%s%I64d", j > 0 ? "," : "", entry.ids[j]);
            ids += number;
         }

         json += AnsiString(i > 0 ? "," : "") +
            "{\"value\":" + Quoted(entry.value) +
            ",\"list\":" + Quoted(listWord) +
            ",\"match_on\":" + Quoted(entry.match_on) +
            ",\"public_suffix\":" + AnsiString(entry.public_suffix ? "true" : "false") +
            ",\"outcome\":" + Quoted(entry.outcome) +
            ",\"error\":" + Quoted(entry.error) +
            ",\"ids\":[" + ids + "]}";
      }

      json += "],\"results\":[";

      for (size_t i = 0; i < items.size(); i++)
      {
         const ReviewItem &item = items[i];

         AnsiString number;
         number.Format("%I64d", item.id);

         json += AnsiString(i > 0 ? "," : "") +
            "{\"id\":" + number +
            ",\"ok\":" + AnsiString(item.problem.IsEmpty() ? "true" : "false") +
            ",\"outcome\":" + Quoted(item.outcome) +
            ",\"error\":" + Quoted(item.problem) + "}";
      }

      AnsiString counts;
      counts.Format("],\"succeeded\":%d,\"failed\":%d}", succeeded, failed);
      json += counts;

      out_json = json;
      return OutcomeDone;
   }

   void
   QuarantineReviewTester::Test()
   {
      // The HELO this server wrote, and only this server.
      const String ours = _T("from mail.sender.example (ptr.sender.example [192.0.2.7])\r\n\tby MAILHOST with ESMTP id 12\r\n\t; Sun, 27 Sep 2026 10:00:00 +0100");
      if (QuarantineStore::HeloFromReceivedHeader(ours, _T("mailhost")) != _T("mail.sender.example"))
         throw std::logic_error("QuarantineStore::HeloFromReceivedHeader did not read this server's own Received header.");

      if (!QuarantineStore::HeloFromReceivedHeader(ours, _T("otherhost")).IsEmpty())
         throw std::logic_error("QuarantineStore::HeloFromReceivedHeader read a Received header another host wrote.");

      const String noHelo = _T("from 192.0.2.7 ( [192.0.2.7])\r\n\tby MAILHOST with ESMTP id 12\r\n\t; Sun, 27 Sep 2026 10:00:00 +0100");
      if (!QuarantineStore::HeloFromReceivedHeader(noHelo, _T("MAILHOST")).IsEmpty())
         throw std::logic_error("QuarantineStore::HeloFromReceivedHeader took the address written for a client with no HELO as its name.");

      const String literal = _T("from [192.0.2.7] (ptr [192.0.2.7]) by MAILHOST with ESMTP id 3; date");
      if (QuarantineStore::HeloFromReceivedHeader(literal, _T("MAILHOST")) != _T("[192.0.2.7]"))
         throw std::logic_error("QuarantineStore::HeloFromReceivedHeader did not keep an address literal the client gave.");

      if (!QuarantineStore::HeloFromReceivedHeader(_T("by MAILHOST with ESMTP"), _T("MAILHOST")).IsEmpty())
         throw std::logic_error("QuarantineStore::HeloFromReceivedHeader read a header with no from clause.");

      if (!QuarantineStore::HeloFromReceivedHeader(_T("from by MAILHOST with ESMTP id 4"), _T("MAILHOST")).IsEmpty())
         throw std::logic_error("QuarantineStore::HeloFromReceivedHeader read a name out of a from clause that has none.");

      // Grouping by the organization, not the host.
      if (QuarantineReview::SenderDomain(_T("0101@bounces.Notify.Example.co.uk")) != _T("example.co.uk"))
         throw std::logic_error("QuarantineReview::SenderDomain did not give the registrable domain.");

      if (!QuarantineReview::SenderDomain(_T("")).IsEmpty())
         throw std::logic_error("QuarantineReview::SenderDomain gave a domain for the null sender.");

      if (QuarantineReview::HeloDomain(_T("mx1.notify.example.com.")) != _T("example.com"))
         throw std::logic_error("QuarantineReview::HeloDomain did not give the registrable domain.");

      if (!QuarantineReview::HeloDomain(_T("[192.0.2.7]")).IsEmpty())
         throw std::logic_error("QuarantineReview::HeloDomain gave a domain for an address literal.");

      // An address without its brackets, as a client may say it and the session
      // stores it, is no name either: not 2.7, which as .2.7 on the domain list
      // would cover every HELO that is an address ending in those two numbers.
      if (!QuarantineReview::HeloDomain(_T("192.0.2.7")).IsEmpty() || !QuarantineReview::HeloDomain(_T("2001:db8::7")).IsEmpty())
         throw std::logic_error("QuarantineReview::HeloDomain gave a domain for a HELO that is an address.");

      if (!QuarantineReview::SenderDomain(_T("someone@192.0.2.7")).IsEmpty())
         throw std::logic_error("QuarantineReview::SenderDomain gave a domain for an address.");

      // A name that is itself a public suffix, and one that is not.
      if (!QuarantineReview::IsPublicSuffix(_T("co.uk")) || !QuarantineReview::IsPublicSuffix(_T(".com")) ||
          !QuarantineReview::IsPublicSuffix(_T("localhost")))
         throw std::logic_error("QuarantineReview::IsPublicSuffix missed a public suffix.");

      if (QuarantineReview::IsPublicSuffix(_T("example.co.uk")) || QuarantineReview::IsPublicSuffix(_T(".example.com")))
         throw std::logic_error("QuarantineReview::IsPublicSuffix called a registrable domain a suffix.");

      // The request, and its refusals.
      QuarantineReview::Request request;
      String error;

      if (!QuarantineReview::ParseRequest("{\"ids\":[3,4,3],\"action\":\"allow\",\"list\":\"domain\",\"domain_from\":\"helo\"}", request, error) ||
          request.ids.size() != 2 || request.action != QuarantineReview::ActionAllow ||
          request.list != QuarantineReview::ListDomain || request.source != QuarantineReview::DomainOfHelo)
         throw std::logic_error("QuarantineReview::ParseRequest did not read a whole request, or kept an id named twice.");

      const char *const refused[] =
      {
         "[]",
         "{\"ids\":[1]}",
         "{\"ids\":[],\"action\":\"release\"}",
         "{\"ids\":[0],\"action\":\"release\"}",
         "{\"ids\":[1.5],\"action\":\"release\"}",
         "{\"ids\":[\"1\"],\"action\":\"release\"}",
         "{\"ids\":[1],\"action\":\"whitelist\"}",
         "{\"ids\":[1],\"action\":\"release\",\"list\":\"address\"}",
         "{\"ids\":[1],\"action\":\"allow\",\"list\":\"address\",\"domain_from\":\"helo\"}",
         "{\"ids\":[1],\"action\":\"block\",\"list\":\"everything\"}",
         "{\"ids\":[1],\"action\":\"delete\",\"dry_run\":\"yes\"}",
         "{\"ids\":[1],\"action\":\"delete\",\"extra\":1}"
      };

      for (const char *body : refused)
      {
         if (QuarantineReview::ParseRequest(body, request, error))
            throw std::logic_error(std::string("QuarantineReview::ParseRequest accepted a request it must refuse: ") + body);
      }

      // The HTML part's text, without what is not read.
      const String text = QuarantineStore::TextOfHtml(_T("<html><head><title>t</title><style>p{color:red}</style></head><body><script>x()</script><p>Hello &amp; welcome</p></body></html>"));
      if (text.Find(_T("Hello & welcome")) < 0 || text.Find(_T("color")) >= 0 || text.Find(_T("x()")) >= 0)
         throw std::logic_error("QuarantineStore::TextOfHtml left markup, a style sheet or a script in the preview.");

      // An element is cut by its whole name. Cut by "<head", a <header> or a
      // <headline> lost its text, and an unclosed <header> everything after it.
      // A head may leave out its end tag, and then ends where the body starts.
      struct NamedCase { const TCHAR *html; const TCHAR *kept; const TCHAR *dropped; };
      const NamedCase named[] = {
         { _T("<body><header><h1>Invoice 42 overdue</h1></header><p>Pay now</p></body>"), _T("Invoice 42 overdue Pay now"), nullptr },
         { _T("<body><header>Invoice 42 overdue<p>Pay now</p></body>"), _T("Invoice 42 overdue Pay now"), nullptr },
         { _T("<p>Hello</p><headline>Big news</headline><p>after</p>"), _T("Hello Big news after"), nullptr },
         { _T("<HTML><HEAD lang=\"en\"><TITLE>Head only</TITLE></HEAD\r\n><body><p>Body text</p></body></HTML>"), _T("Body text"), _T("Head only") },
         { _T("<html><head><title>Head only</title><body><p>Body text</p></body></html>"), _T("Body text"), _T("Head only") },
         { _T("<head><title>Title</title><p>Body text</p>"), _T("Body text"), nullptr },
         { _T("<style type=\"text/css\">p{color:red}</style ><p>Body text</p><script\n>x()</script>"), _T("Body text"), _T("color") }
      };

      for (size_t i = 0; i < sizeof(named) / sizeof(named[0]); i++)
      {
         const String namedText = QuarantineStore::TextOfHtml(named[i].html);
         if (namedText.Find(named[i].kept) < 0 || (named[i].dropped && namedText.Find(named[i].dropped) >= 0))
            throw std::logic_error("QuarantineStore::TextOfHtml cut an element by the start of its name, or a head without its end tag, in case " +
                                   std::to_string(i + 1) + " of its element names.");
      }

      // A held message is whatever its sender wrote, and a reviewer opening its
      // preview must not hand a thread to it: 80,000 empty style sheets - 1.2
      // million characters, a message of 1.2 MB - once took over half a minute,
      // since every pass read to the end for the two elements it has none of.
      String sheets;
      sheets.reserve(80000 * 15 + 16);
      for (int i = 0; i < 80000; i++)
         sheets += _T("<style></style>");
      sheets += _T("<p>after</p>");

      const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
      const String sheetsText = QuarantineStore::TextOfHtml(sheets);
      const long long took = (long long) std::chrono::duration_cast<std::chrono::milliseconds>(
         std::chrono::steady_clock::now() - started).count();

      if (sheetsText.Find(_T("after")) < 0 || sheetsText.Find(_T("style")) >= 0)
         throw std::logic_error("QuarantineStore::TextOfHtml did not skip 80,000 style sheets to the text after them.");

      const long long bound = (long long) HM_TEST_TIME_BOUND(3000);
      if (took >= bound)
         throw std::logic_error("QuarantineStore::TextOfHtml took " + std::to_string(took) +
                                " ms over 80,000 style sheets, and the bound is " + std::to_string(bound) + " ms.");

      // The same bound for what the whole names and a head's missing end tag
      // add: 80,000 heads with neither an end nor a body, each followed by a
      // </header> the search for </head> passes over; 80,000 heads each ended
      // by the body after it; and 80,000 headers, none of them a head.
      struct RepeatedCase { const TCHAR *unit; const char *name; };
      const RepeatedCase repeated[] = {
         { _T("<head></header>"), "<head></header>" },
         { _T("<head><body>"), "<head><body>" },
         { _T("<header>x</header>"), "<header>x</header>" }
      };

      for (const RepeatedCase &unit : repeated)
      {
         String many;
         many.reserve(80000 * 18 + 16);
         for (int i = 0; i < 80000; i++)
            many += unit.unit;
         many += _T("<p>after</p>");

         const std::chrono::steady_clock::time_point began = std::chrono::steady_clock::now();
         const String manyText = QuarantineStore::TextOfHtml(many);
         const long long spent = (long long) std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - began).count();

         if (manyText.Find(_T("after")) < 0)
            throw std::logic_error(std::string("QuarantineStore::TextOfHtml did not read the text after 80,000 ") + unit.name + ".");

         if (spent >= 3000)
            throw std::logic_error("QuarantineStore::TextOfHtml took " + std::to_string(spent) + " ms over 80,000 " + unit.name +
                                   ", and the bound is 3000 ms.");
      }
   }
}
