alter table hm_calendarshares add column sharegroupid int not null default 0;

alter table hm_calendarshares add column sharedelegate int not null default 0;

CREATE UNIQUE INDEX idx_hm_calendarshares_trio ON hm_calendarshares (sharecalendarid, shareaccountid, sharegroupid);

DROP INDEX idx_hm_calendarshares_pair ON hm_calendarshares /* [IGNORE-ERRORS] */;

ALTER TABLE hm_calendarshares RENAME INDEX idx_hm_calendarshares_trio TO idx_hm_calendarshares_pair;

update hm_dbversion set value = 6086;
