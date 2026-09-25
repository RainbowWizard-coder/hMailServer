CREATE or REPLACE function hm_drop_table (tablename varchar) returns void as '
DECLARE
	table_exists int4;
BEGIN
	SELECT into table_exists count(*) from pg_class where relname = tablename::name;
	if table_exists > 0 then
		execute ''DROP TABLE '' || tablename;
	end if;
END;'
language 'plpgsql' ;

select hm_drop_table('hm_imapfolders');

select hm_drop_table('hm_settings');

select hm_drop_table('hm_inisettings');

select hm_drop_table('hm_roleassignments');

select hm_drop_table('hm_roles');

select hm_drop_table('hm_audit');

select hm_drop_table('hm_alertrules');

select hm_drop_table('hm_webhookqueue');
select hm_drop_table('hm_webhooks');
select hm_drop_table('hm_alertevents');
select hm_drop_table('hm_remotedomainpolicies');

select hm_drop_table('hm_dkimrotations');

select hm_drop_table('hm_reputationchecks');
select hm_drop_table('hm_spamtraps');
select hm_drop_table('hm_journalrules');

select hm_drop_table('hm_spamtraphits');

select hm_drop_table('hm_domainpolicylist');

select hm_drop_table('hm_calendarobjects');

select hm_drop_table('hm_calendars');

select hm_drop_table('hm_addressbookshares');

select hm_drop_table('hm_addressbooks');

select hm_drop_table('hm_contacts');

select hm_drop_table('hm_pushsubscriptions');

select hm_drop_table('hm_accountprefs');

select hm_drop_table('hm_scheduled');

select hm_drop_table('hm_files');
select hm_drop_table('hm_smimekeys');

select hm_drop_table('hm_smimeissued');

select hm_drop_table('hm_smimeca');

select hm_drop_table('hm_accountkeys');

select hm_drop_table('hm_domainrecovery');

select hm_drop_table('hm_pgpring');

select hm_drop_table('hm_protectedmessages');

select hm_drop_table('hm_recall');

select hm_drop_table('hm_settingoverrides');

select hm_drop_table('hm_apisends');
select hm_drop_table('hm_migrationfolders');

select hm_drop_table('hm_migrationjobs');

select hm_drop_table('hm_migrations');
select hm_drop_table('hm_sandboxverdicts');

select hm_drop_table('hm_sandboxholds');
select hm_drop_table('hm_listarchive');
select hm_drop_table('hm_warmupcounts');

select hm_drop_table('hm_warmup');

select hm_drop_table('hm_dmarcreportrecords');

select hm_drop_table('hm_dmarcreports');

select hm_drop_table('hm_tlsreports');
select hm_drop_table('hm_accountactivity');

select hm_drop_table('hm_accountrecipients');

select hm_drop_table('hm_accountreview');

select hm_drop_table('hm_heldmessages');

select hm_drop_table('hm_confidential');

select hm_drop_table('hm_passkeys');

select hm_drop_table('hm_directory');

select hm_drop_table('hm_recoverable');

select hm_drop_table('hm_maskedaddresses');

select hm_drop_table('hm_senderaddresses');

select hm_drop_table('hm_accounts');

select hm_drop_table('hm_aliases');

select hm_drop_table('hm_domains');

select hm_drop_table('hm_domain_aliases');

select hm_drop_table('hm_messages');

select hm_drop_table('hm_storeblobs');

select hm_drop_table('hm_message_metadata');

select hm_drop_table('hm_dbversion');

select hm_drop_table('hm_distributionlists');

select hm_drop_table('hm_distributionlistsrecipients');

select hm_drop_table('hm_messagerecipients');

select hm_drop_table('hm_routes');

select hm_drop_table('hm_routeaddresses');

select hm_drop_table('hm_securityranges');

select hm_drop_table('hm_dnsbl');

select hm_drop_table('hm_fetchaccounts');

select hm_drop_table('hm_fetchaccounts_uids');

select hm_drop_table('hm_rules');

select hm_drop_table('hm_rule_criterias');

select hm_drop_table('hm_rule_actions');

select hm_drop_table('hm_surblservers');

select hm_drop_table('hm_greylisting_triplets');

select hm_drop_table('hm_blocked_attachments');

select hm_drop_table('hm_servermessages');

select hm_drop_table('hm_greylisting_whiteaddresses');

select hm_drop_table('hm_greylisting_bypass');

select hm_drop_table('hm_tcpipports');

select hm_drop_table('hm_whitelist');

select hm_drop_table('hm_sslcertificates');

select hm_drop_table('hm_groups');

select hm_drop_table('hm_group_members');

select hm_drop_table('hm_acl');

select hm_drop_table('hm_incoming_relays');

select hm_drop_table('hm_logon_failures');


create table hm_accounts 
(
	accountid bigserial not null primary key,
	accountdomainid int not null ,
	accountadminlevel smallint not null ,
	accountaddress varchar (255) not null unique,
	accountpassword varchar (255) not null ,
	accountactive smallint not null,
	accountisad smallint not null, 
	accountaddomain varchar (255) not null ,
	accountadusername varchar (255) not null,
	accountmaxsize int not null,
	accountmessageretentiondays int not null default 0,
	accountvacationmessageon smallint  not null,
	accountvacationmessage text not null,
	accountvacationsubject varchar (200) not null,
	accountpwencryption smallint  not null,
	accountforwardenabled smallint  not null,
	accountforwardaddress varchar (255) not null,
	accountforwardkeeporiginal smallint  not null,
	accountenablesignature smallint  not null,
	accountsignatureplaintext text not null,
	accountsignaturehtml text not null,
	accountlastlogontime timestamp not null,
	accountvacationexpires smallint not null,
	accountvacationexpiredate timestamp not null,
	accountvacationbegindate timestamp not null,
	accountpersonfirstname varchar(60) not null,
	accountpersonlastname varchar(60) not null,	
	accountvacationabortspamflagged smallint not null,
	accountvacationdays int not null default 0,
	accountvacationinternalsubject varchar(200) not null default '',
	accountvacationinternalmessage varchar(1000) not null default '',
	accountvacationinternalonly smallint not null default 0,
	accountforwardabortspamflagged smallint not null,
	accounttotpsecret varchar(255) not null default '',
	accountpasswordchanged timestamp not null default '2000-01-01',
	accountantispamenabled smallint not null default 1,
	accountspammarkthreshold int not null default -1,
	accountspamdeletethreshold int not null default -1,
	accountopenpgppublish int not null default 0,
	accountzeroaccess int not null default 0,
	accountrestricteddelivery int not null default 0,
	accountexternalid varchar(255) not null default ''
);



CREATE INDEX idx_hm_accounts ON hm_accounts (accountaddress);

CREATE INDEX idx_hm_accounts_externalid ON hm_accounts (accountexternalid);

create table hm_aliases 
(
	aliasid bigserial not null primary key,
	aliasdomainid int not null ,
	aliasname varchar (255) not null unique,
	aliasvalue varchar (255) not null ,
	aliasactive smallint not null
);

CREATE INDEX idx_hm_aliases ON hm_aliases (aliasdomainid, aliasname);

create table hm_domains 
(
	domainid bigserial not null primary key,
	domainname varchar (80) not null unique,
	domainactive smallint not null,
	domainpostmaster varchar (80) not null,
	domainmaxsize integer not null,
	domainmessageretentiondays int not null default 0,
	domainaddomain varchar(255) not null,
	domainmaxmessagesize integer not null,
	domainuseplusaddressing  smallint not null,
	domainplusaddressingchar varchar(1) not null,
	domainantispamoptions integer not null,
	domainenablesignature smallint not null,
	domainsignaturemethod smallint not null,
	domainsignatureplaintext text not null,
	domainsignaturehtml text not null,
	domainaddsignaturestoreplies smallint not null,
	domainaddsignaturestolocalemail smallint not null,
	domainmaxnoofaccounts int not null,
	domainmaxnoofaliases int not null,
	domainmaxnoofdistributionlists int not null,
	domainlimitationsenabled int not null,
	domainmaxaccountsize int not null,
   domaindkimselector varchar(255) not null,
   domaindkimprivatekeyfile varchar(255) not null,
   domaindkimsecondaryselector varchar(255) not null,
   domaindkimsecondaryprivatekeyfile varchar(255) not null,
   domainrelayhost varchar(255) not null,
   domainrelayport int not null,
   domainrelayrequiresauth int not null,
   domainrelayusername varchar(255) not null,
   domainrelaypassword varchar(1024) not null,
   domainrelayconnectionsecurity int not null,
	domainvacationmessageon int not null default 0,
	domainvacationsubject varchar(200) not null default '',
	domainvacationmessage text not null default '',
	domainvacationinternalsubject varchar(200) not null default '',
	domainvacationinternalmessage text not null default '',
	domainvacationexternaloverride int not null default 0,
	domainvacationdays int not null default 0,
	domainquotawarningpercent int not null default -1,
	domainquotawarningsubject varchar(200) not null default '',
	domainquotawarningtext text not null default '',
	domainquotagracepercent int not null default -1,
	domainquotasendstoppercent int not null default -1,
	domainexternaltagsubject int not null default 0,
	domainexternaltagheader int not null default 0,
	domainexternaltagtext varchar(100) not null default '',
	domainfirstcontacttip int not null default 0,
	domaindisclaimerenabled int not null default 0,
	domaindisclaimerplaintext text not null default '',
	domaindisclaimerhtml text not null default '',
	domainquarantinedigest smallint not null default 0,
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
	domaincountrypolicy varchar(255) not null default '',
	domainsessionidleseconds int not null default 0,
	domainsessionlifetimeseconds int not null default 0,
	domainsessiononeperdevice int not null default 0,
	domainsessionbindaddress int not null default 0,
	domainreportmailbox varchar(255) not null default '',
	domainrestricteddomains varchar(2000) not null default '',
	domainimpersonationaction int not null default 0,
	domainprotectedpersons varchar(2000) not null default '',
	domainprotectedpartners varchar(2000) not null default ''
);

CREATE INDEX idx_hm_domains ON hm_domains (domainname);

create table hm_domain_aliases
(
	daid bigserial not null primary key,
	dadomainid int not null ,
	daalias varchar(255) not null
);

create table hm_messages 
(
	messageid bigserial not null primary key,
	messageaccountid int not null ,
	messagefolderid integer not null DEFAULT 0,
	messagefilename varchar (255) not null ,
	messagetype smallint not null ,
	messagefrom varchar (255) not null ,
	messagesize bigint not null,
	messagecurnooftries int not null,
	messagenexttrytime timestamp not null,
	messageflags smallint not null,
	messagecreatetime timestamp not null,
	messagesavedate timestamp not null,
	messageemailid varchar(48) not null,
	messagelocked smallint not null,
   messageuid bigint not null,
   messagemodseq bigint not null default 1,
   messagekeywords varchar(500) not null default ''
);

CREATE INDEX idx_hm_messages ON hm_messages (messageaccountid, messagefolderid);

CREATE INDEX idx_hm_messages_type ON hm_messages (messagetype);

create table hm_storeblobs
(
	sbid bigserial not null primary key,
	sbsha256 varchar(64) not null,
	sbsize bigint not null,
	sbrefs int not null,
	sbcreated bigint not null
);

CREATE UNIQUE INDEX idx_hm_storeblobs_sha256 ON hm_storeblobs (sbsha256);

create table hm_message_metadata 
(
   metadata_id bigserial not null primary key,
   metadata_accountid int not null,
   metadata_folderid int not null,
   metadata_messageid bigint  not null,
   metadata_dateutc timestamp null,
   metadata_from varchar(255) not null,
   metadata_subject varchar(255) not null,
   metadata_to varchar(255) not null,
   metadata_cc varchar(255) not null
);

CREATE UNIQUE INDEX idx_message_metadata_unique ON hm_message_metadata (metadata_accountid, metadata_folderid, metadata_messageid);

CREATE INDEX idx_message_metadata_id ON hm_message_metadata (metadata_messageid);

create table hm_settings 
(
	settingid bigserial not null primary key,
	settingname varchar (30) not null unique,
	settingstring varchar (4000) not null ,
	settinginteger int not null
);

create table hm_inisettings
(
	inisettingid bigserial not null primary key,
	inisettingname varchar (100) not null unique,
	inisettingvalue text not null,
	inisettingfilevalue text not null
);

create table hm_dbversion 
(
	value int not null
);

create table hm_distributionlists 
(
	distributionlistid bigserial not null primary key,
	distributionlistdomainid int not null,
	distributionlistaddress varchar(255) not null unique,
	distributionlistenabled smallint not null,
   distributionlistrequireauth smallint not null,
	distributionlistrequireaddress varchar(255) not null,
	distributionlistmode smallint not null,
	distributionlistmoderatoraddress varchar(255) not null default '',
	distributionlistbounceaddress varchar(255) not null default '',
	distributionlistmembership varchar(200) not null default '',
	distributionlistmanaged int not null default 0,
	distributionlistsubscribe int not null default 0,
	distributionlistarchive int not null default 0,
	distributionlistarchivedays int not null default 0,
	distributionlistdigest int not null default 0,
	distributionlistdigesthour int not null default 7,
	distributionlistdigestlastid bigint not null default 0,
	distributionlistdigestlasttime bigint not null default 0,
	distributionlistreplyto int not null default 0,
	distributionlistprefix varchar(64) not null default '',
	distributionlistfooter varchar(2000) not null default '',
	distributionlistverp int not null default 0,
	distributionlistowner varchar(255) not null default '',
	distributionlistsecret varchar(64) not null default ''
);

CREATE INDEX idx_hm_distributionlists ON hm_distributionlists (distributionlistdomainid);

create table hm_distributionlistsrecipients
(
	distributionlistrecipientid bigserial not null primary key,
	distributionlistrecipientlistid int not null,
	distributionlistrecipientaddress varchar(255),
	distributionlistrecipientdelivery int not null default 0,
	distributionlistrecipientsubscribed bigint not null default 0,
	distributionlistrecipientconfirmed int not null default 1,
	distributionlistrecipienttoken varchar(64) not null default '',
	distributionlistrecipienthardbounces int not null default 0,
	distributionlistrecipientsoftbounces int not null default 0,
	distributionlistrecipientsoftsince bigint not null default 0,
	distributionlistrecipientlastbounce bigint not null default 0,
	distributionlistrecipientreason varchar(255) not null default ''
);

CREATE INDEX idx_hm_distributionlistsrecipients ON hm_distributionlistsrecipients (distributionlistrecipientlistid);

CREATE INDEX idx_hm_dlrecipients_token ON hm_distributionlistsrecipients (distributionlistrecipienttoken);

create table hm_messagerecipients
(
	recipientid bigserial not null primary key,
 	recipientmessageid bigint not null,
	recipientaddress varchar(255) not null,
	recipientlocalaccountid int not null,
	recipientoriginaladdress varchar(255) not null,
	recipientdsnnotify int not null default 0
);

CREATE INDEX idx_hm_messagerecipients ON hm_messagerecipients (recipientmessageid);

create table hm_imapfolders 
(
  folderid bigserial not null primary key,
  folderaccountid int NOT NULL,
  folderparentid int NOT NULL,
  foldername varchar(255) NOT NULL,
  folderissubscribed smallint NOT NULL,
  foldercreationtime timestamp NOT NULL,
  foldercurrentuid bigint NOT NULL,
  foldercurrentmodseq bigint NOT NULL default 1,
  folderspecialuse int NOT NULL default 0
);

CREATE INDEX idx_hm_imapfolders ON hm_imapfolders (folderaccountid);

CREATE UNIQUE INDEX idx_hm_imapfolders_unique ON hm_imapfolders (folderaccountid, folderparentid, foldername);

create table hm_imapexpunged
(
  expungedaccountid int not null,
  expungedfolderid int not null,
  expungeduid bigint not null,
  expungedmodseq bigint not null
);

CREATE INDEX idx_hm_imapexpunged ON hm_imapexpunged (expungedfolderid, expungedmodseq);

create table hm_routes
(
  routeid bigserial not null primary key,
  routedomainname varchar(255) NOT NULL,
  routedescription varchar(255) NOT NULL,
  routetargetsmthost varchar(255) NOT NULL,
  routetargetsmtport int NOT NULL,
  routenooftries int NOT NULL,
  routeminutesbetweentry int NOT NULL,
  routealladdresses smallint NOT NULL,
  routeuseauthentication smallint NOT NULL,
  routeauthenticationusername varchar(255) NOT NULL,
  routeauthenticationpassword varchar(1024) NOT NULL,
  routetreatsecurityaslocal smallint NOT NULL,
  routeconnectionsecurity smallint not null,
  routetreatsenderaslocaldomain smallint NOT NULL
);

create table hm_routeaddresses
(
  routeaddressid bigserial not null primary key,
  routeaddressrouteid int NOT NULL,
  routeaddressaddress varchar(255) NOT NULL
);

create table hm_securityranges
(
	rangeid bigserial not null primary key,
   rangepriorityid int not null,
	rangelowerip1 bigint not null,
   rangelowerip2 bigint null,
	rangeupperip1 bigint not null,
   rangeupperip2 bigint null,
	rangeoptions int not null,
	rangename varchar(100) not null unique,
   rangeexpires smallint not null,
	rangeexpirestime timestamp not null
   
);

create table hm_dnsbl
(
	sblid bigserial not null primary key,
	sblactive smallint not null,
	sbldnshost varchar(255) not null,
	sblresult varchar(255) not null,
	sblrejectmessage varchar(255) not null,
	sblscore int not null
);

create table hm_fetchaccounts
(	
	faid bigserial not null primary key,
	faactive smallint not null,
	faaccountid int not null,
	faaccountname varchar (255) not null,
	faserveraddress varchar (255) not null,
	faserverport int not null,
	faservertype smallint not null,
	fausername varchar (255) not null,
	fapassword varchar (1024) not null,
	faminutes int not null,
	fanexttry timestamp not null,
	fadaystokeep int not null,
	falocked smallint not null,
	faprocessmimerecipients smallint not null,
	faprocessmimedate smallint not null,
	faconnectionsecurity smallint not null,
    fauseantispam smallint not null,
    fauseantivirus smallint not null,
    faenablerouterecipients smallint not null,
    famirrorfolders smallint not null DEFAULT 0,
	famimerecipientheaders varchar(255) not null DEFAULT 'To,CC,X-RCPT-TO,X-Envelope-To',
	faoauthprovider varchar(16) not null DEFAULT '',
	faoauthtoken text not null DEFAULT '',
	famirrorskip text not null DEFAULT '',
	famirrorseen text not null DEFAULT ''
);

create table hm_fetchaccounts_uids
(
	uidid bigserial not null primary key,
	uidfaid int not null,
	uidvalue varchar(255) not null,
	uidtime timestamp not null
);

CREATE INDEX idx_hm_fetchaccounts_uids ON hm_fetchaccounts_uids (uidfaid);

create table hm_apppasswords
(
	apid bigserial not null primary key,
	apaccountid int not null,
	apname varchar(255) not null,
	aphash varchar(255) not null,
	apencryption int not null,
	apcreated timestamp not null,
	aplastused timestamp null,
	apactive smallint not null
);

CREATE INDEX idx_hm_apppasswords ON hm_apppasswords (apaccountid);

create table hm_contacts
(
	contactid bigserial not null primary key,
	contactaccountid int not null,
	contactname varchar(255) not null,
	contactaddress varchar(255) not null,
	contactsource smallint not null,
	contactcreated timestamp not null,
	contacturi varchar(255) not null default '',
	contactuid varchar(255) not null default '',
	contactvcard text not null default '',
	contactbookid int not null default 0
);

CREATE UNIQUE INDEX idx_hm_contacts_account ON hm_contacts (contactaccountid, contactaddress);

CREATE INDEX idx_hm_contacts_book ON hm_contacts (contactbookid);

create table hm_addressbooks
(
	bookid serial not null primary key,
	bookaccountid int not null,
	bookname varchar(255) not null,
	bookcreated bigint not null
);

CREATE UNIQUE INDEX idx_hm_addressbooks_account ON hm_addressbooks (bookaccountid, bookname);

create table hm_addressbookshares
(
	shareid serial not null primary key,
	sharebookid int not null,
	shareaccountid int not null,
	sharedomainid int not null,
	sharewrite int not null,
	sharecreated bigint not null
);

CREATE UNIQUE INDEX idx_hm_addressbookshares_target ON hm_addressbookshares (sharebookid, shareaccountid, sharedomainid);

create table hm_accountprefs
(
	prefid bigserial not null primary key,
	prefaccountid int not null,
	prefname varchar(64) not null,
	prefvalue varchar(4000) not null
);

CREATE UNIQUE INDEX idx_hm_accountprefs_name ON hm_accountprefs (prefaccountid, prefname);

create table hm_scheduled
(
	schedid bigserial not null primary key,
	schedaccountid int not null,
	schedmessageid bigint not null,
	schedaction smallint not null,
	schedat timestamp not null,
	schedfolderid bigint not null,
	schedcreated timestamp not null
);

CREATE INDEX idx_hm_scheduled_account ON hm_scheduled (schedaccountid);

create table hm_files
(
	fileid bigserial not null primary key,
	fileaccountid int not null,
	filetoken varchar(64) not null,
	filename varchar(255) not null,
	filetype varchar(100) not null,
	filesize bigint not null,
	filestored bigint not null,
	filecomplete smallint not null,
	filecreated bigint not null,
	fileexpires bigint not null,
	filepasswordhash varchar(64) not null,
	filedownloads int not null
);

CREATE INDEX idx_hm_files_account ON hm_files (fileaccountid);
CREATE UNIQUE INDEX idx_hm_files_token ON hm_files (filetoken);

create table hm_smimekeys
(
	smimeid bigserial not null primary key,
	smimeaccountid int not null,
	smimekind smallint not null,
	smimeaddress varchar(255) not null,
	smimename varchar(255) not null,
	smimefingerprint varchar(64) not null,
	smimecertificate text not null,
	smimechain text not null,
	smimekey text not null,
	smimenotafter bigint not null,
	smimecreated bigint not null,
	smimeauthority smallint not null default 0
);

CREATE INDEX idx_hm_smimekeys_account ON hm_smimekeys (smimeaccountid);
CREATE UNIQUE INDEX idx_hm_smimekeys_entry ON hm_smimekeys (smimeaccountid, smimekind, smimefingerprint);

create table hm_calendars
(
	calendarid bigserial not null primary key,
	calendaraccountid int not null,
	calendarname varchar(255) not null,
	calendardisplayname varchar(255) not null,
	calendarsynctoken bigint not null,
	calendarcreated bigint not null,
	calendarcolor varchar(16) not null default ''
);

CREATE UNIQUE INDEX idx_hm_calendars_account ON hm_calendars (calendaraccountid, calendarname);

create table hm_calendarobjects
(
	objectid bigserial not null primary key,
	objectaccountid int not null,
	objectcalendarid int not null,
	objecturi varchar(255) not null,
	objectuid varchar(255) not null,
	objectcomponent varchar(16) not null,
	objectdata text not null,
	objectetag varchar(64) not null,
	objectsynctoken bigint not null,
	objectstart bigint not null,
	objectend bigint not null,
	objectfirst bigint not null,
	objectlast bigint not null,
	objectdeleted smallint not null,
	objectmodified bigint not null
);

CREATE UNIQUE INDEX idx_hm_calendarobjects_uri ON hm_calendarobjects (objectcalendarid, objecturi);
CREATE INDEX idx_hm_calendarobjects_sync ON hm_calendarobjects (objectcalendarid, objectsynctoken);

create table hm_calendarshares
(
	shareid serial not null primary key,
	sharecalendarid int not null,
	shareaccountid int not null,
	sharewrite int not null,
	sharegroupid int not null default 0,
	sharedelegate int not null default 0
);

CREATE UNIQUE INDEX idx_hm_calendarshares_pair ON hm_calendarshares (sharecalendarid, shareaccountid, sharegroupid);

create table hm_senderaddresses
(
	said serial not null primary key,
	saaccountid int not null,
	saaddress varchar(255) not null,
	saname varchar(255) not null,
	saverified smallint not null,
	sacreated timestamp not null
);

CREATE UNIQUE INDEX idx_hm_senderaddresses_address ON hm_senderaddresses (saaccountid, saaddress);

create table hm_maskedaddresses
(
	maid serial not null primary key,
	maaccountid int not null,
	maaddress varchar(255) not null,
	malabel varchar(255) not null,
	maactive smallint not null,
	mareceived int not null,
	macreated timestamp not null
);

CREATE UNIQUE INDEX idx_hm_maskedaddresses_address ON hm_maskedaddresses (maaddress);

CREATE INDEX idx_hm_maskedaddresses_account ON hm_maskedaddresses (maaccountid);

create table hm_passkeys
(
	pkid serial not null primary key,
	pkaccountid int null,
	pkcredential varchar(200) not null,
	pkpublickey text not null,
	pkalgorithm int not null,
	pksigncount bigint not null,
	pkname varchar(255) not null,
	pktransports varchar(255) not null,
	pkaaguid varchar(64) not null,
	pkcreated bigint not null,
	pklastused bigint not null
);

CREATE INDEX idx_hm_passkeys_account ON hm_passkeys (pkaccountid);

CREATE UNIQUE INDEX idx_hm_passkeys_credential ON hm_passkeys (pkcredential);

create table hm_directory
(
	dirid serial not null primary key,
	diraccountid int not null,
	dirtitle varchar(100) not null default '',
	dirdepartment varchar(100) not null default '',
	dirphone varchar(60) not null default '',
	dirmobile varchar(60) not null default '',
	diroffice varchar(100) not null default '',
	dirphototype varchar(40) not null default '',
	dirphoto text not null default '',
	diroptout smallint not null default 0,
	dirnote varchar(255) not null default '',
	dirupdated timestamp not null
);

CREATE UNIQUE INDEX idx_hm_directory_account ON hm_directory (diraccountid);

create table hm_recoverable
(
	rcid serial not null primary key,
	rcaccountid int not null,
	rcfolderid int not null,
	rcfolder varchar(255) not null,
	rcfile varchar(255) not null,
	rcsize int not null,
	rcsubject varchar(255) not null,
	rcfrom varchar(255) not null,
	rcflags int not null,
	rcdeleted bigint not null
);

CREATE INDEX idx_hm_recoverable_account ON hm_recoverable (rcaccountid);

create table hm_confidential
(
	cfid serial not null primary key,
	cfaccountid int not null,
	cftoken varchar(64) not null,
	cfsubject varchar(255) not null,
	cfsender varchar(255) not null,
	cfrecipients varchar(2000) not null,
	cffile varchar(255) not null,
	cfpasscode varchar(255) not null,
	cfcreated bigint not null,
	cfexpires bigint not null,
	cfopened bigint not null,
	cfopenedby varchar(255) not null,
	cfrevoked int not null
);

CREATE UNIQUE INDEX idx_hm_confidential_token ON hm_confidential (cftoken);

CREATE INDEX idx_hm_confidential_account ON hm_confidential (cfaccountid);

create table hm_dmarcreports
(
	drid serial not null primary key,
	drdomainid int not null,
	drkey varchar(64) not null,
	drorgname varchar(255) not null,
	dremail varchar(255) not null,
	drreportid varchar(255) not null,
	drbegin bigint not null,
	drend bigint not null,
	drpolicydomain varchar(255) not null,
	dradkim varchar(8) not null,
	draspf varchar(8) not null,
	drp varchar(16) not null,
	drsp varchar(16) not null,
	drpct int not null,
	drmessages int not null,
	drpassed int not null,
	drreceived bigint not null,
	drsource varchar(255) not null,
	drrecords int not null,
	drmsgdmarc varchar(16) not null,
	drmsgfrom varchar(255) not null
);

CREATE UNIQUE INDEX idx_hm_dmarcreports_key ON hm_dmarcreports (drkey);

CREATE INDEX idx_hm_dmarcreports_domain ON hm_dmarcreports (drdomainid);

CREATE INDEX idx_hm_dmarcreports_received ON hm_dmarcreports (drreceived);

create table hm_dmarcreportrecords
(
	rrid serial not null primary key,
	rrreportid int not null,
	rrsourceip varchar(45) not null,
	rrcount int not null,
	rrdisposition varchar(16) not null,
	rrdkim varchar(8) not null,
	rrspf varchar(8) not null,
	rrheaderfrom varchar(255) not null,
	rrenvelopefrom varchar(255) not null,
	rrdkimauth varchar(500) not null,
	rrspfauth varchar(255) not null
);

CREATE INDEX idx_hm_dmarcreportrecords_report ON hm_dmarcreportrecords (rrreportid);

create table hm_tlsreports
(
	trid serial not null primary key,
	trdomainid int not null,
	trkey varchar(64) not null,
	trorgname varchar(255) not null,
	trcontact varchar(255) not null,
	trreportid varchar(255) not null,
	trbegin bigint not null,
	trend bigint not null,
	trpolicytype varchar(16) not null,
	trpolicydomain varchar(255) not null,
	trsuccess int not null,
	trfailure int not null,
	trfailures varchar(2000) not null,
	trreceived bigint not null,
	trsource varchar(255) not null
);

CREATE UNIQUE INDEX idx_hm_tlsreports_key ON hm_tlsreports (trkey);

CREATE INDEX idx_hm_tlsreports_domain ON hm_tlsreports (trdomainid);

CREATE INDEX idx_hm_tlsreports_received ON hm_tlsreports (trreceived);

create table hm_accountactivity
(
	acid bigserial not null primary key,
	acaccountid int not null,
	actime bigint not null,
	acbucket bigint not null,
	achour int not null,
	acrecipients int not null,
	acnewrecipients int not null,
	acaddress varchar(64) not null,
	acsource int not null,
	acmessageid bigint not null,
	acheld int not null
);

CREATE INDEX idx_hm_accountactivity_account ON hm_accountactivity (acaccountid, actime);

create table hm_accountrecipients
(
	arcid bigserial not null primary key,
	arcaccountid int not null,
	arcaddress varchar(255) not null,
	arcfirst bigint not null,
	arclast bigint not null,
	arccount int not null
);

CREATE INDEX idx_hm_accountrecipients_address ON hm_accountrecipients (arcaccountid, arcaddress);

create table hm_accountreview
(
	arvid serial not null primary key,
	arvaccountid int not null,
	arvexempt int not null,
	arvsensitivity int not null,
	arvlocked int not null,
	arvlockedat bigint not null,
	arvunlockedat bigint not null,
	arvalertedat bigint not null,
	arvscore int not null,
	arvsignals varchar(255) not null,
	arvreason varchar(255) not null
);

CREATE UNIQUE INDEX idx_hm_accountreview_account ON hm_accountreview (arvaccountid);

create table hm_heldmessages
(
	hmid bigserial not null primary key,
	hmaccountid int not null,
	hmmessageid bigint not null,
	hmheld bigint not null,
	hmsource int not null,
	hmsender varchar(255) not null,
	hmrecipients varchar(2000) not null,
	hmsubject varchar(255) not null,
	hmsize bigint not null,
	hmreason varchar(255) not null
);

CREATE INDEX idx_hm_heldmessages_account ON hm_heldmessages (hmaccountid);

CREATE INDEX idx_hm_heldmessages_message ON hm_heldmessages (hmmessageid);

create table hm_remotedomainpolicies
(
	policyid bigserial not null primary key,
	policydomainname varchar(255) not null,
	policydescription varchar(255) not null,
	policyactive smallint not null,
	policyoutboundtls int not null,
	policyinboundtls smallint not null,
	policymaxmessagesizekb int not null,
	policymaxconnections int not null,
	policymaxperminute int not null,
	policyallowreplies smallint not null,
	policyallowforwarding smallint not null,
	policycalloutenabled smallint not null,
	policycallouthost varchar(255) not null,
	policycalloutport int not null,
	policycallouttimeout int not null,
	policycalloutcacheminutes int not null,
	policycalloutperminute int not null,
	policyencrypt int not null default 0,
	policyencryptkey text not null default '',
	policyencryptfallback int not null default 0
);

CREATE INDEX idx_hm_remotedomainpolicies_domain ON hm_remotedomainpolicies (policydomainname);

create table hm_rules
(
	ruleid bigserial not null primary key,
	ruleaccountid int not null,
	ruledomainid int not null default 0,
	rulename varchar(100) not null,
	ruleactive smallint not null,
	ruleuseand smallint not null,
	rulesortorder int not null
);

CREATE INDEX idx_rules ON hm_rules (ruleaccountid);

create table hm_rule_criterias
(
	criteriaid bigserial not null primary key,
	criteriaruleid int not null,
	criteriausepredefined smallint not null,
	criteriapredefinedfield smallint not null,
	criteriaheadername varchar(255) not null,
	criteriamatchtype smallint not null,
	criteriamatchvalue varchar(2000) not null
	
);

CREATE INDEX idx_rules_criterias ON hm_rule_criterias (criteriaruleid);

create table hm_rule_actions
(
	actionid bigserial not null primary key,
	actionruleid int not null,
	actiontype smallint not null,
	actionimapfolder varchar(255) not null,
	actionsubject varchar(255) not null,
	actionfromname varchar(255) not null,
	actionfromaddress varchar(255) not null,
	actionto varchar(255) not null,
	actionbody text not null,
	actionfilename varchar(255) not null,
	actionsortorder int not null,
	actionscriptfunction varchar(255) not null,
	actionheader varchar(80) not null,
	actionvalue varchar(255) not null,
    actionrouteid int not null,
	actionabortspamflagged smallint not null
);

CREATE INDEX idx_rules_actions ON hm_rule_actions (actionruleid);

create table hm_surblservers
(
	surblid bigserial not null primary key,
	surblactive smallint not null,
	surblhost varchar(255) not null,
	surblrejectmessage varchar(255) not null,
	surblresult varchar(255) not null default '',
	surblscore int not null
);

create table hm_greylisting_triplets
(
	glid bigserial not null primary key,
	glcreatetime timestamp not null,
	glblockendtime timestamp not null,
	gldeletetime timestamp not null,
	glipaddress1 bigint not null,
   glipaddress2 bigint null,
	glsenderaddress varchar(320) not null,
	glrecipientaddress varchar(320) not null,
	glblockedcount int not null,
	glpassedcount int not null,
	glsenderhash varchar(32) null,
	glrecipienthash varchar(32) null
);

CREATE INDEX idx_hm_greylisting_triplets_key ON hm_greylisting_triplets (glipaddress1, glipaddress2, glsenderhash, glrecipienthash);

create table hm_greylisting_whiteaddresses
(
	whiteid bigserial not null primary key,
	whiteipaddress varchar(255) not null,
	whiteipdescription varchar(255) not null
);

create table hm_greylisting_bypass
(
	glbid bigserial not null primary key,
	glbpattern varchar(255) not null,
	glbmatch int not null,
	glbrequire int not null,
	glbnote varchar(255) not null
);

create table hm_blocked_attachments
(
	baid bigserial not null primary key,
	bawildcard varchar(255) not null,
	badescription varchar(255) not null
);

create table hm_servermessages
(
	smid bigserial not null primary key,
	smname varchar(255) not null,
	smtext text not null
);

create table hm_tcpipports
(
	portid bigserial not null primary key,
	portprotocol smallint not null,
	portnumber int not null,
	portaddress1 bigint not null,
    portaddress2 bigint null,
	portconnectionsecurity smallint not null,
	portsslcertificateid bigint not null,
	portclientcertificatepolicy smallint not null default 0,
	portclientcertificatecafile varchar(255) not null default ''
);

create table hm_whitelist
(
	whiteid bigserial not null primary key,
	whiteloweripaddress1 bigint not null,
   whiteloweripaddress2 bigint null,
	whiteupperipaddress1 bigint not null,
   whiteupperipaddress2 bigint null,
	whiteemailaddress varchar(255) not null,
	whitedescription varchar(255) not null
);

create table hm_blocked_senders
(
	bsid bigserial not null primary key,
	bsaddress varchar(255) not null,
	bsscore int not null,
	bsdescription varchar(255) not null,
	constraint u_bsaddress unique (bsaddress)
);

create table hm_sslcertificates
(
	sslcertificateid bigserial not null primary key,
	sslcertificatename varchar(255) not null,
	sslcertificatefile varchar(255) not null,
	sslprivatekeyfile varchar(255) not null,
	sslprivatekeypassword varchar(1024) not null
	
);

create table hm_groups
(
	groupid bigserial not null primary key,
    groupname varchar(255)
);

create table hm_group_members
(
	memberid bigserial not null primary key,
	membergroupid bigint not null,
	memberaccountid bigint not null
);

create table hm_acl
(
	aclid bigserial not null primary key,
   aclsharefolderid bigint not null,
	aclpermissiontype smallint not null,
	aclpermissiongroupid bigint not null,
	aclpermissionaccountid bigint not null,
	aclvalue bigint not null,
	unique(aclsharefolderid, aclpermissiontype, aclpermissiongroupid, aclpermissionaccountid)
);

create table hm_incoming_relays
(
   relayid serial not null primary key,
   relayname varchar(100) not null,
   relaylowerip1 bigint not null,
   relaylowerip2 bigint null,
   relayupperip1 bigint not null,
   relayupperip2 bigint null
);

create table hm_logon_failures
(
   ipaddress1 bigint not null,
   ipaddress2 bigint null,
   failuretime timestamp not null
);

CREATE INDEX idx_hm_logon_failures_ipaddress ON hm_logon_failures (ipaddress1, ipaddress2);

CREATE INDEX idx_hm_logon_failures_failuretime ON hm_logon_failures (failuretime);

insert into hm_securityranges (rangepriorityid, rangelowerip1, rangelowerip2, rangeupperip1, rangeupperip2, rangeoptions, rangename, rangeexpires, rangeexpirestime) values (10, 0, NULL, 4294967295, NULL, 96203, 'Internet', 0, '2001-01-01');

insert into hm_securityranges (rangepriorityid, rangelowerip1, rangelowerip2, rangeupperip1, rangeupperip2, rangeoptions, rangename, rangeexpires, rangeexpirestime) values (30, 2130706433, NULL, 2130706433, NULL, 71627, 'My computer', 0, '2001-01-01');

insert into hm_servermessages (smname, smtext) values ('VIRUS_FOUND', 'Virus found');

insert into hm_servermessages (smname, smtext) values ('VIRUS_ATTACHMENT_REMOVED', 'Virus found:\r\nThe attachment(s) of this message was removed since a virus was detected in at least one of them.\r\n\r\n');

insert into hm_servermessages (smname, smtext) values ('VIRUS_NOTIFICATION', E'The message below contained a virus and did not\r\nreach some or all of the intended recipients.\r\n\r\n   From: %MACRO_FROM%\r\n   To: %MACRO_TO%\r\n   Sent: %MACRO_SENT%\r\n   Subject: %MACRO_SUBJECT%\r\n\r\nhMailServer\r\n');

insert into hm_servermessages (smname, smtext) values ('SEND_FAILED_NOTIFICATION', E'Your message did not reach some or all of the intended recipients.\r\n\r\n   Sent: %MACRO_SENT%\r\n   Subject: %MACRO_SUBJECT%\r\n\r\nThe following recipient(s) could not be reached:\r\n\r\n%MACRO_RECIPIENTS%\r\n\r\nhMailServer\r\n');

insert into hm_servermessages (smname, smtext) values ('MESSAGE_UNDELIVERABLE', E'Message undeliverable');

insert into hm_servermessages (smname, smtext) values ('MESSAGE_FILE_MISSING', E'The mail server could not deliver the message to you since the file %MACRO_FILE% does not exist on the server.\r\n\r\nThe file may have been deleted by anti virus software running on the server.\r\n\r\nhMailServer');

insert into hm_servermessages (smname, smtext) values ('ATTACHMENT_REMOVED', E'The attachment %MACRO_FILE% was blocked for delivery by the e-mail server. Please contact your system administrator if you have any questions regarding this.\r\n\r\nhMailServer\r\n');

insert into hm_dnsbl (sblactive, sbldnshost, sblresult, sblrejectmessage, sblscore) values (0, 'zen.spamhaus.org','127.0.0.2-8|127.0.0.10-11', 'Rejected by Spamhaus.', 3);

insert into hm_dnsbl (sblactive, sbldnshost, sblresult, sblrejectmessage, sblscore) values (0, 'bl.spamcop.net', '127.0.0.2','Rejected by SpamCop.', 3);

insert into hm_surblservers (surblactive, surblhost, surblrejectmessage, surblscore) values (0, 'multi.surbl.org', 'Rejected by SURBL.', 3);


insert into hm_blocked_attachments (bawildcard, badescription) values ('*.bat', 'Batch processing file');

insert into hm_blocked_attachments (bawildcard, badescription) values ('*.cmd', 'Command file for Windows NT');

insert into hm_blocked_attachments (bawildcard, badescription) values ('*.com', 'Command');

insert into hm_blocked_attachments (bawildcard, badescription) values ('*.cpl', 'Windows Control Panel extension');

insert into hm_blocked_attachments (bawildcard, badescription) values ('*.csh', 'CSH script');

insert into hm_blocked_attachments (bawildcard, badescription) values ('*.exe', 'Executable file');

insert into hm_blocked_attachments (bawildcard, badescription) values ('*.inf', 'Setup file');

insert into hm_blocked_attachments (bawildcard, badescription) values ('*.lnk', 'Windows link file');

insert into hm_blocked_attachments (bawildcard, badescription) values ('*.msi', 'Windows Installer file');

insert into hm_blocked_attachments (bawildcard, badescription) values ('*.msp', 'Windows Installer patch');

insert into hm_blocked_attachments (bawildcard, badescription) values ('*.pif', 'Program Information file');

insert into hm_blocked_attachments (bawildcard, badescription) values ('*.reg', 'Registration key');

insert into hm_blocked_attachments (bawildcard, badescription) values ('*.scf', 'Windows Explorer command');

insert into hm_blocked_attachments (bawildcard, badescription) values ('*.scr', 'Windows Screen saver');

insert into hm_settings (settingname, settingstring, settinginteger) values ('maxpop3connections', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('maxsmtpconnections', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('mirroremailaddress', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('relaymode', '', 2);

insert into hm_settings (settingname, settingstring, settinginteger) values ('authallowplaintext', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('allowmailfromnull', '', 1);

insert into hm_settings (settingname, settingstring, settinginteger) values ('logging', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('logdevice', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('smtpnoofretries', '', 4);

insert into hm_settings (settingname, settingstring, settinginteger) values ('smtpminutesbetweenretries', '', 60);

insert into hm_settings (settingname, settingstring, settinginteger) values ('protocolimap', '', 1);

insert into hm_settings (settingname, settingstring, settinginteger) values ('protocolsmtp', '', 1);

insert into hm_settings (settingname, settingstring, settinginteger) values ('protocolpop3', '', 1);

insert into hm_settings (settingname, settingstring, settinginteger) values ('welcomeimap', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('welcomepop3', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('welcomesmtp', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('smtprelayer', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('maxdelivertythreads', '', 10);

insert into hm_settings (settingname, settingstring, settinginteger) values ('logformat', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('avclamwinenable', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('avclamwinexec', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('avclamwindb', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('avnotifysender', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('avnotifyreceiver', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('avaction', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('sendstatistics', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('hostname', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('smtprelayerusername', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('smtprelayerpassword', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('usesmtprelayerauthentication', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('smtprelayerport', '', 25);

insert into hm_settings (settingname, settingstring, settinginteger) values ('usecustomvirusscanner', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('customvirusscannerexecutable', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('customviursscannerreturnvalue', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('usespf', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('usemxchecks', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('usescriptserver', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('scriptlanguage', 'VBScript', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('maxmessagesize', '', 20480);

insert into hm_settings (settingname, settingstring, settinginteger) values ('usecache', '', 1);

insert into hm_settings (settingname, settingstring, settinginteger) values ('domaincachettl', '', 60);

insert into hm_settings (settingname, settingstring, settinginteger) values ('accountcachettl', '', 60);

insert into hm_settings (settingname, settingstring, settinginteger) values ('awstatsenabled', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('rulelooplimit', '', 5);

insert into hm_settings (settingname, settingstring, settinginteger) values ('backupoptions', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('backupdestination', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('backupencryptionpassphrase', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('defaultdomain', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('avmaxmsgsize', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('smtpdeliverybindtoip', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('enableimapquota', '', 1);

insert into hm_settings (settingname, settingstring, settinginteger) values ('enableimapidle', '', 1);

insert into hm_settings (settingname, settingstring, settinginteger) values ('enableimapacl', '', 1);

insert into hm_settings (settingname, settingstring, settinginteger) values ('maximapconnections', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('enableimapsort', '', 1);

insert into hm_settings (settingname, settingstring, settinginteger) values ('workerthreadpriority', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('ascheckhostinhelo', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('tcpipthreads', '', 15);

-- Secure default: reject messages with incorrect (bare LF) line endings to harden against SMTP smuggling.
insert into hm_settings (settingname, settingstring, settinginteger) values ('smtpallowincorrectlineendings', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('usegreylisting', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('greylistinginitialdelay', '', 30);

insert into hm_settings (settingname, settingstring, settinginteger) values ('greylistinginitialdelete', '', 24);

insert into hm_settings (settingname, settingstring, settinginteger) values ('greylistingfinaldelete', '', 864);

insert into hm_settings (settingname, settingstring, settinginteger) values ('antispamaddheaderspam', '', 1);

insert into hm_settings (settingname, settingstring, settinginteger) values ('antispamaddheaderreason', '', 1);

insert into hm_settings (settingname, settingstring, settinginteger) values ('antispamprependsubject', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('antispamprependsubjecttext', '[SPAM]', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('enableattachmentblocking', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('maxsmtprecipientsinbatch', '', 100);

insert into hm_settings (settingname, settingstring, settinginteger) values ('disconnectinvalidclients', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('maximumincorrectcommands', '', 100);

insert into hm_settings (settingname, settingstring, settinginteger) values ('aliascachettl', '', 60);

insert into hm_settings (settingname, settingstring, settinginteger) values ('distributionlistcachettl', '', 60);

insert into hm_settings (settingname, settingstring, settinginteger) values ('smtprelayerconnectionsecurity', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('adddeliveredtoheader', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('groupcachettl', '', 60);

insert into hm_settings (settingname, settingstring, settinginteger) values ('imappublicfoldername', '#Public', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('antispamenabled', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('usespfscore', '', 3);

insert into hm_settings (settingname, settingstring, settinginteger) values ('ascheckhostinheloscore', '', 2);

insert into hm_settings (settingname, settingstring, settinginteger) values ('usemxchecksscore', '', 2);

insert into hm_settings (settingname, settingstring, settinginteger) values ('spammarkthreshold', '', 5);

insert into hm_settings (settingname, settingstring, settinginteger) values ('spamdeletethreshold', '', 20);

insert into hm_settings (settingname, settingstring, settinginteger) values ('spamassassinenabled', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('spamassassinscore', '', 5);

insert into hm_settings (settingname, settingstring, settinginteger) values ('spamassassinmergescore', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('spamassassinhost', '127.0.0.1', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('spamassassinport', '', 783);

insert into hm_settings (settingname, settingstring, settinginteger) values ('antispammaxsize', '', 1024);

insert into hm_settings (settingname, settingstring, settinginteger) values ('ASDKIMVerificationEnabled', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('ASDKIMVerificationFailureScore', '', 5);

insert into hm_settings (settingname, settingstring, settinginteger) values ('ASDMARCEnabled', '', 1);

insert into hm_settings (settingname, settingstring, settinginteger) values ('ASArcFilteringEnabled', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('QuarantineDigestBaseUrl', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('QuarantineDigestLinkHours', '', 72);

insert into hm_settings (settingname, settingstring, settinginteger) values ('GeoIpDatabaseFile', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('GeoIpDatabaseMaxAgeDays', '', 45);

insert into hm_settings (settingname, settingstring, settinginteger) values ('CountryBlockingExemptRanges', '127.0.0.0/8,::1/128,10.0.0.0/8,172.16.0.0/12,192.168.0.0/16,169.254.0.0/16,100.64.0.0/10,fe80::/10,fc00::/7', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('CountryPolicySMTP', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('CountryPolicyIMAP', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('CountryPolicyPOP3', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('CountryPolicyHTTP', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('SessionIdleSeconds', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('SessionLifetimeSeconds', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('SessionOnePerDevice', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('SessionBindToAddress', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('ReputationWatchEnabled', '', 1);

insert into hm_settings (settingname, settingstring, settinginteger) values ('ReputationWatchLists', 'ipv4:zen.spamhaus.org, ipv4:bl.spamcop.net, ipv4:psbl.surriel.com, ipv4:bl.mailspike.net', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('ReputationWatchAddresses', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('SpamTrapBanMinutes', '', 1440);

insert into hm_settings (settingname, settingstring, settinginteger) values ('SpamTrapBlockDays', '', 30);

insert into hm_settings (settingname, settingstring, settinginteger) values ('SpamTrapHitRetentionDays', '', 90);

insert into hm_settings (settingname, settingstring, settinginteger) values ('SpamTrapTeachSpamAssassin', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('GreyListingSlideOnEarlyRetry', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('GreyListingAbuseThreshold', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('GreyListingAbuseLockMinutes', '', 60);

insert into hm_settings (settingname, settingstring, settinginteger) values ('ASArcTrustedSealers', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('ASDMARCFailureScore', '', 5);

insert into hm_settings (settingname, settingstring, settinginteger) values ('AutoBanOnLogonFailureEnabled', '', 1);

insert into hm_settings (settingname, settingstring, settinginteger) values ('MaxInvalidLogonAttempts', '', 3);

insert into hm_settings (settingname, settingstring, settinginteger) values ('LogonAttemptsWithinMinutes', '', 30);

insert into hm_settings (settingname, settingstring, settinginteger) values ('AutoBanMinutes', '', 60);

insert into hm_settings (settingname, settingstring, settinginteger) values ('IMAPHierarchyDelimiter', '.', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('MaxNumberOfAsynchronousTasks', '', 15);

insert into hm_settings (settingname, settingstring, settinginteger) values ('MessageIndexing', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('MessageStoreCompression', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('MessageStoreDedupMinKB', '', 256);

insert into hm_settings (settingname, settingstring, settinginteger) values ('BypassGreylistingOnSPFSuccess', '', 1);

insert into hm_settings (settingname, settingstring, settinginteger) values ('BypassGreylistingOnMailFromMX', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('MaxNumberOfMXHosts', '', 15);

insert into hm_settings (settingname, settingstring, settinginteger) values ('ClamAVEnabled', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('ClamAVHost', 'localhost', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('ClamAVPort', '', 3310);

insert into hm_settings (settingname, settingstring, settinginteger) values ('SmtpDeliveryConnectionSecurity', '', 2);

insert into hm_settings (settingname, settingstring, settinginteger) values ('VerifyRemoteSslCertificate', '', 1);

insert into hm_settings (settingname, settingstring, settinginteger) values ('SslCipherList', 'ECDHE-RSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES256-GCM-SHA384:ECDHE-ECDSA-AES256-GCM-SHA384:DHE-RSA-AES128-GCM-SHA256:DHE-DSS-AES128-GCM-SHA256:kEDH+AESGCM:ECDHE-RSA-AES128-SHA256:ECDHE-ECDSA-AES128-SHA256:ECDHE-RSA-AES128-SHA:ECDHE-ECDSA-AES128-SHA:ECDHE-RSA-AES256-SHA384:ECDHE-ECDSA-AES256-SHA384:ECDHE-RSA-AES256-SHA:ECDHE-ECDSA-AES256-SHA:DHE-RSA-AES128-SHA256:DHE-RSA-AES128-SHA:DHE-DSS-AES128-SHA256:DHE-RSA-AES256-SHA256:DHE-DSS-AES256-SHA:DHE-RSA-AES256-SHA:AES128-GCM-SHA256:AES256-GCM-SHA384:ECDHE-RSA-RC4-SHA:ECDHE-ECDSA-RC4-SHA:AES128:AES256:RC4-SHA:HIGH:!aNULL:!eNULL:!EXPORT:!DES:!3DES:!MD5:!PSK;', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('SslVersions', '', 24);

insert into hm_settings (settingname, settingstring, settinginteger) values ('ImapMasterUser', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('EnableImapSASLPlain', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('EnableImapSASLInitialResponse', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('ascheckptr', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('ascheckptrscore', '', 1);

insert into hm_settings (settingname, settingstring, settinginteger) values ('IPv6Preferred', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('CreateDefaultSpecialUseFolders', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('TlsOptions', '', 0);

insert into hm_tcpipports (portprotocol, portnumber, portaddress1, portaddress2, portconnectionsecurity, portsslcertificateid) values (1, 25, 0, NULL, 0, 0);

insert into hm_tcpipports (portprotocol, portnumber, portaddress1, portaddress2, portconnectionsecurity, portsslcertificateid) values (1, 587, 0, NULL, 0, 0);

insert into hm_tcpipports (portprotocol, portnumber, portaddress1, portaddress2, portconnectionsecurity, portsslcertificateid) values (3, 110, 0, NULL, 0, 0);

insert into hm_tcpipports (portprotocol, portnumber, portaddress1, portaddress2, portconnectionsecurity, portsslcertificateid) values (5, 143, 0, NULL, 0, 0);

create table hm_imap_metadata
(
	metadataid bigserial not null primary key,
	metadataaccountid bigint not null,
	metadatafolderid bigint not null,
	metadataentryname varchar(255) not null,
	metadatavalue varchar(2048) not null,
	unique(metadataaccountid, metadatafolderid, metadataentryname)
);

create table hm_quarantine
(
	quarantineid bigserial not null primary key,
	quarantinefilename varchar(255) not null,
	quarantinesender varchar(255) not null,
	quarantinerecipients varchar(1000) not null,
	quarantinesubject varchar(255) not null,
	quarantinereason varchar(255) not null,
	quarantinescore int not null,
	quarantinesize int not null,
	quarantinecreated timestamp not null
);

CREATE INDEX idx_hm_quarantine_created ON hm_quarantine (quarantinecreated);

create table hm_senderlist
(
	slid bigserial not null primary key,
	slscope int not null,
	slownerid int not null,
	sltype int not null,
	slvalue varchar(255) not null,
	slreason varchar(255) not null,
	slexpires smallint not null,
	slexpirestime timestamp not null,
	slcreated timestamp not null
);

CREATE INDEX idx_hm_senderlist_scope ON hm_senderlist (slscope, slownerid);

create table hm_digeststate
(
	dsid bigserial not null primary key,
	dsaccountid int not null,
	dssenttime timestamp not null,
	dslastid int not null,
	dsoptout smallint not null
);

CREATE UNIQUE INDEX idx_hm_digeststate_account ON hm_digeststate (dsaccountid);

create table hm_digesttokens
(
	dtid bigserial not null primary key,
	dtaccountid int not null,
	dtkind int not null,
	dtquarantineid int not null,
	dttoken varchar(64) not null,
	dtexpirestime timestamp not null,
	dtused smallint not null,
	dtcreated timestamp not null
);

CREATE UNIQUE INDEX idx_hm_digesttokens_token ON hm_digesttokens (dttoken);

create table hm_passwordhistory
(
	phid bigserial not null primary key,
	phaccountid int not null,
	phhash varchar(255) not null,
	phencryption int not null,
	phchanged timestamp not null
);

CREATE INDEX idx_hm_passwordhistory ON hm_passwordhistory (phaccountid);

create table hm_messagetrace
(
	mtid bigserial not null primary key,
	mtqueueid int not null,
	mtoccurred timestamp not null,
	mtevent varchar(32) not null,
	mtsender varchar(255) not null,
	mtrecipient varchar(255) not null,
	mtsourceip varchar(64) not null,
	mtstatuscode int not null,
	mtdetail varchar(255) not null
);

CREATE INDEX idx_hm_messagetrace ON hm_messagetrace (mtoccurred);

insert into hm_servermessages (smname, smtext) values ('QUOTA_WARNING', E'Your mailbox is %MACRO_PERCENT%% full.\r\n\r\n   Mailbox: %MACRO_MAILBOX%\r\n   Used: %MACRO_USED% MB of %MACRO_QUOTA% MB\r\n\r\nWhen it is completely full, new messages sent to you will be refused and\r\nthe sender will be told to try again later. Deleting mail you no longer\r\nneed - and emptying the trash folder afterwards - will free the space.\r\n\r\nhMailServer\r\n');

insert into hm_servermessages (smname, smtext) values ('QUOTA_WARNING_SUBJECT', 'Your mailbox is almost full');

create table hm_messageindexterms
(
	mitmessageid bigint not null,
	mitaccountid int not null,
	mitterm varchar(64) not null
);

CREATE INDEX idx_hm_messageindexterms_lookup ON hm_messageindexterms (mitaccountid, mitterm);

CREATE INDEX idx_hm_messageindexterms_message ON hm_messageindexterms (mitmessageid);

create table hm_messageindexstate
(
	misaccountid int not null,
	mishighwatermark bigint not null
);

CREATE INDEX idx_hm_messageindexstate ON hm_messageindexstate (misaccountid);

create table hm_metricsamples
(
	metricsampleid bigserial not null primary key,
	metricsampletime timestamp not null,
	metricsamplename varchar(64) not null,
	metricsamplevalue double precision not null
);

CREATE INDEX idx_hm_metricsamples_name_time ON hm_metricsamples (metricsamplename, metricsampletime);

create table hm_archiveindex
(
	archiveid bigserial not null primary key,
	archivetime timestamp not null,
	archivedomain varchar(255) not null,
	archivemailbox varchar(255) not null,
	archivedirection int not null,
	archivesender varchar(255) not null,
	archiverecipients varchar(1000) not null,
	archivesubject varchar(255) not null,
	archivemessageid varchar(255) not null,
	archivepath varchar(1000) not null,
	archivesize bigint not null,
	archivehold int not null
);

CREATE INDEX idx_hm_archiveindex_domain_time ON hm_archiveindex (archivedomain, archivetime);

CREATE INDEX idx_hm_archiveindex_path ON hm_archiveindex (archivepath);

ALTER TABLE hm_accounts ADD CONSTRAINT fk_hm_accounts_domain FOREIGN KEY (accountdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE;

ALTER TABLE hm_aliases ADD CONSTRAINT fk_hm_aliases_domain FOREIGN KEY (aliasdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE;

ALTER TABLE hm_domain_aliases ADD CONSTRAINT fk_hm_domain_aliases_domain FOREIGN KEY (dadomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE;

ALTER TABLE hm_distributionlists ADD CONSTRAINT fk_hm_distributionlists_domain FOREIGN KEY (distributionlistdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE;

ALTER TABLE hm_distributionlistsrecipients ADD CONSTRAINT fk_hm_dlrecipients_list FOREIGN KEY (distributionlistrecipientlistid) REFERENCES hm_distributionlists (distributionlistid) ON DELETE CASCADE;

ALTER TABLE hm_routeaddresses ADD CONSTRAINT fk_hm_routeaddresses_route FOREIGN KEY (routeaddressrouteid) REFERENCES hm_routes (routeid) ON DELETE CASCADE;

ALTER TABLE hm_fetchaccounts ADD CONSTRAINT fk_hm_fetchaccounts_account FOREIGN KEY (faaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

ALTER TABLE hm_fetchaccounts_uids ADD CONSTRAINT fk_hm_fetchaccounts_uids_fa FOREIGN KEY (uidfaid) REFERENCES hm_fetchaccounts (faid) ON DELETE CASCADE;

ALTER TABLE hm_apppasswords ADD CONSTRAINT fk_hm_apppasswords_account FOREIGN KEY (apaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

ALTER TABLE hm_contacts ADD CONSTRAINT fk_hm_contacts_account FOREIGN KEY (contactaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

ALTER TABLE hm_addressbooks ADD CONSTRAINT fk_hm_addressbooks_account FOREIGN KEY (bookaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

ALTER TABLE hm_addressbookshares ADD CONSTRAINT fk_hm_addressbookshares_book FOREIGN KEY (sharebookid) REFERENCES hm_addressbooks (bookid) ON DELETE CASCADE;

ALTER TABLE hm_accountprefs ADD CONSTRAINT fk_hm_accountprefs_account FOREIGN KEY (prefaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

ALTER TABLE hm_scheduled ADD CONSTRAINT fk_hm_scheduled_account FOREIGN KEY (schedaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

ALTER TABLE hm_files ADD CONSTRAINT fk_hm_files_account FOREIGN KEY (fileaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

ALTER TABLE hm_smimekeys ADD CONSTRAINT fk_hm_smimekeys_account FOREIGN KEY (smimeaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

create table hm_knownsenders
(
	ksid bigserial not null primary key,
	ksaccountid int not null,
	ksaddress varchar(255) not null,
	kscount int not null,
	ksfirstseen varchar(32) not null,
	kslastseen varchar(32) not null
);

CREATE UNIQUE INDEX idx_hm_knownsenders_account ON hm_knownsenders (ksaccountid, ksaddress);

create table hm_autoreplies
(
	arid bigserial not null primary key,
	araccountid int not null,
	arsender varchar(255) not null,
	arsent bigint not null
);

CREATE UNIQUE INDEX idx_hm_autoreplies_account ON hm_autoreplies (araccountid, arsender);

create table hm_domainsamples
(
	dsid serial not null primary key,
	dsday varchar(10) not null,
	dsdomain varchar(255) not null,
	dsspam int not null,
	dsviruses int not null,
	dsstorebytes bigint not null,
	dsstoremessages bigint not null
);

CREATE UNIQUE INDEX idx_hm_domainsamples_day_domain ON hm_domainsamples (dsday, dsdomain);

create table hm_dkimrotations
(
	dkrid bigserial not null primary key,
	dkrdomainid int not null,
	dkrcurrentselector varchar(255) not null,
	dkrcurrentkeyfile varchar(255) not null,
	dkrcurrentgenerated int not null,
	dkrcurrentsince bigint not null,
	dkrcurrentstatus int not null,
	dkrcurrentchecked bigint not null,
	dkrcurrentdetail varchar(255) not null,
	dkrnextselector varchar(255) not null,
	dkrnextkeyfile varchar(255) not null,
	dkrnextgenerated int not null,
	dkrnextcreated bigint not null,
	dkrnextfirstseen bigint not null,
	dkrnextstatus int not null,
	dkrnextchecked bigint not null,
	dkrnextdetail varchar(255) not null,
	dkrretiringselector varchar(255) not null,
	dkrretiringkeyfile varchar(255) not null,
	dkrretiringgenerated int not null,
	dkrretiringsince bigint not null,
	dkrretiringstatus int not null,
	dkrretiringchecked bigint not null,
	dkrretiringdetail varchar(255) not null,
	dkrretiredselector varchar(255) not null,
	dkrretiredat bigint not null,
	dkrretiredkeyfile varchar(255) not null,
	dkrlasterror varchar(255) not null,
	dkrlasterrortime bigint not null
);

CREATE UNIQUE INDEX idx_hm_dkimrotations_domain ON hm_dkimrotations (dkrdomainid);

create table hm_spamtraps
(
	stid bigserial not null primary key,
	stdomainid int not null,
	stlocalpart varchar(64) not null,
	stcreated timestamp not null
);

CREATE UNIQUE INDEX idx_hm_spamtraps_address ON hm_spamtraps (stdomainid, stlocalpart);

create table hm_spamtraphits
(
	sthid bigserial not null primary key,
	sthdomainid int not null,
	sthaddress varchar(255) not null,
	sthipaddress varchar(64) not null,
	sthsender varchar(255) not null,
	sthtime timestamp not null,
	sthbanned smallint not null,
	sthblocked smallint not null,
	sthnote varchar(255) not null
);

CREATE INDEX idx_hm_spamtraphits_time ON hm_spamtraphits (sthtime);

CREATE INDEX idx_hm_spamtraphits_domain ON hm_spamtraphits (sthdomainid);

ALTER TABLE hm_calendars ADD CONSTRAINT fk_hm_calendars_account FOREIGN KEY (calendaraccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

ALTER TABLE hm_calendarobjects ADD CONSTRAINT fk_hm_calendarobjects_calendar FOREIGN KEY (objectcalendarid) REFERENCES hm_calendars (calendarid) ON DELETE CASCADE;

ALTER TABLE hm_calendarshares ADD CONSTRAINT fk_hm_calendarshares_calendar FOREIGN KEY (sharecalendarid) REFERENCES hm_calendars (calendarid) ON DELETE CASCADE;

ALTER TABLE hm_senderaddresses ADD CONSTRAINT fk_hm_senderaddresses_account FOREIGN KEY (saaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

ALTER TABLE hm_maskedaddresses ADD CONSTRAINT fk_hm_maskedaddresses_account FOREIGN KEY (maaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

ALTER TABLE hm_recoverable ADD CONSTRAINT fk_hm_recoverable_account FOREIGN KEY (rcaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

ALTER TABLE hm_passkeys ADD CONSTRAINT fk_hm_passkeys_account FOREIGN KEY (pkaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

ALTER TABLE hm_directory ADD CONSTRAINT fk_hm_directory_account FOREIGN KEY (diraccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

ALTER TABLE hm_confidential ADD CONSTRAINT fk_hm_confidential_account FOREIGN KEY (cfaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

ALTER TABLE hm_dmarcreports ADD CONSTRAINT fk_hm_dmarcreports_domain FOREIGN KEY (drdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE;

ALTER TABLE hm_dmarcreportrecords ADD CONSTRAINT fk_hm_dmarcreportrecords_report FOREIGN KEY (rrreportid) REFERENCES hm_dmarcreports (drid) ON DELETE CASCADE;

ALTER TABLE hm_tlsreports ADD CONSTRAINT fk_hm_tlsreports_domain FOREIGN KEY (trdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE;

ALTER TABLE hm_accountactivity ADD CONSTRAINT fk_hm_accountactivity_account FOREIGN KEY (acaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

ALTER TABLE hm_accountrecipients ADD CONSTRAINT fk_hm_accountrecipients_account FOREIGN KEY (arcaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

ALTER TABLE hm_accountreview ADD CONSTRAINT fk_hm_accountreview_account FOREIGN KEY (arvaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

ALTER TABLE hm_heldmessages ADD CONSTRAINT fk_hm_heldmessages_account FOREIGN KEY (hmaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

ALTER TABLE hm_knownsenders ADD CONSTRAINT fk_hm_knownsenders_account FOREIGN KEY (ksaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

ALTER TABLE hm_autoreplies ADD CONSTRAINT fk_hm_autoreplies_account FOREIGN KEY (araccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

ALTER TABLE hm_dkimrotations ADD CONSTRAINT fk_hm_dkimrotations_domain FOREIGN KEY (dkrdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE;

ALTER TABLE hm_spamtraps ADD CONSTRAINT fk_hm_spamtraps_domain FOREIGN KEY (stdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE;

ALTER TABLE hm_spamtraphits ADD CONSTRAINT fk_hm_spamtraphits_domain FOREIGN KEY (sthdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE;

ALTER TABLE hm_rule_criterias ADD CONSTRAINT fk_hm_rule_criterias_rule FOREIGN KEY (criteriaruleid) REFERENCES hm_rules (ruleid) ON DELETE CASCADE;

ALTER TABLE hm_rule_actions ADD CONSTRAINT fk_hm_rule_actions_rule FOREIGN KEY (actionruleid) REFERENCES hm_rules (ruleid) ON DELETE CASCADE;

ALTER TABLE hm_group_members ADD CONSTRAINT fk_hm_group_members_group FOREIGN KEY (membergroupid) REFERENCES hm_groups (groupid) ON DELETE CASCADE;

ALTER TABLE hm_passwordhistory ADD CONSTRAINT fk_hm_passwordhistory_account FOREIGN KEY (phaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

ALTER TABLE hm_digeststate ADD CONSTRAINT fk_hm_digeststate_account FOREIGN KEY (dsaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

ALTER TABLE hm_digesttokens ADD CONSTRAINT fk_hm_digesttokens_account FOREIGN KEY (dtaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

ALTER TABLE hm_messagerecipients ADD CONSTRAINT fk_hm_messagerecipients_message FOREIGN KEY (recipientmessageid) REFERENCES hm_messages (messageid) ON DELETE CASCADE;

ALTER TABLE hm_message_metadata ADD CONSTRAINT fk_hm_message_metadata_message FOREIGN KEY (metadata_messageid) REFERENCES hm_messages (messageid) ON DELETE CASCADE;

ALTER TABLE hm_imapexpunged ADD CONSTRAINT fk_hm_imapexpunged_folder FOREIGN KEY (expungedfolderid) REFERENCES hm_imapfolders (folderid) ON DELETE CASCADE;

ALTER TABLE hm_messageindexterms ADD CONSTRAINT fk_hm_messageindexterms_message FOREIGN KEY (mitmessageid) REFERENCES hm_messages (messageid) ON DELETE CASCADE;

create table hm_audit
(
	auditid bigserial not null primary key,
	audittime bigint not null,
	auditactor varchar(255) not null,
	auditactortype varchar(32) not null,
	auditinterface varchar(32) not null,
	auditaddress varchar(64) not null,
	auditobjecttype varchar(64) not null,
	auditobjectname varchar(255) not null,
	auditaction varchar(32) not null,
	auditdetail text not null,
	auditprevhash varchar(64) not null,
	audithash varchar(64) not null
);

CREATE INDEX idx_hm_audit_time ON hm_audit (audittime);

CREATE INDEX idx_hm_audit_actor ON hm_audit (auditactor);

create table hm_alertrules
(
	alertruleid bigserial not null primary key,
	alertrulecondition varchar(64) not null,
	alertruleenabled smallint not null,
	alertrulethreshold int not null,
	alertruleactions int not null,
	alertruleaddress varchar(255) not null,
	alertrulewebhook varchar(1024) not null,
	alertrulesecret varchar(255) not null,
	alertrulecooldown int not null,
	alertruledigest smallint not null
);

CREATE UNIQUE INDEX idx_hm_alertrules_condition ON hm_alertrules (alertrulecondition);

create table hm_alertevents
(
	alerteventid bigserial not null primary key,
	alerteventcondition varchar(64) not null,
	alerteventseverity int not null,
	alerteventstate int not null,
	alerteventtime bigint not null,
	alerteventsummary varchar(255) not null,
	alerteventdetail text not null,
	alerteventnotified smallint not null,
	alerteventdigested smallint not null,
	alerteventhookstate int not null,
	alerteventhooktries int not null,
	alerteventhooknext bigint not null
);

CREATE INDEX idx_hm_alertevents_time ON hm_alertevents (alerteventtime);

CREATE INDEX idx_hm_alertevents_condition ON hm_alertevents (alerteventcondition, alerteventtime);

insert into hm_alertrules (alertrulecondition, alertruleenabled, alertrulethreshold, alertruleactions, alertruleaddress, alertrulewebhook, alertrulesecret, alertrulecooldown, alertruledigest) values ('backup.failed', 1, 0, 1, '', '', '', 60, 0);

insert into hm_alertrules (alertrulecondition, alertruleenabled, alertrulethreshold, alertruleactions, alertruleaddress, alertrulewebhook, alertrulesecret, alertrulecooldown, alertruledigest) values ('certificate.expiring', 1, 7, 1, '', '', '', 1440, 1);

insert into hm_alertrules (alertrulecondition, alertruleenabled, alertrulethreshold, alertruleactions, alertruleaddress, alertrulewebhook, alertrulesecret, alertrulecooldown, alertruledigest) values ('disk.low', 0, 0, 1, '', '', '', 60, 1);

insert into hm_alertrules (alertrulecondition, alertruleenabled, alertrulethreshold, alertruleactions, alertruleaddress, alertrulewebhook, alertrulesecret, alertrulecooldown, alertruledigest) values ('queue.stalled', 0, 0, 1, '', '', '', 60, 1);

insert into hm_alertrules (alertrulecondition, alertruleenabled, alertrulethreshold, alertruleactions, alertruleaddress, alertrulewebhook, alertrulesecret, alertrulecooldown, alertruledigest) values ('autoban.storm', 0, 25, 1, '', '', '', 60, 1);

insert into hm_alertrules (alertrulecondition, alertruleenabled, alertrulethreshold, alertruleactions, alertruleaddress, alertrulewebhook, alertrulesecret, alertrulecooldown, alertruledigest) values ('minidump.written', 0, 0, 1, '', '', '', 60, 1);

insert into hm_alertrules (alertrulecondition, alertruleenabled, alertrulethreshold, alertruleactions, alertruleaddress, alertrulewebhook, alertrulesecret, alertrulecooldown, alertruledigest) values ('dkim.rotation.stalled', 1, 7, 1, '', '', '', 1440, 1);

insert into hm_alertrules (alertrulecondition, alertruleenabled, alertrulethreshold, alertruleactions, alertruleaddress, alertrulewebhook, alertrulesecret, alertrulecooldown, alertruledigest) values ('reputation.listed', 1, 0, 1, '', '', '', 1440, 0);

insert into hm_alertrules (alertrulecondition, alertruleenabled, alertrulethreshold, alertruleactions, alertruleaddress, alertrulewebhook, alertrulesecret, alertrulecooldown, alertruledigest) values ('account.compromised', 1, 0, 1, '', '', '', 0, 0);

create table hm_reputationchecks
(
	rcid bigserial not null primary key,
	rcaddress varchar(64) not null,
	rcsources varchar(64) not null,
	rczone varchar(255) not null,
	rcstatus int not null,
	rccode varchar(255) not null,
	rcreason varchar(255) not null,
	rcchecked bigint not null,
	rclistedsince bigint not null,
	rcconfirmed bigint not null
);

insert into hm_settings (settingname, settingstring, settinginteger) values ('AlertsEnabled', '', 1);

insert into hm_settings (settingname, settingstring, settinginteger) values ('AlertRecipient', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('AlertSenderAddress', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('AlertDigestEnabled', '', 1);

insert into hm_settings (settingname, settingstring, settinginteger) values ('AlertDigestHour', '', 7);

insert into hm_settings (settingname, settingstring, settinginteger) values ('AlertMaxPerHour', '', 20);

insert into hm_settings (settingname, settingstring, settinginteger) values ('AlertWebhookMaxAttempts', '', 5);

insert into hm_settings (settingname, settingstring, settinginteger) values ('AuditTrailEnabled', '', 1);

insert into hm_settings (settingname, settingstring, settinginteger) values ('AuditRetentionDays', '', 0);

create table hm_roles
(
	roleid bigserial not null primary key,
	rolename varchar(64) not null,
	roledescription varchar(255) not null,
	rolebuiltin smallint not null,
	rolepermissions varchar(1000) not null
);

CREATE UNIQUE INDEX idx_hm_roles_name ON hm_roles (rolename);

create table hm_roleassignments
(
	raid bigserial not null primary key,
	raaccountid int not null,
	raroleid int not null,
	radomains varchar(1000) not null
);

CREATE UNIQUE INDEX idx_hm_roleassignments_account ON hm_roleassignments (raaccountid);

ALTER TABLE hm_roleassignments ADD CONSTRAINT fk_hm_roleassignments_account FOREIGN KEY (raaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

ALTER TABLE hm_roleassignments ADD CONSTRAINT fk_hm_roleassignments_role FOREIGN KEY (raroleid) REFERENCES hm_roles (roleid);

insert into hm_roles (rolename, roledescription, rolebuiltin, rolepermissions) values ('serveradmin', 'Every permission, in every domain.', 1, 'administer_server,administer_domain,view_configuration,view_accounts,view_mailbox,view_queue,view_quarantine,view_archive,view_logs,view_message_trace,view_audit,manage_accounts,reset_password,unlock_account,manage_mailbox,manage_queue,manage_quarantine,manage_archive,manage_settings,manage_domains,manage_server,manage_roles,send_through_api,provision,view_discovery,manage_discovery');

insert into hm_roles (rolename, roledescription, rolebuiltin, rolepermissions) values ('domainadmin', 'The accounts, mailboxes and quarantine of named domains.', 1, 'administer_domain,view_configuration,view_accounts,view_mailbox,view_quarantine,view_message_trace,manage_accounts,reset_password,unlock_account,manage_mailbox,manage_quarantine');

insert into hm_roles (rolename, roledescription, rolebuiltin, rolepermissions) values ('helpdesk', 'Resets a password, unlocks an account and reads a message trace.', 1, 'view_accounts,view_message_trace,reset_password,unlock_account');

insert into hm_roles (rolename, roledescription, rolebuiltin, rolepermissions) values ('auditor', 'Reads everything and changes nothing, the audit trail included.', 1, 'view_configuration,view_accounts,view_mailbox,view_queue,view_quarantine,view_archive,view_logs,view_message_trace,view_audit,view_discovery');

create table hm_signins
(
	siid bigserial not null primary key,
	siaccountid int not null,
	sidevice varchar(32) not null,
	siprotocol int not null,
	siaddress varchar(64) not null,
	siagent varchar(255) not null,
	sicredential int not null,
	sicredentialname varchar(64) not null,
	sifirstseen timestamp not null,
	silastseen timestamp not null,
	sicount int not null,
	sirevoked smallint not null,
	sirevokedtime timestamp null
);

CREATE UNIQUE INDEX idx_hm_signins_device ON hm_signins (siaccountid, sidevice);

create table hm_bouncesuppressions
(
	bsid bigserial not null primary key,
	bsdomainid int not null,
	bsaddress varchar(255) not null,
	bssource int not null,
	bscategory int not null,
	bsstatus varchar(16) not null,
	bsdiagnostic varchar(255) not null,
	bsremotemta varchar(255) not null,
	bscreated varchar(32) not null,
	bsexpires varchar(32) not null
);

CREATE UNIQUE INDEX idx_hm_bouncesuppressions_address ON hm_bouncesuppressions (bsdomainid, bsaddress);

create table hm_bouncecounts
(
	bcid bigserial not null primary key,
	bcdomainid int not null,
	bcaddress varchar(255) not null,
	bchard int not null,
	bcsoft int not null,
	bcmailboxfull int not null,
	bcpolicy int not null,
	bcother int not null,
	bcconsecutivehard int not null,
	bccomplaints int not null,
	bclaststatus varchar(16) not null,
	bclastcategory int not null,
	bclastbounce varchar(32) not null,
	bclastdelivered varchar(32) not null,
	bclastcomplaint varchar(32) not null
);

CREATE UNIQUE INDEX idx_hm_bouncecounts_address ON hm_bouncecounts (bcdomainid, bcaddress);

create table hm_bouncereportkeys
(
	brkid bigserial not null primary key,
	brkdomainid int not null,
	brkkey varchar(64) not null,
	brksent varchar(32) not null,
	brkaddress varchar(64) not null,
	brkrun int not null
);

CREATE INDEX idx_hm_bouncereportkeys_key ON hm_bouncereportkeys (brkkey);

CREATE INDEX idx_hm_bouncereportkeys_sent ON hm_bouncereportkeys (brksent);

CREATE INDEX idx_hm_bouncereportkeys_address ON hm_bouncereportkeys (brkaddress);

ALTER TABLE hm_bouncesuppressions ADD CONSTRAINT fk_hm_bouncesuppressions_domain FOREIGN KEY (bsdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE;

ALTER TABLE hm_bouncecounts ADD CONSTRAINT fk_hm_bouncecounts_domain FOREIGN KEY (bcdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE;

ALTER TABLE hm_bouncereportkeys ADD CONSTRAINT fk_hm_bouncereportkeys_domain FOREIGN KEY (brkdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE;

create table hm_pgpkeys
(
	pgpkid serial not null primary key,
	pgpkaccountid int not null,
	pgpkfingerprint varchar(64) not null,
	pgpkversion int not null,
	pgpkalgorithm int not null,
	pgpkbits int not null,
	pgpkuserid varchar(255) not null,
	pgpkcreated bigint not null,
	pgpkexpires bigint not null,
	pgpkbytes int not null,
	pgpkkey text not null,
	pgpkuploaded bigint not null,
	pgpkuploadedby varchar(255) not null
);

CREATE INDEX idx_hm_pgpkeys_account ON hm_pgpkeys (pgpkaccountid);

CREATE UNIQUE INDEX idx_hm_pgpkeys_entry ON hm_pgpkeys (pgpkaccountid, pgpkfingerprint);

ALTER TABLE hm_pgpkeys ADD CONSTRAINT fk_hm_pgpkeys_account FOREIGN KEY (pgpkaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

create table hm_pgpring
(
	pgrid bigserial not null primary key,
	pgraccountid int not null,
	pgrkind smallint not null,
	pgraddress varchar(255) not null,
	pgrname varchar(255) not null,
	pgrfingerprint varchar(64) not null,
	pgrkeyid varchar(16) not null,
	pgralgorithm varchar(32) not null,
	pgrpublic text not null,
	pgrsecret text not null,
	pgrcreated bigint not null,
	pgrexpires bigint not null,
	pgrrevoked int not null,
	pgrsource varchar(16) not null,
	pgrprefer int not null,
	pgrfirstseen bigint not null,
	pgrlastseen bigint not null,
	pgrverified int not null default 0,
	pgrverifiedat bigint not null default 0,
	pgrchangedfrom varchar(64) not null default '',
	pgrchangedat bigint not null default 0,
	pgraccepted int not null default 0
);

CREATE INDEX idx_hm_pgpring_account ON hm_pgpring (pgraccountid);

CREATE UNIQUE INDEX idx_hm_pgpring_entry ON hm_pgpring (pgraccountid, pgrkind, pgrfingerprint);

ALTER TABLE hm_pgpring ADD CONSTRAINT fk_hm_pgpring_account FOREIGN KEY (pgraccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

create table hm_protectedmessages
(
	pmid bigserial not null primary key,
	pmaccountid int not null,
	pmtoken varchar(32) not null,
	pmrecipient varchar(255) not null,
	pmhint varchar(255) not null,
	pmfile varchar(255) not null,
	pmsize bigint not null,
	pmstored bigint not null,
	pmcomplete int not null,
	pmcreated bigint not null,
	pmexpires bigint not null,
	pmopened bigint not null,
	pmrevoked int not null,
	pmreplyto varchar(32) not null
);

CREATE UNIQUE INDEX idx_hm_protectedmessages_token ON hm_protectedmessages (pmtoken);

CREATE INDEX idx_hm_protectedmessages_account ON hm_protectedmessages (pmaccountid);

ALTER TABLE hm_protectedmessages ADD CONSTRAINT fk_hm_protectedmessages_account FOREIGN KEY (pmaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

insert into hm_settings (settingname, settingstring, settinginteger) values ('ProtectedMessageDays', '', 30);

create table hm_zeroaccess
(
	zaid serial not null primary key,
	zaaccountid int not null,
	zafingerprint varchar(64) not null,
	zasalt varchar(64) not null,
	zapasses int not null,
	zamemory int not null,
	zalanes int not null,
	zasecret text not null,
	zarecovery text not null,
	zaenabledat bigint not null
);

CREATE UNIQUE INDEX idx_hm_zeroaccess_account ON hm_zeroaccess (zaaccountid);

ALTER TABLE hm_zeroaccess ADD CONSTRAINT fk_hm_zeroaccess_account FOREIGN KEY (zaaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

insert into hm_settings (settingname, settingstring, settinginteger) values ('ZeroAccessAllowed', '', 0);

create table hm_accountkeys
(
	akid serial not null primary key,
	akaccountid int not null,
	akenabled int not null,
	akpublic varchar(64) not null,
	akpassword varchar(512) not null,
	akrecovery varchar(512) not null,
	akrecoverypublic varchar(64) not null,
	akpending varchar(2000) not null,
	akcreated bigint not null,
	akchanged bigint not null
);

CREATE UNIQUE INDEX idx_hm_accountkeys_account ON hm_accountkeys (akaccountid);

ALTER TABLE hm_accountkeys ADD CONSTRAINT fk_hm_accountkeys_account FOREIGN KEY (akaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

create table hm_domainrecovery
(
	drid serial not null primary key,
	drdomainid int not null,
	drpublic varchar(64) not null,
	drsealed varchar(512) not null,
	drhint varchar(255) not null,
	drcreated bigint not null,
	drprevpublic varchar(64) not null,
	drprevsealed varchar(512) not null
);

CREATE UNIQUE INDEX idx_hm_domainrecovery_domain ON hm_domainrecovery (drdomainid);

ALTER TABLE hm_domainrecovery ADD CONSTRAINT fk_hm_domainrecovery_domain FOREIGN KEY (drdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE;

insert into hm_settings (settingname, settingstring, settinginteger) values ('MailboxKeyIdleMinutes', '', 15);

create table hm_recall
(
	recallid serial not null primary key,
	recallsenderid int not null,
	recallmsgid varchar(255) not null,
	recallrecipientid int not null,
	recallqueueid bigint not null,
	recallmessageid bigint not null,
	recallcreated bigint not null,
	recallrecalledat bigint not null,
	recalloutcome int not null
);

CREATE INDEX idx_hm_recall_sender ON hm_recall (recallsenderid);

CREATE INDEX idx_hm_recall_queue ON hm_recall (recallqueueid);

CREATE INDEX idx_hm_recall_message ON hm_recall (recallmessageid);

CREATE INDEX idx_hm_recall_created ON hm_recall (recallcreated);

ALTER TABLE hm_recall ADD CONSTRAINT fk_hm_recall_sender FOREIGN KEY (recallsenderid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

insert into hm_settings (settingname, settingstring, settinginteger) values ('RecallDays', '', 14);

insert into hm_settings (settingname, settingstring, settinginteger) values ('OpenPGPWebKeyDirectoryEnabled', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('OpenPGPKeyMaximumBytes', '', 65536);

insert into hm_settings (settingname, settingstring, settinginteger) values ('OpenPGPKeysPerAccount', '', 8);

insert into hm_settings (settingname, settingstring, settinginteger) values ('OpenPGPRequireAddressInUserId', '', 1);

create table hm_webhooks
(
	whid bigserial not null primary key,
	whname varchar(64) not null,
	whurl varchar(1024) not null,
	whsecret varchar(1024) not null,
	whevents varchar(255) not null,
	whdomains varchar(1024) not null,
	whaddresses varchar(1024) not null,
	whenabled int not null,
	whmaxperminute int not null,
	whincludesubject int not null,
	whcreated bigint not null
);

CREATE UNIQUE INDEX idx_hm_webhooks_name ON hm_webhooks (whname);

create table hm_webhookqueue
(
	wqid bigserial not null primary key,
	wqwebhookid bigint not null,
	wqevent varchar(64) not null,
	wqcreated bigint not null,
	wqstate int not null,
	wqtries int not null,
	wqnext bigint not null,
	wqerror varchar(255) not null,
	wqbody text not null
);

CREATE INDEX idx_hm_webhookqueue_due ON hm_webhookqueue (wqstate, wqnext);

CREATE INDEX idx_hm_webhookqueue_webhook ON hm_webhookqueue (wqwebhookid, wqid);

ALTER TABLE hm_webhookqueue ADD CONSTRAINT fk_hm_webhookqueue_webhook FOREIGN KEY (wqwebhookid) REFERENCES hm_webhooks (whid) ON DELETE CASCADE;

insert into hm_settings (settingname, settingstring, settinginteger) values ('WebhooksEnabled', '', 1);

insert into hm_settings (settingname, settingstring, settinginteger) values ('WebhookMaxAttempts', '', 5);

insert into hm_settings (settingname, settingstring, settinginteger) values ('WebhookMaxPerMinute', '', 60);

insert into hm_settings (settingname, settingstring, settinginteger) values ('WebhookRetentionDays', '', 7);

insert into hm_settings (settingname, settingstring, settinginteger) values ('MigrationConnectionsPerHost', '', 4);

ALTER TABLE hm_signins ADD CONSTRAINT fk_hm_signins_account FOREIGN KEY (siaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

create table hm_pushsubscriptions
(
	psid bigserial not null primary key,
	psaccountid int not null,
	psendpointhash varchar(64) not null,
	psendpoint varchar(1000) not null,
	pskeyp256dh varchar(128) not null,
	pskeyauth varchar(32) not null,
	psdevice varchar(64) not null,
	pscreated bigint not null,
	pslastsent bigint not null,
	psstate int not null,
	psfailures int not null,
	psnextattempt bigint not null,
	pslastdetail varchar(255) not null,
	psdetail int not null,
	pssenders varchar(255) not null,
	pslabels varchar(255) not null,
	psquietfrom int not null,
	psquietto int not null,
	psquietoffset int not null
);

CREATE UNIQUE INDEX idx_hm_pushsubscriptions_endpoint ON hm_pushsubscriptions (psaccountid, psendpointhash);

ALTER TABLE hm_pushsubscriptions ADD CONSTRAINT fk_hm_pushsubscriptions_account FOREIGN KEY (psaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

create table hm_complaints
(
	cpid bigserial not null primary key,
	cpdomainid int not null,
	cpaccountid int not null,
	cpkey varchar(64) not null,
	cpaddress varchar(255) not null,
	cpsender varchar(255) not null,
	cpfeedbacktype varchar(32) not null,
	cpprovider varchar(255) not null,
	cpsourceip varchar(45) not null,
	cpreporteddomain varchar(255) not null,
	cpmessageid varchar(255) not null,
	cpsubject varchar(255) not null,
	cparrival varchar(32) not null,
	cpreceived varchar(32) not null
);

CREATE UNIQUE INDEX idx_hm_complaints_key ON hm_complaints (cpkey);

CREATE INDEX idx_hm_complaints_domain ON hm_complaints (cpdomainid);

CREATE INDEX idx_hm_complaints_address ON hm_complaints (cpaddress);

CREATE INDEX idx_hm_complaints_received ON hm_complaints (cpreceived);

create table hm_complaintvolume
(
	cvid bigserial not null primary key,
	cvdomainid int not null,
	cvday varchar(10) not null,
	cvsent bigint not null,
	cvcomplaints int not null
);

CREATE UNIQUE INDEX idx_hm_complaintvolume_day ON hm_complaintvolume (cvdomainid, cvday);

ALTER TABLE hm_complaints ADD CONSTRAINT fk_hm_complaints_domain FOREIGN KEY (cpdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE;

ALTER TABLE hm_complaintvolume ADD CONSTRAINT fk_hm_complaintvolume_domain FOREIGN KEY (cvdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE;

insert into hm_alertrules (alertrulecondition, alertruleenabled, alertrulethreshold, alertruleactions, alertruleaddress, alertrulewebhook, alertrulesecret, alertrulecooldown, alertruledigest) values ('complaints.rate.high', 1, 1000, 1, '', '', '', 1440, 1);

create table hm_mailboxmoves
(
	mmid bigserial not null primary key,
	mmaccountid int not null,
	mmoldaddress varchar(255) not null,
	mmnewaddress varchar(255) not null,
	mmstage int not null,
	mmkeepalias int not null,
	mmmessages int not null,
	mmstarted varchar(32) not null,
	mmfinished varchar(32) not null,
	mmdetail varchar(255) not null
);

CREATE INDEX idx_hm_mailboxmoves_account ON hm_mailboxmoves (mmaccountid);

ALTER TABLE hm_mailboxmoves ADD CONSTRAINT fk_hm_mailboxmoves_account FOREIGN KEY (mmaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

create table hm_domainpolicylist
(
	dplid serial not null primary key,
	dplpattern varchar(255) not null,
	dplmatchon int not null,
	dplaction int not null,
	dplnote varchar(255) not null,
	dplcreated timestamp not null
);

CREATE UNIQUE INDEX idx_hm_domainpolicylist_pattern ON hm_domainpolicylist (dplpattern, dplmatchon);

create table hm_journalrules
(
	jrid bigserial not null primary key,
	jrscope int not null,
	jrdomainid int not null,
	jraccountid int not null,
	jraddress varchar(255) not null,
	jrdirection int not null,
	jrmode int not null,
	jrenabled smallint not null,
	jrnote varchar(255) not null
);

CREATE INDEX idx_hm_journalrules_domain ON hm_journalrules (jrdomainid);

CREATE INDEX idx_hm_journalrules_account ON hm_journalrules (jraccountid);

ALTER TABLE hm_journalrules ADD CONSTRAINT fk_hm_journalrules_domain FOREIGN KEY (jrdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE;

create table hm_accountrecovery
(
	acrid serial not null primary key,
	acraccountid int not null,
	acraddress varchar(255) not null,
	acrverified int not null,
	acrcreated bigint not null,
	acrupdated bigint not null
);

CREATE UNIQUE INDEX idx_hm_accountrecovery_account ON hm_accountrecovery (acraccountid);

create table hm_recoverytokens
(
	rtid bigserial not null primary key,
	rtaccountid int not null,
	rtkind int not null,
	rttoken varchar(64) not null,
	rtaddress varchar(255) not null,
	rtexpires bigint not null,
	rtused bigint not null,
	rtcreated bigint not null
);

CREATE INDEX idx_hm_recoverytokens_token ON hm_recoverytokens (rttoken);

CREATE INDEX idx_hm_recoverytokens_account ON hm_recoverytokens (rtaccountid);

create table hm_recoverycodes
(
	rkid bigserial not null primary key,
	rkaccountid int not null,
	rkhash varchar(255) not null,
	rkencryption int not null,
	rkused bigint not null,
	rkcreated bigint not null
);

CREATE INDEX idx_hm_recoverycodes_account ON hm_recoverycodes (rkaccountid);

ALTER TABLE hm_accountrecovery ADD CONSTRAINT fk_hm_accountrecovery_account FOREIGN KEY (acraccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

ALTER TABLE hm_recoverytokens ADD CONSTRAINT fk_hm_recoverytokens_account FOREIGN KEY (rtaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

ALTER TABLE hm_recoverycodes ADD CONSTRAINT fk_hm_recoverycodes_account FOREIGN KEY (rkaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

insert into hm_alertrules (alertrulecondition, alertruleenabled, alertrulethreshold, alertruleactions, alertruleaddress, alertrulewebhook, alertrulesecret, alertrulecooldown, alertruledigest) values ('account.password-reset', 1, 0, 1, '', '', '', 0, 0);

create table hm_settingoverrides
(
	soid bigserial not null primary key,
	soscope int not null,
	soownerid bigint not null,
	soname varchar(30) not null,
	sovalue varchar(4000) not null
);

CREATE UNIQUE INDEX idx_hm_settingoverrides_owner ON hm_settingoverrides (soscope, soownerid, soname);

CREATE INDEX idx_hm_settingoverrides_name ON hm_settingoverrides (soname);

create table hm_apisends
(
	apsid bigserial not null primary key,
	apskeyid varchar(32) not null,
	apsqueueid bigint not null,
	apsmessageid varchar(255) not null,
	apssender varchar(255) not null,
	apsrecipients varchar(4000) not null,
	apscreated timestamp not null
);

CREATE INDEX idx_hm_apisends_key ON hm_apisends (apskeyid, apsid);

CREATE INDEX idx_hm_apisends_created ON hm_apisends (apscreated);

CREATE INDEX idx_hm_apisends_keycreated ON hm_apisends (apskeyid, apscreated);

CREATE INDEX idx_hm_apisends_queueid ON hm_apisends (apsqueueid);

CREATE INDEX idx_hm_messagetrace_queueid ON hm_messagetrace (mtqueueid);

create table hm_migrations
(
	migrationid bigserial not null primary key,
	migrationname varchar(255) not null,
	migrationkind int not null,
	migrationhost varchar(255) not null,
	migrationport int not null,
	migrationsecurity int not null,
	migrationoauthprovider varchar(32) not null,
	migrationdavurl varchar(255) not null,
	migrationconcurrency int not null,
	migrationmessagesperminute int not null,
	migrationstate int not null,
	migrationcreated bigint not null,
	migrationstartedat bigint not null,
	migrationfinishedat bigint not null
);

create table hm_migrationjobs
(
	mjid bigserial not null primary key,
	mjmigrationid bigint not null,
	mjsourceuser varchar(255) not null,
	mjsourcesecret text not null,
	mjsigninhash varchar(64) not null,
	mjtargetaccountid bigint not null,
	mjstate int not null,
	mjphase int not null,
	mjfolderstotal int not null,
	mjfoldersdone int not null,
	mjmessagestotal int not null,
	mjmessagesdone int not null,
	mjbytesdone bigint not null,
	mjskipped int not null,
	mjlasterror varchar(1000) not null,
	mjattempts int not null,
	mjnextattempt bigint not null,
	mjstartedat bigint not null,
	mjfinishedat bigint not null,
	mjreport text not null
);

CREATE INDEX idx_hm_migrationjobs_migration ON hm_migrationjobs (mjmigrationid, mjid);

CREATE INDEX idx_hm_migrationjobs_state ON hm_migrationjobs (mjstate);

ALTER TABLE hm_migrationjobs ADD CONSTRAINT fk_hm_migrationjobs_migration FOREIGN KEY (mjmigrationid) REFERENCES hm_migrations (migrationid) ON DELETE CASCADE;

create table hm_migrationfolders
(
	mfid bigserial not null primary key,
	mfjobid bigint not null,
	mffolder varchar(1024) not null,
	mftargetfolder varchar(1024) not null,
	mfuidvalidity varchar(64) not null,
	mflastuid bigint not null,
	mfmessages int not null,
	mfdone int not null
);

CREATE INDEX idx_hm_migrationfolders_job ON hm_migrationfolders (mfjobid, mfid);

ALTER TABLE hm_migrationfolders ADD CONSTRAINT fk_hm_migrationfolders_job FOREIGN KEY (mfjobid) REFERENCES hm_migrationjobs (mjid) ON DELETE CASCADE;

create table hm_sandboxverdicts
(
	svid bigserial not null primary key,
	svsha256 varchar(64) not null,
	svverdict varchar(16) not null,
	svscore int not null,
	svdetail varchar(255) not null,
	svanswered bigint not null,
	svexpires bigint not null
);

CREATE UNIQUE INDEX idx_hm_sandboxverdicts_sha256 ON hm_sandboxverdicts (svsha256);

create table hm_sandboxholds
(
	shid bigserial not null primary key,
	shmessageid bigint not null,
	shsha256 varchar(64) not null,
	shfilename varchar(255) not null,
	shsize bigint not null,
	shsubmission varchar(1000) not null,
	shheld bigint not null,
	shstate int not null,
	shverdict varchar(16) not null,
	shscore int not null,
	shdetail varchar(255) not null,
	sherrors int not null,
	shdecision int not null
);

CREATE INDEX idx_hm_sandboxholds_message ON hm_sandboxholds (shmessageid);

insert into hm_settings (settingname, settingstring, settinginteger) values ('SandboxEnabled', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('SandboxSubmitUrl', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('SandboxVerdictUrl', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('SandboxToken', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('SandboxTimeoutMinutes', '', 15);

insert into hm_settings (settingname, settingstring, settinginteger) values ('SandboxTimeoutAction', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('SandboxMaxSizeKB', '', 20480);

insert into hm_settings (settingname, settingstring, settinginteger) values ('SandboxTypes', 'exe,dll,scr,js,jse,vbs,wsf,hta,msi,bat,cmd,ps1,jar,lnk,iso,img,docm,xlsm,pptm,doc,xls,ppt,rtf,pdf,zip,7z,rar', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('SandboxPollSeconds', '', 30);

insert into hm_settings (settingname, settingstring, settinginteger) values ('SandboxScoreThreshold', '', 70);

insert into hm_settings (settingname, settingstring, settinginteger) values ('SandboxCacheHours', '', 24);

insert into hm_settings (settingname, settingstring, settinginteger) values ('SandboxInternalMail', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('SandboxMaxFiles', '', 10);

create table hm_listarchive
(
	laid bigserial not null primary key,
	lalistid int not null,
	lamessageid varchar(255) not null,
	lasubject varchar(255) not null,
	lasender varchar(255) not null,
	lasent bigint not null,
	lasize int not null,
	lafile varchar(255) not null,
	lamonth varchar(7) not null,
	laarchived int not null
);

CREATE INDEX idx_hm_listarchive_list ON hm_listarchive (lalistid, lasent);

CREATE INDEX idx_hm_listarchive_month ON hm_listarchive (lalistid, lamonth);

ALTER TABLE hm_listarchive ADD CONSTRAINT fk_hm_listarchive_list FOREIGN KEY (lalistid) REFERENCES hm_distributionlists (distributionlistid) ON DELETE CASCADE;

insert into hm_servermessages (smname, smtext) values ('LIST_CONFIRM_SUBJECT', 'Confirm your subscription to %MACRO_LIST%');

insert into hm_servermessages (smname, smtext) values ('LIST_CONFIRM', E'Somebody - probably you - asked for %MACRO_ADDRESS% to be subscribed to the list %MACRO_LIST%.\r\n\r\nTo confirm, reply to this message without changing its subject, or open this page and press the button on it:\r\n\r\n   %MACRO_LINK%\r\n\r\nIf you did not ask for this, ignore this message. Nothing happens unless you confirm, and the request lapses in a week.\r\n\r\nhMailServer\r\n');

insert into hm_servermessages (smname, smtext) values ('LIST_REFUSED_SUBJECT', 'Your request to %MACRO_LIST%');

insert into hm_servermessages (smname, smtext) values ('LIST_REFUSED', E'The list %MACRO_LIST% does not take subscriptions by mail or by page: its members are added by the person who runs it. Write to %MACRO_OWNER% to ask.\r\n\r\nhMailServer\r\n');

insert into hm_servermessages (smname, smtext) values ('LIST_HELP_SUBJECT', 'The addresses of the list %MACRO_LIST%');

insert into hm_servermessages (smname, smtext) values ('LIST_HELP', E'The list %MACRO_LIST% answers these addresses:\r\n\r\n   %MACRO_LIST%\r\n      post to the list\r\n   %MACRO_SUBSCRIBE%\r\n      join the list (you are sent a message to confirm)\r\n   %MACRO_UNSUBSCRIBE%\r\n      leave the list (write from the address you are subscribed with; you are sent a message to confirm)\r\n   %MACRO_REQUEST%\r\n      this help\r\n   %MACRO_OWNERADDRESS%\r\n      the person who runs the list\r\n\r\nEvery message from the list also carries a link at which you can leave it, or choose a daily digest, or pause it.\r\n\r\nhMailServer\r\n');

insert into hm_servermessages (smname, smtext) values ('LIST_APPROVE_SUBJECT', '%MACRO_ADDRESS% asks to join %MACRO_LIST%');

insert into hm_servermessages (smname, smtext) values ('LIST_APPROVE', E'%MACRO_ADDRESS% has asked to join the list %MACRO_LIST% and has confirmed that the address is theirs.\r\n\r\nTo let them in, open this page while you are signed in to this server''s webmail as %MACRO_OWNER%, and press the button on it:\r\n\r\n   %MACRO_LINK%\r\n\r\nOr approve them in the Control Deck, in the list''s member table.\r\n\r\nTo refuse, do nothing: the request lapses in a week.\r\n\r\nhMailServer\r\n');

insert into hm_servermessages (smname, smtext) values ('LIST_BOUNCED_SUBJECT', 'The list %MACRO_LIST% has stopped mailing %MACRO_ADDRESS%');

insert into hm_servermessages (smname, smtext) values ('LIST_BOUNCED', E'Mail from the list %MACRO_LIST% to %MACRO_ADDRESS% kept coming back, so that address is no longer sent the list''s mail. It is still a member.\r\n\r\n   %MACRO_REASON%\r\n\r\nWhen the address works again, reset its bounces in the Control Deck (the list''s member table): it is then sent the list''s mail again. The member can also turn it back on from the link in any message of the list.\r\n\r\nhMailServer\r\n');

insert into hm_servermessages (smname, smtext) values ('LIST_LEAVE_SUBJECT', 'Confirm that you want to leave %MACRO_LIST%');

insert into hm_servermessages (smname, smtext) values ('LIST_LEAVE', E'Somebody - probably you - asked for %MACRO_ADDRESS% to be taken off the list %MACRO_LIST%.\r\n\r\nTo leave, reply to this message without changing its subject, or open this page and press the button on it:\r\n\r\n   %MACRO_LINK%\r\n\r\nIf you did not ask for this, ignore this message: you stay on the list, and nothing changes.\r\n\r\nhMailServer\r\n');

insert into hm_servermessages (smname, smtext) values ('LIST_UNSUBSCRIBED_SUBJECT', 'You have left %MACRO_LIST%');

insert into hm_servermessages (smname, smtext) values ('LIST_UNSUBSCRIBED', E'%MACRO_ADDRESS% has been taken off the list %MACRO_LIST%. It will not be sent the list''s mail any more.\r\n\r\nhMailServer\r\n');

create table hm_warmup
(
	warmupid serial not null primary key,
	warmupaddress varchar(64) not null,
	warmupstarted bigint not null,
	warmupplan int not null,
	warmupenabled int not null
);

CREATE UNIQUE INDEX idx_hm_warmup_address ON hm_warmup (warmupaddress);

create table hm_warmupcounts
(
	wcid serial not null primary key,
	wcwarmupid int not null,
	wcday bigint not null,
	wcprovider varchar(32) not null,
	wcsent int not null,
	wcdeferred int not null,
	wcceiling int not null,
	wcplanday int not null
);

CREATE UNIQUE INDEX idx_hm_warmupcounts_day ON hm_warmupcounts (wcwarmupid, wcday, wcprovider);

CREATE INDEX idx_hm_warmupcounts_date ON hm_warmupcounts (wcday);

ALTER TABLE hm_warmupcounts ADD CONSTRAINT fk_hm_warmupcounts_warmup FOREIGN KEY (wcwarmupid) REFERENCES hm_warmup (warmupid) ON DELETE CASCADE;

create table hm_smimeca
(
	caid serial not null primary key,
	cadomainid int not null,
	cacertificate text not null,
	cafingerprint varchar(64) not null,
	caprivatekey text not null,
	cacreated bigint not null,
	caretired bigint not null,
	caserial bigint not null
);

CREATE INDEX idx_hm_smimeca_domain ON hm_smimeca (cadomainid);

CREATE INDEX idx_hm_smimeca_fingerprint ON hm_smimeca (cafingerprint);

ALTER TABLE hm_smimeca ADD CONSTRAINT fk_hm_smimeca_domain FOREIGN KEY (cadomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE;

create table hm_smimeissued
(
	issuedid serial not null primary key,
	issuedcaid int not null,
	issuedaccountid int not null,
	issuedaddress varchar(255) not null,
	issuedserial bigint not null,
	issuedfingerprint varchar(64) not null,
	issuednotbefore bigint not null,
	issuednotafter bigint not null,
	issuedrevoked bigint not null,
	issuedreason varchar(255) not null,
	issuedcertificate text not null,
	issuedpending smallint not null
);

CREATE UNIQUE INDEX idx_hm_smimeissued_serial ON hm_smimeissued (issuedcaid, issuedserial);

CREATE INDEX idx_hm_smimeissued_address ON hm_smimeissued (issuedaddress);

CREATE INDEX idx_hm_smimeissued_account ON hm_smimeissued (issuedaccountid);

CREATE INDEX idx_hm_smimeissued_fingerprint ON hm_smimeissued (issuedfingerprint);

ALTER TABLE hm_smimeissued ADD CONSTRAINT fk_hm_smimeissued_ca FOREIGN KEY (issuedcaid) REFERENCES hm_smimeca (caid) ON DELETE CASCADE;

create table hm_discoverycases
(
	caseid serial not null primary key,
	casename varchar(255) not null,
	casedescription varchar(1000) not null,
	casestate int not null,
	casecreatedby varchar(255) not null,
	casecreated bigint not null,
	caseclosedby varchar(255) not null,
	caseclosed bigint not null
);

create table hm_discoverysearches
(
	searchid serial not null primary key,
	searchcaseid int not null,
	searchquery text not null,
	searchranby varchar(255) not null,
	searchran bigint not null,
	searchfinished bigint not null,
	searchstate int not null,
	searchscanned int not null,
	searchmatched int not null,
	searchunsearched int not null,
	searchtruncated int not null,
	searcherror varchar(1000) not null,
	searchresultfile varchar(255) not null
);

CREATE INDEX idx_hm_discoverysearches_case ON hm_discoverysearches (searchcaseid);

ALTER TABLE hm_discoverysearches ADD CONSTRAINT fk_hm_discoverysearches_case FOREIGN KEY (searchcaseid) REFERENCES hm_discoverycases (caseid) ON DELETE CASCADE;

create table hm_discoveryholds
(
	holdid serial not null primary key,
	holdcaseid int not null,
	holdscope int not null,
	holdownerid bigint not null,
	holdname varchar(255) not null,
	holdplacedby varchar(255) not null,
	holdplaced bigint not null,
	holdreleasedby varchar(255) not null,
	holdreleased bigint not null
);

CREATE INDEX idx_hm_discoveryholds_case ON hm_discoveryholds (holdcaseid);

CREATE INDEX idx_hm_discoveryholds_owner ON hm_discoveryholds (holdscope, holdownerid);

ALTER TABLE hm_discoveryholds ADD CONSTRAINT fk_hm_discoveryholds_case FOREIGN KEY (holdcaseid) REFERENCES hm_discoverycases (caseid) ON DELETE CASCADE;

create table hm_discoveryexports
(
	exportid serial not null primary key,
	exportcaseid int not null,
	exportsearchid int not null,
	exportstate int not null,
	exportfile varchar(255) not null,
	exportsize bigint not null,
	exportsha256 varchar(64) not null,
	exportitems int not null,
	exportwithheld int not null,
	exportrequestedby varchar(255) not null,
	exportrequested bigint not null,
	exportfinished bigint not null,
	exporterror varchar(1000) not null
);

CREATE INDEX idx_hm_discoveryexports_case ON hm_discoveryexports (exportcaseid);

ALTER TABLE hm_discoveryexports ADD CONSTRAINT fk_hm_discoveryexports_search FOREIGN KEY (exportsearchid) REFERENCES hm_discoverysearches (searchid) ON DELETE CASCADE;

insert into hm_dbversion values (6099);
