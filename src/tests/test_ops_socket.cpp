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

#include <dojio/result.hpp>
#include <dojio/ring_engine.hpp>

namespace {

std::vector<std::byte> to_bytes(std::string_view s) {
  return {reinterpret_cast<const std::byte*>(s.data()), reinterpret_cast<const std::byte*>(s.data()) + s.size()};
}

std::string to_string(const std::vector<std::byte>& b) { return {reinterpret_cast<const char*>(b.data()), b.size()}; }

} // namespace

TEST_CASE("send/recv over a socketpair") {
  for (const bool sqpoll : {true, false}) {
    CAPTURE(sqpoll);
    auto ring_engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 64, .sqpoll = sqpoll});
    REQUIRE(ring_engine_result.has_value());
    auto engine = std::move(ring_engine_result).value();
    CHECK(engine->sqpoll_enabled() == sqpoll);
    using Mode = dojio::RingEngine::ProcessMode;
    std::int32_t sv[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, reinterpret_cast<int*>(sv)) == 0);

    std::string received;
    std::int32_t sent = -1;
    engine->submit(dojio::recv(dojio::FileDescriptor{sv[0]}), std::vector<std::byte>(64),
                   [&](dojio::Result<std::vector<std::byte>> r, dojio::OperationId, std::int32_t res) {
                     REQUIRE(r.has_value());
                     CHECK(res == 4);
                     received = to_string(*r);
                   });
    engine->submit(dojio::send(dojio::FileDescriptor{sv[1]}), to_bytes("ping"),
                   [&](dojio::Result<std::vector<std::byte>> r, dojio::OperationId, std::int32_t res) {
                     REQUIRE(r.has_value());
                     sent = res;
                   });
    engine->process(Mode::Batch);
    CHECK(sent == 4);
    CHECK(received == "ping");
    CHECK(engine->count_operations(dojio::RingEngine::OperationState::Completed) == 2);
    ::close(sv[0]);
    ::close(sv[1]);
  }
}

TEST_CASE("listen then accept over localhost TCP") {
  for (const bool sqpoll : {true, false}) {
    CAPTURE(sqpoll);
    auto ring_engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 64, .sqpoll = sqpoll});
    REQUIRE(ring_engine_result.has_value());
    auto engine = std::move(ring_engine_result).value();
    CHECK(engine->sqpoll_enabled() == sqpoll);
    using Mode = dojio::RingEngine::ProcessMode;
    using State = dojio::RingEngine::OperationState;

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

    // listen through the RingEngine (executed inline when the batch is processed)
    bool listened = false;
    engine->submit(dojio::listen(dojio::FileDescriptor{lfd}, 16),
                   [&](dojio::Result<std::int32_t> r, dojio::OperationId, std::int32_t res) {
                     REQUIRE(r.has_value());
                     CHECK(res == 0);
                     listened = true;
                   });
    engine->process(Mode::Batch);
    CHECK(listened);

    // connect a client with a plain blocking connect
    const std::int32_t cfd = static_cast<std::int32_t>(::socket(AF_INET, SOCK_STREAM, 0));
    REQUIRE(cfd >= 0);
    REQUIRE(::connect(cfd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);

    // accept through the RingEngine, with a buffer for the peer address
    dojio::FileDescriptor accepted{-1};
    engine->submit(dojio::accept(dojio::FileDescriptor{lfd}), std::vector<std::byte>(sizeof(sockaddr_storage)),
                   [&](dojio::Result<dojio::FileDescriptor> r, dojio::OperationId, std::int32_t res) {
                     REQUIRE(r.has_value());
                     CHECK(res >= 0);
                     accepted = *r;
                   });
    engine->process(Mode::Batch);
    REQUIRE(accepted.value() >= 0);

    // verify the accepted socket is connected to the loopback client
    sockaddr_in peer{};
    socklen_t plen = sizeof(peer);
    REQUIRE(::getpeername(accepted.value(), reinterpret_cast<sockaddr*>(&peer), &plen) == 0);
    CHECK(peer.sin_family == AF_INET);
    CHECK(peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK));

    // exchange data over the accepted connection through the RingEngine
    std::string received;
    engine->submit(dojio::recv(accepted), std::vector<std::byte>(16),
                   [&](dojio::Result<std::vector<std::byte>> r, dojio::OperationId, std::int32_t) {
                     REQUIRE(r.has_value());
                     received = to_string(*r);
                   });
    engine->submit(dojio::send(dojio::FileDescriptor{cfd}), to_bytes("hello"),
                   [](dojio::Result<std::vector<std::byte>> r, dojio::OperationId, std::int32_t res) {
                     REQUIRE(r.has_value());
                     CHECK(res == 5);
                   });
    engine->process(Mode::Batch);
    CHECK(received == "hello");

    engine->submit(dojio::close(accepted), [](dojio::Result<std::int32_t>, dojio::OperationId, std::int32_t) {});
    engine->process(Mode::Batch);
    CHECK(engine->count_operations(State::Failed) == 0);
    ::close(cfd);
    ::close(lfd);
  }
}

TEST_CASE("listen on a non-socket fd fails with ENOTSOCK") {
  for (const bool sqpoll : {true, false}) {
    CAPTURE(sqpoll);
    auto ring_engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 64, .sqpoll = sqpoll});
    REQUIRE(ring_engine_result.has_value());
    auto engine = std::move(ring_engine_result).value();
    CHECK(engine->sqpoll_enabled() == sqpoll);
    using State = dojio::RingEngine::OperationState;
    std::int32_t fds[2];
    REQUIRE(::pipe(reinterpret_cast<int*>(fds)) == 0);
    bool called = false;
    engine->submit(dojio::listen(dojio::FileDescriptor{fds[0]}, 4),
                   [&](dojio::Result<std::int32_t> r, dojio::OperationId, std::int32_t) {
                     called = true;
                     CHECK_FALSE(r.has_value());
                     REQUIRE(std::holds_alternative<dojio::IoError>(r.error()));
                     CHECK(std::get<dojio::IoError>(r.error()).value == -ENOTSOCK);
                   });
    engine->process(dojio::RingEngine::ProcessMode::Batch);
    CHECK(called);
    CHECK(engine->count_operations(State::Failed) == 1);
    ::close(fds[0]);
    ::close(fds[1]);
  }
}

TEST_CASE("accept on a non-listening socket fails") {
  for (const bool sqpoll : {true, false}) {
    CAPTURE(sqpoll);
    auto ring_engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 64, .sqpoll = sqpoll});
    REQUIRE(ring_engine_result.has_value());
    auto engine = std::move(ring_engine_result).value();
    CHECK(engine->sqpoll_enabled() == sqpoll);
    using State = dojio::RingEngine::OperationState;
    const std::int32_t fd = static_cast<std::int32_t>(::socket(AF_INET, SOCK_STREAM, 0));
    REQUIRE(fd >= 0);
    bool called = false;
    engine->submit(dojio::accept(dojio::FileDescriptor{fd}),
                   [&](dojio::Result<dojio::FileDescriptor> r, dojio::OperationId, std::int32_t) {
                     called = true;
                     CHECK_FALSE(r.has_value());
                     CHECK(std::holds_alternative<dojio::IoError>(r.error()));
                   });
    engine->process(dojio::RingEngine::ProcessMode::Batch);
    CHECK(called);
    CHECK(engine->count_operations(State::Failed) == 1);
    ::close(fd);
  }
}
