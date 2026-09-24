create table hm_storeblobs
(
	sbid bigserial not null primary key,
	sbsha256 varchar(64) not null,
	sbsize bigint not null,
	sbrefs int not null,
	sbcreated bigint not null
);

CREATE UNIQUE INDEX idx_hm_storeblobs_sha256 ON hm_storeblobs (sbsha256);

insert into hm_settings (settingname, settingstring, settinginteger) select 'MessageStoreCompression', '', 0 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'MessageStoreCompression');

insert into hm_settings (settingname, settingstring, settinginteger) select 'MessageStoreDedupMinKB', '', 256 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'MessageStoreDedupMinKB');

update hm_dbversion set value = 6098;
