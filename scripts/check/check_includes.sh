#!/usr/bin/env bash
# The library layout (docs/architecture/FOLDER_DEPENDENCY.md § Includes):
#   public   include/ledger/<folder>/X.h   included as "ledger/<folder>/X.h" — by anyone
#   private  src/<folder>/X.h              included as "<folder>/X.h" — by this repo's sources only
# A quoted include is one of those, a dependency's ("common/…" "crypto/…" "amp/…": pp-cpp-common /
# pp-cpp-crypto / pp-cpp-amp), or a file beside its includer (an app's or a test's own header). A
# public header includes public headers and dependencies only. The vendored CLI11 and cpp-httplib
# (src/lib/cli, src/lib/http) are not checked.
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
        public = rel.startswith("include/")
        for n, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
            m = re.match(r'\s*#\s*include\s+"([^"]+)"', line)
            if not m:
                continue
            inc = m.group(1)
            if inc.startswith("ledger/"):
                if not (root / "include" / inc).is_file():
                    print(f"FAIL: {rel}:{n}: {inc} is not a public header (include/)")
                    fail = 1
                continue
            if inc.startswith(external):
                continue
            if public:
                print(f'FAIL: {rel}:{n}: "{inc}" — a public header includes public ones ("ledger/…") only')
                fail = 1
                continue
            if "/" in inc and (root / "src" / inc).is_file():
                continue  # private
            if "/" not in inc and (path.parent / inc).is_file():
                continue  # beside its includer
            print(f'FAIL: {rel}:{n}: "{inc}" — "ledger/<folder>/X.h" (public) or "<folder>/X.h" (private)')
            fail = 1
sys.exit(fail)
PY

echo "OK: includes follow the library layout"
