create table hm_neverbannames
(
	nbnid int auto_increment not null, primary key(`nbnid`), unique(`nbnid`),
	nbnpattern varchar(255) not null,
	nbnnote varchar(255) not null,
	nbncreated datetime not null
) ENGINE=InnoDB ROW_FORMAT=DYNAMIC DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;

CREATE UNIQUE INDEX idx_hm_neverbannames_pattern ON hm_neverbannames (nbnpattern);

alter table hm_tcpipports add column portsmtpauth tinyint not null default 0;

update hm_dbversion set value = 6108;
