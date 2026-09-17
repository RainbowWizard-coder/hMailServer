alter table hm_domains add column domainreportmailbox varchar(255) not null default '';

create table hm_dmarcreports
(
	drid serial not null primary key,
	drdomainid int not null,
	drkey varchar(64) not null,
	drorgname varchar(255) not null,
	dremail varchar(255) not null,
	drreportid varchar(255) not null,
	drbegin bigint not null,
	drend bigint not null,
	drpolicydomain varchar(255) not null,
	dradkim varchar(8) not null,
	draspf varchar(8) not null,
	drp varchar(16) not null,
	drsp varchar(16) not null,
	drpct int not null,
	drmessages int not null,
	drpassed int not null,
	drreceived bigint not null,
	drsource varchar(255) not null,
	drrecords int not null,
	drmsgdmarc varchar(16) not null,
	drmsgfrom varchar(255) not null
);

CREATE UNIQUE INDEX idx_hm_dmarcreports_key ON hm_dmarcreports (drkey);

CREATE INDEX idx_hm_dmarcreports_domain ON hm_dmarcreports (drdomainid);

CREATE INDEX idx_hm_dmarcreports_received ON hm_dmarcreports (drreceived);

ALTER TABLE hm_dmarcreports ADD CONSTRAINT fk_hm_dmarcreports_domain FOREIGN KEY (drdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE;

create table hm_dmarcreportrecords
(
	rrid serial not null primary key,
	rrreportid int not null,
	rrsourceip varchar(45) not null,
	rrcount int not null,
	rrdisposition varchar(16) not null,
	rrdkim varchar(8) not null,
	rrspf varchar(8) not null,
	rrheaderfrom varchar(255) not null,
	rrenvelopefrom varchar(255) not null,
	rrdkimauth varchar(500) not null,
	rrspfauth varchar(255) not null
);

CREATE INDEX idx_hm_dmarcreportrecords_report ON hm_dmarcreportrecords (rrreportid);

ALTER TABLE hm_dmarcreportrecords ADD CONSTRAINT fk_hm_dmarcreportrecords_report FOREIGN KEY (rrreportid) REFERENCES hm_dmarcreports (drid) ON DELETE CASCADE;

create table hm_tlsreports
(
	trid serial not null primary key,
	trdomainid int not null,
	trkey varchar(64) not null,
	trorgname varchar(255) not null,
	trcontact varchar(255) not null,
	trreportid varchar(255) not null,
	trbegin bigint not null,
	trend bigint not null,
	trpolicytype varchar(16) not null,
	trpolicydomain varchar(255) not null,
	trsuccess int not null,
	trfailure int not null,
	trfailures varchar(2000) not null,
	trreceived bigint not null,
	trsource varchar(255) not null
);

CREATE UNIQUE INDEX idx_hm_tlsreports_key ON hm_tlsreports (trkey);

CREATE INDEX idx_hm_tlsreports_domain ON hm_tlsreports (trdomainid);

CREATE INDEX idx_hm_tlsreports_received ON hm_tlsreports (trreceived);

ALTER TABLE hm_tlsreports ADD CONSTRAINT fk_hm_tlsreports_domain FOREIGN KEY (trdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE;

update hm_dbversion set value = 6070;
