create table hm_storeblobs
(
	sbid bigint identity(1,1) not null,
	sbsha256 nvarchar(64) not null,
	sbsize bigint not null,
	sbrefs int not null,
	sbcreated bigint not null
)

ALTER TABLE hm_storeblobs ADD CONSTRAINT hm_storeblobs_pk PRIMARY KEY NONCLUSTERED (sbid)

CREATE UNIQUE INDEX idx_hm_storeblobs_sha256 ON hm_storeblobs (sbsha256)

insert into hm_settings (settingname, settingstring, settinginteger) select 'MessageStoreCompression', '', 0 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'MessageStoreCompression')

insert into hm_settings (settingname, settingstring, settinginteger) select 'MessageStoreDedupMinKB', '', 256 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'MessageStoreDedupMinKB')

update hm_dbversion set value = 6098
