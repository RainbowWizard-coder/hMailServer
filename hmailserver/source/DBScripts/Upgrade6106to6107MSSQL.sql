-- No DROP CONSTRAINT u_gltriplet here, unlike the Compact script: step 6072 dropped that
-- constraint, so no database at 6106 has it, and on SQL Server a failed ALTER TABLE ... DROP
-- CONSTRAINT (3728/3727) rolls back the walker's whole transaction, even with the statement
-- marked to ignore its error and XACT_ABORT OFF - DBUpdater's single transaction for the upgrade
-- among them - and the statements after it then commit one by one. A failed DROP INDEX
-- (3701) leaves the transaction open.
DROP INDEX idx_hm_greylisting_triplets ON hm_greylisting_triplets --- [IGNORE-ERRORS]

DROP INDEX idx_hm_greylisting_triplets_key ON hm_greylisting_triplets --- [IGNORE-ERRORS]

ALTER TABLE hm_greylisting_triplets ALTER COLUMN glsenderhash nvarchar(32) NOT NULL

ALTER TABLE hm_greylisting_triplets ALTER COLUMN glrecipienthash nvarchar(32) NOT NULL

CREATE UNIQUE INDEX idx_hm_greylisting_triplets_key ON hm_greylisting_triplets (glipaddress1, glipaddress2, glsenderhash, glrecipienthash)

update hm_dbversion set value = 6107
