#include <doctest/doctest.h>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <new>
#include <vector>

#include <ringu/memory_pool.hpp>
#include <ringu/ring_engine.hpp>

namespace {

/// borrow() that fails the test case when the pool is exhausted.
ringu::RegisteredBuffer borrow_or_fail(ringu::RegisteredMemoryPool& pool) {
  auto result = pool.borrow();
  REQUIRE(result.has_value());
  return std::move(result).value();
}

} // namespace

TEST_CASE("RegisteredMemoryPool::create returns a tl::expected carrying the pool or a RinguError") {
  auto ring_engine_result = ringu::RingEngine::create(ringu::RingEngineOptions{.entries = 64});
  REQUIRE(ring_engine_result.has_value());
  auto engine = std::move(ring_engine_result).value();
  auto pool_result = ringu::RegisteredMemoryPool::create(engine->ring(), 4096, 4);
  REQUIRE(pool_result.has_value());
  CHECK(pool_result.value()->capacity() == 4);
  CHECK(pool_result.value()->buffer_size() == 4096);
  CHECK(pool_result.value()->available() == 4);
}

TEST_CASE("RegisteredMemoryPool borrow/return tracks the active counter") {
  auto ring_engine_result = ringu::RingEngine::create(ringu::RingEngineOptions{.entries = 64});
  REQUIRE(ring_engine_result.has_value());
  auto engine = std::move(ring_engine_result).value();
  auto pool_result = ringu::RegisteredMemoryPool::create(engine->ring(), 4096, 8);
  REQUIRE(pool_result.has_value());
  auto& pool = *pool_result.value();
  CHECK(pool.capacity() == 8);
  CHECK(pool.buffer_size() == 4096);
  CHECK(pool.active_count() == 0);
  CHECK(pool.available() == 8);
  {
    ringu::RegisteredBuffer buf = borrow_or_fail(pool);
    CHECK(static_cast<bool>(buf));
    CHECK(buf.data() != nullptr);
    CHECK(buf.size() == 4096);
    CHECK(pool.active_count() == 1);
    std::memset(buf.data(), 0xAB, buf.size());
    // the backing memory is whole mmap'ed pages, every chunk is cache-line aligned
    const std::size_t cache_line = ringu::cache_line_size();
    const long page = ::sysconf(_SC_PAGESIZE);
    CHECK(reinterpret_cast<std::uintptr_t>(pool.base()) % static_cast<std::uintptr_t>(page) == 0);
    CHECK(reinterpret_cast<std::uintptr_t>(buf.data()) % static_cast<std::uintptr_t>(cache_line) == 0);
  }
  CHECK(pool.active_count() == 0);
  CHECK(pool.available() == 8);
}

TEST_CASE("chunks are cache-line aligned even when buffer_size is not a multiple of it") {
  auto ring_engine_result = ringu::RingEngine::create(ringu::RingEngineOptions{.entries = 64});
  REQUIRE(ring_engine_result.has_value());
  auto engine = std::move(ring_engine_result).value();
  auto pool_result = ringu::RegisteredMemoryPool::create(engine->ring(), 100, 4);
  REQUIRE(pool_result.has_value());
  auto& pool = *pool_result.value();
  const std::size_t cache_line = ringu::cache_line_size();
  std::vector<ringu::RegisteredBuffer> bufs;
  for (std::int32_t i = 0; i < 4; ++i) {
    bufs.push_back(borrow_or_fail(pool));
  }
  for (const auto& b : bufs) {
    CHECK(b.size() == 100);
    CHECK(reinterpret_cast<std::uintptr_t>(b.data()) % static_cast<std::uintptr_t>(cache_line) == 0);
  }
}

TEST_CASE("RegisteredMemoryPool exhaustion returns a RinguError and buffers are reusable") {
  auto ring_engine_result = ringu::RingEngine::create(ringu::RingEngineOptions{.entries = 64});
  REQUIRE(ring_engine_result.has_value());
  auto engine = std::move(ring_engine_result).value();
  auto pool_result = ringu::RegisteredMemoryPool::create(engine->ring(), 64, 4);
  REQUIRE(pool_result.has_value());
  auto& pool = *pool_result.value();
  std::vector<ringu::RegisteredBuffer> bufs;
  for (std::int32_t i = 0; i < 4; ++i) {
    bufs.push_back(borrow_or_fail(pool));
  }
  CHECK(pool.active_count() == 4);
  auto exhausted = pool.borrow();
  CHECK_FALSE(exhausted.has_value());
  CHECK(exhausted.error().opcode == ringu::ErrorOpcode::MemoryPoolExhausted);
  CHECK_FALSE(exhausted.error().message.empty());
  bufs.clear();
  CHECK(pool.active_count() == 0);
  // buffers can be borrowed again after being returned
  ringu::RegisteredBuffer buf = borrow_or_fail(pool);
  CHECK(pool.active_count() == 1);
}

TEST_CASE("RegisteredBuffer move semantics transfer ownership") {
  auto ring_engine_result = ringu::RingEngine::create(ringu::RingEngineOptions{.entries = 64});
  REQUIRE(ring_engine_result.has_value());
  auto engine = std::move(ring_engine_result).value();
  auto pool_result = ringu::RegisteredMemoryPool::create(engine->ring(), 128, 2);
  REQUIRE(pool_result.has_value());
  auto& pool = *pool_result.value();
  ringu::RegisteredBuffer a = borrow_or_fail(pool);
  const std::byte* data = a.data();
  ringu::RegisteredBuffer b = std::move(a);
  CHECK_FALSE(static_cast<bool>(a));
  CHECK(static_cast<bool>(b));
  CHECK(b.data() == data);
  CHECK(pool.active_count() == 1);
}

TEST_CASE("RegisteredBuffer is usable as an io_uring fixed buffer for read") {
  auto ring_engine_result = ringu::RingEngine::create(ringu::RingEngineOptions{.entries = 64});
  REQUIRE(ring_engine_result.has_value());
  auto engine = std::move(ring_engine_result).value();
  // prepare a file with known content
  const std::string path = "/tmp/ringu_pool_test_file";
  {
    const std::int32_t fd = static_cast<std::int32_t>(::open(path.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644));
    REQUIRE(fd >= 0);
    const char* content = "fixed-buffer-content";
    REQUIRE(::write(fd, content, 20) == 20);
    ::close(fd);
  }
  ringu::FileDescriptor fd{-1};
  engine->submit(ringu::open_at(path, O_RDONLY, 0),
                 [&](ringu::Result<ringu::FileDescriptor> r, ringu::OperationId, std::int32_t) {
                   REQUIRE(r.has_value());
                   fd = *r;
                 });
  engine->process(ringu::RingEngine::ProcessMode::Batch);
  REQUIRE(fd.value() >= 0);

  auto pool_result = ringu::RegisteredMemoryPool::create(engine->ring(), 4096, 4);
  REQUIRE(pool_result.has_value());
  auto& pool = *pool_result.value();
  std::vector<std::byte> out;
  engine->submit(ringu::pread(fd, 0), borrow_or_fail(pool),
                 [&](ringu::Result<ringu::RegisteredBuffer> r, ringu::OperationId, std::int32_t res) {
                   REQUIRE(r.has_value());
                   CHECK(res == 20);
                   const auto& buf = *r;
                   out.assign(buf.data(), buf.data() + res);
                 });
  engine->process(ringu::RingEngine::ProcessMode::Batch);
  CHECK(out.size() == 20);
  CHECK(std::memcmp(out.data(), "fixed-buffer-content", 20) == 0);
  CHECK(engine->count_operations(ringu::RingEngine::OperationState::Completed) == 2);

  engine->submit(ringu::close(fd), [](ringu::Result<std::int32_t>, ringu::OperationId, std::int32_t) {});
  engine->process(ringu::RingEngine::ProcessMode::Batch);
  ::unlink(path.c_str());
}

TEST_CASE("Pool invariant: destroying the pool with an active buffer terminates") {
  const std::int32_t pid = static_cast<std::int32_t>(::fork());
  REQUIRE(pid >= 0);
  if (pid == 0) {
    // child: destroy the pool while a buffer is still borrowed -> must terminate.
    // reset the signal handlers inherited from doctest so the child dies cleanly.
    ::signal(SIGABRT, SIG_DFL);
    auto ring_engine_result = ringu::RingEngine::create(ringu::RingEngineOptions{.entries = 16});
    REQUIRE(ring_engine_result.has_value());
    auto engine = std::move(ring_engine_result).value();
    auto pool_result = ringu::RegisteredMemoryPool::create(engine->ring(), 64, 2);
    REQUIRE(pool_result.has_value());
    auto pool = std::move(pool_result).value();
    ringu::RegisteredBuffer buf = borrow_or_fail(*pool);
    (void)buf;
    pool.reset(); // invariant violation: active_count == 1
    ::_exit(0);   // not reached
  }
  std::int32_t status = 0;
  REQUIRE(::waitpid(static_cast<pid_t>(pid), reinterpret_cast<int*>(&status), 0) == pid);
  CHECK(WIFSIGNALED(status));
}
