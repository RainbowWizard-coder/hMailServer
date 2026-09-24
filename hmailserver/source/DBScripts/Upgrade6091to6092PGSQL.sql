alter table hm_distributionlists add column distributionlistmanaged int not null default 0;

alter table hm_distributionlists add column distributionlistsubscribe int not null default 0;

alter table hm_distributionlists add column distributionlistarchive int not null default 0;

alter table hm_distributionlists add column distributionlistarchivedays int not null default 0;

alter table hm_distributionlists add column distributionlistdigest int not null default 0;

alter table hm_distributionlists add column distributionlistdigesthour int not null default 7;

alter table hm_distributionlists add column distributionlistdigestlastid bigint not null default 0;

alter table hm_distributionlists add column distributionlistdigestlasttime bigint not null default 0;

alter table hm_distributionlists add column distributionlistreplyto int not null default 0;

alter table hm_distributionlists add column distributionlistprefix varchar(64) not null default '';

alter table hm_distributionlists add column distributionlistfooter varchar(2000) not null default '';

alter table hm_distributionlists add column distributionlistverp int not null default 0;

alter table hm_distributionlists add column distributionlistowner varchar(255) not null default '';

alter table hm_distributionlists add column distributionlistsecret varchar(64) not null default '';

alter table hm_distributionlistsrecipients add column distributionlistrecipientdelivery int not null default 0;

alter table hm_distributionlistsrecipients add column distributionlistrecipientsubscribed bigint not null default 0;

alter table hm_distributionlistsrecipients add column distributionlistrecipientconfirmed int not null default 1;

alter table hm_distributionlistsrecipients add column distributionlistrecipienttoken varchar(64) not null default '';

alter table hm_distributionlistsrecipients add column distributionlistrecipienthardbounces int not null default 0;

alter table hm_distributionlistsrecipients add column distributionlistrecipientsoftbounces int not null default 0;

alter table hm_distributionlistsrecipients add column distributionlistrecipientsoftsince bigint not null default 0;

alter table hm_distributionlistsrecipients add column distributionlistrecipientlastbounce bigint not null default 0;

alter table hm_distributionlistsrecipients add column distributionlistrecipientreason varchar(255) not null default '';

CREATE INDEX idx_hm_dlrecipients_token ON hm_distributionlistsrecipients (distributionlistrecipienttoken);

create table hm_listarchive
(
	laid bigserial not null primary key,
	lalistid int not null,
	lamessageid varchar(255) not null,
	lasubject varchar(255) not null,
	lasender varchar(255) not null,
	lasent bigint not null,
	lasize int not null,
	lafile varchar(255) not null,
	lamonth varchar(7) not null,
	laarchived int not null
);

CREATE INDEX idx_hm_listarchive_list ON hm_listarchive (lalistid, lasent);

CREATE INDEX idx_hm_listarchive_month ON hm_listarchive (lalistid, lamonth);

ALTER TABLE hm_listarchive ADD CONSTRAINT fk_hm_listarchive_list FOREIGN KEY (lalistid) REFERENCES hm_distributionlists (distributionlistid) ON DELETE CASCADE;

insert into hm_servermessages (smname, smtext) select 'LIST_CONFIRM_SUBJECT', 'Confirm your subscription to %MACRO_LIST%' from hm_dbversion where not exists (select smname from hm_servermessages where smname = 'LIST_CONFIRM_SUBJECT');

insert into hm_servermessages (smname, smtext) select 'LIST_CONFIRM', E'Somebody - probably you - asked for %MACRO_ADDRESS% to be subscribed to the list %MACRO_LIST%.\r\n\r\nTo confirm, reply to this message without changing its subject, or open this page and press the button on it:\r\n\r\n   %MACRO_LINK%\r\n\r\nIf you did not ask for this, ignore this message. Nothing happens unless you confirm, and the request lapses in a week.\r\n\r\nhMailServer\r\n' from hm_dbversion where not exists (select smname from hm_servermessages where smname = 'LIST_CONFIRM');

insert into hm_servermessages (smname, smtext) select 'LIST_REFUSED_SUBJECT', 'Your request to %MACRO_LIST%' from hm_dbversion where not exists (select smname from hm_servermessages where smname = 'LIST_REFUSED_SUBJECT');

insert into hm_servermessages (smname, smtext) select 'LIST_REFUSED', E'The list %MACRO_LIST% does not take subscriptions by mail or by page: its members are added by the person who runs it. Write to %MACRO_OWNER% to ask.\r\n\r\nhMailServer\r\n' from hm_dbversion where not exists (select smname from hm_servermessages where smname = 'LIST_REFUSED');

insert into hm_servermessages (smname, smtext) select 'LIST_HELP_SUBJECT', 'The addresses of the list %MACRO_LIST%' from hm_dbversion where not exists (select smname from hm_servermessages where smname = 'LIST_HELP_SUBJECT');

insert into hm_servermessages (smname, smtext) select 'LIST_HELP', E'The list %MACRO_LIST% answers these addresses:\r\n\r\n   %MACRO_LIST%\r\n      post to the list\r\n   %MACRO_SUBSCRIBE%\r\n      join the list (you are sent a message to confirm)\r\n   %MACRO_UNSUBSCRIBE%\r\n      leave the list (write from the address you are subscribed with; you are sent a message to confirm)\r\n   %MACRO_REQUEST%\r\n      this help\r\n   %MACRO_OWNERADDRESS%\r\n      the person who runs the list\r\n\r\nEvery message from the list also carries a link at which you can leave it, or choose a daily digest, or pause it.\r\n\r\nhMailServer\r\n' from hm_dbversion where not exists (select smname from hm_servermessages where smname = 'LIST_HELP');

insert into hm_servermessages (smname, smtext) select 'LIST_APPROVE_SUBJECT', '%MACRO_ADDRESS% asks to join %MACRO_LIST%' from hm_dbversion where not exists (select smname from hm_servermessages where smname = 'LIST_APPROVE_SUBJECT');

insert into hm_servermessages (smname, smtext) select 'LIST_APPROVE', E'%MACRO_ADDRESS% has asked to join the list %MACRO_LIST% and has confirmed that the address is theirs.\r\n\r\nTo let them in, open this page while you are signed in to this server''s webmail as %MACRO_OWNER%, and press the button on it:\r\n\r\n   %MACRO_LINK%\r\n\r\nOr approve them in the Control Deck, in the list''s member table.\r\n\r\nTo refuse, do nothing: the request lapses in a week.\r\n\r\nhMailServer\r\n' from hm_dbversion where not exists (select smname from hm_servermessages where smname = 'LIST_APPROVE');

insert into hm_servermessages (smname, smtext) select 'LIST_BOUNCED_SUBJECT', 'The list %MACRO_LIST% has stopped mailing %MACRO_ADDRESS%' from hm_dbversion where not exists (select smname from hm_servermessages where smname = 'LIST_BOUNCED_SUBJECT');

insert into hm_servermessages (smname, smtext) select 'LIST_BOUNCED', E'Mail from the list %MACRO_LIST% to %MACRO_ADDRESS% kept coming back, so that address is no longer sent the list''s mail. It is still a member.\r\n\r\n   %MACRO_REASON%\r\n\r\nWhen the address works again, reset its bounces in the Control Deck (the list''s member table): it is then sent the list''s mail again. The member can also turn it back on from the link in any message of the list.\r\n\r\nhMailServer\r\n' from hm_dbversion where not exists (select smname from hm_servermessages where smname = 'LIST_BOUNCED');

insert into hm_servermessages (smname, smtext) select 'LIST_LEAVE_SUBJECT', 'Confirm that you want to leave %MACRO_LIST%' from hm_dbversion where not exists (select smname from hm_servermessages where smname = 'LIST_LEAVE_SUBJECT');

insert into hm_servermessages (smname, smtext) select 'LIST_LEAVE', E'Somebody - probably you - asked for %MACRO_ADDRESS% to be taken off the list %MACRO_LIST%.\r\n\r\nTo leave, reply to this message without changing its subject, or open this page and press the button on it:\r\n\r\n   %MACRO_LINK%\r\n\r\nIf you did not ask for this, ignore this message: you stay on the list, and nothing changes.\r\n\r\nhMailServer\r\n' from hm_dbversion where not exists (select smname from hm_servermessages where smname = 'LIST_LEAVE');

insert into hm_servermessages (smname, smtext) select 'LIST_UNSUBSCRIBED_SUBJECT', 'You have left %MACRO_LIST%' from hm_dbversion where not exists (select smname from hm_servermessages where smname = 'LIST_UNSUBSCRIBED_SUBJECT');

insert into hm_servermessages (smname, smtext) select 'LIST_UNSUBSCRIBED', E'%MACRO_ADDRESS% has been taken off the list %MACRO_LIST%. It will not be sent the list''s mail any more.\r\n\r\nhMailServer\r\n' from hm_dbversion where not exists (select smname from hm_servermessages where smname = 'LIST_UNSUBSCRIBED');

update hm_dbversion set value = 6092;
