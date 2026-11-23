#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include <tl/expected.hpp>

#include <dojio/error.hpp>

namespace dojio::detail {

// The io_uring ring wrapper. Defined in the library (src/dojio/ring.hpp), so no
// io_uring / Linux internal type appears in this public header.
class IoUring;

} // namespace dojio::detail

namespace dojio {

/// Cache line size used to align the pool chunks (64 bytes).
[[nodiscard]] std::uint64_t cache_line_size() noexcept;

/// Page size of the running system (sysconf(_SC_PAGESIZE)), cached in a function-local
/// static variable (thread-safe C++11 initialization); the returned value is stable for
/// the lifetime of the process. Falls back to 4096 when sysconf(3) fails.
[[nodiscard]] std::uint64_t page_size() noexcept;

class RegisteredBuffer;

/// Memory pool of io_uring registered buffers.
///
/// The backing memory is allocated as whole pages with mmap() and distributed in
/// chunks. On Linux every chunk is registered once with the io_uring instance
/// (IORING_REGISTER_BUFFERS), so read/write operations submitted with it take the
/// zero-copy READ_FIXED/WRITE_FIXED path (no per-operation page pinning). On
/// non-Linux platforms (no io_uring), the pool maps the same chunks but performs no
/// registration and ignores the ring: the chunks are then plain mmap'ed memory.
///
/// create() duplicates the ring file descriptor (close-on-exec): the pool holds its
/// own reference on the io_uring instance and closes it in its destructor, so the
/// fixed buffers stay registered even if the ring object is destroyed first.
///
/// Every chunk starts on a cache-line boundary: the chunk stride is buffer_size()
/// rounded up to the cache line size. The booking (which chunks are free) is kept as
/// a free-index stack, so borrow() and the buffer release are O(1).
///
/// borrow() hands out a RegisteredBuffer of buffer_size() bytes; destroying the
/// buffer returns it to the pool.
///
/// The pool is a movable pimpl handle: moving it transfers the ownership of the
/// mapping, of the duplicated ring fd and of the fixed buffers. The buffers borrowed
/// before a move stay valid, since they reference the pool implementation, not the
/// handle. Copies are deleted.
///
/// Invariant: the pool cannot be destroyed while a RegisteredBuffer is still
/// active: the implementation destructor checks that every chunk was returned and
/// terminates the program otherwise. A moved-from pool owns nothing and must not be
/// used any more.
///
/// The pool (and its buffers) are not thread safe.
class RegisteredMemoryPool {
public:
  /// Creates a pool of count fixed buffers of buffer_size bytes, registered on
  /// ring (e.g. RingEngine::ring()). The constructor is private: use create().
  ///
  /// Returns a DojioError (opcode + message) instead of throwing when the ring file
  /// descriptor duplication, the backing mmap(2) or the io_uring fixed-buffer
  /// registration fails. Nothing is left registered on the ring in that case.
  /// On non-Linux platforms the ring file descriptor duplication and the
  /// registration are skipped (and cannot fail).
  static tl::expected<RegisteredMemoryPool, DojioError> create(detail::IoUring& ring, std::uint64_t buffer_size,
                                                               std::uint64_t count);

  RegisteredMemoryPool(RegisteredMemoryPool&& other) noexcept;
  RegisteredMemoryPool& operator=(RegisteredMemoryPool&& other) noexcept;
  RegisteredMemoryPool(const RegisteredMemoryPool&) = delete;
  RegisteredMemoryPool& operator=(const RegisteredMemoryPool&) = delete;
  ~RegisteredMemoryPool();

  /// Borrows a buffer of buffer_size() bytes.
  /// Returns a DojioError (opcode MemoryPoolExhausted) when the pool is
  /// exhausted, instead of throwing.
  [[nodiscard]] tl::expected<RegisteredBuffer, DojioError> borrow();

  [[nodiscard]] std::uint64_t buffer_size() const noexcept;

  [[nodiscard]] std::uint64_t capacity() const noexcept;

  /// Number of buffers currently borrowed.
  [[nodiscard]] std::uint64_t active_count() const noexcept;

  /// Number of buffers available for borrowing.
  [[nodiscard]] std::uint64_t available() const noexcept;

  /// Base address of the whole registered memory region (page aligned).
  [[nodiscard]] std::byte* base() const noexcept;

private:
  friend class RegisteredBuffer;

  /// Implementation of the pool (pimpl): owns the mmap'ed memory, the duplicated ring
  /// fd and the free-index stack, and checks the active buffer invariant when it is
  /// destroyed. Defined in the library (src/dojio/memory_pool.cpp).
  class Impl;

  explicit RegisteredMemoryPool(std::unique_ptr<Impl> impl) noexcept;

  /// The pool implementation. Precondition: the pool is not moved-from.
  [[nodiscard]] Impl& impl() const noexcept;

  std::unique_ptr<Impl> impl_;
};

/// A buffer borrowed from a RegisteredMemoryPool.
///
/// The memory backing a RegisteredBuffer is registered once with the io_uring
/// instance as a fixed buffer, so read/write operations submitted with it take the
/// zero-copy READ_FIXED/WRITE_FIXED path (no per-operation page pinning).
///
/// A buffer starts empty (size() == 0) and can hold up to capacity() bytes, the size
/// of its pool chunk: append() fills it and checks the capacity, size() reports the
/// number of bytes it currently holds (the length a send writes). The length is also
/// set to the received byte count after a read/recv completion.
///
/// Destroying the buffer returns it to its pool.
/// Neither the buffer nor the pool are thread safe.
class RegisteredBuffer {
public:
  using value_type = std::byte;

  RegisteredBuffer() = default;
  RegisteredBuffer(RegisteredBuffer&& other) noexcept;
  RegisteredBuffer& operator=(RegisteredBuffer&& other) noexcept;
  RegisteredBuffer(const RegisteredBuffer&) = delete;
  RegisteredBuffer& operator=(const RegisteredBuffer&) = delete;
  ~RegisteredBuffer();

  /// Data pointer of the buffer. Precondition: the buffer is not empty (not moved-from).
  [[nodiscard]] std::byte* data() const;
  /// Number of bytes currently in the buffer: the total length appended to it, or the
  /// number of bytes received into it (0 for an empty / moved-from buffer).
  [[nodiscard]] std::uint64_t size() const;
  /// Maximum number of bytes the buffer can hold (the size of its pool chunk).
  [[nodiscard]] std::uint64_t capacity() const;

  /// Appends bytes at the end of the buffer.
  /// Precondition: the buffer is not empty and size() + bytes.size() <= capacity().
  void append(std::span<const std::byte> bytes);
  /// Appends text at the end of the buffer (same capacity precondition).
  void append(std::string_view text);

  /// Sets the number of bytes held by the buffer, e.g. the length of a received
  /// payload. The content is left untouched.
  /// Precondition: size <= capacity().
  void resize(std::uint64_t size);

  /// Resets the number of bytes held by the buffer to 0, so that the next append()
  /// starts at the beginning of the chunk. The content is left untouched.
  void clear();

  /// Index of the buffer in the io_uring fixed buffer table.
  [[nodiscard]] std::uint16_t index() const noexcept { return static_cast<std::uint16_t>(index_); }

  [[nodiscard]] explicit operator bool() const { return impl_ != nullptr; }

private:
  friend class RegisteredMemoryPool;

  RegisteredBuffer(RegisteredMemoryPool::Impl& impl, std::uint64_t index) noexcept;

  void release() noexcept;

  RegisteredMemoryPool::Impl* impl_ = nullptr;
  std::uint64_t index_ = 0;
  std::uint64_t length_ = 0;
};

} // namespace dojio
