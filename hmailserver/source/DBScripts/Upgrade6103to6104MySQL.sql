alter table hm_fetchaccounts add column faorigin tinyint not null default 0;

update hm_dbversion set value = 6104;
