ALTER TABLE hm_domains ADD domainrestricteddomains nvarchar(2000) not null DEFAULT ''

ALTER TABLE hm_accounts ADD accountrestricteddelivery int not null DEFAULT 0

update hm_dbversion set value = 6084
