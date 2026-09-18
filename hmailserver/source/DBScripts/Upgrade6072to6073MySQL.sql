create table hm_domainpolicylist
(
	dplid int auto_increment not null, primary key(`dplid`), unique(`dplid`),
	dplpattern varchar(255) not null,
	dplmatchon int not null,
	dplaction int not null,
	dplnote varchar(255) not null,
	dplcreated datetime not null
) DEFAULT CHARSET=utf8;

CREATE UNIQUE INDEX idx_hm_domainpolicylist_pattern ON hm_domainpolicylist (dplpattern, dplmatchon);

ALTER TABLE hm_domainpolicylist ENGINE=InnoDB;

update hm_dbversion set value = 6073;
