create table hm_bouncesuppressions
(
	bsid bigint identity(1,1) not null,
	bsdomainid int not null,
	bsaddress nvarchar(255) not null,
	bssource int not null,
	bscategory int not null,
	bsstatus nvarchar(16) not null,
	bsdiagnostic nvarchar(255) not null,
	bsremotemta nvarchar(255) not null,
	bscreated nvarchar(32) not null,
	bsexpires nvarchar(32) not null
)

ALTER TABLE hm_bouncesuppressions ADD CONSTRAINT hm_bouncesuppressions_pk PRIMARY KEY NONCLUSTERED (bsid)

CREATE UNIQUE CLUSTERED INDEX idx_hm_bouncesuppressions_address ON hm_bouncesuppressions (bsdomainid, bsaddress)

create table hm_bouncecounts
(
	bcid bigint identity(1,1) not null,
	bcdomainid int not null,
	bcaddress nvarchar(255) not null,
	bchard int not null,
	bcsoft int not null,
	bcmailboxfull int not null,
	bcpolicy int not null,
	bcother int not null,
	bcconsecutivehard int not null,
	bclaststatus nvarchar(16) not null,
	bclastcategory int not null,
	bclastbounce nvarchar(32) not null,
	bclastdelivered nvarchar(32) not null
)

ALTER TABLE hm_bouncecounts ADD CONSTRAINT hm_bouncecounts_pk PRIMARY KEY NONCLUSTERED (bcid)

CREATE UNIQUE CLUSTERED INDEX idx_hm_bouncecounts_address ON hm_bouncecounts (bcdomainid, bcaddress)

create table hm_bouncereportkeys
(
	brkid bigint identity(1,1) not null,
	brkdomainid int not null,
	brkkey nvarchar(64) not null,
	brksent nvarchar(32) not null,
	brkaddress nvarchar(64) not null,
	brkrun int not null
)

ALTER TABLE hm_bouncereportkeys ADD CONSTRAINT hm_bouncereportkeys_pk PRIMARY KEY (brkid)

CREATE INDEX idx_hm_bouncereportkeys_key ON hm_bouncereportkeys (brkkey)

CREATE INDEX idx_hm_bouncereportkeys_sent ON hm_bouncereportkeys (brksent)

CREATE INDEX idx_hm_bouncereportkeys_address ON hm_bouncereportkeys (brkaddress)

ALTER TABLE hm_bouncesuppressions ADD CONSTRAINT fk_hm_bouncesuppressions_domain FOREIGN KEY (bsdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE

ALTER TABLE hm_bouncecounts ADD CONSTRAINT fk_hm_bouncecounts_domain FOREIGN KEY (bcdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE

ALTER TABLE hm_bouncereportkeys ADD CONSTRAINT fk_hm_bouncereportkeys_domain FOREIGN KEY (brkdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE

update hm_dbversion set value = 6049
