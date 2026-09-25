create table hm_inboundhooks
(
	ihid bigint identity(1,1) not null,
	ihaddress nvarchar(255) not null,
	ihurl nvarchar(1024) not null,
	ihsecret nvarchar(1024) not null,
	ihdeliver int not null,
	ihonfailure int not null,
	ihformat int not null,
	ihattachments int not null,
	ihincluderaw int not null,
	ihmaxsizekb int not null,
	ihprivatenetwork int not null,
	ihenabled int not null,
	ihcreated bigint not null
)

ALTER TABLE hm_inboundhooks ADD CONSTRAINT hm_inboundhooks_pk PRIMARY KEY NONCLUSTERED (ihid)

CREATE UNIQUE CLUSTERED INDEX idx_hm_inboundhooks_address ON hm_inboundhooks (ihaddress)

create table hm_inboundhookqueue
(
	iqid bigint identity(1,1) not null,
	iqhookid bigint not null,
	iqcreated bigint not null,
	iqstate int not null,
	iqtries int not null,
	iqnext bigint not null,
	iqerror nvarchar(255) not null,
	iqfile nvarchar(255) not null,
	iqqueueid bigint not null,
	iqsender nvarchar(255) not null,
	iqrecipients ntext not null,
	iqquiet ntext not null,
	iqsenderip nvarchar(64) not null,
	iqspam int not null,
	iqsize bigint not null
)

ALTER TABLE hm_inboundhookqueue ADD CONSTRAINT hm_inboundhookqueue_pk PRIMARY KEY (iqid)

CREATE INDEX idx_hm_inboundhookqueue_due ON hm_inboundhookqueue (iqstate, iqnext)

CREATE INDEX idx_hm_inboundhookqueue_hook ON hm_inboundhookqueue (iqhookid, iqid)

ALTER TABLE hm_inboundhookqueue ADD CONSTRAINT fk_hm_inboundhookqueue_hook FOREIGN KEY (iqhookid) REFERENCES hm_inboundhooks (ihid) ON DELETE CASCADE

update hm_roles set rolepermissions = rolepermissions + ',manage_inbound_webhooks' where rolename = 'serveradmin' and rolepermissions not like '%manage_inbound_webhooks%'

update hm_dbversion set value = 6100
