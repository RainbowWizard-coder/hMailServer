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

ALTER TABLE hm_senderlist ADD CONSTRAINT hm_senderlist_pk PRIMARY KEY (slid)

CREATE INDEX idx_hm_senderlist_scope ON hm_senderlist (slscope, slownerid)

create table hm_digeststate
(
	dsid int identity(1,1) not null,
	dsaccountid int not null,
	dssenttime datetime not null,
	dslastid int not null,
	dsoptout tinyint not null
)

ALTER TABLE hm_digeststate ADD CONSTRAINT hm_digeststate_pk PRIMARY KEY (dsid)

CREATE UNIQUE INDEX idx_hm_digeststate_account ON hm_digeststate (dsaccountid)

ALTER TABLE hm_digeststate ADD CONSTRAINT fk_hm_digeststate_account FOREIGN KEY (dsaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

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

ALTER TABLE hm_digesttokens ADD CONSTRAINT hm_digesttokens_pk PRIMARY KEY (dtid)

CREATE UNIQUE INDEX idx_hm_digesttokens_token ON hm_digesttokens (dttoken)

ALTER TABLE hm_digesttokens ADD CONSTRAINT fk_hm_digesttokens_account FOREIGN KEY (dtaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

ALTER TABLE hm_domains ADD domainquarantinedigest tinyint not null DEFAULT 0

ALTER TABLE hm_domains ADD domainquarantinedigesthour int not null DEFAULT 7

insert into hm_settings (settingname, settingstring, settinginteger) select 'QuarantineDigestBaseUrl', '', 0 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'QuarantineDigestBaseUrl')

insert into hm_settings (settingname, settingstring, settinginteger) select 'QuarantineDigestLinkHours', '', 72 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'QuarantineDigestLinkHours')

update hm_dbversion set value = 6046
