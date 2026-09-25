ALTER TABLE hm_accounts ADD accountkind int not null CONSTRAINT df_hm_accounts_kind DEFAULT 0

create table hm_bookingpolicies
(
	bpid int identity(1,1) not null,
	bpaccountid int not null,
	bpautoaccept tinyint not null default 1,
	bpwindowdays int not null default 180,
	bpmaxminutes int not null default 1440,
	bpworkinghoursonly tinyint not null default 0,
	bpworkdays int not null default 31,
	bpworkstart int not null default 480,
	bpworkend int not null default 1020,
	bptimezone nvarchar(64) not null default 'UTC',
	bpallowconflicts tinyint not null default 0,
	bpallowrecurring tinyint not null default 1,
	bpbookers nvarchar(2000) not null default '',
	bpcapacity int not null default 0
)

ALTER TABLE hm_bookingpolicies ADD CONSTRAINT hm_bookingpolicies_pk PRIMARY KEY NONCLUSTERED (bpid)

CREATE UNIQUE CLUSTERED INDEX idx_hm_bookingpolicies_account ON hm_bookingpolicies (bpaccountid)

ALTER TABLE hm_bookingpolicies ADD CONSTRAINT fk_hm_bookingpolicies_account FOREIGN KEY (bpaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

ALTER TABLE hm_messages ADD messagesubmitterid int NOT NULL DEFAULT 0, messageauthdomains nvarchar(255) NOT NULL DEFAULT ''

create table hm_bookinganswers
(
	baid bigint identity(1,1) not null,
	baaccountid int not null,
	baorganiser nvarchar(255) not null,
	bawindow bigint not null,
	baanswers int not null,
	banoticed bigint not null
)

ALTER TABLE hm_bookinganswers ADD CONSTRAINT hm_bookinganswers_pk PRIMARY KEY NONCLUSTERED (baid)

CREATE UNIQUE CLUSTERED INDEX idx_hm_bookinganswers_account ON hm_bookinganswers (baaccountid, baorganiser)

ALTER TABLE hm_bookinganswers ADD CONSTRAINT fk_hm_bookinganswers_account FOREIGN KEY (baaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

create table hm_bookingqueue
(
	bqid bigint identity(1,1) not null,
	bqaccountid int not null,
	bqqueued bigint not null,
	bqsender nvarchar(255) not null,
	bqsubmitterid int not null,
	bqauthdomains nvarchar(255) not null,
	bqautosubmitted tinyint not null,
	bqattempts int not null default 0,
	bqcalendar ntext not null
)

ALTER TABLE hm_bookingqueue ADD CONSTRAINT hm_bookingqueue_pk PRIMARY KEY NONCLUSTERED (bqid)

CREATE CLUSTERED INDEX idx_hm_bookingqueue_account ON hm_bookingqueue (bqaccountid, bqid)

ALTER TABLE hm_bookingqueue ADD CONSTRAINT fk_hm_bookingqueue_account FOREIGN KEY (bqaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

create table hm_bookingdecisions
(
	bdid bigint identity(1,1) not null,
	bdaccountid int not null,
	bduid nvarchar(255) not null,
	bdorganiser nvarchar(255) not null,
	bddecided bigint not null
)

ALTER TABLE hm_bookingdecisions ADD CONSTRAINT hm_bookingdecisions_pk PRIMARY KEY NONCLUSTERED (bdid)

CREATE UNIQUE CLUSTERED INDEX idx_hm_bookingdecisions_account ON hm_bookingdecisions (bdaccountid, bduid)

ALTER TABLE hm_bookingdecisions ADD CONSTRAINT fk_hm_bookingdecisions_account FOREIGN KEY (bdaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

update hm_dbversion set value = 6101
