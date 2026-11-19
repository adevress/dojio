#include <ringu/ring_engine.hpp>

#include <ringu/contracts.hpp>
#include <ringu/ring.hpp>

#include <cerrno>
#include <cstring>

#include <vector>

namespace ringu {

namespace {
/// user_data reserved for the ASYNC_CANCEL requests issued by cancel().
inline constexpr std::uint64_t k_cancel_user_data = ~static_cast<std::uint64_t>(0);
} // namespace

tl::expected<std::unique_ptr<RingEngine>, RinguError> RingEngine::create(RingEngineOptions options) {
  auto ring =
      detail::IoUring::create(options.entries, options.sqpoll ? IORING_SETUP_SQPOLL : 0u,
                              options.sqpoll ? static_cast<std::uint32_t>(options.sqpoll_poll_time.count()) : 0u);
  if (!ring.has_value()) {
    return tl::unexpected(std::move(ring).error());
  }
  return std::unique_ptr<RingEngine>(new RingEngine(std::move(ring).value()));
}

RingEngine::RingEngine(std::unique_ptr<detail::IoUring> ring) : ring_(std::move(ring)), pending_(&pool_resource_) {
  // preallocate the dispatch scratch buffer from the ring size: the completion
  // ring can hold at most cq_entries events at once, and the submission ring
  // bounds the number of queued operations
  done_.reserve(ring_->cq_entries() + ring_->sq_entries());
}

RingEngine::~RingEngine() = default;

std::int32_t RingEngine::ring_fd() const { return ring_->fd(); }

bool RingEngine::sqpoll_enabled() const { return ring_->sqpoll(); }

std::chrono::milliseconds RingEngine::sqpoll_poll_time() const {
  return std::chrono::milliseconds(ring_->params().sq_thread_idle);
}

bool RingEngine::cancel(OperationId id) {
  if (id.value == 0) {
    return false;
  }
  const auto it = pending_.find(id.value);
  if (it == pending_.end()) {
    return false;
  }
  ActionBase& action = *it->second;
  if (action.is_inline()) {
    // inline operation (bind/listen/ftruncate/pipe on an old kernel) not executed yet: cancel it directly
    dispatch(id.value, -ECANCELED);
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
  std::memset(sqe, 0, sizeof(*sqe));
  sqe->opcode = IORING_OP_ASYNC_CANCEL;
  sqe->addr = id.value;
  sqe->user_data = k_cancel_user_data;
  flush(0);
  return true;
}

std::int32_t RingEngine::enter_wait(std::uint32_t to_submit, std::uint32_t min_complete) {
  std::int32_t ret = 0;
  do {
    ret = ring_->enter(to_submit, min_complete, IORING_ENTER_GETEVENTS);
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
  // copy every queued SQE mirror into a reserved kernel SQE slot
  for (auto& [id, action] : pending_) {
    (void)id;
    if (!action->is_inline() && !action->sqe_submitted()) {
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
    }
  }
  // publish the filled SQEs to the kernel (SQ tail); with SQPOLL the kernel
  // thread may start consuming them from this point on
  ring_->publish_sqes();
  if (ring_->sqpoll()) {
    // with SQPOLL the kernel thread consumes the submission queue on its own:
    // no io_uring_enter is needed to submit, only to wake the thread when it
    // sleeps (checked after publishing the SQ tail, per the kernel protocol)
    wake_sqpoll_if_needed();
    if (min_complete > 0) {
      enter_wait(0, min_complete);
    }
    return;
  }
  const std::uint32_t pending = ring_->pending_sqes();
  if (pending > 0) {
    enter_wait(pending, min_complete);
  } else if (min_complete > 0) {
    enter_wait(0, min_complete);
  }
}

void RingEngine::run_inline_pending() {
  done_.clear();
  for (auto it = pending_.begin(); it != pending_.end();) {
    ActionBase& action = *it->second;
    if (action.is_inline()) {
      auto owned = std::move(it->second);
      const std::int32_t res = owned->execute_inline();
      done_.emplace_back(std::move(owned), res);
      it = pending_.erase(it);
    } else {
      ++it;
    }
  }
  for (auto& [action, res] : done_) {
    finish(*action, res);
  }
  // destroy the completed actions now (as a local vector would) and keep the
  // reserved capacity for the next call
  done_.clear();
}

void RingEngine::reap_and_dispatch() {
  done_.clear();
  while (const io_uring_cqe* cqe = ring_->peek_cqe()) {
    const std::uint64_t id = cqe->user_data;
    const std::int32_t res = cqe->res;
    ring_->cq_advance();
    if (id == k_cancel_user_data) {
      continue;
    }
    // extract the action from pending_ now (as run_inline_pending does), so
    // the loop below only has to run the completion handlers
    const auto it = pending_.find(id);
    if (it == pending_.end()) {
      continue;
    }
    done_.emplace_back(std::move(it->second), res);
    pending_.erase(it);
  }
  for (auto& [action, res] : done_) {
    finish(*action, res);
  }
  // destroy the completed actions now (as a local vector would) and keep the
  // reserved capacity for the next call
  done_.clear();
}

void RingEngine::dispatch(std::uint64_t id, std::int32_t res) {
  const auto it = pending_.find(id);
  if (it == pending_.end()) {
    return;
  }
  auto action = std::move(it->second);
  pending_.erase(it);
  finish(*action, res);
}

void RingEngine::finish(ActionBase& action, std::int32_t res) {
  if (!action.is_inline() && in_flight_ > 0) {
    in_flight_ -= 1;
  }
  // a fired timer (-ETIME) counts as a successful completion
  const bool ok = res >= 0 || (action.is_timer() && res == -ETIME);
  if (ok) {
    completed_ += 1;
  } else {
    failed_ += 1;
  }
  action.complete(res);
}

void RingEngine::process(ProcessMode mode) {
  stop_requested_ = false;
  for (;;) {
    run_inline_pending();
    if (mode == ProcessMode::Batch) {
      // wait for every in-flight ring operation, one completion event at a time
      // (io_uring_enter may return before min_complete > 1 events are available)
      while (in_flight_ > 0) {
        // submit the SQEs queued so far (including the ones queued by callbacks)
        // and wait for at least one completion
        flush(1);
        reap_and_dispatch();
      }
      return;
    }
    // ProcessMode::Forever: loop until interrupted (or nothing left to do)
    if (in_flight_ == 0 && pending_.empty()) {
      return;
    }
    flush(1);
    reap_and_dispatch();
    if (stop_requested_) {
      return;
    }
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

} // namespace ringu
