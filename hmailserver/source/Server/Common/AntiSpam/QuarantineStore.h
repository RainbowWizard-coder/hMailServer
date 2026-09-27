// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The spam quarantine: a store for messages the server would otherwise have
// refused, so that a human can look before they are gone.
//
// Until now the two outcomes were "mark it" and "refuse it", and refusing
// happens during the SMTP conversation with a 550 - the message is never
// accepted, so there is nothing to review and nothing to release. That is a
// perfectly defensible design and it is what most of this server still does.
// What it cannot do is let an administrator find the invoice that scored 12
// because somebody's newsletter host got listed.
//
// Quarantining is therefore a DIFFERENT decision, not a tidier one, and the
// difference is worth stating plainly:
//
//   * The message is ACCEPTED. The sender is told 250 and believes it was
//     delivered, which is exactly the outcome a false positive needs - no
//     backscatter, no bounce to a forged return path - and exactly the outcome
//     a true positive does not deserve. That is the trade.
//   * The server now stores mail it considers spam, so the store is bounded by
//     a retention sweep rather than left to grow.
//   * Nothing is released automatically. A release is an administrator saying
//     "this was wrong", and it delivers straight to the mailbox rather than
//     back through the filters that quarantined it in the first place.
//
// Only POST-transmission verdicts can be quarantined, and that is not a
// limitation to be fixed later - it is arithmetic. A pre-transmission verdict
// (a blacklisted connecting IP, a bad HELO) is reached before DATA, when there
// is no message to hold; refusing at that point is also strictly cheaper for
// everyone, since the body never crosses the wire. Those stay refusals.
//
// Off by default. An installation that has been refusing spam for years must
// not silently start accepting and storing it because of an upgrade.

#pragma once

namespace HM
{
   class Message;

   struct QuarantinedMessage
   {
      __int64 id = 0;
      String file_name;      // relative to the quarantine root
      String sender;
      String recipients;     // comma separated, as accepted
      String subject;
      String reason;
      int score = 0;
      int size = 0;
      String created;

      // The HELO or EHLO name the client gave in the SMTP session that brought
      // the message in (schema 6105). Empty for a message held before that
      // step, and for one that did not arrive over SMTP.
      String helo;
   };

   class QuarantineStore
   {
   public:
      static bool GetEnabled();

      // Copies the message's file into the store and records it. Returns false if
      // anything failed, in which case the caller must fall back to refusing the
      // message: quietly accepting mail that was not actually stored would turn a
      // spam refusal into silent deletion, which is the one outcome nobody wants.
      //
      // The HELO name is taken from the Received header this server wrote when
      // the message arrived (HeloFromReceivedHeader): a hold made at delivery -
      // a rule, a block list, an account's own threshold, the sandbox - happens
      // long after the SMTP session has gone.
      static bool Quarantine(std::shared_ptr<Message> message, const String &reason, int score);

      // The same, for a caller who already knows where the message file is. The
      // three-argument form resolves the file the way the SMTP conversation needs
      // it resolved - without an account, which lands on the queue or public
      // folder - and that is wrong for an account-level delivery copy, whose file
      // has already moved into the recipient's own folder. Local delivery's
      // per-account delete threshold passes that path in here.
      static bool Quarantine(std::shared_ptr<Message> message, const String &reason, int score, const String &sourceFile);

      // The hold the SMTP session makes itself, which knows the HELO name the
      // client gave and records it as it was given.
      static bool QuarantineFromSession(std::shared_ptr<Message> message, const String &reason, int score, const String &heloName);

      static std::vector<QuarantinedMessage> List(int maxCount);
      static bool GetById(__int64 id, QuarantinedMessage &out_message);

      // What a reviewer may read of a held message without releasing it: the
      // header block as it is stored, and a plain-text preview - the text part,
      // decoded, or failing that the HTML part with its markup taken out. The
      // HTML is never rendered anywhere. Each is cut at its limit, and
      // out_truncated says so. False, with the reason, when there is no such
      // entry or its file cannot be read.
      static constexpr int MaxHeaderCharacters = 65536;
      static constexpr int MaxPreviewCharacters = 16384;
      static bool ReadHeaders(__int64 id, String &out_headers, bool &out_truncated, String &out_error);
      static bool ReadPreview(__int64 id, String &out_text, bool &out_truncated, bool &out_fromHtml, String &out_error);

      // The HELO name in the Received header this server writes for a message it
      // receives over SMTP ("from <helo> (<ptr> [<address>]) by <this computer>
      // ..."), when localComputerName is the host the header says received it.
      // Empty for somebody else's Received header, and for one written for a
      // client that gave no HELO, whose place the server fills with the address.
      static String HeloFromReceivedHeader(const String &receivedValue, const String &localComputerName);

      // The HTML part's text, for the preview: markup, style sheets and scripts
      // taken out and the entities a reader would see unwrapped.
      static String TextOfHtml(const String &html);

      // Delivers a quarantined message to its original recipients and removes it
      // from the store. Delivery is direct rather than back through the filters:
      // a release is an administrator overruling those filters, and re-running them
      // would simply quarantine it again.
      static bool Release(__int64 id, String &out_error);

      // True when the address is one of the message's recorded recipients,
      // whole and case-insensitively.
      static bool IsRecipient(const QuarantinedMessage &message, const String &address);

      // Release for one recipient only: delivered to that address, which then
      // leaves the entry; the entry stays for the other recipients and goes
      // when it was the last.
      static bool ReleaseTo(__int64 id, const String &recipient, String &out_error);

      // One recipient gives up its copy: the address leaves the entry, which
      // goes when it was the last. Nothing is delivered.
      static bool DiscardFor(__int64 id, const String &recipient);

      static bool Delete(__int64 id);

      // Removes everything older than QuarantineRetentionDays. Returns the number
      // deleted. Zero days means never, which is a real answer here: an
      // administrator who wants to keep the evidence should not have it swept.
      static int DeleteExpired();

      static int GetCount();

      // The directory holding quarantined message files. Created on demand.
      static String GetQuarantineDirectory();

   private:
      static bool Quarantine_(std::shared_ptr<Message> message, const String &reason, int score, const String &sourceFile,
                              bool heloKnown, const String &heloName);
      static String HeloOfStoredCopy_(const String &file);
      static bool OpenFile_(__int64 id, String &out_file, String &out_error);
      static String BuildRelativePath_();
      static bool ReadRow_(std::shared_ptr<DALRecordset> recordset, QuarantinedMessage &out_message);
      static bool RemoveRecipient_(__int64 id, const QuarantinedMessage &quarantined, const String &recipient);
   };
}
