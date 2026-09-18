// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "Journaling.h"

#include "File.h"
#include "FileUtilities.h"
#include "GUIDCreator.h"
#include "Time.h"
#include "Unicode.h"

#include "../BO/Account.h"
#include "../BO/Domain.h"
#include "../BO/JournalRule.h"
#include "../BO/JournalRules.h"
#include "../BO/Message.h"
#include "../BO/MessageRecipient.h"
#include "../BO/MessageRecipients.h"
#include "../Cache/CacheContainer.h"
#include "../Mime/Mime.h"
#include "../Persistence/PersistenceMode.h"
#include "../Persistence/PersistentJournalRule.h"
#include "../Persistence/PersistentMessage.h"

#include "../../SMTP/RecipientParser.h"
#include "../../SMTP/SMTPDeliveryManager.h"

#include <map>
#include <mutex>
#include <set>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // The cache. One read of a table an administrator writes a row at a time
      // at once, kept until something changes it, because the alternative is a
      // query for every accepted message.
      std::mutex rule_mutex;
      bool need_refresh = true;
      std::vector<std::shared_ptr<JournalRule> > rules;
      time_t last_refresh_report = 0;

      String Lower(const String &value)
      {
         String lowered = value;
         lowered.ToLower();
         return lowered;
      }

      AnsiString Utf8(const String &value)
      {
         AnsiString utf8;
         Unicode::WideToMultiByte(value, utf8);
         return utf8;
      }

      // The mark this process puts on its own journal messages, and the only
      // value the loop guard honours. Made when it is first wanted and kept for
      // the life of the process; written nowhere, which is why a restart forgets
      // it - Journaling.h says what that costs.
      std::mutex mark_mutex;
      AnsiString process_mark;

      // GUIDCreator answers with the braced, hyphenated form on both platforms.
      // What is wanted here is the thirty-two hexadecimal digits by themselves:
      // RFC 2046's bcharsnospace has no place for a brace, so a MIME boundary
      // built from the braced form is not a legal boundary, and a header value
      // reads better without them.
      AnsiString HexOfGuid()
      {
         const AnsiString guid = Utf8(GUIDCreator::GetGUID());

         AnsiString hex;

         for (int i = 0; i < guid.GetLength(); i++)
         {
            const char character = (char) guid[i];

            if ((character >= '0' && character <= '9') ||
                (character >= 'a' && character <= 'f') ||
                (character >= 'A' && character <= 'F'))
            {
               const char single[2] = { character, 0 };
               hex += single;
            }
         }

         return hex;
      }

      // Empty only if the platform could not give a GUID at all, in which case
      // the next caller tries again rather than the process settling on a mark
      // that is no mark. An empty mark makes the guard answer "not ours" to
      // everything, which is the safe way round: a report that comes back is
      // journaled again rather than a stranger's header switching the archive off.
      AnsiString ProcessMark()
      {
         std::lock_guard<std::mutex> guard(mark_mutex);

         if (process_mark.IsEmpty())
            process_mark = HexOfGuid();

         return process_mark;
      }

      // A value going into a header of a message this server writes, with
      // everything that could end the field or the header block taken out. The
      // journal address is checked when the rule is written, so for that this is
      // the second line of the same defence; the subject is the sender's and has
      // no other line at all.
      //
      // The tab stays, because a tab is white space inside a header field body
      // and a folded subject arrives with one: taking it out would run two words
      // of somebody's subject together to no purpose.
      AnsiString HeaderSafe(const AnsiString &value)
      {
         AnsiString out;

         for (int i = 0; i < value.GetLength(); i++)
         {
            const unsigned char character = (unsigned char) value[i];

            if ((character < 0x20 && character != '\t') || character == 0x7F)
               continue;

            const char single[2] = { (char) character, 0 };
            out += single;
         }

         return out;
      }

      // Where a line about a journal decision goes. An SMTP session has a log of
      // its own and an id to write under; a fetch account's download has neither,
      // so its lines go to the application log rather than to session zero, where
      // nobody reading an SMTP conversation would know what they were.
      void LogLine(unsigned int sessionId, const String &connectingAddress, const String &text)
      {
         if (sessionId == 0)
         {
            LOG_APPLICATION(text);
         }
         else
         {
            LOG_SMTP(sessionId, connectingAddress, text);
         }
      }

      // The address a rule's journal goes to, and an envelope recipient, compared
      // the way addresses are compared everywhere else here: trimmed and lowered.
      String Canonical(const String &address)
      {
         String value = address;
         value.Trim();
         return Lower(value);
      }

      AnsiString JsonEscaped(const String &value)
      {
         const AnsiString utf8 = Utf8(value);
         AnsiString out;

         for (int i = 0; i < utf8.GetLength(); i++)
         {
            const unsigned char character = (unsigned char) utf8[i];
            const char single[2] = { (char) character, 0 };

            switch (character)
            {
            case '"':
               out += "\\\"";
               break;
            case '\\':
               out += "\\\\";
               break;
            case '\n':
               out += "\\n";
               break;
            case '\r':
               out += "\\r";
               break;
            case '\t':
               out += "\\t";
               break;
            default:
               if (character < 0x20)
               {
                  AnsiString escaped;
                  escaped.Format("\\u%04x", (int) character);
                  out += escaped;
               }
               else
               {
                  out += single;
               }
               break;
            }
         }

         return out;
      }

   }

   AnsiString
   Journaling::MarkValue(__int64 ruleId)
   {
      const AnsiString mark = ProcessMark();

      AnsiString value;
      value.Format("%I64d", ruleId);

      if (mark.IsEmpty())
         return value;

      return value + "." + mark;
   }

   bool
   Journaling::IsOwnMark(const AnsiString &value)
   {
      const AnsiString mark = ProcessMark();

      if (mark.IsEmpty())
         return false;

      AnsiString written = value;
      written.Trim();

      // Longer than the mark by at least the full stop and one digit of id, and
      // the mark exactly where this process puts it. Compared as written rather
      // than through EndsWith, which compares without case.
      if (written.GetLength() < mark.GetLength() + 2)
         return false;

      if (written[written.GetLength() - mark.GetLength() - 1] != '.')
         return false;

      return written.Right(mark.GetLength()) == mark;
   }

   AnsiString
   Journaling::RuleJson(std::shared_ptr<JournalRule> rule)
   {
      AnsiString entry;
      entry.Format("{\"id\":%I64d,\"scope\":\"%hs\",\"target\":\"%hs\",\"journal_address\":\"%hs\","
                   "\"direction\":\"%hs\",\"mode\":\"%hs\",\"enabled\":%hs,\"note\":\"%hs\"}",
         rule->GetID(),
         JsonEscaped(Journaling::ScopeName(rule->GetScope())).c_str(),
         JsonEscaped(rule->GetTarget()).c_str(),
         JsonEscaped(rule->GetJournalAddress()).c_str(),
         JsonEscaped(Journaling::DirectionName(rule->GetDirection())).c_str(),
         JsonEscaped(Journaling::ModeName(rule->GetMode())).c_str(),
         rule->GetEnabled() ? "true" : "false",
         JsonEscaped(rule->GetNote()).c_str());
      return entry;
   }

   String
   Journaling::ScopeName(int scope)
   {
      return scope == JournalRule::ScopeAccount ? String(_T("account")) : String(_T("domain"));
   }

   String
   Journaling::DirectionName(int direction)
   {
      if (direction == JournalRule::DirectionInbound)
         return _T("inbound");

      if (direction == JournalRule::DirectionOutbound)
         return _T("outbound");

      return _T("both");
   }

   String
   Journaling::ModeName(int mode)
   {
      return mode == JournalRule::ModeCopy ? String(_T("copy")) : String(_T("report"));
   }

   AnsiString
   Journaling::RulesDocument(bool &readable)
   {
      JournalRules all;

      readable = all.Refresh();

      if (!readable)
         return "{\"items\":[]}";

      AnsiString document = "{\"items\":[";
      int count = 0;

      for (std::shared_ptr<JournalRule> rule : all.GetSnapshot())
      {
         if (!rule)
            continue;

         if (count > 0)
            document += ",";

         document += RuleJson(rule);
         count++;
      }

      document += "]}";

      return document;
   }

   std::shared_ptr<JournalRule>
   Journaling::ReadRule(__int64 id)
   {
      std::shared_ptr<JournalRule> rule(new JournalRule);

      // The id stays 64 bits the whole way. It used to be narrowed to a long
      // here and to an int again in the persistence layer, and on Windows both
      // are 32-bit: an id of 4294967297 read row 1, so a PUT or a DELETE with
      // an id nothing answers to edited or destroyed a different rule and
      // answered success. An id no row carries now finds no row.
      if (id <= 0 || !PersistentJournalRule::ReadObject(rule, id))
         return std::shared_ptr<JournalRule>();

      return rule;
   }

   bool
   Journaling::SaveRule(__int64 id, const String &scope, const String &target,
                        const String &journalAddress, const String &direction, const String &mode,
                        bool enabled, const String &note, __int64 &savedId, String &error)
   {
      savedId = 0;

      std::shared_ptr<JournalRule> rule;

      if (id > 0)
      {
         rule = ReadRule(id);

         if (!rule)
         {
            error = _T("There is no journaling rule with that id.");
            return false;
         }
      }
      else
      {
         rule = std::shared_ptr<JournalRule>(new JournalRule);
      }

      const String wantedScope = Lower(scope);
      const String wantedDirection = Lower(direction);
      const String wantedMode = Lower(mode);

      if (wantedScope == _T("domain"))
         rule->SetScope(JournalRule::ScopeDomain);
      else if (wantedScope == _T("account"))
         rule->SetScope(JournalRule::ScopeAccount);
      else
      {
         error = _T("scope must be domain or account.");
         return false;
      }

      if (wantedDirection == _T("inbound"))
         rule->SetDirection(JournalRule::DirectionInbound);
      else if (wantedDirection == _T("outbound"))
         rule->SetDirection(JournalRule::DirectionOutbound);
      else if (wantedDirection == _T("both"))
         rule->SetDirection(JournalRule::DirectionBoth);
      else
      {
         error = _T("direction must be inbound, outbound or both.");
         return false;
      }

      if (wantedMode == _T("report"))
         rule->SetMode(JournalRule::ModeReport);
      else if (wantedMode == _T("copy"))
         rule->SetMode(JournalRule::ModeCopy);
      else
      {
         error = _T("mode must be report or copy.");
         return false;
      }

      if (note.GetLength() > 255)
      {
         error = _T("The note is at most 255 characters.");
         return false;
      }

      // The one arrangement that is certainly a mistake rather than a choice: an
      // account's mail copied back into that same account, which puts every
      // message in the mailbox twice and archives nothing anywhere else. Refused
      // here, when it is written, rather than found out later from a mailbox that
      // has doubled - and a domain rule whose journal address is a mailbox in the
      // journalled domain is NOT this, is perfectly ordinary, and works.
      if (rule->GetScope() == JournalRule::ScopeAccount &&
          Canonical(target) == Canonical(journalAddress) && !journalAddress.IsEmpty())
      {
         error = _T("A rule cannot copy an account's mail into that same account.");
         return false;
      }

      rule->SetTarget(target);
      rule->SetJournalAddress(journalAddress);
      rule->SetEnabled(enabled);
      rule->SetNote(note);

      if (!PersistentJournalRule::SaveObject(rule, error, PersistenceModeNormal))
      {
         if (error.IsEmpty())
            error = _T("The journaling rule could not be saved; see the error log.");

         return false;
      }

      savedId = rule->GetID();

      return true;
   }

   bool
   Journaling::DeleteRule(__int64 id, bool &found)
   {
      std::shared_ptr<JournalRule> rule = ReadRule(id);

      found = rule ? true : false;

      if (!found)
         return true;

      return PersistentJournalRule::DeleteObject(rule);
   }

   void
   Journaling::SetNeedRefresh()
   {
      std::lock_guard<std::mutex> guard(rule_mutex);
      need_refresh = true;
   }

   bool
   Journaling::Refresh_()
   {
      JournalRules all;

      if (!all.Refresh())
      {
         // Keep what is here and leave the flag set, so the next message tries
         // again. Emptying the list would switch journaling off for as long as
         // the database was unwell, and a compliance archive with a hole in it
         // that nobody was told about is the one outcome this must not have.
         const time_t now = time(nullptr);

         if (last_refresh_report == 0 || now - last_refresh_report > 3600)
         {
            last_refresh_report = now;
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6990, "Journaling::Refresh_",
               "The journaling rules could not be read. The rules last read are still in force; a rule added, changed or removed since is not in effect yet.");
         }

         return false;
      }

      rules = all.GetSnapshot();
      need_refresh = false;

      return true;
   }

   AnsiString
   Journaling::BuildReportText(const Envelope &envelope)
   {
      // The fixed four-field form. A reader of a journal mailbox - and the
      // programs that read one - must be able to take the envelope out of it
      // without a parser, so the fields are in one order, one per line, and a
      // field with nothing in it is still written.
      AnsiString text;

      text += "Sender: ";
      text += envelope.sender.IsEmpty() ? AnsiString("<>") : Utf8(envelope.sender);
      text += "\r\n";

      text += "Subject: ";
      text += Utf8(envelope.subject);
      text += "\r\n";

      text += "Message-Id: ";
      text += Utf8(envelope.message_id);
      text += "\r\n";

      for (const String &recipient : envelope.recipients)
      {
         text += "Recipient: ";
         text += Utf8(recipient);
         text += "\r\n";
      }

      return text;
   }

   bool
   Journaling::Queue_(std::shared_ptr<JournalRule> rule, const String &originalFileName,
                      const Envelope &envelope, unsigned int sessionId)
   {
      std::shared_ptr<const Domain> domain = CacheContainer::Instance()->GetDomain(rule->GetDomainID());

      if (!domain)
      {
         // Reported, not merely returned. Everything else that can go wrong in
         // this function says so, and this file's own argument in Refresh_ is
         // that a compliance archive with a hole in it that nobody was told about
         // is the one outcome it must not have. The rule's domain can be missing
         // from the cache while the row still names it, and the message is
         // accepted and delivered either way.
         String text;
         text.Format(_T("Journaling rule %I64d names domain %I64d, which is not in the cache, so nothing was journaled for it."),
            rule->GetID(), rule->GetDomainID());

         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6996, "Journaling::Queue_", text);
         return false;
      }

      // Whom a failure comes back to. The administrator who wrote the rule, not
      // the person who wrote the message: a journal address that stops answering
      // is the administrator's problem, and bouncing it to the sender would tell
      // somebody outside this server that their mail is being copied.
      String envelopeSender = domain->GetPostmaster();
      envelopeSender.Trim();

      if (envelopeSender.IsEmpty() || envelopeSender.Find(_T("@")) < 0)
         envelopeSender = _T("postmaster@") + domain->GetName();

      std::shared_ptr<Message> journal = std::shared_ptr<Message>(new Message);
      journal->SetState(Message::Delivering);
      journal->SetFromAddress(envelopeSender);

      const String fileName = PersistentMessage::GetFileName(journal);

      File target;
      if (!target.Open(fileName, File::OTCreate))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6991, "Journaling::Queue_",
            "A journal message could not be written: " + fileName);
         return false;
      }

      // The rule's id and this process's mark. See NOT LOOPING in Journaling.h:
      // the id says which rule made the copy, and the mark is what makes the
      // evaluation's guard a guard rather than an invitation.
      const AnsiString mark = MarkValue(rule->GetID());

      bool written = true;
      AnsiString boundary;

      if (rule->GetMode() == JournalRule::ModeCopy)
      {
         // Dual delivery: the message as it was accepted, with one header line in
         // front of it. Nothing else is touched - not the body, not its transfer
         // encoding, not its signature - which is why this is written byte for
         // byte rather than through MessageData, whose write rebuilds the MIME
         // tree and would change a message this is supposed to reproduce.
         written = target.Write(AnsiString(HeaderName()) + ": " + mark + "\r\n");
      }
      else
      {
         // Hexadecimal digits only. RFC 2046's bcharsnospace has no brace in it,
         // so the braced GUID this used to carry made a boundary no strict MIME
         // reader is obliged to accept - and a compliance archive is read by
         // exactly such readers.
         boundary = AnsiString("hmailserver-journal-") + HexOfGuid();

         AnsiString header;
         header += AnsiString(HeaderName()) + ": " + mark + "\r\n";
         header += "From: " + HeaderSafe(Utf8(envelopeSender)) + "\r\n";
         header += "To: " + HeaderSafe(Utf8(rule->GetJournalAddress())) + "\r\n";
         header += "Subject: " + HeaderSafe(envelope.raw_subject) + "\r\n";
         header += "Date: " + Utf8(Time::GetCurrentMimeDate()) + "\r\n";
         header += "Message-ID: <" + HexOfGuid() + "@" + HeaderSafe(Utf8(domain->GetName())) + ">\r\n";
         header += "Auto-Submitted: auto-generated\r\n";
         header += "MIME-Version: 1.0\r\n";
         header += "Content-Type: multipart/mixed; boundary=\"" + boundary + "\"\r\n";
         header += "\r\n";
         header += "--" + boundary + "\r\n";
         header += "Content-Type: text/plain; charset=\"utf-8\"\r\n";
         header += "Content-Transfer-Encoding: 8bit\r\n";
         header += "\r\n";
         header += BuildReportText(envelope);
         header += "\r\n";
         header += "--" + boundary + "\r\n";
         header += "Content-Type: message/rfc822\r\n";
         header += "Content-Disposition: attachment; filename=\"journaled.eml\"\r\n";
         header += "\r\n";

         written = target.Write(header);
      }

      // Both shapes write the message itself next: the copy is the message with
      // one header line in front of it, and the report has it as its second part.
      // Streamed rather than read into memory, so that journaling a 50 MB message
      // costs what copying the file costs and no more.
      if (written)
      {
         File source;

         if (!source.Open(originalFileName, File::OTReadOnly))
         {
            written = false;
         }
         else
         {
            written = target.Write(source);
            source.Close();
         }
      }

      if (written && rule->GetMode() != JournalRule::ModeCopy)
         written = target.Write(AnsiString("\r\n--") + boundary + "--\r\n");

      target.Close();

      if (!written)
      {
         FileUtilities::DeleteFile(fileName);

         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6992, "Journaling::Queue_",
            "A journal message could not be written from " + originalFileName + "; the partial file has been removed.");
         return false;
      }

      journal->SetSize(FileUtilities::FileSize(fileName));

      bool recipientOK = false;
      RecipientParser recipientParser;
      recipientParser.CreateMessageRecipientList(rule->GetJournalAddress(), journal->GetRecipients(), recipientOK);

      if (journal->GetRecipients()->GetCount() == 0)
      {
         FileUtilities::DeleteFile(fileName);

         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6993, "Journaling::Queue_",
            "The journal address " + rule->GetJournalAddress() + " expanded to no recipient, so nothing was journaled for that rule.");
         return false;
      }

      if (!PersistentMessage::SaveObject(journal))
      {
         // The same treatment a rule's forward gives a failed save: the file goes
         // rather than staying on disk with no row referring to it. The original
         // message is untouched either way; this is a second message.
         FileUtilities::DeleteFile(fileName);

         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6994, "Journaling::Queue_",
            "A journal message for " + rule->GetJournalAddress() + " could not be saved. The file has been removed rather than left on disk with nothing referring to it.");
         return false;
      }

      String logText;
      logText.Format(_T("Journaling: rule %I64d (%s, %s) sent %s to %s."),
         rule->GetID(),
         rule->GetTarget().c_str(),
         rule->GetMode() == JournalRule::ModeCopy ? _T("copy") : _T("report"),
         rule->GetMode() == JournalRule::ModeCopy ? _T("a copy of the message") : _T("an envelope report"),
         rule->GetJournalAddress().c_str());
      LogLine(sessionId, _T(""), logText);

      return true;
   }

   int
   Journaling::Evaluate(std::shared_ptr<Message> message, const String &authenticatedAddress,
                        unsigned int sessionId, const String &connectingAddress)
   {
      if (!message)
         return 0;

      std::vector<std::shared_ptr<JournalRule> > matched;

      {
         std::lock_guard<std::mutex> guard(rule_mutex);

         if (need_refresh)
            Refresh_();

         // The common case, and it must cost nothing: no rule anywhere means the
         // whole feature is one empty test away from the acceptance path.
         if (rules.empty())
            return 0;
      }

      const String fileName = PersistentMessage::GetFileName(message);

      AnsiString rawHeader = PersistentMessage::LoadHeader(fileName, false);

      MimeHeader header;
      header.Load(rawHeader.c_str(), rawHeader.GetLength(), true);

      // The guard, and first because it is the cheapest: this message is one this
      // process journaled, so it is not journaled again. Only this process's own
      // mark counts. A sender who writes the header themselves - the name is
      // published - is journaled exactly as they would have been without it, and
      // the attempt is worth a line in the log, since somebody writing this
      // particular header by hand is somebody trying to stay out of an archive.
      const char *journalMark = header.GetRawFieldValue(String(HeaderName()));

      if (journalMark != nullptr && *journalMark != 0)
      {
         if (IsOwnMark(AnsiString(journalMark)))
         {
            LogLine(sessionId, connectingAddress, _T("Journaling skipped: the message is one this server journaled."));
            return 0;
         }

         LogLine(sessionId, connectingAddress,
            _T("Journaling: the message carries an X-hMailServer-Journal header this server did not write, which decides nothing; the rules are applied to it as they are to any other message."));
      }

      Envelope envelope;
      envelope.sender = message->GetFromAddress();
      envelope.subject = header.GetUnicodeFieldValue("Subject");

      // The subject as the message wrote it, for the wrapper header. The
      // decoded one above is for the report text, whose part declares UTF-8;
      // putting it into a header would be raw UTF-8 where an encoded word
      // belongs.
      const char *rawSubject = header.GetRawFieldValue("Subject");
      if (rawSubject != nullptr)
         envelope.raw_subject = rawSubject;

      const char *messageId = header.GetRawFieldValue("Message-ID");
      if (messageId != nullptr)
         envelope.message_id = messageId;

      // Whose mail this is, seen from the two sides a rule can speak for. The
      // envelope recipients are read once: the archive decision just above this
      // reads the same list.
      std::set<__int64> recipientAccounts;
      std::set<__int64> recipientDomains;

      for (std::shared_ptr<MessageRecipient> recipient : message->GetRecipients()->GetVector())
      {
         if (!recipient)
            continue;

         envelope.recipients.push_back(recipient->GetOriginalAddress().IsEmpty()
            ? recipient->GetAddress() : recipient->GetOriginalAddress());

         if (recipient->GetLocalAccountID() == 0)
            continue;

         std::shared_ptr<const Account> account = CacheContainer::Instance()->GetAccount(recipient->GetLocalAccountID());

         if (!account)
            continue;

         recipientAccounts.insert(account->GetID());
         recipientDomains.insert(account->GetDomainID());
      }

      __int64 senderAccountId = 0;
      __int64 senderDomainId = 0;

      if (!authenticatedAddress.IsEmpty())
      {
         std::shared_ptr<const Account> sender = CacheContainer::Instance()->GetAccount(Canonical(authenticatedAddress));

         if (sender)
         {
            senderAccountId = sender->GetID();
            senderDomainId = sender->GetDomainID();
         }
      }

      {
         std::lock_guard<std::mutex> guard(rule_mutex);

         for (std::shared_ptr<JournalRule> rule : rules)
         {
            if (!rule || !rule->GetEnabled() || rule->GetJournalAddress().IsEmpty())
               continue;

            const bool inbound = rule->CoversInbound() &&
               (rule->GetScope() == JournalRule::ScopeAccount
                  ? recipientAccounts.find(rule->GetAccountID()) != recipientAccounts.end()
                  : recipientDomains.find(rule->GetDomainID()) != recipientDomains.end());

            const bool outbound = rule->CoversOutbound() && senderAccountId != 0 &&
               (rule->GetScope() == JournalRule::ScopeAccount
                  ? rule->GetAccountID() == senderAccountId
                  : rule->GetDomainID() == senderDomainId);

            if (!inbound && !outbound)
               continue;

            matched.push_back(rule);
         }
      }

      int queued = 0;

      for (std::shared_ptr<JournalRule> rule : matched)
      {
         if (Queue_(rule, fileName, envelope, sessionId))
            queued++;
      }

      if (queued > 0)
         Application::Instance()->SubmitPendingEmail();

      return queued;
   }

   void
   JournalingTester::Test()
   {
      Journaling::Envelope envelope;
      envelope.sender = _T("alice@example.com");
      envelope.subject = _T("Quarterly figures");
      envelope.message_id = _T("<abc@example.com>");
      envelope.recipients.push_back(_T("bob@example.test"));
      envelope.recipients.push_back(_T("carol@example.test"));

      const AnsiString report = Journaling::BuildReportText(envelope);

      if (report != "Sender: alice@example.com\r\nSubject: Quarterly figures\r\nMessage-Id: <abc@example.com>\r\n"
                    "Recipient: bob@example.test\r\nRecipient: carol@example.test\r\n")
         throw std::logic_error("The envelope report is not in the four-field form the journal mailbox is read by.");

      // A bounce has no envelope sender, and a journal report of one must say so
      // rather than leaving the field looking as though it was not written.
      Journaling::Envelope bounce;
      bounce.subject = _T("Undeliverable");
      bounce.recipients.push_back(_T("bob@example.test"));

      const AnsiString bounceReport = Journaling::BuildReportText(bounce);

      if (bounceReport != "Sender: <>\r\nSubject: Undeliverable\r\nMessage-Id: \r\nRecipient: bob@example.test\r\n")
         throw std::logic_error("The null envelope sender was not written as <> in the envelope report.");

      // One line per envelope recipient, including the ones no header mentions:
      // that is the whole reason the report exists beside the attached message.
      Journaling::Envelope blind;
      blind.sender = _T("alice@example.com");
      blind.recipients.push_back(_T("one@example.test"));
      blind.recipients.push_back(_T("two@example.test"));
      blind.recipients.push_back(_T("three@example.test"));

      const AnsiString blindReport = Journaling::BuildReportText(blind);

      if (blindReport != "Sender: alice@example.com\r\nSubject: \r\nMessage-Id: \r\n"
                         "Recipient: one@example.test\r\nRecipient: two@example.test\r\nRecipient: three@example.test\r\n")
         throw std::logic_error("The envelope report did not carry one Recipient line per envelope recipient, in order.");

      // The loop guard. Everything below is about one distinction: a value this
      // process wrote against a value somebody else did. Get it wrong the other
      // way and anybody who reads the documentation can keep their mail out of a
      // compliance archive by typing one header into their client.
      const AnsiString ours = Journaling::MarkValue(7);

      if (ours.Find("7.") != 0)
         throw std::logic_error("A journal mark does not begin with the id of the rule that made it.");

      if (!Journaling::IsOwnMark(ours))
         throw std::logic_error("This process does not recognise a mark it wrote itself.");

      // What a sender can write. The id alone is exactly the value the header
      // used to carry, so this is the old forgery, refused.
      if (Journaling::IsOwnMark("7"))
         throw std::logic_error("A bare rule id is honoured as a journal mark, so the header can be forged.");

      if (Journaling::IsOwnMark("1"))
         throw std::logic_error("A value a sender can type is honoured as a journal mark.");

      if (Journaling::IsOwnMark(""))
         throw std::logic_error("An empty journal header is honoured as a journal mark.");

      if (Journaling::IsOwnMark("7." + AnsiString("00000000000000000000000000000000")))
         throw std::logic_error("A mark of the right shape but the wrong value is honoured.");

      // A header value arrives with the space after the colon on some readers and
      // without it on others, and either is the same value.
      if (!Journaling::IsOwnMark(" " + ours + " "))
         throw std::logic_error("A journal mark is not recognised when the header value carries white space around it.");

      // One mark for the process, not one per message: two rules' values differ
      // only in the id in front.
      const AnsiString other = Journaling::MarkValue(8);

      if (other.Mid(2) != ours.Mid(2) || other.Find("8.") != 0)
         throw std::logic_error("Two journal marks of the same process do not share the process's mark.");

      // The MIME boundary and the mark are built from the same digits, and
      // RFC 2046's bcharsnospace has no brace and no hyphen-in-a-GUID's place
      // for them: a compliance archive is read by strict parsers.
      const AnsiString digits = HexOfGuid();

      if (digits.GetLength() != 32)
         throw std::logic_error("A GUID did not give thirty-two hexadecimal digits for the boundary and the mark.");

      for (int i = 0; i < digits.GetLength(); i++)
      {
         const char character = (char) digits[i];

         if (!((character >= '0' && character <= '9') ||
               (character >= 'a' && character <= 'f') ||
               (character >= 'A' && character <= 'F')))
            throw std::logic_error("A character that RFC 2046 does not allow in a MIME boundary reached one.");
      }

      // And the header writer takes out what would end a header field or the
      // header block, whatever the rule or the message carried.
      if (HeaderSafe("archive@example.test\r\nRCPT TO:<elsewhere@example.test>") !=
          "archive@example.testRCPT TO:<elsewhere@example.test>")
         throw std::logic_error("A line break survived into a header this server writes.");
   }
}
