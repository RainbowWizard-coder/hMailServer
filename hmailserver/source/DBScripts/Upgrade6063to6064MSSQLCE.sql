ALTER TABLE hm_calendars ADD calendarcolor nvarchar(16) not null DEFAULT ''

create table hm_calendarshares
(
	shareid int identity(1,1) not null,
	sharecalendarid int not null,
	shareaccountid int not null,
	sharewrite int not null
)

ALTER TABLE hm_calendarshares ADD CONSTRAINT hm_calendarshares_pk PRIMARY KEY (shareid)

CREATE UNIQUE INDEX idx_hm_calendarshares_pair ON hm_calendarshares (sharecalendarid, shareaccountid)

ALTER TABLE hm_calendarshares ADD CONSTRAINT fk_hm_calendarshares_calendar FOREIGN KEY (sharecalendarid) REFERENCES hm_calendars (calendarid) ON DELETE CASCADE

update hm_dbversion set value = 6064
