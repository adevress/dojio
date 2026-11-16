#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <variant>

#include <ringu/reactor.hpp>
#include <ringu/result.hpp>

TEST_CASE("Create returns a tl::expected: the reactor on success, a RinguError on failure") {
  auto result = ringu::Reactor::Create(ringu::ReactorOptions{.entries = 64});
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
  auto reactor_result = ringu::Reactor::Create(ringu::ReactorOptions{.entries = 64});
  REQUIRE(reactor_result.has_value());
  auto reactor = std::move(reactor_result).value();
  using State = ringu::Reactor::OperationState;
  std::int32_t completed = 0;
  const std::function<void(ringu::Result<std::monostate>, ringu::OperationId, std::int32_t)> cb =
      [&](ringu::Result<std::monostate> r, ringu::OperationId, std::int32_t) {
        CHECK(r.has_value());
        completed += 1;
      };
  const ringu::OperationId id1 = reactor->submit(ringu::Timer{std::chrono::milliseconds(5)}, cb);
  const ringu::OperationId id2 = reactor->submit(ringu::Timer{std::chrono::milliseconds(10)}, cb);
  CHECK(id1 != id2);
  CHECK(id1 != ringu::k_invalid_operation_id);
  CHECK(reactor->count_operations(State::Pending) == 2);
  CHECK(reactor->count_operations(State::Completed) == 0);
  CHECK(reactor->count_operations(State::Failed) == 0);

  reactor->process(ringu::Reactor::ProcessMode::Batch);
  CHECK(completed == 2);
  CHECK(reactor->count_operations(State::Pending) == 0);
  CHECK(reactor->count_operations(State::Completed) == 2);
  CHECK(reactor->count_operations(State::Failed) == 0);
}

TEST_CASE("cancel completes the operation with the Cancelled error code (a failure)") {
  auto reactor_result = ringu::Reactor::Create(ringu::ReactorOptions{.entries = 64});
  REQUIRE(reactor_result.has_value());
  auto reactor = std::move(reactor_result).value();
  using State = ringu::Reactor::OperationState;
  bool called = false;
  const ringu::OperationId id =
      reactor->submit(ringu::Timer{std::chrono::seconds(30)},
                      [&](ringu::Result<std::monostate> r, ringu::OperationId got_id, std::int32_t) {
                        called = true;
                        CHECK(got_id == id);
                        CHECK_FALSE(r.has_value());
                        CHECK(std::holds_alternative<ringu::Cancelled>(r.error()));
                      });
  CHECK(reactor->cancel(id));
  reactor->process(ringu::Reactor::ProcessMode::Batch);
  CHECK(called);
  CHECK(reactor->count_operations(State::Pending) == 0);
  CHECK(reactor->count_operations(State::Completed) == 0);
  CHECK(reactor->count_operations(State::Failed) == 1);
  // cancelling an unknown / already completed id returns false
  CHECK_FALSE(reactor->cancel(id));
  CHECK_FALSE(reactor->cancel(ringu::k_invalid_operation_id));
}

TEST_CASE("process(Forever) loops until request_stop interrupts it") {
  auto reactor_result = ringu::Reactor::Create(ringu::ReactorOptions{.entries = 64});
  REQUIRE(reactor_result.has_value());
  auto reactor = std::move(reactor_result).value();
  std::int32_t done = 0;
  constexpr std::int32_t k_target = 3;
  std::function<void(ringu::Result<std::monostate>, ringu::OperationId, std::int32_t)> cb;
  cb = [&](ringu::Result<std::monostate>, ringu::OperationId, std::int32_t) {
    done += 1;
    if (done < k_target) {
      reactor->submit(ringu::Timer{std::chrono::milliseconds(1)}, cb);
    } else {
      reactor->request_stop();
    }
  };
  reactor->submit(ringu::Timer{std::chrono::milliseconds(1)}, cb);
  reactor->process(ringu::Reactor::ProcessMode::Forever);
  CHECK(done == k_target);
  CHECK(reactor->count_operations(ringu::Reactor::OperationState::Completed) == k_target);
}

TEST_CASE("SQPOLL is enabled by default and can be disabled through ReactorOptions") {
  auto default_reactor_result = ringu::Reactor::Create(); // ReactorOptions{} default
  REQUIRE(default_reactor_result.has_value());
  auto default_reactor = std::move(default_reactor_result).value();
  CHECK(default_reactor->sqpoll_enabled());
  CHECK(default_reactor->sqpoll_poll_time() == std::chrono::milliseconds{2000});
  auto custom_entries_result = ringu::Reactor::Create(ringu::ReactorOptions{.entries = 64});
  REQUIRE(custom_entries_result.has_value());
  auto custom_entries = std::move(custom_entries_result).value();
  CHECK(custom_entries->sqpoll_enabled());
  auto no_sqpoll_result = ringu::Reactor::Create(ringu::ReactorOptions{.entries = 64, .sqpoll = false});
  REQUIRE(no_sqpoll_result.has_value());
  auto no_sqpoll = std::move(no_sqpoll_result).value();
  CHECK_FALSE(no_sqpoll->sqpoll_enabled());
  CHECK(no_sqpoll->sqpoll_poll_time() == std::chrono::milliseconds{0});
}

TEST_CASE("the SQPOLL polling time option is applied when setting up the ring") {
  auto reactor_result = ringu::Reactor::Create(
      ringu::ReactorOptions{.entries = 64, .sqpoll = true, .sqpoll_poll_time = std::chrono::milliseconds{500}});
  REQUIRE(reactor_result.has_value());
  auto reactor = std::move(reactor_result).value();
  REQUIRE(reactor->sqpoll_enabled());
  CHECK(reactor->sqpoll_poll_time() == std::chrono::milliseconds{500});
  // the ring still works with a custom poll time
  std::int32_t done = 0;
  reactor->submit(ringu::Timer{std::chrono::milliseconds(1)},
                  [&](ringu::Result<std::monostate> r, ringu::OperationId, std::int32_t) {
                    CHECK(r.has_value());
                    done += 1;
                  });
  reactor->process(ringu::Reactor::ProcessMode::Batch);
  CHECK(done == 1);
  CHECK(reactor->count_operations(ringu::Reactor::OperationState::Completed) == 1);
}

TEST_CASE("a batch of operations on an SQPOLL ring completes (kernel thread submits the SQEs)") {
  auto reactor_result = ringu::Reactor::Create(ringu::ReactorOptions{.entries = 64, .sqpoll = true});
  REQUIRE(reactor_result.has_value());
  auto reactor = std::move(reactor_result).value();
  REQUIRE(reactor->sqpoll_enabled());
  using State = ringu::Reactor::OperationState;
  std::int32_t done = 0;
  for (std::int32_t i = 0; i < 8; ++i) {
    reactor->submit(ringu::Timer{std::chrono::milliseconds(1)},
                    [&](ringu::Result<std::monostate> r, ringu::OperationId, std::int32_t) {
                      CHECK(r.has_value());
                      done += 1;
                    });
  }
  reactor->process(ringu::Reactor::ProcessMode::Batch);
  CHECK(done == 8);
  CHECK(reactor->count_operations(State::Pending) == 0);
  CHECK(reactor->count_operations(State::Completed) == 8);
  CHECK(reactor->count_operations(State::Failed) == 0);
}

TEST_CASE("operations on a non-SQPOLL ring still complete when SQPOLL is disabled") {
  auto reactor_result = ringu::Reactor::Create(ringu::ReactorOptions{.entries = 64, .sqpoll = false});
  REQUIRE(reactor_result.has_value());
  auto reactor = std::move(reactor_result).value();
  REQUIRE_FALSE(reactor->sqpoll_enabled());
  using State = ringu::Reactor::OperationState;
  std::int32_t done = 0;
  reactor->submit(ringu::Timer{std::chrono::milliseconds(1)},
                  [&](ringu::Result<std::monostate> r, ringu::OperationId, std::int32_t) {
                    CHECK(r.has_value());
                    done += 1;
                  });
  reactor->process(ringu::Reactor::ProcessMode::Batch);
  CHECK(done == 1);
  CHECK(reactor->count_operations(State::Completed) == 1);
}

TEST_CASE("process(Batch) with nothing pending is a no-op") {
  auto reactor_result = ringu::Reactor::Create(ringu::ReactorOptions{.entries = 64});
  REQUIRE(reactor_result.has_value());
  auto reactor = std::move(reactor_result).value();
  reactor->process(ringu::Reactor::ProcessMode::Batch);
  CHECK(reactor->count_operations(ringu::Reactor::OperationState::Pending) == 0);
  CHECK(reactor->count_operations(ringu::Reactor::OperationState::Completed) == 0);
}

TEST_CASE("callbacks may submit new operations from the completion handler") {
  auto reactor_result = ringu::Reactor::Create(ringu::ReactorOptions{.entries = 64});
  REQUIRE(reactor_result.has_value());
  auto reactor = std::move(reactor_result).value();
  std::int32_t stage = 0;
  reactor->submit(ringu::Timer{std::chrono::milliseconds(1)},
                  [&](ringu::Result<std::monostate> r, ringu::OperationId, std::int32_t) {
                    REQUIRE(r.has_value());
                    stage = 1;
                    reactor->submit(ringu::Timer{std::chrono::milliseconds(1)},
                                    [&](ringu::Result<std::monostate> r2, ringu::OperationId, std::int32_t) {
                                      REQUIRE(r2.has_value());
                                      stage = 2;
                                    });
                  });
  reactor->process(ringu::Reactor::ProcessMode::Batch);
  // the second timer was submitted from the callback: the batch drains it too
  CHECK(stage == 2);
  CHECK(reactor->count_operations(ringu::Reactor::OperationState::Completed) == 2);
  // a second batch with nothing pending is a no-op
  reactor->process(ringu::Reactor::ProcessMode::Batch);
  CHECK(stage == 2);
  CHECK(reactor->count_operations(ringu::Reactor::OperationState::Completed) == 2);
}
