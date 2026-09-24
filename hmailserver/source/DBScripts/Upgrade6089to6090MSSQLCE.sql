create table hm_migrations
(
	migrationid bigint identity(1,1) not null,
	migrationname nvarchar(255) not null,
	migrationkind int not null,
	migrationhost nvarchar(255) not null,
	migrationport int not null,
	migrationsecurity int not null,
	migrationoauthprovider nvarchar(32) not null,
	migrationdavurl nvarchar(255) not null,
	migrationconcurrency int not null,
	migrationmessagesperminute int not null,
	migrationstate int not null,
	migrationcreated bigint not null,
	migrationstartedat bigint not null,
	migrationfinishedat bigint not null
)

ALTER TABLE hm_migrations ADD CONSTRAINT hm_migrations_pk PRIMARY KEY (migrationid)

create table hm_migrationjobs
(
	mjid bigint identity(1,1) not null,
	mjmigrationid bigint not null,
	mjsourceuser nvarchar(255) not null,
	mjsourcesecret ntext not null,
	mjsigninhash nvarchar(64) not null,
	mjtargetaccountid bigint not null,
	mjstate int not null,
	mjphase int not null,
	mjfolderstotal int not null,
	mjfoldersdone int not null,
	mjmessagestotal int not null,
	mjmessagesdone int not null,
	mjbytesdone bigint not null,
	mjskipped int not null,
	mjlasterror nvarchar(1000) not null,
	mjattempts int not null,
	mjnextattempt bigint not null,
	mjstartedat bigint not null,
	mjfinishedat bigint not null,
	mjreport ntext not null
)

ALTER TABLE hm_migrationjobs ADD CONSTRAINT hm_migrationjobs_pk PRIMARY KEY (mjid)

CREATE INDEX idx_hm_migrationjobs_migration ON hm_migrationjobs (mjmigrationid, mjid)

CREATE INDEX idx_hm_migrationjobs_state ON hm_migrationjobs (mjstate)

ALTER TABLE hm_migrationjobs ADD CONSTRAINT fk_hm_migrationjobs_migration FOREIGN KEY (mjmigrationid) REFERENCES hm_migrations (migrationid) ON DELETE CASCADE

create table hm_migrationfolders
(
	mfid bigint identity(1,1) not null,
	mfjobid bigint not null,
	mffolder nvarchar(1024) not null,
	mftargetfolder nvarchar(1024) not null,
	mfuidvalidity nvarchar(64) not null,
	mflastuid bigint not null,
	mfmessages int not null,
	mfdone int not null
)

ALTER TABLE hm_migrationfolders ADD CONSTRAINT hm_migrationfolders_pk PRIMARY KEY (mfid)

CREATE INDEX idx_hm_migrationfolders_job ON hm_migrationfolders (mfjobid, mfid)

ALTER TABLE hm_migrationfolders ADD CONSTRAINT fk_hm_migrationfolders_job FOREIGN KEY (mfjobid) REFERENCES hm_migrationjobs (mjid) ON DELETE CASCADE

insert into hm_settings (settingname, settingstring, settinginteger) select 'MigrationConnectionsPerHost', '', 4 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'MigrationConnectionsPerHost')

update hm_dbversion set value = 6090
