// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "Message.h"


#include "../Util/File.h"
#include "../Util/GUIDCreator.h"

#include "Attachments.h"
#include "Attachment.h"

#include "MessageRecipients.h"
#include "../Mime/Mime.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   Message::Message(bool generateFileName) 
   {
      Initialize(generateFileName);
   }

   Message::Message() 
   {
      Initialize(true);
   }

   Message::~Message()
   {

   }

   Message::Message(const Message& other) :
      BusinessObject<Message>(other)
   {
      message_size_ = other.message_size_;
      create_time_ = other.create_time_;
      // Every new member must be copied here by hand or it silently vanishes
      // on copy - which is exactly how the save date went missing on the
      // SEARCH path while the freshly-loaded FETCH path still had it.
      save_date_ = other.save_date_;
      email_id_ = other.email_id_;
      filename_ = other.filename_;
      from_address_ = other.from_address_;

      message_account_id_ = other.message_account_id_;
      message_folder_id_ = other.message_folder_id_;

      message_state_ = other.message_state_;
      no_of_retries_ = other.no_of_retries_;
      flags_.store(other.flags_.load());
      // The other message may be a live one another session is changing.
      keywords_ = other.GetKeywords();

      uid_ = other.uid_;
      message_modseq_.store(other.message_modseq_.load());

      submitter_account_id_ = other.submitter_account_id_;
      auth_domains_ = other.auth_domains_;
      from_outside_ = other.from_outside_;
      received_spf_result_ = other.received_spf_result_;
      relaying_domain_id_ = other.relaying_domain_id_;

      // Not copied, on purpose: copied_from_id_, which belongs to the one
      // object CopyToIMAPFolder saves (a copy saved as well would carry the
      // recall rows twice), the recipients, which start empty on the copy for
      // its maker to fill (ExternalDelivery's sealed copy), and the two mutexes.
   }

   namespace
   {
      // The SPF results Message::GetReceivedSpfResult answers, RFC 8601's keywords,
      // each by the byte that stands for it in received_spf_result_; 0 is none
      // recorded.
      const wchar_t *const SpfResultKeywords[] =
      {
         L"", L"pass", L"fail", L"softfail", L"neutral", L"none", L"temperror", L"permerror"
      };

      const size_t SpfResultKeywordCount = sizeof(SpfResultKeywords) / sizeof(SpfResultKeywords[0]);
   }

   String
   Message::GetReceivedSpfResult() const
   {
      return received_spf_result_ < SpfResultKeywordCount ? String(SpfResultKeywords[received_spf_result_]) : String();
   }

   namespace
   {
      // The place of the keyword result names in SpfResultKeywords, or 0.
      unsigned char SpfResultIndex(const String &result)
      {
         String keyword = result;
         keyword.Trim();
         if (keyword.IsEmpty())
            return 0;

         for (size_t i = 1; i < SpfResultKeywordCount; i++)
         {
            // The length as well: CompareNoCase reads both as C strings, and a value
            // with a NUL in it would compare as what comes before the NUL.
            const String candidate(SpfResultKeywords[i]);
            if (keyword.GetLength() == candidate.GetLength() && keyword.CompareNoCase(candidate) == 0)
               return static_cast<unsigned char>(i);
         }

         return 0;
      }
   }

   void
   Message::SetReceivedSpfResult(const String &result)
   {
      received_spf_result_ = SpfResultIndex(result);
   }

   String
   Message::SpfResultKeyword(const String &result)
   {
      return String(SpfResultKeywords[SpfResultIndex(result)]);
   }

   String
   Message::GetKeywords() const
   {
      boost::lock_guard<boost::mutex> guard(keywords_mutex_);
      return keywords_;
   }

   void
   Message::SetKeywords(const String &keywords)
   {
      boost::lock_guard<boost::mutex> guard(keywords_mutex_);
      keywords_ = keywords;
   }

   std::vector<String>
   Message::SplitKeywords_(const String &keywords)
   {
      std::vector<String> list;
      String current;
      for (int i = 0; i < keywords.GetLength(); i++)
      {
         wchar_t c = keywords[i];
         if (c == ' ')
         {
            if (!current.IsEmpty())
               list.push_back(current);
            current = _T("");
         }
         else
            current += c;
      }
      if (!current.IsEmpty())
         list.push_back(current);
      return list;
   }

   String
   Message::JoinKeywords_(const std::vector<String> &keywords)
   {
      // A keyword the same as one before it, case ignored, is left out. It is
      // compared with those kept, which the length limit holds to a few
      // hundred: every keyword before it is one of them or the same as one, the
      // empty ones aside. It was compared with every keyword before it, kept or
      // not, so a STORE of a few hundred thousand repeated keywords - a client's
      // command - took a time that grew with the square of their number, for
      // each message it named.
      String joined;
      std::vector<String> kept;
      for (size_t i = 0; i < keywords.size(); i++)
      {
         if (keywords[i].IsEmpty())
            continue;
         bool seen = false;
         for (size_t j = 0; j < kept.size() && !seen; j++)
            seen = kept[j].CompareNoCase(keywords[i]) == 0;
         if (seen)
            continue;
         String next = joined.IsEmpty() ? keywords[i] : joined + _T(" ") + keywords[i];
         if (next.GetLength() > KeywordsMaximumLength)
            break;
         joined = next;
         kept.push_back(keywords[i]);
      }
      return joined;
   }

   std::vector<String>
   Message::GetKeywordList() const
   {
      return SplitKeywords_(GetKeywords());
   }

   void
   Message::SetKeywordList(const std::vector<String> &keywords)
   {
      String joined = JoinKeywords_(keywords);

      boost::lock_guard<boost::mutex> guard(keywords_mutex_);
      keywords_ = joined;
   }

   bool
   Message::HasKeyword(const String &keyword) const
   {
      std::vector<String> list = GetKeywordList();
      for (size_t i = 0; i < list.size(); i++)
      {
         if (list[i].CompareNoCase(keyword) == 0)
            return true;
      }
      return false;
   }

   bool
   Message::AddKeyword(const String &keyword)
   {
      if (keyword.IsEmpty())
         return true;

      // The test and the write under one hold: see GetKeywords.
      boost::lock_guard<boost::mutex> guard(keywords_mutex_);

      std::vector<String> list = SplitKeywords_(keywords_);
      for (size_t i = 0; i < list.size(); i++)
      {
         if (list[i].CompareNoCase(keyword) == 0)
            return true;
      }

      String next = keywords_.IsEmpty() ? keyword : keywords_ + _T(" ") + keyword;
      if (next.GetLength() > KeywordsMaximumLength)
         return false;
      keywords_ = next;
      return true;
   }

   void
   Message::RemoveKeyword(const String &keyword)
   {
      boost::lock_guard<boost::mutex> guard(keywords_mutex_);

      std::vector<String> list = SplitKeywords_(keywords_);
      std::vector<String> kept;
      for (size_t i = 0; i < list.size(); i++)
      {
         if (list[i].CompareNoCase(keyword) != 0)
            kept.push_back(list[i]);
      }
      keywords_ = JoinKeywords_(kept);
   }


   void
   Message::Initialize(bool generateFileName)
   {
      message_account_id_ = 0;
      message_state_ = Created;
      message_folder_id_ = 0;
      message_size_ = 0;
      no_of_retries_ = 0;
      uid_ = 0;
      message_modseq_.store(0);

      if (generateFileName)
      {
         filename_ = GenerateFileName();
      }

      // Message flags.
      flags_.store(0);

      // RFC 3030: not binary until a MAIL FROM says so. Also the value every
      // message re-read from the database gets, since the mark does not persist
      // yet - see the declaration.
   }

   std::shared_ptr<MessageRecipients>
   Message::GetRecipients()
   {
      if (!recipients_)
         recipients_ = std::shared_ptr<MessageRecipients>(new MessageRecipients);

      return recipients_;
   }

   bool
   Message::GetFlag_(int iFlag) const
   {
      return (GetFlags() & iFlag) != 0;
   }

   void
   Message::SetFlag_(int iFlag, bool bSet)
   {
      // One bit, atomically: see GetFlags in Message.h.
      SetFlagBits((short) iFlag, bSet ? (short) iFlag : (short) 0);
   }

   void
   Message::SetFlagBits(short mask, short values)
   {
      short current = flags_.load();
      short wanted = 0;
      do
      {
         wanted = (short) ((current & ~mask) | (values & mask));
      } while (!flags_.compare_exchange_weak(current, wanted));
   }

   bool 
   Message::GetFlagSeen() const
   {
      return GetFlag_(FlagSeen);
   }

   void 
   Message::SetFlagSeen(bool bNewVal)
   {
      SetFlag_(FlagSeen, bNewVal);
   }

   bool 
   Message::GetFlagDeleted() const
   {
      return GetFlag_(FlagDeleted);
   }

   void 
   Message::SetFlagDeleted(bool bNewVal)
   {
      SetFlag_(FlagDeleted, bNewVal);
   }

   bool 
   Message::GetFlagDraft() const
   {
      return GetFlag_(FlagDraft);
   }

   void 
   Message::SetFlagDraft(bool bNewVal)
   {
      SetFlag_(FlagDraft, bNewVal);
   }


   bool 
   Message::GetFlagAnswered() const
   {
      return GetFlag_(FlagAnswered);
   }

   void 
   Message::SetFlagAnswered(bool bNewVal)
   {
      SetFlag_(FlagAnswered, bNewVal);
   }

   bool 
   Message::GetFlagFlagged() const
   {
      return GetFlag_(FlagFlagged);
   }

   void 
   Message::SetFlagFlagged(bool bNewVal)
   {
      SetFlag_(FlagFlagged, bNewVal);
   }

   bool 
   Message::GetFlagRecent() const
   {
      return GetFlag_(FlagRecent);
   }

   void 
   Message::SetFlagRecent(bool bNewVal)
   {
      SetFlag_(FlagRecent, bNewVal);
   }

   bool 
   Message::GetFlagVirusScan() const
   {
      return GetFlag_(FlagVirusScan);
   }

   void 
   Message::SetFlagVirusScan(bool bNewVal)
   {
      SetFlag_(FlagVirusScan, bNewVal);
   }

   bool 
   Message::GetFlagSpam() const
   {
      return GetFlag_(FlagSpam);
   }

   void 
   Message::SetFlagSpam(bool bNewVal)
   {
      SetFlag_(FlagSpam, bNewVal);
   }

   bool 
   Message::XMLStore(XNode *pParentNode, int iOptions)
   {
      XNode *pNode = pParentNode->AppendChild(_T("Message"));

      String sFilename = filename_;
      sFilename.Replace(IniFileSettings::Instance()->GetDataDirectory() + FileUtilities::PathSeparator, _T(""));

      pNode->AppendAttr(_T("CreateTime"), String(create_time_));
      pNode->AppendAttr(_T("Filename"), FileUtilities::GetFileNameFromFullPath(sFilename));
      pNode->AppendAttr(_T("FromAddress"), String(from_address_));
      pNode->AppendAttr(_T("State"), StringParser::IntToString(message_state_));
      pNode->AppendAttr(_T("Size"), StringParser::IntToString(message_size_));
      pNode->AppendAttr(_T("NoOfRetries"), StringParser::IntToString(no_of_retries_));
      pNode->AppendAttr(_T("Flags"), StringParser::IntToString((int) GetFlags()));

      // The flags that are not system flags: the webmail's labels, its second and
      // third stars, its follow-up flag, mute and pin, and every keyword an IMAP
      // client has ever set with STORE. They live in messagekeywords beside
      // messageflags and are written by the same statement - but this method wrote
      // only the flags, so from the day keywords arrived a restore silently returned
      // every message with its labels stripped. Nothing announced it, because a
      // message that comes back unlabelled still comes back.
      pNode->AppendAttr(_T("Keywords"), GetKeywords());

      pNode->AppendAttr(_T("ID"), StringParser::IntToString(id_));
      pNode->AppendAttr(_T("UID"), StringParser::IntToString(uid_));

      return true;
   }

   bool
   Message::XMLLoad(XNode *pNode, int iOptions)
   {
      create_time_ = pNode->GetAttrValue(_T("CreateTime"));
      filename_ = pNode->GetAttrValue(_T("Filename"));
      from_address_ = pNode->GetAttrValue(_T("FromAddress"));
      message_state_ = (State) _ttoi(pNode->GetAttrValue(_T("State")));
      message_size_ = _ttoi(pNode->GetAttrValue(_T("Size")));
      no_of_retries_ = _ttoi(pNode->GetAttrValue(_T("NoOfRetries")));
      SetFlags((short) _ttoi(pNode->GetAttrValue(_T("Flags"))));

      // An archive taken before the attribute existed simply has none, and
      // GetAttrValue answers an empty string for that - which is exactly "no
      // keywords" and is what those messages had. No version check is needed.
      SetKeywords(pNode->GetAttrValue(_T("Keywords")));

      uid_ = _ttoi(pNode->GetAttrValue(_T("UID")));

      return true;
   }

   String
   Message::GetPartialFileName() const
   {
      return filename_;
   }

   void
   Message::SetPartialFileName(const String &sFileName) 
   {
      filename_ = sFileName;
   }

   String 
   Message::GenerateFileName()
   {
      return GUIDCreator::GetGUID() + ".eml";
   }
}
