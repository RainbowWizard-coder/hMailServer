create table hm_apisends
(
	apsid bigint identity(1,1) not null,
	apskeyid nvarchar(32) not null,
	apsqueueid bigint not null,
	apsmessageid nvarchar(255) not null,
	apssender nvarchar(255) not null,
	apsrecipients nvarchar(4000) not null,
	apscreated datetime not null
)

ALTER TABLE hm_apisends ADD CONSTRAINT hm_apisends_pk PRIMARY KEY NONCLUSTERED (apsid)

CREATE CLUSTERED INDEX idx_hm_apisends_key ON hm_apisends (apskeyid, apsid)

CREATE NONCLUSTERED INDEX idx_hm_apisends_created ON hm_apisends (apscreated)

CREATE NONCLUSTERED INDEX idx_hm_apisends_keycreated ON hm_apisends (apskeyid, apscreated)

CREATE NONCLUSTERED INDEX idx_hm_apisends_queueid ON hm_apisends (apsqueueid)

CREATE NONCLUSTERED INDEX idx_hm_messagetrace_queueid ON hm_messagetrace (mtqueueid)

update hm_roles set rolepermissions = rolepermissions + ',send_through_api' where rolename = 'serveradmin' and rolepermissions not like '%send_through_api%'

update hm_dbversion set value = 6089
