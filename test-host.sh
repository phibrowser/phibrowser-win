#!/bin/bash
# Tier 0 of the test pyramid (docs/architecture-review.md §3.8): everything
# that can be proved on THIS machine, with no Windows box, no network and no
# Chromium build. Fast enough to be a pre-commit hook.
#
#   ./test-host.sh            run every check
#   ./test-host.sh --list     name the checks and exit
#
# It collects ALL failures rather than stopping at the first, because a run
# that reports one broken thing at a time costs a round trip per fix.
set -uo pipefail
cd "$(dirname "$0")"

FAILED=()
run() {  # run <name> <command...>
  local name="$1"; shift
  printf '\n=== %s\n' "$name"
  if "$@"; then
    printf 'ok: %s\n' "$name"
  else
    printf 'FAIL: %s\n' "$name"
    FAILED+=("$name")
  fi
}

if [[ "${1:-}" == "--list" ]]; then
  grep -oP '(?<=^  run ")[^"]+' "$0"
  exit 0
fi

# 1. The host-compiled C++ tests: the pure modules the client actually runs,
#    not copies of them.
if [[ -f test/model/run.sh ]]; then
  run "host unit tests" bash test/model/run.sh
else
  # The suite is not in the public repository; its absence is the
  # checkout's, not a failure of the code.
  printf '\n=== host unit tests\nskip: test/model/run.sh not in this checkout\n'
fi

# 2. JS that needs no browser. selftest-mirror runs entirely on fixtures and
#    was wired to nothing before this file existed.
mirror=tools/phi-browser-skill/scripts/selftest-mirror.mjs
if [[ -f $mirror ]] && command -v sqlite3 >/dev/null; then
  run "skill mirror selftest" node "$mirror"
elif [[ -f $mirror ]]; then
  # It reads a session database through the sqlite3 binary; without it the
  # check would fail for the machine's sake rather than the code's.
  printf '\n=== skill mirror selftest\nskip: sqlite3 not installed\n'
fi

# 3. Every JS file at least parses. A suite that cannot be imported fails on
#    the box as a connection error, which reads as a product bug.
syntax_check() {
  local bad=0
  while IFS= read -r f; do
    node --check "$f" || { echo "  bad syntax: $f"; bad=1; }
  done < <(find test tools -name '*.mjs' -not -path '*/node_modules/*')
  return $bad
}
run "javascript syntax" syntax_check

# 4. Shell scripts parse too — a runner that dies on an unbound variable
#    wastes a whole box run before anyone sees it.
shell_check() {
  local bad=0
  for f in ./*.sh tools/*.sh test/model/run.sh; do
    [[ -f $f ]] || continue
    bash -n "$f" || { echo "  bad syntax: $f"; bad=1; }
  done
  return $bad
}
run "shell syntax" shell_check

# 5. The string catalogue is generated; a stale one ships the wrong words.
if [[ -f tools/gen-strings-catalog.py ]]; then
  run "strings catalogue is current" python3 tools/gen-strings-catalog.py --check
fi

# 6. Every PhiStr key the client asks for must exist, or the UI silently
#    shows the bare key (docs/i18n.md).
strings_keys() {
  local src=client/resources/strings-source.json
  [[ -f $src ]] || { echo "  no $src"; return 1; }
  local missing=0
  while IFS= read -r key; do
    grep -q "\"$key\"" "$src" || { echo "  missing key: $key"; missing=1; }
  # A key split over two lines ("app.foo." "bar") is one literal to C++,
  # so join adjacent literals before reading the keys.
  done < <(perl -0pe 's/"\s*\n\s*"//g' client/*.cc |
           grep -oP '(?<=PhiStrF?\(")[^"]+' | sort -u)
  return $missing
}
run "every PhiStr key exists" strings_keys

# 7. The agent-facing verb reference is generated from the C++ table.
if [[ -f tools/check-verbs-md.sh ]]; then
  run "verb reference is current" bash tools/check-verbs-md.sh
fi

# 8. The pure modules must stay pure, or tier 0 stops being possible. This is
#    the rule that keeps <windows.h> from creeping back into a model header.
pure_headers() {
  local bad=0
  while IFS= read -r cc; do
    local h="${cc%.cc}.h"
    [[ -f $h ]] || continue
    # An include INSIDE `#if defined(_WIN32)` is fine — that is the split
    # these headers use so their pure half still compiles here. Only an
    # unguarded one breaks tier 0.
    if awk '
        /^#if[[:space:]]+defined\(_WIN32\)/ { win = 1 }
        /^#endif/ { if (win) win = 0 }
        # base/memory/raw_ptr_exclusion.h is the ONE base/ header the host
        # supplies as a stub (test/model/stubs); anything else is real.
        !win && /^#include (<windows\.h>|"base\/)/ &&
          $0 !~ /raw_ptr_exclusion\.h/ { found = 1 }
        END { exit found ? 0 : 1 }' "$h"; then
      echo "  not host-pure: $h"
      bad=1
    fi
  done < <(grep -oP '(?<=\.\./\.\./client/)[a-z_]+\.cc' test/model/run.sh | sed 's|^|client/|' | sort -u)
  return $bad
}
run "host-compiled headers stay pure" pure_headers

# 9. The client's main is a deliberate copy of Chromium's; drift outside the
#    labelled phi blocks means an upstream change was silently dropped.
if [[ -f tools/check-main-diff.sh ]]; then
  run "phi_client_main_win matches upstream" bash tools/check-main-diff.sh
fi

printf '\n'
if ((${#FAILED[@]} == 0)); then
  echo "HOST TESTS PASSED"
  exit 0
fi
printf 'HOST TESTS FAILED (%d): %s\n' "${#FAILED[@]}" "${FAILED[*]}"
exit 1
