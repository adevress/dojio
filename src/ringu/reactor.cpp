#include <ringu/reactor.hpp>

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

Reactor::Reactor(std::uint32_t entries)
    : ring_(std::make_unique<detail::IoUring>(entries)), pending_(&pool_resource_) {}

Reactor::~Reactor() = default;

std::int32_t Reactor::ring_fd() const { return ring_->fd(); }

bool Reactor::cancel(OperationId id) {
  if (id.value == 0) {
    return false;
  }
  const auto it = pending_.find(id.value);
  if (it == pending_.end()) {
    return false;
  }
  ActionBase& action = *it->second;
  if (!action.in_ring()) {
    // inline operation (lseek/listen) not executed yet: cancel it directly
    dispatch(id.value, -ECANCELED);
    return true;
  }
  // make sure the target SQE is in the kernel before the cancel request
  flush(0);
  io_uring_sqe* sqe = ring_->get_sqe();
  if (sqe == nullptr) {
    flush(0);
    sqe = ring_->get_sqe();
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

std::int32_t Reactor::enter_wait(std::uint32_t to_submit, std::uint32_t min_complete) {
  std::int32_t ret = 0;
  do {
    ret = ring_->enter(to_submit, min_complete, IORING_ENTER_GETEVENTS);
  } while (ret < 0 && errno == EINTR);
  // any other error is a bug (e.g. bad ring fd); without this check the process
  // loops would spin forever on a dead ring
  enforce_precondition(ret >= 0);
  return ret;
}

void Reactor::flush(std::uint32_t min_complete) {
  // copy every queued SQE mirror onto the kernel submission queue
  for (auto& [id, action] : pending_) {
    (void)id;
    if (action->in_ring() && !action->sqe_submitted()) {
      io_uring_sqe* ksqe = ring_->get_sqe();
      if (ksqe == nullptr) {
        enter_wait(ring_->pending_sqes(), 0);
        ksqe = ring_->get_sqe();
      }
      // after a submit the submission queue always has room: a nullptr here is a bug
      enforce_invariant(ksqe != nullptr);
      detail::copy_sqe(*ksqe, action->sqe());
      action->set_sqe_submitted();
    }
  }
  const std::uint32_t pending = ring_->pending_sqes();
  if (pending > 0) {
    enter_wait(pending, min_complete);
  } else if (min_complete > 0) {
    enter_wait(0, min_complete);
  }
}

void Reactor::run_inline_pending() {
  std::vector<std::pair<std::unique_ptr<ActionBase>, std::int32_t>> done;
  for (auto it = pending_.begin(); it != pending_.end();) {
    ActionBase& action = *it->second;
    if (!action.in_ring() && action.is_inline()) {
      auto owned = std::move(it->second);
      const std::int32_t res = owned->execute_inline();
      done.emplace_back(std::move(owned), res);
      it = pending_.erase(it);
    } else {
      ++it;
    }
  }
  for (auto& [action, res] : done) {
    finish(*action, res);
  }
}

void Reactor::reap_and_dispatch() {
  std::vector<std::pair<std::uint64_t, std::int32_t>> done;
  while (const io_uring_cqe* cqe = ring_->peek_cqe()) {
    done.emplace_back(cqe->user_data, cqe->res);
    ring_->cq_advance();
  }
  for (const auto& [id, res] : done) {
    if (id == k_cancel_user_data) {
      continue;
    }
    dispatch(id, res);
  }
}

void Reactor::dispatch(std::uint64_t id, std::int32_t res) {
  const auto it = pending_.find(id);
  if (it == pending_.end()) {
    return;
  }
  auto action = std::move(it->second);
  pending_.erase(it);
  finish(*action, res);
}

void Reactor::finish(ActionBase& action, std::int32_t res) {
  if (action.in_ring() && in_flight_ > 0) {
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

void Reactor::process(ProcessMode mode) {
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

std::uint64_t Reactor::count_operations(OperationState state) const {
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
