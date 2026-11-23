#include <dojio/ring_engine.hpp>

#include <dojio/contracts.hpp>
#include <dojio/ring.hpp>

#include <sched.h>

#include <cerrno>
#include <cstddef>
#include <limits>
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
  // preallocate the deferred queue and completion-hook scratch buffer from the ring
  // size so process() never reallocates on the hot path
  unsubmitted_.reserve(ring_->sq_entries());
  waiting_hook_.reserve(ring_->cq_entries() + ring_->sq_entries());
}

RingEngine::~RingEngine() = default;

std::int32_t RingEngine::ring_fd() const { return ring_->fd(); }

bool RingEngine::supports_op(IoOp op) const noexcept { return ring_->supports_op(op); }

OpCapabilities RingEngine::capabilities() const noexcept { return capabilities_; }

bool RingEngine::sqpoll_enabled() const { return ring_->sqpoll(); }

std::chrono::milliseconds RingEngine::sqpoll_poll_time() const {
  return std::chrono::milliseconds(ring_->params().sq_thread_idle);
}

void RingEngine::enqueue_action(OperationId id, ActionBase& action) {
  if (action.is_inline()) {
    unsubmitted_.push_back(id);
    return;
  }
  // preserve FIFO order: when earlier ring operations overflowed into unsubmitted_,
  // queue this one behind them instead of bypassing them into a newly freed slot
  if (unsubmitted_ring_count_ == 0 && try_enqueue_sqe(action)) {
    if (ring_->sqpoll()) {
      // proactive submission on SQPOLL: publish immediately so the kernel thread
      // can begin executing the I/O before process() is called
      ring_->publish_sqes();
      wake_sqpoll_if_needed();
    }
    return;
  }
  unsubmitted_.push_back(id);
  unsubmitted_ring_count_ += 1;
}

bool RingEngine::try_enqueue_sqe(ActionBase& action) {
  io_uring_sqe* ksqe = ring_->reserve_sqe();
  if (ksqe == nullptr) {
    return false;
  }
  Sqe sqe{};
  action.fill(sqe);
  sqe.user_data = action.id().value;
  detail::copy_sqe(*ksqe, sqe);
  action.set_sqe_submitted();
  return true;
}

bool RingEngine::cancel(OperationId id) {
  if (id.value == 0) {
    return false;
  }
  bool is_inline = false;
  bool sqe_submitted = false;
  const bool found = pending_.visit(id, [&](const BoxPtr<ActionBase>& action) {
    is_inline = action->is_inline();
    sqe_submitted = action->sqe_submitted();
  });
  if (!found) {
    return false;
  }
  if (is_inline || !sqe_submitted) {
    // inline operation or deferred ring operation still in unsubmitted_: cancel
    // directly in userspace without issuing IORING_OP_ASYNC_CANCEL
    if (!is_inline) {
      enforce_invariant(unsubmitted_ring_count_ > 0);
      unsubmitted_ring_count_ -= 1;
    }
    dispatch(id, -ECANCELED);
    return true;
  }
  // make sure the target SQE is in the kernel before the cancel request
  static_cast<void>(submit_ring_sqes(0));
  io_uring_sqe* sqe = ring_->reserve_sqe();
  if (sqe == nullptr) {
    wait_sq_space();
    sqe = ring_->reserve_sqe();
  }
  // after freeing SQ space the submission queue always has room: a nullptr here is a bug
  enforce_invariant(sqe != nullptr);
  // reserve_sqe() already zeroed *sqe
  sqe->opcode = IORING_OP_ASYNC_CANCEL;
  sqe->addr = id.value;
  sqe->user_data = k_cancel_user_data;
  static_cast<void>(submit_ring_sqes(0));
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

void RingEngine::wait_sq_space() {
  if (ring_->sqpoll()) {
    ring_->publish_sqes();
    wake_sqpoll_if_needed();
    if (ring_->pending_sqes() < ring_->sq_entries()) {
      return;
    }
#if defined(IORING_ENTER_SQ_WAIT)
    std::int32_t ret = 0;
    do {
      ret = ring_->enter(0, 0, IORING_ENTER_SQ_WAIT);
    } while (ret < 0 && errno == EINTR);
    if (ret >= 0) {
      return;
    }
    // kernels before 5.11 do not support IORING_ENTER_SQ_WAIT (EINVAL): yield until
    // the SQPOLL kernel thread consumes at least one SQE
    enforce_precondition(errno == EINVAL);
#endif
    while (ring_->pending_sqes() >= ring_->sq_entries()) {
      wake_sqpoll_if_needed();
      static_cast<void>(::sched_yield());
    }
    return;
  }
  ring_->publish_sqes();
  const std::uint32_t pending = ring_->pending_sqes();
  if (pending > 0) {
    enter_wait(pending, 0);
  }
}

bool RingEngine::submit_ring_sqes(std::uint32_t min_complete) {
  // publish any reserved SQEs to the kernel (SQ tail); with SQPOLL the kernel
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
    return false;
  }
  const std::uint32_t pending = ring_->pending_sqes();
  if (pending > 0) {
    enter_wait(pending, min_complete);
    return true;
  }
  if (min_complete > 0 || ring_->cq_overflow()) {
    enter_wait(0, min_complete);
  }
  return false;
}

void RingEngine::reap_completions(std::uint64_t max_hook_size) {
  const std::uint64_t current = static_cast<std::uint64_t>(waiting_hook_.size());
  if (current >= max_hook_size) {
    return;
  }
  const std::uint64_t budget = max_hook_size - current;
  const std::uint32_t max_cqes = budget > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())
                                     ? std::numeric_limits<std::uint32_t>::max()
                                     : static_cast<std::uint32_t>(budget);
  static_cast<void>(
      ring_->drain_cqes(max_cqes, [this](std::uint64_t user_data, std::int32_t res, std::uint32_t /*flags*/) -> bool {
        if (user_data == k_cancel_user_data) {
          return false;
        }
        std::optional<BoxPtr<ActionBase>> action = pending_.take(OperationId{user_data});
        if (!action.has_value()) {
          return false;
        }
        waiting_hook_.emplace_back(std::move(*action), res);
        return true;
      }));
}

bool RingEngine::drain_unsubmitted(std::uint64_t max_hook_size) {
  bool submitted_to_ring = false;
  while (unsubmitted_head_ < static_cast<std::uint64_t>(unsubmitted_.size())) {
    const OperationId id = unsubmitted_[static_cast<std::size_t>(unsubmitted_head_)];
    enum class Step : std::uint8_t { Skip, RunInline, Enqueued, Stop };
    Step step = Step::Skip;
    static_cast<void>(pending_.visit(id, [&](BoxPtr<ActionBase>& action) {
      if (action->is_inline()) {
        if (static_cast<std::uint64_t>(waiting_hook_.size()) >= max_hook_size) {
          step = Step::Stop;
        } else {
          step = Step::RunInline;
        }
        return;
      }
      if (action->sqe_submitted()) {
        step = Step::Skip;
        return;
      }
      if (try_enqueue_sqe(*action)) {
        step = Step::Enqueued;
      } else {
        step = Step::Stop;
      }
    }));
    if (step == Step::Stop) {
      break;
    }
    unsubmitted_head_ += 1;
    if (step == Step::RunInline) {
      std::optional<BoxPtr<ActionBase>> action = pending_.take(id);
      if (action.has_value()) {
        const std::int32_t res = (*action)->execute_inline();
        waiting_hook_.emplace_back(std::move(*action), res);
      }
    } else if (step == Step::Enqueued) {
      enforce_invariant(unsubmitted_ring_count_ > 0);
      unsubmitted_ring_count_ -= 1;
      submitted_to_ring = true;
    }
  }
  if (unsubmitted_head_ == static_cast<std::uint64_t>(unsubmitted_.size())) {
    unsubmitted_.clear();
    unsubmitted_head_ = 0;
  } else if (unsubmitted_head_ >= static_cast<std::uint64_t>(ring_->sq_entries())) {
    unsubmitted_.erase(unsubmitted_.begin(), unsubmitted_.begin() + static_cast<std::ptrdiff_t>(unsubmitted_head_));
    unsubmitted_head_ = 0;
  }
  if (submitted_to_ring && ring_->sqpoll()) {
    ring_->publish_sqes();
    wake_sqpoll_if_needed();
  }
  return submitted_to_ring;
}

std::uint64_t RingEngine::run_waiting_hooks() {
  const std::uint64_t dispatched = static_cast<std::uint64_t>(waiting_hook_.size());
  for (auto& [action, res] : waiting_hook_) {
    finish(*action, res);
  }
  // destroy the completed actions now (as a local vector would) and keep the
  // reserved capacity for the next call
  waiting_hook_.clear();
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
  action.complete(*this, res);
}

std::uint64_t RingEngine::poll(std::uint64_t max_tasks) { return process(ProcessMode::NonBlocking, max_tasks); }

std::uint64_t RingEngine::process(ProcessMode mode, std::uint64_t max_tasks) {
  stop_requested_ = false;
  if (max_tasks == 0) {
    static_cast<void>(drain_unsubmitted(0));
    static_cast<void>(submit_ring_sqes(0));
    return 0;
  }

  std::uint64_t total_dispatched = 0;
  for (;;) {
    const std::uint64_t remaining = max_tasks - total_dispatched;

    // 1. Collect ready completions from the CQ ring into waiting_hook_ (capped to
    //    the remaining task budget).
    reap_completions(remaining);

    // 2. Drain unsubmitted_: enqueue deferred ring ops into free SQ slots and
    //    execute inline ops into waiting_hook_ (also capped to remaining).
    static_cast<void>(drain_unsubmitted(remaining));

    // 3. Invoke all collected completion callbacks in one pass.
    total_dispatched += run_waiting_hooks();

    const bool reached_limit = total_dispatched >= max_tasks;
    const bool stopped = (mode == ProcessMode::Forever && stop_requested_);
    const bool done_wait_once = (mode == ProcessMode::WaitOnce && total_dispatched > 0);
    if (pending_.empty() || reached_limit || stopped || done_wait_once) {
      // flush any SQEs queued before or during callbacks without blocking
      static_cast<void>(drain_unsubmitted(0));
      static_cast<void>(submit_ring_sqes(0));
      return total_dispatched;
    }

    if (mode == ProcessMode::NonBlocking) {
      // submit any pending SQEs without waiting; on a non-SQPOLL ring, operations
      // that complete synchronously inside io_uring_enter(2) are immediately ready
      // in the CQ ring, so run one trailing reap + hook pass within the budget
      if (submit_ring_sqes(0)) {
        const std::uint64_t after_submit_remaining = max_tasks - total_dispatched;
        reap_completions(after_submit_remaining);
        total_dispatched += run_waiting_hooks();
        static_cast<void>(drain_unsubmitted(0));
        static_cast<void>(submit_ring_sqes(0));
      }
      return total_dispatched;
    }

    // 4. Blocking path (Batch / Forever / WaitOnce with nothing dispatched yet):
    //    flush a full SQ without blocking on completions so deferred ring ops are
    //    enqueued before waiting, and loop back immediately if callbacks queued
    //    inline operations (there may be no in-flight ring SQEs to wait on).
    if (unsubmitted_ring_count_ > 0) {
      wait_sq_space();
      continue;
    }
    if (unsubmitted_head_ < static_cast<std::uint64_t>(unsubmitted_.size())) {
      continue;
    }

    // submit pending SQEs and wait for at least one completion, then loop back to
    // reap and dispatch it
    static_cast<void>(submit_ring_sqes(1));
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
