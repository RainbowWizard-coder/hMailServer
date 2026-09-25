// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// A message that expires (Gmail's confidential mode). The sender sets an
// expiry and the message itself is not sent: it is kept on this server under
// <DataDirectory>/Confidential/<account id>/<token>.eml, with a row in
// hm_confidential (schema 6069), and the recipients are sent a short notice
// with a link to <ConfidentialBaseUrl>/confidential/<token>. The reader proves
// they are a recipient - signed in to the webmail as one, or with a one-time
// code sent to one of the addresses, or with a passcode the sender gave them
// another way - and the page shows the message's text with nothing to
// download, forward, copy or print. When the expiry passes, or the sender
// withdraws it, the page refuses and the kept copy is deleted; the row stays
// thirty days more, so the sender's list still says what happened to it.
//
// What it is not, as the product says too: a reader who can see a message can
// photograph it. This stops a message being forwarded by accident or found in
// a mailbox a year later; it does not stop a determined recipient, and it is
// not end-to-end encryption - a message this server deletes on a timer is a
// message this server can read.
#pragma once

namespace HM
{
   class Account;
   class Message;
   class IMAPFolder;

   class ConfidentialMessages
   {
   public:
      struct Entry
      {
         __int64 id = 0;
         __int64 account_id = 0;
         AnsiString token;
         String subject;
         String sender;                     // the From header, as sent
         std::vector<String> recipients;    // lower case, every To, Cc and Bcc address
         String file;                       // relative to the store; empty once deleted
         AnsiString passcode_hash;          // empty when the sender set none
         __int64 created = 0;               // seconds since 1970, UTC
         __int64 expires = 0;
         __int64 opened = 0;                // the first time it was read; 0 until then
         String opened_by;                  // the address that read it, or "passcode"
         bool revoked = false;

         bool IsRecipient(const String &address) const;
         bool IsReadable(__int64 now) const { return !revoked && expires > now && !file.IsEmpty(); }
      };

      // The expiry is seconds from now: at least one, at most ninety days. The
      // webmail offers an hour, a day, a week and a month.
      static constexpr int MinimumSeconds = 1;
      static constexpr int MaximumSeconds = 90 * 24 * 60 * 60;

      // A passcode the sender sets, when they do: four to sixty-four characters.
      static constexpr int PasscodeMinimum = 4;
      static constexpr int PasscodeMaximum = 64;

      // The recipients are kept as one comma-separated column of this width.
      static constexpr int RecipientsMaximum = 2000;

      // What one account may have outstanding: the kept copies count against
      // the mailbox's quota (they are the mailbox's, kept elsewhere), and no
      // more than this many are live at once, whatever the quota.
      static constexpr int LiveMaximum = 1000;

      // The one-time codes, per message: at most this many asked for in an
      // hour, to any address, and this many wrong answers in all before codes
      // stop opening the message - the sender sends it again. A six-digit
      // code guessed five times a minute for ninety days is a coin toss;
      // thirty wrong answers in all is one chance in thirty thousand. And per
      // address, this many codes a day, so a link cannot be used to mail
      // someone a code a minute for a week.
      static constexpr int CodesPerHour = 6;
      static constexpr int WrongCodesMaximum = 30;
      static constexpr int CodesPerAddressPerDay = 5;

      // The absolute address the links are written against: ConfidentialBaseUrl,
      // or failing that the digest's or the import sign-in's base URL, without a
      // trailing slash. Empty means the feature is not offered.
      static String BaseUrl();

      // The name in a From header - Name <address> - or the address itself.
      static String DisplayName(const String &fromHeader);

      // Keeps the message at sourceFile - the full message as written for
      // sending - and makes its row; made is the row, its token the link's.
      static bool Create(std::shared_ptr<const Account> account, const String &sourceFile, const String &subject, const String &sender,
                         const std::vector<String> &recipients, __int64 expires, const String &passcode, Entry &made);

      // A row Create made that the send then failed on: the row and the copy go.
      static void Discard(const Entry &made);

      // The notice that goes to the recipients in the message's place, written
      // over fileName with the message's own From, To, Cc and Subject.
      static bool WriteNotice(std::shared_ptr<Message> message, const String &fileName, const String &fromHeader, const String &toHeader, const String &ccHeader,
                              const String &subject, const String &senderName, const AnsiString &link, __int64 expires, bool passcode);

      // The kept copy - the full message - copied into a folder as a new message
      // with the folder's account and id, the envelope sender and its size set,
      // not yet saved; for the sender's \Sent folder. Empty when it could not be.
      static std::shared_ptr<Message> CopyKeptToFolder(const String &keptPath, const String &fromAddress, std::shared_ptr<IMAPFolder> folder);

      static bool FindByToken(const AnsiString &token, Entry &entry);
      static bool List(__int64 accountId, std::vector<Entry> &entries);
      static String PathOf(const Entry &entry);

      // What the account has outstanding: the live rows (not expired, not
      // withdrawn, copy still kept), and the bytes of every copy kept for it.
      // -1 when the database did not answer.
      static int CountLive(__int64 accountId);
      static __int64 KeptBytes(__int64 accountId);

      // Withdrawn by the sender: the row says so from now on, which is what
      // the page refuses on, and the copy goes - now, or on the next sweep if
      // the delete fails, since the row keeps naming the file until it is gone.
      static bool Revoke(__int64 accountId, __int64 id);

      // An erasure of an address (AddressTraceEraser). A message somebody else
      // sent to it has the address taken out of its recipients - matched whole,
      // never inside a longer address - and out of opened_by, which then reads
      // [erased]; the other recipients go on reading it. A message the address
      // sent (its From), or one left with no recipient, is ended: withdrawn,
      // its kept copy deleted, and its row removed. The one-time codes and the
      // reading grants the address holds go too. A message whose account is
      // under a legal hold (DiscoveryHolds.h) is left as it is and counted in
      // held. The number of rows changed or removed, or -1 when the rows could
      // not be read or one could not be changed - a kept copy that cannot be
      // deleted leaves its row withdrawn, for the sweep to retry the file.
      static int EraseAddress(const String &address, int &held);

      static void MarkOpened(const Entry &entry, const String &by);

      // One-time codes, sent to a recipient's address; kept in memory for ten
      // minutes, five tries a code, one a minute per address, and the ceilings
      // above. An address the message did not go to gets the same answers and
      // the same limits - an entry with a code nothing matches - so nothing on
      // the page tells a stranger who the recipients are.
      enum CodeResult { CodeSent, CodeTooSoon, CodeTooMany, CodeClosed, CodeFailed };
      static CodeResult SendCode(const Entry &entry, const String &address);
      enum CheckResult { CheckRight, CheckWrong, CheckLocked, CheckClosed };
      static CheckResult CheckCode(const Entry &entry, const String &address, const AnsiString &code);

      // The passcode, when one was set: ten wrong in a quarter of an hour pause it.
      // Stored as a PBKDF2 hash with its own salt, so a copy of the table does
      // not give up a short passcode offline.
      static CheckResult CheckPasscode(const Entry &entry, const String &passcode);
      static AnsiString HashPasscode(const String &passcode);

      // A reading grant: what the browser holds, in a cookie scoped to the
      // message's path, once it has proved itself. Half an hour.
      static AnsiString Grant(const Entry &entry, const String &by);
      static bool Granted(const Entry &entry, const AnsiString &grant, String &by);

      // Every minute from the scheduled-mail task, and with the retention task:
      // the copies of what has expired or been withdrawn, and the rows thirty
      // days after their expiry. With orphanDirectories - the retention task
      // only - the directories of accounts that no longer exist, each decided
      // by a query that answered, never by a lookup that may have failed.
      // Nothing is touched when the database does not answer. The number of
      // copies deleted.
      static int Sweep(bool orphanDirectories);
   };
}
