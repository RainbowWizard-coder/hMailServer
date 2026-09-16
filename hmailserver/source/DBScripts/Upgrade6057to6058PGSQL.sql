create table hm_spamtraps
(
	stid bigserial not null primary key,
	stdomainid int not null,
	stlocalpart varchar(64) not null,
	stcreated timestamp not null
);

CREATE UNIQUE INDEX idx_hm_spamtraps_address ON hm_spamtraps (stdomainid, stlocalpart);

create table hm_spamtraphits
(
	sthid bigserial not null primary key,
	sthdomainid int not null,
	sthaddress varchar(255) not null,
	sthipaddress varchar(64) not null,
	sthsender varchar(255) not null,
	sthtime timestamp not null,
	sthbanned smallint not null,
	sthblocked smallint not null,
	sthnote varchar(255) not null
);

CREATE INDEX idx_hm_spamtraphits_time ON hm_spamtraphits (sthtime);

CREATE INDEX idx_hm_spamtraphits_domain ON hm_spamtraphits (sthdomainid);

ALTER TABLE hm_spamtraps ADD CONSTRAINT fk_hm_spamtraps_domain FOREIGN KEY (stdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE;

ALTER TABLE hm_spamtraphits ADD CONSTRAINT fk_hm_spamtraphits_domain FOREIGN KEY (sthdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE;

insert into hm_settings (settingname, settingstring, settinginteger) select 'SpamTrapBanMinutes', '', 1440 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'SpamTrapBanMinutes');

insert into hm_settings (settingname, settingstring, settinginteger) select 'SpamTrapBlockDays', '', 30 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'SpamTrapBlockDays');

insert into hm_settings (settingname, settingstring, settinginteger) select 'SpamTrapHitRetentionDays', '', 90 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'SpamTrapHitRetentionDays');

insert into hm_settings (settingname, settingstring, settinginteger) select 'SpamTrapTeachSpamAssassin', '', 0 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'SpamTrapTeachSpamAssassin');

update hm_dbversion set value = 6058;
