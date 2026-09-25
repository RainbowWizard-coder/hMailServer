// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Mailbox encryption at rest over the REST API (Util/MailboxKeys.h,
// docs/EncryptionAtRest.md).
//
//   GET    /api/v1/accounts/{address}/mailbox-encryption
//             whether the account is keyed, encrypts new mail, has its key
//             open now, is still waiting for its first sign-in (pending), the
//             key's fingerprint, and whether its recovery copy is sealed to the
//             domain's current recovery key.
//   PUT    /api/v1/accounts/{address}/mailbox-encryption   {"enabled": bool}
//             on: a key is made - which needs the domain's recovery key - or
//             the one there encrypts new mail again, and a new key ends every
//             session the account had; off: new mail is stored as it comes,
//             and what is encrypted stays encrypted, with its key.
//   DELETE /api/v1/accounts/{address}/mailbox-encryption?confirm=forfeit
//             the key removed, and with it every message still encrypted to
//             it. Refused without the confirmation in the query.
//   POST   /api/v1/accounts/{address}/mailbox-encryption/recover
//             {"passphrase": the domain's, "password": the new one}: the key
//             opened with the domain's recovery key and wrapped by a password
//             the administrator sets - the person forgot theirs, and their mail
//             is reachable again. The password goes through the policy, the
//             reuse history and the breach check as every password does.
//   GET    /api/v1/domains/{domain}/mailbox-recovery
//   PUT    /api/v1/domains/{domain}/mailbox-recovery
//             {"passphrase", "hint", "current_passphrase"}: made, or replaced -
//             which needs the current passphrase and seals every account's key
//             of the domain to the new one before the old one goes.
//   DELETE /api/v1/domains/{domain}/mailbox-recovery
//             refused while any account of the domain has a key.
//   GET    /api/v1/me/mailbox-encryption
//             the account's own view: enabled, keyed, open.
//
// Nothing here answers a key, wrapped or not: the private halves never leave
// the server, and the passphrase is never stored. The writes are recorded on
// the audit trail as events, since the two tables are not audited row by row
// (their rows are key material, and a sign-in writes them with no actor).
#include "StdAfx.h"

#include "RestApiServer.h"
#include "HttpServer.h"
#include "JsonDocument.h"
#include "MailboxKeys.h"
#include "SignInStore.h"
#include "AuditTrail.h"
#include "AccountLockout.h"
#include "Crypt.h"
#include "PasswordPolicy.h"
#include "PasswordHistory.h"
#include "PasswordBreachCheck.h"
#include "Time.h"
#include "Unicode.h"
#include "../Application/IniFileSettings.h"
#include "../Application/RoleManager.h"
#include "../BO/Account.h"
#include "../BO/Domain.h"
#include "../Persistence/PersistentAccount.h"
#include "../Persistence/PersistentDomain.h"

#include <string>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      std::shared_ptr<Account> KeyedAccount(const AnsiString &address)
      {
         std::shared_ptr<Account> account = std::shared_ptr<Account>(new Account());
         if (!PersistentAccount::ReadObject(account, String(address)) || account->GetID() == 0)
            return std::shared_ptr<Account>();
         return account;
      }

      // The body as an object, every member a string or a boolean the route
      // names, anything else refused by name.
      bool ReadBody(const AnsiString &requestBody, JsonValue &body, AnsiString &problem)
      {
         AnsiString trimmed = requestBody;
         trimmed.Trim();
         std::string parseError;
         if (trimmed.IsEmpty() || !JsonValue::Parse(std::string(trimmed.c_str(), (size_t) trimmed.GetLength()), body, parseError) || !body.IsObject())
         {
            problem = "the body must be a JSON object";
            return false;
         }
         return true;
      }

      bool OnlyMembers(const JsonValue &body, const char *const *names, size_t count, AnsiString &problem)
      {
         for (const std::pair<std::string, JsonValue> &member : body.Members())
         {
            bool known = false;
            for (size_t i = 0; i < count && !known; i++)
               known = member.first == names[i];

            if (!known)
            {
               problem = AnsiString("unknown field: ") + AnsiString(member.first.substr(0, 48).c_str());
               return false;
            }
         }
         return true;
      }

      // A string member as its UTF-8 bytes; false when it is there and not a string.
      bool StringMember(const JsonValue &body, const char *name, AnsiString &value)
      {
         value = AnsiString();
         const JsonValue *member = body.Get(name);
         if (!member)
            return true;
         if (!member->IsString())
            return false;
         value = AnsiString(member->AsString().c_str());
         return true;
      }
   }

   HttpResponse
   RestApiServer::HandleMeMailboxEncryption_(const Caller &caller)
   {
      if (!caller.account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      return BuildResponse_(200, MailboxKeys::AccountJson(caller.account, true));
   }

   HttpResponse
   RestApiServer::HandleMailboxEncryption_(const Caller &caller, const Route &route, const AnsiString &requestBody)
   {
      // ---- a domain's recovery key -------------------------------------------------
      if (route.kind == RouteDomainMailboxRecoveryGet || route.kind == RouteDomainMailboxRecoveryPut || route.kind == RouteDomainMailboxRecoveryDelete)
      {
         std::shared_ptr<Domain> domain = std::shared_ptr<Domain>(new Domain());
         if (!PersistentDomain::ReadObject(domain, String(route.identifier)) || domain->GetID() == 0)
            return BuildResponse_(404, "{\"error\":\"no such domain\"}");

         if (route.kind == RouteDomainMailboxRecoveryGet)
            return BuildResponse_(200, MailboxKeys::RecoveryJson(domain->GetID()));

         AnsiString problem;
         int status = 500;

         if (route.kind == RouteDomainMailboxRecoveryDelete)
         {
            if (!MailboxKeys::RemoveRecovery(domain->GetID(), problem, status))
               return BuildResponse_(status, "{\"error\":\"" + JsonEscape_(problem) + "\"}");

            AuditTrail::Instance()->RecordEvent(_T("domain"), domain->GetName(), _T("mailbox-recovery-key-removed"),
               _T("the domain's recovery key for mailbox encryption at rest was removed; no account of the domain had a key sealed to it"));
            LOG_APPLICATION("RestApi: the mailbox recovery key of " + domain->GetName() + " was removed by " + String(caller.identity) + ".");
            return BuildResponse_(200, "{\"removed\":true}");
         }

         JsonValue body;
         if (!ReadBody(requestBody, body, problem))
            return BuildResponse_(400, "{\"error\":\"" + JsonEscape_(problem) + "\"}");

         static const char *const names[] = { "passphrase", "hint", "current_passphrase" };
         if (!OnlyMembers(body, names, sizeof(names) / sizeof(names[0]), problem))
            return BuildResponse_(400, "{\"error\":\"" + JsonEscape_(problem) + "\"}");

         AnsiString passphrase;
         AnsiString hintText;
         AnsiString current;
         if (!StringMember(body, "passphrase", passphrase) || !StringMember(body, "hint", hintText) || !StringMember(body, "current_passphrase", current))
            return BuildResponse_(400, "{\"error\":\"passphrase, hint and current_passphrase are strings\"}");

         if (passphrase.IsEmpty())
            return BuildResponse_(400, "{\"error\":\"passphrase is required: the secret the domain's recovery key is wrapped by, which this server never stores\"}");

         String hint;
         Unicode::MultiByteToWide(hintText, hint);

         MailboxKeys::Recovery before;
         const bool existed = MailboxKeys::GetRecovery(domain->GetID(), before);

         int resealed = 0;
         if (!MailboxKeys::SetRecovery(domain->GetID(), passphrase, hint, current, problem, status, resealed))
         {
            // A wrong current passphrase is a guess at a secret, and feeds the
            // per-address auto-ban as a wrong password does.
            if (status == 403)
               RegisterAuthenticationFailure_(caller.peer);
            return BuildResponse_(status, "{\"error\":\"" + JsonEscape_(problem) + "\"}");
         }

         String detail;
         detail.Format(_T("the domain's recovery key for mailbox encryption at rest was %s; %d account key(s) sealed to it"),
            existed ? _T("replaced") : _T("made"), resealed);
         AuditTrail::Instance()->RecordEvent(_T("domain"), domain->GetName(), existed ? _T("mailbox-recovery-key-replaced") : _T("mailbox-recovery-key-made"), detail);
         LOG_APPLICATION("RestApi: " + detail + " (" + domain->GetName() + ", by " + String(caller.identity) + ").");

         return BuildResponse_(status, MailboxKeys::RecoveryJson(domain->GetID()));
      }

      // ---- an account's key --------------------------------------------------------
      std::shared_ptr<Account> account = KeyedAccount(route.identifier);
      if (!account)
         return BuildResponse_(404, "{\"error\":\"no such account\"}");

      if (route.kind == RouteAccountMailboxEncryptionGet)
         return BuildResponse_(200, MailboxKeys::AccountJson(account, false));

      // Every write here changes how the account signs in and whether its mail
      // can be read - the switch refuses its app passwords, passkeys and
      // single sign-on, the forfeit makes its encrypted mail unreadable, the
      // recovery sets its password - which is taking the account over, as a
      // password reset is: the rule HandleAccountPasswordReset_ and the account
      // update apply, for the same reason. A domain administrator does not
      // switch encryption on for a server administrator's mailbox.
      if (!RoleManager::Encompasses(caller.grant, RoleManager::GrantForAccount(account)))
         return BuildResponse_(403, "{\"error\":\"that account holds authority this credential does not, so it is not this credential's to change\"}");

      AnsiString problem;
      int status = 500;

      if (route.kind == RouteAccountMailboxEncryptionPut)
      {
         JsonValue body;
         if (!ReadBody(requestBody, body, problem))
            return BuildResponse_(400, "{\"error\":\"" + JsonEscape_(problem) + "\"}");

         static const char *const names[] = { "enabled" };
         if (!OnlyMembers(body, names, sizeof(names) / sizeof(names[0]), problem))
            return BuildResponse_(400, "{\"error\":\"" + JsonEscape_(problem) + "\"}");

         const JsonValue *enabled = body.Get("enabled");
         if (!enabled || !enabled->IsBool())
            return BuildResponse_(400, "{\"error\":\"enabled is required, true or false\"}");

         const bool on = enabled->AsBool();
         if (!(on ? MailboxKeys::Enable(account, problem, status) : MailboxKeys::Disable(account, problem, status)))
            return BuildResponse_(status, "{\"error\":\"" + JsonEscape_(problem) + "\",\"code\":\"mailbox_key\"}");

         // A new key ends every session the account had - its webmail sessions
         // and its IMAP and POP3 connections, as SCIM's deactivation ends them:
         // each was signed in before the mailbox was encrypted, by whatever
         // credential it was - an app password, a token, a passkey, another
         // account signing in as it - that could never have opened it, and the
         // switch-on left the key open. A new one is made only on the password.
         // The sessions an IMAP or POP3 connection could not be registered for
         // are ended at their next command (IMAPConnection, POP3Connection),
         // and a webmail session the revoke missed at its next request, which
         // finds it made under no opening of the key (RestApiServer).
         if (on && status == 201)
         {
            RevokeSessionsForAccount_(account->GetID(), AnsiString());
            SignInStore::DropConnectionsForAccount(account->GetID());
         }

         AuditTrail::Instance()->RecordEvent(_T("account"), account->GetAddress(), on ? _T("mailbox-encryption-on") : _T("mailbox-encryption-off"),
            on ? _T("mail stored in the mailbox from now on is encrypted to its key") : _T("new mail is stored as it comes; what is encrypted stays encrypted, and the key stays to read it"));

         return BuildResponse_(status, MailboxKeys::AccountJson(account, false));
      }

      if (route.kind == RouteAccountMailboxEncryptionForfeit)
      {
         if (QueryParameter_(route.query, "confirm") != "forfeit")
            return BuildResponse_(400, "{\"error\":\"removing a mailbox key makes every message still encrypted to it unreadable, by anybody, for ever: add ?confirm=forfeit to say that is meant\"}");

         if (!MailboxKeys::Forfeit(account, problem, status))
            return BuildResponse_(status, "{\"error\":\"" + JsonEscape_(problem) + "\"}");

         AuditTrail::Instance()->RecordEvent(_T("account"), account->GetAddress(), _T("mailbox-key-forfeited"),
            _T("the mailbox key was removed; any message still encrypted to it can no longer be read"));

         return BuildResponse_(200, "{\"forfeited\":true}");
      }

      if (route.kind == RouteAccountMailboxEncryptionRecover)
      {
         // Setting the password is taking the account over, as a reset is;
         // asked above, for every write.
         JsonValue body;
         if (!ReadBody(requestBody, body, problem))
            return BuildResponse_(400, "{\"error\":\"" + JsonEscape_(problem) + "\"}");

         static const char *const names[] = { "passphrase", "password" };
         if (!OnlyMembers(body, names, sizeof(names) / sizeof(names[0]), problem))
            return BuildResponse_(400, "{\"error\":\"" + JsonEscape_(problem) + "\"}");

         AnsiString passphrase;
         AnsiString passwordText;
         if (!StringMember(body, "passphrase", passphrase) || !StringMember(body, "password", passwordText) || passphrase.IsEmpty() || passwordText.IsEmpty())
            return BuildResponse_(400, "{\"error\":\"passphrase (the domain's recovery passphrase) and password (the account's new one) are required\"}");

         const MailboxKeys::Keyed keyed = MailboxKeys::KeyedState(account->GetID());
         if (keyed == MailboxKeys::KeyedUnknown)
            return BuildResponse_(503, "{\"error\":\"whether the account has a mailbox key could not be read just now; try again in a moment\",\"code\":\"mailbox_key\"}");
         if (keyed == MailboxKeys::KeyedDamaged)
            return BuildResponse_(409, "{\"error\":\"" + JsonEscape_(Utf8_(MailboxKeys::DamagedSentence())) + "\",\"code\":\"mailbox_key\"}");
         if (keyed == MailboxKeys::KeyedNo)
            return BuildResponse_(404, "{\"error\":\"the account has no mailbox key\"}");

         String password;
         Unicode::MultiByteToWide(passwordText, password);

         // The password first, as every password is judged: a refusal here
         // opens nothing and records nothing.
         String policyFailure;
         if (!PasswordPolicy::IsAcceptable(account->GetAddress(), password, policyFailure))
            return BuildResponse_(400, "{\"error\":\"" + JsonEscape_(Utf8_(policyFailure)) + "\"}");

         if (PasswordHistory::IsReuse(account, password))
            return BuildResponse_(409, "{\"error\":\"This password has been used recently on this account. Choose one that has not.\"}");

         {
            String breachRefusal;
            AnsiString breachReason;
            int breachStatus = 0;
            if (!PasswordBreachCheck::IsAcceptable(account->GetAddress(), password, breachRefusal, breachReason, breachStatus))
            {
               AnsiString answer;
               answer.Format("{\"error\":\"%s\",\"reason\":\"%s\"}", JsonEscape_(Utf8_(breachRefusal)).c_str(), breachReason.c_str());
               return BuildResponse_(breachStatus, answer);
            }
         }

         // The key opened with the domain's passphrase, and left open for the
         // password to wrap.
         if (!MailboxKeys::OpenWithRecovery(account, passphrase, problem, status))
         {
            if (status == 403)
               RegisterAuthenticationFailure_(caller.peer);
            return BuildResponse_(status, "{\"error\":\"" + JsonEscape_(problem) + "\",\"code\":\"mailbox_key\"}");
         }

         String keyRefusal;
         if (!MailboxKeys::PrepareForNewPassword(account, password, keyRefusal))
            return BuildResponse_(MailboxKeys::RefusalStatus(keyRefusal, 500), "{\"error\":\"" + JsonEscape_(Utf8_(keyRefusal)) + "\"}");

         PasswordHistory::Record(account);

         const int preferredHashAlgorithm = IniFileSettings::Instance()->GetPreferredHashAlgorithm();
         account->SetPassword(Crypt::Instance()->EnCrypt(password, (Crypt::EncryptionType) preferredHashAlgorithm));
         account->SetPasswordEncryption(preferredHashAlgorithm);
         account->SetPasswordChanged(Time::GetCurrentDateTime());

         // The wrap is stored by the save, once the password is (MailboxKeys::
         // StorePreparedWrap).
         if (!PersistentAccount::SaveObject(account))
            return BuildResponse_(500, "{\"error\":\"the account could not be saved; the key stays wrapped as it was\"}");

         AccountLockout::Instance()->RecordSuccess(account->GetAddress());

         AuditTrail::Instance()->RecordEvent(_T("account"), account->GetAddress(), _T("mailbox-key-recovered"),
            _T("the mailbox key was opened with the domain's recovery key and wrapped by a new password the administrator set"));
         LOG_APPLICATION("RestApi: the mailbox key of " + account->GetAddress() + " was recovered with its domain's recovery key, and a new password set, by " + String(caller.identity) + ".");

         return BuildResponse_(200, "{\"recovered\":true}");
      }

      return BuildResponse_(404, "{\"error\":\"not found\"}");
   }

   AnsiString
   RestApiServer::OpenApiMailboxEncryptionPaths_()
   {
      static const char *paths =
         ",\"/api/v1/accounts/{address}/mailbox-encryption\":{"
         "\"get\":{\"summary\":\"Whether an account's mailbox is encrypted at rest\",\"description\":\"{enabled, keyed, open, damaged, pending, fingerprint, created, changed, zero_access, domain_recovery, recovery_current}. keyed is whether the account has a mailbox key at all - so that messages encrypted to it may be in its folders - and enabled whether new mail is encrypted to it; damaged whether its row holds no key, in which case nothing is stored in the mailbox or read from it until the forfeit removes the row; open whether the key is open in the server's memory now (a sign-in with the account's password opens it, and so does the switch-on; it closes mailbox_key_idle_minutes after its owner's last use once no IMAP or POP3 session holds it); pending whether the account has not yet signed in with its password since the key was made, in which case the key is held under this machine's own key until it does, and whoever has that key with the database can open it; fingerprint the first 16 hex digits of the SHA-256 of its public half; recovery_current whether the domain's passphrase opens its recovery copy - sealed to the domain's recovery key, or to the one a replacement under way keeps beside it. Never a key. docs/EncryptionAtRest.md.\",\"responses\":{\"200\":{\"description\":\"The state\"},\"404\":{\"description\":\"No such account\"}}},"
         "\"put\":{\"summary\":\"Switch encryption at rest on or off for a mailbox\",\"description\":\"Body: {enabled}. On: the account gets an X25519 mailbox key - its private half sealed to the domain's recovery key and held under this machine's key until the account's first sign-in with its password wraps it - and from now on every message stored in its folders (delivered, appended, filed by a rule or the IMAP mirror, imported, a draft, a message put in over REST or COM, a copy from another mailbox, a restored or recovered one) is encrypted to it, compressed first when message_store_compression is on, with no part held once. The key is left open, as a sign-in would leave it, and a new key ends every session the account had - its webmail sessions and its IMAP and POP3 connections - since each was signed in before the mailbox was encrypted. Refused (409, code mailbox_key) until the domain has a recovery key (PUT /api/v1/domains/{domain}/mailbox-recovery), for a zero-access mailbox, whose mail is already encrypted end to end, for an account that signs in against a Windows directory, and for one with a second factor (TOTP) enrolled, whose mail clients sign in only with app passwords. Refused (403) for an account that holds authority this credential does not. hmailserver --encrypt-store --account encrypts what is already there, with the server stopped. Off: new mail is stored as it comes; what is encrypted stays encrypted, and the key stays to read it. From the moment a mailbox has a key, IMAP and POP3 sign it in only with its own password (an app password, a token, a certificate, a ticket or the master user is refused with a sentence), no /api/v1/me route answers an app password, it is not moved to another domain, and the full-text index holds none of its words. Recorded on the audit trail.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"enabled\"],\"properties\":{\"enabled\":{\"type\":\"boolean\"}}}}}},\"responses\":{\"200\":{\"description\":\"The state\"},\"201\":{\"description\":\"A key was made; the state\"},\"403\":{\"description\":\"The account holds authority this credential does not\"},\"409\":{\"description\":\"No recovery key in the domain, a zero-access mailbox, a directory-linked account, a second factor enrolled, a key row that holds no key (the forfeit removes it), or a key made a moment ago by another request\"}}},"
         "\"delete\":{\"summary\":\"Remove a mailbox's key, and every message still encrypted to it with it\",\"description\":\"Requires ?confirm=forfeit. The key is removed from the database and from memory, and any message still encrypted to it can never be read again, by anybody: what an administrator does when the password and the domain's recovery passphrase are both lost, or after hmailserver --decrypt-store has put every message back in the clear. Refused while a legal hold covers the account (docs/Discovery.md), since the key is the only way to its encrypted mail, and for an account that holds authority this credential does not. Recorded on the audit trail.\",\"responses\":{\"200\":{\"description\":\"{forfeited: true}\"},\"400\":{\"description\":\"No confirmation\"},\"403\":{\"description\":\"The account holds authority this credential does not\"},\"404\":{\"description\":\"The account has no key\"},\"409\":{\"description\":\"A legal hold covers the account\"}}}},"
         "\"/api/v1/accounts/{address}/mailbox-encryption/recover\":{\"post\":{\"summary\":\"Open a mailbox's key with its domain's recovery key, and set a new password that wraps it\",\"description\":\"Body: {passphrase, password}. The person forgot their password: the domain's recovery key is opened with the administrator's passphrase, the account's key with it, and the key is wrapped by the new password, which becomes the account's - through the password policy, the reuse history and the breach check as every password does. The only way an encrypted mailbox's password can be set while its key is closed: every other way of setting one (this API's account update and reset, the Control Panel, COM) is refused then with a sentence naming this route, because a password set then would open nothing. 403 (code mailbox_key) when the passphrase does not open the recovery key, which also counts towards the auto-ban; 409 when the key is sealed to a recovery key the domain does not have. Recorded on the audit trail.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"passphrase\",\"password\"],\"properties\":{\"passphrase\":{\"type\":\"string\"},\"password\":{\"type\":\"string\"}}}}}},\"responses\":{\"200\":{\"description\":\"{recovered: true}\"},\"400\":{\"description\":\"A field missing, or a password the policy refuses\"},\"403\":{\"description\":\"The passphrase does not open the recovery key, or the account holds authority this credential does not\"},\"404\":{\"description\":\"The account has no key\"},\"409\":{\"description\":\"A password used recently, a key sealed to a recovery key the domain does not have, or a key row that holds no key\"},\"503\":{\"description\":\"Whether the account has a key, or its key's row, could not be read just now; try again\"}}}},"
         "\"/api/v1/domains/{domain}/mailbox-recovery\":{"
         "\"get\":{\"summary\":\"A domain's recovery key for mailbox encryption at rest\",\"description\":\"{exists, fingerprint, hint, created, accounts, replacement_unfinished}: whether the domain has one, its fingerprint, the hint its maker left for the passphrase, when it was made, how many of the domain's accounts have a key sealed to it, and whether the last replacement was cut short - its accounts still open with the current passphrase, and the next replacement finishes it. Needs view_accounts. Never the key.\",\"responses\":{\"200\":{\"description\":\"The state\"},\"404\":{\"description\":\"No such domain\"}}},"
         "\"put\":{\"summary\":\"Make or replace a domain's recovery key\",\"description\":\"Body: {passphrase, hint, current_passphrase}. An X25519 key pair whose private half is wrapped by the passphrase (at least 12 characters) with Argon2id and stored only so: the passphrase is never stored, and without it the recovery key opens nothing. It is what an account's mailbox key is sealed to when its encryption is switched on, and what POST /api/v1/accounts/{address}/mailbox-encryption/recover opens. Replacing it needs current_passphrase, and seals every account's key of the domain to the new one: each is opened and sealed in memory first, and one that cannot be changes nothing; then the new key is written with the old one beside it, both wrapped by the new passphrase, then each account, then the old key goes - a replacement cut short loses nothing, and the next one, given the new passphrase as the current one, finishes it. Keep the passphrase somewhere this server is not: a domain whose passphrase is lost has no recovery. Recorded on the audit trail.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"passphrase\"],\"properties\":{\"passphrase\":{\"type\":\"string\"},\"hint\":{\"type\":\"string\"},\"current_passphrase\":{\"type\":\"string\"}}}}}},\"responses\":{\"200\":{\"description\":\"Replaced; the state\"},\"201\":{\"description\":\"Made; the state\"},\"400\":{\"description\":\"A passphrase shorter than 12 characters, or a hint longer than 255\"},\"403\":{\"description\":\"current_passphrase does not open the key there now\"},\"409\":{\"description\":\"A key is there and no current_passphrase was given, or an account's key could not be sealed again\"}}},"
         "\"delete\":{\"summary\":\"Remove a domain's recovery key\",\"description\":\"Refused (409) while any account of the domain has a mailbox key, since every one of them is sealed to it. Recorded on the audit trail.\",\"responses\":{\"200\":{\"description\":\"{removed: true}\"},\"404\":{\"description\":\"No recovery key\"},\"409\":{\"description\":\"Accounts of the domain have keys\"}}}},"
         "\"/api/v1/me/mailbox-encryption\":{\"get\":{\"summary\":\"Whether the signed-in account's mailbox is encrypted at rest\",\"description\":\"{enabled, keyed, open}. Self-service and read-only: the administrator switches it (docs/EncryptionAtRest.md), and a session of a keyed mailbox is refused, or ends, whenever its key is closed.\",\"responses\":{\"200\":{\"description\":\"The state\"}}}}";

      return AnsiString(paths);
   }
}
