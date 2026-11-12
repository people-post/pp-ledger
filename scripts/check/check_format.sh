#!/usr/bin/env bash
# C and C++ formatting: every first-party source is clang-formatted with the repo's .clang-format, by exactly
# clang-format 23.1.5 (other versions format differently): `pip install clang-format==23.1.5`, or set CLANG_FORMAT
# to that binary. Vendored code is left as it came: third_party/ and the CLI11 and cpp-httplib copies
# (src/lib/cli, src/lib/http).
#
#   check_format.sh          check: list the files that are not formatted, exit 1 if any (CI)
#   check_format.sh --fix    format them in place
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

want="23.1.5"
cf="${CLANG_FORMAT:-clang-format}"
have="$("$cf" --version 2>/dev/null | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1 || true)"
if [[ "$have" != "$want" ]]; then
  echo "clang-format $want is required (found: ${have:-none}). pip install clang-format==$want, or set CLANG_FORMAT." >&2
  exit 2
fi

files() {
  git ls-files -z -- '*.h' '*.hpp' '*.hh' '*.c' '*.cc' '*.cpp' '*.cxx' '*.inl' |
    tr '\0' '\n' | grep -vE '(^|/)third_party/|^src/lib/(cli|http)/' || true
}

if [[ "${1:-}" == "--fix" ]]; then
  files | tr '\n' '\0' | xargs -0 -r -n 50 -P 8 "$cf" -i
  exit 0
fi

bad="$(files | tr '\n' '\0' | xargs -0 -r -n 50 -P 8 "$cf" --dry-run --Werror 2>&1 | grep -oE '^[^:]+:[0-9]+:[0-9]+: error' | cut -d: -f1 | sort -u || true)"
if [[ -n "$bad" ]]; then
  echo "Not clang-formatted (run $0 --fix):" >&2
  echo "$bad" >&2
  exit 1
fi
echo "clang-format: all first-party sources formatted"
