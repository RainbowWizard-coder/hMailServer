alter table hm_quarantine add column quarantinehelo varchar(255) not null default '';

update hm_dbversion set value = 6105;
