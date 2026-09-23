// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include "../SQL/DatabaseSettings.h"

namespace HM
{
   class IniFileSettings : public Singleton<IniFileSettings>
   {
   public:

      /// <summary>
      /// Every value the [Settings] section is read into, as ONE IMMUTABLE BLOCK.
      ///
      /// Until 22 September 2026 these were ~240 members of this class, latched by
      /// LoadSettings() at start-up and read by inline getters from every thread,
      /// which is why a saved setting applied at the next start: re-running the load
      /// while sessions read the members would have rewritten a String underneath a
      /// reader. Now LoadSettings() builds a NEW block and publishes it with one
      /// atomic store, and every getter reads through Current() - one lock-free load
      /// of a generation counter, and the shared pointer itself only when a publish
      /// has happened since this thread last looked. A reader holds the block it
      /// loaded for as long as its expression lasts, so it sees one block or the
      /// other and never half of each.
      ///
      /// WHY EVERY GETTER RETURNS A COPY. A getter that answered a const String &
      /// into the block would hand out a reference whose block may be replaced - and
      /// freed, once the last reader lets go - the moment after it returns: a
      /// use-after-free waiting for the first save. Every String getter here returns
      /// by value, and one added later must too.
      ///
      /// The bootstrap - [Database], [Directories], [Security], [GUILanguages] - is
      /// deliberately NOT in the block. LoadSettings() reads it into plain members:
      /// at a start, at a reinitialise, and when COM's Settings.Directories or
      /// Database.CreateInternalDatabase asks for a load; a save's reload never
      /// touches it. Those members are rewritten in place under the reload lock,
      /// which their readers do not take - a race older than the block, and not one
      /// it closes.
      /// </summary>
      struct Values
      {
         bool add_xauth_user_header_ = false;
         String daemonaddress_domain_;
         bool add_xoriginal_rcpt_to_header_ = false;
         int max_no_of_external_fetch_threads_ = 15;
         bool greylisting_enabled_during_record_expiration_ = true;
         int greylisting_expiration_interval_ = 240;
         int preferred_hash_algorithm_ = 3;
         int minimum_accepted_hash_algorithm_ = 0;
         String password_pepper_;
         int password_hash_iterations_ = 0;
         int password_hash_memory_kib_ = 0;
         int password_hash_time_cost_ = 0;
         bool oauth2_enabled_ = false;
         bool oauth2_require_tls_ = false;
         String outbound_oauth2_token_url_;
         String outbound_oauth2_client_id_;
         String outbound_oauth2_client_secret_;
         String outbound_oauth2_scope_;
         String outbound_oauth2_hosts_;
         String outbound_oauth2_fixed_token_;
         String fetch_oauth2_hosts_;
         String oauth2_allowed_algorithms_;
         String oauth2_hmac_secret_;
         String oauth2_rsa_public_key_file_;
         String oauth2_issuer_;
         String oauth2_audience_;
         String oauth2_username_claim_;
         String oauth2_jwks_url_;
         int oauth2_jwks_cache_seconds_ = 3600;
         String oauth2_introspection_url_;
         String oauth2_introspection_client_id_;
         String oauth2_introspection_client_secret_;
         int oauth2_introspection_cache_seconds_ = 300;
         bool oauth2_introspection_fail_open_ = false;
         bool protect_stored_secrets_with_dpapi_ = false;
         String service_account_name_;
         String service_account_password_;
         bool dnsbl_checks_after_mail_from_ = false;
         bool sep_svc_logs_ = false;
         int log_level_ = 0;
         int max_log_line_len_ = 500;
         int quick_retries_ = 0;
         int quick_retries_Minutes = 0;
         int queue_randomness_minutes_ = 0;
         int mxtries_factor_ = 0;
         String archive_dir_;
         bool archive_hardlinks_ = false;
         bool delivery_hardlinks_ = false;
         int pop3dmin_timeout_ = 0;
         int pop3dmax_timeout_ = 0;
         int pop3cmin_timeout_ = 0;
         int pop3cmax_timeout_ = 0;
         int smtpdmin_timeout_ = 0;
         int smtpdmax_timeout_ = 0;
         int smtpcmin_timeout_ = 0;
         int smtpcmax_timeout_ = 0;
         int samin_timeout_ = 0;
         int samax_timeout_ = 0;
         String spamassassin_user_;
         bool spamassassin_user_from_recipient_ = false;
         bool spamassassin_learn_on_move_ = false;
         int finalization_timeout_ = 0;
         int clam_min_timeout_ = 0;
         int clam_max_timeout_ = 0;
         int dns_query_timeout_ = 0;
         int client_session_ceiling_ = 0;
         int db_connection_acquire_timeout_ = 0;
         int script_timeout_ = 0;
         String script_allowed_objects_;
         int external_process_timeout_ = 0;
         int async_queue_stall_threshold_ = 0;
         int async_queue_reserved_threads_ = 0;
         bool samove_vs_copy_ = false;
         String auth_user_replacement_ip_;
         int indexer_full_minutes_ = 0;
         int indexer_full_limit_ = 0;
         int indexer_quick_limit_ = 0;
         bool indexer_full_text_enabled_ = false;
         int indexer_full_text_batch_size_ = 0;
         int indexer_full_text_min_token_length_ = 0;
         int indexer_full_text_max_tokens_ = 0;
         int load_header_read_size_ = 0;
         int load_body_read_size_ = 0;
         int blocked_iphold_seconds_ = 0;
         int smtpdmax_size_drop_ = 0;
         bool backup_messages_dbonly_ = false;
         bool backup_verify_restore_ = true;
         bool add_xauth_user_ip_ = false;
         bool rewrite_envelope_from_when_forwarding_ = false;
         bool srs_enabled_ = false;
         String srs_secret_;
         bool batv_enabled_ = false;
         bool outbound_pipelining_ = true;
         bool outbound_chunking_ = true;
         bool smtp_authenticated_sender_check_ = false;
         int metrics_history_days_ = 7;
         String batv_secret_;
         int max_submissions_per_ip_per_minute_ = 0;
         int pop3_login_delay_seconds_ = 0;
         bool smtp_proxy_protocol_enabled_ = false;
         String smtp_proxy_protocol_trusted_ips_;
         bool smtp_xclient_enabled_ = false;
         String smtp_xclient_trusted_ips_;
         bool quarantine_enabled_ = false;
         int quarantine_retention_days_ = 0;
         bool message_trace_enabled_ = false;
         int message_trace_retention_days_ = 0;
         bool windows_event_log_enabled_ = true;
         int windows_event_log_level_ = 2;
         bool dmarc_tree_walk_enabled_ = false;
         int spf_void_lookup_limit_ = 0;
         bool reject_full_mailbox_at_rcpt_ = false;
         bool dkim_accept_sha1_ = false;
         int quota_warning_percent_ = 0;
         int archive_retention_days_ = 0;
         std::set<String> archive_domains_;
         int database_statement_timeout_ = 0;
         int imap_expunge_retention_records_ = 0;
         bool metrics_per_domain_enabled_ = false;
         String filter_hook_url_;
         int filter_hook_timeout_seconds_ = 0;
         bool filter_hook_fail_closed_ = false;
         int filter_hook_reject_score_ = 0;
         int filter_hook_max_message_size_kb_ = 0;
         int password_policy_minimum_length_ = 0;
         bool password_policy_require_mixed_case_ = false;
         bool password_policy_require_digit_ = false;
         bool password_policy_require_non_alphanumeric_ = false;
         bool password_policy_reject_common_ = false;
         int password_policy_history_count_ = 0;
         int password_policy_maximum_age_days_ = 0;
         int max_outbound_per_destination_per_minute_ = 0;
         bool use_dns_cache_ = true;
         String dns_server_;
         bool mta_sts_enabled_ = true;
         bool auto_ban_firewall_enabled_ = false;
         String auto_ban_command_;
         String auto_ban_never_ban_;
         bool dane_enabled_ = true;
         bool dnssec_validation_enabled_ = true;
         String dnssec_trust_anchors_;
         bool update_check_enabled_ = false;
         String update_channel_;
         String update_feed_url_;
         String http_proxy_;
         String caldav_redirect_url_;
         String carddav_redirect_url_;
         int update_check_hours_ = 24;
         bool imap_compression_enabled_ = true;
         String update_trust_roots_file_;
         String update_log_public_key_file_;
         String update_signing_identity_;
         String update_signing_issuer_;
         String update_source_repository_;
         bool update_require_authenticode_ = false;
         int update_service_wait_seconds_ = 180;
         String update_window_;
         bool update_auto_download_ = false;
         bool update_backup_before_apply_ = true;
         bool json_logging_ = false;
         int log_delete_days_ = 0;
         int shutdown_drain_seconds_ = 0;
         int slow_query_log_ms_ = 0;
         bool message_store_fsync_ = false;
         int minimum_free_disk_space_mb_ = 100;
         int disk_space_warning_threshold_mb_ = 1024;
         int simulate_spool_write_failure_ = 0;
         String simulate_database_failure_for_;
         bool message_store_consistency_check_ = false;
         int metrics_server_port_ = 0;
         String metrics_server_bind_address_;
         String metrics_server_auth_token_;
         String metrics_server_auth_username_;
         String metrics_server_auth_password_;
         String metrics_server_certificate_file_;
         String metrics_server_private_key_file_;
         String scheduled_backup_time_;
         int scheduled_backup_interval_minutes_ = 0;
         int scheduled_backup_keep_count_ = 0;
         int scheduled_backup_max_age_days_ = 0;
         String otel_endpoint_;
         String otel_service_name_;
         String otel_metrics_endpoint_;
         String otel_logs_endpoint_;
         int otel_metrics_interval_ = 60;
         int manage_sieve_server_port_ = 0;
         String manage_sieve_server_bind_address_;
         bool arc_sealing_enabled_ = false;
         int dkim_signature_validity_seconds_ = 0;
         bool dkim_enforce_signature_expiry_ = true;
         int dkim_expiry_clock_skew_seconds_ = 300;
         String dkim_oversign_headers_;
         bool authentication_results_enabled_ = false;
         bool received_spf_header_enabled_ = false;
         String authentication_results_identity_;
         String tls_rpt_from_address_;
         String tls_rpt_organization_name_;
         String dmarc_rpt_from_address_;
         String dmarc_rpt_organization_name_;
         int dmarc_rpt_schema_version_ = 1;
         int av_fail_action_ = 0;
         int av_fail_retry_minutes_ = 15;
         int av_fail_max_holds_ = 16;
         int account_lockout_threshold_ = 0;
         int account_lockout_window_minutes_ = 30;
         int account_lockout_minutes_ = 30;
         int logon_tarpit_seconds_ = 0;
         int smtp_tarpit_count_ = 0;
         int smtp_tarpit_delay_seconds_ = 0;
         String tls_key_exchange_groups_;
         String tls_cipher_suites13_;
         bool tls_session_tickets_enabled_ = true;
         int tls_session_cache_size_ = 0;
         int tls_session_timeout_seconds_ = 0;
         int tls_ticket_key_rotation_seconds_ = 0;
         int rest_api_port_ = 0;
         String rest_api_bind_address_;
         String rest_api_certificate_file_;
         String rest_api_private_key_file_;
         bool acme_enabled_ = false;
         String acme_directory_url_;
         String acme_contact_email_;
         String acme_domains_;
         String acme_certificate_directory_;
         int acme_http_port_ = 80;
         bool acme_reuse_key_ = true;
         int web_services_http_port_ = 0;
         int web_services_https_port_ = 0;
         String web_services_bind_address_;
         String web_services_certificate_file_;
         String web_services_private_key_file_;
         String web_services_trusted_proxies_;
         bool mta_sts_hosting_enabled_ = true;
         String mta_sts_policy_mode_;
         int mta_sts_policy_max_age_ = 604800;
         String mta_sts_policy_mx_;
         bool autoconfig_enabled_ = true;
         String autoconfig_client_host_;
         int imap_search_timeout_ = 60;
         int imap_search_max_megabytes_ = 2048;
         String m_sDisableAUTHList;

         /// <summary>
         /// What each [Settings] key resolved to when this block was built - the text
         /// for a string read, the number as text for an integer read - by the name
         /// the loader reads it with. The pending list compares the store with these,
         /// and a reload copies a key's entry from the running block when the key's
         /// mark says a save must not apply it.
         /// </summary>
         std::map<String, String> resolved;
      };

      IniFileSettings();
      virtual ~IniFileSettings();

      /// <summary>
      /// The block the server is running on. Never null. Through a copy each thread
      /// keeps of the last block it loaded, checked against a generation counter
      /// that every publish advances - see the definition for why.
      /// </summary>
      std::shared_ptr<const Values> Current() const;

      /// <summary>
      /// Which [Settings] keys a load takes from the store, by their mark in
      /// build/ini-settings-baseline.txt (IniSettingsBaseline.h). Until the process
      /// has started its servers once (LatchRunningValues) every load reads every
      /// key, whatever the scope says.
      ///
      ///   ScopeNow           the keys marked now; every reinitialize and restart
      ///                      key keeps the value it is running on. What a save,
      ///                      and every LoadSettings() outside the start sequence,
      ///                      does - a COM call that re-reads the bootstrap must not
      ///                      apply a listener's port under a live socket.
      ///   ScopeReinitialize  the keys marked now and reinitialize: what
      ///                      Application::InitInstance does when it rebuilds the
      ///                      servers in place. The restart keys keep the values
      ///                      the process started with.
      /// </summary>
      enum LoadScope
      {
         ScopeNow,
         ScopeReinitialize
      };

      /// <summary>
      /// Reads the bootstrap sections from the file, and builds and publishes a new
      /// [Settings] block in the given scope.
      /// </summary>
      void LoadSettings(LoadScope scope = ScopeNow);

      /// <summary>
      /// What a save does, and the whole of this row: the overlay is re-read from
      /// hm_inisettings - the rows alone, READ-ONLY (IniSettingStore::ReadStoredValues);
      /// the reconcile that writes the file and the table is a start's and a
      /// reinitialise's - the [SettingsOverride] values the last start or reinitialise
      /// loaded are applied over it again, and a new block is built in ScopeNow and
      /// published. Then, with the reload lock released, every subscriber of a key
      /// whose value moved is called. The bootstrap is not re-read, and nor is
      /// [SettingsOverride]: a change to that section waits for a reinitialise
      /// (Apply now) or a start, which re-read it and announce it (HM5807), and an
      /// override of a key marked restart waits for a start.
      ///
      /// WriteSettingsValue and RemoveSettingsValue - every door to the store: COM
      /// SetIniSetting and DeleteIniSetting, the REST ini route, the settings-group
      /// fields kept in this store, the branding and file-link routes, hmailserver
      /// --set-setting, the audit trail's revert - do the same inside the one hold of
      /// the reload lock that covers their store write. This is for a caller that
      /// stored nothing itself.
      /// </summary>
      void Reload();

      /// <summary>
      /// Calls subscriber after every reload that changed the named key's value, on
      /// the thread that saved, after the new block is published and the reload lock
      /// released, so the subscriber reads the new value through the getters and no
      /// other save waits for it. One subscription may name several keys and is
      /// called once however many of them moved. For a subsystem that copied a
      /// getter into a member of its own when it started, and can take a new value in
      /// place: it re-latches here. Subscribers are called one at a time, under a
      /// lock of their own, and each must READ THE VALUE IT TAKES WHEN IT RUNS rather
      /// than be handed one - that is what makes the last call take the last save
      /// when two saves of one key are told out of order. A subscriber must not throw
      /// (a throw is contained and logged). A key whose latch cannot be redone safely
      /// is marked reinitialize in the baseline instead of subscribing. Subscriptions
      /// last for the process: the servers a reinitialise rebuilds find theirs still
      /// in place.
      /// </summary>
      void Subscribe(const String &name, std::function<void()> subscriber);
      void Subscribe(const std::vector<String> &names, std::function<void()> subscriber);

      /// <summary>
      /// Marks the values the servers have just been started with as the running
      /// ones. From here a load keeps every restart key at the value this process
      /// started with, and a load in ScopeNow keeps every reinitialize key at the
      /// value the servers were last built with. Called by Application::InitInstance
      /// once the store has been read; idempotent.
      /// </summary>
      void LatchRunningValues();

      /// <summary>
      /// When a change to a [Settings] key reaches the running server: "now",
      /// "reinitialize" or "restart", from the key's mark in the baseline, and its
      /// spelling there. False for a name this build does not read through the
      /// loader - a key a script of the administrator's own reads, UseLanguage, the
      /// webmail's branding keys, a misspelling - which has no mark: COM's
      /// IniSettingApplies refuses it with NoApplyMarkSentence, and the REST answers
      /// carry applies null with the same sentence, rather than either answering
      /// "now" and making a misspelled key look like a working setting.
      /// </summary>
      static bool GetApplies(const String &name, String &applies, String &spelling);

      /// <summary>What COM and REST both say for a name GetApplies does not know.</summary>
      static const char *NoApplyMarkSentence();

      /// <summary>
      /// Every key the loader reads, in the baseline's spelling and order, with its
      /// mark: what COM's IniSettingAppliesAll answers, so that a client labels a
      /// whole page with one call.
      /// </summary>
      static void GetAllApplies(std::vector<std::pair<String, String> > &marks);

      /// <summary>
      /// A [Settings] key whose stored value is not the value the server is running
      /// on, and will not be until what its mark names has happened.
      /// </summary>
      struct PendingSetting
      {
         String name;
         String stored;
         String running;
         String applies;
      };

      /// <summary>
      /// Every key marked reinitialize or restart whose stored value - the value a
      /// load would resolve now, overlay, override and file as ever - differs from
      /// the running block's. RECOMPUTED on every call rather than remembered, so a
      /// value put back before anything applied it is not pending, and whatever
      /// applies a key clears it by making the two agree.
      /// </summary>
      std::vector<PendingSetting> GetPendingSettings();

      /// <summary>Whether one key is in GetPendingSettings, by the name as stored.</summary>
      bool IsPending(const String &name);

      /// <summary>
      /// Loads the [Settings] values from hm_inisettings - THE SETTINGS STORE from
      /// schema 6042 - and makes them the source for the next LoadSettings(). Call
      /// once, after the database is open AND its schema version has been accepted:
      /// the table only exists at schema 6011, and querying it on an older database
      /// would replace the clear "run DBUpdater" message with a SQL error.
      ///
      /// Three things happen here, in this order, and the order is the design:
      ///
      ///   1. IniSettingStore::Synchronize reconciles the file against the table.
      ///      The table wins; a key edited in the file is named in the error log and
      ///      not applied; a key the table has never seen is adopted. The values are
      ///      written back INTO the file as well, so every reader that goes to it
      ///      directly keeps seeing the right one - notably hMailServer.exe
      ///      /Register, which reads ServiceAccountName and ServiceAccountPassword
      ///      with no database open at all, because registering the service is what
      ///      happens before there is one.
      ///   2. [SettingsOverride] from the file is applied over the result and every
      ///      key in it is named in the error log. This is the door for a database
      ///      that is unreachable or holds a value that will not let the server
      ///      start, and it deliberately shouts at every start so that nobody leaves
      ///      one behind.
      ///   3. The overlay is marked loaded, which is what makes ReadIniSettingString_
      ///      and ReadIniSettingInteger_ consult it. It is marked loaded even when
      ///      the table could not be read, PROVIDED there are overrides - because the
      ///      unreachable database is exactly the case the door exists for.
      /// </summary>
      void LoadDatabaseSettings();

      /// <summary>
      /// Forgets the reconciled values, so the next LoadSettings() reads the file
      /// alone. Used when the process is torn down and rebuilt in place
      /// (Application::Reinitialize), where the previous cycle's map would otherwise
      /// outlive the database connection it came from.
      /// </summary>
      void ForgetDatabaseSettings();

      // ---- the administrative surface behind the COM API --------------------
      //
      // These are what let a Control Panel on another machine administer the
      // [Settings] section at all. Before them, IniFeatureStore had to open the file
      // itself, which only works when it is running on the server - which is why the
      // log-folder card has to say "not readable from this machine".
      //
      // A write PUBLISHES (22 September 2026, docs/SettingsApply.md). The store takes
      // the value, then Reload() builds a new block and swaps it in, so a key marked
      // now in the baseline - most of them - is in effect when the call returns. A key
      // marked reinitialize keeps its running value until POST
      // /api/v1/server/reinitialize, and one marked restart until the service starts
      // again; until then the key is PENDING, stored and not yet running, and
      // GetPendingSettings says so. Before this the contract was persist now, apply at
      // the next start, for every key - which is what the Control Panel's "applies
      // after a service restart" wording described, and what it no longer says.

      /// <summary>
      /// Reads one [Settings] value as it stands in the store right now - the stored
      /// value when the overlay has been loaded, the override when one is in force,
      /// and the file otherwise. For a key marked now that is also the value the
      /// server is running on. For a PENDING key - one marked reinitialize or restart
      /// and saved since what applies it last happened - it is the stored value and
      /// NOT the one in use: GetPendingSettings carries both, and the running block
      /// (Current) holds the one in use.
      /// </summary>
      String GetSettingsValue(const String &key);

      /// <summary>
      /// Stores one [Settings] value, brings the file's copy into line, and publishes
      /// it (Reload). False means the DATABASE could not be written and nothing was
      /// changed; see IniSettingStore::WriteSetting for why that ordering is the
      /// opposite of what it was.
      /// </summary>
      bool WriteSettingsValue(const String &key, const String &value);

      /// <summary>
      /// Drops one setting's row and removes its key from the file, returning the
      /// setting to its default. Deliberately distinct from writing an empty string:
      /// an absent key falls back to the caller's default, while "Key=" reads as 0
      /// through GetPrivateProfileInt. This is now the ONLY way to return a setting
      /// to its default - deleting the line by hand no longer does it, because the
      /// row would be written straight back at the next start. Publishes, as
      /// WriteSettingsValue does.
      /// </summary>
      bool RemoveSettingsValue(const String &key);

      /// <summary>Every setting name the server holds: the table and the file, united.</summary>
      void GetSettingsNames(std::vector<String> &names);

      /// <summary>
      /// The [Settings] keys the file's [SettingsOverride] section is forcing, as the
      /// overlay loaded them at the last start - not as the file reads now, because a
      /// section edited since is not what this server is running on.
      ///
      /// It exists because an override is the one value the two stores do not know
      /// about: GetSettingsNames unites the table and the [Settings] section, and a
      /// key forced from [SettingsOverride] that has neither a row nor a line is in
      /// neither. Anything reporting what the server is RUNNING on has to add these,
      /// or it silently leaves out the one mechanism whose whole purpose is to run a
      /// server on something other than what is stored.
      /// </summary>
      void GetOverriddenNames(std::set<String> &names);

      bool CheckSettings(String &sErrorMessage);

      static String GetInitializationFile();

      /// <summary>
      /// Names the configuration file explicitly, before anything has read it.
      /// The POSIX service host passes --config through to this; on Windows
      /// nothing calls it, because the file's location is derived from the
      /// registry and the executable's own directory and there is no command
      /// line to say otherwise.
      /// </summary>
      static void SetInitializationFile(const String &file);

      bool GetDatabaseSettingsExists();

      String GetDatabaseProvider() const { return database_provider_; }
      String GetDatabaseServer() const { return database_server_; }
      String GetDatabaseName() const { return database_name_; }
      String GetUsername() const { return username_; }
      String GetPassword() const { return password_; }
      DatabaseSettings::SQLDBType GetDatabaseType() const { return sqldbtype_; }
      long GetDatabasePort() const { return dbport_;}
      bool GetIsInternalDatabase() const {return is_internal_database_; }

      void SetDatabaseServer(const String &sNewValue);
      void SetDatabaseName(const String &sNewValue);
      void SetUsername(const String &sNewValue);
      void SetPassword(const String &sNewValue);
      void SetDatabaseType(DatabaseSettings::SQLDBType type);
      void SetDatabasePort(long lNewValue);
      void SetIsInternalDatabase(bool newValue);

      void SetAdministratorPassword(const String &sNewPassword);
      String GetAdministratorPassword();

      // The administrator credential's second factor: a base32 TOTP secret kept
      // under [Security] AdministratorTotpSecret in the machine-bound DPAPI
      // envelope route passwords use (Crypt::ProtectSecret), and handed out only
      // in the otpauth URI at enrolment. Empty means no second factor. Setting an
      // empty string removes it - which is also the recovery for a lost
      // authenticator: delete the key from the file, as a local administrator can.
      String GetAdministratorTotpSecret();
      // Whether a second factor is enrolled at all, judged from the stored form
      // and not from whether it can be opened: an enrolled secret this machine
      // cannot unprotect must refuse the logon, never wave it through.
      bool IsAdministratorTotpEnrolled() const { return !administrator_totp_secret_protected_.IsEmpty(); }
      bool SetAdministratorTotpSecret(const String &base32Secret);
      String GetLogDirectory();

      String GetBinDirectory();
      String GetProgramDirectory() const { return app_directory_; }
      String GetDataDirectory() const { return data_directory_; }
      String GetTempDirectory() const { return temp_directory_; }
      String GetEventDirectory() const { return event_directory_; }
      String GetDBScriptDirectory() const { return dbscript_directory_; }
      String GetDatabaseDirectory() const { return database_directory_; }
      String GetLanguageDirectory() const;
      String GetDatabaseServerFailoverPartner() const { return database_server_FailoverPartner; }

      void SetProgramDirectory(const String &sNewVal);
      void SetDataDirectory(const String &sNewVal);
      void SetTempDirectory(const String &sNewVal);
      void SetEventDirectory(const String &sNewVal);
      void SetDatabaseDirectory(const String &sNewVal);
      void SetLogDirectory(const String &sLogDirectory);

      
      std::vector<String> GetValidLanguages() const {return valid_languages_; }

      String GetUserInterfaceLanguage();
      void SetUserInterfaceLanguage(String sLanguage);

      int GetNumberOfDatabaseConnections() const;
      int GetNumberOfDatabaseConnectionAttempts() const;
      int GetDBConnectionAttemptsDelay() const;

      // [Database] AllowUnencryptedConnection=1. The bundled MariaDB Connector/C
      // requires TLS from a MySQL/MariaDB server and refuses one that has none; this
      // asks it to prefer TLS and fall back to plaintext instead. Off by default, so
      // that an encrypted database connection stays the default and a plaintext-only
      // server is a choice written in the ini rather than a silent downgrade. See
      // MySQLConnection::Connect for the client option it sets.
      bool GetDatabaseAllowUnencryptedConnection() const { return database_allow_unencrypted_connection_; }

      // [Database] UpgradeOnStart: whether the service brings an older schema
      // forward itself at its own start (Application::OnDatabaseConnected,
      // through SchemaUpgrader) rather than refusing to start with HM5011. A
      // file key, and the one this row adds, because it has to be read before
      // the database is at a version this build can open - which is before the
      // settings store in it exists. Off by default; the doc says when to set it.
      bool GetDatabaseUpgradeOnStart() const { return database_upgrade_on_start_; }

      // [Database] PostgreSQLSslMode and PostgreSQLSslRootCert: libpq's sslmode and
      // sslrootcert connection parameters, from this file rather than from PGSSLMODE in
      // the service's environment. Empty leaves libpq's own default, prefer - encrypted
      // when the server offers it, verified never. verify-full with a root certificate is
      // what makes the connection one the administrator can trust rather than hope for.
      // See PGConnection::Connect, which refuses a mode it does not know.
      String GetDatabasePostgreSQLSslMode() const { return database_postgresql_ssl_mode_; }
      String GetDatabasePostgreSQLSslRootCert() const { return database_postgresql_ssl_root_cert_; }

      // [Database] ConnectionStringOptions: appended verbatim to the OLE DB connection
      // string of an MS SQL Server backend. Encryption and certificate verification are
      // the provider's own keywords there (Encrypt=yes;TrustServerCertificate=no for
      // MSOLEDBSQL), and any list of them written here would only ever lag the provider.
      String GetDatabaseConnectionStringOptions() const { return database_connection_string_options_; }
      
      bool GetAddXAuthUserHeader() {return Current()->add_xauth_user_header_; }
      String GetDaemonAddressDomain() const { return Current()->daemonaddress_domain_; }
      bool GetAddXOriginalRcptToHeader() { return Current()->add_xoriginal_rcpt_to_header_; }    
      int GetMaxNumberOfExternalFetchThreads() {return Current()->max_no_of_external_fetch_threads_ ;}
      bool GetGreylistingEnabledDuringRecordExpiration() {return Current()->greylisting_enabled_during_record_expiration_;}
      int GetGreylistingExpirationInterval() {return Current()->greylisting_expiration_interval_; }
      int GetPreferredHashAlgorithm() {return Current()->preferred_hash_algorithm_;}
      int GetMinimumAcceptedHashAlgorithm() {return Current()->minimum_accepted_hash_algorithm_;}
      String GetPasswordPepper() {return Current()->password_pepper_;}

      // Work factors for NEW password hashes, from [Settings]: PasswordHashIterations
      // (PBKDF2), PasswordHashMemoryKB and PasswordHashTimeCost (Argon2id). 0 means
      // HashCreator's built-in default. A stored hash carries its own parameters, so
      // raising one of these never breaks a logon: a hash cheaper than the configured
      // value is re-derived on the next successful logon (PasswordValidator), and a
      // costlier one is left alone - lowering a value affects new hashes only, the
      // same never-downgrade rule the scheme itself follows. Upstream #554, without
      // its schema rows and its rewrite of every stored hash into PHC format.
      int GetPasswordHashIterations() const { return Current()->password_hash_iterations_; }
      int GetPasswordHashMemoryKiB() const { return Current()->password_hash_memory_kib_; }
      int GetPasswordHashTimeCost() const { return Current()->password_hash_time_cost_; }
      bool GetOAuth2Enabled() {return Current()->oauth2_enabled_;}
      bool GetOAuth2RequireTLS() {return Current()->oauth2_require_tls_;}
      String GetOAuth2AllowedAlgorithms() {return Current()->oauth2_allowed_algorithms_;}
      String GetOAuth2HmacSecret() {return Current()->oauth2_hmac_secret_;}
      String GetOAuth2RsaPublicKeyFile() {return Current()->oauth2_rsa_public_key_file_;}
      String GetOAuth2Issuer() {return Current()->oauth2_issuer_;}

      // Outbound XOAUTH2 (relaying through a provider that has switched Basic
      // auth off, Microsoft 365 first among them). TokenUrl/ClientId/Secret/
      // Scope drive the client_credentials fetch; Hosts is the comma-separated
      // list of relay hosts the token is presented to; FixedToken, when set, is
      // used verbatim instead of fetching - for tokens obtained outside this
      // server, and for tests.
      String GetOutboundOAuth2TokenUrl() {return Current()->outbound_oauth2_token_url_;}
      String GetOutboundOAuth2ClientId() {return Current()->outbound_oauth2_client_id_;}
      String GetOutboundOAuth2ClientSecret() {return Current()->outbound_oauth2_client_secret_;}
      String GetOutboundOAuth2Scope() {return Current()->outbound_oauth2_scope_;}
      String GetOutboundOAuth2Hosts() {return Current()->outbound_oauth2_hosts_;}
      String GetOutboundOAuth2FixedToken() {return Current()->outbound_oauth2_fixed_token_;}

      // The POP3-fetch counterpart of OutboundOAuth2Hosts: external accounts
      // whose server is listed here authenticate with XOAUTH2 using the same
      // token client and credentials as the outbound relay.
      String GetFetchOAuth2Hosts() {return Current()->fetch_oauth2_hosts_;}
      String GetOAuth2Audience() {return Current()->oauth2_audience_;}
      String GetOAuth2UsernameClaim() {return Current()->oauth2_username_claim_;}
      // [Settings] OAuth2JwksUrl / OAuth2JwksCacheSeconds: the provider's JWK Set as the
      // signing-key source (JwksKeySet). OAuth2Introspection*: RFC 7662 revocation
      // checks after local validation (TokenIntrospection).
      String GetOAuth2JwksUrl() const {return Current()->oauth2_jwks_url_;}
      int GetOAuth2JwksCacheSeconds() const {return Current()->oauth2_jwks_cache_seconds_;}
      String GetOAuth2IntrospectionUrl() const {return Current()->oauth2_introspection_url_;}
      String GetOAuth2IntrospectionClientId() const {return Current()->oauth2_introspection_client_id_;}
      String GetOAuth2IntrospectionClientSecret() const {return Current()->oauth2_introspection_client_secret_;}
      int GetOAuth2IntrospectionCacheSeconds() const {return Current()->oauth2_introspection_cache_seconds_;}
      bool GetOAuth2IntrospectionFailOpen() const {return Current()->oauth2_introspection_fail_open_;}
      bool GetProtectStoredSecretsWithDPAPI() {return Current()->protect_stored_secrets_with_dpapi_;}
      String GetServiceAccountName() {return Current()->service_account_name_;}
      String GetServiceAccountPassword() {return Current()->service_account_password_;}
      bool GetDNSBLChecksAfterMailFrom() {return Current()->dnsbl_checks_after_mail_from_; }
      bool GetSepSvcLogs() {return Current()->sep_svc_logs_; }
      int GetLogLevel() {return Current()->log_level_; }
      int GetMaxLogLineLen() {return Current()->max_log_line_len_; }
      int GetQuickRetries() {return Current()->quick_retries_; }
      int GetQuickRetriesMinutes() {return Current()->quick_retries_Minutes; }
      int GetQueueRandomnessMinutes () {return Current()->queue_randomness_minutes_; }
      int GetMXTriesFactor () {return Current()->mxtries_factor_; }
      String GetArchiveDir() const { return Current()->archive_dir_; }
      bool GetArchiveHardlinks() const { return Current()->archive_hardlinks_; }

      // DeliveryHardLinks: a message to several local recipients is one file with a
      // name in each recipient's folder rather than a copy per recipient. Safe
      // because every rewrite of a message file is a temporary file renamed into
      // place, which replaces one name and leaves the others with the old content.
      // Off by default, like ArchiveHardLinks: a hard link is a promise that nothing
      // outside the server edits a message file in place, and only the operator
      // can make it.
      bool GetDeliveryHardLinks() const { return Current()->delivery_hardlinks_; }
      int GetPOP3DMinTimeout () {return Current()->pop3dmin_timeout_; }
      int GetPOP3DMaxTimeout () {return Current()->pop3dmax_timeout_; }
      int GetPOP3CMinTimeout () {return Current()->pop3cmin_timeout_; }
      int GetPOP3CMaxTimeout () {return Current()->pop3cmax_timeout_; }
      int GetSMTPDMinTimeout () {return Current()->smtpdmin_timeout_; }
      int GetSMTPDMaxTimeout () {return Current()->smtpdmax_timeout_; }
      int GetSMTPCMinTimeout () {return Current()->smtpcmin_timeout_; }
      int GetSMTPCMaxTimeout () {return Current()->smtpcmax_timeout_; }
      int GetSAMinTimeout () {return Current()->samin_timeout_; }
      int GetSAMaxTimeout () {return Current()->samax_timeout_; }

      // [Settings] SpamAssassinUser: the User: header of every spamd PROCESS request,
      // which is the user whose preferences spamd applies to the scan. Empty, the
      // default, sends no header, so an existing spamd sees exactly what it always did.
      // [Settings] SpamAssassinUserFromRecipient=1 names the recipient instead when the
      // message has exactly one; see SpamTestSpamAssassin::RunTest for why one is the limit.
      String GetSpamAssassinUser() const { return Current()->spamassassin_user_; }
      bool GetSpamAssassinUserFromRecipient() const { return Current()->spamassassin_user_from_recipient_; }

      // [Settings] SpamAssassinLearnOnMove=1: a message moved into the user's Junk
      // folder is told to spamd as spam and one moved out of it as ham (spamc TELL,
      // Set: local). Off by default - TELL is a command spamd refuses unless started
      // with --allow-tell. See SpamAssassinLearner.
      bool GetSpamAssassinLearnOnMove() const { return Current()->spamassassin_learn_on_move_; }
      int GetFinalizationTimeout () {return Current()->finalization_timeout_; }
      int GetClamMinTimeout () {return Current()->clam_min_timeout_; }
      int GetClamMaxTimeout () {return Current()->clam_max_timeout_; }

      // Seconds; 0 disables the bound. See the comment at the read site.
      int GetDNSQueryTimeout () {return Current()->dns_query_timeout_; }
      int GetClientSessionCeiling () {return Current()->client_session_ceiling_; }
      int GetDBConnectionAcquireTimeout () {return Current()->db_connection_acquire_timeout_; }
      int GetScriptTimeout () {return Current()->script_timeout_; }

      // ScriptAllowedObjects: the COM classes an event script may create with
      // CreateObject / new ActiveXObject, by ProgID or CLSID, comma-separated.
      // "*" - the default when the key is absent - means any class, as before the
      // setting existed; empty means none. Enforced by ScriptObjectPolicy through
      // the script site's IInternetHostSecurityManager.
      String GetScriptAllowedObjects() const { return Current()->script_allowed_objects_; }
      int GetExternalProcessTimeout () {return Current()->external_process_timeout_; }
      int GetAsyncQueueStallThreshold () {return Current()->async_queue_stall_threshold_; }
      int GetAsyncQueueReservedThreads () {return Current()->async_queue_reserved_threads_; }

      // Absolute ceilings on a single IMAP SEARCH / SORT, measured from the start
      // of the search. Seconds and megabytes-of-message-content respectively; 0
      // disables that half. See the comment at the read site for the defaults and
      // for why there are two of them.
      int GetIMAPSearchTimeout () {return Current()->imap_search_timeout_; }
      int GetIMAPSearchMaxMegabytes () {return Current()->imap_search_max_megabytes_; }
      bool GetSAMoveVsCopy() const { return Current()->samove_vs_copy_; }
      String GetAuthUserReplacementIP() const { return Current()->auth_user_replacement_ip_; }
      int GetIndexerFullMinutes () {return Current()->indexer_full_minutes_; }
      int GetIndexerFullLimit () {return Current()->indexer_full_limit_; }
      int GetIndexerQuickLimit () {return Current()->indexer_quick_limit_; }

      // The full-text term index behind IMAP SEARCH BODY/TEXT
      // (hm_messageindexterms). Off by default, and that default is
      // load-bearing: enabling it grows a term table of roughly a few
      // kilobytes per message and starts a backfill over the whole store, so
      // an upgrade must never switch it on by itself. It is a pure filter -
      // search results are identical either way, only the cost changes - so
      // off is always safe. Requires the message indexer to be enabled
      // (Settings.MessageIndexing over COM) for anything to be indexed.
      bool GetIndexerFullTextEnabled () {return Current()->indexer_full_text_enabled_; }
      // Backfill batch: the width of the message-id range one pass covers, so
      // also the most messages one pass can read. See the read site for the
      // clamps.
      int GetIndexerFullTextBatchSize () {return Current()->indexer_full_text_batch_size_; }
      // The shortest search-string run the index may answer for; shorter runs
      // fall back to the scan. Query-side only: the index itself always stores
      // tokens of three characters and up, so LOWERING this back down later
      // stays sound without a re-index.
      int GetIndexerFullTextMinTokenLength () {return Current()->indexer_full_text_min_token_length_; }
      // Distinct terms stored per message before the indexer gives up on it
      // and marks it always-scanned.
      int GetIndexerFullTextMaxTokensPerMessage () {return Current()->indexer_full_text_max_tokens_; }
      int GetLoadHeaderReadSize () {return Current()->load_header_read_size_; }
      int GetLoadBodyReadSize () {return Current()->load_body_read_size_; }
      int GetBlockedIPHoldSeconds () {return Current()->blocked_iphold_seconds_; }
      int GetSMTPDMaxSizeDrop () {return Current()->smtpdmax_size_drop_; }
      bool GetBackupMessagesDBOnly () const { return Current()->backup_messages_dbonly_; }

      // Whether every backup that includes messages is followed by a trial
      // extraction of its message store and a reconciliation of the message rows
      // against it (BackupExecuter::VerifyRestore_). On by default: a backup that
      // has never been restored is a hope. Off for a store too large to exist
      // twice in the temp directory for the duration of the check.
      bool GetBackupVerifyRestore() const { return Current()->backup_verify_restore_; }
      bool GetAddXAuthUserIP () const { return Current()->add_xauth_user_ip_; }
      bool GetRewriteEnvelopeFromWhenForwarding() const { return Current()->rewrite_envelope_from_when_forwarding_; }
      void SetRewriteEnvelopeFromWhenForwarding(bool value);
      // SRS (Sender Rewriting Scheme) for forwarded mail (SPF alignment).
      bool GetSRSEnabled() const { return Current()->srs_enabled_; }
      String GetSRSSecret() const { return Current()->srs_secret_; }
      // BATV (Bounce Address Tag Validation, prvs) for backscatter protection.
      bool GetBATVEnabled() const { return Current()->batv_enabled_; }

      // RFC 2920 and RFC 3030 on the outbound client, each used only when the
      // remote advertises it. On by default: pipelining removes a round trip per
      // envelope command and chunking removes the 354 wait and the dot-stuffing.
      // Off is for a remote that advertises what it cannot do.
      bool GetOutboundPipelining() const { return Current()->outbound_pipelining_; }
      bool GetOutboundChunking() const { return Current()->outbound_chunking_; }

      // Whether an authenticated SMTP session may only put in MAIL FROM an address
      // its account owns - its own, an alias of it - or has been granted, by the
      // post (p) right on another account's INBOX (Send-As). Off by default,
      // because nothing constrained this before 5 September 2026 and an
      // installation may depend on that; on, a compromised or careless account
      // can no longer send as anyone it likes, and a Send-As grant means something.
      bool GetSmtpAuthenticatedSenderCheck() const { return Current()->smtp_authenticated_sender_check_; }

      // How many days of metric samples hm_metricsamples keeps: one row per metric
      // per minute, read back by the Control Panel's dashboard and
      // /api/v1/metrics/history. 7 by default - a week of minute-resolution
      // history is fourteen metrics times ten thousand rows, which is small - and
      // 0 turns the sampler off.
      int GetMetricsHistoryDays() const { return Current()->metrics_history_days_; }
      String GetBATVSecret() const { return Current()->batv_secret_; }
      // Rate shaping (0 = unlimited / disabled).
      int GetMaxSubmissionsPerIPPerMinute() const { return Current()->max_submissions_per_ip_per_minute_; }
      int GetMaxOutboundPerDestinationPerMinute() const { return Current()->max_outbound_per_destination_per_minute_; }
      // RFC 2449 LOGIN-DELAY: the minimum number of seconds a POP3 client should
      // leave between logins for one account, advertised in CAPA and enforced. 0
      // (the default) advertises nothing and enforces nothing.
      int GetPop3LoginDelaySeconds() const { return Current()->pop3_login_delay_seconds_; }

      // Reverse-proxy client-address forwarding for the SMTP listener: the
      // HAProxy PROXY protocol (v1/v2, consumed before TLS and the banner) and
      // the Postfix XCLIENT ESMTP verb. Both are OFF by default and their
      // trusted lists default to EMPTY, and that default is load-bearing: a
      // peer that may rewrite its own source address has defeated DNSBL, SPF,
      // greylisting, auto-ban and every IP-range setting at once, so nothing
      // is trusted until the administrator names it. The lists are
      // comma-separated IP addresses and/or CIDR ranges, always matched
      // against the REAL TCP peer address.
      bool GetSMTPProxyProtocolEnabled() const { return Current()->smtp_proxy_protocol_enabled_; }
      String GetSMTPProxyProtocolTrustedIPs() const { return Current()->smtp_proxy_protocol_trusted_ips_; }
      bool GetSMTPXClientEnabled() const { return Current()->smtp_xclient_enabled_; }
      String GetSMTPXClientTrustedIPs() const { return Current()->smtp_xclient_trusted_ips_; }

      // Spam quarantine. Off by default, and that default is load-bearing: turning
      // it on changes a REFUSAL into an ACCEPTANCE, so a server that has been
      // rejecting spam at the door starts storing it instead. Retention 0 means
      // never sweep, which is a real answer for an administrator keeping evidence.
      bool GetQuarantineEnabled() const { return Current()->quarantine_enabled_; }
      int GetQuarantineRetentionDays() const { return Current()->quarantine_retention_days_; }

      // The queryable message trace. Off by default, and that is a PRIVACY default
      // rather than a performance one: the table records who corresponds with whom,
      // retained, so switching it on should be a decision.
      bool GetMessageTraceEnabled() const { return Current()->message_trace_enabled_; }
      int GetMessageTraceRetentionDays() const { return Current()->message_trace_retention_days_; }

      // The Windows event log sink (WindowsEventLog.h has the whole design, the
      // event-id table and the defence of the default). ON by default: at the
      // default level a healthy server writes nothing, while the administrator
      // who never finds the setting is the one whose only monitoring is Event
      // Viewer. Level is ErrorManager severity: 1 Critical only, 2 adds High,
      // 3 adds Medium, 4 adds Low; LoadSettings clamps it to that range. Both
      // carry their shipped values in the block's declaration, which is the
      // block the constructor publishes, because they are read on the ERROR
      // path, which must behave sensibly even for an error reported before
      // LoadSettings has run.
      bool GetWindowsEventLogEnabled() const { return Current()->windows_event_log_enabled_; }
      int GetWindowsEventLogLevel() const { return Current()->windows_event_log_level_; }

      // RFC 9989 replaces the Public Suffix List with a DNS tree walk for finding a
      // domain's organizational domain. ON by default, because the RFC that made the
      // list authoritative has been obsoleted and a PSL answer is now wrong rather
      // than merely dated. 0 selects the compiled-in list, which stays as the escape
      // hatch for a domain the walk handles worse during the transition.
      bool GetDmarcTreeWalkEnabled() const { return Current()->dmarc_tree_walk_enabled_; }

      // RFC 7208 4.6.4: how many terms of an SPF policy may resolve to nothing
      // before the evaluation is a permerror. The RFC says two and allows the limit
      // to be configurable; 0 disables it, which is the escape hatch for a
      // correspondent whose policy is wrong in exactly this way.
      int GetSpfVoidLookupLimit() const { return Current()->spf_void_lookup_limit_; }

      // Refuse a recipient at RCPT TO when its mailbox is already at or over quota,
      // instead of accepting the message and answering it with a bounce. ON by
      // default, unlike most switches added here, because the behaviour it replaces
      // is not a conservative default - it makes this server send mail to whoever
      // the envelope sender was forged as.
      bool GetRejectFullMailboxAtRcpt() const { return Current()->reject_full_mailbox_at_rcpt_; }

      // RFC 8301 removed rsa-sha1 from DKIM outright: a signer MUST NOT use it and
      // a verifier MUST treat a signature that does as invalid. Off, so this server
      // does neither. 1 restores both halves for an operator with a correspondent
      // still signing that way, at the cost of trusting SHA-1 for the identity DMARC
      // alignment is built on.
      bool GetDkimAcceptSha1() const { return Current()->dkim_accept_sha1_; }

      // Warn an account holder when a delivery takes their mailbox past this
      // percentage of its quota. 0 disables it. On by default at 90, which is
      // unusual for a setting added here - but the alternative is what this server
      // did before, which is to say nothing at all until mail starts being refused,
      // and by then the person it happens to is the last to know.
      int GetQuotaWarningPercent() const { return Current()->quota_warning_percent_; }

      // How long a message stays in ArchiveDir. 0, the default, keeps everything -
      // an archive is usually kept for a legal or contractual reason, and a server
      // that started deleting from one on upgrade would be destroying the evidence
      // it was told to keep.
      int GetArchiveRetentionDays() const { return Current()->archive_retention_days_; }

      // ArchiveDomains: the domains whose mail ArchiveDir keeps. Empty, the
      // default, means every message. Otherwise a message is archived only when
      // its local sender or one of its recipients belongs to a listed domain, and
      // only the copies for listed domains are made - an archive kept for a reason
      // that applies to particular domains must not accumulate everybody else's
      // mail on the side.
      bool GetArchiveScoped() const { return !Current()->archive_domains_.empty(); }
      bool IsArchiveDomain(const String &domain) const;

      // How long a single database statement is allowed to run, in seconds. 0 means
      // no limit.
      //
      // 30 is the default because that is what two of the four backends already did:
      // ADO's CommandTimeout defaults to 30, and SQLScriptRunner has always restored
      // exactly 30 after raising the limit for a schema script. PostgreSQL and MySQL
      // had no statement timeout at all, which is what this setting exists to fix -
      // on those two a statement blocked on a lock held its pooled connection until
      // the network gave up, and the pool is fixed-size.
      //
      // Raising it is the safe direction. Lowering it below the time a legitimate
      // maintenance statement needs - a domain removal, a backup sweep - makes that
      // statement fail rather than merely be slow.
      int GetDatabaseStatementTimeout() const { return Current()->database_statement_timeout_; }

      // How many expunged-message records (hm_imapexpunged) are remembered per IMAP
      // mailbox. 0 keeps every one of them.
      //
      // These are the QRESYNC tombstones: one row per message ever expunged, written
      // so that a client reconnecting after a disconnect can be told which UIDs
      // vanished while it was away. Nothing has ever removed them except deleting the
      // whole folder, so the table grows for the life of the installation and the
      // SELECT that reads it gets slower forever. RFC 7162 section 5.3 anticipates
      // exactly this and says to cap the queue and expire the oldest records.
      //
      // Capping it is safe because of the rule in RFC 7162 section 3.2.6: a client
      // asking about a mod-sequence older than the oldest record still held is told
      // about every UID in the requested set that is no longer in the mailbox, rather
      // than about nothing. So the cost of setting this low is a larger VANISHED
      // response for a client that has been away a long time - never a client left
      // believing a deleted message is still there.
      int GetIMAPExpungeRetentionRecords() const { return Current()->imap_expunge_retention_records_; }

      // Whether /metrics carries per-domain message counters. Off by default, and
      // that is a cardinality decision rather than a cautious one: every label value
      // is a separate time series in whatever scrapes this, and an operator hosting
      // several thousand domains should choose to pay for that rather than discover
      // it. The values themselves are safe - only domains this server hosts are ever
      // labelled - so the cost is volume, not exposure.
      bool GetMetricsPerDomainEnabled() const { return Current()->metrics_per_domain_enabled_; }

      // An external filtering engine in the path of every accepted message - rspamd,
      // most likely. Empty disables it, which is the default: this sends the whole
      // message somewhere, and that is not something to start doing on an upgrade.
      String GetFilterHookUrl() const { return Current()->filter_hook_url_; }

      // How long the engine gets, for the whole exchange. This runs while an SMTP
      // client waits for its answer to DATA, so the number is a promise about the
      // worst case rather than a hint.
      int GetFilterHookTimeoutSeconds() const { return Current()->filter_hook_timeout_seconds_; }

      // What happens when the engine does not answer. Open by default: a filter
      // outage that lets spam through is recoverable, and one that stops the mail
      // while nobody is watching ends in bounces.
      bool GetFilterHookFailClosed() const { return Current()->filter_hook_fail_closed_; }

      // What the engine's "reject" is worth, in the same units as every other spam
      // test - so it still passes through the administrator's thresholds instead of
      // being the one verdict that ignores them.
      int GetFilterHookRejectScore() const { return Current()->filter_hook_reject_score_; }

      // Messages above this are passed without being sent. 0 means no ceiling.
      int GetFilterHookMaxMessageSizeKB() const { return Current()->filter_hook_max_message_size_kb_; }

      // Password policy, enforced where a password is CHOSEN (Account.Password over
      // COM) and never where one is verified or re-hashed. All off at 0/false, which
      // is what an existing installation gets until an administrator decides
      // otherwise - a policy applied retroactively locks people out of their own
      // mailboxes without warning.
      int GetPasswordPolicyMinimumLength() const { return Current()->password_policy_minimum_length_; }
      bool GetPasswordPolicyRequireMixedCase() const { return Current()->password_policy_require_mixed_case_; }
      bool GetPasswordPolicyRequireDigit() const { return Current()->password_policy_require_digit_; }
      bool GetPasswordPolicyRequireNonAlphanumeric() const { return Current()->password_policy_require_non_alphanumeric_; }
      bool GetPasswordPolicyRejectCommon() const { return Current()->password_policy_reject_common_; }

      // Reuse history and maximum age. Both 0 (off) by default.
      //
      // Expiry has a consequence worth stating where it is read as well as where it
      // is set: this server has no self-service password change, so an expired
      // password must be reset by an administrator. History has no such catch - it
      // only ever refuses a CHANGE, at the moment somebody can pick something else.
      int GetPasswordPolicyHistoryCount() const { return Current()->password_policy_history_count_; }
      int GetPasswordPolicyMaximumAgeDays() const { return Current()->password_policy_maximum_age_days_; }
      bool GetUseDNSCache() const { return Current()->use_dns_cache_; }
      String GetDNSServer() const { return Current()->dns_server_; }

      bool GetMtaStsEnabled() const { return Current()->mta_sts_enabled_; }

      // What an auto-ban does below the server - see AutoBanFirewall.h.
      bool GetAutoBanFirewallEnabled() const { return Current()->auto_ban_firewall_enabled_; }
      String GetAutoBanCommand() const { return Current()->auto_ban_command_; }
      String GetAutoBanNeverBan() const { return Current()->auto_ban_never_ban_; }
      bool GetDaneEnabled() const { return Current()->dane_enabled_; }
      bool GetDnssecValidationEnabled() const { return Current()->dnssec_validation_enabled_; }
      String GetDnssecTrustAnchors() const { return Current()->dnssec_trust_anchors_; }
      // The update check: off until an administrator opts in, the GitHub Releases API
      // unless UpdateFeedUrl says otherwise, stable unless UpdateChannel=prerelease.
      bool GetUpdateCheckEnabled() const { return Current()->update_check_enabled_; }
      String GetUpdateChannel() const { return Current()->update_channel_; }
      String GetUpdateFeedUrl() const { return Current()->update_feed_url_; }

      // A forward proxy for every request this server makes as a web client - the
      // update feed and its downloads, JWKS, token introspection. host:port (or
      // [ipv6]:port); empty means direct. No proxy credentials: a proxy that wants
      // them refuses the CONNECT, and the refusal is reported with the proxy's name.
      String GetHttpProxy() const { return Current()->http_proxy_; }

      // This server implements neither CalDAV nor CardDAV. These are the URLs the
      // well-known discovery endpoints redirect to - the server that does.
      String GetCalDavRedirectUrl() const { return Current()->caldav_redirect_url_; }
      String GetCardDavRedirectUrl() const { return Current()->carddav_redirect_url_; }
      int GetUpdateCheckHours() const { return Current()->update_check_hours_; }
      // RFC 4978 COMPRESS=DEFLATE on IMAP; on unless an operator says otherwise.
      bool GetImapCompressionEnabled() const { return Current()->imap_compression_enabled_; }
      // The verify: what the installer's Sigstore bundle is checked against. Empty
      // means the public Sigstore instance and this repository's release workflow.
      String GetUpdateTrustRootsFile() const { return Current()->update_trust_roots_file_; }
      String GetUpdateLogPublicKeyFile() const { return Current()->update_log_public_key_file_; }
      String GetUpdateSigningIdentity() const { return Current()->update_signing_identity_; }
      String GetUpdateSigningIssuer() const { return Current()->update_signing_issuer_; }
      String GetUpdateSourceRepository() const { return Current()->update_source_repository_; }
      bool GetUpdateRequireAuthenticode() const { return Current()->update_require_authenticode_; }
      // How long the update helper gives the service to come back after the installer
      // has run before it rolls back.
      int GetUpdateServiceWaitSeconds() const { return Current()->update_service_wait_seconds_; }
      // The unattended part: when a verified update may be applied without a click
      // (UpdateWindow, see Util/UpdateWindow.h), whether a newer release is fetched
      // and verified as soon as the check finds it, and whether a backup comes first.
      String GetUpdateWindow() const { return Current()->update_window_; }
      bool GetUpdateAutoDownload() const { return Current()->update_auto_download_; }
      bool GetUpdateBackupBeforeApply() const { return Current()->update_backup_before_apply_; }
      bool GetJsonLogging() const { return Current()->json_logging_; }
      int GetLogDeleteDays() const { return Current()->log_delete_days_; }
      int GetShutdownDrainSeconds() const { return Current()->shutdown_drain_seconds_; }
      // Statements that take at least this many milliseconds are counted as slow and
      // logged (redacted). 0 disables the slow-query log (latency metrics still run).
      int GetSlowQueryLogMilliseconds() const { return Current()->slow_query_log_ms_; }
      bool GetMessageStoreFsync() const { return Current()->message_store_fsync_; }

      // Free-space policy on the volume holding the message store. Both in
      // megabytes, both disabled by 0. See Common/Util/DiskSpace.h for what is
      // and is not guaranteed by them.
      //
      // MinimumFreeDiskSpaceMB is the floor below which new mail is refused with
      // a TEMPORARY failure (SMTP 452 4.3.1; IMAP APPEND NO [UNAVAILABLE]). The
      // default is 100 MB, and the number is argued rather than picked:
      //
      //   * It has to be above the largest thing accepting one message can cost.
      //     The shipped maximum message size is 20480 KB, and local delivery
      //     copies the spool file once per local recipient, so a single accepted
      //     message can cost several multiples of 20 MB. 100 MB is five of them.
      //
      //   * It has to be far below what any working installation sits at. 100 MB
      //     is 0.4% of a 25 GB volume, which is about the smallest anybody runs a
      //     mail server on. A server with less than 100 MB free has problems that
      //     are not about mail.
      //
      //   * The two errors are not symmetrical. Refusing early costs delivery
      //     latency and nothing else, because the refusal is temporary and a
      //     sending server retries for days. Reaching zero costs a database that
      //     will not open and a service that will not start. So the floor is
      //     biased upwards.
      //
      // DiskSpaceWarningThresholdMB is where the administrator is TOLD, in the
      // application log, well before any of that happens; 1024 MB by default,
      // which is roughly fifty more maximum-size messages. Absolute rather than a
      // percentage on purpose: what decides whether the next message fits is how
      // many bytes are left, not what fraction of the volume they are. The
      // percentage is written into the log line, where it is information rather
      // than a trigger.
      int GetMinimumFreeDiskSpaceMB() const { return Current()->minimum_free_disk_space_mb_; }
      int GetDiskSpaceWarningThresholdMB() const { return Current()->disk_space_warning_threshold_mb_; }

      // Fault injection, for the regression suite only: makes writes of received message
      // data to the spool file report failure without writing anything.
      //
      //   0  off.
      //   1  every write fails, so the spool file stays empty.
      //   2  the first write succeeds and every write after it fails, so the spool file
      //      is left non-empty and truncated.
      //
      // Two modes because the two shapes are caught by different things, and only one of
      // them was ever dangerous. An empty spool file was already refused before this
      // work - not by any check that meant to catch it, but because the accept path
      // failed downstream on a message with no content, with nothing reported. A
      // *truncated* file is the shape a disk that fills up mid-message actually has, and
      // it passes every downstream guard: it has content, it has headers, its size is
      // non-zero. That one was accepted with a 250 and delivered short, which is the
      // defect. Mode 2 is therefore the mode that matters, and the one whose test fails
      // against the unfixed build.
      //
      // It exists because the alternative is filling a real disk, which is not a test.
      // CrashSimulationMode is the existing precedent for a switch of this kind.
      // Deliberately not exposed in the Control Panel, and read from the ini rather than
      // over COM, so that turning it on takes a deliberate edit plus a reinitialize.
      int GetSimulateSpoolWriteFailure() const { return Current()->simulate_spool_write_failure_; }

      // Test-only fault injection for the database layer, and the reason it exists is
      // worth stating: three sweeps in August 2026 fixed 51 places where the result of
      // a database write was discarded, and every one of those defects lived in error
      // handling that had never executed. Adding 51 more error branches that also never
      // execute repeats the mistake at one remove. This is what lets a test make one
      // specific statement fail so the branch actually runs.
      //
      // A substring of the statement rather than a switch, so a test can name exactly
      // the write it wants to break - "update hm_imapfolders set foldercurrentuid" -
      // and every other statement on every other thread carries on normally. That
      // precision is what makes it usable at all on a running server with delivery
      // threads working.
      //
      // Empty out of the box, which is the whole of its safety: the check is one
      // IsEmpty() per statement, it can only be turned on by storing it (COM, REST,
      // hmctl - it applies when saved), and HM6119 is reported whenever it is set:
      // at every start and reinitialise, and by a subscriber the moment a save sets
      // it - because a fault injector that is silently on is far worse than not
      // having one. Follows SimulateSpoolWriteFailure and CrashSimulationMode, which
      // are the same idea for the disk and the crash handler.
      // Asked before every single database statement, so it must cost nothing when the
      // facility is off. Every String getter in this class returns by value, which is
      // fine for settings read once at startup and quite wrong for one read per
      // statement - a heap copy per query is not a price a mail server should pay for a
      // test hook. Hence the bool: the string is only fetched once it says yes.
      bool GetSimulateDatabaseFailureEnabled() const { return simulate_database_failure_enabled_.load(std::memory_order_relaxed); }

      String GetSimulateDatabaseFailureFor() const { return Current()->simulate_database_failure_for_; }
      bool GetMessageStoreConsistencyCheck() const { return Current()->message_store_consistency_check_; }
      int GetMetricsServerPort() const { return Current()->metrics_server_port_; }
      String GetMetricsServerBindAddress() const { return Current()->metrics_server_bind_address_; }

      // Access control on the metrics listener. All five are absent by default, so a
      // loopback scrape behaves exactly as it always has, and /livez, /readyz and
      // /healthz are never authenticated whatever these say - a load balancer cannot
      // present a credential and a probe cannot hold a secret.
      //
      // A credential is REQUIRED for /metrics when the bind address is not loopback:
      // without one /metrics answers 503 and names these settings, while the listener
      // and the probes keep serving. Refusing to start instead would have taken the
      // probes down with the exposition, which is the configuration
      // docs/HighAvailabilityRunbook.md section 3 actually ships.
      String GetMetricsServerAuthToken() const { return Current()->metrics_server_auth_token_; }
      String GetMetricsServerAuthUsername() const { return Current()->metrics_server_auth_username_; }
      String GetMetricsServerAuthPassword() const { return Current()->metrics_server_auth_password_; }

      // Both must be set to serve HTTPS on the metrics port. Never inferred: a
      // non-loopback bind without TLS is warned about in the application log and served,
      // because a monitoring endpoint that silently stops answering is worse than one
      // that answers over plain HTTP on a network the operator chose.
      String GetMetricsServerCertificateFile() const { return Current()->metrics_server_certificate_file_; }
      String GetMetricsServerPrivateKeyFile() const { return Current()->metrics_server_private_key_file_; }

      // Scheduled backups. Absent means the task is never constructed and no archive is
      // ever deleted, so an upgrade changes nothing until an administrator asks for it.
      //
      // ScheduledBackupTime is 24-hour local "HH:MM" and wins over the interval when
      // both are set. What goes into a backup and where it is written still come from the
      // existing backup settings; these only decide WHEN a backup happens and which old
      // archives survive.
      String GetScheduledBackupTime() const { return Current()->scheduled_backup_time_; }
      int GetScheduledBackupIntervalMinutes() const { return Current()->scheduled_backup_interval_minutes_; }
      int GetScheduledBackupKeepCount() const { return Current()->scheduled_backup_keep_count_; }
      int GetScheduledBackupMaxAgeDays() const { return Current()->scheduled_backup_max_age_days_; }
      // OTLP/HTTP collector endpoint for OpenTelemetry trace export (e.g.
      // http://127.0.0.1:4318/v1/traces). Empty disables tracing.
      String GetOtelEndpoint() const { return Current()->otel_endpoint_; }
      String GetOtelServiceName() const { return Current()->otel_service_name_; }

      // The other two OTLP signals, each gated exactly as the traces signal is:
      // by its own endpoint URL, empty (and therefore off) by default. The
      // metrics signal pushes the same ServerStatus counters /metrics scrapes;
      // the logs signal forwards what the configured log mask already produces.
      String GetOtelMetricsEndpoint() const { return Current()->otel_metrics_endpoint_; }
      String GetOtelLogsEndpoint() const { return Current()->otel_logs_endpoint_; }

      // Seconds between metric pushes. Clamped by the exporter to 5..3600.
      int GetOtelMetricsInterval() const { return Current()->otel_metrics_interval_; }
      int GetManageSieveServerPort() const { return Current()->manage_sieve_server_port_; }
      String GetManageSieveServerBindAddress() const { return Current()->manage_sieve_server_bind_address_; }
      bool GetArcSealingEnabled() const { return Current()->arc_sealing_enabled_; }

      // RFC 6376 3.5 signature timestamps.
      //
      // How long a signature we PRODUCE claims to stay valid. 0 - the default - emits
      // no x= at all, keeping the emitted tag set as it was. An expiry is a promise
      // about our own outbound mail that cannot be withdrawn once sent, and a window
      // shorter than the delay a legitimate retry, greylist or mailing list introduces
      // costs the message its DKIM pass and its DMARC alignment at the far end. A
      // sender who wants one usually picks around 604800 (a week).
      int GetDKIMSignatureValiditySeconds() const { return Current()->dkim_signature_validity_seconds_; }

      // Whether a signature we VERIFY whose x= has passed is refused. On by default:
      // x= is inside the bytes b= covers, so it is the signer's own instruction rather
      // than something a third party can inject, and a check that ships off ships dead.
      bool GetDKIMEnforceSignatureExpiry() const { return Current()->dkim_enforce_signature_expiry_; }

      // RFC 6376 3.5's "fudge factor" for clock drift between us and the signer.
      // Without it, our own clock running a few minutes fast turns other people's
      // valid mail into a DKIM failure.
      int GetDKIMExpiryClockSkewSeconds() const { return Current()->dkim_expiry_clock_skew_seconds_; }

      // Header field names to oversign (RFC 6376 5.4): listed in h= once more often
      // than the message carries them, so an instance ADDED after signing lands where
      // nothing was hashed and the signature fails. Colon, comma or semicolon
      // separated. Empty by default - oversigning a field the message does not carry
      // also forbids a legitimate intermediary from adding a first one, and a
      // discussion list adding Reply-To or Sender is normal, so the administrator says.
      String GetDkimOversignHeaders() const { return Current()->dkim_oversign_headers_; }

      // RFC 8601 Authentication-Results and RFC 7208 9.1 Received-SPF on inbound mail.
      //
      // Off by default. The header is a statement other systems act on, and it is only
      // worth anything if the reader trusts this server's name - so switching it on is
      // a deliberate act, not something an upgrade does quietly.
      bool GetAuthenticationResultsEnabled() const { return Current()->authentication_results_enabled_; }
      bool GetReceivedSpfHeaderEnabled() const { return Current()->received_spf_header_enabled_; }

      // The authserv-id: the name this server authenticates as. Empty means use the
      // computer name, which is what the Received header already says the message was
      // received "by", so the two agree. Set it explicitly when several servers share
      // one administrative identity.
      String GetAuthenticationResultsIdentity() const { return Current()->authentication_results_identity_; }
      String GetTlsRptFromAddress() const { return Current()->tls_rpt_from_address_; }
      String GetTlsRptOrganizationName() const { return Current()->tls_rpt_organization_name_; }
      String GetDmarcRptFromAddress() const { return Current()->dmarc_rpt_from_address_; }
      String GetDmarcRptOrganizationName() const { return Current()->dmarc_rpt_organization_name_; }

      // 1 = RFC 7489 Appendix C (the shipped default, and the only form report
      // processors in the field reliably parse); 2 = RFC 9990 / DMARCbis.
      // Validated at load, so this only ever returns 1 or 2.
      int GetDmarcRptSchemaVersion() const { return Current()->dmarc_rpt_schema_version_; }
      // 0 = deliver a message the scanner could not examine (the shipped default,
      // and what this server has always done); 1 = hold it, retry on the ordinary
      // schedule, and bounce it if the scanner is still unreachable when the retry
      // budget runs out. See SMTPDeliverer::HandleUnscannableMessage_.
      int GetAVFailAction() const { return Current()->av_fail_action_; }

      // How long a held message waits before the next scan attempt, and how many
      // such attempts it gets before it is returned to the sender. Both are the
      // hold's own settings rather than the SMTP delivery retry budget, because
      // `smtpnooftries` ships as 0: borrowing it would make AVFailAction=1 bounce
      // on the first scanner blip and never hold at all, so the feature would not
      // do what its name says on any stock installation.
      int GetAVFailRetryMinutes() const { return Current()->av_fail_retry_minutes_; }

      // Counted in ATTEMPTS, deliberately not in elapsed time. An earlier version
      // bounded the hold by the message's age, which is anchored to its ARRIVAL:
      // any message already older than the window - a backlog released after an
      // outage, an ETRN batch, POP3-fetched mail carrying an upstream Received
      // date - would then have had its whole budget spent before the scanner ever
      // failed, and would be bounced on the first attempt with no hold at all.
      // Restarting a server whose scanner takes a minute longer to come up than
      // the mail queue does would have mass-bounced the entire backlog. Attempts
      // are something this policy itself advances, so they cannot already be
      // spent, and a future-dated arrival time cannot make the hold unbounded.
      //
      // 0 means do not hold at all - tell the sender immediately. A legitimate
      // choice for an operator who never wants unscanned mail sitting in a queue.
      int GetAVFailMaxHolds() const { return Current()->av_fail_max_holds_; }
      int GetAccountLockoutThreshold() const { return Current()->account_lockout_threshold_; }
      int GetAccountLockoutWindowMinutes() const { return Current()->account_lockout_window_minutes_; }
      int GetAccountLockoutMinutes() const { return Current()->account_lockout_minutes_; }

      // The longest pause either tarpit will queue, whatever the ini says.
      static const int TARPIT_MAX_SECONDS = 30;

      // Logon tarpit: each failed logon's refusal on a connection waits this many
      // seconds times the number of failures that connection has made, capped at
      // TARPIT_MAX_SECONDS. 0 (the default) refuses at once.
      int GetLogonTarpitSeconds() const { return Current()->logon_tarpit_seconds_; }

      // Recipient tarpit: in an unauthenticated SMTP session, every RCPT TO past
      // the first SmtpTarpitCount waits SmtpTarpitDelaySeconds before its reply.
      // Either at 0 (the default) means no tarpit. Settable at run time through
      // AntiSpam.TarpitCount / TarpitDelay, which is why these have setters.
      int GetSmtpTarpitCount() const { return Current()->smtp_tarpit_count_; }
      int GetSmtpTarpitDelaySeconds() const { return Current()->smtp_tarpit_delay_seconds_; }
      bool SetSmtpTarpitCount(int count);
      bool SetSmtpTarpitDelaySeconds(int seconds);
      // TLS key-exchange groups, in OpenSSL group-list syntax, most preferred
      // first. The default puts the hybrid post-quantum KEMs ahead of the
      // classical curves: a PQC-capable peer then gets a quantum-resistant
      // handshake, and a peer that does not know them simply negotiates one of
      // the classical groups that follow. An empty value, or a value OpenSSL
      // rejects, falls back to the classical list.
      //
      // Scope: this reaches the SMTP, POP3, IMAP and ManageSieve listeners, which
      // all get their context from SslContextInitializer via TCPServer, plus
      // outbound SMTP delivery. It does NOT reach the REST API listener or the
      // Web Services HTTPS listener - both build their own SSL_CTX directly and
      // never consult SslContextInitializer. Unifying that is worth doing, but it
      // is a wider change than this setting.
      String GetTlsKeyExchangeGroups() const { return Current()->tls_key_exchange_groups_; }
      // TLS 1.3 cipher suites, colon separated, most preferred first, in the RFC 8446
      // names: TLS_AES_256_GCM_SHA384, TLS_CHACHA20_POLY1305_SHA256,
      // TLS_AES_128_GCM_SHA256, TLS_AES_128_CCM_SHA256, TLS_AES_128_CCM_8_SHA256.
      //
      // These are NOT the OpenSSL cipher-list names SslCipherList uses. OpenSSL keeps
      // the two lists apart and neither constrains the other, so SslCipherList - which
      // goes through SSL_CTX_set_cipher_list - governs TLS 1.2 and below only and
      // restricts nothing on TLS 1.3, the version most peers now negotiate and one this
      // server ships with enabled. Before this setting existed there was no way to
      // influence the TLS 1.3 suites at all.
      //
      // Empty is the default and means "leave OpenSSL's own defaults alone". It has to
      // be handled by not calling the setter: OpenSSL accepts an empty list, returns
      // success, and leaves the context with no TLS 1.3 suite at all, after which every
      // TLS 1.3 handshake fails with no shared cipher.
      //
      // Scope matches GetTlsKeyExchangeGroups above: every context built by
      // SslContextInitializer, i.e. the SMTP, POP3, IMAP and ManageSieve listeners and
      // outbound delivery, but NOT the REST API or Web Services listeners, which build
      // their own SSL_CTX.
      String GetTlsCipherSuites13() const { return Current()->tls_cipher_suites13_; }

      // TLS session resumption and session-ticket management, on the listener contexts
      // SslContextInitializer builds. Every one of these defaults to "make no OpenSSL
      // call at all", which is the house pattern here and the only way to mean "leave
      // the library's own behaviour alone".
      //
      // Tickets on is today's behaviour. Turning them off sets SSL_OP_NO_TICKET AND
      // SSL_CTX_set_num_tickets(0), because the option alone only makes TLS 1.3 tickets
      // stateful rather than absent.
      bool GetTlsSessionTicketsEnabled() const { return Current()->tls_session_tickets_enabled_; }

      // 0 leaves OpenSSL's cache size; a negative value turns the server-side
      // session-ID cache off. Bounds what a client churning handshakes can make this
      // server remember.
      int GetTlsSessionCacheSize() const { return Current()->tls_session_cache_size_; }

      // 0 leaves OpenSSL's timeout. Bounds how long a stolen resumption secret stays
      // usable.
      int GetTlsSessionTimeoutSeconds() const { return Current()->tls_session_timeout_seconds_; }

      // 0 - the default - installs no ticket-key callback, so OpenSSL keeps its own key:
      // one per context, generated at startup and never rotated, so every ticket the
      // process ever issues is sealed under it and a key recovered later decrypts all of
      // them. A positive interval rotates, keeping the previous key so tickets issued
      // under it still resume, which bounds that exposure to two intervals.
      int GetTlsTicketKeyRotationSeconds() const { return Current()->tls_ticket_key_rotation_seconds_; }
      int GetRestApiPort() const { return Current()->rest_api_port_; }
      String GetRestApiBindAddress() const { return Current()->rest_api_bind_address_; }
      String GetRestApiCertificateFile() const { return Current()->rest_api_certificate_file_; }
      String GetRestApiPrivateKeyFile() const { return Current()->rest_api_private_key_file_; }
      bool GetAcmeEnabled() const { return Current()->acme_enabled_; }
      String GetAcmeDirectoryUrl() const { return Current()->acme_directory_url_; }
      String GetAcmeContactEmail() const { return Current()->acme_contact_email_; }
      String GetAcmeDomains() const { return Current()->acme_domains_; }
      String GetAcmeCertificateDirectory() const { return Current()->acme_certificate_directory_; }
      int GetAcmeHttpPort() const { return Current()->acme_http_port_; }
      bool GetAcmeReuseKey() const { return Current()->acme_reuse_key_; }
      int GetWebServicesHttpPort() const { return Current()->web_services_http_port_; }
      int GetWebServicesHttpsPort() const { return Current()->web_services_https_port_; }
      String GetWebServicesBindAddress() const { return Current()->web_services_bind_address_; }
      String GetWebServicesCertificateFile() const { return Current()->web_services_certificate_file_; }
      String GetWebServicesPrivateKeyFile() const { return Current()->web_services_private_key_file_; }
      String GetWebServicesTrustedProxies() const { return Current()->web_services_trusted_proxies_; }
      bool GetMtaStsHostingEnabled() const { return Current()->mta_sts_hosting_enabled_; }
      String GetMtaStsPolicyMode() const { return Current()->mta_sts_policy_mode_; }
      int GetMtaStsPolicyMaxAge() const { return Current()->mta_sts_policy_max_age_; }
      String GetMtaStsPolicyMx() const { return Current()->mta_sts_policy_mx_; }
      bool GetAutoconfigEnabled() const { return Current()->autoconfig_enabled_; }
      String GetAutoconfigClientHost() const { return Current()->autoconfig_client_host_; }
      std::set<int> GetAuthDisabledOnPorts();

   private:   

      void WriteIniSetting_(const String &sSection, const String &sKey, const String &sValue);

      // A [Directories] value that is not a full path is taken to be relative to
      // the program folder, so that an installation can be written to be movable
      // (docs/RelocatingAnInstallation.md, issue #158). An absolute value, which
      // is what every installation before 6.3.0 holds, is returned exactly as
      // it was; an empty one stays empty, because empty means "not set" to
      // every reader of these.
      String ResolveAgainstProgramDirectory_(const String &value) const;
      void WriteIniSetting_(const String &sSection, const String &sKey, int Value);

      // Holds a tarpit delay in [0, TARPIT_MAX_SECONDS]. A negative value is off;
      // a huge one would be a pause longer than any protocol's idle timeout.
      static int ClampTarpitSeconds_(int seconds);
      String ReadIniSettingString_(const String &sSection, const String &sKey, const String &sDefault);
      int ReadIniSettingInteger_(const String &sSection, const String &sKey, int iDefault);

      // What the two above resolve a key to - the overlay, an override, the file, the
      // default - without a build: the reads themselves, and the pending list.
      String ResolveString_(const String &sSection, const String &sKey, const String &sDefault);
      int ResolveInteger_(const String &sSection, const String &sKey, int iDefault);

      // A [Settings] work factor: 0 (the default, meaning "built-in") or a value within
      // [minimum, maximum]. Anything else is reported and read as 0.
      int ReadPasswordHashWorkFactor_(const String &sKey, int minimum, int maximum);

      /// <summary>
      /// The stored [Settings] values with any [SettingsOverride] applied over them,
      /// or empty before LoadDatabaseSettings has run. Consulted ONLY for the
      /// [Settings] section: the keys that are needed to reach the database in the
      /// first place - everything in [Database], [Directories] and the administrator
      /// password - can never come from the database, which is the ordering answer
      /// and the security answer at once. A server whose database location could be
      /// changed through the database it is currently using is a server that can be
      /// pointed somewhere else by anyone who reaches the one it is on.
      ///
      /// Guarded because the two readers are called from LoadSettings, which runs on
      /// the startup thread and again on a COM thread (get_Directories,
      /// CreateInternalDatabase). Recursive because Synchronize writes the map's
      /// source while holding nothing, and the house type for this is
      /// boost::recursive_mutex.
      /// </summary>
      std::map<String, String> database_settings_;
      bool database_settings_loaded_;

      /// <summary>
      /// The names in [SettingsOverride], kept beside the values so that a caller
      /// about to store a value can be told the override will go on shadowing it.
      /// </summary>
      std::set<String> file_overrides_;
      mutable boost::recursive_mutex database_settings_mutex_;

      String database_server_;
      String database_name_;
      String username_;
      String password_;
      String data_directory_;
      String app_directory_;
      String logs_directory_;
      String temp_directory_;
      String event_directory_;
      String dbscript_directory_;
      String database_directory_;
      String database_server_FailoverPartner;
      String administrator_password_;
      // As stored: protected, not the base32 secret. See GetAdministratorTotpSecret.
      String administrator_totp_secret_protected_;

      std::vector<String> valid_languages_;

      DatabaseSettings::SQLDBType sqldbtype_;
      long dbport_;

      int no_of_dbconnections_;
      int no_of_dbconnection_attempts_;
      int no_of_dbconnection_attempts_Delay;
      bool database_allow_unencrypted_connection_;
      bool database_upgrade_on_start_;
      String database_postgresql_ssl_mode_;
      String database_postgresql_ssl_root_cert_;
      String database_connection_string_options_;

      bool is_internal_database_;

      String log_directory_;
      static String ini_file_;

      String database_provider_;

      // ---- the block, and what publishes it -------------------------------------

      /// <summary>
      /// Builds a new [Settings] block and publishes it. The reads go through
      /// ReadIniSettingString_ and ReadIniSettingInteger_ with a build context on
      /// this thread, which is what records each key's resolved value and, for a key
      /// the scope must not apply, answers the running block's value instead of the
      /// store's. reload is true for Reload(), which reports a value it has to
      /// correct only when that value changed - a save of one setting must not
      /// repeat the complaint about another every time.
      /// </summary>
      void BuildAndPublish_(LoadScope scope, bool reload);

      /// <summary>
      /// Whether this build of the block should report a value it corrects: always
      /// at a start or a reinitialise, and on a reload only when the key moved.
      /// </summary>
      bool ReportThisLoad_(const String &key) const;

      /// <summary>The overlay re-read for a reload: the rows, read only, with the last load's overrides over them.</summary>
      void RefreshDatabaseSettings_();

      struct Subscription
      {
         std::vector<String> names;
         std::function<void()> call;
      };

      /// <summary>
      /// Reload with the reload lock already held: the overlay refreshed, a block
      /// built and published, and the subscriptions whose keys moved returned for
      /// Tell_ to call once the lock is released.
      /// </summary>
      std::vector<Subscription> ReloadLocked_();

      /// <summary>The subscriptions with a key whose resolved value differs between the two blocks.</summary>
      std::vector<Subscription> Due_(const std::shared_ptr<const Values> &before, const std::shared_ptr<const Values> &after);

      /// <summary>Calls them, one at a time, with the reload lock released; see the definition.</summary>
      void Tell_(const std::vector<Subscription> &due);

      /// <summary>The value a load would resolve for one baseline key now, outside any build.</summary>
      String ResolveStored_(const String &name, const String &defaultValue, bool isInteger);

      // Kept beside the block rather than in it: asked before every single database
      // statement, where even an atomic load of the block is a price not worth paying
      // for a test hook. Set after each publish from the block's own value.
      std::atomic<bool> simulate_database_failure_enabled_;

      // Set once the servers have been started on the store's values; see
      // LatchRunningValues.
      std::atomic<bool> running_values_latched_;

      // The block. Read with std::atomic_load and replaced with std::atomic_store
      // (or the std::atomic<std::shared_ptr> of a C++20 library that has one), so a
      // reader never sees a half-written pointer and a block lives until its last
      // reader lets go.
#if defined(__cpp_lib_atomic_shared_ptr)
      std::atomic<std::shared_ptr<const Values>> values_;
#else
      std::shared_ptr<const Values> values_;
#endif

      // Advanced after every publish, so Current() can tell without touching
      // values_ whether the block a thread holds is still the published one.
      std::atomic<unsigned long long> values_generation_;

      // One build at a time, and ONE HOLD for a save's store write and its reload:
      // WriteSettingsValue and RemoveSettingsValue take it before they write the
      // store and release it after they publish, so no other save's reload reads the
      // store half-way through a write, and no LoadSettings() publishes a value
      // before the save that stored it has compared its blocks. LoadSettings,
      // LoadDatabaseSettings and BuildAndPublish_ take it too. The subscribers are
      // called after it is released (Tell_). Recursive, because the save holds it
      // across BuildAndPublish_, which takes it again.
      mutable boost::recursive_mutex reload_mutex_;

      // One subscriber call at a time; see Tell_. Recursive, because a subscriber
      // may itself save a setting.
      boost::recursive_mutex notify_mutex_;

      // The [SettingsOverride] values the last LoadDatabaseSettings applied - at a
      // start or a reinitialise - applied again by every save's reload so that an
      // override keeps winning until the section is next read.
      std::map<String, String> start_overrides_;

      std::vector<Subscription> subscriptions_;
      boost::mutex subscriptions_mutex_;

      friend class IniFileSettingsTester;
   };

   /// <summary>
   /// The block swapped under load, in ClassTester::DoTests: see the definition. A
   /// friend, because a thousand publishes a second cannot go through the database,
   /// so the tester changes the in-memory overlay and publishes from it directly.
   /// </summary>
   class IniFileSettingsTester
   {
   public:
      void Test();

   private:
      static void SetOverlay_(const String &key, const String &value);
      static bool GetOverlay_(const String &key, String &value);
      static void RestoreOverlay_(const String &key, bool had, const String &value);
      static void Publish_();
      static bool OverlayLoaded_();
      static void TestSavesRacingSaves_();
   };
}
