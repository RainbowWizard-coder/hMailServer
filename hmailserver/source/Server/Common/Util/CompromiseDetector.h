// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Compromised-account detection: the shape of an account takeover, judged at
// the moment an authenticated account submits a message - SMTP AUTH's DATA and
// the webmail's send - against what that account has done before.
//
// The per-account sending limits (RateLimiter.h) are a ceiling: so many
// messages a day, whoever is sending them. What they cannot see is a change of
// shape below the ceiling - the account that sent six messages in its busiest
// hour sending forty in this one, to addresses it has never written to, at an
// hour it never sends at, from a network it has never signed in from. That is
// what a stolen password looks like from here, and it is what this judges.
//
// Four signals, each against the account's own baseline over the last
// CompromiseBaselineDays days of hm_accountactivity (one row per submission):
//
//    volume      the messages in the last hour, this one included, reach
//                CompromiseVolumeFloor and exceed CompromiseVolumeFactor times
//                the account's busiest hour in the baseline          (2 points)
//    recipients  the recipients in the last hour reach
//                CompromiseRecipientFloor and at least
//                CompromiseNewRecipientShare per cent of them are addresses
//                the account has never written to (hm_accountrecipients)
//                                                                     (2 points)
//    hour        the account has never submitted at this hour of the day (UTC)
//                in the baseline                                      (1 point)
//    address     the client's network (/24, or the /64 of an IPv6 address) has
//                not been seen for this account in the baseline       (1 point)
//
// The points add up; CompromiseScoreToLock (3) is the takeover. One new address
// is a phone on a different network; one late night is a late night; a surge
// to strangers from a strange place at a strange hour is not. An account with
// fewer than CompromiseMinimumHistory submissions in the baseline has no
// baseline, and for it only the two floors count - so a brand-new account is
// locked for a surge to strangers, which is the only shape that means anything
// without history, and for nothing subtler.
//
// CompromiseDetectionMode says what a takeover shape does. 0 records nothing
// and judges nothing. 1, WATCH, the default, records the baseline and raises
// the account.compromised alert with what would have been done, and does
// nothing else - it is how an administrator sees what enforcing would do to
// their users before it does it to them. 2, ENFORCE, does what the roadmap row
// says: the message that completed the shape is kept out of the delivery queue
// as hm_messages.messagetype 4 (Message::HeldForReview, which the deliverer's
// select never matches), the account's other messages still queued from the
// last hour are held the same way, the account is locked - AccountLockout holds
// the name until an administrator releases it, at the check every password
// logon makes (AccountLogon::Logon) and at the one the logons that prove
// identity without a password make (AccountLogon::RefusedByHold: a bearer
// token, a Kerberos ticket, a client certificate) - its browser sessions are
// ended, the alert is raised, and the account.locked webhook is raised with
// the reason. A held message is released or discarded from GET /api/v1/review
// and the Deck's "Held for review" page; one nobody has looked at in
// CompromiseHeldDays days is discarded by the retention sweep. The client that
// submitted the held message is answered as if it were queued: a takeover
// learns nothing from the reply, and a legitimate sender's mail is a review
// away from being sent. A lock refuses the next logon, not the session that is
// up, so what that session goes on submitting is held on the standing lock
// without being judged again, and an account being deleted takes its held
// mail with it. A draft the webmail scheduled for later is a submission when
// its minute comes (ScheduledMailTask), judged and counted like a send, and
// held on the standing lock like one.
//
// The alert is raised once per account per hour, whatever the mode: a surge
// that goes on scoring for the rest of its hour - every message after the
// shape completes does, since the last-hour counts stay past the floors - is
// one alert, not one per message, and hm_accountreview.arvalertedat is what
// says the hour is not up. A lock is always said, since it is a new state.
//
// Per account, hm_accountreview holds exempt (never judged), sensitivity
// (relaxed needs one point more, strict one point fewer), the lock, the last
// shape seen, and the time of the last unlock. An unlock is a fresh start:
// what the account submitted before it has been looked at, and does not count
// towards the next hour's volume or recipients, so the next message after a
// false positive is not the same shape again. The lock survives a restart:
// LoadLocks reads the locked rows back into AccountLockout at start-up and on
// every sweep.
//
// Every entry point is called from a submission or a request thread and none of
// them throws: a detector that failed would refuse nothing and hold nothing,
// which is the right failure for a control whose false positive is a person
// locked out of their mail.
#pragma once

#include <vector>

namespace HM
{
   class Account;

   class CompromiseDetector
   {
   public:
      enum Mode
      {
         ModeOff = 0,
         ModeWatch = 1,
         ModeEnforce = 2
      };

      enum Source
      {
         SourceSmtp = 1,
         SourceWebmail = 2
      };

      enum Sensitivity
      {
         SensitivityDefault = 0,
         SensitivityRelaxed = 1,
         SensitivityStrict = 2
      };

      // The thresholds, read from the settings by Assess and handed in by the
      // self-test, so the scoring can be proved without a database.
      struct Thresholds
      {
         int baseline_days = 30;
         int minimum_history = 20;
         int volume_factor = 4;
         int volume_floor = 30;
         int recipient_floor = 20;
         int new_recipient_share = 80;   // per cent
         int score_to_lock = 3;
      };

      // What the account's history says, and what this submission adds. The
      // pure input of Score.
      struct Inputs
      {
         int history = 0;              // submissions in the baseline
         int last_hour = 0;            // submissions in the last hour, this one included
         int peak_hour = 0;            // the busiest earlier hour in the baseline
         int hour_recipients = 0;      // recipients in the last hour, this message's included
         int hour_new_recipients = 0;  // of them, addresses never written to
         bool hour_seen = true;        // the account has submitted at this hour of the day before
         bool address_seen = true;     // from this network before
         int sensitivity = SensitivityDefault;
      };

      struct Verdict
      {
         bool takeover = false;        // the score reached the lock
         bool has_baseline = false;
         int score = 0;
         String signals;               // "volume, recipients, hour, address" - the ones that scored
         String reason;                // one sentence per signal, for the review page and the alert
      };

      // The scoring alone. No database, no clock: the self-test's entry point.
      static Verdict Score(const Inputs &inputs, const Thresholds &thresholds);

      // What Assess found, carried to Apply once the message id is known.
      struct Assessment
      {
         bool assessed = false;        // false: the mode is off, the account exempt, or nothing could be read
         int mode = ModeOff;
         __int64 account_id = 0;
         String account;               // the address
         String address;               // the client's
         int source = SourceSmtp;
         int hour = 0;                 // hour of the day, UTC
         __int64 bucket = 0;           // hour bucket, seconds since 1970 / 3600
         std::vector<String> recipients;      // distinct, folded
         std::vector<String> new_recipients;  // of them, never written to
         Verdict verdict;
         bool hold = false;            // enforce, and a takeover: the message goes to HeldForReview
         bool already_locked = false;  // the account was locked before this submission: held, not judged again
         __int64 hour_from = 0;        // where the last hour starts: an hour ago, or the last unlock when that is later
         bool alert_due = false;       // no account.compromised alert for this account within the hour
      };

      static int Mode();
      static Thresholds ReadThresholds();

      // Judges this submission before it is saved. hold says whether the caller
      // is to save the message as Message::HeldForReview rather than queue it.
      static void Assess(std::shared_ptr<const Account> account, const AnsiString &clientAddress,
                         const std::vector<String> &recipients, int source, Assessment &assessment);

      // After the message is saved: records the submission in the baseline,
      // remembers the recipients, and on a takeover does what the mode says -
      // holds, locks, alerts. Never called for a message that was not saved.
      static void Apply(const Assessment &assessment, __int64 messageId, const String &sender,
                        const String &recipientsText, const String &subject, __int64 size);

      struct Held
      {
         __int64 id = 0;
         __int64 message_id = 0;
         __int64 account_id = 0;
         String account;
         String sender;
         String recipients;
         String subject;
         __int64 size = 0;
         __int64 held = 0;             // seconds since 1970
         int source = SourceSmtp;
         String reason;
      };

      static bool ListHeld(std::vector<Held> &held);
      static bool ReadHeld(__int64 id, Held &held);

      // Back into the delivery queue, or gone. False when the row is not there
      // or the message could not be moved. A released message the webmail sent
      // gets its \Sent copy here (MessageUtilities::CopyToDesignatedSent), not
      // when it was held: a copy of a message that was then discarded would
      // say it was sent.
      static bool Release(__int64 id, String &problem);
      static bool Discard(__int64 id, String &problem);

      struct Review
      {
         __int64 account_id = 0;
         bool exempt = false;
         int sensitivity = SensitivityDefault;
         bool locked = false;
         __int64 locked_at = 0;
         __int64 unlocked_at = 0;      // the last unlock, seconds since 1970; 0 for never
         __int64 alerted_at = 0;       // the last account.compromised alert; 0 for never
         int score = 0;
         String signals;
         String reason;
      };

      static bool ReadReview(__int64 accountId, Review &review);
      static bool WriteReviewChoices(__int64 accountId, bool exempt, int sensitivity);

      // What the baseline holds for the account, for the review route: the
      // submissions in the window, the busiest hour, the hours of the day seen,
      // the networks seen and the recipients remembered.
      struct Baseline
      {
         int days = 0;
         int history = 0;
         int minimum_history = 0;
         int peak_hour = 0;
         int last_hour = 0;
         std::vector<int> hours;
         std::vector<String> networks;
         int recipients_known = 0;
         int held = 0;
      };

      static bool ReadBaseline(__int64 accountId, Baseline &baseline);

      // Clears the compromise lock alone: the row, and the hold on the name,
      // and records the time, which is where the next hour's counting starts.
      // The general unlock (REST and COM) calls this beside the lockout's own
      // clearing, so that one Unlock lets the person back in whichever lock it
      // was - unless the caller is an API key, which is refused this lock as it
      // is refused the review routes.
      static void Unlock(__int64 accountId, const String &address);

      // The locks the database holds, put back into AccountLockout: at start-up,
      // and on every sweep so that a lock cleared elsewhere or an account deleted
      // does not leave a name held in memory.
      static void LoadLocks();

      // An account being deleted: its held messages are removed from the queue
      // now, since nobody is left to release them, and the hold on its name is
      // let go, since account ids and addresses are reissued. Called by
      // PersistentAccount::DeleteObject while the account's rows still exist.
      static void ForgetAccount(__int64 accountId, const String &address);

      // Ends what the baseline no longer covers, discards held messages nobody
      // reviewed within CompromiseHeldDays, removes the hold rows of messages
      // that are gone and the held messages of accounts that are gone, and
      // reloads the locks. Returns the number of held messages discarded.
      static int Sweep();

      // The network an address is judged by: the /24 of an IPv4 address, the
      // /64 of an IPv6 one, the address itself when it is neither.
      static String NetworkOf(const String &address);
   };
}
