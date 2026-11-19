#pragma once

#include <cstdint>

namespace dojio {

/// Strongly typed identifier of an operation submitted to a RingEngine.
///
/// Returned by RingEngine::submit and accepted by RingEngine::cancel.
struct OperationId {
  std::uint64_t value = 0;

  constexpr bool operator==(const OperationId& other) const = default;
};

/// Identifier that never matches a submitted operation.
inline constexpr OperationId k_invalid_operation_id{0};

} // namespace dojio
