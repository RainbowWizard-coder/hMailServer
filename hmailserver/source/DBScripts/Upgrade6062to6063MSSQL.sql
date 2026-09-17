ALTER TABLE hm_rules ADD ruledomainid int not null CONSTRAINT df_ruledomainid DEFAULT 0

update hm_dbversion set value = 6063
