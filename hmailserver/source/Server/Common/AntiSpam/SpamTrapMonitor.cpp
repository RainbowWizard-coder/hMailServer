// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "SpamTrapMonitor.h"
#include "SenderPattern.h"
#include "SenderListCache.h"
#include "AntiSpamConfiguration.h"
#include "SpamAssassin/SpamAssassinLearner.h"

#include "../Application/Configuration.h"
#include "../Application/IniFileSettings.h"
#include "../Application/ObjectCache.h"
#include "../BO/Account.h"
#include "../BO/Alias.h"
#include "../BO/DistributionList.h"
#include "../BO/Domain.h"
#include "../BO/DomainAliases.h"
#include "../BO/SenderListEntries.h"
#include "../BO/SenderListEntry.h"
#include "../BO/SpamTrap.h"
#include "../BO/SpamTraps.h"
#include "../Cache/CacheContainer.h"
#include "../Persistence/PersistentAccount.h"
#include "../Persistence/PersistentAlias.h"
#include "../Persistence/PersistentDistributionList.h"
#include "../Persistence/PersistentSenderListEntry.h"
#include "../Persistence/PersistentSpamTrap.h"
#include "../TCPIP/IPAddress.h"
#include "../Threading/WorkQueue.h"
#include "../Util/AccountLogon.h"
#include "../Util/AuditTrail.h"
#include "../Util/AutoBanFirewall.h"
#include "../Util/FileUtilities.h"
#include "../../SMTP/SPF/SPF.h"
#include "../Util/GUIDCreator.h"
#include "../Util/Parsing/StringParser.h"
#include "../Util/ServerStatus.h"
#include "../Util/Time.h"
#include "../Util/Unicode.h"
#include "../../SMTP/PlusAddressing.h"

#include <ctime>
#include <map>
#include <mutex>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      std::mutex trap_mutex;
      bool need_refresh = true;

      // Lower-case address -> the domain it belongs to. Small by construction:
      // traps are typed in one at a time by an administrator.
      std::map<String, __int64> traps;

      // A table that cannot be read is reported at most once an hour. Without the
      // ceiling one unreadable table would write an error line per RCPT TO, which
      // is the shape of failure that fills a disk while somebody is already
      // dealing with a database problem.
      time_t last_refresh_report = 0;

      String Lower(const String &value)
      {
         String out = value;
         out.Trim();
         out.ToLower();
         return out;
      }

      // The last @ of an address, or -1. Written out rather than using a helper
      // so that a quoted local part containing an @ still splits at the right one.
      int LastAt(const String &address)
      {
         for (int i = address.GetLength() - 1; i >= 0; i--)
         {
            if (address[i] == '@')
               return i;
         }

         return -1;
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

      // Whether a real mailbox, alias or list already answers for this address.
      bool AddressIsTaken(const String &address)
      {
         std::shared_ptr<Account> account = std::shared_ptr<Account>(new Account);
         if (PersistentAccount::ReadObject(account, address))
            return true;

         std::shared_ptr<Alias> alias = std::shared_ptr<Alias>(new Alias);
         if (PersistentAlias::ReadObject(alias, address))
            return true;

         std::shared_ptr<DistributionList> list = std::shared_ptr<DistributionList>(new DistributionList);
         if (PersistentDistributionList::ReadObject(list, address))
            return true;

         return false;
      }
   }

   void
   SpamTrapMonitor::SetNeedRefresh()
   {
      std::lock_guard<std::mutex> guard(trap_mutex);
      need_refresh = true;
   }

   bool
   SpamTrapMonitor::Refresh_()
   {
      SpamTraps all;

      if (!all.RefreshAll())
      {
         // Keep what is here and leave the flag set so the next message retries.
         // Replacing the map with an empty one would switch every trap off for as
         // long as the database was unwell, which is when a harvester is no less
         // likely to be working through the list.
         const time_t now = time(nullptr);

         if (last_refresh_report == 0 || now - last_refresh_report > 3600)
         {
            last_refresh_report = now;
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6915, "SpamTrapMonitor::Refresh_",
               "The spam traps could not be read. The addresses last read are still trapping; a trap added or removed since is not in effect yet.");
         }

         return false;
      }

      std::map<String, __int64> fresh;

      for (std::shared_ptr<SpamTrap> trap : all.GetSnapshot())
      {
         if (!trap)
            continue;

         std::shared_ptr<const Domain> domain = CacheContainer::Instance()->GetDomain(trap->GetDomainID());

         // A trap whose domain has gone is inert rather than an error: the delete
         // of the domain takes its traps, and a row that outlives it by a moment
         // must not trap an address in a domain somebody else now has.
         if (!domain)
            continue;

         fresh[Lower(trap->GetLocalPart() + _T("@") + domain->GetName())] = trap->GetDomainID();
      }

      traps.swap(fresh);
      need_refresh = false;

      return true;
   }

   bool
   SpamTrapMonitor::Match(const String &recipient, String &trapAddress, __int64 &domainId)
   {
      trapAddress.Empty();
      domainId = 0;

      {
         std::lock_guard<std::mutex> guard(trap_mutex);

         if (need_refresh)
            Refresh_();

         // The common case, and it must cost nothing: no trap anywhere means the
         // whole feature is one map lookup away from the delivery path.
         if (traps.empty())
            return false;
      }

      std::shared_ptr<DomainAliases> aliases = ObjectCache::Instance()->GetDomainAliases();
      const String primary = Lower(aliases ? aliases->ApplyAliasesOnAddress(recipient) : recipient);

      std::shared_ptr<const Domain> domain = CacheContainer::Instance()->GetDomain(StringParser::ExtractDomain(primary));

      // Not a domain this server hosts, or one that is switched off: the recipient
      // is refused for its own reason, and nothing here has anything to say.
      if (!domain || !domain->GetIsActive())
         return false;

      String candidates[2];
      candidates[0] = primary;
      candidates[1] = Lower(PlusAddressing::ExtractAccountAddress(primary, domain));

      for (int i = 0; i < 2; i++)
      {
         if (candidates[i].IsEmpty())
            continue;

         if (i == 1 && candidates[1] == candidates[0])
            continue;

         __int64 found = 0;

         {
            std::lock_guard<std::mutex> guard(trap_mutex);

            std::map<String, __int64>::const_iterator entry = traps.find(candidates[i]);
            if (entry == traps.end())
               continue;

            found = entry->second;
         }

         // Belt and braces. A trap and an account cannot both be created for one
         // address - each refuses the other - but a restore, a directory sync or a
         // hand-edited table could produce both, and then the mailbox wins.
         if (AddressIsTaken(candidates[i]))
            return false;

         trapAddress = candidates[i];
         domainId = found;

         return true;
      }

      return false;
   }

   bool
   SpamTrapMonitor::IsTrapAddress(const String &address)
   {
      const String lower = Lower(address);
      const int at = LastAt(lower);

      if (at <= 0)
         return false;

      std::shared_ptr<const Domain> domain = CacheContainer::Instance()->GetDomain(lower.Mid(at + 1));

      if (!domain)
         return false;

      __int64 id = 0;

      return PersistentSpamTrap::Find(domain->GetID(), lower.Left(at), id);
   }

   bool
   SpamTrapMonitor::List(std::shared_ptr<const Domain> domain, std::vector<Listed> &listed)
   {
      listed.clear();

      if (!domain)
         return false;

      SpamTraps collection(domain->GetID());

      if (!collection.Refresh())
         return false;

      for (std::shared_ptr<SpamTrap> trap : collection.GetSnapshot())
      {
         if (!trap)
            continue;

         Listed entry;
         entry.id = trap->GetID();
         entry.local_part = trap->GetLocalPart();
         entry.address = Lower(trap->GetLocalPart() + _T("@") + domain->GetName());
         entry.created = trap->GetCreated();

         listed.push_back(entry);
      }

      return true;
   }

   SpamTrapMonitor::AddResult
   SpamTrapMonitor::Add(std::shared_ptr<const Domain> domain, const String &address,
                        String &canonicalAddress, String &refusal)
   {
      canonicalAddress.Empty();
      refusal.Empty();

      if (!domain || domain->GetID() <= 0)
      {
         refusal = _T("Save the domain before giving it a spam trap.");
         return AddRefused;
      }

      const String domainName = Lower(domain->GetName());
      String value = Lower(address);

      if (value.IsEmpty())
      {
         refusal = _T("A spam trap needs an address, such as never-used@") + domainName + _T(".");
         return AddRefused;
      }

      // Just the part before the @ is taken to mean this domain, which is what an
      // administrator means when they type "sales-2004" into a domain's page.
      if (LastAt(value) < 0)
         value = value + _T("@") + domainName;

      const int at = LastAt(value);

      if (at <= 0 || at >= value.GetLength() - 1)
      {
         refusal = _T("A spam trap is an e-mail address, such as never-used@") + domainName + _T(".");
         return AddRefused;
      }

      if (!StringParser::IsValidEmailAddress(value, true))
      {
         refusal = _T("A spam trap is an e-mail address, such as never-used@") + domainName + _T(".");
         return AddRefused;
      }

      if (value.Mid(at + 1) != domainName)
      {
         refusal = _T("A spam trap must be an address in ") + domainName + _T(".");
         return AddRefused;
      }

      const String localPart = value.Left(at);

      if (localPart.GetLength() > 64)
      {
         refusal = _T("The part of the address before the @ is at most 64 characters.");
         return AddRefused;
      }

      if (AddressIsTaken(value))
      {
         // Said plainly to the administrator, who is entitled to know: it is the
         // sender at RCPT TO who is told nothing.
         refusal = value + _T(" is already an account, an alias or a distribution list. A spam trap has to be an address nobody uses.");
         return AddTaken;
      }

      __int64 existing = 0;

      if (PersistentSpamTrap::Find(domain->GetID(), localPart, existing))
      {
         canonicalAddress = value;
         return AddAlreadyATrap;
      }

      std::shared_ptr<SpamTrap> trap = std::shared_ptr<SpamTrap>(new SpamTrap());
      trap->SetDomainID(domain->GetID());
      trap->SetLocalPart(localPart);
      trap->SetCreated(DateTime::GetCurrentTime());

      // The words the trail records this with. The statement carries the
      // domain's id and the local part, and AuditTrail names an object from a
      // column ending in name, address or alias - none of which hm_spamtraps
      // has - so without these a declared trap would read as
      // "spam trap / created / stdomainid=4, stlocalpart=sales-2004" and the
      // delete below, which carries nothing but a row id, as "stid=42". The
      // address is the one fact an auditor needs and the one the row stops
      // holding the moment it is gone.
      AuditDescription described(_T("hm_spamtraps"), value,
                                String(_T("declared a spam trap of ")) + domainName);

      if (!PersistentSpamTrap::SaveObject(trap))
      {
         refusal = _T("The spam trap could not be saved; see the error log.");
         return AddFailed;
      }

      canonicalAddress = value;

      LOG_APPLICATION(_T("Spam trap: ") + value + _T(" is now a spam trap. Mail addressed to it is refused as an unknown user, and the sender is banned and blocked."));

      return AddCreated;
   }

   bool
   SpamTrapMonitor::Remove(std::shared_ptr<const Domain> domain, const String &address,
                           String &canonicalAddress, bool &found)
   {
      found = false;
      canonicalAddress.Empty();

      if (!domain || domain->GetID() <= 0)
         return true;

      const String domainName = Lower(domain->GetName());
      String value = Lower(address);

      if (LastAt(value) < 0)
         value = value + _T("@") + domainName;

      const int at = LastAt(value);

      if (at <= 0 || value.Mid(at + 1) != domainName)
         return true;

      canonicalAddress = value;

      SpamTraps collection(domain->GetID());

      if (!collection.Refresh())
         return false;

      std::shared_ptr<SpamTrap> trap = collection.GetItemByName(value.Left(at));

      if (!trap)
         return true;

      // As on the save above: the delete statement is "where stid = 42" and
      // nothing in it says which address stopped trapping.
      AuditDescription described(_T("hm_spamtraps"), canonicalAddress,
                                String(_T("no longer a spam trap of ")) + domainName);

      if (!collection.DeleteItemByDBID(trap->GetID()))
         return false;

      found = true;

      LOG_APPLICATION(_T("Spam trap: ") + value + _T(" is no longer a spam trap."));

      return true;
   }

   SpamTrapMonitor::Outcome
   SpamTrapMonitor::OnHit(const String &trapAddress, __int64 domainId, const IPAddress &address,
                          const String &envelopeSender, const String &heloHost, bool authenticated, bool rangeRunsSpamTests,
                          bool whiteListed, bool incomingRelay, bool bannedEarlierInSession)
   {
      Outcome outcome;

      const String connecting = String(address.ToString());
      const String sender = Lower(envelopeSender);

      ServerStatus::Instance()->OnSpamTrapHit();

      // ---- whether this session may be taught from at all
      //
      // Decided here, beside the ban and the block, because it is the same
      // question they ask: is this a party the server has already been told to
      // trust. A lesson outlives a ban, so every reason not to ban is a reason
      // not to teach - except the incoming relay, which is the machine carrying
      // the message rather than whoever wrote it.
      outcome.teachable = !authenticated && rangeRunsSpamTests && !whiteListed &&
                          !AutoBanFirewall::IsNeverBanned(address);

      // ---- the ban
      String banNote;
      const int banMinutes = Configuration::Instance()->GetSpamTrapBanMinutes();

      if (banMinutes <= 0)
         banNote = _T("not banned: SpamTrapBanMinutes is 0");
      else if (authenticated)
         banNote = _T("not banned: the session had authenticated");
      else if (!rangeRunsSpamTests)
         banNote = _T("not banned: its IP range does not run the spam tests");
      else if (whiteListed)
         banNote = _T("not banned: the anti-spam white list covers it");
      else if (incomingRelay)
         banNote = _T("not banned: it is an incoming relay");
      else if (AutoBanFirewall::IsNeverBanned(address))
         banNote = _T("not banned: AutoBanNeverBan lists it");
      else if (bannedEarlierInSession)
      {
         outcome.banned = true;
         banNote = _T("banned earlier in this session");
      }
      else
      {
         AccountLogon logon;

         if (logon.BanForSpamTrap(address, trapAddress, banMinutes))
         {
            outcome.banned = true;
            banNote = _T("banned for ") + StringParser::IntToString(banMinutes) + _T(" minutes");
         }
         else
         {
            banNote = _T("not banned: the ban could not be saved");
         }
      }

      // ---- the sender
      String blockNote;
      const int blockDays = Configuration::Instance()->GetSpamTrapBlockDays();

      std::shared_ptr<DomainAliases> aliases = ObjectCache::Instance()->GetDomainAliases();
      const String senderDomain = sender.IsEmpty()
         ? String()
         : StringParser::ExtractDomain(aliases ? aliases->ApplyAliasesOnAddress(sender) : sender);

      if (blockDays <= 0)
         blockNote = _T("sender not blocked: SpamTrapBlockDays is 0");
      else if (authenticated)
         blockNote = _T("sender not blocked: the session had authenticated");
      else if (sender.IsEmpty())
         blockNote = _T("sender not blocked: the null sender, which no list may swallow");
      else if (whiteListed)
         blockNote = _T("sender not blocked: the anti-spam white list covers it");
      else if (CacheContainer::Instance()->GetDomain(senderDomain))
         blockNote = _T("sender not blocked: it claims a domain this server hosts, so it is a forgery");
      else
      {
         // The envelope sender is a claim the client typed, and a trap address is
         // published to be found. Blocked on the claim alone, any correspondent an
         // unauthenticated client cared to name - a bank, a supplier - went on the
         // server's block list and had its real mail held or marked for every
         // mailbox for SpamTrapBlockDays; the hosted-domain case above was the only
         // forgery refused. So the sender is blocked only when its domain's SPF
         // authorises the address that addressed the trap, which is the one case in
         // which the block lands on whoever really sent it. Anything short of a pass
         // - none, neutral, softfail, fail, an error - leaves the list alone; the
         // hit's row still names the sender, and the address is banned all the same.
         String explanation;
         const SPF::Result spf = SPF::Instance()->Test(connecting, sender, heloHost, explanation);

         if (spf != SPF::Result::Pass)
            blockNote = _T("sender not blocked: SPF does not authorise the connecting address to send for it, so the envelope sender may be forged");
         else
            outcome.blocked = BlockSender_(sender, trapAddress, connecting, blockDays, blockNote);
      }

      // ---- the record
      SpamTrapHit hit;
      hit.domain_id = domainId;
      hit.address = trapAddress;
      hit.ip_address = connecting;
      hit.sender = sender;
      hit.time = DateTime::GetCurrentTime();
      hit.banned = outcome.banned;
      hit.blocked = outcome.blocked;
      hit.note = banNote + _T("; ") + blockNote;

      if (!PersistentSpamTrap::RecordHit(hit))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6910, "SpamTrapMonitor::OnHit",
            "A spam trap was addressed and the hit could not be recorded. The ban and the sender block still happened; the listing will not show this one.");
      }

      LOG_APPLICATION(_T("Spam trap: ") + trapAddress + _T(" was addressed from ") + connecting +
                      _T(" by ") + (sender.IsEmpty() ? String(_T("<>")) : sender) + _T("; ") + hit.note + _T("."));

      return outcome;
   }

   bool
   SpamTrapMonitor::BlockSender_(const String &sender, const String &trapAddress,
                                 const String &connectingAddress, int days, String &note)
   {
      String problem;

      if (!SenderPattern::Validate(sender, problem))
      {
         note = _T("sender not blocked: it is not an address the list can hold");
         return false;
      }

      const String value = SenderPattern::Canonical(sender);

      // Does the server's own list already say this sender is wanted? Asked of
      // SenderListCache, which is the one place the precedence between entries
      // is decided, rather than of a lookup for a row whose value is exactly
      // this address: an administrator's allow entry usually names the DOMAIN,
      // and an exact-value read cannot see it. The block written below would be
      // an address entry, which beats a domain entry on specificity - so an
      // exact-value read would let an automatic block overturn a decision
      // somebody wrote down on purpose, which is the opposite of what this
      // feature promises. Only the server scope can answer, because a trap hit
      // has no recipient account or domain behind it, and that is what Decide
      // consults when it is given neither.
      if (SenderListCache::Decide(sender, String(), 0, 0).verdict == SenderListCache::Allowed)
      {
         note = _T("sender not blocked: the server's allow list names it");
         return false;
      }

      DateTimeSpan span;
      span.SetDateTimeSpan(days, 0, 0, 0);
      const DateTime until = DateTime::GetCurrentTime() + span;

      String reason = _T("Addressed the spam trap ") + trapAddress + _T(" from ") + connectingAddress;
      if (reason.GetLength() > 255)
         reason = reason.Left(255);

      std::shared_ptr<SenderListEntry> entry;
      __int64 existingId = 0;

      if (PersistentSenderListEntry::Find(SenderListEntry::ScopeServer, 0, value, existingId))
      {
         SenderListEntries entries(SenderListEntry::ScopeServer, 0);

         if (entries.Refresh())
            entry = entries.GetItemByDBID((unsigned __int64) existingId);

         if (!entry)
         {
            note = _T("sender not blocked: the server's list could not be read");
            return false;
         }

         if (entry->HasExpired())
         {
            // The row has run out, so it is not a statement in force and the
            // sweep will take it away. A value is unique within a scope, so the
            // block has to be written INTO this row rather than beside it - but
            // it is a new statement, made now, by this hit. Created says so;
            // left as it was, the listing would show today's automatic block as
            // something an administrator wrote down on the day their own
            // expired entry was made.
            entry->SetCreated(DateTime::GetCurrentTime());
         }
         else if (!entry->GetBlocked())
         {
            // An administrator has decided about this sender, and allowing is a
            // decision a trap hit does not get to overturn. Decide above will
            // normally have answered Allowed and returned already; this is the
            // same answer read from the row itself, for the moment when the
            // cache has not caught up with a list just written to.
            note = _T("sender not blocked: the server's allow list names it");
            return false;
         }
         else
         {
            // A block already wider than this one is left exactly as it is.
            if (!entry->GetExpires())
            {
               note = _T("sender already blocked, with no expiry");
               return true;
            }

            if (!(entry->GetExpiresTime() < until))
            {
               note = _T("sender already blocked for longer");
               return true;
            }
         }
      }
      else
      {
         entry = std::shared_ptr<SenderListEntry>(new SenderListEntry());
         entry->SetScope(SenderListEntry::ScopeServer);
         entry->SetOwnerID(0);
         entry->SetValue(value);
         entry->SetCreated(DateTime::GetCurrentTime());
      }

      entry->SetBlocked(true);
      entry->SetReason(reason);
      entry->SetExpires(true);
      entry->SetExpiresTime(until);

      if (!PersistentSenderListEntry::SaveObject(entry))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6911, "SpamTrapMonitor::BlockSender_",
            "A spam trap was addressed and the sender could not be added to the server's block list. The address was still banned; mail from this sender is not held.");

         note = _T("sender not blocked: the server's list could not be written");
         return false;
      }

      note = _T("sender blocked for ") + StringParser::IntToString(days) + _T(" days");

      return true;
   }

   void
   SpamTrapMonitor::TeachSpamAssassin(const String &messageFile)
   {
      if (!Configuration::Instance()->GetSpamTrapTeachSpamAssassin())
         return;

      if (!Configuration::Instance()->GetAntiSpamConfiguration().GetSpamAssassinEnabled())
      {
         LOG_DEBUG("SpamTrapMonitor::TeachSpamAssassin - SpamAssassin is not enabled, so there is nothing to teach.");
         return;
      }

      std::shared_ptr<WorkQueue> queue = Application::Instance()->GetAsyncWorkQueue();

      if (!queue)
         return;

      // A copy, because the message is on its way to a mailbox: delivery removes
      // the queue file as soon as it has been written, and the lesson is a
      // conversation with another process that may take seconds. The task deletes
      // the copy when it is done with it, whatever spamd answered.
      const String copy = FileUtilities::Combine(IniFileSettings::Instance()->GetTempDirectory(),
                                                 _T("spamtrap-") + GUIDCreator::GetGUID() + _T(".eml"));

      if (!FileUtilities::Copy(messageFile, copy, true))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Low, 6914, "SpamTrapMonitor::TeachSpamAssassin",
            "The message from a session that addressed a spam trap could not be copied, so SpamAssassin was not taught from it.");
         return;
      }

      std::shared_ptr<SpamAssassinLearnTask> task = std::shared_ptr<SpamAssassinLearnTask>(
         new SpamAssassinLearnTask(copy, true, IniFileSettings::Instance()->GetSpamAssassinUser(), true));

      queue->AddTask(task);

      LOG_APPLICATION(_T("Spam trap: the message sent by a session that addressed a spam trap is being taught to SpamAssassin as spam."));
   }

   AnsiString
   SpamTrapMonitor::HitsDocument(__int64 domainId, int maxRows, bool &readable)
   {
      if (maxRows < 1)
         maxRows = 1;
      if (maxRows > 1000)
         maxRows = 1000;

      std::vector<SpamTrapHit> hits;

      readable = PersistentSpamTrap::ReadHits(domainId, maxRows, hits);

      if (!readable)
         return AnsiString("{\"items\":[]}");

      AnsiString json = "{\"items\":[";

      bool first = true;

      for (const SpamTrapHit &hit : hits)
      {
         if (!first)
            json += ",";

         first = false;

         AnsiString row;
         row.Format("{\"id\":%I64d,\"time\":\"%hs\",\"address\":\"%hs\",\"domain\":\"%hs\",\"ip_address\":\"%hs\","
                    "\"sender\":\"%hs\",\"banned\":%hs,\"blocked\":%hs,\"note\":\"%hs\"}",
            hit.id,
            JsonText(Time::GetTimeStampFromDateTime(hit.time)).c_str(),
            JsonText(hit.address).c_str(),
            JsonText(StringParser::ExtractDomain(hit.address)).c_str(),
            JsonText(hit.ip_address).c_str(),
            JsonText(hit.sender).c_str(),
            hit.banned ? "true" : "false",
            hit.blocked ? "true" : "false",
            JsonText(hit.note).c_str());

         json += row;
      }

      json += "]";

      if (domainId <= 0)
      {
         AnsiString total;
         total.Format(",\"hits_since_start\":%I64u", ServerStatus::Instance()->GetNumberOfSpamTrapHits());
         json += total;
      }

      json += "}";

      return json;
   }

   int
   SpamTrapMonitor::ClearHits(__int64 domainId)
   {
      const int before = PersistentSpamTrap::CountHits(domainId);

      if (before < 0)
         return -1;

      if (!PersistentSpamTrap::DeleteHits(domainId))
         return -1;

      return before;
   }

   void
   SpamTrapMonitor::SweepHits()
   {
      const int days = Configuration::Instance()->GetSpamTrapHitRetentionDays();

      if (days <= 0)
         return;

      if (!PersistentSpamTrap::DeleteHitsOlderThan(days))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Low, 6913, "SpamTrapMonitor::SweepHits",
            "Spam trap hits older than SpamTrapHitRetentionDays could not be removed. They are kept until a later sweep succeeds.");
      }
   }
}
