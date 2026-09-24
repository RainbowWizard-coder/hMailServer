create table hm_storeblobs
(
	sbid bigint auto_increment not null, primary key(`sbid`), unique(`sbid`),
	sbsha256 varchar(64) not null,
	sbsize bigint not null,
	sbrefs int not null,
	sbcreated bigint not null
) DEFAULT CHARSET=utf8;

CREATE UNIQUE INDEX idx_hm_storeblobs_sha256 ON hm_storeblobs (sbsha256);

ALTER TABLE hm_storeblobs ENGINE=InnoDB;

insert into hm_settings (settingname, settingstring, settinginteger) select 'MessageStoreCompression', '', 0 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'MessageStoreCompression');

insert into hm_settings (settingname, settingstring, settinginteger) select 'MessageStoreDedupMinKB', '', 256 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'MessageStoreDedupMinKB');

update hm_dbversion set value = 6098;
