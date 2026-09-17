create table hm_recoverable
(
	rcid serial not null primary key,
	rcaccountid int not null,
	rcfolderid int not null,
	rcfolder varchar(255) not null,
	rcfile varchar(255) not null,
	rcsize int not null,
	rcsubject varchar(255) not null,
	rcfrom varchar(255) not null,
	rcflags int not null,
	rcdeleted bigint not null
);

CREATE INDEX idx_hm_recoverable_account ON hm_recoverable (rcaccountid);

ALTER TABLE hm_recoverable ADD CONSTRAINT fk_hm_recoverable_account FOREIGN KEY (rcaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

update hm_dbversion set value = 6068;
