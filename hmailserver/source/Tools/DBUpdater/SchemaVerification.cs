// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Collections.Generic;
using System.Linq;

namespace DBUpdater
{
   /// <summary>
   /// One statement that proves a schema object an upgrade step was supposed to
   /// create is really there.
   /// </summary>
   internal sealed class SchemaProbe
   {
      public SchemaProbe(int version, string describes, string statement)
      {
         Version = version;
         Describes = describes;
         Statement = statement;
      }

      /// <summary>The database version this probe proves has been reached.</summary>
      public int Version { get; private set; }

      /// <summary>Human-readable name of the object, e.g. "hm_imapfolders.folderspecialuse".</summary>
      public string Describes { get; private set; }

      public string Statement { get; private set; }
   }

   /// <summary>
   /// Proof that an upgrade step changed the schema, rather than merely not
   /// throwing.
   ///
   /// Why this exists. Three of the four dialects mark their ALTER TABLE
   /// statements with the DAL's [IGNORE-ERRORS] marker so that re-running a step
   /// against an already-upgraded database is harmless. But the marker is matched
   /// anywhere in the statement text and then discards *every* error, not just
   /// "column already exists" - see MySQLConnection::TryExecute
   /// ("bool bIgnoreErrors = SQL.Find(_T(\"[IGNORE-ERRORS]\")) >= 0"),
   /// ADOConnection::Execute and SQLCEConnection::Execute, which all do the same
   /// thing. The "update hm_dbversion set value = N" statement that follows in
   /// the same script carries no marker, so it always runs.
   ///
   /// The consequence is the worst shape a schema upgrade has: the ALTER fails
   /// for a real reason - insufficient rights, a row-size limit, a syntax form
   /// the backend rejects - the failure is discarded, hm_dbversion is stamped
   /// with the new version anyway, and DBUpdater reports success. The server then
   /// starts happily, because the version it requires is the version it finds,
   /// and IMAPFolders::LoadFolders - which selects folderspecialuse by name -
   /// fails for every account on the system. Nothing anywhere says why.
   ///
   /// A probe is a statement that cannot fail for any reason other than the
   /// object being absent, and costs nothing to run:
   /// "update <table> set <column> = <column> where 1 = 0". All four backends
   /// resolve column names when they prepare a statement, so an empty table
   /// proves a column exists just as well as a populated one, and "where 1 = 0"
   /// matches nothing - the statement writes no rows on any backend.
   ///
   /// It is deliberately an UPDATE rather than the more obvious
   /// "select <column> from <table> where 1 = 0". Both prove the same thing, but
   /// InterfaceDatabase::ExecuteSQLWithReturn always passes a non-null insert-id
   /// pointer, and on a statement that returns tuples that sends
   /// PGConnection::TryExecute into "PQgetvalue(pResult, 0, 0)" against an empty
   /// result - an out-of-range read libpq answers with NULL and an internal
   /// notice, harmless but not something a verification path should be the first
   /// caller to reach routinely. An UPDATE returns a command status instead, which
   /// is the same path every upgrade script already takes. DBUpdater holds rights
   /// to ALTER the table a line earlier, so it certainly holds rights to update
   /// nothing in it.
   ///
   /// Registering a probe is not optional for a new upgrade step whose script
   /// carries [IGNORE-ERRORS]: build\check-schema-versions.ps1 fails the release
   /// if a marked script has no probe here.
   /// </summary>
   internal static class SchemaVerification
   {
      private static readonly SchemaProbe[] Probes =
      {
         // Upgrade5001to5002{MSSQL,MySQL,MSSQLCE}.sql - marked [IGNORE-ERRORS].
         new SchemaProbe(5002, "hm_rule_actions.actionrouteid",
                         "update hm_rule_actions set actionrouteid = actionrouteid where 1 = 0"),

         // Upgrade6005to6006{MSSQL,MySQL,MSSQLCE}.sql - marked [IGNORE-ERRORS].
         // The PGSQL script for this step uses "add column if not exists" and so
         // reports genuine failures on its own; the probe covers it regardless,
         // because a probe that only runs on some backends is a probe that gets
         // out of step with the scripts.
         new SchemaProbe(6006, "hm_imapfolders.folderspecialuse",
                         "update hm_imapfolders set folderspecialuse = folderspecialuse where 1 = 0"),

         // Upgrade6006to6007* - two columns, and therefore two probes rather than one.
         // SQL CE commits each ALTER implicitly and cannot roll back, so this step can
         // half-apply: the first column lands, the second fails, and the version row is
         // never reached. A single probe on either column alone would then either miss
         // the failure or misreport a database that is genuinely fine.
         new SchemaProbe(6007, "hm_domains.domaindkimsecondaryselector",
                         "update hm_domains set domaindkimsecondaryselector = domaindkimsecondaryselector where 1 = 0"),
         new SchemaProbe(6007, "hm_domains.domaindkimsecondaryprivatekeyfile",
                         "update hm_domains set domaindkimsecondaryprivatekeyfile = domaindkimsecondaryprivatekeyfile where 1 = 0"),

         // Upgrade6007to6008* - two columns again, so two probes, for the same
         // half-apply reason as the step above.
         new SchemaProbe(6008, "hm_tcpipports.portclientcertificatepolicy",
                         "update hm_tcpipports set portclientcertificatepolicy = portclientcertificatepolicy where 1 = 0"),
         new SchemaProbe(6008, "hm_tcpipports.portclientcertificatecafile",
                         "update hm_tcpipports set portclientcertificatecafile = portclientcertificatecafile where 1 = 0"),

         // Upgrade6008to6009* - one column, so one probe. The half-apply hazard that
         // forced two probes on the steps above cannot arise with a single ALTER, but the
         // probe is still needed: the MSSQL/CE/MySQL scripts carry [IGNORE-ERRORS], so
         // without it a failed ALTER would be swallowed and the version stamped anyway.
         new SchemaProbe(6009, "hm_sslcertificates.sslprivatekeypassword",
                         "update hm_sslcertificates set sslprivatekeypassword = sslprivatekeypassword where 1 = 0"),

         // Upgrade6010to6011* - a whole new table, hm_inisettings, which holds the
         // [Settings] section of hMailServer.INI so that it reaches backups and can be
         // administered remotely.
         //
         // One probe per COLUMN even though a CREATE TABLE cannot half-apply the way a
         // multi-column ALTER can. Two reasons it is still right here. The step is not
         // one statement: on MSSQL and SQL CE the table is created and then two
         // constraints are added by separate ALTERs, and SQL CE commits each of those as
         // it executes, so "the table exists" does not mean "the step finished". And a
         // probe naming a column is what rule 7 of check-schema-versions.ps1 uses to
         // confirm the same column exists in all three CreateTables scripts - which is
         // the check that catches a new table added to the upgrade path but forgotten in
         // the fresh-install path, leaving new installations without it.
         //
         // There is deliberately NO probe for inisettingid. Every probe is an UPDATE of
         // a column to itself, and inisettingid is an identity column: SQL Server and
         // SQL Server Compact reject "cannot update identity column" when the statement
         // is compiled, before "where 1 = 0" is ever evaluated. The probe would fail on
         // a perfectly correct database, VerifyUpgradedSchema would read that as the
         // step having failed, and the upgrade would roll back - on the two backends
         // most installations use. Every pre-existing probe names a plain data column
         // for the same reason.
         new SchemaProbe(6011, "hm_inisettings.inisettingname",
                         "update hm_inisettings set inisettingname = inisettingname where 1 = 0"),
         new SchemaProbe(6011, "hm_inisettings.inisettingvalue",
                         "update hm_inisettings set inisettingvalue = inisettingvalue where 1 = 0"),
         new SchemaProbe(6011, "hm_inisettings.inisettingfilevalue",
                         "update hm_inisettings set inisettingfilevalue = inisettingfilevalue where 1 = 0"),

         // Upgrade6011to6012* - the rule-criteria value column widened to 2000
         // (probing the column proves it exists, not its width; the width is the
         // upgrade script's to get right) and the vacation begin date on
         // accounts. One probe per column, as ever: SQL CE commits each ALTER
         // implicitly, so a two-column step can half-apply.
         new SchemaProbe(6012, "hm_rule_criterias.criteriamatchvalue",
                         "update hm_rule_criterias set criteriamatchvalue = criteriamatchvalue where 1 = 0"),
         new SchemaProbe(6012, "hm_accounts.accountvacationbegindate",
                         "update hm_accounts set accountvacationbegindate = accountvacationbegindate where 1 = 0"),

         // Upgrade6012to6013* - the message save date (RFC 8514 SAVEDATE): when
         // the message was saved into its current mailbox, distinct from
         // messagecreatetime, which IMAP COPY must preserve as INTERNALDATE.
         new SchemaProbe(6013, "hm_messages.messagesavedate",
                         "update hm_messages set messagesavedate = messagesavedate where 1 = 0"),

         // Upgrade6013to6014* - the RFC 5464 METADATA store: annotations on
         // mailboxes (accountid + folderid) and on the server (0 + 0).
         new SchemaProbe(6014, "hm_imap_metadata.metadatavalue",
                         "update hm_imap_metadata set metadatavalue = metadatavalue where 1 = 0"),

         // Upgrade6014to6015* - the RFC 8474 OBJECTID email id: stable for a
         // message across COPY, unlike the row id, which is why it is its own
         // column stamped at first save and carried by copies.
         new SchemaProbe(6015, "hm_messages.messageemailid",
                         "update hm_messages set messageemailid = messageemailid where 1 = 0"),

         // Upgrade6015to6016* - app passwords: a per-account credential that can
         // be revoked on its own, which is what makes per-account 2FA possible for
         // clients that have nowhere to type a code. One probe per column, because
         // SQL CE commits each statement implicitly and a multi-column step can
         // half-apply.
         new SchemaProbe(6016, "hm_apppasswords.aphash",
                         "update hm_apppasswords set aphash = aphash where 1 = 0"),
         new SchemaProbe(6016, "hm_apppasswords.aplastused",
                         "update hm_apppasswords set aplastused = aplastused where 1 = 0"),
         new SchemaProbe(6016, "hm_apppasswords.apactive",
                         "update hm_apppasswords set apactive = apactive where 1 = 0"),

         // Upgrade6016to6017* - the per-account TOTP secret. Empty means the account
         // has no second factor, which is every account until an administrator enrols
         // one, so the column is the whole feature's on/off switch.
         new SchemaProbe(6017, "hm_accounts.accounttotpsecret",
                         "update hm_accounts set accounttotpsecret = accounttotpsecret where 1 = 0"),

         // Upgrade6017to6018* - the quarantine store. One probe per column that the
         // server writes, because SQL CE commits each statement implicitly and a
         // multi-column step can half-apply.
         new SchemaProbe(6018, "hm_quarantine.quarantinefilename",
                         "update hm_quarantine set quarantinefilename = quarantinefilename where 1 = 0"),
         new SchemaProbe(6018, "hm_quarantine.quarantinerecipients",
                         "update hm_quarantine set quarantinerecipients = quarantinerecipients where 1 = 0"),
         new SchemaProbe(6018, "hm_quarantine.quarantinecreated",
                         "update hm_quarantine set quarantinecreated = quarantinecreated where 1 = 0"),

         // Upgrade6018to6019* - password age and reuse history. The column is on an
         // existing table and the history is a new one, so a half-applied step is
         // exactly the shape these probes exist to catch.
         new SchemaProbe(6019, "hm_accounts.accountpasswordchanged",
                         "update hm_accounts set accountpasswordchanged = accountpasswordchanged where 1 = 0"),
         new SchemaProbe(6019, "hm_passwordhistory.phhash",
                         "update hm_passwordhistory set phhash = phhash where 1 = 0"),
         new SchemaProbe(6019, "hm_passwordhistory.phchanged",
                         "update hm_passwordhistory set phchanged = phchanged where 1 = 0"),

         // Upgrade6019to6020* - the message trace.
         new SchemaProbe(6020, "hm_messagetrace.mtqueueid",
                         "update hm_messagetrace set mtqueueid = mtqueueid where 1 = 0"),
         new SchemaProbe(6020, "hm_messagetrace.mtevent",
                         "update hm_messagetrace set mtevent = mtevent where 1 = 0"),
         new SchemaProbe(6020, "hm_messagetrace.mtoccurred",
                         "update hm_messagetrace set mtoccurred = mtoccurred where 1 = 0"),

         // Upgrade6020to6021* - the per-domain outbound relay. One probe per column,
         // because a partially-applied ALTER leaves a database that opens fine and
         // then fails on the one domain somebody configured.
         new SchemaProbe(6021, "hm_domains.domainrelayhost",
                         "update hm_domains set domainrelayhost = domainrelayhost where 1 = 0"),

         new SchemaProbe(6021, "hm_domains.domainrelayport",
                         "update hm_domains set domainrelayport = domainrelayport where 1 = 0"),

         new SchemaProbe(6021, "hm_domains.domainrelayrequiresauth",
                         "update hm_domains set domainrelayrequiresauth = domainrelayrequiresauth where 1 = 0"),

         new SchemaProbe(6021, "hm_domains.domainrelayusername",
                         "update hm_domains set domainrelayusername = domainrelayusername where 1 = 0"),

         new SchemaProbe(6021, "hm_domains.domainrelaypassword",
                         "update hm_domains set domainrelaypassword = domainrelaypassword where 1 = 0"),

         new SchemaProbe(6021, "hm_domains.domainrelayconnectionsecurity",
                         "update hm_domains set domainrelayconnectionsecurity = domainrelayconnectionsecurity where 1 = 0"),

         // Upgrade6021to6022* - the quota warning notice. This step inserts ROWS
         // rather than altering a table, so the probe asks whether the row is there:
         // a step that half-ran leaves a server that starts fine and then reports
         // "Server message 'QUOTA_WARNING' could not be found" to the one user whose
         // mailbox filled up.
         new SchemaProbe(6022, "hm_servermessages.QUOTA_WARNING",
                         "update hm_servermessages set smtext = smtext where smname = 'QUOTA_WARNING' and 1 = 0"),

         // Upgrade6022to6023* - the full-text index. One probe per column across
         // both tables: a half-applied step here leaves a server that starts, indexes
         // happily into a table missing the column the search then reads, and answers
         // SEARCH with silence rather than an error - which is the worst shape a
         // failure can take, because nobody reports mail they cannot find.
         new SchemaProbe(6023, "hm_messageindexterms.mitmessageid",
                         "update hm_messageindexterms set mitmessageid = mitmessageid where 1 = 0"),

         new SchemaProbe(6023, "hm_messageindexterms.mitaccountid",
                         "update hm_messageindexterms set mitaccountid = mitaccountid where 1 = 0"),

         new SchemaProbe(6023, "hm_messageindexterms.mitterm",
                         "update hm_messageindexterms set mitterm = mitterm where 1 = 0"),

         new SchemaProbe(6023, "hm_messageindexstate.misaccountid",
                         "update hm_messageindexstate set misaccountid = misaccountid where 1 = 0"),

         new SchemaProbe(6023, "hm_messageindexstate.mishighwatermark",
                         "update hm_messageindexstate set mishighwatermark = mishighwatermark where 1 = 0"),

         // Upgrade6023to6024* - the domain-wide out-of-office reply and the blocked
         // sender list. Two features in one step because they landed together, and
         // both fail quietly if half-applied: a missing vacation column means the
         // domain reads back as "no auto-reply" and simply never answers, and a
         // missing blocked-senders table means every listed sender is silently
         // delivered again. Neither raises anything an administrator would see, so
         // each column is probed rather than trusting the step's own success.
         new SchemaProbe(6024, "hm_domains.domainvacationmessageon",
                         "update hm_domains set domainvacationmessageon = domainvacationmessageon where 1 = 0"),
         new SchemaProbe(6024, "hm_domains.domainvacationsubject",
                         "update hm_domains set domainvacationsubject = domainvacationsubject where 1 = 0"),
         new SchemaProbe(6024, "hm_domains.domainvacationmessage",
                         "update hm_domains set domainvacationmessage = domainvacationmessage where 1 = 0"),
         new SchemaProbe(6024, "hm_domains.domainvacationinternalsubject",
                         "update hm_domains set domainvacationinternalsubject = domainvacationinternalsubject where 1 = 0"),
         new SchemaProbe(6024, "hm_domains.domainvacationinternalmessage",
                         "update hm_domains set domainvacationinternalmessage = domainvacationinternalmessage where 1 = 0"),
         new SchemaProbe(6024, "hm_domains.domainvacationexternaloverride",
                         "update hm_domains set domainvacationexternaloverride = domainvacationexternaloverride where 1 = 0"),
         new SchemaProbe(6024, "hm_blocked_senders.bsaddress",
                         "update hm_blocked_senders set bsaddress = bsaddress where 1 = 0"),
         new SchemaProbe(6024, "hm_blocked_senders.bsscore",
                         "update hm_blocked_senders set bsscore = bsscore where 1 = 0"),
         new SchemaProbe(6024, "hm_blocked_senders.bsdescription",
                         "update hm_blocked_senders set bsdescription = bsdescription where 1 = 0"),

         // Upgrade6024to6025* - per-account spam overrides, list moderation, and
         // the messageflags widening BINARYMIME's ninth flag bit needs. The
         // widening cannot be probed by a column-exists trick (the column was
         // always there); what CAN be probed is the five new columns, and a
         // half-applied step fails on one of them.
         new SchemaProbe(6025, "hm_accounts.accountantispamenabled",
                         "update hm_accounts set accountantispamenabled = accountantispamenabled where 1 = 0"),
         new SchemaProbe(6025, "hm_accounts.accountspammarkthreshold",
                         "update hm_accounts set accountspammarkthreshold = accountspammarkthreshold where 1 = 0"),
         new SchemaProbe(6025, "hm_accounts.accountspamdeletethreshold",
                         "update hm_accounts set accountspamdeletethreshold = accountspamdeletethreshold where 1 = 0"),
         new SchemaProbe(6025, "hm_distributionlists.distributionlistmoderatoraddress",
                         "update hm_distributionlists set distributionlistmoderatoraddress = distributionlistmoderatoraddress where 1 = 0"),
         new SchemaProbe(6025, "hm_distributionlists.distributionlistbounceaddress",
                         "update hm_distributionlists set distributionlistbounceaddress = distributionlistbounceaddress where 1 = 0"),

         // Upgrade6027to6028* - the metric history. One probe per column the
         // sampler writes: a half-applied step fails on the first missing one.
         new SchemaProbe(6028, "hm_metricsamples.metricsampletime",
                         "update hm_metricsamples set metricsampletime = metricsampletime where 1 = 0"),
         new SchemaProbe(6028, "hm_metricsamples.metricsamplename",
                         "update hm_metricsamples set metricsamplename = metricsamplename where 1 = 0"),
         new SchemaProbe(6028, "hm_metricsamples.metricsamplevalue",
                         "update hm_metricsamples set metricsamplevalue = metricsamplevalue where 1 = 0"),

         // Upgrade6028to6029* - the archive index. One probe per column the
         // archiver writes and the sweep reads.
         new SchemaProbe(6029, "hm_archiveindex.archivepath",
                         "update hm_archiveindex set archivepath = archivepath where 1 = 0"),
         new SchemaProbe(6029, "hm_archiveindex.archivehold",
                         "update hm_archiveindex set archivehold = archivehold where 1 = 0"),
         new SchemaProbe(6029, "hm_archiveindex.archivemessageid",
                         "update hm_archiveindex set archivemessageid = archivemessageid where 1 = 0"),

         // Upgrade6029to6030* - the foreign keys. A constraint cannot be proved by a
         // statement that writes nothing, since only a violation makes a database
         // complain, so these ask the catalogue through the one view all four
         // dialects share, and fail by dividing by zero when the constraint is
         // absent: the update touches hm_dbversion's one row only WHERE NOT EXISTS
         // the constraint, and "value / (value - value)" on that row is a division
         // by zero on every backend (SQL Server 8134, PostgreSQL 22012, MySQL 1365
         // in its default strict mode, SQL Server Compact "expression evaluation
         // caused an overflow"). When the constraint is present no row matches,
         // nothing is evaluated and nothing is written.
         //
         // The shape matters, and this is the second one. The first was
         // "set value = value / (case when exists (select 1 from ...) then 1 else 0 end)",
         // which is fine on three backends and takes the SQL Server Compact OLE DB
         // provider down with an access violation on the fourth - on a correct
         // database, with every constraint present. The server catches that in
         // SQLCEConnection::Execute's catch-all and reports it as HM10045 "Unknown
         // error", DBUpdater reads the failed probe as a missing constraint, and
         // every Compact Edition upgrade through 6030 failed after having, in fact,
         // succeeded (issue #114, 6.2.25). Compact Edition also rejects a scalar
         // subquery in a SET expression outright, so the count of matching rows
         // cannot be the divisor either; a subquery in the WHERE clause is the one
         // place it accepts one. Every probe is run against the bench database by
         // Infrastructure/SchemaProbes.cs and against a fresh database by
         // build\check-db-scripts.ps1, so a shape the provider cannot execute is
         // caught before a release rather than by the first upgrade in the field.
         new SchemaProbe(6030, "hm_accounts.fk_hm_accounts_domain",
                         "update hm_dbversion set value = value / (value - value) where not exists (select 1 from information_schema.table_constraints where constraint_name = 'fk_hm_accounts_domain' and constraint_type = 'FOREIGN KEY')"),
         new SchemaProbe(6030, "hm_distributionlistsrecipients.fk_hm_dlrecipients_list",
                         "update hm_dbversion set value = value / (value - value) where not exists (select 1 from information_schema.table_constraints where constraint_name = 'fk_hm_dlrecipients_list' and constraint_type = 'FOREIGN KEY')"),
         new SchemaProbe(6030, "hm_messagerecipients.fk_hm_messagerecipients_message",
                         "update hm_dbversion set value = value / (value - value) where not exists (select 1 from information_schema.table_constraints where constraint_name = 'fk_hm_messagerecipients_message' and constraint_type = 'FOREIGN KEY')"),
         new SchemaProbe(6030, "hm_messageindexterms.fk_hm_messageindexterms_message",
                         "update hm_dbversion set value = value / (value - value) where not exists (select 1 from information_schema.table_constraints where constraint_name = 'fk_hm_messageindexterms_message' and constraint_type = 'FOREIGN KEY')"),

         // Upgrade6030to6031* - the fetch account's mirror switch: every folder of a
         // remote IMAP account collected into a local folder of the same name.
         new SchemaProbe(6031, "hm_fetchaccounts.famirrorfolders",
                         "update hm_fetchaccounts set famirrorfolders = famirrorfolders where 1 = 0"),
         // Upgrade6031to6032* - the account's address book (hm_contacts): one
         // probe per column, since Compact Edition commits each statement as it
         // goes and a half-applied table must be seen as such. The identity
         // column is not probed: Compact Edition refuses to update one even
         // where 1 = 0, and the other columns cannot exist without it.
         new SchemaProbe(6032, "hm_contacts.contactaccountid",
                         "update hm_contacts set contactaccountid = contactaccountid where 1 = 0"),
         new SchemaProbe(6032, "hm_contacts.contactname",
                         "update hm_contacts set contactname = contactname where 1 = 0"),
         new SchemaProbe(6032, "hm_contacts.contactaddress",
                         "update hm_contacts set contactaddress = contactaddress where 1 = 0"),
         new SchemaProbe(6032, "hm_contacts.contactsource",
                         "update hm_contacts set contactsource = contactsource where 1 = 0"),
         new SchemaProbe(6032, "hm_contacts.contactcreated",
                         "update hm_contacts set contactcreated = contactcreated where 1 = 0"),
         // Upgrade6032to6033* - the account's preferences (hm_accountprefs), the
         // webmail's key/value store; the identity column is not probed.
         new SchemaProbe(6033, "hm_accountprefs.prefaccountid",
                         "update hm_accountprefs set prefaccountid = prefaccountid where 1 = 0"),
         new SchemaProbe(6033, "hm_accountprefs.prefname",
                         "update hm_accountprefs set prefname = prefname where 1 = 0"),
         new SchemaProbe(6033, "hm_accountprefs.prefvalue",
                         "update hm_accountprefs set prefvalue = prefvalue where 1 = 0"),
         // Upgrade6033to6034* - what is scheduled (hm_scheduled): a draft to be
         // sent at a time, a snoozed message to come back; the identity column
         // is not probed.
         new SchemaProbe(6034, "hm_scheduled.schedaccountid",
                         "update hm_scheduled set schedaccountid = schedaccountid where 1 = 0"),
         new SchemaProbe(6034, "hm_scheduled.schedmessageid",
                         "update hm_scheduled set schedmessageid = schedmessageid where 1 = 0"),
         new SchemaProbe(6034, "hm_scheduled.schedaction",
                         "update hm_scheduled set schedaction = schedaction where 1 = 0"),
         new SchemaProbe(6034, "hm_scheduled.schedat",
                         "update hm_scheduled set schedat = schedat where 1 = 0"),
         new SchemaProbe(6034, "hm_scheduled.schedfolderid",
                         "update hm_scheduled set schedfolderid = schedfolderid where 1 = 0"),
         new SchemaProbe(6034, "hm_scheduled.schedcreated",
                         "update hm_scheduled set schedcreated = schedcreated where 1 = 0"),
         // Upgrade6034to6035* - files sent as links (hm_files); the identity
         // column is not probed.
         new SchemaProbe(6035, "hm_files.fileaccountid",
                         "update hm_files set fileaccountid = fileaccountid where 1 = 0"),
         new SchemaProbe(6035, "hm_files.filetoken",
                         "update hm_files set filetoken = filetoken where 1 = 0"),
         new SchemaProbe(6035, "hm_files.filename",
                         "update hm_files set filename = filename where 1 = 0"),
         new SchemaProbe(6035, "hm_files.filetype",
                         "update hm_files set filetype = filetype where 1 = 0"),
         new SchemaProbe(6035, "hm_files.filesize",
                         "update hm_files set filesize = filesize where 1 = 0"),
         new SchemaProbe(6035, "hm_files.filestored",
                         "update hm_files set filestored = filestored where 1 = 0"),
         new SchemaProbe(6035, "hm_files.filecomplete",
                         "update hm_files set filecomplete = filecomplete where 1 = 0"),
         new SchemaProbe(6035, "hm_files.filecreated",
                         "update hm_files set filecreated = filecreated where 1 = 0"),
         new SchemaProbe(6035, "hm_files.fileexpires",
                         "update hm_files set fileexpires = fileexpires where 1 = 0"),
         new SchemaProbe(6035, "hm_files.filepasswordhash",
                         "update hm_files set filepasswordhash = filepasswordhash where 1 = 0"),
         new SchemaProbe(6035, "hm_files.filedownloads",
                         "update hm_files set filedownloads = filedownloads where 1 = 0"),
         // Upgrade6035to6036* - IMAP keywords (labels) on a message.
         new SchemaProbe(6036, "hm_messages.messagekeywords",
                         "update hm_messages set messagekeywords = messagekeywords where 1 = 0"),
         // Upgrade6036to6037* - the webmail's S/MIME key store (hm_smimekeys); the
         // identity column is never probed.
         new SchemaProbe(6037, "hm_smimekeys.smimeaccountid",
                         "update hm_smimekeys set smimeaccountid = smimeaccountid where 1 = 0"),
         new SchemaProbe(6037, "hm_smimekeys.smimekind",
                         "update hm_smimekeys set smimekind = smimekind where 1 = 0"),
         new SchemaProbe(6037, "hm_smimekeys.smimeaddress",
                         "update hm_smimekeys set smimeaddress = smimeaddress where 1 = 0"),
         new SchemaProbe(6037, "hm_smimekeys.smimename",
                         "update hm_smimekeys set smimename = smimename where 1 = 0"),
         new SchemaProbe(6037, "hm_smimekeys.smimefingerprint",
                         "update hm_smimekeys set smimefingerprint = smimefingerprint where 1 = 0"),
         new SchemaProbe(6037, "hm_smimekeys.smimecertificate",
                         "update hm_smimekeys set smimecertificate = smimecertificate where 1 = 0"),
         new SchemaProbe(6037, "hm_smimekeys.smimechain",
                         "update hm_smimekeys set smimechain = smimechain where 1 = 0"),
         new SchemaProbe(6037, "hm_smimekeys.smimekey",
                         "update hm_smimekeys set smimekey = smimekey where 1 = 0"),
         new SchemaProbe(6037, "hm_smimekeys.smimenotafter",
                         "update hm_smimekeys set smimenotafter = smimenotafter where 1 = 0"),
         new SchemaProbe(6037, "hm_smimekeys.smimecreated",
                         "update hm_smimekeys set smimecreated = smimecreated where 1 = 0"),
         // Upgrade6037to6038* - a SURBL server's expected result, judged like a DNSBL's.
         new SchemaProbe(6038, "hm_surblservers.surblresult",
                         "update hm_surblservers set surblresult = surblresult where 1 = 0"),
         // Upgrade6038to6039* - the domain relay password widened to 1024, the width
         // the route, fetch-account and certificate secrets have had since 6004: a
         // DPAPI envelope for an eleven-character password is 314 characters, and
         // the column was 255 (probing proves the column exists, not its width).
         new SchemaProbe(6039, "hm_domains.domainrelaypassword",
                         "update hm_domains set domainrelaypassword = domainrelaypassword where 1 = 0"),
         // Upgrade6039to6040* - a contact's CardDAV resource name, its UID, and the
         // card as the client sent it, so a sync round-trips the whole card.
         new SchemaProbe(6040, "hm_contacts.contacturi",
                         "update hm_contacts set contacturi = contacturi where 1 = 0"),
         new SchemaProbe(6040, "hm_contacts.contactuid",
                         "update hm_contacts set contactuid = contactuid where 1 = 0"),
         new SchemaProbe(6040, "hm_contacts.contactvcard",
                         "update hm_contacts set contactvcard = contactvcard where 1 = 0"),
         // Upgrade6040to6041* - the CalDAV calendars (hm_calendars) and their
         // objects (hm_calendarobjects); the identity columns are never probed.
         new SchemaProbe(6041, "hm_calendars.calendaraccountid",
                         "update hm_calendars set calendaraccountid = calendaraccountid where 1 = 0"),
         new SchemaProbe(6041, "hm_calendars.calendarname",
                         "update hm_calendars set calendarname = calendarname where 1 = 0"),
         new SchemaProbe(6041, "hm_calendars.calendardisplayname",
                         "update hm_calendars set calendardisplayname = calendardisplayname where 1 = 0"),
         new SchemaProbe(6041, "hm_calendars.calendarsynctoken",
                         "update hm_calendars set calendarsynctoken = calendarsynctoken where 1 = 0"),
         new SchemaProbe(6041, "hm_calendars.calendarcreated",
                         "update hm_calendars set calendarcreated = calendarcreated where 1 = 0"),
         new SchemaProbe(6041, "hm_calendarobjects.objectaccountid",
                         "update hm_calendarobjects set objectaccountid = objectaccountid where 1 = 0"),
         new SchemaProbe(6041, "hm_calendarobjects.objectcalendarid",
                         "update hm_calendarobjects set objectcalendarid = objectcalendarid where 1 = 0"),
         new SchemaProbe(6041, "hm_calendarobjects.objecturi",
                         "update hm_calendarobjects set objecturi = objecturi where 1 = 0"),
         new SchemaProbe(6041, "hm_calendarobjects.objectuid",
                         "update hm_calendarobjects set objectuid = objectuid where 1 = 0"),
         new SchemaProbe(6041, "hm_calendarobjects.objectcomponent",
                         "update hm_calendarobjects set objectcomponent = objectcomponent where 1 = 0"),
         new SchemaProbe(6041, "hm_calendarobjects.objectdata",
                         "update hm_calendarobjects set objectdata = objectdata where 1 = 0"),
         new SchemaProbe(6041, "hm_calendarobjects.objectetag",
                         "update hm_calendarobjects set objectetag = objectetag where 1 = 0"),
         new SchemaProbe(6041, "hm_calendarobjects.objectsynctoken",
                         "update hm_calendarobjects set objectsynctoken = objectsynctoken where 1 = 0"),
         new SchemaProbe(6041, "hm_calendarobjects.objectstart",
                         "update hm_calendarobjects set objectstart = objectstart where 1 = 0"),
         new SchemaProbe(6041, "hm_calendarobjects.objectend",
                         "update hm_calendarobjects set objectend = objectend where 1 = 0"),
         new SchemaProbe(6041, "hm_calendarobjects.objectfirst",
                         "update hm_calendarobjects set objectfirst = objectfirst where 1 = 0"),
         new SchemaProbe(6041, "hm_calendarobjects.objectlast",
                         "update hm_calendarobjects set objectlast = objectlast where 1 = 0"),
         new SchemaProbe(6041, "hm_calendarobjects.objectdeleted",
                         "update hm_calendarobjects set objectdeleted = objectdeleted where 1 = 0"),
         new SchemaProbe(6041, "hm_calendarobjects.objectmodified",
                         "update hm_calendarobjects set objectmodified = objectmodified where 1 = 0"),

         // Upgrade6042to6043* - the audit trail (hm_audit), the alert rules
         // (hm_alertrules) and the alert events (hm_alertevents). The two text
         // columns, auditdetail and alerteventdetail, are ntext/mediumtext/text:
         // SQL Server refuses "set col = col" on one, so they are proved by the
         // rest of their own table being there.
         new SchemaProbe(6043, "hm_audit.audittime",
                         "update hm_audit set audittime = audittime where 1 = 0"),
         new SchemaProbe(6043, "hm_audit.auditactor",
                         "update hm_audit set auditactor = auditactor where 1 = 0"),
         new SchemaProbe(6043, "hm_audit.auditactortype",
                         "update hm_audit set auditactortype = auditactortype where 1 = 0"),
         new SchemaProbe(6043, "hm_audit.auditinterface",
                         "update hm_audit set auditinterface = auditinterface where 1 = 0"),
         new SchemaProbe(6043, "hm_audit.auditaddress",
                         "update hm_audit set auditaddress = auditaddress where 1 = 0"),
         new SchemaProbe(6043, "hm_audit.auditobjecttype",
                         "update hm_audit set auditobjecttype = auditobjecttype where 1 = 0"),
         new SchemaProbe(6043, "hm_audit.auditobjectname",
                         "update hm_audit set auditobjectname = auditobjectname where 1 = 0"),
         new SchemaProbe(6043, "hm_audit.auditaction",
                         "update hm_audit set auditaction = auditaction where 1 = 0"),
         new SchemaProbe(6043, "hm_audit.auditprevhash",
                         "update hm_audit set auditprevhash = auditprevhash where 1 = 0"),
         new SchemaProbe(6043, "hm_audit.audithash",
                         "update hm_audit set audithash = audithash where 1 = 0"),
         new SchemaProbe(6043, "hm_alertrules.alertrulecondition",
                         "update hm_alertrules set alertrulecondition = alertrulecondition where 1 = 0"),
         new SchemaProbe(6043, "hm_alertrules.alertruleenabled",
                         "update hm_alertrules set alertruleenabled = alertruleenabled where 1 = 0"),
         new SchemaProbe(6043, "hm_alertrules.alertrulethreshold",
                         "update hm_alertrules set alertrulethreshold = alertrulethreshold where 1 = 0"),
         new SchemaProbe(6043, "hm_alertrules.alertruleactions",
                         "update hm_alertrules set alertruleactions = alertruleactions where 1 = 0"),
         new SchemaProbe(6043, "hm_alertrules.alertruleaddress",
                         "update hm_alertrules set alertruleaddress = alertruleaddress where 1 = 0"),
         new SchemaProbe(6043, "hm_alertrules.alertrulewebhook",
                         "update hm_alertrules set alertrulewebhook = alertrulewebhook where 1 = 0"),
         new SchemaProbe(6043, "hm_alertrules.alertrulesecret",
                         "update hm_alertrules set alertrulesecret = alertrulesecret where 1 = 0"),
         new SchemaProbe(6043, "hm_alertrules.alertrulecooldown",
                         "update hm_alertrules set alertrulecooldown = alertrulecooldown where 1 = 0"),
         new SchemaProbe(6043, "hm_alertrules.alertruledigest",
                         "update hm_alertrules set alertruledigest = alertruledigest where 1 = 0"),
         new SchemaProbe(6043, "hm_alertevents.alerteventcondition",
                         "update hm_alertevents set alerteventcondition = alerteventcondition where 1 = 0"),
         new SchemaProbe(6043, "hm_alertevents.alerteventseverity",
                         "update hm_alertevents set alerteventseverity = alerteventseverity where 1 = 0"),
         new SchemaProbe(6043, "hm_alertevents.alerteventstate",
                         "update hm_alertevents set alerteventstate = alerteventstate where 1 = 0"),
         new SchemaProbe(6043, "hm_alertevents.alerteventtime",
                         "update hm_alertevents set alerteventtime = alerteventtime where 1 = 0"),
         new SchemaProbe(6043, "hm_alertevents.alerteventsummary",
                         "update hm_alertevents set alerteventsummary = alerteventsummary where 1 = 0"),
         new SchemaProbe(6043, "hm_alertevents.alerteventnotified",
                         "update hm_alertevents set alerteventnotified = alerteventnotified where 1 = 0"),
         new SchemaProbe(6043, "hm_alertevents.alerteventdigested",
                         "update hm_alertevents set alerteventdigested = alerteventdigested where 1 = 0"),
         new SchemaProbe(6043, "hm_alertevents.alerteventhookstate",
                         "update hm_alertevents set alerteventhookstate = alerteventhookstate where 1 = 0"),
         new SchemaProbe(6043, "hm_alertevents.alerteventhooktries",
                         "update hm_alertevents set alerteventhooktries = alerteventhooktries where 1 = 0"),
         new SchemaProbe(6043, "hm_alertevents.alerteventhooknext",
                         "update hm_alertevents set alerteventhooknext = alerteventhooknext where 1 = 0"),
         // Upgrade6043to6044* - what a domain does to a message: the
         // external-sender tag, the first-contact note and the disclaimer, and
         // the memory the note is answered from (hm_knownsenders). The identity
         // column is never probed.
         new SchemaProbe(6044, "hm_domains.domainexternaltagsubject",
                         "update hm_domains set domainexternaltagsubject = domainexternaltagsubject where 1 = 0"),
         new SchemaProbe(6044, "hm_domains.domainexternaltagheader",
                         "update hm_domains set domainexternaltagheader = domainexternaltagheader where 1 = 0"),
         new SchemaProbe(6044, "hm_domains.domainexternaltagtext",
                         "update hm_domains set domainexternaltagtext = domainexternaltagtext where 1 = 0"),
         new SchemaProbe(6044, "hm_domains.domainfirstcontacttip",
                         "update hm_domains set domainfirstcontacttip = domainfirstcontacttip where 1 = 0"),
         new SchemaProbe(6044, "hm_domains.domaindisclaimerenabled",
                         "update hm_domains set domaindisclaimerenabled = domaindisclaimerenabled where 1 = 0"),
         new SchemaProbe(6044, "hm_domains.domaindisclaimerplaintext",
                         "update hm_domains set domaindisclaimerplaintext = domaindisclaimerplaintext where 1 = 0"),
         new SchemaProbe(6044, "hm_domains.domaindisclaimerhtml",
                         "update hm_domains set domaindisclaimerhtml = domaindisclaimerhtml where 1 = 0"),
         new SchemaProbe(6044, "hm_knownsenders.ksaccountid",
                         "update hm_knownsenders set ksaccountid = ksaccountid where 1 = 0"),
         new SchemaProbe(6044, "hm_knownsenders.ksaddress",
                         "update hm_knownsenders set ksaddress = ksaddress where 1 = 0"),
         new SchemaProbe(6044, "hm_knownsenders.kscount",
                         "update hm_knownsenders set kscount = kscount where 1 = 0"),
         new SchemaProbe(6044, "hm_knownsenders.ksfirstseen",
                         "update hm_knownsenders set ksfirstseen = ksfirstseen where 1 = 0"),
         new SchemaProbe(6044, "hm_knownsenders.kslastseen",
                         "update hm_knownsenders set kslastseen = kslastseen where 1 = 0"),
         // Upgrade6044to6045* - what this server will do for one named remote
         // domain: the TLS it demands outbound and inbound, the size and
         // concurrency it will attempt, and the backup-MX recipient callout.
         // The identity column is never probed.
         new SchemaProbe(6045, "hm_remotedomainpolicies.policydomainname",
                         "update hm_remotedomainpolicies set policydomainname = policydomainname where 1 = 0"),
         new SchemaProbe(6045, "hm_remotedomainpolicies.policydescription",
                         "update hm_remotedomainpolicies set policydescription = policydescription where 1 = 0"),
         new SchemaProbe(6045, "hm_remotedomainpolicies.policyactive",
                         "update hm_remotedomainpolicies set policyactive = policyactive where 1 = 0"),
         new SchemaProbe(6045, "hm_remotedomainpolicies.policyoutboundtls",
                         "update hm_remotedomainpolicies set policyoutboundtls = policyoutboundtls where 1 = 0"),
         new SchemaProbe(6045, "hm_remotedomainpolicies.policyinboundtls",
                         "update hm_remotedomainpolicies set policyinboundtls = policyinboundtls where 1 = 0"),
         new SchemaProbe(6045, "hm_remotedomainpolicies.policymaxmessagesizekb",
                         "update hm_remotedomainpolicies set policymaxmessagesizekb = policymaxmessagesizekb where 1 = 0"),
         new SchemaProbe(6045, "hm_remotedomainpolicies.policymaxconnections",
                         "update hm_remotedomainpolicies set policymaxconnections = policymaxconnections where 1 = 0"),
         new SchemaProbe(6045, "hm_remotedomainpolicies.policymaxperminute",
                         "update hm_remotedomainpolicies set policymaxperminute = policymaxperminute where 1 = 0"),
         new SchemaProbe(6045, "hm_remotedomainpolicies.policyallowreplies",
                         "update hm_remotedomainpolicies set policyallowreplies = policyallowreplies where 1 = 0"),
         new SchemaProbe(6045, "hm_remotedomainpolicies.policyallowforwarding",
                         "update hm_remotedomainpolicies set policyallowforwarding = policyallowforwarding where 1 = 0"),
         new SchemaProbe(6045, "hm_remotedomainpolicies.policycalloutenabled",
                         "update hm_remotedomainpolicies set policycalloutenabled = policycalloutenabled where 1 = 0"),
         new SchemaProbe(6045, "hm_remotedomainpolicies.policycallouthost",
                         "update hm_remotedomainpolicies set policycallouthost = policycallouthost where 1 = 0"),
         new SchemaProbe(6045, "hm_remotedomainpolicies.policycalloutport",
                         "update hm_remotedomainpolicies set policycalloutport = policycalloutport where 1 = 0"),
         new SchemaProbe(6045, "hm_remotedomainpolicies.policycallouttimeout",
                         "update hm_remotedomainpolicies set policycallouttimeout = policycallouttimeout where 1 = 0"),
         new SchemaProbe(6045, "hm_remotedomainpolicies.policycalloutcacheminutes",
                         "update hm_remotedomainpolicies set policycalloutcacheminutes = policycalloutcacheminutes where 1 = 0"),
         new SchemaProbe(6045, "hm_remotedomainpolicies.policycalloutperminute",
                         "update hm_remotedomainpolicies set policycalloutperminute = policycalloutperminute where 1 = 0"),
         // Upgrade6045to6046* - the sender list (hm_senderlist), the quarantine
         // digest state and its one-shot links (hm_digeststate, hm_digesttokens),
         // and the two per-domain digest columns. The identity columns are never
         // probed.
         new SchemaProbe(6046, "hm_senderlist.slscope",
                         "update hm_senderlist set slscope = slscope where 1 = 0"),
         new SchemaProbe(6046, "hm_senderlist.slownerid",
                         "update hm_senderlist set slownerid = slownerid where 1 = 0"),
         new SchemaProbe(6046, "hm_senderlist.sltype",
                         "update hm_senderlist set sltype = sltype where 1 = 0"),
         new SchemaProbe(6046, "hm_senderlist.slvalue",
                         "update hm_senderlist set slvalue = slvalue where 1 = 0"),
         new SchemaProbe(6046, "hm_senderlist.slreason",
                         "update hm_senderlist set slreason = slreason where 1 = 0"),
         new SchemaProbe(6046, "hm_senderlist.slexpires",
                         "update hm_senderlist set slexpires = slexpires where 1 = 0"),
         new SchemaProbe(6046, "hm_senderlist.slexpirestime",
                         "update hm_senderlist set slexpirestime = slexpirestime where 1 = 0"),
         new SchemaProbe(6046, "hm_senderlist.slcreated",
                         "update hm_senderlist set slcreated = slcreated where 1 = 0"),
         new SchemaProbe(6046, "hm_digeststate.dsaccountid",
                         "update hm_digeststate set dsaccountid = dsaccountid where 1 = 0"),
         new SchemaProbe(6046, "hm_digeststate.dssenttime",
                         "update hm_digeststate set dssenttime = dssenttime where 1 = 0"),
         new SchemaProbe(6046, "hm_digeststate.dslastid",
                         "update hm_digeststate set dslastid = dslastid where 1 = 0"),
         new SchemaProbe(6046, "hm_digeststate.dsoptout",
                         "update hm_digeststate set dsoptout = dsoptout where 1 = 0"),
         new SchemaProbe(6046, "hm_digesttokens.dtaccountid",
                         "update hm_digesttokens set dtaccountid = dtaccountid where 1 = 0"),
         new SchemaProbe(6046, "hm_digesttokens.dtkind",
                         "update hm_digesttokens set dtkind = dtkind where 1 = 0"),
         new SchemaProbe(6046, "hm_digesttokens.dtquarantineid",
                         "update hm_digesttokens set dtquarantineid = dtquarantineid where 1 = 0"),
         new SchemaProbe(6046, "hm_digesttokens.dttoken",
                         "update hm_digesttokens set dttoken = dttoken where 1 = 0"),
         new SchemaProbe(6046, "hm_digesttokens.dtexpirestime",
                         "update hm_digesttokens set dtexpirestime = dtexpirestime where 1 = 0"),
         new SchemaProbe(6046, "hm_digesttokens.dtused",
                         "update hm_digesttokens set dtused = dtused where 1 = 0"),
         new SchemaProbe(6046, "hm_digesttokens.dtcreated",
                         "update hm_digesttokens set dtcreated = dtcreated where 1 = 0"),
         new SchemaProbe(6046, "hm_domains.domainquarantinedigest",
                         "update hm_domains set domainquarantinedigest = domainquarantinedigest where 1 = 0"),
         new SchemaProbe(6046, "hm_domains.domainquarantinedigesthour",
                         "update hm_domains set domainquarantinedigesthour = domainquarantinedigesthour where 1 = 0"),
         // Upgrade6046to6047* - the named roles (hm_roles) and who holds them
         // (hm_roleassignments). One probe per column, because Compact Edition
         // commits each statement of a step on its own and a half-applied step
         // is what this list exists to find.
         new SchemaProbe(6047, "hm_roles.rolename",
                         "update hm_roles set rolename = rolename where 1 = 0"),
         new SchemaProbe(6047, "hm_roles.roledescription",
                         "update hm_roles set roledescription = roledescription where 1 = 0"),
         new SchemaProbe(6047, "hm_roles.rolebuiltin",
                         "update hm_roles set rolebuiltin = rolebuiltin where 1 = 0"),
         new SchemaProbe(6047, "hm_roles.rolepermissions",
                         "update hm_roles set rolepermissions = rolepermissions where 1 = 0"),
         new SchemaProbe(6047, "hm_roleassignments.raaccountid",
                         "update hm_roleassignments set raaccountid = raaccountid where 1 = 0"),
         new SchemaProbe(6047, "hm_roleassignments.raroleid",
                         "update hm_roleassignments set raroleid = raroleid where 1 = 0"),
         new SchemaProbe(6047, "hm_roleassignments.radomains",
                         "update hm_roleassignments set radomains = radomains where 1 = 0"),
         // Upgrade6047to6048* - DKIM keys that rotate: the per-domain policy on
         // hm_domains and the state of each managed rotation (hm_dkimrotations).
         // The identity column is never probed.
         new SchemaProbe(6048, "hm_domains.domaindkimrotationenabled",
                         "update hm_domains set domaindkimrotationenabled = domaindkimrotationenabled where 1 = 0"),
         new SchemaProbe(6048, "hm_domains.domaindkimrotationintervaldays",
                         "update hm_domains set domaindkimrotationintervaldays = domaindkimrotationintervaldays where 1 = 0"),
         new SchemaProbe(6048, "hm_domains.domaindkimrotationpropagationhours",
                         "update hm_domains set domaindkimrotationpropagationhours = domaindkimrotationpropagationhours where 1 = 0"),
         new SchemaProbe(6048, "hm_domains.domaindkimrotationretentiondays",
                         "update hm_domains set domaindkimrotationretentiondays = domaindkimrotationretentiondays where 1 = 0"),
         new SchemaProbe(6048, "hm_domains.domaindkimrotationkeybits",
                         "update hm_domains set domaindkimrotationkeybits = domaindkimrotationkeybits where 1 = 0"),
         new SchemaProbe(6048, "hm_dkimrotations.dkrdomainid",
                         "update hm_dkimrotations set dkrdomainid = dkrdomainid where 1 = 0"),
         new SchemaProbe(6048, "hm_dkimrotations.dkrcurrentselector",
                         "update hm_dkimrotations set dkrcurrentselector = dkrcurrentselector where 1 = 0"),
         new SchemaProbe(6048, "hm_dkimrotations.dkrcurrentkeyfile",
                         "update hm_dkimrotations set dkrcurrentkeyfile = dkrcurrentkeyfile where 1 = 0"),
         new SchemaProbe(6048, "hm_dkimrotations.dkrcurrentgenerated",
                         "update hm_dkimrotations set dkrcurrentgenerated = dkrcurrentgenerated where 1 = 0"),
         new SchemaProbe(6048, "hm_dkimrotations.dkrcurrentsince",
                         "update hm_dkimrotations set dkrcurrentsince = dkrcurrentsince where 1 = 0"),
         new SchemaProbe(6048, "hm_dkimrotations.dkrcurrentstatus",
                         "update hm_dkimrotations set dkrcurrentstatus = dkrcurrentstatus where 1 = 0"),
         new SchemaProbe(6048, "hm_dkimrotations.dkrcurrentchecked",
                         "update hm_dkimrotations set dkrcurrentchecked = dkrcurrentchecked where 1 = 0"),
         new SchemaProbe(6048, "hm_dkimrotations.dkrcurrentdetail",
                         "update hm_dkimrotations set dkrcurrentdetail = dkrcurrentdetail where 1 = 0"),
         new SchemaProbe(6048, "hm_dkimrotations.dkrnextselector",
                         "update hm_dkimrotations set dkrnextselector = dkrnextselector where 1 = 0"),
         new SchemaProbe(6048, "hm_dkimrotations.dkrnextkeyfile",
                         "update hm_dkimrotations set dkrnextkeyfile = dkrnextkeyfile where 1 = 0"),
         new SchemaProbe(6048, "hm_dkimrotations.dkrnextgenerated",
                         "update hm_dkimrotations set dkrnextgenerated = dkrnextgenerated where 1 = 0"),
         new SchemaProbe(6048, "hm_dkimrotations.dkrnextcreated",
                         "update hm_dkimrotations set dkrnextcreated = dkrnextcreated where 1 = 0"),
         new SchemaProbe(6048, "hm_dkimrotations.dkrnextfirstseen",
                         "update hm_dkimrotations set dkrnextfirstseen = dkrnextfirstseen where 1 = 0"),
         new SchemaProbe(6048, "hm_dkimrotations.dkrnextstatus",
                         "update hm_dkimrotations set dkrnextstatus = dkrnextstatus where 1 = 0"),
         new SchemaProbe(6048, "hm_dkimrotations.dkrnextchecked",
                         "update hm_dkimrotations set dkrnextchecked = dkrnextchecked where 1 = 0"),
         new SchemaProbe(6048, "hm_dkimrotations.dkrnextdetail",
                         "update hm_dkimrotations set dkrnextdetail = dkrnextdetail where 1 = 0"),
         new SchemaProbe(6048, "hm_dkimrotations.dkrretiringselector",
                         "update hm_dkimrotations set dkrretiringselector = dkrretiringselector where 1 = 0"),
         new SchemaProbe(6048, "hm_dkimrotations.dkrretiringkeyfile",
                         "update hm_dkimrotations set dkrretiringkeyfile = dkrretiringkeyfile where 1 = 0"),
         new SchemaProbe(6048, "hm_dkimrotations.dkrretiringgenerated",
                         "update hm_dkimrotations set dkrretiringgenerated = dkrretiringgenerated where 1 = 0"),
         new SchemaProbe(6048, "hm_dkimrotations.dkrretiringsince",
                         "update hm_dkimrotations set dkrretiringsince = dkrretiringsince where 1 = 0"),
         new SchemaProbe(6048, "hm_dkimrotations.dkrretiringstatus",
                         "update hm_dkimrotations set dkrretiringstatus = dkrretiringstatus where 1 = 0"),
         new SchemaProbe(6048, "hm_dkimrotations.dkrretiringchecked",
                         "update hm_dkimrotations set dkrretiringchecked = dkrretiringchecked where 1 = 0"),
         new SchemaProbe(6048, "hm_dkimrotations.dkrretiringdetail",
                         "update hm_dkimrotations set dkrretiringdetail = dkrretiringdetail where 1 = 0"),
         new SchemaProbe(6048, "hm_dkimrotations.dkrretiredselector",
                         "update hm_dkimrotations set dkrretiredselector = dkrretiredselector where 1 = 0"),
         new SchemaProbe(6048, "hm_dkimrotations.dkrretiredat",
                         "update hm_dkimrotations set dkrretiredat = dkrretiredat where 1 = 0"),
         new SchemaProbe(6048, "hm_dkimrotations.dkrretiredkeyfile",
                         "update hm_dkimrotations set dkrretiredkeyfile = dkrretiredkeyfile where 1 = 0"),
         new SchemaProbe(6048, "hm_dkimrotations.dkrlasterror",
                         "update hm_dkimrotations set dkrlasterror = dkrlasterror where 1 = 0"),
         new SchemaProbe(6048, "hm_dkimrotations.dkrlasterrortime",
                         "update hm_dkimrotations set dkrlasterrortime = dkrlasterrortime where 1 = 0"),
         // Upgrade6048to6049* - bounces read, and a suppression list: the
         // per-domain list (hm_bouncesuppressions), the per-address counts
         // (hm_bouncecounts) and the register a delivery report is matched
         // against (hm_bouncereportkeys). The identity columns are never probed.
         new SchemaProbe(6049, "hm_bouncesuppressions.bsdomainid",
                         "update hm_bouncesuppressions set bsdomainid = bsdomainid where 1 = 0"),
         new SchemaProbe(6049, "hm_bouncesuppressions.bsaddress",
                         "update hm_bouncesuppressions set bsaddress = bsaddress where 1 = 0"),
         new SchemaProbe(6049, "hm_bouncesuppressions.bssource",
                         "update hm_bouncesuppressions set bssource = bssource where 1 = 0"),
         new SchemaProbe(6049, "hm_bouncesuppressions.bscategory",
                         "update hm_bouncesuppressions set bscategory = bscategory where 1 = 0"),
         new SchemaProbe(6049, "hm_bouncesuppressions.bsstatus",
                         "update hm_bouncesuppressions set bsstatus = bsstatus where 1 = 0"),
         new SchemaProbe(6049, "hm_bouncesuppressions.bsdiagnostic",
                         "update hm_bouncesuppressions set bsdiagnostic = bsdiagnostic where 1 = 0"),
         new SchemaProbe(6049, "hm_bouncesuppressions.bsremotemta",
                         "update hm_bouncesuppressions set bsremotemta = bsremotemta where 1 = 0"),
         new SchemaProbe(6049, "hm_bouncesuppressions.bscreated",
                         "update hm_bouncesuppressions set bscreated = bscreated where 1 = 0"),
         new SchemaProbe(6049, "hm_bouncesuppressions.bsexpires",
                         "update hm_bouncesuppressions set bsexpires = bsexpires where 1 = 0"),
         new SchemaProbe(6049, "hm_bouncecounts.bcdomainid",
                         "update hm_bouncecounts set bcdomainid = bcdomainid where 1 = 0"),
         new SchemaProbe(6049, "hm_bouncecounts.bcaddress",
                         "update hm_bouncecounts set bcaddress = bcaddress where 1 = 0"),
         new SchemaProbe(6049, "hm_bouncecounts.bchard",
                         "update hm_bouncecounts set bchard = bchard where 1 = 0"),
         new SchemaProbe(6049, "hm_bouncecounts.bcsoft",
                         "update hm_bouncecounts set bcsoft = bcsoft where 1 = 0"),
         new SchemaProbe(6049, "hm_bouncecounts.bcmailboxfull",
                         "update hm_bouncecounts set bcmailboxfull = bcmailboxfull where 1 = 0"),
         new SchemaProbe(6049, "hm_bouncecounts.bcpolicy",
                         "update hm_bouncecounts set bcpolicy = bcpolicy where 1 = 0"),
         new SchemaProbe(6049, "hm_bouncecounts.bcother",
                         "update hm_bouncecounts set bcother = bcother where 1 = 0"),
         new SchemaProbe(6049, "hm_bouncecounts.bcconsecutivehard",
                         "update hm_bouncecounts set bcconsecutivehard = bcconsecutivehard where 1 = 0"),
         new SchemaProbe(6049, "hm_bouncecounts.bclaststatus",
                         "update hm_bouncecounts set bclaststatus = bclaststatus where 1 = 0"),
         new SchemaProbe(6049, "hm_bouncecounts.bclastcategory",
                         "update hm_bouncecounts set bclastcategory = bclastcategory where 1 = 0"),
         new SchemaProbe(6049, "hm_bouncecounts.bclastbounce",
                         "update hm_bouncecounts set bclastbounce = bclastbounce where 1 = 0"),
         new SchemaProbe(6049, "hm_bouncecounts.bclastdelivered",
                         "update hm_bouncecounts set bclastdelivered = bclastdelivered where 1 = 0"),
         new SchemaProbe(6049, "hm_bouncereportkeys.brkdomainid",
                         "update hm_bouncereportkeys set brkdomainid = brkdomainid where 1 = 0"),
         new SchemaProbe(6049, "hm_bouncereportkeys.brkkey",
                         "update hm_bouncereportkeys set brkkey = brkkey where 1 = 0"),
         new SchemaProbe(6049, "hm_bouncereportkeys.brksent",
                         "update hm_bouncereportkeys set brksent = brksent where 1 = 0"),
         new SchemaProbe(6049, "hm_bouncereportkeys.brkaddress",
                         "update hm_bouncereportkeys set brkaddress = brkaddress where 1 = 0"),
         new SchemaProbe(6049, "hm_bouncereportkeys.brkrun",
                         "update hm_bouncereportkeys set brkrun = brkrun where 1 = 0"),

         // Upgrade6049to6050* - the OpenPGP key store (hm_pgpkeys) this server
         // publishes a Web Key Directory from, and the three switches that decide
         // whether anything is served. The identity column is never probed.
         new SchemaProbe(6050, "hm_domains.domainopenpgppublish",
                         "update hm_domains set domainopenpgppublish = domainopenpgppublish where 1 = 0"),
         new SchemaProbe(6050, "hm_domains.domainopenpgpnewaccounts",
                         "update hm_domains set domainopenpgpnewaccounts = domainopenpgpnewaccounts where 1 = 0"),
         new SchemaProbe(6050, "hm_accounts.accountopenpgppublish",
                         "update hm_accounts set accountopenpgppublish = accountopenpgppublish where 1 = 0"),
         new SchemaProbe(6050, "hm_pgpkeys.pgpkaccountid",
                         "update hm_pgpkeys set pgpkaccountid = pgpkaccountid where 1 = 0"),
         new SchemaProbe(6050, "hm_pgpkeys.pgpkfingerprint",
                         "update hm_pgpkeys set pgpkfingerprint = pgpkfingerprint where 1 = 0"),
         new SchemaProbe(6050, "hm_pgpkeys.pgpkversion",
                         "update hm_pgpkeys set pgpkversion = pgpkversion where 1 = 0"),
         new SchemaProbe(6050, "hm_pgpkeys.pgpkalgorithm",
                         "update hm_pgpkeys set pgpkalgorithm = pgpkalgorithm where 1 = 0"),
         new SchemaProbe(6050, "hm_pgpkeys.pgpkbits",
                         "update hm_pgpkeys set pgpkbits = pgpkbits where 1 = 0"),
         new SchemaProbe(6050, "hm_pgpkeys.pgpkuserid",
                         "update hm_pgpkeys set pgpkuserid = pgpkuserid where 1 = 0"),
         new SchemaProbe(6050, "hm_pgpkeys.pgpkcreated",
                         "update hm_pgpkeys set pgpkcreated = pgpkcreated where 1 = 0"),
         new SchemaProbe(6050, "hm_pgpkeys.pgpkexpires",
                         "update hm_pgpkeys set pgpkexpires = pgpkexpires where 1 = 0"),
         new SchemaProbe(6050, "hm_pgpkeys.pgpkbytes",
                         "update hm_pgpkeys set pgpkbytes = pgpkbytes where 1 = 0"),
         new SchemaProbe(6050, "hm_pgpkeys.pgpkkey",
                         "update hm_pgpkeys set pgpkkey = pgpkkey where 1 = 0"),
         new SchemaProbe(6050, "hm_pgpkeys.pgpkuploaded",
                         "update hm_pgpkeys set pgpkuploaded = pgpkuploaded where 1 = 0"),
         new SchemaProbe(6050, "hm_pgpkeys.pgpkuploadedby",
                         "update hm_pgpkeys set pgpkuploadedby = pgpkuploadedby where 1 = 0"),
         // Upgrade6050to6051* - outbound webhooks: the two flags a domain
         // carries (whether events about it are raised at all, and whether a
         // body may carry its subjects), the subscriptions (hm_webhooks) and
         // the delivery queue that survives a restart (hm_webhookqueue). The
         // identity columns are never probed.
         new SchemaProbe(6051, "hm_domains.domainwebhooksenabled",
                         "update hm_domains set domainwebhooksenabled = domainwebhooksenabled where 1 = 0"),
         new SchemaProbe(6051, "hm_domains.domainwebhooksubjects",
                         "update hm_domains set domainwebhooksubjects = domainwebhooksubjects where 1 = 0"),
         new SchemaProbe(6051, "hm_webhooks.whname",
                         "update hm_webhooks set whname = whname where 1 = 0"),
         new SchemaProbe(6051, "hm_webhooks.whurl",
                         "update hm_webhooks set whurl = whurl where 1 = 0"),
         new SchemaProbe(6051, "hm_webhooks.whsecret",
                         "update hm_webhooks set whsecret = whsecret where 1 = 0"),
         new SchemaProbe(6051, "hm_webhooks.whevents",
                         "update hm_webhooks set whevents = whevents where 1 = 0"),
         new SchemaProbe(6051, "hm_webhooks.whdomains",
                         "update hm_webhooks set whdomains = whdomains where 1 = 0"),
         new SchemaProbe(6051, "hm_webhooks.whaddresses",
                         "update hm_webhooks set whaddresses = whaddresses where 1 = 0"),
         new SchemaProbe(6051, "hm_webhooks.whenabled",
                         "update hm_webhooks set whenabled = whenabled where 1 = 0"),
         new SchemaProbe(6051, "hm_webhooks.whmaxperminute",
                         "update hm_webhooks set whmaxperminute = whmaxperminute where 1 = 0"),
         new SchemaProbe(6051, "hm_webhooks.whincludesubject",
                         "update hm_webhooks set whincludesubject = whincludesubject where 1 = 0"),
         new SchemaProbe(6051, "hm_webhooks.whcreated",
                         "update hm_webhooks set whcreated = whcreated where 1 = 0"),
         new SchemaProbe(6051, "hm_webhookqueue.wqwebhookid",
                         "update hm_webhookqueue set wqwebhookid = wqwebhookid where 1 = 0"),
         new SchemaProbe(6051, "hm_webhookqueue.wqevent",
                         "update hm_webhookqueue set wqevent = wqevent where 1 = 0"),
         new SchemaProbe(6051, "hm_webhookqueue.wqcreated",
                         "update hm_webhookqueue set wqcreated = wqcreated where 1 = 0"),
         new SchemaProbe(6051, "hm_webhookqueue.wqstate",
                         "update hm_webhookqueue set wqstate = wqstate where 1 = 0"),
         new SchemaProbe(6051, "hm_webhookqueue.wqtries",
                         "update hm_webhookqueue set wqtries = wqtries where 1 = 0"),
         new SchemaProbe(6051, "hm_webhookqueue.wqnext",
                         "update hm_webhookqueue set wqnext = wqnext where 1 = 0"),
         new SchemaProbe(6051, "hm_webhookqueue.wqerror",
                         "update hm_webhookqueue set wqerror = wqerror where 1 = 0"),
         new SchemaProbe(6051, "hm_webhookqueue.wqbody",
                         "update hm_webhookqueue set wqbody = wqbody where 1 = 0"),
         // Upgrade6051to6052* - country blocking. One column, on hm_domains: the
         // policy applied when an account of the domain signs in. The seven
         // hm_settings rows the same step seeds are rows and not columns, so
         // there is nothing for a probe to assert about them - a missing row
         // reads as the setting's default, which is off.
         new SchemaProbe(6052, "hm_domains.domaincountrypolicy",
                         "update hm_domains set domaincountrypolicy = domaincountrypolicy where 1 = 0"),
         // Upgrade6052to6053* - the devices that have signed in (hm_signins) and the
         // per-domain session policy. The identity column is never probed.
         new SchemaProbe(6053, "hm_signins.siaccountid",
                         "update hm_signins set siaccountid = siaccountid where 1 = 0"),
         new SchemaProbe(6053, "hm_signins.sidevice",
                         "update hm_signins set sidevice = sidevice where 1 = 0"),
         new SchemaProbe(6053, "hm_signins.siprotocol",
                         "update hm_signins set siprotocol = siprotocol where 1 = 0"),
         new SchemaProbe(6053, "hm_signins.siaddress",
                         "update hm_signins set siaddress = siaddress where 1 = 0"),
         new SchemaProbe(6053, "hm_signins.siagent",
                         "update hm_signins set siagent = siagent where 1 = 0"),
         new SchemaProbe(6053, "hm_signins.sicredential",
                         "update hm_signins set sicredential = sicredential where 1 = 0"),
         new SchemaProbe(6053, "hm_signins.sicredentialname",
                         "update hm_signins set sicredentialname = sicredentialname where 1 = 0"),
         new SchemaProbe(6053, "hm_signins.sifirstseen",
                         "update hm_signins set sifirstseen = sifirstseen where 1 = 0"),
         new SchemaProbe(6053, "hm_signins.silastseen",
                         "update hm_signins set silastseen = silastseen where 1 = 0"),
         new SchemaProbe(6053, "hm_signins.sicount",
                         "update hm_signins set sicount = sicount where 1 = 0"),
         new SchemaProbe(6053, "hm_signins.sirevoked",
                         "update hm_signins set sirevoked = sirevoked where 1 = 0"),
         new SchemaProbe(6053, "hm_signins.sirevokedtime",
                         "update hm_signins set sirevokedtime = sirevokedtime where 1 = 0"),
         new SchemaProbe(6053, "hm_domains.domainsessionidleseconds",
                         "update hm_domains set domainsessionidleseconds = domainsessionidleseconds where 1 = 0"),
         new SchemaProbe(6053, "hm_domains.domainsessionlifetimeseconds",
                         "update hm_domains set domainsessionlifetimeseconds = domainsessionlifetimeseconds where 1 = 0"),
         new SchemaProbe(6053, "hm_domains.domainsessiononeperdevice",
                         "update hm_domains set domainsessiononeperdevice = domainsessiononeperdevice where 1 = 0"),
         new SchemaProbe(6053, "hm_domains.domainsessionbindaddress",
                         "update hm_domains set domainsessionbindaddress = domainsessionbindaddress where 1 = 0"),
         // Upgrade6053to6054* - Web Push: hm_pushsubscriptions, one row per device a
         // person asked to be told on, with what it may be told and when it may not be.
         new SchemaProbe(6054, "hm_pushsubscriptions.psaccountid",
                         "update hm_pushsubscriptions set psaccountid = psaccountid where 1 = 0"),
         new SchemaProbe(6054, "hm_pushsubscriptions.psendpointhash",
                         "update hm_pushsubscriptions set psendpointhash = psendpointhash where 1 = 0"),
         new SchemaProbe(6054, "hm_pushsubscriptions.psendpoint",
                         "update hm_pushsubscriptions set psendpoint = psendpoint where 1 = 0"),
         new SchemaProbe(6054, "hm_pushsubscriptions.pskeyp256dh",
                         "update hm_pushsubscriptions set pskeyp256dh = pskeyp256dh where 1 = 0"),
         new SchemaProbe(6054, "hm_pushsubscriptions.pskeyauth",
                         "update hm_pushsubscriptions set pskeyauth = pskeyauth where 1 = 0"),
         new SchemaProbe(6054, "hm_pushsubscriptions.psdevice",
                         "update hm_pushsubscriptions set psdevice = psdevice where 1 = 0"),
         new SchemaProbe(6054, "hm_pushsubscriptions.pscreated",
                         "update hm_pushsubscriptions set pscreated = pscreated where 1 = 0"),
         new SchemaProbe(6054, "hm_pushsubscriptions.pslastsent",
                         "update hm_pushsubscriptions set pslastsent = pslastsent where 1 = 0"),
         new SchemaProbe(6054, "hm_pushsubscriptions.psstate",
                         "update hm_pushsubscriptions set psstate = psstate where 1 = 0"),
         new SchemaProbe(6054, "hm_pushsubscriptions.psfailures",
                         "update hm_pushsubscriptions set psfailures = psfailures where 1 = 0"),
         new SchemaProbe(6054, "hm_pushsubscriptions.psnextattempt",
                         "update hm_pushsubscriptions set psnextattempt = psnextattempt where 1 = 0"),
         new SchemaProbe(6054, "hm_pushsubscriptions.pslastdetail",
                         "update hm_pushsubscriptions set pslastdetail = pslastdetail where 1 = 0"),
         new SchemaProbe(6054, "hm_pushsubscriptions.psdetail",
                         "update hm_pushsubscriptions set psdetail = psdetail where 1 = 0"),
         new SchemaProbe(6054, "hm_pushsubscriptions.pssenders",
                         "update hm_pushsubscriptions set pssenders = pssenders where 1 = 0"),
         new SchemaProbe(6054, "hm_pushsubscriptions.pslabels",
                         "update hm_pushsubscriptions set pslabels = pslabels where 1 = 0"),
         new SchemaProbe(6054, "hm_pushsubscriptions.psquietfrom",
                         "update hm_pushsubscriptions set psquietfrom = psquietfrom where 1 = 0"),
         new SchemaProbe(6054, "hm_pushsubscriptions.psquietto",
                         "update hm_pushsubscriptions set psquietto = psquietto where 1 = 0"),
         new SchemaProbe(6054, "hm_pushsubscriptions.psquietoffset",
                         "update hm_pushsubscriptions set psquietoffset = psquietoffset where 1 = 0"),

         // Upgrade6054to6055* - complaints read, the feedback loops: the complaint
         // count beside each address's bounce counts (hm_bouncecounts.bccomplaints)
         // with the date of the last one (bclastcomplaint, which is what keeps a
         // complaint-only row from being swept before its count can reach the
         // threshold), the reports themselves (hm_complaints) and the per-domain
         // daily tally the complaint rate is computed from (hm_complaintvolume).
         new SchemaProbe(6055, "hm_bouncecounts.bccomplaints",
                         "update hm_bouncecounts set bccomplaints = bccomplaints where 1 = 0"),
         new SchemaProbe(6055, "hm_bouncecounts.bclastcomplaint",
                         "update hm_bouncecounts set bclastcomplaint = bclastcomplaint where 1 = 0"),
         new SchemaProbe(6055, "hm_complaints.cpdomainid",
                         "update hm_complaints set cpdomainid = cpdomainid where 1 = 0"),
         new SchemaProbe(6055, "hm_complaints.cpaccountid",
                         "update hm_complaints set cpaccountid = cpaccountid where 1 = 0"),
         new SchemaProbe(6055, "hm_complaints.cpkey",
                         "update hm_complaints set cpkey = cpkey where 1 = 0"),
         new SchemaProbe(6055, "hm_complaints.cpaddress",
                         "update hm_complaints set cpaddress = cpaddress where 1 = 0"),
         new SchemaProbe(6055, "hm_complaints.cpsender",
                         "update hm_complaints set cpsender = cpsender where 1 = 0"),
         new SchemaProbe(6055, "hm_complaints.cpfeedbacktype",
                         "update hm_complaints set cpfeedbacktype = cpfeedbacktype where 1 = 0"),
         new SchemaProbe(6055, "hm_complaints.cpprovider",
                         "update hm_complaints set cpprovider = cpprovider where 1 = 0"),
         new SchemaProbe(6055, "hm_complaints.cpsourceip",
                         "update hm_complaints set cpsourceip = cpsourceip where 1 = 0"),
         new SchemaProbe(6055, "hm_complaints.cpreporteddomain",
                         "update hm_complaints set cpreporteddomain = cpreporteddomain where 1 = 0"),
         new SchemaProbe(6055, "hm_complaints.cpmessageid",
                         "update hm_complaints set cpmessageid = cpmessageid where 1 = 0"),
         new SchemaProbe(6055, "hm_complaints.cpsubject",
                         "update hm_complaints set cpsubject = cpsubject where 1 = 0"),
         new SchemaProbe(6055, "hm_complaints.cparrival",
                         "update hm_complaints set cparrival = cparrival where 1 = 0"),
         new SchemaProbe(6055, "hm_complaints.cpreceived",
                         "update hm_complaints set cpreceived = cpreceived where 1 = 0"),
         new SchemaProbe(6055, "hm_complaintvolume.cvdomainid",
                         "update hm_complaintvolume set cvdomainid = cvdomainid where 1 = 0"),
         new SchemaProbe(6055, "hm_complaintvolume.cvday",
                         "update hm_complaintvolume set cvday = cvday where 1 = 0"),
         new SchemaProbe(6055, "hm_complaintvolume.cvsent",
                         "update hm_complaintvolume set cvsent = cvsent where 1 = 0"),
         new SchemaProbe(6055, "hm_complaintvolume.cvcomplaints",
                         "update hm_complaintvolume set cvcomplaints = cvcomplaints where 1 = 0"),

         // Upgrade6055to6056* - the journal of a mailbox rename or move.
         new SchemaProbe(6056, "hm_mailboxmoves.mmaccountid",
                         "update hm_mailboxmoves set mmaccountid = mmaccountid where 1 = 0"),
         new SchemaProbe(6056, "hm_mailboxmoves.mmoldaddress",
                         "update hm_mailboxmoves set mmoldaddress = mmoldaddress where 1 = 0"),
         new SchemaProbe(6056, "hm_mailboxmoves.mmnewaddress",
                         "update hm_mailboxmoves set mmnewaddress = mmnewaddress where 1 = 0"),
         new SchemaProbe(6056, "hm_mailboxmoves.mmstage",
                         "update hm_mailboxmoves set mmstage = mmstage where 1 = 0"),
         new SchemaProbe(6056, "hm_mailboxmoves.mmkeepalias",
                         "update hm_mailboxmoves set mmkeepalias = mmkeepalias where 1 = 0"),
         new SchemaProbe(6056, "hm_mailboxmoves.mmmessages",
                         "update hm_mailboxmoves set mmmessages = mmmessages where 1 = 0"),
         new SchemaProbe(6056, "hm_mailboxmoves.mmstarted",
                         "update hm_mailboxmoves set mmstarted = mmstarted where 1 = 0"),
         new SchemaProbe(6056, "hm_mailboxmoves.mmfinished",
                         "update hm_mailboxmoves set mmfinished = mmfinished where 1 = 0"),
         new SchemaProbe(6056, "hm_mailboxmoves.mmdetail",
                         "update hm_mailboxmoves set mmdetail = mmdetail where 1 = 0"),
         // Upgrade6056to6057* - the reputation watch: the last answer each block list
         // gave about each outbound address (hm_reputationchecks).
         new SchemaProbe(6057, "hm_reputationchecks.rcaddress",
                         "update hm_reputationchecks set rcaddress = rcaddress where 1 = 0"),
         new SchemaProbe(6057, "hm_reputationchecks.rcsources",
                         "update hm_reputationchecks set rcsources = rcsources where 1 = 0"),
         new SchemaProbe(6057, "hm_reputationchecks.rczone",
                         "update hm_reputationchecks set rczone = rczone where 1 = 0"),
         new SchemaProbe(6057, "hm_reputationchecks.rcstatus",
                         "update hm_reputationchecks set rcstatus = rcstatus where 1 = 0"),
         new SchemaProbe(6057, "hm_reputationchecks.rccode",
                         "update hm_reputationchecks set rccode = rccode where 1 = 0"),
         new SchemaProbe(6057, "hm_reputationchecks.rcreason",
                         "update hm_reputationchecks set rcreason = rcreason where 1 = 0"),
         new SchemaProbe(6057, "hm_reputationchecks.rcchecked",
                         "update hm_reputationchecks set rcchecked = rcchecked where 1 = 0"),
         new SchemaProbe(6057, "hm_reputationchecks.rclistedsince",
                         "update hm_reputationchecks set rclistedsince = rclistedsince where 1 = 0"),
         new SchemaProbe(6057, "hm_reputationchecks.rcconfirmed",
                         "update hm_reputationchecks set rcconfirmed = rcconfirmed where 1 = 0"),
         // Upgrade6057to6058* - spam traps: the addresses a domain declares
         // (hm_spamtraps) and each time one was addressed (hm_spamtraphits). The
         // identity columns are never probed.
         new SchemaProbe(6058, "hm_spamtraps.stdomainid",
                         "update hm_spamtraps set stdomainid = stdomainid where 1 = 0"),
         new SchemaProbe(6058, "hm_spamtraps.stlocalpart",
                         "update hm_spamtraps set stlocalpart = stlocalpart where 1 = 0"),
         new SchemaProbe(6058, "hm_spamtraps.stcreated",
                         "update hm_spamtraps set stcreated = stcreated where 1 = 0"),
         new SchemaProbe(6058, "hm_spamtraphits.sthdomainid",
                         "update hm_spamtraphits set sthdomainid = sthdomainid where 1 = 0"),
         new SchemaProbe(6058, "hm_spamtraphits.sthaddress",
                         "update hm_spamtraphits set sthaddress = sthaddress where 1 = 0"),
         new SchemaProbe(6058, "hm_spamtraphits.sthipaddress",
                         "update hm_spamtraphits set sthipaddress = sthipaddress where 1 = 0"),
         new SchemaProbe(6058, "hm_spamtraphits.sthsender",
                         "update hm_spamtraphits set sthsender = sthsender where 1 = 0"),
         new SchemaProbe(6058, "hm_spamtraphits.sthtime",
                         "update hm_spamtraphits set sthtime = sthtime where 1 = 0"),
         new SchemaProbe(6058, "hm_spamtraphits.sthbanned",
                         "update hm_spamtraphits set sthbanned = sthbanned where 1 = 0"),
         new SchemaProbe(6058, "hm_spamtraphits.sthblocked",
                         "update hm_spamtraphits set sthblocked = sthblocked where 1 = 0"),
         new SchemaProbe(6058, "hm_spamtraphits.sthnote",
                         "update hm_spamtraphits set sthnote = sthnote where 1 = 0"),
         // Both tables are the domain's, and a row left behind when the domain
         // goes would start trapping - or claiming to have trapped - an address
         // in whichever domain is issued that id next. The code deletes them, and
         // reports it when it cannot; these are the database's own guarantee
         // underneath that, in the shape the 6030 probes above explain.
         new SchemaProbe(6058, "hm_spamtraps.fk_hm_spamtraps_domain",
                         "update hm_dbversion set value = value / (value - value) where not exists (select 1 from information_schema.table_constraints where constraint_name = 'fk_hm_spamtraps_domain' and constraint_type = 'FOREIGN KEY')"),
         // Upgrade6059to6060* - the out-of-office policy: a window and an
         // internal text on the account, a window on the domain, and the memory
         // of who was answered when (hm_autoreplies), which had been in process
         // memory. The identity column is never probed.
         new SchemaProbe(6060, "hm_accounts.accountvacationdays",
                         "update hm_accounts set accountvacationdays = accountvacationdays where 1 = 0"),
         new SchemaProbe(6060, "hm_accounts.accountvacationinternalsubject",
                         "update hm_accounts set accountvacationinternalsubject = accountvacationinternalsubject where 1 = 0"),
         new SchemaProbe(6060, "hm_accounts.accountvacationinternalmessage",
                         "update hm_accounts set accountvacationinternalmessage = accountvacationinternalmessage where 1 = 0"),
         new SchemaProbe(6060, "hm_accounts.accountvacationinternalonly",
                         "update hm_accounts set accountvacationinternalonly = accountvacationinternalonly where 1 = 0"),
         new SchemaProbe(6060, "hm_domains.domainvacationdays",
                         "update hm_domains set domainvacationdays = domainvacationdays where 1 = 0"),
         new SchemaProbe(6060, "hm_autoreplies.araccountid",
                         "update hm_autoreplies set araccountid = araccountid where 1 = 0"),
         new SchemaProbe(6060, "hm_autoreplies.arsender",
                         "update hm_autoreplies set arsender = arsender where 1 = 0"),
         new SchemaProbe(6060, "hm_autoreplies.arsent",
                         "update hm_autoreplies set arsent = arsent where 1 = 0"),
         // Upgrade6060to6061* - the quota policy per domain: the warning's
         // threshold, subject and text, the grace past the limit and the point
         // at which sending stops. -1 and an empty text mean the server's own.
         new SchemaProbe(6061, "hm_domains.domainquotawarningpercent",
                         "update hm_domains set domainquotawarningpercent = domainquotawarningpercent where 1 = 0"),
         new SchemaProbe(6061, "hm_domains.domainquotawarningsubject",
                         "update hm_domains set domainquotawarningsubject = domainquotawarningsubject where 1 = 0"),
         new SchemaProbe(6061, "hm_domains.domainquotawarningtext",
                         "update hm_domains set domainquotawarningtext = domainquotawarningtext where 1 = 0"),
         new SchemaProbe(6061, "hm_domains.domainquotagracepercent",
                         "update hm_domains set domainquotagracepercent = domainquotagracepercent where 1 = 0"),
         new SchemaProbe(6061, "hm_domains.domainquotasendstoppercent",
                         "update hm_domains set domainquotasendstoppercent = domainquotasendstoppercent where 1 = 0"),
         // Upgrade6061to6062* - the per-domain daily sample: spam and viruses
         // counted by recipient domain, and the store's size per domain, one
         // row per day and domain (hm_domainsamples). The identity column is
         // never probed.
         new SchemaProbe(6062, "hm_domainsamples.dsday",
                         "update hm_domainsamples set dsday = dsday where 1 = 0"),
         new SchemaProbe(6062, "hm_domainsamples.dsdomain",
                         "update hm_domainsamples set dsdomain = dsdomain where 1 = 0"),
         new SchemaProbe(6062, "hm_domainsamples.dsspam",
                         "update hm_domainsamples set dsspam = dsspam where 1 = 0"),
         new SchemaProbe(6062, "hm_domainsamples.dsviruses",
                         "update hm_domainsamples set dsviruses = dsviruses where 1 = 0"),
         new SchemaProbe(6062, "hm_domainsamples.dsstorebytes",
                         "update hm_domainsamples set dsstorebytes = dsstorebytes where 1 = 0"),
         new SchemaProbe(6062, "hm_domainsamples.dsstoremessages",
                         "update hm_domainsamples set dsstoremessages = dsstoremessages where 1 = 0"),
         // Upgrade6062to6063* - a rule's domain: 0 for the server's and an
         // account's rules, the domain's id for a rule that runs for one domain.
         new SchemaProbe(6063, "hm_rules.ruledomainid",
                         "update hm_rules set ruledomainid = ruledomainid where 1 = 0"),
         // Upgrade6063to6064* - a calendar's colour, and the table that shares a
         // calendar with another account of this server, read-only or read-write.
         new SchemaProbe(6064, "hm_calendars.calendarcolor",
                         "update hm_calendars set calendarcolor = calendarcolor where 1 = 0"),
         new SchemaProbe(6064, "hm_calendarshares.sharewrite",
                         "update hm_calendarshares set sharewrite = sharewrite where 1 = 0"),
         // Upgrade6064to6065* - an external account signed in with OAuth (the
         // provider and the refresh token, protected), and the folders a
         // migration skips and last saw on the other server.
         new SchemaProbe(6065, "hm_fetchaccounts.faoauthprovider",
                         "update hm_fetchaccounts set faoauthprovider = faoauthprovider where 1 = 0"),
         new SchemaProbe(6065, "hm_fetchaccounts.faoauthtoken",
                         "update hm_fetchaccounts set faoauthtoken = faoauthtoken where 1 = 0"),
         new SchemaProbe(6065, "hm_fetchaccounts.famirrorskip",
                         "update hm_fetchaccounts set famirrorskip = famirrorskip where 1 = 0"),
         new SchemaProbe(6065, "hm_fetchaccounts.famirrorseen",
                         "update hm_fetchaccounts set famirrorseen = famirrorseen where 1 = 0"),
         // Upgrade6065to6066* - the addresses elsewhere an account has proved it owns
         // with a code sent to them, and sends as.
         new SchemaProbe(6066, "hm_senderaddresses.sacreated",
                         "update hm_senderaddresses set sacreated = sacreated where 1 = 0"),
         // Upgrade6066to6067* - masked addresses: an address a reader makes for one
         // sign-up that delivers to the mailbox and can be switched off.
         new SchemaProbe(6067, "hm_maskedaddresses.macreated",
                         "update hm_maskedaddresses set macreated = macreated where 1 = 0"),
         // Upgrade6067to6068* - deleted messages kept for the recovery window.
         new SchemaProbe(6068, "hm_recoverable.rcdeleted",
                         "update hm_recoverable set rcdeleted = rcdeleted where 1 = 0"),
         // Upgrade6068to6069* - messages that expire: kept on the server behind a
         // link, with the expiry, the first opening and the revocation.
         new SchemaProbe(6069, "hm_confidential.cfexpires",
                         "update hm_confidential set cfexpires = cfexpires where 1 = 0"),
         // Upgrade6069to6070* - the reports our own domains receive, read: the
         // domain's report mailbox, the DMARC aggregate reports and their records,
         // and the TLS-RPT reports.
         new SchemaProbe(6070, "hm_domains.domainreportmailbox",
                         "update hm_domains set domainreportmailbox = domainreportmailbox where 1 = 0"),
         new SchemaProbe(6070, "hm_dmarcreports.drreceived",
                         "update hm_dmarcreports set drreceived = drreceived where 1 = 0"),
         new SchemaProbe(6070, "hm_dmarcreports.drrecords",
                         "update hm_dmarcreports set drrecords = drrecords where 1 = 0"),
         new SchemaProbe(6070, "hm_dmarcreportrecords.rrcount",
                         "update hm_dmarcreportrecords set rrcount = rrcount where 1 = 0"),
         new SchemaProbe(6070, "hm_tlsreports.trreceived",
                         "update hm_tlsreports set trreceived = trreceived where 1 = 0"),
         // Upgrade6070to6071* - compromised-account detection: the baseline of
         // authenticated submissions, the recipients remembered, the review state
         // and the messages held for review, of which hm_heldmessages is the last
         // table the step creates.
         new SchemaProbe(6071, "hm_heldmessages.hmheld",
                         "update hm_heldmessages set hmheld = hmheld where 1 = 0"),
         // Upgrade6071to6072* - greylisting hardened: the two hash columns of the
         // triplet key (one probe each, since the step can half-apply on SQL CE)
         // and the bypass list, of which glbnote is the last column created.
         new SchemaProbe(6072, "hm_greylisting_triplets.glsenderhash",
                         "update hm_greylisting_triplets set glsenderhash = glsenderhash where 1 = 0"),
         new SchemaProbe(6072, "hm_greylisting_triplets.glrecipienthash",
                         "update hm_greylisting_triplets set glrecipienthash = glrecipienthash where 1 = 0"),
         new SchemaProbe(6072, "hm_greylisting_bypass.glbnote",
                         "update hm_greylisting_bypass set glbnote = glbnote where 1 = 0"),
         // Upgrade6072to6073* - the domain white and black list keyed by envelope
         // domain or HELO suffix (discussion #285): one table, and this is its
         // last column.
         new SchemaProbe(6073, "hm_domainpolicylist.dplcreated",
                         "update hm_domainpolicylist set dplcreated = dplcreated where 1 = 0"),
         // Upgrade6073to6074* - distribution lists that define themselves: the
         // membership query on the list.
         new SchemaProbe(6074, "hm_distributionlists.distributionlistmembership",
                         "update hm_distributionlists set distributionlistmembership = distributionlistmembership where 1 = 0"),
         // Upgrade6074to6075* - journaling and dual delivery: one row per rule,
         // of which jrmode is the column that says whether the copy is the
         // envelope report or the message itself.
         new SchemaProbe(6075, "hm_journalrules.jrmode",
                         "update hm_journalrules set jrmode = jrmode where 1 = 0"),

         // Upgrade6075to6076* - the recovery address, the one-shot links that
         // prove it and reset a password, and the recovery codes. One probe per
         // table: SQL Server Compact commits each statement as it runs, so a step
         // that stopped after the first CREATE TABLE would otherwise stamp a
         // version onto a database holding one table of three.
         new SchemaProbe(6076, "hm_accountrecovery.acrverified",
                         "update hm_accountrecovery set acrverified = acrverified where 1 = 0"),
         new SchemaProbe(6076, "hm_recoverytokens.rttoken",
                         "update hm_recoverytokens set rttoken = rttoken where 1 = 0"),
         new SchemaProbe(6076, "hm_recoverycodes.rkhash",
                         "update hm_recoverycodes set rkhash = rkhash where 1 = 0"),
         // Upgrade6076to6077* - passkeys: one row per WebAuthn credential, the
         // account's or (with a null account id) the administrator's own, with
         // the COSE public key and the signature counter a cloned authenticator
         // is caught by.
         new SchemaProbe(6077, "hm_passkeys.pksigncount",
                         "update hm_passkeys set pksigncount = pksigncount where 1 = 0"),
         // Upgrade6077to6078* - the organisation's directory: one row per account
         // with a directory entry, of which the opt-out is the column the step
         // exists for.
         new SchemaProbe(6078, "hm_directory.diroptout",
                         "update hm_directory set diroptout = diroptout where 1 = 0"),
         // Upgrade6078to6079* - shared address books: the books an account keeps,
         // the shares that hand one to another account or to a whole domain, and
         // the column on hm_contacts that says which book a contact is in. The
         // column is the one an ALTER adds to an existing table, so it is the one
         // that can fail while the step still stamps the version.
         new SchemaProbe(6079, "hm_addressbooks.bookname",
                         "update hm_addressbooks set bookname = bookname where 1 = 0"),
         new SchemaProbe(6079, "hm_addressbookshares.sharewrite",
                         "update hm_addressbookshares set sharewrite = sharewrite where 1 = 0"),
         new SchemaProbe(6079, "hm_contacts.contactbookid",
                         "update hm_contacts set contactbookid = contactbookid where 1 = 0"),
         new SchemaProbe(6058, "hm_spamtraphits.fk_hm_spamtraphits_domain",
                         "update hm_dbversion set value = value / (value - value) where not exists (select 1 from information_schema.table_constraints where constraint_name = 'fk_hm_spamtraphits_domain' and constraint_type = 'FOREIGN KEY')")
      };

      /// <summary>
      /// Every probe, for the checks that run them all against a database known to
      /// be correct: a probe that fails there is a probe the backend cannot
      /// execute, not a missing object.
      /// </summary>
      public static IList<SchemaProbe> All
      {
         get { return Probes.ToList(); }
      }

      /// <summary>
      /// The probes that must succeed once the database has reached
      /// <paramref name="version"/>. Empty for a step that has none registered -
      /// most of the historical chain predates this check.
      /// </summary>
      public static IList<SchemaProbe> GetProbesFor(int version)
      {
         return Probes.Where(probe => probe.Version == version).ToList();
      }
   }
}
