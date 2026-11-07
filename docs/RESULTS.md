# Results

Every figure here comes from a run recorded in [`results/`](../results), with the command that reproduces it. All
guest figures are **QEMU, WHPX**: QNX SDP 8.0 x86_64 under QEMU 11.1 with the Windows Hypervisor Platform, 2 vCPUs,
4 GB, on a Windows PC. They count who ran first, which priority a thread ran at, and deadline misses. They are not
microsecond timing claims: a virtual machine's timer is not a board's.

## 1. Priority across a request, at the transport level

**Command** (Git Bash on Windows, after `qnx/stage1.sh` in WSL): `bash scripts/pinv_run.sh 200`

**Setup** ([`bench/pinv/pinv.c`](../bench/pinv/pinv.c)): three processes pinned to CPU 0 with `ThreadCtl(_NTO_TCTL_RUNMASK)`.
A client at priority 20 sends one request every 10 ms and needs the reply within 5 ms. A server at priority 10 does
1 ms of computation per request. A load at priority 15 is busy for 20 ms of every 50 ms. The same request travels
over QNX `MsgSend`/`MsgReply`, a UDP socket on loopback (through io-sock), or a shared-memory slot with two
process-shared semaphores.

**Result** ([`results/pinv-20261107-002853.txt`](../results/pinv-20261107-002853.txt)), 200 requests per row:

| Transport | Load | Deadline misses | Server ran at client's priority (20) | Requests sent mid-burst | Of those, load finished first | Worst response |
|---|---|---|---|---|---|---|
| MsgSend/MsgReply | off | 0 | 200 | 0 | 0 | 1.9 ms |
| UDP | off | 0 | 0 | 0 | 0 | 1.8 ms |
| shared memory | off | 0 | 0 | 0 | 0 | 2.5 ms |
| MsgSend/MsgReply | on | **0** | **200** | 80 | **0** | 1.9 ms |
| UDP | on | **40** | 0 | 40 | **40** | 16.0 ms |
| shared memory | on | **40** | 0 | 40 | **40** | 15.1 ms |

Over UDP and shared memory, every request sent while the load was busy waited for the whole burst: the
priority-10 server could not run while the priority-15 load did, so the priority-20 client waited on the load. Over
message passing the server ran at 20 from the moment it received, ahead of the load, every time. (Over UDP and shared
memory only 40 requests were sent mid-burst, because a delayed client sends its next request late.)

"Server ran at client's priority" is the server's own reading of its current priority (`SchedGet`,
`sched_curpriority`) while it worked. "Load finished first" means the request's work ended after the burst that was
running when it was sent.

## 2. The same, from the kernel's trace

**Command**: `bash scripts/pinv_trace.sh`, then in WSL `scripts/trace_decode.sh results/trace-<time>` and
`python scripts/trace_summary.py results/trace-<time>/<mode>.trace results/trace-<time>/<mode>.pinv`

`tracelogger` ran for 2 s on the instrumented kernel (`procnto-smp-instr`) during 100 requests per transport, under
load. The `.kev` files are in [`results/trace-20261107-003244/`](../results/trace-20261107-003244); `traceprinter` from
the SDP decodes them on the host. The summary keeps the kernel's `THRUNNING` events on CPU 0 and, for every interval
in which the client was off the CPU and the server ran, asks which of server or load the kernel ran first.

| Transport | Request waits | Server ran first | Load ran first | Server's `THRUNNING` events at priority 20 / 10 |
|---|---|---|---|---|
| MsgSend/MsgReply | 104 | 104 | **0** | 109 / 3 |
| UDP | 170 | 149 | **21** | 1 / 250 |
| shared memory | 184 | 163 | **21** | 5 / 190 |

The kernel's view agrees with the programs' own log of the same runs (`.pinv` files: 0, 20 and 20 requests where the
load finished first). There are more waits than requests because the client also leaves the CPU inside a request
(for io-sock, for instance). The three priority-10 events over MsgSend are the server starting up, before its first
request.

## 3. Through ROS 2: the kill test

**Command** (Git Bash on Windows, after `qnx/ros2/build.sh` and `qnx/ros2/build_ours.sh` in WSL):

```
bash scripts/inversion_ros2.sh up
bash scripts/inversion_ros2.sh run 200 rmw_cyclonedds_cpp
bash scripts/inversion_ros2.sh run 200 rmw_qnx
bash scripts/inversion_ros2.sh down
```

**Setup** ([`bench/inversion_ros2/src/inversion.cpp`](../bench/inversion_ros2/src/inversion.cpp)): the scenario of
section 1 as three ROS 2 processes, unchanged between middlewares. A client node at priority 20 calls
`example_interfaces/srv/AddTwoInts` every 10 ms with a 5 ms deadline. A server node at priority 10 does 1 ms of
work in its callback and returns its own current priority. A load process at priority 15 is busy 20 ms of every 50 ms.
The measured threads (the client's calling thread, the server's executor thread, the load) are pinned to CPU 0; the
middleware's own threads keep their process's priority and may use either CPU. The middleware is chosen with
`RMW_IMPLEMENTATION` and nothing else. 200 requests per row, after 5 warm-up calls.

**Result** ([`results/inversion-ros2-20261107-043659.txt`](../results/inversion-ros2-20261107-043659.txt) for Cyclone DDS,
[`results/inversion-ros2-20261107-051340.txt`](../results/inversion-ros2-20261107-051340.txt) for rmw_qnx):

| Middleware | Load | Deadline misses | Callback ran at client's priority (20) | Requests sent mid-burst | Of those, load finished first | p50 | Worst |
|---|---|---|---|---|---|---|---|
| Cyclone DDS | off | 0 | 0 | 0 | 0 | 1.90 ms | 2.37 ms |
| rmw_qnx | off | 0 | 200 | 0 | 0 | 1.42 ms | 2.24 ms |
| Cyclone DDS | on | **40** | 0 | 40 | **40** | 1.87 ms | 14.08 ms |
| rmw_qnx | on | **1** | **200** | 80 | **0** | 2.13 ms | 5.98 ms |

Over Cyclone DDS every request sent during a burst waited for the whole burst, because the server's callback ran at
10. Over rmw_qnx the callback ran at the client's priority 20 for all 200 requests, and finished ahead of the load
every time it was contended. One rmw_qnx request took 5.98 ms, over the 5 ms deadline; it was not one where the load
ran first, and the run log does not say more about it. Run once each.

Fast DDS is not in this comparison yet: it is not built (`RMWQNX_SKIP` in [`qnx/ros2/build.sh`](../qnx/ros2/build.sh)).

### What it took to run ROS 2 on this guest

- The ROS 2 cross-build with build-files' toolchain file links shared objects with `--as-needed` before qcc adds
  `-lc`, so libc pieces from `libcS.a` were copied into, and exported from, many libraries. `qnx/ros2/build.sh` adds
  `-Wl,--no-as-needed`.
- QNX 8.0's x86_64 `memchr` crashes when the length is `SIZE_MAX` and the start pointer is not 8-byte aligned, which
  `rcutils_strdup` does. [`qnx/ros2/patches/memchr_size_max.c`](../qnx/ros2/patches/memchr_size_max.c) shows it in
  the guest (aligned start: ok; offsets 1 to 7: crashed);
  [`qnx/ros2/patches/rcutils-strndup.patch`](../qnx/ros2/patches/rcutils-strndup.patch) uses `strnlen` instead.
- With every middleware thread pinned to CPU 0, Cyclone DDS discovery never completed (a priority-20 thread kept the
  CPU from the priority-10 server's discovery). Only the measured threads are pinned now.

## 4. Host tests of the core

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
Result: `mutants killed: 9 of 9`. The forged-length mutant is killed by the test process aborting on a huge
allocation, which is the failure the check exists to prevent.
