ALTER TABLE hm_bouncecounts ADD bccomplaints int not null DEFAULT 0;

ALTER TABLE hm_bouncecounts ADD bclastcomplaint varchar(32) not null DEFAULT '';

create table hm_complaints
(
	cpid bigint auto_increment not null, primary key(`cpid`), unique(`cpid`),
	cpdomainid int not null,
	cpaccountid int not null,
	cpkey varchar(64) not null,
	cpaddress varchar(255) not null,
	cpsender varchar(255) not null,
	cpfeedbacktype varchar(32) not null,
	cpprovider varchar(255) not null,
	cpsourceip varchar(45) not null,
	cpreporteddomain varchar(255) not null,
	cpmessageid varchar(255) not null,
	cpsubject varchar(255) not null,
	cparrival varchar(32) not null,
	cpreceived varchar(32) not null
) DEFAULT CHARSET=utf8;

CREATE UNIQUE INDEX idx_hm_complaints_key ON hm_complaints (cpkey);

CREATE INDEX idx_hm_complaints_domain ON hm_complaints (cpdomainid);

CREATE INDEX idx_hm_complaints_address ON hm_complaints (cpaddress);

CREATE INDEX idx_hm_complaints_received ON hm_complaints (cpreceived);

create table hm_complaintvolume
(
	cvid bigint auto_increment not null, primary key(`cvid`), unique(`cvid`),
	cvdomainid int not null,
	cvday varchar(10) not null,
	cvsent bigint not null,
	cvcomplaints int not null
) DEFAULT CHARSET=utf8;

CREATE UNIQUE INDEX idx_hm_complaintvolume_day ON hm_complaintvolume (cvdomainid, cvday);

ALTER TABLE hm_complaints ADD CONSTRAINT fk_hm_complaints_domain FOREIGN KEY (cpdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE;

ALTER TABLE hm_complaintvolume ADD CONSTRAINT fk_hm_complaintvolume_domain FOREIGN KEY (cvdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE;

insert into hm_alertrules (alertrulecondition, alertruleenabled, alertrulethreshold, alertruleactions, alertruleaddress, alertrulewebhook, alertrulesecret, alertrulecooldown, alertruledigest) select 'complaints.rate.high', 1, 1000, 1, '', '', '', 1440, 1 from hm_dbversion where not exists (select alertrulecondition from hm_alertrules where alertrulecondition = 'complaints.rate.high');

update hm_dbversion set value = 6055;
