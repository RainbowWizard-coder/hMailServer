create table hm_senderaddresses
(
	said serial not null primary key,
	saaccountid int not null,
	saaddress varchar(255) not null,
	saname varchar(255) not null,
	saverified smallint not null,
	sacreated timestamp not null
);

CREATE UNIQUE INDEX idx_hm_senderaddresses_address ON hm_senderaddresses (saaccountid, saaddress);

ALTER TABLE hm_senderaddresses ADD CONSTRAINT fk_hm_senderaddresses_account FOREIGN KEY (saaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

update hm_dbversion set value = 6066;
