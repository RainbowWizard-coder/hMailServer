ALTER TABLE hm_domains ADD domainimpersonationaction int not null CONSTRAINT df_hm_domains_impersonationaction DEFAULT 0

ALTER TABLE hm_domains ADD domainprotectedpersons nvarchar(2000) not null CONSTRAINT df_hm_domains_protectedpersons DEFAULT ''

ALTER TABLE hm_domains ADD domainprotectedpartners nvarchar(2000) not null CONSTRAINT df_hm_domains_protectedpartners DEFAULT ''

update hm_dbversion set value = 6087
