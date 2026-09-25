// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "IniFileSettings.h"

#include "IniSettingStore.h"
#include "IniSettingsBaseline.h"

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include "../Util/Crypt.h"
#include "../Util/Hashing/HashCreator.h"
#include "../Util/DiskSpace.h"
#include "../Util/Utilities.h"
#include "../Util/FileUtilities.h"

// The Win32 profile API - GetPrivateProfileString and the three calls
// beside it - lives in kernel32 and has no POSIX equivalent, so the POSIX
// build declares those four functions here and implements them over the INI
// file itself in Common/Util/IniFile.cpp. The Windows build never reaches
// this line and binds the same calls to <windows.h> as it always has.
#ifdef HM_PLATFORM_POSIX
#include "../Util/IniFile.h"
#endif

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // A build of the [Settings] block in progress on THIS thread: the block being
      // filled, the block the server is running on, and which marks the build must
      // leave at their running value. Thread-local because the reads are the
      // ordinary ReadIniSettingString_ / ReadIniSettingInteger_ calls - which is
      // what lets build/check-ini-coverage.py go on reading every key and its
      // default out of this file - and a read on another thread at the same moment
      // (GetSettingsValue over COM, the pending list) must see none of it.
      struct BuildContext
      {
         IniFileSettings::Values *block;
         const IniFileSettings::Values *running;
         bool keep_reinitialize;
         bool keep_restart;
         bool reload;
      };

      thread_local BuildContext *build_context = nullptr;

      // Installs a build context on this thread for its lifetime and puts back
      // whatever was there, so a throw out of a read cannot leave a context
      // pointing at a block that no longer exists.
      struct BuildScope
      {
         explicit BuildScope(BuildContext *context) : previous_(build_context) { build_context = context; }
         ~BuildScope() { build_context = previous_; }

         BuildContext *previous_;
      };

      // The baseline's rows by lower-cased name, built once. The loader spells every
      // key as the baseline does - both come from this file - so a build's lookups
      // match exactly; a caller's name is matched without case, as the file matches.
      const std::map<String, size_t> &BaselineIndex()
      {
         static const std::map<String, size_t> index = []()
         {
            std::map<String, size_t> built;
            for (size_t i = 0; i < IniSettingsBaseline::SettingsCount; i++)
            {
               String lowered = IniSettingsBaseline::Settings[i].Name;
               lowered.ToLower();
               built[lowered] = i;
            }
            return built;
         }();

         return index;
      }

      const IniSettingsBaseline::Setting *FindBaselineSetting(const String &name)
      {
         String lowered = name;
         lowered.ToLower();

         const std::map<String, size_t> &index = BaselineIndex();
         auto found = index.find(lowered);
         if (found == index.end())
            return nullptr;

         return &IniSettingsBaseline::Settings[(*found).second];
      }

      const TCHAR *AppliesWord(IniSettingsBaseline::ApplyMark mark)
      {
         switch (mark)
         {
         case IniSettingsBaseline::AppliesReinitialize:
            return _T("reinitialize");
         case IniSettingsBaseline::AppliesRestart:
            return _T("restart");
         default:
            return _T("now");
         }
      }

      // Whether a build keeps this key at its running value, and that value. A key
      // the running block never resolved - the first build of a process - is read
      // from the store whatever its mark.
      bool KeepRunningValue(const BuildContext &build, const String &key, String &value)
      {
         if (!build.running || (!build.keep_reinitialize && !build.keep_restart))
            return false;

         const IniSettingsBaseline::Setting *setting = FindBaselineSetting(key);
         if (!setting)
            return false;

         bool keep = (setting->Applies == IniSettingsBaseline::AppliesReinitialize && build.keep_reinitialize) ||
                     (setting->Applies == IniSettingsBaseline::AppliesRestart && build.keep_restart);
         if (!keep)
            return false;

         auto found = build.running->resolved.find(key);
         if (found == build.running->resolved.end())
            return false;

         value = (*found).second;
         return true;
      }
   }

   String IniFileSettings::ini_file_;
  
   IniFileSettings::IniFileSettings() :
      database_settings_loaded_(false),
      is_internal_database_(false),
      dbport_(0),
      no_of_dbconnections_(0),
      no_of_dbconnection_attempts_(6),
      no_of_dbconnection_attempts_Delay(5),
      database_allow_unencrypted_connection_(false),
      sqldbtype_(HM::DatabaseSettings::TypeUnknown),
      simulate_database_failure_enabled_(false),
      running_values_latched_(false),
      values_(std::make_shared<Values>()),
      values_generation_(0)
   {

   }

   IniFileSettings::~IniFileSettings()
   {  
   

   }

   void
   IniFileSettings::LoadSettings(LoadScope scope)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Reads the bootstrap sections from hMailServer.ini, then builds and publishes
   // the [Settings] block (BuildAndPublish_). The bootstrap is read here and
   // nowhere else - at a start, at a reinitialise, and when COM's
   // Settings.Directories or Database.CreateInternalDatabase asks - and a save's
   // reload never re-reads it.
   //---------------------------------------------------------------------------()
   {
      boost::lock_guard<boost::recursive_mutex> guard(reload_mutex_);

      administrator_password_ = ReadIniSettingString_("Security", "AdministratorPassword", "");
      administrator_totp_secret_protected_ = ReadIniSettingString_("Security", "AdministratorTotpSecret", "");

      database_server_ = ReadIniSettingString_("Database", "Server", "");
      database_name_ = ReadIniSettingString_("Database", "Database", "");
      username_ = ReadIniSettingString_("Database", "Username", "");
      password_ = ReadIniSettingString_("Database", "Password", "");
      is_internal_database_ = ReadIniSettingInteger_("Database", "Internal", 0) == 1;
      database_server_FailoverPartner = ReadIniSettingString_("Database", "ServerFailoverPartner", "");
      database_provider_ = ReadIniSettingString_("Database", "Provider", "");

      String sDatabaseType = ReadIniSettingString_("Database", "Type", "");
      
      Crypt::EncryptionType iPWDEncryptionType = (Crypt::EncryptionType) ReadIniSettingInteger_("Database", "Passwordencryption", 0);

      // Decrypt password read from hmailserver.ini
      password_ = Crypt::Instance()->DeCrypt(password_, iPWDEncryptionType);

      if (sDatabaseType.CompareNoCase(_T("MSSQL")) == 0)
         sqldbtype_ = HM::DatabaseSettings::TypeMSSQLServer;
      else if (sDatabaseType.CompareNoCase(_T("MYSQL")) == 0)
         sqldbtype_ = HM::DatabaseSettings::TypeMYSQLServer;
      else if (sDatabaseType.CompareNoCase(_T("PostgreSQL")) == 0)
         sqldbtype_ = HM::DatabaseSettings::TypePGServer;
      else if (sDatabaseType.CompareNoCase(_T("MSSQLCE")) == 0)
         sqldbtype_ = HM::DatabaseSettings::TypeMSSQLCompactEdition;

      dbport_ = ReadIniSettingInteger_( "Database", "Port", 0);

      app_directory_ = ReadIniSettingString_("Directories", "ProgramFolder", "");
      // The separator this platform uses. Everything derived from the program
      // directory is built by concatenation - Languages, DBScripts, Bin - so a
      // backslash here is not cosmetic: it becomes part of every one of those
      // names, and a POSIX filesystem creates the directory rather than
      // objecting.
      if (app_directory_.Right(1) != FileUtilities::PathSeparator)
         app_directory_ += FileUtilities::PathSeparator;

      data_directory_ = ResolveDataDirectory_(app_directory_, ReadIniSettingString_("Directories", "DataFolder", ""));

      temp_directory_ = ResolveAgainstProgramDirectory_(ReadIniSettingString_("Directories", "TempFolder", ""));
#ifdef HM_PLATFORM_POSIX
      if (temp_directory_.Right(1) == _T("/") || temp_directory_.Right(1) == _T("\\"))
         temp_directory_ = temp_directory_.Left(temp_directory_.GetLength() -1);
#else
      if (temp_directory_.Right(1) == _T("\\"))
         temp_directory_ = temp_directory_.Left(temp_directory_.GetLength() -1);
#endif

      event_directory_ = ResolveAgainstProgramDirectory_(ReadIniSettingString_("Directories", "EventFolder", ""));

      // Derived from the program folder, which has already been given this
      // platform's separator above, so it is reused rather than re-derived.
      dbscript_directory_ = app_directory_ + "DBScripts";

      no_of_dbconnections_ = ReadIniSettingInteger_("Database", "NumberOfConnections", 5);            
      no_of_dbconnection_attempts_ = ReadIniSettingInteger_("Database", "ConnectionAttempts", 6);  
      no_of_dbconnection_attempts_Delay = ReadIniSettingInteger_("Database", "ConnectionAttemptsDelay", 5);  
      database_allow_unencrypted_connection_ = ReadIniSettingInteger_("Database", "AllowUnencryptedConnection", 0) == 1;
      // A [Database] key by necessity: read before the database is open, so it
      // cannot live in the database. See GetDatabaseUpgradeOnStart.
      database_upgrade_on_start_ = ReadIniSettingInteger_("Database", "UpgradeOnStart", 0) == 1;
      database_postgresql_ssl_mode_ = ReadIniSettingString_("Database", "PostgreSQLSslMode", "");
      database_postgresql_ssl_root_cert_ = ReadIniSettingString_("Database", "PostgreSQLSslRootCert", "");
      database_connection_string_options_ = ReadIniSettingString_("Database", "ConnectionStringOptions", "");
      
      if (sqldbtype_ == HM::DatabaseSettings::TypeMSSQLCompactEdition)
      {
         // Always use one database connection when working with SQL CE. SQL CE is supposed
         // to be ACID, robust and so on but isn't really.
         // http://forums.microsoft.com/MSDN/ShowPost.aspx?PostID=4141097&SiteID=1
         no_of_dbconnections_ = 1;
      }


      database_directory_ = ResolveAgainstProgramDirectory_(ReadIniSettingString_("Directories", "DatabaseFolder", ""));
#ifdef HM_PLATFORM_POSIX
      if (database_directory_.Right(1) == _T("/") || database_directory_.Right(1) == _T("\\"))
         database_directory_ = database_directory_.Left(database_directory_.GetLength() -1);
#else
      if (database_directory_.Right(1) == _T("\\"))
         database_directory_ = database_directory_.Left(database_directory_.GetLength() -1);
#endif

      String sValidLanguages = ReadIniSettingString_("GUILanguages", "ValidLanguages", "");
      valid_languages_ = StringParser::SplitString(sValidLanguages, ",");

      BuildAndPublish_(scope, false);

      // DataFolder may just have changed, and the cached free-space reading is
      // about whichever volume it named a moment ago.
      DiskSpace::InvalidateCache();
   }

   void
   IniFileSettings::BuildAndPublish_(LoadScope scope, bool reload)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Builds a new [Settings] block from the store and publishes it with one atomic
   // store. Every read below is the ordinary ReadIniSettingString_ /
   // ReadIniSettingInteger_ call, with this thread's build context set: the
   // context records what each key resolved to and, for a key the scope must not
   // apply, answers the running block's value in place of the store's - so the
   // clamps and corrections below run on the same text either way, and a key kept
   // at its running value comes out exactly as it went in.
   //---------------------------------------------------------------------------()
   {
      boost::lock_guard<boost::recursive_mutex> guard(reload_mutex_);

      std::shared_ptr<const Values> running = Current();
      std::shared_ptr<Values> block = std::make_shared<Values>();
      Values &v = *block;

      const bool latched = running_values_latched_.load();

      BuildContext context;
      context.block = block.get();
      context.running = running.get();
      context.keep_restart = latched;
      context.keep_reinitialize = latched && scope == ScopeNow;
      context.reload = reload;

      // Set on this thread for the reads below and cleared however they end.
      BuildScope scope_of_build(&context);

      v.max_no_of_external_fetch_threads_ = ReadIniSettingInteger_("Settings", "MaxNumberOfExternalFetchThreads", 15);
      v.add_xauth_user_header_ = ReadIniSettingInteger_("Settings", "AddXAuthUserHeader", 0) == 1;

      v.daemonaddress_domain_ = ReadIniSettingString_("Settings", "DaemonAddressDomain", "");
   
      v.greylisting_enabled_during_record_expiration_ = ReadIniSettingInteger_("Settings", "GreylistingEnabledDuringRecordExpiration", 1) == 1;
      v.greylisting_expiration_interval_ = ReadIniSettingInteger_("Settings", "GreylistingRecordExpirationInterval", 240);

      v.preferred_hash_algorithm_ = ReadIniSettingInteger_("Settings", "PreferredHashAlgorithm", 4);

      // Crypt::EncryptionType: 0 None, 1 Blowfish, 2 MD5, 3 SHA256, 4 PBKDF2,
      // 5 Argon2id. The first three are not password hashes - None and Blowfish are
      // recoverable rather than hashed at all, and MD5 has not been a defensible
      // choice for storing a password for well over a decade.
      //
      // Clamped here, at the single place the value is read, rather than at each of
      // the five call sites that consume it: two of them hand it straight to Crypt
      // as the scheme to store a NEW secret under, so a 0 or a 1 there would write
      // account passwords and app passwords in a form anybody with the database can
      // read back. Reported rather than silently corrected, because an administrator
      // who set this deliberately is owed an explanation of why it did not take.
      if (v.preferred_hash_algorithm_ < 3 || v.preferred_hash_algorithm_ > 7 || v.preferred_hash_algorithm_ == 6)
      {
         String message;
         message.Format(_T("PreferredHashAlgorithm is set to %d, which is not a password-hashing scheme this server will store a new secret under (0 is no hashing, 1 is reversible, 2 is MD5, 6 is the machine-bound DPAPI scheme for stored secrets). Using 4 (PBKDF2) instead. Set it to 3 (SHA256), 4 (PBKDF2), 5 (Argon2id) or 7 (scrypt) to choose deliberately."),
                        v.preferred_hash_algorithm_);

         if (ReportThisLoad_(_T("PreferredHashAlgorithm")))
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5528, "IniFileSettings::LoadSettings", message);
         }

         v.preferred_hash_algorithm_ = 4;
      }

      // Argon2id (5) is only a choice on a build whose OpenSSL has the KDF - 3.2
      // or later; a Linux build against a distribution's 3.0 does not. Read as
      // scrypt (7), the other memory-hard scheme, and reported: a preference this
      // build cannot honour must not become an empty hash stored for a password.
      if (v.preferred_hash_algorithm_ == 5 && !HashCreator::Argon2idAvailable())
      {
         if (ReportThisLoad_(_T("PreferredHashAlgorithm")))
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5607, "IniFileSettings::LoadSettings",
               "PreferredHashAlgorithm is 5 (Argon2id), but this build's OpenSSL has no Argon2id KDF - OpenSSL 3.2 or later has it. "
               "Using 7 (scrypt) instead, which is memory-hard as well. Set it to 7 or 4 to choose deliberately.");
         }

         v.preferred_hash_algorithm_ = 7;
      }

      // Minimum password hash scheme an account may use to authenticate. Accounts
      // whose stored hash is weaker than this (Crypt::StrengthRank ordering, under
      // which Argon2id and scrypt are peers) are refused. 0 (ETNone) disables the
      // policy and preserves prior behaviour.
      v.minimum_accepted_hash_algorithm_ = ReadIniSettingInteger_("Settings", "MinimumAcceptedHashAlgorithm", 0);

      // Optional server-wide secret ("pepper") HMAC-mixed into Argon2id password hashes
      // (see Crypt::EnCrypt/Validate). Empty disables it (no behaviour change). It only
      // affects Argon2id hashes so PBKDF2 stays usable as the SCRAM SaltedPassword.
      v.password_pepper_ = ReadIniSettingString_("Settings", "PasswordPepper", "");

      // Work factors for new password hashes. 0 is the built-in default: 210,000
      // PBKDF2 iterations, and 19,456 KiB with 2 passes for Argon2id, the OWASP
      // recommended minimum. Bounded at both ends because both ends are a failure -
      // below the floor is a hash an attacker brute-forces at leisure, above the
      // ceiling is a logon the server cannot afford, since every verification runs on
      // a connection thread. The ceilings match what ValidatePBKDF2 and
      // ValidateArgon2id will verify at all. Out of range is reported and read as
      // the default, the way PreferredHashAlgorithm above is.
      v.password_hash_iterations_ = ReadPasswordHashWorkFactor_("PasswordHashIterations", 10000, 10000000);
      v.password_hash_memory_kib_ = ReadPasswordHashWorkFactor_("PasswordHashMemoryKB", 4096, 1048576);
      v.password_hash_time_cost_ = ReadPasswordHashWorkFactor_("PasswordHashTimeCost", 1, 20);

      // OAuth2 bearer-token authentication (SASL XOAUTH2 / OAUTHBEARER). Disabled by
      // default. When enabled, presented JWT bearer tokens are verified locally against
      // the configured signing key(s); the mechanism is TLS-gated by default.
      v.oauth2_enabled_ = ReadIniSettingInteger_("Settings", "OAuth2Enabled", 0) == 1;
      v.oauth2_require_tls_ = ReadIniSettingInteger_("Settings", "OAuth2RequireTLS", 1) == 1;
      v.outbound_oauth2_token_url_ = ReadIniSettingString_("Settings", "OutboundOAuth2TokenUrl", "");
      v.outbound_oauth2_client_id_ = ReadIniSettingString_("Settings", "OutboundOAuth2ClientId", "");
      v.outbound_oauth2_client_secret_ = ReadIniSettingString_("Settings", "OutboundOAuth2ClientSecret", "");
      // The .default scope is what Exchange Online's client-credentials flow expects.
      v.outbound_oauth2_scope_ = ReadIniSettingString_("Settings", "OutboundOAuth2Scope", "https://outlook.office365.com/.default");
      v.outbound_oauth2_hosts_ = ReadIniSettingString_("Settings", "OutboundOAuth2Hosts", "smtp.office365.com");
      v.outbound_oauth2_fixed_token_ = ReadIniSettingString_("Settings", "OutboundOAuth2FixedToken", "");
      v.fetch_oauth2_hosts_ = ReadIniSettingString_("Settings", "FetchOAuth2Hosts", "outlook.office365.com");
      // Comma-separated allow-list of accepted JWT "alg" values (e.g. "RS256,HS256").
      // "none" is never accepted regardless of this list.
      v.oauth2_allowed_algorithms_ = ReadIniSettingString_("Settings", "OAuth2AllowedAlgorithms", "RS256");
      // Shared secret for HS256 verification (HMAC key).
      v.oauth2_hmac_secret_ = ReadIniSettingString_("Settings", "OAuth2HmacSecret", "");
      // PEM file holding the RSA/EC public key for RS256 verification.
      v.oauth2_rsa_public_key_file_ = ReadIniSettingString_("Settings", "OAuth2PublicKeyFile", "");
      // Expected token issuer (iss) and audience (aud); empty disables that check.
      v.oauth2_issuer_ = ReadIniSettingString_("Settings", "OAuth2Issuer", "");
      v.oauth2_audience_ = ReadIniSettingString_("Settings", "OAuth2Audience", "");
      // Claim that carries the account's e-mail address / login name.
      v.oauth2_username_claim_ = ReadIniSettingString_("Settings", "OAuth2UsernameClaim", "email");
      // RFC 7517 JWK Set: where the provider publishes its current signing keys, so a
      // key rotation needs no hand-copied PEM file. Empty keeps the PEM file the only
      // source. https, or plain http to a loopback address only.
      v.oauth2_jwks_url_ = ReadIniSettingString_("Settings", "OAuth2JwksUrl", "");
      v.oauth2_jwks_cache_seconds_ = ReadIniSettingInteger_("Settings", "OAuth2JwksCacheSeconds", 3600);
      // RFC 7662 token introspection: the provider's live word on whether a token
      // that verified is still active, which is the only check that sees a
      // revocation. Empty = no such check. An unanswerable check refuses the token
      // unless FailOpen=1, the way RFC 7662 reads.
      v.oauth2_introspection_url_ = ReadIniSettingString_("Settings", "OAuth2IntrospectionUrl", "");
      v.oauth2_introspection_client_id_ = ReadIniSettingString_("Settings", "OAuth2IntrospectionClientId", "");
      v.oauth2_introspection_client_secret_ = ReadIniSettingString_("Settings", "OAuth2IntrospectionClientSecret", "");
      v.oauth2_introspection_cache_seconds_ = ReadIniSettingInteger_("Settings", "OAuth2IntrospectionCacheSeconds", 300);
      v.oauth2_introspection_fail_open_ = ReadIniSettingInteger_("Settings", "OAuth2IntrospectionFailOpen", 0) == 1;

      // Protect reversible secrets at rest (the database password in this INI plus
      // the DB-stored route/fetch/relayer passwords) with the platform's secret
      // store so they cannot be decrypted off-box: machine-scoped Windows DPAPI on
      // Windows, and on Linux - which has no DPAPI - a 0600 key file under the data
      // directory with AES-256-GCM (DataProtector.h says what each promises). The
      // key is named for the Windows store and kept for compatibility; the default
      // is 1 on both platforms. Set to 0 to keep the legacy Blowfish scheme (e.g. to
      // allow restoring a backup onto a different Windows machine; on Linux the
      // key file travels with the data directory, so there is no such reason).
      // Existing legacy values are always still readable.
      v.protect_stored_secrets_with_dpapi_ = ReadIniSettingInteger_("Settings", "ProtectStoredSecretsWithDPAPI", 1) == 1;

      // Optional least-privilege Windows service account for the hMailServer service.
      // Empty (the default) leaves the service running under LocalSystem, exactly as
      // before. Set ServiceAccountName to run under a dedicated account - the
      // recommended choice is the password-less virtual account "NT SERVICE\hMailServer"
      // (leave ServiceAccountPassword empty for virtual/managed accounts). The chosen
      // account must be granted "Log on as a service" plus access to the hMailServer
      // program, data and database directories. Takes effect when the service is
      // (re)registered.
      v.service_account_name_ = ReadIniSettingString_("Settings", "ServiceAccountName", "");
      v.service_account_password_ = ReadIniSettingString_("Settings", "ServiceAccountPassword", "");

      v.dnsbl_checks_after_mail_from_ = ReadIniSettingInteger_("Settings", "DNSBLChecksAfterMailFrom", 1) == 1;

      v.sep_svc_logs_ = ReadIniSettingInteger_("Settings", "SepSvcLogs", 0) == 1;
      v.log_level_ = ReadIniSettingInteger_("Settings", "LogLevel", 9);
      v.max_log_line_len_ = ReadIniSettingInteger_("Settings", "MaxLogLineLen", 500);
      if (v.max_log_line_len_ < 100) v.max_log_line_len_ = 100;
      v.quick_retries_ = ReadIniSettingInteger_("Settings", "QuickRetries", 0);
      v.quick_retries_Minutes = ReadIniSettingInteger_("Settings", "QuickRetriesMinutes", 6);
      v.queue_randomness_minutes_ = ReadIniSettingInteger_("Settings", "QueueRandomnessMinutes", 0);
      // If v.queue_randomness_minutes_ out of range use 0 
      if (v.queue_randomness_minutes_ <= 0) v.queue_randomness_minutes_ = 0;
      v.mxtries_factor_ = ReadIniSettingInteger_("Settings", "MXTriesFactor", 0);
      if (v.mxtries_factor_ <= 0) v.mxtries_factor_ = 0;
      v.archive_dir_ = ReadIniSettingString_("Settings", "ArchiveDir", "");
      if (v.archive_dir_.Right(1) == FileUtilities::PathSeparator)
         v.archive_dir_ = v.archive_dir_.Left(v.archive_dir_.GetLength() -1);
      v.archive_hardlinks_ =  ReadIniSettingInteger_("Settings", "ArchiveHardLinks", 0) == 1;
      v.delivery_hardlinks_ = ReadIniSettingInteger_("Settings", "DeliveryHardLinks", 0) == 1;

      // Comma-separated, case-insensitive, whitespace tolerated. Kept lower-cased
      // so that IsArchiveDomain is one lookup.
      v.archive_domains_.clear();
      std::vector<String> archiveDomains = StringParser::SplitString(ReadIniSettingString_("Settings", "ArchiveDomains", ""), ",");
      for (size_t i = 0; i < archiveDomains.size(); i++)
      {
         String domain = archiveDomains[i];
         domain.Trim();

         if (!domain.IsEmpty())
            v.archive_domains_.insert(domain.ToLower());
      }
      v.pop3dmin_timeout_ =  ReadIniSettingInteger_("Settings", "POP3DMinTimeout", 10);
      v.pop3dmax_timeout_ =  ReadIniSettingInteger_("Settings", "POP3DMaxTimeout",600);
      v.pop3cmin_timeout_ =  ReadIniSettingInteger_("Settings", "POP3CMinTimeout", 30);
      v.pop3cmax_timeout_ =  ReadIniSettingInteger_("Settings", "POP3CMaxTimeout",900);
      v.smtpdmin_timeout_ =  ReadIniSettingInteger_("Settings", "SMTPDMinTimeout", 10);
      v.smtpdmax_timeout_ =  ReadIniSettingInteger_("Settings", "SMTPDMaxTimeout",1800);
      v.smtpcmin_timeout_ =  ReadIniSettingInteger_("Settings", "SMTPCMinTimeout", 30);
      v.smtpcmax_timeout_ =  ReadIniSettingInteger_("Settings", "SMTPCMaxTimeout",600);
      v.samin_timeout_ =  ReadIniSettingInteger_("Settings", "SAMinTimeout", 30);
      v.samax_timeout_ =  ReadIniSettingInteger_("Settings", "SAMaxTimeout",90);

      // The spamd User: header. The value goes onto the wire as a protocol header line,
      // so a control character in it would end that line early or start another one;
      // refused rather than sent, with the reason reported, because a header spamd
      // rejects is a scan that never happens.
      v.spamassassin_user_ = ReadIniSettingString_("Settings", "SpamAssassinUser", "");
      for (int i = 0; i < v.spamassassin_user_.GetLength(); i++)
      {
         const wchar_t ch = v.spamassassin_user_[i];
         if (ch < 0x20 || ch == 0x7F)
         {
            if (ReportThisLoad_(_T("SpamAssassinUser")))
            {
               ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5562, "IniFileSettings::LoadSettings",
                  "SpamAssassinUser in hMailServer.ini contains a control character and has been ignored; no User: header is sent to spamd.");
            }
            v.spamassassin_user_ = "";
            break;
         }
      }
      v.spamassassin_user_from_recipient_ = ReadIniSettingInteger_("Settings", "SpamAssassinUserFromRecipient", 0) == 1;
      v.spamassassin_learn_on_move_ = ReadIniSettingInteger_("Settings", "SpamAssassinLearnOnMove", 0) == 1;
      // Upper bound, in seconds, on the accept/save work that runs after end-of-data
      // and holds the thread that sends the "250". Past this the message is refused
      // with a temporary 451 so the sending MTA retries, rather than the reply
      // stalling past the sender's own timeout (discussion #18). 0 disables it.
      // Default 240s: comfortably under Postfix's 600s data-done timeout.
      v.finalization_timeout_ =  ReadIniSettingInteger_("Settings", "FinalizationTimeout", 240);
      v.clam_min_timeout_ =  ReadIniSettingInteger_("Settings", "ClamMinTimeout", 15);
      v.clam_max_timeout_ =  ReadIniSettingInteger_("Settings", "ClamMaxTimeout",90);

      // Bounds on operations that would otherwise wait forever and, because they
      // run on bounded thread pools, take the whole server down with them when
      // enough of them pile up. Each is in seconds and 0 disables that bound.
      //
      // A DNS query the OS resolver never answers; a database connection that
      // never becomes free; an administrator's event script that loops or blocks
      // in a COM call; an external virus scanner that hangs. All four were
      // unbounded, and all four sit on a path that a remote sender can drive.
      v.dns_query_timeout_ =  ReadIniSettingInteger_("Settings", "DNSQueryTimeout", 10);

      // Absolute ceiling on an outbound client session, as distinct from the idle
      // timeouts above. 30 minutes is far longer than any healthy delivery needs
      // and still bounds a remote server that answers slowly enough to keep the
      // idle timer alive forever.
      v.client_session_ceiling_ =  ReadIniSettingInteger_("Settings", "ClientSessionCeiling", 1800);

      // Bounding this wait is only safe because the callers that act on a lookup
      // can now tell "the database did not answer" from "the database answered
      // with nothing" - see DatabaseUnavailableMarker. Without that, a wait that
      // expired during a backup would have reported a valid recipient as unknown
      // and the sender would have bounced the mail instead of retrying.
      v.db_connection_acquire_timeout_ =  ReadIniSettingInteger_("Settings", "DBConnectionAcquireTimeout", 60);
      v.script_timeout_ =  ReadIniSettingInteger_("Settings", "ScriptTimeout", 60);
      // Absent means "*": a key that appears in an upgrade must not break the
      // scripts an installation already runs. Present and empty means none.
      v.script_allowed_objects_ = ReadIniSettingString_("Settings", "ScriptAllowedObjects", "*");
      v.external_process_timeout_ =  ReadIniSettingInteger_("Settings", "ExternalProcessTimeout", 300);

      // Async work-queue health. The stall threshold is how long every worker may
      // be busy before the server reports which tasks are holding them; the
      // reserved count keeps that many threads out of the reach of scanning and
      // scripting so short work (saving an accepted message) always has one.
      v.async_queue_stall_threshold_ =  ReadIniSettingInteger_("Settings", "AsyncQueueStallThreshold", 120);
      v.async_queue_reserved_threads_ =  ReadIniSettingInteger_("Settings", "AsyncQueueReservedThreads", 2);

      // Absolute ceilings on a single IMAP SEARCH. SEARCH BODY and SEARCH TEXT are
      // resolved by reading and MIME-parsing every message in the mailbox
      // (hm_message_metadata indexes only date, from, subject, to and cc), so one
      // authenticated command occupies a worker thread for as long as the mailbox
      // takes to read, and can be repeated. Both ceilings are measured from the
      // start of the search and neither re-arms; 0 disables that half. Full-text
      // search will remove the motive; these remove the vector, with no index and
      // no schema change.
      //
      // IMAPSearchTimeout, 60 seconds: this is the bound on the resource actually
      // under attack, which is thread-seconds rather than bytes. 60s sits below
      // Thunderbird's own 100-second socket timeout (mailnews.tcptimeout), so a
      // search this cuts short is one the client had most likely already given up
      // on, and it is well above what a mailbox of the size this server's own
      // indexer is dimensioned for (IndexerFullLimit, 25000 messages) takes to
      // read on server storage. The honest caveat: a genuinely huge mailbox - a
      // few hundred thousand messages with attachments - can take minutes to body
      // search on any hardware, and for those the setting has to be raised. That
      // is the correct trade: the administrator who has such mailboxes knows it,
      // whereas the default has to protect the server that does not.
      //
      // IMAPSearchMaxMegabytes, 2048 MB of message content examined: the
      // deterministic half. A time bound alone makes the same mailbox succeed on
      // fast storage and fail on slow storage, which is right for a DoS bound but
      // leaves the behaviour unpredictable for an administrator and untestable; a
      // byte bound behaves identically on every machine. Bytes rather than message
      // count because the cost tracks bytes read and parsed - ten 40 MB messages
      // cost far more than ten thousand 2 KB ones. 2 GB is more content than any
      // interactive search legitimately reads and still a hard cap on the I/O a
      // single command can demand.
      v.imap_search_timeout_ = ReadIniSettingInteger_("Settings", "IMAPSearchTimeout", 60);
      if (v.imap_search_timeout_ < 0) v.imap_search_timeout_ = 0;
      v.imap_search_max_megabytes_ = ReadIniSettingInteger_("Settings", "IMAPSearchMaxMegabytes", 2048);
      if (v.imap_search_max_megabytes_ < 0) v.imap_search_max_megabytes_ = 0;

      v.samove_vs_copy_ = ReadIniSettingInteger_("Settings", "SAMoveVsCopy", 0) == 1;
      v.auth_user_replacement_ip_ = ReadIniSettingString_("Settings", "AuthUserReplacementIP", "");
      v.indexer_full_minutes_ =  ReadIniSettingInteger_("Settings", "IndexerFullMinutes",720);
      v.indexer_full_limit_ =  ReadIniSettingInteger_("Settings", "IndexerFullLimit",25000);
      v.indexer_quick_limit_ =  ReadIniSettingInteger_("Settings", "IndexerQuickLimit",1000);

      // The full-text term index behind SEARCH BODY/TEXT. Off by default -
      // enabling it costs a term table of a few kilobytes per message and a
      // backfill pass over every delivered message, and that has to be an
      // administrator's decision, never an upgrade's. The index only ever
      // narrows the set of messages the existing substring scan reads, so the
      // results are identical on and off; correctness never depends on these
      // values, which is why they can be clamped rather than rejected.
      v.indexer_full_text_enabled_ = ReadIniSettingInteger_("Settings", "IndexerFullText", 0) == 1;

      // Batch: the id-range width of one backfill pass, so also the most
      // messages one pass will read and tokenise before the cursor is saved
      // and the thread pauses. Small enough not to monopolise the database,
      // large enough that a million-message backfill finishes in hours, not
      // weeks.
      v.indexer_full_text_batch_size_ = ReadIniSettingInteger_("Settings", "IndexerFullTextBatchSize", 250);
      if (v.indexer_full_text_batch_size_ < 1) v.indexer_full_text_batch_size_ = 1;
      if (v.indexer_full_text_batch_size_ > 100000) v.indexer_full_text_batch_size_ = 100000;

      // The shortest search-string run the index is allowed to answer for.
      // Clamped at 3 from below because the index never STORES tokens shorter
      // than three characters - the stored floor is a constant precisely so
      // that changing this setting can never make the index promise answers
      // the stored terms cannot give. Raising it only makes more searches
      // fall back to the scan.
      v.indexer_full_text_min_token_length_ = ReadIniSettingInteger_("Settings", "IndexerFullTextMinTokenLength", 3);
      if (v.indexer_full_text_min_token_length_ < 3) v.indexer_full_text_min_token_length_ = 3;
      if (v.indexer_full_text_min_token_length_ > 64) v.indexer_full_text_min_token_length_ = 64;

      // Distinct terms per message before the indexer marks the message
      // always-scanned instead. 2048 covers ordinary mail and most HTML;
      // messages richer than that (inline base64 images, enormous newsletters)
      // are scanned on every search, which is exactly what they cost today.
      v.indexer_full_text_max_tokens_ = ReadIniSettingInteger_("Settings", "IndexerFullTextMaxTokensPerMessage", 2048);
      if (v.indexer_full_text_max_tokens_ < 64) v.indexer_full_text_max_tokens_ = 64;
      if (v.indexer_full_text_max_tokens_ > 1000000) v.indexer_full_text_max_tokens_ = 1000000;
      v.load_header_read_size_ =  ReadIniSettingInteger_("Settings", "LoadHeaderReadSize",4000);
      v.load_body_read_size_ =  ReadIniSettingInteger_("Settings", "LoadBodyReadSize",4000);
      v.blocked_iphold_seconds_ =  ReadIniSettingInteger_("Settings", "BlockedIPHoldSeconds",0);
      v.smtpdmax_size_drop_ =  ReadIniSettingInteger_("Settings", "SMTPDMaxSizeDrop",0);
      v.backup_messages_dbonly_ =  ReadIniSettingInteger_("Settings", "BackupMessagesDBOnly",0) == 1;
      v.backup_verify_restore_ = ReadIniSettingInteger_("Settings", "BackupVerifyRestore", 1) == 1;
      v.add_xauth_user_ip_ =  ReadIniSettingInteger_("Settings", "AddXAuthUserIP",1) == 1;
      v.add_xoriginal_rcpt_to_header_ = ReadIniSettingInteger_("Settings", "AddXOriginalRcptTo", 0) == 1;
      v.use_dns_cache_ = ReadIniSettingInteger_("Settings", "UseDNSCache", 1) == 1;
      v.dns_server_ = ReadIniSettingString_("Settings", "DNSServer", "");
      v.mta_sts_enabled_ = ReadIniSettingInteger_("Settings", "MtaStsEnabled", 1) == 1;

      // Auto-ban below the server. Off as shipped: a rule in the operating
      // system's firewall outlives a mistake in a way a row in hm_securityranges
      // does not, so an administrator turns it on knowing what it does, and lists
      // their own addresses in AutoBanNeverBan first. AutoBanCommand is any
      // program, run as "<command> ban <address> <minutes> <ports>" and
      // "<command> unban <address>", on both platforms. See AutoBanFirewall.h.
      v.auto_ban_firewall_enabled_ = ReadIniSettingInteger_("Settings", "AutoBanFirewall", 0) == 1;
      v.auto_ban_command_ = ReadIniSettingString_("Settings", "AutoBanCommand", "");
      v.auto_ban_never_ban_ = ReadIniSettingString_("Settings", "AutoBanNeverBan", "");
      v.dane_enabled_ = ReadIniSettingInteger_("Settings", "DaneEnforcementEnabled", 1) == 1;
      v.dnssec_validation_enabled_ = ReadIniSettingInteger_("Settings", "DnssecValidationEnabled", 1) == 1;
      v.dnssec_trust_anchors_ = ReadIniSettingString_("Settings", "DnssecTrustAnchors", "");
      v.update_check_enabled_ = ReadIniSettingInteger_("Settings", "UpdateCheckEnabled", 0) == 1;
      v.update_channel_ = ReadIniSettingString_("Settings", "UpdateChannel", "stable");
      v.update_feed_url_ = ReadIniSettingString_("Settings", "UpdateFeedUrl", "");
      v.http_proxy_ = ReadIniSettingString_("Settings", "HttpProxy", "");
      v.caldav_redirect_url_ = ReadIniSettingString_("Settings", "CalDavRedirectUrl", "");
      v.carddav_redirect_url_ = ReadIniSettingString_("Settings", "CardDavRedirectUrl", "");
      v.update_check_hours_ = ReadIniSettingInteger_("Settings", "UpdateCheckHours", 24);
      if (v.update_check_hours_ < 1)
         v.update_check_hours_ = 1;
      v.imap_compression_enabled_ = ReadIniSettingInteger_("Settings", "IMAPCompressionEnabled", 1) == 1;
      v.update_trust_roots_file_ = ReadIniSettingString_("Settings", "UpdateTrustRootsFile", "");
      v.update_log_public_key_file_ = ReadIniSettingString_("Settings", "UpdateLogPublicKeyFile", "");
      v.update_signing_identity_ = ReadIniSettingString_("Settings", "UpdateSigningIdentity", "");
      v.update_signing_issuer_ = ReadIniSettingString_("Settings", "UpdateSigningIssuer", "");
      v.update_source_repository_ = ReadIniSettingString_("Settings", "UpdateSourceRepository", "");
      v.update_require_authenticode_ = ReadIniSettingInteger_("Settings", "UpdateRequireAuthenticode", 0) == 1;
      v.update_service_wait_seconds_ = ReadIniSettingInteger_("Settings", "UpdateServiceWaitSeconds", 180);
      if (v.update_service_wait_seconds_ < 5)
         v.update_service_wait_seconds_ = 5;
      v.update_window_ = ReadIniSettingString_("Settings", "UpdateWindow", "");
      v.update_auto_download_ = ReadIniSettingInteger_("Settings", "UpdateAutoDownload", 0) == 1;
      v.update_backup_before_apply_ = ReadIniSettingInteger_("Settings", "UpdateBackupBeforeApply", 1) == 1;
      v.json_logging_ = ReadIniSettingInteger_("Settings", "JsonLogging", 0) == 1;
      v.log_delete_days_ = ReadIniSettingInteger_("Settings", "LogDeleteDays", 0);
      v.shutdown_drain_seconds_ = ReadIniSettingInteger_("Settings", "ShutdownDrainSeconds", 0);
      v.slow_query_log_ms_ = ReadIniSettingInteger_("Settings", "SlowQueryLogMilliseconds", 0);
      v.message_store_fsync_ = ReadIniSettingInteger_("Settings", "MessageStoreFsync", 0) == 1;

      // Free-space policy on the message-store volume. Both clamped at 0 (= off)
      // rather than trusted, because a negative megabyte count would otherwise
      // become an enormous unsigned byte count in DiskSpace and refuse every
      // message on a perfectly healthy server. See the getters for the defaults
      // and the argument for them.
      v.minimum_free_disk_space_mb_ = ReadIniSettingInteger_("Settings", "MinimumFreeDiskSpaceMB", 100);
      if (v.minimum_free_disk_space_mb_ < 0) v.minimum_free_disk_space_mb_ = 0;
      v.disk_space_warning_threshold_mb_ = ReadIniSettingInteger_("Settings", "DiskSpaceWarningThresholdMB", 1024);
      if (v.disk_space_warning_threshold_mb_ < 0) v.disk_space_warning_threshold_mb_ = 0;

      v.simulate_spool_write_failure_ = ReadIniSettingInteger_("Settings", "SimulateSpoolWriteFailure", 0);
      v.simulate_database_failure_for_ = ReadIniSettingString_("Settings", "SimulateDatabaseFailureFor", "");
      v.message_store_consistency_check_ = ReadIniSettingInteger_("Settings", "MessageStoreConsistencyCheck", 0) == 1;
      v.metrics_server_port_ = ReadIniSettingInteger_("Settings", "MetricsServerPort", 0);
      v.metrics_server_bind_address_ = ReadIniSettingString_("Settings", "MetricsServerBindAddress", "127.0.0.1");

      // Metrics access control and TLS. Every one of these defaults to empty, which is
      // the old behaviour exactly: a loopback scrape is unauthenticated plain HTTP as it
      // has always been. They only start mattering once an operator moves the listener
      // off loopback, which is when /metrics needs a credential to stay open.
      v.metrics_server_auth_token_ = ReadIniSettingString_("Settings", "MetricsServerAuthToken", "");
      v.metrics_server_auth_username_ = ReadIniSettingString_("Settings", "MetricsServerAuthUsername", "");
      v.metrics_server_auth_password_ = ReadIniSettingString_("Settings", "MetricsServerAuthPassword", "");
      v.metrics_server_certificate_file_ = ReadIniSettingString_("Settings", "MetricsServerCertificateFile", "");
      v.metrics_server_private_key_file_ = ReadIniSettingString_("Settings", "MetricsServerPrivateKeyFile", "");

      // Scheduled backups. All four absent by default: no schedule is registered and no
      // archive is ever deleted, so an existing installation is untouched until somebody
      // configures one. Retention is deliberately off rather than defaulted to something
      // tidy - a default that deletes archives is a default that loses somebody's only
      // backup.
      v.scheduled_backup_time_ = ReadIniSettingString_("Settings", "ScheduledBackupTime", "");
      v.scheduled_backup_interval_minutes_ = ReadIniSettingInteger_("Settings", "ScheduledBackupIntervalMinutes", 0);
      v.scheduled_backup_keep_count_ = ReadIniSettingInteger_("Settings", "ScheduledBackupKeepCount", 0);
      v.scheduled_backup_max_age_days_ = ReadIniSettingInteger_("Settings", "ScheduledBackupMaxAgeDays", 0);
      v.otel_endpoint_ = ReadIniSettingString_("Settings", "OtelEndpoint", "");
      v.otel_service_name_ = ReadIniSettingString_("Settings", "OtelServiceName", "hmailserver");
      v.otel_metrics_endpoint_ = ReadIniSettingString_("Settings", "OtelMetricsEndpoint", "");
      v.otel_logs_endpoint_ = ReadIniSettingString_("Settings", "OtelLogsEndpoint", "");
      v.otel_metrics_interval_ = ReadIniSettingInteger_("Settings", "OtelMetricsInterval", 60);
      v.manage_sieve_server_port_ = ReadIniSettingInteger_("Settings", "ManageSieveServerPort", 0);
      v.manage_sieve_server_bind_address_ = ReadIniSettingString_("Settings", "ManageSieveServerBindAddress", "127.0.0.1");
      v.arc_sealing_enabled_ = ReadIniSettingInteger_("Settings", "ArcSealingEnabled", 0) == 1;

      // DKIM signature timestamps (RFC 6376 3.5). See DKIM::Sign and
      // DKIM::VerifySignature_ for what each of these does.
      //
      // Enforcement defaults ON while emission of x= defaults OFF, and the asymmetry is
      // deliberate: honouring an expiry another server published costs us nothing and is
      // what the signer asked for, whereas publishing one of our own is a promise about
      // mail already in flight.
      v.dkim_signature_validity_seconds_ = ReadIniSettingInteger_("Settings", "DKIMSignatureValiditySeconds", 0);
      if (v.dkim_signature_validity_seconds_ < 0)
         v.dkim_signature_validity_seconds_ = 0;

      v.dkim_enforce_signature_expiry_ = ReadIniSettingInteger_("Settings", "DKIMEnforceSignatureExpiry", 1) == 1;

      v.dkim_expiry_clock_skew_seconds_ = ReadIniSettingInteger_("Settings", "DKIMExpiryClockSkewSeconds", 300);
      if (v.dkim_expiry_clock_skew_seconds_ < 0)
         v.dkim_expiry_clock_skew_seconds_ = 0;

      v.dkim_oversign_headers_ = ReadIniSettingString_("Settings", "DkimOversignHeaders", "");

      // RFC 8601 / RFC 7208 9.1 trace headers on inbound mail. Both default off: an
      // Authentication-Results field is a claim other systems act on, and it is worth
      // something only if the reader trusts this server's name, so turning it on is a
      // deliberate act rather than something an upgrade does quietly.
      v.authentication_results_enabled_ = ReadIniSettingInteger_("Settings", "AuthenticationResultsEnabled", 0) == 1;
      v.received_spf_header_enabled_ = ReadIniSettingInteger_("Settings", "ReceivedSpfHeaderEnabled", 0) == 1;
      v.authentication_results_identity_ = ReadIniSettingString_("Settings", "AuthenticationResultsIdentity", "");
      v.tls_rpt_from_address_ = ReadIniSettingString_("Settings", "TlsRptFromAddress", "");
      v.tls_rpt_organization_name_ = ReadIniSettingString_("Settings", "TlsRptOrganizationName", "hMailServer");

      // DMARC aggregate reporting (RFC 7489 7.2), the exact shape of the two
      // TLS-RPT keys above: an empty sender address means statistics are
      // collected and discarded, never sent - a mail server must not start
      // mailing third parties because someone turned DMARC checking on.
      v.dmarc_rpt_from_address_ = ReadIniSettingString_("Settings", "DmarcRptFromAddress", "");
      v.dmarc_rpt_organization_name_ = ReadIniSettingString_("Settings", "DmarcRptOrganizationName", "hMailServer");

      // Which schema the aggregate report itself is written in. 1 is RFC 7489
      // Appendix C; 2 is RFC 9990 (DMARCbis), which changes the namespace, drops
      // pct, adds np/testing/discovery_method and makes the DKIM selector
      // mandatory.
      //
      // 1 is the default and has to be. A report is only worth sending if the
      // domain it is addressed to can read it, and essentially every report
      // processor deployed today parses 7489 and nothing else - so defaulting to
      // 2 would mean the domains this server reports on quietly stopped being
      // able to read their own reports. The value of shipping 2 at all is that
      // an operator whose report consumers have caught up can switch, and that
      // the day the balance tips the default is a one-line change rather than a
      // feature to write.
      v.dmarc_rpt_schema_version_ = ReadIniSettingInteger_("Settings", "DmarcRptSchemaVersion", 1);

      // Anything else is a typo, and a typo must not decide what goes on the
      // wire. Falling back to 1 rather than refusing to report: the value only
      // selects a spelling of a report this server was going to send anyway, and
      // the 7489 spelling is the one that is certain to be understood. Reported
      // rather than corrected silently, because an administrator who asked for 2
      // is owed the reason their reports are still going out as 7489 - and this
      // cannot fire on a stock server, where the key is absent and the default
      // is in range.
      if (v.dmarc_rpt_schema_version_ != 1 && v.dmarc_rpt_schema_version_ != 2)
      {
         String message;
         message.Format(_T("DmarcRptSchemaVersion is set to %d, which is not a DMARC aggregate report schema this server can write. Using 1 (RFC 7489 Appendix C) instead. Set it to 1, or to 2 for the RFC 9990 (DMARCbis) schema."),
                        v.dmarc_rpt_schema_version_);

         if (ReportThisLoad_(_T("DmarcRptSchemaVersion")))
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6210, "IniFileSettings::LoadSettings", message);
         }

         v.dmarc_rpt_schema_version_ = 1;
      }

      // What happens to a message the virus scanner could not examine. 0, the
      // shipped default, delivers it - the fail-open posture this server has
      // always had, now at least named and documented rather than implicit. 1
      // holds the message and retries it on the ordinary queue schedule, and
      // bounces it to the sender if the scanner is still unreachable when the
      // retry budget runs out; it never delivers unscanned mail and never holds
      // a message silently for ever. Kept in the ini rather than the settings
      // table deliberately: a new settings row would report HM5015 on every read
      // until every database in the field had been upgraded, and this is one
      // integer.
      v.av_fail_action_ = ReadIniSettingInteger_("Settings", "AVFailAction", 0);
      if (v.av_fail_action_ < 0 || v.av_fail_action_ > 1)
         v.av_fail_action_ = 0;

      // The hold's own schedule, deliberately independent of the SMTP delivery
      // retry budget - see the getters for why borrowing `smtpnooftries` (which
      // ships as 0) would have made the whole policy bounce on the first blip.
      v.av_fail_retry_minutes_ = ReadIniSettingInteger_("Settings", "AVFailRetryMinutes", 15);
      if (v.av_fail_retry_minutes_ < 1)
         v.av_fail_retry_minutes_ = 15;

      // Sixteen attempts at the default fifteen-minute interval is about four
      // hours of holding, which is the intended window - expressed in attempts
      // because attempts are what this policy advances itself. See the getter.
      v.av_fail_max_holds_ = ReadIniSettingInteger_("Settings", "AVFailMaxHolds", 16);
      if (v.av_fail_max_holds_ < 0)
         v.av_fail_max_holds_ = 16;

      // Per-name authentication lockout (see AccountLockout.h). Threshold 0 -
      // the default - disables the whole mechanism; the windows only matter
      // once it is on.
      v.account_lockout_threshold_ = ReadIniSettingInteger_("Settings", "AccountLockoutThreshold", 0);
      v.account_lockout_window_minutes_ = ReadIniSettingInteger_("Settings", "AccountLockoutWindowMinutes", 30);
      v.account_lockout_minutes_ = ReadIniSettingInteger_("Settings", "AccountLockoutMinutes", 30);
      if (v.account_lockout_window_minutes_ < 1)
         v.account_lockout_window_minutes_ = 30;
      if (v.account_lockout_minutes_ < 1)
         v.account_lockout_minutes_ = 30;

      // Tarpits (see AccountLogon::TarpitDelaySeconds and SMTPConnection::
      // TarpitRecipient_). Both off by default: a delay is a policy, and the
      // installations that upgrade onto this must behave as they did. Bounded
      // above because a pause longer than a protocol's idle timeout is a
      // disconnect with extra steps.
      v.logon_tarpit_seconds_ = ClampTarpitSeconds_(ReadIniSettingInteger_("Settings", "LogonTarpitSeconds", 0));
      v.smtp_tarpit_count_ = ReadIniSettingInteger_("Settings", "SmtpTarpitCount", 0);
      if (v.smtp_tarpit_count_ < 0)
         v.smtp_tarpit_count_ = 0;
      v.smtp_tarpit_delay_seconds_ = ClampTarpitSeconds_(ReadIniSettingInteger_("Settings", "SmtpTarpitDelaySeconds", 0));

      // Hybrid post-quantum key exchange first, classical curves after it. The
      // list is only a preference order - the group actually used is negotiated,
      // so a peer that does not implement the ML-KEM hybrids picks X25519 or one
      // of the NIST curves and nothing changes for it. See
      // SslContextInitializer::SetKeyExchangeGroups_ for the fallback that keeps
      // TLS working if OpenSSL rejects whatever is configured here.
      v.tls_key_exchange_groups_ = ReadIniSettingString_("Settings", "TlsKeyExchangeGroups", "X25519MLKEM768:SecP256r1MLKEM768:X25519:secp384r1:secp256r1");

      // Empty, unlike the group list above, and deliberately so. A default here would
      // pin the TLS 1.3 suites to whatever was current when this line was written and
      // silently keep them pinned as OpenSSL's own defaults moved on. Empty means "use
      // OpenSSL's", which is the better answer until an administrator says otherwise.
      v.tls_cipher_suites13_ = ReadIniSettingString_("Settings", "TlsCipherSuites13", "");

      // TLS session resumption and ticket management. See SslContextInitializer's
      // SetSessionResumption_ for what each one defends. Every default reproduces
      // today's behaviour exactly: tickets on, OpenSSL's own cache size and timeout, and
      // no ticket-key callback installed.
      v.tls_session_tickets_enabled_ = ReadIniSettingInteger_("Settings", "TlsSessionTicketsEnabled", 1) == 1;
      v.tls_session_cache_size_ = ReadIniSettingInteger_("Settings", "TlsSessionCacheSize", 0);
      v.tls_session_timeout_seconds_ = ReadIniSettingInteger_("Settings", "TlsSessionTimeoutSeconds", 0);

      v.tls_ticket_key_rotation_seconds_ = ReadIniSettingInteger_("Settings", "TlsTicketKeyRotationSeconds", 0);
      if (v.tls_ticket_key_rotation_seconds_ < 0)
         v.tls_ticket_key_rotation_seconds_ = 0;
      v.rest_api_port_ = ReadIniSettingInteger_("Settings", "RestApiPort", 0);
      v.rest_api_bind_address_ = ReadIniSettingString_("Settings", "RestApiBindAddress", "127.0.0.1");
      v.rest_api_certificate_file_ = ReadIniSettingString_("Settings", "RestApiCertificateFile", "");
      v.rest_api_private_key_file_ = ReadIniSettingString_("Settings", "RestApiPrivateKeyFile", "");
      v.acme_enabled_ = ReadIniSettingInteger_("Settings", "AcmeEnabled", 0) == 1;
      v.acme_directory_url_ = ReadIniSettingString_("Settings", "AcmeDirectoryUrl", "https://acme-v02.api.letsencrypt.org/directory");
      v.acme_contact_email_ = ReadIniSettingString_("Settings", "AcmeContactEmail", "");
      v.acme_domains_ = ReadIniSettingString_("Settings", "AcmeDomains", "");
      v.acme_certificate_directory_ = ReadIniSettingString_("Settings", "AcmeCertificateDirectory", "");
      v.acme_http_port_ = ReadIniSettingInteger_("Settings", "AcmeHttpPort", 80);
      v.acme_reuse_key_ = ReadIniSettingInteger_("Settings", "AcmeReuseKey", 1) == 1;
      v.web_services_http_port_ = ReadIniSettingInteger_("Settings", "WebServicesHttpPort", 0);
      v.web_services_https_port_ = ReadIniSettingInteger_("Settings", "WebServicesHttpsPort", 0);
      v.web_services_bind_address_ = ReadIniSettingString_("Settings", "WebServicesBindAddress", "0.0.0.0");
      v.web_services_certificate_file_ = ReadIniSettingString_("Settings", "WebServicesCertificateFile", "");
      v.web_services_private_key_file_ = ReadIniSettingString_("Settings", "WebServicesPrivateKeyFile", "");
      v.web_services_trusted_proxies_ = ReadIniSettingString_("Settings", "WebServicesTrustedProxies", "");
      v.mta_sts_hosting_enabled_ = ReadIniSettingInteger_("Settings", "MtaStsHostingEnabled", 1) == 1;
      v.mta_sts_policy_mode_ = ReadIniSettingString_("Settings", "MtaStsPolicyMode", "enforce");
      v.mta_sts_policy_max_age_ = ReadIniSettingInteger_("Settings", "MtaStsPolicyMaxAge", 604800);
      v.mta_sts_policy_mx_ = ReadIniSettingString_("Settings", "MtaStsPolicyMx", "");
      v.autoconfig_enabled_ = ReadIniSettingInteger_("Settings", "AutoconfigEnabled", 1) == 1;
      v.autoconfig_client_host_ = ReadIniSettingString_("Settings", "AutoconfigClientHost", "");
      v.rewrite_envelope_from_when_forwarding_ = ReadIniSettingInteger_("Settings", "RewriteEnvelopeFromWhenForwarding", 0) == 1;
      v.srs_enabled_ = ReadIniSettingInteger_("Settings", "SRSEnabled", 0) == 1;
      v.srs_secret_ = ReadIniSettingString_("Settings", "SRSSecret", "");
      v.batv_enabled_ = ReadIniSettingInteger_("Settings", "BATVEnabled", 0) == 1;
      v.outbound_pipelining_ = ReadIniSettingInteger_("Settings", "OutboundPipelining", 1) == 1;
      v.outbound_chunking_ = ReadIniSettingInteger_("Settings", "OutboundChunking", 1) == 1;
      v.smtp_authenticated_sender_check_ = ReadIniSettingInteger_("Settings", "SmtpAuthenticatedSenderCheck", 0) == 1;
      v.metrics_history_days_ = ReadIniSettingInteger_("Settings", "MetricsHistoryDays", 7);
      if (v.metrics_history_days_ < 0)
         v.metrics_history_days_ = 0;
      v.batv_secret_ = ReadIniSettingString_("Settings", "BATVSecret", "");
      v.max_submissions_per_ip_per_minute_ = ReadIniSettingInteger_("Settings", "MaxSubmissionsPerIPPerMinute", 0);
      v.pop3_login_delay_seconds_ = ReadIniSettingInteger_("Settings", "Pop3LoginDelaySeconds", 0);
      v.smtp_proxy_protocol_enabled_ = ReadIniSettingInteger_("Settings", "SMTPProxyProtocolEnabled", 0) == 1;
      v.smtp_proxy_protocol_trusted_ips_ = ReadIniSettingString_("Settings", "SMTPProxyProtocolTrustedIPs", "");
      v.smtp_xclient_enabled_ = ReadIniSettingInteger_("Settings", "SMTPXClientEnabled", 0) == 1;
      v.smtp_xclient_trusted_ips_ = ReadIniSettingString_("Settings", "SMTPXClientTrustedIPs", "");
      v.quarantine_enabled_ = ReadIniSettingInteger_("Settings", "QuarantineEnabled", 0) == 1;
      v.quarantine_retention_days_ = ReadIniSettingInteger_("Settings", "QuarantineRetentionDays", 30);
      v.message_trace_enabled_ = ReadIniSettingInteger_("Settings", "MessageTraceEnabled", 0) == 1;
      v.message_trace_retention_days_ = ReadIniSettingInteger_("Settings", "MessageTraceRetentionDays", 30);
      v.windows_event_log_enabled_ = ReadIniSettingInteger_("Settings", "WindowsEventLogEnabled", 1) == 1;
      v.windows_event_log_level_ = ReadIniSettingInteger_("Settings", "WindowsEventLogLevel", 2);

      // Severities run 1 (Critical) to 4 (Low); anything outside is folded to the
      // nearest end rather than passed to the sink, whose own defensive clamp
      // exists for the value that arrives between this load and the next.
      if (v.windows_event_log_level_ < 1)
         v.windows_event_log_level_ = 1;

      if (v.windows_event_log_level_ > 4)
         v.windows_event_log_level_ = 4;
      v.dmarc_tree_walk_enabled_ = ReadIniSettingInteger_("Settings", "DmarcTreeWalkEnabled", 1) == 1;
      v.spf_void_lookup_limit_ = ReadIniSettingInteger_("Settings", "SpfVoidLookupLimit", 2);
      v.reject_full_mailbox_at_rcpt_ = ReadIniSettingInteger_("Settings", "RejectFullMailboxAtRcpt", 1) == 1;
      v.dkim_accept_sha1_ = ReadIniSettingInteger_("Settings", "DkimAcceptSha1", 0) == 1;
      v.quota_warning_percent_ = ReadIniSettingInteger_("Settings", "QuotaWarningPercent", 90);
      v.archive_retention_days_ = ReadIniSettingInteger_("Settings", "ArchiveRetentionDays", 0);
      v.database_statement_timeout_ = ReadIniSettingInteger_("Settings", "DatabaseStatementTimeout", 30);
      v.imap_expunge_retention_records_ = ReadIniSettingInteger_("Settings", "IMAPExpungeRetentionRecords", 5000);

      // Both are "0 means no limit", so a negative value is folded into that rather
      // than being passed on to a backend that would read it as an enormous one.
      if (v.database_statement_timeout_ < 0)
         v.database_statement_timeout_ = 0;

      if (v.imap_expunge_retention_records_ < 0)
         v.imap_expunge_retention_records_ = 0;

      v.metrics_per_domain_enabled_ = ReadIniSettingInteger_("Settings", "MetricsPerDomainEnabled", 0) == 1;

      v.filter_hook_url_ = ReadIniSettingString_("Settings", "FilterHookUrl", "");
      v.filter_hook_timeout_seconds_ = ReadIniSettingInteger_("Settings", "FilterHookTimeoutSeconds", 10);
      v.filter_hook_fail_closed_ = ReadIniSettingInteger_("Settings", "FilterHookFailClosed", 0) == 1;
      v.filter_hook_reject_score_ = ReadIniSettingInteger_("Settings", "FilterHookRejectScore", 100);
      v.filter_hook_max_message_size_kb_ = ReadIniSettingInteger_("Settings", "FilterHookMaxMessageSizeKB", 10240);

      if (v.filter_hook_timeout_seconds_ < 1)
         v.filter_hook_timeout_seconds_ = 1;
      v.password_policy_minimum_length_ = ReadIniSettingInteger_("Settings", "PasswordPolicyMinimumLength", 0);
      v.password_policy_require_mixed_case_ = ReadIniSettingInteger_("Settings", "PasswordPolicyRequireMixedCase", 0) == 1;
      v.password_policy_require_digit_ = ReadIniSettingInteger_("Settings", "PasswordPolicyRequireDigit", 0) == 1;
      v.password_policy_require_non_alphanumeric_ = ReadIniSettingInteger_("Settings", "PasswordPolicyRequireNonAlphanumeric", 0) == 1;
      v.password_policy_reject_common_ = ReadIniSettingInteger_("Settings", "PasswordPolicyRejectCommon", 0) == 1;
      v.password_policy_history_count_ = ReadIniSettingInteger_("Settings", "PasswordPolicyHistoryCount", 0);
      v.password_policy_maximum_age_days_ = ReadIniSettingInteger_("Settings", "PasswordPolicyMaximumAgeDays", 0);
      v.max_outbound_per_destination_per_minute_ = ReadIniSettingInteger_("Settings", "MaxOutboundPerDestinationPerMinute", 0);
      v.m_sDisableAUTHList = ReadIniSettingString_("Settings", "DisableAUTHList", "");

      std::shared_ptr<const Values> published = block;
#if defined(__cpp_lib_atomic_shared_ptr)
      values_.store(published);
#else
      std::atomic_store(&values_, published);
#endif

      // After the block, so a reader that sees the new generation loads this block
      // or a later one (Current).
      values_generation_.fetch_add(1, std::memory_order_release);

      // After the block, so a reader that sees the flag set finds the pattern in
      // the block it loads; an empty pattern is no failure either way.
      simulate_database_failure_enabled_ = !v.simulate_database_failure_for_.IsEmpty();
   }

   std::shared_ptr<const IniFileSettings::Values>
   IniFileSettings::Current() const
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // The published block, through a copy this thread keeps of the last one it
   // loaded. The server is built as C++14 on Windows, where std::atomic_load of a
   // shared_ptr takes ONE spin lock that every shared_ptr atomic in the process
   // shares - and every getter on every thread would take it: four per log line,
   // several per database statement. A block is replaced only by a publish, so
   // the thread asks the generation counter, one lock-free load, whether one has
   // happened since, and goes to the shared pointer only when one has. The copy it
   // keeps holds that block alive until the thread next reads a setting, which
   // costs one block per idle thread and nothing else.
   //---------------------------------------------------------------------------()
   {
      struct Cached
      {
         const IniFileSettings *owner = nullptr;
         unsigned long long generation = 0;
         std::shared_ptr<const Values> values;
      };

      thread_local Cached cached;

      // Acquire, paired with the release in BuildAndPublish_: a generation read
      // here was published after its block was stored, so the load below finds that
      // block or a later one. Caching a later block under an earlier generation
      // only means the next read loads it again.
      const unsigned long long generation = values_generation_.load(std::memory_order_acquire);
      if (cached.owner == this && cached.generation == generation && cached.values)
         return cached.values;

#if defined(__cpp_lib_atomic_shared_ptr)
      std::shared_ptr<const Values> values = values_.load();
#else
      std::shared_ptr<const Values> values = std::atomic_load(&values_);
#endif

      cached.owner = this;
      cached.generation = generation;
      cached.values = values;

      return values;
   }

   void
   IniFileSettings::Reload()
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // What a save does, for a caller that has stored nothing itself: the overlay
   // re-read from the store, a new block built in ScopeNow and published, and then
   // - with the reload lock released - the subscribers of every key that moved.
   //---------------------------------------------------------------------------()
   {
      std::vector<Subscription> due;

      {
         boost::lock_guard<boost::recursive_mutex> guard(reload_mutex_);
         due = ReloadLocked_();
      }

      Tell_(due);
   }

   std::vector<IniFileSettings::Subscription>
   IniFileSettings::ReloadLocked_()
   {
      RefreshDatabaseSettings_();

      std::shared_ptr<const Values> before = Current();
      BuildAndPublish_(ScopeNow, true);
      std::shared_ptr<const Values> after = Current();

      return Due_(before, after);
   }

   void
   IniFileSettings::RefreshDatabaseSettings_()
   {
      {
         boost::lock_guard<boost::recursive_mutex> guard(database_settings_mutex_);

         // No store loaded - the bootstrap before the database is open, a command
         // line with no server - and the file is the only source there is. The
         // build below reads it.
         if (!database_settings_loaded_)
            return;
      }

      // The rows, READ ONLY (IniSettingStore::ReadStoredValues): the reconcile that
      // writes the file and the table is a start's and a reinitialise's, because run
      // here it raced other saves' store writes and could undo them. A table that
      // cannot be read keeps the overlay as it was, rather than dropping every
      // stored value to the file's copy mid-flight.
      std::map<String, String> resolved;
      if (!IniSettingStore::ReadStoredValues(resolved))
      {
         LOG_APPLICATION("IniFileSettings: the settings store could not be read back after a save, so the settings in effect are the ones loaded before it, with the saved value over them.");
         return;
      }

      boost::lock_guard<boost::recursive_mutex> guard(database_settings_mutex_);

      // The overrides the last start or reinitialise loaded, applied again and NOT
      // re-read: a key forced from [SettingsOverride] goes on winning across saves,
      // and a change to that section waits for a reinitialise (Apply now) or a start,
      // which re-read it and announce it (HM5807). An override of a key marked
      // restart waits for a start, as every restart key does.
      for (auto iter = start_overrides_.begin(); iter != start_overrides_.end(); iter++)
         resolved[(*iter).first] = (*iter).second;

      database_settings_ = resolved;
   }

   void
   IniFileSettings::Subscribe(const String &name, std::function<void()> subscriber)
   {
      std::vector<String> names;
      names.push_back(name);
      Subscribe(names, subscriber);
   }

   void
   IniFileSettings::Subscribe(const std::vector<String> &names, std::function<void()> subscriber)
   {
      if (names.empty() || !subscriber)
         return;

      Subscription subscription;
      subscription.names = names;
      subscription.call = subscriber;

      boost::lock_guard<boost::mutex> guard(subscriptions_mutex_);
      subscriptions_.push_back(subscription);
   }

   std::vector<IniFileSettings::Subscription>
   IniFileSettings::Due_(const std::shared_ptr<const Values> &before, const std::shared_ptr<const Values> &after)
   {
      std::vector<Subscription> subscriptions;
      {
         boost::lock_guard<boost::mutex> guard(subscriptions_mutex_);
         subscriptions = subscriptions_;
      }

      std::vector<Subscription> due;

      const String nothing;

      for (size_t i = 0; i < subscriptions.size(); i++)
      {
         const Subscription &subscription = subscriptions[i];

         bool moved = false;
         for (size_t n = 0; n < subscription.names.size() && !moved; n++)
         {
            auto was = before->resolved.find(subscription.names[n]);
            auto now = after->resolved.find(subscription.names[n]);

            const String &wasValue = was == before->resolved.end() ? nothing : (*was).second;
            const String &nowValue = now == after->resolved.end() ? nothing : (*now).second;

            moved = wasValue != nowValue;
         }

         if (moved)
            due.push_back(subscription);
      }

      return due;
   }

   void
   IniFileSettings::Tell_(const std::vector<Subscription> &due)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Calls the subscribers a reload found due, with the reload lock RELEASED - an
   // OpenTelemetry exporter's restart joins a worker that may be inside a connect
   // or a POST for seconds, and no other save or load should wait for that. One
   // call at a time, under a lock of their own: every subscriber reads the value
   // it takes from the published block when it runs, so whichever call runs last
   // runs after the last publish and takes the latest value - two saves of one key
   // told out of order cannot leave the subsystem on the older one, as a read and a
   // set racing another save's read and set could. Recursive, because a subscriber
   // may itself save a setting.
   //---------------------------------------------------------------------------()
   {
      if (due.empty())
         return;

      boost::lock_guard<boost::recursive_mutex> guard(notify_mutex_);

      for (size_t i = 0; i < due.size(); i++)
      {
         const Subscription &subscription = due[i];

         // Contained: a subscriber that fails must not turn a save that succeeded
         // into an error, and must not keep the others from being told.
         try
         {
            subscription.call();
         }
         catch (...)
         {
            LOG_APPLICATION("IniFileSettings: a subscriber to " + StringParser::JoinVector(subscription.names, _T(", ")) +
               " failed while taking a new value. The value is stored and published; the subsystem takes it at the next reinitialise.");
         }
      }
   }

   void
   IniFileSettings::LatchRunningValues()
   {
      running_values_latched_ = true;
   }

   bool
   IniFileSettings::ReportThisLoad_(const String &key) const
   {
      BuildContext *build = build_context;
      if (!build || !build->reload || !build->running)
         return true;

      auto was = build->running->resolved.find(key);
      auto now = build->block->resolved.find(key);
      if (was == build->running->resolved.end() || now == build->block->resolved.end())
         return true;

      return (*was).second != (*now).second;
   }

   bool
   IniFileSettings::GetApplies(const String &name, String &applies, String &spelling)
   {
      const IniSettingsBaseline::Setting *setting = FindBaselineSetting(name);
      if (!setting)
         return false;

      applies = AppliesWord(setting->Applies);
      spelling = setting->Name;
      return true;
   }

   bool
   IniFileSettings::GetLevels(const String &name, String &levels)
   {
      const IniSettingsBaseline::Setting *setting = FindBaselineSetting(name);
      if (!setting)
         return false;

      switch (setting->Levels)
      {
      case IniSettingsBaseline::LevelAccount:
         levels = _T("account");
         break;
      case IniSettingsBaseline::LevelDomain:
         levels = _T("domain");
         break;
      default:
         levels = _T("server");
         break;
      }

      return true;
   }

   void
   IniFileSettings::GetAllApplies(std::vector<std::pair<String, String> > &marks)
   {
      marks.clear();

      for (size_t i = 0; i < IniSettingsBaseline::SettingsCount; i++)
      {
         const IniSettingsBaseline::Setting &setting = IniSettingsBaseline::Settings[i];
         marks.push_back(std::make_pair(String(setting.Name), String(AppliesWord(setting.Applies))));
      }
   }

   const char *
   IniFileSettings::NoApplyMarkSentence()
   {
      // One sentence for both surfaces, so COM's refusal and REST's applies null say
      // the same thing about the same name.
      return "This build does not read a [Settings] key of that name through its settings loader, so it has no mark saying when a change to it applies. "
             "The keys it reads, and the mark of each, are in IniSettingsBaseline.h and in the applies map of GET /api/v1/settings/ini; "
             "a key a script of the administrator's own reads, or one the webmail reads per request, applies whenever it is next read.";
   }

   String
   IniFileSettings::ResolveStored_(const String &name, const String &defaultValue, bool isInteger)
   {
      if (isInteger)
         return StringParser::IntToString(ResolveInteger_(_T("Settings"), name, _ttoi(defaultValue.c_str())));

      return ResolveString_(_T("Settings"), name, defaultValue);
   }

   std::vector<IniFileSettings::PendingSetting>
   IniFileSettings::GetPendingSettings()
   {
      std::vector<PendingSetting> pending;

      std::shared_ptr<const Values> running = Current();

      for (size_t i = 0; i < IniSettingsBaseline::SettingsCount; i++)
      {
         const IniSettingsBaseline::Setting &setting = IniSettingsBaseline::Settings[i];

         if (setting.Applies == IniSettingsBaseline::AppliesNow)
            continue;

         // A key the running block never resolved is one this process has not
         // loaded at all yet, and nothing is running on it to differ from.
         auto found = running->resolved.find(setting.Name);
         if (found == running->resolved.end())
            continue;

         // Read as the loader reads it - the overlay, an override, the file, the
         // default - and compared as the loader resolved it, so "0009" stored for a
         // running 9 is not a difference, and a value put back is not pending.
         String stored = ResolveStored_(setting.Name, setting.Default, setting.IsInteger);

         if (stored == (*found).second)
            continue;

         PendingSetting entry;
         entry.name = setting.Name;
         entry.stored = stored;
         entry.running = (*found).second;
         entry.applies = AppliesWord(setting.Applies);
         pending.push_back(entry);
      }

      return pending;
   }

   bool
   IniFileSettings::IsPending(const String &name)
   {
      std::vector<PendingSetting> pending = GetPendingSettings();

      for (size_t i = 0; i < pending.size(); i++)
      {
         if (pending[i].name.CompareNoCase(name) == 0)
            return true;
      }

      return false;
   }

   int
   IniFileSettings::ReadPasswordHashWorkFactor_(const String &sKey, int minimum, int maximum)
   {
      int value = ReadIniSettingInteger_("Settings", sKey, 0);

      if (value == 0)
         return 0;

      if (value >= minimum && value <= maximum)
         return value;

      String message;
      message.Format(_T("%s is set to %d, which is outside the range %d to %d this server will derive a password hash with. Using the built-in default instead. Set it to 0 to choose the default deliberately."),
                     sKey.c_str(), value, minimum, maximum);

      if (ReportThisLoad_(sKey))
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5561, "IniFileSettings::LoadSettings", message);

      return 0;
   }

   bool
   IniFileSettings::GetDatabaseSettingsExists()
   {
      if (sqldbtype_ == HM::DatabaseSettings::TypeUnknown)
         return false;

      return true;
   }


   void
   IniFileSettings::WriteIniSetting_(const String &sSection, const String &sKey, const String &sValue)
   {
      WritePrivateProfileString(sSection, sKey, sValue, GetInitializationFile() );
   }

   void
   IniFileSettings::WriteIniSetting_(const String &sSection, const String &sKey, int Value)
   {
      String sValue = StringParser::IntToString(Value);
      WritePrivateProfileString(sSection, sKey, sValue, GetInitializationFile() );
   }

   void
   IniFileSettings::LoadDatabaseSettings()
   {
      boost::lock_guard<boost::recursive_mutex> reloadGuard(reload_mutex_);

      IniSettingStore store;

      std::map<String, String> resolved;

      // Synchronize reports its own reason when it fails. Carrying on rather than
      // refusing to start is deliberate: the file still holds a complete copy of
      // the settings, written by this same store, so a server whose settings table
      // is momentarily unreadable runs on that copy rather than not running at all.
      bool stored = store.Synchronize(resolved);

      // The door. Read from the file, applied over everything, and read EVEN WHEN
      // the table could not be - because a database that cannot be reached, or that
      // holds a value which stops the server starting, is the case this exists for.
      std::map<String, String> overrides;
      IniSettingStore::ReadOverrides(overrides);

      std::set<String> overridden;

      for (auto iter = overrides.begin(); iter != overrides.end(); iter++)
      {
         resolved[(*iter).first] = (*iter).second;
         overridden.insert((*iter).first);
      }

      if (!overrides.empty())
      {
         std::vector<String> names;
         for (auto iter = overrides.begin(); iter != overrides.end(); iter++)
            names.push_back((*iter).first);

         // Every start, by name, and as an error rather than a log line. An override
         // is a deliberate deviation from the stored configuration that no page will
         // show and no backup will carry, and the way it stops being forgotten is
         // that it says so every time the service starts.
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5807, "IniFileSettings::LoadDatabaseSettings",
            Formatter::Format("These settings are being forced from the [SettingsOverride] section of hMailServer.INI and are NOT what the database holds: {0}. That section is honoured on purpose - it is how a server is started when its stored configuration is unreachable or wrong - but it is invisible to the Control Panel, to the REST API and to every backup. Remove it once the stored value is right.",
               StringParser::JoinVector(names, _T(", "))));
      }

      if (!stored && overrides.empty())
      {
         // Nothing to overlay and nothing stored: leave the overlay unloaded so that
         // every read goes to the file, which is what the bootstrap has always done.
         return;
      }

      boost::lock_guard<boost::recursive_mutex> guard(database_settings_mutex_);

      database_settings_ = resolved;
      file_overrides_ = overridden;
      start_overrides_ = overrides;
      database_settings_loaded_ = true;
   }

   void
   IniFileSettings::ForgetDatabaseSettings()
   {
      boost::lock_guard<boost::recursive_mutex> guard(database_settings_mutex_);

      database_settings_.clear();
      file_overrides_.clear();
      start_overrides_.clear();
      database_settings_loaded_ = false;
   }

   String
   IniFileSettings::GetSettingsValue(const String &key)
   {
      // Straight through the ordinary read path, so a value fetched over COM is the
      // same value the server itself would read for that key - overlay and override
      // included. A separate implementation here would be a second answer to the
      // same question, and the two would eventually disagree.
      return ReadIniSettingString_(_T("Settings"), key, _T(""));
   }

   bool
   IniFileSettings::WriteSettingsValue(const String &key, const String &requestedValue)
   {
      // Trimmed here as well as in the store, so the overlay below holds what the
      // store and the file hold rather than what was typed.
      String value = requestedValue;
      value.Trim();

      std::vector<Subscription> due;

      {
         // The store write and the reload under ONE hold of the reload lock, so no
         // other save's reload reads the store between this write and this publish,
         // and no LoadSettings() publishes this value first - which would leave the
         // reload below finding nothing moved and telling no subscriber.
         boost::lock_guard<boost::recursive_mutex> reloadGuard(reload_mutex_);

         if (!IniSettingStore::WriteSetting(key, value))
            return false;

         // The overlay is updated only after the store took the value. Updating it
         // first would make this process read back a value nothing else can see.
         //
         // An override is NOT disturbed: what the file forces is what the server is
         // running on, and quietly replacing it here would make the overlay disagree
         // with the next start. The administrator is told instead, because a save that
         // is stored and then shadowed is the one case where "saved" and "in effect"
         // are not the same thing.
         bool shadowed = false;

         {
            boost::lock_guard<boost::recursive_mutex> guard(database_settings_mutex_);

            shadowed = file_overrides_.find(key) != file_overrides_.end();

            // A key kept in the file only is never in the overlay: it is read from the
            // file, which is where the store just put it.
            if (database_settings_loaded_ && !shadowed && !IniSettingStore::IsFileOnlyName(key))
               database_settings_[key] = value;
         }

         if (shadowed)
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5808, "IniFileSettings::WriteSettingsValue",
               Formatter::Format("The setting '{0}' was stored, but the [SettingsOverride] section of hMailServer.INI forces a different value for it, so the stored one will not take effect. Remove the key from that section to let the stored value be used.", key));
         }

         // And published: a key marked now is in effect when this returns, one marked
         // reinitialize or restart is pending until what applies it has happened.
         due = ReloadLocked_();
      }

      // The subscribers, with the reload lock released (Tell_).
      Tell_(due);

      return true;
   }

   int
   IniFileSettings::ClampTarpitSeconds_(int seconds)
   {
      if (seconds < 0)
         return 0;
      if (seconds > TARPIT_MAX_SECONDS)
         return TARPIT_MAX_SECONDS;
      return seconds;
   }

   bool
   IniFileSettings::SetSmtpTarpitCount(int count)
   {
      if (count < 0)
         count = 0;

      // The reload inside WriteSettingsValue publishes the value it stored.
      return WriteSettingsValue(_T("SmtpTarpitCount"), StringParser::IntToString(count));
   }

   bool
   IniFileSettings::SetSmtpTarpitDelaySeconds(int seconds)
   {
      seconds = ClampTarpitSeconds_(seconds);

      return WriteSettingsValue(_T("SmtpTarpitDelaySeconds"), StringParser::IntToString(seconds));
   }

   bool
   IniFileSettings::RemoveSettingsValue(const String &key)
   {
      std::vector<Subscription> due;

      {
         // As WriteSettingsValue: the removal and the reload under one hold, so no
         // other save's reload can read the row before it goes and the file after,
         // and write the line back.
         boost::lock_guard<boost::recursive_mutex> reloadGuard(reload_mutex_);

         if (!IniSettingStore::RemoveSetting(key))
            return false;

         {
            boost::lock_guard<boost::recursive_mutex> guard(database_settings_mutex_);

            // An override is left where it is, for the same reason as above: it is what
            // the server is running on, and it is the file's statement rather than the
            // store's. Erasing the overlay entry would have this process read the
            // default while the next start read the override.
            if (database_settings_loaded_ && file_overrides_.find(key) == file_overrides_.end())
               database_settings_.erase(key);
         }

         // Back to its default, in effect or pending as WriteSettingsValue says.
         due = ReloadLocked_();
      }

      Tell_(due);

      return true;
   }

   void
   IniFileSettings::GetSettingsNames(std::vector<String> &names)
   {
      IniSettingStore::ReadSettingNames(names);
   }

   void
   IniFileSettings::GetOverriddenNames(std::set<String> &names)
   {
      boost::lock_guard<boost::recursive_mutex> guard(database_settings_mutex_);

      names = file_overrides_;
   }

   String
   IniFileSettings::ReadIniSettingString_(const String &sSection, const String &sKey, const String &sDefault)
   {
      // A build of the block on this thread records what the key resolved to and,
      // for a key its scope must not apply, answers the running value instead.
      BuildContext *build = sSection.CompareNoCase(_T("Settings")) == 0 ? build_context : nullptr;

      if (build)
      {
         String running;
         if (KeepRunningValue(*build, sKey, running))
         {
            build->block->resolved[sKey] = running;
            return running;
         }
      }

      String value = ResolveString_(sSection, sKey, sDefault);

      if (build)
         build->block->resolved[sKey] = value;

      return value;
   }

   String
   IniFileSettings::ResolveString_(const String &sSection, const String &sKey, const String &sDefault)
   {
      // [Settings] only, and only once the store has been loaded. Every other
      // section is read from the file and nowhere else, because those are the
      // sections that say where the database IS - so a value from the database could
      // never be needed to reach it, and allowing one would be a way to point a
      // server at a different database by writing to the one it already has.
      if (sSection.CompareNoCase(_T("Settings")) == 0)
      {
         boost::lock_guard<boost::recursive_mutex> guard(database_settings_mutex_);

         if (database_settings_loaded_)
         {
            auto found = database_settings_.find(sKey);
            if (found != database_settings_.end())
               return (*found).second;
         }
      }

      // The buffer must be large enough for the longest value we may store. Most
      // settings are short, but a DPAPI-protected secret (e.g. the database
      // password, OAuth2 HMAC secret or password pepper) is a base64 envelope that
      // easily exceeds 255 characters. A buffer that is too small silently
      // truncates the value, which corrupts the secret on read (a truncated DPAPI
      // blob fails to decrypt and yields an empty password, so the server can no
      // longer connect to its own database). Use a generous buffer to be safe.
      const DWORD nBufferSize = 4096;
      TCHAR Value[nBufferSize];
      GetPrivateProfileString( sSection, sKey, sDefault, Value, nBufferSize, GetInitializationFile() );
      return Value;
   }

   int
   IniFileSettings::ReadIniSettingInteger_(const String &sSection, const String &sKey, int iDefault)
   {
      // As ReadIniSettingString_, with the number kept as text: a key held at its
      // running value comes back as the same number, and the pending list compares
      // numbers as the loader read them rather than as the store spells them.
      BuildContext *build = sSection.CompareNoCase(_T("Settings")) == 0 ? build_context : nullptr;

      if (build)
      {
         String running;
         if (KeepRunningValue(*build, sKey, running))
         {
            build->block->resolved[sKey] = running;
            return _ttoi(running.c_str());
         }
      }

      int value = ResolveInteger_(sSection, sKey, iDefault);

      if (build)
         build->block->resolved[sKey] = StringParser::IntToString(value);

      return value;
   }

   int
   IniFileSettings::ResolveInteger_(const String &sSection, const String &sKey, int iDefault)
   {
      if (sSection.CompareNoCase(_T("Settings")) == 0)
      {
         boost::lock_guard<boost::recursive_mutex> guard(database_settings_mutex_);

         if (database_settings_loaded_)
         {
            auto found = database_settings_.find(sKey);
            if (found != database_settings_.end())
            {
               // Deliberately mirrors GetPrivateProfileInt rather than being
               // stricter than it: that function stops at the first character that
               // is not part of a number and yields 0 for a value that does not
               // start with one, so "5x" is 5 and "x" is 0. _ttoi does the same.
               // A reconciled value that parses differently from the file it came
               // from would be a difference nobody could see.
               return _ttoi((*found).second.c_str());
            }
         }
      }

      int iValue = GetPrivateProfileInt( sSection, sKey, iDefault, GetInitializationFile() );
      return iValue;
   }

   void
   IniFileSettings::SetInitializationFile(const String &file)
   {
      ini_file_ = file;
   }

   String
   IniFileSettings::GetInitializationFile() 
   {
      if (ini_file_.IsEmpty())
      {
         String AppPath = Utilities::GetBinDirectory();

         ini_file_ = AppPath;

         // The separator this platform uses. This is the one path that has to be
         // right before any of the rest can even be read: nothing in this server
         // knows anything until hMailServer.INI has been found.
         if (ini_file_.Right(1) != FileUtilities::PathSeparator)
            ini_file_ += FileUtilities::PathSeparator;

         ini_file_ += "hMailServer.ini";

#ifdef HM_PLATFORM_POSIX
         // A package installs the binary under /usr/bin and its configuration
         // under /etc, which is where an administrator looks for it and where a
         // distribution's own tooling expects it. The executable's own directory
         // is still tried first, because a build tree and a tarball both keep the
         // file beside the binary and neither should need an argument.
         if (!FileUtilities::Exists(ini_file_))
         {
            const String packaged = _T("/etc/hmailserver/hMailServer.ini");

            // The file itself, when a package installed it - the ordinary case.
            //
            // And when the file is absent but its directory is not: that is a
            // package installation whose configuration has been removed (an
            // apt-get remove leaves dpkg's conffile record behind, so a later
            // reinstall does not put it back), and the next thing an
            // administrator is told to run is --set-admin-password. Without
            // this, that command CREATES /usr/bin/hMailServer.ini and writes
            // the hash there, and from then on the file beside the binary wins
            // over /etc on every start - a shadowing configuration nobody knows
            // they have. A build tree and a tarball have no /etc/hmailserver at
            // all and keep the file beside the binary, which is right for them.
            if (FileUtilities::Exists(packaged) ||
                FileUtilities::Exists(_T("/etc/hmailserver")))
            {
               ini_file_ = packaged;
            }
         }
#endif
      }

      return ini_file_;
   }
   
   String
   IniFileSettings::GetLogDirectory() 
   { 
      if (log_directory_.IsEmpty())
      {
         TCHAR Value[255];
         GetPrivateProfileString( _T("Directories"), _T("LogFolder"), _T(""), Value, 255, GetInitializationFile() );
         log_directory_ = ResolveAgainstProgramDirectory_(Value);
      }

      return log_directory_; 
   }

   String 
   IniFileSettings::GetLanguageDirectory() const
   {
      return app_directory_ + "Languages";
   }

   String
   IniFileSettings::ResolveAgainstProgramDirectory_(const String &value) const
   {
      if (value.IsEmpty() || FileUtilities::IsFullPath(value))
         return value;

      // app_directory_ is read before any of the values this resolves and
      // already ends with the platform separator; Combine tolerates that.
      return FileUtilities::Combine(app_directory_, value);
   }

   String
   IniFileSettings::ResolveDataDirectory_(const String &programDirectory, const String &configured)
   {
      // As ResolveAgainstProgramDirectory_, against the directory given.
      String directory = configured;
      if (!directory.IsEmpty() && !FileUtilities::IsFullPath(directory))
         directory = FileUtilities::Combine(programDirectory, directory);

#ifdef HM_PLATFORM_POSIX
      if (directory.Right(1) == _T("/") || directory.Right(1) == _T("\\"))
         directory = directory.Left(directory.GetLength() -1);
#else
      if (directory.Right(1) == _T("\\"))
         directory = directory.Left(directory.GetLength() -1);
#endif

      return directory;
   }

   String
   IniFileSettings::ReadDataDirectory()
   {
      // ProgramFolder as LoadSettings reads it into app_directory_, ending with
      // this platform's separator, then DataFolder resolved against it. Both are
      // [Directories] keys, which are read from the file and nowhere else.
      String program = ReadIniSettingString_("Directories", "ProgramFolder", "");
      if (program.Right(1) != FileUtilities::PathSeparator)
         program += FileUtilities::PathSeparator;

      return ResolveDataDirectory_(program, ReadIniSettingString_("Directories", "DataFolder", ""));
   }

   bool
   IniFileSettings::CheckSettings(String &sErrorMessage)
   {
      String sIniFile = GetInitializationFile();

      String sLog;
      sLog.Format(_T("Configuration::CheckSettings - %s"), sIniFile.c_str());
      LOG_DEBUG(sLog);

      switch (GetDatabaseType())
      {
      case HM::DatabaseSettings::TypeMSSQLCompactEdition:
         {
            if (database_name_.IsEmpty())
            {
               sErrorMessage.Format(_T("The setting Database in the section Database could not be read from %s"), sIniFile.c_str());
               return false;
            }
            break;
         }
      default:
         {
            if (database_server_.IsEmpty())
            {
               sErrorMessage.Format(_T("The setting Server in the section Database could not be read from %s"), sIniFile.c_str());
               return false;
            }
            break;
         }
      }


      return true;
   }

   String 
   IniFileSettings::GetUserInterfaceLanguage()
   {
      TCHAR Value[255];

      GetPrivateProfileString( _T("Settings"), _T("UseLanguage"), _T("English"), Value, 255, GetInitializationFile() );
      return Value;

   }

   void 
   IniFileSettings::SetUserInterfaceLanguage(String sLanguage)
   {
      // Through the ordinary store-then-file path, like every other setting.
      //
      // This wrote the file itself and then recorded the value in the table, which
      // was the right order while the file was the store and is the wrong one now: a
      // failed row write would have left the file holding a language the store did
      // not have, and the next start would have seen a file edited away from its
      // stored value and correctly put it back - reverting a save that reported
      // success. UseLanguage remains the one [Settings] key that is neither loaded by
      // LoadSettings nor cached in a member - GetUserInterfaceLanguage reads the file
      // on every call - which is exactly why the file copy has to keep being written.
      WriteSettingsValue(_T("UseLanguage"), sLanguage);
   }

   int 
   IniFileSettings::GetNumberOfDatabaseConnections() const
   {
      return no_of_dbconnections_;
   }

   int 
   IniFileSettings::GetNumberOfDatabaseConnectionAttempts() const
   {
      return no_of_dbconnection_attempts_;
   }

   int 
   IniFileSettings::GetDBConnectionAttemptsDelay() const
   {
      return no_of_dbconnection_attempts_Delay;
   }


   String 
   IniFileSettings::GetAdministratorPassword()
   {
      return administrator_password_;
   }

   void 
   IniFileSettings::SetAdministratorPassword(const String &sNewPassword)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Updates the main hMailServer administration password found in hMailServer.ini
   //---------------------------------------------------------------------------()
   {
      administrator_password_ = HM::Crypt::Instance()->EnCrypt(sNewPassword, HM::Crypt::ETPBKDF2);

      WriteIniSetting_("Security", "AdministratorPassword", administrator_password_);
   }

   String
   IniFileSettings::GetAdministratorTotpSecret()
   {
      if (administrator_totp_secret_protected_.IsEmpty())
         return String();

      // Unprotected on each use rather than held in the clear: the secret is
      // needed for a moment per administrator logon, and this process holds the
      // plain form for exactly that moment.
      return HM::Crypt::Instance()->UnprotectSecret(administrator_totp_secret_protected_);
   }

   bool
   IniFileSettings::SetAdministratorTotpSecret(const String &base32Secret)
   {
      const String protectedSecret = base32Secret.IsEmpty() ? String() : HM::Crypt::Instance()->ProtectSecret(base32Secret);

      if (!base32Secret.IsEmpty() && protectedSecret.IsEmpty())
         return false;

      if (!WritePrivateProfileString(_T("Security"), _T("AdministratorTotpSecret"), protectedSecret, GetInitializationFile()))
         return false;

      administrator_totp_secret_protected_ = protectedSecret;
      return true;
   }

   void 
   IniFileSettings::SetProgramDirectory(const String &sNewVal)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Updates the program folder in hMailServer.ini. Normalised the way LoadSettings
   // normalises it and the derived DBScripts directory recomputed, so the running
   // server reads the same paths a restarted one would (issue #158).
   //---------------------------------------------------------------------------()
   {
      app_directory_ = sNewVal;
      if (!app_directory_.IsEmpty() && app_directory_.Right(1) != FileUtilities::PathSeparator)
         app_directory_ += FileUtilities::PathSeparator;

      dbscript_directory_ = app_directory_ + "DBScripts";

      WriteIniSetting_("Directories", "ProgramFolder", app_directory_);
   }

   void 
   IniFileSettings::SetDataDirectory(const String &sNewVal)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Updates a directory in hMailServer.ini.
   //---------------------------------------------------------------------------()
   {
      data_directory_ = sNewVal;
      WriteIniSetting_("Directories", "DataFolder", data_directory_);
   }

   void 
   IniFileSettings::SetTempDirectory(const String &sNewVal)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Updates a directory in hMailServer.ini.
   //---------------------------------------------------------------------------()
   {
      temp_directory_ = sNewVal;
      WriteIniSetting_("Directories", "TempFolder", temp_directory_);
   }

   void 
   IniFileSettings::SetEventDirectory(const String &sNewVal)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Updates a directory in hMailServer.ini.
   //---------------------------------------------------------------------------()
   {
      event_directory_ = sNewVal;
      WriteIniSetting_("Directories", "EventFolder", event_directory_);
   }

   void 
   IniFileSettings::SetDatabaseDirectory(const String &sNewVal)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Updates a directory in hMailServer.ini.
   //---------------------------------------------------------------------------()
   {
      database_directory_ = sNewVal;
      WriteIniSetting_("Directories", "DatabaseFolder", database_directory_);
   }

   void 
   IniFileSettings::SetLogDirectory(const String &sNewVal)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Updates a directory in hMailServer.ini.
   //---------------------------------------------------------------------------()
   {
      log_directory_ = sNewVal;
      WriteIniSetting_("Directories", "LogFolder", log_directory_);
   }

   void 
   IniFileSettings::SetDatabaseServer(const String &sNewVal)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Updates a directory in hMailServer.ini.
   //---------------------------------------------------------------------------()
   {
      database_server_ = sNewVal;
      WriteIniSetting_("Database", "Server", database_server_);
   }

   void 
   IniFileSettings::SetDatabaseName(const String &sNewVal)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Updates a directory in hMailServer.ini.
   //---------------------------------------------------------------------------()
   {
      database_name_ = sNewVal;
      WriteIniSetting_("Database", "Database", database_name_);
   }

   void 
   IniFileSettings::SetUsername(const String &sNewVal)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Updates a directory in hMailServer.ini.
   //---------------------------------------------------------------------------()
   {
      username_ = sNewVal;
      WriteIniSetting_("Database", "Username", username_);
   }

   void 
   IniFileSettings::SetPassword(const String &sNewVal)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Updates a directory in hMailServer.ini.
   //---------------------------------------------------------------------------()
   {
      password_ = sNewVal;

      // Prefer machine-scoped DPAPI so the database password in hMailServer.ini
      // cannot be decrypted off-box. Fall back to the legacy Blowfish scheme when
      // DPAPI is disabled or unavailable (so a non-empty password is never lost).
      if (GetProtectStoredSecretsWithDPAPI())
      {
         String protectedValue = Crypt::Instance()->EnCrypt(password_, Crypt::ETDPAPI);
         if (!protectedValue.IsEmpty() || password_.IsEmpty())
         {
            WriteIniSetting_("Database", "Password", protectedValue);
            WriteIniSetting_("Database", "PasswordEncryption", Crypt::ETDPAPI);
            return;
         }
#ifdef HM_PLATFORM_POSIX
         // No Blowfish here. On this platform the key-file store is the only
         // store, and a secret it could not protect is not written at all - the
         // same rule Crypt::ProtectSecret keeps - rather than written under a
         // scheme anybody with the file can reverse.
         ErrorManager::Instance()->ReportError(ErrorManager::High, 6418, "IniFileSettings::SetPassword",
            "The database password was NOT written: the secret store refused it (see the preceding error), and this build does not fall back to the reversible Blowfish scheme. Fix the key file and set the password again.");
         return;
#endif
      }

      WriteIniSetting_("Database", "Password", Crypt::Instance()->EnCrypt(password_, Crypt::ETBlowFish));
      WriteIniSetting_("Database", "PasswordEncryption", Crypt::ETBlowFish);
   }

   void 
   IniFileSettings::SetDatabaseType(HM::DatabaseSettings::SQLDBType type)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Updates a directory in hMailServer.ini.
   //---------------------------------------------------------------------------()
   {
      String sDatabaseType;
      switch (type)
      {
      case HM::DatabaseSettings::TypeMSSQLServer:
          sDatabaseType = _T("MSSQL");
          break;
      case HM::DatabaseSettings::TypeMYSQLServer:
         sDatabaseType = _T("MYSQL");
         break;
      case HM::DatabaseSettings::TypePGServer:
         sDatabaseType = _T("PostgreSQL");
         break;
      case HM::DatabaseSettings::TypeMSSQLCompactEdition:
         sDatabaseType = _T("MSSQLCE");
         break;
      default:
         return;
      }

      LOG_DEBUG("Setting database type to " + sDatabaseType);
      sqldbtype_ = type;

      WriteIniSetting_("Database", "Type", sDatabaseType);
   }

   void 
   IniFileSettings::SetDatabasePort(long lNewValue)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Updates a directory in hMailServer.ini.
   //---------------------------------------------------------------------------()
   {
      dbport_ = lNewValue;
      WriteIniSetting_("Database", "Port", dbport_);
   }

   void 
   IniFileSettings::SetIsInternalDatabase(bool newValue)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Updates a directory in hMailServer.ini.
   //---------------------------------------------------------------------------()
   {
      is_internal_database_ = newValue;

      WriteIniSetting_("Database", "Internal", is_internal_database_ ? 1 : 0);
   }

   void
   IniFileSettings::SetRewriteEnvelopeFromWhenForwarding(bool value)
   {
      // Store first, file second, as every write does now; the reload inside
      // WriteSettingsValue publishes it. Writing the file here and
      // recording the row afterwards - which is what this did - would let a failed
      // row write produce a file the next start reads as an administrator's edit and
      // puts back, reverting a change that reported success.
      WriteSettingsValue(_T("RewriteEnvelopeFromWhenForwarding"), value ? _T("1") : _T("0"));
   }

   String
   IniFileSettings::GetBinDirectory()
   {
      return FileUtilities::Combine(app_directory_, "Bin");
   }

   std::set<int> 
   IniFileSettings::GetAuthDisabledOnPorts()
   {
      const String disableAuthList = Current()->m_sDisableAUTHList;

      if (disableAuthList.IsEmpty())
      {
         std::set<int> empty;
         return empty;
      }

      std::vector<String> authDisabledOnPortsStr = StringParser::SplitString(disableAuthList, ",");

      std::set<int> authDisabledOnPorts;

      for (AnsiString port : authDisabledOnPortsStr)
      {
         authDisabledOnPorts.insert(atoi(port));
      }

      return authDisabledOnPorts;
   }

   bool
   IniFileSettings::IsArchiveDomain(const String &domain) const
   {
      // One block for the whole question: the set it tests against must be the one
      // it asked about being empty.
      std::shared_ptr<const Values> values = Current();

      if (values->archive_domains_.empty())
         return true;

      String lowered = domain;
      lowered.Trim();
      lowered.ToLower();

      return values->archive_domains_.find(lowered) != values->archive_domains_.end();
   }

   // ---- the tester -------------------------------------------------------------

   namespace
   {
      void FailSettingsTest_(const String &sentence)
      {
         throw std::logic_error(std::string(AnsiString(sentence).c_str()));
      }
   }

   void
   IniFileSettingsTester::SetOverlay_(const String &key, const String &value)
   {
      IniFileSettings *ini = IniFileSettings::Instance();
      boost::lock_guard<boost::recursive_mutex> guard(ini->database_settings_mutex_);
      ini->database_settings_[key] = value;
   }

   void
   IniFileSettingsTester::Publish_()
   {
      IniFileSettings::Instance()->BuildAndPublish_(IniFileSettings::ScopeNow, true);
   }

   bool
   IniFileSettingsTester::GetOverlay_(const String &key, String &value)
   {
      IniFileSettings *ini = IniFileSettings::Instance();
      boost::lock_guard<boost::recursive_mutex> guard(ini->database_settings_mutex_);
      auto found = ini->database_settings_.find(key);
      if (found == ini->database_settings_.end())
         return false;
      value = (*found).second;
      return true;
   }

   void
   IniFileSettingsTester::RestoreOverlay_(const String &key, bool had, const String &value)
   {
      IniFileSettings *ini = IniFileSettings::Instance();
      boost::lock_guard<boost::recursive_mutex> guard(ini->database_settings_mutex_);
      if (had)
         ini->database_settings_[key] = value;
      else
         ini->database_settings_.erase(key);
   }

   bool
   IniFileSettingsTester::OverlayLoaded_()
   {
      IniFileSettings *ini = IniFileSettings::Instance();
      boost::lock_guard<boost::recursive_mutex> guard(ini->database_settings_mutex_);
      return ini->database_settings_loaded_;
   }

   void
   IniFileSettingsTester::Test()
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // The swap under load, which is the test for the block: one thread publishes a
   // new block thousands of times, alternating one String key between two values
   // of different lengths, and calls the real Reload among them, while this thread
   // reads the key's getter ten thousand times and more. Every read must be one of
   // the two values - never a mixture, never empty, never a crash. Then the three
   // promises around it: a save of a key marked now is in effect when it returns
   // and tells its subscriber once; a key marked reinitialize is held at its
   // running value by a reload and is pending until the stored value is put back;
   // and the pending list is recomputed, not remembered.
   //
   // TlsRptOrganizationName, because nothing reads it except the hourly TLS-RPT
   // pass, which only sends when TlsRptFromAddress is set - so a value it holds for
   // the length of the test changes nothing on the server running it. The stored
   // value is put back however the test ends.
   //---------------------------------------------------------------------------()
   {
      IniFileSettings *ini = IniFileSettings::Instance();

      if (!OverlayLoaded_())
         FailSettingsTest_(_T("The settings store is not loaded in this process, so the tester has nothing to publish from."));

      const String key = _T("TlsRptOrganizationName");
      const String first = _T("hMailServer settings tester A");
      const String second = _T("hMailServer settings tester B - longer, so that half of one and half of the other is neither");

      std::vector<String> names;
      ini->GetSettingsNames(names);
      bool wasStored = false;
      for (size_t i = 0; i < names.size(); i++)
      {
         if (names[i].CompareNoCase(key) == 0)
            wasStored = true;
      }
      const String before = ini->GetSettingsValue(key);

      // Subscribed once per process: a subscription lives as long as the process
      // does, and a tester run twice must not leave two behind.
      static std::shared_ptr<std::atomic<int>> notified;
      if (!notified)
      {
         notified = std::make_shared<std::atomic<int>>(0);
         std::shared_ptr<std::atomic<int>> counter = notified;
         ini->Subscribe(key, [counter]() { (*counter)++; });
      }

      const String reinitializeKey = _T("RestApiBindAddress");
      const String runningBind = ini->GetRestApiBindAddress();
      String storedBind;
      const bool bindInOverlay = GetOverlay_(reinitializeKey, storedBind);
      const bool bindPendingBefore = ini->IsPending(reinitializeKey);

      try
      {
         // A save of a key marked now: in effect when it returns, and its subscriber
         // told once - and not told again by a save that changes nothing.
         if (!ini->WriteSettingsValue(key, first))
            FailSettingsTest_(_T("The tester could not store a value."));

         const int told = notified->load();

         if (ini->GetTlsRptOrganizationName() != first)
            FailSettingsTest_(_T("A save of a key marked now was not in effect when it returned."));

         if (!ini->WriteSettingsValue(key, second) || ini->GetTlsRptOrganizationName() != second)
            FailSettingsTest_(_T("A second save of a key marked now was not in effect when it returned."));

         if (notified->load() != told + 1)
            FailSettingsTest_(Formatter::Format(_T("A save that moved the key told its subscriber {0} time(s), not once."), notified->load() - told));

         if (!ini->WriteSettingsValue(key, second) || notified->load() != told + 1)
            FailSettingsTest_(_T("A save that did not move the key told its subscriber."));

         if (!ini->WriteSettingsValue(key, first))
            FailSettingsTest_(_T("The tester could not store a value."));

         // The swap under load.
         std::atomic<bool> stop(false);
         std::atomic<int> publishes(0);
         std::atomic<bool> writerFailed(false);

         std::thread writer([&]()
         {
            try
            {
               bool flip = false;
               while (!stop.load())
               {
                  SetOverlay_(key, flip ? first : second);
                  Publish_();

                  // The real thing among them: the store read back, which puts the
                  // stored value - the first - in place again.
                  if ((publishes.load() % 64) == 63)
                     ini->Reload();

                  flip = !flip;
                  publishes++;
               }
            }
            catch (...)
            {
               writerFailed = true;
            }
         });

         int reads = 0;
         int torn = 0;
         int sawFirst = 0;
         int sawSecond = 0;
         String example;

         const unsigned __int64 started = GetTickCount64();

         // Ten thousand reads at least, and on until the writer has published a
         // thousand blocks under them, so the reads cannot all fall between two
         // publishes. Bounded, so a writer that stalls fails the test rather than
         // hanging the suite.
         while ((reads < 10000 || publishes.load() < 1000) && GetTickCount64() - started < 60000)
         {
            const String value = ini->GetTlsRptOrganizationName();

            if (value == first)
               sawFirst++;
            else if (value == second)
               sawSecond++;
            else
            {
               torn++;
               example = value;
            }

            reads++;
         }

         stop = true;
         writer.join();

         // Timed, in the application log, because the budget above is a minute and
         // every publish rebuilds the whole block - reading hMailServer.ini once for
         // each key the overlay does not hold - so a slow or scanned disk is where it
         // would run out first.
         LOG_APPLICATION(Formatter::Format(_T("IniFileSettingsTester: {0} reads under {1} publishes took {2} ms."),
            reads, publishes.load(), (int) (GetTickCount64() - started)));

         if (writerFailed.load())
            FailSettingsTest_(_T("The publishing thread failed while the reads went on."));

         if (torn > 0)
            FailSettingsTest_(Formatter::Format(_T("{0} of {1} reads under {2} publishes were neither value - for example '{3}'."), torn, reads, publishes.load(), example));

         if (reads < 10000 || publishes.load() < 1000)
            FailSettingsTest_(Formatter::Format(_T("The load did not run its course in a minute: {0} reads, {1} publishes."), reads, publishes.load()));

         if (sawFirst == 0 || sawSecond == 0)
            FailSettingsTest_(Formatter::Format(_T("The reads saw only one of the two values ({0} and {1}), so they never ran across a swap."), sawFirst, sawSecond));

         // A key marked reinitialize: held at its running value by a reload, pending
         // while the store says otherwise, and not pending once the stored value is
         // put back - recomputed, not remembered.
         const String elsewhere = runningBind == _T("127.0.0.2") ? _T("127.0.0.3") : _T("127.0.0.2");
         SetOverlay_(reinitializeKey, elsewhere);
         Publish_();

         if (ini->GetRestApiBindAddress() != runningBind)
            FailSettingsTest_(_T("A reload applied a key marked reinitialize - RestApiBindAddress took the stored value without a reinitialise."));

         if (!ini->IsPending(reinitializeKey))
            FailSettingsTest_(_T("A key marked reinitialize whose stored value differs from the running one is not pending."));

         bool listed = false;
         std::vector<IniFileSettings::PendingSetting> pending = ini->GetPendingSettings();
         for (size_t i = 0; i < pending.size(); i++)
         {
            if (pending[i].name == reinitializeKey)
               listed = pending[i].stored == elsewhere && pending[i].running == runningBind && pending[i].applies == _T("reinitialize");
         }
         if (!listed)
            FailSettingsTest_(_T("The pending list does not carry the key with its stored value, its running value and reinitialize."));

         RestoreOverlay_(reinitializeKey, bindInOverlay, storedBind);
         Publish_();

         if (!bindPendingBefore && ini->IsPending(reinitializeKey))
            FailSettingsTest_(_T("A key marked reinitialize is still pending after its stored value was put back."));
      }
      catch (...)
      {
         RestoreOverlay_(reinitializeKey, bindInOverlay, storedBind);
         if (wasStored)
            ini->WriteSettingsValue(key, before);
         else
            ini->RemoveSettingsValue(key);
         throw;
      }

      // Put back through the store, which is also what puts the overlay back - and
      // checked with no reload of the tester's own after it, so a RemoveSettingsValue
      // that stopped publishing would leave the tester's value in effect and fail
      // here: the default is in effect because the removal published it.
      if (wasStored)
         ini->WriteSettingsValue(key, before);
      else
         ini->RemoveSettingsValue(key);

      if (ini->GetTlsRptOrganizationName() != (wasStored ? before : String(_T("hMailServer"))))
         FailSettingsTest_(wasStored ?
            String(_T("A save of TlsRptOrganizationName's old value was not in effect when it returned.")) :
            String(_T("A removal of TlsRptOrganizationName did not publish its default: the tester's value was still in effect when it returned.")));

      TestSavesRacingSaves_();
   }

   void
   IniFileSettingsTester::TestSavesRacingSaves_()
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Saves racing saves. Eight threads each alternate a write and a delete of a key
   // of their own, two hundred times, while a ninth reloads over and over; then
   // every key's row in the store and its running value are what its own thread
   // did last - half end on a write, half on a delete.
   //
   // What it guards: before the save path took the reload lock for its store
   // write, and before a save's reload was made read-only, a reload on one thread
   // reconciled the file against the table while another thread's save was half
   // way through its own store write - reading a row before a DELETE dropped it and
   // the file after the line had gone, writing the line back, and then the other
   // save's reload adopting it - so a DELETE that answered success left the setting
   // stored and in effect; and reading a row before a PUT updated it and the file
   // after, putting the old value back into both.
   //
   // Eight keys nothing reads while the internal suite runs: report and signing
   // names, an ACME contact, the autoconfig and MTA-STS hosts. The threads have no
   // audit actor, so the storm writes no rows into the audit trail. What each key
   // held is put back however the test ends.
   //---------------------------------------------------------------------------()
   {
      IniFileSettings *ini = IniFileSettings::Instance();

      const TCHAR *keys[] =
      {
         _T("DmarcRptOrganizationName"),
         _T("AuthenticationResultsIdentity"),
         _T("AcmeContactEmail"),
         _T("AutoconfigClientHost"),
         _T("MtaStsPolicyMx"),
         _T("UpdateSigningIdentity"),
         _T("UpdateSigningIssuer"),
         _T("UpdateSourceRepository")
      };
      const size_t count = sizeof(keys) / sizeof(keys[0]);
      const int operations = 200;

      std::map<String, std::pair<String, String> > rows;
      if (!IniSettingStore::ReadAllRows(rows))
         FailSettingsTest_(_T("The tester could not read the settings store."));

      std::vector<char> wasStored(count, 0);
      std::vector<String> before(count);
      for (size_t k = 0; k < count; k++)
      {
         auto row = rows.find(keys[k]);
         if (row != rows.end())
         {
            wasStored[k] = 1;
            before[k] = (*row).second.first;
         }
      }

      auto putBack = [&]()
      {
         for (size_t k = 0; k < count; k++)
         {
            if (wasStored[k])
               ini->WriteSettingsValue(keys[k], before[k]);
            else
               ini->RemoveSettingsValue(keys[k]);
         }
      };

      try
      {
         // Written by thread k only, so no two threads touch one element.
         std::vector<String> last(count);
         std::vector<char> lastWasWrite(count, 0);

         std::atomic<int> failures(0);
         std::atomic<bool> writersDone(false);
         std::atomic<int> reloads(0);

         const unsigned __int64 started = GetTickCount64();

         std::vector<std::thread> writers;
         for (size_t k = 0; k < count; k++)
         {
            writers.push_back(std::thread([&, k]()
            {
               try
               {
                  for (int i = 0; i < operations; i++)
                  {
                     // Even threads end on a delete, odd ones on a write.
                     const bool write = ((i + (int) k) % 2) == 0;
                     if (write)
                     {
                        const String value = Formatter::Format(_T("settings tester {0}-{1}"), (int) k, i);
                        if (!ini->WriteSettingsValue(keys[k], value))
                           failures++;
                        last[k] = value;
                        lastWasWrite[k] = 1;
                     }
                     else
                     {
                        if (!ini->RemoveSettingsValue(keys[k]))
                           failures++;
                        last[k].Empty();
                        lastWasWrite[k] = 0;
                     }
                  }
               }
               catch (...)
               {
                  failures++;
               }
            }));
         }

         std::thread reloader([&]()
         {
            try
            {
               while (!writersDone.load())
               {
                  ini->Reload();
                  reloads++;
                  std::this_thread::sleep_for(std::chrono::milliseconds(2));
               }
            }
            catch (...)
            {
               failures++;
            }
         });

         for (size_t k = 0; k < writers.size(); k++)
            writers[k].join();
         writersDone = true;
         reloader.join();

         LOG_APPLICATION(Formatter::Format(_T("IniFileSettingsTester: {0} saves on {1} threads, beside {2} reloads, took {3} ms."),
            (int) (count * operations), (int) count, reloads.load(), (int) (GetTickCount64() - started)));

         if (failures.load() > 0)
            FailSettingsTest_(Formatter::Format(_T("{0} of the racing saves failed or threw."), failures.load()));

         std::map<String, std::pair<String, String> > after;
         if (!IniSettingStore::ReadAllRows(after))
            FailSettingsTest_(_T("The tester could not read the settings store back."));

         std::shared_ptr<const IniFileSettings::Values> running = ini->Current();

         for (size_t k = 0; k < count; k++)
         {
            const String key = keys[k];

            auto row = after.find(key);
            auto resolved = running->resolved.find(key);
            const String runningValue = resolved == running->resolved.end() ? String() : (*resolved).second;

            if (lastWasWrite[k])
            {
               if (row == after.end())
                  FailSettingsTest_(Formatter::Format(_T("{0}: the last save was a write of '{1}', and the store holds no row for it."), key, last[k]));
               if ((*row).second.first != last[k])
                  FailSettingsTest_(Formatter::Format(_T("{0}: the last save was a write of '{1}', and the store holds '{2}' - another save put an older value back."), key, last[k], (*row).second.first));
               if (runningValue != last[k])
                  FailSettingsTest_(Formatter::Format(_T("{0}: the last save was a write of '{1}', and the server is running on '{2}'."), key, last[k], runningValue));
            }
            else
            {
               if (row != after.end())
                  FailSettingsTest_(Formatter::Format(_T("{0}: the last save was a delete, and the store holds '{1}' - another save brought the setting back."), key, (*row).second.first));

               String defaultValue;
               bool isInteger = false;
               IniSettingStore::DefaultFor(key, defaultValue, isInteger);
               if (runningValue != defaultValue)
                  FailSettingsTest_(Formatter::Format(_T("{0}: the last save was a delete, and the server is running on '{1}' rather than its default '{2}'."), key, runningValue, defaultValue));
            }
         }
      }
      catch (...)
      {
         putBack();
         throw;
      }

      putBack();
   }
}
