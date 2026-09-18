create table hm_directory
(
	dirid int identity(1,1) not null,
	diraccountid int not null,
	dirtitle nvarchar(100) not null default '',
	dirdepartment nvarchar(100) not null default '',
	dirphone nvarchar(60) not null default '',
	dirmobile nvarchar(60) not null default '',
	diroffice nvarchar(100) not null default '',
	dirphototype nvarchar(40) not null default '',
	dirphoto ntext not null default '',
	diroptout tinyint not null default 0,
	dirnote nvarchar(255) not null default '',
	dirupdated datetime not null
)

ALTER TABLE hm_directory ADD CONSTRAINT hm_directory_pk PRIMARY KEY NONCLUSTERED (dirid)

CREATE UNIQUE CLUSTERED INDEX idx_hm_directory_account ON hm_directory (diraccountid)

ALTER TABLE hm_directory ADD CONSTRAINT fk_hm_directory_account FOREIGN KEY (diraccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

update hm_dbversion set value = 6078
