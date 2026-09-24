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

// Greylisting, hardened (SMTP/GreyListing.h): the sliding wait on an early retry,
// the number of early retries at which a pending triplet is refused with 550,
// and how long that refusal lasts. Every one of them off or inert by default: 0,
// 0 and 60 minutes, and the minutes mean nothing while the threshold is 0.
#define PROPERTY_GL_SLIDE_ON_EARLY_RETRY        _T("GreyListingSlideOnEarlyRetry")
#define PROPERTY_GL_ABUSE_THRESHOLD             _T("GreyListingAbuseThreshold")
#define PROPERTY_GL_ABUSE_LOCK_MINUTES          _T("GreyListingAbuseLockMinutes")

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
#define PROPERTY_BRAND_LOGOS_ENABLED            _T("BrandLogosEnabled")
#define PROPERTY_READER_FETCH_ACCOUNTS_MAX      _T("ReaderFetchAccountsMax")
#define PROPERTY_READER_FETCH_PRIVATE_ADDRESSES _T("ReaderFetchPrivateHosts")
#define PROPERTY_READER_SENDER_ADDRESSES_MAX    _T("ReaderSenderAddressesMax")
#define PROPERTY_READER_MASKED_ADDRESSES_MAX    _T("ReaderMaskedAddressesMax")
#define PROPERTY_RECOVER_DELETED_DAYS           _T("RecoverDeletedDays")
// A recovery address and one-time recovery codes (Util/AccountRecovery.h):
// whether the webmail offers them at all, and how long each of the two links
// lives - the one that proves a recovery address, and the one that resets a
// password.
#define PROPERTY_ACCOUNT_RECOVERY_ENABLED       _T("AccountRecoveryEnabled")
#define PROPERTY_RECOVERY_RESET_LINK_MINUTES    _T("RecoveryResetLinkMinutes")
#define PROPERTY_RECOVERY_VERIFY_LINK_HOURS     _T("RecoveryVerifyLinkHours")
// The organisation's directory (Util/DirectoryStore.h): whether the accounts and
// distribution lists of a domain are answered to the people of that domain, and
// whether a person may set their own title, telephone numbers, photo and opt-out
// rather than only the administrator.
#define PROPERTY_DIRECTORY_ENABLED              _T("DirectoryEnabled")
#define PROPERTY_DIRECTORY_USER_EDITABLE        _T("DirectoryUserEditable")
// Offline in the webmail (Util/RestApiOffline.cpp): the most days of bodies
// and attachments a reader may keep in their browser; 0 offers none.
#define PROPERTY_READER_OFFLINE_DAYS_MAX        _T("ReaderOfflineDaysMax")
// Messages that expire (Util/ConfidentialMessages.h): the absolute address the
// links in the notices are written against. Empty falls back to the digest's
// and then the import sign-in's base URL; with none set, none can be sent.
#define PROPERTY_CONFIDENTIAL_BASE_URL          _T("ConfidentialBaseUrl")
// Outbound content policy (Util/ContentPolicy.h): the regular expression the
// account_number detector searches the readable text for. Empty, the default,
// is a detector that never fires; a card number and an IBAN need no pattern.
#define PROPERTY_DLP_ACCOUNT_NUMBER_PATTERN     _T("DlpAccountNumberPattern")
// The sample domain (Application/SampleDomain.h): the database id of the
// example.test the sample made, or 0 when it made none. The marker the removal
// matches against, so that an example.test somebody made by hand is never the
// one it removes, and the checklist leaves the sample out of its count.
#define PROPERTY_SAMPLE_DOMAIN_ID               _T("SampleDomainId")
#define PROPERTY_DOMAIN_REPORT_RETENTION_DAYS   _T("DomainReportRetentionDays")
// Compromised-account detection (Util/CompromiseDetector.h): what a takeover
// shape does (0 off, 1 watch, 2 enforce), the baseline window, the history an
// account needs before it has a baseline, the four signals' thresholds, the
// score that locks, and how long a held message waits for a review.
#define PROPERTY_COMPROMISE_DETECTION_MODE      _T("CompromiseDetectionMode")
#define PROPERTY_COMPROMISE_BASELINE_DAYS       _T("CompromiseBaselineDays")
#define PROPERTY_COMPROMISE_MINIMUM_HISTORY     _T("CompromiseMinimumHistory")
#define PROPERTY_COMPROMISE_VOLUME_FACTOR       _T("CompromiseVolumeFactor")
#define PROPERTY_COMPROMISE_VOLUME_FLOOR        _T("CompromiseVolumeFloor")
#define PROPERTY_COMPROMISE_RECIPIENT_FLOOR     _T("CompromiseRecipientFloor")
#define PROPERTY_COMPROMISE_NEW_RECIPIENT_SHARE _T("CompromiseNewRecipientShare")
#define PROPERTY_COMPROMISE_SCORE_TO_LOCK       _T("CompromiseScoreToLock")
#define PROPERTY_COMPROMISE_HELD_DAYS           _T("CompromiseHeldDays")
// Importing from the old provider with a sign-in (Util/ReaderOAuth.h): the base
// URL the providers send the reader back to, and per provider the client this
// server is registered as and the two endpoints of the authorization code flow.
#define PROPERTY_IMPORT_OAUTH_REDIRECT_BASE_URL          _T("ImportOAuthRedirectBaseUrl")
#define PROPERTY_IMPORT_OAUTH_GOOGLE_CLIENT_ID           _T("ImportOAuthGoogleClientId")
#define PROPERTY_IMPORT_OAUTH_GOOGLE_CLIENT_SECRET       _T("ImportOAuthGoogleClientSecret")
#define PROPERTY_IMPORT_OAUTH_GOOGLE_AUTHORIZE_URL       _T("ImportOAuthGoogleAuthorizeUrl")
#define PROPERTY_IMPORT_OAUTH_GOOGLE_TOKEN_URL           _T("ImportOAuthGoogleTokenUrl")
#define PROPERTY_IMPORT_OAUTH_MICROSOFT_CLIENT_ID        _T("ImportOAuthMsClientId")
#define PROPERTY_IMPORT_OAUTH_MICROSOFT_CLIENT_SECRET    _T("ImportOAuthMsClientSecret")
#define PROPERTY_IMPORT_OAUTH_MICROSOFT_AUTHORIZE_URL    _T("ImportOAuthMsAuthorizeUrl")
#define PROPERTY_IMPORT_OAUTH_MICROSOFT_TOKEN_URL        _T("ImportOAuthMsTokenUrl")

// Passkeys and single sign-on (Util/WebAuthn.h, Util/PasskeyStore.h,
// Util/OidcSignIn.h). Passkeys are offered unless the switch is off; nothing
// changes for an installation until somebody enrols one. The relying party id
// is the host the page was served on unless one is named here, which is what a
// server reached at several names needs so that a passkey made at one of them
// works at the others. OIDC is off until an issuer is set: the client id and
// secret this server is registered with at the identity provider, the scopes
// asked for, whether an unknown but verified address may have an account made
// for it, and the addresses that sign in as the administrator.
#define PROPERTY_PASSKEYS_ENABLED               _T("PasskeysEnabled")
#define PROPERTY_PASSKEY_RELYING_PARTY          _T("PasskeyRelyingParty")
#define PROPERTY_OIDC_ISSUER                    _T("OidcIssuer")
#define PROPERTY_OIDC_CLIENT_ID                 _T("OidcClientId")
#define PROPERTY_OIDC_CLIENT_SECRET             _T("OidcClientSecret")
#define PROPERTY_OIDC_SCOPES                    _T("OidcScopes")
#define PROPERTY_OIDC_AUTO_PROVISION            _T("OidcAutoProvision")
#define PROPERTY_OIDC_ADMINISTRATORS            _T("OidcAdministrators")
#define PROPERTY_OIDC_REDIRECT_BASE_URL         _T("OidcRedirectBaseUrl")

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
// OpenPGP in the webmail (docs/OpenPGP.md). OpenPGPPortalEnabled decides whether
// the page offers any of it at all - it is ON, because a key nobody made costs
// nothing and an installation that wants none turns it off in one place.
// The two lookups are how a correspondent's key is FOUND, and they are separate
// because they leak differently: a Web Key Directory lookup tells the
// correspondent's own domain that somebody is about to write to them, which
// their mail server would learn a second later anyway, so it is ON; a keyserver
// lookup tells keys.openpgp.org, which is nobody's mail server, so it is OFF
// until an administrator decides otherwise. The timeout bounds the fetch a REST
// worker makes while a page waits on it.
#define PROPERTY_OPENPGP_PORTAL_ENABLED         _T("OpenPGPPortalEnabled")
#define PROPERTY_OPENPGP_LOOKUP_WKD             _T("OpenPGPLookupWkd")
#define PROPERTY_OPENPGP_LOOKUP_KEYSERVER       _T("OpenPGPLookupKeyserver")
#define PROPERTY_OPENPGP_KEYSERVER_URL          _T("OpenPGPKeyserverUrl")
#define PROPERTY_OPENPGP_LOOKUP_TIMEOUT         _T("OpenPGPLookupTimeout")
// How many days a password-protected message (Util/ProtectedMessages.h) is
// kept for its reading page before the ciphertext goes: the default a sender
// gets, and the most a sender may ask for. 1 to 365, 30 by default.
#define PROPERTY_PROTECTED_MESSAGE_DAYS         _T("ProtectedMessageDays")
// Message recall within the server (Util/MessageRecall.h, docs/MessageRecall.md):
// how many days after a message was sent from one of this server's accounts
// to another its sender may still withdraw the unread copies, and how long
// the rows that make that possible are kept. 1 to 365, 14 by default.
#define PROPERTY_RECALL_DAYS                    _T("RecallDays")
// The list manager (Util/ListManager.h, docs/MailingLists.md). The absolute
// https base the links in list mail are written against - the confirmation
// page, the member's own page with its RFC 8058 one-click unsubscribe, the
// archive - and empty, the default, for none: a list then offers its mailto
// addresses alone, because a link that leads nowhere is worse than no link.
// And the bounce rules: how many hard bounces (1 by default) or soft bounces
// within how many days (5 within 30) stop a member being sent the list's mail.
#define PROPERTY_LIST_BASE_URL                  _T("ListBaseUrl")
#define PROPERTY_LIST_HARD_BOUNCES              _T("ListHardBouncesToUnsubscribe")
#define PROPERTY_LIST_SOFT_BOUNCES              _T("ListSoftBouncesToUnsubscribe")
#define PROPERTY_LIST_BOUNCE_WINDOW_DAYS        _T("ListBounceWindowDays")
// The organisation CA for S/MIME (Util/OrganisationCa.h, docs/SmimeAuthority.md):
// where the web services listener is reached from the clients, such as
// http://pki.example.com, under which every certificate a domain's authority
// issues names its CRL (/smime-ca/<domain>/<root>.crl) and its root
// (.cer). Empty, the default, writes neither extension into a certificate.
#define PROPERTY_SMIME_AUTHORITY_BASE_URL       _T("SmimeAuthorityBaseUrl")
// Outbound shaping (SMTP/DestinationBackoff.h, docs/OutboundShaping.md). The
// back-off a destination's slow-down reply opens: the first window in minutes
// (0 turns it off; 2 by default) and the most it doubles to (60). And the two
// connection ceilings, both off at 0: how many sessions this server holds open
// to one destination whose remote domain policy sets none, and how many
// deliveries one tenant - the hosted domain of the envelope sender, or
// "(external)" for relayed mail - may have in flight at once. The last two row
// names are shorter than their COM properties
// (MaxOutboundConnectionsPerDestination, MaxOutboundConnectionsPerTenant),
// because hm_settings.settingname holds thirty characters.
#define PROPERTY_OUTBOUND_BACKOFF_MINUTES       _T("OutboundBackoffMinutes")
#define PROPERTY_OUTBOUND_BACKOFF_MAX_MINUTES   _T("OutboundBackoffMaxMinutes")
#define PROPERTY_MAX_OUTBOUND_CONNS_PER_DEST    _T("MaxOutboundConnsPerDestination")
#define PROPERTY_MAX_OUTBOUND_CONNS_PER_TENANT  _T("MaxOutboundConnsPerTenant")
// Attachments opened somewhere safe (AntiVirus/Sandbox.h, docs/Sandbox.md). Off
// until an administrator names a sandbox: the switch, the two addresses of the
// HTTP service (https, or plain http to this machine), the bearer token -
// stored protected, never emitted - how long a message waits for a verdict (15
// minutes) and what happens when it stops waiting or the sandbox answers error
// (0 delivered with a header, 1 returned to the sender), the largest attachment
// submitted (20480 KB), the extensions and media types that are submitted, how
// often the verdicts are asked for (30 seconds), the score at which suspicious
// counts as malicious (70), how long a verdict is remembered (24 hours),
// whether what this server's own signed-in users submit is held too (no), and
// the most files one message may send to the sandbox (10).
#define PROPERTY_SANDBOX_ENABLED                _T("SandboxEnabled")
#define PROPERTY_SANDBOX_SUBMIT_URL             _T("SandboxSubmitUrl")
#define PROPERTY_SANDBOX_VERDICT_URL            _T("SandboxVerdictUrl")
#define PROPERTY_SANDBOX_TOKEN                  _T("SandboxToken")
#define PROPERTY_SANDBOX_TIMEOUT_MINUTES        _T("SandboxTimeoutMinutes")
#define PROPERTY_SANDBOX_TIMEOUT_ACTION         _T("SandboxTimeoutAction")
#define PROPERTY_SANDBOX_MAX_SIZE_KB            _T("SandboxMaxSizeKB")
#define PROPERTY_SANDBOX_TYPES                  _T("SandboxTypes")
#define PROPERTY_SANDBOX_POLL_SECONDS           _T("SandboxPollSeconds")
#define PROPERTY_SANDBOX_SCORE_THRESHOLD        _T("SandboxScoreThreshold")
#define PROPERTY_SANDBOX_CACHE_HOURS            _T("SandboxCacheHours")
#define PROPERTY_SANDBOX_INTERNAL_MAIL          _T("SandboxInternalMail")
#define PROPERTY_SANDBOX_MAX_FILES              _T("SandboxMaxFiles")
// Zero-access mailboxes (docs/ZeroAccess.md). OFF: an administrator has to
// allow the opt-in before any account can make it, because a mailbox with this
// on is one the administrator cannot read, search, export or recover, and a
// person who loses the password and the recovery key has lost the mail - a
// consequence an installation should choose with its eyes open. Allowing it
// switches nobody on; each person does that from the Security page.
#define PROPERTY_ZERO_ACCESS_ALLOWED            _T("ZeroAccessAllowed")
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
// Migration jobs (Application/MigrationManager.h): the most connections every
// running job of every migration together holds to one source host. A
// provider counts concurrent IMAP sessions per user and per client address,
// and five hundred mailboxes started at once from one address is the shape
// that gets an address throttled for the rest of the day.
#define PROPERTY_MIGRATION_CONNECTIONS_PER_HOST _T("MigrationConnectionsPerHost")
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

// SMTP policy the score cannot express (discussions #283, #285, #286), in
// hm_settings with rows created by EnsureSettingDefaults_. Every default is the
// server as it was: SPF's SoftFail, TempError and PermError add nothing, a
// TempError is accepted and scored rather than deferred, and a null sender with
// several recipients is accepted. The names are the operator's own, shortened
// only where hm_settings.settingname (nvarchar(30)) demands it.
#define PROPERTY_USE_SPF_SOFTFAIL_SCORE         _T("UseSPFSoftFailScore")
#define PROPERTY_USE_SPF_TEMPERROR_SCORE        _T("UseSPFTempErrorScore")
#define PROPERTY_USE_SPF_PERMERROR_SCORE        _T("UseSPFPermErrorScore")
#define PROPERTY_SPF_TEMPERROR_DEFER            _T("SPFTempErrorDefer")
#define PROPERTY_REJECT_MULTI_RCPT_NULL_SENDER  _T("RejectMultiRcptNullSender")
#define PROPERTY_BAN_MULTI_RCPT_NULL_SENDER     _T("BanMultiRcptNullSender")
#define PROPERTY_BAN_MULTI_RCPT_NULL_SENDER_HOURS _T("BanMultiRcptNullSenderHours")

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
// The wiki the three help panes offer "Open on the wiki" against, with the
// document's name as the page (Common/Util/HelpPages.h): empty hides the
// link. The Deck and the webmail read it from GET /api/v1/help, the Control
// Panel over COM. An hm_settings row, seeded by EnsureSettingDefaults_.
#define PROPERTY_HELP_WIKI_BASE_URL             _T("HelpWikiBaseUrl")
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
// 6070 adds hm_domains.domainreportmailbox (the address a domain's DMARC and
//      TLS-RPT aggregate reports are posted to, '' until an administrator names
//      one), hm_dmarcreports and hm_dmarcreportrecords (one row per aggregate
//      report read from that mailbox and one per <record> of it) and hm_tlsreports
//      (one row per TLS-RPT policy result), and seeds the hm_settings row
//      DomainReportRetentionDays (365). Nothing is read until a domain names a
//      mailbox, so an upgraded database behaves exactly as it did.
// 6071 adds hm_accountactivity, hm_accountrecipients, hm_accountreview and
//      hm_heldmessages (Util/CompromiseDetector.h): the per-account baseline of
//      authenticated submissions, the memory of whom an account has written to,
//      the account's review state (exempt, sensitivity, the compromise lock and
//      the last takeover shape seen) and the messages kept out of the delivery
//      queue for review, which are hm_messages rows of messagetype 4; and seeds
//      the account.compromised alert rule, on.
// 6072 hardens greylisting (SMTP/GreyListing.h). hm_greylisting_triplets gains
//      glsenderhash and glrecipienthash, the fixed-width key of the triplet (the
//      MD5 of the normalised address, as hex), with the address columns widened
//      to 320 and the index moved from the addresses to the hashes; rows from
//      before this step have a NULL hash and are matched by their addresses once
//      and rewritten. hm_greylisting_bypass is the per-domain bypass list: a
//      pattern, what it is matched against and the authentication results it
//      must carry. Seeds GreyListingSlideOnEarlyRetry (0), GreyListingAbuseThreshold
//      (0) and GreyListingAbuseLockMinutes (60); with the first two 0 and the list
//      empty, greylisting is what it was.
// 6073 adds hm_domainpolicylist (AntiSpam/DomainPolicyList.h): the domain white
//      and black list of discussion #285, keyed by envelope domain or HELO suffix
//      rather than by address - pattern, what it matches on, white or black, and
//      a note. The table starts empty, and an empty list changes nothing. The
//      seven hm_settings rows of the same row (UseSPFSoftFailScore,
//      UseSPFTempErrorScore, UseSPFPermErrorScore, SPFTempErrorDefer,
//      RejectMultiRcptNullSender, BanMultiRcptNullSender,
//      BanMultiRcptNullSenderHours) are created at start by
//      EnsureSettingDefaults_, every one at the value that leaves the server as
//      it was.
// 6074 adds hm_distributionlists.distributionlistmembership
//      (BO/DistributionList.h): the text that makes a list define its own
//      members instead of keeping a table of them - empty, which is a static
//      list; domain; domain:<name>; pattern:<glob>; everyone. Empty by default,
//      so every list an upgraded database holds keeps exactly the members it
//      had.
// 6075 adds hm_journalrules (Util/Journaling.h): one row per journaling or
//      dual-delivery rule - whose mail (a domain, or one account in it), which
//      way it is going, where the copy goes, and whether the copy is an
//      envelope report with the original attached or the original itself.
//      Nothing is journaled until a row exists, so an upgraded database
//      behaves exactly as it did.
// 6076 adds hm_accountrecovery, hm_recoverytokens and hm_recoverycodes
//      (Util/AccountRecovery.h): an account's recovery address and whether it
//      has been proved, the one-shot tokens a proving or a reset link carries,
//      and the ten recovery codes that stand in for a lost authenticator. No
//      row exists until the person sets an address or makes codes, so an
//      upgraded database can recover exactly what it could before: nothing.
// 6077 adds hm_passkeys (Util/PasskeyStore.h): one row per WebAuthn credential
//      an account or the administrator registered - the credential id and its
//      public key, the signature counter, what the device is called and when it
//      last signed anybody in. Nothing is registered until somebody registers
//      it, so an upgraded database signs in exactly as it did.
// 6078 adds hm_directory (Util/DirectoryStore.h): one row per account that has a
//      directory entry - a title, a department, three telephone numbers, a photo
//      as base64 with its media type, the administrator's note, and the opt-out
//      the person themselves sets. Every column is empty or off, and an account
//      with no row is listed from hm_accounts alone, so an upgraded database
//      lists exactly the names and addresses it already held.
// 6079 adds hm_addressbooks and hm_addressbookshares (Util/AddressBookStore.h):
//      an account may keep several address books, and share one of them with
//      another account or with a whole domain, read-only or writable. hm_contacts
//      gains contactbookid, which is 0 on every row an upgrade finds - meaning
//      "the account's default book" - and is stamped with that book's id the
//      first time anything asks the account for its books, so the upgrade needs
//      no data migration and an untouched installation behaves exactly as it did.
// 6080 adds hm_pgpring (Util/OpenPgpRing.h): one row per OpenPGP key an account
//      keeps in the webmail - its own secret key, encrypted under a passphrase
//      this server never sees, and the public keys of the people it writes to -
//      with a ceiling of eight megabytes per account. Nothing is there until
//      somebody makes or imports a key, so an upgraded database signs and
//      encrypts exactly as it did: not at all, until asked.
// 6081 adds five columns to hm_pgpring (Util/OpenPgpRing.h), so that a change of
//      a correspondent's key is a warning and never a silent switch: pgrverified
//      and pgrverifiedat, the account's own "verified in person" mark on a key
//      and when it was set; pgrchangedfrom and pgrchangedat, the fingerprint a
//      key REPLACED for the same address and when it arrived, kept so that the
//      reader can be told "it was this; it is now this"; and pgraccepted,
//      whether the person accepted that change. Every row an upgrade finds is
//      unverified, replaced nothing and needs no accepting, so an upgraded ring
//      encrypts to exactly the keys it did and shows no mark it did not earn.
// 6082 adds hm_protectedmessages (Util/ProtectedMessages.h): one row per
//      password-protected message an account sent - the token its link names,
//      the recipient, the hint, the file the ciphertext is in, its size and
//      how much of it has arrived, when it expires, when it was first fetched,
//      whether it was withdrawn, and the original's token when the row is a
//      reply. The plaintext is never in a row or a file: the page encrypts it
//      to a passphrase before this server sees it. The step also seeds
//      ProtectedMessageDays (30). Nothing is there until somebody sends one,
//      so an upgraded database behaves exactly as it did. The step is written
//      6080 to 6082 in this tree, whose terminus is 6080; the integrator
//      renumbers it 6081 to 6082 when the row holding 6081 lands first.
// 6083 adds accountzeroaccess to hm_accounts and the table hm_zeroaccess
//      (Util/ZeroAccess.h): a zero-access mailbox has every delivered message
//      encrypted, after the filters have run, to the account's own OpenPGP key,
//      and the table holds the two copies of that key's secret half this server
//      cannot open - one under a key the browser derives from the password with
//      Argon2id (the salt and the three parameters beside it), one under the
//      recovery key the person wrote down - and which key of the ring it is.
//      Every account an upgrade finds has the switch off and no row, and the
//      server-wide ZeroAccessAllowed setting the step seeds is off, so an
//      upgraded server delivers exactly as it did until an administrator allows
//      the opt-in and a person makes it.
// 6084 adds hm_domains.domainrestricteddomains and
//      hm_accounts.accountrestricteddelivery (Util/RestrictedDelivery.h):
//      restricted delivery, Google Workspace's "restrict delivery". The
//      domain's column is the list of domains its accounts may exchange mail
//      with - at most 2,000 characters of ASCII names separated by ", ", each
//      partner.example or .example for the domain and everything beneath it -
//      and the account's is 0 to follow the domain, 1 to be restricted even
//      when the domain names nothing (its own domain alone), 2 to be exempt.
//      Both are enforced at RCPT TO in both directions and at local delivery
//      for a recipient reached by expansion, a forward or an auto-reply. Every
//      domain an upgrade finds names nothing and every account follows it, so
//      an upgraded server delivers exactly as it did.
// 6085 adds hm_recall (Util/MessageRecall.h): message recall within the
//      server. One row per local recipient of a message one of this server's
//      own accounts submitted - over authenticated SMTP or from the webmail -
//      holding the sender's account id, the message's Message-ID without its
//      angle brackets, the recipient's account id, the queue row the message
//      was accepted as, the recipient's hm_messages row once delivered (0
//      until then), when it was recorded, and when and how it was recalled.
//      POST /api/v1/me/recall deletes the copies that are still unread. The
//      step also seeds RecallDays (14): rows older than that are swept and a
//      recall of an older message answers 404. Nothing is recorded for mail
//      from outside or for an unauthenticated submission, and nothing is
//      there until an account sends, so an upgraded server behaves exactly
//      as it did.
// 6086 adds hm_calendarshares.sharegroupid and hm_calendarshares.sharedelegate
//      (Util/CalendarStore.h): a calendar shared with one of the server's
//      groups rather than with a named account - the row names the group and
//      shareaccountid is 0; a member of the group sees the calendar as a named
//      sharee does, resolved at read time from hm_group_members - and a share
//      that is a delegation, whose sharee may answer the owner's invitations
//      on the owner's behalf (POST /api/v1/me/calendar/events/{id}/reply). The
//      unique index is widened to (calendar, account, group). There is no
//      foreign key to hm_groups, because 0 is not a group: the group's own
//      removal deletes its share rows and the owner's listing drops a row
//      whose group is gone. Every row an upgrade finds names no group and is
//      no delegation, so an upgraded server shares exactly as it did.
// 6087 adds hm_domains.domainimpersonationaction, domainprotectedpersons and
//      domainprotectedpartners (Util/ImpersonationCheck.h): impersonation
//      protection, Microsoft 365's anti-phishing policies, per domain. The
//      action is 0 off, 1 flag (X-hMailServer-Impersonation on the recipient's
//      copy, which the webmail shows as a warning) or 2 quarantine (the copy
//      held through the spam quarantine, released as spam is, without a second
//      check); the persons are names and addresses outside the domain's
//      accounts whose name must not be borrowed, one per line, at most 2,000
//      characters; the partners are lower-cased ASCII domain names whose
//      lookalikes are judged as the hosted domains' are, at most 2,000
//      characters. Both checks run at local delivery on a copy from outside
//      only - the tagger's own decision, shared. Every domain an upgrade finds
//      has the action off and both lists empty, so an upgraded server delivers
//      exactly as it did.
// 6088 adds hm_settingoverrides (Application/EffectiveSetting.h,
//      docs/SettingLevels.md): one value, three levels. A row is a domain's
//      (soscope 1) or an account's (soscope 2) own value for one setting that
//      may be set below the server - soownerid the domain or account id,
//      soname the setting's name as REST, COM and hmctl spell it, sovalue its
//      text - unique on (scope, owner, name), so a level holds at most one
//      value per name and no row means the level inherits; a second index on
//      soname serves the reads of one name across every owner (the digest
//      pass, the OpenPGP portal's switch, the sweeps). The dedicated
//      domain and account columns stay where they are and remain the storage
//      of the levels they already held; the overlay holds only the levels
//      that had nowhere to live. There is no foreign key, because one column
//      names a domain or an account by the scope beside it: the rows go with
//      their domain or account in PersistentDomain::DeleteObject and
//      PersistentAccount::DeleteObject, as the sender lists' do. An upgrade
//      finds the table empty, and every setting resolves exactly as it did.
// 6089 adds hm_apisends (Util/ApiSends.h, docs/SendApi.md): the send API's
//      record of what each API key sent through POST /api/v1/send, so that
//      the key - and only that key - can ask afterwards what became of it.
//      apskeyid is the key's id in the key store (hMailServerApiKeys.ini, not
//      a table, so there is no foreign key: a revoked key's rows stay until
//      they age out and answer nobody), apsqueueid the queue id the message
//      was saved under - the message trace's correlation key - apsmessageid
//      its Message-ID, apssender the envelope sender, apsrecipients the
//      addresses the body named (to, cc and bcc, comma-separated, as many
//      whole addresses as 4,000 characters hold) and apscreated when it was
//      queued. Indexed on (key, id), which is the listing's order, on (key,
//      time), on the queue id, which is one send's lookup, and on the time,
//      which is the sweep's. Kept MessageTraceRetentionDays and then removed by
//      the hourly pass of RemoveExpiredRecords (0 keeps them), with a key's
//      rows removed when the key is revoked. The step also indexes
//      hm_messagetrace on mtqueueid, which no step had: one message's events
//      are what the send API reads for every send it describes, and the trace
//      route's queue lookup reads them too. And it adds send_through_api to
//      the serveradmin row's permission column, which is kept in step for
//      anybody reading the table. An upgrade finds the table empty, and
//      nothing else changes.

// 6090 adds hm_migrations, hm_migrationjobs and hm_migrationfolders
//      (Application/MigrationManager.h, docs/Migration.md "Five hundred
//      mailboxes: migration jobs") and the hm_settings row
//      MigrationConnectionsPerHost (4). A migration is a batch of jobs, one
//      per mailbox: the source (kind 0 an IMAP server, 1 Google Workspace, 2
//      Microsoft 365; its host, port, security, OAuth provider and DAV URL),
//      how many of its jobs run at once and how many messages a minute each
//      may fetch, and its state (0 new, 1 running, 2 paused, 3 finished). A
//      job is one source mailbox copied into one account here: the source
//      user, the password or the OAuth refresh token (protected at rest as a
//      fetch account's is; mjsigninhash holds the SHA-256 of a per-job sign-in
//      link's key while an OAuth job waits for its owner), the target account
//      id, the state (0 queued, 1 running, 2 paused, 3 done, 4 failed, 5
//      cancelled), the phase (0 mail, 1 contacts, 2 calendars, 3 report), the
//      counters, the last error, the attempts and when the next is due, and the
//      report as JSON. A folder row is the resume state of one source folder:
//      its UIDVALIDITY and the highest UID dealt with, written after every 50
//      messages and at the folder's end, so that an interrupted job goes on
//      from its mark. The jobs cascade from their migration and the folders
//      from their job; a job names its target account without a foreign key,
//      because deleting an account must not delete the record of the migration
//      that filled it - deleting the account cancels its unfinished jobs, with
//      the reason (PersistentAccount::DeleteObject). An
//      upgrade finds the three tables empty and nothing runs until a migration
//      is created and started.

// 6091 adds hm_sandboxverdicts and hm_sandboxholds (AntiVirus/Sandbox.h,
//      docs/Sandbox.md): attachments opened somewhere safe. A verdict row is
//      one file's answer from the sandbox, by SHA-256 (unique): the verdict
//      (clean or malicious - a suspicious answer is stored as whichever the
//      score threshold made it, and an error is never stored), the score, the
//      detail, when it was answered and when it stops being believed
//      (SandboxCacheHours). A hold row is one submitted file of a message held
//      for a verdict (hm_messages.messagetype 5, Message::HeldForSandbox): the
//      queue row, the file's SHA-256, name and size, the id the sandbox's
//      submit answered (up to 1000 characters), when the hold began, how far
//      the file has got (to submit, submitted, answered), its verdict, score
//      and detail, how many requests for it failed, and the message's
//      decision - pending until the poller or an administrator decides, the
//      same on every row of one message. Neither table has a foreign key: a
//      hold row goes with its message (the deliverer's Forget, an
//      administrator's discard, and the poller for a row whose message is
//      gone), and a verdict row expires. The step also seeds the thirteen
//      Sandbox* settings, with the switch off, so an upgraded server holds
//      nothing until an administrator names a sandbox and turns it on.

// 6092 is the list manager (Util/ListManager.h, docs/MailingLists.md): a
//      distribution list with its manager switched on answers its own
//      addresses (name-subscribe, -unsubscribe, -request, -owner, -bounces and
//      the VERP form name-bounces+local=domain), sends every member a copy of
//      its own with the RFC 2369 and RFC 8058 headers filled in, keeps an
//      archive, sends a daily digest and counts its members' bounces. It adds
//      to hm_distributionlists the switch (distributionlistmanaged), who may
//      join (distributionlistsubscribe: 0 closed, 1 confirm by mail, 2 the
//      owner approves), the archive (0 none, 1 members, 2 public) and its
//      retention in days, the digest switch and hour and where the last one
//      stopped (distributionlistdigestlastid, distributionlistdigestlasttime),
//      whether replies go to the list, the subject prefix (64 characters),
//      the footer (2,000), whether the bounce address encodes the member
//      (VERP), the owner, and the list's own secret, from which the links in a
//      membership query member's copies are made (distributionlistsecret); to
//      hm_distributionlistsrecipients each member's delivery (0 each message,
//      1 the digest, 2 none), when they joined, their state (1 a member; 0
//      awaiting their own confirmation, 2 awaiting the owner's, 3 an address
//      the membership query answers that has left), their secret token, and
//      their hard and soft bounces with when the soft count started, the last
//      bounce and the reason delivery was stopped; the table hm_listarchive,
//      one row per distributed message with its file under the data
//      directory's Lists folder, its month and whether it was kept for the
//      archive or only for the digest; and the seven pairs of translatable
//      texts the manager answers with (LIST_CONFIRM, LIST_REFUSED, LIST_HELP,
//      LIST_APPROVE, LIST_BOUNCED, LIST_LEAVE, LIST_UNSUBSCRIBED and their
//      subjects). Every list an upgrade finds is unmanaged and every member
//      confirmed, receiving each message, so an upgraded server distributes
//      exactly as it did.

// 6093 adds hm_warmup and hm_warmupcounts (SMTP/IpWarmup.h,
//      docs/IpWarmup.md): the IP warm-up. A hm_warmup row is a schedule for
//      one outbound address - warmupaddress, the local IPv4 or IPv6 address a
//      delivery binds, written as IPAddress::ToString writes it and unique;
//      warmupstarted, the UTC day number (seconds since the epoch divided by
//      86,400) of its day 1; warmupplan, 0 standard, 1 gentle, 2 fast; and
//      warmupenabled. A hm_warmupcounts row is one schedule's count for one
//      UTC day and one provider (google, microsoft, yahoo, apple, proton or
//      other): wcsent, the recipients delivered from the address that day,
//      wcdeferred, the recipients held over the day's ceiling, once per
//      pass, and wcceiling and wcplanday, the ceiling and the warm-up day in
//      force when the row was last written, so that the history says what
//      applied then after a plan or a start has changed; unique on
//      (schedule, day, provider), and removed with its schedule by the
//      foreign key. Nothing is counted until an administrator makes a
//      schedule, so an upgraded server delivers exactly as it did.

// 6094 adds hm_remotedomainpolicies.policyencrypt, policyencryptkey and
//      policyencryptfallback (Util/GatewayEncryption.h, docs/RemoteDomains.md
//      "Gateway encryption"): mail to a partner domain encrypted at this
//      server to the partner's key, whatever the sender's client did. The
//      method is 0 none, 1 OpenPGP (PGP/MIME to the partner's public key) or
//      2 S/MIME (enveloped-data to the partner's certificate); the key is the
//      partner's armoured OpenPGP public key or PEM certificates - one gateway
//      key per partner domain, never one per recipient address - kept in the
//      clear, since it is public, and never answered back by REST or COM,
//      which answer its fingerprint, expiry and subject instead; the fallback
//      is 0 refuse (the recipients fail 5.7.10 and the sender is told) or 1
//      send unencrypted and log it. The key is judged when it is saved and
//      again at every delivery. Every policy an upgrade finds encrypts
//      nothing, holds no key and refuses, so an upgraded server delivers
//      exactly as it did.

// 6095 adds hm_smimeca and hm_smimeissued, and hm_smimekeys.smimeauthority
//      (Util/OrganisationCa.h, docs/SmimeAuthority.md): an organisation CA for
//      S/MIME, one per domain. A hm_smimeca row is one of a domain's roots -
//      cadomainid (the domain; the row goes with it by the foreign key),
//      cacertificate (the self-signed root, base64 DER), caprivatekey (its
//      private key, PKCS#8 protected by Crypt::ProtectSecret - an envelope
//      only the machine that wrote it opens, and a column the audit trail
//      withholds by its name), cacreated, caretired (0 for the domain's one
//      current root; the time it was replaced for a root a rotation kept for
//      the certificates it signed) and caserial (the next serial number,
//      carried on to the new root by a rotation, so that a serial names one
//      certificate of the domain). A hm_smimeissued row is one certificate a
//      root signed for an account's own key, which the webmail made and this
//      server never saw: issuedcaid (the root; the row goes with it),
//      issuedaccountid (no foreign key: a deleted account's certificates are
//      revoked, the account id cleared, and stay on the CRL), issuedaddress,
//      issuedserial (unique per root), issuedfingerprint (SHA-256 of the DER,
//      64 lowercase hex digits), issuednotbefore, issuednotafter,
//      issuedrevoked (0, or when it was revoked), issuedreason (superseded,
//      keyCompromise or cessationOfOperation) and issuedcertificate (base64
//      DER). smimeauthority marks a stored certificate that this server's
//      authority issued - set by the server from hm_smimeissued, never taken
//      from the page. Nothing is issued until an administrator creates a
//      domain's authority, so an upgraded server does exactly what it did.
#define REQUIRED_DB_VERSION            6095
