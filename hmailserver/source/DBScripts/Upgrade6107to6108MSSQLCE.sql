create table hm_neverbannames
(
	nbnid int identity(1,1) not null,
	nbnpattern nvarchar(255) not null,
	nbnnote nvarchar(255) not null,
	nbncreated datetime not null
)

ALTER TABLE hm_neverbannames ADD CONSTRAINT hm_neverbannames_pk PRIMARY KEY (nbnid)

CREATE UNIQUE INDEX idx_hm_neverbannames_pattern ON hm_neverbannames (nbnpattern)

ALTER TABLE hm_tcpipports ADD portsmtpauth tinyint not null DEFAULT 0

update hm_dbversion set value = 6108
