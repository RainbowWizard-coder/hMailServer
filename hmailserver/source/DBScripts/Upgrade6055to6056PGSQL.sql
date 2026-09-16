create table hm_mailboxmoves
(
	mmid bigserial not null primary key,
	mmaccountid int not null,
	mmoldaddress varchar(255) not null,
	mmnewaddress varchar(255) not null,
	mmstage int not null,
	mmkeepalias int not null,
	mmmessages int not null,
	mmstarted varchar(32) not null,
	mmfinished varchar(32) not null,
	mmdetail varchar(255) not null
);

CREATE INDEX idx_hm_mailboxmoves_account ON hm_mailboxmoves (mmaccountid);

ALTER TABLE hm_mailboxmoves ADD CONSTRAINT fk_hm_mailboxmoves_account FOREIGN KEY (mmaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

update hm_dbversion set value = 6056;
