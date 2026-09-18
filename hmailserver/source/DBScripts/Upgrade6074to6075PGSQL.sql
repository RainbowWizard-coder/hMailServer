create table hm_journalrules
(
	jrid bigserial not null primary key,
	jrscope int not null,
	jrdomainid int not null,
	jraccountid int not null,
	jraddress varchar(255) not null,
	jrdirection int not null,
	jrmode int not null,
	jrenabled smallint not null,
	jrnote varchar(255) not null
);

CREATE INDEX idx_hm_journalrules_domain ON hm_journalrules (jrdomainid);

CREATE INDEX idx_hm_journalrules_account ON hm_journalrules (jraccountid);

ALTER TABLE hm_journalrules ADD CONSTRAINT fk_hm_journalrules_domain FOREIGN KEY (jrdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE;

update hm_dbversion set value = 6075;
