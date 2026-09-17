create table hm_senderaddresses
(
	said int identity(1,1) not null,
	saaccountid int not null,
	saaddress nvarchar(255) not null,
	saname nvarchar(255) not null,
	saverified tinyint not null,
	sacreated datetime not null
)

ALTER TABLE hm_senderaddresses ADD CONSTRAINT hm_senderaddresses_pk PRIMARY KEY NONCLUSTERED (said)

CREATE UNIQUE CLUSTERED INDEX idx_hm_senderaddresses_address ON hm_senderaddresses (saaccountid, saaddress)

ALTER TABLE hm_senderaddresses ADD CONSTRAINT fk_hm_senderaddresses_account FOREIGN KEY (saaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

update hm_dbversion set value = 6066
