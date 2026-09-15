alter table hm_domains add column domaindkimrotationenabled int not null default 0;

alter table hm_domains add column domaindkimrotationintervaldays int not null default 180;

alter table hm_domains add column domaindkimrotationpropagationhours int not null default 72;

alter table hm_domains add column domaindkimrotationretentiondays int not null default 7;

alter table hm_domains add column domaindkimrotationkeybits int not null default 2048;

create table hm_dkimrotations
(
	dkrid bigserial not null primary key,
	dkrdomainid int not null,
	dkrcurrentselector varchar(255) not null,
	dkrcurrentkeyfile varchar(255) not null,
	dkrcurrentgenerated int not null,
	dkrcurrentsince bigint not null,
	dkrcurrentstatus int not null,
	dkrcurrentchecked bigint not null,
	dkrcurrentdetail varchar(255) not null,
	dkrnextselector varchar(255) not null,
	dkrnextkeyfile varchar(255) not null,
	dkrnextgenerated int not null,
	dkrnextcreated bigint not null,
	dkrnextfirstseen bigint not null,
	dkrnextstatus int not null,
	dkrnextchecked bigint not null,
	dkrnextdetail varchar(255) not null,
	dkrretiringselector varchar(255) not null,
	dkrretiringkeyfile varchar(255) not null,
	dkrretiringgenerated int not null,
	dkrretiringsince bigint not null,
	dkrretiringstatus int not null,
	dkrretiringchecked bigint not null,
	dkrretiringdetail varchar(255) not null,
	dkrretiredselector varchar(255) not null,
	dkrretiredat bigint not null,
	dkrretiredkeyfile varchar(255) not null,
	dkrlasterror varchar(255) not null,
	dkrlasterrortime bigint not null
);

CREATE UNIQUE INDEX idx_hm_dkimrotations_domain ON hm_dkimrotations (dkrdomainid);

ALTER TABLE hm_dkimrotations ADD CONSTRAINT fk_hm_dkimrotations_domain FOREIGN KEY (dkrdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE;

insert into hm_alertrules (alertrulecondition, alertruleenabled, alertrulethreshold, alertruleactions, alertruleaddress, alertrulewebhook, alertrulesecret, alertrulecooldown, alertruledigest) select 'dkim.rotation.stalled', 1, 7, 1, '', '', '', 1440, 1 from hm_dbversion where not exists (select alertrulecondition from hm_alertrules where alertrulecondition = 'dkim.rotation.stalled');

update hm_dbversion set value = 6050;
