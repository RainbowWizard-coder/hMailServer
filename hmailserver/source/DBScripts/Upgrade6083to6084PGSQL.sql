alter table hm_domains add column domainrestricteddomains varchar(2000) not null default '';

alter table hm_accounts add column accountrestricteddelivery int not null default 0;

update hm_dbversion set value = 6084;
