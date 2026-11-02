#!/bin/bash
# Weekly Mac-parity check. docs/mac-parity.md records the Mac commit the
# feature list was last reconciled against ("Checked against phibrowser-mac
# <hash>"). This prints every Mac commit since that hash, feature-bearing
# ones first, with the user-facing areas they touched, so the weekly pass
# has a bounded reading list instead of the whole Mac tree. It changes
# nothing; after the pass, bump the hash and date in docs/mac-parity.md.
#
#   tools/mac-parity-check.sh            # since the hash in the doc
#   tools/mac-parity-check.sh <hash>     # since an explicit Mac commit
#   tools/mac-parity-check.sh --table    # one markdown row per commit, verdict
#                                        # column empty, to paste into the
#                                        # doc's Triage log and fill in
#
# Reads origin/dev, not the checkout's HEAD: that checkout may sit behind, and
# then a bumped hash would read as "0 commits". PHI_MAC_REF overrides the ref;
# run `git fetch origin` in the Mac checkout first.
#
# Every commit gets a verdict in the log: port (Windows shares the
# behaviour; say what and where), skip (Mac-only, AI suite, platform
# notion; one-line reason) or watch (waits on a 🔧 row; name it).
set -uo pipefail
cd "$(dirname "$0")/.."
MAC="${PHI_MAC_SRC:-../phibrowser-mac}"
DOC=docs/mac-parity.md
if [[ ! -d $MAC/.git ]]; then
  echo "skip: no Mac checkout at $MAC (set PHI_MAC_SRC)"
  exit 0
fi
table=0
if [[ ${1:-} == --table ]]; then table=1; shift; fi
since="${1:-$(grep -oE 'Checked against phibrowser-mac `?[0-9a-f]{7,40}' "$DOC" | grep -oE '[0-9a-f]{7,40}$' | head -1)}"
if [[ -z $since ]]; then
  echo "error: no 'Checked against phibrowser-mac <hash>' line in $DOC and no hash given"
  exit 1
fi
if ! git -C "$MAC" cat-file -e "$since^{commit}" 2>/dev/null; then
  echo "error: $since is not a commit in $MAC (run git fetch there?)"
  exit 1
fi
ref="${PHI_MAC_REF:-origin/dev}"
if ! git -C "$MAC" rev-parse --verify -q "$ref^{commit}" >/dev/null; then
  echo "error: no $ref in $MAC (run git fetch origin there, or set PHI_MAC_REF)"
  exit 1
fi
head=$(git -C "$MAC" rev-parse --short "$ref")
total=$(git -C "$MAC" rev-list --count "$since..$ref")
if (( table )); then
  echo "### $(date +%F) — phibrowser-mac $since → $head ($total commits)"
  echo
  echo "| Mac commit | Subject | Verdict | Windows action / reason |"
  echo "|---|---|---|---|"
  git -C "$MAC" log --reverse --format='%h%x09%s' "$since..$ref" \
    | while IFS=$'\t' read -r h s; do printf '| `%s` | %s | | |\n' "$h" "${s//|/\\|}"; done
  exit 0
fi
echo "phibrowser-mac: $total commit(s) from $since to $head ($(git -C "$MAC" log -1 --format=%ad --date=short "$ref"))"
echo

# User-facing areas: a commit touching none of these is almost always
# invisible to a parity list (tests, perf, Mac-only rendering plumbing).
areas='Sources/UserInterface|Sources/Application/AppController\+Menu|Sources/UserInterface/Preferences|Sources/States|Sources/CommandDispatcher|Sources/ChromiumBridge/AgentCDP|Resources/Localizable.xcstrings|^docs/'

print_group() {
  local title=$1 pattern=$2
  local rows
  rows=$(git -C "$MAC" log --reverse --format='%h%x09%ad%x09%s' --date=short "$since..$ref" | grep -E "	$pattern" || true)
  [[ -z $rows ]] && return
  echo "## $title"
  while IFS=$'\t' read -r h d s; do
    touched=$(git -C "$MAC" show --stat --format= --name-only "$h" | grep -E "$areas" | sed -E 's#^Sources/UserInterface/([^/]+).*#\1#; s#^Sources/States/([^/]+).*#States/\1#; s#^Sources/([^/]+).*#\1#; s#^docs/(.*)\.md#docs:\1#; s#^Resources/.*#strings#' | sort -u | tr '\n' ' ')
    printf '  %s  %s  %s\n' "$h" "$d" "$s"
    [[ -n $touched ]] && printf '             → %s\n' "$touched"
  done <<<"$rows"
  echo
}

print_group "feat (new user-facing behaviour — needs a row or a decision)" 'feat'
print_group "fix / improve (check only if the Windows port shares the code path)" '(fix|improve)'
print_group "docs (a contract may have moved)" 'docs'
print_group "everything else (perf / refactor / test / chore — skim the subjects)" '(perf|refactor|test|chore|ci|build|style)'

echo "New Mac strings since $since (new NSLocalizedString keys usually mean a new surface):"
git -C "$MAC" diff "$since..$ref" -- Resources/Localizable.xcstrings 2>/dev/null \
  | grep -E '^\+ {4}"[^"]+" : \{' | sed -E 's/^\+ {4}"([^"]+)".*/  \1/' | head -80
echo
echo "When done: update 'Checked against phibrowser-mac $head' and the date in $DOC."
exit 0
