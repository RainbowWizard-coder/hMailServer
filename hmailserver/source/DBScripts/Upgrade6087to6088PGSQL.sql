create table hm_settingoverrides
(
	soid bigserial not null primary key,
	soscope int not null,
	soownerid bigint not null,
	soname varchar(30) not null,
	sovalue varchar(4000) not null
);

CREATE UNIQUE INDEX idx_hm_settingoverrides_owner ON hm_settingoverrides (soscope, soownerid, soname);

update hm_dbversion set value = 6088;
