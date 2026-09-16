alter table hm_domains add column domaincountrypolicy varchar(255) not null default '';

insert into hm_settings (settingname, settingstring, settinginteger) select 'GeoIpDatabaseFile', '', 0 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'GeoIpDatabaseFile');

insert into hm_settings (settingname, settingstring, settinginteger) select 'GeoIpDatabaseMaxAgeDays', '', 45 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'GeoIpDatabaseMaxAgeDays');

insert into hm_settings (settingname, settingstring, settinginteger) select 'CountryBlockingExemptRanges', '127.0.0.0/8,::1/128,10.0.0.0/8,172.16.0.0/12,192.168.0.0/16,169.254.0.0/16,100.64.0.0/10,fe80::/10,fc00::/7', 0 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'CountryBlockingExemptRanges');

insert into hm_settings (settingname, settingstring, settinginteger) select 'CountryPolicySMTP', '', 0 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'CountryPolicySMTP');

insert into hm_settings (settingname, settingstring, settinginteger) select 'CountryPolicyIMAP', '', 0 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'CountryPolicyIMAP');

insert into hm_settings (settingname, settingstring, settinginteger) select 'CountryPolicyPOP3', '', 0 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'CountryPolicyPOP3');

insert into hm_settings (settingname, settingstring, settinginteger) select 'CountryPolicyHTTP', '', 0 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'CountryPolicyHTTP');

update hm_dbversion set value = 6052;
