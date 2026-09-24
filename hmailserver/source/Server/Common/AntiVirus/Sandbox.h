// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Attachments opened somewhere safe (docs/Sandbox.md).
//
// ClamAV says whether a file is known to be bad. What an unknown file does when
// it is opened is a different question, and the answer takes a sandbox and
// minutes. This is the route to one: a message that no signed-in user of this
// server submitted, carrying an attachment of a submitted type (SandboxTypes -
// the executable and document set by default), is HELD - hm_messages.
// messagetype 5 (Message::HeldForSandbox), which the deliverer's select never
// matches - while its files are POSTed to SandboxSubmitUrl by the poller, and
// until SandboxVerdictUrl has answered for every file.
//
// A sandbox is an HTTP service the administrator names, not a vendor
// integration. The contract is small on purpose so that an adapter of a dozen
// lines maps any product onto it:
//
//    POST SandboxSubmitUrl       multipart/form-data: file, filename, sha256,
//                                message_id (this server's queue id);
//                                Authorization: Bearer SandboxToken when set.
//                                Answers {"id":"..."} (at most 1000
//                                characters); any 2xx is accepted and an answer
//                                without an id is polled by sha256.
//    GET  SandboxVerdictUrl      ?id=<id> (or ?sha256=<hex> when the submit
//                                answered no id); {id} and {sha256} in the URL
//                                are replaced instead when present, {id} by the
//                                hash when there is no id. Answers
//                                {"state":"pending|done","verdict":"clean|
//                                malicious|suspicious|error","score":0-100,
//                                "detail":"..."}; suspicious needs a numeric
//                                score, and without one is an error.
//
// The hold is not the quarantine and not the virus scanner's fail-closed hold.
// It keeps the message out of every mailbox, off the queue's delivery attempts,
// out of the quarantine and its digest, and visible in GET /api/v1/queue as
// held: sandbox and in GET /api/v1/sandbox/held with its submissions. The
// delivery thread only records the hold; a thread of its own (Start/Stop)
// submits the files and asks the verdict URL every SandboxPollSeconds, within
// a time budget per pass, and the hold ends SandboxTimeoutMinutes after it
// began whatever the sandbox is doing.
//
// The decision is recorded on the hold's rows and the message is put back in
// the queue; the deliverer, reaching it again, acts on it (Examine):
//
//    clean for every file    delivered, X-hMailServer-Sandbox: clean (<n> file(s), <s>s)
//    malicious, or suspicious at or above SandboxScoreThreshold
//                            QuarantineStore::Quarantine with the verdict's
//                            detail as the reason, or returned to the sender
//                            with 5.7.1 while the quarantine is off; the
//                            quarantine's release delivers without a second
//                            submission (FlagReleasedFromSandboxQuarantine)
//    error on every file that is not clean, once all have answered, or the
//    timeout                 SandboxTimeoutAction: 0 delivered with a header
//                            saying so, 1 returned to the sender with 5.7.1
//    released                by an administrator, without the verdict; the
//                            header says released by administrator
//
// Verdicts are remembered by SHA-256 in hm_sandboxverdicts for
// SandboxCacheHours, clean and malicious alike, so a file already answered is
// not submitted again; an error is never remembered.
//
// Exempt, and nothing else is: what a signed-in user of this server submitted
// (Message::FlagSubmittedBySignedInUser) while SandboxInternalMail is off; a
// message whose attachments are all outside the types; a message the
// anti-virus already deleted (it never reaches here); and a message a person
// released from the quarantine the sandbox's own verdict put it in. A message
// that cannot be read, one with more files to submit than SandboxMaxFiles,
// and one whose hold cannot be recorded are not exempt: SandboxTimeoutAction
// decides them, with a header saying why. An attachment larger than
// SandboxMaxSizeKB is not submitted and the header says so. Links are not
// detonated, only files; an archive is submitted whole; a forwarded message
// (message/rfc822) is opened, five levels deep, and the level past that is
// submitted whole.
#pragma once

#include <functional>
#include <vector>

namespace HM
{
   class Message;

   class Sandbox
   {
   public:
      // hm_sandboxholds.shdecision, the message-level outcome, the same on every
      // row of one held message.
      enum Decision
      {
         DecisionPending = 0,
         DecisionClean = 1,
         DecisionMalicious = 2,
         DecisionError = 3,
         DecisionTimedOut = 4,
         DecisionReleased = 5,
         DecisionTurnedOff = 6
      };

      // hm_sandboxholds.shstate, one file's progress.
      enum FileState
      {
         FileNotSubmitted = 0,   // the POST has not been accepted yet: the poller submits it
         FileSubmitted = 1,      // accepted, the verdict not in
         FileAnswered = 2        // the verdict is in (or came from the cache)
      };

      // What the deliverer is to do with a message it has just asked about.
      enum Outcome
      {
         OutcomeContinue = 0,    // deliver; any header has been written
         OutcomeHeld = 1,        // the message is held (messagetype 5, unlocked): keep it, delete nothing
         OutcomeQuarantine = 2,  // quarantine with reason and score, then drop the queue row
         OutcomeBounce = 3       // return to the sender with 5.7.1 and reason, then drop the queue row
      };

      struct Result
      {
         Result() : outcome(OutcomeContinue), score(0) {}

         Outcome outcome;
         int score;
         String reason;
      };

      // One submitted file of a held message, as hm_sandboxholds keeps it.
      struct File
      {
         File() : id(0), message_id(0), size(0), held(0), state(FileNotSubmitted), score(0), errors(0), decision(DecisionPending) {}

         __int64 id;
         __int64 message_id;
         AnsiString sha256;
         String filename;
         __int64 size;
         String submission;      // the id the submit answered; empty until it did
         __int64 held;           // when the hold began, seconds since 1970
         int state;
         String verdict;         // clean, malicious, suspicious or error
         int score;
         String detail;
         int errors;             // failed requests for this file
         int decision;
      };

      // One held message, for GET /api/v1/sandbox/held and GET /api/v1/queue.
      struct Held
      {
         Held() : message_id(0), held(0), decision(DecisionPending), locked(false), tries(0) {}

         __int64 message_id;
         String from;
         String recipients;      // comma separated
         String created;
         __int64 held;           // seconds since 1970; 0 when no row says
         int decision;
         bool locked;
         int tries;
         std::vector<File> files;
      };

      // The deliverer's one call, after the anti-virus scan and before the
      // rules and local delivery. Never throws.
      static Result Examine(std::shared_ptr<Message> message);

      // Puts a message whose quarantine failed back on hold, its decision kept,
      // so that the next poll releases it and the quarantine is tried again.
      static bool Rehold(std::shared_ptr<Message> message);

      // The rows of a message the deliverer has finished with.
      static void Forget(__int64 messageId);

      // The held messages, oldest first: count of them from offset, and the
      // number held in all. Three queries a page, whatever its length.
      static bool ListHeld(int offset, int count, std::vector<Held> &held, int &total);
      static bool ReadHeld(__int64 messageId, Held &held);

      // The sender, the recipients and the files of a held message in one
      // sentence, for the audit trail.
      static String Describe(const Held &held);

      // An administrator's two decisions. Release delivers without the verdict
      // (the header says so); Discard deletes the message and tells nobody.
      // problem is "not held" when the message is not held for the sandbox,
      // and "decided" when a verdict decided it between the ask and the
      // release - the message is then on its way as the verdict says.
      static bool Release(__int64 messageId, String &problem);
      static bool Discard(__int64 messageId, String &problem);

      // The poller: a thread of its own, started with the servers. Stop joins
      // it - a request in flight ends within its own timeout - calling progress
      // about once a second while it waits, so a service stop is seen to move.
      static void Start();
      static void Stop(const std::function<void()> &progress = std::function<void()>());

      // One pass of the poller. Public for the thread and for nothing else.
      static void PollOnce();

      // The checks every surface refuses a value with, in the same sentence.
      // Empty means acceptable.
      // settingName is the name the caller's surface uses (sandbox_submit_url
      // over REST, SandboxSubmitUrl over COM), which begins the sentence.
      static String UrlProblem(const String &url, const String &settingName);

      // Parses the list - extensions and media types, separated by commas,
      // semicolons or white space; a leading "." or "*." is dropped - and
      // writes it out as canonical: lower case, comma separated, each once.
      static String TypesProblem(const String &types, const String &settingName, String &canonical);

      // The switch may not be saved on while either address is empty: a
      // message would be held with nowhere to send its files. The addresses
      // are the ones the save would leave, trimmed.
      static String SwitchProblem(bool enabled, const String &submitUrl, const String &verdictUrl, const String &settingName);

      // A token the store can hold and a header can carry: visible ASCII only
      // (no space, CR or LF, which would end the Authorization header), at
      // most 2048 characters, and short enough, once protected, for
      // hm_settings.settingstring - which the legacy Blowfish scheme, used
      // while ProtectStoredSecretsWithDPAPI is 0, makes 248 characters.
      static String TokenProblem(const String &token, const String &settingName);

      // Whether a quarantine entry's reason is one this unit wrote - the mark
      // QuarantineStore's release sets FlagReleasedFromSandboxQuarantine by.
      static bool IsSandboxQuarantineReason(const String &reason);

      // The words a decision is spoken with over REST.
      static const char *DecisionWord(int decision);
      static const char *FileStateWord(int state);

      // The default list, the one the create scripts seed.
      static const TCHAR *DefaultTypes;

   private:
      static void WorkerFunc_();
      static void WorkerLoop_();
   };
}
