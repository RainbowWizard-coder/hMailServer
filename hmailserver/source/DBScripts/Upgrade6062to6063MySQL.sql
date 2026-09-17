alter table hm_rules add column ruledomainid int not null default 0;

update hm_dbversion set value = 6063;
