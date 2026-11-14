#include <doctest/doctest.h>

#include <cstdint>
#include <stdexcept>

#include <ringu/contracts.hpp>

namespace {

constexpr std::int32_t checked_precondition(bool ok) {
  ringu::enforce_precondition(ok);
  return 42;
}

constexpr std::int32_t checked_invariant(bool ok) {
  ringu::enforce_invariant(ok);
  return 43;
}

// a passing precondition/invariant is usable in constant evaluation
static_assert(checked_precondition(true) == 42);
static_assert(checked_invariant(true) == 43);

} // namespace

TEST_CASE("enforce_precondition passes when the condition holds") {
  CHECK_NOTHROW(ringu::enforce_precondition(true));
  CHECK_NOTHROW(ringu::enforce_precondition(1 + 1 == 2));
}

TEST_CASE("enforce_precondition throws std::logic_error when the condition fails") {
  CHECK_THROWS_AS(ringu::enforce_precondition(false), std::logic_error);
  CHECK_THROWS_AS(ringu::enforce_precondition(1 + 1 == 3), std::logic_error);
}

TEST_CASE("enforce_invariant throws std::logic_error when the invariant fails") {
  CHECK_NOTHROW(ringu::enforce_invariant(true));
  CHECK_THROWS_AS(ringu::enforce_invariant(false), std::logic_error);
}

TEST_CASE("unreachable throws std::logic_error when executed at runtime") {
  CHECK_THROWS_AS(ringu::unreachable(), std::logic_error);
}
