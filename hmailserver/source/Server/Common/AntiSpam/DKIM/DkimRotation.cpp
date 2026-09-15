// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "DkimRotation.h"
#include "DkimRotationPlan.h"
#include "DkimKeyPair.h"

#include "../../BO/Domain.h"
#include "../../BO/Domains.h"
#include "../../BO/DomainAlias.h"
#include "../../BO/DomainAliases.h"
#include "../../Cache/CacheContainer.h"
#include "../../Persistence/PersistentDomain.h"
#include "../../Persistence/PersistentDkimRotation.h"
#include "../../TCPIP/DNSResolver.h"
#include "../../Util/ProtectedFile.h"
#include "../../Util/Unicode.h"

#include <boost/filesystem.hpp>

#include <ctime>
#include <set>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      typedef DkimRotationPlan Plan;
      typedef DkimRotationPlan::State State;
      typedef DkimRotationPlan::Slot Slot;
      typedef DkimRotation::Result Result;

      // Every operation on every domain, one at a time. The scheduled pass takes it
      // per domain, so a status read waits for at most one domain's lookups.
      boost::recursive_mutex &Mutex()
      {
         static boost::recursive_mutex mutex;
         return mutex;
      }

      // Current and retiring records are informational; they are looked at again
      // once an hour, or when the clock a caller passed is behind the last look.
      const __int64 RecheckSeconds = 3600;

      // ------------------------------------------------------------------ JSON

      AnsiString Quote(const String &value)
      {
         AnsiString utf8;
         Unicode::WideToMultiByte(value, utf8);

         AnsiString result = "\"";
         for (int i = 0; i < utf8.GetLength(); i++)
         {
            const unsigned char character = (unsigned char) utf8[i];
            switch (character)
            {
            case '"':
               result += "\\\"";
               break;
            case '\\':
               result += "\\\\";
               break;
            case '\n':
               result += "\\n";
               break;
            case '\r':
               result += "\\r";
               break;
            case '\t':
               result += "\\t";
               break;
            default:
               if (character < 0x20)
               {
                  AnsiString escaped;
                  escaped.Format("\\u%04x", (int) character);
                  result += escaped;
               }
               else
               {
                  result += (char) character;
               }
            }
         }
         result += "\"";
         return result;
      }

      AnsiString Quote(const AnsiString &value)
      {
         return Quote(String(value));
      }

      AnsiString Number(__int64 value)
      {
         AnsiString text;
         text.Format("%I64d", value);
         return text;
      }

      AnsiString Bool(bool value)
      {
         return value ? "true" : "false";
      }

      String UtcText(__int64 seconds)
      {
         struct tm parts;
         memset(&parts, 0, sizeof(parts));
#ifdef HM_PLATFORM_POSIX
         time_t value = (time_t) seconds;
         if (::gmtime_r(&value, &parts) == nullptr)
            return String();
#else
         __time64_t value = seconds;
         if (_gmtime64_s(&parts, &value) != 0)
            return String();
#endif
         String text;
         text.Format(_T("%04d-%02d-%02d %02d:%02d UTC"), parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday, parts.tm_hour, parts.tm_min);
         return text;
      }

      // ------------------------------------------------------------------ the domain

      std::shared_ptr<Domain> ReadDomain(__int64 domainId)
      {
         // Always from the database, never the cache: the cache is what the signer
         // reads, and the whole point of the compare-and-set updates below is to see
         // what is stored now.
         std::shared_ptr<Domain> domain = std::shared_ptr<Domain>(new Domain());
         if (domainId <= 0 || !PersistentDomain::ReadObject(domain, domainId))
            return std::shared_ptr<Domain>();
         return domain;
      }

      Plan::Policy PolicyOf(const Domain &domain)
      {
         Plan::Policy policy;
         policy.enabled = domain.GetDKIMRotationEnabled();
         policy.interval_days = domain.GetDKIMRotationIntervalDays();
         policy.propagation_hours = domain.GetDKIMRotationPropagationHours();
         policy.retention_days = domain.GetDKIMRotationRetentionDays();
         policy.key_bits = domain.GetDKIMRotationKeyBits();

         // A value no interface would have accepted - a database edited by hand, a
         // backup edited before it was restored - reads as the default rather than
         // as a period of zero, which would rotate on every pass.
         Plan::Policy defaults;
         if (policy.interval_days < 1 || policy.interval_days > 3650)
            policy.interval_days = defaults.interval_days;
         if (policy.propagation_hours < 1 || policy.propagation_hours > 720)
            policy.propagation_hours = defaults.propagation_hours;
         if (policy.retention_days < 1 || policy.retention_days > 365)
            policy.retention_days = defaults.retention_days;
         if (policy.key_bits != 2048 && policy.key_bits != 3072 && policy.key_bits != 4096)
            policy.key_bits = defaults.key_bits;

         return policy;
      }

      String LowerName(const Domain &domain)
      {
         String name = domain.GetName();
         name.MakeLower();
         return name;
      }

      // The names a selector's record must be published under: the domain's own, and
      // with alias signing on, each of its domain aliases - DKIMSigner signs a message
      // from an alias with d=<alias> and the main domain's selector, so a switch that
      // looked only at the main name would break DKIM for every alias.
      std::vector<AnsiString> HostsFor(const AnsiString &selector, const Domain &domain)
      {
         std::vector<AnsiString> hosts;
         if (selector.IsEmpty())
            return hosts;

         hosts.push_back(selector + "._domainkey." + AnsiString(LowerName(domain)));

         if (domain.GetDKIMAliasesEnabled())
         {
            DomainAliases aliases(domain.GetID());
            aliases.Refresh();

            for (std::shared_ptr<DomainAlias> alias : aliases.GetVector())
            {
               if (!alias || alias->GetAlias().IsEmpty())
                  continue;

               String name = alias->GetAlias();
               name.MakeLower();
               hosts.push_back(selector + "._domainkey." + AnsiString(name));
            }
         }

         return hosts;
      }

      bool SamePath(const String &left, const String &right)
      {
#ifdef HM_PLATFORM_POSIX
         return left == right;
#else
         return left.CompareNoCase(right) == 0;
#endif
      }

      // ------------------------------------------------------------------ checks

      // Checks one slot's record(s) from the resolver and records the outcome on the
      // slot (not the time, which the caller sets). Answers the status.
      int CheckSlot(Slot &slot, const Domain &domain)
      {
         AnsiString keyType;
         std::vector<unsigned char> publicKey;
         String error;

         if (!DkimKeyPair::DeriveFromFile(slot.key_file, keyType, publicKey, error))
         {
            slot.status = Plan::StatusKeyUnreadable;
            slot.detail = error;
            return slot.status;
         }

         for (const AnsiString &host : HostsFor(slot.selector, domain))
         {
            std::vector<String> results;
            DNSResolver resolver;

            if (!resolver.GetTXTRecords(String(host), results))
            {
               slot.status = Plan::StatusLookupFailed;
               slot.detail = String(host);
               return slot.status;
            }

            if (results.empty())
            {
               slot.status = Plan::StatusNotFound;
               slot.detail = String(host);
               return slot.status;
            }

            std::vector<AnsiString> records;
            for (const String &result : results)
               records.push_back(AnsiString(result));

            AnsiString found;
            if (Plan::MatchRecords(records, keyType, publicKey, found) != Plan::StatusMatches)
            {
               slot.status = Plan::StatusDifferent;
               slot.detail = String(host) + _T(": ") + String(found);
               return slot.status;
            }
         }

         slot.status = Plan::StatusMatches;
         slot.detail = String();
         return slot.status;
      }

      void CheckNext(State &state, const Domain &domain, __int64 now)
      {
         int status = CheckSlot(state.next, domain);
         Plan::ApplyNextCheck(state, status, now);
      }

      bool CheckIfStale(Slot &slot, const Domain &domain, __int64 now)
      {
         if (slot.IsEmpty() || slot.key_file.IsEmpty())
            return false;

         if (slot.status != Plan::StatusUnchecked && slot.checked <= now && now - slot.checked < RecheckSeconds)
            return false;

         CheckSlot(slot, domain);
         slot.checked = now;
         return true;
      }

      // ------------------------------------------------------------------ key files

      String Canonical(const String &path)
      {
         boost::system::error_code error_code;
         boost::filesystem::path canonical = boost::filesystem::canonical(boost::filesystem::path(std::wstring(path)), error_code);
         if (error_code)
            return String();
         return String(canonical.wstring().c_str());
      }

      enum Removal
      {
         Removed = 0,
         KeptNotGenerated,
         KeptOutsideDirectory,
         KeptNotAKey,
         KeptInUse,
         RemoveFailed
      };

      const char *RemovalWords(Removal removal)
      {
         switch (removal)
         {
         case KeptNotGenerated:
            return "the rotation did not generate it";
         case KeptOutsideDirectory:
            return "it is not in the rotation's key directory";
         case KeptNotAKey:
            return "it does not hold a private key";
         case KeptInUse:
            return "another domain or rotation still names it";
         case RemoveFailed:
            return "it could not be deleted";
         default:
            return "it was removed";
         }
      }

      // The whole of the deletion guard. excludeDomainId is a domain whose own
      // references do not count - the one being deleted.
      Removal RemoveGeneratedKey(const String &path, bool generated, __int64 excludeDomainId)
      {
         if (path.IsEmpty())
            return Removed;

         if (!generated)
            return KeptNotGenerated;

         if (!FileUtilities::Exists(path))
            return Removed;

         const String directory = Canonical(DkimRotation::KeyDirectory());
         const String file = Canonical(path);

         if (directory.IsEmpty() || file.IsEmpty())
            return KeptOutsideDirectory;

         const String parent = String(boost::filesystem::path(std::wstring(file)).parent_path().wstring().c_str());
         if (!SamePath(parent, directory))
            return KeptOutsideDirectory;

         AnsiString keyType;
         std::vector<unsigned char> publicKey;
         String readError;
         if (!DkimKeyPair::DeriveFromFile(path, keyType, publicKey, readError))
            return KeptNotAKey;

         Domains domains;
         domains.Refresh();
         for (std::shared_ptr<Domain> domain : domains.GetVector())
         {
            if (!domain || domain->GetID() == excludeDomainId)
               continue;

            const String primary = domain->GetDKIMPrivateKeyFile();
            const String secondary = domain->GetDKIMSecondaryPrivateKeyFile();

            if ((!primary.IsEmpty() && (SamePath(primary, path) || SamePath(Canonical(primary), file))) ||
                (!secondary.IsEmpty() && (SamePath(secondary, path) || SamePath(Canonical(secondary), file))))
               return KeptInUse;
         }

         std::vector<std::pair<__int64, String> > retiring;
         if (!PersistentDkimRotation::ListRetiringKeyFiles(retiring))
            return KeptInUse;

         for (const std::pair<__int64, String> &entry : retiring)
         {
            if (entry.first == excludeDomainId)
               continue;
            if (SamePath(entry.second, path) || SamePath(Canonical(entry.second), file))
               return KeptInUse;
         }

         boost::system::error_code error_code;
         boost::filesystem::remove(boost::filesystem::path(std::wstring(path)), error_code);

         if (error_code)
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6615, "DkimRotation::RemoveGeneratedKey",
               Formatter::Format("The DKIM key file {0}, generated by the key rotation and no longer used, could not be deleted: {1}",
                                 path, String(error_code.message().c_str())));
            return RemoveFailed;
         }

         return Removed;
      }

      // ------------------------------------------------------------------ the row

      bool ReadRow(__int64 domainId, State &state, bool &exists)
      {
         if (PersistentDkimRotation::Read(domainId, state, exists))
            return true;

         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6613, "DkimRotation::ReadRow",
            Formatter::Format("The DKIM key rotation state of domain {0} could not be read from hm_dkimrotations.", domainId));
         return false;
      }

      bool SaveRow(__int64 domainId, const State &state)
      {
         if (PersistentDkimRotation::Save(domainId, state))
            return true;

         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6613, "DkimRotation::SaveRow",
            Formatter::Format("The DKIM key rotation state of domain {0} could not be written to hm_dkimrotations.", domainId));
         return false;
      }

      // Brings the row into line with what the domain actually holds. Somebody may
      // have staged a secondary by hand, promoted one, cancelled one, or restored a
      // backup; the domain's own columns are the truth and the row follows them.
      bool Reconcile(const Domain &domain, State &state, __int64 now, bool removeOrphans)
      {
         bool changed = false;

         const AnsiString primary = domain.GetDKIMSelector();
         const String primaryFile = domain.GetDKIMPrivateKeyFile();
         const AnsiString secondary = domain.GetDKIMSecondarySelector();
         const String secondaryFile = domain.GetDKIMSecondaryPrivateKeyFile();

         if (secondary.IsEmpty())
         {
            if (!state.next.IsEmpty())
            {
               if (state.next.selector == primary && SamePath(state.next.key_file, primaryFile))
               {
                  // Promoted outside the rotation. What it would have done itself:
                  // the old key retires, the new one is current from now.
                  if (!state.current.IsEmpty() && state.current.selector != primary && state.retiring.IsEmpty())
                  {
                     state.retiring = state.current;
                     state.retiring.since = now;
                  }

                  state.current = state.next;
                  state.current.since = now;
               }
               else
               {
                  // Cancelled outside the rotation. A key the rotation made for it
                  // never signed anything.
                  if (removeOrphans)
                     RemoveGeneratedKey(state.next.key_file, state.next.generated, domain.GetID());
               }

               state.next = Slot();
               state.next_first_seen = 0;
               changed = true;
            }
         }
         else if (secondary != state.next.selector || !SamePath(secondaryFile, state.next.key_file))
         {
            if (removeOrphans && !state.next.IsEmpty() && !SamePath(state.next.key_file, secondaryFile))
               RemoveGeneratedKey(state.next.key_file, state.next.generated, domain.GetID());

            Slot adopted;
            adopted.selector = secondary;
            adopted.key_file = secondaryFile;
            adopted.generated = false;
            adopted.since = now;
            state.next = adopted;
            state.next_first_seen = 0;
            changed = true;
         }

         if (primary != state.current.selector || !SamePath(primaryFile, state.current.key_file))
         {
            Slot adopted;
            if (!primary.IsEmpty())
            {
               adopted.selector = primary;
               adopted.key_file = primaryFile;
               adopted.generated = false;

               // A key adopted into the rotation is as old as its file: a key made two
               // years ago is due for rotation the moment the schedule is switched on,
               // which is the ordinary practice this exists for.
               __int64 modified = 0;
               adopted.since = ProtectedFile::ModifiedAt(primaryFile, modified) && modified > 0 && modified <= now ? modified : now;
            }

            state.current = adopted;
            changed = true;
         }

         if (!state.retiring.IsEmpty() && (state.retiring.selector == primary || state.retiring.selector == secondary))
         {
            // Back in use, so not retiring.
            state.retiring = Slot();
            changed = true;
         }

         return changed;
      }

      // ------------------------------------------------------------------ the status document

      AnsiString RecordsJson(const Slot &slot, const Domain &domain, const char *action)
      {
         AnsiString value;
         AnsiString keyType;
         std::vector<unsigned char> publicKey;
         String error;
         if (DkimKeyPair::DeriveFromFile(slot.key_file, keyType, publicKey, error))
            value = DkimKeyPair::RecordValue(keyType, publicKey);

         AnsiString json = "[";
         bool first = true;
         for (const AnsiString &host : HostsFor(slot.selector, domain))
         {
            if (!first)
               json += ",";
            first = false;
            json += "{\"host\":" + Quote(host) + ",\"type\":\"TXT\",\"value\":" + Quote(value) + ",\"action\":\"" + action + "\"}";
         }
         json += "]";
         return json;
      }

      AnsiString CheckJson(const Slot &slot)
      {
         return "{\"status\":\"" + AnsiString(Plan::StatusWord(slot.status)) + "\",\"checked\":" + Number(slot.checked) +
                ",\"detail\":" + Quote(slot.detail) + "}";
      }

      AnsiString StatusJson(const Domain &domain, const State &state, __int64 now)
      {
         const Plan::Policy policy = PolicyOf(domain);
         const Plan::Decision decision = Plan::Decide(policy, state, now);

         AnsiString json = "{\"domain\":" + Quote(domain.GetName()) + ",\"now\":" + Number(now);

         json += ",\"policy\":{\"enabled\":" + Bool(policy.enabled) +
                 ",\"interval_days\":" + Number(policy.interval_days) +
                 ",\"propagation_hours\":" + Number(policy.propagation_hours) +
                 ",\"retention_days\":" + Number(policy.retention_days) +
                 ",\"key_bits\":" + Number(policy.key_bits) + "}";

         json += ",\"phase\":\"" + decision.phase + "\"";

         if (state.current.IsEmpty())
            json += ",\"current\":null";
         else
            json += ",\"current\":{\"selector\":" + Quote(state.current.selector) +
                    ",\"key_file\":" + Quote(state.current.key_file) +
                    ",\"generated\":" + Bool(state.current.generated) +
                    ",\"since\":" + Number(state.current.since) +
                    ",\"next_rotation_due\":" + Number(Plan::NextRotationDue(policy, state)) +
                    ",\"records\":" + RecordsJson(state.current, domain, "keep") +
                    ",\"check\":" + CheckJson(state.current) + "}";

         if (state.next.IsEmpty())
            json += ",\"next\":null";
         else
            json += ",\"next\":{\"selector\":" + Quote(state.next.selector) +
                    ",\"key_file\":" + Quote(state.next.key_file) +
                    ",\"generated\":" + Bool(state.next.generated) +
                    ",\"created\":" + Number(state.next.since) +
                    ",\"first_seen\":" + Number(state.next_first_seen) +
                    ",\"switch_due\":" + Number(Plan::SwitchDue(policy, state)) +
                    ",\"records\":" + RecordsJson(state.next, domain, "publish") +
                    ",\"check\":" + CheckJson(state.next) + "}";

         if (state.retiring.IsEmpty())
            json += ",\"retiring\":null";
         else
            json += ",\"retiring\":{\"selector\":" + Quote(state.retiring.selector) +
                    ",\"key_file\":" + Quote(state.retiring.key_file) +
                    ",\"generated\":" + Bool(state.retiring.generated) +
                    ",\"since\":" + Number(state.retiring.since) +
                    ",\"retire_due\":" + Number(Plan::RetireDue(policy, state)) +
                    ",\"records\":" + RecordsJson(state.retiring, domain, "keep") +
                    ",\"check\":" + CheckJson(state.retiring) + "}";

         if (state.retired_selector.IsEmpty())
         {
            json += ",\"retired\":null";
         }
         else
         {
            AnsiString records = "[";
            bool first = true;
            for (const AnsiString &host : HostsFor(state.retired_selector, domain))
            {
               if (!first)
                  records += ",";
               first = false;
               records += "{\"host\":" + Quote(host) + ",\"type\":\"TXT\",\"action\":\"remove\"}";
            }
            records += "]";

            json += ",\"retired\":{\"selector\":" + Quote(state.retired_selector) +
                    ",\"at\":" + Number(state.retired_at) +
                    ",\"key_file_kept\":" + Quote(state.retired_key_file) +
                    ",\"records\":" + records + "}";
         }

         json += ",\"waiting\":[";
         for (size_t i = 0; i < decision.waiting.size(); i++)
         {
            const Plan::Waiting &waiting = decision.waiting[i];
            if (i > 0)
               json += ",";
            json += "{\"step\":\"" + waiting.step + "\",\"reason\":\"" + waiting.reason + "\",\"until\":" + Number(waiting.until) +
                    ",\"detail\":" + Quote(waiting.detail) + "}";
         }
         json += "]";

         if (state.last_error.IsEmpty())
            json += ",\"last_error\":null";
         else
            json += ",\"last_error\":{\"time\":" + Number(state.last_error_time) + ",\"text\":" + Quote(state.last_error) + "}";

         json += "}";
         return json;
      }

      Result Refuse(int status, const String &sentence)
      {
         Result result;
         result.http_status = status;
         result.error = sentence;
         return result;
      }

      Result Refuse(int status, const String &sentence, const AnsiString &reason, const Domain &domain, const State &state, __int64 now)
      {
         Result result = Refuse(status, sentence);
         result.reason = reason;
         result.json = StatusJson(domain, state, now);
         return result;
      }

      Result Answer(int status, const Domain &domain, const State &state, __int64 now)
      {
         Result result;
         result.http_status = status;
         result.json = StatusJson(domain, state, now);
         return result;
      }

      void RememberError(State &state, const String &sentence, __int64 now)
      {
         state.last_error = sentence;
         state.last_error_time = now;
      }

      // ------------------------------------------------------------------ the operations

      String TakenFileName(const String &directory, const String &base)
      {
         String candidate = directory + FileUtilities::PathSeparator + base + _T(".pem");
         for (int attempt = 2; FileUtilities::Exists(candidate) && attempt < 100; attempt++)
            candidate = directory + FileUtilities::PathSeparator + base + Formatter::Format(_T("-{0}.pem"), attempt);
         return candidate;
      }

      Result Generate(std::shared_ptr<Domain> &domain, State &state, const AnsiString &requested, __int64 now)
      {
         const Plan::Policy policy = PolicyOf(*domain);

         if (!state.next.IsEmpty() || !domain->GetDKIMSecondarySelector().IsEmpty() || !domain->GetDKIMSecondaryPrivateKeyFile().IsEmpty())
            return Refuse(409, _T("A next key is already staged for this domain. Switch to it or cancel it before starting another rotation."),
                          "next_staged", *domain, state, now);

         std::set<AnsiString> taken;
         for (const AnsiString &selector : { state.current.selector, state.retiring.selector, state.retired_selector,
                                             domain->GetDKIMSelector() })
         {
            if (!selector.IsEmpty())
               taken.insert(selector);
         }

         AnsiString selector = requested;
         selector.Trim();
         if (selector.IsEmpty())
            selector = Plan::SuggestSelector(now, taken);

         if (!Plan::IsValidSelector(selector))
            return Refuse(400, _T("The selector must be DNS labels of lower-case letters, digits and hyphens, joined by dots, at most 63 characters, with no underscore, slash or backslash."));

         if (taken.find(selector) != taken.end())
            return Refuse(409, Formatter::Format(_T("The selector {0} is in use by this domain, or was until recently. A rotation needs a selector of its own."), String(selector)),
                          "selector_in_use", *domain, state, now);

         const String directory = DkimRotation::KeyDirectory();
         if (directory.IsEmpty())
            return Refuse(500, _T("The data directory is not configured, so there is nowhere to keep the key."));

         if (!FileUtilities::DirectoryExists(directory) && !FileUtilities::CreateDirectory(directory))
         {
            const String sentence = Formatter::Format(_T("The DKIM key directory {0} could not be created."), directory);
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6612, "DkimRotation::Generate", sentence);
            RememberError(state, sentence, now);
            return Refuse(500, sentence);
         }

         AnsiString pem;
         String error;
         if (!DkimKeyPair::Generate(policy.key_bits, pem, error))
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6610, "DkimRotation::Generate", error);
            RememberError(state, error, now);
            return Refuse(500, error);
         }

         const String path = TakenFileName(directory, String(selector) + _T("._domainkey.") + LowerName(*domain));

         if (!ProtectedFile::Write(path, pem, error))
         {
            const String sentence = Formatter::Format(_T("The DKIM key file could not be written with protected permissions: {0}"), error);
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6611, "DkimRotation::Generate", sentence);
            RememberError(state, sentence, now);
            return Refuse(500, sentence);
         }

         // Staged only if nothing was staged a moment ago by somebody else.
         SQLCommand stage("update hm_domains set domaindkimsecondaryselector = @DOMAINDKIMSECONDARYSELECTOR, "
                          "domaindkimsecondaryprivatekeyfile = @DOMAINDKIMSECONDARYPRIVATEKEYFILE "
                          "where domainid = @DOMAINID and domaindkimsecondaryselector = @EXPECTEDSECONDARYSELECTOR");
         stage.AddParameter("@DOMAINDKIMSECONDARYSELECTOR", String(selector));
         stage.AddParameter("@DOMAINDKIMSECONDARYPRIVATEKEYFILE", path);
         stage.AddParameter("@DOMAINID", domain->GetID());
         stage.AddParameter("@EXPECTEDSECONDARYSELECTOR", String());

         if (!Application::Instance()->GetDBManager()->Execute(stage))
         {
            boost::system::error_code ignored;
            boost::filesystem::remove(boost::filesystem::path(std::wstring(path)), ignored);

            const String sentence = Formatter::Format(_T("The next DKIM key of {0} could not be staged in the database."), domain->GetName());
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6614, "DkimRotation::Generate", sentence);
            RememberError(state, sentence, now);
            return Refuse(500, sentence);
         }

         std::shared_ptr<Domain> reread = ReadDomain(domain->GetID());
         if (!reread || reread->GetDKIMSecondarySelector() != selector || !SamePath(reread->GetDKIMSecondaryPrivateKeyFile(), path))
         {
            boost::system::error_code ignored;
            boost::filesystem::remove(boost::filesystem::path(std::wstring(path)), ignored);

            LOG_APPLICATION(Formatter::Format("DKIM rotation: {0} - another change staged a key at the same moment; the key made here was discarded.", domain->GetName()));

            if (reread)
               domain = reread;

            return Refuse(409, _T("Another change staged a key for this domain at the same moment. Look at the rotation again."),
                          "next_staged", *domain, state, now);
         }

         domain = reread;
         CacheContainer::Instance()->RemoveDomain(domain);

         Slot next;
         next.selector = selector;
         next.key_file = path;
         next.generated = true;
         next.since = now;
         state.next = next;
         state.next_first_seen = 0;
         state.last_error = String();
         state.last_error_time = 0;

         LOG_APPLICATION(Formatter::Format("DKIM rotation: {0} - a {1}-bit key was generated for selector {2} and staged as the next key. "
                                           "Publish its TXT record at {3}; signing moves to it once the record has resolved for {4} hours.",
                                           domain->GetName(), policy.key_bits, String(selector),
                                           String(selector) + _T("._domainkey.") + LowerName(*domain), policy.propagation_hours));

         return Answer(201, *domain, state, now);
      }

      Result SwitchTo(std::shared_ptr<Domain> &domain, State &state, bool force, bool checkFirst, __int64 now)
      {
         const Plan::Policy policy = PolicyOf(*domain);

         if (state.next.IsEmpty())
            return Refuse(409, _T("No next key is staged, so there is nothing to switch to."), "nothing_staged", *domain, state, now);

         if (checkFirst)
            CheckNext(state, *domain, now);

         switch (state.next.status)
         {
         case Plan::StatusMatches:
            break;
         case Plan::StatusNotFound:
            return Refuse(409, Formatter::Format(_T("The next key's record was not found at {0}. Publish it first; a switch never happens before the record resolves."), state.next.detail),
                          "record_not_found", *domain, state, now);
         case Plan::StatusDifferent:
            return Refuse(409, Formatter::Format(_T("The record published for the next key does not carry its public key ({0}). Correct the record."), state.next.detail),
                          "record_differs", *domain, state, now);
         case Plan::StatusLookupFailed:
            return Refuse(409, Formatter::Format(_T("The DNS lookup of {0} failed, so the record could not be confirmed."), state.next.detail),
                          "lookup_failed", *domain, state, now);
         case Plan::StatusKeyUnreadable:
            return Refuse(409, Formatter::Format(_T("The next key could not be read: {0}"), state.next.detail),
                          "key_unreadable", *domain, state, now);
         default:
            return Refuse(409, _T("The next key's record has not been checked."), "unchecked", *domain, state, now);
         }

         if (!force)
         {
            const __int64 due = Plan::SwitchDue(policy, state);
            if (due <= 0 || now < due)
               return Refuse(409, Formatter::Format(_T("The record has not yet been published for the propagation period; the switch is due at {0}."), UtcText(due)),
                             "propagation", *domain, state, now);
         }

         if (!state.retiring.IsEmpty())
            return Refuse(409, Formatter::Format(_T("The selector {0} is still being kept for late verifiers. Retire it first, or wait until {1}."),
                                                 String(state.retiring.selector), UtcText(Plan::RetireDue(policy, state))),
                          "retiring_slot", *domain, state, now);

         const State previous = state;
         const AnsiString oldSelector = state.current.selector;
         const AnsiString nextSelector = state.next.selector;

         // The row first. Retiring names the old primary, never the new one, so a
         // failure after this line leaves nothing a later pass could delete wrongly.
         if (!state.current.IsEmpty())
         {
            state.retiring = state.current;
            state.retiring.since = now;
         }

         state.current = state.next;
         state.current.since = now;
         state.next = Slot();
         state.next_first_seen = 0;

         if (!SaveRow(domain->GetID(), state))
         {
            state = previous;
            return Refuse(500, _T("The rotation state could not be written, so signing was not moved."));
         }

         SQLCommand move("update hm_domains set domaindkimselector = @DOMAINDKIMSELECTOR, domaindkimprivatekeyfile = @DOMAINDKIMPRIVATEKEYFILE, "
                         "domaindkimsecondaryselector = @DOMAINDKIMSECONDARYSELECTOR, domaindkimsecondaryprivatekeyfile = @DOMAINDKIMSECONDARYPRIVATEKEYFILE "
                         "where domainid = @DOMAINID and domaindkimselector = @EXPECTEDSELECTOR and domaindkimsecondaryselector = @EXPECTEDNEXTSELECTOR");
         move.AddParameter("@DOMAINDKIMSELECTOR", String(state.current.selector));
         move.AddParameter("@DOMAINDKIMPRIVATEKEYFILE", state.current.key_file);
         move.AddParameter("@DOMAINDKIMSECONDARYSELECTOR", String());
         move.AddParameter("@DOMAINDKIMSECONDARYPRIVATEKEYFILE", String());
         move.AddParameter("@DOMAINID", domain->GetID());
         move.AddParameter("@EXPECTEDSELECTOR", String(domain->GetDKIMSelector()));
         move.AddParameter("@EXPECTEDNEXTSELECTOR", String(nextSelector));

         if (!Application::Instance()->GetDBManager()->Execute(move))
         {
            const String sentence = Formatter::Format(_T("Signing for {0} could not be moved to selector {1}: the domain could not be updated in the database."),
                                                      domain->GetName(), String(nextSelector));
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6614, "DkimRotation::SwitchTo", sentence);

            state = previous;
            RememberError(state, sentence, now);
            SaveRow(domain->GetID(), state);
            return Refuse(500, sentence);
         }

         std::shared_ptr<Domain> reread = ReadDomain(domain->GetID());
         if (!reread || reread->GetDKIMSelector() != nextSelector)
         {
            // Somebody changed the domain's selectors between the read and the
            // update. Their change stands; the row goes back to what it was and the
            // next pass reconciles it with whatever they did.
            state = previous;
            SaveRow(domain->GetID(), state);

            LOG_APPLICATION(Formatter::Format("DKIM rotation: {0} - the domain's selectors changed while signing was being moved to {1}; the switch was not made.",
                                              domain->GetName(), String(nextSelector)));

            if (reread)
               domain = reread;

            return Refuse(409, _T("The domain's DKIM selectors changed while the switch was being made. Look at the rotation again."),
                          "changed", *domain, state, now);
         }

         domain = reread;

         // The signer reads the cache; without this it would go on signing with the
         // old key until the entry expired - and a retire in that window would delete
         // a key still in use.
         CacheContainer::Instance()->RemoveDomain(domain);

         state.last_error = String();
         state.last_error_time = 0;
         SaveRow(domain->GetID(), state);

         if (oldSelector.IsEmpty())
         {
            LOG_APPLICATION(Formatter::Format("DKIM rotation: {0} now signs with selector {1}.", domain->GetName(), String(nextSelector)));
         }
         else
         {
            LOG_APPLICATION(Formatter::Format("DKIM rotation: {0} now signs with selector {1}{2}. Keep the TXT record at {3} published until {4}, for mail signed with the old key that is still on its way.",
                                              domain->GetName(), String(nextSelector), String(force ? _T(" (the propagation period was skipped on request)") : _T("")),
                                              String(oldSelector) + _T("._domainkey.") + LowerName(*domain), UtcText(Plan::RetireDue(policy, state))));
         }

         return Answer(200, *domain, state, now);
      }

      Result RetireOld(std::shared_ptr<Domain> &domain, State &state, __int64 now)
      {
         if (state.retiring.IsEmpty())
            return Refuse(409, _T("No selector is retiring."), "nothing_retiring", *domain, state, now);

         const Slot old = state.retiring;
         const Removal removal = RemoveGeneratedKey(old.key_file, old.generated, 0);

         state.retired_selector = old.selector;
         state.retired_at = now;
         state.retired_key_file = removal == Removed ? String() : old.key_file;
         state.retiring = Slot();

         if (!SaveRow(domain->GetID(), state))
            return Refuse(500, _T("The rotation state could not be written."));

         LOG_APPLICATION(Formatter::Format("DKIM rotation: {0} retired selector {1}. The TXT record at {2} may now be removed. Its key file {3}: {4}.",
                                           domain->GetName(), String(old.selector), String(old.selector) + _T("._domainkey.") + LowerName(*domain),
                                           old.key_file, String(RemovalWords(removal))));

         return Answer(200, *domain, state, now);
      }

      Result CancelNext(std::shared_ptr<Domain> &domain, State &state, __int64 now)
      {
         const AnsiString staged = domain->GetDKIMSecondarySelector();

         if (staged.IsEmpty() && domain->GetDKIMSecondaryPrivateKeyFile().IsEmpty())
            return Refuse(409, _T("No next key is staged, so there is nothing to cancel."), "nothing_staged", *domain, state, now);

         SQLCommand clear("update hm_domains set domaindkimsecondaryselector = @DOMAINDKIMSECONDARYSELECTOR, "
                          "domaindkimsecondaryprivatekeyfile = @DOMAINDKIMSECONDARYPRIVATEKEYFILE "
                          "where domainid = @DOMAINID and domaindkimsecondaryselector = @EXPECTEDNEXTSELECTOR");
         clear.AddParameter("@DOMAINDKIMSECONDARYSELECTOR", String());
         clear.AddParameter("@DOMAINDKIMSECONDARYPRIVATEKEYFILE", String());
         clear.AddParameter("@DOMAINID", domain->GetID());
         clear.AddParameter("@EXPECTEDNEXTSELECTOR", String(staged));

         if (!Application::Instance()->GetDBManager()->Execute(clear))
         {
            const String sentence = Formatter::Format(_T("The next DKIM key of {0} could not be removed from the domain in the database."), domain->GetName());
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6614, "DkimRotation::CancelNext", sentence);
            return Refuse(500, sentence);
         }

         std::shared_ptr<Domain> reread = ReadDomain(domain->GetID());
         if (!reread || !reread->GetDKIMSecondarySelector().IsEmpty())
         {
            if (reread)
               domain = reread;
            return Refuse(409, _T("The domain's next key changed while it was being cancelled. Look at the rotation again."), "changed", *domain, state, now);
         }

         domain = reread;
         CacheContainer::Instance()->RemoveDomain(domain);

         const Slot cancelled = state.next;
         Removal removal = KeptNotGenerated;
         if (!cancelled.IsEmpty() && cancelled.selector == staged)
            removal = RemoveGeneratedKey(cancelled.key_file, cancelled.generated, 0);

         state.next = Slot();
         state.next_first_seen = 0;
         state.last_error = String();
         state.last_error_time = 0;

         if (!SaveRow(domain->GetID(), state))
            return Refuse(500, _T("The rotation state could not be written."));

         LOG_APPLICATION(Formatter::Format("DKIM rotation: {0} - the next key, selector {1}, was cancelled; signing is unchanged. Its key file: {2}.",
                                           domain->GetName(), String(staged), String(RemovalWords(removal))));

         return Answer(200, *domain, state, now);
      }

      // Loads the domain and its row, reconciled. False with result set when either
      // cannot be had.
      bool Load(__int64 domainId, __int64 now, std::shared_ptr<Domain> &domain, State &state, bool &exists, bool &changed, Result &result,
                bool removeOrphans = true)
      {
         domain = ReadDomain(domainId);
         if (!domain)
         {
            result = Refuse(404, _T("domain not found"));
            return false;
         }

         if (!ReadRow(domainId, state, exists))
         {
            result = Refuse(500, _T("The rotation state could not be read."));
            return false;
         }

         changed = Reconcile(*domain, state, now, removeOrphans);
         return true;
      }
   }

   // ---------------------------------------------------------------------- public

   String
   DkimRotation::KeyDirectory()
   {
      const String data = IniFileSettings::Instance()->GetDataDirectory();
      if (data.IsEmpty())
         return String();

      return data + FileUtilities::PathSeparator + _T("DKIM");
   }

   AnsiString
   DkimRotation::RunSummary::ToJson() const
   {
      return "{\"as_of\":" + Number(as_of) + ",\"domains\":" + Number(domains) + ",\"generated\":" + Number(generated) +
             ",\"checked\":" + Number(checked) + ",\"switched\":" + Number(switched) + ",\"retired\":" + Number(retired) +
             ",\"errors\":" + Number(errors) + "}";
   }

   DkimRotation::RunSummary
   DkimRotation::RunAll(__int64 now)
   {
      RunSummary summary;
      summary.as_of = now;

      std::vector<__int64> domainIds;
      if (!PersistentDkimRotation::ListInFlight(domainIds))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6613, "DkimRotation::RunAll",
            "The domains with a DKIM key rotation in flight could not be listed.");
         summary.errors++;
         return summary;
      }

      summary.domains = (int) domainIds.size();

      for (__int64 domainId : domainIds)
      {
         boost::lock_guard<boost::recursive_mutex> guard(Mutex());

         std::shared_ptr<Domain> domain;
         State state;
         bool exists = false, changed = false;
         Result loaded;

         if (!Load(domainId, now, domain, state, exists, changed, loaded))
         {
            if (loaded.http_status != 404)
               summary.errors++;
            continue;
         }

         if (!state.next.IsEmpty())
         {
            CheckNext(state, *domain, now);
            summary.checked++;
            changed = true;
         }

         if (CheckIfStale(state.current, *domain, now))
         {
            summary.checked++;
            changed = true;
         }

         if (CheckIfStale(state.retiring, *domain, now))
         {
            summary.checked++;
            changed = true;
         }

         const Plan::Decision decision = Plan::Decide(PolicyOf(*domain), state, now);

         if (decision.retire_now)
         {
            Result retired = RetireOld(domain, state, now);
            if (retired.Succeeded())
               summary.retired++;
            else
               summary.errors++;
            changed = true;
         }

         if (decision.switch_now)
         {
            // The check above was made in this pass, which is the check the switch
            // needs; it is not made twice.
            Result switched = SwitchTo(domain, state, false, false, now);
            if (switched.Succeeded())
            {
               summary.switched++;
            }
            else if (switched.http_status == 500 || switched.reason == "changed")
            {
               summary.errors++;
               RememberError(state, switched.error, now);
            }
            changed = true;
         }

         if (decision.generate)
         {
            Result generated = Generate(domain, state, AnsiString(), now);
            if (generated.Succeeded())
            {
               summary.generated++;
            }
            else
            {
               summary.errors++;
               RememberError(state, generated.error, now);
            }
            changed = true;
         }

         if (changed || !exists)
            SaveRow(domainId, state);
      }

      return summary;
   }

   DkimRotation::Result
   DkimRotation::Status(__int64 domainId, __int64 now)
   {
      boost::lock_guard<boost::recursive_mutex> guard(Mutex());

      std::shared_ptr<Domain> domain;
      State state;
      bool exists = false, changed = false;
      Result result;

      if (!Load(domainId, now, domain, state, exists, changed, result, false))
         return result;

      // Reconciled in memory only: reading the status writes nothing and deletes
      // nothing, so a domain with the rotation off is never given a row by being
      // looked at.
      return Answer(200, *domain, state, now);
   }

   DkimRotation::Result
   DkimRotation::Start(__int64 domainId, const AnsiString &selector, __int64 now)
   {
      boost::lock_guard<boost::recursive_mutex> guard(Mutex());

      std::shared_ptr<Domain> domain;
      State state;
      bool exists = false, changed = false;
      Result result;

      if (!Load(domainId, now, domain, state, exists, changed, result))
         return result;

      result = Generate(domain, state, selector, now);

      if (result.Succeeded() || changed || !state.last_error.IsEmpty())
         SaveRow(domainId, state);

      return result;
   }

   DkimRotation::Result
   DkimRotation::CheckNow(__int64 domainId, __int64 now)
   {
      boost::lock_guard<boost::recursive_mutex> guard(Mutex());

      std::shared_ptr<Domain> domain;
      State state;
      bool exists = false, changed = false;
      Result result;

      if (!Load(domainId, now, domain, state, exists, changed, result))
         return result;

      if (!state.next.IsEmpty())
         CheckNext(state, *domain, now);

      if (!state.current.IsEmpty() && !state.current.key_file.IsEmpty())
      {
         CheckSlot(state.current, *domain);
         state.current.checked = now;
      }

      if (!state.retiring.IsEmpty())
      {
         CheckSlot(state.retiring, *domain);
         state.retiring.checked = now;
      }

      if (!SaveRow(domainId, state))
         return Refuse(500, _T("The rotation state could not be written."));

      return Answer(200, *domain, state, now);
   }

   DkimRotation::Result
   DkimRotation::Switch(__int64 domainId, bool force, __int64 now)
   {
      boost::lock_guard<boost::recursive_mutex> guard(Mutex());

      std::shared_ptr<Domain> domain;
      State state;
      bool exists = false, changed = false;
      Result result;

      if (!Load(domainId, now, domain, state, exists, changed, result))
         return result;

      result = SwitchTo(domain, state, force, true, now);

      // A refused switch still made a check worth keeping. A switch that failed in
      // the database has already put its row back.
      if (!result.Succeeded() && result.http_status != 500)
         SaveRow(domainId, state);

      return result;
   }

   DkimRotation::Result
   DkimRotation::Retire(__int64 domainId, __int64 now)
   {
      boost::lock_guard<boost::recursive_mutex> guard(Mutex());

      std::shared_ptr<Domain> domain;
      State state;
      bool exists = false, changed = false;
      Result result;

      if (!Load(domainId, now, domain, state, exists, changed, result))
         return result;

      result = RetireOld(domain, state, now);

      if (!result.Succeeded() && changed)
         SaveRow(domainId, state);

      return result;
   }

   DkimRotation::Result
   DkimRotation::Cancel(__int64 domainId, __int64 now)
   {
      boost::lock_guard<boost::recursive_mutex> guard(Mutex());

      std::shared_ptr<Domain> domain;
      State state;
      bool exists = false, changed = false;
      Result result;

      if (!Load(domainId, now, domain, state, exists, changed, result))
         return result;

      result = CancelNext(domain, state, now);

      if (!result.Succeeded() && changed)
         SaveRow(domainId, state);

      return result;
   }

   void
   DkimRotation::OnDomainDeleted(__int64 domainId)
   {
      boost::lock_guard<boost::recursive_mutex> guard(Mutex());

      State state;
      bool exists = false;
      if (!PersistentDkimRotation::Read(domainId, state, exists) || !exists)
         return;

      std::shared_ptr<Domain> domain = ReadDomain(domainId);

      // The row's generated flags are only trusted for the files the domain still
      // names, or the retiring file the row itself holds - a row that has drifted
      // from its domain does not get to delete what the domain no longer points at.
      if (domain)
      {
         if (state.current.generated && SamePath(state.current.key_file, domain->GetDKIMPrivateKeyFile()))
            RemoveGeneratedKey(state.current.key_file, true, domainId);

         if (state.next.generated && SamePath(state.next.key_file, domain->GetDKIMSecondaryPrivateKeyFile()))
            RemoveGeneratedKey(state.next.key_file, true, domainId);
      }

      RemoveGeneratedKey(state.retiring.key_file, state.retiring.generated, domainId);
   }

   bool
   DkimRotation::IsManaged(const Domain &stored)
   {
      if (stored.GetDKIMRotationEnabled())
         return true;

      if (stored.GetID() <= 0)
         return false;

      State state;
      bool exists = false;
      if (!PersistentDkimRotation::Read(stored.GetID(), state, exists))
         return true;   // unknown is treated as managed: refusing a save is recoverable, a lost switch is not

      return exists && (!state.next.IsEmpty() || !state.retiring.IsEmpty());
   }

   bool
   DkimRotation::RefusesKeyChange(const Domain &stored, const Domain &proposed, String &reason)
   {
      if (stored.GetID() <= 0)
         return false;

      const bool unchanged =
         stored.GetDKIMSelector() == proposed.GetDKIMSelector() &&
         stored.GetDKIMPrivateKeyFile() == proposed.GetDKIMPrivateKeyFile() &&
         stored.GetDKIMSecondarySelector() == proposed.GetDKIMSecondarySelector() &&
         stored.GetDKIMSecondaryPrivateKeyFile() == proposed.GetDKIMSecondaryPrivateKeyFile();

      if (unchanged)
         return false;

      if (!IsManaged(stored))
         return false;

      reason = Formatter::Format(_T("The DKIM selectors and key files of {0} are managed by its key rotation, and this save would change them - "
                                    "probably from values read before the rotation last moved. Use the rotation's own operations "
                                    "(rotate now, switch, retire, cancel), or cancel the rotation and switch its schedule off first."),
                                 stored.GetName());
      return true;
   }

   bool
   DkimRotation::CollectStalled(__int64 now, int thresholdDays, String &summary)
   {
      summary = String();

      std::vector<std::pair<__int64, State> > rows;
      if (!PersistentDkimRotation::ListAll(rows))
         return false;

      if (thresholdDays < 1)
         thresholdDays = 1;

      int count = 0;
      for (const std::pair<__int64, State> &row : rows)
      {
         const State &state = row.second;

         // Measured to the rotation's own last look when that is later than now:
         // a pass run with the clock moved forward has looked at a later moment
         // than the wall clock, and what it saw then is what is stuck.
         const __int64 lookedAt = state.next.checked > now ? state.next.checked : now;
         const bool waitingTooLong = !state.next.IsEmpty() && state.next_first_seen <= 0 &&
                                     lookedAt - state.next.since >= (__int64) thresholdDays * Plan::SecondsPerDay;
         const bool failed = !state.last_error.IsEmpty() && state.last_error_time > 0;

         if (!waitingTooLong && !failed)
            continue;

         std::shared_ptr<Domain> domain = ReadDomain(row.first);
         const String name = domain ? domain->GetName() : Formatter::Format(_T("(domain {0})"), row.first);

         String line;
         if (failed)
            line = Formatter::Format(_T("{0}: {1}"), name, state.last_error);
         else
            line = Formatter::Format(_T("{0}: the record for selector {1} has not been seen since the key was made on {2}"),
                                     name, String(state.next.selector), UtcText(state.next.since));

         if (count > 0)
            summary += _T("; ");
         summary += line;
         count++;
      }

      return count > 0;
   }
}
