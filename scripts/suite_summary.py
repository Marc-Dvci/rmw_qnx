"""Passed and failed counts per middleware from a scripts/rmw_suite.sh output file.

    python scripts/suite_summary.py results/test_rmw_implementation-<time>.txt
"""

from __future__ import annotations

import re
import sys
from dataclasses import dataclass, field


@dataclass
class Tally:
    run: int = 0
    passed: int = 0
    failed: int = 0
    unfinished: list[str] = field(default_factory=list)


def summarise(lines: list[str]) -> dict[str, Tally]:
    out: dict[str, Tally] = {}
    cur = exe = ""
    finished = False

    def close() -> None:
        if cur and not finished:
            out[cur].unfinished.append(exe)

    for line in lines:
        m = re.match(r"## (\S+) (\S+)", line)
        if m:
            close()
            cur, exe, finished = m.group(1), m.group(2), False
            out.setdefault(cur, Tally())
            continue
        if not cur:
            continue
        t = out[cur]
        if m := re.match(r"\[==========\] (\d+) tests? from .* ran", line):
            t.run += int(m.group(1))
            finished = True
        elif m := re.match(r"\[  PASSED  \] (\d+) tests?", line):
            t.passed += int(m.group(1))
        elif m := re.match(r"\[  FAILED  \] (\d+) tests?, listed below", line):
            t.failed += int(m.group(1))
    close()
    return out


def main(argv: list[str]) -> int:
    with open(argv[1], encoding="utf-8") as f:
        for name, t in summarise(f.read().splitlines()).items():
            rest = f"; executables that did not finish: {' '.join(t.unfinished)}" if t.unfinished else ""
            print(f"{name}: {t.passed} passed, {t.failed} failed, of {t.run} tests run{rest}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
