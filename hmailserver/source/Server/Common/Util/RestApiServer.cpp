// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// REST administration API over HTTPS. See RestApiServer.h.

#include "StdAfx.h"

#include "RestApiServer.h"
#include "CountryBlocking.h"

#include "AuditTrail.h"
#include "../Application/RoleManager.h"
#include "DavSupport.h"
#include "HttpServer.h"
#include "HttpCompression.h"
#include "JsonDocument.h"
#include "Encoding/ModifiedUTF7.h"
#include <boost/thread/thread.hpp>
#include <boost/chrono.hpp>
#include <mutex>
#include "../Application/MetricsHistoryTask.h"
#include "ServerStatus.h"
#include "OtelTracer.h"
#include "OtelTraceContext.h"
#include "Crypt.h"
#include "Totp.h"
#include "AccountLogon.h"
#include "SignInStore.h"
#include "SessionFollow.h"
#include "SessionPolicies.h"
#include "PasswordPolicy.h"
#include "PasswordBreachCheck.h"
#include "PasswordHistory.h"
#include "../Cache/AccountSizeCache.h"
#include "AcmeClient.h"
#include "UpdateChecker.h"
#include "BrandLogo.h"
#include "../Diagnostics/Diagnostic.h"
#include "UpdateDownloader.h"
#include "UpdateInstaller.h"
#include "WebServicesServer.h"

// The Win32 profile API - GetPrivateProfileString and the three calls beside
// it - lives in kernel32 and has no POSIX equivalent, so the POSIX build
// declares those four functions here and implements them over the INI file
// itself in Common/Util/IniFile.cpp. The Windows build never reaches this line
// and binds the same calls to <windows.h> as it always has.
#ifdef HM_PLATFORM_POSIX
#include "IniFile.h"
#endif
#include "../AntiSpam/QuarantineStore.h"
#include "../AntiSpam/SpamAssassin/SpamAssassinLearner.h"
#include "../BO/IMAPFolders.h"
#include "../BO/IMAPFolder.h"
#include "../BO/Messages.h"
#include "../BO/MessageData.h"
#include "../BO/Attachments.h"
#include "../BO/Attachment.h"
#include "../Mime/Mime.h"
#include "../Application/ACLManager.h"
#include "../../IMAP/IMAPFolderContainer.h"
#include "../../IMAP/IMAPSpecialUse.h"
#include "../../IMAP/MessagesContainer.h"
#include "../../SMTP/RecipientParser.h"
#include "../../SMTP/RuleApplier.h"
#include "../Persistence/PersistentMessageIndex.h"
#include "../Cache/CacheContainer.h"
#include "../AntiSpam/AuthenticationResults.h"
#include "../AntiSpam/DMARC/DMARC.h"
#include "../Sieve/SieveStorage.h"
#include "../Sieve/SieveScript.h"
#include "GUIDCreator.h"
#include "InlineImageParts.h"
#include "RecoverableStore.h"
#include "AccountRecovery.h"
#include "PasswordValidator.h"
#include "PasskeyStore.h"
#include "OidcSignIn.h"
#include "CompromiseDetector.h"
#include "ConfidentialMessages.h"
#include "ContactGroups.h"
#include <iterator>
#include <list>
#include <set>
#include "../BO/MessageRecipients.h"
#include "../BO/MessageRecipient.h"
#include "../../SMTP/BounceTracker.h"
#include "Unicode.h"
#include "../Application/FolderManager.h"
#include "../BO/ACLPermission.h"
#include "../Tracking/ChangeNotification.h"
#include "../Tracking/NotificationServer.h"
#include "MessageUtilities.h"
#include "OutgoingFooters.h"
#include "../Application/Application.h"
#include "../BO/Aliases.h"
#include "../BO/Alias.h"
#include "../BO/SecurityRanges.h"
#include "../BO/SecurityRange.h"
#include "../BO/DistributionLists.h"
#include "../BO/DistributionList.h"
#include "../BO/DistributionListRecipients.h"
#include "../BO/DistributionListRecipient.h"
#include "../BO/Rules.h"
#include "../BO/Rule.h"
#include "../BO/RuleCriterias.h"
#include "../BO/RuleCriteria.h"
#include "../BO/RuleActions.h"
#include "../BO/RuleAction.h"
#include "../Persistence/PersistentSecurityRange.h"
#include "../Persistence/PersistentArchiveIndex.h"
#include "../Persistence/PersistentDistributionList.h"
#include "../Persistence/PersistentDistributionListRecipient.h"
#include "../Application/ObjectCache.h"
#include "../Application/BackupManager.h"
#include "../Application/Backup.h"
#include "../Application/BackupRestorer.h"
#include "Compression.h"
#include "../Application/IniFileSettings.h"
#include "../TCPIP/IPAddress.h"
#include "../../SMTP/SMTPConfiguration.h"
#include "../../IMAP/IMAPConfiguration.h"
#include "../../POP3/POP3Configuration.h"
#include "FileInfo.h"
#include <fstream>
#include "FileUtilities.h"
#include "Time.h"
#include "Encoding/Base64.h"
#include "Hashing/HashCreator.h"
#include "../Sieve/SieveParser.h"
#include "JsonDocument.h"

#include "../BO/Domains.h"
#include "../BO/Domain.h"
#include "../BO/Accounts.h"
#include "../BO/Account.h"
#include "../BO/SSLCertificates.h"
#include "../BO/SSLCertificate.h"
#include "../BO/TCPIPPort.h"
#include "../BO/TCPIPPorts.h"
#include "../BO/Message.h"
#include "../Persistence/PersistentAccount.h"
#include "../Persistence/PersistentMessage.h"
#include "../Persistence/PersistenceMode.h"
#include "../TCPIP/SocketConstants.h"
#include "../TCPIP/SslContextInitializer.h"
#include "../../SMTP/DeliveryQueue.h"

// <ws2tcpip.h> is Winsock's TCP/IP header. Everything this file takes from it -
// the address structures and the address-conversion calls - comes from
// <netinet/in.h>, <arpa/inet.h> and <netdb.h> on POSIX, which the platform layer
// has already included.
#ifdef _MSC_VER
#include <ws2tcpip.h>
#endif

#include <algorithm>
#include <cstring>
#include <ctime>
#include <mutex>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // size_t rather than int so that every comparison against a std::string
      // size below is between like types. /W3 /WX would otherwise turn one of
      // them into a signed/unsigned build failure the first time it is written
      // without a constant on the signed side.
      // The ceilings HttpServer enforces for this listener. Every one is
      // absolute - see HttpServer.cpp for why an idle timeout is not enough.
      const size_t MaxRequestSize = 64 * 1024;

      // The two routes that carry attachments: sixteen megabytes on the
      // wire, and five minutes for them to arrive.
      const size_t MaxLargeRequestSize = 16 * 1024 * 1024;
      const unsigned LargeRequestSeconds = 300;
      const unsigned RequestSeconds = 30;
      const unsigned ConnectionSeconds = 300;
      const unsigned MaxConnections = 64;
      const unsigned WorkerThreads = 4;

      // hm_messages.messagetype for a message held for ETRN. There is no
      // Message::State enumerator for it, but GET /api/v1/queue lists it
      // alongside the ordinary Message::Delivering rows, so the retry and
      // delete endpoints have to accept it too.
      const int EtrnHeldMessageType = 3;

      // The TLS context the HTTPS listener hands to HttpServer. Its configuration
      // comes from SslContextInitializer; the boost object owns the underlying
      // SSL_CTX and frees it in its destructor, so it has to outlive every session
      // made from it, and Stop() releases it only after the server has stopped.
      std::shared_ptr<boost::asio::ssl::context> tls_context_owner;

      // API key store layout. One section per key; the section suffix is the
      // key id, which is what DELETE /api/v1/apikeys/<id> revokes.
      const TCHAR *ApiKeySectionPrefix = _T("Key.");

      // Presented tokens carry this prefix so that a leaked string is
      // recognisable as an hMailServer API key (and so that an administrator
      // pasting a password by mistake fails fast).
      const char *ApiKeyTokenPrefix = "hmapi_";

      // 32 bytes of entropy in the secret; 8 in the id.
      const int ApiKeySecretBytes = 32;
      const int ApiKeyIdBytes = 8;

      // Length of a SHA-256 digest written as lower-case hex.
      const int ApiKeyHashHexLength = 64;

      // Used when a create request does not name an expiry. A key with no
      // expiry at all is exactly the property that makes the administrator
      // password dangerous, so "unlimited" is not an option.
      const int ApiKeyDefaultLifetimeDays = 90;

      // The only Scope value that widens a key beyond reading. Compared
      // case-insensitively; anything else - including nothing at all - leaves
      // the key read-only.
      const TCHAR *ApiKeyScopeFull = _T("full");
      const TCHAR *ApiKeyScopeReadOnly = _T("readonly");

      // The same two words narrow, for the JSON bodies (Format's %hs) and for
      // reading the create request. Spelled once each so the store, the request
      // and the response can never disagree about them.
      const char *ApiKeyScopeFullNarrow = "full";
      const char *ApiKeyScopeReadOnlyNarrow = "readonly";

      // ------------------------------------------------------------------
      // Per-credential request budget.
      //
      // The refused-source set below bounds *failed* authentication. Nothing
      // bounded successful requests at all: one valid credential could drive
      // this listener as fast as its single worker thread could answer, and
      // every request costs a key-store read plus, on most routes, queries on
      // the same database connection pool that SMTP, IMAP and POP3 use. So a
      // leaked key was not only administrative access, it was a lever on mail
      // delivery.
      //
      // Per credential rather than per source address, deliberately, and in
      // both directions:
      //
      //  - a key is the thing whose budget we want to cap, and an address is
      //    not: rotating source addresses must not multiply what one leaked key
      //    can spend;
      //  - two honest keys behind one NAT must not be able to starve each
      //    other, which is what a per-address budget would do.
      //
      // A fixed window, not a sliding one: when a window is older than
      // RateWindowMilliseconds its entry is dropped and the next request starts
      // a fresh one. That makes Retry-After exact - a caller that waits the
      // window out is certainly inside a new window - at the cost of allowing
      // up to twice the budget across a window boundary, which is the right
      // trade for a management API.
      //
      // A short window and a generous budget, on purpose. This is not a throttle
      // meant to shape normal use; it is a ceiling that stops one credential
      // turning into a denial of service against mail. Twenty requests a second
      // sustained is far more than any administrator or dashboard produces
      // (a status poll once a second is 5% of it) and far less than the thread
      // can serve, so what it removes is only the abusive case. The short window
      // also means an honest client that briefly overshoots is forgiven in
      // seconds rather than being locked out for a minute.
      //
      // Nothing here fires on the shipped default configuration: the listener
      // does not run unless RestApiPort is set.
      // ------------------------------------------------------------------
      const int MaxRequestsPerWindowPerCredential = 200;
      const ULONGLONG RateWindowMilliseconds = 10 * 1000;

      // Only the administrator can mint keys, so this cap is not reachable by
      // an attacker. It is here so that a store with thousands of keys cannot
      // turn this into unbounded memory.
      const size_t MaxRateLimitedCredentials = 64;

      struct CredentialRate
      {
         AnsiString identity;
         ULONGLONG window_started_at;
         int count;
      };

      std::mutex credential_rate_mutex;
      std::vector<CredentialRate> credential_rates;

      // ------------------------------------------------------------------
      // The refused-source set.
      //
      // An IP range created by auto-ban is only ever consulted through
      // TCPConnection::GetSecurityRange(). This listener uses raw sockets and
      // consults nothing, so a ban created from a REST authentication failure
      // would never refuse a single REST request.
      //
      // That is not just a missing feature, it is an amplifier. Every
      // MaxInvalidLogonAttempts failures (default 3) AccountLogon inserts
      // another hm_securityranges row, and GetIPRangeName_ pays a run of
      // PersistentSecurityRange::Exists SELECTs looking for a name nothing has
      // used yet. On the mail ports that self-limits, because the very next
      // connection from the banned address is refused before AUTH. Here
      // nothing stopped it: a sustained flood would keep the single REST
      // worker thread doing database round-trips and would grow
      // hm_securityranges at a third of the request rate for AutoBanMinutes.
      // PersistentSecurityRange::ReadMatchingIP is an uncached SELECT on every
      // SMTP, IMAP and POP3 connection, so that growth is paid by every mail
      // connection on the shared database. Slowing mail down is worse than the
      // brute-force attempt being reacted to.
      //
      // So the 'disconnect' flag AccountLogon returns - set exactly when this
      // failure was the one that tripped the ban - is recorded here, and Run_
      // refuses the address before the request is read, before the TLS
      // handshake, and before anything touches the database.
      //
      // Deliberately in-process and deliberately small:
      //
      //  - Bounded at MaxRefusedAddresses. When it is full the entry that
      //    expires soonest is replaced, so an attacker rotating source
      //    addresses churns the set rather than growing it, and the addresses
      //    kept are the ones that offended most recently.
      //  - Every entry carries its own deadline and expired entries are dropped
      //    whenever the set is walked, so it empties itself even if no further
      //    request ever arrives, and nothing here can refuse an administrator
      //    for longer than RefusedAddressMinutes. An entry in force cannot be
      //    renewed either: while an address is refused its connections are
      //    closed before authentication, so no further failure is registered
      //    for it and no later deadline can be written. The window is a
      //    ceiling, not a rolling one.
      //  - A fixed short window rather than AutoBanMinutes. It only has to be
      //    long enough to collapse a flood - one ban per address per window
      //    instead of one per three requests - and an administrator who
      //    mistyped a password should not lose the management interface for the
      //    hour that the shipped AutoBanMinutes grants. It also means
      //    AutoBanMinutes=0 ("disconnect, but do not block") is honoured as a
      //    brief refusal rather than ignored, which is what stops the flood of
      //    logon-failure rows that setting would otherwise still pay for.
      //
      // Nothing here fires on the shipped default configuration: the listener
      // does not run unless RestApiPort is set, and an address only enters the
      // set after it has failed authentication MaxInvalidLogonAttempts times.
      // ------------------------------------------------------------------
      const int RefusedAddressMinutes = 5;
      const ULONGLONG RefusedAddressMilliseconds = static_cast<ULONGLONG>(RefusedAddressMinutes) * 60 * 1000;
      const size_t MaxRefusedAddresses = 256;

      // The address is held as its numeric halves rather than as text, so that
      // the check on the accept path allocates nothing at all.
      struct RefusedAddress
      {
         int family;
         unsigned __int64 address_high;
         unsigned __int64 address_low;
         ULONGLONG expires_at;
      };

      std::mutex refused_addresses_mutex;
      std::vector<RefusedAddress> refused_addresses;

      // Browser sessions for the self-service page and for the Control Deck.
      // One row per sign-in, holding the SHA-256 of the cookie's token and who
      // it stands for - an account, or the administrator; the token itself
      // lives only in the browser. Bounded in number and in time (see
      // AuthenticateSession_), and dropped on Stop() - a restarted listener
      // holds no sessions.
      struct BrowserSession
      {
         BrowserSession() : account_id(0), administrator(false), created_at(0), last_seen_at(0) { }

         AnsiString token_hash;
         __int64 account_id;

         // The administrator's session: account_id is 0 and credential_stamp
         // is the SHA-256 of the stored administrator password hash as it was
         // at sign-in, so that a change of the administrator password ends the
         // session on its next request, as a change of an account's password
         // ends that account's other sessions. A hash of a hash: the table
         // still holds nothing that logs anybody in.
         bool administrator;
         AnsiString credential_stamp;

         // The session was started with one of the account's app passwords:
         // every request it carries is as limited as that password is.
         bool via_app_password = false;

         // The token was answered in the body of the sign-in rather than in a
         // Set-Cookie, and is accepted as "Authorization: Bearer <token>" and
         // in no Cookie header - the form the webmail asks for when a person
         // adds a second account beside the one whose cookie the browser
         // already holds. A cookie session is not accepted as a bearer either:
         // a token is accepted in the one place it was made for. Never made
         // for the administrator, whose session is the Control Deck's cookie.
         bool bearer = false;

         ULONGLONG created_at;
         ULONGLONG last_seen_at;

         // Empty for the account's own sessions; the administrator's identity
         // when the session was opened for support.
         AnsiString support_by;

         // The client this session belongs to - SignInStore's device key, made
         // from the protocol, the address and the user agent - so that revoking
         // a device in the account's list ends its browser session as well as
         // dropping its mail connections, and so that the session list and the
         // device list name the same thing.
         AnsiString device;

         // The address the session began on, for the bind-to-address policy, and
         // the domain whose policy governs it (empty for the administrator's own
         // session, which belongs to no domain).
         AnsiString bound_address;
         String domain;

         // The policy as it stood when this session was last looked at. Written
         // by ApplyPolicy_ when the session is made and again at each of its
         // requests: an administrator who shortens an idle timeout means it for
         // the sessions that exist, not only for the next ones.
         ULONGLONG idle_milliseconds = 0;
         ULONGLONG lifetime_milliseconds = 0;
         bool bind_to_address = false;
      };

      const char *SessionCookieName = "hmailsession";
      const int SessionTokenBytes = 32;

      // The ceilings that apply where no policy has been set - which is every
      // server until an administrator sets one, and what this listener has always
      // enforced. Named once, in SessionPolicies, so that the policy resolution
      // and the sessions themselves cannot disagree about what "unset" means.
      const ULONGLONG SessionIdleMilliseconds = (ULONGLONG) SessionPolicies::DefaultIdleSeconds * 1000;
      const ULONGLONG SessionAbsoluteMilliseconds = (ULONGLONG) SessionPolicies::DefaultLifetimeSeconds * 1000;
      const size_t MaxBrowserSessions = 1000;

      std::mutex browser_sessions_mutex;
      std::vector<BrowserSession> browser_sessions;

      // The policy in force for one session, written onto it. Called under
      // browser_sessions_mutex; the resolution itself reads the settings map and
      // the domain cache, both in memory.
      void ApplyPolicy_(BrowserSession &session)
      {
         const SessionPolicy policy = session.domain.IsEmpty()
            ? SessionPolicies::ServerWide()
            : SessionPolicies::ForDomain(session.domain);

         session.idle_milliseconds = (ULONGLONG) policy.idle_seconds * 1000;
         session.lifetime_milliseconds = (ULONGLONG) policy.lifetime_seconds * 1000;
         session.bind_to_address = policy.bind_to_address;
      }

      // Whether a session has run out of time, by either of the two ceilings. A
      // session whose policy has never been applied - which cannot happen, but
      // would leave the fields at zero - falls back to the built-in ceilings
      // rather than to "never expires".
      bool HasExpired_(const BrowserSession &session, ULONGLONG now)
      {
         const ULONGLONG idle = session.idle_milliseconds > 0 ? session.idle_milliseconds : SessionIdleMilliseconds;
         const ULONGLONG lifetime = session.lifetime_milliseconds > 0 ? session.lifetime_milliseconds : SessionAbsoluteMilliseconds;

         return now - session.last_seen_at > idle || now - session.created_at > lifetime;
      }

      bool IsSameAddress(const RefusedAddress &entry, int family, unsigned __int64 high, unsigned __int64 low)
      {
         return entry.family == family && entry.address_high == high && entry.address_low == low;
      }

      AnsiString BytesToLowerHex(const unsigned char *data, int length)
      {
         AnsiString result;
         char buffer[3];

         for (int i = 0; i < length; i++)
         {
            sprintf_s(buffer, sizeof(buffer), "%02x", data[i]);
            result += buffer;
         }

         return result;
      }

      // True only for a lower-case hex string of exactly the expected length.
      // Guards the stored hash: a truncated or hand-mangled value must be
      // rejected outright rather than compared against.
      bool IsLowerHex(const AnsiString &value, int expectedLength)
      {
         if (value.GetLength() != expectedLength)
            return false;

         for (int i = 0; i < value.GetLength(); i++)
         {
            char character = value[i];
            bool isDigit = character >= '0' && character <= '9';
            bool isHexLetter = character >= 'a' && character <= 'f';

            if (!isDigit && !isHexLetter)
               return false;
         }

         return true;
      }

      // Constant-time equality. A byte-by-byte comparison that returns on the
      // first difference tells an attacker how much of a guessed token was
      // correct, which turns a 256-bit secret into a per-byte search.
      bool ConstantTimeEquals(const AnsiString &left, const AnsiString &right)
      {
         if (left.GetLength() != right.GetLength() || left.GetLength() == 0)
            return false;

         return CRYPTO_memcmp(left.c_str(), right.c_str(), (size_t) left.GetLength()) == 0;
      }

      // Lower-case hex SHA-256 of the presented token. See LoadKeys_ for why
      // this, rather than one of the password KDFs, is the correct primitive.
      AnsiString HashApiKeyToken(const AnsiString &token)
      {
         HashCreator hasher(HashCreator::SHA256);
         return hasher.GenerateHashNoSalt(token, HashCreator::hex);
      }

      // What an administrator session is checked against on every request: the
      // stored administrator password hash, hashed once more. Empty when no
      // administrator password is set, which is the state in which this API
      // answers nobody at all.
      AnsiString AdministratorCredentialStamp()
      {
         const AnsiString stored = AnsiString(IniFileSettings::Instance()->GetAdministratorPassword());
         if (stored.IsEmpty())
            return AnsiString();

         return HashApiKeyToken(stored);
      }

      // Every administrator session at once. Called when the credential they
      // were minted from is no longer the one in force. Takes the table's lock
      // itself.
      void RevokeAdministratorSessions()
      {
         std::lock_guard<std::mutex> guard(browser_sessions_mutex);

         browser_sessions.erase(
            std::remove_if(browser_sessions.begin(), browser_sessions.end(),
               [](const BrowserSession &session)
               {
                  return session.administrator;
               }),
            browser_sessions.end());
      }

      // Parses "<address>/<prefix>" into an inclusive address range. Handles
      // both families; returns false for anything that is not a valid CIDR.
      bool ParseCidr(const AnsiString &text, IPAddress &lower, IPAddress &upper)
      {
         int slashPosition = text.Find("/");
         if (slashPosition <= 0)
            return false;

         AnsiString addressPart = text.Mid(0, slashPosition);
         AnsiString prefixPart = text.Mid(slashPosition + 1);

         if (prefixPart.IsEmpty() || prefixPart.GetLength() > 3)
            return false;

         for (int i = 0; i < prefixPart.GetLength(); i++)
         {
            if (prefixPart[i] < '0' || prefixPart[i] > '9')
               return false;
         }

         int prefix = atoi(prefixPart.c_str());

         IPAddress base;
         if (!base.TryParse(addressPart, false))
            return false;

         if (base.GetType() == IPAddress::IPV4)
         {
            if (prefix > 32)
               return false;

            const unsigned __int64 all = 0xFFFFFFFFULL;

            unsigned __int64 mask = (prefix == 0) ? 0 : ((all << (32 - prefix)) & all);
            unsigned __int64 network = base.GetAddress1() & mask;

            lower = IPAddress(static_cast<__int64>(network));
            upper = IPAddress(static_cast<__int64>(network | (all & ~mask)));
            return true;
         }

         if (base.GetType() == IPAddress::IPV6)
         {
            if (prefix > 128)
               return false;

            const unsigned __int64 all = 0xFFFFFFFFFFFFFFFFULL;

            unsigned __int64 highMask = 0;
            unsigned __int64 lowMask = 0;

            // Shifting a 64-bit value by 64 is undefined, so the boundary
            // prefixes (0, 64 and 128) are spelled out rather than computed.
            if (prefix == 0)
            {
               highMask = 0;
               lowMask = 0;
            }
            else if (prefix < 64)
            {
               highMask = all << (64 - prefix);
               lowMask = 0;
            }
            else if (prefix == 64)
            {
               highMask = all;
               lowMask = 0;
            }
            else if (prefix < 128)
            {
               highMask = all;
               lowMask = all << (128 - prefix);
            }
            else
            {
               highMask = all;
               lowMask = all;
            }

            unsigned __int64 networkHigh = base.GetAddress1() & highMask;
            unsigned __int64 networkLow = base.GetAddress2() & lowMask;

            lower = IPAddress(static_cast<__int64>(networkHigh), static_cast<__int64>(networkLow));
            upper = IPAddress(static_cast<__int64>(networkHigh | ~highMask),
                              static_cast<__int64>(networkLow | ~lowMask));
            return true;
         }

         return false;
      }

      // Parses an API key's allowed-source restriction into an inclusive
      // address range. Accepted forms: a single address, "lower-upper", or
      // CIDR. Anything else is refused - a restriction we cannot understand
      // must not silently become "any source".
      bool ParseSourceRestriction(const AnsiString &text, IPAddress &lower, IPAddress &upper)
      {
         AnsiString restriction = text;
         restriction.Trim();

         if (restriction.IsEmpty())
            return false;

         if (restriction.Find("/") >= 0)
            return ParseCidr(restriction, lower, upper);

         int dashPosition = restriction.Find("-");
         if (dashPosition > 0)
         {
            AnsiString lowerText = restriction.Mid(0, dashPosition);
            AnsiString upperText = restriction.Mid(dashPosition + 1);

            lowerText.Trim();
            upperText.Trim();

            if (!lower.TryParse(lowerText, false) || !upper.TryParse(upperText, false))
               return false;

            return lower.GetType() == upper.GetType();
         }

         if (!lower.TryParse(restriction, false))
            return false;

         upper = lower;
         return true;
      }

      // Parses a strictly numeric message id.
      bool ParseQueueId(const AnsiString &value, __int64 &id)
      {
         if (value.IsEmpty() || value.GetLength() > 18)
            return false;

         for (int i = 0; i < value.GetLength(); i++)
         {
            if (value[i] < '0' || value[i] > '9')
               return false;
         }

         id = _atoi64(value.c_str());
         return id > 0;
      }

   }

   namespace
   {
      // The answers this listener lets HttpCompression compress: the documents
      // the server itself holds, which are the same bytes for every reader -
      // the webmail's page (its branding and catalogue are admin-set and
      // served to anyone), its script, its S/MIME module, its manifest, its
      // service worker and its language catalogues; the Deck's page and
      // catalogues; the OpenAPI description. Nothing built from the database:
      // not a mailbox listing, not a message, not a report, not an
      // administrator's trace. That is the BREACH line, argued at the top of
      // HttpCompression.cpp - text an attacker chose (a subject they sent)
      // compressed beside what the reader would keep from them (every other
      // subject in the listing), fetched again by the webmail on its own each
      // time a message arrives. Drawn here, once, rather than route by route,
      // so a route added later is plain until it is named.
      bool CompressibleRoute_(const AnsiString &target)
      {
         AnsiString path = target;
         const int query = path.Find("?");
         if (query >= 0)
            path = path.Mid(0, query);

         if (path == "/" || path == "/index.html")
            return true;
         if (path.StartsWith("/deck-lang/"))
            return true;
         if (path == "/portal" || path == "/portal.js" || path == "/portal-smime.js" ||
             path == "/portal.webmanifest" || path == "/portal-sw.js")
            return true;
         if (path.StartsWith("/portal-lang/"))
            return true;
         return path == "/api/v1/openapi.json";
      }

      // The per-request documents among those, compressed once per distinct
      // body and held: the webmail's page in each language, the Deck's page
      // and its catalogues from disk, the OpenAPI description. Capacity for
      // the page in every language the portal has and every catalogue of
      // both, with room over; at most some ten megabytes if every one is
      // asked for, and nothing until it is.
      HttpCompressedCache pageCache_(96);
   }

   HttpCompressedCache &
   PortalPageCache()
   {
      return pageCache_;
   }

   RestApiServer::RestApiServer() :
      running_(false),
      use_tls_(false)
   {

   }

   RestApiServer::~RestApiServer()
   {
      Stop();
   }

   bool
   RestApiServer::Start(const String &bind_address, int port, const String &certificate_file, const String &private_key_file)
   {
      if (running_)
         return true;

      if (IniFileSettings::Instance()->GetAdministratorPassword().IsEmpty())
      {
         LOG_APPLICATION("RestApi: Refusing to start - the administrator password is not set.");
         return false;
      }

      use_tls_ = !certificate_file.IsEmpty() && !private_key_file.IsEmpty();
      certificate_file_ = certificate_file;
      private_key_file_ = private_key_file;

      // ::1 carries exactly the guarantee 127.0.0.1 does - only a process on
      // this machine can connect - so it satisfies the TLS exemption on the
      // same grounds. Exact literals only, as before: this is a security gate,
      // and widening it (127/8, mapped forms) is a separate decision.
      bool isLoopback = bind_address == _T("127.0.0.1") || bind_address == _T("localhost") ||
                        bind_address == _T("::1");

      if (!use_tls_ && !isLoopback)
      {
         LOG_APPLICATION("RestApi: Refusing to start - TLS certificate is required unless bound to 127.0.0.1 or ::1. Set RestApiCertificateFile and RestApiPrivateKeyFile.");
         return false;
      }

      if (use_tls_)
      {
         // Why this goes through SslContextInitializer rather than building its own
         // SSL_CTX: that function is the single place the mail protocols get their
         // TLS configuration from - the cipher list, the enabled protocol versions,
         // the server-preference and ChaCha options, the DH parameters and the
         // key-exchange group list, which since August 2026 carries the hybrid
         // post-quantum KEMs from [Settings] TlsKeyExchangeGroups.
         //
         // What this listener did instead is worth stating, because it is the shape
         // the problem takes every time: it set a TLS 1.2 floor and loaded the
         // certificate, and then took OpenSSL's defaults for everything else. So the
         // configured cipher list did not apply to it, the option mask did not apply
         // to it, and the post-quantum groups did not apply to it - an administrator
         // who set TlsKeyExchangeGroups and then served the API over TLS got
         // classical-only key exchange, with nothing anywhere saying so.
         //
         // The bridge is deliberately thin: SslContextInitializer wants an
         // SSLCertificate, so one is built here from the two configured paths rather
         // than looked up in the database - the REST listener's certificate is
         // RestApiCertificateFile/RestApiPrivateKeyFile and is not one of the
         // certificates bound to a mailbox port. Nothing about the TLS configuration
         // is restated locally, so nothing local can drift from the mail protocols
         // again.
         try
         {
            auto certificate = std::shared_ptr<SSLCertificate>(new SSLCertificate());

            certificate->SetName(_T("RestApi"));
            certificate->SetCertificateFile(certificate_file);
            certificate->SetPrivateKeyFile(private_key_file);

            auto context = std::shared_ptr<boost::asio::ssl::context>(
               new boost::asio::ssl::context(boost::asio::ssl::context::sslv23));
            context->set_options(HM_TLS_CONTEXT_FLOOR);

            if (!SslContextInitializer::InitServer(*context, certificate, bind_address, port))
            {
               // InitServer has already reported the specific failure - an unreadable
               // certificate file, a private key that does not match - as HM5113.
               // This line records only the consequence for this listener.
               LOG_APPLICATION("RestApi: Refusing to start - the shared TLS configuration could not be applied to the configured certificate.");
               return false;
            }

            tls_context_owner = context;

            // The one thing that is deliberately *not* taken from the shared
            // configuration. This listener enforced a TLS 1.2 floor before, and the
            // shared option mask is driven by the [Settings] protocol toggles, which
            // an administrator may well have opened up to TLS 1.0 for an ancient mail
            // client. That argument does not extend to an HTTP API - there is no
            // 2008-era REST client to keep working - so the floor stays, applied after
            // InitServer so it can only tighten what the shared configuration allows.
            SSL_CTX_set_min_proto_version(context->native_handle(), TLS1_2_VERSION);
         }
         catch (...)
         {
            // Constructing the context can throw. This is the startup path, where an
            // escape would take the whole service down before any listener is up.
            LOG_APPLICATION("RestApi: Refusing to start - an exception was raised while preparing TLS.");
            return false;
         }
      }

      HttpLimits limits;
      limits.max_request_bytes = MaxRequestSize;
      limits.max_request_bytes_large = MaxLargeRequestSize;
      limits.request_seconds_large = LargeRequestSeconds;
      limits.request_seconds = RequestSeconds;
      limits.connection_seconds = ConnectionSeconds;
      limits.max_connections = MaxConnections;
      limits.worker_threads = WorkerThreads;

      std::shared_ptr<HttpServer> server(new HttpServer("RestApi", limits,
         [this](const HttpRequest &request)
         {
            HttpResponse response = ProcessRequest_(request.raw, request.peer);

            // Compression is for the server's own documents and nothing
            // built from the database: CompressibleRoute_ above says which,
            // and HttpCompression.cpp says why.
            if (!CompressibleRoute_(request.target))
               response.no_compress = true;

            return response;
         },
         [](const IPAddress &peer)
         {
            // An address that has already tripped the auto-ban is refused here:
            // before the request is read, before the TLS handshake, and - the
            // point of the exercise - before anything touches the database. A
            // refused connection is closed without a response, exactly as a
            // security range refuses an SMTP connection before its banner.
            if (IsRefusedAddress_(peer))
            {
               LOG_DEBUG("RestApiServer: Refused a connection from " + String(peer.ToString()) +
                  ", which has recently failed authentication repeatedly.");
               return false;
            }

            // The country policy for the HTTP listeners, asked in the same place
            // and closed the same way: no response, nothing said about why. The
            // administrator's line and the counter are CountryBlocking's.
            if (!CountryBlocking::Instance()->IsConnectionAllowed(CountryBlocking::SurfaceHttp, peer))
               return false;

            return true;
         },
         [](int status, const AnsiString &message)
         {
            // The server's own refusals - malformed, oversized, chunked, a
            // handler that threw - in the API's JSON shape. Says nothing about
            // the cap: an administrator hitting it reads the documentation, a
            // stranger measuring it learns nothing useful.
            AnsiString body;
            body.Format("{\"error\":\"%hs\"}", JsonEscape_(message).c_str());
            return BuildResponse_(status, body);
         }));

      server->SetLargeRequestFilter(&RestApiServer::IsLargeRequest_);

      if (!server->Listen(bind_address, port, use_tls_ ? tls_context_owner : std::shared_ptr<boost::asio::ssl::context>()))
      {
         tls_context_owner.reset();
         return false;
      }

      server_ = server;
      running_ = true;

      // A previous Stop left the session ring refusing to hold a follow open;
      // this listener can hold one again. Before the workers start, so no
      // request of this listener's can be refused a wait it should have had.
      SessionFollow::Instance()->AllowWaits();

      server_->Start();

      String message;
      message.Format(_T("RestApi: Listening on %s (%s)."), HttpServer::FormatEndpoint(bind_address, port).c_str(), use_tls_ ? _T("https") : _T("http, loopback only"));
      LOG_APPLICATION(message);

      return true;
   }

   void
   RestApiServer::Stop()
   {
      if (!running_)
         return;

      running_ = false;

      // A follow waiting for a session's next line holds a worker thread
      // until its wait ends; woken here, so the stop below never waits on a
      // follower's timer.
      SessionFollow::Instance()->CancelWaits();

      if (server_)
      {
         server_->Stop();
         server_.reset();
      }

      // After the stop, so there is no request left to race with. A stopped
      // listener holds no refusals and no request counts: whatever was in force
      // is dropped rather than surviving into the next Start().
      ClearRefusedAddresses_();
      ClearRequestRates_();
      ClearBrowserSessions_();

      // The challenges a passkey registration or sign-in was waiting on, the
      // sign-ins waiting to come back from the identity provider and the
      // tickets they made: all of them name a browser that was talking to a
      // listener that has stopped, and every one is answerable by pressing the
      // button again.
      PasskeyStore::Clear();
      OidcSignIn::Clear();

      // Deliberately no SSL_CTX_free: the boost context owns the SSL_CTX and frees it
      // in its own destructor. Released after the server has stopped, so no session
      // is still using it.
      tls_context_owner.reset();
   }

   AnsiString
   RestApiServer::GetAuthorizationHeader_(const AnsiString &request)
   {
      return GetHeader_(request, "authorization");
   }

   AnsiString
   RestApiServer::GetHeader_(const AnsiString &request, const AnsiString &lowerCaseName)
   {
      AnsiString lowerRequest = request;
      lowerRequest.MakeLower();

      const AnsiString needle = "\r\n" + lowerCaseName + ":";

      int headerPosition = lowerRequest.Find(needle);
      if (headerPosition < 0)
         return "";

      int valueStart = headerPosition + needle.GetLength();
      int lineEnd = request.Find("\r\n", valueStart);
      if (lineEnd < 0)
         return "";

      AnsiString headerValue = request.Mid(valueStart, lineEnd - valueStart);
      headerValue.Trim();

      return headerValue;
   }

   bool
   RestApiServer::IsSessionCreateRequest_(const AnsiString &request)
   {
      const int lineEnd = request.Find("\r\n");
      if (lineEnd <= 0)
         return false;

      const std::vector<AnsiString> parts = StringParser::SplitString(request.Mid(0, lineEnd), " ");
      if (parts.size() < 2 || parts[0] != "POST")
         return false;

      AnsiString path = parts[1];
      const int query = path.Find("?");
      if (query >= 0)
         path = path.Mid(0, query);

      return path == "/api/v1/session";
   }

   bool
   RestApiServer::AccountMayStartSession_(std::shared_ptr<const Account> &account, const IPAddress &peer_address)
   {
      if (!account || account->GetID() == 0 || !account->GetActive())
         return false;

      // The compromised-account detection's hold. A password logon meets it at
      // AccountLockout::IsLockedOut inside AccountLogon::Logon; a credential
      // that proves identity without one meets it here, exactly as a client
      // certificate does in ClientCertificateIdentity and a bearer token does
      // in IMAP, POP3 and SMTP. Without this, a takeover that had enrolled a
      // passkey while it still had the password would keep a way in after the
      // detector had locked the name, ended the sessions and held the mail.
      //
      // The per-name lockout that counts failed passwords is deliberately NOT
      // applied, for the reason AccountLogon.h gives for the other passwordless
      // credentials: it counts guesses, and neither an assertion signed by a
      // registered key nor a ticket this server minted two minutes ago is one.
      if (AccountLogon::RefusedByHold(account))
         return false;

      // The domain's country policy, so that which credential was used does not
      // decide whether a domain accepts a sign-in from where this one came
      // from. Logon makes the same call for every protocol; this is the one
      // path that does not go through Logon.
      if (!CountryBlocking::Instance()->IsLogonAllowed(account->GetAddress(), peer_address))
         return false;

      PersistentAccount::UpdateLastLogonTime(account);
      ServerStatus::Instance()->OnAuthenticationSucceeded();
      return true;
   }

   RestApiServer::Caller
   RestApiServer::Authenticate_(const AnsiString &request, const IPAddress &peer_address)
   {
      // Default-constructed: AuthenticationFailed, read-only, no domains. Every
      // early return below is therefore a refusal that grants nothing, and a
      // path that forgets to set the authority cannot accidentally widen one.
      Caller caller;
      caller.peer = peer_address;

      AnsiString headerValue = GetAuthorizationHeader_(request);
      if (headerValue.IsEmpty())
      {
         // A passkey, or a ticket from the organisation's identity provider,
         // standing where a password would: both arrive at POST
         // /api/v1/session with no Authorization header at all, and both are
         // judged here so that what follows - the rate window, Authorize_,
         // HandleSessionCreate_ and the policy it applies - is the same code
         // that runs for a password. Nothing else on this listener accepts
         // either, because nothing else looks at them.
         //
         // X-Requested-With, and it is not decoration. A credential read out of
         // the REQUEST BODY is the one credential a cross-site form can carry:
         // a password rides in an Authorization header another origin cannot
         // set, and a session cookie is SameSite=Strict, but a body is just a
         // body, and text/plain needs no preflight. Without this, a page on
         // another site could post an attacker's own single sign-on ticket and
         // silently replace the reader's session cookie with a session in the
         // attacker's mailbox - login forgery, whose cost is everything the
         // person writes or files afterwards. The header a browser will not add
         // across origins without a preflight this server never grants is the
         // proof that the post came from this server's own page, which is the
         // same proof the guard below demands of every session-authenticated
         // write. Both of this server's pages send it (Portal.js's call, the
         // Deck's api and its two sign-in fetches).
         if (IsSessionCreateRequest_(request) && GetHeader_(request, "x-requested-with") == "hMailServer")
         {
            if (AuthenticatePasskey_(request, peer_address, caller))
               return caller;
            if (AuthenticateOidcTicket_(request, peer_address, caller))
               return caller;
         }

         // No password and no key: a browser session cookie is the one other
         // credential, and it stands for an account and nothing more.
         AuthenticateSession_(request, peer_address, caller);
         return caller;
      }

      // Bearer is preferred when present. Basic is still accepted, unchanged,
      // because every existing script depends on it.
      // >= and not >, for both schemes below: a header value of exactly
      // "Bearer " or "Basic " is a presented credential that happens to be
      // empty, and it has to reach the failure path so that it is logged and
      // counted like any other. Requiring a byte after the space dropped it
      // silently instead.
      if (headerValue.GetLength() >= 7 && headerValue.Mid(0, 7).CompareNoCase("bearer ") == 0)
      {
         AnsiString token = headerValue.Mid(7);
         token.Trim();

         // A session token in the bearer form: 64 hex digits and nothing else,
         // which no API key is - every key begins with "hmapi_". It is the
         // second account of a webmail page, and it is judged exactly as the
         // same token in a Cookie header would be: looked up, policed for
         // idleness, lifetime and address, and dropped when it has expired.
         // Not counted as an authentication failure when it is not found, as
         // a cookie that has expired is not: a page whose second session ran
         // out is an ordinary event, not a guess at a credential, and counting
         // it would auto-ban the person's own address after three of them.
         if (IsLowerHex(token, SessionTokenBytes * 2))
         {
            AuthenticateSessionToken_(token, true, peer_address, caller);
            return caller;
         }

         if (!AuthenticateBearer_(token, peer_address, caller))
         {
            // Deliberately does not say whether the token was unknown, expired
            // or refused by source address - the caller gets one 401 either
            // way, and the log entry is for the administrator, not the client.
            LOG_APPLICATION("REST API: API key authentication failed from " + String(peer_address.ToString()) + ".");
            RegisterAuthenticationFailure_(peer_address);
         }

         return caller;
      }

      if (headerValue.GetLength() >= 6 && headerValue.Mid(0, 6).CompareNoCase("basic ") == 0)
      {
         AnsiString encodedCredentials = headerValue.Mid(6);
         encodedCredentials.Trim();

         // Any Basic user name but the administrator's is an account. The two
         // are told apart here, once, so that an account can never be tried
         // against the administrator password and the administrator's name can
         // never be tried against the accounts.
         {
            AnsiString decoded = Base64::Decode(encodedCredentials, encodedCredentials.GetLength());
            int separatorPosition = decoded.Find(":");
            if (separatorPosition > 0)
            {
               String basicUser = String(decoded.Mid(0, separatorPosition));
               if (basicUser.CompareNoCase(_T("administrator")) != 0)
               {
                  if (!AuthenticateAccount_(basicUser, String(decoded.Mid(separatorPosition + 1)), peer_address, request, caller))
                     RegisterAuthenticationFailure_(peer_address);
                  return caller;
               }
            }
         }

         const BasicResult basic = AuthenticateBasic_(encodedCredentials, request);

         if (basic == BasicAccepted)
         {
            // The administrator password is the full-authority credential and
            // always has been. It is not read-only and it is not restricted to
            // any domain: the scoping added for API keys narrows keys, and
            // narrowing this one would break every script that exists.
            caller.result = AuthenticatedAsAdministrator;
            caller.read_only = false;
            caller.identity = "administrator";
            caller.grant = RoleManager::ServerAdministrator();

            return caller;
         }

         if (basic == BasicCodeMissing)
         {
            // The password was right and no code came with it: a client that has
            // not been told a second factor is enrolled, not a guess. Said so in
            // the log (this is exactly when the administrator needs telling) and
            // in the response, and not counted towards the auto-ban.
            LOG_APPLICATION("REST API: the administrator password was accepted but a second factor is enrolled and no X-hMailServer-OTP header carried a code.");
            caller.second_factor_required = true;
            return caller;
         }

         // A rejected credential leaves a trace, so repeated guessing against an
         // exposed management port is at least visible to the administrator.
         // (Presenting no credential at all is normal for the login page and is
         // deliberately not logged here.) A wrong code is a guess at six digits
         // and is counted like any other wrong credential.
         if (basic == BasicCodeWrong)
         {
            LOG_APPLICATION("REST API: the administrator password was accepted but the one-time code in X-hMailServer-OTP was not.");
            caller.second_factor_required = true;
         }
         else
         {
            LOG_APPLICATION("REST API: administrator authentication failed.");
         }

         RegisterAuthenticationFailure_(peer_address);
      }

      return caller;
   }

   RestApiServer::BasicResult
   RestApiServer::AuthenticateBasic_(const AnsiString &encodedCredentials, const AnsiString &request)
   {
      // An empty value is refused before the decoder sees it, rather than
      // relying on what MimeCodeBase64 does with a zero-length input.
      if (encodedCredentials.IsEmpty())
         return BasicRefused;

      AnsiString credentials = Base64::Decode(encodedCredentials, encodedCredentials.GetLength());

      int separatorPosition = credentials.Find(":");
      if (separatorPosition <= 0)
         return BasicRefused;

      String username = credentials.Mid(0, separatorPosition);
      String password = credentials.Mid(separatorPosition + 1);

      if (username.CompareNoCase(_T("administrator")) != 0)
         return BasicRefused;

      String correctPassword = IniFileSettings::Instance()->GetAdministratorPassword();
      if (correctPassword.IsEmpty())
         return BasicRefused;

      Crypt::EncryptionType hashType = Crypt::Instance()->GetHashType(correctPassword);

      if (!Crypt::Instance()->Validate(password, correctPassword, hashType))
         return BasicRefused;

      // The second factor, once the password has been accepted - the same order
      // COMAuthentication uses, for the same reason: what is said about the code
      // is said only to somebody who holds the password.
      const String secret = IniFileSettings::Instance()->GetAdministratorTotpSecret();
      if (IniFileSettings::Instance()->IsAdministratorTotpEnrolled() && secret.IsEmpty())
      {
         // Enrolled but unopenable: refused, as COMAuthentication refuses, and
         // for the same reason - a lost key must not read as a second factor
         // switched off.
         ErrorManager::Instance()->ReportError(ErrorManager::High, 6417, "RestApiServer::AuthenticateBasic_",
            "Administrator logon refused: a second factor is enrolled but its secret cannot be opened on this machine (see the preceding secret-store error). Restore the key the secret was protected with, or clear AdministratorTotpSecret in hMailServer.ini and enrol again.");
         return BasicRefused;
      }
      if (secret.IsEmpty())
         return BasicAccepted;

      const AnsiString code = GetHeader_(request, "x-hmailserver-otp");
      if (code.IsEmpty())
         return BasicCodeMissing;

      return Totp::VerifyCode(AnsiString(secret), code) ? BasicAccepted : BasicCodeWrong;
   }

   bool
   RestApiServer::AuthenticateBearer_(const AnsiString &token, const IPAddress &peer_address, Caller &caller)
   {
      // Cheap syntactic rejection first, so that a flood of junk tokens does
      // not cause the store to be read at all.
      AnsiString prefix(ApiKeyTokenPrefix);

      if (token.GetLength() != prefix.GetLength() + ApiKeySecretBytes * 2)
         return false;

      if (token.Mid(0, prefix.GetLength()) != prefix)
         return false;

      if (!IsLowerHex(token.Mid(prefix.GetLength()), ApiKeySecretBytes * 2))
         return false;

      AnsiString presentedHash = HashApiKeyToken(token);
      if (presentedHash.IsEmpty())
         return false;

      std::vector<ApiKeyRecord> keys = LoadKeys_();

      // Every record is compared, with no early exit, so the work done is a
      // function of the number of stored keys only - never of how much of a
      // guessed token happened to be right.
      const ApiKeyRecord *matched = nullptr;

      for (const ApiKeyRecord &key : keys)
      {
         if (ConstantTimeEquals(presentedHash, key.hash) && matched == nullptr)
            matched = &key;
      }

      if (matched == nullptr)
         return false;

      // Expiry and source restriction are checked after the secret matched, and
      // both produce the same indistinguishable failure.
      if (IsExpired_(matched->expires))
      {
         String message;
         message.Format(_T("REST API: API key '%s' was presented after it expired (%s)."),
            matched->label.c_str(), matched->expires.c_str());
         LOG_APPLICATION(message);
         return false;
      }

      if (!IsSourceAllowed_(matched->allowed_from, peer_address))
      {
         String message;
         message.Format(_T("REST API: API key '%s' was presented from %s, which is outside its allowed source '%s'."),
            matched->label.c_str(), String(peer_address.ToString()).c_str(), matched->allowed_from.c_str());
         LOG_APPLICATION(message);
         return false;
      }

      // The key's authority travels with the request from here. Copied out of
      // the record rather than looked up again later, so that a store edited
      // between the authentication and the authorisation of one request cannot
      // change the answer half way through.
      caller.result = AuthenticatedWithApiKey;
      caller.identity = AnsiString("key:") + AnsiString(matched->id);
      caller.read_only = matched->read_only;
      caller.domains = matched->domains;

      // The key's role: its Role= line when it has one, and otherwise the
      // Scope it has carried since August 2026 - full is the server
      // administrator, read-only is the auditor. A key with neither is a key
      // that holds nothing, and fails closed exactly as it did.
      caller.grant = RoleManager::GrantForApiKey(matched->role, matched->read_only, matched->domains);

      return true;
   }

   bool
   RestApiServer::IsWithinRequestRate_(const AnsiString &identity, bool &firstRefusal)
   {
      firstRefusal = false;

      // Not reachable: every authenticated caller has an identity. Belt and
      // braces, because the alternative to returning true here would be an
      // unnamed credential sharing one budget with every other.
      if (identity.IsEmpty())
         return true;

      const ULONGLONG now = GetTickCount64();

      std::lock_guard<std::mutex> guard(credential_rate_mutex);

      for (std::vector<CredentialRate>::iterator it = credential_rates.begin(); it != credential_rates.end(); )
      {
         // Closed windows are dropped as they are met, so the set empties itself
         // even if no further request ever arrives.
         if (now - it->window_started_at >= RateWindowMilliseconds)
         {
            it = credential_rates.erase(it);
            continue;
         }

         if (it->identity == identity)
         {
            it->count++;

            // The refused requests are counted too. A caller that keeps pushing
            // stays refused for the rest of its window rather than being let
            // back in one request at a time.
            firstRefusal = it->count == MaxRequestsPerWindowPerCredential + 1;

            return it->count <= MaxRequestsPerWindowPerCredential;
         }

         ++it;
      }

      CredentialRate entry;
      entry.identity = identity;
      entry.window_started_at = now;
      entry.count = 1;

      if (credential_rates.size() < MaxRateLimitedCredentials)
      {
         credential_rates.push_back(entry);
         return true;
      }

      // Full. Replace the oldest window rather than growing, so this is a hard
      // bound on memory whatever the store contains.
      size_t oldest = 0;

      for (size_t i = 1; i < credential_rates.size(); i++)
      {
         if (credential_rates[i].window_started_at < credential_rates[oldest].window_started_at)
            oldest = i;
      }

      credential_rates[oldest] = entry;

      return true;
   }

   void
   RestApiServer::ClearRequestRates_()
   {
      std::lock_guard<std::mutex> guard(credential_rate_mutex);

      credential_rates.clear();
      credential_rates.shrink_to_fit();
   }

   bool
   RestApiServer::IsRefusedAddress_(const IPAddress &peer_address)
   {
      // Memory only: no allocation, no formatting, no file and no query, so a
      // refused connection costs a lock and a walk of at most
      // MaxRefusedAddresses entries. The lock is held across nothing but that
      // walk - never across the socket work the caller does afterwards.
      if (peer_address.IsAny())
         return false;

      const int family = static_cast<int>(peer_address.GetType());
      const unsigned __int64 high = peer_address.GetAddress1();
      const unsigned __int64 low = peer_address.GetAddress2();
      const ULONGLONG now = GetTickCount64();

      std::lock_guard<std::mutex> guard(refused_addresses_mutex);

      for (std::vector<RefusedAddress>::iterator it = refused_addresses.begin(); it != refused_addresses.end(); )
      {
         // Expired entries are dropped as they are met. Combined with the sweep
         // in RefuseAddress_ this is what makes the set self-emptying: an
         // administrator refused by mistake is let back in by the passage of
         // time alone, with no restart and no configuration change.
         if (now >= it->expires_at)
         {
            it = refused_addresses.erase(it);
            continue;
         }

         if (IsSameAddress(*it, family, high, low))
            return true;

         ++it;
      }

      return false;
   }

   void
   RestApiServer::RefuseAddress_(const IPAddress &peer_address)
   {
      // 0.0.0.0 is what a failed peer-address parse leaves behind. It is never
      // recorded, because it is not one host: refusing it would refuse every
      // connection whose address could not be read.
      if (peer_address.IsAny())
         return;

      const int family = static_cast<int>(peer_address.GetType());
      const unsigned __int64 high = peer_address.GetAddress1();
      const unsigned __int64 low = peer_address.GetAddress2();
      const ULONGLONG now = GetTickCount64();
      const ULONGLONG expiresAt = now + RefusedAddressMilliseconds;

      std::lock_guard<std::mutex> guard(refused_addresses_mutex);

      // Sweep first, so that the size cap below is only reached by addresses
      // that are genuinely still being refused.
      refused_addresses.erase(
         std::remove_if(refused_addresses.begin(), refused_addresses.end(),
            [now](const RefusedAddress &entry) { return now >= entry.expires_at; }),
         refused_addresses.end());

      for (RefusedAddress &entry : refused_addresses)
      {
         if (IsSameAddress(entry, family, high, low))
         {
            entry.expires_at = expiresAt;
            return;
         }
      }

      RefusedAddress entry;
      entry.family = family;
      entry.address_high = high;
      entry.address_low = low;
      entry.expires_at = expiresAt;

      if (refused_addresses.size() < MaxRefusedAddresses)
      {
         refused_addresses.push_back(entry);
         return;
      }

      // Full. Replace the entry closest to expiry rather than growing, so the
      // set is a hard-bounded amount of memory whatever an attacker does with
      // source addresses.
      size_t soonest = 0;

      for (size_t i = 1; i < refused_addresses.size(); i++)
      {
         if (refused_addresses[i].expires_at < refused_addresses[soonest].expires_at)
            soonest = i;
      }

      refused_addresses[soonest] = entry;
   }

   void
   RestApiServer::ClearRefusedAddresses_()
   {
      std::lock_guard<std::mutex> guard(refused_addresses_mutex);

      refused_addresses.clear();
      refused_addresses.shrink_to_fit();
   }

   void
   RestApiServer::RegisterAuthenticationFailure_(const IPAddress &peer_address)
   {
      // The same accounting the SMTP/IMAP/POP3 front ends use, so a brute-force
      // attempt against the management port is counted alongside one against
      // the mail ports and trips the same auto-ban.
      //
      // Loopback is excluded on purpose. Auto-ban creates an IP range at
      // priority 100, and a range covering 127.0.0.1 would deny the server's
      // own local clients, hMailAdmin and every local script - a self-inflicted
      // outage far worse than the brute-force attempt it was reacting to. An
      // attacker who is already on the loopback interface is not being kept out
      // by an IP ban in any case, and the listener refuses to run on a
      // non-loopback address without TLS, so remote attempts are still counted.
      if (peer_address.ToString() == "127.0.0.1" || peer_address.ToString() == "::1")
         return;

      if (peer_address.IsAny())
         return;

      try
      {
         AccountLogon accountLogon;
         bool disconnect = false;

         // The per-name lockout is deliberately not fed here. The credential this
         // listener accepts is the administrator password from the ini, not a
         // mailbox name, so "REST API" is a label for the per-IP accounting and
         // never a name anybody authenticates as - counting it could only lock a
         // string nobody uses, while logging a line that claims a control acted.
         // Brute force against this listener is answered by the per-IP auto-ban
         // and the refused-source set above. See AccountLogon.h.
         accountLogon.RegisterFailedLogin(peer_address, _T("REST API"), disconnect, false);

         // 'disconnect' is set exactly when this failure was the one that
         // tripped the ban. The connection carrying it is closed after its one
         // response either way, so what matters is the *next* connection from
         // this address - and an IP range would not refuse that one, because
         // ranges are only consulted through TCPConnection. Recording the
         // address here is what makes the ban real on this listener, and it is
         // also what stops a flood from creating a ban (and its row, and its
         // run of name-collision SELECTs) every third request. See the
         // refused-source set at the top of this file.
         if (disconnect)
         {
            RefuseAddress_(peer_address);

            // One line per ban, not one per refused request: a line per
            // connection would answer a flood of cheap requests with a flood of
            // log writes, which is the shape of problem being fixed. The
            // refusals themselves are visible under debug logging only.
            String message;
            message.Format(_T("REST API: Refusing requests from %s for %d minutes after repeated authentication failures."),
               String(peer_address.ToString()).c_str(), RefusedAddressMinutes);
            LOG_APPLICATION(message);
         }
      }
      catch (...)
      {
         // Auto-ban accounting touches the database. A failure there must not
         // turn into a 500 on a request that was going to be refused anyway.
         LOG_DEBUG("RestApiServer: Failed to record an authentication failure for auto-ban.");
      }
   }

   HttpResponse
   RestApiServer::ProcessRequest_(const AnsiString &request, const IPAddress &peer_address)
   {
      // Parse the request line.
      int lineEnd = request.Find("\r\n");
      if (lineEnd < 0)
         return BuildResponse_(400, "{\"error\":\"malformed request\"}");

      AnsiString requestLine = request.Mid(0, lineEnd);

      std::vector<AnsiString> requestParts = StringParser::SplitString(requestLine, " ");
      if (requestParts.size() < 2)
         return BuildResponse_(400, "{\"error\":\"malformed request\"}");

      AnsiString method = requestParts[0];
      AnsiString path = requestParts[1];

      // Strip the query string from the path, keeping it for the routes that
      // take one.
      AnsiString query;
      int queryPosition = path.Find("?");
      if (queryPosition >= 0)
      {
         query = path.Mid(queryPosition + 1);
         path = path.Mid(0, queryPosition);
      }

      // OpenTelemetry: span this request, continuing the caller's trace when a
      // valid traceparent header arrived and starting a fresh local one when it
      // was absent or rejected - a rejected value never refuses the request.
      // Named after the sanitized method (low-cardinality even for junk input)
      // with the path as an attribute.
      // The RAII scope covers every return below. No-op unless OtelEndpoint is
      // configured.
      OtelSpanScope otelSpan(OtelTraceContext::SanitizeSpanName(method), OtelSpanKindServer,
                             OtelTraceContext::FromHttpRequest(request));
      otelSpan.AddAttribute("http.target", path);

      // The web admin SPA shell is served without authentication (it is a
      // static login page). This is the only unauthenticated route in the
      // listener: everything else goes through Authenticate_ below, once, and
      // no handler is reachable except from the dispatch at the bottom of this
      // function.
      if (method == "GET" && (path == "/" || path == "/index.html"))
         return HandleWebAdminPage_();

      // One language's catalogue of that page's texts, from the languages
      // directory beside it; unauthenticated as the page is, since the sign-in
      // card is in the reader's language before there is a session.
      if (method == "GET" && path.StartsWith("/deck-lang/") && path.EndsWith(".json") && path.GetLength() > 16)
         return HandleWebAdminLanguage_(path.Mid(11, path.GetLength() - 16));

      // The self-service page, likewise unauthenticated: it is a static sign-in
      // form whose script presents the account's credentials to /api/v1/me.
      if (method == "GET" && path == "/portal")
         return HandlePortalPage_(request);

      if (method == "GET" && path == "/portal.js")
         return HandlePortalScript_();

      if (method == "GET" && path == "/portal-smime.js")
         return HandlePortalSmimeScript_();

      // The two files that make the page installable and reachable without a
      // connection: the manifest, and the service worker that keeps the shell.
      if (method == "GET" && path == "/portal.webmanifest")
         return HandlePortalManifest_();

      // What the page says it is, for the sign-in page, which has no account yet.
      if (method == "GET" && path == "/api/v1/portal/branding")
         return HandlePortalBranding_(query);

      // One language's catalogue of the page's texts, for a reader who chose it.
      if (method == "GET" && path.StartsWith("/portal-lang/") && path.EndsWith(".json") && path.GetLength() > 18)
         return HandlePortalLanguage_(path.Mid(13, path.GetLength() - 18));

      // A file sent as a link: anyone with the link, and the password when
      // the sender set one (a form, posted back here).
      if ((method == "GET" || method == "POST") && path.StartsWith("/files/"))
         return HandlePublicFile_(method, path.Mid(7), GetRequestBody_(request));

      // A message that expires: the page a recipient's notice links to. No
      // credentials to get there; the reader proves they are a recipient on the
      // page itself, or is already signed in to the webmail as one.
      if ((method == "GET" || method == "POST") && path.StartsWith("/confidential/"))
         return HandleConfidentialPage_(method, path.Mid(14), request, peer_address);

      // The two links a quarantine digest carries. No credentials: the token in
      // the query string is the whole capability, it names one account and one
      // entry, it works once and it expires. See RestApiQuarantineDigest.cpp.
      // The provider sending the browser back from an import sign-in. No
      // credentials: the state names the sign-in, and the cookie the browser
      // was given when it started it proves this is the same browser.
      if (method == "GET" && path == "/portal/oauth/callback")
         return HandlePortalOAuthCallback_(request, query);

      // Single sign-on, the same shape for the same reason: the browser is on
      // its way to the organisation's identity provider, or coming back from
      // it, and has no session yet. Neither of these mints one - the callback
      // hands out a one-use ticket and POST /api/v1/session does the minting.
      // See RestApiOidcSignIn.cpp.
      if (method == "GET" && path == "/portal/oidc/start")
         return HandleOidcStart_(query);

      if (method == "GET" && path == "/portal/oidc/callback")
         return HandleOidcCallback_(request, query);

      // What the two sign-in pages may offer, and the challenge a passkey
      // sign-in answers. Both before Authenticate_, and necessarily: a person
      // signing in with a passkey has no credential to present yet, which is
      // the whole point of a passkey. The challenge answers nothing about who
      // exists here - it is random bytes and a relying party id the page
      // already knows.
      if (method == "GET" && path == "/api/v1/signin-methods")
         return HandleSignInMethods_();

      if (method == "POST" && path == "/api/v1/passkeys/challenge")
         return HandlePasskeyChallenge_(request);

      // Recovery: the page a confirmation link opens, the "I have forgotten my
      // password" route, the page the reset link opens, and the route the
      // webmail posts the new password to. No credentials at any of them - the
      // person using them has none they can present - and the token in the
      // query string is the whole capability, one account, once, expiring.
      // See RestApiAccountRecovery.cpp.
      if ((method == "GET" || method == "POST") && path == "/recovery/verify")
         return HandleRecoveryVerifyPage_(method, query);

      if (method == "POST" && path == "/api/v1/password-reset")
         return HandlePasswordResetRequest_(GetRequestBody_(request), peer_address);

      if (method == "POST" && path == "/api/v1/password-reset/complete")
         return HandlePasswordResetComplete_(GetRequestBody_(request), peer_address);

      if ((method == "GET" || method == "POST") && path == "/portal/reset")
         return HandlePasswordResetPage_(method, query, method == "POST" ? GetRequestBody_(request) : AnsiString(), peer_address);

      if ((method == "GET" || method == "POST") && path == "/held/release")
         return HandleDigestRelease_(method, query, method == "POST" ? GetRequestBody_(request) : AnsiString());

      if ((method == "GET" || method == "POST") && path == "/held/stop")
         return HandleDigestStop_(method, query, method == "POST" ? GetRequestBody_(request) : AnsiString());

      if (method == "GET" && path == "/portal-sw.js")
         return HandlePortalServiceWorker_();

      try
      {
         // Inside the try: authenticating a bearer token reads the key store and
         // recording a failure touches the database, neither of which may turn
         // an unauthenticated request into an unhandled exception on the single
         // REST worker thread.
         Caller caller = Authenticate_(request, peer_address);

         // Cross-site request forgery, closed twice over: the cookie is
         // SameSite=Strict, so another site's request does not carry it, and
         // a request that changes something must carry a header a browser
         // never adds on its own - which another origin cannot add without a
         // preflight this server never grants. A session presented in the
         // bearer form is held to the same header: it is a session, and the
         // page that holds it is the same page.
         if (caller.via_session && method != "GET" && method != "HEAD" &&
             GetHeader_(request, "x-requested-with") != "hMailServer")
         {
            return BuildResponse_(403, "{\"error\":\"a request that changes something must carry X-Requested-With: hMailServer when it is authenticated by a session cookie or a session token\"}");
         }

         if (caller.result == AuthenticationFailed)
            return BuildUnauthorizedResponse_(caller.second_factor_required, IsPageScriptRequest_(request));

         // Who is acting, for the length of this request and no longer.
         // Nothing may decide from it, which is why the decision below takes
         // the grant as an argument rather than reaching for the actor. The
         // audit trail's own actor is installed from the same caller, and so
         // from the same grant, once the request has been authorised - see
         // AuditActorFor_ below the dispatch's AuditScope.
         RoleActor actor;
         actor.identity = caller.identity;
         actor.account_id = caller.account ? caller.account->GetID() : 0;
         actor.domain_id = caller.account ? caller.account->GetDomainID() : 0;
         actor.grant = caller.grant;
         RoleManager::ActorScope actorScope(actor);

         // After authentication, so the budget belongs to the credential rather
         // than to a source address, and before routing, so that being over it
         // costs nothing but this comparison.
         bool firstRefusal = false;

         if (!IsWithinRequestRate_(caller.identity, firstRefusal))
         {
            if (firstRefusal)
            {
               // One line per credential per window. A line per refused request
               // would answer a flood of cheap requests with a flood of log
               // writes, which is the shape of problem being fixed.
               String message;
               message.Format(_T("REST API: Credential '%s' has exceeded %d requests in %d seconds and is refused for the rest of the window."),
                  String(caller.identity).c_str(), MaxRequestsPerWindowPerCredential,
                  (int) (RateWindowMilliseconds / 1000));
               LOG_APPLICATION(message);
            }

            return BuildTooManyRequestsResponse_();
         }

         Route route;
         // The query string is attached BEFORE the path is parsed, because
         // one route decides what it is from it: a report names the domain it
         // is about in ?domain=, and that domain has to be in the Route for
         // Authorize_ to judge it. Everything else ignores it at this stage.
         route.query = query;
         ParseRoute_(method, path, route);

         // The two segments a route names a thing by, decoded once, here -
         // before Authorize_ scopes the request by the domain in the identifier
         // and before any handler looks the identifier up. A client that
         // percent-encodes a path segment is doing the ordinary thing (the
         // Control Deck and the PowerShell module encode every address they put
         // in a path), and "bob%40example.test" is not an address anything in
         // this server can find: the route parses, the authorisation scopes it
         // to an empty domain, and the handler answers 404 for an account that
         // is there.
         //
         // A separator is deliberately NOT decoded. The route has already been
         // split on "/" and several routes refuse an identifier that contains
         // one, so decoding %2F or %5C here would let a segment gain a
         // separator the parse rejected - which is how an identifier that is
         // used to name a file becomes a path. Everything else decodes; the log
         // route's own allow-list still decides what a log name may be.
         route.identifier = DecodePathSegment_(route.identifier);
         route.name = DecodePathSegment_(route.name);

         // The single authorisation choke point. Every route is decided here,
         // by kind, before any handler runs - so a handler cannot be reached by
         // a credential that was never checked against it, and a new endpoint
         // cannot be added without appearing in Authorize_ as well.
         AnsiString refusalReason;
         AuthorizationResult authorization = Authorize_(caller, route, refusalReason);

         if (authorization == AuthorizationUnauthenticated)
            return BuildUnauthorizedResponse_(false, IsPageScriptRequest_(request));

         if (authorization == AuthorizationForbidden)
         {
            String message;
            message.Format(_T("REST API: Credential '%s' was refused %s %s - %s."),
               String(caller.identity).c_str(), String(method).c_str(),
               String(path).c_str(), String(refusalReason).c_str());
            LOG_APPLICATION(message);

            return BuildForbiddenResponse_(refusalReason);
         }

         // The audit trail's identity chokepoint for REST, and the only one:
         // every route reaches its handler through the switch below, so an
         // administrative change made over REST - by the Deck, by hmctl, by an
         // API key or by a person with curl - is attributed to the credential
         // that was just authorised and to the address it came from, whichever
         // handler ends up making it. Nothing is recorded here: the record is
         // taken where the change lands, in AuditTrail::RecordStatement and
         // RecordSettingChange, so one change is one row however many routes
         // could have caused it. See AuditTrail.h.
         AuditScope auditScope(AuditActorFor_(caller));

         switch (route.kind)
         {
         case RouteApiKeyList:
            return HandleListApiKeys_();

         case RouteApiKeyCreate:
            return HandleCreateApiKey_(GetRequestBody_(request));

         case RouteApiKeyRevoke:
            return HandleRevokeApiKey_(route.identifier);

         case RouteStatus:
            return HandleStatus_();

         case RouteSessionGet:
            return HandleSessionGet_(caller);

         case RouteRoleList:
            return HandleRoleList_();

         case RouteRoleGet:
            return HandleRoleGet_(route.identifier);

         case RouteRoleCreate:
            return HandleRoleSave_(AnsiString(), GetRequestBody_(request), true);

         case RouteRoleUpdate:
            return HandleRoleSave_(route.identifier, GetRequestBody_(request), false);

         case RouteRoleDelete:
            return HandleRoleDelete_(route.identifier);

         case RouteRoleAssignmentList:
            return HandleRoleAssignmentList_(caller);

         case RouteRoleAssign:
            return HandleRoleAssign_(caller, route.identifier, GetRequestBody_(request));

         case RouteRoleUnassign:
            return HandleRoleUnassign_(caller, route.identifier);

         case RouteAccountPasswordReset:
            return HandleAccountPasswordReset_(caller, route.identifier, GetRequestBody_(request));

         case RouteAccountUnlock:
            return HandleAccountUnlock_(caller, route.identifier);

         case RouteAccountPushGet:
            return HandleAccountPush_(caller, route.identifier);

         case RouteAccountPushRemove:
            return HandleAccountPushRemove_(caller, route.identifier);

         case RouteAccountRecoveryClear:
            return HandleAccountRecoveryClear_(caller, String(route.identifier));

         case RouteAccountRecoveryCodesClear:
            return HandleAccountRecoveryCodesClear_(caller, String(route.identifier));

         case RouteMailboxMoveGet:
            return HandleMailboxMoveGet_(route.identifier);

         case RouteMailboxMove:
            return HandleMailboxMove_(caller, route.identifier, GetRequestBody_(request));

         case RouteDomainList:
            return HandleListDomains_(caller.domains);

         case RouteDomainGet:
            return HandleGetDomain_(String(route.identifier));

         case RouteDomainCreate:
            return HandleCreateDomain_(GetRequestBody_(request));

         case RouteDomainUpdate:
            return HandleUpdateDomain_(caller, String(route.identifier), GetRequestBody_(request));

         case RouteDomainDelete:
            return HandleDeleteDomain_(String(route.identifier));

         case RouteAccountList:
            return HandleListAccounts_(String(route.identifier));

         case RouteAccountCreate:
            return HandleCreateAccount_(caller, String(route.identifier), GetRequestBody_(request));

         case RouteAccountDelete:
            return HandleDeleteAccount_(String(route.identifier));

         case RouteQueueList:
            return HandleListQueue_();

         case RouteQueueRetry:
            return HandleQueueRetry_(route.message_id);

         case RouteQueueDelete:
            return HandleQueueDelete_(route.message_id);

         case RouteTlsa:
            return HandleTlsa_();

         case RouteSrv:
            return HandleSrv_(caller.domains);
         case RouteMetricsHistory:
            return HandleMetricsHistory_(route.query);
         case RouteReport:
            return HandleReport_(caller.domains, route.name, route.query);
         case RouteUpdateGet:
            return HandleUpdateGet_();
         case RouteUpdateCheck:
            return HandleUpdateCheck_();
         case RouteUpdateDownload:
            return HandleUpdateDownload_();
         case RouteUpdateInstall:
            return HandleUpdateInstall_();
         case RouteDiagnosticsRun:
            return HandleDiagnosticsRun_(GetRequestBody_(request));

         case RouteQuarantineList:
            return HandleListQuarantine_();

         case RouteQuarantineRelease:
            return HandleQuarantineRelease_(route.message_id);

         case RouteQuarantineDelete:
            return HandleQuarantineDelete_(route.message_id);

         case RouteAliasList:
            return HandleListAliases_(String(route.identifier));

         case RouteIpRangeList:
            return HandleListIpRanges_();
         case RouteIpRangeCreate:
            return HandleCreateIpRange_(GetRequestBody_(request));
         case RouteIpRangeDelete:
            return HandleDeleteIpRange_(route.range_id);
         case RouteListList:
            return HandleListLists_(String(route.identifier));
         case RouteListCreate:
            return HandleCreateList_(String(route.identifier), caller.domains, GetRequestBody_(request));
         case RouteListDelete:
            return HandleDeleteList_(String(route.identifier));
         case RouteListUpdate:
            return HandleUpdateList_(String(route.identifier), caller.domains, GetRequestBody_(request));
         case RouteListMembers:
            return HandleListMembers_(String(route.identifier), caller.domains);
         case RouteDnsBlackListList:
         case RouteDnsBlackListCreate:
         case RouteDnsBlackListUpdate:
         case RouteDnsBlackListDelete:
         case RouteSurblServerList:
         case RouteSurblServerCreate:
         case RouteSurblServerUpdate:
         case RouteSurblServerDelete:
         case RouteWhiteListAddressList:
         case RouteWhiteListAddressCreate:
         case RouteWhiteListAddressUpdate:
         case RouteWhiteListAddressDelete:
         case RouteBlockedSenderList:
         case RouteBlockedSenderCreate:
         case RouteBlockedSenderUpdate:
         case RouteBlockedSenderDelete:
         case RouteIncomingRelayList:
         case RouteIncomingRelayCreate:
         case RouteIncomingRelayUpdate:
         case RouteIncomingRelayDelete:
         case RouteGreyListingWhiteAddressList:
         case RouteGreyListingWhiteAddressCreate:
         case RouteGreyListingWhiteAddressUpdate:
         case RouteGreyListingWhiteAddressDelete:
            return HandleAntiSpamLists_(route.kind, route.record_id, GetRequestBody_(request));
         case RouteGreylistBypassList:
         case RouteGreylistBypassCreate:
         case RouteGreylistBypassUpdate:
         case RouteGreylistBypassDelete:
            return HandleGreylistBypass_(route.kind, route.record_id, GetRequestBody_(request));
         case RouteBlockedAttachmentList:
         case RouteBlockedAttachmentCreate:
         case RouteBlockedAttachmentUpdate:
         case RouteBlockedAttachmentDelete:
            return HandleBlockedAttachments_(route.kind, route.record_id, GetRequestBody_(request));

         case RouteJournalRuleList:
         case RouteJournalRuleCreate:
         case RouteJournalRuleUpdate:
         case RouteJournalRuleDelete:
            return HandleJournalRules_(route.kind, route.record_id, GetRequestBody_(request));
         case RouteSenderListList:
         case RouteSenderListCreate:
         case RouteSenderListUpdate:
         case RouteSenderListDelete:
         case RouteSenderListDomainList:
         case RouteSenderListDomainCreate:
         case RouteSenderListDomainUpdate:
         case RouteSenderListDomainDelete:
         case RouteSenderListMeList:
         case RouteSenderListMeCreate:
         case RouteSenderListMeUpdate:
         case RouteSenderListMeDelete:
            return HandleSenderLists_(route, caller, GetRequestBody_(request));
         case RouteSpamTrapDomainList:
         case RouteSpamTrapDomainCreate:
         case RouteSpamTrapDelete:
         case RouteSpamTrapDomainHits:
         case RouteSpamTrapHits:
         case RouteSpamTrapHitsClear:
            return HandleSpamTraps_(route, caller, GetRequestBody_(request));
         case RouteDomainPolicyList:
         case RouteDomainPolicyCreate:
         case RouteDomainPolicyUpdate:
         case RouteDomainPolicyDelete:
            return HandleDomainPolicyList_(route.kind, route.record_id, GetRequestBody_(request));
         case RouteGroupList:
         case RouteGroupCreate:
         case RouteGroupGet:
         case RouteGroupUpdate:
         case RouteGroupDelete:
         case RouteGroupMemberList:
         case RouteGroupMemberCreate:
         case RouteGroupMemberDelete:
            return HandleGroups_(route, GetRequestBody_(request));
         case RouteAuditList:
         case RouteAuditVerify:
         case RouteAuditRevert:
         case RouteAlertRuleList:
         case RouteAlertRuleUpdate:
         case RouteAlertEventList:
         case RouteAlertTest:
         case RouteAlertRun:
            return HandleAudit_(route, GetRequestBody_(request));

         case RouteRuleMatch:
            return HandleRuleMatch_(GetRequestBody_(request));
         case RouteCertificateList:
            return HandleListCertificates_();
         case RouteDkimGet:
            return HandleDkim_(String(route.identifier));
         case RouteDkimRotationGet:
         case RouteDkimRotationStart:
         case RouteDkimRotationCheck:
         case RouteDkimRotationSwitch:
         case RouteDkimRotationRetire:
         case RouteDkimRotationCancel:
         case RouteDkimRotationRun:
            return HandleDkimRotation_(caller, route, GetRequestBody_(request));
         case RouteReputationGet:
         case RouteReputationCheck:
            return HandleReputation_(caller, route, GetRequestBody_(request));
         case RouteReviewList:
         case RouteReviewRelease:
         case RouteReviewDiscard:
         case RouteAccountReviewGet:
         case RouteAccountReviewPut:
         case RouteAccountReviewUnlock:
            return HandleAccountReview_(caller, route, GetRequestBody_(request));
         case RouteSetupChecklist:
            return HandleSetupChecklist_(route);
         case RouteSampleDomainGet:
         case RouteSampleDomainCreate:
         case RouteSampleDomainRemove:
            return HandleSampleDomain_(route);
         case RouteDomainRuleList:
         case RouteDomainRuleCreate:
         case RouteDomainRuleUpdate:
         case RouteDomainRuleDelete:
            return HandleDomainRules_(route, GetRequestBody_(request));
         case RouteAccountRuleList:
         case RouteAccountRuleCreate:
         case RouteAccountRuleUpdate:
         case RouteAccountRuleDelete:
            return HandleAccountRules_(route, GetRequestBody_(request));
         case RouteAccountExport:
            return HandleAccountExport_(route, GetRequestBody_(request));
         case RouteAccountExportArchive:
            return HandleAccountExportArchive_(route);
         case RouteAddressErase:
            return HandleAddressErase_(GetRequestBody_(request));
         case RouteAdminSecondFactorGet:
         case RouteAdminSecondFactorEnrol:
         case RouteAdminSecondFactorDisable:
            return HandleAdministratorSecondFactor_(route);
         case RouteRuleList:
            return HandleListRules_(0);
         case RouteLogList:
            return HandleListLogs_();
         case RouteMessageTraceRecent:
         case RouteMessageTraceAddress:
         case RouteMessageTraceQueue:
            return HandleMessageTrace_(caller, route.kind, route.identifier);
         case RouteLogTail:
            return HandleLogTail_(route.identifier, route.query);
         case RouteLogSessions:
            return HandleLogSessions_();
         case RouteLogSessionFollow:
            return HandleLogSessionFollow_(route);
         case RouteBackupStart:
            return HandleBackupStart_();
         case RouteBackupStatus:
            return HandleBackupStatus_();
         case RouteBackupRestore:
            return HandleBackupRestore_(GetRequestBody_(request));
         case RouteSettingsGet:
            return HandleSettings_();
         case RouteSettingsPut:
            return HandleSettingsPut_(caller, GetRequestBody_(request));
         case RouteDomainSessionPolicyGet:
            return HandleDomainSessionPolicy_(route);
         case RouteSessionPolicyGet:
            return HandleSessionPolicy_();
         case RouteSettingsAntiSpamGet:
            return HandleSettingsAntiSpam_();
         case RouteSettingsAntiSpamPut:
            return HandleSettingsAntiSpamPut_(GetRequestBody_(request));
         case RouteSettingsAntiVirusGet:
            return HandleSettingsAntiVirus_();
         case RouteSettingsAntiVirusPut:
            return HandleSettingsAntiVirusPut_(GetRequestBody_(request));
         case RouteSettingsLoggingGet:
            return HandleSettingsLogging_();
         case RouteSettingsLoggingPut:
            return HandleSettingsLoggingPut_(GetRequestBody_(request));
         case RouteSettingsDirectoriesGet:
            return HandleSettingsDirectories_();
         case RouteIniSettingList:
            return HandleIniSettingList_();
         case RouteSettingsChanged:
            return HandleSettingsChanged_();
         case RouteIniSettingGet:
            return HandleIniSettingGet_(route.identifier);
         case RouteIniSettingPut:
            return HandleIniSettingPut_(route.identifier, GetRequestBody_(request));
         case RouteIniSettingDelete:
            return HandleIniSettingDelete_(route.identifier);
         case RouteLogonFailuresClear:
            return HandleLogonFailuresClear_();
         case RouteSettingsScriptingGet:
            return HandleSettingsScripting_();
         case RouteSettingsScriptingPut:
            return HandleSettingsScriptingPut_(GetRequestBody_(request));
         case RouteScriptingReload:
            return HandleScriptingReload_();
         case RouteScriptingCheck:
            return HandleScriptingCheck_();
         case RouteBreachCheckTest:
            return HandleBreachCheckTest_(GetRequestBody_(request));
         case RouteSettingsCacheGet:
            return HandleSettingsCache_();
         case RouteSettingsCachePut:
            return HandleSettingsCachePut_(GetRequestBody_(request));
         case RouteCacheClear:
            return HandleCacheClear_();
         case RouteSettingsIndexingGet:
            return HandleSettingsIndexing_();
         case RouteSettingsIndexingPut:
            return HandleSettingsIndexingPut_(GetRequestBody_(request));
         case RouteIndexingIndex:
            return HandleIndexingIndex_();
         case RouteIndexingClear:
            return HandleIndexingClear_();
         case RouteSettingsBackupGet:
            return HandleSettingsBackup_();
         case RouteSettingsBackupPut:
            return HandleSettingsBackupPut_(GetRequestBody_(request));
         case RouteMePush:
            return HandleMePush_(caller);
         case RouteMePushSubscribe:
            return HandleMePushSubscribe_(caller, GetRequestBody_(request));
         case RouteMePushUpdate:
            return HandleMePushUpdate_(caller, route.message_id, GetRequestBody_(request));
         case RouteMePushDelete:
            return HandleMePushDelete_(caller, route.message_id);
         case RouteMePushTest:
            return HandleMePushTest_(caller, GetRequestBody_(request));
         case RouteSettingsPushGet:
            return HandleSettingsPush_();
         case RouteSettingsPushPut:
            return HandleSettingsPushPut_(GetRequestBody_(request));
         case RouteSettingsPushVapid:
            return HandleSettingsPushVapid_();
         case RouteSettingsBouncesGet:
            return HandleSettingsBounces_();
         case RouteSettingsBouncesPut:
            return HandleSettingsBouncesPut_(GetRequestBody_(request));
         case RouteSettingsOpenPgpGet:
            return HandleSettingsOpenPgp_();
         case RouteSettingsOpenPgpPut:
            return HandleSettingsOpenPgpPut_(GetRequestBody_(request));
         case RouteAccountOpenPgpList:
            return HandleAccountOpenPgp_(route.identifier);
         case RouteAccountOpenPgpPut:
            return HandleAccountOpenPgpPut_(caller, route.identifier, GetRequestBody_(request));
         case RouteAccountOpenPgpDelete:
            return HandleAccountOpenPgpDelete_(route.identifier, route.name);
         case RouteDomainOpenPgpList:
            return HandleDomainOpenPgpList_(route.identifier);
         case RouteSuppressionList:
         case RouteSuppressionCreate:
         case RouteSuppressionDelete:
         case RouteBounceCountList:
         case RouteBounceClassify:
         case RouteBounceExpire:
            return HandleBounces_(route, GetRequestBody_(request));
         case RouteSettingsComplaintsGet:
            return HandleSettingsComplaints_();
         case RouteSettingsComplaintsPut:
            return HandleSettingsComplaintsPut_(GetRequestBody_(request));
         case RouteComplaintList:
         case RouteComplaintRates:
         case RouteComplaintExpire:
            return HandleComplaints_(route, GetRequestBody_(request));
         case RouteDomainReportsSummary:
         case RouteDomainReportsList:
         case RouteDomainReportGet:
         case RouteDomainTlsReportsList:
         case RouteDomainReportsExpire:
            return HandleDomainReports_(route, GetRequestBody_(request));
         case RouteWebhookList:
         case RouteWebhookGet:
         case RouteWebhookCreate:
         case RouteWebhookUpdate:
         case RouteWebhookDelete:
         case RouteWebhookDeliveryList:
         case RouteWebhookRun:
         case RouteWebhookTest:
            return HandleWebhooks_(caller, route, GetRequestBody_(request));
         case RouteServerMessageList:
            return HandleServerMessageList_();
         case RouteServerMessagePut:
            return HandleServerMessagePut_(route.name, GetRequestBody_(request));
         case RouteSieveEvaluate:
            return HandleSieveEvaluate_(GetRequestBody_(request));
         case RouteCalendarExpand:
            return HandleCalendarExpand_(GetRequestBody_(request));
         case RouteIpRangeUpdate:
            return HandleUpdateIpRange_(route.range_id, GetRequestBody_(request));
         case RouteDomainAliasList:
            return HandleListDomainAliases_(String(route.identifier));
         case RouteDomainAliasCreate:
            return HandleCreateDomainAlias_(String(route.identifier), GetRequestBody_(request));
         case RouteDomainAliasDelete:
            return HandleDeleteDomainAlias_(String(route.identifier), String(route.name));
         case RouteRuleCreate:
            return HandleCreateRule_(0, GetRequestBody_(request));
         case RouteRuleUpdate:
            return HandleUpdateRule_(0, route.record_id, GetRequestBody_(request));
         case RouteRuleDelete:
            return HandleDeleteRule_(0, route.record_id);
         case RouteCertificateCreate:
            return HandleCreateCertificate_(GetRequestBody_(request));
         case RouteCertificateDelete:
            return HandleDeleteCertificate_(route.record_id);
         case RoutePortList:
            return HandleListPorts_();
         case RoutePortCreate:
            return HandleCreatePort_(GetRequestBody_(request));
         case RoutePortUpdate:
            return HandleUpdatePort_(route.record_id, GetRequestBody_(request));
         case RoutePortDelete:
            return HandleDeletePort_(route.record_id);
         case RouteRouteList:
            return HandleListRoutes_();
         case RouteRouteCreate:
            return HandleCreateRoute_(GetRequestBody_(request));
         case RouteRouteUpdate:
            return HandleUpdateRoute_(route.record_id, GetRequestBody_(request));
         case RouteRouteDelete:
            return HandleDeleteRoute_(route.record_id);
         case RouteRemoteDomainList:
            return HandleListRemoteDomains_();
         case RouteRemoteDomainCreate:
            return HandleCreateRemoteDomain_(GetRequestBody_(request));
         case RouteRemoteDomainUpdate:
            return HandleUpdateRemoteDomain_(route.record_id, GetRequestBody_(request));
         case RouteRemoteDomainDelete:
            return HandleDeleteRemoteDomain_(route.record_id);
         case RouteRemoteDomainEffective:
            return HandleEffectiveRemoteDomain_(String(QueryParameter_(route.query, "domain")));
         case RouteRemoteDomainClearCache:
            return HandleClearRemoteDomainVerificationCache_();
         case RouteAliasCreate:
            return HandleCreateAlias_(String(route.identifier), GetRequestBody_(request));
         case RouteAliasDelete:
            return HandleDeleteAlias_(String(route.identifier));
         case RouteAccountUpdate:
            return HandleUpdateAccount_(caller, String(route.identifier), GetRequestBody_(request));
         case RouteAccountGet:
            return HandleGetAccount_(String(route.identifier));
         case RouteFetchAccountList:
            return HandleListFetchAccounts_(String(route.identifier));
         case RouteFetchAccountCreate:
            return HandleCreateFetchAccount_(String(route.identifier), GetRequestBody_(request));
         case RouteFetchAccountGet:
            return HandleGetFetchAccount_(String(route.identifier), route.record_id);
         case RouteFetchAccountUpdate:
            return HandleUpdateFetchAccount_(String(route.identifier), route.record_id, GetRequestBody_(request));
         case RouteFetchAccountDelete:
            return HandleDeleteFetchAccount_(String(route.identifier), route.record_id);
         case RouteFetchAccountDownload:
            return HandleDownloadFetchAccount_(String(route.identifier), route.record_id);
         case RouteServerReinitialize:
            return HandleServerReinitialize_();
         case RouteArchiveSearch:
            return HandleArchiveSearch_(caller.domains, route.query);
         case RouteArchiveGet:
            return HandleArchiveGet_(caller.domains, route.archive_id);
         case RouteArchiveHold:
            return HandleArchiveHold_(caller.domains, route.archive_id, true);
         case RouteArchiveRelease:
            return HandleArchiveHold_(caller.domains, route.archive_id, false);
         case RouteMe:
            return HandleMe_(caller);

         case RouteMePassword:
            return HandleMePassword_(caller, request);

         case RouteMeVacation:
            return HandleMeVacation_(caller, GetRequestBody_(request));

         case RouteMeQuarantineList:
            return HandleMeQuarantineList_(caller);

         case RouteMeQuarantineRelease:
            return HandleMeQuarantineRelease_(caller, route.message_id);

         case RouteMeQuarantineDigestGet:
            return HandleMeQuarantineDigest_(caller, "GET", AnsiString());

         case RouteMeQuarantineDigestPut:
            return HandleMeQuarantineDigest_(caller, "PUT", GetRequestBody_(request));

         case RouteMeQuarantineDelete:
            return HandleMeQuarantineDelete_(caller, route.message_id);

         case RouteMeFolders:
            return HandleMeFolders_(caller);

         case RouteMeFolderCreate:
            return HandleMeFolderCreate_(caller, GetRequestBody_(request));

         case RouteMeFolderRename:
            return HandleMeFolderRename_(caller, route.folder_id, GetRequestBody_(request));

         case RouteMeFolderDelete:
            return HandleMeFolderDelete_(caller, route.folder_id);

         case RouteMeChanges:
            return HandleMeChanges_(caller, route.query);

         case RouteMeFolderMessages:
            return HandleMeFolderMessages_(caller, route.folder_id, route.query);

         case RouteMeMessage:
            return HandleMeMessage_(caller, route.message_id);

         case RouteMeMessageFlags:
            return HandleMeMessageFlags_(caller, route.message_id, GetRequestBody_(request));

         case RouteMeMessageMove:
            return HandleMeMessageMove_(caller, route.message_id, GetRequestBody_(request));

         case RouteMeMessageDelete:
            return HandleMeMessageDelete_(caller, route.message_id, route.query);

         case RouteMeMessageSend:
            return HandleMeMessageSend_(caller, GetRequestBody_(request));

         case RouteMeMessageAttachment:
            return HandleMeMessageAttachment_(caller, route.message_id, route.attachment_index);

         case RouteMeSearch:
            return HandleMeSearch_(caller, route.query);

         case RouteMeSettings:
            return HandleMeSettings_(caller);

         case RouteMeSettingsPut:
            return HandleMeSettingsPut_(caller, GetRequestBody_(request));

         case RouteMeFilters:
            return HandleMeFilters_(caller);

         case RouteMeFiltersPut:
            return HandleMeFiltersPut_(caller, GetRequestBody_(request));

         case RouteMeDraftSave:
            return HandleMeDraftSave_(caller, GetRequestBody_(request));

         case RouteMeContacts:
            return HandleMeContacts_(caller, route.query);

         case RouteMeContactCreate:
            return HandleMeContactCreate_(caller, GetRequestBody_(request));

         case RouteMeContactUpdate:
            return HandleMeContactUpdate_(caller, route.message_id, GetRequestBody_(request));

         case RouteMeContactPhoto:
            return HandleMeContactPhoto_(caller, route.message_id);

         case RouteMeContactGroups:
         case RouteMeContactGroupCreate:
         case RouteMeContactGroupGet:
         case RouteMeContactGroupUpdate:
         case RouteMeContactGroupDelete:
         case RouteMeContactGroupMemberAdd:
         case RouteMeContactGroupMemberRemove:
            return HandleMeContactGroups_(caller, route.kind, route.record_id, route.name, GetRequestBody_(request));

         case RouteMeCalendars:
            return HandleMeCalendars_(caller);
         case RouteMeCalendarCreate:
            return HandleMeCalendarCreate_(caller, GetRequestBody_(request));
         case RouteMeCalendarUpdate:
            return HandleMeCalendarUpdate_(caller, route.message_id, GetRequestBody_(request));
         case RouteMeCalendarDelete:
            return HandleMeCalendarDelete_(caller, route.message_id);
         case RouteMeCalendarSharePut:
            return HandleMeCalendarSharePut_(caller, route.message_id, route.identifier, GetRequestBody_(request));
         case RouteMeCalendarShareDelete:
            return HandleMeCalendarShareDelete_(caller, route.message_id, route.identifier);
         case RouteMeEvents:
            return HandleMeEvents_(caller, route.query);
         case RouteMeEventGet:
            return HandleMeEventGet_(caller, route.message_id);
         case RouteMeEventCreate:
            return HandleMeEventCreate_(caller, GetRequestBody_(request));
         case RouteMeEventUpdate:
            return HandleMeEventUpdate_(caller, route.message_id, GetRequestBody_(request));
         case RouteMeEventDelete:
            return HandleMeEventDelete_(caller, route.message_id, route.query);
         case RouteMeTasks:
            return HandleMeTasks_(caller);
         case RouteMeTaskCreate:
            return HandleMeTaskCreate_(caller, GetRequestBody_(request));
         case RouteMeTaskUpdate:
            return HandleMeTaskUpdate_(caller, route.message_id, GetRequestBody_(request));
         case RouteMeTaskDelete:
            return HandleMeTaskDelete_(caller, route.message_id);
         case RouteMeMessageInvitation:
            return HandleMeMessageInvitation_(caller, route.message_id);
         case RouteMeMessageInvitationReply:
            return HandleMeMessageInvitationReply_(caller, route.message_id, GetRequestBody_(request));
         case RouteMeContactDelete:
            return HandleMeContactDelete_(caller, route.message_id);

         case RouteMeIdentities:
            return HandleMeIdentities_(caller);

         case RouteMePreferences:
            return HandleMePreferences_(caller);

         case RouteMePreferencesPut:
            return HandleMePreferencesPut_(caller, GetRequestBody_(request));

         case RouteMeFolderEmpty:
            return HandleMeFolderEmpty_(caller, route.folder_id, route.query);

         case RouteMeMessageSource:
            return HandleMeMessageSource_(caller, route.message_id);

         case RouteMeMessageHtml:
            return HandleMeMessageHtml_(caller, route.message_id, route.query);
         case RouteMeAppPasswords:
            return HandleMeAppPasswords_(caller);

         case RouteMeAppPasswordCreate:
            return HandleMeAppPasswordCreate_(caller, request);

         case RouteMeAppPasswordDelete:
            return HandleMeAppPasswordDelete_(caller, route.message_id);

         case RouteMeFiles:
            return HandleMeFiles_(caller);

         case RouteMeFileCreate:
            return HandleMeFileCreate_(caller, GetRequestBody_(request));

         case RouteMeFileContent:
            return HandleMeFileContent_(caller, route.message_id, route.query, GetRequestBody_(request));

         case RouteMeFileUpdate:
            return HandleMeFileUpdate_(caller, route.message_id, GetRequestBody_(request));

         case RouteMeFileDelete:
            return HandleMeFileDelete_(caller, route.message_id);

         case RoutePortalFilesPolicy:
            return HandlePortalFilesPolicy_(route.query);

         case RoutePortalFilesPolicyPut:
            return HandlePortalFilesPolicyPut_(GetRequestBody_(request));

         case RouteMeSessions:
            return HandleMeSessions_(caller);

         case RouteMeSessionsEnd:
            return HandleMeSessionsEnd_(caller);

         case RouteMeSessionDelete:
            return HandleMeSessionDelete_(caller, route.identifier);

         case RouteMeDevices:
            return HandleMeDevices_(caller);

         case RouteMeDeviceDelete:
            return HandleMeDeviceDelete_(caller, route.identifier, route.query);

         case RouteMeSmime:
            return HandleMeSmime_(caller);

         case RouteMeSmimeOwnPut:
            return HandleMeSmimeOwnPut_(caller, GetRequestBody_(request));

         case RouteMeSmimeOwnDelete:
            return HandleMeSmimeOwnDelete_(caller, route.identifier);

         case RouteMeSmimeRecipientPut:
            return HandleMeSmimeRecipientPut_(caller, GetRequestBody_(request));

         case RouteMeSmimeRecipientDelete:
            return HandleMeSmimeRecipientDelete_(caller, route.identifier);

         case RouteMeSmimeChain:
            return HandleMeSmimeChain_(GetRequestBody_(request));

         case RouteMeOpenPgp:
            return HandleMeOpenPgp_(caller);

         case RouteMeOpenPgpPut:
            return HandleMeOpenPgpPut_(caller, GetRequestBody_(request));

         case RouteMeOpenPgpDelete:
            return HandleMeOpenPgpDelete_(caller, route.identifier);

         case RouteMeOpenPgpPublish:
            return HandleMeOpenPgpPublish_(caller, GetRequestBody_(request));

         case RouteMeStorage:
            return HandleMeStorage_(caller);

         case RouteMeMessageReceipt:
            return HandleMeMessageReceipt_(caller, route.message_id);

         case RouteMeMessageUnsubscribe:
            return HandleMeMessageUnsubscribe_(caller, route.message_id);

         case RouteMeSubscriptions:
            return HandleMeSubscriptions_(caller, route.query);

         case RouteMeBrandLogo:
            return HandleMeBrandLogo_(route.query);

         case RouteMeFetchAccounts:
         case RouteMeFetchAccountCreate:
         case RouteMeFetchAccount:
         case RouteMeFetchAccountUpdate:
         case RouteMeFetchAccountDelete:
         case RouteMeFetchAccountDownload:
            return HandleMeFetchAccounts_(caller, route.kind, route.record_id, GetRequestBody_(request));

         case RouteMeFetchImport:
         case RouteMeFetchImportStart:
            return HandleMeFetchImport_(caller, route.kind, GetRequestBody_(request));

         case RouteMeSenderAddresses:
         case RouteMeSenderAddressCreate:
         case RouteMeSenderAddressVerify:
         case RouteMeSenderAddressResend:
         case RouteMeSenderAddressDelete:
            return HandleMeSenderAddresses_(caller, route.kind, route.record_id, GetRequestBody_(request));

         case RouteMeMaskedAddresses:
         case RouteMeMaskedAddressCreate:
         case RouteMeMaskedAddressUpdate:
         case RouteMeMaskedAddressDelete:
            return HandleMeMaskedAddresses_(caller, route.kind, route.record_id, GetRequestBody_(request));

         case RouteMeRecovery:
         case RouteMeRecoveryPut:
         case RouteMeRecoveryDelete:
         case RouteMeRecoveryResend:
         case RouteMeRecoveryCodes:
            return HandleMeRecovery_(caller, route.kind, request);

         case RouteMeRecoverable:
         case RouteMeRecoverableRestore:
         case RouteMeRecoverableDelete:
            return HandleMeRecoverable_(caller, route.kind, route.record_id);

         case RoutePasskeyList:
         case RoutePasskeyOptions:
         case RoutePasskeyCreate:
         case RoutePasskeyDelete:
            return HandlePasskeys_(caller, route.kind, route.record_id, request);

         case RouteMeConfidential:
         case RouteMeConfidentialRevoke:
            return HandleMeConfidential_(caller, route.kind, route.record_id);

         case RouteMeOffline:
         case RouteMeOfflinePut:
         case RouteMeOfflineMessages:
            return HandleMeOffline_(caller, route.kind, route.query, GetRequestBody_(request));

         case RouteMeDraftSchedule:
            return HandleMeDraftSchedule_(caller, route.message_id, GetRequestBody_(request));

         case RouteMeDraftUnschedule:
            return HandleMeDraftUnschedule_(caller, route.message_id);

         case RouteMeMessageSnooze:
            return HandleMeMessageSnooze_(caller, route.message_id, GetRequestBody_(request));

         case RouteMeScheduled:
            return HandleMeScheduled_(caller);

         case RouteMeScheduledCancel:
            return HandleMeScheduledCancel_(caller, route.message_id);

         case RouteScheduledRun:
            return HandleScheduledRun_();

         case RoutePortalBrandingPut:
            return HandlePortalBrandingPut_(GetRequestBody_(request));

         case RouteAccountSupportSession:
            return HandleAccountSupportSession_(caller, route.identifier, request);

         case RouteMeFolderExport:
            return HandleMeFolderExport_(caller, route.folder_id);
         case RouteMeExportArchive:
            return HandleMeExportArchive_(caller);

         case RouteMeFolderImport:
            return HandleMeFolderImport_(caller, route.folder_id, GetRequestBody_(request));

         case RouteSessionCreate:
            return HandleSessionCreate_(caller, request, route.query);

         case RouteSessionDelete:
            return HandleSessionDelete_(caller);

         case RouteAccountAppPasswordList:
         case RouteAccountAppPasswordCreate:
         case RouteAccountAppPasswordDelete:
         case RouteAccountFolderList:
         case RouteAccountFolderPermissionList:
         case RouteAccountFolderPermissionCreate:
         case RouteAccountFolderPermissionUpdate:
         case RouteAccountFolderPermissionDelete:
         case RouteAccountMessageList:
         case RouteAccountMessagesDelete:
         case RouteAccountFolderMessageList:
         case RouteAccountFolderMessageCreate:
         case RouteAccountMessageGet:
         case RouteAccountMessageFlags:
         case RouteAccountMessageDelete:
         case RouteAccountMessageSource:
         case RouteAccountAppPasswordUpdate:
         case RouteAccountMessageCopy:
         case RouteAccountDeviceList:
         case RouteAccountDeviceDelete:
            return HandleAccountResources_(route, GetRequestBody_(request));

         case RouteOpenApi:
            return HandleOpenApi_();

         default:
            break;
         }

         // RouteUnknown, and RouteApiKeyUnsupported once the administrator check
         // in Authorize_ has let it through. Deliberately after the switch rather
         // than inside its default: a switch every arm of which returns still
         // leaves /W3 asking whether the function does, and answering that with
         // an unreachable return is worse than this.
         return BuildResponse_(404, "{\"error\":\"not found\"}");
      }
      catch (...)
      {
         return BuildResponse_(500, "{\"error\":\"internal error\"}");
      }
   }

   AnsiString
   RestApiServer::DecodePathSegment_(const AnsiString &raw)
   {
      if (raw.Find("%") < 0)
         return raw;

      AnsiString value;

      for (int i = 0; i < raw.GetLength(); i++)
      {
         const char c = raw.GetAt(i);

         if (c == '%' && i + 2 < raw.GetLength() &&
             isxdigit((unsigned char) raw.GetAt(i + 1)) && isxdigit((unsigned char) raw.GetAt(i + 2)))
         {
            const char hex[3] = { raw.GetAt(i + 1), raw.GetAt(i + 2), 0 };
            const char decoded = (char) strtol(hex, nullptr, 16);

            // A separator, or a NUL that would end a narrow string early, is
            // left as it was written. See the note at the call. The backslash
            // is written as its code so that no reader has to count escapes.
            if (decoded == '/' || decoded == (char) 92 || decoded == 0)
            {
               value += c;
               continue;
            }

            value += decoded;
            i += 2;
            continue;
         }

         value += c;
      }

      return value;
   }

   void
   RestApiServer::ParseRoute_(const AnsiString &method, const AnsiString &path, Route &route)
   {
      // Transcribed from the dispatch this replaced, predicate for predicate and
      // in the same order, so that which requests reach which handler is
      // unchanged. Note that StartsWith and EndsWith are case-insensitive in
      // this tree while operator== is not - long-standing behaviour, preserved
      // here rather than tidied, because tightening it is a separate change with
      // its own compatibility question.
      route.kind = RouteUnknown;
      route.identifier = "";
      route.message_id = 0;

      const AnsiString apiKeysPath = "/api/v1/apikeys";

      if (path == apiKeysPath || path.StartsWith(apiKeysPath + "/"))
      {
         route.kind = RouteApiKeyUnsupported;

         if (path == apiKeysPath)
         {
            if (method == "GET")
               route.kind = RouteApiKeyList;
            else if (method == "POST")
               route.kind = RouteApiKeyCreate;
         }
         else if (method == "DELETE")
         {
            AnsiString id = path.Mid(apiKeysPath.GetLength() + 1);

            if (!id.IsEmpty() && id.Find("/") < 0)
            {
               route.kind = RouteApiKeyRevoke;
               route.identifier = id;
            }
         }

         return;
      }

      if (path == "/api/v1/me" && method == "GET")
      {
         route.kind = RouteMe;
         return;
      }

      // Later: a draft sent at a time, what is scheduled, one item cancelled.
      const AnsiString meDraftsPath = "/api/v1/me/drafts/";
      if (path.StartsWith(meDraftsPath))
      {
         AnsiString rest = path.Mid(meDraftsPath.GetLength());
         if (rest.EndsWith("/schedule"))
         {
            AnsiString idText = rest.Mid(0, rest.GetLength() - AnsiString("/schedule").GetLength());
            if (ParseQueueId(idText, route.message_id))
            {
               if (method == "POST")
                  route.kind = RouteMeDraftSchedule;
               else if (method == "DELETE")
                  route.kind = RouteMeDraftUnschedule;
            }
         }
         return;
      }

      const AnsiString meScheduledPath = "/api/v1/me/scheduled";
      if (path == meScheduledPath)
      {
         if (method == "GET")
            route.kind = RouteMeScheduled;
         return;
      }
      if (path.StartsWith(meScheduledPath + "/"))
      {
         AnsiString rest = path.Mid(meScheduledPath.GetLength() + 1);
         if (method == "DELETE" && ParseQueueId(rest, route.message_id))
            route.kind = RouteMeScheduledCancel;
         return;
      }

      // The account's address book: the list (q= and limit= for the completion
      // popup), one new contact, and one contact by id.
      const AnsiString meContactsPath = "/api/v1/me/contacts";

      if (path == meContactsPath)
      {
         if (method == "GET")
            route.kind = RouteMeContacts;
         else if (method == "POST")
            route.kind = RouteMeContactCreate;

         return;
      }

      if (path.StartsWith(meContactsPath + "/"))
      {
         AnsiString rest = path.Mid(meContactsPath.GetLength() + 1);

         // The groups in the address book (RestApiContactGroups.cpp): the
         // list, one group by id, and its members.
         const AnsiString groupsSegment = "groups";
         if (rest == groupsSegment)
         {
            if (method == "GET")
               route.kind = RouteMeContactGroups;
            else if (method == "POST")
               route.kind = RouteMeContactGroupCreate;

            return;
         }

         if (rest.StartsWith(groupsSegment + "/"))
         {
            AnsiString tail = rest.Mid(groupsSegment.GetLength() + 1);
            const AnsiString membersSegment = "/members";
            int membersAt = tail.Find(membersSegment);
            if (membersAt > 0)
            {
               if (!ParseQueueId(tail.Mid(0, membersAt), route.record_id))
                  return;

               AnsiString after = tail.Mid(membersAt + membersSegment.GetLength());
               if (after.IsEmpty() && method == "POST")
                  route.kind = RouteMeContactGroupMemberAdd;
               else if (after.GetLength() > 1 && after.StartsWith("/") && method == "DELETE")
               {
                  route.name = DecodePathSegment_(after.Mid(1));
                  route.kind = RouteMeContactGroupMemberRemove;
               }

               return;
            }

            if (ParseQueueId(tail, route.record_id))
            {
               if (method == "GET")
                  route.kind = RouteMeContactGroupGet;
               else if (method == "PUT")
                  route.kind = RouteMeContactGroupUpdate;
               else if (method == "DELETE")
                  route.kind = RouteMeContactGroupDelete;
            }

            return;
         }

         if (method == "GET" && rest.EndsWith("/photo"))
         {
            AnsiString idText = rest.Mid(0, rest.GetLength() - AnsiString("/photo").GetLength());
            if (ParseQueueId(idText, route.message_id))
               route.kind = RouteMeContactPhoto;

            return;
         }

         if (ParseQueueId(rest, route.message_id))
         {
            if (method == "PUT")
               route.kind = RouteMeContactUpdate;
            else if (method == "DELETE")
               route.kind = RouteMeContactDelete;
         }

         return;
      }

      // The account's calendars and their shares, its events, its tasks.
      const AnsiString meCalendarsPath = "/api/v1/me/calendars";
      if (path == meCalendarsPath)
      {
         if (method == "GET")
            route.kind = RouteMeCalendars;
         else if (method == "POST")
            route.kind = RouteMeCalendarCreate;
         return;
      }
      if (path.StartsWith(meCalendarsPath + "/"))
      {
         AnsiString rest = path.Mid(meCalendarsPath.GetLength() + 1);
         int sharesAt = rest.Find("/shares/");
         if (sharesAt > 0)
         {
            AnsiString idText = rest.Mid(0, sharesAt);
            AnsiString address = rest.Mid(sharesAt + AnsiString("/shares/").GetLength());
            if (ParseQueueId(idText, route.message_id) && !address.IsEmpty())
            {
               route.identifier = address;
               if (method == "PUT")
                  route.kind = RouteMeCalendarSharePut;
               else if (method == "DELETE")
                  route.kind = RouteMeCalendarShareDelete;
            }
            return;
         }
         if (ParseQueueId(rest, route.message_id))
         {
            if (method == "PUT")
               route.kind = RouteMeCalendarUpdate;
            else if (method == "DELETE")
               route.kind = RouteMeCalendarDelete;
         }
         return;
      }

      const AnsiString meEventsPath = "/api/v1/me/calendar/events";
      if (path == meEventsPath)
      {
         if (method == "GET")
            route.kind = RouteMeEvents;
         else if (method == "POST")
            route.kind = RouteMeEventCreate;
         return;
      }
      if (path.StartsWith(meEventsPath + "/"))
      {
         AnsiString rest = path.Mid(meEventsPath.GetLength() + 1);
         if (ParseQueueId(rest, route.message_id))
         {
            if (method == "GET")
               route.kind = RouteMeEventGet;
            else if (method == "PUT")
               route.kind = RouteMeEventUpdate;
            else if (method == "DELETE")
               route.kind = RouteMeEventDelete;
         }
         return;
      }

      const AnsiString meTasksPath = "/api/v1/me/tasks";
      if (path == meTasksPath)
      {
         if (method == "GET")
            route.kind = RouteMeTasks;
         else if (method == "POST")
            route.kind = RouteMeTaskCreate;
         return;
      }
      if (path.StartsWith(meTasksPath + "/"))
      {
         AnsiString rest = path.Mid(meTasksPath.GetLength() + 1);
         if (ParseQueueId(rest, route.message_id))
         {
            if (method == "PUT")
               route.kind = RouteMeTaskUpdate;
            else if (method == "DELETE")
               route.kind = RouteMeTaskDelete;
         }
         return;
      }

      // The account's app passwords, its browser sessions, and its storage.
      const AnsiString meAppPasswordsPath = "/api/v1/me/app-passwords";
      if (path == meAppPasswordsPath)
      {
         if (method == "GET")
            route.kind = RouteMeAppPasswords;
         else if (method == "POST")
            route.kind = RouteMeAppPasswordCreate;

         return;
      }

      if (path.StartsWith(meAppPasswordsPath + "/"))
      {
         AnsiString rest = path.Mid(meAppPasswordsPath.GetLength() + 1);
         if (method == "DELETE" && ParseQueueId(rest, route.message_id))
            route.kind = RouteMeAppPasswordDelete;

         return;
      }

      const AnsiString meDevicesPath = "/api/v1/me/devices";
      if (path == meDevicesPath)
      {
         if (method == "GET")
            route.kind = RouteMeDevices;

         return;
      }

      if (path.StartsWith(meDevicesPath + "/"))
      {
         // A device is named by its key: thirty-two hex characters, which is
         // what SignInStore::DeviceKey answers and what the listing shows.
         // Any non-empty remainder is the route; the handler is what answers 400
         // for one that is not thirty-two hex digits, so that a garbage id gets
         // that sentence and not the authorisation's one about an unknown route.
         AnsiString rest = path.Mid(meDevicesPath.GetLength() + 1);
         if (method == "DELETE" && !rest.IsEmpty())
         {
            route.identifier = rest;
            route.kind = RouteMeDeviceDelete;
         }

         return;
      }

      const AnsiString meSessionsPath = "/api/v1/me/sessions";
      if (path == meSessionsPath)
      {
         if (method == "GET")
            route.kind = RouteMeSessions;
         else if (method == "DELETE")
            route.kind = RouteMeSessionsEnd;

         return;
      }

      if (path.StartsWith(meSessionsPath + "/"))
      {
         // A session is named by the first twelve hex digits of its token's
         // hash - enough to tell sessions apart, nothing that signs in.
         AnsiString rest = path.Mid(meSessionsPath.GetLength() + 1);
         if (method == "DELETE" && rest.GetLength() == 12)
         {
            route.identifier = rest;
            route.kind = RouteMeSessionDelete;
         }

         return;
      }

      // S/MIME: the key store (own certificates with wrapped keys, recipients'
      // certificates, each named by its SHA-256 fingerprint) and the chain check.
      const AnsiString meSmimePath = "/api/v1/me/smime";
      if (path == meSmimePath)
      {
         if (method == "GET")
            route.kind = RouteMeSmime;

         return;
      }

      if (path.StartsWith(meSmimePath + "/"))
      {
         AnsiString rest = path.Mid(meSmimePath.GetLength() + 1);
         if (rest == "own" && method == "PUT")
            route.kind = RouteMeSmimeOwnPut;
         else if (rest == "recipients" && method == "PUT")
            route.kind = RouteMeSmimeRecipientPut;
         else if (rest == "chain" && method == "POST")
            route.kind = RouteMeSmimeChain;
         else if (rest.StartsWith("own/") && method == "DELETE" && rest.GetLength() == 4 + 64)
         {
            route.identifier = rest.Mid(4);
            route.kind = RouteMeSmimeOwnDelete;
         }
         else if (rest.StartsWith("recipients/") && method == "DELETE" && rest.GetLength() == 11 + 64)
         {
            route.identifier = rest.Mid(11);
            route.kind = RouteMeSmimeRecipientDelete;
         }

         return;
      }

      // The account's own OpenPGP public keys, and the switch that decides
      // whether the Web Key Directory serves them.
      const AnsiString meOpenPgpPath = "/api/v1/me/openpgp";
      if (path == meOpenPgpPath)
      {
         if (method == "GET")
            route.kind = RouteMeOpenPgp;
         else if (method == "PUT")
            route.kind = RouteMeOpenPgpPut;

         return;
      }

      if (path.StartsWith(meOpenPgpPath + "/"))
      {
         AnsiString rest = path.Mid(meOpenPgpPath.GetLength() + 1);
         if (rest == "publish" && method == "PUT")
            route.kind = RouteMeOpenPgpPublish;
         else if (method == "DELETE" && rest.Find("/") < 0 && !rest.IsEmpty())
         {
            route.identifier = rest;
            route.kind = RouteMeOpenPgpDelete;
         }

         return;
      }

      if (path == "/api/v1/me/storage")
      {
         if (method == "GET")
            route.kind = RouteMeStorage;

         return;
      }

      // The account's files sent as links: the list and a new record, then
      // the bytes in chunks, the link's life and password, and removal.
      const AnsiString meFilesPath = "/api/v1/me/files";
      if (path == meFilesPath)
      {
         if (method == "GET")
            route.kind = RouteMeFiles;
         else if (method == "POST")
            route.kind = RouteMeFileCreate;

         return;
      }

      if (path.StartsWith(meFilesPath + "/"))
      {
         AnsiString rest = path.Mid(meFilesPath.GetLength() + 1);
         const AnsiString contentSuffix = "/content";
         if (rest.EndsWith(contentSuffix))
         {
            if (method == "PUT" && ParseQueueId(rest.Mid(0, rest.GetLength() - contentSuffix.GetLength()), route.message_id))
               route.kind = RouteMeFileContent;

            return;
         }

         if (ParseQueueId(rest, route.message_id))
         {
            if (method == "PUT")
               route.kind = RouteMeFileUpdate;
            else if (method == "DELETE")
               route.kind = RouteMeFileDelete;
         }

         return;
      }

      // Who the account may write as, and its preferences.
      if (path == "/api/v1/me/identities")
      {
         if (method == "GET")
            route.kind = RouteMeIdentities;

         return;
      }

      if (path == "/api/v1/me/preferences")
      {
         if (method == "GET")
            route.kind = RouteMePreferences;
         else if (method == "PUT")
            route.kind = RouteMePreferencesPut;

         return;
      }

      // The account's own Web Push devices. Matched before the quarantine block
      // for the same reason the digest is: nothing below would recognise it.
      if (path == "/api/v1/me/push")
      {
         if (method == "GET")
            route.kind = RouteMePush;

         return;
      }

      if (path == "/api/v1/me/push/test")
      {
         if (method == "POST")
            route.kind = RouteMePushTest;

         return;
      }

      const AnsiString mePushPath = "/api/v1/me/push/subscriptions";
      if (path == mePushPath)
      {
         if (method == "POST")
            route.kind = RouteMePushSubscribe;

         return;
      }

      if (path.StartsWith(mePushPath + "/"))
      {
         AnsiString rest = path.Mid(mePushPath.GetLength() + 1);
         if (ParseQueueId(rest, route.message_id))
         {
            if (method == "PUT")
               route.kind = RouteMePushUpdate;
            else if (method == "DELETE")
               route.kind = RouteMePushDelete;
         }

         return;
      }

      // Before the quarantine block below, because "/api/v1/me/quarantine-digest"
      // does not begin with "/api/v1/me/quarantine/" and would otherwise fall
      // through to the unknown route - but the next reader looking at these two
      // lines should not have to work that out, so it is matched first and said
      // here.
      if (path == "/api/v1/me/quarantine-digest")
      {
         if (method == "GET")
            route.kind = RouteMeQuarantineDigestGet;
         else if (method == "PUT")
            route.kind = RouteMeQuarantineDigestPut;

         return;
      }

      const AnsiString meSenderListPath = "/api/v1/me/sender-list";

      if (path == meSenderListPath)
      {
         if (method == "GET")
            route.kind = RouteSenderListMeList;
         else if (method == "POST")
            route.kind = RouteSenderListMeCreate;

         return;
      }

      if ((method == "PUT" || method == "DELETE") && path.StartsWith(meSenderListPath + "/"))
      {
         AnsiString idPart = path.Mid(meSenderListPath.GetLength() + 1);
         __int64 id = 0;

         if (idPart.Find("/") < 0 && ParseQueueId(idPart, id))
         {
            route.kind = method == "PUT" ? RouteSenderListMeUpdate : RouteSenderListMeDelete;
            route.record_id = id;
         }

         return;
      }

      const AnsiString meQuarantinePath = "/api/v1/me/quarantine";

      if (path == meQuarantinePath && method == "GET")
      {
         route.kind = RouteMeQuarantineList;
         return;
      }

      if (path.StartsWith(meQuarantinePath + "/"))
      {
         AnsiString rest = path.Mid(meQuarantinePath.GetLength() + 1);

         if (method == "POST" && rest.EndsWith("/release"))
         {
            AnsiString idText = rest.Mid(0, rest.GetLength() - AnsiString("/release").GetLength());
            if (ParseQueueId(idText, route.message_id))
               route.kind = RouteMeQuarantineRelease;
            return;
         }

         if (method == "DELETE" && ParseQueueId(rest, route.message_id))
            route.kind = RouteMeQuarantineDelete;

         return;
      }

      // The other mailboxes the reader collects into this one.
      if (path == "/api/v1/me/fetch-accounts")
      {
         if (method == "GET")
            route.kind = RouteMeFetchAccounts;
         else if (method == "POST")
            route.kind = RouteMeFetchAccountCreate;

         return;
      }

      // The recovery address, and the one-time recovery codes.
      if (path == "/api/v1/me/recovery")
      {
         if (method == "GET")
            route.kind = RouteMeRecovery;
         else if (method == "PUT")
            route.kind = RouteMeRecoveryPut;
         else if (method == "DELETE")
            route.kind = RouteMeRecoveryDelete;

         return;
      }

      if (method == "POST" && path == "/api/v1/me/recovery/resend")
      {
         route.kind = RouteMeRecoveryResend;
         return;
      }

      if (method == "POST" && path == "/api/v1/me/recovery/codes")
      {
         route.kind = RouteMeRecoveryCodes;
         return;
      }

      // Deleted items still within the recovery window.
      if (path == "/api/v1/me/recoverable")
      {
         if (method == "GET")
            route.kind = RouteMeRecoverable;

         return;
      }

      if (path.StartsWith("/api/v1/me/recoverable/"))
      {
         AnsiString rest = path.Mid(AnsiString("/api/v1/me/recoverable/").GetLength());
         const bool restore = rest.EndsWith("/restore");
         if (restore)
            rest = rest.Mid(0, rest.GetLength() - AnsiString("/restore").GetLength());

         if (!ParseQueueId(rest, route.record_id))
            return;

         if (restore && method == "POST")
            route.kind = RouteMeRecoverableRestore;
         else if (!restore && method == "DELETE")
            route.kind = RouteMeRecoverableDelete;

         return;
      }

      // Offline in the webmail: the reader's window, and the listing the page
      // fills its browser store from.
      if (path == "/api/v1/me/offline")
      {
         if (method == "GET")
            route.kind = RouteMeOffline;
         else if (method == "PUT")
            route.kind = RouteMeOfflinePut;

         return;
      }

      if (path == "/api/v1/me/offline/messages")
      {
         if (method == "GET")
            route.kind = RouteMeOfflineMessages;

         return;
      }

      // Messages that expire: the ones this account sent, and withdrawing one.
      if (path == "/api/v1/me/confidential")
      {
         if (method == "GET")
            route.kind = RouteMeConfidential;

         return;
      }

      if (path.StartsWith("/api/v1/me/confidential/") && path.EndsWith("/revoke"))
      {
         AnsiString rest = path.Mid(AnsiString("/api/v1/me/confidential/").GetLength());
         rest = rest.Mid(0, rest.GetLength() - AnsiString("/revoke").GetLength());

         if (!ParseQueueId(rest, route.record_id))
            return;

         if (method == "POST")
            route.kind = RouteMeConfidentialRevoke;

         return;
      }

      // Masked addresses: the reader's own, for one sign-up each.
      if (path == "/api/v1/me/masked-addresses")
      {
         if (method == "GET")
            route.kind = RouteMeMaskedAddresses;
         else if (method == "POST")
            route.kind = RouteMeMaskedAddressCreate;

         return;
      }

      if (path.StartsWith("/api/v1/me/masked-addresses/"))
      {
         if (!ParseQueueId(path.Mid(AnsiString("/api/v1/me/masked-addresses/").GetLength()), route.record_id))
            return;

         if (method == "PUT")
            route.kind = RouteMeMaskedAddressUpdate;
         else if (method == "DELETE")
            route.kind = RouteMeMaskedAddressDelete;

         return;
      }

      // Addresses elsewhere the reader has proved and sends as.
      if (path == "/api/v1/me/sender-addresses")
      {
         if (method == "GET")
            route.kind = RouteMeSenderAddresses;
         else if (method == "POST")
            route.kind = RouteMeSenderAddressCreate;

         return;
      }

      if (path.StartsWith("/api/v1/me/sender-addresses/"))
      {
         AnsiString rest = path.Mid(AnsiString("/api/v1/me/sender-addresses/").GetLength());
         AnsiString action;
         const int slash = rest.Find("/");
         if (slash >= 0)
         {
            action = rest.Mid(slash + 1);
            rest = rest.Mid(0, slash);
         }

         if (!ParseQueueId(rest, route.record_id))
            return;

         if (action.IsEmpty() && method == "DELETE")
            route.kind = RouteMeSenderAddressDelete;
         else if (action == "verify" && method == "POST")
            route.kind = RouteMeSenderAddressVerify;
         else if (action == "resend" && method == "POST")
            route.kind = RouteMeSenderAddressResend;

         return;
      }

      // Another account signed in to with a provider rather than a password.
      if (path == "/api/v1/me/fetch-accounts/import")
      {
         if (method == "GET")
            route.kind = RouteMeFetchImport;
         else if (method == "POST")
            route.kind = RouteMeFetchImportStart;

         return;
      }

      if (path.StartsWith("/api/v1/me/fetch-accounts/"))
      {
         AnsiString rest = path.Mid(AnsiString("/api/v1/me/fetch-accounts/").GetLength());
         const bool download = rest.EndsWith("/download");
         if (download)
            rest = rest.Mid(0, rest.GetLength() - AnsiString("/download").GetLength());

         if (!ParseQueueId(rest, route.record_id))
            return;

         if (download)
         {
            if (method == "POST")
               route.kind = RouteMeFetchAccountDownload;
         }
         else if (method == "GET")
            route.kind = RouteMeFetchAccount;
         else if (method == "PUT")
            route.kind = RouteMeFetchAccountUpdate;
         else if (method == "DELETE")
            route.kind = RouteMeFetchAccountDelete;

         return;
      }

      // The logo a sender's domain publishes, for the avatar of its mail.
      if (path == "/api/v1/me/brand-logo")
      {
         if (method == "GET")
            route.kind = RouteMeBrandLogo;

         return;
      }

      // The senders whose mail offers a way out, gathered from the mailbox.
      if (path == "/api/v1/me/subscriptions")
      {
         if (method == "GET")
            route.kind = RouteMeSubscriptions;

         return;
      }

      // The account's own mailbox: the folder tree and its writes, one folder's
      // messages, one message.
      if (path == "/api/v1/me/folders")
      {
         if (method == "GET")
            route.kind = RouteMeFolders;
         else if (method == "POST")
            route.kind = RouteMeFolderCreate;

         return;
      }

      const AnsiString meFoldersPath = "/api/v1/me/folders/";

      if (path.StartsWith(meFoldersPath))
      {
         AnsiString rest = path.Mid(meFoldersPath.GetLength());

         if (method == "GET" && rest.EndsWith("/messages"))
         {
            AnsiString idText = rest.Mid(0, rest.GetLength() - AnsiString("/messages").GetLength());
            if (ParseQueueId(idText, route.folder_id))
               route.kind = RouteMeFolderMessages;

            return;
         }

         if (method == "POST" && rest.EndsWith("/messages"))
         {
            AnsiString idText = rest.Mid(0, rest.GetLength() - AnsiString("/messages").GetLength());
            if (ParseQueueId(idText, route.folder_id))
               route.kind = RouteMeFolderImport;

            return;
         }

         if (method == "GET" && rest.EndsWith("/export"))
         {
            AnsiString idText = rest.Mid(0, rest.GetLength() - AnsiString("/export").GetLength());
            if (ParseQueueId(idText, route.folder_id))
               route.kind = RouteMeFolderExport;

            return;
         }

         if (method == "POST" && rest.EndsWith("/empty"))
         {
            AnsiString idText = rest.Mid(0, rest.GetLength() - AnsiString("/empty").GetLength());
            if (ParseQueueId(idText, route.folder_id))
               route.kind = RouteMeFolderEmpty;

            return;
         }

         // The remainder is the folder id itself, with no resource segment after
         // it - which is what ParseQueueId enforces, since an id is digits only.
         if (method == "PUT" && ParseQueueId(rest, route.folder_id))
            route.kind = RouteMeFolderRename;
         else if (method == "DELETE" && ParseQueueId(rest, route.folder_id))
            route.kind = RouteMeFolderDelete;

         return;
      }

      if (path == "/api/v1/me/changes" && method == "GET")
      {
         route.kind = RouteMeChanges;
         return;
      }

      if (path == "/api/v1/me/drafts" && method == "POST")
      {
         route.kind = RouteMeDraftSave;
         return;
      }

      if (path == "/api/v1/me/export.zip" && method == "GET")
      {
         route.kind = RouteMeExportArchive;
         return;
      }

      if (path == "/api/v1/me/settings")
      {
         if (method == "GET")
            route.kind = RouteMeSettings;
         else if (method == "PUT")
            route.kind = RouteMeSettingsPut;
         return;
      }

      if (path == "/api/v1/me/filters")
      {
         if (method == "GET")
            route.kind = RouteMeFilters;
         else if (method == "PUT")
            route.kind = RouteMeFiltersPut;
         return;
      }

      if (path == "/api/v1/me/search" && method == "GET")
      {
         route.kind = RouteMeSearch;
         return;
      }

      if (path == "/api/v1/me/messages" && method == "POST")
      {
         route.kind = RouteMeMessageSend;
         return;
      }

      const AnsiString meMessagesPath = "/api/v1/me/messages/";

      if (path.StartsWith(meMessagesPath))
      {
         AnsiString rest = path.Mid(meMessagesPath.GetLength());

         if (method == "PUT" && rest.EndsWith("/flags"))
         {
            AnsiString idText = rest.Mid(0, rest.GetLength() - AnsiString("/flags").GetLength());
            if (ParseQueueId(idText, route.message_id))
               route.kind = RouteMeMessageFlags;
            return;
         }

         if (method == "GET" && rest.EndsWith("/source"))
         {
            AnsiString idText = rest.Mid(0, rest.GetLength() - AnsiString("/source").GetLength());
            if (ParseQueueId(idText, route.message_id))
               route.kind = RouteMeMessageSource;
            return;
         }

         if (method == "GET" && rest.EndsWith("/html"))
         {
            AnsiString idText = rest.Mid(0, rest.GetLength() - AnsiString("/html").GetLength());
            if (ParseQueueId(idText, route.message_id))
               route.kind = RouteMeMessageHtml;
            return;
         }

         int attachmentsAt = rest.Find("/attachments/");
         if (method == "GET" && attachmentsAt > 0)
         {
            AnsiString idText = rest.Mid(0, attachmentsAt);
            AnsiString indexText = rest.Mid(attachmentsAt + AnsiString("/attachments/").GetLength());
            // The index starts at zero, which no queue id does.
            bool digits = !indexText.IsEmpty() && indexText.GetLength() <= 5;
            for (int k = 0; digits && k < indexText.GetLength(); k++)
               digits = indexText[k] >= '0' && indexText[k] <= '9';
            if (ParseQueueId(idText, route.message_id) && digits)
            {
               route.attachment_index = atoi(indexText.c_str());
               route.kind = RouteMeMessageAttachment;
            }
            return;
         }

         if (method == "POST" && rest.EndsWith("/move"))
         {
            AnsiString idText = rest.Mid(0, rest.GetLength() - AnsiString("/move").GetLength());
            if (ParseQueueId(idText, route.message_id))
               route.kind = RouteMeMessageMove;
            return;
         }

         if (method == "POST" && rest.EndsWith("/snooze"))
         {
            AnsiString idText = rest.Mid(0, rest.GetLength() - AnsiString("/snooze").GetLength());
            if (ParseQueueId(idText, route.message_id))
               route.kind = RouteMeMessageSnooze;
            return;
         }

         if (method == "POST" && rest.EndsWith("/receipt"))
         {
            AnsiString idText = rest.Mid(0, rest.GetLength() - AnsiString("/receipt").GetLength());
            if (ParseQueueId(idText, route.message_id))
               route.kind = RouteMeMessageReceipt;
            return;
         }

         if (method == "GET" && rest.EndsWith("/invitation"))
         {
            AnsiString idText = rest.Mid(0, rest.GetLength() - AnsiString("/invitation").GetLength());
            if (ParseQueueId(idText, route.message_id))
               route.kind = RouteMeMessageInvitation;
            return;
         }

         if (method == "POST" && rest.EndsWith("/invitation/reply"))
         {
            AnsiString idText = rest.Mid(0, rest.GetLength() - AnsiString("/invitation/reply").GetLength());
            if (ParseQueueId(idText, route.message_id))
               route.kind = RouteMeMessageInvitationReply;
            return;
         }

         if (method == "POST" && rest.EndsWith("/unsubscribe"))
         {
            AnsiString idText = rest.Mid(0, rest.GetLength() - AnsiString("/unsubscribe").GetLength());
            if (ParseQueueId(idText, route.message_id))
               route.kind = RouteMeMessageUnsubscribe;
            return;
         }

         if (method == "GET" && ParseQueueId(rest, route.message_id))
            route.kind = RouteMeMessage;
         else if (method == "DELETE" && ParseQueueId(rest, route.message_id))
            route.kind = RouteMeMessageDelete;

         return;
      }

      if (path == "/api/v1/me/password" && method == "POST")
      {
         route.kind = RouteMePassword;
         return;
      }

      if (path == "/api/v1/me/vacation" && method == "PUT")
      {
         route.kind = RouteMeVacation;
         return;
      }

      if (path == "/api/v1/session" && method == "POST")
      {
         route.kind = RouteSessionCreate;
         return;
      }

      // The passkeys whoever is signed in holds (RestApiPasskeys.cpp). The
      // sign-in challenge at /api/v1/passkeys/challenge is not here: it carries
      // no credential and is answered before Authenticate_.
      if (path == "/api/v1/passkeys")
      {
         if (method == "GET")
            route.kind = RoutePasskeyList;
         else if (method == "POST")
            route.kind = RoutePasskeyCreate;

         return;
      }

      if (path == "/api/v1/passkeys/options" && method == "POST")
      {
         route.kind = RoutePasskeyOptions;
         return;
      }

      if (method == "DELETE" && path.StartsWith("/api/v1/passkeys/"))
      {
         if (ParseQueueId(path.Mid(AnsiString("/api/v1/passkeys/").GetLength()), route.record_id))
            route.kind = RoutePasskeyDelete;

         return;
      }

      if (path == "/api/v1/session" && method == "DELETE")
      {
         route.kind = RouteSessionDelete;
         return;
      }

      // What the credential this request came in on is, and what it may do.
      if (path == "/api/v1/session" && method == "GET")
      {
         route.kind = RouteSessionGet;
         return;
      }

      // The roles, and who holds them (RestApiRoles.cpp).
      if (path == "/api/v1/roles")
      {
         if (method == "GET")
            route.kind = RouteRoleList;
         else if (method == "POST")
            route.kind = RouteRoleCreate;
         return;
      }

      if (path.StartsWith("/api/v1/roles/"))
      {
         AnsiString name = path.Mid(AnsiString("/api/v1/roles/").GetLength());
         if (!name.IsEmpty() && name.Find("/") < 0)
         {
            if (method == "GET")
               route.kind = RouteRoleGet;
            else if (method == "PUT")
               route.kind = RouteRoleUpdate;
            else if (method == "DELETE")
               route.kind = RouteRoleDelete;

            if (route.kind != RouteUnknown)
               route.identifier = name;
         }
         return;
      }

      if (method == "GET" && path == "/api/v1/role-assignments")
      {
         route.kind = RouteRoleAssignmentList;
         return;
      }

      if (path.StartsWith("/api/v1/role-assignments/"))
      {
         AnsiString address = path.Mid(AnsiString("/api/v1/role-assignments/").GetLength());
         if (!address.IsEmpty() && address.Find("/") < 0)
         {
            if (method == "PUT")
               route.kind = RouteRoleAssign;
            else if (method == "DELETE")
               route.kind = RouteRoleUnassign;

            if (route.kind != RouteUnknown)
               route.identifier = address;
         }
         return;
      }

      if (method == "GET" && path == "/api/v1/status")
      {
         route.kind = RouteStatus;
         return;
      }

      if (method == "GET" && path == "/api/v1/domains")
      {
         route.kind = RouteDomainList;
         return;
      }

      if (method == "POST" && path == "/api/v1/domains")
      {
         route.kind = RouteDomainCreate;
         return;
      }

      // /api/v1/domains/<name>/accounts
      const AnsiString domainsPrefix = "/api/v1/domains/";

      // /api/v1/domains/<name> - the domain itself, read, changed or deleted.
      // The remainder has no slash in it, which is what keeps this apart from
      // every /api/v1/domains/<name>/<resource> route below: those all end in a
      // resource segment and this one ends in the name.
      if ((method == "GET" || method == "PUT" || method == "DELETE") && path.StartsWith(domainsPrefix))
      {
         AnsiString domainName = path.Mid(domainsPrefix.GetLength());

         if (!domainName.IsEmpty() && domainName.Find("/") < 0)
         {
            route.kind = method == "GET" ? RouteDomainGet
                       : method == "PUT" ? RouteDomainUpdate
                       : RouteDomainDelete;
            route.identifier = domainName;
            return;
         }
      }

      // A domain's own rules, under the domain (RestApiRules.cpp).
      if ((method == "GET" || method == "POST") && path.StartsWith(domainsPrefix) && path.EndsWith("/rules"))
      {
         AnsiString domainName = path.Mid(domainsPrefix.GetLength(), path.GetLength() - domainsPrefix.GetLength() - AnsiString("/rules").GetLength());
         if (!domainName.IsEmpty() && domainName.Find("/") < 0)
         {
            route.kind = method == "GET" ? RouteDomainRuleList : RouteDomainRuleCreate;
            route.identifier = domainName;
            return;
         }
      }

      if ((method == "PUT" || method == "DELETE") && path.StartsWith(domainsPrefix) && path.Find("/rules/") > 0)
      {
         const AnsiString rest = path.Mid(domainsPrefix.GetLength());
         const int at = rest.Find("/rules/");
         AnsiString domainName = rest.Mid(0, at);
         AnsiString idPart = rest.Mid(at + AnsiString("/rules/").GetLength());
         __int64 id = 0;
         if (!domainName.IsEmpty() && domainName.Find("/") < 0 && idPart.Find("/") < 0 && ParseQueueId(idPart, id))
         {
            route.kind = method == "PUT" ? RouteDomainRuleUpdate : RouteDomainRuleDelete;
            route.identifier = domainName;
            route.record_id = id;
            return;
         }
      }

      if (path.StartsWith(domainsPrefix) && path.EndsWith("/accounts"))
      {
         AnsiString domainName = path.Mid(domainsPrefix.GetLength(),
            path.GetLength() - domainsPrefix.GetLength() - AnsiString("/accounts").GetLength());

         if (!domainName.IsEmpty() && domainName.Find("/") < 0)
         {
            if (method == "GET")
            {
               route.kind = RouteAccountList;
               route.identifier = domainName;
               return;
            }

            if (method == "POST")
            {
               route.kind = RouteAccountCreate;
               route.identifier = domainName;
               return;
            }
         }
      }

      // /api/v1/accounts/<address>
      if (method == "PUT" && path == "/api/v1/portal/branding")
      {
         route.kind = RoutePortalBrandingPut;
         return;
      }

      // The policy for files sent as links, server-wide or a domain's own.
      if (path == "/api/v1/portal/files")
      {
         if (method == "GET")
            route.kind = RoutePortalFilesPolicy;
         else if (method == "PUT")
            route.kind = RoutePortalFilesPolicyPut;

         return;
      }

      const AnsiString accountsPrefix = "/api/v1/accounts/";

      // A password reset and an unlock, each its own route so that the
      // help-desk role can be allowed exactly these two and nothing else. They
      // do what PUT /api/v1/accounts/<address> with a password field and the
      // Control Panel's Unlock button have always done; what is new is that a
      // permission can now name them without naming the account update.
      if (method == "POST" && path.StartsWith(accountsPrefix) && path.EndsWith("/password"))
      {
         AnsiString address = path.Mid(accountsPrefix.GetLength(), path.GetLength() - accountsPrefix.GetLength() - AnsiString("/password").GetLength());
         if (!address.IsEmpty() && address.Find("/") < 0)
         {
            route.kind = RouteAccountPasswordReset;
            route.identifier = address;
            return;
         }
      }

      if (method == "POST" && path.StartsWith(accountsPrefix) && path.EndsWith("/unlock"))
      {
         AnsiString address = path.Mid(accountsPrefix.GetLength(), path.GetLength() - accountsPrefix.GetLength() - AnsiString("/unlock").GetLength());
         if (!address.IsEmpty() && address.Find("/") < 0)
         {
            route.kind = RouteAccountUnlock;
            route.identifier = address;
            return;
         }
      }

      // The account's review state under the compromised-account detection:
      // read, changed (exempt, sensitivity), and the compromise lock alone
      // cleared - the unlock above clears every lock the account is behind.
      if (path.StartsWith(accountsPrefix) && (path.EndsWith("/review") || path.EndsWith("/review/unlock")))
      {
         const bool unlock = path.EndsWith("/review/unlock");
         const AnsiString suffix = unlock ? "/review/unlock" : "/review";
         AnsiString address = path.Mid(accountsPrefix.GetLength(), path.GetLength() - accountsPrefix.GetLength() - suffix.GetLength());
         if (!address.IsEmpty() && address.Find("/") < 0)
         {
            if (unlock && method == "POST")
               route.kind = RouteAccountReviewUnlock;
            else if (!unlock && method == "GET")
               route.kind = RouteAccountReviewGet;
            else if (!unlock && method == "PUT")
               route.kind = RouteAccountReviewPut;
            else
               return;
            route.identifier = address;
            return;
         }
      }

      // An account's one-time recovery codes, ended by an administrator. A
      // separate act from clearing the address because the two answer separate
      // reports: "that address is not mine any more" and "somebody else has my
      // code card". A code stands in for the account's second factor, so a
      // leaked set is a standing way past it and somebody has to be able to end
      // one.
      if (method == "DELETE" && path.StartsWith(accountsPrefix) && path.EndsWith("/recovery/codes"))
      {
         AnsiString address = path.Mid(accountsPrefix.GetLength(), path.GetLength() - accountsPrefix.GetLength() - AnsiString("/recovery/codes").GetLength());
         if (!address.IsEmpty() && address.Find("/") < 0)
         {
            route.kind = RouteAccountRecoveryCodesClear;
            route.identifier = address;
            return;
         }
      }

      // An account's recovery address, cleared by an administrator: the person
      // sets one again from the webmail. There is no route that reads it whole.
      if (method == "DELETE" && path.StartsWith(accountsPrefix) && path.EndsWith("/recovery"))
      {
         AnsiString address = path.Mid(accountsPrefix.GetLength(), path.GetLength() - accountsPrefix.GetLength() - AnsiString("/recovery").GetLength());
         if (!address.IsEmpty() && address.Find("/") < 0)
         {
            route.kind = RouteAccountRecoveryClear;
            route.identifier = address;
            return;
         }
      }

      // An account's Web Push devices, for the administrator: seen, or all of
      // them stopped, which is what a lost phone needs.
      if ((method == "GET" || method == "DELETE") && path.StartsWith(accountsPrefix) && path.EndsWith("/push"))
      {
         AnsiString address = path.Mid(accountsPrefix.GetLength(), path.GetLength() - accountsPrefix.GetLength() - AnsiString("/push").GetLength());
         if (!address.IsEmpty() && address.Find("/") < 0)
         {
            route.kind = method == "GET" ? RouteAccountPushGet : RouteAccountPushRemove;
            route.identifier = address;
            return;
         }
      }

      // A mailbox renamed, or moved to another domain on this server. Its own
      // route rather than a field of the account update, for the reason the
      // reset and the unlock are: it is one operation with its own refusals, and
      // a permission can name it.
      if ((method == "GET" || method == "POST") && path.StartsWith(accountsPrefix) && path.EndsWith("/move"))
      {
         AnsiString address = path.Mid(accountsPrefix.GetLength(), path.GetLength() - accountsPrefix.GetLength() - AnsiString("/move").GetLength());
         if (!address.IsEmpty() && address.Find("/") < 0)
         {
            route.kind = method == "GET" ? RouteMailboxMoveGet : RouteMailboxMove;
            route.identifier = address;
            return;
         }
      }

      // An account's own rules, under the account (RestApiRules.cpp).
      if ((method == "GET" || method == "POST") && path.StartsWith(accountsPrefix) && path.EndsWith("/rules"))
      {
         AnsiString address = path.Mid(accountsPrefix.GetLength(), path.GetLength() - accountsPrefix.GetLength() - AnsiString("/rules").GetLength());
         if (!address.IsEmpty() && address.Find("/") < 0)
         {
            route.kind = method == "GET" ? RouteAccountRuleList : RouteAccountRuleCreate;
            route.identifier = address;
            return;
         }
      }

      if ((method == "PUT" || method == "DELETE") && path.StartsWith(accountsPrefix) && path.Find("/rules/") > 0)
      {
         const AnsiString rest = path.Mid(accountsPrefix.GetLength());
         const int at = rest.Find("/rules/");
         AnsiString address = rest.Mid(0, at);
         AnsiString idPart = rest.Mid(at + AnsiString("/rules/").GetLength());
         __int64 id = 0;
         if (!address.IsEmpty() && address.Find("/") < 0 && idPart.Find("/") < 0 && ParseQueueId(idPart, id))
         {
            route.kind = method == "PUT" ? RouteAccountRuleUpdate : RouteAccountRuleDelete;
            route.identifier = address;
            route.record_id = id;
            return;
         }
      }

      // Everything the server holds on the account, as one download
      // (RestApiOperator.cpp).
      if (method == "GET" && path.StartsWith(accountsPrefix) && path.EndsWith("/export.zip"))
      {
         AnsiString address = path.Mid(accountsPrefix.GetLength(), path.GetLength() - accountsPrefix.GetLength() - AnsiString("/export.zip").GetLength());
         if (!address.IsEmpty() && address.Find("/") < 0)
         {
            route.kind = RouteAccountExportArchive;
            route.identifier = address;
            return;
         }
      }

      // The mailbox written to the server's disk (RestApiOperator.cpp).
      if (method == "POST" && path.StartsWith(accountsPrefix) && path.EndsWith("/export"))
      {
         AnsiString address = path.Mid(accountsPrefix.GetLength(), path.GetLength() - accountsPrefix.GetLength() - AnsiString("/export").GetLength());
         if (!address.IsEmpty() && address.Find("/") < 0)
         {
            route.kind = RouteAccountExport;
            route.identifier = address;
            return;
         }
      }

      if (method == "POST" && path == "/api/v1/addresses/erase")
      {
         route.kind = RouteAddressErase;
         return;
      }

      if (path == "/api/v1/administrator/second-factor")
      {
         if (method == "GET") { route.kind = RouteAdminSecondFactorGet; return; }
         if (method == "POST") { route.kind = RouteAdminSecondFactorEnrol; return; }
         if (method == "DELETE") { route.kind = RouteAdminSecondFactorDisable; return; }
      }

      // An administrator opens a mailbox as its user, for support.
      if (method == "POST" && path.StartsWith(accountsPrefix) && path.EndsWith("/support-session"))
      {
         AnsiString address = path.Mid(accountsPrefix.GetLength(), path.GetLength() - accountsPrefix.GetLength() - AnsiString("/support-session").GetLength());
         if (!address.IsEmpty() && address.Find("/") < 0)
         {
            route.kind = RouteAccountSupportSession;
            route.identifier = address;
            return;
         }
      }

      // /api/v1/accounts/<address>/fetch-accounts[/<id>[/download]]: the
      // account's external accounts, under the address that scopes them.
      if (path.StartsWith(accountsPrefix) && path.Find("/fetch-accounts") > 0)
      {
         AnsiString rest = path.Mid(accountsPrefix.GetLength());
         int slash = rest.Find("/");
         AnsiString address = slash > 0 ? rest.Mid(0, slash) : AnsiString();
         AnsiString tail = slash > 0 ? rest.Mid(slash) : AnsiString();

         if (!address.IsEmpty() && tail == "/fetch-accounts")
         {
            if (method == "GET")
               route.kind = RouteFetchAccountList;
            else if (method == "POST")
               route.kind = RouteFetchAccountCreate;

            if (route.kind != RouteUnknown)
               route.identifier = address;
            return;
         }

         const AnsiString itemPrefix = "/fetch-accounts/";
         if (!address.IsEmpty() && tail.StartsWith(itemPrefix))
         {
            AnsiString idPart = tail.Mid(itemPrefix.GetLength());
            const AnsiString downloadSuffix = "/download";
            bool download = idPart.EndsWith(downloadSuffix);
            if (download)
               idPart = idPart.Mid(0, idPart.GetLength() - downloadSuffix.GetLength());

            __int64 id = 0;
            if (idPart.Find("/") < 0 && ParseQueueId(idPart, id))
            {
               if (download && method == "POST")
                  route.kind = RouteFetchAccountDownload;
               else if (!download && method == "GET")
                  route.kind = RouteFetchAccountGet;
               else if (!download && method == "PUT")
                  route.kind = RouteFetchAccountUpdate;
               else if (!download && method == "DELETE")
                  route.kind = RouteFetchAccountDelete;

               if (route.kind != RouteUnknown)
               {
                  route.identifier = address;
                  route.record_id = id;
               }
            }
            return;
         }
      }

      if ((method == "DELETE" || method == "PUT" || method == "GET") && path.StartsWith(accountsPrefix))
      {
         AnsiString address = path.Mid(accountsPrefix.GetLength());

         if (!address.IsEmpty() && address.Find("/") < 0)
         {
            route.kind = method == "DELETE" ? RouteAccountDelete : method == "PUT" ? RouteAccountUpdate : RouteAccountGet;
            route.identifier = address;
            return;
         }
      }

      // /api/v1/accounts/<address>/<resource>...: the account's resources
      // administered under its address (RestApiAccountResources.cpp), which
      // owns the tails; the address is split off here once.
      if (path.StartsWith(accountsPrefix))
      {
         AnsiString rest = path.Mid(accountsPrefix.GetLength());
         int slash = rest.Find("/");
         if (slash > 0)
         {
            AnsiString address = rest.Mid(0, slash);
            if (ParseAccountResourceRoute_(method, rest.Mid(slash), route))
            {
               route.identifier = address;
               return;
            }
         }
      }

      if (method == "GET" && path == "/api/v1/queue")
      {
         route.kind = RouteQueueList;
         return;
      }

      // The administrator runs what the webmail scheduled, now.
      if (path == "/api/v1/scheduled/run")
      {
         if (method == "POST")
            route.kind = RouteScheduledRun;
         return;
      }

      // /api/v1/queue/<id>/retry and /api/v1/queue/<id>
      const AnsiString queuePrefix = "/api/v1/queue/";

      if (path.StartsWith(queuePrefix))
      {
         AnsiString remainder = path.Mid(queuePrefix.GetLength());

         if (method == "POST" && remainder.EndsWith("/retry"))
         {
            AnsiString idPart = remainder.Mid(0, remainder.GetLength() - AnsiString("/retry").GetLength());

            __int64 messageId = 0;
            if (ParseQueueId(idPart, messageId))
            {
               route.kind = RouteQueueRetry;
               route.message_id = messageId;
               return;
            }
         }

         if (method == "DELETE" && remainder.Find("/") < 0)
         {
            __int64 messageId = 0;
            if (ParseQueueId(remainder, messageId))
            {
               route.kind = RouteQueueDelete;
               route.message_id = messageId;
               return;
            }
         }
      }

      if (method == "GET" && path == "/api/v1/tlsa")
      {
         route.kind = RouteTlsa;
         return;
      }

      if (method == "GET" && path == "/api/v1/srv")
      {
         route.kind = RouteSrv;
         return;
      }

      if (method == "GET" && path == "/api/v1/metrics/history")
      {
         route.kind = RouteMetricsHistory;
         return;
      }

      // The reports. /api/v1/reports is the index - what sections there are,
      // what each is built from and what this server cannot answer - and
      // /api/v1/reports/<section> is one of them. The section is put in name
      // and the domain the query asks about in identifier, both so that
      // Authorize_ can decide without parsing anything itself: a route is
      // data by the time it reaches the choke point.
      const AnsiString reportsPath = "/api/v1/reports";

      if (method == "GET" && (path == reportsPath || path.StartsWith(reportsPath + "/")))
      {
         AnsiString section = path == reportsPath ? AnsiString("") : path.Mid(reportsPath.GetLength() + 1);

         if (section.Find("/") < 0)
         {
            route.kind = RouteReport;
            route.name = section;
            route.identifier = QueryParameter_(route.query, "domain");
            return;
         }
      }

      if (method == "GET" && path == "/api/v1/update")
      {
         route.kind = RouteUpdateGet;
         return;
      }

      if (method == "POST" && path == "/api/v1/update/check")
      {
         route.kind = RouteUpdateCheck;
         return;
      }

      if (method == "POST" && path == "/api/v1/update/download")
      {
         route.kind = RouteUpdateDownload;
         return;
      }

      if (method == "POST" && path == "/api/v1/update/install")
      {
         route.kind = RouteUpdateInstall;
         return;
      }

      if (method == "POST" && path == "/api/v1/diagnostics/run")
      {
         route.kind = RouteDiagnosticsRun;
         return;
      }

      // /api/v1/quarantine, /api/v1/quarantine/<id>/release, /api/v1/quarantine/<id>
      if (method == "GET" && path == "/api/v1/quarantine")
      {
         route.kind = RouteQuarantineList;
         return;
      }

      const AnsiString quarantinePrefix = "/api/v1/quarantine/";

      if (path.StartsWith(quarantinePrefix))
      {
         AnsiString remainder = path.Mid(quarantinePrefix.GetLength());

         if (method == "POST" && remainder.EndsWith("/release"))
         {
            AnsiString idPart = remainder.Mid(0, remainder.GetLength() - AnsiString("/release").GetLength());

            __int64 quarantineId = 0;
            if (ParseQueueId(idPart, quarantineId))
            {
               route.kind = RouteQuarantineRelease;
               route.message_id = quarantineId;
               return;
            }
         }

         if (method == "DELETE" && remainder.Find("/") < 0)
         {
            __int64 quarantineId = 0;
            if (ParseQueueId(remainder, quarantineId))
            {
               route.kind = RouteQuarantineDelete;
               route.message_id = quarantineId;
               return;
            }
         }
      }

      // /api/v1/domains/<name>/aliases - same shape as the accounts listing.
      // /api/v1/domains/<name>/domain-aliases, and one alias under it by name.
      if ((method == "GET" || method == "POST") && path.StartsWith(domainsPrefix) && path.EndsWith("/domain-aliases"))
      {
         AnsiString domainName = path.Mid(domainsPrefix.GetLength(),
            path.GetLength() - domainsPrefix.GetLength() - AnsiString("/domain-aliases").GetLength());

         if (!domainName.IsEmpty() && domainName.Find("/") < 0)
         {
            route.kind = method == "GET" ? RouteDomainAliasList : RouteDomainAliasCreate;
            route.identifier = domainName;
            return;
         }
      }

      if (method == "DELETE" && path.StartsWith(domainsPrefix) && path.Find("/domain-aliases/") > 0)
      {
         AnsiString rest = path.Mid(domainsPrefix.GetLength());
         const AnsiString marker = "/domain-aliases/";
         int at = rest.Find(marker);
         AnsiString domainName = at > 0 ? rest.Mid(0, at) : AnsiString();
         AnsiString aliasName = at > 0 ? rest.Mid(at + marker.GetLength()) : AnsiString();

         if (!domainName.IsEmpty() && domainName.Find("/") < 0 && !aliasName.IsEmpty() && aliasName.Find("/") < 0)
         {
            route.kind = RouteDomainAliasDelete;
            route.identifier = domainName;
            route.name = aliasName;
            return;
         }
      }

      // /api/v1/domains/<name>/suppressions, one entry under it by address, and
      // /api/v1/domains/<name>/bounces. The address is the last segment and may
      // be percent-encoded; the handler decodes it.
      if ((method == "GET" || method == "POST") && path.StartsWith(domainsPrefix) && path.EndsWith("/suppressions"))
      {
         AnsiString domainName = path.Mid(domainsPrefix.GetLength(),
            path.GetLength() - domainsPrefix.GetLength() - AnsiString("/suppressions").GetLength());

         if (!domainName.IsEmpty() && domainName.Find("/") < 0)
         {
            route.kind = method == "GET" ? RouteSuppressionList : RouteSuppressionCreate;
            route.identifier = domainName;
            return;
         }
      }

      if (method == "DELETE" && path.StartsWith(domainsPrefix) && path.Find("/suppressions/") > 0)
      {
         AnsiString rest = path.Mid(domainsPrefix.GetLength());
         const AnsiString marker = "/suppressions/";
         int at = rest.Find(marker);
         AnsiString domainName = at > 0 ? rest.Mid(0, at) : AnsiString();
         AnsiString address = at > 0 ? rest.Mid(at + marker.GetLength()) : AnsiString();

         if (!domainName.IsEmpty() && domainName.Find("/") < 0 && !address.IsEmpty() && address.Find("/") < 0)
         {
            route.kind = RouteSuppressionDelete;
            route.identifier = domainName;
            route.name = address;
            return;
         }
      }

      // Every OpenPGP key held for the accounts of one domain.
      if (method == "GET" && path.StartsWith(domainsPrefix) && path.EndsWith("/openpgp"))
      {
         AnsiString domainName = path.Mid(domainsPrefix.GetLength(),
            path.GetLength() - domainsPrefix.GetLength() - AnsiString("/openpgp").GetLength());

         if (!domainName.IsEmpty() && domainName.Find("/") < 0)
         {
            route.kind = RouteDomainOpenPgpList;
            route.identifier = domainName;
            return;
         }
      }

      if (method == "GET" && path.StartsWith(domainsPrefix) && path.EndsWith("/bounces"))
      {
         AnsiString domainName = path.Mid(domainsPrefix.GetLength(),
            path.GetLength() - domainsPrefix.GetLength() - AnsiString("/bounces").GetLength());

         if (!domainName.IsEmpty() && domainName.Find("/") < 0)
         {
            route.kind = RouteBounceCountList;
            route.identifier = domainName;
            return;
         }
      }

      if (method == "GET" && path.StartsWith(domainsPrefix) && path.EndsWith("/session-policy"))
      {
         AnsiString domainName = path.Mid(domainsPrefix.GetLength(),
            path.GetLength() - domainsPrefix.GetLength() - AnsiString("/session-policy").GetLength());

         if (!domainName.IsEmpty() && domainName.Find("/") < 0)
         {
            route.kind = RouteDomainSessionPolicyGet;
            route.identifier = domainName;
            return;
         }
      }

      // The reports our own domains receive (RestApiDomainReports.cpp): the
      // summary, the listing, one report by id, and the TLS-RPT reports, all
      // under the domain.
      if (method == "GET" && path.StartsWith(domainsPrefix))
      {
         const AnsiString remainder = path.Mid(domainsPrefix.GetLength());
         const int slash = remainder.Find("/");

         if (slash > 0)
         {
            const AnsiString domainName = remainder.Mid(0, slash);
            const AnsiString rest = remainder.Mid(slash);

            if (rest == "/dmarc-reports")
            {
               route.kind = RouteDomainReportsList;
               route.identifier = domainName;
               return;
            }

            if (rest == "/dmarc-reports/summary")
            {
               route.kind = RouteDomainReportsSummary;
               route.identifier = domainName;
               return;
            }

            if (rest == "/tls-reports")
            {
               route.kind = RouteDomainTlsReportsList;
               route.identifier = domainName;
               return;
            }

            const AnsiString reportPrefix = "/dmarc-reports/";
            if (rest.StartsWith(reportPrefix))
            {
               const AnsiString idText = rest.Mid(reportPrefix.GetLength());
               __int64 id = 0;

               if (idText.Find("/") < 0 && ParseQueueId(idText, id))
               {
                  route.kind = RouteDomainReportGet;
                  route.identifier = domainName;
                  route.record_id = id;
                  return;
               }
            }
         }
      }

      // The abuse reports a domain's mail has drawn (RestApiComplaints.cpp).
      if (method == "GET" && path.StartsWith(domainsPrefix) && path.EndsWith("/complaints"))
      {
         AnsiString domainName = path.Mid(domainsPrefix.GetLength(),
            path.GetLength() - domainsPrefix.GetLength() - AnsiString("/complaints").GetLength());

         if (!domainName.IsEmpty() && domainName.Find("/") < 0)
         {
            route.kind = RouteComplaintList;
            route.identifier = domainName;
            return;
         }
      }

      if ((method == "GET" || method == "POST") && path.StartsWith(domainsPrefix) && path.EndsWith("/aliases"))
      {
         AnsiString domainName = path.Mid(domainsPrefix.GetLength(),
            path.GetLength() - domainsPrefix.GetLength() - AnsiString("/aliases").GetLength());

         if (!domainName.IsEmpty() && domainName.Find("/") < 0)
         {
            route.kind = method == "GET" ? RouteAliasList : RouteAliasCreate;
            route.identifier = domainName;
            return;
         }
      }

      // /api/v1/aliases/<address> - the alias itself, named by its address as
      // an account is; scoped to the address's domain in Authorize_.
      const AnsiString aliasesPrefix = "/api/v1/aliases/";

      if (method == "DELETE" && path.StartsWith(aliasesPrefix))
      {
         AnsiString address = path.Mid(aliasesPrefix.GetLength());

         if (!address.IsEmpty() && address.Find("/") < 0)
         {
            route.kind = RouteAliasDelete;
            route.identifier = address;
            return;
         }
      }

      // Wave 88: the surfaces that were COM-only.
      const AnsiString ipRangesPath = "/api/v1/ipranges";
      if (path == ipRangesPath)
      {
         if (method == "GET")
            route.kind = RouteIpRangeList;
         else if (method == "POST")
            route.kind = RouteIpRangeCreate;
         return;
      }
      if ((method == "DELETE" || method == "PUT") && path.StartsWith(ipRangesPath + "/"))
      {
         AnsiString idPart = path.Mid(ipRangesPath.GetLength() + 1);
         __int64 rangeId = 0;
         if (idPart.Find("/") < 0 && ParseQueueId(idPart, rangeId))
         {
            route.kind = method == "DELETE" ? RouteIpRangeDelete : RouteIpRangeUpdate;
            route.range_id = rangeId;
            return;
         }
      }

      // The five small collections (RestApiAntiSpamLists.cpp), each in the
      // IP range's shape: a list and a create at the collection's path, an
      // update and a delete at /{id}.
      {
         struct SmallCollection
         {
            const char *path;
            RouteKind list;
            RouteKind create;
            RouteKind update;
            RouteKind remove;
         };
         static const SmallCollection collections[] =
         {
            { "/api/v1/dns-blacklists", RouteDnsBlackListList, RouteDnsBlackListCreate, RouteDnsBlackListUpdate, RouteDnsBlackListDelete },
            { "/api/v1/surbl-servers", RouteSurblServerList, RouteSurblServerCreate, RouteSurblServerUpdate, RouteSurblServerDelete },
            { "/api/v1/whitelist-addresses", RouteWhiteListAddressList, RouteWhiteListAddressCreate, RouteWhiteListAddressUpdate, RouteWhiteListAddressDelete },
            { "/api/v1/blocked-senders", RouteBlockedSenderList, RouteBlockedSenderCreate, RouteBlockedSenderUpdate, RouteBlockedSenderDelete },
            { "/api/v1/incoming-relays", RouteIncomingRelayList, RouteIncomingRelayCreate, RouteIncomingRelayUpdate, RouteIncomingRelayDelete },
            { "/api/v1/blocked-attachments", RouteBlockedAttachmentList, RouteBlockedAttachmentCreate, RouteBlockedAttachmentUpdate, RouteBlockedAttachmentDelete },
            { "/api/v1/journal-rules", RouteJournalRuleList, RouteJournalRuleCreate, RouteJournalRuleUpdate, RouteJournalRuleDelete },
            { "/api/v1/greylisting-white-addresses", RouteGreyListingWhiteAddressList, RouteGreyListingWhiteAddressCreate, RouteGreyListingWhiteAddressUpdate, RouteGreyListingWhiteAddressDelete },
            { "/api/v1/greylisting-bypass", RouteGreylistBypassList, RouteGreylistBypassCreate, RouteGreylistBypassUpdate, RouteGreylistBypassDelete },
            { "/api/v1/sender-list", RouteSenderListList, RouteSenderListCreate, RouteSenderListUpdate, RouteSenderListDelete },
            { "/api/v1/domain-policy-list", RouteDomainPolicyList, RouteDomainPolicyCreate, RouteDomainPolicyUpdate, RouteDomainPolicyDelete },
         };

         for (const SmallCollection &collection : collections)
         {
            const AnsiString base = collection.path;

            if (path == base)
            {
               if (method == "GET")
                  route.kind = collection.list;
               else if (method == "POST")
                  route.kind = collection.create;
               return;
            }

            if ((method == "PUT" || method == "DELETE") && path.StartsWith(base + "/"))
            {
               AnsiString idPart = path.Mid(base.GetLength() + 1);
               __int64 id = 0;
               if (idPart.Find("/") < 0 && ParseQueueId(idPart, id))
               {
                  route.kind = method == "PUT" ? collection.update : collection.remove;
                  route.record_id = id;
               }
               return;
            }
         }
      }

      // The account groups (RestApiGroups.cpp): the collection, a group by
      // id, its members, and a member by the account's id.
      const AnsiString groupsPath = "/api/v1/groups";
      if (path == groupsPath)
      {
         if (method == "GET")
            route.kind = RouteGroupList;
         else if (method == "POST")
            route.kind = RouteGroupCreate;
         return;
      }
      if (path.StartsWith(groupsPath + "/"))
      {
         AnsiString rest = path.Mid(groupsPath.GetLength() + 1);
         int slash = rest.Find("/");
         AnsiString idPart = slash < 0 ? rest : rest.Mid(0, slash);
         AnsiString sub = slash < 0 ? AnsiString() : rest.Mid(slash);

         __int64 groupId = 0;
         if (!ParseQueueId(idPart, groupId))
            return;

         const AnsiString members = "/members";
         if (sub.IsEmpty())
         {
            if (method == "GET")
               route.kind = RouteGroupGet;
            else if (method == "PUT")
               route.kind = RouteGroupUpdate;
            else if (method == "DELETE")
               route.kind = RouteGroupDelete;
         }
         else if (sub == members)
         {
            if (method == "GET")
               route.kind = RouteGroupMemberList;
            else if (method == "POST")
               route.kind = RouteGroupMemberCreate;
         }
         else if (method == "DELETE" && sub.StartsWith(members + "/"))
         {
            AnsiString accountPart = sub.Mid(members.GetLength() + 1);
            __int64 accountId = 0;
            if (accountPart.Find("/") < 0 && ParseQueueId(accountPart, accountId))
            {
               route.kind = RouteGroupMemberDelete;
               route.member_account_id = accountId;
            }
         }

         if (route.kind != RouteUnknown)
            route.record_id = groupId;
         return;
      }
      if (path.StartsWith(domainsPrefix) && path.EndsWith("/lists"))
      {
         AnsiString domainName = path.Mid(domainsPrefix.GetLength(),
            path.GetLength() - domainsPrefix.GetLength() - AnsiString("/lists").GetLength());
         if (!domainName.IsEmpty() && domainName.Find("/") < 0)
         {
            if (method == "GET")
            {
               route.kind = RouteListList;
               route.identifier = domainName;
               return;
            }
            if (method == "POST")
            {
               route.kind = RouteListCreate;
               route.identifier = domainName;
               return;
            }
         }
      }
      // A domain's spam traps, in the shape an alias has: listed and created
      // under the domain, removed at the address itself - an address is unique
      // in itself and names the domain it belongs to, so the same rule that
      // scopes an alias delete to a domain scopes this one. The record of hits
      // is a verb of the collection; "hits" is not an address, so the two cannot
      // be confused for one another.
      {
         const AnsiString trapsSuffix = "/spam-traps";
         const AnsiString trapHitsSuffix = "/spam-traps/hits";

         if (method == "GET" && path.StartsWith(domainsPrefix) && path.EndsWith(trapHitsSuffix))
         {
            AnsiString domainName = path.Mid(domainsPrefix.GetLength(),
               path.GetLength() - domainsPrefix.GetLength() - trapHitsSuffix.GetLength());

            if (!domainName.IsEmpty() && domainName.Find("/") < 0)
            {
               route.kind = RouteSpamTrapDomainHits;
               route.identifier = domainName;
               return;
            }
         }

         if (path.StartsWith(domainsPrefix) && path.EndsWith(trapsSuffix))
         {
            AnsiString domainName = path.Mid(domainsPrefix.GetLength(),
               path.GetLength() - domainsPrefix.GetLength() - trapsSuffix.GetLength());

            if (!domainName.IsEmpty() && domainName.Find("/") < 0)
            {
               if (method == "GET")
               {
                  route.kind = RouteSpamTrapDomainList;
                  route.identifier = domainName;
                  return;
               }

               if (method == "POST")
               {
                  route.kind = RouteSpamTrapDomainCreate;
                  route.identifier = domainName;
                  return;
               }
            }
         }

         if (path == "/api/v1/spam-traps/hits")
         {
            if (method == "GET")
            {
               route.kind = RouteSpamTrapHits;
               return;
            }

            if (method == "DELETE")
            {
               route.kind = RouteSpamTrapHitsClear;
               return;
            }
         }

         const AnsiString trapsPrefix = "/api/v1/spam-traps/";

         if (method == "DELETE" && path.StartsWith(trapsPrefix))
         {
            AnsiString address = path.Mid(trapsPrefix.GetLength());

            if (!address.IsEmpty() && address.Find("/") < 0)
            {
               // Percent-decoded, because the identifier is an ADDRESS and every
               // client that builds this path encodes the @ in it: the Deck's
               // encodeURIComponent and the module's EscapeDataString both send
               // %40. An identifier still holding %40 has no @ in it at all, so
               // the handler answered "no such spam trap" for a trap that was
               // there, and DomainNamedByRoute_ - which scopes this route to the
               // domain in the address - could not read a domain out of it either.
               // Decoded after the segment test, so a %2F cannot smuggle in a
               // second path segment.
               const AnsiString decoded = Dav::PercentDecode(address);

               if (!decoded.IsEmpty() && decoded.Find("/") < 0)
               {
                  route.kind = RouteSpamTrapDelete;
                  route.identifier = decoded;
                  return;
               }
            }
         }
      }

      // One domain's allow and block list, in the shape the distribution lists
      // use: the collection under its domain, the entry under the collection.
      // The id stays inside the domain's path rather than moving to a flat
      // /api/v1/sender-list/{id}, so that a key restricted to some domains is
      // scoped by the same rule that scopes every other domain resource.
      {
         const AnsiString senderListSuffix = "/sender-list";

         if (path.StartsWith(domainsPrefix) && path.EndsWith(senderListSuffix))
         {
            AnsiString domainName = path.Mid(domainsPrefix.GetLength(),
               path.GetLength() - domainsPrefix.GetLength() - senderListSuffix.GetLength());

            if (!domainName.IsEmpty() && domainName.Find("/") < 0)
            {
               if (method == "GET")
               {
                  route.kind = RouteSenderListDomainList;
                  route.identifier = domainName;
                  return;
               }

               if (method == "POST")
               {
                  route.kind = RouteSenderListDomainCreate;
                  route.identifier = domainName;
                  return;
               }
            }
         }

         const AnsiString senderListEntry = "/sender-list/";
         const int entryAt = path.Find(senderListEntry);

         if ((method == "PUT" || method == "DELETE") && path.StartsWith(domainsPrefix) && entryAt > 0)
         {
            AnsiString domainName = path.Mid(domainsPrefix.GetLength(), entryAt - domainsPrefix.GetLength());
            AnsiString idPart = path.Mid(entryAt + senderListEntry.GetLength());
            __int64 id = 0;

            if (!domainName.IsEmpty() && domainName.Find("/") < 0 && idPart.Find("/") < 0 && ParseQueueId(idPart, id))
            {
               route.kind = method == "PUT" ? RouteSenderListDomainUpdate : RouteSenderListDomainDelete;
               route.identifier = domainName;
               route.record_id = id;
               return;
            }
         }
      }

      const AnsiString listsPrefix = "/api/v1/lists/";
      if ((method == "DELETE" || method == "PUT") && path.StartsWith(listsPrefix))
      {
         AnsiString address = path.Mid(listsPrefix.GetLength());
         if (!address.IsEmpty() && address.Find("/") < 0)
         {
            route.kind = method == "DELETE" ? RouteListDelete : RouteListUpdate;
            route.identifier = address;
            return;
         }
      }
      // Who the list delivers to now: its rows and what its membership query
      // answers. "members" is a verb of the list rather than an address, and
      // an address holds no slash, so the two cannot be confused.
      if (method == "GET" && path.StartsWith(listsPrefix) && path.EndsWith("/members"))
      {
         const AnsiString membersSuffix = "/members";
         AnsiString address = path.Mid(listsPrefix.GetLength(),
            path.GetLength() - listsPrefix.GetLength() - membersSuffix.GetLength());
         if (!address.IsEmpty() && address.Find("/") < 0)
         {
            route.kind = RouteListMembers;
            route.identifier = address;
            return;
         }
      }
      // /api/v1/domains/<name>/dkim/rotation[/start|/check|/switch|/retire|/next]
      // - the domain's key rotation. Matched before the plain /dkim route below,
      // whose EndsWith these paths do not satisfy anyway, and only with a domain
      // segment that holds no slash.
      if (path.StartsWith(domainsPrefix) && path.Find("/dkim/rotation") > 0)
      {
         const AnsiString rotation = "/dkim/rotation";
         AnsiString rest = path.Mid(domainsPrefix.GetLength());
         int at = rest.Find(rotation);
         AnsiString domainName = at > 0 ? rest.Mid(0, at) : AnsiString("");
         AnsiString action = at > 0 ? rest.Mid(at + rotation.GetLength()) : AnsiString("");

         if (!domainName.IsEmpty() && domainName.Find("/") < 0)
         {
            if (method == "GET" && action.IsEmpty())
               route.kind = RouteDkimRotationGet;
            else if (method == "POST" && action == "/start")
               route.kind = RouteDkimRotationStart;
            else if (method == "POST" && action == "/check")
               route.kind = RouteDkimRotationCheck;
            else if (method == "POST" && action == "/switch")
               route.kind = RouteDkimRotationSwitch;
            else if (method == "POST" && action == "/retire")
               route.kind = RouteDkimRotationRetire;
            else if (method == "DELETE" && action == "/next")
               route.kind = RouteDkimRotationCancel;

            if (route.kind != RouteUnknown)
            {
               route.identifier = domainName;
               return;
            }
         }
      }
      if (method == "POST" && path == "/api/v1/dkim/rotation/run")
      {
         route.kind = RouteDkimRotationRun;
         return;
      }
      if (method == "GET" && path == "/api/v1/reputation")
      {
         route.kind = RouteReputationGet;
         return;
      }
      if (method == "POST" && path == "/api/v1/reputation/check")
      {
         route.kind = RouteReputationCheck;
         return;
      }

      // Held for review: the messages the compromised-account detection kept
      // out of the delivery queue, and releasing or discarding one.
      if (method == "GET" && path == "/api/v1/review")
      {
         route.kind = RouteReviewList;
         return;
      }
      if (path.StartsWith("/api/v1/review/"))
      {
         AnsiString rest = path.Mid(AnsiString("/api/v1/review/").GetLength());
         const bool release = rest.EndsWith("/release");
         if (release)
            rest = rest.Mid(0, rest.GetLength() - AnsiString("/release").GetLength());

         if (!ParseQueueId(rest, route.record_id))
            return;

         if (release && method == "POST")
            route.kind = RouteReviewRelease;
         else if (!release && method == "DELETE")
            route.kind = RouteReviewDiscard;

         return;
      }
      if (method == "GET" && path == "/api/v1/setup/checklist")
      {
         route.kind = RouteSetupChecklist;
         return;
      }
      if (path == "/api/v1/setup/sample-domain")
      {
         if (method == "GET")
            route.kind = RouteSampleDomainGet;
         else if (method == "POST")
            route.kind = RouteSampleDomainCreate;
         else if (method == "DELETE")
            route.kind = RouteSampleDomainRemove;

         if (route.kind != RouteUnknown)
            return;
      }
      if (method == "GET" && path.StartsWith(domainsPrefix) && path.EndsWith("/dkim"))
      {
         AnsiString domainName = path.Mid(domainsPrefix.GetLength(),
            path.GetLength() - domainsPrefix.GetLength() - AnsiString("/dkim").GetLength());
         if (!domainName.IsEmpty() && domainName.Find("/") < 0)
         {
            route.kind = RouteDkimGet;
            route.identifier = domainName;
            return;
         }
      }
      if (method == "GET" && path == "/api/v1/certificates")
      {
         route.kind = RouteCertificateList;
         return;
      }

      if (method == "POST" && path == "/api/v1/certificates")
      {
         route.kind = RouteCertificateCreate;
         return;
      }

      // The numbered resources of the write surface: /api/v1/<collection>/<id>
      // with a decimal id and nothing after it, as an IP range is named.
      const AnsiString certificatesPrefix = "/api/v1/certificates/";

      if (method == "DELETE" && path.StartsWith(certificatesPrefix))
      {
         AnsiString idPart = path.Mid(certificatesPrefix.GetLength());
         __int64 id = 0;
         if (idPart.Find("/") < 0 && ParseQueueId(idPart, id))
         {
            route.kind = RouteCertificateDelete;
            route.record_id = id;
            return;
         }
      }

      if (path == "/api/v1/ports" && (method == "GET" || method == "POST"))
      {
         route.kind = method == "GET" ? RoutePortList : RoutePortCreate;
         return;
      }

      const AnsiString portsPrefix = "/api/v1/ports/";

      if ((method == "PUT" || method == "DELETE") && path.StartsWith(portsPrefix))
      {
         AnsiString idPart = path.Mid(portsPrefix.GetLength());
         __int64 id = 0;
         if (idPart.Find("/") < 0 && ParseQueueId(idPart, id))
         {
            route.kind = method == "PUT" ? RoutePortUpdate : RoutePortDelete;
            route.record_id = id;
            return;
         }
      }

      if (path == "/api/v1/routes" && (method == "GET" || method == "POST"))
      {
         route.kind = method == "GET" ? RouteRouteList : RouteRouteCreate;
         return;
      }

      if (path == "/api/v1/remote-domains" && (method == "GET" || method == "POST"))
      {
         route.kind = method == "GET" ? RouteRemoteDomainList : RouteRemoteDomainCreate;
         return;
      }

      // Before the {id} tail below, so that neither word is ever read as a
      // decimal id: "effective" and "verification-cache" are not ids, and
      // ParseQueueId would refuse them, but the order is stated rather than
      // relied upon. The query string has already been split off the path.
      if (method == "GET" && path == "/api/v1/remote-domains/effective")
      {
         route.kind = RouteRemoteDomainEffective;
         return;
      }

      if (method == "POST" && path == "/api/v1/remote-domains/verification-cache/clear")
      {
         route.kind = RouteRemoteDomainClearCache;
         return;
      }

      const AnsiString remoteDomainsPrefix = "/api/v1/remote-domains/";

      if ((method == "PUT" || method == "DELETE") && path.StartsWith(remoteDomainsPrefix))
      {
         AnsiString idPart = path.Mid(remoteDomainsPrefix.GetLength());
         __int64 id = 0;
         if (idPart.Find("/") < 0 && ParseQueueId(idPart, id))
         {
            route.kind = method == "PUT" ? RouteRemoteDomainUpdate : RouteRemoteDomainDelete;
            route.record_id = id;
            return;
         }
      }

      const AnsiString routesPrefix = "/api/v1/routes/";

      if ((method == "PUT" || method == "DELETE") && path.StartsWith(routesPrefix))
      {
         AnsiString idPart = path.Mid(routesPrefix.GetLength());
         __int64 id = 0;
         if (idPart.Find("/") < 0 && ParseQueueId(idPart, id))
         {
            route.kind = method == "PUT" ? RouteRouteUpdate : RouteRouteDelete;
            route.record_id = id;
            return;
         }
      }
      if (method == "GET" && path == "/api/v1/rules")
      {
         route.kind = RouteRuleList;
         return;
      }

      if (method == "POST" && path == "/api/v1/rules")
      {
         route.kind = RouteRuleCreate;
         return;
      }

      // A criterion tried against a value, beside the rules it is written
      // for; "match" is no rule id, so the id routes below never claim it.
      if (method == "POST" && path == "/api/v1/rules/match")
      {
         route.kind = RouteRuleMatch;
         return;
      }

      const AnsiString rulesPrefix = "/api/v1/rules/";

      if ((method == "PUT" || method == "DELETE") && path.StartsWith(rulesPrefix))
      {
         AnsiString idPart = path.Mid(rulesPrefix.GetLength());
         __int64 id = 0;
         if (idPart.Find("/") < 0 && ParseQueueId(idPart, id))
         {
            route.kind = method == "PUT" ? RouteRuleUpdate : RouteRuleDelete;
            route.record_id = id;
            return;
         }
      }
      if (method == "GET" && path == "/api/v1/logs")
      {
         route.kind = RouteLogList;
         return;
      }

      if (method == "GET" && path == "/api/v1/message-trace")
      {
         route.kind = RouteMessageTraceRecent;
         return;
      }

      if (method == "GET" && path.StartsWith("/api/v1/message-trace/address/"))
      {
         AnsiString address = path.Mid(AnsiString("/api/v1/message-trace/address/").GetLength());
         if (!address.IsEmpty() && address.Find("/") < 0)
         {
            route.kind = RouteMessageTraceAddress;
            route.identifier = address;
         }
         return;
      }

      if (method == "GET" && path.StartsWith("/api/v1/message-trace/queue/"))
      {
         AnsiString idPart = path.Mid(AnsiString("/api/v1/message-trace/queue/").GetLength());
         __int64 queueId = 0;
         if (idPart.Find("/") < 0 && ParseQueueId(idPart, queueId))
         {
            route.kind = RouteMessageTraceQueue;
            route.identifier = idPart;
         }
         return;
      }
      // The live sessions and one of them followed (RestApiSessionFollow.cpp).
      // Before the log-file tail below, whose {name} would otherwise take
      // "sessions" for a file.
      if (method == "GET" && path == "/api/v1/logs/sessions")
      {
         route.kind = RouteLogSessions;
         return;
      }

      const AnsiString sessionsPrefix = "/api/v1/logs/sessions/";
      if (method == "GET" && path.StartsWith(sessionsPrefix))
      {
         AnsiString idPart = path.Mid(sessionsPrefix.GetLength());
         __int64 sessionId = 0;
         if (idPart.Find("/") < 0 && ParseQueueId(idPart, sessionId))
         {
            route.kind = RouteLogSessionFollow;
            route.record_id = sessionId;
         }
         return;
      }

      const AnsiString logsPrefix = "/api/v1/logs/";
      if (method == "GET" && path.StartsWith(logsPrefix))
      {
         AnsiString name = path.Mid(logsPrefix.GetLength());
         if (!name.IsEmpty() && name.Find("/") < 0)
         {
            route.kind = RouteLogTail;
            route.identifier = name;
            return;
         }
      }
      if (path == "/api/v1/backup/restore" && method == "POST")
      {
         route.kind = RouteBackupRestore;
         return;
      }
      if (path == "/api/v1/backup")
      {
         if (method == "POST")
            route.kind = RouteBackupStart;
         else if (method == "GET")
            route.kind = RouteBackupStatus;
         return;
      }
      if (method == "GET" && path == "/api/v1/settings")
      {
         route.kind = RouteSettingsGet;
         return;
      }

      if (method == "PUT" && path == "/api/v1/settings")
      {
         route.kind = RouteSettingsPut;
         return;
      }

      if (path == "/api/v1/settings/antispam" && (method == "GET" || method == "PUT"))
      {
         route.kind = method == "GET" ? RouteSettingsAntiSpamGet : RouteSettingsAntiSpamPut;
         return;
      }

      if (path == "/api/v1/settings/antivirus" && (method == "GET" || method == "PUT"))
      {
         route.kind = method == "GET" ? RouteSettingsAntiVirusGet : RouteSettingsAntiVirusPut;
         return;
      }

      if (path == "/api/v1/settings/logging" && (method == "GET" || method == "PUT"))
      {
         route.kind = method == "GET" ? RouteSettingsLoggingGet : RouteSettingsLoggingPut;
         return;
      }

      if (method == "GET" && path == "/api/v1/settings/directories")
      {
         route.kind = RouteSettingsDirectoriesGet;
         return;
      }

      if (method == "GET" && path == "/api/v1/settings/changed")
      {
         route.kind = RouteSettingsChanged;
         return;
      }

      // One [Settings] key of hMailServer.ini, named in the path as it is
      // spelled in the file. The name is not decoded: the characters a key
      // cannot hold - = [ ] and a line break - are refused by the handler with
      // the sentence COM's SetIniSetting uses, and a slash never reaches it.
      const AnsiString iniPath = "/api/v1/settings/ini";
      if (path == iniPath)
      {
         if (method == "GET")
            route.kind = RouteIniSettingList;
         return;
      }

      if (path.StartsWith(iniPath + "/"))
      {
         AnsiString name = path.Mid(iniPath.GetLength() + 1);
         if (!name.IsEmpty() && name.Find("/") < 0)
         {
            if (method == "GET")
               route.kind = RouteIniSettingGet;
            else if (method == "PUT")
               route.kind = RouteIniSettingPut;
            else if (method == "DELETE")
               route.kind = RouteIniSettingDelete;

            if (route.kind != RouteUnknown)
               route.identifier = name;
         }
         return;
      }

      if (method == "POST" && path == "/api/v1/settings/logon-failures/clear")
      {
         route.kind = RouteLogonFailuresClear;
         return;
      }

      if (path == "/api/v1/settings/scripting" && (method == "GET" || method == "PUT"))
      {
         route.kind = method == "GET" ? RouteSettingsScriptingGet : RouteSettingsScriptingPut;
         return;
      }

      if (method == "POST" && path == "/api/v1/settings/scripting/reload")
      {
         route.kind = RouteScriptingReload;
         return;
      }

      if (method == "POST" && path == "/api/v1/settings/scripting/check")
      {
         route.kind = RouteScriptingCheck;
         return;
      }

      if (method == "POST" && path == "/api/v1/settings/breach-check/test")
      {
         route.kind = RouteBreachCheckTest;
         return;
      }

      if (path == "/api/v1/settings/cache" && (method == "GET" || method == "PUT"))
      {
         route.kind = method == "GET" ? RouteSettingsCacheGet : RouteSettingsCachePut;
         return;
      }

      if (method == "POST" && path == "/api/v1/settings/cache/clear")
      {
         route.kind = RouteCacheClear;
         return;
      }

      if (path == "/api/v1/settings/indexing" && (method == "GET" || method == "PUT"))
      {
         route.kind = method == "GET" ? RouteSettingsIndexingGet : RouteSettingsIndexingPut;
         return;
      }

      if (method == "POST" && path == "/api/v1/settings/indexing/index")
      {
         route.kind = RouteIndexingIndex;
         return;
      }

      if (method == "POST" && path == "/api/v1/settings/indexing/clear")
      {
         route.kind = RouteIndexingClear;
         return;
      }

      if (path == "/api/v1/settings/backup" && (method == "GET" || method == "PUT"))
      {
         route.kind = method == "GET" ? RouteSettingsBackupGet : RouteSettingsBackupPut;
         return;
      }

      if (method == "GET" && path == "/api/v1/session-policy")
      {
         route.kind = RouteSessionPolicyGet;
         return;
      }

      if (path == "/api/v1/settings/bounces" && (method == "GET" || method == "PUT"))
      {
         route.kind = method == "GET" ? RouteSettingsBouncesGet : RouteSettingsBouncesPut;
         return;
      }

      if (path == "/api/v1/settings/openpgp" && (method == "GET" || method == "PUT"))
      {
         route.kind = method == "GET" ? RouteSettingsOpenPgpGet : RouteSettingsOpenPgpPut;
         return;
      }

      if (method == "POST" && path == "/api/v1/settings/push/vapid")
      {
         route.kind = RouteSettingsPushVapid;
         return;
      }

      if (path == "/api/v1/settings/push" && (method == "GET" || method == "PUT"))
      {
         route.kind = method == "GET" ? RouteSettingsPushGet : RouteSettingsPushPut;
         return;
      }

      if (method == "POST" && path == "/api/v1/bounces/classify")
      {
         route.kind = RouteBounceClassify;
         return;
      }

      if (method == "POST" && path == "/api/v1/bounces/expire")
      {
         route.kind = RouteBounceExpire;
         return;
      }

      if (path == "/api/v1/settings/complaints" && (method == "GET" || method == "PUT"))
      {
         route.kind = method == "GET" ? RouteSettingsComplaintsGet : RouteSettingsComplaintsPut;
         return;
      }

      if (method == "GET" && path == "/api/v1/complaints/rates")
      {
         route.kind = RouteComplaintRates;
         return;
      }

      if (method == "POST" && path == "/api/v1/complaints/expire")
      {
         route.kind = RouteComplaintExpire;
         return;
      }

      if (method == "POST" && path == "/api/v1/domain-reports/expire")
      {
         route.kind = RouteDomainReportsExpire;
         return;
      }

      // The server's message texts, named as the table names them.
      const AnsiString messagesPath = "/api/v1/settings/messages";
      if (path == messagesPath)
      {
         if (method == "GET")
            route.kind = RouteServerMessageList;
         return;
      }

      if (path.StartsWith(messagesPath + "/"))
      {
         AnsiString name = path.Mid(messagesPath.GetLength() + 1);
         if (method == "PUT" && !name.IsEmpty() && name.Find("/") < 0)
         {
            route.kind = RouteServerMessagePut;
            route.name = name;
         }
         return;
      }

      if (method == "POST" && path == "/api/v1/sieve/evaluate")
      {
         route.kind = RouteSieveEvaluate;
         return;
      }

      if (method == "POST" && path == "/api/v1/calendar/expand")
      {
         route.kind = RouteCalendarExpand;
         return;
      }

      if (method == "POST" && path == "/api/v1/server/reinitialize")
      {
         route.kind = RouteServerReinitialize;
         return;
      }
      const AnsiString archivePath = "/api/v1/archive";
      if (method == "GET" && path == archivePath)
      {
         route.kind = RouteArchiveSearch;
         return;
      }
      if (path.StartsWith(archivePath + "/"))
      {
         AnsiString remainder = path.Mid(archivePath.GetLength() + 1);
         if (remainder.EndsWith("/hold") && (method == "POST" || method == "DELETE"))
         {
            AnsiString idPart = remainder.Mid(0, remainder.GetLength() - AnsiString("/hold").GetLength());
            __int64 archiveId = 0;
            if (idPart.Find("/") < 0 && ParseQueueId(idPart, archiveId))
            {
               route.kind = method == "POST" ? RouteArchiveHold : RouteArchiveRelease;
               route.archive_id = archiveId;
               return;
            }
         }
         if (method == "GET" && remainder.Find("/") < 0)
         {
            __int64 archiveId = 0;
            if (ParseQueueId(remainder, archiveId))
            {
               route.kind = RouteArchiveGet;
               route.archive_id = archiveId;
               return;
            }
         }
      }
      if (method == "GET" && path == "/api/v1/audit")
      {
         route.kind = RouteAuditList;
         return;
      }

      if (method == "POST" && path.StartsWith("/api/v1/audit/") && path.EndsWith("/revert"))
      {
         const AnsiString prefix = "/api/v1/audit/";
         AnsiString idPart = path.Mid(prefix.GetLength(), path.GetLength() - prefix.GetLength() - AnsiString("/revert").GetLength());
         __int64 id = 0;
         if (!idPart.IsEmpty() && idPart.Find("/") < 0 && ParseQueueId(idPart, id))
         {
            route.kind = RouteAuditRevert;
            route.record_id = id;
            return;
         }
      }

      if (method == "GET" && path == "/api/v1/audit/verify")
      {
         route.kind = RouteAuditVerify;
         return;
      }

      if (method == "GET" && path == "/api/v1/alerts/events")
      {
         route.kind = RouteAlertEventList;
         return;
      }

      // "I have configured this - does it work?" A condition raised on demand
      // goes through exactly the path a real one does, so what arrives is what
      // would arrive; and a pass run on demand saves waiting a minute for the
      // scheduled one, which is the difference between verifying a webhook and
      // giving up on it.
      if (method == "POST" && path == "/api/v1/alerts/test")
      {
         route.kind = RouteAlertTest;
         return;
      }

      if (method == "POST" && path == "/api/v1/alerts/run")
      {
         route.kind = RouteAlertRun;
         return;
      }

      const AnsiString alertRulesPath = "/api/v1/alerts/rules";
      if (method == "GET" && path == alertRulesPath)
      {
         route.kind = RouteAlertRuleList;
         return;
      }
      if (method == "PUT" && path.StartsWith(alertRulesPath + "/"))
      {
         AnsiString condition = path.Mid(alertRulesPath.GetLength() + 1);

         if (!condition.IsEmpty() && condition.Find("/") < 0)
         {
            route.kind = RouteAlertRuleUpdate;
            route.identifier = condition;
         }

         return;
      }


      // Outbound webhooks. The name is the identifier, as the alert rules'
      // condition is, and the deliveries and the two verbs sit under the
      // collection rather than under a name so that no subscription can ever be
      // called "deliveries", "run" or "test".
      const AnsiString webhooksPath = "/api/v1/webhooks";

      if (method == "GET" && path == webhooksPath + "/deliveries")
      {
         route.kind = RouteWebhookDeliveryList;
         return;
      }

      if (method == "POST" && path == webhooksPath + "/run")
      {
         route.kind = RouteWebhookRun;
         return;
      }

      if (method == "POST" && path == webhooksPath + "/test")
      {
         route.kind = RouteWebhookTest;
         return;
      }

      if (path == webhooksPath)
      {
         if (method == "GET")
            route.kind = RouteWebhookList;
         else if (method == "POST")
            route.kind = RouteWebhookCreate;

         return;
      }

      if (path.StartsWith(webhooksPath + "/"))
      {
         AnsiString name = path.Mid(webhooksPath.GetLength() + 1);

         if (!name.IsEmpty() && name.Find("/") < 0)
         {
            if (method == "GET")
               route.kind = RouteWebhookGet;
            else if (method == "PUT")
               route.kind = RouteWebhookUpdate;
            else if (method == "DELETE")
               route.kind = RouteWebhookDelete;

            if (route.kind != RouteUnknown)
               route.identifier = name;
         }

         return;
      }

      if (method == "GET" && path == "/api/v1/openapi.json")
         route.kind = RouteOpenApi;
   }

   bool
   RestApiServer::IsApiKeyRoute_(RouteKind kind)
   {
      switch (kind)
      {
      case RouteApiKeyUnsupported:
      case RouteApiKeyList:
      case RouteApiKeyCreate:
      case RouteApiKeyRevoke:
      // The operator's tools (RestApiOperator.cpp): see PermissionFor_. Reading
      // whether a second factor is enrolled is not among them.
      case RouteAccountExport:
      case RouteAccountExportArchive:
      case RouteAddressErase:
      case RouteAdminSecondFactorEnrol:
      case RouteAdminSecondFactorDisable:
      // The [Settings] section of hMailServer.ini holds the server's secrets
      // (the OAuth2 HMAC secret, the password pepper, the service account's
      // password) and the commands it runs (AutoBanCommand, as the service):
      // the administrator password's, never a key's of any scope. Found by
      // the review of 14 September 2026.
      case RouteIniSettingList:
      case RouteIniSettingGet:
      case RouteIniSettingPut:
      case RouteIniSettingDelete:
      // The list of what has been changed from stock is those settings' values
      // seen from another angle, so it is the administrator password's for the
      // same reason, and an account holding a role is refused it as well - it is
      // NOT one of the seven below. A secret is masked in it, but AutoBanCommand
      // - the command the server runs, as the service - is not a secret by name
      // and is exactly the kind of setting somebody changes, so it is in this
      // list when it has been set. The Control Deck's page for it is left out
      // for every credential but the administrator password, by data-admin on
      // its button, so that a role is not offered a page it would be refused.
      case RouteSettingsChanged:
      // The audit trail names every administrator and every change they made,
      // and the alert rules hold the webhook secrets. A key that could read the
      // first could watch for its own footprints; a key that could write the
      // second could silence the alerting or point it at an endpoint of its own.
      // A key of any scope is answered 401 rather than 403 so that it learns
      // nothing from asking. These seven - and only these - are not the
      // administrator password's alone: an account that holds a role goes on
      // from Authorize_ to the permission decision for them (view_audit for the
      // trail, which is what the auditor role is for; view_configuration to
      // read the alert rules and events, manage_settings to change a rule or
      // raise a test, manage_server to run a pass), and to the server-wide
      // refusal when its role is confined to named domains. No key reaches
      // them, whatever its scope or role.
      case RouteAuditList:
      case RouteAuditVerify:
      case RouteAuditRevert:
      case RouteAlertRuleList:
      case RouteAlertRuleUpdate:
      case RouteAlertEventList:
      case RouteAlertTest:
      case RouteAlertRun:
      // A webhook subscription holds a signing secret and the address this
      // server will POST to. A key that could read one could forge bodies a
      // receiver would accept; a key that could write one could point the
      // server's whole mail stream at an endpoint of its own. The same
      // refusal, for the same reason, and with the same 401.
      case RouteWebhookList:
      case RouteWebhookGet:
      case RouteWebhookCreate:
      case RouteWebhookUpdate:
      case RouteWebhookDelete:
      case RouteWebhookDeliveryList:
      case RouteWebhookRun:
      case RouteWebhookTest:
      // What the compromised-account detection held, and an account's review
      // state. A key that could release held mail could send what a takeover
      // of its own account got held, and one that could mark an account exempt
      // could switch the detection off for the account it stole; the same
      // refusal as the alerts, with the same 401.
      case RouteReviewList:
      case RouteReviewRelease:
      case RouteReviewDiscard:
      case RouteAccountReviewGet:
      case RouteAccountReviewPut:
      case RouteAccountReviewUnlock:
         return true;

      default:
         break;
      }

      return false;
   }

   bool
   RestApiServer::IsMutatingRoute_(RouteKind kind)
   {
      // By kind and not by HTTP method, deliberately. The method is what a
      // client asserts; the kind is what this server decided the request
      // actually does, so a route that changed something under a GET could not
      // slip past a read-only key by being spelled harmlessly.
      switch (kind)
      {
      // Answered here as well as in IsApiKeyRoute_: a kind that writes must say
      // so whether or not a key can reach it at all, so that the answer does not
      // depend on the order two guards happen to be asked in.
      case RouteAlertRuleUpdate:
      case RouteAlertTest:
      case RouteAlertRun:
      case RouteWebhookCreate:
      case RouteWebhookUpdate:
      case RouteWebhookDelete:
      case RouteWebhookRun:
      case RouteWebhookTest:
      case RouteMeFetchAccountCreate:
      case RouteMeFetchAccountUpdate:
      case RouteMeFetchAccountDelete:
      case RouteMeFetchAccountDownload:
      case RouteMeFetchImportStart:
      case RouteMeSenderAddressCreate:
      case RouteMeSenderAddressVerify:
      case RouteMeSenderAddressResend:
      case RouteMeSenderAddressDelete:
      case RouteMeMaskedAddressCreate:
      case RouteMeMaskedAddressUpdate:
      case RouteMeMaskedAddressDelete:
      case RouteMeRecoverableRestore:
      case RouteMeRecoverableDelete:
      // Setting the address a forgotten password can be reset from, removing
      // it, sending its confirmation again, and replacing the recovery codes.
      case RouteMeRecoveryPut:
      case RouteMeRecoveryDelete:
      case RouteMeRecoveryResend:
      case RouteMeRecoveryCodes:
      case RoutePasskeyOptions:
      case RoutePasskeyCreate:
      case RoutePasskeyDelete:
      case RouteMeConfidentialRevoke:
      case RouteMeOfflinePut:
      case RouteMeFileCreate:
      case RouteMeFileContent:
      case RouteMeFileUpdate:
      case RouteMeFileDelete:
      case RouteMeSmimeOwnPut:
      case RouteMeSmimeOwnDelete:
      case RouteMeSmimeRecipientPut:
      case RouteMeSmimeRecipientDelete:
      case RoutePortalFilesPolicyPut:
      case RoutePortalBrandingPut:
      case RouteAccountSupportSession:
      case RouteMeDraftSchedule:
      case RouteMeDraftUnschedule:
      case RouteMeMessageSnooze:
      case RouteMeScheduledCancel:
      case RouteScheduledRun:
      case RouteMeFolderImport:
      case RouteMeMessageReceipt:
      case RouteMeMessageUnsubscribe:
      case RouteMeAppPasswordCreate:
      case RouteMeAppPasswordDelete:
      case RouteMeSessionsEnd:
      // A mailbox moved: rows in seven tables and a directory tree on disk. The
      // GET beside it only reports, so it is not here.
      case RouteMailboxMove:
      case RouteMeSessionDelete:
      case RouteMeDeviceDelete:
      case RouteMeFolderEmpty:
      case RouteMePreferencesPut:
      case RouteMePushSubscribe:
      case RouteMePushUpdate:
      case RouteMePushDelete:
      case RouteMePushTest:
      case RouteSettingsPushPut:
      case RouteSettingsPushVapid:
      case RouteMeCalendarCreate:
      case RouteMeCalendarUpdate:
      case RouteMeCalendarDelete:
      case RouteMeCalendarSharePut:
      case RouteMeCalendarShareDelete:
      case RouteMeEventCreate:
      case RouteMeEventUpdate:
      case RouteMeEventDelete:
      case RouteMeTaskCreate:
      case RouteMeTaskUpdate:
      case RouteMeTaskDelete:
      case RouteMeMessageInvitationReply:
      case RouteMeContactCreate:
      case RouteMeContactUpdate:
      case RouteMeContactDelete:
      case RouteMeContactGroupCreate:
      case RouteMeContactGroupUpdate:
      case RouteMeContactGroupDelete:
      case RouteMeContactGroupMemberAdd:
      case RouteMeContactGroupMemberRemove:
      case RouteApiKeyCreate:
      case RouteApiKeyRevoke:
      case RouteAccountCreate:
      case RouteAccountDelete:
      case RouteDomainCreate:
      case RouteDomainUpdate:
      case RouteDomainDelete:
      case RouteQueueRetry:
      case RouteQueueDelete:
      case RouteQuarantineRelease:
      case RouteQuarantineDelete:
      case RouteIpRangeCreate:
      case RouteIpRangeDelete:
      case RouteListCreate:
      case RouteListDelete:
      case RouteListUpdate:
      case RouteDnsBlackListCreate:
      case RouteDnsBlackListUpdate:
      case RouteDnsBlackListDelete:
      case RouteSurblServerCreate:
      case RouteSurblServerUpdate:
      case RouteSurblServerDelete:
      case RouteWhiteListAddressCreate:
      case RouteWhiteListAddressUpdate:
      case RouteWhiteListAddressDelete:
      case RouteBlockedSenderCreate:
      case RouteBlockedSenderUpdate:
      case RouteComplaintExpire:
      case RouteSettingsComplaintsPut:
      case RouteDomainReportsExpire:
      case RouteBlockedSenderDelete:
      case RouteIncomingRelayCreate:
      case RouteIncomingRelayUpdate:
      case RouteIncomingRelayDelete:
      case RouteBlockedAttachmentCreate:
      case RouteBlockedAttachmentUpdate:
      case RouteBlockedAttachmentDelete:
      // A journaling rule written, changed or taken away decides whether the
      // next accepted message is copied elsewhere. All three are writes.
      case RouteJournalRuleCreate:
      case RouteJournalRuleUpdate:
      case RouteJournalRuleDelete:
      case RouteGreyListingWhiteAddressCreate:
      case RouteGreyListingWhiteAddressUpdate:
      case RouteGreyListingWhiteAddressDelete:
      case RouteGreylistBypassCreate:
      case RouteGreylistBypassUpdate:
      case RouteGreylistBypassDelete:
      case RouteSenderListCreate:
      case RouteSenderListUpdate:
      case RouteSenderListDelete:
      case RouteSenderListDomainCreate:
      case RouteSenderListDomainUpdate:
      case RouteSenderListDomainDelete:
      case RouteSenderListMeCreate:
      case RouteSenderListMeUpdate:
      case RouteSenderListMeDelete:
      // A trap declared or taken back changes what the next RCPT TO does with
      // an address; clearing the record removes rows. All three are writes.
      case RouteSpamTrapDomainCreate:
      case RouteSpamTrapDelete:
      case RouteSpamTrapHitsClear:
      // A row of the domain policy list changes what the next MAIL FROM from
      // that domain is answered.
      case RouteDomainPolicyCreate:
      case RouteDomainPolicyUpdate:
      case RouteDomainPolicyDelete:
      case RouteMeQuarantineDigestPut:
      case RouteGroupCreate:
      case RouteGroupUpdate:
      case RouteGroupDelete:
      case RouteGroupMemberCreate:
      case RouteGroupMemberDelete:
      case RouteAccountMessageCopy:
      case RouteBackupStart:
      // The most destructive thing this API can be asked to do: it deletes every
      // domain, account and public folder and puts the archive's back.
      case RouteBackupRestore:
      case RouteAuditRevert:
      case RouteSettingsPut:
      case RouteSettingsAntiSpamPut:
      case RouteSettingsAntiVirusPut:
      case RouteSettingsLoggingPut:
      case RouteIniSettingPut:
      case RouteIniSettingDelete:
      case RouteLogonFailuresClear:
      case RouteSettingsScriptingPut:
      case RouteScriptingReload:
      // Makes the server send a request to a URL the caller chose.
      case RouteBreachCheckTest:
      case RouteSettingsCachePut:
      case RouteCacheClear:
      case RouteSettingsIndexingPut:
      case RouteIndexingIndex:
      case RouteIndexingClear:
      case RouteSettingsBackupPut:
      case RouteServerMessagePut:
      case RouteIpRangeUpdate:
      case RouteDomainAliasCreate:
      case RouteDomainAliasDelete:
      case RouteRuleCreate:
      case RouteRuleUpdate:
      case RouteRuleDelete:
      case RouteAccountRuleCreate:
      case RouteAccountRuleUpdate:
      case RouteAccountRuleDelete:
      case RouteDomainRuleCreate:
      case RouteDomainRuleUpdate:
      case RouteDomainRuleDelete:
      case RouteAccountExport:
      case RouteAddressErase:
      case RouteAdminSecondFactorEnrol:
      case RouteAdminSecondFactorDisable:
      case RouteFetchAccountCreate:
      case RouteFetchAccountUpdate:
      case RouteFetchAccountDelete:
      case RouteFetchAccountDownload:
      case RouteCertificateCreate:
      case RouteCertificateDelete:
      case RoutePortCreate:
      case RoutePortUpdate:
      case RoutePortDelete:
      case RouteRouteCreate:
      case RouteRouteUpdate:
      case RouteRouteDelete:
      case RouteRemoteDomainCreate:
      case RouteRemoteDomainUpdate:
      case RouteRemoteDomainDelete:
      // Clearing the verification cache changes what the next RCPT TO decides
      // and makes this server call out again, neither of which a read-only
      // credential should be able to cause.
      case RouteRemoteDomainClearCache:
      case RouteAliasCreate:
      case RouteAliasDelete:
      case RouteAccountUpdate:
      case RouteServerReinitialize:
      case RouteArchiveHold:
      case RouteArchiveRelease:
      case RouteSuppressionCreate:
      case RouteSuppressionDelete:
      case RouteBounceExpire:
      case RouteSettingsBouncesPut:
      // A key uploaded or removed changes what the whole internet is told
      // this server's users' keys are, and the switches decide whether it is
      // told anything at all.
      case RouteMeOpenPgpPut:
      case RouteMeOpenPgpDelete:
      case RouteMeOpenPgpPublish:
      case RouteSettingsOpenPgpPut:
      case RouteAccountOpenPgpPut:
      case RouteAccountOpenPgpDelete:
      // An update check changes the recorded verdict and makes the server call
      // out, neither of which a read-only credential should be able to cause;
      // a download writes a file the next part will run; an install runs it.
      case RouteUpdateCheck:
      case RouteUpdateDownload:
      case RouteUpdateInstall:
      case RouteDiagnosticsRun:
      case RouteMePassword:
      case RouteMeVacation:
      case RouteMeQuarantineRelease:
      case RouteMeQuarantineDelete:
      case RouteMeMessageFlags:
      case RouteMeMessageMove:
      case RouteMeMessageDelete:
      case RouteMeMessageSend:
      case RouteMeSettingsPut:
      case RouteMeFiltersPut:
      case RouteMeDraftSave:
      case RouteMeFolderCreate:
      case RouteMeFolderRename:
      case RouteMeFolderDelete:
      case RouteSessionCreate:
      case RouteSessionDelete:
      case RouteAccountAppPasswordCreate:
      case RouteAccountAppPasswordDelete:
      case RouteAccountFolderPermissionCreate:
      case RouteAccountFolderPermissionUpdate:
      case RouteAccountFolderPermissionDelete:
      case RouteAccountMessagesDelete:
      case RouteAccountFolderMessageCreate:
      case RouteAccountMessageFlags:
      case RouteAccountMessageDelete:
      case RouteAccountAppPasswordUpdate:
      case RouteAccountDeviceDelete:
      // The roles themselves, and the two the help-desk role exists for. Each
      // changes something, which is what keeps them out of an auditor's reach
      // and out of a read-only key's - see build/check-role-choke-point.py,
      // which proves that this list and PermissionFor_ agree.
      case RouteRoleCreate:
      case RouteRoleUpdate:
      case RouteRoleDelete:
      case RouteRoleAssign:
      case RouteRoleUnassign:
      case RouteAccountPasswordReset:
      case RouteAccountUnlock:
      case RouteAccountPushRemove:
      // Clearing an account's recovery address is changing that account, as
      // stopping its Web Push devices is, and ending its recovery codes is
      // ending a credential.
      case RouteAccountRecoveryClear:
      case RouteAccountRecoveryCodesClear:
      // Releasing held mail sends it; discarding it deletes it; the account's
      // review choices and the compromise lock are its state.
      case RouteReviewRelease:
      case RouteReviewDiscard:
      case RouteAccountReviewPut:
      case RouteAccountReviewUnlock:
      // A check writes what it found and starts a propagation clock; the rest move
      // keys, and the run moves every domain's rotation.
      case RouteDkimRotationStart:
      case RouteDkimRotationCheck:
      case RouteDkimRotationSwitch:
      case RouteDkimRotationRetire:
      case RouteDkimRotationCancel:
      case RouteDkimRotationRun:
      // A check makes this server send queries to third parties and rewrites what
      // the report says, neither of which a read-only credential should cause.
      case RouteReputationCheck:
      // The sample domain makes a domain, three mailboxes with a documented
      // password, a list, a rule and eight messages - or takes all of it away
      // again. Nothing this API does changes more in one call.
      case RouteSampleDomainCreate:
      case RouteSampleDomainRemove:
         return true;

      default:
         break;
      }

      return false;
   }

   bool
   RestApiServer::IsDomainAllowed_(const std::vector<String> &domains, const String &domainName)
   {
      // No list means every domain, which is what an unrestricted key and the
      // administrator password both have.
      if (domains.empty())
         return true;

      if (domainName.IsEmpty())
         return false;

      for (const String &allowed : domains)
      {
         if (allowed.CompareNoCase(domainName.c_str()) == 0)
            return true;
      }

      return false;
   }

   bool
   RestApiServer::HoldsServerAuthority_(const Caller &caller)
   {
      return caller.domains.empty() &&
             RoleManager::Allows(caller.grant, RolePermission::AdministerServer);
   }

   void
   RestApiServer::ApplyAccountGrant_(Caller &caller)
   {
      if (!caller.account)
         return;

      caller.grant = RoleManager::GrantForAccount(caller.account);

      // The two older spellings of the same authority, so that the handlers
      // that already narrow their output to a list of domains - the domain
      // listing, the SRV records, the archive - narrow it for a role exactly
      // as they do for a key, with no second implementation.
      caller.domains = caller.grant.domains;

      // An app password is a credential for a program that reads and sends
      // mail. It has never administered anything and it does not start now:
      // whatever role the account holds, a request that came in on one is an
      // ordinary account's request.
      if (caller.via_app_password)
      {
         caller.grant = RoleGrant();
         caller.domains.clear();
      }
   }

   bool
   RestApiServer::IsSessionRoute_(RouteKind kind)
   {
      // The three routes that are about the credential and not about the
      // server. Starting a browser session and ending it have always been
      // decided before anything else in Authorize_; asking what the credential
      // may do joins them, because a caller has to be able to learn what it is
      // allowed to do without already being allowed to do it - which is what
      // lets the Control Deck leave a view out rather than show one whose
      // every button answers 403.
      switch (kind)
      {
      case RouteSessionCreate:
      case RouteSessionDelete:
      case RouteSessionGet:
      // A passkey is a credential, not a resource: the four routes answer the
      // account that signed in with that account's passkeys and the
      // administrator with the administrator's, so there is no permission to
      // ask for and nobody else's to reach. An API key is refused them with
      // the sessions, and for the same reason - a key is already the
      // credential a script holds, and minting a second way in from one would
      // be a credential with none of the key's restrictions written on it.
      case RoutePasskeyList:
      case RoutePasskeyOptions:
      case RoutePasskeyCreate:
      case RoutePasskeyDelete:
         return true;

      default:
         break;
      }

      return false;
   }

   RolePermission
   RestApiServer::PermissionFor_(RouteKind kind)
   {
      // Every route this server is administered with, and the permission it
      // needs - the first of two, for the few routes SecondPermissionFor_
      // names. Written out rather than derived from the HTTP method, for
      // the reason IsMutatingRoute_ gives: the method is what a client
      // asserts, and the kind is what this server decided the request does.
      //
      // build/check-role-choke-point.py holds three things true of this
      // function: every RouteKind appears here or in IsSelfServiceRoute_ or in
      // IsSessionRoute_; every route IsMutatingRoute_ calls mutating needs a
      // permission the auditor does not hold; and every other route needs one
      // it does. The last two are what make "an auditor reads everything" and
      // "an auditor changes nothing" properties of the code rather than
      // claims about it.
      switch (kind)
      {
      // The API keys and the roles themselves. Both are the credential
      // store: a role that could edit either could give itself every other
      // permission, so this one is serveradmin's alone and no custom role may
      // name it. The key routes are refused to everything but the
      // administrator password before this is consulted at all.
      case RouteApiKeyUnsupported:
      case RouteApiKeyList:
      case RouteApiKeyCreate:
      case RouteApiKeyRevoke:
      // The operator's tools are the administrator password's alone too
      // (IsApiKeyRoute_): an export writes wherever on the server's disk the
      // caller names, an erasure reaches every domain's tables, and the second
      // factor is the credential store itself. Classified here for the same
      // reason the key routes are - no custom role may name them. The archive
      // download is a read and is classified with the mailbox reads below; it
      // stays the administrator password's through IsApiKeyRoute_.
      case RouteAccountExport:
      case RouteAddressErase:
      case RouteAdminSecondFactorEnrol:
      case RouteAdminSecondFactorDisable:
      case RouteRoleCreate:
      case RouteRoleUpdate:
      case RouteRoleDelete:
      case RouteRoleAssign:
      case RouteRoleUnassign:
         return RolePermission::ManageRoles;

      // Reading the server's shape: its state, its settings, its listeners,
      // its certificates and its rules, the roles that exist and who holds
      // them - and not one secret among them. RouteUnknown is here so that a
      // path this server does not have answers 404 as it always has, rather
      // than telling an unauthorised caller which paths exist.
      //
      // Three of them never reach here: the [Settings] list, one of its keys,
      // and the list of what this server has changed from stock are refused to
      // everything but the administrator password in Authorize_, before any
      // permission is consulted, as the key routes above are. Every route must
      // be classified all the same - build/check-role-choke-point.py holds that,
      // because a route in no classification is a route nobody decided about -
      // and for these the classification says what kind of route it is, not what
      case RouteReputationGet:
      case RouteSetupChecklist:
      // Whether the sample domain is there, and what its addresses and its one
      // password are, is a fact of the server's shape like the checklist above;
      // making it and removing it are the server administrator's, below.
      case RouteSampleDomainGet:
      // Whether the administrator password has a second factor is one fact of
      // the server's shape - the checklist above says the same thing - so a
      // reader may read it; enrolling and disabling are the password's alone.
      case RouteAdminSecondFactorGet:
      // a role may read.
      case RouteUnknown:
      case RouteStatus:
      case RouteTlsa:
      case RouteSrv:
      case RouteMetricsHistory:
      case RouteUpdateGet:
      case RouteDkimGet:
      case RouteDkimRotationGet:
      case RouteDomainSessionPolicyGet:
      case RouteSessionPolicyGet:
      case RouteCertificateList:
      case RouteRuleList:
      case RouteSettingsGet:
      case RouteSettingsAntiSpamGet:
      case RouteSettingsAntiVirusGet:
      case RouteSettingsLoggingGet:
      case RouteSettingsDirectoriesGet:
      case RouteIniSettingList:
      case RouteIniSettingGet:
      case RouteSettingsChanged:
      case RouteSettingsScriptingGet:
      case RouteScriptingCheck:
      case RouteSettingsPushGet:
      case RouteSettingsBackupGet:
      // A domain's spam traps and what has addressed them: configuration of
      // the domain, as its allow and block list is. A hit names an envelope
      // sender and a connecting address, which is what a refused RCPT TO
      // already writes into this server's own log, and never a secret. The
      // server-wide record is refused to a confined credential below.
      case RouteSpamTrapDomainList:
      case RouteSpamTrapDomainHits:
      case RouteSpamTrapHits:
      case RouteSettingsBouncesGet:
      case RouteSettingsComplaintsGet:
      // The Web Key Directory's settings, one account's keys and one
      // domain's. A key here is a PUBLIC key and never a secret, so reading
      // one is reading configuration; the domain and the account routes are
      // scoped to their domain below.
      case RouteSettingsOpenPgpGet:
      case RouteAccountOpenPgpList:
      case RouteDomainOpenPgpList:
      case RouteBackupStatus:
      case RouteSettingsCacheGet:
      case RouteSettingsIndexingGet:
      case RouteServerMessageList:
      case RouteSieveEvaluate:
      case RouteCalendarExpand:
      case RoutePortList:
      case RouteRouteList:
      case RouteIpRangeList:
      case RouteDnsBlackListList:
      case RouteSurblServerList:
      case RouteWhiteListAddressList:
      case RouteBlockedSenderList:
      case RouteIncomingRelayList:
      case RouteBlockedAttachmentList:
      // The journaling rules: who is copied, to where. Server-wide
      // configuration, and refused to a confined credential below.
      case RouteJournalRuleList:
      case RouteGreyListingWhiteAddressList:
      case RouteGreylistBypassList:
      // The domain white and black list: anti-spam configuration like the
      // lists above, server-wide, and refused to a confined credential below.
      case RouteDomainPolicyList:
      case RoutePortalFilesPolicy:
      case RouteDomainAliasList:
      case RouteRuleMatch:
      case RouteRoleList:
      case RouteRoleGet:
      case RouteRoleAssignmentList:
      // The remote domain policies: what this server demands of somebody
      // else's domain. Server-wide configuration, and refused to a confined
      // credential below.
      case RouteRemoteDomainList:
      case RouteRemoteDomainEffective:
      // The allow and block lists: the server's, refused to a confined
      // credential below, and one domain's, scoped to that domain. Anti-spam
      // configuration like the lists above, and never a secret.
      case RouteSenderListList:
      case RouteSenderListDomainList:
      // A domain's suppression list and the bounce counts it is fed from,
      // scoped to that domain. Every row is an address outside the server that
      // the domain sent to and what became of the mail, so SecondPermissionFor_
      // asks for view_message_trace as well, as it does for the reports.
      case RouteSuppressionList:
      case RouteBounceCountList:
      // The abuse reports about one domain's mail, scoped to that domain, and
      // every domain's complaint rate, which is refused to a confined credential
      // below. A complaint names a reader outside this server who was mailed
      // from it and did not want it, so SecondPermissionFor_ asks for
      // view_message_trace as well, exactly as it does for the suppression list.
      // The rates carry no address at all - a domain name, two counts and a
      // ratio - so they take view_configuration alone.
      case RouteComplaintList:
      case RouteComplaintRates:
      // The DMARC aggregate and TLS-RPT reports read for a domain, scoped to
      // that domain. A row names an IP address that sent as the domain and what
      // a receiver made of it - configuration's evidence, never a person - so
      // view_configuration alone reads them.
      case RouteDomainReportsSummary:
      case RouteDomainReportsList:
      case RouteDomainReportGet:
      case RouteDomainTlsReportsList:
      // How a status and a diagnostic would be classified: the server's rule
      // applied to what the caller typed. It reads and writes nothing and names
      // no domain.
      case RouteBounceClassify:
      // The alert rules and what they have raised. A webhook secret is never
      // in either answer (webhook_secret_set says whether there is one), so
      // this is configuration like any other; a key of any scope is still
      // refused both, before this is consulted, by IsApiKeyRoute_.
      case RouteAlertRuleList:
      case RouteAlertEventList:
      // The webhook subscriptions and their delivery queue. A shared secret is
      // never in either answer (secret_set says whether there is one), so this
      // is configuration like any other; a key of any scope is refused before
      // this is consulted, by IsApiKeyRoute_.
      case RouteWebhookList:
      case RouteWebhookGet:
      case RouteWebhookDeliveryList:
      // The reports: per-domain counts built from the message trace, the
      // metric history and the mailbox sizes. The domain administrator's for
      // their own domains, the auditor's for all of them, and not the help
      // desk's - it reads one address's trace, not a domain's traffic. The
      // domain a report names is judged below, and its server-wide sections
      // are refused to a confined credential there. The senders and
      // recipients sections are who mails whom, which is the message trace's
      // and not the configuration's: SecondPermissionFor_ asks for
      // view_message_trace as well.
      case RouteReport:
         return RolePermission::ViewConfiguration;

      // The audit trail, and the walk that verifies its chain. The one
      // permission nothing but the auditor and the server administrator
      // holds. A key of any scope is refused both before this is consulted
      // (IsApiKeyRoute_): the record of who changed what is the thing a
      // stolen key must not be able to read. An account that holds the
      // auditor role is not a key, and reading it is the whole of that role.
      // How many devices an account is notified on, and which: the names and the
      // person's choices, never the endpoint or a key.
      case RouteAccountPushGet:
      case RouteAuditList:
      case RouteAuditVerify:
         return RolePermission::ViewAudit;

      // Putting a setting back is changing a setting, whoever the row names.
      case RouteAuditRevert:
         return RolePermission::ManageSettings;

      // The names: which domains there are, and which mailboxes, aliases,
      // lists, groups and fetch accounts are in them. A help desk holds this
      // and nothing else that reads, because it cannot answer the telephone
      // without being able to find the account it was rung up about.
      case RouteDomainList:
      // One domain read whole says no more than the listing's entry for it
      // says, and a credential confined to a list of domains is refused
      // another's by DomainNamedByRoute_ below - so reading one sits with the
      // listing rather than with the update that changes it.
      case RouteDomainGet:
      case RouteAccountList:
      case RouteAccountExportArchive:
      case RouteAccountGet:
      case RouteAccountRuleList:
      case RouteDomainRuleList:
      case RouteAliasList:
      case RouteListList:
      // Who a list delivers to now: the addresses of accounts, which is what
      // the listing of accounts already carries, read rather than changed.
      case RouteListMembers:
      case RouteFetchAccountList:
      case RouteFetchAccountGet:
      case RouteAccountAppPasswordList:
      // Which clients have signed in to an account. Not its mail and not its
      // credentials: protocol, address, user agent and the NAME of the app
      // password each used. A help desk answering "something is signing in as
      // me" needs exactly this, which is why it sits with the names rather
      // than with the mailbox.
      case RouteAccountDeviceList:
      case RouteGroupList:
      case RouteGroupGet:
      case RouteGroupMemberList:
      // An account's review state under the compromised-account detection:
      // whether it is exempt, its sensitivity, whether it is locked and the
      // last shape seen. A read of the account, with the account's other reads.
      case RouteAccountReviewGet:
      // Whether a mailbox could be moved right now, and what it has been
      // through. It reads the account and the move journal and changes nothing,
      // so it sits with the other reads of an account - which is also what
      // keeps an auditor able to see it while the move itself stays out of
      // reach.
      case RouteMailboxMoveGet:
      // The OpenAPI document is the map every Control Deck form is drawn from,
      // not the server's state, and it holds nothing a caller could not learn
      // by reading the source. It sits with the lowest read every built-in
      // administrative role holds, so that a help desk's account editor has
      // the document it is built from.
      case RouteOpenApi:
         return RolePermission::ViewAccounts;

      // Another person's folders and the messages in them. Deliberately not
      // a help desk's: a role that can open the mail of the account whose
      // password it is resetting has not been narrowed at all.
      case RouteAccountFolderList:
      case RouteAccountFolderPermissionList:
      case RouteAccountMessageList:
      case RouteAccountFolderMessageList:
      case RouteAccountMessageGet:
      case RouteAccountMessageSource:
         return RolePermission::ViewMailbox;

      // Writing in another person's mailbox, and the support session, which
      // opens the portal as them and is therefore the strongest of these.
      case RouteAccountFolderPermissionCreate:
      case RouteAccountFolderPermissionUpdate:
      case RouteAccountFolderPermissionDelete:
      case RouteAccountMessagesDelete:
      case RouteAccountFolderMessageCreate:
      case RouteAccountMessageFlags:
      case RouteAccountMessageDelete:
      case RouteAccountMessageCopy:
      case RouteAccountSupportSession:
         return RolePermission::ManageMailbox;

      // The delivery queue, which names senders and recipients in every domain.
      // What the compromised-account detection holds is the queue's held part,
      // with the same senders and recipients in it.
      case RouteQueueList:
      case RouteReviewList:
         return RolePermission::ViewQueue;

      // Retrying a queued message, or dropping it; releasing a held one back
      // into the queue, or discarding it.
      case RouteQueueRetry:
      case RouteQueueDelete:
      case RouteReviewRelease:
      case RouteReviewDiscard:
         return RolePermission::ManageQueue;

      // What was held back, and why.
      case RouteQuarantineList:
         return RolePermission::ViewQuarantine;

      // Releasing held mail, which delivers it, or throwing it away.
      case RouteQuarantineRelease:
      case RouteQuarantineDelete:
         return RolePermission::ManageQuarantine;

      // The archive index, and one archived message.
      case RouteArchiveSearch:
      case RouteArchiveGet:
         return RolePermission::ViewArchive;

      // Holding an archived message against deletion, and letting it go.
      case RouteArchiveHold:
      case RouteArchiveRelease:
         return RolePermission::ManageArchive;

      // The logs, which carry every domain's traffic and are therefore never
      // a domain-confined role's.
      case RouteLogList:
      case RouteLogTail:
      case RouteLogSessions:
      case RouteLogSessionFollow:
         return RolePermission::ViewLogs;

      // Where a message went. The recent events and one message's story are
      // server-wide and refused to a confined credential below; one address's
      // events are scoped to that address's domain.
      case RouteMessageTraceRecent:
      case RouteMessageTraceAddress:
      case RouteMessageTraceQueue:
         return RolePermission::ViewMessageTrace;

      // Which domains exist. Deliberately not ManageAccounts: a domain
      // administrator administers what is inside a domain and does not decide
      // which domains the server hosts, and deleting one takes every mailbox
      // in it.
      case RouteDomainCreate:
      case RouteDomainDelete:
         return RolePermission::ManageDomains;

      // The mailboxes, aliases, lists, groups, fetch accounts and domain
      // properties inside a domain. An app password is a mailbox credential
      // and is here rather than under ResetPassword, because minting one for
      // somebody else's account is quieter than resetting their password and
      // reaches further.
      case RouteDomainUpdate:
      // Stopping Web Push on every device of an account - a lost phone - is
      // changing that account, as revoking its app passwords is. Clearing its
      // recovery address is the same kind of act: it takes away the way back in
      // that the person set for themselves.
      case RouteAccountPushRemove:
      case RouteAccountRecoveryClear:
      case RouteAccountRecoveryCodesClear:
      case RouteAccountCreate:
      case RouteAccountDelete:
      case RouteAccountUpdate:
      case RouteAccountRuleCreate:
      case RouteAccountRuleUpdate:
      case RouteAccountRuleDelete:
      case RouteDomainRuleCreate:
      case RouteDomainRuleUpdate:
      case RouteDomainRuleDelete:
      case RouteAliasCreate:
      case RouteAliasDelete:
      case RouteListCreate:
      case RouteListUpdate:
      case RouteListDelete:
      case RouteDomainAliasCreate:
      case RouteDomainAliasDelete:
      case RouteFetchAccountCreate:
      case RouteFetchAccountUpdate:
      case RouteFetchAccountDelete:
      case RouteFetchAccountDownload:
      case RouteGroupCreate:
      case RouteGroupUpdate:
      case RouteGroupDelete:
      case RouteGroupMemberCreate:
      case RouteGroupMemberDelete:
      case RouteAccountAppPasswordCreate:
      case RouteAccountAppPasswordUpdate:
      case RouteAccountAppPasswordDelete:
      // Revoking a client ends its sessions and can deactivate the app
      // password behind it, which is a change to the account's credentials -
      // so it sits with issuing one rather than with reading the list.
      case RouteAccountDeviceDelete:
      // A domain's own allow and block list is a property of the domain, as
      // its limits and its aliases are, and COM's Domain.SenderList answers a
      // domain administrator; the server's list is ManageSettings's, below.
      case RouteSenderListDomainCreate:
      case RouteSenderListDomainUpdate:
      case RouteSenderListDomainDelete:
      // Declaring an address of a domain a spam trap, and taking one back: a
      // property of the domain as its allow and block list is, and COM's
      // Domain.AddSpamTrap answers a domain administrator.
      case RouteSpamTrapDomainCreate:
      case RouteSpamTrapDelete:
      // A domain's DKIM key rotation acted on now: a key generated and
      // staged, a check that starts the propagation clock, signing moved, the
      // old selector retired, the next key cancelled. The key and its selector
      // are the domain's, as its limits are, and COM's Domain.DKIMRotation*
      // members answer a domain administrator.
      case RouteDkimRotationStart:
      case RouteDkimRotationCheck:
      case RouteDkimRotationSwitch:
      case RouteDkimRotationRetire:
      case RouteDkimRotationCancel:
      // Renaming a mailbox or moving it to another domain is an account change,
      // and the largest one there is, so it takes the permission every other
      // account change takes - in both domains, which HandleMailboxMove_ asks
      // about the one in the body.
      case RouteMailboxMove:
      // A domain's suppression list changed by hand: an address put on it,
      // which stops the domain's mail to that address, or taken off, which lets
      // it through again. The list is the domain's, as its own allow and block
      // list is, and COM's Utilities.AddSuppression and RemoveSuppression answer
      // that domain's administrator.
      case RouteSuppressionCreate:
      case RouteSuppressionDelete:
      // A key stored for an account, or taken away from it. The address it is
      case RouteSettingsPushPut:
      // Making a new VAPID key pair is a change to the server's configuration and
      // nothing narrower: it invalidates every subscription on every account.
      case RouteSettingsComplaintsPut:
      case RouteSettingsPushVapid:
      // published under is the account's, so this is administering that account
      // in that domain, as COM's Utilities.ImportOpenPGPKey answers that domain's
      // administrator. Publishing a key under somebody's address is what every
      // correspondent of theirs would then encrypt to, so it is never a
      // help-desk's or an auditor's.
      case RouteAccountOpenPgpPut:
      case RouteAccountOpenPgpDelete:
      // Marking an account exempt from the compromised-account detection, or
      // setting its sensitivity, is administering that account in that domain.
      case RouteAccountReviewPut:
         return RolePermission::ManageAccounts;

      // Setting somebody's password, as a route of its own rather than as a
      // field of the account update: a help-desk role can only be this narrow
      // if the one thing it is allowed to do is a route of its own.
      case RouteAccountPasswordReset:
         return RolePermission::ResetPassword;

      // Letting somebody back in after the lockout, and clearing the
      // logon-failure count that feeds the auto-ban.
      case RouteAccountUnlock:
      case RouteLogonFailuresClear:
      // The compromise lock alone cleared: letting somebody back in, as above.
      case RouteAccountReviewUnlock:
      // A check of this server's own addresses against third parties' block
      // lists: it makes the server send the queries, and it rewrites what the
      // report says. The same shape as the alert pass above.
      case RouteReputationCheck:
         return RolePermission::UnlockAccount;

      // Every setting, listener, certificate, route, rule and anti-spam list.
      // One permission and not twenty: an administrator who may change the TLS
      // certificate may also change the port it is bound to, and pretending
      // otherwise would be a menu rather than a boundary.
      case RouteSettingsPut:
      case RouteSettingsAntiSpamPut:
      case RouteSettingsAntiVirusPut:
      case RouteSettingsLoggingPut:
      case RouteIniSettingPut:
      case RouteIniSettingDelete:
      case RouteSettingsScriptingPut:
      case RouteSettingsBackupPut:
      case RouteSettingsBouncesPut:
      // The Web Key Directory's own switch and its limits: server-wide, like
      // the backup and the bounce settings beside it.
      case RouteSettingsOpenPgpPut:
      case RouteSettingsCachePut:
      case RouteSettingsIndexingPut:
      case RouteServerMessagePut:
      case RoutePortalBrandingPut:
      case RoutePortalFilesPolicyPut:
      case RouteIpRangeCreate:
      case RouteIpRangeUpdate:
      case RouteIpRangeDelete:
      case RouteRuleCreate:
      case RouteRuleUpdate:
      case RouteRuleDelete:
      case RouteCertificateCreate:
      case RouteCertificateDelete:
      case RoutePortCreate:
      case RoutePortUpdate:
      case RoutePortDelete:
      case RouteRouteCreate:
      case RouteRouteUpdate:
      case RouteRouteDelete:
      case RouteDnsBlackListCreate:
      case RouteDnsBlackListUpdate:
      case RouteDnsBlackListDelete:
      case RouteSurblServerCreate:
      case RouteSurblServerUpdate:
      case RouteSurblServerDelete:
      case RouteWhiteListAddressCreate:
      case RouteWhiteListAddressUpdate:
      case RouteWhiteListAddressDelete:
      case RouteBlockedSenderCreate:
      case RouteBlockedSenderUpdate:
      case RouteBlockedSenderDelete:
      case RouteIncomingRelayCreate:
      case RouteIncomingRelayUpdate:
      case RouteIncomingRelayDelete:
      case RouteBlockedAttachmentCreate:
      case RouteBlockedAttachmentUpdate:
      case RouteBlockedAttachmentDelete:
      // A journaling rule: a standing instruction that somebody's mail is
      // copied to an address they cannot see, which is manage_server's alone.
      case RouteJournalRuleCreate:
      case RouteJournalRuleUpdate:
      case RouteJournalRuleDelete:
      case RouteGreyListingWhiteAddressCreate:
      case RouteGreyListingWhiteAddressUpdate:
      case RouteGreyListingWhiteAddressDelete:
      case RouteGreylistBypassCreate:
      case RouteGreylistBypassUpdate:
      case RouteGreylistBypassDelete:
      // The domain white and black list: a row refuses a domain's mail at MAIL
      // FROM or lets it past greylisting and the score, for every recipient.
      case RouteDomainPolicyCreate:
      case RouteDomainPolicyUpdate:
      case RouteDomainPolicyDelete:
      // A remote domain policy, and the verification cache whose clearing
      // changes what the next RCPT TO decides.
      case RouteRemoteDomainCreate:
      case RouteRemoteDomainUpdate:
      case RouteRemoteDomainDelete:
      case RouteRemoteDomainClearCache:
      // An alert rule - where the alerting goes, and whether it goes at all -
      // and the condition raised on demand to prove a rule works.
      case RouteAlertRuleUpdate:
      case RouteAlertTest:
      // Removing a webhook subscription: configuration, and the one webhook
      // change that cannot make the server reach anything. Creating or changing
      // one, and raising a test event, are manage_server's below.
      case RouteWebhookDelete:
      // The server's allow and block list, which applies to every recipient
      // of every domain.
      case RouteSenderListCreate:
      case RouteSenderListUpdate:
      case RouteSenderListDelete:
      // Clearing the server-wide record of spam trap hits: server-wide, and a
      // record the auditor reads and does not remove.
      case RouteSpamTrapHitsClear:
      // The breach check's endpoint test, which makes this server send a
      // request to a URL the caller chooses: acting as the server, not
      // reading it, and so never an auditor's.
      case RouteBreachCheckTest:
         return RolePermission::ManageSettings;

      // Restarting, backing up, reloading the scripts, clearing the caches and
      // the index, running the scheduled sends, and installing an update: the
      // things that act on the whole process.
      case RouteServerReinitialize:
      case RouteBackupStart:
      // Restoring is the backup's other half and acts on the whole server, so it
      // is the same permission - the one an auditor does not hold.
      case RouteBackupRestore:
      case RouteScriptingReload:
      case RouteCacheClear:
      case RouteIndexingIndex:
      case RouteIndexingClear:
      case RouteUpdateCheck:
      case RouteUpdateDownload:
      case RouteUpdateInstall:
      case RouteDiagnosticsRun:
      case RouteScheduledRun:
      // The alert pass run now rather than in a minute, which is the scheduled
      // sends' shape exactly.
      case RouteAlertRun:
      // One webhook delivery pass now rather than in a minute: the same shape,
      // and it attempts every subscription's queue, not one domain's.
      case RouteWebhookRun:
      // A webhook subscription - where this server's mail events go, signed
      // with what, and how much of it - and the test event raised to prove one
      // works. manage_server and not manage_settings, deliberately: an
      // endpoint is reached FROM this server, so it may be an address only this
      // server can reach, and with /test, /run and the queue's last error the
      // holder has an answer-or-refused-or-timed-out oracle over the inside of
      // the network, one host and port per subscription. That is acting as the
      // server, which is what manage_server names; the policy is written down
      // in docs/Webhooks.md.
      case RouteWebhookCreate:
      case RouteWebhookUpdate:
      case RouteWebhookTest:
      // One pass of every domain's DKIM key rotation now, with a clock the
      // caller may move: the scheduled task's pass, and refused to a confined
      // credential below because it advances every domain's rotation.
      case RouteDkimRotationRun:
      // The suppression list's hourly sweep run now: expired entries, old counts
      // and old report keys removed from every domain's list, which is also why
      // it is refused to a confined credential below.
      case RouteBounceExpire:
      // The feedback loops' hourly sweep run now: old complaints and the tally
      // rows behind the rate removed for every domain, which is also why it is
      // refused to a confined credential below.
      case RouteComplaintExpire:
      // The domain reports' retention sweep run now: every domain's old DMARC
      // and TLS-RPT reports removed, which is also why it is refused to a
      // confined credential below.
      case RouteDomainReportsExpire:
      // Making and removing the sample domain. ManageServer rather than
      // ManageDomains, deliberately: the accounts it makes all carry one
      // password that is printed in the documentation, so whoever may ask for
      // them is agreeing that this server is not yet taking real mail - and the
      // removal deletes a whole domain with everything in it, including anything
      // a learner put there. That is acting as the server, which is what
      // ManageServer names.
      case RouteSampleDomainCreate:
      case RouteSampleDomainRemove:
         return RolePermission::ManageServer;

      default:
         break;
      }

      // Unreachable while the check above passes, and the safest answer when
      // it somehow does not: a route nobody classified needs the one
      // permission only the server administrator holds.
      return RolePermission::AdministerServer;
   }

   bool
   RestApiServer::SecondPermissionFor_(RouteKind kind, RolePermission &permission)
   {
      // The routes whose answer is two kinds of thing at once, and so needs the
      // permission for each. A second permission can only take a route away
      // from somebody: Authorize_ asks for both, in the one domain the route
      // names, and refuses when either is missing.
      //
      // build/check-role-choke-point.py reads this switch in the shape
      // PermissionFor_ has - case labels, then one "permission = " line - holds
      // every route here to being classified there too, and holds both
      // permissions of a route to the auditor rule: a read needs two the
      // auditor holds.
      switch (kind)
      {
      // The reports. Most of a report is configuration-shaped - counts per
      // domain and per day - but GET /api/v1/reports/senders and /recipients,
      // and the summary that carries both, list which addresses sent and
      // received the most: who mails whom, which is traffic metadata read out
      // of the message trace. A role that may read the configuration is not
      // thereby a role that may read that. The built-in outcomes do not move -
      // domainadmin and auditor hold both, the help desk holds only the trace
      // and was never let in - but a custom role holding view_configuration
      // alone no longer sees who corresponds with whom.
      case RouteReport:
         permission = RolePermission::ViewMessageTrace;
         return true;

      // A domain's suppression list and its bounce counts: configuration in
      // shape, but every row is an address outside the server that the domain
      // sent mail to, with the remote's answer and when it last bounced or was
      // delivered - who mails whom, as the reports' senders and recipients are.
      // domainadmin and the auditor hold both permissions; a custom role holding
      // view_configuration alone does not read them.
      case RouteSuppressionList:
      case RouteBounceCountList:
      // A domain's complaints: every row is a reader outside this server that
      // the domain mailed, with what they said about it and when. Who mails
      // whom, as the suppression list's rows are. The rates are not here: they
      // name no address.
      case RouteComplaintList:
         permission = RolePermission::ViewMessageTrace;
         return true;

      case RouteSpamTrapDomainList:
      case RouteSpamTrapDomainCreate:
      case RouteSpamTrapDomainHits:
      default:
         break;
      }

      return false;
   }

   String
   RestApiServer::DomainNamedByRoute_(const Route &route)
   {
      // The domain a route acts inside, or nothing when it acts on the server.
      // Split out of Authorize_ so that the two decisions made about a domain -
      // may this ROLE reach it, and is this KEY permitted it - are made about
      // the same name rather than about two readings of the path.
      switch (route.kind)
      {
      case RouteAccountList:
      case RouteAccountCreate:
      // Reading one domain names it, exactly as changing it does. Left out of
      // this list a confined credential could read any domain it could name,
      // which is every field of another tenant's configuration.
      case RouteDomainGet:
      case RouteDomainUpdate:
      case RouteAliasList:
      case RouteAliasCreate:
      case RouteListList:
      case RouteListCreate:
      case RouteDkimGet:
      // Left out of this list, a rotation route would name no domain, and a
      // credential confined to one domain could then switch another domain's key.
      case RouteAccountPushGet:
      case RouteAccountPushRemove:
      case RouteAccountRecoveryClear:
      case RouteAccountRecoveryCodesClear:
      case RouteDkimRotationGet:
      case RouteDkimRotationStart:
      case RouteDkimRotationCheck:
      case RouteDkimRotationSwitch:
      case RouteDkimRotationRetire:
      case RouteDkimRotationCancel:
      // The policy in force for one domain's sessions: left out, a confined
      // credential could read another domain's.
      case RouteDomainSessionPolicyGet:
      case RouteDomainAliasList:
      case RouteDomainAliasCreate:
      case RouteDomainAliasDelete:
      case RouteSenderListDomainList:
      case RouteSenderListDomainCreate:
      case RouteSenderListDomainUpdate:
      case RouteSenderListDomainDelete:
      // A domain's suppression list and the bounce counts behind it. Left out
      // of this list, one of these routes would name no domain, and a
      // A domain's complaints. Left out of this list, the route would name no
      // domain, and a credential confined to one domain could read another's.
      case RouteComplaintList:
      // A domain's DMARC and TLS-RPT reports, under the domain's name.
      case RouteDomainReportsSummary:
      case RouteDomainReportsList:
      case RouteDomainReportGet:
      case RouteDomainTlsReportsList:
      // credential confined to one domain could read or change another's list.
      case RouteSuppressionList:
      case RouteSuppressionCreate:
      case RouteSuppressionDelete:
      case RouteBounceCountList:
      // Every key held for a domain's accounts. Left out of this list it
      // would name no domain, and a credential confined to one domain could
      // read another domain's.
      case RouteDomainOpenPgpList:
      // A report names the domain it is about in ?domain=, which ParseRoute_
      // puts in identifier; a report that names none names nothing, and the
      // handler confines it to the credential's domains.
      case RouteReport:
      // A domain's rules are under the domain's name.
      case RouteDomainRuleList:
      case RouteDomainRuleCreate:
      case RouteDomainRuleUpdate:
      case RouteDomainRuleDelete:
         return String(route.identifier);

      case RouteListDelete:
      case RouteListUpdate:
      // The members route names the list at its own address, as the update
      // and the delete do, so the domain it is scoped to is the one in it.
      case RouteListMembers:
      case RouteAliasDelete:
      // A spam trap is removed at its own address, as an alias is, so the
      // domain it belongs to is the domain in that address. Without this a
      // credential issued for one domain could stop another domain trapping.
      case RouteSpamTrapDelete:
      case RouteAccountUpdate:
      case RouteAccountGet:
      case RouteAccountRuleList:
      case RouteAccountRuleCreate:
      case RouteAccountRuleUpdate:
      case RouteAccountRuleDelete:
      case RouteAccountExport:
      case RouteAccountExportArchive:
      case RouteFetchAccountList:
      case RouteFetchAccountCreate:
      case RouteFetchAccountGet:
      case RouteFetchAccountUpdate:
      case RouteFetchAccountDelete:
      case RouteFetchAccountDownload:
      case RouteAccountAppPasswordList:
      case RouteAccountAppPasswordCreate:
      case RouteAccountAppPasswordDelete:
      case RouteAccountFolderList:
      case RouteAccountFolderPermissionList:
      case RouteAccountFolderPermissionCreate:
      case RouteAccountFolderPermissionUpdate:
      case RouteAccountFolderPermissionDelete:
      case RouteAccountMessageList:
      case RouteAccountMessagesDelete:
      case RouteAccountFolderMessageList:
      case RouteAccountFolderMessageCreate:
      case RouteAccountMessageGet:
      case RouteAccountMessageFlags:
      case RouteAccountMessageDelete:
      case RouteAccountMessageSource:
      case RouteAccountAppPasswordUpdate:
      case RouteAccountMessageCopy:
      case RouteAccountDeviceList:
      case RouteAccountDeviceDelete:
      case RouteAccountSupportSession:
      // The account's keys are named by an address in the path and nothing
      // else. Without this, a credential issued for one domain could publish
      // a key under an address in another by editing one path segment - and
      // a key published under somebody's address is what every correspondent
      // of theirs would then encrypt to.
      case RouteAccountOpenPgpList:
      case RouteAccountOpenPgpPut:
      case RouteAccountOpenPgpDelete:
      // The reset and the unlock name an address in the path exactly as the
      // account routes do, and are the two a help-desk role reaches: without
      // this, a role issued for one domain could reset a password in another
      // by editing one path segment.
      case RouteAccountPasswordReset:
      case RouteAccountUnlock:
      // The account's review state and its compromise lock name the address
      // the same way, and are scoped the same way.
      case RouteAccountReviewGet:
      case RouteAccountReviewPut:
      case RouteAccountReviewUnlock:
      // The mailbox move names the address it is moving FROM in the path, and
      // the one it is moving TO in its body; this scopes the first, and
      // HandleMailboxMove_ judges the second, which nothing here can see.
      case RouteMailboxMoveGet:
      case RouteMailboxMove:
      // One address's trace, scoped to that address's domain.
      case RouteMessageTraceAddress:
      // An assignment names the account it is for, and giving somebody a role
      // in a domain is administering that domain.
      case RouteRoleAssign:
      case RouteRoleUnassign:
         return StringParser::ExtractDomain(String(route.identifier));

      case RouteAccountDelete:
         // The reason the whole mechanism exists. The account to delete is named
         // by an address in the path and nothing else, so without this a key
         // issued for one domain could delete a mailbox in another by editing
         // one path segment - the classic identifier-in-the-path authorisation
         // bypass, against a route that destroys mail.
         return StringParser::ExtractDomain(String(route.identifier));

      default:
         break;
      }

      return String();
   }

   RestApiServer::AuthorizationResult
   RestApiServer::Authorize_(const Caller &caller, const Route &route, AnsiString &refusalReason)
   {
      refusalReason = "";

      // A browser session is started by whoever holds a password - an
      // account's, for the self-service page, or the administrator's, for the
      // Control Deck - and ended by the session it came with; the passkeys
      // classified here are that credential's own. An API key is refused all of
      // them: a key is already the credential a script keeps, and a cookie or a
      // passkey minted from one would be a second credential with none of the
      // key's restrictions written on it.
      if (IsSessionRoute_(route.kind))
      {
         if (route.kind == RouteSessionGet)
         {
            // What am I, and what may I do. Answered to every credential that
            // authenticated, API keys included: a caller cannot be asked to
            // discover its own authority by trying things and reading the
            // 403s, and the answer holds nothing it did not already present.
            return AuthorizationAllowed;
         }

         if (caller.result == AuthenticatedAsAccount || caller.result == AuthenticatedAsAdministrator)
            return AuthorizationAllowed;

         refusalReason = "a browser session, and the passkeys the credential holding it has, answer to an account's password or to the administrator password, not to an api key";
         return AuthorizationForbidden;
      }

      // The account's own endpoints, and the account's own credentials: each
      // reaches the other and nothing else. The administrator password and an
      // API key are refused there because neither is an account - there is no
      // mailbox behind them whose quota or vacation message could be meant -
      // and an account is refused everywhere else because the rest of the
      // API administers the server.
      if (IsSelfServiceRoute_(route.kind))
      {
         if (caller.result == AuthenticatedAsAccount)
            return AuthorizationAllowed;

         refusalReason = "this endpoint answers to an account's own credentials, not to the administrator password or an api key";
         return AuthorizationForbidden;
      }

      if (caller.result == AuthenticatedAsAccount && !caller.grant.administrative)
      {
         // An ordinary account, exactly as before: its credentials reach its
         // own mailbox and nothing else. What is new is the account that holds
         // an administrative role - the domain administrator who signs in to
         // the Control Deck for their domain, the help desk, the auditor - and
         // it falls through to the permission check below rather than being
         // refused here.
         refusalReason = "an account's credentials reach only the account's own endpoints under /api/v1/me";
         return AuthorizationForbidden;
      }

      // The administrator password carries full authority and always has. This
      // is the one credential nothing below narrows.
      if (caller.result == AuthenticatedAsAdministrator)
         return AuthorizationAllowed;

      // Key management is administrator-password only. An API key that could
      // mint keys would be able to issue itself a replacement with no expiry,
      // no source restriction and full scope, which would give away the whole
      // point of having scoped keys; and one that could revoke keys could lock
      // the administrator out of their own management interface. Answering 401
      // rather than 403 keeps the refusal indistinguishable from any other
      // credential problem - including for a verb that does not exist, which is
      // why RouteApiKeyUnsupported is in this set.
      if (IsApiKeyRoute_(route.kind))
      {
         // An account that holds a role has already proved its credential and
         // is refused as a matter of permission, with the reason - a 401 would
         // tell the Control Deck its session had ended and sign it out. The
         // 401 stays for keys, where it keeps "valid but not permitted"
         // indistinguishable from "not a key at all".
         if (caller.result != AuthenticatedAsAccount)
            return AuthorizationUnauthenticated;

         // The audit trail and the alerts are in that set to keep them from a
         // KEY - a stolen key must not read the record of what it did, nor
         // silence the alerting. An account that holds a role is not a key:
         // the auditor role exists to read the audit trail, and the server
         // administrator's own mailbox is the server administrator. Those
         // routes go on to the permission decision below (ViewAudit for the
         // trail; ViewConfiguration, ManageSettings and ManageServer for the
         // alerts) and to the server-wide refusal for a confined role. The key
         // store and the [Settings] section stay the administrator password's
         // alone.
         switch (route.kind)
         {
         case RouteAuditList:
         case RouteAuditVerify:
         case RouteAuditRevert:
         case RouteAlertRuleList:
         case RouteAlertRuleUpdate:
         case RouteAlertEventList:
         case RouteAlertTest:
         case RouteAlertRun:
         case RouteWebhookList:
         case RouteWebhookGet:
         case RouteWebhookCreate:
         case RouteWebhookUpdate:
         case RouteWebhookDelete:
         case RouteWebhookDeliveryList:
         case RouteWebhookRun:
         case RouteWebhookTest:
         // Held mail and an account's review state: refused to a key above,
         // and decided by the permission below for an account with a role -
         // the queue's permissions for the held list, the account's for its
         // state, the unlock permission for the compromise lock.
         case RouteReviewList:
         case RouteReviewRelease:
         case RouteReviewDiscard:
         case RouteAccountReviewGet:
         case RouteAccountReviewPut:
         case RouteAccountReviewUnlock:
            break;

         default:
            refusalReason = "the api keys and the [Settings] section of hMailServer.ini, including the list of what this server has changed from stock, answer to the administrator password only";
            return AuthorizationForbidden;
         }
      }

      // THE DECISION, and the only one. Every route of this API is answered
      // by this one call: the permission the route needs (PermissionFor_, and
      // SecondPermissionFor_ for the few that need two), the
      // authority the credential carries (caller.grant, resolved once when it
      // was authenticated), and the domain the route names. A read-only key is
      // the auditor role and a full key is the server administrator, so the
      // scopes that existed before roles did are decided here too rather than
      // beside this - which is what stops the two from drifting apart.
      {
         String roleRefusal;
         const String routeDomain = DomainNamedByRoute_(route);

         // The few routes that need two permissions ask for the second in
         // the same domain, and only once the first has been granted, so that
         // the refusal names the first permission the credential lacks.
         RolePermission secondPermission = RolePermission::Count;
         const bool permitted =
            RoleManager::Allows(caller.grant, PermissionFor_(route.kind), routeDomain, roleRefusal) &&
            (!SecondPermissionFor_(route.kind, secondPermission) ||
             RoleManager::Allows(caller.grant, secondPermission, routeDomain, roleRefusal));

         if (!permitted)
         {
            // A read-only key hears the sentence it has heard since August
            // 2026, because that is the restriction it was issued with and the
            // one its holder will look for; every other refusal names the role
            // and the permission.
            refusalReason = (caller.result == AuthenticatedWithApiKey && caller.read_only &&
                             caller.grant.name == _T("auditor") && IsMutatingRoute_(route.kind))
               ? AnsiString("this api key is read-only")
               : AnsiString(roleRefusal);

            return AuthorizationForbidden;
         }
      }

      if (caller.domains.empty())
         return AuthorizationAllowed;

      // A key restricted to named domains. The delivery queue is server-wide -
      // one queued message carries recipients in any number of domains, and
      // GET /api/v1/queue lists the sender and every recipient of all of them -
      // so there is no honest way to narrow it to a domain. Refused outright
      // rather than narrowed wrongly.
      if (route.kind == RouteQueueList || route.kind == RouteQueueRetry || route.kind == RouteQueueDelete)
      {
         refusalReason = "this credential is restricted to named domains, and the delivery queue is server-wide";
         return AuthorizationForbidden;
      }

      // The quarantine has the queue's shape exactly: one entry names a sender
      // and recipients in any number of domains, and releasing one delivers
      // mail. The same reasoning gives the same answer.
      // An archive holds every domain on the server, so a credential confined to
      // named domains is no more able to restore one than to take one.
      if (route.kind == RouteQuarantineList || route.kind == RouteQuarantineRelease || route.kind == RouteQuarantineDelete)
      {
         refusalReason = "this credential is restricted to named domains, and the quarantine is server-wide";
         return AuthorizationForbidden;
      }

      // The reports. Two of the sections are counted from the server-wide
      // metric history - the spam and virus totals, the delivered, deferred
      // and bounced totals, and the size of the message store - and those
      // counters have no domain in them at all. Narrowing them would mean
      // inventing an attribution, so they are refused outright, as the queue
      // is, and for the same reason. The rest are counted from the message
      // trace and the mailbox sizes, both of which carry an address: a
      // request that names a domain is judged against the key's list below,
      // and one that names none is confined to the key's domains by the
      // handler, as the domain listing is.
      if (route.kind == RouteReport)
      {
         if (IsServerWideReportSection_(route.name))
         {
            refusalReason = "this credential is restricted to named domains, and that report section is counted from "
                            "server-wide metrics that carry no domain";
            return AuthorizationForbidden;
         }

         if (!route.identifier.IsEmpty() && !IsDomainAllowed_(caller.domains, String(route.identifier)))
         {
            refusalReason = "this credential is not permitted for that domain";
            return AuthorizationForbidden;
         }

         return AuthorizationAllowed;
      }
      switch (route.kind)
      {
      // The set of domains is the server's. A key issued for named domains
      // administers what is inside them; it does not decide which domains
      // exist, so creating one - or deleting one, with every mailbox in it -
      // needs a credential that was never narrowed to domains. Changing a
      // domain's active flag is scoped to that domain further down, as the
      // The Web Push settings and the key pair are the server's, and the key pair
      // is every account's: a credential narrowed to one domain must not be able
      // to replace it and silence every other domain's devices.
      case RouteSettingsPushGet:
      case RouteSettingsPushPut:
      case RouteSettingsPushVapid:
      // account routes are.
      case RouteDomainCreate:
      case RouteDomainDelete:
      case RouteIpRangeList:
      case RouteIpRangeCreate:
      case RouteIpRangeDelete:
      case RouteDnsBlackListList:
      case RouteDnsBlackListCreate:
      case RouteDnsBlackListUpdate:
      case RouteDnsBlackListDelete:
      case RouteSurblServerList:
      case RouteSurblServerCreate:
      case RouteSurblServerUpdate:
      case RouteSurblServerDelete:
      case RouteWhiteListAddressList:
      case RouteWhiteListAddressCreate:
      case RouteWhiteListAddressUpdate:
      case RouteWhiteListAddressDelete:
      case RouteBlockedSenderList:
      case RouteBlockedSenderCreate:
      case RouteBlockedSenderUpdate:
      case RouteBlockedSenderDelete:
      case RouteIncomingRelayList:
      case RouteIncomingRelayCreate:
      case RouteIncomingRelayUpdate:
      case RouteIncomingRelayDelete:
      case RouteBlockedAttachmentList:
      case RouteBlockedAttachmentCreate:
      case RouteBlockedAttachmentUpdate:
      case RouteBlockedAttachmentDelete:
      // The journaling rules. A rule names a domain, but reading the list would
      // show a confined credential every other domain's, and writing one would
      // let it point a copy of somebody else's mail wherever it liked.
      case RouteJournalRuleList:
      case RouteJournalRuleCreate:
      case RouteJournalRuleUpdate:
      case RouteJournalRuleDelete:
      case RouteGreyListingWhiteAddressList:
      case RouteGreyListingWhiteAddressCreate:
      case RouteGreyListingWhiteAddressUpdate:
      case RouteGreyListingWhiteAddressDelete:
      case RouteGreylistBypassList:
      case RouteGreylistBypassCreate:
      case RouteGreylistBypassUpdate:
      case RouteGreylistBypassDelete:
      case RouteSenderListList:
      case RouteSenderListCreate:
      case RouteSenderListUpdate:
      case RouteSenderListDelete:
      // The domain white and black list decides for the connection, before
      // there is a recipient, so no hosted domain can scope it.
      case RouteDomainPolicyList:
      case RouteDomainPolicyCreate:
      case RouteDomainPolicyUpdate:
      case RouteDomainPolicyDelete:
      // The record of every domain's spam trap hits, and its clearing: one
      // list across every domain, so a credential confined to some of them is
      // refused it and reads its own domains' hits instead.
      case RouteSpamTrapHits:
      case RouteSpamTrapHitsClear:
      case RouteCertificateList:
      case RouteRuleList:
      case RouteAdminSecondFactorGet:
      case RouteLogList:
      case RouteLogTail:
      case RouteLogSessions:
      case RouteLogSessionFollow:
      // A search that matches any address, and one queued message whose
      // recipients are in any number of domains, have no domain to confine to.
      case RouteMessageTraceRecent:
      case RouteMessageTraceQueue:
      case RouteBackupStart:
      case RouteBackupRestore:
      case RouteBackupStatus:
      // The complaint settings apply to every domain, the rates are every
      // domain's, and the sweep prunes every domain's rows.
      case RouteSettingsComplaintsGet:
      case RouteSettingsComplaintsPut:
      case RouteComplaintRates:
      case RouteComplaintExpire:
      // The domain reports' sweep prunes every domain's rows.
      case RouteDomainReportsExpire:
      case RouteSettingsGet:
      case RouteSettingsChanged:
      case RouteSettingsPut:
      case RouteSettingsAntiSpamGet:
      case RouteSettingsAntiSpamPut:
      case RouteSettingsAntiVirusGet:
      case RouteSettingsAntiVirusPut:
      case RouteSettingsLoggingGet:
      case RouteSettingsLoggingPut:
      case RouteSettingsDirectoriesGet:
      case RouteIniSettingList:
      case RouteIniSettingGet:
      case RouteIniSettingPut:
      case RouteIniSettingDelete:
      // Classified with the unlock, which a help desk holds, and here all the
      // same. hm_logon_failures records the ADDRESS a failure came from and its
      // time; it holds no account and no domain, so there is no per-domain form
      // of it to offer and clearing it resets the auto-ban counter for every
      // The server-wide session policy is the settings group's four rows
      // resolved; a confined credential reads its own domain's instead.
      case RouteSessionPolicyGet:
      // domain on the server. A help desk confined to one of them lifts one
      // person's lockout with POST /api/v1/accounts/{address}/unlock instead.
      // Written down in docs/Roles.md, "The logon-failure list".
      case RouteLogonFailuresClear:
      case RouteSettingsScriptingGet:
      case RouteSettingsScriptingPut:
      case RouteScriptingReload:
      case RouteScriptingCheck:
      case RouteBreachCheckTest:
      case RouteSettingsCacheGet:
      case RouteSettingsCachePut:
      case RouteCacheClear:
      case RouteSettingsIndexingGet:
      case RouteSettingsIndexingPut:
      case RouteIndexingIndex:
      case RouteIndexingClear:
      case RouteSettingsBackupGet:
      case RouteSettingsBackupPut:
      // The bounce settings apply to every domain, and the sweep removes entries
      // The bounce settings apply to every domain, and the sweep removes entries
      // from every domain's list.
      case RouteSettingsBouncesGet:
      case RouteSettingsBouncesPut:
      case RouteBounceExpire:
      // The Web Key Directory's own settings apply to every domain.
      case RouteSettingsOpenPgpGet:
      case RouteSettingsOpenPgpPut:
      case RouteServerMessageList:
      case RouteServerMessagePut:
      case RouteSieveEvaluate:
      case RouteRuleMatch:
      case RouteCalendarExpand:
      case RouteIpRangeUpdate:
      case RouteRuleCreate:
      case RouteRuleUpdate:
      case RouteRuleDelete:
      // An address's traces are in every domain's tables, and the second
      // factor is the server's credential.
      case RouteAddressErase:
      case RouteAdminSecondFactorEnrol:
      case RouteAdminSecondFactorDisable:
      case RouteCertificateCreate:
      case RouteCertificateDelete:
      case RoutePortList:
      case RoutePortCreate:
      case RoutePortUpdate:
      case RoutePortDelete:
      case RouteRouteList:
      case RouteRouteCreate:
      case RouteRouteUpdate:
      case RouteRouteDelete:
      // A remote domain policy names somebody else's domain, not one hosted
      // here, so "restricted to these domains" cannot scope it - and what it
      // decides (whether this server refuses to deliver a domain's mail in the
      // clear, whether it opens verification sessions to a third party) is the
      // server's posture rather than any one hosted domain's.
      case RouteRemoteDomainList:
      case RouteRemoteDomainCreate:
      case RouteRemoteDomainUpdate:
      case RouteRemoteDomainDelete:
      case RouteRemoteDomainEffective:
      case RouteRemoteDomainClearCache:
      // The audit trail records every domain's changes and the alerts watch the
      // whole server. A key never gets this far with them (IsApiKeyRoute_); a
      // role confined to named domains - a custom role holding view_audit and
      // a list of domains - does, and is refused here.
      case RouteAuditList:
      case RouteAuditVerify:
      case RouteAuditRevert:
      case RouteAlertRuleList:
      case RouteAlertRuleUpdate:
      case RouteAlertEventList:
      case RouteAlertTest:
      case RouteAlertRun:
      // A subscription reaches every domain it is not confined to, and the
      // queue holds every domain's events. A role confined to named domains is
      // refused them here, as it is the alerts.
      case RouteWebhookList:
      case RouteWebhookGet:
      case RouteWebhookCreate:
      case RouteWebhookUpdate:
      case RouteWebhookDelete:
      case RouteWebhookDeliveryList:
      case RouteWebhookRun:
      case RouteWebhookTest:
      // What is held for review has the queue's shape exactly - one entry
      // names a sender and recipients in any number of domains, and releasing
      // one delivers mail - so it is refused to a confined role as the queue
      // is. An account's own review state is scoped to its domain instead
      // (DomainNamedByRoute_).
      case RouteReviewList:
      case RouteReviewRelease:
      case RouteReviewDiscard:
      case RouteServerReinitialize:
      case RouteUpdateGet:
      case RouteUpdateCheck:
      case RouteUpdateDownload:
      case RouteUpdateInstall:
      case RouteDiagnosticsRun:
      // A group holds accounts of any domain and is an ACL principal on any
      // folder: a key issued for one domain could add its own account to a
      // group another domain's folders trust, so the groups are the server's.
      case RouteGroupList:
      case RouteGroupCreate:
      case RouteGroupGet:
      case RouteGroupUpdate:
      case RouteGroupDelete:
      case RouteGroupMemberList:
      case RouteGroupMemberCreate:
      case RouteGroupMemberDelete:
      // One pass over every domain's DKIM key rotation, with a clock the caller
      // moves: a key issued for one domain would be advancing everybody's.
      case RouteDkimRotationRun:
      // The addresses this server sends from are the same whichever domain's
      // mail is leaving, so its reputation is not any one domain's business.
      case RouteReputationGet:
      case RouteReputationCheck:
      // The first-hour checklist judges the submission port, the backup
      // destination and the administrator's second factor, none of which is
      // any domain's.
      case RouteSetupChecklist:
      // The sample domain is a domain this server does not have yet, so a
      // credential confined to the domains it does have names nothing here.
      case RouteSampleDomainGet:
      case RouteSampleDomainCreate:
      case RouteSampleDomainRemove:
         // IP ranges, certificates, global rules, the logs, the backup, the
         // server settings and the update check are all server-wide: none of
         // them belongs to a domain, and the logs in particular carry every
         // domain's traffic.
         refusalReason = "this credential is restricted to named domains, and that resource is server-wide";
         return AuthorizationForbidden;
      default:
         break;
      }

      // Which domain this route names, if any. Empty means the route is
      // server-wide and read-only - /status, /tlsa, /srv and the domain
      // listing. The latter two filter their per-domain output to the
      // credential's domains in their handlers rather than being refused here
      // (a listing that answered 403 would be useless to exactly the
      // credential the restriction exists for, and /srv names every local
      // domain in its records - which a key issued for one customer must not
      // be handed).
      const String targetDomain = DomainNamedByRoute_(route);

      if (targetDomain.IsEmpty())
         return AuthorizationAllowed;

      if (!IsDomainAllowed_(caller.domains, targetDomain))
      {
         refusalReason = "this credential is not permitted for that domain";
         return AuthorizationForbidden;
      }

      return AuthorizationAllowed;
   }

   HttpResponse
   RestApiServer::BuildResponse_(int statusCode, const AnsiString &body, const AnsiString &extraHeaders)
   {
      // The status line, the framing headers and the connection header are the
      // server's; a status it does not know becomes a 500 there, deliberately -
      // a wrong number in a response line is worse than an honest server error.
      HttpResponse response;
      response.status = statusCode;
      response.content_type = "application/json";
      response.body = body;
      response.extra_headers = extraHeaders;

      // Nothing the API answers may be served from a cache: every response is
      // the state of the server at that moment, and most of them were only
      // given because a credential was presented with the request. A browser
      // or a proxy that kept one would hand it to the next caller.
      response.extra_headers += "Cache-Control: no-store\r\n";

      return response;
   }

   bool
   RestApiServer::IsPageScriptRequest_(const AnsiString &request)
   {
      // Sec-Fetch-Mode is sent by every current browser and says how the
      // request was made: "navigate" is the address bar or a link, and anything
      // else - cors, same-origin, no-cors - is a script's fetch.
      // X-Requested-With is what this server's own pages set on a write.
      AnsiString mode = GetHeader_(request, "sec-fetch-mode");
      mode.ToLower();

      if (!mode.IsEmpty() && mode != "navigate")
         return true;

      return !GetHeader_(request, "x-requested-with").IsEmpty();
   }

   HttpResponse
   RestApiServer::BuildUnauthorizedResponse_(bool secondFactorRequired, bool suppressChallenge)
   {
      // One response for every possible authentication problem: no credential,
      // a wrong administrator password, an unknown API key, an expired key and
      // a key refused by source address are all answered identically. Telling
      // the caller that the token was valid but expired, or valid but presented
      // from the wrong network, would confirm a working secret.
      //
      // The one exception is deliberate: when the administrator PASSWORD was
      // accepted and the one-time code was what was missing or wrong, the
      // response carries "X-hMailServer-OTP: required" - the shape GitHub's API
      // uses - so a client knows to ask its user for the code. That confirms
      // the password to somebody who already holds it, and nothing to anybody
      // else.
      //
      // The challenge advertises Basic only, exactly as before - for every
      // caller it helps. It is left out for one kind of request: the fetch a
      // page's own script makes. A browser answers WWW-Authenticate by opening
      // its own credential box, in front of the page that asked, and the fetch
      // does not settle until somebody answers it - so the Control Deck and the
      // portal, which sign in through a form and show this response's own
      // sentence, were talking to a user standing behind a box they never put
      // there. A script, curl, and a browser that navigated straight to a route
      // still get the challenge, because for them the box is the way in.
      const AnsiString body = secondFactorRequired
         ? "{\"error\":\"authentication failed\",\"second_factor\":\"required\"}"
         : "{\"error\":\"authentication failed\"}";

      AnsiString headers;

      if (!suppressChallenge)
         headers += "WWW-Authenticate: Basic realm=\"hMailServer\"\r\n";
      if (secondFactorRequired)
         headers += "X-hMailServer-OTP: required\r\n";

      return BuildResponse_(401, body, headers);
   }

   HttpResponse
   RestApiServer::BuildForbiddenResponse_(const AnsiString &reason)
   {
      // 403 and not 401, and it says why.
      //
      // The refusals answered 401 above are about the *credential*, where every
      // extra word confirms something to somebody holding a token they should
      // not have. This one is about permission: the caller has already proved
      // its key is genuine (it got this far), so concealing which restriction
      // stopped it protects nothing and costs an administrator an afternoon
      // wondering why a key that authenticates cannot delete an account.
      //
      // The reasons are fixed sentences written here. None of them names a file,
      // a query, a row or another domain.
      AnsiString body;
      body.Format("{\"error\":\"%hs\"}", JsonEscape_(reason).c_str());

      return BuildResponse_(403, body);
   }

   HttpResponse
   RestApiServer::BuildTooManyRequestsResponse_()
   {
      // Retry-After is the whole window. The window is fixed rather than
      // sliding, so a caller that waits that long is certainly inside a new one
      // - which is what makes the advice honest rather than a guess.
      AnsiString retryAfter;
      retryAfter.Format("Retry-After: %d\r\n", (int) (RateWindowMilliseconds / 1000));

      return BuildResponse_(429, "{\"error\":\"too many requests\"}", retryAfter);
   }

   String
   RestApiServer::GetApiKeyStoreFile()
   {
      // Storage decision: a dedicated ini file alongside hMailServer.ini.
      //
      //  - The database schema may not change, and the keys are a property of
      //    this listener rather than of any mail object, so no existing table
      //    fits.
      //  - hMailServer.ini itself is owned by IniFileSettings, which caches its
      //    values at InitInstance and would need to change to hold a list; a
      //    separate file needs no change there at all.
      //  - The service account already has write access to that directory (it
      //    writes hMailServer.ini), and the directory is not web-served.
      //  - The file is read on every authentication attempt, so adding or
      //    revoking a key takes effect immediately: no restart, no rebuild.
      //
      // Format - one section per key, section name "Key.<id>":
      //
      //    [Key.3f9a1c4b5d6e7f80]
      //    Label=CI deploy
      //    Hash=<64 lower-case hex characters>
      //    Expires=2027-01-01 00:00:00
      //    AllowedFrom=10.0.0.0/24
      //    Scope=full
      //    Domains=example.com,example.net
      //
      // Scope and Domains both fail closed, which is what makes hand-editing
      // this file safe: a section with no Scope, or a Scope value that is not
      // the literal "full", is read-only, and a Domains list that survives
      // normalisation as nothing at all leaves the key able to reach no domain.
      //
      // An administrator can revoke a key by hand by deleting its section, and
      // can revoke every key by deleting the file; either takes effect on the
      // next request. Adding a key by hand is possible but pointless, since only
      // the hash is stored and the clear text is never recoverable -
      // POST /api/v1/apikeys is the supported way in.
      String iniFile = IniFileSettings::GetInitializationFile();

      return FileUtilities::Combine(FileUtilities::GetFilePath(iniFile), _T("hMailServerApiKeys.ini"));
   }

   std::vector<RestApiServer::ApiKeyRecord>
   RestApiServer::LoadKeys_()
   {
      // Why SHA-256 and not Argon2id or PBKDF2, both of which Crypt offers:
      //
      //  - Those are deliberately slow because they defend low-entropy,
      //    human-chosen passwords against offline guessing. An API key here is
      //    32 bytes straight from RAND_bytes; there is no dictionary to run and
      //    no amount of hash speed that makes 256 bits guessable, so the
      //    slowness buys nothing.
      //  - It would cost something real. Argon2id as configured in HashCreator
      //    allocates 19 MiB and burns two passes over it per verification, on
      //    the single REST worker thread, for any unauthenticated stranger who
      //    sends "Authorization: Bearer junk". That is a free remote CPU and
      //    memory denial of service against the management port - a worse
      //    security bug than the one being fixed.
      //  - A per-record salt would also make lookup impossible without running
      //    the KDF once per stored key, multiplying that cost by the number of
      //    keys. An unsalted digest lets us hash the presented token once and
      //    compare, in constant time, against each stored value.
      //
      // So: HashCreator(SHA256).GenerateHashNoSalt(token, hex) - an existing
      // primitive, used the way the rest of the server already uses it, with no
      // new hashing code introduced.
      //
      // The store is parsed from the file's bytes rather than through
      // GetPrivateProfileString. Writes still go through
      // WritePrivateProfileString, which merges correctly and leaves an
      // administrator's comments alone, but the Windows profile functions keep
      // a cache, and a read served from that cache would mean a key deleted by
      // hand in this file carried on working. There is no cache in this path,
      // so a revocation - through the API or in an editor - takes effect on the
      // very next request.
      std::vector<ApiKeyRecord> keys;

      String storeFile = GetApiKeyStoreFile();

      // No file means no keys. This is the shipped default and is emphatically
      // not an error, so nothing is reported.
      if (!FileUtilities::Exists(storeFile))
         return keys;

      String content = FileUtilities::ReadCompleteTextFile(storeFile);
      if (content.IsEmpty())
         return keys;

      String sectionPrefix(ApiKeySectionPrefix);

      ApiKeyRecord current;
      bool haveSection = false;

      // A record is only usable once its Hash has been seen, so records are
      // committed when the next section starts (and once more at the end).
      auto commit = [&keys, &current, &haveSection, &storeFile]()
      {
         if (!haveSection)
            return;

         haveSection = false;

         // A record whose hash is not a full SHA-256 digest is unusable. It is
         // skipped rather than compared against, because a short or mangled
         // value would otherwise be compared over its own length only - which
         // is exactly how a one-character "hash" would match everything.
         //
         // Deliberately LOG_APPLICATION and not ErrorManager. This condition is
         // reached by hand-editing the store, it is re-evaluated on every single
         // request, and a reported error per request would bury the ERROR log
         // (and fail the fixtures that assert it is clean) over something whose
         // only symptom is one key not working - which the administrator sees
         // immediately anyway.
         if (IsLowerHex(current.hash, ApiKeyHashHexLength))
            keys.push_back(current);
         else
         {
            LOG_APPLICATION("RestApi: Ignoring a record in " + storeFile +
               " with no usable Hash value. Key: " + current.id + ".");
         }

         current = ApiKeyRecord();
      };

      std::vector<String> lines = StringParser::SplitString(content, _T("\n"));

      for (const String &rawLine : lines)
      {
         String line = rawLine;
         line.Trim();

         if (line.IsEmpty() || line.StartsWith(_T(";")) || line.StartsWith(_T("#")))
            continue;

         if (line.StartsWith(_T("[")))
         {
            commit();

            if (!line.EndsWith(_T("]")))
               continue;

            String section = line.Mid(1, line.GetLength() - 2);
            section.Trim();

            if (!section.StartsWith(sectionPrefix))
               continue;

            // StartsWith is case-insensitive, so a hand-written [KEY.AB12...]
            // section is accepted here. The id is normalised to lower case
            // because HandleRevokeApiKey_ only accepts ids that are lower-case
            // hex; without this, such a key would be listed by
            // GET /api/v1/apikeys under an id that DELETE /api/v1/apikeys/<id>
            // answers 404 for, leaving it unrevocable through the API.
            current.id = section.Mid(sectionPrefix.GetLength());
            current.id.MakeLower();

            haveSection = true;
            continue;
         }

         if (!haveSection)
            continue;

         int equalsPosition = line.Find(_T("="));
         if (equalsPosition <= 0)
            continue;

         String name = line.Mid(0, equalsPosition);
         String value = line.Mid(equalsPosition + 1);

         name.Trim();
         value.Trim();

         if (name.CompareNoCase(_T("Hash")) == 0)
            current.hash = AnsiString(value);
         else if (name.CompareNoCase(_T("Label")) == 0)
            current.label = value;
         else if (name.CompareNoCase(_T("Expires")) == 0)
            current.expires = value;
         else if (name.CompareNoCase(_T("AllowedFrom")) == 0)
            current.allowed_from = value;
         else if (name.CompareNoCase(_T("Role")) == 0)
         {
            // A named role, which is the whole of the key's authority when it
            // is present. Lower-cased here so that the store and the database
            // compare equal whatever an administrator typed.
            current.role = value;
            current.role.MakeLower();
         }
         else if (name.CompareNoCase(_T("Scope")) == 0)
         {
            // Only the literal "full" widens a key. Every other value - a
            // misspelling, a truncated write, a line an administrator meant to
            // comment out - leaves it read-only, because the opposite default
            // would turn a typo into write access over every domain.
            current.read_only = value.CompareNoCase(ApiKeyScopeFull) != 0;
         }
         else if (name.CompareNoCase(_T("Domains")) == 0)
         {
            // Normalised on the way in - trimmed, lower-cased, empty items
            // dropped - so that the exact comparison in IsDomainAllowed_ is the
            // only thing that has to be right.
            //
            // An entry that is not a real domain name is kept and simply matches
            // nothing, which is the fail-closed direction. A value that is empty
            // or is nothing but separators leaves the list empty, which means
            // "every domain" - the same reading AllowedFrom gives an empty value,
            // and the only one consistent with a section that has no Domains line
            // at all.
            current.domains.clear();

            std::vector<String> parts = StringParser::SplitString(value, _T(","));

            for (const String &part : parts)
            {
               String domainName = part;
               domainName.Trim();
               domainName.MakeLower();

               if (!domainName.IsEmpty())
                  current.domains.push_back(domainName);
            }
         }
      }

      commit();

      return keys;
   }

   bool
   RestApiServer::IsExpired_(const String &expires)
   {
      // Fail closed: a missing or unparseable expiry counts as expired. A key
      // that never expires is the property that makes the administrator
      // password dangerous in the first place, so there is no "no expiry" case
      // to fall through to.
      if (expires.GetLength() < 19)
         return true;

      DateTime expiryTime = Time::GetDateFromSystemDate(expires);
      if (expiryTime.GetStatus() != DateTime::valid)
         return true;

      DateTime now = DateTime::GetCurrentTime();

      return now >= expiryTime ? true : false;
   }

   bool
   RestApiServer::IsSourceAllowed_(const String &allowed_from, const IPAddress &peer_address)
   {
      // No restriction configured: any source, which is the behaviour of the
      // administrator password today and so is not a regression.
      String restriction = allowed_from;
      restriction.Trim();

      if (restriction.IsEmpty())
         return true;

      IPAddress lower;
      IPAddress upper;

      // A restriction we cannot parse refuses the request rather than being
      // ignored. A typo in the store must not quietly widen a key's scope.
      if (!ParseSourceRestriction(AnsiString(restriction), lower, upper))
         return false;

      // WithinRange compares the low 64 bits for IPv4 and both halves for IPv6,
      // so mixing families would compare unrelated numbers. A restriction
      // written in the other family simply does not match.
      if (peer_address.GetType() != lower.GetType() || lower.GetType() != upper.GetType())
         return false;

      return peer_address.WithinRange(lower, upper);
   }

   HttpResponse
   RestApiServer::HandleListApiKeys_()
   {
      // Metadata only. The hash is not returned: it is not a usable credential,
      // but publishing it over the API would hand an offline target to anyone
      // who briefly held the administrator password.
      std::vector<ApiKeyRecord> keys = LoadKeys_();

      AnsiString items;
      int count = 0;

      for (const ApiKeyRecord &key : keys)
      {
         // scope and domains are reported because a restriction an administrator
         // cannot see is a restriction they will not trust: the whole point of
         // issuing a narrow key is being able to confirm afterwards that it is
         // narrow.
         AnsiString item;
         item.Format("{\"id\":\"%hs\",\"label\":\"%hs\",\"scope\":\"%hs\",\"role\":\"%hs\",\"domains\":\"%hs\","
                     "\"expires\":\"%hs\",\"allowed_from\":\"%hs\",\"expired\":%hs}",
            JsonEscape_(Utf8_(key.id)).c_str(),
            JsonEscape_(Utf8_(key.label)).c_str(),
            key.read_only ? ApiKeyScopeReadOnlyNarrow : ApiKeyScopeFullNarrow,
            JsonEscape_(Utf8_(key.role)).c_str(),
            JsonEscape_(Utf8_(StringParser::JoinVector(key.domains, _T(",")))).c_str(),
            JsonEscape_(Utf8_(key.expires)).c_str(),
            JsonEscape_(Utf8_(key.allowed_from)).c_str(),
            IsExpired_(key.expires) ? "true" : "false");

         if (count > 0)
            items += ",";

         items += item;
         count++;
      }

      AnsiString body;
      body.Format("{\"count\":%d,\"keys\":[%hs]}", count, items.c_str());

      return BuildResponse_(200, body);
   }

   HttpResponse
   RestApiServer::HandleCreateApiKey_(const AnsiString &requestBody)
   {
      AnsiString label = GetJsonStringValue_(requestBody, "label");
      AnsiString expires = GetJsonStringValue_(requestBody, "expires");
      AnsiString allowedFrom = GetJsonStringValue_(requestBody, "allowed_from");
      AnsiString scope = GetJsonStringValue_(requestBody, "scope");
      AnsiString domains = GetJsonStringValue_(requestBody, "domains");
      AnsiString role = GetJsonStringValue_(requestBody, "role");

      label.Trim();
      expires.Trim();
      allowedFrom.Trim();
      scope.Trim();
      domains.Trim();
      role.Trim();
      role.MakeLower();

      if (label.IsEmpty())
         return BuildResponse_(400, "{\"error\":\"label is required\"}");

      // The label ends up in an ini value and in log lines, so keep it to
      // something printable and one line long.
      if (label.GetLength() > 64)
         return BuildResponse_(400, "{\"error\":\"label must be 64 characters or fewer\"}");

      for (int i = 0; i < label.GetLength(); i++)
      {
         unsigned char character = static_cast<unsigned char>(label[i]);
         if (character < 0x20 || character == 0x7F)
            return BuildResponse_(400, "{\"error\":\"label must not contain control characters\"}");
      }

      // Least privilege by default. A create request that does not name a scope
      // gets a read-only key, because the alternative is that every caller who
      // has not read the documentation is handed a credential that can delete
      // accounts - and a key is most often minted for something that only reads
      // (a monitoring probe, a CI status check). "full" is one word away for the
      // callers that need it, and the 201 below says which one they got.
      bool readOnly = true;

      if (!scope.IsEmpty())
      {
         if (scope.CompareNoCase(ApiKeyScopeFullNarrow) == 0)
            readOnly = false;
         else if (scope.CompareNoCase(ApiKeyScopeReadOnlyNarrow) != 0)
            return BuildResponse_(400, "{\"error\":\"scope must be 'readonly' or 'full'\"}");
      }

      // A named role instead of a scope, which is how a key is issued for a
      // help desk or an auditor rather than for "everything" or "reads". The
      // role has to exist now, because a key that names a role the server does
      // not have holds nothing, and finding that out at the first request is
      // finding it out too late.
      if (!role.IsEmpty())
      {
         RoleRecord record;
         if (!RoleManager::FindRole(String(role), record))
            return BuildResponse_(400, "{\"error\":\"no such role\"}");
      }

      // The domain restriction is normalised here and stored in that form, so
      // that nothing the caller typed reaches the store verbatim and the value
      // LoadKeys_ reads back is the value this function decided on.
      String normalizedDomains;

      if (!domains.IsEmpty())
      {
         std::vector<AnsiString> parts = StringParser::SplitString(domains, ",");

         for (const AnsiString &part : parts)
         {
            String domainName = String(part);
            domainName.Trim();
            domainName.MakeLower();

            if (domainName.IsEmpty())
               continue;

            // Refuse a restriction we would refuse every request against,
            // rather than issuing a key that can never reach anything. The
            // domain does not have to exist yet - a key may legitimately be
            // issued before the domain it will manage - but it does have to be
            // a domain name.
            if (!StringParser::IsValidDomainName(domainName))
               return BuildResponse_(400, "{\"error\":\"domains must be a comma-separated list of domain names\"}");

            if (!normalizedDomains.IsEmpty())
               normalizedDomains += _T(",");

            normalizedDomains += domainName;
         }

         // The caller asked for a restriction and nothing survived
         // normalisation ("domains":" , "). Storing that would silently mean
         // "every domain", which is the opposite of what was asked for.
         if (normalizedDomains.IsEmpty())
            return BuildResponse_(400, "{\"error\":\"domains must be a comma-separated list of domain names\"}");
      }

      if (expires.IsEmpty())
      {
         // No expiry named: default to a bounded lifetime rather than forever.
         DateTimeSpan span;
         span.SetDateTimeSpan(ApiKeyDefaultLifetimeDays, 0, 0, 0);

         DateTime expiryTime = DateTime::GetCurrentTime() + span;
         expires = AnsiString(Time::GetTimeStampFromDateTime(expiryTime));
      }

      // Validate the caller's expiry rather than storing something that would
      // silently be treated as expired on first use.
      if (IsExpired_(String(expires)))
         return BuildResponse_(400, "{\"error\":\"expires must be a future date in the form YYYY-MM-DD HH:MM:SS\"}");

      // Then store the parsed date rather than the text that produced it.
      //
      // Defence in depth against ini injection: the store is one section per
      // key and is parsed line by line from the file's bytes, so any value that
      // reached WritePrivateProfileString carrying a CRLF would appear to
      // LoadKeys_ as further lines - a second [Key.*] section, or a Scope=full
      // line under an existing one. The label is already refused if it holds a
      // control character and AllowedFrom has to parse as an address, but the
      // expiry had only a length-and-parse check, which says nothing about what
      // follows the nineteenth character. Canonicalising removes the question
      // instead of answering it: what is written is generated here.
      DateTime canonicalExpiry = Time::GetDateFromSystemDate(String(expires));
      if (canonicalExpiry.GetStatus() != DateTime::valid)
         return BuildResponse_(400, "{\"error\":\"expires must be a future date in the form YYYY-MM-DD HH:MM:SS\"}");

      expires = AnsiString(Time::GetTimeStampFromDateTime(canonicalExpiry));

      // Same for the source restriction: reject a form we would refuse every
      // request against, instead of issuing a key that can never be used.
      if (!allowedFrom.IsEmpty())
      {
         IPAddress lower;
         IPAddress upper;

         if (!ParseSourceRestriction(allowedFrom, lower, upper))
            return BuildResponse_(400, "{\"error\":\"allowed_from must be an address, an address range 'lower-upper', or CIDR\"}");
      }

      unsigned char secret[ApiKeySecretBytes];
      unsigned char idBytes[ApiKeyIdBytes];

      if (RAND_bytes(secret, sizeof(secret)) != 1 || RAND_bytes(idBytes, sizeof(idBytes)) != 1)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5791, "RestApiServer::HandleCreateApiKey_",
            "Failed to obtain random bytes for a new REST API key. No key was created.");
         return BuildResponse_(500, "{\"error\":\"internal error\"}");
      }

      AnsiString token = AnsiString(ApiKeyTokenPrefix) + BytesToLowerHex(secret, ApiKeySecretBytes);
      AnsiString hash = HashApiKeyToken(token);

#ifdef HM_PLATFORM_POSIX
      // explicit_bzero is SecureZeroMemory's promise on this platform: a write
      // the compiler is not allowed to remove because the buffer is dead
      // afterwards. A plain memset here would be optimised away and the raw key
      // would stay on the stack of the REST worker thread.
      ::explicit_bzero(secret, sizeof(secret));
#else
      SecureZeroMemory(secret, sizeof(secret));
#endif

      if (!IsLowerHex(hash, ApiKeyHashHexLength))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5792, "RestApiServer::HandleCreateApiKey_",
            "Failed to hash a new REST API key. No key was created.");
         return BuildResponse_(500, "{\"error\":\"internal error\"}");
      }

      String id = String(BytesToLowerHex(idBytes, ApiKeyIdBytes));
      String section = String(ApiKeySectionPrefix) + id;
      String storeFile = GetApiKeyStoreFile();

      if (!FileUtilities::Exists(storeFile))
      {
         // Explain the file inside the file. This is the only place an
         // administrator will go looking, and LoadKeys_ ignores ';' lines. A
         // failure here is not fatal: WritePrivateProfileString below creates
         // the file anyway, just without the preamble.
         AnsiString preamble;
         preamble += "; hMailServer REST administration API keys.\r\n";
         preamble += ";\r\n";
         preamble += "; One section per key. Only the SHA-256 digest of a key is stored, so a\r\n";
         preamble += "; key cannot be recovered from this file: it is shown once, in the reply\r\n";
         preamble += "; to POST /api/v1/apikeys, and never again.\r\n";
         preamble += ";\r\n";
         preamble += "; To revoke a key, either DELETE /api/v1/apikeys/<id> or delete its\r\n";
         preamble += "; section below. Both take effect on the next request - no restart and no\r\n";
         preamble += "; rebuild. Deleting this file revokes every key.\r\n";
         preamble += ";\r\n";
         preamble += "; Expires     YYYY-MM-DD HH:MM:SS, local time. Required. A key whose\r\n";
         preamble += ";             expiry is missing or unreadable counts as expired.\r\n";
         preamble += "; AllowedFrom Optional. An address, a 'lower-upper' range, or CIDR.\r\n";
         preamble += ";             Empty means any source address.\r\n";
         preamble += "; Scope       'full' or 'readonly'. Anything else - including a missing\r\n";
         preamble += ";             line - is readonly, so a typo cannot widen a key. A\r\n";
         preamble += ";             readonly key is refused every request that changes\r\n";
         preamble += ";             something.\r\n";
         preamble += "; Domains     Optional, comma-separated. Empty means every domain. A key\r\n";
         preamble += ";             with a list may only act on those domains, and is refused\r\n";
         preamble += ";             the delivery-queue endpoints outright because the queue is\r\n";
         preamble += ";             server-wide.\r\n";
         preamble += ";\r\n";
         preamble += "; No key of any scope can create or revoke keys: that needs the\r\n";
         preamble += "; administrator password.\r\n";
         preamble += "\r\n";

         FileUtilities::WriteToFile(storeFile, preamble);
      }

      // Write the hash last: until it is there the section is ignored by
      // LoadKeys_, so a failure part way through leaves an unusable record
      // rather than a usable key with no expiry - or, now, one with no
      // restrictions. Scope and Domains are written before it for the same
      // reason: a key that became usable before its restrictions landed would be
      // briefly unrestricted, and briefly is enough.
      bool written =
         WritePrivateProfileString(section, _T("Label"), String(label), storeFile) != FALSE &&
         WritePrivateProfileString(section, _T("Expires"), String(expires), storeFile) != FALSE &&
         WritePrivateProfileString(section, _T("AllowedFrom"), String(allowedFrom), storeFile) != FALSE &&
         WritePrivateProfileString(section, _T("Scope"), readOnly ? ApiKeyScopeReadOnly : ApiKeyScopeFull, storeFile) != FALSE &&
         WritePrivateProfileString(section, _T("Domains"), normalizedDomains, storeFile) != FALSE &&
         WritePrivateProfileString(section, _T("Role"), String(role), storeFile) != FALSE &&
         WritePrivateProfileString(section, _T("Hash"), String(hash), storeFile) != FALSE;

      // Flush the profile cache so the very next request sees the new key.
      WritePrivateProfileString(nullptr, nullptr, nullptr, storeFile);

      if (!written)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5790, "RestApiServer::HandleCreateApiKey_",
            "Failed to write the REST API key store. No key was created. File: " + storeFile);

         WritePrivateProfileString(section, nullptr, nullptr, storeFile);
         WritePrivateProfileString(nullptr, nullptr, nullptr, storeFile);

         return BuildResponse_(500, "{\"error\":\"failed to store the key\"}");
      }

      // The scope is in the log line as well as the response: which keys are
      // full-authority is the thing an administrator will want to answer months
      // later from the log alone.
      String created;
      created.Format(_T("RestApi: API key '%s' (%s) created. Scope: %s. Role: %s. Domains: %s."),
         String(label).c_str(), id.c_str(),
         readOnly ? ApiKeyScopeReadOnly : ApiKeyScopeFull,
         role.IsEmpty() ? _T("(from the scope)") : String(role).c_str(),
         normalizedDomains.IsEmpty() ? _T("(all)") : normalizedDomains.c_str());
      LOG_APPLICATION(created);

      // The clear-text token is returned exactly once, here. It is not stored
      // and cannot be recovered afterwards.
      AnsiString body;
      body.Format("{\"id\":\"%hs\",\"label\":\"%hs\",\"scope\":\"%hs\",\"role\":\"%hs\",\"domains\":\"%hs\","
                  "\"expires\":\"%hs\",\"allowed_from\":\"%hs\",\"key\":\"%hs\"}",
         JsonEscape_(Utf8_(id)).c_str(),
         JsonEscape_(label).c_str(),
         readOnly ? ApiKeyScopeReadOnlyNarrow : ApiKeyScopeFullNarrow,
         JsonEscape_(role).c_str(),
         JsonEscape_(Utf8_(normalizedDomains)).c_str(),
         JsonEscape_(expires).c_str(),
         JsonEscape_(allowedFrom).c_str(),
         JsonEscape_(token).c_str());

      return BuildResponse_(201, body);
   }

   HttpResponse
   RestApiServer::HandleRevokeApiKey_(const AnsiString &id)
   {
      // Only ids of the shape we hand out, so that nothing here can be talked
      // into naming another section (or another file).
      if (!IsLowerHex(id, ApiKeyIdBytes * 2))
         return BuildResponse_(404, "{\"error\":\"api key not found\"}");

      String storeFile = GetApiKeyStoreFile();
      String section = String(ApiKeySectionPrefix) + String(id);

      std::vector<ApiKeyRecord> keys = LoadKeys_();

      bool exists = false;
      String label;

      for (const ApiKeyRecord &key : keys)
      {
         if (key.id.CompareNoCase(String(id)) == 0)
         {
            exists = true;
            label = key.label;
         }
      }

      if (!exists)
         return BuildResponse_(404, "{\"error\":\"api key not found\"}");

      // Passing a null key name deletes the whole section.
      BOOL deleted = WritePrivateProfileString(section, nullptr, nullptr, storeFile);
      WritePrivateProfileString(nullptr, nullptr, nullptr, storeFile);

      if (deleted == FALSE)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5790, "RestApiServer::HandleRevokeApiKey_",
            "Failed to write the REST API key store. The key was not revoked. File: " + storeFile);
         return BuildResponse_(500, "{\"error\":\"failed to revoke the key\"}");
      }

      LOG_APPLICATION("RestApi: API key '" + label + "' (" + String(id) + ") revoked.");

      return BuildResponse_(200, "{\"revoked\":true}");
   }

   // A file of the Control Deck's directory, <ProgramFolder>/WebAdmin, read as
   // the bytes on disk. The page and its catalogues are UTF-8 and carry
   // characters outside the system code page (the navigation glyphs, every
   // translated text); a round trip through String and back would have put
   // them through the ANSI code page and served question marks. False when
   // the file is not there.
   bool
   RestApiServer::ReadWebAdminFile_(const String &relativePath, AnsiString &body)
   {
      const String filePath = FileUtilities::Combine(
         FileUtilities::Combine(IniFileSettings::Instance()->GetProgramDirectory(), _T("WebAdmin")), relativePath);
      if (!FileUtilities::Exists(filePath))
         return false;
#ifdef HM_PLATFORM_POSIX
      // MSVC's <fstream> has a constructor taking a wide path; libstdc++ has
      // none, because a file name here is bytes. Narrowed the way the rest of
      // the tree narrows a String for a C API.
      const AnsiString narrowPath = filePath.c_str();
      std::ifstream stream(narrowPath.c_str(), std::ios::binary);
#else
      std::ifstream stream(filePath.c_str(), std::ios::binary);
#endif
      std::string bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
      body = bytes.c_str();
      if (body.GetLength() != bytes.size())
         body = AnsiString(bytes);
      return true;
   }

   HttpResponse
   RestApiServer::HandleWebAdminPage_()
   {
      AnsiString body;
      if (!ReadWebAdminFile_(_T("index.html"), body))
      {
         body = "<!doctype html><html><body style=\"font-family:sans-serif\">"
                "<h1>hMailServer</h1><p>Web administration page not installed. "
                "The REST API is available under /api/v1/.</p></body></html>";
      }

      HttpResponse response;
      response.content_type = "text/html; charset=utf-8";
      response.body = body;
      // Read from disk on every request, compressed once per distinct file:
      // the cache is keyed by the bytes, so an upgrade that replaces the file
      // is a new entry, never a stale one.
      pageCache_.Fill(response);
      // The same discipline the portal gets. This page has inline script and
      // style of its own, so those are allowed for it; what is refused is
      // anything from anywhere else, any frame, any form posted off this
      // origin, and a base tag that would redirect its relative calls.
      response.extra_headers =
         "Content-Security-Policy: default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; img-src 'self' data:; connect-src 'self'; form-action 'self'; frame-ancestors 'none'; base-uri 'none'\r\n"
         "X-Content-Type-Options: nosniff\r\n"
         "Referrer-Policy: no-referrer\r\n"
         "Cache-Control: no-store\r\n";

      return response;
   }

   // GET /deck-lang/<code>.json: the Deck's texts in one language, the file
   // <ProgramFolder>/WebAdmin/languages/<code>.json the packages install
   // beside the page. The page fetches it for the language the browser
   // prefers or the administrator chose in its header, and shows English for
   // anything the catalogue lacks. Read from disk on every request, as the
   // page is - an upgrade replaces both, and a page with a stale catalogue
   // would show its new texts in English - and, like the page, compressed
   // once per distinct file rather than once per request.
   HttpResponse
   RestApiServer::HandleWebAdminLanguage_(const AnsiString &code)
   {
      // The code names a file, so it is letters with a dash between them and
      // nothing else - no separator, no dot, no way out of the directory - and
      // a tag's length: two letters, or a language and a region or a script.
      bool wellFormed = code.GetLength() >= 2 && code.GetLength() <= 12;
      for (int i = 0; wellFormed && i < code.GetLength(); i++)
      {
         const char c = code[i];
         const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
         wellFormed = letter || (c == '-' && i > 0 && i + 1 < code.GetLength());
      }

      AnsiString body;
      if (!wellFormed || !ReadWebAdminFile_(FileUtilities::Combine(_T("languages"), String(code) + _T(".json")), body))
         return BuildResponse_(404, "{\"error\":\"no such language\"}");

      HttpResponse response;
      response.status = 200;
      response.content_type = "application/json; charset=utf-8";
      response.body = body;
      pageCache_.Fill(response);
      response.extra_headers =
         "X-Content-Type-Options: nosniff\r\n"
         "Cache-Control: no-store\r\n";
      return response;
   }

   HttpResponse
   RestApiServer::HandleStatus_()
   {
      ServerStatus *status = ServerStatus::Instance();

      AnsiString version = Application::Instance()->GetVersionNumber();

      AnsiString body;
      body.Format("{\"version\":\"%hs\",\"state\":%d,\"processedMessages\":%d,\"spamMessages\":%d,\"virusesRemoved\":%d,"
                  "\"sessions\":{\"smtp\":%d,\"imap\":%d,\"pop3\":%d}}",
         JsonEscape_(version).c_str(),
         status->GetState(),
         status->GetNumberOfProcessedMessages(),
         status->GetNumberOfDetectedSpamMessages(),
         status->GetNumberOfRemovedViruses(),
         status->GetNumberOfSessions(STSMTP),
         status->GetNumberOfSessions(STIMAP),
         status->GetNumberOfSessions(STPOP3));

      // What this build of the server can do, settled when it was compiled, so
      // that a fixture layer - or an operator - can ask instead of finding out
      // from an error: whether there is a script engine for the event handlers
      // (Windows Active Scripting; no Linux build has one), whether the OpenSSL
      // it was built against has the Argon2id KDF (3.2 or later) and the hybrid
      // post-quantum key exchange groups (3.5 or later).
#ifdef HM_PLATFORM_POSIX
      const bool scriptEngine = false;
#else
      const bool scriptEngine = true;
#endif
      const bool argon2id = OPENSSL_VERSION_NUMBER >= 0x30200000L;
      const bool postQuantum = OPENSSL_VERSION_NUMBER >= 0x30500000L;

      AnsiString build;
      build.Format(",\"build\":{\"script_engine\":%hs,\"argon2id\":%hs,\"post_quantum_key_exchange\":%hs}}",
         scriptEngine ? "true" : "false",
         argon2id ? "true" : "false",
         postQuantum ? "true" : "false");

      body = body.Left(body.GetLength() - 1) + build;

      return BuildResponse_(200, body);
   }

   HttpResponse
   RestApiServer::HandleListDomains_(const std::vector<String> &allowedDomains)
   {
      Domains domains;
      domains.Refresh();

      AnsiString body = "[";

      // A separate count rather than the loop index, which is the difference
      // between valid and invalid JSON now that entries can be skipped: the
      // separator has to follow the last entry *emitted*, not the last index
      // visited. With `i > 0` a skipped first domain produced "[,{...}]".
      int count = 0;

      for (int i = 0; i < domains.GetCount(); i++)
      {
         std::shared_ptr<Domain> domain = domains.GetItem(i);
         if (!domain)
            continue;

         // A key restricted to named domains sees only those. Filtered rather
         // than refused, because a listing that answered 403 would be useless to
         // exactly the credential the restriction exists for - and a listing that
         // returned everything would tell a key issued for one customer the names
         // of all the others.
         if (!IsDomainAllowed_(allowedDomains, domain->GetName()))
            continue;

         if (count > 0)
            body += ",";

         body += DomainEntryJson_(domain);
         count++;
      }

      body += "]";

      return BuildResponse_(200, body);
   }

   HttpResponse
   RestApiServer::HandleCreateDomain_(const AnsiString &requestBody)
   {
      // Trimmed as InterfaceDomain::put_Name trims, and nothing else done to
      // it here. The name is judged by PreSaveLimitationsCheck inside
      // PersistentDomain::SaveObject, exactly as a domain saved from the
      // Control Panel or over COM is judged: the same IsValidDomainName, the
      // same refusal of a name a domain alias already answers to.
      String name = JsonUtf8Value_(requestBody, "name");
      name.Trim();

      if (name.IsEmpty())
         return BuildResponse_(400, "{\"error\":\"name is required\"}");

      // A duplicate is refused before the save rather than by it, so that it
      // is a 409 and not a 400 carrying the limitation check's sentence: to a
      // caller the two are different answers, and one of them means "already
      // done".
      Domains domains;
      domains.Refresh();

      if (domains.GetItemByName(name))
         return BuildResponse_(409, "{\"error\":\"domain already exists\"}");

      // A fresh Domain, as InterfaceDomains::Add makes one, so that every
      // default the Control Panel would leave in place - the plus-addressing
      // character, the signature settings, the limits switched off - is the
      // same here.
      std::shared_ptr<Domain> domain = std::shared_ptr<Domain>(new Domain());
      domain->SetName(name);
      domain->SetIsActive(GetJsonBoolValue_(requestBody, "active", true));
      domain->SetPostmaster(JsonUtf8Value_(requestBody, "postmaster"));

      String saveError;

      if (!PersistentDomain::SaveObject(domain, saveError, PersistenceModeNormal))
      {
         // As for an account: a message is the limitation check explaining the
         // caller's own mistake - a name that is not a domain name, a name a
         // domain alias already has - in fixed sentences written for an
         // administrator, which is what makes passing them through safe. No
         // message is a failed INSERT, and that one is ours.
         if (!saveError.IsEmpty())
         {
            LOG_APPLICATION("RestApi: Refused to create domain " + name + ": " + saveError);

            AnsiString body;
            body.Format("{\"error\":\"%hs\"}", JsonEscape_(Utf8_(saveError)).c_str());

            return BuildResponse_(400, body);
         }

         return BuildResponse_(500, "{\"error\":\"failed to save domain\"}");
      }

      // SaveObject has already dropped the name from the domain cache, so the
      // next lookup - a listing, or SMTP deciding whether a recipient is
      // local - reads the row that was just written.
      LOG_APPLICATION("RestApi: Domain " + name + " created.");

      return BuildResponse_(201, DomainEntryJson_(domain));
   }

   HttpResponse
   RestApiServer::HandleDeleteDomain_(const String &domainName)
   {
      Domains domains;
      domains.Refresh();

      std::shared_ptr<Domain> domain = domains.GetItemByName(domainName);
      if (!domain)
         return BuildResponse_(404, "{\"error\":\"domain not found\"}");

      // Through the collection, as InterfaceDomain::Delete goes through its
      // parent collection. Collection::DeleteItemByDBID calls
      // PersistentDomain::DeleteObject, which deletes the accounts with their
      // messages, the aliases, the distribution lists and the domain aliases,
      // rewrites whatever another domain pointed at any of those names, drops
      // the domain from the cache and removes its data and Sieve directories -
      // and reports a refused delete instead of dropping the object anyway.
      if (!domains.DeleteItemByDBID(domain->GetID()))
         return BuildResponse_(500, "{\"error\":\"failed to delete domain\"}");

      LOG_APPLICATION("RestApi: Domain " + domain->GetName() + " deleted.");

      return BuildResponse_(200, "{\"deleted\":true}");
   }

   HttpResponse
   RestApiServer::HandleListAccounts_(const String &domainName)
   {
      Domains domains;
      domains.Refresh();

      std::shared_ptr<Domain> domain = domains.GetItemByName(domainName);
      if (!domain)
         return BuildResponse_(404, "{\"error\":\"domain not found\"}");

      Accounts accounts(domain->GetID());
      accounts.Refresh();

      AnsiString body = "[";

      // As in HandleListDomains_: counted, not indexed, so that a skipped entry
      // cannot put a separator where there is nothing to separate.
      int count = 0;

      for (int i = 0; i < accounts.GetCount(); i++)
      {
         std::shared_ptr<Account> account = accounts.GetItem(i);
         if (!account)
            continue;

         if (count > 0)
            body += ",";

         // address and active first, as the listing has always begun, then
         // the four fields a spreadsheet export writes. Before, the entry was
         // those two alone, so an export of a real server had empty name, size
         // and level columns.
         body += AccountListEntryJson_(account);
         count++;
      }

      body += "]";

      return BuildResponse_(200, body);
   }

   HttpResponse
   RestApiServer::HandleCreateAccount_(const Caller &caller, const String &domainName, const AnsiString &requestBody)
   {
      // The body is read as a document in ReadNewAccount_ (RestApiRoutes.cpp,
      // beside the update): address and password, active, first_name,
      // last_name, max_size_mb and admin_level, each of its type, and any other
      // field refused by name. Before, this searched the text for five keys,
      // so a spreadsheet import's name and admin_level columns were dropped
      // without a word and a password of digits alone - sent as a JSON number
      // - was reported as missing.
      std::shared_ptr<Account> account = std::shared_ptr<Account>(new Account());
      String password;
      HttpResponse refusal;

      if (!ReadNewAccount_(caller, requestBody, account, password, refusal))
         return refusal;

      const String address = account->GetAddress();

      String addressDomain = StringParser::ExtractDomain(address);
      if (addressDomain.CompareNoCase(domainName) != 0)
         return BuildResponse_(400, "{\"error\":\"address does not belong to the domain\"}");

      Domains domains;
      domains.Refresh();

      std::shared_ptr<Domain> domain = domains.GetItemByName(domainName);
      if (!domain)
         return BuildResponse_(404, "{\"error\":\"domain not found\"}");

      // Reject duplicates.
      std::shared_ptr<Account> existingAccount = std::shared_ptr<Account>(new Account());
      if (PersistentAccount::ReadObject(existingAccount, address) && existingAccount->GetID() > 0)
         return BuildResponse_(409, "{\"error\":\"account already exists\"}");

      // A password chosen at creation is a password chosen: the policy and the
      // breach check apply as they do in InterfaceAccount::put_Password, which
      // the Control Panel's new account goes through, and in the update. The
      // create applied neither, so a spreadsheet import - which makes domain and
      // server administrators too - walked round a policy every other door
      // enforces. After the duplicate check, so an import run twice still hears
      // 409 for the rows it made the first time; the breach check after the
      // policy, so a password the policy refuses costs no request.
      {
         String policyFailure;
         if (!PasswordPolicy::IsAcceptable(address, password, policyFailure))
         {
            LOG_APPLICATION("RestApi: Refused a password for new account " + address + ": " + policyFailure);
            AnsiString body;
            body.Format("{\"error\":\"%hs\"}", JsonEscape_(Utf8_(policyFailure)).c_str());
            return BuildResponse_(400, body);
         }

         String breachRefusal;
         AnsiString breachReason;
         int breachStatus = 0;
         if (!PasswordBreachCheck::IsAcceptable(address, password, breachRefusal, breachReason, breachStatus))
         {
            AnsiString body;
            body.Format("{\"error\":\"%hs\",\"reason\":\"%hs\"}", JsonEscape_(Utf8_(breachRefusal)).c_str(), breachReason.c_str());
            return BuildResponse_(breachStatus, body);
         }
      }

      // The password is the UTF-8 the body carried, decoded as the update
      // decodes it; the conversion used before read those bytes in the
      // machine's ANSI code page, so a password with an accented letter made
      // here was not the password the Control Panel or the update would set.
      int preferredHashAlgorithm = IniFileSettings::Instance()->GetPreferredHashAlgorithm();
      String hashedPassword = Crypt::Instance()->EnCrypt(password, (Crypt::EncryptionType) preferredHashAlgorithm);

      account->SetDomainID(domain->GetID());
      account->SetPassword(hashedPassword);
      account->SetPasswordEncryption(preferredHashAlgorithm);

      // createInbox: true. This argument is the whole reason this call changed,
      // and it was losing mail.
      //
      // PersistentAccount::SaveObject(account) - the one-argument overload used
      // here before - forwards createInbox as *false*, so the hm_accounts row was
      // written and no INBOX row in hm_imapfolders was. Every COM caller passes
      // true (InterfaceAccount::Save), so an account made in hMailAdmin or the
      // Control Panel got one and an account made over the REST API did not.
      //
      // What that costs, following the delivery path for a message addressed to
      // such an account: SMTP accepts it at RCPT TO because the account exists,
      // then LocalDelivery::CreateAccountLevelMessage_ asks
      // InboxIDCache::GetUserInboxFolder for the folder to put it in.
      // PersistentIMAPFolder::GetUserInboxFolder selects folderid where
      // foldername = 'INBOX', finds no row, and returns 0; the delivery gives up
      // and returns an empty message. The caller reports HM5209 and returns
      // without adding anything to the bounce list - so the sender is told the
      // message was accepted, the recipient never sees it, and no non-delivery
      // report is generated. The message is simply gone.
      //
      // Worse, InboxIDCache caches the zero, so creating the inbox afterwards
      // does not fix delivery until the cache is cleared.
      //
      // The error message is also captured now. SaveObject already ran
      // PreSaveLimitationsCheck through this path and the old code discarded
      // everything it said, answering 500 "failed to save account" for what are
      // almost always the caller's own doing.
      String saveError;

      if (!PersistentAccount::SaveObject(account, saveError, true, PersistenceModeNormal))
      {
         // Two different failures, told apart by whether anything explained it.
         //
         // A non-empty message comes from PreSaveLimitationsCheck: the address is
         // not a valid mailbox address, the domain has reached its maximum number
         // of accounts, the domain has a maximum account size and this account has
         // none. Those are all the caller's problem, so 400 and say which - an
         // administrator who was told only "failed to save account", with a 500,
         // could not tell a configured limit from a broken database. Every one of
         // those strings is a fixed sentence written for an administrator; none of
         // them carries a path, a query, a row or another account's name, which is
         // what makes passing it through safe.
         //
         // An empty message means the INSERT itself failed. That one is ours: 500,
         // and deliberately without detail.
         if (!saveError.IsEmpty())
         {
            LOG_APPLICATION("RestApi: Refused to create account " + address + ": " + saveError);

            AnsiString body;
            body.Format("{\"error\":\"%hs\"}", JsonEscape_(Utf8_(saveError)).c_str());

            return BuildResponse_(400, body);
         }

         return BuildResponse_(500, "{\"error\":\"failed to save account\"}");
      }

      // Confirm the account is really there before reporting that it was made.
      //
      // Not belt and braces: PersistentAccount::SaveObject returns the result of
      // the INSERT, and when the inbox it now creates cannot be created it
      // deletes the row it has just written and *still* returns true. So without
      // this the endpoint could answer 201 for a mailbox that no longer exists -
      // the same "reported a success that did not happen" that the queue
      // endpoints were fixed for. The account object keeps its id after the
      // delete, so the question has to be put to the database.
      std::shared_ptr<Account> savedAccount = std::shared_ptr<Account>(new Account());

      if (!PersistentAccount::ReadObject(savedAccount, address) || savedAccount->GetID() == 0)
      {
         LOG_APPLICATION("RestApi: Account " + address +
            " could not be created - it was not present after being saved, which happens when its inbox could not be created.");

         return BuildResponse_(500, "{\"error\":\"failed to save account\"}");
      }

      LOG_APPLICATION("RestApi: Account " + address + " created.");

      AnsiString body;
      body.Format("{\"address\":\"%hs\",\"created\":true}", JsonEscape_(Utf8_(address)).c_str());

      return BuildResponse_(201, body);
   }

   HttpResponse
   RestApiServer::HandleDeleteAccount_(const String &address)
   {
      std::shared_ptr<Account> account = std::shared_ptr<Account>(new Account());

      if (!PersistentAccount::ReadObject(account, address) || account->GetID() == 0)
         return BuildResponse_(404, "{\"error\":\"account not found\"}");

      if (!PersistentAccount::DeleteObject(account))
         return BuildResponse_(500, "{\"error\":\"failed to delete account\"}");

      LOG_APPLICATION("RestApi: Account " + address + " deleted.");

      return BuildResponse_(200, "{\"deleted\":true}");
   }

   HttpResponse
   RestApiServer::HandleListQueue_()
   {
      // Reuses the same query that backs the COM Status.UndeliveredMessages
      // property: tab-separated columns id, created, from, recipients,
      // next try, file name, locked, tries.
      AnsiString queueData = ServerStatus::Instance()->GetUnsortedMessageStatus();

      std::vector<AnsiString> lines = StringParser::SplitString(queueData, "\r\n");

      AnsiString items;
      int count = 0;

      for (const AnsiString &line : lines)
      {
         if (line.IsEmpty())
            continue;

         std::vector<AnsiString> columns = StringParser::SplitString(line, "\t");
         if (columns.size() < 8)
            continue;

         AnsiString item;
         item.Format("{\"id\":%hs,\"created\":\"%hs\",\"from\":\"%hs\",\"recipients\":\"%hs\",\"next_try\":\"%hs\",\"locked\":%hs,\"tries\":%hs}",
            columns[0].c_str(),
            JsonEscape_(columns[1]).c_str(),
            JsonEscape_(columns[2]).c_str(),
            JsonEscape_(columns[3]).c_str(),
            JsonEscape_(columns[4]).c_str(),
            columns[6] == "1" ? "true" : "false",
            columns[7].c_str());

         if (count > 0)
            items += ",";

         items += item;
         count++;
      }

      AnsiString body;
      body.Format("{\"count\":%d,\"messages\":[%hs]}", count, items.c_str());

      return BuildResponse_(200, body);
   }

   bool
   RestApiServer::QueueMessageExists_(__int64 messageId)
   {
      // Both DeliveryQueue::ResetDeliveryTime and DeliveryQueue::Remove
      // return void and do nothing at all for an id that is not there
      // (PersistentMessage::SetNextTryTime reports success for an UPDATE
      // that matched no rows), so without this check the retry and delete
      // endpoints answered 200 {"retried":true} for any well-formed
      // number - including one an administrator mistyped, which then read
      // as "the message was requeued" when nothing had happened.
      //
      // Only delivery-queue rows count. A delivered message
      // (Message::Delivered, messagetype 2) lives in a mailbox and is not
      // what GET /api/v1/queue lists; accepting one here would let
      // DELETE /api/v1/queue/<id> destroy delivered mail.
      std::shared_ptr<Message> message = std::shared_ptr<Message>(new Message());

      if (!PersistentMessage::ReadObject(message, messageId))
         return false;

      int messageType = static_cast<int>(message->GetState());

      return messageType == static_cast<int>(Message::Delivering) ||
             messageType == EtrnHeldMessageType;
   }

   HttpResponse
   RestApiServer::HandleQueueRetry_(__int64 messageId)
   {
      if (!QueueMessageExists_(messageId))
         return BuildResponse_(404, "{\"error\":\"queue message not found\"}");

      DeliveryQueue::ResetDeliveryTime(messageId);
      DeliveryQueue::StartDelivery();

      LOG_APPLICATION("RestApi: Queue message " + StringParser::IntToString(messageId) + " scheduled for immediate delivery.");

      return BuildResponse_(200, "{\"retried\":true}");
   }

   HttpResponse
   RestApiServer::HandleQueueDelete_(__int64 messageId)
   {
      if (!QueueMessageExists_(messageId))
         return BuildResponse_(404, "{\"error\":\"queue message not found\"}");

      DeliveryQueue::Remove(messageId);

      LOG_APPLICATION("RestApi: Queue message " + StringParser::IntToString(messageId) + " removed from the delivery queue.");

      return BuildResponse_(200, "{\"deleted\":true}");
   }

   HttpResponse
   RestApiServer::HandleListQuarantine_()
   {
      // The same store call the administration surface uses, bounded the same
      // way, so what the API reports and what a reviewer sees cannot disagree.
      const std::vector<QuarantinedMessage> messages = QuarantineStore::List(1000);

      AnsiString body = "[";
      int count = 0;

      for (const QuarantinedMessage &message : messages)
      {
         if (count > 0)
            body += ",";

         AnsiString entry;
         entry.Format("{\"id\":%I64d,\"sender\":\"%hs\",\"recipients\":\"%hs\",\"subject\":\"%hs\",\"reason\":\"%hs\",\"score\":%d,\"size\":%d,\"created\":\"%hs\"}",
            message.id,
            JsonEscape_(Utf8_(message.sender)).c_str(),
            JsonEscape_(Utf8_(message.recipients)).c_str(),
            JsonEscape_(Utf8_(message.subject)).c_str(),
            JsonEscape_(Utf8_(message.reason)).c_str(),
            message.score,
            message.size,
            JsonEscape_(Utf8_(message.created)).c_str());

         body += entry;
         count++;
      }

      body += "]";

      return BuildResponse_(200, body);
   }

   HttpResponse
   RestApiServer::HandleQuarantineRelease_(__int64 id)
   {
      // Existence checked first, so an unknown id is a 404 rather than a 500
      // with somebody else's wording - the queue routes learned this the
      // expensive way.
      QuarantinedMessage message;
      if (!QuarantineStore::GetById(id, message))
         return BuildResponse_(404, "{\"error\":\"quarantined message not found\"}");

      String error;
      if (!QuarantineStore::Release(id, error))
      {
         AnsiString body;
         body.Format("{\"error\":\"%hs\"}", JsonEscape_(Utf8_(error)).c_str());
         return BuildResponse_(500, body);
      }

      return BuildResponse_(200, "{\"released\":true}");
   }

   HttpResponse
   RestApiServer::HandleQuarantineDelete_(__int64 id)
   {
      QuarantinedMessage message;
      if (!QuarantineStore::GetById(id, message))
         return BuildResponse_(404, "{\"error\":\"quarantined message not found\"}");

      if (!QuarantineStore::Delete(id))
         return BuildResponse_(500, "{\"error\":\"the quarantined message could not be deleted\"}");

      return BuildResponse_(200, "{\"deleted\":true}");
   }

   HttpResponse
   RestApiServer::HandleListAliases_(const String &domainName)
   {
      Domains domains;
      domains.Refresh();

      std::shared_ptr<Domain> domain = domains.GetItemByName(domainName);
      if (!domain)
         return BuildResponse_(404, "{\"error\":\"domain not found\"}");

      std::shared_ptr<Aliases> aliases = domain->GetAliases();
      if (!aliases)
         return BuildResponse_(200, "[]");

      aliases->Refresh();

      AnsiString body = "[";
      int count = 0;

      for (int i = 0; i < aliases->GetCount(); i++)
      {
         std::shared_ptr<Alias> alias = aliases->GetItem(i);
         if (!alias)
            continue;

         if (count > 0)
            body += ",";

         AnsiString entry;
         entry.Format("{\"name\":\"%hs\",\"value\":\"%hs\",\"active\":%hs}",
            JsonEscape_(Utf8_(alias->GetName())).c_str(),
            JsonEscape_(Utf8_(alias->GetValue())).c_str(),
            alias->GetIsActive() ? "true" : "false");

         body += entry;
         count++;
      }

      body += "]";

      return BuildResponse_(200, body);
   }

   // ------------------------------------------------------------------------
   // Wave 88: the surfaces that were COM-only. Same shape as the routes above:
   // a 404 names what was not found, a 400 names what was wrong with the body,
   // every string goes through JsonEscape_, and nothing here is reachable
   // except through the dispatch in ProcessRequest_ after Authorize_.

   HttpResponse
   RestApiServer::HandleListIpRanges_()
   {
      SecurityRanges ranges;
      ranges.Refresh();

      AnsiString body = "[";
      int count = 0;
      for (int i = 0; i < ranges.GetCount(); i++)
      {
         std::shared_ptr<SecurityRange> range = ranges.GetItem(i);
         if (!range)
            continue;

         if (count > 0)
            body += ",";

         AnsiString entry = IpRangeEntryJson_(range);
         body += entry;
         count++;
      }
      body += "]";
      return BuildResponse_(200, body);
   }

   HttpResponse
   RestApiServer::HandleCreateIpRange_(const AnsiString &requestBody)
   {
      AnsiString name = GetJsonStringValue_(requestBody, "name");
      AnsiString lowerText = GetJsonStringValue_(requestBody, "lower");
      AnsiString upperText = GetJsonStringValue_(requestBody, "upper");
      if (name.IsEmpty() || lowerText.IsEmpty() || upperText.IsEmpty())
         return BuildResponse_(400, "{\"error\":\"name, lower and upper are required\"}");

      IPAddress lower;
      IPAddress upper;
      // The two-argument form: a caller's typo is answered with a 400, not
      // written to the ERROR log by the parser as the one-argument form does.
      if (!lower.TryParse(lowerText, false) || !upper.TryParse(upperText, false))
         return BuildResponse_(400, "{\"error\":\"lower and upper must be IP addresses\"}");

      long priority = 0;
      {
         // A number in the body is not a string, so read it the way the
         // integer routes do: everything after the key up to the next separator.
         AnsiString needle = "\"priority\"";
         int keyPosition = requestBody.Find(needle);
         if (keyPosition >= 0)
         {
            int colon = requestBody.Find(":", keyPosition + needle.GetLength());
            if (colon >= 0)
            {
               AnsiString digits;
               for (int i = colon + 1; i < requestBody.GetLength(); i++)
               {
                  char c = requestBody[i];
                  if (c == ' ' || c == '\t')
                     continue;
                  if ((c >= '0' && c <= '9') || (c == '-' && digits.IsEmpty()))
                  {
                     digits += c;
                     continue;
                  }
                  break;
               }
               if (!digits.IsEmpty())
                  priority = atol(digits.c_str());
            }
         }
      }

      std::shared_ptr<SecurityRange> range(new SecurityRange);
      range->SetName(String(name));
      range->SetLowerIP(lower);
      range->SetUpperIP(upper);
      range->SetPriority(priority);
      range->SetAllowSMTP(GetJsonBoolValue_(requestBody, "allow_smtp", true));
      range->SetAllowIMAP(GetJsonBoolValue_(requestBody, "allow_imap", true));
      range->SetAllowPOP3(GetJsonBoolValue_(requestBody, "allow_pop3", true));
      range->SetAllowRelayL2L(GetJsonBoolValue_(requestBody, "deliver_local_to_local", true));
      range->SetAllowRelayL2R(GetJsonBoolValue_(requestBody, "deliver_local_to_remote", false));
      range->SetAllowRelayR2L(GetJsonBoolValue_(requestBody, "deliver_remote_to_local", true));
      range->SetAllowRelayR2R(GetJsonBoolValue_(requestBody, "deliver_remote_to_remote", false));
      range->SetRequireSMTPAuthLocalToLocal(GetJsonBoolValue_(requestBody, "require_auth_local_to_local", false));
      range->SetRequireSMTPAuthLocalToExternal(GetJsonBoolValue_(requestBody, "require_auth_local_to_remote", true));
      range->SetRequireSMTPAuthExternalToLocal(GetJsonBoolValue_(requestBody, "require_auth_remote_to_local", false));
      range->SetRequireSMTPAuthExternalToExternal(GetJsonBoolValue_(requestBody, "require_auth_remote_to_remote", true));
      range->SetRequireTLSForAuth(GetJsonBoolValue_(requestBody, "require_tls_for_auth", false));
      range->SetSpamProtection(GetJsonBoolValue_(requestBody, "spam_protection", true));
      range->SetVirusProtection(GetJsonBoolValue_(requestBody, "virus_protection", true));

      // The expiry the auto-ban gives its ranges and the desktop dialog
      // offers: the flag, and a time on the server's clock that goes with it.
      bool expires = GetJsonBoolValue_(requestBody, "expires", false);
      String expiresText = String(GetJsonStringValue_(requestBody, "expires_time"));
      expiresText.Trim();
      DateTime expiresTime;
      if (!expiresText.IsEmpty())
      {
         if (!expires)
            return BuildResponse_(400, "{\"error\":\"expires_time is taken only when expires is true\"}");
         if (!ParseExpiryTime_(expiresText, expiresTime))
            return BuildResponse_(400, "{\"error\":\"expires_time must be a date and time as YYYY-MM-DD HH:MM:SS\"}");
      }
      else if (expires)
         return BuildResponse_(400, "{\"error\":\"expires_time is required when a range expires\"}");
      range->SetExpires(expires);
      if (expires)
         range->SetExpiresTime(expiresTime);

      String result;
      if (!PersistentSecurityRange::SaveObject(range, result, PersistenceModeNormal))
      {
         AnsiString error;
         error.Format("{\"error\":\"%hs\"}", JsonEscape_(Utf8_(result)).c_str());
         return BuildResponse_(400, error);
      }

      LOG_APPLICATION("RestApi: IP range '" + range->GetName() + "' created.");

      AnsiString body;
      body.Format("{\"id\":%I64d}", range->GetID());
      return BuildResponse_(201, body);
   }

   HttpResponse
   RestApiServer::HandleDeleteIpRange_(__int64 rangeId)
   {
      SecurityRanges ranges;
      ranges.Refresh();
      std::shared_ptr<SecurityRange> range = ranges.GetItemByDBID(rangeId);
      if (!range)
         return BuildResponse_(404, "{\"error\":\"ip range not found\"}");

      String name = range->GetName();
      if (!ranges.DeleteItemByDBID(rangeId))
         return BuildResponse_(500, "{\"error\":\"the range could not be deleted\"}");

      LOG_APPLICATION("RestApi: IP range '" + name + "' deleted.");
      return BuildResponse_(200, "{\"deleted\":true}");
   }

   HttpResponse
   RestApiServer::HandleListLists_(const String &domainName)
   {
      Domains domains;
      domains.Refresh();
      std::shared_ptr<Domain> domain = domains.GetItemByName(domainName);
      if (!domain)
         return BuildResponse_(404, "{\"error\":\"domain not found\"}");

      DistributionLists lists(domain->GetID());
      lists.Refresh();

      AnsiString body = "[";
      int count = 0;
      for (int i = 0; i < lists.GetCount(); i++)
      {
         std::shared_ptr<DistributionList> list = lists.GetItem(i);
         if (!list)
            continue;

         if (count > 0)
            body += ",";

         // The same entry the create and the update answer with
         // (RestApiAdministration.cpp), members included.
         body += ListEntryJson_(list);
         count++;
      }
      body += "]";
      return BuildResponse_(200, body);
   }

   HttpResponse
   RestApiServer::HandleDeleteList_(const String &address)
   {
      String domainName = StringParser::ExtractDomain(address);
      Domains domains;
      domains.Refresh();
      std::shared_ptr<Domain> domain = domains.GetItemByName(domainName);
      if (!domain)
         return BuildResponse_(404, "{\"error\":\"list not found\"}");

      DistributionLists lists(domain->GetID());
      lists.Refresh();
      std::shared_ptr<DistributionList> list = lists.GetItemByAddress(address);
      if (!list)
         return BuildResponse_(404, "{\"error\":\"list not found\"}");

      if (!lists.DeleteItemByDBID(list->GetID()))
         return BuildResponse_(500, "{\"error\":\"the list could not be deleted\"}");

      LOG_APPLICATION("RestApi: Distribution list '" + address + "' deleted.");
      return BuildResponse_(200, "{\"deleted\":true}");
   }

   HttpResponse
   RestApiServer::HandleListCertificates_()
   {
      std::shared_ptr<SSLCertificates> certificates = Configuration::Instance()->GetSSLCertificates();
      if (!certificates)
         return BuildResponse_(200, "[]");
      certificates->Refresh();

      AnsiString body = "[";
      int count = 0;
      for (int i = 0; i < certificates->GetCount(); i++)
      {
         std::shared_ptr<SSLCertificate> certificate = certificates->GetItem(i);
         if (!certificate)
            continue;
         if (count > 0)
            body += ",";
         // The private key password is deliberately not here: a listing is for
         // finding out what the server has, not for lifting what unlocks it.
         AnsiString entry;
         entry.Format("{\"id\":%I64d,\"name\":\"%hs\",\"certificate_file\":\"%hs\",\"private_key_file\":\"%hs\"}",
            certificate->GetID(),
            JsonEscape_(Utf8_(certificate->GetName())).c_str(),
            JsonEscape_(Utf8_(certificate->GetCertificateFile())).c_str(),
            JsonEscape_(Utf8_(certificate->GetPrivateKeyFile())).c_str());
         body += entry;
         count++;
      }
      body += "]";
      return BuildResponse_(200, body);
   }

   HttpResponse
   RestApiServer::HandleDkim_(const String &domainName)
   {
      Domains domains;
      domains.Refresh();
      std::shared_ptr<Domain> domain = domains.GetItemByName(domainName);
      if (!domain)
         return BuildResponse_(404, "{\"error\":\"domain not found\"}");

      AnsiString body;
      body.Format("{\"domain\":\"%hs\",\"enabled\":%hs,\"selector\":\"%hs\",\"sign_aliases\":%hs,\"private_key_file\":\"%hs\"}",
         JsonEscape_(Utf8_(domain->GetName())).c_str(),
         domain->GetDKIMEnabled() ? "true" : "false",
         JsonEscape_(domain->GetDKIMSelector()).c_str(),
         domain->GetDKIMAliasesEnabled() ? "true" : "false",
         JsonEscape_(Utf8_(domain->GetDKIMPrivateKeyFile())).c_str());
      return BuildResponse_(200, body);
   }

   namespace
   {
      const char *RuleFieldName(RuleCriteria::PredefinedField field)
      {
         switch (field)
         {
         case RuleCriteria::FTFrom: return "from";
         case RuleCriteria::FTTo: return "to";
         case RuleCriteria::FTCC: return "cc";
         case RuleCriteria::FTSubject: return "subject";
         case RuleCriteria::FTBody: return "body";
         case RuleCriteria::FTMessageSize: return "message_size";
         case RuleCriteria::FTRecipientList: return "recipient_list";
         case RuleCriteria::FTDeliveryAttempts: return "delivery_attempts";
         default: return "unknown";
         }
      }

      const char *RuleMatchName(RuleCriteria::MatchType match)
      {
         switch (match)
         {
         case RuleCriteria::Equals: return "equals";
         case RuleCriteria::Contains: return "contains";
         case RuleCriteria::LessThan: return "less_than";
         case RuleCriteria::GreaterThan: return "greater_than";
         case RuleCriteria::MatchesRegEx: return "regex";
         case RuleCriteria::NotContains: return "not_contains";
         case RuleCriteria::NotEquals: return "not_equals";
         case RuleCriteria::Wildcard: return "wildcard";
         default: return "none";
         }
      }

      const char *RuleActionName(RuleAction::Type type)
      {
         switch (type)
         {
         case RuleAction::Delete: return "delete";
         case RuleAction::Forward: return "forward";
         case RuleAction::Reply: return "reply";
         case RuleAction::MoveToIMAPFolder: return "move_to_folder";
         case RuleAction::ScriptFunction: return "script_function";
         case RuleAction::StopRuleProcessing: return "stop";
         case RuleAction::SetHeaderValue: return "set_header";
         case RuleAction::SendUsingRoute: return "send_using_route";
         case RuleAction::CreateCopy: return "copy";
         case RuleAction::BindToAddress: return "bind_to_address";
         default: return "unknown";
         }
      }
   }

   HttpResponse
   RestApiServer::HandleListRules_(__int64 accountId, __int64 domainId)
   {
      // The global rules, or one account's, from the cache the delivery path
      // itself reads, so the answer is what is in force and not a second
      // reading of the table.
      std::shared_ptr<Rules> rules = domainId > 0
         ? ObjectCache::Instance()->GetDomainRules(domainId)
         : accountId == 0
            ? ObjectCache::Instance()->GetGlobalRules()
            : ObjectCache::Instance()->GetAccountRules(accountId);
      if (!rules)
         return BuildResponse_(200, "[]");

      AnsiString body = "[";
      int count = 0;
      for (int i = 0; i < rules->GetCount(); i++)
      {
         std::shared_ptr<Rule> rule = rules->GetItem(i);
         if (!rule)
            continue;

         if (count > 0)
            body += ",";
         body += RuleEntryJson_(rule);
         count++;
      }
      body += "]";
      return BuildResponse_(200, body);
   }

   bool
   RestApiServer::IsSafeLogName_(const AnsiString &name)
   {
      // A log file name and nothing else: no separators, no dot-dot, no leading
      // dot, and the .log suffix the logger writes. The list route is the only
      // source of names a client should have, and this is what it hands out.
      if (name.IsEmpty() || name.GetLength() > 128)
         return false;
      if (!name.EndsWith(".log"))
         return false;
      if (name.Find("..") >= 0 || name[0] == '.')
         return false;
      for (int i = 0; i < name.GetLength(); i++)
      {
         char c = name[i];
         bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                   c == '_' || c == '-' || c == '.';
         if (!ok)
            return false;
      }
      return true;
   }

   HttpResponse
   RestApiServer::HandleListLogs_()
   {
      String directory = IniFileSettings::Instance()->GetLogDirectory();
      std::vector<FileInfo> files = FileUtilities::GetFilesInDirectory(directory, _T(".*\\.log"));

      AnsiString body = "[";
      int count = 0;
      for (FileInfo &file : files)
      {
         AnsiString name(file.GetName());
         if (!IsSafeLogName_(name))
            continue;
         String path = FileUtilities::Combine(directory, file.GetName());
         unsigned __int64 size = 0;
         FileUtilities::FileSize64(path, size);
         if (count > 0)
            body += ",";
         AnsiString entry;
         entry.Format("{\"name\":\"%hs\",\"size\":%I64u,\"created\":\"%hs\"}",
            JsonEscape_(name).c_str(),
            size,
            JsonEscape_(Utf8_(Time::GetTimeStampFromDateTime(file.GetCreateTime()))).c_str());
         body += entry;
         count++;
      }
      body += "]";
      return BuildResponse_(200, body);
   }

   HttpResponse
   RestApiServer::HandleLogTail_(const AnsiString &name, const AnsiString &query)
   {
      if (!IsSafeLogName_(name))
         return BuildResponse_(400, "{\"error\":\"not a log file name\"}");

      int lines = 200;
      AnsiString linesText = QueryParameter_(query, "lines");
      if (!linesText.IsEmpty())
      {
         lines = atoi(linesText.c_str());
         if (lines < 1)
            lines = 1;
         if (lines > 2000)
            lines = 2000;
      }

      String path = FileUtilities::Combine(IniFileSettings::Instance()->GetLogDirectory(), String(name));
      if (!FileUtilities::Exists(path))
         return BuildResponse_(404, "{\"error\":\"no such log file\"}");

      // Read the tail only: a busy day's log is hundreds of megabytes and the
      // request thread is the single REST worker. 512 KB holds 2000 lines of
      // anything this logger writes.
      const std::streamoff MaxBytes = 512 * 1024;
#ifdef HM_PLATFORM_POSIX
      // As above: a narrow path, because libstdc++ has no wide-path constructor.
      const AnsiString narrowPath = path.c_str();
      std::ifstream stream(narrowPath.c_str(), std::ios::binary);
#else
      std::ifstream stream(path.c_str(), std::ios::binary);
#endif
      if (!stream)
         return BuildResponse_(404, "{\"error\":\"no such log file\"}");
      // The backup log is written as UTF-16 with a byte-order mark; the others
      // are narrow. Decode by what the file says it is, not by its name.
      unsigned char bom[2] = { 0, 0 };
      stream.read(reinterpret_cast<char *>(bom), 2);
      bool utf16 = stream.gcount() == 2 && bom[0] == 0xFF && bom[1] == 0xFE;
      stream.clear();
      stream.seekg(0, std::ios::end);
      std::streamoff size = stream.tellg();
      std::streamoff start = size > MaxBytes ? size - MaxBytes : 0;
      if (utf16 && (start % 2) != 0)
         start++;   // stay on a code-unit boundary
      stream.seekg(start, std::ios::beg);
      std::string chunk;
      chunk.resize(static_cast<size_t>(size - start));
      if (!chunk.empty())
         stream.read(&chunk[0], chunk.size());

      std::string text;
      if (utf16)
      {
         size_t offset = start == 0 ? 2 : 0;
         if (chunk.size() > offset)
         {
            // Decoded rather than reinterpreted: the file is UTF-16LE on disk on
            // every platform, and a wchar_t is four bytes on Linux, so a copy of the
            // bytes would read every other character as a surrogate. The codec is
            // the one File::Write and ReadCompleteTextFile use.
            std::wstring wide = Unicode::FromUtf16Le(
               reinterpret_cast<const unsigned char *>(chunk.data()) + offset, chunk.size() - offset);
            AnsiString narrow(String(wide.c_str()));
            text = narrow.c_str();
         }
      }
      else
      {
         text.swap(chunk);
      }

      std::vector<std::string> all;
      size_t position = 0;
      while (position < text.size())
      {
         size_t end = text.find('\n', position);
         if (end == std::string::npos)
            end = text.size();
         std::string line = text.substr(position, end - position);
         if (!line.empty() && line[line.size() - 1] == '\r')
            line.erase(line.size() - 1);
         all.push_back(line);
         position = end + 1;
      }
      // A partial first line when the tail started mid-line, and a trailing
      // empty one from the final newline, are both noise.
      if (start > 0 && !all.empty())
         all.erase(all.begin());
      if (!all.empty() && all.back().empty())
         all.pop_back();

      size_t first = all.size() > static_cast<size_t>(lines) ? all.size() - lines : 0;
      AnsiString body;
      body.Format("{\"name\":\"%hs\",\"lines\":[", JsonEscape_(name).c_str());
      for (size_t i = first; i < all.size(); i++)
      {
         if (i > first)
            body += ",";
         body += "\"" + JsonEscape_(Utf8_(all[i].c_str())) + "\"";
      }
      body += "]}";
      return BuildResponse_(200, body);
   }

   HttpResponse
   RestApiServer::HandleBackupStart_()
   {
      std::shared_ptr<BackupManager> manager = Application::Instance()->GetBackupManager();
      if (!manager)
         return BuildResponse_(503, "{\"error\":\"the backup manager is not running\"}");

      if (!manager->StartBackup())
      {
         AnsiString body;
         body.Format("{\"error\":\"the backup did not start\",\"status\":\"%hs\"}",
            JsonEscape_(Utf8_(manager->GetStatus())).c_str());
         return BuildResponse_(409, body);
      }

      LOG_APPLICATION("RestApi: Backup started.");
      return BuildResponse_(202, "{\"started\":true}");
   }

   // How long POST /api/v1/backup/restore waits for 7za to read the archive's
   // index before it gives its worker thread back.
   //
   // Generous next to the work - the index is one small file - and deliberately
   // longer than the listener's own 30-second request timeout, so a caller still
   // holding the connection always gets the real answer rather than this one. It
   // exists for the case where the archive's path leads somewhere that has
   // stopped answering: there are four HTTP worker threads, and without a bound
   // four such requests are the whole API, for as long as the drive stays that
   // way. No other caller of LoadBackup has this problem or this bound.
   const unsigned int RESTORE_ARCHIVE_OPEN_TIMEOUT_MS = 120000;

   HttpResponse
   RestApiServer::HandleBackupRestore_(const AnsiString &requestBody)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Restores an archive, and until this existed there was no way to restore one
   // except the desktop Control Panel over COM - which is to say no way at all on
   // Linux, where the backup runs and COM does not. The Deck and hmctl are this
   // route.
   //
   // Everything that can be established is established HERE, while the request is
   // still being answered: the archive is opened (with the passphrase, for an
   // encrypted one), its version checked, and what was asked for checked against
   // what the archive holds. A refusal is then a 400 with the reason in the
   // administrator's hands, and nothing on the server has been touched.
   //
   // What must not happen is the alternative: queueing the restore and letting it
   // discover the problem on the maintenance thread, where the only way it can
   // report anything is OnBackupFailed - a critical entry in the error log and an
   // alert to whoever the backup-failed rule mails - for a request that was
   // simply wrong.
   //---------------------------------------------------------------------------()
   {
      std::shared_ptr<BackupManager> manager = Application::Instance()->GetBackupManager();
      if (!manager)
         return BuildResponse_(503, "{\"error\":\"the backup manager is not running\"}");

      auto refusal = [] (const AnsiString &reason)
      {
         AnsiString body;
         body.Format("{\"error\":\"%hs\"}", JsonEscape_(reason).c_str());
         return BuildResponse_(400, body);
      };

      JsonValue document;
      std::string parseError;
      if (!JsonValue::Parse(std::string(requestBody.c_str(), requestBody.GetLength()), document, parseError) || !document.IsObject())
         return refusal("the body must be a JSON object");

      String file;
      String passphrase;
      int restoreOptions = 0;

      const std::vector<std::pair<std::string, JsonValue> > &members = document.Members();

      for (size_t i = 0; i < members.size(); i++)
      {
         const std::string &key = members[i].first;
         const JsonValue &value = members[i].second;

         if (key == "file" || key == "passphrase")
         {
            if (!value.IsString())
               return refusal(AnsiString(key.c_str()) + " must be a string");

            String text;
            Unicode::MultiByteToWide(AnsiString(value.AsString().c_str()), text);

            if (key == "file")
               file = text;
            else
               passphrase = text;

            continue;
         }

         int part = 0;

         if (key == "restore_settings")
            part = Backup::BOSettings;
         else if (key == "restore_domains")
            part = Backup::BODomains;
         else if (key == "restore_messages")
            part = Backup::BOMessages;
         else
            return refusal(AnsiString(key.c_str()) + " is not a field of a restore: the fields are file, passphrase, restore_settings, restore_domains and restore_messages");

         if (!value.IsBool())
            return refusal(AnsiString(key.c_str()) + " must be true or false");

         if (value.AsBool())
            restoreOptions |= part;
      }

      if (file.IsEmpty())
         return refusal("file is required: the path of the backup archive on this server");

      // Opening it is also what proves the passphrase: an encrypted archive
      // cannot be read at all without the right one, so a restore that gets past
      // this line cannot fail later for want of it.
      // Bounded, unlike every other way of opening an archive, because of the
      // thread this is on rather than the work it is doing: opening the archive
      // runs 7za, and the Control Panel over COM and the backup's own
      // verification each do it on a thread whose only job is that one backup.
      // Here it is one of four HTTP workers. See RESTORE_ARCHIVE_OPEN_TIMEOUT_MS.
      String failureReason;
      bool openTimedOut = false;
      std::shared_ptr<Backup> backup = manager->LoadBackup(file, passphrase, failureReason,
         RESTORE_ARCHIVE_OPEN_TIMEOUT_MS, &openTimedOut);

      if (!backup)
      {
         // 504 and not 400: the request was not wrong, and nothing was learned
         // about the archive. A 400 would tell the administrator their backup is
         // bad on the strength of a slow disk.
         if (openTimedOut)
         {
            AnsiString timeout;
            timeout.Format("{\"error\":\"%hs\"}", JsonEscape_(Utf8_(failureReason)).c_str());
            return BuildResponse_(504, timeout);
         }

         return refusal(Utf8_(failureReason));
      }

      backup->SetRestoreOptions(restoreOptions);

      if (!BackupRestorer::CheckSelection(backup->GetContains(), restoreOptions, failureReason))
         return refusal(Utf8_(failureReason));

      const bool encrypted = Compression::GetArchiveEncryption(file) == Compression::EncryptionHeaders;

      if (!manager->StartRestore(backup))
      {
         AnsiString conflict;
         conflict.Format("{\"error\":\"the restore did not start\",\"status\":\"%hs\"}",
            JsonEscape_(Utf8_(manager->GetStatus())).c_str());
         return BuildResponse_(409, conflict);
      }

      // The archive's path, and never the passphrase.
      LOG_APPLICATION("RestApi: Restore started from " + file + ".");

      // And the audit trail, on this thread, while the actor ProcessRequest_
      // installed is still on it. Nothing the restore does afterwards can be
      // recorded: it runs on the maintenance work queue, which no actor is ever
      // installed on, so the deletes that empty every domain and the inserts that
      // put an archive's back reach the statement chokepoint with nobody to
      // attribute them to. What is recorded is therefore the request - who asked,
      // for which archive, for which parts - which is the half that says whose
      // decision this was. The same row, in the same words, as the COM path's; see
      // BackupManager::DescribeRestore.
      AuditTrail::Instance()->RecordEvent(_T("backup"), file, _T("restore started"),
         BackupManager::DescribeRestore(backup));

      AnsiString body;
      body.Format("{\"started\":true,\"encrypted\":%hs,\"contains\":{\"settings\":%hs,\"domains\":%hs,\"messages\":%hs}}",
         encrypted ? "true" : "false",
         backup->GetContains(Backup::BOSettings) ? "true" : "false",
         backup->GetContains(Backup::BODomains) ? "true" : "false",
         backup->GetContains(Backup::BOMessages) ? "true" : "false");

      return BuildResponse_(202, body);
   }

   HttpResponse
   RestApiServer::HandleBackupStatus_()
   {
      std::shared_ptr<BackupManager> manager = Application::Instance()->GetBackupManager();
      if (!manager)
         return BuildResponse_(503, "{\"error\":\"the backup manager is not running\"}");

      // The manager's status text holds the last failure reason; the progress
      // of a backup goes to the backup log, which is what the Control Panel and
      // the regression fixtures read. Both are here: status, and the last
      // twenty lines of hmailserver_backup.log, newest last.
      AnsiString body;
      body.Format("{\"status\":\"%hs\",\"log\":[", JsonEscape_(Utf8_(manager->GetStatus())).c_str());

      String logPath = FileUtilities::Combine(IniFileSettings::Instance()->GetLogDirectory(), _T("hmailserver_backup.log"));
      if (FileUtilities::Exists(logPath))
      {
         AnsiString text(FileUtilities::ReadCompleteTextFile(logPath));
         std::vector<AnsiString> lines;
         int position = 0;
         while (position < text.GetLength())
         {
            int end = text.Find("\n", position);
            if (end < 0)
               end = text.GetLength();
            AnsiString line = text.Mid(position, end - position);
            line.TrimRight("\r");
            if (!line.IsEmpty())
               lines.push_back(line);
            position = end + 1;
         }
         size_t first = lines.size() > 20 ? lines.size() - 20 : 0;
         for (size_t i = first; i < lines.size(); i++)
         {
            if (i > first)
               body += ",";
            body += "\"" + JsonEscape_(lines[i]) + "\"";
         }
      }
      body += "]}";
      return BuildResponse_(200, body);
   }

   HttpResponse
   RestApiServer::HandleSettings_()
   {
      // The group is the table in RestApiSettings.cpp: the same rows the PUT
      // applies and the OpenAPI document describes, so what a read shows is
      // exactly what a write answers with. The ten keys this snapshot has
      // always carried are still there under their names; the rest of
      // InterfaceSettings' scalars come after them. No passwords: the one the
      // group accepts (smtp_relayer_password) is write-only. The function is
      // declared in RestApiServer.h beside the class: a block-scope extern
      // here was placed in the global namespace by MSVC and in HM by GCC and
      // clang, and linked on one side only.
      return BuildResponse_(200, RestApiSettingsServerGroupJson(&RestApiServer::JsonEscape_));
   }

   HttpResponse
   RestApiServer::HandleServerReinitialize_()
   {
      // Application::Reinitialize stops every service - this listener among
      // them - reloads the configuration and starts them again, which is how
      // a port, a certificate binding or a TLS setting written over the API
      // takes effect. It cannot run on this thread: the answer has to leave
      // before the listener that carries it is stopped. So the answer goes
      // first, and a thread of its own does the work a moment later; a caller
      // polls GET /api/v1/status until it answers again. Reinitialize holds
      // its own mutex, so two callers restart twice, in turn, and never at
      // once.
      // One thread while one restart is pending: two hundred requests in one
      // rate window queued two hundred stop-and-start cycles behind the mutex.
      bool notPending = false;
      if (!reinitialize_pending_.compare_exchange_strong(notPending, true))
      {
         LOG_APPLICATION("RestApi: Reinitialize requested while one is pending; the pending one serves both.");
         return BuildResponse_(202, "{\"reinitializing\":true}");
      }
      LOG_APPLICATION("RestApi: Reinitialize requested; the services restart in a moment.");
      boost::thread(&ReinitializeAfterTheAnswer_).detach();
      return BuildResponse_(202, "{\"reinitializing\":true}");
   }

   std::atomic<bool> RestApiServer::reinitialize_pending_(false);

   void
   RestApiServer::ReinitializeAfterTheAnswer_()
   {
      boost::this_thread::sleep_for(boost::chrono::milliseconds(500));

      // Cleared before the restart, so a request that arrives during it
      // queues one more restart rather than none.
      reinitialize_pending_ = false;
      String error = Application::Instance()->Reinitialize();

      if (error.IsEmpty())
      {
         LOG_APPLICATION("RestApi: Reinitialize completed.");
      }
      else
      {
         LOG_APPLICATION("RestApi: Reinitialize failed: " + error);
      }
   }

   bool
   RestApiServer::GetJsonBoolValue_(const AnsiString &json, const AnsiString &key, bool defaultValue)
   {
      AnsiString needle = "\"" + key + "\"";
      int keyPosition = json.Find(needle);
      if (keyPosition < 0)
         return defaultValue;
      int colon = json.Find(":", keyPosition + needle.GetLength());
      if (colon < 0)
         return defaultValue;
      for (int i = colon + 1; i < json.GetLength(); i++)
      {
         char c = json[i];
         if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            continue;
         if (json.Mid(i, 4) == "true")
            return true;
         if (json.Mid(i, 5) == "false")
            return false;
         break;
      }
      return defaultValue;
   }

   std::vector<AnsiString>
   RestApiServer::GetJsonStringArray_(const AnsiString &json, const AnsiString &key)
   {
      // The strings of a flat array: "members":["a@x","b@x"]. Nothing nested,
      // which is all the bodies this server accepts contain.
      std::vector<AnsiString> values;
      AnsiString needle = "\"" + key + "\"";
      int keyPosition = json.Find(needle);
      if (keyPosition < 0)
         return values;
      int open = json.Find("[", keyPosition + needle.GetLength());
      if (open < 0)
         return values;
      int close = json.Find("]", open);
      if (close < 0)
         return values;

      AnsiString current;
      bool inString = false;
      for (int i = open + 1; i < close; i++)
      {
         char c = json[i];
         if (inString)
         {
            if (c == '\\' && i + 1 < close)
            {
               current += json[i + 1];
               i++;
               continue;
            }
            if (c == '\"')
            {
               values.push_back(current);
               current = "";
               inString = false;
               continue;
            }
            current += c;
            continue;
         }
         if (c == '\"')
            inString = true;
      }
      return values;
   }

   // ------------------------------------------------------------------------
   // The archive index (roadmap row 1024). Domain scope is decided here rather
   // than in Authorize_, because the domain is in the query or in the row:
   // a domain-restricted key searches one of its own domains and touches only
   // rows in them; the Inbound copies belong to no domain and are the
   // administrator's alone.

   HttpResponse
   RestApiServer::HandleArchiveSearch_(const std::vector<String> &domains, const AnsiString &query)
   {
      PersistentArchiveIndex::Criteria criteria;
      criteria.domain = String(QueryParameter_(query, "domain"));
      criteria.mailbox = String(QueryParameter_(query, "mailbox"));
      criteria.sender = String(QueryParameter_(query, "sender"));
      criteria.recipient = String(QueryParameter_(query, "recipient"));
      criteria.subject = String(QueryParameter_(query, "subject"));
      criteria.since = String(QueryParameter_(query, "since"));
      criteria.until = String(QueryParameter_(query, "until"));
      criteria.holdOnly = QueryParameter_(query, "hold") == "1";
      AnsiString limit = QueryParameter_(query, "limit");
      if (!limit.IsEmpty())
         criteria.maxRows = atoi(limit.c_str());

      if (!domains.empty())
      {
         if (criteria.domain.IsEmpty())
            return BuildForbiddenResponse_("this api key is restricted to named domains; name one of them in the domain parameter");
         if (!IsDomainAllowed_(domains, criteria.domain))
            return BuildForbiddenResponse_("this api key is not permitted for that domain");
      }

      std::vector<PersistentArchiveIndex::Entry> entries;
      if (!PersistentArchiveIndex::Search(criteria, entries))
         return BuildResponse_(500, "{\"error\":\"the archive index could not be read\"}");
      return BuildResponse_(200, PersistentArchiveIndex::ToJson(entries));
   }

   HttpResponse
   RestApiServer::HandleArchiveGet_(const std::vector<String> &domains, __int64 archiveId)
   {
      PersistentArchiveIndex::Entry entry;
      if (!PersistentArchiveIndex::Get(archiveId, entry))
         return BuildResponse_(404, "{\"error\":\"archive entry not found\"}");
      if (!domains.empty() && (entry.domain.IsEmpty() || !IsDomainAllowed_(domains, entry.domain)))
         return BuildForbiddenResponse_("this api key is not permitted for that domain");
      return BuildResponse_(200, PersistentArchiveIndex::ToJson(entry));
   }

   HttpResponse
   RestApiServer::HandleArchiveHold_(const std::vector<String> &domains, __int64 archiveId, bool hold)
   {
      PersistentArchiveIndex::Entry entry;
      if (!PersistentArchiveIndex::Get(archiveId, entry))
         return BuildResponse_(404, "{\"error\":\"archive entry not found\"}");
      if (!domains.empty() && (entry.domain.IsEmpty() || !IsDomainAllowed_(domains, entry.domain)))
         return BuildForbiddenResponse_("this api key is not permitted for that domain");
      if (!PersistentArchiveIndex::SetHold(archiveId, hold))
         return BuildResponse_(500, "{\"error\":\"the hold could not be changed\"}");

      LOG_APPLICATION("RestApi: Archive entry " + StringParser::IntToString((int) archiveId) + (hold ? " placed on hold." : " released from hold."));
      return BuildResponse_(200, hold ? "{\"hold\":true}" : "{\"hold\":false}");
   }

   bool
   RestApiServer::AuthenticateAccount_(const String &username, const String &password, const IPAddress &peer_address,
                                       const AnsiString &request, Caller &caller)
   {
      // A recovery code in place of the second factor, for somebody whose
      // authenticator is gone. It is handed to Logon rather than decided here,
      // and that is the whole of what keeps it honest: everything Logon puts in
      // front of a password verification - the per-name lockout, the domain's
      // country policy - is therefore in front of the code too, and the
      // account's own password is compared exactly once instead of once here
      // and once again inside. See PasswordValidator::SecondFactorOffer, which
      // is where the ordering and the cost model are written down.
      //
      // Only an account that HAS a second factor can use one: for any other the
      // header does nothing and nothing is spent. Matched in there and SPENT
      // below, once Logon has accepted the whole attempt, so that a failure
      // after this point burns nothing; a wrong code leaves the account password
      // refused exactly as it is refused with no code at all, and that refusal
      // is counted by Logon like any other.
      SecondFactorOffer secondFactor;
      Unicode::MultiByteToWide(GetHeader_(request, "x-hmailserver-recovery-code"), secondFactor.recovery_code);

      // The same path IMAP and POP3 take, and for the same reason those two
      // share it: every password scheme the server stores, directory-linked
      // accounts, app passwords, the per-name lockout, the logon failure count
      // that feeds the auto-ban and the last-logon stamp all live behind
      // AccountLogon::Logon. A second implementation here would be a second
      // place for them to drift apart.
      bool disconnect = false;
      bool viaAppPassword = false;
      String appPasswordName;
      std::shared_ptr<const Account> account = AccountLogon().Logon(peer_address, username, password, disconnect,
                                                                   &viaAppPassword, &appPasswordName, &secondFactor);

      if (!account)
      {
         LOG_APPLICATION("REST API: account authentication failed for " + username + " from " + String(peer_address.ToString()) + ".");
         return false;
      }

      if (!account->GetActive())
      {
         LOG_APPLICATION("REST API: account " + username + " authenticated but is inactive; refused.");
         return false;
      }

      caller.result = AuthenticatedAsAccount;
      caller.read_only = false;
      caller.identity = AnsiString("account:") + AnsiString(account->GetAddress());
      caller.account = account;
      caller.via_app_password = viaAppPassword;
      caller.credential_name = appPasswordName;
      ApplyAccountGrant_(caller);

      if (secondFactor.matched_recovery_code != 0 && AccountRecovery::SpendCode(secondFactor.matched_recovery_code))
      {
         AccountRecovery::CodeState codes;
         AccountRecovery::GetCodes(account->GetID(), codes);
         caller.recovery_codes_left = codes.unused;

         String left;
         left.Format(_T("%d"), codes.unused);
         LOG_APPLICATION("REST API: " + account->GetAddress() + " signed in with a recovery code in place of its one-time code from " +
            String(peer_address.ToString()) + "; " + left + " of its recovery codes are left.");
      }

      return true;
   }

   bool
   RestApiServer::IsSelfServiceRoute_(RouteKind kind)
   {
      switch (kind)
      {
      case RouteMeFiles:
      case RouteMeFileCreate:
      case RouteMeFileContent:
      case RouteMeFileUpdate:
      case RouteMeFileDelete:
      case RouteMeDraftSchedule:
      case RouteMeDraftUnschedule:
      case RouteMeMessageSnooze:
      case RouteMeScheduled:
      case RouteMeScheduledCancel:
      case RouteMeFolderExport:
      case RouteMeExportArchive:
      case RouteMeFolderImport:
      case RouteMeMessageReceipt:
      case RouteMeMessageUnsubscribe:
      case RouteMeSubscriptions:
      case RouteMeBrandLogo:
      case RouteMeFetchAccounts:
      case RouteMeFetchAccountCreate:
      case RouteMeFetchAccount:
      case RouteMeFetchAccountUpdate:
      case RouteMeFetchAccountDelete:
      case RouteMeFetchAccountDownload:
      case RouteMeFetchImport:
      case RouteMeFetchImportStart:
      case RouteMeSenderAddresses:
      case RouteMeSenderAddressCreate:
      case RouteMeSenderAddressVerify:
      case RouteMeSenderAddressResend:
      case RouteMeSenderAddressDelete:
      case RouteMeMaskedAddresses:
      case RouteMeMaskedAddressCreate:
      case RouteMeMaskedAddressUpdate:
      case RouteMeMaskedAddressDelete:
      case RouteMeRecovery:
      case RouteMeRecoveryPut:
      case RouteMeRecoveryDelete:
      case RouteMeRecoveryResend:
      case RouteMeRecoveryCodes:
      case RouteMeRecoverable:
      case RouteMeRecoverableRestore:
      case RouteMeRecoverableDelete:
      case RouteMeConfidential:
      case RouteMeConfidentialRevoke:
      case RouteMeOffline:
      case RouteMeOfflinePut:
      case RouteMeOfflineMessages:
      case RouteMeAppPasswords:
      case RouteMeAppPasswordCreate:
      case RouteMeAppPasswordDelete:
      case RouteMeSessions:
      case RouteMeSessionsEnd:
      case RouteMeSessionDelete:
      case RouteMeDevices:
      case RouteMeDeviceDelete:
      case RouteMeStorage:
      case RouteMeSmime:
      case RouteMeSmimeOwnPut:
      case RouteMeSmimeOwnDelete:
      case RouteMeSmimeRecipientPut:
      case RouteMeSmimeRecipientDelete:
      case RouteMeSmimeChain:
      case RouteMeOpenPgp:
      case RouteMeOpenPgpPut:
      case RouteMeOpenPgpDelete:
      case RouteMeOpenPgpPublish:
      case RouteMeFolderEmpty:
      case RouteMeMessageSource:
      case RouteMeMessageHtml:
      case RouteMeIdentities:
      case RouteMePreferences:
      case RouteMePreferencesPut:
      case RouteMePush:
      case RouteMePushSubscribe:
      case RouteMePushUpdate:
      case RouteMePushDelete:
      case RouteMePushTest:
      case RouteMeCalendars:
      case RouteMeCalendarCreate:
      case RouteMeCalendarUpdate:
      case RouteMeCalendarDelete:
      case RouteMeCalendarSharePut:
      case RouteMeCalendarShareDelete:
      case RouteMeEvents:
      case RouteMeEventGet:
      case RouteMeEventCreate:
      case RouteMeEventUpdate:
      case RouteMeEventDelete:
      case RouteMeTasks:
      case RouteMeTaskCreate:
      case RouteMeTaskUpdate:
      case RouteMeTaskDelete:
      case RouteMeMessageInvitation:
      case RouteMeMessageInvitationReply:
      case RouteMeContacts:
      case RouteMeContactCreate:
      case RouteMeContactUpdate:
      case RouteMeContactDelete:
      case RouteMeContactPhoto:
      case RouteMeContactGroups:
      case RouteMeContactGroupCreate:
      case RouteMeContactGroupGet:
      case RouteMeContactGroupUpdate:
      case RouteMeContactGroupDelete:
      case RouteMeContactGroupMemberAdd:
      case RouteMeContactGroupMemberRemove:
      case RouteMe:
      case RouteMePassword:
      case RouteMeVacation:
      case RouteMeQuarantineList:
      case RouteMeQuarantineRelease:
      case RouteMeQuarantineDelete:
      case RouteMeQuarantineDigestGet:
      case RouteMeQuarantineDigestPut:
      case RouteSenderListMeList:
      case RouteSenderListMeCreate:
      case RouteSenderListMeUpdate:
      case RouteSenderListMeDelete:
      case RouteMeFolders:
      case RouteMeFolderCreate:
      case RouteMeFolderRename:
      case RouteMeFolderDelete:
      case RouteMeChanges:
      case RouteMeFolderMessages:
      case RouteMeMessage:
      case RouteMeMessageFlags:
      case RouteMeMessageMove:
      case RouteMeMessageDelete:
      case RouteMeMessageSend:
      case RouteMeMessageAttachment:
      case RouteMeSearch:
      case RouteMeSettings:
      case RouteMeSettingsPut:
      case RouteMeFilters:
      case RouteMeFiltersPut:
      case RouteMeDraftSave:
      case RouteSessionCreate:
      case RouteSessionDelete:
         return true;

      default:
         break;
      }

      return false;
   }

   HttpResponse
   RestApiServer::HandleMe_(const Caller &caller)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      // Quota as the IMAP QUOTA extension reports it: the limit is stored in
      // megabytes (0 = none) and the usage is the size cache's byte count.
      __int64 usedBytes = AccountSizeCache::Instance()->GetSize(account->GetID());

      AnsiString json;
      json.Format(
         "{\"address\":\"%hs\",\"domain\":\"%hs\",\"active\":%hs,"
         "\"quota\":{\"limit_mb\":%d,\"used_bytes\":%I64d},"
         "\"vacation\":{\"enabled\":%hs,\"active\":%hs,\"subject\":\"%hs\",\"message\":\"%hs\",\"expires\":%hs,\"expires_date\":\"%hs\"},"
         "\"password_changed\":\"%hs\",\"second_factor\":%hs,\"directory_linked\":%hs}",
         JsonEscape_(Utf8_(account->GetAddress())).c_str(),
         JsonEscape_(Utf8_(StringParser::ExtractDomain(account->GetAddress()))).c_str(),
         account->GetActive() ? "true" : "false",
         (int) account->GetAccountMaxSize(),
         usedBytes,
         account->GetVacationMessageIsOn() ? "true" : "false",
         PersistentAccount::GetIsVacationMessageOn(account) ? "true" : "false",
         JsonEscape_(Utf8_(account->GetVacationSubject())).c_str(),
         JsonEscape_(Utf8_(account->GetVacationMessage())).c_str(),
         account->GetVacationExpires() ? "true" : "false",
         // The store keeps a date-time ("2099-12-31 00:00:00"); the day is the
         // part that was chosen, and the form the PUT accepts back.
         JsonEscape_(Utf8_(account->GetVacationExpiresDate().Mid(0, 10))).c_str(),
         JsonEscape_(Utf8_(account->GetPasswordChanged())).c_str(),
         account->GetTotpSecret().IsEmpty() ? "false" : "true",
         account->GetIsAD() ? "true" : "false");

      return BuildResponse_(200, json);
   }

   HttpResponse
   RestApiServer::HandleMePassword_(const Caller &caller, const AnsiString &request)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      AnsiString requestBody = GetRequestBody_(request);
      String currentPassword = JsonUtf8Value_(requestBody, "current");
      String newPassword = JsonUtf8Value_(requestBody, "new");

      if (currentPassword.IsEmpty() || newPassword.IsEmpty())
         return BuildResponse_(400, "{\"error\":\"current and new are required\"}");

      // A directory-linked account has no password of its own here: the
      // directory holds it, and this server only ever asks the directory
      // whether one is right.
      if (account->GetIsAD())
         return BuildResponse_(409, "{\"error\":\"this account's password is managed by the directory it is linked to\"}");

      // The password that was presented to log on may have been an app
      // password, which is a mailbox credential and not a licence to replace
      // the account's own. "current" therefore has to be the account password
      // itself, checked here against the stored hash, whatever authenticated
      // the request.
      Crypt::EncryptionType currentType = (Crypt::EncryptionType) account->GetPasswordEncryption();
      if (account->GetPassword().IsEmpty() ||
          !Crypt::Instance()->Validate(currentPassword, account->GetPassword(), currentType))
      {
         LOG_APPLICATION("REST API: a password change for " + account->GetAddress() + " was refused - the current password did not match.");
         RegisterAuthenticationFailure_(caller.peer);
         return BuildResponse_(403, "{\"error\":\"the current password is not correct\"}");
      }

      // An account with a second factor proves it here the way the
      // administrator does on every request: the code travels in
      // X-hMailServer-OTP, and the 401 that asks for it names the header. A
      // recovery code in X-hMailServer-Recovery-Code stands in for it for
      // somebody whose authenticator is gone (AccountRecovery.h). It is matched
      // here and spent at the bottom, once the password has actually changed:
      // the policy, the reuse history and the breach corpus below can all still
      // refuse the new password, and ten refused choices must not leave an
      // account with no authenticator and no codes.
      __int64 recoveryCodeId = 0;
      if (!SecondFactorProved_(account, request, &recoveryCodeId))
         return BuildUnauthorizedResponse_(true, IsPageScriptRequest_(request));

      // The same rules the Control Panel and COM apply when an administrator
      // sets a password, from the same two places, so a user cannot choose
      // what an administrator could not.
      String policyFailure;
      if (!PasswordPolicy::IsAcceptable(account->GetAddress(), newPassword, policyFailure))
      {
         AnsiString body;
         body.Format("{\"error\":\"%hs\"}", JsonEscape_(Utf8_(policyFailure)).c_str());
         return BuildResponse_(400, body);
      }

      if (PasswordHistory::IsReuse(account, newPassword))
         return BuildResponse_(409, "{\"error\":\"this password has been used recently on this account; choose one that has not\"}");

      // The breach corpus last, so that the rules decided here cost no request, and
      // before anything is recorded. The reason word is what the webmail translates.
      {
         String breachRefusal;
         AnsiString breachReason;
         int breachStatus = 0;

         if (!PasswordBreachCheck::IsAcceptable(account->GetAddress(), newPassword, breachRefusal, breachReason, breachStatus))
         {
            AnsiString body;
            body.Format("{\"error\":\"%hs\",\"reason\":\"%hs\"}", JsonEscape_(Utf8_(breachRefusal)).c_str(), breachReason.c_str());
            return BuildResponse_(breachStatus, body);
         }
      }

      std::shared_ptr<Account> mutableAccount = std::shared_ptr<Account>(new Account());
      if (!PersistentAccount::ReadObject(mutableAccount, account->GetID()) || mutableAccount->GetID() == 0)
         return BuildResponse_(500, "{\"error\":\"the account could not be read\"}");

      PasswordHistory::Record(mutableAccount);

      int preferredHashAlgorithm = IniFileSettings::Instance()->GetPreferredHashAlgorithm();
      mutableAccount->SetPassword(Crypt::Instance()->EnCrypt(newPassword, (Crypt::EncryptionType) preferredHashAlgorithm));
      mutableAccount->SetPasswordEncryption(preferredHashAlgorithm);
      mutableAccount->SetPasswordChanged(Time::GetCurrentDateTime());

      String saveError;
      if (!PersistentAccount::SaveObject(mutableAccount, saveError, PersistenceModeNormal))
      {
         AnsiString body;
         body.Format("{\"error\":\"%hs\"}", JsonEscape_(Utf8_(saveError.IsEmpty() ? String(_T("the account could not be saved")) : saveError)).c_str());
         return BuildResponse_(500, body);
      }

      // Every other browser signed in to this account is signed out: a
      // password is changed most urgently when somebody else may have it,
      // and a session they already hold would outlive the change.
      RevokeSessionsForAccount_(account->GetID(), caller.via_session ? caller.session_hash : AnsiString());

      SpendSecondFactor_(account, recoveryCodeId);

      LOG_APPLICATION("REST API: " + account->GetAddress() + " changed its own password from " + String(caller.peer.ToString()) + ".");

      return BuildResponse_(200, "{\"changed\":true}");
   }

   HttpResponse
   RestApiServer::HandleMeVacation_(const Caller &caller, const AnsiString &requestBody)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      // The whole state at once - enabled, subject, message and the optional
      // expiry - so that what the account has afterwards is exactly what the
      // body said, and nothing is left over from before.
      AnsiString enabledText = GetJsonStringValue_(requestBody, "enabled");
      bool enabled = GetJsonBoolValue_(requestBody, "enabled", false);
      String subject = JsonUtf8Value_(requestBody, "subject");
      String message = JsonUtf8Value_(requestBody, "message");
      bool expires = GetJsonBoolValue_(requestBody, "expires", false);
      String expiresDate = JsonUtf8Value_(requestBody, "expires_date");

      if (requestBody.Find("\"enabled\"") < 0)
         return BuildResponse_(400, "{\"error\":\"enabled is required\"}");

      if (subject.GetLength() > 200 || message.GetLength() > 20000)
         return BuildResponse_(400, "{\"error\":\"the subject may be 200 characters and the message 20000\"}");

      if (expires)
      {
         // YYYY-MM-DD, the form the account stores and the deliverer compares.
         bool wellFormed = expiresDate.GetLength() == 10 && expiresDate[4] == '-' && expiresDate[7] == '-';
         for (int i = 0; wellFormed && i < 10; i++)
         {
            if (i == 4 || i == 7)
               continue;
            if (expiresDate[i] < '0' || expiresDate[i] > '9')
               wellFormed = false;
         }

         if (!wellFormed)
            return BuildResponse_(400, "{\"error\":\"expires_date must be YYYY-MM-DD when expires is true\"}");
      }

      std::shared_ptr<Account> mutableAccount = std::shared_ptr<Account>(new Account());
      if (!PersistentAccount::ReadObject(mutableAccount, account->GetID()) || mutableAccount->GetID() == 0)
         return BuildResponse_(500, "{\"error\":\"the account could not be read\"}");

      mutableAccount->SetVacationSubject(subject);
      mutableAccount->SetVacationMessage(message);
      mutableAccount->SetVacationExpires(expires);
      mutableAccount->SetVacationExpiresDate(expires ? expiresDate : String());
      mutableAccount->SetVacationMessageIsOn(enabled);

      String saveError;
      if (!PersistentAccount::SaveObject(mutableAccount, saveError, PersistenceModeNormal))
      {
         AnsiString body;
         body.Format("{\"error\":\"%hs\"}", JsonEscape_(Utf8_(saveError.IsEmpty() ? String(_T("the account could not be saved")) : saveError)).c_str());
         return BuildResponse_(500, body);
      }

      AnsiString json;
      json.Format("{\"enabled\":%hs,\"subject\":\"%hs\",\"message\":\"%hs\",\"expires\":%hs,\"expires_date\":\"%hs\"}",
         enabled ? "true" : "false",
         JsonEscape_(Utf8_(subject)).c_str(),
         JsonEscape_(Utf8_(message)).c_str(),
         expires ? "true" : "false",
         JsonEscape_(Utf8_(expires ? expiresDate : String())).c_str());

      return BuildResponse_(200, json);
   }

   bool
   RestApiServer::AuthenticateSession_(const AnsiString &request, const IPAddress &peer_address, Caller &caller)
   {
      // The cookie is the only credential a browser can hold without the page
      // keeping the password in memory. Its value is a random token; only the
      // token's SHA-256 is stored, so the table leaks nothing if it is read.
      AnsiString cookies = GetHeader_(request, "cookie");
      if (cookies.IsEmpty())
         return false;

      AnsiString token;
      std::vector<AnsiString> parts = StringParser::SplitString(cookies, ";");
      for (size_t i = 0; i < parts.size(); i++)
      {
         AnsiString part = parts[i];
         part.Trim();

         const AnsiString prefix = AnsiString(SessionCookieName) + "=";
         if (part.GetLength() > prefix.GetLength() && part.Mid(0, prefix.GetLength()) == prefix)
         {
            token = part.Mid(prefix.GetLength());
            break;
         }
      }

      return AuthenticateSessionToken_(token, false, peer_address, caller);
   }

   bool
   RestApiServer::AuthenticateSessionToken_(const AnsiString &token, bool presentedAsBearer, const IPAddress &peer_address, Caller &caller)
   {
      if (!IsLowerHex(token, SessionTokenBytes * 2))
         return false;

      AnsiString presentedHash = HashApiKeyToken(token);
      if (presentedHash.IsEmpty())
         return false;

      __int64 accountId = 0;
      bool administrator = false;
      bool viaAppPassword = false;
      AnsiString credentialStamp;
      AnsiString supportBy;
      AnsiString device;
      {
         std::lock_guard<std::mutex> guard(browser_sessions_mutex);

         const ULONGLONG now = GetTickCount64();

         for (std::vector<BrowserSession>::iterator it = browser_sessions.begin(); it != browser_sessions.end(); ++it)
         {
            if (!ConstantTimeEquals(presentedHash, it->token_hash))
               continue;

            // The right token in the wrong place: a value that was answered
            // in a body and has turned up in a Cookie header, or a cookie's
            // value turned up as a bearer. Neither is how the session was
            // made, and neither is accepted - refused, and left standing for
            // the client that holds it properly.
            if (it->bearer != presentedAsBearer)
               return false;

            // The policy is re-read here rather than remembered from sign-in,
            // so that shortening an idle timeout or turning a switch on reaches
            // the sessions that already exist.
            ApplyPolicy_(*it);

            // Two ceilings, both absolute in their own way: the session ends
            // when it has been idle for the policy's idle period, and it ends
            // the policy's lifetime after it was created whatever the user is
            // doing - a captured cookie stays useful for a bounded time, not
            // for as long as the victim keeps clicking.
            if (HasExpired_(*it, now))
            {
               browser_sessions.erase(it);
               return false;
            }

            // The session bound to the network it began on. Off unless an
            // administrator asked for it, because an address that changes is
            // ordinary - a phone moving from wi-fi to a mobile network - and
            // the session ends when it does. With it on, a cookie taken from
            // one machine is of no use from another.
            if (it->bind_to_address && it->bound_address != AnsiString(peer_address.ToString()))
            {
               LOG_APPLICATION("REST API: a session that began at " + String(it->bound_address) + " was presented from " +
                  String(peer_address.ToString()) + " and has been ended; SessionBindToAddress is on.");
               browser_sessions.erase(it);
               return false;
            }

            it->last_seen_at = now;
            accountId = it->account_id;
            administrator = it->administrator;
            credentialStamp = it->credential_stamp;
            viaAppPassword = it->via_app_password;
            supportBy = it->support_by;
            device = it->device;
            break;
         }
      }

      if (administrator)
      {
         // Checked afresh on every request, as an account's row is below: the
         // administrator password that started the session has to be the one
         // in force. An empty password is the API switched off; a different
         // one is a change made since sign-in - quite possibly because
         // somebody else has the old one - and either ends every
         // administrator session.
         const AnsiString current = AdministratorCredentialStamp();
         if (current.IsEmpty() || !ConstantTimeEquals(current, credentialStamp))
         {
            RevokeAdministratorSessions();
            return false;
         }

         // The administrator password's authority, exactly: full, unscoped,
         // and named the same for the rate budget and for the log. via_session
         // is what makes a write on it carry X-Requested-With.
         caller.result = AuthenticatedAsAdministrator;
         caller.read_only = false;
         caller.identity = "administrator";
         caller.via_session = true;
         caller.via_bearer = presentedAsBearer;
         caller.session_hash = presentedHash;
         caller.session_device = device;
         caller.grant = RoleManager::ServerAdministrator();

         return true;
      }

      if (accountId == 0)
         return false;

      // Read fresh on every request rather than remembered from sign-in: an
      // account that was deactivated or deleted after the session began is
      // refused from the next request, and its session is dropped.
      std::shared_ptr<Account> account = std::shared_ptr<Account>(new Account());
      if (!PersistentAccount::ReadObject(account, accountId) || account->GetID() == 0 || !account->GetActive())
      {
         RevokeSessionsForAccount_(accountId, "");
         return false;
      }

      caller.result = AuthenticatedAsAccount;
      caller.read_only = false;
      caller.identity = AnsiString("account:") + AnsiString(account->GetAddress());
      caller.account = account;
      caller.via_session = true;
      caller.via_bearer = presentedAsBearer;
      caller.session_hash = presentedHash;
      caller.session_device = device;
      caller.via_app_password = viaAppPassword;
      ApplyAccountGrant_(caller);

      if (presentedAsBearer)
      {
         // The bearer form is the mailbox and nothing more. The token is a
         // credential a page holds in script, which a cross-site script in
         // that page could read, and that is why the form is refused to the
         // administrator password - but an account that holds a role would
         // carry the same reach in the same place: the server administrator's
         // own mailbox, added as a second account, would put every
         // administration route behind a script-readable token. So the role
         // goes, exactly as it goes for an app password (ApplyAccountGrant_)
         // and for a support session (below): Authorize_ then refuses the
         // bearer everything outside /api/v1/me, as it refuses an ordinary
         // account. Nothing the webmail does is lost, since it asks for
         // nothing else on that token; a role account administers from its
         // own sign-in, whose cookie no script reads.
         caller.grant = RoleGrant();
         caller.domains.clear();
      }

      if (!supportBy.IsEmpty())
      {
         // A support session opens the portal as the account, for the person
         // who asked for help. It is not that account's administration: an
         // administrator who opens one into a mailbox that happens to hold a
         // role must not find the session reaching the administration API as
         // well, so the grant goes.
         caller.grant = RoleGrant();
         caller.domains.clear();

         // Every request of a support session is on the record: who, as whom,
         // from where, what.
         caller.support_by = supportBy;
         LOG_APPLICATION("REST API: support session: " + String(supportBy) + " acting as " + account->GetAddress() + " from " + String(caller.peer.ToString()) + ".");
      }
      return true;
   }

   HttpResponse
   RestApiServer::HandleSessionCreate_(const Caller &caller, const AnsiString &request, const AnsiString &query)
   {
      if (caller.via_session)
         return BuildResponse_(403, "{\"error\":\"a session is started with a password, not with another session\"}");

      // Who the session stands for: the account whose password was presented,
      // or the administrator. Authorize_ lets nothing else this far.
      const bool administrator = caller.result == AuthenticatedAsAdministrator;

      std::shared_ptr<const Account> account = caller.account;
      if (!administrator && !account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      // The bearer form, ?bearer=1: the token is answered in the body and no
      // cookie is set, for a page that already holds one account's cookie and
      // is adding a second account beside it - a cookie is one per origin,
      // and the first account's is already there. The page keeps the token in
      // memory and sends it as Authorization: Bearer on the second account's
      // requests; the session is in every other respect the one a cookie
      // would be. Accounts only: the administrator's session is the Control
      // Deck's cookie, and a token for the administrator held in a page's
      // script would be the whole server in the reach of one cross-site
      // script, which is not a thing this route mints.
      const bool bearer = QueryParameter_(query, "bearer") == "1";
      if (bearer && administrator)
         return BuildResponse_(403, "{\"error\":\"the bearer form of a session is for an account added beside another in the webmail; the administrator's session is a cookie\"}");

      if (bearer)
      {
         // The account the browser's cookie already stands for is not a
         // second account. The request carries that cookie - the page sends
         // it with the Basic sign-in as it sends it with everything - and
         // when it is this account's, the sign-in is refused here, before
         // the one-per-device rule below could end the cookie's session for
         // a sign-in of the same account from the same device: the page
         // would then find every request on its cookie answered 401 and
         // sign itself out for having added the account it was already in.
         // The page refuses the address it knows before asking; this is
         // for the spellings it cannot know, an alias domain among them,
         // which PasswordValidator resolves to the one account. A cookie
         // that has expired, or another account's, or the administrator's,
         // is not this case, and neither is no cookie at all.
         Caller viaCookie;
         viaCookie.peer = caller.peer;
         if (AuthenticateSession_(request, caller.peer, viaCookie) && viaCookie.account && viaCookie.account->GetID() == account->GetID())
            return BuildResponse_(409, "{\"error\":\"that is the account this browser is already signed in as; a second account is another account\"}");
      }

      // The stamp an administrator session is checked against on every
      // request, taken now from the credential that was just accepted.
      AnsiString credentialStamp;
      if (administrator)
      {
         credentialStamp = AdministratorCredentialStamp();
         if (credentialStamp.IsEmpty())
            return BuildResponse_(500, "{\"error\":\"internal error\"}");
      }

      unsigned char secret[SessionTokenBytes];
      if (RAND_bytes(secret, sizeof(secret)) != 1)
         return BuildResponse_(500, "{\"error\":\"no entropy for a session token\"}");

      AnsiString token = BytesToLowerHex(secret, SessionTokenBytes);
      AnsiString tokenHash = HashApiKeyToken(token);
      if (tokenHash.IsEmpty())
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      // The client this sign-in came from: the protocol, the address and the
      // user agent, hashed into one key by SignInStore, which is what the
      // account's device list names and what revoking a device ends.
      const AnsiString peerAddress = caller.peer.ToString();
      const AnsiString agent = GetHeader_(request, "user-agent");
      String wideAgent;
      Unicode::MultiByteToWide(agent, wideAgent);
      const String device = SignInStore::DeviceKey(SignInStore::ProtocolWeb, String(peerAddress), wideAgent);

      SessionPolicy policy = administrator
         ? SessionPolicies::ServerWide()
         : SessionPolicies::ForAddress(account->GetAddress());

      {
         std::lock_guard<std::mutex> guard(browser_sessions_mutex);

         const ULONGLONG now = GetTickCount64();

         // Expired sessions go first, so the table holds what is live; then,
         // if it is still full, the least recently used one goes. The cap is
         // what stops a script from filling memory with sign-ins.
         browser_sessions.erase(
            std::remove_if(browser_sessions.begin(), browser_sessions.end(),
               [now](const BrowserSession &session)
               {
                  return HasExpired_(session, now);
               }),
            browser_sessions.end());

         // One session per device: signing in again from a client that already
         // has a session for this account ends the earlier one, so somebody who
         // signs in on a shared machine and forgets leaves nothing behind the
         // next time they sign in there.
         if (policy.one_per_device && !device.IsEmpty())
         {
            const __int64 owner = administrator ? 0 : account->GetID();
            const bool wantAdministrator = administrator;
            const AnsiString deviceKey = AnsiString(device);

            browser_sessions.erase(
               std::remove_if(browser_sessions.begin(), browser_sessions.end(),
                  [&deviceKey, owner, wantAdministrator](const BrowserSession &session)
                  {
                     return session.administrator == wantAdministrator && session.account_id == owner &&
                            session.device == deviceKey;
                  }),
               browser_sessions.end());
         }

         if (browser_sessions.size() >= MaxBrowserSessions)
         {
            size_t oldest = 0;
            for (size_t i = 1; i < browser_sessions.size(); i++)
            {
               if (browser_sessions[i].last_seen_at < browser_sessions[oldest].last_seen_at)
                  oldest = i;
            }

            browser_sessions.erase(browser_sessions.begin() + oldest);
         }

         BrowserSession session;
         session.token_hash = tokenHash;
         session.account_id = administrator ? 0 : account->GetID();
         session.administrator = administrator;
         session.credential_stamp = credentialStamp;
         session.via_app_password = caller.via_app_password;
         session.bearer = bearer;
         session.created_at = now;
         session.last_seen_at = now;
         session.device = AnsiString(device);
         session.bound_address = peerAddress;
         session.domain = administrator ? String() : StringParser::ExtractDomain(account->GetAddress());

         ApplyPolicy_(session);
         policy.idle_seconds = (int) (session.idle_milliseconds / 1000);
         policy.lifetime_seconds = (int) (session.lifetime_milliseconds / 1000);

         browser_sessions.push_back(session);
      }

      // The device list is the account's, so only an account's sign-in writes
      // one: the administrator password is not an account and has no mailbox to
      // list devices for. A session started with an app password says so, and
      // names the password, because which credential a client is using is the
      // question the list exists to answer.
      if (!administrator)
      {
         const int credential = caller.via_passkey ? SignInStore::CredentialPasskey
                              : caller.via_single_sign_on ? SignInStore::CredentialSingleSignOn
                              : caller.via_app_password ? SignInStore::CredentialAppPassword
                              : SignInStore::CredentialPassword;

         SignInStore::RecordFrom(account, SignInStore::ProtocolWeb, String(peerAddress), wideAgent,
            credential, caller.credential_name);
      }

      // Braced: LOG_APPLICATION is a macro that carries its own if, and an
      // unbraced else after it binds to the wrong one. The token itself is
      // in neither line, nor anywhere else the server writes.
      if (administrator)
      {
         LOG_APPLICATION("REST API: the administrator started a browser session from " + String(caller.peer.ToString()) + ".");
      }
      else if (bearer)
      {
         LOG_APPLICATION("REST API: " + account->GetAddress() + " started a browser session in the bearer form (a second account in a webmail page) from " + String(caller.peer.ToString()) + ".");
      }
      else
      {
         LOG_APPLICATION("REST API: " + account->GetAddress() + " started a browser session from " + String(caller.peer.ToString()) + ".");
      }

      if (bearer)
      {
         // The token in the body and no Set-Cookie at all: the browser's cookie
         // jar is the first account's, and this answer must not touch it. What
         // the page holds is a credential a script can read, so the answer
         // says how long it lasts and the page says where it lives.
         AnsiString bearerBody;
         bearerBody.Format("{\"address\":\"%hs\",\"device\":\"%hs\",\"bearer\":true,\"token\":\"%hs\",\"idle_seconds\":%d,\"lifetime_seconds\":%d,\"one_per_device\":%hs,\"bound_to_address\":%hs%hs}",
            JsonEscape_(Utf8_(account->GetAddress())).c_str(),
            JsonEscape_(Utf8_(device)).c_str(),
            token.c_str(),
            policy.idle_seconds, policy.lifetime_seconds,
            policy.one_per_device ? "true" : "false",
            policy.bind_to_address ? "true" : "false",
            RecoveryCodesLeftJson_(caller).c_str());

         return BuildResponse_(201, bearerBody);
      }

      // HttpOnly: the script never reads it, so a script that should not be
      // there cannot either. SameSite=Strict: a request from another site does
      // not carry it. Secure whenever the listener speaks TLS - which it does
      // everywhere but loopback. Max-Age is the absolute ceiling; the idle
      // ceiling is shorter and is the server's to enforce.
      AnsiString cookie;
      cookie.Format("Set-Cookie: %hs=%hs; Path=/; HttpOnly; SameSite=Strict; Max-Age=%d%hs\r\n",
         SessionCookieName, token.c_str(), policy.lifetime_seconds, use_tls_ ? "; Secure" : "");

      AnsiString body;
      if (administrator)
      {
         body.Format("{\"administrator\":true,\"idle_seconds\":%d,\"lifetime_seconds\":%d,\"one_per_device\":%hs,\"bound_to_address\":%hs}",
            policy.idle_seconds, policy.lifetime_seconds,
            policy.one_per_device ? "true" : "false",
            policy.bind_to_address ? "true" : "false");
      }
      else
      {
         body.Format("{\"address\":\"%hs\",\"device\":\"%hs\",\"idle_seconds\":%d,\"lifetime_seconds\":%d,\"one_per_device\":%hs,\"bound_to_address\":%hs%hs}",
            JsonEscape_(Utf8_(account->GetAddress())).c_str(),
            JsonEscape_(Utf8_(device)).c_str(),
            policy.idle_seconds, policy.lifetime_seconds,
            policy.one_per_device ? "true" : "false",
            policy.bind_to_address ? "true" : "false",
            RecoveryCodesLeftJson_(caller).c_str());
      }

      return BuildResponse_(201, body, cookie);
   }

   HttpResponse
   RestApiServer::HandleSessionDelete_(const Caller &caller)
   {
      if (!caller.via_session)
         return BuildResponse_(400, "{\"error\":\"there is no session to end: this request carried a password, not a session cookie\"}");

      {
         std::lock_guard<std::mutex> guard(browser_sessions_mutex);

         browser_sessions.erase(
            std::remove_if(browser_sessions.begin(), browser_sessions.end(),
               [&caller](const BrowserSession &session)
               {
                  return ConstantTimeEquals(session.token_hash, caller.session_hash);
               }),
            browser_sessions.end());
      }

      // A session that came as a bearer leaves the cookie alone: the cookie
      // in this browser is another account's session, and clearing it here
      // would sign that account out for ending this one.
      if (caller.via_bearer)
         return BuildResponse_(200, "{\"ended\":true,\"bearer\":true}");

      AnsiString cookie;
      cookie.Format("Set-Cookie: %hs=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0%hs\r\n",
         SessionCookieName, use_tls_ ? "; Secure" : "");

      return BuildResponse_(200, "{\"ended\":true}", cookie);
   }

   // Every browser session one client of an account holds, ended. Called by
   // SignInStore::Revoke, which is the one place a device is revoked from,
   // whichever interface asked - so a revoke over REST and a revoke over COM
   // end the same things in the same order.
   int
   RestApiServer::EndSessionsForDevice(__int64 accountId, const String &device)
   {
      if (accountId == 0 || device.IsEmpty())
         return 0;

      const AnsiString wanted = AnsiString(device);
      int ended = 0;

      {
         std::lock_guard<std::mutex> guard(browser_sessions_mutex);

         for (std::vector<BrowserSession>::iterator it = browser_sessions.begin(); it != browser_sessions.end();)
         {
            if (!it->administrator && it->account_id == accountId && it->device == wanted)
            {
               it = browser_sessions.erase(it);
               ended++;
            }
            else
               ++it;
         }
      }

      return ended;
   }

   // Every browser session of an account, ended - the compromised-account
   // detection's call (CompromiseDetector::Apply), made from the submission
   // thread when it locks an account: a takeover that came in through the
   // webmail is signed out by the same act that locks it.
   void
   RestApiServer::RevokeSessionsForAccount(__int64 accountId)
   {
      if (accountId == 0)
         return;

      RevokeSessionsForAccount_(accountId, AnsiString());
   }

   void
   RestApiServer::RevokeSessionsForAccount_(__int64 accountId, const AnsiString &keepTokenHash)
   {
      std::lock_guard<std::mutex> guard(browser_sessions_mutex);

      browser_sessions.erase(
         std::remove_if(browser_sessions.begin(), browser_sessions.end(),
            [accountId, &keepTokenHash](const BrowserSession &session)
            {
               // Never an administrator row: its account_id is 0, and nothing
               // that names an account means the administrator.
               return !session.administrator && session.account_id == accountId &&
                      (keepTokenHash.IsEmpty() || !ConstantTimeEquals(session.token_hash, keepTokenHash));
            }),
         browser_sessions.end());
   }

   void
   RestApiServer::ClearBrowserSessions_()
   {
      std::lock_guard<std::mutex> guard(browser_sessions_mutex);
      browser_sessions.clear();
   }

   HttpResponse
   RestApiServer::HandleMeQuarantineList_(const Caller &caller)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      // The whole table, filtered here to the rows this address is a recipient
      // of; nothing about anyone else's mail is computed into the answer, not
      // even a count. The recipients field is left out for the same reason:
      // the user learns that a message was held for them, not for whom else.
      const std::vector<QuarantinedMessage> messages = QuarantineStore::List(1000);

      AnsiString body;
      body.Format("{\"enabled\":%hs,\"messages\":[", QuarantineStore::GetEnabled() ? "true" : "false");

      int count = 0;
      for (const QuarantinedMessage &message : messages)
      {
         if (!QuarantineStore::IsRecipient(message, account->GetAddress()))
            continue;

         if (count > 0)
            body += ",";

         AnsiString entry;
         entry.Format("{\"id\":%I64d,\"sender\":\"%hs\",\"subject\":\"%hs\",\"reason\":\"%hs\",\"score\":%d,\"size\":%d,\"created\":\"%hs\"}",
            message.id,
            JsonEscape_(Utf8_(message.sender)).c_str(),
            JsonEscape_(Utf8_(message.subject)).c_str(),
            JsonEscape_(Utf8_(message.reason)).c_str(),
            message.score,
            message.size,
            JsonEscape_(Utf8_(message.created)).c_str());

         body += entry;
         count++;
      }

      body += "]}";

      return BuildResponse_(200, body);
   }

   HttpResponse
   RestApiServer::HandleMeQuarantineRelease_(const Caller &caller, __int64 id)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      // A message this address was not sent is "not found", not "forbidden":
      // the id space is shared, and a refusal that differed would confirm that
      // somebody else's message exists.
      QuarantinedMessage message;
      if (!QuarantineStore::GetById(id, message) || !QuarantineStore::IsRecipient(message, account->GetAddress()))
         return BuildResponse_(404, "{\"error\":\"quarantined message not found\"}");

      String error;
      if (!QuarantineStore::ReleaseTo(id, account->GetAddress(), error))
      {
         AnsiString body;
         body.Format("{\"error\":\"%hs\"}", JsonEscape_(Utf8_(error)).c_str());
         return BuildResponse_(500, body);
      }

      return BuildResponse_(200, "{\"released\":true}");
   }

   HttpResponse
   RestApiServer::HandleMeQuarantineDelete_(const Caller &caller, __int64 id)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      QuarantinedMessage message;
      if (!QuarantineStore::GetById(id, message) || !QuarantineStore::IsRecipient(message, account->GetAddress()))
         return BuildResponse_(404, "{\"error\":\"quarantined message not found\"}");

      if (!QuarantineStore::DiscardFor(id, account->GetAddress()))
         return BuildResponse_(500, "{\"error\":\"the quarantined message could not be discarded\"}");

      return BuildResponse_(200, "{\"deleted\":true}");
   }

   namespace
   {
      // A message is read whole into memory for GET /api/v1/me/messages/{id};
      // this is the ceiling. Streaming is the HTTP foundation's declared debt,
      // and until it is paid a larger message is described and not read.
      const int MaxMessageBodyBytes = 1024 * 1024;

      // Up to this size a message is still parsed - for its attachment
      // list, and for one attachment at a time on the download route.
      const int MaxMessageParseBytes = 32 * 1024 * 1024;

      // The size a user expects: the bytes the file has, not the base64
      // that carries them. Decoding is a pass over the part, so only a
      // message small enough to read whole pays it; above that the encoded
      // length stands in.
      int AttachmentSize(std::shared_ptr<Attachment> attachment, bool decode)
      {
         if (decode)
         {
            AnsiString decoded;
            if (attachment->GetContent(decoded))
               return decoded.GetLength();
         }

         return attachment->GetSize();
      }

      // The media type a part declares, without the parameters the field
      // carries beside it (name=, charset=): "image/png" out of
      // "image/png; name=\"logo.png\"". Lower-cased, because a media type is
      // case-insensitive and a reader that compares it should not have to know
      // that. Empty when the part declares nothing usable, or when the value
      // could not be a media type at all - no solidus, or a byte that would
      // break out of the header it is written back into.
      AnsiString MediaType(const AnsiString &declared)
      {
         AnsiString type = declared;

         int semicolon = type.Find(";");
         if (semicolon >= 0)
            type = type.Mid(0, semicolon);

         type.ToLower();
         type.TrimLeft();
         type.TrimRight();

         if (type.IsEmpty() || type.Find("\r") >= 0 || type.Find("\n") >= 0 || type.Find("/") < 0)
            return "";

         return type;
      }

      // The part's Content-ID with the angle brackets stripped. RFC 2045 writes
      // the field as a msg-id, angle brackets and all; RFC 2392 writes the same
      // value in a cid: URL without them, and a cid: URL in the HTML body is
      // what a page has to match against. Empty when the part carries no
      // Content-ID, and empty for a value that could not be one.
      AnsiString ContentId(std::shared_ptr<MimeBody> part)
      {
         if (!part)
            return "";

         const char *raw = part->GetRawFieldValue(CMimeConst::ContentID());
         if (!raw)
            return "";

         AnsiString value = raw;
         value.TrimLeft();
         value.TrimRight();

         if (value.GetLength() >= 2 && value.StartsWith("<") && value.EndsWith(">"))
            value = value.Mid(1, value.GetLength() - 2);

         if (value.Find("\r") >= 0 || value.Find("\n") >= 0)
            return "";

         return value;
      }

      // How many messages one listing returns at most: the newest, and the
      // caller pages further back with before_uid.
      const int MaxMessagesPerPage = 200;

      // The listing decodes Subject, From and Date from the head of each
      // message's file - what IMAP FETCH ENVELOPE does - rather than parsing
      // the MIME tree, so a folder of thousands stays cheap.
      void DescribeHeaderWide(const String &fileName, String &subject, String &from, String &date)
      {
         AnsiString header = PersistentMessage::LoadHeader(fileName, false);
         if (header.IsEmpty())
            return;

         MimeHeader mimeHeader;
         mimeHeader.Load(header.c_str(), header.GetLength(), true);

         subject = mimeHeader.GetUnicodeFieldValue("Subject");
         from = mimeHeader.GetUnicodeFieldValue("From");

         const char *rawDate = mimeHeader.GetRawFieldValue("Date");
         date = rawDate ? String(rawDate) : String();
      }

      // The same, as UTF-8 for the JSON.
      void DescribeHeader(const String &fileName, AnsiString &subject, AnsiString &from, AnsiString &date)
      {
         String wideSubject, wideFrom, wideDate;
         DescribeHeaderWide(fileName, wideSubject, wideFrom, wideDate);

         Unicode::WideToMultiByte(wideSubject, subject);
         Unicode::WideToMultiByte(wideFrom, from);
         Unicode::WideToMultiByte(wideDate, date);
      }

      AnsiString FlagsJson(std::shared_ptr<Message> message)
      {
         // The keywords are IMAP atoms - no quote, no backslash, nothing
         // above ASCII - so they go into the JSON as they are.
         AnsiString keywords;
         std::vector<String> list = message->GetKeywordList();
         for (size_t i = 0; i < list.size(); i++)
         {
            if (i > 0)
               keywords += ",";
            keywords += "\"" + AnsiString(list[i]) + "\"";
         }
         AnsiString flags;
         flags.Format("{\"seen\":%hs,\"flagged\":%hs,\"answered\":%hs,\"draft\":%hs,\"deleted\":%hs,\"keywords\":[%hs]}",
            message->GetFlagSeen() ? "true" : "false",
            message->GetFlagFlagged() ? "true" : "false",
            message->GetFlagAnswered() ? "true" : "false",
            message->GetFlagDraft() ? "true" : "false",
            message->GetFlagDeleted() ? "true" : "false",
            keywords.c_str());
         return flags;
      }

      // The messages one search may look at before answering; the client
      // continues from next_before_uid. A mailbox is never scanned whole in
      // one request.
      const int MaxSearchScan = 2000;

      String ToLowerCopy(const String &value)
      {
         String lowered = value;
         lowered.ToLower();
         return lowered;
      }

      bool ContainsNoCase(const String &haystack, const String &needleLower)
      {
         if (needleLower.IsEmpty())
            return true;

         return ToLowerCopy(haystack).Find(needleLower) >= 0;
      }

      // What the full-text index can prove for one query: which of the
      // account's fully indexed messages cannot contain the text. The filter
      // SEARCH uses (IMAPCommandSearch.cpp), and it fails open the same way:
      // anything the index cannot answer for is read.
      class SearchIndexPrune
      {
      public:
         SearchIndexPrune(__int64 accountId, const String &text) :
            usable_(false)
         {
            if (text.IsEmpty() || !IniFileSettings::Instance()->GetIndexerFullTextEnabled())
               return;

            std::vector<String> needles;
            PersistentMessageIndex::CreateQueryNeedles(text, IniFileSettings::Instance()->GetIndexerFullTextMinTokenLength(), needles);
            if (needles.empty())
               return;

            if (!PersistentMessageIndex::GetMessagesWithTerm(accountId, PersistentMessageIndex::CompleteMarker(), complete_))
               return;

            if (!PersistentMessageIndex::GetMessagesWithTerm(accountId, PersistentMessageIndex::OverflowMarker(), overflow_))
               return;

            bool first = true;
            for (size_t i = 0; i < needles.size(); i++)
            {
               std::set<__int64> matches;
               if (!PersistentMessageIndex::GetMessagesWithTermContaining(accountId, needles[i], matches))
                  return;

               if (first)
               {
                  candidates_.swap(matches);
                  first = false;
               }
               else
               {
                  std::set<__int64> merged;
                  std::set_intersection(candidates_.begin(), candidates_.end(), matches.begin(), matches.end(), std::inserter(merged, merged.begin()));
                  candidates_.swap(merged);
               }

               if (candidates_.empty())
                  break;
            }

            usable_ = true;
         }

         bool CannotContain(__int64 messageId) const
         {
            if (!usable_)
               return false;

            if (complete_.find(messageId) == complete_.end())
               return false;

            if (overflow_.find(messageId) != overflow_.end())
               return false;

            return candidates_.find(messageId) == candidates_.end();
         }

      private:
         bool usable_;
         std::set<__int64> complete_;
         std::set<__int64> overflow_;
         std::set<__int64> candidates_;
      };

      // Whether one message matches the query: the operators first (from:,
      // subject:, has:attachment and the rest) against what the head of the
      // file says, then the free words in its Subject or From; a word found
      // in neither, or a to:, has the message read whole - once, each part
      // lower-cased once - for a message small enough and not ruled out by
      // the index.
      // A search as a reader writes it: words, "quoted phrases", and the
      // operators every webmail has taught - from:, to:, cc:, bcc:, subject:,
      // has:attachment, has:link, filename:, larger:10M, smaller:,
      // before:YYYY-MM-DD, after:, older_than:7d, newer_than:, in:folder,
      // is:unread, is:read, is:flagged, is:unflagged, is:answered, is:muted,
      // is:pinned, label:name, category:promotions, in_reply_to:<message-id>.
      // Every word must be found; the
      // operators narrow further. A term after a minus is one the message
      // must not match, and terms joined by OR are choices of which one must
      // match: each is a query of its own, held here and asked against the
      // same message, so a word, a phrase or an operator may be turned round
      // or offered as an alternative alike.
      struct SearchQuery
      {
         std::vector<String> terms;
         String from;
         String to;
         String cc;
         String bcc;
         String subject;
         String inFolder;
         String before;
         String after;
         String olderThan;
         String newerThan;
         String filename;
         String inReplyTo;
         AnsiString category;
         __int64 largerThan;
         __int64 smallerThan;
         bool hasAttachment;
         bool hasLink;
         bool wantUnread;
         bool wantRead;
         bool wantFlagged;
         bool wantUnflagged;
         bool wantAnswered;
         bool wantMuted;
         bool wantPinned;
         std::vector<String> labels;
         std::vector<std::vector<SearchQuery>> either;
         std::vector<SearchQuery> excluded;
         String freeText;

         SearchQuery() :
            largerThan(-1), smallerThan(-1),
            hasAttachment(false), hasLink(false), wantUnread(false), wantRead(false), wantFlagged(false), wantUnflagged(false), wantAnswered(false),
            wantMuted(false), wantPinned(false)
         {
         }
      };

      // The choices and the exclusions one query may hold, in all: each may
      // cost a read of the message, as a word past the subject and the
      // sender does, and words are capped at the same number.
      const size_t MaxSubQueries = 16;

      size_t SubQueryCount(const SearchQuery &q)
      {
         size_t count = q.excluded.size();
         for (size_t g = 0; g < q.either.size(); g++)
            count += q.either[g].size();
         return count;
      }

      bool IsDay(const String &value)
      {
         if (value.GetLength() != 10 || value[4] != '-' || value[7] != '-')
            return false;
         for (int i = 0; i < 10; i++)
            if (i != 4 && i != 7 && (value[i] < '0' || value[i] > '9'))
               return false;
         return true;
      }

      // A size as a reader writes one: digits, then K, M or G for multiples
      // of 1024 (a B after the letter is allowed). -1 when the value is not
      // one, or is past a terabyte.
      __int64 ParseSize(const String &value)
      {
         __int64 bytes = 0;
         int i = 0;
         for (; i < value.GetLength() && value[i] >= '0' && value[i] <= '9'; i++)
         {
            bytes = bytes * 10 + (value[i] - '0');
            if (bytes > 1024LL * 1024 * 1024 * 1024)
               return -1;
         }
         if (i == 0)
            return -1;
         if (i == value.GetLength())
            return bytes;

         String unit = value.Mid(i);
         if (unit == _T("k") || unit == _T("kb"))
            return bytes * 1024;
         if (unit == _T("m") || unit == _T("mb"))
            return bytes * 1024 * 1024;
         if (unit == _T("g") || unit == _T("gb"))
            return bytes * 1024 * 1024 * 1024;
         return -1;
      }

      // A span as a reader writes one - 7d, 2w, 3m, 1y - as the moment that
      // long ago: "YYYY-MM-DD HH:MM:SS" in local time, the shape and the
      // clock of the stored time (Time::GetCurrentDateTime), so the
      // comparison is between like and like. A month or a year back is the
      // same day of the month as the C library counts it. Empty when the
      // value is not a span.
      String MomentAgo(const String &value)
      {
         if (value.GetLength() < 2)
            return String();

         int count = 0;
         int i = 0;
         for (; i < value.GetLength() - 1; i++)
         {
            if (value[i] < '0' || value[i] > '9')
               return String();
            count = count * 10 + (value[i] - '0');
            if (count > 10000)
               return String();
         }
         if (count == 0)
            return String();

         const std::time_t now = std::time(nullptr);
         struct tm moment;
         if (localtime_s(&moment, &now) != 0)
            return String();

         const wchar_t unit = value[i];
         if (unit == 'd')
            moment.tm_mday -= count;
         else if (unit == 'w')
            moment.tm_mday -= count * 7;
         else if (unit == 'm')
            moment.tm_mon -= count;
         else if (unit == 'y')
            moment.tm_year -= count;
         else
            return String();

         // mktime carries the field that went negative into the ones above
         // it, and decides for itself whether that moment was in summer time.
         moment.tm_isdst = -1;
         if (mktime(&moment) == (std::time_t) -1)
            return String();

         char text[32];
         if (strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &moment) == 0)
            return String();

         return String(text);
      }

      bool IsCategory(const String &value)
      {
         return value == _T("primary") || value == _T("social") || value == _T("promotions") || value == _T("updates") || value == _T("forums");
      }

      bool StartsWithWebScheme(const String &text, int at)
      {
         return text.Find(_T("http://"), at) == at || text.Find(_T("https://"), at) == at;
      }

      // Whether the message carries a link a reader could follow: an http or
      // https address written in the text part, or an anchor whose href is
      // one in the HTML part. An image fetched from the web is not a link,
      // so the HTML is read for href= and not for any address in it. Both
      // parts arrive lower-cased.
      bool HasWebLink(const String &bodyLower, const String &htmlLower)
      {
         if (bodyLower.Find(_T("http://")) >= 0 || bodyLower.Find(_T("https://")) >= 0)
            return true;

         int at = htmlLower.Find(_T("href"));
         while (at >= 0)
         {
            int i = at + 4;
            while (i < htmlLower.GetLength() && (htmlLower[i] == ' ' || htmlLower[i] == '\t' || htmlLower[i] == '\r' || htmlLower[i] == '\n'))
               i++;
            if (i < htmlLower.GetLength() && htmlLower[i] == '=')
            {
               i++;
               while (i < htmlLower.GetLength() && (htmlLower[i] == ' ' || htmlLower[i] == '\t' || htmlLower[i] == '\r' || htmlLower[i] == '\n'))
                  i++;
               if (i < htmlLower.GetLength() && (htmlLower[i] == '"' || htmlLower[i] == '\''))
                  i++;
               if (StartsWithWebScheme(htmlLower, i))
                  return true;
            }
            at = htmlLower.Find(_T("href"), at + 4);
         }
         return false;
      }

      // One term applied to a query: an operator sets its field, a term the
      // parser does not know is a word.
      void ApplyTerm(SearchQuery &q, const String &token)
      {
         if (token.GetLength() > 1 && token[0] == '-' && token[1] != '-')
         {
            // -term: what follows the minus, as a query of its own the
            // message must not match.
            if (SubQueryCount(q) < MaxSubQueries)
            {
               SearchQuery none;
               ApplyTerm(none, token.Mid(1));
               q.excluded.push_back(none);
            }
            return;
         }

         String lower = ToLowerCopy(token);
         int colon = lower.Find(_T(":"));
         String key = colon > 0 ? lower.Mid(0, colon) : String();
         String value = colon > 0 ? lower.Mid(colon + 1) : String();

         if (key == _T("from") && !value.IsEmpty())
            q.from = value;
         else if (key == _T("to") && !value.IsEmpty())
            q.to = value;
         else if (key == _T("cc") && !value.IsEmpty())
            q.cc = value;
         else if (key == _T("bcc") && !value.IsEmpty())
            q.bcc = value;
         else if (key == _T("subject") && !value.IsEmpty())
            q.subject = value;
         else if (key == _T("in") && !value.IsEmpty())
            q.inFolder = value;
         else if (key == _T("has") && value == _T("attachment"))
            q.hasAttachment = true;
         else if (key == _T("has") && value == _T("link"))
            q.hasLink = true;
         else if (key == _T("filename") && !value.IsEmpty())
            q.filename = value;
         else if (key == _T("in_reply_to") && !value.IsEmpty())
         {
            // A Message-ID, with or without the angle brackets it is written
            // in; looked for in the brackets, so that one id is not the head
            // of another.
            String id = value;
            id.Replace(_T("<"), _T(""));
            id.Replace(_T(">"), _T(""));
            if (!id.IsEmpty())
               q.inReplyTo = _T("<") + id + _T(">");
         }
         else if (key == _T("larger") && ParseSize(value) >= 0)
            q.largerThan = ParseSize(value);
         else if (key == _T("smaller") && ParseSize(value) >= 0)
            q.smallerThan = ParseSize(value);
         else if (key == _T("before") && IsDay(value))
            q.before = value;
         else if (key == _T("after") && IsDay(value))
            q.after = value;
         else if (key == _T("older_than") && !MomentAgo(value).IsEmpty())
            q.olderThan = MomentAgo(value);
         else if (key == _T("newer_than") && !MomentAgo(value).IsEmpty())
            q.newerThan = MomentAgo(value);
         else if (key == _T("is") && value == _T("unread"))
            q.wantUnread = true;
         else if (key == _T("is") && value == _T("read"))
            q.wantRead = true;
         else if (key == _T("is") && value == _T("flagged"))
            q.wantFlagged = true;
         else if (key == _T("is") && value == _T("unflagged"))
            q.wantUnflagged = true;
         else if (key == _T("is") && value == _T("answered"))
            q.wantAnswered = true;
         else if (key == _T("is") && value == _T("muted"))
            q.wantMuted = true;
         else if (key == _T("is") && value == _T("pinned"))
            q.wantPinned = true;
         else if (key == _T("label") && !value.IsEmpty())
            q.labels.push_back(value);
         else if (key == _T("category") && IsCategory(value))
            q.category = AnsiString(value);
         else
         {
            // Each word once, and sixteen at most: every word past the
            // subject and the sender costs a read of the message, and a
            // request head holds thirty thousand of them.
            bool seen = false;
            for (size_t k = 0; k < q.terms.size() && !seen; k++)
               seen = q.terms[k] == lower;
            if (!seen && q.terms.size() < 16)
            {
               q.terms.push_back(lower);
               if (!q.freeText.IsEmpty())
                  q.freeText += _T(" ");
               q.freeText += token;
            }
         }
      }

      SearchQuery ParseSearchQuery(const String &text)
      {
         SearchQuery q;
         std::vector<String> tokens;

         // Whitespace separates tokens; a quoted stretch, on its own or after
         // an operator, is one token with its spaces.
         String current;
         bool quoted = false;
         for (int i = 0; i < text.GetLength(); i++)
         {
            wchar_t c = text[i];
            if (c == '"')
            {
               quoted = !quoted;
               continue;
            }
            if (!quoted && (c == ' ' || c == '\t' || c == '\r' || c == '\n'))
            {
               if (!current.IsEmpty())
                  tokens.push_back(current);
               current = _T("");
               continue;
            }
            current += c;
         }
         if (!current.IsEmpty())
            tokens.push_back(current);

         // OR, in capitals as Gmail has it, joins the term before it and the
         // term after it - and on through "a OR b OR c" - into one group of
         // choices; "or" in small letters is a word. An OR with nothing on
         // one side joins nothing and is dropped.
         const String Or = _T("OR");
         size_t i = 0;
         while (i < tokens.size())
         {
            if (i + 2 < tokens.size() && tokens[i + 1] == Or && tokens[i] != Or && tokens[i + 2] != Or)
            {
               std::vector<SearchQuery> choices;
               SearchQuery first;
               ApplyTerm(first, tokens[i]);
               choices.push_back(first);
               i++;
               while (i + 1 < tokens.size() && tokens[i] == Or && tokens[i + 1] != Or)
               {
                  SearchQuery next;
                  ApplyTerm(next, tokens[i + 1]);
                  choices.push_back(next);
                  i += 2;
               }
               if (SubQueryCount(q) + choices.size() <= MaxSubQueries)
                  q.either.push_back(choices);
               continue;
            }

            if (tokens[i] != Or)
               ApplyTerm(q, tokens[i]);
            i++;
         }

         return q;
      }

      // The listing's classifier, defined with the listing's extras below
      // in this same unnamed namespace; category: asks it, so the search
      // and the tabs never disagree.
      AnsiString CategoryOf(const MimeHeader &header);

      // Whether one message answers the query. What the row says is asked
      // first, then what the head of the file says; the body is read only
      // when a word was not in the subject or the sender, or to: or
      // filename: is asked, and only within the size the listing reads
      // bodies at all. The folder path is empty in a folder listing, where
      // in: has nothing to narrow.
      bool MessageMatchesQuery(const String &fileName, std::shared_ptr<Message> message, const String &folderPathLower, const SearchQuery &q, const SearchIndexPrune &prune)
      {
         if (!q.inFolder.IsEmpty() && !folderPathLower.IsEmpty() && folderPathLower.Find(q.inFolder) < 0)
            return false;
         if (q.wantUnread && message->GetFlagSeen())
            return false;
         if (q.wantRead && !message->GetFlagSeen())
            return false;
         if (q.wantFlagged && !message->GetFlagFlagged())
            return false;
         if (q.wantUnflagged && message->GetFlagFlagged())
            return false;
         if (q.wantAnswered && !message->GetFlagAnswered())
            return false;
         // The keywords the page keeps a muted thread and a pinned message
         // under (Portal.js, MUTE and PIN).
         if (q.wantMuted && !message->HasKeyword(_T("$Muted")))
            return false;
         if (q.wantPinned && !message->HasKeyword(_T("$Pinned")))
            return false;
         for (size_t i = 0; i < q.labels.size(); i++)
         {
            if (!message->HasKeyword(q.labels[i]))
               return false;
         }

         if (q.largerThan >= 0 && (__int64) message->GetSize() <= q.largerThan)
            return false;
         if (q.smallerThan >= 0 && (__int64) message->GetSize() >= q.smallerThan)
            return false;

         if (!q.before.IsEmpty() || !q.after.IsEmpty())
         {
            String day = message->GetCreateTime().Mid(0, 10);
            if (!q.before.IsEmpty() && day >= q.before)
               return false;
            if (!q.after.IsEmpty() && day <= q.after)
               return false;
         }

         // older_than: and newer_than: against the same clock before: and
         // after: read, to the second.
         if (!q.olderThan.IsEmpty() || !q.newerThan.IsEmpty())
         {
            const String when = message->GetCreateTime();
            if (!q.olderThan.IsEmpty() && when >= q.olderThan)
               return false;
            if (!q.newerThan.IsEmpty() && when < q.newerThan)
               return false;
         }

         // The head of the file, read and parsed once: Subject and From for
         // the words and their operators, Cc and Bcc for theirs, the tab for
         // category:, and the shape of the body for has:attachment.
         AnsiString header = PersistentMessage::LoadHeader(fileName, false);
         MimeHeader mimeHeader;
         if (!header.IsEmpty())
            mimeHeader.Load(header.c_str(), header.GetLength(), true);
         const String subject = mimeHeader.GetUnicodeFieldValue("Subject");
         const String from = mimeHeader.GetUnicodeFieldValue("From");

         if (!q.from.IsEmpty() && !ContainsNoCase(from, q.from))
            return false;
         if (!q.subject.IsEmpty() && !ContainsNoCase(subject, q.subject))
            return false;
         if (!q.cc.IsEmpty() && !ContainsNoCase(mimeHeader.GetUnicodeFieldValue("Cc"), q.cc))
            return false;
         // Bcc is in the file only where the sender's own copy kept it.
         if (!q.bcc.IsEmpty() && !ContainsNoCase(mimeHeader.GetUnicodeFieldValue("Bcc"), q.bcc))
            return false;
         // in_reply_to: the message-id in In-Reply-To or in References - a
         // reply to that message, or a later turn of its conversation.
         if (!q.inReplyTo.IsEmpty() && !ContainsNoCase(mimeHeader.GetUnicodeFieldValue("In-Reply-To"), q.inReplyTo) && !ContainsNoCase(mimeHeader.GetUnicodeFieldValue("References"), q.inReplyTo))
            return false;
         // The tab the listing gives the message, by the same rule.
         if (!q.category.IsEmpty() && CategoryOf(mimeHeader) != q.category)
            return false;

         if (q.hasAttachment)
         {
            // What the head of the file says: a mixed multipart, or a part
            // declared an attachment, is what a mail client shows a clip for.
            AnsiString lower = header;
            lower.ToLower();
            if (lower.Find("multipart/mixed") < 0 && lower.Find("content-disposition: attachment") < 0)
               return false;
         }

         // The choices and the exclusions, each asked of the same message as
         // a query of its own, before the body is read for this one. The
         // index is not consulted for them: it answers for the words that
         // must all be present, not for one of several, nor for one that
         // must be absent. What cannot be read is not found, for these as
         // for a word: a message too large to read matches no choice that
         // needs its body, and is kept by an exclusion that would.
         if (!q.either.empty() || !q.excluded.empty())
         {
            const SearchIndexPrune noPrune(0, String());
            for (size_t g = 0; g < q.either.size(); g++)
            {
               bool any = false;
               for (size_t k = 0; k < q.either[g].size() && !any; k++)
                  any = MessageMatchesQuery(fileName, message, folderPathLower, q.either[g][k], noPrune);
               if (!any)
                  return false;
            }
            for (size_t i = 0; i < q.excluded.size(); i++)
            {
               if (MessageMatchesQuery(fileName, message, folderPathLower, q.excluded[i], noPrune))
                  return false;
            }
         }

         std::vector<String> pending;
         for (size_t i = 0; i < q.terms.size(); i++)
            if (!ContainsNoCase(subject, q.terms[i]) && !ContainsNoCase(from, q.terms[i]))
               pending.push_back(q.terms[i]);

         if (pending.empty() && q.to.IsEmpty() && q.filename.IsEmpty() && !q.hasLink)
            return true;

         if (message->GetSize() > MaxMessageBodyBytes)
            return false;

         if (!pending.empty() && prune.CannotContain(message->GetID()))
            return false;

         MessageData data;
         if (!data.LoadFromMessage(fileName, message))
            return false;

         // Each part decoded and lower-cased once, however many words ask.
         const String to = ToLowerCopy(data.GetTo());
         const String cc = ToLowerCopy(data.GetCC());
         if (!q.to.IsEmpty() && to.Find(q.to) < 0 && cc.Find(q.to) < 0)
            return false;

         if (!q.filename.IsEmpty())
         {
            // The name of any part the message carries as an attachment,
            // as the download names it.
            std::shared_ptr<Attachments> attachments = data.GetAttachments();
            bool named = false;
            for (size_t i = 0; attachments && i < attachments->GetCount() && !named; i++)
            {
               std::shared_ptr<Attachment> attachment = attachments->GetItem((unsigned int) i);
               named = attachment && ContainsNoCase(attachment->GetFileName(), q.filename);
            }
            if (!named)
               return false;
         }

         if (pending.empty() && !q.hasLink)
            return true;

         const String body = ToLowerCopy(data.GetBody());
         const String html = ToLowerCopy(data.GetHTMLBody());
         // has:link: an address in the text, or an anchor to one in the HTML,
         // read from the same two parts the words are.
         if (q.hasLink && !HasWebLink(body, html))
            return false;

         for (size_t i = 0; i < pending.size(); i++)
         {
            const String &term = pending[i];
            if (to.Find(term) < 0 && cc.Find(term) < 0 && body.Find(term) < 0 && html.Find(term) < 0)
               return false;
         }

         return true;
      }
   }

   // The right an account has on a folder, asked the way IMAP asks it: on the
   // account's own tree and the public tree it is CheckPermission (everything
   // allowed when enforcement is off); on another account's folder it is a
   // right that account granted, and with enforcement off nothing is granted.
   bool
   RestApiServer::RightOn_(std::shared_ptr<const Account> account, std::shared_ptr<IMAPFolder> folder, int permission)
   {
      if (!folder)
         return false;

      if (folder->GetAccountID() != 0 && folder->GetAccountID() != account->GetID())
         return ACLManager::CheckDelegatedRight(account->GetID(), folder, permission);

      return ACLManager::CheckPermission(account->GetID(), folder, permission);
   }

   // The folder the id names, if the account may read it: in its own tree,
   // in the public tree, or in the tree of an owner who has shared with it -
   // the three places IMAP LIST looks, under the same rights.
   std::shared_ptr<IMAPFolder>
   RestApiServer::FindReadableFolder_(std::shared_ptr<const Account> account, __int64 folderId)
   {
      std::vector<std::shared_ptr<IMAPFolders>> trees;

      trees.push_back(IMAPFolderContainer::Instance()->GetFoldersForAccount(account->GetID()));
      trees.push_back(IMAPFolderContainer::Instance()->GetPublicFolders());

      if (ACLManager::GetOtherUsersNamespaceEnabled())
      {
         ACLManager aclManager;
         std::vector<__int64> owners = aclManager.GetAccountsWithFolderShares();
         for (size_t i = 0; i < owners.size(); i++)
         {
            if (owners[i] != account->GetID())
               trees.push_back(IMAPFolderContainer::Instance()->GetFoldersForAccount(owners[i]));
         }
      }

      for (size_t i = 0; i < trees.size(); i++)
      {
         if (!trees[i])
            continue;

         std::shared_ptr<IMAPFolder> folder = trees[i]->GetItemByDBIDRecursive(folderId);
         if (!folder)
            continue;

         if (!RightOn_(account, folder, ACLPermission::PermissionLookup))
            return std::shared_ptr<IMAPFolder>();

         bool readAccess = false;
         bool writeAccess = false;
         ACLManager::GetReadWriteAccess(account->GetID(), folder, readAccess, writeAccess);

         return readAccess ? folder : std::shared_ptr<IMAPFolder>();
      }

      return std::shared_ptr<IMAPFolder>();
   }

   // Where a message's bytes are: under the account that owns it, or in the
   // public folder store when no account does. Never under the caller.
   String
   RestApiServer::MessageFile_(std::shared_ptr<const Message> message)
   {
      if (message->GetAccountID() > 0)
      {
         std::shared_ptr<const Account> owner = CacheContainer::Instance()->GetAccount(message->GetAccountID());
         if (!owner)
            return String();

         return PersistentMessage::GetFileName(owner, message);
      }

      return PersistentMessage::GetFileName(message);
   }

   void
   RestApiServer::AppendFolderJson_(std::shared_ptr<const Account> account, std::shared_ptr<IMAPFolders> folders,
                                    const String &parentPath, const std::map<__int64, int> &designations,
                                    const String &delimiter, AnsiString &json, int depth)
   {
      // Depth-first, the way IMAP LIST walks the same tree. A tree deeper
      // than this is not one an IMAP client made, and it is not walked.
      if (!folders || depth > 32)
         return;

      int written = 0;
      for (int i = 0; i < folders->GetCount(); i++)
      {
         std::shared_ptr<IMAPFolder> folder = folders->GetItem(i);
         if (!folder)
            continue;

         // A folder the account may not look up or read is left out with its
         // subtree, exactly as LIST leaves it out.
         if (!RightOn_(account, folder, ACLPermission::PermissionLookup))
            continue;

         bool readAccess = false;
         bool writeAccess = false;
         ACLManager::GetReadWriteAccess(account->GetID(), folder, readAccess, writeAccess);
         if (!readAccess)
            continue;

         String path = parentPath.IsEmpty() ? folder->GetFolderName() : parentPath + delimiter + folder->GetFolderName();

         if (written > 0)
            json += ",";

         AppendOneFolderJson_(account, folder, path, designations, delimiter, writeAccess, json, depth);

         written++;
      }
   }

   // A stored folder name as a person should read it. IMAP and COM store the
   // name in modified UTF-7 - "&AMQ-renden" for "Ärenden" - and that is the form
   // to decode. But modified UTF-7 cannot contain a character above 127 by
   // construction, so a stored name that has one was never encoded: it is real
   // Unicode, put there by something that did not encode (an importer, or this
   // release's own create route before it learned to). Handing THAT to the
   // decoder means an AnsiString conversion first, which is lossy above ASCII -
   // the name would come back as question marks. So it is returned as it is.
   String
   RestApiServer::DecodeFolderName_(const String &stored)
   {
      for (int i = 0; i < stored.GetLength(); i++)
      {
         if (stored[i] > 127)
            return stored;
      }

      return ModifiedUTF7::Decode(AnsiString(stored));
   }

   void
   RestApiServer::AppendOneFolderJson_(std::shared_ptr<const Account> account, std::shared_ptr<IMAPFolder> folder,
                                       const String &path, const std::map<__int64, int> &designations,
                                       const String &delimiter, bool writeAccess, AnsiString &json, int depth)
   {
      if (!folder)
         return;

      std::shared_ptr<Messages> messages = folder->GetMessages();
      long messageCount = messages ? messages->GetCount() : 0;
      long seen = messages ? messages->GetNoOfSeen() : 0;

      std::map<__int64, int>::const_iterator designation = designations.find(folder->GetID());
      String specialUse = designation != designations.end() ? IMAPSpecialUse::FormatDesignations(designation->second) : String();

      AnsiString entry;
      entry.Format("{\"id\":%I64d,\"account_id\":%I64d,\"name\":\"%hs\",\"path\":\"%hs\",\"parent_id\":%I64d,\"special_use\":\"%hs\",\"subscribed\":%hs,\"writable\":%hs,\"messages\":%ld,\"unseen\":%ld,\"uidvalidity\":%u,\"uidnext\":%I64d,\"subfolders\":[",
         folder->GetID(),
         folder->GetAccountID(),
         // Decoded, as InterfaceIMAPFolder::get_Name decodes it for COM. A
         // folder name is stored in modified UTF-7 - "&AMQ-renden" for
         // "Ärenden" - which is the form IMAP puts on the wire and no form
         // any other caller wants: a page that shows this to a reader shows
         // them the encoding. The path is the same names joined by the
         // delimiter, and decoding it whole is safe because an encoded run
         // begins with '&' and ends with '-', neither of which a delimiter is.
         JsonEscape_(Utf8_(DecodeFolderName_(folder->GetFolderName()))).c_str(),
         JsonEscape_(Utf8_(DecodeFolderName_(path))).c_str(),
         folder->GetParentFolderID(),
         JsonEscape_(Utf8_(specialUse)).c_str(),
         folder->GetIsSubscribed() ? "true" : "false",
         writeAccess ? "true" : "false",
         messageCount,
         messageCount - seen,
         // UIDVALIDITY is the folder's creation time, as SELECT and STATUS
         // report it; UIDNEXT is the UID the next message will be given, which
         // STATUS reports as the folder's current UID plus one. The two are
         // different numbers, and the current UID - COM's IMAPFolder.CurrentUID,
         // which delivery moves on - is only readable from the second.
         folder->GetCreationTime().ToInt(),
         (__int64) folder->GetCurrentUID() + 1);
      json += entry;

      AppendFolderJson_(account, folder->GetSubFolders(), path, designations, delimiter, json, depth + 1);

      json += "]}";
   }

   HttpResponse
   RestApiServer::HandleMeFolders_(const Caller &caller)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      String delimiter = Configuration::Instance()->GetIMAPConfiguration()->GetHierarchyDelimiter();

      std::shared_ptr<IMAPFolders> folders = IMAPFolderContainer::Instance()->GetFoldersForAccount(account->GetID());

      std::map<__int64, int> designations;
      if (folders)
         IMAPSpecialUse::Resolve(folders, designations);

      AnsiString json;
      json.Format("{\"delimiter\":\"%hs\",\"folders\":[", JsonEscape_(Utf8_(delimiter)).c_str());
      AppendFolderJson_(account, folders, String(), designations, delimiter, json, 0);
      json += "],\"shared\":[";

      // What LIST shows beyond the account's own tree, named as LIST names
      // it: the public folders under their namespace, then the folders of
      // each owner who has granted this account a right, under "#Users" and
      // the owner's address. An owner who granted nothing this account may
      // read produces no entry, and so no trace.
      int shares = 0;

      std::shared_ptr<IMAPFolders> publicFolders = IMAPFolderContainer::Instance()->GetPublicFolders();
      if (publicFolders)
      {
         String publicName = Configuration::Instance()->GetIMAPConfiguration()->GetIMAPPublicFolderName();

         AnsiString tree;
         std::map<__int64, int> none;
         AppendFolderJson_(account, publicFolders, publicName, none, delimiter, tree, 0);

         if (!tree.IsEmpty())
         {
            AnsiString entry;
            entry.Format("{\"owner\":\"%hs\",\"folders\":[", JsonEscape_(Utf8_(publicName)).c_str());
            json += entry + tree + "]}";
            shares++;
         }
      }

      if (ACLManager::GetOtherUsersNamespaceEnabled())
      {
         String otherUsersName = ACLManager::GetOtherUsersFolderName();

         ACLManager aclManager;
         std::vector<__int64> owners = aclManager.GetAccountsWithFolderShares();

         for (size_t i = 0; i < owners.size(); i++)
         {
            if (owners[i] == account->GetID())
               continue;

            std::shared_ptr<const Account> owner = CacheContainer::Instance()->GetAccount(owners[i]);
            if (!owner)
               continue;

            std::shared_ptr<IMAPFolders> ownerFolders = IMAPFolderContainer::Instance()->GetFoldersForAccount(owners[i]);
            if (!ownerFolders)
               continue;

            std::map<__int64, int> ownerDesignations;
            IMAPSpecialUse::Resolve(ownerFolders, ownerDesignations);

            AnsiString tree;
            AppendFolderJson_(account, ownerFolders, otherUsersName + delimiter + owner->GetAddress(), ownerDesignations, delimiter, tree, 0);

            if (tree.IsEmpty())
               continue;

            if (shares > 0)
               json += ",";

            AnsiString entry;
            entry.Format("{\"owner\":\"%hs\",\"folders\":[", JsonEscape_(Utf8_(owner->GetAddress())).c_str());
            json += entry + tree + "]}";
            shares++;
         }
      }

      json += "]}";

      return BuildResponse_(200, json);
   }

   HttpResponse
   RestApiServer::HandleMeFolderMessages_(const Caller &caller, __int64 folderId, const AnsiString &query)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      std::shared_ptr<IMAPFolder> folder = FindReadableFolder_(account, folderId);
      if (!folder)
         return BuildResponse_(404, "{\"error\":\"folder not found\"}");

      int limit = MaxMessagesPerPage;
      AnsiString limitText = QueryParameter_(query, "limit");
      if (!limitText.IsEmpty())
      {
         limit = atoi(limitText.c_str());
         if (limit < 1 || limit > MaxMessagesPerPage)
            limit = MaxMessagesPerPage;
      }

      unsigned int beforeUid = 0;
      AnsiString beforeText = QueryParameter_(query, "before_uid");
      if (!beforeText.IsEmpty())
         beforeUid = (unsigned int) strtoul(beforeText.c_str(), nullptr, 10);

      // q narrows the listing to the messages containing the text.
      String text;
      Unicode::MultiByteToWide(QueryParameter_(query, "q"), text);
      text.TrimLeft();
      text.TrimRight();
      const SearchQuery searchQuery = ParseSearchQuery(text);
      const bool searching = !text.IsEmpty();
      SearchIndexPrune prune(account->GetID(), searchQuery.freeText);

      // A snapshot, walked newest UID first: the collection is shared with
      // every IMAP session on the mailbox, and a copy is what lets this read
      // files without holding its lock.
      std::shared_ptr<Messages> messages = folder->GetMessages();
      std::vector<std::shared_ptr<Message>> snapshot = messages ? messages->GetCopy() : std::vector<std::shared_ptr<Message>>();

      int total = 0;
      for (std::vector<std::shared_ptr<Message>>::const_iterator it = snapshot.begin(); it != snapshot.end(); ++it)
      {
         if (*it && !(*it)->GetFlagDeleted())
            total++;
      }

      AnsiString json;
      json.Format("{\"folder_id\":%I64d,\"total\":%d,\"query\":\"%hs\",\"messages\":[", folder->GetID(), total, JsonEscape_(Utf8_(text)).c_str());

      int written = 0;
      int scanned = 0;
      bool complete = true;
      unsigned int lastUid = 0;

      for (std::vector<std::shared_ptr<Message>>::reverse_iterator it = snapshot.rbegin(); it != snapshot.rend(); ++it)
      {
         std::shared_ptr<Message> message = *it;
         if (!message || message->GetFlagDeleted())
            continue;

         if (beforeUid > 0 && message->GetUID() >= beforeUid)
            continue;

         if (written >= limit)
         {
            // A page is full. For a search that is a place to continue from;
            // a plain listing pages by before_uid as before.
            if (searching)
               complete = false;
            break;
         }

         const String fileName = MessageFile_(message);

         if (searching)
         {
            if (scanned >= MaxSearchScan)
            {
               complete = false;
               break;
            }

            scanned++;
            lastUid = message->GetUID();

            if (!MessageMatchesQuery(fileName, message, String(), searchQuery, prune))
               continue;
         }

         AnsiString subject, from, date;
         DescribeHeader(fileName, subject, from, date);

         if (written > 0)
            json += ",";

         AnsiString entry;
         entry.Format("{\"id\":%I64d,\"uid\":%u,\"size\":%d,\"received\":\"%hs\",\"subject\":\"%hs\",\"from\":\"%hs\",\"date\":\"%hs\",\"flags\":%hs,%hs,%hs}",
            message->GetID(),
            message->GetUID(),
            message->GetSize(),
            JsonEscape_(Utf8_(message->GetCreateTime())).c_str(),
            JsonEscape_(subject).c_str(),
            JsonEscape_(from).c_str(),
            JsonEscape_(date).c_str(),
            FlagsJson(message).c_str(),
            ThreadFieldsJson_(MessageFile_(message)).c_str(),
            ListingExtrasJson_(MessageFile_(message), message).c_str());
         json += entry;
         written++;
         lastUid = message->GetUID();
      }

      AnsiString tail;
      tail.Format("],\"scanned\":%d,\"complete\":%hs,\"next_before_uid\":%u}", scanned, complete ? "true" : "false", complete ? 0 : lastUid);
      json += tail;

      return BuildResponse_(200, json);
   }

   // Every folder of the account's own tree the account may read, with its
   // path, depth-first as the tree lists them.
   void
   RestApiServer::CollectReadableFolders_(std::shared_ptr<const Account> account, std::shared_ptr<IMAPFolders> folders, const String &parentPath,
                                          const String &delimiter, std::vector<std::pair<std::shared_ptr<IMAPFolder>, String>> &out, int depth)
   {
      if (!folders || depth > 32)
         return;

      for (int i = 0; i < folders->GetCount(); i++)
      {
         std::shared_ptr<IMAPFolder> folder = folders->GetItem(i);
         if (!folder)
            continue;

         bool readAccess = false;
         bool writeAccess = false;
         ACLManager::GetReadWriteAccess(account->GetID(), folder, readAccess, writeAccess);
         if (!readAccess)
            continue;

         String path = parentPath.IsEmpty() ? folder->GetFolderName() : parentPath + delimiter + folder->GetFolderName();
         out.push_back(std::make_pair(folder, path));

         CollectReadableFolders_(account, folder->GetSubFolders(), path, delimiter, out, depth + 1);
      }
   }

   HttpResponse
   RestApiServer::HandleMeSearch_(const Caller &caller, const AnsiString &query)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      String text;
      Unicode::MultiByteToWide(QueryParameter_(query, "q"), text);
      text.TrimLeft();
      text.TrimRight();
      if (text.IsEmpty())
         return BuildResponse_(400, "{\"error\":\"q is required\"}");

      int limit = 50;
      AnsiString limitText = QueryParameter_(query, "limit");
      if (!limitText.IsEmpty())
      {
         limit = atoi(limitText.c_str());
         if (limit < 1 || limit > MaxMessagesPerPage)
            limit = MaxMessagesPerPage;
      }

      const SearchQuery searchQuery = ParseSearchQuery(text);
      SearchIndexPrune prune(account->GetID(), searchQuery.freeText);

      String delimiter = Configuration::Instance()->GetIMAPConfiguration()->GetHierarchyDelimiter();

      std::vector<std::pair<std::shared_ptr<IMAPFolder>, String>> folders;
      CollectReadableFolders_(account, IMAPFolderContainer::Instance()->GetFoldersForAccount(account->GetID()), String(), delimiter, folders, 0);

      struct Hit
      {
         std::shared_ptr<Message> message;
         std::shared_ptr<IMAPFolder> folder;
         String path;
      };

      std::vector<Hit> hits;
      int scanned = 0;
      bool complete = true;

      // Every folder, newest first within each, under one budget for the
      // whole request.
      for (size_t f = 0; f < folders.size() && complete; f++)
      {
         std::shared_ptr<Messages> messages = folders[f].first->GetMessages();
         if (!messages)
            continue;

         const String pathLower = ToLowerCopy(DecodeFolderName_(folders[f].second));
         // in:folder: a folder whose path does not match is skipped whole,
         // before its messages count against the budget.
         if (!searchQuery.inFolder.IsEmpty() && pathLower.Find(searchQuery.inFolder) < 0)
            continue;
         std::vector<std::shared_ptr<Message>> snapshot = messages->GetCopy();
         for (std::vector<std::shared_ptr<Message>>::reverse_iterator it = snapshot.rbegin(); it != snapshot.rend(); ++it)
         {
            std::shared_ptr<Message> message = *it;
            if (!message || message->GetFlagDeleted())
               continue;

            if (scanned >= MaxSearchScan)
            {
               complete = false;
               break;
            }

            scanned++;

            if (!MessageMatchesQuery(MessageFile_(message), message, pathLower, searchQuery, prune))
               continue;

            Hit hit;
            hit.message = message;
            hit.folder = folders[f].first;
            hit.path = folders[f].second;
            hits.push_back(hit);
         }
      }

      // Newest first across folders: ids grow with time.
      std::sort(hits.begin(), hits.end(), [](const Hit &a, const Hit &b) { return a.message->GetID() > b.message->GetID(); });

      bool more = (int) hits.size() > limit;
      if (more)
         hits.resize(limit);

      AnsiString json;
      json.Format("{\"query\":\"%hs\",\"scanned\":%d,\"complete\":%hs,\"more\":%hs,\"messages\":[",
         JsonEscape_(Utf8_(text)).c_str(), scanned, complete ? "true" : "false", more ? "true" : "false");

      for (size_t i = 0; i < hits.size(); i++)
      {
         std::shared_ptr<Message> message = hits[i].message;

         AnsiString subject, from, date;
         DescribeHeader(MessageFile_(message), subject, from, date);

         if (i > 0)
            json += ",";

         AnsiString entry;
         entry.Format("{\"folder_id\":%I64d,\"folder\":\"%hs\",\"id\":%I64d,\"uid\":%u,\"size\":%d,\"received\":\"%hs\",\"subject\":\"%hs\",\"from\":\"%hs\",\"date\":\"%hs\",\"flags\":%hs,%hs,%hs}",
            hits[i].folder->GetID(),
            // Decoded, as the folder listing decodes it: the path is stored
            // names joined, and a stored name is modified UTF-7.
            JsonEscape_(Utf8_(DecodeFolderName_(hits[i].path))).c_str(),
            message->GetID(),
            message->GetUID(),
            message->GetSize(),
            JsonEscape_(Utf8_(message->GetCreateTime())).c_str(),
            JsonEscape_(subject).c_str(),
            JsonEscape_(from).c_str(),
            JsonEscape_(date).c_str(),
            FlagsJson(message).c_str(),
            ThreadFieldsJson_(MessageFile_(message)).c_str(),
            ListingExtrasJson_(MessageFile_(message), message).c_str());
         json += entry;
      }

      json += "]}";
      return BuildResponse_(200, json);
   }

   HttpResponse
   RestApiServer::HandleMeMessage_(const Caller &caller, __int64 messageId)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      // The row first, then the folder it is in, which is what decides
      // whether this account may read it. Another account's message, or one
      // in a folder the ACL keeps from this account, is "not found": the id
      // space is shared, and a refusal that differed would say it exists.
      std::shared_ptr<Message> message = std::shared_ptr<Message>(new Message());
      if (!PersistentMessage::ReadObject(message, messageId) || message->GetID() == 0)
         return BuildResponse_(404, "{\"error\":\"message not found\"}");

      std::shared_ptr<IMAPFolder> folder = FindReadableFolder_(account, message->GetFolderID());
      if (!folder || folder->GetAccountID() != message->GetAccountID())
         return BuildResponse_(404, "{\"error\":\"message not found\"}");

      String fileName = MessageFile_(message);
      if (!FileUtilities::Exists(fileName))
         return BuildResponse_(404, "{\"error\":\"the message file is missing\"}");

      AnsiString subject, from, date;
      DescribeHeader(fileName, subject, from, date);

      AnsiString json;
      json.Format("{\"id\":%I64d,\"uid\":%u,\"folder_id\":%I64d,\"size\":%d,\"received\":\"%hs\",\"flags\":%hs,\"subject\":\"%hs\",\"from\":\"%hs\",\"date\":\"%hs\",",
         message->GetID(),
         message->GetUID(),
         message->GetFolderID(),
         message->GetSize(),
         JsonEscape_(Utf8_(message->GetCreateTime())).c_str(),
         FlagsJson(message).c_str(),
         JsonEscape_(subject).c_str(),
         JsonEscape_(from).c_str(),
         JsonEscape_(date).c_str());
      json += ThreadFieldsJson_(fileName) + ",";
      json += HeaderFieldsJson_(fileName, account) + ",";

      if (message->GetSize() > MaxMessageParseBytes)
      {
         json += "\"truncated\":true,\"to\":\"\",\"cc\":\"\",\"text\":\"\",\"html\":\"\",\"html_remote\":false,\"invitation\":false,\"attachments\":[]}";
         return BuildResponse_(200, json);
      }

      // Between the two ceilings the message is parsed for its attachment
      // list, which the download route serves one at a time, and its text
      // is left out.
      bool bodyTooLarge = message->GetSize() > MaxMessageBodyBytes;

      MessageData messageData;
      if (!messageData.LoadFromMessage(MessageFile_(message), message))
         return BuildResponse_(500, "{\"error\":\"the message could not be parsed\"}");

      // The MIME parts behind the attachment list, in the very order
      // Attachments::Load walked them, so that index i here and index i there
      // are the same part. Attachment itself exposes neither the Content-ID nor
      // an unparameterised type, and adding accessors to a business object the
      // COM API also publishes is a wider change than reading the header the
      // list was built from.
      std::list<std::shared_ptr<MimeBody>> attachmentBodies;
      std::shared_ptr<MimeBody> mimeMessage = messageData.GetMimeMessage();
      if (mimeMessage)
         mimeMessage->GetAttachmentList(mimeMessage, attachmentBodies);

      std::vector<std::shared_ptr<MimeBody>> attachmentParts(attachmentBodies.begin(), attachmentBodies.end());

      AnsiString attachments = "[";
      std::shared_ptr<Attachments> attachmentList = messageData.GetAttachments();
      if (attachmentList)
      {
         for (size_t i = 0; i < attachmentList->GetCount(); i++)
         {
            std::shared_ptr<Attachment> attachment = attachmentList->GetItem((unsigned int) i);
            if (!attachment)
               continue;

            if (i > 0)
               attachments += ",";

            // The declared media type without its parameters, and the
            // Content-ID with the angle brackets RFC 2392 requires in the
            // header and forbids in a cid: URL taken off - which is the form a
            // page needs to match a cid: reference in the HTML body against
            // this list. Both are the empty string when the part declares
            // neither.
            AnsiString contentType = MediaType(attachment->GetContentType());
            AnsiString contentId = i < attachmentParts.size() ? ContentId(attachmentParts[i]) : AnsiString();

            AnsiString entry;
            entry.Format("{\"index\":%d,\"name\":\"%hs\",\"size\":%d,\"content_type\":\"%hs\",\"content_id\":\"%hs\"}",
               (int) i, JsonEscape_(Utf8_(attachment->GetFileName())).c_str(), AttachmentSize(attachment, message->GetSize() <= MaxMessageBodyBytes),
               JsonEscape_(contentType).c_str(), JsonEscape_(contentId).c_str());
            attachments += entry;
         }
      }
      attachments += "]";

      AnsiString tail;
      AnsiString text = bodyTooLarge ? AnsiString() : JsonEscape_(Utf8_(messageData.GetBody()));
      AnsiString html = bodyTooLarge ? AnsiString() : JsonEscape_(Utf8_(messageData.GetHTMLBody()));
      bool htmlRemote = !bodyTooLarge && HtmlNamesRemoteContent_(Utf8_(messageData.GetHTMLBody()));

      // An invitation: a text/calendar part, or an .ics attachment. The page
      // asks /invitation for what it says only when this is true.
      bool invitation = false;
      if (!bodyTooLarge && mimeMessage)
      {
         std::vector<std::shared_ptr<MimeBody>> stack;
         stack.push_back(mimeMessage);
         while (!stack.empty() && !invitation)
         {
            std::shared_ptr<MimeBody> part = stack.back();
            stack.pop_back();
            AnsiString type = part->GetContentType() ? part->GetContentType() : "";
            type.ToLower();
            AnsiString name = AnsiString(part->GetName().c_str());
            name.ToLower();
            if (type == "text/calendar" || name.EndsWith(".ics"))
               invitation = true;
            else if (part->IsMultiPart())
            {
               std::shared_ptr<MimeBody> child = part->FindFirstPart();
               while (child)
               {
                  stack.push_back(child);
                  child = part->FindNextPart();
               }
            }
         }
      }

      tail.Format("\"truncated\":%hs,\"to\":\"%hs\",\"cc\":\"%hs\",\"text\":\"%hs\",\"html\":\"%hs\",\"html_remote\":%hs,\"invitation\":%hs,\"attachments\":%hs}",
         bodyTooLarge ? "true" : "false",
         JsonEscape_(Utf8_(messageData.GetTo())).c_str(),
         JsonEscape_(Utf8_(messageData.GetCC())).c_str(),
         text.c_str(),
         html.c_str(),
         htmlRemote ? "true" : "false",
         invitation ? "true" : "false",
         attachments.c_str());
      json += tail;

      return BuildResponse_(200, json);
   }

   namespace
   {
      // A bare number in a JSON body - folder_id - with or without quotes.
      bool JsonNumber(const AnsiString &json, const AnsiString &key, __int64 &value)
      {
         AnsiString needle = "\"" + key + "\"";
         int keyPosition = json.Find(needle);
         if (keyPosition < 0)
            return false;

         int colon = json.Find(":", keyPosition + needle.GetLength());
         if (colon < 0)
            return false;

         int i = colon + 1;
         while (i < json.GetLength() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\r' || json[i] == '\n' || json[i] == '\"'))
            i++;

         int start = i;
         while (i < json.GetLength() && json[i] >= '0' && json[i] <= '9')
            i++;

         if (i == start)
            return false;

         value = _atoi64(json.Mid(start, i - start).c_str());
         return true;
      }

      // Every IMAP session on the folder is told, the way STORE, MOVE and
      // EXPUNGE tell them: by message id, which each turns into its own
      // sequence numbers.
      void NotifyFolder(std::shared_ptr<IMAPFolder> folder, ChangeNotification::NotificationType type, const std::vector<__int64> &messageIds)
      {
         std::shared_ptr<ChangeNotification> notification =
            std::shared_ptr<ChangeNotification>(new ChangeNotification(folder->GetAccountID(), folder->GetID(), type, messageIds));

         Application::Instance()->GetNotificationServer()->SendNotification(notification);
      }
   }

   // The message the id names, if it is the signed-in account's and sits in
   // a folder the account may read - and the live object every IMAP session
   // on the mailbox shares, since what is changed here must be what they see.
   std::shared_ptr<Message>
   RestApiServer::FindOwnMessage_(std::shared_ptr<const Account> account, __int64 messageId, std::shared_ptr<IMAPFolder> &folder)
   {
      std::shared_ptr<Message> row = std::shared_ptr<Message>(new Message());
      if (!PersistentMessage::ReadObject(row, messageId) || row->GetID() == 0)
         return std::shared_ptr<Message>();

      // The folder decides: the account's own, or one it may read under a
      // right the owner granted - and the row must belong to that folder's
      // owner, so a folder id cannot be used to reach another store's row.
      folder = FindReadableFolder_(account, row->GetFolderID());
      if (!folder || folder->GetAccountID() != row->GetAccountID())
         return std::shared_ptr<Message>();

      std::shared_ptr<Messages> messages = folder->GetMessages();
      if (!messages)
         return std::shared_ptr<Message>();

      return messages->GetItemByDBID(messageId);
   }

   // The account's folder carrying a special-use designation - \Trash,
   // \Sent - if it has one: a CREATE USE designation, or the name a client
   // gave it.
   std::shared_ptr<IMAPFolder>
   RestApiServer::FindDesignatedFolder_(std::shared_ptr<const Account> account, int designation)
   {
      std::shared_ptr<IMAPFolders> folders = IMAPFolderContainer::Instance()->GetFoldersForAccount(account->GetID());
      if (!folders)
         return std::shared_ptr<IMAPFolder>();

      std::map<__int64, int> designations;
      IMAPSpecialUse::Resolve(folders, designations);

      for (std::map<__int64, int>::const_iterator it = designations.begin(); it != designations.end(); ++it)
      {
         if ((it->second & designation) == 0)
            continue;

         std::shared_ptr<IMAPFolder> folder = folders->GetItemByDBIDRecursive(it->first);
         if (folder)
            return folder;
      }

      return std::shared_ptr<IMAPFolder>();
   }

   // What EXPUNGE does for one message: the row and the file go, the
   // folder's shared collection drops it, and every session is told.
   bool
   RestApiServer::DeleteOwnMessage_(std::shared_ptr<const Account> account, std::shared_ptr<Message> message, std::shared_ptr<IMAPFolder> folder)
   {
      std::shared_ptr<Messages> messages = MessagesContainer::Instance()->GetMessages(folder->GetAccountID(), folder->GetID());
      if (!messages)
         return false;

      std::set<__int64> ids;
      ids.insert(message->GetID());

      RecoverableStore::Scope recoverable;   // a person deleting mail: kept for recovery
      std::vector<__int64> deleted = messages->DeleteMessagesById(ids);
      if (deleted.empty())
         return false;

      NotifyFolder(folder, ChangeNotification::NotificationMessageDeleted, deleted);
      return true;
   }

   HttpResponse
   RestApiServer::HandleMeMessageFlags_(const Caller &caller, __int64 messageId, const AnsiString &requestBody)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      std::shared_ptr<IMAPFolder> folder;
      std::shared_ptr<Message> message = FindOwnMessage_(account, messageId, folder);
      if (!message)
         return BuildResponse_(404, "{\"error\":\"message not found\"}");

      // Only the flags the body names change; the rest stay as they are, so
      // two clients touching different flags do not undo each other.
      static const char *names[] = { "seen", "flagged", "answered", "draft", "deleted" };
      bool named[5] = { false, false, false, false, false };
      bool values[5] = { false, false, false, false, false };
      int mentioned = 0;

      for (int i = 0; i < 5; i++)
      {
         AnsiString needle = AnsiString("\"") + names[i] + "\"";
         if (requestBody.Find(needle) < 0)
            continue;

         named[i] = true;
         values[i] = GetJsonBoolValue_(requestBody, names[i], false);
         mentioned++;
      }

      // Labels: keywords_add and keywords_remove, each an array of IMAP
      // keyword atoms; removed first, then added.
      std::vector<String> keywordsAdd;
      std::vector<String> keywordsRemove;
      {
         JsonValue document;
         std::string parseError;
         if (JsonValue::Parse(std::string(requestBody.c_str(), requestBody.size()), document, parseError) && document.IsObject())
         {
            const JsonValue *lists[2] = { document.Get("keywords_add"), document.Get("keywords_remove") };
            for (int l = 0; l < 2; l++)
            {
               if (!lists[l] || lists[l]->IsNull())
                  continue;
               if (!lists[l]->IsArray())
                  return BuildResponse_(400, "{\"error\":\"keywords_add and keywords_remove are arrays of keyword names\"}");
               for (size_t i = 0; i < lists[l]->Size(); i++)
               {
                  const JsonValue *item = lists[l]->At(i);
                  if (!item || !item->IsString())
                     return BuildResponse_(400, "{\"error\":\"keywords_add and keywords_remove are arrays of keyword names\"}");
                  String keyword;
                  Unicode::MultiByteToWide(AnsiString(item->AsString().c_str()), keyword);
                  keyword.TrimLeft();
                  keyword.TrimRight();
                  if (keyword.IsEmpty() || keyword[0] == '\\' || !SieveParser::IsValidFlagName(keyword))
                     return BuildResponse_(400, "{\"error\":\"a keyword is an IMAP atom: printable ASCII without spaces, parentheses, braces, brackets, quotes, %, * or a backslash\"}");
                  (l == 0 ? keywordsAdd : keywordsRemove).push_back(keyword);
               }
               mentioned++;
            }
         }
      }

      if (mentioned == 0)
         return BuildResponse_(400, "{\"error\":\"no flag named: seen, flagged, answered, draft or deleted; and no keywords_add or keywords_remove\"}");

      // The rights STORE asks for. RFC 4314 makes \Seen, \Deleted and the
      // other flags three separate permissions, and they are asked for
      // separately here too.
      if (named[0] && !RightOn_(account, folder, ACLPermission::PermissionWriteSeen))
         return BuildResponse_(403, "{\"error\":\"the folder does not allow this account to change the seen flag\"}");

      if (named[4] && !RightOn_(account, folder, ACLPermission::PermissionWriteDeleted))
         return BuildResponse_(403, "{\"error\":\"the folder does not allow this account to change the deleted flag\"}");

      if ((named[1] || named[2] || named[3] || !keywordsAdd.empty() || !keywordsRemove.empty()) && !RightOn_(account, folder, ACLPermission::PermissionWriteOthers))
         return BuildResponse_(403, "{\"error\":\"the folder does not allow this account to change flags\"}");

      if (named[0])
         message->SetFlagSeen(values[0]);
      if (named[1])
         message->SetFlagFlagged(values[1]);
      if (named[2])
         message->SetFlagAnswered(values[2]);
      if (named[3])
         message->SetFlagDraft(values[3]);
      if (named[4])
         message->SetFlagDeleted(values[4]);
      for (size_t i = 0; i < keywordsRemove.size(); i++)
         message->RemoveKeyword(keywordsRemove[i]);
      for (size_t i = 0; i < keywordsAdd.size(); i++)
      {
         if (!message->AddKeyword(keywordsAdd[i]))
            return BuildResponse_(400, "{\"error\":\"too many keywords on the message\"}");
      }

      // The path STORE takes: the flags are written with the folder's next
      // mod-sequence, so CONDSTORE and QRESYNC clients see the change.
      if (!Application::Instance()->GetFolderManager()->UpdateMessageFlags((int) folder->GetAccountID(), (int) folder->GetID(), message->GetID(), message->GetFlags(), message->GetKeywords()))
         return BuildResponse_(500, "{\"error\":\"the flags could not be stored\"}");

      std::vector<__int64> changed;
      changed.push_back(message->GetID());
      NotifyFolder(folder, ChangeNotification::NotificationMessageFlagsChanged, changed);

      AnsiString json;
      json.Format("{\"id\":%I64d,\"folder_id\":%I64d,\"flags\":%hs}", message->GetID(), folder->GetID(), FlagsJson(message).c_str());
      return BuildResponse_(200, json);
   }

   HttpResponse
   RestApiServer::HandleMeMessageMove_(const Caller &caller, __int64 messageId, const AnsiString &requestBody)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      std::shared_ptr<IMAPFolder> source;
      std::shared_ptr<Message> message = FindOwnMessage_(account, messageId, source);
      if (!message)
         return BuildResponse_(404, "{\"error\":\"message not found\"}");

      // The destination is a folder id, or one of the four the page has a key
      // for: archive, junk, trash - the folder designated so, made by that
      // name when the account has none, as Drafts is made - and inbox.
      std::shared_ptr<IMAPFolder> destination;
      String to = JsonUtf8Value_(requestBody, "to");
      to.ToLower();
      if (!to.IsEmpty())
      {
         if (to == _T("archive"))
            destination = DesignatedFolderOrCreate_(account, IMAPSpecialUse::DesignationArchive, _T("Archive"));
         else if (to == _T("junk"))
            destination = DesignatedFolderOrCreate_(account, IMAPSpecialUse::DesignationJunk, _T("Junk"));
         else if (to == _T("trash"))
            destination = DesignatedFolderOrCreate_(account, IMAPSpecialUse::DesignationTrash, _T("Trash"));
         else if (to == _T("inbox"))
         {
            std::shared_ptr<IMAPFolders> own = IMAPFolderContainer::Instance()->GetFoldersForAccount(account->GetID());
            if (own)
               destination = own->GetFolderByName(_T("INBOX"));
         }
         else
            return BuildResponse_(400, "{\"error\":\"to must be archive, junk, trash or inbox\"}");

         if (!destination)
            return BuildResponse_(500, "{\"error\":\"the folder could not be found or made\"}");
      }
      else
      {
         __int64 folderId = 0;
         if (!JsonNumber(requestBody, "folder_id", folderId))
            return BuildResponse_(400, "{\"error\":\"folder_id or to is required\"}");

         // The destination goes through the same test as any folder id: the
         // account's own tree, readable. Another account's folder is 404.
         destination = FindReadableFolder_(account, folderId);
         if (!destination)
            return BuildResponse_(404, "{\"error\":\"folder not found\"}");
      }

      if (destination->GetID() == source->GetID())
         return BuildResponse_(400, "{\"error\":\"the message is already in that folder\"}");

      if (destination->GetAccountID() != source->GetAccountID())
         return BuildResponse_(400, "{\"error\":\"a message moves within its own mailbox; copy it by other means\"}");

      // The rights MOVE asks for: insert there, mark deleted and expunge here.
      if (!RightOn_(account, destination, ACLPermission::PermissionInsert))
         return BuildResponse_(403, "{\"error\":\"the folder does not allow this account to add messages\"}");

      if (!RightOn_(account, source, ACLPermission::PermissionWriteDeleted) ||
          !RightOn_(account, source, ACLPermission::PermissionExpunge))
         return BuildResponse_(403, "{\"error\":\"the folder does not allow this account to remove messages\"}");

      // Copy, then expunge: the shape MOVE has, so the moved message is a
      // new row with a new UID in the destination and every session on
      // either folder is told. A copy that could not be followed by the
      // expunge is reported as exactly that, and the copy is left.
      __int64 newMessageId = 0;
      if (!MessageUtilities::CopyToIMAPFolder(message, (int) destination->GetID(), newMessageId))
         return BuildResponse_(500, "{\"error\":\"the message could not be copied to the folder\"}");

      if (!DeleteOwnMessage_(account, message, source))
         return BuildResponse_(500, "{\"error\":\"the message was copied to the folder but the original could not be removed\"}");

      // Into or out of Junk teaches spamd, as the same move over IMAP does.
      LearnAfterMove_(source, destination, newMessageId, account);

      AnsiString json;
      json.Format("{\"id\":%I64d,\"folder_id\":%I64d}", newMessageId, destination->GetID());
      return BuildResponse_(200, json);
   }

   HttpResponse
   RestApiServer::HandleMeMessageDelete_(const Caller &caller, __int64 messageId, const AnsiString &query)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      std::shared_ptr<IMAPFolder> folder;
      std::shared_ptr<Message> message = FindOwnMessage_(account, messageId, folder);
      if (!message)
         return BuildResponse_(404, "{\"error\":\"message not found\"}");

      // The rights EXPUNGE asks for, since that is what this is.
      if (!RightOn_(account, folder, ACLPermission::PermissionWriteDeleted) ||
          !RightOn_(account, folder, ACLPermission::PermissionExpunge))
         return BuildResponse_(403, "{\"error\":\"the folder does not allow this account to delete messages\"}");

      AnsiString permanentText = QueryParameter_(query, "permanent");
      bool permanent = permanentText == "1" || permanentText == "true";

      // Deleting is moving to the folder designated \Trash when the account
      // has one and the message is not in it already - what a mail client
      // does - and final otherwise, or when the caller says permanent.
      if (!permanent && folder->GetAccountID() == account->GetID())
      {
         std::shared_ptr<IMAPFolder> trash = DesignatedFolderOrCreate_(account, IMAPSpecialUse::DesignationTrash, _T("Trash"));
         if (trash && trash->GetID() != folder->GetID())
         {
            if (!RightOn_(account, trash, ACLPermission::PermissionInsert))
               return BuildResponse_(403, "{\"error\":\"the trash folder does not allow this account to add messages\"}");

            __int64 newMessageId = 0;
            if (!MessageUtilities::CopyToIMAPFolder(message, (int) trash->GetID(), newMessageId))
               return BuildResponse_(500, "{\"error\":\"the message could not be copied to the trash folder\"}");

            if (!DeleteOwnMessage_(account, message, folder))
               return BuildResponse_(500, "{\"error\":\"the message was copied to the trash folder but the original could not be removed\"}");

            AnsiString json;
            json.Format("{\"deleted\":false,\"moved_to\":%I64d,\"id\":%I64d}", trash->GetID(), newMessageId);
            return BuildResponse_(200, json);
         }
      }

      if (!DeleteOwnMessage_(account, message, folder))
         return BuildResponse_(500, "{\"error\":\"the message could not be deleted\"}");

      return BuildResponse_(200, "{\"deleted\":true}");
   }

   // The folder designated so, or one made by that name - which earns it the
   // designation, as "Drafts" does - so the page's Archive, Junk and Trash
   // keys work on an account that never made those folders.
   std::shared_ptr<IMAPFolder>
   RestApiServer::DesignatedFolderOrCreate_(std::shared_ptr<const Account> account, int designation, const String &name)
   {
      std::shared_ptr<IMAPFolder> folder = FindDesignatedFolder_(account, designation);
      if (folder)
         return folder;

      std::shared_ptr<IMAPFolders> folders = IMAPFolderContainer::Instance()->GetFoldersForAccount(account->GetID());
      if (!folders)
         return std::shared_ptr<IMAPFolder>();

      std::vector<String> path;
      path.push_back(name);
      folders->CreatePath(folders, path, true);

      return FindDesignatedFolder_(account, designation);
   }

   // What an IMAP MOVE into or out of \Junk does after the copy: the learner
   // decides whether the move is a lesson and queues it off this thread.
   void
   RestApiServer::LearnAfterMove_(std::shared_ptr<IMAPFolder> source, std::shared_ptr<IMAPFolder> destination, __int64 newMessageId, std::shared_ptr<const Account> account)
   {
      if (!source || !destination || newMessageId <= 0 || !account)
         return;

      std::shared_ptr<Message> copy = std::shared_ptr<Message>(new Message());
      if (!PersistentMessage::ReadObject(copy, newMessageId) || copy->GetID() == 0)
         return;

      // Learned for the destination folder's owner, as IMAP MOVE does: a
      // delegate filing into a shared mailbox is a lesson about that mailbox,
      // and the message's file is in that owner's store.
      std::shared_ptr<const Account> owner = CacheContainer::Instance()->GetAccount(destination->GetAccountID());
      SpamAssassinLearner::LearnFromMove(source, destination, copy, owner ? owner : account);
   }

   namespace
   {
      // "YYYY-MM-DD HH:MM:SS", the form a message's receipt time is stored in,
      // as local time; 0 when the text is not that.
      time_t LocalTimeOf(const String &stamp)
      {
         int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
         const AnsiString text(stamp);
         if (sscanf_s(text.c_str(), "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second) != 6)
            return 0;
         struct tm parts = { 0 };
         parts.tm_year = year - 1900;
         parts.tm_mon = month - 1;
         parts.tm_mday = day;
         parts.tm_hour = hour;
         parts.tm_min = minute;
         parts.tm_sec = second;
         parts.tm_isdst = -1;
         time_t value = mktime(&parts);
         return value < 0 ? 0 : value;
      }
   }

   // Every message in a Junk or Trash folder, gone for good - the one folder
   // action a mail client offers that is not a move - or, with
   // older_than_days=N, only what arrived more than N days ago: the clean-up
   // a storage view offers. Any other folder is refused: emptying is what
   // those two folders are for.
   HttpResponse
   RestApiServer::HandleMeFolderEmpty_(const Caller &caller, __int64 folderId, const AnsiString &query)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      std::shared_ptr<IMAPFolder> folder = FindReadableFolder_(account, folderId);
      if (!folder || folder->GetAccountID() != account->GetID())
         return BuildResponse_(404, "{\"error\":\"folder not found\"}");

      std::shared_ptr<IMAPFolders> folders = IMAPFolderContainer::Instance()->GetFoldersForAccount(account->GetID());
      std::map<__int64, int> designations;
      if (folders)
         IMAPSpecialUse::Resolve(folders, designations);
      int designation = designations.count(folder->GetID()) ? designations[folder->GetID()] : 0;
      if ((designation & (IMAPSpecialUse::DesignationJunk | IMAPSpecialUse::DesignationTrash)) == 0)
         return BuildResponse_(400, "{\"error\":\"only the Junk and Trash folders can be emptied\"}");

      if (!RightOn_(account, folder, ACLPermission::PermissionWriteDeleted) ||
          !RightOn_(account, folder, ACLPermission::PermissionExpunge))
         return BuildResponse_(403, "{\"error\":\"the folder does not allow this account to delete messages\"}");

      std::shared_ptr<Messages> messages = MessagesContainer::Instance()->GetMessages(folder->GetAccountID(), folder->GetID());
      if (!messages)
         return BuildResponse_(500, "{\"error\":\"the folder could not be read\"}");

      // Digits, and a number of days a calendar holds. A value this route
      // cannot read is refused, never read as "everything": with the filter
      // off the whole folder goes, so the one mistake that must not be made
      // is to take a typo for that. 0 is the filter off, as the page sends
      // for "everything"; more than a hundred years is nobody's clean-up.
      int olderThanDays = 0;
      AnsiString olderText = QueryParameter_(query, "older_than_days");
      if (!olderText.IsEmpty())
      {
         bool digits = olderText.size() <= 5;
         for (size_t i = 0; digits && i < olderText.size(); i++)
            digits = olderText.c_str()[i] >= '0' && olderText.c_str()[i] <= '9';
         if (!digits)
            return BuildResponse_(400, "{\"error\":\"older_than_days is a number of days, 0 to 36500\"}");
         olderThanDays = atoi(olderText.c_str());
         if (olderThanDays > 36500)
            return BuildResponse_(400, "{\"error\":\"older_than_days is a number of days, 0 to 36500\"}");
      }
      const time_t cutoff = olderThanDays > 0 ? time(nullptr) - (time_t) olderThanDays * 86400 : 0;
      if (olderThanDays > 0 && cutoff <= 0)
         return BuildResponse_(400, "{\"error\":\"older_than_days reaches before this clock began\"}");

      // At most this many go in one request: every delete happens under the
      // folder's lock and the request has a deadline, so a Junk folder of
      // years is emptied by asking again while remaining says there is more.
      const size_t MaxDeletesPerCall = 500;
      std::set<__int64> ids;
      size_t matched = 0;
      std::vector<std::shared_ptr<Message>> snapshot = messages->GetCopy();
      for (size_t i = 0; i < snapshot.size(); i++)
      {
         if (!snapshot[i])
            continue;
         if (cutoff > 0)
         {
            time_t created = LocalTimeOf(snapshot[i]->GetCreateTime());
            if (created == 0 || created >= cutoff)
               continue;
         }
         matched++;
         if (ids.size() < MaxDeletesPerCall)
            ids.insert(snapshot[i]->GetID());
      }

      std::vector<__int64> deleted;
      if (!ids.empty())
      {
         RecoverableStore::Scope recoverable;   // a person deleting mail: kept for recovery
         deleted = messages->DeleteMessagesById(ids);
         if (!deleted.empty())
            NotifyFolder(folder, ChangeNotification::NotificationMessageDeleted, deleted);
      }

      const size_t remaining = matched > ids.size() ? matched - ids.size() : 0;
      AnsiString json;
      json.Format("{\"deleted\":%d,\"remaining\":%d,\"folder_id\":%I64d,\"older_than_days\":%d}",
         (int) deleted.size(), (int) remaining, folder->GetID(), olderThanDays);
      return BuildResponse_(200, json);
   }

   // The message as it is on disk, for "view source" and for saving as .eml:
   // message/rfc822, as a download, under the same right as reading it.
   HttpResponse
   RestApiServer::HandleMeMessageSource_(const Caller &caller, __int64 messageId)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      std::shared_ptr<Message> message = std::shared_ptr<Message>(new Message());
      if (!PersistentMessage::ReadObject(message, messageId) || message->GetID() == 0)
         return BuildResponse_(404, "{\"error\":\"message not found\"}");

      std::shared_ptr<IMAPFolder> folder = FindReadableFolder_(account, message->GetFolderID());
      if (!folder || folder->GetAccountID() != message->GetAccountID())
         return BuildResponse_(404, "{\"error\":\"message not found\"}");

      String fileName = MessageFile_(message);
      if (!FileUtilities::Exists(fileName))
         return BuildResponse_(404, "{\"error\":\"the message file is missing\"}");

      const __int64 MaxSourceBytes = 25 * 1024 * 1024;
      __int64 size = FileUtilities::FileSize(fileName);
      if (size > MaxSourceBytes)
         return BuildResponse_(413, "{\"error\":\"the message is larger than 25 MB; fetch it with a mail client\"}");

      AnsiString bytes;
      {
#ifdef HM_PLATFORM_POSIX
         // As the other reads in this unit: a narrow path, because libstdc++
         // has no wide-path constructor.
         const AnsiString narrowFileName = fileName.c_str();
         std::ifstream in(narrowFileName.c_str(), std::ios::binary);
#else
         std::ifstream in(fileName.c_str(), std::ios::binary);
#endif
         if (!in)
            return BuildResponse_(500, "{\"error\":\"the message file could not be read\"}");
         std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
         bytes = AnsiString(contents);
      }

      AnsiString fileLabel;
      fileLabel.Format("message-%I64d.eml", message->GetID());

      HttpResponse response;
      response.status = 200;
      response.content_type = "message/rfc822";
      response.body = bytes;
      response.extra_headers =
         "Content-Disposition: attachment; filename=\"" + fileLabel + "\"\r\n"
         "X-Content-Type-Options: nosniff\r\n"
         "Content-Security-Policy: sandbox\r\n"
         "Cache-Control: no-store\r\n";
      return response;
   }

   namespace
   {
      // "spf=pass" inside an Authentication-Results value -> "pass"; the empty
      // string when the method is not mentioned.
      AnsiString VerdictOf(const AnsiString &results, const AnsiString &method)
      {
         AnsiString lower = results;
         lower.ToLower();
         AnsiString needle = method + "=";
         int at = lower.Find(needle);
         while (at >= 0)
         {
            bool boundary = at == 0 || lower[at - 1] == ' ' || lower[at - 1] == ';' || lower[at - 1] == '\t' || lower[at - 1] == '\r' || lower[at - 1] == '\n';
            if (boundary)
            {
               int start = at + needle.GetLength();
               int end = start;
               while (end < lower.GetLength() && ((lower[end] >= 'a' && lower[end] <= 'z') || (lower[end] >= '0' && lower[end] <= '9')))
                  end++;
               return lower.Mid(start, end - start);
            }
            at = lower.Find(needle, at + 1);
         }
         return "";
      }

      // The domain a dmarc=pass verdict was for: header.from in the dmarc clause,
      // which is what a brand logo may be shown for. Empty for any other verdict.
      AnsiString BrandOf(const AnsiString &results)
      {
         if (VerdictOf(results, "dmarc") != "pass")
            return "";
         AnsiString lower = results;
         lower.ToLower();
         int at = lower.Find("dmarc=");
         if (at < 0)
            return "";
         int clauseEnd = lower.Find(";", at);
         if (clauseEnd < 0)
            clauseEnd = lower.GetLength();
         int from = lower.Find("header.from=", at);
         if (from < 0 || from >= clauseEnd)
            return "";
         int start = from + 12;
         int end = start;
         while (end < clauseEnd && lower[end] != ' ' && lower[end] != '\t' && lower[end] != '\r' && lower[end] != '\n' && lower[end] != ')')
            end++;
         const String domain = BrandLogo::NormaliseDomain(String(lower.Mid(start, end - start)));
         return AnsiString(domain);
      }

      // The domain a DKIM verdict was for: header.d in the dkim clause.
      AnsiString DkimDomainOf(const AnsiString &results)
      {
         AnsiString lower = results;
         lower.ToLower();
         int at = lower.Find("dkim=");
         if (at < 0)
            return "";
         int clauseEnd = lower.Find(";", at);
         if (clauseEnd < 0)
            clauseEnd = lower.GetLength();
         int d = lower.Find("header.d=", at);
         if (d < 0 || d >= clauseEnd)
            return "";
         int start = d + 9;
         int end = start;
         while (end < clauseEnd && lower[end] != ' ' && lower[end] != '\t' && lower[end] != '\r' && lower[end] != '\n')
            end++;
         return lower.Mid(start, end - start);
      }

      // The domain of the first address in a From header, lower-cased.
      AnsiString DomainOfFrom(const AnsiString &from)
      {
         int at = from.Find("@");
         if (at < 0)
            return "";
         int end = at + 1;
         while (end < from.GetLength())
         {
            char c = from[end];
            if (c == '>' || c == ' ' || c == ',' || c == ';' || c == '\t' || c == '\r' || c == '\n' || c == '(' || c == '"')
               break;
            end++;
         }
         AnsiString domain = from.Mid(at + 1, end - at - 1);
         domain.ToLower();
         return domain;
      }
   }

   // The head of the file as it is, capped; the three verdicts a reader
   // learns to look at, from the first Authentication-Results header, which
   // is the one this server wrote on receipt when it wrote one; and whether
   // the sender's domain is another domain than the account's.
   // The Authentication-Results field this server wrote, and only that: the
   // one whose authserv-id is this server's own, read the way the strip on
   // receipt reads it (unfolded; the first token before ';', unquoted, a
   // version number dropped), and only while the server writes one at all -
   // with AuthenticationResultsEnabled off nothing is stripped on receipt, so
   // a sender's own "spf=pass; dkim=pass" would otherwise draw three green
   // badges. Empty when there is none, and the page then shows nothing.
   AnsiString
   RestApiServer::OwnAuthenticationResults_(const MimeHeader &mimeHeader)
   {
      if (!IniFileSettings::Instance()->GetAuthenticationResultsEnabled())
         return "";

      AnsiString ours = AuthenticationResults::GetAuthservId();
      ours.MakeLower();
      if (ours.IsEmpty())
         return "";

      const std::vector<MimeField> &fields = const_cast<MimeHeader &>(mimeHeader).Fields();
      for (size_t i = 0; i < fields.size(); i++)
      {
         AnsiString name = fields[i].GetName();
         if (name.CompareNoCase("Authentication-Results") != 0)
            continue;

         AnsiString value = fields[i].GetValue();
         value.Replace("\r", "");
         value.Replace("\n", " ");

         AnsiString id = value;
         int semicolon = id.Find(";");
         if (semicolon >= 0)
            id = id.Mid(0, semicolon);
         id.Trim();
         if (id.GetLength() >= 2 && id[0] == '"')
         {
            int closing = id.Find("\"", 1);
            if (closing > 0)
               id = id.Mid(1, closing - 1);
         }
         // "authserv-id 1": the version number after the identity is not part of it.
         int space = id.Find(" ");
         if (space > 0)
            id = id.Mid(0, space);
         id.MakeLower();

         if (id == ours)
            return value;
      }

      return "";
   }

   // Whether an HTML part names anything a browser would fetch from the
   // network: an src= or a url() with an http(s) address, or a stylesheet
   // link. A plain href is a link the reader may click, not a request.
   bool
   RestApiServer::HtmlNamesRemoteContent_(const AnsiString &html)
   {
      AnsiString lower = html;
      lower.ToLower();
      lower.Replace("\r", " ");
      lower.Replace("\n", " ");
      lower.Replace("\t", " ");
      static const char *const needles[] = { "src=\"http", "src='http", "src=http", "src= \"http", "src= 'http", "url(http", "url('http", "url(\"http", "url( http", "<link ", "@import" };
      for (size_t i = 0; i < sizeof(needles) / sizeof(needles[0]); i++)
         if (lower.Find(needles[i]) >= 0)
            return true;
      return false;
   }

   // The message's HTML part as a document of its own, for the page's frame:
   // a frame with a src has the policy this answer carries, where a srcdoc
   // frame would inherit the page's, which allows no remote image at all -
   // so remote images and styles are blocked here unless the reader asked for
   // them with ?remote=1, nothing runs, no form is submitted and no address is
   // rewritten. Images the message embeds (cid:) are put in as data: URLs
   // under the budget below.
   HttpResponse
   RestApiServer::HandleMeMessageHtml_(const Caller &caller, __int64 messageId, const AnsiString &query)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      std::shared_ptr<IMAPFolder> folder;
      std::shared_ptr<Message> message = FindOwnMessage_(account, messageId, folder);
      if (!message)
         return BuildResponse_(404, "{\"error\":\"message not found\"}");

      if (message->GetSize() > MaxMessageParseBytes)
         return BuildResponse_(413, "{\"error\":\"the message is too large to read here\"}");

      String fileName = MessageFile_(message);
      if (!FileUtilities::Exists(fileName))
         return BuildResponse_(404, "{\"error\":\"the message file is missing\"}");

      MessageData messageData;
      if (!messageData.LoadFromMessage(fileName, message))
         return BuildResponse_(500, "{\"error\":\"the message could not be parsed\"}");

      AnsiString html = Utf8_(messageData.GetHTMLBody());
      if (html.IsEmpty())
         return BuildResponse_(404, "{\"error\":\"the message has no HTML part\"}");

      // cid: references, resolved to the parts they name - image types only,
      // 4 MB each, 12 MB together, and as many parts as the send path moves
      // out of the HTML (InlineImageParts::MostImages), so a Sent copy or a
      // draft shows every image it was written with.
      const __int64 InlineEach = 4 * 1024 * 1024;
      const __int64 InlineTogether = 12 * 1024 * 1024;
      const int InlineMost = InlineImageParts::MostImages;
      std::shared_ptr<Attachments> attachments = messageData.GetAttachments();
      std::list<std::shared_ptr<MimeBody>> parts;
      std::shared_ptr<MimeBody> mimeMessage = messageData.GetMimeMessage();
      if (mimeMessage)
         mimeMessage->GetAttachmentList(mimeMessage, parts);
      std::vector<std::shared_ptr<MimeBody>> partList(parts.begin(), parts.end());
      __int64 budget = InlineTogether;
      int inlined = 0;
      if (attachments)
      {
         for (size_t i = 0; i < attachments->GetCount() && i < partList.size() && inlined < InlineMost; i++)
         {
            std::shared_ptr<Attachment> attachment = attachments->GetItem((unsigned int) i);
            if (!attachment)
               continue;
            AnsiString contentId = ContentId(partList[i]);
            AnsiString type = MediaType(attachment->GetContentType());
            if (contentId.IsEmpty() || type.Find("image/") != 0)
               continue;
            if (html.Find("cid:" + contentId) < 0)
               continue;
            AnsiString bytes;
            if (!attachment->GetContent(bytes) || bytes.GetLength() > InlineEach || bytes.GetLength() > budget)
               continue;
            budget -= bytes.GetLength();
            inlined++;
            AnsiString dataUrl = "data:" + type + ";base64," + Base64::Encode(bytes.c_str(), bytes.GetLength());
            html.Replace("cid:" + contentId, dataUrl);
         }
      }

      bool remote = QueryParameter_(query, "remote") == "1";

      // The policy, once for the header and once inside the document as a
      // meta tag, so that the document is under it wherever it is shown: in
      // the frame the page points at this route, and in a frame the page
      // fills from a blob it fetched - which is how the webmail shows a
      // second account's message, since a frame's own request carries the
      // cookie and not that account's bearer. A meta policy cannot carry
      // sandbox or frame-ancestors, so those stay the header's and the
      // frame's sandbox attribute's.
      const AnsiString sources = AnsiString("default-src 'none'; style-src 'unsafe-inline'; img-src data:") + (remote ? " https: http:" : "") +
         (remote ? "; font-src https: http:" : "") + "; form-action 'none'; base-uri 'none'";

      AnsiString document =
         "<!doctype html><html><head><meta charset=\"utf-8\"><meta http-equiv=\"Content-Security-Policy\" content=\"" + sources + "\">"
         "<base target=\"_blank\" rel=\"noopener noreferrer\">"
         "<style>body{font-family:system-ui,sans-serif;margin:.5rem;word-break:break-word}img{max-width:100%}</style></head><body>" + html + "</body></html>";

      HttpResponse response;
      response.status = 200;
      response.content_type = "text/html; charset=utf-8";
      response.body = document;
      response.extra_headers =
         AnsiString("Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; img-src data:") + (remote ? " https: http:" : "") +
         (remote ? "; font-src https: http:" : "") + "; sandbox allow-popups allow-popups-to-escape-sandbox; form-action 'none'; base-uri 'none'; frame-ancestors 'self'\r\n"
         "X-Content-Type-Options: nosniff\r\n"
         "Referrer-Policy: no-referrer\r\n"
         "Cache-Control: no-store\r\n";
      return response;
   }

   AnsiString
   RestApiServer::HeaderFieldsJson_(const String &fileName, std::shared_ptr<const Account> account)
   {
      const int MaxHeaderBytes = 64 * 1024;

      AnsiString header = PersistentMessage::LoadHeader(fileName, false);
      AnsiString results;
      AnsiString from;
      AnsiString listUnsubscribe;
      AnsiString listUnsubscribePost;
      AnsiString receiptRequestedBy;
      AnsiString externalStamp;
      AnsiString firstContactStamp;
      if (!header.IsEmpty())
      {
         MimeHeader mimeHeader;
         mimeHeader.Load(header.c_str(), header.GetLength(), true);
         results = OwnAuthenticationResults_(mimeHeader);
         const char *value = mimeHeader.GetRawFieldValue("From");
         from = value ? value : "";
         value = mimeHeader.GetRawFieldValue("List-Unsubscribe");
         listUnsubscribe = value ? value : "";
         value = mimeHeader.GetRawFieldValue("List-Unsubscribe-Post");
         listUnsubscribePost = value ? value : "";
         value = mimeHeader.GetRawFieldValue("Disposition-Notification-To");
         receiptRequestedBy = value ? value : "";
         value = mimeHeader.GetRawFieldValue("X-hMailServer-External");
         externalStamp = value ? value : "";
         value = mimeHeader.GetRawFieldValue("X-hMailServer-First-Contact");
         firstContactStamp = value ? value : "";
         externalStamp.TrimLeft();
         externalStamp.TrimRight();
         firstContactStamp.TrimLeft();
         firstContactStamp.TrimRight();
         listUnsubscribe.TrimLeft();
         listUnsubscribe.TrimRight();
         listUnsubscribePost.ToLower();
         receiptRequestedBy.TrimLeft();
         receiptRequestedBy.TrimRight();
      }
      if (header.GetLength() > MaxHeaderBytes)
         header = header.Mid(0, MaxHeaderBytes);

      // A header block is bytes; the JSON is UTF-8. Decoding and re-encoding
      // it turns any byte that is not UTF-8 into the replacement character,
      // so the document stays valid whatever an old client wrote in a header.
      String headerText;
      Unicode::MultiByteToWide(header, headerText);
      String resultsText;
      Unicode::MultiByteToWide(results, resultsText);

      AnsiString accountDomain = Utf8_(account->GetAddress());
      accountDomain = DomainOfFrom(accountDomain);
      // The sender's domain from the address the header names, parsed as the
      // DMARC check parses it - not from the first @ in the header, which a
      // display name can carry.
      AnsiString senderDomain = DomainOfFrom(Utf8_(DMARC::ExtractAddressFromHeaderValue(String(from))));

      // The server's own verdict when the recipient's domain asked for it -
      // ExternalSenderTagger stamped X-hMailServer-External using the rule in
      // MessageOrigin, which knows about authenticated submissions and trusted
      // relays and this one does not. Where the domain has not turned the tag
      // on there is no stamp, and the comparison below stands, which is what
      // the badge has always done.
      bool externalStamped = externalStamp.CompareNoCase("YES") == 0;
      bool external = externalStamped ||
                      (!senderDomain.IsEmpty() && senderDomain != accountDomain);
      bool firstContact = firstContactStamp.CompareNoCase("YES") == 0;

      String listText;
      Unicode::MultiByteToWide(listUnsubscribe, listText);
      String receiptText;
      Unicode::MultiByteToWide(receiptRequestedBy, receiptText);

      AnsiString json;
      json.Format("\"headers\":\"%hs\",\"authentication\":{\"spf\":\"%hs\",\"dkim\":\"%hs\",\"dkim_domain\":\"%hs\",\"dmarc\":\"%hs\",\"results\":\"%hs\"},\"external\":%hs,\"external_stamped\":%hs,\"first_contact\":%hs,\"list_unsubscribe\":\"%hs\",\"list_unsubscribe_post\":%hs,\"receipt_requested_by\":\"%hs\"",
         JsonEscape_(Utf8_(headerText)).c_str(),
         VerdictOf(results, "spf").c_str(),
         VerdictOf(results, "dkim").c_str(),
         JsonEscape_(DkimDomainOf(results)).c_str(),
         VerdictOf(results, "dmarc").c_str(),
         JsonEscape_(Utf8_(resultsText)).c_str(),
         external ? "true" : "false",
         externalStamped ? "true" : "false",
         firstContact ? "true" : "false",
         JsonEscape_(Utf8_(listText)).c_str(),
         listUnsubscribePost.Find("list-unsubscribe=one-click") >= 0 ? "true" : "false",
         JsonEscape_(Utf8_(receiptText)).c_str());
      const AnsiString brand = Configuration::Instance()->GetBrandLogosEnabled() ? BrandOf(results) : AnsiString("");
      json += ",\"brand\":\"" + JsonEscape_(brand) + "\"";
      return json;
   }

   // The account's live browser sessions: named by the first twelve hex
   // digits of the token's hash, with how long ago each began and was last
   // seen, and which one is asking. A caller on a password rather than a
   // session sees them all and is current for none.
   HttpResponse
   RestApiServer::HandleMeSessions_(const Caller &caller)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      AnsiString json = "{\"sessions\":[";
      {
         std::lock_guard<std::mutex> guard(browser_sessions_mutex);
         const ULONGLONG now = GetTickCount64();
         bool first = true;
         for (std::vector<BrowserSession>::const_iterator it = browser_sessions.begin(); it != browser_sessions.end(); ++it)
         {
            if (it->administrator || it->account_id != account->GetID())
               continue;
            if (HasExpired_(*it, now))
               continue;
            if (!first)
               json += ",";
            first = false;
            // bearer: the session is held by a webmail page as its second
            // account, beside another account's cookie - so the person can
            // tell it from their own sign-ins and end it from here.
            AnsiString entry;
            entry.Format("{\"id\":\"%hs\",\"device\":\"%hs\",\"address\":\"%hs\",\"created_seconds_ago\":%I64u,\"idle_seconds\":%I64u,\"current\":%hs,\"support\":%hs,\"bearer\":%hs}",
               it->token_hash.Mid(0, 12).c_str(),
               JsonEscape_(it->device).c_str(),
               JsonEscape_(it->bound_address).c_str(),
               (unsigned __int64) ((now - it->created_at) / 1000),
               (unsigned __int64) ((now - it->last_seen_at) / 1000),
               (caller.via_session && ConstantTimeEquals(it->token_hash, caller.session_hash)) ? "true" : "false",
               it->support_by.IsEmpty() ? "false" : "true",
               it->bearer ? "true" : "false");
            json += entry;
         }
      }
      json += "]}";
      return BuildResponse_(200, json);
   }

   // Every other session of the account ends; the one asking, when it is a
   // session, stays. What a password change does, on request.
   HttpResponse
   RestApiServer::HandleMeSessionsEnd_(const Caller &caller)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      HttpResponse refusal;
      if (RefuseAppPasswordCaller_(caller, "ending the account's sessions", refusal))
         return refusal;

      int ended = 0;
      {
         std::lock_guard<std::mutex> guard(browser_sessions_mutex);
         const __int64 accountId = account->GetID();
         const AnsiString keep = caller.via_session ? caller.session_hash : AnsiString();
         for (std::vector<BrowserSession>::iterator it = browser_sessions.begin(); it != browser_sessions.end();)
         {
            bool theirs = !it->administrator && it->account_id == accountId && (keep.IsEmpty() || !ConstantTimeEquals(it->token_hash, keep));
            if (theirs)
            {
               it = browser_sessions.erase(it);
               ended++;
            }
            else
               ++it;
         }
      }

      AnsiString json;
      json.Format("{\"ended\":%d}", ended);
      return BuildResponse_(200, json);
   }

   HttpResponse
   RestApiServer::HandleMeSessionDelete_(const Caller &caller, const AnsiString &id)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      HttpResponse refusal;
      if (RefuseAppPasswordCaller_(caller, "ending one of the account's sessions", refusal))
         return refusal;

      if (!IsLowerHex(id, 12))
         return BuildResponse_(400, "{\"error\":\"a session id is twelve hex digits\"}");

      bool ended = false;
      {
         std::lock_guard<std::mutex> guard(browser_sessions_mutex);
         for (std::vector<BrowserSession>::iterator it = browser_sessions.begin(); it != browser_sessions.end(); ++it)
         {
            if (it->administrator || it->account_id != account->GetID())
               continue;
            if (it->token_hash.Mid(0, 12) != id)
               continue;
            browser_sessions.erase(it);
            ended = true;
            break;
         }
      }

      if (!ended)
         return BuildResponse_(404, "{\"error\":\"no such session\"}");

      return BuildResponse_(200, "{\"ended\":true}");
   }

   // The clients this account has signed in from - protocol, address, user
   // agent, which credential each used, when each was first and last seen -
   // and which of them this request came in on. The same rows an administrator
   // reads under the account's address, built by the same code, so that the
   // person and the administrator are never shown two different lists.
   HttpResponse
   RestApiServer::HandleMeDevices_(const Caller &caller)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      const std::vector<SignInRecord> records = SignInStore::List(account->GetID());

      AnsiString json = "{\"devices\":[";

      for (size_t index = 0; index < records.size(); index++)
      {
         if (index > 0)
            json += ",";

         // Current for the client this request came in on, and for nothing when
         // the request carried a password rather than a session - exactly as the
         // session listing answers.
         const bool current = caller.via_session && !caller.session_device.IsEmpty() &&
                              caller.session_device == AnsiString(records[index].device);

         json += SignInStore::ToJson(records[index], AnsiString(current ? "\"current\":true" : "\"current\":false"));
      }

      json += "]}";

      return BuildResponse_(200, json);
   }

   // One client revoked: its browser session ended, its live mail connections
   // dropped and - with credential=revoke - the app password it signed in with
   // deactivated. forget=true removes the entry as well, for somebody who wants
   // a client they no longer own off the list rather than marked.
   HttpResponse
   RestApiServer::HandleMeDeviceDelete_(const Caller &caller, const AnsiString &id, const AnsiString &query)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      HttpResponse refusal;
      if (RefuseAppPasswordCaller_(caller, "revoking one of the account's devices", refusal))
         return refusal;

      if (!IsLowerHex(id, 32))
         return BuildResponse_(400, "{\"error\":\"a device id is thirty-two hex digits\"}");

      const bool revokeCredential = QueryParameter_(query, "credential") == "revoke";
      const bool forget = QueryParameter_(query, "forget") == "true";

      int connections = 0;
      int sessions = 0;
      bool credentialDisabled = false;

      const bool found = forget
         ? SignInStore::Forget(account->GetID(), String(id), connections, sessions)
         : SignInStore::Revoke(account->GetID(), String(id), revokeCredential, connections, sessions, credentialDisabled);

      if (!found)
         return BuildResponse_(404, "{\"error\":\"no such device of this account\"}");

      LOG_APPLICATION("REST API: " + account->GetAddress() + " revoked a device from " + String(caller.peer.ToString()) + ".");

      AnsiString json;
      json.Format("{\"revoked\":true,\"forgotten\":%hs,\"sessions_ended\":%d,\"connections_dropped\":%d,\"credential_disabled\":%hs}",
         forget ? "true" : "false", sessions, connections, credentialDisabled ? "true" : "false");

      return BuildResponse_(200, json);
   }

   // What the mailbox holds: the quota, each of the account's own folders
   // with its count and bytes, and the twenty largest messages, so that a
   // reader can see where the space went and what to let go of.
   HttpResponse
   RestApiServer::HandleMeStorage_(const Caller &caller)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      String delimiter = Configuration::Instance()->GetIMAPConfiguration()->GetHierarchyDelimiter();
      std::vector<std::pair<std::shared_ptr<IMAPFolder>, String>> folders;
      CollectReadableFolders_(account, IMAPFolderContainer::Instance()->GetFoldersForAccount(account->GetID()), String(), delimiter, folders, 0);

      // The twenty largest, and never more than twenty held: each folder's
      // messages are looked at a folder at a time, and a message is kept only
      // while it is among the largest seen so far. The folder is an index
      // into the list above, not a copy of its path per message.
      struct Held
      {
         std::shared_ptr<Message> message;
         size_t folder;
      };
      const size_t LargestCount = 20;
      std::vector<Held> largest;
      const auto larger = [](const Held &a, const Held &b) { return a.message->GetSize() > b.message->GetSize(); };

      AnsiString json;
      json.Format("{\"used_bytes\":%I64d,\"limit_mb\":%d,\"folders\":[",
         AccountSizeCache::Instance()->GetSize(account->GetID()),
         (int) account->GetAccountMaxSize());

      bool first = true;
      for (size_t f = 0; f < folders.size(); f++)
      {
         std::shared_ptr<IMAPFolder> folder = folders[f].first;
         if (!folder || folder->GetAccountID() != account->GetID())
            continue;
         std::shared_ptr<Messages> messages = folder->GetMessages();
         if (!messages)
            continue;

         __int64 bytes = 0;
         long count = 0;
         std::vector<std::shared_ptr<Message>> snapshot = messages->GetCopy();
         for (size_t i = 0; i < snapshot.size(); i++)
         {
            if (!snapshot[i] || snapshot[i]->GetFlagDeleted())
               continue;
            count++;
            bytes += snapshot[i]->GetSize();
            if (largest.size() < LargestCount || snapshot[i]->GetSize() > largest.back().message->GetSize())
            {
               Held one;
               one.message = snapshot[i];
               one.folder = f;
               largest.insert(std::upper_bound(largest.begin(), largest.end(), one, larger), one);
               if (largest.size() > LargestCount)
                  largest.pop_back();
            }
         }

         if (!first)
            json += ",";
         first = false;
         AnsiString entry;
         entry.Format("{\"id\":%I64d,\"path\":\"%hs\",\"messages\":%ld,\"bytes\":%I64d}",
            folder->GetID(), JsonEscape_(Utf8_(DecodeFolderName_(folders[f].second))).c_str(), count, bytes);
         json += entry;
      }

      json += "],\"largest\":[";
      for (size_t i = 0; i < largest.size(); i++)
      {
         AnsiString subject, from, date;
         DescribeHeader(MessageFile_(largest[i].message), subject, from, date);
         if (i > 0)
            json += ",";
         AnsiString entry;
         entry.Format("{\"id\":%I64d,\"folder_id\":%I64d,\"folder\":\"%hs\",\"subject\":\"%hs\",\"from\":\"%hs\",\"date\":\"%hs\",\"size\":%d}",
            largest[i].message->GetID(), folders[largest[i].folder].first->GetID(),
            JsonEscape_(Utf8_(DecodeFolderName_(folders[largest[i].folder].second))).c_str(),
            JsonEscape_(subject).c_str(), JsonEscape_(from).c_str(), JsonEscape_(date).c_str(),
            largest[i].message->GetSize());
         json += entry;
      }
      json += "]}";
      return BuildResponse_(200, json);
   }

   namespace
   {
      String PreferenceOf(__int64 accountId, const String &name)
      {
         SQLCommand command("select prefvalue from hm_accountprefs where prefaccountid = @ACCOUNTID and prefname = @NAME");
         command.AddParameter("@ACCOUNTID", accountId);
         command.AddParameter("@NAME", name);
         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (!recordset || recordset->IsEOF())
            return String();
         return recordset->GetStringValue("prefvalue");
      }

      void RecordPreference(__int64 accountId, const String &name, const String &value)
      {
         SQLCommand find("select prefid from hm_accountprefs where prefaccountid = @ACCOUNTID and prefname = @NAME");
         find.AddParameter("@ACCOUNTID", accountId);
         find.AddParameter("@NAME", name);
         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(find);
         SQLStatement statement;
         statement.SetTable("hm_accountprefs");
         if (recordset && !recordset->IsEOF())
         {
            AnsiString id;
            id.Format("%I64d", recordset->GetInt64Value("prefid"));
            statement.SetStatementType(SQLStatement::STUpdate);
            statement.AddColumn("prefvalue", value);
            statement.SetWhereClause("prefid = " + id);
            Application::Instance()->GetDBManager()->Execute(statement);
            return;
         }
         statement.SetStatementType(SQLStatement::STInsert);
         statement.SetIdentityColumn("prefid");
         statement.AddColumnInt64("prefaccountid", accountId);
         statement.AddColumn("prefname", name);
         statement.AddColumn("prefvalue", value);
         __int64 prefId = 0;
         Application::Instance()->GetDBManager()->Execute(statement, &prefId);
      }
   }

   // A session for the account, opened by the administrator - only when the
   // account has said so (its support_allowed preference), and never quietly:
   // the moment and who are written where the user sees them, every request
   // of the session goes to the application log, and the user's own session
   // list shows it and can end it.
   HttpResponse
   RestApiServer::HandleAccountSupportSession_(const Caller &caller, const AnsiString &address, const AnsiString &request)
   {
      String wanted;
      Unicode::MultiByteToWide(address, wanted);
      wanted.ToLower();
      std::shared_ptr<const Account> account = CacheContainer::Instance()->GetAccount(wanted);
      if (!account || account->GetID() == 0)
         return BuildResponse_(404, "{\"error\":\"no such account\"}");
      if (!account->GetActive())
         return BuildResponse_(400, "{\"error\":\"the account is not active\"}");

      if (PreferenceOf(account->GetID(), _T("support_allowed")) != _T("1"))
         return BuildResponse_(403, "{\"error\":\"the account has not allowed support access; the user turns it on under Security in the webmail\"}");

      unsigned char secret[SessionTokenBytes];
      if (RAND_bytes(secret, sizeof(secret)) != 1)
         return BuildResponse_(500, "{\"error\":\"no entropy for a session token\"}");
      AnsiString token = BytesToLowerHex(secret, SessionTokenBytes);
      AnsiString tokenHash = HashApiKeyToken(token);
      if (tokenHash.IsEmpty())
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      // A support session is a session of the ACCOUNT, opened from the
      // administrator's own client, so the client it records is the
      // administrator's and the policy that governs it is the account's domain's.
      // The account's device list shows it with the support mark, and ending
      // that device ends this session as it would any other.
      const AnsiString supportAddress = caller.peer.ToString();
      const AnsiString supportAgent = GetHeader_(request, "user-agent");
      String supportWideAgent;
      Unicode::MultiByteToWide(supportAgent, supportWideAgent);
      const String supportDevice = SignInStore::DeviceKey(SignInStore::ProtocolWeb, String(supportAddress), supportWideAgent);

      int idleSeconds = 0;
      int lifetimeSeconds = 0;

      {
         std::lock_guard<std::mutex> guard(browser_sessions_mutex);
         const ULONGLONG now = GetTickCount64();
         if (browser_sessions.size() >= MaxBrowserSessions)
            return BuildResponse_(503, "{\"error\":\"the session table is full\"}");
         BrowserSession session;
         session.token_hash = tokenHash;
         session.account_id = account->GetID();
         session.administrator = false;
         session.support_by = caller.identity;
         session.created_at = now;
         session.last_seen_at = now;
         session.device = AnsiString(supportDevice);
         session.bound_address = supportAddress;
         session.domain = StringParser::ExtractDomain(account->GetAddress());
         ApplyPolicy_(session);
         idleSeconds = (int) (session.idle_milliseconds / 1000);
         lifetimeSeconds = (int) (session.lifetime_milliseconds / 1000);
         browser_sessions.push_back(session);
      }

      String stamp = Time::GetCurrentDateTime() + _T(" by ") + String(caller.identity);
      RecordPreference(account->GetID(), _T("support_last"), stamp);
      LOG_APPLICATION("REST API: " + String(caller.identity) + " opened a support session as " + account->GetAddress() + " from " + String(caller.peer.ToString()) + ".");

      AnsiString cookie;
      cookie.Format("Set-Cookie: %hs=%hs; Path=/; HttpOnly; SameSite=Strict; Max-Age=%d%hs\r\n",
         SessionCookieName, token.c_str(), lifetimeSeconds, use_tls_ ? "; Secure" : "");
      AnsiString body;
      body.Format("{\"support\":true,\"account\":\"%hs\",\"idle_seconds\":%d,\"lifetime_seconds\":%d}",
         JsonEscape_(Utf8_(account->GetAddress())).c_str(), idleSeconds, lifetimeSeconds);
      return BuildResponse_(201, body, cookie);
   }

   // A JSON body is UTF-8; the server's strings are wide.
   String
   RestApiServer::JsonUtf8Value_(const AnsiString &json, const AnsiString &key)
   {
      String value;
      Unicode::MultiByteToWide(GetJsonStringValue_(json, key), value);
      return value;
   }

   int
   RestApiServer::JsonWholeNumber_(const AnsiString &json, const AnsiString &key, __int64 &value)
   {
      JsonValue document;
      std::string parseError;
      if (JsonValue::Parse(std::string(json.c_str(), json.size()), document, parseError) && document.IsObject())
      {
         const JsonValue *found = FindJsonMember_(document, std::string(key.c_str()));
         if (!found)
            return 0;
         if (found->IsString())
         {
            // Digits in quotes are read as the other readers read them; any
            // other string is not a number.
            const std::string &text = found->AsString();
            if (text.empty() || text.size() > 15 || text.find_first_not_of("0123456789") != std::string::npos)
               return -1;
            value = _atoi64(text.c_str());
            return 1;
         }
         if (!found->IsNumber())
            return -1;
         const double number = found->AsNumber();
         if (number < 0 || number > 9007199254740991.0)
            return -1;
         const __int64 whole = (__int64) number;
         if ((double) whole != number)
            return -1;
         value = whole;
         return 1;
      }

      // What does not parse is read the way the other readers read it: a bare
      // run of digits after the key, or nothing.
      if (JsonNumber(json, key, value))
         return 1;
      return json.Find("\"" + key + "\"") >= 0 ? -1 : 0;
   }

   namespace
   {
      // "a@x, Name <b@y>; c@z" -> the three addresses, and the entries as
      // written for the header. A list is split on commas and semicolons; an
      // entry keeps its display name for the header and gives its address to
      // the envelope.
      void SplitAddressList(const String &list, std::vector<String> &addresses, std::vector<String> &entries)
      {
         std::vector<String> parts = StringParser::SplitString(list, _T(","));
         std::vector<String> all;
         for (size_t i = 0; i < parts.size(); i++)
         {
            std::vector<String> inner = StringParser::SplitString(parts[i], _T(";"));
            all.insert(all.end(), inner.begin(), inner.end());
         }

         for (size_t i = 0; i < all.size(); i++)
         {
            String entry = all[i];
            entry.TrimLeft();
            entry.TrimRight();
            if (entry.IsEmpty())
               continue;

            // "Name <address>" gives its address to the envelope and keeps the
            // whole entry for the header.
            String address = entry;
            int open = entry.Find(_T("<"));
            int close = entry.ReverseFind(_T(">"));
            if (open >= 0 && close > open)
               address = entry.Mid(open + 1, close - open - 1);
            address.TrimLeft();
            address.TrimRight();
            if (address.IsEmpty())
               address = entry;

            addresses.push_back(address);
            entries.push_back(entry);
         }
      }

      String JoinEntries(const std::vector<String> &entries)
      {
         String joined;
         for (size_t i = 0; i < entries.size(); i++)
         {
            if (i > 0)
               joined += _T(", ");
            joined += entries[i];
         }
         return joined;
      }
   }

   HttpResponse
   RestApiServer::HandleMeMessageSend_(const Caller &caller, const AnsiString &requestBody)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      // The envelope sender and the From header: the account itself unless the
      // body names one of the account's identities (RestApiIdentities.cpp).
      String from;
      String fromHeader;
      {
         AnsiString problem;
         int refused = ResolveSender_(account, JsonUtf8Value_(requestBody, "from"), from, fromHeader, problem);
         if (refused != 0)
            return BuildResponse_(refused, problem);
      }

      std::vector<String> toAddresses, toEntries, ccAddresses, ccEntries, bccAddresses, bccEntries;
      SplitAddressList(JsonUtf8Value_(requestBody, "to"), toAddresses, toEntries);
      SplitAddressList(JsonUtf8Value_(requestBody, "cc"), ccAddresses, ccEntries);
      SplitAddressList(JsonUtf8Value_(requestBody, "bcc"), bccAddresses, bccEntries);

      // An entry that is the name of one of the account's contact groups -
      // "Family", with no address in it - becomes the group's recipients, in
      // the header as in the envelope (ContactGroups.h). The page expands a
      // group as it is chosen; this is the same for a client of the API. The
      // address book is read once for the three fields, and not at all when
      // every entry carries an address, which is the common send.
      if (ContactGroups::NeedsExpansion(toEntries) || ContactGroups::NeedsExpansion(ccEntries) || ContactGroups::NeedsExpansion(bccEntries))
      {
         std::vector<ContactGroup> groups;
         if (ContactGroups::List(account, groups) && !groups.empty())
         {
            ContactGroups::ExpandEntries(groups, toAddresses, toEntries);
            ContactGroups::ExpandEntries(groups, ccAddresses, ccEntries);
            ContactGroups::ExpandEntries(groups, bccAddresses, bccEntries);
         }
      }

      std::vector<String> recipients;
      recipients.insert(recipients.end(), toAddresses.begin(), toAddresses.end());
      recipients.insert(recipients.end(), ccAddresses.begin(), ccAddresses.end());
      recipients.insert(recipients.end(), bccAddresses.begin(), bccAddresses.end());

      if (recipients.empty())
         return BuildResponse_(400, "{\"error\":\"at least one recipient is required, in to, cc or bcc\"}");

      // A message that expires (ConfidentialMessages.h): expires_in is seconds
      // from now, and passcode a word the sender gives the readers another way.
      // Everything about it is refused here, before anything is written - and
      // an expires_in that is not a whole number of seconds is refused too,
      // never read as absent: a client that wrote the key asked for an expiry,
      // and sending the message in full, for ever, is the opposite of that.
      __int64 expiresIn = 0;
      String passcode;
      const int expiresState = JsonWholeNumber_(requestBody, "expires_in", expiresIn);
      if (expiresState < 0)
      {
         AnsiString json;
         json.Format("{\"error\":\"expires_in is a whole number of seconds from now, from %d to %d (ninety days), or 0 for no expiry\"}", ConfidentialMessages::MinimumSeconds, ConfidentialMessages::MaximumSeconds);
         return BuildResponse_(400, json);
      }
      const bool confidential = expiresState > 0 && expiresIn != 0;
      if (!confidential && !JsonUtf8Value_(requestBody, "passcode").IsEmpty())
         return BuildResponse_(400, "{\"error\":\"passcode is for a message that expires: give expires_in with it\"}");
      if (confidential)
      {
         if (expiresIn < ConfidentialMessages::MinimumSeconds || expiresIn > ConfidentialMessages::MaximumSeconds)
         {
            AnsiString json;
            json.Format("{\"error\":\"expires_in is seconds from now, from %d to %d (ninety days)\"}", ConfidentialMessages::MinimumSeconds, ConfidentialMessages::MaximumSeconds);
            return BuildResponse_(400, json);
         }
         if (ConfidentialMessages::BaseUrl().IsEmpty())
            return BuildResponse_(403, "{\"error\":\"a message that expires is not offered on this server: no public address is set for its links (confidential_base_url)\",\"code\":\"confidential_unavailable\"}");
         if (!JsonUtf8Value_(requestBody, "mime").IsEmpty())
            return BuildResponse_(400, "{\"error\":\"a message the page signed or encrypted cannot be one that expires: the reading page could not show it\"}");
         passcode = JsonUtf8Value_(requestBody, "passcode");
         passcode.Trim();
         if (!passcode.IsEmpty() && (passcode.GetLength() < ConfidentialMessages::PasscodeMinimum || passcode.GetLength() > ConfidentialMessages::PasscodeMaximum))
         {
            AnsiString json;
            json.Format("{\"error\":\"passcode is %d to %d characters\"}", ConfidentialMessages::PasscodeMinimum, ConfidentialMessages::PasscodeMaximum);
            return BuildResponse_(400, json);
         }
         int recipientsLength = 0;
         for (size_t i = 0; i < recipients.size(); i++)
            recipientsLength += recipients[i].GetLength() + 1;
         if (recipientsLength > ConfidentialMessages::RecipientsMaximum)
            return BuildResponse_(400, "{\"error\":\"too many recipients for a message that expires\"}");
      }

      std::shared_ptr<SMTPConfiguration> smtpConfiguration = Configuration::Instance()->GetSMTPConfiguration();

      int maxRecipients = smtpConfiguration->GetMaxSMTPRecipientsInBatch();
      if (maxRecipients > 0 && (int) recipients.size() > maxRecipients)
      {
         AnsiString json;
         json.Format("{\"error\":\"too many recipients; the server allows %d\"}", maxRecipients);
         return BuildResponse_(400, json);
      }

      std::shared_ptr<Message> message = std::shared_ptr<Message>(new Message());
      message->SetFromAddress(from);

      // Every address is put through the two questions RCPT TO asks, as an
      // authenticated sender: may this sender deliver there, and what does
      // the address resolve to - so aliases, distribution lists, routes and
      // the relay rules apply unchanged, and a refused address is named.
      RecipientParser parser;
      for (size_t i = 0; i < recipients.size(); i++)
      {
         const String &address = recipients[i];

         if (!StringParser::IsValidEmailAddress(address))
            return BuildResponse_(400, "{\"error\":\"" + JsonEscape_(Utf8_(address)) + ": not an e-mail address\"}");

         String reason;
         bool treatSecurityAsLocal = false;
         RecipientParser::DeliveryPossibility possibility =
            parser.CheckDeliveryPossibility(true, from, address, reason, treatSecurityAsLocal, 0, true);

         if (possibility != RecipientParser::DP_Possible)
         {
            if (reason.IsEmpty())
            {
               if (possibility == RecipientParser::DP_RecipientUnknown)
                  reason = _T("unknown recipient");
               else if (possibility == RecipientParser::DP_MailboxFull)
                  reason = _T("the mailbox is full");
               else
                  reason = _T("delivery is not permitted");
            }

            return BuildResponse_(400, "{\"error\":\"" + JsonEscape_(Utf8_(address)) + ": " + JsonEscape_(Utf8_(reason)) + "\"}");
         }

         // The suppression list, as RCPT TO applies it to a submission: refused
         // here, where the page can tell the person who typed the address, and
         // nothing is queued. The code lets the page say it in their language,
         // and the source lets it say why - an entry an administrator added by
         // hand is not an address that bounced.
         {
            String suppressionCode;
            String suppressionText;
            String suppressionSource;

            if (BounceTracker::RefuseAtSubmission(from, address, suppressionCode, suppressionText, suppressionSource))
            {
               return BuildResponse_(400, "{\"error\":\"" + JsonEscape_(Utf8_(address)) + ": " + JsonEscape_(Utf8_(suppressionText)) +
                  "\",\"code\":\"recipient_suppressed\",\"address\":\"" + JsonEscape_(Utf8_(address)) +
                  "\",\"source\":\"" + JsonEscape_(Utf8_(suppressionSource)) + "\"}");
            }
         }

         bool recipientOK = false;
         parser.CreateMessageRecipientList(address, message->GetRecipients(), recipientOK);
         if (!recipientOK)
            return BuildResponse_(400, "{\"error\":\"" + JsonEscape_(Utf8_(address)) + ": unknown recipient\"}");
      }

      if (message->GetRecipients()->GetCount() == 0)
         return BuildResponse_(400, "{\"error\":\"no address resolved to a recipient\"}");

      // The message: the sender's name and address, the lists as written,
      // a Date and a Message-ID, the text as UTF-8. Bcc goes to the envelope
      // and nowhere else.
      const String fileName = PersistentMessage::GetFileName(message);

      // "mime": a message the page built itself - a signed or an encrypted one,
      // whose body bytes must reach the recipient exactly as signed. The
      // entity (its Content-Type headers, a blank line, its body) is written
      // under this server's own RFC 5322 headers; text, html and attachments
      // are not read. RestApiSmime.cpp.
      String mimeEntity = JsonUtf8Value_(requestBody, "mime");
      if (!mimeEntity.IsEmpty())
      {
         AnsiString problem;
         int refused = WriteMimeEntity_(account, fromHeader, JoinEntries(toEntries), ccEntries.empty() ? String() : JoinEntries(ccEntries), requestBody, mimeEntity, fileName, problem);
         if (refused != 0)
            return BuildResponse_(refused, problem);
      }
      else
      {

         MessageData messageData;
         messageData.LoadFromMessage(fileName, message);
         messageData.SetCharset(_T("utf-8"));
         messageData.SetFrom(fromHeader);
         messageData.SetTo(JoinEntries(toEntries));
         if (!ccEntries.empty())
            messageData.SetCC(JoinEntries(ccEntries));
         messageData.SetSubject(JsonUtf8Value_(requestBody, "subject"));
         messageData.SetBody(JsonUtf8Value_(requestBody, "text"));
         {
            // html beside text: the message goes as multipart/alternative, the
            // text part first, as a client with a formatting bar writes it.
            String html = JsonUtf8Value_(requestBody, "html");
            if (!html.IsEmpty())
               messageData.SetHTMLBody(html);
         }
         messageData.SetSentTime(Time::GetCurrentMimeDate());
         messageData.GenerateMessageID();

         // A reply names what it answers, so the recipient's client threads it.
         String inReplyTo = JsonUtf8Value_(requestBody, "in_reply_to");
         String references = JsonUtf8Value_(requestBody, "references");
         if (!inReplyTo.IsEmpty())
            messageData.SetFieldValue(_T("In-Reply-To"), inReplyTo);
         if (!references.IsEmpty())
            messageData.SetFieldValue(_T("References"), references);

         // receipt: true asks the recipient's client to say when it was read.
         if (GetJsonBoolValue_(requestBody, "receipt", false))
            messageData.SetFieldValue(_T("Disposition-Notification-To"), fromHeader);

         AnsiString attachmentError;
         int attachmentStatus = AddAttachmentsFromJson_(messageData, requestBody, attachmentError);
         if (attachmentStatus != 0)
            return BuildResponse_(attachmentStatus, attachmentError);

         // Images the HTML holds as data: URLs go as cid: parts - last, after the attachments (InlineImageParts.h).
         InlineImageParts::FromDataUrls(messageData);

         if (!messageData.Write(fileName))
            return BuildResponse_(500, "{\"error\":\"the message could not be written\"}");
      }

      // The domain's signature and disclaimer, by the rules SMTP submission
      // applies, before the size is taken and the \Sent copy made.
      OutgoingFooters::AddToWebmailMessage(message, account);

      // The rules that block, asked here as the SMTP session asks them at the
      // end of DATA and with the same collections (RuleApplier): the page is
      // the sender on the line. A message queued past a reject rule would be
      // dropped by the deliverer with "Sent" in the folder and the sender none
      // the wiser; refused here, with the rule's text, nothing is queued or
      // kept. Before the confidential rewrite below, which replaces the file
      // with the notice: what the rule reads is the message that would go.
      {
         String refusal;
         if (RuleApplier::RejectedBySubmissionRules(message, refusal))
         {
            FileUtilities::DeleteFile(fileName);
            return BuildResponse_(403, "{\"error\":\"" + JsonEscape_(Utf8_(refusal)) + "\",\"code\":\"policy_rejected\"}");
         }
      }

      message->SetSize((int) FileUtilities::FileSize(fileName));

      int maxKB = smtpConfiguration->GetMaxMessageSize();
      if (maxKB > 0 && (__int64) message->GetSize() > (__int64) maxKB * 1024)
      {
         FileUtilities::DeleteFile(fileName);
         return BuildResponse_(413, "{\"error\":\"the message is larger than the server allows\"}");
      }

      // A message that expires: the full message, footers and all, is kept on
      // this server and what goes into the queue in its place is the notice
      // with the link. Done before the queue row exists, so the deliverer can
      // never pick up the full message; the \Sent copy below is the full one.
      ConfidentialMessages::Entry kept;
      String keptPath;
      if (confidential)
      {
         // The kept copy is the mailbox's, kept elsewhere: it counts against
         // the quota with everything else kept for the account, and an account
         // has at most so many outstanding, so the data disk is bounded by the
         // quotas and not by the request rate.
         const __int64 quotaBytes = (__int64) account->GetAccountMaxSize() * 1024 * 1024;
         if (quotaBytes > 0 && AccountSizeCache::Instance()->GetSize(account->GetID()) + ConfidentialMessages::KeptBytes(account->GetID()) + (__int64) message->GetSize() > quotaBytes)
         {
            FileUtilities::DeleteFile(fileName);
            return BuildResponse_(413, "{\"error\":\"the mailbox's quota has no room for the copy this server keeps of a message that expires\",\"code\":\"quota_exceeded\"}");
         }
         const int live = ConfidentialMessages::CountLive(account->GetID());
         if (live < 0)
         {
            FileUtilities::DeleteFile(fileName);
            return BuildResponse_(500, "{\"error\":\"the messages that expire could not be counted\"}");
         }
         if (live >= ConfidentialMessages::LiveMaximum)
         {
            FileUtilities::DeleteFile(fileName);
            AnsiString json;
            json.Format("{\"error\":\"this account has %d messages that expire outstanding, which is the most it may; withdraw some, or wait for them to expire\",\"code\":\"confidential_ceiling\"}", ConfidentialMessages::LiveMaximum);
            return BuildResponse_(400, json);
         }

         const __int64 expires = (__int64) time(nullptr) + expiresIn;
         if (!ConfidentialMessages::Create(account, fileName, JsonUtf8Value_(requestBody, "subject"), fromHeader, recipients, expires, passcode, kept))
         {
            FileUtilities::DeleteFile(fileName);
            return BuildResponse_(500, "{\"error\":\"the message could not be kept\"}");
         }
         keptPath = ConfidentialMessages::PathOf(kept);

         const AnsiString link = Utf8_(ConfidentialMessages::BaseUrl()) + "/confidential/" + kept.token;
         if (!ConfidentialMessages::WriteNotice(message, fileName, fromHeader, JoinEntries(toEntries), ccEntries.empty() ? String() : JoinEntries(ccEntries),
                                                JsonUtf8Value_(requestBody, "subject"), ConfidentialMessages::DisplayName(fromHeader), link, expires, !passcode.IsEmpty()))
         {
            ConfidentialMessages::Discard(kept);
            FileUtilities::DeleteFile(fileName);
            return BuildResponse_(500, "{\"error\":\"the notice could not be written\"}");
         }
         message->SetSize((int) FileUtilities::FileSize(fileName));
      }

      // The shape of a takeover (CompromiseDetector.h), judged before the row
      // exists: a message in that shape is saved as HeldForReview, which the
      // deliverer never selects, rather than as Delivering. The caller is
      // answered as for a queued message either way.
      CompromiseDetector::Assessment shape;
      CompromiseDetector::Assess(account, caller.peer.ToString(), recipients, CompromiseDetector::SourceWebmail, shape);
      const bool heldForReview = shape.hold;

      message->SetState(heldForReview ? Message::HeldForReview : Message::Delivering);

      if (!PersistentMessage::SaveObject(message))
      {
         if (confidential)
            ConfidentialMessages::Discard(kept);
         FileUtilities::DeleteFile(fileName);
         return BuildResponse_(500, "{\"error\":\"the message could not be queued\"}");
      }

      {
         String recipientList;
         for (size_t i = 0; i < recipients.size(); i++)
            recipientList += (i ? _T(", ") : _T("")) + recipients[i];
         CompromiseDetector::Apply(shape, message->GetID(), from, recipientList, JsonUtf8Value_(requestBody, "subject"), (__int64) message->GetSize());
      }

      // A copy for the folder designated \Sent, marked read, before the queue
      // takes the file - when the account has such a folder and its quota
      // allows. Without one the message is sent and not kept, as over SMTP. A
      // message held for review gets its copy when it is released
      // (CompromiseDetector::Release, through MessageUtilities::CopyToDesignatedSent):
      // a copy of a message that is then discarded would say it was sent.
      __int64 sentId = 0;
      std::shared_ptr<IMAPFolder> sent = heldForReview ? std::shared_ptr<IMAPFolder>() : FindDesignatedFolder_(account, IMAPSpecialUse::DesignationSent);
      if (sent)
      {
         __int64 maxBytes = (__int64) account->GetAccountMaxSize() * 1024 * 1024;
         __int64 used = AccountSizeCache::Instance()->GetSize(account->GetID());
         const __int64 copySize = confidential ? (__int64) FileUtilities::FileSize(keptPath) : (__int64) message->GetSize();
         bool fits = maxBytes <= 0 || used + copySize <= maxBytes;

         if (fits && RightOn_(account, sent, ACLPermission::PermissionInsert))
         {
            // The queued file of a message that expires is the notice; the copy
            // the sender keeps is the message itself, from the kept file.
            std::shared_ptr<Message> copy = confidential
               ? ConfidentialMessages::CopyKeptToFolder(keptPath, from, sent)
               : PersistentMessage::CopyToIMAPFolder(message, sent);
            if (copy)
            {
               copy->SetFlagSeen(true);

               if (PersistentMessage::SaveObject(copy))
               {
                  sentId = copy->GetID();
                  sent->GetMessages()->Refresh(false);

                  std::shared_ptr<ChangeNotification> notification =
                     std::shared_ptr<ChangeNotification>(new ChangeNotification(account->GetID(), sent->GetID(), ChangeNotification::NotificationMessageAdded));
                  Application::Instance()->GetNotificationServer()->SendNotification(notification);
               }
               else
               {
                  FileUtilities::DeleteFile(PersistentMessage::GetFileName(account, copy));
               }
            }
         }
      }

      if (!heldForReview)
         Application::Instance()->SubmitPendingEmail();

      // The message this answers, when the caller names it, gets \Answered -
      // what a mail client does on send, under the right STORE would ask for.
      __int64 answeredId = 0;
      if (JsonNumber(requestBody, "answered_id", answeredId) && answeredId > 0)
      {
         std::shared_ptr<IMAPFolder> answeredFolder;
         std::shared_ptr<Message> answered = FindOwnMessage_(account, answeredId, answeredFolder);
         if (answered && !answered->GetFlagAnswered() && RightOn_(account, answeredFolder, ACLPermission::PermissionWriteOthers))
         {
            answered->SetFlagAnswered(true);
            if (Application::Instance()->GetFolderManager()->UpdateMessageFlags((int) answeredFolder->GetAccountID(), (int) answeredFolder->GetID(), answered->GetID(), answered->GetFlags()))
            {
               std::vector<__int64> changed;
               changed.push_back(answered->GetID());
               NotifyFolder(answeredFolder, ChangeNotification::NotificationMessageFlagsChanged, changed);
            }
         }
      }

      AnsiString json;
      // Every recipient becomes a contact of the account, once, so the To field
      // completes from what the account has written to (RestApiContacts.cpp).
      CollectContacts_(caller.account, toEntries);
      CollectContacts_(caller.account, ccEntries);
      CollectContacts_(caller.account, bccEntries);

      // held says whether the message went to the queue or to the review
      // page; queued stays true for both, as the SMTP reply stays 250, because
      // the message is this server's now and what the caller does next is the
      // same. The account's browser sessions are ended by the hold itself.
      if (confidential)
         json.Format("{\"queued\":true,\"held\":%hs,\"recipients\":%d,\"sent_id\":%I64d,\"confidential_id\":%I64d,\"expires\":%I64d}", heldForReview ? "true" : "false", message->GetRecipients()->GetCount(), sentId, kept.id, kept.expires);
      else
         json.Format("{\"queued\":true,\"held\":%hs,\"recipients\":%d,\"sent_id\":%I64d}", heldForReview ? "true" : "false", message->GetRecipients()->GetCount(), sentId);
      return BuildResponse_(201, json);
   }

   namespace
   {
      // RFC 8187: the bytes of a UTF-8 name that are not attr-char, percent-encoded.
      AnsiString PercentEncodeUtf8(const String &value)
      {
         AnsiString utf8;
         Unicode::WideToMultiByte(value, utf8);

         static const char *hex = "0123456789ABCDEF";
         AnsiString encoded;
         for (int i = 0; i < utf8.GetLength(); i++)
         {
            unsigned char c = (unsigned char) utf8.c_str()[i];
            bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                         c == '-' || c == '.' || c == '_' || c == '~';
            if (plain)
            {
               encoded += (char) c;
               continue;
            }

            encoded += '%';
            encoded += hex[c >> 4];
            encoded += hex[c & 15];
         }
         return encoded;
      }

      // The name as a quoted-string an old client can read: ASCII only, and
      // nothing that would end the string or the header.
      AnsiString AsciiFileName(const String &value)
      {
         AnsiString ascii;
         for (int i = 0; i < value.GetLength(); i++)
         {
            wchar_t c = value.c_str()[i];
            bool safe = c >= 0x20 && c < 0x7F && c != '"' && c != '\\';
            ascii += safe ? (char) c : '_';
         }
         if (ascii.IsEmpty())
            ascii = "attachment";
         return ascii;
      }

      // A media type that a browser would run or render is not served under
      // it: with the session cookie attached, an attachment that rendered as
      // a page on this origin could act as the page. Everything else keeps
      // its type; the disposition, nosniff and the sandbox policy hold too.
      AnsiString SafeMediaType(const AnsiString &declared)
      {
         // The same media type the message listing reports in content_type, so
         // that a page which decided from that field what the part is gets the
         // very same type when it fetches the bytes - which is what lets an
         // <img> pointed at this route render, with nosniff set.
         AnsiString type = MediaType(declared);

         if (type.IsEmpty())
            return "application/octet-stream";

         if (type == "text/html" || type == "application/xhtml+xml" || type == "image/svg+xml" ||
             type == "text/xml" || type == "application/xml" || type.EndsWith("+xml") ||
             type == "text/javascript" || type == "application/javascript")
            return "application/octet-stream";

         return type;
      }
   }

   HttpResponse
   RestApiServer::HandleMeMessageAttachment_(const Caller &caller, __int64 messageId, int index)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      std::shared_ptr<IMAPFolder> folder;
      std::shared_ptr<Message> message = FindOwnMessage_(account, messageId, folder);
      if (!message)
         return BuildResponse_(404, "{\"error\":\"message not found\"}");

      if (message->GetSize() > MaxMessageParseBytes)
         return BuildResponse_(413, "{\"error\":\"the message is too large to read here\"}");

      String fileName = MessageFile_(message);
      if (!FileUtilities::Exists(fileName))
         return BuildResponse_(404, "{\"error\":\"the message file is missing\"}");

      MessageData messageData;
      if (!messageData.LoadFromMessage(MessageFile_(message), message))
         return BuildResponse_(500, "{\"error\":\"the message could not be parsed\"}");

      std::shared_ptr<Attachments> attachments = messageData.GetAttachments();
      if (!attachments || index < 0 || index >= (int) attachments->GetCount())
         return BuildResponse_(404, "{\"error\":\"attachment not found\"}");

      std::shared_ptr<Attachment> attachment = attachments->GetItem((unsigned int) index);
      if (!attachment)
         return BuildResponse_(404, "{\"error\":\"attachment not found\"}");

      AnsiString bytes;
      if (!attachment->GetContent(bytes))
         return BuildResponse_(500, "{\"error\":\"the attachment could not be decoded\"}");

      String name = attachment->GetFileName();
      if (name.IsEmpty())
         name = _T("attachment");

      HttpResponse response;
      response.status = 200;
      response.content_type = SafeMediaType(attachment->GetContentType());
      response.body = bytes;
      // Never compressed. The media type on this answer is the one the part
      // declared, which is to say the one whoever sent the message chose: a
      // JPEG, a PDF, a zip and a 7-Zip backup are all things DEFLATE would
      // spend a worker thread making very slightly larger, and a part that says
      // "text/plain" over thirty megabytes of anything is not a reason to
      // believe it. A download is bytes to keep, not a document to render.
      response.no_compress = true;
      response.extra_headers =
         "Content-Disposition: attachment; filename=\"" + AsciiFileName(name) + "\"; filename*=UTF-8''" + PercentEncodeUtf8(name) + "\r\n"
         "X-Content-Type-Options: nosniff\r\n"
         "Content-Security-Policy: sandbox\r\n"
         "Cache-Control: no-store\r\n";
      return response;
   }

   namespace
   {
      // The text of one object in a JSON body - for {"forwarding":{...}} the
      // braces and what is between them - or empty when the key is absent.
      // Keys inside two objects may repeat (enabled), so each object is read
      // on its own.
      AnsiString JsonObject(const AnsiString &json, const AnsiString &key)
      {
         AnsiString needle = "\"" + key + "\"";
         int keyPosition = json.Find(needle);
         if (keyPosition < 0)
            return "";

         int open = json.Find("{", keyPosition + needle.GetLength());
         if (open < 0)
            return "";

         int depth = 0;
         bool inString = false;
         for (int i = open; i < json.GetLength(); i++)
         {
            char c = json[i];

            if (inString)
            {
               if (c == '\\')
               {
                  i++;
                  continue;
               }

               if (c == '\"')
                  inString = false;

               continue;
            }

            if (c == '\"')
            {
               inString = true;
               continue;
            }

            if (c == '{')
               depth++;
            else if (c == '}')
            {
               depth--;
               if (depth == 0)
                  return json.Mid(open, i - open + 1);
            }
         }

         return "";
      }

      const int MaxNameLength = 100;
      const int MaxSignatureLength = 20000;
      // The formatted signature, counted in bytes of UTF-8 with its images in it:
      // the webmail keeps an image there as a data: URL. The narrowest column
      // decides - hm_accounts.accountsignaturehtml is text on MySQL, 65,535
      // bytes, and ntext or text, far larger, on the other three - so 64,000,
      // a number a person can be told, fits every backend. Portal.js holds the
      // same number (SIGNATURE_HTML_BYTES), and this route is a large request
      // (IsLargeRequest_) so that the listener's 64 KB cap is not what says no.
      const int MaxSignatureHtmlBytes = 64000;
      const int MaxScriptLength = 256 * 1024;
   }

   AnsiString
   RestApiServer::SettingsJson_(std::shared_ptr<const Account> account)
   {
      AnsiString json;
      json.Format("{\"name\":{\"first\":\"%hs\",\"last\":\"%hs\"},"
                  "\"forwarding\":{\"enabled\":%hs,\"address\":\"%hs\",\"keep_original\":%hs},"
                  "\"signature\":{\"enabled\":%hs,\"text\":\"%hs\",\"html\":\"%hs\"}}",
         JsonEscape_(Utf8_(account->GetPersonFirstName())).c_str(),
         JsonEscape_(Utf8_(account->GetPersonLastName())).c_str(),
         account->GetForwardEnabled() ? "true" : "false",
         JsonEscape_(Utf8_(account->GetForwardAddress())).c_str(),
         account->GetForwardKeepOriginal() ? "true" : "false",
         account->GetEnableSignature() ? "true" : "false",
         JsonEscape_(Utf8_(account->GetSignaturePlainText())).c_str(),
         JsonEscape_(Utf8_(account->GetSignatureHTML())).c_str());
      return json;
   }

   HttpResponse
   RestApiServer::HandleMeSettings_(const Caller &caller)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      return BuildResponse_(200, SettingsJson_(account));
   }

   HttpResponse
   RestApiServer::HandleMeSettingsPut_(const Caller &caller, const AnsiString &requestBody)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      // Each of the three objects the body names is applied whole; one it
      // does not name is left as it is.
      AnsiString nameObject = JsonObject(requestBody, "name");
      AnsiString forwardingObject = JsonObject(requestBody, "forwarding");
      AnsiString signatureObject = JsonObject(requestBody, "signature");

      if (nameObject.IsEmpty() && forwardingObject.IsEmpty() && signatureObject.IsEmpty())
         return BuildResponse_(400, "{\"error\":\"nothing to change: name, forwarding or signature\"}");

      std::shared_ptr<Account> mutableAccount = std::shared_ptr<Account>(new Account());
      if (!PersistentAccount::ReadObject(mutableAccount, account->GetID()) || mutableAccount->GetID() == 0)
         return BuildResponse_(500, "{\"error\":\"the account could not be read\"}");

      if (!nameObject.IsEmpty())
      {
         String first = JsonUtf8Value_(nameObject, "first");
         String last = JsonUtf8Value_(nameObject, "last");
         if (first.GetLength() > MaxNameLength || last.GetLength() > MaxNameLength)
            return BuildResponse_(400, "{\"error\":\"a name is at most 100 characters\"}");

         mutableAccount->SetPersonFirstName(first);
         mutableAccount->SetPersonLastName(last);
      }

      if (!forwardingObject.IsEmpty())
      {
         bool enabled = GetJsonBoolValue_(forwardingObject, "enabled", false);
         String address = JsonUtf8Value_(forwardingObject, "address");
         bool keepOriginal = GetJsonBoolValue_(forwardingObject, "keep_original", true);
         address.TrimLeft();
         address.TrimRight();

         if (enabled)
         {
            if (!StringParser::IsValidEmailAddress(address))
               return BuildResponse_(400, "{\"error\":\"forwarding needs an e-mail address\"}");

            if (address.CompareNoCase(account->GetAddress()) == 0)
               return BuildResponse_(400, "{\"error\":\"forwarding to the account itself would loop\"}");
         }

         mutableAccount->SetForwardEnabled(enabled);
         mutableAccount->SetForwardAddress(address);
         mutableAccount->SetForwardKeepOriginal(keepOriginal);
      }

      if (!signatureObject.IsEmpty())
      {
         bool enabled = GetJsonBoolValue_(signatureObject, "enabled", false);
         String text = JsonUtf8Value_(signatureObject, "text");
         String html = JsonUtf8Value_(signatureObject, "html");
         if (text.GetLength() > MaxSignatureLength)
            return BuildResponse_(400, "{\"error\":\"a signature is at most 20000 characters\"}");
         if (Utf8_(html).GetLength() > MaxSignatureHtmlBytes)
            return BuildResponse_(400, "{\"error\":\"a formatted signature is at most 64 KB (64,000 bytes), its images included\"}");

         mutableAccount->SetEnableSignature(enabled);
         mutableAccount->SetSignaturePlainText(text);
         mutableAccount->SetSignatureHTML(html);
      }

      String saveError;
      if (!PersistentAccount::SaveObject(mutableAccount, saveError, PersistenceModeNormal))
      {
         AnsiString body;
         body.Format("{\"error\":\"%hs\"}", JsonEscape_(Utf8_(saveError.IsEmpty() ? String(_T("the account could not be saved")) : saveError)).c_str());
         return BuildResponse_(500, body);
      }

      return BuildResponse_(200, SettingsJson_(mutableAccount));
   }

   HttpResponse
   RestApiServer::HandleMeFilters_(const Caller &caller)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      AnsiString json;
      json.Format("{\"active\":\"%hs\",\"name\":\"%hs\"}",
         JsonEscape_(Utf8_(SieveStorage::GetActiveScript(account->GetAddress()))).c_str(),
         JsonEscape_(Utf8_(SieveStorage::GetActiveScriptName(account->GetAddress()))).c_str());
      return BuildResponse_(200, json);
   }

   HttpResponse
   RestApiServer::HandleMeFiltersPut_(const Caller &caller, const AnsiString &requestBody)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      if (requestBody.Find("\"script\"") < 0)
         return BuildResponse_(400, "{\"error\":\"script is required; an empty script removes the filter\"}");

      String script = JsonUtf8Value_(requestBody, "script");
      if (script.GetLength() > MaxScriptLength)
         return BuildResponse_(400, "{\"error\":\"a script is at most 256 KB\"}");

      // The check ManageSieve's PUTSCRIPT and CHECKSCRIPT make, with the
      // same wording; a script that does not parse is not stored.
      if (!script.IsEmpty())
      {
         String syntaxError = SieveScript::CheckSyntax(script);
         if (!syntaxError.IsEmpty())
         {
            AnsiString body;
            body.Format("{\"error\":\"%hs\"}", JsonEscape_(Utf8_(syntaxError)).c_str());
            return BuildResponse_(400, body);
         }
      }

      if (!SieveStorage::SetActiveScript(account->GetAddress(), script))
         return BuildResponse_(500, "{\"error\":\"the script could not be stored\"}");

      AnsiString json;
      json.Format("{\"active\":\"%hs\"}", JsonEscape_(Utf8_(script)).c_str());
      return BuildResponse_(200, json);
   }

   // The three headers a thread is built from, read from the head of the
   // file the way the listing reads Subject and From.
   AnsiString
   RestApiServer::ThreadFieldsJson_(const String &fileName)
   {
      AnsiString messageId, inReplyTo, references;

      AnsiString header = PersistentMessage::LoadHeader(fileName, false);
      if (!header.IsEmpty())
      {
         MimeHeader mimeHeader;
         mimeHeader.Load(header.c_str(), header.GetLength(), true);

         const char *value = mimeHeader.GetRawFieldValue("Message-ID");
         messageId = value ? value : "";
         value = mimeHeader.GetRawFieldValue("In-Reply-To");
         inReplyTo = value ? value : "";
         value = mimeHeader.GetRawFieldValue("References");
         references = value ? value : "";

         messageId.TrimLeft();
         messageId.TrimRight();
         inReplyTo.TrimLeft();
         inReplyTo.TrimRight();
         references.TrimLeft();
         references.TrimRight();
      }

      AnsiString json;
      json.Format("\"message_id\":\"%hs\",\"in_reply_to\":\"%hs\",\"references\":\"%hs\"",
         JsonEscape_(messageId).c_str(), JsonEscape_(inReplyTo).c_str(), JsonEscape_(references).c_str());
      return json;
   }

   // What a mail client's list shows beside the subject, from the file: the
   // recipients (a Sent folder lists who a message went to), whether the
   // message carries attachments (what the search's has:attachment reads: a
   // mixed multipart or a part declared one), and a snippet of the text - the
   // first hundred and sixty characters that are not a quotation, or the
   // text of an HTML-only message with its markup removed. The text needs the
   // message parsed, which MessageData caps by size; a message over the cap
   // has no snippet. And the tab of the inbox the message belongs under, see
   // CategoryOf. Kept per message id, since a folder is listed again at every
   // change the probe reports and a message file does not change.
   namespace
   {
      struct ListingExtras
      {
         AnsiString json;
         // The domain a dmarc=pass was recorded for; whether it is shown is the
         // switch's, read as the listing is written, so it is kept apart.
         AnsiString brand;
      };

      boost::mutex listingExtrasLock;
      std::map<__int64, ListingExtras> listingExtras;
      const size_t ListingExtrasKept = 4000;
      const int SnippetLength = 160;

      // The tab of the inbox a message belongs under, the way Gmail sorts an
      // inbox, decided from the header alone: social when the sender is one of
      // the networks; forums when it came through a list a person can post to
      // (List-Post, or Precedence: list); promotions when it carries an
      // unsubscribe link or was sent in bulk; updates when a machine sent it -
      // Auto-Submitted, or an address such as no-reply, notifications or
      // receipts; and primary for the rest. The page sorts its inbox by it and
      // lets the reader overrule it per sender.
      const char *const SocialDomains[] =
      {
         "facebook.com", "facebookmail.com", "twitter.com", "x.com", "linkedin.com", "instagram.com",
         "pinterest.com", "reddit.com", "redditmail.com", "tiktok.com", "youtube.com", "discord.com",
         "snapchat.com", "threads.net", "tumblr.com", "meetup.com", "nextdoor.com", "quora.com",
         "strava.com", "bsky.app", "mastodon.social", "twitch.tv", "flickr.com", "goodreads.com",
         "yelp.com", "whatsapp.com", "telegram.org", 0
      };

      const char *const MachineSenders[] =
      {
         "noreply", "no-reply", "no_reply", "donotreply", "do-not-reply", "do_not_reply", "notification",
         "notifications", "notify", "alert", "alerts", "mailer-daemon", "postmaster", "receipt", "receipts",
         "billing", "invoice", "invoices", "order", "orders", "statement", "statements", "security",
         "verify", "verification", "confirm", "confirmation", "update", "updates", 0
      };

      AnsiString LowerHeaderValue(const MimeHeader &header, const char *name)
      {
         const char *value = header.GetRawFieldValue(name);
         AnsiString out = value ? value : "";
         out.ToLower();
         return out;
      }

      AnsiString CategoryOf(const MimeHeader &header)
      {
         AnsiString from = LowerHeaderValue(header, "From");
         int open = from.Find("<");
         int close = from.Find(">");
         if (open >= 0 && close > open)
            from = from.Mid(open + 1, close - open - 1);
         from.Trim();
         int at = from.Find("@");
         AnsiString local = at >= 0 ? from.Mid(0, at) : from;
         AnsiString domain = at >= 0 ? from.Mid(at + 1) : AnsiString();

         for (int i = 0; SocialDomains[i]; i++)
         {
            AnsiString social = SocialDomains[i];
            AnsiString suffix = ".";
            suffix += social;
            if (domain == social || domain.EndsWith(suffix.c_str()))
               return "social";
         }

         AnsiString precedence = LowerHeaderValue(header, "Precedence");
         if (header.FieldExists("List-Post") || precedence.Find("list") >= 0)
            return "forums";
         if (header.FieldExists("List-Unsubscribe") || precedence.Find("bulk") >= 0)
            return "promotions";

         AnsiString autoSubmitted = LowerHeaderValue(header, "Auto-Submitted");
         if ((!autoSubmitted.IsEmpty() && !autoSubmitted.StartsWith("no")) || header.FieldExists("X-Auto-Response-Suppress"))
            return "updates";
         for (int i = 0; MachineSenders[i]; i++)
            if (local == MachineSenders[i])
               return "updates";

         return "primary";
      }

      String TextWithoutMarkup(const String &html)
      {
         String out;
         out.reserve(html.GetLength());
         bool inTag = false;
         bool inStyle = false;
         String lower = html;
         lower.ToLower();
         for (int i = 0; i < html.GetLength(); i++)
         {
            const wchar_t c = html[i];
            if (c == '<')
            {
               inTag = true;
               if (lower.Mid(i, 6) == _T("<style") || lower.Mid(i, 7) == _T("<script"))
                  inStyle = true;
               else if (lower.Mid(i, 8) == _T("</style>") || lower.Mid(i, 9) == _T("</script>"))
                  inStyle = false;
               continue;
            }
            if (c == '>')
            {
               inTag = false;
               out += ' ';
               continue;
            }
            if (!inTag && !inStyle)
               out += c;
         }
         out.Replace(_T("&nbsp;"), _T(" "));
         out.Replace(_T("&amp;"), _T("&"));
         out.Replace(_T("&lt;"), _T("<"));
         out.Replace(_T("&gt;"), _T(">"));
         out.Replace(_T("&quot;"), _T("\""));
         out.Replace(_T("&#39;"), _T("'"));
         return out;
      }

      String SnippetOf(const String &text)
      {
         // Lines that quote an earlier message are not the message.
         String kept;
         std::vector<String> lines = StringParser::SplitString(text, _T("\n"));
         for (size_t i = 0; i < lines.size(); i++)
         {
            String line = lines[i];
            line.TrimLeft();
            if (line.IsEmpty() || line[0] == '>')
               continue;
            if (line == _T("-- "))
               break;
            kept += line + _T(" ");
            if (kept.GetLength() > SnippetLength * 3)
               break;
         }
         String out;
         bool space = true;
         for (int i = 0; i < kept.GetLength() && out.GetLength() < SnippetLength; i++)
         {
            const wchar_t c = kept[i];
            const bool blank = c == ' ' || c == '\t' || c == '\r' || c == '\n';
            if (blank)
            {
               if (!space)
                  out += ' ';
               space = true;
               continue;
            }
            out += c;
            space = false;
         }
         out.TrimRight();
         return out;
      }
   }

   AnsiString
   RestApiServer::ListingExtrasJson_(const String &fileName, std::shared_ptr<Message> message)
   {
      const __int64 id = message->GetID();
      {
         boost::lock_guard<boost::mutex> guard(listingExtrasLock);
         std::map<__int64, ListingExtras>::const_iterator known = listingExtras.find(id);
         if (known != listingExtras.end())
            return known->second.json + BrandJson_(known->second.brand);
      }

      String to;
      bool attachments = false;
      AnsiString category = "primary";
      AnsiString brand;
      AnsiString header = PersistentMessage::LoadHeader(fileName, false);
      if (!header.IsEmpty())
      {
         MimeHeader mimeHeader;
         mimeHeader.Load(header.c_str(), header.GetLength(), true);
         to = mimeHeader.GetUnicodeFieldValue("To");
         brand = BrandOf(OwnAuthenticationResults_(mimeHeader));
         category = CategoryOf(mimeHeader);

         AnsiString lower = header;
         lower.ToLower();
         attachments = lower.Find("multipart/mixed") >= 0 || lower.Find("content-disposition: attachment") >= 0;
      }

      String snippet;
      MessageData data;
      if (data.LoadFromMessage(fileName, message))
      {
         String text = data.GetBody();
         if (text.IsEmpty())
            text = TextWithoutMarkup(data.GetHTMLBody());
         snippet = SnippetOf(text);
      }

      AnsiString json;
      json.Format("\"to\":\"%hs\",\"has_attachments\":%hs,\"snippet\":\"%hs\",\"category\":\"%hs\"",
         JsonEscape_(Utf8_(to)).c_str(),
         attachments ? "true" : "false",
         JsonEscape_(Utf8_(snippet)).c_str(),
         category.c_str());

      boost::lock_guard<boost::mutex> guard(listingExtrasLock);
      if (listingExtras.size() >= ListingExtrasKept)
         listingExtras.clear();
      listingExtras[id].json = json;
      listingExtras[id].brand = brand;
      return json + BrandJson_(brand);
   }

   AnsiString
   RestApiServer::BrandJson_(const AnsiString &brand)
   {
      if (brand.IsEmpty() || !Configuration::Instance()->GetBrandLogosEnabled())
         return "";
      return ",\"brand\":\"" + JsonEscape_(brand) + "\"";
   }

   // "First Last" <address>, or the bare address when the account has no name.
   String
   RestApiServer::FromHeader_(std::shared_ptr<const Account> account)
   {
      String name = account->GetPersonFirstName();
      String lastName = account->GetPersonLastName();
      if (!lastName.IsEmpty())
      {
         if (!name.IsEmpty())
            name += _T(" ");
         name += lastName;
      }

      if (name.IsEmpty())
         return account->GetAddress();

      String header = _T("\"");
      header += name;
      header += _T("\" <");
      header += account->GetAddress();
      header += _T(">");
      return header;
   }

   HttpResponse
   RestApiServer::HandleMeDraftSave_(const Caller &caller, const AnsiString &requestBody)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      // The folder designated \Drafts, made as "Drafts" when the account has
      // none - what a mail client does the first time it saves one.
      std::shared_ptr<IMAPFolder> drafts = FindDesignatedFolder_(account, IMAPSpecialUse::DesignationDrafts);
      if (!drafts)
      {
         std::shared_ptr<IMAPFolders> folders = IMAPFolderContainer::Instance()->GetFoldersForAccount(account->GetID());
         if (!folders)
            return BuildResponse_(500, "{\"error\":\"the folders could not be read\"}");

         std::vector<String> path;
         path.push_back(_T("Drafts"));
         folders->CreatePath(folders, path, true);

         drafts = FindDesignatedFolder_(account, IMAPSpecialUse::DesignationDrafts);
         if (!drafts)
            return BuildResponse_(500, "{\"error\":\"the Drafts folder could not be created\"}");
      }

      if (!RightOn_(account, drafts, ACLPermission::PermissionInsert))
         return BuildResponse_(403, "{\"error\":\"the Drafts folder does not allow this account to add messages\"}");

      // A draft may carry a chosen From too, under the same rule as a send.
      String draftFrom;
      String draftFromHeader;
      {
         AnsiString problem;
         int refused = ResolveSender_(account, JsonUtf8Value_(requestBody, "from"), draftFrom, draftFromHeader, problem);
         if (refused != 0)
            return BuildResponse_(refused, problem);
      }

      // Written as the send route writes a message, and kept instead of
      // queued: Bcc stays in the draft, since nothing is sent.
      std::shared_ptr<Message> draft = std::shared_ptr<Message>(new Message());
      draft->SetAccountID(drafts->GetAccountID());
      draft->SetFolderID(drafts->GetID());

      const String fileName = PersistentMessage::GetFileName(account, draft);

      MessageData messageData;
      messageData.LoadFromMessage(fileName, draft);
      messageData.SetCharset(_T("utf-8"));
      messageData.SetFrom(draftFromHeader);
      messageData.SetTo(JsonUtf8Value_(requestBody, "to"));
      String cc = JsonUtf8Value_(requestBody, "cc");
      if (!cc.IsEmpty())
         messageData.SetCC(cc);
      String bcc = JsonUtf8Value_(requestBody, "bcc");
      if (!bcc.IsEmpty())
         messageData.SetBCC(bcc);
      messageData.SetSubject(JsonUtf8Value_(requestBody, "subject"));
      messageData.SetBody(JsonUtf8Value_(requestBody, "text"));
      {
         // html beside text: the message goes as multipart/alternative, the
         // text part first, as a client with a formatting bar writes it.
         String html = JsonUtf8Value_(requestBody, "html");
         if (!html.IsEmpty())
            messageData.SetHTMLBody(html);
      }
      messageData.SetSentTime(Time::GetCurrentMimeDate());
      messageData.GenerateMessageID();

      AnsiString attachmentError;
      int attachmentStatus = AddAttachmentsFromJson_(messageData, requestBody, attachmentError);
      if (attachmentStatus != 0)
         return BuildResponse_(attachmentStatus, attachmentError);

      // As on a send, so a draft sent later by the schedule, or opened in another client, shows its images.
      InlineImageParts::FromDataUrls(messageData);

      if (!messageData.Write(fileName))
         return BuildResponse_(500, "{\"error\":\"the draft could not be written\"}");

      draft->SetSize((int) FileUtilities::FileSize(fileName));

      if (!account->SpaceAvailable(draft->GetSize()))
      {
         FileUtilities::DeleteFile(fileName);
         return BuildResponse_(413, "{\"error\":\"the mailbox is full\"}");
      }

      draft->SetState(Message::Delivered);
      draft->SetFlagDraft(true);
      draft->SetFlagSeen(true);

      if (!PersistentMessage::SaveObject(draft))
      {
         FileUtilities::DeleteFile(fileName);
         return BuildResponse_(500, "{\"error\":\"the draft could not be saved\"}");
      }

      MessagesContainer::Instance()->SetFolderNeedsRefresh(drafts->GetID());

      std::shared_ptr<ChangeNotification> notification =
         std::shared_ptr<ChangeNotification>(new ChangeNotification(drafts->GetAccountID(), drafts->GetID(), ChangeNotification::NotificationMessageAdded));
      Application::Instance()->GetNotificationServer()->SendNotification(notification);

      // The draft this one replaces goes once the new one is safe - a new UID
      // for new content, as IMAP requires, and the old one expunged.
      __int64 replaceId = 0;
      if (JsonNumber(requestBody, "replace_id", replaceId) && replaceId > 0)
      {
         std::shared_ptr<IMAPFolder> oldFolder;
         std::shared_ptr<Message> old = FindOwnMessage_(account, replaceId, oldFolder);
         if (old && old->GetFlagDraft() &&
             RightOn_(account, oldFolder, ACLPermission::PermissionWriteDeleted) &&
             RightOn_(account, oldFolder, ACLPermission::PermissionExpunge))
            DeleteOwnMessage_(account, old, oldFolder);
      }

      AnsiString json;
      json.Format("{\"id\":%I64d,\"folder_id\":%I64d}", draft->GetID(), drafts->GetID());
      return BuildResponse_(201, json);
   }

   namespace
   {
      // What one message may carry from the portal. The request cap for the
      // two routes that take attachments is sixteen megabytes on the wire;
      // base64 adds a third, so twelve megabytes of files is what fits.
      const int MaxAttachmentsPerMessage = 20;
      const __int64 MaxAttachmentBytesPerMessage = 12 * 1024 * 1024;

      // The text of the n-th object in the "attachments" array of a JSON
      // body, by brace matching - or empty when there is none.
      AnsiString JsonArrayObject(const AnsiString &json, const AnsiString &key, int index)
      {
         AnsiString needle = "\"" + key + "\"";
         int keyPosition = json.Find(needle);
         if (keyPosition < 0)
            return "";

         int bracket = json.Find("[", keyPosition + needle.GetLength());
         if (bracket < 0)
            return "";

         int depth = 0;
         bool inString = false;
         int objectStart = -1;
         int found = 0;
         for (int i = bracket + 1; i < json.GetLength(); i++)
         {
            char c = json[i];

            if (inString)
            {
               if (c == '\\')
               {
                  i++;
                  continue;
               }

               if (c == '\"')
                  inString = false;

               continue;
            }

            if (c == '\"')
            {
               inString = true;
               continue;
            }

            if (c == '{')
            {
               if (depth == 0)
                  objectStart = i;
               depth++;
            }
            else if (c == '}')
            {
               depth--;
               if (depth == 0)
               {
                  if (found == index)
                     return json.Mid(objectStart, i - objectStart + 1);
                  found++;
               }
            }
            else if (c == ']' && depth == 0)
            {
               break;
            }
         }

         return "";
      }

      // A file name for the temporary file the MIME library reads: the name
      // the sender gave, minus anything a path or a header could not carry.
      String SafeAttachmentName(const String &name)
      {
         String safe;
         for (int i = 0; i < name.GetLength(); i++)
         {
            wchar_t c = name.c_str()[i];
            bool bad = c < 0x20 || c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '\"' || c == '<' || c == '>' || c == '|';
            safe += bad ? L'_' : c;
         }
         safe.TrimLeft();
         safe.TrimRight();
         if (safe.IsEmpty() || safe == _T(".") || safe == _T(".."))
            safe = _T("attachment");
         if (safe.GetLength() > 200)
            safe = safe.Mid(0, 200);
         return safe;
      }
   }

   // The attachments a body names - name, type, data (base64) - added to the
   // message the way COM's Attachments.Add adds a file: each is written under
   // its own name in a directory of its own in the temp folder, read from
   // there, and the directory removed. Returns 0, or the status to answer.
   int
   RestApiServer::AddAttachmentsFromJson_(MessageData &messageData, const AnsiString &requestBody, AnsiString &error)
   {
      if (requestBody.Find("\"attachments\"") < 0)
         return 0;

      __int64 totalBytes = 0;

      for (int index = 0; index < MaxAttachmentsPerMessage + 1; index++)
      {
         AnsiString object = JsonArrayObject(requestBody, "attachments", index);
         if (object.IsEmpty())
            break;

         if (index >= MaxAttachmentsPerMessage)
         {
            error.Format("{\"error\":\"at most %d attachments\"}", MaxAttachmentsPerMessage);
            return 400;
         }

         String name = SafeAttachmentName(JsonUtf8Value_(object, "name"));
         String type = JsonUtf8Value_(object, "type");
         AnsiString encoded = GetJsonStringValue_(object, "data");

         AnsiString bytes = Base64::Decode(encoded.c_str(), encoded.GetLength());
         totalBytes += bytes.GetLength();
         if (totalBytes > MaxAttachmentBytesPerMessage)
         {
            error = "{\"error\":\"the attachments are larger than twelve megabytes together\"}";
            return 413;
         }

         type.TrimLeft();
         type.TrimRight();
         if (type.IsEmpty() || type.Find(_T("/")) < 0 || type.Find(_T("\r")) >= 0 || type.Find(_T("\n")) >= 0 || type.GetLength() > 100)
            type = _T("application/octet-stream");

         String directory = IniFileSettings::Instance()->GetTempDirectory() + FileUtilities::PathSeparator + GUIDCreator::GetGUID();
         if (!FileUtilities::CreateDirectory(directory))
         {
            error = "{\"error\":\"the attachment could not be written\"}";
            return 500;
         }

         String path = directory + FileUtilities::PathSeparator + name;
         bool added = FileUtilities::WriteToFile(path, bytes) && messageData.GetAttachments()->Add(path, type);

         FileUtilities::DeleteDirectory(directory, true);

         if (!added)
         {
            error = "{\"error\":\"the attachment could not be added\"}";
            return 500;
         }

         std::shared_ptr<Attachments> attachments = messageData.GetAttachments();
         std::shared_ptr<Attachment> last = attachments->GetItem((unsigned int) attachments->GetCount() - 1);
         if (last)
            last->SetFileName(name);
      }

      return 0;
   }

   bool
   RestApiServer::IsLargeRequest_(const AnsiString &method, const AnsiString &target)
   {
      AnsiString path = target;
      int query = path.Find("?");
      if (query >= 0)
         path = path.Mid(0, query);

      // A chunk of a file sent as a link: raw bytes, up to the large cap. And the
      // account's settings, whose formatted signature may carry its images:
      // 64,000 bytes of HTML beside a 20,000-character plain signature, JSON-
      // escaped, is more than the ordinary cap - the route answers the exact
      // limit itself, as a sentence (HandleMeSettingsPut_).
      if (method == "PUT")
         return (path.StartsWith("/api/v1/me/files/") && path.EndsWith("/content")) || path == "/api/v1/me/settings";

      if (method != "POST")
         return false;

      if (path.StartsWith("/api/v1/me/folders/") && path.EndsWith("/messages"))
         return true;
      // A raw message into one of an account's folders, by the administrator
      // (RestApiAccountResources.cpp): the same 25 MB as the account's own import.
      if (path.StartsWith("/api/v1/accounts/") && path.Find("/folders/") > 0 && path.EndsWith("/messages"))
         return true;
      return path == "/api/v1/me/messages" || path == "/api/v1/me/drafts";
   }

   // The page, its script, its manifest and its service worker are files
   // beside RestApiPortal.cpp - Portal.html, Portal.js, Portal.webmanifest,
   // PortalServiceWorker.js - carried into the binary by
   // build/generate-portal-page.py as PortalPageData.cpp, each in pieces of
   // four thousand characters (a compiler's limit on one literal), joined here
   // on first use. The headers both are served with stay a literal in
   // RestApiPortal.cpp.
   extern const char *const PortalHtmlPieces[];
   extern const char *const PortalScriptPieces[];
   extern const char *const PortalManifestPieces[];
   extern const char *const PortalServiceWorkerPieces[];
   extern const char *PortalHeaders;

   namespace
   {
      const AnsiString &
      Joined_(const char *const *pieces, AnsiString &into, std::once_flag &once)
      {
         std::call_once(once, [&]()
         {
            for (const char *const *piece = pieces; *piece; piece++)
               into += *piece;
         });
         return into;
      }

      AnsiString portalHtml_;
      std::once_flag portalHtmlOnce_;

      // The three that go out unchanged are joined AND compressed once, and
      // held both ways for the life of the process. The page itself is not one
      // of them: every copy of it differs, because the branding, the
      // catalogue for the reader's language and the direction of the first
      // paint are substituted into it below. It goes through pageCache_
      // instead, which holds one compressed copy per distinct result - one
      // per language, in practice, until the branding is saved again.
      HttpStaticBody portalScript_, portalManifest_, portalServiceWorker_;
   }

   HttpResponse
   RestApiServer::HandlePortalPage_(const AnsiString &request)
   {
      HttpResponse response;
      response.content_type = "text/html; charset=utf-8";
      // The server-wide branding rides in the page as data, not as script:
      // a JSON block the policy does not run and the script reads for the
      // first paint. A domain's own arrives after sign-in.
      AnsiString page = Joined_(PortalHtmlPieces, portalHtml_, portalHtmlOnce_);
      page.Replace("<!--hm-branding-->", ("<script type=\"application/json\" id=\"branding-data\">" + BrandingJson_(String()) + "</script>").c_str());
      // The catalogue for the reader's Accept-Language rides in the page too,
      // so the first paint is already in their language; the script fetches
      // another on a choice. English is the page itself: no block.
      const AnsiString language = NegotiatePortalLanguage_(request);
      AnsiString languageBlock;
      if (!language.IsEmpty())
         languageBlock = "<script type=\"application/json\" id=\"lang-data\" data-lang=\"" + language + "\">" + PortalLanguageJson_(language) + "</script>";
      page.Replace("<!--hm-language-->", languageBlock.c_str());
      // And the direction of the first paint, which the script alone cannot give:
      // the page ships <html lang="en" dir="ltr"> and Portal.js settles both from
      // the catalogue it is handed, but it runs at the end of the body - so a reader
      // of Arabic, Hebrew or Persian saw one left-to-right frame before the page
      // turned round. The language is already known here, so the tag leaves the
      // server right. The script writes the same two attributes again when it runs,
      // and on every later change of language, which is where a choice made in the
      // page is answered.
      if (!language.IsEmpty())
      {
         const bool rightToLeft = language == "ar" || language == "he" || language == "fa";
         const AnsiString tag = "<html lang=\"" + language + "\" dir=\"" +
            AnsiString(rightToLeft ? "rtl" : "ltr") + "\">";
         page.Replace("<html lang=\"en\" dir=\"ltr\">", tag.c_str());
      }
      response.body = page;
      pageCache_.Fill(response);
      response.extra_headers = PortalHeaders;
      return response;
   }

   HttpResponse
   RestApiServer::HandlePortalScript_()
   {
      HttpResponse response;
      response.content_type = "text/javascript; charset=utf-8";
      portalScript_.Fill(response, PortalScriptPieces);
      response.extra_headers = PortalHeaders;
      return response;
   }

   HttpResponse
   RestApiServer::HandlePortalManifest_()
   {
      HttpResponse response;
      response.content_type = "application/manifest+json";
      portalManifest_.Fill(response, PortalManifestPieces);
      response.extra_headers = PortalHeaders;
      return response;
   }

   // Service-Worker-Allowed lets a worker served from /portal-sw.js take the
   // whole origin as its scope, which the page under /portal needs.
   HttpResponse
   RestApiServer::HandlePortalServiceWorker_()
   {
      HttpResponse response;
      response.content_type = "text/javascript; charset=utf-8";
      portalServiceWorker_.Fill(response, PortalServiceWorkerPieces);
      response.extra_headers = AnsiString(PortalHeaders) + "Service-Worker-Allowed: /\r\n";
      return response;
   }

   HttpResponse
   RestApiServer::HandleOpenApi_()
   {
      // The description lives here, beside the router it describes, so a route
      // change and its documentation change land in the same diff - a separate
      // file would drift the way every hand-maintained count in this project
      // has. Kept to OpenAPI 3.0 syntax and deliberately terse: the reference
      // for behaviour is the server, and this is the map, not the territory.
      static const char *openApiHead =
         "{"
         "\"openapi\":\"3.0.3\","
         "\"info\":{\"title\":\"hMailServer REST API\",\"version\":\"1\","
         "\"description\":\"Administration API. Authenticate with the administrator password (HTTP Basic, user 'Administrator') or an API key (Bearer). API keys can be read-only or restricted to named domains; key management itself requires the administrator password. The /api/v1/me endpoints are the exception: they answer to an account\'s own credentials (HTTP Basic, user = the mailbox address) and to nothing else, and /portal is a sign-in page for them. An account that holds an administrative role also reaches the routes that role permits, with the same credential or a browser session: GET /api/v1/session says what a credential may do, and GET /api/v1/roles lists the roles and what each holds.\"},"
         "\"paths\":{"
         "\"/api/v1/status\":{\"get\":{\"summary\":\"Server status\",\"responses\":{\"200\":{\"description\":\"Version, state, the message counters, the session counts, and build: what this build can do - script_engine, argon2id, post_quantum_key_exchange\"}}}},"
         "\"/api/v1/server/reinitialize\":{\"post\":{\"summary\":\"Restart the services in place\",\"description\":\"What the Control Panel's Reinitialize does: every service is stopped, the configuration reloaded and the services started again in the same process, so that a port, a certificate binding or a setting the document marks as taking effect on restart takes effect now. Answers before it happens, because the REST listener itself restarts: poll GET /api/v1/status until it answers again. Server-wide; refused for domain-restricted and read-only keys.\",\"responses\":{\"202\":{\"description\":\"Reinitialising; the listeners restart in a moment\"}}}},"
         "\"/api/v1/me\":{\"get\":{\"summary\":\"The signed-in account's own state\",\"description\":\"HTTP Basic with the account's address and password - the same credential and the same checks as an IMAP logon, including a per-name lockout and the auto-ban. Refused for the administrator password and for API keys.\",\"responses\":{\"200\":{\"description\":\"address, domain, active, quota (limit_mb, used_bytes), vacation (enabled, active, subject, message, expires, expires_date), password_changed, second_factor, directory_linked\"},\"401\":{\"description\":\"Not an account's credentials\"},\"403\":{\"description\":\"The administrator password or an API key was presented\"}}}},"
         "\"/api/v1/me/password\":{\"post\":{\"summary\":\"Change the signed-in account's password\",\"description\":\"Body: current and new. current has to be the account password itself, not an app password. An account with a second factor sends the code in X-hMailServer-OTP; without it the answer is 401 with X-hMailServer-OTP: required. The password policy, the reuse history and the breach check apply exactly as when an administrator sets a password; a breach-check refusal carries reason (breached or breach_check_unavailable) beside error, for a page to translate.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"current\",\"new\"],\"properties\":{\"current\":{\"type\":\"string\"},\"new\":{\"type\":\"string\"}}}}}},\"responses\":{\"200\":{\"description\":\"Changed\"},\"400\":{\"description\":\"Missing fields, the policy refused the new password (the reason is in error), or it was found in the published breach corpus (reason: breached)\"},\"403\":{\"description\":\"The current password did not match\"},\"409\":{\"description\":\"A directory-linked account, or a recently used password\"},\"503\":{\"description\":\"The breach check could not reach its endpoint and breach_check_unreachable_action is refuse (reason: breach_check_unavailable); nothing changed\"}}}},"
         "\"/api/v1/me/vacation\":{\"put\":{\"summary\":\"Set the signed-in account's automatic reply\",\"description\":\"The whole state at once: enabled (required), subject, message, expires and expires_date (YYYY-MM-DD, required when expires is true).\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"enabled\"],\"properties\":{\"enabled\":{\"type\":\"boolean\"},\"subject\":{\"type\":\"string\"},\"message\":{\"type\":\"string\"},\"expires\":{\"type\":\"boolean\"},\"expires_date\":{\"type\":\"string\"}}}}}},\"responses\":{\"200\":{\"description\":\"The state as saved\"},\"400\":{\"description\":\"enabled missing, a field over its length, or a malformed expires_date\"}}}},"
         "\"/api/v1/session\":{\"post\":{\"summary\":\"Start a browser session for the signed-in account, or for the administrator\",\"description\":\"HTTP Basic, once: an account's address and password, or the administrator's name and password - and, when a second factor is enrolled on the administrator credential, the one-time code in X-hMailServer-OTP (without it the answer is 401 with X-hMailServer-OTP: required). Answers 201 with a Set-Cookie (hmailsession; HttpOnly, SameSite=Strict, Secure over TLS). The cookie then authenticates as the credential itself would - an account reaches the /api/v1/me endpoints, the administrator reaches everything but them - without a password, for 30 minutes of idleness and 12 hours at most; a request that changes something must also carry X-Requested-With: hMailServer. A password change ends the account's other sessions, and a change of the administrator password ends every administrator session. An API key cannot start one. The bearer form, ?bearer=1, is for an account only: the session is the same in every respect, but the token is answered in the body as token and no cookie is set, and it is then accepted as Authorization: Bearer <token> and in no Cookie header (a cookie's value is not accepted as a bearer either). It is what the webmail asks for when a person adds a second account beside the one whose cookie the browser already holds - a cookie is one per origin. The page holds the token in memory only, so a reload forgets it; it ends after the same idle time and lifetime as any session of that account, a password change ends it, its rows in GET /api/v1/me/sessions say bearer true, and DELETE /api/v1/session with the bearer ends it without touching the cookie. What the form means: a token a page holds in script is a credential a cross-site script in that page could read, which the cookie is not; the webmail is served under a policy that allows no inline script and no eval, which is why the form is acceptable there and why it is never minted for the administrator (403). For the same reason a bearer session is the mailbox and nothing more: an account that holds an administrative role - an admin level, or a role assignment - reaches the administration API on its password and on its cookie, and never on a bearer, which is refused there (403) as an ordinary account is and answers administrative false to GET /api/v1/session. And the account the request's own cookie stands for is refused the form (409): it is the account the browser is already signed in as, and signing it in again from the same device would end the cookie's session under the one-per-device policy.\",\"responses\":{\"201\":{\"description\":\"address (an account) or administrator true, with idle_seconds and lifetime_seconds; the cookie in Set-Cookie - or, with ?bearer=1, bearer true and the token in the body and no Set-Cookie\"},\"401\":{\"description\":\"Not an account's or the administrator's credentials\"},\"403\":{\"description\":\"A session cookie or an API key was presented, or the bearer form was asked for with the administrator password\"},\"409\":{\"description\":\"The bearer form was asked for with the password of the account the request's cookie already stands for\"}}},\"delete\":{\"summary\":\"End the browser session the request came with\",\"responses\":{\"200\":{\"description\":\"Ended; the cookie is cleared - or, for a session presented as a bearer, ended and bearer true, with no Set-Cookie\"},\"400\":{\"description\":\"The request carried a password, not a session\"}}},\"get\":{\"summary\":\"What this credential is, and what it may do\",\"description\":\"What the credential this request came in on is, and what it may do: identity, credential (administrator, apikey or account), administrative, role, permissions and domains. Answered to every credential that authenticated, an API key included: a caller must be able to learn what it may do without trying things and reading the 403s, which is what lets the Control Deck leave a view out rather than show one whose every button is refused.\",\"responses\":{\"200\":{\"description\":\"identity, credential, administrative, role, permissions, domains\"}}}},"
         "\"/api/v1/me/contacts\":{\"get\":{\"summary\":\"The signed-in account's address book\",\"description\":\"q= narrows to names and addresses containing the text, case-insensitively, which is what the To field's completion asks; limit= caps the answer (200 by default); source=manual is what the person kept and source=collected what they merely wrote to; kind=person or kind=group answers one kind, and without it both are listed; fields=card puts the stored vCard on each contact. Each contact carries kind (person), birthday, note, phones and has_photo, read from the card a phone stored. A group (see /api/v1/me/contacts/groups) is listed with kind group, an empty address, its members and its recipients, so that the completion can offer it by name and expand it. Recipients of what the account sends through the API are added on their own, marked collected.\",\"responses\":{\"200\":{\"description\":\"contacts (id, name, address, source manual|collected, created, kind), count, total\"}}},\"post\":{\"summary\":\"Add a contact\",\"description\":\"Body: address (one e-mail address, or Name <address>), name (optional). One row per address per account.\",\"responses\":{\"201\":{\"description\":\"The contact\"},\"400\":{\"description\":\"Not one address\"},\"409\":{\"description\":\"That address is already a contact; id says which\"}}}},"
         "\"/api/v1/me/contacts/{id}/photo\":{\"get\":{\"summary\":\"The photo on the contact's card\",\"description\":\"The image a phone stored on the card, as it is; 404 for a contact with no card, no photo, or a photo that is only a URL.\",\"responses\":{\"200\":{\"description\":\"The image\"},\"404\":{\"description\":\"No photo\"}}}},"
         "\"/api/v1/me/contacts/{id}\":{\"put\":{\"summary\":\"Change a contact's name or address, or keep a collected address as a contact\",\"description\":\"Body: name and/or address, and/or source, which can only be set to manual - what the person wrote to becomes a contact by their choice, and is served to their phones from then on. Another account's contact is 404.\",\"responses\":{\"200\":{\"description\":\"The contact\"},\"404\":{\"description\":\"No such contact\"},\"409\":{\"description\":\"The new address is already a contact\"}}},\"delete\":{\"summary\":\"Remove a contact\",\"responses\":{\"200\":{\"description\":\"Removed\"},\"404\":{\"description\":\"No such contact\"}}}},"
         "\"/api/v1/me/identities\":{\"get\":{\"summary\":\"The addresses the signed-in account may write as\",\"description\":\"The account's own address (kind account), every active alias that resolves to it (alias), and every account whose INBOX grants this one the post right (granted) - the rule SMTP submission applies to MAIL FROM. Each carries the From header the page would send. A send or a draft names one in its from field; anything else is refused with 403.\",\"responses\":{\"200\":{\"description\":\"identities (address, name, kind, header)\"}}}},"
         "\"/api/v1/me/preferences\":{\"get\":{\"summary\":\"The signed-in account's preferences\",\"description\":\"A key/value store the webmail keeps its choices in (theme, density, undo-send delay, notifications) so they follow the account between browsers. The server attaches no meaning to a key.\",\"responses\":{\"200\":{\"description\":\"preferences: an object of string values\"}}},\"put\":{\"summary\":\"Change preferences\",\"description\":\"Body: an object. Each string member is written, each null member removed, anything else refused. Keys are 1-64 letters, digits, dots, dashes or underscores; values at most 4000 characters; at most 100 keys per account.\",\"responses\":{\"200\":{\"description\":\"The preferences after the change\"},\"400\":{\"description\":\"Not an object of strings, a bad key, a long value, or too many keys\"}}}},"
         "\"/api/v1/me/push\":{\"get\":{\"summary\":\"The signed-in account's Web Push devices\",\"description\":\"enabled: whether the server has Web Push switched on at all. vapid_public_key: the application server key a browser needs for pushManager.subscribe; empty when the administrator has not generated a key pair, and a page must say so rather than subscribing. max_devices: how many devices this account may have at once. subscriptions: one object per device with id, device (the name the person gave it), service (the origin of the push service, so a row can be recognised), created, last_sent, state (active or dead-lettered), failures, next_attempt, last_detail (why the last attempt failed), and the person's own choices - detail (0 a count only, 1 and the sender, 2 and the subject), senders, labels, quiet_from, quiet_to and quiet_offset. The endpoint and the two keys are NEVER in this answer: together they are the capability to send a notification to that device, and echoing them would put it in every log and cache this route touches.\",\"responses\":{\"200\":{\"description\":\"enabled, vapid_public_key, max_devices, subscriptions\"}}}},"
         "\"/api/v1/me/push/subscriptions\":{\"post\":{\"summary\":\"Tell this account's device about new mail\",\"description\":\"Body: endpoint (the https address pushManager.subscribe returned), keys: {p256dh, auth} as the browser gave them (also accepted at the top level), device (a name for this browser or phone, at most 64 characters), and optionally the choices - detail (0 a count only, 1 and the sender, 2 and the subject; 1 by default), senders (a comma-separated list of addresses and @domain suffixes, empty for anyone), labels (a comma-separated list of IMAP keywords the message must carry, empty for any), quiet_from and quiet_to (minutes past midnight in the device's own time, -1 for no quiet hours) and quiet_offset (the minutes that device's clock is ahead of UTC). A device subscribing again under the same endpoint replaces its own row. Refused with 409 when the server has Web Push off or no key pair, and with 400 when the account already has max_devices devices or a key is not the length RFC 8291 needs.\",\"responses\":{\"201\":{\"description\":\"The device, as the listing gives it\"},\"400\":{\"description\":\"A key of the wrong shape, a bad choice, or too many devices\"},\"409\":{\"description\":\"Web Push is off on this server, or it has no key pair\"},\"413\":{\"description\":\"The body is too large\"}}}},"
         "\"/api/v1/me/push/subscriptions/{id}\":{\"put\":{\"summary\":\"Change what one device is told\",\"description\":\"Body: any of detail, senders, labels, quiet_from, quiet_to, quiet_offset. Each member left out keeps the value it had. The endpoint and the keys cannot be changed here: a device that has a new endpoint subscribes again.\",\"responses\":{\"200\":{\"description\":\"The device after the change\"},\"400\":{\"description\":\"A choice outside its range\"},\"404\":{\"description\":\"No such device of this account\"}}},\"delete\":{\"summary\":\"Stop telling one device\",\"description\":\"The row is removed. The browser's own subscription is the browser's to remove; this server simply stops sending to it.\",\"responses\":{\"200\":{\"description\":\"Removed\"},\"404\":{\"description\":\"No such device of this account\"}}}},"
         "\"/api/v1/me/push/test\":{\"post\":{\"summary\":\"Send one push to a device now\",\"description\":\"Body: id, the device. Sends a notification at once, marked test, so a person can find out whether their device is reachable before they rely on it. Neither the quiet hours nor the sender list applies - this is a button being pressed, not mail arriving. A push service that answers 404 or 410 means the subscription is gone, and the device is removed. 502 carries the push service's own refusal.\",\"responses\":{\"200\":{\"description\":\"sent\"},\"400\":{\"description\":\"No device named\"},\"404\":{\"description\":\"No such device of this account\"},\"502\":{\"description\":\"The push service refused it, or the subscription is gone\"}}}},"
         "\"/api/v1/me/app-passwords\":{\"get\":{\"summary\":\"The signed-in account's app passwords\",\"description\":\"id, name, created, last_used, active - never the password.\",\"responses\":{\"200\":{\"description\":\"app_passwords\"}}},\"post\":{\"summary\":\"Make an app password\",\"description\":\"Body: name (what it is for) and password - the account's own password, proving it is the account holder asking; never an app password. An account with a second factor enrolled sends the code in X-hMailServer-OTP, and the 401 that asks for it says X-hMailServer-OTP: required. Refused (403) to a request that was itself authenticated with an app password, or by a session started with one. The answer carries the password in clear text, the only time it exists outside the caller; at most 20 per account. Logged, with the caller's address.\",\"responses\":{\"201\":{\"description\":\"id, name, created, active, password\"},\"400\":{\"description\":\"No name, no password, or twenty already\"},\"401\":{\"description\":\"The one-time code is missing or wrong\"},\"403\":{\"description\":\"The password is not the account's own, or the request came in on an app password\"}}}},"
         "\"/api/v1/me/app-passwords/{id}\":{\"delete\":{\"summary\":\"Remove an app password\",\"description\":\"Refused to a request authenticated with an app password. Logged, with the caller's address.\",\"responses\":{\"200\":{\"description\":\"Removed\"},\"403\":{\"description\":\"The request came in on an app password\"},\"404\":{\"description\":\"No such app password of this account\"}}}},"
         "\"/api/v1/me/smime\":{\"get\":{\"summary\":\"The account's S/MIME key store\",\"description\":\"own: the account's certificates, each with its chain and its private key as the page wrapped it (AES-256-GCM under a PBKDF2 key from the account password; this server cannot open it); recipients: the certificates of correspondents. Each carries address, name, fingerprint (SHA-256, hex), certificate (base64 DER), not_after and created (Unix seconds). limits: how many of each.\",\"responses\":{\"200\":{\"description\":\"own, recipients, limits\"}}}},"
         "\"/api/v1/me/smime/own\":{\"put\":{\"summary\":\"Add or replace one of the account's own certificates\",\"description\":\"Body: address, name, fingerprint, certificate, chain (array of base64 DER, at most eight), key {v, kdf, iterations, salt, iv, data} as the page wraps a private key (v is 1 when absent and is kept; iterations at least 600,000), not_after. Replaced by fingerprint. At most twenty.\",\"responses\":{\"201\":{\"description\":\"added\"},\"200\":{\"description\":\"replaced\"},\"400\":{\"description\":\"a field is missing or malformed, or the limit is reached\"}}}},"
         "\"/api/v1/me/smime/own/{fingerprint}\":{\"delete\":{\"summary\":\"Remove one of the account's own certificates and its key\",\"responses\":{\"200\":{\"description\":\"removed\"},\"404\":{\"description\":\"no such certificate\"}}}},"
         "\"/api/v1/me/smime/recipients\":{\"put\":{\"summary\":\"Add or replace a correspondent's certificate\",\"description\":\"Body: address, name, fingerprint, certificate, not_after. Replaced by fingerprint. At most five hundred.\",\"responses\":{\"201\":{\"description\":\"added\"},\"200\":{\"description\":\"replaced\"},\"400\":{\"description\":\"a field is missing or malformed, or the limit is reached\"}}}},"
         "\"/api/v1/me/smime/recipients/{fingerprint}\":{\"delete\":{\"summary\":\"Remove a correspondent's certificate\",\"responses\":{\"200\":{\"description\":\"removed\"},\"404\":{\"description\":\"no such certificate\"}}}},"
         "\"/api/v1/me/smime/chain\":{\"post\":{\"summary\":\"Does a certificate chain end at a root this machine trusts\",\"description\":\"Body: certificates (the leaf first, then its chain; base64 DER each, at most nine), purpose (sign or encrypt). OpenSSL's verdict against the system's roots - the Windows ROOT and CA stores, the distribution's bundle on Linux: trusted, error (OpenSSL's reason when not), depth, roots (how many were consulted; -1 when the platform's bundle was taken), subject, issuer, not_after.\",\"responses\":{\"200\":{\"description\":\"the verdict\"},\"400\":{\"description\":\"not a list of base64 certificates\"}}}},"
         "\"/portal-smime.js\":{\"get\":{\"summary\":\"The webmail's S/MIME module\",\"description\":\"Unauthenticated. DER, X.509, PKCS#8 and PKCS#12, CMS SignedData and EnvelopedData and the MIME around them, on the Web Crypto API; what build/check-portal-smime.js runs against OpenSSL.\",\"responses\":{\"200\":{\"description\":\"The script\"}}}},"
         "\"/api/v1/me/sessions\":{\"get\":{\"summary\":\"The signed-in account's browser sessions\",\"description\":\"Each: id (twelve hex digits of the token's hash), created_seconds_ago, idle_seconds, current, support, and bearer - true for a session a webmail page holds as its second account, beside another account's cookie.\",\"responses\":{\"200\":{\"description\":\"sessions\"}}},\"delete\":{\"summary\":\"End every other session\",\"description\":\"The session asking stays; a caller on a password ends them all.\",\"responses\":{\"200\":{\"description\":\"ended (how many)\"}}}},"
         "\"/api/v1/me/sessions/{id}\":{\"delete\":{\"summary\":\"End one session\",\"responses\":{\"200\":{\"description\":\"Ended\"},\"404\":{\"description\":\"No such session of this account\"}}}},"
         "\"/api/v1/me/devices\":{\"get\":{\"summary\":\"The clients that have signed in to the account\",\"description\":\"One entry per client, most recently used first: id (thirty-two hex characters, what a revoke names), protocol (smtp, pop3, imap, web, dav or managesieve), address, agent (the user agent where the protocol carries one, else empty), credential (password, app_password, token, certificate or single_sign_on), credential_name (the app password's name, where one was used), first_seen, last_seen, sign_ins, revoked, revoked_at and current - true for the client this request came in on, and for none when the request carried a password rather than a session. At most fifty are kept per account; the least recently seen goes when a fifty-first appears.\",\"responses\":{\"200\":{\"description\":\"devices\"}}}},"
         "\"/api/v1/me/devices/{id}\":{\"delete\":{\"summary\":\"Revoke a client: end what it holds now\",\"description\":\"Its browser session is ended (the token is invalidated, so its next request is refused) and its live IMAP, POP3 and SMTP connections are dropped. credential=revoke in the query also deactivates the app password it signed in with, where it used one - which is what stops a stolen client simply signing in again; forget=true removes the entry rather than marking it revoked. A revoke is NOT a ban: the entry stays, marked, and a later sign-in from that client with a credential that still works clears the mark. Refused (403) to a request authenticated with an app password, or by a session started with one. Answers sessions_ended, connections_dropped and credential_disabled.\",\"responses\":{\"200\":{\"description\":\"revoked, forgotten, sessions_ended, connections_dropped, credential_disabled\"},\"400\":{\"description\":\"The id is not thirty-two hex characters\"},\"403\":{\"description\":\"The request came in on an app password\"},\"404\":{\"description\":\"No such device of this account\"}}}},"
         "\"/api/v1/me/storage\":{\"get\":{\"summary\":\"What the mailbox holds\",\"description\":\"used_bytes and limit_mb (0 = no limit); folders, each with id, path, messages and bytes; largest, the twenty biggest messages with id, folder_id, folder, subject, from, date and size.\",\"responses\":{\"200\":{\"description\":\"The storage view\"}}}},"
         "\"/api/v1/me/quarantine\":{\"get\":{\"summary\":\"The messages held as suspected spam for the signed-in account\",\"description\":\"Only the entries this address is a recipient of, without the other recipients. enabled says whether the server holds spam at all.\",\"responses\":{\"200\":{\"description\":\"enabled, messages (id, sender, subject, reason, score, size, created)\"}}}},"
         "\"/api/v1/me/quarantine/{id}/release\":{\"post\":{\"summary\":\"Deliver a held message to the signed-in account\",\"description\":\"Delivered to this address only; the entry stays for its other recipients and goes when this was the last. A message this address was not sent is 404.\",\"responses\":{\"200\":{\"description\":\"Released\"},\"404\":{\"description\":\"Not held for this account\"}}}},"
         "\"/api/v1/me/quarantine/{id}\":{\"delete\":{\"summary\":\"Give up the signed-in account's copy of a held message\",\"description\":\"This address leaves the entry; the entry and its file go when no recipient is left. Nothing is delivered.\",\"responses\":{\"200\":{\"description\":\"Deleted\"},\"404\":{\"description\":\"Not held for this account\"}}}},"
         "\"/api/v1/me/folders/{id}/messages\":{\"get\":{\"summary\":\"One folder's messages, newest first\",\"description\":\"Query parameters: limit (1-200, default 200), before_uid (only messages with a lower UID - the way to page back) and q (only messages containing the text, case-insensitively, in Subject, From, To, Cc, the text or the HTML; at most 2000 are looked at per request - scanned says how many, complete whether that was all, and next_before_uid where to continue). Each entry: id, uid, size, received, subject, from, date (decoded from the head of the file, as FETCH ENVELOPE would), flags (seen, flagged, answered, draft, deleted), message_id, in_reply_to, references, to, has_attachments, snippet (the first 160 characters of the text, quotations and the signature left out) and category - primary, social, promotions, updates or forums, decided from the header alone: the sender's domain, List-Post, List-Unsubscribe, Precedence and Auto-Submitted. total is the folder's count. A folder of another account, or one the ACL keeps from this one, is 404.\",\"responses\":{\"200\":{\"description\":\"folder_id, total, messages\"},\"404\":{\"description\":\"Not this account's folder\"}}}},"
         "\"/api/v1/me/drafts\":{\"post\":{\"summary\":\"Keep a draft in the Drafts folder\",\"description\":\"Body: to, cc, bcc, subject, text, html (its data: images kept as inline parts, as on a send), from (as on a send), and optionally replace_id - the draft this one supersedes, expunged once the new one is saved (new content is a new message with a new UID, as IMAP requires). The Drafts folder is made as Drafts when the account has none. The draft carries the \\\\Draft and \\\\Seen flags and is read, moved and deleted through the message routes.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"properties\":{\"to\":{\"type\":\"string\"},\"cc\":{\"type\":\"string\"},\"bcc\":{\"type\":\"string\"},\"subject\":{\"type\":\"string\"},\"text\":{\"type\":\"string\"},\"replace_id\":{\"type\":\"integer\"}}}}}},\"responses\":{\"201\":{\"description\":\"id, folder_id\"},\"403\":{\"description\":\"The Drafts folder does not allow it\"},\"413\":{\"description\":\"The mailbox is full\"}}}},"
         "\"/api/v1/me/settings\":{\"get\":{\"summary\":\"The signed-in account's own settings\",\"responses\":{\"200\":{\"description\":\"name (first, last), forwarding (enabled, address, keep_original), signature (enabled, text, html)\"}}},\"put\":{\"summary\":\"Change the signed-in account's own settings\",\"description\":\"Each of name, forwarding and signature the body names is applied whole; one it does not name is left as it is. A forwarding that is enabled needs an e-mail address, and not the account's own. signature.text is at most 20,000 characters; signature.html, the formatted signature - where the webmail keeps an image as a data: URL - is at most 64,000 bytes of UTF-8, images included, which is what the narrowest backend's column (MySQL's text) holds. A request to this route may be as large as a message's, so the limit is answered here as a sentence rather than by the listener.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"object\"},\"forwarding\":{\"type\":\"object\"},\"signature\":{\"type\":\"object\"}}}}}},\"responses\":{\"200\":{\"description\":\"The settings as saved\"},\"400\":{\"description\":\"Nothing named, a name or signature too long, or a forwarding address refused\"}}}},"
         "\"/api/v1/me/filters\":{\"get\":{\"summary\":\"The signed-in account's active Sieve script\",\"responses\":{\"200\":{\"description\":\"active (the script, empty when none), name (the active script's name when ManageSieve set one)\"}}},\"put\":{\"summary\":\"Set the signed-in account's active Sieve script\",\"description\":\"Body: script. Checked as ManageSieve's PUTSCRIPT checks it, with the same wording in error; an empty script removes the filter. The script runs on every message that arrives from then on.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"script\"],\"properties\":{\"script\":{\"type\":\"string\"}}}}}},\"responses\":{\"200\":{\"description\":\"active\"},\"400\":{\"description\":\"script missing, over 256 KB, or not parsing (the reason is in error)\"}}}},"
         "\"/api/v1/me/search\":{\"get\":{\"x-operators\":\"q takes words (every one must be found), quoted phrases, -term (a word, phrase or operator the message must not match), a OR b (one of the two must match, and so on through a OR b OR c; OR in capitals), and from:, to:, cc:, bcc: (address or name in that header; Bcc only where the sender's own copy kept it), subject:, has:attachment, has:link (an http or https address in the text, or a link to one in the HTML; the message is read for it), filename: (an attachment's file name contains), larger:10M and smaller: (bytes, or K, M, G for multiples of 1024), before:YYYY-MM-DD, after:YYYY-MM-DD, older_than:7d and newer_than: (d, w, m or y, against the time the message was stored, as before: and after: are), in:folder, is:unread, is:read, is:flagged, is:unflagged, is:answered, is:muted, is:pinned (the $Muted and $Pinned keywords), label:name (an IMAP keyword the message carries; several must all be there), category: (primary, social, promotions, updates or forums - the tab the listing gives the message), in_reply_to:<message-id> (the id, with or without its angle brackets, is in In-Reply-To or References: the replies to that message, which is what the webmail asks for the Sent folder's nudges); a folder listing's q takes the same, without in:.\",\"summary\":\"Search every folder of the signed-in account\",\"description\":\"Query parameters: q (required) and limit (1-200, default 50). The same match as q on a folder listing, over every folder the account may read, newest first; at most 2000 messages are looked at per request (scanned, complete), and more says whether hits beyond limit were cut. Each hit names its folder_id and folder path.\",\"responses\":{\"200\":{\"description\":\"query, scanned, complete, more, messages\"},\"400\":{\"description\":\"q missing\"}}}},"
         "\"/api/v1/me/messages\":{\"post\":{\"x-body\":\"text and, when given, html (the message goes as multipart/alternative); attachments; from (one of the identities); receipt; in_reply_to, references, answered_id; expires_in and passcode for a message that expires; or mime - the MIME entity the page built (a signed or an encrypted message: its Content-Type headers, a blank line, its body, 7-bit), sent under this server's headers in place of text, html and attachments.\",\"summary\":\"Send a message as the signed-in account\",\"description\":\"The domain of the address the message is sent from adds its signature and its disclaimer, by the rules SMTP submission applies; the account's own signature is not added, because it is the caller's to write into the text, as the webmail does. Body: to, cc, bcc (address lists, comma or semicolon separated, display names allowed), subject, text, and from - one of the account's identities (GET /api/v1/me/identities: its own address, an alias of it, or an address whose owner granted it the post right), as address or Name <address>; optionally in_reply_to and references (written as the headers of those names, so the recipient's client threads the reply) and answered_id (the id of the message this answers, which gets \\\\Answered). Every address is put through the checks RCPT TO makes for an authenticated sender, and a refused one is named in error. The message is queued through the same delivery pipeline as SMTP submission, and a copy marked read is kept in the folder designated \\\\Sent when the account has one and its quota allows. attachments is an array of {name, type, data} with data as base64 - at most 20, twelve megabytes together; this route and the drafts route take a request of up to sixteen megabytes. An image html holds as a data: URL - PNG, JPEG, GIF or WebP, the whole value of a src attribute - is sent as an inline part beside the HTML in a multipart/related part, and the HTML names it with a cid: URL, since Gmail and Outlook do not display a data: image; the copy in Sent is the same message, and GET /api/v1/me/messages/{id}/html puts the image back in place for the page. expires_in (seconds from now, 1 to 7776000) makes a message that expires, Gmail's confidential mode: the message is kept on this server under the data directory and each recipient is sent a short notice with a link to /confidential/{token} under confidential_base_url, where they read it after proving they are a recipient; passcode (4 to 64 characters) adds a word the sender gives them another way. The copy in Sent is the full message; the answer adds confidential_id and expires; GET /api/v1/me/confidential lists them and POST .../{id}/revoke withdraws one. Refused with 403 and code confidential_unavailable when no base URL is set, and with 400 beside mime; an expires_in that is not a whole number of seconds (negative, a fraction, a string, true, null) is refused with 400 rather than read as no expiry, and so is passcode without expires_in. The kept copy counts against the mailbox's quota with everything else the account keeps (413, code quota_exceeded), and an account has at most 1000 outstanding (400, code confidential_ceiling).\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"properties\":{\"to\":{\"type\":\"string\"},\"cc\":{\"type\":\"string\"},\"bcc\":{\"type\":\"string\"},\"subject\":{\"type\":\"string\"},\"text\":{\"type\":\"string\"}}}}}},\"responses\":{\"201\":{\"description\":\"queued, recipients, sent_id (0 when no copy was kept), and for a message that expires confidential_id and expires\"},\"400\":{\"description\":\"No recipient, or an address refused (named in error). A recipient on the sending domain's suppression list also answers code recipient_suppressed, address, and source - bounce or report for an address that bounced, manual for one an administrator listed.\"},\"403\":{\"description\":\"Refused by a rule of the server's or of a domain's with a reject action, as the SMTP session would refuse it at DATA: error is the rule's text (or a sentence naming the rule) and code is policy_rejected; nothing is queued or kept. Also a message that expires where no base URL is set (code confidential_unavailable).\"},\"413\":{\"description\":\"Larger than the server allows\"}}}},"
         "\"/api/v1/me/messages/{id}\":{\"get\":{\"summary\":\"One message, read\",\"description\":\"The listing's fields plus folder_id, to, cc, text, html and attachments (index, name, size, content_type, content_id). content_type is the media type the part declares, lower-cased and without its parameters, and is the empty string when the part declares none; content_id is the part's Content-ID with the angle brackets stripped - the form a cid: URL in html uses - and is the empty string when the part carries none. An inline image is an attachment here like any other part, so a page renders one by matching a cid: URL in html against content_id and pointing at the attachment route. A message over one megabyte is described with truncated true and no body. Another account's message, or one in a folder the ACL keeps from this account, is 404.\",\"responses\":{\"200\":{\"description\":\"The message\"},\"404\":{\"description\":\"Not this account's message\"}}},\"delete\":{\"summary\":\"Delete one message\",\"description\":\"Moved to the folder designated \\\\Trash when the account has one and the message is not in it already; expunged instead when the account has no Trash folder, when the message is already in it, or when the caller adds ?permanent=1. The rights EXPUNGE asks for.\",\"responses\":{\"200\":{\"description\":\"deleted true, or deleted false with moved_to and the new id\"},\"403\":{\"description\":\"The folder does not allow it\"},\"404\":{\"description\":\"Not this account's message\"}}}},"
         "\"/api/v1/me/messages/{id}/flags\":{\"put\":{\"summary\":\"Change one message's flags\",\"description\":\"Body: any of seen, flagged, answered, draft, deleted as booleans; only the flags named change. The rights STORE asks for - seen, deleted and the rest are three permissions. Every IMAP session on the folder is told.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"properties\":{\"seen\":{\"type\":\"boolean\"},\"flagged\":{\"type\":\"boolean\"},\"answered\":{\"type\":\"boolean\"},\"draft\":{\"type\":\"boolean\"},\"deleted\":{\"type\":\"boolean\"}}}}}},\"responses\":{\"200\":{\"description\":\"id, folder_id, flags\"},\"400\":{\"description\":\"No flag named\"},\"403\":{\"description\":\"The folder does not allow it\"},\"404\":{\"description\":\"Not this account's message\"}}}},"
         "\"/api/v1/me/messages/{id}/html\":{\"get\":{\"summary\":\"The message's HTML part as a document for a frame\",\"description\":\"text/html under a policy of its own: nothing runs, no form is submitted, no address is rewritten, and remote images and styles are blocked unless ?remote=1 is given - which the page does when the reader allowed this message or this sender. Images the message embeds (cid:) are inlined as data: URLs under the inline budget. The message JSON's html_remote says whether the part names anything remote at all. 404 when the message has no HTML part.\",\"responses\":{\"200\":{\"description\":\"The document\"},\"404\":{\"description\":\"No such message, or no HTML part\"}}}},"
         "\"/api/v1/me/files\":{\"get\":{\"summary\":\"The signed-in account's files sent as links\",\"description\":\"policy (link_above_kb: the size above which the webmail sends a file as a link; days: how long a link lives by default; max_days; max_mb: the most one file may be; quota_mb: the most the account may keep), used_bytes, count, and files - each with id, token, name, type, size, stored, complete, created and expires (Unix seconds), expired, downloads, protected, link (/files/{token}).\",\"responses\":{\"200\":{\"description\":\"policy, used_bytes, count, files\"}}},\"post\":{\"summary\":\"Record a file to be sent as a link\",\"description\":\"Body: name, type, size (bytes), and days (0 to max_days; the domain's default when absent; 0 makes a link that is dead at once) and password when wanted. The bytes follow with PUT .../{id}/content. At most 200 files an account; a file at most max_mb; the account's files together at most quota_mb.\",\"responses\":{\"201\":{\"description\":\"The record, as the list shows it\"},\"400\":{\"description\":\"A value refused\"},\"413\":{\"description\":\"Too large for the domain's policy, or the account's allowance is used up\"}}}},"
         "\"/api/v1/me/files/{id}\":{\"put\":{\"summary\":\"Re-time or protect a file\",\"description\":\"Body: days (from now; 0 ends the link) and/or password (a string; empty removes it).\",\"responses\":{\"200\":{\"description\":\"The record\"},\"404\":{\"description\":\"Not this account's file\"}}},\"delete\":{\"summary\":\"Remove a file and its link\",\"responses\":{\"200\":{\"description\":\"Removed\"},\"404\":{\"description\":\"Not this account's file\"}}}},"
         "\"/api/v1/me/files/{id}/content\":{\"put\":{\"summary\":\"Append a chunk of the file's bytes\",\"description\":\"The body is the chunk, raw (up to the large request cap); offset= is where it starts and must be what the record has stored so far, so a chunk lost is sent again and a chunk sent twice is refused (409, with stored). The record is complete when the declared size is reached.\",\"responses\":{\"200\":{\"description\":\"id, stored, size, complete, link\"},\"409\":{\"description\":\"The offset is not where the file has got to, or the file is complete\"},\"413\":{\"description\":\"More than the size that was declared\"}}}},"
         "\"/files/{token}\":{\"get\":{\"summary\":\"Fetch a file sent as a link (no credentials)\",\"description\":\"The bytes as a download (Content-Disposition attachment, nosniff, a sandbox policy, no-store), under the declared type unless a browser would render or run it. A file with a password answers a form instead, posted back here as password=; ten wrong answers pause the file for fifteen minutes. An expired link answers 410, an unknown or unfinished one 404.\",\"responses\":{\"200\":{\"description\":\"The bytes, or the password form\"},\"410\":{\"description\":\"Expired\"},\"404\":{\"description\":\"No such file\"}}}},"
         "\"/api/v1/portal/files\":{\"get\":{\"summary\":\"The policy for files sent as links (administrator)\",\"description\":\"link_above_kb, days, max_days, max_mb, quota_mb - the domain's own when domain= names one that has some, else the server's.\",\"responses\":{\"200\":{\"description\":\"The policy\"}}},\"put\":{\"summary\":\"Set the policy (administrator)\",\"description\":\"Body: link_above_kb (0 to 1048576), days (1 to 90), max_mb (1 to 500), quota_mb (1 to 102400) - each written when given a number, removed when given null, left alone when absent; domain, when given, sets the domain's own instead of the server's.\",\"responses\":{\"200\":{\"description\":\"The policy as it now stands\"},\"400\":{\"description\":\"A value refused\"}}}},"
         "\"/portal-lang/{code}.json\":{\"get\":{\"summary\":\"The webmail's texts in one language\",\"description\":\"Unauthenticated. An object, English to translation, for one of the languages the page speaks (the page's Language preference lists them); cached a day. The page itself carries the catalogue for the request's Accept-Language.\",\"responses\":{\"200\":{\"description\":\"The catalogue\"},\"404\":{\"description\":\"No such language\"}}}},"
         "\"/deck-lang/{code}.json\":{\"get\":{\"summary\":\"The Control Deck's texts in one language\",\"description\":\"Unauthenticated, as the page at / is. An object, English to translation, read from WebAdmin/languages/{code}.json beside the page - the file the packages install - for one of the languages the page's switch offers; the page fetches it for the language the browser prefers or the administrator chose, and shows English for a text the catalogue lacks. Not cached, as the page is not.\",\"responses\":{\"200\":{\"description\":\"The catalogue\"},\"404\":{\"description\":\"No such language, or the catalogues are not installed\"}}}},"
         "\"/api/v1/portal/branding\":{\"get\":{\"summary\":\"What the webmail says it is\",\"description\":\"Unauthenticated, so the sign-in page can ask: name, logo (an inline image), announcement - the domain's own when domain= names one that has some, else the server's.\",\"responses\":{\"200\":{\"description\":\"name, logo, announcement, domain\"}}},\"put\":{\"summary\":\"Set the branding (administrator)\",\"description\":\"Body: name (100), logo (an inline data:image/ under 3,900 characters), announcement (1,000), each written when given and removed when given empty; domain, when given, sets the domain's own instead of the server's.\",\"responses\":{\"200\":{\"description\":\"The branding as it now stands\"},\"400\":{\"description\":\"A value refused\"}}}},"
         "\"/api/v1/accounts/{address}/support-session\":{\"post\":{\"summary\":\"Open a mailbox as its user, for support (administrator)\",\"description\":\"Refused with 403 unless the user has turned on support access under Security. Answers a session cookie for the account; the moment and the administrator are recorded where the user sees them, every request of the session is written to the application log, and the user's session list shows it and can end it.\",\"responses\":{\"201\":{\"description\":\"support, account, idle_seconds, lifetime_seconds, with the cookie\"},\"403\":{\"description\":\"Not allowed by the user\"},\"404\":{\"description\":\"No such account\"}}}},"
         "\"/api/v1/accounts/{address}/push\":{\"get\":{\"summary\":\"The Web Push devices of an account (administrator)\",\"description\":\"count, and subscriptions: the same listing GET /api/v1/me/push gives the account itself - id, device, service, created, last_sent, state, failures, next_attempt, last_detail and the person's own choices. The endpoint and the two keys are never in it: together they are the capability to notify that device. Account.PushSubscriptionCount over COM.\",\"responses\":{\"200\":{\"description\":\"count, subscriptions\"},\"404\":{\"description\":\"No such account\"}}},\"delete\":{\"summary\":\"Stop Web Push on every device of an account (administrator)\",\"description\":\"What a lost phone needs: every subscription of the account is removed, so nothing more is sent to any of its devices, and the person turns notifications on again on the devices they still have. Recorded in the audit trail as push-subscriptions-removed. Account.RemovePushSubscriptions over COM.\",\"responses\":{\"200\":{\"description\":\"removed: how many devices\"},\"404\":{\"description\":\"No such account\"}}}},"
         "\"/api/v1/me/drafts/{id}/schedule\":{\"post\":{\"summary\":\"Send a draft later\",\"description\":\"Body: send_at, YYYY-MM-DD HH:MM in the server's local time, within a year. At that minute the draft is sent as the account would have sent it - its To, Cc and Bcc under the checks a send makes, a copy in the Sent folder - and the draft goes. One schedule per draft; a new one replaces it.\",\"responses\":{\"201\":{\"description\":\"id, message_id, send_at\"},\"400\":{\"description\":\"Not a draft, not a time, in the past or too far\"}}},\"delete\":{\"summary\":\"Do not send it later after all\",\"responses\":{\"200\":{\"description\":\"cancelled\"},\"404\":{\"description\":\"Nothing scheduled for it\"}}}},"
         "\"/api/v1/me/messages/{id}/snooze\":{\"post\":{\"summary\":\"Snooze a message\",\"description\":\"Body: until, YYYY-MM-DD HH:MM. The message waits in a folder named Snoozed, made when the account has none, and comes back to the folder it left - or the inbox - unread, at that minute.\",\"responses\":{\"200\":{\"description\":\"id, message_id (its new id), until, folder_id (Snoozed)\"},\"400\":{\"description\":\"Not a time, in the past, too far, or snoozed already\"}}}},"
         "\"/api/v1/me/scheduled\":{\"get\":{\"summary\":\"What the signed-in account has put off\",\"responses\":{\"200\":{\"description\":\"scheduled: id, action (send | return), message_id, at, folder_id, subject\"}}}},"
         "\"/api/v1/me/scheduled/{id}\":{\"delete\":{\"summary\":\"Cancel one\",\"description\":\"A cancelled send leaves the draft where it is; a cancelled snooze brings the message back now.\",\"responses\":{\"200\":{\"description\":\"cancelled, returned\"},\"404\":{\"description\":\"Nothing by that id\"}}}},"
         "\"/api/v1/scheduled/run\":{\"post\":{\"summary\":\"Run what is due now\",\"description\":\"Administrator. What the minute task would do at its next tick, done now; answers how many rows were acted on.\",\"responses\":{\"200\":{\"description\":\"ran\"}}}},"
         "\"/api/v1/me/export.zip\":{\"get\":{\"summary\":\"Everything the server holds for the signed-in account, as one archive\",\"description\":\"application/zip, as a download: every folder with mail as an mbox under mail/, the address book as contacts.vcf, every calendar object under calendars/, the webmail's preferences, the active Sieve script, the log lines that name the address, an account.xml in the backup's form with the rules, app passwords, fetch accounts, S/MIME keys and the rest, and a manifest.json that counts what is there. Data portability from the person's own page, with no administrator involved. An archive past 500 MB is refused with 413; an administrator writes it to disk with POST /api/v1/accounts/{address}/export instead.\",\"responses\":{\"200\":{\"description\":\"The archive\"},\"413\":{\"description\":\"Past 500 MB\"},\"500\":{\"description\":\"The archive could not be built\"}}}},"
         "\"/api/v1/me/folders/{id}/export\":{\"get\":{\"summary\":\"The folder as mbox\",\"description\":\"application/mbox, as a download: every message after a From_ line, body lines beginning with From quoted. Larger than 200 MB is 413.\",\"responses\":{\"200\":{\"description\":\"The file\"},\"413\":{\"description\":\"Too large for this route\"}}}},"
         "\"/api/v1/me/folders/{id}/messages\":{\"post\":{\"summary\":\"Import a message into the folder\",\"description\":\"The body is the message itself (message/rfc822, a .eml file), at most 25 MB; stored unread, as IMAP APPEND would store it.\",\"responses\":{\"201\":{\"description\":\"id, folder_id\"},\"400\":{\"description\":\"Not a message\"},\"403\":{\"description\":\"The folder does not allow it\"},\"413\":{\"description\":\"Too large\"}}}},"
         "\"/api/v1/me/messages/{id}/receipt\":{\"post\":{\"summary\":\"Send a read receipt\",\"description\":\"For a message that carries Disposition-Notification-To: an RFC 8098 disposition notification (displayed, sent manually) queued from the account to that address, under the recipient checks a send makes. A message that did not ask is 400.\",\"responses\":{\"201\":{\"description\":\"queued, to\"},\"400\":{\"description\":\"No receipt was asked for, or the address is refused\"},\"404\":{\"description\":\"No such message\"}}}},"
         "\"/api/v1/me/brand-logo\":{\"get\":{\"summary\":\"The logo a sender's domain publishes (BIMI)\",\"description\":\"Query parameter: domain - the brand a message listing or a message names, which it does only for mail this server judged dmarc=pass and recorded so in its own Authentication-Results header. The answer is the logo itself, image/svg+xml, when default._bimi at the domain (or its organizational domain) is a v=BIMI1 record with an l= location, the domain's DMARC policy is quarantine or reject for all of its mail, the location is https at a public address and answers within ten seconds and 32 KB, and the SVG is Tiny PS - a tiny-ps root, a title, and no script, foreignObject, image, link, animation, style element, event attribute, entity or external reference. A Verified Mark Certificate (a=) is not checked. Every answer, found or not, is kept a day per domain. Otherwise 404 with error (the reason) and outcome: no_record, declined, weak_policy, not_https, fetch_failed, not_tiny_ps or switched_off (BrandLogosEnabled is off); a domain that is not a domain name is 400 bad_domain.\",\"responses\":{\"200\":{\"description\":\"The logo, image/svg+xml\"},\"400\":{\"description\":\"outcome bad_domain\"},\"404\":{\"description\":\"error and outcome\"}}}},"
         "\"/api/v1/me/subscriptions\":{\"get\":{\"summary\":\"The senders whose mail offers a way to unsubscribe\",\"description\":\"Every sender of the mail in the signed-in account's own folders - not Drafts, Sent, Junk or Trash - that carries a List-Unsubscribe header and arrived in the last days days (1 to 365, default 90), one entry per From address: address, name, list_id, count and unread in the window, first and last (when the first and the newest arrived), message_id (the newest message, which is what POST /api/v1/me/messages/{id}/unsubscribe is given), method - one-click when the newest offers RFC 8058's one-click POST to an https address, mail when it offers a mailto, page when it offers only an https address a person has to visit, and url, that address, for page alone. Most first, then newest. At most 5000 message headers are read per request, newest first in each folder: scanned says how many were, complete whether that was all of the window.\",\"responses\":{\"200\":{\"description\":\"days, since, scanned, complete, subscriptions\"},\"400\":{\"description\":\"days is not a whole number from 1 to 365\"}}}},"
         "\"/api/v1/me/messages/{id}/unsubscribe\":{\"post\":{\"summary\":\"Unsubscribe by the list's own method\",\"description\":\"With List-Unsubscribe-Post: List-Unsubscribe=One-Click and an https address in List-Unsubscribe, the one RFC 8058 POST (method one-click, with the list's status) - made only to a public address, judged by what the name resolves to and connected to those addresses, never through HttpProxy, within ten seconds and 32 KB; else a message to the mailto address with its subject (method mail, queued). Neither is 400. A mailto address carrying a control character is refused.\",\"responses\":{\"200\":{\"description\":\"one-click: ok, status\"},\"201\":{\"description\":\"mail: queued, to\"},\"400\":{\"description\":\"No method, or a mailto address that cannot be written into a header\"},\"502\":{\"description\":\"The list could not be reached, or its address is not public (error says which)\"}}}},"
         "\"/api/v1/me/messages/{id}/source\":{\"get\":{\"summary\":\"The message as it is on disk\",\"description\":\"message/rfc822, as a download named message-{id}.eml, under the same right as reading the message. Larger than 25 MB is 413.\",\"responses\":{\"200\":{\"description\":\"The file\"},\"404\":{\"description\":\"No such message\"},\"413\":{\"description\":\"Too large for this route\"}}}},"
         "\"/api/v1/me/folders/{id}/empty\":{\"post\":{\"summary\":\"Empty a Junk or Trash folder\",\"description\":\"Every message in the folder is expunged for good - or, with older_than_days=N (digits, 0 to 36500; 0 is everything), only what arrived more than N days ago; any other value is refused rather than read as everything. At most 500 go per call: remaining says how many more match, so call again until it is 0. Any other folder is 400: emptying is what those two are for.\",\"responses\":{\"200\":{\"description\":\"deleted (how many), remaining, folder_id, older_than_days\"},\"400\":{\"description\":\"Not a Junk or Trash folder, or older_than_days is not a number of days\"},\"403\":{\"description\":\"The folder does not allow it\"},\"404\":{\"description\":\"No such folder\"}}}},"
         "\"/api/v1/me/messages/{id}/move\":{\"post\":{\"summary\":\"Move one message to another of the account's folders\",\"description\":\"Body: folder_id, or to = archive | junk | trash | inbox - the folder designated so, made by that name when the account has none. As MOVE does: a copy with a new UID in the destination, then the original expunged, every session on either folder told; a move into or out of Junk teaches spamd when SpamAssassinLearnOnMove is on. Another account's folder is 404.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"properties\":{\"folder_id\":{\"type\":\"integer\"},\"to\":{\"type\":\"string\",\"enum\":[\"archive\",\"junk\",\"trash\",\"inbox\"]}}}}}},\"responses\":{\"200\":{\"description\":\"id (the new one), folder_id\"},\"400\":{\"description\":\"folder_id missing, or the same folder\"},\"403\":{\"description\":\"A folder does not allow it\"},\"404\":{\"description\":\"Not this account's message or folder\"}}}},"
         "\"/api/v1/me/messages/{id}/attachments/{index}\":{\"get\":{\"summary\":\"One attachment, decoded, as a download\",\"description\":\"index is the attachment's position in the message's attachments list. Served under the very media type that listing reports in content_type - so an img element pointed here renders, with nosniff set - except the types a browser would run or render (HTML, SVG, XML, script), which go out as application/octet-stream, and a part declaring no usable type, which does too; with Content-Disposition attachment (the name in both filename and RFC 8187 filename*), nosniff, a sandbox policy and no-store. A message over 32 MB is not parsed.\",\"responses\":{\"200\":{\"description\":\"The attachment's bytes\"},\"404\":{\"description\":\"Not this account's message, or no such attachment\"},\"413\":{\"description\":\"The message is too large to read here\"}}}},"
         "\"/api/v1/domains\":{"
         "\"get\":{\"summary\":\"List domains\",\"description\":\"A domain-restricted key sees only its own domains. Each entry: name, active, postmaster.\",\"responses\":{\"200\":{\"description\":\"Array of domains\"}}},"
         "\"post\":{\"summary\":\"Create a domain\",\"description\":\"Body: name (required), active (default true) and postmaster. The name is judged as the Control Panel judges it - a valid domain name, not one a domain alias already has - and every other setting takes the default a new domain gets there. Server-wide; refused for domain-restricted keys.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"name\"],\"properties\":{\"name\":{\"type\":\"string\"},\"active\":{\"type\":\"boolean\"},\"postmaster\":{\"type\":\"string\"}}}}}},\"responses\":{\"201\":{\"description\":\"Created: name, active, postmaster\"},\"400\":{\"description\":\"name missing, not a domain name, or taken by a domain alias (the reason is in error)\"},\"409\":{\"description\":\"A domain with that name exists\"}}}},"
         "\"/api/v1/domains/{domain}\":{"
         "\"get\":{\"summary\":\"One domain, whole\",\"description\":\"The domain by name, with every field the listing shows and the update takes - the document hmctl domain get and Get-HmDomain -Name answer with. Nothing here is a secret: a relay password is never in it, and a DKIM key is named by its file and not by its content. A domain-restricted key may read the domains it is issued for, and is refused another's with 403.\",\"parameters\":[{\"name\":\"domain\",\"in\":\"path\",\"required\":true,\"schema\":{\"type\":\"string\"}}],\"responses\":{\"200\":{\"description\":\"The domain, as one entry of the listing\"},\"404\":{\"description\":\"Unknown domain\"}}},"
         "\"put\":{\"summary\":\"Change a domain: on or off, its postmaster, its limits and everything else the Control Panel's domain pages hold, or its name\",\"description\":\"Body: active (required) and any subset of postmaster, name (a new name renames the domain and every address in it, as the Control Panel does), max_message_size_kb, max_size_mb, max_account_size_mb, max_accounts, max_aliases, max_lists and their switches max_accounts_enabled, max_aliases_enabled, max_lists_enabled, plus_addressing_enabled, plus_addressing_character, use_greylisting, signature_enabled, signature_method (set_if_not_specified, overwrite or append), signature_plain_text, signature_html, signature_add_to_replies, signature_add_to_local_mail, dkim_enabled, dkim_selector, dkim_private_key_file, dkim_signing_algorithm (sha1 or sha256), dkim_header_canonicalization and dkim_body_canonicalization (simple or relaxed), dkim_secondary_selector and dkim_secondary_private_key_file (the key a rotation promotes), dkim_sign_aliases, dkim_rotation_enabled (rotate the key on a schedule), dkim_rotation_interval_days (1 to 3650), dkim_rotation_propagation_hours (1 to 720), dkim_rotation_retention_days (1 to 365) and dkim_rotation_key_bits (2048, 3072 or 4096) - the rotation's policy; its state and its operations are under /dkim/rotation, and while a rotation manages the domain a save that would change its four selector and key file keys is refused with 409, message_retention_days, relay_host, relay_port, relay_requires_auth, relay_username, relay_password (write-only), relay_connection_security, vacation_enabled, vacation_subject, vacation_message, vacation_internal_subject and vacation_internal_message (the reply a sender in the same domain gets, when set), vacation_external_override, ad_domain_name (the Active Directory domain the domain's AD accounts log on to), external_tag_subject and external_tag_header (tag mail from outside this installation in the subject, in a header, or both), external_tag_text (the subject tag, at most 100 characters; empty is [EXTERNAL]), first_contact_tip (tell the reader when an outside sender has not written to the account before), disclaimer_enabled, disclaimer_plain_text and disclaimer_html (the legal footer appended to mail leaving the organisation), quarantine_digest and quarantine_digest_hour (whether each account of the domain is sent a daily list of what the quarantine holds for it, and at what local hour; the links in it need Settings.QuarantineDigestBaseUrl), openpgp_publish (publish this domain's accounts' OpenPGP public keys in the Web Key Directory; the server setting and each account's own switch have to be on as well) and openpgp_publish_new_accounts (what an account CREATED in this domain from now on starts with; it never changes an account that already exists), webhooks_enabled (whether anything this domain's mail or accounts do raises an outbound webhook event at all - on by default, and an off switch a domain administrator holds whatever the subscriptions at /api/v1/webhooks ask for) and webhook_subjects (whether a webhook body about this domain may carry a message subject - OFF by default on every domain, because a subject line is what a message is about; a subject travels only where this and the subscription's include_subject both say so), and country_policy (the country policy applied when an account of this domain signs in, on any protocol: empty is off, allow:GB,IE admits only those countries, block:CN,RU refuses those; a refused sign-in is the ordinary invalid-credentials answer and is counted against neither the account nor the address), session_idle_seconds and session_lifetime_seconds (how long a signed-in browser session of this domain's accounts may go unused, and live at all; 0 takes the server-wide value, at most 2592000), session_one_per_device and session_bind_to_address (0 takes the server-wide answer, 1 is on, 2 is off), and report_mailbox (the address this domain's DMARC aggregate and TLS-RPT reports are posted to - what its rua= tags name - and the one address they are read from into /dmarc-reports and /tls-reports; one address in a domain this server hosts, or empty, which is the default and reads nothing; the account or alias at it is the administrator's to make). A field left out keeps its value; everything is checked before anything is applied, and an unknown field or a wrong type is a 400 naming it.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"active\"],\"properties\":{\"active\":{\"type\":\"boolean\"},\"postmaster\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"}}}}}},\"responses\":{\"200\":{\"description\":\"The domain as saved, every field the listing shows\"},\"400\":{\"description\":\"active missing, a field refused, or the save refused (the reason is in error)\"},\"404\":{\"description\":\"Unknown domain\"},\"409\":{\"description\":\"The save would change the selectors or key files a DKIM key rotation manages\"}}},"
         "\"delete\":{\"summary\":\"Delete a domain with everything in it\",\"description\":\"The accounts and their messages, the aliases, the distribution lists, the domain aliases and the domain's directories go with it, exactly as when the Control Panel deletes a domain. Server-wide; refused for domain-restricted keys.\",\"responses\":{\"200\":{\"description\":\"Deleted\"},\"404\":{\"description\":\"Unknown domain\"}}}},"
         "\"/api/v1/domains/{domain}/accounts\":{"
         "\"get\":{\"summary\":\"List accounts in a domain\",\"description\":\"Each entry: address, active, max_size_mb (0 is no limit), first_name, last_name and admin_level (user, domain or server) - the columns hmctl accounts export and Export-HmAccount write, under the names the create takes, so an entry with a password added is a create body. GET /api/v1/accounts/{address} reads the rest of an account. Scoped to the domain.\",\"responses\":{\"200\":{\"description\":\"Array of accounts\"},\"404\":{\"description\":\"Unknown domain\"}}},"
         "\"post\":{\"summary\":\"Create an account\",\"description\":\"One account, made in the domain as the Control Panel makes one: the address must be in the domain and is judged with the domain's own limits, and the password, read as UTF-8, goes through the password policy and the breach check (breach_check_enabled), as when the Control Panel sets one, before it is stored under the configured hash. hmctl accounts import and Import-HmAccount send each row of a spreadsheet here. An unknown field, or a field of the wrong type, is refused by name rather than ignored, so a password of digits alone is sent as a string. A key restricted to named domains may make a user or a domain administrator, never a server administrator. Scoped to the domain. Body: address and password (required), active (default true), first_name and last_name (at most 60 characters each), max_size_mb (megabytes; 0, the default, is none; maxSizeMB is the older spelling) and admin_level (user, domain or server; default user).\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"address\",\"password\"],\"additionalProperties\":false,\"properties\":{\"address\":{\"type\":\"string\"},\"password\":{\"type\":\"string\",\"writeOnly\":true},\"active\":{\"type\":\"boolean\"},\"first_name\":{\"type\":\"string\",\"maxLength\":60},\"last_name\":{\"type\":\"string\",\"maxLength\":60},\"max_size_mb\":{\"type\":\"integer\",\"minimum\":0},\"maxSizeMB\":{\"type\":\"integer\",\"minimum\":0,\"deprecated\":true},\"admin_level\":{\"type\":\"string\",\"enum\":[\"user\",\"domain\",\"server\"]}}}}}},\"responses\":{\"201\":{\"description\":\"Created: address, created\"},\"400\":{\"description\":\"address or password missing, an unknown field, a field of the wrong type, a name longer than 60 characters, a max_size_mb that is not a whole number of megabytes, an admin_level that is not user, domain or server, an address outside the domain, a password the policy refuses or one found in the published breach corpus (reason: breached), or the save refused (the reason is in error)\"},\"403\":{\"description\":\"A domain-restricted key naming admin_level server, or a domain outside the key's own\"},\"404\":{\"description\":\"Unknown domain\"},\"409\":{\"description\":\"An account with that address exists\"},\"503\":{\"description\":\"The breach check could not reach its endpoint and breach_check_unreachable_action is refuse (reason: breach_check_unavailable); nothing changed\"}}}},"
         "\"/api/v1/queue\":{\"get\":{\"summary\":\"List the delivery queue\",\"description\":\"Server-wide; refused for domain-restricted keys.\",\"responses\":{\"200\":{\"description\":\"Array of queued messages\"}}}},"
         "\"/api/v1/queue/{id}/retry\":{\"post\":{\"summary\":\"Retry a queued message now\",\"responses\":{\"200\":{\"description\":\"Rescheduled\"},\"404\":{\"description\":\"Unknown id\"}}}},"
         "\"/api/v1/queue/{id}\":{\"delete\":{\"summary\":\"Remove a message from the queue\",\"responses\":{\"200\":{\"description\":\"Deleted\"},\"404\":{\"description\":\"Unknown id\"}}}},"
         "\"/api/v1/quarantine\":{\"get\":{\"summary\":\"List quarantined messages\",\"description\":\"Server-wide; refused for domain-restricted keys. Bounded to the newest 1000.\",\"responses\":{\"200\":{\"description\":\"Array of quarantined messages\"}}}},"
         "\"/api/v1/quarantine/{id}/release\":{\"post\":{\"summary\":\"Release a quarantined message to its original recipients\",\"description\":\"Delivery is direct rather than back through the filters: a release is an administrator overruling them.\",\"responses\":{\"200\":{\"description\":\"Released\"},\"404\":{\"description\":\"Unknown id\"}}}},"
         "\"/api/v1/quarantine/{id}\":{\"delete\":{\"summary\":\"Delete a quarantined message\",\"responses\":{\"200\":{\"description\":\"Deleted\"},\"404\":{\"description\":\"Unknown id\"}}}},"
         "\"/api/v1/tlsa\":{\"get\":{\"summary\":\"Recommended DANE TLSA records for the configured certificates\",\"responses\":{\"200\":{\"description\":\"Array of TLSA records\"}}}},"
         "\"/api/v1/srv\":{\"get\":{\"summary\":\"Recommended client-discovery SRV records (RFC 6186/8314 and Outlook autodiscover) for the enabled listeners\",\"description\":\"One record set per active domain; a domain-restricted key sees only its own domains. Only services that are enabled and not loopback-bound are advertised.\",\"responses\":{\"200\":{\"description\":\"Array of SRV records\"}}}},"
         "\"/api/v1/update\":{\"get\":{\"summary\":\"The update check\'s verdict\",\"description\":\"state: 0 not checked since the service started, 1 up to date, 2 a newer release is available, 3 its installer is downloaded and verified, 4 installing, 5 the last check failed (lastError). availableVersion, releaseName, publishedAt, releaseUrl and installer describe the newer release when there is one. Nothing is fetched by this route; the scheduled check (UpdateCheckEnabled) or POST /api/v1/update/check does that.\",\"responses\":{\"200\":{\"description\":\"The verdict\"}}}},"
         "\"/api/v1/update/check\":{\"post\":{\"summary\":\"Read the release feed now\",\"description\":\"Runs the update check on this request, whether or not the scheduled check is on, and returns the verdict as GET /api/v1/update does. Refused for read-only keys.\",\"responses\":{\"200\":{\"description\":\"The verdict after the check; state 5 with lastError when the feed could not be read\"}}}},"
         "\"/api/v1/update/download\":{\"post\":{\"summary\":\"Download and verify the newer release\'s installer\",\"description\":\"Fetches the installer the last check found and its Sigstore bundle into the data directory\'s Updates folder and verifies the installer against the bundle: digest, certificate chain at the time the transparency log recorded the signature, the release workflow\'s identity, the signature, and the log\'s own signature. A file that fails is deleted. Nothing is run. Returns the verdict as GET /api/v1/update does; state 3 when the installer is in place and verified, 5 with lastError when it is not. Refused for read-only keys.\",\"responses\":{\"200\":{\"description\":\"The verdict after the download\"}}}},"
         "\"/api/v1/update/install\":{\"post\":{\"summary\":\"Apply the verified installer\",\"description\":\"Verifies the downloaded installer once more, fetches and verifies the running version\'s installer as the rollback image where the feed offers it, and hands the installer to the update helper, which runs it silently, waits for the service to come back, runs the rollback image if it does not, and writes an outcome the service reports at its next start (apply in GET /api/v1/update). On Linux the installer is the .deb, .rpm or AppImage this server was installed from, and the helper is the packaged hmailserver-update, run as root by hmailserver-update.path when the server writes its request to /run/hmailserver: it verifies the package again, stops the unit, installs or swaps the file, starts the unit, rolls back if it does not come back, and writes the same outcome. The service stops and starts during the update. Returns state 4 when the helper was started. Refused for read-only keys.\",\"responses\":{\"200\":{\"description\":\"The verdict; state 4 when the helper was started, 5 with lastError when it was not\"}}}},"
         "\"/api/v1/diagnostics/run\":{\"post\":{\"summary\":\"Run the server's diagnostic tests\",\"description\":\"What the desktop program's Diagnostics page runs over COM (Diagnostics.PerformTests): the MX records of a domain hosted here, an outbound connection on port 25 to a domain elsewhere, a connection to this server's own ports, IPv6, the data and backup directories, the IP ranges, the error logs and the installation paths. Body: local_domain (a domain hosted here) and test_domain (one to send to; example.com when absent). Each result: name, description, success and details. The tests connect out and wait for their timeouts, so the answer can take some seconds. Refused for read-only keys.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"properties\":{\"local_domain\":{\"type\":\"string\"},\"test_domain\":{\"type\":\"string\"}}}}}},\"responses\":{\"200\":{\"description\":\"local_domain, test_domain, results\"}}}},"
         "\"/api/v1/metrics/history\":{\"get\":{\"summary\":\"The history of one metric\",\"description\":\"Query parameters: metric (a name from the exporter without the hmailserver_ prefix, e.g. sessions_smtp or processed_messages_total) and range (24h, 7d or 30d - samples averaged per minute, per ten minutes or per hour). Counters are totals; a rate is the difference between two samples. Empty when MetricsHistoryDays is 0.\",\"responses\":{\"200\":{\"description\":\"The samples\"},\"400\":{\"description\":\"Unknown metric or range\"}}}},"
         "\"/api/v1/apikeys\":{"
         "\"get\":{\"summary\":\"List API keys\",\"description\":\"Administrator password only.\",\"responses\":{\"200\":{\"description\":\"Array of keys, never the clear-text tokens\"}}},"
         "\"post\":{\"summary\":\"Create an API key\",\"description\":\"Administrator password only. The token is returned once, at creation.\",\"responses\":{\"201\":{\"description\":\"Created\"}}}},"
         "\"/api/v1/apikeys/{id}\":{\"delete\":{\"summary\":\"Revoke an API key\",\"description\":\"Administrator password only.\",\"responses\":{\"200\":{\"description\":\"Revoked\"}}}},"
         "\"/api/v1/ipranges\":{"
         "\"get\":{\"summary\":\"List the IP ranges\",\"description\":\"Each entry: id, name, lower, upper, priority, the permission flags, expires and expires_time (the time on the server's clock at which an expiring range is removed, as the auto-ban sets it; empty for a range that does not expire). Server-wide; refused for domain-restricted keys.\",\"responses\":{\"200\":{\"description\":\"Array of ranges with their permissions\"}}},"
         "\"post\":{\"summary\":\"Create an IP range\",\"description\":\"Body: name, lower and upper (required), priority, the permission flags, and expires with expires_time (YYYY-MM-DD HH:MM:SS on the server's clock, or a date alone for midnight; required when expires is true and taken only then). A range that expires is removed once that time passes, as one the auto-ban placed is. The same check as saving the range in the Control Panel. Server-wide; refused for domain-restricted and read-only keys.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"name\",\"lower\",\"upper\"],\"properties\":{\"name\":{\"type\":\"string\"},\"lower\":{\"type\":\"string\"},\"upper\":{\"type\":\"string\"},\"priority\":{\"type\":\"integer\"},\"allow_smtp\":{\"type\":\"boolean\"},\"allow_imap\":{\"type\":\"boolean\"},\"allow_pop3\":{\"type\":\"boolean\"},\"deliver_local_to_local\":{\"type\":\"boolean\"},\"deliver_local_to_remote\":{\"type\":\"boolean\"},\"deliver_remote_to_local\":{\"type\":\"boolean\"},\"deliver_remote_to_remote\":{\"type\":\"boolean\"},\"require_auth_local_to_local\":{\"type\":\"boolean\"},\"require_auth_local_to_remote\":{\"type\":\"boolean\"},\"require_auth_remote_to_local\":{\"type\":\"boolean\"},\"require_auth_remote_to_remote\":{\"type\":\"boolean\"},\"require_tls_for_auth\":{\"type\":\"boolean\"},\"spam_protection\":{\"type\":\"boolean\"},\"virus_protection\":{\"type\":\"boolean\"},\"expires\":{\"type\":\"boolean\"},\"expires_time\":{\"type\":\"string\"}}}}}},\"responses\":{\"201\":{\"description\":\"Created, with its id\"},\"400\":{\"description\":\"Missing name, an address that does not parse, an expiry without its time or a time that is not YYYY-MM-DD HH:MM:SS\"}}}},"
         "\"/api/v1/ipranges/{id}\":{\"put\":{\"summary\":\"Change an IP range\",\"description\":\"Body: any subset of the fields POST takes - name, lower, upper, priority, the permission flags, expires and expires_time; a field left out keeps its value, and a body that turns expiry on must carry expires_time in it. The same check as saving the range in the Control Panel; nothing changes when it is refused. Server-wide; refused for domain-restricted and read-only keys.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\"}}}},\"responses\":{\"200\":{\"description\":\"The range as saved, as the listing shows it\"},\"400\":{\"description\":\"A field refused (error names it); nothing changed\"},\"404\":{\"description\":\"Unknown id\"}}},"
         "\"delete\":{\"summary\":\"Delete an IP range\",\"responses\":{\"200\":{\"description\":\"Deleted\"},\"404\":{\"description\":\"Unknown id\"}}}},"
         "\"/api/v1/domains/{domain}/lists\":{"
         "\"get\":{\"summary\":\"List the distribution lists in a domain, with their members\",\"description\":\"Each entry: address, active, require_auth, mode (public, membership, announcement or domain_members), require_sender_address, moderator_address, bounce_address, membership and members. members is the addresses kept by hand; membership is the query that finds the rest among the server's accounts when a message is sent to the list, and is empty for a list whose members are its rows alone. GET /api/v1/lists/{address}/members answers what the two come to now. Scoped to the domain.\",\"responses\":{\"200\":{\"description\":\"Array of lists\"},\"404\":{\"description\":\"Unknown domain\"}}},"
         "\"post\":{\"summary\":\"Create a distribution list\",\"description\":\"Body: address (required, in this domain), members (an array of addresses), active (default true), require_auth (default false), mode (public, membership, announcement or domain_members; default public, and any other word is refused as put_Mode refuses it), require_sender_address (the one sender an announcement list accepts), moderator_address, bounce_address and membership. membership is the list's query, evaluated when a message is sent to it and never stored: empty (the members are the rows in members, which is every list made before this existed), domain (every active account of this domain), domain:NAME (every active account of that domain, which this server has to host), pattern:WILDCARD (active accounts whose address matches it, with * for any run of characters and ? for one; the part after the @ has to name a domain this server hosts unless it is itself a wildcard) or everyone (every active account of every active domain). What it answers is in union with members and deduplicated by address, and a query list still honours require_auth, require_sender_address, mode and moderator_address. A key confined to named domains may set empty, domain, or a pattern in a domain it holds; the other forms reach accounts it does not administer and are refused. Saved as the Control Panel saves a list, with the same limitation check. Scoped to the domain.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"address\"],\"properties\":{\"address\":{\"type\":\"string\"},\"members\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"active\":{\"type\":\"boolean\"},\"require_auth\":{\"type\":\"boolean\"},\"mode\":{\"type\":\"string\",\"enum\":[\"public\",\"membership\",\"announcement\",\"domain_members\"]},\"require_sender_address\":{\"type\":\"string\"},\"moderator_address\":{\"type\":\"string\"},\"bounce_address\":{\"type\":\"string\"},\"membership\":{\"type\":\"string\"}}}}}},\"responses\":{\"201\":{\"description\":\"Created: the list as the listing shows it\"},\"400\":{\"description\":\"Missing address, one outside the domain, a mode that is not one of the four words, a membership that is none of the five forms or that names a domain this server does not host, a field of the wrong type, an unknown field, or the save refused (the reason is in error)\"},\"403\":{\"description\":\"A membership reaching past the domains this key administers\"},\"404\":{\"description\":\"Unknown domain\"},\"409\":{\"description\":\"A list with that address exists\"}}}},"
         "\"/api/v1/lists/{address}\":{"
         "\"put\":{\"summary\":\"Change a distribution list\",\"description\":\"Body: any subset of active, require_auth, mode (public, membership, announcement or domain_members), require_sender_address, moderator_address, bounce_address and membership; a field left out keeps its value, and the members are not changed here. membership takes the five forms the create documents, and an empty string turns a query list back into a static one - either way its rows are untouched. Everything is checked before anything is applied. Scoped to the address's domain.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"properties\":{\"active\":{\"type\":\"boolean\"},\"require_auth\":{\"type\":\"boolean\"},\"mode\":{\"type\":\"string\",\"enum\":[\"public\",\"membership\",\"announcement\",\"domain_members\"]},\"require_sender_address\":{\"type\":\"string\"},\"moderator_address\":{\"type\":\"string\"},\"bounce_address\":{\"type\":\"string\"},\"membership\":{\"type\":\"string\"}}}}}},\"responses\":{\"200\":{\"description\":\"The list as saved, as the listing shows it\"},\"400\":{\"description\":\"A mode that is not one of the four words, a membership that is none of the five forms or that names a domain this server does not host, a field of the wrong type, an unknown field, or the save refused (the reason is in error)\"},\"403\":{\"description\":\"A membership reaching past the domains this key administers\"},\"404\":{\"description\":\"Unknown list\"}}},"
         "\"delete\":{\"summary\":\"Delete a distribution list\",\"responses\":{\"200\":{\"description\":\"Deleted\"},\"404\":{\"description\":\"Unknown list\"}}}},"
         "\"/api/v1/domains/{domain}/dkim\":{\"get\":{\"summary\":\"The DKIM signing configuration of a domain\",\"responses\":{\"200\":{\"description\":\"enabled, selector, sign_aliases and the private key file\"},\"404\":{\"description\":\"Unknown domain\"}}}},"
         "\"/api/v1/logs\":{\"get\":{\"summary\":\"List the log files\",\"description\":\"Server-wide; refused for domain-restricted keys.\",\"responses\":{\"200\":{\"description\":\"Array of files with size and creation time\"}}}},"
         "\"/api/v1/logs/{name}\":{\"get\":{\"summary\":\"The last lines of a log file\",\"description\":\"Query parameter lines (default 200, at most 2000). The name must be one the list returns; anything with a path in it is refused.\",\"responses\":{\"200\":{\"description\":\"The lines, newest last\"},\"400\":{\"description\":\"Not a log file name\"},\"404\":{\"description\":\"No such log file\"}}}},"
         "\"/api/v1/backup\":{"
         "\"get\":{\"summary\":\"The backup manager's status text and the last lines of the backup log\",\"responses\":{\"200\":{\"description\":\"status (the last failure reason, if any) and log (the backup log's tail, newest last)\"}}},"
         "\"post\":{\"summary\":\"Start a backup with the configured settings\",\"description\":\"Runs on the maintenance queue; poll GET for the outcome.\",\"responses\":{\"202\":{\"description\":\"Started\"},\"409\":{\"description\":\"A backup or restore is already running, or the backup is not configured\"}}}},"
         "\"/api/v1/backup/restore\":{"
         "\"post\":{\"summary\":\"Restore a backup archive\",\"description\":\"Body: file (the archive's path on this server, required), passphrase (the one the archive was encrypted with; accepted here and never stored, logged or returned), and restore_settings, restore_domains and restore_messages - at least one of them true, and messages only together with domains. The archive is opened and everything that can be checked is checked before anything is changed: an encrypted archive with no passphrase, a wrong passphrase, a damaged archive, a part the archive does not hold, or an archive from a newer hMailServer is refused with the reason and nothing on this server is touched. What follows is destructive - the restore deletes every domain, account and public folder it is replacing - and runs on the maintenance queue; poll GET /api/v1/backup for the outcome. The check itself is bounded at two minutes so that an archive on a drive that has stopped answering cannot hold a worker thread; that answers 504 and starts nothing, and the Control Panel, which waits as long as it takes, still restores such an archive. Server-wide; refused for domain-restricted keys.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"file\"],\"properties\":{\"file\":{\"type\":\"string\",\"description\":\"The archive's path on the server\"},\"passphrase\":{\"type\":\"string\",\"writeOnly\":true,\"description\":\"The passphrase an encrypted archive was written with\"},\"restore_settings\":{\"type\":\"boolean\"},\"restore_domains\":{\"type\":\"boolean\"},\"restore_messages\":{\"type\":\"boolean\"}}}}}},\"responses\":{\"202\":{\"description\":\"Started: encrypted, and contains (settings, domains, messages)\"},\"400\":{\"description\":\"Refused before anything was changed; the reason is in error\"},\"409\":{\"description\":\"A backup or restore is already running\"},\"504\":{\"description\":\"The archive could not be read within the two minutes this route allows - a slow or unresponsive drive, not a verdict on the archive; nothing was started\"}}}},"
         "\"/api/v1/archive\":{\"get\":{\"summary\":\"Search the archive index\",\"description\":\"Query parameters: domain and mailbox (exact), sender, recipient and subject (contains), since and until (YYYY-MM-DD HH:MM:SS), hold=1 for held copies only, limit (1-1000, default 200). Newest first. A domain-restricted key must name one of its domains.\",\"responses\":{\"200\":{\"description\":\"Array of archive entries: id, time, domain, mailbox, direction, sender, recipients, subject, message_id, path, size, hold\"},\"403\":{\"description\":\"A domain-restricted key without a domain of its own\"}}}},"
         "\"/api/v1/archive/{id}\":{\"get\":{\"summary\":\"One archive entry\",\"responses\":{\"200\":{\"description\":\"The entry\"},\"404\":{\"description\":\"Unknown id\"}}}},"
         "\"/api/v1/archive/{id}/hold\":{"
         "\"post\":{\"summary\":\"Put an archived copy on legal hold\",\"description\":\"A held copy is never removed by the retention sweep or by an address erasure.\",\"responses\":{\"200\":{\"description\":\"Held\"},\"404\":{\"description\":\"Unknown id\"}}},"
         "\"delete\":{\"summary\":\"Lift the hold\",\"responses\":{\"200\":{\"description\":\"Released\"},\"404\":{\"description\":\"Unknown id\"}}}},"
         "\"/api/v1/openapi.json\":{\"get\":{\"summary\":\"This document\",\"responses\":{\"200\":{\"description\":\"The OpenAPI description\"}}}}";

      static const char *openApiTail =
         "},"
         "\"components\":{\"securitySchemes\":{"
         "\"basic\":{\"type\":\"http\",\"scheme\":\"basic\"},"
         "\"bearer\":{\"type\":\"http\",\"scheme\":\"bearer\"}}},"
         "\"security\":[{\"basic\":[]},{\"bearer\":[]}]"
         "}";

      // The write surface's paths come from the translation units that
      // implement them, so a route and its description still land in one
      // diff; each function returns entries that begin with a comma, or
      // nothing.
      AnsiString openApiJson = AnsiString(openApiHead);
      openApiJson += OpenApiSettingsPaths_();
      openApiJson += OpenApiRulesPaths_();
      openApiJson += OpenApiCertificatesPaths_();
      openApiJson += OpenApiRoutesPaths_();
      openApiJson += OpenApiRemoteDomainsPaths_();
      openApiJson += OpenApiFetchAccountsPaths_();
      openApiJson += OpenApiReaderFetchAccountsPaths_();
      openApiJson += OpenApiReaderFetchImportPaths_();
      openApiJson += OpenApiSenderAddressesPaths_();
      openApiJson += OpenApiMaskedAddressesPaths_();
      openApiJson += OpenApiRecoverablePaths_();
      openApiJson += OpenApiRecoveryPaths_();
      openApiJson += OpenApiPasskeyPaths_();
      openApiJson += OpenApiOidcPaths_();
      openApiJson += OpenApiConfidentialPaths_();
      openApiJson += OpenApiContactGroupsPaths_();
      openApiJson += OpenApiOfflinePaths_();
      openApiJson += OpenApiAdministrationPaths_();
      openApiJson += OpenApiAntiSpamListsPaths_();
      openApiJson += OpenApiGreylistBypassPaths_();
      openApiJson += OpenApiBlockedAttachmentsPaths_();
      openApiJson += OpenApiJournalRulePaths_();
      openApiJson += OpenApiSenderListPaths_();
      openApiJson += OpenApiSpamTrapPaths_();
      openApiJson += OpenApiDomainPolicyListPaths_();
      openApiJson += OpenApiDistributionListMembersPath_();
      openApiJson += OpenApiMailboxPaths_();
      openApiJson += OpenApiCalendarPaths_();
      openApiJson += OpenApiAccountResourcesPaths_();
      openApiJson += OpenApiGroupsPaths_();
      openApiJson += OpenApiDkimRotationPaths_();
      openApiJson += OpenApiReputationPaths_();
      openApiJson += OpenApiAccountReviewPaths_();
      openApiJson += OpenApiSetupChecklistPaths_();
      openApiJson += OpenApiSampleDomainPaths_();
      openApiJson += OpenApiAccountRulesPaths_();
      openApiJson += OpenApiDomainRulesPaths_();
      openApiJson += OpenApiOperatorPaths_();
      openApiJson += OpenApiAuditPaths_();
      openApiJson += OpenApiBouncesPaths_();
      openApiJson += OpenApiComplaintsPaths_();
      openApiJson += OpenApiDomainReportsPaths_();
      openApiJson += OpenApiReportsPaths_();
      openApiJson += OpenApiSessionFollowPaths_();
      openApiJson += OpenApiRolesPaths_();
      openApiJson += OpenApiOpenPgpPaths_();
      openApiJson += OpenApiWebhooksPaths_();
      openApiJson += OpenApiMailboxMovePaths_();
      openApiJson += openApiTail;

      // The same bytes for every caller, built on every call: compressed once.
      HttpResponse response = BuildResponse_(200, openApiJson);
      pageCache_.Fill(response);
      return response;
   }

   HttpResponse
   RestApiServer::HandleTlsa_()
   {
      // Recommended DANE TLSA records (3 1 1: DANE-EE, SPKI, SHA-256) for
      // every configured certificate, so administrators can publish or
      // verify DNS without manual hashing.
      AnsiString hostName = AnsiString(Configuration::Instance()->GetHostName());
      if (hostName.IsEmpty())
         hostName = "<your-mx-hostname>";

      AnsiString items;
      int count = 0;

      auto appendCertificate = [&](const String &name, const String &certificateFile)
      {
         if (certificateFile.IsEmpty() || !FileUtilities::Exists(certificateFile))
            return;

         AnsiString spkiHex;
         if (!AcmeClient::GetCertificateTlsa(certificateFile, spkiHex))
            return;

         AnsiString item;
         item.Format("{\"certificate\":\"%hs\",\"spki_sha256\":\"%hs\",\"record\":\"_25._tcp.%hs. IN TLSA 3 1 1 %hs\"}",
            JsonEscape_(Utf8_(name)).c_str(),
            spkiHex.c_str(),
            JsonEscape_(hostName).c_str(),
            spkiHex.c_str());

         if (count > 0)
            items += ",";

         items += item;
         count++;
      };

      SSLCertificates certificates;
      certificates.Refresh();

      for (int i = 0; i < certificates.GetCount(); i++)
      {
         std::shared_ptr<SSLCertificate> certificate = certificates.GetItem(i);
         if (certificate)
            appendCertificate(certificate->GetName(), certificate->GetCertificateFile());
      }

      if (count == 0)
         appendCertificate(_T("ACME (automatic)"), AcmeClient::GetCertificateDirectory() + FileUtilities::PathSeparator + _T("fullchain.pem"));

      AnsiString body;
      body.Format("{\"host\":\"%hs\",\"count\":%d,\"records\":[%hs]}",
         JsonEscape_(hostName).c_str(), count, items.c_str());

      return BuildResponse_(200, body);
   }

   AnsiString
   RestApiServer::QueryParameter_(const AnsiString &query, const AnsiString &name)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // The value of one query-string parameter, or "". Enough decoding for the
   // values these routes take (a metric name, a range): '+' and %XX. Anything a
   // caller could smuggle past that is refused by the handler's own validation.
   //---------------------------------------------------------------------------()
   {
      std::vector<AnsiString> pairs = StringParser::SplitString(query, "&");

      for (const AnsiString &pair : pairs)
      {
         int equals = pair.Find("=");
         AnsiString key = equals >= 0 ? pair.Mid(0, equals) : pair;

         if (key.CompareNoCase(name) != 0)
            continue;

         AnsiString raw = equals >= 0 ? pair.Mid(equals + 1) : AnsiString();
         AnsiString value;

         for (int i = 0; i < raw.GetLength(); i++)
         {
            char c = raw.GetAt(i);

            if (c == '+')
            {
               value += " ";
            }
            else if (c == '%' && i + 2 < raw.GetLength())
            {
               char hex[3] = { raw.GetAt(i + 1), raw.GetAt(i + 2), 0 };
               value += (char) strtol(hex, nullptr, 16);
               i += 2;
            }
            else
            {
               value += c;
            }
         }

         return value;
      }

      return "";
   }

   HttpResponse
   RestApiServer::HandleMetricsHistory_(const AnsiString &query)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // GET /api/v1/metrics/history?metric=<name>&range=24h|7d|30d. The three ranges
   // are the three views: a minute, ten minutes and an hour per point, so that
   // each answer is at most a few hundred points however long the retention.
   //---------------------------------------------------------------------------()
   {
      AnsiString metric = QueryParameter_(query, "metric");
      AnsiString range = QueryParameter_(query, "range");

      if (metric.IsEmpty() || !MetricsHistoryTask::IsMetricName(String(metric)))
      {
         AnsiString names;

         for (const AnsiString &name : MetricsHistoryTask::MetricNames())
            names += (names.IsEmpty() ? "\"" : ",\"") + name + "\"";

         return BuildResponse_(400, "{\"error\":\"unknown metric\",\"metrics\":[" + names + "]}");
      }

      int minutesBack = 24 * 60;
      int bucketMinutes = 1;

      if (range.IsEmpty() || range.CompareNoCase("24h") == 0)
      {
         minutesBack = 24 * 60;
         bucketMinutes = 1;
      }
      else if (range.CompareNoCase("7d") == 0)
      {
         minutesBack = 7 * 24 * 60;
         bucketMinutes = 10;
      }
      else if (range.CompareNoCase("30d") == 0)
      {
         minutesBack = 30 * 24 * 60;
         bucketMinutes = 60;
      }
      else
      {
         return BuildResponse_(400, "{\"error\":\"unknown range: use 24h, 7d or 30d\"}");
      }

      return BuildResponse_(200, MetricsHistoryTask::QueryAsJson(String(metric), minutesBack, bucketMinutes));
   }

   HttpResponse
   RestApiServer::HandleSrv_(const std::vector<String> &allowedDomains)
   {
      // Ready-to-publish client-discovery SRV records: RFC 6186 for
      // _imap/_imaps/_pop3/_pop3s/_submission, RFC 8314 section 5.1 for
      // _submissions, and the Outlook _autodiscover._tcp convention. Built
      // from the ports that are actually configured and enabled - the same
      // one-source-of-truth idea as /api/v1/tlsa, which hashes the real
      // certificates rather than restating them, and the same port ranking as
      // the autoconfig XML, so DNS discovery and autoconfig cannot disagree.
      //
      // A record is only emitted for a service that is genuinely served,
      // because a published SRV pointing at a dead port sends every client of
      // that domain to a socket that will never answer - worse than publishing
      // nothing, which at least leaves clients to their fallback probing. So:
      // a protocol whose service is switched off contributes nothing (its port
      // rows survive in the configuration, but no listener starts for them),
      // and a port bound to a loopback address contributes nothing (the only
      // clients that resolve SRV records are on other machines). What this
      // reads is the configuration, exactly as the autoconfig handlers do; a
      // port added since the last restart is advertised even though its
      // listener starts at the next one, which is the established semantics of
      // every client-discovery answer this server gives.
      AnsiString target = AnsiString(IniFileSettings::Instance()->GetAutoconfigClientHost());
      target.Trim();

      if (target.IsEmpty())
         target = AnsiString(Configuration::Instance()->GetHostName());

      target.Trim();
      target.MakeLower();

      // The placeholder convention of /api/v1/tlsa: with no host name
      // configured there is nothing honest to point a record at, so the
      // records carry a placeholder the administrator has to replace.
      if (target.IsEmpty())
         target = "<your-mail-hostname>";

      bool smtpEnabled = Configuration::Instance()->GetUseSMTP();
      bool imapEnabled = Configuration::Instance()->GetUseIMAP();
      bool pop3Enabled = Configuration::Instance()->GetUsePOP3();

      // Per protocol, the best implicit-TLS port and the best
      // STARTTLS-or-plain port. Ranked the way GetClientAccessSettings_ ranks
      // the autoconfig answer: required STARTTLS over optional over plain, and
      // the standard port over an unusual one.
      int imapsPort = 0, imapsRank = -1;
      int imapPort = 0, imapRank = -1;
      int pop3sPort = 0, pop3sRank = -1;
      int pop3Port = 0, pop3Rank = -1;
      int submissionsPort = 0, submissionsRank = -1;
      int submissionPort = 0, submissionRank = -1;

      auto consider = [](int &bestPort, int &bestRank, int candidatePort, int candidateRank)
      {
         if (candidateRank > bestRank)
         {
            bestRank = candidateRank;
            bestPort = candidatePort;
         }
      };

      // A copy of the vector, exactly as GetClientAccessSettings_ takes one,
      // rather than a Refresh() of the shared collection from this thread.
      std::vector<std::shared_ptr<TCPIPPort>> ports = Configuration::Instance()->GetTCPIPPorts()->GetVector();

      for (std::shared_ptr<TCPIPPort> port : ports)
      {
         if (!port)
            continue;

         // Bound to loopback: real, but unreachable from any machine that
         // would be resolving the record.
         if (port->GetAddress().GetAddress().is_loopback())
            continue;

         int portNumber = port->GetPortNumber();
         ConnectionSecurity security = port->GetConnectionSecurity();
         bool implicitTls = security == CSSSL;

         int starttlsRank = security == CSSTARTTLSRequired ? 30 :
                            security == CSSTARTTLSOptional ? 20 : 10;

         switch (port->GetProtocol())
         {
         case STIMAP:
            if (!imapEnabled)
               break;

            if (implicitTls)
               consider(imapsPort, imapsRank, portNumber, portNumber == 993 ? 2 : 1);
            else
               consider(imapPort, imapRank, portNumber, starttlsRank + (portNumber == 143 ? 5 : 0));
            break;

         case STPOP3:
            if (!pop3Enabled)
               break;

            if (implicitTls)
               consider(pop3sPort, pop3sRank, portNumber, portNumber == 995 ? 2 : 1);
            else
               consider(pop3Port, pop3Rank, portNumber, starttlsRank + (portNumber == 110 ? 5 : 0));
            break;

         case STSMTP:
            if (!smtpEnabled)
               break;

            // Port 25 is the server-to-server MX port, whatever its security
            // setting says. Advertising it for client submission would invite
            // every discovered client onto the port the MX record owns, which
            // is exactly the confusion RFC 6186 discovery exists to end.
            if (portNumber == 25)
               break;

            if (implicitTls)
               consider(submissionsPort, submissionsRank, portNumber, portNumber == 465 ? 2 : 1);
            else
               consider(submissionPort, submissionRank, portNumber, starttlsRank + (portNumber == 587 ? 5 : 0));
            break;

         default:
            break;
         }
      }

      struct ServiceRecord
      {
         AnsiString service;
         int priority;
         int weight;
         int port;
      };

      // Priority 0, weight 1 - the published RFC 6186 example form. With one
      // record per service name the two values carry no load anyway.
      std::vector<ServiceRecord> services;

      if (imapsPort > 0)
         services.push_back({ "_imaps._tcp", 0, 1, imapsPort });
      if (imapPort > 0)
         services.push_back({ "_imap._tcp", 0, 1, imapPort });
      if (pop3sPort > 0)
         services.push_back({ "_pop3s._tcp", 0, 1, pop3sPort });
      if (pop3Port > 0)
         services.push_back({ "_pop3._tcp", 0, 1, pop3Port });
      if (submissionsPort > 0)
         services.push_back({ "_submissions._tcp", 0, 1, submissionsPort });
      if (submissionPort > 0)
         services.push_back({ "_submission._tcp", 0, 1, submissionPort });

      // _autodiscover._tcp points Outlook at the web-services HTTPS listener.
      // The condition is the listener that is RUNNING, not the one configured:
      // WebServicesServer silently keeps HTTPS down when no certificate is
      // available yet (ACME may not have issued one), and an SRV record
      // pointing into that gap would break every Outlook profile setup until
      // it closed. Priority 0 weight 0, the form Microsoft's own documentation
      // publishes. Loopback binds are excluded for the same reason as the mail
      // ports above.
      int autodiscoverPort = WebServicesServer::GetHttpsListenPort();

      if (autodiscoverPort > 0 && IniFileSettings::Instance()->GetAutoconfigEnabled())
      {
         String webBindAddress = IniFileSettings::Instance()->GetWebServicesBindAddress();

         IPAddress parsedBind;
         bool loopbackBound = webBindAddress == _T("localhost") ||
            (parsedBind.TryParse(AnsiString(webBindAddress), false) && parsedBind.GetAddress().is_loopback());

         if (!loopbackBound)
            services.push_back({ "_autodiscover._tcp", 0, 0, autodiscoverPort });
      }

      AnsiString serviceJson;
      int serviceCount = 0;

      for (const ServiceRecord &service : services)
      {
         AnsiString entry;
         entry.Format("{\"service\":\"%hs\",\"priority\":%d,\"weight\":%d,\"port\":%d}",
            service.service.c_str(), service.priority, service.weight, service.port);

         if (serviceCount > 0)
            serviceJson += ",";

         serviceJson += entry;
         serviceCount++;
      }

      // One record set per active local domain: SRV owner names live under the
      // mail domain, not under the server's host name, so this is the shape an
      // administrator actually pastes into a zone file.
      Domains domains;
      domains.Refresh();

      AnsiString recordJson;
      int recordCount = 0;

      for (int i = 0; i < domains.GetCount(); i++)
      {
         std::shared_ptr<Domain> domain = domains.GetItem(i);
         if (!domain)
            continue;

         // An inactive domain accepts no mail; nothing should be inviting
         // clients to configure accounts in it.
         if (!domain->GetIsActive())
            continue;

         // The same filtering as the domain listing, for the same reason: a
         // key issued for one customer's domain must not be handed the names
         // of all the others as a side effect of asking for DNS records.
         if (!IsDomainAllowed_(allowedDomains, domain->GetName()))
            continue;

         AnsiString domainName = JsonEscape_(Utf8_(domain->GetName()));

         for (const ServiceRecord &service : services)
         {
            AnsiString entry;
            entry.Format("{\"domain\":\"%hs\",\"service\":\"%hs\",\"record\":\"%hs.%hs. IN SRV %d %d %d %hs.\"}",
               domainName.c_str(), service.service.c_str(),
               service.service.c_str(), domainName.c_str(),
               service.priority, service.weight, service.port,
               JsonEscape_(target).c_str());

            if (recordCount > 0)
               recordJson += ",";

            recordJson += entry;
            recordCount++;
         }
      }

      AnsiString body;
      body.Format("{\"target\":\"%hs\",\"count\":%d,\"services\":[%hs],\"records\":[%hs]}",
         JsonEscape_(target).c_str(), recordCount, serviceJson.c_str(), recordJson.c_str());

      return BuildResponse_(200, body);
   }

   AnsiString
   RestApiServer::GetRequestBody_(const AnsiString &request)
   {
      int bodyStart = request.Find("\r\n\r\n");
      if (bodyStart < 0)
         return "";

      return request.Mid(bodyStart + 4);
   }

   // A member by name: the object's own first, then, in document order, the
   // first object below it that has one.
   const JsonValue *
   RestApiServer::FindJsonMember_(const JsonValue &object, const std::string &key)
   {
      const JsonValue *own = object.Get(key);
      if (own)
         return own;
      const std::vector<std::pair<std::string, JsonValue> > &members = object.Members();
      for (size_t i = 0; i < members.size(); i++)
      {
         if (!members[i].second.IsObject())
            continue;
         const JsonValue *below = FindJsonMember_(members[i].second, key);
         if (below)
            return below;
      }
      return nullptr;
   }

   AnsiString
   RestApiServer::GetJsonStringValue_(const AnsiString &json, const AnsiString &key)
   {
      // The document is parsed and the member found by name - at the top
      // level, else the first of that name in any object below it, which is
      // what the search below found for a nested member - so a value that
      // happens to spell a key's name (a subject of exactly "from") is a
      // value. The search below is kept for what does not parse.
      {
         JsonValue document;
         std::string parseError;
         if (JsonValue::Parse(std::string(json.c_str(), json.size()), document, parseError) && document.IsObject())
         {
            const JsonValue *found = FindJsonMember_(document, std::string(key.c_str()));
            if (!found || !found->IsString())
               return "";
            return AnsiString(found->AsString().c_str());
         }
      }

      AnsiString needle = "\"" + key + "\"";

      int keyPosition = json.Find(needle);
      if (keyPosition < 0)
         return "";

      int colonPosition = json.Find(":", keyPosition + needle.GetLength());
      if (colonPosition < 0)
         return "";

      int valueStart = json.Find("\"", colonPosition);
      if (valueStart < 0)
         return "";

      valueStart++;

      // RFC 8259 section 7: the two-character escapes, and \uXXXX with a
      // surrogate pair for what is outside the BMP. The result stays UTF-8.
      AnsiString result;
      for (int i = valueStart; i < json.GetLength(); i++)
      {
         char character = json[i];

         if (character == '\\' && i + 1 < json.GetLength())
         {
            char next = json[i + 1];
            i++;

            switch (next)
            {
            case '\"': result += '\"'; continue;
            case '\\': result += '\\'; continue;
            case '/':  result += '/';  continue;
            case 'n':  result += '\n'; continue;
            case 'r':  result += '\r'; continue;
            case 't':  result += '\t'; continue;
            case 'b':  result += '\b'; continue;
            case 'f':  result += '\f'; continue;
            case 'u':
               {
                  unsigned int codePoint = 0;
                  if (!ReadJsonHex4_(json, i + 1, codePoint))
                  {
                     result += "\\u";
                     continue;
                  }
                  i += 4;

                  if (codePoint >= 0xD800 && codePoint <= 0xDBFF && i + 6 < json.GetLength() &&
                      json[i + 1] == '\\' && json[i + 2] == 'u')
                  {
                     unsigned int low = 0;
                     if (ReadJsonHex4_(json, i + 3, low) && low >= 0xDC00 && low <= 0xDFFF)
                     {
                        codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (low - 0xDC00);
                        i += 6;
                     }
                  }

                  AppendUtf8_(result, codePoint);
                  continue;
               }
            default:
               result += character;
               result += next;
               continue;
            }
         }

         if (character == '\"')
            break;

         result += character;
      }

      return result;
   }

   bool
   RestApiServer::ReadJsonHex4_(const AnsiString &json, int at, unsigned int &value)
   {
      if (at + 4 > json.GetLength())
         return false;

      value = 0;
      for (int i = 0; i < 4; i++)
      {
         char c = json[at + i];
         unsigned int digit;
         if (c >= '0' && c <= '9')
            digit = c - '0';
         else if (c >= 'a' && c <= 'f')
            digit = 10 + c - 'a';
         else if (c >= 'A' && c <= 'F')
            digit = 10 + c - 'A';
         else
            return false;

         value = (value << 4) | digit;
      }

      return true;
   }

   void
   RestApiServer::AppendUtf8_(AnsiString &out, unsigned int codePoint)
   {
      if (codePoint < 0x80)
      {
         out += (char) codePoint;
      }
      else if (codePoint < 0x800)
      {
         out += (char) (0xC0 | (codePoint >> 6));
         out += (char) (0x80 | (codePoint & 0x3F));
      }
      else if (codePoint < 0x10000)
      {
         out += (char) (0xE0 | (codePoint >> 12));
         out += (char) (0x80 | ((codePoint >> 6) & 0x3F));
         out += (char) (0x80 | (codePoint & 0x3F));
      }
      else
      {
         out += (char) (0xF0 | (codePoint >> 18));
         out += (char) (0x80 | ((codePoint >> 12) & 0x3F));
         out += (char) (0x80 | ((codePoint >> 6) & 0x3F));
         out += (char) (0x80 | (codePoint & 0x3F));
      }
   }

   HttpResponse
   RestApiServer::HandleUpdateGet_()
   {
      return BuildResponse_(200, UpdateChecker::ToJson(UpdateChecker::Current()));
   }

   HttpResponse
   RestApiServer::HandleUpdateInstall_()
   {
      String error;
      if (!UpdateInstaller::Apply(error))
         UpdateChecker::RecordFailure(error);
      return BuildResponse_(200, UpdateChecker::ToJson(UpdateChecker::Current()));
   }

   HttpResponse
   RestApiServer::HandleUpdateDownload_()
   {
      String error;
      UpdateDownloader::DownloadAndVerify(error);
      return BuildResponse_(200, UpdateChecker::ToJson(UpdateChecker::Current()));
   }

   HttpResponse
   RestApiServer::HandleDiagnosticsRun_(const AnsiString &requestBody)
   {
      String localDomain = JsonUtf8Value_(requestBody, "local_domain");
      localDomain.TrimLeft();
      localDomain.TrimRight();
      String testDomain = JsonUtf8Value_(requestBody, "test_domain");
      testDomain.TrimLeft();
      testDomain.TrimRight();
      if (testDomain.IsEmpty())
         testDomain = _T("example.com");

      Diagnostic diagnostic;
      diagnostic.SetLocalDomain(localDomain);
      diagnostic.SetTestDomain(testDomain);
      std::vector<DiagnosticResult> results = diagnostic.PerformTests();

      AnsiString json = "{\"local_domain\":\"" + JsonEscape_(Utf8_(localDomain)) + "\",\"test_domain\":\"" + JsonEscape_(Utf8_(testDomain)) + "\",\"results\":[";
      for (size_t i = 0; i < results.size(); i++)
      {
         if (i > 0)
            json += ",";
         json += "{\"name\":\"" + JsonEscape_(Utf8_(results[i].GetName())) +
            "\",\"description\":\"" + JsonEscape_(Utf8_(results[i].GetDescription())) +
            "\",\"success\":" + AnsiString(results[i].GetSuccess() ? "true" : "false") +
            ",\"details\":\"" + JsonEscape_(Utf8_(results[i].GetDetails())) + "\"}";
      }
      json += "]}";
      LOG_DEBUG("REST: the diagnostic tests were run for " + localDomain + " against " + testDomain + ".");
      return BuildResponse_(200, json);
   }

   HttpResponse
   RestApiServer::HandleUpdateCheck_()
   {
      // The check's own outcome is in the verdict it returns (state 5 and
      // lastError when the feed could not be read), so the response is 200
      // either way: the request - run a check - was carried out.
      String error;
      UpdateChecker::CheckNow(error);
      return BuildResponse_(200, UpdateChecker::ToJson(UpdateChecker::Current()));
   }

   AnsiString
   RestApiServer::Utf8_(const String &value)
   {
      AnsiString utf8;
      Unicode::WideToMultiByte(value, utf8);
      return utf8;
   }

   /*
      The credential, as the audit trail names it. "administrator" and "key:<id>"
      are the identity strings Authenticate_ already produces for the rate
      accounting and the log, so an audit row and a log line about the same
      request name the caller the same way.

      The kind and the role are read from caller.grant - the grant Authorize_
      has just decided this request on - and never from AdminLevel. An account
      that holds serveradmin or domainadmin is an "administrator", as it is
      over COM; every other account is an "account", a help desk included, and
      the role it acted under is recorded with the change. Before roles an
      account reached nothing here but its own /api/v1/me endpoints, so every
      account row was self-service; now an account can administer, and a row
      that did not say with what authority would not say enough.
   */
   AuditTrail::Actor
   RestApiServer::AuditActorFor_(const Caller &caller)
   {
      AuditTrail::Actor actor;

      if (caller.result == AuthenticationFailed)
         return actor;

      actor.present = true;
      actor.name = String(caller.identity);
      actor.address = caller.peer.ToString();

      // The Deck is a REST client with a session cookie, and it is worth saying
      // so: "who changed this, and were they at a browser or holding a key" is
      // the first question asked of a record like this.
      actor.interface_name = caller.via_session ? _T("Deck") : _T("REST");

      switch (caller.result)
      {
      case AuthenticatedAsAdministrator:
         actor.kind = _T("administrator");
         break;
      case AuthenticatedWithApiKey:
         actor.kind = _T("apikey");
         break;
      default:
         actor.kind = RoleManager::IsAdministratorGrant(caller.grant) ? _T("administrator") : _T("account");
         break;
      }

      // Empty for an ordinary account, for an app password and for a support
      // session, all of which hold no administrative grant.
      if (caller.grant.administrative)
         actor.role = caller.grant.name;

      return actor;
   }

   AnsiString
   RestApiServer::JsonEscape_(const AnsiString &value)
   {
      AnsiString result;
      result.reserve(value.GetLength() + 8);

      for (int i = 0; i < value.GetLength(); i++)
      {
         char character = value[i];

         switch (character)
         {
         case '\"':
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
            if (static_cast<unsigned char>(character) >= 0x20)
            {
               result += character;
            }
            else
            {
               // RFC 8259 section 7: a control character is written as \u00XX,
               // not dropped - a signature or a script has to come back whole.
               AnsiString escaped;
               escaped.Format("\\u%04x", (unsigned int) (unsigned char) character);
               result += escaped;
            }
            break;
         }
      }

      return result;
   }
}
