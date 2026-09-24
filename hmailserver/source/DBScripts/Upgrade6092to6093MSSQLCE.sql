create table hm_warmup
(
	warmupid int identity(1,1) not null,
	warmupaddress nvarchar(64) not null,
	warmupstarted bigint not null,
	warmupplan int not null,
	warmupenabled int not null
)

ALTER TABLE hm_warmup ADD CONSTRAINT hm_warmup_pk PRIMARY KEY (warmupid)

CREATE UNIQUE INDEX idx_hm_warmup_address ON hm_warmup (warmupaddress)

create table hm_warmupcounts
(
	wcid int identity(1,1) not null,
	wcwarmupid int not null,
	wcday bigint not null,
	wcprovider nvarchar(32) not null,
	wcsent int not null,
	wcdeferred int not null,
	wcceiling int not null,
	wcplanday int not null
)

ALTER TABLE hm_warmupcounts ADD CONSTRAINT hm_warmupcounts_pk PRIMARY KEY (wcid)

CREATE UNIQUE INDEX idx_hm_warmupcounts_day ON hm_warmupcounts (wcwarmupid, wcday, wcprovider)

CREATE INDEX idx_hm_warmupcounts_date ON hm_warmupcounts (wcday)

ALTER TABLE hm_warmupcounts ADD CONSTRAINT fk_hm_warmupcounts_warmup FOREIGN KEY (wcwarmupid) REFERENCES hm_warmup (warmupid) ON DELETE CASCADE

update hm_dbversion set value = 6093
