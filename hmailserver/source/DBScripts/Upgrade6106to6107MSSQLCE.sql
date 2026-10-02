ALTER TABLE hm_greylisting_triplets DROP CONSTRAINT u_gltriplet --- [IGNORE-ERRORS]

DROP INDEX hm_greylisting_triplets.idx_hm_greylisting_triplets --- [IGNORE-ERRORS]

DROP INDEX hm_greylisting_triplets.idx_hm_greylisting_triplets_key --- [IGNORE-ERRORS]

ALTER TABLE hm_greylisting_triplets ALTER COLUMN glsenderhash nvarchar(32) NOT NULL

ALTER TABLE hm_greylisting_triplets ALTER COLUMN glrecipienthash nvarchar(32) NOT NULL

CREATE UNIQUE INDEX idx_hm_greylisting_triplets_key ON hm_greylisting_triplets (glipaddress1, glipaddress2, glsenderhash, glrecipienthash)

update hm_dbversion set value = 6107
