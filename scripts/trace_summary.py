"""Who ran first, from the kernel's own trace.

Reads the traceprinter output of a pinv run and the PIDS line pinv printed, keeps the THRUNNING events on CPU 0, and
counts, for each time the client stopped running (it sent a request and blocked), which of server or load the kernel
ran first before the client ran again. Also counts the priorities the server ran at.

    python scripts/trace_summary.py results/trace-<time>/udp.trace results/trace-<time>/udp.pinv
"""

from __future__ import annotations

import re
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path

RUN = re.compile(r"^t:(0x[0-9a-f]+) CPU:(\d+) THREAD\s+:THRUNNING\s+pid:(\d+) tid:(\d+) priority:(\d+)")
PIDS = re.compile(r"PIDS client=(\d+) server=(\d+) load=(-?\d+)")


@dataclass(frozen=True)
class Run:
    t: int
    pid: int
    prio: int


@dataclass
class Summary:
    waits: int
    server_first: int
    load_first: int
    server_prios: Counter[int]


def parse_runs(lines: list[str], cpu: int = 0) -> list[Run]:
    runs = []
    for line in lines:
        m = RUN.match(line)
        if m and int(m.group(2)) == cpu:
            runs.append(Run(int(m.group(1), 16), int(m.group(3)), int(m.group(5))))
    runs.sort(key=lambda r: r.t)
    return runs


def summarise(runs: list[Run], client: int, server: int, load: int) -> Summary:
    """A request wait runs from the client leaving the CPU to its next run, and is one in which the server ran (the
    server runs only to serve a request). Each one is classified by whether the load ran before the server."""
    s = Summary(0, 0, 0, Counter())
    seen: list[int] = []
    for r in [*runs, Run(-1, client, 0)]:
        if r.pid == client:
            if server in seen:
                s.waits += 1
                if load in seen and seen.index(load) < seen.index(server):
                    s.load_first += 1
                else:
                    s.server_first += 1
            seen = []
            continue
        if r.pid == server:
            s.server_prios[r.prio] += 1
        seen.append(r.pid)
    return s


def main(argv: list[str]) -> int:
    trace, pinv = Path(argv[1]), Path(argv[2])
    m = PIDS.search(pinv.read_text(encoding="utf-8"))
    if not m:
        print("no PIDS line in", pinv, file=sys.stderr)
        return 1
    client, server, load = (int(x) for x in m.groups())
    runs = parse_runs(trace.read_text(encoding="utf-8", errors="replace").splitlines())
    s = summarise(runs, client, server, load)
    prios = ", ".join(f"{p}: {n}" for p, n in sorted(s.server_prios.items()))
    print(
        f"{trace.stem}: CPU 0 THRUNNING events {len(runs)}; client waits {s.waits}; "
        f"server ran first {s.server_first}; load ran first {s.load_first}; server ran at priority {{{prios}}}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
