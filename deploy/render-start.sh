#!/usr/bin/env bash
set -euo pipefail

: "${OLLAMA_API_KEY:?OLLAMA_API_KEY is required}"
: "${AGENTOS_APP_PASSWORD:?AGENTOS_APP_PASSWORD is required}"
export AGENTOS_WORK_DIR="${AGENTOS_WORK_DIR:-/tmp/agentos-work}"
export AGENTOS_RUNTIME_ADDRESS="127.0.0.1:50051"
export AGENTOS_INTERFACE_URL="http://127.0.0.1:8000"
mkdir -p "$AGENTOS_WORK_DIR"

# This is the Render-only entrypoint. Create a new local database on every
# container start; local Windows/PowerShell runs still use their configured DB.
pg_bin="$(pg_config --bindir)"
pg_data="$(mktemp -d /tmp/agentos-pg.XXXXXX)"
chown postgres:postgres "$pg_data"
chmod 700 "$pg_data"
runuser -u postgres -- "$pg_bin/initdb" -D "$pg_data" --auth-local=trust --auth-host=trust --no-instructions
runuser -u postgres -- "$pg_bin/pg_ctl" -D "$pg_data" -l "$pg_data/server.log" \
  -o "-c listen_addresses=127.0.0.1 -c unix_socket_directories=$pg_data -c shared_buffers=16MB -c max_connections=20" \
  -w start
"$pg_bin/psql" -h 127.0.0.1 -U postgres -d postgres -v ON_ERROR_STOP=1 -c "CREATE ROLE agentos LOGIN"
"$pg_bin/createdb" -h 127.0.0.1 -U postgres -O agentos agentos
export AGENTOS_DATABASE_URL="postgresql://agentos@127.0.0.1:5432/agentos"
psql "$AGENTOS_DATABASE_URL" -v ON_ERROR_STOP=1 -f database/schema.sql

agentos_server &
runtime_pid=$!
python -m uvicorn api.main:app --host 127.0.0.1 --port 8000 &
api_pid=$!
python -m uvicorn chat_service.main:app --host 0.0.0.0 --port "${PORT:-10000}" &
chat_pid=$!
(while sleep 5; do "$pg_bin/pg_isready" -h 127.0.0.1 -q || exit 1; done) &
db_watch_pid=$!

cleanup() {
  trap - TERM INT EXIT
  kill "$chat_pid" "$api_pid" "$runtime_pid" "$db_watch_pid" 2>/dev/null || true
  wait "$chat_pid" "$api_pid" "$runtime_pid" "$db_watch_pid" 2>/dev/null || true
  runuser -u postgres -- "$pg_bin/pg_ctl" -D "$pg_data" -m immediate -w stop >/dev/null 2>&1 || true
}
trap cleanup TERM INT EXIT
wait -n "$runtime_pid" "$api_pid" "$chat_pid" "$db_watch_pid"
