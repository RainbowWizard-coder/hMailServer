ALTER TABLE hm_fetchaccounts ADD faorigin tinyint not null CONSTRAINT df_faorigin DEFAULT 0

update hm_dbversion set value = 6104
