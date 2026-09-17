ALTER TABLE hm_domains ADD domainreportmailbox nvarchar(255) not null DEFAULT ''

create table hm_dmarcreports
(
	drid int identity(1,1) not null,
	drdomainid int not null,
	drkey nvarchar(64) not null,
	drorgname nvarchar(255) not null,
	dremail nvarchar(255) not null,
	drreportid nvarchar(255) not null,
	drbegin bigint not null,
	drend bigint not null,
	drpolicydomain nvarchar(255) not null,
	dradkim nvarchar(8) not null,
	draspf nvarchar(8) not null,
	drp nvarchar(16) not null,
	drsp nvarchar(16) not null,
	drpct int not null,
	drmessages int not null,
	drpassed int not null,
	drreceived bigint not null,
	drsource nvarchar(255) not null,
	drrecords int not null,
	drmsgdmarc nvarchar(16) not null,
	drmsgfrom nvarchar(255) not null
)

ALTER TABLE hm_dmarcreports ADD CONSTRAINT hm_dmarcreports_pk PRIMARY KEY (drid)

CREATE UNIQUE INDEX idx_hm_dmarcreports_key ON hm_dmarcreports (drkey)

CREATE INDEX idx_hm_dmarcreports_domain ON hm_dmarcreports (drdomainid)

CREATE INDEX idx_hm_dmarcreports_received ON hm_dmarcreports (drreceived)

ALTER TABLE hm_dmarcreports ADD CONSTRAINT fk_hm_dmarcreports_domain FOREIGN KEY (drdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE

create table hm_dmarcreportrecords
(
	rrid int identity(1,1) not null,
	rrreportid int not null,
	rrsourceip nvarchar(45) not null,
	rrcount int not null,
	rrdisposition nvarchar(16) not null,
	rrdkim nvarchar(8) not null,
	rrspf nvarchar(8) not null,
	rrheaderfrom nvarchar(255) not null,
	rrenvelopefrom nvarchar(255) not null,
	rrdkimauth nvarchar(500) not null,
	rrspfauth nvarchar(255) not null
)

ALTER TABLE hm_dmarcreportrecords ADD CONSTRAINT hm_dmarcreportrecords_pk PRIMARY KEY (rrid)

CREATE INDEX idx_hm_dmarcreportrecords_report ON hm_dmarcreportrecords (rrreportid)

ALTER TABLE hm_dmarcreportrecords ADD CONSTRAINT fk_hm_dmarcreportrecords_report FOREIGN KEY (rrreportid) REFERENCES hm_dmarcreports (drid) ON DELETE CASCADE

create table hm_tlsreports
(
	trid int identity(1,1) not null,
	trdomainid int not null,
	trkey nvarchar(64) not null,
	trorgname nvarchar(255) not null,
	trcontact nvarchar(255) not null,
	trreportid nvarchar(255) not null,
	trbegin bigint not null,
	trend bigint not null,
	trpolicytype nvarchar(16) not null,
	trpolicydomain nvarchar(255) not null,
	trsuccess int not null,
	trfailure int not null,
	trfailures nvarchar(2000) not null,
	trreceived bigint not null,
	trsource nvarchar(255) not null
)

ALTER TABLE hm_tlsreports ADD CONSTRAINT hm_tlsreports_pk PRIMARY KEY (trid)

CREATE UNIQUE INDEX idx_hm_tlsreports_key ON hm_tlsreports (trkey)

CREATE INDEX idx_hm_tlsreports_domain ON hm_tlsreports (trdomainid)

CREATE INDEX idx_hm_tlsreports_received ON hm_tlsreports (trreceived)

ALTER TABLE hm_tlsreports ADD CONSTRAINT fk_hm_tlsreports_domain FOREIGN KEY (trdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE

update hm_dbversion set value = 6070
