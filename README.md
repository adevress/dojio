# ringu

[![gcc](https://img.shields.io/github/check-runs/adevress/ringu/main?nameFilter=gcc%20build%20%2B%20tests%20%2B%20benches&label=gcc)](https://github.com/adevress/ringu/actions/workflows/ci.yml)
[![clang](https://img.shields.io/github/check-runs/adevress/ringu/main?nameFilter=clang%20build%20%2B%20tests%20%2B%20benches&label=clang)](https://github.com/adevress/ringu/actions/workflows/ci.yml)
[![format](https://img.shields.io/github/check-runs/adevress/ringu/main?nameFilter=nix-format%20check&label=format)](https://github.com/adevress/ringu/actions/workflows/ci.yml)

A high-performance io_uring interface for modern C++20.

## Summary

`ringu` is a header-and-static-library wrapper around the Linux `io_uring` kernel
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

- `libringu.a` — the static library.
- `ringu_unit_tests` — the doctest unit-test binary.
- `nanobench_ring_engine`, `nanobench_memory_pool` — benchmarks.
- `demo_udp_pingpong`, `demo_tcp_hello` — demo programs.

### CMake options

| Option                 | Default | Description                              |
| ---------------------- | ------- | ---------------------------------------- |
| `RINGU_COVERAGE`       | `OFF`   | Enable code coverage instrumentation.    |
| `RINGU_SANITIZER`      | `OFF`   | Enable AddressSanitizer/UBSan.           |
| `RINGU_LTO_OPT`        | `ON`    | Enable link-time optimization.           |
| `RINGU_STATIC_BUILD`   | `OFF`   | Produce fully static binaries.           |

Example: `cmake -G Ninja -DRINGU_SANITIZER=ON ../`

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
```

### Install

```sh
cd build-agent
cmake --install . --prefix /path/to/prefix
```

This installs the static library, the public headers, a CMake package config
(`ringuConfig.cmake`, namespace `ringu::`) and a pkg-config file (`ringu.pc`).

### Formatting

```sh
task format
```
