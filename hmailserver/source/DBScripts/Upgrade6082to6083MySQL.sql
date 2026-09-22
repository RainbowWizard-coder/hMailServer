alter table hm_accounts add column accountzeroaccess int not null default 0;

create table hm_zeroaccess
(
	zaid int auto_increment not null, primary key(`zaid`), unique(`zaid`),
	zaaccountid int not null,
	zafingerprint varchar(64) not null,
	zasalt varchar(64) not null,
	zapasses int not null,
	zamemory int not null,
	zalanes int not null,
	zasecret mediumtext not null,
	zarecovery mediumtext not null,
	zaenabledat bigint not null
) DEFAULT CHARSET=utf8;

CREATE UNIQUE INDEX idx_hm_zeroaccess_account ON hm_zeroaccess (zaaccountid);

ALTER TABLE hm_zeroaccess ENGINE=InnoDB;

ALTER TABLE hm_zeroaccess ADD CONSTRAINT fk_hm_zeroaccess_account FOREIGN KEY (zaaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

insert into hm_settings (settingname, settingstring, settinginteger) select 'ZeroAccessAllowed', '', 0 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'ZeroAccessAllowed');

update hm_dbversion set value = 6083;
