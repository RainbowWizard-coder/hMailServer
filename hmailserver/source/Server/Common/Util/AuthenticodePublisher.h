// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Whether an installer is signed by the publisher an administrator's install of
// an update accepts (docs/ServiceAccount.md, "The live update"), checked by
// Windows' own code on the bytes a held handle has.
//
// This is the one check of that install a compromised service cannot satisfy,
// so it is a hard gate in every configuration. The Sigstore bundle, the other
// signer, is parsed in a child process with the administrator's rights taken
// away (AdminUpdate.h); the stage cannot rely on what a child it had to
// de-elevate says, so it is this check that keeps a crafted bundle from being
// enough. Two pins, and nothing weaker:
//
//   THE PROJECT'S PUBLISHER, by default. The primary signer's subject is exactly
//   CN=Progressive Robot Ltd, O=Progressive Robot Ltd, L=Chester, S=Cheshire West
//   and Chester, C=GB - those five attributes and no other - and its chain is
//   four certificates ending in Microsoft ID Verified Code Signing PCA 2021 and
//   Microsoft Identity Verification Root Certificate Authority 2020, pinned by
//   the SHA-256 of their DER. The evidence, from the five signed releases 6.3.1
//   to 6.3.5 (checked 30 September 2026): the same subject, PCA and root on all
//   five; three different issuing CAs (ID Verified CS EOC CA 03, AOC CA 04, EOC
//   CA 04), so the issuing CA is not pinned; a leaf valid for three days, so the
//   leaf is not pinned either and the RFC 3161 timestamp is required; the
//   subscriber EKU 1.3.6.1.4.1.311.97.133916927.779571195.419187632.771983864
//   the same on all five, but it changes when the identity validation is
//   renewed, so it is not pinned (a renewal would otherwise stop every
//   installation's one-click install the same day). What the pin rests on is
//   Microsoft's verification of the company, Companies House 15400700, whose
//   name and registered office are the subject.
//
//   THE INSTALLED PROGRAM'S OWN SIGNER, for a site that signs its own builds -
//   one whose hMailServer.INI names, in UpdateSigningIdentity, a Sigstore
//   identity that is not one of this project's. The installer's primary signer's
//   subject must equal the primary signer's subject of the hMailServer.exe that
//   is installed and running, which only administrators can replace, and its
//   chain must end in the same root as that signature's does - a name is only
//   what one certificate authority was asked to write, and another authority
//   this machine trusts could have been asked for the same one. No setting
//   names the publisher, and none is added: an installed hMailServer.exe that is
//   not signed leaves nothing to pin to, and the one-click install is refused.
//
// Every attribute of a subject is compared whole, at the length Windows gives
// it: a value with a NUL in it is refused rather than cut at the NUL.
//
// Both require: WinVerifyTrust Valid, with revocation checked online for the
// whole chain but the root and the answer failing closed; the code-signing EKU;
// and an RFC 3161 timestamp. Revocation is the one network use in the elevated
// process, and it is Windows' own (crypt32), not a parser of this program's; it
// is bounded (VerifyHeld's timeout), and "could not be checked" is told apart
// from "revoked".
#ifndef HM_PLATFORM_POSIX

#include <string>
#include <utility>
#include <vector>

namespace HM
{
   namespace AuthenticodePublisher
   {
      // The SHA-256 of the two pinned certificates' DER, uppercase hex.
      extern const char *const IdentityVerificationRoot2020;      // valid to 2045-04-16
      extern const char *const IdVerifiedCodeSigningPca2021;      // valid to 2036-04-01

      // One attribute of a certificate's subject: its OID and its text.
      typedef std::pair<std::string, std::wstring> NameAttribute;

      struct Signer
      {
         Signer() : timestamped(false), secondarySignatures(0) {}

         std::vector<NameAttribute> subject;     // the primary signer's leaf, attribute by attribute
         std::wstring subjectText;               // "CN=..., O=..., ..." as Get-AuthenticodeSignature shows it
         std::vector<std::string> chainSha256;   // the primary signer's chain, leaf first, root last
         std::vector<std::wstring> chainNames;   // the same, each certificate's simple name
         std::vector<std::string> ekus;          // the leaf's enhanced key usages
         bool timestamped;                       // an RFC 3161 counter-signature is present
         unsigned long secondarySignatures;      // signatures beside the primary one
         std::wstring crlUrl;                    // the first CRL distribution point the leaf names
      };

      enum Outcome
      {
         Passed,
         NotSigned,               // no signature, or not a signed file
         BadDigest,               // the file changed since it was signed
         Revoked,                 // a certificate in the chain is revoked
         RevocationOffline,       // the revocation server could not be reached
         RevocationTimedOut,      // the check did not answer in time
         Untrusted,               // WinVerifyTrust refused it for another reason
         NotTimestamped,
         WrongPublisher,          // the subject is not the one pinned
         WrongChain,              // the chain does not end in the pinned PCA and root
         NoCodeSigning,           // the leaf has no code-signing EKU
         NothingToPinTo,          // site signer, and the installed hMailServer.exe is unsigned
         Failed                   // could not be checked
      };

      struct Result
      {
         Result() : outcome(Failed), status(0) {}

         Outcome outcome;
         long status;              // WinVerifyTrust's answer
         Signer signer;
         std::wstring why;         // one sentence, English, naming what failed
         std::wstring pin;         // which pin applied, for the stage's log
      };

      struct Pin
      {
         Pin() : sameAsInstalled(false) {}

         // False: the project's publisher (subject and Microsoft chain). True: the
         // subject and the root below, which are the installed hMailServer.exe's.
         bool sameAsInstalled;
         std::vector<NameAttribute> installedSubject;
         std::wstring installedSubjectText;
         std::string installedRootSha256;   // the SHA-256 of the root its chain ends in, uppercase hex
      };

      Pin ProjectPin();

      // The project's five subject attributes, as they are pinned.
      std::vector<NameAttribute> ProjectSubject();

      // The attributes of a DER-encoded X.500 name, each value whole: false, and
      // none, when it does not decode or a value holds a NUL.
      bool AttributesOfName(const unsigned char *der, unsigned long size, std::vector<NameAttribute> &attributes);

      // The pin alone, on what a verification found, with no file and no network:
      // Passed, or the outcome and why. For the project's publisher: the subject
      // exactly the five attributes, the chain four certificates whose root and
      // PCA are the pinned ones, the code-signing EKU, and a timestamp. For the
      // installed program's signer: the subject equal to it, attribute for
      // attribute, the chain ending in the same root, the code-signing EKU and a
      // timestamp.
      Outcome EvaluatePin(const Signer &signer, const Pin &pin, std::wstring &why);

      // WinVerifyTrust on what held holds (WINTRUST_FILE_INFO.hFile - the bytes
      // of the handle, not of a path looked up again), the primary signature
      // only, then the pin. With onlineRevocation, revocation is checked for the
      // whole chain but the root, over the network where the cache does not
      // answer, and anything but a clear answer refuses; the whole check is
      // given timeoutMilliseconds, after which it is refused as
      // RevocationTimedOut and left to finish on its own thread. Without it,
      // revocation is not checked (the testers' offline use).
      bool VerifyHeld(HANDLE held, const std::wstring &path, const Pin &pin, bool onlineRevocation, unsigned long timeoutMilliseconds,
                      Result &result);

      // The primary signer of a file that WinVerifyTrust calls Valid, without
      // revocation: what the site-signer pin is taken from (the installed
      // hMailServer.exe). False with why when it is not signed or not Valid.
      bool SignerOf(const std::wstring &path, Signer &signer, std::wstring &why);

      // What a WinVerifyTrust answer means for the install: Passed for
      // ERROR_SUCCESS; Revoked for CERT_E_REVOKED alone; RevocationOffline for
      // CRYPT_E_REVOCATION_OFFLINE and CRYPT_E_NO_REVOCATION_CHECK (the check could
      // not be made, which refuses too, but says so rather than "revoked");
      // BadDigest, NotSigned, and Untrusted for the rest.
      Outcome OutcomeOfStatus(long status);

      // "Revoked", "could not be checked" and the rest, for the stage's status.
      const wchar_t *OutcomeName(Outcome outcome);

      // The host of a URL ("www.microsoft.com" from "http://www.microsoft.com/pkiops/...").
      std::wstring HostOf(const std::wstring &url);
   }
}

#endif
