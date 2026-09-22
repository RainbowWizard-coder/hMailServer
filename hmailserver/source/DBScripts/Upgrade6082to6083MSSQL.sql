ALTER TABLE hm_accounts ADD accountzeroaccess int not null CONSTRAINT df_hm_accounts_zeroaccess DEFAULT 0

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

insert into hm_settings (settingname, settingstring, settinginteger) select 'ZeroAccessAllowed', '', 0 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'ZeroAccessAllowed')

update hm_dbversion set value = 6083
