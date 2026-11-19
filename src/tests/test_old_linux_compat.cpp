// Built as its own target with -DRINGU_FORCE_LINUX_OLD_KERNEL, which makes the
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

#include <ringu/result.hpp>
#include <ringu/ring_engine.hpp>

namespace {

/// The whole point of this target: fail to build if the flag is not enforced.
#if !defined(RINGU_FORCE_LINUX_OLD_KERNEL)
#error "this target requires -DRINGU_FORCE_LINUX_OLD_KERNEL"
#endif

std::string make_tmp_path(const std::string& tag) {
  static std::int32_t counter = 0;
  counter += 1;
  return (std::filesystem::temp_directory_path() / fmt::format("ringu_test_{}_{}_{}", tag, ::getpid(), counter))
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

TEST_CASE("RINGU_FORCE_LINUX_OLD_KERNEL runs bind/listen through the synchronous path") {
  for (const bool sqpoll : {true, false}) {
    CAPTURE(sqpoll);
    auto ring_engine_result = ringu::RingEngine::create(ringu::RingEngineOptions{.entries = 64, .sqpoll = sqpoll});
    REQUIRE(ring_engine_result.has_value());
    auto engine = std::move(ring_engine_result).value();
    using Mode = ringu::RingEngine::ProcessMode;
    using State = ringu::RingEngine::OperationState;

    const std::int32_t lfd = static_cast<std::int32_t>(::socket(AF_INET, SOCK_STREAM, 0));
    REQUIRE(lfd >= 0);
    const std::int32_t one = 1;
    ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    bool bound = false;
    engine->submit(ringu::bind(ringu::FileDescriptor{lfd}, ringu::Ipv4Endpoint::loopback(0)),
                   [&](ringu::Result<std::int32_t> r, ringu::OperationId, std::int32_t res) {
                     REQUIRE(r.has_value());
                     CHECK(res == 0);
                     bound = true;
                   });
    engine->process(Mode::Batch);
    CHECK(bound);

    bool listened = false;
    engine->submit(ringu::listen(ringu::FileDescriptor{lfd}, 16),
                   [&](ringu::Result<std::int32_t> r, ringu::OperationId, std::int32_t res) {
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

TEST_CASE("RINGU_FORCE_LINUX_OLD_KERNEL runs ftruncate and pipe through the synchronous path") {
  for (const bool sqpoll : {true, false}) {
    CAPTURE(sqpoll);
    auto ring_engine_result = ringu::RingEngine::create(ringu::RingEngineOptions{.entries = 64, .sqpoll = sqpoll});
    REQUIRE(ring_engine_result.has_value());
    auto engine = std::move(ring_engine_result).value();
    using Mode = ringu::RingEngine::ProcessMode;
    using State = ringu::RingEngine::OperationState;
    const std::string path = make_tmp_file("old_linux_kernel", "0123456789");

    ringu::FileDescriptor fd{-1};
    engine->submit(ringu::open_at(path, O_RDWR),
                   [&](ringu::Result<ringu::FileDescriptor> r, ringu::OperationId, std::int32_t) {
                     REQUIRE(r.has_value());
                     fd = *r;
                   });
    engine->process(Mode::Batch);
    REQUIRE(fd.value() >= 0);

    // ftruncate runs inline (old kernel) and truncates the file
    engine->submit(ringu::ftruncate(fd, 4), [&](ringu::Result<std::int32_t> r, ringu::OperationId, std::int32_t res) {
      REQUIRE(r.has_value());
      CHECK(res == 0);
    });
    engine->process(Mode::Batch);
    engine->submit(ringu::fstat(fd), std::vector<std::byte>(ringu::k_statx_size),
                   [&](ringu::Result<std::vector<std::byte>> r, ringu::OperationId, std::int32_t res) {
                     REQUIRE(r.has_value());
                     CHECK(res == 0);
                     const auto* st = reinterpret_cast<const struct stat*>(r->data());
                     CHECK(st->st_size == 4);
                   });
    engine->process(Mode::Batch);

    // pipe runs inline (old kernel) and yields two usable descriptors
    ringu::PipeBuffer pipe_fds{};
    engine->submit(ringu::pipe(0), std::move(pipe_fds),
                   [&](ringu::Result<ringu::PipeBuffer> r, ringu::OperationId, std::int32_t res) {
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

    engine->submit(ringu::close(fd), [](ringu::Result<std::int32_t>, ringu::OperationId, std::int32_t) {});
    engine->submit(ringu::close(pipe_fds[0]), [](ringu::Result<std::int32_t>, ringu::OperationId, std::int32_t) {});
    engine->submit(ringu::close(pipe_fds[1]), [](ringu::Result<std::int32_t>, ringu::OperationId, std::int32_t) {});
    engine->process(Mode::Batch);
    CHECK(engine->count_operations(State::Failed) == 0);
    ::unlink(path.c_str());
  }
}
