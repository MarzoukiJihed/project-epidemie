#!/usr/bin/env bash
# ============================================================================
# Cree l'utilisateur et la base PostgreSQL, puis applique le schema.
# A adapter si PostgreSQL est deja configure differemment sur la machine.
# ============================================================================
set -e
cd "$(dirname "$0")/.."

DB_USER=${DB_USER:-epidemic}
DB_PASS=${DB_PASS:-epidemic}
DB_NAME=${DB_NAME:-epidemic_db}

echo ">> Creation du role et de la base (via l'utilisateur systeme 'postgres')"
sudo -u postgres psql -v ON_ERROR_STOP=0 -c "CREATE USER ${DB_USER} WITH PASSWORD '${DB_PASS}';" || true
sudo -u postgres psql -v ON_ERROR_STOP=0 -c "CREATE DATABASE ${DB_NAME} OWNER ${DB_USER};" || true

echo ">> Application du schema"
PGPASSWORD=${DB_PASS} psql -h localhost -U ${DB_USER} -d ${DB_NAME} -f db/schema.sql

echo ">> Termine. Chaine de connexion :"
echo "   host=localhost dbname=${DB_NAME} user=${DB_USER} password=${DB_PASS}"
