LOAD 'plantuner';

SHOW	plantuner.forbid_index;

CREATE TABLE wow (i int, j int);
CREATE INDEX i_idx ON wow (i);
CREATE INDEX j_idx ON wow (j);

SET enable_seqscan=off;

SELECT * FROM wow;

SET plantuner.forbid_index="i_idx, j_idx";

SELECT * FROM wow;

SHOW plantuner.forbid_index;

SET plantuner.forbid_index="i_idx, nonexistent, public.j_idx, wow";

SHOW plantuner.forbid_index;
