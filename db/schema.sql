-- ============================================================================
-- Schéma PostgreSQL : simulation épidémique parallèle (backend C)
-- ============================================================================

DROP TABLE IF EXISTS run_states;
DROP TABLE IF EXISTS runs;

CREATE TABLE runs (
    id                   SERIAL PRIMARY KEY,
    created_at           TIMESTAMPTZ NOT NULL DEFAULT now(),

    -- paramètres du modèle
    population           INTEGER NOT NULL,
    steps                INTEGER NOT NULL,
    domain_size          DOUBLE PRECISION NOT NULL,
    radius               DOUBLE PRECISION NOT NULL,
    beta                 DOUBLE PRECISION NOT NULL,
    gamma_infecte        DOUBLE PRECISION NOT NULL,
    gamma_isole          DOUBLE PRECISION NOT NULL,
    isolation_prob       DOUBLE PRECISION NOT NULL,
    min_days_isolation   INTEGER NOT NULL,
    isolation_effect     DOUBLE PRECISION NOT NULL,
    mobility             DOUBLE PRECISION NOT NULL,
    init_infected        INTEGER NOT NULL,
    seed                 BIGINT NOT NULL,
    threads              INTEGER NOT NULL,

    -- résultats
    exec_time_s          DOUBLE PRECISION NOT NULL,
    final_sain           INTEGER NOT NULL,
    final_infecte        INTEGER NOT NULL,
    final_isole          INTEGER NOT NULL,
    final_retabli        INTEGER NOT NULL
);

CREATE TABLE run_states (
    run_id      INTEGER NOT NULL REFERENCES runs(id) ON DELETE CASCADE,
    step        INTEGER NOT NULL,
    sain        INTEGER NOT NULL,
    infecte     INTEGER NOT NULL,
    isole       INTEGER NOT NULL,
    retabli     INTEGER NOT NULL,
    PRIMARY KEY (run_id, step)
);

CREATE INDEX idx_run_states_run_id ON run_states(run_id);
CREATE INDEX idx_runs_created_at ON runs(created_at DESC);
