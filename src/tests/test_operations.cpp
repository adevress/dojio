#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <dojio/buffer.hpp>
#include <dojio/operations.hpp>
#include <dojio/ring_engine.hpp>

// StaticBuffer satisfies the buffer concept accepted by RingEngine::submit
static_assert(dojio::BufferLike<dojio::StaticBuffer<64>>);
static_assert(dojio::BufferLike<dojio::AcceptBuffer>);

TEST_CASE("StaticBuffer is a fixed-size buffer") {
  dojio::StaticBuffer<64> buf;
  CHECK(buf.size() == 64);
  CHECK(buf.data() != nullptr);
  CHECK(buf.span().size() == 64);
  buf.data()[0] = std::byte{0xAB};
  CHECK(buf.span()[0] == std::byte{0xAB});

  dojio::AcceptBuffer accept_buf;
  CHECK(accept_buf.size() == dojio::k_sockaddr_storage_size);
  CHECK(dojio::k_sockaddr_storage_size == 128);
}

TEST_CASE("every operation has a free function returning a constructed operation") {
  const dojio::FileDescriptor fd{7};

  const auto open = dojio::open_at("/tmp/some_file", O_RDWR, 0644);
  CHECK(open.dirfd == dojio::FileDescriptor{dojio::k_at_fdcwd});
  CHECK(open.path == "/tmp/some_file");
  CHECK(open.flags == O_RDWR);
  CHECK(open.mode == 0644);
  const auto open_default_mode = dojio::open_at("/tmp/some_file", O_RDONLY);
  CHECK(open_default_mode.mode == 0644);
  const auto openat = dojio::open_at(fd, "/tmp/some_file", O_RDONLY);
  CHECK(openat.dirfd == fd);
  CHECK(openat.path == "/tmp/some_file");
  CHECK(openat.flags == O_RDONLY);
  CHECK(openat.mode == 0644);

  CHECK(dojio::read(fd).fd == fd);
  CHECK(dojio::pread(fd, 42).fd == fd);
  CHECK(dojio::pread(fd, 42).offset == 42);
  CHECK(dojio::write(fd).fd == fd);
  CHECK(dojio::pwrite(fd, 8).fd == fd);
  CHECK(dojio::pwrite(fd, 8).offset == 8);
  const auto ftruncate = dojio::ftruncate(fd, 4096);
  CHECK(ftruncate.fd == fd);
  CHECK(ftruncate.length == 4096);
  CHECK(dojio::pipe(O_CLOEXEC).flags == O_CLOEXEC);
  CHECK(dojio::fstat(fd).fd == fd);
  CHECK(dojio::close(fd).fd == fd);
  CHECK(dojio::listen(fd, 128).backlog == 128);
  const auto accept = dojio::accept(fd, 0);
  CHECK(accept.fd == fd);
  CHECK(accept.flags == 0);
  CHECK(accept.addr_len == 0);
#if defined(MSG_NOSIGNAL)
  // MSG_NOSIGNAL is Linux-specific: exercise the flags pass-through where available
  CHECK(dojio::send(fd, MSG_NOSIGNAL).flags == MSG_NOSIGNAL);
#else
  CHECK(dojio::send(fd, 0).flags == 0);
#endif
  CHECK(dojio::recv(fd).fd == fd);

  const auto timeout = dojio::timeout(std::chrono::milliseconds(1500));
  CHECK(timeout.ts.sec == 1);
  CHECK(timeout.ts.nsec == 500000000);
}

TEST_CASE("extract_os_version caches the kernel version and drives the dispatcher") {
  const dojio::detail::OsVersion& version = dojio::detail::extract_os_version();
  // the version is cached in a function-local static: the same object is returned
  CHECK(&version == &dojio::detail::extract_os_version());
#if defined(__linux__)
  CHECK(version.major > 0);
  // the capability queries follow the kernel version thresholds
  CHECK(dojio::detail::kernel_supports_io_uring() == (version.major > 5 || (version.major == 5 && version.minor >= 1)));
  CHECK(dojio::detail::kernel_supports_bind_listen() ==
        (version.major > 6 || (version.major == 6 && version.minor >= 11)));
  CHECK(dojio::detail::kernel_supports_ftruncate() ==
        (version.major > 6 || (version.major == 6 && version.minor >= 9)));
  CHECK(dojio::detail::kernel_supports_pipe() == (version.major > 6 || (version.major == 6 && version.minor >= 16)));
#else
  CHECK(version.major == 0);
  CHECK_FALSE(dojio::detail::kernel_supports_io_uring());
  CHECK_FALSE(dojio::detail::kernel_supports_bind_listen());
  CHECK_FALSE(dojio::detail::kernel_supports_ftruncate());
  CHECK_FALSE(dojio::detail::kernel_supports_pipe());
#endif
}

TEST_CASE("Accept submitted with a static AcceptBuffer receives the peer address") {
  for (const bool sqpoll : {true, false}) {
    CAPTURE(sqpoll);
    auto ring_engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 64, .sqpoll = sqpoll});
    REQUIRE(ring_engine_result.has_value());
    auto engine = std::move(ring_engine_result).value();
    CHECK(engine->sqpoll_enabled() == sqpoll);
    using Mode = dojio::RingEngine::ProcessMode;

    const std::int32_t lfd = static_cast<std::int32_t>(::socket(AF_INET, SOCK_STREAM, 0));
    REQUIRE(lfd >= 0);
    const std::int32_t one = 1;
    ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    // bind through the RingEngine (inline Bind operation)
    bool bound = false;
    engine->submit(dojio::bind(dojio::FileDescriptor{lfd}, dojio::Ipv4Endpoint::loopback(0)),
                   [&](dojio::Result<std::int32_t> r, dojio::OperationId, std::int32_t res) {
                     REQUIRE(r.has_value());
                     CHECK(res == 0);
                     bound = true;
                   });
    engine->process(Mode::Batch);
    CHECK(bound);

    sockaddr_in addr{};
    socklen_t alen = sizeof(addr);
    REQUIRE(::getsockname(lfd, reinterpret_cast<sockaddr*>(&addr), &alen) == 0);

    engine->submit(dojio::listen(dojio::FileDescriptor{lfd}, 16),
                   [](dojio::Result<std::int32_t>, dojio::OperationId, std::int32_t) {});
    engine->process(Mode::Batch);

    const std::int32_t cfd = static_cast<std::int32_t>(::socket(AF_INET, SOCK_STREAM, 0));
    REQUIRE(cfd >= 0);
    REQUIRE(::connect(cfd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);

    dojio::FileDescriptor accepted{-1};
    sockaddr_in peer{};
    engine->submit(dojio::accept(dojio::FileDescriptor{lfd}), dojio::AcceptBuffer{},
                   [&](dojio::Result<dojio::FileDescriptor> r, dojio::OperationId, std::int32_t res) {
                     REQUIRE(r.has_value());
                     CHECK(res >= 0);
                     accepted = *r;
                   });
    engine->process(Mode::Batch);
    REQUIRE(accepted.value() >= 0);

    // the AcceptBuffer was consumed by the submit; verify the peer via getpeername
    socklen_t plen = sizeof(peer);
    REQUIRE(::getpeername(accepted.value(), reinterpret_cast<sockaddr*>(&peer), &plen) == 0);
    CHECK(peer.sin_family == AF_INET);

    engine->submit(dojio::close(accepted), [](dojio::Result<std::int32_t>, dojio::OperationId, std::int32_t) {});
    engine->process(Mode::Batch);
    ::close(cfd);
    ::close(lfd);
  }
}
