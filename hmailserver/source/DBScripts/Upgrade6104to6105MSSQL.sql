ALTER TABLE hm_quarantine ADD quarantinehelo nvarchar(255) not null CONSTRAINT df_hm_quarantine_helo DEFAULT ''

update hm_dbversion set value = 6105
