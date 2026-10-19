// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "QuarantineStore.h"

#include "../Application/Application.h"
#include "../Application/IniFileSettings.h"
#include "../BO/Message.h"
#include "../BO/MessageRecipient.h"
#include "../BO/MessageRecipients.h"
#include "../Persistence/PersistentMessage.h"
#include "../Persistence/MessageStore.h"
#include "../SQL/DALRecordset.h"
#include "../SQL/SQLCommand.h"
#include "../SQL/SQLStatement.h"
#include "../SQL/DatabaseConnectionManager.h"
#include "../SQL/DatabaseSettings.h"
#include "../Util/FileUtilities.h"
#include "../Util/GUIDCreator.h"
#include "../Util/Time.h"
#include "../Util/Utilities.h"
#include "../Util/WebhookManager.h"
#include "../Mime/Mime.h"
#include <ctime>
#include "../../SMTP/RecipientParser.h"
#include "../Util/Parsing/StringParser.h"
#include "../BO/MessageData.h"
#include "../AntiVirus/Sandbox.h"
#include "../Util/ContentPolicy.h"
#include "../Util/Unicode.h"
#include "../Util/MessageOrigin.h"
#include "DKIM/DKIMSigner.h"
#include "DMARC/DMARC.h"

#include <stdexcept>
#include <utility>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // Cuts a text to at most limit characters, and never between the two
      // halves of a UTF-16 surrogate pair: half a character is no character, and
      // the JSON it is written into would carry a lone surrogate.
      bool CutAt(String &text, int limit)
      {
         if (text.GetLength() <= limit)
            return false;

         int cut = limit;
         if (cut > 0 && (unsigned int) text[cut - 1] >= 0xD800 && (unsigned int) text[cut - 1] <= 0xDBFF)
            cut--;

         text = text.Left(cut);
         return true;
      }

      // Where a tag ("<head", "</head") next starts at or after from, by its
      // whole name: the name is followed by a space, '/' or '>', or the text
      // ends. "<head" also starts <header> and <headline>, whose text a person
      // reads. A match that is not the whole name is passed over and the
      // search goes on from the character after it, so the text is still read
      // once.
      int FindTag(const String &lower, const String &tag, int from)
      {
         for (int at = lower.Find(tag.c_str(), from); at >= 0; at = lower.Find(tag.c_str(), at + 1))
         {
            const int after = at + tag.GetLength();
            if (after >= lower.GetLength())
               return at;

            const wchar_t c = lower[after];
            if (c == L'>' || c == L'/' || c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'\f')
               return at;
         }

         return -1;
      }
   }

   bool
   QuarantineStore::GetEnabled()
   {
      return IniFileSettings::Instance()->GetQuarantineEnabled();
   }

   String
   QuarantineStore::GetQuarantineDirectory()
   {
      // Not created here. FileUtilities::Copy makes missing directories on demand,
      // so the store comes into existence the first time something is quarantined -
      // and a server that never quarantines anything never grows an empty directory
      // it did not ask for.
      return FileUtilities::Combine(IniFileSettings::Instance()->GetDataDirectory(), "Quarantine");
   }

   namespace
   {
      // The ends of a stored copy's name that say where the message came from
      // (QuarantineStore::StoredName). Strings, so that each carries its length.
      const String OutsideNameEnd = _T(".outside.eml");
      const String InsideNameEnd = _T(".inside.eml");

      // The words of a stored copy's name between the GUID and its end
      // (QuarantineStore::StoredMarks).
      const String SpfNameMark = _T("spf-");
      const String RelayNameMark = _T("relay-");
      const String SendersDomainNameMark = _T("mailfrom");
      const String DmarcNameMark = _T("dmarc");

      bool EndsWithNoCase(const String &text, const String &end)
      {
         const int length = end.GetLength();
         return text.GetLength() >= length && text.Right(length).CompareNoCase(end) == 0;
      }

      bool StartsWithNoCase(const String &text, const String &start)
      {
         const int length = start.GetLength();
         return text.GetLength() > length && text.Left(length).CompareNoCase(start) == 0;
      }
   }

   String
   QuarantineStore::BuildRelativePath_(const StoredMarks &marks)
   {
      // A day-per-directory layout. A flat directory is fine until the day somebody
      // points this at a mail stream that quarantines thousands a day, at which point
      // enumerating it becomes the slowest thing in the server - and the retention
      // sweep gets to delete whole directories rather than walking every file.
      String day = Time::GetCurrentDateTime().Mid(0, 10);   // YYYY-MM-DD

      return FileUtilities::Combine(day, StoredName(GUIDCreator::GetGUID(), marks));
   }

   String
   QuarantineStore::StoredName(const String &guid, const StoredMarks &marks)
   {
      String name = guid;

      // One of RFC 8601's seven keywords, in lower case, or nothing: none holds a dot
      // or anything else a file name may not (Message::SpfResultKeyword).
      const String spf = Message::SpfResultKeyword(marks.spf_result);
      if (!spf.IsEmpty())
         name += String(_T(".")) + SpfNameMark + spf;

      if (marks.spf_domain_is_senders)
         name += String(_T(".")) + SendersDomainNameMark;

      if (marks.dmarc_passed_for_from)
         name += String(_T(".")) + DmarcNameMark;

      // A hosted domain's id, digits alone.
      if (marks.relaying_domain_id > 0 && marks.relaying_domain_id <= INT_MAX)
      {
         String relay;
         relay.Format(_T(".%s%I64d"), RelayNameMark.c_str(), marks.relaying_domain_id);
         name += relay;
      }

      return name + (marks.from_outside ? OutsideNameEnd : InsideNameEnd);
   }

   QuarantineStore::StoredMarks
   QuarantineStore::MarksOfStoredName(const String &relativePath)
   {
      StoredMarks marks;

      // The name, without the day's directory, whichever separator wrote it.
      const int backslash = relativePath.ReverseFind(L'\\');
      const int forwardSlash = relativePath.ReverseFind(L'/');
      const int slash = backslash > forwardSlash ? backslash : forwardSlash;
      const String name = slash >= 0 ? relativePath.Mid(slash + 1) : relativePath;

      String stem;
      if (EndsWithNoCase(name, OutsideNameEnd))
      {
         marks.from_outside = true;
         stem = name.Left(name.GetLength() - OutsideNameEnd.GetLength());
      }
      else if (EndsWithNoCase(name, InsideNameEnd))
      {
         stem = name.Left(name.GetLength() - InsideNameEnd.GetLength());
      }
      else
      {
         return marks;
      }

      marks.known = true;

      // The GUID, then each mark after a dot. A word this version does not know is
      // passed over: a later one may write more.
      const std::vector<String> words = StringParser::SplitString(stem, _T("."));
      for (size_t i = 1; i < words.size(); i++)
      {
         const String &word = words[i];

         if (StartsWithNoCase(word, SpfNameMark))
            marks.spf_result = Message::SpfResultKeyword(word.Mid(SpfNameMark.GetLength()));
         else if (word.CompareNoCase(SendersDomainNameMark) == 0)
            marks.spf_domain_is_senders = true;
         else if (word.CompareNoCase(DmarcNameMark) == 0)
            marks.dmarc_passed_for_from = true;
         else if (StartsWithNoCase(word, RelayNameMark))
         {
            const String digits = word.Mid(RelayNameMark.GetLength());
            bool allDigits = digits.GetLength() <= 10;
            __int64 id = 0;
            for (int c = 0; allDigits && c < digits.GetLength(); c++)
            {
               allDigits = digits[c] >= L'0' && digits[c] <= L'9';
               id = id * 10 + (digits[c] - L'0');
            }

            marks.relaying_domain_id = allDigits && id > 0 && id <= INT_MAX ? id : 0;
         }
      }

      return marks;
   }

   QuarantineStore::StoredOrigin
   QuarantineStore::OriginOfStoredName(const String &relativePath)
   {
      const StoredMarks marks = MarksOfStoredName(relativePath);
      if (!marks.known)
         return OriginUnknown;

      return marks.from_outside ? OriginOutside : OriginInside;
   }

   String
   QuarantineStore::FromDomainAsDmarcReadsIt(const AnsiString &header)
   {
      if (header.IsEmpty())
         return String();

      MimeHeader mimeHeader;
      mimeHeader.Load(header.c_str(), header.GetLength(), true);

      // The first From field, decoded, as MessageData::GetFrom gives it to the DMARC
      // check (SpamTestDMARC), and the address in it.
      String domain = StringParser::ExtractDomain(DMARC::ExtractAddressFromHeaderValue(mimeHeader.GetUnicodeFieldValue("From")));
      domain.Trim();
      domain.MakeLower();
      return domain;
   }

   bool
   QuarantineStore::RecordsDmarcPassFor(const String &authDomains, const String &domain)
   {
      if (authDomains.IsEmpty() || domain.IsEmpty())
         return false;

      const String wanted = _T("dmarc=") + domain;

      String words = authDomains;
      words.Replace(_T(","), _T(" "));
      words.Replace(_T("\t"), _T(" "));

      for (String word : StringParser::SplitString(words, _T(" ")))
      {
         word.Trim();
         if (word.GetLength() == wanted.GetLength() && word.CompareNoCase(wanted) == 0)
            return true;
      }

      return false;
   }

   bool
   QuarantineStore::HeaderSaysFromOutside(const AnsiString &header, const String &thisServerName)
   {
      // Nothing to read: the safe side, unsigned.
      if (header.IsEmpty())
         return true;

      // No trace field at all: a message this server made - a reply, a notice, a
      // message made over COM or the webmail - since every message that came in over
      // SMTP has the field this server wrote on top.
      AnsiString received = MessageOrigin::FirstFieldValue(header, "Received");
      received.Trim();
      if (received.IsEmpty())
         return false;

      if (thisServerName.IsEmpty())
         return true;

      AnsiString thisServer = AnsiString(thisServerName);
      thisServer.MakeLower();

      AnsiString lowered = received;
      lowered.MakeLower();

      // The send API's own field (RestApiSend), for a key's sender it held to its own.
      if (lowered.Find("by " + thisServer + " (hmailserver send api") == 0)
         return false;

      // Whose field it is: the name after its last "by", since everything a client
      // puts in a Received field - its HELO, the name its address resolves to - comes
      // before the clause the field's writer adds. Another server's field on top is a
      // download from another mailbox or a message imported from a file, whose From
      // this server never saw written, even when the HELO in it names this server.
      AnsiString writer;
      for (int at = lowered.Find("by "); at >= 0; at = lowered.Find("by ", at + 1))
      {
         if (at > 0 && lowered[at - 1] != ' ')
            continue;

         writer = lowered.Mid(at + 3);
         const int end = writer.Find(" ");
         if (end >= 0)
            writer = writer.Left(end);
      }

      if (writer != thisServer || !MessageOrigin::ReceivedByThisServer(header, thisServerName))
         return true;

      // This server's own SMTP field on top: from outside unless the session signed in.
      return !MessageOrigin::ReceivedSaysAuthenticated(header, thisServerName);
   }

   bool
   QuarantineStore::Quarantine(std::shared_ptr<Message> message, const String &reason, int score)
   {
      if (!message)
         return false;

      return Quarantine(message, reason, score, MessageStore::WorkingFile(message));
   }

   bool
   QuarantineStore::Quarantine(std::shared_ptr<Message> message, const String &reason, int score, const String &sourceFileName)
   {
      return Quarantine_(message, reason, score, sourceFileName, false, String());
   }

   bool
   QuarantineStore::QuarantineFromSession(std::shared_ptr<Message> message, const String &reason, int score, const String &heloName)
   {
      if (!message)
         return false;

      return Quarantine_(message, reason, score, MessageStore::WorkingFile(message), true, heloName);
   }

   String
   QuarantineStore::HeloFromReceivedHeader(const String &receivedValue, const String &localComputerName)
   {
      // The header SMTPMessageHeaderCreator::GenerateReceivedHeader_ writes:
      //
      //    from <helo> (<ptr> [<address>])
      //       by <computer name> with ESMTP id <session>
      //
      // folded over lines. Only a header naming this computer after "by" is
      // this server's own; anybody else's Received header says what a client of
      // theirs claimed, which is not what the review queue shows.
      String value = receivedValue;
      value.Replace(_T("\r"), _T(" "));
      value.Replace(_T("\n"), _T(" "));
      value.Replace(_T("\t"), _T(" "));
      value.Trim();

      if (localComputerName.IsEmpty() || value.GetLength() < 5 || value.Left(5).CompareNoCase(_T("from ")) != 0)
         return String();

      // Searched from the end of "from" on, so that "from by host" - a from
      // clause with no name in it - is read as no name rather than cut at a
      // negative length.
      const int byPosition = value.Find(_T(" by "), 4);
      if (byPosition < 5)
         return String();

      String by = value.Mid(byPosition + 4);
      by.Trim();
      const int byEnd = by.Find(_T(" "));
      if (byEnd >= 0)
         by = by.Left(byEnd);

      if (by.CompareNoCase(localComputerName) != 0)
         return String();

      String helo = value.Mid(5, byPosition - 5);
      helo.Trim();
      const int heloEnd = helo.Find(_T(" "));
      if (heloEnd >= 0)
         helo = helo.Left(heloEnd);

      // A client that gave no HELO has its address written in the name's place.
      // The address is already beside it in brackets; it is not a name.
      if (helo.IsEmpty() || StringParser::IsValidIPAddress(helo))
         return String();

      return helo.Mid(0, 255);
   }

   String
   QuarantineStore::HeloOfStoredCopy_(const String &file)
   {
      // The newest Received header is the first one, and for a message this
      // server received over SMTP it is the one the server wrote.
      AnsiString header = PersistentMessage::LoadHeader(file, false);
      if (header.IsEmpty())
         return String();

      MimeHeader mimeHeader;
      mimeHeader.Load(header, header.GetLength(), true);

      MimeField *received = mimeHeader.GetField("Received");
      if (received == nullptr)
         return String();

      return HeloFromReceivedHeader(String(received->GetValue()), Utilities::ComputerName());
   }

   bool
   QuarantineStore::ReleasedCopyIsFromOutside_(const String &relativePath, const String &file)
   {
      // What the quarantine kept when it held the message, which nothing in the
      // message can change: a domain's rule may rewrite the Received field on top
      // before another of its rules holds the message, and a message downloaded from
      // another mailbox carries whatever that mailbox's server and its sender wrote.
      switch (OriginOfStoredName(relativePath))
      {
      case OriginOutside:
         return true;
      case OriginInside:
         return false;
      default:
         break;
      }

      return HeaderSaysFromOutside(PersistentMessage::LoadHeader(file, false), Utilities::ComputerName());
   }

   void
   QuarantineStore::ApplyStoredMarks_(const QuarantinedMessage &quarantined, Message &message, const String &file)
   {
      // What the name kept beside the origin, which nothing in the message can change.
      // A copy held before the name kept it has none: no SPF result and no domain
      // recorded to seal it, as it was released before.
      const StoredMarks marks = MarksOfStoredName(quarantined.file_name);
      message.SetReceivedSpfResult(marks.spf_result);
      message.SetRelayingDomainID(marks.relaying_domain_id);

      // The domain the check evaluated, when it was the held message's sender's - the
      // row's sender, which the release gives the message it queues.
      if (marks.spf_domain_is_senders)
         message.SetReceivedSpfDomain(StringParser::ExtractDomain(quarantined.sender));

      // What DMARC passed for when the message arrived, when that was the domain of the
      // From the stored copy carries: read from the copy again, as the hold read it
      // (Quarantine_), for the envelope a forward of the released message is sent on
      // with alone (Message::GetCopiedAuthDomains).
      if (marks.from_outside && marks.dmarc_passed_for_from)
      {
         const String domain = FromDomainAsDmarcReadsIt(PersistentMessage::LoadHeader(file, false));
         if (!domain.IsEmpty())
            message.SetCopiedAuthDomains(_T("dmarc=") + domain);
      }
   }

   bool
   QuarantineStore::Quarantine_(std::shared_ptr<Message> message, const String &reason, int score, const String &sourceFileName,
                                bool heloKnown, const String &heloName)
   {
      try
      {
         if (!message)
            return false;

         String sourceFile = sourceFileName;

         if (sourceFile.IsEmpty() || !FileUtilities::Exists(sourceFile))
            return false;

         // The name of the copy says where the message came from, for its release to
         // read back (StoredName): whether it is mail this server would not sign as
         // the domain its From names, the SPF result the ARC set over a copy of it
         // relayed on attests, and the hosted domain that seals that set.
         StoredMarks marks;
         marks.from_outside = !DKIMSigner::SignsAsAuthorDomain(*message);
         marks.spf_result = message->GetReceivedSpfResult();
         marks.spf_domain_is_senders = !message->GetReceivedSpfDomain().IsEmpty() &&
            message->GetReceivedSpfDomain().CompareNoCase(StringParser::ExtractDomain(message->GetFromAddress())) == 0;
         marks.relaying_domain_id = DKIMSigner::RelayingDomainID(message, false);

         // And whether mail from outside passed DMARC, when it arrived, for the domain of
         // the From the stored copy carries, which its release gives back
         // (ApplyStoredMarks_): the copy's header is read only when there is a pass.
         const String passedOnArrival = marks.from_outside ? message->GetAuthDomainsOnArrival() : String();
         marks.dmarc_passed_for_from = !passedOnArrival.IsEmpty() &&
            RecordsDmarcPassFor(passedOnArrival, FromDomainAsDmarcReadsIt(PersistentMessage::LoadHeader(sourceFile, false)));
         String relativePath = BuildRelativePath_(marks);
         String targetFile = FileUtilities::Combine(GetQuarantineDirectory(), relativePath);

         // Copied rather than moved: the caller still owns the original and will
         // delete it as part of not delivering the message. A move would leave the
         // caller's cleanup deleting a file that is now the quarantine's only copy.
         //
         // The third argument creates the day directory, so the layout needs no
         // separate setup step.
         if (!FileUtilities::Copy(sourceFile, targetFile, true))
            return false;

         String recipients;

         if (message->GetRecipients())
         {
            // owned by this call: the recipients of the message its caller holds - a session's, a delivery's or a task's own
            for (auto recipient : message->GetRecipients()->GetVector())
            {
               if (!recipients.IsEmpty())
                  recipients += ",";

               recipients += recipient->GetAddress();
            }
         }

         // The subject is read from the stored file rather than carried in: at this
         // point in the SMTP conversation the message object holds envelope data, and
         // a review queue that cannot show a subject is not reviewable.
         String subject;

         {
            MessageData messageData;
            if (messageData.LoadFromMessage(targetFile, message))
               subject = messageData.GetSubject();
         }

         String helo = heloKnown ? heloName : HeloOfStoredCopy_(targetFile);
         helo.Trim();

         SQLStatement statement;

         statement.SetTable("hm_quarantine");
         statement.AddColumn("quarantinefilename", relativePath);
         statement.AddColumn("quarantinesender", message->GetFromAddress());
         statement.AddColumn("quarantinerecipients", recipients.Mid(0, 1000));
         statement.AddColumn("quarantinesubject", subject.Mid(0, 255));
         statement.AddColumn("quarantinereason", reason.Mid(0, 255));
         statement.AddColumnInt64("quarantinescore", score);
         statement.AddColumnInt64("quarantinesize", FileUtilities::FileSize(targetFile));
         statement.AddColumnDate("quarantinecreated", Time::GetDateFromSystemDate(Time::GetCurrentDateTime()));
         statement.AddColumn("quarantinehelo", helo.Mid(0, 255));
         statement.SetStatementType(SQLStatement::STInsert);
         statement.SetIdentityColumn("quarantineid");

         __int64 dbid = 0;

         if (!Application::Instance()->GetDBManager()->Execute(statement, &dbid))
         {
            // The row is the index; a file with no row is invisible and will never be
            // swept. Remove it rather than leave litter that grows for ever.
            FileUtilities::DeleteFile(targetFile);
            return false;
         }


         // The outbound webhooks: one event per recipient the held message was for,
         // since a subscription can be confined to one address and each of those
         // people has had a message of theirs held. Raised after the row is written,
         // so nothing is announced that the quarantine does not actually hold. The
         // subject travels only where the domain and the subscription both allow it;
         // it is already in hand here, so there is nothing extra to read.
         for (const String &address : StringParser::SplitString(recipients, _T(",")))
         {
            String recipientAddress = address;
            recipientAddress.Trim();

            if (recipientAddress.IsEmpty())
               continue;

            WebhookManager::Event event;
            event.name = WebhookManager::EventMessageQuarantined;
            event.sender = message->GetFromAddress();
            event.recipient = recipientAddress;
            event.queue_id = message->GetID();
            event.size = FileUtilities::FileSize(targetFile);
            event.subject = subject;
            event.reason = reason;
            event.number = score;
            event.has_number = true;

            WebhookManager::ScopeToLocalDomain(event.sender, event.recipient, event.domain, event.account);

            WebhookManager::Instance()->Raise(event);
         }

         return true;
      }
      catch (const boost::thread_interrupted&)
      {
         // A stop's interruption, not a fault: see WorkQueue::Stop.
         boost::this_thread::disable_interruption disabled;
         LOG_DEBUG("QuarantineStore::Quarantine: a stop of the servers interrupted the quarantining of a message; it was not quarantined.");
         throw;
      }
      catch (...)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6200, "QuarantineStore::Quarantine",
            "An unexpected error occurred while quarantining a message. The message was not quarantined.");

         return false;
      }
   }

   bool
   QuarantineStore::ReadRow_(std::shared_ptr<DALRecordset> recordset, QuarantinedMessage &out_message)
   {
      if (!recordset || recordset->IsEOF())
         return false;

      out_message.id = recordset->GetInt64Value("quarantineid");
      out_message.file_name = recordset->GetStringValue("quarantinefilename");
      out_message.sender = recordset->GetStringValue("quarantinesender");
      out_message.recipients = recordset->GetStringValue("quarantinerecipients");
      out_message.subject = recordset->GetStringValue("quarantinesubject");
      out_message.reason = recordset->GetStringValue("quarantinereason");
      out_message.score = (int) recordset->GetLongValue("quarantinescore");
      out_message.size = (int) recordset->GetLongValue("quarantinesize");
      out_message.created = recordset->GetStringValue("quarantinecreated");
      out_message.helo = recordset->GetStringValue("quarantinehelo");

      return true;
   }

   bool
   QuarantineStore::OpenFile_(__int64 id, String &out_file, String &out_error)
   {
      QuarantinedMessage quarantined;

      if (!GetById(id, quarantined))
      {
         out_error = "No quarantined message with that id.";
         return false;
      }

      out_file = FileUtilities::Combine(GetQuarantineDirectory(), quarantined.file_name);

      if (!FileUtilities::Exists(out_file))
      {
         out_error = "The quarantined message file is missing. Delete the entry to clear it.";
         return false;
      }

      return true;
   }

   bool
   QuarantineStore::ReadHeaders(__int64 id, String &out_headers, bool &out_truncated, String &out_error)
   {
      out_headers.Empty();
      out_truncated = false;

      String file;
      if (!OpenFile_(id, file, out_error))
         return false;

      const AnsiString header = PersistentMessage::LoadHeader(file, false);

      // As the bytes are: UTF-8 where a header carries any (RFC 6532), and every
      // other byte kept as the character of that value, so that an 8-bit header
      // in another character set is shown garbled rather than dropped.
      String text;
      if (!Unicode::MultiByteToWide(header, text))
      {
         text.Empty();
         for (int i = 0; i < header.GetLength(); i++)
            text += (wchar_t) (unsigned char) header[i];
      }

      out_truncated = CutAt(text, MaxHeaderCharacters);

      out_headers = text;
      return true;
   }

   String
   QuarantineStore::TextOfHtml(const String &html)
   {
      // What is between <style> and </style>, <script> and </script>, and
      // <head> and </head> is not text a person reads; ContentPolicy's stripper
      // takes out the tags and would leave it. Each is found by its whole name
      // (FindTag): found by "<head", a <header> was cut to the next </head - its
      // text lost, and with no </head after it the rest of the message.
      String lower = html;
      lower.MakeLower();

      String kept;
      kept.reserve(html.GetLength());

      // Where each element next opens at or after the position reached: looked
      // for again only once the position has passed it, and never again once
      // there is no more of it (-1). Looked for afresh on every pass, the two
      // elements a part does not have were read to its end once for every one
      // it does, so a held message of many <style></style> pairs kept the
      // thread that previews it for half a minute a megabyte, rising with the
      // square of the size.
      const TCHAR *const elements[] = { _T("style"), _T("script"), _T("head") };
      const int head = 2;
      int next[] = { -2, -2, -2 };

      // A head's end tag may be left out, as HTML allows, and the head then
      // ends where the body starts; with neither, only the <head> tag goes.
      // Where </head> and <body> next are is kept the way next[] is: looked for
      // afresh at every <head>, a message of many heads and no </head> was read
      // to its end once for every one of them.
      int headEnd = -2;
      int bodyStart = -2;

      int position = 0;
      while (position < html.GetLength())
      {
         int cut = -1;
         int element = -1;

         for (int e = 0; e < 3; e++)
         {
            if (next[e] == -2 || (next[e] >= 0 && next[e] < position))
               next[e] = FindTag(lower, String(_T("<")) + elements[e], position);

            if (next[e] >= 0 && (cut < 0 || next[e] < cut))
            {
               cut = next[e];
               element = e;
            }
         }

         if (cut < 0)
         {
            kept += html.Mid(position);
            break;
         }

         kept += html.Mid(position, cut - position);

         int end = -1;
         if (element == head)
         {
            if (headEnd == -2 || (headEnd >= 0 && headEnd < cut))
               headEnd = FindTag(lower, _T("</head"), cut);
            if (bodyStart == -2 || (bodyStart >= 0 && bodyStart < cut))
               bodyStart = FindTag(lower, _T("<body"), cut);

            if (bodyStart >= 0 && (headEnd < 0 || bodyStart < headEnd))
            {
               // The <body> tag itself is left to the stripper.
               kept += _T(" ");
               position = bodyStart;
               continue;
            }

            if (headEnd < 0)
            {
               const int tagClose = lower.Find(_T(">"), cut);
               if (tagClose < 0)
                  break;

               kept += _T(" ");
               position = tagClose + 1;
               continue;
            }

            end = headEnd;
         }
         else
         {
            end = FindTag(lower, String(_T("</")) + elements[element], cut);
            if (end < 0)
               break;
         }

         const int close = lower.Find(_T(">"), end);
         if (close < 0)
            break;

         kept += _T(" ");
         position = close + 1;
      }

      return ContentPolicy::StripHtml(kept);
   }

   bool
   QuarantineStore::ReadPreview(__int64 id, String &out_text, bool &out_truncated, bool &out_fromHtml, String &out_error)
   {
      out_text.Empty();
      out_truncated = false;
      out_fromHtml = false;

      String file;
      if (!OpenFile_(id, file, out_error))
         return false;

      std::shared_ptr<Message> message = std::shared_ptr<Message>(new Message(false));
      std::shared_ptr<MessageData> data = std::shared_ptr<MessageData>(new MessageData());

      if (!data->LoadFromMessage(file, message))
      {
         out_error = "The quarantined message could not be read. It may be larger than the parser reads, or damaged.";
         return false;
      }

      String text = data->GetBody();

      if (text.IsEmpty())
      {
         const String html = data->GetHTMLBody();
         if (!html.IsEmpty())
         {
            text = TextOfHtml(html);
            out_fromHtml = true;
         }
      }

      out_truncated = CutAt(text, MaxPreviewCharacters);

      out_text = text;
      return true;
   }

   std::vector<QuarantinedMessage>
   QuarantineStore::List(int maxCount)
   {
      std::vector<QuarantinedMessage> result;

      // Newest first, and bounded. A review queue is worked from the top, and an
      // unbounded select on a table nobody has pruned is how an administration tool
      // hangs on the one server that most needs looking at. The id breaks a tie, as
      // it does in Search: messages held in one second come in one order in both,
      // so that Search("", 500, 500) is the page after the one Refresh loads, and
      // ?offset=1000&limit=1000 the page after the list's, where a group held in
      // the second at the edge of a page could come in another order here and show
      // a message on both pages or on neither. BuildListCommand writes the
      // statement, and QuarantineSearchTester holds its order.
      SQLCommand command;
      BuildListCommand(command);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);

      if (!recordset)
         return result;

      while (!recordset->IsEOF() && (int) result.size() < maxCount)
      {
         QuarantinedMessage message;

         if (ReadRow_(recordset, message))
            result.push_back(message);

         recordset->MoveNext();
      }

      return result;
   }

   bool
   QuarantineStore::GetById(__int64 id, QuarantinedMessage &out_message)
   {
      SQLCommand command("select * from hm_quarantine where quarantineid = @ID");
      command.AddParameter("@ID", id);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);

      return ReadRow_(recordset, out_message);
   }

   namespace
   {
      // The columns ReadRow_ reads, named: the page's statement puts its row
      // limit in front of them.
      const TCHAR *const SearchColumns =
         _T("quarantineid, quarantinefilename, quarantinesender, quarantinerecipients, quarantinesubject, ")
         _T("quarantinereason, quarantinescore, quarantinesize, quarantinecreated, quarantinehelo");

      // The one order of every list of held messages - the list Refresh and
      // GET /api/v1/quarantine read, and each page of a search: newest first,
      // with the id to break a tie, so that messages held in one second come in
      // one order everywhere and a page after another starts where it ended.
      const TCHAR *const NewestFirst = _T(" order by quarantinecreated desc, quarantineid desc");
   }

   void
   QuarantineStore::BuildListCommand(SQLCommand &out_command)
   {
      out_command.SetQueryString(String(_T("select * from hm_quarantine")) + NewestFirst);
   }

   void
   QuarantineStore::BuildSearchCommands(const SearchRequest &request, bool topSyntax, SQLCommand &out_count, SQLCommand &out_page)
   {
      // Both sides lowered: PostgreSQL's LIKE compares case, and the other three
      // backends' default collations do not, so a search for "Invoice" found
      // "invoice" on three of them and nothing on the fourth. On MySQL and
      // PostgreSQL, whose values are written into the statement, the database
      // lowers the text as it lowers the column - lower(@QSENDER) - where
      // MakeLower lowered it here: MakeLower goes by the classic locale the
      // server runs in, which leaves a capital outside ASCII as it is (every one
      // on Linux, the Latin ones on Windows), while PostgreSQL's lower() folds
      // the column's, so a text with a capital U umlaut (U+00DC) in it found
      // nothing there, not even itself. SQL Server and Compact compare without
      // case, and their text is lowered here as the rest of the tree lowers a
      // LIKE's: nothing puts lower() on a parameter that Compact binds by name.
      // The text's own wildcards are written as the characters they are
      // (EscapeLikeText).
      const bool databaseLowers = !topSyntax;

      String text = request.text;
      text.Trim();
      if (!databaseLowers)
         text.MakeLower();

      String recipient = request.recipient;
      recipient.Trim();
      if (!databaseLowers)
         recipient.MakeLower();

      // One column's LIKE, the pattern the named parameter's value.
      const auto like = [databaseLowers](const TCHAR *column, const TCHAR *name)
      {
         const String pattern = databaseLowers ? String(_T("lower(")) + name + _T(")") : String(name);
         return String(_T("lower(")) + column + _T(") like ") + pattern + _T(" escape '/'");
      };

      // Every LIKE has a parameter of its own, added in the order the statement
      // names them, and no name is the start of another. ADOConnection binds SQL
      // Server's parameters by position - it writes a ? wherever it finds a name
      // and adds one value for each parameter - so a name used twice is two
      // places and one value, and two names added out of order take each other's
      // values. Compact binds them by name and MySQL and PostgreSQL have them
      // written in, so SQL Server alone would show the mistake, and no bench runs
      // on it: QuarantineSearchTester holds the order.
      String where;
      std::vector<std::pair<AnsiString, String>> parameters;

      if (!text.IsEmpty())
      {
         const String contains = _T("%") + SQLStatement::EscapeLikeText(text) + _T("%");

         where = _T("(") + like(_T("quarantinesender"), _T("@QSENDER"));
         parameters.push_back(std::make_pair(AnsiString("@QSENDER"), contains));

         // A search confined to one recipient leaves the recipients out: they say
         // who else a message was for, which is not that recipient's to learn.
         if (recipient.IsEmpty())
         {
            where += _T(" or ") + like(_T("quarantinerecipients"), _T("@QRECIPIENTS"));
            parameters.push_back(std::make_pair(AnsiString("@QRECIPIENTS"), contains));
         }

         where += _T(" or ") + like(_T("quarantinesubject"), _T("@QSUBJECT"));
         parameters.push_back(std::make_pair(AnsiString("@QSUBJECT"), contains));

         where += _T(" or ") + like(_T("quarantinereason"), _T("@QREASON")) + _T(")");
         parameters.push_back(std::make_pair(AnsiString("@QREASON"), contains));
      }

      if (!recipient.IsEmpty())
      {
         // One of the recipients, whole: the list alone, at its start, at its end
         // or inside it. The store writes the list comma separated with nothing
         // between (Quarantine_, RemoveRecipient_), so these four are every place
         // an address can be, and bob@example.com is not found in
         // xbob@example.com or bob@example.community. Four patterns rather than a
         // comma joined on each side of the column, which the four backends each
         // spell their own way.
         const String exact = SQLStatement::EscapeLikeText(recipient);

         if (!where.IsEmpty())
            where += _T(" and ");

         where += _T("(") + like(_T("quarantinerecipients"), _T("@QFORONE"));
         parameters.push_back(std::make_pair(AnsiString("@QFORONE"), exact));

         where += _T(" or ") + like(_T("quarantinerecipients"), _T("@QFORFIRST"));
         parameters.push_back(std::make_pair(AnsiString("@QFORFIRST"), exact + _T(",%")));

         where += _T(" or ") + like(_T("quarantinerecipients"), _T("@QFORLAST"));
         parameters.push_back(std::make_pair(AnsiString("@QFORLAST"), _T("%,") + exact));

         where += _T(" or ") + like(_T("quarantinerecipients"), _T("@QFORMIDDLE")) + _T(")");
         parameters.push_back(std::make_pair(AnsiString("@QFORMIDDLE"), _T("%,") + exact + _T(",%")));
      }

      const String filter = where.IsEmpty() ? String() : String(_T(" where ")) + where;

      out_count.SetQueryString(String(_T("select count(*) as quarantinecount from hm_quarantine")) + filter);

      // Newest first, with the id to break a tie (NewestFirst): several messages
      // held in one second must come in one order on every page, or a page could
      // show one twice and another never. The page is offset + count rows of
      // which the first offset are passed over, as the audit trail's are: SQL
      // Server Compact has no OFFSET.
      const int rows = request.offset + request.count;

      String page;
      if (topSyntax)
         page.Format(_T("select top (%d) %s from hm_quarantine%s%s"), rows, SearchColumns, filter.c_str(), NewestFirst);
      else
         page.Format(_T("select %s from hm_quarantine%s%s limit %d"), SearchColumns, filter.c_str(), NewestFirst, rows);

      out_page.SetQueryString(page);

      for (const std::pair<AnsiString, String> &parameter : parameters)
      {
         out_count.AddParameter(parameter.first, parameter.second);
         out_page.AddParameter(parameter.first, parameter.second);
      }
   }

   bool
   QuarantineStore::Search(const SearchRequest &request, std::vector<QuarantinedMessage> &out_page, int &out_matches)
   {
      out_page.clear();
      out_matches = 0;

      String text = request.text;
      text.Trim();

      String recipient = request.recipient;
      recipient.Trim();

      if (text.GetLength() > MaxSearchText || request.offset < 0 || request.offset > MaxSearchOffset ||
          request.count < 1 || request.count > MaxSearchPage)
         return false;

      const DatabaseSettings::SQLDBType type = IniFileSettings::Instance()->GetDatabaseType();
      const bool topSyntax = type == DatabaseSettings::TypeMSSQLServer || type == DatabaseSettings::TypeMSSQLCompactEdition;

      SQLCommand countCommand;
      SQLCommand pageCommand;
      BuildSearchCommands(request, topSyntax, countCommand, pageCommand);

      std::shared_ptr<DALRecordset> counted = Application::Instance()->GetDBManager()->OpenRecordset(countCommand);
      if (!counted)
         return false;

      out_matches = counted->IsEOF() ? 0 : (int) counted->GetLongValue("quarantinecount");

      if (request.offset >= out_matches)
         return true;

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(pageCommand);
      if (!recordset)
      {
         out_matches = 0;
         return false;
      }

      int index = 0;
      while (!recordset->IsEOF() && (int) out_page.size() < request.count)
      {
         if (index >= request.offset)
         {
            QuarantinedMessage message;

            // A search confined to a recipient says so again of every row it
            // sends, as the portal's list did of every row it read: the statement
            // found the address whole, and a row shown to the wrong person is
            // somebody else's mail.
            if (ReadRow_(recordset, message) && (recipient.IsEmpty() || IsRecipient(message, recipient)))
               out_page.push_back(message);
         }

         index++;
         recordset->MoveNext();
      }

      return true;
   }

   int
   QuarantineStore::GetCount()
   {
      SQLCommand command("select count(*) as quarantinecount from hm_quarantine");

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);

      if (!recordset)
         return 0;

      return (int) recordset->GetLongValue("quarantinecount");
   }

   bool
   QuarantineStore::Delete(__int64 id)
   {
      QuarantinedMessage message;

      if (!GetById(id, message))
         return false;

      // The row goes first. A row with no file is a broken entry an administrator can
      // see and remove; a file with no row is invisible, and nothing will ever sweep
      // it - so if only one of the two can succeed, losing the file is the better
      // failure.
      SQLCommand command("delete from hm_quarantine where quarantineid = @ID");
      command.AddParameter("@ID", id);

      if (!Application::Instance()->GetDBManager()->Execute(command))
         return false;

      String file = FileUtilities::Combine(GetQuarantineDirectory(), message.file_name);

      if (FileUtilities::Exists(file))
         FileUtilities::DeleteFile(file);

      return true;
   }

   bool
   QuarantineStore::Release(__int64 id, String &out_error)
   {
      QuarantinedMessage quarantined;

      if (!GetById(id, quarantined))
      {
         out_error = "No quarantined message with that id.";
         return false;
      }

      String sourceFile = FileUtilities::Combine(GetQuarantineDirectory(), quarantined.file_name);

      if (!FileUtilities::Exists(sourceFile))
      {
         out_error = "The quarantined message file is missing, so there is nothing to release. "
                     "Delete the entry to clear it.";
         return false;
      }

      if (quarantined.recipients.IsEmpty())
      {
         out_error = "The quarantined message has no recorded recipients, so there is nowhere to release it to.";
         return false;
      }

      // Re-injected into the delivery queue rather than written straight into a
      // mailbox, so that the things which SHOULD still apply do: rules, forwarding,
      // quota, the account actually existing. What it deliberately skips is the spam
      // filtering, and that skip is free rather than argued for - the verdict that
      // quarantined this message was reached during the SMTP conversation, and a
      // message entering at the queue never goes near it. A spam verdict therefore
      // cannot re-quarantine; the two holds that run in the queue - a recipient's
      // block list and a rule's quarantine action - are told apart by the mark
      // below, since either would otherwise hold the message again on the way
      // back, for ever.
      std::shared_ptr<Message> message = std::shared_ptr<Message>(new Message());
      message->SetState(Message::Delivering);
      message->SetFromAddress(quarantined.sender);

      // Marked, so that the recipients' own block lists (LocalDelivery) and the
      // rule that held it (SMTPDeliverer::RunGlobalRules_) do not hold it a
      // second time on its way back in, and the rule's warn_sender does not
      // write to the sender twice: a release is a person overruling the filters,
      // and a message released from a hold would otherwise go straight back
      // into the quarantine it was just taken out of. Saved with the row, in
      // messageflags, so the deliverer reads it back.
      message->SetReleasedFromQuarantine(true);

      // And, for an entry the sandbox's verdict put here, the one mark the
      // sandbox skips a message for: its remembered malicious verdict would
      // otherwise put it straight back. Any other release - the spam
      // quarantine's, a rule's, a block list's - never met the sandbox, and is
      // examined on its way back in like new mail (AntiVirus/Sandbox.h).
      message->SetReleasedFromSandboxQuarantine(Sandbox::IsSandboxQuarantineReason(quarantined.reason));

      const String targetFile = MessageStore::WorkingFile(message);

      if (!FileUtilities::Copy(sourceFile, targetFile, true))
      {
         out_error = "The quarantined message could not be copied into the delivery queue.";
         return false;
      }

      message->SetSize(MessageStore::FileSize(targetFile));
      message->SetFromOutside(ReleasedCopyIsFromOutside_(quarantined.file_name, targetFile));
      ApplyStoredMarks_(quarantined, *message, targetFile);

      RecipientParser recipientParser;

      for (const String &address : StringParser::SplitString(quarantined.recipients, ","))
      {
         bool recipientOk = false;
         recipientParser.CreateMessageRecipientList(address, message->GetRecipients(), recipientOk);
      }

      if (message->GetRecipients()->GetCount() == 0)
      {
         FileUtilities::DeleteFile(targetFile);
         out_error = "None of the recorded recipients could be resolved, so the message was not released.";
         return false;
      }

      if (!PersistentMessage::SaveObject(message))
      {
         FileUtilities::DeleteFile(targetFile);
         out_error = "The released message could not be saved to the delivery queue.";
         return false;
      }

      Application::Instance()->SubmitPendingEmail();

      // Removed only after the copy is safely queued. The other order loses the
      // message entirely if the save fails, which is the one thing a quarantine must
      // never do - an administrator reached for it precisely because it mattered.
      Delete(id);

      LOG_APPLICATION(Formatter::Format("Quarantine: released message {0} from {1} to {2}.",
         id, quarantined.sender, quarantined.recipients));

      return true;
   }

   int
   QuarantineStore::DeleteExpired()
   {
      int retentionDays = IniFileSettings::Instance()->GetQuarantineRetentionDays();

      if (retentionDays <= 0)
         return 0;

      // Every row is read and the cutoff applied in C++ rather than in the WHERE
      // clause. Date arithmetic and date literals are the least portable corner of
      // SQL and this server supports four engines; a sweep that silently deletes
      // nothing on one of them - or everything - is a worse outcome than reading a
      // table that the sweep itself keeps small.
      //
      // One row at a time, too, because each owns a file. A set-based DELETE would
      // strip the index and leave the messages on disk for ever.
      SQLCommand command("select quarantineid, quarantinecreated from hm_quarantine");

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);

      if (!recordset)
         return 0;

      // Compared as timestamp strings, which sounds lazy and is actually the most
      // robust option here. The stored format is "YYYY-MM-DD HH:MM:SS", where
      // lexicographic order IS chronological order, and doing the comparison in C++
      // keeps four SQL dialects' date arithmetic out of a sweep that deletes mail.
      // DateTime looks like the obvious type for this and is not: it has no
      // conversion to DATE, so the natural expression does not compile.
      std::time_t cutoffTime = std::time(0) - (static_cast<std::time_t>(retentionDays) * 24 * 60 * 60);

      struct tm cutoffTm = {};

      if (localtime_s(&cutoffTm, &cutoffTime) != 0)
         return 0;

      char cutoffText[32];

      if (strftime(cutoffText, sizeof(cutoffText), "%Y-%m-%d %H:%M:%S", &cutoffTm) == 0)
         return 0;

      // Local time on both sides: the stored value came from Time::GetCurrentDateTime,
      // which is local, so a UTC cutoff would be right for most of the year and wrong
      // by an hour twice.
      const String cutoff = cutoffText;

      std::vector<__int64> expired;

      while (!recordset->IsEOF())
      {
         String created = recordset->GetStringValue("quarantinecreated");

         // An unreadable stamp is left alone rather than swept. A row whose date
         // cannot be read is already odd, and deleting the message it points at -
         // which is the only copy - on the strength of that would be the wrong way to
         // resolve the oddity.
         if (created.GetLength() >= 19 && created.Compare(cutoff) < 0)
            expired.push_back(recordset->GetInt64Value("quarantineid"));

         recordset->MoveNext();
      }

      int deleted = 0;

      for (__int64 id : expired)
      {
         if (Delete(id))
            deleted++;
      }

      return deleted;
   }
   bool
   QuarantineStore::IsRecipient(const QuarantinedMessage &message, const String &address)
   {
      // The recorded list is the envelope recipients as accepted, comma
      // separated. An address matches whole and case-insensitively; a prefix
      // or a substring does not, so bob@ cannot see bobby@'s mail.
      std::vector<String> recipients = StringParser::SplitString(message.recipients, ",");

      for (size_t i = 0; i < recipients.size(); i++)
      {
         String candidate = recipients[i];
         candidate.Trim();

         if (candidate.CompareNoCase(address) == 0)
            return true;
      }

      return false;
   }

   bool
   QuarantineStore::ReleaseTo(__int64 id, const String &recipient, String &out_error)
   {
      // Release for one recipient of a message that may have several. The
      // delivery is the administrator's release confined to that address; the
      // entry then loses that address and stays for the others, or goes when
      // it was the last. A user releasing their own copy must not deliver
      // somebody else's, and must not take somebody else's chance to.
      QuarantinedMessage quarantined;
      if (!GetById(id, quarantined))
      {
         out_error = "No quarantined message with that id.";
         return false;
      }

      if (!IsRecipient(quarantined, recipient))
      {
         out_error = "That address is not a recipient of the quarantined message.";
         return false;
      }

      String sourceFile = FileUtilities::Combine(GetQuarantineDirectory(), quarantined.file_name);
      if (!FileUtilities::Exists(sourceFile))
      {
         out_error = "The quarantined message file is missing, so there is nothing to release. "
                     "Delete the entry to clear it.";
         return false;
      }

      std::shared_ptr<Message> message = std::shared_ptr<Message>(new Message());
      message->SetState(Message::Delivering);
      message->SetFromAddress(quarantined.sender);

      // Marked, so that the recipients' own block lists do not hold it a second
      // time on its way back in: a release is a person overruling the filters,
      // and a message released from a block would otherwise go straight back
      // into the quarantine it was just taken out of.
      message->SetReleasedFromQuarantine(true);

      // The sandbox's own mark, as Release sets it: only for an entry its
      // verdict made.
      message->SetReleasedFromSandboxQuarantine(Sandbox::IsSandboxQuarantineReason(quarantined.reason));

      const String targetFile = MessageStore::WorkingFile(message);
      if (!FileUtilities::Copy(sourceFile, targetFile, true))
      {
         out_error = "The quarantined message could not be copied into the delivery queue.";
         return false;
      }

      message->SetSize(MessageStore::FileSize(targetFile));
      message->SetFromOutside(ReleasedCopyIsFromOutside_(quarantined.file_name, targetFile));
      ApplyStoredMarks_(quarantined, *message, targetFile);

      RecipientParser recipientParser;
      bool recipientOk = false;
      recipientParser.CreateMessageRecipientList(recipient, message->GetRecipients(), recipientOk);

      if (message->GetRecipients()->GetCount() == 0)
      {
         FileUtilities::DeleteFile(targetFile);
         out_error = "The recipient could not be resolved, so the message was not released.";
         return false;
      }

      if (!PersistentMessage::SaveObject(message))
      {
         FileUtilities::DeleteFile(targetFile);
         out_error = "The released message could not be saved to the delivery queue.";
         return false;
      }

      Application::Instance()->SubmitPendingEmail();

      RemoveRecipient_(id, quarantined, recipient);

      LOG_APPLICATION(Formatter::Format("Quarantine: released message {0} from {1} to {2}, at that recipient's request.",
         id, quarantined.sender, recipient));

      return true;
   }

   bool
   QuarantineStore::DiscardFor(__int64 id, const String &recipient)
   {
      // The recipient's own copy is given up: their address leaves the entry,
      // and the entry and its file go only when no recipient is left.
      QuarantinedMessage quarantined;
      if (!GetById(id, quarantined))
         return false;

      if (!IsRecipient(quarantined, recipient))
         return false;

      return RemoveRecipient_(id, quarantined, recipient);
   }

   bool
   QuarantineStore::RemoveRecipient_(__int64 id, const QuarantinedMessage &quarantined, const String &recipient)
   {
      std::vector<String> recipients = StringParser::SplitString(quarantined.recipients, ",");
      String remaining;

      for (size_t i = 0; i < recipients.size(); i++)
      {
         String candidate = recipients[i];
         candidate.Trim();

         if (candidate.IsEmpty() || candidate.CompareNoCase(recipient) == 0)
            continue;

         if (!remaining.IsEmpty())
            remaining += ",";
         remaining += candidate;
      }

      if (remaining.IsEmpty())
         return Delete(id);

      SQLCommand command("update hm_quarantine set quarantinerecipients = @RECIPIENTS where quarantineid = @ID");
      command.AddParameter("@RECIPIENTS", remaining);
      command.AddParameter("@ID", id);

      if (!Application::Instance()->GetDBManager()->Execute(command))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5795, "QuarantineStore::RemoveRecipient_",
            Formatter::Format("The recipient list of quarantined message {0} could not be updated after {1} acted on it, so that address still sees the message.", id, recipient));
         return false;
      }

      return true;
   }

   namespace
   {
      void FailSearch(const std::string &what)
      {
         throw std::logic_error("QuarantineSearchTester: " + what);
      }

      std::string Utf8Of(const String &text)
      {
         return std::string(Unicode::ToUtf8(text).c_str());
      }

      // Every parameter of the command named once in its statement, in the order
      // the parameters were added, none the start of another, and nothing else
      // in the statement that ADOConnection would take for a name.
      void ExpectBindsInOrder(const SQLCommand &command, const std::string &which)
      {
         const String query = command.GetQueryString();

         std::vector<String> names;
         int previous = -1;

         for (const SQLParameter &parameter : command.GetParameters())
         {
            const String name = parameter.GetName();

            const int at = query.Find(name.c_str());
            if (at < 0)
               FailSearch(which + ": " + Utf8Of(name) + " is not in the statement.");

            if (query.Find(name.c_str(), at + 1) >= 0)
               FailSearch(which + ": " + Utf8Of(name) + " is in the statement twice, which SQL Server would bind as two values and one parameter.");

            if (at <= previous)
               FailSearch(which + ": " + Utf8Of(name) + " is added out of the statement's order, which SQL Server would bind to another's place.");

            previous = at;
            names.push_back(name);
         }

         for (const String &one : names)
         {
            for (const String &other : names)
            {
               if (one != other && other.GetLength() > one.GetLength() && other.Left(one.GetLength()) == one)
                  FailSearch(which + ": " + Utf8Of(one) + " is the start of " + Utf8Of(other) + ".");
            }
         }

         int tokens = 0;
         for (int i = 0; i < query.GetLength(); i++)
         {
            if (query[i] == L'@')
               tokens++;
         }

         if (tokens != (int) names.size())
            FailSearch(which + ": the statement names " + std::to_string(tokens) + " parameters and the command carries " + std::to_string(names.size()) + ".");
      }

      void ExpectParameters(const SQLCommand &command, const std::vector<std::pair<String, String>> &expected, const std::string &which)
      {
         const std::list<SQLParameter> &parameters = command.GetParameters();
         if (parameters.size() != expected.size())
            FailSearch(which + ": " + std::to_string(parameters.size()) + " parameters, not " + std::to_string(expected.size()) + ".");

         size_t i = 0;
         for (const SQLParameter &parameter : parameters)
         {
            if (parameter.GetName() != expected[i].first || parameter.GetType() != SQLParameter::ParamTypeString ||
                parameter.GetStringValue() != expected[i].second)
            {
               FailSearch(which + ": parameter " + std::to_string(i + 1) + " is " + Utf8Of(parameter.GetName()) + " = [" + Utf8Of(parameter.GetStringValue()) +
                  "], not " + Utf8Of(expected[i].first) + " = [" + Utf8Of(expected[i].second) + "].");
            }

            i++;
         }
      }

      void ExpectText(const String &actual, const String &expected, const std::string &which)
      {
         if (actual != expected)
            FailSearch(which + " is [" + Utf8Of(actual) + "], not [" + Utf8Of(expected) + "].");
      }
   }

   void
   QuarantineSearchTester::Test()
   {
      const String columns = _T("quarantineid, quarantinefilename, quarantinesender, quarantinerecipients, quarantinesubject, ")
                             _T("quarantinereason, quarantinescore, quarantinesize, quarantinecreated, quarantinehelo");
      const String order = _T(" order by quarantinecreated desc, quarantineid desc");

      // A text with the wildcards of every backend in it, in capitals, around
      // spaces: lowered and trimmed, each wildcard escaped, and looked for in all
      // four columns, each with its own parameter.
      {
         QuarantineStore::SearchRequest request;
         request.text = _T("  A_B%[Promo]  ");

         SQLCommand count;
         SQLCommand page;
         QuarantineStore::BuildSearchCommands(request, true, count, page);

         const String where = _T(" where (lower(quarantinesender) like @QSENDER escape '/' or lower(quarantinerecipients) like @QRECIPIENTS escape '/' ")
                              _T("or lower(quarantinesubject) like @QSUBJECT escape '/' or lower(quarantinereason) like @QREASON escape '/')");

         ExpectText(count.GetQueryString(), _T("select count(*) as quarantinecount from hm_quarantine") + where, "the count of a text search");
         ExpectText(page.GetQueryString(), _T("select top (100) ") + columns + _T(" from hm_quarantine") + where + order, "the page of a text search");

         const String contains = _T("%a/_b/%/[promo]%");
         const std::vector<std::pair<String, String>> expected =
         {
            { _T("@QSENDER"), contains }, { _T("@QRECIPIENTS"), contains }, { _T("@QSUBJECT"), contains }, { _T("@QREASON"), contains }
         };

         ExpectParameters(count, expected, "the count of a text search");
         ExpectParameters(page, expected, "the page of a text search");
         ExpectBindsInOrder(count, "the count of a text search");
         ExpectBindsInOrder(page, "the page of a text search");
      }

      // Confined to one recipient: the recipients are not searched for the text,
      // and the address is found whole in any of its four places, as LIMIT on
      // MySQL and PostgreSQL, the page as long as the offset and the count. The
      // database lowers both sides there, so the text and the address go as they
      // were typed, trimmed and escaped.
      {
         QuarantineStore::SearchRequest request;
         request.text = _T("Invoice");
         request.recipient = _T(" Bob_1@Example.COM ");
         request.offset = 200;
         request.count = 50;

         SQLCommand count;
         SQLCommand page;
         QuarantineStore::BuildSearchCommands(request, false, count, page);

         const String where = _T(" where (lower(quarantinesender) like lower(@QSENDER) escape '/' or lower(quarantinesubject) like lower(@QSUBJECT) escape '/' ")
                              _T("or lower(quarantinereason) like lower(@QREASON) escape '/') and (lower(quarantinerecipients) like lower(@QFORONE) escape '/' ")
                              _T("or lower(quarantinerecipients) like lower(@QFORFIRST) escape '/' or lower(quarantinerecipients) like lower(@QFORLAST) escape '/' ")
                              _T("or lower(quarantinerecipients) like lower(@QFORMIDDLE) escape '/')");

         ExpectText(count.GetQueryString(), _T("select count(*) as quarantinecount from hm_quarantine") + where, "the count of a recipient's search");
         ExpectText(page.GetQueryString(), _T("select ") + columns + _T(" from hm_quarantine") + where + order + _T(" limit 250"), "the page of a recipient's search");

         const std::vector<std::pair<String, String>> expected =
         {
            { _T("@QSENDER"), _T("%Invoice%") }, { _T("@QSUBJECT"), _T("%Invoice%") }, { _T("@QREASON"), _T("%Invoice%") },
            { _T("@QFORONE"), _T("Bob/_1@Example.COM") }, { _T("@QFORFIRST"), _T("Bob/_1@Example.COM,%") },
            { _T("@QFORLAST"), _T("%,Bob/_1@Example.COM") }, { _T("@QFORMIDDLE"), _T("%,Bob/_1@Example.COM,%") }
         };

         ExpectParameters(count, expected, "the count of a recipient's search");
         ExpectParameters(page, expected, "the page of a recipient's search");
         ExpectBindsInOrder(count, "the count of a recipient's search");
         ExpectBindsInOrder(page, "the page of a recipient's search");
      }

      // A capital outside ASCII, as LIMIT on MySQL and PostgreSQL: left to the
      // database to lower, which folds it as it folds the column's, where lowered
      // here by the classic locale it stayed a capital and PostgreSQL found
      // nothing, not even the text itself.
      {
         QuarantineStore::SearchRequest request;
         request.text = _T("\u00DCBERWEISUNG");

         SQLCommand count;
         SQLCommand page;
         QuarantineStore::BuildSearchCommands(request, false, count, page);

         const String where = _T(" where (lower(quarantinesender) like lower(@QSENDER) escape '/' or lower(quarantinerecipients) like lower(@QRECIPIENTS) escape '/' ")
                              _T("or lower(quarantinesubject) like lower(@QSUBJECT) escape '/' or lower(quarantinereason) like lower(@QREASON) escape '/')");

         ExpectText(count.GetQueryString(), _T("select count(*) as quarantinecount from hm_quarantine") + where, "the count of a search with a capital outside ASCII");

         const String contains = _T("%\u00DCBERWEISUNG%");
         const std::vector<std::pair<String, String>> expected =
         {
            { _T("@QSENDER"), contains }, { _T("@QRECIPIENTS"), contains }, { _T("@QSUBJECT"), contains }, { _T("@QREASON"), contains }
         };

         ExpectParameters(count, expected, "the count of a search with a capital outside ASCII");
         ExpectParameters(page, expected, "the page of a search with a capital outside ASCII");
         ExpectBindsInOrder(page, "the page of a search with a capital outside ASCII");
      }

      // A recipient's every message: the address alone.
      {
         QuarantineStore::SearchRequest request;
         request.recipient = _T("ada@example.test");
         request.count = 1000;

         SQLCommand count;
         SQLCommand page;
         QuarantineStore::BuildSearchCommands(request, true, count, page);

         const String where = _T(" where (lower(quarantinerecipients) like @QFORONE escape '/' or lower(quarantinerecipients) like @QFORFIRST escape '/' ")
                              _T("or lower(quarantinerecipients) like @QFORLAST escape '/' or lower(quarantinerecipients) like @QFORMIDDLE escape '/')");

         ExpectText(page.GetQueryString(), _T("select top (1000) ") + columns + _T(" from hm_quarantine") + where + order, "the page of a recipient's messages");
         ExpectBindsInOrder(count, "the count of a recipient's messages");
         ExpectBindsInOrder(page, "the page of a recipient's messages");
      }

      // No text and no recipient: every message, with no condition at all, a page
      // after the first.
      {
         QuarantineStore::SearchRequest request;
         request.text = _T("   ");
         request.offset = 500;
         request.count = 500;

         SQLCommand count;
         SQLCommand page;
         QuarantineStore::BuildSearchCommands(request, true, count, page);

         ExpectText(count.GetQueryString(), _T("select count(*) as quarantinecount from hm_quarantine"), "the count of every message");
         ExpectText(page.GetQueryString(), _T("select top (1000) ") + columns + _T(" from hm_quarantine") + order, "the page of every message");

         if (!count.GetParameters().empty() || !page.GetParameters().empty())
            FailSearch("a search for every message carries parameters.");

         ExpectBindsInOrder(page, "the page of every message");
      }

      // The list Refresh and GET /api/v1/quarantine read (List): every held
      // message in the pages' order above, newest first with a tie broken by the
      // id, so that the search for every message from offset 500 is the page
      // after the one Refresh loads. The bench's SQL Server Compact returns
      // messages held in one second newest id first without the id as well, and
      // the Linux suite cannot seed rows held in one second, so in both gates only
      // the statement shows the order.
      {
         SQLCommand list;
         QuarantineStore::BuildListCommand(list);

         ExpectText(list.GetQueryString(), _T("select * from hm_quarantine") + order, "the list's statement");

         if (!list.GetParameters().empty())
            FailSearch("the list's statement carries parameters.");
      }

      // Every bound is held before a database is asked: the text's length, the
      // page's size and the offset's range.
      {
         std::vector<QuarantinedMessage> found;
         int matches = 7;

         QuarantineStore::SearchRequest tooLong;
         tooLong.text = String(QuarantineStore::MaxSearchText + 1, _T('x'));
         if (QuarantineStore::Search(tooLong, found, matches) || matches != 0)
            FailSearch("a text longer than MaxSearchText was searched for.");

         QuarantineStore::SearchRequest noPage;
         noPage.count = 0;
         if (QuarantineStore::Search(noPage, found, matches))
            FailSearch("a page of no messages was asked for.");

         QuarantineStore::SearchRequest bigPage;
         bigPage.count = QuarantineStore::MaxSearchPage + 1;
         if (QuarantineStore::Search(bigPage, found, matches))
            FailSearch("a page over MaxSearchPage was asked for.");

         QuarantineStore::SearchRequest before;
         before.offset = -1;
         if (QuarantineStore::Search(before, found, matches))
            FailSearch("a negative offset was asked for.");

         QuarantineStore::SearchRequest beyond;
         beyond.offset = QuarantineStore::MaxSearchOffset + 1;
         if (QuarantineStore::Search(beyond, found, matches))
            FailSearch("an offset over MaxSearchOffset was asked for.");
      }
   }
}
