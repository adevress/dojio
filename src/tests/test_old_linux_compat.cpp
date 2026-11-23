// Built as its own target with -DDOJIO_FORCE_LINUX_OLD_KERNEL, which makes the
// dispatcher send the syscall-backed operations (bind/listen, ftruncate, pipe) down
// the inline path even when the running kernel exposes
// IORING_OP_BIND/LISTEN/FTRUNCATE/PIPE.
//
// The opposite path (io_uring opcodes) is covered by the regular test targets, which
// are built without the flag.

#include <doctest/doctest.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <variant>
#include <vector>

#include <fmt/format.h>

#include <dojio/result.hpp>
#include <dojio/ring_engine.hpp>

namespace {

/// The whole point of this target: fail to build if the flag is not enforced.
#if !defined(DOJIO_FORCE_LINUX_OLD_KERNEL)
#error "this target requires -DDOJIO_FORCE_LINUX_OLD_KERNEL"
#endif

std::string make_tmp_path(const std::string& tag) {
  static std::int32_t counter = 0;
  counter += 1;
  return (std::filesystem::temp_directory_path() / fmt::format("dojio_test_{}_{}_{}", tag, ::getpid(), counter))
      .string();
}

std::string make_tmp_file(const std::string& tag, const std::string& content) {
  const std::string path = make_tmp_path(tag);
  const std::int32_t fd = static_cast<std::int32_t>(::open(path.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644));
  REQUIRE(fd >= 0);
  REQUIRE(::write(fd, content.data(), content.size()) == static_cast<::ssize_t>(content.size()));
  ::close(fd);
  return path;
}

} // namespace

TEST_CASE("DOJIO_FORCE_LINUX_OLD_KERNEL runs bind/listen through the synchronous path") {
  for (const bool sqpoll : {true, false}) {
    CAPTURE(sqpoll);
    auto ring_engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 64, .sqpoll = sqpoll});
    REQUIRE(ring_engine_result.has_value());
    auto engine = std::move(ring_engine_result).value();
    using Mode = dojio::RingEngine::ProcessMode;
    using State = dojio::RingEngine::OperationState;

    const std::int32_t lfd = static_cast<std::int32_t>(::socket(AF_INET, SOCK_STREAM, 0));
    REQUIRE(lfd >= 0);
    const std::int32_t one = 1;
    ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    bool bound = false;
    engine->submit(dojio::bind(dojio::FileDescriptor{lfd}, dojio::Ipv4Endpoint::loopback(0)),
                   [&](dojio::Result<std::int32_t> r, dojio::OperationId, std::int32_t res) {
                     REQUIRE(r.has_value());
                     CHECK(res == 0);
                     bound = true;
                   });
    engine->process(Mode::Batch);
    CHECK(bound);

    bool listened = false;
    engine->submit(dojio::listen(dojio::FileDescriptor{lfd}, 16),
                   [&](dojio::Result<std::int32_t> r, dojio::OperationId, std::int32_t res) {
                     REQUIRE(r.has_value());
                     CHECK(res == 0);
                     listened = true;
                   });
    engine->process(Mode::Batch);
    CHECK(listened);
    CHECK(engine->count_operations(State::Failed) == 0);

    // the inline syscalls really took effect: a plain client can connect
    sockaddr_in addr{};
    socklen_t alen = sizeof(addr);
    REQUIRE(::getsockname(lfd, reinterpret_cast<sockaddr*>(&addr), &alen) == 0);
    const std::int32_t cfd = static_cast<std::int32_t>(::socket(AF_INET, SOCK_STREAM, 0));
    REQUIRE(cfd >= 0);
    REQUIRE(::connect(cfd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);

    ::close(cfd);
    ::close(lfd);
  }
}

TEST_CASE("DOJIO_FORCE_LINUX_OLD_KERNEL runs ftruncate and pipe through the synchronous path") {
  for (const bool sqpoll : {true, false}) {
    CAPTURE(sqpoll);
    auto ring_engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 64, .sqpoll = sqpoll});
    REQUIRE(ring_engine_result.has_value());
    auto engine = std::move(ring_engine_result).value();
    using Mode = dojio::RingEngine::ProcessMode;
    using State = dojio::RingEngine::OperationState;
    const std::string path = make_tmp_file("old_linux_kernel", "0123456789");

    dojio::FileDescriptor fd{-1};
    engine->submit(dojio::open_at(path, O_RDWR),
                   [&](dojio::Result<dojio::FileDescriptor> r, dojio::OperationId, std::int32_t) {
                     REQUIRE(r.has_value());
                     fd = *r;
                   });
    engine->process(Mode::Batch);
    REQUIRE(fd.value() >= 0);

    // ftruncate runs inline (old kernel) and truncates the file
    engine->submit(dojio::ftruncate(fd, 4), [&](dojio::Result<std::int32_t> r, dojio::OperationId, std::int32_t res) {
      REQUIRE(r.has_value());
      CHECK(res == 0);
    });
    engine->process(Mode::Batch);
    engine->submit(dojio::fstat(fd), std::vector<std::byte>(dojio::k_statx_size),
                   [&](dojio::Result<struct stat> r, dojio::OperationId, std::int32_t res) {
                     REQUIRE(r.has_value());
                     CHECK(res == 0);
                     CHECK(r->st_size == 4);
                   });
    engine->process(Mode::Batch);

    // pipe runs inline (old kernel) and yields two usable descriptors
    dojio::PipeBuffer pipe_fds{};
    engine->submit(dojio::pipe(0), std::move(pipe_fds),
                   [&](dojio::Result<dojio::PipeBuffer> r, dojio::OperationId, std::int32_t res) {
                     REQUIRE(r.has_value());
                     CHECK(res == 0);
                     pipe_fds = *r;
                   });
    engine->process(Mode::Batch);
    REQUIRE(pipe_fds[0].value() >= 0);
    REQUIRE(pipe_fds[1].value() >= 0);
    REQUIRE(::write(pipe_fds[1].value(), "xy", 2) == 2);
    std::array<char, 2> buf{};
    REQUIRE(::read(pipe_fds[0].value(), buf.data(), buf.size()) == 2);
    CHECK(std::string(buf.data(), buf.size()) == "xy");

    engine->submit(dojio::close(fd), [](dojio::Result<std::int32_t>, dojio::OperationId, std::int32_t) {});
    engine->submit(dojio::close(pipe_fds[0]), [](dojio::Result<std::int32_t>, dojio::OperationId, std::int32_t) {});
    engine->submit(dojio::close(pipe_fds[1]), [](dojio::Result<std::int32_t>, dojio::OperationId, std::int32_t) {});
    engine->process(Mode::Batch);
    CHECK(engine->count_operations(State::Failed) == 0);
    ::unlink(path.c_str());
  }
}

TEST_CASE("Batch runs inline operations chained from a completion callback") {
  for (const bool sqpoll : {true, false}) {
    CAPTURE(sqpoll);
    auto ring_engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 64, .sqpoll = sqpoll});
    REQUIRE(ring_engine_result.has_value());
    auto engine = std::move(ring_engine_result).value();
    using State = dojio::RingEngine::OperationState;
    using Mode = dojio::RingEngine::ProcessMode;
    const std::string path = make_tmp_file("batch_chained_inline", "0123456789");

    dojio::FileDescriptor fd{-1};
    bool truncated = false;
    // open_at completes on the ring; its callback chains an ftruncate, which runs
    // inline under DOJIO_FORCE_LINUX_OLD_KERNEL and must still be drained by Batch
    engine->submit(dojio::open_at(path, O_RDWR),
                   [&](dojio::Result<dojio::FileDescriptor> r, dojio::OperationId, std::int32_t) {
                     REQUIRE(r.has_value());
                     fd = *r;
                     engine->submit(dojio::ftruncate(fd, 4),
                                    [&](dojio::Result<std::int32_t> r2, dojio::OperationId, std::int32_t res) {
                                      REQUIRE(r2.has_value());
                                      CHECK(res == 0);
                                      truncated = true;
                                    });
                   });
    engine->process(Mode::Batch);
    CHECK(truncated);
    CHECK(engine->count_operations(State::Pending) == 0);
    CHECK(engine->count_operations(State::Failed) == 0);

    engine->submit(dojio::close(fd), [](dojio::Result<std::int32_t>, dojio::OperationId, std::int32_t) {});
    engine->process(Mode::Batch);
    ::unlink(path.c_str());
  }
}
