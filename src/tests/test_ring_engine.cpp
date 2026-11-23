#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <thread>
#include <variant>
#include <vector>

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
  const std::function<void(dojio::RingEngine&, dojio::Result<dojio::TimeoutResult>)> cb =
      [&](dojio::RingEngine&, dojio::Result<dojio::TimeoutResult> r) {
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
  const dojio::OperationId id = engine->submit(dojio::Timer{std::chrono::seconds(30)},
                                               [&](dojio::RingEngine&, dojio::Result<dojio::TimeoutResult> r) {
                                                 called = true;
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
  std::function<void(dojio::RingEngine&, dojio::Result<dojio::TimeoutResult>)> cb;
  cb = [&](dojio::RingEngine&, dojio::Result<dojio::TimeoutResult>) {
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
                 [&](dojio::RingEngine&, dojio::Result<dojio::TimeoutResult> r) {
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
                   [&](dojio::RingEngine&, dojio::Result<dojio::TimeoutResult> r) {
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
                 [&](dojio::RingEngine&, dojio::Result<dojio::TimeoutResult> r) {
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
                   [&](dojio::RingEngine&, dojio::Result<dojio::TimeoutResult> r) {
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
                 [&](dojio::RingEngine&, dojio::Result<dojio::TimeoutResult> r) {
                   CHECK(r.has_value());
                   done += 1;
                 });
  const dojio::OperationId long_id = engine->submit(dojio::Timer{std::chrono::seconds(30)},
                                                    [](dojio::RingEngine&, dojio::Result<dojio::TimeoutResult>) {});
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
                 [&](dojio::RingEngine&, dojio::Result<dojio::TimeoutResult> r) {
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
                 [&](dojio::RingEngine&, dojio::Result<dojio::TimeoutResult> r) {
                   REQUIRE(r.has_value());
                   stage = 1;
                   engine->submit(dojio::Timer{std::chrono::milliseconds(1)},
                                  [&](dojio::RingEngine&, dojio::Result<dojio::TimeoutResult> r2) {
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

TEST_CASE("SQPOLL submit() publishes proactively before process() is called") {
  auto ring_engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 64, .sqpoll = true});
  if (!ring_engine_result.has_value()) {
    CHECK(ring_engine_result.error().opcode == dojio::ErrorOpcode::IoUringSetup);
    return;
  }
  auto engine = std::move(ring_engine_result).value();
  REQUIRE(engine->sqpoll_enabled());
  bool fired = false;
  engine->submit(dojio::Timer{std::chrono::milliseconds(5)},
                 [&](dojio::RingEngine&, dojio::Result<dojio::TimeoutResult> r) {
                   CHECK(r.has_value());
                   fired = true;
                 });
  // wait longer than the timer WITHOUT calling process()/poll(): the SQPOLL thread
  // should already have picked up and completed the timer
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  const std::uint64_t dispatched = engine->poll();
  CHECK(dispatched == 1);
  CHECK(fired);
  CHECK(engine->count_operations(dojio::RingEngine::OperationState::Pending) == 0);
  CHECK(engine->count_operations(dojio::RingEngine::OperationState::Completed) == 1);
}

TEST_CASE("submitting more operations than sq_entries overflows cleanly into unsubmitted_") {
  for (const bool sqpoll : {true, false}) {
    CAPTURE(sqpoll);
    auto ring_engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 8, .sqpoll = sqpoll});
    if (!ring_engine_result.has_value()) {
      CHECK(ring_engine_result.error().opcode == dojio::ErrorOpcode::IoUringSetup);
      continue;
    }
    auto engine = std::move(ring_engine_result).value();
    using State = dojio::RingEngine::OperationState;
    constexpr std::int32_t k_total = 24;
    std::int32_t completed = 0;
    for (std::int32_t i = 0; i < k_total; ++i) {
      engine->submit(dojio::Timer{std::chrono::milliseconds(1)},
                     [&](dojio::RingEngine&, dojio::Result<dojio::TimeoutResult> r) {
                       CHECK(r.has_value());
                       completed += 1;
                     });
    }
    CHECK(engine->count_operations(State::Pending) == static_cast<std::uint64_t>(k_total));
    const std::uint64_t dispatched = engine->process(dojio::RingEngine::ProcessMode::Batch);
    CHECK(dispatched == static_cast<std::uint64_t>(k_total));
    CHECK(completed == k_total);
    CHECK(engine->count_operations(State::Pending) == 0);
    CHECK(engine->count_operations(State::Completed) == static_cast<std::uint64_t>(k_total));
    CHECK(engine->count_operations(State::Failed) == 0);
  }
}

TEST_CASE("overflowed operations are flushed before blocking and can be cancelled in userspace") {
  auto ring_engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 8, .sqpoll = false});
  REQUIRE(ring_engine_result.has_value());
  auto engine = std::move(ring_engine_result).value();
  using State = dojio::RingEngine::OperationState;

  // fill all 8 SQ slots with long timers
  std::vector<dojio::OperationId> long_ids;
  long_ids.reserve(8);
  for (std::int32_t i = 0; i < 8; ++i) {
    long_ids.push_back(engine->submit(dojio::Timer{std::chrono::seconds(30)},
                                      [](dojio::RingEngine&, dojio::Result<dojio::TimeoutResult>) {}));
  }
  // 9th op overflows into unsubmitted_: cancel it before process()
  bool cancelled_called = false;
  const dojio::OperationId overflow_cancel_id = engine->submit(
      dojio::Timer{std::chrono::seconds(30)}, [&](dojio::RingEngine&, dojio::Result<dojio::TimeoutResult> r) {
        cancelled_called = true;
        CHECK_FALSE(r.has_value());
        CHECK(std::holds_alternative<dojio::Cancelled>(r.error()));
      });
  // 10th op also overflows into unsubmitted_: a short 5 ms timer
  bool short_fired = false;
  engine->submit(dojio::Timer{std::chrono::milliseconds(5)},
                 [&](dojio::RingEngine&, dojio::Result<dojio::TimeoutResult> r) {
                   CHECK(r.has_value());
                   short_fired = true;
                 });

  CHECK(engine->cancel(overflow_cancel_id));
  CHECK(cancelled_called);
  CHECK_FALSE(engine->cancel(overflow_cancel_id));

  // WaitOnce must submit the 8 long timers AND flush the overflowed 5 ms timer
  // before waiting, so it returns as soon as the 5 ms timer fires
  const auto start = std::chrono::steady_clock::now();
  const std::uint64_t dispatched = engine->process(dojio::RingEngine::ProcessMode::WaitOnce);
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
  CHECK(dispatched == 1);
  CHECK(short_fired);
  CHECK(engine->count_operations(State::Completed) == 1);
  CHECK(engine->count_operations(State::Failed) == 1);
  CHECK(engine->count_operations(State::Pending) == 8);

  for (const dojio::OperationId id : long_ids) {
    CHECK(engine->cancel(id));
  }
  engine->process(dojio::RingEngine::ProcessMode::Batch);
  CHECK(engine->count_operations(State::Pending) == 0);
  CHECK(engine->count_operations(State::Failed) == 9);
}

TEST_CASE("process() and poll() respect the max_tasks cap") {
  for (const bool sqpoll : {true, false}) {
    CAPTURE(sqpoll);
    auto ring_engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 64, .sqpoll = sqpoll});
    if (!ring_engine_result.has_value()) {
      CHECK(ring_engine_result.error().opcode == dojio::ErrorOpcode::IoUringSetup);
      continue;
    }
    auto engine = std::move(ring_engine_result).value();
    using State = dojio::RingEngine::OperationState;
    constexpr std::int32_t k_total = 10;
    std::int32_t completed = 0;
    for (std::int32_t i = 0; i < k_total; ++i) {
      engine->submit(dojio::Timer{std::chrono::milliseconds(1)},
                     [&](dojio::RingEngine&, dojio::Result<dojio::TimeoutResult> r) {
                       CHECK(r.has_value());
                       completed += 1;
                     });
    }
    // max_tasks == 0 flushes SQEs without dispatching any callback
    CHECK(engine->poll(0) == 0);
    CHECK(completed == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(15));

    // cap non-blocking poll to 3 tasks
    const std::uint64_t first = engine->poll(3);
    CHECK(first == 3);
    CHECK(completed == 3);
    CHECK(engine->count_operations(State::Pending) == 7);

    // cap Batch to 4 tasks
    const std::uint64_t second = engine->process(dojio::RingEngine::ProcessMode::Batch, 4);
    CHECK(second == 4);
    CHECK(completed == 7);
    CHECK(engine->count_operations(State::Pending) == 3);

    // drain the remaining 3 tasks
    const std::uint64_t rest = engine->process(dojio::RingEngine::ProcessMode::Batch);
    CHECK(rest == 3);
    CHECK(completed == k_total);
    CHECK(engine->count_operations(State::Pending) == 0);
    CHECK(engine->count_operations(State::Completed) == static_cast<std::uint64_t>(k_total));
  }
}
