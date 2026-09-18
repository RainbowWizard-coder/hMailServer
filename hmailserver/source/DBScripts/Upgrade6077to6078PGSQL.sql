create table hm_directory
(
	dirid serial not null primary key,
	diraccountid int not null,
	dirtitle varchar(100) not null default '',
	dirdepartment varchar(100) not null default '',
	dirphone varchar(60) not null default '',
	dirmobile varchar(60) not null default '',
	diroffice varchar(100) not null default '',
	dirphototype varchar(40) not null default '',
	dirphoto text not null default '',
	diroptout smallint not null default 0,
	dirnote varchar(255) not null default '',
	dirupdated timestamp not null
);

CREATE UNIQUE INDEX idx_hm_directory_account ON hm_directory (diraccountid);

ALTER TABLE hm_directory ADD CONSTRAINT fk_hm_directory_account FOREIGN KEY (diraccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

update hm_dbversion set value = 6078;
