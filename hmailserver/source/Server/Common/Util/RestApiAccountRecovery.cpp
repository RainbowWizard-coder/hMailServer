// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The recovery address and the recovery codes (AccountRecovery.h), on the wire.
//
// The account's own, behind its own credential:
//
//    GET    /api/v1/me/recovery           the address (masked), whether it is
//                                         proved, how many codes are unused
//    PUT    /api/v1/me/recovery           set or change it - the account's own
//                                         password, and its second factor
//    DELETE /api/v1/me/recovery           remove it
//    POST   /api/v1/me/recovery/resend    the verification message again
//    POST   /api/v1/me/recovery/codes     ten fresh codes, shown once
//
// The administrator's, on the account:
//
//    DELETE /api/v1/accounts/{address}/recovery         the address
//    DELETE /api/v1/accounts/{address}/recovery/codes   the code set
//
// and GET /api/v1/accounts/{address} carries recovery_address (masked),
// recovery_address_set, recovery_address_verified and recovery_codes_unused.
//
// Two acts and not one, because they answer two different reports: "that
// address is not mine any more" and "somebody else has my code card". A code
// stands in for the account's second factor, so a leaked set is a standing way
// past it, and the account holder's own replacement from the webmail is exactly
// what somebody locked out of the webmail cannot reach.
//
// And three that take no credentials at all, because the person using them has
// none they can present:
//
//    GET/POST /recovery/verify?token=     the page a verification link opens
//    POST     /api/v1/password-reset      "I have forgotten my password"
//    GET/POST /portal/reset?token=        the page a reset link opens
//    POST     /api/v1/password-reset/complete
//
// WHAT THE UNAUTHENTICATED HALF IS CAREFUL ABOUT
//
// It never says whether an address exists. POST /api/v1/password-reset answers
// 202 and the same sentence for a mailbox that is not there, for one with no
// recovery address, for one whose address has not been proved and for one where
// a link has just gone out. The rate limits are charged before anything is
// looked up and for every address alike, so neither the answer nor the time it
// takes is an address directory.
//
// A link opens a PAGE with a button rather than acting on a GET. Mail security
// products follow every link in a message before the reader sees it, and a GET
// that confirmed an address - or spent a reset link - would be spent by the
// scanner rather than by the person.

#include "StdAfx.h"
#include "RestApiServer.h"
#include "HttpServer.h"
#include "AccountRecovery.h"
#include "AuditTrail.h"
#include "Totp.h"
#include "Unicode.h"
#include "../Application/Logger.h"
#include "../BO/Account.h"
#include "../Cache/CacheContainer.h"
#include "../TCPIP/IPAddress.h"

#include <ctime>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // The one sentence every answer of the forgotten-password route gives,
      // whatever really happened. Kept in one place so that a later hand cannot
      // make one branch of it more informative than another.
      const char *SameAnswer =
         "If that address belongs to a mailbox here and it has a confirmed recovery address, a link to choose a new password is on its way there.";

      AnsiString Html(const String &value)
      {
         AnsiString bytes;
         Unicode::WideToMultiByte(value, bytes);
         AnsiString out;
         for (int i = 0; i < bytes.GetLength(); i++)
         {
            const char c = bytes[i];
            if (c == '&') out += "&amp;";
            else if (c == '<') out += "&lt;";
            else if (c == '>') out += "&gt;";
            else if (c == '"') out += "&quot;";
            else if (c == '\'') out += "&#39;";
            else out += c;
         }
         return out;
      }

      String Wide(const AnsiString &value)
      {
         String out;
         Unicode::MultiByteToWide(value, out);
         return out;
      }

      // The same shell the confidential reading page uses: no script at all, no
      // remote anything, forms only back to here, nothing to index and nothing
      // to cache. A page that asks for a new password must be the plainest
      // thing this server serves.
      HttpResponse Page(int status, const AnsiString &title, const AnsiString &inner)
      {
         HttpResponse response;
         response.status = status;
         response.content_type = "text/html; charset=utf-8";
         response.body =
            "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><title>" + title + "</title>"
            "<style>"
            ":root{--bg:#0f1419;--card:#161b22;--line:#30363d;--text:#e6edf3;--muted:#9aa4ae;--accent:#36c2ff;--on:#04121c;--err:#ff7b72;--ok:#3fb950}"
            "@media (prefers-color-scheme: light){:root{--bg:#f6f8fa;--card:#ffffff;--line:#d0d7de;--text:#1f2328;--muted:#57606a;--accent:#0969da;--on:#ffffff;--err:#cf222e;--ok:#1a7f37}}"
            "body{font-family:system-ui,sans-serif;background:var(--bg);color:var(--text);display:flex;justify-content:center;padding:32px 16px;margin:0}"
            "main{max-width:520px;width:100%;box-sizing:border-box;background:var(--card);border:1px solid var(--line);border-radius:12px;padding:24px}"
            "h1{font-size:20px;margin:0 0 12px;overflow-wrap:anywhere}p{margin:8px 0;color:var(--muted);font-size:14px;overflow-wrap:anywhere}"
            "label{display:block;font-size:14px;color:var(--muted);margin-top:12px}"
            "input{width:100%;box-sizing:border-box;padding:10px;border-radius:8px;border:1px solid var(--line);background:var(--bg);color:var(--text);font-size:15px;margin:6px 0}"
            "button{padding:10px 16px;border-radius:8px;border:0;background:var(--accent);color:var(--on);font-weight:600;font-size:15px;cursor:pointer;margin-top:10px}"
            ".err{color:var(--err)}.ok{color:var(--ok)}"
            "</style></head><body><main>" + inner + "</main></body></html>";

         response.extra_headers =
            "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; base-uri 'none'; frame-ancestors 'none'\r\n"
            "X-Frame-Options: DENY\r\n"
            "X-Content-Type-Options: nosniff\r\n"
            "Referrer-Policy: no-referrer\r\n"
            "X-Robots-Tag: noindex, noarchive\r\n"
            "Cache-Control: no-store\r\n";
         return response;
      }

      bool IsTokenText(const AnsiString &token)
      {
         if (token.GetLength() != 64)
            return false;
         for (int i = 0; i < token.GetLength(); i++)
         {
            const char c = token[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
               return false;
         }
         return true;
      }
   }

   AnsiString
   RestApiServer::OpenApiRecoveryPaths_()
   {
      static const char *paths =
         ",\"/api/v1/me/recovery\":{\"get\":{\"summary\":\"The signed-in account's recovery address and codes\",\"description\":\"{enabled,available,address,address_set,address_verified,updated,codes:{total,unused,created}}. address is masked - the first letter of the local part, the first letter of each domain label and the top-level label - and the full address is never answered to anybody, the account included, once it has been set. enabled is whether this server offers recovery at all (account_recovery_enabled); available is whether a link can be sent, which needs a public base URL. Seconds since 1970 throughout.\",\"responses\":{\"200\":{\"description\":\"The state\"}}},"
         "\"put\":{\"summary\":\"Set or change the recovery address\",\"description\":\"Body: address, password (the account's own, never an app password). An account with a second factor sends the code in X-hMailServer-OTP, or one of its recovery codes in X-hMailServer-Recovery-Code. The address is stored unverified and a confirmation link is sent to it; it counts for nothing until that link is followed, and changing it asks again. The account's own address is refused, and so is a request authenticated with an app password. The confirmation is charged against the same per-address budget a password-reset link is charged against, and 429 with retry_after says so.\",\"responses\":{\"200\":{\"description\":\"Set, and a confirmation link sent\"},\"400\":{\"description\":\"Not an address, or the account's own\"},\"403\":{\"description\":\"The password is not right, or recovery is off\"}}},"
         "\"delete\":{\"summary\":\"Remove the recovery address\",\"description\":\"The address and every link in flight for it go. The recovery codes are not touched; POST /api/v1/me/recovery/codes replaces those. Refused to a request authenticated with an app password, as every act that ends one of the account's credentials is.\",\"responses\":{\"200\":{\"description\":\"Removed\"}}}},"
         "\"/api/v1/me/recovery/resend\":{\"post\":{\"summary\":\"Send the confirmation link again\",\"description\":\"For the address already stored, when it has not been confirmed. The link that was in flight stops working. Charged against the same per-address budget a password-reset link is charged against - one message a minute and six an hour for the address it would go to - because a verification message is the same capability. Refused to a request authenticated with an app password.\",\"responses\":{\"200\":{\"description\":\"Sent\"},\"409\":{\"description\":\"There is no address, or it is confirmed already\"},\"429\":{\"description\":\"That address has had a message too recently; retry_after says when\"}}}},"
         "\"/api/v1/me/recovery/codes\":{\"post\":{\"summary\":\"Make ten one-time recovery codes\",\"description\":\"Body: password (the account's own), with the second factor as for the PUT above. Answers {codes:[...],created}: the only time they are shown. They are stored hashed with the scheme the account password uses, each works once, and making a set ends whatever set the account had. A code is presented in X-hMailServer-Recovery-Code in place of a one-time code - at POST /api/v1/session, at POST /api/v1/me/password and here - which is the way back in for somebody whose authenticator is gone.\",\"responses\":{\"201\":{\"description\":\"The ten codes\"},\"403\":{\"description\":\"The password is not right, or recovery is off\"}}}},"
         "\"/api/v1/accounts/{address}/recovery\":{\"delete\":{\"summary\":\"Clear an account's recovery address (administrator)\",\"description\":\"The address, and every link in flight for it, go; the person sets one again from the webmail. The recovery codes are not touched - DELETE /api/v1/accounts/{address}/recovery/codes is the act for those. What an administrator can read of the address is the masked form on GET /api/v1/accounts/{address} - never the address itself.\",\"responses\":{\"200\":{\"description\":\"Cleared\"},\"404\":{\"description\":\"No such account\"}}}},"
         "\"/api/v1/accounts/{address}/recovery/codes\":{\"delete\":{\"summary\":\"End an account's one-time recovery codes (administrator)\",\"description\":\"Every code the account holds goes, and the account holder makes a fresh set from the webmail. The remedy for a code card somebody else has seen: a code stands in for the account's second factor, so a leaked set is a standing way past it. The recovery address is not touched. Answers {codes_cleared}.\",\"responses\":{\"200\":{\"description\":\"Ended\"},\"404\":{\"description\":\"No such account\"}}}},"
         "\"/api/v1/password-reset\":{\"post\":{\"summary\":\"I have forgotten my password (no credentials)\",\"description\":\"Body: address. Always 202 with the same sentence, for a mailbox that is not here as much as for one that is: a refusal that differed would be an address directory. When the address names a mailbox whose recovery address has been confirmed, a single-use link that lives recovery_reset_link_minutes (30 by default) is sent THERE and nowhere else. One request a minute and six an hour per address and per client address.\",\"responses\":{\"202\":{\"description\":\"The same answer, whatever happened\"},\"503\":{\"description\":\"Recovery is off, or this server has no public address for a link\"}}}},"
         "\"/api/v1/password-reset/complete\":{\"post\":{\"summary\":\"Choose the new password (no credentials)\",\"description\":\"Body: token (from the link), password. The new password goes through the same policy, reuse history and breach check as POST /api/v1/me/password. A password one of those refuses leaves the link alive, so the person can choose again; the link is spent the moment a password is accepted, and never works twice. Every browser session of the account is ended, and the mailbox and the recovery address are both told the password changed. The account.password-reset alert is raised.\",\"responses\":{\"200\":{\"description\":\"Changed\"},\"400\":{\"description\":\"The password was refused; the sentence says why\"},\"410\":{\"description\":\"The link is used, expired, or for an address that is no longer the recovery address\"}}}},"
         "\"/recovery/verify\":{\"get\":{\"summary\":\"Confirm a recovery address (no credentials)\",\"description\":\"The page a confirmation link opens. The GET shows a button and confirms nothing - a mail scanner follows links, and a GET that confirmed would be followed by the scanner rather than by the person - and the POST is the confirmation. The link works once.\",\"responses\":{\"200\":{\"description\":\"The page\"},\"410\":{\"description\":\"The link is used or expired\"}}}},"
         "\"/portal/reset\":{\"get\":{\"summary\":\"Choose a new password (no credentials)\",\"description\":\"The page a reset link opens: a form for the new password, posted back here. The same refusals POST /api/v1/password-reset/complete gives, shown on the page.\",\"responses\":{\"200\":{\"description\":\"The form\"},\"410\":{\"description\":\"The link is used or expired\"}}}}";
      return AnsiString(paths);
   }

   // What a sign-in that spent a recovery code adds to its answer, and what
   // every other sign-in adds: nothing. A fragment rather than a field, so the
   // shape of the body a page has always read is unchanged when no code was
   // spent.
   AnsiString
   RestApiServer::RecoveryCodesLeftJson_(const Caller &caller)
   {
      if (caller.recovery_codes_left < 0)
         return AnsiString();

      AnsiString fragment;
      fragment.Format(",\"recovery_codes_left\":%d", caller.recovery_codes_left);
      return fragment;
   }

   // The refusal a route gives when the message it would have sent is one the
   // per-address budget will not have yet. A verification message is the same
   // capability as a reset link - the server posting mail to an address somebody
   // named - so the two share one counter, and this is the one place the refusal
   // is worded. Answered with retry_after and Retry-After, exactly as the
   // sender-address proof answers its own resend.
   HttpResponse
   RestApiServer::TooManyRecoveryMessages_(const String &problem, int retryAfter)
   {
      const int wait = retryAfter > 0 ? retryAfter : 60;

      AnsiString refusal;
      refusal.Format("{\"error\":\"%hs\",\"retry_after\":%d}", JsonEscape_(Utf8_(problem)).c_str(), wait);

      AnsiString header;
      header.Format("Retry-After: %d\r\n", wait);

      return BuildResponse_(429, refusal, header);
   }

   // The second factor a route asks for: the one-time code in X-hMailServer-OTP,
   // or - for somebody whose authenticator is gone - one of the account's
   // recovery codes in X-hMailServer-Recovery-Code. True for an account that has
   // no second factor at all.
   //
   // The code is MATCHED here and spent by SpendSecondFactor_, which the handler
   // calls once the act it was gating has actually happened. Everything this
   // proves can still be refused afterwards - a password the policy will not
   // have, a recovery address that is not an address - and the account must not
   // pay one of its ten codes for a refusal. Only the sign-in used to have that
   // property; now every route that takes a code has it.
   bool
   RestApiServer::SecondFactorProved_(std::shared_ptr<const Account> account, const AnsiString &request,
                                      __int64 *recoveryCodeId)
   {
      if (recoveryCodeId)
         *recoveryCodeId = 0;

      if (!account)
         return false;

      const String secret = account->GetTotpSecret();
      if (secret.IsEmpty())
         return true;

      const AnsiString code = GetHeader_(request, "x-hmailserver-otp");
      if (!code.IsEmpty() && Totp::VerifyCode(AnsiString(secret), code))
         return true;

      const AnsiString recovery = GetHeader_(request, "x-hmailserver-recovery-code");
      if (recovery.IsEmpty())
         return false;

      const __int64 matched = AccountRecovery::CheckCode(account->GetID(), Wide(recovery));
      if (matched == 0)
         return false;

      if (recoveryCodeId)
         *recoveryCodeId = matched;

      return true;
   }

   // The other half: the code that stood in for the one-time code, spent, now
   // that the act it proved has happened. Nothing else spends one.
   void
   RestApiServer::SpendSecondFactor_(std::shared_ptr<const Account> account, __int64 recoveryCodeId)
   {
      if (!account || recoveryCodeId == 0)
         return;

      if (!AccountRecovery::SpendCode(recoveryCodeId))
         return;

      AccountRecovery::CodeState codes;
      AccountRecovery::GetCodes(account->GetID(), codes);

      String left;
      left.Format(_T("%d"), codes.unused);
      LOG_APPLICATION("RestApi: " + account->GetAddress() + " used a recovery code in place of its one-time code; " +
         left + " of its recovery codes are left.");
   }

   HttpResponse
   RestApiServer::HandleMeRecovery_(const Caller &caller, RouteKind kind, const AnsiString &request)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      if (kind == RouteMeRecovery)
      {
         AccountRecovery::Address address;
         AccountRecovery::Get(account->GetID(), address);

         AccountRecovery::CodeState codes;
         AccountRecovery::GetCodes(account->GetID(), codes);

         AnsiString body;
         body.Format("{\"enabled\":%hs,\"available\":%hs,\"address_set\":%hs,\"address_verified\":%hs,"
                     "\"updated\":%I64d,\"codes\":{\"total\":%d,\"unused\":%d,\"created\":%I64d},",
            AccountRecovery::Enabled() ? "true" : "false",
            AccountRecovery::BaseUrl().IsEmpty() ? "false" : "true",
            address.exists ? "true" : "false",
            address.verified ? "true" : "false",
            address.updated,
            codes.total, codes.unused, codes.created);
         body += "\"address\":\"" + JsonEscape_(Utf8_(AccountRecovery::Mask(address.address))) + "\"}";
         return BuildResponse_(200, body);
      }

      if (!AccountRecovery::Enabled())
         return BuildResponse_(403, "{\"error\":\"this server does not offer a recovery address\"}");

      if (kind == RouteMeRecoveryPut)
      {
         // The account's own password, and its second factor, exactly as
         // minting an app password asks for them: setting the address that can
         // reset the password is at least as strong a thing to do.
         HttpResponse refusal;
         __int64 recoveryCodeId = 0;
         if (!ConfirmAccountPassword_(caller, request, refusal, "setting a recovery address", &recoveryCodeId))
            return refusal;

         const String wanted = JsonUtf8Value_(GetRequestBody_(request), "address");

         String problem;
         int retryAfter = 0;
         if (!AccountRecovery::Set(account, wanted, problem, &retryAfter))
         {
            if (retryAfter > 0)
               return TooManyRecoveryMessages_(problem, retryAfter);

            AnsiString body;
            body.Format("{\"error\":\"%hs\"}", JsonEscape_(Utf8_(problem)).c_str());
            return BuildResponse_(problem.Find(_T("could not")) >= 0 ? 500 : 400, body);
         }

         SpendSecondFactor_(account, recoveryCodeId);

         AuditTrail::Instance()->RecordEvent(_T("account"), account->GetAddress(), _T("recovery-address-set"),
            _T("A recovery address was set and a confirmation link sent to it: ") + AccountRecovery::Mask(wanted));

         LOG_APPLICATION("RestApi: " + account->GetAddress() + " set a recovery address from " +
            String(caller.peer.ToString()) + "; it is not in use until the link sent to it is followed.");

         return BuildResponse_(200, "{\"address_set\":true,\"address_verified\":false}");
      }

      if (kind == RouteMeRecoveryDelete)
      {
         // Removing the way back in is a credential-lifecycle act, and this
         // codebase refuses those to a request that was itself authenticated
         // with an app password - a credential deliberately handed to a mail
         // program, which has no business taking away the account's own way
         // back in. The same gate the session and device revocations use.
         HttpResponse refusal;
         if (RefuseAppPasswordCaller_(caller, "removing a recovery address", refusal))
            return refusal;

         AccountRecovery::Clear(account->GetID());

         AuditTrail::Instance()->RecordEvent(_T("account"), account->GetAddress(), _T("recovery-address-cleared"),
            _T("The account removed its own recovery address."));

         LOG_APPLICATION("RestApi: " + account->GetAddress() + " removed its recovery address.");
         return BuildResponse_(200, "{\"address_set\":false}");
      }

      if (kind == RouteMeRecoveryResend)
      {
         // The same gate, for the same reason and one more: this route makes the
         // server post mail to an address the account named, and an app password
         // that leaked would otherwise be a way to drive that at a third party.
         HttpResponse refusal;
         if (RefuseAppPasswordCaller_(caller, "sending a recovery confirmation", refusal))
            return refusal;

         AccountRecovery::Address address;
         if (!AccountRecovery::Get(account->GetID(), address) || !address.exists)
            return BuildResponse_(409, "{\"error\":\"there is no recovery address to confirm\"}");
         if (address.verified)
            return BuildResponse_(409, "{\"error\":\"that address is confirmed already\"}");

         String problem;
         int retryAfter = 0;
         if (!AccountRecovery::SendVerification(account, address, problem, &retryAfter))
         {
            // The same per-address budget the forgotten-password route is
            // charged against, and answered the way the sender-address proof
            // answers it: a message the server sends because somebody asked is
            // the same capability whichever route asked for it.
            if (retryAfter > 0)
               return TooManyRecoveryMessages_(problem, retryAfter);

            AnsiString body;
            body.Format("{\"error\":\"%hs\"}", JsonEscape_(Utf8_(problem)).c_str());
            return BuildResponse_(500, body);
         }

         return BuildResponse_(200, "{\"sent\":true}");
      }

      if (kind == RouteMeRecoveryCodes)
      {
         // Nothing to spend on this one route, and deliberately: a recovery code
         // presented here is one of the set Generate is about to replace, so it
         // is ended by the act itself. A generator that failed leaves the old
         // set whole - the code that proved the request among it - which is the
         // same property every other route now has, reached a different way.
         HttpResponse refusal;
         if (!ConfirmAccountPassword_(caller, request, refusal, "making recovery codes"))
            return refusal;

         std::vector<String> codes;
         if (!AccountRecovery::Generate(account->GetID(), codes) || codes.empty())
            return BuildResponse_(500, "{\"error\":\"the recovery codes could not be made; nothing has changed\"}");

         AuditTrail::Instance()->RecordEvent(_T("account"), account->GetAddress(), _T("recovery-codes-generated"),
            _T("A new set of one-time recovery codes was made; whatever set the account had before no longer works."));

         LOG_APPLICATION("RestApi: " + account->GetAddress() + " made a new set of recovery codes from " +
            String(caller.peer.ToString()) + ".");

         AnsiString body = "{\"codes\":[";
         for (size_t i = 0; i < codes.size(); i++)
            body += (i ? ",\"" : "\"") + JsonEscape_(Utf8_(codes[i])) + "\"";
         body += "]}";
         return BuildResponse_(201, body);
      }

      return BuildResponse_(404, "{\"error\":\"not found\"}");
   }

   HttpResponse
   RestApiServer::HandleAccountRecoveryClear_(const Caller &caller, const String &address)
   {
      std::shared_ptr<const Account> account = CacheContainer::Instance()->GetAccount(address);
      if (!account || account->GetID() == 0)
         return BuildResponse_(404, "{\"error\":\"account not found\"}");

      AccountRecovery::Clear(account->GetID());

      AuditTrail::Instance()->RecordEvent(_T("account"), account->GetAddress(), _T("recovery-address-cleared"),
         _T("An administrator removed the account's recovery address; every reset link in flight for it stopped working."));

      LOG_APPLICATION("RestApi: the recovery address of " + account->GetAddress() + " was cleared by " +
         String(caller.identity) + ".");

      return BuildResponse_(200, "{\"address_set\":false}");
   }

   // The other administrator's act: ending the code set. A code stands in for
   // the account's second factor, so a card somebody else has read is a standing
   // way past it - and until this existed the only way to end one was the
   // account holder making a fresh set from the webmail, which is precisely what
   // somebody locked out of the webmail cannot do.
   HttpResponse
   RestApiServer::HandleAccountRecoveryCodesClear_(const Caller &caller, const String &address)
   {
      std::shared_ptr<const Account> account = CacheContainer::Instance()->GetAccount(address);
      if (!account || account->GetID() == 0)
         return BuildResponse_(404, "{\"error\":\"account not found\"}");

      AccountRecovery::CodeState codes;
      AccountRecovery::GetCodes(account->GetID(), codes);

      AccountRecovery::ClearCodes(account->GetID());

      String went;
      went.Format(_T("%d"), codes.unused);
      AuditTrail::Instance()->RecordEvent(_T("account"), account->GetAddress(), _T("recovery-codes-cleared"),
         _T("An administrator removed the account's one-time recovery codes; ") + went +
         _T(" of them were still unused. The account holder makes a fresh set from the webmail."));

      LOG_APPLICATION("RestApi: the recovery codes of " + account->GetAddress() + " were cleared by " +
         String(caller.identity) + ".");

      AnsiString body;
      body.Format("{\"codes_cleared\":%d}", codes.total);
      return BuildResponse_(200, body);
   }

   // The page a confirmation link opens. The GET shows a button; the POST is
   // the confirmation, and spends the link.
   HttpResponse
   RestApiServer::HandleRecoveryVerifyPage_(const AnsiString &method, const AnsiString &query)
   {
      const AnsiString token = QueryParameter_(query, "token");

      if (!IsTokenText(token))
         return Page(404, "No such link", "<h1>No such link</h1><p>There is nothing at this address. Open the link from the message exactly as it was sent.</p>");

      if (method == "GET")
      {
         AnsiString inner = "<h1>Confirm this recovery address</h1>";
         inner += "<p>A mailbox on this server has named this address as the one to use if its password is ever forgotten. Press the button to confirm it.</p>";
         inner += "<form method=\"post\" action=\"/recovery/verify?token=" + token + "\">"
                  "<button type=\"submit\">Confirm this address</button></form>";
         inner += "<p>If you were not expecting this, close the page and do nothing: an address that is not confirmed is used for nothing.</p>";
         return Page(200, "Confirm this recovery address", inner);
      }

      String address;
      String problem;
      if (!AccountRecovery::VerifyAddress(token, address, problem))
         return Page(410, "This link does not work", "<h1>This link does not work</h1><p>" + Html(problem) + "</p>");

      AnsiString inner = "<h1 class=\"ok\">Confirmed</h1>";
      inner += "<p>This address is now the recovery address of that mailbox. If its password is forgotten, a link to choose a new one can be sent here.</p>";
      inner += "<p>Whoever can read this mailbox can reset that password. Keep it as safe as the mailbox it recovers.</p>";
      return Page(200, "Confirmed", inner);
   }

   HttpResponse
   RestApiServer::HandlePasswordResetRequest_(const AnsiString &requestBody, const IPAddress &peer_address)
   {
      AnsiString same;
      same.Format("{\"requested\":true,\"detail\":\"%hs\"}", SameAnswer);

      if (!AccountRecovery::Enabled() || AccountRecovery::BaseUrl().IsEmpty())
         return BuildResponse_(503, "{\"error\":\"this server does not offer a self-service password reset\"}");

      const String address = JsonUtf8Value_(requestBody, "address");

      const AccountRecovery::ResetOutcome outcome = AccountRecovery::BeginReset(address, peer_address);

      // Every outcome but "the server cannot do this at all" is the same answer.
      // The log below is the only place they differ, and it is the
      // administrator's, not the caller's.
      if (outcome == AccountRecovery::ResetUnavailable)
         return BuildResponse_(503, "{\"error\":\"this server does not offer a self-service password reset\"}");

      if (outcome == AccountRecovery::ResetTooSoon || outcome == AccountRecovery::ResetTooMany)
      {
         LOG_APPLICATION("RestApi: a password-reset request from " + String(peer_address.ToString()) +
            " was rate limited.");
      }

      if (outcome == AccountRecovery::ResetFailed)
      {
         LOG_APPLICATION("RestApi: a password-reset link could not be made or queued; the request was answered as every other is.");
      }

      return BuildResponse_(202, same);
   }

   HttpResponse
   RestApiServer::HandlePasswordResetComplete_(const AnsiString &requestBody, const IPAddress &peer_address)
   {
      const AnsiString token = GetJsonStringValue_(requestBody, "token");
      const String password = JsonUtf8Value_(requestBody, "password");

      String problem;
      int status = 400;
      String mailbox;

      const AccountRecovery::CompleteOutcome outcome =
         AccountRecovery::CompleteReset(token, password, problem, status, mailbox);

      if (outcome == AccountRecovery::CompleteDone)
         return BuildResponse_(200, "{\"changed\":true}");

      // A link that is not a link, used or expired: counted against the source
      // address as a failed logon is, so that guessing at sixty-four hex digits
      // trips the same short refusal as guessing at a password.
      if (outcome == AccountRecovery::CompleteBadToken)
         RegisterAuthenticationFailure_(peer_address);

      AnsiString body;
      body.Format("{\"error\":\"%hs\"}", JsonEscape_(Utf8_(problem)).c_str());
      return BuildResponse_(status, body);
   }

   // The page a reset link opens: the form, and the change.
   HttpResponse
   RestApiServer::HandlePasswordResetPage_(const AnsiString &method, const AnsiString &query,
                                           const AnsiString &requestBody, const IPAddress &peer_address)
   {
      const AnsiString token = QueryParameter_(query, "token");

      if (!IsTokenText(token))
         return Page(404, "No such link", "<h1>No such link</h1><p>There is nothing at this address. Open the link from the message exactly as it was sent.</p>");

      auto form = [&token](int status, const AnsiString &problem)
      {
         AnsiString inner = "<h1>Choose a new password</h1>";
         inner += "<p>This link came to the recovery address of a mailbox on this server. Choose a new password for it; the link works once.</p>";
         if (!problem.IsEmpty())
            inner += "<p class=\"err\">" + problem + "</p>";
         inner += "<form method=\"post\" action=\"/portal/reset?token=" + token + "\">"
                  "<label for=\"password\">New password</label>"
                  "<input id=\"password\" name=\"password\" type=\"password\" autocomplete=\"new-password\" required>"
                  "<button type=\"submit\">Set this password</button></form>";
         inner += "<p>Everything signed in to the mailbox in a browser is signed out when the password changes.</p>";
         return Page(status, "Choose a new password", inner);
      };

      if (method == "GET")
      {
         AccountRecovery::Token found;
         if (!AccountRecovery::FindToken(token, found) || found.kind != AccountRecovery::KindPasswordReset ||
             found.used != 0 || found.expires <= (__int64) time(nullptr))
         {
            return Page(410, "This link does not work",
               "<h1>This link does not work</h1><p>It has been used already, or it has expired. Ask for another from the sign-in page.</p>");
         }

         return form(200, AnsiString());
      }

      const String password = Wide(QueryParameter_(requestBody, "password"));

      String problem;
      int status = 400;
      String mailbox;

      const AccountRecovery::CompleteOutcome outcome =
         AccountRecovery::CompleteReset(token, password, problem, status, mailbox);

      if (outcome == AccountRecovery::CompleteDone)
      {
         AnsiString inner = "<h1 class=\"ok\">The password is changed</h1>";
         inner += "<p>Sign in to " + Html(mailbox) + " with the new password. Everything that was signed in to it in a browser has been signed out.</p>";
         inner += "<p>A note has gone to the mailbox and to its recovery address saying the password changed.</p>";
         return Page(200, "The password is changed", inner);
      }

      if (outcome == AccountRecovery::CompleteBadToken)
      {
         RegisterAuthenticationFailure_(peer_address);
         return Page(status, "This link does not work", "<h1>This link does not work</h1><p>" + Html(problem) + "</p>");
      }

      return form(status, Html(problem));
   }
}
