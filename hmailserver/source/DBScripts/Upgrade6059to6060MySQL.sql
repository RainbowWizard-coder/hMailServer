alter table hm_accounts add column accountvacationdays int not null default 0;

alter table hm_accounts add column accountvacationinternalsubject varchar(200) not null default '';

alter table hm_accounts add column accountvacationinternalmessage varchar(1000) not null default '';

alter table hm_accounts add column accountvacationinternalonly tinyint not null default 0;

alter table hm_domains add column domainvacationdays int not null default 0;

create table hm_autoreplies
(
	arid bigint auto_increment not null, primary key(`arid`), unique(`arid`),
	araccountid int not null,
	arsender varchar(255) character set utf8 collate utf8_bin not null,
	arsent bigint not null
) DEFAULT CHARSET=utf8;

CREATE UNIQUE INDEX idx_hm_autoreplies_account ON hm_autoreplies (araccountid, arsender);

ALTER TABLE hm_autoreplies ENGINE=InnoDB;

ALTER TABLE hm_autoreplies ADD CONSTRAINT fk_hm_autoreplies_account FOREIGN KEY (araccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

update hm_dbversion set value = 6060;
