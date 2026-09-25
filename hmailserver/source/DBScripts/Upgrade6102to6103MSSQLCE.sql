ALTER TABLE hm_accounts ALTER COLUMN accounttotpsecret nvarchar(1024) NOT NULL

update hm_dbversion set value = 6103
