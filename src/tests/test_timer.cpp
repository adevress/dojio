#include <doctest/doctest.h>

#include <chrono>
#include <variant>

#include <ringu/reactor.hpp>
#include <ringu/result.hpp>
#include <ringu/timer.hpp>

TEST_CASE("Timer fires after the requested duration") {
  auto reactor_result = ringu::Reactor::Create(ringu::ReactorOptions{.entries = 64});
  REQUIRE(reactor_result.has_value());
  auto reactor = std::move(reactor_result).value();
  const auto start = std::chrono::steady_clock::now();
  std::chrono::nanoseconds elapsed{0};
  bool fired = false;
  reactor->submit(ringu::Timer{std::chrono::milliseconds(50)}, [&](ringu::Result<std::monostate> r, ringu::OperationId,
                                                                   std::int32_t) {
    CHECK(r.has_value()); // a fired timer completes successfully
    fired = true;
    elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start);
  });
  reactor->process(ringu::Reactor::ProcessMode::Batch);
  CHECK(fired);
  CHECK(elapsed >= std::chrono::milliseconds(50));
  CHECK(reactor->count_operations(ringu::Reactor::OperationState::Completed) == 1);
}

TEST_CASE("Timer can be cancelled before it fires") {
  auto reactor_result = ringu::Reactor::Create(ringu::ReactorOptions{.entries = 64});
  REQUIRE(reactor_result.has_value());
  auto reactor = std::move(reactor_result).value();
  bool called = false;
  const ringu::OperationId id = reactor->submit(ringu::Timer{std::chrono::seconds(30)},
                                                [&](ringu::Result<std::monostate> r, ringu::OperationId, std::int32_t) {
                                                  called = true;
                                                  CHECK_FALSE(r.has_value());
                                                  CHECK(std::holds_alternative<ringu::Cancelled>(r.error()));
                                                });
  CHECK(reactor->cancel(id));
  reactor->process(ringu::Reactor::ProcessMode::Batch);
  CHECK(called);
  CHECK(reactor->count_operations(ringu::Reactor::OperationState::Failed) == 1);
  CHECK(reactor->count_operations(ringu::Reactor::OperationState::Completed) == 0);
}
