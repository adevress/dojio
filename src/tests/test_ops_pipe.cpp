#include <doctest/doctest.h>

#include <fcntl.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include <dojio/result.hpp>
#include <dojio/ring_engine.hpp>

TEST_CASE("pipe through the RingEngine hands out two working descriptors") {
  for (const bool sqpoll : {true, false}) {
    CAPTURE(sqpoll);
    auto ring_engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 64, .sqpoll = sqpoll});
    REQUIRE(ring_engine_result.has_value());
    auto engine = std::move(ring_engine_result).value();
    CHECK(engine->sqpoll_enabled() == sqpoll);
    using Mode = dojio::RingEngine::ProcessMode;
    using State = dojio::RingEngine::OperationState;

    // the operation writes the read end at index 0 and the write end at index 1
    dojio::PipeBuffer pipe_fds{};
    bool piped = false;
    engine->submit(dojio::pipe(0), [&](dojio::RingEngine&, dojio::Result<dojio::PipeResult> r) {
      REQUIRE(r.has_value());
      pipe_fds = r->fds;
      piped = true;
    });
    engine->process(Mode::Batch);
    CHECK(piped);
    const dojio::FileDescriptor read_fd = pipe_fds[0];
    const dojio::FileDescriptor write_fd = pipe_fds[1];
    REQUIRE(read_fd.value() >= 0);
    REQUIRE(write_fd.value() >= 0);
    CHECK(read_fd.value() != write_fd.value());

    // the descriptors are real: write through the write end, read through the read end
    REQUIRE(::write(write_fd.value(), "data", 4) == 4);
    std::string received;
    engine->submit(dojio::read(read_fd, std::vector<std::byte>(16)),
                   [&](dojio::RingEngine&, dojio::Result<dojio::ReadResult<std::vector<std::byte>>> r) {
                     REQUIRE(r.has_value());
                     CHECK(r->size == 4);
                     received.assign(reinterpret_cast<const char*>(r->buffer.data()), r->buffer.size());
                   });
    engine->process(Mode::Batch);
    CHECK(received == "data");
    CHECK(engine->count_operations(State::Failed) == 0);
    CHECK(engine->count_operations(State::Completed) == 2);

    engine->submit(dojio::close(read_fd), [](dojio::RingEngine&, dojio::Result<dojio::CloseResult>) {});
    engine->submit(dojio::close(write_fd), [](dojio::RingEngine&, dojio::Result<dojio::CloseResult>) {});
    engine->process(Mode::Batch);
  }
}

TEST_CASE("pipe honors the pipe2 flags") {
  for (const bool sqpoll : {true, false}) {
    CAPTURE(sqpoll);
    auto ring_engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 64, .sqpoll = sqpoll});
    REQUIRE(ring_engine_result.has_value());
    auto engine = std::move(ring_engine_result).value();
    CHECK(engine->sqpoll_enabled() == sqpoll);
    using Mode = dojio::RingEngine::ProcessMode;

    dojio::PipeBuffer pipe_fds{};
    engine->submit(dojio::pipe(O_CLOEXEC), [&](dojio::RingEngine&, dojio::Result<dojio::PipeResult> r) {
      REQUIRE(r.has_value());
      pipe_fds = r->fds;
    });
    engine->process(Mode::Batch);
    REQUIRE(pipe_fds[0].value() >= 0);
    REQUIRE(pipe_fds[1].value() >= 0);
    CHECK((::fcntl(pipe_fds[0].value(), F_GETFD) & FD_CLOEXEC) != 0);
    CHECK((::fcntl(pipe_fds[1].value(), F_GETFD) & FD_CLOEXEC) != 0);

    engine->submit(dojio::close(pipe_fds[0]), [](dojio::RingEngine&, dojio::Result<dojio::CloseResult>) {});
    engine->submit(dojio::close(pipe_fds[1]), [](dojio::RingEngine&, dojio::Result<dojio::CloseResult>) {});
    engine->process(Mode::Batch);
  }
}
