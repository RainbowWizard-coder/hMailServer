create table hm_spamtraps
(
	stid int auto_increment not null, primary key(`stid`), unique(`stid`),
	stdomainid int not null,
	stlocalpart varchar(64) not null,
	stcreated datetime not null
) DEFAULT CHARSET=utf8;

CREATE UNIQUE INDEX idx_hm_spamtraps_address ON hm_spamtraps (stdomainid, stlocalpart);

ALTER TABLE hm_spamtraps ENGINE=InnoDB;

create table hm_spamtraphits
(
	sthid int auto_increment not null, primary key(`sthid`), unique(`sthid`),
	sthdomainid int not null,
	sthaddress varchar(255) not null,
	sthipaddress varchar(64) not null,
	sthsender varchar(255) not null,
	sthtime datetime not null,
	sthbanned tinyint not null,
	sthblocked tinyint not null,
	sthnote varchar(255) not null
) DEFAULT CHARSET=utf8;

CREATE INDEX idx_hm_spamtraphits_time ON hm_spamtraphits (sthtime);

CREATE INDEX idx_hm_spamtraphits_domain ON hm_spamtraphits (sthdomainid);

ALTER TABLE hm_spamtraps ADD CONSTRAINT fk_hm_spamtraps_domain FOREIGN KEY (stdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE;

ALTER TABLE hm_spamtraphits ADD CONSTRAINT fk_hm_spamtraphits_domain FOREIGN KEY (sthdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE;

ALTER TABLE hm_spamtraphits ENGINE=InnoDB;

insert into hm_settings (settingname, settingstring, settinginteger) select 'SpamTrapBanMinutes', '', 1440 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'SpamTrapBanMinutes');

insert into hm_settings (settingname, settingstring, settinginteger) select 'SpamTrapBlockDays', '', 30 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'SpamTrapBlockDays');

insert into hm_settings (settingname, settingstring, settinginteger) select 'SpamTrapHitRetentionDays', '', 90 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'SpamTrapHitRetentionDays');

insert into hm_settings (settingname, settingstring, settinginteger) select 'SpamTrapTeachSpamAssassin', '', 0 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'SpamTrapTeachSpamAssassin');

update hm_dbversion set value = 6058;
