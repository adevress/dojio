#pragma once

#include <cstdlib>
#include <exception>
#include <source_location>
#include <string_view>
#include <type_traits>

// Contracts for dojio, adapted from arcane-lang's utils/contracts.hpp.
//
// enforce_precondition / enforce_invariant check a condition at runtime (or during
// constant evaluation) and, on failure, log the source location and throw a
// std::logic_error. In a constant expression the check fails compilation instead.

namespace dojio {
namespace intern {

/// Logs a contract failure at the given location and throws a std::logic_error.
[[noreturn]] void log_and_throw(const std::source_location& loc);

// Ugly hack to enforce a fail method that is properly supported within constexpr in C++20
// in C++20:
// - cassert can not be used in a constexpr method
// - static_assert fails to evaluate properly in if constexpr statement on some compiler
// - std::abort is not properly marked as constexpr

/// Aborts a constexpr evaluation or a runtime execution at the given location.
[[noreturn]] constexpr void compile_time_enforce(const std::source_location&) {
  // std abort is somewhat tolerated in constant evaluated expression by GCC and clang
#if (defined __GNUG__) && !(defined __clang__)
  std::abort();
#else
  // make no damn sense
  // but satisfy the consteval system of clang
  if (std::is_constant_evaluated()) {
    while (true) {
      // cppcheck-suppress misra-cpp-2023-9.2.1 ; MISRA C++ deviation: void cast keeps the consteval loop non-empty for
      // clang
      static_cast<void>(1);
    }
  } else {
    std::abort();
  }
#endif
}

} // namespace intern

/// Enforces a runtime (or constexpr) precondition, throwing on failure.
constexpr void enforce_precondition(bool condition,
                                    const std::source_location location = std::source_location::current()) {
  if (!condition) [[unlikely]] {
    if (std::is_constant_evaluated()) {
      intern::compile_time_enforce(location);
    } else {
      intern::log_and_throw(location);
    }
  }
}

/// Enforces a runtime (or constexpr) invariant, throwing on failure.
/// Intended for class invariants (checked e.g. in constructors and destructors);
/// a violation is a bug, and in a noexcept context (like a destructor) the thrown
/// std::logic_error turns into std::terminate.
constexpr void enforce_invariant(bool condition,
                                 const std::source_location location = std::source_location::current()) {
  if (!condition) [[unlikely]] {
    if (std::is_constant_evaluated()) {
      intern::compile_time_enforce(location);
    } else {
      intern::log_and_throw(location);
    }
  }
}

/// Marks a code path as unreachable, throwing if it is executed.
[[noreturn]] constexpr void unreachable(const std::source_location location = std::source_location::current()) {
  if (std::is_constant_evaluated()) {
    intern::compile_time_enforce(location);
  } else {
    intern::log_and_throw(location);
  }
}

} // namespace dojio
