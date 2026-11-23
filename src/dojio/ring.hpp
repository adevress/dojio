#pragma once

// Private header: <linux/io_uring.h> is only included here and in memory_pool.cpp
// (which registers the pool chunks as fixed buffers). None of these types may leak
// into include/dojio/.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <linux/io_uring.h>
#include <memory>
#include <memory_resource>
#include <utility>

#include <tl/expected.hpp>

#include <dojio/error.hpp>
#include <dojio/sqe.hpp>

namespace dojio::detail {

/// Copies a dojio Sqe mirror onto a real kernel SQE.
void copy_sqe(io_uring_sqe& dst, const Sqe& src);

/// Combines flag constants as unsigned values (MISRA Rule 7.0.4) and returns the
/// result as the int expected by the C API.
template <typename... Flags> [[nodiscard]] constexpr int combine_flags(Flags... flags) noexcept {
  return static_cast<int>((static_cast<unsigned int>(flags) | ...));
}

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
  /// Returns a DojioError (opcode + message) instead of throwing when the
  /// io_uring_setup(2) syscall or the ring mmap fails. The constructor is
  /// private: use create().
  static tl::expected<std::unique_ptr<IoUring>, DojioError> create(std::uint32_t entries, std::uint32_t setup_flags = 0,
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

  /// True when the running kernel exposes the given io_uring opcode, as reported
  /// by IORING_REGISTER_PROBE when the ring was created. False when the probe
  /// could not be run (older kernels) or the opcode is unavailable.
  [[nodiscard]] bool supports_op(IoOp op) const noexcept;

  /// The opcodes of the syscall-backed operations (bind/listen, ftruncate, pipe)
  /// the running kernel exposes; the matching operations stay inline otherwise.
  [[nodiscard]] OpCapabilities capabilities() const noexcept;

  /// True when the completion queue overflowed (IORING_SQ_CQ_OVERFLOW): the next
  /// io_uring_enter(2) must pass IORING_ENTER_GETEVENTS to flush the overflowed
  /// completions into the CQ ring.
  [[nodiscard]] bool cq_overflow() const noexcept;

  /// Pool owned by the ring and used by the RingEngine to allocate its Action
  /// objects. Its largest_required_pool_block is max_size_action and its
  /// max_blocks_per_chunk is the ring size (RingEngineOptions::entries).
  [[nodiscard]] std::pmr::unsynchronized_pool_resource& action_pool() noexcept { return action_pool_; }

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

  /// Consumes every completion event currently in the ring, invoking fn(user_data,
  /// res, flags) for each, and advances the completion queue head once per batch
  /// (a single release store instead of one per completion). Returns the number of
  /// completions consumed. fn must not advance the CQ head itself.
  template <typename Fn> std::uint32_t drain_cqes(Fn&& fn) {
    std::uint32_t head = std::atomic_ref<std::uint32_t>(*cq_head_).load(std::memory_order_relaxed);
    const std::uint32_t tail = std::atomic_ref<std::uint32_t>(*cq_tail_).load(std::memory_order_acquire);
    std::uint32_t count = 0;
    while (head != tail) {
      const io_uring_cqe& cqe = cqes_[head & cq_mask_];
      fn(cqe.user_data, cqe.res, cqe.flags);
      head += 1;
      count += 1;
    }
    if (count > 0) {
      std::atomic_ref<std::uint32_t>(*cq_head_).store(head, std::memory_order_release);
    }
    return count;
  }

private:
  explicit IoUring(std::pmr::pool_options action_pool_options);

  /// Fallible setup: fills the members from io_uring_setup(2) + mmap(2). On
  /// failure returns the error and leaves the object in a destructible partial
  /// state (fd_ may be set while the mappings are not).
  tl::expected<void, DojioError> setup(std::uint32_t entries, std::uint32_t setup_flags,
                                       std::uint32_t sq_thread_idle_ms);

  /// Queries IORING_REGISTER_PROBE and fills supported_ops_. Best effort: when the
  /// kernel does not implement the probe (pre-5.6) or the call fails, no opcode is
  /// marked as supported and the syscall-backed operations fall back inline.
  void probe_opcodes();

  // the action pool is declared first so it is destroyed last: the BoxPtrs of the
  // RingEngine pending map may deallocate into it while the engine is torn down
  std::pmr::unsynchronized_pool_resource action_pool_;
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
  std::array<bool, 256> supported_ops_{};
};

} // namespace dojio::detail
