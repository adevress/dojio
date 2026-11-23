#include <dojio/ring_engine.hpp>

#include <dojio/contracts.hpp>
#include <dojio/ring.hpp>

#include <cerrno>
#include <cstring>
#include <optional>

#include <vector>

namespace dojio {

namespace {
/// user_data reserved for the ASYNC_CANCEL requests issued by cancel().
inline constexpr std::uint64_t k_cancel_user_data = ~static_cast<std::uint64_t>(0);
} // namespace

tl::expected<std::unique_ptr<RingEngine>, DojioError> RingEngine::create(RingEngineOptions options) {
  auto ring =
      detail::IoUring::create(options.entries, options.sqpoll ? IORING_SETUP_SQPOLL : 0u,
                              options.sqpoll ? static_cast<std::uint32_t>(options.sqpoll_poll_time.count()) : 0u);
  if (!ring.has_value()) {
    return tl::unexpected(std::move(ring).error());
  }
  return std::unique_ptr<RingEngine>(new RingEngine(std::move(ring).value()));
}

RingEngine::RingEngine(std::unique_ptr<detail::IoUring> ring)
    : ring_(std::move(ring)), capabilities_(ring_->capabilities()), action_pool_(&ring_->action_pool()),
      pending_(static_cast<std::uint32_t>(ring_->sq_entries()) * 2U) {
  // preallocate the work queues and the dispatch scratch buffers from the ring
  // size: the completion ring can hold at most cq_entries events at once, and
  // the submission ring bounds the number of queued operations
  unsubmitted_.reserve(ring_->sq_entries());
  inline_pending_.reserve(ring_->sq_entries());
  inline_scratch_.reserve(ring_->sq_entries());
  done_.reserve(ring_->cq_entries() + ring_->sq_entries());
}

RingEngine::~RingEngine() = default;

std::int32_t RingEngine::ring_fd() const { return ring_->fd(); }

bool RingEngine::supports_op(IoOp op) const noexcept { return ring_->supports_op(op); }

OpCapabilities RingEngine::capabilities() const noexcept { return capabilities_; }

bool RingEngine::sqpoll_enabled() const { return ring_->sqpoll(); }

std::chrono::milliseconds RingEngine::sqpoll_poll_time() const {
  return std::chrono::milliseconds(ring_->params().sq_thread_idle);
}

bool RingEngine::cancel(OperationId id) {
  if (id.value == 0) {
    return false;
  }
  bool is_inline = false;
  const bool found = pending_.visit(id, [&](const BoxPtr<ActionBase>& action) { is_inline = action->is_inline(); });
  if (!found) {
    return false;
  }
  if (is_inline) {
    // inline operation (bind/listen/ftruncate/pipe on an old kernel) not executed yet: cancel it directly
    dispatch(id, -ECANCELED);
    return true;
  }
  // make sure the target SQE is in the kernel before the cancel request
  flush(0);
  io_uring_sqe* sqe = ring_->reserve_sqe();
  if (sqe == nullptr) {
    flush(0);
    sqe = ring_->reserve_sqe();
  }
  // after a flush the submission queue always has room: a nullptr here is a bug
  enforce_invariant(sqe != nullptr);
  // cppcheck-suppress misra-cpp-2023-21.2.2 ; MISRA C++ deviation: memset zeroes the kernel SQE (sizeof-bounded)
  std::memset(sqe, 0, sizeof(*sqe));
  sqe->opcode = IORING_OP_ASYNC_CANCEL;
  sqe->addr = id.value;
  sqe->user_data = k_cancel_user_data;
  flush(0);
  return true;
}

std::int32_t RingEngine::enter_wait(std::uint32_t to_submit, std::uint32_t min_complete) {
  // IORING_ENTER_GETEVENTS is only needed to wait for completions or to flush an
  // overflowed completion queue; with min_complete == 0 and no overflow the call
  // only submits (and, on a non-SQPOLL ring, never sleeps)
  const std::uint32_t flags = (min_complete > 0 || ring_->cq_overflow()) ? IORING_ENTER_GETEVENTS : std::uint32_t{0};
  std::int32_t ret = 0;
  do {
    ret = ring_->enter(to_submit, min_complete, flags);
  } while (ret < 0 && errno == EINTR);
  // any other error is a bug (e.g. bad ring fd); without this check the process
  // loops would spin forever on a dead ring
  enforce_precondition(ret >= 0);
  return ret;
}

void RingEngine::wake_sqpoll_if_needed() {
  if (!ring_->sqpoll_needs_wakeup()) {
    return;
  }
  std::int32_t ret = 0;
  do {
    ret = ring_->wake_sqpoll();
  } while (ret < 0 && errno == EINTR);
  enforce_precondition(ret >= 0);
}

void RingEngine::flush(std::uint32_t min_complete) {
  // copy every newly submitted SQE mirror into a reserved kernel SQE slot; the
  // work queue only holds the operations queued since the last flush, so the
  // cost is proportional to the new operations, not to the whole pending set
  for (const OperationId id : unsubmitted_) {
    // visit skips a cancelled operation; an already submitted one (e.g. re-flushed
    // by an inner flush()) is left untouched
    static_cast<void>(pending_.visit(id, [&](BoxPtr<ActionBase>& action) {
      if (action->sqe_submitted()) {
        return;
      }
      io_uring_sqe* ksqe = ring_->reserve_sqe();
      if (ksqe == nullptr) {
        // submission queue full: publish and submit synchronously to free a slot
        ring_->publish_sqes();
        enter_wait(ring_->pending_sqes(), 0);
        ksqe = ring_->reserve_sqe();
      }
      // after a submit the submission queue always has room: a nullptr here is a bug
      enforce_invariant(ksqe != nullptr);
      detail::copy_sqe(*ksqe, action->sqe());
      action->set_sqe_submitted();
    }));
  }
  unsubmitted_.clear();
  // publish the filled SQEs to the kernel (SQ tail); with SQPOLL the kernel
  // thread may start consuming them from this point on
  ring_->publish_sqes();
  if (ring_->sqpoll()) {
    // with SQPOLL the kernel thread consumes the submission queue on its own:
    // no io_uring_enter is needed to submit, only to wake the thread when it
    // sleeps (checked after publishing the SQ tail, per the kernel protocol)
    wake_sqpoll_if_needed();
    // enter only to wait for completions or to flush an overflowed CQ
    if (min_complete > 0 || ring_->cq_overflow()) {
      enter_wait(0, min_complete);
    }
    return;
  }
  const std::uint32_t pending = ring_->pending_sqes();
  if (pending > 0) {
    enter_wait(pending, min_complete);
  } else if (min_complete > 0 || ring_->cq_overflow()) {
    enter_wait(0, min_complete);
  }
}

std::uint64_t RingEngine::run_inline_pending() {
  // O(1) no-op when no inline operation was ever queued (the common case on a
  // kernel that exposes every opcode)
  if (inline_pending_.empty()) {
    return 0;
  }
  // detach the queue so the completion callbacks may submit new inline operations
  // (pushed to the now-empty inline_pending_) without invalidating the iteration
  inline_scratch_.clear();
  inline_scratch_.swap(inline_pending_);
  done_.clear();
  for (const OperationId id : inline_scratch_) {
    std::optional<BoxPtr<ActionBase>> action = pending_.take(id);
    if (!action.has_value()) {
      continue;
    }
    const std::int32_t res = (*action)->execute_inline();
    done_.emplace_back(std::move(*action), res);
  }
  inline_scratch_.clear();
  const std::uint64_t dispatched = static_cast<std::uint64_t>(done_.size());
  for (auto& [action, res] : done_) {
    finish(*action, res);
  }
  // destroy the completed actions now (as a local vector would) and keep the
  // reserved capacity for the next call
  done_.clear();
  return dispatched;
}

std::uint64_t RingEngine::reap_and_dispatch() {
  done_.clear();
  // consume the whole completion ring, extracting the actions from pending_, and
  // only then run the completion handlers (a callback may submit new operations)
  static_cast<void>(ring_->drain_cqes([this](std::uint64_t user_data, std::int32_t res, std::uint32_t /*flags*/) {
    if (user_data == k_cancel_user_data) {
      return;
    }
    std::optional<BoxPtr<ActionBase>> action = pending_.take(OperationId{user_data});
    if (!action.has_value()) {
      return;
    }
    done_.emplace_back(std::move(*action), res);
  }));
  const std::uint64_t dispatched = static_cast<std::uint64_t>(done_.size());
  for (auto& [action, res] : done_) {
    finish(*action, res);
  }
  // destroy the completed actions now (as a local vector would) and keep the
  // reserved capacity for the next call
  done_.clear();
  return dispatched;
}

void RingEngine::dispatch(OperationId id, std::int32_t res) {
  std::optional<BoxPtr<ActionBase>> action = pending_.take(id);
  if (!action.has_value()) {
    return;
  }
  finish(**action, res);
}

void RingEngine::finish(ActionBase& action, std::int32_t res) {
  // a fired timer (-ETIME) counts as a successful completion
  const bool ok = res >= 0 || (action.is_timer() && res == -ETIME);
  if (ok) {
    completed_ += 1;
  } else {
    failed_ += 1;
  }
  action.complete(res);
}

std::uint64_t RingEngine::poll() {
  // non-blocking: submit the queued operations and dispatch only the completions
  // that are already available (flush(0) never waits)
  const std::uint64_t inline_dispatched = run_inline_pending();
  flush(0);
  const std::uint64_t ring_dispatched = reap_and_dispatch();
  return inline_dispatched + ring_dispatched;
}

void RingEngine::process(ProcessMode mode) {
  stop_requested_ = false;
  if (mode == ProcessMode::NonBlocking) {
    // submit the queued operations and dispatch only the completions that are
    // already ready; flush(0) submits without ever waiting on the kernel
    static_cast<void>(poll());
    return;
  }
  if (mode == ProcessMode::WaitOnce) {
    // dispatch whatever is already ready; blocking only when nothing was ready
    // and at least one operation is still pending
    if (poll() > 0) {
      return;
    }
    if (!pending_.empty()) {
      flush(1);
      reap_and_dispatch();
    }
    return;
  }
  // Batch / Forever: process every operation registered in pending_ (ring and
  // inline, including the ones chained from completion callbacks) and return as
  // soon as none is left. Forever additionally stops when request_stop() is called.
  while (!pending_.empty()) {
    static_cast<void>(run_inline_pending());
    if (pending_.empty() || (mode == ProcessMode::Forever && stop_requested_)) {
      return;
    }
    // submit the SQEs queued so far (including the ones queued by callbacks) and
    // wait for at least one completion (io_uring_enter may return before
    // min_complete > 1 events are available, hence the loop)
    flush(1);
    reap_and_dispatch();
  }
}

std::uint64_t RingEngine::count_operations(OperationState state) const {
  switch (state) {
  case OperationState::Pending:
    return static_cast<std::uint64_t>(pending_.size());
  case OperationState::Completed:
    return completed_;
  case OperationState::Failed:
    return failed_;
  default:
    unreachable();
  }
}

} // namespace dojio
