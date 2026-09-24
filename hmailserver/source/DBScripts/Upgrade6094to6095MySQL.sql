create table hm_smimeca
(
	caid int auto_increment not null, primary key(`caid`), unique(`caid`),
	cadomainid int not null,
	cacertificate text not null,
	cafingerprint varchar(64) not null,
	caprivatekey text not null,
	cacreated bigint not null,
	caretired bigint not null,
	caserial bigint not null
) DEFAULT CHARSET=utf8;

CREATE INDEX idx_hm_smimeca_domain ON hm_smimeca (cadomainid);

CREATE INDEX idx_hm_smimeca_fingerprint ON hm_smimeca (cafingerprint);

ALTER TABLE hm_smimeca ENGINE=InnoDB;

ALTER TABLE hm_smimeca ADD CONSTRAINT fk_hm_smimeca_domain FOREIGN KEY (cadomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE;

create table hm_smimeissued
(
	issuedid int auto_increment not null, primary key(`issuedid`), unique(`issuedid`),
	issuedcaid int not null,
	issuedaccountid int not null,
	issuedaddress varchar(255) not null,
	issuedserial bigint not null,
	issuedfingerprint varchar(64) not null,
	issuednotbefore bigint not null,
	issuednotafter bigint not null,
	issuedrevoked bigint not null,
	issuedreason varchar(255) not null,
	issuedcertificate text not null,
	issuedpending tinyint not null
) DEFAULT CHARSET=utf8;

CREATE UNIQUE INDEX idx_hm_smimeissued_serial ON hm_smimeissued (issuedcaid, issuedserial);

CREATE INDEX idx_hm_smimeissued_address ON hm_smimeissued (issuedaddress);

CREATE INDEX idx_hm_smimeissued_account ON hm_smimeissued (issuedaccountid);

CREATE INDEX idx_hm_smimeissued_fingerprint ON hm_smimeissued (issuedfingerprint);

ALTER TABLE hm_smimeissued ENGINE=InnoDB;

ALTER TABLE hm_smimeissued ADD CONSTRAINT fk_hm_smimeissued_ca FOREIGN KEY (issuedcaid) REFERENCES hm_smimeca (caid) ON DELETE CASCADE;

alter table hm_smimekeys add column smimeauthority tinyint not null default 0;

update hm_dbversion set value = 6095;
