create table hm_accountkeys
(
	akid int auto_increment not null, primary key(`akid`), unique(`akid`),
	akaccountid int not null,
	akenabled int not null,
	akpublic varchar(64) not null,
	akpassword varchar(512) not null,
	akrecovery varchar(512) not null,
	akrecoverypublic varchar(64) not null,
	akpending varchar(2000) not null,
	akcreated bigint not null,
	akchanged bigint not null
) DEFAULT CHARSET=utf8;

CREATE UNIQUE INDEX idx_hm_accountkeys_account ON hm_accountkeys (akaccountid);

ALTER TABLE hm_accountkeys ENGINE=InnoDB;

ALTER TABLE hm_accountkeys ADD CONSTRAINT fk_hm_accountkeys_account FOREIGN KEY (akaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

create table hm_domainrecovery
(
	drid int auto_increment not null, primary key(`drid`), unique(`drid`),
	drdomainid int not null,
	drpublic varchar(64) not null,
	drsealed varchar(512) not null,
	drhint varchar(255) not null,
	drcreated bigint not null,
	drprevpublic varchar(64) not null,
	drprevsealed varchar(512) not null
) DEFAULT CHARSET=utf8;

CREATE UNIQUE INDEX idx_hm_domainrecovery_domain ON hm_domainrecovery (drdomainid);

ALTER TABLE hm_domainrecovery ENGINE=InnoDB;

ALTER TABLE hm_domainrecovery ADD CONSTRAINT fk_hm_domainrecovery_domain FOREIGN KEY (drdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE;

insert into hm_settings (settingname, settingstring, settinginteger) select 'MailboxKeyIdleMinutes', '', 15 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'MailboxKeyIdleMinutes');

update hm_dbversion set value = 6099;
