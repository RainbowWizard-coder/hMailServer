// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "PersistenceMode.h"

namespace HM
{
   class JournalRule;

   class PersistentJournalRule
   {
   public:
      PersistentJournalRule();
      virtual ~PersistentJournalRule();

      static bool DeleteObject(std::shared_ptr<JournalRule> rule);

      // Every rule of a domain, and every rule naming an account. Called by the
      // domain's and the account's own delete beside the other per-domain and
      // per-account stores, so that a reissued id does not inherit a rule, and so
      // the running cache learns of it (Journaling::SetNeedRefresh).
      static bool DeleteByDomain(__int64 domainId);
      static bool DeleteByAccount(__int64 accountId);

      static bool SaveObject(std::shared_ptr<JournalRule> rule);
      static bool SaveObject(std::shared_ptr<JournalRule> rule, String &errorMessage, PersistenceMode mode);
      // The id is sixty-four bits the whole way. It used to be a long here and an
      // int in the parameter, which are one type on Windows: an id above the
      // thirty-two-bit range wrapped onto a real row, so a PUT or a DELETE that
      // should have answered 404 edited or destroyed somebody else's rule and
      // said it had succeeded.
      static bool ReadObject(std::shared_ptr<JournalRule> rule, __int64 id);
      static bool ReadObject(std::shared_ptr<JournalRule> rule, std::shared_ptr<DALRecordset> recordset);

      // The target the administrator typed - a domain name or an account address,
      // according to the rule's scope - resolved to the domain id and the account
      // id the row holds. False, with the sentence, when no such domain or account
      // is hosted here; a rule cannot name a domain this server does not have.
      static bool ResolveTarget(std::shared_ptr<JournalRule> rule, String &errorMessage);
   };
}
