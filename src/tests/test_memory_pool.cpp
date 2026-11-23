#include <doctest/doctest.h>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <dojio/memory_pool.hpp>
#include <dojio/ring_engine.hpp>

namespace {

/// borrow() that fails the test case when the pool is exhausted.
dojio::RegisteredBuffer borrow_or_fail(dojio::RegisteredMemoryPool& pool) {
  auto result = pool.borrow();
  REQUIRE(result.has_value());
  return std::move(result).value();
}

} // namespace

TEST_CASE("RegisteredMemoryPool::create returns a tl::expected carrying the pool or a DojioError") {
  auto ring_engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 64});
  REQUIRE(ring_engine_result.has_value());
  auto engine = std::move(ring_engine_result).value();
  auto pool_result = dojio::RegisteredMemoryPool::create(engine->ring(), 4096, 4);
  REQUIRE(pool_result.has_value());
  CHECK(pool_result->capacity() == 4);
  CHECK(pool_result->buffer_size() == 4096);
  CHECK(pool_result->available() == 4);
}

TEST_CASE("page_size() caches the sysconf(3) value in a function-local static") {
  CHECK(dojio::page_size() > 0);
  CHECK(dojio::page_size() == dojio::page_size());
  CHECK(dojio::page_size() == static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE)));
}

TEST_CASE("RegisteredMemoryPool borrow/return tracks the active counter") {
  auto ring_engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 64});
  REQUIRE(ring_engine_result.has_value());
  auto engine = std::move(ring_engine_result).value();
  auto pool_result = dojio::RegisteredMemoryPool::create(engine->ring(), 4096, 8);
  REQUIRE(pool_result.has_value());
  auto& pool = *pool_result;
  CHECK(pool.capacity() == 8);
  CHECK(pool.buffer_size() == 4096);
  CHECK(pool.active_count() == 0);
  CHECK(pool.available() == 8);
  {
    dojio::RegisteredBuffer buf = borrow_or_fail(pool);
    CHECK(static_cast<bool>(buf));
    CHECK(buf.data() != nullptr);
    CHECK(buf.size() == 0);
    CHECK(buf.capacity() == 4096);
    CHECK(pool.active_count() == 1);
    std::memset(buf.data(), 0xAB, buf.capacity());
    // the backing memory is whole mmap'ed pages, every chunk is cache-line aligned
    const std::size_t cache_line = dojio::cache_line_size();
    const long page = ::sysconf(_SC_PAGESIZE);
    CHECK(reinterpret_cast<std::uintptr_t>(pool.base()) % static_cast<std::uintptr_t>(page) == 0);
    CHECK(reinterpret_cast<std::uintptr_t>(buf.data()) % static_cast<std::uintptr_t>(cache_line) == 0);
  }
  CHECK(pool.active_count() == 0);
  CHECK(pool.available() == 8);
}

TEST_CASE("chunks are cache-line aligned even when buffer_size is not a multiple of it") {
  auto ring_engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 64});
  REQUIRE(ring_engine_result.has_value());
  auto engine = std::move(ring_engine_result).value();
  auto pool_result = dojio::RegisteredMemoryPool::create(engine->ring(), 100, 4);
  REQUIRE(pool_result.has_value());
  auto& pool = *pool_result;
  const std::size_t cache_line = dojio::cache_line_size();
  std::vector<dojio::RegisteredBuffer> bufs;
  for (std::int32_t i = 0; i < 4; ++i) {
    bufs.push_back(borrow_or_fail(pool));
  }
  for (const auto& b : bufs) {
    CHECK(b.capacity() == 100);
    CHECK(reinterpret_cast<std::uintptr_t>(b.data()) % static_cast<std::uintptr_t>(cache_line) == 0);
  }
}

TEST_CASE("RegisteredMemoryPool exhaustion returns a DojioError and buffers are reusable") {
  auto ring_engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 64});
  REQUIRE(ring_engine_result.has_value());
  auto engine = std::move(ring_engine_result).value();
  auto pool_result = dojio::RegisteredMemoryPool::create(engine->ring(), 64, 4);
  REQUIRE(pool_result.has_value());
  auto& pool = *pool_result;
  std::vector<dojio::RegisteredBuffer> bufs;
  for (std::int32_t i = 0; i < 4; ++i) {
    bufs.push_back(borrow_or_fail(pool));
  }
  CHECK(pool.active_count() == 4);
  auto exhausted = pool.borrow();
  CHECK_FALSE(exhausted.has_value());
  CHECK(exhausted.error().opcode == dojio::ErrorOpcode::MemoryPoolExhausted);
  CHECK_FALSE(exhausted.error().message.empty());
  bufs.clear();
  CHECK(pool.active_count() == 0);
  // buffers can be borrowed again after being returned
  dojio::RegisteredBuffer buf = borrow_or_fail(pool);
  CHECK(pool.active_count() == 1);
}

TEST_CASE("a second RegisteredMemoryPool on the same ring fails without crashing and leaves the first usable") {
  auto ring_engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 64});
  REQUIRE(ring_engine_result.has_value());
  auto engine = std::move(ring_engine_result).value();
  auto first_result = dojio::RegisteredMemoryPool::create(engine->ring(), 64, 2);
  REQUIRE(first_result.has_value());
  auto& first = *first_result;
  // the ring already has fixed buffers registered: the second registration is rejected
  auto second_result = dojio::RegisteredMemoryPool::create(engine->ring(), 64, 2);
  REQUIRE_FALSE(second_result.has_value());
  CHECK(second_result.error().opcode == dojio::ErrorOpcode::MemoryPoolRegisterBuffers);
  CHECK_FALSE(second_result.error().message.empty());
  // the failed pool must not have unregistered the first pool's fixed buffers:
  // a READ_FIXED through a buffer of the first pool still succeeds
  const std::string path = "/tmp/dojio_pool_test_second_pool";
  {
    const std::int32_t raw_fd = static_cast<std::int32_t>(::open(path.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644));
    REQUIRE(raw_fd >= 0);
    REQUIRE(::write(raw_fd, "still-registered", 17) == 17);
    ::close(raw_fd);
  }
  dojio::FileDescriptor fd{-1};
  engine->submit(dojio::open_at(path, O_RDONLY, 0),
                 [&](dojio::Result<dojio::FileDescriptor> r, dojio::OperationId, std::int32_t) {
                   REQUIRE(r.has_value());
                   fd = *r;
                 });
  engine->process(dojio::RingEngine::ProcessMode::Batch);
  REQUIRE(fd.value() >= 0);

  bool read_ok = false;
  engine->submit(dojio::pread(fd, 0), borrow_or_fail(first),
                 [&](dojio::Result<dojio::RegisteredBuffer> r, dojio::OperationId, std::int32_t res) {
                   REQUIRE(r.has_value());
                   CHECK(res == 17);
                   read_ok = true;
                 });
  engine->process(dojio::RingEngine::ProcessMode::Batch);
  CHECK(read_ok);
  CHECK(first.active_count() == 0);

  engine->submit(dojio::close(fd), [](dojio::Result<std::int32_t>, dojio::OperationId, std::int32_t) {});
  engine->process(dojio::RingEngine::ProcessMode::Batch);
  ::unlink(path.c_str());
  CHECK(engine->count_operations(dojio::RingEngine::OperationState::Failed) == 0);
}

TEST_CASE("RegisteredBuffer move semantics transfer ownership") {
  auto ring_engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 64});
  REQUIRE(ring_engine_result.has_value());
  auto engine = std::move(ring_engine_result).value();
  auto pool_result = dojio::RegisteredMemoryPool::create(engine->ring(), 128, 2);
  REQUIRE(pool_result.has_value());
  auto& pool = *pool_result;
  dojio::RegisteredBuffer a = borrow_or_fail(pool);
  const std::byte* data = a.data();
  dojio::RegisteredBuffer b = std::move(a);
  CHECK_FALSE(static_cast<bool>(a));
  CHECK(static_cast<bool>(b));
  CHECK(b.data() == data);
  CHECK(pool.active_count() == 1);
}

TEST_CASE("RegisteredBuffer::append fills the chunk and checks the capacity") {
  auto ring_engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 64});
  REQUIRE(ring_engine_result.has_value());
  auto engine = std::move(ring_engine_result).value();
  auto pool_result = dojio::RegisteredMemoryPool::create(engine->ring(), 8, 2);
  REQUIRE(pool_result.has_value());
  auto& pool = *pool_result;
  dojio::RegisteredBuffer buf = borrow_or_fail(pool);
  CHECK(buf.size() == 0);
  CHECK(buf.capacity() == 8);

  buf.append(std::string_view{"hello"});
  CHECK(buf.size() == 5);
  CHECK(std::memcmp(buf.data(), "hello", 5) == 0);

  // a span of bytes is appended right after the text
  const std::array<std::byte, 2> tail{std::byte{'!'}, std::byte{'?'}};
  buf.append(tail);
  CHECK(buf.size() == 7);
  CHECK(std::memcmp(buf.data(), "hello!?", 7) == 0);

  // the capacity is checked: 7 + 2 bytes do not fit in the 8-byte chunk
  CHECK_THROWS_AS(buf.append(std::string_view{"no"}), std::logic_error);
  CHECK_THROWS_AS(buf.append(std::array<std::byte, 2>{}), std::logic_error);
  CHECK(buf.size() == 7);

  // resize records a length (e.g. after a receive) but cannot exceed the capacity
  buf.resize(2);
  CHECK(buf.size() == 2);
  CHECK_THROWS_AS(buf.resize(9), std::logic_error);
  CHECK(buf.size() == 2);

  // clear() resets the length: the next append starts at the beginning of the chunk
  buf.clear();
  CHECK(buf.size() == 0);
  buf.append(std::string_view{"ab"});
  CHECK(buf.size() == 2);
  CHECK(std::memcmp(buf.data(), "ab", 2) == 0);

  // a moved-from buffer is empty
  dojio::RegisteredBuffer moved = std::move(buf);
  CHECK(buf.size() == 0);
  CHECK(moved.size() == 2);
  CHECK(pool.active_count() == 1);
}

TEST_CASE("RegisteredBuffer is usable as an io_uring fixed buffer for read") {
  auto ring_engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 64});
  REQUIRE(ring_engine_result.has_value());
  auto engine = std::move(ring_engine_result).value();
  // prepare a file with known content
  const std::string path = "/tmp/dojio_pool_test_file";
  {
    const std::int32_t fd = static_cast<std::int32_t>(::open(path.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644));
    REQUIRE(fd >= 0);
    const char* content = "fixed-buffer-content";
    REQUIRE(::write(fd, content, 20) == 20);
    ::close(fd);
  }
  dojio::FileDescriptor fd{-1};
  engine->submit(dojio::open_at(path, O_RDONLY, 0),
                 [&](dojio::Result<dojio::FileDescriptor> r, dojio::OperationId, std::int32_t) {
                   REQUIRE(r.has_value());
                   fd = *r;
                 });
  engine->process(dojio::RingEngine::ProcessMode::Batch);
  REQUIRE(fd.value() >= 0);

  auto pool_result = dojio::RegisteredMemoryPool::create(engine->ring(), 4096, 4);
  REQUIRE(pool_result.has_value());
  auto& pool = *pool_result;
  std::vector<std::byte> out;
  engine->submit(dojio::pread(fd, 0), borrow_or_fail(pool),
                 [&](dojio::Result<dojio::RegisteredBuffer> r, dojio::OperationId, std::int32_t res) {
                   REQUIRE(r.has_value());
                   CHECK(res == 20);
                   const auto& buf = *r;
                   // the received byte count is recorded by the buffer (like a vector resize)
                   CHECK(buf.size() == 20);
                   CHECK(buf.capacity() == 4096);
                   out.assign(buf.data(), buf.data() + res);
                 });
  engine->process(dojio::RingEngine::ProcessMode::Batch);
  CHECK(out.size() == 20);
  CHECK(std::memcmp(out.data(), "fixed-buffer-content", 20) == 0);
  CHECK(engine->count_operations(dojio::RingEngine::OperationState::Completed) == 2);

  engine->submit(dojio::close(fd), [](dojio::Result<std::int32_t>, dojio::OperationId, std::int32_t) {});
  engine->process(dojio::RingEngine::ProcessMode::Batch);
  ::unlink(path.c_str());
}

TEST_CASE("the pool holds its own reference on the ring and outlives the RingEngine") {
  auto ring_engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 16});
  REQUIRE(ring_engine_result.has_value());
  auto engine = std::move(ring_engine_result).value();
  {
    auto pool = dojio::RegisteredMemoryPool::create(engine->ring(), 64, 2);
    REQUIRE(pool.has_value());
    // the pool duplicated the ring fd with close-on-exec: it owns a reference on the
    // io_uring instance and stays fully usable after the engine dropped its own
    engine.reset();
    dojio::RegisteredBuffer buf = borrow_or_fail(*pool);
    CHECK(static_cast<bool>(buf));
    std::memset(buf.data(), 0x5A, buf.size());
    CHECK(pool->active_count() == 1);
    buf = dojio::RegisteredBuffer{};
    CHECK(pool->active_count() == 0);
  } // destroying the pool unregisters the fixed buffers and closes the duplicated fd
}

TEST_CASE("the pool is movable and the borrowed buffers stay valid across the move") {
  auto ring_engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 16});
  REQUIRE(ring_engine_result.has_value());
  auto engine = std::move(ring_engine_result).value();
  auto pool = dojio::RegisteredMemoryPool::create(engine->ring(), 64, 2);
  REQUIRE(pool.has_value());
  dojio::RegisteredBuffer buf = borrow_or_fail(*pool);
  const std::byte* data = buf.data();
  // move construction transfers the pimpl: the buffer references the implementation,
  // not the handle, so a borrowed buffer stays valid
  dojio::RegisteredMemoryPool moved = std::move(*pool);
  CHECK(moved.capacity() == 2);
  CHECK(moved.active_count() == 1);
  CHECK(buf.data() == data);
  // move assignment transfers it once more
  *pool = std::move(moved);
  CHECK(pool->active_count() == 1);
  CHECK(buf.data() == data);
  // releasing the buffer returns the chunk to the pool that now owns the pimpl
  buf = dojio::RegisteredBuffer{};
  CHECK(pool->active_count() == 0);
  CHECK(pool->available() == 2);
}

TEST_CASE("Pool invariant: destroying the pool with an active buffer terminates") {
  const std::int32_t pid = static_cast<std::int32_t>(::fork());
  REQUIRE(pid >= 0);
  if (pid == 0) {
    // child: destroy the pool while a buffer is still borrowed -> must terminate.
    // reset the signal handlers inherited from doctest so the child dies cleanly.
    ::signal(SIGABRT, SIG_DFL);
    auto ring_engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 16});
    REQUIRE(ring_engine_result.has_value());
    auto engine = std::move(ring_engine_result).value();
    // the buffer is declared first so it outlives the pool: destroying the pool with
    // one active buffer violates the invariant
    dojio::RegisteredBuffer buf;
    {
      auto pool_result = dojio::RegisteredMemoryPool::create(engine->ring(), 64, 2);
      REQUIRE(pool_result.has_value());
      buf = borrow_or_fail(*pool_result);
    }
    (void)buf;
    ::_exit(0); // not reached
  }
  std::int32_t status = 0;
  REQUIRE(::waitpid(static_cast<pid_t>(pid), &status, 0) == pid);
  CHECK(WIFSIGNALED(status));
}
