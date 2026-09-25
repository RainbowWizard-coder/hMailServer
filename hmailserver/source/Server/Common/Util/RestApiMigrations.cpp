// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Migration jobs over REST (Application/MigrationManager.h has the model).
//
//    GET    /api/v1/migrations                          the migrations, each summarised
//    POST   /api/v1/migrations                          create one
//    GET    /api/v1/migrations/{id}                     one, with its jobs counted by state
//    DELETE /api/v1/migrations/{id}                     delete it, when no job of it runs
//    POST   /api/v1/migrations/{id}/start               start it
//    POST   /api/v1/migrations/{id}/pause               pause it: running jobs stop between two commands
//    POST   /api/v1/migrations/{id}/resume              resume it: paused jobs are queued again
//    GET    /api/v1/migrations/{id}/summary.csv         one row per job, for a spreadsheet
//    GET    /api/v1/migrations/{id}/jobs                its jobs
//    POST   /api/v1/migrations/{id}/jobs                one job (JSON) or many (a CSV body)
//    GET    /api/v1/migrations/{id}/jobs/{job}          one job
//    GET    /api/v1/migrations/{id}/jobs/{job}/report   its report, while it runs as well
//    POST   /api/v1/migrations/{id}/jobs/{job}/retry    queue it again, with a new secret if given
//    POST   /api/v1/migrations/{id}/jobs/{job}/cancel   cancel it; what was copied stays
//    POST   /api/v1/migrations/{id}/jobs/{job}/signin   a new sign-in link for its owner
//
// A migration of kind pst is Outlook data files rather than a server: each job's
// source_user is the name of a file in <pst_import_directory>/<the target's
// domain>/ (Util/PstImportPath.h), its secret the file's password if it has one,
// and the file is resolved and checked when the job is added - a name that is
// not a plain one, or that leads anywhere but into that directory, refuses the
// whole request as any wrong line does - and kept by the one name the file has
// there however it was spelled, so that two spellings of one file are one job.
// Adding one needs manage_mailbox in the target's domain beside the route's
// manage_accounts: the job writes into another account's folders. Nothing of
// the file is read here: the job reads it (Application/MigrationJobPst.cpp), and
// a file it refuses fails the job with the reason.
//
// And, before any credential, GET /portal/oauth/migration?job=&key=: the page a
// job's sign-in link opens, which sends the browser to Google or Microsoft with
// the cookie the callback checks - the fetch-account import's consent flow
// (ReaderOAuth), for a mailbox that is not an account here.
//
// WHO. No new permission: a migration fills accounts, so it takes what every
// change to an account takes - manage_accounts, and view_accounts to read. A
// credential confined to named domains reaches a migration only when it has a
// job copying into one of those domains and none copying anywhere else (a job
// whose account has been deleted counts for nobody), and adds a job only for a
// target in one of them; a migration it does not reach is not found, as another
// domain's account is not. A migration with no jobs is no domain's, so only a
// credential that is not confined creates one or reaches it.
//
// A SECRET IS NEVER RETURNED. A job's password or refresh token is written once
// and kept protected at rest (MigrationStore); a sign-in link's key is answered
// once, when it is made, and kept as its digest; opening the link spends it,
// and a flow begun from a link since replaced is refused at its end. The audit
// trail records a migration and its jobs made, started, paused and deleted by
// whoever did it (AuditTrail lists both tables), a job by its mailbox and its
// target, the secret as "(changed)"; the cancel of a running job, which its own
// thread writes, is recorded as an event by MigrationManager.

#include "StdAfx.h"

#include "RestApiServer.h"

#include "DavSupport.h"
#include "HttpsClient.h"
#include "JsonDocument.h"
#include "MigrationSources.h"
#include "MigrationStore.h"
#include "PstImportPath.h"
#include "ReaderOAuth.h"
#include "SecureCompare.h"
#include "Unicode.h"
#include "Parsing/StringParser.h"
#include "Strings/Formatter.h"
#include "../Application/MigrationJob.h"
#include "../Application/MigrationManager.h"
#include "../BO/Account.h"
#include "../Cache/CacheContainer.h"
#include "../Persistence/PersistentAccount.h"

#include <openssl/rand.h>

#include <cmath>
#include <set>
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
      // The most jobs one request adds: a CSV of five hundred mailboxes is the
      // case this is for, and ten times that is still one request.
      const size_t MaxJobsPerRequest = 5000;

      String Utf8ToString(const std::string &utf8)
      {
         String value;
         Unicode::MultiByteToWide(AnsiString(utf8.c_str()), value);
         return value;
      }

      AnsiString Utf8(const String &value)
      {
         AnsiString bytes;
         Unicode::WideToMultiByte(value, bytes);
         return bytes;
      }

      AnsiString Number(__int64 value)
      {
         AnsiString text;
         text.Format("%I64d", value);
         return text;
      }

      const char *SecurityWord(int security)
      {
         switch (security)
         {
         case CSSSL:              return "tls";
         case CSSTARTTLSOptional: return "starttls_optional";
         case CSSTARTTLSRequired: return "starttls_required";
         default:                 return "none";
         }
      }

      bool ParseSecurityWord(const std::string &word, int &security)
      {
         if (word == "none") security = CSNone;
         else if (word == "tls") security = CSSSL;
         else if (word == "starttls_optional") security = CSSTARTTLSOptional;
         else if (word == "starttls_required") security = CSSTARTTLSRequired;
         else return false;
         return true;
      }

      const char *MigrationStateWord(int state)
      {
         switch (state)
         {
         case MigrationStore::MigrationRunning:  return "running";
         case MigrationStore::MigrationPaused:   return "paused";
         case MigrationStore::MigrationFinished: return "finished";
         default:                                return "new";
         }
      }

      String TargetAddress(__int64 accountId)
      {
         std::shared_ptr<const Account> account = CacheContainer::Instance()->GetAccount(accountId);
         return account ? account->GetAddress() : String();
      }

      bool ParseObject(const AnsiString &text, JsonValue &value)
      {
         AnsiString trimmed = text;
         trimmed.Trim();
         std::string json = trimmed.IsEmpty() ? std::string("{}") : std::string(trimmed.c_str(), (size_t) trimmed.GetLength());
         std::string error;
         return JsonValue::Parse(json, value, error) && value.IsObject();
      }

      bool OnlyKnown(const JsonValue &body, const char *const *known, size_t count, AnsiString &unknown)
      {
         for (const std::pair<std::string, JsonValue> &member : body.Members())
         {
            bool found = false;
            for (size_t i = 0; i < count && !found; i++)
               found = member.first == known[i];
            if (!found)
            {
               unknown = AnsiString(member.first.substr(0, 48).c_str());
               return false;
            }
         }
         return true;
      }

      bool ReadInteger(const JsonValue &body, const char *key, long minimum, long maximum, long &out, AnsiString &error)
      {
         const JsonValue *member = body.Get(key);
         if (!member || member->IsNull())
            return true;

         const double number = member->IsNumber() ? member->AsNumber() : 0.0;
         if (!member->IsNumber() || std::floor(number) != number || number < (double) minimum || number > (double) maximum)
         {
            error.Format("%s must be a whole number between %ld and %ld", key, minimum, maximum);
            return false;
         }

         out = (long) number;
         return true;
      }

      bool ReadText(const JsonValue &body, const char *key, std::string &out, AnsiString &error)
      {
         const JsonValue *member = body.Get(key);
         if (!member || member->IsNull())
            return true;

         if (!member->IsString())
         {
            error.Format("%s must be a string", key);
            return false;
         }

         out = member->AsString();
         return true;
      }

      std::string Trim(const std::string &value)
      {
         const size_t first = value.find_first_not_of(" \t\r\n");
         if (first == std::string::npos)
            return std::string();
         const size_t last = value.find_last_not_of(" \t\r\n");
         return value.substr(first, last - first + 1);
      }

      // One line of CSV (RFC 4180): fields split on commas, a field in double
      // quotes may hold a comma, and "" inside one is a quote.
      bool ParseCsvLine(const std::string &line, std::vector<std::string> &fields, std::string &problem)
      {
         fields.clear();
         std::string field;
         bool quoted = false;
         bool wasQuoted = false;

         for (size_t i = 0; i < line.size(); i++)
         {
            const char c = line[i];
            if (quoted)
            {
               if (c == '"')
               {
                  if (i + 1 < line.size() && line[i + 1] == '"')
                  {
                     field += '"';
                     i++;
                  }
                  else
                     quoted = false;
               }
               else
                  field += c;
               continue;
            }

            if (c == '"')
            {
               if (!Trim(field).empty())
               {
                  problem = "a quote in the middle of a field";
                  return false;
               }
               field.clear();
               quoted = true;
               wasQuoted = true;
            }
            else if (c == ',')
            {
               fields.push_back(wasQuoted ? field : Trim(field));
               field.clear();
               wasQuoted = false;
            }
            else
               field += c;
         }

         if (quoted)
         {
            problem = "a quoted field that is not closed";
            return false;
         }

         fields.push_back(wasQuoted ? field : Trim(field));
         return true;
      }

      AnsiString CsvField(const AnsiString &raw)
      {
         // A spreadsheet reads a cell that starts with = + - @ (or a tab or a
         // CR) as a formula, and a last error is the remote server's own text:
         // such a value is written as text, behind an apostrophe.
         AnsiString value = raw;
         if (!value.IsEmpty())
         {
            const char first = value.GetAt(0);
            if (first == '=' || first == '+' || first == '-' || first == '@' || first == '\t' || first == '\r')
               value = "'" + value;
         }

         if (value.FindOneOf(",\"\r\n") < 0)
            return value;

         AnsiString quoted = value;
         quoted.Replace("\"", "\"\"");
         return "\"" + quoted + "\"";
      }

      // A sign-in link's key: 32 random bytes as hex. Only its digest is kept.
      bool NewSignInKey(AnsiString &key)
      {
         unsigned char bytes[32];
         if (RAND_bytes(bytes, sizeof(bytes)) != 1)
            return false;

         static const char *hex = "0123456789abcdef";
         key = "";
         for (size_t i = 0; i < sizeof(bytes); i++)
         {
            key += hex[bytes[i] >> 4];
            key += hex[bytes[i] & 0x0f];
         }
         return true;
      }

      AnsiString SignInUrl(__int64 jobId, const AnsiString &key)
      {
         AnsiString base = Utf8(Configuration::Instance()->GetImportOAuthRedirectBaseUrl());
         base.Trim();
         while (base.EndsWith("/"))
            base = base.Mid(0, base.GetLength() - 1);
         return base + "/portal/oauth/migration?job=" + Number(jobId) + "&key=" + key;
      }

      AnsiString HtmlEscape(const AnsiString &value)
      {
         AnsiString out;
         for (int i = 0; i < value.GetLength(); i++)
         {
            const char c = value.GetAt(i);
            if (c == '&') out += "&amp;";
            else if (c == '<') out += "&lt;";
            else if (c == '>') out += "&gt;";
            else if (c == '"') out += "&quot;";
            else if (c == '\'') out += "&#39;";
            else out += c;
         }
         return out;
      }

      // The page the owner of a mailbox being migrated sees: no script, no
      // form, nothing but what happened.
      HttpResponse Page(int status, const AnsiString &title, const AnsiString &inner)
      {
         HttpResponse response;
         response.status = status;
         response.content_type = "text/html; charset=utf-8";
         response.body =
            "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><title>" + HtmlEscape(title) + "</title>"
            "<style>"
            ":root{--bg:#0f1419;--card:#161b22;--line:#30363d;--text:#e6edf3;--muted:#9aa4ae}"
            "@media (prefers-color-scheme: light){:root{--bg:#f6f8fa;--card:#ffffff;--line:#d0d7de;--text:#1f2328;--muted:#57606a}}"
            "body{font-family:system-ui,sans-serif;background:var(--bg);color:var(--text);display:flex;justify-content:center;padding:32px 16px;margin:0}"
            "main{max-width:520px;width:100%;box-sizing:border-box;background:var(--card);border:1px solid var(--line);border-radius:12px;padding:24px}"
            "h1{font-size:20px;margin:0 0 12px;overflow-wrap:anywhere}p{margin:8px 0;color:var(--muted);font-size:14px;overflow-wrap:anywhere}"
            "</style></head><body><main><h1>" + HtmlEscape(title) + "</h1>" + inner + "</main></body></html>";
         response.extra_headers =
            "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; base-uri 'none'; frame-ancestors 'none'\r\n"
            "X-Frame-Options: DENY\r\n"
            "X-Content-Type-Options: nosniff\r\n"
            "Referrer-Policy: no-referrer\r\n"
            "X-Robots-Tag: noindex, noarchive\r\n"
            "Cache-Control: no-store\r\n";
         return response;
      }

      AnsiString Paragraph(const AnsiString &text)
      {
         return "<p>" + HtmlEscape(text) + "</p>";
      }

      AnsiString JobJson(const MigrationJobRecord &job, const MigrationRecord &migration)
      {
         const MigrationProvider *provider = MigrationSources::ByKind(migration.kind);
         const bool awaiting = !job.signInHash.IsEmpty();

         AnsiString json = "{";
         json += "\"id\":" + Number(job.id);
         json += ",\"migration\":" + Number(job.migrationId);
         json += ",\"source_user\":" + MigrationJob::JsonString(Utf8(job.sourceUser));
         json += ",\"target\":" + MigrationJob::JsonString(Utf8(TargetAddress(job.targetAccountId)));
         json += ",\"state\":" + MigrationJob::JsonString(MigrationJob::StateWord(job.state));
         json += ",\"phase\":" + MigrationJob::JsonString(MigrationJob::PhaseWord(job.phase));
         json += AnsiString(",\"awaiting_signin\":") + (awaiting ? "true" : "false");
         json += AnsiString(",\"signs_in\":") + (provider && provider->oauthProvider[0] != '\0' ? "true" : "false");
         json += ",\"folders_total\":" + Number(job.foldersTotal);
         json += ",\"folders_done\":" + Number(job.foldersDone);
         json += ",\"messages_total\":" + Number(job.messagesTotal);
         json += ",\"messages_copied\":" + Number(job.messagesDone);
         json += ",\"bytes\":" + Number(job.bytesDone);
         json += ",\"skipped\":" + Number(job.skipped);
         json += ",\"last_error\":" + MigrationJob::JsonString(Utf8(job.lastError));
         json += ",\"attempts\":" + Number(job.attempts);
         json += ",\"next_attempt\":" + Number(job.nextAttempt);
         json += ",\"started\":" + Number(job.startedAt);
         json += ",\"finished\":" + Number(job.finishedAt);
         json += "}";
         return json;
      }

      AnsiString MigrationJson(const MigrationRecord &migration, const std::vector<MigrationJobRecord> &jobs)
      {
         const MigrationProvider *provider = MigrationSources::ByKind(migration.kind);

         int counts[6] = { 0, 0, 0, 0, 0, 0 };
         int awaiting = 0;
         __int64 total = 0, copied = 0, bytes = 0, skipped = 0;
         for (const MigrationJobRecord &job : jobs)
         {
            if (job.state >= 0 && job.state < 6)
               counts[job.state]++;
            if (!job.signInHash.IsEmpty())
               awaiting++;
            total += job.messagesTotal;
            copied += job.messagesDone;
            bytes += job.bytesDone;
            skipped += job.skipped;
         }

         AnsiString json = "{";
         json += "\"id\":" + Number(migration.id);
         json += ",\"name\":" + MigrationJob::JsonString(Utf8(migration.name));
         json += ",\"kind\":" + MigrationJob::JsonString(provider ? provider->word : "imap");
         json += ",\"host\":" + MigrationJob::JsonString(Utf8(migration.host));
         json += ",\"port\":" + Number(migration.port);
         json += ",\"security\":" + MigrationJob::JsonString(SecurityWord(migration.security));
         json += ",\"oauth_provider\":" + MigrationJob::JsonString(Utf8(migration.oauthProvider));
         json += ",\"dav_url\":" + MigrationJob::JsonString(Utf8(migration.davUrl));
         json += ",\"concurrency\":" + Number(migration.concurrency);
         json += ",\"messages_per_minute\":" + Number(migration.messagesPerMinute);
         json += ",\"state\":" + MigrationJob::JsonString(MigrationStateWord(migration.state));
         json += ",\"created\":" + Number(migration.created);
         json += ",\"started\":" + Number(migration.startedAt);
         json += ",\"finished\":" + Number(migration.finishedAt);
         json += ",\"jobs\":{\"total\":" + Number((__int64) jobs.size()) +
                 ",\"queued\":" + Number(counts[MigrationStore::JobQueued]) +
                 ",\"running\":" + Number(counts[MigrationStore::JobRunning]) +
                 ",\"paused\":" + Number(counts[MigrationStore::JobPaused]) +
                 ",\"done\":" + Number(counts[MigrationStore::JobDone]) +
                 ",\"failed\":" + Number(counts[MigrationStore::JobFailed]) +
                 ",\"cancelled\":" + Number(counts[MigrationStore::JobCancelled]) +
                 ",\"awaiting_signin\":" + Number(awaiting) + "}";
         json += ",\"messages_total\":" + Number(total);
         json += ",\"messages_copied\":" + Number(copied);
         json += ",\"bytes\":" + Number(bytes);
         json += ",\"skipped\":" + Number(skipped);
         json += "}";
         return json;
      }

      AnsiString ErrorJson(const String &sentence)
      {
         return "{\"error\":" + MigrationJob::JsonString(Utf8(sentence)) + "}";
      }

      // One job as a request asked for it, before anything is written.
      struct Requested
      {
         Requested() : line(0), targetAccountId(0) { }

         int line;
         std::string user;
         std::string secret;
         std::string target;
         __int64 targetAccountId;
      };
   }

   AnsiString
   RestApiServer::OpenApiMigrationsPaths_()
   {
      static const char *paths =
         ",\"/api/v1/migrations\":{"
         "\"get\":{\"summary\":\"The migrations\",\"description\":\"Every migration - a batch of mailboxes copied from another server, a job per mailbox - with its jobs counted by state and its messages totalled. A credential confined to named domains sees only the migrations with a job copying into one of them and none copying anywhere else - a job whose account was deleted counts for nobody, and a migration with no jobs is no domain's. Needs view_accounts.\",\"responses\":{\"200\":{\"description\":\"{migrations:[...]}\"}}},"
         "\"post\":{\"summary\":\"Create a migration\",\"description\":\"Body: name (required); kind (imap, google, microsoft365 or pst; default imap); host (required for imap); port (993 with tls and 143 otherwise, when not given); security (none, tls, starttls_optional, starttls_required; default tls); dav_url (where CardDAV and CalDAV are, when not https://<host>/ - an https URL, or plain http only to this machine); concurrency (1 to 32; default 4), how many of its jobs run at once, below MigrationConnectionsPerHost across every migration at one host; messages_per_minute (0 to 100000, 0 for no limit; default 0), each job waiting between fetches to keep below it. For google and microsoft365 the source is the provider's own IMAP server - imap.gmail.com or outlook.office365.com, 993, tls - and a host, port or security naming anything else is refused, since the owner's token is sent there. A pst migration copies Outlook data files, read by this server's own PST reader: it takes no host, port, security or dav_url (what is sent for them is not used), and each of its jobs names a file in the pst_import_directory setting's subdirectory for the target account's domain. It is created new; POST .../jobs adds the mailboxes and POST .../start runs them. Needs manage_accounts, from a credential not confined to named domains: a migration with no jobs is no domain's.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"name\"],\"properties\":{"
            "\"name\":{\"type\":\"string\",\"description\":\"What the migration is called here.\"},"
            "\"kind\":{\"type\":\"string\",\"enum\":[\"imap\",\"google\",\"microsoft365\",\"pst\"],\"description\":\"An IMAP server with passwords; Google Workspace or Microsoft 365, each job signed in to by its owner; or Outlook data files (PST) copied onto this server.\"},"
            "\"host\":{\"type\":\"string\",\"description\":\"The source's IMAP host.\"},"
            "\"port\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":65535},"
            "\"security\":{\"type\":\"string\",\"enum\":[\"tls\",\"starttls_required\",\"starttls_optional\",\"none\"]},"
            "\"dav_url\":{\"type\":\"string\",\"description\":\"Where the source's CardDAV and CalDAV are, when not https://<host>/.\"},"
            "\"concurrency\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":32,\"description\":\"How many of its jobs run at once.\"},"
            "\"messages_per_minute\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":100000,\"description\":\"The most messages a minute each job fetches; 0 for no limit.\"}"
         "}}}}},\"responses\":{\"201\":{\"description\":\"The migration\"},\"400\":{\"description\":\"A field refused (error names it)\"},\"403\":{\"description\":\"A credential confined to named domains\"}}}},"
         "\"/api/v1/migrations/{id}\":{"
         "\"get\":{\"summary\":\"One migration\",\"description\":\"As the listing shows it: the source, the throttle, the state (new, running, paused, finished), its jobs counted by state and awaiting a sign-in, and the messages totalled across them.\",\"responses\":{\"200\":{\"description\":\"The migration\"},\"404\":{\"description\":\"No such migration\"}}},"
         "\"delete\":{\"summary\":\"Delete a migration\",\"description\":\"The migration, its jobs and their resume state. What was copied stays where it was copied to. Refused while a job of it runs: pause it first.\",\"responses\":{\"200\":{\"description\":\"Deleted\"},\"404\":{\"description\":\"No such migration\"},\"409\":{\"description\":\"A job of it is running\"}}}},"
         "\"/api/v1/migrations/{id}/start\":{\"post\":{\"summary\":\"Start a migration\",\"description\":\"Its queued jobs start as the concurrency and MigrationConnectionsPerHost allow. A finished migration started again runs what was added or retried since.\",\"responses\":{\"200\":{\"description\":\"The migration\"},\"409\":{\"description\":\"Running already, or paused (resume it)\"}}}},"
         "\"/api/v1/migrations/{id}/pause\":{\"post\":{\"summary\":\"Pause a migration\",\"description\":\"Its running jobs stop between two commands, write what they copied and their resume marks, and are paused; its queued jobs wait.\",\"responses\":{\"200\":{\"description\":\"The migration\"},\"409\":{\"description\":\"Not running\"}}}},"
         "\"/api/v1/migrations/{id}/resume\":{\"post\":{\"summary\":\"Resume a migration\",\"description\":\"Every paused job of it is queued again at once, a retry that was waiting its turn included, and goes on from its marks.\",\"responses\":{\"200\":{\"description\":\"The migration\"},\"409\":{\"description\":\"Not paused, or its jobs still stopping\"}}}},"
         "\"/api/v1/migrations/{id}/summary.csv\":{\"get\":{\"summary\":\"A migration's jobs as CSV\",\"description\":\"One row per job: job, source_user, target, state, phase, folders_total, folders_done, messages_total, messages_copied, skipped, bytes, attempts, last_error, started, finished (UTC seconds). For the person running five hundred.\",\"responses\":{\"200\":{\"description\":\"text/csv\"}}}},"
         "\"/api/v1/migrations/{id}/jobs\":{"
         "\"get\":{\"summary\":\"A migration's jobs\",\"description\":\"Each: id, source_user, target, state (queued, running, paused, done, failed, cancelled), phase (mail, contacts, calendars, report), awaiting_signin, folders and messages done of their totals, bytes, skipped, last_error, attempts and next_attempt. The secret is never returned.\",\"responses\":{\"200\":{\"description\":\"{jobs:[...]}\"}}},"
         "\"post\":{\"summary\":\"Add jobs to a migration\",\"description\":\"A JSON body - source_user, secret and target - adds one job; any other body is CSV, one job a line as sourceuser,secret,target, a first line naming the columns skipped, fields quoted as RFC 4180 quotes them. target is an account of this server. secret is the source password for kind imap; for google and microsoft365 it may be a refresh token, or empty, when the job waits for its owner to sign in and the answer carries signin_url - a link for that person to open, answered once. For kind pst, sourceuser is the name of a file in <pst_import_directory>/<the target's domain>/ - archive.pst, or sub/archive.pst, never a path, a drive, a share or a name with .. in it, and never one that leads out of that directory through a link or a junction - and secret is its password when it has one; the file must be there, and the job keeps it by the one name it has there (sub/archive.pst, however it was spelled). A first line file,password,target is a heading as well. Every line is checked before any job is made: a malformed line, an unknown target, a target outside the credential's domains, a mailbox already in the migration, or one a job of another migration is copying into that account now (409) refuses the whole request, naming the line; a target that is not an address is not repeated back, since a row with its columns the wrong way round has a password there. A UTF-8 byte order mark before the first line is ignored. Needs manage_accounts in every target's domain, and for kind pst manage_mailbox there as well, since the job writes into the account's folders.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"properties\":{\"source_user\":{\"type\":\"string\"},\"secret\":{\"type\":\"string\",\"writeOnly\":true},\"target\":{\"type\":\"string\"}}}},\"text/csv\":{\"schema\":{\"type\":\"string\"}}}},\"responses\":{\"201\":{\"description\":\"{created, jobs:[... with signin_url where one was made]}\"},\"400\":{\"description\":\"A line refused (error and line name it)\"},\"403\":{\"description\":\"A target outside the credential's domains\"},\"409\":{\"description\":\"A mailbox another migration's job is copying into that account now\"}}}},"
         "\"/api/v1/migrations/{id}/jobs/{job}\":{\"get\":{\"summary\":\"One job\",\"responses\":{\"200\":{\"description\":\"The job\"},\"404\":{\"description\":\"No such job in this migration\"}}}},"
         "\"/api/v1/migrations/{id}/jobs/{job}/report\":{\"get\":{\"summary\":\"A job's report\",\"description\":\"JSON, written at every resume mark and at the end, so readable while the job runs: per folder the source's count, what was copied, what was skipped and why, the warnings (what a message was copied without, by kind), the special use and any UIDVALIDITY restart; the contacts and calendars copied or skipped and why; the bytes, the duration, the errors met and retried (the most recent fifty, as many as fit), the first and last message dates. A job of kind pst adds pst: the file and its size, the store's name, record key, version and encoding, not_read (the folders that could not be read), not_mail (what is not mail, by class, counted and not imported; not_mail_folders and not_mail_messages are its two parts), the refusal when the file was refused, and list_attempts.\",\"responses\":{\"200\":{\"description\":\"The report\"}}}},"
         "\"/api/v1/migrations/{id}/jobs/{job}/retry\":{\"post\":{\"summary\":\"Retry a job\",\"description\":\"A failed, cancelled or paused job - or one whose row says running with nothing running it - is queued again from its phase and its marks, its attempts counted afresh. Body, optional: secret, the mended password or a new refresh token.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"properties\":{\"secret\":{\"type\":\"string\",\"writeOnly\":true}}}}}},\"responses\":{\"200\":{\"description\":\"The job\"},\"409\":{\"description\":\"Running, queued or done\"}}}},"
         "\"/api/v1/migrations/{id}/jobs/{job}/cancel\":{\"post\":{\"summary\":\"Cancel a job\",\"description\":\"A running job stops between two commands; what it copied stays, and its state is cancelled. The audit trail records who cancelled it, a running one as an event, since its own thread writes the row.\",\"responses\":{\"200\":{\"description\":\"The job\"},\"409\":{\"description\":\"Done, or cancelled already\"}}}},"
         "\"/api/v1/migrations/{id}/jobs/{job}/signin\":{\"post\":{\"summary\":\"A new sign-in link for a job\",\"description\":\"For google and microsoft365: a new link for the mailbox's owner to open, answered once as signin_url; the one before it stops working, and so does a sign-in begun from it and not finished. A link works once: opening it spends it, and a sign-in as another address than the job's gives it back. The job waits for the sign-in and is not started until it completes; a failed or cancelled job is queued again when it does. Needs import_oauth_redirect_base_url and the provider's client registered.\",\"responses\":{\"200\":{\"description\":\"signin_url\"},\"409\":{\"description\":\"Not a kind that signs in, or the job is running or done\"},\"503\":{\"description\":\"Signing in with the provider is not set up on this server\"}}}}";

      return AnsiString(paths);
   }

   HttpResponse
   RestApiServer::HandleMigrations_(const Caller &caller, const Route &route, const AnsiString &request)
   {
      const AnsiString requestBody = GetRequestBody_(request);

      // A confined credential reaches a migration when it has a job copying
      // into one of its domains and none copying anywhere else. A job whose
      // account has been deleted counts for nobody, so it neither gives a
      // migration away nor keeps it from its domain; and a migration with no
      // job at all is no domain's, and reached only by a credential that is not
      // confined.
      auto within = [&caller](const std::vector<MigrationJobRecord> &jobs) -> bool
      {
         if (caller.domains.empty())
            return true;

         bool reached = false;
         for (const MigrationJobRecord &job : jobs)
         {
            const String address = TargetAddress(job.targetAccountId);
            if (address.IsEmpty())
               continue;
            if (!IsDomainAllowed_(caller.domains, StringParser::ExtractDomain(address)))
               return false;
            reached = true;
         }
         return reached;
      };

      if (route.kind == RouteMigrationList)
      {
         std::vector<MigrationRecord> migrations;
         if (!MigrationStore::ListMigrations(migrations))
            return BuildResponse_(500, "{\"error\":\"the migrations could not be read\"}");

         AnsiString body = "{\"migrations\":[";
         bool first = true;
         for (const MigrationRecord &migration : migrations)
         {
            std::vector<MigrationJobRecord> jobs;
            if (!MigrationStore::ListJobs(migration.id, false, jobs) || !within(jobs))
               continue;

            if (!first)
               body += ",";
            body += MigrationJson(migration, jobs);
            first = false;
         }
         body += "]}";
         return BuildResponse_(200, body);
      }

      if (route.kind == RouteMigrationCreate)
      {
         // A migration with no jobs is no domain's: a credential confined to
         // named domains would make one it could not then reach.
         if (!caller.domains.empty())
            return BuildResponse_(403, "{\"error\":\"a credential confined to named domains cannot create a migration: until it has jobs a migration is no domain's, so an administrator of the whole server creates it, and a job into one of your domains then lets you see it and add to it\"}");

         JsonValue body;
         if (!ParseObject(requestBody, body))
            return BuildResponse_(400, "{\"error\":\"the body must be a JSON object\"}");

         static const char *const known[] = { "name", "kind", "host", "port", "security", "dav_url", "concurrency", "messages_per_minute" };
         AnsiString unknown;
         if (!OnlyKnown(body, known, sizeof(known) / sizeof(known[0]), unknown))
            return BuildResponse_(400, "{\"error\":" + MigrationJob::JsonString("unknown field: " + unknown) + "}");

         std::string name, kind = "imap", host, security = "tls", davUrl;
         AnsiString error;
         if (!ReadText(body, "name", name, error) || !ReadText(body, "kind", kind, error) || !ReadText(body, "host", host, error) ||
             !ReadText(body, "security", security, error) || !ReadText(body, "dav_url", davUrl, error))
            return BuildResponse_(400, "{\"error\":" + MigrationJob::JsonString(error) + "}");

         name = Trim(name);
         host = Trim(host);
         davUrl = Trim(davUrl);

         if (name.empty() || name.size() > (size_t) MigrationStore::MaximumNameLength)
            return BuildResponse_(400, "{\"error\":\"name is required, at most 255 characters\"}");

         const MigrationProvider *provider = MigrationSources::ByWord(AnsiString(kind.c_str()));
         if (!provider)
            return BuildResponse_(400, "{\"error\":\"kind must be imap, google, microsoft365 or pst\"}");

         if (provider->kind == MigrationStore::KindPst)
         {
            // Outlook data files: no host, port, security or DAV - what a form
            // sends for them is not used - and no sign-in.
            long pstConcurrency = 4;
            long pstPerMinute = 0;
            if (!ReadInteger(body, "concurrency", 1, 32, pstConcurrency, error) ||
                !ReadInteger(body, "messages_per_minute", 0, 100000, pstPerMinute, error))
               return BuildResponse_(400, "{\"error\":" + MigrationJob::JsonString(error) + "}");

            MigrationRecord files;
            files.name = Utf8ToString(name);
            files.kind = MigrationStore::KindPst;
            files.port = 0;
            files.security = CSNone;
            files.concurrency = (int) pstConcurrency;
            files.messagesPerMinute = (int) pstPerMinute;
            files.state = MigrationStore::MigrationNew;

            if (!MigrationStore::CreateMigration(files))
               return BuildResponse_(500, "{\"error\":\"the migration could not be saved; see the error log\"}");

            LOG_APPLICATION(Formatter::Format(_T("RestApi: migration {0} ({1}) of PST files created by {2}."), files.id, files.name, String(caller.identity)));

            return BuildResponse_(201, MigrationJson(files, std::vector<MigrationJobRecord>()));
         }

         int securityValue = CSSSL;
         if (!ParseSecurityWord(security, securityValue))
            return BuildResponse_(400, "{\"error\":\"security must be one of none, tls, starttls_optional, starttls_required\"}");

         // Google and Microsoft 365: the provider's own IMAP server, on its own
         // port, over TLS, and nothing else - the job sends the mailbox owner's
         // token there, and a token sent to another host, or in the clear, is
         // a mailbox given away.
         if (provider->oauthProvider[0] != '\0')
         {
            const std::string own = provider->imapHost;
            AnsiString problem;
            if (!host.empty() && AnsiString(host.c_str()).CompareNoCase(own.c_str()) != 0)
               problem.Format("host must be %s for a %s migration, or left out: the owner's token is sent only to the provider's own server", own.c_str(), provider->label);
            else if (body.Get("port") && !body.Get("port")->IsNull() &&
                     (!body.Get("port")->IsNumber() || body.Get("port")->AsNumber() != (double) provider->imapPort))
               problem.Format("port must be %d for a %s migration, or left out", provider->imapPort, provider->label);
            else if (securityValue != CSSSL)
               problem.Format("security must be tls for a %s migration, or left out: the owner's token never crosses the network in the clear", provider->label);

            if (!problem.IsEmpty())
               return BuildResponse_(400, "{\"error\":" + MigrationJob::JsonString(problem) + "}");
         }

         if (host.empty())
            host = provider->imapHost;
         if (host.empty())
            return BuildResponse_(400, "{\"error\":\"host is required for a migration from an IMAP server\"}");
         if (host.size() > (size_t) MigrationStore::MaximumHostLength || host.find_first_of(" /\\@") != std::string::npos)
            return BuildResponse_(400, "{\"error\":\"host must be a host name or an address, at most 255 characters\"}");

         long port = provider->imapPort;
         if (!body.Get("port") && securityValue != CSSSL && provider->kind == MigrationStore::KindImap)
            port = 143;
         long concurrency = 4;
         long perMinute = 0;
         if (!ReadInteger(body, "port", 1, 65535, port, error) ||
             !ReadInteger(body, "concurrency", 1, 32, concurrency, error) ||
             !ReadInteger(body, "messages_per_minute", 0, 100000, perMinute, error))
            return BuildResponse_(400, "{\"error\":" + MigrationJob::JsonString(error) + "}");

         if (!davUrl.empty())
         {
            bool https = false;
            AnsiString davHost, davPort, davPath;
            if (davUrl.size() > (size_t) 255 || !HttpsClient::ParseUrl(AnsiString(davUrl.c_str()), https, davHost, davPort, davPath))
               return BuildResponse_(400, "{\"error\":\"dav_url must be an http or https URL of at most 255 characters\"}");
            if (!https && !HttpsClient::IsLoopbackHost(davHost))
               return BuildResponse_(400, "{\"error\":\"dav_url may be plain http only to this machine, since the source's password would cross the network in the clear; use https\"}");
         }

         MigrationRecord migration;
         migration.name = Utf8ToString(name);
         migration.kind = provider->kind;
         migration.host = Utf8ToString(host);
         migration.port = (int) port;
         migration.security = securityValue;
         migration.oauthProvider = String(AnsiString(provider->oauthProvider));
         migration.davUrl = Utf8ToString(davUrl);
         migration.concurrency = (int) concurrency;
         migration.messagesPerMinute = (int) perMinute;
         migration.state = MigrationStore::MigrationNew;

         if (!MigrationStore::CreateMigration(migration))
            return BuildResponse_(500, "{\"error\":\"the migration could not be saved; see the error log\"}");

         LOG_APPLICATION(Formatter::Format(_T("RestApi: migration {0} ({1}) from {2} created by {3}."), migration.id, migration.name, migration.host, String(caller.identity)));

         return BuildResponse_(201, MigrationJson(migration, std::vector<MigrationJobRecord>()));
      }

      // Everything below names a migration.
      const __int64 migrationId = _atoi64(route.identifier.c_str());

      MigrationRecord migration;
      std::vector<MigrationJobRecord> jobs;
      if (!MigrationStore::GetMigration(migrationId, migration))
         return BuildResponse_(404, "{\"error\":\"no such migration\"}");
      if (!MigrationStore::ListJobs(migrationId, false, jobs))
         return BuildResponse_(500, "{\"error\":\"the migration's jobs could not be read\"}");
      if (!within(jobs))
         return BuildResponse_(404, "{\"error\":\"no such migration\"}");

      const MigrationProvider *provider = MigrationSources::ByKind(migration.kind);
      const bool signsIn = provider && provider->oauthProvider[0] != '\0';
      const bool pstFiles = migration.kind == MigrationStore::KindPst;

      auto again = [&]() -> HttpResponse
      {
         MigrationRecord now;
         std::vector<MigrationJobRecord> nowJobs;
         if (!MigrationStore::GetMigration(migrationId, now) || !MigrationStore::ListJobs(migrationId, false, nowJobs))
            return BuildResponse_(200, "{\"done\":true}");
         return BuildResponse_(200, MigrationJson(now, nowJobs));
      };

      int status = 0;
      String problem;

      switch (route.kind)
      {
      case RouteMigrationGet:
         return BuildResponse_(200, MigrationJson(migration, jobs));

      case RouteMigrationDelete:
         if (!MigrationManager::Instance()->DeleteMigration(migrationId, status, problem))
            return BuildResponse_(status, ErrorJson(problem));
         LOG_APPLICATION(Formatter::Format(_T("RestApi: migration {0} ({1}) deleted by {2}."), migrationId, migration.name, String(caller.identity)));
         return BuildResponse_(200, "{\"deleted\":true}");

      case RouteMigrationStart:
         if (!MigrationManager::Instance()->StartMigration(migrationId, status, problem))
            return BuildResponse_(status, ErrorJson(problem));
         LOG_APPLICATION(Formatter::Format(_T("RestApi: migration {0} ({1}) started by {2}."), migrationId, migration.name, String(caller.identity)));
         return again();

      case RouteMigrationPause:
         if (!MigrationManager::Instance()->PauseMigration(migrationId, status, problem))
            return BuildResponse_(status, ErrorJson(problem));
         LOG_APPLICATION(Formatter::Format(_T("RestApi: migration {0} ({1}) paused by {2}."), migrationId, migration.name, String(caller.identity)));
         return again();

      case RouteMigrationResume:
         if (!MigrationManager::Instance()->ResumeMigration(migrationId, status, problem))
            return BuildResponse_(status, ErrorJson(problem));
         LOG_APPLICATION(Formatter::Format(_T("RestApi: migration {0} ({1}) resumed by {2}."), migrationId, migration.name, String(caller.identity)));
         return again();

      case RouteMigrationSummary:
         {
            AnsiString csv = "job,source_user,target,state,phase,folders_total,folders_done,messages_total,messages_copied,skipped,bytes,attempts,last_error,started,finished\r\n";
            for (const MigrationJobRecord &job : jobs)
            {
               csv += Number(job.id) + "," + CsvField(Utf8(job.sourceUser)) + "," + CsvField(Utf8(TargetAddress(job.targetAccountId))) + "," +
                      MigrationJob::StateWord(job.state) + "," + MigrationJob::PhaseWord(job.phase) + "," +
                      Number(job.foldersTotal) + "," + Number(job.foldersDone) + "," + Number(job.messagesTotal) + "," +
                      Number(job.messagesDone) + "," + Number(job.skipped) + "," + Number(job.bytesDone) + "," +
                      Number(job.attempts) + "," + CsvField(Utf8(job.lastError)) + "," + Number(job.startedAt) + "," +
                      Number(job.finishedAt) + "\r\n";
            }

            HttpResponse response = BuildResponse_(200, csv);
            response.content_type = "text/csv; charset=utf-8";
            response.extra_headers += "Content-Disposition: attachment; filename=\"migration-" + Number(migrationId) + ".csv\"\r\n";
            return response;
         }

      case RouteMigrationJobList:
         {
            AnsiString body = "{\"jobs\":[";
            for (size_t i = 0; i < jobs.size(); i++)
            {
               if (i > 0)
                  body += ",";
               body += JobJson(jobs[i], migration);
            }
            body += "]}";
            return BuildResponse_(200, body);
         }

      case RouteMigrationJobCreate:
         {
            std::vector<Requested> requested;

            // A CSV saved as a spreadsheet's "CSV UTF-8" starts with a byte
            // order mark, which is not part of the first source user.
            AnsiString jobsBody = requestBody;
            if (jobsBody.GetLength() >= 3 && (unsigned char) jobsBody.GetAt(0) == 0xEF &&
                (unsigned char) jobsBody.GetAt(1) == 0xBB && (unsigned char) jobsBody.GetAt(2) == 0xBF)
               jobsBody = jobsBody.Mid(3);

            AnsiString trimmed = jobsBody;
            trimmed.Trim();
            if (trimmed.StartsWith("{"))
            {
               JsonValue body;
               if (!ParseObject(jobsBody, body))
                  return BuildResponse_(400, "{\"error\":\"the body must be a JSON object, or CSV\"}");

               static const char *const known[] = { "source_user", "secret", "target" };
               AnsiString unknown;
               if (!OnlyKnown(body, known, sizeof(known) / sizeof(known[0]), unknown))
                  return BuildResponse_(400, "{\"error\":" + MigrationJob::JsonString("unknown field: " + unknown) + "}");

               Requested one;
               one.line = 1;
               AnsiString error;
               if (!ReadText(body, "source_user", one.user, error) || !ReadText(body, "secret", one.secret, error) ||
                   !ReadText(body, "target", one.target, error))
                  return BuildResponse_(400, "{\"error\":" + MigrationJob::JsonString(error) + "}");
               one.user = Trim(one.user);
               one.target = Trim(one.target);
               requested.push_back(one);
            }
            else
            {
               const std::string text(jobsBody.c_str(), (size_t) jobsBody.GetLength());
               size_t start = 0;
               int lineNumber = 0;
               while (start <= text.size())
               {
                  size_t end = text.find('\n', start);
                  if (end == std::string::npos)
                     end = text.size();
                  std::string line = text.substr(start, end - start);
                  if (!line.empty() && line[line.size() - 1] == '\r')
                     line.erase(line.size() - 1);
                  start = end + 1;
                  lineNumber++;

                  if (Trim(line).empty())
                  {
                     if (end >= text.size())
                        break;
                     continue;
                  }

                  std::vector<std::string> fields;
                  std::string csvProblem;
                  if (!ParseCsvLine(line, fields, csvProblem))
                  {
                     AnsiString sentence;
                     sentence.Format("line %d: %s", lineNumber, csvProblem.c_str());
                     return BuildResponse_(400, "{\"error\":" + MigrationJob::JsonString(sentence) + ",\"line\":" + Number(lineNumber) + "}");
                  }

                  // A first line that names the columns is a heading - for a
                  // migration of PST files, "file,password,target" as well,
                  // but only as a whole line: a mailbox, or a PST, called
                  // "file" is a job.
                  if (requested.empty() && !fields.empty())
                  {
                     AnsiString first = AnsiString(fields[0].c_str());
                     first.MakeLower();
                     AnsiString last = AnsiString(fields[fields.size() - 1].c_str());
                     last.MakeLower();
                     if (first == "sourceuser" || first == "source_user" ||
                         (pstFiles && first == "file" && fields.size() == 3 && last == "target"))
                        continue;
                  }

                  if (fields.size() != 3)
                  {
                     AnsiString sentence;
                     sentence.Format("line %d: a row is sourceuser,secret,target - three fields - and this one has %d", lineNumber, (int) fields.size());
                     return BuildResponse_(400, "{\"error\":" + MigrationJob::JsonString(sentence) + ",\"line\":" + Number(lineNumber) + "}");
                  }

                  Requested one;
                  one.line = lineNumber;
                  one.user = fields[0];
                  one.secret = fields[1];
                  one.target = fields[2];
                  requested.push_back(one);

                  if (requested.size() > MaxJobsPerRequest)
                     return BuildResponse_(400, "{\"error\":\"at most 5000 jobs are added in one request\"}");

                  if (end >= text.size())
                     break;
               }
            }

            if (requested.empty())
               return BuildResponse_(400, "{\"error\":\"the body names no job: a JSON object, or CSV rows of sourceuser,secret,target\"}");

            // Every line checked before anything is written, so a refused
            // request made nothing.
            std::set<std::string> seen;
            for (const MigrationJobRecord &job : jobs)
            {
               AnsiString key = Utf8(job.sourceUser) + "\n" + Utf8(TargetAddress(job.targetAccountId));
               key.MakeLower();
               seen.insert(std::string(key.c_str()));
            }

            const bool linkPossible = !Utf8(Configuration::Instance()->GetImportOAuthRedirectBaseUrl()).Trim().IsEmpty();

            for (Requested &one : requested)
            {
               auto refuse = [&one](int httpStatus, const std::string &what) -> HttpResponse
               {
                  AnsiString sentence;
                  sentence.Format("line %d: %s", one.line, what.c_str());
                  return BuildResponse_(httpStatus, "{\"error\":" + MigrationJob::JsonString(sentence) + ",\"line\":" + Number(one.line) + "}");
               };

               if (one.user.empty() || one.user.size() > (size_t) MigrationStore::MaximumUserLength)
                  return refuse(400, "the source user is required, at most 255 characters");

               if (one.target.empty())
                  return refuse(400, "the target account is required");

               const String target = Utf8ToString(one.target);
               std::shared_ptr<Account> account = std::shared_ptr<Account>(new Account());
               if (!PersistentAccount::ReadObject(account, target) || account->GetID() == 0)
               {
                  // Named only when it is an address: a row with its columns
                  // the wrong way round has a password in the third field.
                  if (!StringParser::IsValidEmailAddress(target))
                     return refuse(400, "the third field, the target, is not an address of an account of this server");
                  return refuse(400, "the target " + one.target + " is not an account of this server");
               }

               if (!IsDomainAllowed_(caller.domains, StringParser::ExtractDomain(account->GetAddress())))
                  return refuse(403, "this credential may not administer accounts in the domain of " + one.target);

               one.targetAccountId = account->GetID();

               if (pstFiles)
               {
                  // Writing into another account's folders, which manage_mailbox
                  // is for, in the target's domain.
                  const String targetDomain = StringParser::ExtractDomain(account->GetAddress());
                  if (!CallerMayManageMailboxIn_(caller, targetDomain))
                     return refuse(403, "a PST job writes into the target's folders, which needs manage_mailbox in the domain of " + one.target);

                  // A file in the target's domain's own directory, there now; the
                  // secret is its password, when it has one.
                  String file, directory, why;
                  bool missing = false;
                  if (!PstImportPath::Resolve(Utf8ToString(one.user), targetDomain, file, directory, why, missing))
                     return refuse(400, std::string(Utf8(why).c_str()));

                  // Kept by its one name below the directory, so that the checks
                  // below - one job a file in this request, and none while
                  // another migration's job reads it into this account - see
                  // 2019\a.pst and 2019/a.pst, or a name and its 8.3 name, as the
                  // one file they are.
                  const String canonical = PstImportPath::RelativeName(file, directory);
                  if (canonical.IsEmpty())
                     return refuse(400, "the file could not be named below the domain's PST import directory");
                  one.user = std::string(Utf8(canonical).c_str());
                  if (one.user.size() > (size_t) MigrationStore::MaximumUserLength)
                     return refuse(400, "the file's name is longer than 255 characters");
               }
               else
               {
                  if (!signsIn && one.secret.empty())
                     return refuse(400, "a migration from an IMAP server needs the source password as the secret");

                  if (signsIn && one.secret.empty())
                  {
                     const ReaderOAuth::Provider *oauth = ReaderOAuth::Find(AnsiString(provider->oauthProvider));
                     if (!linkPossible || !oauth || !ReaderOAuth::Configured(*oauth))
                        return refuse(400, "no secret was given and no sign-in link can be made: set import_oauth_redirect_base_url and register this server with the provider, or give the refresh token as the secret");
                  }
               }

               AnsiString key = AnsiString(one.user.c_str()) + "\n" + Utf8(account->GetAddress());
               key.MakeLower();
               if (!seen.insert(std::string(key.c_str())).second)
                  return refuse(400, "that mailbox is already a job of this migration, into that account");

               // Nor while another migration's job copies it there: each job
               // knows only what it wrote itself, so two at once would copy
               // every message twice.
               __int64 twin = 0;
               if (!MigrationStore::FindActiveJob(account->GetID(), Utf8ToString(one.user), twin))
                  return BuildResponse_(500, "{\"error\":\"the migrations' jobs could not be read\"}");
               if (twin > 0)
                  return refuse(409, "that mailbox is being copied into that account already, by job " + std::to_string(twin) +
                                     " of another migration; a second job at the same time would copy every message twice");
            }

            AnsiString made = "[";
            int created = 0;
            for (const Requested &one : requested)
            {
               MigrationJobRecord job;
               job.migrationId = migrationId;
               job.sourceUser = Utf8ToString(one.user);
               job.sourceSecret = Utf8ToString(one.secret);
               job.targetAccountId = one.targetAccountId;
               job.state = MigrationStore::JobQueued;

               AnsiString signInKey;
               if (signsIn && one.secret.empty())
               {
                  if (!NewSignInKey(signInKey))
                     return BuildResponse_(500, "{\"error\":\"the random generator failed\"}");
                  job.signInHash = String(Dav::Sha256Hex(signInKey));
               }

               if (!MigrationStore::CreateJob(job, job.sourceUser + _T(" -> ") + TargetAddress(one.targetAccountId)))
                  return BuildResponse_(500, "{\"error\":\"a job could not be saved; the jobs before it were made - see the error log\"}");

               AnsiString entry = JobJson(job, migration);
               if (!signInKey.IsEmpty())
                  entry = entry.Mid(0, entry.GetLength() - 1) + ",\"signin_url\":" + MigrationJob::JsonString(SignInUrl(job.id, signInKey)) + "}";

               if (created > 0)
                  made += ",";
               made += entry;
               created++;
            }
            made += "]";

            // Jobs added to a finished migration run: the migration runs again,
            // decided under the scheduler's lock (MigrationManager::JobsAdded).
            MigrationManager::Instance()->JobsAdded(migrationId);

            LOG_APPLICATION(Formatter::Format(_T("RestApi: {0} job(s) added to migration {1} ({2}) by {3}."), created, migrationId, migration.name, String(caller.identity)));

            return BuildResponse_(201, "{\"created\":" + Number(created) + ",\"jobs\":" + made + "}");
         }

      default:
         break;
      }

      // Everything below names a job of this migration.
      MigrationJobRecord job;
      if (!MigrationStore::GetJob(route.record_id, job) || job.migrationId != migrationId)
         return BuildResponse_(404, "{\"error\":\"no such job in this migration\"}");

      auto jobAgain = [&]() -> HttpResponse
      {
         MigrationJobRecord now;
         if (!MigrationStore::GetJob(route.record_id, now))
            return BuildResponse_(200, "{\"done\":true}");
         return BuildResponse_(200, JobJson(now, migration));
      };

      switch (route.kind)
      {
      case RouteMigrationJobGet:
         return BuildResponse_(200, JobJson(job, migration));

      case RouteMigrationJobReport:
         {
            // The report the job wrote at its last mark, or - for one that has
            // not run - what its row says.
            if (!job.report.IsEmpty())
               return BuildResponse_(200, Utf8(job.report));

            return BuildResponse_(200, MigrationJob::ReportJson(job, Utf8(TargetAddress(job.targetAccountId)), MigrationJob::Report()));
         }

      case RouteMigrationJobRetry:
         {
            JsonValue body;
            if (!ParseObject(requestBody, body))
               return BuildResponse_(400, "{\"error\":\"the body must be a JSON object\"}");

            static const char *const known[] = { "secret" };
            AnsiString unknown;
            if (!OnlyKnown(body, known, 1, unknown))
               return BuildResponse_(400, "{\"error\":" + MigrationJob::JsonString("unknown field: " + unknown) + "}");

            std::string secret;
            AnsiString error;
            if (!ReadText(body, "secret", secret, error))
               return BuildResponse_(400, "{\"error\":" + MigrationJob::JsonString(error) + "}");

            const bool secretGiven = body.Get("secret") != nullptr && !secret.empty();
            if (!MigrationManager::Instance()->RetryJob(job.id, secretGiven, Utf8ToString(secret), status, problem))
               return BuildResponse_(status, ErrorJson(problem));

            LOG_APPLICATION(Formatter::Format(_T("RestApi: job {0} of migration {1} retried by {2}{3}."), job.id, migrationId, String(caller.identity),
               secretGiven ? String(_T(", with a new secret")) : String()));
            return jobAgain();
         }

      case RouteMigrationJobCancel:
         if (!MigrationManager::Instance()->CancelJob(job.id, status, problem))
            return BuildResponse_(status, ErrorJson(problem));
         LOG_APPLICATION(Formatter::Format(_T("RestApi: job {0} of migration {1} cancelled by {2}."), job.id, migrationId, String(caller.identity)));
         return jobAgain();

      case RouteMigrationJobSignIn:
         {
            if (!signsIn)
               return BuildResponse_(409, "{\"error\":\"a migration from an IMAP server uses passwords, not sign-ins\"}");
            if (job.state == MigrationStore::JobDone || MigrationManager::Instance()->IsJobRunning(job.id))
               return BuildResponse_(409, "{\"error\":\"the job is running or done\"}");

            const ReaderOAuth::Provider *oauth = ReaderOAuth::Find(AnsiString(provider->oauthProvider));
            if (!oauth || !ReaderOAuth::Configured(*oauth))
               return BuildResponse_(503, "{\"error\":\"signing in with this provider is not set up on this server: register it and set import_oauth_redirect_base_url\"}");

            AnsiString signInKey;
            if (!NewSignInKey(signInKey))
               return BuildResponse_(500, "{\"error\":\"the random generator failed\"}");

            if (!MigrationStore::SaveSignInHash(job.id, String(Dav::Sha256Hex(signInKey))))
               return BuildResponse_(500, "{\"error\":\"the job could not be saved\"}");

            return BuildResponse_(200, "{\"signin_url\":" + MigrationJob::JsonString(SignInUrl(job.id, signInKey)) + "}");
         }

      default:
         break;
      }

      return BuildResponse_(404, "{\"error\":\"not found\"}");
   }

   HttpResponse
   RestApiServer::HandleMigrationSignIn_(const AnsiString &query)
   {
      const AnsiString jobText = QueryParameter_(query, "job");
      const AnsiString key = QueryParameter_(query, "key");

      const HttpResponse unknown = Page(404, "This link does not work",
         Paragraph("This sign-in link has been used already, or it is not one this server gave out. Ask the person running the migration for a new one."));

      bool digits = !jobText.IsEmpty() && jobText.GetLength() <= 18;
      for (int i = 0; digits && i < jobText.GetLength(); i++)
         digits = jobText.GetAt(i) >= '0' && jobText.GetAt(i) <= '9';
      if (!digits || key.GetLength() != 64)
         return unknown;

      MigrationJobRecord job;
      MigrationRecord migration;
      if (!MigrationStore::GetJob(_atoi64(jobText.c_str()), job) || job.signInHash.IsEmpty() ||
          !MigrationStore::GetMigration(job.migrationId, migration))
         return unknown;

      const AnsiString expected = Utf8(job.signInHash);
      const AnsiString presented = Dav::Sha256Hex(key);
      if (!SecureEquals(expected, presented))
         return unknown;

      const MigrationProvider *provider = MigrationSources::ByKind(migration.kind);
      const ReaderOAuth::Provider *oauth = provider ? ReaderOAuth::Find(AnsiString(provider->oauthProvider)) : nullptr;
      if (!provider || !oauth || !ReaderOAuth::Configured(*oauth))
         return Page(503, "Signing in is not set up", Paragraph("This server is not set up to sign in to that provider. Tell the person running the migration."));

      // Microsoft redeems a code for one resource at a time; the consent covers
      // the mail and Graph, and the code is redeemed for the mail.
      const AnsiString redeem = provider->kind == MigrationStore::KindMicrosoft ? AnsiString(provider->mailScope) + " openid email" : AnsiString();

      // The link works once: opening it spends it - the job keeps a digest of
      // its digest, so the job still waits for a sign-in and the link no longer
      // opens - and the flow carries the digest to its end, where a link since
      // replaced by a newer one is refused.
      if (!MigrationStore::SaveSignInHash(job.id, String(Dav::Sha256Hex(expected))))
         return Page(500, "Not now", Paragraph("The sign-in could not start. Try the link again in a few minutes."));

      AnsiString url, nonce, problem;
      if (!ReaderOAuth::BeginForMigration(job.id, expected, *oauth, provider->consentScope, redeem, url, nonce, problem))
      {
         MigrationStore::SaveSignInHash(job.id, String(expected));
         return Page(503, "Signing in could not start", Paragraph(problem));
      }

      AnsiString headers;
      headers.Format("Location: %s\r\nSet-Cookie: hmimport=%s; Path=/portal/oauth; HttpOnly; SameSite=Lax; Max-Age=600%s\r\nReferrer-Policy: no-referrer\r\n",
         url.c_str(), nonce.c_str(), use_tls_ ? "; Secure" : "");

      LOG_APPLICATION(Formatter::Format(_T("RestApi: the owner of {0} opened the sign-in link of migration job {1}."), job.sourceUser, job.id));

      return BuildResponse_(302, "{}", headers);
   }

   HttpResponse
   RestApiServer::CompleteMigrationSignIn_(__int64 jobId, const AnsiString &linkDigest, const String &address,
                                           const AnsiString &refreshToken, const AnsiString &provider)
   {
      MigrationJobRecord job;
      MigrationRecord migration;
      if (!MigrationStore::GetJob(jobId, job) || !MigrationStore::GetMigration(job.migrationId, migration))
         return Page(404, "The migration is gone", Paragraph("The job this sign-in was for no longer exists. Nothing was kept."));

      const MigrationProvider *kind = MigrationSources::ByKind(migration.kind);
      if (!kind || provider != kind->oauthProvider)
         return Page(400, "The wrong provider", Paragraph("This sign-in was made with a different provider from the one the migration reads. Nothing was kept."));

      // The flow has to be the one the job's newest link began: one begun from
      // a link since replaced, or finished already, attaches nothing.
      if (job.signInHash.IsEmpty() || linkDigest.IsEmpty() || !SecureEquals(Utf8(job.signInHash), Dav::Sha256Hex(linkDigest)))
      {
         return Page(409, "This link was replaced",
            Paragraph("A newer sign-in link was made for this mailbox, or this one has been used. Nothing was kept. Open the newest link you were sent."));
      }

      // The grant has to be for the mailbox the job copies: a link opened by
      // somebody signed in as somebody else attaches nothing, and the link
      // works again for the right person.
      if (job.sourceUser.CompareNoCase(address) != 0)
      {
         MigrationStore::SaveSignInHash(jobId, String(linkDigest));
         return Page(409, "Signed in as another address",
            Paragraph("This link is for " + Utf8(job.sourceUser) + ", and you signed in as " + Utf8(address) +
                      ". Nothing was kept. Open the link again and sign in as " + Utf8(job.sourceUser) + "."));
      }

      // Kept protected, and the link forgotten.
      if (!MigrationStore::SaveSecret(jobId, String(refreshToken), true))
         return Page(500, "Not saved", Paragraph("The sign-in could not be saved here. Try the link again in a few minutes."));

      // A failed or cancelled job - its owner signing in again after the grant
      // was refused - is queued again, and a finished migration runs for it.
      MigrationManager::Instance()->SignedIn(jobId);

      LOG_APPLICATION(Formatter::Format(_T("RestApi: {0} signed in for migration job {1}."), address, jobId));

      return Page(200, "Your mailbox will be copied",
         Paragraph("You signed in as " + Utf8(address) + ". Your mail, contacts and calendars will be copied to your new mailbox; you can close this page."));
   }
}
