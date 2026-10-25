// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#pragma once

namespace HM
{
   class MimeBody;

   class Attachment  
   {
   public:
      Attachment(std::shared_ptr<MimeBody> pMessage, std::shared_ptr<MimeBody> pAttachment);
      virtual ~Attachment();

      void SetFileName(const String &file_name);
      void SetContent(const String &content);
      String GetFileName();
      int GetSize();
      void Delete();
      void SaveAs(const String &sSaveTo) const;
      bool GetContent(AnsiString &decoded) const;
      AnsiString GetContentType() const;

      // The part itself, so that a caller can tell WHICH part of the message
      // this is - VirusScanner compares it with the one detached signature
      // SealedMessage finds - rather than what it says it is.
      std::shared_ptr<MimeBody> GetMimeBody() const { return attachment_; }


   private:
      
      std::shared_ptr<MimeBody> attachment_;
      std::shared_ptr<MimeBody> message_;
   };

}
