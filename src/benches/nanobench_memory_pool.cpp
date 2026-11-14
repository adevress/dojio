#include <nanobench.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include <ringu/memory_pool.hpp>
#include <ringu/reactor.hpp>

int main() {
  ringu::Reactor reactor{1024};

  ankerl::nanobench::Bench bench;
  bench.title("ringu memory pool").warmup(2);

  bench.run("pool construction (256 x 4KB registered buffers)", [&] {
    ringu::RegisteredMemoryPool pool{reactor.ring(), 4096, 256};
    ankerl::nanobench::doNotOptimizeAway(pool.capacity());
  });

  bench.run("borrow 256 buffers", [&] {
    ringu::RegisteredMemoryPool pool{reactor.ring(), 4096, 256};
    std::vector<ringu::RegisteredBuffer> bufs;
    bufs.reserve(256);
    for (std::int32_t i = 0; i < 256; ++i) {
      bufs.push_back(pool.borrow());
    }
    ankerl::nanobench::doNotOptimizeAway(pool.active_count());
  });

  bench.run("borrow + return 1 buffer x1000", [&] {
    ringu::RegisteredMemoryPool pool{reactor.ring(), 4096, 8};
    for (std::int32_t i = 0; i < 1000; ++i) {
      ringu::RegisteredBuffer buf = pool.borrow();
      ankerl::nanobench::doNotOptimizeAway(buf.data());
    }
    ankerl::nanobench::doNotOptimizeAway(pool.active_count());
  });

  bench.run("borrow + return 8 buffers x1000", [&] {
    ringu::RegisteredMemoryPool pool{reactor.ring(), 4096, 8};
    for (std::int32_t i = 0; i < 1000; ++i) {
      std::vector<ringu::RegisteredBuffer> bufs;
      bufs.reserve(8);
      for (std::int32_t j = 0; j < 8; ++j) {
        bufs.push_back(pool.borrow());
      }
      ankerl::nanobench::doNotOptimizeAway(pool.active_count());
    }
  });

  return 0;
}
