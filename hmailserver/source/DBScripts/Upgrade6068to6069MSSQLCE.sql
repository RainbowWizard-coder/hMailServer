create table hm_confidential
(
	cfid int identity(1,1) not null,
	cfaccountid int not null,
	cftoken nvarchar(64) not null,
	cfsubject nvarchar(255) not null,
	cfsender nvarchar(255) not null,
	cfrecipients nvarchar(2000) not null,
	cffile nvarchar(255) not null,
	cfpasscode nvarchar(255) not null,
	cfcreated bigint not null,
	cfexpires bigint not null,
	cfopened bigint not null,
	cfopenedby nvarchar(255) not null,
	cfrevoked int not null
)

ALTER TABLE hm_confidential ADD CONSTRAINT hm_confidential_pk PRIMARY KEY (cfid)

CREATE UNIQUE INDEX idx_hm_confidential_token ON hm_confidential (cftoken)

CREATE INDEX idx_hm_confidential_account ON hm_confidential (cfaccountid)

ALTER TABLE hm_confidential ADD CONSTRAINT fk_hm_confidential_account FOREIGN KEY (cfaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

update hm_dbversion set value = 6069
