ALTER TABLE hm_calendarshares ADD sharegroupid int not null CONSTRAINT df_hm_calendarshares_group DEFAULT 0

ALTER TABLE hm_calendarshares ADD sharedelegate int not null CONSTRAINT df_hm_calendarshares_delegate DEFAULT 0

DROP INDEX idx_hm_calendarshares_pair ON hm_calendarshares --- [IGNORE-ERRORS]

CREATE UNIQUE CLUSTERED INDEX idx_hm_calendarshares_pair ON hm_calendarshares (sharecalendarid, shareaccountid, sharegroupid)

update hm_dbversion set value = 6086
