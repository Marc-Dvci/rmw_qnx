# Porting notes: ROS 2 Jazzy on QNX 8.0 x86_64

What it took, beyond QNX's own port in [qnx-ports/build-files](https://github.com/qnx-ports/build-files/tree/main/ports/ros2/ros2-jazzy),
to run ROS 2 processes in a QNX SDP 8.0 x86_64 guest (QEMU, WHPX). Each note gives what was seen, how to reproduce it,
and what this repository does about it. These are observations from this setup, recorded so that anyone porting to
QNX 8.0 can check them on theirs.

## 1. `memchr` with a length of `SIZE_MAX`

**Seen.** Every process that set `RMW_IMPLEMENTATION` crashed with a memory fault while its libraries were still
loading. The core's backtrace (SDP `gdb`):

```
#0  0x0000000000000000 in ?? ()
#1  rcutils_strndup () from librcutils.so
#2  rcl_rmw_implementation_identifier_check () from librcl.so
#3  initialize () from librcl.so
#4  init_array.part () from ldqnx-64.so.2
```

`rcutils_strdup` calls `rcutils_strndup(str, SIZE_MAX, ...)`, which finds the terminator with
`memchr(str, '\0', SIZE_MAX)`. In this guest, libc's x86_64 `memchr` returns the right answer when the start address
is 8-byte aligned and faults at every other alignment.

**Reproduce.** [`qnx/ros2/patches/memchr_size_max.c`](../qnx/ros2/patches/memchr_size_max.c) calls
`memchr(p, 0, SIZE_MAX)` on a string 7 bytes long, at start offsets 0 to 7, each in its own child process:

```
qcc -Vgcc_ntox86_64 -o memchr_size_max qnx/ros2/patches/memchr_size_max.c   # in WSL, SDP environment sourced
# copy to the guest and run it:
memchr(start aligned +0, 0, SIZE_MAX): ok
memchr(start aligned +1, 0, SIZE_MAX): crashed
memchr(start aligned +2, 0, SIZE_MAX): crashed
memchr(start aligned +3, 0, SIZE_MAX): crashed
memchr(start aligned +4, 0, SIZE_MAX): crashed
memchr(start aligned +5, 0, SIZE_MAX): crashed
memchr(start aligned +6, 0, SIZE_MAX): crashed
memchr(start aligned +7, 0, SIZE_MAX): crashed
```

The disassembly of libc.so.6's `memchr` (SDP `objdump`) starts with an alignment step that compares the remaining
length with a signed compare (`cmovle`) before jumping through a table, which is consistent with `SIZE_MAX` being
read as -1. That reading is an inference from the disassembly, not a confirmed cause.

**What this repository does.** [`qnx/ros2/patches/rcutils-strndup.patch`](../qnx/ros2/patches/rcutils-strndup.patch)
makes `rcutils_strndup` use `strnlen(str, max_length)`, which gives the same length. `qnx/ros2/build.sh` applies it.
Any code that passes a "no limit" length of `SIZE_MAX` to `memchr` is worth checking on QNX 8.0 x86_64.

## 2. Shared objects exporting copies of libc functions

**Seen.** Before note 1 could even show, the same crash happened with `memchr` resolved to an address inside
`libexample_interfaces__rosidl_typesupport_c.so`: thirteen ROS 2 libraries defined and exported their own `memchr`.

**Cause.** build-files' toolchain file links shared objects with `-Wl,--as-needed` ahead of the libraries `qcc` adds.
`libc.so` was then dropped from those libraries, and the pieces of libc they used were copied in from the static
`libcS.a` and exported. Relinking one of them with `-Wl,--no-as-needed` after the toolchain's flags gave 0 exported
`memchr` and a `NEEDED libc.so.6` entry; without it, 1 and no libc.

**Reproduce.** In the build folder of any interface package, re-run the link line from `link.txt` and check
`ntox86_64-nm -D --defined-only lib<...>.so | grep -w memchr`, with and without `-Wl,--no-as-needed` added.

**What this repository does.** `qnx/ros2/build.sh` and `qnx/ros2/build_ours.sh` pass
`-DEXTRA_CMAKE_LINKER_FLAGS=-Wl,--no-as-needed`.

## 3. Discovery under strict priorities on one CPU

**Seen.** With every thread of each ROS 2 process (including the middleware's) pinned to CPU 0 at SCHED_FIFO
priorities 10, 15 and 20, Cyclone DDS never discovered the priority-10 server; without the pinning it did.

**What this repository does.** The inversion scenario pins only the threads it measures
(`pin_this_thread` in [`bench/inversion_ros2/src/inversion.cpp`](../bench/inversion_ros2/src/inversion.cpp)).

## 4. Guest file count

The full ROS 2 install tree (about 10,000 files, a third of them headers) filled the guest's data partition's inode
table before its blocks. `qnx/ros2/build_ours.sh` packs only what runs: libraries, executables and the ament index
(1,527 files).
