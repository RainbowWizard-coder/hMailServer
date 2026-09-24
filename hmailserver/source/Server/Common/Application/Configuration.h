// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Backup.h"

#include "../AntiSpam/AntiSpamConfiguration.h"
#include "../AntiVirus/AntiVirusConfiguration.h"
#include "../TCPIP/SocketConstants.h"

namespace HM
{

   class SMTPConfiguration;
   class POP3Configuration;
   class IMAPConfiguration;
   class AntiSpamConfiguration;
   class Logger;
   class PropertySet;
   class ServerMessages;
   class TCPIPPorts;
   class CacheConfiguration;
   class Settings;
   class ServerMessages;
   class TCPIPPorts;
   class BlockedAttachments;
   class SSLCertificates;

   class Configuration : public Singleton<Configuration>
   {
   public:

      Configuration();
      virtual ~Configuration();

      std::shared_ptr<POP3Configuration> GetPOP3Configuration() const { return pop3_configuration_; }
      std::shared_ptr<SMTPConfiguration> GetSMTPConfiguration() const { return smtp_configuration_; }
      std::shared_ptr<IMAPConfiguration> GetIMAPConfiguration() const { return imap_configuration_; }
      AntiSpamConfiguration &GetAntiSpamConfiguration() { return anti_spam_configuration_; }
      AntiVirusConfiguration &GetAntiVirusConfiguration() { return anti_virus_configuration_; }
      std::shared_ptr<CacheConfiguration> GetCacheConfiguration() const { return cache_configuration_; }

      bool Load();

      bool IsIPv6Available();

      void OnPropertyChanged(std::shared_ptr<Property> pProperty);

      bool GetUseSMTP() const;
      void SetUseSMTP(bool UseSMTP);
      bool GetUsePOP3() const;
      void SetUsePOP3(bool UsePOP3);
      bool GetUseIMAP() const;
      void SetUseIMAP(bool UseIMAP);

      bool GetAWStatsEnabled() const;
      void SetAWStatsEnabled(bool newVal);

      long GetLogFormat() const;
      void SetLogFormat(long lNewVal);

      bool GetUseLogging() const;
      void SetUseLogging(bool bLogging);

      void SetLogSMTPConversations(bool newVal);
      bool GetLogSMTPConversations() const;

      void SetLogPOP3Conversations(bool newVal);
      bool GetLogPOP3Conversations() const;

      void SetLogIMAPConversations(bool newVal);
      bool GetLogIMAPConversations() const;

      void SetLogTCPIP(bool newVal);
      bool GetLogTCPIP() const;

      void SetLogApplication(bool newVal);
      bool GetLogApplication() const;

      void SetLogDebug(bool newVal);
      bool GetLogDebug() const;

      long GetLogDevice() const;
      void SetLogDevice(long newVal);

      // The syslog sink. Seven settings, stored in hm_settings, read by
      // SyslogSink::Start - so a change takes effect at the next service start or
      // Reinitialize, which is what the Control Panel editor says on the page.
      bool GetSyslogEnabled() const;
      void SetSyslogEnabled(bool newVal);

      String GetSyslogHost() const;
      void SetSyslogHost(const String &newVal);

      long GetSyslogPort() const;
      void SetSyslogPort(long newVal);

      long GetSyslogTransport() const;
      void SetSyslogTransport(long newVal);

      long GetSyslogFacility() const;
      void SetSyslogFacility(long newVal);

      long GetSyslogMinimumSeverity() const;
      void SetSyslogMinimumSeverity(long newVal);

      long GetSyslogLogTypes() const;
      void SetSyslogLogTypes(long newVal);

      // One category of the SyslogLogTypes mask, by its Logger::LogSource bit.
      // The same shape as the six GetLog*Conversations above, which read bits of
      // PROPERTY_LOGGING: one stored integer, one switch per category, and no
      // second place for the two to disagree.
      bool GetSyslogLogType(int mask) const;
      void SetSyslogLogType(int mask, bool enabled);

      // The password breach check (PasswordBreachCheck). Four settings in
      // hm_settings, read at every password change, so a change applies to the
      // next one. The getters normalise what a hand-edited row could hold: an
      // empty URL reads as the public corpus, a minimum below one as one, and an
      // action other than refuse (1) as allow (0).
      bool GetBreachCheckEnabled() const;
      void SetBreachCheckEnabled(bool newVal);

      String GetBreachCheckUrl() const;
      void SetBreachCheckUrl(const String &newVal);

      long GetBreachCheckMinimumCount() const;
      void SetBreachCheckMinimumCount(long newVal);

      long GetBreachCheckUnreachableAction() const;
      void SetBreachCheckUnreachableAction(long newVal);

      // Spam traps (SpamTrapMonitor). Four settings in hm_settings: how long a
      // hit bans the connecting address (0 bans nothing), how long the envelope
      // sender stays on the server's block list (0 blocks nobody), how long the
      // record of hits is kept, and whether the message a session that addressed
      // a trap goes on to send is taught to SpamAssassin as spam. None of them
      // does anything until a domain has a trap.
      int GetSpamTrapBanMinutes() const;
      void SetSpamTrapBanMinutes(int minutes);

      int GetSpamTrapBlockDays() const;
      void SetSpamTrapBlockDays(int days);

      int GetSpamTrapHitRetentionDays() const;
      void SetSpamTrapHitRetentionDays(int days);

      bool GetSpamTrapTeachSpamAssassin() const;
      void SetSpamTrapTeachSpamAssassin(bool newVal);

      void SetUseScriptServer(bool newVal);
      bool GetUseScriptServer() const;

      void SetKeepFilesOpen(bool newVal);
      bool GetKeepFilesOpen() const;

      // The reputation watch (ReputationWatch), in hm_settings. Enabled is the
      // daily check and the alert; the lists are the block lists to ask, one
      // entry each, [ipv4:|ipv6:]zone[=codes]; the addresses are the ones an
      // administrator says this server is seen as, beside the bind address and
      // the ones its rules bind. Neither string is normalised on the way in or
      // defaulted on the way out: an empty list of lists, or of addresses, means
      // there is nothing to look up, and that is a decision somebody made.
      bool GetReputationWatchEnabled() const;
      void SetReputationWatchEnabled(bool newVal);

      String GetReputationWatchLists() const;
      void SetReputationWatchLists(const String &newVal);

      String GetReputationWatchAddresses() const;
      void SetReputationWatchAddresses(const String &newVal);

      // The wiki base URL the help panes link to, with the document's name as
      // the page; empty hides the link. Validated where it is set - the REST
      // row and the COM property refuse a value that is not empty and not an
      // absolute http or https URL - so what is stored is usable.
      String GetHelpWikiBaseUrl() const;
      void SetHelpWikiBaseUrl(const String &newVal);


      int GetWorkerThreadPriority() const;
      void SetWorkerThreadPriority(int iNewVal);

      int GetTCPIPThreads() const;
      void SetTCPIPThreads(int iNewVal);

      void SetScriptLanguage(const String &newVal);
      String GetScriptLanguage() const;

      void SetDefaultDomain(const String &newVal);
      String GetDefaultDomain() const;

      String GetMirrorAddress() const;
      void SetMirrorAddress(const String &sAddress);

      // The quarantine digest, in hm_settings as a setting added after 15
      // September 2026 belongs. The base URL is the absolute address the digest
      // writes its release links against; empty means no digest is sent, because
      // a link nobody can follow is worse than no message. The link hours are how
      // long one of those links stays usable, and each also works only once.
      String GetQuarantineDigestBaseUrl() const;
      void SetQuarantineDigestBaseUrl(const String &url);

      int GetQuarantineDigestLinkHours() const;
      void SetQuarantineDigestLinkHours(int hours);

      // Whether the webmail shows the logo a sender's domain publishes (BIMI)
      // beside mail this server judged dmarc=pass. See BrandLogo.h.
      bool GetBrandLogosEnabled() const;
      void SetBrandLogosEnabled(bool enabled);

      // The other accounts a reader may add in the webmail: how many per
      // account (0 offers none), and whether a host on a private address may
      // be one of them, which only an administrator can allow.
      int GetReaderFetchAccountsMax() const;
      void SetReaderFetchAccountsMax(int count);
      bool GetReaderFetchAccountsPrivateAddresses() const;
      void SetReaderFetchAccountsPrivateAddresses(bool allowed);

      // Importing from the old provider with a sign-in (ReaderOAuth.h). The base
      // URL has its own pair; the eight provider settings are read and written by
      // their PROPERTY_IMPORT_OAUTH_* names, the two secrets stored protected.
      // How many addresses elsewhere a reader may prove and send as (SenderAddresses.h); 0 offers none.
      int GetReaderSenderAddressesMax() const;
      void SetReaderSenderAddressesMax(int count);

      // How many masked addresses a reader may make (MaskedAddresses.h); 0 offers none.
      int GetReaderMaskedAddressesMax() const;
      void SetReaderMaskedAddressesMax(int count);

      // How many days a message a person deletes can be recovered (RecoverableStore.h); 0 keeps none.
      int GetRecoverDeletedDays() const;
      void SetRecoverDeletedDays(int days);

      // Whether the accounts and lists of a domain are answered to the people of
      // that domain, and whether a person sets their own entry (DirectoryStore.h).
      bool GetDirectoryEnabled() const;
      void SetDirectoryEnabled(bool enabled);
      bool GetDirectoryUserEditable() const;
      void SetDirectoryUserEditable(bool editable);

      // The recovery address and the one-time recovery codes (AccountRecovery.h):
      // whether they are offered, and how long each of the two links lives.
      bool GetAccountRecoveryEnabled() const;
      void SetAccountRecoveryEnabled(bool enabled);
      int GetRecoveryResetLinkMinutes() const;
      void SetRecoveryResetLinkMinutes(int minutes);
      int GetRecoveryVerifyLinkHours() const;
      void SetRecoveryVerifyLinkHours(int hours);

      // The most days of mail a reader may keep in their browser for reading
      // without a connection (RestApiOffline.cpp); 0 offers none.
      int GetReaderOfflineDaysMax() const;
      void SetReaderOfflineDaysMax(int days);

      // The database id of the example.test the sample domain made
      // (SampleDomain.h), or 0 when it made none. Written by SampleDomain alone.
      __int64 GetSampleDomainId() const;
      void SetSampleDomainId(__int64 domainId);

      // The address the links to messages that expire are written against
      // (ConfidentialMessages.h); empty falls back to the two base URLs below.
      String GetConfidentialBaseUrl() const;
      void SetConfidentialBaseUrl(const String &url);

      // Outbound content policy (Util/ContentPolicy.h): the regular expression
      // the account_number detector searches for; empty never fires.
      String GetDlpAccountNumberPattern() const;
      void SetDlpAccountNumberPattern(const String &pattern);

      // The reports our own domains receive (Util/DomainReports.h): how many
      // days a DMARC aggregate or TLS-RPT report read from a domain's report
      // mailbox is kept, by the day it was read. 0 keeps for ever; at most 3650.
      int GetDomainReportRetentionDays() const;
      void SetDomainReportRetentionDays(int days);
      // Compromised-account detection (CompromiseDetector.h): the mode (0 off,
      // 1 watch, 2 enforce) and the thresholds of the four signals.
      int GetCompromiseDetectionMode() const;
      void SetCompromiseDetectionMode(int mode);
      int GetCompromiseBaselineDays() const;
      void SetCompromiseBaselineDays(int days);
      int GetCompromiseMinimumHistory() const;
      void SetCompromiseMinimumHistory(int messages);
      int GetCompromiseVolumeFactor() const;
      void SetCompromiseVolumeFactor(int factor);
      int GetCompromiseVolumeFloor() const;
      void SetCompromiseVolumeFloor(int messages);
      int GetCompromiseRecipientFloor() const;
      void SetCompromiseRecipientFloor(int recipients);
      int GetCompromiseNewRecipientShare() const;
      void SetCompromiseNewRecipientShare(int percent);
      int GetCompromiseScoreToLock() const;
      void SetCompromiseScoreToLock(int score);
      int GetCompromiseHeldDays() const;
      void SetCompromiseHeldDays(int days);

      String GetImportOAuthRedirectBaseUrl() const;
      void SetImportOAuthRedirectBaseUrl(const String &url);
      String GetImportOAuthString(const String &propertyName) const;
      void SetImportOAuthString(const String &propertyName, const String &value);

      // PASSKEYS AND SINGLE SIGN-ON. Nine rows in hm_settings; see WebAuthn.h,
      // PasskeyStore.h and OidcSignIn.h. The two switches have accessors of
      // their own and the seven strings share one pair, named by the PROPERTY_
      // constants in Constants.h.
      bool GetPasskeysEnabled() const;
      void SetPasskeysEnabled(bool enabled);
      bool GetOidcAutoProvision() const;
      void SetOidcAutoProvision(bool enabled);
      String GetSignInString(const String &propertyName) const;
      void SetSignInString(const String &propertyName, const String &value);

      // COUNTRY BLOCKING. Seven rows in hm_settings; see CountryBlocking.h for
      // what each means and why nothing here is compiled in. The file is the
      // MaxMind database the administrator supplies - empty means no connection
      // is judged by country at all, which is every stock install.
      String GetGeoIpDatabaseFile() const;
      void SetGeoIpDatabaseFile(const String &path);

      int GetGeoIpDatabaseMaxAgeDays() const;
      void SetGeoIpDatabaseMaxAgeDays(int days);

      String GetCountryBlockingExemptRanges() const;
      void SetCountryBlockingExemptRanges(const String &ranges);

      String GetCountryPolicySMTP() const;
      void SetCountryPolicySMTP(const String &policy);

      String GetCountryPolicyIMAP() const;
      void SetCountryPolicyIMAP(const String &policy);

      String GetCountryPolicyPOP3() const;
      void SetCountryPolicyPOP3(const String &policy);

      String GetCountryPolicyHTTP() const;
      void SetCountryPolicyHTTP(const String &policy);

      // The session policies (schema 6050), server-wide; a domain's own row
      // overrides them. 0 in either of the numbers means the policy is not set and
      // the built-in ceiling applies - see SessionPolicies, which is the only place
      // the three levels are resolved into one answer.
      int GetSessionIdleSeconds() const;
      void SetSessionIdleSeconds(int seconds);

      int GetSessionLifetimeSeconds() const;
      void SetSessionLifetimeSeconds(int seconds);

      bool GetSessionOnePerDevice() const;
      void SetSessionOnePerDevice(bool value);

      bool GetSessionBindToAddress() const;
      void SetSessionBindToAddress(bool value);


      // BACKUP OPTIONS
      String GetBackupDestination() const;
      void SetBackupDestination(const String &sNewPath);

      bool GetBackupOption(Backup::BackupOptions iMode) const;
      int GetBackupOptions() const;
      void SetBackupOption(Backup::BackupOptions iMode, bool bSet);

      // THE BACKUP PASSPHRASE
      //
      // The archive holds every mailbox on the server and is written wherever an
      // administrator pointed it - often a share, sometimes a cloud mount - so it
      // can be encrypted, and this is the passphrase it is encrypted with. It is
      // kept the way the route and relayer passwords are kept (Crypt::ProtectSecret:
      // a DPAPI envelope on Windows where ProtectStoredSecretsWithDPAPI applies, the
      // key file on Linux) and is never read back over COM or REST, never logged,
      // never put on a command line, and never written into the archive it protects.
      //
      // Reading it and asking whether one is set are two questions, not one. A
      // stored secret that this machine cannot open - a database carried from
      // another machine, a key file that has gone - reads back as an empty string,
      // and the difference between "no passphrase, write in the clear" and "there is
      // a passphrase and it cannot be opened" is the difference between a backup and
      // a data breach. So the integer records that one was set, the string holds it,
      // and BackupExecuter refuses to write anything when they disagree.
      String GetBackupEncryptionPassphrase() const;
      bool GetBackupEncryptionPassphraseSet() const;

      // False with the reason - the sentence to show the administrator - when the
      // passphrase is refused or could not be protected for storage; nothing is
      // changed then. An empty passphrase clears it, and the next backup is written
      // unencrypted.
      bool SetBackupEncryptionPassphrase(const String &passphrase, String &failureReason);

      // Empty when the passphrase is one this server will store, otherwise the
      // reason it will not. Static: the REST layer asks before it applies anything.
      static String CheckBackupEncryptionPassphrase(const String &passphrase);

      int GetRequiredDBVersion() { return REQUIRED_DB_VERSION; }

      String GetHostName() const;
      void SetHostName(String &NewVal);

      bool XMLStore(XNode *pBackupNode);
      bool XMLLoad(XNode *pBackupNode, int iRestoreOptions);

      bool GetDisconnectInvalidClients();
      void SetDisconnectInvalidClients(bool bNewVal);

      int GetMaximumIncorrectCommands();
      void SetMaximumIncorrectCommands(int iNewVal);

      bool GetAutoBanLogonEnabled();
      void SetAutoBanLogonEnabled(bool newValue);

      int GetMaxInvalidLogonAttempts();
      void SetMaxInvalidLogonAttempts(int iNewVal);

      int GetMaxLogonAttemptsWithin();
      void SetMaxLogonAttemptsWithin(int iNewVal);

      int GetAutoBanMinutes();
      void SetAutoBanMinutes(int iNewVal);

      // The quota policy the domains inherit: how far past its limit a mailbox
      // still accepts incoming mail (0 is not at all), and the share of the
      // limit at which the account stops sending (0 is never).
      int GetQuotaGracePercent();
      void SetQuotaGracePercent(int iNewVal);
      int GetQuotaSendStopPercent();
      void SetQuotaSendStopPercent(int iNewVal);

      int GetAsynchronousThreads() const;
      void SetAsynchronousThreads(int iNewVal);

      bool ClearOldLogonFailures();

      bool GetMessageIndexing();
      void SetMessageIndexing(bool enable);

      bool GetVerifyRemoteSslCertificate() const;
      void SetVerifyRemoteSslCertificate(bool newValue);

      String GetSslCipherList() const;
      void SetSslCipherList(String newValue);

      bool GetSslVersionEnabled(SslTlsVersion version) const;
      void SetSslVersionEnabled(SslTlsVersion version, bool enabled);

      bool GetTlsOptionEnabled(TlsOption option) const;
      void SetTlsOptionEnabled(TlsOption option, bool enabled);

      bool GetIPv6Preferred();
      void SetIPv6Preferred(bool enabled);

      bool GetCreateDefaultSpecialUseFolders();
      void SetCreateDefaultSpecialUseFolders(bool enabled);

      // The audit trail (see Common/Util/AuditTrail.h) and the alerts (see
      // Common/Util/AlertManager.h). All nine live in hm_settings, which is
      // where a setting belongs; none of them is an hMailServer.ini key.
      bool GetAuditTrailEnabled();
      void SetAuditTrailEnabled(bool enabled);
      int GetAuditRetentionDays();
      void SetAuditRetentionDays(int days);

      bool GetAlertsEnabled();
      void SetAlertsEnabled(bool enabled);
      String GetAlertRecipient();
      void SetAlertRecipient(const String &address);
      String GetAlertSenderAddress();
      void SetAlertSenderAddress(const String &address);
      bool GetAlertDigestEnabled();
      void SetAlertDigestEnabled(bool enabled);
      int GetAlertDigestHour();
      void SetAlertDigestHour(int hour);
      int GetAlertMaxPerHour();
      void SetAlertMaxPerHour(int maximum);
      int GetAlertWebhookMaxAttempts();
      void SetAlertWebhookMaxAttempts(int attempts);

      // Outbound webhooks (see Util/WebhookManager.h). Four rows in hm_settings.
      // The getters clamp to the documented range, so a hand-edited row cannot
      // make a subscription unlimited or a queue immortal.
      bool GetWebhooksEnabled();
      void SetWebhooksEnabled(bool enabled);
      int GetWebhookMaxAttempts();
      void SetWebhookMaxAttempts(int attempts);
      int GetWebhookMaxPerMinute();
      void SetWebhookMaxPerMinute(int maximum);
      int GetWebhookRetentionDays();
      void SetWebhookRetentionDays(int days);

      // Migration jobs (Application/MigrationManager.h): the most connections
      // every running job of every migration together holds to one source
      // host, 1 to 64. The getter clamps, so a hand-edited row cannot make it
      // unbounded or zero, which would start nothing.
      int GetMigrationConnectionsPerHost();
      void SetMigrationConnectionsPerHost(int connections);

      // Bounces read, and the suppression list (see SMTP/BounceTracker.h). All
      // eight live in hm_settings. The getters clamp to the documented range, so a
      // hand-edited row cannot turn a threshold of 0 into "list on no bounce at all".
      bool GetBounceProcessingEnabled();
      void SetBounceProcessingEnabled(bool enabled);
      int GetBounceHardBounceThreshold();          // 1 to 100
      void SetBounceHardBounceThreshold(int threshold);
      bool GetBounceMailboxFullIsHard();
      void SetBounceMailboxFullIsHard(bool enabled);
      int GetBounceSuppressionAction();            // 0 refuse, 1 drop, 2 observe
      void SetBounceSuppressionAction(int action);
      int GetBounceSuppressionExpiryDays();        // 0 never, up to 3650
      void SetBounceSuppressionExpiryDays(int days);
      bool GetBounceReadDeliveryReports();
      void SetBounceReadDeliveryReports(bool enabled);
      int GetBounceCountRetentionDays();           // 0 keep, up to 3650
      void SetBounceCountRetentionDays(int days);
      int GetBounceReportWindowDays();             // 1 to 30
      void SetBounceReportWindowDays(int days);

      // The Web Key Directory (docs/OpenPGP.md). All four live in hm_settings, and
      // the getters clamp, so a hand-edited row cannot make the size cap 0 - which
      // would refuse every key - or let one account fill the store.
      bool GetOpenPGPWebKeyDirectoryEnabled();
      void SetOpenPGPWebKeyDirectoryEnabled(bool enabled);
      int GetOpenPGPKeyMaximumBytes();             // 1 KB to 1 MB
      void SetOpenPGPKeyMaximumBytes(int bytes);
      int GetOpenPGPKeysPerAccount();              // 1 to 64
      void SetOpenPGPKeysPerAccount(int keys);
      bool GetOpenPGPRequireAddressInUserId();
      void SetOpenPGPRequireAddressInUserId(bool required);

      // OpenPGP in the webmail (Common/Util/OpenPgpRing.h, docs/OpenPGP.md):
      // whether the page offers it, how a correspondent's key is looked up, and
      // the deadline on the fetch the server makes for it.
      bool GetOpenPGPPortalEnabled();
      void SetOpenPGPPortalEnabled(bool enabled);

      // Whether an account may opt into being a zero-access mailbox
      // (docs/ZeroAccess.md). Off by default; allowing it switches nobody on.
      bool GetZeroAccessAllowed();
      void SetZeroAccessAllowed(bool allowed);
      bool GetOpenPGPLookupWkd();
      void SetOpenPGPLookupWkd(bool enabled);
      bool GetOpenPGPLookupKeyserver();
      void SetOpenPGPLookupKeyserver(bool enabled);
      String GetOpenPGPKeyserverUrl();
      void SetOpenPGPKeyserverUrl(const String &url);
      int GetOpenPGPLookupTimeout();               // 2 to 30 seconds
      void SetOpenPGPLookupTimeout(int seconds);
      // A password-protected message (Common/Util/ProtectedMessages.h): how
      // many days its ciphertext is kept for the reading page, 1 to 365.
      int GetProtectedMessageDays();
      void SetProtectedMessageDays(int days);
      // Message recall within the server (Common/Util/MessageRecall.h): how
      // many days after sending an account may still withdraw the unread
      // copies, and how long the rows are kept. 1 to 365.
      int GetRecallDays();
      void SetRecallDays(int days);

      // Outbound shaping (SMTP/DestinationBackoff.h, docs/OutboundShaping.md).
      // Four rows in hm_settings; the getters clamp, so a hand-edited row
      // cannot make a window of a week or a ceiling of a million.
      int GetOutboundBackoffMinutes();             // 0 (off) to 10080
      void SetOutboundBackoffMinutes(int minutes);
      int GetOutboundBackoffMaxMinutes();          // 1 to 10080; read never below the base
      void SetOutboundBackoffMaxMinutes(int minutes);
      int GetMaxOutboundConnectionsPerDestination(); // 0 (off) to 1000
      void SetMaxOutboundConnectionsPerDestination(int connections);
      int GetMaxOutboundConnectionsPerTenant();    // 0 (off) to 1000
      void SetMaxOutboundConnectionsPerTenant(int connections);

      // Attachments opened somewhere safe (AntiVirus/Sandbox.h, docs/Sandbox.md).
      // Thirteen rows in hm_settings; the getters clamp, so a hand-edited row
      // cannot make a poll every millisecond or a hold that never ends. The
      // setters store what they are given within the range: every interface
      // refuses a value outside it before it gets here, in the same sentence.
      // The two addresses are stored trimmed, whichever interface set them.
      bool GetSandboxEnabled();
      void SetSandboxEnabled(bool enabled);
      String GetSandboxSubmitUrl();
      void SetSandboxSubmitUrl(const String &url);
      String GetSandboxVerdictUrl();
      void SetSandboxVerdictUrl(const String &url);
      // Protected at rest (PropertySet::IsCryptedProperty_) and never emitted.
      String GetSandboxToken();
      void SetSandboxToken(const String &token);
      int GetSandboxTimeoutMinutes();              // 1 to 1440
      void SetSandboxTimeoutMinutes(int minutes);
      int GetSandboxTimeoutAction();               // 0 deliver with a header, 1 return to the sender
      void SetSandboxTimeoutAction(int action);
      int GetSandboxMaxSizeKB();                   // 1 to 81920, what MessageData can load
      void SetSandboxMaxSizeKB(int kilobytes);
      String GetSandboxTypes();                    // canonical: lower case, comma separated
      void SetSandboxTypes(const String &types);
      int GetSandboxPollSeconds();                 // 5 to 3600
      void SetSandboxPollSeconds(int seconds);
      int GetSandboxScoreThreshold();              // 0 to 100
      void SetSandboxScoreThreshold(int threshold);
      int GetSandboxCacheHours();                  // 0 (nothing remembered) to 720
      void SetSandboxCacheHours(int hours);
      bool GetSandboxInternalMail();
      void SetSandboxInternalMail(bool enabled);
      int GetSandboxMaxFiles();                    // 1 to 100: the files one message may submit
      void SetSandboxMaxFiles(int files);

      // Web Push (see Common/Util/WebPush.h). Seven rows in hm_settings; the
      // getters clamp, so a hand-edited row cannot make a time to live of a year
      // or an attempt count of none.
      bool GetWebPushEnabled();
      void SetWebPushEnabled(bool enabled);
      String GetWebPushSubject();
      void SetWebPushSubject(const String &subject);
      int GetWebPushTimeToLiveSeconds();           // 0 to 2419200 (four weeks)
      void SetWebPushTimeToLiveSeconds(int seconds);
      int GetWebPushMaxPerAccount();               // 1 to 50
      void SetWebPushMaxPerAccount(int maximum);
      int GetWebPushMaxAttempts();                 // 1 to 20
      void SetWebPushMaxAttempts(int attempts);

      // The public half of the VAPID key pair, which every page needs to
      // subscribe at all, and which is therefore emitted freely.
      String GetWebPushVapidPublicKey();

      // The private half. There is no getter on any interface: it is read here,
      // by the one class that signs with it, and by nothing else. Setting it also
      // derives and stores the public half, so the two can never disagree.
      String GetWebPushVapidPrivateKey();
      bool SetWebPushVapidKeys(const String &privateKey, String &refusal);

      // A fresh pair. Replacing a pair invalidates every subscription made under
      // the old one, which is why nothing does this on its own.
      bool GenerateWebPushVapidKeys(String &refusal);

      // Complaints, read: the feedback loops (see SMTP/ComplaintTracker.h). Off,
      // with no mailbox, and with either of those nothing is read, counted or
      // written. Stored in hm_settings like the bounce settings above.
      bool GetComplaintProcessingEnabled();
      void SetComplaintProcessingEnabled(bool enabled);
      String GetComplaintMailbox();                // empty: no feedback loop
      void SetComplaintMailbox(const String &mailbox);
      int GetComplaintSuppressionThreshold();      // 0 never lists, up to 100
      void SetComplaintSuppressionThreshold(int threshold);
      int GetComplaintRetentionDays();             // 0 keeps, up to 3650
      void SetComplaintRetentionDays(int days);

      int GetCrashSimulationMode() const;
      void SetCrashSimulationMode(int mode);


      std::shared_ptr<PropertySet> GetSettings() const;

      std::shared_ptr<ServerMessages> GetServerMessages() {return server_messages_;}
      std::shared_ptr<TCPIPPorts> GetTCPIPPorts() const;
      std::shared_ptr<BlockedAttachments> GetBlockedAttachments() {return blocked_attachments_;}
      std::shared_ptr<SSLCertificates> GetSSLCertificates() {return ssl_certificates_;}
      std::shared_ptr<PropertySet> GetPropertySet() {return property_set_;}

   private:

      // Creates the hm_settings rows that a database made before a feature
      // existed has none of, each with the default that is today's behaviour.
      // Runs once per Load, on the start-up thread, before anything else can read
      // the property set. See PropertySet::EnsureLong for why it is needed at all.
      void EnsureSettingDefaults_();

      std::shared_ptr<POP3Configuration> pop3_configuration_;
      std::shared_ptr<SMTPConfiguration> smtp_configuration_;
      std::shared_ptr<IMAPConfiguration> imap_configuration_;
      
      std::shared_ptr<CacheConfiguration> cache_configuration_;

      AntiSpamConfiguration anti_spam_configuration_;
      AntiVirusConfiguration anti_virus_configuration_;

      std::shared_ptr<ServerMessages> server_messages_;
      std::shared_ptr<TCPIPPorts> tcpip_ports_;
      std::shared_ptr<BlockedAttachments> blocked_attachments_;
      std::shared_ptr<SSLCertificates> ssl_certificates_;

      std::shared_ptr<PropertySet> property_set_;

      int crash_simulation_mode_;

      bool ipv6_available_;
   };
}
