// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// See OrganisationCa.h. This is the first code in the tree that signs a
// certificate or a CRL; AcmeClient.cpp builds a request and is the reference for
// the X509 calls, WebPush.cpp and DkimKeyPair.cpp for the two key generations.

#include "StdAfx.h"

#include "OrganisationCa.h"

#include "AuditTrail.h"
#include "Crypt.h"
#include "DirectoryStore.h"
#include "GatewayEncryption.h"
#include "Unicode.h"
#include "Encoding/Base64.h"
#include "../BO/Account.h"
#include "../BO/Alias.h"
#include "../BO/Domain.h"
#include "../Cache/CacheContainer.h"

#include <cstring>
#include <ctime>
#include <cwctype>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string.h>
#include <vector>

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // ---- ownership of what OpenSSL hands out ---------------------------------

      struct FreeCertificate { void operator()(X509 *p) const { X509_free(p); } };
      struct FreeKey { void operator()(EVP_PKEY *p) const { EVP_PKEY_free(p); } };
      struct FreeRequest { void operator()(X509_REQ *p) const { X509_REQ_free(p); } };
      struct FreeCrl { void operator()(X509_CRL *p) const { X509_CRL_free(p); } };
      struct FreeName { void operator()(X509_NAME *p) const { X509_NAME_free(p); } };
      struct FreeTime { void operator()(ASN1_TIME *p) const { ASN1_TIME_free(p); } };
      struct FreeBio { void operator()(BIO *p) const { BIO_free(p); } };

      typedef std::unique_ptr<X509, FreeCertificate> CertificatePtr;
      typedef std::unique_ptr<EVP_PKEY, FreeKey> KeyPtr;
      typedef std::unique_ptr<X509_REQ, FreeRequest> RequestPtr;
      typedef std::unique_ptr<X509_CRL, FreeCrl> CrlPtr;
      typedef std::unique_ptr<X509_NAME, FreeName> NamePtr;
      typedef std::unique_ptr<ASN1_TIME, FreeTime> TimePtr;
      typedef std::unique_ptr<BIO, FreeBio> BioPtr;

      // One lock for everything that decides which root is current, which
      // serial comes next and whether a certificate is revoked: a create, a
      // rotation, an enrolment, a confirmation, a revocation, an account's
      // deletion and an erasure. Held across the few statements each makes,
      // which is what "at most one current root", "a serial names one
      // certificate" and "the first revocation stands" rest on in this
      // process; the unique index on (root, serial) and the revocation's
      // "issuedrevoked = 0" are what rest on them across processes.
      std::mutex &AuthorityLock()
      {
         static std::mutex lock;
         return lock;
      }

      // The CRLs made so far, by root: its DER, the root it was made for (a root
      // id handed out again after a restore is not the root the cached list
      // speaks for), when it was made and its number. generation counts the
      // revocations of the root this process has seen; a list is fresh only
      // while it was made at the generation the entry has now, so that a
      // revocation committed while a list was being made is never left out of
      // the next one.
      struct CachedCrl
      {
         AnsiString der;
         AnsiString root;
         __int64 made = 0;
         __int64 number = 0;
         unsigned __int64 generation = 0;
         unsigned __int64 madeAtGeneration = 0;
         bool present = false;
      };

      std::mutex &CrlLock()
      {
         static std::mutex lock;
         return lock;
      }

      std::map<__int64, CachedCrl> &CrlCache()
      {
         static std::map<__int64, CachedCrl> cache;
         return cache;
      }

      // Every revocation, of whatever kind, ends here: the root's next CRL is
      // made afresh. The entry is made when there is none, so that a list
      // being made for the first time is not stored as fresh either.
      void ForgetCrl(__int64 caId)
      {
         std::lock_guard<std::mutex> guard(CrlLock());
         CrlCache()[caId].generation++;
      }

      // The roots whose key did not open, said once each until a restart.
      std::mutex &ReportedLock()
      {
         static std::mutex lock;
         return lock;
      }

      std::set<__int64> &ReportedKeys()
      {
         static std::set<__int64> reported;
         return reported;
      }

      __int64 Now()
      {
         return (__int64) time(nullptr);
      }

      AnsiString Utf8(const String &value)
      {
         AnsiString out;
         Unicode::WideToMultiByte(value, out);
         return out;
      }

      String Wide(const AnsiString &value)
      {
         String out;
         Unicode::MultiByteToWide(value, out);
         return out;
      }

      AnsiString Hex(const unsigned char *bytes, size_t length)
      {
         static const char digits[] = "0123456789abcdef";
         AnsiString out;
         for (size_t i = 0; i < length; i++)
         {
            out += digits[(bytes[i] >> 4) & 0x0f];
            out += digits[bytes[i] & 0x0f];
         }
         return out;
      }

      // Base64 text to bytes, white space and PEM armour lines ignored. False
      // for anything that is not base64.
      bool DecodeBase64(const AnsiString &text, std::string &bytes)
      {
         bytes.clear();

         AnsiString clean;
         std::vector<AnsiString> lines = StringParser::SplitString(text, "\n");
         for (size_t i = 0; i < lines.size(); i++)
         {
            AnsiString line = lines[i];
            line.Trim();
            if (line.IsEmpty() || line.StartsWith("-----"))
               continue;
            clean += line;
         }

         if (clean.IsEmpty() || clean.GetLength() % 4 != 0)
            return false;

         for (int i = 0; i < clean.GetLength(); i++)
         {
            const char c = clean[i];
            const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=';
            if (!ok)
               return false;
         }

         AnsiString decoded = Base64::Decode(clean.c_str(), clean.GetLength());
         bytes.assign(decoded.c_str(), decoded.size());
         return !bytes.empty();
      }

      AnsiString EncodeBase64(const std::string &bytes)
      {
         if (bytes.empty())
            return AnsiString();
         return Base64::Encode(bytes.data(), (int) bytes.size());
      }

      // The civil date to a day number, days since 1970-01-01 (Howard Hinnant's
      // days_from_civil), so that a certificate's time becomes seconds since the
      // epoch the same way on every platform, with no timegm or _mkgmtime.
      __int64 DaysFromCivil(int year, int month, int day)
      {
         year -= month <= 2 ? 1 : 0;
         const int era = (year >= 0 ? year : year - 399) / 400;
         const int yearOfEra = year - era * 400;
         const int shiftedMonth = month > 2 ? month - 3 : month + 9;
         const int dayOfYear = (153 * shiftedMonth + 2) / 5 + day - 1;
         const int dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
         return (__int64) era * 146097 + (__int64) dayOfEra - 719468;
      }

      __int64 EpochOf(const ASN1_TIME *time)
      {
         if (time == nullptr)
            return 0;

         struct tm parts = {};
         if (ASN1_TIME_to_tm(time, &parts) != 1)
            return 0;

         return DaysFromCivil(parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday) * 86400 +
                (__int64) parts.tm_hour * 3600 + (__int64) parts.tm_min * 60 + (__int64) parts.tm_sec;
      }

      bool SetTime(ASN1_TIME *target, __int64 seconds)
      {
         return target != nullptr && ASN1_TIME_set(target, (time_t) seconds) != nullptr;
      }

      CertificatePtr ReadDer(const std::string &der)
      {
         const unsigned char *cursor = (const unsigned char *) der.data();
         X509 *certificate = d2i_X509(nullptr, &cursor, (long) der.size());
         if (certificate == nullptr)
            ERR_clear_error();
         return CertificatePtr(certificate);
      }

      CertificatePtr ReadCertificate(const AnsiString &base64Der)
      {
         std::string der;
         if (!DecodeBase64(base64Der, der))
            return CertificatePtr();
         return ReadDer(der);
      }

      bool DerOf(X509 *certificate, std::string &der)
      {
         der.clear();
         const int length = i2d_X509(certificate, nullptr);
         if (length <= 0)
            return false;

         der.resize((size_t) length);
         unsigned char *cursor = (unsigned char *) &der[0];
         return i2d_X509(certificate, &cursor) == length;
      }

      AnsiString FingerprintOf(X509 *certificate)
      {
         unsigned char digest[EVP_MAX_MD_SIZE];
         unsigned int length = 0;
         if (X509_digest(certificate, EVP_sha256(), digest, &length) != 1 || length == 0)
            return AnsiString();
         return Hex(digest, (size_t) length);
      }

      AnsiString KeyTypeOf(EVP_PKEY *key)
      {
         if (key == nullptr)
            return AnsiString("unknown");

         AnsiString text;
         if (EVP_PKEY_is_a(key, "RSA"))
         {
            text.Format("RSA-%d", EVP_PKEY_get_bits(key));
            return text;
         }

         if (EVP_PKEY_is_a(key, "EC"))
         {
            char group[64];
            size_t length = 0;
            group[0] = 0;
            if (EVP_PKEY_get_group_name(key, group, sizeof(group), &length) == 1)
            {
               const AnsiString name(group);
               if (name == "prime256v1" || name == "P-256" || name == "secp256r1")
                  return AnsiString("P-256");
               if (name == "secp384r1" || name == "P-384")
                  return AnsiString("P-384");
               return name;
            }

            text.Format("EC-%d", EVP_PKEY_get_bits(key));
            return text;
         }

         return AnsiString("unknown");
      }

      // An envelope of the platform's own secret store: "DPAPI:" on Windows,
      // "LINUX1:" on Linux (Crypt::ProtectSecret's prefixes). Anything else -
      // the legacy Blowfish form above all, whose key is in the binary - is not
      // one, and an authority's key is never taken from it.
      bool IsMachineEnvelope(const String &envelope)
      {
         return envelope.Left(6) == _T("DPAPI:") || envelope.Left(7) == _T("LINUX1:");
      }

      // The authority's private key, protected by the platform's store itself
      // rather than through Crypt::ProtectSecret, which honours
      // ProtectStoredSecretsWithDPAPI and on Windows falls back to Blowfish when
      // DPAPI fails. Empty when the store is unavailable; the caller refuses.
      String ProtectAuthorityKey(const AnsiString &pem)
      {
         const String inner = Crypt::Instance()->EnCrypt(Wide(pem), Crypt::ETDPAPI);
         if (inner.IsEmpty())
            return String();
#ifdef HM_PLATFORM_POSIX
         return _T("LINUX1:") + inner;
#else
         return _T("DPAPI:") + inner;
#endif
      }

      // The authority's private key, opened: the envelope as stored, through the
      // machine's secret store, to PKCS#8 PEM. Empty when it does not open here,
      // when it was wiped, and when it is not a machine envelope at all.
      KeyPtr OpenKey(const OrganisationCa::Authority &authority)
      {
         if (authority.protectedKey.IsEmpty() || !IsMachineEnvelope(authority.protectedKey))
            return KeyPtr();

         const AnsiString pem = Utf8(Crypt::Instance()->UnprotectSecret(authority.protectedKey));
         if (pem.IsEmpty() || pem.Find("PRIVATE KEY") < 0)
            return KeyPtr();

         BioPtr bio(BIO_new_mem_buf(pem.c_str(), pem.GetLength()));
         if (!bio)
            return KeyPtr();

         EVP_PKEY *key = PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr);
         if (key == nullptr)
            ERR_clear_error();
         return KeyPtr(key);
      }

      // One extension, from OpenSSL's configuration syntax, with the issuer and
      // the subject the key identifiers are worked out from.
      bool AddExtension(X509 *certificate, X509 *issuer, int nid, const char *value)
      {
         X509V3_CTX context;
         X509V3_set_ctx_nodb(&context);
         X509V3_set_ctx(&context, issuer, certificate, nullptr, nullptr, 0);

         X509_EXTENSION *extension = X509V3_EXT_nconf_nid(nullptr, &context, nid, value);
         if (extension == nullptr)
         {
            ERR_clear_error();
            return false;
         }

         const bool added = X509_add_ext(certificate, extension, -1) == 1;
         X509_EXTENSION_free(extension);
         return added;
      }

      // The rfc822Name built as a structure rather than from configuration text,
      // so that no character of an address is read as syntax.
      bool AddEmailName(X509 *certificate, const AnsiString &address)
      {
         GENERAL_NAMES *names = GENERAL_NAMES_new();
         GENERAL_NAME *name = GENERAL_NAME_new();
         ASN1_IA5STRING *text = ASN1_IA5STRING_new();
         if (names == nullptr || name == nullptr || text == nullptr ||
             ASN1_STRING_set(text, address.c_str(), address.GetLength()) != 1)
         {
            if (text)
               ASN1_IA5STRING_free(text);
            if (name)
               GENERAL_NAME_free(name);
            if (names)
               GENERAL_NAMES_free(names);
            ERR_clear_error();
            return false;
         }

         GENERAL_NAME_set0_value(name, GEN_EMAIL, text);
         if (sk_GENERAL_NAME_push(names, name) <= 0)
         {
            GENERAL_NAME_free(name);
            GENERAL_NAMES_free(names);
            return false;
         }

         const bool added = X509_add1_ext_i2d(certificate, NID_subject_alt_name, names, 0, X509V3_ADD_DEFAULT) == 1;
         GENERAL_NAMES_free(names);
         if (!added)
            ERR_clear_error();
         return added;
      }

      bool AddNameEntry(X509_NAME *name, int nid, const AnsiString &value, bool utf8)
      {
         return X509_NAME_add_entry_by_NID(name, nid, utf8 ? MBSTRING_UTF8 : MBSTRING_ASC,
                                           (const unsigned char *) value.c_str(), value.GetLength(), -1, 0) == 1;
      }

      // A text of at most maximum characters - code points, not UTF-16 units, so
      // that a pair of surrogates is never cut in half into UTF-8 OpenSSL would
      // refuse. A certificate's commonName holds 64.
      String Clip(const String &value, int maximum)
      {
         String text = value;
         text.Trim();

         int characters = 0;
         int i = 0;
         while (i < text.GetLength())
         {
            if (characters == maximum)
               return text.Mid(0, i);

            const unsigned int unit = (unsigned int) text[i];
            const bool pair = unit >= 0xD800 && unit <= 0xDBFF && i + 1 < text.GetLength() &&
                              (unsigned int) text[i + 1] >= 0xDC00 && (unsigned int) text[i + 1] <= 0xDFFF;
            i += pair ? 2 : 1;
            characters++;
         }

         return text;
      }

      // A path segment of a URL: the UTF-8 of a domain name with every byte that
      // is not a letter, a digit, a dot or a hyphen written as %XX.
      AnsiString PathSegment(const String &value)
      {
         static const char digits[] = "0123456789ABCDEF";
         const AnsiString utf8 = Utf8(value);
         AnsiString out;
         for (int i = 0; i < utf8.GetLength(); i++)
         {
            const unsigned char c = (unsigned char) utf8[i];
            const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
            if (plain)
            {
               out += (char) c;
               continue;
            }
            out += '%';
            out += digits[(c >> 4) & 0x0f];
            out += digits[c & 0x0f];
         }
         return out;
      }

      String DomainNameOf(__int64 domainId)
      {
         std::shared_ptr<const Domain> domain = CacheContainer::Instance()->GetDomain(domainId);
         return domain ? domain->GetName() : String();
      }

      // The domain's name as a name constraint can hold it: lower-case letters,
      // digits, dots and hyphens. Empty for a name that is not written so (an
      // internationalised one), whose root then carries no constraint.
      AnsiString ConstraintDomain(const String &domainName)
      {
         AnsiString name = Utf8(domainName);
         name.Trim();
         name.MakeLower();
         if (name.IsEmpty() || name.GetLength() > 253 || name[0] == '.' || name[name.GetLength() - 1] == '.')
            return AnsiString();

         for (int i = 0; i < name.GetLength(); i++)
         {
            const char c = name[i];
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-'))
               return AnsiString();
         }

         return name;
      }

      // ---- the tables ---------------------------------------------------------

      const char *AuthorityColumns = "caid, cadomainid, cacertificate, cafingerprint, caprivatekey, cacreated, caretired, caserial";
      const char *IssuedColumns = "i.issuedid, i.issuedcaid, i.issuedaccountid, i.issuedaddress, i.issuedserial, i.issuedfingerprint, "
                                  "i.issuednotbefore, i.issuednotafter, i.issuedrevoked, i.issuedreason, i.issuedcertificate, i.issuedpending";

      void ReadAuthority(std::shared_ptr<DALRecordset> recordset, OrganisationCa::Authority &authority)
      {
         authority.id = recordset->GetInt64Value("caid");
         authority.domainId = recordset->GetInt64Value("cadomainid");
         authority.certificate = Utf8(recordset->GetStringValue("cacertificate"));
         authority.fingerprint = Utf8(recordset->GetStringValue("cafingerprint"));
         authority.protectedKey = recordset->GetStringValue("caprivatekey");
         authority.created = recordset->GetInt64Value("cacreated");
         authority.retired = recordset->GetInt64Value("caretired");
         authority.serial = recordset->GetInt64Value("caserial");
      }

      void ReadIssued(std::shared_ptr<DALRecordset> recordset, OrganisationCa::Issued &issued)
      {
         issued.id = recordset->GetInt64Value("issuedid");
         issued.caId = recordset->GetInt64Value("issuedcaid");
         issued.accountId = recordset->GetInt64Value("issuedaccountid");
         issued.address = recordset->GetStringValue("issuedaddress");
         issued.serial = recordset->GetInt64Value("issuedserial");
         issued.fingerprint = Utf8(recordset->GetStringValue("issuedfingerprint"));
         issued.notBefore = recordset->GetInt64Value("issuednotbefore");
         issued.notAfter = recordset->GetInt64Value("issuednotafter");
         issued.revoked = recordset->GetInt64Value("issuedrevoked");
         issued.reason = recordset->GetStringValue("issuedreason");
         issued.certificate = Utf8(recordset->GetStringValue("issuedcertificate"));
         issued.pending = (int) recordset->GetLongValue("issuedpending");
      }

      bool ReadIssuedList(const SQLCommand &command, std::vector<OrganisationCa::Issued> &issued, int limit)
      {
         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (!recordset)
            return false;

         while (!recordset->IsEOF())
         {
            if (limit > 0 && (int) issued.size() >= limit)
               break;

            OrganisationCa::Issued row;
            ReadIssued(recordset, row);
            issued.push_back(row);
            recordset->MoveNext();
         }

         return true;
      }

      bool ReadIssuedById(__int64 id, OrganisationCa::Issued &issued)
      {
         SQLCommand command(String(_T("select ")) + String(IssuedColumns) + _T(" from hm_smimeissued i where i.issuedid = @ID"));
         command.AddParameter("@ID", id);

         std::vector<OrganisationCa::Issued> found;
         if (!ReadIssuedList(command, found, 1) || found.empty())
            return false;

         issued = found[0];
         return true;
      }

      // How a certificate's row is named on the audit trail: by its address
      // and serial, which is how a person and the Deck know a certificate;
      // the statement itself carries neither.
      String NameOf(const OrganisationCa::Issued &issued)
      {
         if (issued.address.IsEmpty())
            return String(_T("serial ")) + StringParser::IntToString(issued.serial);
         return issued.address + _T(" serial ") + StringParser::IntToString(issued.serial);
      }

      // How many certificates of the domain an account holds that are valid now,
      // and how many of those are for one address.
      bool CountValid(__int64 domainId, __int64 accountId, const String &address, __int64 now, int &all, int &forAddress)
      {
         all = 0;
         forAddress = 0;

         SQLCommand command(String(_T("select i.issuedaddress from hm_smimeissued i inner join hm_smimeca c on c.caid = i.issuedcaid ")
                                   _T("where c.cadomainid = @DOMAINID and i.issuedaccountid = @ACCOUNTID and i.issuedrevoked = 0 and i.issuednotafter > @NOW")));
         command.AddParameter("@DOMAINID", domainId);
         command.AddParameter("@ACCOUNTID", accountId);
         command.AddParameter("@NOW", now);

         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (!recordset)
            return false;

         while (!recordset->IsEOF())
         {
            all++;
            if (recordset->GetStringValue("issuedaddress").CompareNoCase(address.c_str()) == 0)
               forAddress++;
            recordset->MoveNext();
         }

         return true;
      }

      // How many certificates were issued to the account in the day before now.
      bool CountToday(__int64 accountId, __int64 now, int &count)
      {
         count = 0;

         // issuednotbefore is the moment of signing less the five minutes a
         // certificate is back-dated by.
         SQLCommand command("select count(*) as c from hm_smimeissued where issuedaccountid = @ACCOUNTID and issuednotbefore > @SINCE");
         command.AddParameter("@ACCOUNTID", accountId);
         command.AddParameter("@SINCE", now - 86400 - 300);

         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (!recordset || recordset->IsEOF())
            return false;

         count = (int) recordset->GetLongValue("c");
         return true;
      }

      // Revokes one certificate, unless something else revoked it first: the
      // statement changes the row only while it is not revoked, and the row
      // read back says whose revocation stands. won is true when it is this
      // one's. False when the table could not be written or read.
      bool WriteRevocation(const OrganisationCa::Issued &issued, __int64 when, const String &reason, bool &won)
      {
         won = false;

         SQLStatement statement;
         statement.SetStatementType(SQLStatement::STUpdate);
         statement.SetTable("hm_smimeissued");
         statement.SetWhereClause(String(_T("issuedid = ")) + StringParser::IntToString(issued.id) + _T(" and issuedrevoked = 0"));
         statement.AddColumnInt64("issuedrevoked", when);
         statement.AddColumn("issuedreason", reason);

         {
            AuditDescription described(_T("hm_smimeissued"), NameOf(issued));
            if (!Application::Instance()->GetDBManager()->Execute(statement))
               return false;
         }

         OrganisationCa::Issued after;
         if (!ReadIssuedById(issued.id, after))
            return false;

         won = after.revoked == when && after.reason.CompareNoCase(reason) == 0;
         if (won)
            ForgetCrl(issued.caId);
         return true;
      }

      bool SetPending(const OrganisationCa::Issued &issued, int pending)
      {
         SQLStatement statement;
         statement.SetStatementType(SQLStatement::STUpdate);
         statement.SetTable("hm_smimeissued");
         statement.SetWhereClause(String(_T("issuedid = ")) + StringParser::IntToString(issued.id));
         statement.AddColumn("issuedpending", (long) pending);

         AuditDescription described(_T("hm_smimeissued"), NameOf(issued));
         return Application::Instance()->GetDBManager()->Execute(statement);
      }

      // The address's other certificates of the domain that are valid now -
      // confirmed or not, since a pending one whose key was never stored is
      // no use to anybody either - revoked as superseded by this one.
      bool Supersede(__int64 domainId, const OrganisationCa::Issued &by, __int64 now, std::vector<__int64> &superseded)
      {
         SQLCommand command(String(_T("select ")) + String(IssuedColumns) +
                            _T(" from hm_smimeissued i inner join hm_smimeca c on c.caid = i.issuedcaid ")
                            _T("where c.cadomainid = @DOMAINID and i.issuedaddress = @ADDRESS and i.issuedrevoked = 0 and i.issuedid <> @ID and i.issuednotafter > @NOW"));
         command.AddParameter("@DOMAINID", domainId);
         command.AddParameter("@ADDRESS", by.address);
         command.AddParameter("@ID", by.id);
         command.AddParameter("@NOW", now);

         std::vector<OrganisationCa::Issued> previous;
         if (!ReadIssuedList(command, previous, 0))
            return false;

         bool ok = true;
         for (size_t i = 0; i < previous.size(); i++)
         {
            bool won = false;
            if (!WriteRevocation(previous[i], now, OrganisationCa::ReasonName(OrganisationCa::ReasonSuperseded), won))
               ok = false;
            else if (won)
               superseded.push_back(previous[i].serial);
         }

         return ok;
      }

      // A pending certificate confirmed: what it supersedes revoked, and then the
      // mark taken off. Called under the authority lock. 409 for a revoked one.
      int ConfirmIssued(__int64 domainId, const OrganisationCa::Issued &issued, std::vector<__int64> &superseded, String &problem)
      {
         if (issued.revoked > 0)
         {
            problem = _T("the certificate is revoked");
            return 409;
         }

         if (issued.pending == OrganisationCa::PendingNone)
            return 200;

         const __int64 now = Now();
         if (issued.pending == OrganisationCa::PendingSupersedes && !Supersede(domainId, issued, now, superseded))
         {
            problem = _T("the certificates it supersedes could not all be revoked");
            return 500;
         }

         if (!SetPending(issued, OrganisationCa::PendingNone))
         {
            problem = _T("the confirmation could not be stored");
            return 500;
         }

         return 200;
      }

      // The key of a root does not open here: said once per root until the
      // server restarts, since it stays so until the authority is rotated, and
      // every enrolment and CRL fetch meets it.
      void ReportKeyOnce(const OrganisationCa::Authority &authority, const char *where)
      {
         {
            std::lock_guard<std::mutex> guard(ReportedLock());
            if (!ReportedKeys().insert(authority.id).second)
               return;
         }

         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7231, where,
            "The S/MIME authority root " + Wide(authority.fingerprint.Mid(0, 16)) + " of " + DomainNameOf(authority.domainId) +
            " cannot sign: its private key does not open on this machine. An envelope of DPAPI opens only on the machine that "
            "protected it, and one of the Linux key file only where that key file is, so a database or a backup carried here holds "
            "an authority that must be rotated in the Control Deck before it signs a certificate or a CRL again. Reported once per "
            "root until the server restarts.");
      }

      // ---- the request ----------------------------------------------------------

      // Every address the request names: its subject's emailAddress and its
      // subjectAltName's rfc822Names, lower-cased, each once.
      void AddressesOf(X509_REQ *request, std::vector<AnsiString> &addresses)
      {
         auto add = [&addresses](const unsigned char *data, int length)
         {
            if (data == nullptr || length <= 0 || length > 255)
               return;
            AnsiString address((const char *) data, length);
            address.Trim();
            address.MakeLower();
            for (size_t i = 0; i < addresses.size(); i++)
            {
               if (addresses[i] == address)
                  return;
            }
            addresses.push_back(address);
         };

         const X509_NAME *subject = X509_REQ_get_subject_name(request);
         if (subject != nullptr)
         {
            int index = -1;
            while ((index = X509_NAME_get_index_by_NID(subject, NID_pkcs9_emailAddress, index)) >= 0)
            {
               const X509_NAME_ENTRY *entry = X509_NAME_get_entry(subject, index);
               const ASN1_STRING *value = entry ? X509_NAME_ENTRY_get_data(entry) : nullptr;
               if (value == nullptr)
                  continue;

               unsigned char *text = nullptr;
               const int length = ASN1_STRING_to_UTF8(&text, value);
               if (length > 0 && text != nullptr)
                  add(text, length);
               if (text != nullptr)
                  OPENSSL_free(text);
            }
         }

         STACK_OF(X509_EXTENSION) *extensions = X509_REQ_get_extensions(request);
         if (extensions != nullptr)
         {
            GENERAL_NAMES *names = (GENERAL_NAMES *) X509V3_get_d2i(extensions, NID_subject_alt_name, nullptr, nullptr);
            if (names != nullptr)
            {
               for (int i = 0; i < sk_GENERAL_NAME_num(names); i++)
               {
                  const GENERAL_NAME *name = sk_GENERAL_NAME_value(names, i);
                  int type = 0;
                  void *value = GENERAL_NAME_get0_value(name, &type);
                  if (type != GEN_EMAIL || value == nullptr)
                     continue;
                  const ASN1_STRING *text = (const ASN1_STRING *) value;
                  add(ASN1_STRING_get0_data(text), ASN1_STRING_length(text));
               }
               GENERAL_NAMES_free(names);
            }
            sk_X509_EXTENSION_pop_free(extensions, X509_EXTENSION_free);
         }

         ERR_clear_error();
      }

      // The request's key against the floors the gateway encryption holds a
      // partner's certificate to: RSA of MinimumRsaBits or more (and no more
      // than 8192, which is a request for work rather than for a certificate),
      // or an elliptic-curve key on P-256 or P-384.
      bool JudgeKey(EVP_PKEY *key, bool &elliptic, String &problem)
      {
         elliptic = false;
         const int minimumRsa = GatewayEncryption::MinimumRsaBits;
         const int minimumEc = GatewayEncryption::MinimumEcBits;

         if (key == nullptr)
         {
            problem = _T("the request carries no public key this server can read");
            return false;
         }

         if (EVP_PKEY_is_a(key, "RSA"))
         {
            const int bits = EVP_PKEY_get_bits(key);
            if (bits < minimumRsa || bits > 8192)
            {
               problem = Formatter::Format(_T("the request's RSA key is {0} bits; this authority certifies RSA keys of {1} to 8192 bits"), bits, minimumRsa);
               return false;
            }
            return true;
         }

         if (EVP_PKEY_is_a(key, "EC"))
         {
            const AnsiString type = KeyTypeOf(key);
            if ((type != "P-256" && type != "P-384") || EVP_PKEY_get_bits(key) < minimumEc)
            {
               problem = _T("the request's elliptic-curve key is not on P-256 or P-384, the two curves this authority certifies");
               return false;
            }
            elliptic = true;
            return true;
         }

         problem = _T("the request's key is neither RSA nor elliptic-curve (P-256 or P-384)");
         return false;
      }

      // Whether an address is the account's: its own, or an alias of its own
      // domain that is switched on and points at it. Not an address another
      // account let it send as, not a proved external address and not a masked
      // one - the authority vouches for who owns a mailbox, and those are
      // somebody else's, or nobody's. Asked at the enrolment and again every
      // time a colleague is answered, since an alias can be deleted, switched
      // off or pointed elsewhere, and an address handed to somebody else.
      bool Permitted(std::shared_ptr<const Account> account, const String &address)
      {
         if (address.CompareNoCase(account->GetAddress().c_str()) == 0)
            return true;

         std::shared_ptr<const Alias> alias = CacheContainer::Instance()->GetAlias(address);
         if (!alias || !alias->GetIsActive() || alias->GetDomainID() != account->GetDomainID())
            return false;

         String value = alias->GetValue();
         value.Trim();
         return value.CompareNoCase(account->GetAddress().c_str()) == 0;
      }

      bool IsAscii(const AnsiString &text)
      {
         for (int i = 0; i < text.GetLength(); i++)
         {
            const unsigned char c = (unsigned char) text[i];
            if (c < 0x21 || c > 0x7e)
               return false;
         }
         return true;
      }

      // ---- the root ---------------------------------------------------------------

      KeyPtr GenerateKey(OrganisationCa::KeyKind kind)
      {
         EVP_PKEY *key = nullptr;
         if (kind == OrganisationCa::KeyRsa3072)
            key = EVP_RSA_gen(3072);
         else
            key = EVP_EC_gen("prime256v1");

         if (key == nullptr)
            ERR_clear_error();
         return KeyPtr(key);
      }

      bool PrivateKeyPem(EVP_PKEY *key, AnsiString &pem)
      {
         pem = "";
         BioPtr memory(BIO_new(BIO_s_mem()));
         if (!memory)
            return false;

         // PKCS#8, "BEGIN PRIVATE KEY", as DkimKeyPair writes a DKIM key.
         if (PEM_write_bio_PrivateKey(memory.get(), key, nullptr, nullptr, 0, nullptr, nullptr) != 1)
         {
            ERR_clear_error();
            return false;
         }

         char *data = nullptr;
         const long length = BIO_get_mem_data(memory.get(), &data);
         if (length <= 0 || data == nullptr)
            return false;

         pem = AnsiString(data, (int) length);
         return true;
      }

      // The self-signed root: CN "<domain> S/MIME Authority" (the name alone when
      // that would pass the 64 characters a commonName holds), CA:TRUE pathlen:0,
      // keyCertSign and cRLSign, emailProtection, a critical name constraint
      // permitting the domain's own addresses and nothing else, a random 127-bit
      // serial, ten years. A root whose key leaked would then vouch for no
      // address outside the domain and for no TLS server at all.
      CertificatePtr MakeRoot(EVP_PKEY *key, const String &domainName, __int64 now)
      {
         CertificatePtr root(X509_new());
         if (!root)
            return CertificatePtr();

         String commonName = domainName + _T(" S/MIME Authority");
         if (commonName.GetLength() > 64)
            commonName = _T("S/MIME Authority");

         // A domain whose name is not written in plain ASCII gets no constraint
         // (docs/SmimeAuthority.md says so); every other gets exactly its name.
         const AnsiString constraintDomain = ConstraintDomain(domainName);
         const AnsiString constraint = constraintDomain.IsEmpty() ? AnsiString() : "critical,permitted;email:" + constraintDomain;

         NamePtr name(X509_NAME_new());
         BIGNUM *serial = BN_new();

         bool ok = name && serial &&
                   X509_set_version(root.get(), X509_VERSION_3) == 1 &&
                   BN_rand(serial, 127, BN_RAND_TOP_ANY, BN_RAND_BOTTOM_ANY) == 1 &&
                   BN_to_ASN1_INTEGER(serial, X509_get_serialNumber(root.get())) != nullptr &&
                   AddNameEntry(name.get(), NID_commonName, Utf8(commonName), true) &&
                   X509_set_subject_name(root.get(), name.get()) == 1 &&
                   X509_set_issuer_name(root.get(), name.get()) == 1 &&
                   SetTime(X509_getm_notBefore(root.get()), now - 300) &&
                   SetTime(X509_getm_notAfter(root.get()), now + (__int64) OrganisationCa::RootYears * 36525 / 100 * 86400) &&
                   X509_set_pubkey(root.get(), key) == 1 &&
                   AddExtension(root.get(), root.get(), NID_basic_constraints, "critical,CA:TRUE,pathlen:0") &&
                   AddExtension(root.get(), root.get(), NID_key_usage, "critical,keyCertSign,cRLSign") &&
                   AddExtension(root.get(), root.get(), NID_ext_key_usage, "emailProtection") &&
                   (constraint.IsEmpty() || AddExtension(root.get(), root.get(), NID_name_constraints, constraint.c_str())) &&
                   AddExtension(root.get(), root.get(), NID_subject_key_identifier, "hash") &&
                   AddExtension(root.get(), root.get(), NID_authority_key_identifier, "keyid:always") &&
                   X509_sign(root.get(), key, EVP_sha256()) > 0;

         if (serial)
            BN_free(serial);

         if (!ok)
         {
            ERR_clear_error();
            return CertificatePtr();
         }

         return root;
      }

      bool InsertAuthority(__int64 domainId, const String &domainName, const AnsiString &certificate, const AnsiString &fingerprint,
                           const String &protectedKey, __int64 created, __int64 retired, __int64 serial, __int64 &id)
      {
         SQLStatement statement;
         statement.SetStatementType(SQLStatement::STInsert);
         statement.SetTable("hm_smimeca");
         statement.SetIdentityColumn("caid");
         statement.AddColumnInt64("cadomainid", domainId);
         statement.AddColumn("cacertificate", Wide(certificate));
         statement.AddColumn("cafingerprint", Wide(fingerprint));
         statement.AddColumn("caprivatekey", protectedKey);
         statement.AddColumnInt64("cacreated", created);
         statement.AddColumnInt64("caretired", retired);
         statement.AddColumnInt64("caserial", serial);

         // A root's row is ids, a certificate and a key; the domain is its name.
         AuditDescription described(_T("hm_smimeca"), domainName);
         id = 0;
         return Application::Instance()->GetDBManager()->Execute(statement, &id) && id > 0;
      }

      bool SetRetired(const OrganisationCa::Authority &authority, const String &domainName, __int64 retired)
      {
         SQLStatement statement;
         statement.SetStatementType(SQLStatement::STUpdate);
         statement.SetTable("hm_smimeca");
         statement.SetWhereClause(String(_T("caid = ")) + StringParser::IntToString(authority.id));
         statement.AddColumnInt64("caretired", retired);

         AuditDescription described(_T("hm_smimeca"), domainName);
         return Application::Instance()->GetDBManager()->Execute(statement);
      }

      bool SetNextSerial(const OrganisationCa::Authority &authority, const String &domainName, __int64 next)
      {
         SQLStatement statement;
         statement.SetStatementType(SQLStatement::STUpdate);
         statement.SetTable("hm_smimeca");
         statement.SetWhereClause(String(_T("caid = ")) + StringParser::IntToString(authority.id));
         statement.AddColumnInt64("caserial", next);

         AuditDescription described(_T("hm_smimeca"), domainName);
         return Application::Instance()->GetDBManager()->Execute(statement);
      }

      // A retired root whose last certificate has expired has no CRL left to
      // sign, so its private key has no work left: it is wiped, and a backup
      // made from then on no longer carries it. Called under the authority
      // lock, from the create, the rotation, the enrolment and the backup.
      void WipeSpentKeys(__int64 domainId, const String &domainName, __int64 now)
      {
         std::vector<OrganisationCa::Authority> authorities;
         if (!OrganisationCa::ForDomain(domainId, authorities))
            return;

         for (size_t i = 0; i < authorities.size(); i++)
         {
            const OrganisationCa::Authority &authority = authorities[i];
            if (authority.retired == 0 || authority.protectedKey.IsEmpty() || OrganisationCa::IsPublished(authority, now))
               continue;

            SQLStatement statement;
            statement.SetStatementType(SQLStatement::STUpdate);
            statement.SetTable("hm_smimeca");
            statement.SetWhereClause(String(_T("caid = ")) + StringParser::IntToString(authority.id));
            statement.AddColumn("caprivatekey", String());

            AuditDescription described(_T("hm_smimeca"), domainName);
            Application::Instance()->GetDBManager()->Execute(statement);
         }
      }
   }

   // ---- reading -------------------------------------------------------------------

   bool
   OrganisationCa::Current(__int64 domainId, Authority &authority)
   {
      authority = Authority();
      if (domainId <= 0)
         return false;

      SQLCommand command(String(_T("select ")) + String(AuthorityColumns) +
                         _T(" from hm_smimeca where cadomainid = @DOMAINID and caretired = 0 order by caid desc"));
      command.AddParameter("@DOMAINID", domainId);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return false;

      ReadAuthority(recordset, authority);
      return true;
   }

   bool
   OrganisationCa::ForDomain(__int64 domainId, std::vector<Authority> &authorities)
   {
      authorities.clear();

      SQLCommand command(String(_T("select ")) + String(AuthorityColumns) +
                         _T(" from hm_smimeca where cadomainid = @DOMAINID order by caid desc"));
      command.AddParameter("@DOMAINID", domainId);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return false;

      std::vector<Authority> retired;
      while (!recordset->IsEOF())
      {
         Authority authority;
         ReadAuthority(recordset, authority);
         if (authority.retired == 0)
            authorities.push_back(authority);
         else
            retired.push_back(authority);
         recordset->MoveNext();
      }

      authorities.insert(authorities.end(), retired.begin(), retired.end());
      return true;
   }

   bool
   OrganisationCa::Find(__int64 domainId, __int64 caId, Authority &authority)
   {
      authority = Authority();

      SQLCommand command(String(_T("select ")) + String(AuthorityColumns) +
                         _T(" from hm_smimeca where caid = @CAID and cadomainid = @DOMAINID"));
      command.AddParameter("@CAID", caId);
      command.AddParameter("@DOMAINID", domainId);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return false;

      ReadAuthority(recordset, authority);
      return true;
   }

   bool
   OrganisationCa::FindByFingerprint(const AnsiString &fingerprint, Authority &authority)
   {
      authority = Authority();
      if (!IsFingerprint(fingerprint))
         return false;

      SQLCommand command(String(_T("select ")) + String(AuthorityColumns) +
                         _T(" from hm_smimeca where cafingerprint = @FINGERPRINT order by caid desc"));
      command.AddParameter("@FINGERPRINT", Wide(fingerprint));

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return false;

      ReadAuthority(recordset, authority);
      return true;
   }

   bool
   OrganisationCa::IsPublished(const Authority &authority, __int64 now)
   {
      if (authority.id <= 0)
         return false;
      if (authority.retired == 0)
         return true;

      SQLCommand command("select count(*) as c from hm_smimeissued where issuedcaid = @CAID and issuednotafter > @NOW");
      command.AddParameter("@CAID", authority.id);
      command.AddParameter("@NOW", now);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return false;

      return recordset->GetLongValue("c") > 0;
   }

   OrganisationCa::RootFacts
   OrganisationCa::Facts(const Authority &authority, bool tryKey)
   {
      RootFacts facts;

      CertificatePtr certificate = ReadCertificate(authority.certificate);
      if (!certificate)
         return facts;

      facts.readable = true;

      char subject[512];
      subject[0] = 0;
      X509_NAME_oneline(X509_get_subject_name(certificate.get()), subject, sizeof(subject));
      facts.subject = AnsiString(subject);
      facts.fingerprint = FingerprintOf(certificate.get());
      facts.keyType = KeyTypeOf(X509_get0_pubkey(certificate.get()));
      facts.notBefore = EpochOf(X509_get0_notBefore(certificate.get()));
      facts.notAfter = EpochOf(X509_get0_notAfter(certificate.get()));

      if (tryKey)
      {
         KeyPtr key = OpenKey(authority);
         facts.keyOpens = key && X509_check_private_key(certificate.get(), key.get()) == 1;
         ERR_clear_error();
      }

      return facts;
   }

   bool
   OrganisationCa::IssuedForDomain(__int64 domainId, int limit, std::vector<Issued> &issued)
   {
      issued.clear();

      SQLCommand command(String(_T("select ")) + String(IssuedColumns) +
                         _T(" from hm_smimeissued i inner join hm_smimeca c on c.caid = i.issuedcaid where c.cadomainid = @DOMAINID order by i.issuedid desc"));
      command.AddParameter("@DOMAINID", domainId);

      return ReadIssuedList(command, issued, limit);
   }

   bool
   OrganisationCa::IssuedForAccount(__int64 domainId, __int64 accountId, std::vector<Issued> &issued)
   {
      issued.clear();

      SQLCommand command(String(_T("select ")) + String(IssuedColumns) +
                         _T(" from hm_smimeissued i inner join hm_smimeca c on c.caid = i.issuedcaid ")
                         _T("where c.cadomainid = @DOMAINID and i.issuedaccountid = @ACCOUNTID order by i.issuedid desc"));
      command.AddParameter("@DOMAINID", domainId);
      command.AddParameter("@ACCOUNTID", accountId);

      return ReadIssuedList(command, issued, 50);
   }

   bool
   OrganisationCa::Published(__int64 domainId, const String &address, std::vector<Issued> &issued)
   {
      issued.clear();
      if (domainId <= 0 || address.IsEmpty() || address.GetLength() > 255)
         return true;

      // One query, the directory's own rule for who is in it - this domain, an
      // account that can sign in - without the opt-out, which hides a person
      // from the LIST and not from somebody who already has the address. A
      // certificate still waiting for its key to be stored is not answered:
      // mail encrypted to it could be to a key nobody holds.
      SQLCommand command(String(_T("select ")) + String(IssuedColumns) +
                         _T(" from hm_smimeissued i inner join hm_smimeca c on c.caid = i.issuedcaid ")
                         _T("inner join hm_accounts a on a.accountid = i.issuedaccountid ")
                         _T("where c.cadomainid = @DOMAINID and a.accountdomainid = @ACCOUNTDOMAINID and a.accountactive = 1 and ")
                         _T("i.issuedaddress = @ADDRESS and i.issuedrevoked = 0 and i.issuedpending = 0 and i.issuednotafter > @NOW order by i.issuedid desc"));
      command.AddParameter("@DOMAINID", domainId);
      command.AddParameter("@ACCOUNTDOMAINID", domainId);
      command.AddParameter("@ADDRESS", address);
      command.AddParameter("@NOW", Now());

      std::vector<Issued> found;
      if (!ReadIssuedList(command, found, MaximumValidPerAccount))
         return false;

      // And the address must still be the account's: an alias deleted, switched
      // off or pointed at somebody else, or an address given up in a rename,
      // takes the certificate out of reach, since mail encrypted to it would be
      // for a key its new owner does not hold.
      for (size_t i = 0; i < found.size(); i++)
      {
         std::shared_ptr<const Account> account = CacheContainer::Instance()->GetAccount(found[i].accountId);
         if (account && Permitted(account, found[i].address))
            issued.push_back(found[i]);
      }

      return true;
   }

   bool
   OrganisationCa::IssuedByFingerprint(__int64 domainId, const AnsiString &fingerprint, Issued &issued)
   {
      issued = Issued();
      if (domainId <= 0 || !IsFingerprint(fingerprint))
         return false;

      SQLCommand command(String(_T("select ")) + String(IssuedColumns) +
                         _T(" from hm_smimeissued i inner join hm_smimeca c on c.caid = i.issuedcaid ")
                         _T("where c.cadomainid = @DOMAINID and i.issuedfingerprint = @FINGERPRINT order by i.issuedid desc"));
      command.AddParameter("@DOMAINID", domainId);
      command.AddParameter("@FINGERPRINT", Wide(fingerprint));

      std::vector<Issued> found;
      if (!ReadIssuedList(command, found, 1) || found.empty())
         return false;

      issued = found[0];
      return true;
   }

   bool
   OrganisationCa::IssuedHere(__int64 domainId, const AnsiString &fingerprint)
   {
      Issued issued;
      return IssuedByFingerprint(domainId, fingerprint, issued);
   }

   bool
   OrganisationCa::StoredStatuses(__int64 accountId, __int64 domainId, std::map<AnsiString, AnsiString> &statuses)
   {
      statuses.clear();

      SQLCommand command("select k.smimefingerprint, i.issuedrevoked, i.issuednotafter from hm_smimekeys k "
                         "inner join hm_smimeissued i on i.issuedfingerprint = k.smimefingerprint "
                         "inner join hm_smimeca c on c.caid = i.issuedcaid "
                         "where k.smimeaccountid = @ACCOUNTID and k.smimeauthority = 1 and c.cadomainid = @DOMAINID");
      command.AddParameter("@ACCOUNTID", accountId);
      command.AddParameter("@DOMAINID", domainId);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return false;

      const __int64 now = Now();
      while (!recordset->IsEOF())
      {
         const AnsiString fingerprint = Utf8(recordset->GetStringValue("smimefingerprint"));
         const __int64 revoked = recordset->GetInt64Value("issuedrevoked");
         const __int64 notAfter = recordset->GetInt64Value("issuednotafter");
         statuses[fingerprint] = revoked > 0 ? AnsiString("revoked") : (notAfter <= now ? AnsiString("expired") : AnsiString("valid"));
         recordset->MoveNext();
      }

      return true;
   }

   bool
   OrganisationCa::TrustAnchors(__int64 domainId, std::vector<std::string> &ders)
   {
      ders.clear();

      std::vector<Authority> authorities;
      if (!ForDomain(domainId, authorities))
         return false;

      // A retired root only while it is published: once its last certificate
      // has expired there is nothing left for it to vouch for, and a root that
      // was rotated because its key was exposed must stop being trusted then
      // at the latest.
      const __int64 now = Now();
      for (size_t i = 0; i < authorities.size(); i++)
      {
         if (authorities[i].retired != 0 && !IsPublished(authorities[i], now))
            continue;

         std::string der;
         if (DecodeBase64(authorities[i].certificate, der))
            ders.push_back(der);
      }

      return true;
   }

   // ---- acting ----------------------------------------------------------------------

   int
   OrganisationCa::Create(__int64 domainId, const String &domainName, KeyKind kind, bool rotate, Authority &made, String &problem)
   {
      made = Authority();
      problem = _T("");

      std::lock_guard<std::mutex> guard(AuthorityLock());

      Authority existing;
      const bool present = Current(domainId, existing);

      if (!rotate && present)
      {
         problem = _T("the domain has an S/MIME authority already; rotate it to replace its root");
         return 409;
      }

      if (rotate && !present)
      {
         problem = _T("the domain has no S/MIME authority to rotate; create one first");
         return 404;
      }

      const __int64 now = Now();

      KeyPtr key = GenerateKey(kind);
      if (!key)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7230, "OrganisationCa::Create",
            "OpenSSL could not generate the key of a new S/MIME authority for " + domainName + ".");
         problem = _T("the authority's key could not be generated; the error log says why");
         return 500;
      }

      CertificatePtr root = MakeRoot(key.get(), domainName, now);
      AnsiString pem;
      std::string der;
      if (!root || !DerOf(root.get(), der) || !PrivateKeyPem(key.get(), pem))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7230, "OrganisationCa::Create",
            "OpenSSL could not make the root certificate of a new S/MIME authority for " + domainName + ".");
         problem = _T("the authority's root could not be made; the error log says why");
         return 500;
      }

      // Protected before anything is written, because a row whose key nothing
      // can read is an authority that looks created and signs nothing - and by
      // the platform's store itself, never the legacy scheme whose key is in the
      // binary. When the store is unavailable nothing is created; on Linux the
      // error log already says what is wrong with the key file.
      const String protectedKey = ProtectAuthorityKey(pem);
      pem = "";
      if (protectedKey.IsEmpty())
      {
         problem = _T("the authority was not created: the machine's secret store (DPAPI on Windows, the stored-secret key file on Linux) could not protect its private key, and this authority never keeps its key under the legacy scheme whose key is in the program. The error log says what is wrong with the store. Nothing has changed.");
         return 503;
      }

      const AnsiString certificate = EncodeBase64(der);
      const AnsiString fingerprint = FingerprintOf(root.get());

      // A rotation retires the root it replaces first and puts it back if the new
      // one cannot be written, so that the domain never has two current roots
      // and, if it has none for a moment, is given its old one back.
      if (rotate && !SetRetired(existing, domainName, now))
      {
         problem = _T("the current root could not be retired; nothing has changed");
         return 500;
      }

      __int64 id = 0;
      const __int64 serial = rotate ? (existing.serial > 0 ? existing.serial : 1) : 1;
      if (!InsertAuthority(domainId, domainName, certificate, fingerprint, protectedKey, now, 0, serial, id))
      {
         if (rotate)
            SetRetired(existing, domainName, 0);
         problem = _T("the authority could not be stored");
         return 500;
      }

      if (rotate)
      {
         ForgetCrl(existing.id);
         // A root retired with no certificate left alive has nothing to sign.
         WipeSpentKeys(domainId, domainName, now);
      }

      if (!Find(domainId, id, made))
      {
         problem = _T("the authority was stored and could not be read back");
         return 500;
      }

      return rotate ? 200 : 201;
   }

   int
   OrganisationCa::Enrol(std::shared_ptr<const Account> account, const AnsiString &requestText, bool keep,
                         Issued &made, std::vector<__int64> &supersedes, String &problem)
   {
      made = Issued();
      supersedes.clear();
      problem = _T("");

      if (!account)
      {
         problem = _T("internal error");
         return 500;
      }

      const __int64 domainId = account->GetDomainID();
      const String domainName = DomainNameOf(domainId);

      Authority peek;
      if (!Current(domainId, peek))
      {
         problem = _T("this domain has no S/MIME authority: an administrator creates one in the Control Deck's domain editor, and until then a certificate has to come from elsewhere");
         return 404;
      }

      // ---- the request, before anything is locked

      if (requestText.GetLength() > MaximumRequestLength)
      {
         problem = _T("csr: a certificate request of at most 16 KB is expected");
         return 400;
      }

      std::string der;
      if (!DecodeBase64(requestText, der))
      {
         problem = _T("csr: a PKCS#10 certificate request, base64 DER or PEM, is expected");
         return 400;
      }

      const unsigned char *cursor = (const unsigned char *) der.data();
      RequestPtr request(d2i_X509_REQ(nullptr, &cursor, (long) der.size()));
      if (!request)
      {
         ERR_clear_error();
         problem = _T("csr: not a PKCS#10 certificate request");
         return 400;
      }

      EVP_PKEY *requestKey = X509_REQ_get0_pubkey(request.get());
      bool elliptic = false;
      if (!JudgeKey(requestKey, elliptic, problem))
         return 400;

      // The request is signed by the key it asks to have certified, which is the
      // proof that whoever asks holds it.
      if (X509_REQ_verify(request.get(), requestKey) != 1)
      {
         ERR_clear_error();
         problem = _T("csr: the request's signature does not verify with its own key");
         return 400;
      }

      std::vector<AnsiString> addresses;
      AddressesOf(request.get(), addresses);
      if (addresses.empty())
      {
         problem = _T("csr: the request names no address - an emailAddress in its subject or an rfc822Name in its subjectAltName");
         return 400;
      }

      if (addresses.size() > 1)
      {
         problem = _T("csr: the request names more than one address; a certificate from this authority names one");
         return 400;
      }

      const AnsiString addressText = addresses[0];
      if (!IsAscii(addressText) || addressText.Find("@") <= 0)
      {
         problem = _T("csr: the request's address is not an ASCII e-mail address, and a certificate's rfc822Name holds nothing else");
         return 400;
      }

      if (addressText.GetLength() > MaximumAddressLength)
      {
         problem = _T("csr: the address is longer than the 128 characters a certificate's emailAddress holds");
         return 400;
      }

      const String address = Wide(addressText);
      if (!Permitted(account, address))
      {
         problem = Formatter::Format(_T("csr: {0} is not this account's address or one of its aliases, so this authority does not certify it"), address);
         return 400;
      }

      // ---- the signature, under the lock

      std::lock_guard<std::mutex> guard(AuthorityLock());

      Authority authority;
      if (!Current(domainId, authority))
      {
         problem = _T("this domain has no S/MIME authority");
         return 404;
      }

      const __int64 now = Now();
      WipeSpentKeys(domainId, domainName, now);

      CertificatePtr root = ReadCertificate(authority.certificate);
      KeyPtr authorityKey = OpenKey(authority);
      if (!root || !authorityKey)
      {
         ReportKeyOnce(authority, "OrganisationCa::Enrol");
         problem = _T("the domain's S/MIME authority cannot sign on this machine: its key does not open here, which is what a database or a backup carried from another machine does. An administrator rotates the authority to give it a new root.");
         return 503;
      }

      int today = 0;
      if (!CountToday(account->GetID(), now, today))
      {
         problem = _T("the certificates already issued could not be read");
         return 500;
      }

      if (today >= EnrolmentsPerDay)
      {
         problem = Formatter::Format(_T("this account has been issued {0} certificates in the last day, which is as many as it may; enrol again tomorrow"), today);
         return 429;
      }

      int valid = 0;
      int forAddress = 0;
      if (!CountValid(domainId, account->GetID(), address, now, valid, forAddress))
      {
         problem = _T("the certificates already issued could not be read");
         return 500;
      }

      // A pending certificate supersedes the others only once it is confirmed,
      // so they are all counted here; the person revokes one to make room.
      if (valid >= MaximumValidPerAccount)
      {
         problem = Formatter::Format(_T("the account holds {0} valid certificates from this authority, which is as many as it may; revoke one first"), valid);
         return 409;
      }

      const __int64 serial = authority.serial > 0 ? authority.serial : 1;
      if (!SetNextSerial(authority, domainName, serial + 1))
      {
         problem = _T("the authority's serial number could not be advanced");
         return 500;
      }

      // One year, and never past the root's own end.
      const __int64 notBefore = now - 300;
      __int64 notAfter = now + (__int64) CertificateDays * 86400;
      const __int64 rootEnd = EpochOf(X509_get0_notAfter(root.get()));
      if (rootEnd > 0 && notAfter > rootEnd)
         notAfter = rootEnd;

      String displayName = DirectoryStore::NameOf(account->GetPersonFirstName(), account->GetPersonLastName());
      if (displayName.IsEmpty())
         displayName = address;
      displayName = Clip(displayName, 64);

      CertificatePtr leaf(X509_new());
      NamePtr subject(X509_NAME_new());
      bool ok = leaf && subject &&
                X509_set_version(leaf.get(), X509_VERSION_3) == 1 &&
                ASN1_INTEGER_set_int64(X509_get_serialNumber(leaf.get()), serial) == 1 &&
                X509_set_issuer_name(leaf.get(), X509_get_subject_name(root.get())) == 1 &&
                AddNameEntry(subject.get(), NID_commonName, Utf8(displayName), true) &&
                AddNameEntry(subject.get(), NID_pkcs9_emailAddress, addressText, false) &&
                X509_set_subject_name(leaf.get(), subject.get()) == 1 &&
                SetTime(X509_getm_notBefore(leaf.get()), notBefore) &&
                SetTime(X509_getm_notAfter(leaf.get()), notAfter) &&
                X509_set_pubkey(leaf.get(), requestKey) == 1 &&
                AddExtension(leaf.get(), root.get(), NID_basic_constraints, "critical,CA:FALSE") &&
                AddExtension(leaf.get(), root.get(), NID_key_usage, elliptic ? "critical,digitalSignature,keyAgreement" : "critical,digitalSignature,keyEncipherment") &&
                AddExtension(leaf.get(), root.get(), NID_ext_key_usage, "emailProtection") &&
                AddEmailName(leaf.get(), addressText) &&
                AddExtension(leaf.get(), root.get(), NID_subject_key_identifier, "hash") &&
                AddExtension(leaf.get(), root.get(), NID_authority_key_identifier, "keyid:always");

      // Where the CRL and the root are fetched from, when the administrator has
      // said where the web services listener is reached, the root named by its
      // fingerprint - which a restore and a domain's rename both leave as it
      // is. Without the setting the certificate carries neither, and a client
      // that insists on checking revocation cannot; the doc says what that costs.
      const String base = BaseUrl();
      if (ok && !base.IsEmpty())
      {
         const AnsiString crlUrl = Utf8(base) + CrlPath(authority.fingerprint);
         const AnsiString rootUrl = Utf8(base) + RootPath(authority.fingerprint, "cer");
         ok = AddExtension(leaf.get(), root.get(), NID_crl_distribution_points, ("URI:" + crlUrl).c_str()) &&
              AddExtension(leaf.get(), root.get(), NID_info_access, ("caIssuers;URI:" + rootUrl).c_str());
      }

      std::string leafDer;
      ok = ok && X509_sign(leaf.get(), authorityKey.get(), EVP_sha256()) > 0 && DerOf(leaf.get(), leafDer);
      if (!ok)
      {
         ERR_clear_error();
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7232, "OrganisationCa::Enrol",
            "OpenSSL could not sign a certificate for " + address + " with the S/MIME authority of " + domainName + ".");
         problem = _T("the certificate could not be signed; the error log says why");
         return 500;
      }

      // The root's name constraint, where it has one, must permit the address:
      // a root made before the domain was renamed permits the old name's
      // addresses only, and a certificate outside it would verify nowhere. The
      // certificate as it will be read - decoded again - is what is checked.
      NAME_CONSTRAINTS *constraints = (NAME_CONSTRAINTS *) X509_get_ext_d2i(root.get(), NID_name_constraints, nullptr, nullptr);
      if (constraints != nullptr)
      {
         CertificatePtr issuedLeaf = ReadDer(leafDer);
         const int verdict = issuedLeaf ? NAME_CONSTRAINTS_check(issuedLeaf.get(), constraints) : X509_V_ERR_UNSPECIFIED;
         NAME_CONSTRAINTS_free(constraints);
         ERR_clear_error();
         if (verdict != X509_V_OK)
         {
            problem = _T("the domain's S/MIME authority was made for another name of the domain - it was renamed since - and its root certifies addresses of that name only. An administrator rotates the authority to give it a root for this one.");
            return 409;
         }
      }
      ERR_clear_error();

      Issued row;
      row.caId = authority.id;
      row.accountId = account->GetID();
      row.address = address;
      row.serial = serial;
      row.fingerprint = FingerprintOf(leaf.get());
      row.notBefore = notBefore;
      row.notAfter = notAfter;
      row.revoked = 0;
      row.reason = _T("");
      row.certificate = EncodeBase64(leafDer);
      row.pending = keep ? PendingKeeps : PendingSupersedes;

      SQLStatement statement;
      statement.SetStatementType(SQLStatement::STInsert);
      statement.SetTable("hm_smimeissued");
      statement.SetIdentityColumn("issuedid");
      statement.AddColumnInt64("issuedcaid", row.caId);
      statement.AddColumnInt64("issuedaccountid", row.accountId);
      statement.AddColumn("issuedaddress", row.address);
      statement.AddColumnInt64("issuedserial", row.serial);
      statement.AddColumn("issuedfingerprint", Wide(row.fingerprint));
      statement.AddColumnInt64("issuednotbefore", row.notBefore);
      statement.AddColumnInt64("issuednotafter", row.notAfter);
      statement.AddColumnInt64("issuedrevoked", 0);
      statement.AddColumn("issuedreason", row.reason);
      statement.AddColumn("issuedcertificate", Wide(row.certificate));
      statement.AddColumn("issuedpending", (long) row.pending);

      __int64 id = 0;
      {
         AuditDescription described(_T("hm_smimeissued"), NameOf(row));
         if (!Application::Instance()->GetDBManager()->Execute(statement, &id) || id <= 0)
         {
            problem = _T("the certificate was signed and could not be stored, so it was not issued");
            return 500;
         }
      }
      row.id = id;

      // What its confirmation will supersede: the address's other valid
      // certificates, unless the person keeps them - two devices, each with its
      // own key. Nothing is revoked yet: that waits for the key to be stored.
      if (!keep)
      {
         SQLCommand command(String(_T("select ")) + String(IssuedColumns) +
                            _T(" from hm_smimeissued i inner join hm_smimeca c on c.caid = i.issuedcaid ")
                            _T("where c.cadomainid = @DOMAINID and i.issuedaddress = @ADDRESS and i.issuedrevoked = 0 and i.issuedid <> @ID and i.issuednotafter > @NOW"));
         command.AddParameter("@DOMAINID", domainId);
         command.AddParameter("@ADDRESS", address);
         command.AddParameter("@ID", id);
         command.AddParameter("@NOW", now);

         std::vector<Issued> previous;
         ReadIssuedList(command, previous, 0);
         for (size_t i = 0; i < previous.size(); i++)
            supersedes.push_back(previous[i].serial);
      }

      made = row;
      return 201;
   }

   int
   OrganisationCa::Confirm(std::shared_ptr<const Account> account, __int64 serial, std::vector<__int64> &superseded, String &problem)
   {
      superseded.clear();
      problem = _T("");
      if (!account)
      {
         problem = _T("internal error");
         return 500;
      }

      std::lock_guard<std::mutex> guard(AuthorityLock());

      SQLCommand command(String(_T("select ")) + String(IssuedColumns) +
                         _T(" from hm_smimeissued i inner join hm_smimeca c on c.caid = i.issuedcaid ")
                         _T("where c.cadomainid = @DOMAINID and i.issuedserial = @SERIAL and i.issuedaccountid = @ACCOUNTID"));
      command.AddParameter("@DOMAINID", account->GetDomainID());
      command.AddParameter("@SERIAL", serial);
      command.AddParameter("@ACCOUNTID", account->GetID());

      std::vector<Issued> found;
      if (!ReadIssuedList(command, found, 1))
      {
         problem = _T("the certificates could not be read");
         return 500;
      }

      if (found.empty())
      {
         problem = _T("no such certificate");
         return 404;
      }

      return ConfirmIssued(account->GetDomainID(), found[0], superseded, problem);
   }

   void
   OrganisationCa::ConfirmStored(std::shared_ptr<const Account> account, const AnsiString &fingerprint)
   {
      if (!account || !IsFingerprint(fingerprint))
         return;

      std::lock_guard<std::mutex> guard(AuthorityLock());

      Issued issued;
      if (!IssuedByFingerprint(account->GetDomainID(), fingerprint, issued))
         return;

      // Only the account's own, and only one still waiting.
      if (issued.accountId != account->GetID() || issued.pending == PendingNone || issued.revoked > 0)
         return;

      std::vector<__int64> superseded;
      String problem;
      ConfirmIssued(account->GetDomainID(), issued, superseded, problem);
   }

   int
   OrganisationCa::Revoke(__int64 domainId, __int64 serial, __int64 accountId, Reason reason, Issued &revoked, String &problem)
   {
      revoked = Issued();
      problem = _T("");

      std::lock_guard<std::mutex> guard(AuthorityLock());

      SQLCommand command(String(_T("select ")) + String(IssuedColumns) +
                         _T(" from hm_smimeissued i inner join hm_smimeca c on c.caid = i.issuedcaid ")
                         _T("where c.cadomainid = @DOMAINID and i.issuedserial = @SERIAL order by i.issuedid desc"));
      command.AddParameter("@DOMAINID", domainId);
      command.AddParameter("@SERIAL", serial);

      std::vector<Issued> found;
      if (!ReadIssuedList(command, found, 1))
      {
         problem = _T("the certificates could not be read");
         return 500;
      }

      // A person's own route answers another account's certificate exactly as it
      // answers one that does not exist.
      if (found.empty() || (accountId > 0 && found[0].accountId != accountId))
      {
         problem = _T("no such certificate");
         return 404;
      }

      Issued row = found[0];
      if (row.revoked > 0)
      {
         revoked = row;
         problem = _T("the certificate is revoked already");
         return 409;
      }

      const __int64 now = Now();
      bool won = false;
      if (!WriteRevocation(row, now, ReasonName(reason), won))
      {
         problem = _T("the revocation could not be stored");
         return 500;
      }

      // Revoked by something else between the read and the write: the first
      // revocation's date and reason stand, as they may be on a CRL already.
      if (!won)
      {
         ReadIssuedById(row.id, revoked);
         problem = _T("the certificate is revoked already");
         return 409;
      }

      row.revoked = now;
      row.reason = ReasonName(reason);
      revoked = row;
      return 200;
   }

   bool
   OrganisationCa::AccountDeleted(__int64 accountId, const String &address)
   {
      if (accountId <= 0)
         return true;

      std::lock_guard<std::mutex> guard(AuthorityLock());

      SQLCommand command(String(_T("select ")) + String(IssuedColumns) + _T(" from hm_smimeissued i where i.issuedaccountid = @ACCOUNTID"));
      command.AddParameter("@ACCOUNTID", accountId);

      std::vector<Issued> rows;
      if (!ReadIssuedList(command, rows, 0))
         return false;

      // Nothing names the account - which is every account on a server without
      // an authority - and nothing is written, or recorded.
      if (rows.empty())
         return true;

      // Revoked with the reason RFC 5280 gives for a subject that has gone, and
      // left on the CRL: a certificate that verified yesterday must now say why
      // it does not. Each is revoked on its own, so that the audit trail names
      // each by its address and serial.
      bool ok = true;
      const __int64 now = Now();
      for (size_t i = 0; i < rows.size(); i++)
      {
         if (rows[i].revoked > 0 || rows[i].notAfter <= now)
            continue;

         bool won = false;
         if (!WriteRevocation(rows[i], now, ReasonName(ReasonCessationOfOperation), won))
            ok = false;
      }

      // Then the account's number goes from every row, because account numbers
      // are handed out again and a certificate must never become the next
      // account's.
      SQLStatement clear;
      clear.SetStatementType(SQLStatement::STUpdate);
      clear.SetTable("hm_smimeissued");
      clear.SetWhereClause(String(_T("issuedaccountid = ")) + StringParser::IntToString(accountId));
      clear.AddColumnInt64("issuedaccountid", 0);

      {
         AuditDescription described(_T("hm_smimeissued"), address + _T(": the certificates of a deleted account"));
         if (!Application::Instance()->GetDBManager()->Execute(clear))
            ok = false;
      }

      return ok;
   }

   int
   OrganisationCa::EraseAddress(const String &address)
   {
      String lowered = address;
      lowered.Trim();
      lowered.MakeLower();
      if (lowered.IsEmpty())
         return 0;

      std::lock_guard<std::mutex> guard(AuthorityLock());

      SQLCommand command(String(_T("select ")) + String(IssuedColumns) + _T(" from hm_smimeissued i where i.issuedaddress = @ADDRESS"));
      command.AddParameter("@ADDRESS", lowered);

      std::vector<Issued> rows;
      if (!ReadIssuedList(command, rows, 0))
         return -1;

      const __int64 now = Now();
      int changed = 0;
      for (size_t i = 0; i < rows.size(); i++)
      {
         const Issued &row = rows[i];
         const bool valid = row.revoked == 0 && row.notAfter > now;

         SQLStatement statement;
         statement.SetStatementType(SQLStatement::STUpdate);
         statement.SetTable("hm_smimeissued");
         statement.SetWhereClause(String(_T("issuedid = ")) + StringParser::IntToString(row.id));
         statement.AddColumn("issuedaddress", String());
         statement.AddColumn("issuedcertificate", String());
         statement.AddColumnInt64("issuedaccountid", 0);
         if (valid)
         {
            statement.AddColumnInt64("issuedrevoked", now);
            statement.AddColumn("issuedreason", ReasonName(ReasonCessationOfOperation));
         }

         // Named by its serial alone: the address is what is being forgotten.
         AuditDescription described(_T("hm_smimeissued"), String(_T("serial ")) + StringParser::IntToString(row.serial) + _T(", erased"));
         if (!Application::Instance()->GetDBManager()->Execute(statement))
            return -1;

         if (valid)
            ForgetCrl(row.caId);
         changed++;
      }

      return changed;
   }

   // ---- publication ------------------------------------------------------------------

   int
   OrganisationCa::Crl(const Authority &authority, AnsiString &der, String &problem)
   {
      der = "";
      problem = _T("");

      const __int64 now = Now();
      __int64 previousNumber = 0;
      unsigned __int64 generation = 0;

      {
         std::lock_guard<std::mutex> guard(CrlLock());
         CachedCrl &entry = CrlCache()[authority.id];
         generation = entry.generation;
         if (entry.present && entry.root == authority.certificate)
         {
            previousNumber = entry.number;
            if (entry.madeAtGeneration == entry.generation && now - entry.made < CrlRefreshSeconds && now >= entry.made)
            {
               der = entry.der;
               return 200;
            }
         }
      }

      CertificatePtr root = ReadCertificate(authority.certificate);
      KeyPtr key = OpenKey(authority);
      if (!root || !key)
      {
         ReportKeyOnce(authority, "OrganisationCa::Crl");
         problem = _T("the authority's key does not open on this machine, so it cannot sign its CRL");
         return 404;
      }

      SQLCommand command("select issuedserial, issuedrevoked, issuedreason from hm_smimeissued where issuedcaid = @CAID and issuedrevoked > 0 order by issuedserial");
      command.AddParameter("@CAID", authority.id);
      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
      {
         problem = _T("the revoked certificates could not be read");
         return 500;
      }

      CrlPtr crl(X509_CRL_new());
      TimePtr thisUpdate(ASN1_TIME_new());
      TimePtr nextUpdate(ASN1_TIME_new());
      bool ok = crl && thisUpdate && nextUpdate &&
                X509_CRL_set_version(crl.get(), X509_CRL_VERSION_2) == 1 &&
                X509_CRL_set_issuer_name(crl.get(), X509_get_subject_name(root.get())) == 1 &&
                SetTime(thisUpdate.get(), now) &&
                SetTime(nextUpdate.get(), now + (__int64) CrlLifetimeDays * 86400) &&
                X509_CRL_set1_lastUpdate(crl.get(), thisUpdate.get()) == 1 &&
                X509_CRL_set1_nextUpdate(crl.get(), nextUpdate.get()) == 1;

      while (ok && !recordset->IsEOF())
      {
         const __int64 serial = recordset->GetInt64Value("issuedserial");
         const __int64 when = recordset->GetInt64Value("issuedrevoked");
         Reason reason = ReasonKeyCompromise;
         ParseReason(recordset->GetStringValue("issuedreason"), reason);

         X509_REVOKED *entry = X509_REVOKED_new();
         ASN1_INTEGER *number = ASN1_INTEGER_new();
         TimePtr revokedAt(ASN1_TIME_new());
         ASN1_ENUMERATED *code = ASN1_ENUMERATED_new();

         // The serial and the date are copied in by their setters; the reason
         // code by the i2d that encodes it.
         bool entryOk = entry && number && revokedAt && code &&
                        ASN1_INTEGER_set_int64(number, serial) == 1 &&
                        X509_REVOKED_set_serialNumber(entry, number) == 1 &&
                        SetTime(revokedAt.get(), when) &&
                        X509_REVOKED_set_revocationDate(entry, revokedAt.get()) == 1 &&
                        ASN1_ENUMERATED_set(code, (long) reason) == 1 &&
                        X509_REVOKED_add1_ext_i2d(entry, NID_crl_reason, code, 0, 0) == 1;

         if (number)
            ASN1_INTEGER_free(number);
         if (code)
            ASN1_ENUMERATED_free(code);

         if (entryOk && X509_CRL_add0_revoked(crl.get(), entry) == 1)
            entry = nullptr;
         else
            ok = false;

         if (entry)
            X509_REVOKED_free(entry);

         recordset->MoveNext();
      }

      // The CRL's number rises with every list this process makes for the root,
      // and starts from the clock, so that a restart does not go back.
      const __int64 crlNumber = previousNumber >= now ? previousNumber + 1 : now;

      if (ok)
      {
         ASN1_INTEGER *number = ASN1_INTEGER_new();
         ok = number && ASN1_INTEGER_set_int64(number, crlNumber) == 1 &&
              X509_CRL_add1_ext_i2d(crl.get(), NID_crl_number, number, 0, 0) == 1;
         if (number)
            ASN1_INTEGER_free(number);
      }

      if (ok)
      {
         X509V3_CTX context;
         X509V3_set_ctx_nodb(&context);
         X509V3_set_ctx(&context, root.get(), nullptr, nullptr, crl.get(), 0);
         X509_EXTENSION *keyIdentifier = X509V3_EXT_nconf_nid(nullptr, &context, NID_authority_key_identifier, "keyid:always");
         ok = keyIdentifier != nullptr && X509_CRL_add_ext(crl.get(), keyIdentifier, -1) == 1;
         if (keyIdentifier)
            X509_EXTENSION_free(keyIdentifier);
      }

      ok = ok && X509_CRL_sort(crl.get()) == 1 && X509_CRL_sign(crl.get(), key.get(), EVP_sha256()) > 0;

      std::string bytes;
      if (ok)
      {
         const int length = i2d_X509_CRL(crl.get(), nullptr);
         if (length > 0)
         {
            bytes.resize((size_t) length);
            unsigned char *cursor = (unsigned char *) &bytes[0];
            ok = i2d_X509_CRL(crl.get(), &cursor) == length;
         }
         else
            ok = false;
      }

      if (!ok)
      {
         ERR_clear_error();
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7233, "OrganisationCa::Crl",
            "OpenSSL could not sign the CRL of S/MIME authority root " + StringParser::IntToString(authority.id) + ".");
         problem = _T("the CRL could not be signed; the error log says why");
         return 500;
      }

      der = AnsiString(bytes.data(), (int) bytes.size());

      // Kept as made at the generation read before the revoked rows were: a
      // revocation that arrived while this list was being made has moved the
      // generation on, so this list is not served as fresh and the next fetch
      // makes one with it.
      {
         std::lock_guard<std::mutex> guard(CrlLock());
         CachedCrl &entry = CrlCache()[authority.id];
         entry.der = der;
         entry.root = authority.certificate;
         entry.made = now;
         entry.number = crlNumber > entry.number ? crlNumber : entry.number;
         entry.madeAtGeneration = generation;
         entry.present = true;
      }

      return 200;
   }

   AnsiString
   OrganisationCa::Pem(const AnsiString &base64Der, const char *label)
   {
      AnsiString pem = AnsiString("-----BEGIN ") + label + "-----\r\n";
      for (int i = 0; i < base64Der.GetLength(); i += 64)
         pem += base64Der.Mid(i, 64) + "\r\n";
      pem += AnsiString("-----END ") + label + "-----\r\n";
      return pem;
   }

   String
   OrganisationCa::BaseUrl()
   {
      String url = Configuration::Instance()->GetSmimeAuthorityBaseUrl();
      String reason;
      if (!IsUsableBaseUrl(url, reason))
         return String();

      url.Trim();
      while (url.Right(1) == _T("/"))
         url = url.Mid(0, url.GetLength() - 1);

      return url;
   }

   bool
   OrganisationCa::IsUsableBaseUrl(const String &value, String &reason)
   {
      String url = value;
      url.Trim();

      // Empty is the setting's off: no distribution point in any certificate.
      if (url.IsEmpty())
         return true;

      const String sentence = _T("The base URL is absolute: http:// or https://, a host name with a port if it needs one, and an optional path, such as http://pki.example.com - where the clients reach the web services listener (WebServicesHttpPort); RFC 5280 recommends plain HTTP for a CRL distribution point. It is written into every certificate the authority issues, so it holds only what a URI may: a host of letters, digits, dots and hyphens (a name that is not ASCII in its punycode form), and a path of letters, digits, - . _ ~ / ! $ & ' ( ) * + ; = : @ and %XX - no space, comma, quote, query or fragment - in at most 200 characters.");

      if (url.GetLength() > 200)
      {
         reason = sentence;
         return false;
      }

      String lowered = url;
      lowered.MakeLower();

      int rest = 0;
      if (lowered.Find(_T("http://")) == 0)
         rest = 7;
      else if (lowered.Find(_T("https://")) == 0)
         rest = 8;
      else
      {
         reason = sentence;
         return false;
      }

      // The authority: host, and a port after a colon. Nothing else - no user,
      // no IPv6 literal.
      int at = rest;
      int hostLength = 0;
      while (at < url.GetLength() && url[at] != '/' && url[at] != ':')
      {
         const wchar_t c = url[at];
         const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
         if (!ok)
         {
            reason = sentence;
            return false;
         }
         at++;
         hostLength++;
      }

      if (hostLength == 0 || url[rest] == '.' || url[rest] == '-' || url[at - 1] == '.' || url[at - 1] == '-')
      {
         reason = sentence;
         return false;
      }

      if (at < url.GetLength() && url[at] == ':')
      {
         at++;
         int digits = 0;
         long port = 0;
         while (at < url.GetLength() && url[at] >= '0' && url[at] <= '9')
         {
            port = port * 10 + (url[at] - '0');
            digits++;
            at++;
            if (digits > 5)
               break;
         }

         if (digits == 0 || digits > 5 || port < 1 || port > 65535 || (at < url.GetLength() && url[at] != '/'))
         {
            reason = sentence;
            return false;
         }
      }

      // The path. The comma stays out although RFC 3986 allows it: OpenSSL's
      // configuration syntax, which the extensions are written in, reads it
      // as the end of the value.
      for (; at < url.GetLength(); at++)
      {
         const wchar_t c = url[at];
         const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                            c == '-' || c == '.' || c == '_' || c == '~' || c == '/' || c == '!' || c == '$' || c == '&' ||
                            c == '\'' || c == '(' || c == ')' || c == '*' || c == '+' || c == ';' || c == '=' || c == ':' || c == '@';
         if (plain)
            continue;

         const bool escaped = c == '%' && at + 2 < url.GetLength() && iswxdigit(url[at + 1]) && iswxdigit(url[at + 2]);
         if (!escaped)
         {
            reason = sentence;
            return false;
         }
         at += 2;
      }

      return true;
   }

   AnsiString
   OrganisationCa::RootPath(const AnsiString &fingerprint, const char *extension)
   {
      return "/smime-ca/" + fingerprint + "." + extension;
   }

   AnsiString
   OrganisationCa::CrlPath(const AnsiString &fingerprint)
   {
      return RootPath(fingerprint, "crl");
   }

   AnsiString
   OrganisationCa::CurrentPath(const String &domainName, const char *extension)
   {
      return "/smime-ca/" + PathSegment(domainName) + "." + extension;
   }

   bool
   OrganisationCa::IsFingerprint(const AnsiString &text)
   {
      if (text.GetLength() != 64)
         return false;

      for (int i = 0; i < 64; i++)
      {
         const char c = text[i];
         if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
      }

      return true;
   }

   // ---- reasons ------------------------------------------------------------------------

   bool
   OrganisationCa::ParseReason(const String &text, Reason &reason)
   {
      String value = text;
      value.Trim();

      if (value.CompareNoCase(_T("keyCompromise")) == 0)
         reason = ReasonKeyCompromise;
      else if (value.CompareNoCase(_T("superseded")) == 0)
         reason = ReasonSuperseded;
      else if (value.CompareNoCase(_T("cessationOfOperation")) == 0)
         reason = ReasonCessationOfOperation;
      else
         return false;

      return true;
   }

   String
   OrganisationCa::ReasonName(int reason)
   {
      switch (reason)
      {
      case ReasonSuperseded:
         return _T("superseded");
      case ReasonCessationOfOperation:
         return _T("cessationOfOperation");
      default:
         return _T("keyCompromise");
      }
   }

   // ---- backup --------------------------------------------------------------------------

   bool
   OrganisationCa::XMLStore(XNode *parentNode, __int64 domainId)
   {
      const __int64 now = Now();

      {
         std::lock_guard<std::mutex> guard(AuthorityLock());
         WipeSpentKeys(domainId, DomainNameOf(domainId), now);
      }

      std::vector<Authority> authorities;
      if (!ForDomain(domainId, authorities))
         return false;

      if (authorities.empty())
         return true;

      // The account each certificate was issued to, by the address a restore
      // finds it under again: the id is reissued, the address is not.
      std::map<__int64, String> accounts;
      {
         SQLCommand command("select accountid, accountaddress from hm_accounts where accountdomainid = @DOMAINID");
         command.AddParameter("@DOMAINID", domainId);
         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (!recordset)
            return false;
         while (!recordset->IsEOF())
         {
            accounts[recordset->GetInt64Value("accountid")] = recordset->GetStringValue("accountaddress");
            recordset->MoveNext();
         }
      }

      XNode *collection = parentNode->AppendChild(_T("SmimeAuthorities"));

      for (size_t i = 0; i < authorities.size(); i++)
      {
         const Authority &authority = authorities[i];

         // The key as it is stored: an envelope of the machine's secret store.
         // A retired root no longer published has no CRL left to sign, and its
         // key is not carried. Every value here is base64, hex or a number, so
         // no line break is lost to the attribute.
         const bool keyNeeded = authority.retired == 0 || IsPublished(authority, now);

         XNode *node = collection->AppendChild(_T("SmimeAuthority"));
         node->AppendAttr(_T("Certificate"), Wide(authority.certificate));
         node->AppendAttr(_T("Fingerprint"), Wide(authority.fingerprint));
         node->AppendAttr(_T("Key"), keyNeeded ? authority.protectedKey : String());
         node->AppendAttr(_T("Created"), StringParser::IntToString(authority.created));
         node->AppendAttr(_T("Retired"), StringParser::IntToString(authority.retired));
         node->AppendAttr(_T("Serial"), StringParser::IntToString(authority.serial));

         SQLCommand command(String(_T("select ")) + String(IssuedColumns) +
                            _T(" from hm_smimeissued i where i.issuedcaid = @CAID order by i.issuedid"));
         command.AddParameter("@CAID", authority.id);
         std::vector<Issued> issued;
         if (!ReadIssuedList(command, issued, 0))
            return false;

         for (size_t j = 0; j < issued.size(); j++)
         {
            const Issued &row = issued[j];
            std::map<__int64, String>::const_iterator owner = accounts.find(row.accountId);

            XNode *item = node->AppendChild(_T("Issued"));
            item->AppendAttr(_T("Account"), owner != accounts.end() ? owner->second : String());
            item->AppendAttr(_T("Address"), row.address);
            item->AppendAttr(_T("Serial"), StringParser::IntToString(row.serial));
            item->AppendAttr(_T("Fingerprint"), Wide(row.fingerprint));
            item->AppendAttr(_T("NotBefore"), StringParser::IntToString(row.notBefore));
            item->AppendAttr(_T("NotAfter"), StringParser::IntToString(row.notAfter));
            item->AppendAttr(_T("Revoked"), StringParser::IntToString(row.revoked));
            item->AppendAttr(_T("Reason"), row.reason);
            item->AppendAttr(_T("Certificate"), Wide(row.certificate));
            item->AppendAttr(_T("Pending"), StringParser::IntToString(row.pending));
         }
      }

      return true;
   }

   bool
   OrganisationCa::XMLLoad(XNode *parentNode, __int64 domainId)
   {
      XNode *collection = parentNode->GetChild(_T("SmimeAuthorities"));
      if (!collection)
         return true;

      // The accounts are restored by now (Domain::XMLLoadSubItems calls this
      // after them), under new ids.
      std::map<String, __int64> accounts;
      {
         SQLCommand command("select accountid, accountaddress from hm_accounts where accountdomainid = @DOMAINID");
         command.AddParameter("@DOMAINID", domainId);
         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (recordset)
         {
            while (!recordset->IsEOF())
            {
               String address = recordset->GetStringValue("accountaddress");
               address.MakeLower();
               accounts[address] = recordset->GetInt64Value("accountid");
               recordset->MoveNext();
            }
         }
      }

      const String domainName = DomainNameOf(domainId);
      int refused = 0;

      for (int i = 0; i < collection->GetChildCount(); i++)
      {
         XNode *node = collection->GetChild((unsigned int) i);
         if (!node)
            continue;

         // The root as the archive has it, parsed and written again from its
         // DER, and its fingerprint worked out here rather than read: the
         // archive is the administrator's input, and everything restored from
         // it reaches JSON a person is answered.
         CertificatePtr rootCertificate = ReadCertificate(Utf8(node->GetAttrValue(_T("Certificate"))));
         std::string rootDer;
         if (!rootCertificate || !DerOf(rootCertificate.get(), rootDer))
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7234, "OrganisationCa::XMLLoad",
               "An S/MIME authority root of " + domainName + " in the backup is not a certificate and was not restored.");
            continue;
         }

         const AnsiString certificate = EncodeBase64(rootDer);
         const AnsiString fingerprint = FingerprintOf(rootCertificate.get());

         // Only an envelope of a machine's secret store is restored as a key;
         // anything else - a legacy value, or no key because the root was spent -
         // restores the root without one.
         String protectedKey = node->GetAttrValue(_T("Key"));
         if (!IsMachineEnvelope(protectedKey))
            protectedKey = String();

         const __int64 created = _ttoi64(node->GetAttrValue(_T("Created")));
         const __int64 retired = _ttoi64(node->GetAttrValue(_T("Retired")));
         const __int64 serial = _ttoi64(node->GetAttrValue(_T("Serial")));

         __int64 caId = 0;
         if (!InsertAuthority(domainId, domainName, certificate, fingerprint, protectedKey, created, retired, serial > 0 ? serial : 1, caId))
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7234, "OrganisationCa::XMLLoad",
               "An S/MIME authority root of " + domainName + " could not be restored.");
            continue;
         }

         for (int j = 0; j < node->GetChildCount(); j++)
         {
            XNode *item = node->GetChild((unsigned int) j);
            if (!item)
               continue;

            Issued row;
            row.caId = caId;
            row.serial = _ttoi64(item->GetAttrValue(_T("Serial")));
            row.notBefore = _ttoi64(item->GetAttrValue(_T("NotBefore")));
            row.notAfter = _ttoi64(item->GetAttrValue(_T("NotAfter")));
            row.revoked = _ttoi64(item->GetAttrValue(_T("Revoked")));
            row.pending = (int) _ttoi64(item->GetAttrValue(_T("Pending")));
            if (row.pending != PendingSupersedes && row.pending != PendingKeeps)
               row.pending = PendingNone;

            Reason reason = ReasonKeyCompromise;
            row.reason = ParseReason(item->GetAttrValue(_T("Reason")), reason) ? ReasonName(reason) : String();

            // The certificate parsed and written again, its fingerprint worked out
            // from it. An erased row has neither certificate nor address, and
            // keeps the fingerprint it had if that is one.
            const AnsiString certificateText = Utf8(item->GetAttrValue(_T("Certificate")));
            if (!certificateText.IsEmpty())
            {
               CertificatePtr leaf = ReadCertificate(certificateText);
               std::string leafDer;
               if (!leaf || !DerOf(leaf.get(), leafDer))
               {
                  refused++;
                  continue;
               }
               row.certificate = EncodeBase64(leafDer);
               row.fingerprint = FingerprintOf(leaf.get());
            }
            else
            {
               row.fingerprint = Utf8(item->GetAttrValue(_T("Fingerprint")));
               if (!IsFingerprint(row.fingerprint))
               {
                  refused++;
                  continue;
               }
            }

            String address = item->GetAttrValue(_T("Address"));
            address.Trim();
            address.MakeLower();
            if (address.GetLength() > 255 || (!address.IsEmpty() && !StringParser::IsValidEmailAddress(address)))
            {
               refused++;
               continue;
            }
            row.address = address;

            String owner = item->GetAttrValue(_T("Account"));
            owner.MakeLower();
            std::map<String, __int64>::const_iterator account = owner.IsEmpty() ? accounts.end() : accounts.find(owner);
            row.accountId = account != accounts.end() ? account->second : 0;

            SQLStatement issued;
            issued.SetStatementType(SQLStatement::STInsert);
            issued.SetTable("hm_smimeissued");
            issued.SetIdentityColumn("issuedid");
            issued.AddColumnInt64("issuedcaid", row.caId);
            issued.AddColumnInt64("issuedaccountid", row.accountId);
            issued.AddColumn("issuedaddress", row.address);
            issued.AddColumnInt64("issuedserial", row.serial);
            issued.AddColumn("issuedfingerprint", Wide(row.fingerprint));
            issued.AddColumnInt64("issuednotbefore", row.notBefore);
            issued.AddColumnInt64("issuednotafter", row.notAfter);
            issued.AddColumnInt64("issuedrevoked", row.revoked);
            issued.AddColumn("issuedreason", row.reason);
            issued.AddColumn("issuedcertificate", Wide(row.certificate));
            issued.AddColumn("issuedpending", (long) row.pending);

            __int64 issuedId = 0;
            AuditDescription described(_T("hm_smimeissued"), NameOf(row));
            if (!Application::Instance()->GetDBManager()->Execute(issued, &issuedId))
               refused++;
         }
      }

      if (refused > 0)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7234, "OrganisationCa::XMLLoad",
            Formatter::Format(_T("{0} certificate(s) the S/MIME authority of {1} issued were not restored: in the backup they were not a certificate, a fingerprint or an address, or they could not be written."),
               refused, domainName));
      }

      return true;
   }
}
