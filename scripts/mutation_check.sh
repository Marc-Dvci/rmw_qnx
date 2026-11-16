#!/bin/bash
# Breaks the core on purpose, one mutation at a time, in a scratch copy, and checks that the host tests go red.
#   scripts/mutation_check.sh [SCRATCH]     default SCRATCH=~/rmw_qnx-work/mutants
set -uo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
S="${1:-$HOME/rmw_qnx-work/mutants}"
# file | original text | mutated text | what the mutation breaks
MUTANTS=(
  "ring.hpp|if (s->lock.load(std::memory_order_relaxed) != before) {|if (false) {|torn-read detection"
  "ring.hpp|r.lost = want - *pos;|r.lost = 0;|lost-message count"
  "ring.hpp|if (length > h_->slot_payload) return UINT64_MAX;|if (length > h_->slot_payload + 8) return UINT64_MAX;|slot size check"
  "cdr.hpp|if (p_[pos_ + n - 1] != 0) return ok_ = false;|(void)0;|string NUL check"
  "cdr.hpp|if (*n > (len_ - pos_) / min_elem_size) return ok_ = false;|(void)0;|forged length check"
  "cdr.hpp|if (min_elem_size == 0) min_elem_size = 1;|if (min_elem_size == 0) return true;|unbounded length for an unknown element size"
  "cdr.hpp|ok_ = data != nullptr && len >= 4|ok_ = len >= 4|null buffer check"
  "sendq.hpp|if (it->prio == prio) {|if (true) {|request taken by its pulse's priority"
  "names.hpp|out += \"%25\";|out += '%';|percent escaping"
  "graph.hpp|if (it->second.first == o) {|if (true) {|removal by owner"
  "qos.hpp|err(\"ERROR: Volatile publisher and transient local subscription;\");|(void)0;|durability rule"
  "qos.hpp|const bool miss = !seen_;|const bool miss = true;|deadline miss rule"
)
pass=0
for m in "${MUTANTS[@]}"; do
  IFS='|' read -r file from to what <<<"$m"
  rm -rf "$S/work"
  mkdir -p "$S/work/rmw_qnx" "$S/work/tests"
  cp -r "$REPO/rmw_qnx/core" "$S/work/rmw_qnx/"
  cp -r "$REPO/tests/core" "$S/work/tests/"
  python3 - "$S/work/rmw_qnx/core/$file" "$from" "$to" <<'EOF' || { echo "NOT APPLIED $what"; continue; }
import sys
p, a, b = sys.argv[1], sys.argv[2], sys.argv[3]
t = open(p).read()
assert a in t, f"mutation site not found: {a}"
open(p, "w").write(t.replace(a, b, 1))
EOF
  if cmake -S "$S/work/tests/core" -B "$S/build" -DFETCHCONTENT_BASE_DIR="$S/deps" >/dev/null 2>&1 &&
     cmake --build "$S/build" -j4 >/dev/null 2>&1 &&
     "$S/build/core_tests" --gtest_brief=1 >/dev/null 2>&1; then
    echo "SURVIVED  $what ($file)"
  else
    echo "killed    $what ($file)"
    pass=$((pass + 1))
  fi
done
echo "mutants killed: $pass of ${#MUTANTS[@]}"
