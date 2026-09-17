create table hm_maskedaddresses
(
	maid int identity(1,1) not null,
	maaccountid int not null,
	maaddress nvarchar(255) not null,
	malabel nvarchar(255) not null,
	maactive tinyint not null,
	mareceived int not null,
	macreated datetime not null
)

ALTER TABLE hm_maskedaddresses ADD CONSTRAINT hm_maskedaddresses_pk PRIMARY KEY NONCLUSTERED (maid)

CREATE UNIQUE CLUSTERED INDEX idx_hm_maskedaddresses_address ON hm_maskedaddresses (maaddress)

CREATE INDEX idx_hm_maskedaddresses_account ON hm_maskedaddresses (maaccountid)

ALTER TABLE hm_maskedaddresses ADD CONSTRAINT fk_hm_maskedaddresses_account FOREIGN KEY (maaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

update hm_dbversion set value = 6067
