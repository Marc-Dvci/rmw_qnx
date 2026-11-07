# rmw_qnx

[![Tests](https://github.com/Marc-Dvci/rmw_qnx/actions/workflows/tests.yml/badge.svg)](https://github.com/Marc-Dvci/rmw_qnx/actions/workflows/tests.yml)
[![License: Apache-2.0](https://img.shields.io/badge/license-Apache--2.0-blue)](LICENSE)

**A ROS 2 middleware for QNX 8.0 that sends a node's messages through the kernel's own message passing, so the
node's priority travels with them.**

| Track | Built with | Headline measurement (QEMU, WHPX) |
|---|---|---|
| Open-Source Contribution & Ports | QNX SDP 8.0, ROS 2 Jazzy, `MsgSend`/`MsgReply`, pulses, `tracelogger` | Under a medium-priority load, a high-priority client's requests missed their 5 ms deadline 0 times in 200 over QNX message passing, and 40 times in 200 over UDP and over shared memory ([results](docs/RESULTS.md)) |

[How it works](#how-it-works) · [Evidence](#evidence) · [Build and run](#build-and-run) · [Status](#status)

> [!IMPORTANT]
> Status: the transport-level property is measured in a QNX guest. The RMW layer carries ROS 2 services over
> message passing; topics, the `/dev/ros2` graph and QoS events are not implemented yet, and no ROS 2 test suite has
> been run on it. [Status](#status) lists exactly what works.

## For judges

| Criterion | Where it is answered |
|---|---|
| Technological implementation | [`rmw_qnx/src/rmw_qnx.cpp`](rmw_qnx/src/rmw_qnx.cpp) (services over `MsgSend`/`MsgReply`, wait sets on QNX channels), [`rmw_qnx/core/`](rmw_qnx/core) (ring, CDR, names, host-tested), [`bench/pinv/pinv.c`](bench/pinv/pinv.c) |
| Design | `RMW_IMPLEMENTATION=rmw_qnx` is the only change an application needs ([Build and run](#build-and-run)) |
| Potential impact | [The problem](#the-problem) |
| Innovation | [How it works](#how-it-works): priority inheritance carried through ROS 2's asynchronous service API |
| QNX-specific technologies | [How QNX is used](#how-qnx-is-used) |

## The problem

ROS 2 on QNX today talks through DDS: QNX's own ROS 2 Jazzy port builds Fast DDS, Cyclone DDS and Connext
([qnx-ports/build-files](https://github.com/qnx-ports/build-files/tree/main/ports/ros2)). DDS moves bytes over UDP
or shared memory, so when a safety node at high priority asks a service hosted by a low-priority node, the
request waits until the service's threads are scheduled at their own low priority. Any medium-priority work on the
same CPU then delays the safety node: priority inversion between nodes, which the RTOS underneath could have
prevented. QNX's message passing runs the receiving thread at the sender's priority. This project puts ROS 2 on that
mechanism.

## How it works

| ROS 2 concept | rmw_qnx | QNX mechanism |
|---|---|---|
| Service request and response | The client's call goes to a per-client sender thread by a pulse that carries the caller's priority; that thread does `MsgSend` to the server's context channel; the server thread blocked in `MsgReceive` inside `rmw_wait` takes the caller's priority, runs the callback, and `rmw_send_response` does `MsgReply` | `MsgSendPulse` with priority -1, `MsgSendv`, `MsgReceive`, `MsgReplyv`, priority inheritance on the channel |
| Wait set | `rmw_wait` blocks in `MsgReceive` on one channel that receives requests and wake-up pulses; guard conditions pulse every channel waiting on them | `ChannelCreate`, `ConnectAttach`, `TimerTimeout` |
| Topic (planned) | Shared-memory ring per topic, subscribers woken by pulses at their own priority | `shm_open`, `MsgSendPulse` |
| Graph (planned) | A resource manager: `ls /dev/ros2/topics`, `cat` a topic for its latest message | `resmgr_attach`, `iofunc` |
| QoS deadline (planned) | Timer pulses to the context channel | `timer_create` with `SIGEV_PULSE` |

The service path is in [`rmw_qnx/src/rmw_qnx.cpp`](rmw_qnx/src/rmw_qnx.cpp): `rmw_send_request`, `sender_main`,
`rmw_wait`, `accept_request`, `rmw_take_request`, `rmw_send_response`. Serialisation is CDR from the C++
introspection type support ([`rmw_qnx/src/serdes.hpp`](rmw_qnx/src/serdes.hpp) over
[`rmw_qnx/core/cdr.hpp`](rmw_qnx/core/cdr.hpp)).

## How QNX is used

| QNX technology | Where | Why it is needed |
|---|---|---|
| Message passing with priority inheritance | `rmw_wait`, `sender_main`, `rmw_send_response` in [`rmw_qnx.cpp`](rmw_qnx/src/rmw_qnx.cpp); `serve_msg` in [`pinv.c`](bench/pinv/pinv.c) | The server thread runs at the caller's priority: the measured 0 misses against 40 |
| Pulses carrying a priority | `rmw_send_request` (`MsgSendPulse(coid, -1, ...)`) | rcl's request call must not block, so the priority is handed to the thread that blocks |
| Run masks | `become` in [`pinv.c`](bench/pinv/pinv.c) and [`inversion.cpp`](bench/inversion_ros2/src/inversion.cpp) | All competitors on one CPU, so priority alone decides who runs |
| `tracelogger` and the instrumented kernel | [`scripts/pinv_trace.sh`](scripts/pinv_trace.sh), [`qnx/image/build.sh`](qnx/image/build.sh) (`procnto-smp-instr`) | The kernel's own record of who ran first |
| `mkqnximage` | [`qnx/image/build.sh`](qnx/image/build.sh) | The guest image, with `procnto -P 22` |

## Evidence

All figures are QEMU, WHPX, and each has its command in [`docs/RESULTS.md`](docs/RESULTS.md).

| Claim | Measured | Command | Recorded output |
|---|---|---|---|
| Over `MsgSend`/`MsgReply` the server runs at the client's priority | 200 of 200 requests, with and without load | `bash scripts/pinv_run.sh 200` | [`results/pinv-20261107-002853.txt`](results/pinv-20261107-002853.txt) |
| Over UDP and shared memory a medium load makes the high-priority client miss | 40 of 200 misses each; 0 over message passing | same | same |
| The kernel ran the load before the server in no request wait over message passing, and in 21 over UDP and over shared memory | `tracelogger`, 100 requests each | `bash scripts/pinv_trace.sh` | [`results/trace-20261107-003244/`](results/trace-20261107-003244) |
| With no load, no transport misses | 0 of 200 each | `bash scripts/pinv_run.sh 200` | [`results/pinv-20261107-002853.txt`](results/pinv-20261107-002853.txt) |
| The core (ring, CDR reader, name encoding, graph bookkeeping, QoS rules) behaves as specified | 53 host tests, including property tests; 95.8 % of the core's lines, 70.1 % of its branches | `COVERAGE=1 scripts/host_tests.sh` | [docs/RESULTS.md](docs/RESULTS.md#4-host-tests-of-the-core) |

Real and synthetic: the programs and the kernel are real; the load is a synthetic busy loop and the work is a
calibrated loop of about 1 ms.

## Build and run

- **QNX version:** QNX SDP 8.0 (free QNX Everywhere licence)
- **Target:** QEMU x86_64 (`-accel whpx`, 2 vCPUs, 4 GB); the same sources build for aarch64
- **Toolchain:** `qcc`/`q++` with GCC 12.2.0 (`gcc_ntox86_64`, `gcc_ntox86_64_cxx`), CMake and colcon on an Ubuntu
  22.04 host (WSL on Windows here)

```sh
# In Ubuntu (WSL), with the SDP at ~/qnx800:
bash qnx/stage1.sh                  # builds pinv and the guest image (qnx/image/build.sh, mkqnximage)
bash qnx/ros2/fetch.sh              # ROS 2 Jazzy sources and QNX's patches, from qnx-ports/build-files
bash qnx/ros2/build.sh              # cross-builds the ROS 2 subset for x86_64 into ~/rmw_qnx-work
bash qnx/ros2/build_ours.sh         # builds rmw_qnx and the inversion scenario, packs them for the guest
scripts/host_tests.sh               # the core's host tests (any Linux, no SDP needed)
```

```sh
# In Git Bash on Windows (QEMU with WHPX; on Linux, use KVM and the same QEMU arguments):
bash scripts/pinv_run.sh 200        # transport-level inversion test
bash scripts/pinv_trace.sh          # the same with tracelogger
bash scripts/inversion_ros2.sh 200  # through ROS 2: Fast DDS, Cyclone DDS, rmw_qnx
```

An application selects the middleware with `RMW_IMPLEMENTATION=rmw_qnx` and nothing else.

## Status

| Part | State |
|---|---|
| Transport-level measurement | Done, recorded |
| Guest image and boot scripts | Done |
| ROS 2 Jazzy subset cross-built for QNX x86_64 | rclcpp, rcl, Cyclone DDS and the interfaces build with QNX's patches ([`qnx/ros2/build.sh`](qnx/ros2/build.sh)); Fast DDS not built yet |
| rmw_qnx services, wait sets, guard conditions | `librmw_qnx.so` cross-builds against that subset; not yet run in the guest |
| rmw_qnx topics, graph (`/dev/ros2`), QoS events, graph queries | Not implemented: their entry points return `RMW_RET_UNSUPPORTED` |
| `test_rmw_implementation`, `test_communication`, `pendulum_control` | Not run |
| Latency against Fast DDS and Cyclone DDS | Not measured |

## Upstream

- ROS 2 middleware interface: <https://github.com/ros2/rmw> (Jazzy). rmw_qnx implements it; no upstream code is
  copied into this repository.
- QNX's ROS 2 port, whose patches and toolchain file the build uses: <https://github.com/qnx-ports/build-files>
  (fetched at build time, not included).
- Demos to run unchanged: <https://github.com/ros2/demos>.

No public RMW implementation over QNX message passing was found in a search of GitHub, qnx-ports and openqnx.

## Layout

- `rmw_qnx/` the ROS 2 package: `src/` the RMW layer, `core/` the host-testable logic
- `bench/pinv/` the transport-level test; `bench/inversion_ros2/` the same scenario through ROS 2
- `qnx/` image build, boot, ssh and ROS 2 cross-build scripts
- `scripts/` test runs, trace decoding and summary
- `tests/core/` host tests of the core (GoogleTest); `tests/tools/` tests of the trace summary
- `results/` recorded runs; `docs/RESULTS.md` every figure with its command

## License

[Apache-2.0](LICENSE), as ROS 2.
