#!/usr/bin/env bash
# The library layout (docs/architecture/FOLDER_DEPENDENCY.md § Includes): public headers live in
# include/ledger/<folder>/ and are included as "ledger/<folder>/X.h" — from the library, its tests,
# its apps and its embedders alike. A quoted include is one of:
#   "ledger/…"                       this library (include/ledger/)
#   "common/…" "crypto/…" "amp/…"    pp-cpp-common / pp-cpp-crypto / pp-cpp-amp
#   a file in the includer's folder  an app's or a test's private header (no path)
# The vendored CLI11 and cpp-httplib (src/lib/cli, src/lib/http) are not checked.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

python3 - "$ROOT" <<'PY'
import re
import sys
from pathlib import Path

root = Path(sys.argv[1])
external = ("common/", "crypto/", "amp/")
fail = 0
for base in (root / "include", root / "src"):
    for path in sorted(base.rglob("*")):
        if path.suffix not in (".h", ".hpp", ".cpp", ".cc"):
            continue
        rel = path.relative_to(root).as_posix()
        if rel.startswith(("src/lib/cli/", "src/lib/http/")):
            continue
        for n, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
            m = re.match(r'\s*#\s*include\s+"([^"]+)"', line)
            if not m:
                continue
            inc = m.group(1)
            if inc.startswith("ledger/"):
                if not (root / "include" / inc).is_file():
                    print(f"FAIL: {rel}:{n}: {inc} is not under include/")
                    fail = 1
                continue
            if inc.startswith(external):
                continue
            if "/" not in inc and (path.parent / inc).is_file() and not rel.startswith("include/"):
                continue
            print(f'FAIL: {rel}:{n}: "{inc}" — include library headers as "ledger/<folder>/…"')
            fail = 1
sys.exit(fail)
PY

echo "OK: includes follow the library layout"
