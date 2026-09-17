alter table hm_domains add column domainquotawarningpercent int not null default -1;

alter table hm_domains add column domainquotawarningsubject varchar(200) not null default '';

alter table hm_domains add column domainquotawarningtext text not null default '';

alter table hm_domains add column domainquotagracepercent int not null default -1;

alter table hm_domains add column domainquotasendstoppercent int not null default -1;

update hm_dbversion set value = 6061;
