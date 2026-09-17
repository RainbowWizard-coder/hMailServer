create table hm_senderaddresses
(
	said int auto_increment not null, primary key(`said`), unique(`said`),
	saaccountid int not null,
	saaddress varchar(255) not null,
	saname varchar(255) not null,
	saverified tinyint not null,
	sacreated datetime not null
) DEFAULT CHARSET=utf8;

CREATE UNIQUE INDEX idx_hm_senderaddresses_address ON hm_senderaddresses (saaccountid, saaddress);

ALTER TABLE hm_senderaddresses ENGINE=InnoDB;

ALTER TABLE hm_senderaddresses ADD CONSTRAINT fk_hm_senderaddresses_account FOREIGN KEY (saaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

update hm_dbversion set value = 6066;
