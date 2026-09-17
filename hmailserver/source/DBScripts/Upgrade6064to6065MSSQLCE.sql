ALTER TABLE hm_fetchaccounts ADD faoauthprovider nvarchar(16) not null DEFAULT ''

ALTER TABLE hm_fetchaccounts ADD faoauthtoken ntext not null DEFAULT ''

ALTER TABLE hm_fetchaccounts ADD famirrorskip ntext not null DEFAULT ''

ALTER TABLE hm_fetchaccounts ADD famirrorseen ntext not null DEFAULT ''

update hm_dbversion set value = 6065
