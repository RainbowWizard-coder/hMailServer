create table hm_discoverycases
(
	caseid int identity(1,1) not null,
	casename nvarchar(255) not null,
	casedescription nvarchar(1000) not null,
	casestate int not null,
	casecreatedby nvarchar(255) not null,
	casecreated bigint not null,
	caseclosedby nvarchar(255) not null,
	caseclosed bigint not null
)

ALTER TABLE hm_discoverycases ADD CONSTRAINT hm_discoverycases_pk PRIMARY KEY NONCLUSTERED (caseid)

create table hm_discoverysearches
(
	searchid int identity(1,1) not null,
	searchcaseid int not null,
	searchquery ntext not null,
	searchranby nvarchar(255) not null,
	searchran bigint not null,
	searchfinished bigint not null,
	searchstate int not null,
	searchscanned int not null,
	searchmatched int not null,
	searchunsearched int not null,
	searchtruncated int not null,
	searcherror nvarchar(1000) not null,
	searchresultfile nvarchar(255) not null
)

ALTER TABLE hm_discoverysearches ADD CONSTRAINT hm_discoverysearches_pk PRIMARY KEY NONCLUSTERED (searchid)

CREATE CLUSTERED INDEX idx_hm_discoverysearches_case ON hm_discoverysearches (searchcaseid)

ALTER TABLE hm_discoverysearches ADD CONSTRAINT fk_hm_discoverysearches_case FOREIGN KEY (searchcaseid) REFERENCES hm_discoverycases (caseid) ON DELETE CASCADE

create table hm_discoveryholds
(
	holdid int identity(1,1) not null,
	holdcaseid int not null,
	holdscope int not null,
	holdownerid bigint not null,
	holdname nvarchar(255) not null,
	holdplacedby nvarchar(255) not null,
	holdplaced bigint not null,
	holdreleasedby nvarchar(255) not null,
	holdreleased bigint not null
)

ALTER TABLE hm_discoveryholds ADD CONSTRAINT hm_discoveryholds_pk PRIMARY KEY NONCLUSTERED (holdid)

CREATE CLUSTERED INDEX idx_hm_discoveryholds_case ON hm_discoveryholds (holdcaseid)

CREATE NONCLUSTERED INDEX idx_hm_discoveryholds_owner ON hm_discoveryholds (holdscope, holdownerid)

ALTER TABLE hm_discoveryholds ADD CONSTRAINT fk_hm_discoveryholds_case FOREIGN KEY (holdcaseid) REFERENCES hm_discoverycases (caseid) ON DELETE CASCADE

create table hm_discoveryexports
(
	exportid int identity(1,1) not null,
	exportcaseid int not null,
	exportsearchid int not null,
	exportstate int not null,
	exportfile nvarchar(255) not null,
	exportsize bigint not null,
	exportsha256 nvarchar(64) not null,
	exportitems int not null,
	exportwithheld int not null,
	exportrequestedby nvarchar(255) not null,
	exportrequested bigint not null,
	exportfinished bigint not null,
	exporterror nvarchar(1000) not null
)

ALTER TABLE hm_discoveryexports ADD CONSTRAINT hm_discoveryexports_pk PRIMARY KEY NONCLUSTERED (exportid)

CREATE CLUSTERED INDEX idx_hm_discoveryexports_case ON hm_discoveryexports (exportcaseid)

ALTER TABLE hm_discoveryexports ADD CONSTRAINT fk_hm_discoveryexports_search FOREIGN KEY (exportsearchid) REFERENCES hm_discoverysearches (searchid) ON DELETE CASCADE

update hm_roles set rolepermissions = rolepermissions + ',view_discovery,manage_discovery' where rolename = 'serveradmin' and rolepermissions not like '%view_discovery%'

update hm_roles set rolepermissions = rolepermissions + ',view_discovery' where rolename = 'auditor' and rolepermissions not like '%view_discovery%'

update hm_dbversion set value = 6097
