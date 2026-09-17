ALTER TABLE hm_accounts ADD accountvacationdays int not null CONSTRAINT df_accountvacationdays DEFAULT 0

ALTER TABLE hm_accounts ADD accountvacationinternalsubject nvarchar(200) not null CONSTRAINT df_accountvacationinternalsubject DEFAULT ''

ALTER TABLE hm_accounts ADD accountvacationinternalmessage nvarchar(1000) not null CONSTRAINT df_accountvacationinternalmessage DEFAULT ''

ALTER TABLE hm_accounts ADD accountvacationinternalonly tinyint not null CONSTRAINT df_accountvacationinternalonly DEFAULT 0

ALTER TABLE hm_domains ADD domainvacationdays int not null CONSTRAINT df_domainvacationdays DEFAULT 0

create table hm_autoreplies
(
	arid bigint identity(1,1) not null,
	araccountid int not null,
	arsender nvarchar(255) not null,
	arsent bigint not null
)

ALTER TABLE hm_autoreplies ADD CONSTRAINT hm_autoreplies_pk PRIMARY KEY NONCLUSTERED (arid)

CREATE UNIQUE CLUSTERED INDEX idx_hm_autoreplies_account ON hm_autoreplies (araccountid, arsender)

ALTER TABLE hm_autoreplies ADD CONSTRAINT fk_hm_autoreplies_account FOREIGN KEY (araccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

update hm_dbversion set value = 6060
