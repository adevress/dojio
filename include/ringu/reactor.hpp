#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <unordered_map>
#include <utility>
#include <variant>

#include <ringu/action.hpp>
#include <ringu/contracts.hpp>
#include <ringu/operation_id.hpp>
#include <ringu/operations.hpp>
#include <ringu/timer.hpp>

namespace ringu::detail {

// The io_uring ring wrapper. Defined in the library (src/ringu/ring.hpp) and held
// through a pimpl, so no io_uring / Linux internal type appears in this header.
class IoUring;

} // namespace ringu::detail

namespace ringu {

/// The Reactor submits I/O operations, their associated buffer and their completion
/// handler to a Linux io_uring instance, and dispatches the completions.
///
/// NOT THREAD SAFE: a Reactor, and every object it hands out (operation ids, buffers
/// borrowed from a pool created on it), must be used from a single thread. For
/// multi-threaded share-nothing designs, create one Reactor per thread (see the
/// demo_tcp_hello demo).
///
/// Usage: submit() queues an operation (all parameters are passed by value, buffers
/// are moved) and returns a typed OperationId; process() pushes the queued operations
/// to the kernel and dispatches their completions.
class Reactor {
public:
  /// Selects how process() behaves.
  enum class ProcessMode {
    Batch,  ///< submit and process the current batch of operations, then return
    Forever ///< loop processing operations until request_stop() is called (interruption)
  };

  /// Selects which operations count_operations reports.
  enum class OperationState {
    Pending,   ///< waiting in the pool (submitted but not completed yet)
    Completed, ///< successfully processed
    Failed     ///< failed (an I/O error code, or a cancellation)
  };

  explicit Reactor(std::uint32_t entries = 256);
  ~Reactor();

  Reactor(const Reactor&) = delete;
  Reactor& operator=(const Reactor&) = delete;

  /// Submits op with its associated buffer and completion callback.
  ///
  /// All parameters are passed by value; buffers use move semantics. Returns the
  /// typed OperationId of the operation, usable with cancel(). The callback is
  /// invoked at completion time with the Result as first parameter:
  /// callback(Result<Value>, OperationId, raw_result).
  template <typename Op, typename Buffer, typename Callback>
  OperationId submit(Op op, Buffer buffer, Callback callback) {
    static_assert(is_operation_v<Op>, "Op must be one of the ringu operation structs");
    static_assert(BufferLike<Buffer>, "Buffer must be a std::vector, std::array or RegisteredBuffer");
    static_assert(uses_buffer_v<Op>, "this operation does not take a buffer; use submit(op, callback)");
    if constexpr (std::is_same_v<Op, Fstat>) {
      enforce_precondition(buffer.size() * sizeof(typename Buffer::value_type) >= k_statx_size);
    }
    return do_submit(std::move(op), std::move(buffer), std::move(callback), false);
  }

  /// Submits op without a buffer.
  template <typename Op, typename Callback> OperationId submit(Op op, Callback callback) {
    static_assert(is_operation_v<Op>, "Op must be one of the ringu operation structs");
    static_assert(!uses_buffer_v<Op> || std::is_same_v<Op, Accept>,
                  "this operation requires a buffer; use submit(op, buffer, callback)");
    return do_submit(std::move(op), std::monostate{}, std::move(callback), false);
  }

  /// Arms a timer: the action completes successfully once the duration has elapsed.
  template <typename Callback> OperationId submit(Timer timer, Callback callback) {
    enforce_precondition(timer.duration() >= Timer::Duration{0});
    Timeout op{};
    const std::int64_t ns = timer.duration().count();
    op.ts.sec = ns / 1000000000LL;
    op.ts.nsec = ns % 1000000000LL;
    return do_submit(std::move(op), std::monostate{}, std::move(callback), true);
  }

  /// Cancels a pending operation. The operation completes with the Cancelled error
  /// code (counted as a failure). Returns false when the id is unknown.
  bool cancel(OperationId id);

  /// Processes operations according to mode.
  void process(ProcessMode mode);

  /// Interrupts a process(ProcessMode::Forever) loop; checked between batches.
  void request_stop() { stop_requested_ = true; }

  /// Number of operations in the given state. Cancellation counts as a failure.
  [[nodiscard]] std::uint64_t count_operations(OperationState state) const;

  /// File descriptor of the underlying io_uring instance.
  [[nodiscard]] std::int32_t ring_fd() const;

  /// The underlying io_uring ring (e.g. to create a RegisteredMemoryPool on this
  /// reactor).
  [[nodiscard]] detail::IoUring& ring() { return *ring_; }

private:
  template <typename Op, typename Buffer, typename Callback>
  OperationId do_submit(Op op, Buffer buffer, Callback callback, bool timer) {
    const OperationId id{next_id_};
    next_id_ += 1;
    auto action = std::make_unique<Action<Op, Buffer, Callback>>(id, std::move(op), std::move(buffer),
                                                                 std::move(callback), timer);
    if (!action->is_inline()) {
      // fill the SQE mirror now; the Reactor copies it onto the kernel ring at flush
      action->fill();
      action->sqe().user_data = id.value;
      action->set_in_ring();
      in_flight_ += 1;
    }
    pending_.emplace(id.value, std::move(action));
    return id;
  }

  /// Copies every queued SQE mirror onto the kernel submission queue and submits it;
  /// when min_complete > 0, also waits for at least that many completions.
  void flush(std::uint32_t min_complete);
  /// Executes the inline operations (lseek/listen) and dispatches them.
  void run_inline_pending();
  /// Reaps every available completion and dispatches it.
  void reap_and_dispatch();
  void dispatch(std::uint64_t id, std::int32_t res);
  void finish(ActionBase& action, std::int32_t res);
  /// io_uring_enter with EINTR retry; any other error is a contract failure.
  std::int32_t enter_wait(std::uint32_t to_submit, std::uint32_t min_complete);

  // pimpl: the io_uring ring lives in the library
  std::unique_ptr<detail::IoUring> ring_;
  std::pmr::unsynchronized_pool_resource pool_resource_;
  std::pmr::unordered_map<std::uint64_t, std::unique_ptr<ActionBase>> pending_;
  std::uint64_t next_id_ = 1;
  std::uint64_t in_flight_ = 0;
  std::uint64_t completed_ = 0;
  std::uint64_t failed_ = 0;
  bool stop_requested_ = false;
};

} // namespace ringu
