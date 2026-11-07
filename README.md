# rmw_qnx

[![Tests](https://github.com/Marc-Dvci/rmw_qnx/actions/workflows/tests.yml/badge.svg)](https://github.com/Marc-Dvci/rmw_qnx/actions/workflows/tests.yml)
[![License: Apache-2.0](https://img.shields.io/badge/license-Apache--2.0-blue)](LICENSE)
[![Core tests](https://img.shields.io/badge/core%20tests-53%20passed-brightgreen)](docs/RESULTS.md#3-host-tests-of-the-core)
[![Core line coverage](https://img.shields.io/badge/core%20line%20coverage-95.8%25-brightgreen)](docs/RESULTS.md#3-host-tests-of-the-core)

**A ROS 2 middleware for QNX 8.0 that carries messages through the kernel's own message passing, so a robot's
high-priority node stays high-priority when it calls a low-priority one.** For teams running ROS 2 on QNX who need
a safety node's request to beat background work.

An application changes one line, and no code:

```sh
export RMW_IMPLEMENTATION=rmw_qnx
```

| Track | Built with | Headline (QEMU, WHPX) |
|---|---|---|
| Open-Source Contribution & Ports | QNX SDP 8.0, ROS 2 Jazzy, `MsgSend`/`MsgReply`, pulses, a resource manager, `tracelogger` | Under a medium-priority load, a priority-20 client's ROS 2 service calls missed their 5 ms deadline **40 times in 200** on Fast DDS and on Cyclone DDS, in each of three runs, and **0 times** on rmw_qnx in each of three runs. On rmw_qnx the server's callback ran at the client's priority in all 600 calls. |

There is no live link: rmw_qnx runs inside QNX. The terminal sessions below are recorded from a QNX 8.0 guest.

[For judges](#for-judges) · [See it work](#see-it-work) · [How it works](#how-it-works) ·
[Evidence of QNX use](#evidence-of-qnx-use) · [Evidence](#evidence) · [Build and run](#build-and-run)

## For judges

| Criterion | Where it is answered |
|---|---|
| Technological implementation | [`rmw_qnx/src/rmw_qnx.cpp`](rmw_qnx/src/rmw_qnx.cpp) (the ROS 2 middleware interface over QNX channels), [`rmw_qnx/src/topics.hpp`](rmw_qnx/src/topics.hpp) (topics), [`rmw_qnx/src/ros2graphd.cpp`](rmw_qnx/src/ros2graphd.cpp) (`/dev/ros2`), [`rmw_qnx/core/`](rmw_qnx/core) (host-tested core), [`docs/PORTING_NOTES.md`](docs/PORTING_NOTES.md) (what it took to run ROS 2 on QNX 8.0 x86_64) |
| Design | One environment variable; `ls /dev/ros2/topics` and `cat` on a topic ([session](docs/dev_ros2_session.txt)); ROS 2's `pendulum_control` demo unchanged ([result](docs/RESULTS.md#1d-the-pendulum-demo-unchanged)) |
| Potential impact | [The problem](#the-problem) |
| Innovation | Priority inheritance carried through ROS 2's asynchronous service call ([How it works](#how-it-works)) |
| Best use of QNX-specific technologies | [Evidence of QNX use](#evidence-of-qnx-use): message passing, pulses, a resource manager, the instrumented kernel |

## See it work

The graph is a QNX resource manager, so the shell is the browser
([recorded session](docs/dev_ros2_session.txt), `bash scripts/dev_ros2_demo.sh`):

```
# ls /dev/ros2/topics
chatter
parameter_events

# cat /dev/ros2/topics/chatter
topic:        /chatter
type:         example_interfaces/msg/String
publishers:   1
subscribers:  1
written:      108 messages since the topic was created
transport:    shared-memory ring, 64 slots of 16384 bytes, /rmwqnx.d0.%2Fchatter
latest:       message 108 in the ring, number 21 from its publisher
  data: hello 21

# cat /dev/ros2/services/inversion
service:      /inversion
server:       pid 1310744, channel 1, service 2
transport:    MsgSend/MsgReply: the server's thread runs at the calling client's priority
```

The kill test, with one command (`bash scripts/killtest_all.sh`, [full output](results/inversion-ros2-20261107-063223.txt)),
loaded runs:

```
RESULT rmw=rmw_fastrtps_cpp   requests=200 misses=40 contended=40 load_ran_first=40 served_at_client_prio=0
RESULT rmw=rmw_cyclonedds_cpp requests=200 misses=40 contended=40 load_ran_first=40 served_at_client_prio=0
RESULT rmw=rmw_qnx            requests=200 misses=0  contended=80 load_ran_first=0  served_at_client_prio=200
```

(one of three runs per middleware shown; columns trimmed for width.) The kernel's own trace of one run each
(`scripts/trace_summary.py --threads`):

```
rmw_fastrtps_cpp:   load held CPU 0 for more than 1 ms before the server's thread ran 21 times
rmw_cyclonedds_cpp: load held CPU 0 for more than 1 ms before the server's thread ran 21 times
rmw_qnx:            load held CPU 0 for more than 1 ms before the server's thread ran 0 times
```

## The problem

ROS 2 on QNX today talks through DDS: QNX's own ROS 2 Jazzy port builds Fast DDS, Cyclone DDS and Connext
([qnx-ports/build-files](https://github.com/qnx-ports/build-files/tree/main/ports/ros2)). DDS moves bytes over UDP
or shared memory. When a safety node at high priority calls a service hosted by a low-priority node, the request
waits until the service's thread runs at its own low priority, and any medium-priority work on the same CPU runs
first. That is priority inversion between nodes, and the RTOS underneath has the mechanism to prevent it: QNX message
passing runs the receiving thread at the sender's priority. This project puts ROS 2 on that mechanism.

## How it works

```
 client node (prio 20)                                    server node (prio 10)
   rclcpp -> rcl -> rmw_qnx                                 rclcpp -> rcl -> rmw_qnx
   rmw_send_request                                          rmw_wait: MsgReceive on the context channel
     | MsgSendPulse, priority of the caller                    ^  the receiving thread takes priority 20,
     v                                                         |  returns to the executor, the callback runs at 20
   sender thread, now at 20  ------------ MsgSend ------------+
                             <----------- MsgReply ----------- rmw_send_response

 publisher -> shared-memory ring per topic -> MsgSendPulse at each subscriber's own priority -> subscriber's wait set
 /dev/ros2 (ros2graphd, a resource manager) reads the rings and the service registry on every open
```

| ROS 2 concept | rmw_qnx | QNX mechanism |
|---|---|---|
| Service call | rcl's request call must not block, so `rmw_send_request` hands the request to the client's sender thread with a pulse sent at the caller's priority; that thread, now at the caller's priority, does `MsgSend` to the server's context channel; the server thread blocked in `MsgReceive` inside `rmw_wait` takes that priority, runs the callback, and `rmw_send_response` replies | `MsgSendPulse` with priority -1, `MsgSendv`, `MsgReceive`, `MsgReplyv`, priority inheritance on the channel |
| Wait set | `rmw_wait` blocks in `MsgReceive` on one channel; guard conditions and topics wake it with pulses at the waiting thread's own priority | `ChannelCreate`, `ConnectAttach`, `TimerTimeout` |
| Topic | A shared-memory ring per topic ([`topics.hpp`](rmw_qnx/src/topics.hpp) over [`core/ring.hpp`](rmw_qnx/core/ring.hpp)); the publisher serialises once, never blocks, and pulses each subscriber at the priority that subscriber registered with | `shm_open`, `mmap`, `MsgSendPulse`, `MsgReceivePulse` |
| Graph | `ros2graphd`: `/dev/ros2/topics` and `/dev/ros2/services`, rendered fresh on every open; a topic's latest message is decoded with its type's own introspection library | `resmgr_attach`, `iofunc` layer, `_IO_XTYPE_READDIR` |
| Serialisation | CDR from the C++ introspection type support, with a bounds-checked reader ([`core/cdr.hpp`](rmw_qnx/core/cdr.hpp)) | |

Plain logic (ring, CDR, name encoding, graph bookkeeping, QoS rules) lives in [`rmw_qnx/core/`](rmw_qnx/core),
compiles on any host, and is tested there.

## Evidence of QNX use

| QNX technology | File and function | What it does here |
|---|---|---|
| Message passing with priority inheritance | [`rmw_qnx.cpp`](rmw_qnx/src/rmw_qnx.cpp): `sender_main` (`MsgSendv`), `rmw_wait` (`MsgReceive`), `accept_request`, `rmw_send_response` (`MsgReplyv`) | The server's callback runs at the client's priority: 600 of 600 loaded calls |
| Pulses carrying a priority | `rmw_send_request` (`MsgSendPulse(coid, -1, ...)`), `Waitable::notify`, `Topic::publish` in [`topics.hpp`](rmw_qnx/src/topics.hpp), `relay_main` | A caller's priority reaches the thread that blocks for it; subscribers wake at their own priority |
| Resource manager | [`ros2graphd.cpp`](rmw_qnx/src/ros2graphd.cpp): `main` (`resmgr_attach` on `/dev/ros2`), `io_open`, `io_read`, `render_topic` | `ls` and `cat` on the live ROS 2 graph |
| Shared memory | `Topic::Topic` (`shm_open`, `mmap`) | One ring per topic, read by every subscriber and by `/dev/ros2` |
| Instrumented kernel and `tracelogger` | [`qnx/image/build.sh`](qnx/image/build.sh) (`procnto-smp-instr -P 22`), [`scripts/guest/run_inversion.sh`](scripts/guest/run_inversion.sh), [`scripts/trace_summary.py`](scripts/trace_summary.py) | The kernel's own record of who ran first |
| Run masks and priorities | `become`, `pin_this_thread` in [`inversion.cpp`](bench/inversion_ros2/src/inversion.cpp); `become` in [`pinv.c`](bench/pinv/pinv.c) | The measured threads share CPU 0, so priority alone decides who runs |
| `mkqnximage` | [`qnx/image/build.sh`](qnx/image/build.sh) | The guest image |

## What it will not do

- It does not carry messages between machines: message passing and shared memory are local to one QNX system.
- It carries data for types with the C++ introspection type support. Types offered only with the C type support (rcl's
  own `/rosout`) are created but carry nothing yet.
- Loaned messages, content filters, security enclaves, QoS events and graph queries return `RMW_RET_UNSUPPORTED`.
- It makes no timing claim beyond the counts below: the figures come from a virtual machine.

## Evidence

All guest figures are QEMU, WHPX, and each has its command in [`docs/RESULTS.md`](docs/RESULTS.md).

| Claim | Measured | Command | Recorded output | Code | Test |
|---|---|---|---|---|---|
| A medium load delays a high-priority ROS 2 client on DDS and not on rmw_qnx | Loaded misses: Fast DDS 40, 40, 40; Cyclone DDS 40, 40, 40; rmw_qnx 0, 0, 0 (of 200) | `bash scripts/killtest_all.sh` | [`inversion-ros2-20261107-063223.txt`](results/inversion-ros2-20261107-063223.txt) | [`rmw_qnx.cpp`](rmw_qnx/src/rmw_qnx.cpp) | the command itself |
| Across every recorded loaded run, rmw_qnx misses are not caused by the load | rmw_qnx 3 misses in 1,600 calls, load finished first in 0 of 640 mid-burst calls; Cyclone DDS 280 in 1,400; Fast DDS 120 in 600 | as above, repeated | [RESULTS §1](docs/RESULTS.md#1-the-kill-test-priority-inversion-through-ros-2) | | |
| The server's callback runs at the client's priority | 200 of 200 in every rmw_qnx run; 0 on DDS | same | same | `rmw_wait`, `sender_main` | same |
| The kernel's trace agrees | Load held CPU 0 for more than 1 ms before the server's thread: 21, 21, 0 waits | `bash scripts/killtest_all.sh`, then `scripts/trace_summary.py --threads` | [`trace-ros2-20261107-063419/`](results/trace-ros2-20261107-063419) | [`trace_summary.py`](scripts/trace_summary.py) | [`tests/tools`](tests/tools/test_trace_summary.py) (11) |
| The same property holds at the transport level | MsgSend 0 misses; UDP and shared memory 40 of 200 | `bash scripts/pinv_run.sh 200` | [`pinv-20261107-002853.txt`](results/pinv-20261107-002853.txt) | [`pinv.c`](bench/pinv/pinv.c) | |
| `pendulum_control` runs unchanged | Exit 0; 998 and 998 messages received on rmw_qnx | `bash scripts/pendulum_run.sh 2000` | [`pendulum-20261107-062433.txt`](results/pendulum-20261107-062433.txt) | | |
| Topics cross processes | 20 of 20 messages, in order | `bash scripts/topics_check.sh 20` | [`topics-20261107-060803.txt`](results/topics-20261107-060803.txt) | [`topics.hpp`](rmw_qnx/src/topics.hpp) | |
| `/dev/ros2` lists the graph and decodes the latest message | Recorded session | `bash scripts/dev_ros2_demo.sh` | [`dev_ros2_session.txt`](docs/dev_ros2_session.txt) | [`ros2graphd.cpp`](rmw_qnx/src/ros2graphd.cpp) | |
| A reader never takes a torn or repeated message; a lapped reader counts what it lost | Property tests over random interleavings and a racing writer | `scripts/host_tests.sh` | CI | [`core/ring.hpp`](rmw_qnx/core/ring.hpp) | [`test_ring.cpp`](tests/core/test_ring.cpp) |
| Truncated or forged CDR fails without reading past the buffer | Every truncation of 200 random messages | same | CI | [`core/cdr.hpp`](rmw_qnx/core/cdr.hpp) | [`test_cdr.cpp`](tests/core/test_cdr.cpp) |
| The core's tests catch deliberate faults | 9 of 9 mutants killed | `scripts/mutation_check.sh` | [RESULTS §3](docs/RESULTS.md#3-host-tests-of-the-core) | [`core/`](rmw_qnx/core) | |

Tests: 53 host tests of the core (GoogleTest, random order, including property tests; 95.8 % of its lines, 70.1 % of
its branches) and 11 tests of the trace tool (pytest). The guest runs above are the integration and end-to-end tests.

**Real and synthetic.** The kernel, ROS 2, the middlewares and the measurements are real; the load is a busy loop and
the server's work a calibrated loop of about 1 ms. Porting notes, each with its reproduction:
[`docs/PORTING_NOTES.md`](docs/PORTING_NOTES.md), including a QNX 8.0 x86_64 `memchr` behaviour that stopped every
ROS 2 process.

## Build and run

- **QNX version:** QNX SDP 8.0, free QNX Everywhere licence
- **Target:** QEMU x86_64 (`-accel whpx`, 2 vCPUs, 4 GB); aarch64 has not been tried
- **Toolchain:** `qcc`/`q++`, GCC 12.2.0 (`gcc_ntox86_64`, `gcc_ntox86_64_cxx`), CMake and colcon, on Ubuntu 22.04
  (WSL on Windows here)

```sh
# In Ubuntu, with the SDP at ~/qnx800:
bash qnx/stage1.sh            # guest image (mkqnximage) and the transport-level test
bash qnx/ros2/fetch.sh        # ROS 2 Jazzy sources and QNX's patches, from qnx-ports/build-files
bash qnx/ros2/build.sh        # cross-builds the ROS 2 subset, with this repository's patches (qnx/ros2/patches)
bash qnx/ros2/build.sh ~/rmw_qnx-work rmw_fastrtps_cpp pendulum_control
bash qnx/ros2/build_ours.sh   # rmw_qnx, ros2graphd and the scenarios; packs them for the guest
scripts/host_tests.sh         # host tests of the core (any Linux, no SDP)
```

```sh
# In Git Bash on Windows (QEMU with WHPX; on Linux use KVM with the same QEMU arguments):
bash scripts/killtest_all.sh              # the kill test, three middlewares, three runs each, traced
bash scripts/inversion_ros2.sh up         # boot the guest for the demos below
bash scripts/dev_ros2_demo.sh             # /dev/ros2 session
bash scripts/pendulum_run.sh 2000         # pendulum_control, unchanged
bash scripts/inversion_ros2.sh down
```

On the guest, any ROS 2 program uses rmw_qnx with `RMW_IMPLEMENTATION=rmw_qnx`, and `ros2graphd &` mounts
`/dev/ros2`.

## Upstream

This is a new repository with the RMW layer, the QNX build files, patches, instructions and docs. It is not a fork
and holds no upstream history.

- ROS 2 middleware interface it implements: <https://github.com/ros2/rmw> (Jazzy)
- QNX's ROS 2 port, whose patches and toolchain file the build fetches: <https://github.com/qnx-ports/build-files>
- Demos run unchanged: <https://github.com/ros2/demos>; real-time support (`rttest`): <https://github.com/ros2/realtime_support>
- Patches this repository applies to upstream sources at build time: [`qnx/ros2/patches/`](qnx/ros2/patches)

No public RMW implementation over QNX message passing was found in a search of GitHub, qnx-ports and openqnx.

## Layout

- `rmw_qnx/` the ROS 2 package: `src/` the middleware layer and `ros2graphd`, `core/` the host-testable logic
- `bench/pinv/` the transport-level test; `bench/inversion_ros2/` the ROS 2 scenario and the chatter check
- `qnx/` image build, boot, ssh, ROS 2 cross-build and patches
- `scripts/` test runs, guest-side scripts, trace decoding and summary
- `tests/core/` host tests (GoogleTest); `tests/tools/` tests of the trace summary (pytest)
- `results/` recorded runs; `docs/` results, porting notes, recorded sessions

Limitations: QoS deadline events and graph queries (`rmw_get_topic_names_and_types` and the rest) are not
implemented, and ROS 2's `test_rmw_implementation` and `test_communication` suites have not been run.

## License

[Apache-2.0](LICENSE), as ROS 2.
