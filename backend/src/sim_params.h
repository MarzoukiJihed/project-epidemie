#ifndef SIM_PARAMS_H
#define SIM_PARAMS_H

/* Paramètres d'une simulation, tels qu'envoyés par le frontend en JSON et
 * transmis en ligne de commande au binaire moteur (engine/simulate_par). */
typedef struct {
    int    population;
    int    steps;
    double domain_size;
    double radius;
    double beta;
    double gamma_infecte;
    double gamma_isole;
    double isolation_prob;
    int    min_days_isolation;
    double isolation_effect;
    double mobility;
    int    init_infected;
    long   seed;
    int    threads;
} SimParams;

typedef struct {
    int sain, infecte, isole, retabli;
} Counts;

/* Une ligne de la série temporelle (un pas de temps). */
typedef struct {
    int step;
    int sain, infecte, isole, retabli;
} StepRow;

#endif
