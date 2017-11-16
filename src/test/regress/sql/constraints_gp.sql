--
-- CONSTRAINTS (GPDB-specific)
--

--
-- Regression test: ensure we don't trip any assertions when
-- optimizer_enable_dml_triggers is true
--
SET optimizer_enable_dml_triggers = true;
BEGIN;
CREATE TABLE fkc_primary_table1_gp(a int PRIMARY KEY, b text) DISTRIBUTED BY (a);
CREATE TABLE fkc_foreign_table1_gp(a int REFERENCES fkc_primary_table1_gp ON DELETE RESTRICT ON UPDATE RESTRICT, b text) DISTRIBUTED BY (a);
INSERT INTO fkc_primary_table1_gp VALUES (1, 'bar');
INSERT INTO fkc_foreign_table1_gp VALUES (1, 'bar');
COMMIT;
