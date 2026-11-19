#pragma once

#include <cstdint>

namespace dojio {

/// Typed handler for a file descriptor.
///
/// FileDescriptor is a strong, non-owning handle: it never closes the descriptor
/// on destruction. Ownership stays with the caller unless the descriptor is
/// explicitly submitted to a Close operation.
class FileDescriptor {
public:
  constexpr FileDescriptor() = default;
  constexpr explicit FileDescriptor(std::int32_t fd) : fd_(fd) {}

  /// Raw descriptor value.
  [[nodiscard]] constexpr std::int32_t value() const { return fd_; }

  /// True when the handle wraps a valid (non negative) descriptor.
  [[nodiscard]] constexpr explicit operator bool() const { return fd_ >= 0; }

  constexpr bool operator==(const FileDescriptor& other) const = default;

private:
  std::int32_t fd_ = -1;
};

} // namespace dojio
