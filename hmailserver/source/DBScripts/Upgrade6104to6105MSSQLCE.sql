ALTER TABLE hm_quarantine ADD quarantinehelo nvarchar(255) not null DEFAULT ''

update hm_dbversion set value = 6105
