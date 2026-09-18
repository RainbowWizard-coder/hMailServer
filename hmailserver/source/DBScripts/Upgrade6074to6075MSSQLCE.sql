create table hm_journalrules
(
	jrid int identity(1,1) not null,
	jrscope int not null,
	jrdomainid int not null,
	jraccountid int not null,
	jraddress nvarchar(255) not null,
	jrdirection int not null,
	jrmode int not null,
	jrenabled tinyint not null,
	jrnote nvarchar(255) not null
)

ALTER TABLE hm_journalrules ADD CONSTRAINT hm_journalrules_pk PRIMARY KEY (jrid)

CREATE INDEX idx_hm_journalrules_domain ON hm_journalrules (jrdomainid)

CREATE INDEX idx_hm_journalrules_account ON hm_journalrules (jraccountid)

ALTER TABLE hm_journalrules ADD CONSTRAINT fk_hm_journalrules_domain FOREIGN KEY (jrdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE

update hm_dbversion set value = 6075
