#include <nanobench.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include <dojio/memory_pool.hpp>
#include <dojio/ring_engine.hpp>

int main() {
  auto ring_engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 1024});
  if (!ring_engine_result.has_value()) {
    throw std::runtime_error(ring_engine_result.error().message);
  }
  auto engine = std::move(ring_engine_result).value();

  ankerl::nanobench::Bench bench;
  bench.title("dojio memory pool").warmup(2);

  bench.run("pool construction (256 x 4KB registered buffers)", [&] {
    auto pool_result = dojio::RegisteredMemoryPool::create(engine->ring(), 4096, 256);
    if (!pool_result.has_value()) {
      throw std::runtime_error(pool_result.error().message);
    }
    auto& pool = *pool_result;
    ankerl::nanobench::doNotOptimizeAway(pool.capacity());
  });

  bench.run("borrow 256 buffers", [&] {
    auto pool_result = dojio::RegisteredMemoryPool::create(engine->ring(), 4096, 256);
    if (!pool_result.has_value()) {
      throw std::runtime_error(pool_result.error().message);
    }
    auto& pool = *pool_result;
    std::vector<dojio::RegisteredBuffer> bufs;
    bufs.reserve(256);
    for (std::int32_t i = 0; i < 256; ++i) {
      bufs.push_back(pool.borrow().value());
    }
    ankerl::nanobench::doNotOptimizeAway(pool.active_count());
  });

  bench.run("borrow + return 1 buffer x1000", [&] {
    auto pool_result = dojio::RegisteredMemoryPool::create(engine->ring(), 4096, 8);
    if (!pool_result.has_value()) {
      throw std::runtime_error(pool_result.error().message);
    }
    auto& pool = *pool_result;
    for (std::int32_t i = 0; i < 1000; ++i) {
      dojio::RegisteredBuffer buf = pool.borrow().value();
      ankerl::nanobench::doNotOptimizeAway(buf.data());
    }
    ankerl::nanobench::doNotOptimizeAway(pool.active_count());
  });

  bench.run("borrow + return 1 buffer, 4095/4096 held x1000", [&] {
    auto pool_result = dojio::RegisteredMemoryPool::create(engine->ring(), 4096, 4096);
    if (!pool_result.has_value()) {
      throw std::runtime_error(pool_result.error().message);
    }
    auto& pool = *pool_result;
    std::vector<dojio::RegisteredBuffer> held;
    held.reserve(4095);
    for (std::int32_t i = 0; i < 4095; ++i) {
      held.push_back(pool.borrow().value());
    }
    for (std::int32_t i = 0; i < 1000; ++i) {
      dojio::RegisteredBuffer buf = pool.borrow().value();
      ankerl::nanobench::doNotOptimizeAway(buf.data());
    }
    ankerl::nanobench::doNotOptimizeAway(pool.active_count());
  });

  bench.run("borrow + return 8 buffers x1000", [&] {
    auto pool_result = dojio::RegisteredMemoryPool::create(engine->ring(), 4096, 8);
    if (!pool_result.has_value()) {
      throw std::runtime_error(pool_result.error().message);
    }
    auto& pool = *pool_result;
    for (std::int32_t i = 0; i < 1000; ++i) {
      std::vector<dojio::RegisteredBuffer> bufs;
      bufs.reserve(8);
      for (std::int32_t j = 0; j < 8; ++j) {
        bufs.push_back(pool.borrow().value());
      }
      ankerl::nanobench::doNotOptimizeAway(pool.active_count());
    }
  });

  return 0;
}
