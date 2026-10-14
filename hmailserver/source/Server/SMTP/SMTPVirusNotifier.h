// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once


namespace HM
{
   class Message;
   
   class SMTPVirusNotifier  
   {
   public:
      SMTPVirusNotifier();
      virtual ~SMTPVirusNotifier();

      // The notice that the message was deleted, to sRecipient, its macros
      // filled from the header of the message's file.
      static void CreateMessageDeletedNotification(const std::shared_ptr<Message> pOriginalMessage, const String &sRecipient);

      // The same notice, from a header read already: the file is not read, and
      // nothing is reported about it. A header with nothing in it - the file
      // could not be read - names the envelope's sender and the recipient in
      // place of the header's From and To.
      static void CreateMessageDeletedNotification(const std::shared_ptr<Message> pOriginalMessage, const String &sRecipient, const AnsiString &originalHeader);

   };

}
