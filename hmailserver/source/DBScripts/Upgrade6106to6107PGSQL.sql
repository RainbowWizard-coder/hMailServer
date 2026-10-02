drop index if exists idx_greylisting_triplets;

drop index if exists idx_hm_greylisting_triplets;

drop index if exists idx_hm_greylisting_triplets_key;

drop index if exists idx_hm_greylisting_triplets_key4;

alter table hm_greylisting_triplets alter column glsenderhash set not null;

alter table hm_greylisting_triplets alter column glrecipienthash set not null;

create unique index idx_hm_greylisting_triplets_key on hm_greylisting_triplets (glipaddress1, glipaddress2, glsenderhash, glrecipienthash) where glipaddress2 is not null;

create unique index idx_hm_greylisting_triplets_key4 on hm_greylisting_triplets (glipaddress1, glsenderhash, glrecipienthash) where glipaddress2 is null;

update hm_dbversion set value = 6107;
