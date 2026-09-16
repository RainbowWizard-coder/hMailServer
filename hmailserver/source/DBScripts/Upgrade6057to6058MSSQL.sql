create table hm_spamtraps
(
	stid int identity(1,1) not null,
	stdomainid int not null,
	stlocalpart nvarchar(64) not null,
	stcreated datetime not null
)

ALTER TABLE hm_spamtraps ADD CONSTRAINT hm_spamtraps_pk PRIMARY KEY NONCLUSTERED (stid)

CREATE UNIQUE CLUSTERED INDEX idx_hm_spamtraps_address ON hm_spamtraps (stdomainid, stlocalpart)

create table hm_spamtraphits
(
	sthid int identity(1,1) not null,
	sthdomainid int not null,
	sthaddress nvarchar(255) not null,
	sthipaddress nvarchar(64) not null,
	sthsender nvarchar(255) not null,
	sthtime datetime not null,
	sthbanned tinyint not null,
	sthblocked tinyint not null,
	sthnote nvarchar(255) not null
)

ALTER TABLE hm_spamtraphits ADD CONSTRAINT hm_spamtraphits_pk PRIMARY KEY NONCLUSTERED (sthid)

CREATE CLUSTERED INDEX idx_hm_spamtraphits_time ON hm_spamtraphits (sthtime)

CREATE INDEX idx_hm_spamtraphits_domain ON hm_spamtraphits (sthdomainid)

ALTER TABLE hm_spamtraps ADD CONSTRAINT fk_hm_spamtraps_domain FOREIGN KEY (stdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE

ALTER TABLE hm_spamtraphits ADD CONSTRAINT fk_hm_spamtraphits_domain FOREIGN KEY (sthdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE

insert into hm_settings (settingname, settingstring, settinginteger) select 'SpamTrapBanMinutes', '', 1440 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'SpamTrapBanMinutes')

insert into hm_settings (settingname, settingstring, settinginteger) select 'SpamTrapBlockDays', '', 30 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'SpamTrapBlockDays')

insert into hm_settings (settingname, settingstring, settinginteger) select 'SpamTrapHitRetentionDays', '', 90 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'SpamTrapHitRetentionDays')

insert into hm_settings (settingname, settingstring, settinginteger) select 'SpamTrapTeachSpamAssassin', '', 0 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'SpamTrapTeachSpamAssassin')

update hm_dbversion set value = 6058
