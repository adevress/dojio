#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <variant>

#include <ringu/result.hpp>
#include <ringu/ring_engine.hpp>

TEST_CASE("create returns a tl::expected: the RingEngine on success, a RinguError on failure") {
  auto result = ringu::RingEngine::create(ringu::RingEngineOptions{.entries = 64});
  REQUIRE(result.has_value());
  CHECK(result.value()->ring_fd() >= 0);
  // a RinguError carries the opcode of the failing operation plus a message
  const ringu::RinguError err{ringu::ErrorOpcode::IoUringSetup, "boom"};
  CHECK(err.opcode == ringu::ErrorOpcode::IoUringSetup);
  CHECK(err.message == "boom");
  CHECK(err == ringu::RinguError{ringu::ErrorOpcode::IoUringSetup, "boom"});
  CHECK_FALSE(err == ringu::RinguError{ringu::ErrorOpcode::IoUringMmap, "boom"});
}

TEST_CASE("submit returns typed operation ids and count_operations tracks the states") {
  auto ring_engine_result = ringu::RingEngine::create(ringu::RingEngineOptions{.entries = 64});
  REQUIRE(ring_engine_result.has_value());
  auto engine = std::move(ring_engine_result).value();
  using State = ringu::RingEngine::OperationState;
  std::int32_t completed = 0;
  const std::function<void(ringu::Result<std::monostate>, ringu::OperationId, std::int32_t)> cb =
      [&](ringu::Result<std::monostate> r, ringu::OperationId, std::int32_t) {
        CHECK(r.has_value());
        completed += 1;
      };
  const ringu::OperationId id1 = engine->submit(ringu::Timer{std::chrono::milliseconds(5)}, cb);
  const ringu::OperationId id2 = engine->submit(ringu::Timer{std::chrono::milliseconds(10)}, cb);
  CHECK(id1 != id2);
  CHECK(id1 != ringu::k_invalid_operation_id);
  CHECK(engine->count_operations(State::Pending) == 2);
  CHECK(engine->count_operations(State::Completed) == 0);
  CHECK(engine->count_operations(State::Failed) == 0);

  engine->process(ringu::RingEngine::ProcessMode::Batch);
  CHECK(completed == 2);
  CHECK(engine->count_operations(State::Pending) == 0);
  CHECK(engine->count_operations(State::Completed) == 2);
  CHECK(engine->count_operations(State::Failed) == 0);
}

TEST_CASE("cancel completes the operation with the Cancelled error code (a failure)") {
  auto ring_engine_result = ringu::RingEngine::create(ringu::RingEngineOptions{.entries = 64});
  REQUIRE(ring_engine_result.has_value());
  auto engine = std::move(ring_engine_result).value();
  using State = ringu::RingEngine::OperationState;
  bool called = false;
  const ringu::OperationId id =
      engine->submit(ringu::Timer{std::chrono::seconds(30)},
                     [&](ringu::Result<std::monostate> r, ringu::OperationId got_id, std::int32_t) {
                       called = true;
                       CHECK(got_id == id);
                       CHECK_FALSE(r.has_value());
                       CHECK(std::holds_alternative<ringu::Cancelled>(r.error()));
                     });
  CHECK(engine->cancel(id));
  engine->process(ringu::RingEngine::ProcessMode::Batch);
  CHECK(called);
  CHECK(engine->count_operations(State::Pending) == 0);
  CHECK(engine->count_operations(State::Completed) == 0);
  CHECK(engine->count_operations(State::Failed) == 1);
  // cancelling an unknown / already completed id returns false
  CHECK_FALSE(engine->cancel(id));
  CHECK_FALSE(engine->cancel(ringu::k_invalid_operation_id));
}

TEST_CASE("process(Forever) loops until request_stop interrupts it") {
  auto ring_engine_result = ringu::RingEngine::create(ringu::RingEngineOptions{.entries = 64});
  REQUIRE(ring_engine_result.has_value());
  auto engine = std::move(ring_engine_result).value();
  std::int32_t done = 0;
  constexpr std::int32_t k_target = 3;
  std::function<void(ringu::Result<std::monostate>, ringu::OperationId, std::int32_t)> cb;
  cb = [&](ringu::Result<std::monostate>, ringu::OperationId, std::int32_t) {
    done += 1;
    if (done < k_target) {
      engine->submit(ringu::Timer{std::chrono::milliseconds(1)}, cb);
    } else {
      engine->request_stop();
    }
  };
  engine->submit(ringu::Timer{std::chrono::milliseconds(1)}, cb);
  engine->process(ringu::RingEngine::ProcessMode::Forever);
  CHECK(done == k_target);
  CHECK(engine->count_operations(ringu::RingEngine::OperationState::Completed) == k_target);
}

TEST_CASE("the SQPOLL polling time option is applied when setting up the ring") {
  auto ring_engine_result = ringu::RingEngine::create(
      ringu::RingEngineOptions{.entries = 64, .sqpoll = true, .sqpoll_poll_time = std::chrono::milliseconds{500}});
  if (!ring_engine_result.has_value()) {
    // SQPOLL is not available on this kernel / without the required privileges:
    // the setup failure is reported as an IoUringSetup error and there is nothing more to test
    CHECK(ring_engine_result.error().opcode == ringu::ErrorOpcode::IoUringSetup);
    return;
  }
  auto engine = std::move(ring_engine_result).value();
  REQUIRE(engine->sqpoll_enabled());
  CHECK(engine->sqpoll_poll_time() == std::chrono::milliseconds{500});
  // the ring still works with a custom poll time
  std::int32_t done = 0;
  engine->submit(ringu::Timer{std::chrono::milliseconds(1)},
                 [&](ringu::Result<std::monostate> r, ringu::OperationId, std::int32_t) {
                   CHECK(r.has_value());
                   done += 1;
                 });
  engine->process(ringu::RingEngine::ProcessMode::Batch);
  CHECK(done == 1);
  CHECK(engine->count_operations(ringu::RingEngine::OperationState::Completed) == 1);
}

TEST_CASE("a batch of operations on an SQPOLL ring completes (kernel thread submits the SQEs)") {
  auto ring_engine_result = ringu::RingEngine::create(ringu::RingEngineOptions{.entries = 64, .sqpoll = true});
  if (!ring_engine_result.has_value()) {
    // SQPOLL is not available on this kernel / without the required privileges:
    // the setup failure is reported as an IoUringSetup error and there is nothing more to test
    CHECK(ring_engine_result.error().opcode == ringu::ErrorOpcode::IoUringSetup);
    return;
  }
  auto engine = std::move(ring_engine_result).value();
  REQUIRE(engine->sqpoll_enabled());
  using State = ringu::RingEngine::OperationState;
  std::int32_t done = 0;
  for (std::int32_t i = 0; i < 8; ++i) {
    engine->submit(ringu::Timer{std::chrono::milliseconds(1)},
                   [&](ringu::Result<std::monostate> r, ringu::OperationId, std::int32_t) {
                     CHECK(r.has_value());
                     done += 1;
                   });
  }
  engine->process(ringu::RingEngine::ProcessMode::Batch);
  CHECK(done == 8);
  CHECK(engine->count_operations(State::Pending) == 0);
  CHECK(engine->count_operations(State::Completed) == 8);
  CHECK(engine->count_operations(State::Failed) == 0);
}

TEST_CASE("operations on a non-SQPOLL ring still complete when SQPOLL is disabled") {
  auto ring_engine_result = ringu::RingEngine::create(ringu::RingEngineOptions{.entries = 64, .sqpoll = false});
  REQUIRE(ring_engine_result.has_value());
  auto engine = std::move(ring_engine_result).value();
  REQUIRE_FALSE(engine->sqpoll_enabled());
  using State = ringu::RingEngine::OperationState;
  std::int32_t done = 0;
  engine->submit(ringu::Timer{std::chrono::milliseconds(1)},
                 [&](ringu::Result<std::monostate> r, ringu::OperationId, std::int32_t) {
                   CHECK(r.has_value());
                   done += 1;
                 });
  engine->process(ringu::RingEngine::ProcessMode::Batch);
  CHECK(done == 1);
  CHECK(engine->count_operations(State::Completed) == 1);
}

TEST_CASE("process(Batch) with nothing pending is a no-op") {
  auto ring_engine_result = ringu::RingEngine::create(ringu::RingEngineOptions{.entries = 64});
  REQUIRE(ring_engine_result.has_value());
  auto engine = std::move(ring_engine_result).value();
  engine->process(ringu::RingEngine::ProcessMode::Batch);
  CHECK(engine->count_operations(ringu::RingEngine::OperationState::Pending) == 0);
  CHECK(engine->count_operations(ringu::RingEngine::OperationState::Completed) == 0);
}

TEST_CASE("callbacks may submit new operations from the completion handler") {
  auto ring_engine_result = ringu::RingEngine::create(ringu::RingEngineOptions{.entries = 64});
  REQUIRE(ring_engine_result.has_value());
  auto engine = std::move(ring_engine_result).value();
  std::int32_t stage = 0;
  engine->submit(ringu::Timer{std::chrono::milliseconds(1)},
                 [&](ringu::Result<std::monostate> r, ringu::OperationId, std::int32_t) {
                   REQUIRE(r.has_value());
                   stage = 1;
                   engine->submit(ringu::Timer{std::chrono::milliseconds(1)},
                                  [&](ringu::Result<std::monostate> r2, ringu::OperationId, std::int32_t) {
                                    REQUIRE(r2.has_value());
                                    stage = 2;
                                  });
                 });
  engine->process(ringu::RingEngine::ProcessMode::Batch);
  // the second timer was submitted from the callback: the batch drains it too
  CHECK(stage == 2);
  CHECK(engine->count_operations(ringu::RingEngine::OperationState::Completed) == 2);
  // a second batch with nothing pending is a no-op
  engine->process(ringu::RingEngine::ProcessMode::Batch);
  CHECK(stage == 2);
  CHECK(engine->count_operations(ringu::RingEngine::OperationState::Completed) == 2);
}
