insert into hm_settings (settingname, settingstring, settinginteger) select 'backupencryptionpassphrase', '', 0 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'backupencryptionpassphrase')

update hm_dbversion set value = 6059
