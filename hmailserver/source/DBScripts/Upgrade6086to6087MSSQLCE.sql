ALTER TABLE hm_domains ADD domainimpersonationaction int not null DEFAULT 0

ALTER TABLE hm_domains ADD domainprotectedpersons nvarchar(2000) not null DEFAULT ''

ALTER TABLE hm_domains ADD domainprotectedpartners nvarchar(2000) not null DEFAULT ''

update hm_dbversion set value = 6087
