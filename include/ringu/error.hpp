#pragma once

#include <cstdint>
#include <string>

namespace ringu {

/// Opcode of the ringu operation that failed, carried by a RinguError.
enum class ErrorOpcode : std::uint32_t {
  IoUringSetup = 1,              ///< the io_uring_setup(2) syscall failed
  IoUringMmap = 2,               ///< mapping the submission / completion rings (mmap(2)) failed
  MemoryPoolMmap = 3,            ///< mmap(2) of the pool backing memory failed
  MemoryPoolRegisterBuffers = 4, ///< io_uring_register(IORING_REGISTER_BUFFERS) failed
  MemoryPoolExhausted = 5,       ///< borrow() on an exhausted pool
};

/// Error reported by the fallible operations of the library (e.g.
/// RingEngine::create, RegisteredMemoryPool::create, RegisteredMemoryPool::borrow)
/// through a tl::expected: the opcode of the failing operation and a
/// human-readable message (including the errno description).
struct RinguError {
  ErrorOpcode opcode = ErrorOpcode::IoUringSetup;
  std::string message;

  bool operator==(const RinguError& other) const = default;
};

} // namespace ringu
