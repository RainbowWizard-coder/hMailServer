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

ALTER TABLE hm_digeststate ADD CONSTRAINT fk_hm_digeststate_account FOREIGN KEY (dsaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

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

ALTER TABLE hm_digesttokens ADD CONSTRAINT fk_hm_digesttokens_account FOREIGN KEY (dtaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

alter table hm_domains add column domainquarantinedigest smallint not null default 0;

alter table hm_domains add column domainquarantinedigesthour int not null default 7;

insert into hm_settings (settingname, settingstring, settinginteger) select 'QuarantineDigestBaseUrl', '', 0 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'QuarantineDigestBaseUrl');

insert into hm_settings (settingname, settingstring, settinginteger) select 'QuarantineDigestLinkHours', '', 72 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'QuarantineDigestLinkHours');

update hm_dbversion set value = 6046;
