create table hm_domainsamples
(
	dsid serial not null primary key,
	dsday varchar(10) not null,
	dsdomain varchar(255) not null,
	dsspam int not null,
	dsviruses int not null,
	dsstorebytes bigint not null,
	dsstoremessages bigint not null
);

CREATE UNIQUE INDEX idx_hm_domainsamples_day_domain ON hm_domainsamples (dsday, dsdomain);

update hm_dbversion set value = 6062;
