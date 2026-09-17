ALTER TABLE hm_fetchaccounts ADD faoauthprovider nvarchar(16) not null CONSTRAINT df_faoauthprovider DEFAULT ''

ALTER TABLE hm_fetchaccounts ADD faoauthtoken ntext not null CONSTRAINT df_faoauthtoken DEFAULT ''

ALTER TABLE hm_fetchaccounts ADD famirrorskip ntext not null CONSTRAINT df_famirrorskip DEFAULT ''

ALTER TABLE hm_fetchaccounts ADD famirrorseen ntext not null CONSTRAINT df_famirrorseen DEFAULT ''

update hm_dbversion set value = 6065
