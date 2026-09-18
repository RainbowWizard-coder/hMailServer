create table hm_accountrecovery
(
	acrid serial not null primary key,
	acraccountid int not null,
	acraddress varchar(255) not null,
	acrverified int not null,
	acrcreated bigint not null,
	acrupdated bigint not null
);

CREATE UNIQUE INDEX idx_hm_accountrecovery_account ON hm_accountrecovery (acraccountid);

create table hm_recoverytokens
(
	rtid bigserial not null primary key,
	rtaccountid int not null,
	rtkind int not null,
	rttoken varchar(64) not null,
	rtaddress varchar(255) not null,
	rtexpires bigint not null,
	rtused bigint not null,
	rtcreated bigint not null
);

CREATE INDEX idx_hm_recoverytokens_token ON hm_recoverytokens (rttoken);

CREATE INDEX idx_hm_recoverytokens_account ON hm_recoverytokens (rtaccountid);

create table hm_recoverycodes
(
	rkid bigserial not null primary key,
	rkaccountid int not null,
	rkhash varchar(255) not null,
	rkencryption int not null,
	rkused bigint not null,
	rkcreated bigint not null
);

CREATE INDEX idx_hm_recoverycodes_account ON hm_recoverycodes (rkaccountid);

ALTER TABLE hm_accountrecovery ADD CONSTRAINT fk_hm_accountrecovery_account FOREIGN KEY (acraccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

ALTER TABLE hm_recoverytokens ADD CONSTRAINT fk_hm_recoverytokens_account FOREIGN KEY (rtaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

ALTER TABLE hm_recoverycodes ADD CONSTRAINT fk_hm_recoverycodes_account FOREIGN KEY (rkaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

insert into hm_alertrules (alertrulecondition, alertruleenabled, alertrulethreshold, alertruleactions, alertruleaddress, alertrulewebhook, alertrulesecret, alertrulecooldown, alertruledigest) select 'account.password-reset', 1, 0, 1, '', '', '', 0, 0 from hm_dbversion where not exists (select alertrulecondition from hm_alertrules where alertrulecondition = 'account.password-reset');

update hm_dbversion set value = 6076;
