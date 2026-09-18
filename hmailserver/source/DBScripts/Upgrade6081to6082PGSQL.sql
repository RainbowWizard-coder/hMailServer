create table hm_protectedmessages
(
	pmid bigserial not null primary key,
	pmaccountid int not null,
	pmtoken varchar(32) not null,
	pmrecipient varchar(255) not null,
	pmhint varchar(255) not null,
	pmfile varchar(255) not null,
	pmsize bigint not null,
	pmstored bigint not null,
	pmcomplete int not null,
	pmcreated bigint not null,
	pmexpires bigint not null,
	pmopened bigint not null,
	pmrevoked int not null,
	pmreplyto varchar(32) not null
);

CREATE UNIQUE INDEX idx_hm_protectedmessages_token ON hm_protectedmessages (pmtoken);

CREATE INDEX idx_hm_protectedmessages_account ON hm_protectedmessages (pmaccountid);

ALTER TABLE hm_protectedmessages ADD CONSTRAINT fk_hm_protectedmessages_account FOREIGN KEY (pmaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

insert into hm_settings (settingname, settingstring, settinginteger) select 'ProtectedMessageDays', '', 30 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'ProtectedMessageDays');

update hm_dbversion set value = 6082;
