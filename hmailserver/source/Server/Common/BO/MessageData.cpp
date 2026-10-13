// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "MessageData.h"

#include "../Mime/Mime.h"
#include "../Mime/MimeCode.h"
#include "../Util/Charset.h"
#include "../BO/Message.h"
#include "../BO/Account.h"
#include "../BO/Attachments.h"
#include "../Util/Time.h"
#include "../Util/GUIDCreator.h"
#include "../Util/Utilities.h"
#include "../Util/File.h"
#include "../Util/ByteBuffer.h"

#include <cstring>
#include <stdexcept>

// Test
#include "../Persistence/PersistentMessage.h"
#include "../Persistence/MessageStore.h"
#include "../Persistence/MessageStoreObjects.h"
#include "../Util/MailboxKeys.h"
#include "../Rules/RuleGuard.h"
#include "../../SMTP/RecipientParser.h"

#define XHMAILSERVER_LOOPCOUNT _T("X-hMailServer-LoopCount")
#define AUTOSUBMITTED_HEADER _T("Auto-Submitted")

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   MessageData::MessageData()
   {
      encode_fields_ = true;
      unfold_with_space_ = true;

      // A MessageData that is built up from scratch (a bounce, an auto-reply, a
      // distribution-list copy) never calls LoadFromMessage, and writing that is
      // exactly what it is for - so the default has to be "loaded".
      load_failed_ = false;
      large_ = LargeMessage::Refuse;

      mime_mail_ = std::shared_ptr<MimeBody>(new MimeBody);
   }

   bool
   MessageData::LoadFromMessage(std::shared_ptr<const Account> account, std::shared_ptr<Message> pMessage, LargeMessage large)
   {
      String fileName = PersistentMessage::GetFileName(account, pMessage);

      return LoadFromMessage(fileName, pMessage, large);
   }


   bool
   MessageData::LoadFromMessage(const String &fileName, std::shared_ptr<Message> pMessage, LargeMessage large)
   {
      message_ = pMessage;
      message_file_name_ = fileName;
      large_ = large;

      // Note that the previous contents are dropped here, before either of the failure
      // returns below: after a failed load - including a failed RefreshFromMessage on a
      // MessageData that was perfectly good a moment ago - this object holds an empty
      // message, not the one it held before.
      mime_mail_ = std::shared_ptr<MimeBody>(new MimeBody);
      load_failed_ = true;

      // What is parsed: the message's own file, or for a message the store keeps
      // in another form its original bytes, decoded into memory and never
      // written out (MessageStore.h) - so that reading a message of a mailbox
      // encrypted at rest puts none of it on the disk. A stored file that
      // cannot be read is refused there, with the reason in the error log, and
      // is not treated as a message being composed.
      source_ = std::shared_ptr<MessageStoreParseSource>(new MessageStoreParseSource(message_file_name_));
      if (!source_->IsReady())
         return false;

      encrypted_file_ = String();
      encrypted_to_.clear();

      // Larger than a message is parsed whole: refused, as it always was, or -
      // for a caller that asked - its header alone (LargeMessage). A message of
      // this size is never one being composed, so the answer is the header or
      // nothing.
      if (source_->InMemory() ? source_->GetOriginal()->size() > (size_t) MaxParseBytes : FileUtilities::FileSize(message_file_name_) > MaxParseBytes)
      {
         if (large != LargeMessage::HeaderOnly)
            return false;

         const MimeLoadResult loaded = source_->InMemory()
            ? mime_mail_->LoadHeaderFromMemory(source_->GetOriginal())
            : mime_mail_->LoadHeaderFromFile(message_file_name_);

         if (loaded != MimeLoadResult::Loaded)
         {
            const String reason = loaded == MimeLoadResult::OpenFailed
               ? String(_T("the file could not be opened or read"))
               : Formatter::Format("its first {0} MB hold no end of the header, or a header field that could not be parsed", (int) (MimeBody::MaxHeaderOnlyBytes / (static_cast<size_t>(1024) * 1024)));

            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7841, "MessageData::LoadFromMessage",
               Formatter::Format("The header of the message file {0}, which is larger than the {1} MB a message is read whole at, could not be read: {2}. The message was not loaded, and nothing is written over it.",
                  message_file_name_, (int) (MaxParseBytes / (1024 * 1024)), reason));

            mime_mail_ = std::shared_ptr<MimeBody>(new MimeBody);
            return false;
         }

         load_failed_ = false;
         return true;
      }

      bool bNewMessage = false;
      try
      {
         // Three answers, not two. LoadFromFile used to return false for "I could not
         // open the file" and true for everything else including a parse that threw,
         // and this branch read the first as "there is no file yet, this is a message
         // being composed". A message file briefly held open by a virus scanner or a
         // backup therefore produced an empty MessageData reported as a successful
         // load - which the rule engine then evaluated every criterion against, and
         // which the forwarding and attachment-stripping paths then wrote back over
         // the real message.
         //
         // Whether a missing file means "compose" or "the message has been lost" is
         // this function's question and not the parser's, so the existence test lives
         // here. It races, as any such test does; losing the race means treating a
         // file that has just been deleted as a new message, which is exactly what
         // happened before, so the race cannot make anything worse.
         // A container's bytes are parsed where they are, in memory; one of a
         // mailbox encrypted at rest is never copied aside to "Problematic
         // messages" in the clear when the parse fails.
         const MimeLoadResult loaded = source_->InMemory()
            ? mime_mail_->LoadFromMemory(source_->GetOriginal(), message_file_name_, source_->GetEncryptedTo().empty())
            : mime_mail_->LoadFromFile(message_file_name_);

         switch (loaded)
         {
         case MimeLoadResult::Loaded:
            break;

         case MimeLoadResult::OpenFailed:
            if (FileUtilities::Exists(message_file_name_))
            {
               ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6060, "MessageData::LoadFromMessage",
                  "The message file " + message_file_name_ + " exists but could not be opened, so the message could not be loaded. It has not been treated as a new message - the caller is told the load failed, so that nothing is written over it.");
               return false;
            }

            // Not on the disk: while an object store is named the message may
            // be there, put by the committer, and one the store could not be
            // asked about is not taken for one being composed either - the
            // caller is told the load failed (docs/ObjectStorage.md).
            if (MessageStoreObjects::Active())
            {
               MessageStore::ClearUnavailable();
               if (MessageStore::Exists(message_file_name_) || MessageStore::Unavailable())
                  return false;
            }

            // No file at all: a message being composed from nothing.
            bNewMessage = true;
            break;

         case MimeLoadResult::ParseFailed:
            // LoadFromFile has already reported 4228 and copied the file aside. It
            // holds whatever was parsed before the throw, which must not be written.
            return false;
         }
      }
      catch (...)
      {
         try
         {
            String sFileNameExclPath = FileUtilities::GetFileNameFromFullPath(message_file_name_);

            // The bytes that were parsed, which are the message's whatever form
            // the store keeps it in - and never, in the clear, a message of a
            // mailbox encrypted at rest, whose stored file is what to look at.
            String sMessageBackupPath = FileUtilities::Combine(FileUtilities::Combine(IniFileSettings::Instance()->GetLogDirectory(), "Problematic messages"), sFileNameExclPath);
            if (!source_->InMemory())
               FileUtilities::Copy(message_file_name_, sMessageBackupPath, true);
            else if (source_->GetEncryptedTo().empty())
               FileUtilities::WriteToFile(sMessageBackupPath, AnsiString(source_->GetOriginal()->data(), source_->GetOriginal()->size()));
            else
               sMessageBackupPath = _T("(not copied: the mailbox is encrypted at rest)");

            String sErrorMessage;
            sErrorMessage.Format(_T("An unknown error occurred while loading message. File: %s. Backuped to: %s"), message_file_name_.c_str(), sMessageBackupPath.c_str());

            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 4218, "MessageData::LoadFromMessage", sErrorMessage);
         }
         catch (...)
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 4218, "MessageData::LoadFromMessage", "An unknown error occurred while loading message.");
         }

         return false;
      }

      if (bNewMessage)
      {
         // For new messages, we default to UTF-8. This way client
         // can put any values into headers without having to care
         // about setting the correct character set first.
         SetCharset("utf-8");
         SetFieldValue(CMimeConst::MimeVersion(), "1.0");
      }

      load_failed_ = false;

      return true;
   }


   bool 
   MessageData::RefreshFromMessage()
   {
      if (!message_)
         return false;

      return LoadFromMessage(message_file_name_, message_, large_);
   }

   bool
   MessageData::IsHeaderOnly() const
   {
      return mime_mail_ && mime_mail_->IsHeaderOnly();
   }


   MessageData::~MessageData()
   {

   }

   void 
   MessageData::DeleteField(const AnsiString &headerName)
   {
      // All of them in one pass (MimeHeader::DeleteFields says why not one by one).
      mime_mail_->DeleteFields(headerName);
   }

   String
   MessageData::GetSubject()  const
   {
      return GetFieldValue("Subject");
   }

   void 
   MessageData::SetSubject(const String &sSubject)
   {
      SetFieldValue("Subject", sSubject);
   }

   String
   MessageData::GetCharset()  const
   {
      return mime_mail_->GetCharset();
   }

   void 
   MessageData::SetCharset(const String &sCharset)
   {
      AnsiString sCharsetStr = sCharset;
      mime_mail_->SetCharset(sCharsetStr);
   }

   String
   MessageData::GetHeader()  const
   {
      if (encode_fields_)
         return mime_mail_->GetUnicodeHeaderContents();
      else
         return mime_mail_->GetHeaderContents();
   }

   String
   MessageData::GetSentTime()  const
   {
      return GetFieldValue("Date");
   }   

   void
   MessageData::SetSentTime(const String &sSentTime)
   {
      String sValue = sSentTime;
      if (sValue.IsEmpty())
      {
         // Use default value
         sValue = Time::GetCurrentMimeDate();
      }

      SetFieldValue("Date", sValue);
   }

   void 
   MessageData::SetFieldValue(const String &sField, const String &sValue)
   {
      if (encode_fields_)
         mime_mail_->SetUnicodeFieldValue(sField, sValue, "");
      else
         mime_mail_->SetRawFieldValue(sField, sValue, "");
   }

   String
   MessageData::GetTo() const
   {
      return GetFieldValue("To");
   } 

   String
   MessageData::GetReturnPath() const
   {
      return GetFieldValue("Return-Path");
   } 

   void 
   MessageData::SetReturnPath(const String &sReturnPath)
   {
      SetFieldValue("Return-Path", "<" + sReturnPath + ">");
   }

   String
   MessageData::GetCC() const
   {
      return GetFieldValue("CC");
   } 

   void 
   MessageData::SetCC(const String &sCC)
   {
      SetFieldValue("CC", sCC);
   }

   String
   MessageData::GetBCC() const
   {
      return GetFieldValue("BCC");
   } 

   void 
   MessageData::SetBCC(const String &sBCC)
   {
      SetFieldValue("BCC", sBCC);
   }

   String
   MessageData::GetFrom() const
   {
      return GetFieldValue("From");
   } 

   void 
   MessageData::SetFrom(const String &sFrom)
   {
      SetFieldValue("From", sFrom);
   }

   void 
   MessageData::SetTo(const String &sTo)
   {
      SetFieldValue("To", sTo);
   }


   String
   MessageData::GetFieldValue(const String &sName) const
   {
      String sRetVal;
      if (encode_fields_)
         sRetVal = mime_mail_->GetUnicodeFieldValue(sName);
      else
         sRetVal = mime_mail_->GetRawFieldValue(sName);

      return sRetVal;
   }

   int
   MessageData::GetFieldOccurrenceCount(const String &sName) const
   {
      if (!mime_mail_)
         return 0;

      // Counted over the header this object already parsed, rather than re-reading and
      // re-parsing the message file: the comparison is the same case-insensitive one
      // MimeHeader::FindField uses, so the two agree on what counts as a match.
      AnsiString sFieldName = sName;

      int count = 0;
      for (MimeField &field : mime_mail_->Fields())
      {
         if (!::_stricmp(field.GetName(), sFieldName))
            count++;
      }

      return count;
   }

   int 
   MessageData::GetSize() const
   {
      return message_->GetSize();
   }

   std::shared_ptr<Attachments>
   MessageData::GetAttachments()
   {
      if (!attachments_)
      {
         attachments_ = std::shared_ptr<Attachments>(new Attachments(mime_mail_, this));
         
         // Load attachments.
         attachments_->Load();
      }

      return attachments_;
   }

   String 
   MessageData::GetBody() const
   {
      std::shared_ptr<MimeBody> pPart = GetBodyTextPlainPart();

      if (pPart)
      {
         if (encode_fields_)
            return pPart->GetUnicodeText();
         else
            return pPart->GetRawText();
      }
      else
         return "";
    
   }

   void
   MessageData::SetBody(const String &sBody)
   {
      std::shared_ptr<MimeBody> pPart = GetBodyTextPlainPart();

      if (!pPart)
         pPart = CreatePart("text/plain");

      if (sBody.IsEmpty())
      {
         pPart->SetRawText("");
         return;
      }

      String sModifiedBody = sBody;
      if (sModifiedBody.Right(2) != _T("\r\n"))
      {
         // Add carriage return.
         sModifiedBody += "\r\n";
      }

      // Set the text to the part
      if (encode_fields_)
         pPart->SetUnicodeText(sModifiedBody);
      else
         pPart->SetRawText(sModifiedBody);
   }

   String 
   MessageData::GetHTMLBody() const
   {
      std::shared_ptr<MimeBody> pPart = GetBodyTextHtmlPart();

      if (pPart)
      {
         if (encode_fields_)
            return pPart->GetUnicodeText();
         else
            return pPart->GetRawText();
      }

      return "";
   }

   void
   MessageData::SetHTMLBody(const String &sNewVal)
   {
      std::shared_ptr<MimeBody> pHTMLPart = GetBodyTextHtmlPart();

      if (!pHTMLPart)
      {
         // Create a new part.
         pHTMLPart = CreatePart("text/html");
      }

      if (sNewVal.IsEmpty())
      {
         pHTMLPart->SetRawText("");
         return;
      }

      String sModifiedBody = sNewVal;
      if (sModifiedBody.Right(2) != _T("\r\n"))
      {
         // Add carriage return.
         sModifiedBody += "\r\n";
      }

      // Set the text to the part
      if (encode_fields_)
         pHTMLPart->SetUnicodeText(sModifiedBody);
      else
         pHTMLPart->SetRawText(sModifiedBody);
   }

   String
   MessageData::GetAttachmentText() const
   {
      // Walk every part. IsAttachment() is the MIME layer's own rule: a
      // Content-Disposition of attachment, or any discrete type other than text
      // that has no disposition at all. The body parts SEARCH already scans are
      // text parts that are NOT attachments, so this collects exactly the
      // complement: the text-typed parts that ARE attachments.
      const int MaxCharacters = 1024 * 1024;
      String result;
      if (!mime_mail_)
         return result;

      std::vector<std::shared_ptr<MimeBody> > pending;
      pending.push_back(mime_mail_);
      while (!pending.empty())
      {
         std::shared_ptr<MimeBody> part = pending.back();
         pending.pop_back();

         for (std::shared_ptr<MimeBody> child = part->FindFirstPart(); child; child = part->FindNextPart())
         {
            pending.push_back(child);

            if (!child->IsAttachment())
               continue;
            if (child->GetMainType() != "text")
               continue;

            // GetUnicodeText decodes the transfer encoding only when the part
            // declares a charset, and a text attachment most often does not: its
            // Content-Type carries just the file name, so a base64 minutes.txt
            // would come back as base64. Decode it here and read it as UTF-8, of
            // which the RFC 2046 default of US-ASCII is a subset.
            String text;
            if (!child->GetCharset().empty())
               text = child->GetUnicodeText();
            else
            {
               AnsiString raw = child->GetRawText();
               if (!raw.IsEmpty())
               {
                  AnsiString decoded;
                  MimeCodeBase *coder = MimeEnvironment::CreateCoder(child->GetTransferEncoding());
                  coder->SetInput(raw, raw.GetLength(), false);
                  coder->GetOutput(decoded);
                  delete coder;
                  text = Charset::ToWideChar(decoded, "utf-8");
               }
            }
            if (text.IsEmpty())
               continue;

            if (result.GetLength() + text.GetLength() > MaxCharacters)
            {
               result += text.Mid(0, MaxCharacters - result.GetLength());
               return result;
            }
            // A separator, so a word at the end of one file and one at the start
            // of the next never merge into a token neither file contains.
            if (!result.IsEmpty())
               result += _T("\n");
            result += text;
         }
      }
      return result;
   }

   std::shared_ptr<MimeBody>
   MessageData::FindPartNoRecurse(std::shared_ptr<MimeBody> parent, const AnsiString &sType) const
   {
      std::shared_ptr<MimeBody> pPart = parent->FindFirstPart();

      while (pPart)
      {
         AnsiString sContentType = pPart->GetCleanContentType();
         sContentType.MakeLower();

         if (sContentType.CompareNoCase(sType) == 0)
         {
            // Create a new part in the end of the message. We have already
            // looked for a part with the proper type without success. This
            // is probably a new attachment.
            return pPart;
         }

         pPart = parent->FindNextPart();
      }
      
      std::shared_ptr<MimeBody> empty;
      return empty;
   }

   // A multipart/related part whose root - its first part, RFC 2387's default -
   // is the formatted body: the HTML itself (Thunderbird's shape, and what
   // InlineImageParts writes), or an alternative with the HTML in it (Outlook's).
   // The images beside that root are the ones the HTML names by cid:.
   bool
   MessageData::IsFormattedRelated_(std::shared_ptr<MimeBody> part) const
   {
      String type = part->GetCleanContentType();
      if (type.CompareNoCase(_T("multipart/related")) != 0)
         return false;

      std::shared_ptr<MimeBody> root = part->FindFirstPart();
      if (!root || root->IsAttachment())
         return false;

      String rootType = root->GetCleanContentType();
      if (rootType.CompareNoCase(_T("text/html")) == 0)
         return true;

      if (rootType.CompareNoCase(_T("multipart/alternative")) != 0)
         return false;

      for (std::shared_ptr<MimeBody> child = root->FindFirstPart(); child; child = root->FindNextPart())
      {
         if (child->GetCleanContentType().CompareNoCase(_T("text/html")) == 0 && !child->IsAttachment())
            return true;
      }

      return false;
   }

   // The formatted body among a part's own children, for CreatePart to carry
   // across the rebuild: an HTML part that is not an attachment, else a related
   // part holding the HTML with its images - moved whole, since taking the HTML
   // out of it and leaving the related part behind drops the HTML and the images
   // together (a related part is multipart, so not an attachment, so not kept) -
   // else whatever text/html part is there, as CreatePart always took.
   std::shared_ptr<MimeBody>
   MessageData::FindFormattedBodyNoRecurse_(std::shared_ptr<MimeBody> parent) const
   {
      std::vector<std::shared_ptr<MimeBody> > children;
      for (std::shared_ptr<MimeBody> child = parent->FindFirstPart(); child; child = parent->FindNextPart())
         children.push_back(child);

      for (std::shared_ptr<MimeBody> child : children)
      {
         if (child->GetCleanContentType().CompareNoCase(_T("text/html")) == 0 && !child->IsAttachment())
            return child;
      }

      for (std::shared_ptr<MimeBody> child : children)
      {
         if (IsFormattedRelated_(child))
            return child;
      }

      return FindPartNoRecurse(parent, "text/html");
   }

   // The message is the related part (an HTML-only message with images, as
   // InlineImageParts leaves one, or Outlook's related around the alternative):
   // its parts move into a new related part, keeping the type and start the
   // message named, and the message is left with no parts for CreatePart to
   // rebuild around it.
   std::shared_ptr<MimeBody>
   MessageData::LiftRelatedBody_()
   {
      const AnsiString type = mime_mail_->GetParameter(CMimeConst::ContentType(), "type");
      const AnsiString start = mime_mail_->GetParameter(CMimeConst::ContentType(), "start");

      std::shared_ptr<MimeBody> related = std::shared_ptr<MimeBody>(new MimeBody);
      related->SetContentType("multipart/related", "");
      if (!type.IsEmpty())
         related->SetParameter(CMimeConst::ContentType(), "type", type);
      if (!start.IsEmpty())
         related->SetParameter(CMimeConst::ContentType(), "start", start);
      related->SetRawText("This is a multi-part message.\r\n\r\n");

      std::vector<std::shared_ptr<MimeBody> > children;
      for (std::shared_ptr<MimeBody> child = mime_mail_->FindFirstPart(); child; child = mime_mail_->FindNextPart())
         children.push_back(child);

      for (std::shared_ptr<MimeBody> child : children)
      {
         mime_mail_->ErasePart(child);
         related->AddPart(child);
      }

      related->SetBoundary(NULL);
      return related;
   }

   std::shared_ptr<MimeBody> 
   MessageData::CreatePart(const String &sContentType)
   {
      // Step 1: Extract all parts.
      // Step 2: Delete everything
      // Step 3: Create the new type.
      // Step 4: Insert the new type and all others.

      // Create a new part by rebuilding the message more or less from scratch.
      AnsiString sMainBodyType = mime_mail_->GetCleanContentType();
      AnsiString sMainBodyCharset = mime_mail_->GetCharset();
      sMainBodyType.MakeLower();
      
      // A message that is itself HTML with the images it shows is lifted into a
      // related part of its own first, which is then the formatted body.
      std::shared_ptr<MimeBody> liftedBody;
      if (sMainBodyType == "multipart/related" && IsFormattedRelated_(mime_mail_))
         liftedBody = LiftRelatedBody_();

      std::shared_ptr<MimeBody> textPart = FindPartNoRecurse(mime_mail_, "text/plain");
      std::shared_ptr<MimeBody> htmlPart = liftedBody ? liftedBody : FindFormattedBodyNoRecurse_(mime_mail_);

      std::shared_ptr<MimeBody> retValue;

      std::shared_ptr<MimeBody> alternativeNode = FindPartNoRecurse(mime_mail_, "multipart/alternative");
      if (alternativeNode)
      {
         if (!textPart) 
         {
            textPart = FindPartNoRecurse(alternativeNode, "text/plain");
            if (textPart)
               alternativeNode->ErasePart(textPart);
         }

         if (!htmlPart)
         {
            htmlPart = FindFormattedBodyNoRecurse_(alternativeNode);

            if (htmlPart)
               alternativeNode->ErasePart(htmlPart);
         }

         mime_mail_->ErasePart(alternativeNode);
      }

      if (!textPart && !htmlPart)
      {
         // We don't have any text or HMTL part. Copy the main content
         // of the message to a new part, if the main content isn't empty.
         if (sMainBodyType == "" || sMainBodyType == "text/plain")
         {
            if (mime_mail_->GetRawText().size() > 0)
            {
               textPart = std::shared_ptr<MimeBody>(new MimeBody);
               textPart->SetRawText(mime_mail_->GetRawText());
               textPart->SetContentType("text/plain", "");
               
               if (!sMainBodyCharset.IsEmpty())
                  textPart->SetCharset(sMainBodyCharset);

               AnsiString originalTransferEncoding = mime_mail_->GetTransferEncoding();
               if (!originalTransferEncoding.IsEmpty())
                  textPart->SetTransferEncoding(originalTransferEncoding);
            }
         }
         else if (sMainBodyType == "text/html")
         {
            if (mime_mail_->GetRawText().size() > 0)
            {
               htmlPart = std::shared_ptr<MimeBody>(new MimeBody);
               htmlPart->SetRawText(mime_mail_->GetRawText());
               htmlPart->SetContentType("text/html", "");
               
               if (!sMainBodyCharset.IsEmpty())
                  htmlPart->SetCharset(sMainBodyCharset);

               AnsiString originalTransferEncoding = mime_mail_->GetTransferEncoding();
               if (!originalTransferEncoding.IsEmpty())
                  htmlPart->SetTransferEncoding(originalTransferEncoding);
            }
         }
      }

      // Locate the other parts which are not text or html.
      //
      // When we get here, any alternative, text or html parts
      // should have been removed from the message already.
      //
      std::shared_ptr<MimeBody> part = mime_mail_->FindFirstPart();
      // In the order they appear in the message, and a part created here is
      // appended after them. Held in a set until 2026-09-08, which ordered
      // them by pointer address: the message was rebuilt on every call, so
      // adding a second attachment could reorder the first one, and which
      // attachment answered to index 0 was decided by the heap.
      std::vector<std::shared_ptr<MimeBody> > setAttachments;
      while (part)
      {
         if (part->IsAttachment())
            setAttachments.push_back(part);

         part = mime_mail_->FindNextPart();
      }

      // Remove all parts so that we can rebuild it again.
      mime_mail_->DeleteAll();

      // Create the brand new part...
      if (sContentType.CompareNoCase(_T("text/plain")) == 0)
      {
         HM_ASSERT(textPart == 0);

         if (setAttachments.size() == 0 && !htmlPart)
         {
            // Reuse the main part. There's no need to add a new one.
            textPart = mime_mail_;
            textPart->SetContentType("text/plain", "");
         }
         else
         {
            textPart = std::shared_ptr<MimeBody>(new MimeBody);
            textPart->SetContentType("text/plain", "");

            AnsiString transferEncoding = mime_mail_->GetTransferEncoding();
            if (!transferEncoding.IsEmpty())
               textPart->SetTransferEncoding(transferEncoding);

            if (!sMainBodyCharset.IsEmpty())
               textPart->SetCharset(sMainBodyCharset);

         }

         retValue = textPart;
      }
      else if (sContentType.CompareNoCase(_T("text/html")) == 0)
      {
         HM_ASSERT(htmlPart == 0);

         if (setAttachments.size() == 0 && !textPart)
         {
            // Reuse the main part. There's no need to add a new one.

            htmlPart = mime_mail_;
            htmlPart->SetContentType("text/html", "");
         }
         else
         {
            htmlPart = std::shared_ptr<MimeBody>(new MimeBody);
            htmlPart->SetContentType("text/html", "");

            AnsiString transferEncoding = mime_mail_->GetTransferEncoding();
            if (!transferEncoding.IsEmpty())
               htmlPart->SetTransferEncoding(transferEncoding);

            if (!sMainBodyCharset.IsEmpty())
               htmlPart->SetCharset(sMainBodyCharset);
         }

         retValue = htmlPart;
         
      }
      else
      {
         // create a new item. treat as an attachment.
         retValue = std::shared_ptr<MimeBody>(new MimeBody);
         setAttachments.push_back(retValue);
      }

      AnsiString mainBodyType;
      if (setAttachments.size() > 0)
         mainBodyType = "multipart/mixed";
      else if (textPart && htmlPart)
         mainBodyType = "multipart/alternative";
      else if (htmlPart)
         mainBodyType = "text/html";
      else
         mainBodyType = "text/plain";

      if (textPart && htmlPart)
      {
         if (mainBodyType == "multipart/mixed")
         {
            std::shared_ptr<MimeBody> alternativePart = std::shared_ptr<MimeBody>(new MimeBody);
            alternativePart->SetContentType("multipart/alternative", "");
            alternativePart->SetRawText("This is a multi-part message.\r\n\r\n");

            alternativePart->AddPart(textPart);
            alternativePart->AddPart(htmlPart);
            alternativePart->SetBoundary(NULL);

            mime_mail_->AddPart(alternativePart);
         }
         else
         {
            if (mime_mail_ != textPart)
               mime_mail_->AddPart(textPart);

            if (mime_mail_ != htmlPart)
               mime_mail_->AddPart(htmlPart);
         }
         
      }
      else if (textPart)
      {
         if (mainBodyType == "multipart/mixed")
         {
            if (mime_mail_ != textPart)
               mime_mail_->AddPart(textPart);
         }
      }
      else if (htmlPart)
      {
         if (mainBodyType == "multipart/mixed")
         {
            if (mime_mail_ != htmlPart)
               mime_mail_->AddPart(htmlPart);
         }
      }

      for(std::shared_ptr<MimeBody> pAttachment : setAttachments)
      {
         mime_mail_->AddPart(pAttachment);
      }

      mime_mail_->SetContentType(mainBodyType, ""); 
      if (!sMainBodyCharset.IsEmpty())
         mime_mail_->SetCharset(sMainBodyCharset);

      if (mime_mail_->GetPartCount() > 0)
      {
         mime_mail_->DeleteField(CMimeConst::TransferEncoding());
         mime_mail_->SetRawText("This is a multi-part message.\r\n\r\n");
         mime_mail_->SetBoundary(NULL);
      }

      return retValue;
   }

   std::shared_ptr<MimeBody>
   MessageData::GetBodyTextPlainPart() const
   {
      String part_type = mime_mail_->GetCleanContentType();
      if (part_type.IsEmpty())
         return mime_mail_;

      if (part_type == _T("text/plain"))
         return mime_mail_;

      return GetViewBodyPart_(0, mime_mail_, "text/plain");
   }

   std::shared_ptr<MimeBody>
   MessageData::GetBodyTextHtmlPart() const
   {
      String part_type = mime_mail_->GetCleanContentType();

      if (part_type == _T("text/html"))
         return mime_mail_;

      return GetViewBodyPart_(0, mime_mail_, "text/html");
   }

   std::shared_ptr<MimeBody> 
   MessageData::GetViewBodyPart_(int recursion_level, std::shared_ptr<MimeBody> source, const String &requested_content_type) const
   {
      if (recursion_level > 10)
         return nullptr;

      String part_type = source->GetCleanContentType();

      if (part_type.CompareNoCase(requested_content_type) == 0)
      {
         // This method should return the body shown to the user in his email client, not attachments which happens to match the requested content type.
         if (!source->IsAttachment())
            return source;
      }

      String main_type = source->GetMainType();
      if (main_type.CompareNoCase(_T("multipart")) == 0)
      {
         std::shared_ptr<MimeBody> sub_part = source->FindFirstPart();

         while (sub_part)
         {
            shared_ptr<MimeBody> found = GetViewBodyPart_(recursion_level + 1, sub_part, requested_content_type);
            if (found)
               return found;

            sub_part = source->FindNextPart();
         }
      }

      return nullptr;
   }



   std::shared_ptr<MimeBody>
   MessageData::FindPart(const String &sType) const
   {
      String sPartType = mime_mail_->GetCleanContentType();

      if (sPartType.CompareNoCase(sType) == 0)
         return mime_mail_;

      std::shared_ptr<MimeBody> pPart = mime_mail_->FindFirstPart();

      if (!pPart)
      {
         std::shared_ptr<MimeBody> pEmpty;
         return pEmpty;
      }

      while (pPart)
      {
         String sContentType = pPart->GetCleanContentType();

         if (sContentType.CompareNoCase(sType) == 0)
            return pPart;
         
         if (pPart->IsMultiPart())
         {
            std::shared_ptr<MimeBody> pSubPart = pPart->FindFirstPart();

            while (pSubPart)
            {
               String sSubContentType = pSubPart->GetCleanContentType();
               
               if (sSubContentType.CompareNoCase(sType) == 0)
                  return pSubPart;

            
               pSubPart = pPart->FindNextPart();
            }

         }
         
         pPart = mime_mail_->FindNextPart();
      }

      std::shared_ptr<MimeBody> pEmpty;
      return pEmpty;     

   }

   bool
   MessageData::Write(const String &fileName)
   {
      if (!CheckWritable_(fileName))
         return false;

      // A rewrite of what is stored encrypted at rest - the stored file this was
      // read from, or one this wrote encrypted - is encrypted again to the same
      // key, in memory, and written beside the stored file and renamed over it:
      // a rewrite (COM's Message.Save, a script's, a rule's on a stored
      // message) never puts the message on the disk in the clear, and one that
      // cannot be encrypted is not made (HM7362), leaving the stored file as it
      // was.
      if (source_ && source_->InMemory() && !source_->GetEncryptedTo().empty() && fileName == message_file_name_)
      {
         if (!WriteEncrypted_(fileName, source_->GetEncryptedTo(), MailboxKeys::CodeReEncrypt))
            return false;

         // The container named no part held once (an encrypted one never
         // does); said all the same, as for any container written over.
         source_->StoredFileReplaced();
         return true;
      }

      if (!encrypted_file_.IsEmpty() && fileName == encrypted_file_)
         return WriteEncrypted_(fileName, encrypted_to_, MailboxKeys::CodeReEncrypt);

      if (!mime_mail_->SaveAllToFile(fileName))
      {
         // The size below used to be taken from the file even when the write had
         // failed. For a message being written for the first time that is a size of
         // zero - the file is not there - which makes PersistentMessage::AddObject
         // refuse the row as "zero bytes" and the message is dropped without a word
         // about the real cause; for a rewrite in place it is whatever survived on
         // disk. Either way the row stops describing the message. Leave the size alone
         // and report the write, which the callers that ignore the return value would
         // otherwise turn into silence.
         ErrorManager::Instance()->ReportError(ErrorManager::High, 6011, "MessageData::Write",
            "Failed to write the message file " + fileName + ". The message data was not stored.");
         return false;
      }

      // The stored file this was read from, written over by the plain message: a
      // file the store kept in another form referenced parts held once, and this
      // name no longer does (MessageStore.h).
      if (source_ && source_->InMemory() && fileName == message_file_name_)
         source_->StoredFileReplaced();

      if (message_)
      {
         message_->SetSize(MessageStore::FileSize(fileName));
      }

      return true;
   }

   bool
   MessageData::WriteEncrypted(const String &fileName, const std::string &publicKey)
   {
      if (!CheckWritable_(fileName))
         return false;

      return WriteEncrypted_(fileName, publicKey, MailboxKeys::CodePlace);
   }

   bool
   MessageData::WriteToMailbox(const String &fileName, std::shared_ptr<const Account> account)
   {
      bool unknown = false;
      bool damaged = false;
      return WriteToMailbox(fileName, account, unknown, damaged);
   }

   bool
   MessageData::WriteToMailbox(const String &fileName, std::shared_ptr<const Account> account, bool &unknown, bool &damaged)
   {
      unknown = false;
      damaged = false;

      std::string publicKey;
      const MailboxKeys::Encryption encryption = account && message_ && message_->GetAccountID() == account->GetID()
         ? MailboxKeys::EncryptionFor(account, publicKey)
         : MailboxKeys::EncryptionNone;

      if (encryption == MailboxKeys::EncryptionOn)
         return WriteEncrypted(fileName, publicKey);

      if (encryption == MailboxKeys::EncryptionUnknown)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::High, MailboxKeys::CodePlace, "MessageData::WriteToMailbox",
            "The message " + fileName + " was not written now: whether its mailbox is encrypted at rest is not known, and it is not written in the clear on a guess.");
         unknown = true;
         return false;
      }

      if (encryption == MailboxKeys::EncryptionDamaged)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::High, MailboxKeys::CodePlace, "MessageData::WriteToMailbox",
            "The message " + fileName + " was not written: " + MailboxKeys::DamagedSentence());
         damaged = true;
         return false;
      }

      return Write(fileName);
   }

   bool
   MessageData::WriteEncrypted_(const String &fileName, const std::string &publicKey, int code)
   {
      std::shared_ptr<const std::string> serialized;
      if (!mime_mail_->SaveAllToMemory(fileName, serialized) || !serialized)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::High, 6011, "MessageData::Write",
            "Failed to write the message file " + fileName + ". The message data was not stored.");
         return false;
      }

      String problem;
      bool written = false;
      if ((unsigned __int64) serialized->size() > MessageStoreFormat::MaximumOriginalBytes)
         problem = _T("it is larger than an encrypted container holds (64 MB)");
      else
         written = MessageStore::WriteEncryptedContainer(*serialized, fileName, publicKey, problem);

      if (!written)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::High, code, "MessageData::Write",
            "The message " + fileName + " is in a mailbox encrypted at rest and could not be written encrypted: " + problem +
            ". It was not written, in the clear or otherwise" + (code == MailboxKeys::CodeReEncrypt ? String(_T("; the stored message is as it was.")) : String(_T("."))));
         return false;
      }

      encrypted_file_ = fileName;
      encrypted_to_ = publicKey;

      if (message_)
         message_->SetSize(MessageStore::FileSize(fileName));

      return true;
   }

   bool
   MessageData::CheckWritable_(const String &fileName)
   {
      // A MessageData whose load failed holds an empty MimeBody, and writing that over
      // a message file replaces the message with a couple of bytes of nothing. It is
      // reachable: LoadFromMessage answers false for a message above the 80 MB parser
      // cap and for one that threw while being parsed, and most callers ignore that
      // answer and write anyway - RuleApplier (a set-header rule on a large message),
      // VirusScanner's attachment stripping, SMTPForwarding, EmailAllUsers.
      // SpamTestSpamAssassin already guards its own call site for precisely this
      // reason; the guard belongs here, where it covers all of them.
      if (load_failed_)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::High, 6010, "MessageData::Write",
            "Refusing to write the message file " + fileName + " because the message could not be loaded into memory. Writing it would have replaced the message with an empty one.");
         return false;
      }

      // A message whose first bytes would be the mark of a message store
      // container (Persistence/MessageStoreFormat.h) is not written, here or
      // anywhere. No RFC 5322 header field begins with that byte, so only bytes
      // shaped to be taken for a container begin with it - a client can APPEND
      // such a message, which the store keeps wrapped, and its first line
      // survives a parse and a rewrite unchanged - and a stored file written so
      // would be read as a container the store never wrote, naming parts other
      // messages share. The file on disk is left as it was. The header is the
      // start of whatever this writes; with no field at all it is the blank line.
      {
         const AnsiString start = static_cast<const MimeHeader &>(*mime_mail_).Store();
         if (MessageStoreFormat::HasMagic(start.c_str(), (size_t) start.GetLength()))
         {
            ErrorManager::Instance()->ReportError(ErrorManager::High, 7353, "MessageData::Write",
               "Refusing to write the message file " + fileName + ": the message begins with the bytes that mark a container of the message store, and written so it would be read as one. The message on disk was left as it was.");
            return false;
         }
      }

      const HM::String directoryName = HM::FileUtilities::GetFilePath(fileName);
      if (!HM::FileUtilities::Exists(directoryName) && !HM::FileUtilities::CreateDirectory(directoryName))
      {
         // Without the directory the write below cannot succeed, and going on to it
         // would only add a second, less specific error. CreateDirectory has already
         // reported the operating-system reason.
         ErrorManager::Instance()->ReportError(ErrorManager::High, 6011, "MessageData::Write",
            "Failed to write the message file " + fileName + " because its directory could not be created.");
         return false;
      }

      return true;
   }

   bool
   MessageData::WriteReported(const String &fileName, const String &description)
   //---------------------------------------------------------------------------
   // DESCRIPTION:
   // Write, reporting the failure rather than discarding it. See the header.
   //
   // LOG_APPLICATION and not ReportError: Write has already reported the cause at
   // High with the specific reason (6010 or 6011), and a second ERROR record for
   // the same event would say nothing new while doubling the chance of tripping a
   // fixture that asserts a clean error log. What is added here is the thing Write
   // cannot know - which piece of work has silently not been done.
   //---------------------------------------------------------------------------
   {
      if (Write(fileName))
         return true;

      LOG_APPLICATION(Formatter::Format("{0} could not be written to {1}. The message has been left as it was and delivery has continued, so that change is not present in it.",
                                        description, fileName));

      return false;
   }

   bool
   MessageData::GetHasBodyType(const String &sBodyType)
   {
      std::shared_ptr<MimeBody> pPart = FindPart(sBodyType);      

      return pPart ? true : false;
   }

   int
   MessageData::GetRuleLoopCount()
   {
      // Absent, the message has been through no generator of ours. Present, it is
      // read by RuleGuard::ReadLoopCount, the header's one reader, whatever it holds:
      // a sender writes it as easily as we do, and _ttoi took "-5" as a count that
      // passed every limit - and on Linux, where it is (int) wcstol, "4294967295" as
      // -1. An empty value is no count either, and reads as the limit, where this
      // read it as 0.
      const char *value = mime_mail_->GetRawFieldValue(XHMAILSERVER_LOOPCOUNT);
      if (value == nullptr)
         return 0;

      return RuleGuard::ReadLoopCount(String(value));
   }

   void
   MessageData::IncreaseRuleLoopCount()
   {
      int iCurrentNo = GetRuleLoopCount() + 1;
      
      SetRuleLoopCount(iCurrentNo);
   }

   void 
   MessageData::SetRuleLoopCount(int iLoopCount)
   {
      mime_mail_->SetRawFieldValue(XHMAILSERVER_LOOPCOUNT, StringParser::IntToString(iLoopCount), "");
   }


   void 
   MessageData::GenerateMessageID()
   {
      String sGUID = GUIDCreator::GetGUID();
      sGUID.Replace(_T("{"), _T(""));
      sGUID.Replace(_T("}"), _T(""));
      
      String sMsgID;
      sMsgID.Format(_T("<%s@%s>"), sGUID, Utilities::ComputerName().c_str());

      SetFieldValue("Message-ID", sMsgID);
   }

   bool 
   MessageData::IsTextType(const String &sContentType)
   {
      return sContentType.CompareNoCase(_T("text/plain")) == 0;
   }

   bool 
   MessageData::IsHTMLType(const String &sContentType)
   {
      return sContentType.CompareNoCase(_T("text/html")) == 0;
   }

   std::shared_ptr<MimeBody> 
   MessageData::GetMimeMessage()
   {
      return mime_mail_;
   }

   void
   MessageData::SetAutoReplied()
   {
      SetFieldValue(AUTOSUBMITTED_HEADER, _T("auto-replied"));
   }

   void
   MessageData::SetAutoGenerated()
   {
      SetFieldValue(AUTOSUBMITTED_HEADER, _T("auto-generated"));
   }

   bool
   MessageData::IsAutoSubmitted()
   {
      String autoSubmitted = GetFieldValue(AUTOSUBMITTED_HEADER);
      if (autoSubmitted.IsEmpty())
         return false;

      if (autoSubmitted.CompareNoCase(_T("no")) == 0)
         return false;

      return true;
   }

   void
   MessageDataTester::Test()
   {
      return;
      // Following tests are only made in DEBUG.
      #ifndef _DEBUG
         return;
      #endif

      std::shared_ptr<Message> pMessage = std::shared_ptr<Message>(new Message);

      // Add recipient
      bool recipientOK = false;
      RecipientParser recipientParser;
      recipientParser.CreateMessageRecipientList("test@example.test", pMessage->GetRecipients(), recipientOK);

      std::shared_ptr<Account> account;

      // Create message data structure
      std::shared_ptr<MessageData> pMsgData = std::shared_ptr<MessageData>(new MessageData());
      pMsgData->LoadFromMessage(account, pMessage);
      pMsgData->SetTo("test@example.test");
      pMsgData->SetFrom("test@example.test");
      pMsgData->SetSubject("Hejsan");
      pMsgData->SetFieldValue("MIME-Version", "1.0");
      
      // Date was not specified. Specify it now.
      String sDate = Time::GetCurrentMimeDate();
      pMsgData->SetSentTime(sDate);


      // Add an attachment.
      pMsgData->GetAttachments()->Add("C:\\windows\\notepad.exe");
      pMsgData->GetAttachments()->Add("C:\\windows\\system32\\calc.exe");

      // Set body contents
      pMsgData->SetHTMLBody("Min <b>HTML</b> body");
      pMsgData->SetHTMLBody("Min <b>HTML</b> body");
      pMsgData->SetBody("Min plaintext body");

      String fileName = MessageStore::WorkingFile(account, pMessage);

      // Write it
      pMsgData->WriteReported(fileName, "The self-test message");
      

      // Save it
      PersistentMessage::SaveObject(pMessage);

      Application::Instance()->SubmitPendingEmail();

      Sleep(50000);
   }

   namespace
   {
      void ExpectLarge_(bool condition, const char *what)
      {
         if (!condition)
            throw std::logic_error(std::string("LargeMessageTester: ") + what);
      }

      // The body of the test message: a megabyte of numbered lines, repeated
      // to past MaxParseBytes, then a last line no other line is - so that a
      // copy cut short, or one with a block out of place, compares unequal.
      const char *const LargeBodyEnd = "The last line of the large message.\r\n";

      AnsiString LargeBodyBlock_()
      {
         AnsiString block;
         for (int line = 0; block.GetLength() < 1024 * 1024 - 100; line++)
            block += Formatter::FormatAsAnsi("Line {0} of a block of a message too large to be read whole.\r\n", line);
         return block;
      }

      int LargeBodyBlocks_()
      {
         return MessageData::MaxParseBytes / (1024 * 1024) + 1;
      }

      bool WriteLargeMessage_(const String &fileName, const AnsiString &header)
      {
         File file;
         if (!file.Open(fileName, File::OTCreate) || !file.Write(header))
            return false;

         const AnsiString block = LargeBodyBlock_();
         for (int i = 0; i < LargeBodyBlocks_(); i++)
         {
            if (!file.Write(block))
               return false;
         }

         return file.Write(AnsiString(LargeBodyEnd)) && file.Close();
      }

      // The header of the file - everything before its first blank line, that
      // line counted - and whether the rest is the test message's body, byte
      // for byte, read in blocks.
      bool ReadLargeMessage_(const String &fileName, AnsiString &header, bool &bodyIntact)
      {
         header = "";
         bodyIntact = false;

         File file;
         if (!file.Open(fileName, File::OTReadOnly))
            return false;

         std::shared_ptr<ByteBuffer> start = file.ReadChunk(64 * 1024);
         if (!start)
            return false;

         const AnsiString startText(start->GetCharBuffer(), start->GetSize());
         const int end = startText.Find("\r\n\r\n");
         if (end < 0)
            return false;

         header = startText.Mid(0, end + 4);
         if (!file.SetPosition(end + 4))
            return false;

         const AnsiString block = LargeBodyBlock_();
         for (int i = 0; i < LargeBodyBlocks_(); i++)
         {
            std::shared_ptr<ByteBuffer> read = file.ReadChunk(block.GetLength());
            if (!read || read->GetSize() != (size_t) block.GetLength() || std::memcmp(read->GetCharBuffer(), block.c_str(), read->GetSize()) != 0)
               return true;
         }

         const AnsiString last(LargeBodyEnd);
         std::shared_ptr<ByteBuffer> tail = file.ReadChunk(last.GetLength() + 1);
         bodyIntact = tail && tail->GetSize() == (size_t) last.GetLength() && std::memcmp(tail->GetCharBuffer(), last.c_str(), tail->GetSize()) == 0;
         return true;
      }

      const char *const LargeHeader_ =
         "Return-Path: <sender@example.test>\r\n"
         "X-hMailServer-LoopCount: 2\r\n"
         "From: sender@example.test\r\n"
         "To: recipient@example.test\r\n"
         "Subject: A message too large to be read whole\r\n"
         "MIME-Version: 1.0\r\n"
         "Content-Type: text/plain; charset=\"us-ascii\"\r\n"
         "\r\n";

      // The loop count as GetRuleLoopCount reads it and IncreaseRuleLoopCount
      // raises it, held to a limit of the tester's own: those two ask the
      // server's settings for the limit (RuleGuard::ReadLoopCount), and
      // file-utilities-tests on Linux runs with none - its PropertySet was never
      // loaded, and the read crashed the program there.
      const int TesterLoopLimit = 10;

      int LoopCountOf_(MessageData &data)
      {
         return RuleGuard::ReadLoopCount(data.GetFieldValue(XHMAILSERVER_LOOPCOUNT), TesterLoopLimit);
      }

      void RaiseLoopCount_(MessageData &data)
      {
         data.SetRuleLoopCount(LoopCountOf_(data) + 1);
      }

      void ExpectLarge_(bool condition, const std::string &what)
      {
         ExpectLarge_(condition, what.c_str());
      }

      // A field's value as the header holds it, empty when it holds none.
      AnsiString FieldOf_(const MimeHeader &header, const char *name)
      {
         const char *value = header.GetRawFieldValue(name);
         return value ? AnsiString(value) : AnsiString();
      }

      // The whole of a small file, byte for byte.
      bool ReadWholeFile_(const String &fileName, AnsiString &bytes)
      {
         bytes = "";

         File file;
         if (!file.Open(fileName, File::OTReadOnly))
            return false;

         std::shared_ptr<ByteBuffer> read = file.ReadFile();
         if (!read)
            return false;

         bytes = AnsiString(read->GetCharBuffer(), read->GetSize());
         return true;
      }

      // What a message read header only must be once a field was set on it and
      // it was written back: a header holding that field and the fields it was
      // read with, and after the blank line that ends it the body it had, byte
      // for byte.
      void ExpectWrittenBack_(const AnsiString &written, const AnsiString &header, const AnsiString &body, const std::string &what)
      {
         const int end = written.Find("\r\n\r\n");
         ExpectLarge_(end > 0, what + ": what was written has no end of its header");

         const AnsiString writtenHeader = written.Mid(0, end + 4);
         ExpectLarge_(writtenHeader.Find("X-Large-Message-Test: set\r\n") >= 0, what + ": the field set is not in the header written");
         ExpectLarge_(writtenHeader.Find(header.Mid(0, header.GetLength() - 2)) == 0, what + ": the header written does not begin with the fields it was read with");
         ExpectLarge_(written.Mid(end + 4) == body, what + ": the body written is not the body read, byte for byte");
      }

      // A header whose last field's line ends at lineEnd - the CRLF that ends it
      // begins there, and the blank line follows - with fields before it that
      // fill the bytes up to it; lastValue is that last field's value.
      AnsiString HeaderWithLastLineEndingAt_(size_t lineEnd, AnsiString &lastValue)
      {
         AnsiString header = "Subject: A header read in steps\r\n";
         const AnsiString filler = "X-Filler: 0123456789012345678901234567890123456789012345678901234567890123456789\r\n";
         while ((size_t) header.GetLength() + (size_t) filler.GetLength() + 100 < lineEnd)
            header += filler;

         const AnsiString name = "X-Last: ";
         lastValue = AnsiString(std::string(lineEnd - (size_t) header.GetLength() - (size_t) name.GetLength(), 'x').c_str());
         header += name + lastValue + "\r\n\r\n";
         return header;
      }

      // A header that ends past the first step MimeBody::LoadHeaderFromFile reads
      // a file in, and one whose blank line falls across the edge of a step at
      // each place it can: read whole, with the field after the edge, and written
      // back - to another file - with the body after it byte for byte. The
      // messages are small: what is read in steps is the header, whatever the
      // size of the message, and only MessageData asks for it by size.
      void HeaderReadInSteps_(const String &directory)
      {
         const String fileName = FileUtilities::Combine(directory, GUIDCreator::GetGUID() + _T(".eml"));
         const String copyName = FileUtilities::Combine(directory, GUIDCreator::GetGUID() + _T(".eml"));

         // The CRLF that ends the last field half a step past the first edge, and
         // four, three, two and one bytes before it and on it: the four bytes of
         // the line end and the blank line all before the edge, three, two or one
         // of them before it, and all after it.
         const size_t step = MimeBody::HeaderOnlyReadStep;
         const size_t lineEnds[] = { step + step / 2, step - 4, step - 3, step - 2, step - 1, step };
         const AnsiString body = "The body after a header read in steps.\r\nIts last line.\r\n";

         try
         {
            for (size_t lineEnd : lineEnds)
            {
               const std::string what = std::string("a header whose last line ends at byte ") + std::to_string(lineEnd);

               AnsiString lastValue;
               const AnsiString header = HeaderWithLastLineEndingAt_(lineEnd, lastValue);
               ExpectLarge_((size_t) header.GetLength() == lineEnd + 4, what + ": the test's header is not the length it was made to be");
               ExpectLarge_(FileUtilities::WriteToFile(fileName, header + body), what + ": the test message could not be written");

               std::shared_ptr<MimeBody> read = std::shared_ptr<MimeBody>(new MimeBody);
               ExpectLarge_(read->LoadHeaderFromFile(fileName) == MimeLoadResult::Loaded && read->IsHeaderOnly(), what + ": the header was not read");
               ExpectLarge_(FieldOf_(*read, "Subject") == "A header read in steps", what + ": the first field was not read");
               ExpectLarge_(FieldOf_(*read, "X-Last") == lastValue, what + ": the last field was not read whole");

               read->SetRawFieldValue("X-Large-Message-Test", "set", "");
               ExpectLarge_(read->SaveAllToFile(copyName), what + ": the header was not written back");

               AnsiString written;
               ExpectLarge_(ReadWholeFile_(copyName, written), what + ": what was written could not be read");
               ExpectWrittenBack_(written, header, body, what);
            }
         }
         catch (...)
         {
            FileUtilities::DeleteFile(fileName);
            FileUtilities::DeleteFile(copyName);
            throw;
         }

         FileUtilities::DeleteFile(fileName);
         FileUtilities::DeleteFile(copyName);
      }

      const char *const HeldHeader_ =
         "Subject: A message held in memory\r\n"
         "X-hMailServer-LoopCount: 1\r\n"
         "\r\n";

      const char *const HeldBody_ = "The body of a message held in memory.\r\nIts last line.\r\n";

      // A message held in memory, as an object store hands one over
      // (MessageStoreParseSource), read header only.
      std::shared_ptr<MimeBody> HeldHeaderOnly_()
      {
         std::shared_ptr<const std::string> held = std::shared_ptr<const std::string>(new std::string(std::string(HeldHeader_) + HeldBody_));

         std::shared_ptr<MimeBody> read = std::shared_ptr<MimeBody>(new MimeBody);
         ExpectLarge_(read->LoadHeaderFromMemory(held) == MimeLoadResult::Loaded && read->IsHeaderOnly(), "the header of a message held in memory was not read");
         return read;
      }

      // That message's header read and a field set on it, and the message
      // written into memory and to a file, each with the body copied from the
      // bytes it was read from.
      void HeaderHeldInMemory_(const String &directory)
      {
         const String fileName = FileUtilities::Combine(directory, GUIDCreator::GetGUID() + _T(".eml"));

         try
         {
            std::shared_ptr<MimeBody> read = HeldHeaderOnly_();
            ExpectLarge_(FieldOf_(*read, "Subject") == "A message held in memory", "the subject of a message held in memory was not read");

            read->SetRawFieldValue("X-Large-Message-Test", "set", "");

            std::shared_ptr<const std::string> written;
            ExpectLarge_(read->SaveAllToMemory(_T("a message held in memory"), written) && written, "a message held in memory, read header only, was not written into memory");
            ExpectWrittenBack_(AnsiString(written->data(), written->size()), HeldHeader_, HeldBody_, "a message held in memory, written into memory");

            ExpectLarge_(read->SaveAllToFile(fileName), "a message held in memory, read header only, was not written to a file");

            AnsiString inFile;
            ExpectLarge_(ReadWholeFile_(fileName, inFile), "the message written to a file could not be read");
            ExpectWrittenBack_(inFile, HeldHeader_, HeldBody_, "a message held in memory, written to a file");
         }
         catch (...)
         {
            FileUtilities::DeleteFile(fileName);
            throw;
         }

         FileUtilities::DeleteFile(fileName);
      }
   }

   void
   LargeMessageTester::Test(const String &directory)
   {
      const String fileName = FileUtilities::Combine(directory, GUIDCreator::GetGUID() + _T(".eml"));
      const String copyName = FileUtilities::Combine(directory, GUIDCreator::GetGUID() + _T(".eml"));

      try
      {
         ExpectLarge_(WriteLargeMessage_(fileName, LargeHeader_), "the test message could not be written");

         std::shared_ptr<Message> message = std::shared_ptr<Message>(new Message(false));

         // Refused, as every caller that does not ask has it.
         MessageData refused;
         ExpectLarge_(!refused.LoadFromMessage(fileName, message), "a message over MaxParseBytes loaded although the caller did not ask for its header alone");
         ExpectLarge_(!refused.IsHeaderOnly(), "a refused load says it read the header");

         // The header alone: read as any message's, the body empty.
         std::shared_ptr<MessageData> data = std::shared_ptr<MessageData>(new MessageData());
         ExpectLarge_(data->LoadFromMessage(fileName, message, MessageData::LargeMessage::HeaderOnly), "a message over MaxParseBytes was not loaded header only when asked");
         ExpectLarge_(data->IsHeaderOnly(), "a header-only load does not say so");
         ExpectLarge_(data->GetSubject() == _T("A message too large to be read whole"), "the subject of a header-only message was not read");
         ExpectLarge_(LoopCountOf_(*data) == 2, "the loop count of a header-only message was not read");
         ExpectLarge_(data->GetBody().IsEmpty() && data->GetAttachments()->GetCount() == 0, "a header-only message has a body or parts");

         // The loop count raised and written: the header with it, and the body
         // copied behind it byte for byte.
         RaiseLoopCount_(*data);
         data->SetFieldValue(_T("X-Large-Message-Test"), _T("set"));
         ExpectLarge_(data->Write(fileName), "a header-only message whose header changed was not written");

         AnsiString header;
         bool bodyIntact = false;
         ExpectLarge_(ReadLargeMessage_(fileName, header, bodyIntact), "the rewritten message could not be read back");
         ExpectLarge_(header.Find("X-hMailServer-LoopCount: 3\r\n") >= 0, "the raised loop count is not in the rewritten header");
         ExpectLarge_(header.Find("X-Large-Message-Test: set\r\n") >= 0, "the header set is not in the rewritten header");
         ExpectLarge_(header.Find("Subject: A message too large to be read whole\r\n") >= 0, "the rewritten header lost a field it had");
         ExpectLarge_(bodyIntact, "the body of the rewritten message is not the body it had, byte for byte");
         ExpectLarge_(message->GetSize() == FileUtilities::FileSize(fileName), "the message's size is not its rewritten file's");

         // Loaded again, as the last load chose: the header only, the count as written.
         ExpectLarge_(data->RefreshFromMessage() && data->IsHeaderOnly() && LoopCountOf_(*data) == 3, "a refreshed header-only message was not loaded header only again");

         // Written again to another name, from the file it was rewritten to.
         RaiseLoopCount_(*data);
         ExpectLarge_(data->Write(copyName), "a header-only message was not written to another name");
         ExpectLarge_(ReadLargeMessage_(copyName, header, bodyIntact) && bodyIntact && header.Find("X-hMailServer-LoopCount: 4\r\n") >= 0, "the copy written to another name is not the header and the whole body");
         ExpectLarge_(ReadLargeMessage_(fileName, header, bodyIntact) && bodyIntact && header.Find("X-hMailServer-LoopCount: 3\r\n") >= 0, "writing the copy changed the message it was read from");
      }
      catch (...)
      {
         FileUtilities::DeleteFile(fileName);
         FileUtilities::DeleteFile(copyName);
         throw;
      }

      FileUtilities::DeleteFile(fileName);
      FileUtilities::DeleteFile(copyName);

      // The two reads the message above does not take: a header longer than
      // the first step it is read in, or ending across a step's edge, and a
      // message an object store holds in memory.
      HeaderReadInSteps_(directory);
      HeaderHeldInMemory_(directory);
   }

   void
   LargeMessageTester::TestRefusals(const String &directory)
   {
      const String fileName = FileUtilities::Combine(directory, GUIDCreator::GetGUID() + _T(".eml"));
      const String longHeaderName = FileUtilities::Combine(directory, GUIDCreator::GetGUID() + _T(".eml"));

      try
      {
         ExpectLarge_(WriteLargeMessage_(fileName, LargeHeader_), "the test message could not be written");
         const long size = FileUtilities::FileSize(fileName);

         std::shared_ptr<Message> message = std::shared_ptr<Message>(new Message(false));

         // A changed body: refused (HM7840), the message as it was.
         std::shared_ptr<MessageData> data = std::shared_ptr<MessageData>(new MessageData());
         ExpectLarge_(data->LoadFromMessage(fileName, message, MessageData::LargeMessage::HeaderOnly), "a message over MaxParseBytes was not loaded header only when asked");
         data->SetBody(_T("A body that would replace the one never read."));
         ExpectLarge_(!data->Write(fileName), "a header-only message whose body changed was written");

         AnsiString header;
         bool bodyIntact = false;
         ExpectLarge_(ReadLargeMessage_(fileName, header, bodyIntact) && bodyIntact && FileUtilities::FileSize(fileName) == size &&
                      header == AnsiString(LargeHeader_), "a refused write of a changed body changed the message");

         // A source cut short after its header was read: refused (HM7842), the
         // file left as it now is.
         data = std::shared_ptr<MessageData>(new MessageData());
         ExpectLarge_(data->LoadFromMessage(fileName, message, MessageData::LargeMessage::HeaderOnly), "a message over MaxParseBytes was not loaded header only when asked");
         const AnsiString shortened = AnsiString(LargeHeader_) + "A body cut short.\r\n";
         ExpectLarge_(FileUtilities::WriteToFile(fileName, shortened), "the message could not be cut short");
         RaiseLoopCount_(*data);
         ExpectLarge_(!data->Write(fileName), "a header-only message whose source was cut short was written");
         ExpectLarge_(FileUtilities::ReadCompleteTextFile(fileName) == String(shortened), "a refused write over a source cut short changed the file");

         // A header with no end in the first MaxHeaderOnlyBytes: not loaded (HM7841).
         AnsiString longHeader = "Subject: A header longer than is read\r\n";
         const AnsiString filler = "X-Filler: 0123456789012345678901234567890123456789012345678901234567890123456789\r\n";
         while ((size_t) longHeader.GetLength() <= MimeBody::MaxHeaderOnlyBytes)
            longHeader += filler;
         longHeader += "\r\n";
         ExpectLarge_(WriteLargeMessage_(longHeaderName, longHeader), "the message with a long header could not be written");
         MessageData longData;
         ExpectLarge_(!longData.LoadFromMessage(longHeaderName, message, MessageData::LargeMessage::HeaderOnly), "a message whose header has no end in the bytes read was loaded");
         ExpectLarge_(!longData.Write(longHeaderName), "a message whose header could not be read was written");

         // A changed body of a message held in memory, read header only: refused
         // when written into memory (HM7840), and nothing handed back.
         std::shared_ptr<MimeBody> held = HeldHeaderOnly_();
         held->SetRawText("A body that would replace the one never read.");
         std::shared_ptr<const std::string> written;
         ExpectLarge_(!held->SaveAllToMemory(_T("a message held in memory"), written) && !written, "a message held in memory, read header only, was written with its body changed");
      }
      catch (...)
      {
         FileUtilities::DeleteFile(fileName);
         FileUtilities::DeleteFile(longHeaderName);
         throw;
      }

      FileUtilities::DeleteFile(fileName);
      FileUtilities::DeleteFile(longHeaderName);
   }



}
