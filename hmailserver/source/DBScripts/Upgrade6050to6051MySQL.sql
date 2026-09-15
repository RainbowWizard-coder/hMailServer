create table hm_bouncesuppressions
(
	bsid bigint auto_increment not null, primary key(`bsid`), unique(`bsid`),
	bsdomainid int not null,
	bsaddress varchar(255) not null,
	bssource int not null,
	bscategory int not null,
	bsstatus varchar(16) not null,
	bsdiagnostic varchar(255) not null,
	bsremotemta varchar(255) not null,
	bscreated varchar(32) not null,
	bsexpires varchar(32) not null
);

CREATE UNIQUE INDEX idx_hm_bouncesuppressions_address ON hm_bouncesuppressions (bsdomainid, bsaddress);

create table hm_bouncecounts
(
	bcid bigint auto_increment not null, primary key(`bcid`), unique(`bcid`),
	bcdomainid int not null,
	bcaddress varchar(255) not null,
	bchard int not null,
	bcsoft int not null,
	bcmailboxfull int not null,
	bcpolicy int not null,
	bcother int not null,
	bcconsecutivehard int not null,
	bclaststatus varchar(16) not null,
	bclastcategory int not null,
	bclastbounce varchar(32) not null,
	bclastdelivered varchar(32) not null
);

CREATE UNIQUE INDEX idx_hm_bouncecounts_address ON hm_bouncecounts (bcdomainid, bcaddress);

create table hm_bouncereportkeys
(
	brkid bigint auto_increment not null, primary key(`brkid`), unique(`brkid`),
	brkdomainid int not null,
	brkkey varchar(64) not null,
	brksent varchar(32) not null
);

CREATE INDEX idx_hm_bouncereportkeys_key ON hm_bouncereportkeys (brkkey);

CREATE INDEX idx_hm_bouncereportkeys_sent ON hm_bouncereportkeys (brksent);

ALTER TABLE hm_bouncesuppressions ENGINE=InnoDB;

ALTER TABLE hm_bouncesuppressions ADD CONSTRAINT fk_hm_bouncesuppressions_domain FOREIGN KEY (bsdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE;

ALTER TABLE hm_bouncecounts ENGINE=InnoDB;

ALTER TABLE hm_bouncecounts ADD CONSTRAINT fk_hm_bouncecounts_domain FOREIGN KEY (bcdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE;

ALTER TABLE hm_bouncereportkeys ENGINE=InnoDB;

ALTER TABLE hm_bouncereportkeys ADD CONSTRAINT fk_hm_bouncereportkeys_domain FOREIGN KEY (brkdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE;

update hm_dbversion set value = 6051;
