create table hm_journalrules
(
	jrid int auto_increment not null, primary key(`jrid`), unique(`jrid`),
	jrscope int not null,
	jrdomainid int not null,
	jraccountid int not null,
	jraddress varchar(255) not null,
	jrdirection int not null,
	jrmode int not null,
	jrenabled tinyint not null,
	jrnote varchar(255) not null
) DEFAULT CHARSET=utf8;

CREATE INDEX idx_hm_journalrules_domain ON hm_journalrules (jrdomainid);

CREATE INDEX idx_hm_journalrules_account ON hm_journalrules (jraccountid);

ALTER TABLE hm_journalrules ADD CONSTRAINT fk_hm_journalrules_domain FOREIGN KEY (jrdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE;

ALTER TABLE hm_journalrules ENGINE=InnoDB;

update hm_dbversion set value = 6075;
