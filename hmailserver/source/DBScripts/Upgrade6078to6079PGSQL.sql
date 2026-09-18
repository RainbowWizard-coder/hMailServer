create table hm_addressbooks
(
	bookid serial not null primary key,
	bookaccountid int not null,
	bookname varchar(255) not null,
	bookcreated bigint not null
);

CREATE UNIQUE INDEX idx_hm_addressbooks_account ON hm_addressbooks (bookaccountid, bookname);

ALTER TABLE hm_addressbooks ADD CONSTRAINT fk_hm_addressbooks_account FOREIGN KEY (bookaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

create table hm_addressbookshares
(
	shareid serial not null primary key,
	sharebookid int not null,
	shareaccountid int not null,
	sharedomainid int not null,
	sharewrite int not null,
	sharecreated bigint not null
);

CREATE UNIQUE INDEX idx_hm_addressbookshares_target ON hm_addressbookshares (sharebookid, shareaccountid, sharedomainid);

ALTER TABLE hm_addressbookshares ADD CONSTRAINT fk_hm_addressbookshares_book FOREIGN KEY (sharebookid) REFERENCES hm_addressbooks (bookid) ON DELETE CASCADE;

alter table hm_contacts add column contactbookid int not null default 0;

CREATE INDEX idx_hm_contacts_book ON hm_contacts (contactbookid);

update hm_dbversion set value = 6079;
