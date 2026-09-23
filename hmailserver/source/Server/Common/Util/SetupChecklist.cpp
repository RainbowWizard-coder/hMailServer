// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The first-hour checklist. See SetupChecklist.h.
//
// THE DOCUMENT
//
//   {"generated": <unix time>, "dns_looked_up": true,
//    "done": 3, "action_needed": 4, "cannot_tell": 0, "not_needed": 1,
//    "items": [
//      {"key": "domain", "state": "done",
//       "findings": [{"state": "done", "code": "domain_count", "args": ["1", "1"],
//                     "text": "Active domains: 1 of 1."}]},
//      ...]}
//
// The items, in the order a new server meets them: profile, domain, account,
// submission, spf, dkim, dmarc, backup, second_factor. The profile item is first
// because a profile sets thirty of the settings the rest of the first hour is
// spent finding (Application/Profiles.h, docs/Profiles.md); it asks for one on a
// young server - fewer than ten accounts, the sample domain's not counted - that
// no profile has been applied to, and says which was applied, and what became of
// it, once one has been. An item's state is the worst of its
// findings' - action_needed over cannot_tell over done over not_needed - and an
// item with no finding at all cannot be told, which never happens by design and
// is the honest answer if it ever does.
//
// THE CODES
//
// A finding's text is the English sentence the code and its arguments render
// to; a client with a catalogue renders the same code in its own language and
// a script switches on the code. The arguments are strings whatever they count.
//
//   profile_none             No profile has been applied. (a young server)
//   profile_in_service       This server holds {0} accounts ... (not a young one)
//   profile_applied          The {0} profile was applied on {1}.
//   profile_kept             ... applied on {1}; its way back was discarded.
//   profile_reverted         ... applied and then reverted on {1}.
//   domain_none              No domain has been added yet.
//   domain_none_active       {0} domains, none of them active.
//   domain_count             Active domains: {0} of {1}.
//   sample_domain            {0} is the sample domain and is not counted here.
//   account_none             No account has been added in any domain.
//   account_count            Accounts: {0}.
//   submission_none          No port takes message submission.
//   submission_plain         Port {0} takes submission without TLS.
//   submission_no_certificate Port {0} is set to TLS but has no certificate.
//   submission_tls           Port {0} offers TLS with the certificate {1}.
//   no_active_domain         No active domain, so nothing to publish yet.
//   record_published         {0}: the {1} record at {2} is published.
//   record_missing           {0}: no {1} record at {2}.
//   record_not_looked_up     {0}: the {1} record at {2} was not looked up.
//   lookup_failed            {0}: the lookup for {2} failed.
//   domains_not_looked_up    {0} more active domains were not looked up.
//   dkim_off                 {0}: signing is off.
//   dkim_no_selector         {0}: signing is on but no selector is set.
//   dkim_key_missing         {0}: signing is on but the key file {1} was not found.
//   backup_none              No backup destination is set.
//   backup_folder_missing    The backup destination {0} does not exist.
//   backup_set               Backups go to {0}.
//   second_factor_on         The administrator's password is backed by a one-time code.
//   second_factor_off        The administrator signs in with a password alone.
//
// WHAT COUNTS
//
// The submission port is any SMTP listener that is not port 25 - port 25 is the
// server-to-server port whatever its security says, and clients are not sent
// there. A listener with STARTTLS or implicit TLS and a certificate that exists
// is done; one with no TLS, or TLS with no certificate to start it, needs action,
// and a plain one beside a secured one still needs action, because the plain one
// is the one a badly configured client will use.
//
// An SPF record is a TXT at the domain beginning v=spf1; a DMARC record is a TXT
// at _dmarc.<domain> beginning v=DMARC1; a DKIM record is a TXT at
// <selector>._domainkey.<domain> carrying a p= tag. Presence is what is checked
// here - whether the DKIM record carries the key the domain signs with is the
// DKIM rotation check's business (DkimRotation.h), which compares the record with
// the private key file.
//
// A lookup that fails is cannot_tell, not action_needed: a resolver that is down
// says nothing about the zone. An empty answer, or an answer with no record of
// the kind asked for, is action_needed.
//
// THE SAMPLE DOMAIN IS NOT A DOMAIN HERE
//
// example.test as the sample made it (Application/SampleDomain.h) is left out of
// every item: it is not the administrator's first domain, its mailboxes are not
// their first accounts, and its records are not theirs to publish - .test is a
// reserved name that no zone answers for, so looking it up would put three
// cannot_tell findings on a list somebody is reading to learn what is left to do.
// The domain item says it is there, with a not_needed finding, so that the reader
// is told why the count is what it is rather than left to wonder. An example.test
// somebody made themselves is a domain like any other and is counted.
#include "StdAfx.h"
#include "SetupChecklist.h"

#include "FileUtilities.h"
#include "Unicode.h"
#include "../Application/Configuration.h"
#include "../Application/IniFileSettings.h"
#include "../Application/Profiles.h"
#include "../Application/SampleDomain.h"
#include "../BO/Domain.h"
#include "../BO/Domains.h"
#include "../BO/SSLCertificate.h"
#include "../BO/SSLCertificates.h"
#include "../BO/TCPIPPort.h"
#include "../BO/TCPIPPorts.h"
#include "../SQL/DALRecordset.h"
#include "../SQL/SQLCommand.h"
#include "../TCPIP/DNSResolver.h"

#include <ctime>
#include <string>
#include <vector>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // Worst last, so that an item's state is the largest of its findings'.
      enum State
      {
         StateNotNeeded = 0,
         StateDone = 1,
         StateCannotTell = 2,
         StateActionNeeded = 3
      };

      struct Finding
      {
         State state;
         AnsiString code;
         std::vector<AnsiString> args;
      };

      struct Item
      {
         AnsiString key;
         std::vector<Finding> findings;
      };

      // How many active domains have their three records looked up. Each missing
      // record costs the resolver's timeout, and a server hosting hundreds of
      // domains is past its first hour: the rest are counted as not looked up, and
      // the Deck's DNS records view checks any one of them on request.
      constexpr int MaxDomainsLookedUp = 20;

      const char *
      StateName(State state)
      {
         switch (state)
         {
         case StateDone: return "done";
         case StateActionNeeded: return "action_needed";
         case StateNotNeeded: return "not_needed";
         default: return "cannot_tell";
         }
      }

      const char *
      EnglishFor(const AnsiString &code)
      {
         static const struct { const char *code; const char *text; } texts[] =
         {
            { "profile_none", "No profile has been applied. A profile sets about thirty settings for the kind of server this is - a small office, behind a filtering gateway, an outbound relay, a high-volume sender - and shows every change before it is made." },
            { "profile_in_service", "This server holds {0} accounts, so it is in service: a profile is offered to a new server, and any of the four can still be compared with this one." },
            { "profile_applied", "The {0} profile was applied on {1}." },
            { "profile_kept", "The {0} profile was applied on {1}; its way back was discarded and its settings kept." },
            { "profile_reverted", "The {0} profile was applied and then reverted on {1}; the settings are the ones it replaced." },
            { "domain_none", "No domain has been added yet." },
            { "domain_none_active", "{0} domains, none of them active." },
            { "domain_count", "Active domains: {0} of {1}." },
            { "sample_domain", "{0} is the sample domain and is not counted here; remove it when you are done with it." },
            { "account_none", "No account has been added in any domain." },
            { "account_count", "Accounts: {0}." },
            { "submission_none", "No port takes message submission, so mail clients cannot send through this server." },
            { "submission_plain", "Port {0} takes submission without TLS, so passwords and mail would cross the network in the clear." },
            { "submission_no_certificate", "Port {0} is set to TLS but has no certificate, so TLS cannot start." },
            { "submission_tls", "Port {0} offers TLS with the certificate {1}." },
            { "no_active_domain", "No active domain, so there is nothing to publish yet." },
            { "record_published", "{0}: the {1} record at {2} is published." },
            { "record_missing", "{0}: no {1} record at {2}." },
            { "record_not_looked_up", "{0}: the {1} record at {2} was not looked up." },
            { "lookup_failed", "{0}: the lookup for {2} failed, which says nothing about the {1} record." },
            { "domains_not_looked_up", "{0} more active domains were not looked up." },
            { "dkim_off", "{0}: signing is off, so receivers cannot verify the mail as this domain's." },
            { "dkim_no_selector", "{0}: signing is on but no selector is set." },
            { "dkim_key_missing", "{0}: signing is on but the key file {1} was not found." },
            { "backup_none", "No backup destination is set." },
            { "backup_folder_missing", "The backup destination {0} does not exist on the server." },
            { "backup_set", "Backups go to {0}." },
            { "second_factor_on", "The administrator's password is backed by a one-time code." },
            { "second_factor_off", "The administrator signs in with a password alone." }
         };

         for (const auto &entry : texts)
         {
            if (code == entry.code)
               return entry.text;
         }

         return "";
      }

      AnsiString
      Render(const AnsiString &sentence, const std::vector<AnsiString> &args)
      {
         AnsiString out = sentence;
         for (size_t i = 0; i < args.size(); i++)
         {
            AnsiString marker;
            marker.Format("{%d}", (int) i);
            size_t at = out.find(marker);
            while (at != std::string::npos)
            {
               out.replace(at, marker.size(), args[i]);
               at = out.find(marker, at + args[i].size());
            }
         }
         return out;
      }

      AnsiString
      Utf8(const String &value)
      {
         AnsiString out;
         Unicode::WideToMultiByte(value, out);
         return out;
      }

      AnsiString
      JsonEscape(const AnsiString &value)
      {
         AnsiString out;
         const char *chars = value.c_str();
         for (size_t i = 0; i < value.size(); i++)
         {
            const unsigned char c = (unsigned char) chars[i];
            switch (c)
            {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
               if (c < 0x20)
               {
                  AnsiString escaped;
                  escaped.Format("\\u%04x", (int) c);
                  out += escaped;
               }
               else
                  out += (char) c;
            }
         }
         return out;
      }

      AnsiString
      JsonString(const AnsiString &value)
      {
         return "\"" + JsonEscape(value) + "\"";
      }

      AnsiString
      Number(long long value)
      {
         return AnsiString(std::to_string(value).c_str());
      }

      void
      Add(Item &item, State state, const char *code, const std::vector<AnsiString> &args = std::vector<AnsiString>())
      {
         Finding finding;
         finding.state = state;
         finding.code = code;
         finding.args = args;
         item.findings.push_back(finding);
      }

      State
      StateOf(const Item &item)
      {
         if (item.findings.empty())
            return StateCannotTell;

         State worst = item.findings[0].state;
         for (const Finding &finding : item.findings)
         {
            if (finding.state > worst)
               worst = finding.state;
         }
         return worst;
      }

      bool
      IsSpfRecord(const String &record)
      {
         String lower = record;
         lower.Trim();
         lower.ToLower();
         return lower.StartsWith(_T("v=spf1"));
      }

      bool
      IsDmarcRecord(const String &record)
      {
         String lower = record;
         lower.Trim();
         lower.ToLower();
         return lower.StartsWith(_T("v=dmarc1"));
      }

      bool
      IsDkimRecord(const String &record)
      {
         String stripped = record;
         stripped.Replace(_T(" "), _T(""));
         return stripped.StartsWith(_T("p=")) || stripped.Find(_T(";p=")) >= 0;
      }

      // One domain's record of one kind: published, missing, not looked up, or
      // the lookup failed. The arguments are the same for all four, so a client
      // renders any of them from the one triple.
      void
      RecordFinding(Item &item, const AnsiString &domain, const char *kind, const AnsiString &host, bool lookUp, bool (*isRecord)(const String &))
      {
         std::vector<AnsiString> args;
         args.push_back(domain);
         args.push_back(kind);
         args.push_back(host);

         if (!lookUp)
         {
            Add(item, StateCannotTell, "record_not_looked_up", args);
            return;
         }

         DNSResolver resolver;
         std::vector<String> records;
         if (!resolver.GetTXTRecords(String(host), records))
         {
            Add(item, StateCannotTell, "lookup_failed", args);
            return;
         }

         for (const String &record : records)
         {
            if (isRecord(record))
            {
               Add(item, StateDone, "record_published", args);
               return;
            }
         }

         Add(item, StateActionNeeded, "record_missing", args);
      }

      // A server with fewer accounts than this, the sample domain's not counted, is
      // young: the checklist asks it to choose a profile. Past it the server is in
      // service, and a profile that closes a listener or turns a filter off is not
      // something to suggest in a list of things left to do.
      constexpr long long YoungAccountLimit = 10;

      AnsiString
      UtcDate(__int64 seconds)
      {
         struct tm parts;
         memset(&parts, 0, sizeof(parts));
#ifdef HM_PLATFORM_POSIX
         time_t value = (time_t) seconds;
         if (::gmtime_r(&value, &parts) == nullptr)
            return AnsiString();
#else
         __time64_t value = seconds;
         if (_gmtime64_s(&parts, &value) != 0)
            return AnsiString();
#endif
         AnsiString text;
         text.Format("%04d-%02d-%02d", parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday);
         return text;
      }

      // Every account on the server, less the sample domain's three when the
      // sample is there. excludeDomainId is 0 when it is not, and the statement
      // is then the one this has always asked.
      long long
      CountAccounts(__int64 excludeDomainId)
      {
         SQLCommand command(excludeDomainId == 0
            ? _T("select count(*) as accountcount from hm_accounts")
            : _T("select count(*) as accountcount from hm_accounts where accountdomainid <> @DOMAINID"));

         if (excludeDomainId != 0)
            command.AddParameter("@DOMAINID", excludeDomainId);

         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (!recordset || recordset->IsEOF())
            return 0;
         return recordset->GetInt64Value("accountcount");
      }

      AnsiString
      ItemJson(const Item &item)
      {
         AnsiString json = "{\"key\":" + JsonString(item.key) + ",\"state\":\"" + AnsiString(StateName(StateOf(item))) + "\",\"findings\":[";
         for (size_t i = 0; i < item.findings.size(); i++)
         {
            const Finding &finding = item.findings[i];
            if (i > 0)
               json += ",";
            json += "{\"state\":\"" + AnsiString(StateName(finding.state)) + "\",\"code\":" + JsonString(finding.code) + ",\"args\":[";
            for (size_t a = 0; a < finding.args.size(); a++)
            {
               if (a > 0)
                  json += ",";
               json += JsonString(finding.args[a]);
            }
            json += "],\"text\":" + JsonString(Render(AnsiString(EnglishFor(finding.code)), finding.args)) + "}";
         }
         json += "]}";
         return json;
      }
   }

   AnsiString
   SetupChecklist::Json(bool lookUpDns)
   {
      Domains domains;
      domains.Refresh();

      // The sample's domain, if the sample made one. 0 otherwise, and 0 for an
      // example.test the sample did not make.
      const __int64 sampleDomainId = SampleDomain::PresentDomainId();
      AnsiString sampleDomainName;

      std::vector<std::shared_ptr<Domain>> active;
      int total = 0;
      for (std::shared_ptr<Domain> domain : domains.GetVector())
      {
         if (!domain)
            continue;
         if (sampleDomainId != 0 && domain->GetID() == sampleDomainId)
         {
            sampleDomainName = Utf8(domain->GetName());
            continue;
         }
         total++;
         if (domain->GetIsActive())
            active.push_back(domain);
      }

      std::vector<Item> items;

      const long long accounts = CountAccounts(sampleDomainId);

      Item profileItem;
      profileItem.key = "profile";
      {
         String profile;
         __int64 applied = 0;
         String state;
         __int64 changed = 0;
         Profiles::ReadApplied(profile, applied, state, changed);

         if (profile.IsEmpty())
         {
            if (accounts < YoungAccountLimit)
               Add(profileItem, StateActionNeeded, "profile_none");
            else
               Add(profileItem, StateNotNeeded, "profile_in_service", { Number(accounts) });
         }
         else if (state == _T("reverted"))
            Add(profileItem, StateNotNeeded, "profile_reverted", { Utf8(profile), UtcDate(changed) });
         else if (state == _T("kept"))
            Add(profileItem, StateDone, "profile_kept", { Utf8(profile), UtcDate(applied) });
         else
            Add(profileItem, StateDone, "profile_applied", { Utf8(profile), UtcDate(applied) });
      }
      items.push_back(profileItem);

      Item domainItem;
      domainItem.key = "domain";
      if (total == 0)
         Add(domainItem, StateActionNeeded, "domain_none");
      else if (active.empty())
         Add(domainItem, StateActionNeeded, "domain_none_active", { Number(total) });
      else
         Add(domainItem, StateDone, "domain_count", { Number((long long) active.size()), Number(total) });

      // Last, and not_needed, so that it says why the count is what it is
      // without being able to change the item's state.
      if (!sampleDomainName.empty())
         Add(domainItem, StateNotNeeded, "sample_domain", { sampleDomainName });

      items.push_back(domainItem);

      Item accountItem;
      accountItem.key = "account";
      if (accounts == 0)
         Add(accountItem, StateActionNeeded, "account_none");
      else
         Add(accountItem, StateDone, "account_count", { Number(accounts) });
      items.push_back(accountItem);

      Item submission;
      submission.key = "submission";
      {
         // Read from the database rather than from the collections the listeners
         // were started from, so that a port or certificate saved a moment ago is
         // judged before the restart that makes it listen - the checklist is about
         // what is configured, and the restart is the next thing the page says.
         TCPIPPorts ports;
         ports.Refresh();
         SSLCertificates certificates;
         certificates.Refresh();

         bool anySubmission = false;
         for (std::shared_ptr<TCPIPPort> port : ports.GetVector())
         {
            if (!port || port->GetProtocol() != STSMTP || port->GetPortNumber() == 25)
               continue;

            anySubmission = true;
            const AnsiString portNumber = Number(port->GetPortNumber());
            if (port->GetConnectionSecurity() == CSNone)
            {
               Add(submission, StateActionNeeded, "submission_plain", { portNumber });
               continue;
            }

            std::shared_ptr<SSLCertificate> certificate = certificates.GetItemByDBID(port->GetSSLCertificateID());
            if (!certificate)
               Add(submission, StateActionNeeded, "submission_no_certificate", { portNumber });
            else
               Add(submission, StateDone, "submission_tls", { portNumber, Utf8(certificate->GetName()) });
         }

         if (!anySubmission)
            Add(submission, StateActionNeeded, "submission_none");
      }
      items.push_back(submission);

      Item spf;
      spf.key = "spf";
      Item dkim;
      dkim.key = "dkim";
      Item dmarc;
      dmarc.key = "dmarc";

      if (active.empty())
      {
         Add(spf, StateNotNeeded, "no_active_domain");
         Add(dkim, StateNotNeeded, "no_active_domain");
         Add(dmarc, StateNotNeeded, "no_active_domain");
      }

      for (size_t i = 0; i < active.size(); i++)
      {
         if (i >= (size_t) MaxDomainsLookedUp)
         {
            const AnsiString remaining = Number((long long) (active.size() - i));
            Add(spf, StateCannotTell, "domains_not_looked_up", { remaining });
            Add(dkim, StateCannotTell, "domains_not_looked_up", { remaining });
            Add(dmarc, StateCannotTell, "domains_not_looked_up", { remaining });
            break;
         }

         std::shared_ptr<Domain> domain = active[i];
         const AnsiString name = Utf8(domain->GetName());

         RecordFinding(spf, name, "SPF", name, lookUpDns, IsSpfRecord);
         RecordFinding(dmarc, name, "DMARC", "_dmarc." + name, lookUpDns, IsDmarcRecord);

         if (!domain->GetDKIMEnabled())
         {
            Add(dkim, StateActionNeeded, "dkim_off", { name });
            continue;
         }

         const AnsiString selector = AnsiString(domain->GetDKIMSelector()).Trim();
         if (selector.empty())
         {
            Add(dkim, StateActionNeeded, "dkim_no_selector", { name });
            continue;
         }

         const String keyFile = domain->GetDKIMPrivateKeyFile();
         if (keyFile.IsEmpty() || !FileUtilities::Exists(keyFile))
         {
            Add(dkim, StateActionNeeded, "dkim_key_missing", { name, Utf8(keyFile) });
            continue;
         }

         RecordFinding(dkim, name, "DKIM", selector + "._domainkey." + name, lookUpDns, IsDkimRecord);
      }

      items.push_back(spf);
      items.push_back(dkim);
      items.push_back(dmarc);

      Item backup;
      backup.key = "backup";
      {
         const String destination = Configuration::Instance()->GetBackupDestination().Trim();
         if (destination.IsEmpty())
            Add(backup, StateActionNeeded, "backup_none");
         else if (!FileUtilities::DirectoryExists(destination))
            Add(backup, StateActionNeeded, "backup_folder_missing", { Utf8(destination) });
         else
            Add(backup, StateDone, "backup_set", { Utf8(destination) });
      }
      items.push_back(backup);

      Item secondFactor;
      secondFactor.key = "second_factor";
      if (IniFileSettings::Instance()->IsAdministratorTotpEnrolled())
         Add(secondFactor, StateDone, "second_factor_on");
      else
         Add(secondFactor, StateActionNeeded, "second_factor_off");
      items.push_back(secondFactor);

      int done = 0;
      int actionNeeded = 0;
      int cannotTell = 0;
      int notNeeded = 0;
      for (const Item &item : items)
      {
         switch (StateOf(item))
         {
         case StateDone: done++; break;
         case StateActionNeeded: actionNeeded++; break;
         case StateNotNeeded: notNeeded++; break;
         default: cannotTell++; break;
         }
      }

      AnsiString json = "{\"generated\":" + Number((long long) time(nullptr));
      json += AnsiString(",\"dns_looked_up\":") + (lookUpDns ? "true" : "false");
      json += ",\"done\":" + Number(done);
      json += ",\"action_needed\":" + Number(actionNeeded);
      json += ",\"cannot_tell\":" + Number(cannotTell);
      json += ",\"not_needed\":" + Number(notNeeded);
      json += ",\"items\":[";
      for (size_t i = 0; i < items.size(); i++)
      {
         if (i > 0)
            json += ",";
         json += ItemJson(items[i]);
      }
      json += "]}";
      return json;
   }
}
