alter table hm_domains add column domainopenpgppublish int not null default 0;

alter table hm_domains add column domainopenpgpnewaccounts int not null default 0;

alter table hm_accounts add column accountopenpgppublish int not null default 0;

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

insert into hm_settings (settingname, settingstring, settinginteger) select 'OpenPGPWebKeyDirectoryEnabled', '', 0 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'OpenPGPWebKeyDirectoryEnabled');

insert into hm_settings (settingname, settingstring, settinginteger) select 'OpenPGPKeyMaximumBytes', '', 65536 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'OpenPGPKeyMaximumBytes');

insert into hm_settings (settingname, settingstring, settinginteger) select 'OpenPGPKeysPerAccount', '', 8 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'OpenPGPKeysPerAccount');

insert into hm_settings (settingname, settingstring, settinginteger) select 'OpenPGPRequireAddressInUserId', '', 1 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'OpenPGPRequireAddressInUserId');

update hm_dbversion set value = 6050;
