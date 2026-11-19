// Non-Linux stub of the RingEngine: io_uring only exists on Linux, so create()
// always fails with a NotSupported RinguError and no engine can be instantiated.

#include <ringu/ring_engine.hpp>

#include <ringu/contracts.hpp>

#include <fmt/format.h>

#include <string_view>

#include <ringu/io_uring_stub.hpp>

namespace ringu {

namespace {

RinguError not_supported_error(std::string_view what) {
  return RinguError{ErrorOpcode::NotSupported,
                    fmt::format("ringu: {} is not supported on this platform (io_uring requires Linux)", what)};
}

} // namespace

tl::expected<std::unique_ptr<RingEngine>, RinguError> RingEngine::create(RingEngineOptions options) {
  (void)options;
  return tl::unexpected(not_supported_error("RingEngine"));
}

RingEngine::RingEngine(std::unique_ptr<detail::IoUring> ring) : ring_(std::move(ring)), pending_(&pool_resource_) {}

RingEngine::~RingEngine() = default;

bool RingEngine::cancel(OperationId id) {
  (void)id;
  unreachable();
  return false; // never reached: unreachable() is [[noreturn]]
}

void RingEngine::process(ProcessMode mode) {
  (void)mode;
  unreachable();
}

std::uint64_t RingEngine::count_operations(OperationState state) const {
  (void)state;
  unreachable();
  return 0; // never reached: unreachable() is [[noreturn]]
}

std::int32_t RingEngine::ring_fd() const {
  unreachable();
  return -1; // never reached: unreachable() is [[noreturn]]
}

bool RingEngine::sqpoll_enabled() const {
  unreachable();
  return false; // never reached: unreachable() is [[noreturn]]
}

std::chrono::milliseconds RingEngine::sqpoll_poll_time() const {
  unreachable();
  return std::chrono::milliseconds{0}; // never reached: unreachable() is [[noreturn]]
}

} // namespace ringu
