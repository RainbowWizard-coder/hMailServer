// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#ifndef HM_PLATFORM_POSIX

#include "AuthenticodePublisher.h"

#include <bcrypt.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>

#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")

// Nothing in this file writes a log line, reports an error or reads a setting:
// it runs inside hMailServer.exe /InstallUpdate, elevated, before anything of the
// server's own is set up (build/check-elevated-verbs.py holds it to that).

namespace HM
{
   namespace AuthenticodePublisher
   {
      const char *const IdentityVerificationRoot2020 = "5367F20C7ADE0E2BCA790915056D086B720C33C1FA2A2661ACF787E3292E1270";
      const char *const IdVerifiedCodeSigningPca2021 = "3D29798CC5D3F0644A7E0DC9CB1CADE523EA5EC83B335109B605BFEAA7D5F5C1";

      bool AttributesOfName(const unsigned char *der, unsigned long size, std::vector<NameAttribute> &attributes)
      {
         attributes.clear();
         CERT_NAME_INFO *info = nullptr;
         DWORD decoded = 0;
         // X509_NAME is wincrypt's structure type 7; the name itself is OpenSSL's
         // type once OpenSSL's headers are in, as they are in the server's.
         if (der == nullptr || size == 0 ||
             !CryptDecodeObjectEx(X509_ASN_ENCODING, (LPCSTR) 7, der, size, CRYPT_DECODE_ALLOC_FLAG, nullptr, &info, &decoded) || info == nullptr)
            return false;

         // Each value at the length CertRDNValueToStr gives, not up to its first
         // NUL: "Progressive Robot Ltd\0anything" is not "Progressive Robot Ltd",
         // and a subject whose value holds a NUL is not one to pin to at all.
         bool clean = true;
         for (DWORD r = 0; r < info->cRDN; r++)
         {
            const CERT_RDN &rdn = info->rgRDN[r];
            for (DWORD a = 0; a < rdn.cRDNAttr; a++)
            {
               const CERT_RDN_ATTR &attribute = rdn.rgRDNAttr[a];
               const DWORD needed = CertRDNValueToStrW(attribute.dwValueType, const_cast<PCERT_RDN_VALUE_BLOB>(&attribute.Value), nullptr, 0);
               std::vector<wchar_t> text(needed ? needed : 1, L'\0');
               const DWORD written = CertRDNValueToStrW(attribute.dwValueType, const_cast<PCERT_RDN_VALUE_BLOB>(&attribute.Value), text.data(), (DWORD) text.size());
               const size_t length = written > 0 ? (size_t) written - 1 : 0;
               std::wstring value(text.data(), std::min<size_t>(length, text.size()));
               if (value.find(L'\0') != std::wstring::npos)
                  clean = false;
               attributes.push_back(NameAttribute(attribute.pszObjId ? attribute.pszObjId : "", value));
            }
         }
         LocalFree(info);
         if (!clean)
            attributes.clear();
         return clean;
      }

      namespace
      {
         const char *CodeSigningEku_ = "1.3.6.1.5.5.7.3.3";

         std::string Sha256OfCertificate_(PCCERT_CONTEXT certificate)
         {
            BYTE hash[32];
            DWORD length = sizeof(hash);
            if (!CryptHashCertificate2(BCRYPT_SHA256_ALGORITHM, 0, nullptr, certificate->pbCertEncoded, certificate->cbCertEncoded, hash, &length) ||
                length != sizeof(hash))
               return std::string();
            const char *digits = "0123456789ABCDEF";
            std::string hex;
            for (DWORD i = 0; i < length; i++)
            {
               hex += digits[hash[i] >> 4];
               hex += digits[hash[i] & 0x0F];
            }
            return hex;
         }

         std::wstring SimpleName_(PCCERT_CONTEXT certificate)
         {
            wchar_t name[512];
            const DWORD length = CertGetNameStringW(certificate, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name, 512);
            return length > 1 ? std::wstring(name) : std::wstring();
         }

         std::wstring SubjectText_(PCCERT_CONTEXT certificate)
         {
            const DWORD needed = CertNameToStrW(X509_ASN_ENCODING, &certificate->pCertInfo->Subject,
                                                CERT_X500_NAME_STR | CERT_NAME_STR_REVERSE_FLAG, nullptr, 0);
            if (needed <= 1)
               return std::wstring();
            std::vector<wchar_t> text(needed);
            CertNameToStrW(X509_ASN_ENCODING, &certificate->pCertInfo->Subject, CERT_X500_NAME_STR | CERT_NAME_STR_REVERSE_FLAG, text.data(), needed);
            return std::wstring(text.data());
         }

         bool SubjectAttributes_(PCCERT_CONTEXT certificate, std::vector<NameAttribute> &attributes)
         {
            return AttributesOfName(certificate->pCertInfo->Subject.pbData, certificate->pCertInfo->Subject.cbData, attributes);
         }

         void Ekus_(PCCERT_CONTEXT certificate, std::vector<std::string> &ekus)
         {
            ekus.clear();
            DWORD size = 0;
            if (!CertGetEnhancedKeyUsage(certificate, CERT_FIND_EXT_ONLY_ENHKEY_USAGE_FLAG, nullptr, &size) || size == 0)
               return;
            std::vector<unsigned char> buffer(size);
            CERT_ENHKEY_USAGE *usage = (CERT_ENHKEY_USAGE *) buffer.data();
            if (!CertGetEnhancedKeyUsage(certificate, CERT_FIND_EXT_ONLY_ENHKEY_USAGE_FLAG, usage, &size))
               return;
            for (DWORD i = 0; i < usage->cUsageIdentifier; i++)
               ekus.push_back(usage->rgpszUsageIdentifier[i]);
         }

         std::wstring FirstCrlUrl_(PCCERT_CONTEXT certificate)
         {
            PCERT_EXTENSION extension = CertFindExtension(szOID_CRL_DIST_POINTS, certificate->pCertInfo->cExtension, certificate->pCertInfo->rgExtension);
            if (extension == nullptr)
               return std::wstring();
            CRL_DIST_POINTS_INFO *points = nullptr;
            DWORD size = 0;
            if (!CryptDecodeObjectEx(X509_ASN_ENCODING, X509_CRL_DIST_POINTS, extension->Value.pbData, extension->Value.cbData,
                                     CRYPT_DECODE_ALLOC_FLAG, nullptr, &points, &size) || points == nullptr)
               return std::wstring();
            std::wstring url;
            for (DWORD p = 0; p < points->cDistPoint && url.empty(); p++)
            {
               const CRL_DIST_POINT_NAME &name = points->rgDistPoint[p].DistPointName;
               if (name.dwDistPointNameChoice != CRL_DIST_POINT_FULL_NAME)
                  continue;
               for (DWORD e = 0; e < name.FullName.cAltEntry && url.empty(); e++)
               {
                  const CERT_ALT_NAME_ENTRY &entry = name.FullName.rgAltEntry[e];
                  if (entry.dwAltNameChoice == CERT_ALT_NAME_URL && entry.pwszURL != nullptr)
                     url = entry.pwszURL;
               }
            }
            LocalFree(points);
            return url;
         }

         // What WinVerifyTrust's state holds about the primary signer.
         void ReadSigner_(HANDLE stateData, Signer &signer)
         {
            CRYPT_PROVIDER_DATA *provider = WTHelperProvDataFromStateData(stateData);
            if (provider == nullptr)
               return;
            CRYPT_PROVIDER_SGNR *primary = WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0);
            if (primary == nullptr)
               return;

            signer.timestamped = primary->csCounterSigners > 0;

            PCCERT_CONTEXT leaf = nullptr;
            if (primary->pChainContext != nullptr && primary->pChainContext->cChain > 0 && primary->pChainContext->rgpChain[0] != nullptr)
            {
               const CERT_SIMPLE_CHAIN *chain = primary->pChainContext->rgpChain[0];
               for (DWORD i = 0; i < chain->cElement; i++)
               {
                  PCCERT_CONTEXT certificate = chain->rgpElement[i]->pCertContext;
                  signer.chainSha256.push_back(Sha256OfCertificate_(certificate));
                  signer.chainNames.push_back(SimpleName_(certificate));
                  signer.chainErrors.push_back(chain->rgpElement[i]->TrustStatus.dwErrorStatus);
                  if (i == 0)
                     leaf = certificate;
               }
            }
            else if (primary->csCertChain > 0 && primary->pasCertChain != nullptr)
            {
               leaf = primary->pasCertChain[0].pCert;
            }

            if (leaf != nullptr)
            {
               SubjectAttributes_(leaf, signer.subject);
               signer.subjectText = SubjectText_(leaf);
               Ekus_(leaf, signer.ekus);
               signer.crlUrl = FirstCrlUrl_(leaf);
            }
         }

         bool SameAttributes_(std::vector<NameAttribute> left, std::vector<NameAttribute> right)
         {
            std::sort(left.begin(), left.end());
            std::sort(right.begin(), right.end());
            return left == right;
         }

         Outcome OutcomeOfStatusInner_(long status)
         {
            switch ((unsigned long) status)
            {
            case ERROR_SUCCESS:
               return Passed;
            case (unsigned long) TRUST_E_NOSIGNATURE:
            case (unsigned long) TRUST_E_SUBJECT_FORM_UNKNOWN:
            case (unsigned long) TRUST_E_PROVIDER_UNKNOWN:
               return NotSigned;
            case (unsigned long) TRUST_E_BAD_DIGEST:
               return BadDigest;
            case (unsigned long) CERT_E_REVOKED:
               return Revoked;
            case (unsigned long) CRYPT_E_REVOCATION_OFFLINE:
            case (unsigned long) CRYPT_E_NO_REVOCATION_CHECK:
               return RevocationOffline;
            default:
               return Untrusted;
            }
         }

         std::wstring Hex_(long status)
         {
            wchar_t text[16];
            swprintf_s(text, L"0x%08lX", (unsigned long) status);
            return text;
         }

         // One WinVerifyTrust call and its reading, on whatever thread runs it.
         struct Work_
         {
            Work_() : held(INVALID_HANDLE_VALUE), online(false), status(0), done(false) {}
            ~Work_()
            {
               if (held != INVALID_HANDLE_VALUE)
                  CloseHandle(held);
            }

            HANDLE held;
            std::wstring path;
            bool online;

            long status;
            Signer signer;

            std::mutex mutex;
            std::condition_variable finished;
            bool done;
         };

         void Run_(std::shared_ptr<Work_> work)
         {
            WINTRUST_FILE_INFO file;
            memset(&file, 0, sizeof(file));
            file.cbStruct = sizeof(file);
            file.pcwszFilePath = work->path.c_str();
            file.hFile = work->held;

            // The primary signature, and how many others there are beside it.
            WINTRUST_SIGNATURE_SETTINGS settings;
            memset(&settings, 0, sizeof(settings));
            settings.cbStruct = sizeof(settings);
            settings.dwIndex = 0;
            settings.dwFlags = WSS_VERIFY_SPECIFIC | WSS_GET_SECONDARY_SIG_COUNT;

            WINTRUST_DATA data;
            memset(&data, 0, sizeof(data));
            data.cbStruct = sizeof(data);
            data.dwUIChoice = WTD_UI_NONE;
            data.fdwRevocationChecks = work->online ? WTD_REVOKE_WHOLECHAIN : WTD_REVOKE_NONE;
            data.dwUnionChoice = WTD_CHOICE_FILE;
            data.pFile = &file;
            data.dwStateAction = WTD_STATEACTION_VERIFY;
            data.dwProvFlags = WTD_DISABLE_MD2_MD4 |
                               (work->online ? WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT : (WTD_REVOCATION_CHECK_NONE | WTD_CACHE_ONLY_URL_RETRIEVAL));
            data.pSignatureSettings = &settings;

            GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
            const LONG status = WinVerifyTrust((HWND) INVALID_HANDLE_VALUE, &action, &data);

            Signer signer;
            if (data.hWVTStateData != nullptr)
               ReadSigner_(data.hWVTStateData, signer);
            signer.secondarySignatures = settings.cSecondarySigs;

            data.dwStateAction = WTD_STATEACTION_CLOSE;
            WinVerifyTrust((HWND) INVALID_HANDLE_VALUE, &action, &data);

            std::lock_guard<std::mutex> lock(work->mutex);
            work->status = status;
            work->signer = signer;
            work->done = true;
            work->finished.notify_all();
         }
      }

      Outcome OutcomeOfStatus(long status)
      {
         return OutcomeOfStatusInner_(status);
      }

      bool RevocationEstablished(const std::vector<unsigned long> &chainErrors)
      {
         if (chainErrors.empty())
            return false;
         // The root is not checked (WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT): a
         // chain of one is its own root, with nothing below it to check.
         const unsigned long unknown = CERT_TRUST_REVOCATION_STATUS_UNKNOWN | CERT_TRUST_IS_OFFLINE_REVOCATION | CERT_TRUST_IS_REVOKED;
         for (size_t i = 0; i + 1 < chainErrors.size(); i++)
         {
            if ((chainErrors[i] & unknown) != 0)
               return false;
         }
         return true;
      }

      std::vector<NameAttribute> ProjectSubject()
      {
         std::vector<NameAttribute> subject;
         subject.push_back(NameAttribute("2.5.4.3", L"Progressive Robot Ltd"));
         subject.push_back(NameAttribute("2.5.4.10", L"Progressive Robot Ltd"));
         subject.push_back(NameAttribute("2.5.4.7", L"Chester"));
         subject.push_back(NameAttribute("2.5.4.8", L"Cheshire West and Chester"));
         subject.push_back(NameAttribute("2.5.4.6", L"GB"));
         return subject;
      }

      Pin ProjectPin()
      {
         return Pin();
      }

      std::wstring HostOf(const std::wstring &url)
      {
         size_t start = url.find(L"://");
         start = start == std::wstring::npos ? 0 : start + 3;
         size_t end = url.find_first_of(L"/:?#", start);
         return url.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
      }

      const wchar_t *OutcomeName(Outcome outcome)
      {
         switch (outcome)
         {
         case Passed: return L"passed";
         case NotSigned: return L"not signed";
         case BadDigest: return L"changed since it was signed";
         case Revoked: return L"revoked";
         case RevocationOffline: return L"revocation could not be checked";
         case RevocationTimedOut: return L"revocation check timed out";
         case Untrusted: return L"not trusted";
         case NotTimestamped: return L"not timestamped";
         case WrongPublisher: return L"another publisher";
         case WrongChain: return L"another certificate chain";
         case NoCodeSigning: return L"not a code-signing certificate";
         case NothingToPinTo: return L"nothing to pin to";
         default: return L"could not be checked";
         }
      }

      Outcome EvaluatePin(const Signer &signer, const Pin &pin, std::wstring &why)
      {
         if (signer.subject.empty())
         {
            why = L"the signer's certificate could not be read";
            return Failed;
         }

         if (!pin.sameAsInstalled)
         {
            if (!SameAttributes_(signer.subject, ProjectSubject()))
            {
               why = L"it is signed by " + (signer.subjectText.empty() ? std::wstring(L"a publisher whose name could not be read") : signer.subjectText) +
                     L", not by CN=Progressive Robot Ltd, O=Progressive Robot Ltd, L=Chester, S=Cheshire West and Chester, C=GB";
               return WrongPublisher;
            }
            if (signer.chainSha256.size() != 4 || signer.chainSha256[3] != IdentityVerificationRoot2020 || signer.chainSha256[2] != IdVerifiedCodeSigningPca2021)
            {
               std::wstring chain;
               for (size_t i = 1; i < signer.chainNames.size(); i++)
                  chain += (chain.empty() ? L"" : L" <- ") + signer.chainNames[i];
               why = L"its certificate chain (" + (chain.empty() ? std::wstring(L"unreadable") : chain) +
                     L") does not end in Microsoft ID Verified Code Signing PCA 2021 and Microsoft Identity Verification Root Certificate Authority 2020";
               return WrongChain;
            }
         }
         else
         {
            if (pin.installedSubject.empty())
            {
               why = L"the installed hMailServer.exe is not signed, so there is no publisher to hold the installer to";
               return NothingToPinTo;
            }
            if (!SameAttributes_(signer.subject, pin.installedSubject))
            {
               why = L"it is signed by " + (signer.subjectText.empty() ? std::wstring(L"a publisher whose name could not be read") : signer.subjectText) +
                     L", not by " + pin.installedSubjectText + L", who signed the hMailServer.exe installed here";
               return WrongPublisher;
            }
            // And through the same root: a name is what a certificate authority
            // was asked to put in it, so the same name from another of the
            // authorities this machine trusts is not the same signer.
            if (pin.installedRootSha256.empty() || signer.chainSha256.empty() || signer.chainSha256.back() != pin.installedRootSha256)
            {
               why = L"its certificate chain ends in " + (signer.chainNames.empty() ? std::wstring(L"a root that could not be read") : signer.chainNames.back()) +
                     L", not in the root the signature of the hMailServer.exe installed here ends in";
               return WrongChain;
            }
         }

         if (std::find(signer.ekus.begin(), signer.ekus.end(), std::string(CodeSigningEku_)) == signer.ekus.end())
         {
            why = L"its certificate is not issued for code signing";
            return NoCodeSigning;
         }
         if (!signer.timestamped)
         {
            why = L"its signature carries no RFC 3161 timestamp";
            return NotTimestamped;
         }
         return Passed;
      }

      bool VerifyHeld(HANDLE held, const std::wstring &path, const Pin &pin, bool onlineRevocation, unsigned long timeoutMilliseconds, Result &result)
      {
         result = Result();
         result.pin = pin.sameAsInstalled
            ? L"the signer of the installed hMailServer.exe, " + pin.installedSubjectText
            : std::wstring(L"the project's publisher, CN=Progressive Robot Ltd through Microsoft ID Verified Code Signing PCA 2021");

         std::shared_ptr<Work_> work = std::make_shared<Work_>();
         work->path = path;
         work->online = onlineRevocation;
         if (!DuplicateHandle(GetCurrentProcess(), held, GetCurrentProcess(), &work->held, 0, FALSE, DUPLICATE_SAME_ACCESS))
         {
            work->held = INVALID_HANDLE_VALUE;
            result.outcome = Failed;
            result.why = L"its handle could not be duplicated (Windows error " + std::to_wstring(GetLastError()) + L")";
            return false;
         }

         // Its own thread, so that a revocation server that does not answer holds
         // this process for the timeout and no longer; a thread given up on
         // finishes on its own and closes its own copy of the handle.
         std::thread worker(Run_, work);
         bool done = false;
         {
            std::unique_lock<std::mutex> lock(work->mutex);
            done = work->finished.wait_for(lock, std::chrono::milliseconds(timeoutMilliseconds), [&] { return work->done; });
         }
         if (!done)
         {
            worker.detach();
            result.outcome = RevocationTimedOut;
            result.why = L"checking whether its certificates are revoked did not finish within " + std::to_wstring(timeoutMilliseconds / 1000) +
                         L" seconds, so it is refused rather than trusted";
            return false;
         }
         worker.join();

         result.status = work->status;
         result.signer = work->signer;

         const Outcome trust = OutcomeOfStatus(work->status);
         if (trust != Passed)
         {
            result.outcome = trust;
            const std::wstring host = HostOf(result.signer.crlUrl);
            switch (trust)
            {
            case NotSigned:
               result.why = L"it carries no Authenticode signature (" + Hex_(work->status) + L")";
               break;
            case BadDigest:
               result.why = L"it has been changed since it was signed (TRUST_E_BAD_DIGEST)";
               break;
            case Revoked:
               result.why = L"a certificate it is signed with has been revoked (CERT_E_REVOKED)";
               break;
            case RevocationOffline:
               result.why = L"the revocation server " + (host.empty() ? std::wstring(L"its certificate names") : host) +
                            L" could not be reached, so whether its certificate is revoked could not be checked (" + Hex_(work->status) + L")";
               break;
            default:
               result.why = L"Windows does not trust its signature (WinVerifyTrust " + Hex_(work->status) + L")";
               break;
            }
            return false;
         }

         // Windows' answer above takes a revocation server that could not be
         // reached as good; the chain it built says whether it was.
         if (onlineRevocation && !result.signer.chainErrors.empty() && !RevocationEstablished(result.signer.chainErrors))
         {
            const std::wstring host = HostOf(result.signer.crlUrl);
            result.outcome = RevocationOffline;
            result.why = L"the revocation server " + (host.empty() ? std::wstring(L"its certificate names") : host) +
                         L" could not be reached, so whether its certificate is revoked could not be checked (its chain's revocation status is unknown)";
            return false;
         }

         std::wstring why;
         result.outcome = EvaluatePin(result.signer, pin, why);
         result.why = why;
         return result.outcome == Passed;
      }

      bool SignerOf(const std::wstring &path, Signer &signer, std::wstring &why)
      {
         signer = Signer();
         HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
         if (file == INVALID_HANDLE_VALUE)
         {
            why = path + L" could not be opened (Windows error " + std::to_wstring(GetLastError()) + L")";
            return false;
         }

         Result result;
         Pin none;
         none.sameAsInstalled = true;
         VerifyHeld(file, path, none, false, 60000, result);
         CloseHandle(file);

         if (result.status != ERROR_SUCCESS || result.signer.subject.empty())
         {
            why = result.outcome == NotSigned ? path + L" is not signed" : path + L" is not signed validly: " + result.why;
            return false;
         }
         signer = result.signer;
         return true;
      }
   }
}

#endif
