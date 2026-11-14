#include <doctest/doctest.h>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

#include <ringu/memory_pool.hpp>
#include <ringu/reactor.hpp>

TEST_CASE("RegisteredMemoryPool borrow/return tracks the active counter") {
  ringu::Reactor reactor{64};
  ringu::RegisteredMemoryPool pool{reactor.ring_fd(), 4096, 8};
  CHECK(pool.capacity() == 8);
  CHECK(pool.buffer_size() == 4096);
  CHECK(pool.active_count() == 0);
  CHECK(pool.available() == 8);
  {
    ringu::RegisteredBuffer buf = pool.borrow();
    CHECK(static_cast<bool>(buf));
    CHECK(buf.data() != nullptr);
    CHECK(buf.size() == 4096);
    CHECK(pool.active_count() == 1);
    std::memset(buf.data(), 0xAB, buf.size());
  }
  CHECK(pool.active_count() == 0);
  CHECK(pool.available() == 8);
}

TEST_CASE("RegisteredMemoryPool exhaustion throws and buffers are reusable") {
  ringu::Reactor reactor{64};
  ringu::RegisteredMemoryPool pool{reactor.ring_fd(), 64, 4};
  std::vector<ringu::RegisteredBuffer> bufs;
  for (std::int32_t i = 0; i < 4; ++i) {
    bufs.push_back(pool.borrow());
  }
  CHECK(pool.active_count() == 4);
  CHECK_THROWS_AS(pool.borrow(), std::runtime_error);
  bufs.clear();
  CHECK(pool.active_count() == 0);
  // buffers can be borrowed again after being returned
  ringu::RegisteredBuffer buf = pool.borrow();
  CHECK(pool.active_count() == 1);
}

TEST_CASE("RegisteredBuffer move semantics transfer ownership") {
  ringu::Reactor reactor{64};
  ringu::RegisteredMemoryPool pool{reactor.ring_fd(), 128, 2};
  ringu::RegisteredBuffer a = pool.borrow();
  const std::byte* data = a.data();
  ringu::RegisteredBuffer b = std::move(a);
  CHECK_FALSE(static_cast<bool>(a));
  CHECK(static_cast<bool>(b));
  CHECK(b.data() == data);
  CHECK(pool.active_count() == 1);
}

TEST_CASE("RegisteredBuffer is usable as an io_uring fixed buffer for read") {
  ringu::Reactor reactor{64};
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
  reactor.submit(ringu::Open::make(path, O_RDONLY, 0),
                 [&](ringu::Result<ringu::FileDescriptor> r, ringu::OperationId, std::int32_t) {
                   REQUIRE(r.has_value());
                   fd = *r;
                 });
  reactor.process(ringu::Reactor::ProcessMode::Batch);
  REQUIRE(fd.value() >= 0);

  ringu::RegisteredMemoryPool pool{reactor.ring_fd(), 4096, 4};
  std::vector<std::byte> out;
  reactor.submit(ringu::Pread::make(fd, 0), pool.borrow(),
                 [&](ringu::Result<ringu::RegisteredBuffer> r, ringu::OperationId, std::int32_t res) {
                   REQUIRE(r.has_value());
                   CHECK(res == 20);
                   const auto& buf = *r;
                   out.assign(buf.data(), buf.data() + res);
                 });
  reactor.process(ringu::Reactor::ProcessMode::Batch);
  CHECK(out.size() == 20);
  CHECK(std::memcmp(out.data(), "fixed-buffer-content", 20) == 0);
  CHECK(reactor.count_operations(ringu::Reactor::OperationState::Completed) == 2);

  reactor.submit(ringu::Close::make(fd), [](ringu::Result<std::int32_t>, ringu::OperationId, std::int32_t) {});
  reactor.process(ringu::Reactor::ProcessMode::Batch);
  ::unlink(path.c_str());
}

TEST_CASE("Pool invariant: destroying the pool with an active buffer terminates") {
  const std::int32_t pid = static_cast<std::int32_t>(::fork());
  REQUIRE(pid >= 0);
  if (pid == 0) {
    // child: destroy the pool while a buffer is still borrowed -> must terminate.
    // reset the signal handlers inherited from doctest so the child dies cleanly.
    ::signal(SIGABRT, SIG_DFL);
    ringu::Reactor reactor{16};
    auto* pool = new ringu::RegisteredMemoryPool{reactor.ring_fd(), 64, 2};
    ringu::RegisteredBuffer buf = pool->borrow();
    (void)buf;
    delete pool; // invariant violation: active_count == 1
    ::_exit(0);  // not reached
  }
  std::int32_t status = 0;
  REQUIRE(::waitpid(static_cast<pid_t>(pid), reinterpret_cast<int*>(&status), 0) == pid);
  CHECK(WIFSIGNALED(status));
}
