create table hm_accountactivity
(
	acid bigint identity(1,1) not null,
	acaccountid int not null,
	actime bigint not null,
	acbucket bigint not null,
	achour int not null,
	acrecipients int not null,
	acnewrecipients int not null,
	acaddress nvarchar(64) not null,
	acsource int not null,
	acmessageid bigint not null,
	acheld int not null
)

ALTER TABLE hm_accountactivity ADD CONSTRAINT hm_accountactivity_pk PRIMARY KEY (acid)

CREATE INDEX idx_hm_accountactivity_account ON hm_accountactivity (acaccountid, actime)

create table hm_accountrecipients
(
	arcid bigint identity(1,1) not null,
	arcaccountid int not null,
	arcaddress nvarchar(255) not null,
	arcfirst bigint not null,
	arclast bigint not null,
	arccount int not null
)

ALTER TABLE hm_accountrecipients ADD CONSTRAINT hm_accountrecipients_pk PRIMARY KEY (arcid)

CREATE INDEX idx_hm_accountrecipients_address ON hm_accountrecipients (arcaccountid, arcaddress)

create table hm_accountreview
(
	arvid int identity(1,1) not null,
	arvaccountid int not null,
	arvexempt int not null,
	arvsensitivity int not null,
	arvlocked int not null,
	arvlockedat bigint not null,
	arvunlockedat bigint not null,
	arvalertedat bigint not null,
	arvscore int not null,
	arvsignals nvarchar(255) not null,
	arvreason nvarchar(255) not null
)

ALTER TABLE hm_accountreview ADD CONSTRAINT hm_accountreview_pk PRIMARY KEY (arvid)

CREATE UNIQUE INDEX idx_hm_accountreview_account ON hm_accountreview (arvaccountid)

create table hm_heldmessages
(
	hmid bigint identity(1,1) not null,
	hmaccountid int not null,
	hmmessageid bigint not null,
	hmheld bigint not null,
	hmsource int not null,
	hmsender nvarchar(255) not null,
	hmrecipients nvarchar(2000) not null,
	hmsubject nvarchar(255) not null,
	hmsize bigint not null,
	hmreason nvarchar(255) not null
)

ALTER TABLE hm_heldmessages ADD CONSTRAINT hm_heldmessages_pk PRIMARY KEY (hmid)

CREATE INDEX idx_hm_heldmessages_account ON hm_heldmessages (hmaccountid)

CREATE INDEX idx_hm_heldmessages_message ON hm_heldmessages (hmmessageid)

ALTER TABLE hm_accountactivity ADD CONSTRAINT fk_hm_accountactivity_account FOREIGN KEY (acaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

ALTER TABLE hm_accountrecipients ADD CONSTRAINT fk_hm_accountrecipients_account FOREIGN KEY (arcaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

ALTER TABLE hm_accountreview ADD CONSTRAINT fk_hm_accountreview_account FOREIGN KEY (arvaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

ALTER TABLE hm_heldmessages ADD CONSTRAINT fk_hm_heldmessages_account FOREIGN KEY (hmaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

insert into hm_alertrules (alertrulecondition, alertruleenabled, alertrulethreshold, alertruleactions, alertruleaddress, alertrulewebhook, alertrulesecret, alertrulecooldown, alertruledigest) select 'account.compromised', 1, 0, 1, '', '', '', 0, 0 from hm_dbversion where not exists (select alertrulecondition from hm_alertrules where alertrulecondition = 'account.compromised')

update hm_dbversion set value = 6071
