create table hm_pushsubscriptions
(
	psid bigint identity(1,1) not null,
	psaccountid int not null,
	psendpointhash nvarchar(64) not null,
	psendpoint nvarchar(1000) not null,
	pskeyp256dh nvarchar(128) not null,
	pskeyauth nvarchar(32) not null,
	psdevice nvarchar(64) not null,
	pscreated bigint not null,
	pslastsent bigint not null,
	psstate int not null,
	psfailures int not null,
	psnextattempt bigint not null,
	pslastdetail nvarchar(255) not null,
	psdetail int not null,
	pssenders nvarchar(255) not null,
	pslabels nvarchar(255) not null,
	psquietfrom int not null,
	psquietto int not null,
	psquietoffset int not null
)

ALTER TABLE hm_pushsubscriptions ADD CONSTRAINT hm_pushsubscriptions_pk PRIMARY KEY (psid)

CREATE UNIQUE INDEX idx_hm_pushsubscriptions_endpoint ON hm_pushsubscriptions (psaccountid, psendpointhash)

ALTER TABLE hm_pushsubscriptions ADD CONSTRAINT fk_hm_pushsubscriptions_account FOREIGN KEY (psaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE

insert into hm_settings (settingname, settingstring, settinginteger) select 'WebPushEnabled', '', 0 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'WebPushEnabled')

insert into hm_settings (settingname, settingstring, settinginteger) select 'WebPushSubject', '', 0 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'WebPushSubject')

insert into hm_settings (settingname, settingstring, settinginteger) select 'WebPushTimeToLiveSeconds', '', 86400 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'WebPushTimeToLiveSeconds')

insert into hm_settings (settingname, settingstring, settinginteger) select 'WebPushMaxPerAccount', '', 10 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'WebPushMaxPerAccount')

insert into hm_settings (settingname, settingstring, settinginteger) select 'WebPushMaxAttempts', '', 5 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'WebPushMaxAttempts')

insert into hm_settings (settingname, settingstring, settinginteger) select 'WebPushVapidPublicKey', '', 0 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'WebPushVapidPublicKey')

insert into hm_settings (settingname, settingstring, settinginteger) select 'WebPushVapidPrivateKey', '', 0 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'WebPushVapidPrivateKey')

update hm_dbversion set value = 6054
