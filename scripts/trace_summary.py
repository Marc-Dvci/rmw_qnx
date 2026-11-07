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
CPS = re.compile(r"TRACE_CYCLES_PER_SEC::\s*(\d+)")


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


@dataclass
class ThreadSummary:
    waits: int
    load_held_first: int
    server_prios: Counter[int]
    load_ms_max: float


def parse_thread_runs(lines: list[str], cpu: int = 0) -> list[tuple[int, int, int, int]]:
    """(time, pid, tid, priority) of every THRUNNING event on `cpu`, in time order."""
    runs = []
    for line in lines:
        m = RUN.match(line)
        if m and int(m.group(2)) == cpu:
            runs.append((int(m.group(1), 16), int(m.group(3)), int(m.group(4)), int(m.group(5))))
    runs.sort()
    return runs


def summarise_threads(
    runs: list[tuple[int, int, int, int]], client: int, server: int, load: int, cps: int, hold_ms: float = 1.0
) -> ThreadSummary:
    """For processes with several threads (ROS 2 nodes): only thread 1 of the client (its calling thread) and of the
    server (its executor thread, which runs the callback) count; the middleware's other threads count as others.

    A request wait runs from the client's thread leaving CPU 0 to its next run there, and is one in which the
    server's thread ran. A thread runs from its THRUNNING event to the next one on the CPU. The load "held the CPU
    first" in a wait when it ran for more than `hold_ms` before the server's thread last started in that wait: the
    server's work then waited for the load."""
    s = ThreadSummary(0, 0, Counter(), 0.0)
    hold = hold_ms * cps / 1000.0
    load_cycles = 0  # load's run time on CPU 0 since the wait began
    before_server = 0  # load's run time up to the server thread's latest start in this wait
    server_ran = False
    started = False  # the first wait begins at the client's first run, not at the start of the trace
    for i, (t, pid, tid, prio) in enumerate(runs):
        dur = runs[i + 1][0] - t if i + 1 < len(runs) else 0
        if pid == client and tid == 1:
            if server_ran and started:
                s.waits += 1
                s.load_held_first += before_server > hold
                s.load_ms_max = max(s.load_ms_max, before_server * 1000.0 / cps)
            load_cycles, before_server, server_ran, started = 0, 0, False, True
        elif pid == server and tid == 1:
            s.server_prios[prio] += 1
            server_ran = True
            before_server = load_cycles
        elif pid == load:
            load_cycles += dur
    return s


def main(argv: list[str]) -> int:
    trace, pinv = Path(argv[1]), Path(argv[2])
    m = PIDS.search(pinv.read_text(encoding="utf-8"))
    if not m:
        print("no PIDS line in", pinv, file=sys.stderr)
        return 1
    client, server, load = (int(x) for x in m.groups())
    lines = trace.read_text(encoding="utf-8", errors="replace").splitlines()
    if "--threads" in argv:
        cm = CPS.search("\n".join(lines[:40]))
        cps = int(cm.group(1)) if cm else 1
        ts = summarise_threads(parse_thread_runs(lines), client, server, load, cps)
        prios = ", ".join(f"{p}: {n}" for p, n in sorted(ts.server_prios.items()))
        print(
            f"{trace.stem}: request waits {ts.waits}; load held CPU 0 for more than 1 ms before the server's "
            f"thread ran {ts.load_held_first}; longest {ts.load_ms_max:.1f} ms; "
            f"server's thread ran at priority {{{prios}}}"
        )
        return 0
    runs = parse_runs(lines)
    s = summarise(runs, client, server, load)
    prios = ", ".join(f"{p}: {n}" for p, n in sorted(s.server_prios.items()))
    print(
        f"{trace.stem}: CPU 0 THRUNNING events {len(runs)}; client waits {s.waits}; "
        f"server ran first {s.server_first}; load ran first {s.load_first}; server ran at priority {{{prios}}}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
