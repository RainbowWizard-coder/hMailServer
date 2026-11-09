#pragma once
// Etapa 54: SHA-256 of EXACT MIME bytes, no normalization.
// Standalone experimental component; not yet wired to SMTP/IMAP or SQLCE.
#include <cstdint>
#include <string>

namespace sentlab {
struct Fingerprint {
    std::string sha256_hex;
    std::uint64_t bytes = 0;
};
// Returns false on invalid path, I/O or CNG failure. Never logs message content.
bool FingerprintFile(const wchar_t* path, Fingerprint& output);
// SHA-256 of a file suffix beginning at the exact byte offset. No MIME normalization.
// Returns false if offset exceeds physical file length or read/CNG fails.
bool FingerprintFileFromOffset(const wchar_t* path, std::uint64_t offset, Fingerprint& output);
}
