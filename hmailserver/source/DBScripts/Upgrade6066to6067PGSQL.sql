create table hm_maskedaddresses
(
	maid serial not null primary key,
	maaccountid int not null,
	maaddress varchar(255) not null,
	malabel varchar(255) not null,
	maactive smallint not null,
	mareceived int not null,
	macreated timestamp not null
);

CREATE UNIQUE INDEX idx_hm_maskedaddresses_address ON hm_maskedaddresses (maaddress);

CREATE INDEX idx_hm_maskedaddresses_account ON hm_maskedaddresses (maaccountid);

ALTER TABLE hm_maskedaddresses ADD CONSTRAINT fk_hm_maskedaddresses_account FOREIGN KEY (maaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

update hm_dbversion set value = 6067;
