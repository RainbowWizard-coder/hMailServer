// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// REST administration API over HTTPS.
//
// Disabled by default. Enabled with RestApiPort in hMailServer.ini.
// Security model:
//   - Two credentials are accepted:
//       * Authorization: Bearer <api key> - a scoped, expiring API key. This is
//         the preferred form and is tried first when present.
//       * HTTP Basic authentication against the hMailServer administrator
//         password (same credential as the COM API / hMailAdmin). Retained
//         because every existing script uses it. It is the full-authority
//         credential: nothing below narrows it.
//   - API key management (/api/v1/apikeys) deliberately requires the
//     administrator password: a key must not be able to mint or revoke keys,
//     otherwise a narrowly scoped key trivially escalates to an unscoped one.
//   - TLS is mandatory unless the listener is bound to loopback (127.0.0.1
//     or ::1).
//   - An empty administrator password disables the API entirely.
//
// What "scoped" means, and what it did not mean before August 2026: a key
// carried a label, an expiry and an optional source restriction, and that was
// all. Every key was otherwise the administrator password with a different
// spelling - a key minted for a monitoring probe could DELETE any account in
// any domain and drop any message from the delivery queue. Two restrictions
// now carry that weight, and both are enforced in one place (Authorize_, over
// a Route decided by ParseRoute_) rather than at each endpoint:
//
//   - Scope=readonly|full. readonly keys may only read; every route that
//     changes something is refused with 403. This is the default: a create
//     request that does not name a scope gets a read-only key, because the
//     opposite default hands out full authority to anyone who did not read
//     the documentation.
//   - Domains=<comma-separated list>. Empty means every domain. A key with a
//     list may only act on those domains, which is what stops the attack the
//     path shape invites: changing the address in
//     DELETE /api/v1/accounts/<address> to name a mailbox in someone else's
//     domain. Such a key is also refused the delivery-queue routes outright,
//     because a queued message carries recipients in any number of domains
//     and there is no honest way to narrow it.
//
// Both restrictions fail closed when the store is hand-edited: an absent or
// unrecognised Scope leaves the key read-only, and a Domains entry that is not
// a real domain name simply matches nothing. (An entirely empty Domains value
// means every domain, exactly as an empty AllowedFrom means any source and as a
// section with no Domains line at all already did.)
//
// Rate: requests are counted per credential (see IsWithinRequestRate_), not
// per source address, so one leaked key cannot spend the listener's whole
// capacity by rotating source addresses.
//
// API key store: <directory of hMailServer.ini>\hMailServerApiKeys.ini, read
// on every authentication attempt so that adding or revoking a key takes
// effect immediately with no restart and no rebuild. Only the SHA-256 of a key
// is ever stored; the clear-text token exists once, in the 201 response that
// created it. See the comment above LoadKeys_ in the .cpp for the file format
// and for why SHA-256 (and not Argon2id) is the right primitive here.

#pragma once

#include <memory>
#include <vector>
#include <map>
#include <atomic>

#include "HttpServer.h"
#include "ZeroAccess.h"
#include "../Application/RoleManager.h"
#include "AuditTrail.h"
#include "PushSubscriptions.h"
#include "../Tracking/ChangeNotification.h"

namespace HM
{
   struct ContactRecord;
   struct ContactGroup;
   struct DirectoryEntry;
   struct DirectoryRecord;
   class HttpCompressedCache;

   // The cache RestApiServer.cpp holds the compressed forms of its per-request
   // documents in - the webmail's page per language, the catalogues, the
   // OpenAPI description - for the handlers in the other RestApi*.cpp files
   // that serve one of them. See HttpCompression.h.
   HttpCompressedCache &PortalPageCache();

   class MimeHeader;
   class JsonValue;
   struct ICalComponent;
   struct CalendarRecord;
   struct CalendarObjectRecord;

   class IPAddress;
   class Account;
   class Domain;
   class DistributionList;
   class Rule;
   class SecurityRange;
   class IMAPFolder;
   class IMAPFolders;
   class Message;
   class AppPassword;
   class MessageData;
   class DateTime;

   // The server-wide settings group as one JSON object, from the table in
   // RestApiSettings.cpp that the GET, the PUT and the OpenAPI description all
   // read; escape is the JSON string escaper to use for its values.
   AnsiString RestApiSettingsServerGroupJson(AnsiString (*escape)(const AnsiString &));

   class RestApiServer
   {
   public:
      RestApiServer();
      ~RestApiServer();

      bool Start(const String &bind_address, int port, const String &certificate_file, const String &private_key_file);
      void Stop();

      // Full path of the API key store. Public so that tooling and tests can
      // locate the file without duplicating the derivation.
      static String GetApiKeyStoreFile();

      // Files sent as links that have expired, or were never finished, or
      // whose record is gone: removed. Run by the scheduled task every minute
      // (RestApiFiles.cpp). Returns how many went.
      static int SweepExpiredFiles();

      // Every IMAP session on the folder is told, the way STORE, MOVE and
      // EXPUNGE tell them: by message id, which each turns into its own
      // sequence numbers. The one notifier the self-service routes use, in
      // this file and in the route files beside it.
      static void NotifyFolder(std::shared_ptr<IMAPFolder> folder, ChangeNotification::NotificationType type, const std::vector<__int64> &messageIds);

      // Every browser session one client of an account holds, ended; the number
      // that went. Public because the sessions are this listener's and the
      // revocation is not: SignInStore::Revoke is the one place a device is
      // revoked from, whichever interface asked, and this is the part of it that
      // only this class can do. See SignInStore.h.
      static int EndSessionsForDevice(__int64 accountId, const String &device);

      // Every browser session of an account, ended. Public for the same reason
      // as the one above: the compromised-account detection locks an account
      // from the submission thread (CompromiseDetector.h), and the sessions
      // are this listener's to end.
      static void RevokeSessionsForAccount(__int64 accountId);

      // JsonEscape_ of Utf8_, for the helpers of a route file outside the class.
      static AnsiString JsonEscapeUtf8(const String &value);

      // An address book the signed-in account can see: one of its own, or one
      // another account shares with it or with its whole domain
      // (RestApiAddressBooks.cpp). Plain fields rather than the store's record,
      // so that the contacts routes can ask about a book without this header
      // pulling in AddressBookStore.h.
      struct VisibleBook
      {
         VisibleBook() : id(0), ownerAccountId(0), created(0), mine(true), writable(true), isDefault(false) { }

         __int64 id;
         __int64 ownerAccountId;   // whose rows the contacts in it are
         __int64 created;          // UTC seconds
         String name;
         String owner;             // the owner's address, lower-cased
         bool mine;                // the signed-in account owns it
         bool writable;            // it may add, change and delete in it
         bool isDefault;           // the owner's default book
      };

      // One address the signed-in account may write as (RestApiIdentities.cpp).
      struct Identity
      {
         String address;
         String name;
         AnsiString kind;
      };

   private:

      // Which credential a request presented. Used to keep key management off
      // limits to API keys themselves.
      enum AuthenticationResult
      {
         AuthenticationFailed = 0,
         AuthenticatedAsAdministrator = 1,
         AuthenticatedWithApiKey = 2,

         // An account's own credentials, for the account's own endpoints
         // under /api/v1/me. Reaches nothing else.
         AuthenticatedAsAccount = 3
      };

      // What a request asks for, decided once from the method and the path.
      // Authorisation is then a decision about a RouteKind rather than about a
      // string, which is what lets it be made in exactly one place: an
      // endpoint that forgets to check something cannot exist if no endpoint
      // does the checking.
      enum RouteKind
      {
         RouteUnknown = 0,

         // Under /api/v1/apikeys but not one of the three supported calls.
         // Distinct from RouteUnknown so the administrator-only rule still
         // covers it: an API key probing this prefix must learn nothing from
         // the answer, not even whether a verb exists.
         RouteApiKeyUnsupported,

         RouteApiKeyList,
         RouteApiKeyCreate,
         RouteApiKeyRevoke,
         RouteStatus,
         RouteDomainList,
         // One domain by name, whole: the same document an entry of the listing
         // is, and the one hmctl domain get and Get-HmDomain -Name have always
         // asked for. Scoped in DomainNamedByRoute_ to the domain it names, so
         // a key confined to a list of domains reads its own and not another's.
         RouteDomainGet,
         // The domain's own writes. Create and delete are server-wide - the set
         // of domains is the server's, not any one domain's - and are refused
         // for a domain-restricted key in Authorize_; the update is scoped to
         // the domain it names, as the account routes are.
         RouteDomainCreate,
         RouteDomainUpdate,
         RouteDomainDelete,
         RouteAccountList,
         RouteAccountCreate,
         RouteAccountDelete,
         RouteQueueList,
         RouteQueueRetry,
         RouteQueueDelete,
         // The outbound shaping (SMTP/DestinationBackoff.h): the destinations
         // that asked this server to slow down, with the connections in use,
         // and one back-off cleared by name. Server-wide, as the queue is.
         RouteQueueShaping,
         RouteQueueShapingClear,
         RouteTlsa,
         RouteSrv,
         RouteMetricsHistory,
         RouteUpdateGet,
         RouteUpdateCheck,
         RouteUpdateDownload,
         RouteUpdateInstall,
         RouteDiagnosticsRun,
         RouteQuarantineList,
         RouteQuarantineRelease,
         RouteQuarantineDelete,
         RouteAliasList,
         // Wave 88: the surfaces that were COM-only. Server-wide ones are refused
         // for domain-restricted keys in Authorize_; the list routes are scoped to
         // their domain the way the alias and account routes are.
         RouteIpRangeList,
         RouteIpRangeCreate,
         RouteIpRangeDelete,
         RouteListList,
         RouteListCreate,
         RouteListDelete,
         RouteCertificateList,
         RouteDkimGet,
         // DKIM keys that rotate (RestApiDkimRotation.cpp). The six under a domain
         // are scoped to the key's domains; the run is server-wide.
         RouteDkimRotationGet,
         RouteDkimRotationStart,
         RouteDkimRotationCheck,
         RouteDkimRotationSwitch,
         RouteDkimRotationRetire,
         RouteDkimRotationCancel,
         RouteDkimRotationRun,
         // This server's own reputation (RestApiReputation.cpp): what the last
         // check of its outbound addresses found, and a check now. Both are the
         // server's rather than a domain's.
         RouteReputationGet,
         RouteReputationCheck,
         // The first-hour checklist (RestApiSetupChecklist.cpp): what a new
         // server still needs done, judged by the server. The server's, not a
         // domain's: the submission port, the backup destination and the
         // administrator's second factor belong to no domain.
         RouteSetupChecklist,
         // The sample domain (RestApiSampleDomain.cpp): example.test with three
         // accounts, a list, a rule and a handful of delivered messages, made in
         // one call and removed in one. The server's own, like the checklist it
         // sits beside, and the two that change anything are the server
         // administrator's - they create mailboxes with a documented password.
         RouteSampleDomainGet,
         RouteSampleDomainCreate,
         RouteSampleDomainRemove,
         RouteRuleList,
         RouteLogList,
         RouteLogTail,
         // The sessions the server has open and one of them followed live
         // (RestApiSessionFollow.cpp): the log's own permission, server-wide,
         // since a session's lines are whichever domain's traffic it carried.
         RouteLogSessions,
         RouteLogSessionFollow,
         RouteBackupStart,
         RouteBackupStatus,
         RouteBackupRestore,
         // One mailbox, or one message, put back from an archive in the
         // backup destination (RestApiBackupMailbox.cpp): the archives there,
         // an account's folders and messages as an archive holds them, the
         // restore, and what became of one.
         RouteBackupArchiveList,
         RouteBackupMailboxList,
         RouteBackupMailboxRestore,
         RouteBackupMailboxRestoreStatus,
         RouteSettingsGet,
         RouteArchiveSearch,
         RouteArchiveGet,
         RouteArchiveHold,
         RouteArchiveRelease,
         // The account's own endpoints: answered to the account's credentials
         // and to nothing else.
         RouteMe,
         RouteMePassword,
         RouteMeVacation,
         RouteMeQuarantineList,
         RouteMeQuarantineRelease,
         RouteMeQuarantineDelete,
         // Whether the quarantine digest reaches this account, read and
         // written by its owner. The domain decides whether one is sent at all;
         // this is the reader's own half of that decision.
         RouteMeQuarantineDigestGet,
         RouteMeQuarantineDigestPut,
         RouteMeFolders,
         RouteMeFolderMessages,
         // Wave 164: the account's own folder writes, and the change probe the
         // portal polls instead of reloading the whole folder tree.
         RouteMeFolderCreate,
         RouteMeFolderRename,
         RouteMeFolderDelete,
         RouteMeChanges,
         RouteMeMessage,
         RouteMeMessageFlags,
         RouteMeMessageMove,
         RouteMeMessageDelete,
         RouteMeMessageSend,
         RouteMeMessageAttachment,
         RouteMeSearch,
         RouteMeSettings,
         RouteMeSettingsPut,
         RouteMeFilters,
         RouteMeFiltersPut,
         RouteMeDraftSave,
         RouteMeContacts,
         RouteMeContactCreate,
         RouteMeContactUpdate,
         RouteMeContactDelete,
         RouteMeContactPhoto,

         // Contact groups in the same address book (RestApiContactGroups.cpp).
         RouteMeContactGroups,
         RouteMeContactGroupCreate,
         RouteMeContactGroupGet,
         RouteMeContactGroupUpdate,
         RouteMeContactGroupDelete,
         RouteMeContactGroupMemberAdd,
         RouteMeContactGroupMemberRemove,

         // The organisation's directory (RestApiDirectory.cpp): the reader's own
         // domain listed, their own entry, and one entry's photo.
         RouteMeDirectory,
         RouteMeDirectoryOwn,
         RouteMeDirectoryOwnPut,
         RouteMeDirectoryOwnPhoto,
         RouteMeDirectoryPhoto,

         // The account's address books and who they are shared with
         // (RestApiAddressBooks.cpp).
         RouteMeAddressBooks,
         RouteMeAddressBookCreate,
         RouteMeAddressBookUpdate,
         RouteMeAddressBookDelete,
         RouteMeAddressBookShares,
         RouteMeAddressBookSharePut,
         RouteMeAddressBookShareDelete,

         // The account's calendars, events, tasks and the invitations its
         // mail carries (RestApiCalendar.cpp).
         RouteMeCalendars,
         RouteMeCalendarCreate,
         RouteMeCalendarUpdate,
         RouteMeCalendarDelete,
         RouteMeCalendarSharePut,
         RouteMeCalendarShareDelete,
         RouteMeEvents,
         RouteMeEventGet,
         RouteMeEventCreate,
         RouteMeEventUpdate,
         RouteMeEventDelete,
         RouteMeTasks,
         RouteMeTaskCreate,
         RouteMeTaskUpdate,
         RouteMeTaskDelete,
         RouteMeMessageInvitation,
         RouteMeMessageInvitationReply,
         RouteMeCalendarGet,
         RouteMeFreeBusy,
         RouteMeEventReply,
         RouteMeIdentities,
         RouteMePreferences,
         RouteMePreferencesPut,
         RouteMeFolderEmpty,
         RouteMeMessageSource,
         RouteMeMessageHtml,
         RouteMeAppPasswords,
         RouteMeAppPasswordCreate,
         RouteMeAppPasswordDelete,
         RouteMeSessions,
         RouteMeSessionsEnd,
         RouteMeSessionDelete,
         // The clients that have signed in to the account (hm_signins, schema
         // 6050), and the revoke that ends what one holds. The account's own,
         // beside its sessions and its app passwords.
         RouteMeDevices,
         RouteMeDeviceDelete,
         RouteMeStorage,
         RouteMeSmime,
         RouteMeSmimeOwnPut,
         RouteMeSmimeOwnDelete,
         RouteMeSmimeRecipientPut,
         RouteMeSmimeRecipientDelete,
         RouteMeSmimeChain,

         // The account's own OpenPGP public keys and its publishing switch
         // (RestApiOpenPgp.cpp). Self-service: the roles do not reach them.
         RouteMeOpenPgp,
         RouteMeOpenPgpPut,
         RouteMeOpenPgpDelete,
         RouteMeOpenPgpPublish,
         RouteMeOpenPgpKeys,
         RouteMeOpenPgpKeyOwnPut,
         RouteMeOpenPgpKeyOwnDelete,
         RouteMeOpenPgpKeyPeerPut,
         RouteMeOpenPgpKeyPeerDelete,
         RouteMeOpenPgpLookup,
         // The account's own word on one of its correspondents' keys
         // (RestApiOpenPgpKeys.cpp): marked verified in person, and a change
         // of key accepted. Self-service, and refused on an app password.
         RouteMeOpenPgpKeyVerified,
         RouteMeOpenPgpKeyAccepted,
         // Zero-access mailboxes (RestApiZeroAccess.cpp): the account's own
         // switch and the wrapped copies of its key. Self-service; the three
         // writes are refused on an app password.
         RouteMeZeroAccess,
         RouteMeZeroAccessPut,
         RouteMeZeroAccessRewrap,
         RouteMeZeroAccessDelete,
         RouteMeRecall,
         RouteMeMessageReceipt,
         RouteMeMessageUnsubscribe,
         RouteMeSubscriptions,
         RouteMeBrandLogo,
         RouteMeFetchAccounts,
         RouteMeFetchAccountCreate,
         RouteMeFetchAccount,
         RouteMeFetchAccountUpdate,
         RouteMeFetchAccountDelete,
         RouteMeFetchAccountDownload,
         RouteMeFetchImport,
         RouteMeFetchImportStart,
         RouteMeSenderAddresses,
         RouteMeSenderAddressCreate,
         RouteMeSenderAddressVerify,
         RouteMeSenderAddressResend,
         RouteMeSenderAddressDelete,
         RouteMeMaskedAddresses,
         RouteMeMaskedAddressCreate,
         RouteMeMaskedAddressUpdate,
         RouteMeMaskedAddressDelete,
         RouteMeRecovery,
         RouteMeRecoveryPut,
         RouteMeRecoveryDelete,
         RouteMeRecoveryResend,
         RouteMeRecoveryCodes,
         RouteMeRecoverable,
         RouteMeRecoverableRestore,
         RouteMeRecoverableDelete,
         // Passkeys (RestApiPasskeys.cpp). Not under /me: the same four routes
         // answer an account's session with that account's passkeys and the
         // administrator's session with the administrator's, which is why they
         // are classified in IsSessionRoute_ rather than as self-service or as
         // administration.
         RoutePasskeyList,
         RoutePasskeyOptions,
         RoutePasskeyCreate,
         RoutePasskeyDelete,
         // One account's directory entry, administered under its address
         // (RestApiDirectory.cpp).
         RouteAccountDirectoryGet,
         RouteAccountDirectoryPut,
         RouteAccountDirectoryPhoto,
         RouteMeConfidential,
         RouteMeConfidentialRevoke,
         // A password-protected message to anyone (RestApiProtectedMessages.cpp):
         // the sender's list, the record, the ciphertext in chunks and the
         // withdrawal. Self-service; the reader's routes take no credential
         // and are dispatched before Authenticate_.
         RouteMeProtected,
         RouteMeProtectedCreate,
         RouteMeProtectedContent,
         RouteMeProtectedDelete,
         RouteMeOffline,
         RouteMeOfflinePut,
         RouteMeOfflineMessages,
         RouteMeDraftSchedule,
         RouteMeDraftUnschedule,
         RouteMeMessageSnooze,
         RouteMeScheduled,
         RouteMeScheduledCancel,
         RouteScheduledRun,
         RouteMeFolderExport,
         RouteMeExportArchive,
         RouteMeFolderImport,
         RoutePortalBrandingPut,
         RouteAccountSupportSession,
         RouteMeFiles,
         RouteMeFileCreate,
         RouteMeFileContent,
         RouteMeFileUpdate,
         RouteMeFileDelete,
         RoutePortalFilesPolicy,
         RoutePortalFilesPolicyPut,
         RouteSessionCreate,
         RouteSessionDelete,
         // Wave 162: the write surface. Server-wide ones are refused for
         // domain-restricted keys in Authorize_; the alias and account writes
         // are scoped to their domain the way the account routes are.
         RouteSettingsPut,
         RouteSettingsAntiSpamGet,
         RouteSettingsAntiSpamPut,
         RouteSettingsLoggingGet,
         RouteSettingsLoggingPut,
         RouteSettingsDirectoriesGet,
         RouteIniSettingList,
         RouteIniSettingGet,
         RouteIniSettingPut,
         RouteIniSettingDelete,
         RouteSettingsChanged,
         // GET /api/v1/server/settings/pending: the [Settings] keys saved and not
         // yet running, with both values (docs/SettingsApply.md).
         RouteServerSettingsPending,
         RouteLogonFailuresClear,
         RouteSettingsScriptingGet,
         RouteSettingsScriptingPut,
         RouteScriptingReload,
         RouteScriptingCheck,
         RouteBreachCheckTest,
         RouteSettingsBackupGet,
         RouteSettingsBackupPut,
         RouteSettingsCacheGet,
         RouteSettingsCachePut,
         RouteCacheClear,
         RouteSettingsIndexingGet,
         RouteSettingsIndexingPut,
         RouteIndexingIndex,
         RouteIndexingClear,
         RouteServerMessageList,
         RouteServerMessagePut,
         RouteSieveEvaluate,
         RouteCalendarExpand,
         RouteIpRangeUpdate,
         RouteDomainAliasList,
         RouteDomainAliasCreate,
         RouteDomainAliasDelete,
         RouteRuleCreate,
         RouteRuleUpdate,
         RouteRuleDelete,
         RouteCertificateCreate,
         RouteCertificateDelete,
         RoutePortList,
         RoutePortCreate,
         RoutePortUpdate,
         RoutePortDelete,
         RouteRouteList,
         RouteRouteCreate,
         RouteRouteUpdate,
         RouteRouteDelete,
         // What this server will do for a named remote domain: the TLS it
         // demands, the size and concurrency it will attempt, the backup-MX
         // recipient callout. Server-wide, like the routes: a policy decides
         // whether this server refuses to deliver a domain's mail in the clear
         // and whether it opens verification sessions to a third party, and
         // neither is one hosted domain administrator's to decide.
         RouteRemoteDomainList,
         RouteRemoteDomainCreate,
         RouteRemoteDomainUpdate,
         RouteRemoteDomainDelete,
         RouteRemoteDomainEffective,
         RouteRemoteDomainClearCache,
         RouteAliasCreate,
         RouteAliasDelete,
         RouteAccountUpdate,
         RouteAccountGet,
         RouteFetchAccountList,
         RouteFetchAccountCreate,
         RouteFetchAccountGet,
         RouteFetchAccountUpdate,
         RouteFetchAccountDelete,
         RouteFetchAccountDownload,
         RouteServerReinitialize,
         // Maintenance mode: the listeners closed and the queue paused while
         // what is in flight finishes, with the process up. Server-wide, as the
         // reinitialize above is; the GET is a reader's, the other two the
         // server administrator's.
         RouteServerDrainGet,
         RouteServerDrainEnter,
         RouteServerDrainLeave,
         // The anti-virus settings group, server-wide as the other groups are.
         RouteSettingsAntiVirusGet,
         RouteSettingsAntiVirusPut,
         // The distribution list's update, scoped to the address's domain as
         // its delete is.
         RouteListUpdate,
         // The five small collections of RestApiAntiSpamLists.cpp, each a
         // list, a create, an update and a delete; all server-wide.
         RouteDnsBlackListList,
         RouteDnsBlackListCreate,
         RouteDnsBlackListUpdate,
         RouteDnsBlackListDelete,
         RouteSurblServerList,
         RouteSurblServerCreate,
         RouteSurblServerUpdate,
         RouteSurblServerDelete,
         RouteWhiteListAddressList,
         RouteWhiteListAddressCreate,
         RouteWhiteListAddressUpdate,
         RouteWhiteListAddressDelete,
         RouteBlockedSenderList,
         RouteBlockedSenderCreate,
         RouteBlockedSenderUpdate,
         RouteBlockedSenderDelete,
         RouteIncomingRelayList,
         RouteIncomingRelayCreate,
         RouteIncomingRelayUpdate,
         RouteIncomingRelayDelete,
         // The blocked attachments (RestApiBlockedAttachments.cpp), the same
         // four verbs; server-wide.
         RouteBlockedAttachmentList,
         RouteBlockedAttachmentCreate,
         RouteBlockedAttachmentUpdate,
         RouteBlockedAttachmentDelete,
         // The journaling and dual-delivery rules (RestApiJournalRules.cpp),
         // the same four verbs; server-wide, and refused to a credential
         // confined to named domains even though a rule names one.
         RouteJournalRuleList,
         RouteJournalRuleCreate,
         RouteJournalRuleUpdate,
         RouteJournalRuleDelete,
         // The greylisting white list, the sixth collection of
         // RestApiAntiSpamLists.cpp; server-wide.
         RouteGreyListingWhiteAddressList,
         RouteGreyListingWhiteAddressCreate,
         RouteGreyListingWhiteAddressUpdate,
         RouteGreyListingWhiteAddressDelete,
         // The per-domain greylisting bypass list (RestApiGreylistBypass.cpp):
         // the same four verbs; server-wide.
         RouteGreylistBypassList,
         RouteGreylistBypassCreate,
         RouteGreylistBypassUpdate,
         RouteGreylistBypassDelete,
         // The sender list (RestApiSenderLists.cpp): the same four verbs at
         // each of its three scopes. The server's is server-wide; the domain's
         // is scoped to the domain in its path, as the distribution lists are;
         // the account's own is a /me route and needs no authorisation beyond
         // being signed in as that account.
         RouteSenderListList,
         RouteSenderListCreate,
         RouteSenderListUpdate,
         RouteSenderListDelete,
         RouteSenderListDomainList,
         RouteSenderListDomainCreate,
         RouteSenderListDomainUpdate,
         RouteSenderListDomainDelete,
         RouteSenderListMeList,
         RouteSenderListMeCreate,
         RouteSenderListMeUpdate,
         RouteSenderListMeDelete,

         // Spam traps (RestApiSpamTraps.cpp): a domain's traps, one trap removed
         // at its own address as an alias is, and the record of what has
         // addressed them - one domain's, every domain's, and its clearing.
         RouteSpamTrapDomainList,
         RouteSpamTrapDomainCreate,
         RouteSpamTrapDelete,
         RouteSpamTrapDomainHits,
         RouteSpamTrapHits,
         RouteSpamTrapHitsClear,

         // The domain white and black list (RestApiDomainPolicyList.cpp): one
         // server-wide list keyed by envelope domain or HELO name, in the
         // shape of the anti-spam lists above.
         RouteDomainPolicyList,
         RouteDomainPolicyCreate,
         RouteDomainPolicyUpdate,
         RouteDomainPolicyDelete,
         // An account's resources under its address (RestApiAccountResources.cpp):
         // its app passwords, its folders and their ACLs, its messages. Each
         // scoped to the address's domain in Authorize_ as the account routes are.
         RouteAccountAppPasswordList,
         RouteAccountAppPasswordCreate,
         RouteAccountAppPasswordDelete,
         RouteAccountFolderList,
         RouteAccountFolderPermissionList,
         RouteAccountFolderPermissionCreate,
         RouteAccountFolderPermissionUpdate,
         RouteAccountFolderPermissionDelete,
         RouteAccountMessageList,
         RouteAccountMessagesDelete,
         RouteAccountFolderMessageList,
         RouteAccountFolderMessageCreate,
         RouteAccountMessageGet,
         RouteAccountMessageFlags,
         RouteAccountMessageDelete,
         RouteAccountMessageSource,
         RouteAccountAppPasswordUpdate,
      // An account's devices, read and revoked by an administrator
      // (RestApiAccountResources.cpp). Scoped to the address's domain in
      // Authorize_ as the other routes under an address are.
      RouteAccountDeviceList,
      RouteAccountDeviceDelete,
         // An account's address books and who they are shared with, read and
         // a share removed by an administrator (RestApiAddressBooks.cpp).
         // Scoped to the address's domain in Authorize_ as the rest are. There
         // is no route here that reads a shared book's CONTACTS: an
         // administrator sees the shape of the sharing, not the address book.
         RouteAccountAddressBooks,
         RouteAccountAddressBookShareDelete,
         // The account groups (RestApiGroups.cpp): Settings.Groups, Group and
         // Group.Members over COM. A group holds accounts of any domain and is
         // an ACL principal, so the resource is server-wide and refused for
         // domain-restricted keys in Authorize_.
         RouteGroupList,
         RouteGroupCreate,
         RouteGroupGet,
         RouteGroupUpdate,
         RouteGroupDelete,
         RouteGroupMemberList,
         RouteGroupMemberCreate,
         RouteGroupMemberDelete,
         // A message copied into another folder of its account
         // (RestApiAccountResources.cpp): Message.Copy over COM. Scoped to the
         // address's domain as the other message routes are.
         RouteAccountMessageCopy,
         // A rule criterion tried against a value (RestApiRules.cpp):
         // Utilities.CriteriaMatch over COM. Nothing is read or written;
         // server-wide as the Sieve evaluation is.
         RouteRuleMatch,
         // Who may administer what (RestApiRoles.cpp). The reads are
         // ViewConfiguration - an auditor's whole job is to be able to see who
         // holds which role - and the writes are ManageRoles, which no custom
         // role can be given, because a role that can hand out roles is the
         // server administrator with a different name.
         RouteRoleList,
         RouteRoleGet,
         RouteRoleCreate,
         RouteRoleUpdate,
         RouteRoleDelete,
         RouteRoleAssignmentList,
         RouteRoleAssign,
         RouteRoleUnassign,
         // What the credential this request came in on may do, so that the
         // Control Deck can leave out a view its role may not use rather than
         // showing one that answers 403 to every button on it.
         RouteSessionGet,
         // A password reset and an unlock as routes of their own, rather than
         // as fields of the account update. The help-desk role exists to do
         // exactly these two things and nothing else, and a permission can
         // only be that narrow when the route is.
         RouteAccountPasswordReset,
         RouteAccountUnlock,
         // An account's own rules (RestApiRules.cpp), under the account: what
         // Account.Rules holds over COM, which /api/v1/rules - the global rules
         // - never carried. Scoped to the address's domain.
         RouteAccountRuleList,
         RouteDomainRuleList,
         RouteDomainRuleCreate,
         RouteDomainRuleUpdate,
         RouteDomainRuleDelete,
         RouteAccountRuleCreate,
         RouteAccountRuleUpdate,
         RouteAccountRuleDelete,
         // The operator's tools (RestApiOperator.cpp), the administrator
         // password's alone: a mailbox exported to the server's disk, an
         // address's traces erased, the administrator's second factor.
         RouteAccountExport,
         RouteAccountExportArchive,
         RouteAddressErase,
         RouteAdminSecondFactorGet,
         RouteAdminSecondFactorEnrol,
         RouteAdminSecondFactorDisable,
         // An account's Web Push devices, seen and stopped by an administrator
         // (RestApiPush.cpp): what a lost phone needs, and Account.
         // PushSubscriptionCount and RemovePushSubscriptions over COM. The
         // listing is the same safe one the account itself gets - never the
         // endpoint or a key.
         RouteAccountPushGet,
         RouteAccountPushRemove,
         // An account's recovery address, cleared by an administrator
         // (RestApiAccountRecovery.cpp). There is no route that reads it in
         // full and no interface that can: GET /api/v1/accounts/{address}
         // carries the masked form and whether it has been confirmed.
         RouteAccountRecoveryClear,
         RouteAccountRecoveryCodesClear,
         // The message trace (RestApiRoles.cpp): Application.GlobalObjects.
         // MessageTrace over COM. The recent events and one queued message's
         // story are server-wide; one address's events are scoped to its domain,
         // which is how a help desk reads the trace of a person it was rung up
         // about and nobody else's.
         RouteMessageTraceRecent,
         RouteMessageTraceAddress,
         // Renaming a mailbox, and moving one to another domain on this server
         // (RestApiMailboxMove.cpp). Both are scoped by the address in the path
         // as every account route is; the POST also judges the domain in its
         // BODY, which no generic authorisation can see.
         RouteMailboxMoveGet,
         RouteMailboxMove,
         // Complaints read, the feedback loops (RestApiComplaints.cpp). The
         // listing under a domain is scoped to it in Authorize_ as the
         // suppression list is; the rates, the settings group and the sweep are
         // server-wide, because a rate is every domain's and the sweep prunes
         // every domain's rows.
         RouteComplaintList,
         RouteComplaintRates,
         RouteComplaintExpire,
         RouteSettingsComplaintsGet,
         RouteSettingsComplaintsPut,
         // The reports our own domains receive, read (RestApiDomainReports.cpp):
         // the DMARC aggregate reports and the TLS-RPT reports read from a
         // domain's report mailbox. The four under a domain are scoped to it in
         // Authorize_ as the complaints are; the sweep prunes every domain's rows
         // and is server-wide.
         RouteDomainReportsSummary,
         RouteDomainReportsList,
         RouteDomainReportGet,
         RouteDomainTlsReportsList,
         RouteDomainReportsExpire,
         RouteMessageTraceQueue,
         // The audit trail and the alerts (RestApiAudit.cpp). Every one of these
         // is refused to an API key of any scope - they are in IsApiKeyRoute_ -
         // because the record of who changed what is the one thing a stolen key
         // must not be able to read, edit the rules of, or watch for its own
         // footprints. The administrator password reaches them, and so does an
         // account whose role holds the permission PermissionFor_ names and
         // reaches every domain: view_audit for the trail, which is what the
         // auditor role is for; view_configuration for the alert rules and
         // events; manage_settings to change a rule or raise a test;
         // manage_server to run a pass.
         RouteAuditList,
         RouteAuditVerify,
         RouteAuditRevert,
         RouteAlertRuleList,
         RouteAlertRuleUpdate,
         RouteAlertEventList,
         RouteAlertTest,
         RouteAlertRun,
         // Compromised-account detection (RestApiAccountReview.cpp): the
         // messages kept out of the delivery queue for review, server-wide
         // like the queue and refused to keys like the alerts - a stolen key
         // must not release what its own sending got held; and, under an
         // account's address, its review state: exempt, sensitivity, the
         // compromise lock and the last shape seen.
         RouteReviewList,
         RouteReviewRelease,
         RouteReviewDiscard,
         RouteAccountReviewGet,
         RouteAccountReviewPut,
         RouteAccountReviewUnlock,
         // The reports (RestApiReports.cpp): what happened on this server,
         // per domain and per day, aggregated from the message trace, the
         // metric history and the message store. Read-only. The sections
         // built from the message trace and from the mailbox sizes carry a
         // domain and are scoped to it in Authorize_ exactly as the account
         // routes are; the two built from the server-wide metric history
         // (volume and storage) are refused for a domain-restricted key, for
         // Outbound webhooks (RestApiWebhooks.cpp): the subscriptions, the
         // delivery queue, a pass run on demand and a test event. Server-wide,
         // and refused to every API key for the reason the alert rules are - a
         // subscription holds a signing secret and points this server at a URL,
         // so a stolen key must not be able to read one or make one.
         RouteWebhookList,
         RouteWebhookGet,
         RouteWebhookCreate,
         RouteWebhookUpdate,
         RouteWebhookDelete,
         RouteWebhookDeliveryList,
         RouteWebhookRun,
         RouteWebhookTest,
         // the reason the delivery queue is - the counters are the server's
         // and there is no honest way to narrow them to one domain.

         // Web Push (Common/Util/WebPush.h). The five under /api/v1/me are the
         // account's own devices - self-service, so they are classified in
         // IsSelfServiceRoute_ and the roles do not reach them; the three under
         // /api/v1/settings are the administrator's switch, limits and key pair.
         RouteMePush,
         RouteMePushSubscribe,
         RouteMePushUpdate,
         RouteMePushDelete,
         RouteMePushTest,
         RouteSettingsPushGet,
         RouteSettingsPushPut,
         RouteSettingsPushVapid,
         RouteReport,
         // Bounces read, and the suppression list (RestApiBounces.cpp). The four
         // under a domain are scoped to it in Authorize_ as the account routes are;
         // the settings group and the sweep are server-wide; classify reads and
         // writes nothing.
         RouteSuppressionList,
         RouteSuppressionCreate,
         RouteSuppressionDelete,
         RouteBounceCountList,
         RouteBounceClassify,
         RouteBounceExpire,
         // The session policy in force for a domain's accounts, resolved from
         // the server's four settings and the domain's own four columns
         // (SessionPolicies). Scoped to the domain as the account routes are;
         // what Utilities.GetSessionPolicy answers over COM, so that a script
         // on Linux can ask the same question.
         RouteDomainSessionPolicyGet,
         // The same document for the server alone - what governs the
         // administrator's own session and every domain that says nothing.
         RouteSessionPolicyGet,
         RouteSettingsBouncesGet,
         RouteSettingsBouncesPut,

         // The Web Key Directory: the server's settings, one account's keys
         // and one domain's (RestApiOpenPgp.cpp).
         RouteSettingsOpenPgpGet,
         RouteSettingsOpenPgpPut,
         RouteAccountOpenPgpList,
         RouteAccountOpenPgpPut,
         RouteAccountOpenPgpDelete,
         RouteDomainOpenPgpList,

         // Who a distribution list delivers to now: its rows and what its
         // membership query answers at the moment of the call
         // (RestApiDistributionLists.cpp). A read of accounts, scoped to the
         // domain in the list's address exactly as the update and the delete
         // are.
         RouteListMembers,
         RouteOpenApi
      };

      struct Route
      {
         Route() : kind(RouteUnknown), message_id(0), range_id(0), archive_id(0), folder_id(0), attachment_index(0), record_id(0), member_account_id(0) { }

         RouteKind kind;
         AnsiString identifier;   // domain name, account address or api key id
         __int64 message_id;
         __int64 range_id;        // an IP range id, for the routes that name one
         __int64 archive_id;      // an archive index row id, for the routes that name one
         __int64 folder_id;       // an IMAP folder id, for the account's own mailbox routes
         int attachment_index;    // which attachment of a message, for the download route
         __int64 record_id;       // a rule, certificate, port, route or group id, for the write routes that name one
         __int64 member_account_id; // the account id under a group's members, for the route that removes one
         AnsiString query;        // the part after "?", for the routes that take one
         AnsiString name;         // a second path segment naming a member of the resource (a domain alias under its domain, a server message)
      };

      // One record in the API key store. Never holds the clear-text token.
      struct ApiKeyRecord
      {
         // read_only defaults to true so that every path that fails to read a
         // Scope - a hand-written section, a truncated file, a value we do not
         // recognise - leaves the key unable to change anything.
         ApiKeyRecord() : read_only(true) { }

         String id;             // store section suffix; used to revoke
         String label;          // human-readable, so keys can be told apart
         AnsiString hash;       // lower-case hex SHA-256 of the token
         String expires;        // hMailServer system date: YYYY-MM-DD HH:MM:SS
         String allowed_from;   // empty = any source; else address, range or CIDR
         bool read_only;        // Scope: false only for the literal "full"
         std::vector<String> domains;   // empty = every domain

         // Role: the name of a role in hm_roles. Empty - which is what every
         // key written before September 2026 has - means the Scope above
         // decides, exactly as it always did. A name this server does not have
         // leaves the key holding nothing, rather than falling back to the
         // scope: a key whose role was deleted must stop working.
         String role;
      };

      // The authority a request carries, and the one thing an authorisation
      // decision is allowed to look at.
      struct Caller
      {
         Caller() : result(AuthenticationFailed), read_only(true), second_factor_required(false) { }

         AuthenticationResult result;

         // Set on a refusal when the administrator password was right and what
         // was missing was the one-time code; the 401 then says so in a header,
         // as GitHub's API does, so a client knows to ask for it.
         bool second_factor_required;

         // "administrator", or "key:<id>". Identifies the credential for rate
         // accounting and for the log; never the secret itself.
         AnsiString identity;

         bool read_only;
         std::vector<String> domains;

         // The role this credential holds and what it reaches: the whole of
         // the authority decision, resolved once when the credential was
         // authenticated and carried with the request from there. read_only
         // and domains above are the same authority in the two older spellings
         // the handlers already filter by, and are set FROM this.
         RoleGrant grant;

         // Set when result is AuthenticatedAsAccount.
         std::shared_ptr<const Account> account;

         // Where the request came from, for the handlers that count a
         // failure against it.
         IPAddress peer;

         // True when the credential was a browser session cookie rather than
         // a password: such a request must carry the X-Requested-With header
         // to change anything, and may end the session it came with.
         bool via_session = false;
         AnsiString session_hash;

         // True when that session was presented as "Authorization: Bearer
         // <token>" rather than as the cookie - the form the webmail holds for
         // an account added beside the one it signed in as. Everything above
         // holds of it; what differs is that ending it must not touch the
         // cookie, which is another account's.
         bool via_bearer = false;

         // The client the session belongs to: SignInStore's device key, so that a
         // handler can mark the entry the reader is looking at as this one.
         AnsiString session_device;

         // True when the password the request came in on - or the password
         // that started the session it came in on - was one of the account's
         // app passwords: a credential for a program, which reads and sends
         // mail as the account but does not mint or revoke credentials or
         // end the account holder's browser sessions.
         bool via_app_password = false;

         // That app password's name, where one was used - the label its owner
         // gave it, and nothing derived from the secret. Recorded against the
         // client in the account's device list, so a person can see which of
         // their credentials a client they do not recognise is using.
         String credential_name;

         // How many of the account's one-time recovery codes are left, when THIS
         // request spent one in place of a second factor (AccountRecovery.h).
         // -1 when none was spent, which is every request but the sign-in of
         // somebody whose authenticator is gone. The webmail says the number
         // once it is in, because a person who has just spent one of ten needs
         // to know that.
         int recovery_codes_left = -1;
         // Set when the session was opened by an administrator for support:
         // who, for the log line every request of it writes.
         AnsiString support_by;

         // True when this request proved itself with a passkey rather than a
         // password. The person was verified by their own authenticator - a
         // PIN or a fingerprint - so the one-time code is not asked for on top
         // of it, and the account's device list records which credential it was.
         // The assertion is refused unless it says so: AuthenticatePasskey_
         // checks the user-verified flag on every sign-in and not only at
         // enrolment, because this is the claim the missing code rests on.
         bool via_passkey = false;

         // True when it proved itself with a ticket from the organisation's
         // identity provider (OidcSignIn.h). Same reason, same record.
         bool via_single_sign_on = false;
      };

      enum AuthorizationResult
      {
         AuthorizationAllowed = 0,

         // Refused as a credential problem: answered 401, so that a key
         // probing key management cannot tell "valid but not permitted" from
         // "not a key at all".
         AuthorizationUnauthenticated = 1,

         // Refused as a permission problem: answered 403 and said why. The
         // caller already knows its credential is good, so there is nothing
         // left to conceal and everything to gain from being clear.
         AuthorizationForbidden = 2
      };

      // Request processing. Returns the full HTTP response.
      HttpResponse ProcessRequest_(const AnsiString &request, const IPAddress &peer_address);

      static void ParseRoute_(const AnsiString &method, const AnsiString &path, Route &route);

      // One path segment as the client sent it, with %XX turned back into the
      // character it stands for - except a separator, which is never decoded.
      // See the note beside the call for both halves of that.
      static AnsiString DecodePathSegment_(const AnsiString &raw);

      // The single authorisation choke point. Every route passes through this
      // and nothing else decides access.
      static AuthorizationResult Authorize_(const Caller &caller, const Route &route, AnsiString &refusalReason);

      // The permission each administrative route needs. Every RouteKind is
      // named here or in IsSelfServiceRoute_ or in IsSessionRoute_, and
      // build/check-role-choke-point.py fails the build when one is not: a
      // route nobody classified is a route that decides for itself.
      static RolePermission PermissionFor_(RouteKind kind);

      // The second permission a route needs, for the few that need two, and
      // false for every other. Asked in Authorize_ beside PermissionFor_, in the
      // same domain, and both must be held: a second permission only ever
      // narrows who reaches a route. build/check-role-choke-point.py holds a
      // route named here to being classified in PermissionFor_ as well, and
      // holds both of its permissions to the auditor rule.
      static bool SecondPermissionFor_(RouteKind kind, RolePermission &permission);

      // The routes that are about the credential rather than about the server:
      // starting a browser session, ending it, asking what it is, and the
      // passkeys whoever signed in holds. Decided before the permissions,
      // because a caller has to be able to ask what it may do without already
      // being allowed to do it - and because a passkey belongs to a credential
      // rather than to a resource an administrator administers.
      static bool IsSessionRoute_(RouteKind kind);

      // The role an account holds, written into the caller. Called at each of
      // the two places an account authenticates - a password, and a session
      // cookie - so that both read the same store and neither decides.
      static void ApplyAccountGrant_(Caller &caller);

      // True when the request carries the server administrator's whole
      // authority: AdministerServer, with no list of domains narrowing it. The
      // handlers that decide what may be done to a server administrator's own
      // account, or to a domain's limits, ask this rather than reading the
      // domain list alone - which is what they did when a key's only narrowing
      // was a list of domains, and which would let a key issued with Role=
      // domainadmin and no Domains line promote somebody to server
      // administrator.
      static bool HoldsServerAuthority_(const Caller &caller);

      // The domain a route acts inside, or nothing when it acts on the server.
      // One reading of the path for both decisions made about it.
      static String DomainNamedByRoute_(const Route &route);

      static bool IsMutatingRoute_(RouteKind kind);
      static bool IsApiKeyRoute_(RouteKind kind);
      static bool IsSelfServiceRoute_(RouteKind kind);
      // request is here for one header: a recovery code in place of the second
      // factor, for an account whose authenticator is gone (AccountRecovery.h).
      static bool AuthenticateAccount_(const String &username, const String &password, const IPAddress &peer_address,
                                       const AnsiString &request, Caller &caller);
      static HttpResponse HandleMe_(const Caller &caller);
      static HttpResponse HandleMePassword_(const Caller &caller, const AnsiString &request);
      static HttpResponse HandleMeVacation_(const Caller &caller, const AnsiString &requestBody);
      static HttpResponse HandlePortalPage_(const AnsiString &request);
      static HttpResponse HandleMeQuarantineList_(const Caller &caller);
      static HttpResponse HandleMeQuarantineRelease_(const Caller &caller, __int64 id);
      static HttpResponse HandleMeQuarantineDelete_(const Caller &caller, __int64 id);
      static HttpResponse HandleMeFolders_(const Caller &caller);
      // Wave 164, all of them in RestApiMailbox.cpp: the account's own folder
      // writes and the change probe. Each judges a name exactly as the IMAP
      // command of the same purpose judges it and answers with that command's
      // own sentence, so a portal and a mail client are never told different
      // things about the same mailbox.
      static HttpResponse HandleMeFolderCreate_(const Caller &caller, const AnsiString &requestBody);
      static HttpResponse HandleMeFolderRename_(const Caller &caller, __int64 folderId, const AnsiString &requestBody);
      static HttpResponse HandleMeFolderDelete_(const Caller &caller, __int64 folderId);
      static HttpResponse HandleMeChanges_(const Caller &caller, const AnsiString &query);
      static AnsiString OpenApiMailboxPaths_();
      // The folder the id names in the SIGNED-IN ACCOUNT'S OWN tree, and
      // nowhere else: the public namespace and a delegating owner's folders are
      // readable through the message routes, but they are not this account's to
      // create, rename or delete.
      static std::shared_ptr<IMAPFolder> FindOwnFolder_(std::shared_ptr<const Account> account, __int64 folderId);
      // The path of one folder inside its own tree, walked up by parent id, as
      // the vector of names IMAP would have split from a mailbox name. False
      // when the walk does not reach the root, which is what a folderparentid
      // cycle looks like.
      static String DecodeFolderName_(const String &stored);
      static bool IsAtOrBelow_(std::shared_ptr<IMAPFolders> tree, __int64 folderId, __int64 ancestorId);
      static std::vector<String> StoredFolderPath_(const String &name, const String &delimiter);
      static bool OwnFolderPath_(std::shared_ptr<IMAPFolders> tree, std::shared_ptr<IMAPFolder> folder, std::vector<String> &path);
      // One folder as the listing renders it, subtree and all, so that what a
      // create or a rename answers is exactly what the next listing shows.
      static AnsiString FolderEntryJson_(std::shared_ptr<const Account> account, std::shared_ptr<IMAPFolder> folder);
      static HttpResponse HandleMeFolderMessages_(const Caller &caller, __int64 folderId, const AnsiString &query);
      static HttpResponse HandleMeMessage_(const Caller &caller, __int64 messageId);
      static std::shared_ptr<IMAPFolder> FindReadableFolder_(std::shared_ptr<const Account> account, __int64 folderId);
      static bool RightOn_(std::shared_ptr<const Account> account, std::shared_ptr<IMAPFolder> folder, int permission);
      static String MessageFile_(std::shared_ptr<const Message> message);
      static void AppendFolderJson_(std::shared_ptr<const Account> account, std::shared_ptr<IMAPFolders> folders,
                                    const String &parentPath, const std::map<__int64, int> &designations,
                                    const String &delimiter, AnsiString &json, int depth);
      // One entry of that listing, with its subtree. Split out of the loop so
      // that the folder writes answer with the very same document rather than a
      // second spelling of it that could drift.
      static void AppendOneFolderJson_(std::shared_ptr<const Account> account, std::shared_ptr<IMAPFolder> folder,
                                       const String &path, const std::map<__int64, int> &designations,
                                       const String &delimiter, bool writeAccess, AnsiString &json, int depth);
      static HttpResponse HandleMeMessageFlags_(const Caller &caller, __int64 messageId, const AnsiString &requestBody);
      static HttpResponse HandleMeMessageMove_(const Caller &caller, __int64 messageId, const AnsiString &requestBody);
      static HttpResponse HandleMeMessageDelete_(const Caller &caller, __int64 messageId, const AnsiString &query);
      static std::shared_ptr<Message> FindOwnMessage_(std::shared_ptr<const Account> account, __int64 messageId, std::shared_ptr<IMAPFolder> &folder);
      static std::shared_ptr<IMAPFolder> FindDesignatedFolder_(std::shared_ptr<const Account> account, int designation);
      static HttpResponse HandleMeMessageSend_(const Caller &caller, const AnsiString &requestBody);
      static String JsonUtf8Value_(const AnsiString &json, const AnsiString &key);

      // A member that is a whole number, zero or more: 1 when read into value,
      // 0 when the body does not name the key at all, -1 when it names it as
      // something else - a negative number, a fraction, a string, true, null -
      // which a route refuses rather than reads as absent, since a client
      // that wrote the key meant something by it.
      static int JsonWholeNumber_(const AnsiString &json, const AnsiString &key, __int64 &value);
      static HttpResponse HandleMeMessageAttachment_(const Caller &caller, __int64 messageId, int index);
      static HttpResponse HandleMeSearch_(const Caller &caller, const AnsiString &query);
      static HttpResponse HandleMeSettings_(const Caller &caller);
      static HttpResponse HandleMeSettingsPut_(const Caller &caller, const AnsiString &requestBody);
      static HttpResponse HandleMeFilters_(const Caller &caller);
      static HttpResponse HandleMeFiltersPut_(const Caller &caller, const AnsiString &requestBody);
      static AnsiString SettingsJson_(std::shared_ptr<const Account> account);
      static HttpResponse HandleMeDraftSave_(const Caller &caller, const AnsiString &requestBody);

      // The account's address book (RestApiContacts.cpp).
      static HttpResponse HandleMeContacts_(const Caller &caller, const AnsiString &query);
      static HttpResponse HandleMeContactCreate_(const Caller &caller, const AnsiString &requestBody);
      static HttpResponse HandleMeContactUpdate_(const Caller &caller, __int64 id, const AnsiString &requestBody);
      static HttpResponse HandleMeContactDelete_(const Caller &caller, __int64 id);
      static HttpResponse HandleMeContactPhoto_(const Caller &caller, __int64 id);

      // The groups in it (RestApiContactGroups.cpp): the list, one group, its
      // name and members, one member added or removed. memberReference is the
      // path segment under /members - a contact id or an address.
      static HttpResponse HandleMeContactGroups_(const Caller &caller, RouteKind kind, __int64 groupId, const AnsiString &memberReference, const AnsiString &requestBody);
      static AnsiString ContactGroupJson_(const ContactGroup &group, const ContactRecord *record, bool includeCard, const VisibleBook *book = nullptr);
      static AnsiString OpenApiContactGroupsPaths_();

      // The account's address books and their shares (RestApiAddressBooks.cpp).
      static HttpResponse HandleMeAddressBooks_(const Caller &caller);
      static HttpResponse HandleMeAddressBookCreate_(const Caller &caller, const AnsiString &requestBody);
      static HttpResponse HandleMeAddressBookUpdate_(const Caller &caller, __int64 bookId, const AnsiString &requestBody);
      static HttpResponse HandleMeAddressBookDelete_(const Caller &caller, __int64 bookId);
      static HttpResponse HandleMeAddressBookShares_(const Caller &caller, __int64 bookId);
      static HttpResponse HandleMeAddressBookSharePut_(const Caller &caller, __int64 bookId, const AnsiString &target, const AnsiString &requestBody);
      static HttpResponse HandleMeAddressBookShareDelete_(const Caller &caller, __int64 bookId, const AnsiString &target);
      static HttpResponse HandleAccountAddressBooks_(const AnsiString &address);
      static HttpResponse HandleAccountAddressBookShareDelete_(const AnsiString &address, __int64 bookId, const AnsiString &target);
      static AnsiString OpenApiAddressBookPaths_();

      // Every book the account can see, its own first (the default before the
      // rest), then the ones shared with it. False when they could not be read.
      static bool VisibleBooks_(std::shared_ptr<const Account> account, std::vector<VisibleBook> &books);

      // One of them by id. A book the account cannot see is simply not found.
      static bool FindVisibleBook_(std::shared_ptr<const Account> account, __int64 bookId, VisibleBook &found);

      // The contact a /me/contacts/{id} request is about, and the book it is
      // in - one of the account's own or one shared with it. needWrite refuses
      // a book shared read-only. status and problem carry the refusal.
      static bool FindVisibleContact_(std::shared_ptr<const Account> account, __int64 id, bool needWrite,
                                      ContactRecord &contact, VisibleBook &book, int &status, AnsiString &problem);

      // The account's calendars, events, tasks and invitations (RestApiCalendar.cpp).
      static HttpResponse HandleMeCalendars_(const Caller &caller);
      static HttpResponse HandleMeCalendarCreate_(const Caller &caller, const AnsiString &requestBody);
      static HttpResponse HandleMeCalendarUpdate_(const Caller &caller, __int64 calendarId, const AnsiString &requestBody);
      static HttpResponse HandleMeCalendarDelete_(const Caller &caller, __int64 calendarId);
      static HttpResponse HandleMeCalendarSharePut_(const Caller &caller, __int64 calendarId, const AnsiString &address, const AnsiString &requestBody);
      static HttpResponse HandleMeCalendarShareDelete_(const Caller &caller, __int64 calendarId, const AnsiString &address);
      static HttpResponse HandleMeEvents_(const Caller &caller, const AnsiString &query);
      static HttpResponse HandleMeEventGet_(const Caller &caller, __int64 objectId);
      static HttpResponse HandleMeEventCreate_(const Caller &caller, const AnsiString &requestBody);
      static HttpResponse HandleMeEventUpdate_(const Caller &caller, __int64 objectId, const AnsiString &requestBody);
      static HttpResponse HandleMeEventDelete_(const Caller &caller, __int64 objectId, const AnsiString &query);
      static HttpResponse HandleMeTasks_(const Caller &caller);
      static HttpResponse HandleMeTaskCreate_(const Caller &caller, const AnsiString &requestBody);
      static HttpResponse HandleMeTaskUpdate_(const Caller &caller, __int64 objectId, const AnsiString &requestBody);
      static HttpResponse HandleMeTaskDelete_(const Caller &caller, __int64 objectId);
      static HttpResponse HandleMeMessageInvitation_(const Caller &caller, __int64 messageId);
      static HttpResponse HandleMeMessageInvitationReply_(const Caller &caller, __int64 messageId, const AnsiString &requestBody);
      static HttpResponse HandleMeCalendarGet_(const Caller &caller, __int64 calendarId);
      static HttpResponse HandleMeFreeBusy_(const Caller &caller, const AnsiString &query);
      static HttpResponse HandleMeEventReply_(const Caller &caller, __int64 objectId, const AnsiString &requestBody);
      static int SendEventMail_(std::shared_ptr<const Account> account, const ICalComponent &tree, const AnsiString &method, AnsiString &problem);
      static bool FindStoredEvent_(std::shared_ptr<const Account> account, const AnsiString &uid, CalendarRecord &calendar, CalendarObjectRecord &record);
      static AnsiString OpenApiCalendarPaths_();
      static void CollectContacts_(std::shared_ptr<const Account> account, const std::vector<String> &entries);
      static AnsiString ContactJson_(const ContactRecord &contact, bool includeCard, const VisibleBook *book = nullptr);

      // Who the account may write as, and the From a send or draft asked for (RestApiIdentities.cpp).
      static HttpResponse HandleMeIdentities_(const Caller &caller);
      static void IdentitiesFor_(std::shared_ptr<const Account> account, std::vector<Identity> &identities);
      static int ResolveSender_(std::shared_ptr<const Account> account, const String &requested, String &address, String &header, AnsiString &problem);

      // The account's preferences, a key/value store (RestApiPreferences.cpp).
      static HttpResponse HandleMePreferences_(const Caller &caller);
      static HttpResponse HandleMePreferencesPut_(const Caller &caller, const AnsiString &requestBody);

      // Web Push, in RestApiPush.cpp: the account's own devices. The listing
      // never carries the endpoint or either key - see the comment at the top of
      // that file for why that is a rule and not a convenience.
      static AnsiString PushSubscriptionJson_(const PushSubscriptions::Subscription &subscription);
      static AnsiString PushStateJson_(__int64 accountId);
      static HttpResponse HandleMePush_(const Caller &caller);
      static HttpResponse HandleMePushSubscribe_(const Caller &caller, const AnsiString &requestBody);
      static HttpResponse HandleMePushUpdate_(const Caller &caller, __int64 id, const AnsiString &requestBody);
      static HttpResponse HandleMePushDelete_(const Caller &caller, __int64 id);
      static HttpResponse HandleMePushTest_(const Caller &caller, const AnsiString &requestBody);
      static HttpResponse HandleAccountPush_(const Caller &caller, const AnsiString &address);
      static HttpResponse HandleAccountPushRemove_(const Caller &caller, const AnsiString &address);

      // And the administrator's half, in RestApiSettings.cpp beside the other
      // settings groups.
      static HttpResponse HandleSettingsPush_();
      static HttpResponse HandleSettingsPushPut_(const AnsiString &requestBody);
      static HttpResponse HandleSettingsPushVapid_();
      static AnsiString PreferencesJson_(__int64 accountId);
      // One preference read or written from another route (RestApiPreferences.cpp).
      // The read is false when the query failed - which is not the same as no
      // row: a route that treats a failed read as "nothing chosen" would have
      // the page act on a choice nobody made - and true with the empty string
      // when there is none. The write is false when it failed; full says
      // whether that was the hundred-preference cap, which is the reader's
      // to answer (400) rather than the server's (500).
      static bool ReadPreference_(__int64 accountId, const String &name, String &value);
      static bool WritePreference_(__int64 accountId, const String &name, const String &value, bool &full);
      // Offline in the webmail: the reader's window under the administrator's
      // ceiling, and the listing the page fills its store from (RestApiOffline.cpp).
      static HttpResponse HandleMeOffline_(const Caller &caller, RouteKind kind, const AnsiString &query, const AnsiString &requestBody);
      static AnsiString OpenApiOfflinePaths_();

      // Archive, Junk and Trash as actions: a designated folder made on demand,
      // spamd taught by a move into or out of Junk, a Junk or Trash folder
      // emptied; the message's raw headers, its Authentication-Results
      // verdicts and whether its sender is external; the file itself.
      static std::shared_ptr<IMAPFolder> DesignatedFolderOrCreate_(std::shared_ptr<const Account> account, int designation, const String &name);
      static void LearnAfterMove_(std::shared_ptr<IMAPFolder> source, std::shared_ptr<IMAPFolder> destination, __int64 newMessageId, std::shared_ptr<const Account> account);
      static HttpResponse HandleMeFolderEmpty_(const Caller &caller, __int64 folderId, const AnsiString &query);
      static HttpResponse HandleMeMessageSource_(const Caller &caller, __int64 messageId);
      static HttpResponse HandleMeMessageHtml_(const Caller &caller, __int64 messageId, const AnsiString &query);
      static bool HtmlNamesRemoteContent_(const AnsiString &html);
      static AnsiString HeaderFieldsJson_(const String &fileName, std::shared_ptr<const Account> account);
      static AnsiString OwnAuthenticationResults_(const MimeHeader &mimeHeader);

      // The account's app passwords (RestApiAppPasswords.cpp), its browser
      // sessions, and what its mailbox holds by folder and by message.
      static HttpResponse HandleMeAppPasswords_(const Caller &caller);
      static HttpResponse HandleMeAppPasswordCreate_(const Caller &caller, const AnsiString &request);

      // The account holder, proven again for a route that mints or revokes a
      // credential: the account's own password in the body's "password" -
      // never an app password - and, when a second factor is enrolled, the
      // code in X-hMailServer-OTP, as HandleMePassword_ asks. False with the
      // refusal in failure. RefuseAppPasswordCaller_ is the first of its
      // checks on its own, for the routes that ask no password but must not
      // be reached on one that was issued to a program.
      // The account's own password in the body, and its second factor, for a
      // route that has to know the person is the account holder rather than a
      // program they once gave a password to. what names the act in the log and
      // in the refusal - "making an app password", "setting a recovery address".
      //
      // recoveryCodeId, when given, is the recovery code that stood in for the
      // second factor: MATCHED and not spent, because the act this proves can
      // still be refused afterwards - a password the policy will not have, an
      // address that is not one - and a refusal must not cost the account one
      // of its ten. The handler calls SpendSecondFactor_ on the path that
      // succeeded, and nowhere else.
      static bool ConfirmAccountPassword_(const Caller &caller, const AnsiString &request, HttpResponse &failure,
                                          const char *what = "making an app password",
                                          __int64 *recoveryCodeId = nullptr);
      static bool RefuseAppPasswordCaller_(const Caller &caller, const char *what, HttpResponse &failure);
      static HttpResponse HandleMeAppPasswordDelete_(const Caller &caller, __int64 id);
      static AnsiString AppPasswordJson_(std::shared_ptr<AppPassword> password, const String &clearText);
      static HttpResponse HandleMeSessions_(const Caller &caller);
      static HttpResponse HandleMeSessionsEnd_(const Caller &caller);
      static HttpResponse HandleMeSessionDelete_(const Caller &caller, const AnsiString &id);

      // The clients that have signed in to the account, and the one press that
      // ends one. The same two things an administrator reaches under an
      // address in RestApiAccountResources.cpp; the answers are built by
      // SignInStore so that the two listings cannot describe a device
      // differently.
      static HttpResponse HandleMeDevices_(const Caller &caller);
      static HttpResponse HandleMeDeviceDelete_(const Caller &caller, const AnsiString &id, const AnsiString &query);

      // S/MIME (RestApiSmime.cpp): the key store, the chain check, the module, a page-built send.
      static HttpResponse HandleMeSmime_(const Caller &caller);
      static HttpResponse HandleMeSmimeOwnPut_(const Caller &caller, const AnsiString &requestBody);
      static HttpResponse HandleMeSmimeOwnDelete_(const Caller &caller, const AnsiString &fingerprint);
      static HttpResponse HandleMeSmimeRecipientPut_(const Caller &caller, const AnsiString &requestBody);
      static HttpResponse HandleMeSmimeRecipientDelete_(const Caller &caller, const AnsiString &fingerprint);
      static HttpResponse HandleMeSmimeChain_(const AnsiString &requestBody);
      static HttpResponse HandlePortalSmimeScript_();

      // The documentation (RestApiHelp.cpp, HelpPages.h): the list, one page,
      // and the webmail's markdown renderer. All three unauthenticated, as
      // /deck-lang/ is - the text is the repository's public documentation.
      static HttpResponse HandleHelpList_(const AnsiString &request);
      static HttpResponse HandleHelpPage_(const AnsiString &name, const AnsiString &request);
      static HttpResponse HandlePortalMarkdownScript_();
      static AnsiString OpenApiHelpPaths_();

      // OpenPGP in the webmail (RestApiOpenPgpKeys.cpp): the account's key
      // ring, the lookup a browser cannot make for itself, and the module the
      // page does the cryptography in.
      static HttpResponse HandleMeOpenPgpKeys_(const Caller &caller);
      static HttpResponse HandleMeOpenPgpKeyPut_(const Caller &caller, bool own, const AnsiString &requestBody);
      static HttpResponse HandleMeOpenPgpKeyDelete_(const Caller &caller, bool own, const AnsiString &fingerprint);
      static HttpResponse HandleMeOpenPgpLookup_(const Caller &caller, const AnsiString &query);
      // PUT .../keys/{id}/verified and .../keys/{id}/accepted, one boolean
      // each: the two things only the account holder can say about a
      // correspondent's key. verifiedRoute picks which.
      static HttpResponse HandleMeOpenPgpKeyTrust_(const Caller &caller, bool verifiedRoute, __int64 id, const AnsiString &requestBody);
      static HttpResponse HandlePortalOpenPgpScript_();
      static AnsiString OpenApiOpenPgpKeysPaths_();

      // Zero-access mailboxes (RestApiZeroAccess.cpp, Common/Util/ZeroAccess.h).
      static HttpResponse HandleMeZeroAccess_(const Caller &caller);
      static HttpResponse HandleMeZeroAccessPut_(const Caller &caller, const AnsiString &requestBody);
      static HttpResponse HandleMeZeroAccessRewrap_(const Caller &caller, const AnsiString &requestBody);
      static HttpResponse HandleMeZeroAccessDelete_(const Caller &caller);
      static AnsiString ZeroAccessJson_(std::shared_ptr<const Account> account);

      // Message recall within the server (RestApiMessageRecall.cpp).
      static HttpResponse HandleMeRecall_(const Caller &caller, const AnsiString &requestBody);
      static AnsiString OpenApiMessageRecallPaths_();
      // A password change for a zero-access mailbox carries the key sealed
      // again under the new password, or is refused: 0 to go ahead, else the
      // status and a sentence. rewrapped is written after the password is.
      static int ZeroAccessPasswordCheck_(std::shared_ptr<const Account> account, const AnsiString &requestBody, ZeroAccess::Row &rewrapped, AnsiString &problem);
      // True, with the 403 in refusal, when the account is a zero-access
      // mailbox whose mail an administrator asked to read or export.
      static bool RefuseZeroAccessRead_(std::shared_ptr<const Account> account, HttpResponse &refusal);
      static AnsiString OpenApiZeroAccessPaths_();
      // The Autocrypt header value a send may carry, checked before it goes
      // into a message: 1 good, 0 absent, -1 refused with the reason.
      static int AutocryptHeaderValue_(const AnsiString &requestBody, String &value, AnsiString &problem);
      static int WriteMimeEntity_(std::shared_ptr<const Account> account, const String &fromHeader, const String &toHeader, const String &ccHeader, const AnsiString &requestBody, const String &entityText, const String &autocrypt, const String &fileName, AnsiString &problem);
      static HttpResponse HandleMeStorage_(const Caller &caller);

      // OpenPGP public keys (RestApiOpenPgp.cpp): the account's own, an
      // administrator's for any account, and a domain's whole listing. The
      // serving of them is on the web services listener, not here.
      static HttpResponse HandleMeOpenPgp_(const Caller &caller);
      static HttpResponse HandleMeOpenPgpPut_(const Caller &caller, const AnsiString &requestBody);
      static HttpResponse HandleMeOpenPgpDelete_(const Caller &caller, const AnsiString &fingerprint);
      static HttpResponse HandleMeOpenPgpPublish_(const Caller &caller, const AnsiString &requestBody);
      static HttpResponse HandleAccountOpenPgp_(const AnsiString &address);
      static HttpResponse HandleAccountOpenPgpPut_(const Caller &caller, const AnsiString &address, const AnsiString &requestBody);
      static HttpResponse HandleAccountOpenPgpDelete_(const AnsiString &address, const AnsiString &fingerprint);
      static HttpResponse HandleDomainOpenPgpList_(const AnsiString &domainName);
      static bool SetAccountOpenPgpPublish_(__int64 accountId, bool publish, AnsiString &problem);
      static AnsiString OpenApiOpenPgpPaths_();

      // A read receipt for a message that asked for one, an unsubscribe by the
      // list's own method, and the queueing they share (RestApiMessageReplies.cpp).
      static HttpResponse HandleMeMessageReceipt_(const Caller &caller, __int64 messageId);
      static HttpResponse HandleMeMessageUnsubscribe_(const Caller &caller, __int64 messageId);
      static HttpResponse HandleMeSubscriptions_(const Caller &caller, const AnsiString &query);
      static HttpResponse HandleMeBrandLogo_(const AnsiString &query);
      HttpResponse HandleMeFetchAccounts_(const Caller &caller, RouteKind kind, __int64 fetchAccountId, const AnsiString &requestBody);
      // Importing from the old provider with a sign-in (ReaderOAuth.h): what is
      // offered, the start that hands the page the provider's address and the
      // browser its cookie, and the unauthenticated callback the provider sends
      // the browser back to. Not static: the cookie is Secure over TLS.
      HttpResponse HandleMeFetchImport_(const Caller &caller, RouteKind kind, const AnsiString &requestBody);
      // Addresses elsewhere, proved with a code and then sent as (RestApiSenderAddresses.cpp).
      static HttpResponse HandleMeSenderAddresses_(const Caller &caller, RouteKind kind, __int64 id, const AnsiString &requestBody);
      static AnsiString OpenApiSenderAddressesPaths_();
      // Masked addresses, the reader's own (RestApiMaskedAddresses.cpp).
      static HttpResponse HandleMeMaskedAddresses_(const Caller &caller, RouteKind kind, __int64 id, const AnsiString &requestBody);
      static AnsiString OpenApiMaskedAddressesPaths_();
      // The recovery address and the one-time recovery codes
      // (RestApiAccountRecovery.cpp). The three unauthenticated pages are not
      // static, because a refusal on one of them counts against the source
      // address the way a failed logon does.
      static HttpResponse HandleMeRecovery_(const Caller &caller, RouteKind kind, const AnsiString &request);
      static HttpResponse HandleAccountRecoveryClear_(const Caller &caller, const String &address);
      static HttpResponse HandleAccountRecoveryCodesClear_(const Caller &caller, const String &address);
      static HttpResponse HandleRecoveryVerifyPage_(const AnsiString &method, const AnsiString &query);
      static HttpResponse HandlePasswordResetRequest_(const AnsiString &requestBody, const IPAddress &peer_address);
      static HttpResponse HandlePasswordResetComplete_(const AnsiString &requestBody, const IPAddress &peer_address);
      static HttpResponse HandlePasswordResetPage_(const AnsiString &method, const AnsiString &query,
                                                   const AnsiString &requestBody, const IPAddress &peer_address);
      static AnsiString OpenApiRecoveryPaths_();
      static AnsiString RecoveryCodesLeftJson_(const Caller &caller);
      // 429 for a route that would have made the server post a recovery message
      // to an address that has had one too recently, in the shape the
      // sender-address proof answers the same refusal in.
      static HttpResponse TooManyRecoveryMessages_(const String &problem, int retryAfter);
      // The second factor a route asks for, or a recovery code in its place.
      // recoveryCodeId comes back as the row of the code that MATCHED - nothing
      // is spent here, so that a refusal further on in the handler leaves the
      // code where it was. SpendSecondFactor_ is the other half, called once the
      // act has actually happened.
      static bool SecondFactorProved_(std::shared_ptr<const Account> account, const AnsiString &request,
                                      __int64 *recoveryCodeId = nullptr);
      static void SpendSecondFactor_(std::shared_ptr<const Account> account, __int64 recoveryCodeId);
      // Deleted items still within the recovery window (RestApiRecoverable.cpp).
      static HttpResponse HandleMeRecoverable_(const Caller &caller, RouteKind kind, __int64 id);
      static AnsiString OpenApiRecoverablePaths_();

      // Passkeys (RestApiPasskeys.cpp): the four routes above, the
      // unauthenticated sign-in challenge, and the assertion Authenticate_
      // judges in place of a password.
      static HttpResponse HandlePasskeys_(const Caller &caller, RouteKind kind, __int64 id, const AnsiString &request);
      HttpResponse HandlePasskeyChallenge_(const AnsiString &request);
      static bool AuthenticatePasskey_(const AnsiString &request, const IPAddress &peer_address, Caller &caller);
      static bool ConfirmAdministratorPassword_(const Caller &caller, const AnsiString &request, HttpResponse &failure);
      static bool PasskeyOwner_(const Caller &caller, __int64 &accountId, bool &administrator, String &owner);
      static AnsiString PasskeyChallengeKey_(const Caller &caller);
      static AnsiString OpenApiPasskeyPaths_();

      // Single sign-on (RestApiOidcSignIn.cpp): the two pre-authentication
      // steps of the flow, what the sign-in pages may offer, and the ticket
      // Authenticate_ redeems in place of a password.
      HttpResponse HandleOidcStart_(const AnsiString &query);
      HttpResponse HandleOidcCallback_(const AnsiString &request, const AnsiString &query);
      static HttpResponse HandleSignInMethods_();
      static bool AuthenticateOidcTicket_(const AnsiString &request, const IPAddress &peer_address, Caller &caller);
      static AnsiString OpenApiOidcPaths_();

      // The organisation's directory (RestApiDirectory.cpp): the reader's own
      // domain, the reader's own entry, one entry's photo, and the
      // administrator's view of one account's entry.
      static HttpResponse HandleMeDirectory_(const Caller &caller, RouteKind kind, __int64 id, const AnsiString &query, const AnsiString &requestBody);
      static HttpResponse HandleAccountDirectory_(const Caller &caller, const String &address, RouteKind kind, const AnsiString &requestBody);
      static AnsiString DirectoryEntryJson_(const DirectoryEntry &entry);
      static AnsiString DirectoryRecordJson_(const DirectoryRecord &record, bool withNote);
      static bool ReadDirectoryBody_(const AnsiString &requestBody, bool allowNote, bool allowOptOut, bool allowFields,
                                     DirectoryRecord &record, String &problem);
      static AnsiString OpenApiDirectoryPaths_();
      // Messages that expire, the sender's side, and the reading page a
      // recipient's notice links to (RestApiConfidential.cpp). The page is not
      // static: the grant cookie it sets is Secure over TLS.
      static HttpResponse HandleMeConfidential_(const Caller &caller, RouteKind kind, __int64 id);
      HttpResponse HandleConfidentialPage_(const AnsiString &method, const AnsiString &token, const AnsiString &request, const IPAddress &peer_address);
      static AnsiString OpenApiConfidentialPaths_();
      // A password-protected message to anyone (RestApiProtectedMessages.cpp,
      // ProtectedMessages.h): the sender's four routes, the reader's fetch and
      // reply - no credential, the token is the capability - and the reading
      // page with its script, static as the portal's scripts are.
      static HttpResponse HandleMeProtected_(const Caller &caller);
      static HttpResponse HandleMeProtectedCreate_(const Caller &caller, const AnsiString &requestBody);
      static HttpResponse HandleMeProtectedContent_(const Caller &caller, __int64 id, const AnsiString &query, const AnsiString &body);
      static HttpResponse HandleMeProtectedDelete_(const Caller &caller, __int64 id);
      static HttpResponse HandleProtectedFetch_(const AnsiString &token, const IPAddress &peer_address);
      static HttpResponse HandleProtectedReply_(const AnsiString &token, const AnsiString &requestBody, const IPAddress &peer_address);
      static HttpResponse HandleProtectedPage_();
      static HttpResponse HandlePortalProtectedScript_();
      static AnsiString OpenApiProtectedPaths_();
      HttpResponse HandlePortalOAuthCallback_(const AnsiString &request, const AnsiString &query);
      static AnsiString OpenApiReaderFetchAccountsPaths_();
      static AnsiString OpenApiReaderFetchImportPaths_();
      static AnsiString BrandJson_(const AnsiString &brand);
      static int QueueRawMessage_(std::shared_ptr<const Account> account, const String &toAddress, const AnsiString &rawText, AnsiString &problem);

      // Later: a draft sent at a time, a message snoozed until a time, what is
      // scheduled and its cancelling, the administrator's run-now; a folder as
      // mbox and a message into a folder (RestApiScheduled.cpp).
      static HttpResponse HandleMeDraftSchedule_(const Caller &caller, __int64 messageId, const AnsiString &requestBody);
      static HttpResponse HandleMeDraftUnschedule_(const Caller &caller, __int64 messageId);
      static HttpResponse HandleMeMessageSnooze_(const Caller &caller, __int64 messageId, const AnsiString &requestBody);
      static HttpResponse HandleMeScheduled_(const Caller &caller);
      static HttpResponse HandleMeScheduledCancel_(const Caller &caller, __int64 schedId);
      static HttpResponse HandleScheduledRun_();
      static HttpResponse HandleMeFolderExport_(const Caller &caller, __int64 folderId);
      static HttpResponse HandleMeExportArchive_(const Caller &caller);
      static HttpResponse HandleMeFolderImport_(const Caller &caller, __int64 folderId, const AnsiString &requestBody);
      static AnsiString ThreadFieldsJson_(const String &fileName);
      static AnsiString ListingExtrasJson_(const String &fileName, std::shared_ptr<Message> message);
      static String FromHeader_(std::shared_ptr<const Account> account);
      static int AddAttachmentsFromJson_(MessageData &messageData, const AnsiString &requestBody, AnsiString &error);
      static bool IsLargeRequest_(const AnsiString &method, const AnsiString &target);
      static bool ReadJsonHex4_(const AnsiString &json, int at, unsigned int &value);
      static void AppendUtf8_(AnsiString &out, unsigned int codePoint);
      static void CollectReadableFolders_(std::shared_ptr<const Account> account, std::shared_ptr<IMAPFolders> folders, const String &parentPath,
                                          const String &delimiter, std::vector<std::pair<std::shared_ptr<IMAPFolder>, String>> &out, int depth);
      // UTF-8 for the JSON: the plain String-to-AnsiString conversion is the
      // system code page, which is not what a JSON reader expects.
      static AnsiString Utf8_(const String &value);
      static AnsiString Utf8_(const AnsiString &value) { return value; }
      static AnsiString Utf8_(const char *value) { return AnsiString(value); }
      static bool DeleteOwnMessage_(std::shared_ptr<const Account> account, std::shared_ptr<Message> message, std::shared_ptr<IMAPFolder> folder);
      static bool AuthenticateSession_(const AnsiString &request, const IPAddress &peer_address, Caller &caller);
      // One session token, however it arrived - the cookie's value, or the
      // bearer form the webmail holds for a second account - looked up,
      // policed and written onto the caller. The form it was made in is the
      // form it is accepted in and no other.
      static bool AuthenticateSessionToken_(const AnsiString &token, bool presentedAsBearer, const IPAddress &peer_address, Caller &caller);
      HttpResponse HandleSessionCreate_(const Caller &caller, const AnsiString &request, const AnsiString &query);
      HttpResponse HandleSessionDelete_(const Caller &caller);
      static void RevokeSessionsForAccount_(__int64 accountId, const AnsiString &keepTokenHash);
      static void ClearBrowserSessions_();
      static HttpResponse HandlePortalScript_();
      static HttpResponse HandlePortalManifest_();
      static HttpResponse HandlePortalServiceWorker_();

      // What the webmail says it is (RestApiBranding.cpp), and an
      // administrator opening a mailbox as its user, with consent and a record.
      static AnsiString BrandingJson_(const String &domain);
      static HttpResponse HandlePortalBranding_(const AnsiString &query);
      static HttpResponse HandlePortalBrandingPut_(const AnsiString &requestBody);
      HttpResponse HandleAccountSupportSession_(const Caller &caller, const AnsiString &address, const AnsiString &request);

      // The webmail's catalogues (RestApiLanguages.cpp).
      static HttpResponse HandlePortalLanguage_(const AnsiString &code);
      static AnsiString PortalLanguageJson_(const AnsiString &code);
      static AnsiString NegotiatePortalLanguage_(const AnsiString &request);

      // Files sent as links (RestApiFiles.cpp).
      static HttpResponse HandleMeFiles_(const Caller &caller);
      static HttpResponse HandleMeFileCreate_(const Caller &caller, const AnsiString &requestBody);
      static HttpResponse HandleMeFileContent_(const Caller &caller, __int64 id, const AnsiString &query, const AnsiString &body);
      static HttpResponse HandleMeFileUpdate_(const Caller &caller, __int64 id, const AnsiString &requestBody);
      static HttpResponse HandleMeFileDelete_(const Caller &caller, __int64 id);
      static HttpResponse HandlePublicFile_(const AnsiString &method, const AnsiString &token, const AnsiString &body);
      static HttpResponse HandlePortalFilesPolicy_(const AnsiString &query);
      static HttpResponse HandlePortalFilesPolicyPut_(const AnsiString &requestBody);
      static bool IsDomainAllowed_(const std::vector<String> &domains, const String &domainName);

      static Caller Authenticate_(const AnsiString &request, const IPAddress &peer_address);

      // True for POST /api/v1/session and nothing else, read from the request
      // line alone. Authenticate_ asks it before it will look at a passkey or a
      // single sign-on ticket in a body, so that neither is ever a credential
      // for any other route - the route table has not been consulted yet at
      // that point, and "the body says so" must not be enough.
      static bool IsSessionCreateRequest_(const AnsiString &request);

      // The two gates an account has to pass when it has proved who it is
      // WITHOUT a password - a passkey assertion, a ticket from the
      // organisation's identity provider - which is the same shape of
      // credential as a client certificate or a Kerberos ticket and gets the
      // same treatment those get: the compromised-account hold
      // (AccountLogon::RefusedByHold) and the domain's country policy. A
      // password sign-in meets both inside AccountLogon::Logon and never
      // reaches here; a credential that skipped them would be a way past a
      // lock every other way in obeys. True when the account may start a
      // session, and the last-logon stamp and the server's authentication
      // counter are moved on here, where the success is known. The account is
      // dropped when it is held, so the caller cannot go on using it.
      static bool AccountMayStartSession_(std::shared_ptr<const Account> &account, const IPAddress &peer_address);
      static bool AuthenticateBearer_(const AnsiString &token, const IPAddress &peer_address, Caller &caller);
      // The Basic credential is the administrator password, and - once a second
      // factor is enrolled on it - a code in the X-hMailServer-OTP request header
      // as well. The two code outcomes are told apart because only one of them is
      // a guess: a missing code is a client that has not been told yet, a wrong one
      // counts towards the per-address auto-ban like any other wrong credential.
      enum BasicResult
      {
         BasicRefused,
         BasicAccepted,
         BasicCodeMissing,
         BasicCodeWrong
      };

      static BasicResult AuthenticateBasic_(const AnsiString &encodedCredentials, const AnsiString &request);

      static AnsiString GetAuthorizationHeader_(const AnsiString &request);
      static AnsiString GetHeader_(const AnsiString &request, const AnsiString &lowerCaseName);
      // suppressChallenge leaves out WWW-Authenticate, for a request a page's
      // own script made: see the definition, and IsPageScriptRequest_ below.
      static HttpResponse BuildUnauthorizedResponse_(bool secondFactorRequired, bool suppressChallenge);
      static bool IsPageScriptRequest_(const AnsiString &request);
      static HttpResponse BuildForbiddenResponse_(const AnsiString &reason);
      static HttpResponse BuildTooManyRequestsResponse_();

      // Per-credential request budget. Returns false when this request is over
      // it; firstRefusal is set only for the request that crosses the line, so
      // that a flood costs one log line per window rather than one per request.
      static bool IsWithinRequestRate_(const AnsiString &identity, bool &firstRefusal);
      static void ClearRequestRates_();

      // Records a rejected credential so that Configuration's auto-ban
      // machinery counts it, exactly as the SMTP/IMAP/POP3 front ends do.
      static void RegisterAuthenticationFailure_(const IPAddress &peer_address);

      // The refused-source set: the in-process, bounded, self-expiring record
      // of addresses whose credentials have already tripped the auto-ban.
      // Security ranges are only ever consulted through
      // TCPConnection::GetSecurityRange(), which this listener does not use, so
      // without this the ban it creates would be inert here - and creating a
      // ban per three requests while never honouring one turns a credential
      // flood on this port into database load that every mail connection pays
      // for. See the block comment above the definitions in the .cpp.
      HttpResponse HandleSettingsComplaints_();
      HttpResponse HandleSettingsComplaintsPut_(const AnsiString &requestBody);
      static bool IsRefusedAddress_(const IPAddress &peer_address);
      static void RefuseAddress_(const IPAddress &peer_address);
      static void ClearRefusedAddresses_();

      // API key store.
      static std::vector<ApiKeyRecord> LoadKeys_();
      static bool IsExpired_(const String &expires);
      static bool IsSourceAllowed_(const String &allowed_from, const IPAddress &peer_address);

      static HttpResponse HandleListApiKeys_();
      static HttpResponse HandleCreateApiKey_(const AnsiString &requestBody);
      static HttpResponse HandleRevokeApiKey_(const AnsiString &id);

      // extraHeaders, when it is not empty, must be complete header lines each
      // ending in CRLF. Only the 429 uses it (Retry-After).
      static HttpResponse BuildResponse_(int statusCode, const AnsiString &body, const AnsiString &extraHeaders = "");
      // The Control Deck: the page at /, and its catalogues at
      // /deck-lang/<code>.json, both files of <ProgramFolder>/WebAdmin.
      static HttpResponse HandleWebAdminPage_();
      static HttpResponse HandleWebAdminLanguage_(const AnsiString &code);
      static bool ReadWebAdminFile_(const String &relativePath, AnsiString &body);
      static HttpResponse HandleStatus_();

      // allowedDomains empty means every domain; otherwise the listing is
      // filtered to those, so a key issued for one customer is not handed the
      // names of all the others.
      static HttpResponse HandleListDomains_(const std::vector<String> &allowedDomains);

      // One domain as the listing renders it, so that what a create or an
      // update answers is exactly what the next listing will show.
      static AnsiString DomainEntryJson_(const std::shared_ptr<Domain> &domain);

      // That same entry for one domain by name, and a 404 when this server
      // hosts no such domain.
      static HttpResponse HandleGetDomain_(const String &domainName);
      static HttpResponse HandleCreateDomain_(const AnsiString &requestBody);
      static HttpResponse HandleUpdateDomain_(const Caller &caller, const String &domainName, const AnsiString &requestBody);
      static HttpResponse HandleDeleteDomain_(const String &domainName);
      static HttpResponse HandleListAccounts_(const String &domainName);
      static HttpResponse HandleCreateAccount_(const Caller &caller, const String &domainName, const AnsiString &requestBody);

      // RestApiRoutes.cpp, beside the account update. The listing's entry is
      // what a spreadsheet export writes (address, active, max_size_mb,
      // first_name, last_name, admin_level); the create's body is read as a
      // typed document with every field named, so what an import sends is
      // either stored or refused by name - never dropped. On false, refusal
      // holds the answer to send.
      static AnsiString AccountListEntryJson_(const std::shared_ptr<Account> &account);
      static bool ReadNewAccount_(const Caller &caller, const AnsiString &requestBody,
         std::shared_ptr<Account> account, String &password, HttpResponse &refusal);
      static HttpResponse HandleDeleteAccount_(const String &address);
      static HttpResponse HandleListQueue_();
      static HttpResponse HandleQueueRetry_(__int64 messageId);
      static HttpResponse HandleQueueDelete_(__int64 messageId);
      static HttpResponse HandleQueueShaping_();
      static HttpResponse HandleQueueShapingClear_(const String &destination);
      static HttpResponse HandleTlsa_();

      // Ready-to-publish client-discovery SRV records (RFC 6186 / RFC 8314,
      // plus the Outlook _autodiscover convention), derived from the ports
      // that are actually configured and enabled. allowedDomains filters the
      // per-domain records exactly as HandleListDomains_ filters the domain
      // listing, and for the same reason.
      static HttpResponse HandleSrv_(const std::vector<String> &allowedDomains);
      static HttpResponse HandleMetricsHistory_(const AnsiString &query);
      // The update check's verdict, and a check run now. Both server-wide.
      static HttpResponse HandleUpdateGet_();
      static HttpResponse HandleUpdateCheck_();
      static HttpResponse HandleUpdateDownload_();
      static HttpResponse HandleUpdateInstall_();
      static HttpResponse HandleDiagnosticsRun_(const AnsiString &requestBody);
      static AnsiString QueryParameter_(const AnsiString &query, const AnsiString &name);

      HttpResponse HandleListQuarantine_();
      HttpResponse HandleQuarantineRelease_(__int64 id);
      HttpResponse HandleQuarantineDelete_(__int64 id);
      HttpResponse HandleListAliases_(const String &domainName);
      HttpResponse HandleListIpRanges_();
      HttpResponse HandleCreateIpRange_(const AnsiString &requestBody);
      HttpResponse HandleDeleteIpRange_(__int64 rangeId);
      HttpResponse HandleListLists_(const String &domainName);
      HttpResponse HandleCreateList_(const String &domainName, const std::vector<String> &callerDomains, const AnsiString &requestBody);
      HttpResponse HandleDeleteList_(const String &address);
      HttpResponse HandleListCertificates_();
      HttpResponse HandleDkim_(const String &domainName);
      HttpResponse HandleDkimRotation_(const Caller &caller, const Route &route, const AnsiString &requestBody);
      static AnsiString OpenApiDkimRotationPaths_();

      HttpResponse HandleReputation_(const Caller &caller, const Route &route, const AnsiString &requestBody);
      static AnsiString OpenApiReputationPaths_();

      // Compromised-account detection (RestApiAccountReview.cpp): what is held
      // for review, releasing or discarding one, and an account's review state.
      HttpResponse HandleAccountReview_(const Caller &caller, const Route &route, const AnsiString &requestBody);
      static AnsiString OpenApiAccountReviewPaths_();

      HttpResponse HandleSetupChecklist_(const Route &route);
      static AnsiString OpenApiSetupChecklistPaths_();

      // The sample domain (RestApiSampleDomain.cpp): its state, making it and
      // removing it.
      HttpResponse HandleSampleDomain_(const Route &route);
      static AnsiString OpenApiSampleDomainPaths_();
      HttpResponse HandleListRules_(__int64 accountId, __int64 domainId = 0);
      HttpResponse HandleListLogs_();
      HttpResponse HandleLogTail_(const AnsiString &name, const AnsiString &query);

      // Follow one connection live (RestApiSessionFollow.cpp): the sessions
      // open now and lately ended, and one session's lines after a cursor,
      // waited for when there are none yet.
      HttpResponse HandleLogSessions_();
      HttpResponse HandleLogSessionFollow_(const Route &route);
      static AnsiString OpenApiSessionFollowPaths_();
      HttpResponse HandleBackupStart_();
      HttpResponse HandleBackupStatus_();
      HttpResponse HandleBackupRestore_(const AnsiString &requestBody);
      // One mailbox or one message from an archive (RestApiBackupMailbox.cpp).
      HttpResponse HandleBackupArchiveList_();
      HttpResponse HandleBackupMailboxList_(const Route &route, const AnsiString &request);
      HttpResponse HandleBackupMailboxRestore_(const Route &route, const AnsiString &requestBody);
      HttpResponse HandleBackupMailboxRestoreStatus_(const Route &route);
      // Its two helpers: a value as a quoted JSON string, and a refusal body.
      static AnsiString BackupJsonQuoted_(const String &value);
      static HttpResponse BackupRefusal_(int statusCode, const String &reason);
      HttpResponse HandleSettings_();
      HttpResponse HandleServerReinitialize_();
      static void ReinitializeAfterTheAnswer_();

      // GET, POST and DELETE /api/v1/server/drain: the state, entering
      // maintenance mode, leaving it. One handler, because the three answer the
      // same document.
      HttpResponse HandleServerDrain_(RouteKind kind);

      // GET /api/v1/server/settings/pending: every [Settings] key marked
      // reinitialize or restart whose stored value is not the running one, as
      // [{name, stored, running, applies, secret}]. RestApiSettings.cpp.
      HttpResponse HandleServerSettingsPending_();
      static AnsiString DrainStatusJson_(const std::vector<String> &notReopened);
      static std::atomic<bool> reinitialize_pending_;

      // Wave 162: the write surface. Each group lives in its own translation
      // unit beside this one - RestApiSettings.cpp, RestApiRules.cpp,
      // RestApiCertificates.cpp, RestApiRoutes.cpp - and each contributes its
      // paths to the OpenAPI document through the OpenApi*Paths_ function,
      // which returns either nothing or a run of entries each beginning with
      // a comma, appended after the last path HandleOpenApi_ writes itself.
      HttpResponse HandleSettingsPut_(const Caller &caller, const AnsiString &requestBody);
      HttpResponse HandleSettingsAntiSpam_();
      HttpResponse HandleSettingsAntiSpamPut_(const AnsiString &requestBody);
      HttpResponse HandleSettingsLogging_();
      HttpResponse HandleSettingsLoggingPut_(const AnsiString &requestBody);
      HttpResponse HandleSettingsAntiVirus_();
      HttpResponse HandleSettingsAntiVirusPut_(const AnsiString &requestBody);
      static AnsiString OpenApiSettingsPaths_();
      // The Linux suite's route backlog, wave A: the directories the server
      // runs in, the [Settings] section of hMailServer.ini, and the
      // logon-failure list - what Settings.Directories, GetIniSetting /
      // SetIniSetting / DeleteIniSetting / IniSettingNames and
      // ClearLogonFailureList answer over COM. All in RestApiSettings.cpp.
      HttpResponse HandleSettingsDirectories_();
      HttpResponse HandleIniSettingList_();
      HttpResponse HandleIniSettingGet_(const AnsiString &name);
      HttpResponse HandleIniSettingPut_(const AnsiString &name, const AnsiString &requestBody);
      HttpResponse HandleIniSettingDelete_(const AnsiString &name);
      // What this server has changed from stock: every setting whose value is
      // not the one this version ships with (RestApiSettings.cpp), which is what
      // Settings.GetChangedFromDefault answers over COM, from the same function.
      HttpResponse HandleSettingsChanged_();
      HttpResponse HandleLogonFailuresClear_();
      // Wave E: the scripting and backup groups, the server messages and the
      // Sieve evaluation (RestApiSettings.cpp).
      HttpResponse HandleSettingsScripting_();
      HttpResponse HandleSettingsScriptingPut_(const AnsiString &requestBody);
      HttpResponse HandleScriptingReload_();
      HttpResponse HandleScriptingCheck_();
      // POST /api/v1/settings/breach-check/test: Settings.TestBreachCheckEndpoint.
      HttpResponse HandleBreachCheckTest_(const AnsiString &requestBody);
      // Settings.Cache and Settings.MessageIndexing over REST (RestApiSettings.cpp).
      HttpResponse HandleSettingsCache_();
      HttpResponse HandleSettingsCachePut_(const AnsiString &requestBody);
      HttpResponse HandleCacheClear_();
      HttpResponse HandleSettingsIndexing_();
      HttpResponse HandleSettingsIndexingPut_(const AnsiString &requestBody);
      HttpResponse HandleIndexingIndex_();
      HttpResponse HandleIndexingClear_();
      HttpResponse HandleSettingsBackup_();
      HttpResponse HandleSettingsBackupPut_(const AnsiString &requestBody);
      HttpResponse HandleSettingsBounces_();
      HttpResponse HandleSettingsBouncesPut_(const AnsiString &requestBody);
      HttpResponse HandleSettingsOpenPgp_();
      HttpResponse HandleSettingsOpenPgpPut_(const AnsiString &requestBody);
      HttpResponse HandleServerMessageList_();
      HttpResponse HandleServerMessagePut_(const AnsiString &name, const AnsiString &requestBody);
      HttpResponse HandleSieveEvaluate_(const AnsiString &requestBody);
      HttpResponse HandleCalendarExpand_(const AnsiString &requestBody);
      HttpResponse HandleCreateRule_(__int64 accountId, const AnsiString &requestBody, __int64 domainId = 0);
      HttpResponse HandleUpdateRule_(__int64 accountId, __int64 ruleId, const AnsiString &requestBody, __int64 domainId = 0);
      HttpResponse HandleDeleteRule_(__int64 accountId, __int64 ruleId, __int64 domainId = 0);
      HttpResponse HandleAccountRules_(const Route &route, const AnsiString &requestBody);
      HttpResponse HandleDomainRules_(const Route &route, const AnsiString &requestBody);
      static AnsiString OpenApiRulesPaths_();
      static AnsiString OpenApiAccountRulesPaths_();
      static AnsiString OpenApiDomainRulesPaths_();
      HttpResponse HandleAccountExport_(const Route &route, const AnsiString &requestBody);
      HttpResponse HandleAccountExportArchive_(const Route &route);
      static HttpResponse ArchiveDownload_(std::shared_ptr<Account> account);
      HttpResponse HandleAddressErase_(const AnsiString &requestBody);
      HttpResponse HandleAdministratorSecondFactor_(const Route &route);
      static AnsiString OpenApiOperatorPaths_();
      // One rule as every rule route emits it: the listing, the create and
      // the replace answer with the same document (defined in RestApiRules.cpp).
      static AnsiString RuleEntryJson_(const std::shared_ptr<Rule> &rule);
      HttpResponse HandleCreateCertificate_(const AnsiString &requestBody);
      HttpResponse HandleDeleteCertificate_(__int64 certificateId);
      HttpResponse HandleListPorts_();
      HttpResponse HandleCreatePort_(const AnsiString &requestBody);
      HttpResponse HandleUpdatePort_(__int64 portId, const AnsiString &requestBody);
      HttpResponse HandleDeletePort_(__int64 portId);
      static AnsiString OpenApiCertificatesPaths_();
      HttpResponse HandleListRoutes_();
      HttpResponse HandleCreateRoute_(const AnsiString &requestBody);
      HttpResponse HandleUpdateRoute_(__int64 routeId, const AnsiString &requestBody);
      HttpResponse HandleDeleteRoute_(__int64 routeId);
      // The remote domain policies (RestApiRemoteDomains.cpp): what this server
      // will do when it talks to a named remote domain.
      HttpResponse HandleListRemoteDomains_();
      HttpResponse HandleCreateRemoteDomain_(const AnsiString &requestBody);
      HttpResponse HandleUpdateRemoteDomain_(__int64 policyId, const AnsiString &requestBody);
      HttpResponse HandleDeleteRemoteDomain_(__int64 policyId);
      // Which record GOVERNS a domain, which is not the record named by it.
      HttpResponse HandleEffectiveRemoteDomain_(const String &domainName);
      HttpResponse HandleClearRemoteDomainVerificationCache_();
      static AnsiString OpenApiRemoteDomainsPaths_();
      HttpResponse HandleCreateAlias_(const String &domainName, const AnsiString &requestBody);
      HttpResponse HandleDeleteAlias_(const String &address);
      HttpResponse HandleUpdateAccount_(const Caller &caller, const String &address, const AnsiString &requestBody);
      HttpResponse HandleGetAccount_(const String &address);
      // The external (fetch) accounts of an account, under its address
      // (RestApiFetchAccounts.cpp): what Account.FetchAccounts does over COM.
      HttpResponse HandleListFetchAccounts_(const String &address);
      HttpResponse HandleCreateFetchAccount_(const String &address, const AnsiString &requestBody);
      HttpResponse HandleGetFetchAccount_(const String &address, __int64 fetchAccountId);
      HttpResponse HandleUpdateFetchAccount_(const String &address, __int64 fetchAccountId, const AnsiString &requestBody);
      HttpResponse HandleDeleteFetchAccount_(const String &address, __int64 fetchAccountId);
      HttpResponse HandleDownloadFetchAccount_(const String &address, __int64 fetchAccountId);
      static AnsiString OpenApiFetchAccountsPaths_();
      // The domain whole, the domain aliases and the IP range update
      // (RestApiAdministration.cpp, which also defines DomainEntryJson_ and
      // HandleUpdateDomain_).
      HttpResponse HandleListDomainAliases_(const String &domainName);
      HttpResponse HandleCreateDomainAlias_(const String &domainName, const AnsiString &requestBody);
      HttpResponse HandleDeleteDomainAlias_(const String &domainName, const String &aliasName);
      HttpResponse HandleUpdateIpRange_(__int64 rangeId, const AnsiString &requestBody);
      static AnsiString IpRangeEntryJson_(const std::shared_ptr<SecurityRange> &range);
      // A range's expiry as the body spells it - YYYY-MM-DD HH:MM:SS on the
      // server's clock, or a date alone for midnight - as a DateTime; false
      // for anything else (RestApiAdministration.cpp, used by the create in
      // RestApiServer.cpp as well).
      static bool ParseExpiryTime_(const String &text, DateTime &out);
      // The distribution list's create and update, and the entry every list
      // route answers with (RestApiAdministration.cpp); the listing and the
      // delete stay in RestApiServer.cpp.
      HttpResponse HandleUpdateList_(const String &address, const std::vector<String> &callerDomains, const AnsiString &requestBody);
      static AnsiString ListEntryJson_(const std::shared_ptr<DistributionList> &list);
      static AnsiString OpenApiAdministrationPaths_();

      // Who a distribution list delivers to now - its rows and what its
      // membership query answers at the moment of the call
      // (RestApiDistributionLists.cpp).
      HttpResponse HandleListMembers_(const String &address, const std::vector<String> &callerDomains);
      static AnsiString OpenApiDistributionListMembersPath_();
      // The six small collections - DNS black lists, SURBL servers,
      // white-list addresses, blocked senders, incoming relays and the
      // greylisting white list - as resources (RestApiAntiSpamLists.cpp): one
      // entry point for the twenty-four routes, the kind saying which
      // collection and which verb.
      HttpResponse HandleAntiSpamLists_(RouteKind kind, __int64 id, const AnsiString &requestBody);
      static AnsiString OpenApiAntiSpamListsPaths_();
      // The per-domain greylisting bypass list as a resource
      // (RestApiGreylistBypass.cpp), in the same shape.
      HttpResponse HandleGreylistBypass_(RouteKind kind, __int64 id, const AnsiString &requestBody);
      static AnsiString OpenApiGreylistBypassPaths_();
      // The blocked attachments as a resource (RestApiBlockedAttachments.cpp),
      // in the same shape: one entry point for the four routes.
      HttpResponse HandleBlockedAttachments_(RouteKind kind, __int64 id, const AnsiString &requestBody);
      static AnsiString OpenApiBlockedAttachmentsPaths_();
      // The journaling and dual-delivery rules (RestApiJournalRules.cpp), in
      // the same shape: one entry point for the four routes.
      HttpResponse HandleJournalRules_(RouteKind kind, __int64 id, const AnsiString &requestBody);
      static AnsiString OpenApiJournalRulePaths_();
      // The sender list at all three of its scopes (RestApiSenderLists.cpp):
      // one entry point, the route saying which scope and which verb, and the
      // caller carrying the account for the /me routes.
      HttpResponse HandleSenderLists_(const Route &route, const Caller &caller, const AnsiString &requestBody);
      static AnsiString OpenApiSenderListPaths_();

      // Spam traps and the record of what has addressed them
      // (RestApiSpamTraps.cpp).
      HttpResponse HandleSpamTraps_(const Route &route, const Caller &caller, const AnsiString &requestBody);
      static AnsiString OpenApiSpamTrapPaths_();
      static AnsiString TrapJson_(__int64 id, const String &address, const String &localPart, const String &created);

      // The domain white and black list as a resource
      // (RestApiDomainPolicyList.cpp): one entry point for its four routes.
      HttpResponse HandleDomainPolicyList_(RouteKind kind, __int64 id, const AnsiString &requestBody);
      static AnsiString OpenApiDomainPolicyListPaths_();
      // Whether the quarantine digest reaches the signed-in account, read and
      // written by its owner (RestApiSenderLists.cpp).
      HttpResponse HandleMeQuarantineDigest_(const Caller &caller, const AnsiString &method, const AnsiString &requestBody);
      // The two pages a digest's links lead to, which carry no credentials at
      // all: the capability is the token (RestApiQuarantineDigest.cpp).
      static HttpResponse HandleDigestRelease_(const AnsiString &method, const AnsiString &query, const AnsiString &body);
      static HttpResponse HandleDigestStop_(const AnsiString &method, const AnsiString &query, const AnsiString &body);
      // An account's resources under its address, administered
      // (RestApiAccountResources.cpp): the app passwords, the folders and
      // their ACLs, the messages. One entry point,
      // the route saying which resource and which verb; the parser for what
      // follows the address in the path, so that ParseRoute_ splits the
      // address off once and this unit owns its own tails.
      static bool ParseAccountResourceRoute_(const AnsiString &method, const AnsiString &tail, Route &route);
      HttpResponse HandleAccountResources_(const Route &route, const AnsiString &requestBody);
      static AnsiString OpenApiAccountResourcesPaths_();
      // The account groups as a resource (RestApiGroups.cpp): one entry point
      // for the eight routes, the route saying which verb, its record_id the
      // group and its member_account_id the member.
      HttpResponse HandleGroups_(const Route &route, const AnsiString &requestBody);
      static AnsiString OpenApiGroupsPaths_();

      // Roles: who may administer what (RestApiRoles.cpp).
      HttpResponse HandleRoleList_();
      HttpResponse HandleRoleGet_(const AnsiString &name);
      HttpResponse HandleRoleSave_(const AnsiString &name, const AnsiString &requestBody, bool creating);
      HttpResponse HandleRoleDelete_(const AnsiString &name);
      HttpResponse HandleRoleAssignmentList_(const Caller &caller);
      HttpResponse HandleRoleAssign_(const Caller &caller, const AnsiString &address, const AnsiString &requestBody);
      HttpResponse HandleRoleUnassign_(const Caller &caller, const AnsiString &address);
      HttpResponse HandleSessionGet_(const Caller &caller);
      HttpResponse HandleAccountPasswordReset_(const Caller &caller, const AnsiString &address, const AnsiString &requestBody);
      HttpResponse HandleAccountUnlock_(const Caller &caller, const AnsiString &address);
      HttpResponse HandleMessageTrace_(const Caller &caller, RouteKind kind, const AnsiString &identifier);
      // A mailbox renamed or moved to another domain (RestApiMailboxMove.cpp):
      // what Account.MoveTo does over COM.
      HttpResponse HandleMailboxMoveGet_(const AnsiString &address);
      HttpResponse HandleMailboxMove_(const Caller &caller, const AnsiString &address, const AnsiString &requestBody);
      static AnsiString OpenApiMailboxMovePaths_();
      static AnsiString RoleJson_(const RoleRecord &record);
      static AnsiString OpenApiRolesPaths_();
      // A rule criterion tried against a value (RestApiRules.cpp): what
      // Utilities.CriteriaMatch answers over COM.
      HttpResponse HandleRuleMatch_(const AnsiString &requestBody);
      static AnsiString OpenApiRoutesPaths_();
      HttpResponse HandleArchiveSearch_(const std::vector<String> &domains, const AnsiString &query);
      HttpResponse HandleArchiveGet_(const std::vector<String> &domains, __int64 archiveId);
      HttpResponse HandleArchiveHold_(const std::vector<String> &domains, __int64 archiveId, bool hold);
      static bool GetJsonBoolValue_(const AnsiString &json, const AnsiString &key, bool defaultValue);
      static std::vector<AnsiString> GetJsonStringArray_(const AnsiString &json, const AnsiString &key);
      static bool IsSafeLogName_(const AnsiString &name);

      // The reports (RestApiReports.cpp). One handler for every section,
      // because they share a window, a domain filter and a CSV writer and
      // differ only in what they count. domains is the caller's restriction:
      // empty means every domain, and a non-empty list confines the per-domain
      // sections to it the way the domain listing is confined.
      static HttpResponse HandleReport_(const std::vector<String> &domains, const AnsiString &section, const AnsiString &query);

      // Whether a section is built from the server-wide metric history and so
      // cannot be narrowed to a domain. Read by Authorize_, which is where
      // that refusal is made.
      static bool IsServerWideReportSection_(const AnsiString &section);
      static AnsiString OpenApiReportsPaths_();

      HttpResponse HandleOpenApi_();

      // True if the id names a message that is really in the delivery queue.
      static bool QueueMessageExists_(__int64 messageId);

      static AnsiString GetRequestBody_(const AnsiString &request);
      static AnsiString GetJsonStringValue_(const AnsiString &json, const AnsiString &key);
      static const JsonValue *FindJsonMember_(const JsonValue &object, const std::string &key);
      static AnsiString JsonEscape_(const AnsiString &value);

      // The audit trail's actor for one authorised request: the credential that
      // was authenticated, the address it came from, and the role it holds -
      // its kind read from the grant, not from AdminLevel. Installed once, in
      // ProcessRequest_, for the length of the dispatch - see AuditTrail.h.
      static AuditTrail::Actor AuditActorFor_(const Caller &caller);

      // GET /api/v1/audit, /api/v1/audit/verify and the alert rules and events.
      // In RestApiAudit.cpp.
      HttpResponse HandleAudit_(const Route &route, const AnsiString &requestBody);
      static AnsiString OpenApiAuditPaths_();

      // The suppression list, the bounce counts, classify and the sweep. In
      // RestApiBounces.cpp.
      HttpResponse HandleBounces_(const Route &route, const AnsiString &requestBody);
      HttpResponse HandleDomainSessionPolicy_(const Route &route);
      HttpResponse HandleSessionPolicy_();
      static AnsiString OpenApiBouncesPaths_();

      // The webhook subscriptions and their delivery queue. In
      // RestApiWebhooks.cpp.
      HttpResponse HandleWebhooks_(const Caller &caller, const Route &route, const AnsiString &requestBody);
      static AnsiString OpenApiWebhooksPaths_();

      // The complaints of one domain, every domain's rate, and the sweep. In
      // RestApiComplaints.cpp.
      HttpResponse HandleComplaints_(const Route &route, const AnsiString &requestBody);
      static AnsiString OpenApiComplaintsPaths_();

      // The DMARC aggregate and TLS-RPT reports read for a domain, and the
      // sweep. In RestApiDomainReports.cpp.
      HttpResponse HandleDomainReports_(const Route &route, const AnsiString &requestBody);
      static AnsiString OpenApiDomainReportsPaths_();

      std::shared_ptr<HttpServer> server_;
      bool running_;
      bool use_tls_;
      String certificate_file_;
      String private_key_file_;
   };
}
