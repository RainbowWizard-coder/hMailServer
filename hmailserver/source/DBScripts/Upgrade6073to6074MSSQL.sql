ALTER TABLE hm_distributionlists ADD distributionlistmembership nvarchar(200) not null CONSTRAINT df_distributionlistmembership DEFAULT ''

update hm_dbversion set value = 6074
