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

ALTER TABLE hm_digeststate ADD CONSTRAINT fk_hm_digeststate_account FOREIGN KEY (dsaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

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

ALTER TABLE hm_digesttokens ADD CONSTRAINT fk_hm_digesttokens_account FOREIGN KEY (dtaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

alter table hm_domains add column domainquarantinedigest tinyint not null default 0;

alter table hm_domains add column domainquarantinedigesthour int not null default 7;

insert into hm_settings (settingname, settingstring, settinginteger) select 'QuarantineDigestBaseUrl', '', 0 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'QuarantineDigestBaseUrl');

insert into hm_settings (settingname, settingstring, settinginteger) select 'QuarantineDigestLinkHours', '', 72 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'QuarantineDigestLinkHours');

update hm_dbversion set value = 6046;
