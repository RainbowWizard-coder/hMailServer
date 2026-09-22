create table hm_recall
(
	recallid int identity(1,1) not null,
	recallsenderid int not null,
	recallmsgid nvarchar(255) not null,
	recallrecipientid int not null,
	recallqueueid bigint not null,
	recallmessageid bigint not null,
	recallcreated bigint not null,
	recallrecalledat bigint not null,
	recalloutcome int not null
)

ALTER TABLE hm_recall ADD CONSTRAINT hm_recall_pk PRIMARY KEY (recallid)

CREATE INDEX idx_hm_recall_sender ON hm_recall (recallsenderid)

CREATE INDEX idx_hm_recall_queue ON hm_recall (recallqueueid)

ALTER TABLE hm_recall ADD CONSTRAINT fk_hm_recall_sender FOREIGN KEY (recallsenderid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

insert into hm_settings (settingname, settingstring, settinginteger) select 'RecallDays', '', 14 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'RecallDays')

update hm_dbversion set value = 6085
