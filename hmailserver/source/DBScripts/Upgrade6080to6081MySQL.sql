alter table hm_pgpring add column pgrverified int not null default 0;

alter table hm_pgpring add column pgrverifiedat bigint not null default 0;

alter table hm_pgpring add column pgrchangedfrom varchar(64) not null default '';

alter table hm_pgpring add column pgrchangedat bigint not null default 0;

alter table hm_pgpring add column pgraccepted int not null default 0;

update hm_dbversion set value = 6081;
