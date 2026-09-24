create table hm_discoverycases
(
	caseid int auto_increment not null, primary key(`caseid`), unique(`caseid`),
	casename varchar(255) not null,
	casedescription varchar(1000) not null,
	casestate int not null,
	casecreatedby varchar(255) not null,
	casecreated bigint not null,
	caseclosedby varchar(255) not null,
	caseclosed bigint not null
) DEFAULT CHARSET=utf8;

ALTER TABLE hm_discoverycases ENGINE=InnoDB;

create table hm_discoverysearches
(
	searchid int auto_increment not null, primary key(`searchid`), unique(`searchid`),
	searchcaseid int not null,
	searchquery text not null,
	searchranby varchar(255) not null,
	searchran bigint not null,
	searchfinished bigint not null,
	searchstate int not null,
	searchscanned int not null,
	searchmatched int not null,
	searchunsearched int not null,
	searchtruncated int not null,
	searcherror varchar(1000) not null,
	searchresultfile varchar(255) not null
) DEFAULT CHARSET=utf8;

CREATE INDEX idx_hm_discoverysearches_case ON hm_discoverysearches (searchcaseid);

ALTER TABLE hm_discoverysearches ENGINE=InnoDB;

ALTER TABLE hm_discoverysearches ADD CONSTRAINT fk_hm_discoverysearches_case FOREIGN KEY (searchcaseid) REFERENCES hm_discoverycases (caseid) ON DELETE CASCADE;

create table hm_discoveryholds
(
	holdid int auto_increment not null, primary key(`holdid`), unique(`holdid`),
	holdcaseid int not null,
	holdscope int not null,
	holdownerid bigint not null,
	holdname varchar(255) not null,
	holdplacedby varchar(255) not null,
	holdplaced bigint not null,
	holdreleasedby varchar(255) not null,
	holdreleased bigint not null
) DEFAULT CHARSET=utf8;

CREATE INDEX idx_hm_discoveryholds_case ON hm_discoveryholds (holdcaseid);

CREATE INDEX idx_hm_discoveryholds_owner ON hm_discoveryholds (holdscope, holdownerid);

ALTER TABLE hm_discoveryholds ENGINE=InnoDB;

ALTER TABLE hm_discoveryholds ADD CONSTRAINT fk_hm_discoveryholds_case FOREIGN KEY (holdcaseid) REFERENCES hm_discoverycases (caseid) ON DELETE CASCADE;

create table hm_discoveryexports
(
	exportid int auto_increment not null, primary key(`exportid`), unique(`exportid`),
	exportcaseid int not null,
	exportsearchid int not null,
	exportstate int not null,
	exportfile varchar(255) not null,
	exportsize bigint not null,
	exportsha256 varchar(64) not null,
	exportitems int not null,
	exportwithheld int not null,
	exportrequestedby varchar(255) not null,
	exportrequested bigint not null,
	exportfinished bigint not null,
	exporterror varchar(1000) not null
) DEFAULT CHARSET=utf8;

CREATE INDEX idx_hm_discoveryexports_case ON hm_discoveryexports (exportcaseid);

ALTER TABLE hm_discoveryexports ENGINE=InnoDB;

ALTER TABLE hm_discoveryexports ADD CONSTRAINT fk_hm_discoveryexports_search FOREIGN KEY (exportsearchid) REFERENCES hm_discoverysearches (searchid) ON DELETE CASCADE;

update hm_roles set rolepermissions = concat(rolepermissions, ',view_discovery,manage_discovery') where rolename = 'serveradmin' and rolepermissions not like '%view_discovery%';

update hm_roles set rolepermissions = concat(rolepermissions, ',view_discovery') where rolename = 'auditor' and rolepermissions not like '%view_discovery%';

update hm_dbversion set value = 6097;
