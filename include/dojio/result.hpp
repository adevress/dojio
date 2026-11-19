#pragma once

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <variant>

#include <tl/expected.hpp>

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

} // namespace dojio
