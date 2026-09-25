create table hm_inboundhooks
(
	ihid bigserial not null primary key,
	ihaddress varchar(255) not null,
	ihurl varchar(1024) not null,
	ihsecret varchar(1024) not null,
	ihdeliver int not null,
	ihonfailure int not null,
	ihformat int not null,
	ihattachments int not null,
	ihincluderaw int not null,
	ihmaxsizekb int not null,
	ihprivatenetwork int not null,
	ihenabled int not null,
	ihcreated bigint not null
);

CREATE UNIQUE INDEX idx_hm_inboundhooks_address ON hm_inboundhooks (ihaddress);

create table hm_inboundhookqueue
(
	iqid bigserial not null primary key,
	iqhookid bigint not null,
	iqcreated bigint not null,
	iqstate int not null,
	iqtries int not null,
	iqnext bigint not null,
	iqerror varchar(255) not null,
	iqfile varchar(255) not null,
	iqqueueid bigint not null,
	iqsender varchar(255) not null,
	iqrecipients text not null,
	iqquiet text not null,
	iqsenderip varchar(64) not null,
	iqspam int not null,
	iqsize bigint not null
);

CREATE INDEX idx_hm_inboundhookqueue_due ON hm_inboundhookqueue (iqstate, iqnext);

CREATE INDEX idx_hm_inboundhookqueue_hook ON hm_inboundhookqueue (iqhookid, iqid);

ALTER TABLE hm_inboundhookqueue ADD CONSTRAINT fk_hm_inboundhookqueue_hook FOREIGN KEY (iqhookid) REFERENCES hm_inboundhooks (ihid) ON DELETE CASCADE;

update hm_roles set rolepermissions = rolepermissions || ',manage_inbound_webhooks' where rolename = 'serveradmin' and rolepermissions not like '%manage_inbound_webhooks%';

update hm_dbversion set value = 6100;
