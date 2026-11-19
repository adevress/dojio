#include <doctest/doctest.h>

#include <fcntl.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include <ringu/result.hpp>
#include <ringu/ring_engine.hpp>

TEST_CASE("pipe through the RingEngine hands out two working descriptors") {
  for (const bool sqpoll : {true, false}) {
    CAPTURE(sqpoll);
    auto ring_engine_result = ringu::RingEngine::create(ringu::RingEngineOptions{.entries = 64, .sqpoll = sqpoll});
    REQUIRE(ring_engine_result.has_value());
    auto engine = std::move(ring_engine_result).value();
    CHECK(engine->sqpoll_enabled() == sqpoll);
    using Mode = ringu::RingEngine::ProcessMode;
    using State = ringu::RingEngine::OperationState;

    // the operation writes the read end at index 0 and the write end at index 1
    ringu::PipeBuffer pipe_fds{};
    bool piped = false;
    engine->submit(ringu::pipe(0), std::move(pipe_fds),
                   [&](ringu::Result<ringu::PipeBuffer> r, ringu::OperationId, std::int32_t res) {
                     REQUIRE(r.has_value());
                     CHECK(res == 0);
                     pipe_fds = *r;
                     piped = true;
                   });
    engine->process(Mode::Batch);
    CHECK(piped);
    const ringu::FileDescriptor read_fd = pipe_fds[0];
    const ringu::FileDescriptor write_fd = pipe_fds[1];
    REQUIRE(read_fd.value() >= 0);
    REQUIRE(write_fd.value() >= 0);
    CHECK(read_fd.value() != write_fd.value());

    // the descriptors are real: write through the write end, read through the read end
    REQUIRE(::write(write_fd.value(), "data", 4) == 4);
    std::string received;
    engine->submit(ringu::read(read_fd), std::vector<std::byte>(16),
                   [&](ringu::Result<std::vector<std::byte>> r, ringu::OperationId, std::int32_t res) {
                     REQUIRE(r.has_value());
                     CHECK(res == 4);
                     received.assign(reinterpret_cast<const char*>(r->data()), r->size());
                   });
    engine->process(Mode::Batch);
    CHECK(received == "data");
    CHECK(engine->count_operations(State::Failed) == 0);
    CHECK(engine->count_operations(State::Completed) == 2);

    engine->submit(ringu::close(read_fd), [](ringu::Result<std::int32_t>, ringu::OperationId, std::int32_t) {});
    engine->submit(ringu::close(write_fd), [](ringu::Result<std::int32_t>, ringu::OperationId, std::int32_t) {});
    engine->process(Mode::Batch);
  }
}

TEST_CASE("pipe honors the pipe2 flags") {
  for (const bool sqpoll : {true, false}) {
    CAPTURE(sqpoll);
    auto ring_engine_result = ringu::RingEngine::create(ringu::RingEngineOptions{.entries = 64, .sqpoll = sqpoll});
    REQUIRE(ring_engine_result.has_value());
    auto engine = std::move(ring_engine_result).value();
    CHECK(engine->sqpoll_enabled() == sqpoll);
    using Mode = ringu::RingEngine::ProcessMode;

    ringu::PipeBuffer pipe_fds{};
    engine->submit(ringu::pipe(O_CLOEXEC), std::move(pipe_fds),
                   [&](ringu::Result<ringu::PipeBuffer> r, ringu::OperationId, std::int32_t res) {
                     REQUIRE(r.has_value());
                     CHECK(res == 0);
                     pipe_fds = *r;
                   });
    engine->process(Mode::Batch);
    REQUIRE(pipe_fds[0].value() >= 0);
    REQUIRE(pipe_fds[1].value() >= 0);
    CHECK((::fcntl(pipe_fds[0].value(), F_GETFD) & FD_CLOEXEC) != 0);
    CHECK((::fcntl(pipe_fds[1].value(), F_GETFD) & FD_CLOEXEC) != 0);

    engine->submit(ringu::close(pipe_fds[0]), [](ringu::Result<std::int32_t>, ringu::OperationId, std::int32_t) {});
    engine->submit(ringu::close(pipe_fds[1]), [](ringu::Result<std::int32_t>, ringu::OperationId, std::int32_t) {});
    engine->process(Mode::Batch);
  }
}
