ALTER TABLE hm_domains ADD domainwebhooksenabled int not null CONSTRAINT df_domainwebhooksenabled DEFAULT 1

ALTER TABLE hm_domains ADD domainwebhooksubjects int not null CONSTRAINT df_domainwebhooksubjects DEFAULT 0

create table hm_webhooks
(
	whid bigint identity(1,1) not null,
	whname nvarchar(64) not null,
	whurl nvarchar(1024) not null,
	whsecret nvarchar(1024) not null,
	whevents nvarchar(255) not null,
	whdomains nvarchar(1024) not null,
	whaddresses nvarchar(1024) not null,
	whenabled int not null,
	whmaxperminute int not null,
	whincludesubject int not null,
	whcreated bigint not null
)

ALTER TABLE hm_webhooks ADD CONSTRAINT hm_webhooks_pk PRIMARY KEY NONCLUSTERED (whid)

CREATE UNIQUE CLUSTERED INDEX idx_hm_webhooks_name ON hm_webhooks (whname)

create table hm_webhookqueue
(
	wqid bigint identity(1,1) not null,
	wqwebhookid bigint not null,
	wqevent nvarchar(64) not null,
	wqcreated bigint not null,
	wqstate int not null,
	wqtries int not null,
	wqnext bigint not null,
	wqerror nvarchar(255) not null,
	wqbody ntext not null
)

ALTER TABLE hm_webhookqueue ADD CONSTRAINT hm_webhookqueue_pk PRIMARY KEY (wqid)

CREATE INDEX idx_hm_webhookqueue_due ON hm_webhookqueue (wqstate, wqnext)

CREATE INDEX idx_hm_webhookqueue_webhook ON hm_webhookqueue (wqwebhookid, wqid)

ALTER TABLE hm_webhookqueue ADD CONSTRAINT fk_hm_webhookqueue_webhook FOREIGN KEY (wqwebhookid) REFERENCES hm_webhooks (whid) ON DELETE CASCADE

insert into hm_settings (settingname, settingstring, settinginteger) select 'WebhooksEnabled', '', 1 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'WebhooksEnabled')

insert into hm_settings (settingname, settingstring, settinginteger) select 'WebhookMaxAttempts', '', 5 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'WebhookMaxAttempts')

insert into hm_settings (settingname, settingstring, settinginteger) select 'WebhookMaxPerMinute', '', 60 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'WebhookMaxPerMinute')

insert into hm_settings (settingname, settingstring, settinginteger) select 'WebhookRetentionDays', '', 7 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'WebhookRetentionDays')

update hm_dbversion set value = 6051
