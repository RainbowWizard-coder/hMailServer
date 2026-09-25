alter table hm_accounts add column accountkind int not null default 0;

create table hm_bookingpolicies
(
	bpid serial not null primary key,
	bpaccountid int not null,
	bpautoaccept smallint not null default 1,
	bpwindowdays int not null default 180,
	bpmaxminutes int not null default 1440,
	bpworkinghoursonly smallint not null default 0,
	bpworkdays int not null default 31,
	bpworkstart int not null default 480,
	bpworkend int not null default 1020,
	bptimezone varchar(64) not null default 'UTC',
	bpallowconflicts smallint not null default 0,
	bpallowrecurring smallint not null default 1,
	bpbookers varchar(2000) not null default '',
	bpcapacity int not null default 0
);

CREATE UNIQUE INDEX idx_hm_bookingpolicies_account ON hm_bookingpolicies (bpaccountid);

ALTER TABLE hm_bookingpolicies ADD CONSTRAINT fk_hm_bookingpolicies_account FOREIGN KEY (bpaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

ALTER TABLE hm_messages ADD COLUMN messagesubmitterid int NOT NULL DEFAULT 0, ADD COLUMN messageauthdomains varchar(255) NOT NULL DEFAULT '';

create table hm_bookinganswers
(
	baid bigserial not null primary key,
	baaccountid int not null,
	baorganiser varchar(255) not null,
	bawindow bigint not null,
	baanswers int not null,
	banoticed bigint not null
);

CREATE UNIQUE INDEX idx_hm_bookinganswers_account ON hm_bookinganswers (baaccountid, baorganiser);

ALTER TABLE hm_bookinganswers ADD CONSTRAINT fk_hm_bookinganswers_account FOREIGN KEY (baaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

create table hm_bookingqueue
(
	bqid bigserial not null primary key,
	bqaccountid int not null,
	bqqueued bigint not null,
	bqsender varchar(255) not null,
	bqsubmitterid int not null,
	bqauthdomains varchar(255) not null,
	bqautosubmitted smallint not null,
	bqattempts int not null default 0,
	bqcalendar text not null
);

CREATE INDEX idx_hm_bookingqueue_account ON hm_bookingqueue (bqaccountid, bqid);

ALTER TABLE hm_bookingqueue ADD CONSTRAINT fk_hm_bookingqueue_account FOREIGN KEY (bqaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

create table hm_bookingdecisions
(
	bdid bigserial not null primary key,
	bdaccountid int not null,
	bduid varchar(255) not null,
	bdorganiser varchar(255) not null,
	bddecided bigint not null
);

CREATE UNIQUE INDEX idx_hm_bookingdecisions_account ON hm_bookingdecisions (bdaccountid, bduid);

ALTER TABLE hm_bookingdecisions ADD CONSTRAINT fk_hm_bookingdecisions_account FOREIGN KEY (bdaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

update hm_dbversion set value = 6101;
