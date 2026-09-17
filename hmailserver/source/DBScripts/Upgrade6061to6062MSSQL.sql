create table hm_domainsamples
(
	dsid int identity(1,1) not null,
	dsday nvarchar(10) not null,
	dsdomain nvarchar(255) not null,
	dsspam int not null,
	dsviruses int not null,
	dsstorebytes bigint not null,
	dsstoremessages bigint not null
)

ALTER TABLE hm_domainsamples ADD CONSTRAINT hm_domainsamples_pk PRIMARY KEY NONCLUSTERED (dsid)

CREATE UNIQUE CLUSTERED INDEX idx_hm_domainsamples_day_domain ON hm_domainsamples (dsday, dsdomain)

update hm_dbversion set value = 6062
