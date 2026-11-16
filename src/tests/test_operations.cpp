#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <ringu/buffer.hpp>
#include <ringu/operations.hpp>
#include <ringu/reactor.hpp>

// StaticBuffer satisfies the buffer concept accepted by Reactor::submit
static_assert(ringu::BufferLike<ringu::StaticBuffer<64>>);
static_assert(ringu::BufferLike<ringu::AcceptBuffer>);

TEST_CASE("StaticBuffer is a fixed-size buffer") {
  ringu::StaticBuffer<64> buf;
  CHECK(buf.size() == 64);
  CHECK(buf.data() != nullptr);
  CHECK(buf.span().size() == 64);
  buf.data()[0] = std::byte{0xAB};
  CHECK(buf.span()[0] == std::byte{0xAB});

  ringu::AcceptBuffer accept_buf;
  CHECK(accept_buf.size() == ringu::k_sockaddr_storage_size);
  CHECK(ringu::k_sockaddr_storage_size == 128);
}

TEST_CASE("every operation has a free function returning a constructed operation") {
  const ringu::FileDescriptor fd{7};

  const auto open = ringu::open_at("/tmp/some_file", O_RDWR, 0644);
  CHECK(open.dirfd == ringu::FileDescriptor{ringu::k_at_fdcwd});
  CHECK(open.path == "/tmp/some_file");
  CHECK(open.flags == O_RDWR);
  CHECK(open.mode == 0644);
  const auto open_default_mode = ringu::open_at("/tmp/some_file", O_RDONLY);
  CHECK(open_default_mode.mode == 0644);
  const auto openat = ringu::open_at(fd, "/tmp/some_file", O_RDONLY);
  CHECK(openat.dirfd == fd);
  CHECK(openat.path == "/tmp/some_file");
  CHECK(openat.flags == O_RDONLY);
  CHECK(openat.mode == 0644);

  CHECK(ringu::read(fd).fd == fd);
  CHECK(ringu::pread(fd, 42).fd == fd);
  CHECK(ringu::pread(fd, 42).offset == 42);
  CHECK(ringu::write(fd).fd == fd);
  CHECK(ringu::pwrite(fd, 8).offset == 8);
  const auto lseek = ringu::lseek(fd, 16, SEEK_SET);
  CHECK(lseek.fd == fd);
  CHECK(lseek.offset == 16);
  CHECK(lseek.whence == SEEK_SET);
  CHECK(ringu::fstat(fd).fd == fd);
  CHECK(ringu::close(fd).fd == fd);
  CHECK(ringu::listen(fd, 128).backlog == 128);
  const auto accept = ringu::accept(fd, 0);
  CHECK(accept.fd == fd);
  CHECK(accept.flags == 0);
  CHECK(accept.addr_len == 0);
  CHECK(ringu::send(fd, MSG_NOSIGNAL).flags == MSG_NOSIGNAL);
  CHECK(ringu::recv(fd).fd == fd);

  const auto timeout = ringu::timeout(std::chrono::milliseconds(1500));
  CHECK(timeout.ts.sec == 1);
  CHECK(timeout.ts.nsec == 500000000);
}

TEST_CASE("Accept submitted with a static AcceptBuffer receives the peer address") {
  ringu::Reactor reactor{64};
  using Mode = ringu::Reactor::ProcessMode;

  const std::int32_t lfd = static_cast<std::int32_t>(::socket(AF_INET, SOCK_STREAM, 0));
  REQUIRE(lfd >= 0);
  const std::int32_t one = 1;
  ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  // bind through the reactor (inline Bind operation)
  bool bound = false;
  reactor.submit(ringu::bind(ringu::FileDescriptor{lfd}, ringu::Ipv4Endpoint::loopback(0)),
                 [&](ringu::Result<std::int32_t> r, ringu::OperationId, std::int32_t res) {
                   REQUIRE(r.has_value());
                   CHECK(res == 0);
                   bound = true;
                 });
  reactor.process(Mode::Batch);
  CHECK(bound);

  sockaddr_in addr{};
  socklen_t alen = sizeof(addr);
  REQUIRE(::getsockname(lfd, reinterpret_cast<sockaddr*>(&addr), &alen) == 0);

  reactor.submit(ringu::listen(ringu::FileDescriptor{lfd}, 16),
                 [](ringu::Result<std::int32_t>, ringu::OperationId, std::int32_t) {});
  reactor.process(Mode::Batch);

  const std::int32_t cfd = static_cast<std::int32_t>(::socket(AF_INET, SOCK_STREAM, 0));
  REQUIRE(cfd >= 0);
  REQUIRE(::connect(cfd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);

  ringu::FileDescriptor accepted{-1};
  sockaddr_in peer{};
  reactor.submit(ringu::accept(ringu::FileDescriptor{lfd}), ringu::AcceptBuffer{},
                 [&](ringu::Result<ringu::FileDescriptor> r, ringu::OperationId, std::int32_t res) {
                   REQUIRE(r.has_value());
                   CHECK(res >= 0);
                   accepted = *r;
                 });
  reactor.process(Mode::Batch);
  REQUIRE(accepted.value() >= 0);

  // the AcceptBuffer was consumed by the submit; verify the peer via getpeername
  socklen_t plen = sizeof(peer);
  REQUIRE(::getpeername(accepted.value(), reinterpret_cast<sockaddr*>(&peer), &plen) == 0);
  CHECK(peer.sin_family == AF_INET);

  reactor.submit(ringu::close(accepted), [](ringu::Result<std::int32_t>, ringu::OperationId, std::int32_t) {});
  reactor.process(Mode::Batch);
  ::close(cfd);
  ::close(lfd);
}
