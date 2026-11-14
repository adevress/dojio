#pragma once

// Private header: this is the only place where <linux/io_uring.h> is included.
// None of these types may leak into include/ringu/.

#include <cstddef>
#include <cstdint>
#include <linux/io_uring.h>
#include <sys/uio.h>

#include <ringu/sqe.hpp>

namespace ringu::detail {

/// Copies a ringu Sqe mirror onto a real kernel SQE.
void copy_sqe(io_uring_sqe& dst, const Sqe& src);

/// Thin RAII wrapper over the Linux io_uring kernel API, implemented directly on
/// the io_uring_setup(2) / io_uring_enter(2) / io_uring_register(2) syscalls with
/// the submission and completion rings mapped into memory.
///
/// Not thread safe.
class IoUring {
public:
  explicit IoUring(std::uint32_t entries, std::uint32_t setup_flags = 0);
  ~IoUring();

  IoUring(const IoUring&) = delete;
  IoUring& operator=(const IoUring&) = delete;

  /// File descriptor of the ring, usable with io_uring_register(2)-style calls.
  [[nodiscard]] std::int32_t fd() const { return fd_; }

  [[nodiscard]] const io_uring_params& params() const { return params_; }
  [[nodiscard]] std::uint32_t sq_entries() const { return params_.sq_entries; }
  [[nodiscard]] std::uint32_t cq_entries() const { return params_.cq_entries; }

  /// Returns a zeroed SQE slot and publishes it on the submission queue tail,
  /// or nullptr when the submission queue is full.
  [[nodiscard]] io_uring_sqe* get_sqe();

  /// Number of SQEs ready to be submitted to the kernel (tail - head).
  [[nodiscard]] std::uint32_t pending_sqes() const;

  /// io_uring_enter(2) wrapper.
  std::int32_t enter(std::uint32_t to_submit, std::uint32_t min_complete, std::uint32_t flags);

  /// Returns the next completion event, or nullptr when the completion queue is empty.
  [[nodiscard]] const io_uring_cqe* peek_cqe() const;

  /// Marks n completion events as consumed by the application.
  void cq_advance(std::uint32_t n = 1);

private:
  std::int32_t fd_ = -1;
  io_uring_params params_{};
  void* sq_ring_ = nullptr;
  void* cq_ring_ = nullptr;
  io_uring_sqe* sqes_ = nullptr;
  std::uint64_t sq_ring_size_ = 0;
  std::uint64_t cq_ring_size_ = 0;
  std::uint32_t* sq_head_ = nullptr;
  std::uint32_t* sq_tail_ = nullptr;
  std::uint32_t* sq_array_ = nullptr;
  std::uint32_t* cq_head_ = nullptr;
  std::uint32_t* cq_tail_ = nullptr;
  io_uring_cqe* cqes_ = nullptr;
  std::uint32_t sq_mask_ = 0;
  std::uint32_t cq_mask_ = 0;
};

/// io_uring_register(IORING_REGISTER_BUFFERS, ...) wrapper: registers nr fixed buffers.
std::int32_t register_buffers(std::int32_t ring_fd, const iovec* iovecs, std::uint32_t nr);

/// io_uring_register(IORING_UNREGISTER_BUFFERS, ...) wrapper.
std::int32_t unregister_buffers(std::int32_t ring_fd);

} // namespace ringu::detail
