#pragma once

// Private header: this is the only place where <linux/io_uring.h> is included.
// None of these types may leak into include/ringu/.

#include <cstddef>
#include <cstdint>
#include <linux/io_uring.h>
#include <memory>
#include <sys/uio.h>

#include <tl/expected.hpp>

#include <ringu/error.hpp>
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
  /// Creates the ring. entries: number of submission / completion queue entries
  /// (must be > 0 and <= 32768). setup_flags: io_uring_setup(2) flags (e.g.
  /// IORING_SETUP_SQPOLL). sq_thread_idle_ms: how long the SQPOLL kernel thread
  /// polls the submission queue before sleeping, in milliseconds
  /// (io_uring_params.sq_thread_idle); only applied when setup_flags contains
  /// IORING_SETUP_SQPOLL.
  ///
  /// Returns a RinguError (opcode + message) instead of throwing when the
  /// io_uring_setup(2) syscall or the ring mmap fails. The constructor is
  /// private: use Create().
  static tl::expected<std::unique_ptr<IoUring>, RinguError> Create(std::uint32_t entries, std::uint32_t setup_flags = 0,
                                                                   std::uint32_t sq_thread_idle_ms = 0);
  ~IoUring();

  IoUring(const IoUring&) = delete;
  IoUring& operator=(const IoUring&) = delete;

  /// File descriptor of the ring, usable with io_uring_register(2)-style calls.
  [[nodiscard]] std::int32_t fd() const { return fd_; }

  /// True when the ring was created with IORING_SETUP_SQPOLL: a kernel thread
  /// polls the submission queue, so SQEs are consumed without io_uring_enter(2).
  [[nodiscard]] bool sqpoll() const { return (params_.flags & IORING_SETUP_SQPOLL) != 0; }

  /// True when the SQPOLL kernel thread is sleeping (IORING_SQ_NEED_WAKEUP) and
  /// must be woken to observe newly published SQEs. Only meaningful with sqpoll().
  [[nodiscard]] bool sqpoll_needs_wakeup() const;

  /// Wakes the SQPOLL kernel thread (io_uring_enter with IORING_ENTER_SQ_WAKEUP).
  std::int32_t wake_sqpoll();

  [[nodiscard]] const io_uring_params& params() const { return params_; }
  [[nodiscard]] std::uint32_t sq_entries() const { return params_.sq_entries; }
  [[nodiscard]] std::uint32_t cq_entries() const { return params_.cq_entries; }

  /// Reserves a zeroed SQE slot and records it in the submission queue array,
  /// but does NOT publish it to the kernel yet: the SQ tail is only advanced by
  /// publish_sqes(). The slot must be fully written before publish_sqes() —
  /// with SQPOLL the kernel thread may consume a slot as soon as the tail moves.
  /// Returns nullptr when the submission queue is full.
  [[nodiscard]] io_uring_sqe* reserve_sqe();

  /// Publishes every reserved SQE to the kernel by advancing the SQ tail
  /// (release store, so the kernel sees the fully written SQE contents).
  void publish_sqes();

  /// Number of SQEs awaiting consumption by the kernel (reserved or published):
  /// the application tail minus the kernel-consumed head.
  [[nodiscard]] std::uint32_t pending_sqes() const;

  /// io_uring_enter(2) wrapper.
  std::int32_t enter(std::uint32_t to_submit, std::uint32_t min_complete, std::uint32_t flags);

  /// Returns the next completion event, or nullptr when the completion queue is empty.
  [[nodiscard]] const io_uring_cqe* peek_cqe() const;

  /// Marks n completion events as consumed by the application.
  void cq_advance(std::uint32_t n = 1);

private:
  IoUring() = default;

  /// Fallible setup: fills the members from io_uring_setup(2) + mmap(2). On
  /// failure returns the error and leaves the object in a destructible partial
  /// state (fd_ may be set while the mappings are not).
  tl::expected<void, RinguError> setup(std::uint32_t entries, std::uint32_t setup_flags,
                                       std::uint32_t sq_thread_idle_ms);

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
  std::uint32_t* sq_flags_ = nullptr;
  std::uint32_t sq_tail_shadow_ = 0; // application-side SQ tail (reserved slots included)
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
