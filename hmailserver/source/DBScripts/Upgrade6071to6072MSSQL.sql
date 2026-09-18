ALTER TABLE hm_greylisting_triplets DROP CONSTRAINT u_gltriplet --- [IGNORE-ERRORS]

DROP INDEX idx_hm_greylisting_triplets ON hm_greylisting_triplets --- [IGNORE-ERRORS]

ALTER TABLE hm_greylisting_triplets ALTER COLUMN glsenderaddress nvarchar(320) NOT NULL

ALTER TABLE hm_greylisting_triplets ALTER COLUMN glrecipientaddress nvarchar(320) NOT NULL

ALTER TABLE hm_greylisting_triplets ADD glsenderhash nvarchar(32) NULL

ALTER TABLE hm_greylisting_triplets ADD glrecipienthash nvarchar(32) NULL

CREATE INDEX idx_hm_greylisting_triplets_key ON hm_greylisting_triplets (glipaddress1, glipaddress2, glsenderhash, glrecipienthash)

create table hm_greylisting_bypass
(
	glbid bigint identity(1,1) not null,
	glbpattern nvarchar(255) not null,
	glbmatch int not null,
	glbrequire int not null,
	glbnote nvarchar(255) not null
)

ALTER TABLE hm_greylisting_bypass ADD CONSTRAINT hm_greylisting_bypass_pk PRIMARY KEY (glbid)

insert into hm_settings (settingname, settingstring, settinginteger) select 'GreyListingSlideOnEarlyRetry', '', 0 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'GreyListingSlideOnEarlyRetry')

insert into hm_settings (settingname, settingstring, settinginteger) select 'GreyListingAbuseThreshold', '', 0 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'GreyListingAbuseThreshold')

insert into hm_settings (settingname, settingstring, settinginteger) select 'GreyListingAbuseLockMinutes', '', 60 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'GreyListingAbuseLockMinutes')

update hm_dbversion set value = 6072
