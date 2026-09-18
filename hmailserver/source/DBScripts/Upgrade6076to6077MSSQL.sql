create table hm_passkeys
(
	pkid int identity(1,1) not null,
	pkaccountid int null,
	pkcredential nvarchar(200) not null,
	pkpublickey ntext not null,
	pkalgorithm int not null,
	pksigncount bigint not null,
	pkname nvarchar(255) not null,
	pktransports nvarchar(255) not null,
	pkaaguid nvarchar(64) not null,
	pkcreated bigint not null,
	pklastused bigint not null
)

ALTER TABLE hm_passkeys ADD CONSTRAINT hm_passkeys_pk PRIMARY KEY NONCLUSTERED (pkid)

CREATE CLUSTERED INDEX idx_hm_passkeys_account ON hm_passkeys (pkaccountid)

CREATE UNIQUE INDEX idx_hm_passkeys_credential ON hm_passkeys (pkcredential)

ALTER TABLE hm_passkeys ADD CONSTRAINT fk_hm_passkeys_account FOREIGN KEY (pkaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

update hm_dbversion set value = 6077
