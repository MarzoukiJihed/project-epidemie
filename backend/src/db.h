#ifndef DB_H
#define DB_H

#include <libpq-fe.h>
#include "sim_params.h"

/* Connexion unique et globale à PostgreSQL (le serveur HTTP tourne en un
 * seul thread interne -> pas besoin de pool de connexions ni de mutex). */
PGconn *db_connect(const char *conninfo);
void    db_disconnect(PGconn *conn);

/* Insère une nouvelle simulation (paramètres + résultat final) et renvoie
 * son id (-1 en cas d'erreur). */
long db_insert_run(PGconn *conn, const SimParams *p, double exec_time_s,
                    const Counts *final_counts);

/* Insère en une seule requête multi-valeurs toute la série temporelle
 * d'une simulation. */
int db_insert_states(PGconn *conn, long run_id, const StepRow *rows, int n);

/* Construit le JSON de la liste des simulations (jusqu'à `limit`, plus
 * récentes en premier). Retourne une chaîne allouée (à libérer par
 * l'appelant) ou NULL en cas d'erreur. */
char *db_list_runs_json(PGconn *conn, int limit);

/* Construit le JSON détaillé d'une simulation (paramètres + résultat final
 * + série temporelle complète). Retourne NULL si l'id n'existe pas. */
char *db_get_run_json(PGconn *conn, long run_id);

/* Supprime une simulation (cascade sur run_states). Retourne 1 si une ligne
 * a été supprimée, 0 sinon. */
int db_delete_run(PGconn *conn, long run_id);

#endif
