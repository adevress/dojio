#include <ringu/ring.hpp>

#include <ringu/contracts.hpp>

#include <fmt/format.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>

namespace ringu::detail {

namespace {

// kernel limit on the number of SQ entries (IORING_MAX_ENTRIES, not exposed by the uapi header)
constexpr std::uint32_t k_max_entries = 32768;

// syscall(2) returns long (C ABI); the wrappers narrow to fixed-width at the boundary
long sys_io_uring_setup(std::uint32_t entries, io_uring_params* params) {
  return ::syscall(__NR_io_uring_setup, entries, params);
}

long sys_io_uring_enter(std::int32_t fd, std::uint32_t to_submit, std::uint32_t min_complete, std::uint32_t flags) {
  return ::syscall(__NR_io_uring_enter, fd, to_submit, min_complete, flags, nullptr, 0);
}

long sys_io_uring_register(std::int32_t fd, std::uint32_t opcode, const void* arg, std::uint32_t nr_args) {
  return ::syscall(__NR_io_uring_register, fd, opcode, arg, nr_args);
}

void* map_ring(std::int32_t fd, std::uint64_t size, std::uint64_t offset) {
  // mmap takes a size_t length (C ABI boundary)
  void* ptr = ::mmap(nullptr, static_cast<std::size_t>(size), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd,
                     static_cast<off_t>(offset));
  if (ptr == MAP_FAILED) {
    return nullptr;
  }
  return ptr;
}

} // namespace

tl::expected<std::unique_ptr<IoUring>, RinguError> IoUring::create(std::uint32_t entries, std::uint32_t setup_flags,
                                                                   std::uint32_t sq_thread_idle_ms) {
  enforce_precondition(entries > 0);
  enforce_precondition(entries <= k_max_entries);
  auto ring = std::unique_ptr<IoUring>(new IoUring());
  auto setup_result = ring->setup(entries, setup_flags, sq_thread_idle_ms);
  if (!setup_result.has_value()) {
    return tl::unexpected(std::move(setup_result).error());
  }
  return ring;
}

tl::expected<void, RinguError> IoUring::setup(std::uint32_t entries, std::uint32_t setup_flags,
                                              std::uint32_t sq_thread_idle_ms) {
  params_.flags = setup_flags;
  if ((setup_flags & IORING_SETUP_SQPOLL) != 0) {
    // SQPOLL thread polling time before it goes to sleep (ms)
    params_.sq_thread_idle = sq_thread_idle_ms;
  }
  fd_ = static_cast<std::int32_t>(sys_io_uring_setup(entries, &params_));
  if (fd_ < 0) {
    // errno is only valid right after the failing syscall: capture it before formatting
    const int err = errno;
    return tl::unexpected(
        RinguError{ErrorOpcode::IoUringSetup,
                   fmt::format("ringu: io_uring_setup(entries={}) failed: {}", entries, std::strerror(err))});
  }
  sq_mask_ = params_.sq_entries - 1;
  cq_mask_ = params_.cq_entries - 1;
  sq_ring_size_ = static_cast<std::uint64_t>(params_.sq_off.array) +
                  static_cast<std::uint64_t>(params_.sq_entries) * sizeof(std::uint32_t);
  cq_ring_size_ = static_cast<std::uint64_t>(params_.cq_off.cqes) +
                  static_cast<std::uint64_t>(params_.cq_entries) * sizeof(io_uring_cqe);
  sq_ring_ = map_ring(fd_, sq_ring_size_, IORING_OFF_SQ_RING);
  cq_ring_ = map_ring(fd_, cq_ring_size_, IORING_OFF_CQ_RING);
  sqes_ = static_cast<io_uring_sqe*>(
      map_ring(fd_, static_cast<std::uint64_t>(params_.sq_entries) * sizeof(io_uring_sqe), IORING_OFF_SQES));
  if (sq_ring_ == nullptr || cq_ring_ == nullptr || sqes_ == nullptr) {
    const int err = errno;
    return tl::unexpected(
        RinguError{ErrorOpcode::IoUringMmap, fmt::format("ringu: io_uring ring mmap failed: {}", std::strerror(err))});
  }
  auto* sq_base = static_cast<std::uint8_t*>(sq_ring_);
  auto* cq_base = static_cast<std::uint8_t*>(cq_ring_);
  sq_head_ = reinterpret_cast<std::uint32_t*>(sq_base + params_.sq_off.head);
  sq_tail_ = reinterpret_cast<std::uint32_t*>(sq_base + params_.sq_off.tail);
  sq_array_ = reinterpret_cast<std::uint32_t*>(sq_base + params_.sq_off.array);
  sq_flags_ = reinterpret_cast<std::uint32_t*>(sq_base + params_.sq_off.flags);
  cq_head_ = reinterpret_cast<std::uint32_t*>(cq_base + params_.cq_off.head);
  cq_tail_ = reinterpret_cast<std::uint32_t*>(cq_base + params_.cq_off.tail);
  cqes_ = reinterpret_cast<io_uring_cqe*>(cq_base + params_.cq_off.cqes);
  return {};
}

IoUring::~IoUring() {
  if (fd_ >= 0 && sqpoll()) {
    // nudge the SQPOLL kernel thread so it observes the ring shutdown promptly
    (void)sys_io_uring_enter(fd_, 0, 0, IORING_ENTER_SQ_WAKEUP);
  }
  if (sqes_ != nullptr) {
    ::munmap(sqes_, static_cast<std::size_t>(static_cast<std::uint64_t>(params_.sq_entries) * sizeof(io_uring_sqe)));
  }
  if (sq_ring_ != nullptr) {
    ::munmap(sq_ring_, sq_ring_size_);
  }
  if (cq_ring_ != nullptr) {
    ::munmap(cq_ring_, cq_ring_size_);
  }
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

io_uring_sqe* IoUring::reserve_sqe() {
  const std::uint32_t head = std::atomic_ref<std::uint32_t>(*sq_head_).load(std::memory_order_acquire);
  if (sq_tail_shadow_ - head >= params_.sq_entries) {
    return nullptr;
  }
  const std::uint32_t tail = sq_tail_shadow_;
  sq_tail_shadow_ = tail + 1;
  io_uring_sqe* sqe = &sqes_[tail & sq_mask_];
  std::memset(sqe, 0, sizeof(*sqe));
  sq_array_[tail & sq_mask_] = tail & sq_mask_;
  return sqe;
}

void IoUring::publish_sqes() {
  // publish the whole batch at once: the release store orders the SQE contents
  // and the SQ array writes before the kernel (or the SQPOLL thread) reads them
  std::atomic_ref<std::uint32_t>(*sq_tail_).store(sq_tail_shadow_, std::memory_order_release);
}

std::uint32_t IoUring::pending_sqes() const {
  const std::uint32_t head = std::atomic_ref<std::uint32_t>(*sq_head_).load(std::memory_order_acquire);
  return sq_tail_shadow_ - head;
}

std::int32_t IoUring::enter(std::uint32_t to_submit, std::uint32_t min_complete, std::uint32_t flags) {
  return static_cast<std::int32_t>(sys_io_uring_enter(fd_, to_submit, min_complete, flags));
}

bool IoUring::sqpoll_needs_wakeup() const {
  return sqpoll() &&
         (std::atomic_ref<std::uint32_t>(*sq_flags_).load(std::memory_order_acquire) & IORING_SQ_NEED_WAKEUP) != 0;
}

std::int32_t IoUring::wake_sqpoll() { return enter(0, 0, IORING_ENTER_SQ_WAKEUP); }

const io_uring_cqe* IoUring::peek_cqe() const {
  const std::uint32_t head = std::atomic_ref<std::uint32_t>(*cq_head_).load(std::memory_order_acquire);
  const std::uint32_t tail = std::atomic_ref<std::uint32_t>(*cq_tail_).load(std::memory_order_acquire);
  if (head == tail) {
    return nullptr;
  }
  return &cqes_[head & cq_mask_];
}

void IoUring::cq_advance(std::uint32_t n) {
  const std::uint32_t head = std::atomic_ref<std::uint32_t>(*cq_head_).load(std::memory_order_acquire);
  std::atomic_ref<std::uint32_t>(*cq_head_).store(head + n, std::memory_order_release);
}

void copy_sqe(io_uring_sqe& dst, const Sqe& src) {
  std::memset(&dst, 0, sizeof(dst));
  std::memset(&dst, 0, sizeof(dst));
  dst.opcode = static_cast<__u8>(src.opcode);
  dst.fd = src.fd;
  dst.addr = src.addr;
  dst.len = src.len;
  dst.user_data = src.user_data;
  dst.buf_index = src.buf_index;
  // off and addr2 share a union in the kernel SQE, and the per-opcode flags share
  // another one: set only the field the opcode actually uses
  switch (static_cast<IoOp>(src.opcode)) {
  case IoOp::Read:
  case IoOp::ReadFixed:
  case IoOp::Write:
  case IoOp::WriteFixed:
  case IoOp::Ftruncate:
    dst.off = src.off;
    break;
  case IoOp::Statx:
    dst.addr2 = src.addr2;
    dst.statx_flags = src.op_flags;
    break;
  case IoOp::Accept:
    dst.addr2 = src.addr2;
    dst.accept_flags = src.op_flags;
    break;
  case IoOp::OpenAt:
    dst.open_flags = src.op_flags;
    break;
  case IoOp::Fsync:
    dst.fsync_flags = src.op_flags;
    break;
  case IoOp::Connect:
  case IoOp::Bind:
    // off and addr2 are the same union member: the connect / bind address length
    dst.addr2 = src.addr2;
    break;
  case IoOp::Pipe:
    // pipe_flags shares its union slot with rw_flags; using rw_flags keeps the
    // library compiling against older <linux/io_uring.h> that predate pipe_flags.
    // rw_flags is __kernel_rwf_t (signed) on recent headers, hence the explicit cast.
    dst.rw_flags = static_cast<decltype(dst.rw_flags)>(src.op_flags);
    break;
  case IoOp::Send:
  case IoOp::Recv:
    dst.msg_flags = src.op_flags;
    break;
  case IoOp::Timeout:
    dst.timeout_flags = src.op_flags;
    break;
  default:
    break;
  }
}

std::int32_t register_buffers(std::int32_t ring_fd, const iovec* iovecs, std::uint32_t nr) {
  enforce_precondition(ring_fd >= 0);
  enforce_precondition(iovecs != nullptr && nr > 0);
  return static_cast<std::int32_t>(sys_io_uring_register(ring_fd, IORING_REGISTER_BUFFERS, iovecs, nr));
}

std::int32_t unregister_buffers(std::int32_t ring_fd) {
  enforce_precondition(ring_fd >= 0);
  return static_cast<std::int32_t>(sys_io_uring_register(ring_fd, IORING_UNREGISTER_BUFFERS, nullptr, 0));
}

} // namespace ringu::detail
