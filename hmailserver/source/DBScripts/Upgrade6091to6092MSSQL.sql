ALTER TABLE hm_distributionlists ADD distributionlistmanaged int not null CONSTRAINT df_hm_dl_managed DEFAULT 0

ALTER TABLE hm_distributionlists ADD distributionlistsubscribe int not null CONSTRAINT df_hm_dl_subscribe DEFAULT 0

ALTER TABLE hm_distributionlists ADD distributionlistarchive int not null CONSTRAINT df_hm_dl_archive DEFAULT 0

ALTER TABLE hm_distributionlists ADD distributionlistarchivedays int not null CONSTRAINT df_hm_dl_archivedays DEFAULT 0

ALTER TABLE hm_distributionlists ADD distributionlistdigest int not null CONSTRAINT df_hm_dl_digest DEFAULT 0

ALTER TABLE hm_distributionlists ADD distributionlistdigesthour int not null CONSTRAINT df_hm_dl_digesthour DEFAULT 7

ALTER TABLE hm_distributionlists ADD distributionlistdigestlastid bigint not null CONSTRAINT df_hm_dl_digestlastid DEFAULT 0

ALTER TABLE hm_distributionlists ADD distributionlistdigestlasttime bigint not null CONSTRAINT df_hm_dl_digestlasttime DEFAULT 0

ALTER TABLE hm_distributionlists ADD distributionlistreplyto int not null CONSTRAINT df_hm_dl_replyto DEFAULT 0

ALTER TABLE hm_distributionlists ADD distributionlistprefix nvarchar(64) not null CONSTRAINT df_hm_dl_prefix DEFAULT ''

ALTER TABLE hm_distributionlists ADD distributionlistfooter nvarchar(2000) not null CONSTRAINT df_hm_dl_footer DEFAULT ''

ALTER TABLE hm_distributionlists ADD distributionlistverp int not null CONSTRAINT df_hm_dl_verp DEFAULT 0

ALTER TABLE hm_distributionlists ADD distributionlistowner nvarchar(255) not null CONSTRAINT df_hm_dl_owner DEFAULT ''

ALTER TABLE hm_distributionlists ADD distributionlistsecret nvarchar(64) not null CONSTRAINT df_hm_dl_secret DEFAULT ''

ALTER TABLE hm_distributionlistsrecipients ADD distributionlistrecipientdelivery int not null CONSTRAINT df_hm_dlr_delivery DEFAULT 0

ALTER TABLE hm_distributionlistsrecipients ADD distributionlistrecipientsubscribed bigint not null CONSTRAINT df_hm_dlr_subscribed DEFAULT 0

ALTER TABLE hm_distributionlistsrecipients ADD distributionlistrecipientconfirmed int not null CONSTRAINT df_hm_dlr_confirmed DEFAULT 1

ALTER TABLE hm_distributionlistsrecipients ADD distributionlistrecipienttoken nvarchar(64) not null CONSTRAINT df_hm_dlr_token DEFAULT ''

ALTER TABLE hm_distributionlistsrecipients ADD distributionlistrecipienthardbounces int not null CONSTRAINT df_hm_dlr_hardbounces DEFAULT 0

ALTER TABLE hm_distributionlistsrecipients ADD distributionlistrecipientsoftbounces int not null CONSTRAINT df_hm_dlr_softbounces DEFAULT 0

ALTER TABLE hm_distributionlistsrecipients ADD distributionlistrecipientsoftsince bigint not null CONSTRAINT df_hm_dlr_softsince DEFAULT 0

ALTER TABLE hm_distributionlistsrecipients ADD distributionlistrecipientlastbounce bigint not null CONSTRAINT df_hm_dlr_lastbounce DEFAULT 0

ALTER TABLE hm_distributionlistsrecipients ADD distributionlistrecipientreason nvarchar(255) not null CONSTRAINT df_hm_dlr_reason DEFAULT ''

CREATE INDEX idx_hm_dlrecipients_token ON hm_distributionlistsrecipients (distributionlistrecipienttoken)

create table hm_listarchive
(
	laid bigint identity(1,1) not null,
	lalistid int not null,
	lamessageid nvarchar(255) not null,
	lasubject nvarchar(255) not null,
	lasender nvarchar(255) not null,
	lasent bigint not null,
	lasize int not null,
	lafile nvarchar(255) not null,
	lamonth nvarchar(7) not null,
	laarchived int not null
)

ALTER TABLE hm_listarchive ADD CONSTRAINT hm_listarchive_pk PRIMARY KEY NONCLUSTERED (laid)

CREATE INDEX idx_hm_listarchive_list ON hm_listarchive (lalistid, lasent)

CREATE INDEX idx_hm_listarchive_month ON hm_listarchive (lalistid, lamonth)

ALTER TABLE hm_listarchive ADD CONSTRAINT fk_hm_listarchive_list FOREIGN KEY (lalistid) REFERENCES hm_distributionlists (distributionlistid) ON DELETE CASCADE

insert into hm_servermessages (smname, smtext) select 'LIST_CONFIRM_SUBJECT', 'Confirm your subscription to %MACRO_LIST%' from hm_dbversion where not exists (select smname from hm_servermessages where smname = 'LIST_CONFIRM_SUBJECT')

insert into hm_servermessages (smname, smtext) select 'LIST_CONFIRM', 'Somebody - probably you - asked for %MACRO_ADDRESS% to be subscribed to the list %MACRO_LIST%.' + NCHAR(13) + NCHAR(10) + '' + NCHAR(13) + NCHAR(10) + 'To confirm, reply to this message without changing its subject, or open this page and press the button on it:' + NCHAR(13) + NCHAR(10) + '' + NCHAR(13) + NCHAR(10) + '   %MACRO_LINK%' + NCHAR(13) + NCHAR(10) + '' + NCHAR(13) + NCHAR(10) + 'If you did not ask for this, ignore this message. Nothing happens unless you confirm, and the request lapses in a week.' + NCHAR(13) + NCHAR(10) + '' + NCHAR(13) + NCHAR(10) + 'hMailServer' + NCHAR(13) + NCHAR(10) from hm_dbversion where not exists (select smname from hm_servermessages where smname = 'LIST_CONFIRM')

insert into hm_servermessages (smname, smtext) select 'LIST_REFUSED_SUBJECT', 'Your request to %MACRO_LIST%' from hm_dbversion where not exists (select smname from hm_servermessages where smname = 'LIST_REFUSED_SUBJECT')

insert into hm_servermessages (smname, smtext) select 'LIST_REFUSED', 'The list %MACRO_LIST% does not take subscriptions by mail or by page: its members are added by the person who runs it. Write to %MACRO_OWNER% to ask.' + NCHAR(13) + NCHAR(10) + '' + NCHAR(13) + NCHAR(10) + 'hMailServer' + NCHAR(13) + NCHAR(10) from hm_dbversion where not exists (select smname from hm_servermessages where smname = 'LIST_REFUSED')

insert into hm_servermessages (smname, smtext) select 'LIST_HELP_SUBJECT', 'The addresses of the list %MACRO_LIST%' from hm_dbversion where not exists (select smname from hm_servermessages where smname = 'LIST_HELP_SUBJECT')

insert into hm_servermessages (smname, smtext) select 'LIST_HELP', 'The list %MACRO_LIST% answers these addresses:' + NCHAR(13) + NCHAR(10) + '' + NCHAR(13) + NCHAR(10) + '   %MACRO_LIST%' + NCHAR(13) + NCHAR(10) + '      post to the list' + NCHAR(13) + NCHAR(10) + '   %MACRO_SUBSCRIBE%' + NCHAR(13) + NCHAR(10) + '      join the list (you are sent a message to confirm)' + NCHAR(13) + NCHAR(10) + '   %MACRO_UNSUBSCRIBE%' + NCHAR(13) + NCHAR(10) + '      leave the list (write from the address you are subscribed with; you are sent a message to confirm)' + NCHAR(13) + NCHAR(10) + '   %MACRO_REQUEST%' + NCHAR(13) + NCHAR(10) + '      this help' + NCHAR(13) + NCHAR(10) + '   %MACRO_OWNERADDRESS%' + NCHAR(13) + NCHAR(10) + '      the person who runs the list' + NCHAR(13) + NCHAR(10) + '' + NCHAR(13) + NCHAR(10) + 'Every message from the list also carries a link at which you can leave it, or choose a daily digest, or pause it.' + NCHAR(13) + NCHAR(10) + '' + NCHAR(13) + NCHAR(10) + 'hMailServer' + NCHAR(13) + NCHAR(10) from hm_dbversion where not exists (select smname from hm_servermessages where smname = 'LIST_HELP')

insert into hm_servermessages (smname, smtext) select 'LIST_APPROVE_SUBJECT', '%MACRO_ADDRESS% asks to join %MACRO_LIST%' from hm_dbversion where not exists (select smname from hm_servermessages where smname = 'LIST_APPROVE_SUBJECT')

insert into hm_servermessages (smname, smtext) select 'LIST_APPROVE', '%MACRO_ADDRESS% has asked to join the list %MACRO_LIST% and has confirmed that the address is theirs.' + NCHAR(13) + NCHAR(10) + '' + NCHAR(13) + NCHAR(10) + 'To let them in, open this page while you are signed in to this server''s webmail as %MACRO_OWNER%, and press the button on it:' + NCHAR(13) + NCHAR(10) + '' + NCHAR(13) + NCHAR(10) + '   %MACRO_LINK%' + NCHAR(13) + NCHAR(10) + '' + NCHAR(13) + NCHAR(10) + 'Or approve them in the Control Deck, in the list''s member table.' + NCHAR(13) + NCHAR(10) + '' + NCHAR(13) + NCHAR(10) + 'To refuse, do nothing: the request lapses in a week.' + NCHAR(13) + NCHAR(10) + '' + NCHAR(13) + NCHAR(10) + 'hMailServer' + NCHAR(13) + NCHAR(10) from hm_dbversion where not exists (select smname from hm_servermessages where smname = 'LIST_APPROVE')

insert into hm_servermessages (smname, smtext) select 'LIST_BOUNCED_SUBJECT', 'The list %MACRO_LIST% has stopped mailing %MACRO_ADDRESS%' from hm_dbversion where not exists (select smname from hm_servermessages where smname = 'LIST_BOUNCED_SUBJECT')

insert into hm_servermessages (smname, smtext) select 'LIST_BOUNCED', 'Mail from the list %MACRO_LIST% to %MACRO_ADDRESS% kept coming back, so that address is no longer sent the list''s mail. It is still a member.' + NCHAR(13) + NCHAR(10) + '' + NCHAR(13) + NCHAR(10) + '   %MACRO_REASON%' + NCHAR(13) + NCHAR(10) + '' + NCHAR(13) + NCHAR(10) + 'When the address works again, reset its bounces in the Control Deck (the list''s member table): it is then sent the list''s mail again. The member can also turn it back on from the link in any message of the list.' + NCHAR(13) + NCHAR(10) + '' + NCHAR(13) + NCHAR(10) + 'hMailServer' + NCHAR(13) + NCHAR(10) from hm_dbversion where not exists (select smname from hm_servermessages where smname = 'LIST_BOUNCED')

insert into hm_servermessages (smname, smtext) select 'LIST_LEAVE_SUBJECT', 'Confirm that you want to leave %MACRO_LIST%' from hm_dbversion where not exists (select smname from hm_servermessages where smname = 'LIST_LEAVE_SUBJECT')

insert into hm_servermessages (smname, smtext) select 'LIST_LEAVE', 'Somebody - probably you - asked for %MACRO_ADDRESS% to be taken off the list %MACRO_LIST%.' + NCHAR(13) + NCHAR(10) + '' + NCHAR(13) + NCHAR(10) + 'To leave, reply to this message without changing its subject, or open this page and press the button on it:' + NCHAR(13) + NCHAR(10) + '' + NCHAR(13) + NCHAR(10) + '   %MACRO_LINK%' + NCHAR(13) + NCHAR(10) + '' + NCHAR(13) + NCHAR(10) + 'If you did not ask for this, ignore this message: you stay on the list, and nothing changes.' + NCHAR(13) + NCHAR(10) + '' + NCHAR(13) + NCHAR(10) + 'hMailServer' + NCHAR(13) + NCHAR(10) from hm_dbversion where not exists (select smname from hm_servermessages where smname = 'LIST_LEAVE')

insert into hm_servermessages (smname, smtext) select 'LIST_UNSUBSCRIBED_SUBJECT', 'You have left %MACRO_LIST%' from hm_dbversion where not exists (select smname from hm_servermessages where smname = 'LIST_UNSUBSCRIBED_SUBJECT')

insert into hm_servermessages (smname, smtext) select 'LIST_UNSUBSCRIBED', '%MACRO_ADDRESS% has been taken off the list %MACRO_LIST%. It will not be sent the list''s mail any more.' + NCHAR(13) + NCHAR(10) + '' + NCHAR(13) + NCHAR(10) + 'hMailServer' + NCHAR(13) + NCHAR(10) from hm_dbversion where not exists (select smname from hm_servermessages where smname = 'LIST_UNSUBSCRIBED')

update hm_dbversion set value = 6092
