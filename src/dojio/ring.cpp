#include <dojio/ring.hpp>

#include <dojio/action.hpp>
#include <dojio/contracts.hpp>

#include <fmt/format.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <system_error>

namespace dojio::detail {

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

/// Upper bound on the number of opcodes IORING_REGISTER_PROBE reports.
inline constexpr std::uint32_t k_max_probe_ops = 256;

/// Storage for the IORING_REGISTER_PROBE answer: a struct io_uring_probe header
/// immediately followed by up to k_max_probe_ops io_uring_probe_op entries. The uapi
/// header ends in a flexible array member, so the two views cannot be members of the
/// same struct; the storage is an aligned byte buffer interpreted as the header and
/// then the entries at offset sizeof(io_uring_probe).
struct ProbeBuffer {
  alignas(io_uring_probe)
      std::array<std::byte, sizeof(io_uring_probe) + (sizeof(io_uring_probe_op) * k_max_probe_ops)> bytes{};
};

void* map_ring(std::int32_t fd, std::uint64_t size, std::uint64_t offset) {
  // mmap takes a size_t length (C ABI boundary)
  void* ptr = ::mmap(nullptr, static_cast<std::size_t>(size), combine_flags(PROT_READ, PROT_WRITE),
                     combine_flags(MAP_SHARED, MAP_POPULATE), fd, static_cast<off_t>(offset));
  if (ptr == MAP_FAILED) {
    return nullptr;
  }
  return ptr;
}

} // namespace

IoUring::IoUring(std::pmr::pool_options action_pool_options) : action_pool_(action_pool_options) {}

tl::expected<std::unique_ptr<IoUring>, DojioError> IoUring::create(std::uint32_t entries, std::uint32_t setup_flags,
                                                                   std::uint32_t sq_thread_idle_ms) {
  enforce_precondition(entries > 0);
  enforce_precondition(entries <= k_max_entries);
  // the action pool is sized for max_size_action and for a chunk of at most one
  // action per ring entry (RingEngineOptions::entries)
  std::pmr::pool_options action_pool_options{};
  action_pool_options.largest_required_pool_block = max_size_action;
  action_pool_options.max_blocks_per_chunk = static_cast<std::size_t>(entries);
  auto ring = std::unique_ptr<IoUring>(new IoUring(action_pool_options));
  auto setup_result = ring->setup(entries, setup_flags, sq_thread_idle_ms);
  if (!setup_result.has_value()) {
    return tl::unexpected(std::move(setup_result).error());
  }
  return ring;
}

tl::expected<void, DojioError> IoUring::setup(std::uint32_t entries, std::uint32_t setup_flags,
                                              std::uint32_t sq_thread_idle_ms) {
  params_.flags = setup_flags;
  if ((setup_flags & IORING_SETUP_SQPOLL) != 0) {
    // SQPOLL thread polling time before it goes to sleep (ms)
    params_.sq_thread_idle = sq_thread_idle_ms;
  }
  fd_ = static_cast<std::int32_t>(sys_io_uring_setup(entries, &params_));
  if (fd_ < 0) {
    // errno is only valid right after the failing syscall: capture it before formatting
    const std::int32_t err = static_cast<std::int32_t>(errno);
    return tl::unexpected(
        DojioError{ErrorOpcode::IoUringSetup, fmt::format("dojio: io_uring_setup(entries={}) failed: {}", entries,
                                                          std::error_code(err, std::generic_category()).message())});
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
    const std::int32_t err = static_cast<std::int32_t>(errno);
    return tl::unexpected(
        DojioError{ErrorOpcode::IoUringMmap, fmt::format("dojio: io_uring ring mmap failed: {}",
                                                         std::error_code(err, std::generic_category()).message())});
  }
  auto* sq_base = static_cast<std::uint8_t*>(sq_ring_);
  auto* cq_base = static_cast<std::uint8_t*>(cq_ring_);
  sq_head_ = uring_cast<std::uint32_t*>(sq_base + params_.sq_off.head);
  sq_tail_ = uring_cast<std::uint32_t*>(sq_base + params_.sq_off.tail);
  sq_array_ = uring_cast<std::uint32_t*>(sq_base + params_.sq_off.array);
  sq_flags_ = uring_cast<std::uint32_t*>(sq_base + params_.sq_off.flags);
  cq_head_ = uring_cast<std::uint32_t*>(cq_base + params_.cq_off.head);
  cq_tail_ = uring_cast<std::uint32_t*>(cq_base + params_.cq_off.tail);
  cqes_ = uring_cast<io_uring_cqe*>(cq_base + params_.cq_off.cqes);
  probe_opcodes();
  return {};
}

void IoUring::probe_opcodes() {
  ProbeBuffer probe{};
  const long rc = sys_io_uring_register(fd_, IORING_REGISTER_PROBE, probe.bytes.data(), k_max_probe_ops);
  if (rc != 0) {
    return;
  }
  auto* header = uring_cast<io_uring_probe*>(probe.bytes.data());
  auto* ops = uring_cast<io_uring_probe_op*>(probe.bytes.data() + sizeof(io_uring_probe));
  const std::uint32_t count =
      std::min<std::uint32_t>(static_cast<std::uint32_t>(header->last_op) + 1U, k_max_probe_ops);
  for (std::uint32_t i = 0; i < count; ++i) {
    if ((ops[i].flags & IO_URING_OP_SUPPORTED) != 0U) {
      supported_ops_[ops[i].op] = true;
    }
  }
}

bool IoUring::supports_op(IoOp op) const noexcept { return supported_ops_[static_cast<std::uint8_t>(op)]; }

OpCapabilities IoUring::capabilities() const noexcept {
  OpCapabilities capabilities{};
  capabilities.bind_listen = supports_op(IoOp::Bind) && supports_op(IoOp::Listen);
  capabilities.ftruncate = supports_op(IoOp::Ftruncate);
  capabilities.pipe = supports_op(IoOp::Pipe);
  return capabilities;
}

bool IoUring::cq_overflow() const noexcept {
  return (std::atomic_ref<std::uint32_t>(*sq_flags_).load(std::memory_order_acquire) & IORING_SQ_CQ_OVERFLOW) != 0;
}

IoUring::~IoUring() {
  if (fd_ >= 0 && sqpoll()) {
    // nudge the SQPOLL kernel thread so it observes the ring shutdown promptly
    const std::int32_t ret = static_cast<std::int32_t>(sys_io_uring_enter(fd_, 0, 0, IORING_ENTER_SQ_WAKEUP));
    enforce_precondition(ret >= 0);
  }
  if (sqes_ != nullptr) {
    const std::uint64_t sqes_size = static_cast<std::uint64_t>(params_.sq_entries) * sizeof(io_uring_sqe);
    const std::int32_t rc = ::munmap(sqes_, static_cast<std::size_t>(sqes_size));
    enforce_invariant(rc == 0);
  }
  if (sq_ring_ != nullptr) {
    const std::int32_t rc = ::munmap(sq_ring_, sq_ring_size_);
    enforce_invariant(rc == 0);
  }
  if (cq_ring_ != nullptr) {
    const std::int32_t rc = ::munmap(cq_ring_, cq_ring_size_);
    enforce_invariant(rc == 0);
  }
  if (fd_ >= 0) {
    const std::int32_t rc = ::close(fd_);
    enforce_invariant(rc == 0);
  }
}

io_uring_sqe* IoUring::reserve_sqe() {
  const std::uint32_t head = std::atomic_ref<std::uint32_t>(*sq_head_).load(std::memory_order_acquire);
  if ((sq_tail_shadow_ - head) >= params_.sq_entries) {
    return nullptr;
  }
  const std::uint32_t tail = sq_tail_shadow_;
  sq_tail_shadow_ = tail + 1;
  io_uring_sqe* sqe = &sqes_[tail & sq_mask_];
  // cppcheck-suppress misra-cpp-2023-21.2.2 ; MISRA C++ deviation: memset zeroes the kernel SQE (sizeof-bounded)
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

void copy_sqe(io_uring_sqe& dst, const Sqe& src) {
  // cppcheck-suppress misra-cpp-2023-21.2.2 ; MISRA C++ deviation: memset zeroes the kernel SQE (sizeof-bounded)
  std::memset(&dst, 0, sizeof(dst));
  dst.opcode = static_cast<std::uint8_t>(src.opcode);
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

} // namespace dojio::detail
