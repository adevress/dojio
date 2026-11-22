#pragma once

#include <chrono>

namespace dojio {

/// Typed handler for a timer request.
///
/// Submitting a Timer to a RingEngine arms an io_uring timeout: the associated
/// action completes successfully once the duration has elapsed.
class Timer {
public:
  using Duration = std::chrono::nanoseconds;

  constexpr Timer() = default;

  template <typename Rep, typename Period>
  constexpr explicit Timer(std::chrono::duration<Rep, Period> duration)
      : duration_(std::chrono::duration_cast<Duration>(duration)) {}

  /// Requested timeout.
  [[nodiscard]] constexpr Duration duration() const noexcept { return duration_; }

private:
  Duration duration_{};
};

} // namespace dojio
