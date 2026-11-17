#include <nanobench.h>

#include <fcntl.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include <ringu/memory_pool.hpp>
#include <ringu/ring_engine.hpp>

namespace {

constexpr std::int32_t k_batch = 64;
constexpr std::uint64_t k_chunk = 4096;

std::string make_bench_file() {
  const std::string path = (std::filesystem::temp_directory_path() / "ringu_bench_file").string();
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

  auto ring_engine_result = ringu::RingEngine::create(ringu::RingEngineOptions{.entries = 1024});
  if (!ring_engine_result.has_value()) {
    throw std::runtime_error(ring_engine_result.error().message);
  }
  auto engine = std::move(ring_engine_result).value();
  auto pool_result = ringu::RegisteredMemoryPool::create(engine->ring(), k_chunk, 256);
  if (!pool_result.has_value()) {
    throw std::runtime_error(pool_result.error().message);
  }
  auto& pool = *pool_result.value();

  ankerl::nanobench::Bench bench;
  bench.title("ringu ring engine").unit("op").warmup(2);

  bench.run("preads 64x4KB + process(Batch)", [&] {
    for (std::int32_t i = 0; i < k_batch; ++i) {
      engine->submit(ringu::pread(ringu::FileDescriptor{file_fd}, static_cast<std::uint64_t>(i) * k_chunk),
                     std::vector<std::byte>(static_cast<std::size_t>(k_chunk)),
                     [](ringu::Result<std::vector<std::byte>>, ringu::OperationId, std::int32_t) {});
    }
    engine->process(ringu::RingEngine::ProcessMode::Batch);
    ankerl::nanobench::doNotOptimizeAway(engine->count_operations(ringu::RingEngine::OperationState::Completed));
  });

  bench.run("preads 64x4KB with registered buffers (READ_FIXED)", [&] {
    for (std::int32_t i = 0; i < k_batch; ++i) {
      engine->submit(ringu::pread(ringu::FileDescriptor{file_fd}, static_cast<std::uint64_t>(i) * k_chunk),
                     pool.borrow().value(),
                     [](ringu::Result<ringu::RegisteredBuffer>, ringu::OperationId, std::int32_t) {});
    }
    engine->process(ringu::RingEngine::ProcessMode::Batch);
    ankerl::nanobench::doNotOptimizeAway(engine->count_operations(ringu::RingEngine::OperationState::Completed));
  });

  bench.run("open+close churn 64x", [&] {
    std::vector<std::int32_t> fds;
    fds.reserve(k_batch);
    for (std::int32_t i = 0; i < k_batch; ++i) {
      engine->submit(ringu::open_at(path, O_RDONLY, 0),
                     [&](ringu::Result<ringu::FileDescriptor> r, ringu::OperationId, std::int32_t) {
                       if (r.has_value()) {
                         fds.push_back(r->value());
                       }
                     });
    }
    engine->process(ringu::RingEngine::ProcessMode::Batch);
    for (const std::int32_t fd : fds) {
      engine->submit(ringu::close(ringu::FileDescriptor{fd}),
                     [](ringu::Result<std::int32_t>, ringu::OperationId, std::int32_t) {});
    }
    engine->process(ringu::RingEngine::ProcessMode::Batch);
    ankerl::nanobench::doNotOptimizeAway(fds.size());
  });

  bench.run("timer submit + cancel + process", [&] {
    const ringu::OperationId id = engine->submit(
        ringu::Timer{std::chrono::seconds(60)}, [](ringu::Result<std::monostate>, ringu::OperationId, std::int32_t) {});
    engine->cancel(id);
    engine->process(ringu::RingEngine::ProcessMode::Batch);
    ankerl::nanobench::doNotOptimizeAway(engine->count_operations(ringu::RingEngine::OperationState::Failed));
  });

  bench.run("count_operations", [&] {
    ankerl::nanobench::doNotOptimizeAway(engine->count_operations(ringu::RingEngine::OperationState::Pending));
    ankerl::nanobench::doNotOptimizeAway(engine->count_operations(ringu::RingEngine::OperationState::Completed));
    ankerl::nanobench::doNotOptimizeAway(engine->count_operations(ringu::RingEngine::OperationState::Failed));
  });

  ::close(file_fd);
  ::unlink(path.c_str());
  return 0;
}
