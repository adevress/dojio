#include <nanobench.h>

#include <fcntl.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <ringu/memory_pool.hpp>
#include <ringu/reactor.hpp>

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

  ringu::Reactor reactor{1024};
  ringu::RegisteredMemoryPool pool{reactor.ring_fd(), k_chunk, 256};

  ankerl::nanobench::Bench bench;
  bench.title("ringu reactor").unit("op").warmup(2);

  bench.run("preads 64x4KB + process(Batch)", [&] {
    for (std::int32_t i = 0; i < k_batch; ++i) {
      reactor.submit(ringu::Pread{ringu::FileDescriptor{file_fd}, static_cast<std::uint64_t>(i) * k_chunk},
                     std::vector<std::byte>(static_cast<std::size_t>(k_chunk)),
                     [](ringu::Result<std::vector<std::byte>>, ringu::OperationId, std::int32_t) {});
    }
    reactor.process(ringu::Reactor::ProcessMode::Batch);
    ankerl::nanobench::doNotOptimizeAway(reactor.count_operations(ringu::Reactor::OperationState::Completed));
  });

  bench.run("preads 64x4KB with registered buffers (READ_FIXED)", [&] {
    for (std::int32_t i = 0; i < k_batch; ++i) {
      reactor.submit(ringu::Pread{ringu::FileDescriptor{file_fd}, static_cast<std::uint64_t>(i) * k_chunk},
                     pool.borrow(), [](ringu::Result<ringu::RegisteredBuffer>, ringu::OperationId, std::int32_t) {});
    }
    reactor.process(ringu::Reactor::ProcessMode::Batch);
    ankerl::nanobench::doNotOptimizeAway(reactor.count_operations(ringu::Reactor::OperationState::Completed));
  });

  bench.run("open+close churn 64x", [&] {
    std::vector<std::int32_t> fds;
    fds.reserve(k_batch);
    for (std::int32_t i = 0; i < k_batch; ++i) {
      reactor.submit(ringu::Open::open(path, O_RDONLY, 0),
                     [&](ringu::Result<ringu::FileDescriptor> r, ringu::OperationId, std::int32_t) {
                       if (r.has_value()) {
                         fds.push_back(r->value());
                       }
                     });
    }
    reactor.process(ringu::Reactor::ProcessMode::Batch);
    for (const std::int32_t fd : fds) {
      reactor.submit(ringu::Close::close(ringu::FileDescriptor{fd}),
                     [](ringu::Result<std::int32_t>, ringu::OperationId, std::int32_t) {});
    }
    reactor.process(ringu::Reactor::ProcessMode::Batch);
    ankerl::nanobench::doNotOptimizeAway(fds.size());
  });

  bench.run("timer submit + cancel + process", [&] {
    const ringu::OperationId id = reactor.submit(
        ringu::Timer{std::chrono::seconds(60)}, [](ringu::Result<std::monostate>, ringu::OperationId, std::int32_t) {});
    reactor.cancel(id);
    reactor.process(ringu::Reactor::ProcessMode::Batch);
    ankerl::nanobench::doNotOptimizeAway(reactor.count_operations(ringu::Reactor::OperationState::Failed));
  });

  bench.run("count_operations", [&] {
    ankerl::nanobench::doNotOptimizeAway(reactor.count_operations(ringu::Reactor::OperationState::Pending));
    ankerl::nanobench::doNotOptimizeAway(reactor.count_operations(ringu::Reactor::OperationState::Completed));
    ankerl::nanobench::doNotOptimizeAway(reactor.count_operations(ringu::Reactor::OperationState::Failed));
  });

  ::close(file_fd);
  ::unlink(path.c_str());
  return 0;
}
