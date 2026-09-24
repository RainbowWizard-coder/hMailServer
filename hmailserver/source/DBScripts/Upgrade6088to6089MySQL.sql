create table hm_apisends
(
	apsid bigint auto_increment not null, primary key(`apsid`), unique(`apsid`),
	apskeyid varchar(32) not null,
	apsqueueid bigint not null,
	apsmessageid varchar(255) not null,
	apssender varchar(255) not null,
	apsrecipients varchar(4000) not null,
	apscreated datetime not null
) DEFAULT CHARSET=utf8;

CREATE INDEX idx_hm_apisends_key ON hm_apisends (apskeyid, apsid);

CREATE INDEX idx_hm_apisends_created ON hm_apisends (apscreated);

ALTER TABLE hm_apisends ENGINE=InnoDB;

update hm_roles set rolepermissions = concat(rolepermissions, ',send_through_api') where rolename = 'serveradmin' and rolepermissions not like '%send_through_api%';

update hm_dbversion set value = 6089;
