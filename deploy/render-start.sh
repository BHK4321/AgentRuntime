#!/usr/bin/env bash
set -euo pipefail

: "${AGENTOS_DATABASE_URL:?AGENTOS_DATABASE_URL is required}"
: "${OLLAMA_API_KEY:?OLLAMA_API_KEY is required}"
: "${AGENTOS_APP_PASSWORD:?AGENTOS_APP_PASSWORD is required}"
export AGENTOS_WORK_DIR="${AGENTOS_WORK_DIR:-/tmp/agentos-work}"
export AGENTOS_RUNTIME_ADDRESS="127.0.0.1:50051"
export AGENTOS_INTERFACE_URL="http://127.0.0.1:8000"
mkdir -p "$AGENTOS_WORK_DIR"

psql "$AGENTOS_DATABASE_URL" -v ON_ERROR_STOP=1 -f database/schema.sql

agentos_server &
runtime_pid=$!
python -m uvicorn api.main:app --host 127.0.0.1 --port 8000 &
api_pid=$!
python -m uvicorn chat_service.main:app --host 0.0.0.0 --port "${PORT:-10000}" &
chat_pid=$!

cleanup() {
  trap - TERM INT EXIT
  kill "$chat_pid" "$api_pid" "$runtime_pid" 2>/dev/null || true
  wait "$chat_pid" "$api_pid" "$runtime_pid" 2>/dev/null || true
}
trap cleanup TERM INT EXIT
wait -n "$runtime_pid" "$api_pid" "$chat_pid"
