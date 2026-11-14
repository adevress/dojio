#include <doctest/doctest.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include <ringu/ringu.hpp>

namespace {

std::vector<std::byte> to_bytes(std::string_view s) {
  return {reinterpret_cast<const std::byte*>(s.data()), reinterpret_cast<const std::byte*>(s.data()) + s.size()};
}

std::string to_string(const std::vector<std::byte>& b) { return {reinterpret_cast<const char*>(b.data()), b.size()}; }

} // namespace

TEST_CASE("create_socket creates sockets without exposing C flags") {
  auto tcp = ringu::create_socket(ringu::AddressFamily::Inet, ringu::SocketType::Stream, ringu::SocketProtocol::Tcp);
  REQUIRE(tcp.has_value());
  CHECK(tcp->value() >= 0);
  ::close(tcp->value());

  auto udp = ringu::create_socket(ringu::AddressFamily::Inet, ringu::SocketType::Dgram, ringu::SocketProtocol::Udp);
  REQUIRE(udp.has_value());
  ::close(udp->value());

  auto ux = ringu::create_socket(ringu::AddressFamily::Unix, ringu::SocketType::Stream);
  REQUIRE(ux.has_value());
  ::close(ux->value());

  auto flagged =
      ringu::create_socket(ringu::AddressFamily::Inet, ringu::SocketType::Stream, ringu::SocketProtocol::Default,
                           ringu::SocketFlag::NonBlock | ringu::SocketFlag::CloExec);
  REQUIRE(flagged.has_value());
  ::close(flagged->value());
}

TEST_CASE("create_socket with an invalid family is a contract failure") {
  CHECK_THROWS_AS(ringu::create_socket(static_cast<ringu::AddressFamily>(99), ringu::SocketType::Stream),
                  std::logic_error);
}

TEST_CASE("Ipv4Endpoint parse and loopback") {
  const auto ep = ringu::Ipv4Endpoint::parse("127.0.0.1", 8080);
  REQUIRE(ep.has_value());
  CHECK(ep->address == 0x7F000001u);
  CHECK(ep->port == 8080);
  CHECK(ringu::Ipv4Endpoint::loopback(1234).address == 0x7F000001u);
  CHECK(ringu::Ipv4Endpoint::loopback(1234).port == 1234);
  CHECK_FALSE(ringu::Ipv4Endpoint::parse("not_an_ip", 80).has_value());
}

TEST_CASE("Connect operation connects to a listening localhost server") {
  ringu::Reactor reactor{64};
  using Mode = ringu::Reactor::ProcessMode;

  auto server = ringu::create_socket(ringu::AddressFamily::Inet, ringu::SocketType::Stream, ringu::SocketProtocol::Tcp);
  REQUIRE(server.has_value());
  const std::int32_t lfd = server->value();
  const std::int32_t one = 1;
  ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  REQUIRE(::bind(lfd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);
  socklen_t alen = sizeof(addr);
  REQUIRE(::getsockname(lfd, reinterpret_cast<sockaddr*>(&addr), &alen) == 0);
  const auto port = static_cast<std::uint16_t>(ntohs(addr.sin_port));

  reactor.submit(ringu::Listen::make(ringu::FileDescriptor{lfd}, 16),
                 [](ringu::Result<std::int32_t>, ringu::OperationId, std::int32_t) {});
  reactor.process(Mode::Batch);

  auto client = ringu::create_socket(ringu::AddressFamily::Inet, ringu::SocketType::Stream, ringu::SocketProtocol::Tcp);
  REQUIRE(client.has_value());
  const std::int32_t cfd = client->value();

  bool connected = false;
  reactor.submit(ringu::Connect::make(ringu::FileDescriptor{cfd}, ringu::Ipv4Endpoint::loopback(port)),
                 [&](ringu::Result<std::int32_t> r, ringu::OperationId, std::int32_t res) {
                   REQUIRE(r.has_value());
                   CHECK(res == 0);
                   connected = true;
                 });
  reactor.process(Mode::Batch);
  CHECK(connected);

  // the server side accepts the connection
  ringu::FileDescriptor accepted{-1};
  reactor.submit(ringu::Accept::make(ringu::FileDescriptor{lfd}), ringu::AcceptBuffer{},
                 [&](ringu::Result<ringu::FileDescriptor> r, ringu::OperationId, std::int32_t res) {
                   REQUIRE(r.has_value());
                   CHECK(res >= 0);
                   accepted = *r;
                 });
  reactor.process(Mode::Batch);
  REQUIRE(accepted.value() >= 0);

  // data flows over the ring-connected socket
  std::string received;
  reactor.submit(ringu::Recv::make(accepted), std::vector<std::byte>(16),
                 [&](ringu::Result<std::vector<std::byte>> r, ringu::OperationId, std::int32_t) {
                   REQUIRE(r.has_value());
                   received = to_string(*r);
                 });
  reactor.submit(ringu::Send::make(ringu::FileDescriptor{cfd}), to_bytes("ring-connect"),
                 [](ringu::Result<std::vector<std::byte>> r, ringu::OperationId, std::int32_t res) {
                   REQUIRE(r.has_value());
                   CHECK(res == 12);
                 });
  reactor.process(Mode::Batch);
  CHECK(received == "ring-connect");

  reactor.submit(ringu::Close::make(accepted), [](ringu::Result<std::int32_t>, ringu::OperationId, std::int32_t) {});
  reactor.submit(ringu::Close::make(ringu::FileDescriptor{cfd}),
                 [](ringu::Result<std::int32_t>, ringu::OperationId, std::int32_t) {});
  reactor.process(Mode::Batch);
  ::close(lfd);
}

TEST_CASE("Connect to a closed port fails with ECONNREFUSED") {
  ringu::Reactor reactor{64};
  auto client = ringu::create_socket(ringu::AddressFamily::Inet, ringu::SocketType::Stream, ringu::SocketProtocol::Tcp);
  REQUIRE(client.has_value());
  // port 1 on loopback is not listening
  bool called = false;
  reactor.submit(ringu::Connect::make(*client, ringu::Ipv4Endpoint::loopback(1)),
                 [&](ringu::Result<std::int32_t> r, ringu::OperationId, std::int32_t) {
                   called = true;
                   CHECK_FALSE(r.has_value());
                   REQUIRE(std::holds_alternative<ringu::IoError>(r.error()));
                   CHECK(std::get<ringu::IoError>(r.error()).value == -ECONNREFUSED);
                 });
  reactor.process(ringu::Reactor::ProcessMode::Batch);
  CHECK(called);
  ::close(client->value());
}

TEST_CASE("Fsync operation syncs a file") {
  ringu::Reactor reactor{64};
  using Mode = ringu::Reactor::ProcessMode;
  const std::string path = (std::filesystem::temp_directory_path() / "ringu_fsync_test").string();
  const std::int32_t fd = static_cast<std::int32_t>(::open(path.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644));
  REQUIRE(fd >= 0);
  REQUIRE(::write(fd, "data", 4) == 4);

  bool synced = false;
  reactor.submit(ringu::Fsync::make(ringu::FileDescriptor{fd}),
                 [&](ringu::Result<std::int32_t> r, ringu::OperationId, std::int32_t res) {
                   REQUIRE(r.has_value());
                   CHECK(res == 0);
                   synced = true;
                 });
  reactor.process(Mode::Batch);
  CHECK(synced);

  bool datasync_ok = false;
  reactor.submit(ringu::Fsync::make(ringu::FileDescriptor{fd}, true),
                 [&](ringu::Result<std::int32_t> r, ringu::OperationId, std::int32_t res) {
                   REQUIRE(r.has_value());
                   CHECK(res == 0);
                   datasync_ok = true;
                 });
  reactor.process(Mode::Batch);
  CHECK(datasync_ok);

  reactor.submit(ringu::Close::make(ringu::FileDescriptor{fd}),
                 [](ringu::Result<std::int32_t>, ringu::OperationId, std::int32_t) {});
  reactor.process(Mode::Batch);
  ::unlink(path.c_str());
}

TEST_CASE("OpenAt operation opens a file relative to a dirfd") {
  ringu::Reactor reactor{64};
  using Mode = ringu::Reactor::ProcessMode;
  const auto dir = std::filesystem::temp_directory_path();
  const std::string name = "ringu_openat_test_file";
  const std::string full = (dir / name).string();
  {
    const std::int32_t fd = static_cast<std::int32_t>(::open(full.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644));
    REQUIRE(fd >= 0);
    REQUIRE(::write(fd, "openat-content", 14) == 14);
    ::close(fd);
  }
  const std::int32_t dirfd = static_cast<std::int32_t>(::open(dir.c_str(), O_RDONLY | O_DIRECTORY));
  REQUIRE(dirfd >= 0);

  ringu::FileDescriptor fd{-1};
  reactor.submit(ringu::OpenAt::make(ringu::FileDescriptor{dirfd}, name, O_RDONLY),
                 [&](ringu::Result<ringu::FileDescriptor> r, ringu::OperationId, std::int32_t res) {
                   REQUIRE(r.has_value());
                   CHECK(res >= 0);
                   fd = *r;
                 });
  reactor.process(Mode::Batch);
  REQUIRE(fd.value() >= 0);

  std::string content;
  reactor.submit(ringu::Pread::make(fd, 0), std::vector<std::byte>(64),
                 [&](ringu::Result<std::vector<std::byte>> r, ringu::OperationId, std::int32_t res) {
                   REQUIRE(r.has_value());
                   CHECK(res == 14);
                   content = to_string(*r);
                 });
  reactor.process(Mode::Batch);
  CHECK(content == "openat-content");

  reactor.submit(ringu::Close::make(fd), [](ringu::Result<std::int32_t>, ringu::OperationId, std::int32_t) {});
  reactor.process(Mode::Batch);
  ::close(dirfd);
  ::unlink(full.c_str());
}
