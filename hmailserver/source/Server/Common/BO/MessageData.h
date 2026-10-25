// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#pragma once

namespace HM
{
   class Account;
   class Message;
   class MimeHeader;
   class Attachments;

   class MimeBody;
   class MessageStoreParseSource;

   class MessageData  
   {
   public:
      MessageData();
      virtual ~MessageData();

      // The size above which a message is not parsed whole: 80 MB.
      static const int MaxParseBytes = 1024 * 1024 * 80;

      // What LoadFromMessage does with a message larger than MaxParseBytes.
      //
      // Refuse answers false, as it always has, and every caller but a few
      // asks for it. HeaderOnly loads the header alone (MimeBody::
      // LoadHeaderFromFile) and answers true: the fields read and change as
      // any message's, the body reads as empty and has no parts, and Write
      // copies the body unread from where it is behind the header - refusing
      // (HM7840) when the body was changed. It is for what reads or changes
      // only the header (docs/MailFlowRules.md): the rule-loop count a forward,
      // a redirect, a rule's copy or a bounce reads and raises, the rules' header
      // criteria and header actions, and the signature and disclaimers, which
      // need the header to know whether they apply and say in the log that
      // they were not appended to such a message.
      enum class LargeMessage
      {
         Refuse,
         HeaderOnly
      };

      bool LoadFromMessage(const String &fileName, std::shared_ptr<Message> pMessage, LargeMessage large = LargeMessage::Refuse);
      bool LoadFromMessage(std::shared_ptr<const Account> account, std::shared_ptr<Message> pMessage, LargeMessage large = LargeMessage::Refuse);

      // Loaded again from the same file, as the last load chose to treat a
      // large message.
      bool RefreshFromMessage();

      // True when only the header was loaded (LargeMessage::HeaderOnly).
      bool IsHeaderOnly() const;

      String GetHeader() const;
      
      String GetSubject() const;
      void SetSubject(const String &sSubject);

      String GetCharset() const;
      void SetCharset(const String &sCharset);

      void DeleteField(const AnsiString &sHeaderName);

      String GetSentTime() const;
      void SetSentTime(const String &sSentTime);
      
      String GetTo() const;
      void SetTo(const String &sTo);
      
      String GetReturnPath() const;
      void SetReturnPath(const String &sReturnPath);

      String GetCC() const;
      void SetCC(const String &sCC);
      
      String GetBCC() const;
      void SetBCC(const String &sBCC);

      String GetFrom() const;
      void SetFrom(const String &sFrom);

      String GetHTMLBody() const;

      // The decoded text of every attachment whose media type is text - a .txt,
      // a .csv, an .html, a .ics - concatenated, for SEARCH BODY/TEXT and the
      // full-text index. Never a binary part: bytes that happen to spell a word
      // inside an executable are not text a person could read. Bounded, because
      // a message can carry a very large text attachment and the searcher and
      // the indexer both hold the result in memory.
      String GetAttachmentText() const;
      void SetHTMLBody(const String &sHTMLBody);
      
      String GetBody() const;
      void SetBody(const String &sBody);

      void SetFieldValue(const String &sField, const String &sValue);
      String GetFieldValue(const String &sName) const;

      // How many times a header field appears.
      //
      // GetFieldValue answers with the FIRST match, which is the right answer for a
      // field RFC 5322 allows only once - but it cannot distinguish a message carrying
      // one From from a message carrying three, and that distinction is a DMARC
      // question. See SpamTestDMARC::RunTest.
      int GetFieldOccurrenceCount(const String &sName) const;

      int GetRuleLoopCount();
      void SetRuleLoopCount(int iLoopCount);
      void IncreaseRuleLoopCount();
      
      std::shared_ptr<MimeBody> GetBodyTextPlainPart() const;
      std::shared_ptr<MimeBody> GetBodyTextHtmlPart() const;

      bool GetHasBodyType(const String &sBodyType);

      // The message written to fileName. A rewrite of the stored file this was
      // read from - or last wrote - when that file is encrypted at rest (docs/
      // EncryptionAtRest.md) is encrypted again to the same key, in memory,
      // and written beside it and renamed over it: its plaintext never reaches
      // the disk, and a rewrite that cannot be encrypted is not made (HM7362).
      bool Write(const String &fileName);

      // The message written to fileName encrypted to publicKey, from memory:
      // never in the clear on the disk. False, reported (HM7365), when it
      // could not be.
      bool WriteEncrypted(const String &fileName, const std::string &publicKey);

      // What a writer of a NEW message into a folder of account's mailbox
      // writes with - COM's Message.Save and the script binding's for a
      // message added to a folder, the webmail's draft: encrypted in memory to
      // the account's key when its mailbox is encrypted at rest, and Write
      // otherwise. False when nothing may be kept - including when whether the
      // mailbox is encrypted is not known (HM7365), which unknown says, so that
      // the caller answers it as the temporary refusal it is, and when the
      // mailbox's key row holds no key, which damaged says.
      bool WriteToMailbox(const String &fileName, std::shared_ptr<const Account> account);
      bool WriteToMailbox(const String &fileName, std::shared_ptr<const Account> account, bool &unknown, bool &damaged);

      // Write, and report it if it fails, naming what the caller was trying to write.
      //
      // Twelve call sites across rules, forwarding, vacation, virus notification, the
      // SpamAssassin pass and the attachment stripper discarded Write's result. That was
      // survivable while a failed Write still produced a file - it produced the WRONG
      // file, a message with no body, which is the defect Write now refuses to commit.
      // Refusing made those twelve quieter rather than correct: the header, the trace
      // line, the vacation body simply does not get written and delivery carries on as
      // though it had. None of them can do anything useful about the failure, so this
      // exists to make sure none of them can hide it either.
      bool WriteReported(const String &fileName, const String &description);

      int GetSize() const;

      std::shared_ptr<Attachments> GetAttachments();

      std::shared_ptr<Message> GetMessage() {return message_; }
   
      bool GetEncodeFields() {return encode_fields_; }
      void SetEncodeFields(bool bNewVal) {encode_fields_ = bNewVal; }

      void GenerateMessageID();

      std::shared_ptr<MimeBody> CreatePart(const String &sContentType);

      std::shared_ptr<MimeBody> GetMimeMessage();

     void SetAutoReplied();
     void SetAutoGenerated();
     bool IsAutoSubmitted();

   private:

      std::shared_ptr<MimeBody> GetViewBodyPart_(int recursion_level, std::shared_ptr<MimeBody> source, const String &requested_content_type) const;

      bool IsTextType(const String &sContentType);
      bool IsHTMLType(const String &sContentType);

      std::shared_ptr<MimeBody> FindPart(const String &sType) const;
      std::shared_ptr<MimeBody> FindPartNoRecurse(std::shared_ptr<MimeBody> parent, const AnsiString &sType) const;

      // CreatePart's view of an HTML body that shows images carried beside it
      // (multipart/related): kept whole across the rebuild.
      bool IsFormattedRelated_(std::shared_ptr<MimeBody> part) const;
      std::shared_ptr<MimeBody> FindFormattedBodyNoRecurse_(std::shared_ptr<MimeBody> parent) const;
      std::shared_ptr<MimeBody> LiftRelatedBody_();

      std::shared_ptr<Message> message_;
      std::shared_ptr<MimeBody> mime_mail_;
      std::shared_ptr<Attachments> attachments_;

      String message_file_name_;

      bool encode_fields_;
      bool unfold_with_space_;

      // Set when LoadFromMessage gave up (message over the parser's size cap, or an
      // exception while parsing). The MimeBody is empty in that case, so Write must
      // refuse rather than replace the message on disk with nothing. See Write.
      bool load_failed_;

      // What the last LoadFromMessage was asked to do with a large message, for
      // RefreshFromMessage.
      LargeMessage large_;

      // What was parsed (Persistence/MessageStore.h): the stored file itself, or
      // the original bytes of one the store keeps in another form, held in
      // memory for as long as this is - never a copy on the disk - because
      // MimeBody's rewrite copies the body from what it parsed.
      std::shared_ptr<MessageStoreParseSource> source_;

      // A file this wrote encrypted (WriteEncrypted), and the key: a later
      // Write to it is encrypted again, as a rewrite of an encrypted source is.
      String encrypted_file_;
      std::string encrypted_to_;

      // Write's refusals before anything is written: a failed load, a message
      // that would begin with the container's magic, a directory that cannot
      // be made.
      bool CheckWritable_(const String &fileName);

      // The message serialized in memory, encrypted to publicKey and written
      // beside fileName, then renamed over it. code is what a failure is
      // reported as: 7362 for a rewrite, 7365 for a new message.
      bool WriteEncrypted_(const String &fileName, const std::string &publicKey, int code);
   };

   class MessageDataTester
   {
   public:
      MessageDataTester() {};
      void Test();
   };

   // A message larger than MessageData::MaxParseBytes, read header only, in
   // files written to directory and removed: ClassTester runs Test on
   // Windows, file-utilities-tests on Linux runs both.
   class LargeMessageTester
   {
   public:
      // Refuse answers false; HeaderOnly reads the header, raises the loop count
      // and writes it back with the body byte for byte, to the same name and to
      // another. And MimeBody's two header-only reads on small messages: from a
      // file, a header that ends past the first step it is read in, and one
      // whose blank line falls across a step's edge at each place it can; and
      // from memory, as an object store hands a message over, written into
      // memory and to a file. Reports nothing.
      void Test(const String &directory);

      // What is refused, each reported: a changed body (HM7840, and HM6011 from
      // Write), with the message left as it was; a body source cut short
      // (HM7842 and HM6011); a header with no end in the bytes read (HM7841),
      // and a write of what that load left (HM6010); and a changed body of a
      // message held in memory, written into memory (HM7840 again). The caller
      // counts them.
      void TestRefusals(const String &directory);
   };

}
