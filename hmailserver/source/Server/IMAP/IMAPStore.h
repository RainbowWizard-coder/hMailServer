// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#pragma once

#include "IMAPCommandRangeAction.h"

namespace HM
{

   class IMAPStore : public IMAPCommandRangeAction
   {
   public:
      IMAPStore();
      virtual ~IMAPStore();

      IMAPResult DoAction(std::shared_ptr<IMAPConnection> pConnection, int messageIndex, std::shared_ptr<Message> pMessage, const std::shared_ptr<IMAPCommandArgument> pArgument);

      // STORE updates the flags on the message, so it must not work on a copy.
      virtual bool UsesLiveMessages() const { return true; }
      static String GetMessageFlags(std::shared_ptr<Message> pMessage, int messageIndex, bool includeModSeq);

      // RFC 7162 (CONDSTORE): "[MODIFIED <set>] " prefix listing the messages skipped because
      // their mod-sequence exceeded the UNCHANGEDSINCE value, or "" when none were skipped.
      virtual String GetConditionalStoreResponseCode();

   private:

      void ParseModifier_(const String &sCommand);
      IMAPResult ParseFlags_(const String &sCommand);

      bool modifier_parsed_ = false;
      bool has_unchangedsince_ = false;
      __int64 unchangedsince_ = 0;
      std::vector<unsigned int> modified_;

      // ParseFlags_'s reading of the command, made for its first message.
      bool flags_parsed_ = false;
      IMAPResult flags_result_;
      bool silent_ = false;
      bool seen_ = false;
      bool deleted_ = false;
      bool draft_ = false;
      bool answered_ = false;
      bool flagged_ = false;
      bool removing_ = false;
      bool adding_ = false;
      bool replacing_ = false;
      std::vector<String> keywords_given_;
   };

   // The keywords a message keeps of a list (Message::SetKeywordList, which a
   // STORE's FLAGS list reaches) held to what 6.3.5 kept - its code kept here -
   // over generated lists, and to the time it takes: 60,000 repeated keywords,
   // each compared with the few kept, where 6.3.5 compared each with every one
   // before it. Run by ClassTester on Windows and by imap-parse-tests on Linux.
   class IMAPStoreKeywordsTester
   {
   public:
      void Test(int cases = 20000);
   };

}
