create table hm_sandboxverdicts
(
	svid bigint auto_increment not null, primary key(`svid`), unique(`svid`),
	svsha256 varchar(64) not null,
	svverdict varchar(16) not null,
	svscore int not null,
	svdetail varchar(255) not null,
	svanswered bigint not null,
	svexpires bigint not null
) DEFAULT CHARSET=utf8;

CREATE UNIQUE INDEX idx_hm_sandboxverdicts_sha256 ON hm_sandboxverdicts (svsha256);

create table hm_sandboxholds
(
	shid bigint auto_increment not null, primary key(`shid`), unique(`shid`),
	shmessageid bigint not null,
	shsha256 varchar(64) not null,
	shfilename varchar(255) not null,
	shsize bigint not null,
	shsubmission varchar(1000) not null,
	shheld bigint not null,
	shstate int not null,
	shverdict varchar(16) not null,
	shscore int not null,
	shdetail varchar(255) not null,
	sherrors int not null,
	shdecision int not null
) DEFAULT CHARSET=utf8;

CREATE INDEX idx_hm_sandboxholds_message ON hm_sandboxholds (shmessageid);

ALTER TABLE hm_sandboxverdicts ENGINE=InnoDB;

ALTER TABLE hm_sandboxholds ENGINE=InnoDB;

insert into hm_settings (settingname, settingstring, settinginteger) select 'SandboxEnabled', '', 0 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'SandboxEnabled');

insert into hm_settings (settingname, settingstring, settinginteger) select 'SandboxSubmitUrl', '', 0 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'SandboxSubmitUrl');

insert into hm_settings (settingname, settingstring, settinginteger) select 'SandboxVerdictUrl', '', 0 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'SandboxVerdictUrl');

insert into hm_settings (settingname, settingstring, settinginteger) select 'SandboxToken', '', 0 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'SandboxToken');

insert into hm_settings (settingname, settingstring, settinginteger) select 'SandboxTimeoutMinutes', '', 15 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'SandboxTimeoutMinutes');

insert into hm_settings (settingname, settingstring, settinginteger) select 'SandboxTimeoutAction', '', 0 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'SandboxTimeoutAction');

insert into hm_settings (settingname, settingstring, settinginteger) select 'SandboxMaxSizeKB', '', 20480 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'SandboxMaxSizeKB');

insert into hm_settings (settingname, settingstring, settinginteger) select 'SandboxTypes', 'exe,dll,scr,js,jse,vbs,wsf,hta,msi,bat,cmd,ps1,jar,lnk,iso,img,docm,xlsm,pptm,doc,xls,ppt,rtf,pdf,zip,7z,rar', 0 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'SandboxTypes');

insert into hm_settings (settingname, settingstring, settinginteger) select 'SandboxPollSeconds', '', 30 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'SandboxPollSeconds');

insert into hm_settings (settingname, settingstring, settinginteger) select 'SandboxScoreThreshold', '', 70 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'SandboxScoreThreshold');

insert into hm_settings (settingname, settingstring, settinginteger) select 'SandboxCacheHours', '', 24 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'SandboxCacheHours');

insert into hm_settings (settingname, settingstring, settinginteger) select 'SandboxInternalMail', '', 0 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'SandboxInternalMail');

insert into hm_settings (settingname, settingstring, settinginteger) select 'SandboxMaxFiles', '', 10 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'SandboxMaxFiles');

update hm_dbversion set value = 6091;
