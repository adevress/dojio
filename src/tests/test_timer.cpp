#include <doctest/doctest.h>

#include <chrono>
#include <variant>

#include <ringu/result.hpp>
#include <ringu/ring_engine.hpp>
#include <ringu/timer.hpp>

TEST_CASE("Timer fires after the requested duration") {
  auto ring_engine_result = ringu::RingEngine::create(ringu::RingEngineOptions{.entries = 64});
  REQUIRE(ring_engine_result.has_value());
  auto engine = std::move(ring_engine_result).value();
  const auto start = std::chrono::steady_clock::now();
  std::chrono::nanoseconds elapsed{0};
  bool fired = false;
  engine->submit(ringu::Timer{std::chrono::milliseconds(50)}, [&](ringu::Result<std::monostate> r, ringu::OperationId,
                                                                  std::int32_t) {
    CHECK(r.has_value()); // a fired timer completes successfully
    fired = true;
    elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start);
  });
  engine->process(ringu::RingEngine::ProcessMode::Batch);
  CHECK(fired);
  CHECK(elapsed >= std::chrono::milliseconds(50));
  CHECK(engine->count_operations(ringu::RingEngine::OperationState::Completed) == 1);
}

TEST_CASE("Timer can be cancelled before it fires") {
  auto ring_engine_result = ringu::RingEngine::create(ringu::RingEngineOptions{.entries = 64});
  REQUIRE(ring_engine_result.has_value());
  auto engine = std::move(ring_engine_result).value();
  bool called = false;
  const ringu::OperationId id = engine->submit(ringu::Timer{std::chrono::seconds(30)},
                                               [&](ringu::Result<std::monostate> r, ringu::OperationId, std::int32_t) {
                                                 called = true;
                                                 CHECK_FALSE(r.has_value());
                                                 CHECK(std::holds_alternative<ringu::Cancelled>(r.error()));
                                               });
  CHECK(engine->cancel(id));
  engine->process(ringu::RingEngine::ProcessMode::Batch);
  CHECK(called);
  CHECK(engine->count_operations(ringu::RingEngine::OperationState::Failed) == 1);
  CHECK(engine->count_operations(ringu::RingEngine::OperationState::Completed) == 0);
}
