create table hm_roles
(
	roleid bigserial not null primary key,
	rolename varchar(64) not null,
	roledescription varchar(255) not null,
	rolebuiltin smallint not null,
	rolepermissions varchar(1000) not null
);

CREATE UNIQUE INDEX idx_hm_roles_name ON hm_roles (rolename);

create table hm_roleassignments
(
	raid bigserial not null primary key,
	raaccountid int not null,
	raroleid int not null,
	radomains varchar(1000) not null
);

CREATE UNIQUE INDEX idx_hm_roleassignments_account ON hm_roleassignments (raaccountid);

ALTER TABLE hm_roleassignments ADD CONSTRAINT fk_hm_roleassignments_account FOREIGN KEY (raaccountid) REFERENCES hm_accounts (accountid) ON DELETE CASCADE;

ALTER TABLE hm_roleassignments ADD CONSTRAINT fk_hm_roleassignments_role FOREIGN KEY (raroleid) REFERENCES hm_roles (roleid);

insert into hm_roles (rolename, roledescription, rolebuiltin, rolepermissions) values ('serveradmin', 'Every permission, in every domain.', 1, 'administer_server,administer_domain,view_configuration,view_accounts,view_mailbox,view_queue,view_quarantine,view_archive,view_logs,view_message_trace,view_audit,manage_accounts,reset_password,unlock_account,manage_mailbox,manage_queue,manage_quarantine,manage_archive,manage_settings,manage_domains,manage_server,manage_roles');

insert into hm_roles (rolename, roledescription, rolebuiltin, rolepermissions) values ('domainadmin', 'The accounts, mailboxes and quarantine of named domains.', 1, 'administer_domain,view_configuration,view_accounts,view_mailbox,view_quarantine,view_message_trace,manage_accounts,reset_password,unlock_account,manage_mailbox,manage_quarantine');

insert into hm_roles (rolename, roledescription, rolebuiltin, rolepermissions) values ('helpdesk', 'Resets a password, unlocks an account and reads a message trace.', 1, 'view_accounts,view_message_trace,reset_password,unlock_account');

insert into hm_roles (rolename, roledescription, rolebuiltin, rolepermissions) values ('auditor', 'Reads everything and changes nothing, the audit trail included.', 1, 'view_configuration,view_accounts,view_mailbox,view_queue,view_quarantine,view_archive,view_logs,view_message_trace,view_audit');

update hm_dbversion set value = 6048;