drop index if exists idx_greylisting_triplets;

drop index if exists idx_hm_greylisting_triplets;

alter table hm_greylisting_triplets alter column glsenderaddress type varchar(320);

alter table hm_greylisting_triplets alter column glrecipientaddress type varchar(320);

alter table hm_greylisting_triplets add column glsenderhash varchar(32) null;

alter table hm_greylisting_triplets add column glrecipienthash varchar(32) null;

CREATE INDEX idx_hm_greylisting_triplets_key ON hm_greylisting_triplets (glipaddress1, glipaddress2, glsenderhash, glrecipienthash);

create table hm_greylisting_bypass
(
	glbid bigserial not null primary key,
	glbpattern varchar(255) not null,
	glbmatch int not null,
	glbrequire int not null,
	glbnote varchar(255) not null
);

insert into hm_settings (settingname, settingstring, settinginteger) select 'GreyListingSlideOnEarlyRetry', '', 0 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'GreyListingSlideOnEarlyRetry');

insert into hm_settings (settingname, settingstring, settinginteger) select 'GreyListingAbuseThreshold', '', 0 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'GreyListingAbuseThreshold');

insert into hm_settings (settingname, settingstring, settinginteger) select 'GreyListingAbuseLockMinutes', '', 60 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'GreyListingAbuseLockMinutes');

update hm_dbversion set value = 6072;
