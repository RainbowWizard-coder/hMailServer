create table hm_apisends
(
	apsid bigserial not null primary key,
	apskeyid varchar(32) not null,
	apsqueueid bigint not null,
	apsmessageid varchar(255) not null,
	apssender varchar(255) not null,
	apsrecipients varchar(4000) not null,
	apscreated timestamp not null
);

CREATE INDEX idx_hm_apisends_key ON hm_apisends (apskeyid, apsid);

CREATE INDEX idx_hm_apisends_created ON hm_apisends (apscreated);

CREATE INDEX idx_hm_apisends_keycreated ON hm_apisends (apskeyid, apscreated);

CREATE INDEX idx_hm_apisends_queueid ON hm_apisends (apsqueueid);

CREATE INDEX idx_hm_messagetrace_queueid ON hm_messagetrace (mtqueueid);

update hm_roles set rolepermissions = rolepermissions || ',send_through_api' where rolename = 'serveradmin' and rolepermissions not like '%send_through_api%';

update hm_dbversion set value = 6089;
