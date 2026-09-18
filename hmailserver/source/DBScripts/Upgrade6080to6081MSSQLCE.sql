ALTER TABLE hm_pgpring ADD pgrverified int not null DEFAULT 0

ALTER TABLE hm_pgpring ADD pgrverifiedat bigint not null DEFAULT 0

ALTER TABLE hm_pgpring ADD pgrchangedfrom nvarchar(64) not null DEFAULT ''

ALTER TABLE hm_pgpring ADD pgrchangedat bigint not null DEFAULT 0

ALTER TABLE hm_pgpring ADD pgraccepted int not null DEFAULT 0

update hm_dbversion set value = 6081
