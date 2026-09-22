ALTER TABLE hm_calendarshares ADD sharegroupid int not null DEFAULT 0

ALTER TABLE hm_calendarshares ADD sharedelegate int not null DEFAULT 0

DROP INDEX hm_calendarshares.idx_hm_calendarshares_pair --- [IGNORE-ERRORS]

CREATE UNIQUE INDEX idx_hm_calendarshares_pair ON hm_calendarshares (sharecalendarid, shareaccountid, sharegroupid)

update hm_dbversion set value = 6086
