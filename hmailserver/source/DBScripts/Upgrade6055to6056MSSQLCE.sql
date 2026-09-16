create table hm_mailboxmoves
(
	mmid bigint identity(1,1) not null,
	mmaccountid int not null,
	mmoldaddress nvarchar(255) not null,
	mmnewaddress nvarchar(255) not null,
	mmstage int not null,
	mmkeepalias int not null,
	mmmessages int not null,
	mmstarted nvarchar(32) not null,
	mmfinished nvarchar(32) not null,
	mmdetail nvarchar(255) not null
)

ALTER TABLE hm_mailboxmoves ADD CONSTRAINT hm_mailboxmoves_pk PRIMARY KEY (mmid)

CREATE INDEX idx_hm_mailboxmoves_account ON hm_mailboxmoves (mmaccountid)

ALTER TABLE hm_mailboxmoves ADD CONSTRAINT fk_hm_mailboxmoves_account FOREIGN KEY (mmaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

update hm_dbversion set value = 6056
