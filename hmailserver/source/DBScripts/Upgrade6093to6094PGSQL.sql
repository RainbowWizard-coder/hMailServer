alter table hm_remotedomainpolicies add column policyencrypt int not null default 0;

alter table hm_remotedomainpolicies add column policyencryptkey text not null default '';

alter table hm_remotedomainpolicies add column policyencryptfallback int not null default 0;

update hm_dbversion set value = 6094;
