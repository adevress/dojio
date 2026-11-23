#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <thread>
#include <variant>

#include <dojio/result.hpp>
#include <dojio/ring_engine.hpp>

TEST_CASE("create returns a tl::expected: the RingEngine on success, a DojioError on failure") {
  auto result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 64});
  REQUIRE(result.has_value());
  CHECK(result.value()->ring_fd() >= 0);
  // a DojioError carries the opcode of the failing operation plus a message
  const dojio::DojioError err{dojio::ErrorOpcode::IoUringSetup, "boom"};
  CHECK(err.opcode == dojio::ErrorOpcode::IoUringSetup);
  CHECK(err.message == "boom");
  CHECK(err == dojio::DojioError{dojio::ErrorOpcode::IoUringSetup, "boom"});
  CHECK_FALSE(err == dojio::DojioError{dojio::ErrorOpcode::IoUringMmap, "boom"});
}

TEST_CASE("submit returns typed operation ids and count_operations tracks the states") {
  auto ring_engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 64});
  REQUIRE(ring_engine_result.has_value());
  auto engine = std::move(ring_engine_result).value();
  using State = dojio::RingEngine::OperationState;
  std::int32_t completed = 0;
  const std::function<void(dojio::Result<std::monostate>, dojio::OperationId, std::int32_t)> cb =
      [&](dojio::Result<std::monostate> r, dojio::OperationId, std::int32_t) {
        CHECK(r.has_value());
        completed += 1;
      };
  const dojio::OperationId id1 = engine->submit(dojio::Timer{std::chrono::milliseconds(5)}, cb);
  const dojio::OperationId id2 = engine->submit(dojio::Timer{std::chrono::milliseconds(10)}, cb);
  CHECK(id1 != id2);
  CHECK(id1 != dojio::k_invalid_operation_id);
  CHECK(engine->count_operations(State::Pending) == 2);
  CHECK(engine->count_operations(State::Completed) == 0);
  CHECK(engine->count_operations(State::Failed) == 0);

  engine->process(dojio::RingEngine::ProcessMode::Batch);
  CHECK(completed == 2);
  CHECK(engine->count_operations(State::Pending) == 0);
  CHECK(engine->count_operations(State::Completed) == 2);
  CHECK(engine->count_operations(State::Failed) == 0);
}

TEST_CASE("cancel completes the operation with the Cancelled error code (a failure)") {
  auto ring_engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 64});
  REQUIRE(ring_engine_result.has_value());
  auto engine = std::move(ring_engine_result).value();
  using State = dojio::RingEngine::OperationState;
  bool called = false;
  const dojio::OperationId id =
      engine->submit(dojio::Timer{std::chrono::seconds(30)},
                     [&](dojio::Result<std::monostate> r, dojio::OperationId got_id, std::int32_t) {
                       called = true;
                       CHECK(got_id == id);
                       CHECK_FALSE(r.has_value());
                       CHECK(std::holds_alternative<dojio::Cancelled>(r.error()));
                     });
  CHECK(engine->cancel(id));
  engine->process(dojio::RingEngine::ProcessMode::Batch);
  CHECK(called);
  CHECK(engine->count_operations(State::Pending) == 0);
  CHECK(engine->count_operations(State::Completed) == 0);
  CHECK(engine->count_operations(State::Failed) == 1);
  // cancelling an unknown / already completed id returns false
  CHECK_FALSE(engine->cancel(id));
  CHECK_FALSE(engine->cancel(dojio::k_invalid_operation_id));
}

TEST_CASE("process(Forever) loops until request_stop interrupts it") {
  auto ring_engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 64});
  REQUIRE(ring_engine_result.has_value());
  auto engine = std::move(ring_engine_result).value();
  std::int32_t done = 0;
  constexpr std::int32_t k_target = 3;
  std::function<void(dojio::Result<std::monostate>, dojio::OperationId, std::int32_t)> cb;
  cb = [&](dojio::Result<std::monostate>, dojio::OperationId, std::int32_t) {
    done += 1;
    if (done < k_target) {
      engine->submit(dojio::Timer{std::chrono::milliseconds(1)}, cb);
    } else {
      engine->request_stop();
    }
  };
  engine->submit(dojio::Timer{std::chrono::milliseconds(1)}, cb);
  engine->process(dojio::RingEngine::ProcessMode::Forever);
  CHECK(done == k_target);
  CHECK(engine->count_operations(dojio::RingEngine::OperationState::Completed) == k_target);
}

TEST_CASE("the SQPOLL polling time option is applied when setting up the ring") {
  auto ring_engine_result = dojio::RingEngine::create(
      dojio::RingEngineOptions{.entries = 64, .sqpoll = true, .sqpoll_poll_time = std::chrono::milliseconds{500}});
  if (!ring_engine_result.has_value()) {
    // SQPOLL is not available on this kernel / without the required privileges:
    // the setup failure is reported as an IoUringSetup error and there is nothing more to test
    CHECK(ring_engine_result.error().opcode == dojio::ErrorOpcode::IoUringSetup);
    return;
  }
  auto engine = std::move(ring_engine_result).value();
  REQUIRE(engine->sqpoll_enabled());
  CHECK(engine->sqpoll_poll_time() == std::chrono::milliseconds{500});
  // the ring still works with a custom poll time
  std::int32_t done = 0;
  engine->submit(dojio::Timer{std::chrono::milliseconds(1)},
                 [&](dojio::Result<std::monostate> r, dojio::OperationId, std::int32_t) {
                   CHECK(r.has_value());
                   done += 1;
                 });
  engine->process(dojio::RingEngine::ProcessMode::Batch);
  CHECK(done == 1);
  CHECK(engine->count_operations(dojio::RingEngine::OperationState::Completed) == 1);
}

TEST_CASE("a batch of operations on an SQPOLL ring completes (kernel thread submits the SQEs)") {
  auto ring_engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 64, .sqpoll = true});
  if (!ring_engine_result.has_value()) {
    // SQPOLL is not available on this kernel / without the required privileges:
    // the setup failure is reported as an IoUringSetup error and there is nothing more to test
    CHECK(ring_engine_result.error().opcode == dojio::ErrorOpcode::IoUringSetup);
    return;
  }
  auto engine = std::move(ring_engine_result).value();
  REQUIRE(engine->sqpoll_enabled());
  using State = dojio::RingEngine::OperationState;
  std::int32_t done = 0;
  for (std::int32_t i = 0; i < 8; ++i) {
    engine->submit(dojio::Timer{std::chrono::milliseconds(1)},
                   [&](dojio::Result<std::monostate> r, dojio::OperationId, std::int32_t) {
                     CHECK(r.has_value());
                     done += 1;
                   });
  }
  engine->process(dojio::RingEngine::ProcessMode::Batch);
  CHECK(done == 8);
  CHECK(engine->count_operations(State::Pending) == 0);
  CHECK(engine->count_operations(State::Completed) == 8);
  CHECK(engine->count_operations(State::Failed) == 0);
}

TEST_CASE("operations on a non-SQPOLL ring still complete when SQPOLL is disabled") {
  auto ring_engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 64, .sqpoll = false});
  REQUIRE(ring_engine_result.has_value());
  auto engine = std::move(ring_engine_result).value();
  REQUIRE_FALSE(engine->sqpoll_enabled());
  using State = dojio::RingEngine::OperationState;
  std::int32_t done = 0;
  engine->submit(dojio::Timer{std::chrono::milliseconds(1)},
                 [&](dojio::Result<std::monostate> r, dojio::OperationId, std::int32_t) {
                   CHECK(r.has_value());
                   done += 1;
                 });
  engine->process(dojio::RingEngine::ProcessMode::Batch);
  CHECK(done == 1);
  CHECK(engine->count_operations(State::Completed) == 1);
}

TEST_CASE("process(Batch) with nothing pending is a no-op") {
  auto ring_engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 64});
  REQUIRE(ring_engine_result.has_value());
  auto engine = std::move(ring_engine_result).value();
  engine->process(dojio::RingEngine::ProcessMode::Batch);
  CHECK(engine->count_operations(dojio::RingEngine::OperationState::Pending) == 0);
  CHECK(engine->count_operations(dojio::RingEngine::OperationState::Completed) == 0);
}

TEST_CASE("process(NonBlocking) dispatches ready completions without blocking") {
  for (const bool sqpoll : {true, false}) {
    CAPTURE(sqpoll);
    auto ring_engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 64, .sqpoll = sqpoll});
    if (!ring_engine_result.has_value()) {
      // SQPOLL is not available on this kernel / without the required privileges:
      // the setup failure is reported as an IoUringSetup error and there is nothing more to test
      CHECK(ring_engine_result.error().opcode == dojio::ErrorOpcode::IoUringSetup);
      continue;
    }
    auto engine = std::move(ring_engine_result).value();
    using State = dojio::RingEngine::OperationState;
    std::int32_t done = 0;
    engine->submit(dojio::Timer{std::chrono::milliseconds(50)},
                   [&](dojio::Result<std::monostate> r, dojio::OperationId, std::int32_t) {
                     CHECK(r.has_value());
                     done += 1;
                   });
    // the 50 ms timer cannot have fired yet: NonBlocking returns immediately and
    // leaves the operation pending
    const auto start = std::chrono::steady_clock::now();
    engine->process(dojio::RingEngine::ProcessMode::NonBlocking);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(10));
    CHECK(done == 0);
    CHECK(engine->count_operations(State::Pending) == 1);
    CHECK(engine->count_operations(State::Completed) == 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    engine->process(dojio::RingEngine::ProcessMode::NonBlocking);
    CHECK(done == 1);
    CHECK(engine->count_operations(State::Pending) == 0);
    CHECK(engine->count_operations(State::Completed) == 1);
  }
}

TEST_CASE("process(WaitOnce) returns after dispatching one completion") {
  auto ring_engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 64});
  REQUIRE(ring_engine_result.has_value());
  auto engine = std::move(ring_engine_result).value();
  using State = dojio::RingEngine::OperationState;
  std::int32_t done = 0;
  engine->submit(dojio::Timer{std::chrono::milliseconds(5)},
                 [&](dojio::Result<std::monostate> r, dojio::OperationId, std::int32_t) {
                   CHECK(r.has_value());
                   done += 1;
                 });
  const dojio::OperationId long_id = engine->submit(
      dojio::Timer{std::chrono::seconds(30)}, [](dojio::Result<std::monostate>, dojio::OperationId, std::int32_t) {});
  // WaitOnce blocks at most until the 5 ms timer fires, not until the 30 s one
  const auto start = std::chrono::steady_clock::now();
  engine->process(dojio::RingEngine::ProcessMode::WaitOnce);
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
  CHECK(done == 1);
  CHECK(engine->count_operations(State::Completed) == 1);
  CHECK(engine->count_operations(State::Pending) == 1);
  // the 30 s timer is still in flight: cancel it and drain the cancellation
  CHECK(engine->cancel(long_id));
  engine->process(dojio::RingEngine::ProcessMode::Batch);
  CHECK(engine->count_operations(State::Pending) == 0);
  CHECK(engine->count_operations(State::Failed) == 1);
}

TEST_CASE("poll() returns the number of dispatched callbacks") {
  auto ring_engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 64});
  REQUIRE(ring_engine_result.has_value());
  auto engine = std::move(ring_engine_result).value();
  std::int32_t done = 0;
  engine->submit(dojio::Timer{std::chrono::milliseconds(1)},
                 [&](dojio::Result<std::monostate> r, dojio::OperationId, std::int32_t) {
                   CHECK(r.has_value());
                   done += 1;
                 });
  std::uint64_t dispatched = 0;
  for (std::int32_t attempt = 0; attempt < 100 && dispatched == 0; ++attempt) {
    dispatched = engine->poll();
    if (dispatched == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  CHECK(dispatched == 1);
  CHECK(done == 1);
  // nothing left to dispatch
  CHECK(engine->poll() == 0);
}

TEST_CASE("callbacks may submit new operations from the completion handler") {
  auto ring_engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 64});
  REQUIRE(ring_engine_result.has_value());
  auto engine = std::move(ring_engine_result).value();
  std::int32_t stage = 0;
  engine->submit(dojio::Timer{std::chrono::milliseconds(1)},
                 [&](dojio::Result<std::monostate> r, dojio::OperationId, std::int32_t) {
                   REQUIRE(r.has_value());
                   stage = 1;
                   engine->submit(dojio::Timer{std::chrono::milliseconds(1)},
                                  [&](dojio::Result<std::monostate> r2, dojio::OperationId, std::int32_t) {
                                    REQUIRE(r2.has_value());
                                    stage = 2;
                                  });
                 });
  engine->process(dojio::RingEngine::ProcessMode::Batch);
  // the second timer was submitted from the callback: the batch drains it too
  CHECK(stage == 2);
  CHECK(engine->count_operations(dojio::RingEngine::OperationState::Completed) == 2);
  // a second batch with nothing pending is a no-op
  engine->process(dojio::RingEngine::ProcessMode::Batch);
  CHECK(stage == 2);
  CHECK(engine->count_operations(dojio::RingEngine::OperationState::Completed) == 2);
}
