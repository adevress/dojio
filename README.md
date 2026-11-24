<p align="center">
  <img src="misc/dojio-logo.png" alt="dojio logo" width="200">
</p>

<p align="center">Dojio - Bring value semantics to your async I/O in Modern C++</p>

[![gcc](https://img.shields.io/github/check-runs/adevress/dojio/main?nameFilter=gcc%20build%20%2B%20tests%20%2B%20benches&label=gcc)](https://github.com/adevress/dojio/actions/workflows/ci.yml)
[![clang](https://img.shields.io/github/check-runs/adevress/dojio/main?nameFilter=clang%20build%20%2B%20tests%20%2B%20benches&label=clang)](https://github.com/adevress/dojio/actions/workflows/ci.yml)
[![format](https://img.shields.io/github/check-runs/adevress/dojio/main?nameFilter=nix-format%20check&label=format)](https://github.com/adevress/dojio/actions/workflows/ci.yml)
[![asan](https://img.shields.io/github/check-runs/adevress/dojio/main?nameFilter=address%20sanitizer%20build%20%2B%20tests&label=asan)](https://github.com/adevress/dojio/actions/workflows/ci.yml)
[![ubsan](https://img.shields.io/github/check-runs/adevress/dojio/main?nameFilter=undefined%20sanitizer%20build%20%2B%20tests&label=ubsan)](https://github.com/adevress/dojio/actions/workflows/ci.yml)

## Summary

`Dojio` is a C++ library for Asynchronous I/O. Dojio has been built from the start around Linux `io_uring` capabilities and Modern C++ capabilities.

Dojio differs from other Async I/O toolkit by being *value based* and centered around a *share-nothing* per-thread architecture.
Dojio also specifically aims to integrate with the RDMA capabilities of modern NIC.

Dojio uses cases:
- High performance I/O in HPC applications.
- High throughput servers

## Why Dojio ?

For decades, the defacto standard for asynchronous I/O in C++ has been Boost.ASIO.

If Boost.Asio is still a widely used solution today, it comes with its fair share of issues:
- The API is large and complex and with a fair amount of foot guns in term of lifetime management.
- It has been designed in 2003, pre-C++11 and the introduction of move semantics in C++.
- The reference semantics + multi-thread architecture contains many death trap for performance.
- It has been designed first and foremost for the Windows pro-actor I/O interface.
- It is not *real* async I/O for filesystem operations.
- The Linux Kernel API for I/O has dramatically improved since 2003.

**Dojio** starts on something new:
- Value semantics makes lifetime one order of magnitude easier to manage
- It is smaller and lighter and a fraction of the API complexity.
- it focus on `io_uring` for truly async I/O even on file operations


## Feature support

- Asynchronous File I/O (open/read/write/fsync/close)
- Asynchronous Socket operations (socket,bind,listen,connect,send,recv)
- Interprocess (Unix domain socket, pipe)
- Channels (Inter-queue, Inter-thread communications)
- Zero Copy
- Low latency with Kernel polling
- Deterministic Latency (P99 < 6us )

## Details
 
The library ships as a static library, with tests built on
[doctest](https://github.com/doctest/doctest) and benchmarks built on
[nanobench](https://github.com/martinus/nanobench).

### Operation model


#### Async mode

Every I/O operation is a struct that carries its own buffer, so `submit()` takes only the
operation and its completion callback:

```cpp
const dojio::OperationId id =
    engine.submit(dojio::pread(fd, offset, std::vector<std::byte>(4096)),
                  [](dojio::RingEngine& ring, dojio::Result<dojio::PreadResult<std::vector<std::byte>>> r) {
                    if (!r.has_value()) {
                      // r.error() is the mapped I/O code or the cancellation code
                      return;
                    }
                    auto& buffer = r.value().buffer;
                    fmt::print("data : ", std::string_view(buffer.data(), buffer.size())
                  });
```


### Processing options

`RingEngine::process()` selects how the queued operations are driven:
i
| Mode | Behavior |
| ---- | -------- |
| `Batch` | Process every queued operation (ring and inline, including the ones chained from callbacks) until none is left. |
| `Forever` | Like `Batch`, but returns early when `request_stop()` is called. |
| `NonBlocking` | Submit the queued operations and dispatch only the completions that are already ready; never blocks. |
| `WaitOnce` | Submit, then wait for at least one completion when operations are still pending, dispatch the ready ones and return. |


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
