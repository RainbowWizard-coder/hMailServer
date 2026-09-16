alter table hm_domains add column domainwebhooksenabled int not null default 1;

alter table hm_domains add column domainwebhooksubjects int not null default 0;

create table hm_webhooks
(
	whid bigint auto_increment not null, primary key(`whid`), unique(`whid`),
	whname varchar(64) not null,
	whurl varchar(1024) not null,
	whsecret varchar(1024) not null,
	whevents varchar(255) not null,
	whdomains varchar(1024) not null,
	whaddresses varchar(1024) not null,
	whenabled int not null,
	whmaxperminute int not null,
	whincludesubject int not null,
	whcreated bigint not null
);

CREATE UNIQUE INDEX idx_hm_webhooks_name ON hm_webhooks (whname);

create table hm_webhookqueue
(
	wqid bigint auto_increment not null, primary key(`wqid`), unique(`wqid`),
	wqwebhookid bigint not null,
	wqevent varchar(64) not null,
	wqcreated bigint not null,
	wqstate int not null,
	wqtries int not null,
	wqnext bigint not null,
	wqerror varchar(255) not null,
	wqbody text not null
);

CREATE INDEX idx_hm_webhookqueue_due ON hm_webhookqueue (wqstate, wqnext);

CREATE INDEX idx_hm_webhookqueue_webhook ON hm_webhookqueue (wqwebhookid, wqid);

ALTER TABLE hm_webhooks ENGINE=InnoDB;

ALTER TABLE hm_webhookqueue ENGINE=InnoDB;

ALTER TABLE hm_webhookqueue ADD CONSTRAINT fk_hm_webhookqueue_webhook FOREIGN KEY (wqwebhookid) REFERENCES hm_webhooks (whid) ON DELETE CASCADE;

insert into hm_settings (settingname, settingstring, settinginteger) select 'WebhooksEnabled', '', 1 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'WebhooksEnabled');

insert into hm_settings (settingname, settingstring, settinginteger) select 'WebhookMaxAttempts', '', 5 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'WebhookMaxAttempts');

insert into hm_settings (settingname, settingstring, settinginteger) select 'WebhookMaxPerMinute', '', 60 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'WebhookMaxPerMinute');

insert into hm_settings (settingname, settingstring, settinginteger) select 'WebhookRetentionDays', '', 7 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'WebhookRetentionDays');

update hm_dbversion set value = 6051;
