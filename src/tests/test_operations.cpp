#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <dojio/buffer.hpp>
#include <dojio/operations.hpp>
#include <dojio/ring_engine.hpp>

// StaticBuffer satisfies the buffer concept accepted by the operations
static_assert(dojio::BufferLike<dojio::StaticBuffer<64>>);
static_assert(dojio::BufferLike<dojio::AcceptBuffer>);
static_assert(dojio::BufferLike<std::vector<std::byte>>);

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

TEST_CASE("every operation embeds its buffer and exposes its result aliases") {
  const dojio::FileDescriptor fd{7};
  const std::vector<std::byte> buffer(4);

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

  // the buffer is a member of the operation struct
  CHECK(dojio::read(fd, buffer).fd == fd);
  CHECK(dojio::read(fd, buffer).buffer.size() == 4);
  CHECK(dojio::pread(fd, 42, buffer).fd == fd);
  CHECK(dojio::pread(fd, 42, buffer).offset == 42);
  CHECK(dojio::write(fd, buffer).fd == fd);
  CHECK(dojio::pwrite(fd, 8, buffer).fd == fd);
  CHECK(dojio::pwrite(fd, 8, buffer).offset == 8);
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
  CHECK(dojio::send(fd, buffer, MSG_NOSIGNAL).flags == MSG_NOSIGNAL);
#else
  CHECK(dojio::send(fd, buffer, 0).flags == 0);
#endif
  CHECK(dojio::recv(fd, buffer).fd == fd);

  const auto timeout = dojio::timeout(std::chrono::milliseconds(1500));
  CHECK(timeout.ts.sec == 1);
  CHECK(timeout.ts.nsec == 500000000);

  // every operation exposes a result_value struct and a tl::expected result alias
  static_assert(
      std::is_same_v<dojio::ReadOp<std::vector<std::byte>>::result_value, dojio::ReadResult<std::vector<std::byte>>>);
  static_assert(std::is_same_v<dojio::TimeoutOp::result_value, dojio::TimeoutResult>);
  static_assert(std::is_same_v<dojio::OpenAtOp::result_value, dojio::OpenAtResult>);
  static_assert(dojio::OperationType<dojio::ReadOp<std::vector<std::byte>>>);
  static_assert(dojio::OperationType<dojio::TimeoutOp>);
}

// every operation exposes a result_value struct and a tl::expected result alias
static_assert(dojio::OperationType<dojio::OpenAtOp>);
static_assert(dojio::OperationType<dojio::ReadOp<std::vector<std::byte>>>);
static_assert(dojio::OperationType<dojio::PreadOp<std::vector<std::byte>>>);
static_assert(dojio::OperationType<dojio::WriteOp<std::vector<std::byte>>>);
static_assert(dojio::OperationType<dojio::PwriteOp<std::vector<std::byte>>>);
static_assert(dojio::OperationType<dojio::FtruncateOp>);
static_assert(dojio::OperationType<dojio::PipeOp>);
static_assert(dojio::OperationType<dojio::FstatOp>);
static_assert(dojio::OperationType<dojio::CloseOp>);
static_assert(dojio::OperationType<dojio::ListenOp>);
static_assert(dojio::OperationType<dojio::AcceptOp>);
static_assert(dojio::OperationType<dojio::SendOp<std::vector<std::byte>>>);
static_assert(dojio::OperationType<dojio::RecvOp<std::vector<std::byte>>>);
static_assert(dojio::OperationType<dojio::FsyncOp>);
static_assert(dojio::OperationType<dojio::BindOp>);
static_assert(dojio::OperationType<dojio::ConnectOp>);
static_assert(dojio::OperationType<dojio::TimeoutOp>);
// the operations with no caller buffer silently embed a monostate
static_assert(std::is_same_v<dojio::FstatOp::buffer_type, std::monostate>);
static_assert(std::is_same_v<dojio::PipeOp::buffer_type, std::monostate>);
static_assert(std::is_same_v<dojio::AcceptOp::buffer_type, std::monostate>);

#if defined(__linux__)
TEST_CASE("supports_op reports the opcodes the running kernel exposes") {
  auto ring_engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 64});
  REQUIRE(ring_engine_result.has_value());
  auto engine = std::move(ring_engine_result).value();
  // READ and TIMEOUT have been part of io_uring since it appeared in Linux 5.1:
  // every kernel that can create a ring exposes them (IORING_REGISTER_PROBE)
  CHECK(engine->supports_op(dojio::IoOp::Read));
  CHECK(engine->supports_op(dojio::IoOp::Timeout));
  CHECK(engine->capabilities().bind_listen ==
        (engine->supports_op(dojio::IoOp::Bind) && engine->supports_op(dojio::IoOp::Listen)));
  CHECK(engine->capabilities().ftruncate == engine->supports_op(dojio::IoOp::Ftruncate));
  CHECK(engine->capabilities().pipe == engine->supports_op(dojio::IoOp::Pipe));
}
#endif

TEST_CASE("Accept embeds its peer-address buffer transparently") {
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
                   [&](dojio::RingEngine&, dojio::Result<dojio::BindResult> r) {
                     REQUIRE(r.has_value());
                     CHECK(r->fd.value() == lfd);
                     bound = true;
                   });
    engine->process(Mode::Batch);
    CHECK(bound);

    sockaddr_in addr{};
    socklen_t alen = sizeof(addr);
    REQUIRE(::getsockname(lfd, reinterpret_cast<sockaddr*>(&addr), &alen) == 0);

    engine->submit(dojio::listen(dojio::FileDescriptor{lfd}, 16),
                   [](dojio::RingEngine&, dojio::Result<dojio::ListenResult>) {});
    engine->process(Mode::Batch);

    const std::int32_t cfd = static_cast<std::int32_t>(::socket(AF_INET, SOCK_STREAM, 0));
    REQUIRE(cfd >= 0);
    REQUIRE(::connect(cfd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);

    dojio::FileDescriptor accepted{-1};
    engine->submit(dojio::accept(dojio::FileDescriptor{lfd}),
                   [&](dojio::RingEngine&, dojio::Result<dojio::AcceptResult> r) {
                     REQUIRE(r.has_value());
                     accepted = r->fd;
                   });
    engine->process(Mode::Batch);
    REQUIRE(accepted.value() >= 0);

    // the peer address was captured by the operation's embedded buffer: verify the
    // connection through the accepted descriptor
    sockaddr_in peer{};
    socklen_t plen = sizeof(peer);
    REQUIRE(::getpeername(accepted.value(), reinterpret_cast<sockaddr*>(&peer), &plen) == 0);
    CHECK(peer.sin_family == AF_INET);

    engine->submit(dojio::close(accepted), [](dojio::RingEngine&, dojio::Result<dojio::CloseResult>) {});
    engine->process(Mode::Batch);
    ::close(cfd);
    ::close(lfd);
  }
}
