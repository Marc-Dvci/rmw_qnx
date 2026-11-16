# Results

Every figure here comes from a transcript in [`results/`](../results), with the command that reproduces it. All
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
middleware (`scripts/inversion_ros2.sh trace`), and stops the guest. With three middlewares it took 3 minutes.

**Setup** ([`bench/inversion_ros2/src/inversion.cpp`](../bench/inversion_ros2/src/inversion.cpp), run by
[`scripts/guest/run_inversion.sh`](../scripts/guest/run_inversion.sh)): three processes, the same binary for every
middleware, which is chosen with `RMW_IMPLEMENTATION` and nothing else.

- A client node at priority 20 calls `example_interfaces/srv/AddTwoInts` every 10 ms and needs the reply within 5 ms.
- A server node at priority 10 does 1 ms of work in its callback and returns its own current priority in the reply.
- A load process at priority 15, with no ROS, is busy 20 ms of every 50 ms.
- The measured threads (the client's calling thread, the server's executor thread, the load) are pinned to CPU 0.
  The middleware's own threads keep their process's priority and may use either CPU.
- 200 requests per run, after 5 warm-up calls.

**Result** ([`results/inversion-ros2-20261107-080444.txt`](../results/inversion-ros2-20261107-080444.txt), the run of
`killtest_all.sh` after the send-queue fix of section 1f), every run:

| Middleware | Load | Runs | Deadline misses | Callback ran at the client's priority (20) | Requests sent mid-burst | Of those, the load finished first |
|---|---|---|---|---|---|---|
| Fast DDS | off | 1, 2, 3 | 0, 0, 0 | 0, 0, 0 | 0, 0, 0 | - |
| Cyclone DDS | off | 1, 2, 3 | 0, 0, 0 | 0, 0, 0 | 0, 0, 0 | - |
| rmw_qnx | off | 1, 2, 3 | 0, 1, 0 | 200, 200, 200 | 0, 0, 0 | - |
| Fast DDS | on | 1, 2, 3 | **40, 40, 40** | 0, 0, 0 | 40, 40, 40 | **40, 40, 40** |
| Cyclone DDS | on | 1, 2, 3 | **40, 40, 40** | 0, 0, 0 | 40, 40, 40 | **40, 40, 40** |
| rmw_qnx | on | 1, 2, 3 | **0, 0, 0** | **200, 200, 200** | 80, 80, 80 | **0, 0, 0** |

Over both DDS middlewares every request sent during a burst waited for the whole burst, because the server's
callback ran at its own priority 10, below the load. Over rmw_qnx the callback ran at the client's priority in every
call, and every request sent mid-burst finished ahead of the load. The DDS rows have fewer mid-burst requests because
a late client sends its next request late. Without the load, one rmw_qnx call missed in run 2; nothing
in the log says why. The earlier runs ([`-071150`](../results/inversion-ros2-20261107-071150.txt) after the send-queue fix,
[`-063223`](../results/inversion-ros2-20261107-063223.txt) before it) gave the same loaded counts; the events work of
section 1g did not change them.

**Every recorded loaded run**, all with the same scenario and 200 requests:

| Middleware | Runs | Deadline misses | Loaded calls where the load finished first | Files |
|---|---|---|---|---|
| Fast DDS | 9 | 360 of 1800 | 360 of 360 sent mid-burst | `inversion-ros2-20261107-063223`, `-071150`, `-080444` |
| Cyclone DDS | 13 | 520 of 2600 | 520 of 520 sent mid-burst | `-043659`, `-053325`, `-063223`, `-071150`, `-080444` |
| rmw_qnx | 15 | **3 of 3000** | **0 of 1200** sent mid-burst | `-051340`, `-053325`, `-060819`, `-063223`, `-064714`, `-071150`, `-080444` |

The three rmw_qnx misses (one each in `-051340`, `-053325` run 3, and `-060819`) were not requests the load delayed:
in no loaded rmw_qnx run did the load finish first.

**From the kernel's trace** ([`results/trace-ros2-20261107-080641/`](../results/trace-ros2-20261107-080641)): one
loaded run of 100 requests per middleware under `tracelogger` on the instrumented kernel, decoded on the host with
the SDP's `traceprinter` and summarised by thread:

```
wsl: scripts/trace_decode.sh results/trace-ros2-20261107-080641
python scripts/trace_summary.py results/trace-ros2-20261107-080641/<rmw>.trace results/trace-ros2-20261107-080641/<rmw>.pinv --threads
```

```
rmw_fastrtps_cpp: request waits 185; load held CPU 0 for more than 1 ms before the server's thread ran 21; longest 32.8 ms; server's thread ran at priority {10: 446, 20: 103}
rmw_cyclonedds_cpp: request waits 176; load held CPU 0 for more than 1 ms before the server's thread ran 21; longest 16.0 ms; server's thread ran at priority {10: 347}
rmw_qnx: request waits 105; load held CPU 0 for more than 1 ms before the server's thread ran 0; longest 0.1 ms; server's thread ran at priority {10: 3, 20: 316}
```

A request wait is an interval in which the client's calling thread was off CPU 0 and the server's executor thread
ran. In the kernel's record the load ran first, and held CPU 0 for more than 1 ms before the server's thread ran, in
21 waits on each DDS middleware and in none on rmw_qnx. Over rmw_qnx the server's thread ran at priority 20 in 316 of
its 319 scheduling events; the 3 at priority 10 are its start-up, before the first request. Over Fast DDS the
server's thread also ran at 20 in 103 of 549 events; the trace does not say what raised it (it was not enough to
keep the load from running first in 21 waits). The traced runs' own logs agree: 20 requests where the load finished
first on each DDS middleware, 0 on rmw_qnx.

The trace before the fix ([`trace-ros2-20261107-063419/`](../results/trace-ros2-20261107-063419)) gave 21, 21 and 0.
An earlier trace with two middlewares ([`results/trace-ros2-20261107-053447/`](../results/trace-ros2-20261107-053447))
gave the same picture: 21 Cyclone DDS waits, 0 rmw_qnx waits.

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

20 of 20 received, in order. (`subscribers_matched=0` is read after the listener had received its 20 and left.)

## 1c. `/dev/ros2`

**Command**: `bash scripts/inversion_ros2.sh up`, then `bash scripts/dev_ros2_demo.sh`. It starts `ros2graphd`
([`rmw_qnx/src/ros2graphd.cpp`](../rmw_qnx/src/ros2graphd.cpp)), a talker, a listener and a service server, then
runs `ls` and `cat` on `/dev/ros2`. The recorded session is [`docs/dev_ros2_session.txt`](dev_ros2_session.txt).

## 1d. The pendulum demo, unchanged

**Command**: `bash scripts/pendulum_run.sh 2000 rmw_qnx rmw_cyclonedds_cpp`
([`results/pendulum-20261107-062433.txt`](../results/pendulum-20261107-062433.txt)). ROS 2's own `pendulum_demo`
from [ros2/demos](https://github.com/ros2/demos) (Jazzy), built from its source with no change, run with `-i 2000`:

```
## RMW_IMPLEMENTATION=rmw_qnx pendulum_demo -i 2000
PendulumMotor received 998 messages
PendulumController received 998 messages
exit=0
## RMW_IMPLEMENTATION=rmw_cyclonedds_cpp pendulum_demo -i 2000
PendulumMotor received 989 messages
PendulumController received 990 messages
exit=0
```

Its dependency `rttest` needed one change for QNX ([`qnx/ros2/patches/rttest-rusage.patch`](../qnx/ros2/patches/rttest-rusage.patch)):
QNX has no `RUSAGE_THREAD`, so rttest counts page faults for the whole process. In this guest rttest also reports
`mallopt for mmap failed` and `Couldn't lock all cached virtual memory`, and goes on. The demo's latency statistics
are in the result file as raw output; they are not claims.

## 1e. ROS 2's own rmw test suite

**Command**: build once with `RMWQNX_TESTING=ON ONLY=1 bash qnx/ros2/build.sh ~/rmw_qnx-work test_rmw_implementation`
(WSL), then `bash scripts/inversion_ros2.sh up` and `bash scripts/rmw_suite.sh rmw_qnx rmw_cyclonedds_cpp rmw_fastrtps_cpp`
(rmw_qnx: [`results/test_rmw_implementation-20261107-080334.txt`](../results/test_rmw_implementation-20261107-080334.txt);
DDS: [`results/test_rmw_implementation-20261107-064622.txt`](../results/test_rmw_implementation-20261107-064622.txt);
summarised by [`scripts/suite_summary.py`](../scripts/suite_summary.py)). The suite is ROS 2 Jazzy's, unchanged: 16
test executables, each given 120 s.

```
rmw_qnx: 100 passed, 21 failed, of 125 tests run; executables that did not finish: test_serialize_deserialize
rmw_cyclonedds_cpp: 122 passed, 0 failed, of 126 tests run; executables that did not finish: test_create_destroy_node
rmw_fastrtps_cpp: 126 passed, 0 failed, of 126 tests run; executables that did not finish: test_create_destroy_node
```

"Tests run" counts the executables that finished; an executable that crashed is listed and its tests are not counted.
`test_create_destroy_node` crashed on both DDS middlewares in this guest and finished on rmw_qnx;
`test_serialize_deserialize` crashed on rmw_qnx after its C-message round trip failed (rmw_qnx serialises only C++
introspection types). The 21 rmw_qnx failures, by test suite: subscription use 6 (matched counts across endpoints, `take_sequence`,
ignoring local publications, no content filter set), content filters 3, events 4 (the matched-event tests set an event callback and expect a first status before any change; callbacks are not implemented), publisher
use 3, service 3, client 1, serialisation of C types 1, infinite durations 1. The graph suite, 15 failures before
graph queries existed, now passes. Earlier runs: 66 of 112 before argument checks and validation
([`-064053`](../results/test_rmw_implementation-20261107-064053.txt)), 85 of 125 before graph queries
([`-064622`](../results/test_rmw_implementation-20261107-064622.txt)).

## 1f. Guest checks of rmw_qnx

**Command**: `COPY=1 bash scripts/inversion_ros2.sh up`, then `bash scripts/rmw_checks.sh`
([`bench/inversion_ros2/src/rmw_checks.cpp`](../bench/inversion_ros2/src/rmw_checks.cpp)). Each check runs in its own
child process, with `ros2graphd` running.

| Check | Before the fixes ([`-070020`](../results/rmw_checks-20261107-070020.txt)) | After ([`-072311`](../results/rmw_checks-20261107-072311.txt)) |
|---|---|---|
| Two threads at 10 and 20 call one client while its sender is busy: each request is served at its caller's priority | FAIL: the request from 10 served at 20 | PASS: 20 at 20, 10 at 10 |
| The 33rd subscription on a topic is refused, 32 stay matched | FAIL: created, never woken | PASS |
| `rmw_deserialize` of `00 01 00 00 ff ff ff ff` as `rcl_interfaces/srv/GetParameters_Response` returns an error | FAIL: `std::bad_alloc`, abort | PASS |
| `rmw_deserialize` with a null buffer and length 8 returns `RMW_RET_INVALID_ARGUMENT` | FAIL: signal 11 | PASS |
| After its last endpoint goes, a topic is gone from `/dev/shmem` and `/dev/ros2/topics`, and its name takes another type | FAIL: still there | PASS |
| Two threads create and destroy endpoints on one topic 400 times: every publication reaches its subscription | FAIL only on the final "topic gone" step | PASS: 0 failed, 0 lost |
| Another process's node and publisher appear in the graph queries and `/dev/ros2/nodes`, and go when it ends | (added later) | PASS |

The first fix (one queue per client, taken in order) was not enough. With the queue fixed
([`-070536`](../results/rmw_checks-20261107-070536.txt)), the request from 10 still ran at 20. A trace of priorities
([`-070734`](../results/rmw_checks-20261107-070734.txt) shows that a received pulse or message sets the receiver's
priority both up and down) placed it: on receiving a request, the server pulsed its own wait set at its previous
priority, 20, and that pending pulse raised it again. The server no longer pulses the channel the request came in on
([`-071057`](../results/rmw_checks-20261107-071057.txt): 6 of 6).

## 1g. QoS events from QNX timers

**Command**: `bash scripts/rmw_checks.sh` ([`results/rmw_checks-20261107-080329.txt`](../results/rmw_checks-20261107-080329.txt): 9 passed,
0 failed). A publisher with a deadline owns a QNX timer (`timer_create` with `SIGEV_PULSE_INIT`) whose pulse goes to
the context's relay thread at the priority of the thread that created the event; the relay thread wakes the wait set.
No thread polls. Counts come from the clock when asked: one missed event per deadline period elapsed since the last
publication.

```
  healthy publisher, deadline 200 ms, a message every 20 ms for 1.5 s: 0 events
  quiet publisher: woke 100 ms after its last message; first status total 1, change 1
  after 450 ms of silence: total 4
  join: total 1 (+1), current 1 (+1)
  leave: total 1 (+0), current 0 (-1)
```

The "woke 100 ms" figure is raw output from a virtual machine, not a timing claim. What is claimed: 0 false events from
the healthy publisher, and exactly 4 events in 450 ms of silence on a 100 ms deadline. Matched events are woken by the
graph pulse and counted from per-topic join counters; they do not call `rmw_event_set_callback` callbacks, which the
4 `TestEvent` tests need. Requested-deadline (subscription), liveliness and incompatible-QoS events are not raised.
`test_rmw_implementation` after this work: 100 passed, 21 failed
([`results/test_rmw_implementation-20261107-080334.txt`](../results/test_rmw_implementation-20261107-080334.txt)), the same count as before.
An intermediate build ([`-075911`](../results/test_rmw_implementation-20261107-075911.txt), 99) failed `TestWaitSetUse.rmw_wait`
because a zero-initialised event was dereferenced. A second build ([`-080125`](../results/test_rmw_implementation-20261107-080125.txt), 99)
still failed it, because a subscription event of a type this implementation does not raise was refused at creation;
such events are now created and stay inert.

**Rings of dead processes.** A process that ends without destroying its endpoints leaves its topic ring in
`/dev/shmem` and its counts in the ring's header. Graph queries and `/dev/ros2` skip dead processes, the ring stays.
A freshly booted guest lists only live endpoints.

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

([`results/host_tests-20261107-072606.txt`](../results/host_tests-20261107-072606.txt), gcovr 8.8)

```
[==========] 63 tests from 13 test suites ran. (128 ms total)
[  PASSED  ] 63 tests.
lines: 96.3% (412 out of 428)
functions: 100.0% (69 out of 69)
branches: 71.1% (377 out of 530)
```

`SANITIZE=1 scripts/host_tests.sh` builds the same tests with AddressSanitizer and UndefinedBehaviorSanitizer; 63
passed. The core now also holds the client send queue (`sendq.hpp`) and the graph file format (`graph.hpp`).

Coverage is of `rmw_qnx/core/` (ring, CDR, names, graph and graph files, QoS, send queue), the code that compiles on
the host. The QNX layer (`rmw_qnx/src/`) is exercised by the guest runs of section 1.

**Do the tests fail when the core breaks?** `scripts/mutation_check.sh` applies twelve deliberate faults to a scratch
copy of the core, one at a time (torn-read detection removed, lost-message count zeroed, slot size check loosened,
string NUL check removed, forged-length check removed, no bound for an unknown element size, null buffer accepted,
send queue taking the oldest request whatever its priority, percent escaping broken, removal by owner ignoring the
owner, the durability rule removed, every deadline period counted as a miss) and rebuilds the tests against each.
Result: `mutants killed: 12 of 12` ([`results/mutation_check-20261107-072525.txt`](../results/mutation_check-20261107-072525.txt)).

The trace summary has its own tests: `python -m pytest -q tests/tools` (11 passed, random order).

## 4. Getting ROS 2 Jazzy to run on QNX 8.0 x86_64

Two faults stopped every ROS 2 process in the guest before `main` ran. Both are described, with reproduction, in
[`PORTING_NOTES.md`](PORTING_NOTES.md).
