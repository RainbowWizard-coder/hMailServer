create table hm_domainpolicylist
(
	dplid int identity(1,1) not null,
	dplpattern nvarchar(255) not null,
	dplmatchon int not null,
	dplaction int not null,
	dplnote nvarchar(255) not null,
	dplcreated datetime not null
)

ALTER TABLE hm_domainpolicylist ADD CONSTRAINT hm_domainpolicylist_pk PRIMARY KEY (dplid)

CREATE UNIQUE INDEX idx_hm_domainpolicylist_pattern ON hm_domainpolicylist (dplpattern, dplmatchon)

update hm_dbversion set value = 6073
