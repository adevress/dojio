#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <variant>

#include <ringu/reactor.hpp>
#include <ringu/result.hpp>

TEST_CASE("submit returns typed operation ids and count_operations tracks the states") {
  ringu::Reactor reactor{64};
  using State = ringu::Reactor::OperationState;
  std::int32_t completed = 0;
  const std::function<void(ringu::Result<std::monostate>, ringu::OperationId, std::int32_t)> cb =
      [&](ringu::Result<std::monostate> r, ringu::OperationId, std::int32_t) {
        CHECK(r.has_value());
        completed += 1;
      };
  const ringu::OperationId id1 = reactor.submit(ringu::Timer{std::chrono::milliseconds(5)}, cb);
  const ringu::OperationId id2 = reactor.submit(ringu::Timer{std::chrono::milliseconds(10)}, cb);
  CHECK(id1 != id2);
  CHECK(id1 != ringu::k_invalid_operation_id);
  CHECK(reactor.count_operations(State::Pending) == 2);
  CHECK(reactor.count_operations(State::Completed) == 0);
  CHECK(reactor.count_operations(State::Failed) == 0);

  reactor.process(ringu::Reactor::ProcessMode::Batch);
  CHECK(completed == 2);
  CHECK(reactor.count_operations(State::Pending) == 0);
  CHECK(reactor.count_operations(State::Completed) == 2);
  CHECK(reactor.count_operations(State::Failed) == 0);
}

TEST_CASE("cancel completes the operation with the Cancelled error code (a failure)") {
  ringu::Reactor reactor{64};
  using State = ringu::Reactor::OperationState;
  bool called = false;
  const ringu::OperationId id =
      reactor.submit(ringu::Timer{std::chrono::seconds(30)},
                     [&](ringu::Result<std::monostate> r, ringu::OperationId got_id, std::int32_t) {
                       called = true;
                       CHECK(got_id == id);
                       CHECK_FALSE(r.has_value());
                       CHECK(std::holds_alternative<ringu::Cancelled>(r.error()));
                     });
  CHECK(reactor.cancel(id));
  reactor.process(ringu::Reactor::ProcessMode::Batch);
  CHECK(called);
  CHECK(reactor.count_operations(State::Pending) == 0);
  CHECK(reactor.count_operations(State::Completed) == 0);
  CHECK(reactor.count_operations(State::Failed) == 1);
  // cancelling an unknown / already completed id returns false
  CHECK_FALSE(reactor.cancel(id));
  CHECK_FALSE(reactor.cancel(ringu::k_invalid_operation_id));
}

TEST_CASE("process(Forever) loops until request_stop interrupts it") {
  ringu::Reactor reactor{64};
  std::int32_t done = 0;
  constexpr std::int32_t k_target = 3;
  std::function<void(ringu::Result<std::monostate>, ringu::OperationId, std::int32_t)> cb;
  cb = [&](ringu::Result<std::monostate>, ringu::OperationId, std::int32_t) {
    done += 1;
    if (done < k_target) {
      reactor.submit(ringu::Timer{std::chrono::milliseconds(1)}, cb);
    } else {
      reactor.request_stop();
    }
  };
  reactor.submit(ringu::Timer{std::chrono::milliseconds(1)}, cb);
  reactor.process(ringu::Reactor::ProcessMode::Forever);
  CHECK(done == k_target);
  CHECK(reactor.count_operations(ringu::Reactor::OperationState::Completed) == k_target);
}

TEST_CASE("process(Batch) with nothing pending is a no-op") {
  ringu::Reactor reactor{64};
  reactor.process(ringu::Reactor::ProcessMode::Batch);
  CHECK(reactor.count_operations(ringu::Reactor::OperationState::Pending) == 0);
  CHECK(reactor.count_operations(ringu::Reactor::OperationState::Completed) == 0);
}

TEST_CASE("callbacks may submit new operations from the completion handler") {
  ringu::Reactor reactor{64};
  std::int32_t stage = 0;
  reactor.submit(ringu::Timer{std::chrono::milliseconds(1)},
                 [&](ringu::Result<std::monostate> r, ringu::OperationId, std::int32_t) {
                   REQUIRE(r.has_value());
                   stage = 1;
                   reactor.submit(ringu::Timer{std::chrono::milliseconds(1)},
                                  [&](ringu::Result<std::monostate> r2, ringu::OperationId, std::int32_t) {
                                    REQUIRE(r2.has_value());
                                    stage = 2;
                                  });
                 });
  reactor.process(ringu::Reactor::ProcessMode::Batch);
  // the second timer was submitted from the callback: the batch drains it too
  CHECK(stage == 2);
  CHECK(reactor.count_operations(ringu::Reactor::OperationState::Completed) == 2);
  // a second batch with nothing pending is a no-op
  reactor.process(ringu::Reactor::ProcessMode::Batch);
  CHECK(stage == 2);
  CHECK(reactor.count_operations(ringu::Reactor::OperationState::Completed) == 2);
}
