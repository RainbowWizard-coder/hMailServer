// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <string>
#include <vector>

namespace HM
{
   // One signer a release may come from: what a signing certificate must record,
   // all of it together.
   struct SigstoreSigner
   {
      SigstoreSigner() : repository_id_optional(false), issued_before(0), version_in_ref(false) {}

      AnsiString identity_prefix;         // the certificate's subject alternative name must start with this
      AnsiString issuer;                  // the OIDC issuer the certificate records must be this
      AnsiString repository;              // the source repository it records must be this (empty: not checked)
      AnsiString repository_id;           // the repository's immutable id it records must be this (empty: not checked)
      bool repository_id_optional;        // ...and a certificate that records no id at all is not refused for it
      __int64 issued_before;              // the certificate must have been issued before this, Unix seconds (0: no limit)
      AnsiString last_version;            // the newest release this signer signed: a file is not installed as a later one (empty: no limit)
      bool version_in_ref;                // a tag the identity names after its "@" must be v<the release being installed>
   };

   // What a Sigstore bundle is checked against: the certificate authority that
   // issued the signing certificate, the transparency log that recorded the
   // signature, and the signers whose certificate is accepted. A certificate is
   // accepted when it is one signer's whole: its identity, that same signer's
   // issuer and that same signer's repository. A list, not a pattern: every entry
   // names one repository and one workflow exactly.
   struct SigstoreTrust
   {
      std::vector<std::string> ca_pems;   // the CA certificates trusted to have issued the signer's certificate
      std::string log_key_pem;            // the transparency log's public key
      std::vector<SigstoreSigner> signers;
   };

   struct SigstoreVerdict
   {
      SigstoreVerdict() : verified(false), integrated_time(0) {}

      bool verified;
      String error;                       // why not, when not
      AnsiString identity;                // the signer's subject alternative name
      __int64 integrated_time;            // when the log recorded the signature, Unix seconds
   };

   // Verifies a file against the Sigstore bundle cosign wrote for it, with no network
   // and no cosign: the bundle carries everything, and the trust is embedded.
   //
   // A release of this project is signed keylessly by its release workflow: the
   // workflow proves who it is to Fulcio with an OIDC token from the forge it runs
   // on, receives a ten-minute certificate naming the workflow, signs the file's
   // SHA-256 with the ephemeral key, and records the signature with Rekor, which
   // vouches for the time. The releases up to 6.3.3 were signed by the GitHub
   // Actions workflow sign-release.yml, and their bundles are re-hosted on GitLab
   // byte for byte; the releases after are to be signed by a job of this project's
   // GitLab CI pipeline for a v* tag, which no release has been yet. The bundle is
   // the certificate, the signature, and Rekor's record with its proof. Verifying
   // it means (RFC 9162 for the log, RFC 5280 for the chain):
   //
   //   1. the file's SHA-256 is the digest the bundle signs and the log recorded;
   //   2. the certificate chains to the Fulcio CA at the time the log recorded the
   //      signature - which is the only time a ten-minute certificate is valid, and
   //      why the log's word on the time matters;
   //   3. the certificate is one trusted signer's: that signer's workflow, issued
   //      on that signer's forge's token, for that signer's repository, by its
   //      id where the signer has one, before the signer's cut-off where it has
   //      one, for a release no later than the last it signed where it has one,
   //      and - where the workflow ran for a v* tag - for the tag of the release
   //      being installed;
   //   4. the signature verifies under the certificate's key;
   //   5. the log's record matches the signature and certificate, and the log vouches
   //      for it: its signed entry timestamp, or its inclusion proof to a checkpoint
   //      it signed, under the log's key.
   //
   // A file that fails any of these is not the release's file, whatever the
   // transport said.
   class SigstoreVerifier
   {
   public:
      // The public Sigstore instance (Fulcio's root and intermediate, Rekor's key)
      // and this project's two release signers: the GitHub workflow that signed
      // every release up to 6.3.3, and the GitLab CI pipeline for a v* tag.
      static SigstoreTrust DefaultTrust();

      // The trust as configured: DefaultTrust, with any UpdateTrustRootsFile,
      // UpdateLogPublicKeyFile, UpdateSigningIdentity, UpdateSigningIssuer and
      // UpdateSourceRepository from hMailServer.INI replacing their part of it -
      // of every signer in the list, so that all three set leave one signer.
      // False, with error, when a configured file cannot be read.
      static bool ConfiguredTrust(SigstoreTrust &trust, String &error);

      // expectedVersion is the release the file is being installed as ("6.3.4"):
      // a signer with last_version accepts no file as a release after it, and a
      // signer with version_in_ref accepts a certificate whose identity names a
      // tag only when the tag is v<expectedVersion>. Empty checks no version,
      // which is what --verify-update does, having no release to hold it to.
      static bool VerifyFile(const String &path, const AnsiString &bundleJson, const SigstoreTrust &trust, SigstoreVerdict &verdict,
                             const String &expectedVersion = String());

      // The bundle's bytes, exactly as cosign wrote them, for VerifyFile. Not
      // ReadCompleteTextFile: that reads text through the wide String and back,
      // and Rekor's checkpoint carries an em dash - three UTF-8 bytes that no
      // single-byte locale carries back. On Linux the round trip answered an
      // empty string (wcstombs refuses U+00E2 in the C locale), so no real
      // bundle ever verified there; under a UTF-8 locale it would have
      // double-encoded the three bytes, and the checkpoint's signature is over
      // the bytes the log wrote. False when the file cannot be read.
      static bool ReadBundle(const String &path, AnsiString &bundleJson);

      static bool Sha256File(const String &path, std::vector<unsigned char> &digest, String &error);
      static bool Base64Decode(const std::string &text, std::vector<unsigned char> &out);
      static std::string Hex(const std::vector<unsigned char> &bytes);
   };
}
