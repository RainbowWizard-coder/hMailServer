create table hm_warmup
(
	warmupid serial not null primary key,
	warmupaddress varchar(64) not null,
	warmupstarted bigint not null,
	warmupplan int not null,
	warmupenabled int not null
);

CREATE UNIQUE INDEX idx_hm_warmup_address ON hm_warmup (warmupaddress);

create table hm_warmupcounts
(
	wcid serial not null primary key,
	wcwarmupid int not null,
	wcday bigint not null,
	wcprovider varchar(32) not null,
	wcsent int not null,
	wcdeferred int not null,
	wcceiling int not null,
	wcplanday int not null
);

CREATE UNIQUE INDEX idx_hm_warmupcounts_day ON hm_warmupcounts (wcwarmupid, wcday, wcprovider);

CREATE INDEX idx_hm_warmupcounts_date ON hm_warmupcounts (wcday);

ALTER TABLE hm_warmupcounts ADD CONSTRAINT fk_hm_warmupcounts_warmup FOREIGN KEY (wcwarmupid) REFERENCES hm_warmup (warmupid) ON DELETE CASCADE;

update hm_dbversion set value = 6093;
