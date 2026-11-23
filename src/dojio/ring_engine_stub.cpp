// Non-Linux stub of the RingEngine: io_uring only exists on Linux, so create()
// always fails with a NotSupported DojioError and no engine can be instantiated.

#include <dojio/ring_engine.hpp>

#include <dojio/contracts.hpp>

#include <fmt/format.h>

#include <string_view>

#include <dojio/io_uring_stub.hpp>

namespace dojio {

namespace {

DojioError not_supported_error(std::string_view what) {
  return DojioError{ErrorOpcode::NotSupported,
                    fmt::format("dojio: {} is not supported on this platform (io_uring requires Linux)", what)};
}

} // namespace

tl::expected<std::unique_ptr<RingEngine>, DojioError> RingEngine::create(RingEngineOptions) {
  return tl::unexpected(not_supported_error("RingEngine"));
}

RingEngine::RingEngine(std::unique_ptr<detail::IoUring> ring) : ring_(std::move(ring)) {}

RingEngine::~RingEngine() = default;

void RingEngine::enqueue_action(OperationId, ActionBase&) { unreachable(); }

bool RingEngine::cancel(OperationId) {
  unreachable();
  // cppcheck-suppress unreachableCode ; MISRA C++ deviation (Rule 0.0.1): defensive return after unreachable()
  return false;
}

std::uint64_t RingEngine::process(ProcessMode, std::uint64_t) {
  unreachable();
  // cppcheck-suppress unreachableCode ; MISRA C++ deviation (Rule 0.0.1): defensive return after unreachable()
  return 0;
}

std::uint64_t RingEngine::poll(std::uint64_t) {
  unreachable();
  // cppcheck-suppress unreachableCode ; MISRA C++ deviation (Rule 0.0.1): defensive return after unreachable()
  return 0;
}

bool RingEngine::supports_op(IoOp) const noexcept {
  unreachable();
  // cppcheck-suppress unreachableCode ; MISRA C++ deviation (Rule 0.0.1): defensive return after unreachable()
  return false;
}

OpCapabilities RingEngine::capabilities() const noexcept {
  unreachable();
  // cppcheck-suppress unreachableCode ; MISRA C++ deviation (Rule 0.0.1): defensive return after unreachable()
  return OpCapabilities{};
}

std::uint64_t RingEngine::count_operations(OperationState) const {
  unreachable();
  // cppcheck-suppress unreachableCode ; MISRA C++ deviation (Rule 0.0.1): defensive return after unreachable()
  return 0;
}

std::int32_t RingEngine::ring_fd() const {
  unreachable();
  // cppcheck-suppress unreachableCode ; MISRA C++ deviation (Rule 0.0.1): defensive return after unreachable()
  return -1;
}

bool RingEngine::sqpoll_enabled() const {
  unreachable();
  // cppcheck-suppress unreachableCode ; MISRA C++ deviation (Rule 0.0.1): defensive return after unreachable()
  return false;
}

std::chrono::milliseconds RingEngine::sqpoll_poll_time() const {
  unreachable();
  // cppcheck-suppress unreachableCode ; MISRA C++ deviation (Rule 0.0.1): defensive return after unreachable()
  return std::chrono::milliseconds{0};
}

} // namespace dojio
