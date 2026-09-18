create table hm_passkeys
(
	pkid serial not null primary key,
	pkaccountid int null,
	pkcredential varchar(200) not null,
	pkpublickey text not null,
	pkalgorithm int not null,
	pksigncount bigint not null,
	pkname varchar(255) not null,
	pktransports varchar(255) not null,
	pkaaguid varchar(64) not null,
	pkcreated bigint not null,
	pklastused bigint not null
);

CREATE INDEX idx_hm_passkeys_account ON hm_passkeys (pkaccountid);

CREATE UNIQUE INDEX idx_hm_passkeys_credential ON hm_passkeys (pkcredential);

ALTER TABLE hm_passkeys ADD CONSTRAINT fk_hm_passkeys_account FOREIGN KEY (pkaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

update hm_dbversion set value = 6077;
