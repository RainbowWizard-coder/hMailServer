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
      // filled from the header of the message's file. sAddressedTo is what the
      // notice gives as the message's To when that header cannot be read
      // (below).
      static void CreateMessageDeletedNotification(const std::shared_ptr<Message> pOriginalMessage, const String &sRecipient, const String &sAddressedTo);

      // The same notice, from a header read already: the file is not read, and
      // nothing is reported about it. A header with nothing in it - the file
      // could not be read - names the envelope's sender in place of the
      // header's From and sAddressedTo in place of its To: in a recipient's
      // notice that recipient alone, never the others, whom the message may
      // have reached unseen (Bcc); in the sender's notice the addresses the
      // sender sent it to (MessageRecipients::GetCommaSeparatedAddressesAsSent).
      static void CreateMessageDeletedNotification(const std::shared_ptr<Message> pOriginalMessage, const String &sRecipient, const String &sAddressedTo, const AnsiString &originalHeader);

   };

}
