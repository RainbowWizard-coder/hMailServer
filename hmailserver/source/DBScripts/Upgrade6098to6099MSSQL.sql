create table hm_accountkeys
(
	akid int identity(1,1) not null,
	akaccountid int not null,
	akenabled int not null,
	akpublic nvarchar(64) not null,
	akpassword nvarchar(512) not null,
	akrecovery nvarchar(512) not null,
	akrecoverypublic nvarchar(64) not null,
	akpending nvarchar(2000) not null,
	akcreated bigint not null,
	akchanged bigint not null
)

ALTER TABLE hm_accountkeys ADD CONSTRAINT hm_accountkeys_pk PRIMARY KEY NONCLUSTERED (akid)

CREATE UNIQUE INDEX idx_hm_accountkeys_account ON hm_accountkeys (akaccountid)

ALTER TABLE hm_accountkeys ADD CONSTRAINT fk_hm_accountkeys_account FOREIGN KEY (akaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

create table hm_domainrecovery
(
	drid int identity(1,1) not null,
	drdomainid int not null,
	drpublic nvarchar(64) not null,
	drsealed nvarchar(512) not null,
	drhint nvarchar(255) not null,
	drcreated bigint not null,
	drprevpublic nvarchar(64) not null,
	drprevsealed nvarchar(512) not null
)

ALTER TABLE hm_domainrecovery ADD CONSTRAINT hm_domainrecovery_pk PRIMARY KEY NONCLUSTERED (drid)

CREATE UNIQUE INDEX idx_hm_domainrecovery_domain ON hm_domainrecovery (drdomainid)

ALTER TABLE hm_domainrecovery ADD CONSTRAINT fk_hm_domainrecovery_domain FOREIGN KEY (drdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE

insert into hm_settings (settingname, settingstring, settinginteger) select 'MailboxKeyIdleMinutes', '', 15 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'MailboxKeyIdleMinutes')

update hm_dbversion set value = 6099
