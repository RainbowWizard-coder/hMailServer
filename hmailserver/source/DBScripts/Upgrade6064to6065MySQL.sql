alter table hm_fetchaccounts add column faoauthprovider varchar(16) not null default '';

alter table hm_fetchaccounts add column faoauthtoken text not null;

alter table hm_fetchaccounts add column famirrorskip text not null;

alter table hm_fetchaccounts add column famirrorseen text not null;

update hm_dbversion set value = 6065;
