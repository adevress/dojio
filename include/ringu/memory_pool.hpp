#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <vector>

#include <ringu/contracts.hpp>

namespace ringu {

class RegisteredMemoryPool;

/// A buffer borrowed from a RegisteredMemoryPool.
///
/// The memory backing a RegisteredBuffer is registered once with io_uring as a
/// fixed buffer, so read/write operations submitted with it take the zero-copy
/// READ_FIXED/WRITE_FIXED path (no per-operation page pinning).
///
/// Destroying the buffer returns it to its pool.
/// Neither the buffer nor the pool are thread safe.
class RegisteredBuffer {
public:
  using value_type = std::byte;

  RegisteredBuffer() = default;
  RegisteredBuffer(RegisteredMemoryPool& pool, std::uint64_t index);
  RegisteredBuffer(RegisteredBuffer&& other) noexcept;
  RegisteredBuffer& operator=(RegisteredBuffer&& other) noexcept;
  RegisteredBuffer(const RegisteredBuffer&) = delete;
  RegisteredBuffer& operator=(const RegisteredBuffer&) = delete;
  ~RegisteredBuffer();

  /// Data pointer of the buffer. Precondition: the buffer is not empty (not moved-from).
  [[nodiscard]] std::byte* data() const;
  /// Size of the buffer (0 for an empty / moved-from buffer).
  [[nodiscard]] std::uint64_t size() const;

  /// Index of the buffer in the io_uring fixed buffer table.
  [[nodiscard]] std::uint16_t index() const { return static_cast<std::uint16_t>(index_); }

  [[nodiscard]] explicit operator bool() const { return pool_ != nullptr; }

private:
  void release() noexcept;

  RegisteredMemoryPool* pool_ = nullptr;
  std::uint64_t index_ = 0;
};

/// Factory acting as a memory pool of io_uring registered buffers.
///
/// borrow() hands out a RegisteredBuffer of buffer_size() bytes; destroying the
/// buffer returns it to the pool. The whole memory region is registered once with
/// the given io_uring (see Reactor::ring_fd()).
///
/// Invariant: the pool cannot be destroyed while a RegisteredBuffer is still
/// active. The active buffer counter is checked in the destructor and a violation
/// terminates the program.
///
/// The pool (and its buffers) are not thread safe.
class RegisteredMemoryPool {
public:
  RegisteredMemoryPool(std::int32_t ring_fd, std::uint64_t buffer_size, std::uint64_t count,
                       std::pmr::memory_resource* resource = std::pmr::get_default_resource());
  ~RegisteredMemoryPool();

  RegisteredMemoryPool(const RegisteredMemoryPool&) = delete;
  RegisteredMemoryPool& operator=(const RegisteredMemoryPool&) = delete;

  /// Borrows a buffer of buffer_size() bytes.
  /// Throws std::runtime_error when the pool is exhausted.
  [[nodiscard]] RegisteredBuffer borrow();

  [[nodiscard]] std::uint64_t buffer_size() const { return buffer_size_; }
  [[nodiscard]] std::uint64_t capacity() const { return count_; }

  /// Number of buffers currently borrowed (the invariant counter).
  [[nodiscard]] std::uint64_t active_count() const { return active_count_; }

  /// Number of buffers available for borrowing.
  [[nodiscard]] std::uint64_t available() const { return count_ - active_count_; }

  [[nodiscard]] std::int32_t ring_fd() const { return ring_fd_; }

  /// Base address of the whole registered memory region.
  [[nodiscard]] std::byte* base() const { return memory_; }

private:
  friend class RegisteredBuffer;
  void return_buffer(std::uint64_t index) noexcept;

  std::int32_t ring_fd_;
  std::uint64_t buffer_size_;
  std::uint64_t count_;
  std::byte* memory_ = nullptr;
  std::pmr::memory_resource* resource_;
  std::pmr::vector<std::uint8_t> free_;
  std::uint64_t active_count_ = 0;
  std::uint64_t hint_ = 0;
};

} // namespace ringu
