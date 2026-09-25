/*
 * ============================================================================
 * Simulation parallele de propagation d'une epidemie (modele SIIsoR)
 * ============================================================================
 * Etats : SAIN (S), INFECTE (I), ISOLE (Q), RETABLI (R)
 *
 * Modele a base d'agents mobiles sur un domaine 2D continu [0,L]x[0,L].
 * A chaque pas de temps :
 *   1. Deplacement (mobilite) de chaque individu (marche aleatoire reflechie)
 *   2. Contagion   : un SAIN devient INFECTE s'il est a portee (rayon R) d'un
 *                    INFECTE ou d'un ISOLE (avec efficacite reduite), avec
 *                    une probabilite liee au taux de transmission beta.
 *   3. Evolution de la maladie : un INFECTE peut etre place en ISOLEMENT
 *                    (mesure de prevention) ou guerir (taux gamma).
 *                    Un ISOLE guerit egalement (taux gamma_iso).
 *
 * Parallelisation OpenMP :
 *   - Double-buffering strict (etat courant en lecture seule / etat suivant
 *     en ecriture) => aucune dependance entre iterations du "parallel for",
 *     donc AUCUNE synchronisation (barriere implicite en fin de boucle
 *     uniquement).
 *   - Generateur aleatoire independant par individu (xorshift32) => les
 *     resultats sont rigoureusement REPRODUCTIBLES quel que soit le nombre
 *     de threads (determinisme, cf. etude de stabilite).
 *   - La recherche de voisins est le noyau couteux (O(N^2) par pas de temps)
 *     et beneficie pleinement de #pragma omp parallel for schedule(dynamic).
 *
 * Compilation : gcc -O3 -march=native -fopenmp simulate.c -o simulate -lm
 * ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#ifdef _OPENMP
#include <omp.h>
#endif

typedef enum { SAIN = 0, INFECTE = 1, ISOLE = 2, RETABLI = 3 } Etat;
#define NB_ETATS 4
static const char *NOM_ETAT[NB_ETATS] = {"Sain", "Infecte", "Isole", "Retabli"};

/* ---- Parametres globaux de simulation (valeurs par defaut) ---- */
typedef struct {
    long   N;                  /* nombre d'individus                       */
    double L;                  /* taille du domaine carre [0,L]x[0,L]      */
    int    steps;               /* nombre de pas de temps                   */
    double radius;              /* rayon de contagion                       */
    double beta;                 /* probabilite de transmission par contact  */
    double gamma_i;              /* probabilite de guerison (infecte)/pas    */
    double gamma_q;               /* probabilite de guerison (isole)/pas      */
    double isolation_prob;        /* probabilite d'isolement d'un infecte/pas */
    int    min_days_isolation;    /* delai avant detection possible           */
    double isolation_effect;      /* facteur reducteur de transmission isole  */
    double mobility;              /* deplacement max par pas (individu libre) */
    double mobility_isolated;     /* deplacement max (individu isole)         */
    int    init_infected;         /* nombre initial d'infectes                */
    unsigned int seed;            /* graine maitresse                         */
    int    threads;               /* nombre de threads OpenMP (0 = defaut)    */
    int    write_interval;        /* ecrire le CSV tous les k pas (0 = jamais)*/
    char   csv_path[512];
    int    quiet;
} Params;

/* ---- xorshift32 : generateur rapide, un flux independant par individu ---- */
static inline unsigned int xorshift32(unsigned int *s) {
    unsigned int x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return x;
}
static inline double rand01(unsigned int *s) {
    return (xorshift32(s) & 0xFFFFFF) / (double)0x1000000; /* [0,1) */
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  -n N              population totale (defaut 4000)\n"
        "  -t steps          nombre de pas de temps (defaut 300)\n"
        "  -L taille         cote du domaine (defaut 100.0)\n"
        "  -r rayon          rayon de contagion (defaut 1.0)\n"
        "  -b beta           taux de transmission [0,1] (defaut 0.30)\n"
        "  -g gamma          taux de guerison infecte [0,1] (defaut 0.05)\n"
        "  -G gamma_isole    taux de guerison isole [0,1] (defaut 0.08)\n"
        "  -p prob_isolement taux d'isolement/pas [0,1] (defaut 0.10)\n"
        "  -d jours_min      jours avant isolement possible (defaut 2)\n"
        "  -e efficacite_iso reduction transmission si isole (defaut 0.10)\n"
        "  -m mobilite       deplacement max/pas individu libre (defaut 1.0)\n"
        "  -i infectes_init  nombre d'infectes au depart (defaut 10)\n"
        "  -s seed           graine aleatoire (defaut 42)\n"
        "  -T threads        nombre de threads OpenMP (defaut: auto)\n"
        "  -o fichier.csv    fichier de sortie (defaut resultats.csv)\n"
        "  -w intervalle     ecrire CSV tous les k pas (defaut 1)\n"
        "  -q                mode silencieux (pas d'affichage console)\n"
        "  -h                affiche cette aide\n", prog);
}

static void parse_args(int argc, char **argv, Params *p) {
    p->N = 4000; p->L = 100.0; p->steps = 300; p->radius = 1.0;
    p->beta = 0.30; p->gamma_i = 0.05; p->gamma_q = 0.08;
    p->isolation_prob = 0.10; p->min_days_isolation = 2;
    p->isolation_effect = 0.10; p->mobility = 1.0; p->mobility_isolated = 0.05;
    p->init_infected = 10; p->seed = 42; p->threads = 0;
    p->write_interval = 1; p->quiet = 0;
    strcpy(p->csv_path, "resultats.csv");

    for (int a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "-h")) { usage(argv[0]); exit(0); }
        else if (!strcmp(argv[a], "-q")) { p->quiet = 1; continue; }
        if (a + 1 >= argc) { usage(argv[0]); exit(1); }
        char *v = argv[a + 1];
        if (!strcmp(argv[a], "-n")) p->N = atol(v);
        else if (!strcmp(argv[a], "-t")) p->steps = atoi(v);
        else if (!strcmp(argv[a], "-L")) p->L = atof(v);
        else if (!strcmp(argv[a], "-r")) p->radius = atof(v);
        else if (!strcmp(argv[a], "-b")) p->beta = atof(v);
        else if (!strcmp(argv[a], "-g")) p->gamma_i = atof(v);
        else if (!strcmp(argv[a], "-G")) p->gamma_q = atof(v);
        else if (!strcmp(argv[a], "-p")) p->isolation_prob = atof(v);
        else if (!strcmp(argv[a], "-d")) p->min_days_isolation = atoi(v);
        else if (!strcmp(argv[a], "-e")) p->isolation_effect = atof(v);
        else if (!strcmp(argv[a], "-m")) p->mobility = atof(v);
        else if (!strcmp(argv[a], "-i")) p->init_infected = atoi(v);
        else if (!strcmp(argv[a], "-s")) p->seed = (unsigned int)atol(v);
        else if (!strcmp(argv[a], "-T")) p->threads = atoi(v);
        else if (!strcmp(argv[a], "-o")) strncpy(p->csv_path, v, sizeof(p->csv_path) - 1);
        else if (!strcmp(argv[a], "-w")) p->write_interval = atoi(v);
        else { usage(argv[0]); exit(1); }
        a++;
    }
    p->mobility_isolated = p->mobility * 0.05;
}

int main(int argc, char **argv) {
    Params p;
    parse_args(argc, argv, &p);

#ifdef _OPENMP
    if (p.threads > 0) omp_set_num_threads(p.threads);
    int actual_threads = omp_get_max_threads();
#else
    int actual_threads = 1;
#endif

    long N = p.N;
    double *x = malloc(N * sizeof(double));
    double *y = malloc(N * sizeof(double));
    unsigned char *state = malloc(N * sizeof(unsigned char));
    unsigned char *nextState = malloc(N * sizeof(unsigned char));
    int *infDays = malloc(N * sizeof(int));
    int *infDaysNext = malloc(N * sizeof(int));
    unsigned int *rng = malloc(N * sizeof(unsigned int));

    if (!x || !y || !state || !nextState || !infDays || !infDaysNext || !rng) {
        fprintf(stderr, "Erreur allocation memoire pour N=%ld\n", N);
        return 1;
    }

    /* Initialisation reproductible : un flux RNG independant par individu,
       derive de la graine maitresse -> resultat identique quel que soit le
       nombre de threads utilise. */
    for (long i = 0; i < N; i++) {
        unsigned int seed_i = p.seed ^ (unsigned int)(i * 2654435761u) ^ 0x9E3779B9u;
        if (seed_i == 0) seed_i = 1;
        rng[i] = seed_i;
        x[i] = rand01(&rng[i]) * p.L;
        y[i] = rand01(&rng[i]) * p.L;
        state[i] = SAIN;
        infDays[i] = 0;
    }
    for (int k = 0; k < p.init_infected && k < N; k++) {
        state[k] = INFECTE;
    }

    FILE *csv = NULL;
    if (p.write_interval > 0) {
        csv = fopen(p.csv_path, "w");
        if (!csv) { fprintf(stderr, "Impossible d'ouvrir %s\n", p.csv_path); return 1; }
        fprintf(csv, "pas,sain,infecte,isole,retabli\n");
    }

    if (!p.quiet) {
        fprintf(stderr, "=== Simulation epidemique parallele (OpenMP) ===\n");
        fprintf(stderr, "N=%ld  steps=%d  threads=%d  beta=%.2f  gamma=%.2f  "
                        "p_isolement=%.2f  mobilite=%.2f  rayon=%.2f\n",
                N, p.steps, actual_threads, p.beta, p.gamma_i,
                p.isolation_prob, p.mobility, p.radius);
    }

    double t_start =
#ifdef _OPENMP
        omp_get_wtime();
#else
        (double)clock() / CLOCKS_PER_SEC;
#endif

    long counts[NB_ETATS];

    for (int step = 0; step <= p.steps; step++) {

        /* ---- 1. Deplacement (parallele, sans dependance) ---- */
        #pragma omp parallel for schedule(static)
        for (long i = 0; i < N; i++) {
            double mob = (state[i] == ISOLE) ? p.mobility_isolated : p.mobility;
            double dx = (rand01(&rng[i]) * 2.0 - 1.0) * mob;
            double dy = (rand01(&rng[i]) * 2.0 - 1.0) * mob;
            double nx = x[i] + dx, ny = y[i] + dy;
            /* reflexion aux bords du domaine */
            if (nx < 0) nx = -nx; if (nx > p.L) nx = 2 * p.L - nx;
            if (ny < 0) ny = -ny; if (ny > p.L) ny = 2 * p.L - ny;
            x[i] = nx; y[i] = ny;
        }

        /* ---- 2. Mise a jour des etats (lecture de 'state', ecriture de
                   'nextState' : double-buffering => zero synchronisation
                   necessaire a l'interieur de la boucle parallele) ---- */
        #pragma omp parallel for schedule(dynamic, 64)
        for (long i = 0; i < N; i++) {
            unsigned char s = state[i];
            unsigned char ns = s;
            int nd = infDays[i];

            if (s == SAIN) {
                double r2 = p.radius * p.radius;
                for (long j = 0; j < N; j++) {
                    if (j == i) continue;
                    unsigned char sj = state[j];
                    if (sj != INFECTE && sj != ISOLE) continue;
                    double ddx = x[i] - x[j], ddy = y[i] - y[j];
                    if (ddx * ddx + ddy * ddy > r2) continue;
                    double eff_beta = (sj == ISOLE) ? p.beta * p.isolation_effect : p.beta;
                    if (rand01(&rng[i]) < eff_beta) { ns = INFECTE; nd = 0; break; }
                }
            } else if (s == INFECTE) {
                nd = infDays[i] + 1;
                if (rand01(&rng[i]) < p.gamma_i) {
                    ns = RETABLI;
                } else if (nd >= p.min_days_isolation && rand01(&rng[i]) < p.isolation_prob) {
                    ns = ISOLE;
                }
            } else if (s == ISOLE) {
                nd = infDays[i] + 1;
                if (rand01(&rng[i]) < p.gamma_q) ns = RETABLI;
            } /* RETABLI : etat absorbant (pas de reinfection dans ce modele) */

            nextState[i] = ns;
            infDaysNext[i] = nd;
        }

        /* ---- swap des buffers (O(1), pas de copie) ---- */
        unsigned char *tmp = state; state = nextState; nextState = tmp;
        int *tmpd = infDays; infDays = infDaysNext; infDaysNext = tmpd;

        /* ---- comptage des etats (reduction parallele) ---- */
        long c0 = 0, c1 = 0, c2 = 0, c3 = 0;
        #pragma omp parallel for reduction(+:c0,c1,c2,c3) schedule(static)
        for (long i = 0; i < N; i++) {
            switch (state[i]) {
                case SAIN: c0++; break;
                case INFECTE: c1++; break;
                case ISOLE: c2++; break;
                default: c3++; break;
            }
        }
        counts[SAIN] = c0; counts[INFECTE] = c1; counts[ISOLE] = c2; counts[RETABLI] = c3;

        if (csv && p.write_interval > 0 && (step % p.write_interval == 0)) {
            fprintf(csv, "%d,%ld,%ld,%ld,%ld\n", step, counts[0], counts[1], counts[2], counts[3]);
        }
    }

    double t_end =
#ifdef _OPENMP
        omp_get_wtime();
#else
        (double)clock() / CLOCKS_PER_SEC;
#endif
    double elapsed = t_end - t_start;

    if (csv) fclose(csv);

    if (!p.quiet) {
        fprintf(stderr, "Etat final : Sain=%ld Infecte=%ld Isole=%ld Retabli=%ld\n",
                counts[SAIN], counts[INFECTE], counts[ISOLE], counts[RETABLI]);
        fprintf(stderr, "Temps de simulation : %.4f s\n", elapsed);
    }
    /* Ligne unique et facile a parser pour les scripts de benchmark */
    printf("%ld,%d,%d,%.6f,%ld,%ld,%ld,%ld\n",
           N, p.steps, actual_threads, elapsed,
           counts[SAIN], counts[INFECTE], counts[ISOLE], counts[RETABLI]);

    free(x); free(y); free(state); free(nextState);
    free(infDays); free(infDaysNext); free(rng);
    return 0;
}
