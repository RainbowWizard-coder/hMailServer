-- Schema 6107: the greylisting key is complete. Every row has had its key since the server's own
-- prerequisite (PreReqGreyListKeys) ran just before this script, so both key columns become NOT NULL
-- and the key unique. A unique index here would not hold over glipaddress2, which is NULL for every
-- IPv4 triplet and which MySQL and MariaDB count as distinct from every other NULL, so the index is
-- over glipaddress2key instead: the second half of the address, or 'v4' where there is none, a
-- stored generated column no statement writes. It leads with the address's first half and the two
-- keys, which is what the lookup asks for. Every statement can be run again, so a step stopped
-- part of the way - MySQL commits each ALTER as it runs - is finished by running it again.
DROP INDEX idx_greylisting_triplets ON hm_greylisting_triplets /* [IGNORE-ERRORS] */;

DROP INDEX idx_hm_greylisting_triplets ON hm_greylisting_triplets /* [IGNORE-ERRORS] */;

DROP INDEX idx_hm_greylisting_triplets_key ON hm_greylisting_triplets /* [IGNORE-ERRORS] */;

alter table hm_greylisting_triplets drop column glipaddress2key /* [IGNORE-ERRORS] */;

alter table hm_greylisting_triplets modify glsenderhash varchar(32) not null;

alter table hm_greylisting_triplets modify glrecipienthash varchar(32) not null;

alter table hm_greylisting_triplets add column glipaddress2key varchar(20) as (coalesce(cast(glipaddress2 as char(20)), 'v4')) stored;

CREATE UNIQUE INDEX idx_hm_greylisting_triplets_key ON hm_greylisting_triplets (glipaddress1, glsenderhash, glrecipienthash, glipaddress2key);

update hm_dbversion set value = 6107;
