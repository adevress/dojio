#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <memory_resource>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <tl/expected.hpp>

#include <dojio/action.hpp>
#include <dojio/contracts.hpp>
#include <dojio/dojio_memory.hpp>
#include <dojio/error.hpp>
#include <dojio/operation_id.hpp>
#include <dojio/operations.hpp>
#include <dojio/slot_table.hpp>
#include <dojio/timer.hpp>

namespace dojio::detail {

// The io_uring ring wrapper. Defined in the library (src/dojio/ring.hpp) and held
// through a pimpl, so no io_uring / Linux internal type appears in this header.
class IoUring;

} // namespace dojio::detail

namespace dojio {

/// Options controlling how a RingEngine's underlying io_uring instance is set up.
struct RingEngineOptions {
  /// Number of entries of the submission / completion rings.
  std::uint32_t entries = 1024;
  /// Create the ring with IORING_SETUP_SQPOLL: a kernel thread polls the
  /// submission queue, so submitting operations does not require an
  /// io_uring_enter(2) syscall per batch (lower submission latency and CPU
  /// usage). Disabled by default; requires a kernel and privileges allowing
  /// SQPOLL.
  bool sqpoll = false;
  /// How long the SQPOLL kernel thread keeps polling the submission queue for
  /// new SQEs before going to sleep (io_uring_params.sq_thread_idle), in
  /// milliseconds. Only used when sqpoll is enabled. Default: 2000 ms.
  std::chrono::milliseconds sqpoll_poll_time{2000};
};

/// The RingEngine submits I/O operations, their associated buffer and their completion
/// handler to a Linux io_uring instance, and dispatches the completions.
///
/// NOT THREAD SAFE: a RingEngine, and every object it hands out (operation ids, buffers
/// borrowed from a pool created on it), must be used from a single thread. For
/// multi-threaded share-nothing designs, create one RingEngine per thread (see the
/// demo_tcp_hello demo).
///
/// Usage: auto engine = RingEngine::create(RingEngineOptions{...}); check the returned
/// tl::expected, then submit() registers an operation straight onto the underlying
/// io_uring submission queue (proactively publishing on SQPOLL rings, or deferring to
/// an internal queue only for inline operations or when the SQ ring is full) and
/// returns a typed OperationId; process() / poll() drives submission, completion
/// reaping and callback dispatch.
class RingEngine {
public:
  /// Selects how process() behaves.
  enum class ProcessMode : std::uint8_t {
    Batch,       ///< process every queued operation (ring and inline) until none is left
    Forever,     ///< like Batch, but returns early when request_stop() is called
    NonBlocking, ///< submit queued ops and dispatch only already-ready completions without blocking
    WaitOnce,    ///< submit queued ops, wait for at least one completion if any are pending, dispatch and return
  };

  /// Selects which operations count_operations reports.
  enum class OperationState : std::uint8_t {
    Pending,   ///< waiting in the pool (submitted but not completed yet)
    Completed, ///< successfully processed
    Failed     ///< failed (an I/O error code, or a cancellation)
  };

  /// Creates a RingEngine with the given options (SQPOLL disabled by default,
  /// 2000 ms poll time when enabled). The constructor is private: use create().
  ///
  /// Returns a DojioError (opcode + message) when the underlying io_uring ring
  /// cannot be created (io_uring_setup(2) or ring mmap failure) instead of
  /// throwing.
  static tl::expected<std::unique_ptr<RingEngine>, DojioError> create(RingEngineOptions options = RingEngineOptions{});
  ~RingEngine();

  RingEngine(const RingEngine&) = delete;
  RingEngine& operator=(const RingEngine&) = delete;

  /// Submits an operation, with its embedded buffer and its completion callback.
  ///
  /// The operation is passed by value (buffers use move semantics; the buffer is part
  /// of the operation struct). Returns the typed OperationId of the operation, usable
  /// with cancel(). The callback is invoked at completion time as
  /// callback(RingEngine& engine, result): a reference to the underlying ring first,
  /// then the operation result (a tl::expected carrying the operation result_value or
  /// an ErrorCode).
  template <OperationType Op, typename Callback> OperationId submit(Op op, Callback callback) {
    static_assert(std::is_invocable_v<Callback&, RingEngine&, typename std::remove_cvref_t<Op>::result>,
                  "callback must be invocable as callback(RingEngine&, Op::result)");
    return do_submit(std::move(op), std::move(callback), false);
  }

  /// Arms a timer: the action completes successfully once the duration has elapsed.
  /// The callback is invoked as callback(RingEngine&, Result<TimeoutResult>).
  template <typename Callback> OperationId submit(Timer timer, Callback callback) {
    static_assert(std::is_invocable_v<Callback&, RingEngine&, TimeoutOp::result>,
                  "callback must be invocable as callback(RingEngine&, Result<TimeoutResult>)");
    enforce_precondition(timer.duration() >= Timer::Duration{0});
    return do_submit(timeout(timer.duration()), std::move(callback), true);
  }

  /// Cancels a pending operation. The operation completes with the Cancelled error
  /// code (counted as a failure). Returns false when the id is unknown.
  bool cancel(OperationId id);

  /// Processes operations according to mode, dispatching at most max_tasks
  /// completion callbacks, and returns the number of callbacks dispatched.
  std::uint64_t process(ProcessMode mode, std::uint64_t max_tasks = std::numeric_limits<std::uint64_t>::max());

  /// Non-blocking convenience for fiber / event-loop schedulers: equivalent to
  /// process(ProcessMode::NonBlocking, max_tasks). Returns the number of dispatched
  /// callbacks (inline + ring).
  ///
  /// A scheduler can yield or park the fiber when the returned count is zero.
  std::uint64_t poll(std::uint64_t max_tasks = std::numeric_limits<std::uint64_t>::max());

  /// Interrupts a process(ProcessMode::Forever) loop; checked between batches.
  void request_stop() { stop_requested_ = true; }

  /// Number of operations in the given state. Cancellation counts as a failure.
  [[nodiscard]] std::uint64_t count_operations(OperationState state) const;

  /// File descriptor of the underlying io_uring instance.
  [[nodiscard]] std::int32_t ring_fd() const;

  /// True when the underlying ring exposes the given io_uring opcode, as reported
  /// by IORING_REGISTER_PROBE. False when the probe could not be run or the opcode
  /// is unavailable.
  [[nodiscard]] bool supports_op(IoOp op) const noexcept;

  /// The opcodes of the syscall-backed operations (bind/listen, ftruncate, pipe)
  /// the underlying ring exposes; the matching operations stay inline otherwise.
  [[nodiscard]] OpCapabilities capabilities() const noexcept;

  /// True when the underlying ring was created with IORING_SETUP_SQPOLL.
  [[nodiscard]] bool sqpoll_enabled() const;

  /// The SQPOLL kernel thread polling time actually set on the ring
  /// (io_uring_params.sq_thread_idle); zero when SQPOLL is disabled.
  [[nodiscard]] std::chrono::milliseconds sqpoll_poll_time() const;

  /// The underlying io_uring ring (e.g. to create a RegisteredMemoryPool on this
  /// engine).
  [[nodiscard]] detail::IoUring& ring() { return *ring_; }

private:
  explicit RingEngine(std::unique_ptr<detail::IoUring> ring);

  template <OperationType Op, typename Callback> OperationId do_submit(Op op, Callback callback, bool timer) {
    using ActionType = Action<Op, Callback>;
    static_assert(sizeof(ActionType) <= max_size_action,
                  "Action exceeds max_size_action: shrink the callback / buffer or raise max_size_action");
    // reserve the id first so the Action can be constructed with it; release the
    // slot if the allocation / construction throws
    const OperationId id = pending_.reserve_id();
    try {
      std::pmr::polymorphic_allocator<ActionType> allocator{action_pool_};
      auto action = BoxPtr<ActionType>::make(allocator, id, std::move(op), std::move(callback), timer, capabilities_);
      ActionBase& action_ref = *action;
      // insert into pending_ before writing to the ring so an SQPOLL kernel thread
      // observing the SQE immediately always sees a live, stable Action
      pending_.assign(id, std::move(action));
      enqueue_action(id, action_ref);
    } catch (...) {
      pending_.erase(id);
      throw;
    }
    return id;
  }

  /// Registers a newly submitted action straight onto the kernel SQ ring when
  /// possible (publishing immediately on SQPOLL), or defers it to unsubmitted_
  /// when the operation is inline or the SQ ring is full.
  void enqueue_action(OperationId id, ActionBase& action);
  /// Reserves a kernel SQE slot and writes action into it. Returns false when the
  /// kernel submission queue is full.
  bool try_enqueue_sqe(ActionBase& action);
  /// Reaps available CQ completions into waiting_hook_ until waiting_hook_.size()
  /// reaches max_hook_size or the CQ ring is empty.
  void reap_completions(std::uint64_t max_hook_size);
  /// Drains unsubmitted_ FIFO: enqueues deferred ring operations into free SQ slots
  /// and executes inline operations into waiting_hook_ (while waiting_hook_.size()
  /// < max_hook_size). Returns true when at least one deferred ring SQE was enqueued.
  bool drain_unsubmitted(std::uint64_t max_hook_size);
  /// Invokes all completion callbacks collected in waiting_hook_ and clears it.
  /// Returns the number of callbacks dispatched.
  std::uint64_t run_waiting_hooks();
  /// Publishes pending SQEs to the kernel and calls io_uring_enter(2) when needed
  /// (to submit on non-SQPOLL rings, to wait for min_complete > 0 completions, or
  /// to flush a CQ overflow). Returns true when non-SQPOLL SQEs were submitted.
  bool submit_ring_sqes(std::uint32_t min_complete);
  /// Frees at least one slot in a full kernel submission queue without blocking on
  /// completions (submits pending SQEs on non-SQPOLL; waits for SQ space on SQPOLL).
  void wait_sq_space();
  void dispatch(OperationId id, std::int32_t res);
  void finish(ActionBase& action, std::int32_t res);
  /// io_uring_enter with EINTR retry; any other error is a contract failure.
  std::int32_t enter_wait(std::uint32_t to_submit, std::uint32_t min_complete);
  /// Wakes the SQPOLL kernel thread when it is sleeping (IORING_SQ_NEED_WAKEUP);
  /// a no-op on non-SQPOLL rings.
  void wake_sqpoll_if_needed();

  // pimpl: the io_uring ring lives in the library
  std::unique_ptr<detail::IoUring> ring_;
  // opcodes the ring exposes, probed once at construction and passed to every
  // Action so compute_inline() stays a pure function of the operation
  OpCapabilities capabilities_{};
  // pool owned by the ring, used to allocate the Action objects (actions are made
  // with BoxPtr so the allocation size follows the concrete Action type)
  std::pmr::unsynchronized_pool_resource* action_pool_ = nullptr;
  // pending operations, addressed by their OperationId (O(1) index + generation)
  detail::SlotTable<BoxPtr<ActionBase>> pending_;
  // FIFO queue of operation ids that could not be placed on the kernel ring at
  // submit() time: inline operations (syscall-backed ops on older kernels) and
  // overflowed ring operations (when the kernel SQ ring was full). Consumed via
  // unsubmitted_head_ for O(1) front pop.
  std::vector<OperationId> unsubmitted_;
  std::uint64_t unsubmitted_head_ = 0;
  // Number of live ring (non-inline) operations currently queued in unsubmitted_.
  // While > 0, new ring submissions append to unsubmitted_ to preserve FIFO order.
  std::uint64_t unsubmitted_ring_count_ = 0;
  // Completed actions (reaped from the CQ ring or executed inline) waiting for their
  // completion callbacks to run, preallocated once from the ring size so process()
  // never reallocates on the hot path.
  std::vector<std::pair<BoxPtr<ActionBase>, std::int32_t>> waiting_hook_;
  std::uint64_t completed_ = 0;
  std::uint64_t failed_ = 0;
  bool stop_requested_ = false;
};

} // namespace dojio
