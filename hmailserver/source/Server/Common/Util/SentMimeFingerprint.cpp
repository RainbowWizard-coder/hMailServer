#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <bcrypt.h>
#include "SentMimeFingerprint.h"
#include <array>
#include <vector>
#pragma comment(lib, "bcrypt.lib")

namespace sentlab {
namespace {
struct HandleGuard {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~HandleGuard() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
};
struct AlgGuard {
    BCRYPT_ALG_HANDLE h = nullptr;
    ~AlgGuard() { if (h) BCryptCloseAlgorithmProvider(h, 0); }
};
struct HashGuard {
    BCRYPT_HASH_HANDLE h = nullptr;
    ~HashGuard() { if (h) BCryptDestroyHash(h); }
};
std::string Hex(const unsigned char* p, size_t n) {
    static const char digits[] = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s.push_back(digits[p[i] >> 4]);
        s.push_back(digits[p[i] & 15]);
    }
    return s;
}
}
bool FingerprintFileFromOffset(const wchar_t* path, std::uint64_t offset, Fingerprint& output) {
    output = Fingerprint{};
    if (!path || !*path) return false;
    HandleGuard file;
    file.h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file.h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(file.h, &length) || length.QuadPart < 0 ||
        offset > static_cast<std::uint64_t>(length.QuadPart) ||
        offset > static_cast<std::uint64_t>(INT64_MAX)) return false;
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(file.h, position, nullptr, FILE_BEGIN)) return false;
    AlgGuard alg;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg.h, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) return false;
    DWORD object_len = 0, hash_len = 0, returned = 0;
    if (!BCRYPT_SUCCESS(BCryptGetProperty(alg.h, BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&object_len), sizeof(object_len), &returned, 0)) || object_len == 0) return false;
    if (!BCRYPT_SUCCESS(BCryptGetProperty(alg.h, BCRYPT_HASH_LENGTH,
        reinterpret_cast<PUCHAR>(&hash_len), sizeof(hash_len), &returned, 0)) || hash_len != 32) return false;
    std::vector<UCHAR> object(object_len);
    HashGuard hash;
    if (!BCRYPT_SUCCESS(BCryptCreateHash(alg.h, &hash.h, object.data(), object_len, nullptr, 0, 0))) return false;
    std::array<UCHAR, 65536> buffer{};
    std::uint64_t total = 0;
    while (true) {
        DWORD read = 0;
        if (!ReadFile(file.h, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) return false;
        if (read == 0) break;
        if (total > UINT64_MAX - read) return false;
        total += read;
        if (!BCRYPT_SUCCESS(BCryptHashData(hash.h, buffer.data(), read, 0))) return false;
    }
    std::array<UCHAR, 32> digest{};
    if (!BCRYPT_SUCCESS(BCryptFinishHash(hash.h, digest.data(), static_cast<ULONG>(digest.size()), 0))) return false;
    output.sha256_hex = Hex(digest.data(), digest.size());
    output.bytes = total;
    return true;
}

bool FingerprintFile(const wchar_t* path, Fingerprint& output) {
    return FingerprintFileFromOffset(path, 0, output);
}
}
