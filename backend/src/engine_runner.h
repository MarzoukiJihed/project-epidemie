#ifndef ENGINE_RUNNER_H
#define ENGINE_RUNNER_H

#include "sim_params.h"

/* Lance le binaire moteur (engine/simulate_par) avec les parametres donnes,
 * attend sa fin, recupere le temps d'execution + l'etat final (via la
 * ligne resumee ecrite sur stdout par le moteur) et la serie temporelle
 * complete (via le fichier CSV qu'il ecrit).
 *
 * Sortie : *out_rows est alloue par cette fonction (a liberer par
 * l'appelant), *out_n est son nombre de lignes, *out_time le temps mesure
 * par le moteur (omp_get_wtime), *out_final l'etat final.
 *
 * Retourne 0 en cas de succes, -1 en cas d'erreur (binaire introuvable,
 * parametres invalides, sortie illisible...).
 */
int engine_run(const SimParams *p, const char *engine_path,
                double *out_time, Counts *out_final,
                StepRow **out_rows, int *out_n);

#endif
