#!/usr/bin/env bash
# Интеграционные тесты без облака и без кредитов:
#   * mock OpenAI-совместимый API  → проверка, что наружу не уезжает PII, что работают ретраи и reasoning-бюджет;
#   * локальный HTTP-сервер        → проверка загрузчика моделей (SHA-256, отмена, политика URL).
# Использование: scripts/run_tests.sh <каталог сборки>
set -u
BUILD="${1:-build}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MOCK_PORT="${MOCK_PORT:-8099}"
DL_PORT="${DL_PORT:-8098}"
TMP="$(mktemp -d)"
PIDS=()

cleanup() {
  for p in "${PIDS[@]:-}"; do [ -n "$p" ] && kill "$p" 2>/dev/null; done
  rm -rf "$TMP"
}
trap cleanup EXIT

sha() { if command -v sha256sum >/dev/null; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi; }
wait_port() { for _ in $(seq 1 50); do curl -s -o /dev/null "http://127.0.0.1:$1/" && return 0; sleep 0.1; done; echo "port $1 did not come up" >&2; return 1; }

MOCK_LOG="$TMP/mock.jsonl" python3 "$ROOT/scripts/mock_openai.py" "$MOCK_PORT" >"$TMP/mock.out" 2>&1 &
PIDS+=($!)
mkdir -p "$TMP/srv"
head -c 2000000 /dev/urandom > "$TMP/srv/blob.bin"
( cd "$TMP/srv" && exec python3 -m http.server "$DL_PORT" --bind 127.0.0.1 >"$TMP/http.out" 2>&1 ) &
PIDS+=($!)
wait_port "$MOCK_PORT" || exit 1
wait_port "$DL_PORT" || exit 1

export MOCK_URL="http://127.0.0.1:$MOCK_PORT/v1/"
export MOCK_LOG="$TMP/mock.jsonl"
export DL_URL="http://127.0.0.1:$DL_PORT/blob.bin"
export DL_SHA="$(sha "$TMP/srv/blob.bin")"

fail=0
run() {
  if [ -x "$BUILD/$1" ]; then
    echo "== $1"
    # у каждого теста свой журнал mock-сервера, чтобы проверки утечек не зависели от порядка запуска
    : > "$MOCK_LOG"
    "$BUILD/$1" || { echo "!! $1 FAILED"; fail=1; }
  else
    echo "== $1 (skipped: not built)"
  fi
}
for t in support_test guarded_e2e_test refiner_test corrections_test api_extras_test; do run "$t"; done
[ "$fail" -eq 0 ] && echo "ALL INTEGRATION TESTS PASSED"
exit "$fail"
