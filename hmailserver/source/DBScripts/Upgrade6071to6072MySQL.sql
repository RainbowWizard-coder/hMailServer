DROP INDEX idx_greylisting_triplets ON hm_greylisting_triplets /* [IGNORE-ERRORS] */;

DROP INDEX idx_hm_greylisting_triplets ON hm_greylisting_triplets /* [IGNORE-ERRORS] */;

alter table hm_greylisting_triplets modify glsenderaddress varchar(320) not null;

alter table hm_greylisting_triplets modify glrecipientaddress varchar(320) not null;

alter table hm_greylisting_triplets add column glsenderhash varchar(32) null;

alter table hm_greylisting_triplets add column glrecipienthash varchar(32) null;

CREATE INDEX idx_hm_greylisting_triplets_key ON hm_greylisting_triplets (glipaddress1, glipaddress2, glsenderhash, glrecipienthash);

create table hm_greylisting_bypass
(
	glbid bigint auto_increment not null, primary key(`glbid`), unique(`glbid`),
	glbpattern varchar(255) not null,
	glbmatch int not null,
	glbrequire int not null,
	glbnote varchar(255) not null
) DEFAULT CHARSET=utf8;

ALTER TABLE hm_greylisting_bypass ENGINE=InnoDB;

insert into hm_settings (settingname, settingstring, settinginteger) select 'GreyListingSlideOnEarlyRetry', '', 0 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'GreyListingSlideOnEarlyRetry');

insert into hm_settings (settingname, settingstring, settinginteger) select 'GreyListingAbuseThreshold', '', 0 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'GreyListingAbuseThreshold');

insert into hm_settings (settingname, settingstring, settinginteger) select 'GreyListingAbuseLockMinutes', '', 60 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'GreyListingAbuseLockMinutes');

update hm_dbversion set value = 6072;
