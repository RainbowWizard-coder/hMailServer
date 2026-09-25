-- Schema 6102: every table utf8mb4 under utf8mb4_general_ci, and InnoDB with ROW_FORMAT=DYNAMIC.
-- MySQL's utf8 is three bytes a character and refuses anything outside the Basic Multilingual
-- Plane (error 1366 under strict mode); utf8mb4_general_ci is utf8_general_ci's own twin, so
-- every comparison and every unique index over text that can already hold a value compares it
-- exactly as before. InnoDB and DYNAMIC because a varchar(255) is 1020 bytes at four a character:
-- a MyISAM key stops at 1000 and a COMPACT key part at 767, and an old installation's tables can be
-- either. Every statement here can be run again, so a step stopped part of the way - a duplicate
-- refused by a unique index, a disk that filled - is finished by running it again once the cause
-- is dealt with.
--
-- Each table is rebuilt. MariaDB's statement time limit, which the server sets to half an hour
-- while it runs a script, stops an ALTER TABLE too (error 1969), so a table whose rebuild takes
-- longer would be stopped at the same place every time; it is lifted for this connection, and the
-- script runner puts it back when the script ends. MySQL's limit (max_execution_time) applies to
-- SELECT alone.
SET @hm_statement_time = IF(VERSION() LIKE '%MariaDB%', 'SET SESSION max_statement_time = 0', 'DO 0');

PREPARE hm_statement_time FROM @hm_statement_time;

EXECUTE hm_statement_time;

DEALLOCATE PREPARE hm_statement_time;

-- hm_accounts is the first table because its unique address is a 1020-byte key: on a server too
-- old to hold one (the minimum is MySQL 5.7.9 or MariaDB 10.2.2) this statement fails with error
-- 1071 and nothing has been changed.
ALTER TABLE hm_accounts ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

-- A table a later script creates without naming a character set is utf8mb4 too (none should:
-- build/check-mysql-charset.py refuses one). Changing a database's default needs the ALTER
-- privilege on the database itself, which GRANT ALL PRIVILEGES ON <database>.* gives; without
-- it this statement fails with error 1044, and the step can be run again once it is granted.
ALTER DATABASE CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_aliases ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_domains ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_domain_aliases ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_messages ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_storeblobs ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_message_metadata ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_settings ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_inisettings ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_dbversion ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_distributionlists ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_distributionlistsrecipients ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_messagerecipients ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_imapfolders ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_imapexpunged ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_routes ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_routeaddresses ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_securityranges ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_dnsbl ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_fetchaccounts ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_fetchaccounts_uids ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_apppasswords ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_contacts ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_addressbooks ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_addressbookshares ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_accountprefs ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_scheduled ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_files ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_smimekeys ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_calendars ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_calendarobjects ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_calendarshares ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_senderaddresses ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_maskedaddresses ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_passkeys ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_directory ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_recoverable ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_confidential ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_dmarcreports ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_dmarcreportrecords ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_tlsreports ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_accountactivity ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_accountrecipients ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_accountreview ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_heldmessages ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_remotedomainpolicies ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_rules ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_rule_criterias ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_rule_actions ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_surblservers ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_greylisting_triplets ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_greylisting_whiteaddresses ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_greylisting_bypass ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_blocked_attachments ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_servermessages ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_tcpipports ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_whitelist ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_blocked_senders ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_sslcertificates ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_groups ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_group_members ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_acl ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_incoming_relays ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_logon_failures ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_imap_metadata ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_quarantine ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_senderlist ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_digeststate ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_digesttokens ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_passwordhistory ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_messagetrace ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_messageindexterms ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_messageindexstate ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_metricsamples ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_archiveindex ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

-- A table with a column compared exactly (utf8mb4_bin) is converted column by column: CONVERT TO
-- would set that column to utf8mb4_general_ci as well, and its unique index, made case- and
-- accent-insensitive, would refuse rows that differ only so. Every text column is named.
ALTER TABLE hm_knownsenders ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci, MODIFY ksaddress varchar(255) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NOT NULL, MODIFY ksfirstseen varchar(32) CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci NOT NULL, MODIFY kslastseen varchar(32) CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci NOT NULL;

ALTER TABLE hm_autoreplies ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci, MODIFY arsender varchar(255) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NOT NULL;

ALTER TABLE hm_domainsamples ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci, MODIFY dsday varchar(10) CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci NOT NULL, MODIFY dsdomain varchar(255) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NOT NULL;

ALTER TABLE hm_dkimrotations ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_spamtraps ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_spamtraphits ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_audit ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_alertrules ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_alertevents ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_reputationchecks ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_roles ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_roleassignments ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_signins ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_bouncesuppressions ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_bouncecounts ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_bouncereportkeys ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_pgpkeys ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_pgpring ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_protectedmessages ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_zeroaccess ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_recall ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_webhooks ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_webhookqueue ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_pushsubscriptions ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_complaints ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_complaintvolume ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_mailboxmoves ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_domainpolicylist ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_journalrules ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_accountrecovery ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_recoverytokens ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_recoverycodes ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_settingoverrides ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_apisends ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_migrations ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_migrationjobs ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_migrationfolders ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_sandboxverdicts ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_sandboxholds ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_listarchive ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_warmup ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_warmupcounts ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_smimeca ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_smimeissued ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_discoverycases ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_discoverysearches ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_discoveryholds ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_discoveryexports ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

-- The tables the steps folded beside this one made - 6099 (encryption at rest), 6100 (inbound
-- webhooks) and 6101 (rooms and equipment). The rooms tables were made utf8mb4 already; the
-- statement is then a rebuild that changes nothing.
ALTER TABLE hm_accountkeys ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_domainrecovery ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_inboundhooks ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_inboundhookqueue ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_bookingpolicies ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_bookinganswers ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci, MODIFY baorganiser varchar(255) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NOT NULL;

ALTER TABLE hm_bookingqueue ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

ALTER TABLE hm_bookingdecisions ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci, MODIFY bduid varchar(255) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NOT NULL, MODIFY bdorganiser varchar(255) CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci NOT NULL;

-- hm_log is the SQL log device's table, which the server creates the first time that device is
-- chosen (SqlLogDevice.cpp), so it may not be here.
SET @hm_convert_log = (SELECT IF(COUNT(*) = 0, 'DO 0', 'ALTER TABLE hm_log ENGINE=InnoDB, ROW_FORMAT=DYNAMIC, CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci') FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name = 'hm_log');

PREPARE hm_convert_log FROM @hm_convert_log;

EXECUTE hm_convert_log;

DEALLOCATE PREPARE hm_convert_log;

update hm_dbversion set value = 6102;
