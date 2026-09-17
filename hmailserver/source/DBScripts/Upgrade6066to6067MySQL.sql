create table hm_maskedaddresses
(
	maid int auto_increment not null, primary key(`maid`), unique(`maid`),
	maaccountid int not null,
	maaddress varchar(255) not null,
	malabel varchar(255) not null,
	maactive tinyint not null,
	mareceived int not null,
	macreated datetime not null
) DEFAULT CHARSET=utf8;

CREATE UNIQUE INDEX idx_hm_maskedaddresses_address ON hm_maskedaddresses (maaddress);

CREATE INDEX idx_hm_maskedaddresses_account ON hm_maskedaddresses (maaccountid);

ALTER TABLE hm_maskedaddresses ENGINE=InnoDB;

ALTER TABLE hm_maskedaddresses ADD CONSTRAINT fk_hm_maskedaddresses_account FOREIGN KEY (maaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

update hm_dbversion set value = 6067;
