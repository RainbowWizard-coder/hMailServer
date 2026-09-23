create table hm_settingoverrides
(
	soid bigint identity(1,1) not null,
	soscope int not null,
	soownerid bigint not null,
	soname nvarchar(30) not null,
	sovalue nvarchar(4000) not null
)

ALTER TABLE hm_settingoverrides ADD CONSTRAINT hm_settingoverrides_pk PRIMARY KEY NONCLUSTERED (soid)

CREATE UNIQUE CLUSTERED INDEX idx_hm_settingoverrides_owner ON hm_settingoverrides (soscope, soownerid, soname)

CREATE NONCLUSTERED INDEX idx_hm_settingoverrides_name ON hm_settingoverrides (soname)

update hm_dbversion set value = 6088
