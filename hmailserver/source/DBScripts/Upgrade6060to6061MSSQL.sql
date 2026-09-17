ALTER TABLE hm_domains ADD domainquotawarningpercent int not null CONSTRAINT df_domainquotawarningpercent DEFAULT -1

ALTER TABLE hm_domains ADD domainquotawarningsubject nvarchar(200) not null CONSTRAINT df_domainquotawarningsubject DEFAULT ''

ALTER TABLE hm_domains ADD domainquotawarningtext ntext not null CONSTRAINT df_domainquotawarningtext DEFAULT ''

ALTER TABLE hm_domains ADD domainquotagracepercent int not null CONSTRAINT df_domainquotagracepercent DEFAULT -1

ALTER TABLE hm_domains ADD domainquotasendstoppercent int not null CONSTRAINT df_domainquotasendstoppercent DEFAULT -1

update hm_dbversion set value = 6061
