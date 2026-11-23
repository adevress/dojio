<p align="center">
  <img src="misc/dojio-logo.png" alt="dojio logo" width="200">
</p>

<p align="center">A high-performance io_uring interface for modern C++20.</p>

[![gcc](https://img.shields.io/github/check-runs/adevress/dojio/main?nameFilter=gcc%20build%20%2B%20tests%20%2B%20benches&label=gcc)](https://github.com/adevress/dojio/actions/workflows/ci.yml)
[![clang](https://img.shields.io/github/check-runs/adevress/dojio/main?nameFilter=clang%20build%20%2B%20tests%20%2B%20benches&label=clang)](https://github.com/adevress/dojio/actions/workflows/ci.yml)
[![format](https://img.shields.io/github/check-runs/adevress/dojio/main?nameFilter=nix-format%20check&label=format)](https://github.com/adevress/dojio/actions/workflows/ci.yml)
[![asan](https://img.shields.io/github/check-runs/adevress/dojio/main?nameFilter=address%20sanitizer%20build%20%2B%20tests&label=asan)](https://github.com/adevress/dojio/actions/workflows/ci.yml)
[![ubsan](https://img.shields.io/github/check-runs/adevress/dojio/main?nameFilter=undefined%20sanitizer%20build%20%2B%20tests&label=ubsan)](https://github.com/adevress/dojio/actions/workflows/ci.yml)
[![msan](https://img.shields.io/github/check-runs/adevress/dojio/main?nameFilter=memory%20sanitizer%20build%20%2B%20tests&label=msan)](https://github.com/adevress/dojio/actions/workflows/ci.yml)

## Summary

`dojio` is a header-and-static-library wrapper around the Linux `io_uring` kernel
API, exposing it through a small, data-oriented C++20 interface. It is built
directly on the raw kernel API (no liburing), with a focus on zero-copy moves,
typed operation ids and a predictable completion model.

The library ships as a static library, with tests built on
[doctest](https://github.com/doctest/doctest) and benchmarks built on
[nanobench](https://github.com/martinus/nanobench).

## Goal

Provide a solid, self-contained backbone for high-performance I/O and networking
applications on Linux, by:

- exposing `io_uring` through modern and safe C++20 features
- being data-oriented and performance-focused
- Focus on a "never blocking" API appropriate for high performance server or Fiber based io framework
- Architecture "Zero sharing" (One RingEngine per thread, and message passing between thread)

## Compilation instructions

### Requirements

- Linux with a kernel supporting `io_uring` (5.1+; more recent kernels expose
  more of the supported operations).
- A C++20 compiler (the project also builds cleanly with clang and GCC).
- CMake >= 3.22, a generator such as Ninja, and a network connection for the
  first configure (dependencies are fetched via `FetchContent`).
- Dependencies are fetched automatically: `doctest`, `nanobench`, `tl::expected`
  and `fmt` (used in header-only mode).

### Portability

Incoming

### Build

```sh
mkdir -p build-agent
cd build-agent
cmake -G Ninja ../
ninja -v
```

The default build type is `RelWithDebInfo`. Build artifacts:

- `libdojio.a` — the static library.
- `dojio_unit_tests` — the doctest unit-test binary.
- `nanobench_ring_engine`, `nanobench_memory_pool` — benchmarks.
- `demo_udp_pingpong`, `demo_tcp_hello`, `demo_file_rw` — demo programs.

### CMake options

| Option                       | Default | Description                                                          |
| ---------------------------- | ------- | -------------------------------------------------------------------- |
| `DOJIO_COVERAGE`             | `OFF`   | Enable code coverage instrumentation.                                |
| `DOJIO_SANITIZER_ADDRESS`    | `OFF`   | Enable AddressSanitizer (address/memory errors and leaks).           |
| `DOJIO_SANITIZER_MEMORY`     | `OFF`   | Enable MemorySanitizer (clang-only, needs an instrumented libc++).   |
| `DOJIO_SANITIZER_UNDEFINED`  | `OFF`   | Enable UndefinedBehaviorSanitizer (aborts on undefined behavior).    |
| `DOJIO_LTO_OPT`              | `ON`    | Enable link-time optimization (ignored with a sanitizer).            |
| `DOJIO_STATIC_BUILD`         | `OFF`   | Produce fully static binaries.                                       |

Examples:

```sh
cmake -G Ninja -DDOJIO_SANITIZER_ADDRESS=ON ../
cmake -G Ninja -DDOJIO_SANITIZER_UNDEFINED=ON ../
cmake -G Ninja -DDOJIO_SANITIZER_MEMORY=ON ../
```

The sanitizer switches can be combined, except `DOJIO_SANITIZER_ADDRESS` and
`DOJIO_SANITIZER_MEMORY` whose runtimes are mutually exclusive. `MSan` only
produces reliable results when the C++ standard library (and every other
dependency) is instrumented with `-fsanitize=memory`; with an uninstrumented
`libc++`/`libstdc++` it reports false positives.

### Tests

```sh
cd build-agent
ctest -V
```

### Benchmarks and demos

```sh
cd build-agent
./nanobench_ring_engine
./nanobench_memory_pool

# UDP ping-pong: reports latency. usage: demo_udp_pingpong [iterations=10000] [payload_size=64] [sqpoll=1]
./demo_udp_pingpong

# TCP hello world: reports throughput.
# usage: demo_tcp_hello [threads=4] [messages_per_connection=20000] [base_port=24100] [sqpoll=1]
./demo_tcp_hello

# File write: blocking POSIX vs concurrent RingEngine, reports wall-clock time.
# usage: demo_file_rw [base_directory=/tmp]
./demo_file_rw
```

### Install

```sh
cd build-agent
cmake --install . --prefix /path/to/prefix
```

This installs the static library, the public headers, a CMake package config
(`dojioConfig.cmake`, namespace `dojio::`) and a pkg-config file (`dojio.pc`).

### Formatting

```sh
task format
```
