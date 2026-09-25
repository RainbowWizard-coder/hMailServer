alter table hm_accounts add column accountkind int not null default 0;

create table hm_bookingpolicies
(
	bpid int auto_increment not null, primary key(`bpid`), unique(`bpid`),
	bpaccountid int not null,
	bpautoaccept tinyint not null default 1,
	bpwindowdays int not null default 180,
	bpmaxminutes int not null default 1440,
	bpworkinghoursonly tinyint not null default 0,
	bpworkdays int not null default 31,
	bpworkstart int not null default 480,
	bpworkend int not null default 1020,
	bptimezone varchar(64) not null default 'UTC',
	bpallowconflicts tinyint not null default 0,
	bpallowrecurring tinyint not null default 1,
	bpbookers varchar(2000) not null default '',
	bpcapacity int not null default 0
) ENGINE=InnoDB ROW_FORMAT=DYNAMIC DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;

CREATE UNIQUE INDEX idx_hm_bookingpolicies_account ON hm_bookingpolicies (bpaccountid);

ALTER TABLE hm_bookingpolicies ADD CONSTRAINT fk_hm_bookingpolicies_account FOREIGN KEY (bpaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

ALTER TABLE hm_messages ADD COLUMN messagesubmitterid int NOT NULL DEFAULT 0, ADD COLUMN messageauthdomains varchar(255) NOT NULL DEFAULT '';

create table hm_bookinganswers
(
	baid bigint auto_increment not null, primary key(`baid`), unique(`baid`),
	baaccountid int not null,
	baorganiser varchar(255) character set utf8mb4 collate utf8mb4_bin not null,
	bawindow bigint not null,
	baanswers int not null,
	banoticed bigint not null
) ENGINE=InnoDB ROW_FORMAT=DYNAMIC DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;

CREATE UNIQUE INDEX idx_hm_bookinganswers_account ON hm_bookinganswers (baaccountid, baorganiser);

ALTER TABLE hm_bookinganswers ADD CONSTRAINT fk_hm_bookinganswers_account FOREIGN KEY (baaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

create table hm_bookingqueue
(
	bqid bigint auto_increment not null, primary key(`bqid`), unique(`bqid`),
	bqaccountid int not null,
	bqqueued bigint not null,
	bqsender varchar(255) not null,
	bqsubmitterid int not null,
	bqauthdomains varchar(255) not null,
	bqautosubmitted tinyint not null,
	bqattempts int not null default 0,
	bqcalendar mediumtext not null
) ENGINE=InnoDB ROW_FORMAT=DYNAMIC DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;

CREATE INDEX idx_hm_bookingqueue_account ON hm_bookingqueue (bqaccountid, bqid);

ALTER TABLE hm_bookingqueue ADD CONSTRAINT fk_hm_bookingqueue_account FOREIGN KEY (bqaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

create table hm_bookingdecisions
(
	bdid bigint auto_increment not null, primary key(`bdid`), unique(`bdid`),
	bdaccountid int not null,
	bduid varchar(255) character set utf8mb4 collate utf8mb4_bin not null,
	bdorganiser varchar(255) not null,
	bddecided bigint not null
) ENGINE=InnoDB ROW_FORMAT=DYNAMIC DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;

CREATE UNIQUE INDEX idx_hm_bookingdecisions_account ON hm_bookingdecisions (bdaccountid, bduid);

ALTER TABLE hm_bookingdecisions ADD CONSTRAINT fk_hm_bookingdecisions_account FOREIGN KEY (bdaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

update hm_dbversion set value = 6101;
