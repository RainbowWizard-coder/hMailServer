// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "SigstoreVerifier.h"
#include "JsonDocument.h"
#include "FileUtilities.h"
#include "Unicode.h"
#include "UpdateChecker.h"
#include "../Application/IniFileSettings.h"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <fstream>
#include <memory>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // The public Sigstore instance's trust root, as published in the
      // sigstore/root-signing repository (targets/fulcio_v1.crt.pem,
      // fulcio_intermediate_v1.crt.pem, rekor.pub). Both certificates expire on
      // 5 October 2031; a release before then will carry their successors.
      const char *FULCIO_ROOT_PEM =
         "-----BEGIN CERTIFICATE-----\n"
         "MIIB9zCCAXygAwIBAgIUALZNAPFdxHPwjeDloDwyYChAO/4wCgYIKoZIzj0EAwMw\n"
         "KjEVMBMGA1UEChMMc2lnc3RvcmUuZGV2MREwDwYDVQQDEwhzaWdzdG9yZTAeFw0y\n"
         "MTEwMDcxMzU2NTlaFw0zMTEwMDUxMzU2NThaMCoxFTATBgNVBAoTDHNpZ3N0b3Jl\n"
         "LmRldjERMA8GA1UEAxMIc2lnc3RvcmUwdjAQBgcqhkjOPQIBBgUrgQQAIgNiAAT7\n"
         "XeFT4rb3PQGwS4IajtLk3/OlnpgangaBclYpsYBr5i+4ynB07ceb3LP0OIOZdxex\n"
         "X69c5iVuyJRQ+Hz05yi+UF3uBWAlHpiS5sh0+H2GHE7SXrk1EC5m1Tr19L9gg92j\n"
         "YzBhMA4GA1UdDwEB/wQEAwIBBjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQWBBRY\n"
         "wB5fkUWlZql6zJChkyLQKsXF+jAfBgNVHSMEGDAWgBRYwB5fkUWlZql6zJChkyLQ\n"
         "KsXF+jAKBggqhkjOPQQDAwNpADBmAjEAj1nHeXZp+13NWBNa+EDsDP8G1WWg1tCM\n"
         "WP/WHPqpaVo0jhsweNFZgSs0eE7wYI4qAjEA2WB9ot98sIkoF3vZYdd3/VtWB5b9\n"
         "TNMea7Ix/stJ5TfcLLeABLE4BNJOsQ4vnBHJ\n"
         "-----END CERTIFICATE-----\n";

      const char *FULCIO_INTERMEDIATE_PEM =
         "-----BEGIN CERTIFICATE-----\n"
         "MIICGjCCAaGgAwIBAgIUALnViVfnU0brJasmRkHrn/UnfaQwCgYIKoZIzj0EAwMw\n"
         "KjEVMBMGA1UEChMMc2lnc3RvcmUuZGV2MREwDwYDVQQDEwhzaWdzdG9yZTAeFw0y\n"
         "MjA0MTMyMDA2MTVaFw0zMTEwMDUxMzU2NThaMDcxFTATBgNVBAoTDHNpZ3N0b3Jl\n"
         "LmRldjEeMBwGA1UEAxMVc2lnc3RvcmUtaW50ZXJtZWRpYXRlMHYwEAYHKoZIzj0C\n"
         "AQYFK4EEACIDYgAE8RVS/ysH+NOvuDZyPIZtilgUF9NlarYpAd9HP1vBBH1U5CV7\n"
         "7LSS7s0ZiH4nE7Hv7ptS6LvvR/STk798LVgMzLlJ4HeIfF3tHSaexLcYpSASr1kS\n"
         "0N/RgBJz/9jWCiXno3sweTAOBgNVHQ8BAf8EBAMCAQYwEwYDVR0lBAwwCgYIKwYB\n"
         "BQUHAwMwEgYDVR0TAQH/BAgwBgEB/wIBADAdBgNVHQ4EFgQU39Ppz1YkEZb5qNjp\n"
         "KFWixi4YZD8wHwYDVR0jBBgwFoAUWMAeX5FFpWapesyQoZMi0CrFxfowCgYIKoZI\n"
         "zj0EAwMDZwAwZAIwPCsQK4DYiZYDPIaDi5HFKnfxXx6ASSVmERfsynYBiX2X6SJR\n"
         "nZU84/9DZdnFvvxmAjBOt6QpBlc4J/0DxvkTCqpclvziL6BCCPnjdlIB3Pu3BxsP\n"
         "mygUY7Ii2zbdCdliiow=\n"
         "-----END CERTIFICATE-----\n";

      const char *REKOR_PUBLIC_KEY_PEM =
         "-----BEGIN PUBLIC KEY-----\n"
         "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAE2G2Y+2tabdTV5BcGiBIx0a9fAFwr\n"
         "kBbmLSGtks4L3qX6yYY0zufBnhC8Ur/iy55GhWP/9A/bY2LhC30M9+RYtw==\n"
         "-----END PUBLIC KEY-----\n";

      // The signers a release is accepted from. Each is a pin: one repository, one
      // workflow, one issuer, compared whole - never a pattern a stranger's
      // repository or workflow could also satisfy.
      //
      // Every release up to 6.3.3: this repository's GitHub Actions workflow, as
      // Fulcio names it in the certificate's subject alternative name (the ref
      // after @ is the branch or tag the workflow ran from), GitHub's token service
      // as the issuer, the repository, and the repository's numeric id, which
      // GitHub never reuses. GitHub suspended the account on 18 September 2026 and
      // nothing can be signed there by this project any more; the entry stays for
      // the bundles already made, which are re-hosted on GitLab byte for byte and
      // are what a rollback to one of those releases verifies. It is held to them:
      // a certificate issued from the suspension on is refused, so that whoever
      // might one day hold the name cannot sign as it. All 80 bundles on the
      // re-hosted releases (6.2.19 to 6.3.3) carry the id 1267388815 and were
      // issued by 2026-09-15T01:00:18Z. The cut-off is the certificate's notBefore,
      // which Fulcio signs, and not the log's integratedTime, which a bundle with
      // an inclusion proof alone carries unsigned; the time is inside the
      // certificate's ten minutes in any case (step 4).
      //
      // And it is held to the releases it signed. The cut-off stops new
      // certificates, not old files: 79 of the 80 bundles on the feed were made by
      // a workflow run from a branch (refs/heads/master), so their identity names
      // no release,
      // and without this a feed that was tampered with could offer the genuine
      // 6.2.19 installer as "6.3.9" to a server on 6.3.4 - every check would pass,
      // and the server would install a release too old for its own database. No
      // file this signer made is a release after 6.3.3, so none is installed as
      // one; a server on 6.3.3 still rolls back to its own GitHub-signed release.
      const char *GITHUB_IDENTITY_PREFIX = "https://github.com/Progressiverobot/hmailserver/.github/workflows/sign-release.yml@";
      const char *GITHUB_ISSUER = "https://token.actions.githubusercontent.com";
      const char *GITHUB_REPOSITORY = "https://github.com/Progressiverobot/hmailserver";
      const char *GITHUB_REPOSITORY_ID = "1267388815";
      const __int64 GITHUB_ISSUED_BEFORE = 1789689600;   // 2026-09-18T00:00:00Z
      const char *GITHUB_LAST_VERSION = "6.3.3";

      // Every release from 6.3.5 is to be signed by the release-sign job of this
      // project's GitLab CI pipeline for a v* tag (6.3.4 was released by hand with
      // no bundle). None has been yet: this entry is compiled in from Fulcio's
      // documented GitLab form, and the first signed release is what proves it
      // (docs/UpdateFeed.md, "What the signing job produces").
      // Fulcio names a GitLab CI job by the ci_config_ref_uri claim of its ID token,
      // "https://" + gitlab.com/<project path>//<CI file>@<ref>, records
      // https://gitlab.com as the issuer, https://gitlab.com/<project path> as the
      // source repository and the project's id as the source repository
      // identifier. The prefix is anchored three ways: the double slash ends the
      // project path, so no subgroup or project whose path merely begins with this
      // one matches; the CI file is the project's own .gitlab-ci.yml; and the ref
      // is a tag beginning with v, which only a maintainer can push (v* is a
      // protected tag), so a branch, a merge request's ref or a fork's pipeline
      // presents another identity and is refused. The project id, 86730559, holds
      // it to this project and not to whichever takes the path after a rename or a
      // transfer of the namespace. The id is checked when the certificate carries
      // one: no GitLab-issued certificate has been seen here yet, and a server that
      // refused every one for lacking an extension Fulcio turned out not to write
      // could install nothing; the path, the issuer and the repository are
      // required either way.
      const char *GITLAB_IDENTITY_PREFIX = "https://gitlab.com/Progressiverobot/hmailserver//.gitlab-ci.yml@refs/tags/v";
      const char *GITLAB_ISSUER = "https://gitlab.com";
      const char *GITLAB_REPOSITORY = "https://gitlab.com/Progressiverobot/hmailserver";
      const char *GITLAB_REPOSITORY_ID = "86730559";

      // And the same pipeline after the project moves. The project is to leave the
      // personal namespace Progressiverobot for the group hmailserver
      // (https://gitlab.com/hmailserver), which the owner reserved for it on 26
      // September 2026. A transfer keeps the project's id and changes its path, and
      // with the path the identity and the source repository Fulcio writes, so no
      // certificate from the pipeline after the move names the entry above. A
      // server trusts only the signers compiled into it: one that knew only the
      // entry above would refuse every release signed after the move, and nothing
      // but an upgrade by hand would get it past. This entry is here so that every
      // server from 6.3.4 on takes those releases. Nothing has been signed from
      // this path, and nothing will be until the move. The prefix is anchored as
      // the entry above's is, and the issuer is the same.
      //
      // Here the id is required, where the entry above checks it only when the
      // certificate carries one. That entry's path is the project's own today, in
      // the owner's own namespace, where nobody else can make a project; its id
      // guards against whoever holds the path after the project has left it, and
      // it is optional because a server that refused every certificate for lacking
      // an extension Fulcio turned out not to write could install nothing. This
      // path is not the project's until the move. The group can be lost, or the
      // project moved on again, and whoever holds the path then can run a pipeline
      // for a v* tag of their own there and be named exactly as this entry names
      // it. Only the id tells them apart: a transfer keeps 86730559, and GitLab
      // gives no other project that id, so a certificate for this path must record
      // it, and one that records none is nobody's. Whoever registers the path, now
      // or ever, cannot sign as this project from a project of their own. The cost
      // falls on the project, not on a server: Fulcio's GitLab mapping writes the
      // project's id as the source repository identifier, but no GitLab-issued
      // certificate has been seen here yet, and if Fulcio wrote none, nothing
      // signed from the group would install. The first release signed from the
      // personal namespace shows which (release-verify puts the id it found in the
      // release's notes), and the project is not to move before one has shown it
      // (docs/UpdateFeed.md, "What a server built after 6.3.3 checks").
      const char *GITLAB_GROUP_IDENTITY_PREFIX = "https://gitlab.com/hmailserver/hmailserver//.gitlab-ci.yml@refs/tags/v";
      const char *GITLAB_GROUP_REPOSITORY = "https://gitlab.com/hmailserver/hmailserver";

      // Fulcio's certificate extensions (docs.sigstore.dev/certificate_authority/oid-info/).
      const char *OID_ISSUER_V1 = "1.3.6.1.4.1.57264.1.1";          // raw string
      const char *OID_ISSUER_V2 = "1.3.6.1.4.1.57264.1.8";          // DER UTF8String
      const char *OID_SOURCE_REPOSITORY_URI = "1.3.6.1.4.1.57264.1.12"; // DER UTF8String
      const char *OID_SOURCE_REPOSITORY_IDENTIFIER = "1.3.6.1.4.1.57264.1.15"; // DER UTF8String

      struct X509Free { void operator()(X509 *x) const { X509_free(x); } };
      struct PkeyFree { void operator()(EVP_PKEY *k) const { EVP_PKEY_free(k); } };
      struct BioFree { void operator()(BIO *b) const { BIO_free(b); } };
      struct StoreFree { void operator()(X509_STORE *s) const { X509_STORE_free(s); } };
      struct StoreCtxFree { void operator()(X509_STORE_CTX *c) const { X509_STORE_CTX_free(c); } };
      struct PkeyCtxFree { void operator()(EVP_PKEY_CTX *c) const { EVP_PKEY_CTX_free(c); } };
      struct MdCtxFree { void operator()(EVP_MD_CTX *c) const { EVP_MD_CTX_free(c); } };

      typedef std::unique_ptr<X509, X509Free> X509Ptr;
      typedef std::unique_ptr<EVP_PKEY, PkeyFree> PkeyPtr;
      typedef std::unique_ptr<BIO, BioFree> BioPtr;

      typedef std::vector<unsigned char> Bytes;

      std::string Utf8Of_(const String &text)
      {
         AnsiString narrow;
         Unicode::WideToMultiByte(text, narrow);
         return std::string(narrow.c_str());
      }

      // Protobuf's JSON form writes a 64-bit integer as a string; an older bundle
      // may carry a number. Either is the number.
      bool Int64Of_(const JsonValue *value, __int64 &out)
      {
         if (!value)
            return false;
         if (value->IsNumber())
         {
            out = value->AsInt64();
            return true;
         }
         if (value->IsString())
         {
            const std::string &text = value->AsString();
            if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
               return false;
#ifdef HM_PLATFORM_POSIX
            // strtoll is the standard C name for _strtoi64 - the same conversion,
            // the same arguments, the same 64 bits.
            out = ::strtoll(text.c_str(), nullptr, 10);
#else
            out = _strtoi64(text.c_str(), nullptr, 10);
#endif
            return true;
         }
         return false;
      }

      std::string StringOf_(const JsonValue *value)
      {
         return value && value->IsString() ? value->AsString() : std::string();
      }

      bool Base64Of_(const JsonValue *value, Bytes &out)
      {
         return value && value->IsString() && SigstoreVerifier::Base64Decode(value->AsString(), out);
      }

      Bytes Sha256_(const unsigned char *data, size_t length)
      {
         Bytes digest(SHA256_DIGEST_LENGTH);
         SHA256(data, length, digest.data());
         return digest;
      }

      Bytes Sha256_(const Bytes &data)
      {
         return Sha256_(data.empty() ? nullptr : data.data(), data.size());
      }

      bool ParseCertificatesPem_(const std::string &pem, std::vector<X509Ptr> &out)
      {
         BioPtr bio(BIO_new_mem_buf(pem.data(), (int) pem.size()));
         if (!bio)
            return false;

         while (true)
         {
            X509 *certificate = PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr);
            if (!certificate)
               break;
            out.push_back(X509Ptr(certificate));
         }
         ERR_clear_error();
         return !out.empty();
      }

      PkeyPtr ParsePublicKeyPem_(const std::string &pem)
      {
         BioPtr bio(BIO_new_mem_buf(pem.data(), (int) pem.size()));
         if (!bio)
            return PkeyPtr();
         PkeyPtr key(PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr));
         ERR_clear_error();
         return key;
      }

      bool PublicKeyDer_(EVP_PKEY *key, Bytes &out)
      {
         int length = i2d_PUBKEY(key, nullptr);
         if (length <= 0)
            return false;
         out.resize((size_t) length);
         unsigned char *cursor = out.data();
         return i2d_PUBKEY(key, &cursor) == length;
      }

      bool CertificateDer_(X509 *certificate, Bytes &out)
      {
         int length = i2d_X509(certificate, nullptr);
         if (length <= 0)
            return false;
         out.resize((size_t) length);
         unsigned char *cursor = out.data();
         return i2d_X509(certificate, &cursor) == length;
      }

      // ECDSA (or any EVP key) with SHA-256 over a message, DER signature.
      bool VerifyMessage_(EVP_PKEY *key, const unsigned char *message, size_t length, const Bytes &signature)
      {
         std::unique_ptr<EVP_MD_CTX, MdCtxFree> context(EVP_MD_CTX_new());
         if (!context)
            return false;
         if (EVP_DigestVerifyInit(context.get(), nullptr, EVP_sha256(), nullptr, key) != 1)
            return false;
         return EVP_DigestVerify(context.get(), signature.data(), signature.size(), message, length) == 1;
      }

      // The same, over a digest already computed: what cosign signs is the SHA-256
      // of the file, and the file is not read again to check it.
      bool VerifyDigest_(EVP_PKEY *key, const Bytes &digest, const Bytes &signature)
      {
         std::unique_ptr<EVP_PKEY_CTX, PkeyCtxFree> context(EVP_PKEY_CTX_new(key, nullptr));
         if (!context)
            return false;
         if (EVP_PKEY_verify_init(context.get()) != 1)
            return false;
         if (EVP_PKEY_CTX_set_signature_md(context.get(), EVP_sha256()) != 1)
            return false;
         return EVP_PKEY_verify(context.get(), signature.data(), signature.size(), digest.data(), digest.size()) == 1;
      }

      // The contents of an extension's OCTET STRING, by OID.
      bool ExtensionValue_(X509 *certificate, const char *oid, std::string &out)
      {
         ASN1_OBJECT *object = OBJ_txt2obj(oid, 1);
         if (!object)
            return false;
         int index = X509_get_ext_by_OBJ(certificate, object, -1);
         ASN1_OBJECT_free(object);
         if (index < 0)
            return false;
         // Const on the way in, and cast away only at the call that will not take
         // it. The two OpenSSLs disagree about this pair: 4.0.2, which the Windows
         // build links, returns a const X509_EXTENSION * here and takes one in
         // X509_EXTENSION_get_data, while 3.5 returns and takes a mutable one.
         // Declaring the variable const is accepted by both - adding const to a
         // mutable pointer is always allowed - and the const_cast below is what
         // the older accessor needs; a const parameter takes the mutable pointer
         // just as happily. Nothing here writes through it either way.
         const X509_EXTENSION *extension = X509_get_ext(certificate, index);
         if (!extension)
            return false;
         const ASN1_OCTET_STRING *data = X509_EXTENSION_get_data(const_cast<X509_EXTENSION *>(extension));
         if (!data)
            return false;
         out.assign((const char *) ASN1_STRING_get0_data(data), (size_t) ASN1_STRING_length(data));
         return true;
      }

      // A DER UTF8String (tag 0x0C, a length, the text) as its text.
      bool DerUtf8String_(const std::string &der, std::string &out)
      {
         if (der.size() < 2 || (unsigned char) der[0] != 0x0C)
            return false;
         size_t position = 1;
         size_t length = (unsigned char) der[position++];
         if (length & 0x80)
         {
            size_t lengthBytes = length & 0x7F;
            if (lengthBytes == 0 || lengthBytes > 4 || position + lengthBytes > der.size())
               return false;
            length = 0;
            for (size_t i = 0; i < lengthBytes; i++)
               length = (length << 8) | (unsigned char) der[position++];
         }
         if (position + length != der.size())
            return false;
         out = der.substr(position, length);
         return true;
      }

      void SanUris_(X509 *certificate, std::vector<std::string> &uris)
      {
         GENERAL_NAMES *names = (GENERAL_NAMES *) X509_get_ext_d2i(certificate, NID_subject_alt_name, nullptr, nullptr);
         if (!names)
            return;
         for (int i = 0; i < sk_GENERAL_NAME_num(names); i++)
         {
            GENERAL_NAME *name = sk_GENERAL_NAME_value(names, i);
            if (name && name->type == GEN_URI)
               uris.push_back(std::string((const char *) ASN1_STRING_get0_data(name->d.uniformResourceIdentifier),
                                          (size_t) ASN1_STRING_length(name->d.uniformResourceIdentifier)));
         }
         GENERAL_NAMES_free(names);
      }

      // RFC 9162 section 2.1.3.2: the leaf's hash, the path and the tree size
      // reproduce the root, or the entry is not in that tree.
      bool VerifyInclusion_(const Bytes &leafHash, __int64 leafIndex, __int64 treeSize, const std::vector<Bytes> &path, const Bytes &root)
      {
         if (leafIndex < 0 || treeSize <= 0 || leafIndex >= treeSize)
            return false;

         unsigned __int64 fn = (unsigned __int64) leafIndex;
         unsigned __int64 sn = (unsigned __int64) treeSize - 1;
         Bytes r = leafHash;

         for (size_t i = 0; i < path.size(); i++)
         {
            if (sn == 0)
               return false;

            const Bytes &p = path[i];
            Bytes node;
            node.reserve(1 + p.size() + r.size());
            node.push_back(0x01);
            if ((fn & 1) == 1 || fn == sn)
            {
               node.insert(node.end(), p.begin(), p.end());
               node.insert(node.end(), r.begin(), r.end());
               r = Sha256_(node);
               if ((fn & 1) == 0)
               {
                  while (fn != 0 && (fn & 1) == 0)
                  {
                     fn >>= 1;
                     sn >>= 1;
                  }
               }
            }
            else
            {
               node.insert(node.end(), r.begin(), r.end());
               node.insert(node.end(), p.begin(), p.end());
               r = Sha256_(node);
            }
            fn >>= 1;
            sn >>= 1;
         }

         return sn == 0 && r == root;
      }

      // A signed note (golang.org/x/mod/sumdb/note, as Rekor writes checkpoints):
      // the body - origin, tree size, root hash, each on its own line - then an
      // empty line, then one "— name base64(keyhint||signature)" line per signer.
      struct Checkpoint
      {
         std::string body;
         std::string origin;
         __int64 size;
         Bytes root;
         std::vector<std::pair<std::string, Bytes> > signatures;   // name, hint||signature
      };

      bool ParseCheckpoint_(const std::string &note, Checkpoint &checkpoint)
      {
         size_t separator = note.find("\n\n");
         if (separator == std::string::npos)
            return false;

         checkpoint.body = note.substr(0, separator + 1);

         std::vector<std::string> lines;
         size_t start = 0;
         while (start < checkpoint.body.size())
         {
            size_t end = checkpoint.body.find('\n', start);
            if (end == std::string::npos)
               end = checkpoint.body.size();
            lines.push_back(checkpoint.body.substr(start, end - start));
            start = end + 1;
         }
         if (lines.size() < 3)
            return false;

         checkpoint.origin = lines[0];
         if (lines[1].empty() || lines[1].find_first_not_of("0123456789") != std::string::npos)
            return false;
#ifdef HM_PLATFORM_POSIX
         // See above: strtoll is _strtoi64 under its standard name.
         checkpoint.size = ::strtoll(lines[1].c_str(), nullptr, 10);
#else
         checkpoint.size = _strtoi64(lines[1].c_str(), nullptr, 10);
#endif
         if (!SigstoreVerifier::Base64Decode(lines[2], checkpoint.root))
            return false;

         const std::string dash = "\xE2\x80\x94 ";
         start = separator + 2;
         while (start < note.size())
         {
            size_t end = note.find('\n', start);
            if (end == std::string::npos)
               end = note.size();
            std::string line = note.substr(start, end - start);
            start = end + 1;
            if (line.compare(0, dash.size(), dash) != 0)
               continue;
            line = line.substr(dash.size());
            size_t space = line.find(' ');
            if (space == std::string::npos)
               continue;
            Bytes blob;
            if (!SigstoreVerifier::Base64Decode(line.substr(space + 1), blob) || blob.size() <= 4)
               continue;
            checkpoint.signatures.push_back(std::make_pair(line.substr(0, space), blob));
         }

         return !checkpoint.signatures.empty();
      }

      bool Fail_(SigstoreVerdict &verdict, const String &why)
      {
         verdict.verified = false;
         verdict.error = why;
         return false;
      }

      SigstoreSigner MakeSigner_(const char *identityPrefix, const char *issuer, const char *repository, const char *repositoryId,
                                 bool repositoryIdOptional, __int64 issuedBefore, const char *lastVersion)
      {
         SigstoreSigner signer;
         signer.identity_prefix = identityPrefix;
         signer.issuer = issuer;
         signer.repository = repository;
         signer.repository_id = repositoryId;
         signer.repository_id_optional = repositoryIdOptional;
         signer.issued_before = issuedBefore;
         signer.last_version = lastVersion;
         signer.version_in_ref = true;
         return signer;
      }

      // The same identity, issuer and repository: what picks a signer and what it
      // must record, whatever else it checks.
      bool SameIdentity_(const SigstoreSigner &left, const SigstoreSigner &right)
      {
         return std::string(left.identity_prefix.c_str()) == std::string(right.identity_prefix.c_str()) &&
                std::string(left.issuer.c_str()) == std::string(right.issuer.c_str()) &&
                std::string(left.repository.c_str()) == std::string(right.repository.c_str());
      }

      bool SameSigner_(const SigstoreSigner &left, const SigstoreSigner &right)
      {
         return SameIdentity_(left, right) &&
                std::string(left.repository_id.c_str()) == std::string(right.repository_id.c_str()) &&
                left.repository_id_optional == right.repository_id_optional &&
                left.issued_before == right.issued_before &&
                std::string(left.last_version.c_str()) == std::string(right.last_version.c_str()) &&
                left.version_in_ref == right.version_in_ref;
      }

      // The tag a workflow identity names after its first "@" - "v6.3.4" from
      // ".../.gitlab-ci.yml@refs/tags/v6.3.4" - or empty when the ref there is not
      // a tag (a GitHub workflow run from a branch). The first "@", because the
      // path before it has none and a tag may.
      std::string TaggedRef_(const std::string &identity)
      {
         const std::string tags = "refs/tags/";
         const size_t at = identity.find('@');
         if (at == std::string::npos)
            return std::string();
         const std::string ref = identity.substr(at + 1);
         if (ref.compare(0, tags.size(), tags) != 0)
            return std::string();
         return ref.substr(tags.size());
      }

      // Unix seconds as ISO 8601 UTC, by arithmetic alone (the days-to-civil
      // conversion of Howard Hinnant's date algorithms), so that nothing here
      // depends on a platform's gmtime.
      std::string UtcText_(__int64 seconds)
      {
         __int64 days = seconds / 86400;
         __int64 rest = seconds % 86400;
         if (rest < 0)
         {
            rest += 86400;
            days -= 1;
         }
         days += 719468;
         const __int64 era = (days >= 0 ? days : days - 146096) / 146097;
         const __int64 dayOfEra = days - era * 146097;
         const __int64 yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
         const __int64 dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
         const __int64 monthIndex = (5 * dayOfYear + 2) / 153;
         const __int64 day = dayOfYear - (153 * monthIndex + 2) / 5 + 1;
         const __int64 month = monthIndex < 10 ? monthIndex + 3 : monthIndex - 9;
         const __int64 year = yearOfEra + era * 400 + (month <= 2 ? 1 : 0);

         char text[40];
         sprintf_s(text, sizeof(text), "%04d-%02d-%02dT%02d:%02d:%02dZ", (int) year, (int) month, (int) day,
            (int) (rest / 3600), (int) (rest % 3600 / 60), (int) (rest % 60));
         return std::string(text);
      }
   }

   SigstoreTrust
   SigstoreVerifier::DefaultTrust()
   {
      SigstoreTrust trust;
      trust.ca_pems.push_back(FULCIO_ROOT_PEM);
      trust.ca_pems.push_back(FULCIO_INTERMEDIATE_PEM);
      trust.log_key_pem = REKOR_PUBLIC_KEY_PEM;
      trust.signers.push_back(MakeSigner_(GITHUB_IDENTITY_PREFIX, GITHUB_ISSUER, GITHUB_REPOSITORY, GITHUB_REPOSITORY_ID, false,
                                          GITHUB_ISSUED_BEFORE, GITHUB_LAST_VERSION));
      trust.signers.push_back(MakeSigner_(GITLAB_IDENTITY_PREFIX, GITLAB_ISSUER, GITLAB_REPOSITORY, GITLAB_REPOSITORY_ID, true, 0, ""));
      trust.signers.push_back(MakeSigner_(GITLAB_GROUP_IDENTITY_PREFIX, GITLAB_ISSUER, GITLAB_GROUP_REPOSITORY, GITLAB_REPOSITORY_ID, false, 0, ""));
      return trust;
   }

   bool
   SigstoreVerifier::ConfiguredTrust(SigstoreTrust &trust, String &error)
   {
      trust = DefaultTrust();
      IniFileSettings *settings = IniFileSettings::Instance();

      String rootsFile = settings->GetUpdateTrustRootsFile();
      if (!rootsFile.IsEmpty())
      {
         if (!FileUtilities::Exists(rootsFile))
         {
            error = Formatter::Format(_T("UpdateTrustRootsFile {0} does not exist."), rootsFile);
            return false;
         }
         std::string pem = Utf8Of_(FileUtilities::ReadCompleteTextFile(rootsFile));
         std::vector<X509Ptr> certificates;
         if (!ParseCertificatesPem_(pem, certificates))
         {
            error = Formatter::Format(_T("UpdateTrustRootsFile {0} carries no PEM certificate."), rootsFile);
            return false;
         }
         trust.ca_pems.clear();
         trust.ca_pems.push_back(pem);
      }

      String keyFile = settings->GetUpdateLogPublicKeyFile();
      if (!keyFile.IsEmpty())
      {
         if (!FileUtilities::Exists(keyFile))
         {
            error = Formatter::Format(_T("UpdateLogPublicKeyFile {0} does not exist."), keyFile);
            return false;
         }
         std::string pem = Utf8Of_(FileUtilities::ReadCompleteTextFile(keyFile));
         if (!ParsePublicKeyPem_(pem))
         {
            error = Formatter::Format(_T("UpdateLogPublicKeyFile {0} is not a PEM public key."), keyFile);
            return false;
         }
         trust.log_key_pem = pem;
      }

      // Each of the three replaces its part of every signer in the list: a site
      // that signs its own releases sets all three and is left with one signer, its
      // own; one that sets only the repository check off keeps every one of this
      // project's signers without it. The repository check can be switched off
      // with "-"; an empty setting keeps the default, so that the default is what a
      // blank INI gets. What belongs to this project's own signers goes with the
      // part it belongs to: the repository's id with the repository; the GitHub
      // cut-off and the last release it signed with the identity and the issuer
      // they were set for; and the tag the identity must name with the identity,
      // since a site's workflow names its refs its own way.
      //
      // A setting that restates a signer's own value changes nothing about that
      // signer, and keeps everything that goes with it: restating this project's
      // values must not weaken what they pin. And where a setting makes one signer
      // name what another names unchanged - the three GitLab values
      // docs/UpdateFeed.md gives a server from 6.2.28 to 6.3.3, still set after
      // it is upgraded, turn the GitHub entry into GitLab's identity, issuer and
      // repository without GitLab's checks - the stronger is kept and the weaker
      // dropped, since signers are tried in order and a copy first in the list
      // would take a certificate the real one refuses. Stronger: the one whose
      // identity was not replaced (it keeps its release check and its limits),
      // then the one nothing was replaced in; between equals, the first. The same
      // three values turn the group's entry into the present path's without the
      // id, and it goes the same way: a server that keeps them set trusts the
      // project's present path alone, and refuses what is signed after the move
      // until they are cleared, which docs/UpdateFeed.md tells it to do.
      String identity = settings->GetUpdateSigningIdentity();
      String issuer = settings->GetUpdateSigningIssuer();
      String repository = settings->GetUpdateSourceRepository();
      const std::string identityText = Utf8Of_(identity);
      const std::string issuerText = Utf8Of_(issuer);
      const std::string repositoryText = Utf8Of_(repository);

      std::vector<SigstoreSigner> signers;
      std::vector<int> weakness;   // 2 for a replaced identity, 1 for any other part replaced
      for (size_t i = 0; i < trust.signers.size(); i++)
      {
         SigstoreSigner signer = trust.signers[i];
         bool replaced = false;
         bool identityReplaced = false;
         if (!identityText.empty() && identityText != std::string(signer.identity_prefix.c_str()))
         {
            signer.identity_prefix = identityText.c_str();
            signer.version_in_ref = false;
            signer.issued_before = 0;
            signer.last_version = "";
            replaced = true;
            identityReplaced = true;
         }
         if (!issuerText.empty() && issuerText != std::string(signer.issuer.c_str()))
         {
            signer.issuer = issuerText.c_str();
            signer.issued_before = 0;
            signer.last_version = "";
            replaced = true;
         }
         if (repositoryText == "-")
         {
            if (!signer.repository.IsEmpty() || !signer.repository_id.IsEmpty())
            {
               signer.repository = "";
               signer.repository_id = "";
               signer.repository_id_optional = false;
               replaced = true;
            }
         }
         else if (!repositoryText.empty() && repositoryText != std::string(signer.repository.c_str()))
         {
            signer.repository = repositoryText.c_str();
            signer.repository_id = "";
            signer.repository_id_optional = false;
            replaced = true;
         }

         const int signerWeakness = (identityReplaced ? 2 : 0) + (replaced ? 1 : 0);
         bool known = false;
         for (size_t j = 0; j < signers.size() && !known; j++)
         {
            if (SameSigner_(signers[j], signer))
               known = true;
            else if (SameIdentity_(signers[j], signer))
            {
               known = true;
               if (signerWeakness < weakness[j])
               {
                  signers[j] = signer;
                  weakness[j] = signerWeakness;
               }
            }
         }
         if (!known)
         {
            signers.push_back(signer);
            weakness.push_back(signerWeakness);
         }
      }
      trust.signers = signers;

      return true;
   }

   bool
   SigstoreVerifier::ReadBundle(const String &path, AnsiString &bundleJson)
   {
      bundleJson.Empty();
#ifdef HM_PLATFORM_POSIX
      const AnsiString narrow_path = path;
      std::ifstream file(narrow_path.c_str(), std::ios::binary);
#else
      std::ifstream file(path.c_str(), std::ios::binary);
#endif
      if (!file)
         return false;

      // A bundle is eleven kilobytes; UpdateDownloader refuses to fetch one over
      // a megabyte, and a file past that here is not a bundle either.
      std::vector<char> bytes;
      std::vector<char> buffer(64 * 1024);
      while (file)
      {
         file.read(buffer.data(), (std::streamsize) buffer.size());
         std::streamsize got = file.gcount();
         if (got > 0)
            bytes.insert(bytes.end(), buffer.data(), buffer.data() + got);
         if (bytes.size() > 4 * 1024 * 1024)
            return false;
      }

      bundleJson = AnsiString(bytes.data(), (int) bytes.size());
      return true;
   }

   bool
   SigstoreVerifier::Sha256File(const String &path, std::vector<unsigned char> &digest, String &error)
   {
#ifdef HM_PLATFORM_POSIX
      // std::ifstream's constructor from a WIDE C string is a Microsoft extension;
      // the standard one takes a narrow path, which is also what this filesystem's
      // names are. Narrowed the way the rest of the tree narrows a String for a C
      // API - and a path that could not be represented that way could not be
      // opened by any other call here either, which is the failure the test just
      // below already reports by name.
      const AnsiString narrow_path = path;
      std::ifstream file(narrow_path.c_str(), std::ios::binary);
#else
      std::ifstream file(path.c_str(), std::ios::binary);
#endif
      if (!file)
      {
         error = Formatter::Format(_T("{0} could not be opened."), path);
         return false;
      }

      std::unique_ptr<EVP_MD_CTX, MdCtxFree> context(EVP_MD_CTX_new());
      if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1)
      {
         error = _T("SHA-256 could not be initialised.");
         return false;
      }

      std::vector<char> buffer(1024 * 1024);
      while (file)
      {
         file.read(buffer.data(), (std::streamsize) buffer.size());
         std::streamsize got = file.gcount();
         if (got > 0 && EVP_DigestUpdate(context.get(), buffer.data(), (size_t) got) != 1)
         {
            error = _T("SHA-256 could not be updated.");
            return false;
         }
      }

      digest.resize(EVP_MAX_MD_SIZE);
      unsigned int length = 0;
      if (EVP_DigestFinal_ex(context.get(), digest.data(), &length) != 1)
      {
         error = _T("SHA-256 could not be finished.");
         return false;
      }
      digest.resize(length);
      return true;
   }

   bool
   SigstoreVerifier::Base64Decode(const std::string &text, std::vector<unsigned char> &out)
   {
      static const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
      out.clear();

      unsigned accumulator = 0;
      int bits = 0;
      size_t padding = 0;
      for (size_t i = 0; i < text.size(); i++)
      {
         char c = text[i];
         if (c == '\r' || c == '\n' || c == ' ' || c == '\t')
            continue;
         if (c == '=')
         {
            padding++;
            continue;
         }
         if (padding > 0)
            return false;

         const char *where = strchr(alphabet, c);
         if (!where || c == '\0')
            return false;

         accumulator = (accumulator << 6) | (unsigned) (where - alphabet);
         bits += 6;
         if (bits >= 8)
         {
            bits -= 8;
            out.push_back((unsigned char) ((accumulator >> bits) & 0xFF));
         }
      }

      return padding <= 2;
   }

   std::string
   SigstoreVerifier::Hex(const std::vector<unsigned char> &bytes)
   {
      static const char *digits = "0123456789abcdef";
      std::string result;
      result.reserve(bytes.size() * 2);
      for (size_t i = 0; i < bytes.size(); i++)
      {
         result += digits[bytes[i] >> 4];
         result += digits[bytes[i] & 0x0F];
      }
      return result;
   }

   bool
   SigstoreVerifier::VerifyFile(const String &path, const AnsiString &bundleJson, const SigstoreTrust &trust, SigstoreVerdict &verdict,
                                const String &expectedVersion)
   {
      // No release to hold the file to is no reason to let it through as one: the
      // GitHub signer's last release and the tag a GitLab certificate names would
      // both go unchecked. A caller with no release says so, with
      // VerifyFileForAnyRelease.
      if (expectedVersion.IsEmpty())
      {
         verdict = SigstoreVerdict();
         return Fail_(verdict, _T("There is no release to hold the file to, so nothing shows it is the one to install."));
      }
      return Verify_(path, bundleJson, trust, verdict, expectedVersion);
   }

   bool
   SigstoreVerifier::VerifyFileForAnyRelease(const String &path, const AnsiString &bundleJson, const SigstoreTrust &trust, SigstoreVerdict &verdict)
   {
      return Verify_(path, bundleJson, trust, verdict, String());
   }

   bool
   SigstoreVerifier::Verify_(const String &path, const AnsiString &bundleJson, const SigstoreTrust &trust, SigstoreVerdict &verdict,
                             const String &expectedVersion)
   {
      verdict = SigstoreVerdict();

      // The trust first: a verifier with nothing to trust cannot say yes, and it
      // should say why before it reads a byte of the bundle.
      std::vector<X509Ptr> authorities;
      for (size_t i = 0; i < trust.ca_pems.size(); i++)
         ParseCertificatesPem_(trust.ca_pems[i], authorities);
      if (authorities.empty())
         return Fail_(verdict, _T("No certificate authority is trusted; nothing can verify."));

      PkeyPtr logKey = ParsePublicKeyPem_(trust.log_key_pem);
      if (!logKey)
         return Fail_(verdict, _T("The transparency log's public key could not be read; nothing can verify."));
      Bytes logKeyDer;
      if (!PublicKeyDer_(logKey.get(), logKeyDer))
         return Fail_(verdict, _T("The transparency log's public key could not be encoded."));
      Bytes logId = Sha256_(logKeyDer);

      // 1. The bundle, and the digest it is about.
      JsonValue bundle;
      std::string parseError;
      if (!JsonValue::Parse(std::string(bundleJson.c_str()), bundle, parseError))
         return Fail_(verdict, Formatter::Format(_T("The bundle is not JSON: {0}."), String(parseError.c_str())));

      std::string mediaType = bundle.GetString("mediaType");
      const std::string bundleType = "application/vnd.dev.sigstore.bundle";
      if (mediaType.compare(0, bundleType.size(), bundleType) != 0)
         return Fail_(verdict, Formatter::Format(_T("The bundle's media type is {0}, not a Sigstore bundle."), String(mediaType.c_str())));

      const JsonValue *material = bundle.Get("verificationMaterial");
      const JsonValue *message = bundle.Get("messageSignature");
      if (!material || !material->IsObject())
         return Fail_(verdict, _T("The bundle carries no verification material."));
      if (!message || !message->IsObject())
         return Fail_(verdict, _T("The bundle carries no message signature; an installer is signed as a blob, not as an attestation."));

      const JsonValue *messageDigest = message->Get("messageDigest");
      if (!messageDigest || messageDigest->GetString("algorithm") != "SHA2_256")
         return Fail_(verdict, _T("The bundle's digest is not SHA-256."));
      Bytes digest;
      if (!Base64Of_(messageDigest->Get("digest"), digest) || digest.size() != SHA256_DIGEST_LENGTH)
         return Fail_(verdict, _T("The bundle's digest is malformed."));
      Bytes signature;
      if (!Base64Of_(message->Get("signature"), signature) || signature.empty())
         return Fail_(verdict, _T("The bundle's signature is malformed."));

      Bytes fileDigest;
      String hashError;
      if (!Sha256File(path, fileDigest, hashError))
         return Fail_(verdict, hashError);
      if (fileDigest != digest)
         return Fail_(verdict, Formatter::Format(_T("The file's SHA-256 ({0}) is not the one the bundle signs ({1}): this is not the file that was signed."),
            String(Hex(fileDigest).c_str()), String(Hex(digest).c_str())));

      // 2. The certificate: the leaf, and any intermediates an older bundle carries.
      Bytes leafDer;
      std::vector<X509Ptr> untrusted;
      {
         const JsonValue *certificate = material->Get("certificate");
         const JsonValue *chain = material->Get("x509CertificateChain");
         if (certificate && certificate->IsObject())
         {
            if (!Base64Of_(certificate->Get("rawBytes"), leafDer))
               return Fail_(verdict, _T("The bundle's certificate is malformed."));
         }
         else if (chain && chain->IsObject())
         {
            const JsonValue *certificates = chain->Get("certificates");
            if (!certificates || !certificates->IsArray() || certificates->Size() == 0)
               return Fail_(verdict, _T("The bundle's certificate chain is empty."));
            for (size_t i = 0; i < certificates->Size(); i++)
            {
               Bytes der;
               if (!Base64Of_(certificates->At(i)->Get("rawBytes"), der))
                  return Fail_(verdict, _T("The bundle's certificate chain is malformed."));
               if (i == 0)
                  leafDer = der;
               else
               {
                  const unsigned char *cursor = der.data();
                  X509 *parsed = d2i_X509(nullptr, &cursor, (long) der.size());
                  if (parsed)
                     untrusted.push_back(X509Ptr(parsed));
               }
            }
         }
         else
            return Fail_(verdict, _T("The bundle carries no certificate."));
      }

      const unsigned char *cursor = leafDer.data();
      X509Ptr leaf(d2i_X509(nullptr, &cursor, (long) leafDer.size()));
      if (!leaf)
         return Fail_(verdict, _T("The bundle's certificate is not a DER X.509 certificate."));

      // 3. The log's record: the same digest, signature and certificate, and the
      // log's word for it - the signed entry timestamp, or the inclusion proof to a
      // checkpoint the log signed. Either is the log speaking; one is required.
      const JsonValue *entries = material->Get("tlogEntries");
      const JsonValue *entry = entries && entries->IsArray() ? entries->At(0) : nullptr;
      if (!entry || !entry->IsObject())
         return Fail_(verdict, _T("The bundle carries no transparency log entry."));

      __int64 logIndex = 0;
      __int64 integratedTime = 0;
      if (!Int64Of_(entry->Get("logIndex"), logIndex) || !Int64Of_(entry->Get("integratedTime"), integratedTime) || integratedTime <= 0)
         return Fail_(verdict, _T("The log entry's index or time is malformed."));

      Bytes entryLogId;
      const JsonValue *entryLog = entry->Get("logId");
      if (!entryLog || !Base64Of_(entryLog->Get("keyId"), entryLogId))
         return Fail_(verdict, _T("The log entry names no log."));
      if (entryLogId != logId)
         return Fail_(verdict, _T("The log entry was recorded by a log this server does not trust."));

      std::string bodyBase64 = StringOf_(entry->Get("canonicalizedBody"));
      Bytes body;
      if (bodyBase64.empty() || !Base64Decode(bodyBase64, body))
         return Fail_(verdict, _T("The log entry's body is malformed."));

      {
         JsonValue record;
         std::string recordError;
         if (!JsonValue::Parse(std::string(body.begin(), body.end()), record, recordError))
            return Fail_(verdict, _T("The log entry's body is not JSON."));
         if (record.GetString("kind") != "hashedrekord")
            return Fail_(verdict, Formatter::Format(_T("The log entry is a {0}, not a hashedrekord."), String(record.GetString("kind").c_str())));
         const JsonValue *spec = record.Get("spec");
         const JsonValue *data = spec ? spec->Get("data") : nullptr;
         const JsonValue *hash = data ? data->Get("hash") : nullptr;
         const JsonValue *recordSignature = spec ? spec->Get("signature") : nullptr;
         const JsonValue *publicKey = recordSignature ? recordSignature->Get("publicKey") : nullptr;
         if (!hash || hash->GetString("algorithm") != "sha256" || hash->GetString("value") != Hex(digest))
            return Fail_(verdict, _T("The log recorded a different digest from the one the bundle signs."));
         Bytes recordedSignature;
         if (!recordSignature || !Base64Of_(recordSignature->Get("content"), recordedSignature) || recordedSignature != signature)
            return Fail_(verdict, _T("The log recorded a different signature from the one the bundle carries."));
         Bytes recordedKeyPem;
         std::vector<X509Ptr> recorded;
         Bytes recordedDer;
         if (!publicKey || !Base64Of_(publicKey->Get("content"), recordedKeyPem) ||
             !ParseCertificatesPem_(std::string(recordedKeyPem.begin(), recordedKeyPem.end()), recorded) ||
             !CertificateDer_(recorded[0].get(), recordedDer) || recordedDer != leafDer)
            return Fail_(verdict, _T("The log recorded a different certificate from the one the bundle carries."));
      }

      bool vouched = false;

      const JsonValue *promise = entry->Get("inclusionPromise");
      if (promise && promise->IsObject())
      {
         Bytes timestampSignature;
         if (!Base64Of_(promise->Get("signedEntryTimestamp"), timestampSignature))
            return Fail_(verdict, _T("The log's signed entry timestamp is malformed."));

         // RFC 8785 canonical JSON of the entry, which is what the log signed.
         char numbers[64];
         sprintf_s(numbers, sizeof(numbers), "%I64d", integratedTime);
         std::string canonical = "{\"body\":\"" + bodyBase64 + "\",\"integratedTime\":" + numbers;
         sprintf_s(numbers, sizeof(numbers), "%I64d", logIndex);
         canonical += ",\"logID\":\"" + Hex(logId) + "\",\"logIndex\":" + numbers + "}";

         if (!VerifyMessage_(logKey.get(), (const unsigned char *) canonical.data(), canonical.size(), timestampSignature))
            return Fail_(verdict, _T("The log's signed entry timestamp does not verify under the log's key."));
         vouched = true;
      }

      const JsonValue *proof = entry->Get("inclusionProof");
      if (proof && proof->IsObject())
      {
         __int64 proofIndex = 0;
         __int64 treeSize = 0;
         if (!Int64Of_(proof->Get("logIndex"), proofIndex) || !Int64Of_(proof->Get("treeSize"), treeSize))
            return Fail_(verdict, _T("The inclusion proof's index or tree size is malformed."));
         Bytes root;
         if (!Base64Of_(proof->Get("rootHash"), root) || root.size() != SHA256_DIGEST_LENGTH)
            return Fail_(verdict, _T("The inclusion proof's root hash is malformed."));
         std::vector<Bytes> hashes;
         const JsonValue *hashList = proof->Get("hashes");
         if (hashList && hashList->IsArray())
         {
            for (size_t i = 0; i < hashList->Size(); i++)
            {
               Bytes node;
               if (!Base64Of_(hashList->At(i), node) || node.size() != SHA256_DIGEST_LENGTH)
                  return Fail_(verdict, _T("The inclusion proof's path is malformed."));
               hashes.push_back(node);
            }
         }

         Bytes leafInput;
         leafInput.reserve(1 + body.size());
         leafInput.push_back(0x00);
         leafInput.insert(leafInput.end(), body.begin(), body.end());
         if (!VerifyInclusion_(Sha256_(leafInput), proofIndex, treeSize, hashes, root))
            return Fail_(verdict, _T("The inclusion proof does not lead from the entry to the root it claims."));

         const JsonValue *checkpointValue = proof->Get("checkpoint");
         Checkpoint checkpoint;
         if (!checkpointValue || !ParseCheckpoint_(checkpointValue->GetString("envelope"), checkpoint))
            return Fail_(verdict, _T("The inclusion proof carries no signed checkpoint."));
         if (checkpoint.size != treeSize || checkpoint.root != root)
            return Fail_(verdict, _T("The checkpoint describes a different tree from the inclusion proof."));

         bool checkpointSigned = false;
         for (size_t i = 0; i < checkpoint.signatures.size() && !checkpointSigned; i++)
         {
            const Bytes &blob = checkpoint.signatures[i].second;
            if (!std::equal(blob.begin(), blob.begin() + 4, logId.begin()))
               continue;
            Bytes checkpointSignature(blob.begin() + 4, blob.end());
            checkpointSigned = VerifyMessage_(logKey.get(), (const unsigned char *) checkpoint.body.data(), checkpoint.body.size(), checkpointSignature);
         }
         if (!checkpointSigned)
            return Fail_(verdict, _T("The checkpoint is not signed by the log's key."));
         vouched = true;
      }

      if (!vouched)
         return Fail_(verdict, _T("The log entry carries neither a signed entry timestamp nor an inclusion proof, so the log does not vouch for it."));

      // 4. The certificate was valid when the log recorded the signature, chains
      // to the authority, and is for code signing.
      time_t when = (time_t) integratedTime;
      int notBefore = ASN1_TIME_cmp_time_t(X509_get0_notBefore(leaf.get()), when);
      if (notBefore == -2 || notBefore > 0)
         return Fail_(verdict, _T("The log recorded the signature before the certificate became valid."));
      int notAfter = ASN1_TIME_cmp_time_t(X509_get0_notAfter(leaf.get()), when);
      if (notAfter == -2 || notAfter < 0)
         return Fail_(verdict, _T("The log recorded the signature after the certificate had expired."));

      {
         std::unique_ptr<X509_STORE, StoreFree> store(X509_STORE_new());
         std::unique_ptr<X509_STORE_CTX, StoreCtxFree> context(X509_STORE_CTX_new());
         if (!store || !context)
            return Fail_(verdict, _T("The certificate store could not be created."));
         for (size_t i = 0; i < authorities.size(); i++)
            X509_STORE_add_cert(store.get(), authorities[i].get());

         STACK_OF(X509) *extra = sk_X509_new_null();
         for (size_t i = 0; i < untrusted.size(); i++)
            sk_X509_push(extra, untrusted[i].get());

         bool chained = false;
         String reason;
         if (X509_STORE_CTX_init(context.get(), store.get(), leaf.get(), extra) == 1)
         {
            X509_STORE_CTX_set_time(context.get(), 0, when);
            chained = X509_verify_cert(context.get()) == 1;
            if (!chained)
               reason = String(X509_verify_cert_error_string(X509_STORE_CTX_get_error(context.get())));
         }
         sk_X509_free(extra);

         if (!chained)
            return Fail_(verdict, Formatter::Format(_T("The signing certificate does not chain to a trusted authority: {0}."), reason));
      }

      // Explicitly for code signing: a certificate with no extended key usage at
      // all is "any purpose" to OpenSSL and to Go, and Fulcio never issues one, so
      // one here is not Fulcio's.
      X509_check_purpose(leaf.get(), -1, 0);
      if (X509_get_ext_by_NID(leaf.get(), NID_ext_key_usage, -1) < 0 || (X509_get_extended_key_usage(leaf.get()) & XKU_CODE_SIGN) == 0)
         return Fail_(verdict, _T("The signing certificate is not a code-signing certificate."));

      // 5. Whose certificate it is: one trusted signer's, whole. The subject
      // alternative name picks the signer - this project's three prefixes are
      // disjoint, one on github.com and two on gitlab.com for two project paths,
      // neither of which begins the other - and then that signer's
      // issuer, repository and repository id must be the certificate's too, so a
      // certificate with one signer's identity and another's issuer is nobody's.
      // Then the signer's cut-off, and the release: a signer that signed its last
      // release long ago signed no later one, and a workflow that ran for a v*
      // tag signed that tag's release and no other, so a feed that offered its
      // file as another version is refused. When no signer takes it, the refusal
      // is the first signer's whose identity it did name, or, when it named none,
      // the identity it did name.
      std::vector<std::string> uris;
      SanUris_(leaf.get(), uris);

      std::string issuer;
      bool issuerMalformed = false;
      std::string repository;
      bool repositoryRecorded = false;
      std::string repositoryId;
      bool repositoryIdPresent = false;
      bool repositoryIdMalformed = false;
      {
         std::string extension;
         if (ExtensionValue_(leaf.get(), OID_ISSUER_V2, extension))
            issuerMalformed = !DerUtf8String_(extension, issuer);
         else if (ExtensionValue_(leaf.get(), OID_ISSUER_V1, extension))
            issuer = extension;
         repositoryRecorded = ExtensionValue_(leaf.get(), OID_SOURCE_REPOSITORY_URI, extension) && DerUtf8String_(extension, repository);
         repositoryIdPresent = ExtensionValue_(leaf.get(), OID_SOURCE_REPOSITORY_IDENTIFIER, extension);
         repositoryIdMalformed = repositoryIdPresent && !DerUtf8String_(extension, repositoryId);
      }

      const std::string expectedTag = expectedVersion.IsEmpty() ? std::string() : "v" + Utf8Of_(expectedVersion);

      std::string identity;
      String refusal;
      for (size_t s = 0; s < trust.signers.size() && identity.empty(); s++)
      {
         const SigstoreSigner &signer = trust.signers[s];
         const std::string prefix = signer.identity_prefix.c_str();
         if (prefix.empty())
            continue;

         std::string named;
         for (size_t i = 0; i < uris.size() && named.empty(); i++)
         {
            if (uris[i].compare(0, prefix.size(), prefix) == 0)
               named = uris[i];
         }
         if (named.empty())
            continue;

         String why;
         if (issuerMalformed)
            why = _T("The certificate's issuer extension is malformed.");
         else if (issuer != std::string(signer.issuer.c_str()))
            why = Formatter::Format(_T("The certificate was issued on a token from {0}, not {1}."),
               String(issuer.empty() ? "no issuer" : issuer.c_str()), String(signer.issuer));
         else if (!signer.repository.IsEmpty() && !repositoryRecorded)
            why = _T("The certificate names no source repository.");
         else if (!signer.repository.IsEmpty() && repository != std::string(signer.repository.c_str()))
            why = Formatter::Format(_T("The certificate is for {0}, not {1}."), String(repository.c_str()), String(signer.repository));
         else if (!signer.repository_id.IsEmpty() && repositoryIdMalformed)
            why = _T("The certificate's source repository identifier is malformed.");
         else if (!signer.repository_id.IsEmpty() && !repositoryIdPresent && !signer.repository_id_optional)
            why = _T("The certificate names no source repository identifier.");
         else if (!signer.repository_id.IsEmpty() && repositoryIdPresent && repositoryId != std::string(signer.repository_id.c_str()))
            why = Formatter::Format(_T("The certificate is for the repository with the id {0}, not {1}."), String(repositoryId.c_str()), String(signer.repository_id));
         else if (signer.issued_before > 0 && ASN1_TIME_cmp_time_t(X509_get0_notBefore(leaf.get()), (time_t) signer.issued_before) != -1)
            why = Formatter::Format(_T("The certificate names {0}, a signer this server trusts only for certificates issued before {1}, and this one was issued later."),
               String(named.c_str()), String(UtcText_(signer.issued_before).c_str()));
         else if (!signer.last_version.IsEmpty() && !expectedVersion.IsEmpty() &&
                  UpdateChecker::CompareVersions(expectedVersion, String(signer.last_version)) > 0)
            why = Formatter::Format(_T("The certificate names {0}, which signed no release after {1}; this file is offered as {2}."),
               String(named.c_str()), String(signer.last_version), expectedVersion);
         else if (signer.version_in_ref && !expectedTag.empty() && !TaggedRef_(named).empty() && TaggedRef_(named) != expectedTag)
            why = Formatter::Format(_T("The certificate was made for the tag {0}, not for {1}, the release being installed."),
               String(TaggedRef_(named).c_str()), String(expectedTag.c_str()));

         if (why.IsEmpty())
            identity = named;
         else if (refusal.IsEmpty())
            refusal = why;
      }
      if (identity.empty())
      {
         if (!refusal.IsEmpty())
            return Fail_(verdict, refusal);
         std::string named = uris.empty() ? std::string("nobody") : uris[0];
         return Fail_(verdict, Formatter::Format(_T("The certificate names {0}, not this repository's release workflow."), String(named.c_str())));
      }

      // 6. The signature itself.
      PkeyPtr leafKey(X509_get_pubkey(leaf.get()));
      if (!leafKey)
         return Fail_(verdict, _T("The certificate's public key could not be read."));
      if (!VerifyDigest_(leafKey.get(), digest, signature))
         return Fail_(verdict, _T("The signature does not verify under the certificate's key."));

      verdict.verified = true;
      verdict.identity = identity.c_str();
      verdict.integrated_time = integratedTime;
      return true;
   }
}
