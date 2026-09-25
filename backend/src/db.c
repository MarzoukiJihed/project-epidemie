#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jansson.h>
#include "db.h"

PGconn *db_connect(const char *conninfo) {
    PGconn *conn = PQconnectdb(conninfo);
    if (PQstatus(conn) != CONNECTION_OK) {
        fprintf(stderr, "[db] connexion echouee: %s\n", PQerrorMessage(conn));
        PQfinish(conn);
        return NULL;
    }
    return conn;
}

void db_disconnect(PGconn *conn) {
    if (conn) PQfinish(conn);
}

/* ---- helpers de conversion vers texte (libpq attend des parametres texte
   quand on utilise PQexecParams sans types OID explicites) ---- */
static char *dtoa_buf(double v) {
    char *b = malloc(64);
    snprintf(b, 64, "%.10g", v);
    return b;
}
static char *itoa_buf(long v) {
    char *b = malloc(32);
    snprintf(b, 32, "%ld", v);
    return b;
}

long db_insert_run(PGconn *conn, const SimParams *p, double exec_time_s,
                    const Counts *c) {
    const char *sql =
        "INSERT INTO runs (population, steps, domain_size, radius, beta, "
        "gamma_infecte, gamma_isole, isolation_prob, min_days_isolation, "
        "isolation_effect, mobility, init_infected, seed, threads, "
        "exec_time_s, final_sain, final_infecte, final_isole, final_retabli) "
        "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15,$16,$17,$18,$19) "
        "RETURNING id";

    char *vals[19];
    vals[0]  = itoa_buf(p->population);
    vals[1]  = itoa_buf(p->steps);
    vals[2]  = dtoa_buf(p->domain_size);
    vals[3]  = dtoa_buf(p->radius);
    vals[4]  = dtoa_buf(p->beta);
    vals[5]  = dtoa_buf(p->gamma_infecte);
    vals[6]  = dtoa_buf(p->gamma_isole);
    vals[7]  = dtoa_buf(p->isolation_prob);
    vals[8]  = itoa_buf(p->min_days_isolation);
    vals[9]  = dtoa_buf(p->isolation_effect);
    vals[10] = dtoa_buf(p->mobility);
    vals[11] = itoa_buf(p->init_infected);
    vals[12] = itoa_buf(p->seed);
    vals[13] = itoa_buf(p->threads);
    vals[14] = dtoa_buf(exec_time_s);
    vals[15] = itoa_buf(c->sain);
    vals[16] = itoa_buf(c->infecte);
    vals[17] = itoa_buf(c->isole);
    vals[18] = itoa_buf(c->retabli);

    PGresult *res = PQexecParams(conn, sql, 19, NULL,
                                  (const char *const *)vals, NULL, NULL, 0);
    long id = -1;
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) == 1) {
        id = atol(PQgetvalue(res, 0, 0));
    } else {
        fprintf(stderr, "[db] insert run echoue: %s\n", PQerrorMessage(conn));
    }
    for (int i = 0; i < 19; i++) free(vals[i]);
    PQclear(res);
    return id;
}

int db_insert_states(PGconn *conn, long run_id, const StepRow *rows, int n) {
    if (n <= 0) return 1;

    /* Construction d'un INSERT multi-valeurs unique : beaucoup plus rapide
       qu'une requete par ligne (une seule aller-retour reseau/protocole). */
    size_t cap = 128 + (size_t)n * 48;
    char *sql = malloc(cap);
    size_t off = (size_t)snprintf(sql, cap,
        "INSERT INTO run_states (run_id, step, sain, infecte, isole, retabli) VALUES ");

    for (int i = 0; i < n; i++) {
        off += (size_t)snprintf(sql + off, cap - off, "%s(%ld,%d,%d,%d,%d,%d)",
                                 i == 0 ? "" : ",",
                                 run_id, rows[i].step, rows[i].sain,
                                 rows[i].infecte, rows[i].isole, rows[i].retabli);
        if (off >= cap - 64) {
            cap *= 2;
            sql = realloc(sql, cap);
        }
    }

    PGresult *res = PQexec(conn, sql);
    int ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    if (!ok) fprintf(stderr, "[db] insert states echoue: %s\n", PQerrorMessage(conn));
    PQclear(res);
    free(sql);
    return ok;
}

int db_delete_run(PGconn *conn, long run_id) {
    const char *sql = "DELETE FROM runs WHERE id = $1";
    char *vals[1] = { itoa_buf(run_id) };
    PGresult *res = PQexecParams(conn, sql, 1, NULL, (const char *const *)vals, NULL, NULL, 0);
    int ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    int affected = ok ? atoi(PQcmdTuples(res)) : 0;
    free(vals[0]);
    PQclear(res);
    return affected > 0;
}

char *db_list_runs_json(PGconn *conn, int limit) {
    const char *sql =
        "SELECT id, created_at, population, steps, threads, exec_time_s, "
        "beta, mobility, radius, isolation_prob, "
        "final_sain, final_infecte, final_isole, final_retabli "
        "FROM runs ORDER BY created_at DESC LIMIT $1";
    char *vals[1] = { itoa_buf(limit) };
    PGresult *res = PQexecParams(conn, sql, 1, NULL, (const char *const *)vals, NULL, NULL, 0);
    free(vals[0]);

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        fprintf(stderr, "[db] list runs echoue: %s\n", PQerrorMessage(conn));
        PQclear(res);
        return NULL;
    }

    json_t *arr = json_array();
    int n = PQntuples(res);
    for (int i = 0; i < n; i++) {
        json_t *o = json_object();
        json_object_set_new(o, "id", json_integer(atol(PQgetvalue(res, i, 0))));
        json_object_set_new(o, "created_at", json_string(PQgetvalue(res, i, 1)));
        json_object_set_new(o, "population", json_integer(atol(PQgetvalue(res, i, 2))));
        json_object_set_new(o, "steps", json_integer(atol(PQgetvalue(res, i, 3))));
        json_object_set_new(o, "threads", json_integer(atol(PQgetvalue(res, i, 4))));
        json_object_set_new(o, "exec_time_s", json_real(atof(PQgetvalue(res, i, 5))));
        json_object_set_new(o, "beta", json_real(atof(PQgetvalue(res, i, 6))));
        json_object_set_new(o, "mobility", json_real(atof(PQgetvalue(res, i, 7))));
        json_object_set_new(o, "radius", json_real(atof(PQgetvalue(res, i, 8))));
        json_object_set_new(o, "isolation_prob", json_real(atof(PQgetvalue(res, i, 9))));
        json_object_set_new(o, "final_sain", json_integer(atol(PQgetvalue(res, i, 10))));
        json_object_set_new(o, "final_infecte", json_integer(atol(PQgetvalue(res, i, 11))));
        json_object_set_new(o, "final_isole", json_integer(atol(PQgetvalue(res, i, 12))));
        json_object_set_new(o, "final_retabli", json_integer(atol(PQgetvalue(res, i, 13))));
        json_array_append_new(arr, o);
    }
    PQclear(res);

    char *out = json_dumps(arr, JSON_COMPACT);
    json_decref(arr);
    return out;
}

char *db_get_run_json(PGconn *conn, long run_id) {
    const char *sql_run =
        "SELECT id, created_at, population, steps, domain_size, radius, beta, "
        "gamma_infecte, gamma_isole, isolation_prob, min_days_isolation, "
        "isolation_effect, mobility, init_infected, seed, threads, exec_time_s, "
        "final_sain, final_infecte, final_isole, final_retabli "
        "FROM runs WHERE id = $1";
    char *vid = itoa_buf(run_id);
    char *vals[1] = { vid };
    PGresult *res = PQexecParams(conn, sql_run, 1, NULL, (const char *const *)vals, NULL, NULL, 0);

    if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) != 1) {
        PQclear(res);
        free(vid);
        return NULL;
    }

    json_t *o = json_object();
    json_object_set_new(o, "id", json_integer(atol(PQgetvalue(res, 0, 0))));
    json_object_set_new(o, "created_at", json_string(PQgetvalue(res, 0, 1)));
    json_object_set_new(o, "population", json_integer(atol(PQgetvalue(res, 0, 2))));
    json_object_set_new(o, "steps", json_integer(atol(PQgetvalue(res, 0, 3))));
    json_object_set_new(o, "domain_size", json_real(atof(PQgetvalue(res, 0, 4))));
    json_object_set_new(o, "radius", json_real(atof(PQgetvalue(res, 0, 5))));
    json_object_set_new(o, "beta", json_real(atof(PQgetvalue(res, 0, 6))));
    json_object_set_new(o, "gamma_infecte", json_real(atof(PQgetvalue(res, 0, 7))));
    json_object_set_new(o, "gamma_isole", json_real(atof(PQgetvalue(res, 0, 8))));
    json_object_set_new(o, "isolation_prob", json_real(atof(PQgetvalue(res, 0, 9))));
    json_object_set_new(o, "min_days_isolation", json_integer(atol(PQgetvalue(res, 0, 10))));
    json_object_set_new(o, "isolation_effect", json_real(atof(PQgetvalue(res, 0, 11))));
    json_object_set_new(o, "mobility", json_real(atof(PQgetvalue(res, 0, 12))));
    json_object_set_new(o, "init_infected", json_integer(atol(PQgetvalue(res, 0, 13))));
    json_object_set_new(o, "seed", json_integer(atol(PQgetvalue(res, 0, 14))));
    json_object_set_new(o, "threads", json_integer(atol(PQgetvalue(res, 0, 15))));
    json_object_set_new(o, "exec_time_s", json_real(atof(PQgetvalue(res, 0, 16))));
    json_object_set_new(o, "final_sain", json_integer(atol(PQgetvalue(res, 0, 17))));
    json_object_set_new(o, "final_infecte", json_integer(atol(PQgetvalue(res, 0, 18))));
    json_object_set_new(o, "final_isole", json_integer(atol(PQgetvalue(res, 0, 19))));
    json_object_set_new(o, "final_retabli", json_integer(atol(PQgetvalue(res, 0, 20))));
    PQclear(res);

    const char *sql_states =
        "SELECT step, sain, infecte, isole, retabli FROM run_states "
        "WHERE run_id = $1 ORDER BY step ASC";
    PGresult *res2 = PQexecParams(conn, sql_states, 1, NULL, (const char *const *)vals, NULL, NULL, 0);
    free(vid);

    json_t *series = json_array();
    if (PQresultStatus(res2) == PGRES_TUPLES_OK) {
        int n = PQntuples(res2);
        for (int i = 0; i < n; i++) {
            json_t *row = json_object();
            json_object_set_new(row, "step", json_integer(atol(PQgetvalue(res2, i, 0))));
            json_object_set_new(row, "sain", json_integer(atol(PQgetvalue(res2, i, 1))));
            json_object_set_new(row, "infecte", json_integer(atol(PQgetvalue(res2, i, 2))));
            json_object_set_new(row, "isole", json_integer(atol(PQgetvalue(res2, i, 3))));
            json_object_set_new(row, "retabli", json_integer(atol(PQgetvalue(res2, i, 4))));
            json_array_append_new(series, row);
        }
    }
    PQclear(res2);
    json_object_set_new(o, "series", series);

    char *out = json_dumps(o, JSON_COMPACT);
    json_decref(o);
    return out;
}
