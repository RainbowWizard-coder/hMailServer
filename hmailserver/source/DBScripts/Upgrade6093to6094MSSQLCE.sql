ALTER TABLE hm_remotedomainpolicies ADD policyencrypt int not null DEFAULT 0

ALTER TABLE hm_remotedomainpolicies ADD policyencryptkey ntext not null DEFAULT ''

ALTER TABLE hm_remotedomainpolicies ADD policyencryptfallback int not null DEFAULT 0

update hm_dbversion set value = 6094
