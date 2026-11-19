#pragma once

// Private header used on non-Linux platforms: it stands in for the Linux-only
// ring.hpp so the pimpl member of RingEngine stays a complete type. None of the
// ringu public headers include it.

namespace ringu::detail {

/// Stub of the io_uring ring wrapper. io_uring only exists on Linux, so the ring
/// is never created and every method of this class is unreachable.
class IoUring {};

} // namespace ringu::detail
