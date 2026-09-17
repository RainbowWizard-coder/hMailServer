ALTER TABLE hm_domains ADD domainquotawarningpercent int not null DEFAULT -1

ALTER TABLE hm_domains ADD domainquotawarningsubject nvarchar(200) not null DEFAULT ''

ALTER TABLE hm_domains ADD domainquotawarningtext ntext not null DEFAULT ''

ALTER TABLE hm_domains ADD domainquotagracepercent int not null DEFAULT -1

ALTER TABLE hm_domains ADD domainquotasendstoppercent int not null DEFAULT -1

update hm_dbversion set value = 6061
