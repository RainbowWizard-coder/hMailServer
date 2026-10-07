// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"

#include "COMError.h"
#include "../Common/Util/AuditTrail.h"
#include "InterfaceMessage.h"
#include "InterfaceAttachments.h"
#include "InterfaceRecipients.h"
#include "InterfaceMessageHeaders.h"

#include "../IMAP/MessagesContainer.h"

#include "../Common/Tracking/ChangeNotification.h"
#include "../Common/Tracking/NotificationServer.h"

#include "../Common/Util/Utilities.h"
#include "../Common/Util/Time.h"
#include "../Common/Util/MessageUtilities.h"
#include "../SMTP/RecipientParser.h"
#include "../SMTP/SMTPDeliveryManager.h"

#include "../Common/Cache/CacheContainer.h"
#include "../Common/BO/Account.h"
#include "../Common/BO/Message.h"
#include "../Common/BO/Messages.h"
#include "../Common/BO/MessageRecipients.h"
#include "../Common/Persistence/PersistentMessage.h"
#include "../Common/Persistence/MessageStore.h"
#include "../Common/Persistence/PersistentAccount.h"

#include "../Common/Mime/Mime.h"
#include "../Common/Util/Imip.h"
#include "../Common/Util/Unicode.h"
#include "../Common/Application/Logger.h"

#include "atlsafe.h"

namespace
{
   // What Save and Copy answer a holder of no credential outside an event script
   // (COMAuthentication::CallerIsEventScript). Until 6.3.6 a message a client had
   // created for itself, which carries none, was saved for anybody: with an
   // administrator password set, any local user queued mail from any sender to
   // any recipient through it. Since then such a message answers with the
   // credential of an Application its process holds signed in
   // (COMAuthentication::SignedInFromCallingProcess), and is refused only where
   // the process holds none.
   const wchar_t *const SavedWithoutCredential =
      L"You do not have access to this property / method. A message that carries no credential - one created "
      L"directly as hMailServer.Message by a program that holds no hMailServer.Application on which Authenticate() "
      L"succeeded - cannot be saved or copied: sign in on an Application first and keep it while the message is "
      L"saved, or create the message with Application.CreateMessage().";

   // What Save answers mail to be queued that claims to be from an address
   // the credential may not send as (COMAuthentication::MaySendAs).
   const wchar_t *const NotYoursToSendAs =
      L"You do not have access to this property / method. This credential may not send mail as ";

   // What the caller wrote, fit for one line of the log and of the error: a
   // control character - a line break that would start a log line of its
   // own - shown as '?', and no more than 200 characters of it.
   HM::String Printable(const HM::String &value)
   {
      HM::String printable = value.Mid(0, 200);

      for (int i = 0; i < printable.GetLength(); i++)
      {
         if (printable.GetAt(i) < 0x20 || printable.GetAt(i) == 0x7F)
            printable.SetAt(i, '?');
      }

      return printable;
   }

   // What Save answers mail to be queued whose header is not written as one
   // header (WrittenAsOneHeader), for a credential held to the addresses it
   // may send as.
   const wchar_t *const NotOneHeader =
      L"You do not have access to this property / method. A message this credential queues has a header written as "
      L"one: each field on a line of its own under a name of printable characters, and no line break in a value.";

   // The one mailbox a From or Sender field's value names, lower-cased, read
   // as the server reads a From where a sender's word decides something
   // (Imip::FromAddressOf: one mailbox, that the address list parser and the
   // DMARC check read alike, written as one mailbox is written); empty for
   // any other value - a list, a group, an address in a comment or in the
   // display name, a second <address>. Of the value as the header holds it,
   // and of the same value decoded (UTF-8), which is what a mail program
   // shows.
   HM::AnsiString OneMailboxAsWritten(const HM::AnsiString &value)
   {
      return HM::Imip::FromAddressOf(HM::AnsiString("From: ") + value + HM::AnsiString("\r\n\r\n"));
   }

   HM::AnsiString OneMailboxAsDecoded(const HM::String &value)
   {
      HM::AnsiString utf8;
      HM::Unicode::WideToMultiByte(value, utf8);
      return OneMailboxAsWritten(utf8);
   }

   // Whether the header as Save writes it - MimeHeader::Store, which both of
   // MimeBody::SaveAllToFile's paths write - is one header that every program
   // reads alike, so that the fields ClaimsOnlyWhatItMaySendAs reads are the
   // ones the recipient's programs read: every line ends in CRLF, with no
   // NUL, lone CR or lone LF anywhere, which some programs end a line at and
   // this server does not; a line is a field - a name of printable ASCII
   // other than the colon, then the colon - or the continuation of the field
   // above it, which starts with a space or a tab and holds more than white
   // space, which some programs take for the empty line; and the one empty
   // line is the last. COM took a value as it is given, line breaks and
   // all, and the writer wrote it so until 6 October 2026: a Subject that
   // holds "\r\nFrom: ..." added a From field that the parsed message this
   // checks does not have. The writer now keeps a value to one field, and
   // this is asked of the header as given (MimeHeader::StoreAsGiven), so that
   // such a message is still refused rather than sent with a space in the
   // line break's place - and Save asks besides that the header as given is
   // the header written, since a line break followed by a line that is a
   // field is one header as given and is joined to the value above it when
   // written (InterfaceMessage::Save says what that let through); and a field
   // named "From " is written "From :", which RFC 5322's obsolete syntax
   // reads as From.
   bool WrittenAsOneHeader(const HM::AnsiString &header, HM::String &reason)
   {
      const char *text = header.c_str();
      const int length = header.GetLength();
      bool inField = false;
      int lineStart = 0;

      for (int i = 0; i < length; i++)
      {
         const char c = text[i];

         if (c == '\0')
         {
            reason = "the header holds a NUL.";
            return false;
         }

         if (c == '\n')
         {
            reason = "a value in the header holds a line feed with no carriage return before it.";
            return false;
         }

         if (c != '\r')
            continue;

         if (i + 1 >= length || text[i + 1] != '\n')
         {
            reason = "a value in the header holds a carriage return with no line feed after it.";
            return false;
         }

         const int lineEnd = i;
         i++;

         if (lineEnd == lineStart)
         {
            // The empty line that ends the header, which Store writes last.
            if (i + 1 != length)
            {
               reason = "a value in the header holds an empty line, where the header would end.";
               return false;
            }

            return true;
         }

         const char first = text[lineStart];
         if (first == ' ' || first == '\t')
         {
            if (!inField)
            {
               reason = "the header's first field name starts with white space.";
               return false;
            }

            bool onlyWhiteSpace = true;
            for (int j = lineStart; j < lineEnd && onlyWhiteSpace; j++)
               onlyWhiteSpace = text[j] == ' ' || text[j] == '\t';

            if (onlyWhiteSpace)
            {
               reason = "a value in the header holds a line of nothing but white space.";
               return false;
            }
         }
         else
         {
            int colon = -1;
            for (int j = lineStart; j < lineEnd; j++)
            {
               const unsigned char n = static_cast<unsigned char>(text[j]);
               if (n == ':')
               {
                  colon = j;
                  break;
               }

               if (n < 33 || n > 126)
               {
                  reason = "a field name in the header holds a space, a control character or a character outside ASCII.";
                  return false;
               }
            }

            if (colon <= lineStart)
            {
               reason = "a line of the header is neither a field nor the continuation of one.";
               return false;
            }

            inField = true;
         }

         lineStart = i + 1;
      }

      reason = "the header does not end with an empty line.";
      return false;
   }

   // Whether mail the credential queues may claim to be from what this
   // message names: its envelope sender, and the mailbox in its From field
   // and in its Sender field as the header is written (WrittenAsOneHeader),
   // each one the credential may send as. The envelope is what authenticated
   // SMTP holds to the same rule; the fields are what the recipient reads,
   // which COM - unlike SMTP, where the client writes the fields after the
   // server has answered MAIL FROM, and the webmail, which writes them from
   // the one address it checked - lets the caller write apart from it. A
   // field written more than once, or that names anything but one mailbox -
   // as written, or once decoded, so that an address in an encoded word of
   // the display name counts - is refused: another program could read
   // another address in it. A message with no From field claims nobody's.
   bool ClaimsOnlyWhatItMaySendAs(const HM::COMAuthentication &credential, const HM::String &envelopeSender,
                                  const HM::AnsiString &header, HM::String &claimed, HM::String &reason)
   {
      claimed = envelopeSender;
      if (!credential.MaySendAs(envelopeSender, reason))
         return false;

      HM::MimeHeader written;
      written.Load(header.c_str(), header.GetLength(), true);

      const char *const names[] = { "From", "Sender" };

      for (const char *name : names)
      {
         int occurrences = 0;
         HM::AnsiString value;

         for (HM::MimeField &field : written.Fields())
         {
            if (field.GetName() && ::_stricmp(field.GetName(), name) == 0)
            {
               occurrences++;
               value = field.GetValue() ? field.GetValue() : "";
            }
         }

         if (occurrences == 0)
            continue;

         const HM::String decoded = HM::MimeHeader::GetUnicodeFieldValue(name, value);
         claimed = decoded;

         if (occurrences > 1)
         {
            reason = HM::String(L"the message has ") + HM::String(name) + L" more than once.";
            return false;
         }

         const HM::AnsiString asWritten = OneMailboxAsWritten(value);
         if (asWritten.IsEmpty() || asWritten != OneMailboxAsDecoded(decoded))
         {
            reason = HM::String(L"its ") + HM::String(name) + L" field does not name one mailbox written as one.";
            return false;
         }

         HM::String mailbox;
         HM::Unicode::MultiByteToWide(asWritten, mailbox);

         claimed = mailbox;
         if (!credential.MaySendAs(mailbox, reason))
            return false;
      }

      return true;
   }
}

STDMETHODIMP 
InterfaceMessage::InterfaceSupportsErrorInfo(REFIID riid)
{
   static const IID* arr[] = 
   {
      &IID_IInterfaceMessage,
   };

   for (int i=0;i<sizeof(arr)/sizeof(arr[0]);i++)
   {
      if (InlineIsEqualGUID(*arr[i],riid))
         return S_OK;
   }
   return S_FALSE;
}
   
InterfaceMessage::InterfaceMessage()
{
   object_ = std::shared_ptr<HM::Message>(new HM::Message());

   // Created directly (hMailServer.Message), this message holds no credential
   // of its own: it answers with the one its client process signed in with,
   // while it holds one. A message handed out by another object is given
   // that object's credential instead (SetAuthentication), which never does.
   authentication_->AnswerForCallingProcess();
}

STDMETHODIMP InterfaceMessage::get_ID(hyper *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetID();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceMessage::get_UID(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = (long) object_->GetUID();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceMessage::get_InternalDate(VARIANT *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      HM::DateTime dt = HM::Time::GetDateFromSystemDate(object_->GetCreateTime());
   
      *pVal  = dt.GetVariant();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceMessage::get_Filename(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      std::shared_ptr<const HM::Account> account;
      
      if (object_->GetAccountID() > 0)
      {
         account = HM::CacheContainer::Instance()->GetAccount(object_->GetAccountID());
      }
   
      *pVal = HM::PersistentMessage::GetFileName(account, object_).AllocSysString();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceMessage::get_Subject(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = GetMessageData_()->GetSubject().AllocSysString();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceMessage::put_Subject(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      GetMessageData_()->SetSubject(newVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceMessage::get_Charset(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = GetMessageData_()->GetCharset().AllocSysString();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceMessage::put_Charset(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      GetMessageData_()->SetCharset(newVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceMessage::get_From(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = GetMessageData_()->GetFrom().AllocSysString();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceMessage::put_From(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      GetMessageData_()->SetFrom(newVal);  
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceMessage::get_Date(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = GetMessageData_()->GetSentTime().AllocSysString();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceMessage::put_Date(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      GetMessageData_()->SetSentTime(newVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceMessage::get_Body(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = GetMessageData_()->GetBody().AllocSysString();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceMessage::put_Body(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      GetMessageData_()->SetBody(newVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceMessage::get_HTMLBody(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = GetMessageData_()->GetHTMLBody().AllocSysString();
      
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceMessage::put_HTMLBody(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      GetMessageData_()->SetHTMLBody(newVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceMessage::get_Attachments(IInterfaceAttachments **pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      CComObject<InterfaceAttachments>* pItem = new CComObject<InterfaceAttachments>();
      pItem->SetAuthentication(authentication_);
   
      std::shared_ptr<HM::Attachments> pAttachments = GetMessageData_()->GetAttachments();
   
      if (pAttachments)
      {
         pItem->Attach(pAttachments);
         pItem->AddRef();
         *pVal = pItem;
      }
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceMessage::get_Headers(IInterfaceMessageHeaders **pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      std::shared_ptr<HM::MimeBody> pMimeBody = GetMessageData_()->GetMimeMessage();
   
      if (!pMimeBody)
         return COMError::WithoutMessage(DISP_E_BADINDEX);
   
      CComObject<InterfaceMessageHeaders>* pItem = new CComObject<InterfaceMessageHeaders>();
   
      pItem->AttachItem(pMimeBody);
      pItem->AddRef();
      *pVal = pItem;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP 
InterfaceMessage::SaveNewMessageToIMAPFolder_()
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!object_)
         return GetAccessDenied();

      // Check which account this message belongs to.
      std::shared_ptr<const HM::Account> pAccount = HM::CacheContainer::Instance()->GetAccount(object_->GetAccountID());
   
      // Save the message to the database
      if (!HM::PersistentMessage::SaveObject(object_))
         return S_FALSE;
   
      // Add to parent collection
      HM::MessagesContainer::Instance()->SetFolderNeedsRefresh(object_->GetFolderID());
   
      // Notify...
      std::shared_ptr<HM::ChangeNotification> pNotification = 
         std::shared_ptr<HM::ChangeNotification>(new HM::ChangeNotification(object_->GetAccountID(), object_->GetFolderID(), HM::ChangeNotification::NotificationMessageAdded));
   
      HM::Application::Instance()->GetNotificationServer()->SendNotification(pNotification);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceMessage::Save()
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsAuthenticated() && !HM::COMAuthentication::CallerIsEventScript())
         return COMError::GenerateError(SavedWithoutCredential);

      // The account this belongs to must be one this credential may change
      // (COMAuthentication::MayChangeAccount).
      if (!authentication_->MayChangeAccount(object_->GetAccountID()))
         return COMError::GenerateError(HM::COMAuthentication::NotTakenIn);

      // Mail to be queued for delivery (case 1 below), and mail in the queue
      // written again (case 3: the file its delivery reads, which a program
      // that saved a message keeps the object of, and may change and save
      // again until the message has been delivered), claims to be from no
      // address the credential may not send as, in a header written as one -
      // asked before anything is written. Until 6.3.6 nothing here asked who
      // queued mail claims to be from, whoever queued it. The server
      // administrator and the event scripts send as anybody, as they always
      // have.
      const HM::Message::State queuedState = object_->GetState();
      if ((queuedState == HM::Message::Created || queuedState == HM::Message::Delivering) &&
          object_->GetFolderID() == 0 && object_->GetAccountID() == 0 && !authentication_->SendsAsAnyAddress())
      {
         HM::String claimed;
         HM::String reason;

         // The header with each value as it was set (StoreAsGiven), and the
         // header as Write will write it - MimeHeader::Store, which both of
         // MimeBody::SaveAllToFile's paths call. Write keeps a value to one field,
         // writing each CR and each LF of a line break that does not fold it as a
         // space - a CR LF as two - and a value
         // given with one is refused here, and the program told, rather than sent
         // changed. The two must be the same, not only the one as given one
         // header: a line break followed by a line that is a field of its own is
         // one header as given, and Write joins that line to the value above it.
         // A From of "me@ok.example", a line break and "X-A: Boss
         // <ceo@bank.example>" was checked as me@ok.example and written as one
         // From, whose first address - to AddresslistParser, which DKIMSigner and
         // the rest of this server read a From with - is ceo@bank.example: a
         // credential held to its own addresses wrote another's, and had it
         // signed as that address's domain where that domain is this server's.
         // With no message to store, none, which is no header written as one.
         HM::AnsiString header;
         HM::AnsiString written;
         const std::shared_ptr<HM::MimeBody> mime = GetMessageData_()->GetMimeMessage();
         if (mime)
         {
            mime->StoreAsGiven(header);
            mime->HM::MimeHeader::Store(written);
         }

         bool oneHeader = WrittenAsOneHeader(header, reason);
         if (oneHeader && header != written)
         {
            oneHeader = false;
            reason = "a value in the header holds a line break that does not fold it, whose CR and LF would each be written as a space.";
         }

         if (!oneHeader)
         {
            using namespace HM;
            LOG_APPLICATION("COM: a message was not queued, because its header is not written as one: " + Printable(reason));
            return COMError::GenerateError(NotOneHeader);
         }

         if (!ClaimsOnlyWhatItMaySendAs(*authentication_, object_->GetFromAddress(), header, claimed, reason))
         {
            // The reason to the log, not to the caller, as the webmail's send
            // route does: it can say whether an account exists.
            using namespace HM;
            LOG_APPLICATION("COM: a message was not queued, because it claims to be from " + Printable(claimed) + ": " + Printable(reason));
            return COMError::GenerateError(HM::String(NotYoursToSendAs) + Printable(claimed) + L".");
         }
      }

      // Check that the message has a valid date header.
      HM::String sDate = GetMessageData_()->GetSentTime();
      if (sDate.IsEmpty())
      {
         // Date was not specified. Specify it now.
         sDate = HM::Time::GetCurrentMimeDate();
         GetMessageData_()->SetSentTime(sDate);
      }
   
      std::shared_ptr<const HM::Account> account;
   
      if (object_->GetAccountID() > 0)
      {
         account = HM::CacheContainer::Instance()->GetAccount(object_->GetAccountID());
      }
   
      // Save the message to disk.
      const HM::String fileName = HM::PersistentMessage::GetFileName(account, object_);

      // A new message for a folder of an account's mailbox (case 2 below) is
      // written as the mailbox stores mail: encrypted in memory to the
      // account's key when the mailbox is encrypted at rest, and never in the
      // clear there (MessageData::WriteToMailbox, docs/EncryptionAtRest.md).
      // Anything else is written as it always was; a rewrite of a stored
      // message that is encrypted is encrypted again by Write itself.
      const bool intoMailbox = object_->GetState() == HM::Message::Created && object_->GetFolderID() != 0 && account;
      const bool written = intoMailbox ? GetMessageData_()->WriteToMailbox(fileName, account) : GetMessageData_()->Write(fileName);

      if (!written)
      {
         return COMError::GenerateError("Unable to write to message file.");
      }
   
      // A message can be in a number of different states:
      // Case 1) New message which should be delivered -> Save in file and add to database
      // Case 2) New message which should be added to an existing IMAP folder -> Update message file and save to database// 
      // case 3) Existing message which is being delivered. -> Only update the message fil
      // Case 4) Existing message in IMAP folder which should just be re-saved -> Update message file
   
      HM::Message::State state = object_->GetState();
   
      switch (state)
      {
      case HM::Message::Created:
         {
            // Handle new message. It can either be Case 1 or Case 2. If the message is already
            // connected to an account, it means that it should be stored in a specific IMAP folder
            if (object_->GetFolderID() == 0 && object_->GetAccountID() == 0)
            {
               // Case 1: The message should be delivered. Change the state to delivering
               object_->SetState(HM::Message::Delivering);
   
               if (!HM::PersistentMessage::SaveObject(object_))
               {
                  return COMError::GenerateError("Message could not be saved in database.");
               }
   
               HM::Application::Instance()->SubmitPendingEmail();
            }  
            else
            {
               // Case 2. It's a new message but it should be added to an existing IMAP folder.
               object_->SetState(HM::Message::Delivered);
               return SaveNewMessageToIMAPFolder_();
            }
   
            break;
         }
      case HM::Message::Delivering:
         {
            // Handle message which is being delivered. Saving in database will be taken
            // care about by the delivery process. Since the file has already been updated
            // on disk, there's not more for us to do here.
            break;
         }
      case HM::Message::Delivered:
         {
            // The message has already been delivered. It's placed inside an account mailbox.
            // All we need to do is to update it in the database.
            if (!HM::PersistentMessage::SaveObject(object_))
               return S_FALSE;
   
            break;
         }
      default:
         {
            // Unhandled case.
            return COMError::GenerateError("The message could not be saevd. It is in an unknown state.");
         }
      }
      
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceMessage::RefreshContent()
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      // No need to actually refresh anything now, this
      // will be taken care of by the GetMessageData_()
      // when the client tries to access anything after
      // calling this function.
   
      msg_data_.reset();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceMessage::Copy(long lDestinationFolderID)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsAuthenticated() && !HM::COMAuthentication::CallerIsEventScript())
         return COMError::GenerateError(SavedWithoutCredential);

      if (!authentication_->MayChangeAccount(object_->GetAccountID()))
         return COMError::GenerateError(HM::COMAuthentication::NotTakenIn);

      // The source message must be located in a users account.
      if (object_->GetAccountID() == 0)
         return S_FALSE;
   
      bool bSuccess = HM::MessageUtilities::CopyToIMAPFolder(object_, lDestinationFolderID);
      if (!bSuccess)
      {
         return COMError::GenerateError("Unable to copy message.");
      }
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceMessage::get_To(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = GetMessageData_()->GetTo().AllocSysString();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceMessage::get_CC(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = GetMessageData_()->GetCC().AllocSysString();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceMessage::ClearRecipients()
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->GetRecipients()->Clear();
   
      GetMessageData_()->SetTo("");
      GetMessageData_()->SetCC("");
      GetMessageData_()->SetBCC("");
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceMessage::AddRecipient(BSTR bstrName, BSTR bstrAddress)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      // Add this recipent to the actual email.
      HM::String sAddress(bstrAddress);
      HM::String sName (bstrName);
   
      bool recipientOK = false;
      HM::RecipientParser recipientParser;
      recipientParser.CreateMessageRecipientList(sAddress, object_->GetRecipients(), recipientOK);
   
      // Add this recipient to the mime message.
      HM::String sThisAddress = "\"" + sName + "\"" + " <" + sAddress + ">";
      HM::String sTo = GetMessageData_()->GetTo();
   
      if (!sTo.IsEmpty())
         sTo += ",";
   
      sTo += sThisAddress;
   
      GetMessageData_()->SetTo(sTo);
      
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceMessage::get_FromAddress(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetFromAddress().AllocSysString();
   return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceMessage::put_FromAddress(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetFromAddress(newVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceMessage::get_State(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetState();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceMessage::get_DeliveryAttempt(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetNoOfRetries() + 1;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceMessage::get_Size(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetSize() / 1024;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceMessage::get_Recipients(IInterfaceRecipients**pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      CComObject<InterfaceRecipients>* pItem = new CComObject<InterfaceRecipients>();
      pItem->SetAuthentication(authentication_);
   
      std::shared_ptr<HM::Message> pMessage = GetMessageData_()->GetMessage();
   
      if (pMessage)
      {
         pItem->Attach(pMessage);
         pItem->AddRef();
         *pVal = pItem;
      }
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

std::shared_ptr<HM::MessageData> 
InterfaceMessage::GetMessageData_()
{
   if (!msg_data_)
   {
      std::shared_ptr<const HM::Account> account;

      if (object_->GetAccountID() > 0)
      {
         account = HM::CacheContainer::Instance()->GetAccount(object_->GetAccountID());
      }

      msg_data_ = std::shared_ptr<HM::MessageData>(new HM::MessageData());
      msg_data_->LoadFromMessage(account, object_);
   }

   return msg_data_;
}

STDMETHODIMP InterfaceMessage::get_HeaderValue(BSTR FieldName, BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = GetMessageData_()->GetFieldValue(FieldName).AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceMessage::put_HeaderValue(BSTR FieldName, BSTR FieldValue)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      GetMessageData_()->SetFieldValue(FieldName, FieldValue);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceMessage::HasBodyType(BSTR BodyType, VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = GetMessageData_()->GetHasBodyType(BodyType) ? VARIANT_TRUE : VARIANT_FALSE;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceMessage::get_EncodeFields(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = GetMessageData_()->GetEncodeFields() ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceMessage::put_EncodeFields(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      GetMessageData_()->SetEncodeFields(newVal == VARIANT_TRUE);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP 
InterfaceMessage::get_Flag(eMessageFlag iFlag, VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      int iFlags = object_->GetFlags();
      *pVal = (iFlags & iFlag) > 0 ? VARIANT_TRUE : VARIANT_FALSE;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP 
InterfaceMessage::put_Flag(eMessageFlag iFlag, VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      int iFlags = object_->GetFlags();
      
      if (newVal == VARIANT_TRUE)
         iFlags = iFlags | iFlag;
      else
         iFlags = iFlags & ~iFlag;
   
      object_->SetFlags(iFlags);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

