alter table hm_accounts add column accountexternalid varchar(255) not null default '';

CREATE INDEX idx_hm_accounts_externalid ON hm_accounts (accountexternalid);

update hm_roles set rolepermissions = concat(rolepermissions, ',provision') where rolename = 'serveradmin' and rolepermissions not like '%provision%';

update hm_dbversion set value = 6096;
