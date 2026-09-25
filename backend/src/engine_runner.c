#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include "engine_runner.h"

#define MAX_ARGS 40

/* Bornes de sécurité : le formulaire web est une entrée non fiable, on
 * refuse toute valeur hors de ces plages avant de lancer le sous-processus. */
static int validate(const SimParams *p) {
    if (p->population < 1 || p->population > 50000) return 0;
    if (p->steps < 1 || p->steps > 2000) return 0;
    if (p->threads < 1 || p->threads > 64) return 0;
    if (p->domain_size <= 0 || p->domain_size > 100000) return 0;
    if (p->radius <= 0 || p->radius > p->domain_size) return 0;
    if (p->beta < 0 || p->beta > 1) return 0;
    if (p->gamma_infecte < 0 || p->gamma_infecte > 1) return 0;
    if (p->gamma_isole < 0 || p->gamma_isole > 1) return 0;
    if (p->isolation_prob < 0 || p->isolation_prob > 1) return 0;
    if (p->isolation_effect < 0 || p->isolation_effect > 1) return 0;
    if (p->min_days_isolation < 0 || p->min_days_isolation > p->steps) return 0;
    if (p->mobility < 0 || p->mobility > 1000) return 0;
    if (p->init_infected < 0 || p->init_infected > p->population) return 0;
    return 1;
}

int engine_run(const SimParams *p, const char *engine_path,
               double *out_time, Counts *out_final,
               StepRow **out_rows, int *out_n) {
    if (!validate(p)) return -1;
    if (access(engine_path, X_OK) != 0) {
        fprintf(stderr, "[engine] binaire introuvable ou non executable: %s\n", engine_path);
        return -1;
    }

    char csv_path[] = "/tmp/epi_run_XXXXXX";
    int fdtmp = mkstemp(csv_path);
    if (fdtmp < 0) return -1;
    close(fdtmp);

    char buf_n[16], buf_t[16], buf_L[32], buf_r[32], buf_b[32], buf_g[32],
         buf_G[32], buf_p[32], buf_d[16], buf_e[32], buf_m[32], buf_i[16],
         buf_s[32], buf_T[16];
    snprintf(buf_n, sizeof buf_n, "%d", p->population);
    snprintf(buf_t, sizeof buf_t, "%d", p->steps);
    snprintf(buf_L, sizeof buf_L, "%.6f", p->domain_size);
    snprintf(buf_r, sizeof buf_r, "%.6f", p->radius);
    snprintf(buf_b, sizeof buf_b, "%.6f", p->beta);
    snprintf(buf_g, sizeof buf_g, "%.6f", p->gamma_infecte);
    snprintf(buf_G, sizeof buf_G, "%.6f", p->gamma_isole);
    snprintf(buf_p, sizeof buf_p, "%.6f", p->isolation_prob);
    snprintf(buf_d, sizeof buf_d, "%d", p->min_days_isolation);
    snprintf(buf_e, sizeof buf_e, "%.6f", p->isolation_effect);
    snprintf(buf_m, sizeof buf_m, "%.6f", p->mobility);
    snprintf(buf_i, sizeof buf_i, "%d", p->init_infected);
    snprintf(buf_s, sizeof buf_s, "%ld", p->seed);
    snprintf(buf_T, sizeof buf_T, "%d", p->threads);

    char *argv[MAX_ARGS];
    int ai = 0;
    argv[ai++] = (char *)engine_path;
    argv[ai++] = "-n"; argv[ai++] = buf_n;
    argv[ai++] = "-t"; argv[ai++] = buf_t;
    argv[ai++] = "-L"; argv[ai++] = buf_L;
    argv[ai++] = "-r"; argv[ai++] = buf_r;
    argv[ai++] = "-b"; argv[ai++] = buf_b;
    argv[ai++] = "-g"; argv[ai++] = buf_g;
    argv[ai++] = "-G"; argv[ai++] = buf_G;
    argv[ai++] = "-p"; argv[ai++] = buf_p;
    argv[ai++] = "-d"; argv[ai++] = buf_d;
    argv[ai++] = "-e"; argv[ai++] = buf_e;
    argv[ai++] = "-m"; argv[ai++] = buf_m;
    argv[ai++] = "-i"; argv[ai++] = buf_i;
    argv[ai++] = "-s"; argv[ai++] = buf_s;
    argv[ai++] = "-T"; argv[ai++] = buf_T;
    argv[ai++] = "-o"; argv[ai++] = csv_path;
    argv[ai++] = "-w"; argv[ai++] = "1";
    argv[ai++] = "-q";
    argv[ai] = NULL;

    int pipefd[2];
    if (pipe(pipefd) != 0) { unlink(csv_path); return -1; }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]); close(pipefd[1]);
        unlink(csv_path);
        return -1;
    }
    if (pid == 0) {
        /* enfant : execution directe du binaire (pas de shell -> pas
           d'injection de commande possible via les parametres) */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        execv(engine_path, argv);
        _exit(127); /* execv a echoue */
    }

    /* parent : lit le resume que le moteur imprime sur stdout */
    close(pipefd[1]);
    char summary[512] = {0};
    ssize_t total = 0, r;
    while (total < (ssize_t)sizeof(summary) - 1 &&
           (r = read(pipefd[0], summary + total, sizeof(summary) - 1 - total)) > 0) {
        total += r;
    }
    close(pipefd[0]);

    int status;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "[engine] le moteur a echoue (status=%d)\n", status);
        unlink(csv_path);
        return -1;
    }

    long N, steps, threads, sain, infecte, isole, retabli;
    double temps;
    if (sscanf(summary, "%ld,%ld,%ld,%lf,%ld,%ld,%ld,%ld",
               &N, &steps, &threads, &temps, &sain, &infecte, &isole, &retabli) != 8) {
        fprintf(stderr, "[engine] resume illisible: '%s'\n", summary);
        unlink(csv_path);
        return -1;
    }
    *out_time = temps;
    out_final->sain = (int)sain;
    out_final->infecte = (int)infecte;
    out_final->isole = (int)isole;
    out_final->retabli = (int)retabli;

    FILE *f = fopen(csv_path, "r");
    if (!f) { unlink(csv_path); return -1; }
    char line[256];
    fgets(line, sizeof(line), f); /* en-tete CSV, ignore */

    int cap = p->steps + 4;
    StepRow *rows = malloc(sizeof(StepRow) * (size_t)cap);
    int n = 0;
    while (fgets(line, sizeof(line), f)) {
        int st, s, ii, q, rr;
        if (sscanf(line, "%d,%d,%d,%d,%d", &st, &s, &ii, &q, &rr) == 5) {
            if (n >= cap) {
                cap *= 2;
                rows = realloc(rows, sizeof(StepRow) * (size_t)cap);
            }
            rows[n].step = st; rows[n].sain = s; rows[n].infecte = ii;
            rows[n].isole = q; rows[n].retabli = rr;
            n++;
        }
    }
    fclose(f);
    unlink(csv_path);

    *out_rows = rows;
    *out_n = n;
    return 0;
}
