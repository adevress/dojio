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

#include <dojio/dojio.hpp>

namespace {

std::vector<std::byte> to_bytes(std::string_view s) {
  return {reinterpret_cast<const std::byte*>(s.data()), reinterpret_cast<const std::byte*>(s.data()) + s.size()};
}

std::string to_string(const std::vector<std::byte>& b) { return {reinterpret_cast<const char*>(b.data()), b.size()}; }

} // namespace

TEST_CASE("create_socket creates sockets without exposing C flags") {
  auto tcp = dojio::create_socket(dojio::AddressFamily::Inet, dojio::SocketType::Stream, dojio::SocketProtocol::Tcp);
  REQUIRE(tcp.has_value());
  CHECK(tcp->value() >= 0);
  ::close(tcp->value());

  auto udp = dojio::create_socket(dojio::AddressFamily::Inet, dojio::SocketType::Dgram, dojio::SocketProtocol::Udp);
  REQUIRE(udp.has_value());
  ::close(udp->value());

  auto ux = dojio::create_socket(dojio::AddressFamily::Unix, dojio::SocketType::Stream);
  REQUIRE(ux.has_value());
  ::close(ux->value());

  auto flagged =
      dojio::create_socket(dojio::AddressFamily::Inet, dojio::SocketType::Stream, dojio::SocketProtocol::Default,
                           dojio::SocketFlag::NonBlock | dojio::SocketFlag::CloExec);
  REQUIRE(flagged.has_value());
  ::close(flagged->value());
}

TEST_CASE("create_socket with an invalid family is a contract failure") {
  CHECK_THROWS_AS(dojio::create_socket(static_cast<dojio::AddressFamily>(99), dojio::SocketType::Stream),
                  std::logic_error);
}

TEST_CASE("Ipv4Endpoint parse and loopback") {
  const auto ep = dojio::Ipv4Endpoint::parse("127.0.0.1", 8080);
  REQUIRE(ep.has_value());
  CHECK(ep->address == 0x7F000001u);
  CHECK(ep->port == 8080);
  CHECK(dojio::Ipv4Endpoint::loopback(1234).address == 0x7F000001u);
  CHECK(dojio::Ipv4Endpoint::loopback(1234).port == 1234);
  CHECK_FALSE(dojio::Ipv4Endpoint::parse("not_an_ip", 80).has_value());
}

TEST_CASE("Ipv6Endpoint parse and loopback") {
  const auto ep = dojio::Ipv6Endpoint::parse("::1", 8080);
  REQUIRE(ep.has_value());
  CHECK(ep->port == 8080);
  for (std::size_t i = 0; i < 15; ++i) {
    CHECK(ep->address[i] == 0);
  }
  CHECK(ep->address[15] == 1);
  CHECK(dojio::Ipv6Endpoint::loopback(1234).address == ep->address);
  CHECK(dojio::Ipv6Endpoint::loopback(1234).port == 1234);
  CHECK_FALSE(dojio::Ipv6Endpoint::parse("not_an_ip", 80).has_value());
  const auto full = dojio::Ipv6Endpoint::parse("2001:db8::1", 443);
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
    auto ring_engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 64, .sqpoll = sqpoll});
    REQUIRE(ring_engine_result.has_value());
    auto engine = std::move(ring_engine_result).value();
    CHECK(engine->sqpoll_enabled() == sqpoll);
    using Mode = dojio::RingEngine::ProcessMode;

    auto server =
        dojio::create_socket(dojio::AddressFamily::Inet, dojio::SocketType::Stream, dojio::SocketProtocol::Tcp);
    REQUIRE(server.has_value());
    const std::int32_t lfd = server->value();
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
    const auto port = static_cast<std::uint16_t>(ntohs(addr.sin_port));

    engine->submit(dojio::listen(dojio::FileDescriptor{lfd}, 16),
                   [](dojio::Result<std::int32_t>, dojio::OperationId, std::int32_t) {});
    engine->process(Mode::Batch);

    auto client =
        dojio::create_socket(dojio::AddressFamily::Inet, dojio::SocketType::Stream, dojio::SocketProtocol::Tcp);
    REQUIRE(client.has_value());
    const std::int32_t cfd = client->value();

    bool connected = false;
    engine->submit(dojio::connect(dojio::FileDescriptor{cfd}, dojio::Ipv4Endpoint::loopback(port)),
                   [&](dojio::Result<std::int32_t> r, dojio::OperationId, std::int32_t res) {
                     REQUIRE(r.has_value());
                     CHECK(res == 0);
                     connected = true;
                   });
    engine->process(Mode::Batch);
    CHECK(connected);

    // the server side accepts the connection
    dojio::FileDescriptor accepted{-1};
    engine->submit(dojio::accept(dojio::FileDescriptor{lfd}), dojio::AcceptBuffer{},
                   [&](dojio::Result<dojio::FileDescriptor> r, dojio::OperationId, std::int32_t res) {
                     REQUIRE(r.has_value());
                     CHECK(res >= 0);
                     accepted = *r;
                   });
    engine->process(Mode::Batch);
    REQUIRE(accepted.value() >= 0);

    // data flows over the ring-connected socket
    std::string received;
    engine->submit(dojio::recv(accepted), std::vector<std::byte>(16),
                   [&](dojio::Result<std::vector<std::byte>> r, dojio::OperationId, std::int32_t) {
                     REQUIRE(r.has_value());
                     received = to_string(*r);
                   });
    engine->submit(dojio::send(dojio::FileDescriptor{cfd}), to_bytes("ring-connect"),
                   [](dojio::Result<std::vector<std::byte>> r, dojio::OperationId, std::int32_t res) {
                     REQUIRE(r.has_value());
                     CHECK(res == 12);
                   });
    engine->process(Mode::Batch);
    CHECK(received == "ring-connect");

    engine->submit(dojio::close(accepted), [](dojio::Result<std::int32_t>, dojio::OperationId, std::int32_t) {});
    engine->submit(dojio::close(dojio::FileDescriptor{cfd}),
                   [](dojio::Result<std::int32_t>, dojio::OperationId, std::int32_t) {});
    engine->process(Mode::Batch);
    ::close(lfd);
  }
}

TEST_CASE("Connect to a closed port fails with ECONNREFUSED") {
  for (const bool sqpoll : {true, false}) {
    CAPTURE(sqpoll);
    auto ring_engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 64, .sqpoll = sqpoll});
    REQUIRE(ring_engine_result.has_value());
    auto engine = std::move(ring_engine_result).value();
    CHECK(engine->sqpoll_enabled() == sqpoll);
    auto client =
        dojio::create_socket(dojio::AddressFamily::Inet, dojio::SocketType::Stream, dojio::SocketProtocol::Tcp);
    REQUIRE(client.has_value());
    // port 1 on loopback is not listening
    bool called = false;
    engine->submit(dojio::connect(*client, dojio::Ipv4Endpoint::loopback(1)),
                   [&](dojio::Result<std::int32_t> r, dojio::OperationId, std::int32_t) {
                     called = true;
                     CHECK_FALSE(r.has_value());
                     REQUIRE(std::holds_alternative<dojio::IoError>(r.error()));
                     CHECK(std::get<dojio::IoError>(r.error()).value == -ECONNREFUSED);
                   });
    engine->process(dojio::RingEngine::ProcessMode::Batch);
    CHECK(called);
    ::close(client->value());
  }
}

TEST_CASE("Fsync operation syncs a file") {
  for (const bool sqpoll : {true, false}) {
    CAPTURE(sqpoll);
    auto ring_engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 64, .sqpoll = sqpoll});
    REQUIRE(ring_engine_result.has_value());
    auto engine = std::move(ring_engine_result).value();
    CHECK(engine->sqpoll_enabled() == sqpoll);
    using Mode = dojio::RingEngine::ProcessMode;
    const std::string path = (std::filesystem::temp_directory_path() / "dojio_fsync_test").string();
    const std::int32_t fd = static_cast<std::int32_t>(::open(path.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644));
    REQUIRE(fd >= 0);
    REQUIRE(::write(fd, "data", 4) == 4);

    bool synced = false;
    engine->submit(dojio::fsync(dojio::FileDescriptor{fd}),
                   [&](dojio::Result<std::int32_t> r, dojio::OperationId, std::int32_t res) {
                     REQUIRE(r.has_value());
                     CHECK(res == 0);
                     synced = true;
                   });
    engine->process(Mode::Batch);
    CHECK(synced);

    bool datasync_ok = false;
    engine->submit(dojio::fsync(dojio::FileDescriptor{fd}, true),
                   [&](dojio::Result<std::int32_t> r, dojio::OperationId, std::int32_t res) {
                     REQUIRE(r.has_value());
                     CHECK(res == 0);
                     datasync_ok = true;
                   });
    engine->process(Mode::Batch);
    CHECK(datasync_ok);

    engine->submit(dojio::close(dojio::FileDescriptor{fd}),
                   [](dojio::Result<std::int32_t>, dojio::OperationId, std::int32_t) {});
    engine->process(Mode::Batch);
    ::unlink(path.c_str());
  }
}

TEST_CASE("OpenAt operation opens a file relative to a dirfd") {
  for (const bool sqpoll : {true, false}) {
    CAPTURE(sqpoll);
    auto ring_engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 64, .sqpoll = sqpoll});
    REQUIRE(ring_engine_result.has_value());
    auto engine = std::move(ring_engine_result).value();
    CHECK(engine->sqpoll_enabled() == sqpoll);
    using Mode = dojio::RingEngine::ProcessMode;
    const auto dir = std::filesystem::temp_directory_path();
    const std::string name = "dojio_openat_test_file";
    const std::string full = (dir / name).string();
    {
      const std::int32_t fd = static_cast<std::int32_t>(::open(full.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644));
      REQUIRE(fd >= 0);
      REQUIRE(::write(fd, "openat-content", 14) == 14);
      ::close(fd);
    }
    const std::int32_t dirfd = static_cast<std::int32_t>(::open(dir.c_str(), O_RDONLY | O_DIRECTORY));
    REQUIRE(dirfd >= 0);

    dojio::FileDescriptor fd{-1};
    engine->submit(dojio::open_at(dojio::FileDescriptor{dirfd}, name, O_RDONLY),
                   [&](dojio::Result<dojio::FileDescriptor> r, dojio::OperationId, std::int32_t res) {
                     REQUIRE(r.has_value());
                     CHECK(res >= 0);
                     fd = *r;
                   });
    engine->process(Mode::Batch);
    REQUIRE(fd.value() >= 0);

    std::string content;
    engine->submit(dojio::pread(fd, 0), std::vector<std::byte>(64),
                   [&](dojio::Result<std::vector<std::byte>> r, dojio::OperationId, std::int32_t res) {
                     REQUIRE(r.has_value());
                     CHECK(res == 14);
                     content = to_string(*r);
                   });
    engine->process(Mode::Batch);
    CHECK(content == "openat-content");

    engine->submit(dojio::close(fd), [](dojio::Result<std::int32_t>, dojio::OperationId, std::int32_t) {});
    engine->process(Mode::Batch);
    ::close(dirfd);
    ::unlink(full.c_str());
  }
}

TEST_CASE("Bind operation binds a socket to an IPv4 endpoint") {
  for (const bool sqpoll : {true, false}) {
    CAPTURE(sqpoll);
    auto ring_engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 64, .sqpoll = sqpoll});
    REQUIRE(ring_engine_result.has_value());
    auto engine = std::move(ring_engine_result).value();
    CHECK(engine->sqpoll_enabled() == sqpoll);
    using Mode = dojio::RingEngine::ProcessMode;

    auto sock = dojio::create_socket(dojio::AddressFamily::Inet, dojio::SocketType::Stream, dojio::SocketProtocol::Tcp);
    REQUIRE(sock.has_value());
    const dojio::FileDescriptor fd = *sock;

    bool bound = false;
    engine->submit(dojio::bind(fd, dojio::Ipv4Endpoint::loopback(0)),
                   [&](dojio::Result<std::int32_t> r, dojio::OperationId, std::int32_t res) {
                     REQUIRE(r.has_value());
                     CHECK(res == 0);
                     bound = true;
                   });
    engine->process(Mode::Batch);
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
        dojio::create_socket(dojio::AddressFamily::Inet, dojio::SocketType::Stream, dojio::SocketProtocol::Tcp);
    REQUIRE(sock2.has_value());
    bool failed = false;
    engine->submit(dojio::bind(*sock2, dojio::Ipv4Endpoint::loopback(ntohs(addr.sin_port))),
                   [&](dojio::Result<std::int32_t> r, dojio::OperationId, std::int32_t) {
                     CHECK_FALSE(r.has_value());
                     REQUIRE(std::holds_alternative<dojio::IoError>(r.error()));
                     CHECK(std::get<dojio::IoError>(r.error()).value == -EADDRINUSE);
                     failed = true;
                   });
    engine->process(Mode::Batch);
    CHECK(failed);

    engine->submit(dojio::close(*sock2), [](dojio::Result<std::int32_t>, dojio::OperationId, std::int32_t) {});
    engine->submit(dojio::close(fd), [](dojio::Result<std::int32_t>, dojio::OperationId, std::int32_t) {});
    engine->process(Mode::Batch);
  }
}

TEST_CASE("Bind and Listen operations work over IPv6") {
  for (const bool sqpoll : {true, false}) {
    CAPTURE(sqpoll);
    auto ring_engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 64, .sqpoll = sqpoll});
    REQUIRE(ring_engine_result.has_value());
    auto engine = std::move(ring_engine_result).value();
    CHECK(engine->sqpoll_enabled() == sqpoll);
    using Mode = dojio::RingEngine::ProcessMode;

    auto sock =
        dojio::create_socket(dojio::AddressFamily::Inet6, dojio::SocketType::Stream, dojio::SocketProtocol::Tcp);
    REQUIRE(sock.has_value());
    const dojio::FileDescriptor fd = *sock;

    bool bound = false;
    engine->submit(dojio::bind(fd, dojio::Ipv6Endpoint::loopback(0)),
                   [&](dojio::Result<std::int32_t> r, dojio::OperationId, std::int32_t res) {
                     REQUIRE(r.has_value());
                     CHECK(res == 0);
                     bound = true;
                   });
    engine->process(Mode::Batch);
    CHECK(bound);

    // the kernel assigned an ephemeral port on [::1]
    sockaddr_in6 addr{};
    socklen_t alen = sizeof(addr);
    REQUIRE(::getsockname(fd.value(), reinterpret_cast<sockaddr*>(&addr), &alen) == 0);
    CHECK(addr.sin6_family == AF_INET6);
    CHECK(addr.sin6_addr.s6_addr[15] == 1);
    CHECK(ntohs(addr.sin6_port) != 0);

    bool listened = false;
    engine->submit(dojio::listen(fd, 16), [&](dojio::Result<std::int32_t> r, dojio::OperationId, std::int32_t res) {
      REQUIRE(r.has_value());
      CHECK(res == 0);
      listened = true;
    });
    engine->process(Mode::Batch);
    CHECK(listened);

    // raw blocking connect over IPv6, then accept through the RingEngine
    const std::int32_t cfd = static_cast<std::int32_t>(::socket(AF_INET6, SOCK_STREAM, 0));
    REQUIRE(cfd >= 0);
    sockaddr_in6 caddr{};
    caddr.sin6_family = AF_INET6;
    caddr.sin6_addr = addr.sin6_addr;
    caddr.sin6_port = addr.sin6_port;
    REQUIRE(::connect(cfd, reinterpret_cast<const sockaddr*>(&caddr), sizeof(caddr)) == 0);

    dojio::FileDescriptor accepted{-1};
    engine->submit(dojio::accept(fd), dojio::AcceptBuffer{},
                   [&](dojio::Result<dojio::FileDescriptor> r, dojio::OperationId, std::int32_t res) {
                     REQUIRE(r.has_value());
                     CHECK(res >= 0);
                     accepted = *r;
                   });
    engine->process(Mode::Batch);
    REQUIRE(accepted.value() >= 0);

    sockaddr_in6 peer{};
    socklen_t plen = sizeof(peer);
    REQUIRE(::getpeername(accepted.value(), reinterpret_cast<sockaddr*>(&peer), &plen) == 0);
    CHECK(peer.sin6_family == AF_INET6);
    CHECK(peer.sin6_addr.s6_addr[15] == 1);

    engine->submit(dojio::close(accepted), [](dojio::Result<std::int32_t>, dojio::OperationId, std::int32_t) {});
    engine->submit(dojio::close(fd), [](dojio::Result<std::int32_t>, dojio::OperationId, std::int32_t) {});
    engine->process(Mode::Batch);
    ::close(cfd);
  }
}
