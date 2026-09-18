create table hm_domainpolicylist
(
	dplid serial not null primary key,
	dplpattern varchar(255) not null,
	dplmatchon int not null,
	dplaction int not null,
	dplnote varchar(255) not null,
	dplcreated timestamp not null
);

CREATE UNIQUE INDEX idx_hm_domainpolicylist_pattern ON hm_domainpolicylist (dplpattern, dplmatchon);

update hm_dbversion set value = 6073;
