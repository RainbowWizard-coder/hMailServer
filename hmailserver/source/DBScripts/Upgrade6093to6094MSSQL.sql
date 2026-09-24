ALTER TABLE hm_remotedomainpolicies ADD policyencrypt int not null CONSTRAINT df_hm_remotedomainpolicies_encrypt DEFAULT 0

ALTER TABLE hm_remotedomainpolicies ADD policyencryptkey ntext not null CONSTRAINT df_hm_remotedomainpolicies_encryptkey DEFAULT ''

ALTER TABLE hm_remotedomainpolicies ADD policyencryptfallback int not null CONSTRAINT df_hm_remotedomainpolicies_encryptfallback DEFAULT 0

update hm_dbversion set value = 6094
