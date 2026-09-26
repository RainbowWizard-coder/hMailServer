// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "Configuration.h"
#include "../Util/Crypt.h"

#include "../BO/BlockedAttachments.h"
#include "../BO/SecurityRanges.h"
#include "../BO/ServerMessages.h"
#include "../BO/SSLCertificates.h"
#include "../Application/Property.h"
#include "../Application/IniSettingStore.h"
#include "../Application/Profiles.h"
#include "../Cache/CacheContainer.h"
#include "../Application/ObjectCache.h"
#include "../Scripting/ScriptServer.h"
#include "../Util/TLD.h"
#include "../TCPIP/LocalIPAddresses.h"

#include "../../POP3/POP3Configuration.h"
#include "../../SMTP/SMTPConfiguration.h"

#include "../../IMAP/IMAPConfiguration.h"
#include "../Cache/CacheConfiguration.h"
#include "../Util/AWStats.h"
#include "../Util/PasswordBreachCheck.h"
#include "../Util/ReputationWatch.h"
#include "../Util/WebPush.h"

// BACKUP
#include "../BO/Rules.h"
#include "../BO/TCPIPPorts.h"
#include "../BO/SSLCertificates.h"

#include "../Persistence/PersistentServerMessage.h"
#include "../Persistence/PersistentLogonFailure.h"

#include "../Application/MessageIndexer.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{

   Configuration::Configuration() :
      crash_simulation_mode_(0),
      ipv6_available_(false)
   {
   }

   Configuration::~Configuration()
   {  
   
   }

   bool 
   Configuration::Load()
   {
      IPAddress address;
      ipv6_available_ = address.TryParse("::F", false);

      property_set_ = std::shared_ptr<PropertySet>(new PropertySet());
      property_set_->Refresh();

      EnsureSettingDefaults_();

      // The two records a profile keeps (Profiles.h), made empty here while this is
      // the only thread, so that an apply on a request thread updates a row and never
      // inserts one into the set others are reading. Not settings, so not in
      // EnsureSettingDefaults_, which build/generate-settings-defaults.py reads as
      // the list of what a database ships with.
      Profiles::EnsureRecords(*property_set_);

      pop3_configuration_ = std::shared_ptr<POP3Configuration>(new POP3Configuration);
      smtp_configuration_ = std::shared_ptr<SMTPConfiguration>(new SMTPConfiguration);
      imap_configuration_ = std::shared_ptr<IMAPConfiguration>(new IMAPConfiguration);
      cache_configuration_ = std::shared_ptr<CacheConfiguration>(new CacheConfiguration);
      
      if (!smtp_configuration_->Load())
         return false;

      if (!anti_spam_configuration_.Load())
         return false;

      server_messages_ = std::shared_ptr<ServerMessages> (new ServerMessages);
      server_messages_->Refresh();

      blocked_attachments_ = std::shared_ptr<BlockedAttachments>(new BlockedAttachments);
      blocked_attachments_->Refresh();

      ssl_certificates_ = std::shared_ptr<SSLCertificates>(new SSLCertificates);
      ssl_certificates_->Refresh();

      ScriptServer::Instance()->LoadScripts();
      TLD::Instance()->Initialize();
      LocalIPAddresses::Instance()->LoadIPAddresses();

      return imap_configuration_->Load();
   }

   void
   Configuration::EnsureSettingDefaults_()
   {
      if (!property_set_)
         return;

      // The syslog sink. Every default is the server as it has always been: off,
      // with nowhere to send to. The rows exist all the same, because a read of a
      // missing property reports HM5015 - see PropertySet::EnsureLong.
      property_set_->EnsureLong(PROPERTY_SYSLOG_ENABLED, 0);
      property_set_->EnsureString(PROPERTY_SYSLOG_HOST, _T(""));
      property_set_->EnsureLong(PROPERTY_SYSLOG_PORT, 514);
      property_set_->EnsureLong(PROPERTY_SYSLOG_TRANSPORT, 0);          // UDP
      property_set_->EnsureLong(PROPERTY_SYSLOG_FACILITY, 2);           // mail
      property_set_->EnsureLong(PROPERTY_SYSLOG_SEVERITY, 6);           // informational

      // SMTP, POP3, IMAP and application events, which is what an administrator
      // watching a mail server wants. TCP/IP and debug are left out: both are
      // diagnostic chatter, and a collector charged by volume is a real cost.
      property_set_->EnsureLong(PROPERTY_SYSLOG_LOGTYPES,
         Logger::LSSMTP | Logger::LSPOP3 | Logger::LSApplication | Logger::LSIMAP);

      // The password breach check: off, the public corpus, one sighting, and an
      // endpoint that cannot be used lets the change through. Off is the server as
      // it has always been, which no change on upgrade may alter.
      property_set_->EnsureLong(PROPERTY_BREACH_CHECK_ENABLED, 0);
      property_set_->EnsureString(PROPERTY_BREACH_CHECK_URL, PasswordBreachCheck::DefaultUrl);

      // Spam traps: a day's ban, a month on the block list, three months of
      // record, and nothing taught to SpamAssassin. The rows exist from the
      // start because a read of a missing property reports HM5015, and a server
      // with no trap declared does none of it whatever they say.
      property_set_->EnsureLong(PROPERTY_SPAM_TRAP_BAN_MINUTES, 1440);
      property_set_->EnsureLong(PROPERTY_SPAM_TRAP_BLOCK_DAYS, 30);
      property_set_->EnsureLong(PROPERTY_SPAM_TRAP_HIT_RETENTION_DAYS, 90);
      property_set_->EnsureLong(PROPERTY_SPAM_TRAP_TEACH_SPAMASSASSIN, 0);

      // The message store's form (Persistence/MessageStore.h): off, and 256 KB
      // for the day it is turned on. Schema 6098 seeds the same two rows; these
      // are here because every delivery reads the first, and a read of a missing
      // property reports HM5015 every time.
      property_set_->EnsureLong(PROPERTY_MESSAGE_STORE_COMPRESSION, 0);
      property_set_->EnsureLong(PROPERTY_MESSAGE_STORE_DEDUP_MIN_KB, 256);

      // PST import: no directory, so no PST migration job is taken until an
      // administrator names one. The row exists from the start because a read
      // of a missing property reports HM5015.
      property_set_->EnsureString(PROPERTY_PST_IMPORT_DIRECTORY, _T(""));

      // The message store on an object store (Persistence/MessageStoreObjects.h,
      // docs/ObjectStorage.md): no store, and new mail on the local disk. A
      // server that never names a store reads and writes exactly as it did;
      // every delivery asks the first two through MessageStore::Backend.
      property_set_->EnsureString(PROPERTY_MESSAGE_STORE_BACKEND, _T("file"));
      property_set_->EnsureString(PROPERTY_MESSAGE_STORE_OBJECT_SERVICE, _T(""));
      property_set_->EnsureString(PROPERTY_MESSAGE_STORE_OBJECT_ENDPOINT, _T(""));
      property_set_->EnsureString(PROPERTY_MESSAGE_STORE_OBJECT_REGION, _T("us-east-1"));
      property_set_->EnsureString(PROPERTY_MESSAGE_STORE_OBJECT_BUCKET, _T(""));
      property_set_->EnsureString(PROPERTY_MESSAGE_STORE_OBJECT_ADDRESSING, _T("auto"));
      property_set_->EnsureString(PROPERTY_MESSAGE_STORE_OBJECT_PREFIX, _T(""));
      property_set_->EnsureString(PROPERTY_MESSAGE_STORE_OBJECT_ACCESS_KEY, _T(""));
      property_set_->EnsureString(PROPERTY_MESSAGE_STORE_OBJECT_SECRET, _T(""));
      property_set_->EnsureString(PROPERTY_MESSAGE_STORE_OBJECT_SAS, _T(""));
      property_set_->EnsureString(PROPERTY_MESSAGE_STORE_OBJECT_CA_FILE, _T(""));
      property_set_->EnsureLong(PROPERTY_MESSAGE_STORE_OBJECT_CACHE_MB, 32);

      // Greylisting, hardened (SMTP/GreyListing.h): no slide, no abuse threshold,
      // and an hour's lock that means nothing while the threshold is 0. Schema 6072
      // seeds the same three rows; these are here for the same reason as the rest.
      property_set_->EnsureLong(PROPERTY_GL_SLIDE_ON_EARLY_RETRY, 0);
      property_set_->EnsureLong(PROPERTY_GL_ABUSE_THRESHOLD, 0);
      property_set_->EnsureLong(PROPERTY_GL_ABUSE_LOCK_MINUTES, 60);
      property_set_->EnsureLong(PROPERTY_BREACH_CHECK_MINIMUM_COUNT, 1);

      // SMTP policy the score cannot express (discussions #283, #285, #286).
      // Every one at the value that leaves the server as it was: SPF's SoftFail,
      // TempError and PermError add nothing to the score, a TempError is
      // accepted rather than deferred, and a null sender naming several
      // recipients is neither refused nor banned. A ban, once switched on, lasts
      // a day, which is the operator's own suggestion; it is never applied while
      // the refusal is off.
      property_set_->EnsureLong(PROPERTY_USE_SPF_SOFTFAIL_SCORE, 0);
      property_set_->EnsureLong(PROPERTY_USE_SPF_TEMPERROR_SCORE, 0);
      property_set_->EnsureLong(PROPERTY_USE_SPF_PERMERROR_SCORE, 0);
      property_set_->EnsureLong(PROPERTY_SPF_TEMPERROR_DEFER, 0);
      property_set_->EnsureLong(PROPERTY_REJECT_MULTI_RCPT_NULL_SENDER, 0);
      property_set_->EnsureLong(PROPERTY_BAN_MULTI_RCPT_NULL_SENDER, 0);
      property_set_->EnsureLong(PROPERTY_BAN_MULTI_RCPT_NULL_SENDER_HOURS, 24);

      // The reputation watch: on, the four lists that answer an ordinary resolver
      // for nothing, and no address of its own. Schema 6049 seeds the same three
      // rows; these are here for the same reason the syslog sink's are, because a
      // read of a property no row holds reports HM5015 every time.
      //
      // On here and in the create scripts, and OFF in the upgrade scripts. A new
      // installation has no delivery bind address, no rule binding one and nothing
      // in ReputationWatchAddresses, so an installation that is on looks nothing
      // up until somebody points it at an address - which is the moment they chose
      // it. An existing server may well have a bind address set already, and
      // turning this on beneath it would start sending that address to four third
      // parties daily on the strength of an upgrade. This value therefore only
      // ever applies to a database with no row at all, which is a new one.
      property_set_->EnsureLong(PROPERTY_REPUTATION_WATCH_ENABLED, 1);
      property_set_->EnsureString(PROPERTY_REPUTATION_WATCH_LISTS, ReputationWatch::DefaultLists);
      property_set_->EnsureString(PROPERTY_REPUTATION_WATCH_ADDRESSES, _T(""));

      // The help panes' "Open on the wiki" link: the project's wiki, since that
      // is where the same documents are published. Empty hides the link.
      property_set_->EnsureString(PROPERTY_HELP_WIKI_BASE_URL, _T("https://gitlab.com/Progressiverobot/hmailserver/-/wikis/"));
      property_set_->EnsureLong(PROPERTY_BREACH_CHECK_UNREACHABLE_ACTION, PasswordBreachCheck::UnreachableAllow);

      // Bounces read, and the suppression list. Off, and with it off nothing below
      // is consulted. The other seven are what an administrator gets on switching it
      // on: one hard bounce lists an address, a full mailbox does not, a listed
      // recipient is refused with a reason, entries do not expire by age, delivery
      // reports are read, counts are kept 90 days and a report is matched for 7 days
      // after the message went, which is also how long a BATV tag stays valid.
      property_set_->EnsureLong(PROPERTY_BOUNCE_PROCESSING_ENABLED, 0);
      property_set_->EnsureLong(PROPERTY_BOUNCE_HARD_THRESHOLD, 1);
      property_set_->EnsureLong(PROPERTY_BOUNCE_MAILBOX_FULL_IS_HARD, 0);
      property_set_->EnsureLong(PROPERTY_BOUNCE_SUPPRESSION_ACTION, 0);
      property_set_->EnsureLong(PROPERTY_BOUNCE_SUPPRESSION_EXPIRY_DAYS, 0);
      property_set_->EnsureLong(PROPERTY_BOUNCE_READ_REPORTS, 1);
      property_set_->EnsureLong(PROPERTY_BOUNCE_COUNT_RETENTION_DAYS, 90);

      // Complaints, read: the feedback loops. Off, with no mailbox named, which is
      // the server as it has always been - it has never read an abuse report.
      //
      // The threshold ships at 0, which records and counts every report and lists
      // nobody. That is deliberately the observe-only setting and not the acting
      // one, and the reason is the feature's own threat model: the single thing
      // that tells a genuine report from one somebody wrote is that it arrived at
      // the mailbox, so whoever learns that address - and an operator who names
      // abuse@, which RFC 2142 publishes, has told everybody - can have any address
      // they choose refused for the whole domain with one message. An administrator
      // who has read some real entries raises it to 1, which is the value this
      // feature is for: a reader who pressed the spam button received the message
      // and did not want it. docs/Complaints.md says exactly this, and the shipped
      // default now agrees with it.
      //
      // The complaints themselves are kept for half a year.
      property_set_->EnsureLong(PROPERTY_COMPLAINT_PROCESSING_ENABLED, 0);
      property_set_->EnsureString(PROPERTY_COMPLAINT_MAILBOX, _T(""));
      property_set_->EnsureLong(PROPERTY_COMPLAINT_THRESHOLD, 0);
      property_set_->EnsureLong(PROPERTY_COMPLAINT_RETENTION_DAYS, 180);
      property_set_->EnsureLong(PROPERTY_BOUNCE_REPORT_WINDOW_DAYS, 7);

      // Web Push. Off, with no keys and no contact: a server that has never been
      // configured for it queues nothing and reaches no push service. A day's time
      // to live so that a phone that was off overnight is still told; ten devices
      // an account, which is more than anybody has; five attempts, the alert
      // webhook's number.
      property_set_->EnsureLong(PROPERTY_WEB_PUSH_ENABLED, 0);
      property_set_->EnsureString(PROPERTY_WEB_PUSH_SUBJECT, _T(""));
      property_set_->EnsureLong(PROPERTY_WEB_PUSH_TTL_SECONDS, 86400);
      property_set_->EnsureLong(PROPERTY_WEB_PUSH_MAX_PER_ACCOUNT, 10);
      property_set_->EnsureLong(PROPERTY_WEB_PUSH_MAX_ATTEMPTS, 5);
      property_set_->EnsureString(PROPERTY_WEB_PUSH_VAPID_PUBLIC_KEY, _T(""));
      property_set_->EnsureString(PROPERTY_WEB_PUSH_VAPID_PRIVATE_KEY, _T(""));
      // The regression suite's loopback push service, and nothing else: off.
      property_set_->EnsureLong(PROPERTY_WEB_PUSH_LOOPBACK_HTTP, 0);

      // The Web Key Directory. Off, which is the server as it has always been: no
      // key of anybody's is served until an administrator switches this on AND the
      // domain's switch is on AND the account's is. The other three are the limits
      // a key upload is judged against: 64 KiB of key, eight keys an account, and a
      // key whose user ids do not carry the account's own address is refused unless
      // an administrator says otherwise for that one upload.
      property_set_->EnsureLong(PROPERTY_OPENPGP_WKD_ENABLED, 0);
      property_set_->EnsureLong(PROPERTY_OPENPGP_KEY_MAXIMUM_BYTES, 65536);
      property_set_->EnsureLong(PROPERTY_OPENPGP_KEYS_PER_ACCOUNT, 8);
      property_set_->EnsureLong(PROPERTY_OPENPGP_REQUIRE_ADDRESS, 1);

      // OpenPGP in the webmail. The page offers it; a Web Key Directory lookup
      // is made when a correspondent's key is wanted and a keyserver one is not,
      // because the directory asks the correspondent's own domain and the
      // keyserver asks a third party who is nobody's mail server.
      property_set_->EnsureLong(PROPERTY_OPENPGP_PORTAL_ENABLED, 1);
      property_set_->EnsureLong(PROPERTY_OPENPGP_LOOKUP_WKD, 1);
      property_set_->EnsureLong(PROPERTY_OPENPGP_LOOKUP_KEYSERVER, 0);
      property_set_->EnsureString(PROPERTY_OPENPGP_KEYSERVER_URL, _T("https://keys.openpgp.org"));
      property_set_->EnsureLong(PROPERTY_OPENPGP_LOOKUP_TIMEOUT, 10);
      property_set_->EnsureLong(PROPERTY_PROTECTED_MESSAGE_DAYS, 30);

      // The list manager (Util/ListManager.h): no base URL, so a managed list
      // offers its mailto addresses and no link until an administrator says
      // where the pages are reached; one hard bounce, or five soft ones within
      // thirty days, stop a member being sent the list's mail. Nothing here
      // does anything until a list is managed.
      property_set_->EnsureString(PROPERTY_LIST_BASE_URL, _T(""));
      property_set_->EnsureLong(PROPERTY_LIST_HARD_BOUNCES, 1);
      property_set_->EnsureLong(PROPERTY_LIST_SOFT_BOUNCES, 5);
      property_set_->EnsureLong(PROPERTY_LIST_BOUNCE_WINDOW_DAYS, 30);

      // The organisation CA for S/MIME (Util/OrganisationCa.h): no base URL, so a
      // certificate a domain's authority issues names no CRL and no root to fetch
      // until an administrator says where the web services listener is reached.
      // Nothing is issued at all until an administrator creates an authority.
      property_set_->EnsureString(PROPERTY_SMIME_AUTHORITY_BASE_URL, _T(""));

      // Outbound shaping (SMTP/DestinationBackoff.h). The back-off on, at two
      // minutes doubling to an hour: a destination that says slow down is
      // obeyed on a stock installation, which is what a mail transfer agent is
      // expected to do. The two connection ceilings off, so nothing is held
      // that was not held before until an administrator sets one.
      property_set_->EnsureLong(PROPERTY_OUTBOUND_BACKOFF_MINUTES, 2);
      property_set_->EnsureLong(PROPERTY_OUTBOUND_BACKOFF_MAX_MINUTES, 60);
      property_set_->EnsureLong(PROPERTY_MAX_OUTBOUND_CONNS_PER_DEST, 0);
      property_set_->EnsureLong(PROPERTY_MAX_OUTBOUND_CONNS_PER_TENANT, 0);

      // Attachments opened somewhere safe (AntiVirus/Sandbox.h): OFF, with no
      // sandbox named. Schema 6091 seeds the same thirteen rows; these are here
      // because the poller reads SandboxPollSeconds every second, and a read of
      // a missing property reports HM5015 every time.
      property_set_->EnsureLong(PROPERTY_SANDBOX_ENABLED, 0);
      property_set_->EnsureString(PROPERTY_SANDBOX_SUBMIT_URL, _T(""));
      property_set_->EnsureString(PROPERTY_SANDBOX_VERDICT_URL, _T(""));
      property_set_->EnsureString(PROPERTY_SANDBOX_TOKEN, _T(""));
      property_set_->EnsureLong(PROPERTY_SANDBOX_TIMEOUT_MINUTES, 15);
      property_set_->EnsureLong(PROPERTY_SANDBOX_TIMEOUT_ACTION, 0);
      property_set_->EnsureLong(PROPERTY_SANDBOX_MAX_SIZE_KB, 20480);
      property_set_->EnsureString(PROPERTY_SANDBOX_TYPES, _T("exe,dll,scr,js,jse,vbs,wsf,hta,msi,bat,cmd,ps1,jar,lnk,iso,img,docm,xlsm,pptm,doc,xls,ppt,rtf,pdf,zip,7z,rar"));
      property_set_->EnsureLong(PROPERTY_SANDBOX_POLL_SECONDS, 30);
      property_set_->EnsureLong(PROPERTY_SANDBOX_SCORE_THRESHOLD, 70);
      property_set_->EnsureLong(PROPERTY_SANDBOX_CACHE_HOURS, 24);
      property_set_->EnsureLong(PROPERTY_SANDBOX_INTERNAL_MAIL, 0);
      property_set_->EnsureLong(PROPERTY_SANDBOX_MAX_FILES, 10);

      // The SNMP agent (Util/SnmpAgent.h): OFF, and even when it is switched on
      // it answers nobody until a manager's address is in the allow list - an
      // empty list is not "everyone". Port 161, every address, no SNMPv3 user,
      // SNMPv2c off with no community, and no contact or location. No schema step
      // seeds these; the agent reads them at every start, and a read of a missing
      // property reports HM5015 every time.
      property_set_->EnsureLong(PROPERTY_SNMP_ENABLED, 0);
      property_set_->EnsureLong(PROPERTY_SNMP_PORT, 161);
      property_set_->EnsureString(PROPERTY_SNMP_BIND_ADDRESS, _T(""));
      property_set_->EnsureString(PROPERTY_SNMP_ALLOWED_ADDRESSES, _T(""));
      property_set_->EnsureString(PROPERTY_SNMP_V3_USER, _T(""));
      property_set_->EnsureString(PROPERTY_SNMP_V3_AUTH_PASSWORD, _T(""));
      property_set_->EnsureString(PROPERTY_SNMP_V3_PRIV_PASSWORD, _T(""));
      property_set_->EnsureLong(PROPERTY_SNMP_V2C_ENABLED, 0);
      property_set_->EnsureString(PROPERTY_SNMP_V2C_COMMUNITY, _T(""));
      property_set_->EnsureString(PROPERTY_SNMP_CONTACT, _T(""));
      property_set_->EnsureString(PROPERTY_SNMP_LOCATION, _T(""));

      // Zero-access mailboxes: OFF. A mailbox with this on is one the
      // administrator cannot read, search, export or recover, and a lost
      // password without the recovery key is the mail lost; an installation
      // chooses that with its eyes open, once, here. Schema 6083 seeds the
      // same row; this is here because a read of a missing property reports
      // HM5015 every time.
      property_set_->EnsureLong(PROPERTY_ZERO_ACCESS_ALLOWED, 0);

      // Mailbox encryption at rest: an open key closes fifteen minutes after
      // its last use once no session holds it. Schema 6099 seeds the same row;
      // this is here because every sweep reads it.
      property_set_->EnsureLong(PROPERTY_MAILBOX_KEY_IDLE_MINUTES, 15);

      // The quota policy's two server-wide shares. Both 0, which is the server
      // as it has always been: no grace past a mailbox's limit, and no point at
      // which an account stops being able to send. A domain that sets its own
      // overrides them; these are what a domain that sets none inherits.
      property_set_->EnsureLong(PROPERTY_QUOTA_GRACE_PERCENT, 0);
      property_set_->EnsureLong(PROPERTY_QUOTA_SEND_STOP_PERCENT, 0);

      // Brand logos: on. Nothing is fetched until a reader opens a list holding
      // mail this server judged dmarc=pass and recorded so, which needs the
      // Authentication-Results header on; then one lookup a day per domain.
      property_set_->EnsureLong(PROPERTY_BRAND_LOGOS_ENABLED, 1);

      // Other accounts a reader adds in the webmail: three each, on public hosts
      // only. A private address is the administrator's to allow - an old mail
      // server inside the network being moved from - because the server makes the
      // connection, from inside that network, on the reader's word.
      property_set_->EnsureLong(PROPERTY_READER_FETCH_ACCOUNTS_MAX, 3);
      property_set_->EnsureLong(PROPERTY_READER_FETCH_PRIVATE_ADDRESSES, 0);

      // Importing with a sign-in: nothing is offered until an administrator
      // registers this server with a provider and says where it is reached from.
      // The endpoints are the providers' public ones.
      property_set_->EnsureString(PROPERTY_IMPORT_OAUTH_REDIRECT_BASE_URL, _T(""));

      // Addresses elsewhere a reader proves with a code and sends as: five each.
      property_set_->EnsureLong(PROPERTY_READER_SENDER_ADDRESSES_MAX, 5);

      // Masked addresses a reader makes for one sign-up each: twenty.
      property_set_->EnsureLong(PROPERTY_READER_MASKED_ADDRESSES_MAX, 20);

      // Deleted items kept for recovery: off, as the roadmap row says, since it
      // keeps a copy of everything a person deletes for as long as it says.
      property_set_->EnsureLong(PROPERTY_RECOVER_DELETED_DAYS, 0);

      // The organisation's directory: on, as Outlook's and Google's are, and
      // bounded to the reader's own domain. A server whose one domain holds
      // people who are not one organisation turns it off (DirectoryStore.h).
      property_set_->EnsureLong(PROPERTY_DIRECTORY_ENABLED, 1);

      // And a person may set their own title, telephone numbers and photo. Off
      // leaves those to the administrator; the opt-out stays the person's
      // either way.
      property_set_->EnsureLong(PROPERTY_DIRECTORY_USER_EDITABLE, 1);

      // The recovery address and the recovery codes: offered, but inert until a
      // person sets an address and proves it, and until the server has a public
      // address for the links to point at.
      property_set_->EnsureLong(PROPERTY_ACCOUNT_RECOVERY_ENABLED, 1);
      property_set_->EnsureLong(PROPERTY_RECOVERY_RESET_LINK_MINUTES, 30);
      property_set_->EnsureLong(PROPERTY_RECOVERY_VERIFY_LINK_HOURS, 24);

      // Offline in the webmail: the last thirty days, as the roadmap row says;
      // the reader chooses under it, and chooses nothing until they do.
      property_set_->EnsureLong(PROPERTY_READER_OFFLINE_DAYS_MAX, 30);

      // The sample domain: none made. Not a setting anybody sets - the row is
      // the marker SampleDomain writes when it makes example.test and clears
      // when it removes it, kept in hm_settings because that is where a value
      // the server has to find again on every start lives.
      property_set_->EnsureLong(PROPERTY_SAMPLE_DOMAIN_ID, 0);

      // Messages that expire: no base URL of their own until one is set; the
      // digest's or the import sign-in's serves until then.
      property_set_->EnsureString(PROPERTY_CONFIDENTIAL_BASE_URL, _T(""));

      // Outbound content policy: no account-number pattern until one is set;
      // the card and IBAN detectors need none.
      property_set_->EnsureString(PROPERTY_DLP_ACCOUNT_NUMBER_PATTERN, _T(""));

      // The reports our own domains receive: a year, which is what the paid
      // services keep, by the day the report was read. Nothing is read until a
      // domain names a report mailbox, so the row costs an upgraded server nothing.
      property_set_->EnsureLong(PROPERTY_DOMAIN_REPORT_RETENTION_DAYS, 365);
      // Compromised-account detection: WATCH by default. The baseline is
      // recorded and a takeover shape raises the alert saying what enforcing
      // would have done; nothing is held and nobody is locked until an
      // administrator has read that and set the mode to enforce, because the
      // false positive of this control is a person locked out of their mail.
      property_set_->EnsureLong(PROPERTY_COMPROMISE_DETECTION_MODE, 1);
      property_set_->EnsureLong(PROPERTY_COMPROMISE_BASELINE_DAYS, 30);
      property_set_->EnsureLong(PROPERTY_COMPROMISE_MINIMUM_HISTORY, 20);
      property_set_->EnsureLong(PROPERTY_COMPROMISE_VOLUME_FACTOR, 4);
      property_set_->EnsureLong(PROPERTY_COMPROMISE_VOLUME_FLOOR, 30);
      property_set_->EnsureLong(PROPERTY_COMPROMISE_RECIPIENT_FLOOR, 20);
      property_set_->EnsureLong(PROPERTY_COMPROMISE_NEW_RECIPIENT_SHARE, 80);
      property_set_->EnsureLong(PROPERTY_COMPROMISE_SCORE_TO_LOCK, 3);
      property_set_->EnsureLong(PROPERTY_COMPROMISE_HELD_DAYS, 7);
      property_set_->EnsureString(PROPERTY_IMPORT_OAUTH_GOOGLE_CLIENT_ID, _T(""));
      property_set_->EnsureString(PROPERTY_IMPORT_OAUTH_GOOGLE_CLIENT_SECRET, _T(""));
      property_set_->EnsureString(PROPERTY_IMPORT_OAUTH_GOOGLE_AUTHORIZE_URL, _T("https://accounts.google.com/o/oauth2/v2/auth"));
      property_set_->EnsureString(PROPERTY_IMPORT_OAUTH_GOOGLE_TOKEN_URL, _T("https://oauth2.googleapis.com/token"));
      property_set_->EnsureString(PROPERTY_IMPORT_OAUTH_MICROSOFT_CLIENT_ID, _T(""));
      property_set_->EnsureString(PROPERTY_IMPORT_OAUTH_MICROSOFT_CLIENT_SECRET, _T(""));
      property_set_->EnsureString(PROPERTY_IMPORT_OAUTH_MICROSOFT_AUTHORIZE_URL, _T("https://login.microsoftonline.com/common/oauth2/v2.0/authorize"));
      property_set_->EnsureString(PROPERTY_IMPORT_OAUTH_MICROSOFT_TOKEN_URL, _T("https://login.microsoftonline.com/common/oauth2/v2.0/token"));

      // Passkeys: offered, because offering one changes nothing until somebody
      // enrols one, and a person who never does signs in with their password
      // exactly as before. The relying party id is empty, which means the host
      // the page was served on.
      property_set_->EnsureLong(PROPERTY_PASSKEYS_ENABLED, 1);
      property_set_->EnsureString(PROPERTY_PASSKEY_RELYING_PARTY, _T(""));

      // The origins a passkey may be used at: empty, which means the host the
      // page was served on, until a relying party is set - and then they must
      // be named, since a parent-domain relying party is one every sibling of
      // this server could ask a passkey for (PasskeyStore.h).
      property_set_->EnsureString(PROPERTY_PASSKEY_ALLOWED_ORIGINS, _T(""));

      // Single sign-on: off, because there is no issuer until an administrator
      // names one, and no account is created for a stranger who signs in at one
      // until auto-provisioning is switched on as well.
      property_set_->EnsureString(PROPERTY_OIDC_ISSUER, _T(""));
      property_set_->EnsureString(PROPERTY_OIDC_CLIENT_ID, _T(""));
      property_set_->EnsureString(PROPERTY_OIDC_CLIENT_SECRET, _T(""));
      property_set_->EnsureString(PROPERTY_OIDC_SCOPES, _T("openid email"));
      property_set_->EnsureLong(PROPERTY_OIDC_AUTO_PROVISION, 0);
      property_set_->EnsureString(PROPERTY_OIDC_ADMINISTRATORS, _T(""));
      property_set_->EnsureString(PROPERTY_OIDC_REDIRECT_BASE_URL, _T(""));

      // SAML sign-in: off, for the same reason - no identity provider until an
      // administrator names one - and with an Assertion's own signature
      // demanded, provisioning off and nobody opening the Control Deck.
      property_set_->EnsureString(PROPERTY_SAML_IDP_ENTITY_ID, _T(""));
      property_set_->EnsureString(PROPERTY_SAML_IDP_SSO_URL, _T(""));
      property_set_->EnsureString(PROPERTY_SAML_IDP_CERTIFICATE, _T(""));
      property_set_->EnsureString(PROPERTY_SAML_SP_ENTITY_ID, _T(""));
      property_set_->EnsureString(PROPERTY_SAML_EMAIL_ATTRIBUTE, _T("http://schemas.xmlsoap.org/ws/2005/05/identity/claims/emailaddress"));
      property_set_->EnsureLong(PROPERTY_SAML_REQUIRE_SIGNED_ASSERTION, 1);
      property_set_->EnsureLong(PROPERTY_SAML_AUTO_PROVISION, 0);
      property_set_->EnsureString(PROPERTY_SAML_ADMINISTRATORS, _T(""));
      property_set_->EnsureString(PROPERTY_SAML_BUTTON_LABEL, _T("Sign in with SAML"));
   }

   void
   Configuration::OnPropertyChanged(std::shared_ptr<Property> pProperty)
   {
      String sPropertyName = pProperty->GetName();
      if (sPropertyName == PROPERTY_LOGGING)
      {
         // Activate the logger.
         Logger::Instance()->SetLogMask(pProperty->GetLongValue());
      }
      else if (sPropertyName == PROPERTY_LOGDEVICE)
      {
         // Activate the configured log device. PropertySet::Refresh() raises a
         // change event for every property when the settings are loaded, so this
         // carries the stored value at startup as well as on change - exactly how
         // PROPERTY_LOGGING above gets its initial value. Having the Logger read
         // the setting itself instead is not an option: it logs during startup,
         // before the property set exists.
         Logger::Instance()->SetLogDevice(pProperty->GetLongValue());
      }
      else if (sPropertyName == PROPERTY_LOGFORMAT)
      {
         Logger::Instance()->SetLogFormat(pProperty->GetLongValue());
      }
      else if (sPropertyName == PROPERTY_AWSTATSENABLED)
      {
         AWStats::SetEnabled(GetAWStatsEnabled());
      }
      else
      {
         CacheContainer::Instance()->OnPropertyChanged(pProperty);
         Application::Instance()->OnPropertyChanged(pProperty);
      } 

      if (smtp_configuration_)
         smtp_configuration_->OnPropertyChanged(pProperty);
   }


   std::shared_ptr<PropertySet>
   Configuration::GetSettings() const
   {
      return property_set_;
   }

   std::shared_ptr<TCPIPPorts>
   Configuration::GetTCPIPPorts() const
   {
      std::shared_ptr<TCPIPPorts> tcpipPorts = std::shared_ptr<TCPIPPorts>(new TCPIPPorts);
      tcpipPorts->Refresh();
      return tcpipPorts;
   }

   int
   Configuration::GetWorkerThreadPriority() const
   {
      return (GetSettings()->GetLong(PROPERTY_WORKERTHREADPRIORITY));
   }

   void
   Configuration::SetWorkerThreadPriority(int iNewVal)
   {
      GetSettings()->SetLong(PROPERTY_WORKERTHREADPRIORITY, iNewVal);
   }

   int
   Configuration::GetTCPIPThreads() const
   {
      return (GetSettings()->GetLong(PROPERTY_TCPIPTHREADS));
   }

   void
   Configuration::SetTCPIPThreads(int iNewVal)
   {
      GetSettings()->SetLong(PROPERTY_TCPIPTHREADS, iNewVal);
   }

   bool
   Configuration::GetUseLogging() const
   {
      return (GetSettings()->GetLong(PROPERTY_LOGGING)& Logger::LSEnabled) ? true : false;
   }

   void
   Configuration::SetUseLogging(bool bLogging)
   {
      int iLogging = GetSettings()->GetLong(PROPERTY_LOGGING);

      if (bLogging)
         iLogging = iLogging | Logger::LSEnabled;
      else
         iLogging = iLogging & ~Logger::LSEnabled;

      // Save setting
      GetSettings()->SetLong(PROPERTY_LOGGING, iLogging);
   }


   bool
   Configuration::GetUseSMTP() const
   {
      return GetSettings()->GetBool(PROPERTY_PROTOCOLSMTP);
   }

   void
   Configuration::SetUseSMTP(bool UseSMTP)
   {
      GetSettings()->SetBool(PROPERTY_PROTOCOLSMTP, UseSMTP);
   }

   bool
   Configuration::GetUsePOP3() const
   {
      return GetSettings()->GetBool(PROPERTY_PROTOCOLPOP3);
   }

   void
   Configuration::SetUsePOP3(bool UsePOP3)
   {
      GetSettings()->SetBool(PROPERTY_PROTOCOLPOP3, UsePOP3);
   }


   bool
   Configuration::GetUseIMAP() const
   {
      return GetSettings()->GetBool(PROPERTY_PROTOCOLIMAP);
   }

   void
   Configuration::SetUseIMAP(bool UseIMAP)
   {
      GetSettings()->SetBool(PROPERTY_PROTOCOLIMAP, UseIMAP);
   }


   bool
   Configuration::GetAWStatsEnabled() const
   {
      return GetSettings()->GetBool(PROPERTY_AWSTATSENABLED);
   }

   void
   Configuration::SetAWStatsEnabled(bool bNewVal)
   {
      GetSettings()->SetBool(PROPERTY_AWSTATSENABLED, bNewVal);

      // Update the logger
      AWStats::SetEnabled(bNewVal);
   }

   long
   Configuration::GetLogFormat() const
   {
      return GetSettings()->GetLong(PROPERTY_LOGFORMAT);
   }

   void
   Configuration::SetLogFormat(long lNewVal)
   {
      GetSettings()->SetLong(PROPERTY_LOGFORMAT, lNewVal);
   }

   void 
   Configuration::SetMirrorAddress(const String &sAddress)
   {
      GetSettings()->SetString(PROPERTY_MIRROREMAILADDRESS, sAddress);
   }

   String
   Configuration::GetMirrorAddress() const
   {
      return GetSettings()->GetString(PROPERTY_MIRROREMAILADDRESS);
   }

   void
   Configuration::SetQuarantineDigestBaseUrl(const String &url)
   {
      GetSettings()->SetString(PROPERTY_QUARANTINE_DIGEST_BASE_URL, url);
   }

   String
   Configuration::GetQuarantineDigestBaseUrl() const
   {
      return GetSettings()->GetString(PROPERTY_QUARANTINE_DIGEST_BASE_URL);
   }

   void
   Configuration::SetQuarantineDigestLinkHours(int hours)
   {
      GetSettings()->SetLong(PROPERTY_QUARANTINE_DIGEST_LINK_HOURS, hours);
   }

   int
   Configuration::GetQuarantineDigestLinkHours() const
   {
      return (int) GetSettings()->GetLong(PROPERTY_QUARANTINE_DIGEST_LINK_HOURS);
   }

   bool
   Configuration::GetBrandLogosEnabled() const
   {
      return GetSettings()->GetLong(PROPERTY_BRAND_LOGOS_ENABLED) != 0;
   }

   void
   Configuration::SetBrandLogosEnabled(bool enabled)
   {
      GetSettings()->SetLong(PROPERTY_BRAND_LOGOS_ENABLED, enabled ? 1 : 0);
   }

   int
   Configuration::GetReaderFetchAccountsMax() const
   {
      return (int) GetSettings()->GetLong(PROPERTY_READER_FETCH_ACCOUNTS_MAX);
   }

   void
   Configuration::SetReaderFetchAccountsMax(int count)
   {
      GetSettings()->SetLong(PROPERTY_READER_FETCH_ACCOUNTS_MAX, count);
   }

   bool
   Configuration::GetReaderFetchAccountsPrivateAddresses() const
   {
      return GetSettings()->GetLong(PROPERTY_READER_FETCH_PRIVATE_ADDRESSES) != 0;
   }

   void
   Configuration::SetReaderFetchAccountsPrivateAddresses(bool allowed)
   {
      GetSettings()->SetLong(PROPERTY_READER_FETCH_PRIVATE_ADDRESSES, allowed ? 1 : 0);
   }

   int
   Configuration::GetReaderSenderAddressesMax() const
   {
      return (int) GetSettings()->GetLong(PROPERTY_READER_SENDER_ADDRESSES_MAX);
   }

   void
   Configuration::SetReaderSenderAddressesMax(int count)
   {
      GetSettings()->SetLong(PROPERTY_READER_SENDER_ADDRESSES_MAX, count);
   }

   int
   Configuration::GetReaderMaskedAddressesMax() const
   {
      return (int) GetSettings()->GetLong(PROPERTY_READER_MASKED_ADDRESSES_MAX);
   }

   void
   Configuration::SetReaderMaskedAddressesMax(int count)
   {
      GetSettings()->SetLong(PROPERTY_READER_MASKED_ADDRESSES_MAX, count);
   }

   bool
   Configuration::GetDirectoryEnabled() const
   {
      return GetSettings()->GetLong(PROPERTY_DIRECTORY_ENABLED) != 0;
   }

   void
   Configuration::SetDirectoryEnabled(bool enabled)
   {
      GetSettings()->SetLong(PROPERTY_DIRECTORY_ENABLED, enabled ? 1 : 0);
   }

   bool
   Configuration::GetDirectoryUserEditable() const
   {
      return GetSettings()->GetLong(PROPERTY_DIRECTORY_USER_EDITABLE) != 0;
   }

   void
   Configuration::SetDirectoryUserEditable(bool editable)
   {
      GetSettings()->SetLong(PROPERTY_DIRECTORY_USER_EDITABLE, editable ? 1 : 0);
   }

   int
   Configuration::GetRecoverDeletedDays() const
   {
      return (int) GetSettings()->GetLong(PROPERTY_RECOVER_DELETED_DAYS);
   }

   void
   Configuration::SetRecoverDeletedDays(int days)
   {
      GetSettings()->SetLong(PROPERTY_RECOVER_DELETED_DAYS, days);
   }

   bool
   Configuration::GetAccountRecoveryEnabled() const
   {
      return GetSettings()->GetLong(PROPERTY_ACCOUNT_RECOVERY_ENABLED) == 1;
   }

   void
   Configuration::SetAccountRecoveryEnabled(bool enabled)
   {
      GetSettings()->SetLong(PROPERTY_ACCOUNT_RECOVERY_ENABLED, enabled ? 1 : 0);
   }

   int
   Configuration::GetRecoveryResetLinkMinutes() const
   {
      return (int) GetSettings()->GetLong(PROPERTY_RECOVERY_RESET_LINK_MINUTES);
   }

   void
   Configuration::SetRecoveryResetLinkMinutes(int minutes)
   {
      GetSettings()->SetLong(PROPERTY_RECOVERY_RESET_LINK_MINUTES, minutes);
   }

   int
   Configuration::GetRecoveryVerifyLinkHours() const
   {
      return (int) GetSettings()->GetLong(PROPERTY_RECOVERY_VERIFY_LINK_HOURS);
   }

   void
   Configuration::SetRecoveryVerifyLinkHours(int hours)
   {
      GetSettings()->SetLong(PROPERTY_RECOVERY_VERIFY_LINK_HOURS, hours);
   }

   int
   Configuration::GetReaderOfflineDaysMax() const
   {
      return (int) GetSettings()->GetLong(PROPERTY_READER_OFFLINE_DAYS_MAX);
   }

   void
   Configuration::SetReaderOfflineDaysMax(int days)
   {
      GetSettings()->SetLong(PROPERTY_READER_OFFLINE_DAYS_MAX, days);
   }

   __int64
   Configuration::GetSampleDomainId() const
   {
      return (__int64) GetSettings()->GetLong(PROPERTY_SAMPLE_DOMAIN_ID);
   }

   void
   Configuration::SetSampleDomainId(__int64 domainId)
   {
      GetSettings()->SetLong(PROPERTY_SAMPLE_DOMAIN_ID, (long) domainId);
   }

   String
   Configuration::GetConfidentialBaseUrl() const
   {
      return GetSettings()->GetString(PROPERTY_CONFIDENTIAL_BASE_URL);
   }

   void
   Configuration::SetConfidentialBaseUrl(const String &url)
   {
      GetSettings()->SetString(PROPERTY_CONFIDENTIAL_BASE_URL, url);
   }

   String
   Configuration::GetDlpAccountNumberPattern() const
   {
      return GetSettings()->GetString(PROPERTY_DLP_ACCOUNT_NUMBER_PATTERN);
   }

   void
   Configuration::SetDlpAccountNumberPattern(const String &pattern)
   {
      GetSettings()->SetString(PROPERTY_DLP_ACCOUNT_NUMBER_PATTERN, pattern);
   }

   int
   Configuration::GetDomainReportRetentionDays() const
   {
      return (int) GetSettings()->GetLong(PROPERTY_DOMAIN_REPORT_RETENTION_DAYS);
   }

   void
   Configuration::SetDomainReportRetentionDays(int days)
   {
      GetSettings()->SetLong(PROPERTY_DOMAIN_REPORT_RETENTION_DAYS, days);
   }
   int Configuration::GetCompromiseDetectionMode() const { return (int) GetSettings()->GetLong(PROPERTY_COMPROMISE_DETECTION_MODE); }
   void Configuration::SetCompromiseDetectionMode(int mode) { GetSettings()->SetLong(PROPERTY_COMPROMISE_DETECTION_MODE, mode); }
   int Configuration::GetCompromiseBaselineDays() const { return (int) GetSettings()->GetLong(PROPERTY_COMPROMISE_BASELINE_DAYS); }
   void Configuration::SetCompromiseBaselineDays(int days) { GetSettings()->SetLong(PROPERTY_COMPROMISE_BASELINE_DAYS, days); }
   int Configuration::GetCompromiseMinimumHistory() const { return (int) GetSettings()->GetLong(PROPERTY_COMPROMISE_MINIMUM_HISTORY); }
   void Configuration::SetCompromiseMinimumHistory(int messages) { GetSettings()->SetLong(PROPERTY_COMPROMISE_MINIMUM_HISTORY, messages); }
   int Configuration::GetCompromiseVolumeFactor() const { return (int) GetSettings()->GetLong(PROPERTY_COMPROMISE_VOLUME_FACTOR); }
   void Configuration::SetCompromiseVolumeFactor(int factor) { GetSettings()->SetLong(PROPERTY_COMPROMISE_VOLUME_FACTOR, factor); }
   int Configuration::GetCompromiseVolumeFloor() const { return (int) GetSettings()->GetLong(PROPERTY_COMPROMISE_VOLUME_FLOOR); }
   void Configuration::SetCompromiseVolumeFloor(int messages) { GetSettings()->SetLong(PROPERTY_COMPROMISE_VOLUME_FLOOR, messages); }
   int Configuration::GetCompromiseRecipientFloor() const { return (int) GetSettings()->GetLong(PROPERTY_COMPROMISE_RECIPIENT_FLOOR); }
   void Configuration::SetCompromiseRecipientFloor(int recipients) { GetSettings()->SetLong(PROPERTY_COMPROMISE_RECIPIENT_FLOOR, recipients); }
   int Configuration::GetCompromiseNewRecipientShare() const { return (int) GetSettings()->GetLong(PROPERTY_COMPROMISE_NEW_RECIPIENT_SHARE); }
   void Configuration::SetCompromiseNewRecipientShare(int percent) { GetSettings()->SetLong(PROPERTY_COMPROMISE_NEW_RECIPIENT_SHARE, percent); }
   int Configuration::GetCompromiseScoreToLock() const { return (int) GetSettings()->GetLong(PROPERTY_COMPROMISE_SCORE_TO_LOCK); }
   void Configuration::SetCompromiseScoreToLock(int score) { GetSettings()->SetLong(PROPERTY_COMPROMISE_SCORE_TO_LOCK, score); }
   int Configuration::GetCompromiseHeldDays() const { return (int) GetSettings()->GetLong(PROPERTY_COMPROMISE_HELD_DAYS); }
   void Configuration::SetCompromiseHeldDays(int days) { GetSettings()->SetLong(PROPERTY_COMPROMISE_HELD_DAYS, days); }

   String
   Configuration::GetImportOAuthRedirectBaseUrl() const
   {
      return GetSettings()->GetString(PROPERTY_IMPORT_OAUTH_REDIRECT_BASE_URL);
   }

   void
   Configuration::SetImportOAuthRedirectBaseUrl(const String &url)
   {
      GetSettings()->SetString(PROPERTY_IMPORT_OAUTH_REDIRECT_BASE_URL, url);
   }

   String
   Configuration::GetImportOAuthString(const String &propertyName) const
   {
      return GetSettings()->GetString(propertyName);
   }

   void
   Configuration::SetImportOAuthString(const String &propertyName, const String &value)
   {
      GetSettings()->SetString(propertyName, value);
   }

   // Passkeys and single sign-on. Both groups are read through one pair of
   // string accessors and one pair of number accessors, as the import sign-in's
   // settings are: every one of them is a plain hm_settings row, the names are
   // constants in Constants.h, and a setter of its own per row would be nine
   // pairs of functions that all say GetString.
   bool
   Configuration::GetPasskeysEnabled() const
   {
      return GetSettings()->GetBool(PROPERTY_PASSKEYS_ENABLED);
   }

   void
   Configuration::SetPasskeysEnabled(bool enabled)
   {
      GetSettings()->SetBool(PROPERTY_PASSKEYS_ENABLED, enabled);
   }

   bool
   Configuration::GetOidcAutoProvision() const
   {
      return GetSettings()->GetBool(PROPERTY_OIDC_AUTO_PROVISION);
   }

   void
   Configuration::SetOidcAutoProvision(bool enabled)
   {
      GetSettings()->SetBool(PROPERTY_OIDC_AUTO_PROVISION, enabled);
   }

   bool
   Configuration::GetSamlAutoProvision() const
   {
      return GetSettings()->GetBool(PROPERTY_SAML_AUTO_PROVISION);
   }

   void
   Configuration::SetSamlAutoProvision(bool enabled)
   {
      GetSettings()->SetBool(PROPERTY_SAML_AUTO_PROVISION, enabled);
   }

   bool
   Configuration::GetSamlRequireSignedAssertion() const
   {
      return GetSettings()->GetBool(PROPERTY_SAML_REQUIRE_SIGNED_ASSERTION);
   }

   void
   Configuration::SetSamlRequireSignedAssertion(bool required)
   {
      GetSettings()->SetBool(PROPERTY_SAML_REQUIRE_SIGNED_ASSERTION, required);
   }

   String
   Configuration::GetSignInString(const String &propertyName) const
   {
      return GetSettings()->GetString(propertyName);
   }

   void
   Configuration::SetSignInString(const String &propertyName, const String &value)
   {
      GetSettings()->SetString(propertyName, value);
   }

   // Country blocking. Nothing here validates: the policy grammar is
   // CountryBlocking::Canonicalise's, and every writing surface - COM, REST,
   // hmctl and the Deck - runs a value through it before it reaches these, so
   // that all four refuse a bad policy with the same sentence. A value written
   // straight into the database by hand and not understood is read as "off".
   void
   Configuration::SetGeoIpDatabaseFile(const String &path)
   {
      GetSettings()->SetString(PROPERTY_GEOIP_DATABASE_FILE, path);
   }

   String
   Configuration::GetGeoIpDatabaseFile() const
   {
      return GetSettings()->GetString(PROPERTY_GEOIP_DATABASE_FILE);
   }

   void
   Configuration::SetGeoIpDatabaseMaxAgeDays(int days)
   {
      GetSettings()->SetLong(PROPERTY_GEOIP_DATABASE_MAX_AGE_DAYS, days);
   }

   int
   Configuration::GetGeoIpDatabaseMaxAgeDays() const
   {
      return (int) GetSettings()->GetLong(PROPERTY_GEOIP_DATABASE_MAX_AGE_DAYS);
   }

   void
   Configuration::SetCountryBlockingExemptRanges(const String &ranges)
   {
      GetSettings()->SetString(PROPERTY_COUNTRY_EXEMPT_RANGES, ranges);
   }

   String
   Configuration::GetCountryBlockingExemptRanges() const
   {
      return GetSettings()->GetString(PROPERTY_COUNTRY_EXEMPT_RANGES);
   }

   void
   Configuration::SetCountryPolicySMTP(const String &policy)
   {
      GetSettings()->SetString(PROPERTY_COUNTRY_POLICY_SMTP, policy);
   }

   String
   Configuration::GetCountryPolicySMTP() const
   {
      return GetSettings()->GetString(PROPERTY_COUNTRY_POLICY_SMTP);
   }

   void
   Configuration::SetCountryPolicyIMAP(const String &policy)
   {
      GetSettings()->SetString(PROPERTY_COUNTRY_POLICY_IMAP, policy);
   }

   String
   Configuration::GetCountryPolicyIMAP() const
   {
      return GetSettings()->GetString(PROPERTY_COUNTRY_POLICY_IMAP);
   }

   void
   Configuration::SetCountryPolicyPOP3(const String &policy)
   {
      GetSettings()->SetString(PROPERTY_COUNTRY_POLICY_POP3, policy);
   }

   String
   Configuration::GetCountryPolicyPOP3() const
   {
      return GetSettings()->GetString(PROPERTY_COUNTRY_POLICY_POP3);
   }

   void
   Configuration::SetCountryPolicyHTTP(const String &policy)
   {
      GetSettings()->SetString(PROPERTY_COUNTRY_POLICY_HTTP, policy);
   }

   String
   Configuration::GetCountryPolicyHTTP() const
   {
      return GetSettings()->GetString(PROPERTY_COUNTRY_POLICY_HTTP);
   }

   void
   Configuration::SetSessionIdleSeconds(int seconds)
   {
      GetSettings()->SetLong(PROPERTY_SESSION_IDLE_SECONDS, seconds);
   }

   int
   Configuration::GetSessionIdleSeconds() const
   {
      return (int) GetSettings()->GetLong(PROPERTY_SESSION_IDLE_SECONDS);
   }

   void
   Configuration::SetSessionLifetimeSeconds(int seconds)
   {
      GetSettings()->SetLong(PROPERTY_SESSION_LIFETIME_SECONDS, seconds);
   }

   int
   Configuration::GetSessionLifetimeSeconds() const
   {
      return (int) GetSettings()->GetLong(PROPERTY_SESSION_LIFETIME_SECONDS);
   }

   void
   Configuration::SetSessionOnePerDevice(bool value)
   {
      GetSettings()->SetLong(PROPERTY_SESSION_ONE_PER_DEVICE, value ? 1 : 0);
   }

   bool
   Configuration::GetSessionOnePerDevice() const
   {
      return GetSettings()->GetLong(PROPERTY_SESSION_ONE_PER_DEVICE) != 0;
   }

   void
   Configuration::SetSessionBindToAddress(bool value)
   {
      GetSettings()->SetLong(PROPERTY_SESSION_BIND_TO_ADDRESS, value ? 1 : 0);
   }

   bool
   Configuration::GetSessionBindToAddress() const
   {
      return GetSettings()->GetLong(PROPERTY_SESSION_BIND_TO_ADDRESS) != 0;
   }

   bool
   Configuration::GetReputationWatchEnabled() const
   {
      return GetSettings()->GetBool(PROPERTY_REPUTATION_WATCH_ENABLED);
   }

   void
   Configuration::SetReputationWatchEnabled(bool newVal)
   {
      GetSettings()->SetBool(PROPERTY_REPUTATION_WATCH_ENABLED, newVal);
   }

   String
   Configuration::GetReputationWatchLists() const
   {
      return GetSettings()->GetString(PROPERTY_REPUTATION_WATCH_LISTS);
   }

   void
   Configuration::SetReputationWatchLists(const String &newVal)
   {
      GetSettings()->SetString(PROPERTY_REPUTATION_WATCH_LISTS, newVal);
   }

   String
   Configuration::GetReputationWatchAddresses() const
   {
      return GetSettings()->GetString(PROPERTY_REPUTATION_WATCH_ADDRESSES);
   }

   void
   Configuration::SetReputationWatchAddresses(const String &newVal)
   {
      GetSettings()->SetString(PROPERTY_REPUTATION_WATCH_ADDRESSES, newVal);
   }

   String
   Configuration::GetHelpWikiBaseUrl() const
   {
      return GetSettings()->GetString(PROPERTY_HELP_WIKI_BASE_URL);
   }

   void
   Configuration::SetHelpWikiBaseUrl(const String &newVal)
   {
      GetSettings()->SetString(PROPERTY_HELP_WIKI_BASE_URL, newVal);
   }

   int
   Configuration::GetSpamTrapBanMinutes() const
   {
      return (int) GetSettings()->GetLong(PROPERTY_SPAM_TRAP_BAN_MINUTES);
   }

   void
   Configuration::SetSpamTrapBanMinutes(int minutes)
   {
      GetSettings()->SetLong(PROPERTY_SPAM_TRAP_BAN_MINUTES, minutes);
   }

   int
   Configuration::GetSpamTrapBlockDays() const
   {
      return (int) GetSettings()->GetLong(PROPERTY_SPAM_TRAP_BLOCK_DAYS);
   }

   void
   Configuration::SetSpamTrapBlockDays(int days)
   {
      GetSettings()->SetLong(PROPERTY_SPAM_TRAP_BLOCK_DAYS, days);
   }

   int
   Configuration::GetSpamTrapHitRetentionDays() const
   {
      return (int) GetSettings()->GetLong(PROPERTY_SPAM_TRAP_HIT_RETENTION_DAYS);
   }

   void
   Configuration::SetSpamTrapHitRetentionDays(int days)
   {
      GetSettings()->SetLong(PROPERTY_SPAM_TRAP_HIT_RETENTION_DAYS, days);
   }

   bool
   Configuration::GetSpamTrapTeachSpamAssassin() const
   {
      return GetSettings()->GetBool(PROPERTY_SPAM_TRAP_TEACH_SPAMASSASSIN);
   }

   void
   Configuration::SetSpamTrapTeachSpamAssassin(bool newVal)
   {
      GetSettings()->SetBool(PROPERTY_SPAM_TRAP_TEACH_SPAMASSASSIN, newVal);
   }

   bool
   Configuration::GetLogSMTPConversations() const
   {
      return (GetSettings()->GetLong(PROPERTY_LOGGING)& Logger::LSSMTP) ? true : false;
   }

   void
   Configuration::SetLogSMTPConversations(bool newVal) 
   {
      int iLogging = GetSettings()->GetLong(PROPERTY_LOGGING);

      if (newVal)
         iLogging = iLogging | Logger::LSSMTP;
      else
         iLogging = iLogging & ~Logger::LSSMTP;

      // Save setting
      GetSettings()->SetLong(PROPERTY_LOGGING, iLogging);
   }

   bool
   Configuration::GetLogPOP3Conversations() const
   {
      return (GetSettings()->GetLong(PROPERTY_LOGGING)& Logger::LSPOP3) ? true : false;
   }

   void
   Configuration::SetLogPOP3Conversations(bool newVal)
   {
      int iLogging = GetSettings()->GetLong(PROPERTY_LOGGING);

      if (newVal)
         iLogging = iLogging | Logger::LSPOP3;
      else
         iLogging = iLogging & ~Logger::LSPOP3;

      // Save setting
      GetSettings()->SetLong(PROPERTY_LOGGING, iLogging);
   }

   bool
   Configuration::GetLogIMAPConversations() const
   {
      return (GetSettings()->GetLong(PROPERTY_LOGGING)& Logger::LSIMAP) ? true : false;
   }

   void
   Configuration::SetLogIMAPConversations(bool newVal)
   {
      int iLogging = GetSettings()->GetLong(PROPERTY_LOGGING);

      if (newVal)
         iLogging = iLogging | Logger::LSIMAP;
      else
         iLogging = iLogging & ~Logger::LSIMAP;

      // Save setting
      GetSettings()->SetLong(PROPERTY_LOGGING, iLogging);
   }

   bool
   Configuration::GetLogTCPIP() const
   {
      return (GetSettings()->GetLong(PROPERTY_LOGGING)& Logger::LSTCPIP) ? true : false;
   }

   void
   Configuration::SetLogTCPIP(bool newVal)
   {
      int iLogging = GetSettings()->GetLong(PROPERTY_LOGGING);

      if (newVal)
         iLogging = iLogging | Logger::LSTCPIP;
      else
         iLogging = iLogging & ~Logger::LSTCPIP;

      // Save setting
      GetSettings()->SetLong(PROPERTY_LOGGING, iLogging);
   }

   bool
   Configuration::GetLogApplication() const
   {
      return (GetSettings()->GetLong(PROPERTY_LOGGING)& Logger::LSApplication) ? true : false;
   }

   void
   Configuration::SetLogApplication(bool newVal)
   {
      int iLogging = GetSettings()->GetLong(PROPERTY_LOGGING);

      if (newVal)
         iLogging = iLogging | Logger::LSApplication;
      else
         iLogging = iLogging &~ Logger::LSApplication;

      // Save setting
      GetSettings()->SetLong(PROPERTY_LOGGING, iLogging);
   }

   bool
   Configuration::GetLogDebug() const
   {
      return (GetSettings()->GetLong(PROPERTY_LOGGING) & Logger::LSDebug) ? true : false;
   }

   void
   Configuration::SetLogDebug(bool newVal)
   {
      int iLogging = GetSettings()->GetLong(PROPERTY_LOGGING);

      if (newVal)
         iLogging = iLogging | Logger::LSDebug;
      else
         iLogging = iLogging &~ Logger::LSDebug;

      // Save setting
      GetSettings()->SetLong(PROPERTY_LOGGING, iLogging);
   }

   bool
   Configuration::GetUseScriptServer() const
   {
      return GetSettings()->GetBool(PROPERTY_USESCRIPTSERVER);
   }

   void
   Configuration::SetUseScriptServer(bool newVal)
   {
      GetSettings()->SetBool(PROPERTY_USESCRIPTSERVER, newVal);
   }

   bool
   Configuration::GetKeepFilesOpen() const
   {
      return (GetSettings()->GetLong(PROPERTY_LOGGING)& Logger::LSKeepFilesOpen) ? true : false;
   }

   void
   Configuration::SetKeepFilesOpen(bool bLogging)
   {
      int iLogging = GetSettings()->GetLong(PROPERTY_LOGGING);

      if (bLogging)
         iLogging = iLogging | Logger::LSKeepFilesOpen;
      else
         iLogging = iLogging & ~Logger::LSKeepFilesOpen;

      GetSettings()->SetLong(PROPERTY_LOGGING, iLogging);
   }

   String
   Configuration::GetScriptLanguage() const
   {
      return GetSettings()->GetString(PROPERTY_SCRIPTLANGUAGE);
   }

   void
   Configuration::SetScriptLanguage(const String &newVal)
   {
      GetSettings()->SetString(PROPERTY_SCRIPTLANGUAGE, newVal);
   }

   String
   Configuration::GetDefaultDomain() const
   {
      return GetSettings()->GetString(PROPERTY_DEFAULTDOMAIN);
   }

   void
   Configuration::SetDefaultDomain(const String &newVal)
   {
      GetSettings()->SetString(PROPERTY_DEFAULTDOMAIN, newVal);
   }

   long
   Configuration::GetLogDevice() const
   {
      return GetSettings()->GetLong(PROPERTY_LOGDEVICE);
   }

   void
   Configuration::SetLogDevice(long newVal)
   {
      GetSettings()->SetLong(PROPERTY_LOGDEVICE, newVal);
   }

   bool
   Configuration::GetSyslogEnabled() const
   {
      return GetSettings()->GetBool(PROPERTY_SYSLOG_ENABLED);
   }

   void
   Configuration::SetSyslogEnabled(bool newVal)
   {
      GetSettings()->SetBool(PROPERTY_SYSLOG_ENABLED, newVal);
   }

   String
   Configuration::GetSyslogHost() const
   {
      return GetSettings()->GetString(PROPERTY_SYSLOG_HOST);
   }

   void
   Configuration::SetSyslogHost(const String &newVal)
   {
      GetSettings()->SetString(PROPERTY_SYSLOG_HOST, newVal);
   }

   long
   Configuration::GetSyslogPort() const
   {
      return GetSettings()->GetLong(PROPERTY_SYSLOG_PORT);
   }

   void
   Configuration::SetSyslogPort(long newVal)
   {
      GetSettings()->SetLong(PROPERTY_SYSLOG_PORT, newVal);
   }

   long
   Configuration::GetSyslogTransport() const
   {
      return GetSettings()->GetLong(PROPERTY_SYSLOG_TRANSPORT);
   }

   void
   Configuration::SetSyslogTransport(long newVal)
   {
      GetSettings()->SetLong(PROPERTY_SYSLOG_TRANSPORT, newVal);
   }

   long
   Configuration::GetSyslogFacility() const
   {
      return GetSettings()->GetLong(PROPERTY_SYSLOG_FACILITY);
   }

   void
   Configuration::SetSyslogFacility(long newVal)
   {
      GetSettings()->SetLong(PROPERTY_SYSLOG_FACILITY, newVal);
   }

   long
   Configuration::GetSyslogMinimumSeverity() const
   {
      return GetSettings()->GetLong(PROPERTY_SYSLOG_SEVERITY);
   }

   void
   Configuration::SetSyslogMinimumSeverity(long newVal)
   {
      GetSettings()->SetLong(PROPERTY_SYSLOG_SEVERITY, newVal);
   }

   long
   Configuration::GetSyslogLogTypes() const
   {
      return GetSettings()->GetLong(PROPERTY_SYSLOG_LOGTYPES);
   }

   void
   Configuration::SetSyslogLogTypes(long newVal)
   {
      GetSettings()->SetLong(PROPERTY_SYSLOG_LOGTYPES, newVal);
   }

   bool
   Configuration::GetSyslogLogType(int mask) const
   {
      return (GetSettings()->GetLong(PROPERTY_SYSLOG_LOGTYPES) & mask) != 0;
   }

   void
   Configuration::SetSyslogLogType(int mask, bool enabled)
   {
      long types = GetSettings()->GetLong(PROPERTY_SYSLOG_LOGTYPES);

      if (enabled)
         types |= mask;
      else
         types &= ~mask;

      GetSettings()->SetLong(PROPERTY_SYSLOG_LOGTYPES, types);
   }

   bool
   Configuration::GetBreachCheckEnabled() const
   {
      return GetSettings()->GetBool(PROPERTY_BREACH_CHECK_ENABLED);
   }

   void
   Configuration::SetBreachCheckEnabled(bool newVal)
   {
      GetSettings()->SetBool(PROPERTY_BREACH_CHECK_ENABLED, newVal);
   }

   String
   Configuration::GetBreachCheckUrl() const
   {
      String url = GetSettings()->GetString(PROPERTY_BREACH_CHECK_URL);
      return url.IsEmpty() ? String(PasswordBreachCheck::DefaultUrl) : url;
   }

   void
   Configuration::SetBreachCheckUrl(const String &newVal)
   {
      GetSettings()->SetString(PROPERTY_BREACH_CHECK_URL, newVal);
   }

   long
   Configuration::GetBreachCheckMinimumCount() const
   {
      long minimum = GetSettings()->GetLong(PROPERTY_BREACH_CHECK_MINIMUM_COUNT);
      return minimum < 1 ? 1 : minimum;
   }

   void
   Configuration::SetBreachCheckMinimumCount(long newVal)
   {
      GetSettings()->SetLong(PROPERTY_BREACH_CHECK_MINIMUM_COUNT, newVal);
   }

   long
   Configuration::GetBreachCheckUnreachableAction() const
   {
      return GetSettings()->GetLong(PROPERTY_BREACH_CHECK_UNREACHABLE_ACTION) == PasswordBreachCheck::UnreachableRefuse
         ? PasswordBreachCheck::UnreachableRefuse : PasswordBreachCheck::UnreachableAllow;
   }

   void
   Configuration::SetBreachCheckUnreachableAction(long newVal)
   {
      GetSettings()->SetLong(PROPERTY_BREACH_CHECK_UNREACHABLE_ACTION, newVal);
   }

   String 
   Configuration::GetBackupDestination() const
   {
      return GetSettings()->GetString(PROPERTY_BACKUPDESTINATION);
   }
   
   void 
   Configuration::SetBackupDestination(const String &sNewPath)
   {
      GetSettings()->SetString(PROPERTY_BACKUPDESTINATION, sNewPath);
   }

   bool
   Configuration::GetBackupOption(Backup::BackupOptions iMode) const
   {
      return (GetSettings()->GetLong(PROPERTY_BACKUPOPTIONS)& iMode) ? true : false;      
   }

   int 
   Configuration::GetBackupOptions() const
   {
      return (GetSettings()->GetLong(PROPERTY_BACKUPOPTIONS));
   }

   void
   Configuration::SetBackupOption(Backup::BackupOptions iMode, bool bSet) 
   {
      int iOption = GetSettings()->GetLong(PROPERTY_BACKUPOPTIONS);

      if (bSet)
         iOption = iOption | iMode;
      else
         iOption = iOption &~ iMode;

      GetSettings()->SetLong(PROPERTY_BACKUPOPTIONS, iOption);

   }

   String
   Configuration::GetBackupEncryptionPassphrase() const
   {
      return GetSettings()->GetString(PROPERTY_BACKUPENCRYPTIONPASSPHRASE);
   }

   bool
   Configuration::GetBackupEncryptionPassphraseSet() const
   {
      return GetSettings()->GetLong(PROPERTY_BACKUPENCRYPTIONPASSPHRASE) != 0;
   }

   String
   Configuration::CheckBackupEncryptionPassphrase(const String &passphrase)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // What this server will store as a backup passphrase, and why the limits are
   // where they are.
   //
   // Twelve characters because the archive is the one copy of the data that leaves
   // the server, is kept for months, and can be attacked offline at whatever rate
   // the attacker's hardware allows; 7-Zip's key derivation makes that slower, not
   // impossible.
   //
   // Printable ASCII because the passphrase has to be typed back into a 7-Zip
   // somewhere else on the worst day this server ever has. The bytes reach the
   // archiver on its standard input and are decoded by whatever code page or
   // locale that archiver is running under - which is not the same on the Windows
   // machine that wrote the archive and the Linux machine somebody restores it on
   // - and a character that decodes differently is a key that differs, on an
   // archive nobody can then open. Within ASCII there is no such difference.
   //
   // No leading or trailing space, because it cannot be seen in the box it was
   // typed into and is dropped by half the places it is ever pasted.
   //---------------------------------------------------------------------------()
   {
      if (passphrase.IsEmpty())
         return String();

      const int minimumLength = 12;
      const int maximumLength = 256;

      if (passphrase.GetLength() < minimumLength)
         return "The backup passphrase must be at least 12 characters long. It is the only thing between the backup archive - every mailbox on this server - and whoever can read the folder it is written to.";

      if (passphrase.GetLength() > maximumLength)
         return "The backup passphrase can be at most 256 characters long.";

      for (int index = 0; index < passphrase.GetLength(); index++)
      {
         wchar_t character = passphrase.SafeGetAt((unsigned int) index);

         if (character < 0x20 || character > 0x7E)
            return "The backup passphrase can use only the printable ASCII characters: the letters A to Z and a to z, the digits, the space and the punctuation marks. A passphrase with other characters in it is read differently by a 7-Zip running under another language setting, which is how an archive ends up opening on the machine that wrote it and nowhere else.";
      }

      if (passphrase.SafeGetAt(0) == L' ' || passphrase.SafeGetAt((unsigned int) (passphrase.GetLength() - 1)) == L' ')
         return "The backup passphrase cannot begin or end with a space: it cannot be seen in the box it was typed into, and it is lost wherever the passphrase is pasted or read from a file.";

      return String();
   }

   bool
   Configuration::SetBackupEncryptionPassphrase(const String &passphrase, String &failureReason)
   {
      failureReason = CheckBackupEncryptionPassphrase(passphrase);
      if (!failureReason.IsEmpty())
         return false;

      if (passphrase.IsEmpty())
      {
         // The flag first: between these two writes the server must read "no
         // passphrase", which is what clearing it asked for, rather than "a
         // passphrase is set and cannot be read", which would fail every backup.
         GetSettings()->SetLong(PROPERTY_BACKUPENCRYPTIONPASSPHRASE, 0);
         GetSettings()->SetString(PROPERTY_BACKUPENCRYPTIONPASSPHRASE, _T(""));
         return true;
      }

      // Asked before it is stored, because a secret store that cannot protect it
      // must not end with a row that says a passphrase is set and a value nothing
      // can read. On Windows this cannot fail (Crypt::ProtectSecret falls back to
      // the legacy scheme rather than lose a secret); on Linux a missing key file
      // does fail it, and says so in the error log.
      if (Crypt::Instance()->ProtectSecret(passphrase).IsEmpty())
      {
         failureReason = "The backup passphrase was not stored: this server could not protect it with the machine's secret store, and the error log says why. Nothing has changed - backups go on as they were.";
         return false;
      }

      // The value first here, for the same reason in the other direction.
      GetSettings()->SetString(PROPERTY_BACKUPENCRYPTIONPASSPHRASE, passphrase);
      GetSettings()->SetLong(PROPERTY_BACKUPENCRYPTIONPASSPHRASE, 1);

      return true;
   }

   void 
   Configuration::SetHostName(String &NewVal)
   {
      GetSettings()->SetString(PROPERTY_HOSTNAME, NewVal);
   }

   String
   Configuration::GetHostName() const
   {
      return GetSettings()->GetString(PROPERTY_HOSTNAME);
   }

   void
   Configuration::SetDisconnectInvalidClients(bool newVal)
   {
      GetSettings()->SetBool(PROPERTY_SMTPDISCONNECTINVALIDCLIENTS, newVal);
   }

   bool 
   Configuration::GetDisconnectInvalidClients() 
   {
      return GetSettings()->GetBool(PROPERTY_SMTPDISCONNECTINVALIDCLIENTS);
   }

   void
   Configuration::SetMaximumIncorrectCommands(int newVal)
   {
      GetSettings()->SetLong(PROPERTY_SMTPMAXINCORRECTCOMMANDS, newVal);
   }

   int
   Configuration::GetMaximumIncorrectCommands() 
   {
      return GetSettings()->GetLong(PROPERTY_SMTPMAXINCORRECTCOMMANDS);
   }


   void 
   Configuration::SetAutoBanLogonEnabled(bool newVal)
   {
      GetSettings()->SetBool(PROPERTY_AUTOBAN_ENABLED, newVal);
   }

   bool
   Configuration::GetAutoBanLogonEnabled() 
   {
      return GetSettings()->GetBool(PROPERTY_AUTOBAN_ENABLED);
   }


   void Configuration::SetMaxInvalidLogonAttempts(int newVal)
   {
      GetSettings()->SetLong(PROPERTY_MAX_INVALID_LOGON_ATTEMPTS, newVal);
   }

   int
   Configuration::GetMaxInvalidLogonAttempts() 
   {
      return GetSettings()->GetLong(PROPERTY_MAX_INVALID_LOGON_ATTEMPTS);
   }

   void
   Configuration::SetMaxLogonAttemptsWithin(int newVal)
   {
      GetSettings()->SetLong(PROPERTY_LOGON_ATTEMPTS_WITHIN, newVal);
   }

   int
   Configuration::GetMaxLogonAttemptsWithin() 
   {
      return GetSettings()->GetLong(PROPERTY_LOGON_ATTEMPTS_WITHIN);
   }

   void
   Configuration::SetAutoBanMinutes(int newVal)
   {
      GetSettings()->SetLong(PROPERTY_AUTOBAN_MINUTES, newVal);
   }

   int
   Configuration::GetAutoBanMinutes() 
   {
      return GetSettings()->GetLong(PROPERTY_AUTOBAN_MINUTES);
   }

   int
   Configuration::GetQuotaGracePercent()
   {
      return GetSettings()->GetLong(PROPERTY_QUOTA_GRACE_PERCENT);
   }

   void
   Configuration::SetQuotaGracePercent(int iNewVal)
   {
      GetSettings()->SetLong(PROPERTY_QUOTA_GRACE_PERCENT, iNewVal);
   }

   int
   Configuration::GetQuotaSendStopPercent()
   {
      return GetSettings()->GetLong(PROPERTY_QUOTA_SEND_STOP_PERCENT);
   }

   void
   Configuration::SetQuotaSendStopPercent(int iNewVal)
   {
      GetSettings()->SetLong(PROPERTY_QUOTA_SEND_STOP_PERCENT, iNewVal);
   }

   bool 
   Configuration::ClearOldLogonFailures()
   {
      HM::PersistentLogonFailure failure;
      return failure.ClearOldFailures(-1);
   }

   int
   Configuration::GetAsynchronousThreads() const
   {
      return (GetSettings()->GetLong(PROPERTY_MAX_NUMBER_OF_ASYNC_TASKS));
   }

   void
   Configuration::SetAsynchronousThreads(int iNewVal)
   {
      GetSettings()->SetLong(PROPERTY_MAX_NUMBER_OF_ASYNC_TASKS, iNewVal);
   }

   bool 
   Configuration::GetMessageIndexing()
   {
      return property_set_->GetBool(PROPERTY_MESSAGE_INDEXING);
   }

   void 
   Configuration::SetMessageIndexing(bool enable)
   {
      if (enable)
         MessageIndexer::Instance()->Start();
      else
         MessageIndexer::Instance()->Stop();

      property_set_->SetBool(PROPERTY_MESSAGE_INDEXING, enable);
   }

   bool
   Configuration::GetMessageStoreCompression()
   {
      return GetSettings()->GetBool(PROPERTY_MESSAGE_STORE_COMPRESSION);
   }

   void
   Configuration::SetMessageStoreCompression(bool enabled)
   {
      GetSettings()->SetBool(PROPERTY_MESSAGE_STORE_COMPRESSION, enabled);
   }

   int
   Configuration::GetMessageStoreDedupMinKB()
   {
      // Clamped here as the other getters clamp, so a row edited by hand cannot
      // ask for a negative size or a part larger than a container holds.
      const long kilobytes = GetSettings()->GetLong(PROPERTY_MESSAGE_STORE_DEDUP_MIN_KB);
      return kilobytes < 0 ? 0 : (kilobytes > 65536 ? 65536 : (int) kilobytes);
   }

   void
   Configuration::SetMessageStoreDedupMinKB(int kilobytes)
   {
      GetSettings()->SetLong(PROPERTY_MESSAGE_STORE_DEDUP_MIN_KB, kilobytes < 0 ? 0 : (kilobytes > 65536 ? 65536 : kilobytes));
   }

   bool
   Configuration::GetVerifyRemoteSslCertificate() const
   {
      return GetSettings()->GetBool(PROPERTY_VERIFYREMOTESSLCERTIFICATE);
   }

   void
   Configuration::SetVerifyRemoteSslCertificate(bool UseSMTP)
   {
      GetSettings()->SetBool(PROPERTY_VERIFYREMOTESSLCERTIFICATE, UseSMTP);
   }

   String
   Configuration::GetSslCipherList() const
   {
      return GetSettings()->GetString(PROPERTY_SSLCIPHERLIST);
   }

   void
   Configuration::SetSslCipherList(String newValue)
   {
      GetSettings()->SetString(PROPERTY_SSLCIPHERLIST, newValue);
   }

   bool
   Configuration::GetSslVersionEnabled(SslTlsVersion version) const
   {
      return (GetSettings()->GetLong(PROPERTY_SSLVERSIONS) & version) ? true : false;
   }

   
   void
   Configuration::SetSslVersionEnabled(SslTlsVersion version, bool enabled)
   {
      int versions = GetSettings()->GetLong(PROPERTY_SSLVERSIONS);

      if (enabled)
         versions = versions | version;
      else
         versions = versions &~version;

      GetSettings()->SetLong(PROPERTY_SSLVERSIONS, versions);
   }

   bool
   Configuration::GetTlsOptionEnabled(TlsOption option) const
   {
      return (GetSettings()->GetLong(PROPERTY_TLSOPTIONS) & option) ? true : false;
   }

   void
   Configuration::SetTlsOptionEnabled(TlsOption option, bool enabled)
   {
      int options = GetSettings()->GetLong(PROPERTY_TLSOPTIONS);

      if (enabled)
         options = options | option;
      else
         options = options &~option;

      GetSettings()->SetLong(PROPERTY_TLSOPTIONS, options);
   }

   void
   Configuration::SetIPv6Preferred(bool newVal)
   {
      GetSettings()->SetBool(PROPERTY_IPV6_PREFERRED, newVal);
   }

   bool
   Configuration::GetIPv6Preferred()
   {
      return GetSettings()->GetBool(PROPERTY_IPV6_PREFERRED);
   }

   void
   Configuration::SetCreateDefaultSpecialUseFolders(bool newVal)
   {
      GetSettings()->SetBool(PROPERTY_CREATE_DEFAULT_SPECIAL_USE_FOLDERS, newVal);
   }

   bool
   Configuration::GetCreateDefaultSpecialUseFolders()
   {
      return GetSettings()->GetBool(PROPERTY_CREATE_DEFAULT_SPECIAL_USE_FOLDERS);
   }

   bool
   Configuration::GetAuditTrailEnabled()
   {
      return GetSettings()->GetBool(PROPERTY_AUDIT_TRAIL_ENABLED);
   }

   void
   Configuration::SetAuditTrailEnabled(bool enabled)
   {
      GetSettings()->SetBool(PROPERTY_AUDIT_TRAIL_ENABLED, enabled);
   }

   int
   Configuration::GetAuditRetentionDays()
   {
      return GetSettings()->GetLong(PROPERTY_AUDIT_RETENTION_DAYS);
   }

   void
   Configuration::SetAuditRetentionDays(int days)
   {
      GetSettings()->SetLong(PROPERTY_AUDIT_RETENTION_DAYS, days < 0 ? 0 : days);
   }

   bool
   Configuration::GetAlertsEnabled()
   {
      return GetSettings()->GetBool(PROPERTY_ALERTS_ENABLED);
   }

   void
   Configuration::SetAlertsEnabled(bool enabled)
   {
      GetSettings()->SetBool(PROPERTY_ALERTS_ENABLED, enabled);
   }

   String
   Configuration::GetAlertRecipient()
   {
      return GetSettings()->GetString(PROPERTY_ALERT_RECIPIENT);
   }

   void
   Configuration::SetAlertRecipient(const String &address)
   {
      GetSettings()->SetString(PROPERTY_ALERT_RECIPIENT, address);
   }

   String
   Configuration::GetAlertSenderAddress()
   {
      return GetSettings()->GetString(PROPERTY_ALERT_SENDER_ADDRESS);
   }

   void
   Configuration::SetAlertSenderAddress(const String &address)
   {
      GetSettings()->SetString(PROPERTY_ALERT_SENDER_ADDRESS, address);
   }

   bool
   Configuration::GetAlertDigestEnabled()
   {
      return GetSettings()->GetBool(PROPERTY_ALERT_DIGEST_ENABLED);
   }

   void
   Configuration::SetAlertDigestEnabled(bool enabled)
   {
      GetSettings()->SetBool(PROPERTY_ALERT_DIGEST_ENABLED, enabled);
   }

   int
   Configuration::GetAlertDigestHour()
   {
      int hour = GetSettings()->GetLong(PROPERTY_ALERT_DIGEST_HOUR);

      // An hour outside the day would mean a digest that never goes out, which
      // looks exactly like a digest with nothing in it.
      if (hour < 0 || hour > 23)
         return 7;

      return hour;
   }

   void
   Configuration::SetAlertDigestHour(int hour)
   {
      GetSettings()->SetLong(PROPERTY_ALERT_DIGEST_HOUR, (hour < 0 || hour > 23) ? 7 : hour);
   }

   int
   Configuration::GetAlertMaxPerHour()
   {
      int maximum = GetSettings()->GetLong(PROPERTY_ALERT_MAX_PER_HOUR);

      // 0 is the value a database that never got the row answers with, and it
      // would mean "send nothing" - which is a feature switched off by an
      // upgrade rather than by a person.
      if (maximum <= 0)
         return 20;

      return maximum;
   }

   void
   Configuration::SetAlertMaxPerHour(int maximum)
   {
      GetSettings()->SetLong(PROPERTY_ALERT_MAX_PER_HOUR, maximum <= 0 ? 20 : maximum);
   }

   int
   Configuration::GetAlertWebhookMaxAttempts()
   {
      int attempts = GetSettings()->GetLong(PROPERTY_ALERT_WEBHOOK_MAX_ATTEMPTS);

      if (attempts <= 0)
         return 5;
      if (attempts > 20)
         return 20;

      return attempts;
   }

   void
   Configuration::SetAlertWebhookMaxAttempts(int attempts)
   {
      if (attempts <= 0)
         attempts = 5;
      if (attempts > 20)
         attempts = 20;

      GetSettings()->SetLong(PROPERTY_ALERT_WEBHOOK_MAX_ATTEMPTS, attempts);
   }

   bool
   Configuration::GetWebhooksEnabled()
   {
      return GetSettings()->GetBool(PROPERTY_WEBHOOKS_ENABLED);
   }

   void
   Configuration::SetWebhooksEnabled(bool enabled)
   {
      GetSettings()->SetBool(PROPERTY_WEBHOOKS_ENABLED, enabled);
   }

   int
   Configuration::GetWebhookMaxAttempts()
   {
      int attempts = GetSettings()->GetLong(PROPERTY_WEBHOOK_MAX_ATTEMPTS);

      // 0 is what a database that never got the row answers with, and it would
      // mean every delivery is dead-lettered on its first failure.
      if (attempts <= 0)
         return 5;
      if (attempts > 20)
         return 20;

      return attempts;
   }

   void
   Configuration::SetWebhookMaxAttempts(int attempts)
   {
      if (attempts <= 0)
         attempts = 5;
      if (attempts > 20)
         attempts = 20;

      GetSettings()->SetLong(PROPERTY_WEBHOOK_MAX_ATTEMPTS, attempts);
   }

   int
   Configuration::GetWebhookMaxPerMinute()
   {
      int maximum = GetSettings()->GetLong(PROPERTY_WEBHOOK_MAX_PER_MINUTE);

      // 0 would be "raise nothing", which is a feature switched off by an
      // upgrade rather than by a person.
      if (maximum <= 0)
         return 60;
      if (maximum > 100000)
         return 100000;

      return maximum;
   }

   void
   Configuration::SetWebhookMaxPerMinute(int maximum)
   {
      if (maximum <= 0)
         maximum = 60;
      if (maximum > 100000)
         maximum = 100000;

      GetSettings()->SetLong(PROPERTY_WEBHOOK_MAX_PER_MINUTE, maximum);
   }

   int
   Configuration::GetWebhookRetentionDays()
   {
      int days = GetSettings()->GetLong(PROPERTY_WEBHOOK_RETENTION_DAYS);

      // 0 means keep for ever, which for a per-message stream is a decision and
      // not a default; negative is a hand-edited row and reads as the default.
      if (days < 0)
         return 7;
      if (days > 3650)
         return 3650;

      return days;
   }

   void
   Configuration::SetWebhookRetentionDays(int days)
   {
      if (days < 0)
         days = 7;
      if (days > 3650)
         days = 3650;

      GetSettings()->SetLong(PROPERTY_WEBHOOK_RETENTION_DAYS, days);
   }

   int
   Configuration::GetMigrationConnectionsPerHost()
   {
      int connections = GetSettings()->GetLong(PROPERTY_MIGRATION_CONNECTIONS_PER_HOST);

      // 0 would start no job at all, which is a migration switched off by a
      // hand-edited row rather than by a person pausing it.
      if (connections <= 0)
         return 4;
      if (connections > 64)
         return 64;

      return connections;
   }

   void
   Configuration::SetMigrationConnectionsPerHost(int connections)
   {
      if (connections <= 0)
         connections = 4;
      if (connections > 64)
         connections = 64;

      GetSettings()->SetLong(PROPERTY_MIGRATION_CONNECTIONS_PER_HOST, connections);
   }

   String
   Configuration::GetPstImportDirectory() const
   {
      return GetSettings()->GetString(PROPERTY_PST_IMPORT_DIRECTORY);
   }

   void
   Configuration::SetPstImportDirectory(const String &directory)
   {
      String value = directory;
      value.Trim();
      GetSettings()->SetString(PROPERTY_PST_IMPORT_DIRECTORY, value);
   }

   namespace
   {
      int ClampSetting(int value, int minimum, int maximum)
      {
         if (value < minimum)
            return minimum;
         if (value > maximum)
            return maximum;
         return value;
      }
   }

   bool
   Configuration::GetBounceProcessingEnabled()
   {
      return GetSettings()->GetBool(PROPERTY_BOUNCE_PROCESSING_ENABLED);
   }

   void
   Configuration::SetBounceProcessingEnabled(bool enabled)
   {
      GetSettings()->SetBool(PROPERTY_BOUNCE_PROCESSING_ENABLED, enabled);
   }

   int
   Configuration::GetBounceHardBounceThreshold()
   {
      return ClampSetting(GetSettings()->GetLong(PROPERTY_BOUNCE_HARD_THRESHOLD), 1, 100);
   }

   void
   Configuration::SetBounceHardBounceThreshold(int threshold)
   {
      GetSettings()->SetLong(PROPERTY_BOUNCE_HARD_THRESHOLD, ClampSetting(threshold, 1, 100));
   }

   bool
   Configuration::GetBounceMailboxFullIsHard()
   {
      return GetSettings()->GetBool(PROPERTY_BOUNCE_MAILBOX_FULL_IS_HARD);
   }

   void
   Configuration::SetBounceMailboxFullIsHard(bool enabled)
   {
      GetSettings()->SetBool(PROPERTY_BOUNCE_MAILBOX_FULL_IS_HARD, enabled);
   }

   int
   Configuration::GetBounceSuppressionAction()
   {
      return ClampSetting(GetSettings()->GetLong(PROPERTY_BOUNCE_SUPPRESSION_ACTION), 0, 2);
   }

   void
   Configuration::SetBounceSuppressionAction(int action)
   {
      GetSettings()->SetLong(PROPERTY_BOUNCE_SUPPRESSION_ACTION, ClampSetting(action, 0, 2));
   }

   int
   Configuration::GetBounceSuppressionExpiryDays()
   {
      return ClampSetting(GetSettings()->GetLong(PROPERTY_BOUNCE_SUPPRESSION_EXPIRY_DAYS), 0, 3650);
   }

   void
   Configuration::SetBounceSuppressionExpiryDays(int days)
   {
      GetSettings()->SetLong(PROPERTY_BOUNCE_SUPPRESSION_EXPIRY_DAYS, ClampSetting(days, 0, 3650));
   }

   bool
   Configuration::GetBounceReadDeliveryReports()
   {
      return GetSettings()->GetBool(PROPERTY_BOUNCE_READ_REPORTS);
   }

   void
   Configuration::SetBounceReadDeliveryReports(bool enabled)
   {
      GetSettings()->SetBool(PROPERTY_BOUNCE_READ_REPORTS, enabled);
   }

   int
   Configuration::GetBounceCountRetentionDays()
   {
      return ClampSetting(GetSettings()->GetLong(PROPERTY_BOUNCE_COUNT_RETENTION_DAYS), 0, 3650);
   }

   void
   Configuration::SetBounceCountRetentionDays(int days)
   {
      GetSettings()->SetLong(PROPERTY_BOUNCE_COUNT_RETENTION_DAYS, ClampSetting(days, 0, 3650));
   }

   int
   Configuration::GetBounceReportWindowDays()
   {
      return ClampSetting(GetSettings()->GetLong(PROPERTY_BOUNCE_REPORT_WINDOW_DAYS), 1, 30);
   }

   void
   Configuration::SetBounceReportWindowDays(int days)
   {
      GetSettings()->SetLong(PROPERTY_BOUNCE_REPORT_WINDOW_DAYS, ClampSetting(days, 1, 30));
   }

   bool
   Configuration::GetOpenPGPWebKeyDirectoryEnabled()
   {
      return GetSettings()->GetLong(PROPERTY_OPENPGP_WKD_ENABLED) == 1;
   }

   void
   Configuration::SetOpenPGPWebKeyDirectoryEnabled(bool enabled)
   {
      GetSettings()->SetLong(PROPERTY_OPENPGP_WKD_ENABLED, enabled ? 1 : 0);
   }

   bool
   Configuration::GetZeroAccessAllowed()
   {
      return GetSettings()->GetLong(PROPERTY_ZERO_ACCESS_ALLOWED) == 1;
   }

   void
   Configuration::SetZeroAccessAllowed(bool allowed)
   {
      GetSettings()->SetLong(PROPERTY_ZERO_ACCESS_ALLOWED, allowed ? 1 : 0);
   }

   int
   Configuration::GetMailboxKeyIdleMinutes()
   {
      return ClampSetting(GetSettings()->GetLong(PROPERTY_MAILBOX_KEY_IDLE_MINUTES), 1, 1440);
   }

   void
   Configuration::SetMailboxKeyIdleMinutes(int minutes)
   {
      GetSettings()->SetLong(PROPERTY_MAILBOX_KEY_IDLE_MINUTES, minutes < 1 ? 1 : (minutes > 1440 ? 1440 : minutes));
   }

   int
   Configuration::GetOpenPGPKeyMaximumBytes()
   {
      return ClampSetting(GetSettings()->GetLong(PROPERTY_OPENPGP_KEY_MAXIMUM_BYTES), 1024, 1048576);
   }

   void
   Configuration::SetOpenPGPKeyMaximumBytes(int bytes)
   {
      GetSettings()->SetLong(PROPERTY_OPENPGP_KEY_MAXIMUM_BYTES, ClampSetting(bytes, 1024, 1048576));
   }

   int
   Configuration::GetOpenPGPKeysPerAccount()
   {
      return ClampSetting(GetSettings()->GetLong(PROPERTY_OPENPGP_KEYS_PER_ACCOUNT), 1, 64);
   }

   void
   Configuration::SetOpenPGPKeysPerAccount(int keys)
   {
      GetSettings()->SetLong(PROPERTY_OPENPGP_KEYS_PER_ACCOUNT, ClampSetting(keys, 1, 64));
   }

   bool
   Configuration::GetOpenPGPRequireAddressInUserId()
   {
      return GetSettings()->GetLong(PROPERTY_OPENPGP_REQUIRE_ADDRESS) == 1;
   }

   void
   Configuration::SetOpenPGPRequireAddressInUserId(bool required)
   {
      GetSettings()->SetLong(PROPERTY_OPENPGP_REQUIRE_ADDRESS, required ? 1 : 0);
   }

   bool
   Configuration::GetOpenPGPPortalEnabled()
   {
      return GetSettings()->GetLong(PROPERTY_OPENPGP_PORTAL_ENABLED) == 1;
   }

   void
   Configuration::SetOpenPGPPortalEnabled(bool enabled)
   {
      GetSettings()->SetLong(PROPERTY_OPENPGP_PORTAL_ENABLED, enabled ? 1 : 0);
   }

   bool
   Configuration::GetOpenPGPLookupWkd()
   {
      return GetSettings()->GetLong(PROPERTY_OPENPGP_LOOKUP_WKD) == 1;
   }

   void
   Configuration::SetOpenPGPLookupWkd(bool enabled)
   {
      GetSettings()->SetLong(PROPERTY_OPENPGP_LOOKUP_WKD, enabled ? 1 : 0);
   }

   bool
   Configuration::GetOpenPGPLookupKeyserver()
   {
      return GetSettings()->GetLong(PROPERTY_OPENPGP_LOOKUP_KEYSERVER) == 1;
   }

   void
   Configuration::SetOpenPGPLookupKeyserver(bool enabled)
   {
      GetSettings()->SetLong(PROPERTY_OPENPGP_LOOKUP_KEYSERVER, enabled ? 1 : 0);
   }

   String
   Configuration::GetOpenPGPKeyserverUrl()
   {
      return GetSettings()->GetString(PROPERTY_OPENPGP_KEYSERVER_URL);
   }

   void
   Configuration::SetOpenPGPKeyserverUrl(const String &url)
   {
      GetSettings()->SetString(PROPERTY_OPENPGP_KEYSERVER_URL, url);
   }

   int
   Configuration::GetOpenPGPLookupTimeout()
   {
      return ClampSetting(GetSettings()->GetLong(PROPERTY_OPENPGP_LOOKUP_TIMEOUT), 2, 30);
   }

   void
   Configuration::SetOpenPGPLookupTimeout(int seconds)
   {
      GetSettings()->SetLong(PROPERTY_OPENPGP_LOOKUP_TIMEOUT, ClampSetting(seconds, 2, 30));
   }

   int
   Configuration::GetProtectedMessageDays()
   {
      return ClampSetting(GetSettings()->GetLong(PROPERTY_PROTECTED_MESSAGE_DAYS), 1, 365);
   }

   void
   Configuration::SetProtectedMessageDays(int days)
   {
      GetSettings()->SetLong(PROPERTY_PROTECTED_MESSAGE_DAYS, ClampSetting(days, 1, 365));
   }

   int
   Configuration::GetRecallDays()
   {
      return ClampSetting(GetSettings()->GetLong(PROPERTY_RECALL_DAYS), 1, 365);
   }

   void
   Configuration::SetRecallDays(int days)
   {
      GetSettings()->SetLong(PROPERTY_RECALL_DAYS, ClampSetting(days, 1, 365));
   }

   String
   Configuration::GetListBaseUrl() const
   {
      return GetSettings()->GetString(PROPERTY_LIST_BASE_URL);
   }

   void
   Configuration::SetListBaseUrl(const String &url)
   {
      GetSettings()->SetString(PROPERTY_LIST_BASE_URL, url);
   }

   String
   Configuration::GetSmimeAuthorityBaseUrl() const
   {
      return GetSettings()->GetString(PROPERTY_SMIME_AUTHORITY_BASE_URL);
   }

   void
   Configuration::SetSmimeAuthorityBaseUrl(const String &url)
   {
      GetSettings()->SetString(PROPERTY_SMIME_AUTHORITY_BASE_URL, url);
   }

   int
   Configuration::GetListHardBouncesToUnsubscribe()
   {
      return ClampSetting(GetSettings()->GetLong(PROPERTY_LIST_HARD_BOUNCES), 1, 100);
   }

   void
   Configuration::SetListHardBouncesToUnsubscribe(int count)
   {
      GetSettings()->SetLong(PROPERTY_LIST_HARD_BOUNCES, ClampSetting(count, 1, 100));
   }

   int
   Configuration::GetListSoftBouncesToUnsubscribe()
   {
      return ClampSetting(GetSettings()->GetLong(PROPERTY_LIST_SOFT_BOUNCES), 1, 100);
   }

   void
   Configuration::SetListSoftBouncesToUnsubscribe(int count)
   {
      GetSettings()->SetLong(PROPERTY_LIST_SOFT_BOUNCES, ClampSetting(count, 1, 100));
   }

   int
   Configuration::GetListBounceWindowDays()
   {
      return ClampSetting(GetSettings()->GetLong(PROPERTY_LIST_BOUNCE_WINDOW_DAYS), 1, 365);
   }

   void
   Configuration::SetListBounceWindowDays(int days)
   {
      GetSettings()->SetLong(PROPERTY_LIST_BOUNCE_WINDOW_DAYS, ClampSetting(days, 1, 365));
   }

   int
   Configuration::GetOutboundBackoffMinutes()
   {
      return ClampSetting(GetSettings()->GetLong(PROPERTY_OUTBOUND_BACKOFF_MINUTES), 0, 10080);
   }

   void
   Configuration::SetOutboundBackoffMinutes(int minutes)
   {
      GetSettings()->SetLong(PROPERTY_OUTBOUND_BACKOFF_MINUTES, ClampSetting(minutes, 0, 10080));
   }

   int
   Configuration::GetOutboundBackoffMaxMinutes()
   {
      // The stored value, within its range. The setters on every surface keep
      // it at or above the base and the base at or below it; the back-off
      // itself reads a ceiling under the base as the base, so a hand-edited
      // row cannot make the doubling shrink a window, and what is answered
      // here is always what the row holds.
      return ClampSetting(GetSettings()->GetLong(PROPERTY_OUTBOUND_BACKOFF_MAX_MINUTES), 1, 10080);
   }

   void
   Configuration::SetOutboundBackoffMaxMinutes(int minutes)
   {
      GetSettings()->SetLong(PROPERTY_OUTBOUND_BACKOFF_MAX_MINUTES, ClampSetting(minutes, 1, 10080));
   }

   int
   Configuration::GetMaxOutboundConnectionsPerDestination()
   {
      return ClampSetting(GetSettings()->GetLong(PROPERTY_MAX_OUTBOUND_CONNS_PER_DEST), 0, 1000);
   }

   void
   Configuration::SetMaxOutboundConnectionsPerDestination(int connections)
   {
      GetSettings()->SetLong(PROPERTY_MAX_OUTBOUND_CONNS_PER_DEST, ClampSetting(connections, 0, 1000));
   }

   int
   Configuration::GetMaxOutboundConnectionsPerTenant()
   {
      return ClampSetting(GetSettings()->GetLong(PROPERTY_MAX_OUTBOUND_CONNS_PER_TENANT), 0, 1000);
   }

   void
   Configuration::SetMaxOutboundConnectionsPerTenant(int connections)
   {
      GetSettings()->SetLong(PROPERTY_MAX_OUTBOUND_CONNS_PER_TENANT, ClampSetting(connections, 0, 1000));
   }

   bool
   Configuration::GetSandboxEnabled()
   {
      return GetSettings()->GetBool(PROPERTY_SANDBOX_ENABLED);
   }

   void
   Configuration::SetSandboxEnabled(bool enabled)
   {
      GetSettings()->SetBool(PROPERTY_SANDBOX_ENABLED, enabled);
   }

   String
   Configuration::GetSandboxSubmitUrl()
   {
      return GetSettings()->GetString(PROPERTY_SANDBOX_SUBMIT_URL);
   }

   void
   Configuration::SetSandboxSubmitUrl(const String &url)
   {
      // Trimmed here, in the one place every interface reaches, so that REST,
      // COM and the Control Panel store the same text for the same address.
      String trimmed = url;
      trimmed.Trim();
      GetSettings()->SetString(PROPERTY_SANDBOX_SUBMIT_URL, trimmed);
   }

   String
   Configuration::GetSandboxVerdictUrl()
   {
      return GetSettings()->GetString(PROPERTY_SANDBOX_VERDICT_URL);
   }

   void
   Configuration::SetSandboxVerdictUrl(const String &url)
   {
      String trimmed = url;
      trimmed.Trim();
      GetSettings()->SetString(PROPERTY_SANDBOX_VERDICT_URL, trimmed);
   }

   String
   Configuration::GetSandboxToken()
   {
      return GetSettings()->GetString(PROPERTY_SANDBOX_TOKEN);
   }

   void
   Configuration::SetSandboxToken(const String &token)
   {
      GetSettings()->SetString(PROPERTY_SANDBOX_TOKEN, token);
   }

   int
   Configuration::GetSandboxTimeoutMinutes()
   {
      return ClampSetting(GetSettings()->GetLong(PROPERTY_SANDBOX_TIMEOUT_MINUTES), 1, 1440);
   }

   void
   Configuration::SetSandboxTimeoutMinutes(int minutes)
   {
      GetSettings()->SetLong(PROPERTY_SANDBOX_TIMEOUT_MINUTES, ClampSetting(minutes, 1, 1440));
   }

   int
   Configuration::GetSandboxTimeoutAction()
   {
      return ClampSetting(GetSettings()->GetLong(PROPERTY_SANDBOX_TIMEOUT_ACTION), 0, 1);
   }

   void
   Configuration::SetSandboxTimeoutAction(int action)
   {
      GetSettings()->SetLong(PROPERTY_SANDBOX_TIMEOUT_ACTION, ClampSetting(action, 0, 1));
   }

   int
   Configuration::GetSandboxMaxSizeKB()
   {
      return ClampSetting(GetSettings()->GetLong(PROPERTY_SANDBOX_MAX_SIZE_KB), 1, 81920);
   }

   void
   Configuration::SetSandboxMaxSizeKB(int kilobytes)
   {
      GetSettings()->SetLong(PROPERTY_SANDBOX_MAX_SIZE_KB, ClampSetting(kilobytes, 1, 81920));
   }

   String
   Configuration::GetSandboxTypes()
   {
      return GetSettings()->GetString(PROPERTY_SANDBOX_TYPES);
   }

   void
   Configuration::SetSandboxTypes(const String &types)
   {
      GetSettings()->SetString(PROPERTY_SANDBOX_TYPES, types);
   }

   int
   Configuration::GetSandboxPollSeconds()
   {
      return ClampSetting(GetSettings()->GetLong(PROPERTY_SANDBOX_POLL_SECONDS), 5, 3600);
   }

   void
   Configuration::SetSandboxPollSeconds(int seconds)
   {
      GetSettings()->SetLong(PROPERTY_SANDBOX_POLL_SECONDS, ClampSetting(seconds, 5, 3600));
   }

   int
   Configuration::GetSandboxScoreThreshold()
   {
      return ClampSetting(GetSettings()->GetLong(PROPERTY_SANDBOX_SCORE_THRESHOLD), 0, 100);
   }

   void
   Configuration::SetSandboxScoreThreshold(int threshold)
   {
      GetSettings()->SetLong(PROPERTY_SANDBOX_SCORE_THRESHOLD, ClampSetting(threshold, 0, 100));
   }

   int
   Configuration::GetSandboxCacheHours()
   {
      return ClampSetting(GetSettings()->GetLong(PROPERTY_SANDBOX_CACHE_HOURS), 0, 720);
   }

   void
   Configuration::SetSandboxCacheHours(int hours)
   {
      GetSettings()->SetLong(PROPERTY_SANDBOX_CACHE_HOURS, ClampSetting(hours, 0, 720));
   }

   bool
   Configuration::GetSandboxInternalMail()
   {
      return GetSettings()->GetBool(PROPERTY_SANDBOX_INTERNAL_MAIL);
   }

   void
   Configuration::SetSandboxInternalMail(bool enabled)
   {
      GetSettings()->SetBool(PROPERTY_SANDBOX_INTERNAL_MAIL, enabled);
   }

   int
   Configuration::GetSandboxMaxFiles()
   {
      return ClampSetting(GetSettings()->GetLong(PROPERTY_SANDBOX_MAX_FILES), 1, 100);
   }

   void
   Configuration::SetSandboxMaxFiles(int files)
   {
      GetSettings()->SetLong(PROPERTY_SANDBOX_MAX_FILES, ClampSetting(files, 1, 100));
   }

   bool
   Configuration::GetSnmpEnabled()
   {
      return GetSettings()->GetBool(PROPERTY_SNMP_ENABLED);
   }

   void
   Configuration::SetSnmpEnabled(bool enabled)
   {
      GetSettings()->SetBool(PROPERTY_SNMP_ENABLED, enabled);
   }

   int
   Configuration::GetSnmpPort()
   {
      return ClampSetting(GetSettings()->GetLong(PROPERTY_SNMP_PORT), 1, 65535);
   }

   void
   Configuration::SetSnmpPort(int port)
   {
      GetSettings()->SetLong(PROPERTY_SNMP_PORT, ClampSetting(port, 1, 65535));
   }

   String
   Configuration::GetSnmpBindAddress()
   {
      return GetSettings()->GetString(PROPERTY_SNMP_BIND_ADDRESS);
   }

   void
   Configuration::SetSnmpBindAddress(const String &address)
   {
      String trimmed = address;
      trimmed.Trim();
      GetSettings()->SetString(PROPERTY_SNMP_BIND_ADDRESS, trimmed);
   }

   String
   Configuration::GetSnmpAllowedAddresses()
   {
      return GetSettings()->GetString(PROPERTY_SNMP_ALLOWED_ADDRESSES);
   }

   void
   Configuration::SetSnmpAllowedAddresses(const String &addresses)
   {
      String trimmed = addresses;
      trimmed.Trim();
      GetSettings()->SetString(PROPERTY_SNMP_ALLOWED_ADDRESSES, trimmed);
   }

   String
   Configuration::GetSnmpV3User()
   {
      return GetSettings()->GetString(PROPERTY_SNMP_V3_USER);
   }

   void
   Configuration::SetSnmpV3User(const String &user)
   {
      String trimmed = user;
      trimmed.Trim();
      GetSettings()->SetString(PROPERTY_SNMP_V3_USER, trimmed);
   }

   String
   Configuration::GetSnmpV3AuthPassword()
   {
      return GetSettings()->GetString(PROPERTY_SNMP_V3_AUTH_PASSWORD);
   }

   void
   Configuration::SetSnmpV3AuthPassword(const String &password)
   {
      GetSettings()->SetString(PROPERTY_SNMP_V3_AUTH_PASSWORD, password);
   }

   String
   Configuration::GetSnmpV3PrivPassword()
   {
      return GetSettings()->GetString(PROPERTY_SNMP_V3_PRIV_PASSWORD);
   }

   void
   Configuration::SetSnmpV3PrivPassword(const String &password)
   {
      GetSettings()->SetString(PROPERTY_SNMP_V3_PRIV_PASSWORD, password);
   }

   bool
   Configuration::GetSnmpV2cEnabled()
   {
      return GetSettings()->GetBool(PROPERTY_SNMP_V2C_ENABLED);
   }

   void
   Configuration::SetSnmpV2cEnabled(bool enabled)
   {
      GetSettings()->SetBool(PROPERTY_SNMP_V2C_ENABLED, enabled);
   }

   String
   Configuration::GetSnmpV2cCommunity()
   {
      return GetSettings()->GetString(PROPERTY_SNMP_V2C_COMMUNITY);
   }

   void
   Configuration::SetSnmpV2cCommunity(const String &community)
   {
      GetSettings()->SetString(PROPERTY_SNMP_V2C_COMMUNITY, community);
   }

   String
   Configuration::GetSnmpContact()
   {
      return GetSettings()->GetString(PROPERTY_SNMP_CONTACT);
   }

   void
   Configuration::SetSnmpContact(const String &contact)
   {
      GetSettings()->SetString(PROPERTY_SNMP_CONTACT, contact);
   }

   String
   Configuration::GetSnmpLocation()
   {
      return GetSettings()->GetString(PROPERTY_SNMP_LOCATION);
   }

   void
   Configuration::SetSnmpLocation(const String &location)
   {
      GetSettings()->SetString(PROPERTY_SNMP_LOCATION, location);
   }

   bool
   Configuration::GetWebPushEnabled()
   {
      return GetSettings()->GetBool(PROPERTY_WEB_PUSH_ENABLED);
   }

   void
   Configuration::SetWebPushEnabled(bool enabled)
   {
      GetSettings()->SetBool(PROPERTY_WEB_PUSH_ENABLED, enabled);
   }

   bool
   Configuration::GetWebPushLoopbackHttpForTests()
   {
      return GetSettings()->GetBool(PROPERTY_WEB_PUSH_LOOPBACK_HTTP);
   }

   void
   Configuration::SetWebPushLoopbackHttpForTests(bool allowed)
   {
      GetSettings()->SetBool(PROPERTY_WEB_PUSH_LOOPBACK_HTTP, allowed);
   }

   String
   Configuration::GetWebPushSubject()
   {
      return GetSettings()->GetString(PROPERTY_WEB_PUSH_SUBJECT);
   }

   void
   Configuration::SetWebPushSubject(const String &subject)
   {
      GetSettings()->SetString(PROPERTY_WEB_PUSH_SUBJECT, subject);
   }

   int
   Configuration::GetWebPushTimeToLiveSeconds()
   {
      int seconds = GetSettings()->GetLong(PROPERTY_WEB_PUSH_TTL_SECONDS);

      // 0 is a real choice - "deliver it now or not at all" - so it is kept, and
      // only a value outside the range a push service would accept is replaced.
      if (seconds < 0 || seconds > 2419200)
         return 86400;

      return seconds;
   }

   void
   Configuration::SetWebPushTimeToLiveSeconds(int seconds)
   {
      GetSettings()->SetLong(PROPERTY_WEB_PUSH_TTL_SECONDS, (seconds < 0 || seconds > 2419200) ? 86400 : seconds);
   }

   int
   Configuration::GetWebPushMaxPerAccount()
   {
      int maximum = GetSettings()->GetLong(PROPERTY_WEB_PUSH_MAX_PER_ACCOUNT);

      if (maximum <= 0)
         return 10;
      if (maximum > 50)
         return 50;

      return maximum;
   }

   void
   Configuration::SetWebPushMaxPerAccount(int maximum)
   {
      if (maximum <= 0)
         maximum = 10;
      if (maximum > 50)
         maximum = 50;

      GetSettings()->SetLong(PROPERTY_WEB_PUSH_MAX_PER_ACCOUNT, maximum);
   }

   int
   Configuration::GetWebPushMaxAttempts()
   {
      int attempts = GetSettings()->GetLong(PROPERTY_WEB_PUSH_MAX_ATTEMPTS);

      if (attempts <= 0)
         return 5;
      if (attempts > 20)
         return 20;

      return attempts;
   }

   void
   Configuration::SetWebPushMaxAttempts(int attempts)
   {
      if (attempts <= 0)
         attempts = 5;
      if (attempts > 20)
         attempts = 20;

      GetSettings()->SetLong(PROPERTY_WEB_PUSH_MAX_ATTEMPTS, attempts);
   }

   String
   Configuration::GetWebPushVapidPublicKey()
   {
      return GetSettings()->GetString(PROPERTY_WEB_PUSH_VAPID_PUBLIC_KEY);
   }

   String
   Configuration::GetWebPushVapidPrivateKey()
   {
      return GetSettings()->GetString(PROPERTY_WEB_PUSH_VAPID_PRIVATE_KEY);
   }

   /*
      The public half is DERIVED rather than taken, so the pair can never
      disagree: a server given a private key alone - which is what an
      administrator moving an installation has, and what every other Web Push
      implementation asks for - ends up with the public key that belongs to it,
      and a page that subscribes under that key is subscribing to this server.
   */
   bool
   Configuration::SetWebPushVapidKeys(const String &privateKey, String &refusal)
   {
      refusal.Empty();

      String trimmed = privateKey;
      trimmed.Trim();

      if (trimmed.IsEmpty())
      {
         // Clearing both halves is how an administrator turns the feature off for
         // good: every existing subscription stops working, which is the point.
         GetSettings()->SetString(PROPERTY_WEB_PUSH_VAPID_PRIVATE_KEY, _T(""));
         GetSettings()->SetString(PROPERTY_WEB_PUSH_VAPID_PUBLIC_KEY, _T(""));
         return true;
      }

      AnsiString publicKey;
      String error;
      if (!WebPush::PublicKeyOf(AnsiString(trimmed), publicKey, error))
      {
         refusal = error;
         return false;
      }

      GetSettings()->SetString(PROPERTY_WEB_PUSH_VAPID_PRIVATE_KEY, trimmed);
      GetSettings()->SetString(PROPERTY_WEB_PUSH_VAPID_PUBLIC_KEY, String(publicKey));
      return true;
   }

   bool
   Configuration::GenerateWebPushVapidKeys(String &refusal)
   {
      refusal.Empty();

      AnsiString publicKey;
      AnsiString privateKey;
      String error;

      if (!WebPush::GenerateKeyPair(publicKey, privateKey, error))
      {
         refusal = error;
         return false;
      }

      GetSettings()->SetString(PROPERTY_WEB_PUSH_VAPID_PRIVATE_KEY, String(privateKey));
      GetSettings()->SetString(PROPERTY_WEB_PUSH_VAPID_PUBLIC_KEY, String(publicKey));
      return true;
   }

   bool
   Configuration::GetComplaintProcessingEnabled()
   {
      return GetSettings()->GetBool(PROPERTY_COMPLAINT_PROCESSING_ENABLED);
   }

   void
   Configuration::SetComplaintProcessingEnabled(bool enabled)
   {
      GetSettings()->SetBool(PROPERTY_COMPLAINT_PROCESSING_ENABLED, enabled);
   }

   String
   Configuration::GetComplaintMailbox()
   {
      return GetSettings()->GetString(PROPERTY_COMPLAINT_MAILBOX);
   }

   void
   Configuration::SetComplaintMailbox(const String &mailbox)
   {
      String value = mailbox;
      value.Trim();

      GetSettings()->SetString(PROPERTY_COMPLAINT_MAILBOX, value.Left(255));
   }

   int
   Configuration::GetComplaintSuppressionThreshold()
   {
      return ClampSetting(GetSettings()->GetLong(PROPERTY_COMPLAINT_THRESHOLD), 0, 100);
   }

   void
   Configuration::SetComplaintSuppressionThreshold(int threshold)
   {
      GetSettings()->SetLong(PROPERTY_COMPLAINT_THRESHOLD, ClampSetting(threshold, 0, 100));
   }

   int
   Configuration::GetComplaintRetentionDays()
   {
      return ClampSetting(GetSettings()->GetLong(PROPERTY_COMPLAINT_RETENTION_DAYS), 0, 3650);
   }

   void
   Configuration::SetComplaintRetentionDays(int days)
   {
      GetSettings()->SetLong(PROPERTY_COMPLAINT_RETENTION_DAYS, ClampSetting(days, 0, 3650));
   }

   int
   Configuration::GetCrashSimulationMode() const
   {
      return crash_simulation_mode_;
   }


   void
   Configuration::SetCrashSimulationMode(int mode)
   {
      crash_simulation_mode_ = mode;
   }


   bool 
   Configuration::XMLStore(XNode *pBackupNode)
   {
      // Every one of these was called for its side effect and its answer thrown away,
      // under a function that then returned true - so this half of the backup could
      // not fail. XMLLoad below is checked at every single step, which is the shape
      // that gives it away: the restore was hardened and the backup was not, and a
      // backup that cannot report a failure is worse than one that fails loudly. It
      // is the file somebody reaches for after they have already lost something, and
      // the one place a missing section is guaranteed not to be noticed is at the
      // moment it is written.
      //
      // Ordered as before, and each one still attempted only if the previous
      // succeeded: a half-written settings section is not worth continuing past, and
      // BackupExecuter deletes the archive it was building when this returns false.

      // PROPERTIES
      if (!property_set_->XMLStore(pBackupNode))
         return false;

      // hMailServer.INI's [Settings] section, which is in the database from schema
      // 6011 precisely so that it can be here. Before that it was in no backup at
      // all: an operator who restored onto replacement hardware got their domains,
      // accounts and mail back and none of their server settings, and nothing said
      // so.
      IniSettingStore iniSettings;
      if (!iniSettings.XMLStore(pBackupNode))
         return false;

      // SECURITY RANGES
      SecurityRanges securityRanges;
      securityRanges.Refresh();
      if (!securityRanges.XMLStore(pBackupNode, 0))
         return false;

      // RULES
      std::shared_ptr<Rules> pRules = std::shared_ptr<Rules>(new Rules(0));
      pRules->Refresh();
      if (!pRules->XMLStore(pBackupNode, 0))
         return false;

      // TCP/IP ports
      if (!GetTCPIPPorts()->XMLStore(pBackupNode, 0))
         return false;

      if (!ssl_certificates_->XMLStore(pBackupNode, 0))
         return false;

      if (!blocked_attachments_->XMLStore(pBackupNode, 0))
         return false;

      if (!smtp_configuration_->XMLStore(pBackupNode, 0))
         return false;

      if (!imap_configuration_->XMLStore(pBackupNode, 0))
         return false;

      if (!anti_spam_configuration_.XMLStore(pBackupNode, 0))
         return false;

      if (!cache_configuration_->XMLStore(pBackupNode, 0))
         return false;

      return true;
   }

   bool
   Configuration::XMLLoad(XNode *pBackupNode, int iRestoreOptions)
   {
      // PROPERTIES
      if (!property_set_->XMLLoad(pBackupNode))
         return false;

      // [Settings]. Absent from an archive taken before schema 6011, in which case
      // this does nothing - it must not clear the table, because an old archive
      // carries no evidence about settings either way and wiping them would be a
      // silent loss on a restore that appeared to succeed.
      IniSettingStore iniSettings;
      if (!iniSettings.XMLLoad(pBackupNode))
         return false;

      // SECURITY RANGES
      SecurityRanges securityRanges;
      securityRanges.Refresh();
      if (!securityRanges.XMLLoad(pBackupNode, iRestoreOptions))
         return false;

      // RULES
      std::shared_ptr<Rules> pRules = std::shared_ptr<Rules>(new Rules(0));
      pRules->Refresh();
      if (!pRules->XMLLoad(pBackupNode, iRestoreOptions))
         return false;
      ObjectCache::Instance()->GetGlobalRules()->Refresh();

      // SSL certs must be restored before TCP/IP ports, since the
      // TCP/IP ports refers to the SSL certs.
      ssl_certificates_->Refresh();
      if (!ssl_certificates_->XMLLoad(pBackupNode, iRestoreOptions))
         return false;

      // TCP/IP ports
      if (!GetTCPIPPorts()->XMLLoad(pBackupNode, iRestoreOptions))
         return false;

      blocked_attachments_->Refresh();
      if (!blocked_attachments_->XMLLoad(pBackupNode, iRestoreOptions))
         return false;

      if (!smtp_configuration_->XMLLoad(pBackupNode, iRestoreOptions))
         return false;

      if (!imap_configuration_->XMLLoad(pBackupNode, iRestoreOptions))
         return false;

      if (!anti_spam_configuration_.XMLLoad(pBackupNode, iRestoreOptions))
         return false;

      if (!cache_configuration_->XMLLoad(pBackupNode, iRestoreOptions))
         return false;

      return true;
   }

   bool
   Configuration::IsIPv6Available()
   {
      return ipv6_available_;
   }
} 