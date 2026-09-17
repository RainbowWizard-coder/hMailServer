alter table hm_calendars add column calendarcolor varchar(16) not null default '';

create table hm_calendarshares
(
	shareid serial not null primary key,
	sharecalendarid int not null,
	shareaccountid int not null,
	sharewrite int not null
);

CREATE UNIQUE INDEX idx_hm_calendarshares_pair ON hm_calendarshares (sharecalendarid, shareaccountid);

ALTER TABLE hm_calendarshares ADD CONSTRAINT fk_hm_calendarshares_calendar FOREIGN KEY (sharecalendarid) REFERENCES hm_calendars (calendarid) ON DELETE CASCADE;

update hm_dbversion set value = 6064;
