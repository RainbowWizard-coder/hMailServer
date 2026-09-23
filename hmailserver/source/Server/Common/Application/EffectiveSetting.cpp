// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "EffectiveSetting.h"

#include "Configuration.h"
#include "IniFileSettings.h"
#include "../BO/Account.h"
#include "../BO/Domain.h"
#include "../Cache/CacheContainer.h"
#include "../Persistence/PersistentSettingOverride.h"
#include "../Util/CountryBlocking.h"
#include "../Util/ImpersonationCheck.h"
#include "../Util/RateLimiter.h"
#include "../Util/SessionPolicies.h"
#include "../Util/Unicode.h"
#include "../../SMTP/SMTPConfiguration.h"

#include <climits>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // ------------------------------------------------------------------
      // Text forms. An integer is its decimal digits, a boolean 1 or 0, and
      // text is itself: the forms the overlay stores and COM answers.
      // ------------------------------------------------------------------

      bool ParseInteger(const String &text, long long &number)
      {
         String value = text;
         value.Trim();

         if (value.IsEmpty() || value.GetLength() > 12)
            return false;

         int start = 0;
         if (value[0] == '-' || value[0] == '+')
            start = 1;

         if (start == value.GetLength())
            return false;

         long long result = 0;
         for (int i = start; i < value.GetLength(); i++)
         {
            const wchar_t c = value[i];
            if (c < '0' || c > '9')
               return false;
            result = result * 10 + (c - '0');
         }

         number = value[0] == '-' ? -result : result;
         return true;
      }

      bool ParseBoolean(const String &text, bool &flag)
      {
         String value = text;
         value.Trim();
         value.ToLower();

         if (value == _T("1") || value == _T("true") || value == _T("on") || value == _T("yes"))
            flag = true;
         else if (value == _T("0") || value == _T("false") || value == _T("off") || value == _T("no"))
            flag = false;
         else
            return false;

         return true;
      }

      String Number(long long number)
      {
         return StringParser::IntToString((__int64) number);
      }

      String Flag(bool flag)
      {
         return flag ? _T("1") : _T("0");
      }

      // An integer within bounds, or the sentence that says what the name takes.
      bool Bounded(const String &value, long long minimum, long long maximum, const TCHAR *sentence, String &canonical, String &refusal)
      {
         long long number = 0;
         if (!ParseInteger(value, number) || number < minimum || number > maximum)
         {
            refusal = sentence;
            return false;
         }

         canonical = Number(number);
         return true;
      }

      bool Boolean(const String &value, const TCHAR *name, String &canonical, String &refusal)
      {
         bool flag = false;
         if (!ParseBoolean(value, flag))
         {
            refusal = String(name) + _T(" is true or false (1 or 0).");
            return false;
         }

         canonical = Flag(flag);
         return true;
      }

      // ------------------------------------------------------------------
      // The server levels. Each answers what the server alone would put in
      // force, in the text form - so a read site that resolves a name with no
      // domain and no account is given what it was given before.
      // ------------------------------------------------------------------

      Configuration *Config()
      {
         return Configuration::Instance();
      }

      String ServerMaxMessageSize() { return Number(Config()->GetSMTPConfiguration()->GetMaxMessageSize()); }
      String ServerFalse() { return Flag(false); }
      String ServerZero() { return Number(0); }
      String ServerEmpty() { return String(); }
      // Domain::Domain() makes a domain with its digest at seven in the morning.
      String ServerDigestHour() { return Number(7); }

      // The session seconds: the stored value where it is one the interfaces
      // accept, the built-in ceiling otherwise - SessionPolicies' own rule.
      String ServerSessionIdle()
      {
         const int stored = Config()->GetSessionIdleSeconds();
         return Number(stored > 0 && stored <= SessionPolicies::MaximumSeconds ? stored : SessionPolicies::DefaultIdleSeconds);
      }

      String ServerSessionLifetime()
      {
         const int stored = Config()->GetSessionLifetimeSeconds();
         return Number(stored > 0 && stored <= SessionPolicies::MaximumSeconds ? stored : SessionPolicies::DefaultLifetimeSeconds);
      }

      String ServerSessionOnePerDevice() { return Flag(Config()->GetSessionOnePerDevice()); }
      String ServerSessionBindToAddress() { return Flag(Config()->GetSessionBindToAddress()); }
      String ServerQuotaWarningPercent() { return Number(IniFileSettings::Instance()->GetQuotaWarningPercent()); }
      String ServerQuotaGracePercent() { return Number(Config()->GetQuotaGracePercent()); }
      String ServerQuotaSendStopPercent() { return Number(Config()->GetQuotaSendStopPercent()); }
      String ServerRecallDays() { return Number(Config()->GetRecallDays()); }

      // RecoverableStore::Days has always clamped the stored value to 0-365.
      String ServerRecoverDeletedDays()
      {
         const int days = Config()->GetRecoverDeletedDays();
         return Number(days < 0 ? 0 : (days > 365 ? 365 : days));
      }

      String ServerSendingMaxMessages() { return Number(RateLimiter::Instance()->GetServerLimits().max_messages); }
      String ServerSendingMaxRecipients() { return Number(RateLimiter::Instance()->GetServerLimits().max_recipients); }
      String ServerOpenPgpPortal() { return Flag(Config()->GetOpenPGPPortalEnabled()); }
      String ServerZeroAccessAllowed() { return Flag(Config()->GetZeroAccessAllowed()); }
      String ServerFetchAccountsMax() { return Number(Config()->GetReaderFetchAccountsMax()); }
      String ServerSenderAddressesMax() { return Number(Config()->GetReaderSenderAddressesMax()); }
      String ServerMaskedAddressesMax() { return Number(Config()->GetReaderMaskedAddressesMax()); }
      String ServerOfflineDaysMax() { return Number(Config()->GetReaderOfflineDaysMax()); }
      String ServerDirectoryUserEditable() { return Flag(Config()->GetDirectoryUserEditable()); }

      // ------------------------------------------------------------------
      // The dedicated columns. True with the value when the column holds one of
      // its own; false when it holds its "take the level above". A column that
      // was never optional is always true.
      // ------------------------------------------------------------------

      // 0 is "no limit of the domain's own" - the server's applies.
      bool DomainMaxMessageSize(const Domain &domain, String &value)
      {
         const int kb = domain.GetMaxMessageSize();
         if (kb <= 0)
            return false;
         value = Number(kb);
         return true;
      }

      bool DomainQuarantineDigest(const Domain &domain, String &value) { value = Flag(domain.GetQuarantineDigest()); return true; }
      bool DomainQuarantineDigestHour(const Domain &domain, String &value) { value = Number(domain.GetQuarantineDigestHour()); return true; }
      bool DomainExternalTagSubject(const Domain &domain, String &value) { value = Flag(domain.GetExternalTagSubject()); return true; }
      bool DomainExternalTagHeader(const Domain &domain, String &value) { value = Flag(domain.GetExternalTagHeader()); return true; }
      bool DomainImpersonationAction(const Domain &domain, String &value) { value = Number(domain.GetImpersonationAction()); return true; }
      bool DomainPlusAddressing(const Domain &domain, String &value) { value = Flag(domain.GetUsePlusAddressing()); return true; }
      bool DomainCountryPolicy(const Domain &domain, String &value) { value = domain.GetCountryPolicy(); return true; }

      // SessionPolicies::Resolve_'s rule: a number the interfaces would refuse
      // is treated as unset.
      bool SessionSeconds(int stored, String &value)
      {
         if (stored <= 0 || stored > SessionPolicies::MaximumSeconds)
            return false;
         value = Number(stored);
         return true;
      }

      bool SessionSwitch(int stored, String &value)
      {
         if (stored == SessionPolicies::SwitchOn)
            value = Flag(true);
         else if (stored == SessionPolicies::SwitchOff)
            value = Flag(false);
         else
            return false;
         return true;
      }

      bool DomainSessionIdle(const Domain &domain, String &value) { return SessionSeconds(domain.GetSessionIdleSeconds(), value); }
      bool DomainSessionLifetime(const Domain &domain, String &value) { return SessionSeconds(domain.GetSessionLifetimeSeconds(), value); }
      bool DomainSessionOnePerDevice(const Domain &domain, String &value) { return SessionSwitch(domain.GetSessionOnePerDevice(), value); }
      bool DomainSessionBindToAddress(const Domain &domain, String &value) { return SessionSwitch(domain.GetSessionBindToAddress(), value); }

      // -1 is "the server's value", QuotaPolicy's rule.
      bool QuotaShare(int stored, String &value)
      {
         if (stored < 0)
            return false;
         value = Number(stored);
         return true;
      }

      bool DomainQuotaWarningPercent(const Domain &domain, String &value) { return QuotaShare(domain.GetQuotaWarningPercent(), value); }
      bool DomainQuotaGracePercent(const Domain &domain, String &value) { return QuotaShare(domain.GetQuotaGracePercent(), value); }
      bool DomainQuotaSendStopPercent(const Domain &domain, String &value) { return QuotaShare(domain.GetQuotaSendStopPercent(), value); }

      // The retention: the domain's 0 is no policy of its own, and there is no
      // server policy - so both read as the same 0. The account's 0 follows the
      // domain, its -1 keeps for ever - a value of its own that deletes nothing,
      // which is 0 as an effective number of days. MailboxRetentionTask's
      // EffectiveRetentionDays_ was exactly this.
      bool DomainRetentionDays(const Domain &domain, String &value)
      {
         const int days = domain.GetMessageRetentionDays();
         if (days <= 0)
            return false;
         value = Number(days);
         return true;
      }

      bool AccountRetentionDays(const Account &account, String &value)
      {
         const int days = account.GetMessageRetentionDays();
         if (days == 0)
            return false;
         value = Number(days < 0 ? 0 : days);
         return true;
      }

      // The [SendingLimitsOverrides] line for the address: the server
      // administrator's own exception for that address, and so the address's
      // SERVER level - which the domain's and the account's own values (limits,
      // both) may tighten and not loosen. With no overlay row anywhere it is
      // what the address has always been held to, above the [SendingLimits]
      // value or below it.
      bool ServerFileSendingMessages(const Account &account, String &value)
      {
         AccountSendingLimits limits;
         if (!RateLimiter::Instance()->GetAddressLimits(account.GetAddress(), limits))
            return false;
         value = Number(limits.max_messages);
         return true;
      }

      bool ServerFileSendingRecipients(const Account &account, String &value)
      {
         AccountSendingLimits limits;
         if (!RateLimiter::Instance()->GetAddressLimits(account.GetAddress(), limits))
            return false;
         value = Number(limits.max_recipients);
         return true;
      }

      // ------------------------------------------------------------------
      // What the overlay takes, per name: the same ranges and sentences the
      // server's own row refuses with, less the value that means "take the
      // level above" in a column - which in the overlay is no row at all.
      // ------------------------------------------------------------------

      bool CheckMaxMessageSize(const String &v, String &c, String &r)
      {
         return Bounded(v, 1, 2000000000, _T("max_message_size_kb at a domain or an account is a number of KB from 1 to 2000000000; clear it to take the level above, whose 0 is no limit. The server's own value is a ceiling no level below exceeds."), c, r);
      }

      bool CheckDigest(const String &v, String &c, String &r) { return Boolean(v, _T("quarantine_digest"), c, r); }
      bool CheckDigestHour(const String &v, String &c, String &r) { return Bounded(v, 0, 23, _T("quarantine_digest_hour is an hour of the day, 0 to 23."), c, r); }
      bool CheckTagSubject(const String &v, String &c, String &r) { return Boolean(v, _T("external_tag_subject"), c, r); }
      bool CheckTagHeader(const String &v, String &c, String &r) { return Boolean(v, _T("external_tag_header"), c, r); }

      bool CheckImpersonation(const String &v, String &c, String &r)
      {
         int action = 0;
         if (!ImpersonationCheck::ReadAction(v, action))
         {
            r = _T("impersonation_action must be 0 (off), 1 (flag) or 2 (quarantine), or the words off, flag or quarantine.");
            return false;
         }
         c = Number(action);
         return true;
      }

      bool SessionSecondsCheck(const String &v, const TCHAR *name, String &c, String &r)
      {
         long long number = 0;
         if (!ParseInteger(v, number) || number < 1 || number > SessionPolicies::MaximumSeconds)
         {
            r = String(name) + _T(" at a domain or an account is a number of seconds from 1 to 2592000 (thirty days); clear it to take the level above.");
            return false;
         }
         c = Number(number);
         return true;
      }

      bool CheckSessionIdle(const String &v, String &c, String &r) { return SessionSecondsCheck(v, _T("session_idle_seconds"), c, r); }
      bool CheckSessionLifetime(const String &v, String &c, String &r) { return SessionSecondsCheck(v, _T("session_lifetime_seconds"), c, r); }
      bool CheckOnePerDevice(const String &v, String &c, String &r) { return Boolean(v, _T("session_one_per_device"), c, r); }
      bool CheckBindToAddress(const String &v, String &c, String &r) { return Boolean(v, _T("session_bind_to_address"), c, r); }
      bool CheckQuotaWarning(const String &v, String &c, String &r) { return Bounded(v, 0, 100, _T("quota_warning_percent is a percentage from 0 to 100; 0 warns nobody. Clear it to take the level above."), c, r); }
      bool CheckQuotaGrace(const String &v, String &c, String &r) { return Bounded(v, 0, 1000, _T("quota_grace_percent is a percentage from 0 to 1000. Clear it to take the level above."), c, r); }
      bool CheckQuotaSendStop(const String &v, String &c, String &r) { return Bounded(v, 0, 1000, _T("quota_send_stop_percent is a percentage from 0 to 1000; 0 is never. Clear it to take the level above."), c, r); }
      bool CheckPlusAddressing(const String &v, String &c, String &r) { return Boolean(v, _T("plus_addressing_enabled"), c, r); }

      bool CheckCountryPolicy(const String &v, String &c, String &r)
      {
         String canonical;
         String error;
         if (!CountryBlocking::Canonicalise(v, canonical, error))
         {
            r = error;
            return false;
         }
         c = canonical;
         return true;
      }

      bool CheckRetention(const String &, String &, String &r)
      {
         r = _T("message_retention_days is held in the domain's and the account's own fields (message_retention_days on each), not in the overlay.");
         return false;
      }

      bool CheckRecallDays(const String &v, String &c, String &r) { return Bounded(v, 1, 365, _T("recall_days must be between 1 and 365."), c, r); }
      bool CheckRecoverDays(const String &v, String &c, String &r) { return Bounded(v, 0, 365, _T("recover_deleted_days is between 0 and 365."), c, r); }
      bool CheckSendingMessages(const String &v, String &c, String &r) { return Bounded(v, 0, 1000000000, _T("sending_max_messages is a number of messages a period, 0 for no limit, at most 1000000000."), c, r); }
      bool CheckSendingRecipients(const String &v, String &c, String &r) { return Bounded(v, 0, 1000000000, _T("sending_max_recipients is a number of recipients a period, 0 for no limit, at most 1000000000."), c, r); }
      bool CheckOpenPgpPortal(const String &v, String &c, String &r) { return Boolean(v, _T("openpgp_portal_enabled"), c, r); }
      bool CheckZeroAccess(const String &v, String &c, String &r) { return Boolean(v, _T("zero_access_allowed"), c, r); }
      bool CheckFetchMax(const String &v, String &c, String &r) { return Bounded(v, 0, 20, _T("reader_fetch_accounts_max is between 0 and 20."), c, r); }
      bool CheckSenderMax(const String &v, String &c, String &r) { return Bounded(v, 0, 20, _T("reader_sender_addresses_max is between 0 and 20."), c, r); }
      bool CheckMaskedMax(const String &v, String &c, String &r) { return Bounded(v, 0, 200, _T("reader_masked_addresses_max is between 0 and 200."), c, r); }
      bool CheckOfflineMax(const String &v, String &c, String &r) { return Bounded(v, 0, 365, _T("reader_offline_days_max is between 0 and 365."), c, r); }
      bool CheckDirectoryEditable(const String &v, String &c, String &r) { return Boolean(v, _T("directory_user_editable"), c, r); }

      typedef EffectiveSetting E;

      // THE TABLE. One row per name that may be set below the server. The
      // columns: name, type, levels, where the server level comes from and its
      // key, the kind of setting it is (a limit, a gate or a preference - the
      // header says what each means, docs/SettingLevels.md why each name is the
      // one it is) and, for a limit, which way it tightens, the server's answer
      // and its answer for one address (or null), the domain's column (or null:
      // the overlay) and whether the server's value has always capped it, the
      // account's column (or null), whether the overlay holds the domain level
      // and the account level, and what the overlay takes.
      // build/check-setting-levels.py reads this table: keep one row to a line.
      const E::Definition Table[] =
      {
         { "max_message_size_kb", E::TypeInteger, E::LevelsAccount, E::ServerProperty, "maxmessagesize", E::ClassLimit, E::TightLowerZeroNone, &ServerMaxMessageSize, nullptr, &DomainMaxMessageSize, true, nullptr, false, true, &CheckMaxMessageSize },
         { "quarantine_digest", E::TypeBoolean, E::LevelsAccount, E::ServerDefault, "", E::ClassPreference, E::TightNone, &ServerFalse, nullptr, &DomainQuarantineDigest, false, nullptr, false, true, &CheckDigest },
         { "quarantine_digest_hour", E::TypeInteger, E::LevelsAccount, E::ServerDefault, "", E::ClassPreference, E::TightNone, &ServerDigestHour, nullptr, &DomainQuarantineDigestHour, false, nullptr, false, true, &CheckDigestHour },
         { "external_tag_subject", E::TypeBoolean, E::LevelsAccount, E::ServerDefault, "", E::ClassPreference, E::TightNone, &ServerFalse, nullptr, &DomainExternalTagSubject, false, nullptr, false, true, &CheckTagSubject },
         { "external_tag_header", E::TypeBoolean, E::LevelsAccount, E::ServerDefault, "", E::ClassPreference, E::TightNone, &ServerFalse, nullptr, &DomainExternalTagHeader, false, nullptr, false, true, &CheckTagHeader },
         { "impersonation_action", E::TypeInteger, E::LevelsAccount, E::ServerDefault, "", E::ClassPreference, E::TightNone, &ServerZero, nullptr, &DomainImpersonationAction, false, nullptr, false, true, &CheckImpersonation },
         { "session_idle_seconds", E::TypeInteger, E::LevelsAccount, E::ServerProperty, "SessionIdleSeconds", E::ClassLimit, E::TightLower, &ServerSessionIdle, nullptr, &DomainSessionIdle, false, nullptr, false, true, &CheckSessionIdle },
         { "session_lifetime_seconds", E::TypeInteger, E::LevelsAccount, E::ServerProperty, "SessionLifetimeSeconds", E::ClassLimit, E::TightLower, &ServerSessionLifetime, nullptr, &DomainSessionLifetime, false, nullptr, false, true, &CheckSessionLifetime },
         { "session_one_per_device", E::TypeBoolean, E::LevelsAccount, E::ServerProperty, "SessionOnePerDevice", E::ClassLimit, E::TightOn, &ServerSessionOnePerDevice, nullptr, &DomainSessionOnePerDevice, false, nullptr, false, true, &CheckOnePerDevice },
         { "session_bind_to_address", E::TypeBoolean, E::LevelsAccount, E::ServerProperty, "SessionBindToAddress", E::ClassLimit, E::TightOn, &ServerSessionBindToAddress, nullptr, &DomainSessionBindToAddress, false, nullptr, false, true, &CheckBindToAddress },
         { "quota_warning_percent", E::TypeInteger, E::LevelsAccount, E::ServerIniKey, "QuotaWarningPercent", E::ClassPreference, E::TightNone, &ServerQuotaWarningPercent, nullptr, &DomainQuotaWarningPercent, false, nullptr, false, true, &CheckQuotaWarning },
         { "quota_grace_percent", E::TypeInteger, E::LevelsAccount, E::ServerProperty, "QuotaGracePercent", E::ClassLimit, E::TightLower, &ServerQuotaGracePercent, nullptr, &DomainQuotaGracePercent, false, nullptr, false, true, &CheckQuotaGrace },
         { "quota_send_stop_percent", E::TypeInteger, E::LevelsAccount, E::ServerProperty, "QuotaSendStopPercent", E::ClassLimit, E::TightLowerZeroNone, &ServerQuotaSendStopPercent, nullptr, &DomainQuotaSendStopPercent, false, nullptr, false, true, &CheckQuotaSendStop },
         { "plus_addressing_enabled", E::TypeBoolean, E::LevelsAccount, E::ServerDefault, "", E::ClassGate, E::TightNone, &ServerFalse, nullptr, &DomainPlusAddressing, false, nullptr, false, true, &CheckPlusAddressing },
         { "country_policy", E::TypeText, E::LevelsAccount, E::ServerDefault, "", E::ClassPreference, E::TightNone, &ServerEmpty, nullptr, &DomainCountryPolicy, false, nullptr, false, true, &CheckCountryPolicy },
         { "message_retention_days", E::TypeInteger, E::LevelsAccount, E::ServerDefault, "", E::ClassPreference, E::TightNone, &ServerZero, nullptr, &DomainRetentionDays, false, &AccountRetentionDays, false, false, &CheckRetention },
         { "recall_days", E::TypeInteger, E::LevelsAccount, E::ServerProperty, "RecallDays", E::ClassLimit, E::TightLower, &ServerRecallDays, nullptr, nullptr, false, nullptr, true, true, &CheckRecallDays },
         { "recover_deleted_days", E::TypeInteger, E::LevelsAccount, E::ServerProperty, "RecoverDeletedDays", E::ClassLimit, E::TightLower, &ServerRecoverDeletedDays, nullptr, nullptr, false, nullptr, true, true, &CheckRecoverDays },
         { "sending_max_messages", E::TypeInteger, E::LevelsAccount, E::ServerIniSection, "MaxMessagesPerAccountPerPeriod", E::ClassLimit, E::TightLowerZeroNone, &ServerSendingMaxMessages, &ServerFileSendingMessages, nullptr, false, nullptr, true, true, &CheckSendingMessages },
         { "sending_max_recipients", E::TypeInteger, E::LevelsAccount, E::ServerIniSection, "MaxRecipientsPerAccountPerPeriod", E::ClassLimit, E::TightLowerZeroNone, &ServerSendingMaxRecipients, &ServerFileSendingRecipients, nullptr, false, nullptr, true, true, &CheckSendingRecipients },
         { "openpgp_portal_enabled", E::TypeBoolean, E::LevelsAccount, E::ServerProperty, "OpenPGPPortalEnabled", E::ClassGate, E::TightNone, &ServerOpenPgpPortal, nullptr, nullptr, false, nullptr, true, true, &CheckOpenPgpPortal },
         { "zero_access_allowed", E::TypeBoolean, E::LevelsAccount, E::ServerProperty, "ZeroAccessAllowed", E::ClassGate, E::TightNone, &ServerZeroAccessAllowed, nullptr, nullptr, false, nullptr, true, true, &CheckZeroAccess },
         { "reader_fetch_accounts_max", E::TypeInteger, E::LevelsAccount, E::ServerProperty, "ReaderFetchAccountsMax", E::ClassLimit, E::TightLower, &ServerFetchAccountsMax, nullptr, nullptr, false, nullptr, true, true, &CheckFetchMax },
         { "reader_sender_addresses_max", E::TypeInteger, E::LevelsAccount, E::ServerProperty, "ReaderSenderAddressesMax", E::ClassLimit, E::TightLower, &ServerSenderAddressesMax, nullptr, nullptr, false, nullptr, true, true, &CheckSenderMax },
         { "reader_masked_addresses_max", E::TypeInteger, E::LevelsAccount, E::ServerProperty, "ReaderMaskedAddressesMax", E::ClassLimit, E::TightLower, &ServerMaskedAddressesMax, nullptr, nullptr, false, nullptr, true, true, &CheckMaskedMax },
         { "reader_offline_days_max", E::TypeInteger, E::LevelsAccount, E::ServerProperty, "ReaderOfflineDaysMax", E::ClassLimit, E::TightLower, &ServerOfflineDaysMax, nullptr, nullptr, false, nullptr, true, true, &CheckOfflineMax },
         { "directory_user_editable", E::TypeBoolean, E::LevelsAccount, E::ServerProperty, "DirectoryUserEditable", E::ClassGate, E::TightNone, &ServerDirectoryUserEditable, nullptr, nullptr, false, nullptr, true, true, &CheckDirectoryEditable },
      };

      const size_t TableCount = sizeof(Table) / sizeof(Table[0]);

      // A stored overlay value, judged again as it is read. A row the interfaces
      // would refuse was put there by hand - or by a build whose rule was looser -
      // and is treated as unset rather than honoured, said once per name.
      bool OverlayValue(const E::Definition &definition, int scope, __int64 ownerId, String &value)
      {
         if (ownerId <= 0)
            return false;

         std::shared_ptr<const std::map<String, String> > rows = CacheContainer::Instance()->GetSettingOverrides(scope, ownerId);
         if (!rows)
            return false;

         auto found = rows->find(String(definition.name));
         if (found == rows->end())
            return false;

         String canonical;
         String refusal;
         if (!definition.canonicalise(found->second, canonical, refusal))
         {
            // Said once per name - and again after the caches have been
            // emptied, which is what an administrator does after putting a row
            // right by hand, and what a test does to see the report.
            static std::mutex reported_mutex;
            static std::set<String> reported;
            static unsigned __int64 reported_clears = 0;

            bool report = false;
            {
               std::lock_guard<std::mutex> guard(reported_mutex);
               const unsigned __int64 clears = CacheContainer::Instance()->GetSettingOverridesClears();
               if (clears != reported_clears)
               {
                  reported.clear();
                  reported_clears = clears;
               }
               report = reported.insert(String(definition.name)).second;
            }

            if (report)
            {
               ErrorManager::Instance()->ReportError(ErrorManager::Low, 7311, "EffectiveSetting::Resolve",
                  Formatter::Format("hm_settingoverrides holds a value for {0} that the interfaces would refuse ({1}); that level is treated as inheriting. Said once per setting, and again after the caches are emptied.",
                     String(definition.name), refusal));
            }

            return false;
         }

         value = canonical;
         return true;
      }

      // A limit's value, as a number that is larger the looser it is: a switch
      // on is 0 and off 1, and a "0 is no limit" value of 0 is the largest there
      // is. False for a value that is not the name's type.
      bool Looseness(const E::Definition &definition, const String &value, long long &looseness)
      {
         if (definition.tight == E::TightOn)
         {
            bool flag = false;
            if (!ParseBoolean(value, flag))
               return false;
            looseness = flag ? 0 : 1;
            return true;
         }

         long long number = 0;
         if (!ParseInteger(value, number))
            return false;

         if (definition.tight == E::TightLowerZeroNone && number <= 0)
            number = LLONG_MAX;

         looseness = number;
         return true;
      }

      bool IsLooser(const E::Definition &definition, const String &value, const String &above)
      {
         if (!definition.ceiling())
            return false;

         long long level = 0;
         long long ceiling = 0;
         if (!Looseness(definition, value, level) || !Looseness(definition, above, ceiling))
            return false;

         return level > ceiling;
      }

      // The level below, held to the level above when the name is a limit: the
      // tighter of the two, and where it came from - so an account's value that
      // its domain has since tightened past answers the domain's, from the
      // domain. What SMTPConnection::GetMaxMessageSize_ always did between the
      // domain's and the server's message size, for every limit and level.
      E::Answer Held(const E::Definition &definition, const E::Answer &below, const E::Answer &above)
      {
         return IsLooser(definition, below.value, above.value) ? above : below;
      }

      // The server's answer for one account: its per-address value where the
      // server holds one (the sending limits' file), the server's otherwise.
      E::Answer ServerAnswer(const E::Definition &definition, std::shared_ptr<const Account> account)
      {
         E::Answer answer;
         answer.source = E::SourceServer;

         String value;
         if (account && definition.server_for_account && definition.server_for_account(*account, value))
            answer.value = value;
         else
            answer.value = definition.server();

         return answer;
      }

      // The domain's and the server's levels, combined: what the domain's
      // accounts that hold nothing of their own resolve to, and the level above
      // an account's own value. The domain's own field of a limit that was
      // always free of the server's value stays free of it.
      E::Answer DomainAndServer(const E::Definition &definition, std::shared_ptr<const Domain> domain, std::shared_ptr<const Account> account)
      {
         E::Answer answer = ServerAnswer(definition, account);

         String value;
         if (E::DomainValue(definition, domain, value))
         {
            E::Answer own;
            own.value = value;
            own.source = E::SourceDomain;

            const bool capped = definition.ceiling() && (definition.domain_overlay || definition.domain_column_capped);
            answer = capped ? Held(definition, own, answer) : own;
         }

         return answer;
      }

      // How a value is said in a sentence.
      String Shown(const E::Definition &definition, const String &value)
      {
         if (definition.type == E::TypeBoolean)
         {
            bool flag = false;
            ParseBoolean(value, flag);
            return flag ? _T("on") : _T("off");
         }

         long long number = 0;
         if (definition.tight == E::TightLowerZeroNone && ParseInteger(value, number) && number <= 0)
            return _T("0 (no limit)");

         return value;
      }

      AnsiString JsonEscapedUtf8(const String &value)
      {
         AnsiString utf8;
         Unicode::WideToMultiByte(value, utf8);

         AnsiString result;
         for (int i = 0; i < utf8.GetLength(); i++)
         {
            const char c = utf8[i];
            switch (c)
            {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\r': result += "\\r"; break;
            case '\n': result += "\\n"; break;
            case '\t': result += "\\t"; break;
            default:
               if (static_cast<unsigned char>(c) < 0x20)
               {
                  char escaped[8];
                  snprintf(escaped, sizeof(escaped), "\\u%04x", (unsigned int) static_cast<unsigned char>(c));
                  result += escaped;
               }
               else
                  result += c;
            }
         }
         return result;
      }
   }

   const EffectiveSetting::Definition *
   EffectiveSetting::Definitions(size_t &count)
   {
      count = TableCount;
      return Table;
   }

   const EffectiveSetting::Definition *
   EffectiveSetting::Find(const String &name)
   {
      String wanted = name;
      wanted.Trim();

      for (size_t i = 0; i < TableCount; i++)
      {
         if (wanted.CompareNoCase(String(Table[i].name)) == 0)
            return &Table[i];
      }

      return nullptr;
   }

   bool
   EffectiveSetting::OverlayHolds(const Definition &definition, int scope)
   {
      if (scope == PersistentSettingOverride::ScopeDomain)
         return definition.domain_overlay && definition.levels >= LevelsDomain;
      if (scope == PersistentSettingOverride::ScopeAccount)
         return definition.account_overlay && definition.levels >= LevelsAccount;
      return false;
   }

   bool
   EffectiveSetting::DomainValue(const Definition &definition, std::shared_ptr<const Domain> domain, String &value)
   {
      if (!domain || definition.levels < LevelsDomain)
         return false;

      if (definition.domain_overlay && OverlayValue(definition, PersistentSettingOverride::ScopeDomain, domain->GetID(), value))
         return true;

      if (definition.domain_column && definition.domain_column(*domain, value))
         return true;

      return false;
   }

   bool
   EffectiveSetting::AccountValue(const Definition &definition, std::shared_ptr<const Account> account, String &value)
   {
      if (!account || definition.levels < LevelsAccount)
         return false;

      if (definition.account_overlay && OverlayValue(definition, PersistentSettingOverride::ScopeAccount, account->GetID(), value))
         return true;

      if (definition.account_column && definition.account_column(*account, value))
         return true;

      return false;
   }

   EffectiveSetting::Answer
   EffectiveSetting::Resolve(const String &name, std::shared_ptr<const Domain> domain, std::shared_ptr<const Account> account)
   {
      Answer answer;

      const Definition *definition = Find(name);
      if (!definition)
      {
         // A name that is not in the table is a programming error at the read
         // site - check-setting-levels.py pins every one to a name that is - and
         // the answer is the empty server value rather than a guess.
         HM_ASSERT(0);
         return answer;
      }

      if (account && !domain)
         domain = CacheContainer::Instance()->GetDomain(account->GetDomainID());

      // The server's, then the domain's over it, then the account's over that:
      // a preference and a gate take the deepest level that holds a value, and
      // a limit takes it only as far as the level above allows.
      answer = DomainAndServer(*definition, domain, account);

      String value;
      if (AccountValue(*definition, account, value))
      {
         Answer own;
         own.value = value;
         own.source = SourceAccount;
         answer = definition->ceiling() ? Held(*definition, own, answer) : own;
      }

      return answer;
   }

   EffectiveSetting::Answer
   EffectiveSetting::Resolve(const String &name, __int64 domainId, __int64 accountId)
   {
      std::shared_ptr<const Account> account;
      if (accountId > 0)
         account = CacheContainer::Instance()->GetAccount(accountId);

      std::shared_ptr<const Domain> domain;
      if (domainId > 0)
         domain = CacheContainer::Instance()->GetDomain(domainId);

      return Resolve(name, domain, account);
   }

   int
   EffectiveSetting::ResolveInt(const String &name, std::shared_ptr<const Domain> domain, std::shared_ptr<const Account> account)
   {
      long long number = 0;
      if (!ParseInteger(Resolve(name, domain, account).value, number))
         return 0;
      if (number > INT_MAX)
         return INT_MAX;
      if (number < INT_MIN)
         return INT_MIN;
      return (int) number;
   }

   bool
   EffectiveSetting::ResolveBool(const String &name, std::shared_ptr<const Domain> domain, std::shared_ptr<const Account> account)
   {
      bool flag = false;
      ParseBoolean(Resolve(name, domain, account).value, flag);
      return flag;
   }

   String
   EffectiveSetting::ResolveText(const String &name, std::shared_ptr<const Domain> domain, std::shared_ptr<const Account> account)
   {
      return Resolve(name, domain, account).value;
   }

   int
   EffectiveSetting::ResolveIntHeldTo(const String &name, std::shared_ptr<const Domain> domain, std::shared_ptr<const Account> account,
                                      std::shared_ptr<const Account> signedIn)
   {
      Answer answer = Resolve(name, domain, account);

      const Definition *definition = Find(name);
      String own;
      if (definition && signedIn && definition->ceiling() && AccountValue(*definition, signedIn, own) && IsLooser(*definition, answer.value, own))
         answer.value = own;

      long long number = 0;
      if (!ParseInteger(answer.value, number))
         return 0;
      if (number > INT_MAX)
         return INT_MAX;
      if (number < INT_MIN)
         return INT_MIN;
      return (int) number;
   }

   String
   EffectiveSetting::NotThreeLevelSentence(const String &name)
   {
      String shown = name.Left(64);
      return shown + _T(" cannot be set for a domain or an account: it is not one of the settings that have levels, which GET /api/v1/settings/levels lists. A server setting of that name is set for the whole server only.");
   }

   String
   EffectiveSetting::ColumnSentence(const Definition &definition, int scope)
   {
      return String(definition.name) + (scope == PersistentSettingOverride::ScopeDomain
         ? _T(" is held for a domain in the domain's own field of that name, which is what sets it there, and 'take the level above' is what that field holds for it.")
         : _T(" is held for an account in the account's own field of that name, which is what sets it there, and 'take the level above' is what that field holds for it."));
   }

   EffectiveSetting::Answer
   EffectiveSetting::Above(const Definition &definition, int scope, __int64 ownerId)
   {
      if (scope == PersistentSettingOverride::ScopeAccount)
      {
         std::shared_ptr<const Account> account = CacheContainer::Instance()->GetAccount(ownerId);
         if (account)
            return DomainAndServer(definition, CacheContainer::Instance()->GetDomain(account->GetDomainID()), account);
      }

      return ServerAnswer(definition, std::shared_ptr<const Account>());
   }

   bool
   EffectiveSetting::Looser(const Definition &definition, const String &value, const String &above)
   {
      return IsLooser(definition, value, above);
   }

   bool
   EffectiveSetting::HoldsToCeiling(const Definition &definition, int scope, __int64 ownerId, const String &canonical, String &refusal)
   {
      if (!definition.ceiling())
         return true;

      const Answer above = Above(definition, scope, ownerId);
      if (!IsLooser(definition, canonical, above.value))
         return true;

      const bool account = scope == PersistentSettingOverride::ScopeAccount;
      refusal = String(definition.name) + _T(" is a limit: ") + (account ? _T("an account") : _T("a domain")) +
         _T(" may tighten the level above it and not loosen it. The level above holds ") + Shown(definition, above.value) +
         _T(" (from the ") + String(SourceWord(above.source)) + _T("), and ") + Shown(definition, canonical) + _T(" would loosen it.");
      return false;
   }

   bool
   EffectiveSetting::SetOverride(const String &name, int scope, __int64 ownerId, const String &value, String &refusal, bool holdToCeiling)
   {
      const Definition *definition = Find(name);
      if (!definition)
      {
         refusal = NotThreeLevelSentence(name);
         return false;
      }

      if (!OverlayHolds(*definition, scope))
      {
         refusal = ColumnSentence(*definition, scope);
         return false;
      }

      if (ownerId <= 0)
      {
         refusal = _T("The domain or the account has not been saved yet; save it first, then set its own values.");
         return false;
      }

      String canonical;
      if (!definition->canonicalise(value, canonical, refusal))
         return false;

      // A limit may be tightened below the level above it and not loosened: the
      // write is refused, saying what the level above holds and where that came
      // from, rather than stored and quietly capped.
      if (holdToCeiling && !HoldsToCeiling(*definition, scope, ownerId, canonical, refusal))
         return false;

      if (canonical.GetLength() > 4000)
      {
         refusal = String(definition->name) + _T(" is longer than the 4000 characters a level's value may hold.");
         return false;
      }

      if (!PersistentSettingOverride::Write(scope, ownerId, String(definition->name), canonical))
      {
         refusal = _T("The value could not be stored; the database did not take it.");
         return false;
      }

      return true;
   }

   bool
   EffectiveSetting::ClearOverride(const String &name, int scope, __int64 ownerId, String &refusal)
   {
      const Definition *definition = Find(name);
      if (!definition)
      {
         refusal = NotThreeLevelSentence(name);
         return false;
      }

      if (!OverlayHolds(*definition, scope))
      {
         refusal = ColumnSentence(*definition, scope);
         return false;
      }

      // The same refusal a write gives: an owner that has not been saved has no
      // values to clear, and saying "done" would read as though it had.
      if (ownerId <= 0)
      {
         refusal = _T("The domain or the account has not been saved yet; save it first, then set its own values.");
         return false;
      }

      if (!PersistentSettingOverride::Remove(scope, ownerId, String(definition->name)))
      {
         refusal = _T("The value could not be removed; the database did not answer.");
         return false;
      }

      return true;
   }

   const char *
   EffectiveSetting::SourceWord(Source source)
   {
      switch (source)
      {
      case SourceDomain: return "domain";
      case SourceAccount: return "account";
      default: return "server";
      }
   }

   const char *
   EffectiveSetting::LevelsWord(Levels levels)
   {
      switch (levels)
      {
      case LevelsDomain: return "domain";
      case LevelsAccount: return "account";
      default: return "server";
      }
   }

   const char *
   EffectiveSetting::ClassWord(Class klass)
   {
      switch (klass)
      {
      case ClassLimit: return "limit";
      case ClassGate: return "gate";
      default: return "preference";
      }
   }

   const char *
   EffectiveSetting::TightWord(Tight tight)
   {
      switch (tight)
      {
      case TightLower: return "lower";
      case TightLowerZeroNone: return "lower-zero-is-none";
      case TightOn: return "on";
      default: return "none";
      }
   }

   const char *
   EffectiveSetting::TypeWord(Type type)
   {
      switch (type)
      {
      case TypeBoolean: return "boolean";
      case TypeText: return "string";
      default: return "integer";
      }
   }

   const char *
   EffectiveSetting::ServerKindWord(ServerKind kind)
   {
      switch (kind)
      {
      case ServerProperty: return "property";
      case ServerIniKey: return "ini";
      case ServerIniSection: return "ini-section";
      default: return "default";
      }
   }

   AnsiString
   EffectiveSetting::JsonText(const Definition &definition, const String &value)
   {
      switch (definition.type)
      {
      case TypeBoolean:
         {
            bool flag = false;
            ParseBoolean(value, flag);
            return flag ? "true" : "false";
         }
      case TypeInteger:
         {
            long long number = 0;
            if (!ParseInteger(value, number))
               return "0";
            char text[32];
            snprintf(text, sizeof(text), "%lld", number);
            return text;
         }
      default:
         return "\"" + JsonEscapedUtf8(value) + "\"";
      }
   }

   AnsiString
   EffectiveSetting::TableJson()
   {
      AnsiString json = "{\"settings\":[";

      for (size_t i = 0; i < TableCount; i++)
      {
         const Definition &definition = Table[i];

         if (i > 0)
            json += ",";

         const char *domainStorage = definition.domain_overlay ? "overlay" : (definition.domain_column ? "column" : "none");
         const char *accountStorage = definition.account_overlay ? "overlay" : (definition.account_column ? "column" : "none");

         json += "{\"name\":\"";
         json += definition.name;
         json += "\",\"type\":\"";
         json += TypeWord(definition.type);
         json += "\",\"levels\":\"";
         json += LevelsWord(definition.levels);
         json += "\",\"server\":\"";
         json += ServerKindWord(definition.server_kind);
         json += "\",\"server_key\":\"";
         json += definition.server_key;
         json += "\",\"domain_storage\":\"";
         json += domainStorage;
         json += "\",\"account_storage\":\"";
         json += accountStorage;
         json += "\",\"class\":\"";
         json += ClassWord(definition.klass);
         json += "\",\"tighter\":\"";
         json += TightWord(definition.tight);
         json += "\",\"combine\":\"";
         json += definition.ceiling() ? "ceiling" : "override";
         json += "\",\"server_administrators_only\":";
         json += definition.server_administrators_only() ? "true" : "false";
         json += ",\"server_per_address\":";
         json += definition.server_for_account ? "true" : "false";
         json += ",\"domain_column_capped\":";
         json += definition.domain_column && definition.ceiling() && definition.domain_column_capped ? "true" : "false";
         json += ",\"server_value\":";
         json += JsonText(definition, definition.server());
         json += "}";
      }

      json += "]}";
      return json;
   }

   bool
   EffectiveSetting::XMLStore(XNode *parentNode, int scope, __int64 ownerId)
   {
      std::map<String, String> rows;
      if (!PersistentSettingOverride::ReadOwner(scope, ownerId, rows))
         return false;

      if (rows.empty())
         return true;

      XNode *collection = parentNode->AppendChild(_T("SettingOverrides"));

      for (const auto &row : rows)
      {
         XNode *node = collection->AppendChild(_T("SettingOverride"));
         node->AppendAttr(_T("Name"), row.first);
         node->AppendAttr(_T("Value"), row.second);
      }

      return true;
   }

   bool
   EffectiveSetting::XMLLoad(XNode *parentNode, int scope, __int64 ownerId)
   {
      // A restore deletes every domain and account first, and their rows go with
      // them, so there is nothing of this owner to clear. What is put back goes
      // through the same rule a write does: a name this build does not know, a
      // level the overlay does not hold for it or a value it refuses is left out
      // and said, and the restore carries on - one bad row is not a reason to
      // lose a domain.
      XNode *collection = parentNode->GetChild(_T("SettingOverrides"));
      if (!collection)
         return true;

      for (int i = 0; i < collection->GetChildCount(); i++)
      {
         XNode *node = collection->GetChild(i);
         const String name = node->GetAttrValue(_T("Name"));
         const String value = node->GetAttrValue(_T("Value"));

         // Not held to the ceiling here: the level above may not be restored yet,
         // or the server's value may have moved since the backup. The ceiling
         // caps the value when it is read, as it caps any value the level above
         // has since tightened past.
         String refusal;
         if (!SetOverride(name, scope, ownerId, value, refusal, false))
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7312, "EffectiveSetting::XMLLoad",
               Formatter::Format("The backup's value for {0} at {1} {2} was not restored: {3}",
                  name.Left(64), String(scope == PersistentSettingOverride::ScopeDomain ? _T("domain") : _T("account")), ownerId, refusal));
         }
      }

      return true;
   }

   int
   EffectiveSetting::LargestAnywhere(const String &name)
   {
      const Definition *definition = Find(name);
      if (!definition)
         return 0;

      long long largest = 0;
      ParseInteger(definition->server(), largest);

      // A switch the server already has on is on somewhere, whatever the rows
      // say: nothing to read.
      if (definition->type == TypeBoolean && largest >= 1)
         return 1;

      std::shared_ptr<const std::map<std::pair<int, __int64>, String> > rows =
         CacheContainer::Instance()->GetSettingOverridesNamed(String(definition->name));
      if (!rows)
         return (int) largest;

      for (const auto &row : *rows)
      {
         String canonical;
         String refusal;
         long long number = 0;
         if (definition->canonicalise(row.second, canonical, refusal) && ParseInteger(canonical, number) && number > largest)
            largest = number;
      }

      return (int) largest;
   }

   int
   EffectiveSetting::SmallestAnywhere(const String &name)
   {
      const Definition *definition = Find(name);
      if (!definition)
         return 0;

      long long smallest = 0;
      ParseInteger(definition->server(), smallest);

      std::shared_ptr<const std::map<std::pair<int, __int64>, String> > rows =
         CacheContainer::Instance()->GetSettingOverridesNamed(String(definition->name));
      if (!rows)
         return (int) smallest;

      for (const auto &row : *rows)
      {
         String canonical;
         String refusal;
         long long number = 0;
         if (definition->canonicalise(row.second, canonical, refusal) && ParseInteger(canonical, number) && number < smallest)
            smallest = number;
      }

      return (int) smallest;
   }
}
