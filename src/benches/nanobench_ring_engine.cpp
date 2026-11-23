#include <nanobench.h>

#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include <dojio/memory_pool.hpp>
#include <dojio/ring_engine.hpp>

namespace {

constexpr std::int32_t k_batch = 64;
constexpr std::uint64_t k_chunk = 4096;

std::string make_bench_file() {
  const std::string path = (std::filesystem::temp_directory_path() / "dojio_bench_file").string();
  const std::int32_t fd = static_cast<std::int32_t>(::open(path.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644));
  std::vector<char> chunk(65536, 'x');
  for (std::int32_t i = 0; i < 16; ++i) {
    const ::ssize_t nw = ::write(fd, chunk.data(), chunk.size());
    (void)nw;
  }
  ::close(fd);
  return path;
}

} // namespace

int main() {
  const std::string path = make_bench_file();
  const std::int32_t file_fd = static_cast<std::int32_t>(::open(path.c_str(), O_RDONLY));

  auto ring_engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 1024});
  if (!ring_engine_result.has_value()) {
    throw std::runtime_error(ring_engine_result.error().message);
  }
  auto engine = std::move(ring_engine_result).value();
  auto pool_result = dojio::RegisteredMemoryPool::create(engine->ring(), k_chunk, 256);
  if (!pool_result.has_value()) {
    throw std::runtime_error(pool_result.error().message);
  }
  auto& pool = *pool_result;

  ankerl::nanobench::Bench bench;
  bench.title("dojio ring engine").unit("op").warmup(2);

  // dedicated engine for the pending-set scan benchmark: 1024 long-lived timers
  // are submitted once and stay pending while each timed iteration adds 64 reads
  auto idle_engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 2048});
  if (!idle_engine_result.has_value()) {
    throw std::runtime_error(idle_engine_result.error().message);
  }
  auto idle_engine = std::move(idle_engine_result).value();
  constexpr std::int32_t k_idle_timers = 1024;
  for (std::int32_t i = 0; i < k_idle_timers; ++i) {
    idle_engine->submit(dojio::Timer{std::chrono::seconds(60)},
                        [](dojio::Result<std::monostate>, dojio::OperationId, std::int32_t) {});
  }
  idle_engine->process(dojio::RingEngine::ProcessMode::NonBlocking);

  bench.run("preads 64x4KB + process(Batch)", [&] {
    for (std::int32_t i = 0; i < k_batch; ++i) {
      engine->submit(dojio::pread(dojio::FileDescriptor{file_fd}, static_cast<std::uint64_t>(i) * k_chunk),
                     std::vector<std::byte>(static_cast<std::size_t>(k_chunk)),
                     [](dojio::Result<std::vector<std::byte>>, dojio::OperationId, std::int32_t) {});
    }
    engine->process(dojio::RingEngine::ProcessMode::Batch);
    ankerl::nanobench::doNotOptimizeAway(engine->count_operations(dojio::RingEngine::OperationState::Completed));
  });

  bench.run("preads 64x4KB with registered buffers (READ_FIXED)", [&] {
    for (std::int32_t i = 0; i < k_batch; ++i) {
      engine->submit(dojio::pread(dojio::FileDescriptor{file_fd}, static_cast<std::uint64_t>(i) * k_chunk),
                     pool.borrow().value(),
                     [](dojio::Result<dojio::RegisteredBuffer>, dojio::OperationId, std::int32_t) {});
    }
    engine->process(dojio::RingEngine::ProcessMode::Batch);
    ankerl::nanobench::doNotOptimizeAway(engine->count_operations(dojio::RingEngine::OperationState::Completed));
  });

  bench.run("open+close churn 64x", [&] {
    std::vector<std::int32_t> fds;
    fds.reserve(k_batch);
    for (std::int32_t i = 0; i < k_batch; ++i) {
      engine->submit(dojio::open_at(path, O_RDONLY, 0),
                     [&](dojio::Result<dojio::FileDescriptor> r, dojio::OperationId, std::int32_t) {
                       if (r.has_value()) {
                         fds.push_back(r->value());
                       }
                     });
    }
    engine->process(dojio::RingEngine::ProcessMode::Batch);
    for (const std::int32_t fd : fds) {
      engine->submit(dojio::close(dojio::FileDescriptor{fd}),
                     [](dojio::Result<std::int32_t>, dojio::OperationId, std::int32_t) {});
    }
    engine->process(dojio::RingEngine::ProcessMode::Batch);
    ankerl::nanobench::doNotOptimizeAway(fds.size());
  });

  bench.run("1024 idle timers + 64 preads", [&] {
    for (std::int32_t i = 0; i < k_batch; ++i) {
      idle_engine->submit(dojio::pread(dojio::FileDescriptor{file_fd}, static_cast<std::uint64_t>(i) * k_chunk),
                          std::vector<std::byte>(static_cast<std::size_t>(k_chunk)),
                          [](dojio::Result<std::vector<std::byte>>, dojio::OperationId, std::int32_t) {});
    }
    while (idle_engine->count_operations(dojio::RingEngine::OperationState::Pending) >
           static_cast<std::uint64_t>(k_idle_timers)) {
      idle_engine->process(dojio::RingEngine::ProcessMode::WaitOnce);
    }
    ankerl::nanobench::doNotOptimizeAway(idle_engine->count_operations(dojio::RingEngine::OperationState::Completed));
  });

  bench.run("timer submit + cancel + process", [&] {
    const dojio::OperationId id = engine->submit(
        dojio::Timer{std::chrono::seconds(60)}, [](dojio::Result<std::monostate>, dojio::OperationId, std::int32_t) {});
    engine->cancel(id);
    engine->process(dojio::RingEngine::ProcessMode::Batch);
    ankerl::nanobench::doNotOptimizeAway(engine->count_operations(dojio::RingEngine::OperationState::Failed));
  });

  bench.run("count_operations", [&] {
    ankerl::nanobench::doNotOptimizeAway(engine->count_operations(dojio::RingEngine::OperationState::Pending));
    ankerl::nanobench::doNotOptimizeAway(engine->count_operations(dojio::RingEngine::OperationState::Completed));
    ankerl::nanobench::doNotOptimizeAway(engine->count_operations(dojio::RingEngine::OperationState::Failed));
  });

  ::close(file_fd);
  ::unlink(path.c_str());
  return 0;
}
