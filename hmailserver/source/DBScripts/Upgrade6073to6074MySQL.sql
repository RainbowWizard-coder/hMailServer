alter table hm_distributionlists add column distributionlistmembership varchar(200) not null default '';

update hm_dbversion set value = 6074;
