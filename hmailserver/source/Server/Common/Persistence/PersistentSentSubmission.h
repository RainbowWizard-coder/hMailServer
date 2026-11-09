// Experimental native hMailServer persistence adapter; not installed in production.
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstdint>
#include "../Util/StdString.h"

namespace HM
{

   // The database table must be created by a reviewed schema migration first.
   // Calls must be made after the SMTP submission is durably accepted.
   // No operation in this class reads mailbox contents or deletes a message.
   class PersistentSentSubmission
   {
   public:
      // Called once during StartServers_ BEFORE registering SMTP sessions.
      // Does not block normal server startup if the ledger is unavailable.
      // Enabling experimental copy later requires restarting the server.
      static bool PrepareAtServerStart();
      static bool IsRecoveryReady();

      enum State { Pending = 0, CopyClaimed = 1, ServerCopied = 2,
                   ClientConfirmed = 3, DeleteClaimed = 4, Resolved = 5,
                   NeedsReview = 6 };

      // submissionId must be a new server-generated unique identifier for each
      // SMTP submission, NOT an RFC Message-ID or message-body fingerprint.
      static bool InsertPending(const String &submissionId, __int64 accountId,
                                __int64 createdUtcSeconds, __int64 expiresUtcSeconds);
      // Returns 1 when claim won, 0 if no matching pending row, -1 on DB failure.
      static __int64 ClaimCopy(const String &submissionId, __int64 accountId);
      // Returns 1 for a transition, 0 when state is not CopyClaimed, -1 on DB failure.
      // Atomically confirms the copy and its original MIME fingerprint.
      // Missing fingerprints must not create correlatable records.
      static __int64 CommitCopy(const String &submissionId, __int64 accountId,
                                __int64 messageId, const String &mimeSha256,
                                __int64 mimeBytes, __int64 confirmedUtcSeconds);
      // Build57.36: read-only candidate discovery.
      // The caller must independently validate the designated Sent folder.
      // A unique candidate is NOT an atomic claim.
      static bool FindUniqueSentCandidate(
         __int64 accountId,
         __int64 folderId,
         const String &mimeSha256,
         __int64 mimeBytes,
         __int64 nowUtcSeconds,
         __int64 &messageId);
      // Build57.37: conditional, single-row claim of a confirmed SMTP copy.
      // Returns 1 only for an acknowledged, unique atomic transition.
      // Returns 0 when no eligible unique candidate was claimed.
      // Returns -1 on failure or uncertain database outcome.
      // This does not authorize APPEND suppression without UID and
      // MULTIAPPEND/REPLACE integration.
      static __int64 ClaimUniqueSentCandidate(
         __int64 accountId,
         __int64 folderId,
         const String &mimeSha256,
         __int64 mimeBytes,
         __int64 nowUtcSeconds,
         __int64 &messageId);
      // Read state for reconciliation; returns false for absent or failed read.
      // No destructive reconciliation is implemented here.
      static bool GetState(const String &submissionId, __int64 accountId,
                           int &state, __int64 &messageId);
      // Only invoke at controlled cold start before new SMTP submissions.
      // Suspected partial copies require manual reconciliation, not retries.
      static __int64 QuarantineUncertainClaims();
      static __int64 MarkNeedsReview(const String &submissionId, __int64 accountId);
      // Confirmed copies and resolved entries expire after <=60m; uncertain states persist.
      static __int64 PruneResolved(__int64 nowUtcSeconds);
   };
}
