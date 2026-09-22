if exists (select * from sysobjects where id = object_id('hm_roleassignments') and objectproperty(id, 'isusertable') = 1) drop table hm_roleassignments
if exists (select * from sysobjects where id = object_id('hm_roles') and objectproperty(id, 'isusertable') = 1) drop table hm_roles

if exists (select * from sysobjects where id = object_id('hm_audit') and objectproperty(id, 'isusertable') = 1) drop table hm_audit

if exists (select * from sysobjects where id = object_id('hm_alertrules') and objectproperty(id, 'isusertable') = 1) drop table hm_alertrules

if exists (select * from sysobjects where id = object_id('hm_webhookqueue') and objectproperty(id, 'isusertable') = 1) drop table hm_webhookqueue
if exists (select * from sysobjects where id = object_id('hm_webhooks') and objectproperty(id, 'isusertable') = 1) drop table hm_webhooks
if exists (select * from sysobjects where id = object_id('hm_alertevents') and objectproperty(id, 'isusertable') = 1) drop table hm_alertevents
if exists (select * from sysobjects where id = object_id('hm_remotedomainpolicies') and objectproperty(id, 'isusertable') = 1) drop table hm_remotedomainpolicies

if exists (select * from sysobjects where id = object_id('hm_dkimrotations') and objectproperty(id, 'isusertable') = 1) drop table hm_dkimrotations

if exists (select * from sysobjects where id = object_id('hm_reputationchecks') and objectproperty(id, 'isusertable') = 1) drop table hm_reputationchecks
if exists (select * from sysobjects where id = object_id('hm_spamtraps') and objectproperty(id, 'isusertable') = 1) drop table hm_spamtraps
if exists (select * from sysobjects where id = object_id('hm_journalrules') and objectproperty(id, 'isusertable') = 1) drop table hm_journalrules

if exists (select * from sysobjects where id = object_id('hm_spamtraphits') and objectproperty(id, 'isusertable') = 1) drop table hm_spamtraphits

if exists (select * from sysobjects where id = object_id('hm_domainpolicylist') and objectproperty(id, 'isusertable') = 1) drop table hm_domainpolicylist

if exists (select * from sysobjects where id = object_id('hm_dmarcreportrecords') and objectproperty(id, 'isusertable') = 1) drop table hm_dmarcreportrecords
if exists (select * from sysobjects where id = object_id('hm_dmarcreports') and objectproperty(id, 'isusertable') = 1) drop table hm_dmarcreports
if exists (select * from sysobjects where id = object_id('hm_tlsreports') and objectproperty(id, 'isusertable') = 1) drop table hm_tlsreports
if exists (select * from sysobjects where id = object_id('hm_accountactivity') and objectproperty(id, 'isusertable') = 1) drop table hm_accountactivity
if exists (select * from sysobjects where id = object_id('hm_accountrecipients') and objectproperty(id, 'isusertable') = 1) drop table hm_accountrecipients
if exists (select * from sysobjects where id = object_id('hm_accountreview') and objectproperty(id, 'isusertable') = 1) drop table hm_accountreview
if exists (select * from sysobjects where id = object_id('hm_heldmessages') and objectproperty(id, 'isusertable') = 1) drop table hm_heldmessages
if exists (select * from sysobjects where id = object_id('hm_confidential') and objectproperty(id, 'isusertable') = 1) drop table hm_confidential
if exists (select * from sysobjects where id = object_id('hm_passkeys') and objectproperty(id, 'isusertable') = 1) drop table hm_passkeys

if exists (select * from sysobjects where id = object_id('hm_directory') and objectproperty(id, 'isusertable') = 1) drop table hm_directory
if exists (select * from sysobjects where id = object_id('hm_recoverable') and objectproperty(id, 'isusertable') = 1) drop table hm_recoverable
if exists (select * from sysobjects where id = object_id('hm_maskedaddresses') and objectproperty(id, 'isusertable') = 1) drop table hm_maskedaddresses
if exists (select * from sysobjects where id = object_id('hm_senderaddresses') and objectproperty(id, 'isusertable') = 1) drop table hm_senderaddresses
if exists (select * from sysobjects where id = object_id('hm_calendarshares') and objectproperty(id, 'isusertable') = 1) drop table hm_calendarshares
if exists (select * from sysobjects where id = object_id('hm_calendarobjects') and objectproperty(id, 'isusertable') = 1) drop table hm_calendarobjects
if exists (select * from sysobjects where id = object_id('hm_calendars') and objectproperty(id, 'isusertable') = 1) drop table hm_calendars
if exists (select * from sysobjects where id = object_id('hm_addressbookshares') and objectproperty(id, 'isusertable') = 1) drop table hm_addressbookshares

if exists (select * from sysobjects where id = object_id('hm_addressbooks') and objectproperty(id, 'isusertable') = 1) drop table hm_addressbooks

if exists (select * from sysobjects where id = object_id('hm_contacts') and objectproperty(id, 'isusertable') = 1) drop table hm_contacts
if exists (select * from sysobjects where id = object_id('hm_pushsubscriptions') and objectproperty(id, 'isusertable') = 1) drop table hm_pushsubscriptions
if exists (select * from sysobjects where id = object_id('hm_accountprefs') and objectproperty(id, 'isusertable') = 1) drop table hm_accountprefs
if exists (select * from sysobjects where id = object_id('hm_scheduled') and objectproperty(id, 'isusertable') = 1) drop table hm_scheduled
if exists (select * from sysobjects where id = object_id('hm_files') and objectproperty(id, 'isusertable') = 1) drop table hm_files
if exists (select * from sysobjects where id = object_id('hm_pgpkeys') and objectproperty(id, 'isusertable') = 1) drop table hm_pgpkeys
if exists (select * from sysobjects where id = object_id('hm_pgpring') and objectproperty(id, 'isusertable') = 1) drop table hm_pgpring
if exists (select * from sysobjects where id = object_id('hm_protectedmessages') and objectproperty(id, 'isusertable') = 1) drop table hm_protectedmessages
if exists (select * from sysobjects where id = object_id('hm_recall') and objectproperty(id, 'isusertable') = 1) drop table hm_recall
if exists (select * from sysobjects where id = object_id('hm_smimekeys') and objectproperty(id, 'isusertable') = 1) drop table hm_smimekeys
if exists (select * from sysobjects where id = object_id('hm_accounts') and objectproperty(id, 'isusertable') = 1) drop table hm_accounts 

if exists (select * from sysobjects where id = object_id('hm_imapfolders') and objectproperty(id, 'isusertable') = 1) drop table hm_imapfolders 

if exists (select * from sysobjects where id = object_id('hm_securityranges') and objectproperty(id, 'isusertable') = 1) drop table hm_securityranges 

if exists (select * from sysobjects where id = object_id('hm_aliases') and objectproperty(id, 'isusertable') = 1) drop table hm_aliases 

if exists (select * from sysobjects where id = object_id('hm_domains') and objectproperty(id, 'isusertable') = 1) drop table hm_domains 

if exists (select * from sysobjects where id = object_id('hm_domain_aliases') and objectproperty(id, 'isusertable') = 1) drop table hm_domain_aliases 

if exists (select * from sysobjects where id = object_id('hm_messages') and objectproperty(id, 'isusertable') = 1) drop table hm_messages 

if exists (select * from sysobjects where id = object_id('hm_message_metadata') and objectproperty(id, 'isusertable') = 1) drop table hm_message_metadata 

if exists (select * from sysobjects where id = object_id('hm_settings') and objectproperty(id, 'isusertable') = 1) drop table hm_settings

if exists (select * from sysobjects where id = object_id('hm_senderlist') and objectproperty(id, 'isusertable') = 1) drop table hm_senderlist

if exists (select * from sysobjects where id = object_id('hm_digeststate') and objectproperty(id, 'isusertable') = 1) drop table hm_digeststate

if exists (select * from sysobjects where id = object_id('hm_digesttokens') and objectproperty(id, 'isusertable') = 1) drop table hm_digesttokens

if exists (select * from sysobjects where id = object_id('hm_inisettings') and objectproperty(id, 'isusertable') = 1) drop table hm_inisettings

if exists (select * from sysobjects where id = object_id('hm_dbversion') and objectproperty(id, 'isusertable') = 1) drop table hm_dbversion

if exists (select * from sysobjects where id = object_id('hm_routes') and objectproperty(id, 'isusertable') = 1) drop table hm_routes 

if exists (select * from sysobjects where id = object_id('hm_routeaddresses') and objectproperty(id, 'isusertable') = 1) drop table hm_routeaddresses 

if exists (select * from sysobjects where id = object_id('hm_serverlog') and objectproperty(id, 'isusertable') = 1) drop table hm_serverlog 

if exists (select * from sysobjects where id = object_id('hm_distributionlists') and objectproperty(id, 'isusertable') = 1) drop table hm_distributionlists 

if exists (select * from sysobjects where id = object_id('hm_distributionlistsrecipients') and objectproperty(id, 'isusertable') = 1) drop table hm_distributionlistsrecipients 

if exists (select * from sysobjects where id = object_id('hm_messagerecipients') and objectproperty(id, 'isusertable') = 1) drop table hm_messagerecipients 

if exists (select * from sysobjects where id = object_id('hm_dnsbl') and objectproperty(id, 'isusertable') = 1) drop table hm_dnsbl 

if exists (select * from sysobjects where id = object_id('hm_fetchaccounts') and objectproperty(id, 'isusertable') = 1) drop table hm_fetchaccounts 

if exists (select * from sysobjects where id = object_id('hm_fetchaccounts_uids') and objectproperty(id, 'isusertable') = 1) drop table hm_fetchaccounts_uids 

if exists (select * from sysobjects where id = object_id('hm_apppasswords') and objectproperty(id, 'isusertable') = 1) drop table hm_apppasswords


if exists (select * from sysobjects where id = object_id('hm_rules') and objectproperty(id, 'isusertable') = 1) drop table hm_rules 

if exists (select * from sysobjects where id = object_id('hm_rule_criterias') and objectproperty(id, 'isusertable') = 1) drop table hm_rule_criterias 

if exists (select * from sysobjects where id = object_id('hm_rule_actions') and objectproperty(id, 'isusertable') = 1) drop table hm_rule_actions 

if exists (select * from sysobjects where id = object_id('hm_surblservers') and objectproperty(id, 'isusertable') = 1) drop table hm_surblservers 

if exists (select * from sysobjects where id = object_id('hm_greylisting_triplets') and objectproperty(id, 'isusertable') = 1) drop table hm_greylisting_triplets 

if exists (select * from sysobjects where id = object_id('hm_blocked_attachments') and objectproperty(id, 'isusertable') = 1) drop table hm_blocked_attachments 

if exists (select * from sysobjects where id = object_id('hm_servermessages') and objectproperty(id, 'isusertable') = 1) drop table hm_servermessages 

if exists (select * from sysobjects where id = object_id('hm_greylisting_whiteaddresses') and objectproperty(id, 'isusertable') = 1) drop table hm_greylisting_whiteaddresses 
if exists (select * from sysobjects where id = object_id('hm_greylisting_bypass') and objectproperty(id, 'isusertable') = 1) drop table hm_greylisting_bypass

if exists (select * from sysobjects where id = object_id('hm_tcpipports') and objectproperty(id, 'isusertable') = 1) drop table hm_tcpipports 

if exists (select * from sysobjects where id = object_id('hm_whitelist') and objectproperty(id, 'isusertable') = 1) drop table hm_whitelist 

if exists (select * from sysobjects where id = object_id('hm_sslcertificates') and objectproperty(id, 'isusertable') = 1) drop table hm_sslcertificates 

if exists (select * from sysobjects where id = object_id('hm_groups') and objectproperty(id, 'isusertable') = 1) drop table hm_groups 

if exists (select * from sysobjects where id = object_id('hm_group_members') and objectproperty(id, 'isusertable') = 1) drop table hm_group_members 

if exists (select * from sysobjects where id = object_id('hm_acl') and objectproperty(id, 'isusertable') = 1) drop table hm_acl 

if exists (select * from sysobjects where id = object_id('hm_incoming_relays') and objectproperty(id, 'isusertable') = 1) drop table hm_incoming_relays

if exists (select * from sysobjects where id = object_id('hm_logon_failures') and objectproperty(id, 'isusertable') = 1) drop table hm_logon_failures

create proc hm_drop_table_objects
	@tablename	sysname
as
set nocount on
declare @constname	sysname,
		@cmd		varchar(1024)
declare curs_constraints cursor for
	select 	name
	from 	sysobjects ,
	where 	xtype in ('C', 'F', 'PK', 'UQ', 'D')
	and	(status & 64) = 0
	and     parent_obj = object_id(@tablename)
open curs_constraints
fetch next from curs_constraints into @constname
while (@@fetch_status = 0)
begin
	select @cmd = 'ALTER TABLE ' + @tablename + ' DROP CONSTRAINT ' + @constname
	exec(@cmd)
	fetch next from curs_constraints into @constname
end
close curs_constraints
deallocate curs_constraints
DECLARE @indexName NVARCHAR(128)
DECLARE @dropIndexSql NVARCHAR(4000)
DECLARE tableIndexes CURSOR FOR
SELECT name FROM sysindexes
WHERE id = OBJECT_ID(@tablename) AND
  indid > 0 AND indid < 255 AND
  INDEXPROPERTY(id, name, 'IsStatistics') = 0
ORDER BY indid DESC
OPEN tableIndexes
FETCH NEXT FROM tableIndexes INTO @indexName
WHILE @@fetch_status = 0
BEGIN
  SET @dropIndexSql = N'DROP INDEX ' + @tablename + '.' + @indexName
  EXEC sp_executesql @dropIndexSql
  FETCH NEXT FROM tableIndexes INTO @indexName
END
CLOSE tableIndexes
DEALLOCATE tableIndexes
return 0 

CREATE proc [dbo].[hm_drop_table_column]
	@tablename sysname,
	@columnname sysname
as
	set nocount on,
	declare @constname sysname,
	@cmd varchar(1024)
	declare curs_constraints cursor for
	-- delete constraints on column level
	SELECT CONSTRAINT_NAME FROM INFORMATION_SCHEMA.CONSTRAINT_COLUMN_USAGE
		WHERE TABLE_NAME = @tablename AND COLUMN_NAME = @columnname
	open curs_constraints
	fetch next from curs_constraints into @constname
	while (@@fetch_status = 0)
	begin
		select @cmd = 'ALTER TABLE ' + @tablename + ' DROP CONSTRAINT ' + @constname
		exec(@cmd)
		fetch next from curs_constraints into @constname
	end
	close curs_constraints
	deallocate curs_constraints
	-- delete constraints on table level
	declare curs_constraints cursor for
	select name from sysobjects where id in 
	(select constid from sysconstraints where id = OBJECT_ID(@tablename) and colid in
	(select colid from syscolumns where id = OBJECT_ID(@tablename) and name = @columnname))
	open curs_constraints
	fetch next from curs_constraints into @constname
	while (@@fetch_status = 0)
	begin
		select @cmd = 'ALTER TABLE ' + @tablename + ' DROP CONSTRAINT ' + @constname
		exec(@cmd)
		fetch next from curs_constraints into @constname
	end
	close curs_constraints
	deallocate curs_constraints
	declare curs_constraints cursor for
	-- delete indexes for column
	select name from sysindexes where id = OBJECT_ID(@tablename) and INDEXPROPERTY(id, name, 'IsStatistics') = 0 and
	indid in (select indid from sysindexkeys where id = OBJECT_ID(@tablename)
	and colid in (select colid from syscolumns where id = OBJECT_ID(@tablename) and name = @columnname))
	open curs_constraints
	fetch next from curs_constraints into @constname
	while (@@fetch_status = 0)
	begin
		select @cmd = 'DROP INDEX ' + @tablename + '.' + @constname
		exec(@cmd)
		fetch next from curs_constraints into @constname
	end
	close curs_constraints
	deallocate curs_constraints
	select @cmd = 'ALTER TABLE ' + @tablename + ' DROP COLUMN ' + @columnname
	exec(@cmd)
return 0

create table hm_accounts (
	accountid int identity (1, 1) not null,
	accountdomainid int not null,
	accountadminlevel tinyint not null ,
	accountaddress nvarchar(255) not null,
	accountpassword nvarchar(255) not null ,
	accountactive int not null,
	accountisad int not null,
	accountaddomain nvarchar(255) not null ,
	accountadusername nvarchar(255) not null,
	accountmaxsize int not null,
	accountmessageretentiondays int not null default 0,
	accountvacationmessageon tinyint not null,
	accountvacationmessage nvarchar(1000) not null,
	accountvacationsubject nvarchar(200) not null,
	accountpwencryption tinyint not null,
	accountforwardenabled tinyint not null,
	accountforwardaddress nvarchar(255) not null,
	accountforwardkeeporiginal tinyint not null,
	accountenablesignature tinyint not null,
	accountsignatureplaintext ntext not null,
	accountsignaturehtml ntext not null,
	accountlastlogontime datetime not null,
	accountvacationexpires tinyint not null,
	accountvacationexpiredate datetime not null,
	accountvacationbegindate datetime not null,
	accountpersonfirstname nvarchar(60) not null,
	accountpersonlastname nvarchar(60) not null,
	accountvacationabortspamflagged tinyint not null,
	accountvacationdays int not null default 0,
	accountvacationinternalsubject nvarchar(200) not null default '',
	accountvacationinternalmessage nvarchar(1000) not null default '',
	accountvacationinternalonly tinyint not null default 0,
	accountforwardabortspamflagged tinyint not null,
	accounttotpsecret nvarchar(255) not null,
	accountpasswordchanged datetime not null,
	accountantispamenabled tinyint not null,
	accountspammarkthreshold int not null,
	accountspamdeletethreshold int not null,
	accountopenpgppublish int not null default 0,
	accountzeroaccess int not null default 0,
	accountrestricteddelivery int not null default 0
) 

ALTER TABLE hm_accounts ADD CONSTRAINT hm_accounts_pk PRIMARY KEY NONCLUSTERED (accountid) 

ALTER TABLE hm_accounts ADD CONSTRAINT u_accountaddress UNIQUE NONCLUSTERED (accountaddress) 

CREATE CLUSTERED INDEX idx_hm_accounts ON hm_accounts (accountaddress) 

create table hm_aliases (
	aliasid int identity (1, 1) not null,
	aliasdomainid int not null ,
	aliasname nvarchar(255) not null,
	aliasvalue nvarchar(255) not null ,
	aliasactive tinyint not null
) 

ALTER TABLE hm_aliases ADD CONSTRAINT hm_aliases_pk PRIMARY KEY NONCLUSTERED (aliasid) 

ALTER TABLE hm_aliases ADD CONSTRAINT u_aliasname UNIQUE NONCLUSTERED (aliasname) 

CREATE CLUSTERED INDEX idx_hm_aliases ON hm_aliases (aliasdomainid, aliasname) 

create table hm_domains (
	domainid int identity (1, 1) not null,
	domainname nvarchar(80) not null,
	domainactive tinyint not null,
	domainpostmaster nvarchar(80) not null,
	domainmaxsize int not null,
	domainmessageretentiondays int not null default 0,
	domainaddomain nvarchar(255) not null,
	domainmaxmessagesize int not null,
	domainuseplusaddressing  tinyint not null,
	domainplusaddressingchar nvarchar(1) not null,
	domainantispamoptions integer not null,
	domainenablesignature tinyint not null,
	domainsignaturemethod tinyint not null,
	domainsignatureplaintext ntext not null,
	domainsignaturehtml ntext not null,
	domainaddsignaturestoreplies tinyint not null,
	domainaddsignaturestolocalemail tinyint not null,
	domainmaxnoofaccounts int not null,
	domainmaxnoofaliases int not null,
	domainmaxnoofdistributionlists int not null,
	domainlimitationsenabled int not null,
	domainmaxaccountsize int not null,
   domaindkimselector nvarchar(255) not null,
   domaindkimprivatekeyfile nvarchar(255) not null,
   domaindkimsecondaryselector nvarchar(255) not null,
   domaindkimsecondaryprivatekeyfile nvarchar(255) not null,
   domainrelayhost nvarchar(255) not null,
   domainrelayport int not null,
   domainrelayrequiresauth int not null,
   domainrelayusername nvarchar(255) not null,
   domainrelaypassword nvarchar(1024) not null,
   domainrelayconnectionsecurity int not null,
	domainvacationmessageon int not null,
	domainvacationsubject nvarchar(200) not null,
	domainvacationmessage nvarchar(1000) not null,
	domainvacationinternalsubject nvarchar(200) not null,
	domainvacationinternalmessage nvarchar(1000) not null,
	domainvacationexternaloverride int not null,
	domainvacationdays int not null default 0,
	domainquotawarningpercent int not null default -1,
	domainquotawarningsubject nvarchar(200) not null default '',
	domainquotawarningtext ntext not null default '',
	domainquotagracepercent int not null default -1,
	domainquotasendstoppercent int not null default -1,
	domainexternaltagsubject int not null default 0,
	domainexternaltagheader int not null default 0,
	domainexternaltagtext nvarchar(100) not null default '',
	domainfirstcontacttip int not null default 0,
	domaindisclaimerenabled int not null default 0,
	domaindisclaimerplaintext ntext not null,
	domaindisclaimerhtml ntext not null,
	domainquarantinedigest tinyint not null default 0,
	domainquarantinedigesthour int not null default 7,
	domaindkimrotationenabled int not null default 0,
	domaindkimrotationintervaldays int not null default 180,
	domaindkimrotationpropagationhours int not null default 72,
	domaindkimrotationretentiondays int not null default 7,
	domaindkimrotationkeybits int not null default 2048,
	domainopenpgppublish int not null default 0,
	domainopenpgpnewaccounts int not null default 0,
	domainwebhooksenabled int not null default 1,
	domainwebhooksubjects int not null default 0,
	domaincountrypolicy nvarchar(255) not null default '',
	domainsessionidleseconds int not null default 0,
	domainsessionlifetimeseconds int not null default 0,
	domainsessiononeperdevice int not null default 0,
	domainsessionbindaddress int not null default 0,
	domainreportmailbox nvarchar(255) not null default '',
	domainrestricteddomains nvarchar(2000) not null default ''
) 

ALTER TABLE hm_domains ADD CONSTRAINT hm_domains_pk PRIMARY KEY NONCLUSTERED (domainid) 

ALTER TABLE hm_domains ADD CONSTRAINT u_domainname UNIQUE NONCLUSTERED (domainname) 

CREATE CLUSTERED INDEX idx_hm_domains ON hm_domains (domainname)  

create table hm_domain_aliases (
	daid int identity (1, 1) not null,
	dadomainid integer not null ,
	daalias nvarchar(255) not null
) 

ALTER TABLE hm_domain_aliases ADD CONSTRAINT hm_domain_aliases_pk PRIMARY KEY NONCLUSTERED (daid) 

create table hm_messages (
	messageid bigint identity (1, 1) not null,
	messageaccountid int not null ,
	messagefolderid int not null ,
	messagefilename nvarchar(255) not null ,
	messagetype tinyint not null ,
	messagefrom nvarchar(255) not null ,
	messagesize bigint not null,
	messagecurnooftries int not null,
	messagenexttrytime datetime not null,
	messageflags smallint not null,
	messagecreatetime datetime not null,
	messagesavedate datetime not null,
	messageemailid nvarchar(48) not null,
	messagelocked tinyint not null,
   messageuid bigint not null,
   messagemodseq bigint not null,
   messagekeywords nvarchar(500) not null default ''
) 

ALTER TABLE hm_messages ADD CONSTRAINT hm_messages_pk PRIMARY KEY NONCLUSTERED (messageid) 

CREATE CLUSTERED INDEX idx_hm_messages ON hm_messages (messageaccountid, messagefolderid)  

CREATE INDEX idx_hm_messages_type ON hm_messages (messagetype) 

create table hm_message_metadata 
(
   metadata_id bigint identity (1, 1) not null,
   metadata_accountid int not null,
   metadata_folderid int not null,
   metadata_messageid bigint  not null,
   metadata_dateutc datetime null,
   metadata_from nvarchar(255) not null,
   metadata_subject nvarchar(255) not null,
   metadata_to nvarchar(255) not null,
   metadata_cc nvarchar(255) not null
)

ALTER TABLE hm_message_metadata ADD CONSTRAINT hm_message_metadata_pk PRIMARY KEY CLUSTERED (metadata_id) 

ALTER TABLE hm_message_metadata ADD CONSTRAINT hm_message_metadata_unique UNIQUE NONCLUSTERED (metadata_accountid, metadata_folderid, metadata_messageid)

CREATE NONCLUSTERED INDEX idx_message_metadata_id ON hm_message_metadata (metadata_messageid)

create table hm_settings (
	settingid int identity (1, 1) not null,
	settingname nvarchar(30) not null,
	settingstring nvarchar(4000) not null ,
	settinginteger int not null
) 

ALTER TABLE hm_settings ADD CONSTRAINT hm_settings_pk PRIMARY KEY NONCLUSTERED (settingid) 

ALTER TABLE hm_settings ADD CONSTRAINT u_settingname UNIQUE NONCLUSTERED (settingname)

create table hm_inisettings
(
	inisettingid int identity (1, 1) not null,
	inisettingname nvarchar(100) not null,
	inisettingvalue ntext not null,
	inisettingfilevalue ntext not null
)

ALTER TABLE hm_inisettings ADD CONSTRAINT hm_inisettings_pk PRIMARY KEY NONCLUSTERED (inisettingid)

ALTER TABLE hm_inisettings ADD CONSTRAINT u_inisettingname UNIQUE NONCLUSTERED (inisettingname)

create table hm_dbversion (
	value int not null
) 

create table hm_distributionlists 
(
	distributionlistid int identity (1, 1) not null,
	distributionlistdomainid int not null,
	distributionlistaddress nvarchar(255) not null,
	distributionlistenabled tinyint not null,	
   distributionlistrequireauth tinyint not null,
	distributionlistrequireaddress nvarchar(255) not null,
	distributionlistmode tinyint not null,
	distributionlistmoderatoraddress nvarchar(255) not null,
	distributionlistbounceaddress nvarchar(255) not null,
	distributionlistmembership nvarchar(200) not null default ''
) 

ALTER TABLE hm_distributionlists ADD CONSTRAINT hm_distributionlists_pk PRIMARY KEY NONCLUSTERED (distributionlistid) 

ALTER TABLE hm_distributionlists ADD CONSTRAINT u_distributionlistaddress UNIQUE NONCLUSTERED (distributionlistaddress) 

CREATE CLUSTERED INDEX idx_hm_distributionlists_distributionlistdomainid ON hm_distributionlists (distributionlistdomainid) 

create table hm_distributionlistsrecipients
(
	distributionlistrecipientid int identity (1, 1) not null,
	distributionlistrecipientlistid int not null,
	distributionlistrecipientaddress nvarchar(255) not null
) 

ALTER TABLE hm_distributionlistsrecipients ADD CONSTRAINT hm_distributionlistsrecipients_pk PRIMARY KEY NONCLUSTERED (distributionlistrecipientid) 

CREATE CLUSTERED INDEX idx_hm_distributionlistsrecipients_distributionlistrecipientlistid ON hm_distributionlistsrecipients (distributionlistrecipientlistid)  

create table hm_messagerecipients
(
	recipientid bigint identity (1, 1) not null,
   recipientmessageid bigint not null,
	recipientaddress nvarchar(255) not null,
	recipientlocalaccountid int not null,
	recipientoriginaladdress nvarchar(255) not null,
	recipientdsnnotify int not null default 0
) 

ALTER TABLE hm_messagerecipients ADD CONSTRAINT hm_messagerecipients_pk PRIMARY KEY NONCLUSTERED (recipientid) 

CREATE CLUSTERED INDEX idx_hm_messagerecipients_recipientmessageid ON hm_messagerecipients (recipientmessageid) 

create table hm_imapfolders 
(
	folderid int identity (1, 1) not null,
	folderaccountid int NOT NULL,
	folderparentid int NOT NULL,
	foldername nvarchar(255) NOT NULL,
	folderissubscribed tinyint NOT NULL,
   foldercreationtime datetime NOT NULL,
   foldercurrentuid bigint NOT NULL,
   foldercurrentmodseq bigint NOT NULL,
   folderspecialuse int NOT NULL DEFAULT 0
)

ALTER TABLE hm_imapfolders ADD CONSTRAINT hm_imapfolders_pk PRIMARY KEY NONCLUSTERED (folderid) 

CREATE CLUSTERED INDEX idx_hm_imapfolders_folderaccountid ON hm_imapfolders (folderaccountid)  

ALTER TABLE hm_imapfolders ADD CONSTRAINT idx_hm_imapfolders_unique UNIQUE NONCLUSTERED (folderaccountid, folderparentid, foldername)

create table hm_imapexpunged (
	expungedaccountid int not null,
	expungedfolderid int not null,
	expungeduid bigint not null,
	expungedmodseq bigint not null
) 

CREATE INDEX idx_hm_imapexpunged ON hm_imapexpunged (expungedfolderid, expungedmodseq)

create table hm_securityranges
(
	rangeid int identity (1, 1) not null,
  	rangepriorityid int not null,
	rangelowerip1 bigint not null,
   rangelowerip2 bigint null,
	rangeupperip1 bigint not null,
   rangeupperip2 bigint null,
	rangeoptions int not null,
	rangename nvarchar(100) not null,
   rangeexpires tinyint not null,
	rangeexpirestime datetime not null
) 

ALTER TABLE hm_securityranges ADD CONSTRAINT hm_securityranges_pk PRIMARY KEY NONCLUSTERED (rangeid) 

ALTER TABLE hm_securityranges ADD CONSTRAINT u_rangename UNIQUE NONCLUSTERED (rangename) 

create table hm_routes 
(
  routeid int identity (1, 1) not null,
  routedomainname nvarchar(255) NOT NULL,
  routedescription nvarchar(255) NOT NULL,
  routetargetsmthost nvarchar(255) NOT NULL,
  routetargetsmtport int NOT NULL,
  routenooftries int NOT NULL,
  routeminutesbetweentry int NOT NULL,
  routealladdresses tinyint NOT NULL,
  routeuseauthentication tinyint NOT NULL,
  routeauthenticationusername nvarchar(255) NOT NULL,
  routeauthenticationpassword nvarchar(1024) NOT NULL,
  routetreatsecurityaslocal tinyint NOT NULL,
  routeconnectionsecurity tinyint not null,
  routetreatsenderaslocaldomain tinyint NOT NULL
) 

ALTER TABLE hm_routes ADD CONSTRAINT hm_routes_pk PRIMARY KEY NONCLUSTERED (routeid) 

ALTER TABLE hm_routes ADD CONSTRAINT u_routedomainname UNIQUE NONCLUSTERED (routedomainname) 

create table hm_routeaddresses
(
  routeaddressid int identity (1, 1) not null ,
  routeaddressrouteid int NOT NULL,
  routeaddressaddress nvarchar(255) NOT NULL
) 

ALTER TABLE hm_routeaddresses ADD CONSTRAINT hm_routeaddresses_pk PRIMARY KEY NONCLUSTERED (routeaddressid) 

create table hm_dnsbl
(
	sblid int identity (1, 1) not null,
   sblactive int not null,
   sbldnshost nvarchar(255) not null,
   sblresult nvarchar(255) not null,
   sblrejectmessage nvarchar(255) not null,
	sblscore int not null
) 

ALTER TABLE hm_dnsbl ADD CONSTRAINT hm_dnsbl_pk PRIMARY KEY NONCLUSTERED (sblid) 

create table hm_fetchaccounts
(	
	faid int identity(1,1) not null,
	faactive tinyint not null,
	faaccountid int not null,
	faaccountname nvarchar(255) not null,
	faserveraddress nvarchar(255) not null,
	faserverport int not null,
	faservertype tinyint not null,
	fausername nvarchar(255) not null,
	fapassword nvarchar(1024) not null,
	faminutes int not null,
	fanexttry datetime not null,
	fadaystokeep int not null,
	falocked tinyint not null,
	faprocessmimerecipients tinyint not null,
	faprocessmimedate tinyint not null,
	faconnectionsecurity tinyint not null,
	fauseantispam tinyint not null,
	fauseantivirus tinyint not null,
	faenablerouterecipients tinyint not null,
	famirrorfolders tinyint not null DEFAULT 0,
	famimerecipientheaders nvarchar(255) not null DEFAULT 'To,CC,X-RCPT-TO,X-Envelope-To',
	faoauthprovider nvarchar(16) not null DEFAULT '',
	faoauthtoken ntext not null DEFAULT '',
	famirrorskip ntext not null DEFAULT '',
	famirrorseen ntext not null DEFAULT ''
) 

ALTER TABLE hm_fetchaccounts ADD CONSTRAINT hm_fetchaccounts_pk PRIMARY KEY NONCLUSTERED (faid) 

create table hm_fetchaccounts_uids
(
	uidid int identity(1,1) not null,
	uidfaid int not null,
	uidvalue nvarchar(255) not null,
	uidtime datetime not null
) 

ALTER TABLE hm_fetchaccounts_uids ADD CONSTRAINT hm_fetchaccounts_uids_pk PRIMARY KEY NONCLUSTERED (uidid) 

CREATE CLUSTERED INDEX idx_hm_fetchaccounts_uids ON hm_fetchaccounts_uids (uidfaid)  

create table hm_apppasswords
(
	apid int identity(1,1) not null,
	apaccountid int not null,
	apname nvarchar(255) not null,
	aphash nvarchar(255) not null,
	apencryption int not null,
	apcreated datetime not null,
	aplastused datetime null,
	apactive tinyint not null
) 

ALTER TABLE hm_apppasswords ADD CONSTRAINT hm_apppasswords_pk PRIMARY KEY NONCLUSTERED (apid) 

CREATE CLUSTERED INDEX idx_hm_apppasswords ON hm_apppasswords (apaccountid)  

create table hm_contacts
(
	contactid int identity(1,1) not null,
	contactaccountid int not null,
	contactname nvarchar(255) not null,
	contactaddress nvarchar(255) not null,
	contactsource tinyint not null,
	contactcreated datetime not null,
	contacturi nvarchar(255) not null default '',
	contactuid nvarchar(255) not null default '',
	contactvcard ntext not null default '',
	contactbookid int not null default 0
)

ALTER TABLE hm_contacts ADD CONSTRAINT hm_contacts_pk PRIMARY KEY NONCLUSTERED (contactid)

CREATE UNIQUE CLUSTERED INDEX idx_hm_contacts_account ON hm_contacts (contactaccountid, contactaddress)

CREATE INDEX idx_hm_contacts_book ON hm_contacts (contactbookid)

create table hm_addressbooks
(
	bookid int identity(1,1) not null,
	bookaccountid int not null,
	bookname nvarchar(255) not null,
	bookcreated bigint not null
)

ALTER TABLE hm_addressbooks ADD CONSTRAINT hm_addressbooks_pk PRIMARY KEY NONCLUSTERED (bookid)

CREATE UNIQUE CLUSTERED INDEX idx_hm_addressbooks_account ON hm_addressbooks (bookaccountid, bookname)

create table hm_addressbookshares
(
	shareid int identity(1,1) not null,
	sharebookid int not null,
	shareaccountid int not null,
	sharedomainid int not null,
	sharewrite int not null,
	sharecreated bigint not null
)

ALTER TABLE hm_addressbookshares ADD CONSTRAINT hm_addressbookshares_pk PRIMARY KEY NONCLUSTERED (shareid)

CREATE UNIQUE CLUSTERED INDEX idx_hm_addressbookshares_target ON hm_addressbookshares (sharebookid, shareaccountid, sharedomainid)

create table hm_accountprefs
(
	prefid int identity(1,1) not null,
	prefaccountid int not null,
	prefname nvarchar(64) not null,
	prefvalue nvarchar(4000) not null
)

ALTER TABLE hm_accountprefs ADD CONSTRAINT hm_accountprefs_pk PRIMARY KEY NONCLUSTERED (prefid)

CREATE UNIQUE CLUSTERED INDEX idx_hm_accountprefs_name ON hm_accountprefs (prefaccountid, prefname)

create table hm_scheduled
(
	schedid int identity(1,1) not null,
	schedaccountid int not null,
	schedmessageid bigint not null,
	schedaction tinyint not null,
	schedat datetime not null,
	schedfolderid bigint not null,
	schedcreated datetime not null
)

ALTER TABLE hm_scheduled ADD CONSTRAINT hm_scheduled_pk PRIMARY KEY NONCLUSTERED (schedid)

CREATE CLUSTERED INDEX idx_hm_scheduled_account ON hm_scheduled (schedaccountid)

create table hm_files
(
	fileid int identity(1,1) not null,
	fileaccountid int not null,
	filetoken varchar(64) not null,
	filename nvarchar(255) not null,
	filetype varchar(100) not null,
	filesize bigint not null,
	filestored bigint not null,
	filecomplete tinyint not null,
	filecreated bigint not null,
	fileexpires bigint not null,
	filepasswordhash varchar(64) not null,
	filedownloads int not null
)

ALTER TABLE hm_files ADD CONSTRAINT hm_files_pk PRIMARY KEY NONCLUSTERED (fileid)

CREATE CLUSTERED INDEX idx_hm_files_account ON hm_files (fileaccountid)

CREATE UNIQUE INDEX idx_hm_files_token ON hm_files (filetoken)

create table hm_smimekeys
(
	smimeid int identity(1,1) not null,
	smimeaccountid int not null,
	smimekind tinyint not null,
	smimeaddress nvarchar(255) not null,
	smimename nvarchar(255) not null,
	smimefingerprint varchar(64) not null,
	smimecertificate ntext not null,
	smimechain ntext not null,
	smimekey ntext not null,
	smimenotafter bigint not null,
	smimecreated bigint not null
)

ALTER TABLE hm_smimekeys ADD CONSTRAINT hm_smimekeys_pk PRIMARY KEY NONCLUSTERED (smimeid)

CREATE CLUSTERED INDEX idx_hm_smimekeys_account ON hm_smimekeys (smimeaccountid)

CREATE UNIQUE INDEX idx_hm_smimekeys_entry ON hm_smimekeys (smimeaccountid, smimekind, smimefingerprint)

create table hm_calendars
(
	calendarid int identity(1,1) not null,
	calendaraccountid int not null,
	calendarname nvarchar(255) not null,
	calendardisplayname nvarchar(255) not null,
	calendarsynctoken bigint not null,
	calendarcreated bigint not null,
	calendarcolor nvarchar(16) not null default ''
)

ALTER TABLE hm_calendars ADD CONSTRAINT hm_calendars_pk PRIMARY KEY NONCLUSTERED (calendarid)

CREATE UNIQUE CLUSTERED INDEX idx_hm_calendars_account ON hm_calendars (calendaraccountid, calendarname)

create table hm_calendarobjects
(
	objectid int identity(1,1) not null,
	objectaccountid int not null,
	objectcalendarid int not null,
	objecturi nvarchar(255) not null,
	objectuid nvarchar(255) not null,
	objectcomponent nvarchar(16) not null,
	objectdata ntext not null,
	objectetag nvarchar(64) not null,
	objectsynctoken bigint not null,
	objectstart bigint not null,
	objectend bigint not null,
	objectfirst bigint not null,
	objectlast bigint not null,
	objectdeleted tinyint not null,
	objectmodified bigint not null
)

ALTER TABLE hm_calendarobjects ADD CONSTRAINT hm_calendarobjects_pk PRIMARY KEY NONCLUSTERED (objectid)

CREATE UNIQUE CLUSTERED INDEX idx_hm_calendarobjects_uri ON hm_calendarobjects (objectcalendarid, objecturi)

CREATE INDEX idx_hm_calendarobjects_sync ON hm_calendarobjects (objectcalendarid, objectsynctoken)

create table hm_calendarshares
(
	shareid int identity(1,1) not null,
	sharecalendarid int not null,
	shareaccountid int not null,
	sharewrite int not null
)

ALTER TABLE hm_calendarshares ADD CONSTRAINT hm_calendarshares_pk PRIMARY KEY NONCLUSTERED (shareid)

CREATE UNIQUE CLUSTERED INDEX idx_hm_calendarshares_pair ON hm_calendarshares (sharecalendarid, shareaccountid)

create table hm_senderaddresses
(
	said int identity(1,1) not null,
	saaccountid int not null,
	saaddress nvarchar(255) not null,
	saname nvarchar(255) not null,
	saverified tinyint not null,
	sacreated datetime not null
)

ALTER TABLE hm_senderaddresses ADD CONSTRAINT hm_senderaddresses_pk PRIMARY KEY NONCLUSTERED (said)

CREATE UNIQUE CLUSTERED INDEX idx_hm_senderaddresses_address ON hm_senderaddresses (saaccountid, saaddress)

create table hm_maskedaddresses
(
	maid int identity(1,1) not null,
	maaccountid int not null,
	maaddress nvarchar(255) not null,
	malabel nvarchar(255) not null,
	maactive tinyint not null,
	mareceived int not null,
	macreated datetime not null
)

ALTER TABLE hm_maskedaddresses ADD CONSTRAINT hm_maskedaddresses_pk PRIMARY KEY NONCLUSTERED (maid)

CREATE UNIQUE CLUSTERED INDEX idx_hm_maskedaddresses_address ON hm_maskedaddresses (maaddress)

CREATE INDEX idx_hm_maskedaddresses_account ON hm_maskedaddresses (maaccountid)

create table hm_passkeys
(
	pkid int identity(1,1) not null,
	pkaccountid int null,
	pkcredential nvarchar(200) not null,
	pkpublickey ntext not null,
	pkalgorithm int not null,
	pksigncount bigint not null,
	pkname nvarchar(255) not null,
	pktransports nvarchar(255) not null,
	pkaaguid nvarchar(64) not null,
	pkcreated bigint not null,
	pklastused bigint not null
)

ALTER TABLE hm_passkeys ADD CONSTRAINT hm_passkeys_pk PRIMARY KEY NONCLUSTERED (pkid)

CREATE CLUSTERED INDEX idx_hm_passkeys_account ON hm_passkeys (pkaccountid)

CREATE UNIQUE INDEX idx_hm_passkeys_credential ON hm_passkeys (pkcredential)

create table hm_directory
(
	dirid int identity(1,1) not null,
	diraccountid int not null,
	dirtitle nvarchar(100) not null default '',
	dirdepartment nvarchar(100) not null default '',
	dirphone nvarchar(60) not null default '',
	dirmobile nvarchar(60) not null default '',
	diroffice nvarchar(100) not null default '',
	dirphototype nvarchar(40) not null default '',
	dirphoto ntext not null default '',
	diroptout tinyint not null default 0,
	dirnote nvarchar(255) not null default '',
	dirupdated datetime not null
)

ALTER TABLE hm_directory ADD CONSTRAINT hm_directory_pk PRIMARY KEY NONCLUSTERED (dirid)

CREATE UNIQUE CLUSTERED INDEX idx_hm_directory_account ON hm_directory (diraccountid)

create table hm_recoverable
(
	rcid int identity(1,1) not null,
	rcaccountid int not null,
	rcfolderid int not null,
	rcfolder nvarchar(255) not null,
	rcfile nvarchar(255) not null,
	rcsize int not null,
	rcsubject nvarchar(255) not null,
	rcfrom nvarchar(255) not null,
	rcflags int not null,
	rcdeleted bigint not null
)

ALTER TABLE hm_recoverable ADD CONSTRAINT hm_recoverable_pk PRIMARY KEY NONCLUSTERED (rcid)

CREATE CLUSTERED INDEX idx_hm_recoverable_account ON hm_recoverable (rcaccountid)

create table hm_confidential
(
	cfid int identity(1,1) not null,
	cfaccountid int not null,
	cftoken nvarchar(64) not null,
	cfsubject nvarchar(255) not null,
	cfsender nvarchar(255) not null,
	cfrecipients nvarchar(2000) not null,
	cffile nvarchar(255) not null,
	cfpasscode nvarchar(255) not null,
	cfcreated bigint not null,
	cfexpires bigint not null,
	cfopened bigint not null,
	cfopenedby nvarchar(255) not null,
	cfrevoked int not null
)

ALTER TABLE hm_confidential ADD CONSTRAINT hm_confidential_pk PRIMARY KEY NONCLUSTERED (cfid)

CREATE UNIQUE CLUSTERED INDEX idx_hm_confidential_token ON hm_confidential (cftoken)

CREATE INDEX idx_hm_confidential_account ON hm_confidential (cfaccountid)

create table hm_dmarcreports
(
	drid int identity(1,1) not null,
	drdomainid int not null,
	drkey nvarchar(64) not null,
	drorgname nvarchar(255) not null,
	dremail nvarchar(255) not null,
	drreportid nvarchar(255) not null,
	drbegin bigint not null,
	drend bigint not null,
	drpolicydomain nvarchar(255) not null,
	dradkim nvarchar(8) not null,
	draspf nvarchar(8) not null,
	drp nvarchar(16) not null,
	drsp nvarchar(16) not null,
	drpct int not null,
	drmessages int not null,
	drpassed int not null,
	drreceived bigint not null,
	drsource nvarchar(255) not null,
	drrecords int not null,
	drmsgdmarc nvarchar(16) not null,
	drmsgfrom nvarchar(255) not null
)

ALTER TABLE hm_dmarcreports ADD CONSTRAINT hm_dmarcreports_pk PRIMARY KEY NONCLUSTERED (drid)

CREATE UNIQUE CLUSTERED INDEX idx_hm_dmarcreports_key ON hm_dmarcreports (drkey)

CREATE INDEX idx_hm_dmarcreports_domain ON hm_dmarcreports (drdomainid)

CREATE INDEX idx_hm_dmarcreports_received ON hm_dmarcreports (drreceived)

create table hm_dmarcreportrecords
(
	rrid int identity(1,1) not null,
	rrreportid int not null,
	rrsourceip nvarchar(45) not null,
	rrcount int not null,
	rrdisposition nvarchar(16) not null,
	rrdkim nvarchar(8) not null,
	rrspf nvarchar(8) not null,
	rrheaderfrom nvarchar(255) not null,
	rrenvelopefrom nvarchar(255) not null,
	rrdkimauth nvarchar(500) not null,
	rrspfauth nvarchar(255) not null
)

ALTER TABLE hm_dmarcreportrecords ADD CONSTRAINT hm_dmarcreportrecords_pk PRIMARY KEY NONCLUSTERED (rrid)

CREATE CLUSTERED INDEX idx_hm_dmarcreportrecords_report ON hm_dmarcreportrecords (rrreportid)

create table hm_tlsreports
(
	trid int identity(1,1) not null,
	trdomainid int not null,
	trkey nvarchar(64) not null,
	trorgname nvarchar(255) not null,
	trcontact nvarchar(255) not null,
	trreportid nvarchar(255) not null,
	trbegin bigint not null,
	trend bigint not null,
	trpolicytype nvarchar(16) not null,
	trpolicydomain nvarchar(255) not null,
	trsuccess int not null,
	trfailure int not null,
	trfailures nvarchar(2000) not null,
	trreceived bigint not null,
	trsource nvarchar(255) not null
)

ALTER TABLE hm_tlsreports ADD CONSTRAINT hm_tlsreports_pk PRIMARY KEY NONCLUSTERED (trid)

CREATE UNIQUE CLUSTERED INDEX idx_hm_tlsreports_key ON hm_tlsreports (trkey)

CREATE INDEX idx_hm_tlsreports_domain ON hm_tlsreports (trdomainid)

CREATE INDEX idx_hm_tlsreports_received ON hm_tlsreports (trreceived)

create table hm_accountactivity
(
	acid bigint identity(1,1) not null,
	acaccountid int not null,
	actime bigint not null,
	acbucket bigint not null,
	achour int not null,
	acrecipients int not null,
	acnewrecipients int not null,
	acaddress nvarchar(64) not null,
	acsource int not null,
	acmessageid bigint not null,
	acheld int not null
)

ALTER TABLE hm_accountactivity ADD CONSTRAINT hm_accountactivity_pk PRIMARY KEY (acid)

CREATE INDEX idx_hm_accountactivity_account ON hm_accountactivity (acaccountid, actime)

create table hm_accountrecipients
(
	arcid bigint identity(1,1) not null,
	arcaccountid int not null,
	arcaddress nvarchar(255) not null,
	arcfirst bigint not null,
	arclast bigint not null,
	arccount int not null
)

ALTER TABLE hm_accountrecipients ADD CONSTRAINT hm_accountrecipients_pk PRIMARY KEY (arcid)

CREATE INDEX idx_hm_accountrecipients_address ON hm_accountrecipients (arcaccountid, arcaddress)

create table hm_accountreview
(
	arvid int identity(1,1) not null,
	arvaccountid int not null,
	arvexempt int not null,
	arvsensitivity int not null,
	arvlocked int not null,
	arvlockedat bigint not null,
	arvunlockedat bigint not null,
	arvalertedat bigint not null,
	arvscore int not null,
	arvsignals nvarchar(255) not null,
	arvreason nvarchar(255) not null
)

ALTER TABLE hm_accountreview ADD CONSTRAINT hm_accountreview_pk PRIMARY KEY (arvid)

CREATE UNIQUE INDEX idx_hm_accountreview_account ON hm_accountreview (arvaccountid)

create table hm_heldmessages
(
	hmid bigint identity(1,1) not null,
	hmaccountid int not null,
	hmmessageid bigint not null,
	hmheld bigint not null,
	hmsource int not null,
	hmsender nvarchar(255) not null,
	hmrecipients nvarchar(2000) not null,
	hmsubject nvarchar(255) not null,
	hmsize bigint not null,
	hmreason nvarchar(255) not null
)

ALTER TABLE hm_heldmessages ADD CONSTRAINT hm_heldmessages_pk PRIMARY KEY (hmid)

CREATE INDEX idx_hm_heldmessages_account ON hm_heldmessages (hmaccountid)

CREATE INDEX idx_hm_heldmessages_message ON hm_heldmessages (hmmessageid)

create table hm_remotedomainpolicies
(
	policyid int identity(1,1) not null,
	policydomainname nvarchar(255) not null,
	policydescription nvarchar(255) not null,
	policyactive tinyint not null,
	policyoutboundtls int not null,
	policyinboundtls tinyint not null,
	policymaxmessagesizekb int not null,
	policymaxconnections int not null,
	policymaxperminute int not null,
	policyallowreplies tinyint not null,
	policyallowforwarding tinyint not null,
	policycalloutenabled tinyint not null,
	policycallouthost nvarchar(255) not null,
	policycalloutport int not null,
	policycallouttimeout int not null,
	policycalloutcacheminutes int not null,
	policycalloutperminute int not null
)

ALTER TABLE hm_remotedomainpolicies ADD CONSTRAINT hm_remotedomainpolicies_pk PRIMARY KEY NONCLUSTERED (policyid)

CREATE INDEX idx_hm_remotedomainpolicies_domain ON hm_remotedomainpolicies (policydomainname)

create table hm_rules
(
	ruleid int identity(1,1) not null,
	ruleaccountid int not null,
	ruledomainid int not null default 0,
	rulename nvarchar(100) not null,
	ruleactive tinyint not null,
	ruleuseand tinyint not null,
	rulesortorder int not null
) 

ALTER TABLE hm_rules ADD CONSTRAINT hm_rules_pk PRIMARY KEY NONCLUSTERED (ruleid) 

CREATE CLUSTERED INDEX idx_hm_rules ON hm_rules (ruleaccountid)  

create table hm_rule_criterias
(
	criteriaid int identity(1,1) not null,
	criteriaruleid int not null,
	criteriausepredefined tinyint not null,
	criteriapredefinedfield tinyint not null,
	criteriaheadername nvarchar(255) not null,
	criteriamatchtype tinyint not null,
	criteriamatchvalue nvarchar(2000) not null
	
) 

ALTER TABLE hm_rule_criterias ADD CONSTRAINT hm_rule_criterias_pk PRIMARY KEY NONCLUSTERED (criteriaid) 

CREATE CLUSTERED INDEX idx_hm_rule_criterias ON hm_rule_criterias (criteriaruleid)  

create table hm_rule_actions
(
	actionid int identity(1,1) not null,
	actionruleid int not null,
	actiontype tinyint not null,
	actionimapfolder nvarchar(255) not null,
	actionsubject nvarchar(255) not null,
	actionfromname nvarchar(255) not null,
	actionfromaddress nvarchar(255) not null,
	actionto nvarchar(255) not null,
	actionbody ntext not null,
	actionfilename nvarchar(255) not null,
	actionsortorder int not null,
	actionscriptfunction nvarchar(255) not null,
	actionheader nvarchar(80) not null,
	actionvalue nvarchar(255) not null,
	actionrouteid int not null,
	actionabortspamflagged tinyint not null
) 

ALTER TABLE hm_rule_actions ADD CONSTRAINT hm_rule_actions_pk PRIMARY KEY NONCLUSTERED (actionid) 

CREATE CLUSTERED INDEX idx_hm_rule_actions ON hm_rule_actions (actionruleid) 

create table hm_surblservers
(
	surblid int identity(1,1) not null ,
	surblactive tinyint not null,
	surblhost nvarchar(255) not null,
	surblrejectmessage nvarchar(255) not null,
	surblresult nvarchar(255) not null default '',
	surblscore int not null
) 

ALTER TABLE hm_surblservers ADD CONSTRAINT hm_surblid_pk PRIMARY KEY NONCLUSTERED (surblid) 

create table hm_greylisting_triplets
(
	glid bigint identity(1,1) not null,
	glcreatetime datetime not null,
	glblockendtime datetime not null,
	gldeletetime datetime not null,
	glipaddress1 bigint not null,
   glipaddress2 bigint null,
	glsenderaddress nvarchar(320) not null,
	glrecipientaddress nvarchar(320) not null,
	glblockedcount int not null,
	glpassedcount int not null,
	glsenderhash nvarchar(32) null,
	glrecipienthash nvarchar(32) null
) 

ALTER TABLE hm_greylisting_triplets ADD CONSTRAINT hm_glid_pk PRIMARY KEY NONCLUSTERED (glid) 

CREATE INDEX idx_hm_greylisting_triplets_key ON hm_greylisting_triplets (glipaddress1, glipaddress2, glsenderhash, glrecipienthash)

create table hm_greylisting_whiteaddresses
(
	whiteid bigint identity(1,1) not null,
	whiteipaddress nvarchar(255) not null,
	whiteipdescription nvarchar(255) not null
) 

ALTER TABLE hm_greylisting_whiteaddresses ADD CONSTRAINT hm_glwhite_pk PRIMARY KEY NONCLUSTERED (whiteid) 

ALTER TABLE hm_greylisting_whiteaddresses ADD CONSTRAINT u_glwhite UNIQUE NONCLUSTERED (whiteipaddress) 

create table hm_greylisting_bypass
(
	glbid bigint identity(1,1) not null,
	glbpattern nvarchar(255) not null,
	glbmatch int not null,
	glbrequire int not null,
	glbnote nvarchar(255) not null
)

ALTER TABLE hm_greylisting_bypass ADD CONSTRAINT hm_greylisting_bypass_pk PRIMARY KEY (glbid)

create table hm_blocked_attachments
(
	baid bigint identity(1,1) not null,
	bawildcard nvarchar(255) not null,
	badescription nvarchar(255) not null
) 

ALTER TABLE hm_blocked_attachments ADD CONSTRAINT hm_baid_pk PRIMARY KEY NONCLUSTERED (baid) 

create table hm_servermessages
(
	smid int identity(1,1) not null , 
	smname nvarchar(255) not null,
	smtext ntext not null
) 

ALTER TABLE hm_servermessages ADD CONSTRAINT hm_smid_pk PRIMARY KEY NONCLUSTERED (smid) 

create table hm_tcpipports
(
	portid int identity(1,1) not null, 
	portprotocol tinyint not null,
	portnumber int not null,
	portaddress1 bigint not null,
    portaddress2 bigint null,
	portconnectionsecurity tinyint not null,
	portsslcertificateid bigint not null,
	portclientcertificatepolicy tinyint not null default 0,
	portclientcertificatecafile nvarchar(255) not null default ''
) 

ALTER TABLE hm_tcpipports ADD CONSTRAINT hm_tcpipports_pk PRIMARY KEY NONCLUSTERED (portid) 

create table hm_whitelist
(
	whiteid bigint identity(1,1) not null,
	whiteloweripaddress1 bigint not null,
	whiteloweripaddress2 bigint null,
	whiteupperipaddress1 bigint not null,
	whiteupperipaddress2 bigint null,
	whiteemailaddress nvarchar(255) not null,
	whitedescription nvarchar(255) not null
) 

create table hm_blocked_senders
(
	bsid bigint identity(1,1) not null,
	bsaddress nvarchar(255) not null,
	bsscore int not null,
	bsdescription nvarchar(255) not null
)

ALTER TABLE hm_blocked_senders ADD CONSTRAINT hm_bsid_pk PRIMARY KEY NONCLUSTERED (bsid)

CREATE UNIQUE INDEX u_bsaddress ON hm_blocked_senders (bsaddress)

ALTER TABLE hm_whitelist ADD CONSTRAINT hm_whitelist_pk PRIMARY KEY NONCLUSTERED (whiteid) 

create table hm_sslcertificates
(
	sslcertificateid bigint identity(1,1) not null,
	sslcertificatename nvarchar(255) not null,
	sslcertificatefile nvarchar(255) not null,
	sslprivatekeyfile nvarchar(255) not null,
	sslprivatekeypassword nvarchar(1024) not null
) 

ALTER TABLE hm_sslcertificates ADD CONSTRAINT hm_sslcertificates_pk PRIMARY KEY NONCLUSTERED (sslcertificateid) 

create table hm_groups
(
	groupid bigint identity(1,1) not null,
   groupname nvarchar(255)
) 

ALTER TABLE hm_groups ADD CONSTRAINT hm_groups_pk PRIMARY KEY NONCLUSTERED (groupid) 

create table hm_group_members
(
	memberid bigint identity(1,1) not null,
	membergroupid bigint not null,
	memberaccountid bigint not null
) 

ALTER TABLE hm_group_members ADD CONSTRAINT hm_group_members_pk PRIMARY KEY NONCLUSTERED (memberid) 

create table hm_acl
(
	aclid bigint identity(1,1) not null,
   aclsharefolderid bigint not null,
	aclpermissiontype tinyint not null,
	aclpermissiongroupid bigint not null,
	aclpermissionaccountid bigint not null,
	aclvalue bigint not null
) 

ALTER TABLE hm_acl ADD CONSTRAINT hm_acl_pk PRIMARY KEY NONCLUSTERED (aclid) 

ALTER TABLE hm_acl ADD CONSTRAINT hm_acl_unique UNIQUE (aclsharefolderid, aclpermissiontype, aclpermissiongroupid, aclpermissionaccountid) 

create table hm_incoming_relays
(
   relayid int identity(1,1) not null,
   relayname nvarchar(100) not null,
   relaylowerip1 bigint not null,
   relaylowerip2 bigint null,
   relayupperip1 bigint not null,
   relayupperip2 bigint null
) 

ALTER TABLE hm_incoming_relays ADD CONSTRAINT hm_incoming_relays_pk PRIMARY KEY NONCLUSTERED (relayid)

create table hm_logon_failures
(
   ipaddress1 bigint not null,
   ipaddress2 bigint null,
   failuretime datetime not null
) 

CREATE NONCLUSTERED INDEX idx_hm_logon_failures_ipaddress ON hm_logon_failures (ipaddress1, ipaddress2) 

CREATE CLUSTERED INDEX idx_hm_logon_failures_failuretime ON hm_logon_failures (failuretime) 

insert into hm_securityranges (rangepriorityid, rangelowerip1, rangelowerip2, rangeupperip1, rangeupperip2, rangeoptions, rangename, rangeexpires, rangeexpirestime) values (10, 0, NULL, 4294967295, NULL, 96203, 'Internet', 0, '2001-01-01') 

insert into hm_securityranges (rangepriorityid, rangelowerip1, rangelowerip2, rangeupperip1, rangeupperip2, rangeoptions, rangename, rangeexpires, rangeexpirestime) values (30, 2130706433, NULL, 2130706433, NULL, 71627, 'My computer', 0, '2001-01-01') 

insert into hm_servermessages (smname, smtext) values ('VIRUS_FOUND', 'Virus found')  

insert into hm_servermessages (smname, smtext) values ('VIRUS_ATTACHMENT_REMOVED', 'Virus found:' + NCHAR(13) + NCHAR(10) + 'The attachment(s) of this message was removed since a virus was detected in at least one of them.' + NCHAR(13) + NCHAR(10) + NCHAR(13) + NCHAR(10)) 

insert into hm_servermessages (smname, smtext) values ('VIRUS_NOTIFICATION', 'The message below contained a virus and did not' + NCHAR(13) + NCHAR(10) + 'reach some or all of the intended recipients.' + NCHAR(13) + NCHAR(10) + '' + NCHAR(13) + NCHAR(10) + '   From: %MACRO_FROM%' + NCHAR(13) + NCHAR(10) + '   To: %MACRO_TO%' + NCHAR(13) + NCHAR(10) + '   Sent: %MACRO_SENT%' + NCHAR(13) + NCHAR(10) + '   Subject: %MACRO_SUBJECT%' + NCHAR(13) + NCHAR(10) + '' + NCHAR(13) + NCHAR(10) + 'hMailServer' + NCHAR(13) + NCHAR(10) + '')  

insert into hm_servermessages (smname, smtext) values ('SEND_FAILED_NOTIFICATION', 'Your message did not reach some or all of the intended recipients.' + NCHAR(13) + NCHAR(10) + '' + NCHAR(13) + NCHAR(10) + '   Sent: %MACRO_SENT%' + NCHAR(13) + NCHAR(10) + '   Subject: %MACRO_SUBJECT%' + NCHAR(13) + NCHAR(10) + '' + NCHAR(13) + NCHAR(10) + 'The following recipient(s) could not be reached:' + NCHAR(13) + NCHAR(10) + '' + NCHAR(13) + NCHAR(10) + '%MACRO_RECIPIENTS%' + NCHAR(13) + NCHAR(10) + '' + NCHAR(13) + NCHAR(10) + 'hMailServer' + NCHAR(13) + NCHAR(10) + '')  

insert into hm_servermessages (smname, smtext) values ('MESSAGE_UNDELIVERABLE', 'Message undeliverable')  

insert into hm_servermessages (smname, smtext) values ('MESSAGE_FILE_MISSING', 'The mail server could not deliver the message to you since the file %MACRO_FILE% does not exist on the server.' + NCHAR(13) + NCHAR(10) + '' + NCHAR(13) + NCHAR(10) + 'The file may have been deleted by anti virus software running on the server.' + NCHAR(13) + NCHAR(10) + '' + NCHAR(13) + NCHAR(10) + 'hMailServer')  

insert into hm_servermessages (smname, smtext) values ('ATTACHMENT_REMOVED', 'The attachment %MACRO_FILE% was blocked for delivery by the e-mail server. Please contact your system administrator if you have any questions regarding this.' + NCHAR(13) + NCHAR(10) + '' + NCHAR(13) + NCHAR(10) + 'hMailServer' + NCHAR(13) + NCHAR(10) + '')  

insert into hm_dnsbl (sblactive, sbldnshost, sblresult, sblrejectmessage, sblscore) values (0, 'zen.spamhaus.org', '127.0.0.2-8|127.0.0.10-11', 'Rejected by Spamhaus.',3 ) 

insert into hm_dnsbl (sblactive, sbldnshost, sblresult, sblrejectmessage, sblscore) values (0, 'bl.spamcop.net', '127.0.0.2', 'Rejected by SpamCop.', 3) 

insert into hm_surblservers (surblactive, surblhost, surblrejectmessage, surblscore) values (0, 'multi.surbl.org', 'Rejected by SURBL.', 3) 

insert into hm_blocked_attachments (bawildcard, badescription) values ('*.bat', 'Batch processing file') 

insert into hm_blocked_attachments (bawildcard, badescription) values ('*.cmd', 'Command file for Windows NT') 

insert into hm_blocked_attachments (bawildcard, badescription) values ('*.com', 'Command') 

insert into hm_blocked_attachments (bawildcard, badescription) values ('*.cpl', 'Windows Control Panel extension') 

insert into hm_blocked_attachments (bawildcard, badescription) values ('*.csh', 'CSH script') 

insert into hm_blocked_attachments (bawildcard, badescription) values ('*.exe', 'Executable file') 

insert into hm_blocked_attachments (bawildcard, badescription) values ('*.inf', 'Setup file') 

insert into hm_blocked_attachments (bawildcard, badescription) values ('*.lnk', 'Windows link file') 

insert into hm_blocked_attachments (bawildcard, badescription) values ('*.msi', 'Windows Installer file') 

insert into hm_blocked_attachments (bawildcard, badescription) values ('*.msp', 'Windows Installer patch') 

insert into hm_blocked_attachments (bawildcard, badescription) values ('*.pif', 'Program Information file') 

insert into hm_blocked_attachments (bawildcard, badescription) values ('*.reg', 'Registration key') 

insert into hm_blocked_attachments (bawildcard, badescription) values ('*.scf', 'Windows Explorer command') 

insert into hm_blocked_attachments (bawildcard, badescription) values ('*.scr', 'Windows Screen saver') 


insert into hm_settings (settingname, settingstring, settinginteger) values ('maxpop3connections', '', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('maxsmtpconnections', '', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('mirroremailaddress', '', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('relaymode', '', 2) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('authallowplaintext', '', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('allowmailfromnull', '', 1) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('logging', '', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('logdevice', '', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('smtpnoofretries', '', 4) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('smtpminutesbetweenretries', '', 60) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('smtpnooftries', '', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('protocolimap', '', 1) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('protocolsmtp', '', 1) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('protocolpop3', '', 1) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('welcomeimap', '', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('welcomepop3', '', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('welcomesmtp', '', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('smtprelayer', '', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('maxdelivertythreads', '', 10) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('logformat', '', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('avclamwinenable', '', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('avclamwinexec', '', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('avclamwindb', '', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('avnotifysender', '', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('avnotifyreceiver', '', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('avaction', '', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('sendstatistics', '', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('hostname', '', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('smtprelayerusername', '', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('smtprelayerpassword', '', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('usesmtprelayerauthentication', '', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('smtprelayerport', '', 25) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('usecustomvirusscanner', '', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('customvirusscannerexecutable', '', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('customviursscannerreturnvalue', '', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('usespf', '', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('usemxchecks', '', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('usescriptserver', '', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('scriptlanguage', 'VBScript', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('maxmessagesize', '', 20480)  

insert into hm_settings (settingname, settingstring, settinginteger) values ('usecache', '', 1) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('domaincachettl', '', 60)  

insert into hm_settings (settingname, settingstring, settinginteger) values ('accountcachettl', '', 60) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('awstatsenabled', '', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('rulelooplimit', '', 5) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('backupoptions', '', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('backupdestination', '', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('backupencryptionpassphrase', '', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('defaultdomain', '', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('avmaxmsgsize', '', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('smtpdeliverybindtoip', '', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('enableimapquota', '', 1) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('enableimapidle', '', 1) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('enableimapacl', '', 1) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('maximapconnections', '', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('enableimapsort', '', 1) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('workerthreadpriority', '', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('ascheckhostinhelo', '', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('tcpipthreads', '', 15) 

-- Secure default: reject messages with incorrect (bare LF) line endings to harden against SMTP smuggling.
insert into hm_settings (settingname, settingstring, settinginteger) values ('smtpallowincorrectlineendings', '', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('usegreylisting', '', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('greylistinginitialdelay', '', 30) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('greylistinginitialdelete', '', 24) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('greylistingfinaldelete', '', 864) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('antispamaddheaderspam', '', 1) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('antispamaddheaderreason', '', 1) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('antispamprependsubject', '', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('antispamprependsubjecttext', '[SPAM]', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('enableattachmentblocking', '', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('maxsmtprecipientsinbatch', '', 100) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('disconnectinvalidclients', '', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('maximumincorrectcommands', '', 100) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('aliascachettl', '', 60) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('distributionlistcachettl', '', 60) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('smtprelayerconnectionsecurity', '', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('adddeliveredtoheader', '', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('groupcachettl', '', 60) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('imappublicfoldername', '#Public', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('antispamenabled', '', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('usespfscore', '', 3) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('ascheckhostinheloscore', '', 2) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('usemxchecksscore', '', 2) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('spammarkthreshold', '', 5) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('spamdeletethreshold', '', 20) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('spamassassinenabled', '', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('spamassassinscore', '', 5) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('spamassassinmergescore', '', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('spamassassinhost', '127.0.0.1', 0) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('spamassassinport', '', 783) 

insert into hm_settings (settingname, settingstring, settinginteger) values ('antispammaxsize', '', 1024)

insert into hm_settings (settingname, settingstring, settinginteger) values ('ASDKIMVerificationEnabled', '', 0)

insert into hm_settings (settingname, settingstring, settinginteger) values ('ASDKIMVerificationFailureScore', '', 5)

insert into hm_settings (settingname, settingstring, settinginteger) values ('ASDMARCEnabled', '', 1)

insert into hm_settings (settingname, settingstring, settinginteger) values ('ASArcFilteringEnabled', '', 0)

insert into hm_settings (settingname, settingstring, settinginteger) values ('QuarantineDigestBaseUrl', '', 0)

insert into hm_settings (settingname, settingstring, settinginteger) values ('QuarantineDigestLinkHours', '', 72)

insert into hm_settings (settingname, settingstring, settinginteger) values ('GeoIpDatabaseFile', '', 0)

insert into hm_settings (settingname, settingstring, settinginteger) values ('GeoIpDatabaseMaxAgeDays', '', 45)

insert into hm_settings (settingname, settingstring, settinginteger) values ('CountryBlockingExemptRanges', '127.0.0.0/8,::1/128,10.0.0.0/8,172.16.0.0/12,192.168.0.0/16,169.254.0.0/16,100.64.0.0/10,fe80::/10,fc00::/7', 0)

insert into hm_settings (settingname, settingstring, settinginteger) values ('CountryPolicySMTP', '', 0)

insert into hm_settings (settingname, settingstring, settinginteger) values ('CountryPolicyIMAP', '', 0)

insert into hm_settings (settingname, settingstring, settinginteger) values ('CountryPolicyPOP3', '', 0)

insert into hm_settings (settingname, settingstring, settinginteger) values ('CountryPolicyHTTP', '', 0)

insert into hm_settings (settingname, settingstring, settinginteger) values ('SessionIdleSeconds', '', 0)

insert into hm_settings (settingname, settingstring, settinginteger) values ('SessionLifetimeSeconds', '', 0)

insert into hm_settings (settingname, settingstring, settinginteger) values ('SessionOnePerDevice', '', 0)

insert into hm_settings (settingname, settingstring, settinginteger) values ('SessionBindToAddress', '', 0)

insert into hm_settings (settingname, settingstring, settinginteger) values ('ReputationWatchEnabled', '', 1)

insert into hm_settings (settingname, settingstring, settinginteger) values ('ReputationWatchLists', 'ipv4:zen.spamhaus.org, ipv4:bl.spamcop.net, ipv4:psbl.surriel.com, ipv4:bl.mailspike.net', 0)

insert into hm_settings (settingname, settingstring, settinginteger) values ('ReputationWatchAddresses', '', 0)

insert into hm_settings (settingname, settingstring, settinginteger) values ('SpamTrapBanMinutes', '', 1440)

insert into hm_settings (settingname, settingstring, settinginteger) values ('SpamTrapBlockDays', '', 30)

insert into hm_settings (settingname, settingstring, settinginteger) values ('SpamTrapHitRetentionDays', '', 90)

insert into hm_settings (settingname, settingstring, settinginteger) values ('SpamTrapTeachSpamAssassin', '', 0)

insert into hm_settings (settingname, settingstring, settinginteger) values ('GreyListingSlideOnEarlyRetry', '', 0)

insert into hm_settings (settingname, settingstring, settinginteger) values ('GreyListingAbuseThreshold', '', 0)

insert into hm_settings (settingname, settingstring, settinginteger) values ('GreyListingAbuseLockMinutes', '', 60)

insert into hm_settings (settingname, settingstring, settinginteger) values ('ASArcTrustedSealers', '', 0)

insert into hm_settings (settingname, settingstring, settinginteger) values ('ASDMARCFailureScore', '', 5)

insert into hm_settings (settingname, settingstring, settinginteger) values ('AutoBanOnLogonFailureEnabled', '', 1)

insert into hm_settings (settingname, settingstring, settinginteger) values ('MaxInvalidLogonAttempts', '', 3)

insert into hm_settings (settingname, settingstring, settinginteger) values ('LogonAttemptsWithinMinutes', '', 30)

insert into hm_settings (settingname, settingstring, settinginteger) values ('AutoBanMinutes', '', 60)

insert into hm_settings (settingname, settingstring, settinginteger) values ('IMAPHierarchyDelimiter', '.', 0)

insert into hm_settings (settingname, settingstring, settinginteger) values ('MaxNumberOfAsynchronousTasks', '', 15)

insert into hm_settings (settingname, settingstring, settinginteger) values ('MessageIndexing', '', 0)

insert into hm_settings (settingname, settingstring, settinginteger) values ('BypassGreylistingOnSPFSuccess', '', 1)

insert into hm_settings (settingname, settingstring, settinginteger) values ('BypassGreylistingOnMailFromMX', '', 0)

insert into hm_settings (settingname, settingstring, settinginteger) values ('MaxNumberOfMXHosts', '', 15)

insert into hm_settings (settingname, settingstring, settinginteger) values ('ClamAVEnabled', '', 0)

insert into hm_settings (settingname, settingstring, settinginteger) values ('ClamAVHost', 'localhost', 0)

insert into hm_settings (settingname, settingstring, settinginteger) values ('ClamAVPort', '', 3310)

insert into hm_settings (settingname, settingstring, settinginteger) values ('SmtpDeliveryConnectionSecurity', '', 2)

insert into hm_settings (settingname, settingstring, settinginteger) values ('VerifyRemoteSslCertificate', '', 1)

insert into hm_settings (settingname, settingstring, settinginteger) values ('SslCipherList', 'ECDHE-RSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES256-GCM-SHA384:ECDHE-ECDSA-AES256-GCM-SHA384:DHE-RSA-AES128-GCM-SHA256:DHE-DSS-AES128-GCM-SHA256:kEDH+AESGCM:ECDHE-RSA-AES128-SHA256:ECDHE-ECDSA-AES128-SHA256:ECDHE-RSA-AES128-SHA:ECDHE-ECDSA-AES128-SHA:ECDHE-RSA-AES256-SHA384:ECDHE-ECDSA-AES256-SHA384:ECDHE-RSA-AES256-SHA:ECDHE-ECDSA-AES256-SHA:DHE-RSA-AES128-SHA256:DHE-RSA-AES128-SHA:DHE-DSS-AES128-SHA256:DHE-RSA-AES256-SHA256:DHE-DSS-AES256-SHA:DHE-RSA-AES256-SHA:AES128-GCM-SHA256:AES256-GCM-SHA384:ECDHE-RSA-RC4-SHA:ECDHE-ECDSA-RC4-SHA:AES128:AES256:RC4-SHA:HIGH:!aNULL:!eNULL:!EXPORT:!DES:!3DES:!MD5:!PSK;', 0)

insert into hm_settings (settingname, settingstring, settinginteger) values ('SslVersions', '', 24)

insert into hm_settings (settingname, settingstring, settinginteger) values ('ImapMasterUser', '', 0)

insert into hm_settings (settingname, settingstring, settinginteger) values ('EnableImapSASLPlain', '', 0)

insert into hm_settings (settingname, settingstring, settinginteger) values ('EnableImapSASLInitialResponse', '', 0)

insert into hm_settings (settingname, settingstring, settinginteger) values ('ascheckptr', '', 0)

insert into hm_settings (settingname, settingstring, settinginteger) values ('ascheckptrscore', '', 1)

insert into hm_settings (settingname, settingstring, settinginteger) values ('IPv6Preferred', '', 0)

insert into hm_settings (settingname, settingstring, settinginteger) values ('CreateDefaultSpecialUseFolders', '', 0)

insert into hm_settings (settingname, settingstring, settinginteger) values ('TlsOptions', '', 0)

insert into hm_tcpipports (portprotocol, portnumber, portaddress1, portaddress2, portconnectionsecurity, portsslcertificateid) values (1, 25, 0, NULL, 0, 0) 

insert into hm_tcpipports (portprotocol, portnumber, portaddress1, portaddress2, portconnectionsecurity, portsslcertificateid) values (1, 587, 0, NULL, 0, 0) 

insert into hm_tcpipports (portprotocol, portnumber, portaddress1, portaddress2, portconnectionsecurity, portsslcertificateid) values (3, 110, 0, NULL, 0, 0) 

insert into hm_tcpipports (portprotocol, portnumber, portaddress1, portaddress2, portconnectionsecurity, portsslcertificateid) values (5, 143, 0, NULL, 0, 0) 

create table hm_imap_metadata
(
   metadataid bigint identity(1,1) not null,
   metadataaccountid bigint not null,
   metadatafolderid bigint not null,
   metadataentryname nvarchar(255) not null,
   metadatavalue nvarchar(2048) not null
)

ALTER TABLE hm_imap_metadata ADD CONSTRAINT hm_imap_metadata_pk PRIMARY KEY NONCLUSTERED (metadataid)

ALTER TABLE hm_imap_metadata ADD CONSTRAINT hm_imap_metadata_unique UNIQUE (metadataaccountid, metadatafolderid, metadataentryname)

create table hm_quarantine
(
	quarantineid int identity(1,1) not null,
	quarantinefilename nvarchar(255) not null,
	quarantinesender nvarchar(255) not null,
	quarantinerecipients nvarchar(1000) not null,
	quarantinesubject nvarchar(255) not null,
	quarantinereason nvarchar(255) not null,
	quarantinescore int not null,
	quarantinesize int not null,
	quarantinecreated datetime not null
)

ALTER TABLE hm_quarantine ADD CONSTRAINT hm_quarantine_pk PRIMARY KEY NONCLUSTERED (quarantineid)

CREATE CLUSTERED INDEX idx_hm_quarantine_created ON hm_quarantine (quarantinecreated)

create table hm_senderlist
(
	slid int identity(1,1) not null,
	slscope int not null,
	slownerid int not null,
	sltype int not null,
	slvalue nvarchar(255) not null,
	slreason nvarchar(255) not null,
	slexpires tinyint not null,
	slexpirestime datetime not null,
	slcreated datetime not null
)

ALTER TABLE hm_senderlist ADD CONSTRAINT hm_senderlist_pk PRIMARY KEY NONCLUSTERED (slid)

CREATE CLUSTERED INDEX idx_hm_senderlist_scope ON hm_senderlist (slscope, slownerid)

create table hm_digeststate
(
	dsid int identity(1,1) not null,
	dsaccountid int not null,
	dssenttime datetime not null,
	dslastid int not null,
	dsoptout tinyint not null
)

ALTER TABLE hm_digeststate ADD CONSTRAINT hm_digeststate_pk PRIMARY KEY NONCLUSTERED (dsid)

CREATE UNIQUE CLUSTERED INDEX idx_hm_digeststate_account ON hm_digeststate (dsaccountid)

create table hm_digesttokens
(
	dtid int identity(1,1) not null,
	dtaccountid int not null,
	dtkind int not null,
	dtquarantineid int not null,
	dttoken nvarchar(64) not null,
	dtexpirestime datetime not null,
	dtused tinyint not null,
	dtcreated datetime not null
)

ALTER TABLE hm_digesttokens ADD CONSTRAINT hm_digesttokens_pk PRIMARY KEY NONCLUSTERED (dtid)

CREATE UNIQUE CLUSTERED INDEX idx_hm_digesttokens_token ON hm_digesttokens (dttoken)

create table hm_passwordhistory
(
	phid int identity(1,1) not null,
	phaccountid int not null,
	phhash nvarchar(255) not null,
	phencryption int not null,
	phchanged datetime not null
)

ALTER TABLE hm_passwordhistory ADD CONSTRAINT hm_passwordhistory_pk PRIMARY KEY NONCLUSTERED (phid)

CREATE CLUSTERED INDEX idx_hm_passwordhistory ON hm_passwordhistory (phaccountid)

create table hm_messagetrace
(
	mtid int identity(1,1) not null,
	mtqueueid int not null,
	mtoccurred datetime not null,
	mtevent nvarchar(32) not null,
	mtsender nvarchar(255) not null,
	mtrecipient nvarchar(255) not null,
	mtsourceip nvarchar(64) not null,
	mtstatuscode int not null,
	mtdetail nvarchar(255) not null
)

ALTER TABLE hm_messagetrace ADD CONSTRAINT hm_messagetrace_pk PRIMARY KEY NONCLUSTERED (mtid)

CREATE CLUSTERED INDEX idx_hm_messagetrace ON hm_messagetrace (mtoccurred)

insert into hm_servermessages (smname, smtext) values ('QUOTA_WARNING', 'Your mailbox is %MACRO_PERCENT%% full.' + NCHAR(13) + NCHAR(10) + '' + NCHAR(13) + NCHAR(10) + '   Mailbox: %MACRO_MAILBOX%' + NCHAR(13) + NCHAR(10) + '   Used: %MACRO_USED% MB of %MACRO_QUOTA% MB' + NCHAR(13) + NCHAR(10) + '' + NCHAR(13) + NCHAR(10) + 'When it is completely full, new messages sent to you will be refused and' + NCHAR(13) + NCHAR(10) + 'the sender will be told to try again later. Deleting mail you no longer' + NCHAR(13) + NCHAR(10) + 'need - and emptying the trash folder afterwards - will free the space.' + NCHAR(13) + NCHAR(10) + '' + NCHAR(13) + NCHAR(10) + 'hMailServer' + NCHAR(13) + NCHAR(10) + '')

insert into hm_servermessages (smname, smtext) values ('QUOTA_WARNING_SUBJECT', 'Your mailbox is almost full')

create table hm_messageindexterms
(
	mitmessageid bigint not null,
	mitaccountid int not null,
	mitterm nvarchar(64) not null
)

CREATE INDEX idx_hm_messageindexterms_lookup ON hm_messageindexterms (mitaccountid, mitterm)

CREATE INDEX idx_hm_messageindexterms_message ON hm_messageindexterms (mitmessageid)

create table hm_messageindexstate
(
	misaccountid int not null,
	mishighwatermark bigint not null
)

CREATE INDEX idx_hm_messageindexstate ON hm_messageindexstate (misaccountid)

create table hm_metricsamples
(
	metricsampleid int identity(1,1) not null,
	metricsampletime datetime not null,
	metricsamplename nvarchar(64) not null,
	metricsamplevalue float not null
)

ALTER TABLE hm_metricsamples ADD CONSTRAINT hm_metricsamples_pk PRIMARY KEY NONCLUSTERED (metricsampleid)

CREATE CLUSTERED INDEX idx_hm_metricsamples_name_time ON hm_metricsamples (metricsamplename, metricsampletime)

create table hm_archiveindex
(
	archiveid int identity(1,1) not null,
	archivetime datetime not null,
	archivedomain nvarchar(255) not null,
	archivemailbox nvarchar(255) not null,
	archivedirection int not null,
	archivesender nvarchar(255) not null,
	archiverecipients nvarchar(1000) not null,
	archivesubject nvarchar(255) not null,
	archivemessageid nvarchar(255) not null,
	archivepath nvarchar(1000) not null,
	archivesize bigint not null,
	archivehold int not null
)

ALTER TABLE hm_archiveindex ADD CONSTRAINT hm_archiveindex_pk PRIMARY KEY NONCLUSTERED (archiveid)

CREATE CLUSTERED INDEX idx_hm_archiveindex_domain_time ON hm_archiveindex (archivedomain, archivetime)

CREATE INDEX idx_hm_archiveindex_path ON hm_archiveindex (archivepath)

ALTER TABLE hm_accounts ADD CONSTRAINT fk_hm_accounts_domain FOREIGN KEY (accountdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE

ALTER TABLE hm_aliases ADD CONSTRAINT fk_hm_aliases_domain FOREIGN KEY (aliasdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE

ALTER TABLE hm_domain_aliases ADD CONSTRAINT fk_hm_domain_aliases_domain FOREIGN KEY (dadomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE

ALTER TABLE hm_distributionlists ADD CONSTRAINT fk_hm_distributionlists_domain FOREIGN KEY (distributionlistdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE

ALTER TABLE hm_distributionlistsrecipients ADD CONSTRAINT fk_hm_dlrecipients_list FOREIGN KEY (distributionlistrecipientlistid) REFERENCES hm_distributionlists (distributionlistid) ON DELETE CASCADE

ALTER TABLE hm_routeaddresses ADD CONSTRAINT fk_hm_routeaddresses_route FOREIGN KEY (routeaddressrouteid) REFERENCES hm_routes (routeid) ON DELETE CASCADE

ALTER TABLE hm_fetchaccounts ADD CONSTRAINT fk_hm_fetchaccounts_account FOREIGN KEY (faaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

ALTER TABLE hm_fetchaccounts_uids ADD CONSTRAINT fk_hm_fetchaccounts_uids_fa FOREIGN KEY (uidfaid) REFERENCES hm_fetchaccounts (faid) ON DELETE CASCADE

ALTER TABLE hm_apppasswords ADD CONSTRAINT fk_hm_apppasswords_account FOREIGN KEY (apaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

ALTER TABLE hm_contacts ADD CONSTRAINT fk_hm_contacts_account FOREIGN KEY (contactaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

ALTER TABLE hm_addressbooks ADD CONSTRAINT fk_hm_addressbooks_account FOREIGN KEY (bookaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

ALTER TABLE hm_addressbookshares ADD CONSTRAINT fk_hm_addressbookshares_book FOREIGN KEY (sharebookid) REFERENCES hm_addressbooks (bookid) ON DELETE CASCADE

ALTER TABLE hm_accountprefs ADD CONSTRAINT fk_hm_accountprefs_account FOREIGN KEY (prefaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

ALTER TABLE hm_scheduled ADD CONSTRAINT fk_hm_scheduled_account FOREIGN KEY (schedaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

ALTER TABLE hm_files ADD CONSTRAINT fk_hm_files_account FOREIGN KEY (fileaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

ALTER TABLE hm_smimekeys ADD CONSTRAINT fk_hm_smimekeys_account FOREIGN KEY (smimeaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

create table hm_knownsenders
(
	ksid bigint identity(1,1) not null,
	ksaccountid int not null,
	ksaddress nvarchar(255) not null,
	kscount int not null,
	ksfirstseen nvarchar(32) not null,
	kslastseen nvarchar(32) not null
)

ALTER TABLE hm_knownsenders ADD CONSTRAINT hm_knownsenders_pk PRIMARY KEY NONCLUSTERED (ksid)

CREATE UNIQUE CLUSTERED INDEX idx_hm_knownsenders_account ON hm_knownsenders (ksaccountid, ksaddress)

create table hm_autoreplies
(
	arid bigint identity(1,1) not null,
	araccountid int not null,
	arsender nvarchar(255) not null,
	arsent bigint not null
)

ALTER TABLE hm_autoreplies ADD CONSTRAINT hm_autoreplies_pk PRIMARY KEY NONCLUSTERED (arid)

CREATE UNIQUE CLUSTERED INDEX idx_hm_autoreplies_account ON hm_autoreplies (araccountid, arsender)

create table hm_domainsamples
(
	dsid int identity(1,1) not null,
	dsday nvarchar(10) not null,
	dsdomain nvarchar(255) not null,
	dsspam int not null,
	dsviruses int not null,
	dsstorebytes bigint not null,
	dsstoremessages bigint not null
)

ALTER TABLE hm_domainsamples ADD CONSTRAINT hm_domainsamples_pk PRIMARY KEY NONCLUSTERED (dsid)

CREATE UNIQUE CLUSTERED INDEX idx_hm_domainsamples_day_domain ON hm_domainsamples (dsday, dsdomain)

create table hm_dkimrotations
(
	dkrid bigint identity(1,1) not null,
	dkrdomainid int not null,
	dkrcurrentselector nvarchar(255) not null,
	dkrcurrentkeyfile nvarchar(255) not null,
	dkrcurrentgenerated int not null,
	dkrcurrentsince bigint not null,
	dkrcurrentstatus int not null,
	dkrcurrentchecked bigint not null,
	dkrcurrentdetail nvarchar(255) not null,
	dkrnextselector nvarchar(255) not null,
	dkrnextkeyfile nvarchar(255) not null,
	dkrnextgenerated int not null,
	dkrnextcreated bigint not null,
	dkrnextfirstseen bigint not null,
	dkrnextstatus int not null,
	dkrnextchecked bigint not null,
	dkrnextdetail nvarchar(255) not null,
	dkrretiringselector nvarchar(255) not null,
	dkrretiringkeyfile nvarchar(255) not null,
	dkrretiringgenerated int not null,
	dkrretiringsince bigint not null,
	dkrretiringstatus int not null,
	dkrretiringchecked bigint not null,
	dkrretiringdetail nvarchar(255) not null,
	dkrretiredselector nvarchar(255) not null,
	dkrretiredat bigint not null,
	dkrretiredkeyfile nvarchar(255) not null,
	dkrlasterror nvarchar(255) not null,
	dkrlasterrortime bigint not null
)

ALTER TABLE hm_dkimrotations ADD CONSTRAINT hm_dkimrotations_pk PRIMARY KEY NONCLUSTERED (dkrid)

CREATE UNIQUE CLUSTERED INDEX idx_hm_dkimrotations_domain ON hm_dkimrotations (dkrdomainid)

create table hm_spamtraps
(
	stid int identity(1,1) not null,
	stdomainid int not null,
	stlocalpart nvarchar(64) not null,
	stcreated datetime not null
)

ALTER TABLE hm_spamtraps ADD CONSTRAINT hm_spamtraps_pk PRIMARY KEY NONCLUSTERED (stid)

CREATE UNIQUE CLUSTERED INDEX idx_hm_spamtraps_address ON hm_spamtraps (stdomainid, stlocalpart)

create table hm_spamtraphits
(
	sthid int identity(1,1) not null,
	sthdomainid int not null,
	sthaddress nvarchar(255) not null,
	sthipaddress nvarchar(64) not null,
	sthsender nvarchar(255) not null,
	sthtime datetime not null,
	sthbanned tinyint not null,
	sthblocked tinyint not null,
	sthnote nvarchar(255) not null
)

ALTER TABLE hm_spamtraphits ADD CONSTRAINT hm_spamtraphits_pk PRIMARY KEY NONCLUSTERED (sthid)

CREATE CLUSTERED INDEX idx_hm_spamtraphits_time ON hm_spamtraphits (sthtime)

CREATE INDEX idx_hm_spamtraphits_domain ON hm_spamtraphits (sthdomainid)

ALTER TABLE hm_calendars ADD CONSTRAINT fk_hm_calendars_account FOREIGN KEY (calendaraccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

ALTER TABLE hm_calendarobjects ADD CONSTRAINT fk_hm_calendarobjects_calendar FOREIGN KEY (objectcalendarid) REFERENCES hm_calendars (calendarid) ON DELETE CASCADE

ALTER TABLE hm_calendarshares ADD CONSTRAINT fk_hm_calendarshares_calendar FOREIGN KEY (sharecalendarid) REFERENCES hm_calendars (calendarid) ON DELETE CASCADE

ALTER TABLE hm_senderaddresses ADD CONSTRAINT fk_hm_senderaddresses_account FOREIGN KEY (saaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

ALTER TABLE hm_maskedaddresses ADD CONSTRAINT fk_hm_maskedaddresses_account FOREIGN KEY (maaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

ALTER TABLE hm_recoverable ADD CONSTRAINT fk_hm_recoverable_account FOREIGN KEY (rcaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

ALTER TABLE hm_passkeys ADD CONSTRAINT fk_hm_passkeys_account FOREIGN KEY (pkaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

ALTER TABLE hm_directory ADD CONSTRAINT fk_hm_directory_account FOREIGN KEY (diraccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

ALTER TABLE hm_confidential ADD CONSTRAINT fk_hm_confidential_account FOREIGN KEY (cfaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

ALTER TABLE hm_dmarcreports ADD CONSTRAINT fk_hm_dmarcreports_domain FOREIGN KEY (drdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE

ALTER TABLE hm_dmarcreportrecords ADD CONSTRAINT fk_hm_dmarcreportrecords_report FOREIGN KEY (rrreportid) REFERENCES hm_dmarcreports (drid) ON DELETE CASCADE

ALTER TABLE hm_tlsreports ADD CONSTRAINT fk_hm_tlsreports_domain FOREIGN KEY (trdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE

ALTER TABLE hm_accountactivity ADD CONSTRAINT fk_hm_accountactivity_account FOREIGN KEY (acaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

ALTER TABLE hm_accountrecipients ADD CONSTRAINT fk_hm_accountrecipients_account FOREIGN KEY (arcaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

ALTER TABLE hm_accountreview ADD CONSTRAINT fk_hm_accountreview_account FOREIGN KEY (arvaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

ALTER TABLE hm_heldmessages ADD CONSTRAINT fk_hm_heldmessages_account FOREIGN KEY (hmaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

ALTER TABLE hm_rule_criterias ADD CONSTRAINT fk_hm_rule_criterias_rule FOREIGN KEY (criteriaruleid) REFERENCES hm_rules (ruleid) ON DELETE CASCADE

ALTER TABLE hm_rule_actions ADD CONSTRAINT fk_hm_rule_actions_rule FOREIGN KEY (actionruleid) REFERENCES hm_rules (ruleid) ON DELETE CASCADE

ALTER TABLE hm_group_members ADD CONSTRAINT fk_hm_group_members_group FOREIGN KEY (membergroupid) REFERENCES hm_groups (groupid) ON DELETE CASCADE

ALTER TABLE hm_passwordhistory ADD CONSTRAINT fk_hm_passwordhistory_account FOREIGN KEY (phaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

ALTER TABLE hm_digeststate ADD CONSTRAINT fk_hm_digeststate_account FOREIGN KEY (dsaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

ALTER TABLE hm_digesttokens ADD CONSTRAINT fk_hm_digesttokens_account FOREIGN KEY (dtaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

ALTER TABLE hm_messagerecipients ADD CONSTRAINT fk_hm_messagerecipients_message FOREIGN KEY (recipientmessageid) REFERENCES hm_messages (messageid) ON DELETE CASCADE

ALTER TABLE hm_message_metadata ADD CONSTRAINT fk_hm_message_metadata_message FOREIGN KEY (metadata_messageid) REFERENCES hm_messages (messageid) ON DELETE CASCADE

ALTER TABLE hm_imapexpunged ADD CONSTRAINT fk_hm_imapexpunged_folder FOREIGN KEY (expungedfolderid) REFERENCES hm_imapfolders (folderid) ON DELETE CASCADE

ALTER TABLE hm_messageindexterms ADD CONSTRAINT fk_hm_messageindexterms_message FOREIGN KEY (mitmessageid) REFERENCES hm_messages (messageid) ON DELETE CASCADE

create table hm_audit
(
	auditid bigint identity(1,1) not null,
	audittime bigint not null,
	auditactor nvarchar(255) not null,
	auditactortype nvarchar(32) not null,
	auditinterface nvarchar(32) not null,
	auditaddress nvarchar(64) not null,
	auditobjecttype nvarchar(64) not null,
	auditobjectname nvarchar(255) not null,
	auditaction nvarchar(32) not null,
	auditdetail ntext not null,
	auditprevhash nvarchar(64) not null,
	audithash nvarchar(64) not null
)

ALTER TABLE hm_audit ADD CONSTRAINT hm_audit_pk PRIMARY KEY CLUSTERED (auditid)

CREATE INDEX idx_hm_audit_time ON hm_audit (audittime)

CREATE INDEX idx_hm_audit_actor ON hm_audit (auditactor)

create table hm_alertrules
(
	alertruleid bigint identity(1,1) not null,
	alertrulecondition nvarchar(64) not null,
	alertruleenabled tinyint not null,
	alertrulethreshold int not null,
	alertruleactions int not null,
	alertruleaddress nvarchar(255) not null,
	alertrulewebhook nvarchar(1024) not null,
	alertrulesecret nvarchar(255) not null,
	alertrulecooldown int not null,
	alertruledigest tinyint not null
)

ALTER TABLE hm_alertrules ADD CONSTRAINT hm_alertrules_pk PRIMARY KEY CLUSTERED (alertruleid)

CREATE UNIQUE INDEX idx_hm_alertrules_condition ON hm_alertrules (alertrulecondition)

create table hm_alertevents
(
	alerteventid bigint identity(1,1) not null,
	alerteventcondition nvarchar(64) not null,
	alerteventseverity int not null,
	alerteventstate int not null,
	alerteventtime bigint not null,
	alerteventsummary nvarchar(255) not null,
	alerteventdetail ntext not null,
	alerteventnotified tinyint not null,
	alerteventdigested tinyint not null,
	alerteventhookstate int not null,
	alerteventhooktries int not null,
	alerteventhooknext bigint not null
)

ALTER TABLE hm_alertevents ADD CONSTRAINT hm_alertevents_pk PRIMARY KEY CLUSTERED (alerteventid)

CREATE INDEX idx_hm_alertevents_time ON hm_alertevents (alerteventtime)

CREATE INDEX idx_hm_alertevents_condition ON hm_alertevents (alerteventcondition, alerteventtime)

insert into hm_alertrules (alertrulecondition, alertruleenabled, alertrulethreshold, alertruleactions, alertruleaddress, alertrulewebhook, alertrulesecret, alertrulecooldown, alertruledigest) values ('backup.failed', 1, 0, 1, '', '', '', 60, 0)

insert into hm_alertrules (alertrulecondition, alertruleenabled, alertrulethreshold, alertruleactions, alertruleaddress, alertrulewebhook, alertrulesecret, alertrulecooldown, alertruledigest) values ('certificate.expiring', 1, 7, 1, '', '', '', 1440, 1)

insert into hm_alertrules (alertrulecondition, alertruleenabled, alertrulethreshold, alertruleactions, alertruleaddress, alertrulewebhook, alertrulesecret, alertrulecooldown, alertruledigest) values ('disk.low', 0, 0, 1, '', '', '', 60, 1)

insert into hm_alertrules (alertrulecondition, alertruleenabled, alertrulethreshold, alertruleactions, alertruleaddress, alertrulewebhook, alertrulesecret, alertrulecooldown, alertruledigest) values ('queue.stalled', 0, 0, 1, '', '', '', 60, 1)

insert into hm_alertrules (alertrulecondition, alertruleenabled, alertrulethreshold, alertruleactions, alertruleaddress, alertrulewebhook, alertrulesecret, alertrulecooldown, alertruledigest) values ('autoban.storm', 0, 25, 1, '', '', '', 60, 1)

insert into hm_alertrules (alertrulecondition, alertruleenabled, alertrulethreshold, alertruleactions, alertruleaddress, alertrulewebhook, alertrulesecret, alertrulecooldown, alertruledigest) values ('minidump.written', 0, 0, 1, '', '', '', 60, 1)

insert into hm_alertrules (alertrulecondition, alertruleenabled, alertrulethreshold, alertruleactions, alertruleaddress, alertrulewebhook, alertrulesecret, alertrulecooldown, alertruledigest) values ('dkim.rotation.stalled', 1, 7, 1, '', '', '', 1440, 1)

insert into hm_alertrules (alertrulecondition, alertruleenabled, alertrulethreshold, alertruleactions, alertruleaddress, alertrulewebhook, alertrulesecret, alertrulecooldown, alertruledigest) values ('reputation.listed', 1, 0, 1, '', '', '', 1440, 0)

insert into hm_alertrules (alertrulecondition, alertruleenabled, alertrulethreshold, alertruleactions, alertruleaddress, alertrulewebhook, alertrulesecret, alertrulecooldown, alertruledigest) values ('account.compromised', 1, 0, 1, '', '', '', 0, 0)

create table hm_reputationchecks
(
	rcid bigint identity(1,1) not null,
	rcaddress nvarchar(64) not null,
	rcsources nvarchar(64) not null,
	rczone nvarchar(255) not null,
	rcstatus int not null,
	rccode nvarchar(255) not null,
	rcreason nvarchar(255) not null,
	rcchecked bigint not null,
	rclistedsince bigint not null,
	rcconfirmed bigint not null
)

ALTER TABLE hm_reputationchecks ADD CONSTRAINT hm_reputationchecks_pk PRIMARY KEY CLUSTERED (rcid)

insert into hm_settings (settingname, settingstring, settinginteger) values ('AlertsEnabled', '', 1)

insert into hm_settings (settingname, settingstring, settinginteger) values ('AlertRecipient', '', 0)

insert into hm_settings (settingname, settingstring, settinginteger) values ('AlertSenderAddress', '', 0)

insert into hm_settings (settingname, settingstring, settinginteger) values ('AlertDigestEnabled', '', 1)

insert into hm_settings (settingname, settingstring, settinginteger) values ('AlertDigestHour', '', 7)

insert into hm_settings (settingname, settingstring, settinginteger) values ('AlertMaxPerHour', '', 20)

insert into hm_settings (settingname, settingstring, settinginteger) values ('AlertWebhookMaxAttempts', '', 5)

insert into hm_settings (settingname, settingstring, settinginteger) values ('AuditTrailEnabled', '', 1)

insert into hm_settings (settingname, settingstring, settinginteger) values ('AuditRetentionDays', '', 0)

ALTER TABLE hm_knownsenders ADD CONSTRAINT fk_hm_knownsenders_account FOREIGN KEY (ksaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

ALTER TABLE hm_autoreplies ADD CONSTRAINT fk_hm_autoreplies_account FOREIGN KEY (araccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

create table hm_roles
(
	roleid int identity(1,1) not null,
	rolename nvarchar(64) not null,
	roledescription nvarchar(255) not null,
	rolebuiltin tinyint not null,
	rolepermissions nvarchar(1000) not null
)

ALTER TABLE hm_roles ADD CONSTRAINT hm_roles_pk PRIMARY KEY NONCLUSTERED (roleid)

CREATE UNIQUE CLUSTERED INDEX idx_hm_roles_name ON hm_roles (rolename)

create table hm_roleassignments
(
	raid int identity(1,1) not null,
	raaccountid int not null,
	raroleid int not null,
	radomains nvarchar(1000) not null
)

ALTER TABLE hm_roleassignments ADD CONSTRAINT hm_roleassignments_pk PRIMARY KEY NONCLUSTERED (raid)

CREATE UNIQUE CLUSTERED INDEX idx_hm_roleassignments_account ON hm_roleassignments (raaccountid)

ALTER TABLE hm_roleassignments ADD CONSTRAINT fk_hm_roleassignments_account FOREIGN KEY (raaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

ALTER TABLE hm_roleassignments ADD CONSTRAINT fk_hm_roleassignments_role FOREIGN KEY (raroleid) REFERENCES hm_roles (roleid)

insert into hm_roles (rolename, roledescription, rolebuiltin, rolepermissions) values ('serveradmin', 'Every permission, in every domain.', 1, 'administer_server,administer_domain,view_configuration,view_accounts,view_mailbox,view_queue,view_quarantine,view_archive,view_logs,view_message_trace,view_audit,manage_accounts,reset_password,unlock_account,manage_mailbox,manage_queue,manage_quarantine,manage_archive,manage_settings,manage_domains,manage_server,manage_roles')

insert into hm_roles (rolename, roledescription, rolebuiltin, rolepermissions) values ('domainadmin', 'The accounts, mailboxes and quarantine of named domains.', 1, 'administer_domain,view_configuration,view_accounts,view_mailbox,view_quarantine,view_message_trace,manage_accounts,reset_password,unlock_account,manage_mailbox,manage_quarantine')

insert into hm_roles (rolename, roledescription, rolebuiltin, rolepermissions) values ('helpdesk', 'Resets a password, unlocks an account and reads a message trace.', 1, 'view_accounts,view_message_trace,reset_password,unlock_account')

insert into hm_roles (rolename, roledescription, rolebuiltin, rolepermissions) values ('auditor', 'Reads everything and changes nothing, the audit trail included.', 1, 'view_configuration,view_accounts,view_mailbox,view_queue,view_quarantine,view_archive,view_logs,view_message_trace,view_audit')

ALTER TABLE hm_dkimrotations ADD CONSTRAINT fk_hm_dkimrotations_domain FOREIGN KEY (dkrdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE

create table hm_signins
(
	siid bigint identity(1,1) not null,
	siaccountid int not null,
	sidevice nvarchar(32) not null,
	siprotocol int not null,
	siaddress nvarchar(64) not null,
	siagent nvarchar(255) not null,
	sicredential int not null,
	sicredentialname nvarchar(64) not null,
	sifirstseen datetime not null,
	silastseen datetime not null,
	sicount int not null,
	sirevoked tinyint not null,
	sirevokedtime datetime null
)

ALTER TABLE hm_signins ADD CONSTRAINT hm_signins_pk PRIMARY KEY NONCLUSTERED (siid)

CREATE UNIQUE CLUSTERED INDEX idx_hm_signins_device ON hm_signins (siaccountid, sidevice)

create table hm_bouncesuppressions
(
	bsid bigint identity(1,1) not null,
	bsdomainid int not null,
	bsaddress nvarchar(255) not null,
	bssource int not null,
	bscategory int not null,
	bsstatus nvarchar(16) not null,
	bsdiagnostic nvarchar(255) not null,
	bsremotemta nvarchar(255) not null,
	bscreated nvarchar(32) not null,
	bsexpires nvarchar(32) not null
)

ALTER TABLE hm_bouncesuppressions ADD CONSTRAINT hm_bouncesuppressions_pk PRIMARY KEY NONCLUSTERED (bsid)

CREATE UNIQUE CLUSTERED INDEX idx_hm_bouncesuppressions_address ON hm_bouncesuppressions (bsdomainid, bsaddress)

create table hm_bouncecounts
(
	bcid bigint identity(1,1) not null,
	bcdomainid int not null,
	bcaddress nvarchar(255) not null,
	bchard int not null,
	bcsoft int not null,
	bcmailboxfull int not null,
	bcpolicy int not null,
	bcother int not null,
	bcconsecutivehard int not null,
	bccomplaints int not null,
	bclaststatus nvarchar(16) not null,
	bclastcategory int not null,
	bclastbounce nvarchar(32) not null,
	bclastdelivered nvarchar(32) not null,
	bclastcomplaint nvarchar(32) not null
)

ALTER TABLE hm_bouncecounts ADD CONSTRAINT hm_bouncecounts_pk PRIMARY KEY NONCLUSTERED (bcid)

CREATE UNIQUE CLUSTERED INDEX idx_hm_bouncecounts_address ON hm_bouncecounts (bcdomainid, bcaddress)

create table hm_bouncereportkeys
(
	brkid bigint identity(1,1) not null,
	brkdomainid int not null,
	brkkey nvarchar(64) not null,
	brksent nvarchar(32) not null,
	brkaddress nvarchar(64) not null,
	brkrun int not null
)

ALTER TABLE hm_bouncereportkeys ADD CONSTRAINT hm_bouncereportkeys_pk PRIMARY KEY (brkid)

CREATE INDEX idx_hm_bouncereportkeys_key ON hm_bouncereportkeys (brkkey)

CREATE INDEX idx_hm_bouncereportkeys_sent ON hm_bouncereportkeys (brksent)

CREATE INDEX idx_hm_bouncereportkeys_address ON hm_bouncereportkeys (brkaddress)

ALTER TABLE hm_bouncesuppressions ADD CONSTRAINT fk_hm_bouncesuppressions_domain FOREIGN KEY (bsdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE

ALTER TABLE hm_bouncecounts ADD CONSTRAINT fk_hm_bouncecounts_domain FOREIGN KEY (bcdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE

ALTER TABLE hm_bouncereportkeys ADD CONSTRAINT fk_hm_bouncereportkeys_domain FOREIGN KEY (brkdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE

create table hm_pgpkeys
(
	pgpkid int identity(1,1) not null,
	pgpkaccountid int not null,
	pgpkfingerprint nvarchar(64) not null,
	pgpkversion int not null,
	pgpkalgorithm int not null,
	pgpkbits int not null,
	pgpkuserid nvarchar(255) not null,
	pgpkcreated bigint not null,
	pgpkexpires bigint not null,
	pgpkbytes int not null,
	pgpkkey ntext not null,
	pgpkuploaded bigint not null,
	pgpkuploadedby nvarchar(255) not null
)

ALTER TABLE hm_pgpkeys ADD CONSTRAINT hm_pgpkeys_pk PRIMARY KEY NONCLUSTERED (pgpkid)

CREATE CLUSTERED INDEX idx_hm_pgpkeys_account ON hm_pgpkeys (pgpkaccountid)

CREATE UNIQUE INDEX idx_hm_pgpkeys_entry ON hm_pgpkeys (pgpkaccountid, pgpkfingerprint)

ALTER TABLE hm_pgpkeys ADD CONSTRAINT fk_hm_pgpkeys_account FOREIGN KEY (pgpkaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

create table hm_pgpring
(
	pgrid int identity(1,1) not null,
	pgraccountid int not null,
	pgrkind tinyint not null,
	pgraddress nvarchar(255) not null,
	pgrname nvarchar(255) not null,
	pgrfingerprint nvarchar(64) not null,
	pgrkeyid nvarchar(16) not null,
	pgralgorithm nvarchar(32) not null,
	pgrpublic ntext not null,
	pgrsecret ntext not null,
	pgrcreated bigint not null,
	pgrexpires bigint not null,
	pgrrevoked int not null,
	pgrsource nvarchar(16) not null,
	pgrprefer int not null,
	pgrfirstseen bigint not null,
	pgrlastseen bigint not null,
	pgrverified int not null default 0,
	pgrverifiedat bigint not null default 0,
	pgrchangedfrom nvarchar(64) not null default '',
	pgrchangedat bigint not null default 0,
	pgraccepted int not null default 0
)

ALTER TABLE hm_pgpring ADD CONSTRAINT hm_pgpring_pk PRIMARY KEY NONCLUSTERED (pgrid)

CREATE CLUSTERED INDEX idx_hm_pgpring_account ON hm_pgpring (pgraccountid)

CREATE UNIQUE INDEX idx_hm_pgpring_entry ON hm_pgpring (pgraccountid, pgrkind, pgrfingerprint)

ALTER TABLE hm_pgpring ADD CONSTRAINT fk_hm_pgpring_account FOREIGN KEY (pgraccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

create table hm_protectedmessages
(
	pmid int identity(1,1) not null,
	pmaccountid int not null,
	pmtoken nvarchar(32) not null,
	pmrecipient nvarchar(255) not null,
	pmhint nvarchar(255) not null,
	pmfile nvarchar(255) not null,
	pmsize bigint not null,
	pmstored bigint not null,
	pmcomplete int not null,
	pmcreated bigint not null,
	pmexpires bigint not null,
	pmopened bigint not null,
	pmrevoked int not null,
	pmreplyto nvarchar(32) not null
)

ALTER TABLE hm_protectedmessages ADD CONSTRAINT hm_protectedmessages_pk PRIMARY KEY NONCLUSTERED (pmid)

CREATE UNIQUE CLUSTERED INDEX idx_hm_protectedmessages_token ON hm_protectedmessages (pmtoken)

CREATE INDEX idx_hm_protectedmessages_account ON hm_protectedmessages (pmaccountid)

ALTER TABLE hm_protectedmessages ADD CONSTRAINT fk_hm_protectedmessages_account FOREIGN KEY (pmaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

insert into hm_settings (settingname, settingstring, settinginteger) values ('ProtectedMessageDays', '', 30)

create table hm_zeroaccess
(
	zaid int identity(1,1) not null,
	zaaccountid int not null,
	zafingerprint nvarchar(64) not null,
	zasalt nvarchar(64) not null,
	zapasses int not null,
	zamemory int not null,
	zalanes int not null,
	zasecret ntext not null,
	zarecovery ntext not null,
	zaenabledat bigint not null
)

ALTER TABLE hm_zeroaccess ADD CONSTRAINT hm_zeroaccess_pk PRIMARY KEY NONCLUSTERED (zaid)

CREATE UNIQUE INDEX idx_hm_zeroaccess_account ON hm_zeroaccess (zaaccountid)

ALTER TABLE hm_zeroaccess ADD CONSTRAINT fk_hm_zeroaccess_account FOREIGN KEY (zaaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

insert into hm_settings (settingname, settingstring, settinginteger) values ('ZeroAccessAllowed', '', 0)

create table hm_recall
(
	recallid int identity(1,1) not null,
	recallsenderid int not null,
	recallmsgid nvarchar(255) not null,
	recallrecipientid int not null,
	recallqueueid bigint not null,
	recallmessageid bigint not null,
	recallcreated bigint not null,
	recallrecalledat bigint not null,
	recalloutcome int not null
)

ALTER TABLE hm_recall ADD CONSTRAINT hm_recall_pk PRIMARY KEY NONCLUSTERED (recallid)

CREATE INDEX idx_hm_recall_sender ON hm_recall (recallsenderid)

CREATE INDEX idx_hm_recall_queue ON hm_recall (recallqueueid)

ALTER TABLE hm_recall ADD CONSTRAINT fk_hm_recall_sender FOREIGN KEY (recallsenderid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

insert into hm_settings (settingname, settingstring, settinginteger) values ('RecallDays', '', 14)

insert into hm_settings (settingname, settingstring, settinginteger) values ('OpenPGPWebKeyDirectoryEnabled', '', 0)

insert into hm_settings (settingname, settingstring, settinginteger) values ('OpenPGPKeyMaximumBytes', '', 65536)

insert into hm_settings (settingname, settingstring, settinginteger) values ('OpenPGPKeysPerAccount', '', 8)

insert into hm_settings (settingname, settingstring, settinginteger) values ('OpenPGPRequireAddressInUserId', '', 1)

create table hm_webhooks
(
	whid bigint identity(1,1) not null,
	whname nvarchar(64) not null,
	whurl nvarchar(1024) not null,
	whsecret nvarchar(1024) not null,
	whevents nvarchar(255) not null,
	whdomains nvarchar(1024) not null,
	whaddresses nvarchar(1024) not null,
	whenabled int not null,
	whmaxperminute int not null,
	whincludesubject int not null,
	whcreated bigint not null
)

ALTER TABLE hm_webhooks ADD CONSTRAINT hm_webhooks_pk PRIMARY KEY NONCLUSTERED (whid)

CREATE UNIQUE CLUSTERED INDEX idx_hm_webhooks_name ON hm_webhooks (whname)

create table hm_webhookqueue
(
	wqid bigint identity(1,1) not null,
	wqwebhookid bigint not null,
	wqevent nvarchar(64) not null,
	wqcreated bigint not null,
	wqstate int not null,
	wqtries int not null,
	wqnext bigint not null,
	wqerror nvarchar(255) not null,
	wqbody ntext not null
)

ALTER TABLE hm_webhookqueue ADD CONSTRAINT hm_webhookqueue_pk PRIMARY KEY (wqid)

CREATE INDEX idx_hm_webhookqueue_due ON hm_webhookqueue (wqstate, wqnext)

CREATE INDEX idx_hm_webhookqueue_webhook ON hm_webhookqueue (wqwebhookid, wqid)

ALTER TABLE hm_webhookqueue ADD CONSTRAINT fk_hm_webhookqueue_webhook FOREIGN KEY (wqwebhookid) REFERENCES hm_webhooks (whid) ON DELETE CASCADE

insert into hm_settings (settingname, settingstring, settinginteger) values ('WebhooksEnabled', '', 1)

insert into hm_settings (settingname, settingstring, settinginteger) values ('WebhookMaxAttempts', '', 5)

insert into hm_settings (settingname, settingstring, settinginteger) values ('WebhookMaxPerMinute', '', 60)

insert into hm_settings (settingname, settingstring, settinginteger) values ('WebhookRetentionDays', '', 7)

ALTER TABLE hm_signins ADD CONSTRAINT fk_hm_signins_account FOREIGN KEY (siaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

create table hm_pushsubscriptions
(
	psid bigint identity(1,1) not null,
	psaccountid int not null,
	psendpointhash nvarchar(64) not null,
	psendpoint nvarchar(1000) not null,
	pskeyp256dh nvarchar(128) not null,
	pskeyauth nvarchar(32) not null,
	psdevice nvarchar(64) not null,
	pscreated bigint not null,
	pslastsent bigint not null,
	psstate int not null,
	psfailures int not null,
	psnextattempt bigint not null,
	pslastdetail nvarchar(255) not null,
	psdetail int not null,
	pssenders nvarchar(255) not null,
	pslabels nvarchar(255) not null,
	psquietfrom int not null,
	psquietto int not null,
	psquietoffset int not null
)

ALTER TABLE hm_pushsubscriptions ADD CONSTRAINT hm_pushsubscriptions_pk PRIMARY KEY NONCLUSTERED (psid)

CREATE UNIQUE CLUSTERED INDEX idx_hm_pushsubscriptions_endpoint ON hm_pushsubscriptions (psaccountid, psendpointhash)

ALTER TABLE hm_pushsubscriptions ADD CONSTRAINT fk_hm_pushsubscriptions_account FOREIGN KEY (psaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

create table hm_complaints
(
	cpid bigint identity(1,1) not null,
	cpdomainid int not null,
	cpaccountid int not null,
	cpkey nvarchar(64) not null,
	cpaddress nvarchar(255) not null,
	cpsender nvarchar(255) not null,
	cpfeedbacktype nvarchar(32) not null,
	cpprovider nvarchar(255) not null,
	cpsourceip nvarchar(45) not null,
	cpreporteddomain nvarchar(255) not null,
	cpmessageid nvarchar(255) not null,
	cpsubject nvarchar(255) not null,
	cparrival nvarchar(32) not null,
	cpreceived nvarchar(32) not null
)

ALTER TABLE hm_complaints ADD CONSTRAINT hm_complaints_pk PRIMARY KEY NONCLUSTERED (cpid)

CREATE UNIQUE CLUSTERED INDEX idx_hm_complaints_key ON hm_complaints (cpkey)

CREATE INDEX idx_hm_complaints_domain ON hm_complaints (cpdomainid)

CREATE INDEX idx_hm_complaints_address ON hm_complaints (cpaddress)

CREATE INDEX idx_hm_complaints_received ON hm_complaints (cpreceived)

create table hm_complaintvolume
(
	cvid bigint identity(1,1) not null,
	cvdomainid int not null,
	cvday nvarchar(10) not null,
	cvsent bigint not null,
	cvcomplaints int not null
)

ALTER TABLE hm_complaintvolume ADD CONSTRAINT hm_complaintvolume_pk PRIMARY KEY NONCLUSTERED (cvid)

CREATE UNIQUE CLUSTERED INDEX idx_hm_complaintvolume_day ON hm_complaintvolume (cvdomainid, cvday)

ALTER TABLE hm_complaints ADD CONSTRAINT fk_hm_complaints_domain FOREIGN KEY (cpdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE

ALTER TABLE hm_complaintvolume ADD CONSTRAINT fk_hm_complaintvolume_domain FOREIGN KEY (cvdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE

insert into hm_alertrules (alertrulecondition, alertruleenabled, alertrulethreshold, alertruleactions, alertruleaddress, alertrulewebhook, alertrulesecret, alertrulecooldown, alertruledigest) values ('complaints.rate.high', 1, 1000, 1, '', '', '', 1440, 1)

create table hm_mailboxmoves
(
	mmid bigint identity(1,1) not null,
	mmaccountid int not null,
	mmoldaddress nvarchar(255) not null,
	mmnewaddress nvarchar(255) not null,
	mmstage int not null,
	mmkeepalias int not null,
	mmmessages int not null,
	mmstarted nvarchar(32) not null,
	mmfinished nvarchar(32) not null,
	mmdetail nvarchar(255) not null
)

ALTER TABLE hm_mailboxmoves ADD CONSTRAINT hm_mailboxmoves_pk PRIMARY KEY (mmid)

CREATE INDEX idx_hm_mailboxmoves_account ON hm_mailboxmoves (mmaccountid)

ALTER TABLE hm_mailboxmoves ADD CONSTRAINT fk_hm_mailboxmoves_account FOREIGN KEY (mmaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

ALTER TABLE hm_spamtraps ADD CONSTRAINT fk_hm_spamtraps_domain FOREIGN KEY (stdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE

ALTER TABLE hm_spamtraphits ADD CONSTRAINT fk_hm_spamtraphits_domain FOREIGN KEY (sthdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE

create table hm_domainpolicylist
(
	dplid int identity(1,1) not null,
	dplpattern nvarchar(255) not null,
	dplmatchon int not null,
	dplaction int not null,
	dplnote nvarchar(255) not null,
	dplcreated datetime not null
)

ALTER TABLE hm_domainpolicylist ADD CONSTRAINT hm_domainpolicylist_pk PRIMARY KEY NONCLUSTERED (dplid)

CREATE UNIQUE CLUSTERED INDEX idx_hm_domainpolicylist_pattern ON hm_domainpolicylist (dplpattern, dplmatchon)

create table hm_journalrules
(
	jrid int identity(1,1) not null,
	jrscope int not null,
	jrdomainid int not null,
	jraccountid int not null,
	jraddress nvarchar(255) not null,
	jrdirection int not null,
	jrmode int not null,
	jrenabled tinyint not null,
	jrnote nvarchar(255) not null
)

ALTER TABLE hm_journalrules ADD CONSTRAINT hm_journalrules_pk PRIMARY KEY NONCLUSTERED (jrid)

CREATE CLUSTERED INDEX idx_hm_journalrules_domain ON hm_journalrules (jrdomainid)

CREATE INDEX idx_hm_journalrules_account ON hm_journalrules (jraccountid)

ALTER TABLE hm_journalrules ADD CONSTRAINT fk_hm_journalrules_domain FOREIGN KEY (jrdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE

create table hm_accountrecovery
(
	acrid int identity(1,1) not null,
	acraccountid int not null,
	acraddress nvarchar(255) not null,
	acrverified int not null,
	acrcreated bigint not null,
	acrupdated bigint not null
)

ALTER TABLE hm_accountrecovery ADD CONSTRAINT hm_accountrecovery_pk PRIMARY KEY (acrid)

CREATE UNIQUE INDEX idx_hm_accountrecovery_account ON hm_accountrecovery (acraccountid)

create table hm_recoverytokens
(
	rtid bigint identity(1,1) not null,
	rtaccountid int not null,
	rtkind int not null,
	rttoken nvarchar(64) not null,
	rtaddress nvarchar(255) not null,
	rtexpires bigint not null,
	rtused bigint not null,
	rtcreated bigint not null
)

ALTER TABLE hm_recoverytokens ADD CONSTRAINT hm_recoverytokens_pk PRIMARY KEY (rtid)

CREATE INDEX idx_hm_recoverytokens_token ON hm_recoverytokens (rttoken)

CREATE INDEX idx_hm_recoverytokens_account ON hm_recoverytokens (rtaccountid)

create table hm_recoverycodes
(
	rkid bigint identity(1,1) not null,
	rkaccountid int not null,
	rkhash nvarchar(255) not null,
	rkencryption int not null,
	rkused bigint not null,
	rkcreated bigint not null
)

ALTER TABLE hm_recoverycodes ADD CONSTRAINT hm_recoverycodes_pk PRIMARY KEY (rkid)

CREATE INDEX idx_hm_recoverycodes_account ON hm_recoverycodes (rkaccountid)

ALTER TABLE hm_accountrecovery ADD CONSTRAINT fk_hm_accountrecovery_account FOREIGN KEY (acraccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

ALTER TABLE hm_recoverytokens ADD CONSTRAINT fk_hm_recoverytokens_account FOREIGN KEY (rtaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

ALTER TABLE hm_recoverycodes ADD CONSTRAINT fk_hm_recoverycodes_account FOREIGN KEY (rkaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

insert into hm_alertrules (alertrulecondition, alertruleenabled, alertrulethreshold, alertruleactions, alertruleaddress, alertrulewebhook, alertrulesecret, alertrulecooldown, alertruledigest) values ('account.password-reset', 1, 0, 1, '', '', '', 0, 0)

insert into hm_dbversion values (6085)
