ALTER TABLE hm_bouncecounts ADD bccomplaints int not null CONSTRAINT df_bccomplaints DEFAULT 0

ALTER TABLE hm_bouncecounts ADD bclastcomplaint nvarchar(32) not null CONSTRAINT df_bclastcomplaint DEFAULT ''

create table hm_complaints
(
	cpid bigint identity(1,1) not null,
	cpdomainid int not null,
	cpaccountid int not null,
	cpkey nvarchar(64) not null,
	cpaddress nvarchar(255) not null,
	cpsender nvarchar(255) not null,
	cpfeedbacktype nvarchar(32) not null,
	cpprovider nvarchar(255) not null,
	cpsourceip nvarchar(45) not null,
	cpreporteddomain nvarchar(255) not null,
	cpmessageid nvarchar(255) not null,
	cpsubject nvarchar(255) not null,
	cparrival nvarchar(32) not null,
	cpreceived nvarchar(32) not null
)

ALTER TABLE hm_complaints ADD CONSTRAINT hm_complaints_pk PRIMARY KEY NONCLUSTERED (cpid)

CREATE UNIQUE CLUSTERED INDEX idx_hm_complaints_key ON hm_complaints (cpkey)

CREATE INDEX idx_hm_complaints_domain ON hm_complaints (cpdomainid)

CREATE INDEX idx_hm_complaints_address ON hm_complaints (cpaddress)

CREATE INDEX idx_hm_complaints_received ON hm_complaints (cpreceived)

create table hm_complaintvolume
(
	cvid bigint identity(1,1) not null,
	cvdomainid int not null,
	cvday nvarchar(10) not null,
	cvsent bigint not null,
	cvcomplaints int not null
)

ALTER TABLE hm_complaintvolume ADD CONSTRAINT hm_complaintvolume_pk PRIMARY KEY NONCLUSTERED (cvid)

CREATE UNIQUE CLUSTERED INDEX idx_hm_complaintvolume_day ON hm_complaintvolume (cvdomainid, cvday)

ALTER TABLE hm_complaints ADD CONSTRAINT fk_hm_complaints_domain FOREIGN KEY (cpdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE

ALTER TABLE hm_complaintvolume ADD CONSTRAINT fk_hm_complaintvolume_domain FOREIGN KEY (cvdomainid) REFERENCES hm_domains (domainid) ON DELETE CASCADE

insert into hm_alertrules (alertrulecondition, alertruleenabled, alertrulethreshold, alertruleactions, alertruleaddress, alertrulewebhook, alertrulesecret, alertrulecooldown, alertruledigest) select 'complaints.rate.high', 1, 1000, 1, '', '', '', 1440, 1 from hm_dbversion where not exists (select alertrulecondition from hm_alertrules where alertrulecondition = 'complaints.rate.high')

update hm_dbversion set value = 6055
