create table hm_signins
(
	siid bigserial not null primary key,
	siaccountid int not null,
	sidevice varchar(32) not null,
	siprotocol int not null,
	siaddress varchar(64) not null,
	siagent varchar(255) not null,
	sicredential int not null,
	sicredentialname varchar(64) not null,
	sifirstseen timestamp not null,
	silastseen timestamp not null,
	sicount int not null,
	sirevoked smallint not null,
	sirevokedtime timestamp null
);

CREATE UNIQUE INDEX idx_hm_signins_device ON hm_signins (siaccountid, sidevice);

ALTER TABLE hm_signins ADD CONSTRAINT fk_hm_signins_account FOREIGN KEY (siaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

alter table hm_domains add column domainsessionidleseconds int not null default 0;

alter table hm_domains add column domainsessionlifetimeseconds int not null default 0;

alter table hm_domains add column domainsessiononeperdevice int not null default 0;

alter table hm_domains add column domainsessionbindaddress int not null default 0;

insert into hm_settings (settingname, settingstring, settinginteger) select 'SessionIdleSeconds', '', 0 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'SessionIdleSeconds');

insert into hm_settings (settingname, settingstring, settinginteger) select 'SessionLifetimeSeconds', '', 0 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'SessionLifetimeSeconds');

insert into hm_settings (settingname, settingstring, settinginteger) select 'SessionOnePerDevice', '', 0 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'SessionOnePerDevice');

insert into hm_settings (settingname, settingstring, settinginteger) select 'SessionBindToAddress', '', 0 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'SessionBindToAddress');

update hm_dbversion set value = 6053;
