ALTER TABLE hm_domains ADD domainrestricteddomains nvarchar(2000) not null CONSTRAINT df_hm_domains_restricteddomains DEFAULT ''

ALTER TABLE hm_accounts ADD accountrestricteddelivery int not null CONSTRAINT df_hm_accounts_restricteddelivery DEFAULT 0

update hm_dbversion set value = 6084
