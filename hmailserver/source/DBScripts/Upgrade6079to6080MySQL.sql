create table hm_pgpring
(
	pgrid int auto_increment not null, primary key(`pgrid`), unique(`pgrid`),
	pgraccountid int not null,
	pgrkind tinyint not null,
	pgraddress varchar(255) not null,
	pgrname varchar(255) not null,
	pgrfingerprint varchar(64) not null,
	pgrkeyid varchar(16) not null,
	pgralgorithm varchar(32) not null,
	pgrpublic mediumtext not null,
	pgrsecret mediumtext not null,
	pgrcreated bigint not null,
	pgrexpires bigint not null,
	pgrrevoked int not null,
	pgrsource varchar(16) not null,
	pgrprefer int not null,
	pgrfirstseen bigint not null,
	pgrlastseen bigint not null
) DEFAULT CHARSET=utf8;

CREATE INDEX idx_hm_pgpring_account ON hm_pgpring (pgraccountid);

CREATE UNIQUE INDEX idx_hm_pgpring_entry ON hm_pgpring (pgraccountid, pgrkind, pgrfingerprint);

ALTER TABLE hm_pgpring ENGINE=InnoDB;

ALTER TABLE hm_pgpring ADD CONSTRAINT fk_hm_pgpring_account FOREIGN KEY (pgraccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

update hm_dbversion set value = 6080;
