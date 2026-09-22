alter table hm_domains add column domainimpersonationaction int not null default 0;

alter table hm_domains add column domainprotectedpersons varchar(2000) not null default '';

alter table hm_domains add column domainprotectedpartners varchar(2000) not null default '';

update hm_dbversion set value = 6087;
