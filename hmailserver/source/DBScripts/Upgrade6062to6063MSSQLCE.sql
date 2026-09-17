ALTER TABLE hm_rules ADD ruledomainid int not null DEFAULT 0

update hm_dbversion set value = 6063
