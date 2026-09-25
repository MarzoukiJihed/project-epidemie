# Simulation épidémique parallèle — application full-stack (C + JS + PostgreSQL)

Application web complète : **backend en C** (serveur HTTP + API REST) qui
pilote le moteur de simulation OpenMP et persiste les résultats dans
**PostgreSQL**, avec un **frontend JavaScript** (SPA sans framework) pour
configurer, lancer et visualiser les simulations.

```
┌──────────────┐   HTTP/JSON   ┌────────────────────┐   fork/exec   ┌──────────────────┐
│  Frontend JS │ ────────────► │  Backend C          │ ────────────► │ engine/simulate_par│
│ (public/)     │ ◄──────────── │ (libmicrohttpd)      │ ◄──────────── │ (C + OpenMP)        │
└──────────────┘   JSON        └─────────┬───────────┘   CSV+stdout  └──────────────────┘
                                          │ libpq
                                          ▼
                                   ┌─────────────┐
                                   │ PostgreSQL   │
                                   │ (runs,        │
                                   │  run_states)  │
                                   └─────────────┘
```

## 1. Composants

| Composant | Techno | Rôle |
|---|---|---|
| `engine/` | C + OpenMP | Le moteur de simulation (agents mobiles, 4 états) — identique à la version CLI précédente, invoqué en sous-processus |
| `backend/` | C (libmicrohttpd, libpq, jansson) | Serveur HTTP + API REST JSON, orchestre le moteur, lit/écrit PostgreSQL, sert le frontend statique |
| `backend/public/` | HTML/CSS/JS (Chart.js) | Interface web : formulaire de paramètres, graphique de la courbe épidémique, historique des simulations |
| `db/schema.sql` | PostgreSQL | Tables `runs` (paramètres + résumé) et `run_states` (série temporelle) |

## 2. Pourquoi cette architecture

- **Le moteur de calcul reste un binaire C/OpenMP autonome** (`engine/simulate_par`),
  strictement identique à la version en ligne de commande : il ne sait rien
  du web ni de la base de données, il simule et écrit un CSV + un résumé.
  C'est le backend qui l'orchestre en sous-processus (`fork`/`execv`, sans
  passer par un shell — donc pas d'injection de commande possible via les
  paramètres du formulaire).
- **Le backend est également en C** (conforme à la demande), en utilisant
  deux bibliothèques standard de l'écosystème C pour le web :
  [libmicrohttpd](https://www.gnu.org/software/libmicrohttpd/) pour le
  serveur HTTP et `libpq` (client officiel PostgreSQL) pour la base. Il
  tourne en un seul thread interne (`MHD_USE_INTERNAL_POLLING_THREAD`), ce
  qui permet d'utiliser une connexion PostgreSQL globale sans mutex ni pool
  de connexions — le parallélisme "réel" reste dans le moteur OpenMP, pas
  dans la gestion des requêtes HTTP (dont le volume n'a rien de comparable).
- **PostgreSQL** stocke chaque simulation (paramètres + temps d'exécution +
  état final) dans `runs`, et sa série temporelle complète dans
  `run_states` — ce qui permet de rejouer/comparer des simulations passées
  sans avoir à les relancer.

## 3. Installation (Ubuntu/Debian)

```bash
# Dépendances système
sudo apt-get update
sudo apt-get install -y build-essential libmicrohttpd-dev libjansson-dev \
                         libpq-dev postgresql postgresql-contrib

# Démarrer PostgreSQL si ce n'est pas déjà fait
sudo service postgresql start

# Créer la base et appliquer le schéma
bash scripts/setup_db.sh
```

## 4. Lancer l'application

```bash
bash scripts/run.sh
# >> Demarrage sur http://localhost:8080
```

Ce script compile le moteur (`engine/`) et le backend (`backend/`) s'ils ne
le sont pas déjà, puis démarre le serveur. Ouvrez ensuite
**http://localhost:8080** dans un navigateur.

Variables d'environnement reconnues par le backend (valeurs par défaut
utilisées par `scripts/run.sh`) :

| Variable | Défaut | Rôle |
|---|---|---|
| `DATABASE_URL` | `host=localhost dbname=epidemic_db user=epidemic password=epidemic` | chaîne de connexion libpq |
| `ENGINE_PATH` | `engine/simulate_par` | chemin du binaire moteur |
| `PUBLIC_DIR` | `backend/public` | dossier du frontend statique |
| `-p <port>` (argument CLI) | `8080` | port HTTP |

## 5. Utilisation

1. **Nouvelle simulation** : le formulaire de gauche expose tous les
   paramètres du modèle (population, β, γ, isolement, mobilité, rayon de
   contagion, graine, nombre de threads OpenMP). Cliquer sur *Lancer la
   simulation* envoie `POST /api/runs`, qui déclenche le moteur côté
   serveur et enregistre le résultat en base.
2. **Historique** : chaque simulation lancée apparaît dans la colonne de
   gauche (`GET /api/runs`) ; cliquer dessus recharge sa courbe complète
   (`GET /api/runs/:id`) directement depuis PostgreSQL, sans relancer de
   calcul.
3. **Suppression** : bouton *Supprimer* dans le panneau de résultat
   (`DELETE /api/runs/:id`).

## 6. API REST

| Méthode | Route | Description |
|---|---|---|
| `GET` | `/api/health` | état du serveur, de la base et du moteur |
| `GET` | `/api/runs?limit=50` | liste des simulations (résumé), plus récentes en premier |
| `GET` | `/api/runs/:id` | paramètres + résultat final + série temporelle complète |
| `POST` | `/api/runs` | lance une simulation (body JSON, voir `public/app.js` pour les clés) et l'enregistre |
| `DELETE` | `/api/runs/:id` | supprime une simulation (cascade sur la série temporelle) |

Exemple :
```bash
curl -X POST http://localhost:8080/api/runs \
  -H "Content-Type: application/json" \
  -d '{"population":6000,"steps":200,"beta":0.32,"gammaInfecte":0.045,
       "gammaIsole":0.08,"isolationProb":0.09,"mobility":1.2,"radius":1.4,
       "initInfected":15,"seed":42,"threads":4}'
```

## 7. Vérification faite pendant le développement

Testé de bout en bout dans l'environnement de build :
- `POST /api/runs` → le moteur s'exécute, écrit son CSV, le backend le lit,
  l'insère dans PostgreSQL (`runs` + 201 lignes dans `run_states` pour une
  simulation de 200 pas), et répond avec le JSON complet.
- Deux simulations lancées avec les **mêmes paramètres et graine**, mais
  `threads=1` puis `threads=8`, produisent un **état final rigoureusement
  identique** (double-buffering + RNG par individu du moteur, cf. le projet
  CLI précédent) — vérifié via l'API.
- Validation des entrées côté backend (`engine_runner.c`) : une population
  hors bornes renvoie `422` sans jamais lancer le sous-processus.
- `GET /api/runs/:id` sur un id inexistant renvoie proprement `404`.
- `DELETE` supprime bien la simulation et sa série temporelle (contrainte
  `ON DELETE CASCADE`).

## 8. Limites connues / pistes d'amélioration

- Le formulaire ne montre pas de barre de progression pendant le calcul
  (la requête `POST /api/runs` est synchrone) — pour de très grandes
  populations (proches de la limite de 50 000), prévoir un mode
  asynchrone (job en base + polling) plutôt qu'une réponse HTTP bloquante.
- Un seul thread HTTP interne : suffisant pour un usage mono-utilisateur
  ou une démo, mais à faire évoluer (`MHD_USE_THREAD_PER_CONNECTION` +
  pool de connexions PostgreSQL) pour un usage multi-utilisateurs
  concurrent.
- Pas d'authentification : à ajouter avant toute exposition publique.
