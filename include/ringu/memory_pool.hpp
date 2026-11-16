#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <vector>

#include <tl/expected.hpp>

#include <ringu/contracts.hpp>
#include <ringu/error.hpp>

namespace ringu::detail {

// The io_uring ring wrapper. Defined in the library (src/ringu/ring.hpp), so no
// io_uring / Linux internal type appears in this public header.
class IoUring;

} // namespace ringu::detail

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

/// Memory pool of io_uring registered buffers, bound to a Ring.
///
/// The pool is created with Create(), passing a reference to the io_uring Ring
/// (see Reactor::ring()), and keeps that reference internally: every chunk it
/// hands out is registered with the ring as a fixed buffer
/// (IORING_REGISTER_BUFFERS), so read/write operations submitted with it take
/// the zero-copy READ_FIXED/WRITE_FIXED path (no per-operation page pinning).
///
/// The backing memory is allocated as whole pages with mmap() and distributed in
/// chunks. Every chunk starts on a cache-line boundary: the chunk stride is
/// buffer_size() rounded up to the cache line size. The booking (which chunks are
/// free) is kept internally to the pool.
///
/// borrow() hands out a RegisteredBuffer of buffer_size() bytes; destroying the
/// buffer returns it to the pool.
///
/// Invariant: the pool cannot be destroyed while a RegisteredBuffer is still
/// active. The active buffer counter is checked in the destructor and a violation
/// terminates the program.
///
/// The pool (and its buffers) are not thread safe.
class RegisteredMemoryPool {
public:
  /// Creates a pool of count fixed buffers of buffer_size bytes, registered on
  /// ring (e.g. Reactor::ring()). The constructor is private: use Create().
  ///
  /// Returns a RinguError (opcode + message) instead of throwing when the
  /// backing mmap(2) or the io_uring fixed-buffer registration fails.
  static tl::expected<std::unique_ptr<RegisteredMemoryPool>, RinguError>
  Create(detail::IoUring& ring, std::uint64_t buffer_size, std::uint64_t count,
         std::pmr::memory_resource* resource = std::pmr::get_default_resource());
  ~RegisteredMemoryPool();

  RegisteredMemoryPool(const RegisteredMemoryPool&) = delete;
  RegisteredMemoryPool& operator=(const RegisteredMemoryPool&) = delete;

  /// Borrows a buffer of buffer_size() bytes.
  /// Returns a RinguError (opcode MemoryPoolExhausted) when the pool is
  /// exhausted, instead of throwing.
  [[nodiscard]] tl::expected<RegisteredBuffer, RinguError> borrow();

  [[nodiscard]] std::uint64_t buffer_size() const { return buffer_size_; }
  [[nodiscard]] std::uint64_t capacity() const { return count_; }

  /// Number of buffers currently borrowed (the invariant counter).
  [[nodiscard]] std::uint64_t active_count() const { return active_count_; }

  /// Number of buffers available for borrowing.
  [[nodiscard]] std::uint64_t available() const { return count_ - active_count_; }

  /// Base address of the whole registered memory region (page aligned).
  [[nodiscard]] std::byte* base() const { return memory_; }

private:
  friend class RegisteredBuffer;
  void return_buffer(std::uint64_t index) noexcept;

  RegisteredMemoryPool(detail::IoUring& ring, std::uint64_t buffer_size, std::uint64_t count,
                       std::pmr::memory_resource* resource);
  /// Fallible part of the construction: maps the backing memory and registers
  /// every chunk with the ring. On failure returns the error and leaves the
  /// object in a destructible state (memory_ == nullptr).
  tl::expected<void, RinguError> setup();

  detail::IoUring& ring_; // the ring every chunk is registered with
  std::uint64_t buffer_size_;
  std::uint64_t count_;
  std::uint64_t chunk_size_ = 0;      // cache-line aligned stride between two chunks
  std::uint64_t cache_line_size_ = 0; // std::hardware_destructive_interference_size
  std::uint64_t page_size_ = 0;       // runtime page size (sysconf)
  std::uint64_t mapped_size_ = 0;     // size of the whole mmap'ed page range
  std::byte* memory_ = nullptr;
  std::pmr::memory_resource* resource_;
  std::pmr::vector<std::uint8_t> free_; // internal booking: 1 = free, 0 = borrowed
  std::uint64_t active_count_ = 0;
  std::uint64_t hint_ = 0;
};

} // namespace ringu
