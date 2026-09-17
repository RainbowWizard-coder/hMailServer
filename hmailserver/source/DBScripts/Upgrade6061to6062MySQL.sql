create table hm_domainsamples
(
	dsid int auto_increment not null, primary key(`dsid`), unique(`dsid`),
	dsday varchar(10) not null,
	dsdomain varchar(255) character set utf8 collate utf8_bin not null,
	dsspam int not null,
	dsviruses int not null,
	dsstorebytes bigint not null,
	dsstoremessages bigint not null
) DEFAULT CHARSET=utf8;

CREATE UNIQUE INDEX idx_hm_domainsamples_day_domain ON hm_domainsamples (dsday, dsdomain);

ALTER TABLE hm_domainsamples ENGINE=InnoDB;

update hm_dbversion set value = 6062;
