create table hm_neverbannames
(
	nbnid serial not null primary key,
	nbnpattern varchar(255) not null,
	nbnnote varchar(255) not null,
	nbncreated timestamp not null
);

CREATE UNIQUE INDEX idx_hm_neverbannames_pattern ON hm_neverbannames (nbnpattern);

alter table hm_tcpipports add column portsmtpauth smallint not null default 0;

update hm_dbversion set value = 6108;
