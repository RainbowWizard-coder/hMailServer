create table hm_recall
(
	recallid serial not null primary key,
	recallsenderid int not null,
	recallmsgid varchar(255) not null,
	recallrecipientid int not null,
	recallqueueid bigint not null,
	recallmessageid bigint not null,
	recallcreated bigint not null,
	recallrecalledat bigint not null,
	recalloutcome int not null
);

CREATE INDEX idx_hm_recall_sender ON hm_recall (recallsenderid);

CREATE INDEX idx_hm_recall_queue ON hm_recall (recallqueueid);

ALTER TABLE hm_recall ADD CONSTRAINT fk_hm_recall_sender FOREIGN KEY (recallsenderid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

insert into hm_settings (settingname, settingstring, settinginteger) select 'RecallDays', '', 14 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'RecallDays');

update hm_dbversion set value = 6085;
