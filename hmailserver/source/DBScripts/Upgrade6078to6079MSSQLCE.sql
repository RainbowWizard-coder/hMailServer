create table hm_addressbooks
(
	bookid int identity(1,1) not null,
	bookaccountid int not null,
	bookname nvarchar(255) not null,
	bookcreated bigint not null
)

ALTER TABLE hm_addressbooks ADD CONSTRAINT hm_addressbooks_pk PRIMARY KEY (bookid)

CREATE UNIQUE INDEX idx_hm_addressbooks_account ON hm_addressbooks (bookaccountid, bookname)

ALTER TABLE hm_addressbooks ADD CONSTRAINT fk_hm_addressbooks_account FOREIGN KEY (bookaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

create table hm_addressbookshares
(
	shareid int identity(1,1) not null,
	sharebookid int not null,
	shareaccountid int not null,
	sharedomainid int not null,
	sharewrite int not null,
	sharecreated bigint not null
)

ALTER TABLE hm_addressbookshares ADD CONSTRAINT hm_addressbookshares_pk PRIMARY KEY (shareid)

CREATE UNIQUE INDEX idx_hm_addressbookshares_target ON hm_addressbookshares (sharebookid, shareaccountid, sharedomainid)

ALTER TABLE hm_addressbookshares ADD CONSTRAINT fk_hm_addressbookshares_book FOREIGN KEY (sharebookid) REFERENCES hm_addressbooks (bookid) ON DELETE CASCADE

ALTER TABLE hm_contacts ADD contactbookid int not null DEFAULT 0

CREATE INDEX idx_hm_contacts_book ON hm_contacts (contactbookid)

update hm_dbversion set value = 6079
