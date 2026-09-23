create table hm_settingoverrides
(
	soid bigint auto_increment not null, primary key(`soid`), unique(`soid`),
	soscope int not null,
	soownerid bigint not null,
	soname varchar(30) not null,
	sovalue varchar(4000) not null
) DEFAULT CHARSET=utf8;

CREATE UNIQUE INDEX idx_hm_settingoverrides_owner ON hm_settingoverrides (soscope, soownerid, soname);

CREATE INDEX idx_hm_settingoverrides_name ON hm_settingoverrides (soname);

ALTER TABLE hm_settingoverrides ENGINE=InnoDB;

update hm_dbversion set value = 6088;
