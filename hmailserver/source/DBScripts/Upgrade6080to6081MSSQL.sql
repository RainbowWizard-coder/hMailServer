ALTER TABLE hm_pgpring ADD pgrverified int not null CONSTRAINT df_hm_pgpring_verified DEFAULT 0

ALTER TABLE hm_pgpring ADD pgrverifiedat bigint not null CONSTRAINT df_hm_pgpring_verifiedat DEFAULT 0

ALTER TABLE hm_pgpring ADD pgrchangedfrom nvarchar(64) not null CONSTRAINT df_hm_pgpring_changedfrom DEFAULT ''

ALTER TABLE hm_pgpring ADD pgrchangedat bigint not null CONSTRAINT df_hm_pgpring_changedat DEFAULT 0

ALTER TABLE hm_pgpring ADD pgraccepted int not null CONSTRAINT df_hm_pgpring_accepted DEFAULT 0

update hm_dbversion set value = 6081
