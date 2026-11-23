#include <doctest/doctest.h>

#include <chrono>
#include <variant>

#include <dojio/result.hpp>
#include <dojio/ring_engine.hpp>
#include <dojio/timer.hpp>

TEST_CASE("Timer fires after the requested duration") {
  auto ring_engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 64});
  REQUIRE(ring_engine_result.has_value());
  auto engine = std::move(ring_engine_result).value();
  const auto start = std::chrono::steady_clock::now();
  std::chrono::nanoseconds elapsed{0};
  bool fired = false;
  engine->submit(
      dojio::Timer{std::chrono::milliseconds(50)}, [&](dojio::RingEngine&, dojio::Result<dojio::TimeoutResult> r) {
        CHECK(r.has_value()); // a fired timer completes successfully
        fired = true;
        elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start);
      });
  engine->process(dojio::RingEngine::ProcessMode::Batch);
  CHECK(fired);
  CHECK(elapsed >= std::chrono::milliseconds(50));
  CHECK(engine->count_operations(dojio::RingEngine::OperationState::Completed) == 1);
}

TEST_CASE("Timer can be cancelled before it fires") {
  auto ring_engine_result = dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 64});
  REQUIRE(ring_engine_result.has_value());
  auto engine = std::move(ring_engine_result).value();
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
  CHECK(engine->count_operations(dojio::RingEngine::OperationState::Failed) == 1);
  CHECK(engine->count_operations(dojio::RingEngine::OperationState::Completed) == 0);
}
