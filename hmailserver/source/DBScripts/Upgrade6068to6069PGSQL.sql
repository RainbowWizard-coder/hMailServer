create table hm_confidential
(
	cfid serial not null primary key,
	cfaccountid int not null,
	cftoken varchar(64) not null,
	cfsubject varchar(255) not null,
	cfsender varchar(255) not null,
	cfrecipients varchar(2000) not null,
	cffile varchar(255) not null,
	cfpasscode varchar(255) not null,
	cfcreated bigint not null,
	cfexpires bigint not null,
	cfopened bigint not null,
	cfopenedby varchar(255) not null,
	cfrevoked int not null
);

CREATE UNIQUE INDEX idx_hm_confidential_token ON hm_confidential (cftoken);

CREATE INDEX idx_hm_confidential_account ON hm_confidential (cfaccountid);

ALTER TABLE hm_confidential ADD CONSTRAINT fk_hm_confidential_account FOREIGN KEY (cfaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

update hm_dbversion set value = 6069;
