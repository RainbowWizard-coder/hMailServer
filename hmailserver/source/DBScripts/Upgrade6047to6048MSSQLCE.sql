ALTER TABLE hm_domains ADD domaindkimrotationenabled int not null DEFAULT 0

ALTER TABLE hm_domains ADD domaindkimrotationintervaldays int not null DEFAULT 180

ALTER TABLE hm_domains ADD domaindkimrotationpropagationhours int not null DEFAULT 72

ALTER TABLE hm_domains ADD domaindkimrotationretentiondays int not null DEFAULT 7

ALTER TABLE hm_domains ADD domaindkimrotationkeybits int not null DEFAULT 2048

create table hm_dkimrotations
(
	dkrid bigint identity(1,1) not null,
	dkrdomainid int not null,
	dkrcurrentselector nvarchar(255) not null,
	dkrcurrentkeyfile nvarchar(255) not null,
	dkrcurrentgenerated int not null,
	dkrcurrentsince bigint not null,
	dkrcurrentstatus int not null,
	dkrcurrentchecked bigint not null,
	dkrcurrentdetail nvarchar(255) not null,
	dkrnextselector nvarchar(255) not null,
	dkrnextkeyfile nvarchar(255) not null,
	dkrnextgenerated int not null,
	dkrnextcreated bigint not null,
	dkrnextfirstseen bigint not null,
	dkrnextstatus int not null,
	dkrnextchecked bigint not null,
	dkrnextdetail nvarchar(255) not null,
	dkrretiringselector nvarchar(255) not null,
	dkrretiringkeyfile nvarchar(255) not null,
	dkrretiringgenerated int not null,
	dkrretiringsince bigint not null,
	dkrretiringstatus int not null,
	dkrretiringchecked bigint not null,
	dkrretiringdetail nvarchar(255) not null,
	dkrretiredselector nvarchar(255) not null,
	dkrretiredat bigint not null,
	dkrretiredkeyfile nvarchar(255) not null,
	dkrlasterror nvarchar(255) not null,
	dkrlasterrortime bigint not null
)

ALTER TABLE hm_dkimrotations ADD CONSTRAINT hm_dkimrotations_pk PRIMARY KEY (dkrid)

CREATE UNIQUE INDEX idx_hm_dkimrotations_domain ON hm_dkimrotations (dkrdomainid)

ALTER TABLE hm_dkimrotations ADD CONSTRAINT fk_hm_dkimrotations_domain FOREIGN KEY (dkrdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE

insert into hm_alertrules (alertrulecondition, alertruleenabled, alertrulethreshold, alertruleactions, alertruleaddress, alertrulewebhook, alertrulesecret, alertrulecooldown, alertruledigest) select 'dkim.rotation.stalled', 1, 7, 1, '', '', '', 1440, 1 from hm_dbversion where not exists (select alertrulecondition from hm_alertrules where alertrulecondition = 'dkim.rotation.stalled')

update hm_dbversion set value = 6048
