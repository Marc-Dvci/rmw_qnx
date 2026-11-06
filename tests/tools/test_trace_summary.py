"""The trace summary counts who ran first in each request wait, from traceprinter lines."""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))

import trace_summary as ts  # noqa: E402

CLIENT, SERVER, LOAD, OTHER = 100, 200, 300, 1


def line(t: int, pid: int, prio: int, cpu: int = 0) -> str:
    return f"t:{t:#x} CPU:{cpu:02d} THREAD          :THRUNNING          pid:{pid} tid:1 priority:{prio} policy:1"


def summary(*runs: tuple[int, int]) -> ts.Summary:
    lines = [line(i + 1, pid, prio) for i, (pid, prio) in enumerate(runs)]
    return ts.summarise(ts.parse_runs(lines), CLIENT, SERVER, LOAD)


def test_a_server_that_runs_before_the_load_counts_as_server_first() -> None:
    s = summary((CLIENT, 20), (SERVER, 20), (LOAD, 15), (CLIENT, 20))
    assert (s.waits, s.server_first, s.load_first) == (1, 1, 0)


def test_a_load_that_runs_before_the_server_counts_as_load_first() -> None:
    s = summary((CLIENT, 20), (OTHER, 21), (LOAD, 15), (SERVER, 10), (CLIENT, 20))
    assert (s.waits, s.server_first, s.load_first) == (1, 0, 1)


def test_a_wait_in_which_the_server_never_ran_is_not_a_request_wait() -> None:
    s = summary((CLIENT, 20), (LOAD, 15), (CLIENT, 20))
    assert s.waits == 0


def test_the_server_priorities_are_counted() -> None:
    s = summary((CLIENT, 20), (SERVER, 20), (CLIENT, 20), (SERVER, 10))
    assert s.server_prios == {20: 1, 10: 1}


def test_events_on_other_cpus_are_ignored() -> None:
    lines = [line(1, CLIENT, 20), line(2, LOAD, 15, cpu=1), line(3, SERVER, 20), line(4, CLIENT, 20)]
    s = ts.summarise(ts.parse_runs(lines), CLIENT, SERVER, LOAD)
    assert (s.server_first, s.load_first) == (1, 0)


def test_events_are_ordered_by_time_not_by_line() -> None:
    lines = [line(4, CLIENT, 20), line(2, LOAD, 15), line(1, CLIENT, 20), line(3, SERVER, 10)]
    s = ts.summarise(ts.parse_runs(lines), CLIENT, SERVER, LOAD)
    assert (s.waits, s.load_first) == (1, 1)


def test_the_recorded_udp_run_shows_the_load_running_first() -> None:
    d = Path(__file__).resolve().parents[2] / "results" / "trace-20261107-003244"
    pinv = (d / "udp.pinv").read_text(encoding="utf-8")
    assert "load_ran_first=20" in pinv
