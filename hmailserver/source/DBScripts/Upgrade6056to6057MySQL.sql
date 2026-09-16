create table hm_reputationchecks
(
	rcid bigint auto_increment not null, primary key(`rcid`), unique(`rcid`),
	rcaddress varchar(64) not null,
	rcsources varchar(64) not null,
	rczone varchar(255) not null,
	rcstatus int not null,
	rccode varchar(255) not null,
	rcreason varchar(255) not null,
	rcchecked bigint not null,
	rclistedsince bigint not null,
	rcconfirmed bigint not null
) DEFAULT CHARSET=utf8;

ALTER TABLE hm_reputationchecks ENGINE=InnoDB;

insert into hm_alertrules (alertrulecondition, alertruleenabled, alertrulethreshold, alertruleactions, alertruleaddress, alertrulewebhook, alertrulesecret, alertrulecooldown, alertruledigest) select 'reputation.listed', 1, 0, 1, '', '', '', 1440, 0 from hm_dbversion where not exists (select alertrulecondition from hm_alertrules where alertrulecondition = 'reputation.listed');

insert into hm_settings (settingname, settingstring, settinginteger) select 'ReputationWatchEnabled', '', 0 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'ReputationWatchEnabled');

insert into hm_settings (settingname, settingstring, settinginteger) select 'ReputationWatchLists', 'ipv4:zen.spamhaus.org, ipv4:bl.spamcop.net, ipv4:psbl.surriel.com, ipv4:bl.mailspike.net', 0 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'ReputationWatchLists');

insert into hm_settings (settingname, settingstring, settinginteger) select 'ReputationWatchAddresses', '', 0 from hm_dbversion where not exists (select settingname from hm_settings where settingname = 'ReputationWatchAddresses');

update hm_dbversion set value = 6057;
