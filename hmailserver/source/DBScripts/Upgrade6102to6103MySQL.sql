alter table hm_accounts modify accounttotpsecret varchar(1024) not null default '';

update hm_dbversion set value = 6103;
