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

## 3. Through ROS 2

Not measured yet. [`scripts/inversion_ros2.sh`](../scripts/inversion_ros2.sh) runs the same scenario as three ROS 2
processes (a service server, a client and the load, [`bench/inversion_ros2`](../bench/inversion_ros2)) on Fast DDS,
Cyclone DDS and rmw_qnx.

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
