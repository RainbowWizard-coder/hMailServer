ALTER TABLE hm_domains ADD domainopenpgppublish int not null CONSTRAINT df_domainopenpgppublish DEFAULT 0

ALTER TABLE hm_domains ADD domainopenpgpnewaccounts int not null CONSTRAINT df_domainopenpgpnewaccounts DEFAULT 0

ALTER TABLE hm_accounts ADD accountopenpgppublish int not null CONSTRAINT df_accountopenpgppublish DEFAULT 0

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

insert into hm_settings (settingname, settingstring, settinginteger) select 'OpenPGPWebKeyDirectoryEnabled', '', 0 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'OpenPGPWebKeyDirectoryEnabled')

insert into hm_settings (settingname, settingstring, settinginteger) select 'OpenPGPKeyMaximumBytes', '', 65536 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'OpenPGPKeyMaximumBytes')

insert into hm_settings (settingname, settingstring, settinginteger) select 'OpenPGPKeysPerAccount', '', 8 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'OpenPGPKeysPerAccount')

insert into hm_settings (settingname, settingstring, settinginteger) select 'OpenPGPRequireAddressInUserId', '', 1 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'OpenPGPRequireAddressInUserId')

update hm_dbversion set value = 6050
