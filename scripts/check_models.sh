#!/usr/bin/env bash
# Проверка доступа к Nebius Token Factory и идентификаторов моделей из .env.
#   scripts/check_models.sh          — ключ работает, все три ID есть в каталоге
#   scripts/check_models.sh --live   — плюс по одному минимальному вызову на каждый уровень (Nano/Super/Ultra):
#                                       это «живое доказательство» запуска на Token Factory для жюри (сохраните вывод).
# Ключ в вывод не попадает. .env читается построчно, а не через source (в нём могут быть пробелы и «;»).
set -euo pipefail
cd "$(dirname "$0")/.."

getv() {   # приоритет: окружение > .env > значение по умолчанию
  local k="$1" def="${2:-}" v="${!1:-}"
  if [ -z "$v" ] && [ -f .env ]; then
    v="$(grep -E "^[[:space:]]*(export[[:space:]]+)?$k=" .env | tail -1 | sed -E "s/^[^=]*=//; s/[[:space:]]+#.*$//; s/^[\"']//; s/[\"']$//")"
  fi
  printf '%s' "${v:-$def}"
}

KEY="$(getv NEBIUS_API_KEY)"
[ -n "$KEY" ] || { echo "NEBIUS_API_KEY is not set (put it in .env)"; exit 1; }
BASE="$(getv NEBIUS_BASE_URL https://api.tokenfactory.nebius.com/v1/)"; BASE="${BASE%/}"
case "$BASE" in */v1) ;; *) BASE="$BASE/v1" ;; esac
NANO="$(getv NEMOTRON_MODEL_NANO nvidia/NVIDIA-Nemotron-3-Nano-30B-A3B)"
SUPER="$(getv NEMOTRON_MODEL_SUPER nvidia/nemotron-3-super-120b-a12b)"
ULTRA="$(getv NEMOTRON_MODEL_ULTRA nvidia/Nemotron-3-Ultra-550b-a55b)"

echo "endpoint: ${BASE#*://}"
LIST="$(curl -fsS -H "Authorization: Bearer $KEY" "$BASE/models")" || { echo "cannot list models: wrong key or endpoint"; exit 1; }
missing=0
for pair in "nano:$NANO" "super:$SUPER" "ultra:$ULTRA"; do
  role="${pair%%:*}"; id="${pair#*:}"
  if printf '%s' "$LIST" | grep -qF "\"$id\""; then echo "  ok       $role  $id"; else echo "  MISSING  $role  $id"; missing=1; fi
done
[ "$missing" -eq 0 ] || { echo "Some IDs are not in the provider catalog. Browse https://tokenfactory.nebius.com and fix .env."; exit 2; }

if [ "${1:-}" = "--live" ]; then
  echo "live calls (one tiny request per tier):"
  for pair in "nano:$NANO" "super:$SUPER" "ultra:$ULTRA"; do
    role="${pair%%:*}"; id="${pair#*:}"
    body="$(python3 - "$id" <<'PY'
import json, sys
print(json.dumps({"model": sys.argv[1], "max_tokens": 1024, "temperature": 0,
                  "messages": [{"role": "user", "content": "Reply with the single word: OK"}]}))
PY
)"
    resp="$(curl -sS -m 180 -H "Authorization: Bearer $KEY" -H "Content-Type: application/json" -d "$body" "$BASE/chat/completions")"
    printf '%s' "$resp" | python3 -c '
import json, sys
role = sys.argv[1]
try:
    d = json.load(sys.stdin)
    c = d["choices"][0]
    u = d.get("usage", {})
    txt = (c["message"].get("content") or "").strip().replace("\n", " ")[:40]
    print("  %-6s model=%s  finish=%s  tokens=%s/%s  reply=%r" % (role, d.get("model"), c.get("finish_reason"), u.get("prompt_tokens"), u.get("completion_tokens"), txt))
except Exception as e:
    print("  %-6s FAILED: %s" % (role, e)); sys.exit(1)
' "$role"
  done
fi
