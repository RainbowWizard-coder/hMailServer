-- Before anything else, whether this server can hold the keys this step and the ones after it make
-- (varchar(255) in utf8mb4, 1020 bytes a key part): InnoDB with ROW_FORMAT=DYNAMIC, which is
-- MySQL 5.7.9 or MariaDB 10.2.2, or later. MySQL commits every CREATE and ALTER TABLE as it runs,
-- so a step that failed at such an index left what came before it in place with the version
-- row unchanged, and every later run then failed on a table or a column already there. On a
-- server that cannot hold the key this probe is refused with error 1071 ("Specified key was
-- too long") before anything has been changed; upgrade the database server and run it again.
-- The key is the primary key, since a server with sql_require_primary_key on refuses a table
-- without one.
DROP TABLE IF EXISTS hm_upgradeprobe;

CREATE TABLE hm_upgradeprobe
(
	probevalue varchar(255) not null,
	primary key(probevalue)
) ENGINE=InnoDB ROW_FORMAT=DYNAMIC DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;

DROP TABLE hm_upgradeprobe;

create table hm_calendars
(
	calendarid int auto_increment not null, primary key(`calendarid`), unique(`calendarid`),
	calendaraccountid int not null,
	calendarname varchar(255) not null,
	calendardisplayname varchar(255) not null,
	calendarsynctoken bigint not null,
	calendarcreated bigint not null
) ENGINE=InnoDB ROW_FORMAT=DYNAMIC DEFAULT CHARSET=utf8mb4;

CREATE UNIQUE INDEX idx_hm_calendars_account ON hm_calendars (calendaraccountid, calendarname);

ALTER TABLE hm_calendars ADD CONSTRAINT fk_hm_calendars_account FOREIGN KEY (calendaraccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

create table hm_calendarobjects
(
	objectid int auto_increment not null, primary key(`objectid`), unique(`objectid`),
	objectaccountid int not null,
	objectcalendarid int not null,
	objecturi varchar(255) not null,
	objectuid varchar(255) not null,
	objectcomponent varchar(16) not null,
	objectdata mediumtext not null,
	objectetag varchar(64) not null,
	objectsynctoken bigint not null,
	objectstart bigint not null,
	objectend bigint not null,
	objectfirst bigint not null,
	objectlast bigint not null,
	objectdeleted tinyint not null,
	objectmodified bigint not null
) ENGINE=InnoDB ROW_FORMAT=DYNAMIC DEFAULT CHARSET=utf8mb4;

CREATE UNIQUE INDEX idx_hm_calendarobjects_uri ON hm_calendarobjects (objectcalendarid, objecturi);

CREATE INDEX idx_hm_calendarobjects_sync ON hm_calendarobjects (objectcalendarid, objectsynctoken);

ALTER TABLE hm_calendarobjects ADD CONSTRAINT fk_hm_calendarobjects_calendar FOREIGN KEY (objectcalendarid) REFERENCES hm_calendars (calendarid) ON DELETE CASCADE;

update hm_dbversion set value = 6041;
