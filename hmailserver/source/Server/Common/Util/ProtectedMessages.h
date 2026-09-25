// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// A password-protected message to anyone (Roadmap2 row 152). For the
// correspondent who has no key: the sender sets a password in the webmail,
// the page encrypts the text and the attachments into ONE OpenPGP message -
// symmetric, a passphrase and no key - and hands this server the ciphertext.
// What goes out by SMTP is a short notice from the sender with a link to a
// reading page here; the recipient types the password there and their browser
// decrypts. A reply written on that page is encrypted under the same password
// and comes back the same way, and the sender is told.
//
// What this promises, and all it promises: the plaintext is never in the
// message that crosses the internet and never rests on this server. This
// server stores ciphertext under <DataDirectory>/Protected/<account id>/
// <token>.asc with a row in hm_protectedmessages (schema 6082), and it files
// only what has the SHAPE of an OpenPGP message encrypted to a passphrase -
// one or more symmetric-key encrypted session keys and then one
// integrity-protected body, nothing else - because that is the one thing
// about the blob it can read: it cannot tell ciphertext from noise, but it
// can tell either from a plaintext, a literal packet or a message to a key,
// and it refuses those. It does NOT promise that the recipient cannot copy or
// forward what they read: a page that shows somebody a text has shown it to
// them, and the compose form and docs/OpenPGP.md say so in one sentence.
//
// The link is the whole access control. The token is 128 bits from the
// system's random source, written as 22 characters of base64url, and the
// reading page's fetch takes no credential: whoever holds the link fetches the
// ciphertext, and whoever holds the password reads it. An unguessable name is
// what a capability link is; the fetch is rate-limited per address anyway, so
// a scraper hammering the route is answered with a sentence rather than
// served. A token that names nothing is a plain 404 and counts for nothing
// else - a link is fetched by a page on load, with no human action, and a
// link older than its row is exactly such a token, so counting it as a
// failed logon would ban the reader of a stale link from the whole server.
//
// The bytes arrive in chunks, the way a file sent as a link does
// (RestApiFiles.cpp): a record first, then the ciphertext appended at the
// offset the record has reached, and the record is complete - and the notice
// goes - when the declared size is reached and the blob proves to be what it
// says it is. A record that is never finished goes after a day. The ceilings
// are 25 MB a message, 250 MB an account across everything it has outstanding,
// and a thousand live messages, each refused with a sentence; a reply is text
// written on the reading page and is at most 1 MB.
#pragma once

namespace HM
{
   class Account;
   class Message;
   class IPAddress;

   class ProtectedMessages
   {
   public:
      struct Entry
      {
         __int64 id = 0;
         __int64 account_id = 0;       // the owner: the sender, and for a reply the ORIGINAL's sender
         AnsiString token;             // 22 characters of base64url, 128 bits
         String recipient;             // the address the sender named; for a reply, the address that wrote it
         String hint;                  // what the sender said about the password, or empty
         String file;                  // relative to the store; empty once deleted
         __int64 size = 0;             // bytes declared
         __int64 stored = 0;           // bytes received so far
         bool complete = false;        // the size was reached and the blob is an OpenPGP symmetric message
         __int64 created = 0;          // seconds since 1970, UTC
         __int64 expires = 0;
         __int64 opened = 0;           // the first time the ciphertext was fetched; 0 until then
         bool revoked = false;
         AnsiString reply_to;          // the original's token when this row is a reply; empty otherwise

         bool IsReply() const { return !reply_to.IsEmpty(); }
         bool IsReadable(__int64 now) const { return complete && !revoked && expires > now && !file.IsEmpty(); }
      };

      // The ceilings, each answered with a sentence by the route that meets it.
      static constexpr __int64 MessageMaximum = 25LL * 1024 * 1024;     // one message, ciphertext as stored
      static constexpr __int64 AccountMaximum = 250LL * 1024 * 1024;    // everything an account has outstanding
      static constexpr int LiveMaximum = 1000;                          // live messages an account may have
      static constexpr __int64 ReplyMaximum = 1LL * 1024 * 1024;        // a reply, written as text on the reading page
      static constexpr int RepliesPerMessage = 20;                      // replies one original may collect
      static constexpr int HintMaximum = 255;
      static constexpr int TokenLength = 22;

      // How long a message lives when the sender says nothing: the
      // ProtectedMessageDays setting, 1 to 365 days, 30 by default. A sender
      // may ask for less, never for more.
      static int DefaultDays();

      // The absolute address the links are written against - the same chain
      // the messages that expire use (ConfidentialBaseUrl, then the digest's,
      // then the import sign-in's). Empty means the feature is not offered.
      static String BaseUrl();
      static AnsiString LinkFor(const AnsiString &token);

      static bool IsToken(const AnsiString &value);

      // The record, before any byte of the ciphertext: made with its token, its
      // expiry and no file. replyTo is the original's token for a reply and
      // empty otherwise. False when the row could not be written.
      static bool Create(__int64 accountId, const String &recipient, const String &hint, __int64 size, __int64 expires,
                         const AnsiString &replyTo, Entry &made);

      // The ciphertext, appended at the end of what is stored so far. The
      // route has checked the offset against the row; this checks it again
      // against the FILE, under the lock, because two chunks at one offset
      // that both passed the route's check would both be appended and the
      // whole upload discarded at the end. Behind is that second chunk, with
      // updated.stored the size the file really has; Appended moves the
      // row's count; Failed is a file or a row that could not be written.
      enum AppendResult { Appended, Behind, Failed };
      static AppendResult Append(const Entry &entry, const AnsiString &bytes, Entry &updated);

      // What a stored blob has to be: an ASCII-armoured PGP MESSAGE whose
      // radix-64 decodes, whose checksum matches, and whose packets are
      // exactly the shape the page writes - one or more symmetric-key
      // encrypted session keys (tag 3, each at most 128 bytes) and then ONE
      // integrity-protected data packet (tag 18) that runs to the end. A
      // public-key session key, a literal packet, an unprotected data packet
      // or anything after the body is refused by name. False with a sentence
      // in problem otherwise.
      static bool IsSymmetricMessage(const AnsiString &text, AnsiString &problem);

      // Reads the whole stored file and, when it is what it says it is, marks
      // the row complete. False with a sentence when it is not, in which case
      // the row and the file are discarded: nothing that is not ciphertext is
      // kept here for a moment longer than it takes to look at it.
      static bool Complete(const Entry &entry, Entry &completed, AnsiString &problem);

      static bool ReadFile(const Entry &entry, AnsiString &text);

      static bool FindByToken(const AnsiString &token, Entry &entry);
      static bool FindById(__int64 accountId, __int64 id, Entry &entry);
      static bool List(__int64 accountId, std::vector<Entry> &entries);
      static String PathOf(const Entry &entry);

      // What the account has outstanding: the live rows (not expired, not
      // withdrawn), and the bytes DECLARED for every row that is not expired
      // and not withdrawn, finished or not - a record counts from the moment
      // it is made, so a thousand declared uploads cannot outrun the ceiling.
      // -1 when the database did not answer.
      static int CountLive(__int64 accountId);
      static __int64 HeldBytes(__int64 accountId);
      static int CountReplies(const AnsiString &originalToken);

      // The row and the file, gone: a record whose upload failed, or whose
      // blob proved not to be ciphertext.
      static void Discard(const Entry &entry);

      // Withdrawn by the sender: the row says so from now on, which is what
      // the fetch refuses on, and the file goes - now, or on the next sweep if
      // the delete fails, since the row keeps naming the file until it is gone.
      static bool Revoke(__int64 accountId, __int64 id);

      // An erasure of an address (AddressTraceEraser): every message to it, and
      // every reply it wrote - the rows whose recipient is the address - ended
      // as a withdrawal ends one, the ciphertext deleted and the row removed,
      // under the upload's lock. A message the address's own account sent is
      // not reached: the row names no sender, and goes with its account.
      // A message whose account is under a legal hold (DiscoveryHolds.h) is
      // left as it is and counted in held. The number removed, or -1 when the
      // rows could not be read or one could not be ended - ciphertext that
      // cannot be deleted leaves its row withdrawn, for the sweep to retry.
      static int EraseAddress(const String &address, int &held);

      static void MarkOpened(const Entry &entry);

      // The notice a recipient gets in the message's place, from the sender:
      // the sentence, the hint and the link, nothing else. Written over the
      // file the queued message will be read from; the row is the caller's to
      // save, as the send route saves its own.
      static bool WriteNotice(std::shared_ptr<Message> message, const String &fileName, const String &fromHeader, const String &toAddress,
                              const String &senderName, const Entry &entry, const AnsiString &link);

      // The short internal message the original's sender gets when a reply
      // arrives: from the domain's postmaster, to the sender, with the link to
      // the reply. Queued and submitted here; false when it could not be.
      static bool SendReplyNotice(const Entry &original, const Entry &reply, const AnsiString &link);

      // The per-address ceiling on the routes that take no credential - the
      // fetch and the reply - so that a scraper is answered with a sentence.
      // True when this request may go ahead. Loopback is exempt, as it is
      // from the failed-logon count: a process on this machine is not a
      // scraper, and the regression suite is one.
      static bool AllowUnauthenticated(const IPAddress &peer);
      // The budgets, emptied: the listener stopping is the end of what they
      // were counting.
      static void ForgetBudgets();

      // Every minute from the scheduled-mail task, and with the retention
      // task: the files of what has expired or been withdrawn, the records
      // never finished after a day, and the rows thirty days after their
      // expiry. With orphanDirectories - the retention task only - the
      // directories of accounts that no longer exist, each decided by a query
      // that answered, never by a lookup that may have failed. Nothing is
      // touched when the database does not answer. The number of files
      // deleted.
      static int Sweep(bool orphanDirectories);
   };
}
