alter table hm_accounts add column accountvacationdays int not null default 0;

alter table hm_accounts add column accountvacationinternalsubject varchar(200) not null default '';

alter table hm_accounts add column accountvacationinternalmessage varchar(1000) not null default '';

alter table hm_accounts add column accountvacationinternalonly smallint not null default 0;

alter table hm_domains add column domainvacationdays int not null default 0;

create table hm_autoreplies
(
	arid bigserial not null primary key,
	araccountid int not null,
	arsender varchar(255) not null,
	arsent bigint not null
);

CREATE UNIQUE INDEX idx_hm_autoreplies_account ON hm_autoreplies (araccountid, arsender);

ALTER TABLE hm_autoreplies ADD CONSTRAINT fk_hm_autoreplies_account FOREIGN KEY (araccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

update hm_dbversion set value = 6060;
