alter table hm_calendarshares add column sharegroupid int not null default 0;

alter table hm_calendarshares add column sharedelegate int not null default 0;

drop index if exists idx_hm_calendarshares_pair;

CREATE UNIQUE INDEX idx_hm_calendarshares_pair ON hm_calendarshares (sharecalendarid, shareaccountid, sharegroupid);

update hm_dbversion set value = 6086;
