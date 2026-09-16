// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Renaming a mailbox and moving one to another domain, over REST.
//
//    GET    /api/v1/accounts/{address}/move   would a move be refused, and what
//                                             has this mailbox been through
//    POST   /api/v1/accounts/{address}/move   do it
//
// The whole of the operation is MailboxMove's; this file is the door. It
// decides two things the class cannot: that the caller may administer accounts
// in the domain the mailbox is GOING TO as well as the one it is leaving, and
// which refusal is which HTTP status.
//
// The second domain matters more than it looks. Authorize_ scopes an account
// route by the address in the PATH, which here is the address the mailbox is
// leaving; the address it is going to is in the body, where no generic
// authorisation can see it. Without the check below, a key issued for one
// domain could move somebody's whole mailbox into a domain it has no authority
// over - the identifier-in-the-body form of the identifier-in-the-path bypass.

#include "StdAfx.h"

#include "RestApiServer.h"

#include "../Application/Application.h"
#include "../BO/Account.h"
#include "../Persistence/MailboxMove.h"
#include "../Persistence/PersistentAccount.h"
#include "../Util/Parsing/StringParser.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      std::shared_ptr<Account> AccountAt(const AnsiString &address)
      {
         std::shared_ptr<Account> account = std::shared_ptr<Account>(new Account());

         if (!PersistentAccount::ReadObject(account, String(address)) || account->GetID() == 0)
            return std::shared_ptr<Account>();

         return account;
      }
   }

   HttpResponse
   RestApiServer::HandleMailboxMoveGet_(const AnsiString &address)
   {
      std::shared_ptr<Account> account = AccountAt(address);

      if (!account)
         return BuildResponse_(404, "{\"error\":\"no such account\"}");

      const String reason = MailboxMove::InUseReason(account);

      AnsiString body = "{\"address\":\"" + JsonEscape_(Utf8_(account->GetAddress())) + "\",";
      body += AnsiString("\"in_use\":") + (reason.IsEmpty() ? "false" : "true") + ",";
      body += "\"in_use_reason\":\"" + JsonEscape_(Utf8_(reason)) + "\",";

      MailboxMoveRecord newest;

      if (MailboxMove::Newest(account->GetID(), newest))
         body += AnsiString("\"last_move\":") + MailboxMove::RecordJson(newest) + "}";
      else
         body += "\"last_move\":null}";

      return BuildResponse_(200, body);
   }

   HttpResponse
   RestApiServer::HandleMailboxMove_(const Caller &caller, const AnsiString &address, const AnsiString &requestBody)
   {
      const AnsiString newAddress = GetJsonStringValue_(requestBody, "new_address");

      if (newAddress.IsEmpty())
         return BuildResponse_(400, "{\"error\":\"new_address is required\"}");

      // The default is to keep the old address, because that is what keeps mail
      // flowing; a caller says keep_alias false to not keep it.
      const bool keepAlias = GetJsonBoolValue_(requestBody, "keep_alias", true);

      const String targetDomain = StringParser::ExtractDomain(String(newAddress));

      if (targetDomain.IsEmpty())
         return BuildResponse_(400, "{\"error\":\"new_address must be a full address at a domain this server holds\"}");

      // See the note at the top: the domain in the body is judged here, because
      // Authorize_ has only seen the one in the path.
      if (!IsDomainAllowed_(caller.domains, targetDomain))
      {
         return BuildResponse_(403, "{\"error\":\"this credential may not administer accounts in the domain the mailbox would move to\"}");
      }

      std::shared_ptr<Account> account = AccountAt(address);

      if (!account)
         return BuildResponse_(404, "{\"error\":\"no such account\"}");

      const String previousAddress = account->GetAddress();

      MailboxMoveOutcome outcome;
      String errorMessage;

      if (!MailboxMove::Move(account, String(newAddress), keepAlias, outcome, errorMessage))
      {
         // 409 for the refusals that are about the state of the server rather
         // than the request - something already at the address, a mailbox
         // somebody is using, and a target domain that is full or would be over
         // its size - so that a client can tell "try again later" from "this
         // will never work".
         const bool conflict = !MailboxMove::InUseReason(account).IsEmpty() ||
                               errorMessage.Find(_T("already exists")) >= 0 ||
                               errorMessage.Find(_T("maximum number of accounts")) >= 0 ||
                               errorMessage.Find(_T("maximum size for the domain")) >= 0;

         return BuildResponse_(conflict ? 409 : 400,
                               "{\"error\":\"" + JsonEscape_(Utf8_(errorMessage)) + "\"}");
      }

      LOG_APPLICATION("RestApi: " + previousAddress + " was moved to " + outcome.new_address + " by " + String(caller.identity) + ".");

      return BuildResponse_(200, MailboxMove::OutcomeJson(outcome));
   }

   AnsiString
   RestApiServer::OpenApiMailboxMovePaths_()
   {
      return
         ",\"/api/v1/accounts/{address}/move\":{"
         "\"get\":{\"summary\":\"Whether this mailbox can be moved now, and what it has been through\",\"description\":\"Answers address, in_use, in_use_reason and last_move. in_use is true while something holds the mailbox and a move would be refused: a POP3 session holds it from authentication until its socket closes, an IMAP session while it has one of the account's folders selected. An SMTP delivery to the mailbox in flight is NOT counted - nothing records one - and hmailserver/docs/MailboxMove.md says what that costs. last_move is the newest journal row (hm_mailboxmoves) or null; a row whose complete is false is a move that was interrupted, and hmailserver/docs/MailboxMove.md says what each stage leaves behind. Needs view_accounts, and is scoped to the address's domain.\",\"responses\":{\"200\":{\"description\":\"address, in_use, in_use_reason, last_move\"},\"404\":{\"description\":\"No such account\"}}},"
         "\"post\":{\"summary\":\"Rename a mailbox, or move it to another domain on this server\",\"description\":\"Body: new_address (required; it carries the domain, so a rename and a move are one call) and keep_alias (default true, which leaves the old address behind as an alias to the new one). Everything keyed on the account follows it untouched - rules, folders, subscriptions, folder permissions, application passwords, second factor, contacts, calendars, preferences, known senders, password history, sender lists, group memberships and role. The message store and the Sieve tree are moved, and the aliases pointing at the account, its distribution list memberships, the recipients of mail still in the delivery queue and its quarantined messages are rewritten - which is how mail in flight to the old address reaches the moved mailbox even when the old address is not kept. The archive and the message trace are deliberately NOT rewritten: they record what happened at an address at a time. It either completes or leaves the account as it was, and it is one entry on the audit trail naming both addresses. Needs manage_accounts in the address's domain AND in the domain new_address names.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"new_address\"],\"properties\":{\"new_address\":{\"type\":\"string\"},\"keep_alias\":{\"type\":\"boolean\"}}}}}},\"responses\":{\"200\":{\"description\":\"old_address, new_address, old_domain, new_domain, account_id, alias_kept, store_moved, references_incomplete, messages, folders, aliases_repointed, list_memberships, queued_recipients, quarantine_entries, journal_id. Every count is what was written, so a rewrite that failed counts nothing: references_incomplete is what tells that zero from a zero that means there was nothing to rewrite.\"},\"400\":{\"description\":\"No new_address, an address that is not one, a domain this server does not hold or that is not active, or a store whose message file names are still full paths\"},\"403\":{\"description\":\"This credential may not administer accounts in the domain the mailbox would move to\"},\"404\":{\"description\":\"No such account\"},\"409\":{\"description\":\"An account, alias or distribution list is already at the new address, the mailbox is in use, or the domain being moved into is at its maximum number of accounts or would exceed its maximum size\"}}}}";
   }
}
