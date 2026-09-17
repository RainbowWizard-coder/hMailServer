create table hm_recoverable
(
	rcid int identity(1,1) not null,
	rcaccountid int not null,
	rcfolderid int not null,
	rcfolder nvarchar(255) not null,
	rcfile nvarchar(255) not null,
	rcsize int not null,
	rcsubject nvarchar(255) not null,
	rcfrom nvarchar(255) not null,
	rcflags int not null,
	rcdeleted bigint not null
)

ALTER TABLE hm_recoverable ADD CONSTRAINT hm_recoverable_pk PRIMARY KEY (rcid)

CREATE INDEX idx_hm_recoverable_account ON hm_recoverable (rcaccountid)

ALTER TABLE hm_recoverable ADD CONSTRAINT fk_hm_recoverable_account FOREIGN KEY (rcaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

update hm_dbversion set value = 6068
