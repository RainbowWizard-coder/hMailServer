create table hm_signins
(
	siid bigint identity(1,1) not null,
	siaccountid int not null,
	sidevice nvarchar(32) not null,
	siprotocol int not null,
	siaddress nvarchar(64) not null,
	siagent nvarchar(255) not null,
	sicredential int not null,
	sicredentialname nvarchar(64) not null,
	sifirstseen datetime not null,
	silastseen datetime not null,
	sicount int not null,
	sirevoked tinyint not null,
	sirevokedtime datetime null
)

ALTER TABLE hm_signins ADD CONSTRAINT hm_signins_pk PRIMARY KEY (siid)

CREATE UNIQUE INDEX idx_hm_signins_device ON hm_signins (siaccountid, sidevice)

ALTER TABLE hm_signins ADD CONSTRAINT fk_hm_signins_account FOREIGN KEY (siaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

ALTER TABLE hm_domains ADD domainsessionidleseconds int not null DEFAULT 0

ALTER TABLE hm_domains ADD domainsessionlifetimeseconds int not null DEFAULT 0

ALTER TABLE hm_domains ADD domainsessiononeperdevice int not null DEFAULT 0

ALTER TABLE hm_domains ADD domainsessionbindaddress int not null DEFAULT 0

insert into hm_settings (settingname, settingstring, settinginteger) select 'SessionIdleSeconds', '', 0 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'SessionIdleSeconds')

insert into hm_settings (settingname, settingstring, settinginteger) select 'SessionLifetimeSeconds', '', 0 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'SessionLifetimeSeconds')

insert into hm_settings (settingname, settingstring, settinginteger) select 'SessionOnePerDevice', '', 0 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'SessionOnePerDevice')

insert into hm_settings (settingname, settingstring, settinginteger) select 'SessionBindToAddress', '', 0 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'SessionBindToAddress')

update hm_dbversion set value = 6053
