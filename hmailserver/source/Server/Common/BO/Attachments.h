// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#pragma once

#include <functional>

namespace HM
{
   class Attachment;
   class MimeBody;
   class MessageData;

   class Attachments 
   {
   public:
      Attachments(std::shared_ptr<MimeBody> pMimeBody, MessageData *pMsgData);
      virtual ~Attachments();

      bool Add(const String &sFilename);
      bool Add(const String &sFilename, const String &contentType);

      // An attachment made from bytes in memory, named fileName: nothing is
      // written to the disk (MimeBody::ReadFromMemory).
      bool AddFromMemory(const String &fileName, const AnsiString &bytes, const String &contentType);
      bool Add(std::shared_ptr<Attachment> pAttachment);
      
      std::shared_ptr<Attachment> GetItem(unsigned int index) const;

      size_t GetCount() const;
      void Clear();
      void Load();

      std::vector<std::shared_ptr<Attachment> > GetVector() { return vecObjects; }

   private:
      // The part made and its content read, and the declared type applied.
      bool AddPart_(const String &contentType, const std::function<bool(std::shared_ptr<MimeBody>)> &read);

      std::vector<std::shared_ptr<Attachment> > vecObjects;

      std::shared_ptr<MimeBody> mime_body_;
      MessageData *msg_data_;
   };
}
