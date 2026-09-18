drop table if exists hm_mailboxmoves;

drop table if exists hm_imapfolders;

drop table if exists hm_settings;

drop table if exists hm_inisettings;

drop table if exists hm_roleassignments;

drop table if exists hm_roles;

drop table if exists hm_audit;

drop table if exists hm_alertrules;

drop table if exists hm_alertevents;

drop table if exists hm_remotedomainpolicies;

drop table if exists hm_dkimrotations;

drop table if exists hm_reputationchecks;

drop table if exists hm_spamtraps;

drop table if exists hm_journalrules;

drop table if exists hm_spamtraphits;

drop table if exists hm_domainpolicylist;

drop table if exists hm_dmarcreportrecords;

drop table if exists hm_dmarcreports;

drop table if exists hm_tlsreports;

drop table if exists hm_accountactivity;

drop table if exists hm_accountrecipients;

drop table if exists hm_accountreview;

drop table if exists hm_heldmessages;

drop table if exists hm_confidential;

drop table if exists hm_passkeys;

drop table if exists hm_directory;

drop table if exists hm_recoverable;

drop table if exists hm_maskedaddresses;

drop table if exists hm_senderaddresses;

drop table if exists hm_calendarshares;

drop table if exists hm_calendarobjects;

drop table if exists hm_calendars;

drop table if exists hm_addressbookshares;

drop table if exists hm_addressbooks;

drop table if exists hm_contacts;

drop table if exists hm_pushsubscriptions;

drop table if exists hm_accountprefs;

drop table if exists hm_scheduled;

drop table if exists hm_files;

drop table if exists hm_smimekeys;

drop table if exists hm_pgpring;

drop table if exists hm_protectedmessages;

drop table if exists hm_accounts;

drop table if exists hm_aliases;

drop table if exists hm_domains;

drop table if exists hm_domain_aliases;

drop table if exists hm_messages;

drop table if exists hm_message_metadata;

drop table if exists hm_dbversion;

drop table if exists hm_distributionlists;

drop table if exists hm_distributionlistsrecipients;

drop table if exists hm_messagerecipients;

drop table if exists hm_routes;

drop table if exists hm_routeaddresses;

drop table if exists hm_securityranges;

drop table if exists hm_dnsbl;

drop table if exists hm_fetchaccounts;

drop table if exists hm_fetchaccounts_uids;

drop table if exists hm_rules;

drop table if exists hm_rule_criterias;

drop table if exists hm_rule_actions;

drop table if exists hm_surblservers;

drop table if exists hm_greylisting_triplets;

drop table if exists hm_blocked_attachments;

drop table if exists hm_servermessages;

drop table if exists hm_greylisting_whiteaddresses;

drop table if exists hm_greylisting_bypass;

drop table if exists hm_tcpipports;

drop table if exists hm_whitelist;

drop table if exists hm_sslcertificates;

drop table if exists hm_groups;

drop table if exists hm_group_members;

drop table if exists hm_acl;

drop table if exists hm_incoming_relays;

drop table if exists hm_logon_failures;

create table hm_accounts 
(
	accountid int auto_increment not null, primary key(`accountid`), unique(`accountid`),
	accountdomainid int not null ,
	accountadminlevel tinyint not null ,
	accountaddress varchar (255) not null, unique(`accountaddress`),
	accountpassword varchar (255) not null ,
	accountactive tinyint not null,
	accountisad tinyint not null, 
	accountaddomain varchar (255) not null ,
	accountadusername varchar (255) not null,
	accountmaxsize int not null,
	accountmessageretentiondays int not null default 0,
	accountvacationmessageon tinyint not null,
	accountvacationmessage text not null,
	accountvacationsubject varchar (200) not null,
	accountpwencryption tinyint not null,
	accountforwardenabled tinyint not null,
	accountforwardaddress varchar (255) not null,
	accountforwardkeeporiginal tinyint not null,
	accountenablesignature tinyint not null,
	accountsignatureplaintext text not null,
	accountsignaturehtml text not null,
	accountlastlogontime datetime not null,
	accountvacationexpires tinyint unsigned not null,
	accountvacationexpiredate datetime not null,
	accountvacationbegindate datetime not null,
	accountpersonfirstname varchar(60) not null,
	accountpersonlastname varchar(60) not null,
	accountvacationabortspamflagged tinyint not null,
	accountvacationdays int not null default 0,
	accountvacationinternalsubject varchar(200) not null default '',
	accountvacationinternalmessage varchar(1000) not null default '',
	accountvacationinternalonly tinyint not null default 0,
	accountforwardabortspamflagged tinyint not null,
	accounttotpsecret varchar(255) not null default '',
	accountpasswordchanged datetime not null default '2000-01-01',
	accountantispamenabled tinyint not null default 1,
	accountspammarkthreshold int not null default -1,
	accountspamdeletethreshold int not null default -1,
	accountopenpgppublish int not null default 0
) DEFAULT CHARSET=utf8;

CREATE INDEX idx_hm_accounts ON hm_accounts (accountaddress);

create table hm_aliases 
(
	aliasid int auto_increment not null, primary key(`aliasid`), unique(`aliasid`),
	aliasdomainid int not null ,
	aliasname varchar (255) not null, unique(`aliasname`),
	aliasvalue varchar (255) not null ,
	aliasactive tinyint not null
) DEFAULT CHARSET=utf8;

CREATE INDEX idx_hm_aliases ON hm_aliases (aliasdomainid, aliasname);

create table hm_domains 
(
	domainid int auto_increment not null, primary key(`domainid`), unique(`domainid`),
	domainname varchar (80) not null, unique(`domainname`),
	domainactive tinyint not null,
	domainpostmaster varchar (80) not null,
	domainmaxsize integer not null,
	domainmessageretentiondays int not null default 0,
	domainaddomain varchar(255) not null,
	domainmaxmessagesize integer not null,
	domainuseplusaddressing  tinyint not null,
	domainplusaddressingchar varchar(1) not null,
	domainantispamoptions integer not null,
	domainenablesignature tinyint not null,
	domainsignaturemethod tinyint not null,
	domainsignatureplaintext text not null,
	domainsignaturehtml text not null,
	domainaddsignaturestoreplies tinyint not null,
	domainaddsignaturestolocalemail tinyint not null,
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
	domainvacationmessage text,
	domainvacationinternalsubject varchar(200) not null default '',
	domainvacationinternalmessage text,
	domainvacationexternaloverride int not null default 0,
	domainvacationdays int not null default 0,
	domainquotawarningpercent int not null default -1,
	domainquotawarningsubject varchar(200) not null default '',
	domainquotawarningtext text not null,
	domainquotagracepercent int not null default -1,
	domainquotasendstoppercent int not null default -1,
	domainexternaltagsubject int not null default 0,
	domainexternaltagheader int not null default 0,
	domainexternaltagtext varchar(100) not null default '',
	domainfirstcontacttip int not null default 0,
	domaindisclaimerenabled int not null default 0,
	domaindisclaimerplaintext text,
	domaindisclaimerhtml text,
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
	domaincountrypolicy varchar(255) not null default '',
	domainsessionidleseconds int not null default 0,
	domainsessionlifetimeseconds int not null default 0,
	domainsessiononeperdevice int not null default 0,
	domainsessionbindaddress int not null default 0,
	domainreportmailbox varchar(255) not null default ''
)  DEFAULT CHARSET=utf8;

CREATE INDEX idx_hm_domains ON hm_domains (domainname);

create table hm_domain_aliases
(
	daid int auto_increment not null, primary key(`daid`), unique(`daid`),
	dadomainid int not null ,
	daalias varchar(255) not null
)  DEFAULT CHARSET=utf8;

create table hm_messages 
(
	messageid bigint auto_increment not null, primary key(`messageid`), unique(`messageid`),
	messageaccountid int not null ,
	messagefolderid integer not null DEFAULT 0,
	messagefilename varchar (255) not null ,
	messagetype tinyint not null ,
	messagefrom varchar (255) not null ,
	messagesize bigint not null,
	messagecurnooftries int not null,
	messagenexttrytime datetime not null,
	messageflags smallint unsigned not null,
	messagecreatetime datetime not null,
	messagesavedate datetime not null,
	messageemailid varchar(48) not null,
	messagelocked tinyint not null,
   messageuid bigint not null,
   messagemodseq bigint not null default 1,
   messagekeywords varchar(500) not null default ''
) DEFAULT CHARSET=utf8;

CREATE INDEX idx_hm_messages ON hm_messages (messageaccountid, messagefolderid);

CREATE INDEX idx_hm_messages_type ON hm_messages (messagetype);

create table hm_message_metadata 
(
   metadata_id bigint auto_increment not null, primary key(`metadata_id`),
   metadata_accountid int not null,
   metadata_folderid int not null,
   metadata_messageid bigint  not null,
   metadata_dateutc datetime null,
   metadata_from varchar(255) not null,
   metadata_subject varchar(255) not null,
   metadata_to varchar(255) not null,
   metadata_cc varchar(255) not null
) DEFAULT CHARSET=utf8;

CREATE UNIQUE INDEX idx_message_metadata_unique ON hm_message_metadata (`metadata_accountid`,  `metadata_folderid`, `metadata_messageid`);

CREATE INDEX idx_message_metadata_id ON hm_message_metadata (`metadata_messageid`);

create table hm_settings 
(
	settingid int auto_increment not null, primary key(`settingid`), unique(`settingid`),
	settingname varchar (30) not null, unique(`settingname`),
	settingstring varchar (4000) not null ,
	settinginteger int not null
) DEFAULT CHARSET=utf8;

create table hm_inisettings
(
	inisettingid int auto_increment not null, primary key(`inisettingid`), unique(`inisettingid`),
	inisettingname varchar (100) not null, unique(`inisettingname`),
	inisettingvalue text not null,
	inisettingfilevalue text not null
) DEFAULT CHARSET=utf8;

create table hm_dbversion 
(
	value int not null
) DEFAULT CHARSET=utf8;

create table hm_distributionlists 
(
	distributionlistid int auto_increment not null, primary key(distributionlistid), unique(distributionlistid),
	distributionlistdomainid int not null,
	distributionlistaddress varchar(255) not null,unique(`distributionlistaddress`),
	distributionlistenabled tinyint not null,
   distributionlistrequireauth tinyint not null,
	distributionlistrequireaddress varchar(255) not null,
	distributionlistmode tinyint not null,
	distributionlistmoderatoraddress varchar(255) not null default '',
	distributionlistbounceaddress varchar(255) not null default '',
	distributionlistmembership varchar(200) not null default ''
) DEFAULT CHARSET=utf8;

CREATE INDEX idx_hm_distributionlists ON hm_distributionlists (distributionlistdomainid);

create table hm_distributionlistsrecipients
(
	distributionlistrecipientid int auto_increment not null, primary key(distributionlistrecipientid), unique(distributionlistrecipientid),
	distributionlistrecipientlistid int not null,
	distributionlistrecipientaddress varchar(255)	
);

CREATE INDEX idx_hm_distributionlistsrecipients ON hm_distributionlistsrecipients (distributionlistrecipientlistid);

create table hm_messagerecipients
(
	recipientid bigint auto_increment not null, primary key(recipientid), unique(recipientid),
 	recipientmessageid bigint not null,
	recipientaddress varchar(255) not null,
	recipientlocalaccountid int not null,
	recipientoriginaladdress varchar(255) not null,
	recipientdsnnotify int not null default 0
) DEFAULT CHARSET=utf8;

CREATE INDEX idx_hm_messagerecipients ON hm_messagerecipients (recipientmessageid);

create table hm_imapfolders 
(
  folderid int NOT NULL auto_increment, primary key(folderid), unique(folderid),
  folderaccountid int unsigned NOT NULL,
  folderparentid int NOT NULL,
  foldername varchar(255) NOT NULL,
  folderissubscribed tinyint unsigned NOT NULL,
  foldercreationtime datetime NOT NULL,
  foldercurrentuid bigint NOT NULL,
  foldercurrentmodseq bigint NOT NULL default 1,
  folderspecialuse int NOT NULL default 0
) DEFAULT CHARSET=utf8;

CREATE INDEX idx_hm_imapfolders ON hm_imapfolders (folderaccountid);

CREATE UNIQUE INDEX idx_hm_imapfolders_unique ON hm_imapfolders (`folderaccountid`, `folderparentid`,  `foldername`);

create table hm_imapexpunged
(
  expungedaccountid int not null,
  expungedfolderid int not null,
  expungeduid bigint not null,
  expungedmodseq bigint not null
) DEFAULT CHARSET=utf8;

CREATE INDEX idx_hm_imapexpunged ON hm_imapexpunged (expungedfolderid, expungedmodseq);

create table hm_routes
(
  routeid int NOT NULL auto_increment, primary key(routeid), unique(routeid),
  routedomainname varchar(255) NOT NULL,
  routedescription varchar(255) NOT NULL,
  routetargetsmthost varchar(255) NOT NULL,
  routetargetsmtport int NOT NULL,
  routenooftries int NOT NULL,
  routeminutesbetweentry int NOT NULL,
  routealladdresses tinyint unsigned NOT NULL,
  routeuseauthentication tinyint NOT NULL,
  routeauthenticationusername varchar(255) NOT NULL,
  routeauthenticationpassword varchar(1024) NOT NULL,
  routetreatsecurityaslocal tinyint NOT NULL,
  routeconnectionsecurity tinyint not null,
  routetreatsenderaslocaldomain tinyint NOT NULL
) DEFAULT CHARSET=utf8;

create table hm_routeaddresses
(
  routeaddressid mediumint(9) NOT NULL auto_increment, primary key(routeaddressid), unique(routeaddressid),
  routeaddressrouteid int NOT NULL,
  routeaddressaddress varchar(255) NOT NULL
) DEFAULT CHARSET=utf8;

create table hm_securityranges
(
	rangeid int auto_increment not null, primary key(rangeid), unique(rangeid),
   rangepriorityid int not null,
	rangelowerip1 bigint not null,
   rangelowerip2 bigint null,
	rangeupperip1 bigint not null,
   rangeupperip2 bigint null,
	rangeoptions int not null,
	rangename varchar (100) not null, unique(`rangename`),
   rangeexpires tinyint not null,
	rangeexpirestime datetime not null
) DEFAULT CHARSET=utf8;

create table hm_dnsbl
(
	sblid int auto_increment not null, primary key(sblid), unique(sblid),
	sblactive tinyint not null,
	sbldnshost varchar(255) not null,
	sblresult varchar(255) not null,
	sblrejectmessage varchar(255) not null,
	sblscore int not null
) DEFAULT CHARSET=utf8;

create table hm_fetchaccounts
(	
	faid int auto_increment not null, primary key(`faid`), unique(`faid`),
	faactive tinyint not null,
	faaccountid int not null,
	faaccountname varchar (255) not null,
	faserveraddress varchar (255) not null,
	faserverport int not null,
	faservertype tinyint not null,
	fausername varchar (255) not null,
	fapassword varchar (1024) not null,
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
	famimerecipientheaders varchar(255) not null DEFAULT 'To,CC,X-RCPT-TO,X-Envelope-To',
	faoauthprovider varchar(16) not null DEFAULT '',
	faoauthtoken text not null,
	famirrorskip text not null,
	famirrorseen text not null
) DEFAULT CHARSET=utf8;

create table hm_fetchaccounts_uids
(
	uidid int auto_increment not null, primary key(`uidid`), unique(`uidid`),
	uidfaid int not null,
	uidvalue varchar(255) not null,
	uidtime datetime not null
) DEFAULT CHARSET=utf8;

CREATE INDEX idx_hm_fetchaccounts_uids ON hm_fetchaccounts_uids (uidfaid);

create table hm_apppasswords
(
	apid int auto_increment not null, primary key(`apid`), unique(`apid`),
	apaccountid int not null,
	apname varchar(255) not null,
	aphash varchar(255) not null,
	apencryption int not null,
	apcreated datetime not null,
	aplastused datetime null,
	apactive tinyint not null
) DEFAULT CHARSET=utf8;

CREATE INDEX idx_hm_apppasswords ON hm_apppasswords (apaccountid);

create table hm_contacts
(
	contactid int auto_increment not null, primary key(`contactid`), unique(`contactid`),
	contactaccountid int not null,
	contactname varchar(255) not null,
	contactaddress varchar(255) not null,
	contactsource tinyint not null,
	contactcreated datetime not null,
	contacturi varchar(255) not null default '',
	contactuid varchar(255) not null default '',
	contactvcard mediumtext not null,
	contactbookid int not null default 0
) DEFAULT CHARSET=utf8;

CREATE UNIQUE INDEX idx_hm_contacts_account ON hm_contacts (contactaccountid, contactaddress);

CREATE INDEX idx_hm_contacts_book ON hm_contacts (contactbookid);

create table hm_addressbooks
(
	bookid int auto_increment not null, primary key(`bookid`), unique(`bookid`),
	bookaccountid int not null,
	bookname varchar(255) not null,
	bookcreated bigint not null
) DEFAULT CHARSET=utf8;

CREATE UNIQUE INDEX idx_hm_addressbooks_account ON hm_addressbooks (bookaccountid, bookname);

create table hm_addressbookshares
(
	shareid int auto_increment not null, primary key(`shareid`), unique(`shareid`),
	sharebookid int not null,
	shareaccountid int not null,
	sharedomainid int not null,
	sharewrite int not null,
	sharecreated bigint not null
) DEFAULT CHARSET=utf8;

CREATE UNIQUE INDEX idx_hm_addressbookshares_target ON hm_addressbookshares (sharebookid, shareaccountid, sharedomainid);

create table hm_accountprefs
(
	prefid int auto_increment not null, primary key(`prefid`), unique(`prefid`),
	prefaccountid int not null,
	prefname varchar(64) not null,
	prefvalue varchar(4000) not null
) DEFAULT CHARSET=utf8;

CREATE UNIQUE INDEX idx_hm_accountprefs_name ON hm_accountprefs (prefaccountid, prefname);

create table hm_scheduled
(
	schedid int auto_increment not null, primary key(`schedid`), unique(`schedid`),
	schedaccountid int not null,
	schedmessageid bigint not null,
	schedaction tinyint not null,
	schedat datetime not null,
	schedfolderid bigint not null,
	schedcreated datetime not null
) DEFAULT CHARSET=utf8;

CREATE INDEX idx_hm_scheduled_account ON hm_scheduled (schedaccountid);

create table hm_files
(
	fileid int auto_increment not null, primary key(`fileid`), unique(`fileid`),
	fileaccountid int not null,
	filetoken varchar(64) not null,
	filename varchar(255) not null,
	filetype varchar(100) not null,
	filesize bigint not null,
	filestored bigint not null,
	filecomplete tinyint not null,
	filecreated bigint not null,
	fileexpires bigint not null,
	filepasswordhash varchar(64) not null,
	filedownloads int not null
) DEFAULT CHARSET=utf8;

CREATE INDEX idx_hm_files_account ON hm_files (fileaccountid);

CREATE UNIQUE INDEX idx_hm_files_token ON hm_files (filetoken);

create table hm_smimekeys
(
	smimeid int auto_increment not null, primary key(`smimeid`), unique(`smimeid`),
	smimeaccountid int not null,
	smimekind tinyint not null,
	smimeaddress varchar(255) not null,
	smimename varchar(255) not null,
	smimefingerprint varchar(64) not null,
	smimecertificate text not null,
	smimechain text not null,
	smimekey text not null,
	smimenotafter bigint not null,
	smimecreated bigint not null
);

CREATE INDEX idx_hm_smimekeys_account ON hm_smimekeys (smimeaccountid);

CREATE UNIQUE INDEX idx_hm_smimekeys_entry ON hm_smimekeys (smimeaccountid, smimekind, smimefingerprint);

create table hm_calendars
(
	calendarid int auto_increment not null, primary key(`calendarid`), unique(`calendarid`),
	calendaraccountid int not null,
	calendarname varchar(255) not null,
	calendardisplayname varchar(255) not null,
	calendarsynctoken bigint not null,
	calendarcreated bigint not null,
	calendarcolor varchar(16) not null default ''
) DEFAULT CHARSET=utf8mb4;

CREATE UNIQUE INDEX idx_hm_calendars_account ON hm_calendars (calendaraccountid, calendarname);

create table hm_calendarobjects
(
	objectid int auto_increment not null, primary key(`objectid`), unique(`objectid`),
	objectaccountid int not null,
	objectcalendarid int not null,
	objecturi varchar(255) not null,
	objectuid varchar(255) not null,
	objectcomponent varchar(16) not null,
	objectdata mediumtext not null,
	objectetag varchar(64) not null,
	objectsynctoken bigint not null,
	objectstart bigint not null,
	objectend bigint not null,
	objectfirst bigint not null,
	objectlast bigint not null,
	objectdeleted tinyint not null,
	objectmodified bigint not null
) DEFAULT CHARSET=utf8mb4;

CREATE UNIQUE INDEX idx_hm_calendarobjects_uri ON hm_calendarobjects (objectcalendarid, objecturi);

CREATE INDEX idx_hm_calendarobjects_sync ON hm_calendarobjects (objectcalendarid, objectsynctoken);

create table hm_calendarshares
(
	shareid int auto_increment not null, primary key(`shareid`), unique(`shareid`),
	sharecalendarid int not null,
	shareaccountid int not null,
	sharewrite int not null
) DEFAULT CHARSET=utf8;

CREATE UNIQUE INDEX idx_hm_calendarshares_pair ON hm_calendarshares (sharecalendarid, shareaccountid);

create table hm_senderaddresses
(
	said int auto_increment not null, primary key(`said`), unique(`said`),
	saaccountid int not null,
	saaddress varchar(255) not null,
	saname varchar(255) not null,
	saverified tinyint not null,
	sacreated datetime not null
) DEFAULT CHARSET=utf8;

CREATE UNIQUE INDEX idx_hm_senderaddresses_address ON hm_senderaddresses (saaccountid, saaddress);

create table hm_maskedaddresses
(
	maid int auto_increment not null, primary key(`maid`), unique(`maid`),
	maaccountid int not null,
	maaddress varchar(255) not null,
	malabel varchar(255) not null,
	maactive tinyint not null,
	mareceived int not null,
	macreated datetime not null
) DEFAULT CHARSET=utf8;

CREATE UNIQUE INDEX idx_hm_maskedaddresses_address ON hm_maskedaddresses (maaddress);

CREATE INDEX idx_hm_maskedaddresses_account ON hm_maskedaddresses (maaccountid);

create table hm_passkeys
(
	pkid int auto_increment not null, primary key(`pkid`), unique(`pkid`),
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
) DEFAULT CHARSET=utf8;

CREATE INDEX idx_hm_passkeys_account ON hm_passkeys (pkaccountid);

CREATE UNIQUE INDEX idx_hm_passkeys_credential ON hm_passkeys (pkcredential);

create table hm_directory
(
	dirid int auto_increment not null, primary key(`dirid`), unique(`dirid`),
	diraccountid int not null,
	dirtitle varchar(100) not null default '',
	dirdepartment varchar(100) not null default '',
	dirphone varchar(60) not null default '',
	dirmobile varchar(60) not null default '',
	diroffice varchar(100) not null default '',
	dirphototype varchar(40) not null default '',
	dirphoto mediumtext not null,
	diroptout tinyint not null default 0,
	dirnote varchar(255) not null default '',
	dirupdated datetime not null
) DEFAULT CHARSET=utf8;

CREATE UNIQUE INDEX idx_hm_directory_account ON hm_directory (diraccountid);

create table hm_recoverable
(
	rcid int auto_increment not null, primary key(`rcid`), unique(`rcid`),
	rcaccountid int not null,
	rcfolderid int not null,
	rcfolder varchar(255) not null,
	rcfile varchar(255) not null,
	rcsize int not null,
	rcsubject varchar(255) not null,
	rcfrom varchar(255) not null,
	rcflags int not null,
	rcdeleted bigint not null
) DEFAULT CHARSET=utf8;

CREATE INDEX idx_hm_recoverable_account ON hm_recoverable (rcaccountid);

create table hm_confidential
(
	cfid int auto_increment not null, primary key(`cfid`), unique(`cfid`),
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
) DEFAULT CHARSET=utf8;

CREATE UNIQUE INDEX idx_hm_confidential_token ON hm_confidential (cftoken);

CREATE INDEX idx_hm_confidential_account ON hm_confidential (cfaccountid);

create table hm_dmarcreports
(
	drid int auto_increment not null, primary key(`drid`), unique(`drid`),
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
) DEFAULT CHARSET=utf8;

CREATE UNIQUE INDEX idx_hm_dmarcreports_key ON hm_dmarcreports (drkey);

CREATE INDEX idx_hm_dmarcreports_domain ON hm_dmarcreports (drdomainid);

CREATE INDEX idx_hm_dmarcreports_received ON hm_dmarcreports (drreceived);

create table hm_dmarcreportrecords
(
	rrid int auto_increment not null, primary key(`rrid`), unique(`rrid`),
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
) DEFAULT CHARSET=utf8;

CREATE INDEX idx_hm_dmarcreportrecords_report ON hm_dmarcreportrecords (rrreportid);

create table hm_tlsreports
(
	trid int auto_increment not null, primary key(`trid`), unique(`trid`),
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
) DEFAULT CHARSET=utf8;

CREATE UNIQUE INDEX idx_hm_tlsreports_key ON hm_tlsreports (trkey);

CREATE INDEX idx_hm_tlsreports_domain ON hm_tlsreports (trdomainid);

CREATE INDEX idx_hm_tlsreports_received ON hm_tlsreports (trreceived);

create table hm_accountactivity
(
	acid bigint auto_increment not null, primary key(`acid`), unique(`acid`),
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
) DEFAULT CHARSET=utf8;

CREATE INDEX idx_hm_accountactivity_account ON hm_accountactivity (acaccountid, actime);

create table hm_accountrecipients
(
	arcid bigint auto_increment not null, primary key(`arcid`), unique(`arcid`),
	arcaccountid int not null,
	arcaddress varchar(255) not null,
	arcfirst bigint not null,
	arclast bigint not null,
	arccount int not null
) DEFAULT CHARSET=utf8;

CREATE INDEX idx_hm_accountrecipients_address ON hm_accountrecipients (arcaccountid, arcaddress);

create table hm_accountreview
(
	arvid int auto_increment not null, primary key(`arvid`), unique(`arvid`),
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
) DEFAULT CHARSET=utf8;

CREATE UNIQUE INDEX idx_hm_accountreview_account ON hm_accountreview (arvaccountid);

create table hm_heldmessages
(
	hmid bigint auto_increment not null, primary key(`hmid`), unique(`hmid`),
	hmaccountid int not null,
	hmmessageid bigint not null,
	hmheld bigint not null,
	hmsource int not null,
	hmsender varchar(255) not null,
	hmrecipients varchar(2000) not null,
	hmsubject varchar(255) not null,
	hmsize bigint not null,
	hmreason varchar(255) not null
) DEFAULT CHARSET=utf8;

CREATE INDEX idx_hm_heldmessages_account ON hm_heldmessages (hmaccountid);

CREATE INDEX idx_hm_heldmessages_message ON hm_heldmessages (hmmessageid);

create table hm_remotedomainpolicies
(
	policyid int auto_increment not null, primary key(`policyid`), unique(`policyid`),
	policydomainname varchar(255) not null,
	policydescription varchar(255) not null,
	policyactive tinyint not null,
	policyoutboundtls int not null,
	policyinboundtls tinyint not null,
	policymaxmessagesizekb int not null,
	policymaxconnections int not null,
	policymaxperminute int not null,
	policyallowreplies tinyint not null,
	policyallowforwarding tinyint not null,
	policycalloutenabled tinyint not null,
	policycallouthost varchar(255) not null,
	policycalloutport int not null,
	policycallouttimeout int not null,
	policycalloutcacheminutes int not null,
	policycalloutperminute int not null
);

CREATE INDEX idx_hm_remotedomainpolicies_domain ON hm_remotedomainpolicies (policydomainname);

create table hm_rules
(
	ruleid int auto_increment not null, primary key(`ruleid`), unique(`ruleid`),
	ruleaccountid int not null,
	ruledomainid int not null default 0,
	rulename varchar(100) not null,
	ruleactive tinyint not null,
	ruleuseand tinyint not null,
	rulesortorder int not null
) DEFAULT CHARSET=utf8;

CREATE INDEX idx_rules ON hm_rules (ruleaccountid);

create table hm_rule_criterias
(
	criteriaid int auto_increment not null, primary key(`criteriaid`), unique(`criteriaid`),
	criteriaruleid int not null,
	criteriausepredefined tinyint not null,
	criteriapredefinedfield tinyint not null,
	criteriaheadername varchar(255) not null,
	criteriamatchtype tinyint not null,
	criteriamatchvalue varchar(2000) not null
	
) DEFAULT CHARSET=utf8;

CREATE INDEX idx_rules_criterias ON hm_rule_criterias (criteriaruleid);

create table hm_rule_actions
(
	actionid int auto_increment not null, primary key(`actionid`), unique(`actionid`),
	actionruleid int not null,
	actiontype tinyint not null,
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
	actionabortspamflagged tinyint not null
) DEFAULT CHARSET=utf8;

CREATE INDEX idx_rules_actions ON hm_rule_actions (actionruleid);

create table hm_surblservers
(
	surblid int auto_increment not null, primary key(surblid), unique(surblid),
	surblactive tinyint not null,
	surblhost varchar(255) not null,
	surblrejectmessage varchar(255) not null,
	surblresult varchar(255) not null default '',
	surblscore int not null
) DEFAULT CHARSET=utf8;

create table hm_greylisting_triplets
(
	glid bigint auto_increment not null, primary key(glid), unique(glid),
	glcreatetime datetime not null,
	glblockendtime datetime not null,
	gldeletetime datetime not null,
	glipaddress1 bigint not null,
   glipaddress2 bigint null,
	glsenderaddress varchar(320) not null,
	glrecipientaddress varchar(320) not null,
	glblockedcount int not null,
	glpassedcount int not null,
	glsenderhash varchar(32) null,
	glrecipienthash varchar(32) null
) DEFAULT CHARSET=utf8;

CREATE INDEX idx_hm_greylisting_triplets_key ON hm_greylisting_triplets (glipaddress1, glipaddress2, glsenderhash, glrecipienthash);

create table hm_greylisting_whiteaddresses
(
	whiteid bigint auto_increment not null, primary key(whiteid), unique(whiteid),
	whiteipaddress varchar(255) not null,
	whiteipdescription varchar(255) not null
) DEFAULT CHARSET=utf8;

create table hm_greylisting_bypass
(
	glbid bigint auto_increment not null, primary key(`glbid`), unique(`glbid`),
	glbpattern varchar(255) not null,
	glbmatch int not null,
	glbrequire int not null,
	glbnote varchar(255) not null
) DEFAULT CHARSET=utf8;

create table hm_blocked_attachments
(
	baid bigint auto_increment not null, primary key(baid), unique(baid),
	bawildcard varchar(255) not null,
	badescription varchar(255) not null
) DEFAULT CHARSET=utf8;

create table hm_servermessages
(
	smid int auto_increment not null, primary key(smid), unique(smid), 
	smname varchar(255) not null,
	smtext text not null
) DEFAULT CHARSET=utf8;

create table hm_tcpipports
(
	portid bigint auto_increment not null, primary key(portid), unique(portid),
	portprotocol tinyint not null,
	portnumber int not null,
	portaddress1 bigint not null,
   portaddress2 bigint null,
	portconnectionsecurity tinyint not null,
	portsslcertificateid bigint not null,
	portclientcertificatepolicy tinyint not null default 0,
	portclientcertificatecafile varchar(255) not null default ''
) DEFAULT CHARSET=utf8;

create table hm_whitelist
(
	whiteid bigint auto_increment not null, primary key(whiteid), unique(whiteid),
	whiteloweripaddress1 bigint not null,
   whiteloweripaddress2 bigint null,
	whiteupperipaddress1 bigint not null,
   whiteupperipaddress2 bigint null,
	whiteemailaddress varchar(255) not null,
	whitedescription varchar(255) not null
) DEFAULT CHARSET=utf8;

create table hm_blocked_senders
(
	bsid bigint auto_increment not null, primary key(bsid), unique(bsid),
	bsaddress varchar(255) not null,
	bsscore int not null,
	bsdescription varchar(255) not null,
	unique u_bsaddress (bsaddress)
) DEFAULT CHARSET=utf8;

create table hm_sslcertificates
(
	sslcertificateid bigint auto_increment not null, primary key(sslcertificateid), unique(sslcertificateid),
	sslcertificatename varchar(255) not null,
	sslcertificatefile varchar(255) not null,
	sslprivatekeyfile varchar(255) not null,
	sslprivatekeypassword varchar(1024) not null
	
) DEFAULT CHARSET=utf8;

create table hm_groups
(
	groupid bigint auto_increment not null, primary key(`groupid`), unique(`groupid`),
    groupname varchar(255)
) DEFAULT CHARSET=utf8;

create table hm_group_members
(
	memberid bigint auto_increment not null, primary key(`memberid`), unique(`memberid`),
	membergroupid bigint not null,
	memberaccountid bigint not null
) DEFAULT CHARSET=utf8;

create table hm_acl
(
	aclid bigint auto_increment not null, primary key(`aclid`), unique(`aclid`),
   aclsharefolderid bigint not null,
	aclpermissiontype tinyint not null,
	aclpermissiongroupid bigint not null,
	aclpermissionaccountid bigint not null,
	aclvalue bigint not null,
	unique(aclsharefolderid, aclpermissiontype, aclpermissiongroupid, aclpermissionaccountid)
) DEFAULT CHARSET=utf8;

create table hm_incoming_relays
(
   relayid int auto_increment not null, primary key(`relayid`), unique(`relayid`),
   relayname varchar(100) not null,
   relaylowerip1 bigint not null,
   relaylowerip2 bigint null,
   relayupperip1 bigint not null,
   relayupperip2 bigint null
) DEFAULT CHARSET=utf8; 

create table hm_logon_failures
(
   ipaddress1 bigint not null,
   ipaddress2 bigint null,
   failuretime datetime not null
) DEFAULT CHARSET=utf8;

CREATE INDEX idx_hm_logon_failures_ipaddress ON hm_logon_failures (ipaddress1, ipaddress2);

CREATE INDEX idx_hm_logon_failures_failuretime ON hm_logon_failures (failuretime);

insert into hm_securityranges (rangepriorityid, rangelowerip1, rangelowerip2, rangeupperip1, rangeupperip2, rangeoptions, rangename, rangeexpires, rangeexpirestime) values (10, 0, NULL, 4294967295, NULL, 96203, 'Internet', 0, '2001-01-01');

insert into hm_securityranges (rangepriorityid, rangelowerip1, rangelowerip2, rangeupperip1, rangeupperip2, rangeoptions, rangename, rangeexpires, rangeexpirestime) values (30, 2130706433, NULL, 2130706433, NULL, 71627, 'My computer', 0, '2001-01-01');

insert into hm_servermessages (smname, smtext) values ('VIRUS_FOUND', 'Virus found');

insert into hm_servermessages (smname, smtext) values ('VIRUS_ATTACHMENT_REMOVED', 'Virus found:\r\nThe attachment(s) of this message was removed since a virus was detected in at least one of them.\r\n\r\n');

insert into hm_servermessages (smname, smtext) values ('VIRUS_NOTIFICATION', 'The message below contained a virus and did not\r\nreach some or all of the intended recipients.\r\n\r\n   From: %MACRO_FROM%\r\n   To: %MACRO_TO%\r\n   Sent: %MACRO_SENT%\r\n   Subject: %MACRO_SUBJECT%\r\n\r\nhMailServer\r\n');

insert into hm_servermessages (smname, smtext) values ('SEND_FAILED_NOTIFICATION', 'Your message did not reach some or all of the intended recipients.\r\n\r\n   Sent: %MACRO_SENT%\r\n   Subject: %MACRO_SUBJECT%\r\n\r\nThe following recipient(s) could not be reached:\r\n\r\n%MACRO_RECIPIENTS%\r\n\r\nhMailServer\r\n');

insert into hm_servermessages (smname, smtext) values ('MESSAGE_UNDELIVERABLE', 'Message undeliverable');

insert into hm_servermessages (smname, smtext) values ('MESSAGE_FILE_MISSING', 'The mail server could not deliver the message to you since the file %MACRO_FILE% does not exist on the server.\r\n\r\nThe file may have been deleted by anti virus software running on the server.\r\n\r\nhMailServer');

insert into hm_servermessages (smname, smtext) values ('ATTACHMENT_REMOVED', 'The attachment %MACRO_FILE% was blocked for delivery by the e-mail server. Please contact your system administrator if you have any questions regarding this.\r\n\r\nhMailServer\r\n');

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
	metadataid bigint auto_increment not null, primary key(`metadataid`), unique(`metadataid`),
	metadataaccountid bigint not null,
	metadatafolderid bigint not null,
	metadataentryname varchar(255) not null,
	metadatavalue varchar(2048) not null,
	unique(metadataaccountid, metadatafolderid, metadataentryname)
) DEFAULT CHARSET=utf8;

create table hm_quarantine
(
	quarantineid int auto_increment not null, primary key(`quarantineid`), unique(`quarantineid`),
	quarantinefilename varchar(255) not null,
	quarantinesender varchar(255) not null,
	quarantinerecipients varchar(1000) not null,
	quarantinesubject varchar(255) not null,
	quarantinereason varchar(255) not null,
	quarantinescore int not null,
	quarantinesize int not null,
	quarantinecreated datetime not null
) DEFAULT CHARSET=utf8;

CREATE INDEX idx_hm_quarantine_created ON hm_quarantine (quarantinecreated);

create table hm_senderlist
(
	slid int auto_increment not null, primary key(`slid`), unique(`slid`),
	slscope int not null,
	slownerid int not null,
	sltype int not null,
	slvalue varchar(255) not null,
	slreason varchar(255) not null,
	slexpires tinyint not null,
	slexpirestime datetime not null,
	slcreated datetime not null
);

CREATE INDEX idx_hm_senderlist_scope ON hm_senderlist (slscope, slownerid);

ALTER TABLE hm_senderlist ENGINE=InnoDB;

create table hm_digeststate
(
	dsid int auto_increment not null, primary key(`dsid`), unique(`dsid`),
	dsaccountid int not null,
	dssenttime datetime not null,
	dslastid int not null,
	dsoptout tinyint not null
);

CREATE UNIQUE INDEX idx_hm_digeststate_account ON hm_digeststate (dsaccountid);

ALTER TABLE hm_digeststate ENGINE=InnoDB;

create table hm_digesttokens
(
	dtid int auto_increment not null, primary key(`dtid`), unique(`dtid`),
	dtaccountid int not null,
	dtkind int not null,
	dtquarantineid int not null,
	dttoken varchar(64) not null,
	dtexpirestime datetime not null,
	dtused tinyint not null,
	dtcreated datetime not null
);

CREATE UNIQUE INDEX idx_hm_digesttokens_token ON hm_digesttokens (dttoken);

ALTER TABLE hm_digesttokens ENGINE=InnoDB;

create table hm_passwordhistory
(
	phid int auto_increment not null, primary key(`phid`), unique(`phid`),
	phaccountid int not null,
	phhash varchar(255) not null,
	phencryption int not null,
	phchanged datetime not null
) DEFAULT CHARSET=utf8;

CREATE INDEX idx_hm_passwordhistory ON hm_passwordhistory (phaccountid);

create table hm_messagetrace
(
	mtid int auto_increment not null, primary key(`mtid`), unique(`mtid`),
	mtqueueid int not null,
	mtoccurred datetime not null,
	mtevent varchar(32) not null,
	mtsender varchar(255) not null,
	mtrecipient varchar(255) not null,
	mtsourceip varchar(64) not null,
	mtstatuscode int not null,
	mtdetail varchar(255) not null
) DEFAULT CHARSET=utf8;

CREATE INDEX idx_hm_messagetrace ON hm_messagetrace (mtoccurred);

insert into hm_servermessages (smname, smtext) values ('QUOTA_WARNING', 'Your mailbox is %MACRO_PERCENT%% full.\r\n\r\n   Mailbox: %MACRO_MAILBOX%\r\n   Used: %MACRO_USED% MB of %MACRO_QUOTA% MB\r\n\r\nWhen it is completely full, new messages sent to you will be refused and\r\nthe sender will be told to try again later. Deleting mail you no longer\r\nneed - and emptying the trash folder afterwards - will free the space.\r\n\r\nhMailServer\r\n');

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
	metricsampleid int auto_increment not null, primary key(`metricsampleid`), unique(`metricsampleid`),
	metricsampletime datetime not null,
	metricsamplename varchar(64) not null,
	metricsamplevalue double not null
) DEFAULT CHARSET=utf8;

CREATE INDEX idx_hm_metricsamples_name_time ON hm_metricsamples (metricsamplename, metricsampletime);

create table hm_archiveindex
(
	archiveid int auto_increment not null, primary key(`archiveid`), unique(`archiveid`),
	archivetime datetime not null,
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
) DEFAULT CHARSET=utf8;

CREATE INDEX idx_hm_archiveindex_domain_time ON hm_archiveindex (archivedomain, archivetime);

CREATE INDEX idx_hm_archiveindex_path ON hm_archiveindex (archivepath(255));

-- Referential integrity (schema 6030): InnoDB enforces these.

ALTER TABLE hm_domains ENGINE=InnoDB;

ALTER TABLE hm_accounts ENGINE=InnoDB;

ALTER TABLE hm_aliases ENGINE=InnoDB;

ALTER TABLE hm_domain_aliases ENGINE=InnoDB;

ALTER TABLE hm_distributionlists ENGINE=InnoDB;

ALTER TABLE hm_distributionlistsrecipients ENGINE=InnoDB;

ALTER TABLE hm_routes ENGINE=InnoDB;

ALTER TABLE hm_routeaddresses ENGINE=InnoDB;

ALTER TABLE hm_fetchaccounts ENGINE=InnoDB;

ALTER TABLE hm_fetchaccounts_uids ENGINE=InnoDB;

ALTER TABLE hm_apppasswords ENGINE=InnoDB;

ALTER TABLE hm_contacts ENGINE=InnoDB;

ALTER TABLE hm_addressbooks ENGINE=InnoDB;

ALTER TABLE hm_addressbookshares ENGINE=InnoDB;

ALTER TABLE hm_accountprefs ENGINE=InnoDB;

ALTER TABLE hm_scheduled ENGINE=InnoDB;

ALTER TABLE hm_files ENGINE=InnoDB;

ALTER TABLE hm_smimekeys ENGINE=InnoDB;

ALTER TABLE hm_calendars ENGINE=InnoDB;

ALTER TABLE hm_calendarshares ENGINE=InnoDB;

ALTER TABLE hm_senderaddresses ENGINE=InnoDB;

ALTER TABLE hm_maskedaddresses ENGINE=InnoDB;

ALTER TABLE hm_recoverable ENGINE=InnoDB;

ALTER TABLE hm_passkeys ENGINE=InnoDB;

ALTER TABLE hm_directory ENGINE=InnoDB;

ALTER TABLE hm_confidential ENGINE=InnoDB;

ALTER TABLE hm_dmarcreports ENGINE=InnoDB;

ALTER TABLE hm_dmarcreportrecords ENGINE=InnoDB;

ALTER TABLE hm_tlsreports ENGINE=InnoDB;

ALTER TABLE hm_accountactivity ENGINE=InnoDB;

ALTER TABLE hm_accountrecipients ENGINE=InnoDB;

ALTER TABLE hm_accountreview ENGINE=InnoDB;

ALTER TABLE hm_heldmessages ENGINE=InnoDB;

ALTER TABLE hm_calendarobjects ENGINE=InnoDB;

ALTER TABLE hm_remotedomainpolicies ENGINE=InnoDB;

ALTER TABLE hm_rules ENGINE=InnoDB;

ALTER TABLE hm_rule_criterias ENGINE=InnoDB;

ALTER TABLE hm_rule_actions ENGINE=InnoDB;

ALTER TABLE hm_groups ENGINE=InnoDB;

ALTER TABLE hm_group_members ENGINE=InnoDB;

ALTER TABLE hm_passwordhistory ENGINE=InnoDB;

ALTER TABLE hm_messages ENGINE=InnoDB;

ALTER TABLE hm_messagerecipients ENGINE=InnoDB;

ALTER TABLE hm_message_metadata ENGINE=InnoDB;

ALTER TABLE hm_imapfolders ENGINE=InnoDB;

ALTER TABLE hm_imapexpunged ENGINE=InnoDB;

ALTER TABLE hm_messageindexterms ENGINE=InnoDB;

ALTER TABLE hm_messageindexstate ENGINE=InnoDB;



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
	ksid bigint auto_increment not null, primary key(`ksid`), unique(`ksid`),
	ksaccountid int not null,
	ksaddress varchar(255) character set utf8 collate utf8_bin not null,
	kscount int not null,
	ksfirstseen varchar(32) not null,
	kslastseen varchar(32) not null
) DEFAULT CHARSET=utf8;

CREATE UNIQUE INDEX idx_hm_knownsenders_account ON hm_knownsenders (ksaccountid, ksaddress);

create table hm_autoreplies
(
	arid bigint auto_increment not null, primary key(`arid`), unique(`arid`),
	araccountid int not null,
	arsender varchar(255) character set utf8 collate utf8_bin not null,
	arsent bigint not null
) DEFAULT CHARSET=utf8;

CREATE UNIQUE INDEX idx_hm_autoreplies_account ON hm_autoreplies (araccountid, arsender);

create table hm_domainsamples
(
	dsid int auto_increment not null, primary key(`dsid`), unique(`dsid`),
	dsday varchar(10) not null,
	dsdomain varchar(255) character set utf8 collate utf8_bin not null,
	dsspam int not null,
	dsviruses int not null,
	dsstorebytes bigint not null,
	dsstoremessages bigint not null
) DEFAULT CHARSET=utf8;

CREATE UNIQUE INDEX idx_hm_domainsamples_day_domain ON hm_domainsamples (dsday, dsdomain);

create table hm_dkimrotations
(
	dkrid bigint auto_increment not null, primary key(`dkrid`), unique(`dkrid`),
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
) DEFAULT CHARSET=utf8;

CREATE UNIQUE INDEX idx_hm_dkimrotations_domain ON hm_dkimrotations (dkrdomainid);

create table hm_spamtraps
(
	stid int auto_increment not null, primary key(`stid`), unique(`stid`),
	stdomainid int not null,
	stlocalpart varchar(64) not null,
	stcreated datetime not null
) DEFAULT CHARSET=utf8;

CREATE UNIQUE INDEX idx_hm_spamtraps_address ON hm_spamtraps (stdomainid, stlocalpart);

ALTER TABLE hm_spamtraps ENGINE=InnoDB;

create table hm_spamtraphits
(
	sthid int auto_increment not null, primary key(`sthid`), unique(`sthid`),
	sthdomainid int not null,
	sthaddress varchar(255) not null,
	sthipaddress varchar(64) not null,
	sthsender varchar(255) not null,
	sthtime datetime not null,
	sthbanned tinyint not null,
	sthblocked tinyint not null,
	sthnote varchar(255) not null
) DEFAULT CHARSET=utf8;

CREATE INDEX idx_hm_spamtraphits_time ON hm_spamtraphits (sthtime);

CREATE INDEX idx_hm_spamtraphits_domain ON hm_spamtraphits (sthdomainid);

ALTER TABLE hm_spamtraphits ENGINE=InnoDB;

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
	auditid bigint auto_increment not null, primary key(`auditid`), unique(`auditid`),
	audittime bigint not null,
	auditactor varchar(255) not null,
	auditactortype varchar(32) not null,
	auditinterface varchar(32) not null,
	auditaddress varchar(64) not null,
	auditobjecttype varchar(64) not null,
	auditobjectname varchar(255) not null,
	auditaction varchar(32) not null,
	auditdetail mediumtext not null,
	auditprevhash varchar(64) not null,
	audithash varchar(64) not null
);

ALTER TABLE hm_audit ENGINE=InnoDB;

CREATE INDEX idx_hm_audit_time ON hm_audit (audittime);

CREATE INDEX idx_hm_audit_actor ON hm_audit (auditactor);

create table hm_alertrules
(
	alertruleid bigint auto_increment not null, primary key(`alertruleid`), unique(`alertruleid`),
	alertrulecondition varchar(64) not null,
	alertruleenabled tinyint not null,
	alertrulethreshold int not null,
	alertruleactions int not null,
	alertruleaddress varchar(255) not null,
	alertrulewebhook varchar(1024) not null,
	alertrulesecret varchar(255) not null,
	alertrulecooldown int not null,
	alertruledigest tinyint not null
);

ALTER TABLE hm_alertrules ENGINE=InnoDB;

CREATE UNIQUE INDEX idx_hm_alertrules_condition ON hm_alertrules (alertrulecondition);

create table hm_alertevents
(
	alerteventid bigint auto_increment not null, primary key(`alerteventid`), unique(`alerteventid`),
	alerteventcondition varchar(64) not null,
	alerteventseverity int not null,
	alerteventstate int not null,
	alerteventtime bigint not null,
	alerteventsummary varchar(255) not null,
	alerteventdetail mediumtext not null,
	alerteventnotified tinyint not null,
	alerteventdigested tinyint not null,
	alerteventhookstate int not null,
	alerteventhooktries int not null,
	alerteventhooknext bigint not null
);

ALTER TABLE hm_alertevents ENGINE=InnoDB;

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
	rcid bigint auto_increment not null, primary key(`rcid`), unique(`rcid`),
	rcaddress varchar(64) not null,
	rcsources varchar(64) not null,
	rczone varchar(255) not null,
	rcstatus int not null,
	rccode varchar(255) not null,
	rcreason varchar(255) not null,
	rcchecked bigint not null,
	rclistedsince bigint not null,
	rcconfirmed bigint not null
) DEFAULT CHARSET=utf8;

ALTER TABLE hm_reputationchecks ENGINE=InnoDB;

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
	roleid int auto_increment not null, primary key(`roleid`), unique(`roleid`),
	rolename varchar(64) not null,
	roledescription varchar(255) not null,
	rolebuiltin tinyint not null,
	rolepermissions varchar(1000) not null
);

CREATE UNIQUE INDEX idx_hm_roles_name ON hm_roles (rolename);

ALTER TABLE hm_roles ENGINE=InnoDB;

create table hm_roleassignments
(
	raid int auto_increment not null, primary key(`raid`), unique(`raid`),
	raaccountid int not null,
	raroleid int not null,
	radomains varchar(1000) not null
);

CREATE UNIQUE INDEX idx_hm_roleassignments_account ON hm_roleassignments (raaccountid);

ALTER TABLE hm_roleassignments ENGINE=InnoDB;

ALTER TABLE hm_roleassignments ADD CONSTRAINT fk_hm_roleassignments_account FOREIGN KEY (raaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

ALTER TABLE hm_roleassignments ADD CONSTRAINT fk_hm_roleassignments_role FOREIGN KEY (raroleid) REFERENCES hm_roles (roleid);

insert into hm_roles (rolename, roledescription, rolebuiltin, rolepermissions) values ('serveradmin', 'Every permission, in every domain.', 1, 'administer_server,administer_domain,view_configuration,view_accounts,view_mailbox,view_queue,view_quarantine,view_archive,view_logs,view_message_trace,view_audit,manage_accounts,reset_password,unlock_account,manage_mailbox,manage_queue,manage_quarantine,manage_archive,manage_settings,manage_domains,manage_server,manage_roles');

insert into hm_roles (rolename, roledescription, rolebuiltin, rolepermissions) values ('domainadmin', 'The accounts, mailboxes and quarantine of named domains.', 1, 'administer_domain,view_configuration,view_accounts,view_mailbox,view_quarantine,view_message_trace,manage_accounts,reset_password,unlock_account,manage_mailbox,manage_quarantine');

insert into hm_roles (rolename, roledescription, rolebuiltin, rolepermissions) values ('helpdesk', 'Resets a password, unlocks an account and reads a message trace.', 1, 'view_accounts,view_message_trace,reset_password,unlock_account');

insert into hm_roles (rolename, roledescription, rolebuiltin, rolepermissions) values ('auditor', 'Reads everything and changes nothing, the audit trail included.', 1, 'view_configuration,view_accounts,view_mailbox,view_queue,view_quarantine,view_archive,view_logs,view_message_trace,view_audit');

create table hm_signins
(
	siid bigint auto_increment not null, primary key(`siid`), unique(`siid`),
	siaccountid int not null,
	sidevice varchar(32) not null,
	siprotocol int not null,
	siaddress varchar(64) not null,
	siagent varchar(255) not null,
	sicredential int not null,
	sicredentialname varchar(64) not null,
	sifirstseen datetime not null,
	silastseen datetime not null,
	sicount int not null,
	sirevoked tinyint not null,
	sirevokedtime datetime null
) DEFAULT CHARSET=utf8;

CREATE UNIQUE INDEX idx_hm_signins_device ON hm_signins (siaccountid, sidevice);

ALTER TABLE hm_signins ENGINE=InnoDB;

create table hm_bouncesuppressions
(
	bsid bigint auto_increment not null, primary key(`bsid`), unique(`bsid`),
	bsdomainid int not null,
	bsaddress varchar(255) not null,
	bssource int not null,
	bscategory int not null,
	bsstatus varchar(16) not null,
	bsdiagnostic varchar(255) not null,
	bsremotemta varchar(255) not null,
	bscreated varchar(32) not null,
	bsexpires varchar(32) not null
) DEFAULT CHARSET=utf8;

CREATE UNIQUE INDEX idx_hm_bouncesuppressions_address ON hm_bouncesuppressions (bsdomainid, bsaddress);

create table hm_bouncecounts
(
	bcid bigint auto_increment not null, primary key(`bcid`), unique(`bcid`),
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
) DEFAULT CHARSET=utf8;

CREATE UNIQUE INDEX idx_hm_bouncecounts_address ON hm_bouncecounts (bcdomainid, bcaddress);

create table hm_bouncereportkeys
(
	brkid bigint auto_increment not null, primary key(`brkid`), unique(`brkid`),
	brkdomainid int not null,
	brkkey varchar(64) not null,
	brksent varchar(32) not null,
	brkaddress varchar(64) not null,
	brkrun int not null
) DEFAULT CHARSET=utf8;

CREATE INDEX idx_hm_bouncereportkeys_key ON hm_bouncereportkeys (brkkey);

CREATE INDEX idx_hm_bouncereportkeys_sent ON hm_bouncereportkeys (brksent);

CREATE INDEX idx_hm_bouncereportkeys_address ON hm_bouncereportkeys (brkaddress);

ALTER TABLE hm_bouncesuppressions ADD CONSTRAINT fk_hm_bouncesuppressions_domain FOREIGN KEY (bsdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE;

ALTER TABLE hm_bouncecounts ADD CONSTRAINT fk_hm_bouncecounts_domain FOREIGN KEY (bcdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE;

ALTER TABLE hm_bouncereportkeys ADD CONSTRAINT fk_hm_bouncereportkeys_domain FOREIGN KEY (brkdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE;

create table hm_pgpkeys
(
	pgpkid int auto_increment not null, primary key(`pgpkid`), unique(`pgpkid`),
	pgpkaccountid int not null,
	pgpkfingerprint varchar(64) not null,
	pgpkversion int not null,
	pgpkalgorithm int not null,
	pgpkbits int not null,
	pgpkuserid varchar(255) not null,
	pgpkcreated bigint not null,
	pgpkexpires bigint not null,
	pgpkbytes int not null,
	pgpkkey mediumtext not null,
	pgpkuploaded bigint not null,
	pgpkuploadedby varchar(255) not null
) DEFAULT CHARSET=utf8;

CREATE INDEX idx_hm_pgpkeys_account ON hm_pgpkeys (pgpkaccountid);

CREATE UNIQUE INDEX idx_hm_pgpkeys_entry ON hm_pgpkeys (pgpkaccountid, pgpkfingerprint);

ALTER TABLE hm_pgpkeys ENGINE=InnoDB;

ALTER TABLE hm_pgpkeys ADD CONSTRAINT fk_hm_pgpkeys_account FOREIGN KEY (pgpkaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

create table hm_pgpring
(
	pgrid int auto_increment not null, primary key(`pgrid`), unique(`pgrid`),
	pgraccountid int not null,
	pgrkind tinyint not null,
	pgraddress varchar(255) not null,
	pgrname varchar(255) not null,
	pgrfingerprint varchar(64) not null,
	pgrkeyid varchar(16) not null,
	pgralgorithm varchar(32) not null,
	pgrpublic mediumtext not null,
	pgrsecret mediumtext not null,
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
) DEFAULT CHARSET=utf8;

CREATE INDEX idx_hm_pgpring_account ON hm_pgpring (pgraccountid);

CREATE UNIQUE INDEX idx_hm_pgpring_entry ON hm_pgpring (pgraccountid, pgrkind, pgrfingerprint);

ALTER TABLE hm_pgpring ENGINE=InnoDB;

ALTER TABLE hm_pgpring ADD CONSTRAINT fk_hm_pgpring_account FOREIGN KEY (pgraccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

create table hm_protectedmessages
(
	pmid int auto_increment not null, primary key(`pmid`), unique(`pmid`),
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
) DEFAULT CHARSET=utf8;

CREATE UNIQUE INDEX idx_hm_protectedmessages_token ON hm_protectedmessages (pmtoken);

CREATE INDEX idx_hm_protectedmessages_account ON hm_protectedmessages (pmaccountid);

ALTER TABLE hm_protectedmessages ENGINE=InnoDB;

ALTER TABLE hm_protectedmessages ADD CONSTRAINT fk_hm_protectedmessages_account FOREIGN KEY (pmaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

insert into hm_settings (settingname, settingstring, settinginteger) values ('ProtectedMessageDays', '', 30);

insert into hm_settings (settingname, settingstring, settinginteger) values ('OpenPGPWebKeyDirectoryEnabled', '', 0);

insert into hm_settings (settingname, settingstring, settinginteger) values ('OpenPGPKeyMaximumBytes', '', 65536);

insert into hm_settings (settingname, settingstring, settinginteger) values ('OpenPGPKeysPerAccount', '', 8);

insert into hm_settings (settingname, settingstring, settinginteger) values ('OpenPGPRequireAddressInUserId', '', 1);

create table hm_webhooks
(
	whid bigint auto_increment not null, primary key(`whid`), unique(`whid`),
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
	wqid bigint auto_increment not null, primary key(`wqid`), unique(`wqid`),
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

ALTER TABLE hm_webhooks ENGINE=InnoDB;

ALTER TABLE hm_webhookqueue ENGINE=InnoDB;

ALTER TABLE hm_webhookqueue ADD CONSTRAINT fk_hm_webhookqueue_webhook FOREIGN KEY (wqwebhookid) REFERENCES hm_webhooks (whid) ON DELETE CASCADE;

insert into hm_settings (settingname, settingstring, settinginteger) values ('WebhooksEnabled', '', 1);

insert into hm_settings (settingname, settingstring, settinginteger) values ('WebhookMaxAttempts', '', 5);

insert into hm_settings (settingname, settingstring, settinginteger) values ('WebhookMaxPerMinute', '', 60);

insert into hm_settings (settingname, settingstring, settinginteger) values ('WebhookRetentionDays', '', 7);

ALTER TABLE hm_signins ADD CONSTRAINT fk_hm_signins_account FOREIGN KEY (siaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

create table hm_pushsubscriptions
(
	psid bigint auto_increment not null, primary key(`psid`), unique(`psid`),
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
) DEFAULT CHARSET=utf8;

CREATE UNIQUE INDEX idx_hm_pushsubscriptions_endpoint ON hm_pushsubscriptions (psaccountid, psendpointhash);

ALTER TABLE hm_pushsubscriptions ENGINE=InnoDB;

ALTER TABLE hm_pushsubscriptions ADD CONSTRAINT fk_hm_pushsubscriptions_account FOREIGN KEY (psaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

create table hm_complaints
(
	cpid bigint auto_increment not null, primary key(`cpid`), unique(`cpid`),
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
) DEFAULT CHARSET=utf8;

CREATE UNIQUE INDEX idx_hm_complaints_key ON hm_complaints (cpkey);

CREATE INDEX idx_hm_complaints_domain ON hm_complaints (cpdomainid);

CREATE INDEX idx_hm_complaints_address ON hm_complaints (cpaddress);

CREATE INDEX idx_hm_complaints_received ON hm_complaints (cpreceived);

create table hm_complaintvolume
(
	cvid bigint auto_increment not null, primary key(`cvid`), unique(`cvid`),
	cvdomainid int not null,
	cvday varchar(10) not null,
	cvsent bigint not null,
	cvcomplaints int not null
) DEFAULT CHARSET=utf8;

CREATE UNIQUE INDEX idx_hm_complaintvolume_day ON hm_complaintvolume (cvdomainid, cvday);

ALTER TABLE hm_complaints ADD CONSTRAINT fk_hm_complaints_domain FOREIGN KEY (cpdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE;

ALTER TABLE hm_complaintvolume ADD CONSTRAINT fk_hm_complaintvolume_domain FOREIGN KEY (cvdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE;

insert into hm_alertrules (alertrulecondition, alertruleenabled, alertrulethreshold, alertruleactions, alertruleaddress, alertrulewebhook, alertrulesecret, alertrulecooldown, alertruledigest) values ('complaints.rate.high', 1, 1000, 1, '', '', '', 1440, 1);

create table hm_mailboxmoves
(
	mmid bigint auto_increment not null, primary key(`mmid`), unique(`mmid`),
	mmaccountid int not null,
	mmoldaddress varchar(255) not null,
	mmnewaddress varchar(255) not null,
	mmstage int not null,
	mmkeepalias int not null,
	mmmessages int not null,
	mmstarted varchar(32) not null,
	mmfinished varchar(32) not null,
	mmdetail varchar(255) not null
) DEFAULT CHARSET=utf8;

CREATE INDEX idx_hm_mailboxmoves_account ON hm_mailboxmoves (mmaccountid);

ALTER TABLE hm_mailboxmoves ENGINE=InnoDB;

ALTER TABLE hm_mailboxmoves ADD CONSTRAINT fk_hm_mailboxmoves_account FOREIGN KEY (mmaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

create table hm_domainpolicylist
(
	dplid int auto_increment not null, primary key(`dplid`), unique(`dplid`),
	dplpattern varchar(255) not null,
	dplmatchon int not null,
	dplaction int not null,
	dplnote varchar(255) not null,
	dplcreated datetime not null
) DEFAULT CHARSET=utf8;

CREATE UNIQUE INDEX idx_hm_domainpolicylist_pattern ON hm_domainpolicylist (dplpattern, dplmatchon);

ALTER TABLE hm_domainpolicylist ENGINE=InnoDB;

create table hm_journalrules
(
	jrid int auto_increment not null, primary key(`jrid`), unique(`jrid`),
	jrscope int not null,
	jrdomainid int not null,
	jraccountid int not null,
	jraddress varchar(255) not null,
	jrdirection int not null,
	jrmode int not null,
	jrenabled tinyint not null,
	jrnote varchar(255) not null
) DEFAULT CHARSET=utf8;

CREATE INDEX idx_hm_journalrules_domain ON hm_journalrules (jrdomainid);

CREATE INDEX idx_hm_journalrules_account ON hm_journalrules (jraccountid);

ALTER TABLE hm_journalrules ENGINE=InnoDB;

ALTER TABLE hm_journalrules ADD CONSTRAINT fk_hm_journalrules_domain FOREIGN KEY (jrdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE;

create table hm_accountrecovery
(
	acrid int auto_increment not null, primary key(`acrid`), unique(`acrid`),
	acraccountid int not null,
	acraddress varchar(255) not null,
	acrverified int not null,
	acrcreated bigint not null,
	acrupdated bigint not null
) DEFAULT CHARSET=utf8;

CREATE UNIQUE INDEX idx_hm_accountrecovery_account ON hm_accountrecovery (acraccountid);

ALTER TABLE hm_accountrecovery ENGINE=InnoDB;

create table hm_recoverytokens
(
	rtid bigint auto_increment not null, primary key(`rtid`), unique(`rtid`),
	rtaccountid int not null,
	rtkind int not null,
	rttoken varchar(64) not null,
	rtaddress varchar(255) not null,
	rtexpires bigint not null,
	rtused bigint not null,
	rtcreated bigint not null
) DEFAULT CHARSET=utf8;

CREATE INDEX idx_hm_recoverytokens_token ON hm_recoverytokens (rttoken);

CREATE INDEX idx_hm_recoverytokens_account ON hm_recoverytokens (rtaccountid);

ALTER TABLE hm_recoverytokens ENGINE=InnoDB;

create table hm_recoverycodes
(
	rkid bigint auto_increment not null, primary key(`rkid`), unique(`rkid`),
	rkaccountid int not null,
	rkhash varchar(255) not null,
	rkencryption int not null,
	rkused bigint not null,
	rkcreated bigint not null
) DEFAULT CHARSET=utf8;

CREATE INDEX idx_hm_recoverycodes_account ON hm_recoverycodes (rkaccountid);

ALTER TABLE hm_recoverycodes ENGINE=InnoDB;

ALTER TABLE hm_accountrecovery ADD CONSTRAINT fk_hm_accountrecovery_account FOREIGN KEY (acraccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

ALTER TABLE hm_recoverytokens ADD CONSTRAINT fk_hm_recoverytokens_account FOREIGN KEY (rtaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

ALTER TABLE hm_recoverycodes ADD CONSTRAINT fk_hm_recoverycodes_account FOREIGN KEY (rkaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

insert into hm_alertrules (alertrulecondition, alertruleenabled, alertrulethreshold, alertruleactions, alertruleaddress, alertrulewebhook, alertrulesecret, alertrulecooldown, alertruledigest) values ('account.password-reset', 1, 0, 1, '', '', '', 0, 0);

insert into hm_dbversion values (6082);
