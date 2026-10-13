// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

/*
   Copyright (c) 2003, Jeff Lee
   All rights reserved.

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are met:

   Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
   AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
   IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
   ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
   LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
   CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
   SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
   INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
   CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
   ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
   POSSIBILITY OF SUCH DAMAGE.
*/
   
#pragma once

#include <string>
#include <list>
#include <memory>
using namespace std;

// RFC 1521 - Mechanisms for Specifying and Describing the Format of Internet Message Bodies
// RFC 2045 - Format of Internet Message Bodies
// RFC 2046 - Media Types
// RFC 2047 - Message Header Extensions for Non-ASCII Text
// RFC 2049 - Conformance Criteria and Examples
// RFC 822 - Standard For The Format of ARPA Internet Text Message
namespace HM
{
   const char* FindString(const char* pszStr1, const char* pszStr2, const char* pszEnd);

   class CMimeConst
   {
   public:
      // field names
      static inline const char* MimeVersion() { return "MIME-Version"; }
      static inline const char* ContentType() { return "Content-Type"; }
      static inline const char* TransferEncoding() { return "Content-Transfer-Encoding"; }
      static inline const char* ContentID() { return "Content-ID"; }
      static inline const char* ContentDescription() { return "Content-Description"; }
      static inline const char* ContentDisposition() { return "Content-Disposition"; }

      // parameter names
      static inline const char* Charset() { return "charset"; }
      static inline const char* Name() { return "name"; }
      static inline const char* Filename() { return "filename"; }
      static inline const char* Boundary() { return "boundary"; }
      static inline const char* Inline() { return "inline"; }
      static inline const char* Attachment() { return "attachment"; }

      // parameter values
      static inline const char* Encoding7Bit() { return "7bit"; }
      static inline const char* Encoding8Bit() { return "8bit"; }
      static inline const char* EncodingBinary() { return "binary"; }
      static inline const char* EncodingQP() { return "quoted-printable"; }
      static inline const char* EncodingBase64() { return "base64"; }

      static inline const char* MediaText() { return "text"; }
      static inline const char* MediaImage() { return "image"; }
      static inline const char* MediaAudio() { return "audio"; }
      static inline const char* MediaVedio() { return "vedio"; }
      static inline const char* MediaApplication() { return "application"; }
      static inline const char* MediaMultiPart() { return "multipart"; }
      static inline const char* MediaMessage() { return "message"; }
   };

   //////////////////////////////////////////////////////////////////////
   // MimeField class - Represents a field of a MIME body part header
   //////////////////////////////////////////////////////////////////////
   class MimeField
   {
   public:
      MimeField() {}
      ~MimeField() {}

   public:
      void SetName(const char* pszName);
      const char* GetName() const;
      void SetValue(const char* pszValue);
      const char* GetValue() const;
      void GetValue(string& strValue) const;

      void SetParameter(const char* pszAttr, const char* pszValue);
      bool GetParameter(const char* pszAttr, AnsiString& strValue) const;
      void RemoveParameter(const char* pszAttr);
      /*
      encodedParameter indicates whether the content is encoding according to 

      MIME Parameter Value and Encoded Word Extensions:
      Character Sets, Languages, and Continuations

      http://tools.ietf.org/html/draft-freed-pvcsc-03

      */
      void SetCharset(const char* pszCharset);
      const char* GetCharset() const;

      bool IsModified() const { return modified_; }
      bool IsNew() const { return is_new_; }

      void Clear();
      int GetLength() const;

      // A field set or changed since it was read is written by name, ": ", its value
      // through the field coder and CR LF; one read and left alone, as it was read.
      // The value is kept to one field (AppendAsOneField) unless keepToOneField is
      // false, which MimeHeader::StoreAsGiven alone asks for.
      void Store(AnsiString &output, bool keepToOneField = true) const;
      size_t Load(const char* pszData, size_t nDataSize, bool unfold);

      // Appends a field value written as one field: a CR LF followed by a space or a
      // tab and then by more than white space is a fold and stays, and every other
      // CR or LF is written as a space - the rule RuleGuard::HeaderValueAsOneField
      // applies to a rule's and a Sieve script's values before they are coded.
      //
      // THE DEFECT. The writer wrote a line break in a value as it stood, so a value
      // holding a CR LF and "X-hMailServer-LoopCount: 0" wrote a second field under
      // that name, and what is read first is the count. Every message the server
      // generates is built from values that are not its own: a Sieve notification's
      // :message and :from, a vacation reply's :subject or the sender's decoded
      // subject, a rule's reply subject and sender, a copy rule's name in X-CopyRule,
      // an account's out-of-office subject, a bounce's decoded subject. Written above
      // the count the generator set, the reset count was the one read - and with it
      // an "Auto-Submitted: no" above the generator's own, so that a script notifying
      // its own account notified about its own notification for ever. Held here, the
      // one writer of a field set in memory, rather than at each of them.
      static void AppendAsOneField(AnsiString &output, const char *value, size_t length);
      
      static void UnfoldField(string& strField);

   private:
      string name_;           // field name
      string value_;       // encoded value
      string charset_;        // charset for non-ascii text

      string decoded_value_;

   private:
      string raw_line_;    // Original header line from file (name: value with folding + \r\n)
      bool modified_ = false;
      bool is_new_ = true;  // True if created programmatically, not loaded from file

      bool FindParameter(const char* pszAttr, int& nPos, int& nSize, bool &encodedParameter) const;
      /*
         encodedParameter indicates whether the content is encoding according to 

         MIME Parameter Value and Encoded Word Extensions:
            Character Sets, Languages, and Continuations
      
         http://tools.ietf.org/html/draft-freed-pvcsc-03

      */
   };

   inline void MimeField::SetName(const char* pszName)
   { name_ = pszName; }

   inline const char* MimeField::GetName() const
   { return name_.data(); }

   inline void MimeField::SetValue(const char* pszValue)
   { modified_ = true; value_ = pszValue; }

   inline const char* MimeField::GetValue() const
   { return value_.data(); }

   inline void MimeField::SetCharset(const char* pszCharset)
   { modified_ = true; charset_ = pszCharset; }

   inline const char* MimeField::GetCharset() const
   { return charset_.c_str(); }

   inline void MimeField::Clear()
   { name_.clear(); value_.clear(); charset_.clear(); raw_line_.clear(); modified_ = false; is_new_ = true; }

   class MIMEUnicodeEncoder
   {
   public:

      static AnsiString EncodeValue(const AnsiString &sCharSet, const String &sUnicodeValue);

   };


   class MimeHeader
   {
   public:
      MimeHeader() {}
      virtual ~MimeHeader() { MimeHeader::Clear(); }

   public:
      enum MediaType
      {
         MEDIA_TEXT, MEDIA_IMAGE, MEDIA_AUDIO, MEDIA_VIDEO, MEDIA_APPLICATION,
         MEDIA_MULTIPART, MEDIA_MESSAGE,
         MEDIA_UNKNOWN
      };
      MediaType GetMediaType() const;

      // set/get the values of header fields
      void SetField(const MimeField& field);
      MimeField* GetField(const char* pszFieldName) const;
      MimeField* GetField(unsigned int iIndex);
      
      int GetFieldCount() const;

      void SetUnicodeFieldValue(const AnsiString &sFieldName, const String &sFieldValue, const AnsiString & sCharset);
      void SetRawFieldValue(const AnsiString &pszFieldName, const AnsiString &pszFieldValue, const AnsiString & pszCharset);
      
      const char* GetRawFieldValue(const String &pszFieldName) const;
      
      String GetUnicodeFieldValue(const AnsiString &pszFieldName) const;
      

      bool SetParameter(const AnsiString &pszFieldName, const AnsiString & pszAttr, const AnsiString & pszValue);
      string GetParameter(const char* pszFieldName, const char* pszAttr) const;
      void SetFieldCharset(const char* pszFieldName, const char* pszCharset);
      const char* GetFieldCharset(const char* pszFieldName) const;
      AnsiString GetHeaderContents();
      String GetUnicodeHeaderContents();

      // helper functions for standard body part fields
      void SetContentType(const AnsiString &pszValue, const AnsiString &pszCharset);
      const char* GetContentType() const;       // Content-Type: mediatype/subtype
      string GetMainType() const;
      string GetSubType() const;
      void SetCharset(const char* pszCharset);  // Content-Type: text/...; charset=...
      string GetCharset() const;
      void SetName(const char* pszName);        // Content-Type: image/...; name=...
      string GetName() const;
      void SetBoundary(const char* pszBoundary=NULL); // Content-Type: multipart/...; boundary=...
      string GetBoundary() const;

      void SetTransferEncoding(const char* pszValue);
      const char* GetTransferEncoding() const;  // Content-Transfer-Encoding: ...
      void SetDisposition(const char* pszValue, const char* pszCharset=NULL);
      const char* GetDisposition() const;       // Content-Disposition: ...
      virtual String GetUnicodeFilename() const;               // Content-Disposition: ...; filename=...
      virtual String GetRawFilename() const;             // Content-Disposition: ...; filename=...
      void SetFileName(const String &file_name) ;
      void SetDescription(const char* pszValue, const char* pszCharset=NULL);
      const char* GetDescription() const;       // Content-Description: ...

      typedef std::vector<MimeField> CFieldList;
      CFieldList& Fields() { return fields_; }
   
      bool FieldExists(const char *pszFieldName) const;

      void DeleteField(MimeField *pField);
      void DeleteField(const AnsiString &fieldName);

      // Every field of this name, in one pass; the number deleted.
      size_t DeleteFields(const char *pszFieldName);

      // The fields at these positions (ascending, each once), in one pass, the
      // others kept in their order.
      static void RemoveFieldsAt(std::vector<MimeField> &fields, const std::vector<size_t> &ascending);

   public:
      // overrides
      virtual void Clear();
      virtual int GetLength() const;
      // serialization
      virtual void Store(AnsiString &output) const;
      virtual size_t Load(const char* pszData, size_t nDataSize, bool unfold = true);

      AnsiString Store() const;

      // The header as Store writes it, but with each value as it was set rather than
      // kept to one field (MimeField::AppendAsOneField): for COM's Message.Save,
      // which refuses a credential's message whose values hold a line break, and says
      // so, rather than send it with a space in its place. Never written anywhere.
      void StoreAsGiven(AnsiString &output) const;

   protected:
      std::vector<MimeField> fields_;  // list of all header fields
      std::vector<MimeField>::iterator FindField(const char* pszFieldName) const;
      bool headers_modified_ = false;

      struct MediaTypeCvt
      {
         int nMediaType;            // media type
         const char* pszSubType;    // subtype
         const char* pszFileExt;    // file extension name
      };
      static const MediaTypeCvt type_cvt_table_[];
      static const char* type_table_[];

   public:
      // Decodes one header value that is already in hand, rather than one held by a
      // MimeHeader instance. Made public on 13 August 2026 for the Sieve vacation
      // responder, which has the raw Subject of the message it is replying to and needs
      // the decoded form to build "Auto: <subject>".
      //
      // Public is the right access for it: it is a static pure function of its two
      // arguments, holds no state and touches none, so exposing it grants no access to a
      // header's internals. The alternative - having the caller construct a MimeHeader
      // around a value it already has, purely to reach the instance overload - would be
      // more code doing more work for the same result.
      static String GetUnicodeFieldValue(const AnsiString &pszFieldName, const AnsiString &sRawFieldValue);

   private:

      MimeHeader& operator=(const MimeHeader&);    // forbid operator =
   };

   // add a new field or update an existing field
   inline void MimeHeader::SetField(const MimeField& field)
   {
      headers_modified_ = true;
      auto it = FindField(field.GetName());
      if (it != fields_.end())
         *it = field;
      else
         fields_.push_back(field);
   }

   // find a field by name
   inline MimeField* MimeHeader::GetField(const char* pszFieldName) const
   {
      auto it = FindField(pszFieldName);
   
      if ((std::vector<MimeField>::const_iterator) it != fields_.end())
         return &(*it);
      return NULL;
   }

   inline int MimeHeader::GetFieldCount() const
   {
      return (int) fields_.size();
   }

   inline MimeField* MimeHeader::GetField(unsigned int iIndex)
   {
      // `iIndex <= fields_.size() - 1` is the same test for every non-empty header
      // and a catastrophically different one for an empty header: size() is size_t,
      // so 0 - 1 is SIZE_MAX and every index passes, after which &fields_[iIndex]
      // indexes an empty vector. InterfaceMessageHeaders bounds-checks before
      // calling and says so in a comment, which is a guard in one caller rather
      // than a function that cannot be misused; a header with no fields is
      // ordinary - Clear() produces one, and so does a part whose header block
      // failed to parse.
      if (iIndex < fields_.size())
         return &fields_[iIndex];

      return NULL;
   }


   // add a new field or update an existing field
   inline void MimeHeader::SetRawFieldValue(const AnsiString & pszFieldName, const AnsiString & pszFieldValue, const AnsiString & pszCharset)
   {
      MimeField fd;
      fd.SetName(pszFieldName);
      fd.SetValue(pszFieldValue);
      if (!pszCharset.IsEmpty())
         fd.SetCharset(pszCharset);

      SetField(fd);
   }

   inline const char* MimeHeader::GetRawFieldValue(const String &pszFieldName) const
   {
      AnsiString sHeaderName = pszFieldName;
      MimeField* pfd = GetField(sHeaderName);
      return pfd != NULL ? pfd->GetValue() : NULL;
   }


   

   inline void MimeHeader::SetFieldCharset(const char* pszFieldName, const char* pszCharset)
   {
      MimeField *pfd = GetField(pszFieldName);
      if (pfd)
         pfd->SetCharset(pszCharset);
      else
      {
         MimeField fd;
         fd.SetName(pszFieldName);
         fd.SetCharset(pszCharset);
         SetField(fd);
      }
   }

   inline const char* MimeHeader::GetFieldCharset(const char* pszFieldName) const
   {
      MimeField* pfd = GetField(pszFieldName);
      return pfd != NULL ? pfd->GetCharset() : NULL;
   }

   inline bool MimeHeader::SetParameter(const AnsiString & pszFieldName, const AnsiString & pszAttr, const AnsiString & pszValue)
   {
      MimeField *pfd = GetField(pszFieldName);
      if (pfd)
      {
         pfd->SetParameter(pszAttr, pszValue);
         return true;
      }
      return false;
   }

   inline string MimeHeader::GetParameter(const char* pszFieldName, const char* pszAttr) const
   {
      AnsiString strVal;
      MimeField *pfd = GetField(pszFieldName);
      if (pfd)
         pfd->GetParameter(pszAttr, strVal);

      return strVal;
   }
   
   

   inline void MimeHeader::SetContentType(const AnsiString &pszValue, const AnsiString &pszCharset)
   { SetRawFieldValue(CMimeConst::ContentType(), pszValue, pszCharset); }

   inline const char* MimeHeader::GetContentType() const
   { return GetRawFieldValue(CMimeConst::ContentType()); }

   inline string MimeHeader::GetCharset() const
   { return GetParameter(CMimeConst::ContentType(), CMimeConst::Charset()); }

   inline string MimeHeader::GetName() const
   { return GetParameter(CMimeConst::ContentType(), CMimeConst::Name()); }

   inline string MimeHeader::GetBoundary() const
   { return GetParameter(CMimeConst::ContentType(), CMimeConst::Boundary()); }

   inline void MimeHeader::SetTransferEncoding(const char* pszValue)
   { SetRawFieldValue(CMimeConst::TransferEncoding(), pszValue, ""); }

   inline const char* MimeHeader::GetTransferEncoding() const
   { return GetRawFieldValue(CMimeConst::TransferEncoding()); }

   // Content-Disposition header field specifies how to present this body part. it could be
   // inline or attachment. 'inline' indicates this body part should be displayed in the main body;
   // 'attachment' indicates it is separate from the main body. (RFC 2183)

   inline void MimeHeader::SetDisposition(const char* pszValue, const char* pszCharset)
   { SetRawFieldValue(CMimeConst::ContentDisposition(), pszValue, pszCharset); }

   inline const char* MimeHeader::GetDisposition() const
   { return GetRawFieldValue(CMimeConst::ContentDisposition()); }

   inline void MimeHeader::SetDescription(const char* pszValue, const char* pszCharset)
   { SetRawFieldValue(CMimeConst::ContentDescription(), pszValue, pszCharset); }

   inline const char* MimeHeader::GetDescription() const
   { return GetRawFieldValue(CMimeConst::ContentDescription()); }

   // What MimeBody::LoadFromFile actually did, as opposed to what a bool could say
   // about it.
   //
   // It used to return int: false when the file could not be opened, and true for
   // everything else - including a parse that threw, which its own catch(...)
   // swallowed after reporting 4228 and copying the file aside. So its one checking
   // caller, MessageData::LoadFromMessage, read "I could not open the file" as "there
   // is no file yet, this is a message being composed", and read "the parse threw
   // half way through" as success. Both then went on to use, and in the second case
   // to WRITE BACK, a MimeBody holding nothing or holding half a message.
   //
   // These are three different answers and the caller has to be able to tell them
   // apart, so they are three values. Deliberately NOT a fourth value for "the file
   // does not exist": whether a missing file means "compose a new message" or "the
   // message has been lost" is the caller's question, not the parser's, and the
   // parser has no way to answer it.
   enum class MimeLoadResult
   {
      Loaded,       // the file was opened and parsed
      OpenFailed,   // the file could not be opened - missing, locked, or denied
      ParseFailed,  // the file was read, and parsing it threw
   };

   //////////////////////////////////////////////////////////////////////
   // MimeBody class - Represents a body part in a MIME message
   //////////////////////////////////////////////////////////////////////
   class CMimeMessage;
   class MimeBody : public MimeHeader
   {
   public:
      MimeBody() :            // instantiate a MimeBody object explicitly is not allowed. call CreatePart()
         part_index_(0), find_(bodies_.end()) {}
      virtual ~MimeBody() { MimeBody::Clear(); }

   public:
      int GetContentLength() const;
      int GetContentEncodedLength() const;
      const AnsiString& GetContent() const;

      virtual String GetUnicodeFilename() const;
      virtual String GetRawFilename() const;

      // operations for 'text' or 'message' media
      bool IsText() const;
      
      int SetRawText(const AnsiString &pbText);
      string GetRawText();

      int SetUnicodeText(const String &sText);
      String GetUnicodeText();

      // operations for 'message' media
      bool IsMessage() const;
      bool SetMessage(const MimeBody* pMM);
      void GetMessage(MimeBody* pMM) const;

      // operations for 'image/audio/vedio/application' (attachment) media
      bool IsAttachment() const;
      bool ReadFromFile(const String &pszFilename);

      // The part's content from bytes in memory, named fileName: what
      // ReadFromFile does with a file's bytes, with nothing written anywhere -
      // the webmail's attachments, which for a mailbox encrypted at rest must
      // not pass through the disk in the clear.
      bool ReadFromMemory(const AnsiString &bytes, const String &fileName);
      bool WriteToFile(const String  &sFilename);
      // The content with its transfer encoding removed, in memory.
      bool GetDecodedContent(AnsiString &decoded);

      // operations for 'multipart' media
      bool IsMultiPart() const;
      void DeleteAll();
      std::shared_ptr<MimeBody> CreatePart(const char* pszMediaType, std::shared_ptr<MimeBody> pWhere);
      void AddPart(std::shared_ptr<MimeBody> );
      int GetPartCount();

      void ErasePart(std::shared_ptr<MimeBody> pBP);
      std::shared_ptr<MimeBody> FindFirstPart();
      std::shared_ptr<MimeBody> FindNextPart();
      size_t GetNumberOfParts();

      typedef std::list<std::shared_ptr<MimeBody> > BodyList;
      int GetAttachmentList(std::shared_ptr<MimeBody> pThis, BodyList& rList) const;
      void ClearAttachments();
      void RemoveAttachment(std::shared_ptr<MimeBody> pAttachment);
      MimeLoadResult LoadFromFile(const AnsiString &pszFilename);
      bool SaveAllToFile(const AnsiString &pszFilename);

      // A message held in memory - one the message store decoded and never
      // wrote out, a mailbox encrypted at rest's above all (docs/
      // EncryptionAtRest.md) - parsed as LoadFromFile parses a file's bytes,
      // and kept as the source an unchanged body is copied from on a rewrite,
      // as LoadFromFile keeps its file. keepAsideOnFailure copies the bytes to
      // "Problematic messages" when the parse throws, as LoadFromFile copies
      // its file; false for bytes that must never reach the disk in the clear.
      // name is for the error log.
      MimeLoadResult LoadFromMemory(const std::shared_ptr<const std::string> &message, const String &name, bool keepAsideOnFailure);

      // The header of a message too large to be parsed whole (MessageData::
      // LoadFromMessage, LargeMessage::HeaderOnly): its fields parsed as the
      // whole message's would be, its body left where it is - in the file, or
      // in message for LoadHeaderFromMemory - and nothing of it read. Written
      // with its body unchanged, the header is followed by the body copied
      // byte for byte from there, never held in memory from a file; a change
      // to the body is refused when written (HM7840), since the body that was
      // never read cannot be written with it. ParseFailed when the first
      // MaxHeaderOnlyBytes hold no end of the header, or a field the parser
      // refuses; OpenFailed when the file could not be opened or read.
      MimeLoadResult LoadHeaderFromFile(const AnsiString &pszFilename);
      MimeLoadResult LoadHeaderFromMemory(const std::shared_ptr<const std::string> &message);
      bool IsHeaderOnly() const { return header_only_; }

      // The most of a file LoadHeaderFromFile reads for the end of the header,
      // 10 MB, and the steps it reads it in, 64 KB - each multiplied in size_t
      // from its first factor on, not in int and then widened (clang-tidy's
      // bugprone-implicit-widening-of-multiplication-result, which the
      // static-analysis job holds to its baseline).
      static const size_t MaxHeaderOnlyBytes = static_cast<size_t>(10) * 1024 * 1024;
      static const size_t HeaderOnlyReadStep = static_cast<size_t>(64) * 1024;

      // The whole message as SaveAllToFile would write it, into memory and
      // nowhere else: the header and, when nothing of the body changed, the
      // body copied from the source it was loaded from. False, reported, when
      // that source could not be read. name is for the error log.
      bool SaveAllToMemory(const String &name, std::shared_ptr<const std::string> &message);

      String GetCleanContentType() const;
      bool IsAnyChildModified() const;
      // Returns a clean content type like text/html or text/plain,
      // not other things that exists in the Content-Type header.

      bool IsEncapsulatedRFC822Message() const;
      std::shared_ptr<MimeBody> LoadEncapsulatedMessage() const;

      // Where this part is in the bytes the message was parsed from, as Load
      // found it there: its MIME header's first byte, its body's first byte,
      // and one past its last byte - which is where the CRLF in front of the
      // next boundary of the multipart holding it begins, or the end of the
      // bytes. The header runs from entityStart to bodyStart, blank line and
      // all; the body from bodyStart to entityEnd, a multipart's preamble,
      // boundaries, parts and epilogue all. A part of a message that
      // LoadEncapsulatedMessage parsed from a message/rfc822 part is placed in
      // the same bytes, inside that part's body, so that every part of a
      // message parsed from a stored file or held message is placed in those
      // bytes, however deep. The top-level body's entityEnd is how many bytes
      // were parsed.
      //
      // False where nothing true can be said: a part made rather than read
      // (CreatePart, SetRawText and the like), one whose header did not parse,
      // one of a message encapsulated in a part that was itself not placed, and
      // one whose own header or body was changed since it was read. A part
      // holding one that was changed still answers where it was read from,
      // which no longer holds the change: the parts below are not walked here,
      // where a FETCH asks at every section, and a caller cutting a part from
      // the stored bytes asks the message's IsAnyChildModified, once, as IMAP's
      // FETCH does for a message's sections. It serves a part's MIME header and
      // a multipart's body from these, so that they are the stored bytes rather
      // than the part written out again.
      bool GetStoredBounds(size_t &entityStart, size_t &bodyStart, size_t &entityEnd) const;

      // The bytes this body was parsed from, when LoadFromMemory parsed it and
      // holds them; null otherwise - a file's (LoadFromFile), or bytes handed to
      // Load, which keeps none.
      std::shared_ptr<const std::string> GetSourceBytes() const { return source_bytes_; }

   public:
      // overrides
      virtual void Clear();
      virtual int GetLength(bool bIncludeHeader=true) const;
      // serialization
      virtual void Store(AnsiString &output, bool bIncludeHeader=true) const;
   
      virtual size_t Load(const char* pszData, size_t nDataSize, size_t &index, bool &part_loaded);

      // The nesting depth at which Load stops splitting a multipart into child parts
      // and keeps the rest of it as opaque content instead.
      //
      // Load recurses once per level of multipart nesting, and nothing bounded that:
      // a message is untrusted input, each level costs one stack frame plus the
      // frames of MimeHeader::Load and MimeField::Load beneath it, and a level can be
      // spelled in about forty bytes - a Content-Type with a boundary, a blank line
      // and an opening delimiter. A few hundred kilobytes of such a message, which is
      // an ordinary size to accept over SMTP, is enough to exhaust a one-megabyte
      // thread stack. That is EXCEPTION_STACK_OVERFLOW inside the delivery thread,
      // which is a mail outage rather than a rejected message, and it is not
      // something the catch(...) blocks around this code can be relied on to survive:
      // the handler has to run on the stack that just ran out.
      //
      // 20 is far above anything real. mixed/alternative/related is three, and a
      // forwarded chain adds a level at a time; message/rfc822 does not recurse here
      // at all, because it has no boundary and is parsed on demand by
      // LoadEncapsulatedMessage.
      //
      // Reaching it is not an error and nothing is discarded: the part keeps the
      // whole of its body as content, exactly as a non-multipart part does, so the
      // bytes survive both re-serialisation paths. What is lost is only the SPLIT -
      // BODYSTRUCTURE shows fewer levels, and an attachment buried below the limit is
      // not enumerated. For a message that could not previously be parsed without
      // crashing the server, that is the better trade.
      static const size_t MaxNestingDepth = 20;

   protected:

      AnsiString text_;    // content (text) of the body part
      size_t part_index_;
      // Distance from the top-level body. Set by the parent immediately before it
      // calls Load on a child; the top-level body keeps 0.
      size_t parse_depth_ = 0;
      BodyList bodies_;       // list of all child body parts
      BodyList::iterator find_;

      AnsiString source_file_;          // Path of file this was loaded from
      std::shared_ptr<const std::string> source_bytes_;   // Or the bytes, when they were never a file (LoadFromMemory)
      size_t body_byte_offset_ = 0;    // Byte offset where body starts in source file (after headers + blank line)
      size_t body_byte_end_ = 0;       // Byte offset one past last meaningful body byte (end of closing boundary marker)
      bool body_modified_ = false;     // True if any body content was changed
      size_t last_header_size_ = 0;    // Header size captured from last Load() call
      size_t last_multipart_end_ = 0;  // Position right after "--boundary--" closing marker, or 0 if not multipart
      bool header_only_ = false;       // Only the header was read (LoadHeaderFromFile): the body is unread in the source

      // GetStoredBounds. stored_start_ is set by whoever calls Load - the
      // multipart holding the part, LoadEncapsulatedMessage - before it does,
      // as parse_depth_ is, and Load records the rest. stored_placed_ is false
      // for a message encapsulated in a part that was not placed, and for every
      // part of it.
      size_t stored_start_ = 0;
      size_t stored_body_start_ = 0;
      size_t stored_end_ = 0;
      bool stored_bounds_ = false;
      bool stored_placed_ = true;

   protected:
      bool AllocateBuffer(size_t nBufSize);
      void FreeBuffer();
      // Answers false when the body could NOT be read, which an empty return value used
      // to be indistinguishable from - see the comment on the definition. An empty body
      // with a true return is legitimate: a header-only message has no body.
      bool ReadBodyFromSourceFile(AnsiString &body) const;

      // The body between the offsets the parse recorded, out of the source's
      // bytes (length of them, no terminating NUL counted): false when they
      // are shorter than the header the parse read.
      bool BodyFromSource_(const char *bytes, size_t length, AnsiString &body) const;

      // What SaveAllToFile writes and SaveAllToMemory answers. preserved says
      // the body was copied from the source, whose header and body sizes are
      // then headerSize and bodySize. target is for the error log.
      bool SerializeAll_(const String &target, AnsiString &output, bool &preserved, size_t &headerSize, size_t &bodySize);

      // Where the body is in the bytes a parse has just read (loaded of them).
      void RecordBodyBounds_(const char *bytes, size_t loaded);

      // The header-only load of the first length bytes of a source of total
      // bytes: false, nothing kept, when they do not hold the whole header.
      bool LoadHeaderOnly_(const char *bytes, size_t length, size_t total);

      // True when a header-only message's body is as it was read, so that it
      // can be copied from its source; false, reported (HM7840), when it was
      // changed. target is for the error log.
      bool HeaderOnlyBodyUnchanged_(const String &target) const;

      // SaveAllToFile for a header-only message read from a file: the header,
      // then the unread body copied from the file in chunks, into a file beside
      // the target that is renamed over it.
      bool SaveHeaderOnlyToFile_(const AnsiString &pszFilename);

      String GenerateFileNameFromEncapsulatedSubject(bool unicode) const;
   };

   inline int MimeBody::GetContentLength() const
   { return (int) text_.size(); }

   inline const AnsiString& MimeBody::GetContent() const
   { return text_; }

   inline bool MimeBody::IsText() const
   { return GetMediaType() == MEDIA_TEXT; }

   inline bool MimeBody::IsMessage() const
   { return GetMediaType() == MEDIA_MESSAGE; }

   inline bool MimeBody::IsMultiPart() const
   { return GetMediaType() == MEDIA_MULTIPART; }

   inline size_t MimeBody::GetNumberOfParts()
   {
      return bodies_.size();
   }

   inline std::shared_ptr<MimeBody> MimeBody::FindFirstPart()
   {
      find_ = bodies_.begin();
      return FindNextPart();
   }

   inline std::shared_ptr<MimeBody> MimeBody::FindNextPart()
   {
      if (find_ != bodies_.end())
         return *find_++;
      
      std::shared_ptr<MimeBody> pEmpty;
      return pEmpty;
   }

   inline bool MimeBody::AllocateBuffer(size_t nBufSize)
   {

      text_.reserve(nBufSize);

      return true;
   }

   inline void MimeBody::FreeBuffer()
   {
      text_ = "";
   }

}
