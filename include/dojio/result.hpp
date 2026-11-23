#pragma once

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <tl/expected.hpp>

#include <dojio/error.hpp>

namespace dojio {

/// An I/O error code as reported by the kernel through an io_uring completion event.
///
/// The value is the raw result of the CQE, i.e. the negated errno (e.g. -ENOENT).
struct IoError {
  std::int32_t value = 0;

  /// Human readable description of the mapped I/O code (implemented in the library).
  [[nodiscard]] std::string message() const;

  bool operator==(const IoError& other) const = default;
};

/// Tag carried by the ErrorCode variant when an operation was cancelled.
struct Cancelled {
  bool operator==(const Cancelled& other) const = default;
};

/// Error code of a failed operation: either the mapped I/O code or the cancel code.
using ErrorCode = std::variant<IoError, Cancelled>;

/// Result of a completed action: the registered item (often the associated buffer)
/// on success, an ErrorCode on failure.
template <typename T> using Result = tl::expected<T, ErrorCode>;

namespace detail {

/// Human readable description of an ErrorCode ("operation cancelled" for Cancelled).
[[nodiscard]] std::string describe(const ErrorCode& error);

/// Prints "{message}: {detail}" on stderr, then exits with EXIT_FAILURE.
/// Implemented in the library, which owns the error formatting.
[[noreturn]] void fail(std::string_view message, std::string_view detail);

} // namespace detail

/// Returns the value held by result, or prints message followed by the error
/// description on stderr and exits with EXIT_FAILURE.
///
/// Convenience for applications (and demos) whose only recovery from a failed
/// operation is to report it and stop: it removes the repetitive
/// "if (!r.has_value()) { print(r.error()); std::exit(1); }" boilerplate.
template <typename T> T result_or_fail(Result<T> result, std::string_view message) {
  if (!result.has_value()) {
    detail::fail(message, detail::describe(result.error()));
  }
  return std::move(result).value();
}

/// Same as above for the fallible creation functions of the library
/// (RingEngine::create, RegisteredMemoryPool::create / borrow), which report a
/// DojioError (opcode + message) instead of an ErrorCode.
template <typename T> T result_or_fail(tl::expected<T, DojioError> result, std::string_view message) {
  if (!result.has_value()) {
    detail::fail(message, result.error().message);
  }
  return std::move(result).value();
}

} // namespace dojio
