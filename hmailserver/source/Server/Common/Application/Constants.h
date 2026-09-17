// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The Web Key Directory: this server publishing its users' OpenPGP public keys
// (docs/OpenPGP.md). In hm_settings by the rule of 15 September 2026, with rows
// created at start by EnsureLong when a database has none and seeded by the
// 6049 to 6050 step. Publishing is OFF, which is the server as it has always
// been: an upgrade must not start publishing anybody's key, and it does not -
// the domain switch and the account switch are 0 on every existing row as well.
#define PROPERTY_MAXPOP3CONNECTIONS             _T("maxpop3connections")
#define PROPERTY_MAXSMTPCONNECTIONS             _T("maxsmtpconnections")
#define PROPERTY_MIRROREMAILADDRESS             _T("mirroremailaddress")
#define PROPERTY_AUTHALLOWPLAINTEXT             _T("authallowplaintext")
#define PROPERTY_ALLOWMAILFROMNULL              _T("allowmailfromnull")
#define PROPERTY_LOGDEVICE                      _T("logdevice")
#define PROPERTY_LOGGING                        _T("logging")
#define PROPERTY_SMTPNOOFTRIES                  _T("smtpnoofretries")
#define PROPERTY_SMTPMINUTESBETWEEN             _T("smtpminutesbetweenretries")
#define PROPERTY_WELCOMEIMAP                    _T("welcomeimap")
#define PROPERTY_WELCOMEPOP3                    _T("welcomepop3")
#define PROPERTY_WELCOMESMTP                    _T("welcomesmtp")
#define PROPERTY_SMTPRELAYER                    _T("smtprelayer")
#define PROPERTY_PROTOCOLSMTP                   _T("protocolsmtp")
#define PROPERTY_PROTOCOLPOP3                   _T("protocolpop3")
#define PROPERTY_PROTOCOLIMAP                   _T("protocolimap")
#define PROPERTY_LOGFORMAT                      _T("logformat")
#define PROPERTY_MAXDELIVERYTHREADS             _T("maxdelivertythreads")
#define PROPERTY_AV_CLAMWIN_ENABLE              _T("avclamwinenable")
#define PROPERTY_AV_CLAMWIN_EXEC                _T("avclamwinexec")
#define PROPERTY_AV_CLAMWIN_DB                  _T("avclamwindb")
#define PROPERTY_AV_NOTIFY_SENDER               _T("avnotifysender")
#define PROPERTY_AV_NOTIFY_RECEIVER             _T("avnotifyreceiver")
#define PROPERTY_AV_DELETE_INFC_EMAIL           _T("avdeleteinfectedemail")
#define PROPERTY_AV_ACTION                      _T("avaction")
#define PROPERTY_HOSTNAME                       _T("hostname")
#define PROPERTY_SMTPRELAYER_USERNAME           _T("smtprelayerusername")
#define PROPERTY_SMTPRELAYER_PASSWORD           _T("smtprelayerpassword")
#define PROPERTY_SMTPRELAYER_USEAUTH            _T("usesmtprelayerauthentication")
#define PROPERTY_SMTPRELAYER_PORT               _T("smtprelayerport")
#define PROPERTY_USESCRIPTSERVER                _T("usescriptserver")
#define PROPERTY_SCRIPTLANGUAGE                 _T("scriptlanguage")
#define PROPERTY_MAXMESSAGESIZE                 _T("maxmessagesize")
#define PROPERTY_USECACHE                       _T("usecache")
#define PROPERTY_DOMAINCACHETTL                 _T("domaincachettl")
#define PROPERTY_ACCOUNTCACHETTL                _T("accountcachettl")
#define PROPERTY_ALIASCACHETTL                  _T("aliascachettl")
#define PROPERTY_DISTRIBUTIONLISTCACHETTL       _T("distributionlistcachettl")
#define PROPERTY_GROUPCACHETTL                  _T("groupcachettl")
#define PROPERTY_AWSTATSENABLED                 _T("awstatsenabled")
#define PROPERTY_RULELOOPLIMIT                  _T("rulelooplimit")

#define PROPERTY_USECUSTOMSCANNER               _T("usecustomvirusscanner")
#define PROPERTY_CUSTOMSCANNEREXECUTABLE        _T("customvirusscannerexecutable")
#define PROPERTY_CUSTOMSCANNERRETURNVALUE       _T("customviursscannerreturnvalue")

#define PROPERTY_USESPF                         _T("usespf")
#define PROPERTY_USESPFSCORE                    _T("usespfscore")
#define PROPERTY_USEMXCHECK                     _T("usemxchecks")
#define PROPERTY_USEMXCHECKSCORE                _T("usemxchecksscore")
#define PROPERTY_AS_CHECKHOSTINHELO             _T("ascheckhostinhelo")
#define PROPERTY_AS_CHECKHOSTINHELOSCORE        _T("ascheckhostinheloscore")
#define PROPERTY_AS_CHECKPTR                    _T("ascheckptr")
#define PROPERTY_AS_CHECKPTRSCORE               _T("ascheckptrscore")

#define PROPERTY_BACKUPOPTIONS                  _T("backupoptions")
#define PROPERTY_BACKUPDESTINATION              _T("backupdestination")
// The passphrase the backup archive is encrypted with. A crypted property: the
// string is the passphrase, kept protected by the platform's secret store
// (PropertySet::IsCryptedProperty_), and the integer says whether one is set -
// which is not the same question, because a stored secret this machine cannot
// open reads back empty, and a backup must fail rather than quietly write an
// unencrypted archive. Never written into a backup's own settings section.
#define PROPERTY_BACKUPENCRYPTIONPASSPHRASE     _T("backupencryptionpassphrase")

#define PROPERTY_DEFAULTDOMAIN                  _T("defaultdomain")
#define PROPERTY_AV_MAXMSGSIZE                  _T("avmaxmsgsize")
#define PROPERTY_SMTPDELIVERYBINDTOIP           _T("smtpdeliverybindtoip")
#define PROPERTY_ENABLEIMAPQUOTA                _T("enableimapquota")
#define PROPERTY_ENABLEIMAPIDLE                 _T("enableimapidle")
#define PROPERTY_ENABLEIMAPSORT                 _T("enableimapsort")
#define PROPERTY_ENABLEIMAPACL                  _T("enableimapacl")
#define PROPERTY_MAXIMAPCONNECTIONS             _T("maximapconnections")
#define PROPERTY_WORKERTHREADPRIORITY           _T("workerthreadpriority")

#define PROPERTY_TCPIPTHREADS                   _T("tcpipthreads")
#define PROPERTY_ALLOWINCORRECTLINEENDINGS      _T("smtpallowincorrectlineendings")
#define PROPERTY_MAXSMTPRECIPIENTSINBATCH       _T("maxsmtprecipientsinbatch")
#define PROPERTY_IMAPPUBLICFOLDERNAME           _T("imappublicfoldername")

#define PROPERTY_USEGREYLISTING                 _T("usegreylisting")
#define PROPERTY_GL_INITIALDELAY                _T("greylistinginitialdelay")
#define PROPERTY_GL_INITIALDELETE               _T("greylistinginitialdelete")
#define PROPERTY_GL_FINALDELETE                 _T("greylistingfinaldelete")

#define PROPERTY_AS_ADDHEADERSPAM               _T("antispamaddheaderspam")
#define PROPERTY_AS_ADDHEADERREASON             _T("antispamaddheaderreason")
#define PROPERTY_AS_PREPENDSUBJECT              _T("antispamprependsubject")
#define PROPERTY_AS_PREPENDSUBJECTTEXT          _T("antispamprependsubjecttext")
#define PROPERTY_AS_MAX_SIZE                    _T("antispammaxsize")

#define PROPERTY_ENABLEATTACHMENTBLOCKING       _T("enableattachmentblocking")
#define PROPERTY_SMTPDISCONNECTINVALIDCLIENTS   _T("disconnectinvalidclients")
#define PROPERTY_SMTPMAXINCORRECTCOMMANDS       _T("maximumincorrectcommands")

#define PROPERTY_SMTPRELAYERCONNECTIONSECURITY  _T("smtprelayerconnectionsecurity")

#define PROPERTY_ADDDELIVEREDTOHEADER           _T("adddeliveredtoheader")

#define PROPERTY_SPAMDELETETHRESHOLD            _T("spamdeletethreshold")
#define PROPERTY_SPAMMARKTHRESHOLD              _T("spammarkthreshold")

#define PROPERTY_SPAMASSASSIN_ENABLED           _T("spamassassinenabled")
#define PROPERTY_SPAMASSASSIN_SCORE             _T("spamassassinscore")
#define PROPERTY_SPAMASSASSIN_MERGE_SCORE       _T("spamassassinmergescore")
#define PROPERTY_SPAMASSASSIN_HOST              _T("spamassassinhost")
#define PROPERTY_SPAMASSASSIN_PORT              _T("spamassassinport")

#define PROPERTY_AS_DKIM_VERIFICATION_ENABLE          _T("ASDKIMVerificationEnabled")
#define PROPERTY_AS_DKIM_VERIFICATION_FAILURE_SCORE   _T("ASDKIMVerificationFailureScore")

#define PROPERTY_AS_DMARC_ENABLE                _T("ASDMARCEnabled")
#define PROPERTY_AS_DMARC_FAILURE_SCORE         _T("ASDMARCFailureScore")

// ARC inbound filtering (RFC 8617). Seeded as hm_settings rows at schema 6010.
// The trusted-sealer list is not an option of the feature, it IS the feature: a
// fabricated chain sealed with the attacker's own key validates perfectly, so a
// passing chain proves nothing unless the sealer is already trusted. With the list
// empty the test does nothing at all.
#define PROPERTY_AS_ARC_FILTERING_ENABLED       _T("ASArcFilteringEnabled")
#define PROPERTY_AS_ARC_TRUSTED_SEALERS         _T("ASArcTrustedSealers")

#define PROPERTY_AUTOBAN_ENABLED                _T("AutoBanOnLogonFailureEnabled")
#define PROPERTY_MAX_INVALID_LOGON_ATTEMPTS     _T("MaxInvalidLogonAttempts")
#define PROPERTY_LOGON_ATTEMPTS_WITHIN          _T("LogonAttemptsWithinMinutes")
#define PROPERTY_AUTOBAN_MINUTES                _T("AutoBanMinutes")

#define PROPERTY_IMAP_HIERARCHY_DELIMITER       _T("IMAPHierarchyDelimiter")
#define PROPERTY_MESSAGE_INDEXING               _T("MessageIndexing")
#define PROPERTY_MAX_NUMBER_OF_ASYNC_TASKS      _T("MaxNumberOfAsynchronousTasks")

#define PROPERTY_BYPASS_GREYLISTING_ON_SPFSUCCESS  _T("BypassGreylistingOnSPFSuccess")
#define PROPERTY_BYPASS_GREYLISTING_ON_MAILFROMMX  _T("BypassGreylistingOnMailFromMX")

#define PROPERTY_MAX_NUMBER_OF_MXHOSTS          _T("MaxNumberOfMXHosts")
#define PROPERTY_SMTPCONNECTIONSECURITY         _T("SmtpDeliveryConnectionSecurity")
#define PROPERTY_VERIFYREMOTESSLCERTIFICATE     _T("VerifyRemoteSslCertificate")

#define PROPERTY_SSLCIPHERLIST _T("SslCipherList")
#define PROPERTY_SSLVERSIONS _T("SslVersions")

#define PROPERTY_CLAMAV_ENABLED              _T("ClamAVEnabled")
#define PROPERTY_CLAMAV_HOST                 _T("ClamAVHost")
#define PROPERTY_CLAMAV_PORT                 _T("ClamAVPort")

#define PROPERTY_IMAPMASTERUSER                _T("ImapMasterUser")
#define PROPERTY_ENABLEIMAPSASLPLAIN           _T("EnableImapSASLPlain")
#define PROPERTY_ENABLEIMAPSASLINTIALRESPONSE  _T("EnableImapSASLInitialResponse")

#define PROPERTY_IPV6_PREFERRED  _T("IPv6Preferred")

// Create Drafts, Sent, Trash and Junk, each designated for RFC 6154, when an
// account is created. Off by default: an account created while it is off gets
// an inbox only, exactly as before. Upstream #550.
#define PROPERTY_CREATE_DEFAULT_SPECIAL_USE_FOLDERS  _T("CreateDefaultSpecialUseFolders")

#define PROPERTY_TLSOPTIONS                     _T("TlsOptions")

// The quarantine digest. Both rows live in hm_settings, which is where a
// setting belongs since 15 September 2026: the base URL the digest's release
// links are written against - without one no digest is sent, because a link
// nobody can follow is worse than no message - and how many hours one of those
// links stays usable.
#define PROPERTY_QUARANTINE_DIGEST_BASE_URL     _T("QuarantineDigestBaseUrl")
#define PROPERTY_QUARANTINE_DIGEST_LINK_HOURS   _T("QuarantineDigestLinkHours")

// Country blocking. Seven rows in hm_settings, which is where a setting belongs:
// the MaxMind database the administrator supplies (this project ships none and
// downloads none), how old that file may be before the log says so, the
// addresses no policy judges, and one policy per listener family written as
// "off", "allow:GB,IE" or "block:CN,RU". Every one of them is empty or off in a
// stock install, so a server nobody has configured behaves exactly as it did.
#define PROPERTY_GEOIP_DATABASE_FILE            _T("GeoIpDatabaseFile")
#define PROPERTY_GEOIP_DATABASE_MAX_AGE_DAYS    _T("GeoIpDatabaseMaxAgeDays")
#define PROPERTY_COUNTRY_EXEMPT_RANGES          _T("CountryBlockingExemptRanges")
#define PROPERTY_COUNTRY_POLICY_SMTP            _T("CountryPolicySMTP")
#define PROPERTY_COUNTRY_POLICY_IMAP            _T("CountryPolicyIMAP")
#define PROPERTY_COUNTRY_POLICY_POP3            _T("CountryPolicyPOP3")
#define PROPERTY_COUNTRY_POLICY_HTTP            _T("CountryPolicyHTTP")
// The session policies (schema 6050). Every one is 0 on a fresh install and after
// an upgrade, and 0 means THE POLICY IS NOT SET rather than "no limit": the idle
// and lifetime ceilings the browser sessions have always had - thirty minutes and
// twelve hours - apply, and the two switches are off. A domain's own row
// (hm_domains.domainsession*) overrides these for the accounts of that domain; the
// administrator's own session has no domain and is governed by these alone.
// SessionPolicies is where the three are resolved into one answer.
#define PROPERTY_SESSION_IDLE_SECONDS           _T("SessionIdleSeconds")
#define PROPERTY_SESSION_LIFETIME_SECONDS       _T("SessionLifetimeSeconds")
#define PROPERTY_SESSION_ONE_PER_DEVICE         _T("SessionOnePerDevice")
#define PROPERTY_SESSION_BIND_TO_ADDRESS        _T("SessionBindToAddress")

// The audit trail. Recording is on by default: a server nobody has configured
// still answers "who changed this", and the cost is one row per administrative
// change. Retention of 0 keeps everything, which is what an audit trail is for;
// a number of days is for the installation whose own policy says so.
#define PROPERTY_AUDIT_TRAIL_ENABLED            _T("AuditTrailEnabled")
#define PROPERTY_AUDIT_RETENTION_DAYS           _T("AuditRetentionDays")

// Alerts. The master switch is on, and the rules in hm_alertrules are off
// except the two worth waking up for; with no AlertRecipient set nothing is
// sent, which is what makes "on" safe in a stock install.
#define PROPERTY_ALERTS_ENABLED                 _T("AlertsEnabled")
#define PROPERTY_ALERT_RECIPIENT                _T("AlertRecipient")
#define PROPERTY_ALERT_SENDER_ADDRESS           _T("AlertSenderAddress")
#define PROPERTY_ALERT_DIGEST_ENABLED           _T("AlertDigestEnabled")
#define PROPERTY_ALERT_DIGEST_HOUR              _T("AlertDigestHour")
#define PROPERTY_ALERT_MAX_PER_HOUR             _T("AlertMaxPerHour")
#define PROPERTY_ALERT_WEBHOOK_MAX_ATTEMPTS     _T("AlertWebhookMaxAttempts")

// Outbound webhooks. The master switch is on, and it opens nothing on its own:
// a server with no subscription in hm_webhooks raises no event and makes no
// request, so "on" costs a stock install one comparison per delivery. The
// switch is what an administrator reaches for when a receiver has to be
// silenced at once without deleting the subscriptions that name it.
//
// The other three are the limits an administrator can move: the size of a key
// this server will take (64 KiB, which is a key with a very large number of
// certifications on it), how many keys one account may hold at once (8, so that
// a key being rotated and the key it replaces can both be published), and
// whether a key whose user ids do not carry the account's own address is
// refused. An administrator can override the last one for one upload; nobody
// else can.
#define PROPERTY_OPENPGP_WKD_ENABLED            _T("OpenPGPWebKeyDirectoryEnabled")
#define PROPERTY_OPENPGP_KEY_MAXIMUM_BYTES      _T("OpenPGPKeyMaximumBytes")
#define PROPERTY_OPENPGP_KEYS_PER_ACCOUNT       _T("OpenPGPKeysPerAccount")
#define PROPERTY_OPENPGP_REQUIRE_ADDRESS        _T("OpenPGPRequireAddressInUserId")
// WebhookMaxPerMinute is the ceiling a subscription that names no ceiling of
// its own is held to. Past it events are DROPPED rather than queued, and the
// count of what was dropped is reported: a queue that grows without bound is
// how a busy hour becomes a flood an hour later, and a receiver that cannot
// keep up wants to know it fell behind rather than to be told about it for the
// rest of the day.
#define PROPERTY_WEBHOOKS_ENABLED               _T("WebhooksEnabled")
#define PROPERTY_WEBHOOK_MAX_ATTEMPTS           _T("WebhookMaxAttempts")
#define PROPERTY_WEBHOOK_MAX_PER_MINUTE         _T("WebhookMaxPerMinute")
#define PROPERTY_WEBHOOK_RETENTION_DAYS         _T("WebhookRetentionDays")
// The syslog sink (RFC 5424), beside the log files rather than instead of them.
// In hm_settings rather than in hMailServer.ini because of the rule in
// .github/CONTRIBUTING.md from 15 September 2026: a setting belongs in the
// database. The rows are created with these defaults by
// PropertySet::EnsureLong/EnsureString at start-up when they are absent, which is
// every installation that predates the feature - so no reader ever sees a missing
// property, and none of them reports HM5015 on a server that has never touched
// the page.
//
// SyslogHost empty is not "off": on Linux under systemd it is what selects the
// journal, which is where a Linux administrator expects the lines to be.
#define PROPERTY_SYSLOG_ENABLED                 _T("SyslogEnabled")
#define PROPERTY_SYSLOG_HOST                    _T("SyslogHost")
#define PROPERTY_SYSLOG_PORT                    _T("SyslogPort")
#define PROPERTY_SYSLOG_TRANSPORT               _T("SyslogTransport")
#define PROPERTY_SYSLOG_FACILITY                _T("SyslogFacility")
#define PROPERTY_SYSLOG_SEVERITY                _T("SyslogMinimumSeverity")
#define PROPERTY_SYSLOG_LOGTYPES                _T("SyslogLogTypes")

// The check of a new password against the published breach corpus
// (PasswordBreachCheck). In hm_settings for the same reason as the syslog sink,
// with rows created by EnsureSettingDefaults_. Off by default, because it sends
// five characters of the password's hash off the server; the default endpoint is
// the public corpus; one sighting is enough to refuse; and an endpoint that
// cannot be used lets the change through (0) rather than refusing it (1).
//
// The names say BreachCheck and never Password on purpose: AuditTrail::IsSecretName
// records any setting whose name contains "password" as "(changed)", and an auditor
// needs to read that this one went from false to true.
#define PROPERTY_BREACH_CHECK_ENABLED           _T("BreachCheckEnabled")
#define PROPERTY_BREACH_CHECK_URL               _T("BreachCheckUrl")
#define PROPERTY_BREACH_CHECK_MINIMUM_COUNT     _T("BreachCheckMinimumCount")
#define PROPERTY_BREACH_CHECK_UNREACHABLE_ACTION _T("BreachCheckUnreachableAction")
#define PROPERTY_SPAM_TRAP_BAN_MINUTES          _T("SpamTrapBanMinutes")
#define PROPERTY_SPAM_TRAP_BLOCK_DAYS           _T("SpamTrapBlockDays")
#define PROPERTY_SPAM_TRAP_HIT_RETENTION_DAYS   _T("SpamTrapHitRetentionDays")
#define PROPERTY_SPAM_TRAP_TEACH_SPAMASSASSIN   _T("SpamTrapTeachSpamAssassin")

// Bounces read, and a suppression list (SMTP/BounceTracker.h). In hm_settings, by
// the rule of 15 September 2026, with rows created at start by EnsureLong when a
// database has none. Every default is the server as it was: processing off, so
// nothing is classified, counted, listed or refused until an administrator says so.
#define PROPERTY_BOUNCE_PROCESSING_ENABLED      _T("BounceProcessingEnabled")
#define PROPERTY_BOUNCE_HARD_THRESHOLD          _T("BounceHardBounceThreshold")
#define PROPERTY_BOUNCE_MAILBOX_FULL_IS_HARD    _T("BounceMailboxFullIsHard")
#define PROPERTY_BOUNCE_SUPPRESSION_ACTION      _T("BounceSuppressionAction")
#define PROPERTY_BOUNCE_SUPPRESSION_EXPIRY_DAYS _T("BounceSuppressionExpiryDays")
#define PROPERTY_BOUNCE_READ_REPORTS            _T("BounceReadDeliveryReports")
#define PROPERTY_BOUNCE_COUNT_RETENTION_DAYS    _T("BounceCountRetentionDays")
#define PROPERTY_BOUNCE_REPORT_WINDOW_DAYS      _T("BounceReportWindowDays")

// Web Push (Common/Util/WebPush.h). In hm_settings, by the rule of 15 September
// 2026, with rows created at start by EnsureSettingDefaults_ and seeded by the
// 6050 upgrade step. Off by default: with WebPushEnabled 0 nothing is queued,
// nothing is signed and no push service is ever reached.
//
// The private key's NAME carries "privatekey" on purpose, so that
// AuditTrail::IsSecretName records a change to it as "(changed)" rather than
// writing the key into the audit trail; PropertySet::IsCryptedProperty_ protects
// it at rest as it does the relayer password. The public key is public by
// definition and is named so that nothing masks it - the page needs it to
// subscribe at all.
#define PROPERTY_WEB_PUSH_ENABLED               _T("WebPushEnabled")
#define PROPERTY_WEB_PUSH_SUBJECT               _T("WebPushSubject")
#define PROPERTY_WEB_PUSH_TTL_SECONDS           _T("WebPushTimeToLiveSeconds")
#define PROPERTY_WEB_PUSH_MAX_PER_ACCOUNT       _T("WebPushMaxPerAccount")
#define PROPERTY_WEB_PUSH_MAX_ATTEMPTS          _T("WebPushMaxAttempts")
#define PROPERTY_WEB_PUSH_VAPID_PUBLIC_KEY      _T("WebPushVapidPublicKey")
#define PROPERTY_WEB_PUSH_VAPID_PRIVATE_KEY     _T("WebPushVapidPrivateKey")
// Complaints, read: the feedback loops (SMTP/ComplaintTracker.h).
#define PROPERTY_COMPLAINT_PROCESSING_ENABLED   _T("ComplaintProcessingEnabled")
#define PROPERTY_COMPLAINT_MAILBOX              _T("ComplaintMailbox")
#define PROPERTY_COMPLAINT_THRESHOLD            _T("ComplaintSuppressionThreshold")
#define PROPERTY_COMPLAINT_RETENTION_DAYS       _T("ComplaintRetentionDays")
// The reputation watch (ReputationWatch), in hm_settings: whether the daily check
// runs at all, the block lists it asks, and the addresses an administrator says
// this server is seen as - which is the only way a server behind NAT can name its
// public address, since nothing on the machine knows it.
#define PROPERTY_REPUTATION_WATCH_ENABLED       _T("ReputationWatchEnabled")
#define PROPERTY_REPUTATION_WATCH_LISTS         _T("ReputationWatchLists")
#define PROPERTY_REPUTATION_WATCH_ADDRESSES     _T("ReputationWatchAddresses")
#define PROPERTY_QUOTA_GRACE_PERCENT            _T("QuotaGracePercent")
#define PROPERTY_QUOTA_SEND_STOP_PERCENT        _T("QuotaSendStopPercent")

// 6006 adds hm_imapfolders.folderspecialuse (RFC 6154 SPECIAL-USE).
// 6007 adds hm_domains.domaindkimsecondaryselector and
//      hm_domains.domaindkimsecondaryprivatekeyfile, the staged half of a DKIM key
//      rotation. Both default to empty, which is a domain that has never staged one.
// 6008 adds hm_tcpipports.portclientcertificatepolicy and
//      hm_tcpipports.portclientcertificatecafile, per-port inbound client-certificate
//      policy. Policy 0 is "off", which is every existing port, so an upgraded
//      database behaves exactly as it did.
// 6009 adds hm_sslcertificates.sslprivatekeypassword, the protected passphrase for an
//      encrypted private key file. Empty means the key is not encrypted, which is
//      every existing row, so an upgraded database behaves exactly as it did.
// 6010 seeds hm_settings rows ASArcFilteringEnabled (0) and ASArcTrustedSealers ('').
//      Both are today's behaviour - ARC results are not used for filtering unless an
//      administrator names the sealers to trust. The rows exist rather than being
//      absent-with-a-default because PropertySet reports error 5015 on every read of
//      a missing property, which would fill the error log on ARC-bearing mail.
// 6046 adds hm_senderlist (the reader's, the domain's and the server's allow and
//      block lists, one table with a scope on it), hm_digeststate and
//      hm_digesttokens (when each account last had a quarantine digest, whether it
//      asked to stop, and the one-shot capabilities its links carry),
//      hm_domains.domainquarantinedigest and .domainquarantinedigesthour, and seeds
//      hm_settings rows QuarantineDigestBaseUrl ('') and QuarantineDigestLinkHours
//      (72). The digest is off in every domain until an administrator turns it on.
// 6047 adds hm_roles (the named administrator roles, with serveradmin, domainadmin,
//      helpdesk and auditor seeded as built-in) and hm_roleassignments (which
//      account holds which role, and in which domains). An account with no
//      assignment keeps the authority its AdminLevel has always given it.
// 6048 adds five int columns to hm_domains - domaindkimrotationenabled,
//      domaindkimrotationintervaldays, domaindkimrotationpropagationhours,
//      domaindkimrotationretentiondays and domaindkimrotationkeybits, the per-domain
//      DKIM key rotation policy, off on every existing domain - and hm_dkimrotations,
//      one row per domain whose rotation the server manages, holding the current,
//      next and retiring selectors with their times and DNS check results; and seeds
//      the dkim.rotation.stalled alert rule.
// 6049 adds hm_bouncesuppressions, hm_bouncecounts and hm_bouncereportkeys: the
//      per-domain suppression list, the per-address bounce counts it is fed from,
//      and the register of delivered messages a delivery report is matched against.
//      All three start empty and nothing writes to them until BounceProcessingEnabled
//      is switched on, so an upgraded database behaves exactly as it did.
// 6050 adds hm_signins - one row per client that has signed in to an account, by
//      protocol, address and user agent, with when it was first and last seen, the
//      kind of credential it used and whether it has been revoked - four int columns
//      to hm_domains (domainsessionidleseconds, domainsessionlifetimeseconds,
//      domainsessiononeperdevice and domainsessionbindaddress, the per-domain session
//      policy) and four hm_settings rows (SessionIdleSeconds, SessionLifetimeSeconds,
//      SessionOnePerDevice, SessionBindToAddress). Every one of the eight is 0, which
//      means the policy is not set: an upgraded server keeps the ceilings it always
//      had (thirty minutes idle, twelve hours in all) and refuses nobody it did not
//      refuse before. hm_signins starts empty and fills as people sign in.
// 6050 adds hm_pushsubscriptions, one row per browser or phone an account asked to
//      be told on: the push service's endpoint (and a hash of it, because SQL Server
//      Compact will not compare a long nvarchar parameter with a column), the two
//      keys RFC 8291 encrypts to, what the person allowed a push to carry, which
//      senders and labels it is for, and the quiet hours; and seeds the seven
//      WebPush* settings rows. The table starts empty and WebPushEnabled is 0, so
//      an upgraded database behaves exactly as it did.
// 6050 adds hm_complaints and hm_complaintvolume, and hm_bouncecounts.bccomplaints:
//      the abuse reports the large providers send when a reader marks a message as
//      spam (RFC 5965), one row each, with the per-domain daily tally of messages
//      delivered and complaints received that the complaint rate is computed from,
//      and the complaint count beside each address's bounce counts. It also seeds
//      the complaints.rate.high alert rule. Nothing is written to any of them until
//      ComplaintProcessingEnabled is switched on and a complaint mailbox is named,
//      so an upgraded database behaves exactly as it did.
// 6050 adds hm_mailboxmoves, the journal of a mailbox rename or move: one row per
//      move, naming the account, the address it left, the address it took, how far
//      the move got and whether the old address was kept as an alias. Nothing reads
//      it to decide anything - a move is decided from the account and the domains -
//      so an upgraded database behaves exactly as it did; what it buys is that an
//      interrupted move is visible afterwards instead of being guessed at from the
//      store. The table starts empty and cascades away with its account.
// 6049 adds hm_reputationchecks, the last answer each DNS block list gave about
//      each of the server's outbound addresses (ReputationWatch), seeds the
//      reputation.listed alert rule, and seeds hm_settings rows
//      ReputationWatchEnabled, ReputationWatchLists (four zones) and
//      ReputationWatchAddresses (''). With no bind address, no rule binding one and
//      nothing in ReputationWatchAddresses, the watch has nothing to look up - which
//      is why a new installation is seeded on (1 in the create scripts) and an
//      upgrade is seeded off (0 here): an existing server may already have a bind
//      address, and an upgrade must not start sending it to third parties daily.
// 6049 adds hm_spamtraps (the addresses an administrator declares to be a domain's
//      spam traps, held by local part so that renaming the domain carries them)
//      and hm_spamtraphits (each time a trap was addressed: the address, the
//      connecting address, the envelope sender, whether it was banned and blocked,
//      and why not), and seeds hm_settings rows SpamTrapBanMinutes (1440),
//      SpamTrapBlockDays (30), SpamTrapHitRetentionDays (90) and
//      SpamTrapTeachSpamAssassin (0). No domain has a trap until one is declared.
// 6049 adds the hm_settings row backupencryptionpassphrase ('', 0): the passphrase
//      the backup archive is encrypted with, protected by the platform's secret
//      store, with the integer as "a passphrase is set". Empty on every existing
//      installation, where backups go on being written unencrypted.
#define REQUIRED_DB_VERSION            6063
