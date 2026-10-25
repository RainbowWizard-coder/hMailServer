// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// ACME v2 (RFC 8555) client with http-01 challenge support, plus the
// scheduled task that drives automatic certificate renewal.
//
// Disabled by default. Enable with AcmeEnabled=1 in hMailServer.ini and
// configure AcmeContactEmail and AcmeDomains. Issued certificates are
// written as fullchain.pem / privkey.pem in AcmeCertificateDirectory.

#pragma once

#include "../BO/ScheduledTask.h"

#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

#include <openssl/ossl_typ.h>

namespace HM
{
   class AcmeClient
   {
   public:
      AcmeClient();
      ~AcmeClient();
      // The destructor frees account_key_: a copy would free it a second time.
      AcmeClient(const AcmeClient &) = delete;
      AcmeClient &operator=(const AcmeClient &) = delete;

      // When a certificate with these dates should be renewed.
      //
      // A FRACTION of its lifetime rather than a fixed number of days, which is the
      // whole point. Thirty days was right while certificates lasted ninety; it
      // stops being right the moment they do not. Let us Encrypt defaults to 64 days
      // from February 2027, the maximum falls to 100 days in March 2027 and to 47 in
      // March 2029 - and a fixed 30-day window against a 47-day certificate starts
      // renewing seventeen days after issuance and never stops, which is how an
      // operator gets rate-limited by their own CA.
      //
      // Two thirds through the lifetime is what the ACME community settled on and
      // what ARI suggested-windows approximate. It has the property that matters:
      // whatever the lifetime, a third of it is left to notice a failure.
      //
      // Public and pure so the self-test can pin it against the lifetimes the
      // industry is moving to, instead of waiting for a real certificate to age.
      static time_t GetRenewalTime(time_t notBefore, time_t notAfter);

      // The ARI certificate identifier for a certificate (RFC 9773 section 4.1):
      // base64url of the Authority Key Identifier, a dot, base64url of the serial
      // number. Public and static so the self-test can pin it against a real
      // certificate rather than against another copy of this code.
      static bool GetCertificateAriId(const String &certificateFile, AnsiString &ariId);

      // The identifier's construction, separated from reading a certificate so that
      // the self-test can pin it against the worked example RFC 9773 publishes
      // rather than against another certificate this project happens to have.
      static AnsiString BuildAriId(const unsigned char *keyIdentifier, int keyIdentifierLength,
                                   const unsigned char *serialDer, int serialDerLength);

      // Where in a CA-suggested renewal window this server should renew.
      //
      // RFC 9773 section 4.2 says to pick a random point in the window rather than
      // renewing at its start, because the whole purpose of the window is to spread
      // load - a fleet that all renews at "start" has simply moved the thundering
      // herd rather than removed it.
      //
      // The point is derived from the certificate's own identifier instead of from a
      // random number generator, and that is deliberate: this decision is re-taken
      // every hour, and a fresh roll each time would drift the effective renewal
      // towards the start of the window. Deriving it from the identity gives the
      // same answer every hour for this certificate and a different one for the next
      // server along, which is what the RFC is actually asking for.
      static time_t GetRenewalTimeInWindow(const AnsiString &ariId, time_t windowStart, time_t windowEnd);

      // Asks the CA when it would like this certificate renewed. False when the CA
      // does not implement ARI, when the request fails, or when the answer cannot be
      // read - all of which mean "decide for yourself", never "do not renew".
      bool FetchSuggestedRenewalTime_(time_t &renewAt);

      // Parses an RFC 3339 timestamp of the shape ARI responses carry
      // ("2026-01-02T15:04:05Z"). Returns 0 when it cannot be read.
      static time_t ParseRfc3339(const AnsiString &value);

      // The directory holding account.key, fullchain.pem and privkey.pem.
      static String GetCertificateDirectory();

      // Computes the DANE TLSA "3 1 1" payload (SHA-256 over the subject
      // public key info) for the first certificate in a PEM file.
      static bool GetCertificateTlsa(const String &certificate_file, AnsiString &spki_sha256_hex);

      // Runs the full issuance flow. Returns true on success. On failure the
      // step that failed, a sentence about it in this server's own words and
      // the CA's problem document, if it sent one, are in GetFailure.
      bool RequestCertificate();

      // What the last RequestCertificate that failed left: the step (directory,
      // account, order, authorization, finalize, certificate, install), a
      // sentence, the type and detail of the CA's last problem document, and
      // whether the CA answered urn:ietf:params:acme:error:rateLimited, with
      // its Retry-After in seconds (-1 when it gave none). Never the account
      // key, a private key, a nonce or the account's URL.
      struct Failure
      {
         AnsiString step;
         String sentence;
         AnsiString problem_type;
         String problem_detail;
         bool rate_limited = false;
         int retry_after_seconds = -1;
      };
      const Failure &GetFailure() const { return failure_; }

      // The dNSName entries of the subject alternative names of the first
      // certificate in a PEM file, in lower case - or, for a certificate with
      // no such extension, its common name. False when there is no file or it
      // does not hold a certificate.
      static bool ReadCertificateNames(const String &certificateFile, std::vector<AnsiString> &names);

      // The same, from a PEM text held in memory rather than in a file.
      static bool ReadCertificateNamesFromPem(const AnsiString &pem, std::vector<AnsiString> &names);

      // AcmeDomains read as RequestCertificate reads it: split at commas,
      // trimmed, in lower case, the empty ones dropped - and each name once.
      static std::vector<AnsiString> ParseDomainList(const AnsiString &list);

      // The configured names a certificate whose names are these does not
      // cover, in the order configured. A name of the certificate that starts
      // with "*." covers one label in its place, as a TLS client matches it.
      // Public and static so that the self-test pins the rule itself.
      static std::vector<AnsiString> MissingNames(const std::vector<AnsiString> &certificateNames,
                                                  const std::vector<AnsiString> &configured);

      // A Retry-After value (RFC 9110 section 10.2.3) as seconds from now:
      // delay-seconds, or an HTTP-date (the IMF-fixdate form, which is the one
      // a sender must use). -1 when it is neither; 0 for a date already past.
      static int ParseRetryAfter(const AnsiString &value, time_t now);

      // Whether a response body is an ACME problem document (RFC 8555 section
      // 6.7) of the given type - "rateLimited", say - judged by its top-level
      // "type" member.
      static bool IsProblemOfType(const AnsiString &body, const AnsiString &type);

      // Whether the certificate chain the CA issued is one to install: its
      // first certificate parses, belongs to the key the request was made
      // with, names every domain requested and has not expired. False with
      // the reason when not, and FinalizeOrder_ writes nothing. Public and
      // static so that the self-test holds each refusal.
      static bool CheckIssuedCertificate(const AnsiString &chainPem, EVP_PKEY *key,
                                         const std::vector<AnsiString> &domains, String &reason);

      // A certificate pair on disk that no "ACME (automatic)" record names is
      // deployed the way a fresh issuance is. Returns true when it did so.
      bool ApplyIssuedCertificateIfUnrecorded();

      // Whether to renew now, and why: reason is "missing" (no certificate that
      // can be read), "names" (it does not name every AcmeDomains name), "ari"
      // (the CA's suggested window has come) or "lifetime" (two thirds of its
      // life have passed), and empty when it is not due.
      //
      // A certificate that does not cover every configured name is due at once,
      // whatever its dates: a name added to AcmeDomains used to wait for the old
      // certificate's renewal, two thirds of its life, before any certificate
      // named it. That is judged from the file alone, before the CA is asked
      // anything.
      //
      // Otherwise asks the CA through ARI when it offers it, and falls back to the
      // lifetime-proportional calculation when it does not - which is most CAs
      // today, so the fallback is the normal path rather than an error path.
      //
      // Asking every hour is the intended use of ARI rather than an extravagance:
      // the reason the CA gets a say at all is that it may need to move renewals
      // forward during a mass revocation, and a client that asked once at issuance
      // would never hear about it.
      bool ShouldRenewNow(AnsiString &reason);

      // Finds a challenge whose "type" is the given value, tolerating the
      // whitespace boulder's pretty-printed JSON puts around the colon - the
      // compact-form search this replaces never matched a real Let's Encrypt
      // response (issue #34). Returns the position of the type value, inside
      // the challenge object, or -1. Public and static so the self-test can
      // pin the real CA's response shape.
      static int FindChallengeOfType(const AnsiString &json, const AnsiString &challengeType);

      // The trimmed value of a header in a response's header block - the status
      // line and the headers, cut at the blank line - found by its lower-case
      // name; empty when the block has no such header. Public and static so the
      // self-test can pin the header that comes last, which has no line break
      // after it.
      static AnsiString GetHeaderValue(const std::string &headers, const std::string &headerName);

   private:

      struct HttpResponse
      {
         int status_code = 0;
         AnsiString body;
         AnsiString nonce;
         AnsiString location;
         AnsiString retry_after;
      };

      // Records the step that failed and a sentence about it, once: the first
      // call of an attempt wins, which is the innermost step, since each caller
      // up the chain only adds that the step it called failed.
      void Fail_(const AnsiString &step, const String &sentence);

      // Reads a response for a problem document and keeps its type and detail
      // as the attempt's last problem, and a rateLimited one's Retry-After. A
      // success clears the last problem: it was answered.
      void NoteProblem_(const HttpResponse &response);

      // HTTPS transport.
      bool Transact_(const AnsiString &url, const AnsiString &method, const AnsiString &payload, HttpResponse &response);
      bool SignedPost_(const AnsiString &url, const AnsiString &payload, HttpResponse &response);

      bool FetchDirectory_();
      bool FetchNonce_();
      bool LoadOrCreateAccountKey_();
      bool RegisterAccount_();
      bool CreateOrder_(const std::vector<AnsiString> &domains, HttpResponse &orderResponse);
      bool CompleteAuthorization_(const AnsiString &authorizationUrl);
      bool FinalizeOrder_(const AnsiString &finalizeUrl, const AnsiString &orderUrl, const std::vector<AnsiString> &domains);
      void ApplyCertificate_();

      AnsiString BuildJwk_() const;
      AnsiString GetJwkThumbprint_() const;
      AnsiString SignJws_(const AnsiString &url, const AnsiString &payload, bool useJwk);

      static AnsiString Base64Url_(const unsigned char *data, int length);
      static AnsiString Base64Url_(const AnsiString &data);
      static AnsiString JsonStringValue_(const AnsiString &json, const AnsiString &key, int searchFrom = 0);

      // ACME directory endpoints.
      AnsiString url_new_nonce_;
      AnsiString url_new_account_;
      AnsiString url_new_order_;

      // Absent from the directory of a CA that does not implement ARI, which is
      // most of them today - so everything that uses it has to work without it.
      AnsiString url_renewal_info_;

      AnsiString nonce_;
      AnsiString account_url_;

      EVP_PKEY *account_key_;

      Failure failure_;
   };

   // The ACME checks of this process: the one running, the last one finished,
   // and the rules that space them out, shared by everything that starts one -
   // the hourly task, the check at start, a save of AcmeDomains, and a request
   // over REST (POST /api/v1/acme/run) or COM (Utilities.RunAcmeCheck).
   //
   // In memory, deliberately. Nothing here is configuration: it is what this
   // process has seen, and a start begins again with a check of its own, which is
   // what it always did. A rate limit the CA set is lost with it, and the next
   // order is refused by the CA with the same answer, which costs nothing
   // against its limits; it is learnt again then.
   //
   // The rules, with the reason for each number:
   //
   //   - One check at a time, whatever started it. A request while one is
   //     queued or running is answered with that one.
   //   - A requested check at most once a minute (RequestIntervalSeconds): one
   //     that finds nothing due still asks the CA for its directory and its
   //     renewal window, and a script that polls by asking for a check instead
   //     of reading the status would do that as fast as it can.
   //   - After an attempt to issue that failed, no check orders a certificate
   //     for twenty minutes (FailureCooldownSeconds), whatever started it; the
   //     hourly check, an hour after the last, is held only by a requested or
   //     saved one that failed within those minutes, and then by an hour at
   //     most. Let's Encrypt allows five failed authorizations per name per
   //     account per hour, refilled one every twelve minutes; attempts twenty
   //     minutes apart fail at most three times an hour, so a setup that cannot
   //     validate never exhausts that limit however often it is asked, and the
   //     hourly check that keeps a renewal going is never the one that finds
   //     the limit spent.
   //   - When the CA answers urn:ietf:params:acme:error:rateLimited, no check
   //     orders until its Retry-After has passed (an hour when it gives none,
   //     at most seven days, the longest window Let's Encrypt counts in), as
   //     RFC 8555 section 6.6 asks.
   //
   // The last two belong to the CA they were learnt from: a change of
   // AcmeDirectoryUrl starts them afresh.
   class AcmeChecks
   {
   public:
      enum Trigger
      {
         TriggerSchedule = 0,
         TriggerStart = 1,
         TriggerSettings = 2,
         TriggerRequest = 3
      };

      struct Attempt
      {
         __int64 id = 0;
         Trigger trigger = TriggerSchedule;
         __int64 queued_at = 0;
         __int64 started_at = 0;
         __int64 finished_at = 0;

         // While it runs: queued, deciding, then the issuance's steps (directory,
         // account, order, authorization, finalize, certificate, install).
         AnsiString step;

         // When it is over: issued, failed, not_due, waiting (it was due and the
         // spacing above kept it from ordering) or disabled.
         AnsiString outcome;

         // Why it was due: missing, names, ari or lifetime; empty when not.
         AnsiString reason;

         // For failed and waiting: the failing step, a sentence, and the CA's
         // problem document's type and detail when it sent one.
         AnsiString failed_step;
         String error;
         AnsiString problem_type;
         String problem_detail;

         // The CA it was made against (AcmeDirectoryUrl when it started), which
         // the spacing it leaves belongs to.
         AnsiString directory_url_seen;
      };

      static const int RequestIntervalSeconds = 60;
      static const int FailureCooldownSeconds = 20 * 60;
      static const int DefaultRateLimitSeconds = 60 * 60;
      static const int MaxRateLimitSeconds = 7 * 86400;

      // When the next check may order a certificate or a requested one may
      // start, as Allows judges it.
      struct Spacing
      {
         AnsiString directory_url;
         __int64 last_request_at = 0;
         __int64 failed_at = 0;
         __int64 rate_limited_until = 0;
      };

      // Whether a check may go ahead now against the CA at directoryUrl: a
      // requested one (requested true) is held by all three rules, one that would
      // order a certificate by the last two. False with until (Unix seconds) and
      // why (interval, cooldown or rate_limited) when not. Times recorded for
      // another directory URL hold nothing. Pure, for the self-test.
      static bool Allows(const Spacing &spacing, const AnsiString &directoryUrl, __int64 now, bool requested,
                         __int64 &until, AnsiString &why);

      enum RequestResult
      {
         RequestAccepted,
         RequestAlreadyRunning,
         RequestDisabled,
         RequestNoDomains,
         RequestWaiting,
         RequestNotServing
      };

      // A check asked for over REST or COM, on the calling thread, which carries
      // the actor: queued on the maintenance work queue when the rules let it,
      // and put on the audit trail when it is. attempt is the queued or running
      // check; until and why say what it waits for when RequestWaiting.
      static RequestResult Request(Attempt &attempt, __int64 &until, AnsiString &why);

      // Request, answered as POST /api/v1/acme/run answers it and
      // Utilities.RunAcmeCheck reads it: the HTTP status (202 accepted or
      // already running, 429 waiting, 409 disabled or no names, 503 not
      // serving), the JSON body, the Retry-After seconds of a 429, and the
      // sentence of a refusal.
      static int RequestAnswer(AnsiString &json, int &retryAfterSeconds, String &sentence);

      // The subscription on AcmeDomains: a saved list that names a host the
      // certificate in use does not is checked now rather than at the next hour.
      // Nothing is queued when ACME is off, the list is empty or the certificate
      // already covers it; a save while a check runs is checked after it.
      static void DomainsChanged();

      // The scheduled task's side. Begin is false when a check is already
      // running; Finish ends the check Begin started. Step publishes how far a
      // running check has got.
      static bool Begin(Trigger trigger, Attempt &attempt);
      static void Step(const AnsiString &step);
      static void Finish(Attempt attempt, bool orderedOrTried, const AcmeClient::Failure *failure);

      // Whether a check that is due may order now; when not, the attempt is
      // finished as waiting, with why and until in its sentence.
      static bool MayOrder(__int64 now, __int64 &until, AnsiString &why);

      // When the hourly check is next due: set when it is registered and as
      // each run starts; 0 when none is registered.
      static void SetNextScheduled(__int64 when);

      // GET /api/v1/acme and Utilities.GetAcmeStatus: ACME's switch, the names
      // configured, the certificate in use (names, issuer, subject, serial,
      // validity, whether it covers the names, whether a record deploys it),
      // the running check, the last one, the last one that tried to issue, the
      // next hourly one and what the spacing allows now. Read from this process and the certificate file
      // alone: the CA is not asked.
      static AnsiString StatusJson(__int64 now);

      // One attempt as the status and the run answer it.
      static AnsiString AttemptJson(const Attempt &attempt);

      static const char *TriggerWord(Trigger trigger);

   private:
      static bool Queue_(Trigger trigger, Attempt &attempt);

      static std::mutex mutex_;
      static bool running_;
      static bool recheck_;
      static Attempt current_;
      static Attempt last_;
      static bool has_last_;

      // The last check that tried to issue - issued or failed - which an hourly
      // check that found nothing due does not push out of sight.
      static Attempt last_issuance_;
      static bool has_last_issuance_;
      static __int64 next_id_;
      static __int64 last_issued_at_;
      static __int64 next_scheduled_at_;
      static Spacing spacing_;
   };

   // Process-wide store of pending http-01 challenges. Written by
   // AcmeClient and served either by the transient AcmeChallengeServer
   // or by the always-on WebServicesServer when it owns the HTTP port.
   class AcmeChallengeStore
   {
   public:
      static void Set(const AnsiString &token, const AnsiString &key_authorization);
      static bool Get(const AnsiString &token, AnsiString &key_authorization);
      static void Clear();

   private:
      static boost::recursive_mutex mutex_;
      static std::map<AnsiString, AnsiString> challenges_;
   };

   // Challenge listener: a minimal HTTP server that answers
   // /.well-known/acme-challenge/<token> during validation.
   class AcmeChallengeServer
   {
   public:
      AcmeChallengeServer();
      ~AcmeChallengeServer();

      bool Start(int port);
      void Stop();

      void SetChallenge(const AnsiString &token, const AnsiString &keyAuthorization);

   private:
      void Run_();

      // One accepted connection, served to its end and closed. Run_ gives each
      // its own short-lived thread, so a client that connects and sends nothing
      // holds only that thread for its receive timeout and never the accept
      // loop - which is what lets the CA's own request be answered while an idle
      // client waits (W-11). It reads challenges_ under mutex_, as Run_ did.
      void ServeConnection_(SOCKET clientSocket);

      SOCKET listen_socket_;
      std::thread worker_;
      bool running_;

      boost::recursive_mutex mutex_;
      std::map<AnsiString, AnsiString> challenges_;

      // The connections being served right now, each on a detached thread.
      // Capped so a flood of idle connections cannot start threads without
      // bound; past the cap Run_ drops a connection rather than serve it on the
      // accept loop. Stop waits for the count to reach zero before it releases
      // the socket, so a detached worker never touches this object once it is
      // gone. Guarded by active_mutex_; active_idle_ is signalled when it falls.
      int active_connections_;
      std::mutex active_mutex_;
      std::condition_variable active_idle_;
   };

   // The rules an ACME check is decided by, held without a server, a CA or a
   // file: which names a certificate covers (a wildcard for one label), how
   // AcmeDomains is read, Retry-After in both its forms, a problem document's
   // type, the spacing of AcmeChecks::Allows, and the refusals of
   // AcmeClient::CheckIssuedCertificate against certificates and keys it makes
   // itself. Run by ClassTester on Windows and by acme-tests on Linux; throws
   // std::logic_error with the sentence.
   class AcmeChecksTester
   {
   public:
      void Test();
   };

   class AcmeRenewalTask : public ScheduledTask
   {
   public:
      // What started it, for the status: the hourly registration (the
      // default), the check at start, a save of AcmeDomains or a request.
      explicit AcmeRenewalTask(AcmeChecks::Trigger trigger = AcmeChecks::TriggerSchedule);
      ~AcmeRenewalTask();

      virtual void DoWork();

   private:
      // The check, inside AcmeChecks::Begin and Finish.
      void Check_(AcmeChecks::Attempt &attempt);

      AcmeChecks::Trigger trigger_;
   };
}
