// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "IMAPCommand.h"

namespace HM
{
   
   class IMAPCommandLIST  : public IMAPCommand
   {
   public:
      IMAPCommandLIST();
      virtual ~IMAPCommandLIST();

      virtual IMAPResult ExecuteCommand(std::shared_ptr<HM::IMAPConnection> pConnection, std::shared_ptr<IMAPCommandArgument> pArgument);

   private:
      // RFC 5258: split a parenthesised mailbox-pattern list into individual patterns.
      // A literal in it is the command's literal iLiteralIndex, the next the one after.
      static void ExtractPatterns_(const String &sParenContent, std::shared_ptr<IMAPCommandArgument> pArgument, unsigned int iLiteralIndex, std::vector<String> &patterns);

      // True for "{n}" or "{n+}": what a literal leaves of itself in the command text.
      static bool IsLiteralMarker_(const String &sToken);

      // True when the parenthesised selection/return option list contains sOption as a
      // whole, case-insensitive token.
      static bool HasOption_(const String &sParenContent, const String &sOption);

      // True when every option in the parenthesised list is one of the known ones.
      // An option's own parenthesised argument - STATUS (MESSAGES) - is passed
      // over, not read as options.
      static bool OptionsAreKnown_(const String &sParenContent, const std::vector<String> &knownOptions);

      // RFC 5819 (LIST-STATUS): builds the "* STATUS ..." line that follows one
      // "* LIST ..." line, or "" for mailboxes that get none (\Noselect, no read
      // permission, lookup failure).
      static String CreateListStatusLine_(std::shared_ptr<IMAPConnection> pConnection, const String &sListLine, const String &sStatusItems);
   };

}
