// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// What a new installation starts with. See NewInstallDefaults.h, and
// docs/SecureDefaults.md for what an administrator reads.
//
// THE ORDER, and why it is this one. The certificate is made in memory first and
// written to disk second, before the database is touched: a key that could not
// be made or written leaves the server exactly as it was, and says so. Then the
// certificate's record; then the four listeners, each saved on its own so that
// one that fails is named and the rest still offer TLS; and last the Internet
// range, and only when every listener took the certificate - a range that
// refuses a password without TLS on a listener that cannot start TLS is a
// listener nobody outside this computer can sign in to.
//
// THE CERTIFICATE. RSA 2048, which every mail client of the last fifteen years
// takes, as the ACME client's own key is. 825 days, the longest a TLS server
// certificate may run for Apple's clients to take it even when a person has
// chosen to trust it. The subject names the host the server calls itself - the
// host name setting, else the machine's DNS name - and says what made it, so
// that the warning a mail client shows names something an administrator can
// recognise. basicConstraints CA:FALSE, digitalSignature and keyEncipherment,
// serverAuth, the host as a subjectAltName, a random 127-bit serial.

#include "StdAfx.h"
#include "NewInstallDefaults.h"

#include "Configuration.h"
#include "IniFileSettings.h"
#include "SampleDomain.h"
#include "../BO/Domain.h"
#include "../BO/SecurityRange.h"
#include "../BO/SecurityRanges.h"
#include "../BO/SSLCertificate.h"
#include "../BO/SSLCertificates.h"
#include "../BO/TCPIPPort.h"
#include "../BO/TCPIPPorts.h"
#include "../Persistence/PersistentSecurityRange.h"
#include "../Persistence/PersistentSSLCertificate.h"
#include "../Persistence/PersistentTCPIPPort.h"
#include "../Persistence/SchemaUpgrader.h"
#include "../SQL/DALConnection.h"
#include "../SQL/DALConnectionFactory.h"
#include "../SQL/DALRecordset.h"
#include "../SQL/DatabaseSettings.h"
#include "../SQL/SQLCommand.h"
#include "../TCPIP/SocketConstants.h"
#include "../Util/AuditTrail.h"
#include "../Util/FileUtilities.h"
#include "../Util/ProtectedFile.h"
#include "../Util/Unicode.h"
#include "../Util/Utilities.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#ifdef HM_PLATFORM_POSIX
#include <unistd.h>
#else
#include <aclapi.h>
#endif

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // The error codes of this unit (7510-7519).
      const int ErrorCertificateNotMade = 7510;
      const int ErrorNotAppliedInFull = 7511;

      // Set while the service applies the defaults at a start asked for with
      // --apply-new-install-defaults (ApplyAtServiceStart): a service has no
      // standard output, so every line Print_ says goes into the application log.
      bool say_in_application_log = false;

      // How long the certificate runs, in days: see the top of this file.
      const int CertificateValidityDays = 825;

      // The directory under the data directory the pair is written to, and the
      // two files. On the regression bench only the fixture
      // Security/NewInstallDefaults makes it, and removes it again, so
      // build/preflight-tests.ps1 names one it finds as that fixture's leftover.
      const TCHAR *const CertificateFolderName = _T("Certificates");

      // The file --create-database leaves beside the data, and the hm_settings row
      // it and COM's create methods write into the database they made
      // (NewInstallDefaults.h). The row's name fits hm_settings.settingname's 30.
      const TCHAR *const FirstStartMarkerName = _T(".new-installation");
      const TCHAR *const PendingRowName = _T("NewInstallDefaultsPending");
      const TCHAR *const CertificateFileName = _T("self-signed-certificate.pem");
      const TCHAR *const KeyFileName = _T("self-signed-key.pem");

      // The four listeners a new database has, by protocol and number, as
      // CreateTables*.sql inserts them and TCPIPPorts::SetDefault writes them back.
      struct ShippedListener
      {
         SessionType protocol;
         int number;
      };

      const ShippedListener ShippedListeners[] =
      {
         { STSMTP, 25 },
         { STSMTP, 587 },
         { STPOP3, 110 },
         { STIMAP, 143 }
      };

      const int ShippedListenerCount = (int) (sizeof(ShippedListeners) / sizeof(ShippedListeners[0]));

      // How long a mark stays a new installation's: a day from the create step
      // that wrote it. The installer runs the verb a minute after the create, and
      // the Linux unit at the first start; a mark older than this belongs to a
      // server that has run without the defaults since - a first start that never
      // reached its database, say - and whose clients may have come to depend on
      // listeners without TLS, so it is answered as not new.
      const __int64 MarkLifetimeSeconds = 24 * 60 * 60;

      // ---- ownership of what OpenSSL hands out ---------------------------------

      struct InstallKeyFree { void operator()(EVP_PKEY *p) const { EVP_PKEY_free(p); } };
      struct InstallX509Free { void operator()(X509 *p) const { X509_free(p); } };
      struct InstallNameFree { void operator()(X509_NAME *p) const { X509_NAME_free(p); } };
      struct InstallBioFree { void operator()(BIO *p) const { BIO_free(p); } };
      struct InstallBignumFree { void operator()(BIGNUM *p) const { BN_free(p); } };

      typedef std::unique_ptr<EVP_PKEY, InstallKeyFree> InstallKeyPtr;
      typedef std::unique_ptr<X509, InstallX509Free> InstallX509Ptr;
      typedef std::unique_ptr<X509_NAME, InstallNameFree> InstallNamePtr;
      typedef std::unique_ptr<BIO, InstallBioFree> InstallBioPtr;
      typedef std::unique_ptr<BIGNUM, InstallBignumFree> InstallBignumPtr;

      AnsiString NarrowOf(const String &value)
      {
         AnsiString out;
         Unicode::WideToMultiByte(value, out);
         return out;
      }

      String WideOf(const AnsiString &value)
      {
         String out;
         Unicode::MultiByteToWide(value, out);
         return out;
      }

      String UtcDay(__int64 seconds)
      {
         struct tm parts;
         memset(&parts, 0, sizeof(parts));
#ifdef HM_PLATFORM_POSIX
         time_t value = (time_t) seconds;
         if (::gmtime_r(&value, &parts) == nullptr)
            return String();
#else
         __time64_t value = seconds;
         if (_gmtime64_s(&parts, &value) != 0)
            return String();
#endif
         AnsiString text;
         text.Format("%04d-%02d-%02d", parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday);
         return WideOf(text);
      }

      // The name the certificate is made for: the host name setting - the name
      // the server gives itself in its greetings - else the machine's DNS name,
      // else its plain name; lower case, and nothing a DNS name cannot hold.
      // localhost when nothing usable is left.
      AnsiString CertificateHostName()
      {
         String configured = Configuration::Instance()->GetHostName();
         configured.Trim();

         String name = configured;
#ifndef HM_PLATFORM_POSIX
         if (name.IsEmpty())
         {
            wchar_t buffer[256];
            DWORD size = (DWORD) (sizeof(buffer) / sizeof(buffer[0]));
            if (GetComputerNameExW(ComputerNameDnsFullyQualified, buffer, &size) && size > 0)
               name = String(buffer);
         }
#endif
         if (name.IsEmpty())
            name = Utilities::ComputerName();

         const AnsiString narrow = NarrowOf(name);
         const char *chars = narrow.c_str();

         std::string host;
         for (int i = 0; i < narrow.GetLength(); i++)
         {
            char c = chars[i];
            if (c >= 'A' && c <= 'Z')
               c = (char) (c - 'A' + 'a');

            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-')
               host += c;
         }

         while (!host.empty() && (host.front() == '.' || host.front() == '-'))
            host.erase(0, 1);
         while (!host.empty() && (host.back() == '.' || host.back() == '-'))
            host.pop_back();

         // 253 is the longest a DNS name may be written.
         if (host.empty() || host.size() > 253)
            host = "localhost";

         return AnsiString(host.c_str());
      }

      bool AddInstallExtension(X509 *certificate, int nid, const char *value)
      {
         X509V3_CTX context;
         X509V3_set_ctx_nodb(&context);
         X509V3_set_ctx(&context, certificate, certificate, nullptr, nullptr, 0);

         X509_EXTENSION *extension = X509V3_EXT_nconf_nid(nullptr, &context, nid, value);
         if (extension == nullptr)
            return false;

         const bool added = X509_add_ext(certificate, extension, -1) == 1;
         X509_EXTENSION_free(extension);
         return added;
      }

      bool AddInstallNameEntry(X509_NAME *name, int nid, const AnsiString &value)
      {
         return X509_NAME_add_entry_by_NID(name, nid, MBSTRING_UTF8,
                                           (const unsigned char *) value.c_str(), value.GetLength(), -1, 0) == 1;
      }

      bool PemOfBio(BIO *memory, AnsiString &pem)
      {
         char *data = nullptr;
         const long length = BIO_get_mem_data(memory, &data);
         if (length <= 0 || data == nullptr)
            return false;

         pem = AnsiString(data, (int) length);
         return true;
      }

      // The key and the certificate, as PEM: the key PKCS#8 and unencrypted, as
      // the ACME client writes its own and as SslContextInitializer reads one
      // with no passphrase set on its record.
      bool MakeSelfSignedPair(const AnsiString &host, __int64 notBefore, __int64 notAfter,
                              AnsiString &keyPem, AnsiString &certificatePem)
      {
         InstallKeyPtr key(EVP_RSA_gen(2048));
         InstallX509Ptr certificate(X509_new());
         InstallNamePtr name(X509_NAME_new());
         InstallBignumPtr serial(BN_new());
         if (!key || !certificate || !name || !serial)
         {
            ERR_clear_error();
            return false;
         }

         // A commonName holds 64 characters; a longer host is in the
         // subjectAltName alone, which is what a client checks in any case.
         const AnsiString commonName = host.GetLength() <= 64 ? host : AnsiString("hMailServer");
         const AnsiString alternativeName = "DNS:" + host;

         bool ok = X509_set_version(certificate.get(), X509_VERSION_3) == 1 &&
                   BN_rand(serial.get(), 127, BN_RAND_TOP_ANY, BN_RAND_BOTTOM_ANY) == 1 &&
                   BN_to_ASN1_INTEGER(serial.get(), X509_get_serialNumber(certificate.get())) != nullptr &&
                   AddInstallNameEntry(name.get(), NID_commonName, commonName) &&
                   AddInstallNameEntry(name.get(), NID_organizationName, AnsiString("hMailServer, self-signed at installation")) &&
                   X509_set_subject_name(certificate.get(), name.get()) == 1 &&
                   X509_set_issuer_name(certificate.get(), name.get()) == 1 &&
                   ASN1_TIME_set(X509_getm_notBefore(certificate.get()), (time_t) notBefore) != nullptr &&
                   ASN1_TIME_set(X509_getm_notAfter(certificate.get()), (time_t) notAfter) != nullptr &&
                   X509_set_pubkey(certificate.get(), key.get()) == 1 &&
                   AddInstallExtension(certificate.get(), NID_basic_constraints, "critical,CA:FALSE") &&
                   AddInstallExtension(certificate.get(), NID_key_usage, "critical,digitalSignature,keyEncipherment") &&
                   AddInstallExtension(certificate.get(), NID_ext_key_usage, "serverAuth") &&
                   AddInstallExtension(certificate.get(), NID_subject_alt_name, alternativeName.c_str()) &&
                   AddInstallExtension(certificate.get(), NID_subject_key_identifier, "hash") &&
                   X509_sign(certificate.get(), key.get(), EVP_sha256()) > 0;

         if (!ok)
         {
            ERR_clear_error();
            return false;
         }

         InstallBioPtr keyMemory(BIO_new(BIO_s_mem()));
         InstallBioPtr certificateMemory(BIO_new(BIO_s_mem()));
         ok = keyMemory && certificateMemory &&
              PEM_write_bio_PrivateKey(keyMemory.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) == 1 &&
              PEM_write_bio_X509(certificateMemory.get(), certificate.get()) == 1 &&
              PemOfBio(keyMemory.get(), keyPem) &&
              PemOfBio(certificateMemory.get(), certificatePem);

         if (!ok)
         {
            ERR_clear_error();
            Utilities::SecureWipe(keyPem);
            return false;
         }

         return true;
      }

#ifndef HM_PLATFORM_POSIX
      // The account the hMailServer service is registered to log on as, from the
      // service controller: empty for LocalSystem, which ProtectedFile's SYSTEM
      // entry covers, and when there is no service or it cannot be asked.
      //
      // Why it is asked at all: the key is written by this process, which the
      // installer runs as the administrator who installs, and its DACL names
      // SYSTEM, the Administrators group and this process's own account - and
      // this account, given Read when the file is made (HeldFolder). A service
      // that logs on as any other account - a virtual account (NT SERVICE\...), or
      // the account ServiceAccountName names - could not read the key, and every
      // listener given the certificate would fail to start.
      String ServiceLogOnAccount()
      {
         String account;

         SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
         if (manager == nullptr)
            return account;

         SC_HANDLE service = OpenServiceW(manager, L"hMailServer", SERVICE_QUERY_CONFIG);
         if (service != nullptr)
         {
            DWORD needed = 0;
            QueryServiceConfigW(service, nullptr, 0, &needed);
            if (needed > 0)
            {
               std::vector<BYTE> buffer(needed);
               QUERY_SERVICE_CONFIGW *config = (QUERY_SERVICE_CONFIGW *) &buffer[0];
               if (QueryServiceConfigW(service, config, needed, &needed) && config->lpServiceStartName != nullptr)
                  account = String(config->lpServiceStartName);
            }
            CloseServiceHandle(service);
         }
         CloseServiceHandle(manager);

         account.Trim();
         if (account.CompareNoCase(_T("LocalSystem")) == 0 ||
             account.CompareNoCase(_T(".\\LocalSystem")) == 0 ||
             account.CompareNoCase(_T("NT AUTHORITY\\SYSTEM")) == 0)
            account.Empty();

         // The service controller writes a local account as .\name, which
         // LookupAccountName does not resolve: the dot is this machine.
         if (account.GetLength() > 2 && account.Left(2) == _T(".\\"))
         {
            wchar_t computer[MAX_COMPUTERNAME_LENGTH + 1];
            DWORD size = (DWORD) (sizeof(computer) / sizeof(computer[0]));
            if (GetComputerNameW(computer, &size) && size > 0)
               account = String(computer) + account.Mid(1);
         }

         return account;
      }

#endif

      String JoinPorts(const std::vector<int> &ports)
      {
         String text;
         for (size_t i = 0; i < ports.size(); i++)
         {
            if (i > 0)
               text += _T(", ");
            text += StringParser::IntToString(ports[i]);
         }
         return text;
      }

      // The one count(*) a command selects as column, or false when it could not
      // be read - which is not the same answer as none.
      bool CountOf(const SQLCommand &command, const char *column, long long &count)
      {
         count = 0;

         std::shared_ptr<DatabaseConnectionManager> database = Application::Instance()->GetDBManager();
         std::shared_ptr<DALRecordset> recordset = database ? database->OpenRecordset(command) : std::shared_ptr<DALRecordset>();
         if (!recordset || recordset->IsEOF())
            return false;

         count = recordset->GetInt64Value(column);
         return true;
      }

      // The time MarkCreated writes into the mark's settingstring: Unix seconds,
      // digits only. 0 when there is none or it is not that - a mark this build's
      // create step did not write.
      __int64 MarkTime(const String &text)
      {
         if (text.IsEmpty() || text.GetLength() > 18)
            return 0;

         __int64 value = 0;
         for (int i = 0; i < text.GetLength(); i++)
         {
            const wchar_t character = text[i];
            if (character < L'0' || character > L'9')
               return 0;

            value = value * 10 + (character - L'0');
         }

         return value;
      }
   }

   const TCHAR *
   NewInstallDefaults::CertificateName()
   {
      return _T("Self-signed (made at installation)");
   }

   bool
   NewInstallDefaults::IsInstallationCertificate(const String &name)
   {
      return name.CompareNoCase(CertificateName()) == 0;
   }

   String
   NewInstallDefaults::CertificateDirectory()
   {
      return FileUtilities::Combine(IniFileSettings::Instance()->GetDataDirectory(), CertificateFolderName);
   }

   const TCHAR *
   NewInstallDefaults::PendingName()
   {
      return PendingRowName;
   }

   String
   NewInstallDefaults::FirstStartMarkerFile()
   {
      // DataFolder read from the file alone, resolved as LoadSettings resolves it:
      // the same answer for --create-database, which has loaded the configuration,
      // and for the unit's step before every start, which loads nothing - so that
      // the loader's reports are not written again at every start.
      return FileUtilities::Combine(IniFileSettings::Instance()->ReadDataDirectory(), FirstStartMarkerName);
   }

   bool
   NewInstallDefaults::MarkCreated(std::shared_ptr<DALConnection> connection, String &error)
   {
      if (!connection)
      {
         error = _T("there is no connection to the new database");
         return false;
      }

      // With the time it was made, in Unix seconds: the verb answers a mark older
      // than MarkLifetimeSeconds as not new.
      SQLCommand command(_T("insert into hm_settings (settingname, settingstring, settinginteger) values (@SETTINGNAME, @CREATED, 1)"));
      command.AddParameter("@SETTINGNAME", String(PendingRowName));
      command.AddParameter("@CREATED", StringParser::IntToString((__int64) time(nullptr)));
      return connection->Execute(command, error);
   }

   bool
   NewInstallDefaults::ClearMark_()
   {
      SQLCommand command(_T("delete from hm_settings where settingname = @SETTINGNAME"));
      command.AddParameter("@SETTINGNAME", String(PendingRowName));
      return Application::Instance()->GetDBManager()->Execute(command);
   }

   void
   NewInstallDefaults::Print_(bool error, const String &line)
   {
      if (say_in_application_log)
      {
         LOG_APPLICATION(_T("New installation: ") + line);
         return;
      }

      AnsiString text = NarrowOf(line);
      text += "\n";

#ifdef HM_PLATFORM_POSIX
      std::fputs(text.c_str(), error ? stderr : stdout);
      std::fflush(error ? stderr : stdout);
#else
      HANDLE handle = GetStdHandle(error ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
      if (handle && handle != INVALID_HANDLE_VALUE)
      {
         DWORD written = 0;
         WriteFile(handle, text.c_str(), (DWORD) text.GetLength(), &written, nullptr);
      }
#endif
   }

   int
   NewInstallDefaults::OpenForCommand_()
   {
      IniFileSettings::Instance()->LoadSettings();
      if (!IniFileSettings::Instance()->GetDatabaseSettingsExists())
      {
         Print_(true, Formatter::Format("[Database] in {0} does not name a database, so there is no installation to secure yet. Nothing was changed.",
            IniFileSettings::GetInitializationFile()));
         return 2;
      }

      // The whole of a start short of the listeners, as the profile verb makes it.
      String error;
      if (!Application::Instance()->InitInstance(error))
      {
         if (error.IsEmpty())
            error = Application::Instance()->GetLastErrorMessage();

         Print_(true, Formatter::Format("The configuration could not be read and the database opened: {0} Nothing was changed.", error));
         Application::Instance()->ExitInstance();
         return 2;
      }

      return 0;
   }

   bool
   NewInstallDefaults::IsNewInstallation_(bool ifMarked, String &why, bool &marked, bool &unknown, bool &tooOld)
   {
      tooOld = false;

      // A read that fails is not an answer. The caller gives up without judging -
      // the mark and the Linux marker kept - rather than take "could not read" for
      // "not new" and so spend the one decision on a database hiccup.
      unknown = false;

      // First the mark the create path left: a database this installation made
      // and whose defaults are not decided yet. Without it the database existed
      // before - upgraded, attached rather than created, or decided already - and
      // no shape it has makes it new: a small server in service looks just like
      // one.
      marked = false;
      {
         SQLCommand command(_T("select settinginteger, settingstring from hm_settings where settingname = @SETTINGNAME"));
         command.AddParameter("@SETTINGNAME", String(PendingRowName));

         std::shared_ptr<DatabaseConnectionManager> database = Application::Instance()->GetDBManager();
         std::shared_ptr<DALRecordset> recordset = database ? database->OpenRecordset(command) : std::shared_ptr<DALRecordset>();
         if (!recordset)
         {
            why = _T("whether this installation created the database could not be read");
            unknown = true;
            return false;
         }

         marked = !recordset->IsEOF();
         if (!marked || recordset->GetLongValue("settinginteger") != 1)
         {
            why = Formatter::Format("the database was not created by this installation - it has no {0} mark, which only the create step writes: it was upgraded, attached rather than created, or its defaults were decided before",
               String(PendingRowName));
            return false;
         }

         // At the unit's start (--if-marked), made within a day of this machine's
         // clock (MarkLifetimeSeconds), either way: a mark from further back belongs
         // to a server that has run without the defaults since, and one from further
         // ahead to a clock that was wrong then or is now, which cannot say. A mark
         // with no time was not written by this build's create step. A run by hand,
         // and the installer's a minute after its create, has no such limit: it is
         // somebody asking for the defaults now.
         if (ifMarked)
         {
            const __int64 created = MarkTime(recordset->GetStringValue("settingstring"));
            const __int64 now = (__int64) time(nullptr);
            if (created <= 0 || now - created > MarkLifetimeSeconds || created - now > MarkLifetimeSeconds)
            {
               tooOld = true;
               if (created <= 0)
                  why = _T("its mark carries no time, so when the database was made is not known");
               else if (created > now)
                  why = Formatter::Format("its mark says the database was made on {0}, more than a day ahead of this machine's clock, which was wrong then or is now", UtcDay(created));
               else
                  why = Formatter::Format("the database was made on {0}, more than a day ago, and its server may have run without these defaults since", UtcDay(created));
               return false;
            }
         }
      }

      // The listeners: the four a new database has, on every address, with no
      // TLS, no certificate and no client certificate asked for.
      TCPIPPorts ports;
      if (!ports.Refresh())
      {
         why = _T("its TCP/IP ports could not be read");
         unknown = true;
         return false;
      }

      if (ports.GetCount() != (int) (sizeof(ShippedListeners) / sizeof(ShippedListeners[0])))
      {
         why = Formatter::Format("it has {0} TCP/IP ports where a new database has 4", ports.GetCount());
         return false;
      }

      for (const ShippedListener &shipped : ShippedListeners)
      {
         bool found = false;
         for (int i = 0; i < ports.GetCount(); i++)
         {
            std::shared_ptr<TCPIPPort> port = ports.GetItem(i);
            if (!port || port->GetProtocol() != shipped.protocol || port->GetPortNumber() != shipped.number)
               continue;

            if (!port->GetAddress().IsAny() || port->GetConnectionSecurity() != CSNone ||
                port->GetSSLCertificateID() != 0 || port->GetClientCertificatePolicy() != CCPOff ||
                port->GetSMTPAuthentication() != SAMOn)
            {
               why = Formatter::Format("port {0} has been configured since the database was made", shipped.number);
               return false;
            }

            found = true;
            break;
         }

         if (!found)
         {
            why = Formatter::Format("it has no port {0} as a new database has", shipped.number);
            return false;
         }
      }

      // The two IP ranges, with the options, the priorities and the addresses the
      // create scripts give them, and neither expiring.
      SecurityRanges ranges;
      if (!ranges.Refresh())
      {
         why = _T("its IP ranges could not be read");
         unknown = true;
         return false;
      }

      if (ranges.GetCount() != 2)
      {
         why = Formatter::Format("it has {0} IP ranges where a new database has 2", ranges.GetCount());
         return false;
      }

      bool local = false;
      bool internet = false;
      for (int i = 0; i < ranges.GetCount(); i++)
      {
         std::shared_ptr<SecurityRange> range = ranges.GetItem(i);
         if (!range || range->GetExpires())
            continue;

         const String lower = range->GetLowerIPString();
         const String upper = range->GetUpperIPString();

         if (lower.CompareNoCase(_T("127.0.0.1")) == 0 && upper.CompareNoCase(_T("127.0.0.1")) == 0 &&
             range->GetOptions() == SecurityRanges::ShippedLocalOptions && range->GetPriority() == SecurityRanges::ShippedLocalPriority)
            local = true;
         else if (lower.CompareNoCase(_T("0.0.0.0")) == 0 && upper.CompareNoCase(_T("255.255.255.255")) == 0 &&
                  range->GetOptions() == SecurityRanges::ShippedInternetOptions && range->GetPriority() == SecurityRanges::ShippedInternetPriority)
            internet = true;
      }

      if (!local || !internet)
      {
         why = _T("its IP ranges are not the two a new database has");
         return false;
      }

      // No certificate: a server somebody has given one is a server whose TLS
      // somebody has thought about.
      SSLCertificates certificates;
      if (!certificates.Refresh())
      {
         why = _T("its SSL certificates could not be read");
         unknown = true;
         return false;
      }

      if (certificates.GetCount() != 0)
      {
         why = Formatter::Format("it has {0} SSL certificates, and a new database has none", certificates.GetCount());
         return false;
      }

      // The sample domain (Application/SampleDomain.h), which a new installation
      // may already have been given, and whose domain and accounts are not
      // counted below. Asked first and on its own: a domain table that cannot be
      // read is not an answer, where the sample's id taken as 0 - as the
      // checklist's count takes it - would count its three mailboxes as the
      // administrator's and call a new installation one in service.
      std::shared_ptr<Domain> sample;
      const SampleDomain::State sampleState = SampleDomain::Examine(sample);
      if (sampleState == SampleDomain::StateUnknown)
      {
         why = _T("its domains could not be read");
         unknown = true;
         return false;
      }

      const __int64 sampleId = sampleState == SampleDomain::StatePresent && sample ? sample->GetID() : 0;

      // No account yet: a database the create step has just made has none, and
      // one that has any has somebody signing in to it already.
      SQLCommand accountCommand(sampleId == 0
         ? _T("select count(*) as accountcount from hm_accounts")
         : _T("select count(*) as accountcount from hm_accounts where accountdomainid <> @DOMAINID"));
      if (sampleId != 0)
         accountCommand.AddParameter("@DOMAINID", sampleId);

      long long accounts = 0;
      if (!CountOf(accountCommand, "accountcount", accounts))
      {
         why = _T("its accounts could not be counted");
         unknown = true;
         return false;
      }

      if (accounts > 0)
      {
         why = Formatter::Format("it holds {0} account(s), so somebody signs in to it already", (__int64) accounts);
         return false;
      }

      // No domain and no route either: none is in a create script, and a server
      // with no account but a domain or a route is a gateway or a relay in
      // service, which a mark left behind by a verb that never ran must not make
      // new.
      SQLCommand domainCommand(sampleId == 0
         ? _T("select count(*) as domaincount from hm_domains")
         : _T("select count(*) as domaincount from hm_domains where domainid <> @DOMAINID"));
      if (sampleId != 0)
         domainCommand.AddParameter("@DOMAINID", sampleId);

      long long domains = 0;
      if (!CountOf(domainCommand, "domaincount", domains))
      {
         why = _T("its domains could not be counted");
         unknown = true;
         return false;
      }

      if (domains > 0)
      {
         why = Formatter::Format("it holds {0} domain(s), and a new database has none", (__int64) domains);
         return false;
      }

      long long routes = 0;
      if (!CountOf(SQLCommand(_T("select count(*) as routecount from hm_routes")), "routecount", routes))
      {
         why = _T("its routes could not be counted");
         unknown = true;
         return false;
      }

      if (routes > 0)
      {
         why = Formatter::Format("it has {0} route(s), and a new database has none", (__int64) routes);
         return false;
      }

      return true;
   }

   int
   NewInstallDefaults::Apply_()
   {
      // 0. What steps 4 and 5 change, read before anything is made. A read that
      // fails - or a listener list that comes back short - changes nothing and is
      // not a verdict: exit 2, the mark kept, the next run asks again. Read
      // later, an empty list would give no port the certificate and no port
      // refused, and step 5 would then require TLS on a range with no listener
      // offering it.
      TCPIPPorts ports;
      SecurityRanges ranges;
      if (!ports.Refresh() || ports.GetCount() != ShippedListenerCount || !ranges.Refresh())
      {
         const String sentence = _T("The listeners and the IP ranges could not be read again before they were to be changed, so nothing was changed, and the next run asks again.");
         Print_(true, sentence);
         LOG_APPLICATION(sentence);
         return 2;
      }

      const AnsiString host = CertificateHostName();
      const __int64 now = (__int64) time(nullptr);
      const __int64 notAfter = now + (__int64) CertificateValidityDays * 86400;

      // 1. The pair, in memory.
      AnsiString keyPem;
      AnsiString certificatePem;
      if (!MakeSelfSignedPair(host, now - 300, notAfter, keyPem, certificatePem))
      {
         const String sentence = _T("The self-signed certificate could not be made: OpenSSL refused the key or the certificate. Nothing was changed, and the listeners are as a new database leaves them, without TLS.");
         Print_(true, sentence);
         ErrorManager::Instance()->ReportError(ErrorManager::High, ErrorCertificateNotMade, "NewInstallDefaults::Apply_", sentence);
         return 1;
      }

      // 2. The pair, on disk. The key readable by this server's account and the
      // administrators only (ProtectedFile), the certificate as any file there.
      const String directory = CertificateDirectory();
      const String keyFile = FileUtilities::Combine(directory, KeyFileName);
      const String certificateFile = FileUtilities::Combine(directory, CertificateFileName);

      String problem;
#ifndef HM_PLATFORM_POSIX
      // On Windows this runs elevated - the installer's, DBSetup's or an
      // administrator's - and the data folder is the service's to write (Modify
      // for NT SERVICE\hMailServer, docs/ServiceAccount.md), so nothing here goes
      // by the folder's path: it is held from the drive's root down, no folder
      // of it a junction, a link or a mount point (HeldFolder), and each file is
      // made below the held Certificates folder and checked to be there before
      // it is written. Until 6.3.6 the pair was written, and the key's
      // permissions changed, by path. The key's DACL names, from the start,
      // SYSTEM, the Administrators, this process's account and - when it is not
      // LocalSystem and not this process's - the service's own account, which
      // reads it (ServiceLogOnAccount).
      HeldFolder held;
      bool written = held.Hold(directory, true, problem);
      if (written)
         written = held.WriteNewFile(KeyFileName, keyPem, true, ServiceLogOnAccount(), problem);

      Utilities::SecureWipe(keyPem);

      if (written && !held.WriteNewFile(CertificateFileName, certificatePem, false, String(), problem))
      {
         written = false;
         held.RemoveFile(KeyFileName);
      }
#else
      bool written = FileUtilities::CreateDirectory(directory);
      if (!written)
         problem = Formatter::Format("The directory {0} could not be made.", directory);
      else
         written = ProtectedFile::Write(keyFile, keyPem, problem);

      Utilities::SecureWipe(keyPem);

      // On Linux the unit runs this as the service's user, which ProtectedFile's
      // 0600 already names.
      if (written && !FileUtilities::WriteToFile(certificateFile, certificatePem))
      {
         written = false;
         problem = Formatter::Format("{0} could not be written.", certificateFile);
         FileUtilities::DeleteFile(keyFile);
      }
#endif

      if (!written)
      {
         const String sentence = Formatter::Format("The self-signed certificate could not be written: {0} Nothing was changed in the database, and the listeners are as a new database leaves them, without TLS.", problem);
         Print_(true, sentence);
         ErrorManager::Instance()->ReportError(ErrorManager::High, ErrorCertificateNotMade, "NewInstallDefaults::Apply_", sentence);
         return 1;
      }

      // 3. Its record.
      std::shared_ptr<SSLCertificate> certificate = std::shared_ptr<SSLCertificate>(new SSLCertificate());
      certificate->SetName(CertificateName());
      certificate->SetCertificateFile(certificateFile);
      certificate->SetPrivateKeyFile(keyFile);

      if (!PersistentSSLCertificate::SaveObject(certificate) || certificate->GetID() <= 0)
      {
#ifndef HM_PLATFORM_POSIX
         held.RemoveFile(KeyFileName);
         held.RemoveFile(CertificateFileName);
#else
         FileUtilities::DeleteFile(keyFile);
         FileUtilities::DeleteFile(certificateFile);
#endif

         const String sentence = _T("The self-signed certificate was made, but its record could not be written to the database, so it was deleted again. Nothing was changed, and the listeners are as a new database leaves them, without TLS.");
         Print_(true, sentence);
         ErrorManager::Instance()->ReportError(ErrorManager::High, ErrorNotAppliedInFull, "NewInstallDefaults::Apply_", sentence);
         return 1;
      }

      Print_(false, Formatter::Format("Made a self-signed certificate for {0}, valid until {1}, as the record \"{2}\": {3}, with its key in {4}, which only this server's account and the administrators can read.",
         WideOf(host), UtcDay(notAfter), String(CertificateName()), certificateFile, keyFile));

      // 4. The listeners read in step 0, each on its own. Port 25 also becomes a
      // honeypot for AUTH (GitLab work item #54, docs/SmtpAuthOnDeliveryPorts.md):
      // it is where other servers deliver, and no client of a new installation
      // has ever been told to sign in there, so EHLO goes on offering AUTH and an
      // AUTH is refused without the password being looked at and bans the
      // address. 587 is where clients submit, and stays On. In the same save as
      // the certificate, so a port is changed in full or not at all.
      std::vector<int> offered;
      std::vector<int> refused;
      bool honeypot = false;
      String firstRefusal;
      for (int i = 0; i < ports.GetCount(); i++)
      {
         std::shared_ptr<TCPIPPort> port = ports.GetItem(i);
         if (!port)
            continue;

         const bool deliveryPort = port->GetProtocol() == STSMTP && port->GetPortNumber() == 25;

         port->SetConnectionSecurity(CSSTARTTLSOptional);
         port->SetSSLCertificateID((int) certificate->GetID());
         if (deliveryPort)
            port->SetSMTPAuthentication(SAMHoneypot);

         String error;
         if (PersistentTCPIPPort::SaveObject(port, error, PersistenceModeNormal))
         {
            offered.push_back(port->GetPortNumber());
            if (deliveryPort)
               honeypot = true;
         }
         else
         {
            refused.push_back(port->GetPortNumber());
            if (firstRefusal.IsEmpty())
               firstRefusal = error.IsEmpty() ? String(_T("the database did not take the change")) : error;
         }
      }

      std::sort(offered.begin(), offered.end());
      std::sort(refused.begin(), refused.end());

      if (!offered.empty())
         Print_(false, Formatter::Format("STARTTLS is offered on port {0} with it, and optional: a client or a server that cannot start TLS is still served.", JoinPorts(offered)));

      if (honeypot)
         Print_(false, _T("Port 25 takes no password: its SMTP authentication is Honeypot, so EHLO offers AUTH there, an AUTH is refused without the password being looked at, and the address that sent it is banned for AutoBanMinutes. Mail clients sign in on port 587."));

      if (!refused.empty())
      {
         const String sentence = Formatter::Format("Port {0} could not be given the certificate ({1}), so it still has no TLS; and so the Internet range was left taking a password without TLS, since on that port a client outside this computer could not sign in at all. Give the port the certificate \"{2}\" with STARTTLS, then set the range to require TLS for authentication; the setup checklist says what is left.",
            JoinPorts(refused), firstRefusal, String(CertificateName()));
         Print_(true, sentence);
         ErrorManager::Instance()->ReportError(ErrorManager::High, ErrorNotAppliedInFull, "NewInstallDefaults::Apply_", sentence);
         return 1;
      }

      // Every one of the four offers it, or the range is left alone: the range's
      // rule is only safe behind listeners that can all start TLS.
      if ((int) offered.size() != ShippedListenerCount)
      {
         const String sentence = Formatter::Format("The certificate was given to {0} of the four listeners a new database has ({1}), so the Internet range was left taking a password without TLS. Give every listener the certificate \"{2}\" with STARTTLS, then set the range to require TLS for authentication; the setup checklist says what is left.",
            (int) offered.size(), offered.empty() ? String(_T("none")) : JoinPorts(offered), String(CertificateName()));
         Print_(true, sentence);
         ErrorManager::Instance()->ReportError(ErrorManager::High, ErrorNotAppliedInFull, "NewInstallDefaults::Apply_", sentence);
         return 1;
      }

      // 5. The Internet range read in step 0: a password only over TLS. The
      // loopback range is left as it is, so that this computer's own clients - a
      // webmail on the same machine, a script - sign in as they always have.
      std::shared_ptr<SecurityRange> internet;
      for (int i = 0; i < ranges.GetCount(); i++)
      {
         std::shared_ptr<SecurityRange> range = ranges.GetItem(i);
         if (range && range->GetOptions() == SecurityRanges::ShippedInternetOptions &&
             range->GetLowerIPString().CompareNoCase(_T("0.0.0.0")) == 0)
         {
            internet = range;
            break;
         }
      }

      String rangeError;
      if (internet)
      {
         internet->SetRequireTLSForAuth(true);
         if (!PersistentSecurityRange::SaveObject(internet, rangeError, PersistenceModeNormal))
            internet.reset();
      }

      if (!internet)
      {
         const String sentence = Formatter::Format("The Internet range could not be set to require TLS for authentication ({0}), so it still takes a password without TLS from any address. Set it on the range; the setup checklist's \"Passwords only over TLS\" does it in one click.",
            rangeError.IsEmpty() ? String(_T("it was not found")) : rangeError);
         Print_(true, sentence);
         ErrorManager::Instance()->ReportError(ErrorManager::High, ErrorNotAppliedInFull, "NewInstallDefaults::Apply_", sentence);
         return 1;
      }

      Print_(false, Formatter::Format("The IP range \"{0}\" now takes a password only over TLS: from any address but this computer's, SMTP AUTH, IMAP LOGIN and POP3 USER are refused until STARTTLS. \"My computer\" is unchanged.",
         internet->GetName()));
      Print_(false, _T("Mail clients warn about a self-signed certificate. Replace it with one a certificate authority issued: ACME gets one from Let's Encrypt and gives it to these ports itself (docs/SecureDefaults.md)."));

      LOG_APPLICATION(Formatter::Format("New installation: a self-signed certificate for {0} was made, STARTTLS is offered on port {1}, the IP range \"{2}\" requires TLS for authentication, and port 25's SMTP authentication is {3}.",
         WideOf(host), JoinPorts(offered), internet->GetName(), String(honeypot ? _T("Honeypot") : _T("On"))));

      return 0;
   }

   int
   NewInstallDefaults::RunCommand(bool ifMarked, const String &namedMarker)
   {
#ifdef HM_PLATFORM_POSIX
      // Not as root. The key is written 0600 to whoever runs this, and a key root
      // owns is one the service's user cannot open: every listener given the
      // certificate would then fail to start. Refused before anything is read or
      // opened, so that nothing - a log file included - is left owned by root.
      if (::geteuid() == 0)
      {
         Print_(true, _T("Run this as the user the service runs as, whose key it writes - sudo -u hmailserver hmailserver --apply-new-install-defaults - not as root: a key root owns is one the service cannot read. Nothing was changed."));
         return 2;
      }
#endif

      // The marker (--if-marked): the file named, or else the one --create-database
      // leaves when it makes the database - FirstStartMarkerFile(), in the data
      // directory of the configuration this reads, which is the configuration that
      // step read when it wrote it. Only DataFolder and ProgramFolder are read to
      // find it, not the whole configuration, whose loader would write its reports
      // (HM5528, HM5607 and the like) to the error log a second time at every
      // start. A start of the service with no marker is every start but the first
      // after a new installation, and it is answered without so much as opening
      // the database.
      String markerFile;
      if (ifMarked)
      {
         markerFile = namedMarker.IsEmpty() ? FirstStartMarkerFile() : namedMarker;

         if (!FileUtilities::Exists(markerFile))
            return 0;
      }

      // At a first start, one attempt at the database before the whole start short
      // of the listeners, which retries ConnectionAttempts times and would hold the
      // unit's start for as long: a database that cannot be reached now is left
      // for the next start, the marker with it, and the server's own start says
      // what is wrong with it.
      if (ifMarked)
      {
         IniFileSettings::Instance()->LoadSettings();
         if (IniFileSettings::Instance()->GetDatabaseSettingsExists())
         {
            std::shared_ptr<DALConnection> probe = DALConnectionFactory::CreateConnection(SchemaUpgrader::SettingsFromIni());
            String probeError;
            if (!probe || probe->Connect(probeError) != DALConnection::Connected)
            {
               Print_(true, Formatter::Format("The database could not be reached ({0}), so a new installation's defaults are left for the next start. Nothing was changed.", probeError));
               return 2;
            }
            probe->Disconnect();
         }
      }

      const int opened = OpenForCommand_();
      if (opened != 0)
         return opened;

      int result = 0;
      bool marked = false;
      {
         // The command line is an administrator of its own: every row this
         // writes goes on the audit trail under this name.
         AuditTrail::Actor actor;
         actor.present = true;
         actor.name = _T("hmailserver --apply-new-install-defaults");
         actor.kind = _T("system");
         actor.interface_name = _T("command line");
         AuditScope scope(actor);

         String why;
         bool unknown = false;
         bool tooOld = false;
         if (IsNewInstallation_(ifMarked, why, marked, unknown, tooOld))
            result = Apply_();
         else if (tooOld)
         {
            // Not acted on at the unit's start, and left for a run by hand: the
            // mark stays, and the file goes, so that later starts do not ask again.
            const String sentence = Formatter::Format("The first start does not change this database - {0} - so its listeners, its IP ranges and its certificates are left as they are. Its mark is kept: to apply a new installation's defaults to it now, stop the server and run hmailserver --apply-new-install-defaults as the user it runs as; the setup checklist says whether a password can still cross the network in the clear.", why);
            Print_(false, sentence);
            LOG_APPLICATION(sentence);
            result = 5;
         }
         else if (unknown)
         {
            // Not judged: the mark and the marker stay, and the next run asks again.
            const String sentence = Formatter::Format("Whether this is a new installation's database could not be told - {0} - so nothing was changed, and the next run asks again.", why);
            Print_(true, sentence);
            LOG_APPLICATION(sentence);
            result = 2;
         }
         else
         {
            const String sentence = Formatter::Format("This server is not a new installation's - {0} - so its listeners, its IP ranges and its certificates are left as they are. The setup checklist says whether a password can still cross the network in the clear.", why);
            Print_(false, sentence);
            LOG_APPLICATION(sentence);
            result = 5;
         }

         // Decided, one way or the other: the mark goes, so that the defaults are
         // never weighed against this database again - not after a first start
         // that found it configured, and not after they were applied. A failure
         // part way keeps it, and the next run finds the half-changed server and
         // leaves it, having said so. So does a mark too old for the unit's start,
         // which a run by hand may still act on.
         if (marked && !tooOld && (result == 0 || result == 5) && !ClearMark_())
            Print_(true, Formatter::Format("The {0} mark could not be removed from the database; the next run will find the server changed and leave it as it is.", String(PendingRowName)));
      }

      Application::Instance()->ExitInstance();

      // Answered: applied, or not a new installation. A failure keeps the marker,
      // so that the next start asks again - and finds a server half changed, which
      // it then leaves alone, having said so.
      if (ifMarked && (result == 0 || result == 5))
         FileUtilities::DeleteFile(markerFile);

      return result;
   }

   int
   NewInstallDefaults::ApplyAtServiceStart()
   {
      // The service's start is an administrator of its own on the audit trail, as
      // the command line is, under the request that asked for it.
      AuditTrail::Actor actor;
      actor.present = true;
      actor.name = _T("sc start hMailServer --apply-new-install-defaults");
      actor.kind = _T("system");
      actor.interface_name = _T("service start");
      AuditScope scope(actor);

      String why;
      bool marked = false;
      bool unknown = false;
      bool tooOld = false;
      int result = 0;
      if (IsNewInstallation_(false, why, marked, unknown, tooOld))
      {
         // What Apply_ says, made and not made, into the application log: the
         // service has no output to say it on.
         say_in_application_log = true;
         result = Apply_();
         say_in_application_log = false;
      }
      else if (unknown)
      {
         LOG_APPLICATION(Formatter::Format("New installation: whether this is a new installation's database could not be told at this start - {0} - so nothing was changed, and the next start asked to apply its defaults asks again.", why));
         result = 2;
      }
      else
      {
         LOG_APPLICATION(Formatter::Format("New installation: this start was asked to apply a new installation's defaults, and this server is not a new installation's - {0} - so its listeners, its IP ranges and its certificates are left as they are.", why));
         result = 5;
      }

      if (marked && (result == 0 || result == 5) && !ClearMark_())
         LOG_APPLICATION(Formatter::Format("New installation: the {0} mark could not be removed from the database; the next run will find the server changed and leave it as it is.", String(PendingRowName)));

      return result;
   }
}
