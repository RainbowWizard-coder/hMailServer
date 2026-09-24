ALTER TABLE hm_accounts ADD accountexternalid nvarchar(255) not null CONSTRAINT df_hm_accounts_externalid DEFAULT ''

CREATE INDEX idx_hm_accounts_externalid ON hm_accounts (accountexternalid)

update hm_roles set rolepermissions = rolepermissions + ',provision' where rolename = 'serveradmin' and rolepermissions not like '%provision%'

update hm_dbversion set value = 6096
