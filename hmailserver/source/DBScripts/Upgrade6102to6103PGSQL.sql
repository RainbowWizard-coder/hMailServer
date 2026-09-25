alter table hm_accounts alter column accounttotpsecret type varchar(1024);

update hm_dbversion set value = 6103;
