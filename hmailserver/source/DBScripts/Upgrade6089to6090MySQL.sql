create table hm_migrations
(
	migrationid bigint auto_increment not null, primary key(`migrationid`), unique(`migrationid`),
	migrationname varchar(255) not null,
	migrationkind int not null,
	migrationhost varchar(255) not null,
	migrationport int not null,
	migrationsecurity int not null,
	migrationoauthprovider varchar(32) not null,
	migrationdavurl varchar(255) not null,
	migrationconcurrency int not null,
	migrationmessagesperminute int not null,
	migrationstate int not null,
	migrationcreated bigint not null,
	migrationstartedat bigint not null,
	migrationfinishedat bigint not null
) DEFAULT CHARSET=utf8;

create table hm_migrationjobs
(
	mjid bigint auto_increment not null, primary key(`mjid`), unique(`mjid`),
	mjmigrationid bigint not null,
	mjsourceuser varchar(255) not null,
	mjsourcesecret text not null,
	mjsigninhash varchar(64) not null,
	mjtargetaccountid bigint not null,
	mjstate int not null,
	mjphase int not null,
	mjfolderstotal int not null,
	mjfoldersdone int not null,
	mjmessagestotal int not null,
	mjmessagesdone int not null,
	mjbytesdone bigint not null,
	mjskipped int not null,
	mjlasterror varchar(1000) not null,
	mjattempts int not null,
	mjnextattempt bigint not null,
	mjstartedat bigint not null,
	mjfinishedat bigint not null,
	mjreport text not null
) DEFAULT CHARSET=utf8;

CREATE INDEX idx_hm_migrationjobs_migration ON hm_migrationjobs (mjmigrationid, mjid);

CREATE INDEX idx_hm_migrationjobs_state ON hm_migrationjobs (mjstate);

create table hm_migrationfolders
(
	mfid bigint auto_increment not null, primary key(`mfid`), unique(`mfid`),
	mfjobid bigint not null,
	mffolder varchar(1024) not null,
	mftargetfolder varchar(1024) not null,
	mfuidvalidity varchar(64) not null,
	mflastuid bigint not null,
	mfmessages int not null,
	mfdone int not null
) DEFAULT CHARSET=utf8;

CREATE INDEX idx_hm_migrationfolders_job ON hm_migrationfolders (mfjobid, mfid);

ALTER TABLE hm_migrations ENGINE=InnoDB;

ALTER TABLE hm_migrationjobs ENGINE=InnoDB;

ALTER TABLE hm_migrationfolders ENGINE=InnoDB;

ALTER TABLE hm_migrationjobs ADD CONSTRAINT fk_hm_migrationjobs_migration FOREIGN KEY (mjmigrationid) REFERENCES hm_migrations (migrationid) ON DELETE CASCADE;

ALTER TABLE hm_migrationfolders ADD CONSTRAINT fk_hm_migrationfolders_job FOREIGN KEY (mfjobid) REFERENCES hm_migrationjobs (mjid) ON DELETE CASCADE;

insert into hm_settings (settingname, settingstring, settinginteger) select 'MigrationConnectionsPerHost', '', 4 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'MigrationConnectionsPerHost');

update hm_dbversion set value = 6090;
