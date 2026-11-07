# Results

Every figure here comes from a run recorded in [`results/`](../results), with the command that reproduces it. All
guest figures are **QEMU, WHPX**: QNX SDP 8.0 x86_64 under QEMU 11.1 with the Windows Hypervisor Platform, 2 vCPUs,
4 GB, on a Windows PC. The claims are counts: deadline misses, the priority a thread ran at, and who ran first. The
result files also hold the programs' raw response times in microseconds; they are raw output from a virtual machine,
not timing claims.

## 1. The kill test: priority inversion through ROS 2

**Command** (Git Bash on Windows, after the build in [the README](../README.md#build-and-run)):

```
bash scripts/killtest_all.sh
```

It boots the guest, removes `/data/ros2`, installs the ROS 2 build from the work folder, runs the scenario three
times per middleware with and without the load (`scripts/inversion_ros2.sh run`), runs one traced loaded run per
middleware (`scripts/inversion_ros2.sh trace`), and stops the guest. It took 3 minutes.

**Setup** ([`bench/inversion_ros2/src/inversion.cpp`](../bench/inversion_ros2/src/inversion.cpp), run by
[`scripts/guest/run_inversion.sh`](../scripts/guest/run_inversion.sh)): three processes, the same binary for every
middleware, which is chosen with `RMW_IMPLEMENTATION` and nothing else.

- A client node at priority 20 calls `example_interfaces/srv/AddTwoInts` every 10 ms and needs the reply within 5 ms.
- A server node at priority 10 does 1 ms of work in its callback and returns its own current priority in the reply.
- A load process at priority 15, with no ROS, is busy 20 ms of every 50 ms.
- The measured threads (the client's calling thread, the server's executor thread, the load) are pinned to CPU 0.
  The middleware's own threads keep their process's priority and may use either CPU.
- 200 requests per run, after 5 warm-up calls.

**Result** ([`results/inversion-ros2-20261107-053325.txt`](../results/inversion-ros2-20261107-053325.txt)), every run:

| Middleware | Load | Run | Deadline misses | Callback ran at the client's priority (20) | Requests sent mid-burst | Of those, the load finished first |
|---|---|---|---|---|---|---|
| Cyclone DDS | off | 1, 2, 3 | 0, 0, 0 | 0, 0, 0 | 0, 0, 0 | - |
| rmw_qnx | off | 1, 2, 3 | 0, 0, 0 | 200, 200, 200 | 0, 0, 0 | - |
| Cyclone DDS | on | 1, 2, 3 | **40, 40, 40** | 0, 0, 0 | 40, 40, 40 | **40, 40, 40** |
| rmw_qnx | on | 1, 2, 3 | **0, 0, 1** | **200, 200, 200** | 80, 80, 80 | **0, 0, 0** |

Over Cyclone DDS every request sent during a burst waited for the whole burst, because the server's callback ran at
its own priority 10, below the load. Over rmw_qnx the callback ran at the client's priority in all 600 loaded calls,
and in every one of the 240 sent mid-burst it finished ahead of the load. Cyclone DDS has fewer mid-burst requests
because a late client sends its next request late.

The one rmw_qnx miss (run 3 under load) was not a request the load delayed: no loaded rmw_qnx request had the load
finish first. Its cause is not identified from this run; the scenario log does not record which request it was.
An earlier single run ([`results/inversion-ros2-20261107-051340.txt`](../results/inversion-ros2-20261107-051340.txt))
also had one such miss; Cyclone DDS had 40 in its earlier run
([`results/inversion-ros2-20261107-043659.txt`](../results/inversion-ros2-20261107-043659.txt)).

**From the kernel's trace** ([`results/trace-ros2-20261107-053447/`](../results/trace-ros2-20261107-053447)): one
loaded run of 100 requests per middleware under `tracelogger` on the instrumented kernel, decoded on the host with the
SDP's `traceprinter` and summarised by thread:

```
wsl: scripts/trace_decode.sh results/trace-ros2-20261107-053447
python scripts/trace_summary.py results/trace-ros2-20261107-053447/<rmw>.trace results/trace-ros2-20261107-053447/<rmw>.pinv --threads
```

```
rmw_cyclonedds_cpp: request waits 163; load held CPU 0 for more than 1 ms before the server's thread ran 21; longest 12.9 ms; server's thread ran at priority {10: 344}
rmw_qnx: request waits 105; load held CPU 0 for more than 1 ms before the server's thread ran 0; longest 0.1 ms; server's thread ran at priority {10: 4, 20: 172}
```

A request wait is an interval in which the client's calling thread was off CPU 0 and the server's executor thread
ran. In the kernel's record, the load ran first and held CPU 0 for up to 12.9 ms in 21 Cyclone DDS waits; over
rmw_qnx it never held the CPU longer than 0.1 ms before the server's thread ran, and the server's thread ran at
priority 20 in 172 of its 176 scheduling events (the 4 at priority 10 are its start-up, before the first request).
The traced runs' own logs agree: 20 Cyclone DDS requests where the load finished first, 0 for rmw_qnx.

Fast DDS is not in this comparison: it is not built yet.

**After topics were added to rmw_qnx**, one more rmw_qnx run (`bash scripts/inversion_ros2.sh run 200 rmw_qnx`,
[`results/inversion-ros2-20261107-060819.txt`](../results/inversion-ros2-20261107-060819.txt)): no load, 0 misses;
under load, 1 miss of 200, the callback at priority 20 for 200 of 200, and the load finishing first in 0 of 80
mid-burst requests. That makes 2 misses in 800 loaded rmw_qnx calls across all recorded runs, none of them a request
the load delayed, against 160 in 800 for Cyclone DDS (four runs of 200, all where the load finished first).

## 1b. Topics through rmw_qnx

**Command**: `bash scripts/inversion_ros2.sh up`, then `bash scripts/topics_check.sh 20`
([`bench/inversion_ros2/src/chatter.cpp`](../bench/inversion_ros2/src/chatter.cpp)): a listener process and a talker
process on `/chatter` (`example_interfaces/msg/String`), 20 messages 100 ms apart.

[`results/topics-20261107-060803.txt`](../results/topics-20261107-060803.txt), rmw_qnx:

```
CHATTER published=20 subscribers_matched=0
I heard: [hello 1]
I heard: [hello 2]
I heard: [hello 3]
CHATTER received=20 last=[hello 20] publishers_matched=1
```

20 of 20 received, in order. (`subscribers_matched=0` is read after the listener had received its 20 and left.) The
same check on Cyclone DDS in that session did not run: ssh to the guest timed out during its banner exchange.

## 2. The same property at the transport level

**Command** (Git Bash on Windows, after `qnx/stage1.sh` in WSL): `bash scripts/pinv_run.sh 200`

**Setup** ([`bench/pinv/pinv.c`](../bench/pinv/pinv.c)): three processes pinned to CPU 0 with
`ThreadCtl(_NTO_TCTL_RUNMASK)`, with the priorities, periods, work and deadline of section 1. The same request
travels over QNX `MsgSend`/`MsgReply`, a UDP socket on loopback (through io-sock), or a shared-memory slot with two
process-shared semaphores.

**Result** ([`results/pinv-20261107-002853.txt`](../results/pinv-20261107-002853.txt)), 200 requests per row:

| Transport | Load | Deadline misses | Server ran at client's priority (20) | Requests sent mid-burst | Of those, load finished first |
|---|---|---|---|---|---|
| MsgSend/MsgReply | off | 0 | 200 | 0 | 0 |
| UDP | off | 0 | 0 | 0 | 0 |
| shared memory | off | 0 | 0 | 0 | 0 |
| MsgSend/MsgReply | on | **0** | **200** | 80 | **0** |
| UDP | on | **40** | 0 | 40 | **40** |
| shared memory | on | **40** | 0 | 40 | **40** |

"Server ran at client's priority" is the server's own reading of its current priority (`SchedGet`,
`sched_curpriority`) while it worked. "Load finished first" means the request's work ended after the burst that was
running when it was sent.

**From the kernel's trace**: `bash scripts/pinv_trace.sh`, then `scripts/trace_decode.sh results/trace-<time>` in
WSL and `python scripts/trace_summary.py results/trace-<time>/<mode>.trace results/trace-<time>/<mode>.pinv`.
`tracelogger` ran for 2 s during 100 requests per transport, under load
([`results/trace-20261107-003244/`](../results/trace-20261107-003244)). The summary keeps the `THRUNNING` events on
CPU 0 and, for every interval in which the client was off the CPU and the server ran, asks which of server or load
the kernel ran first.

| Transport | Request waits | Server ran first | Load ran first | Server's `THRUNNING` events at priority 20 / 10 |
|---|---|---|---|---|
| MsgSend/MsgReply | 104 | 104 | **0** | 109 / 3 |
| UDP | 170 | 149 | **21** | 1 / 250 |
| shared memory | 184 | 163 | **21** | 5 / 190 |

The programs' own logs of the same runs agree (0, 20 and 20 requests where the load finished first). There are more
waits than requests because the client also leaves the CPU inside a request (for io-sock, for instance).

## 3. Host tests of the core

**Command** (Linux or WSL): `scripts/host_tests.sh`; with coverage: `COVERAGE=1 scripts/host_tests.sh` (needs gcovr)

```
[==========] 53 tests from 11 test suites ran. (40 ms total)
[  PASSED  ] 53 tests.
lines: 95.8% (248 out of 259)
functions: 100.0% (59 out of 59)
branches: 70.1% (185 out of 264)
```

Coverage is of `rmw_qnx/core/` only (ring, CDR, names, graph, QoS), the code that compiles on the host. The QNX layer
(`rmw_qnx/src/`) is not in this figure.

**Do the tests fail when the core breaks?** `scripts/mutation_check.sh` applies nine deliberate faults to a scratch
copy of the core, one at a time (torn-read detection removed, lost-message count zeroed, slot size check loosened,
string NUL check removed, forged-length check removed, percent escaping broken, removal by owner ignoring the owner,
the durability rule removed, every deadline period counted as a miss) and rebuilds the tests against each.
Result: `mutants killed: 9 of 9`.

The trace summary has its own tests: `python -m pytest -q --noconftest -o addopts="" tests/tools` (11 passed).

## 4. Getting ROS 2 Jazzy to run on QNX 8.0 x86_64

Two faults stopped every ROS 2 process in the guest before `main` ran. Both are described, with reproduction, in
[`PORTING_NOTES.md`](PORTING_NOTES.md).
