#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
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
/// tl::expected, then submit() queues
/// an operation (all parameters are passed by value, buffers are moved) and returns
/// a typed OperationId; process() pushes the queued operations to the kernel and
/// dispatches their completions.
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

  /// Submits op with its associated buffer and completion callback.
  ///
  /// All parameters are passed by value; buffers use move semantics. Returns the
  /// typed OperationId of the operation, usable with cancel(). The callback is
  /// invoked at completion time with the Result as first parameter:
  /// callback(Result<Value>, OperationId, raw_result).
  template <typename Op, typename Buffer, typename Callback>
  OperationId submit(Op op, Buffer buffer, Callback callback) {
    static_assert(is_operation_v<Op>, "Op must be one of the dojio operation structs");
    static_assert(BufferLike<Buffer>, "Buffer must be a std::vector, std::array or RegisteredBuffer");
    static_assert(uses_buffer_v<Op>, "this operation does not take a buffer; use submit(op, callback)");
    if constexpr (std::is_same_v<Op, FstatOp>) {
      // the kernel writes the whole registered chunk, or the current size of a container
      std::uint64_t bytes = 0;
      if constexpr (std::is_same_v<Buffer, RegisteredBuffer>) {
        bytes = buffer.capacity() * sizeof(typename Buffer::value_type);
      } else {
        bytes = static_cast<std::uint64_t>(buffer.size()) * sizeof(typename Buffer::value_type);
      }
      enforce_precondition(bytes >= k_statx_size);
    }
    return do_submit(std::move(op), std::move(buffer), std::move(callback), false);
  }

  /// Submits op without a buffer.
  template <typename Op, typename Callback> OperationId submit(Op op, Callback callback) {
    static_assert(is_operation_v<Op>, "Op must be one of the dojio operation structs");
    static_assert(!uses_buffer_v<Op> || std::is_same_v<Op, AcceptOp>,
                  "this operation requires a buffer; use submit(op, buffer, callback)");
    return do_submit(std::move(op), std::monostate{}, std::move(callback), false);
  }

  /// Arms a timer: the action completes successfully once the duration has elapsed.
  template <typename Callback> OperationId submit(Timer timer, Callback callback) {
    enforce_precondition(timer.duration() >= Timer::Duration{0});
    return do_submit(timeout(timer.duration()), std::monostate{}, std::move(callback), true);
  }

  /// Cancels a pending operation. The operation completes with the Cancelled error
  /// code (counted as a failure). Returns false when the id is unknown.
  bool cancel(OperationId id);

  /// Processes operations according to mode.
  void process(ProcessMode mode);

  /// Non-blocking convenience for fiber / event-loop schedulers: runs the inline
  /// pending operations, submits the queued operations without waiting, dispatches
  /// the completions that are already available and returns the number of dispatched
  /// callbacks (inline + ring).
  ///
  /// A scheduler can yield or park the fiber when the returned count is zero.
  std::uint64_t poll();

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

  template <typename Op, typename Buffer, typename Callback>
  OperationId do_submit(Op op, Buffer buffer, Callback callback, bool timer) {
    using ActionType = Action<Op, Buffer, Callback>;
    static_assert(sizeof(ActionType) <= max_size_action,
                  "Action exceeds max_size_action: shrink the callback / buffer or raise max_size_action");
    // reserve the id first so the Action can be constructed with it; release the
    // slot if the allocation / construction throws
    const OperationId id = pending_.reserve_id();
    try {
      std::pmr::polymorphic_allocator<ActionType> allocator{action_pool_};
      auto action = BoxPtr<ActionType>::make(allocator, id, std::move(op), std::move(buffer), std::move(callback),
                                             timer, capabilities_);
      if (action->is_inline()) {
        inline_pending_.push_back(id);
      } else {
        // fill the SQE mirror now; the RingEngine copies it onto the kernel ring at flush
        action->fill();
        action->sqe().user_data = id.value;
        unsubmitted_.push_back(id);
      }
      pending_.assign(id, std::move(action));
    } catch (...) {
      pending_.erase(id);
      throw;
    }
    return id;
  }

  /// Copies every queued SQE mirror onto the kernel submission queue and submits it;
  /// when min_complete > 0, also waits for at least that many completions.
  void flush(std::uint32_t min_complete);
  /// Executes the operations dispatched inline at submission time (a syscall-backed
  /// operation whose io_uring opcode the ring does not expose) and dispatches them.
  /// Returns the number of callbacks dispatched.
  std::uint64_t run_inline_pending();
  /// Reaps every available completion and dispatches it. Returns the number of
  /// callbacks dispatched.
  std::uint64_t reap_and_dispatch();
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
  // Work queues of the operation ids that still need a ring submission
  // (unsubmitted_) or an inline execution (inline_pending_). They keep flush() and
  // run_inline_pending() proportional to the new operations, not to the whole
  // pending set. inline_scratch_ lets run_inline_pending() detach the queue so the
  // completion callbacks can submit new inline operations safely.
  std::vector<OperationId> unsubmitted_;
  std::vector<OperationId> inline_pending_;
  std::vector<OperationId> inline_scratch_;
  // Scratch buffer for the dispatch loops, preallocated once from the ring
  // size in the constructor so run_inline_pending()/reap_and_dispatch() never
  // reallocate on the hot path. Both loops extract the completed actions from
  // pending_ into it before running the completion handlers.
  std::vector<std::pair<BoxPtr<ActionBase>, std::int32_t>> done_;
  std::uint64_t completed_ = 0;
  std::uint64_t failed_ = 0;
  bool stop_requested_ = false;
};

} // namespace dojio
