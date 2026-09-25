/*
 * ============================================================================
 * Backend C — serveur HTTP (libmicrohttpd) + API REST JSON + PostgreSQL
 * ============================================================================
 * Sert le frontend statique (public/) et expose :
 *   GET    /api/health          -> etat du serveur / de la base
 *   GET    /api/runs?limit=N    -> liste des simulations enregistrees
 *   GET    /api/runs/:id        -> detail + serie temporelle d'une simulation
 *   POST   /api/runs            -> lance une nouvelle simulation (body JSON)
 *   DELETE /api/runs/:id        -> supprime une simulation
 *
 * Le serveur tourne en un seul thread interne (MHD_USE_INTERNAL_POLLING_THREAD)
 * : une connexion PostgreSQL globale suffit, sans mutex ni pool.
 * ============================================================================
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <microhttpd.h>
#include <jansson.h>

#include "sim_params.h"
#include "db.h"
#include "engine_runner.h"

#define DEFAULT_PORT 8080

static PGconn *g_db = NULL;
static char g_engine_path[1024];
static char g_public_dir[1024];

/* ---------------------------------------------------------------------- */
/* Utilitaires reponse HTTP                                                */
/* ---------------------------------------------------------------------- */

static enum MHD_Result send_text(struct MHD_Connection *conn, unsigned int status,
                                  const char *content_type, const char *body) {
    struct MHD_Response *resp = MHD_create_response_from_buffer(
        strlen(body), (void *)body, MHD_RESPMEM_MUST_COPY);
    MHD_add_response_header(resp, "Content-Type", content_type);
    MHD_add_response_header(resp, "Access-Control-Allow-Origin", "*");
    enum MHD_Result ret = MHD_queue_response(conn, status, resp);
    MHD_destroy_response(resp);
    return ret;
}

static enum MHD_Result send_json_str(struct MHD_Connection *conn, unsigned int status,
                                      const char *json_body) {
    return send_text(conn, status, "application/json; charset=utf-8", json_body);
}

static enum MHD_Result send_json_error(struct MHD_Connection *conn, unsigned int status,
                                        const char *message) {
    json_t *o = json_object();
    json_object_set_new(o, "error", json_string(message));
    char *s = json_dumps(o, JSON_COMPACT);
    json_decref(o);
    enum MHD_Result ret = send_json_str(conn, status, s);
    free(s);
    return ret;
}

/* ---------------------------------------------------------------------- */
/* Fichiers statiques (frontend)                                          */
/* ---------------------------------------------------------------------- */

static const char *mime_for(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    if (!strcmp(ext, ".html")) return "text/html; charset=utf-8";
    if (!strcmp(ext, ".js"))   return "application/javascript; charset=utf-8";
    if (!strcmp(ext, ".css"))  return "text/css; charset=utf-8";
    if (!strcmp(ext, ".json")) return "application/json; charset=utf-8";
    if (!strcmp(ext, ".svg"))  return "image/svg+xml";
    return "application/octet-stream";
}

static enum MHD_Result serve_static(struct MHD_Connection *conn, const char *url) {
    if (strstr(url, "..")) return send_json_error(conn, 400, "chemin invalide");

    char path[2048];
    if (strcmp(url, "/") == 0) {
        snprintf(path, sizeof path, "%s/index.html", g_public_dir);
    } else {
        snprintf(path, sizeof path, "%s%s", g_public_dir, url);
    }

    FILE *f = fopen(path, "rb");
    if (!f) return send_json_error(conn, 404, "page non trouvee");

    struct stat st;
    stat(path, &st);
    char *buf = malloc((size_t)st.st_size);
    size_t rd = fread(buf, 1, (size_t)st.st_size, f);
    fclose(f);

    struct MHD_Response *resp = MHD_create_response_from_buffer(
        rd, buf, MHD_RESPMEM_MUST_FREE);
    MHD_add_response_header(resp, "Content-Type", mime_for(path));
    enum MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_OK, resp);
    MHD_destroy_response(resp);
    return ret;
}

/* ---------------------------------------------------------------------- */
/* API : lecture des parametres JSON envoyes par le frontend               */
/* ---------------------------------------------------------------------- */

static double jget_num(json_t *o, const char *key, double def) {
    json_t *v = json_object_get(o, key);
    if (!v || !json_is_number(v)) return def;
    return json_number_value(v);
}

static void parse_params(json_t *body, SimParams *p) {
    p->population         = (int)jget_num(body, "population", 4000);
    p->steps              = (int)jget_num(body, "steps", 150);
    p->domain_size        = jget_num(body, "domainSize", 100.0);
    p->radius             = jget_num(body, "radius", 1.2);
    p->beta               = jget_num(body, "beta", 0.30);
    p->gamma_infecte      = jget_num(body, "gammaInfecte", 0.045);
    p->gamma_isole        = jget_num(body, "gammaIsole", 0.08);
    p->isolation_prob     = jget_num(body, "isolationProb", 0.09);
    p->min_days_isolation = (int)jget_num(body, "minDaysIsolation", 2);
    p->isolation_effect   = jget_num(body, "isolationEffect", 0.10);
    p->mobility           = jget_num(body, "mobility", 1.2);
    p->init_infected      = (int)jget_num(body, "initInfected", 15);
    p->seed               = (long)jget_num(body, "seed", (double)time(NULL));
    p->threads             = (int)jget_num(body, "threads", 4);
}

/* ---------------------------------------------------------------------- */
/* API handlers                                                            */
/* ---------------------------------------------------------------------- */

static enum MHD_Result api_health(struct MHD_Connection *conn) {
    int db_ok = g_db && PQstatus(g_db) == CONNECTION_OK;
    int engine_ok = access(g_engine_path, X_OK) == 0;
    json_t *o = json_object();
    json_object_set_new(o, "status", json_string((db_ok && engine_ok) ? "ok" : "degraded"));
    json_object_set_new(o, "database", json_boolean(db_ok));
    json_object_set_new(o, "engine", json_boolean(engine_ok));
    char *s = json_dumps(o, JSON_COMPACT);
    json_decref(o);
    enum MHD_Result ret = send_json_str(conn, MHD_HTTP_OK, s);
    free(s);
    return ret;
}

static enum MHD_Result api_list_runs(struct MHD_Connection *conn) {
    const char *limit_str = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "limit");
    int limit = limit_str ? atoi(limit_str) : 50;
    if (limit <= 0 || limit > 500) limit = 50;

    char *json = db_list_runs_json(g_db, limit);
    if (!json) return send_json_error(conn, 500, "erreur base de donnees");
    enum MHD_Result ret = send_json_str(conn, MHD_HTTP_OK, json);
    free(json);
    return ret;
}

static enum MHD_Result api_get_run(struct MHD_Connection *conn, long id) {
    char *json = db_get_run_json(g_db, id);
    if (!json) return send_json_error(conn, 404, "simulation introuvable");
    enum MHD_Result ret = send_json_str(conn, MHD_HTTP_OK, json);
    free(json);
    return ret;
}

static enum MHD_Result api_delete_run(struct MHD_Connection *conn, long id) {
    int ok = db_delete_run(g_db, id);
    if (!ok) return send_json_error(conn, 404, "simulation introuvable");
    return send_json_str(conn, MHD_HTTP_OK, "{\"deleted\":true}");
}

static enum MHD_Result api_create_run(struct MHD_Connection *conn, const char *body) {
    json_error_t jerr;
    json_t *jbody = body && *body ? json_loads(body, 0, &jerr) : json_object();
    if (!jbody) return send_json_error(conn, 400, "JSON invalide");

    SimParams p;
    parse_params(jbody, &p);
    json_decref(jbody);

    double exec_time;
    Counts final_counts;
    StepRow *rows = NULL;
    int n = 0;

    if (engine_run(&p, g_engine_path, &exec_time, &final_counts, &rows, &n) != 0) {
        return send_json_error(conn, 422,
            "parametres invalides ou echec du moteur de simulation");
    }

    long run_id = db_insert_run(g_db, &p, exec_time, &final_counts);
    if (run_id < 0) { free(rows); return send_json_error(conn, 500, "echec enregistrement en base"); }

    db_insert_states(g_db, run_id, rows, n);
    free(rows);

    char *json = db_get_run_json(g_db, run_id);
    enum MHD_Result ret = send_json_str(conn, MHD_HTTP_CREATED, json);
    free(json);
    return ret;
}

/* ---------------------------------------------------------------------- */
/* Routage principal                                                       */
/* ---------------------------------------------------------------------- */

struct conn_info { char *data; size_t size; };

static enum MHD_Result ahc(void *cls, struct MHD_Connection *connection,
                            const char *url, const char *method,
                            const char *version, const char *upload_data,
                            size_t *upload_data_size, void **con_cls) {
    (void)cls; (void)version;

    if (*con_cls == NULL) {
        struct conn_info *ci = calloc(1, sizeof(*ci));
        *con_cls = ci;
        return MHD_YES;
    }

    struct conn_info *ci = *con_cls;

    if (strcmp(method, "POST") == 0 || strcmp(method, "DELETE") == 0) {
        if (*upload_data_size != 0) {
            ci->data = realloc(ci->data, ci->size + *upload_data_size + 1);
            memcpy(ci->data + ci->size, upload_data, *upload_data_size);
            ci->size += *upload_data_size;
            ci->data[ci->size] = '\0';
            *upload_data_size = 0;
            return MHD_YES;
        }
    }

    long run_id;
    if (strcmp(method, "GET") == 0) {
        if (!strcmp(url, "/api/health")) return api_health(connection);
        if (!strcmp(url, "/api/runs"))   return api_list_runs(connection);
        if (sscanf(url, "/api/runs/%ld", &run_id) == 1) return api_get_run(connection, run_id);
        return serve_static(connection, url);
    }
    if (strcmp(method, "POST") == 0) {
        if (!strcmp(url, "/api/runs")) return api_create_run(connection, ci->data ? ci->data : "");
        return send_json_error(connection, 404, "route inconnue");
    }
    if (strcmp(method, "DELETE") == 0) {
        if (sscanf(url, "/api/runs/%ld", &run_id) == 1) return api_delete_run(connection, run_id);
        return send_json_error(connection, 404, "route inconnue");
    }
    if (strcmp(method, "OPTIONS") == 0) {
        return send_text(connection, MHD_HTTP_OK, "text/plain", "");
    }

    return send_json_error(connection, 405, "methode non supportee");
}

static void request_completed(void *cls, struct MHD_Connection *connection,
                               void **con_cls, enum MHD_RequestTerminationCode toe) {
    (void)cls; (void)connection; (void)toe;
    struct conn_info *ci = *con_cls;
    if (ci) { free(ci->data); free(ci); }
    *con_cls = NULL;
}

/* ---------------------------------------------------------------------- */

int main(int argc, char **argv) {
    int port = DEFAULT_PORT;
    const char *conninfo = getenv("DATABASE_URL");
    if (!conninfo) conninfo = "host=localhost dbname=epidemic_db user=epidemic password=epidemic";

    const char *engine = getenv("ENGINE_PATH");
    const char *pubdir = getenv("PUBLIC_DIR");
    snprintf(g_engine_path, sizeof g_engine_path, "%s",
             engine ? engine : "../engine/simulate_par");
    snprintf(g_public_dir, sizeof g_public_dir, "%s", pubdir ? pubdir : "public");

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p") && i + 1 < argc) port = atoi(argv[++i]);
    }

    g_db = db_connect(conninfo);
    if (!g_db) {
        fprintf(stderr, "Impossible de se connecter a PostgreSQL (%s)\n", conninfo);
        return 1;
    }

    struct MHD_Daemon *daemon = MHD_start_daemon(
        MHD_USE_INTERNAL_POLLING_THREAD, port, NULL, NULL,
        &ahc, NULL,
        MHD_OPTION_NOTIFY_COMPLETED, &request_completed, NULL,
        MHD_OPTION_END);

    if (!daemon) {
        fprintf(stderr, "Impossible de demarrer le serveur HTTP sur le port %d\n", port);
        db_disconnect(g_db);
        return 1;
    }

    fprintf(stderr, "Backend pret : http://localhost:%d  (moteur=%s, public=%s)\n",
            port, g_engine_path, g_public_dir);
    fprintf(stderr, "Appuyez sur Ctrl+C pour arreter.\n");

    /* boucle infinie : le daemon MHD tourne dans son propre thread interne */
    while (1) pause();

    MHD_stop_daemon(daemon);
    db_disconnect(g_db);
    return 0;
}
