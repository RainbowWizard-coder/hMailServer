#pragma once

#include <cstdint>

namespace HM
{
   namespace SentDeduplication
   {
      // Build57B-C: SMTP/IMAP MIME matching policy.
      // Unit: seconds.
      //
      // This value defines only the correlation window.
      // It must not control the ledger retention period.
      //
      // The window begins at confirmed SMTP Sent persistence.
      // Account, designated Sent folder, SHA-256, MIME length,
      // uniqueness and atomic claim must also be verified.
      //
      // No per-client or per-account exceptions.

      constexpr std::int64_t MatchWindowSeconds = 5;
   }
}