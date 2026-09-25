#!/usr/bin/env bash
# ============================================================================
# Compile (si besoin) et lance le moteur, puis le backend C.
# ============================================================================
set -e
cd "$(dirname "$0")/.."

export DATABASE_URL=${DATABASE_URL:-"host=localhost dbname=epidemic_db user=epidemic password=epidemic"}
export ENGINE_PATH=${ENGINE_PATH:-"$(pwd)/engine/simulate_par"}
export PUBLIC_DIR=${PUBLIC_DIR:-"$(pwd)/backend/public"}
PORT=${PORT:-8080}

if [ ! -x engine/simulate_par ]; then
    echo ">> Compilation du moteur de simulation..."
    make -C engine
fi

if [ ! -x backend/epidemic_backend ]; then
    echo ">> Compilation du backend..."
    make -C backend
fi

echo ">> Demarrage sur http://localhost:${PORT}"
exec backend/epidemic_backend -p "${PORT}"
