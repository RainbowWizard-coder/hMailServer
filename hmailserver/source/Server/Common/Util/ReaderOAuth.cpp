// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "ReaderOAuth.h"
#include "HttpsClient.h"
#include "JsonDocument.h"
#include "Unicode.h"
#include "../BO/FetchAccount.h"
#include "../Persistence/PersistentFetchAccount.h"
#include "../Application/Configuration.h"

#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <boost/thread/mutex.hpp>
#include <boost/thread/lock_guard.hpp>
#include <ctime>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // Gmail: https://mail.google.com/ is the one scope IMAP accepts, and
      // access_type=offline with prompt=consent is what makes Google hand out a
      // refresh token every time rather than only on the first consent.
      // Outlook.com and Microsoft 365: IMAP.AccessAsUser.All on the Outlook
      // resource, offline_access for the refresh token.
      const ReaderOAuth::Provider Providers[] =
      {
         { "google", "Gmail",
           PROPERTY_IMPORT_OAUTH_GOOGLE_CLIENT_ID, PROPERTY_IMPORT_OAUTH_GOOGLE_CLIENT_SECRET,
           PROPERTY_IMPORT_OAUTH_GOOGLE_AUTHORIZE_URL, PROPERTY_IMPORT_OAUTH_GOOGLE_TOKEN_URL,
           "https://mail.google.com/ openid email", "&access_type=offline&prompt=consent", true,
           "imap.gmail.com", 993 },
         { "microsoft", "Outlook.com",
           PROPERTY_IMPORT_OAUTH_MICROSOFT_CLIENT_ID, PROPERTY_IMPORT_OAUTH_MICROSOFT_CLIENT_SECRET,
           PROPERTY_IMPORT_OAUTH_MICROSOFT_AUTHORIZE_URL, PROPERTY_IMPORT_OAUTH_MICROSOFT_TOKEN_URL,
           "https://outlook.office.com/IMAP.AccessAsUser.All offline_access openid email", "&prompt=select_account", false,
           "outlook.office365.com", 993 }
      };

      const time_t PendingSeconds = 10 * 60;
      const size_t PendingMaximum = 1000;

      // Unfinished sign-ins one account, or one migration job, keeps at once. A
      // fourth drops its own oldest: someone starting sign-in after sign-in
      // loses only their own unfinished ones, never anybody else's.
      const size_t PendingPerOwner = 3;

      struct Pending
      {
         __int64 account_id = 0;
         __int64 migration_job_id = 0;   // a migration job's sign-in; account_id is then 0
         AnsiString migration_link;      // the digest of the link it was begun from
         AnsiString redeem_scope;        // the scope the code is redeemed for, when one must be named
         AnsiString provider;
         AnsiString verifier;
         AnsiString nonce;
         bool mirror = false;
         int days_to_keep = -1;
         time_t created = 0;
         unsigned __int64 order = 0;     // when it was admitted, among the others
      };

      struct CachedToken
      {
         String token;
         time_t refresh_at = 0;
      };

      boost::mutex stateLock;
      std::map<AnsiString, Pending> pending;
      unsigned __int64 pendingOrder = 0;
      std::map<__int64, CachedToken> tokens;

      void Expect(bool condition, const char *message)
      {
         if (!condition)
            throw std::logic_error(message);
      }

      String Setting(const TCHAR *name)
      {
         return Configuration::Instance()->GetImportOAuthString(name);
      }

      AnsiString Utf8(const String &text)
      {
         AnsiString bytes;
         Unicode::WideToMultiByte(text, bytes);
         return bytes;
      }

      String FromUtf8(const std::string &bytes)
      {
         String text;
         Unicode::MultiByteToWide(AnsiString(bytes.c_str()), text);
         return text;
      }

      // Random bytes as base64url: 32 bytes are 43 characters, which is inside
      // RFC 7636's 43 to 128 for a verifier and far past guessing for a state.
      bool RandomText(AnsiString &text)
      {
         unsigned char bytes[32];
         if (RAND_bytes(bytes, sizeof(bytes)) != 1)
            return false;
         text = ReaderOAuth::Base64Url(bytes, sizeof(bytes));
         return true;
      }

      bool SameText(const AnsiString &a, const AnsiString &b)
      {
         return a.GetLength() == b.GetLength() && a.GetLength() > 0 &&
                CRYPTO_memcmp(a.c_str(), b.c_str(), (size_t) a.GetLength()) == 0;
      }

      AnsiString Encoded(const AnsiString &value)
      {
         return HttpsClient::FormEncode(value);
      }

      // A form POSTed to a token endpoint, and its JSON answer. A 200 without an
      // access token, or anything else, is a refusal with the provider's own
      // error_description when it gave one. transient, when asked for, says
      // whether the refusal is the provider's trouble rather than the grant's:
      // no answer at all, a 429, a 5xx, or the two OAuth errors that mean "not
      // now" (RFC 6749 section 5.2: temporarily_unavailable, server_error).
      bool PostForm(const AnsiString &url, const AnsiString &form, JsonValue &answer, AnsiString &problem, bool *transient = nullptr)
      {
         if (transient)
            *transient = false;

         HttpsClient::Response response;
         String error;
         std::vector<AnsiString> headers;
         headers.push_back("Accept: application/json");

         if (!HttpsClient::Request("POST", url, headers, "application/x-www-form-urlencoded", form, response, error, 20, 256 * 1024))
         {
            problem = "the provider could not be reached: " + AnsiString(error);
            if (transient)
               *transient = true;
            return false;
         }

         const bool busy = response.status_code == 429 || response.status_code >= 500;

         std::string parseError;
         std::string body(response.body.c_str(), (size_t) response.body.GetLength());
         if (!JsonValue::Parse(body, answer, parseError) || !answer.IsObject())
         {
            problem.Format("the provider answered %d, and not with JSON", response.status_code);
            if (transient)
               *transient = busy;
            return false;
         }

         if (response.status_code != 200 || answer.GetString("access_token").empty())
         {
            const std::string code = answer.GetString("error");
            std::string reason = answer.GetString("error_description", answer.GetString("error", "no access token"));
            problem = AnsiString(("the provider refused: " + reason.substr(0, 240)).c_str());
            if (transient)
               *transient = busy || code == "temporarily_unavailable" || code == "server_error";
            return false;
         }

         return true;
      }

      void PruneLocked(time_t now)
      {
         for (auto it = pending.begin(); it != pending.end();)
         {
            if (now - it->second.created > PendingSeconds)
               it = pending.erase(it);
            else
               ++it;
         }
      }

      bool SameOwner(const Pending &one, const Pending &other)
      {
         if (one.migration_job_id > 0 || other.migration_job_id > 0)
            return one.migration_job_id == other.migration_job_id;
         return one.account_id == other.account_id;
      }

      // Keeps entry under state, making room first. A table that refused when
      // it was full let one reader, a thousand requests in under a minute,
      // switch every other reader's import and every migration job's sign-in
      // off for as long as they kept it up; so, as OidcSignIn::MakeRoomLocked
      // does, the oldest goes instead - the owner's own oldest past
      // PendingPerOwner, then the table's oldest past PendingMaximum.
      void AdmitLocked(const AnsiString &state, Pending entry)
      {
         PruneLocked(entry.created);

         for (;;)
         {
            size_t owned = 0;
            auto oldest = pending.end();
            for (auto it = pending.begin(); it != pending.end(); ++it)
            {
               if (!SameOwner(it->second, entry))
                  continue;
               owned++;
               if (oldest == pending.end() || it->second.order < oldest->second.order)
                  oldest = it;
            }
            if (owned < PendingPerOwner)
               break;
            pending.erase(oldest);
         }

         while (pending.size() >= PendingMaximum)
         {
            auto oldest = pending.begin();
            for (auto it = pending.begin(); it != pending.end(); ++it)
            {
               if (it->second.order < oldest->second.order)
                  oldest = it;
            }
            pending.erase(oldest);
         }

         entry.order = ++pendingOrder;
         pending[state] = entry;
      }
   }

   const ReaderOAuth::Provider *
   ReaderOAuth::Find(const AnsiString &word)
   {
      for (size_t i = 0; i < sizeof(Providers) / sizeof(Providers[0]); i++)
      {
         if (word == Providers[i].word)
            return &Providers[i];
      }
      return nullptr;
   }

   bool
   ReaderOAuth::Configured(const Provider &provider)
   {
      if (RedirectUri().IsEmpty())
         return false;
      if (Setting(provider.client_id_property).IsEmpty())
         return false;
      if (provider.secret_required && Setting(provider.client_secret_property).IsEmpty())
         return false;
      return !Setting(provider.authorize_url_property).IsEmpty() && !Setting(provider.token_url_property).IsEmpty();
   }

   AnsiString
   ReaderOAuth::RedirectUri()
   {
      AnsiString base = Utf8(Configuration::Instance()->GetImportOAuthRedirectBaseUrl());
      base.Trim();
      while (base.EndsWith("/"))
         base = base.Mid(0, base.GetLength() - 1);
      if (base.IsEmpty())
         return "";
      return base + "/portal/oauth/callback";
   }

   namespace
   {
      // What BeginForMigration needs of Begin: the pending entry kept under a
      // fresh state, and the provider's authorize address with the challenge
      // and the scope asked for.
      bool StartSignIn(const ReaderOAuth::Provider &provider, Pending entry, const AnsiString &scope,
                       AnsiString &url, AnsiString &nonce, AnsiString &problem)
      {
         if (!ReaderOAuth::Configured(provider))
         {
            problem = AnsiString("signing in with ") + provider.label + " is not set up on this server";
            return false;
         }

         AnsiString state;
         if (!RandomText(state) || !RandomText(entry.verifier) || !RandomText(entry.nonce))
         {
            problem = "the random generator failed";
            return false;
         }

         entry.provider = provider.word;
         entry.created = time(nullptr);

         {
            boost::lock_guard<boost::mutex> guard(stateLock);
            AdmitLocked(state, entry);
         }

         AnsiString authorize = Utf8(Setting(provider.authorize_url_property));
         url = authorize + (authorize.Find("?") >= 0 ? "&" : "?") +
               "response_type=code" +
               "&client_id=" + Encoded(Utf8(Setting(provider.client_id_property))) +
               "&redirect_uri=" + Encoded(ReaderOAuth::RedirectUri()) +
               "&scope=" + Encoded(scope) +
               "&state=" + state +
               "&code_challenge=" + ReaderOAuth::Challenge(entry.verifier) +
               "&code_challenge_method=S256" +
               provider.extra_authorize_parameters;
         nonce = entry.nonce;
         return true;
      }
   }

   bool
   ReaderOAuth::BeginForMigration(__int64 jobId, const AnsiString &linkDigest, const Provider &provider, const AnsiString &scope,
                                  const AnsiString &redeemScope, AnsiString &url, AnsiString &nonce, AnsiString &problem)
   {
      Pending entry;
      entry.migration_job_id = jobId;
      entry.migration_link = linkDigest;
      entry.redeem_scope = redeemScope;
      return StartSignIn(provider, entry, scope, url, nonce, problem);
   }

   bool
   ReaderOAuth::Begin(__int64 accountId, const Provider &provider, bool mirror, int daysToKeep,
                      AnsiString &url, AnsiString &nonce, AnsiString &problem)
   {
      if (!Configured(provider))
      {
         problem = AnsiString("signing in with ") + provider.label + " is not set up on this server";
         return false;
      }

      Pending entry;
      AnsiString state;
      if (!RandomText(state) || !RandomText(entry.verifier) || !RandomText(entry.nonce))
      {
         problem = "the random generator failed";
         return false;
      }

      entry.account_id = accountId;
      entry.provider = provider.word;
      entry.mirror = mirror;
      entry.days_to_keep = daysToKeep;
      entry.created = time(nullptr);

      {
         boost::lock_guard<boost::mutex> guard(stateLock);
         AdmitLocked(state, entry);
      }

      AnsiString authorize = Utf8(Setting(provider.authorize_url_property));
      url = authorize + (authorize.Find("?") >= 0 ? "&" : "?") +
            "response_type=code" +
            "&client_id=" + Encoded(Utf8(Setting(provider.client_id_property))) +
            "&redirect_uri=" + Encoded(RedirectUri()) +
            "&scope=" + Encoded(provider.scope) +
            "&state=" + state +
            "&code_challenge=" + Challenge(entry.verifier) +
            "&code_challenge_method=S256" +
            provider.extra_authorize_parameters;
      nonce = entry.nonce;
      return true;
   }

   bool
   ReaderOAuth::Complete(const AnsiString &state, const AnsiString &nonce, const AnsiString &code,
                         Grant &grant, AnsiString &problem)
   {
      Pending entry;
      {
         boost::lock_guard<boost::mutex> guard(stateLock);
         auto it = pending.find(state);
         if (it == pending.end())
         {
            problem = "this sign-in was not started here, or it was finished already";
            return false;
         }
         entry = it->second;
         pending.erase(it);
      }

      if (time(nullptr) - entry.created > PendingSeconds)
      {
         problem = "the sign-in took longer than ten minutes; start it again";
         return false;
      }
      if (!SameText(entry.nonce, nonce))
      {
         problem = "this sign-in was started in another browser; start it again from the webmail";
         return false;
      }
      if (code.IsEmpty())
      {
         problem = "the provider sent no code back";
         return false;
      }

      const Provider *provider = Find(entry.provider);
      if (!provider || !Configured(*provider))
      {
         problem = "signing in with this provider is no longer set up on this server";
         return false;
      }

      AnsiString form = "grant_type=authorization_code&code=" + Encoded(code) +
                        "&redirect_uri=" + Encoded(RedirectUri()) +
                        "&client_id=" + Encoded(Utf8(Setting(provider->client_id_property))) +
                        "&code_verifier=" + Encoded(entry.verifier);
      if (!entry.redeem_scope.IsEmpty())
         form += "&scope=" + Encoded(entry.redeem_scope);
      const String secret = Setting(provider->client_secret_property);
      if (!secret.IsEmpty())
         form += "&client_secret=" + Encoded(Utf8(secret));

      JsonValue answer;
      if (!PostForm(Utf8(Setting(provider->token_url_property)), form, answer, problem))
         return false;

      const std::string refreshToken = answer.GetString("refresh_token");
      if (refreshToken.empty())
      {
         problem = "the provider granted no refresh token, so the mail could be collected only for the next hour";
         return false;
      }

      grant.address = AddressOfIdToken(answer.GetString("id_token"));
      if (grant.address.IsEmpty())
      {
         problem = "the provider did not say which address signed in";
         return false;
      }

      grant.account_id = entry.account_id;
      grant.migration_job_id = entry.migration_job_id;
      grant.migration_link = entry.migration_link;
      grant.provider = entry.provider;
      grant.refresh_token = AnsiString(refreshToken.c_str());
      grant.mirror = entry.mirror;
      grant.days_to_keep = entry.days_to_keep;
      return true;
   }

   bool
   ReaderOAuth::AccessToken(std::shared_ptr<FetchAccount> fetchAccount, String &token, String &problem)
   {
      const Provider *provider = Find(AnsiString(fetchAccount->GetOAuthProvider()));
      if (!provider)
      {
         problem = _T("the account names no provider this server knows");
         return false;
      }

      const time_t now = time(nullptr);
      {
         boost::lock_guard<boost::mutex> guard(stateLock);
         auto it = tokens.find(fetchAccount->GetID());
         if (it != tokens.end() && it->second.refresh_at > now)
         {
            token = it->second.token;
            return true;
         }
      }

      if (Setting(provider->client_id_property).IsEmpty() || Setting(provider->token_url_property).IsEmpty())
      {
         problem = _T("signing in with this provider is no longer set up on this server");
         return false;
      }
      if (fetchAccount->GetOAuthToken().IsEmpty())
      {
         problem = _T("the account holds no refresh token; sign in again from the webmail");
         return false;
      }

      AnsiString form = "grant_type=refresh_token&refresh_token=" + Encoded(Utf8(fetchAccount->GetOAuthToken())) +
                        "&client_id=" + Encoded(Utf8(Setting(provider->client_id_property))) +
                        "&scope=" + Encoded(provider->scope);
      const String secret = Setting(provider->client_secret_property);
      if (!secret.IsEmpty())
         form += "&client_secret=" + Encoded(Utf8(secret));

      JsonValue answer;
      AnsiString refusal;
      if (!PostForm(Utf8(Setting(provider->token_url_property)), form, answer, refusal))
      {
         problem = String(refusal);
         return false;
      }

      token = FromUtf8(answer.GetString("access_token"));

      __int64 lifetime = answer.GetInt64("expires_in", 3600);
      if (lifetime <= 0 || lifetime > 24 * 60 * 60)
         lifetime = 3600;

      const std::string rotated = answer.GetString("refresh_token");
      if (!rotated.empty())
      {
         const String replacement = FromUtf8(rotated);
         if (replacement != fetchAccount->GetOAuthToken())
         {
            fetchAccount->SetOAuthToken(replacement);
            PersistentFetchAccount::SaveOAuthToken(fetchAccount->GetID(), replacement);
         }
      }

      boost::lock_guard<boost::mutex> guard(stateLock);
      CachedToken &cached = tokens[fetchAccount->GetID()];
      cached.token = token;
      cached.refresh_at = now + (time_t) (lifetime * 4 / 5);
      return true;
   }

   bool
   ReaderOAuth::RefreshAccessToken(const Provider &provider, const AnsiString &refreshToken, const AnsiString &scope,
                                   String &token, AnsiString &grantedScope, AnsiString &rotatedRefreshToken,
                                   bool &transient, String &problem)
   {
      grantedScope = "";
      rotatedRefreshToken = "";
      transient = false;

      if (Setting(provider.client_id_property).IsEmpty() || Setting(provider.token_url_property).IsEmpty())
      {
         problem = _T("signing in with this provider is not set up on this server");
         return false;
      }
      if (refreshToken.IsEmpty())
      {
         problem = _T("the job holds no refresh token; its owner has not signed in yet");
         return false;
      }

      AnsiString form = "grant_type=refresh_token&refresh_token=" + Encoded(refreshToken) +
                        "&client_id=" + Encoded(Utf8(Setting(provider.client_id_property)));
      if (!scope.IsEmpty())
         form += "&scope=" + Encoded(scope);
      const String secret = Setting(provider.client_secret_property);
      if (!secret.IsEmpty())
         form += "&client_secret=" + Encoded(Utf8(secret));

      JsonValue answer;
      AnsiString refusal;
      if (!PostForm(Utf8(Setting(provider.token_url_property)), form, answer, refusal, &transient))
      {
         problem = String(refusal);
         return false;
      }

      token = FromUtf8(answer.GetString("access_token"));
      grantedScope = AnsiString(answer.GetString("scope").c_str());

      const std::string rotated = answer.GetString("refresh_token");
      if (!rotated.empty() && AnsiString(rotated.c_str()) != refreshToken)
         rotatedRefreshToken = AnsiString(rotated.c_str());

      return true;
   }

   void
   ReaderOAuth::Forget(__int64 fetchAccountId)
   {
      boost::lock_guard<boost::mutex> guard(stateLock);
      tokens.erase(fetchAccountId);
   }

   AnsiString
   ReaderOAuth::Base64Url(const unsigned char *data, size_t length)
   {
      static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
      std::string out;
      out.reserve((length * 4) / 3 + 3);

      size_t i = 0;
      for (; i + 2 < length; i += 3)
      {
         const unsigned int n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
         out += alphabet[(n >> 18) & 63];
         out += alphabet[(n >> 12) & 63];
         out += alphabet[(n >> 6) & 63];
         out += alphabet[n & 63];
      }

      if (length - i == 1)
      {
         const unsigned int n = data[i] << 16;
         out += alphabet[(n >> 18) & 63];
         out += alphabet[(n >> 12) & 63];
      }
      else if (length - i == 2)
      {
         const unsigned int n = (data[i] << 16) | (data[i + 1] << 8);
         out += alphabet[(n >> 18) & 63];
         out += alphabet[(n >> 12) & 63];
         out += alphabet[(n >> 6) & 63];
      }

      return AnsiString(out.c_str());
   }

   bool
   ReaderOAuth::Base64UrlDecode(const std::string &text, std::string &bytes)
   {
      bytes.clear();
      unsigned int buffer = 0;
      int bits = 0;

      for (char c : text)
      {
         int value;
         if (c >= 'A' && c <= 'Z')
            value = c - 'A';
         else if (c >= 'a' && c <= 'z')
            value = c - 'a' + 26;
         else if (c >= '0' && c <= '9')
            value = c - '0' + 52;
         else if (c == '-' || c == '+')
            value = 62;
         else if (c == '_' || c == '/')
            value = 63;
         else if (c == '=')
            break;
         else
            return false;

         buffer = (buffer << 6) | (unsigned int) value;
         bits += 6;
         if (bits >= 8)
         {
            bits -= 8;
            bytes += (char) ((buffer >> bits) & 0xFF);
         }
      }

      return true;
   }

   AnsiString
   ReaderOAuth::Challenge(const AnsiString &verifier)
   {
      unsigned char digest[SHA256_DIGEST_LENGTH];
      SHA256((const unsigned char *) verifier.c_str(), (size_t) verifier.GetLength(), digest);
      return Base64Url(digest, sizeof(digest));
   }

   String
   ReaderOAuth::AddressOfIdToken(const std::string &idToken)
   {
      const size_t first = idToken.find('.');
      const size_t second = first == std::string::npos ? std::string::npos : idToken.find('.', first + 1);
      if (second == std::string::npos)
         return String();

      std::string payload;
      if (!Base64UrlDecode(idToken.substr(first + 1, second - first - 1), payload))
         return String();

      JsonValue claims;
      std::string parseError;
      if (!JsonValue::Parse(payload, claims, parseError) || !claims.IsObject())
         return String();

      std::string address = claims.GetString("email");
      if (address.find('@') == std::string::npos)
         address = claims.GetString("preferred_username");
      if (address.find('@') == std::string::npos || address.size() > 255)
         return String();

      String text = FromUtf8(address);
      text.Trim();
      return text;
   }

   void
   ReaderOAuthTester::Test()
   {
      typedef ReaderOAuth R;

      const unsigned char f[] = { 'f' };
      const unsigned char fo[] = { 'f', 'o' };
      const unsigned char foo[] = { 'f', 'o', 'o' };
      const unsigned char high[] = { 0xfb, 0xff };
      Expect(R::Base64Url(f, 1) == "Zg" && R::Base64Url(fo, 2) == "Zm8" && R::Base64Url(foo, 3) == "Zm9v", "base64url of RFC 4648's vectors, unpadded.");
      Expect(R::Base64Url(high, 2) == "-_8", "base64url writes - and _ where base64 writes + and /.");

      std::string bytes;
      Expect(R::Base64UrlDecode("Zm9v", bytes) && bytes == "foo", "base64url decodes.");
      Expect(R::Base64UrlDecode("-_8", bytes) && bytes.size() == 2 && (unsigned char) bytes[0] == 0xfb && (unsigned char) bytes[1] == 0xff, "- and _ decode.");
      Expect(!R::Base64UrlDecode("Zm9v!", bytes), "A character outside the alphabet is refused.");

      // RFC 7636 Appendix B.
      Expect(R::Challenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk") == "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM",
             "The S256 challenge matches RFC 7636's example.");

      const std::string header = "eyJhbGciOiJub25lIn0";
      const std::string withEmail = "{\"sub\":\"1\",\"email\":\"reader@gmail.example\"}";
      const std::string withName = "{\"sub\":\"2\",\"preferred_username\":\"reader@outlook.example\"}";
      const std::string withNeither = "{\"sub\":\"3\",\"name\":\"Reader\"}";
      auto token = [&header](const std::string &claims)
      {
         return header + "." + std::string(R::Base64Url((const unsigned char *) claims.data(), claims.size()).c_str()) + ".signature";
      };
      Expect(R::AddressOfIdToken(token(withEmail)) == _T("reader@gmail.example"), "The email claim is the address.");
      Expect(R::AddressOfIdToken(token(withName)) == _T("reader@outlook.example"), "preferred_username stands in when it is an address.");
      Expect(R::AddressOfIdToken(token(withNeither)).IsEmpty(), "A token naming no address gives none.");
      Expect(R::AddressOfIdToken("not-a-jwt").IsEmpty(), "A string that is not a JWT gives none.");

      Expect(R::Find("google") != nullptr && R::Find("microsoft") != nullptr && R::Find("yahoo") == nullptr, "The two providers, and no other.");

      // OUT-5: the pending table makes room rather than refusing. Before, a
      // full table answered "too many sign-ins are waiting" to everybody and
      // one account could fill it; the old code kept all five of account 7's
      // below and refused the stranger's entry at the end. Run on a table of
      // its own, swapped in under the lock, so a sign-in in flight is kept.
      {
         boost::lock_guard<boost::mutex> guard(stateLock);
         std::map<AnsiString, Pending> kept;
         kept.swap(pending);

         try
         {
            const time_t now = time(nullptr);
            auto admit = [now](const char *state, __int64 accountId, __int64 jobId)
            {
               Pending entry;
               entry.account_id = accountId;
               entry.migration_job_id = jobId;
               entry.created = now;
               AdmitLocked(state, entry);
            };

            admit("a1", 7, 0); admit("a2", 7, 0); admit("a3", 7, 0); admit("a4", 7, 0); admit("a5", 7, 0);
            Expect(pending.size() == 3 && pending.count("a3") && pending.count("a4") && pending.count("a5"),
                   "One account keeps its three newest unfinished sign-ins.");

            admit("j1", 0, 9); admit("j2", 0, 9); admit("j3", 0, 9); admit("j4", 0, 9);
            admit("other", 8, 0);
            Expect(pending.size() == 7 && !pending.count("j1") && pending.count("j4") && pending.count("a3") && pending.count("other"),
                   "A migration job keeps three of its own, and neither it nor account 7 costs account 8 its sign-in.");

            pending.clear();
            for (int i = 0; i < (int) PendingMaximum; i++)
            {
               AnsiString state;
               state.Format("fill-%d", i);
               admit(state.c_str(), 1000 + i, 0);
            }
            admit("stranger", 5, 0);
            Expect(pending.size() == PendingMaximum && pending.count("stranger") && !pending.count("fill-0") && pending.count("fill-1"),
                   "A full table drops its oldest for a new sign-in rather than refusing it.");
         }
         catch (...)
         {
            pending.swap(kept);
            throw;
         }

         pending.swap(kept);
      }
   }
}
