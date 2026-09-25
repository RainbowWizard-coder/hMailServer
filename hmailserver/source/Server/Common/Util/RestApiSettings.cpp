// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The REST API's settings routes: PUT /api/v1/settings, the anti-spam, anti-virus, logging and directories groups, the INI keys and the logon-failure list. See RestApiServer.h.

// The REST API's settings routes: PUT /api/v1/settings, the anti-spam, logging and directories groups, the INI keys and the logon-failure list. See RestApiServer.h.
//
// The groups of settings, each a flat JSON object of snake_case keys, each
// read with GET and changed with PUT:
//
//    /api/v1/settings            the server-wide group InterfaceSettings holds
//    /api/v1/settings/antispam   InterfaceAntiSpam's scalars
//    /api/v1/settings/antivirus  InterfaceAntiVirus's scalars
//    /api/v1/settings/logging    InterfaceLogging's scalars
//    /api/v1/settings/directories  InterfaceDirectories, read-only: where the server runs
//
// and, after the tables, three routes that are not a group: the [Settings]
// section of hMailServer.ini one key at a time (/api/v1/settings/ini, what
// GetIniSetting, SetIniSetting, DeleteIniSetting and IniSettingNames do over
// COM, with the same refusals), and the logon-failure list's clear
// (/api/v1/settings/logon-failures/clear, ClearLogonFailureList).
//
// Everything about a key is one row of one table below: its name, its type,
// whether it can be read and written, whether the running server picks the
// change up at once or at its next start, the words an enumerated value takes,
// the sentence the OpenAPI document shows for it, and the getter, setter and
// pre-apply check that reach the same code the COM property reaches. The GET
// walks the table, the PUT walks the table and the OpenAPI description is
// generated from the table, so the three cannot disagree about a key.
//
// A PUT accepts any subset of the group's writable keys and is all or nothing:
// every member of the body is resolved to a row and a typed value, every row's
// own check is run, and only then is anything applied - an unknown key, a
// value of the wrong type, a read-only key or a value a setting refuses is a
// 400 that names the key (or carries the setting's own sentence, the one the
// Control Panel shows) with nothing changed. The setters are the ones COM
// calls: a Property write reaches hm_settings at once and raises
// Configuration::OnPropertyChanged, which is what refreshes the logger, the
// work queues and the delivery manager, so a change over this route is seen
// by the running server exactly as one made in the Control Panel is. The
// three settings whose setter can itself refuse (the IMAP hierarchy delimiter,
// and the two tarpit values that live in hMailServer.ini) are applied before
// the rest, so a refusal there still leaves nothing else applied.

#include "StdAfx.h"
#include "RestApiServer.h"
#include "ListManager.h"
#include "OrganisationCa.h"
#include "../Application/EffectiveSetting.h"
#include "HttpsClient.h"
#include "CountryBlocking.h"
#include "ContentPolicy.h"
#include <boost/regex.hpp>
#include "JsonDocument.h"
#include "Unicode.h"
#include "Utilities.h"
#include "ReputationWatch.h"
#include "MessageOrigin.h"
#include "WebPush.h"
#include "PasswordBreachCheck.h"
#include "PstImportPath.h"
#include "CompromiseDetector.h"
#include "../AntiVirus/Sandbox.h"
#include "SnmpAgent.h"
#include "SessionPolicies.h"
#include "SamlSignIn.h"
#include "../Application/IniSettingStore.h"
#include "../Application/IniSettingsBaseline.h"
#include "../Application/SettingsDiff.h"
#include "../Application/EnvironmentSettings.h"
#include "../Application/PropertySet.h"
#include "RestApiSettingsRows.h"
#include "../Application/Backup.h"
#include "../BO/ServerMessages.h"
#include "../BO/ServerMessage.h"
#include "../Persistence/PersistentServerMessage.h"
#include "../Scripting/ScriptServer.h"
#include "../Sieve/SieveScript.h"
#include "ICalendar.h"
#include "FileUtilities.h"
#include "../Persistence/PersistentIMAPFolder.h"
#include "../Persistence/PersistentRuleAction.h"
#include "../../SMTP/SMTPConfiguration.h"
#include "../../IMAP/IMAPConfiguration.h"
#include "../../POP3/POP3Configuration.h"
#include "../Cache/Cache.h"
#include "../Cache/CacheConfiguration.h"
#include "../Cache/CacheContainer.h"
#include "../BO/Domain.h"
#include "../BO/Account.h"
#include "../BO/Alias.h"
#include "../BO/DistributionList.h"
#include "../../IMAP/MessagesContainer.h"
#include "../Persistence/PersistentMessage.h"
#include "../Persistence/PersistentMessageMetaData.h"
#include "../Persistence/PersistentMessageIndex.h"
#include "../Application/MessageIndexer.h"

#include <climits>
#include <mutex>
#include <string>
#include <vector>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace
{
   using namespace HM;

   // ---------------------------------------------------------------------
   // The shape of a row.
   // ---------------------------------------------------------------------

   // A setting's value in transit: one of the three members is meaningful,
   // decided by the row's kind. An enumerated value travels as its number,
   // the word having been mapped on the way in and being mapped on the way out.
   struct Value
   {
      Value() : number(0), flag(false) { }

      String text;
      long number;
      bool flag;
   };

   enum Kind
   {
      KindString,
      KindInteger,
      KindBoolean,
      KindEnum
   };

   enum Access
   {
      ReadWrite,

      // Emitted by GET, refused by PUT: the log directory and the names of
      // the current log files are facts about the server, not choices.
      ReadOnly,

      // Accepted by PUT, never emitted: the relayer password.
      WriteOnly
   };

   // Whether the running server reads the setting again after it changes,
   // decided per key from the code that reads it (see the table). The
   // OpenAPI description says so for every key that waits.
   enum Effect
   {
      EffectNow,

      // Read when the server starts - the network thread pool, which servers
      // to start, the TLS contexts the listeners are built with.
      EffectRestart,

      // Written to the store and read by nothing in this server. Kept because
      // the Control Panel writes it and a Deck that mirrors the Control Panel
      // must be able to as well; the description says it is inert.
      EffectStored
   };

   struct Word
   {
      const char *word;
      long value;
   };

   // What a setter reports. status 0 is applied; 400 carries the setting's
   // own refusal sentence; 500 is a write that failed after the value was
   // accepted (an INI file that could not be written).
   struct Outcome
   {
      Outcome() : status(0) { }

      int status;
      String message;
   };

   struct Row
   {
      const char *key;
      Kind kind;
      Access access;
      Effect effect;

      // KindEnum only: the words and their stored values, ended by a null word.
      const Word *words;

      // The OpenAPI description, in one sentence. No quotes or backslashes.
      const char *description;

      // Null for a write-only row.
      Value (*get)();

      // Null for a read-only row.
      Outcome (*set)(const Value &value);

      // Optional: refuses a value before anything in the request is applied,
      // in the setting's own sentence. Empty means acceptable.
      String (*check)(const Value &value);

      // The [Settings] key the row is kept in, for the rows whose value lives in
      // hm_inisettings rather than hm_settings. When set, when the change applies
      // is the key's mark in build/ini-settings-baseline.txt - one source for the
      // row, the ini route, COM and the Control Panel - and effect is not read.
      const TCHAR *ini_key;

      // For a write-only row, the hm_settings property its setter writes. A row
      // with a getter is known by the property its getter reads (ReadRow); a
      // write-only one has nothing to read, so it says. It is what marks a
      // password the environment sets (<key>_environment) and what lends its
      // check and its setter to hmailserver --apply-environment
      // (docs/EnvironmentConfiguration.md); build/check-environment-names.py
      // holds every write-only row to naming one.
      const TCHAR *property;
   };

   struct Group
   {
      const char *name;          // for the log line and the OpenAPI summary
      const char *path;
      const Row *rows;
      size_t count;
   };

   // ---------------------------------------------------------------------
   // The accessors the rows use.
   // ---------------------------------------------------------------------

   Value Text(const String &text) { Value v; v.text = text; return v; }
   Value Number(long number) { Value v; v.number = number; return v; }
   Value Flag(bool flag) { Value v; v.flag = flag; return v; }

   Outcome Applied() { return Outcome(); }

   Outcome Refused(const char *sentence)
   {
      Outcome o;
      o.status = 400;
      o.message = sentence;
      return o;
   }

   // The same, for a refusal the setter was handed rather than one written here.
   Outcome RefusedText(const String &sentence)
   {
      Outcome o;
      o.status = 400;
      o.message = sentence;
      return o;
   }

   Outcome Failed(const char *sentence)
   {
      Outcome o;
      o.status = 500;
      o.message = sentence;
      return o;
   }

   Configuration *Config() { return Configuration::Instance(); }
   std::shared_ptr<SMTPConfiguration> Smtp() { return Configuration::Instance()->GetSMTPConfiguration(); }
   std::shared_ptr<IMAPConfiguration> Imap() { return Configuration::Instance()->GetIMAPConfiguration(); }
   std::shared_ptr<POP3Configuration> Pop3() { return Configuration::Instance()->GetPOP3Configuration(); }
   AntiSpamConfiguration &AntiSpam() { return Configuration::Instance()->GetAntiSpamConfiguration(); }
   AntiVirusConfiguration &AntiVirus() { return Configuration::Instance()->GetAntiVirusConfiguration(); }
   IniFileSettings *Ini() { return IniFileSettings::Instance(); }

   // The words the ports and routes use for eConnectionSecurity, in the order
   // the Control Panel lists them.
   const Word ConnectionSecurityWords[] =
   {
      { "none", CSNone },
      { "starttls_optional", CSSTARTTLSOptional },
      { "starttls_required", CSSTARTTLSRequired },
      { "tls", CSSSL },
      { nullptr, 0 }
   };

   // Logger::LogDevice after the COM layer's mapping. "unknown" is the value
   // every installation has until the device is chosen, and behaves as file.
   const Word LogDeviceWords[] =
   {
      { "unknown", 0 },
      { "sql", 1 },
      { "file", 2 },
      { nullptr, 0 }
   };

   // Logger::LogOutputFormat after the COM layer's mapping.
   const Word LogFormatWords[] =
   {
      { "default", 0 },
      { "ncsa", 1 },
      { nullptr, 0 }
   };

   // SyslogSink::Transport. "journal" is deliberately absent: it is not a value
   // of the setting but what "enabled, on Linux under systemd, with no collector
   // host" resolves to, and offering it as a transport would let it be chosen on
   // a machine that has no journal to write to.
   const Word SyslogTransportWords[] =
   {
      { "udp", 0 },
      { "tcp", 1 },
      { "tls", 2 },
      { nullptr, 0 }
   };

   // Every address in a complaint mailbox list has to be one, and one this server
   // hosts: a feedback loop is registered against a mailbox here, and a list
   // naming an address somewhere else would read no report and never say so. The
   // same sentences InterfaceSettings::put_ComplaintMailbox refuses with.
   String ComplaintMailboxProblem(const String &value)
   {
      String text = value;
      text.Trim();

      if (text.IsEmpty())
         return String();

      if (text.GetLength() > 255)
         return String(_T("mailbox must be at most 255 characters."));

      String list = text;
      list.Replace(_T(";"), _T(","));
      list.Replace(_T(" "), _T(","));

      std::vector<String> names = StringParser::SplitString(list, _T(","));
      int found = 0;

      for (const String &name : names)
      {
         String address = name;
         address.Trim();

         if (address.IsEmpty())
            continue;

         found++;

         if (!StringParser::IsValidEmailAddress(address, true))
            return String(_T("mailbox must be one address, or several separated by commas."));

         if (!MessageOrigin::IsHostedAddress(address))
            return Formatter::Format(_T("{0} is not an address this server hosts, and a provider could not deliver a report to it."), address);
      }

      if (found == 0)
         return String(_T("mailbox must be one address, or several separated by commas."));

      return String();
   }

   // BounceTracker::Action, in the order the Control Panel lists them.
   const Word BounceSuppressionActionWords[] =
   {
      { "refuse", 0 },
      { "drop", 1 },
      { "observe", 2 },
      { nullptr, 0 }
   };

   // InterfaceAntiVirus::put_Action's two values - eAntivirusAction over COM,
   // hDeleteEmail and hDeleteAttachments - in the order the Control Panel
   // lists them. Anything else is refused there rather than stored, and here.
   const Word AntiVirusActionWords[] =
   {
      { "delete_email", AntiVirusConfiguration::ActionDelete },
      { "delete_attachments", AntiVirusConfiguration::ActionStripAttachments },
      { nullptr, 0 }
   };

   // A port the ClamAV client could never connect to is refused before it is
   // stored, as the ports and routes refuse one.
   const char *ClamAVPortRefusal = "ClamAVPort must be between 1 and 65535.";
   const char *QuotaGraceRefusal = "QuotaGracePercent must be between 0 and 1000.";
   const char *QuotaSendStopRefusal = "QuotaSendStopPercent must be between 0 and 1000.";

   // InterfaceSettings' three sentences for the webhook ranges, verbatim, so that
   // a value outside a range is refused over REST exactly as it is over COM
   // rather than clamped by the setter without a word.
   const char *WebhookMaxAttemptsRefusal = "WebhookMaxAttempts must be between 1 and 20.";
   const char *WebhookMaxPerMinuteRefusal = "WebhookMaxPerMinute must be between 1 and 100000.";
   const char *WebhookRetentionDaysRefusal = "WebhookRetentionDays must be between 0 (keep everything) and 3650.";

   // The migration jobs' ceiling per source host (Application/MigrationManager.h),
   // refused out of range rather than clamped by the setter without a word.
   const char *MigrationConnectionsPerHostRefusal = "MigrationConnectionsPerHost must be between 1 and 64.";

   // InterfaceSettings::put_IMAPHierarchyDelimiter's sentence, verbatim.
   const char *HierarchyDelimiterRefusal =
      "It was not possible to change the IMAP hierarchy delimiter. It has probably failed because there exists one or more IMAP folders containing the new character.";

   // InterfaceAntiSpam's sentences for the two tarpit values, verbatim.
   const char *TarpitDelayRefusal = "TarpitDelay must be between 0 and 30 seconds.";
   const char *TarpitCountRefusal = "TarpitCount cannot be negative.";
   const char *IniWriteFailure = "The setting could not be written to hMailServer.INI. Nothing has been changed.";

   // InterfaceSettings::put_GeoIpDatabaseMaxAgeDays's sentence, verbatim.
   const char *GeoIpMaxAgeRefusal =
      "GeoIpDatabaseMaxAgeDays is between 0 and 3650 days. 0 never calls the database stale.";

   // The four country policies differ only in which setting they write, and
   // every one of them is read, refused and stored by CountryBlocking - so the
   // check below is the same function four times and the setter is the same
   // function with a different destination.
   String CountryPolicyCheck(const Value &v)
   {
      String canonical;
      String error;

      CountryBlocking::Canonicalise(v.text, canonical, error);
      return error;
   }

   Outcome ApplyCountryPolicy(const Value &v, void (Configuration::*setter)(const String &))
   {
      String canonical;
      String error;

      if (!CountryBlocking::Canonicalise(v.text, canonical, error))
      {
         Outcome outcome;
         outcome.status = 400;
         outcome.message = error;
         return outcome;
      }

      (Config()->*setter)(canonical);
      return Applied();
   }

   // InterfaceSettings::put_BreachCheckMinimumCount's sentence, verbatim.
   const char *BreachCheckMinimumRefusal =
      "BreachCheckMinimumCount must be 1 or more: a password is refused when it has appeared in the corpus at least this many times.";

   // InterfaceSettings' sentences for the spam trap settings, verbatim.
   const char *SpamTrapBanRefusal =
      "SpamTrapBanMinutes is between 0 and 525600 (a year). 0 bans nobody, which leaves the sender block and the record.";
   const char *SpamTrapBlockRefusal =
      "SpamTrapBlockDays is between 0 and 3650. 0 blocks nobody, which leaves the ban and the record.";
   const char *SpamTrapRetentionRefusal =
      "SpamTrapHitRetentionDays is between 1 and 3650: the record of spam trap hits is kept for that long.";

   // InterfaceAntiSpam's sentences for the hardened greylisting, verbatim.
   const char *GreyListingAbuseThresholdRefusal =
      "GreyListingAbuseThreshold is between 0 and 100000. 0 never refuses: a triplet retried too soon is deferred again, as it always was.";
   const char *GreyListingAbuseLockRefusal =
      "GreyListingAbuseLockMinutes is between 1 and 10080 (a week): how long the 550 of GreyListingAbuseThreshold lasts.";

   // InterfaceAntiSpam::put_BanMultiRcptNullSenderHours's sentence, verbatim.
   const char *NullSenderBanHoursRefusal =
      "BanMultiRcptNullSenderHours is between 1 and 8760 (a year): how long the address of a refused multi-recipient null sender is banned.";

   // PasswordBreachCheck::UnreachableAction, as the Control Panel lists it.
   const Word BreachCheckUnreachableWords[] =
   {
      { "allow", PasswordBreachCheck::UnreachableAllow },
      { "refuse", PasswordBreachCheck::UnreachableRefuse },
      { nullptr, 0 }
   };

   // CompromiseDetector::Mode: what a takeover shape does.
   const Word CompromiseModeWords[] =
   {
      { "off", CompromiseDetector::ModeOff },
      { "watch", CompromiseDetector::ModeWatch },
      { "enforce", CompromiseDetector::ModeEnforce },
      { nullptr, 0 }
   };

   // SandboxTimeoutAction: what a hold the sandbox did not answer for ends in.
   const Word SandboxTimeoutActionWords[] =
   {
      { "deliver", 0 },
      { "bounce", 1 },
      { nullptr, 0 }
   };

   // Every logging switch is one bit of PROPERTY_LOGGING, and each Configuration
   // setter rewrites the whole mask, so setting one bit at a time is what COM
   // does too (InterfaceLogging::put_LogSMTP and the rest).

   // ---------------------------------------------------------------------
   // The tables. A captureless lambda converts to the function pointer the
   // row holds; the macros keep a row to a few lines so that the table reads
   // as a table. v is the Value being applied.
   // ---------------------------------------------------------------------

   namespace
   {
      // An import endpoint or base URL: https, or http to this machine (which is
      // what a test's local provider is). The base URL may be empty; an endpoint
      // may not, since a provider without one could never finish a sign-in.
      String ImportUrlProblem(const String &value, bool emptyAllowed)
      {
         String url = value;
         url.Trim();
         if (url.IsEmpty())
            return emptyAllowed ? String() : String(_T("An endpoint cannot be empty."));
         if (url.GetLength() > 500)
            return String(_T("At most 500 characters."));
         if (url.Left(8).CompareNoCase(_T("https://")) == 0)
            return String();
         if (url.Left(17).CompareNoCase(_T("http://127.0.0.1:")) == 0 || url.Left(17).CompareNoCase(_T("http://localhost:")) == 0)
            return String();
         return String(_T("An https:// address, or http:// to 127.0.0.1 or localhost."));
      }
   }

#define ROW_TEXT(expr)      [] { return Text(expr); }
#define ROW_NUMBER(expr)    [] { return Number((long) (expr)); }
#define ROW_FLAG(expr)      [] { return Flag((expr) ? true : false); }
#define ROW_SET(statement)      [] (const Value &v) { statement; return Applied(); }
#define ROW_NO_CHECK            nullptr

   const Row ServerRows[] =
   {
      // Identity.
      { "host_name", KindString, ReadWrite, EffectNow, nullptr,
        "The name the server gives in its SMTP greeting, in HELO/EHLO and in Received headers.",
        ROW_TEXT(Config()->GetHostName()),
        [] (const Value &v) { String name = v.text; Config()->SetHostName(name); return Applied(); }, ROW_NO_CHECK },
      { "default_domain", KindString, ReadWrite, EffectNow, nullptr,
        "The domain appended to a bare user name at logon.",
        ROW_TEXT(Config()->GetDefaultDomain()), ROW_SET(Config()->SetDefaultDomain(v.text)), ROW_NO_CHECK },
      { "mirror_email_address", KindString, ReadWrite, EffectNow, nullptr,
        "An address that receives a copy of every message the server handles, or empty for none.",
        ROW_TEXT(Config()->GetMirrorAddress()), ROW_SET(Config()->SetMirrorAddress(v.text)), ROW_NO_CHECK },

      // Limits.
      { "max_message_size_kb", KindInteger, ReadWrite, EffectNow, nullptr,
        "The largest message SMTP and IMAP APPEND accept, in KB; 0 is no limit.",
        ROW_NUMBER(Smtp()->GetMaxMessageSize()), ROW_SET(Smtp()->SetMaxMessageSize((int) v.number)), ROW_NO_CHECK },
      { "max_smtp_connections", KindInteger, ReadWrite, EffectNow, nullptr,
        "The most simultaneous SMTP connections; 0 is no limit.",
        ROW_NUMBER(Smtp()->GetMaxSMTPConnections()), ROW_SET(Smtp()->SetMaxSMTPConnections((int) v.number)), ROW_NO_CHECK },
      { "max_imap_connections", KindInteger, ReadWrite, EffectNow, nullptr,
        "The most simultaneous IMAP connections; 0 is no limit.",
        ROW_NUMBER(Imap()->GetMaxIMAPConnections()), ROW_SET(Imap()->SetMaxIMAPConnections((int) v.number)), ROW_NO_CHECK },
      { "max_pop3_connections", KindInteger, ReadWrite, EffectNow, nullptr,
        "The most simultaneous POP3 connections; 0 is no limit.",
        ROW_NUMBER(Pop3()->GetMaxPOP3Connections()), ROW_SET(Pop3()->SetMaxPOP3Connections((int) v.number)), ROW_NO_CHECK },
      { "max_delivery_threads", KindInteger, ReadWrite, EffectNow, nullptr,
        "The most messages delivered at the same time.",
        ROW_NUMBER(Smtp()->GetMaxNoOfDeliveryThreads()), ROW_SET(Smtp()->SetMaxNoOfDeliveryThreads((int) v.number)), ROW_NO_CHECK },
      { "max_asynchronous_threads", KindInteger, ReadWrite, EffectNow, nullptr,
        "The most background tasks run at the same time.",
        ROW_NUMBER(Config()->GetAsynchronousThreads()), ROW_SET(Config()->SetAsynchronousThreads((int) v.number)), ROW_NO_CHECK },
      { "tcpip_threads", KindInteger, ReadWrite, EffectRestart, nullptr,
        "The size of the thread pool that serves every connection.",
        ROW_NUMBER(Config()->GetTCPIPThreads()), ROW_SET(Config()->SetTCPIPThreads((int) v.number)), ROW_NO_CHECK },
      { "worker_thread_priority", KindInteger, ReadWrite, EffectStored, nullptr,
        "The Control Panel's worker thread priority.",
        ROW_NUMBER(Config()->GetWorkerThreadPriority()), ROW_SET(Config()->SetWorkerThreadPriority((int) v.number)), ROW_NO_CHECK },

      // Which servers run.
      { "service_smtp", KindBoolean, ReadWrite, EffectRestart, nullptr,
        "Whether the SMTP server is started.",
        ROW_FLAG(Config()->GetUseSMTP()), ROW_SET(Config()->SetUseSMTP(v.flag)), ROW_NO_CHECK },
      { "service_pop3", KindBoolean, ReadWrite, EffectRestart, nullptr,
        "Whether the POP3 server is started.",
        ROW_FLAG(Config()->GetUsePOP3()), ROW_SET(Config()->SetUsePOP3(v.flag)), ROW_NO_CHECK },
      { "service_imap", KindBoolean, ReadWrite, EffectRestart, nullptr,
        "Whether the IMAP server is started.",
        ROW_FLAG(Config()->GetUseIMAP()), ROW_SET(Config()->SetUseIMAP(v.flag)), ROW_NO_CHECK },

      // Greetings.
      { "welcome_smtp", KindString, ReadWrite, EffectNow, nullptr,
        "The SMTP greeting, or empty for the default.",
        ROW_TEXT(Smtp()->GetWelcomeMessage()), ROW_SET(Smtp()->SetWelcomeMessage(v.text)), ROW_NO_CHECK },
      { "welcome_pop3", KindString, ReadWrite, EffectNow, nullptr,
        "The POP3 greeting, or empty for the default.",
        ROW_TEXT(Pop3()->GetWelcomeMessage()), ROW_SET(Pop3()->SetWelcomeMessage(v.text)), ROW_NO_CHECK },
      { "welcome_imap", KindString, ReadWrite, EffectNow, nullptr,
        "The IMAP greeting, or empty for the default.",
        ROW_TEXT(Imap()->GetWelcomeMessage()), ROW_SET(Imap()->SetWelcomeMessage(v.text)), ROW_NO_CHECK },

      // Outbound delivery.
      { "smtp_relayer", KindString, ReadWrite, EffectNow, nullptr,
        "The smart host every outbound message is handed to, or empty for direct delivery to the recipient's MX.",
        ROW_TEXT(Smtp()->GetSMTPRelayer()), ROW_SET(Smtp()->SetSMTPRelayer(v.text)), ROW_NO_CHECK },
      { "smtp_relayer_port", KindInteger, ReadWrite, EffectNow, nullptr,
        "The smart host's port.",
        ROW_NUMBER(Smtp()->GetSMTPRelayerPort()), ROW_SET(Smtp()->SetSMTPRelayerPort(v.number)), ROW_NO_CHECK },
      { "smtp_relayer_connection_security", KindEnum, ReadWrite, EffectNow, ConnectionSecurityWords,
        "How the connection to the smart host is secured.",
        ROW_NUMBER(Smtp()->GetSMTPRelayerConnectionSecurity()),
        ROW_SET(Smtp()->SetSMTPRelayerConnectionSecurity((ConnectionSecurity) v.number)), ROW_NO_CHECK },
      { "smtp_relayer_requires_authentication", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether the server authenticates to the smart host.",
        ROW_FLAG(Smtp()->GetSMTPRelayerRequiresAuthentication()),
        ROW_SET(Smtp()->SetSMTPRelayerRequiresAuthentication(v.flag)), ROW_NO_CHECK },
      { "smtp_relayer_username", KindString, ReadWrite, EffectNow, nullptr,
        "The user name for the smart host.",
        ROW_TEXT(Smtp()->GetSMTPRelayerUsername()), ROW_SET(Smtp()->SetSMTPRelayerUsername(v.text)), ROW_NO_CHECK },
      { "smtp_relayer_password", KindString, WriteOnly, EffectNow, nullptr,
        "The password for the smart host. Accepted here and never emitted.",
        nullptr, ROW_SET(Smtp()->SetSMTPRelayerPassword(v.text)), ROW_NO_CHECK, nullptr, PROPERTY_SMTPRELAYER_PASSWORD },
      { "smtp_connection_security", KindEnum, ReadWrite, EffectNow, ConnectionSecurityWords,
        "How a direct delivery to a recipient's MX is secured.",
        ROW_NUMBER(Smtp()->GetSMTPConnectionSecurity()),
        ROW_SET(Smtp()->SetSMTPConnectionSecurity((ConnectionSecurity) v.number)), ROW_NO_CHECK },
      { "smtp_no_of_tries", KindInteger, ReadWrite, EffectNow, nullptr,
        "How many times a delivery is attempted before the message is returned.",
        ROW_NUMBER(Smtp()->GetNoOfRetries()), ROW_SET(Smtp()->SetNoOfRetries(v.number)), ROW_NO_CHECK },
      { "smtp_minutes_between_try", KindInteger, ReadWrite, EffectNow, nullptr,
        "The minutes between two delivery attempts.",
        ROW_NUMBER(Smtp()->GetMinutesBetweenTry()), ROW_SET(Smtp()->SetMinutesBetweenTry(v.number)), ROW_NO_CHECK },
      { "smtp_delivery_bind_to_ip", KindString, ReadWrite, EffectNow, nullptr,
        "The local address outbound SMTP connects from, or empty for any. One address, or one of each family separated by a comma (a dual-stack server binds its static IPv6 address so that its PTR and SPF match, and its IPv4 address for IPv4 hosts); an address of the other family than the host being reached is not used for that connection.",
        ROW_TEXT(Smtp()->GetSMTPDeliveryBindToIP()), ROW_SET(Smtp()->SetSMTPDeliveryBindToIP(v.text)), ROW_NO_CHECK },
      { "max_smtp_recipients_in_batch", KindInteger, ReadWrite, EffectNow, nullptr,
        "The most recipients handed to one host in one SMTP session.",
        ROW_NUMBER(Smtp()->GetMaxSMTPRecipientsInBatch()), ROW_SET(Smtp()->SetMaxSMTPRecipientsInBatch((int) v.number)), ROW_NO_CHECK },
      { "max_number_of_mx_hosts", KindInteger, ReadWrite, EffectNow, nullptr,
        "The most MX hosts tried for one recipient domain.",
        ROW_NUMBER(Smtp()->GetMaxNumberOfMXHosts()), ROW_SET(Smtp()->SetMaxNumberOfMXHosts((int) v.number)), ROW_NO_CHECK },

      // Outbound shaping (SMTP/DestinationBackoff.h, docs/OutboundShaping.md).
      { "outbound_backoff_minutes", KindInteger, ReadWrite, EffectNow, nullptr,
        "How long deliveries to a destination are held after it answers a slow-down reply - a 421, an enhanced code of 4.7.0, 4.7.28, 4.7.500, 4.7.650, 4.7.652 or 4.3.2, or a 4xx whose English text says rate limit, too many messages, try again later and the like; never a 4.7.1, a 4.2.x, a 4.5.3 or a 4.4.x, and never a reply that says greylist: the first window, in minutes. When it ends one delivery probes the destination, and a second slow-down reply doubles the window up to outbound_backoff_max_minutes. A deferral this server chooses is not counted against the message's retries until the message has been queued longer than its retry schedule spans. Never above outbound_backoff_max_minutes: to move both past each other, move the ceiling first when raising and this one first when lowering. 0 turns the back-off off. 0 to 10080; 2 by default.",
        ROW_NUMBER(Config()->GetOutboundBackoffMinutes()), ROW_SET(Config()->SetOutboundBackoffMinutes((int) v.number)),
        [] (const Value &v)
        {
           if (v.number < 0 || v.number > 10080)
              return String(_T("outbound_backoff_minutes must be between 0 and 10080."));
           if (v.number > Config()->GetOutboundBackoffMaxMinutes())
              return String(_T("outbound_backoff_minutes must not be above outbound_backoff_max_minutes."));
           return String();
        } },
      { "outbound_backoff_max_minutes", KindInteger, ReadWrite, EffectNow, nullptr,
        "The most a back-off window grows to by doubling, in minutes. Never below outbound_backoff_minutes, which is never above it: to move both past each other, move this one first when raising and the base first when lowering. 1 to 10080; 60 by default.",
        ROW_NUMBER(Config()->GetOutboundBackoffMaxMinutes()), ROW_SET(Config()->SetOutboundBackoffMaxMinutes((int) v.number)),
        [] (const Value &v)
        {
           if (v.number < 1 || v.number > 10080)
              return String(_T("outbound_backoff_max_minutes must be between 1 and 10080."));
           if (v.number < Config()->GetOutboundBackoffMinutes())
              return String(_T("outbound_backoff_max_minutes must not be below outbound_backoff_minutes."));
           return String();
        } },
      { "max_outbound_connections_per_destination", KindInteger, ReadWrite, EffectNow, nullptr,
        "The most sessions this server holds open at once to one destination - a recipient domain, or the host of a route - that no remote domain policy sets a connection ceiling for; a policy's own ceiling wins where there is one. Over it, a delivery is deferred for a minute without counting a retry. 0 is no ceiling. 0 to 1000; 0 by default.",
        ROW_NUMBER(Config()->GetMaxOutboundConnectionsPerDestination()), ROW_SET(Config()->SetMaxOutboundConnectionsPerDestination((int) v.number)),
        [] (const Value &v) { return (v.number < 0 || v.number > 1000) ? String(_T("max_outbound_connections_per_destination must be between 0 and 1000.")) : String(); } },
      { "max_outbound_connections_per_tenant", KindInteger, ReadWrite, EffectNow, nullptr,
        "The most deliveries one tenant may have in flight at once, where the tenant is the hosted domain of the envelope sender (an alias domain counts as its domain) and every sender this server does not host is the one tenant (external). A share of connections, never more than max_delivery_threads; the per-account sending limit is the volume control. Over it, a delivery is deferred for a minute without counting a retry. 0 is no share. 0 to 1000; 0 by default.",
        ROW_NUMBER(Config()->GetMaxOutboundConnectionsPerTenant()), ROW_SET(Config()->SetMaxOutboundConnectionsPerTenant((int) v.number)),
        [] (const Value &v) { return (v.number < 0 || v.number > 1000) ? String(_T("max_outbound_connections_per_tenant must be between 0 and 1000.")) : String(); } },

      // Attachments opened somewhere safe (AntiVirus/Sandbox.h, docs/Sandbox.md).
      // The checks are Sandbox's own, which COM asks too, so a value is refused
      // in the same words on every surface. The switch and the two addresses
      // are judged together as well, by GroupPut, against what the request
      // would leave: on with an address empty is refused.
      { "sandbox_enabled", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether a message that no signed-in user of this server submitted, with an attachment of a submitted type, is held, in no mailbox and not in the quarantine, until the sandbox has answered for each such file; a clean answer delivers it with a header, a malicious one quarantines it. Off by default. Refused on while either address is empty, in this request or already; a configuration that has it so anyway holds nothing and says so in the application log at each start.",
        ROW_FLAG(Config()->GetSandboxEnabled()), ROW_SET(Config()->SetSandboxEnabled(v.flag)), ROW_NO_CHECK },
      { "sandbox_submit_url", KindString, ReadWrite, EffectNow, nullptr,
        "Where each file is POSTed as multipart/form-data with the fields file, filename, sha256 and message_id (this server's queue id), with the bearer token when one is set; the answer is a JSON object whose id (at most 1000 characters) names the submission. https, or plain http to this machine only; stored trimmed. Empty submits nothing.",
        ROW_TEXT(Config()->GetSandboxSubmitUrl()), ROW_SET(Config()->SetSandboxSubmitUrl(v.text)),
        [] (const Value &v) -> String { String url = v.text; url.Trim(); return Sandbox::UrlProblem(url, _T("sandbox_submit_url")); } },
      { "sandbox_verdict_url", KindString, ReadWrite, EffectNow, nullptr,
        "Where a file's verdict is asked for with a GET, with id=<the submission id> added, or sha256=<the file's hash> when the submit answered no id; {id} and {sha256} in the address are replaced instead when present, {id} by the hash when there is no id. The answer is a JSON object with state (pending or done), verdict (clean, malicious, suspicious or error), score (0 to 100, a number, which a suspicious verdict must carry) and detail. https, or plain http to this machine only; stored trimmed.",
        ROW_TEXT(Config()->GetSandboxVerdictUrl()), ROW_SET(Config()->SetSandboxVerdictUrl(v.text)),
        [] (const Value &v) -> String { String url = v.text; url.Trim(); return Sandbox::UrlProblem(url, _T("sandbox_verdict_url")); } },
      { "sandbox_token", KindString, WriteOnly, EffectNow, nullptr,
        "The bearer token sent to both addresses in an Authorization header: visible ASCII only, at most 2048 characters, and 248 while ProtectStoredSecretsWithDPAPI is 0, since it has to fit the store once protected. Stored protected, never emitted and left out of the backup archive; empty sends none.",
        nullptr, ROW_SET(Config()->SetSandboxToken(v.text)),
        [] (const Value &v) -> String { return Sandbox::TokenProblem(v.text, _T("sandbox_token")); },
        nullptr, PROPERTY_SANDBOX_TOKEN },
      { "sandbox_timeout_minutes", KindInteger, ReadWrite, EffectNow, nullptr,
        "How long a held message waits, in minutes from the moment it was held; past it the hold ends by sandbox_timeout_action whatever the sandbox is doing. 1 to 1440; 15 by default.",
        ROW_NUMBER(Config()->GetSandboxTimeoutMinutes()), ROW_SET(Config()->SetSandboxTimeoutMinutes((int) v.number)),
        [] (const Value &v) { return (v.number < 1 || v.number > 1440) ? String(_T("sandbox_timeout_minutes must be between 1 and 1440.")) : String(); } },
      { "sandbox_timeout_action", KindEnum, ReadWrite, EffectNow, SandboxTimeoutActionWords,
        "What a hold ends in when the wait is over or the sandbox answers error: deliver delivers the message with a header saying so, bounce returns it to its sender with 5.7.1. deliver by default.",
        ROW_NUMBER(Config()->GetSandboxTimeoutAction()), ROW_SET(Config()->SetSandboxTimeoutAction((int) v.number)), ROW_NO_CHECK },
      { "sandbox_max_size_kb", KindInteger, ReadWrite, EffectNow, nullptr,
        "The largest attachment submitted, in KB. A larger one is not submitted and the header names it; the message is still held for any other file of a submitted type, and delivered at once only when it has none. 1 to 81920, the size of message this server parses; 20480 by default.",
        ROW_NUMBER(Config()->GetSandboxMaxSizeKB()), ROW_SET(Config()->SetSandboxMaxSizeKB((int) v.number)),
        [] (const Value &v) { return (v.number < 1 || v.number > 81920) ? String(_T("sandbox_max_size_kb must be between 1 and 81920.")) : String(); } },
      { "sandbox_types", KindString, ReadWrite, EffectNow, nullptr,
        "The attachments that are submitted: file extensions (exe) and media types (application/pdf), separated by commas, stored in lower case with each once. A file matches on the extension of either of its names, trailing dots and spaces aside, or on its media type. An archive is submitted whole and not opened here; a forwarded message is opened. The executable and document set by default; empty submits nothing.",
        ROW_TEXT(Config()->GetSandboxTypes()),
        [] (const Value &v)
        {
           String canonical;
           Sandbox::TypesProblem(v.text, _T("sandbox_types"), canonical);
           Config()->SetSandboxTypes(canonical);
           return Applied();
        },
        [] (const Value &v) -> String
        {
           String canonical;
           return Sandbox::TypesProblem(v.text, _T("sandbox_types"), canonical);
        } },
      { "sandbox_poll_seconds", KindInteger, ReadWrite, EffectNow, nullptr,
        "How often the verdicts of the held messages are asked for, in seconds; a pending answer waits for the next, and so does a request that failed. 5 to 3600; 30 by default.",
        ROW_NUMBER(Config()->GetSandboxPollSeconds()), ROW_SET(Config()->SetSandboxPollSeconds((int) v.number)),
        [] (const Value &v) { return (v.number < 5 || v.number > 3600) ? String(_T("sandbox_poll_seconds must be between 5 and 3600.")) : String(); } },
      { "sandbox_score_threshold", KindInteger, ReadWrite, EffectNow, nullptr,
        "The score at or above which a suspicious verdict is treated as malicious; below it, as clean. 0 to 100; 70 by default.",
        ROW_NUMBER(Config()->GetSandboxScoreThreshold()), ROW_SET(Config()->SetSandboxScoreThreshold((int) v.number)),
        [] (const Value &v) { return (v.number < 0 || v.number > 100) ? String(_T("sandbox_score_threshold must be between 0 and 100.")) : String(); } },
      { "sandbox_cache_hours", KindInteger, ReadWrite, EffectNow, nullptr,
        "How long a clean or malicious verdict is remembered by the file's SHA-256, in hours, server-wide: the same file within it, in a message to any domain, is not submitted again, a remembered clean one is believed until it expires, and a remembered malicious one is quarantined at once. A suspicious answer is remembered as what the threshold made it then. An error is never remembered. 0 remembers nothing. 0 to 720; 24 by default.",
        ROW_NUMBER(Config()->GetSandboxCacheHours()), ROW_SET(Config()->SetSandboxCacheHours((int) v.number)),
        [] (const Value &v) { return (v.number < 0 || v.number > 720) ? String(_T("sandbox_cache_hours must be between 0 and 720.")) : String(); } },
      { "sandbox_internal_mail", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether what this server's own signed-in users submit - over authenticated SMTP or the webmail, to anyone - is held and its files uploaded to the sandbox as mail from outside is. Off by default, so that their outgoing documents never leave for a third party unasked; a stolen account's mail to colleagues is then not examined either.",
        ROW_FLAG(Config()->GetSandboxInternalMail()), ROW_SET(Config()->SetSandboxInternalMail(v.flag)), ROW_NO_CHECK },
      { "sandbox_max_files", KindInteger, ReadWrite, EffectNow, nullptr,
        "The most files one message may send to the sandbox. A message with more to submit is not held: sandbox_timeout_action decides it at once, with a header saying why. 1 to 100; 10 by default.",
        ROW_NUMBER(Config()->GetSandboxMaxFiles()), ROW_SET(Config()->SetSandboxMaxFiles((int) v.number)),
        [] (const Value &v) { return (v.number < 1 || v.number > 100) ? String(_T("sandbox_max_files must be between 1 and 100.")) : String(); } },

      // The SNMP agent (Util/SnmpAgent.h, docs/Snmp.md). Every row waits for a
      // restart: the agent reads them when it starts, at a service start or a
      // Reinitialize. The checks are SnmpAgent's own. The two passwords and the
      // community are write-only, as the relayer's password is. There is no COM
      // property for any of them, as there is none for the metrics listener.
      { "snmp_enabled", KindBoolean, ReadWrite, EffectRestart, nullptr,
        "Whether the read-only SNMP agent runs: SNMPv3 at authPriv, and SNMPv2c only while snmp_v2c_enabled is on as well. Off by default. On, it still answers nobody until snmp_allowed_addresses names a manager.",
        ROW_FLAG(Config()->GetSnmpEnabled()), ROW_SET(Config()->SetSnmpEnabled(v.flag)), ROW_NO_CHECK },
      { "snmp_port", KindInteger, ReadWrite, EffectRestart, nullptr,
        "The UDP port the agent answers on. 161 by default; 1 to 65535.",
        ROW_NUMBER(Config()->GetSnmpPort()), ROW_SET(Config()->SetSnmpPort((int) v.number)),
        [] (const Value &v) { return (v.number < 1 || v.number > 65535) ? String(_T("snmp_port must be between 1 and 65535.")) : String(); } },
      { "snmp_bind_address", KindString, ReadWrite, EffectRestart, nullptr,
        "The address the agent listens on: empty for every address, or one IPv4 or IPv6 address of this machine. 127.0.0.1 is the careful choice when the manager runs on the same machine; otherwise an address on a management network.",
        ROW_TEXT(Config()->GetSnmpBindAddress()), ROW_SET(Config()->SetSnmpBindAddress(v.text)),
        [] (const Value &v) { return SnmpAgent::BindAddressProblem(v.text); } },
      { "snmp_allowed_addresses", KindString, ReadWrite, EffectRestart, nullptr,
        "The managers the agent answers: addresses and ranges in CIDR form separated by commas, such as 192.0.2.10, 192.0.2.11. Empty answers nobody. A datagram from any other address is dropped unanswered before it is read. Name the managers one by one rather than by range: the list judges a datagram's source address, which UDP does not prove.",
        ROW_TEXT(Config()->GetSnmpAllowedAddresses()), ROW_SET(Config()->SetSnmpAllowedAddresses(v.text)),
        [] (const Value &v) { return SnmpAgent::AllowedAddressesProblem(v.text); } },
      { "snmp_v3_user", KindString, ReadWrite, EffectRestart, nullptr,
        "The one SNMPv3 user, answered at authPriv only: HMAC-SHA-256 authentication (usmHMAC192SHA256AuthProtocol, RFC 7860) and AES-128 privacy (RFC 3826). Empty for none. At most 32 visible ASCII characters.",
        ROW_TEXT(Config()->GetSnmpV3User()), ROW_SET(Config()->SetSnmpV3User(v.text)),
        [] (const Value &v) { return SnmpAgent::UserProblem(v.text); } },
      { "snmp_v3_auth_password", KindString, WriteOnly, EffectRestart, nullptr,
        "The SNMPv3 user's authentication password, 8 to 64 ASCII characters. Stored protected, never emitted and left out of the backup archive.",
        nullptr, ROW_SET(Config()->SetSnmpV3AuthPassword(v.text)),
        [] (const Value &v) { return SnmpAgent::PasswordProblem(v.text, _T("snmp_v3_auth_password")); },
        nullptr, PROPERTY_SNMP_V3_AUTH_PASSWORD },
      { "snmp_v3_priv_password", KindString, WriteOnly, EffectRestart, nullptr,
        "The SNMPv3 user's privacy password, 8 to 64 ASCII characters. Stored protected, never emitted and left out of the backup archive.",
        nullptr, ROW_SET(Config()->SetSnmpV3PrivPassword(v.text)),
        [] (const Value &v) { return SnmpAgent::PasswordProblem(v.text, _T("snmp_v3_priv_password")); },
        nullptr, PROPERTY_SNMP_V3_PRIV_PASSWORD },
      { "snmp_v2c_enabled", KindBoolean, ReadWrite, EffectRestart, nullptr,
        "Whether SNMPv2c is answered as well, for a monitoring system that cannot do SNMPv3. Off by default, even with the agent on. Its community crosses the network in the clear with every request, so anyone who can see that traffic can read everything the agent serves.",
        ROW_FLAG(Config()->GetSnmpV2cEnabled()), ROW_SET(Config()->SetSnmpV2cEnabled(v.flag)), ROW_NO_CHECK },
      { "snmp_v2c_community", KindString, WriteOnly, EffectRestart, nullptr,
        "The SNMPv2c community, at most 64 visible ASCII characters. Stored protected, never emitted and left out of the backup archive; empty answers no SNMPv2c request.",
        nullptr, ROW_SET(Config()->SetSnmpV2cCommunity(v.text)),
        [] (const Value &v) { return SnmpAgent::CommunityProblem(v.text); },
        nullptr, PROPERTY_SNMP_V2C_COMMUNITY },
      { "snmp_contact", KindString, ReadWrite, EffectRestart, nullptr,
        "sysContact: who to ask about this server, as a manager shows it. One line of printable ASCII, at most 255 characters (a DisplayString).",
        ROW_TEXT(Config()->GetSnmpContact()), ROW_SET(Config()->SetSnmpContact(v.text)),
        [] (const Value &v) { return SnmpAgent::TextProblem(v.text, _T("snmp_contact")); } },
      { "snmp_location", KindString, ReadWrite, EffectRestart, nullptr,
        "sysLocation: where this server is, as a manager shows it. One line of printable ASCII, at most 255 characters (a DisplayString).",
        ROW_TEXT(Config()->GetSnmpLocation()), ROW_SET(Config()->SetSnmpLocation(v.text)),
        [] (const Value &v) { return SnmpAgent::TextProblem(v.text, _T("snmp_location")); } },

      // Inbound SMTP behaviour.
      { "allow_smtp_auth_plain", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether AUTH PLAIN and AUTH LOGIN are offered on an unencrypted connection.",
        ROW_FLAG(Smtp()->GetAuthAllowPlainText()), ROW_SET(Smtp()->SetAuthAllowPlainText(v.flag)), ROW_NO_CHECK },
      { "deny_mail_from_null", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether an empty envelope sender (MAIL FROM:<>) is refused.",
        ROW_FLAG(!Smtp()->GetAllowMailFromNull()), ROW_SET(Smtp()->SetAllowMailFromNull(!v.flag)), ROW_NO_CHECK },
      { "allow_incorrect_line_endings", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether a bare LF in SMTP data is accepted as a line ending.",
        ROW_FLAG(Smtp()->GetAllowIncorrectLineEndings()), ROW_SET(Smtp()->SetAllowIncorrectLineEndings(v.flag)), ROW_NO_CHECK },
      { "add_delivered_to_header", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether a Delivered-To header is added on local delivery.",
        ROW_FLAG(Smtp()->GetAddDeliveredToHeader()), ROW_SET(Smtp()->SetAddDeliveredToHeader(v.flag)), ROW_NO_CHECK },
      { "disconnect_invalid_clients", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether a client is disconnected after too many invalid commands.",
        ROW_FLAG(Config()->GetDisconnectInvalidClients()), ROW_SET(Config()->SetDisconnectInvalidClients(v.flag)), ROW_NO_CHECK },
      { "max_number_of_invalid_commands", KindInteger, ReadWrite, EffectNow, nullptr,
        "How many invalid commands are tolerated before the disconnect.",
        ROW_NUMBER(Config()->GetMaximumIncorrectCommands()), ROW_SET(Config()->SetMaximumIncorrectCommands((int) v.number)), ROW_NO_CHECK },
      { "rule_loop_limit", KindInteger, ReadWrite, EffectNow, nullptr,
        "How many times rules may re-deliver one message before the loop is stopped.",
        ROW_NUMBER(Smtp()->GetRuleLoopLimit()), ROW_SET(Smtp()->SetRuleLoopLimit((int) v.number)), ROW_NO_CHECK },

      // IMAP.
      { "imap_public_folder_name", KindString, ReadWrite, EffectNow, nullptr,
        "The name under which the public folders appear in IMAP LIST.",
        ROW_TEXT(Imap()->GetIMAPPublicFolderName()), ROW_SET(Imap()->SetIMAPPublicFolderName(v.text)), ROW_NO_CHECK },
      { "imap_hierarchy_delimiter", KindString, ReadWrite, EffectNow, nullptr,
        "The character between IMAP folder names. Refused while a folder or a rule action contains the new character; rule actions are rewritten when it changes.",
        ROW_TEXT(Imap()->GetHierarchyDelimiter()),
        [] (const Value &v) { return Imap()->SetHierarchyDelimiter(v.text) ? Applied() : Refused(HierarchyDelimiterRefusal); },
        [] (const Value &v) -> String
        {
           // The setter's own two read-only checks, run before anything in the
           // request is applied; the setter runs them again when it applies.
           if (Imap()->GetHierarchyDelimiter() == v.text)
              return String();
           if (PersistentIMAPFolder::GetExistsFolderContainingCharacter(v.text) ||
               PersistentRuleAction::GetExistsFolderReferenceContainingCharacter(v.text))
              return String(HierarchyDelimiterRefusal);
           return String();
        } },
      { "imap_master_user", KindString, ReadWrite, EffectNow, nullptr,
        "The IMAP master user name, or empty for none.",
        ROW_TEXT(Imap()->GetIMAPMasterUser()), ROW_SET(Imap()->SetIMAPMasterUser(v.text)), ROW_NO_CHECK },
      { "imap_sasl_plain_enabled", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether AUTHENTICATE PLAIN is offered.",
        ROW_FLAG(Imap()->GetUseIMAPSASLPlain()), ROW_SET(Imap()->SetUseIMAPSASLPlain(v.flag)), ROW_NO_CHECK },
      { "imap_sasl_initial_response_enabled", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether SASL-IR is offered.",
        ROW_FLAG(Imap()->GetUseIMAPSASLInitialResponse()), ROW_SET(Imap()->SetUseIMAPSASLInitialResponse(v.flag)), ROW_NO_CHECK },
      { "imap_sort_enabled", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether the SORT extension is offered.",
        ROW_FLAG(Imap()->GetUseIMAPSort()), ROW_SET(Imap()->SetUseIMAPSort(v.flag)), ROW_NO_CHECK },
      { "imap_quota_enabled", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether the QUOTA extension is offered.",
        ROW_FLAG(Imap()->GetUseIMAPQuota()), ROW_SET(Imap()->SetUseIMAPQuota(v.flag)), ROW_NO_CHECK },
      { "imap_idle_enabled", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether the IDLE extension is offered.",
        ROW_FLAG(Imap()->GetUseIMAPIdle()), ROW_SET(Imap()->SetUseIMAPIdle(v.flag)), ROW_NO_CHECK },
      { "imap_acl_enabled", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether the ACL extension is offered.",
        ROW_FLAG(Imap()->GetUseIMAPACL()), ROW_SET(Imap()->SetUseIMAPACL(v.flag)), ROW_NO_CHECK },

      // The auto-ban.
      { "auto_ban_on_logon_failure", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether an address is banned after repeated failed logons.",
        ROW_FLAG(Config()->GetAutoBanLogonEnabled()), ROW_SET(Config()->SetAutoBanLogonEnabled(v.flag)), ROW_NO_CHECK },
      { "max_invalid_logon_attempts", KindInteger, ReadWrite, EffectNow, nullptr,
        "The failed logons from one address that trip the ban.",
        ROW_NUMBER(Config()->GetMaxInvalidLogonAttempts()), ROW_SET(Config()->SetMaxInvalidLogonAttempts((int) v.number)), ROW_NO_CHECK },
      { "minutes_before_reset", KindInteger, ReadWrite, EffectNow, nullptr,
        "The minutes a failed logon counts towards the ban.",
        ROW_NUMBER(Config()->GetMaxLogonAttemptsWithin()), ROW_SET(Config()->SetMaxLogonAttemptsWithin((int) v.number)), ROW_NO_CHECK },
      { "minutes_to_ban", KindInteger, ReadWrite, EffectNow, nullptr,
        "How long the ban lasts, in minutes.",
        ROW_NUMBER(Config()->GetAutoBanMinutes()), ROW_SET(Config()->SetAutoBanMinutes((int) v.number)), ROW_NO_CHECK },

      // The quota policy the domains inherit. QuotaWarningPercent itself is an
      // ini key, read where the warning is decided, and stays where it is.
      { "quota_grace_percent", KindInteger, ReadWrite, EffectNow, nullptr,
        "How far past its limit a mailbox still accepts incoming mail, as a percentage of the limit; 0 is not at all. A domain may set its own.",
        ROW_NUMBER(Config()->GetQuotaGracePercent()), ROW_SET(Config()->SetQuotaGracePercent((int) v.number)),
        [] (const Value &v) { return (v.number < 0 || v.number > 1000) ? String(QuotaGraceRefusal) : String(); } },
      { "quota_send_stop_percent", KindInteger, ReadWrite, EffectNow, nullptr,
        "The percentage of its limit at which an account stops sending, before it stops receiving; 0 is never. A domain may set its own.",
        ROW_NUMBER(Config()->GetQuotaSendStopPercent()), ROW_SET(Config()->SetQuotaSendStopPercent((int) v.number)),
        [] (const Value &v) { return (v.number < 0 || v.number > 1000) ? String(QuotaSendStopRefusal) : String(); } },

      // TLS. The listeners build their contexts when they start and the
      // outbound client context is built with the I/O service, so these wait
      // for a restart (SslContextInitializer, TCPServer::Run, IOService).
      { "tls_prefer_server_ciphers", KindBoolean, ReadWrite, EffectRestart, nullptr,
        "Whether the server's cipher order wins over the client's.",
        ROW_FLAG(Config()->GetTlsOptionEnabled(TlsOptionPreferServerCiphers)),
        ROW_SET(Config()->SetTlsOptionEnabled(TlsOptionPreferServerCiphers, v.flag)), ROW_NO_CHECK },
      { "tls_prioritize_chacha", KindBoolean, ReadWrite, EffectRestart, nullptr,
        "Whether ChaCha20-Poly1305 is preferred for a client that puts it first.",
        ROW_FLAG(Config()->GetTlsOptionEnabled(TlsOptionPrioritizeChaCha)),
        ROW_SET(Config()->SetTlsOptionEnabled(TlsOptionPrioritizeChaCha, v.flag)), ROW_NO_CHECK },
      { "ssl_cipher_list", KindString, ReadWrite, EffectRestart, nullptr,
        "The OpenSSL cipher list for TLS 1.2 and below.",
        ROW_TEXT(Config()->GetSslCipherList()), ROW_SET(Config()->SetSslCipherList(v.text)), ROW_NO_CHECK },

      // The reputation watch (ReputationWatch). In hm_settings; what the check
      // finds is at GET /api/v1/reputation. The two strings are refused by the
      // watch's own validators, so a value the Control Panel refuses is refused
      // here in the same words.
      { "reputation_watch_enabled", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether the server checks its own outbound addresses against the DNS block lists once a day and raises reputation.listed when one of them is listed. On in a new installation and off after an upgrade, which may already have a bind address set; with no address to check it looks nothing up.",
        ROW_FLAG(Config()->GetReputationWatchEnabled()), ROW_SET(Config()->SetReputationWatchEnabled(v.flag)), ROW_NO_CHECK },
      { "reputation_watch_lists", KindString, ReadWrite, EffectNow, nullptr,
        "The block lists to ask, one entry each as [ipv4:|ipv6:]zone[=codes], separated by commas or spaces - zen.spamhaus.org, ipv4:bl.spamcop.net. ipv4: or ipv6: says the list covers only that family; =127.0.0.2|127.0.0.4-7 says which answers mean listed.",
        ROW_TEXT(Config()->GetReputationWatchLists()), ROW_SET(Config()->SetReputationWatchLists(v.text)),
        [] (const Value &v) -> String
        {
           return ReputationWatch::ValidateLists(v.text);
        } },
      { "reputation_watch_addresses", KindString, ReadWrite, EffectNow, nullptr,
        "The addresses other servers see this one as, beside the delivery bind address and the ones the global rules bind: the public address in front of a server behind NAT, which nothing on the machine can discover. Separated by commas or spaces.",
        ROW_TEXT(Config()->GetReputationWatchAddresses()), ROW_SET(Config()->SetReputationWatchAddresses(v.text)),
        [] (const Value &v) -> String
        {
           return ReputationWatch::ValidateAddresses(v.text);
        } },

      // The help panes (HelpPages.h): where "Open on the wiki" points, with
      // the document's name as the page. GET /api/v1/help answers it to the
      // two browser pages; the Control Panel reads Settings.HelpWikiBaseUrl.
      { "help_wiki_base_url", KindString, ReadWrite, EffectNow, nullptr,
        "The wiki the help panes' Open on the wiki link points at, with the document's name as the page - https://gitlab.com/Progressiverobot/hmailserver/-/wikis/ as shipped. Absolute, beginning with http:// or https://; empty hides the link.",
        ROW_TEXT(Config()->GetHelpWikiBaseUrl()),
        [] (const Value &v) { String url = v.text; url.Trim(); Config()->SetHelpWikiBaseUrl(url); return Applied(); },
        [] (const Value &v) -> String
        {
           String url = v.text;
           url.Trim();
           if (url.IsEmpty())
              return "";
           String lowered = url;
           lowered.MakeLower();
           if (lowered.Find(_T("http://")) != 0 && lowered.Find(_T("https://")) != 0)
              return _T("The wiki base URL is absolute and begins with http:// or https://, such as https://gitlab.com/Progressiverobot/hmailserver/-/wikis/; empty hides the link.");
           return "";
        } },
      { "tls_version_10_enabled", KindBoolean, ReadWrite, EffectRestart, nullptr,
        "Whether TLS 1.0 is accepted.",
        ROW_FLAG(Config()->GetSslVersionEnabled(TlsVersion10)), ROW_SET(Config()->SetSslVersionEnabled(TlsVersion10, v.flag)), ROW_NO_CHECK },
      { "tls_version_11_enabled", KindBoolean, ReadWrite, EffectRestart, nullptr,
        "Whether TLS 1.1 is accepted.",
        ROW_FLAG(Config()->GetSslVersionEnabled(TlsVersion11)), ROW_SET(Config()->SetSslVersionEnabled(TlsVersion11, v.flag)), ROW_NO_CHECK },
      { "tls_version_12_enabled", KindBoolean, ReadWrite, EffectRestart, nullptr,
        "Whether TLS 1.2 is accepted.",
        ROW_FLAG(Config()->GetSslVersionEnabled(TlsVersion12)), ROW_SET(Config()->SetSslVersionEnabled(TlsVersion12, v.flag)), ROW_NO_CHECK },
      { "tls_version_13_enabled", KindBoolean, ReadWrite, EffectRestart, nullptr,
        "Whether TLS 1.3 is accepted.",
        ROW_FLAG(Config()->GetSslVersionEnabled(TlsVersion13)), ROW_SET(Config()->SetSslVersionEnabled(TlsVersion13, v.flag)), ROW_NO_CHECK },
      { "verify_remote_ssl_certificate", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether the certificate of a server this one connects to is verified.",
        ROW_FLAG(Config()->GetVerifyRemoteSslCertificate()), ROW_SET(Config()->SetVerifyRemoteSslCertificate(v.flag)), ROW_NO_CHECK },

      // Miscellany.
      { "ipv6_preferred", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether an AAAA record is tried before an A record.",
        ROW_FLAG(Config()->GetIPv6Preferred()), ROW_SET(Config()->SetIPv6Preferred(v.flag)), ROW_NO_CHECK },
      { "rewrite_envelope_from_when_forwarding", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether a forwarded message leaves with the forwarding account as its envelope sender.",
        ROW_FLAG(Ini()->GetRewriteEnvelopeFromWhenForwarding()), ROW_SET(Ini()->SetRewriteEnvelopeFromWhenForwarding(v.flag)), ROW_NO_CHECK,
        _T("RewriteEnvelopeFromWhenForwarding") },
      { "create_default_special_use_folders", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether a new account gets Drafts, Sent, Junk and Trash folders.",
        ROW_FLAG(Config()->GetCreateDefaultSpecialUseFolders()), ROW_SET(Config()->SetCreateDefaultSpecialUseFolders(v.flag)), ROW_NO_CHECK },

      // The conversation-logging switches the snapshot has always carried;
      // the same bits as log_smtp, log_imap and log_pop3 in the logging group.
      { "log_smtp_conversations", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether SMTP sessions are written to the log; the same switch as log_smtp in the logging group.",
        ROW_FLAG(Config()->GetLogSMTPConversations()), ROW_SET(Config()->SetLogSMTPConversations(v.flag)), ROW_NO_CHECK },
      { "log_imap_conversations", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether IMAP sessions are written to the log; the same switch as log_imap in the logging group.",
        ROW_FLAG(Config()->GetLogIMAPConversations()), ROW_SET(Config()->SetLogIMAPConversations(v.flag)), ROW_NO_CHECK },
      { "log_pop3_conversations", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether POP3 sessions are written to the log; the same switch as log_pop3 in the logging group.",
        ROW_FLAG(Config()->GetLogPOP3Conversations()), ROW_SET(Config()->SetLogPOP3Conversations(v.flag)), ROW_NO_CHECK },

      // Settings.UserInterfaceLanguage over COM: [Settings] UseLanguage in
      // hMailServer.ini, read from the file on every get and mirrored to the
      // database so a backup carries it. The server translates nothing with
      // it - the classic Administrator did, and was retired in 6.2.10 - so it
      // is for third-party administration tools that read it over COM; the
      // Control Panel's own language is its own setting. Stored, as the
      // desktop writes it, so the Deck can too.
      { "user_interface_language", KindString, ReadWrite, EffectStored, nullptr,
        "The language name third-party administration tools read over COM (Settings.UserInterfaceLanguage, UseLanguage in hMailServer.ini); the server translates nothing with it.",
        ROW_TEXT(Ini()->GetUserInterfaceLanguage()), ROW_SET(Ini()->SetUserInterfaceLanguage(v.text)), ROW_NO_CHECK },

      // The audit trail and the alerts. In hm_settings, reached through
      // Configuration like every other row here - none of them is an
      // hMailServer.ini key, and the rules themselves are rows in
      // hm_alertrules reached at /api/v1/alerts/rules rather than settings.
      { "audit_trail_enabled", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether every administrative change is recorded in the hash-chained audit trail, readable at /api/v1/audit.",
        ROW_FLAG(Config()->GetAuditTrailEnabled()), ROW_SET(Config()->SetAuditTrailEnabled(v.flag)), ROW_NO_CHECK },
      { "audit_retention_days", KindInteger, ReadWrite, EffectNow, nullptr,
        "Days of audit trail to keep; 0 keeps everything, which is the default. Deleting the oldest rows re-anchors the chain at the oldest that remains.",
        ROW_NUMBER(Config()->GetAuditRetentionDays()), ROW_SET(Config()->SetAuditRetentionDays((int) v.number)), ROW_NO_CHECK },
      { "alerts_enabled", KindBoolean, ReadWrite, EffectNow, nullptr,
        "The master switch for alerting. With it on, the rules at /api/v1/alerts/rules decide which conditions are watched.",
        ROW_FLAG(Config()->GetAlertsEnabled()), ROW_SET(Config()->SetAlertsEnabled(v.flag)), ROW_NO_CHECK },
      { "alert_recipient", KindString, ReadWrite, EffectNow, nullptr,
        "The address alerts and the daily digest are sent to. Empty means nothing is mailed, which is the default.",
        ROW_TEXT(Config()->GetAlertRecipient()), ROW_SET(Config()->SetAlertRecipient(v.text)), ROW_NO_CHECK },

      // The session policies (SessionPolicies), server-wide. A domain's own row
      // overrides each of these for the accounts of that domain, at
      // PUT /api/v1/domains/{domain}; the administrator's own browser session
      // belongs to no domain and is governed by these alone. Every one is 0 on a
      // fresh install and after an upgrade, and 0 means the policy is not set -
      // NOT that there is no limit: the ceilings this server has always enforced,
      // thirty minutes idle and twelve hours in all, apply where nothing is set.
      { "session_idle_seconds", KindInteger, ReadWrite, EffectNow, nullptr,
        "How long a signed-in browser session - the webmail, the self-service portal, the Control Deck - may go unused before it ends, in seconds. 0 leaves the server's own ceiling of 1800 seconds in force. At most 2592000 (thirty days). A value under 60 is only useful for testing.",
        ROW_NUMBER(Config()->GetSessionIdleSeconds()), ROW_SET(Config()->SetSessionIdleSeconds((int) v.number)),
        [] (const Value &v) -> String { return SessionPolicies::CheckSeconds((int) v.number); } },
      { "session_lifetime_seconds", KindInteger, ReadWrite, EffectNow, nullptr,
        "How long a signed-in browser session may live at all, however busy it is, in seconds. 0 leaves the server's own ceiling of 43200 seconds - twelve hours - in force. At most 2592000 (thirty days).",
        ROW_NUMBER(Config()->GetSessionLifetimeSeconds()), ROW_SET(Config()->SetSessionLifetimeSeconds((int) v.number)),
        [] (const Value &v) -> String { return SessionPolicies::CheckSeconds((int) v.number); } },
      { "session_one_per_device", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether signing in from a client that already has a session for that account ends the earlier one, so somebody who signs in on a shared machine and forgets leaves nothing behind the next time they sign in there. Off by default.",
        ROW_FLAG(Config()->GetSessionOnePerDevice()), ROW_SET(Config()->SetSessionOnePerDevice(v.flag)), ROW_NO_CHECK },

      // The daily list of what the quarantine holds for an account, and the
      // links in it. Both are refused here exactly as Settings refuses them
      // over COM: a base URL that is not absolute switches the whole digest
      // off silently - the sender checks the same thing - and a link that
      // lives no time at all, or for a month, is not a release link.
      { "quarantine_digest_base_url", KindString, ReadWrite, EffectNow, nullptr,
        "Where the links in a quarantine digest point - the address the webmail answers on, such as https://mail.example.com. Absolute, beginning with http:// or https://; empty sends no digest at all, whatever a domain asks for.",
        ROW_TEXT(Config()->GetQuarantineDigestBaseUrl()),
        [] (const Value &v) { String url = v.text; url.Trim(); Config()->SetQuarantineDigestBaseUrl(url); return Applied(); },
        [] (const Value &v) -> String
        {
           String url = v.text;
           url.Trim();
           if (url.IsEmpty())
              return String();
           String lowered = url;
           lowered.MakeLower();
           if (lowered.Find(_T("http://")) != 0 && lowered.Find(_T("https://")) != 0)
              return String(_T("The base URL is absolute and begins with http:// or https://, such as https://mail.example.com."));
           return String();
        } },
      { "quarantine_digest_link_hours", KindInteger, ReadWrite, EffectNow, nullptr,
        "How long a release link in a quarantine digest works for, in hours. Between 1 and 720; the digest is sent once a day, so anything under 24 expires before the next one.",
        ROW_NUMBER(Config()->GetQuarantineDigestLinkHours()),
        ROW_SET(Config()->SetQuarantineDigestLinkHours((int) v.number)),
        [] (const Value &v) -> String
        {
           if (v.number < 1 || v.number > 720)
              return String(_T("A release link lives between 1 and 720 hours."));
           return String();
        } },

      // Brand logos (Util/BrandLogo.h): the logo a sender's domain publishes,
      // beside mail this server judged dmarc=pass.
      { "brand_logos_enabled", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether the webmail shows the logo a sender's domain publishes (BIMI) as the avatar of its mail. Only for mail this server judged dmarc=pass and recorded so in its Authentication-Results header (AuthenticationResultsEnabled), from a domain whose DMARC policy is quarantine or reject for all of its mail, and only an SVG Tiny PS logo fetched over https from a public address. On by default; one lookup a day per domain.",
        ROW_FLAG(Config()->GetBrandLogosEnabled()), ROW_SET(Config()->SetBrandLogosEnabled(v.flag)), ROW_NO_CHECK },

      // The other accounts a reader adds in the webmail (RestApiFetchAccounts.cpp).
      { "reader_fetch_accounts_max", KindInteger, ReadWrite, EffectNow, nullptr,
        "How many other POP3 or IMAP accounts a reader may add in the webmail to collect into their own mailbox. 0 offers none; at most 20. An administrator's fetch accounts are not counted and not limited.",
        ROW_NUMBER(Config()->GetReaderFetchAccountsMax()), ROW_SET(Config()->SetReaderFetchAccountsMax((int) v.number)),
        [] (const Value &v) -> String
        {
           if (v.number < 0 || v.number > 20)
              return String(_T("reader_fetch_accounts_max is between 0 and 20."));
           return String();
        } },
      { "reader_fetch_accounts_private_addresses", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether a reader's other account may be on a host that resolves to a private, loopback or link-local address. Off by default: the server makes the connection from inside its own network, on the reader's word. Turn it on to let people collect from an old mail server inside the network.",
        ROW_FLAG(Config()->GetReaderFetchAccountsPrivateAddresses()), ROW_SET(Config()->SetReaderFetchAccountsPrivateAddresses(v.flag)), ROW_NO_CHECK },

      // The recovery address and the recovery codes (Util/AccountRecovery.h).
      { "account_recovery_enabled", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether a person may set a recovery address and one-time recovery codes in the webmail, and reset a forgotten password with them. On by default, and inert until somebody uses it: nothing happens for an account that has set no address, and a link can only be sent when the server has a public address of its own (confidential_base_url, or the quarantine digest's, or the import sign-in's). Off refuses the Security page's recovery section, the forgotten-password route and the reset link; an address already proved stays in the database and is used for nothing.",
        ROW_FLAG(Config()->GetAccountRecoveryEnabled()), ROW_SET(Config()->SetAccountRecoveryEnabled(v.flag)), ROW_NO_CHECK },
      { "recovery_reset_link_minutes", KindInteger, ReadWrite, EffectNow, nullptr,
        "How many minutes a password-reset link sent to a recovery address works for. 30 by default; between 5 and 1440. The link is good once: a password the policy, the reuse history or the breach check refuses leaves it alive so the person can choose again, and it is spent the moment a password is accepted.",
        ROW_NUMBER(Config()->GetRecoveryResetLinkMinutes()), ROW_SET(Config()->SetRecoveryResetLinkMinutes((int) v.number)),
        [] (const Value &v) -> String
        {
           if (v.number < 5 || v.number > 1440)
              return String(_T("recovery_reset_link_minutes is between 5 and 1440."));
           return String();
        } },
      { "recovery_verify_link_hours", KindInteger, ReadWrite, EffectNow, nullptr,
        "How many hours the link that proves a recovery address works for. 24 by default; between 1 and 168. A recovery address counts for nothing until that link has been followed, and changing the address asks for it again.",
        ROW_NUMBER(Config()->GetRecoveryVerifyLinkHours()), ROW_SET(Config()->SetRecoveryVerifyLinkHours((int) v.number)),
        [] (const Value &v) -> String
        {
           if (v.number < 1 || v.number > 168)
              return String(_T("recovery_verify_link_hours is between 1 and 168."));
           return String();
        } },

      // Deleted items kept for recovery (Util/RecoverableStore.h).
      { "recover_deleted_days", KindInteger, ReadWrite, EffectNow, nullptr,
        "How many days a message a person deletes - by IMAP EXPUNGE or CLOSE, a POP3 DELE, the webmail's delete or emptying of a folder, or their own retention choices - is kept aside, listed on the webmail's Recover deleted items page and put back from there. 0, the default, keeps nothing. At most 365. The copies are under the data directory's Recoverable folder and count against no quota; the retention task ends them when the window passes.",
        ROW_NUMBER(Config()->GetRecoverDeletedDays()), ROW_SET(Config()->SetRecoverDeletedDays((int) v.number)),
        [] (const Value &v) -> String
        {
           if (v.number < 0 || v.number > 365)
              return String(_T("recover_deleted_days is between 0 and 365."));
           return String();
        } },

      // The message store's form (Persistence/MessageStore.h, docs/MessageStore.md).
      { "message_store_compression", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether a message placed in a mailbox - delivered, appended over IMAP, collected by the IMAP mirror or brought in by a migration or an import - is stored compressed (deflate), each MIME part of at least message_store_dedup_min_kb once decoded held once as a blob that every message carrying the same bytes shares. A message stored before is read as it is, whichever form it has; hmailserver --compress-store rewrites the ones already there, and --decompress puts them back. IMAP, POP3, the sizes and DKIM see the original message. Off by default. GET /api/v1/store says what is saved.",
        ROW_FLAG(Config()->GetMessageStoreCompression()), ROW_SET(Config()->SetMessageStoreCompression(v.flag)), ROW_NO_CHECK },
      { "message_store_dedup_min_kb", KindInteger, ReadWrite, EffectNow, nullptr,
        "The smallest MIME part, in KB once decoded, held once as a blob while message_store_compression is on; 0 holds nothing once. 0 to 65536; 256 by default.",
        ROW_NUMBER(Config()->GetMessageStoreDedupMinKB()), ROW_SET(Config()->SetMessageStoreDedupMinKB((int) v.number)),
        [] (const Value &v) { return (v.number < 0 || v.number > 65536) ? String(_T("message_store_dedup_min_kb must be between 0 and 65536.")) : String(); } },
      // Mailbox encryption at rest (Util/MailboxKeys.h, docs/EncryptionAtRest.md).
      { "mailbox_key_idle_minutes", KindInteger, ReadWrite, EffectNow, nullptr,
        "How many minutes the mailbox key of an account encrypted at rest stays open after its last use once no IMAP or POP3 session holds it. A sign-in with the account's password opens it; while it is open the server reads the account's mail; once it closes, the mail is readable again only after the next sign-in with the password. 1 to 1440; 15 by default.",
        ROW_NUMBER(Config()->GetMailboxKeyIdleMinutes()), ROW_SET(Config()->SetMailboxKeyIdleMinutes((int) v.number)),
        [] (const Value &v) { return (v.number < 1 || v.number > 1440) ? String(_T("mailbox_key_idle_minutes must be between 1 and 1440.")) : String(); } },

      // Message recall within the server (Util/MessageRecall.h).
      { "recall_days", KindInteger, ReadWrite, EffectNow, nullptr,
        "How many days after sending a message to other accounts on this server its sender may still recall it from the webmail - the unread copies deleted, the read ones left - and how long the rows that make that possible are kept. A recall of an older message answers 404 as if it had never been recorded. 1 to 365; 14 by default.",
        ROW_NUMBER(Config()->GetRecallDays()), ROW_SET(Config()->SetRecallDays((int) v.number)),
        [] (const Value &v) { return (v.number < 1 || v.number > 365) ? String(_T("recall_days must be between 1 and 365.")) : String(); } },

      // The list manager (Util/ListManager.h, docs/MailingLists.md).
      { "list_base_url", KindString, ReadWrite, EffectNow, nullptr,
        "Where the links in mail from a managed list point - the address the REST listener (RestApiPort) answers on from outside, such as https://lists.example.com; the pages are /lists/confirm, /lists/unsubscribe, /lists/approve, /lists/{address}/subscribe and /lists/{address}/archive under it. Absolute, beginning with http:// or https://, in printable ASCII - a host name that is not ASCII in its punycode form. Only an https base gives the copies RFC 8058's one-click (List-Unsubscribe-Post), which that RFC requires to be HTTPS; an http base gives the links alone. Empty, the default, offers no link at all: a managed list's copies carry the mailto addresses alone, and the confirmation asks for a reply.",
        ROW_TEXT(Config()->GetListBaseUrl()),
        [] (const Value &v) { String url = v.text; url.Trim(); Config()->SetListBaseUrl(url); return Applied(); },
        [] (const Value &v) -> String
        {
           // What the list manager would use, and nothing it would quietly
           // drop: a value refused here is never stored to be ignored.
           String reason;
           if (!ListManager::IsUsableBaseUrl(v.text, reason))
              return reason;
           return String();
        } },
      { "list_hard_bounces_to_unsubscribe", KindInteger, ReadWrite, EffectNow, nullptr,
        "How many hard bounces (a 5.x.x status) stop a member of a managed list being sent its mail. 1 by default; between 1 and 100. The member stays a member, the reason is recorded and the list's owner is told once; the owner's reset, or the member turning delivery back on, starts the count again.",
        ROW_NUMBER(Config()->GetListHardBouncesToUnsubscribe()), ROW_SET(Config()->SetListHardBouncesToUnsubscribe((int) v.number)),
        [] (const Value &v) { return (v.number < 1 || v.number > 100) ? String(_T("list_hard_bounces_to_unsubscribe must be between 1 and 100.")) : String(); } },
      { "list_soft_bounces_to_unsubscribe", KindInteger, ReadWrite, EffectNow, nullptr,
        "How many soft bounces (a 4.x.x status, or a report whose status cannot be read) within list_bounce_window_days stop a member of a managed list being sent its mail. 5 by default; between 1 and 100. A delivery that succeeds does not clear the count; the window does.",
        ROW_NUMBER(Config()->GetListSoftBouncesToUnsubscribe()), ROW_SET(Config()->SetListSoftBouncesToUnsubscribe((int) v.number)),
        [] (const Value &v) { return (v.number < 1 || v.number > 100) ? String(_T("list_soft_bounces_to_unsubscribe must be between 1 and 100.")) : String(); } },
      { "list_bounce_window_days", KindInteger, ReadWrite, EffectNow, nullptr,
        "The days a managed list's soft bounces are counted within: a soft bounce more than this many days after the first of the current count starts a new count. 30 by default; between 1 and 365.",
        ROW_NUMBER(Config()->GetListBounceWindowDays()), ROW_SET(Config()->SetListBounceWindowDays((int) v.number)),
        [] (const Value &v) { return (v.number < 1 || v.number > 365) ? String(_T("list_bounce_window_days must be between 1 and 365.")) : String(); } },

      // The organisation CA for S/MIME (Util/OrganisationCa.h, docs/SmimeAuthority.md).
      { "smime_authority_base_url", KindString, ReadWrite, EffectNow, nullptr,
        "Where the clients reach the web services listener (WebServicesHttpPort), such as http://pki.example.com: every certificate a domain's S/MIME authority issues from now on names its CRL (/smime-ca/{domain}/{root}.crl) and its root (/smime-ca/{domain}/{root}.cer) under it, which is how Outlook and Thunderbird find out that a certificate was revoked. Absolute, beginning with http:// (RFC 5280 recommends plain HTTP for a distribution point) or https://, in printable ASCII with no space, comma, quote, query or fragment, at most 200 characters. Empty, the default, writes neither into a certificate: a client that insists on checking revocation then cannot, and the webmail's own check is unaffected. A certificate already issued keeps what it was issued with.",
        ROW_TEXT(Config()->GetSmimeAuthorityBaseUrl()),
        [] (const Value &v) { String url = v.text; url.Trim(); Config()->SetSmimeAuthorityBaseUrl(url); return Applied(); },
        [] (const Value &v) -> String
        {
           String reason;
           if (!OrganisationCa::IsUsableBaseUrl(v.text, reason))
              return reason;
           return String();
        } },

      // The organisation's directory (Util/DirectoryStore.h).
      { "directory_enabled", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether every active account and enabled distribution list of a domain is answered to the people of that domain - in the webmail's Directory page, in the To, Cc and Bcc completion beside the reader's own contacts, and as a read-only Directory address book over CardDAV. On by default, as Outlook's Global Address List and Google's directory are. It never crosses a domain: a reader is answered their own domain and no other, whatever else the server hosts. Turn it off on a server whose one domain holds people who are not one organisation, where a listing of every address would be an address list for whoever signs in.",
        ROW_FLAG(Config()->GetDirectoryEnabled()), ROW_SET(Config()->SetDirectoryEnabled(v.flag)), ROW_NO_CHECK },
      { "directory_user_editable", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether a person may set their own directory title, department, telephone numbers and photo from the webmail's Settings page. On by default. With it off those are the administrator's alone, set in the Control Panel's Directory tab, the Control Deck's account view or PUT /api/v1/accounts/{address}/directory - and the person can still take themselves out of the directory, because the opt-out is theirs and this switch never reaches it.",
        ROW_FLAG(Config()->GetDirectoryUserEditable()), ROW_SET(Config()->SetDirectoryUserEditable(v.flag)), ROW_NO_CHECK },

      // Offline in the webmail (Util/RestApiOffline.cpp).
      { "reader_offline_days_max", KindInteger, ReadWrite, EffectNow, nullptr,
        "The most days of mail a reader may keep in their browser for reading and searching without a connection: the bodies and attachments of the messages received within that many days, in the browser's own storage, chosen by the reader under this ceiling on the webmail's Settings page and kept nowhere until they choose. 30 by default; 0 offers none, and the webmail then keeps only the inbox listing and the last opened messages as it always has. At most 365. What is kept lives in the browser it was kept in and is removed at sign-out.",
        ROW_NUMBER(Config()->GetReaderOfflineDaysMax()), ROW_SET(Config()->SetReaderOfflineDaysMax((int) v.number)),
        [] (const Value &v) -> String
        {
           if (v.number < 0 || v.number > 365)
              return String(_T("reader_offline_days_max is between 0 and 365."));
           return String();
        } },
      // The sample domain (Application/SampleDomain.h): the marker, shown and
      // never sent. It is written by POST and DELETE /api/v1/setup/sample-domain
      // and by nothing else, which is what makes the removal exact.
      { "sample_domain_id", KindInteger, ReadOnly, EffectNow, nullptr,
        "The database id of the example.test the sample domain made (POST /api/v1/setup/sample-domain), or 0 when it made none. The marker the removal matches against, so that an example.test somebody made by hand is never the one DELETE removes, and the first-hour checklist leaves the sample out of its count. Shown and never sent; a stale id - the domain deleted by hand - is the same as 0.",
        ROW_NUMBER(Config()->GetSampleDomainId()), nullptr, ROW_NO_CHECK },
      // Compromised-account detection (Util/CompromiseDetector.h): the mode
      // and the thresholds of the four signals. Every range is refused in the
      // setting's own sentence rather than clamped, as the webhook settings are.
      { "compromise_detection_mode", KindEnum, ReadWrite, EffectNow, CompromiseModeWords,
        "What a submission in the shape of an account takeover does. off records nothing and judges nothing. watch, the default, records each account's baseline and raises the account.compromised alert saying what enforcing would have done, and does nothing else. enforce keeps the message out of the delivery queue for review, holds the account's other messages queued within the hour, locks the account until an administrator unlocks it, ends its browser sessions, and raises the alert and the account.locked webhook. What is held is at GET /api/v1/review. This key and every other compromise_* key answer to the administrator password or an account holding manage_settings, never to an api key, which is refused them with a 403: a stolen key must not switch the detection off, or put its floors out of reach, for the account it stole.",
        ROW_NUMBER(Config()->GetCompromiseDetectionMode()), ROW_SET(Config()->SetCompromiseDetectionMode((int) v.number)), ROW_NO_CHECK },
      { "compromise_baseline_days", KindInteger, ReadWrite, EffectNow, nullptr,
        "How many days of an account's submissions make its baseline, and how long the record of them and of whom it wrote to is kept. 7 to 365; 30 by default.",
        ROW_NUMBER(Config()->GetCompromiseBaselineDays()), ROW_SET(Config()->SetCompromiseBaselineDays((int) v.number)),
        [] (const Value &v) -> String { if (v.number < 7 || v.number > 365) return String(_T("compromise_baseline_days is between 7 and 365.")); return String(); } },
      { "compromise_minimum_history", KindInteger, ReadWrite, EffectNow, nullptr,
        "How many submissions in the baseline an account needs before its busiest hour, its hours of the day and its client networks count as a baseline. Below it only the two floors are judged. 1 to 10000; 20 by default.",
        ROW_NUMBER(Config()->GetCompromiseMinimumHistory()), ROW_SET(Config()->SetCompromiseMinimumHistory((int) v.number)),
        [] (const Value &v) -> String { if (v.number < 1 || v.number > 10000) return String(_T("compromise_minimum_history is between 1 and 10000.")); return String(); } },
      { "compromise_volume_factor", KindInteger, ReadWrite, EffectNow, nullptr,
        "The volume signal: the messages in the last hour exceed this many times the account's busiest hour in the baseline, and reach compromise_volume_floor. 2 to 100; 4 by default.",
        ROW_NUMBER(Config()->GetCompromiseVolumeFactor()), ROW_SET(Config()->SetCompromiseVolumeFactor((int) v.number)),
        [] (const Value &v) -> String { if (v.number < 2 || v.number > 100) return String(_T("compromise_volume_factor is between 2 and 100.")); return String(); } },
      { "compromise_volume_floor", KindInteger, ReadWrite, EffectNow, nullptr,
        "The fewest messages in the last hour that can be a volume signal, whatever the baseline says; for an account with no baseline the floor is the whole test. 1 to 100000; 30 by default.",
        ROW_NUMBER(Config()->GetCompromiseVolumeFloor()), ROW_SET(Config()->SetCompromiseVolumeFloor((int) v.number)),
        [] (const Value &v) -> String { if (v.number < 1 || v.number > 100000) return String(_T("compromise_volume_floor is between 1 and 100000.")); return String(); } },
      { "compromise_recipient_floor", KindInteger, ReadWrite, EffectNow, nullptr,
        "The fewest recipients in the last hour that can be a recipients signal. 1 to 100000; 20 by default.",
        ROW_NUMBER(Config()->GetCompromiseRecipientFloor()), ROW_SET(Config()->SetCompromiseRecipientFloor((int) v.number)),
        [] (const Value &v) -> String { if (v.number < 1 || v.number > 100000) return String(_T("compromise_recipient_floor is between 1 and 100000.")); return String(); } },
      { "compromise_new_recipient_share", KindInteger, ReadWrite, EffectNow, nullptr,
        "The recipients signal: this percentage or more of the recipients in the last hour are addresses the account has never written to. 1 to 100; 80 by default.",
        ROW_NUMBER(Config()->GetCompromiseNewRecipientShare()), ROW_SET(Config()->SetCompromiseNewRecipientShare((int) v.number)),
        [] (const Value &v) -> String { if (v.number < 1 || v.number > 100) return String(_T("compromise_new_recipient_share is between 1 and 100.")); return String(); } },
      { "compromise_score_to_lock", KindInteger, ReadWrite, EffectNow, nullptr,
        "The score that is a takeover: volume and recipients score 2 each, an hour of the day the account has never sent at and a client network it has never sent from score 1 each. An account whose sensitivity is relaxed needs one more, strict one fewer. 1 to 6; 3 by default.",
        ROW_NUMBER(Config()->GetCompromiseScoreToLock()), ROW_SET(Config()->SetCompromiseScoreToLock((int) v.number)),
        [] (const Value &v) -> String { if (v.number < 1 || v.number > 6) return String(_T("compromise_score_to_lock is between 1 and 6.")); return String(); } },
      { "compromise_held_days", KindInteger, ReadWrite, EffectNow, nullptr,
        "How many days a message held for review waits for somebody to release or discard it before the retention sweep discards it. 1 to 90; 7 by default.",
        ROW_NUMBER(Config()->GetCompromiseHeldDays()), ROW_SET(Config()->SetCompromiseHeldDays((int) v.number)),
        [] (const Value &v) -> String { if (v.number < 1 || v.number > 90) return String(_T("compromise_held_days is between 1 and 90.")); return String(); } },

      // Messages that expire (Util/ConfidentialMessages.h): where the link in
      // the notice points. The same shape as the digest's base URL, and the
      // digest's or the import sign-in's serves when this one is empty.
      { "confidential_base_url", KindString, ReadWrite, EffectNow, nullptr,
        "Where the link in the notice of a message that expires points - the address the webmail answers on from outside, such as https://mail.example.com; the reading page is /confidential/<token> under it. Absolute, beginning with http:// or https://. Empty uses quarantine_digest_base_url, then import_oauth_redirect_base_url; with none of the three set, the webmail does not offer an expiry and the route refuses one.",
        ROW_TEXT(Config()->GetConfidentialBaseUrl()),
        [] (const Value &v) { String url = v.text; url.Trim(); Config()->SetConfidentialBaseUrl(url); return Applied(); },
        [] (const Value &v) -> String
        {
           String url = v.text;
           url.Trim();
           if (url.IsEmpty())
              return String();
           String lowered = url;
           lowered.MakeLower();
           if (lowered.Find(_T("http://")) != 0 && lowered.Find(_T("https://")) != 0)
              return String(_T("The base URL is absolute and begins with http:// or https://, such as https://mail.example.com."));
           return String();
        } },

      // Outbound content policy (Util/ContentPolicy.h): the one detector that
      // needs to be told what to look for. Refused unless it compiles, so the
      // detector cannot be switched off in silence by a pattern that does not.
      { "dlp_account_number_pattern", KindString, ReadWrite, EffectNow, nullptr,
        "The regular expression the account_number detector of the sensitive_content rule condition searches a message's readable text for - the plain and HTML parts and every attachment whose bytes are text - such as an account-number format of this organisation. Perl syntax, searched anywhere in the text, at most 2000 characters. Empty, the default, is a detector that never fires; the card_number and iban detectors need no pattern. Refused unless it compiles.",
        ROW_TEXT(Config()->GetDlpAccountNumberPattern()),
        [] (const Value &v) { String pattern = v.text; pattern.Trim(); Config()->SetDlpAccountNumberPattern(pattern); return Applied(); },
        [] (const Value &v) -> String
        {
           String pattern = v.text;
           pattern.Trim();
           if (pattern.IsEmpty())
              return String();
           if (pattern.GetLength() > 2000)
              return String(_T("dlp_account_number_pattern is at most 2000 characters."));
           if (!ContentPolicy::PatternCompiles(pattern))
              return String(_T("dlp_account_number_pattern is not a regular expression this server can use; it has not been saved."));
           return String();
        } },

      // The reports our own domains receive (Util/DomainReports.h): how long a
      // DMARC aggregate or TLS-RPT report read from a domain's report mailbox is
      // kept. Which mailbox is the domain's own field, report_mailbox.
      { "domain_report_retention_days", KindInteger, ReadWrite, EffectNow, nullptr,
        "How many days a DMARC aggregate report or a TLS-RPT report read from a domain's report mailbox is kept, by the day it was read; the retention task removes older ones, and POST /api/v1/domain-reports/expire runs that now. 0 keeps them for ever; at most 3650. 365 by default, which is what the paid services keep. Which address the reports are read from is each domain's report_mailbox.",
        ROW_NUMBER(Config()->GetDomainReportRetentionDays()), ROW_SET(Config()->SetDomainReportRetentionDays((int) v.number)),
        [] (const Value &v) -> String
        {
           if (v.number < 0 || v.number > 3650)
              return String(_T("domain_report_retention_days is between 0 and 3650."));
           return String();
        } },

      // Masked addresses a reader makes (Util/MaskedAddresses.h).
      { "reader_masked_addresses_max", KindInteger, ReadWrite, EffectNow, nullptr,
        "How many masked addresses a reader may make in the webmail: an address in their own domain for one sign-up, delivering to their mailbox and switched off when it starts to receive spam. 0 offers none; at most 200. An account, alias or list an administrator makes later with the same address takes it over.",
        ROW_NUMBER(Config()->GetReaderMaskedAddressesMax()), ROW_SET(Config()->SetReaderMaskedAddressesMax((int) v.number)),
        [] (const Value &v) -> String
        {
           if (v.number < 0 || v.number > 200)
              return String(_T("reader_masked_addresses_max is between 0 and 200."));
           return String();
        } },

      // Addresses elsewhere a reader proves with a code and sends as (Util/SenderAddresses.h).
      { "reader_sender_addresses_max", KindInteger, ReadWrite, EffectNow, nullptr,
        "How many addresses at other providers a reader may add in the webmail and send as, once each has been proved with a code mailed to it. 0 offers none; at most 20. Mail sent as such an address is not aligned with its domain, so a domain whose DMARC policy is quarantine or reject will see it refused; the page says so for each address.",
        ROW_NUMBER(Config()->GetReaderSenderAddressesMax()), ROW_SET(Config()->SetReaderSenderAddressesMax((int) v.number)),
        [] (const Value &v) -> String
        {
           if (v.number < 0 || v.number > 20)
              return String(_T("reader_sender_addresses_max is between 0 and 20."));
           return String();
        } },

      // Importing from the old provider with a sign-in (Util/ReaderOAuth.h). A
      // provider is offered once it has a client id (and, for Google, a secret)
      // and the base URL is set. The secrets are write-only, as the relayer's
      // password is.
      { "import_oauth_redirect_base_url", KindString, ReadWrite, EffectNow, nullptr,
        "The address the webmail is reached at from outside, such as https://mail.example.com: Google and Microsoft send a reader back to it plus /portal/oauth/callback, and that whole address is the redirect URI to register with each. Empty offers no sign-in import at all.",
        ROW_TEXT(Config()->GetImportOAuthRedirectBaseUrl()), ROW_SET(Config()->SetImportOAuthRedirectBaseUrl(v.text)),
        [] (const Value &v) -> String { return ImportUrlProblem(v.text, true); } },
      { "import_oauth_google_client_id", KindString, ReadWrite, EffectNow, nullptr,
        "The OAuth client id this server is registered under in the Google Cloud console (a Web application client, with the Gmail API's https://mail.google.com/ scope). Empty offers no Gmail sign-in.",
        ROW_TEXT(Config()->GetImportOAuthString(PROPERTY_IMPORT_OAUTH_GOOGLE_CLIENT_ID)), ROW_SET(Config()->SetImportOAuthString(PROPERTY_IMPORT_OAUTH_GOOGLE_CLIENT_ID, v.text)), ROW_NO_CHECK },
      { "import_oauth_google_client_secret", KindString, WriteOnly, EffectNow, nullptr,
        "That client's secret. Stored protected and never emitted; Google requires it for a Web application client.",
        nullptr, ROW_SET(Config()->SetImportOAuthString(PROPERTY_IMPORT_OAUTH_GOOGLE_CLIENT_SECRET, v.text)), ROW_NO_CHECK,
        nullptr, PROPERTY_IMPORT_OAUTH_GOOGLE_CLIENT_SECRET },
      { "import_oauth_google_authorize_url", KindString, ReadWrite, EffectNow, nullptr,
        "Google's authorization endpoint. The public one by default.",
        ROW_TEXT(Config()->GetImportOAuthString(PROPERTY_IMPORT_OAUTH_GOOGLE_AUTHORIZE_URL)), ROW_SET(Config()->SetImportOAuthString(PROPERTY_IMPORT_OAUTH_GOOGLE_AUTHORIZE_URL, v.text)),
        [] (const Value &v) -> String { return ImportUrlProblem(v.text, false); } },
      { "import_oauth_google_token_url", KindString, ReadWrite, EffectNow, nullptr,
        "Google's token endpoint. The public one by default.",
        ROW_TEXT(Config()->GetImportOAuthString(PROPERTY_IMPORT_OAUTH_GOOGLE_TOKEN_URL)), ROW_SET(Config()->SetImportOAuthString(PROPERTY_IMPORT_OAUTH_GOOGLE_TOKEN_URL, v.text)),
        [] (const Value &v) -> String { return ImportUrlProblem(v.text, false); } },
      { "import_oauth_microsoft_client_id", KindString, ReadWrite, EffectNow, nullptr,
        "The application (client) id this server is registered under in Microsoft Entra, with the delegated IMAP.AccessAsUser.All and offline_access permissions and personal Microsoft accounts allowed for Outlook.com. Empty offers no Outlook.com sign-in.",
        ROW_TEXT(Config()->GetImportOAuthString(PROPERTY_IMPORT_OAUTH_MICROSOFT_CLIENT_ID)), ROW_SET(Config()->SetImportOAuthString(PROPERTY_IMPORT_OAUTH_MICROSOFT_CLIENT_ID, v.text)), ROW_NO_CHECK },
      { "import_oauth_microsoft_client_secret", KindString, WriteOnly, EffectNow, nullptr,
        "That application's client secret, for a Web platform registration. Stored protected and never emitted; empty for a registration that needs none.",
        nullptr, ROW_SET(Config()->SetImportOAuthString(PROPERTY_IMPORT_OAUTH_MICROSOFT_CLIENT_SECRET, v.text)), ROW_NO_CHECK,
        nullptr, PROPERTY_IMPORT_OAUTH_MICROSOFT_CLIENT_SECRET },
      { "import_oauth_microsoft_authorize_url", KindString, ReadWrite, EffectNow, nullptr,
        "Microsoft's authorization endpoint: the common one by default, a tenant's own or a national cloud's otherwise.",
        ROW_TEXT(Config()->GetImportOAuthString(PROPERTY_IMPORT_OAUTH_MICROSOFT_AUTHORIZE_URL)), ROW_SET(Config()->SetImportOAuthString(PROPERTY_IMPORT_OAUTH_MICROSOFT_AUTHORIZE_URL, v.text)),
        [] (const Value &v) -> String { return ImportUrlProblem(v.text, false); } },
      { "import_oauth_microsoft_token_url", KindString, ReadWrite, EffectNow, nullptr,
        "Microsoft's token endpoint, matching the authorization endpoint.",
        ROW_TEXT(Config()->GetImportOAuthString(PROPERTY_IMPORT_OAUTH_MICROSOFT_TOKEN_URL)), ROW_SET(Config()->SetImportOAuthString(PROPERTY_IMPORT_OAUTH_MICROSOFT_TOKEN_URL, v.text)),
        [] (const Value &v) -> String { return ImportUrlProblem(v.text, false); } },

      // Passkeys and single sign-on (Util/WebAuthn.h, Util/PasskeyStore.h,
      // Util/OidcSignIn.h). Passkeys are offered and nothing happens until a
      // person enrols one; single sign-on is off until an issuer is named. The
      // client secret is write-only, as the import ones are.
      { "passkeys_enabled", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether people may enrol a passkey (WebAuthn) and sign in with it, in the webmail and in the Control Deck. On changes nothing on its own: a person signs in with their password until they add one. Off hides the button and refuses every passkey sign-in, and the passkeys already enrolled stay in the database until they are removed - which their owners may still do while it is off, since taking a way in away must not depend on the feature being on.",
        ROW_FLAG(Config()->GetPasskeysEnabled()), ROW_SET(Config()->SetPasskeysEnabled(v.flag)), ROW_NO_CHECK },
      { "passkey_relying_party", KindString, ReadWrite, EffectNow, nullptr,
        "The WebAuthn relying party id a passkey is bound to, such as example.com. Empty, the default, means the host the page was served on, which is right for a server reached at one name. Set it to a registrable parent domain when the webmail and the Control Deck are reached at several names, so that one passkey works at all of them; a value that is not the page's host or a parent of it is refused by the browser, and the page says so.",
        ROW_TEXT(Config()->GetSignInString(PROPERTY_PASSKEY_RELYING_PARTY)), ROW_SET(Config()->SetSignInString(PROPERTY_PASSKEY_RELYING_PARTY, v.text)),
        [] (const Value &v) -> String
        {
           if (v.text.Find(_T("/")) >= 0 || v.text.Find(_T(":")) >= 0)
              return String(_T("passkey_relying_party is a host name, not a URL."));
           return String();
        } },
      { "oidc_issuer", KindString, ReadWrite, EffectNow, nullptr,
        "The OpenID Connect issuer this server signs people in at, such as https://login.example.com. Its discovery document is read from <issuer>/.well-known/openid-configuration and cached. Empty, the default, offers no single sign-on at all.",
        ROW_TEXT(Config()->GetSignInString(PROPERTY_OIDC_ISSUER)), ROW_SET(Config()->SetSignInString(PROPERTY_OIDC_ISSUER, v.text)),
        [] (const Value &v) -> String { return ImportUrlProblem(v.text, true); } },
      { "oidc_client_id", KindString, ReadWrite, EffectNow, nullptr,
        "The client id this server is registered under at the issuer, for an authorization-code client with PKCE. Empty offers no single sign-on.",
        ROW_TEXT(Config()->GetSignInString(PROPERTY_OIDC_CLIENT_ID)), ROW_SET(Config()->SetSignInString(PROPERTY_OIDC_CLIENT_ID, v.text)), ROW_NO_CHECK },
      { "oidc_client_secret", KindString, WriteOnly, EffectNow, nullptr,
        "That client's secret. Stored protected and never emitted; empty for a public client, which PKCE makes safe.",
        nullptr, ROW_SET(Config()->SetSignInString(PROPERTY_OIDC_CLIENT_SECRET, v.text)), ROW_NO_CHECK, nullptr, PROPERTY_OIDC_CLIENT_SECRET },
      { "oidc_scopes", KindString, ReadWrite, EffectNow, nullptr,
        "The scopes asked for, space separated. 'openid email' by default, which is what the mapping needs: the id_token's verified email names the local account.",
        ROW_TEXT(Config()->GetSignInString(PROPERTY_OIDC_SCOPES)), ROW_SET(Config()->SetSignInString(PROPERTY_OIDC_SCOPES, v.text)), ROW_NO_CHECK },
      { "oidc_redirect_base_url", KindString, ReadWrite, EffectNow, nullptr,
        "The address the webmail is reached at from outside, such as https://mail.example.com: the issuer sends a person back to it plus /portal/oidc/callback, and that whole address is the redirect URI to register at the issuer. It is also the base of SAML sign-in's ACS URL (plus /portal/saml/acs), its default entity ID (plus /saml) and its metadata (plus /saml/metadata). Empty falls back to import_oauth_redirect_base_url.",
        ROW_TEXT(Config()->GetSignInString(PROPERTY_OIDC_REDIRECT_BASE_URL)), ROW_SET(Config()->SetSignInString(PROPERTY_OIDC_REDIRECT_BASE_URL, v.text)),
        [] (const Value &v) -> String { return ImportUrlProblem(v.text, true); } },
      { "oidc_auto_provision", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether an address the issuer verified that has no mailbox here gets one made for it at its first sign-in. Off, the default: an address this server does not know is refused, which keeps a directory of thousands from becoming thousands of mailboxes. The account made when this is on has no password of its own and is at a domain this server already has.",
        ROW_FLAG(Config()->GetOidcAutoProvision()), ROW_SET(Config()->SetOidcAutoProvision(v.flag)), ROW_NO_CHECK },
      { "oidc_administrators", KindString, ReadWrite, EffectNow, nullptr,
        "The addresses that sign in to the Control Deck as the administrator when the issuer verifies them, comma separated. Empty, the default, means single sign-on reaches the webmail only and the Deck still asks for the administrator password.",
        ROW_TEXT(Config()->GetSignInString(PROPERTY_OIDC_ADMINISTRATORS)), ROW_SET(Config()->SetSignInString(PROPERTY_OIDC_ADMINISTRATORS, v.text)), ROW_NO_CHECK },

      // SAML sign-in, beside OIDC (Util/SamlSignIn.h): off until the identity
      // provider's entity ID, its sign-in URL and its certificate are set, and
      // reached at OIDC's base URL. The certificate is write-only as the
      // client secret is - not because it is secret but so that a paste is
      // never answered back - and its fingerprints are what is read; its name
      // is withheld by AuditTrail::IsSecretName as well, so that the list of
      // what is changed from stock and the audit trail do not answer it back
      // either.
      { "saml_idp_entity_id", KindString, ReadWrite, EffectNow, nullptr,
        "The SAML identity provider's entity ID, exactly as its metadata writes it: the Issuer every answer must carry. Entra ID shows it as the Microsoft Entra Identifier (https://sts.windows.net/ and the tenant id), Okta as the Identity Provider Issuer, ADFS as its Federation Service Identifier. Empty, the default, offers no SAML sign-in at all.",
        ROW_TEXT(Config()->GetSignInString(PROPERTY_SAML_IDP_ENTITY_ID)), ROW_SET(Config()->SetSignInString(PROPERTY_SAML_IDP_ENTITY_ID, v.text)),
        [] (const Value &v) -> String { return v.text.GetLength() > 500 ? String(_T("saml_idp_entity_id is at most 500 characters.")) : String(); } },
      { "saml_idp_sso_url", KindString, ReadWrite, EffectNow, nullptr,
        "The identity provider's sign-in URL for the HTTP-Redirect binding, where the browser is sent with the request: Entra ID's Login URL, Okta's Identity Provider Single Sign-On URL, ADFS's https://<host>/adfs/ls/. One line, https or the loopback. Empty offers no SAML sign-in.",
        ROW_TEXT(Config()->GetSignInString(PROPERTY_SAML_IDP_SSO_URL)), ROW_SET(Config()->SetSignInString(PROPERTY_SAML_IDP_SSO_URL, v.text)),
        [] (const Value &v) -> String
        {
           // The browser is sent to it in a Location header, as it is.
           if (v.text.Find(_T("\r")) >= 0 || v.text.Find(_T("\n")) >= 0)
              return String(_T("saml_idp_sso_url is one line."));
           return ImportUrlProblem(v.text, true);
        } },
      { "saml_idp_certificate", KindString, WriteOnly, EffectNow, nullptr,
        "The identity provider's signing certificate in PEM, or two while its key is rolled over; every signature is verified against these and never against a key the answer carries. RSA of 2048 bits or more, or an elliptic curve of 256 or more. A private key, anything that is not a certificate, or a paste over 60,000 characters is refused; a paste whose line breaks were dropped or made spaces is read. Stored as PEM again, which must fit 4,000 characters, and never answered back - not here, not in the list of what is changed from stock and not in the audit trail: saml_idp_certificate_fingerprints says what is held. Empty offers no SAML sign-in.",
        nullptr, ROW_SET(Config()->SetSignInString(PROPERTY_SAML_IDP_CERTIFICATE, SamlSignIn::NormalisedCertificates(v.text))),
        [] (const Value &v) -> String { return SamlSignIn::CertificateProblem(v.text); },
        nullptr, PROPERTY_SAML_IDP_CERTIFICATE },
      { "saml_idp_certificate_fingerprints", KindString, ReadOnly, EffectNow, nullptr,
        "The certificates saml_idp_certificate holds, by their SHA-256 fingerprints, their SHA-1 thumbprints (what Entra ID and ADFS show) and when each expires, separated by semicolons. Empty when none is held.",
        ROW_TEXT(SamlSignIn::CertificateSummary(Config()->GetSignInString(PROPERTY_SAML_IDP_CERTIFICATE))), nullptr, ROW_NO_CHECK },
      { "saml_sp_entity_id", KindString, ReadWrite, EffectNow, nullptr,
        "This server's entity ID as the identity provider knows it (Entra ID's Identifier, Okta's Audience URI, ADFS's relying party identifier): every assertion's audience must name it. Empty, the default, means the base URL - oidc_redirect_base_url, or import_oauth_redirect_base_url when that is empty - followed by /saml. The metadata at /saml/metadata carries it with the ACS URL, <base URL>/portal/saml/acs.",
        ROW_TEXT(Config()->GetSignInString(PROPERTY_SAML_SP_ENTITY_ID)), ROW_SET(Config()->SetSignInString(PROPERTY_SAML_SP_ENTITY_ID, v.text)),
        [] (const Value &v) -> String { return v.text.GetLength() > 500 ? String(_T("saml_sp_entity_id is at most 500 characters.")) : String(); } },
      { "saml_email_attribute", KindString, ReadWrite, EffectNow, nullptr,
        "The attribute the address is read from when the NameID is not an emailAddress one: the emailaddress claim URI Entra ID and ADFS send by default. Empty means that default. The attribute must carry exactly one value.",
        ROW_TEXT(Config()->GetSignInString(PROPERTY_SAML_EMAIL_ATTRIBUTE)), ROW_SET(Config()->SetSignInString(PROPERTY_SAML_EMAIL_ATTRIBUTE, v.text)),
        [] (const Value &v) -> String { return v.text.GetLength() > 500 ? String(_T("saml_email_attribute is at most 500 characters.")) : String(); } },
      { "saml_require_signed_assertion", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether the Assertion must carry its own signature. On, the default, which is how Entra ID and ADFS sign; off takes a signature over the whole Response as enough, which is what an identity provider set to sign the response only sends. Either way the assertion used is the one read back from the bytes a signature covered.",
        ROW_FLAG(Config()->GetSamlRequireSignedAssertion()), ROW_SET(Config()->SetSamlRequireSignedAssertion(v.flag)), ROW_NO_CHECK },
      { "saml_auto_provision", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether an address the SAML identity provider vouched for that has no mailbox here gets one made for it at its first sign-in, as oidc_auto_provision does for OIDC. Off, the default. The account made has no password of its own and is at a domain this server already has.",
        ROW_FLAG(Config()->GetSamlAutoProvision()), ROW_SET(Config()->SetSamlAutoProvision(v.flag)), ROW_NO_CHECK },
      { "saml_administrators", KindString, ReadWrite, EffectNow, nullptr,
        "The addresses that sign in to the Control Deck as the administrator when the SAML identity provider vouches for them, comma separated, as oidc_administrators does for OIDC. Empty, the default, means SAML reaches the webmail only.",
        ROW_TEXT(Config()->GetSignInString(PROPERTY_SAML_ADMINISTRATORS)), ROW_SET(Config()->SetSignInString(PROPERTY_SAML_ADMINISTRATORS, v.text)), ROW_NO_CHECK },
      { "saml_button_label", KindString, ReadWrite, EffectNow, nullptr,
        "The words on the SAML sign-in button of the webmail and the Control Deck. Sign in with SAML by default, which each page shows in its reader's language; anything else is shown as written. Its own words, so that the SAML button never reads as the OpenID Connect one does when one identity provider serves both. One line, at most 60 characters; empty means the default.",
        ROW_TEXT(Config()->GetSignInString(PROPERTY_SAML_BUTTON_LABEL)), ROW_SET(Config()->SetSignInString(PROPERTY_SAML_BUTTON_LABEL, v.text)),
        [] (const Value &v) -> String
        {
           if (v.text.GetLength() > 60 || v.text.Find(_T("\r")) >= 0 || v.text.Find(_T("\n")) >= 0)
              return String(_T("saml_button_label is one line of at most 60 characters."));
           return String();
        } },

      // Spam traps (Common/AntiSpam/SpamTrapMonitor.h). In hm_settings, read
      // when a trap is addressed, so a change applies to the next hit. Nothing
      // here does anything until a domain has a trap.
      { "spam_trap_ban_minutes", KindInteger, ReadWrite, EffectNow, nullptr,
        "How long the address that addressed a spam trap is banned, in minutes; 0 bans nobody. The ban is the expiring IP range a run of failed logons creates, lifted by the same sweep and mirrored into the firewall by the same reconciliation. An authenticated session, an IP range that does not run the spam tests, the anti-spam white list, an incoming relay and an address on AutoBanNeverBan are never banned.",
        ROW_NUMBER(Config()->GetSpamTrapBanMinutes()), ROW_SET(Config()->SetSpamTrapBanMinutes((int) v.number)),
        [] (const Value &v) -> String
        {
           return (v.number < 0 || v.number > 525600) ? String(SpamTrapBanRefusal) : String();
        } },
      { "spam_trap_block_days", KindInteger, ReadWrite, EffectNow, nullptr,
        "How long the envelope sender of a message to a spam trap stays on the server's block list, in days; 0 blocks nobody. The entry is an ordinary block at server scope, so a nearer allow entry still overrules it. The null sender, a sender in a domain this server hosts, and a sender the server's own allow list names are never blocked.",
        ROW_NUMBER(Config()->GetSpamTrapBlockDays()), ROW_SET(Config()->SetSpamTrapBlockDays((int) v.number)),
        [] (const Value &v) -> String
        {
           return (v.number < 0 || v.number > 3650) ? String(SpamTrapBlockRefusal) : String();
        } },
      { "spam_trap_hit_retention_days", KindInteger, ReadWrite, EffectNow, nullptr,
        "How long the record of spam trap hits is kept, in days. Swept hourly. The bans and blocked senders the hits made are not touched by the sweep.",
        ROW_NUMBER(Config()->GetSpamTrapHitRetentionDays()), ROW_SET(Config()->SetSpamTrapHitRetentionDays((int) v.number)),
        [] (const Value &v) -> String
        {
           return (v.number < 1 || v.number > 3650) ? String(SpamTrapRetentionRefusal) : String();
        } },
      { "spam_trap_teach_spamassassin", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether a message sent to a real mailbox by a session that addressed a spam trap is taught to SpamAssassin as spam (the spamc TELL command, the store its verdicts come from). Off by default, and does nothing unless SpamAssassin is enabled and its spamd allows TELL. A trap hit with no message after it teaches nothing: the trap is refused at RCPT TO, so its own message never arrives.",
        ROW_FLAG(Config()->GetSpamTrapTeachSpamAssassin()), ROW_SET(Config()->SetSpamTrapTeachSpamAssassin(v.flag)), ROW_NO_CHECK },
      { "session_bind_to_address", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether a session is refused from any address but the one it began on, so a stolen cookie is of no use away from the victim's network. Off by default: an address that changes underneath somebody - a phone moving between wi-fi and a mobile network - ends their session while this is on.",
        ROW_FLAG(Config()->GetSessionBindToAddress()), ROW_SET(Config()->SetSessionBindToAddress(v.flag)), ROW_NO_CHECK },
      { "alert_sender_address", KindString, ReadWrite, EffectNow, nullptr,
        "The address alerts are sent FROM. Empty means nothing is mailed, which is the default.",
        ROW_TEXT(Config()->GetAlertSenderAddress()), ROW_SET(Config()->SetAlertSenderAddress(v.text)), ROW_NO_CHECK },
      { "alert_digest_enabled", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether conditions marked for the digest wait for one daily message rather than sending one message each.",
        ROW_FLAG(Config()->GetAlertDigestEnabled()), ROW_SET(Config()->SetAlertDigestEnabled(v.flag)), ROW_NO_CHECK },
      { "alert_digest_hour", KindInteger, ReadWrite, EffectNow, nullptr,
        "The hour of the day, UTC, at which the digest is sent. 0 to 23.",
        ROW_NUMBER(Config()->GetAlertDigestHour()), ROW_SET(Config()->SetAlertDigestHour((int) v.number)), ROW_NO_CHECK },
      { "alert_max_per_hour", KindInteger, ReadWrite, EffectNow, nullptr,
        "The most alerts that will be sent one at a time in an hour. Past it everything goes into the digest instead.",
        ROW_NUMBER(Config()->GetAlertMaxPerHour()), ROW_SET(Config()->SetAlertMaxPerHour((int) v.number)), ROW_NO_CHECK },
      { "alert_webhook_max_attempts", KindInteger, ReadWrite, EffectNow, nullptr,
        "How many times a webhook is attempted before the event is dead-lettered. 1 to 20.",
        ROW_NUMBER(Config()->GetAlertWebhookMaxAttempts()), ROW_SET(Config()->SetAlertWebhookMaxAttempts((int) v.number)), ROW_NO_CHECK },

      // Outbound webhooks (Util/WebhookManager.h). Four rows in hm_settings; the
      // subscriptions themselves are rows in hm_webhooks reached at
      // /api/v1/webhooks rather than settings, exactly as the alert rules are.
      { "webhooks_enabled", KindBoolean, ReadWrite, EffectNow, nullptr,
        "The master switch for outbound webhooks. It opens nothing on its own: a server with no subscription at /api/v1/webhooks raises no event and makes no request. Switch it off to silence every receiver at once without deleting what names them. The inbound webhooks (/api/v1/inbound-webhooks, docs/InboundWebhooks.md) are not silenced by it: each of their routes is switched on or off by itself.",
        ROW_FLAG(Config()->GetWebhooksEnabled()), ROW_SET(Config()->SetWebhooksEnabled(v.flag)), ROW_NO_CHECK },
      { "webhook_max_attempts", KindInteger, ReadWrite, EffectNow, nullptr,
        "How many times a webhook delivery is attempted before it is dead-lettered with its body and the reason kept - and an inbound webhook's message before it is kept or returned to its sender, as its route says (docs/InboundWebhooks.md). 1 to 20.",
        ROW_NUMBER(Config()->GetWebhookMaxAttempts()), ROW_SET(Config()->SetWebhookMaxAttempts((int) v.number)),
        [] (const Value &v) { return (v.number < 1 || v.number > 20) ? String(WebhookMaxAttemptsRefusal) : String(); } },
      { "webhook_max_per_minute", KindInteger, ReadWrite, EffectNow, nullptr,
        "The ceiling a subscription that names no rate limit of its own is held to. Events past it in one minute are dropped and counted, not queued, so a busy hour cannot become a flood an hour later. 1 to 100000.",
        ROW_NUMBER(Config()->GetWebhookMaxPerMinute()), ROW_SET(Config()->SetWebhookMaxPerMinute((int) v.number)),
        [] (const Value &v) { return (v.number < 1 || v.number > 100000) ? String(WebhookMaxPerMinuteRefusal) : String(); } },
      { "webhook_retention_days", KindInteger, ReadWrite, EffectNow, nullptr,
        "Days of delivered and dead-lettered webhook rows to keep, and of an inbound webhook's delivered and returned ones - its kept messages are never swept (docs/InboundWebhooks.md). 0 keeps everything, which for a per-message stream is a decision rather than a default. 0 to 3650.",
        ROW_NUMBER(Config()->GetWebhookRetentionDays()), ROW_SET(Config()->SetWebhookRetentionDays((int) v.number)),
        [] (const Value &v) { return (v.number < 0 || v.number > 3650) ? String(WebhookRetentionDaysRefusal) : String(); } },

      // Migration jobs (Application/MigrationManager.h). In hm_settings, read
      // each time the runner looks for a job to start, so a change applies to
      // the next job started; a job already running keeps its connection.
      { "migration_connections_per_host", KindInteger, ReadWrite, EffectNow, nullptr,
        "The most connections every running migration job of every migration together holds to one source host, however many jobs each migration may run at once. A provider counts sessions per client address, and five hundred mailboxes started at once from one address is how an address gets throttled for the rest of the day. 1 to 64; 4 by default.",
        ROW_NUMBER(Config()->GetMigrationConnectionsPerHost()), ROW_SET(Config()->SetMigrationConnectionsPerHost((int) v.number)),
        [] (const Value &v) { return (v.number < 1 || v.number > 64) ? String(MigrationConnectionsPerHostRefusal) : String(); } },

      // PST import (Util/PstImportPath.h). In hm_settings, read each time a PST
      // job is added or run.
      { "pst_import_directory", KindString, ReadWrite, EffectNow, nullptr,
        "The directory Outlook data files (PST) are copied into for a PST migration job, on this machine: a job names a file in its subdirectory named for the target account's domain (<directory>/example.com/archive.pst), and nowhere else. An absolute local path; empty, as shipped, takes no PST job.",
        ROW_TEXT(Config()->GetPstImportDirectory()), ROW_SET(Config()->SetPstImportDirectory(v.text)),
        [] (const Value &v) -> String { return PstImportPath::SettingProblem(v.text); } },

      // The password breach check (PasswordBreachCheck). In hm_settings, read at
      // every password change. The checks are the COM setters' own, so a value
      // the Control Panel refuses is refused here in the same words.
      { "breach_check_enabled", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether a new password found in the published breach corpus is refused. Off by default, because it sends the first five characters of the password's SHA-1 to breach_check_url and nothing else.",
        ROW_FLAG(Config()->GetBreachCheckEnabled()), ROW_SET(Config()->SetBreachCheckEnabled(v.flag)), ROW_NO_CHECK },
      { "breach_check_url", KindString, ReadWrite, EffectNow, nullptr,
        "The range endpoint the breach check asks, https or plain http to this machine only; the prefix is appended, or put where {prefix} is. Defaults to https://api.pwnedpasswords.com/range/.",
        ROW_TEXT(Config()->GetBreachCheckUrl()), ROW_SET(Config()->SetBreachCheckUrl(v.text)),
        [] (const Value &v) -> String
        {
           String reason;
           PasswordBreachCheck::ValidateEndpoint(v.text, reason);
           return reason;
        } },
      { "breach_check_minimum_count", KindInteger, ReadWrite, EffectNow, nullptr,
        "How many times a password must appear in the corpus to be refused; 1 or more, 1 by default.",
        ROW_NUMBER(Config()->GetBreachCheckMinimumCount()), ROW_SET(Config()->SetBreachCheckMinimumCount(v.number)),
        [] (const Value &v) -> String
        {
           return v.number < 1 ? String(BreachCheckMinimumRefusal) : String();
        } },
      { "breach_check_unreachable_action", KindEnum, ReadWrite, EffectNow, BreachCheckUnreachableWords,
        "What happens to a password change when the endpoint cannot be used: allow lets it through, refuse refuses it. Both are recorded in the error log.",
        ROW_NUMBER(Config()->GetBreachCheckUnreachableAction()), ROW_SET(Config()->SetBreachCheckUnreachableAction(v.number)), ROW_NO_CHECK },

      // Country blocking (CountryBlocking). Seven rows in hm_settings and one
      // read-only answer about the file they name. The four policies share one
      // grammar and one refusal sentence with the COM properties, because both
      // go through CountryBlocking::Canonicalise - which also means a policy is
      // stored in one spelling whichever surface wrote it.
      { "geoip_database_file", KindString, ReadWrite, EffectNow, nullptr,
        "The MaxMind database file (.mmdb) the country policies read. The administrator supplies it; this server ships none and downloads none. Empty means nothing is judged by country, whatever the policies say.",
        ROW_TEXT(Config()->GetGeoIpDatabaseFile()),
        [] (const Value &v) { String path = v.text; path.Trim(); Config()->SetGeoIpDatabaseFile(path); CountryBlocking::Instance()->Invalidate(); return Applied(); }, ROW_NO_CHECK },
      { "geoip_database_max_age_days", KindInteger, ReadWrite, EffectNow, nullptr,
        "How many days old the database may be before the application log calls it stale. 45 by default, 0 never. A stale database is still used, because most addresses in it have not moved.",
        ROW_NUMBER(Config()->GetGeoIpDatabaseMaxAgeDays()),
        [] (const Value &v) { Config()->SetGeoIpDatabaseMaxAgeDays((int) v.number); CountryBlocking::Instance()->Invalidate(); return Applied(); },
        [] (const Value &v) -> String
        {
           return (v.number < 0 || v.number > 3650) ? String(GeoIpMaxAgeRefusal) : String();
        } },
      { "geoip_database_status", KindString, ReadOnly, EffectNow, nullptr,
        "What the server can say about the database right now, as JSON: configured, path, loaded, error, type, build_epoch, age_days, stale, bytes, max_age_days, refusals_total, refusals_by_country and the four policies. Shown and never sent.",
        [] { return Text(String(CountryBlocking::Instance()->StatusJson())); }, nullptr, ROW_NO_CHECK },
      { "country_blocking_exempt_ranges", KindString, ReadWrite, EffectNow, nullptr,
        "Addresses and CIDR ranges no country policy judges, comma separated; the loopback address and the private networks by default. This is the whole of the exemption - emptying it has this server's own network judged like anybody else's, which a strict allow list will refuse.",
        ROW_TEXT(Config()->GetCountryBlockingExemptRanges()),
        [] (const Value &v) { String ranges = v.text; ranges.Trim(); Config()->SetCountryBlockingExemptRanges(ranges); return Applied(); }, ROW_NO_CHECK },
      { "country_policy_smtp", KindString, ReadWrite, EffectNow, nullptr,
        "The country policy for the SMTP listeners, applied when a connection is accepted: empty is off, allow:GB,IE admits only those countries, block:CN,RU refuses those. An allow list refuses an address whose country is not known; a block list allows it.",
        ROW_TEXT(Config()->GetCountryPolicySMTP()), [] (const Value &v) { return ApplyCountryPolicy(v, &Configuration::SetCountryPolicySMTP); }, CountryPolicyCheck },
      { "country_policy_imap", KindString, ReadWrite, EffectNow, nullptr,
        "The country policy for the IMAP listeners, in the same grammar as country_policy_smtp.",
        ROW_TEXT(Config()->GetCountryPolicyIMAP()), [] (const Value &v) { return ApplyCountryPolicy(v, &Configuration::SetCountryPolicyIMAP); }, CountryPolicyCheck },
      { "country_policy_pop3", KindString, ReadWrite, EffectNow, nullptr,
        "The country policy for the POP3 listeners, in the same grammar as country_policy_smtp.",
        ROW_TEXT(Config()->GetCountryPolicyPOP3()), [] (const Value &v) { return ApplyCountryPolicy(v, &Configuration::SetCountryPolicyPOP3); }, CountryPolicyCheck },
      { "country_policy_http", KindString, ReadWrite, EffectNow, nullptr,
        "The country policy for the HTTP listeners - the REST API, this interface and the web services share one - in the same grammar as country_policy_smtp. Set it with care: it is the listener the Control Deck itself arrives on.",
        ROW_TEXT(Config()->GetCountryPolicyHTTP()), [] (const Value &v) { return ApplyCountryPolicy(v, &Configuration::SetCountryPolicyHTTP); }, CountryPolicyCheck },
   };

   const Row AntiSpamRows[] =
   {
      { "spam_mark_threshold", KindInteger, ReadWrite, EffectNow, nullptr,
        "The score at which a message is treated as spam.",
        ROW_NUMBER(AntiSpam().GetSpamMarkThreshold()), ROW_SET(AntiSpam().SetSpamMarkThreshold((int) v.number)), ROW_NO_CHECK },
      { "spam_delete_threshold", KindInteger, ReadWrite, EffectNow, nullptr,
        "The score at which a message is deleted.",
        ROW_NUMBER(AntiSpam().GetSpamDeleteThreshold()), ROW_SET(AntiSpam().SetSpamDeleteThreshold((int) v.number)), ROW_NO_CHECK },
      { "add_header_spam", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether an X-hMailServer-Spam header is added to a message treated as spam.",
        ROW_FLAG(AntiSpam().GetAddHeaderSpam()), ROW_SET(AntiSpam().SetAddHeaderSpam(v.flag)), ROW_NO_CHECK },
      { "add_header_reason", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether X-hMailServer-Reason headers name each test that scored.",
        ROW_FLAG(AntiSpam().GetAddHeaderReason()), ROW_SET(AntiSpam().SetAddHeaderReason(v.flag)), ROW_NO_CHECK },
      { "prepend_subject", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether the subject of a message treated as spam is prefixed.",
        ROW_FLAG(AntiSpam().GetPrependSubject()), ROW_SET(AntiSpam().SetPrependSubject(v.flag)), ROW_NO_CHECK },
      { "prepend_subject_text", KindString, ReadWrite, EffectNow, nullptr,
        "The subject prefix.",
        ROW_TEXT(AntiSpam().GetPrependSubjectText()), ROW_SET(AntiSpam().SetPrependSubjectText(v.text)), ROW_NO_CHECK },
      { "use_spf", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether the sender's SPF record is checked.",
        ROW_FLAG(AntiSpam().GetUseSPF()), ROW_SET(AntiSpam().SetUseSPF(v.flag)), ROW_NO_CHECK },
      { "use_spf_score", KindInteger, ReadWrite, EffectNow, nullptr,
        "The score an SPF failure adds.",
        ROW_NUMBER(AntiSpam().GetUseSPFScore()), ROW_SET(AntiSpam().SetUseSPFScore((int) v.number)), ROW_NO_CHECK },
      // SPF's other results, scored on their own (discussion #286). 0 leaves the
      // result unscored, as every result but Fail always was; Neutral and None
      // are never scored, and SoftFail is never a Pass for the greylisting bypass.
      { "use_spf_softfail_score", KindInteger, ReadWrite, EffectNow, nullptr,
        "The score an SPF softfail adds: the domain's policy names the client as probably unauthorised (~all) and asks that the message be accepted anyway. 0, the default, adds nothing; a score at or above spam_delete_threshold refuses the message during the SMTP conversation as a fail does.",
        ROW_NUMBER(AntiSpam().GetUseSPFSoftFailScore()), ROW_SET(AntiSpam().SetUseSPFSoftFailScore((int) v.number)), ROW_NO_CHECK },
      { "use_spf_temperror_score", KindInteger, ReadWrite, EffectNow, nullptr,
        "The score an SPF temperror adds: the check did not finish, because DNS timed out, answered SERVFAIL or the lookup limit was reached, and the record may be fine. 0, the default, adds nothing. Ignored for a message spf_temperror_defer answers 451 to, because that message is never accepted.",
        ROW_NUMBER(AntiSpam().GetUseSPFTempErrorScore()), ROW_SET(AntiSpam().SetUseSPFTempErrorScore((int) v.number)), ROW_NO_CHECK },
      { "use_spf_permerror_score", KindInteger, ReadWrite, EffectNow, nullptr,
        "The score an SPF permerror adds: the check finished and the domain's published record is unusable - two records, a bad mechanism, a redirect loop, too many lookups. 0, the default, adds nothing; a retry will not fix a permerror, so it is never deferred.",
        ROW_NUMBER(AntiSpam().GetUseSPFPermErrorScore()), ROW_SET(AntiSpam().SetUseSPFPermErrorScore((int) v.number)), ROW_NO_CHECK },
      { "spf_temperror_defer", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether an SPF temperror answers 451 4.4.3 for the transaction, RFC 7208 section 8.6's own answer to a check that did not finish, so that a resolver blip defers the message instead of accepting or refusing it on a guess. Off by default: a temperror is accepted and scored by use_spf_temperror_score. A sender the domain policy list marks white is never deferred.",
        ROW_FLAG(AntiSpam().GetSPFTempErrorDefer()), ROW_SET(AntiSpam().SetSPFTempErrorDefer(v.flag)), ROW_NO_CHECK },
      // MAIL FROM:<> with more than one RCPT TO (discussion #283): not a bounce,
      // because a delivery status notification has one recipient.
      { "reject_multi_rcpt_null_sender", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether MAIL FROM:<> naming more than one recipient is refused: the second and every later RCPT TO answers 550 5.7.1 and the transaction's DATA is refused too, because a delivery status notification has exactly one recipient and a null sender with several is backscatter, a forged report or a probe. A single-recipient null sender is untouched. Off by default; unauthenticated sessions of an IP range that runs the spam tests only, and never a sender the anti-spam white list covers.",
        ROW_FLAG(AntiSpam().GetRejectMultiRcptNullSender()), ROW_SET(AntiSpam().SetRejectMultiRcptNullSender(v.flag)), ROW_NO_CHECK },
      { "ban_multi_rcpt_null_sender", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether the connecting address of a refused multi-recipient null sender is also banned, for ban_multi_rcpt_null_sender_hours, through the expiring IP range a run of failed logons creates - lifted by the same sweep and mirrored into the firewall by the same reconciliation, and never for an address on AutoBanNeverBan or an incoming relay. Off by default, so a single mis-sent report with two recipients does not ban a real mail server; does nothing while reject_multi_rcpt_null_sender is off.",
        ROW_FLAG(AntiSpam().GetBanMultiRcptNullSender()), ROW_SET(AntiSpam().SetBanMultiRcptNullSender(v.flag)), ROW_NO_CHECK },
      { "ban_multi_rcpt_null_sender_hours", KindInteger, ReadWrite, EffectNow, nullptr,
        "How long that ban lasts, in hours, 1 to 8760. 24 by default.",
        ROW_NUMBER(AntiSpam().GetBanMultiRcptNullSenderHours()), ROW_SET(AntiSpam().SetBanMultiRcptNullSenderHours((int) v.number)),
        [] (const Value &v) -> String
        {
           return (v.number < 1 || v.number > 8760) ? String(NullSenderBanHoursRefusal) : String();
        } },
      { "check_host_in_helo", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether the HELO/EHLO name must resolve to the connecting address.",
        ROW_FLAG(AntiSpam().GetCheckHostInHelo()), ROW_SET(AntiSpam().SetCheckHostInHelo(v.flag)), ROW_NO_CHECK },
      { "check_host_in_helo_score", KindInteger, ReadWrite, EffectNow, nullptr,
        "The score a HELO/EHLO mismatch adds.",
        ROW_NUMBER(AntiSpam().GetCheckHostInHeloScore()), ROW_SET(AntiSpam().SetCheckHostInHeloScore((int) v.number)), ROW_NO_CHECK },
      { "check_mx_records", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether the sender's domain must have an MX or an A record.",
        ROW_FLAG(AntiSpam().GetUseMXChecks()), ROW_SET(AntiSpam().SetUseMXChecks(v.flag)), ROW_NO_CHECK },
      { "check_mx_records_score", KindInteger, ReadWrite, EffectNow, nullptr,
        "The score a sender domain without an MX or an A record adds.",
        ROW_NUMBER(AntiSpam().GetUseMXChecksScore()), ROW_SET(AntiSpam().SetUseMXChecksScore((int) v.number)), ROW_NO_CHECK },
      { "check_ptr", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether the connecting address must have a PTR record.",
        ROW_FLAG(AntiSpam().GetCheckPTR()), ROW_SET(AntiSpam().SetCheckPTR(v.flag)), ROW_NO_CHECK },
      { "check_ptr_score", KindInteger, ReadWrite, EffectNow, nullptr,
        "The score a missing PTR record adds.",
        ROW_NUMBER(AntiSpam().GetCheckPTRScore()), ROW_SET(AntiSpam().SetCheckPTRScore((int) v.number)), ROW_NO_CHECK },
      { "dkim_verification_enabled", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether DKIM signatures are verified.",
        ROW_FLAG(AntiSpam().GetDKIMVerificationEnabled()), ROW_SET(AntiSpam().SetDKIMVerificationEnabled(v.flag)), ROW_NO_CHECK },
      { "dkim_verification_failure_score", KindInteger, ReadWrite, EffectNow, nullptr,
        "The score a failed DKIM signature adds.",
        ROW_NUMBER(AntiSpam().GetDKIMVerificationFailureScore()), ROW_SET(AntiSpam().SetDKIMVerificationFailureScore((int) v.number)), ROW_NO_CHECK },
      { "dmarc_enabled", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether the sender domain's DMARC policy is evaluated.",
        ROW_FLAG(AntiSpam().GetDMARCEnabled()), ROW_SET(AntiSpam().SetDMARCEnabled(v.flag)), ROW_NO_CHECK },
      { "dmarc_failure_score", KindInteger, ReadWrite, EffectNow, nullptr,
        "The score a DMARC failure adds.",
        ROW_NUMBER(AntiSpam().GetDMARCFailureScore()), ROW_SET(AntiSpam().SetDMARCFailureScore((int) v.number)), ROW_NO_CHECK },
      { "arc_filtering_enabled", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether an ARC chain from a trusted sealer overrides SPF, DKIM and DMARC failures.",
        ROW_FLAG(AntiSpam().GetArcFilteringEnabled()), ROW_SET(AntiSpam().SetArcFilteringEnabled(v.flag)), ROW_NO_CHECK },
      { "arc_trusted_sealers", KindString, ReadWrite, EffectNow, nullptr,
        "The ARC sealer domains whose chains are trusted, comma-separated.",
        ROW_TEXT(AntiSpam().GetArcTrustedSealers()), ROW_SET(AntiSpam().SetArcTrustedSealers(v.text)), ROW_NO_CHECK },
      { "spamassassin_enabled", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether messages are passed to a SpamAssassin daemon.",
        ROW_FLAG(AntiSpam().GetSpamAssassinEnabled()), ROW_SET(AntiSpam().SetSpamAssassinEnabled(v.flag)), ROW_NO_CHECK },
      { "spamassassin_host", KindString, ReadWrite, EffectNow, nullptr,
        "The SpamAssassin daemon's host.",
        ROW_TEXT(AntiSpam().GetSpamAssassinHost()), ROW_SET(AntiSpam().SetSpamAssassinHost(v.text)), ROW_NO_CHECK },
      { "spamassassin_port", KindInteger, ReadWrite, EffectNow, nullptr,
        "The SpamAssassin daemon's port.",
        ROW_NUMBER(AntiSpam().GetSpamAssassinPort()), ROW_SET(AntiSpam().SetSpamAssassinPort((int) v.number)), ROW_NO_CHECK },
      { "spamassassin_score", KindInteger, ReadWrite, EffectNow, nullptr,
        "The score added when SpamAssassin judges a message to be spam and its own score is not merged.",
        ROW_NUMBER(AntiSpam().GetSpamAssassinScore()), ROW_SET(AntiSpam().SetSpamAssassinScore((int) v.number)), ROW_NO_CHECK },
      { "spamassassin_merge_score", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether SpamAssassin's own score is added instead of spamassassin_score.",
        ROW_FLAG(AntiSpam().GetSpamAssassinMergeScore()), ROW_SET(AntiSpam().SetSpamAssassinMergeScore(v.flag)), ROW_NO_CHECK },
      { "tarpit_count", KindInteger, ReadWrite, EffectNow, nullptr,
        "The recipient count in one session after which each further RCPT TO is delayed; 0 is off. Kept in hMailServer.ini.",
        ROW_NUMBER(Ini()->GetSmtpTarpitCount()),
        [] (const Value &v) { return Ini()->SetSmtpTarpitCount((int) v.number) ? Applied() : Failed(IniWriteFailure); },
        [] (const Value &v) { return v.number < 0 ? String(TarpitCountRefusal) : String(); },
        _T("SmtpTarpitCount") },
      { "tarpit_delay", KindInteger, ReadWrite, EffectNow, nullptr,
        "The delay in seconds, 0 to 30. Kept in hMailServer.ini.",
        ROW_NUMBER(Ini()->GetSmtpTarpitDelaySeconds()),
        [] (const Value &v) { return Ini()->SetSmtpTarpitDelaySeconds((int) v.number) ? Applied() : Failed(IniWriteFailure); },
        [] (const Value &v) { return (v.number < 0 || v.number > IniFileSettings::TARPIT_MAX_SECONDS) ? String(TarpitDelayRefusal) : String(); },
        _T("SmtpTarpitDelaySeconds") },
      { "greylisting_enabled", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether a first delivery attempt from an unknown sender, recipient and address is deferred.",
        ROW_FLAG(AntiSpam().GetUseGreyListing()), ROW_SET(AntiSpam().SetUseGreyListing(v.flag)), ROW_NO_CHECK },
      { "greylisting_initial_delay", KindInteger, ReadWrite, EffectNow, nullptr,
        "The minutes a first attempt is deferred.",
        ROW_NUMBER(AntiSpam().GetGreyListingInitialDelay()), ROW_SET(AntiSpam().SetGreyListingInitialDelay((int) v.number)), ROW_NO_CHECK },
      { "greylisting_initial_delete", KindInteger, ReadWrite, EffectNow, nullptr,
        "The hours after which an unconfirmed triplet is forgotten.",
        ROW_NUMBER(AntiSpam().GetGreyListingInitialDelete()), ROW_SET(AntiSpam().SetGreyListingInitialDelete((int) v.number)), ROW_NO_CHECK },
      { "greylisting_final_delete", KindInteger, ReadWrite, EffectNow, nullptr,
        "The days after which a confirmed triplet is forgotten.",
        ROW_NUMBER(AntiSpam().GetGreyListingFinalDelete()), ROW_SET(AntiSpam().SetGreyListingFinalDelete((int) v.number)), ROW_NO_CHECK },
      { "bypass_greylisting_on_spf_success", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether a sender that passes SPF is not greylisted.",
        ROW_FLAG(AntiSpam().GetBypassGreyListingOnSPFSuccess()), ROW_SET(AntiSpam().SetBypassGreyListingOnSPFSuccess(v.flag)), ROW_NO_CHECK },
      { "bypass_greylisting_on_mail_from_mx", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether a message from one of the sender domain's MX hosts is not greylisted.",
        ROW_FLAG(AntiSpam().GetBypassGreyListingOnMailFromMX()), ROW_SET(AntiSpam().SetBypassGreyListingOnMailFromMX(v.flag)), ROW_NO_CHECK },
      // Greylisting, hardened (SMTP/GreyListing.h). Off, 0 and inert by default:
      // with these at their defaults greylisting is classic Evans, as it was.
      { "greylisting_slide_on_early_retry", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether a retry before the greylisting window opens moves the window: each such retry sets the block end to now plus greylisting_initial_delay again, so a sender that retries too soon never reaches it. Off by default, which is classic greylisting; a well-behaved server that retries once too quickly is delayed a little longer with it on.",
        ROW_FLAG(AntiSpam().GetGreyListingSlideOnEarlyRetry()), ROW_SET(AntiSpam().SetGreyListingSlideOnEarlyRetry(v.flag)), ROW_NO_CHECK },
      { "greylisting_abuse_threshold", KindInteger, ReadWrite, EffectNow, nullptr,
        "The number of delivery attempts before the window opens at which a pending triplet is refused with 550 instead of deferred with 451, for greylisting_abuse_lock_minutes. Attempts and not retries: every RCPT TO for the triplet counts, so a sender with twenty queued messages for one recipient reaches a threshold of twenty down a single connection without having retried anything. 0, the default, never refuses. What it stops is a sender that retries on a timer rather than from a queue; a misconfigured retry loop is stranded until the lock passes, after which the triplet starts over as a first attempt. Between 0 and 100000.",
        ROW_NUMBER(AntiSpam().GetGreyListingAbuseThreshold()), ROW_SET(AntiSpam().SetGreyListingAbuseThreshold((int) v.number)),
        [] (const Value &v) -> String
        {
           return (v.number < 0 || v.number > 100000) ? String(GreyListingAbuseThresholdRefusal) : String();
        } },
      { "greylisting_abuse_lock_minutes", KindInteger, ReadWrite, EffectNow, nullptr,
        "How long the 550 of greylisting_abuse_threshold lasts, in minutes; 60 by default, between 1 and 10080 (a week). Means nothing while the threshold is 0.",
        ROW_NUMBER(AntiSpam().GetGreyListingAbuseLockMinutes()), ROW_SET(AntiSpam().SetGreyListingAbuseLockMinutes((int) v.number)),
        [] (const Value &v) -> String
        {
           return (v.number < 1 || v.number > 10080) ? String(GreyListingAbuseLockRefusal) : String();
        } },
      { "maximum_message_size_kb", KindInteger, ReadWrite, EffectNow, nullptr,
        "Messages larger than this, in KB, are not scanned; 0 is no limit.",
        ROW_NUMBER(AntiSpam().GetAntiSpamMaxSizeKB()), ROW_SET(AntiSpam().SetAntiSpamMaxSizeKB((int) v.number)), ROW_NO_CHECK },
   };

   // InterfaceAntiVirus's fourteen scalars, through the same
   // AntiVirusConfiguration the COM properties write. The scanners read that
   // object each time a message is scanned (VirusScanner, ClamAVVirusScanner,
   // ClamWinVirusScanner, CustomVirusScanner), so a change is in force for the
   // next message; nothing here waits for a restart. The blocked-attachment
   // patterns themselves are AntiVirus.BlockedAttachments over COM, a
   // collection and not a setting, and are not in this group.
   const Row AntiVirusRows[] =
   {
      // ClamAV, over TCP.
      { "clamav_enabled", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether messages are scanned by a ClamAV daemon.",
        ROW_FLAG(AntiVirus().GetClamAVEnabled()), ROW_SET(AntiVirus().SetClamAVEnabled(v.flag)), ROW_NO_CHECK },
      { "clamav_host", KindString, ReadWrite, EffectNow, nullptr,
        "The ClamAV daemon's host.",
        ROW_TEXT(AntiVirus().GetClamAVHost()), ROW_SET(AntiVirus().SetClamAVHost(v.text)), ROW_NO_CHECK },
      { "clamav_port", KindInteger, ReadWrite, EffectNow, nullptr,
        "The ClamAV daemon's port, 1 to 65535.",
        ROW_NUMBER(AntiVirus().GetClamAVPort()), ROW_SET(AntiVirus().SetClamAVPort((int) v.number)),
        [] (const Value &v) { return (v.number < 1 || v.number > 65535) ? String(ClamAVPortRefusal) : String(); } },

      // ClamWin, run as a program.
      { "clamwin_enabled", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether messages are scanned by running the ClamWin scanner.",
        ROW_FLAG(AntiVirus().ClamWinEnabled()), ROW_SET(AntiVirus().ClamWinEnabled(v.flag)), ROW_NO_CHECK },
      { "clamwin_executable", KindString, ReadWrite, EffectNow, nullptr,
        "The full path of the ClamWin scanner program.",
        ROW_TEXT(AntiVirus().ClamWinExecutable()), ROW_SET(AntiVirus().ClamWinExecutable(v.text)), ROW_NO_CHECK },
      { "clamwin_db_folder", KindString, ReadWrite, EffectNow, nullptr,
        "The directory holding the ClamWin virus database.",
        ROW_TEXT(AntiVirus().ClamWinDatabase()), ROW_SET(AntiVirus().ClamWinDatabase(v.text)), ROW_NO_CHECK },

      // Any other scanner, run as a program.
      { "custom_scanner_enabled", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether messages are scanned by running the custom scanner command.",
        ROW_FLAG(AntiVirus().GetCustomScannerEnabled()), ROW_SET(AntiVirus().SetCustomScannerEnabled(v.flag)), ROW_NO_CHECK },
      { "custom_scanner_executable", KindString, ReadWrite, EffectNow, nullptr,
        "The command line of the custom scanner; %FILE% in it stands for the message file.",
        ROW_TEXT(AntiVirus().GetCustomScannerExecutable()), ROW_SET(AntiVirus().SetCustomScannerExecutable(v.text)), ROW_NO_CHECK },
      { "custom_scanner_return_value", KindInteger, ReadWrite, EffectNow, nullptr,
        "The exit code the custom scanner returns when it finds a virus.",
        ROW_NUMBER(AntiVirus().GetCustomScannerReturnValue()), ROW_SET(AntiVirus().SetCustomScannerReturnValue(v.number)), ROW_NO_CHECK },

      // What happens to a message a scanner flags, and who is told.
      { "action", KindEnum, ReadWrite, EffectNow, AntiVirusActionWords,
        "What is done with a message a scanner flags: delete_email drops it, delete_attachments strips its attachments and delivers the rest.",
        ROW_NUMBER(AntiVirus().AVAction()), ROW_SET(AntiVirus().AVAction((AntiVirusConfiguration::eAVAction) v.number)), ROW_NO_CHECK },
      { "notify_sender", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether the sender is told that a virus was found.",
        ROW_FLAG(AntiVirus().AVNotifySender()), ROW_SET(AntiVirus().AVNotifySender(v.flag)), ROW_NO_CHECK },
      { "notify_receiver", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether the recipient is told that a virus was found.",
        ROW_FLAG(AntiVirus().AVNotifyReceiver()), ROW_SET(AntiVirus().AVNotifyReceiver(v.flag)), ROW_NO_CHECK },
      { "maximum_message_size_kb", KindInteger, ReadWrite, EffectNow, nullptr,
        "Messages larger than this, in KB, are not scanned; 0 is no limit.",
        ROW_NUMBER(AntiVirus().GetVirusScanMaxSize()), ROW_SET(AntiVirus().SetVirusScanMaxSize((int) v.number)), ROW_NO_CHECK },

      // Attachments removed by name, whatever the scanners say.
      { "attachment_blocking_enabled", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether attachments whose names match the blocked-attachment list are removed.",
        ROW_FLAG(AntiVirus().GetEnableAttachmentBlocking()), ROW_SET(AntiVirus().SetEnableAttachmentBlocking(v.flag)), ROW_NO_CHECK },
   };

   const Row LoggingRows[] =
   {
      { "enabled", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether anything is logged at all.",
        ROW_FLAG(Config()->GetUseLogging()), ROW_SET(Config()->SetUseLogging(v.flag)), ROW_NO_CHECK },
      { "log_application", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether the server's own events are logged.",
        ROW_FLAG(Config()->GetLogApplication()), ROW_SET(Config()->SetLogApplication(v.flag)), ROW_NO_CHECK },
      { "log_smtp", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether SMTP sessions are logged.",
        ROW_FLAG(Config()->GetLogSMTPConversations()), ROW_SET(Config()->SetLogSMTPConversations(v.flag)), ROW_NO_CHECK },
      { "log_pop3", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether POP3 sessions are logged.",
        ROW_FLAG(Config()->GetLogPOP3Conversations()), ROW_SET(Config()->SetLogPOP3Conversations(v.flag)), ROW_NO_CHECK },
      { "log_imap", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether IMAP sessions are logged.",
        ROW_FLAG(Config()->GetLogIMAPConversations()), ROW_SET(Config()->SetLogIMAPConversations(v.flag)), ROW_NO_CHECK },
      { "log_tcpip", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether connections and disconnections are logged.",
        ROW_FLAG(Config()->GetLogTCPIP()), ROW_SET(Config()->SetLogTCPIP(v.flag)), ROW_NO_CHECK },
      { "log_debug", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether debugging detail is logged.",
        ROW_FLAG(Config()->GetLogDebug()), ROW_SET(Config()->SetLogDebug(v.flag)), ROW_NO_CHECK },
      { "log_awstats", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether an AWStats log is written.",
        ROW_FLAG(Config()->GetAWStatsEnabled()), ROW_SET(Config()->SetAWStatsEnabled(v.flag)), ROW_NO_CHECK },
      { "keep_files_open", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether the log files stay open between writes.",
        ROW_FLAG(Config()->GetKeepFilesOpen()), ROW_SET(Config()->SetKeepFilesOpen(v.flag)), ROW_NO_CHECK },
      { "device", KindEnum, ReadWrite, EffectNow, LogDeviceWords,
        "Where the log is written: file, or the sql database. unknown is the value an installation has until one is chosen, and behaves as file.",
        ROW_NUMBER(Config()->GetLogDevice()), ROW_SET(Config()->SetLogDevice(v.number)), ROW_NO_CHECK },
      { "log_format", KindEnum, ReadWrite, EffectNow, LogFormatWords,
        "How each line is rendered: the default, or the CSA format.",
        ROW_NUMBER(Config()->GetLogFormat()), ROW_SET(Config()->SetLogFormat(v.number)), ROW_NO_CHECK },

      // The syslog sink (RFC 5424). Every one of these is EffectRestart: the sink
      // reads its settings when it starts, which is at the next service start or
      // Reinitialize. These routes are not a convenience - on Linux there is no
      // Control Panel, so they and the Deck are the only way to turn syslog on,
      // which is the one thing a Linux administrator asks for first.
      { "syslog_enabled", KindBoolean, ReadWrite, EffectRestart, nullptr,
        "Whether a copy of each log entry is sent as RFC 5424 to a syslog collector, or - on Linux under systemd with no collector host - to the journal.",
        ROW_FLAG(Config()->GetSyslogEnabled()), ROW_SET(Config()->SetSyslogEnabled(v.flag)), ROW_NO_CHECK },
      { "syslog_host", KindString, ReadWrite, EffectRestart, nullptr,
        "The collector's host name or address. Empty selects the systemd journal on Linux, and means nothing is sent on Windows.",
        ROW_TEXT(Config()->GetSyslogHost()), [] (const Value &v) { String host = v.text; Config()->SetSyslogHost(host); return Applied(); }, ROW_NO_CHECK },
      { "syslog_port", KindInteger, ReadWrite, EffectRestart, nullptr,
        "The collector's port: 514 for UDP and TCP, 6514 for TLS.",
        ROW_NUMBER(Config()->GetSyslogPort()), ROW_SET(Config()->SetSyslogPort((long) v.number)), ROW_NO_CHECK },
      { "syslog_transport", KindEnum, ReadWrite, EffectRestart, SyslogTransportWords,
        "How entries reach the collector: udp (truncated at 1024 bytes), tcp, or tls (RFC 5425, certificate verified).",
        ROW_NUMBER(Config()->GetSyslogTransport()), ROW_SET(Config()->SetSyslogTransport((long) v.number)), ROW_NO_CHECK },
      { "syslog_facility", KindInteger, ReadWrite, EffectRestart, nullptr,
        "The RFC 5424 facility, 0 to 23. 2 is mail; 16 to 23 are local0 to local7.",
        ROW_NUMBER(Config()->GetSyslogFacility()), ROW_SET(Config()->SetSyslogFacility((long) v.number)), ROW_NO_CHECK },
      { "syslog_minimum_severity", KindInteger, ReadWrite, EffectRestart, nullptr,
        "The least severe entry that is sent, 0 emergency to 7 debug. Numerically lower is more severe.",
        ROW_NUMBER(Config()->GetSyslogMinimumSeverity()), ROW_SET(Config()->SetSyslogMinimumSeverity((long) v.number)), ROW_NO_CHECK },
      { "syslog_log_smtp", KindBoolean, ReadWrite, EffectRestart, nullptr,
        "Whether SMTP conversations are sent to syslog. Errors are sent whatever these six say.",
        ROW_FLAG(Config()->GetSyslogLogType(Logger::LSSMTP)), ROW_SET(Config()->SetSyslogLogType(Logger::LSSMTP, v.flag)), ROW_NO_CHECK },
      { "syslog_log_pop3", KindBoolean, ReadWrite, EffectRestart, nullptr,
        "Whether POP3 conversations are sent to syslog.",
        ROW_FLAG(Config()->GetSyslogLogType(Logger::LSPOP3)), ROW_SET(Config()->SetSyslogLogType(Logger::LSPOP3, v.flag)), ROW_NO_CHECK },
      { "syslog_log_imap", KindBoolean, ReadWrite, EffectRestart, nullptr,
        "Whether IMAP conversations are sent to syslog.",
        ROW_FLAG(Config()->GetSyslogLogType(Logger::LSIMAP)), ROW_SET(Config()->SetSyslogLogType(Logger::LSIMAP, v.flag)), ROW_NO_CHECK },
      { "syslog_log_application", KindBoolean, ReadWrite, EffectRestart, nullptr,
        "Whether the server's own events are sent to syslog.",
        ROW_FLAG(Config()->GetSyslogLogType(Logger::LSApplication)), ROW_SET(Config()->SetSyslogLogType(Logger::LSApplication, v.flag)), ROW_NO_CHECK },
      { "syslog_log_tcpip", KindBoolean, ReadWrite, EffectRestart, nullptr,
        "Whether connections and disconnections are sent to syslog.",
        ROW_FLAG(Config()->GetSyslogLogType(Logger::LSTCPIP)), ROW_SET(Config()->SetSyslogLogType(Logger::LSTCPIP, v.flag)), ROW_NO_CHECK },
      { "syslog_log_debug", KindBoolean, ReadWrite, EffectRestart, nullptr,
        "Whether debugging detail is sent to syslog.",
        ROW_FLAG(Config()->GetSyslogLogType(Logger::LSDebug)), ROW_SET(Config()->SetSyslogLogType(Logger::LSDebug, v.flag)), ROW_NO_CHECK },

      // Facts about the log, from the same COM object, for a client that
      // wants to know where to look. Not settings, so not writable.
      { "directory", KindString, ReadOnly, EffectNow, nullptr,
        "The directory the log files are written to.",
        ROW_TEXT(Ini()->GetLogDirectory()), nullptr, ROW_NO_CHECK },
      { "current_default_log", KindString, ReadOnly, EffectNow, nullptr,
        "The full path of the current default log file.",
        ROW_TEXT(Logger::Instance()->GetCurrentLogFileName(Logger::Normal)), nullptr, ROW_NO_CHECK },
      { "current_error_log", KindString, ReadOnly, EffectNow, nullptr,
        "The full path of the current error log file.",
        ROW_TEXT(Logger::Instance()->GetCurrentLogFileName(Logger::Error)), nullptr, ROW_NO_CHECK },
      { "current_event_log", KindString, ReadOnly, EffectNow, nullptr,
        "The full path of the current event log file.",
        ROW_TEXT(Logger::Instance()->GetCurrentLogFileName(Logger::Events)), nullptr, ROW_NO_CHECK },
      { "current_awstats_log", KindString, ReadOnly, EffectNow, nullptr,
        "The full path of the current AWStats log file.",
        ROW_TEXT(Logger::Instance()->GetCurrentLogFileName(Logger::AWStats)), nullptr, ROW_NO_CHECK },
   };

   // InterfaceDirectories' seven properties, as facts: the Control Panel shows
   // them and a fixture reads them to find hMailServer.ini or the message
   // store, and nothing sets them here - a directory is chosen at install time
   // and read at process start. ini_file is the one COM does not have: the
   // file the settings above were read from, which on Linux may be beside the
   // binary or under /etc/hmailserver, and which a caller that writes a key
   // through /api/v1/settings/ini is otherwise left to guess.
   const Row DirectoriesRows[] =
   {
      { "program", KindString, ReadOnly, EffectNow, nullptr,
        "The installation directory ([Directories] ProgramFolder).",
        ROW_TEXT(Ini()->GetProgramDirectory()), nullptr, ROW_NO_CHECK },
      { "bin", KindString, ReadOnly, EffectNow, nullptr,
        "The directory the server binary runs from.",
        ROW_TEXT(Utilities::GetBinDirectory()), nullptr, ROW_NO_CHECK },
      { "data", KindString, ReadOnly, EffectNow, nullptr,
        "The message store, one directory per domain ([Directories] DataFolder).",
        ROW_TEXT(Ini()->GetDataDirectory()), nullptr, ROW_NO_CHECK },
      { "log", KindString, ReadOnly, EffectNow, nullptr,
        "Where the log files are written ([Directories] LogFolder).",
        ROW_TEXT(Ini()->GetLogDirectory()), nullptr, ROW_NO_CHECK },
      { "event", KindString, ReadOnly, EffectNow, nullptr,
        "Where the event-handler scripts live ([Directories] EventFolder).",
        ROW_TEXT(Ini()->GetEventDirectory()), nullptr, ROW_NO_CHECK },
      { "temp", KindString, ReadOnly, EffectNow, nullptr,
        "Where a message is written while it is being received ([Directories] TempFolder).",
        ROW_TEXT(Ini()->GetTempDirectory()), nullptr, ROW_NO_CHECK },
      { "database", KindString, ReadOnly, EffectNow, nullptr,
        "Where the built-in database lives ([Directories] DatabaseFolder).",
        ROW_TEXT(Ini()->GetDatabaseDirectory()), nullptr, ROW_NO_CHECK },
      { "db_scripts", KindString, ReadOnly, EffectNow, nullptr,
        "Where the schema scripts are read from.",
        ROW_TEXT(Ini()->GetDBScriptDirectory()), nullptr, ROW_NO_CHECK },
      { "ini_file", KindString, ReadOnly, EffectNow, nullptr,
        "The full path of the hMailServer.ini this server read its settings from.",
        ROW_TEXT(IniFileSettings::GetInitializationFile()), nullptr, ROW_NO_CHECK },
   };

   // The backup log lives beside the other logs, as InterfaceBackupSettings
   // says: <log directory>/hmailserver_backup.log.
   String BackupLogFile()
   {
      String directory = Ini()->GetLogDirectory();
      if (directory.Right(1) != FileUtilities::PathSeparator)
         directory += FileUtilities::PathSeparator;
      return directory + "hmailserver_backup.log";
   }

   // InterfaceScripting::put_Language's rule: three languages, accepted on every
   // platform so that a backup, an environment configuration or a configuration
   // document (hmctl config export) made on one applies on the other, and a name
   // that is none of them is refused rather than stored, because an unrecognised
   // name turns every event handler off in silence. The word is trimmed first, as
   // COM trims it, so that the two take the same text and refuse it in the same
   // sentence. Which of the three this build runs is GET /api/v1/status's
   // build.script_languages.
   const char *ScriptLanguageRefusalHead = "The script language '";
   const char *ScriptLanguageRefusalTail = "' is not supported. The script language is VBScript, JScript or JavaScript.";

   // The spelling ScriptServer compares against, for a word the check has let
   // through.
   String CanonicalScriptLanguage(const AnsiString &word)
   {
      if (word.CompareNoCase("JScript") == 0)
         return String("JScript");

      if (word.CompareNoCase("JavaScript") == 0)
         return String("JavaScript");

      return String("VBScript");
   }

   const Row ScriptingRows[] =
   {
      { "enabled", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether the event handlers run at all.",
        ROW_FLAG(Config()->GetUseScriptServer()), ROW_SET(Config()->SetUseScriptServer(v.flag)), ROW_NO_CHECK },
      { "language", KindString, ReadWrite, EffectNow, nullptr,
        "VBScript, JScript or JavaScript, the language the event handlers are written in; any other name is refused. The Windows builds run VBScript and JScript, the Linux builds JavaScript.",
        ROW_TEXT(Config()->GetScriptLanguage()),
        [] (const Value &v) { AnsiString word = v.text; word.Trim(); Config()->SetScriptLanguage(CanonicalScriptLanguage(word)); return Applied(); },
        [] (const Value &v) -> String { String trimmed = v.text; trimmed.Trim(); AnsiString word = trimmed; if (word.CompareNoCase("VBScript") == 0 || word.CompareNoCase("JScript") == 0 || word.CompareNoCase("JavaScript") == 0) return String(); return String(ScriptLanguageRefusalHead) + trimmed + ScriptLanguageRefusalTail; } },
      { "current_script_file", KindString, ReadOnly, EffectNow, nullptr,
        "The event-handler file the server is running, in the event directory.",
        ROW_TEXT(ScriptServer::Instance()->GetCurrentScriptFile()), nullptr, ROW_NO_CHECK },
      { "directory", KindString, ReadOnly, EffectNow, nullptr,
        "Where the event-handler scripts live.",
        ROW_TEXT(Ini()->GetEventDirectory()), nullptr, ROW_NO_CHECK },
   };

   // Settings.Cache over COM: the switch, and for each of the four caches what
   // it holds and its hit rate (read-only, as they are there), its ceiling and
   // its time to live. The lives are settings rows - put_DomainCacheTTL writes
   // PROPERTY_DOMAINCACHETTL and CacheContainer::OnPropertyChanged hands the
   // new value to the cache - so a change is stored and in force at once. The
   // ceilings are what put_DomainCacheMaxSizeKb sets: the live Cache<T>'s
   // limit and nothing else. No row and no INI key holds them (the Control
   // Panel's note under the same four editors says so), so a change takes
   // effect at once and the built-in 10240 KB returns when the server starts.
   // A clear is a POST beside the group.
   const char *CacheNegativeRefusalTail = " must be 0 or more.";

   String CacheNotNegative(const char *key, const Value &v)
   {
      return v.number < 0 ? String(key) + CacheNegativeRefusalTail : String();
   }

   const Row CacheRows[] =
   {
      { "enabled", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether the domain, account, alias and distribution-list caches are used at all.",
        ROW_FLAG(Config()->GetCacheConfiguration()->GetUseCache()), ROW_SET(Config()->GetCacheConfiguration()->SetUseCache(v.flag)), ROW_NO_CHECK },
      { "domain_cache_size_kb", KindInteger, ReadOnly, EffectNow, nullptr,
        "What the domain cache holds now, in kilobytes.",
        ROW_NUMBER(CacheContainer::Instance()->GetDomainCacheSize() / 1024), nullptr, ROW_NO_CHECK },
      { "domain_cache_max_size_kb", KindInteger, ReadWrite, EffectNow, nullptr,
        "The domain cache's ceiling, in kilobytes. Held in memory only, as over COM: the built-in 10240 returns when the server starts.",
        ROW_NUMBER(CacheContainer::Instance()->GetDomainCacheMaxSize() / 1024),
        ROW_SET(CacheContainer::Instance()->SetDomainCacheMaxSize((size_t) v.number * 1024)),
        [] (const Value &v) { return CacheNotNegative("domain_cache_max_size_kb", v); } },
      { "domain_cache_ttl", KindInteger, ReadWrite, EffectNow, nullptr,
        "Seconds a domain stays cached.",
        ROW_NUMBER(Config()->GetCacheConfiguration()->GetDomainCacheTTL()),
        ROW_SET(Config()->GetCacheConfiguration()->SetDomainCacheTTL((int) v.number)),
        [] (const Value &v) { return CacheNotNegative("domain_cache_ttl", v); } },
      { "domain_hit_rate", KindInteger, ReadOnly, EffectNow, nullptr,
        "Of the domain lookups since the cache was last cleared, the percentage the cache answered.",
        ROW_NUMBER(Cache<Domain>::Instance()->GetHitRate()), nullptr, ROW_NO_CHECK },
      { "account_cache_size_kb", KindInteger, ReadOnly, EffectNow, nullptr,
        "What the account cache holds now, in kilobytes.",
        ROW_NUMBER(CacheContainer::Instance()->GetAccountCacheSize() / 1024), nullptr, ROW_NO_CHECK },
      { "account_cache_max_size_kb", KindInteger, ReadWrite, EffectNow, nullptr,
        "The account cache's ceiling, in kilobytes. Held in memory only, as over COM: the built-in 10240 returns when the server starts.",
        ROW_NUMBER(CacheContainer::Instance()->GetAccountCacheMaxSize() / 1024),
        ROW_SET(CacheContainer::Instance()->SetAccountCacheMaxSize((size_t) v.number * 1024)),
        [] (const Value &v) { return CacheNotNegative("account_cache_max_size_kb", v); } },
      { "account_cache_ttl", KindInteger, ReadWrite, EffectNow, nullptr,
        "Seconds an account stays cached.",
        ROW_NUMBER(Config()->GetCacheConfiguration()->GetAccountCacheTTL()),
        ROW_SET(Config()->GetCacheConfiguration()->SetAccountCacheTTL((int) v.number)),
        [] (const Value &v) { return CacheNotNegative("account_cache_ttl", v); } },
      { "account_hit_rate", KindInteger, ReadOnly, EffectNow, nullptr,
        "Of the account lookups since the cache was last cleared, the percentage the cache answered.",
        ROW_NUMBER(Cache<Account>::Instance()->GetHitRate()), nullptr, ROW_NO_CHECK },
      { "alias_cache_size_kb", KindInteger, ReadOnly, EffectNow, nullptr,
        "What the alias cache holds now, in kilobytes.",
        ROW_NUMBER(CacheContainer::Instance()->GetAliasCacheSize() / 1024), nullptr, ROW_NO_CHECK },
      { "alias_cache_max_size_kb", KindInteger, ReadWrite, EffectNow, nullptr,
        "The alias cache's ceiling, in kilobytes. Held in memory only, as over COM: the built-in 10240 returns when the server starts.",
        ROW_NUMBER(CacheContainer::Instance()->GetAliasCacheMaxSize() / 1024),
        ROW_SET(CacheContainer::Instance()->SetAliasCacheMaxSize((size_t) v.number * 1024)),
        [] (const Value &v) { return CacheNotNegative("alias_cache_max_size_kb", v); } },
      { "alias_cache_ttl", KindInteger, ReadWrite, EffectNow, nullptr,
        "Seconds an alias stays cached.",
        ROW_NUMBER(Config()->GetCacheConfiguration()->GetAliasCacheTTL()),
        ROW_SET(Config()->GetCacheConfiguration()->SetAliasCacheTTL((int) v.number)),
        [] (const Value &v) { return CacheNotNegative("alias_cache_ttl", v); } },
      { "alias_hit_rate", KindInteger, ReadOnly, EffectNow, nullptr,
        "Of the alias lookups since the cache was last cleared, the percentage the cache answered.",
        ROW_NUMBER(Cache<Alias>::Instance()->GetHitRate()), nullptr, ROW_NO_CHECK },
      { "distribution_list_cache_size_kb", KindInteger, ReadOnly, EffectNow, nullptr,
        "What the distribution-list cache holds now, in kilobytes.",
        ROW_NUMBER(CacheContainer::Instance()->GetDistributionListCacheSize() / 1024), nullptr, ROW_NO_CHECK },
      { "distribution_list_cache_max_size_kb", KindInteger, ReadWrite, EffectNow, nullptr,
        "The distribution-list cache's ceiling, in kilobytes. Held in memory only, as over COM: the built-in 10240 returns when the server starts.",
        ROW_NUMBER(CacheContainer::Instance()->GetDistributionListCacheMaxSize() / 1024),
        ROW_SET(CacheContainer::Instance()->SetDistributionListCacheMaxSize((size_t) v.number * 1024)),
        [] (const Value &v) { return CacheNotNegative("distribution_list_cache_max_size_kb", v); } },
      { "distribution_list_cache_ttl", KindInteger, ReadWrite, EffectNow, nullptr,
        "Seconds a distribution list stays cached.",
        ROW_NUMBER(Config()->GetCacheConfiguration()->GetDistributionListCacheTTL()),
        ROW_SET(Config()->GetCacheConfiguration()->SetDistributionListCacheTTL((int) v.number)),
        [] (const Value &v) { return CacheNotNegative("distribution_list_cache_ttl", v); } },
      { "distribution_list_hit_rate", KindInteger, ReadOnly, EffectNow, nullptr,
        "Of the distribution-list lookups since the cache was last cleared, the percentage the cache answered.",
        ROW_NUMBER(Cache<DistributionList>::Instance()->GetHitRate()), nullptr, ROW_NO_CHECK },
   };

   // Settings.MessageIndexing over COM: the switch, and the two counts the
   // Control Panel shows; a run and a clear are POSTs beside the group.
   const Row IndexingRows[] =
   {
      { "enabled", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether delivered messages are indexed for search.",
        ROW_FLAG(Config()->GetMessageIndexing()), ROW_SET(Config()->SetMessageIndexing(v.flag)), ROW_NO_CHECK },
      { "total_indexed_count", KindInteger, ReadOnly, EffectNow, nullptr,
        "How many messages the index holds.",
        [] { PersistentMessageMetaData metaData; return Number((long) metaData.GetTotalMessageCount()); }, nullptr, ROW_NO_CHECK },
      { "total_message_count", KindInteger, ReadOnly, EffectNow, nullptr,
        "How many delivered messages there are to index.",
        ROW_NUMBER(PersistentMessage::GetTotalMessageCountDelivered()), nullptr, ROW_NO_CHECK },
   };

   const Row BackupRows[] =
   {
      { "destination", KindString, ReadWrite, EffectNow, nullptr,
        "The directory a backup is written to.",
        ROW_TEXT(Config()->GetBackupDestination()), ROW_SET(Config()->SetBackupDestination(v.text)), ROW_NO_CHECK },
      { "backup_domains", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether the domains, accounts, aliases and lists are backed up.",
        ROW_FLAG(Config()->GetBackupOption(Backup::BODomains)), ROW_SET(Config()->SetBackupOption(Backup::BODomains, v.flag)), ROW_NO_CHECK },
      { "backup_messages", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether the message files are backed up.",
        ROW_FLAG(Config()->GetBackupOption(Backup::BOMessages)), ROW_SET(Config()->SetBackupOption(Backup::BOMessages, v.flag)), ROW_NO_CHECK },
      { "backup_settings", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether the server settings are backed up.",
        ROW_FLAG(Config()->GetBackupOption(Backup::BOSettings)), ROW_SET(Config()->SetBackupOption(Backup::BOSettings, v.flag)), ROW_NO_CHECK },
      { "compress", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether the backup is compressed into one file.",
        ROW_FLAG(Config()->GetBackupOption(Backup::BOCompression)), ROW_SET(Config()->SetBackupOption(Backup::BOCompression, v.flag)), ROW_NO_CHECK },
      { "log_file", KindString, ReadOnly, EffectNow, nullptr,
        "The full path of the backup log, beside the other logs.",
        ROW_TEXT(BackupLogFile()), nullptr, ROW_NO_CHECK },
      // Write-only, like the smart host's password above it: there is no read of
      // this value anywhere in the server's interfaces. What can be read is
      // whether one is set, which is the question an administrator actually has.
      { "encryption_passphrase", KindString, WriteOnly, EffectNow, nullptr,
        "The passphrase the backup archive is encrypted with (7-Zip AES-256, with the file names encrypted too). Accepted here and never emitted; an empty string clears it and the next backup is written unencrypted. At least 12 and at most 256 printable ASCII characters, not beginning or ending with a space. It is not kept in the archive or beside it, so a restore asks for it - and if it is lost, nothing in an archive written with it can be recovered by anybody.",
        nullptr,
        [] (const Value &v)
        {
           String failureReason;

           if (!Config()->SetBackupEncryptionPassphrase(v.text, failureReason))
           {
              // Reached only for a passphrase the check below accepted and the
              // machine's secret store then would not protect.
              Outcome failed;
              failed.status = 500;
              failed.message = failureReason;
              return failed;
           }

           return Applied();
        },
        [] (const Value &v) { return Configuration::CheckBackupEncryptionPassphrase(v.text); }, nullptr, PROPERTY_BACKUPENCRYPTIONPASSPHRASE },
      { "encryption_passphrase_set", KindBoolean, ReadOnly, EffectNow, nullptr,
        "Whether a backup passphrase is set, and so whether the next backup is encrypted. The passphrase itself is never read back.",
        ROW_FLAG(Config()->GetBackupEncryptionPassphraseSet()), nullptr, ROW_NO_CHECK },
   };

   // Bounces read, and the suppression list (SMTP/BounceTracker.h). In hm_settings,
   // through the same Configuration setters the COM properties call, and read at the
   // moment they matter, so every one is in force for the next message. The ranges
   // are refused before anything is applied, in the sentences InterfaceSettings uses.
   const Row BounceRows[] =
   {
      { "processing_enabled", KindBoolean, ReadWrite, EffectNow, nullptr,
        "The master switch for READING bounces: classify them, count them per address, and add to the suppression list per sending domain. Off by default, and with it off no bounce is classified, counted or stored. Note that it does not by itself stop the list being APPLIED: the entries are enforced whenever this or complaint processing is on, because the feedback loops fill the same list and an entry nobody applies is a feature that does nothing.",
        ROW_FLAG(Config()->GetBounceProcessingEnabled()), ROW_SET(Config()->SetBounceProcessingEnabled(v.flag)), ROW_NO_CHECK },
      { "hard_bounce_threshold", KindInteger, ReadWrite, EffectNow, nullptr,
        "Hard bounces in a row, with no delivery between them, that put an address on its sending domain's list. 1 to 100; 1 by default.",
        ROW_NUMBER(Config()->GetBounceHardBounceThreshold()), ROW_SET(Config()->SetBounceHardBounceThreshold((int) v.number)),
        [] (const Value &v) { return (v.number < 1 || v.number > 100) ? String(_T("hard_bounce_threshold must be between 1 and 100.")) : String(); } },
      { "mailbox_full_is_hard", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether a full mailbox (X.2.2) counts as a hard bounce. Off by default: a full mailbox is usually emptied.",
        ROW_FLAG(Config()->GetBounceMailboxFullIsHard()), ROW_SET(Config()->SetBounceMailboxFullIsHard(v.flag)), ROW_NO_CHECK },
      { "suppression_action", KindEnum, ReadWrite, EffectNow, BounceSuppressionActionWords,
        "What happens to a listed recipient: refuse (a submission is refused at RCPT with the reason, anything else is returned to the sender), drop (removed without a report), or observe (delivered, and only recorded).",
        ROW_NUMBER(Config()->GetBounceSuppressionAction()), ROW_SET(Config()->SetBounceSuppressionAction((int) v.number)), ROW_NO_CHECK },
      { "suppression_expiry_days", KindInteger, ReadWrite, EffectNow, nullptr,
        "Days after which an entry made by a bounce or a report stops applying and is swept, measured from when it was made. 0, the default, keeps entries until they are removed by hand; at most 3650.",
        ROW_NUMBER(Config()->GetBounceSuppressionExpiryDays()), ROW_SET(Config()->SetBounceSuppressionExpiryDays((int) v.number)),
        [] (const Value &v) { return (v.number < 0 || v.number > 3650) ? String(_T("suppression_expiry_days must be between 0 and 3650.")) : String(); } },
      { "read_delivery_reports", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Whether RFC 3464 delivery reports arriving for this server's senders are read, and the register they are matched against is kept. On by default, and subordinate to processing_enabled.",
        ROW_FLAG(Config()->GetBounceReadDeliveryReports()), ROW_SET(Config()->SetBounceReadDeliveryReports(v.flag)), ROW_NO_CHECK },
      { "count_retention_days", KindInteger, ReadWrite, EffectNow, nullptr,
        "Days bounce counts are kept after an address last bounced and was last delivered to, unless the address is listed. 0 keeps them; 90 by default; at most 3650.",
        ROW_NUMBER(Config()->GetBounceCountRetentionDays()), ROW_SET(Config()->SetBounceCountRetentionDays((int) v.number)),
        [] (const Value &v) { return (v.number < 0 || v.number > 3650) ? String(_T("count_retention_days must be between 0 and 3650.")) : String(); } },
      { "report_window_days", KindInteger, ReadWrite, EffectNow, nullptr,
        "Days after a message is sent that a delivery report about it is still matched. 1 to 30; 7 by default, which is also how long a BATV tag stays valid.",
        ROW_NUMBER(Config()->GetBounceReportWindowDays()), ROW_SET(Config()->SetBounceReportWindowDays((int) v.number)),
        [] (const Value &v) { return (v.number < 1 || v.number > 30) ? String(_T("report_window_days must be between 1 and 30.")) : String(); } },
   };

   // The Web Key Directory (docs/OpenPGP.md). The switch is off, and a domain and
   // an account each have to say yes as well before one key is served, so nothing
   // here alone publishes anybody. Through the same Configuration setters the COM
   // properties call, with the ranges refused in the sentences InterfaceSettings uses.
   const Row OpenPgpRows[] =
   {
      { "web_key_directory_enabled", KindBoolean, ReadWrite, EffectNow, nullptr,
        "The master switch: publish this server's accounts' OpenPGP public keys under /.well-known/openpgpkey on the web services listener. Off by default, and with it off nothing is served whatever a domain or an account says.",
        ROW_FLAG(Config()->GetOpenPGPWebKeyDirectoryEnabled()), ROW_SET(Config()->SetOpenPGPWebKeyDirectoryEnabled(v.flag)), ROW_NO_CHECK },
      { "key_maximum_bytes", KindInteger, ReadWrite, EffectNow, nullptr,
        "The largest OpenPGP key this server will accept, in bytes of the key itself rather than of its armour. 1024 to 1048576; 65536 by default, which is a key with a very large number of third-party certifications on it.",
        ROW_NUMBER(Config()->GetOpenPGPKeyMaximumBytes()), ROW_SET(Config()->SetOpenPGPKeyMaximumBytes((int) v.number)),
        [] (const Value &v) { return (v.number < 1024 || v.number > 1048576) ? String(_T("key_maximum_bytes must be between 1024 and 1048576.")) : String(); } },
      { "keys_per_account", KindInteger, ReadWrite, EffectNow, nullptr,
        "How many OpenPGP keys one account may hold at once, so that a key being rotated and the key replacing it are both findable while correspondents catch up. 1 to 64; 8 by default.",
        ROW_NUMBER(Config()->GetOpenPGPKeysPerAccount()), ROW_SET(Config()->SetOpenPGPKeysPerAccount((int) v.number)),
        [] (const Value &v) { return (v.number < 1 || v.number > 64) ? String(_T("keys_per_account must be between 1 and 64.")) : String(); } },
      { "require_address_in_user_id", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Refuse a key whose user ids do not carry the account's own address. On by default: a key published under an address it does not name is a key every correspondent will encrypt to and its owner cannot read. A server administrator can waive it for one upload.",
        ROW_FLAG(Config()->GetOpenPGPRequireAddressInUserId()), ROW_SET(Config()->SetOpenPGPRequireAddressInUserId(v.flag)), ROW_NO_CHECK },
      { "portal_enabled", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Offer OpenPGP in the webmail: making and importing a key, signing, encrypting, decrypting and verifying, all in the browser. On by default; with it off the page does not load its OpenPGP module, does not offer the key page and does not open a key store route, and an account that already has keys keeps them in the database untouched.",
        ROW_FLAG(Config()->GetOpenPGPPortalEnabled()), ROW_SET(Config()->SetOpenPGPPortalEnabled(v.flag)), ROW_NO_CHECK },
      { "lookup_wkd", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Look a correspondent's key up in their own domain's Web Key Directory when the webmail asks for one - openpgpkey.<domain> first, then the domain itself, over HTTPS only and never to an address inside this network. On by default: the request goes to the correspondent's own mail domain, which is about to learn of the message anyway.",
        ROW_FLAG(Config()->GetOpenPGPLookupWkd()), ROW_SET(Config()->SetOpenPGPLookupWkd(v.flag)), ROW_NO_CHECK },
      { "lookup_keyserver", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Ask the keyserver when the Web Key Directory has no key. OFF by default, and deliberately: a keyserver is a third party who is nobody's mail server, and asking it tells it which of this server's people are about to write to which address.",
        ROW_FLAG(Config()->GetOpenPGPLookupKeyserver()), ROW_SET(Config()->SetOpenPGPLookupKeyserver(v.flag)), ROW_NO_CHECK },
      { "keyserver_url", KindString, ReadWrite, EffectNow, nullptr,
        "The keyserver asked when lookup_keyserver is on: its base address, with the HKP path /vks/v1/by-email/<address> appended. https://keys.openpgp.org by default, which serves only addresses whose owner confirmed them. https, or http to a keyserver on this machine - a lookup of somebody else's domain is never made in the clear, but a keyserver on loopback has nothing between it and this server to protect the request from.",
        ROW_TEXT(Config()->GetOpenPGPKeyserverUrl()), ROW_SET(Config()->SetOpenPGPKeyserverUrl(v.text)),
        [] (const Value &v) { String url = v.text; url.Trim(); url.MakeLower();
                              if (url.IsEmpty() || url.StartsWith(_T("https://")))
                                 return String();
                              bool https = false; AnsiString host, port, path;
                              if (HttpsClient::ParseUrl(AnsiString(url), https, host, port, path) && !https && HttpsClient::IsLoopbackHost(host))
                                 return String();
                              return String(_T("keyserver_url is an https address, an http address on this machine, or empty.")); } },
      { "lookup_timeout", KindInteger, ReadWrite, EffectNow, nullptr,
        "Seconds a key lookup may take before it is given up on. 2 to 30; 10 by default. A webmail request waits on it, so it is short.",
        ROW_NUMBER(Config()->GetOpenPGPLookupTimeout()), ROW_SET(Config()->SetOpenPGPLookupTimeout((int) v.number)),
        [] (const Value &v) { return (v.number < 2 || v.number > 30) ? String(_T("lookup_timeout must be between 2 and 30.")) : String(); } },
      { "protected_message_days", KindInteger, ReadWrite, EffectNow, nullptr,
        "How many days a password-protected message - one the webmail encrypted to a password for a correspondent with no key - is kept for its reading page before the ciphertext goes. The default a sender gets and the most a sender may ask for. 1 to 365; 30 by default.",
        ROW_NUMBER(Config()->GetProtectedMessageDays()), ROW_SET(Config()->SetProtectedMessageDays((int) v.number)),
        [] (const Value &v) { return (v.number < 1 || v.number > 365) ? String(_T("protected_message_days must be between 1 and 365.")) : String(); } },
      { "zero_access_allowed", KindBoolean, ReadWrite, EffectNow, nullptr,
        "Allow an account to become a zero-access mailbox from the webmail's Security page: every message delivered to it is then encrypted to the account's own OpenPGP key after the filters have run, and this server cannot read, search, export or recover its mail; a forgotten password without the recovery key is the mail lost. OFF by default, and allowing it switches nobody on - each person makes that choice, and is told the consequences before they do (docs/ZeroAccess.md). Turning it off again stops new opt-ins and changes nothing for a mailbox already switched.",
        ROW_FLAG(Config()->GetZeroAccessAllowed()), ROW_SET(Config()->SetZeroAccessAllowed(v.flag)), ROW_NO_CHECK },
   };

   // Web Push (Common/Util/WebPush.h). Seven rows in hm_settings, read at the
   // moment they matter so that every one is in force for the next push.
   //
   // The two keys are the reason this group has both an access kind that is never
   // emitted and one that is never accepted. The public key is READ-ONLY here: it
   // is derived from the private one and never set on its own, because a pair
   // whose halves disagree is a feature that fails silently at a push service. The
   // private key is WRITE-ONLY: it is accepted so that an administrator can carry
   // an installation to another machine, and it is never emitted by this group, by
   // the COM interface, by the configuration export or by the audit trail.
   const Row PushRows[] =
   {
      { "enabled", KindBoolean, ReadWrite, EffectNow, nullptr,
        "The master switch. Off by default, and with it off nothing is queued, nothing is signed and no push service is ever reached - whatever subscriptions exist.",
        ROW_FLAG(Config()->GetWebPushEnabled()), ROW_SET(Config()->SetWebPushEnabled(v.flag)), ROW_NO_CHECK },
      { "subject", KindString, ReadWrite, EffectNow, nullptr,
        "The contact a push service may use when this server's subscriptions are failing: a mailto: or https: URI, sent as the sub claim of every VAPID token. Empty means mailto:postmaster@ the server's host name.",
        ROW_TEXT(Config()->GetWebPushSubject()), ROW_SET(Config()->SetWebPushSubject(v.text)),
        [] (const Value &v) { String s = v.text; s.Trim(); s.MakeLower();
                              return (!s.IsEmpty() && !s.StartsWith(_T("mailto:")) && !s.StartsWith(_T("https://")))
                                 ? String(_T("subject is a mailto: or https: address, or empty.")) : String(); } },
      { "time_to_live_seconds", KindInteger, ReadWrite, EffectNow, nullptr,
        "How long a push service should hold a notification for a device that is offline. 0 means deliver it now or not at all; 86400 by default; at most 2419200 (four weeks).",
        ROW_NUMBER(Config()->GetWebPushTimeToLiveSeconds()), ROW_SET(Config()->SetWebPushTimeToLiveSeconds((int) v.number)),
        [] (const Value &v) { return (v.number < 0 || v.number > 2419200) ? String(_T("time_to_live_seconds must be between 0 and 2419200.")) : String(); } },
      { "max_devices_per_account", KindInteger, ReadWrite, EffectNow, nullptr,
        "How many devices one account may have notifications on at once. 1 to 50; 10 by default.",
        ROW_NUMBER(Config()->GetWebPushMaxPerAccount()), ROW_SET(Config()->SetWebPushMaxPerAccount((int) v.number)),
        [] (const Value &v) { return (v.number < 1 || v.number > 50) ? String(_T("max_devices_per_account must be between 1 and 50.")) : String(); } },
      { "max_attempts", KindInteger, ReadWrite, EffectNow, nullptr,
        "How many times a push to one device is attempted before that device is dead-lettered. 1 to 20; 5 by default, as the alert webhooks are.",
        ROW_NUMBER(Config()->GetWebPushMaxAttempts()), ROW_SET(Config()->SetWebPushMaxAttempts((int) v.number)),
        [] (const Value &v) { return (v.number < 1 || v.number > 20) ? String(_T("max_attempts must be between 1 and 20.")) : String(); } },
      { "vapid_public_key", KindString, ReadOnly, EffectNow, nullptr,
        "The public half of this server's VAPID key pair, base64url. Every page that subscribes needs it, so it is public by construction. Derived from the private half; POST /api/v1/settings/push/vapid makes a new pair.",
        ROW_TEXT(Config()->GetWebPushVapidPublicKey()), nullptr, ROW_NO_CHECK },
      { "vapid_private_key", KindString, WriteOnly, EffectNow, nullptr,
        "The private half, base64url of the 32-octet P-256 scalar. Accepted here so an installation can be carried to another machine, and never emitted by anything. Setting it derives the public half; setting it empty clears both and stops every existing subscription working.",
        nullptr,
        [] (const Value &v) { String refusal; String key = v.text;
                              return Config()->SetWebPushVapidKeys(key, refusal) ? Applied() : RefusedText(refusal); },
        [] (const Value &v) { String key = v.text; key.Trim();
                              if (key.IsEmpty()) return String();
                              AnsiString derived; String error;
                              return WebPush::PublicKeyOf(AnsiString(key), derived, error) ? String() : error; },
        nullptr, PROPERTY_WEB_PUSH_VAPID_PRIVATE_KEY },
   };

   // Complaints, read: the feedback loops (SMTP/ComplaintTracker.h). In
   // hm_settings, through the same Configuration setters the COM properties call,
   // and read at the moment they matter, so every one is in force for the next
   // report. The ranges are refused before anything is applied, in the sentences
   // InterfaceSettings uses.
   const Row ComplaintRows[] =
   {
      { "processing_enabled", KindBoolean, ReadWrite, EffectNow, nullptr,
        "The master switch: read the RFC 5965 abuse reports the providers post to the complaint mailbox, record them per domain and per account, and list an address that complains. Off by default, and with it off nothing is read, counted or written.",
        ROW_FLAG(Config()->GetComplaintProcessingEnabled()), ROW_SET(Config()->SetComplaintProcessingEnabled(v.flag)), ROW_NO_CHECK },
      { "mailbox", KindString, ReadWrite, EffectNow, nullptr,
        "The address this server's feedback loops are registered against, such as fbl@example.com, or several separated by commas. ONLY a report delivered here is read as a complaint: arriving where the provider was told to send it is the only thing that tells a report from a forgery, since anybody can write a message that looks like one. Each address must be one this server hosts. Empty, the default, means no complaint is ever read.",
        ROW_TEXT(Config()->GetComplaintMailbox()), ROW_SET(Config()->SetComplaintMailbox(v.text)),
        [] (const Value &v) { return ComplaintMailboxProblem(v.text); } },
      { "suppression_threshold", KindInteger, ReadWrite, EffectNow, nullptr,
        "Complaints about one address that put it on its sending domain's suppression list - the same list the bounces feed, with its own reason on the entry. 0 by default, which records and counts every report and lists nobody: the only thing that tells a genuine report from a forged one is that it arrived at the mailbox, so read some real entries before letting the list act. 1 is the value to move to - a reader who pressed the spam button received the message and did not want it. At most 100.",
        ROW_NUMBER(Config()->GetComplaintSuppressionThreshold()), ROW_SET(Config()->SetComplaintSuppressionThreshold((int) v.number)),
        [] (const Value &v) { return (v.number < 0 || v.number > 100) ? String(_T("suppression_threshold must be between 0 and 100.")) : String(); } },
      { "retention_days", KindInteger, ReadWrite, EffectNow, nullptr,
        "Days a complaint is kept. 180 by default; 0 keeps them for ever; at most 3650. The daily tally behind the complaint rate is swept on the same clock, but never below the seven days of the rate window, so a retention shorter than the window cannot empty the window it is measured over. A suppression list entry a complaint made is not removed by this - it has its own expiry, in the bounce settings.",
        ROW_NUMBER(Config()->GetComplaintRetentionDays()), ROW_SET(Config()->SetComplaintRetentionDays((int) v.number)),
        [] (const Value &v) { return (v.number < 0 || v.number > 3650) ? String(_T("retention_days must be between 0 and 3650.")) : String(); } },
   };

#undef ROW_TEXT
#undef ROW_NUMBER
#undef ROW_FLAG
#undef ROW_SET
#undef ROW_NO_CHECK

   // The one Web Push route that is not a settings group: making a key pair.
   const char *PushVapidPath =
      ",\"/api/v1/settings/push/vapid\":{\"post\":{\"summary\":\"Make a new VAPID key pair\","
      "\"description\":\"Generates a fresh P-256 key pair for Web Push and stores both halves. The answer carries vapid_public_key; the private half never leaves the server, and is not readable over any interface. Every subscription made under the previous pair stops working at once and each device must be asked again, so this is not a routine operation - it is what to do when the private key may have been copied. Server-wide; refused for domain-restricted keys.\","
      "\"responses\":{\"200\":{\"description\":\"vapid_public_key\"},\"500\":{\"description\":\"The key pair could not be generated\"}}}}";

   const Group ServerGroup = { "server", "/api/v1/settings", ServerRows, sizeof(ServerRows) / sizeof(ServerRows[0]) };
   const Group AntiSpamGroup = { "antispam", "/api/v1/settings/antispam", AntiSpamRows, sizeof(AntiSpamRows) / sizeof(AntiSpamRows[0]) };
   const Group AntiVirusGroup = { "antivirus", "/api/v1/settings/antivirus", AntiVirusRows, sizeof(AntiVirusRows) / sizeof(AntiVirusRows[0]) };
   const Group LoggingGroup = { "logging", "/api/v1/settings/logging", LoggingRows, sizeof(LoggingRows) / sizeof(LoggingRows[0]) };
   const Group ComplaintGroup = { "complaints", "/api/v1/settings/complaints", ComplaintRows, sizeof(ComplaintRows) / sizeof(ComplaintRows[0]) };
   const Group DirectoriesGroup = { "directories", "/api/v1/settings/directories", DirectoriesRows, sizeof(DirectoriesRows) / sizeof(DirectoriesRows[0]) };
   const Group ScriptingGroup = { "scripting", "/api/v1/settings/scripting", ScriptingRows, sizeof(ScriptingRows) / sizeof(ScriptingRows[0]) };
   const Group BackupGroup = { "backup", "/api/v1/settings/backup", BackupRows, sizeof(BackupRows) / sizeof(BackupRows[0]) };
   const Group CacheGroup = { "cache", "/api/v1/settings/cache", CacheRows, sizeof(CacheRows) / sizeof(CacheRows[0]) };
   const Group IndexingGroup = { "indexing", "/api/v1/settings/indexing", IndexingRows, sizeof(IndexingRows) / sizeof(IndexingRows[0]) };
   const Group BounceGroup = { "bounces", "/api/v1/settings/bounces", BounceRows, sizeof(BounceRows) / sizeof(BounceRows[0]) };
   const Group PushGroup = { "push", "/api/v1/settings/push", PushRows, sizeof(PushRows) / sizeof(PushRows[0]) };
   const Group OpenPgpGroup = { "openpgp", "/api/v1/settings/openpgp", OpenPgpRows, sizeof(OpenPgpRows) / sizeof(OpenPgpRows[0]) };

   // ---------------------------------------------------------------------
   // The two private helpers of RestApiServer these functions need, handed
   // in by the member functions that own the right to name them. Nothing
   // here duplicates an escape or a response line.
   // ---------------------------------------------------------------------

   struct Bridge
   {
      AnsiString (*escape)(const AnsiString &value);
      HttpResponse (*respond)(int statusCode, const AnsiString &body, const AnsiString &extraHeaders);
   };

   AnsiString Utf8(const String &value)
   {
      AnsiString utf8;
      Unicode::WideToMultiByte(value, utf8);
      return utf8;
   }

   const Row *FindRow(const Group &group, const std::string &key)
   {
      for (size_t i = 0; i < group.count; i++)
      {
         if (key == group.rows[i].key)
            return &group.rows[i];
      }
      return nullptr;
   }

   const char *WordFor(const Row &row, long value)
   {
      for (const Word *word = row.words; word && word->word; word++)
      {
         if (word->value == value)
            return word->word;
      }
      return nullptr;
   }

   AnsiString WordList(const Row &row)
   {
      AnsiString list;
      for (const Word *word = row.words; word && word->word; word++)
      {
         if (!list.IsEmpty())
            list += ", ";
         list += word->word;
      }
      return list;
   }

   // The three configuration objects a row may reach. Null only before
   // Configuration::Load has run, which is before this listener exists; the
   // guard costs nothing and turns a startup race into a 503 rather than a crash.
   bool ConfigurationLoaded()
   {
      return Smtp() && Imap() && Pop3();
   }

   // ---------------------------------------------------------------------
   // The settings the environment manages (docs/EnvironmentConfiguration.md).
   // ---------------------------------------------------------------------

   // What --apply-environment last recorded, read once per request: the settings
   // the environment manages, and the ones whose value came from a file - which
   // stay withheld after the variable is gone, until an administrator writes a
   // new value (EnvironmentSettings::IsSecretOrigin).
   struct Managed
   {
      std::vector<EnvironmentSettings::Entry> entries;
      std::vector<EnvironmentSettings::Entry> secrets;

      Managed()
      {
         EnvironmentSettings::ReadManaged(entries);
         EnvironmentSettings::ReadSecretOrigins(secrets);
      }

      static const EnvironmentSettings::Entry *In(const std::vector<EnvironmentSettings::Entry> &list, EnvironmentSettings::Store store, const String &name)
      {
         for (const EnvironmentSettings::Entry &entry : list)
         {
            if (entry.store == store && entry.name.CompareNoCase(name) == 0)
               return &entry;
         }
         return nullptr;
      }

      const EnvironmentSettings::Entry *Find(EnvironmentSettings::Store store, const String &name) const
      {
         return In(entries, store, name);
      }

      // Whether the value is withheld from every answer: a file supplied it, now or
      // before, and nobody has written another since.
      bool Withheld(EnvironmentSettings::Store store, const String &name) const
      {
         const EnvironmentSettings::Entry *managed = Find(store, name);
         return (managed && managed->source == EnvironmentSettings::SourceFile) || In(secrets, store, name) != nullptr;
      }

      bool Empty() const
      {
         return entries.empty() && secrets.empty();
      }
   };

   // A row's value, and the hm_settings names its getter read to produce it. The
   // getter is the code COM's getter runs, so the one name it reads is the name
   // the row holds - which is how a row is known to be a setting the environment
   // manages without a table of names kept beside the rows.
   Value ReadRow(const Row &row, std::vector<String> &read)
   {
      PropertySet::ReadRecorder recorder;
      Value value = row.get();
      read = recorder.Names();
      return value;
   }

   // The entry a row's value is set by, or null: the key a row kept in the
   // settings store names, or the one property its getter reads. A row whose
   // getter reads more than one property is not one setting and is tied to none
   // here; a change to it that would move a managed property is still refused by
   // the store as it is written. (A row that is one bit of a property - each TLS
   // option is a bit of tlsoptions - reads that property alone, and is managed
   // with it.)
   // A write-only row names the property its setter writes (Row::property).
   bool SettingOf(const Row &row, const std::vector<String> &read, EnvironmentSettings::Store &store, String &name)
   {
      if (row.ini_key)
      {
         store = EnvironmentSettings::StoreIni;
         name = row.ini_key;
         return true;
      }

      store = EnvironmentSettings::StoreDatabase;

      if (row.access == WriteOnly && row.property)
      {
         name = row.property;
         return true;
      }

      if (read.size() == 1)
      {
         name = read[0];
         return true;
      }

      return false;
   }

   const EnvironmentSettings::Entry *EnvironmentOf(const Row &row, const std::vector<String> &read, const Managed &managed)
   {
      if (managed.entries.empty())
         return nullptr;

      EnvironmentSettings::Store store = EnvironmentSettings::StoreDatabase;
      String name;
      if (!SettingOf(row, read, store, name))
         return nullptr;

      return managed.Find(store, name);
   }

   // Whether the row's value is withheld: a file of the environment supplied it
   // (Managed::Withheld).
   bool WithheldOf(const Row &row, const std::vector<String> &read, const Managed &managed)
   {
      if (managed.Empty())
         return false;

      EnvironmentSettings::Store store = EnvironmentSettings::StoreDatabase;
      String name;
      if (!SettingOf(row, read, store, name))
         return false;

      return managed.Withheld(store, name);
   }

   bool SameValue(const Row &row, const Value &left, const Value &right)
   {
      switch (row.kind)
      {
      case KindString:
         return left.text == right.text;
      case KindBoolean:
         return left.flag == right.flag;
      case KindInteger:
      case KindEnum:
         return left.number == right.number;
      }
      return false;
   }

   // ---------------------------------------------------------------------
   // GET: the group as one flat object, in table order.
   // ---------------------------------------------------------------------

   // The setting with levels a group's row is the server level of, or null: a
   // row whose key is the setting's name, and the OpenPGP group's switch, which
   // is named for its group (docs/SettingLevels.md).
   const EffectiveSetting::Definition *LevelsOf(const Group &group, const char *key)
   {
      if (AnsiString(group.name) == "openpgp" && AnsiString(key) == "portal_enabled")
         return EffectiveSetting::Find(_T("openpgp_portal_enabled"));

      const EffectiveSetting::Definition *definition = EffectiveSetting::Find(String(key));
      if (!definition || AnsiString(definition->name) != AnsiString(key))
         return nullptr;

      return definition;
   }

   AnsiString GroupJson(const Group &group, const Bridge &bridge)
   {
      AnsiString json = "{";
      bool first = true;

      const Managed managed;

      for (size_t i = 0; i < group.count; i++)
      {
         const Row &row = group.rows[i];
         if (row.access == WriteOnly)
         {
            // A write-only row has no value to answer, but a password the
            // environment sets says so, so that a page draws no box for it.
            const std::vector<String> none;
            const EnvironmentSettings::Entry *environment = EnvironmentOf(row, none, managed);
            if (environment)
            {
               if (!first)
                  json += ",";
               first = false;

               json += "\"";
               json += row.key;
               json += "_environment\":\"";
               json += EnvironmentSettings::SourceWord(environment->source);
               json += "\"";
            }
            continue;
         }

         std::vector<String> read;
         Value value = ReadRow(row, read);

         // A value a file of the environment supplied is a Secret's, and is not
         // answered over REST - while the variable sets it, and after, until an
         // administrator writes another: (set) for text that has one, null
         // otherwise.
         const EnvironmentSettings::Entry *environment = EnvironmentOf(row, read, managed);
         const bool masked = WithheldOf(row, read, managed);

         if (!first)
            json += ",";
         first = false;

         json += "\"";
         json += row.key;
         json += "\":";

         if (masked)
         {
            if (row.kind == KindString)
               json += value.text.IsEmpty() ? "\"\"" : "\"(set)\"";
            else
               json += "null";
         }
         else switch (row.kind)
         {
         case KindString:
            json += "\"" + bridge.escape(Utf8(value.text)) + "\"";
            break;
         case KindInteger:
            {
               AnsiString number;
               number.Format("%ld", value.number);
               json += number;
            }
            break;
         case KindBoolean:
            json += value.flag ? "true" : "false";
            break;
         case KindEnum:
            {
               // A stored value no word covers can only come from a
               // hand-edited row; null says so without inventing a word.
               const char *word = WordFor(row, value.number);
               if (word)
                  json += "\"" + AnsiString(word) + "\"";
               else
                  json += "null";
            }
            break;
         }

         // The server level of a setting a domain or an account may hold its
         // own value of says so: <key>_levels, the deepest level it may be set
         // at. Answered, never taken.
         const EffectiveSetting::Definition *levels = LevelsOf(group, row.key);
         if (levels)
         {
            json += ",\"";
            json += row.key;
            json += "_levels\":\"";
            json += EffectiveSetting::LevelsWord(levels->levels);
            json += "\"";
         }

         // A key the environment sets says so, and where the value came from:
         // <key>_environment, variable or file. Answered, never taken - a PUT that
         // would change the key is refused with 409 (docs/EnvironmentConfiguration.md).
         if (environment)
         {
            json += ",\"";
            json += row.key;
            json += "_environment\":\"";
            json += EnvironmentSettings::SourceWord(environment->source);
            json += "\"";
         }
      }

      json += "}";
      return json;
   }

   // ---------------------------------------------------------------------
   // PUT.
   // ---------------------------------------------------------------------

   struct Change
   {
      const Row *row;
      Value value;
   };

   // The settings whose values are judged together. The sandbox's switch may
   // not be left on with either address empty (Sandbox::SwitchProblem), and a
   // request may set all three at once, so each is taken from the request when
   // it names it and from the configuration when it does not. A request that
   // names none of the three is not judged by it.
   String CrossRowProblem(const std::vector<Change> &changes)
   {
      bool named = false;
      bool enabled = Config()->GetSandboxEnabled();
      String submitUrl = Config()->GetSandboxSubmitUrl();
      String verdictUrl = Config()->GetSandboxVerdictUrl();

      for (const Change &change : changes)
      {
         const std::string key = change.row->key;

         if (key == "sandbox_enabled")
         {
            enabled = change.value.flag;
            named = true;
         }
         else if (key == "sandbox_submit_url")
         {
            submitUrl = change.value.text;
            named = true;
         }
         else if (key == "sandbox_verdict_url")
         {
            verdictUrl = change.value.text;
            named = true;
         }
      }

      if (!named)
         return String();

      return Sandbox::SwitchProblem(enabled, submitUrl, verdictUrl, _T("sandbox_enabled"));
   }

   HttpResponse Refusal(const Bridge &bridge, int status, const AnsiString &sentence)
   {
      return bridge.respond(status, "{\"error\":\"" + bridge.escape(sentence) + "\"}", "");
   }

   // A refusal that names the caller's key: the key is the caller's own text,
   // escaped, and cut short so a hostile body cannot make the answer or the
   // log line arbitrarily long.
   AnsiString Named(const std::string &key, const char *rest)
   {
      AnsiString shown = AnsiString(key.substr(0, 100).c_str());
      if (key.size() > 100)
         shown += "...";
      return shown + " " + rest;
   }

   // The member's JSON value as the row's type, or the sentence that says why not.
   bool ReadValue(const Row &row, const std::string &key, const JsonValue &member, Value &value, AnsiString &problem)
   {
      switch (row.kind)
      {
      case KindString:
         if (!member.IsString())
         {
            problem = Named(key, "must be a string");
            return false;
         }
         if (!Unicode::MultiByteToWide(AnsiString(member.AsString().c_str()), value.text))
         {
            problem = Named(key, "is not valid UTF-8");
            return false;
         }
         return true;

      case KindInteger:
         {
            // A JSON number with a fraction is not an integer; one with an
            // exponent is, when it comes out whole. The range is the COM
            // property's, a 32-bit long, whatever the platform's long is.
            if (!member.IsNumber())
            {
               problem = Named(key, "must be an integer");
               return false;
            }
            __int64 whole = member.AsInt64();
            if ((double) whole != member.AsNumber())
            {
               problem = Named(key, "must be an integer");
               return false;
            }
            if (whole < INT_MIN || whole > INT_MAX)
            {
               problem = Named(key, "is out of range");
               return false;
            }
            value.number = (long) whole;
            return true;
         }

      case KindBoolean:
         if (!member.IsBool())
         {
            problem = Named(key, "must be true or false");
            return false;
         }
         value.flag = member.AsBool();
         return true;

      case KindEnum:
         {
            if (member.IsString())
            {
               for (const Word *word = row.words; word && word->word; word++)
               {
                  if (member.AsString() == word->word)
                  {
                     value.number = word->value;
                     return true;
                  }
               }
            }
            problem = Named(key, "must be one of ") + WordList(row);
            return false;
         }
      }

      problem = Named(key, "has an unknown kind");
      return false;
   }

   HttpResponse GroupPut(const Group &group, const AnsiString &requestBody, const Bridge &bridge)
   {
      if (!ConfigurationLoaded())
         return Refusal(bridge, 503, "the server's configuration is not loaded");

      JsonValue document;
      std::string parseError;
      if (!JsonValue::Parse(std::string(requestBody.c_str(), requestBody.GetLength()), document, parseError) || !document.IsObject())
         return Refusal(bridge, 400, "the body must be a JSON object");

      // Resolve everything first: every key to its row, every value to its
      // type, every value through its row's own check. The first problem
      // is the answer, and nothing has been applied when it is given.
      std::vector<Change> changes;

      const std::vector<std::pair<std::string, JsonValue> > &members = document.Members();
      for (size_t i = 0; i < members.size(); i++)
      {
         const std::string &key = members[i].first;

         const Row *row = FindRow(group, key);
         if (!row)
         {
            // <key>_levels is answered beside a setting with levels, and is
            // what the setting is, not a value of it.
            const std::string suffix = "_levels";
            if (key.size() > suffix.size() && key.compare(key.size() - suffix.size(), suffix.size(), suffix) == 0)
            {
               const Row *base = FindRow(group, key.substr(0, key.size() - suffix.size()));
               if (base && LevelsOf(group, base->key))
                  return Refusal(bridge, 400, Named(key, "is read-only: it says how deep the setting may be set, which is the server's to know"));
            }

            // <key>_environment is answered beside a key the environment sets, and
            // says where the value came from; it is not a value either.
            const std::string environmentSuffix = "_environment";
            if (key.size() > environmentSuffix.size() && key.compare(key.size() - environmentSuffix.size(), environmentSuffix.size(), environmentSuffix) == 0 &&
                FindRow(group, key.substr(0, key.size() - environmentSuffix.size())))
               return Refusal(bridge, 400, Named(key, "is read-only: it says the server's environment sets the key, which only the environment changes"));

            return Refusal(bridge, 400, Named(key, "is not a setting in this group"));
         }

         if (row->access == ReadOnly)
            return Refusal(bridge, 400, Named(key, "is read-only"));

         Change change;
         change.row = row;

         AnsiString problem;
         if (!ReadValue(*row, key, members[i].second, change.value, problem))
            return Refusal(bridge, 400, problem);

         changes.push_back(change);
      }

      // A key the environment sets is refused when the body would change it, before
      // anything is applied: the next start writes the environment's value back, so
      // the change would be accepted and lost. A key sent with the value it already
      // has is taken. A write-only key the environment sets is refused whatever it
      // is sent with: its value is not read back to be compared, and answering
      // differently for the right value would tell a caller the secret.
      {
         const Managed managed;
         if (!managed.entries.empty())
         {
            for (size_t i = 0; i < changes.size(); i++)
            {
               const Row &row = *changes[i].row;

               std::vector<String> read;
               const EnvironmentSettings::Entry *environment = nullptr;
               if (row.access == WriteOnly)
               {
                  environment = EnvironmentOf(row, read, managed);
                  if (!environment)
                     continue;
               }
               else
               {
                  if (!row.get)
                     continue;

                  const Value current = ReadRow(row, read);
                  environment = EnvironmentOf(row, read, managed);
                  if (!environment || SameValue(row, current, changes[i].value))
                     continue;
               }

               LOG_APPLICATION("RestApi: Refused to change " + String(group.name) + " setting " + String(row.key) + ": the environment sets it (" + environment->variable + ").");
               return Refusal(bridge, 409, Utf8(EnvironmentSettings::RefusalSentence(String(row.key), *environment)));
            }
         }
      }

      for (size_t i = 0; i < changes.size(); i++)
      {
         if (!changes[i].row->check)
            continue;

         String refusal = changes[i].row->check(changes[i].value);
         if (!refusal.IsEmpty())
         {
            LOG_APPLICATION("RestApi: Refused to change " + String(group.name) + " setting " + String(changes[i].row->key) + ": " + refusal);
            return Refusal(bridge, 400, Utf8(refusal));
         }
      }

      // What no one row can judge alone, against what the request would leave.
      {
         String refusal = CrossRowProblem(changes);
         if (!refusal.IsEmpty())
         {
            LOG_APPLICATION("RestApi: Refused to change " + String(group.name) + " settings: " + refusal);
            return Refusal(bridge, 400, Utf8(refusal));
         }
      }

      // Apply, the rows with a check first: those are the ones whose setter
      // can still say no (a delimiter whose rewrite failed, an INI file that
      // could not be written), and a no from them must leave the rest untouched.
      String applied;

      for (int pass = 0; pass < 2; pass++)
      {
         for (size_t i = 0; i < changes.size(); i++)
         {
            const Row &row = *changes[i].row;
            bool checked = row.check != nullptr;
            if ((pass == 0) != checked)
               continue;

            Outcome outcome;
            try
            {
               outcome = row.set(changes[i].value);
            }
            catch (const EnvironmentSettings::Refusal &refusal)
            {
               // The store's own refusal, for a row the check above could not tie to
               // one setting (a getter that reads several). Said honestly: what was
               // applied before it stays.
               AnsiString sentence = Utf8(refusal.Sentence());
               if (!applied.IsEmpty())
                  sentence += " The keys before it in this request were applied: " + Utf8(applied) + ".";

               LOG_APPLICATION("RestApi: Refused to change " + String(group.name) + " setting " + String(row.key) + ": the environment sets it.");
               return Refusal(bridge, 409, sentence);
            }

            if (outcome.status != 0)
            {
               LOG_APPLICATION("RestApi: Failed to change " + String(group.name) + " setting " + String(row.key) + ": " + outcome.message);
               return Refusal(bridge, outcome.status, Utf8(outcome.message));
            }

            if (!applied.IsEmpty())
               applied += ", ";
            applied += row.key;
         }
      }

      // The keys and never the values: a value may be the relayer password.
      if (!applied.IsEmpty())
         LOG_APPLICATION("RestApi: Changed " + String(group.name) + " settings: " + applied + ".");

      return bridge.respond(200, GroupJson(group, bridge), "");
   }

   HttpResponse GroupGet(const Group &group, const Bridge &bridge)
   {
      if (!ConfigurationLoaded())
         return Refusal(bridge, 503, "the server's configuration is not loaded");

      return bridge.respond(200, GroupJson(group, bridge), "");
   }

   // ---------------------------------------------------------------------
   // The OpenAPI paths, from the same tables.
   // ---------------------------------------------------------------------

   // The sentence a key's mark stands for, wherever the document says when an
   // ini-backed value applies (docs/SettingsApply.md).
   const char *IniAppliesSentence(const AnsiString &applies)
   {
      if (applies == "reinitialize")
         return "Stored at once and applied at the next reinitialise (POST /api/v1/server/reinitialize); pending until then.";
      if (applies == "restart")
         return "Stored at once and applied when the service next starts; pending until then.";
      return "Applies when it is saved.";
   }

   AnsiString OpenApiProperties(const Group &group, const Bridge &bridge)
   {
      AnsiString properties;

      for (size_t i = 0; i < group.count; i++)
      {
         const Row &row = group.rows[i];

         if (!properties.IsEmpty())
            properties += ",";

         AnsiString type;
         switch (row.kind)
         {
         case KindString:
         case KindEnum:
            type = "string";
            break;
         case KindInteger:
            type = "integer";
            break;
         case KindBoolean:
            type = "boolean";
            break;
         }

         AnsiString description = row.description;
         AnsiString applies;
         if (row.ini_key)
         {
            // A row kept in hm_inisettings says what the baseline says of its key,
            // and carries the word as x-applies for a client to show beside it.
            String mark;
            String spelling;
            if (IniFileSettings::GetApplies(row.ini_key, mark, spelling))
            {
               applies = AnsiString(mark);
               description += " " + AnsiString(IniAppliesSentence(applies));
            }
         }
         else
         {
            switch (row.effect)
            {
            case EffectNow:
               break;
            case EffectRestart:
               description += " Takes effect when the server restarts.";
               break;
            case EffectStored:
               description += " Stored for the Control Panel; nothing in this server reads it.";
               break;
            }
         }

         properties += "\"";
         properties += row.key;
         properties += "\":{\"type\":\"" + type + "\",\"description\":\"" + bridge.escape(description) + "\"";

         if (row.kind == KindEnum)
         {
            properties += ",\"enum\":[";
            for (const Word *word = row.words; word && word->word; word++)
            {
               if (word != row.words)
                  properties += ",";
               properties += "\"";
               properties += word->word;
               properties += "\"";
            }
            properties += "]";
         }

         if (row.access == ReadOnly)
            properties += ",\"readOnly\":true";
         if (row.access == WriteOnly)
            properties += ",\"writeOnly\":true";
         if (!applies.IsEmpty())
            properties += ",\"x-applies\":\"" + applies + "\"";

         // A setting a domain or an account may hold its own value of: the
         // deepest level, as x-levels, and the name it has below the server
         // where that is not the key (docs/SettingLevels.md).
         const EffectiveSetting::Definition *levels = LevelsOf(group, row.key);
         if (levels)
         {
            properties += ",\"x-levels\":\"";
            properties += EffectiveSetting::LevelsWord(levels->levels);
            properties += "\",\"x-level-name\":\"";
            properties += levels->name;
            properties += "\"";
         }

         properties += "}";
      }

      return properties;
   }

   AnsiString OpenApiPath(const Group &group, const char *getSummary, const char *putSummary, const Bridge &bridge)
   {
      AnsiString path = ",\"";
      path += group.path;
      path += "\":{";

      path += "\"get\":{\"summary\":\"";
      path += getSummary;
      path += "\",\"description\":\"Every key the PUT lists, as the server holds it now; the write-only ones are left out. A key a domain or an account may hold its own value of (x-levels in the PUT's schema) is followed by <key>_levels, the deepest level it may be set at - domain or account - which is answered and never taken; GET /api/v1/settings/levels lists those settings and docs/SettingLevels.md says how the levels resolve. A key the server's environment sets (hmailserver --apply-environment; GET /api/v1/settings/environment lists them, docs/EnvironmentConfiguration.md) is followed by <key>_environment, variable or file - where the value came from - which is answered and never taken - a write-only key the environment sets is answered with its mark alone - and a value a file supplied is answered as (set), or null where the key is not text, while the file sets it and after, until an administrator writes another value. Server-wide; refused for domain-restricted keys.\","
              "\"responses\":{\"200\":{\"description\":\"The group as one flat object\"}}},";

      path += "\"put\":{\"summary\":\"";
      path += putSummary;
      path += "\",\"description\":\"Body: any subset of the writable keys below, applied through the setters the Control Panel uses. "
              "Everything is checked before anything is applied: an unknown key, a value of the wrong type, a read-only key, or a value the setting refuses is a 400 whose error names the key or carries the setting's own sentence, and nothing changes. "
              "A change takes effect at once unless the key's description says it waits for a restart; the response always shows the stored value. "
              "A key the server's environment sets (<key>_environment in the GET) is refused with 409 when the body would change it, before anything is applied, because the next start writes the environment's value back; a key sent with the value it already has is taken. A write-only key the environment sets is refused whatever it is sent with. The <key>_environment and <key>_levels marks and a read-only key are refused with 400, as a read-only key always is, and a value a file supplied - answered as (set) or null - is not one to send back: a client that reads the group and writes it back sends only the keys it changed. "
              "Answers the whole group as GET emits it. Server-wide; refused for domain-restricted and read-only keys.\","
              "\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"properties\":{";
      path += OpenApiProperties(group, bridge);
      path += "}}}}},"
              "\"responses\":{\"200\":{\"description\":\"The whole group after the change\"},"
              "\"400\":{\"description\":\"Refused, and nothing applied: error names the key or carries the setting's own sentence\"},"
              "\"409\":{\"description\":\"A key the server's environment sets would change, or a write-only key it sets was sent; error says which variable sets it, and nothing is applied\"},"
              "\"500\":{\"description\":\"A value was accepted and could not be written\"}}}}";

      return path;
   }

   // A group with no writable row has a GET and nothing else; the properties
   // go under the response, where a reader of the document looks for what a
   // GET answers.
   AnsiString OpenApiReadOnlyPath(const Group &group, const char *getSummary, const char *description, const Bridge &bridge)
   {
      AnsiString path = ",\"";
      path += group.path;
      path += "\":{\"get\":{\"summary\":\"";
      path += getSummary;
      path += "\",\"description\":\"";
      path += description;
      path += "\",\"responses\":{\"200\":{\"description\":\"The group as one flat object\","
              "\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"properties\":{";
      path += OpenApiProperties(group, bridge);
      path += "}}}}}}}}";
      return path;
   }

   // ---------------------------------------------------------------------
   // The [Settings] section of hMailServer.ini, one key at a time. The
   // checks and the sentences are InterfaceSettings::GetIniSetting,
   // SetIniSetting and DeleteIniSetting's, so a name or a value the COM
   // route refuses is refused here in the same words.
   // ---------------------------------------------------------------------

   const char *IniNameRefusal =
      "The setting name is empty, longer than 100 characters, surrounded by whitespace, or contains one of = [ ] or a line break.";
   // The one sentence, kept where --set-setting and --apply-environment read it too.
   const char *IniValueRefusal = EnvironmentSettings::NotStorableSentence();
   const char *IniWriteRefusal =
      "The setting could not be stored. Settings are held in the database, so this means the server could not write to it. Nothing has been changed.";
   const char *IniRemoveRefusal =
      "The setting could not be removed. Settings are held in the database, so this means the server could not write to it. Nothing has been changed.";

   // The name from the path as the store sees it, or the refusal. Not valid
   // UTF-8 is refused with the name sentence too: whatever the bytes were,
   // they are not a key the file holds.
   bool IniName(const AnsiString &raw, String &name)
   {
      if (!Unicode::MultiByteToWide(raw, name))
         return false;
      return IniSettingStore::IsStorableName(name);
   }

   // Whether the section holds the key now, as the names list reports it.
   // Keys are matched the way the profile API matches them, without case.
   bool IniHas(const String &name)
   {
      std::vector<String> names;
      Ini()->GetSettingsNames(names);
      for (size_t i = 0; i < names.size(); i++)
      {
         if (names[i].CompareNoCase(name) == 0)
            return true;
      }
      return false;
   }

   // The applies member for one name: its mark in the baseline, quoted - now,
   // reinitialize or restart - or null for a name the loader does not read, which
   // has no mark. Never "now" for such a name: COM's IniSettingApplies refuses it,
   // and a misspelled key answered as though it applied would look like a working
   // setting.
   AnsiString IniAppliesJson(const String &name)
   {
      String mark;
      String spelling;
      if (!IniFileSettings::GetApplies(name, mark, spelling))
         return "null";
      return "\"" + AnsiString(mark) + "\"";
   }

   // applies is the key's mark (IniAppliesJson), and for a name without one the
   // answer says why in applies_refused - the sentence COM's IniSettingApplies
   // refuses the same name with. The route itself does not refuse the name: it
   // stores and reads any name on purpose, for a setting a script of the
   // administrator's own reads. pending is whether the stored value is not yet the
   // running one: a key marked reinitialize or restart saved since what applies it
   // last happened. Computed from the store and the running values, not
   // remembered, so a value put back is not pending.
   AnsiString IniSettingJson(const String &name, const Bridge &bridge)
   {
      // A key the environment sets answers managed true and where the value came
      // from, and a value a file supplied is a Secret's: (set), never the value -
      // while the variable sets it and after, until an administrator writes another.
      const Managed managed;
      const EnvironmentSettings::Entry *environment = managed.Find(EnvironmentSettings::StoreIni, name);

      String value = Ini()->GetSettingsValue(name);
      if (managed.Withheld(EnvironmentSettings::StoreIni, name) && !value.IsEmpty())
         value = _T("(set)");

      AnsiString json = "{\"name\":\"" + bridge.escape(Utf8(name)) + "\",\"value\":\"";
      json += bridge.escape(Utf8(value));
      json += "\",\"present\":";
      json += IniHas(name) ? "true" : "false";
      const AnsiString applies = IniAppliesJson(name);
      json += ",\"applies\":" + applies;
      if (applies == "null")
         json += ",\"applies_refused\":\"" + bridge.escape(IniFileSettings::NoApplyMarkSentence()) + "\"";
      json += ",\"pending\":";
      json += Ini()->IsPending(name) ? "true" : "false";
      // The deepest level the key may be set at (docs/SettingLevels.md): a key
      // marked domain or account is resolved per domain and account, under the
      // name GET /api/v1/settings/levels gives it. Null where applies is.
      String levels;
      json += ",\"levels\":";
      json += IniFileSettings::GetLevels(name, levels) ? "\"" + AnsiString(levels) + "\"" : AnsiString("null");
      json += ",\"managed\":";
      json += environment ? "true" : "false";
      json += ",\"environment\":";
      json += environment ? "\"" + AnsiString(EnvironmentSettings::SourceWord(environment->source)) + "\"" : AnsiString("null");
      json += "}";
      return json;
   }

   // Both shapes at once. names is what this route has always answered and
   // what the Linux test shim's IniSettingNames reads; settings is what hmctl
   // and the PowerShell module read - and until 15 September 2026 believed
   // they were reading: both parsed a settings array this route never sent,
   // so `hmctl config export` and Export-HmConfiguration wrote an empty ini
   // section against every real server, while build/check-cli.py's recorded
   // server answered the shape the clients expected and the check passed.
   // The recorded server now answers this shape, and the check reads this
   // function to hold the two together.
   //
   // A value is the one GET /api/v1/settings/ini/{name} answers, read through
   // the same path: IniFileSettings' in-process overlay, which the server
   // loaded at start with the stored value, or the [SettingsOverride] value
   // where the file forces one, and which falls back to the file's line for a
   // name it does not hold. A save through this process (COM SetIniSetting, PUT
   // here) reloads the overlay from the store and publishes it
   // (docs/SettingsApply.md). So a value this server stored since it started is
   // listed as stored, and applies is what says whether it is in effect or
   // waits for a reinitialise or a restart; and a value stored by another
   // process - `hmailserver --set-setting`, another node on the same database -
   // is listed as this server last loaded it until its next save, a
   // reinitialise or a restart. names is
   // ReadSettingNames: the union of the store's rows and the file's [Settings]
   // lines, in std::set order, which is by name WITH case - so PasswordPepper,
   // which the store never holds, is in it when the file has it.
   //
   // Every value is listed except a secret's: a
   // name IniSettingStore::IsSecretName recognises is listed as "(set)" when
   // it has a value and empty when it has none, with secret true, because
   // this list is what a configuration export is built from and an export
   // goes into a repository. One key, asked for by name, is still read whole,
   // as Settings.GetIniSetting reads it over COM.
   //
   // Every stored name is listed, whether the loader reads it or not - a
   // setting a script of the administrator's own reads is stored and exported
   // like any other - and each entry is {name, value, secret} and nothing more,
   // the shape the export clients and the tests read (the row that made a save
   // publish added applies and pending to each entry, and an entry that did not
   // end where the clients expected it was not found). When a key applies is
   // said beside the entries instead: applies maps every key the loader reads to
   // its mark, and pending names the keys saved and not yet running.
   HttpResponse IniList(const Bridge &bridge)
   {
      std::vector<String> names;
      Ini()->GetSettingsNames(names);

      AnsiString json = "{\"names\":[";
      for (size_t i = 0; i < names.size(); i++)
      {
         if (i > 0)
            json += ",";
         json += "\"" + bridge.escape(Utf8(names[i])) + "\"";
      }

      // A key whose value a file of the environment supplied - now, or before and
      // not rewritten since - is listed as a secret is: (set), and secret true, so
      // that an export leaves it out as it leaves a password out
      // (docs/EnvironmentConfiguration.md).
      const Managed managed;

      json += "],\"settings\":[";
      for (size_t i = 0; i < names.size(); i++)
      {
         const String &name = names[i];
         bool secret = IniSettingStore::IsSecretName(name) || managed.Withheld(EnvironmentSettings::StoreIni, name);

         String value = Ini()->GetSettingsValue(name);
         if (secret && !value.IsEmpty())
            value = _T("(set)");

         if (i > 0)
            json += ",";
         json += "{\"name\":\"" + bridge.escape(Utf8(name)) + "\",\"value\":\"";
         json += bridge.escape(Utf8(value));
         json += "\",\"secret\":";
         json += secret ? "true" : "false";
         json += "}";
      }

      // Every key this build reads, stored or not, with its mark: the whole of
      // build/ini-settings-baseline.txt's [Settings] half, so that a client can
      // label a field whose key has never been stored.
      json += "],\"applies\":{";
      for (size_t i = 0; i < IniSettingsBaseline::SettingsCount; i++)
      {
         if (i > 0)
            json += ",";
         json += "\"" + bridge.escape(Utf8(IniSettingsBaseline::Settings[i].Name)) + "\":";
         json += IniAppliesJson(IniSettingsBaseline::Settings[i].Name);
      }

      // The keys saved since what applies them last happened, by name, as
      // GET /api/v1/server/settings/pending lists them with both values.
      json += "},\"pending\":[";
      std::vector<IniFileSettings::PendingSetting> pending = Ini()->GetPendingSettings();
      for (size_t i = 0; i < pending.size(); i++)
      {
         if (i > 0)
            json += ",";
         json += "\"" + bridge.escape(Utf8(pending[i].name)) + "\"";
      }

      // The keys the server's environment sets, each with where its value came
      // from - beside the entries rather than in them, whose {name, value, secret}
      // is the shape the export clients read.
      json += "],\"environment\":{";
      bool firstManaged = true;
      for (const EnvironmentSettings::Entry &entry : managed.entries)
      {
         if (entry.store != EnvironmentSettings::StoreIni)
            continue;
         if (!firstManaged)
            json += ",";
         firstManaged = false;
         json += "\"" + bridge.escape(Utf8(entry.name)) + "\":\"" + EnvironmentSettings::SourceWord(entry.source) + "\"";
      }
      json += "}}";
      return bridge.respond(200, json, "");
   }

   // GET /api/v1/server/settings/pending: every key saved since what applies it last
   // happened, with the value stored and the value running. A secret's two values
   // are "(set)" or empty, as the list shows them.
   HttpResponse IniPending(const Bridge &bridge)
   {
      std::vector<IniFileSettings::PendingSetting> pending = Ini()->GetPendingSettings();

      // A key a file of the environment supplied is a Secret's, and is shown as a
      // secret is (docs/EnvironmentConfiguration.md).
      const Managed managed;

      AnsiString json = "[";
      for (size_t i = 0; i < pending.size(); i++)
      {
         const IniFileSettings::PendingSetting &entry = pending[i];
         const bool secret = IniSettingStore::IsSecretName(entry.name) || managed.Withheld(EnvironmentSettings::StoreIni, entry.name);

         String stored = entry.stored;
         String running = entry.running;
         if (secret)
         {
            stored = stored.IsEmpty() ? String() : String(_T("(set)"));
            running = running.IsEmpty() ? String() : String(_T("(set)"));
         }

         if (i > 0)
            json += ",";
         json += "{\"name\":\"" + bridge.escape(Utf8(entry.name)) + "\"";
         json += ",\"stored\":\"" + bridge.escape(Utf8(stored)) + "\"";
         json += ",\"running\":\"" + bridge.escape(Utf8(running)) + "\"";
         json += ",\"applies\":\"" + bridge.escape(Utf8(entry.applies)) + "\"";
         json += ",\"secret\":";
         json += secret ? "true" : "false";
         json += "}";
      }
      json += "]";
      return bridge.respond(200, json, "");
   }

   HttpResponse IniGet(const AnsiString &raw, const Bridge &bridge)
   {
      String name;
      if (!IniName(raw, name))
         return Refusal(bridge, 400, IniNameRefusal);

      return bridge.respond(200, IniSettingJson(name, bridge), "");
   }

   HttpResponse IniPut(const AnsiString &raw, const AnsiString &requestBody, const Bridge &bridge)
   {
      String name;
      if (!IniName(raw, name))
         return Refusal(bridge, 400, IniNameRefusal);

      JsonValue document;
      std::string parseError;
      if (!JsonValue::Parse(std::string(requestBody.c_str(), requestBody.GetLength()), document, parseError) || !document.IsObject())
         return Refusal(bridge, 400, "the body must be a JSON object");

      const JsonValue *member = document.Get("value");
      if (!member || !member->IsString())
         return Refusal(bridge, 400, "value must be a string");

      String value;
      if (!Unicode::MultiByteToWide(AnsiString(member->AsString().c_str()), value))
         return Refusal(bridge, 400, "value is not valid UTF-8");

      if (!IniSettingStore::IsStorableValue(value))
         return Refusal(bridge, 400, IniValueRefusal);

      // A key the environment sets: refused unless the value is the one it has,
      // since the next start would write the environment's back. The store refuses
      // it too; this answers first, and the same way.
      {
         const Managed managed;
         const EnvironmentSettings::Entry *environment = managed.Find(EnvironmentSettings::StoreIni, name);
         String trimmed = value;
         trimmed.Trim();
         if (environment && Ini()->GetSettingsValue(name) != trimmed)
            return Refusal(bridge, 409, Utf8(EnvironmentSettings::RefusalSentence(name, *environment)));
      }

      if (!Ini()->WriteSettingsValue(name, value))
         return Refusal(bridge, 500, IniWriteRefusal);

      LOG_APPLICATION("RestApi: hMailServer.ini [Settings] " + name + " written.");

      return bridge.respond(200, IniSettingJson(name, bridge), "");
   }

   HttpResponse IniDelete(const AnsiString &raw, const Bridge &bridge)
   {
      String name;
      if (!IniName(raw, name))
         return Refusal(bridge, 400, IniNameRefusal);

      {
         const Managed managed;
         const EnvironmentSettings::Entry *environment = managed.Find(EnvironmentSettings::StoreIni, name);
         if (environment)
            return Refusal(bridge, 409, Utf8(EnvironmentSettings::RefusalSentence(name, *environment)));
      }

      if (!Ini()->RemoveSettingsValue(name))
         return Refusal(bridge, 500, IniRemoveRefusal);

      LOG_APPLICATION("RestApi: hMailServer.ini [Settings] " + name + " removed.");

      return bridge.respond(200, IniSettingJson(name, bridge), "");
   }

   // ---------------------------------------------------------------------
   // The server's message texts: Settings.ServerMessages over COM, the
   // texts the server puts in the mail it writes itself (a virus notice, an
   // undeliverable-message notice). The collection is the one Configuration
   // holds and the server reads from, so a changed text is used at once.
   // ---------------------------------------------------------------------

   AnsiString ServerMessageJson(std::shared_ptr<ServerMessage> message, const Bridge &bridge)
   {
      AnsiString json;
      json.Format("{\"id\":%I64d,\"name\":\"%hs\",\"text\":\"%hs\"}",
         message->GetID(),
         bridge.escape(Utf8(message->GetName())).c_str(),
         bridge.escape(Utf8(message->GetText())).c_str());
      return json;
   }

   HttpResponse ServerMessagesList(const Bridge &bridge)
   {
      std::shared_ptr<ServerMessages> messages = Config()->GetServerMessages();
      if (!messages)
         return Refusal(bridge, 503, "the server messages are not loaded");

      AnsiString json = "[";
      int count = 0;
      for (std::shared_ptr<ServerMessage> message : messages->GetSnapshot())
      {
         if (!message)
            continue;
         if (count > 0)
            json += ",";
         json += ServerMessageJson(message, bridge);
         count++;
      }
      json += "]";
      return bridge.respond(200, json, "");
   }

   HttpResponse ServerMessagePut(const AnsiString &rawName, const AnsiString &requestBody, const Bridge &bridge)
   {
      std::shared_ptr<ServerMessages> messages = Config()->GetServerMessages();
      if (!messages)
         return Refusal(bridge, 503, "the server messages are not loaded");

      String name;
      if (!Unicode::MultiByteToWide(rawName, name))
         return Refusal(bridge, 400, "the name is not valid UTF-8");

      std::shared_ptr<ServerMessage> message = messages->GetItemByName(name);
      if (!message)
         return Refusal(bridge, 404, "server message not found");

      JsonValue document;
      std::string parseError;
      if (!JsonValue::Parse(std::string(requestBody.c_str(), requestBody.GetLength()), document, parseError) || !document.IsObject())
         return Refusal(bridge, 400, "the body must be a JSON object");

      const JsonValue *member = document.Get("text");
      if (!member || !member->IsString())
         return Refusal(bridge, 400, "text must be a string");

      String text;
      if (!Unicode::MultiByteToWide(AnsiString(member->AsString().c_str()), text))
         return Refusal(bridge, 400, "text is not valid UTF-8");

      // Onto a copy, which is saved, and then the messages are read again - as
      // the route and list updates do it, and InterfaceServerMessage::Save. The
      // object in the collection is the one the server reads: a bounce or a
      // virus notice being written copies its text while this runs, and a text
      // assigned under that copy frees the buffer being copied. The refresh
      // swaps in new messages under the collection's lock, and a notice being
      // written keeps the one it holds. A refused save changes nothing.
      std::shared_ptr<ServerMessage> changed(new ServerMessage(*message));
      changed->SetText(text);
      if (!PersistentServerMessage::SaveObject(changed))
         return Refusal(bridge, 500, "the server message could not be saved; see the error log");

      if (!messages->Refresh())
      {
         LOG_APPLICATION("RestApi: Server message " + name + " was saved, but the server messages could not be read again; "
            "the running server keeps the previous text until they can be.");
      }

      LOG_APPLICATION("RestApi: Server message " + name + " changed.");

      return bridge.respond(200, ServerMessageJson(changed, bridge), "");
   }

   // ---------------------------------------------------------------------
   // Sieve, evaluated and not delivered: Utilities.EvaluateSieveScript over
   // COM. The summary is the action list the script decided on for the
   // message - "keep", "fileinto:Folder", "discard", "redirect:..." - or
   // "error: ..." when the script does not parse; nothing is sent, filed or
   // recorded, which is what makes this a safe way to try a script.
   // ---------------------------------------------------------------------

   HttpResponse SieveEvaluate(const AnsiString &requestBody, const Bridge &bridge)
   {
      JsonValue document;
      std::string parseError;
      if (!JsonValue::Parse(std::string(requestBody.c_str(), requestBody.GetLength()), document, parseError) || !document.IsObject())
         return Refusal(bridge, 400, "the body must be a JSON object");

      const JsonValue *script = document.Get("script");
      const JsonValue *message = document.Get("message");
      if (!script || !script->IsString())
         return Refusal(bridge, 400, "script must be a string");
      if (!message || !message->IsString())
         return Refusal(bridge, 400, "message must be a string");

      String scriptText, messageText;
      if (!Unicode::MultiByteToWide(AnsiString(script->AsString().c_str()), scriptText) ||
          !Unicode::MultiByteToWide(AnsiString(message->AsString().c_str()), messageText))
         return Refusal(bridge, 400, "script and message must be valid UTF-8");

      String result = SieveScript::Evaluate(scriptText, messageText);

      return bridge.respond(200, "{\"result\":\"" + bridge.escape(Utf8(result)) + "\"}", "");
   }

   // ---------------------------------------------------------------------
   // POST /api/v1/calendar/expand: the recurrence expansion of one iCalendar
   // object over a window, exactly as the CalDAV calendar-query performs it,
   // so a rule can be checked against what the server will answer for it.
   // Nothing is stored. The body: calendar (the object's text), start and
   // end (UTC, YYYYMMDDTHHMMSSZ; the window, unbounded when absent) and
   // limit (instances, 1..10000, 1000 by default).
   // ---------------------------------------------------------------------

   HttpResponse CalendarExpand(const AnsiString &requestBody, const Bridge &bridge)
   {
      JsonValue document;
      std::string parseError;
      if (!JsonValue::Parse(std::string(requestBody.c_str(), requestBody.GetLength()), document, parseError) || !document.IsObject())
         return Refusal(bridge, 400, "the body must be a JSON object");

      const JsonValue *calendar = document.Get("calendar");
      if (!calendar || !calendar->IsString())
         return Refusal(bridge, 400, "calendar must be a string holding one iCalendar object");

      __int64 windowStart = 0;
      __int64 windowEnd = ICalendar::Forever;
      if (const JsonValue *start = document.Get("start"))
      {
         if (!start->IsString() || !ICalendar::ParseUtc(AnsiString(start->AsString().c_str()), windowStart))
            return Refusal(bridge, 400, "start must be a UTC date-time, YYYYMMDDTHHMMSSZ");
      }
      if (const JsonValue *end = document.Get("end"))
      {
         if (!end->IsString() || !ICalendar::ParseUtc(AnsiString(end->AsString().c_str()), windowEnd))
            return Refusal(bridge, 400, "end must be a UTC date-time, YYYYMMDDTHHMMSSZ");
      }
      if (windowEnd <= windowStart)
         return Refusal(bridge, 400, "end must be after start");

      size_t maxInstances = 1000;
      if (const JsonValue *limit = document.Get("limit"))
      {
         if (!limit->IsNumber() || limit->AsNumber() < 1 || limit->AsNumber() > 10000)
            return Refusal(bridge, 400, "limit must be a number from 1 to 10000");
         maxInstances = static_cast<size_t>(limit->AsNumber());
      }

      ICalComponent tree;
      AnsiString problem;
      AnsiString text(calendar->AsString().c_str());
      if (!ICalendar::Parse(text, tree, problem))
         return Refusal(bridge, 400, "the calendar is not one iCalendar object: " + problem);

      ICalendar::Summary summary;
      if (!ICalendar::Summarize(tree, summary, problem))
         return Refusal(bridge, 400, "the object cannot be expanded: " + problem);

      std::vector<ICalInstance> instances;
      bool truncated = false;
      if (!ICalendar::Expand(tree, windowStart, windowEnd, maxInstances, instances, truncated, problem))
         return Refusal(bridge, 400, "the object cannot be expanded: " + problem);

      AnsiString count;
      count.Format("%d", static_cast<int>(instances.size()));

      AnsiString json = "{\"uid\":\"" + bridge.escape(summary.uid) + "\",\"component\":\"" + bridge.escape(summary.component) +
         "\",\"recurring\":" + AnsiString(summary.recurring ? "true" : "false") +
         ",\"first\":\"" + ICalendar::FormatUtc(summary.first) +
         "\",\"last\":\"" + (summary.last >= ICalendar::Forever ? AnsiString("") : ICalendar::FormatUtc(summary.last)) +
         "\",\"count\":" + count + ",\"truncated\":" + AnsiString(truncated ? "true" : "false") + ",\"instances\":[";

      for (size_t i = 0; i < instances.size(); i++)
      {
         const ICalInstance &instance = instances[i];
         AnsiString summaryText = instance.component < tree.children.size() ? tree.children[instance.component].Value("SUMMARY") : AnsiString("");
         if (i > 0)
            json += ",";
         json += "{\"start\":\"" + ICalendar::FormatUtc(instance.start) + "\",\"end\":\"" + ICalendar::FormatUtc(instance.end) +
            "\",\"all_day\":" + AnsiString(instance.allDay ? "true" : "false") +
            ",\"recurrence_id\":\"" + ICalendar::FormatUtc(instance.recurrenceId) +
            "\",\"overridden\":" + AnsiString(instance.overridden ? "true" : "false") +
            ",\"summary\":\"" + bridge.escape(summaryText) + "\"}";
      }

      json += "]}";
      return bridge.respond(200, json, "");
   }

   // The hand-written paths beside the generated groups: the INI routes and
   // the logon-failure clear, each beginning with a comma as OpenApiPath's do.
   const char *ScriptingMessagesAndSievePaths =
      ",\"/api/v1/settings/scripting/reload\":{\"post\":{\"summary\":\"Load the event-handler script again\",\"description\":\"What Scripting.Reload does over COM: the script file is read again and the handlers it defines take over from the next event. Server-wide; refused for domain-restricted and read-only keys.\",\"responses\":{\"200\":{\"description\":\"{reloaded: true}\"}}}},"
      "\"/api/v1/settings/scripting/check\":{\"post\":{\"summary\":\"Check the event-handler script's syntax\",\"description\":\"What Scripting.CheckSyntax does over COM: result is empty when the script parses, and the parser's message otherwise. Nothing is changed. Server-wide; refused for domain-restricted keys.\",\"responses\":{\"200\":{\"description\":\"{result: the parser's message, or empty}\"}}}},"
      "\"/api/v1/settings/messages\":{\"get\":{\"summary\":\"The server's message texts\",\"description\":\"The texts the server puts in the mail it writes itself - Settings.ServerMessages over COM. Each entry: id, name, text. Server-wide; refused for domain-restricted keys.\",\"responses\":{\"200\":{\"description\":\"Array of {id, name, text}\"}}}},"
      "\"/api/v1/settings/messages/{name}\":{\"put\":{\"summary\":\"Change one server message text\",\"description\":\"Body: text. The name is the table's own, as the listing shows it. In use once the messages are read again after the save, which is at once unless that read fails, and the application log says when it does. Server-wide; refused for domain-restricted and read-only keys.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"text\"],\"properties\":{\"text\":{\"type\":\"string\"}}}}}},\"responses\":{\"200\":{\"description\":\"{id, name, text} as saved\"},\"400\":{\"description\":\"text missing or not a string\"},\"404\":{\"description\":\"No message of that name\"}}}},"
      "\"/api/v1/sieve/evaluate\":{\"post\":{\"summary\":\"Try a Sieve script against a message, without delivering anything\",\"description\":\"Body: script (RFC 5228 Sieve) and message (the raw message, headers and body). result is the action list the script decided on - keep, fileinto:Folder, discard, redirect:address, and so on - or error: followed by the parser's message. What Utilities.EvaluateSieveScript does over COM: nothing is filed, sent or recorded. Server-wide; refused for domain-restricted keys.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"script\",\"message\"],\"properties\":{\"script\":{\"type\":\"string\"},\"message\":{\"type\":\"string\"}}}}}},\"responses\":{\"200\":{\"description\":\"{result}\"},\"400\":{\"description\":\"script or message missing\"}}}},"
      "\"/api/v1/calendar/expand\":{\"post\":{\"summary\":\"Expand an iCalendar object's recurrence over a window, without storing anything\",\"description\":\"Body: calendar (one iCalendar object, VEVENT or VTODO, with any VTIMEZONE it needs), start and end (UTC, YYYYMMDDTHHMMSSZ; the window, unbounded when absent) and limit (instances, 1 to 10000, 1000 by default). Answers what the CalDAV calendar-query computes for the object: uid, component, recurring, first and last (the span; last empty for a recurrence with no end), count, truncated, and instances - each with start, end, all_day, recurrence_id, overridden and summary. An object the server would refuse on PUT - a rule it does not expand, a zone it cannot place - is a 400 with the reason. Server-wide; refused for domain-restricted keys.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"calendar\"],\"properties\":{\"calendar\":{\"type\":\"string\"},\"start\":{\"type\":\"string\"},\"end\":{\"type\":\"string\"},\"limit\":{\"type\":\"integer\"}}}}}},\"responses\":{\"200\":{\"description\":\"{uid, component, recurring, first, last, count, truncated, instances: [{start, end, all_day, recurrence_id, overridden, summary}]}\"},\"400\":{\"description\":\"calendar missing, not one iCalendar object, or one this server cannot expand (the reason is in error)\"}}}}";

   const char *CacheAndIndexingPaths =
      ",\"/api/v1/settings/cache/clear\":{\"post\":{\"summary\":\"Empty the caches\",\"description\":\"What Settings.Cache.Clear does over COM: the domain, account, alias and distribution-list caches are emptied, and the open-message container with them, so the next lookup of each reads the database. Server-wide; refused for domain-restricted and read-only keys.\",\"responses\":{\"200\":{\"description\":\"{cleared: true}\"}}}},"
      "\"/api/v1/settings/indexing/index\":{\"post\":{\"summary\":\"Index the messages not yet indexed, now\",\"description\":\"What Settings.MessageIndexing.Index does over COM: the indexer runs at once rather than at its next tick; the answer does not wait for it. Server-wide; refused for domain-restricted and read-only keys.\",\"responses\":{\"200\":{\"description\":\"{started: true}\"}}}},"
      "\"/api/v1/settings/indexing/clear\":{\"post\":{\"summary\":\"Empty the message index\",\"description\":\"What Settings.MessageIndexing.Clear does over COM: every indexed message's metadata, the full-text terms and the backfill cursor go, so the indexer starts again from the first message when it next runs. Server-wide; refused for domain-restricted and read-only keys.\",\"responses\":{\"200\":{\"description\":\"{cleared: true}\"}}}}";

   const char *BreachCheckTestPath =
      ",\"/api/v1/settings/breach-check/test\":{\"post\":{\"summary\":\"Ask the password breach check endpoint for a range\",\"description\":\"What Settings.TestBreachCheckEndpoint does over COM: the endpoint - url in the body, or breach_check_url when the body has none - is asked for the prefix 00000, which belongs to no password, with the padding header and the timeout a real check uses, and the answer is reported: reachable, the HTTP status, how many entries came back, whether any was padding, and how long it took. The body of the answer is never returned. A url the check would not accept is a 400 with the reason. Nothing is changed. Server-wide; refused for domain-restricted and read-only keys.\",\"requestBody\":{\"required\":false,\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\"}}}}}},\"responses\":{\"200\":{\"description\":\"{reachable: true, status, entries, padded, milliseconds}, or {reachable: false, status (0 when no answer arrived), error, milliseconds}\"},\"400\":{\"description\":\"The body is not a JSON object, url is not a string, or the url is not one the check accepts (the reason is in error)\"}}}}";

   const char *EnvironmentPath =
      ",\"/api/v1/settings/environment\":{\"get\":{\"summary\":\"The settings this server's environment sets\",\"description\":\"Every setting the last hmailserver --apply-environment wrote (docs/EnvironmentConfiguration.md): HM_SETTING_<key> for a [Settings] key, HM_PROPERTY_<name> for an hm_settings row, either with _FILE to read the value from a file, and the four HM_REST_* variables the container has always taken. Each is {variable, store, name, source}: the variable as the server recorded it, ini or database, the setting's own name, and variable or file for where the value came from. NEVER a value - a file's is a Secret's, and the rest are read where the setting is read. The environment wins at every start, so the settings routes answer <key>_environment (and managed on the ini route) beside each of these and refuse a change to one with 409; an --apply-environment run with the variable unset takes it off this list, and the setting is the administrator's again. As this server last loaded the record: a run against a running server reaches this list, as its values reach the server, at the next reinitialise or start. Server-wide; a read-only key may read it, a key restricted to named domains may not.\",\"responses\":{\"200\":{\"description\":\"{settings: [{variable, store, name, source}]}\"}}}}";

   const char *ChangedFromStockPath =
      ",\"/api/v1/settings/changed\":{\"get\":{\"summary\":\"What this server has changed from stock\",\"description\":\"Every setting whose value is not the one this version of the server ships with, with the shipped value beside it - the same document Settings.GetChangedFromDefault answers over COM, built by the same function. It is the first thing to look at when a server behaves oddly and the first thing to put in a support message: a server holds several hundred settings across its two stores and an administrator has usually changed twenty, and until this route there was no way to be told which twenty. changed is how many are listed and considered is how many settings the two stores hold altogether, so that a count of 3 reads as 3 of 391. Each entry is {store, name, value, default, secret, known, overridden, environment, profile}; environment is variable or file where the server's environment sets the setting (hmailserver --apply-environment, docs/EnvironmentConfiguration.md) and null otherwise; profile names the setting profile whose apply wrote the value (docs/Profiles.md), while that profile is in effect and the setting still holds what it wrote, and is null otherwise - for a value the apply found already there, one the environment sets and a secret, and a value a file supplied is listed as (set) with secret true, as a secret's is - while the file sets it, and after, until an administrator writes another value. The top-level profile is the record of the last profile applied, {name, state, applied} with state applied, reverted, kept or failed, or null when none was. Beside settings, environment lists every setting the environment sets, as {variable, store, name, source}, whether or not its value differs from stock, and from_file lists every setting whose value is withheld because a file supplied it, as {store, name}: never a value. store is ini for the keys that were the [Settings] section of hMailServer.ini (hm_inisettings since schema 6042), whose shipped value is the third argument of the read in the server's own source, and database for hm_settings, whose shipped value is what this version's create script puts in a new database. DEFAULT MEANS what the server behaves as though the setting has when nothing is stored for it, not what a fresh installation's files happen to contain: hMailServer.ini ships with lines the packaging wrote, every one adopted into the store at the first start, and a line holding the shipped value is not a change to anything and is not listed - which is what keeps this list short enough to read. A line the installer wrote with a value that is NOT the shipped one is listed, and should be. A setting whose name says its value is a secret (a password, a shared secret, a token, the pepper; not a key FILE, which is a path) is listed with value (set) when it has one and empty when it has none, and secret true, by the same rule GET /api/v1/settings/ini uses; GET /api/v1/settings/ini/{name} reads such a value when it is wanted. overridden is true where the [SettingsOverride] section of hMailServer.ini is forcing the value over whatever is stored - the deliberate door for a database that is unreachable or holds a value the server will not start on, which no other page and no backup shows, and a key forced that way is listed even when neither store holds a row for it. A row of hm_settings that this build does not read is not listed at all: an installation upgraded from an old schema holds a dozen that upgrade scripts inserted for features that have gone, and a row nothing reads cannot explain anything the server is doing. known is false for a setting this build ships no default for - a key stored for a script of the administrator's own, or one the server reads by name where it is used rather than through the loader - which is a difference from stock all the same, since stock has no such value at all. The values are this server process's: a setting stored since it started is listed as stored - a save applies most [Settings] keys at once, and GET /api/v1/server/settings/pending lists the ones that wait for a reinitialise or a restart - and one stored by another process (hmailserver --set-setting, another node on the same database) is listed as this server last loaded it until its next save, a reinitialise or a restart. Administrator password only: an api key of any scope is refused with a 401 and an account holding an administrative role with a 403, for the same reason the ini list is - the settings include the server's secrets and the commands it runs, and AutoBanCommand is a command this server runs as the service account.\",\"responses\":{\"200\":{\"description\":\"{changed, considered, settings: [{store, name, value, default, secret, known, overridden, environment, profile}], environment: [{variable, store, name, source}], from_file: [{store, name}], profile: {name, state, applied} or null}\"}}}}";

   const char *IniAndLogonFailurePaths =
      ",\"/api/v1/settings/ini\":{\"get\":{\"summary\":\"The server settings that were the [Settings] section of hMailServer.ini\",\"description\":\"names: every name in the settings store or in the [Settings] section of the file, sorted by name with case - what Settings.IniSettingNames lists over COM; PasswordPepper, which the store never holds, is among them when the file has it. settings: the same names as {name, value, secret}, with the value GET /api/v1/settings/ini/{name} answers - the stored value, or the [SettingsOverride] value where the file forces one, as this server process last loaded or wrote it. Every stored name is listed, whether this build reads it or not: a setting a script of the administrator's own reads is stored and exported like any other. applies maps every [Settings] key this build reads, stored or not, to when a change to it reaches the running server: now (a save applies it - most keys), reinitialize (it waits for POST /api/v1/server/reinitialize) or restart (it waits for the service to start again), the key's mark in build/ini-settings-baseline.txt; a name it does not map has no mark, as Settings.IniSettingApplies refuses it. pending names the keys saved since what applies them last happened; GET /api/v1/server/settings/pending lists them with both values. environment maps each key the server's environment sets (docs/EnvironmentConfiguration.md) to where its value came from, variable or file; a key a file set is listed as a secret is, (set) and secret true. A save reloads the store, so a value stored by another process (hmailserver --set-setting, another node on the same database) is listed as this server last loaded it until the next save here, a reinitialise or a restart. A setting whose name says its value is a secret (a password, a shared secret, a token, the pepper; not a key FILE, which is a path) is listed with value (set) when it has one and empty when it has none, and secret true: this list is what a configuration export is built from, and GET /api/v1/settings/ini/{name} reads such a value when it is wanted. Since schema 6042 the database (hm_inisettings) is the store and the section in the file is the server's copy of it. Administrator password only: an api key of any scope is refused, since the settings include the server's secrets and the commands it runs.\",\"responses\":{\"200\":{\"description\":\"{names: [...], settings: [{name, value, secret}], applies: {name: now|reinitialize|restart}, pending: [name, ...], environment: {name: variable|file}}\"}}}},"
      "\"/api/v1/settings/ini/{name}\":{"
      "\"get\":{\"summary\":\"One server setting, by the name it has in [Settings]\",\"description\":\"name, value, present, applies, pending, levels, managed and environment - managed true, and environment variable or file, for a key the server's environment sets (docs/EnvironmentConfiguration.md), whose value is answered as (set) when a file supplied it. An absent key answers 200 with an empty value and present false, as Settings.GetIniSetting answers an empty string: a setting the store does not hold is at its default, not an error. The value is the stored value, or the [SettingsOverride] value where the file forces one, as this server process last loaded or wrote it. applies says when a change reaches the running server - now, reinitialize or restart, as Settings.IniSettingApplies answers; for a name this build does not read through its loader it is null, and applies_refused carries the sentence Settings.IniSettingApplies refuses the name with - and pending is true while a stored reinitialize or restart value is not yet the running one, so the value is then not the value in use. A value stored by another process (hmailserver --set-setting, another node) shows after the next save here, a reinitialise or a restart. It is given secret or not - this is the one place a secret is read, deliberately and by name; the list masks it. A name the store could not hold - empty, over 100 characters, surrounded by whitespace, or containing = [ ] or a line break - is a 400 with that sentence. The name is taken from the path as spelled, and matched without case. Server-wide; refused for domain-restricted keys.\",\"responses\":{\"200\":{\"description\":\"{name, value, present, applies, pending, levels, managed, environment}, and applies_refused when applies is null\"},\"400\":{\"description\":\"The name cannot be a key\"}}},"
      "\"put\":{\"summary\":\"Store one server setting\",\"description\":\"Body: value (a string of at most 4000 characters with no line break; surrounding whitespace is trimmed). Stored in the database, which is the settings store since schema 6042, and then written into the [Settings] section of hMailServer.ini as the server's copy - what Settings.SetIniSetting does over COM. Then published: the server rebuilds its settings from the store, so a key whose applies is now is in effect when this answers, and one marked reinitialize or restart answers pending true until POST /api/v1/server/reinitialize or a restart of the service takes it. Editing the file instead sets nothing: the stored value is used, the edit is named in the error log as HM5804 and the line is put back at the next start or reinitialise, which is where the file is reconciled with the store - a save reads the store back and leaves the file to them. Answers as GET does. Server-wide; refused for domain-restricted and read-only keys.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"value\"],\"properties\":{\"value\":{\"type\":\"string\"}}}}}},\"responses\":{\"200\":{\"description\":\"{name, value, present: true, applies, pending, levels, managed, environment}\"},\"400\":{\"description\":\"The name cannot be a key, the value cannot be stored, or the body has no string value\"},\"409\":{\"description\":\"The server's environment sets the key (managed true) and the value is not the one it has: the next start would write the environment's back, so nothing is stored\"},\"500\":{\"description\":\"The database could not be written, so nothing was stored and nothing changed (for PasswordPepper, which is kept in the file only, the file could not be written). A value that was stored but whose line could not then be written into the file is a 200: the setting is stored, and the error log names it (HM5806)\"}}},"
      "\"delete\":{\"summary\":\"Remove one server setting, returning it to its default\",\"description\":\"The line is removed from the file and the row is dropped from the store, returning the setting to its default - not the same as writing an empty value, and since schema 6042 the only way a setting is returned to its default: a line deleted from the file by hand is written back from the stored row. Then published, as PUT is: a key whose applies is now is at its default when this answers, and one marked reinitialize or restart answers pending true until POST /api/v1/server/reinitialize or a restart of the service takes it. Removing a key that is not there is a 200. Answers as GET does, with present false. Server-wide; refused for domain-restricted and read-only keys.\",\"responses\":{\"200\":{\"description\":\"{name, value: empty, present: false, applies, pending, levels, managed, environment}\"},\"400\":{\"description\":\"The name cannot be a key\"},\"409\":{\"description\":\"The server's environment sets the key (managed true): the next start would write it back, so nothing is removed\"},\"500\":{\"description\":\"The line could not be removed from the file, or the row could not be dropped; the setting is still set. When the row is what failed the error log says so (HM5806) and the next start writes the line back\"}}}},"
      "\"/api/v1/settings/logon-failures/clear\":{\"post\":{\"summary\":\"Forget every recorded logon failure\",\"description\":\"What Settings.ClearLogonFailureList does over COM: the failures the auto-ban counts are deleted, so that an address that was one failure from a ban starts again at none. It does not lift a ban already placed; that is an IP range, deleted through /api/v1/ipranges. Server-wide, and there is no per-domain form of it: the table behind it records the ADDRESS a failure came from and the time, and holds no account and no domain, so clearing it resets the counter for every domain on the server. Refused for domain-restricted and read-only keys, and to a credential whose role is confined to named domains - a help desk included, although the unlock permission is what classifies this route. A confined help desk lifts one person's lockout with POST /api/v1/accounts/{address}/unlock, which is scoped to that account's domain. On the audit trail as object type logon failures, action cleared.\",\"responses\":{\"200\":{\"description\":\"{cleared: true}\"},\"500\":{\"description\":\"The list could not be cleared\"}}}}";
}

namespace HM
{
   // The server-wide group for GET /api/v1/settings, whose handler lives in
   // RestApiServer.cpp and declares this function where it calls it.
   AnsiString
   RestApiSettingsServerGroupJson(AnsiString (*escape)(const AnsiString &))
   {
      Bridge bridge = { escape, nullptr };

      if (!ConfigurationLoaded())
         return "{}";

      return GroupJson(ServerGroup, bridge);
   }

   AnsiString
   RestApiServer::OpenApiSettingsPaths_()
   {
      Bridge bridge = { &RestApiServer::JsonEscape_, &RestApiServer::BuildResponse_ };

      AnsiString paths;
      paths += OpenApiPath(ServerGroup, "The server-wide settings", "Change server-wide settings", bridge);
      paths += OpenApiPath(AntiSpamGroup, "The anti-spam settings", "Change anti-spam settings", bridge);
      paths += OpenApiPath(AntiVirusGroup, "The anti-virus settings", "Change anti-virus settings", bridge);
      paths += OpenApiPath(LoggingGroup, "The logging settings", "Change logging settings", bridge);
      paths += OpenApiReadOnlyPath(DirectoriesGroup, "The directories the server runs in",
         "What Settings.Directories reports over COM, and the hMailServer.ini the settings came from. Facts about the installation, set when it was made; nothing here is written. Server-wide; refused for domain-restricted keys.", bridge);
      paths += ChangedFromStockPath;
      paths += EnvironmentPath;
      paths += IniAndLogonFailurePaths;
      paths += OpenApiPath(ComplaintGroup, "The feedback loop settings", "Change the feedback loop settings", bridge);
      paths += OpenApiPath(ScriptingGroup, "The event-handler scripting settings", "Change the scripting settings", bridge);
      paths += OpenApiPath(BackupGroup, "The backup settings", "Change the backup settings", bridge);
      paths += ScriptingMessagesAndSievePaths;
      paths += OpenApiPath(CacheGroup, "The cache settings and counters", "Change the cache settings", bridge);
      paths += OpenApiPath(IndexingGroup, "The message indexing settings and counts", "Change the indexing settings", bridge);
      paths += CacheAndIndexingPaths;
      paths += BreachCheckTestPath;
      paths += OpenApiPath(PushGroup, "The Web Push settings", "Change the Web Push settings", bridge);
      paths += PushVapidPath;
      paths += OpenApiPath(BounceGroup, "The bounce and suppression list settings", "Change the bounce and suppression list settings", bridge);
      paths += OpenApiPath(OpenPgpGroup, "The OpenPGP Web Key Directory settings", "Change the OpenPGP Web Key Directory settings", bridge);
      return paths;
   }

   HttpResponse
   RestApiServer::HandleSettingsPut_(const Caller &caller, const AnsiString &requestBody)
   {
      Bridge bridge = { &RestApiServer::JsonEscape_, &RestApiServer::BuildResponse_ };

      // The compromise_* keys are the compromised-account detection's mode and
      // thresholds, and a key of any scope is refused them for the reason it
      // is refused the review routes (IsApiKeyRoute_): a stolen key that could
      // set the mode to off, or the floors out of reach, would switch the
      // detection off for the account it stole. Found by name before the
      // group's own parse, so that nothing in the body is applied; an account
      // holding manage_settings, and the administrator password, go on.
      if (caller.result == AuthenticatedWithApiKey)
      {
         JsonValue document;
         std::string parseError;
         if (JsonValue::Parse(std::string(requestBody.c_str(), requestBody.GetLength()), document, parseError) && document.IsObject())
         {
            const std::vector<std::pair<std::string, JsonValue> > &members = document.Members();
            for (size_t i = 0; i < members.size(); i++)
            {
               if (members[i].first.compare(0, 11, "compromise_") != 0)
                  continue;

               const AnsiString key = AnsiString(members[i].first.substr(0, 48).c_str());
               return BuildResponse_(403, "{\"error\":\"" + JsonEscape_(key) + " answers to the administrator password or an account holding a role, never an api key: the compromised-account detection cannot be switched off or blunted with a key\"}");
            }
         }
      }

      return GroupPut(ServerGroup, requestBody, bridge);
   }

   HttpResponse
   RestApiServer::HandleSettingsAntiSpam_()
   {
      Bridge bridge = { &RestApiServer::JsonEscape_, &RestApiServer::BuildResponse_ };
      return GroupGet(AntiSpamGroup, bridge);
   }

   HttpResponse
   RestApiServer::HandleSettingsAntiSpamPut_(const AnsiString &requestBody)
   {
      Bridge bridge = { &RestApiServer::JsonEscape_, &RestApiServer::BuildResponse_ };
      return GroupPut(AntiSpamGroup, requestBody, bridge);
   }

   HttpResponse
   RestApiServer::HandleSettingsAntiVirus_()
   {
      Bridge bridge = { &RestApiServer::JsonEscape_, &RestApiServer::BuildResponse_ };
      return GroupGet(AntiVirusGroup, bridge);
   }

   HttpResponse
   RestApiServer::HandleSettingsAntiVirusPut_(const AnsiString &requestBody)
   {
      Bridge bridge = { &RestApiServer::JsonEscape_, &RestApiServer::BuildResponse_ };
      return GroupPut(AntiVirusGroup, requestBody, bridge);
   }

   HttpResponse
   RestApiServer::HandleSettingsLogging_()
   {
      Bridge bridge = { &RestApiServer::JsonEscape_, &RestApiServer::BuildResponse_ };
      return GroupGet(LoggingGroup, bridge);
   }

   HttpResponse
   RestApiServer::HandleSettingsLoggingPut_(const AnsiString &requestBody)
   {
      Bridge bridge = { &RestApiServer::JsonEscape_, &RestApiServer::BuildResponse_ };
      return GroupPut(LoggingGroup, requestBody, bridge);
   }

   HttpResponse
   RestApiServer::HandleSettingsDirectories_()
   {
      Bridge bridge = { &RestApiServer::JsonEscape_, &RestApiServer::BuildResponse_ };
      return GroupGet(DirectoriesGroup, bridge);
   }

   HttpResponse
   RestApiServer::HandleIniSettingList_()
   {
      Bridge bridge = { &RestApiServer::JsonEscape_, &RestApiServer::BuildResponse_ };
      return IniList(bridge);
   }

   HttpResponse
   RestApiServer::HandleServerSettingsPending_()
   {
      Bridge bridge = { &RestApiServer::JsonEscape_, &RestApiServer::BuildResponse_ };
      return IniPending(bridge);
   }

   HttpResponse
   RestApiServer::HandleIniSettingGet_(const AnsiString &name)
   {
      Bridge bridge = { &RestApiServer::JsonEscape_, &RestApiServer::BuildResponse_ };
      return IniGet(name, bridge);
   }

   HttpResponse
   RestApiServer::HandleIniSettingPut_(const AnsiString &name, const AnsiString &requestBody)
   {
      Bridge bridge = { &RestApiServer::JsonEscape_, &RestApiServer::BuildResponse_ };
      return IniPut(name, requestBody, bridge);
   }

   HttpResponse
   RestApiServer::HandleIniSettingDelete_(const AnsiString &name)
   {
      Bridge bridge = { &RestApiServer::JsonEscape_, &RestApiServer::BuildResponse_ };
      return IniDelete(name, bridge);
   }

   HttpResponse
   RestApiServer::HandleSettingsEnvironment_()
   {
      // The names and where each value came from, never a value: the route a
      // reader key may read, so it carries nothing the settings routes withhold.
      std::vector<EnvironmentSettings::Entry> entries;
      EnvironmentSettings::ReadManaged(entries);

      AnsiString json = "{\"settings\":[";
      for (size_t i = 0; i < entries.size(); i++)
      {
         const EnvironmentSettings::Entry &entry = entries[i];
         if (i > 0)
            json += ",";
         json += "{\"variable\":\"" + JsonEscape_(Utf8(entry.variable)) + "\"";
         json += ",\"store\":\"" + AnsiString(entry.store == EnvironmentSettings::StoreIni ? "ini" : "database") + "\"";
         json += ",\"name\":\"" + JsonEscape_(Utf8(entry.name)) + "\"";
         json += ",\"source\":\"" + AnsiString(EnvironmentSettings::SourceWord(entry.source)) + "\"}";
      }
      json += "]}";

      return BuildResponse_(200, json, "");
   }

   HttpResponse
   RestApiServer::HandleSettingsChanged_()
   {
      // SettingsDiff::Json is the whole answer, and is the same function COM's
      // Settings.GetChangedFromDefault calls: this route holds no opinion of its
      // own about what a difference is, so the two interfaces cannot describe the
      // same server differently. It is already JSON, and already escaped as JSON,
      // so it goes out as it stands.
      return BuildResponse_(200, SettingsDiff::Json(), "");
   }

   HttpResponse
   RestApiServer::HandleSettingsScripting_()
   {
      Bridge bridge = { &RestApiServer::JsonEscape_, &RestApiServer::BuildResponse_ };
      return GroupGet(ScriptingGroup, bridge);
   }

   HttpResponse
   RestApiServer::HandleSettingsScriptingPut_(const AnsiString &requestBody)
   {
      Bridge bridge = { &RestApiServer::JsonEscape_, &RestApiServer::BuildResponse_ };
      return GroupPut(ScriptingGroup, requestBody, bridge);
   }

   HttpResponse
   RestApiServer::HandleScriptingReload_()
   {
      ScriptServer::Instance()->LoadScripts();
      LOG_APPLICATION("RestApi: the event-handler script was loaded again.");
      return BuildResponse_(200, "{\"reloaded\":true}");
   }

   HttpResponse
   RestApiServer::HandleScriptingCheck_()
   {
      Bridge bridge = { &RestApiServer::JsonEscape_, &RestApiServer::BuildResponse_ };
      String result = ScriptServer::Instance()->CheckSyntax();
      return BuildResponse_(200, "{\"result\":\"" + bridge.escape(Utf8(result)) + "\"}");
   }

   // Settings.TestBreachCheckEndpoint over REST. The answer carries what the
   // endpoint said about itself and never what it sent: an administrator's key
   // must not turn this into a way of reading an arbitrary URL through the server.
   HttpResponse
   RestApiServer::HandleBreachCheckTest_(const AnsiString &requestBody)
   {
      Bridge bridge = { &RestApiServer::JsonEscape_, &RestApiServer::BuildResponse_ };

      String url;
      std::string trimmed(requestBody.c_str(), (size_t) requestBody.GetLength());
      if (trimmed.find_first_not_of(" \t\r\n") != std::string::npos)
      {
         JsonValue document;
         std::string parseError;
         if (!JsonValue::Parse(trimmed, document, parseError) || !document.IsObject())
            return Refusal(bridge, 400, "the body must be a JSON object");

         const JsonValue *member = document.Get("url");
         if (member && !member->IsNull())
         {
            if (!member->IsString())
               return Refusal(bridge, 400, "url must be a string");
            Unicode::MultiByteToWide(AnsiString(member->AsString().c_str()), url);
         }
      }

      if (!url.IsEmpty())
      {
         String reason;
         if (!PasswordBreachCheck::ValidateEndpoint(url, reason))
            return Refusal(bridge, 400, Utf8(reason));
      }

      int status = 0;
      int entries = 0;
      bool padded = false;
      int milliseconds = 0;
      String error;
      const bool reachable = PasswordBreachCheck::Probe(url, status, entries, padded, milliseconds, error);

      AnsiString body;
      if (reachable)
      {
         body.Format("{\"reachable\":true,\"status\":%d,\"entries\":%d,\"padded\":%s,\"milliseconds\":%d}",
            status, entries, padded ? "true" : "false", milliseconds);
      }
      else
      {
         body.Format("{\"reachable\":false,\"status\":%d,\"error\":\"%s\",\"milliseconds\":%d}",
            status, bridge.escape(Utf8(error)).c_str(), milliseconds);
      }

      LOG_APPLICATION(String(_T("RestApi: the password breach check endpoint was tested; it was ")) +
         (reachable ? String(_T("reachable.")) : String(_T("not reachable."))));

      return bridge.respond(200, body, "");
   }

   HttpResponse
   RestApiServer::HandleSettingsCache_()
   {
      Bridge bridge = { &RestApiServer::JsonEscape_, &RestApiServer::BuildResponse_ };
      return GroupGet(CacheGroup, bridge);
   }

   HttpResponse
   RestApiServer::HandleSettingsCachePut_(const AnsiString &requestBody)
   {
      Bridge bridge = { &RestApiServer::JsonEscape_, &RestApiServer::BuildResponse_ };
      return GroupPut(CacheGroup, requestBody, bridge);
   }

   // As InterfaceCache::Clear: the four object caches and the container of
   // open messages, so that nothing read before the clear is answered after it.
   HttpResponse
   RestApiServer::HandleCacheClear_()
   {
      Cache<Account>::Instance()->Clear();
      Cache<Domain>::Instance()->Clear();
      Cache<Alias>::Instance()->Clear();
      Cache<DistributionList>::Instance()->Clear();
      MessagesContainer::Instance()->Clear();
      // The domains' and the accounts' own setting values (docs/SettingLevels.md):
      // a row written behind the server's back is read now, not after its minute.
      CacheContainer::Instance()->ClearSettingOverrides();
      LOG_APPLICATION("RestApi: the caches were cleared.");
      return BuildResponse_(200, "{\"cleared\":true}");
   }

   HttpResponse
   RestApiServer::HandleSettingsIndexing_()
   {
      Bridge bridge = { &RestApiServer::JsonEscape_, &RestApiServer::BuildResponse_ };
      return GroupGet(IndexingGroup, bridge);
   }

   HttpResponse
   RestApiServer::HandleSettingsIndexingPut_(const AnsiString &requestBody)
   {
      Bridge bridge = { &RestApiServer::JsonEscape_, &RestApiServer::BuildResponse_ };
      return GroupPut(IndexingGroup, requestBody, bridge);
   }

   HttpResponse
   RestApiServer::HandleIndexingIndex_()
   {
      MessageIndexer::Instance()->IndexNow();
      LOG_APPLICATION("RestApi: the message indexer was asked to run now.");
      return BuildResponse_(200, "{\"started\":true}");
   }

   // As InterfaceMessageIndexing::Clear: the metadata, the full-text terms and
   // the backfill cursor together - a cursor left behind would clear the terms
   // and then never rebuild them.
   HttpResponse
   RestApiServer::HandleIndexingClear_()
   {
      PersistentMessageMetaData metaData;
      metaData.Clear();
      PersistentMessageIndex::Clear();
      LOG_APPLICATION("RestApi: the message index was cleared.");
      return BuildResponse_(200, "{\"cleared\":true}");
   }

   HttpResponse
   RestApiServer::HandleSettingsBackup_()
   {
      Bridge bridge = { &RestApiServer::JsonEscape_, &RestApiServer::BuildResponse_ };
      return GroupGet(BackupGroup, bridge);
   }

   HttpResponse
   RestApiServer::HandleSettingsBackupPut_(const AnsiString &requestBody)
   {
      Bridge bridge = { &RestApiServer::JsonEscape_, &RestApiServer::BuildResponse_ };
      return GroupPut(BackupGroup, requestBody, bridge);
   }

   HttpResponse
   RestApiServer::HandleSettingsBounces_()
   {
      Bridge bridge = { &RestApiServer::JsonEscape_, &RestApiServer::BuildResponse_ };
      return GroupGet(BounceGroup, bridge);
   }

   HttpResponse
   RestApiServer::HandleSettingsBouncesPut_(const AnsiString &requestBody)
   {
      Bridge bridge = { &RestApiServer::JsonEscape_, &RestApiServer::BuildResponse_ };
      return GroupPut(BounceGroup, requestBody, bridge);
   }

   HttpResponse
   RestApiServer::HandleSettingsOpenPgp_()
   {
      Bridge bridge = { &RestApiServer::JsonEscape_, &RestApiServer::BuildResponse_ };
      return GroupGet(OpenPgpGroup, bridge);
   }

   HttpResponse
   RestApiServer::HandleSettingsOpenPgpPut_(const AnsiString &requestBody)
   {
      Bridge bridge = { &RestApiServer::JsonEscape_, &RestApiServer::BuildResponse_ };
      return GroupPut(OpenPgpGroup, requestBody, bridge);
   }

   HttpResponse
   RestApiServer::HandleSettingsPush_()
   {
      Bridge bridge = { &RestApiServer::JsonEscape_, &RestApiServer::BuildResponse_ };
      return GroupGet(PushGroup, bridge);
   }

   HttpResponse
   RestApiServer::HandleSettingsPushPut_(const AnsiString &requestBody)
   {
      Bridge bridge = { &RestApiServer::JsonEscape_, &RestApiServer::BuildResponse_ };
      return GroupPut(PushGroup, requestBody, bridge);
   }

   /*
      A fresh VAPID key pair. The answer carries the PUBLIC half only - the
      private half never leaves this process - and the log line says what the
      administrator has just done, because the consequence is not obvious from the
      button: every subscription made under the old pair stops working, and every
      device has to be asked again.
   */
   HttpResponse
   RestApiServer::HandleSettingsPushVapid_()
   {
      String refusal;
      if (!Configuration::Instance()->GenerateWebPushVapidKeys(refusal))
         return BuildResponse_(500, "{\"error\":\"" + JsonEscape_(Utf8_(refusal)) + "\"}");

      LOG_APPLICATION("RestApi: a new Web Push VAPID key pair was generated. Every existing subscription is now invalid and each device must subscribe again.");

      AnsiString json = "{\"vapid_public_key\":\"" +
         JsonEscape_(Utf8_(Configuration::Instance()->GetWebPushVapidPublicKey())) + "\"}";

      return BuildResponse_(200, json);
   }

   HttpResponse
   RestApiServer::HandleSettingsComplaints_()
   {
      Bridge bridge = { &RestApiServer::JsonEscape_, &RestApiServer::BuildResponse_ };
      return GroupGet(ComplaintGroup, bridge);
   }

   HttpResponse
   RestApiServer::HandleSettingsComplaintsPut_(const AnsiString &requestBody)
   {
      Bridge bridge = { &RestApiServer::JsonEscape_, &RestApiServer::BuildResponse_ };
      return GroupPut(ComplaintGroup, requestBody, bridge);
   }

   HttpResponse
   RestApiServer::HandleServerMessageList_()
   {
      Bridge bridge = { &RestApiServer::JsonEscape_, &RestApiServer::BuildResponse_ };
      return ServerMessagesList(bridge);
   }

   HttpResponse
   RestApiServer::HandleServerMessagePut_(const AnsiString &name, const AnsiString &requestBody)
   {
      Bridge bridge = { &RestApiServer::JsonEscape_, &RestApiServer::BuildResponse_ };
      return ServerMessagePut(name, requestBody, bridge);
   }

   HttpResponse
   RestApiServer::HandleSieveEvaluate_(const AnsiString &requestBody)
   {
      Bridge bridge = { &RestApiServer::JsonEscape_, &RestApiServer::BuildResponse_ };
      return SieveEvaluate(requestBody, bridge);
   }

   HttpResponse
   RestApiServer::HandleCalendarExpand_(const AnsiString &requestBody)
   {
      Bridge bridge = { &RestApiServer::JsonEscape_, &RestApiServer::BuildResponse_ };
      return CalendarExpand(requestBody, bridge);
   }

   HttpResponse
   RestApiServer::HandleLogonFailuresClear_()
   {
      // Configuration::ClearOldLogonFailures, which is what
      // InterfaceSettings::ClearLogonFailureList calls: every recorded failure
      // deleted, whatever its age.
      if (!Config()->ClearOldLogonFailures())
         return BuildResponse_(500, "{\"error\":\"the logon-failure list could not be cleared\"}");

      // hm_logon_failures is traffic, not configuration, so the statement
      // chokepoint does not record the delete; clearing it is an unlock all
      // the same - an address one failure from a ban starts again at none -
      // and is recorded here, under the actor the dispatch installed, as
      // Settings.ClearLogonFailureList records it over COM.
      AuditTrail::Instance()->RecordEvent(_T("logon failures"), _T("every recorded failure"), _T("cleared"),
         _T("the failures the auto-ban counts were deleted, so every address starts again at none"));

      LOG_APPLICATION("RestApi: the logon-failure list was cleared.");

      return BuildResponse_(200, "{\"cleared\":true}");
   }
}

namespace
{
   using namespace HM;

   // Every group a setting can be written through. The directories are facts of
   // the installation and have no writable row.
   const Group *const WritableGroups[] =
   {
      &ServerGroup, &AntiSpamGroup, &AntiVirusGroup, &LoggingGroup, &ComplaintGroup, &ScriptingGroup,
      &BackupGroup, &CacheGroup, &IndexingGroup, &BounceGroup, &PushGroup, &OpenPgpGroup
   };

   // Which row holds which setting, found once per process (RestApiSettingsRows.h).
   // Keyed by the name in lower case. property_rows: the one faithful read-write
   // row a property's getter is, null where more than one row reads the property,
   // where the row that reads it reads another as well, or where the row's value is
   // not the property's own value as stored. write_only_rows: the write-only rows by
   // the property they name. read_only_rows: a property only read-only rows read.
   // ini_rows: the rows kept in the settings store, by their key.
   std::mutex property_rows_mutex;
   bool property_rows_built = false;
   std::map<String, const Row *> property_rows;
   std::map<String, const Row *> write_only_rows;
   std::map<String, const Row *> read_only_rows;
   std::map<String, const Row *> ini_rows;
   std::map<const Row *, const Group *> row_groups;

   String Lower(const String &name)
   {
      String key = name;
      key.MakeLower();
      return key;
   }

   // Whether the row answers the property as the property is stored: the same
   // text, the same number, or a switch that is on when the number is not 0.
   bool Faithful(const Row &row, const Value &value, const String &name, PropertySet &properties)
   {
      if (!properties.Exists(name))
         return false;

      switch (row.kind)
      {
      case KindString:
         return value.text == properties.GetString(name);
      case KindBoolean:
         return value.flag == (properties.GetLong(name) != 0);
      case KindInteger:
      case KindEnum:
         return value.number == properties.GetLong(name);
      }

      return false;
   }

   void BuildPropertyRows()
   {
      std::shared_ptr<PropertySet> properties = Configuration::Instance()->GetPropertySet();
      if (!properties || !ConfigurationLoaded())
         return;

      std::map<String, const Row *> readOnly;

      for (const Group *group : WritableGroups)
      {
         for (size_t i = 0; i < group->count; i++)
         {
            const Row &row = group->rows[i];
            row_groups[&row] = group;

            if (row.access == WriteOnly)
            {
               if (row.property && row.set)
                  write_only_rows[Lower(row.property)] = &row;
               continue;
            }

            if (row.ini_key)
            {
               if (row.access == ReadWrite && row.set)
                  ini_rows[Lower(row.ini_key)] = &row;
               continue;
            }

            if (!row.get)
               continue;

            std::vector<String> read;
            Value value;
            try
            {
               value = ReadRow(row, read);
            }
            catch (...)
            {
               continue;
            }

            if (row.access == ReadOnly)
            {
               if (read.size() == 1)
                  readOnly[Lower(read[0])] = &row;
               continue;
            }

            if (!row.set)
               continue;

            if (read.size() != 1)
            {
               for (const String &name : read)
                  property_rows[Lower(name)] = nullptr;
               continue;
            }

            const String key = Lower(read[0]);
            auto existing = property_rows.find(key);
            if (existing != property_rows.end())
            {
               (*existing).second = nullptr;
               continue;
            }

            property_rows[key] = Faithful(row, value, read[0], *properties) ? &row : nullptr;
         }
      }

      // A property a read-only row reads and no writable row writes.
      for (auto entry = readOnly.begin(); entry != readOnly.end(); entry++)
      {
         if (property_rows.find((*entry).first) == property_rows.end() &&
             write_only_rows.find((*entry).first) == write_only_rows.end())
            read_only_rows[(*entry).first] = (*entry).second;
      }
   }

   const Row *LentRow(bool iniKey, const String &name, bool &readOnly, bool &writeOnly)
   {
      readOnly = false;
      writeOnly = false;

      std::lock_guard<std::mutex> guard(property_rows_mutex);

      if (!property_rows_built)
      {
         BuildPropertyRows();
         property_rows_built = true;
      }

      const String key = Lower(name);

      if (iniKey)
      {
         auto found = ini_rows.find(key);
         return found == ini_rows.end() ? nullptr : (*found).second;
      }

      auto writing = write_only_rows.find(key);
      if (writing != write_only_rows.end())
      {
         writeOnly = true;
         return (*writing).second;
      }

      auto found = property_rows.find(key);
      if (found != property_rows.end())
         return (*found).second;

      readOnly = read_only_rows.find(key) != read_only_rows.end();
      return nullptr;
   }

   String Wide(const AnsiString &text)
   {
      String wide;
      Unicode::MultiByteToWide(text, wide);
      return wide;
   }

   RestApiSettingsRows::ValueKind KindOfRow(const Row &row)
   {
      switch (row.kind)
      {
      case KindString:
         return RestApiSettingsRows::KindText;
      case KindInteger:
         return RestApiSettingsRows::KindWholeNumber;
      case KindBoolean:
         return RestApiSettingsRows::KindSwitch;
      case KindEnum:
         return RestApiSettingsRows::KindWords;
      }

      return RestApiSettingsRows::KindNone;
   }

   // The value in the row's own form, or the sentence the row's type refuses it
   // with.
   String RowValue(const Row &row, long number, const String &text, Value &value)
   {
      switch (row.kind)
      {
      case KindString:
         value.text = text;
         return String();

      case KindInteger:
         value.number = number;
         return String();

      case KindBoolean:
         if (number != 0 && number != 1)
            return Wide(Named(std::string(row.key), "must be true or false"));
         value.flag = number != 0;
         return String();

      case KindEnum:
         if (!WordFor(row, number))
            return Wide(Named(std::string(row.key), "must be one of ") + WordList(row));
         value.number = number;
         return String();
      }

      return String();
   }
}

namespace HM
{
   namespace RestApiSettingsRows
   {
      bool
      Find(bool iniKey, const String &name, Lent &lent)
      {
         lent = Lent();

         bool readOnly = false;
         bool writeOnly = false;
         const Row *row = LentRow(iniKey, name, readOnly, writeOnly);
         lent.readOnly = readOnly;

         if (!row)
            return false;

         lent.kind = KindOfRow(*row);
         lent.key = row->key;
         lent.writeOnly = writeOnly;

         auto group = row_groups.find(row);
         if (group != row_groups.end())
            lent.path = (*group).second->path;

         if (row->kind == KindEnum)
            lent.words = Wide(WordList(*row));

         lent.applies = "now";
         if (row->ini_key)
         {
            String applies;
            String spelling;
            if (IniFileSettings::GetApplies(String(row->ini_key), applies, spelling))
               lent.applies = AnsiString(applies);
         }
         else if (row->effect == EffectRestart)
            lent.applies = "restart";

         return true;
      }

      String
      Check(bool iniKey, const String &name, long number, const String &text)
      {
         bool readOnly = false;
         bool writeOnly = false;
         const Row *row = LentRow(iniKey, name, readOnly, writeOnly);
         if (!row)
            return String();

         Value value;
         const String refused = RowValue(*row, number, text, value);
         if (!refused.IsEmpty())
            return refused;

         // The row's own check, in its own sentence: the value the REST route
         // would refuse is refused in the same words.
         if (row->check)
            return row->check(value);

         return String();
      }

      bool
      Apply(bool iniKey, const String &name, long number, const String &text, String &refusal)
      {
         refusal.Empty();

         bool readOnly = false;
         bool writeOnly = false;
         const Row *row = LentRow(iniKey, name, readOnly, writeOnly);
         if (!row || !row->set)
         {
            refusal = _T("No settings row writes this setting.");
            return false;
         }

         Value value;
         refusal = RowValue(*row, number, text, value);
         if (!refusal.IsEmpty())
            return false;

         const Outcome outcome = row->set(value);
         if (outcome.status != 0)
         {
            refusal = outcome.message;
            return false;
         }

         return true;
      }

      bool
      WordValue(bool iniKey, const String &name, const String &word, long &number, String &words)
      {
         words.Empty();

         bool readOnly = false;
         bool writeOnly = false;
         const Row *row = LentRow(iniKey, name, readOnly, writeOnly);
         if (!row || row->kind != KindEnum)
            return false;

         words = Wide(WordList(*row));

         const AnsiString narrow = Utf8(word);
         for (const Word *candidate = row->words; candidate && candidate->word; candidate++)
         {
            if (narrow == candidate->word)
            {
               number = candidate->value;
               return true;
            }
         }

         return false;
      }

      bool
      DeclaresProperty(const String &property)
      {
         for (const Group *group : WritableGroups)
         {
            for (size_t i = 0; i < group->count; i++)
            {
               const Row &row = group->rows[i];
               if (row.access == WriteOnly && row.property && row.set && property.CompareNoCase(row.property) == 0)
                  return true;
            }
         }

         return false;
      }
   }
}
