create table hm_pgpring
(
	pgrid int identity(1,1) not null,
	pgraccountid int not null,
	pgrkind tinyint not null,
	pgraddress nvarchar(255) not null,
	pgrname nvarchar(255) not null,
	pgrfingerprint nvarchar(64) not null,
	pgrkeyid nvarchar(16) not null,
	pgralgorithm nvarchar(32) not null,
	pgrpublic ntext not null,
	pgrsecret ntext not null,
	pgrcreated bigint not null,
	pgrexpires bigint not null,
	pgrrevoked int not null,
	pgrsource nvarchar(16) not null,
	pgrprefer int not null,
	pgrfirstseen bigint not null,
	pgrlastseen bigint not null
)

ALTER TABLE hm_pgpring ADD CONSTRAINT hm_pgpring_pk PRIMARY KEY (pgrid)

CREATE INDEX idx_hm_pgpring_account ON hm_pgpring (pgraccountid)

CREATE UNIQUE INDEX idx_hm_pgpring_entry ON hm_pgpring (pgraccountid, pgrkind, pgrfingerprint)

ALTER TABLE hm_pgpring ADD CONSTRAINT fk_hm_pgpring_account FOREIGN KEY (pgraccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

update hm_dbversion set value = 6080
