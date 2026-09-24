create table hm_smimeca
(
	caid int identity(1,1) not null,
	cadomainid int not null,
	cacertificate ntext not null,
	cafingerprint nvarchar(64) not null,
	caprivatekey ntext not null,
	cacreated bigint not null,
	caretired bigint not null,
	caserial bigint not null
)

ALTER TABLE hm_smimeca ADD CONSTRAINT hm_smimeca_pk PRIMARY KEY (caid)

CREATE INDEX idx_hm_smimeca_domain ON hm_smimeca (cadomainid)

CREATE INDEX idx_hm_smimeca_fingerprint ON hm_smimeca (cafingerprint)

ALTER TABLE hm_smimeca ADD CONSTRAINT fk_hm_smimeca_domain FOREIGN KEY (cadomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE

create table hm_smimeissued
(
	issuedid int identity(1,1) not null,
	issuedcaid int not null,
	issuedaccountid int not null,
	issuedaddress nvarchar(255) not null,
	issuedserial bigint not null,
	issuedfingerprint nvarchar(64) not null,
	issuednotbefore bigint not null,
	issuednotafter bigint not null,
	issuedrevoked bigint not null,
	issuedreason nvarchar(255) not null,
	issuedcertificate ntext not null,
	issuedpending tinyint not null
)

ALTER TABLE hm_smimeissued ADD CONSTRAINT hm_smimeissued_pk PRIMARY KEY (issuedid)

CREATE UNIQUE INDEX idx_hm_smimeissued_serial ON hm_smimeissued (issuedcaid, issuedserial)

CREATE INDEX idx_hm_smimeissued_address ON hm_smimeissued (issuedaddress)

CREATE INDEX idx_hm_smimeissued_account ON hm_smimeissued (issuedaccountid)

CREATE INDEX idx_hm_smimeissued_fingerprint ON hm_smimeissued (issuedfingerprint)

ALTER TABLE hm_smimeissued ADD CONSTRAINT fk_hm_smimeissued_ca FOREIGN KEY (issuedcaid) REFERENCES hm_smimeca (caid) ON DELETE CASCADE

ALTER TABLE hm_smimekeys ADD smimeauthority tinyint not null DEFAULT 0

update hm_dbversion set value = 6095
