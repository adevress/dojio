#include <doctest/doctest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <variant>
#include <vector>

#include <ringu/reactor.hpp>
#include <ringu/result.hpp>

namespace {

std::vector<std::byte> to_bytes(std::string_view s) {
  return {reinterpret_cast<const std::byte*>(s.data()), reinterpret_cast<const std::byte*>(s.data()) + s.size()};
}

std::string to_string(const std::vector<std::byte>& b) { return {reinterpret_cast<const char*>(b.data()), b.size()}; }

} // namespace

TEST_CASE("send/recv over a socketpair") {
  ringu::Reactor reactor{64};
  using Mode = ringu::Reactor::ProcessMode;
  std::int32_t sv[2];
  REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, reinterpret_cast<int*>(sv)) == 0);

  std::string received;
  std::int32_t sent = -1;
  reactor.submit(ringu::Recv{ringu::FileDescriptor{sv[0]}}, std::vector<std::byte>(64),
                 [&](ringu::Result<std::vector<std::byte>> r, ringu::OperationId, std::int32_t res) {
                   REQUIRE(r.has_value());
                   CHECK(res == 4);
                   received = to_string(*r);
                 });
  reactor.submit(ringu::Send{ringu::FileDescriptor{sv[1]}}, to_bytes("ping"),
                 [&](ringu::Result<std::vector<std::byte>> r, ringu::OperationId, std::int32_t res) {
                   REQUIRE(r.has_value());
                   sent = res;
                 });
  reactor.process(Mode::Batch);
  CHECK(sent == 4);
  CHECK(received == "ping");
  CHECK(reactor.count_operations(ringu::Reactor::OperationState::Completed) == 2);
  ::close(sv[0]);
  ::close(sv[1]);
}

TEST_CASE("listen then accept over localhost TCP") {
  ringu::Reactor reactor{64};
  using Mode = ringu::Reactor::ProcessMode;
  using State = ringu::Reactor::OperationState;

  const std::int32_t lfd = static_cast<std::int32_t>(::socket(AF_INET, SOCK_STREAM, 0));
  REQUIRE(lfd >= 0);
  const std::int32_t one = 1;
  ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  // bind through the reactor (inline Bind operation)
  bool bound = false;
  reactor.submit(ringu::Bind::make(ringu::FileDescriptor{lfd}, ringu::Ipv4Endpoint::loopback(0)),
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

  // listen through the reactor (executed inline when the batch is processed)
  bool listened = false;
  reactor.submit(ringu::Listen::make(ringu::FileDescriptor{lfd}, 16),
                 [&](ringu::Result<std::int32_t> r, ringu::OperationId, std::int32_t res) {
                   REQUIRE(r.has_value());
                   CHECK(res == 0);
                   listened = true;
                 });
  reactor.process(Mode::Batch);
  CHECK(listened);

  // connect a client with a plain blocking connect
  const std::int32_t cfd = static_cast<std::int32_t>(::socket(AF_INET, SOCK_STREAM, 0));
  REQUIRE(cfd >= 0);
  REQUIRE(::connect(cfd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);

  // accept through the reactor, with a buffer for the peer address
  ringu::FileDescriptor accepted{-1};
  reactor.submit(ringu::Accept::make(ringu::FileDescriptor{lfd}), std::vector<std::byte>(sizeof(sockaddr_storage)),
                 [&](ringu::Result<ringu::FileDescriptor> r, ringu::OperationId, std::int32_t res) {
                   REQUIRE(r.has_value());
                   CHECK(res >= 0);
                   accepted = *r;
                 });
  reactor.process(Mode::Batch);
  REQUIRE(accepted.value() >= 0);

  // verify the accepted socket is connected to the loopback client
  sockaddr_in peer{};
  socklen_t plen = sizeof(peer);
  REQUIRE(::getpeername(accepted.value(), reinterpret_cast<sockaddr*>(&peer), &plen) == 0);
  CHECK(peer.sin_family == AF_INET);
  CHECK(peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK));

  // exchange data over the accepted connection through the reactor
  std::string received;
  reactor.submit(ringu::Recv::make(accepted), std::vector<std::byte>(16),
                 [&](ringu::Result<std::vector<std::byte>> r, ringu::OperationId, std::int32_t) {
                   REQUIRE(r.has_value());
                   received = to_string(*r);
                 });
  reactor.submit(ringu::Send::make(ringu::FileDescriptor{cfd}), to_bytes("hello"),
                 [](ringu::Result<std::vector<std::byte>> r, ringu::OperationId, std::int32_t res) {
                   REQUIRE(r.has_value());
                   CHECK(res == 5);
                 });
  reactor.process(Mode::Batch);
  CHECK(received == "hello");

  reactor.submit(ringu::Close{accepted}, [](ringu::Result<std::int32_t>, ringu::OperationId, std::int32_t) {});
  reactor.process(Mode::Batch);
  CHECK(reactor.count_operations(State::Failed) == 0);
  ::close(cfd);
  ::close(lfd);
}

TEST_CASE("listen on a non-socket fd fails with ENOTSOCK") {
  ringu::Reactor reactor{64};
  using State = ringu::Reactor::OperationState;
  std::int32_t fds[2];
  REQUIRE(::pipe(reinterpret_cast<int*>(fds)) == 0);
  bool called = false;
  reactor.submit(ringu::Listen{ringu::FileDescriptor{fds[0]}, 4},
                 [&](ringu::Result<std::int32_t> r, ringu::OperationId, std::int32_t) {
                   called = true;
                   CHECK_FALSE(r.has_value());
                   REQUIRE(std::holds_alternative<ringu::IoError>(r.error()));
                   CHECK(std::get<ringu::IoError>(r.error()).value == -ENOTSOCK);
                 });
  reactor.process(ringu::Reactor::ProcessMode::Batch);
  CHECK(called);
  CHECK(reactor.count_operations(State::Failed) == 1);
  ::close(fds[0]);
  ::close(fds[1]);
}

TEST_CASE("accept on a non-listening socket fails") {
  ringu::Reactor reactor{64};
  using State = ringu::Reactor::OperationState;
  const std::int32_t fd = static_cast<std::int32_t>(::socket(AF_INET, SOCK_STREAM, 0));
  REQUIRE(fd >= 0);
  bool called = false;
  reactor.submit(ringu::Accept::make(ringu::FileDescriptor{fd}),
                 [&](ringu::Result<ringu::FileDescriptor> r, ringu::OperationId, std::int32_t) {
                   called = true;
                   CHECK_FALSE(r.has_value());
                   CHECK(std::holds_alternative<ringu::IoError>(r.error()));
                 });
  reactor.process(ringu::Reactor::ProcessMode::Batch);
  CHECK(called);
  CHECK(reactor.count_operations(State::Failed) == 1);
  ::close(fd);
}
