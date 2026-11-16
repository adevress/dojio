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

TEST_CASE("Ipv6Endpoint parse and loopback") {
  const auto ep = ringu::Ipv6Endpoint::parse("::1", 8080);
  REQUIRE(ep.has_value());
  CHECK(ep->port == 8080);
  for (std::size_t i = 0; i < 15; ++i) {
    CHECK(ep->address[i] == 0);
  }
  CHECK(ep->address[15] == 1);
  CHECK(ringu::Ipv6Endpoint::loopback(1234).address == ep->address);
  CHECK(ringu::Ipv6Endpoint::loopback(1234).port == 1234);
  CHECK_FALSE(ringu::Ipv6Endpoint::parse("not_an_ip", 80).has_value());
  const auto full = ringu::Ipv6Endpoint::parse("2001:db8::1", 443);
  REQUIRE(full.has_value());
  CHECK(full->address[0] == 0x20);
  CHECK(full->address[1] == 0x01);
  CHECK(full->address[2] == 0x0d);
  CHECK(full->address[3] == 0xb8);
  CHECK(full->address[15] == 1);
  CHECK(full->port == 443);
}

TEST_CASE("Connect operation connects to a listening localhost server") {
  for (const bool sqpoll : {true, false}) {
    CAPTURE(sqpoll);
    auto reactor_result = ringu::Reactor::Create(ringu::ReactorOptions{.entries = 64, .sqpoll = sqpoll});
    REQUIRE(reactor_result.has_value());
    auto reactor = std::move(reactor_result).value();
    CHECK(reactor->sqpoll_enabled() == sqpoll);
    using Mode = ringu::Reactor::ProcessMode;

    auto server =
        ringu::create_socket(ringu::AddressFamily::Inet, ringu::SocketType::Stream, ringu::SocketProtocol::Tcp);
    REQUIRE(server.has_value());
    const std::int32_t lfd = server->value();
    const std::int32_t one = 1;
    ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    // bind through the reactor (inline Bind operation)
    bool bound = false;
    reactor->submit(ringu::bind(ringu::FileDescriptor{lfd}, ringu::Ipv4Endpoint::loopback(0)),
                    [&](ringu::Result<std::int32_t> r, ringu::OperationId, std::int32_t res) {
                      REQUIRE(r.has_value());
                      CHECK(res == 0);
                      bound = true;
                    });
    reactor->process(Mode::Batch);
    CHECK(bound);

    sockaddr_in addr{};
    socklen_t alen = sizeof(addr);
    REQUIRE(::getsockname(lfd, reinterpret_cast<sockaddr*>(&addr), &alen) == 0);
    const auto port = static_cast<std::uint16_t>(ntohs(addr.sin_port));

    reactor->submit(ringu::listen(ringu::FileDescriptor{lfd}, 16),
                    [](ringu::Result<std::int32_t>, ringu::OperationId, std::int32_t) {});
    reactor->process(Mode::Batch);

    auto client =
        ringu::create_socket(ringu::AddressFamily::Inet, ringu::SocketType::Stream, ringu::SocketProtocol::Tcp);
    REQUIRE(client.has_value());
    const std::int32_t cfd = client->value();

    bool connected = false;
    reactor->submit(ringu::connect(ringu::FileDescriptor{cfd}, ringu::Ipv4Endpoint::loopback(port)),
                    [&](ringu::Result<std::int32_t> r, ringu::OperationId, std::int32_t res) {
                      REQUIRE(r.has_value());
                      CHECK(res == 0);
                      connected = true;
                    });
    reactor->process(Mode::Batch);
    CHECK(connected);

    // the server side accepts the connection
    ringu::FileDescriptor accepted{-1};
    reactor->submit(ringu::accept(ringu::FileDescriptor{lfd}), ringu::AcceptBuffer{},
                    [&](ringu::Result<ringu::FileDescriptor> r, ringu::OperationId, std::int32_t res) {
                      REQUIRE(r.has_value());
                      CHECK(res >= 0);
                      accepted = *r;
                    });
    reactor->process(Mode::Batch);
    REQUIRE(accepted.value() >= 0);

    // data flows over the ring-connected socket
    std::string received;
    reactor->submit(ringu::recv(accepted), std::vector<std::byte>(16),
                    [&](ringu::Result<std::vector<std::byte>> r, ringu::OperationId, std::int32_t) {
                      REQUIRE(r.has_value());
                      received = to_string(*r);
                    });
    reactor->submit(ringu::send(ringu::FileDescriptor{cfd}), to_bytes("ring-connect"),
                    [](ringu::Result<std::vector<std::byte>> r, ringu::OperationId, std::int32_t res) {
                      REQUIRE(r.has_value());
                      CHECK(res == 12);
                    });
    reactor->process(Mode::Batch);
    CHECK(received == "ring-connect");

    reactor->submit(ringu::close(accepted), [](ringu::Result<std::int32_t>, ringu::OperationId, std::int32_t) {});
    reactor->submit(ringu::close(ringu::FileDescriptor{cfd}),
                    [](ringu::Result<std::int32_t>, ringu::OperationId, std::int32_t) {});
    reactor->process(Mode::Batch);
    ::close(lfd);
  }
}

TEST_CASE("Connect to a closed port fails with ECONNREFUSED") {
  for (const bool sqpoll : {true, false}) {
    CAPTURE(sqpoll);
    auto reactor_result = ringu::Reactor::Create(ringu::ReactorOptions{.entries = 64, .sqpoll = sqpoll});
    REQUIRE(reactor_result.has_value());
    auto reactor = std::move(reactor_result).value();
    CHECK(reactor->sqpoll_enabled() == sqpoll);
    auto client =
        ringu::create_socket(ringu::AddressFamily::Inet, ringu::SocketType::Stream, ringu::SocketProtocol::Tcp);
    REQUIRE(client.has_value());
    // port 1 on loopback is not listening
    bool called = false;
    reactor->submit(ringu::connect(*client, ringu::Ipv4Endpoint::loopback(1)),
                    [&](ringu::Result<std::int32_t> r, ringu::OperationId, std::int32_t) {
                      called = true;
                      CHECK_FALSE(r.has_value());
                      REQUIRE(std::holds_alternative<ringu::IoError>(r.error()));
                      CHECK(std::get<ringu::IoError>(r.error()).value == -ECONNREFUSED);
                    });
    reactor->process(ringu::Reactor::ProcessMode::Batch);
    CHECK(called);
    ::close(client->value());
  }
}

TEST_CASE("Fsync operation syncs a file") {
  for (const bool sqpoll : {true, false}) {
    CAPTURE(sqpoll);
    auto reactor_result = ringu::Reactor::Create(ringu::ReactorOptions{.entries = 64, .sqpoll = sqpoll});
    REQUIRE(reactor_result.has_value());
    auto reactor = std::move(reactor_result).value();
    CHECK(reactor->sqpoll_enabled() == sqpoll);
    using Mode = ringu::Reactor::ProcessMode;
    const std::string path = (std::filesystem::temp_directory_path() / "ringu_fsync_test").string();
    const std::int32_t fd = static_cast<std::int32_t>(::open(path.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644));
    REQUIRE(fd >= 0);
    REQUIRE(::write(fd, "data", 4) == 4);

    bool synced = false;
    reactor->submit(ringu::fsync(ringu::FileDescriptor{fd}),
                    [&](ringu::Result<std::int32_t> r, ringu::OperationId, std::int32_t res) {
                      REQUIRE(r.has_value());
                      CHECK(res == 0);
                      synced = true;
                    });
    reactor->process(Mode::Batch);
    CHECK(synced);

    bool datasync_ok = false;
    reactor->submit(ringu::fsync(ringu::FileDescriptor{fd}, true),
                    [&](ringu::Result<std::int32_t> r, ringu::OperationId, std::int32_t res) {
                      REQUIRE(r.has_value());
                      CHECK(res == 0);
                      datasync_ok = true;
                    });
    reactor->process(Mode::Batch);
    CHECK(datasync_ok);

    reactor->submit(ringu::close(ringu::FileDescriptor{fd}),
                    [](ringu::Result<std::int32_t>, ringu::OperationId, std::int32_t) {});
    reactor->process(Mode::Batch);
    ::unlink(path.c_str());
  }
}

TEST_CASE("OpenAt operation opens a file relative to a dirfd") {
  for (const bool sqpoll : {true, false}) {
    CAPTURE(sqpoll);
    auto reactor_result = ringu::Reactor::Create(ringu::ReactorOptions{.entries = 64, .sqpoll = sqpoll});
    REQUIRE(reactor_result.has_value());
    auto reactor = std::move(reactor_result).value();
    CHECK(reactor->sqpoll_enabled() == sqpoll);
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
    reactor->submit(ringu::open_at(ringu::FileDescriptor{dirfd}, name, O_RDONLY),
                    [&](ringu::Result<ringu::FileDescriptor> r, ringu::OperationId, std::int32_t res) {
                      REQUIRE(r.has_value());
                      CHECK(res >= 0);
                      fd = *r;
                    });
    reactor->process(Mode::Batch);
    REQUIRE(fd.value() >= 0);

    std::string content;
    reactor->submit(ringu::pread(fd, 0), std::vector<std::byte>(64),
                    [&](ringu::Result<std::vector<std::byte>> r, ringu::OperationId, std::int32_t res) {
                      REQUIRE(r.has_value());
                      CHECK(res == 14);
                      content = to_string(*r);
                    });
    reactor->process(Mode::Batch);
    CHECK(content == "openat-content");

    reactor->submit(ringu::close(fd), [](ringu::Result<std::int32_t>, ringu::OperationId, std::int32_t) {});
    reactor->process(Mode::Batch);
    ::close(dirfd);
    ::unlink(full.c_str());
  }
}

TEST_CASE("Bind operation binds a socket to an IPv4 endpoint") {
  for (const bool sqpoll : {true, false}) {
    CAPTURE(sqpoll);
    auto reactor_result = ringu::Reactor::Create(ringu::ReactorOptions{.entries = 64, .sqpoll = sqpoll});
    REQUIRE(reactor_result.has_value());
    auto reactor = std::move(reactor_result).value();
    CHECK(reactor->sqpoll_enabled() == sqpoll);
    using Mode = ringu::Reactor::ProcessMode;

    auto sock = ringu::create_socket(ringu::AddressFamily::Inet, ringu::SocketType::Stream, ringu::SocketProtocol::Tcp);
    REQUIRE(sock.has_value());
    const ringu::FileDescriptor fd = *sock;

    bool bound = false;
    reactor->submit(ringu::bind(fd, ringu::Ipv4Endpoint::loopback(0)),
                    [&](ringu::Result<std::int32_t> r, ringu::OperationId, std::int32_t res) {
                      REQUIRE(r.has_value());
                      CHECK(res == 0);
                      bound = true;
                    });
    reactor->process(Mode::Batch);
    CHECK(bound);

    // the kernel assigned an ephemeral port on 127.0.0.1
    sockaddr_in addr{};
    socklen_t alen = sizeof(addr);
    REQUIRE(::getsockname(fd.value(), reinterpret_cast<sockaddr*>(&addr), &alen) == 0);
    CHECK(addr.sin_family == AF_INET);
    CHECK(addr.sin_addr.s_addr == htonl(INADDR_LOOPBACK));
    CHECK(ntohs(addr.sin_port) != 0);

    // another socket binding the same endpoint fails with EADDRINUSE
    auto sock2 =
        ringu::create_socket(ringu::AddressFamily::Inet, ringu::SocketType::Stream, ringu::SocketProtocol::Tcp);
    REQUIRE(sock2.has_value());
    bool failed = false;
    reactor->submit(ringu::bind(*sock2, ringu::Ipv4Endpoint::loopback(ntohs(addr.sin_port))),
                    [&](ringu::Result<std::int32_t> r, ringu::OperationId, std::int32_t) {
                      CHECK_FALSE(r.has_value());
                      REQUIRE(std::holds_alternative<ringu::IoError>(r.error()));
                      CHECK(std::get<ringu::IoError>(r.error()).value == -EADDRINUSE);
                      failed = true;
                    });
    reactor->process(Mode::Batch);
    CHECK(failed);

    reactor->submit(ringu::close(*sock2), [](ringu::Result<std::int32_t>, ringu::OperationId, std::int32_t) {});
    reactor->submit(ringu::close(fd), [](ringu::Result<std::int32_t>, ringu::OperationId, std::int32_t) {});
    reactor->process(Mode::Batch);
  }
}

TEST_CASE("Bind and Listen operations work over IPv6") {
  for (const bool sqpoll : {true, false}) {
    CAPTURE(sqpoll);
    auto reactor_result = ringu::Reactor::Create(ringu::ReactorOptions{.entries = 64, .sqpoll = sqpoll});
    REQUIRE(reactor_result.has_value());
    auto reactor = std::move(reactor_result).value();
    CHECK(reactor->sqpoll_enabled() == sqpoll);
    using Mode = ringu::Reactor::ProcessMode;

    auto sock =
        ringu::create_socket(ringu::AddressFamily::Inet6, ringu::SocketType::Stream, ringu::SocketProtocol::Tcp);
    REQUIRE(sock.has_value());
    const ringu::FileDescriptor fd = *sock;

    bool bound = false;
    reactor->submit(ringu::bind(fd, ringu::Ipv6Endpoint::loopback(0)),
                    [&](ringu::Result<std::int32_t> r, ringu::OperationId, std::int32_t res) {
                      REQUIRE(r.has_value());
                      CHECK(res == 0);
                      bound = true;
                    });
    reactor->process(Mode::Batch);
    CHECK(bound);

    // the kernel assigned an ephemeral port on [::1]
    sockaddr_in6 addr{};
    socklen_t alen = sizeof(addr);
    REQUIRE(::getsockname(fd.value(), reinterpret_cast<sockaddr*>(&addr), &alen) == 0);
    CHECK(addr.sin6_family == AF_INET6);
    CHECK(addr.sin6_addr.s6_addr[15] == 1);
    CHECK(ntohs(addr.sin6_port) != 0);

    bool listened = false;
    reactor->submit(ringu::listen(fd, 16), [&](ringu::Result<std::int32_t> r, ringu::OperationId, std::int32_t res) {
      REQUIRE(r.has_value());
      CHECK(res == 0);
      listened = true;
    });
    reactor->process(Mode::Batch);
    CHECK(listened);

    // raw blocking connect over IPv6, then accept through the reactor
    const std::int32_t cfd = static_cast<std::int32_t>(::socket(AF_INET6, SOCK_STREAM, 0));
    REQUIRE(cfd >= 0);
    sockaddr_in6 caddr{};
    caddr.sin6_family = AF_INET6;
    caddr.sin6_addr = addr.sin6_addr;
    caddr.sin6_port = addr.sin6_port;
    REQUIRE(::connect(cfd, reinterpret_cast<const sockaddr*>(&caddr), sizeof(caddr)) == 0);

    ringu::FileDescriptor accepted{-1};
    reactor->submit(ringu::accept(fd), ringu::AcceptBuffer{},
                    [&](ringu::Result<ringu::FileDescriptor> r, ringu::OperationId, std::int32_t res) {
                      REQUIRE(r.has_value());
                      CHECK(res >= 0);
                      accepted = *r;
                    });
    reactor->process(Mode::Batch);
    REQUIRE(accepted.value() >= 0);

    sockaddr_in6 peer{};
    socklen_t plen = sizeof(peer);
    REQUIRE(::getpeername(accepted.value(), reinterpret_cast<sockaddr*>(&peer), &plen) == 0);
    CHECK(peer.sin6_family == AF_INET6);
    CHECK(peer.sin6_addr.s6_addr[15] == 1);

    reactor->submit(ringu::close(accepted), [](ringu::Result<std::int32_t>, ringu::OperationId, std::int32_t) {});
    reactor->submit(ringu::close(fd), [](ringu::Result<std::int32_t>, ringu::OperationId, std::int32_t) {});
    reactor->process(Mode::Batch);
    ::close(cfd);
  }
}
