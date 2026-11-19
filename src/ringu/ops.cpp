#include <ringu/action.hpp>

#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#if defined(__linux__)
#include <linux/stat.h>
#include <sys/sysmacros.h>
#endif

// Library-side implementations of the small helpers declared in the public
// headers. This translation unit is where the POSIX / Linux types live.

namespace ringu::detail {

std::int32_t listen_fd(std::int32_t fd, std::int32_t backlog) {
  if (::listen(fd, backlog) != 0) {
    return -errno;
  }
  return 0;
}

std::int32_t bind_fd(std::int32_t fd, const void* addr, std::uint32_t addr_len) {
  // bind(2) uses socklen_t (C ABI boundary)
  if (::bind(fd, reinterpret_cast<const sockaddr*>(addr), static_cast<socklen_t>(addr_len)) != 0) {
    return -errno;
  }
  return 0;
}

std::int32_t ftruncate_fd(std::int32_t fd, std::uint64_t length) {
  // ftruncate(2) uses off_t (C ABI boundary)
  if (::ftruncate(fd, static_cast<off_t>(length)) != 0) {
    return -errno;
  }
  return 0;
}

std::int32_t pipe_fd(std::int32_t* fds, std::int32_t flags) {
#if defined(__linux__)
  if (::pipe2(fds, flags) != 0) {
    return -errno;
  }
#else
  // pipe2(2) is Linux-specific: fall back to pipe(2) and ignore the flags
  (void)flags;
  if (::pipe(fds) != 0) {
    return -errno;
  }
#endif
  return 0;
}

void statx_to_stat(std::byte* data, std::uint64_t size) {
#if defined(__linux__)
  if (size < static_cast<std::uint64_t>(sizeof(struct statx))) {
    return;
  }
  const auto* stx = reinterpret_cast<const struct statx*>(data);
  auto* st = reinterpret_cast<struct stat*>(data);
  struct stat tmp{};
  tmp.st_dev = makedev(stx->stx_dev_major, stx->stx_dev_minor);
  tmp.st_ino = static_cast<decltype(tmp.st_ino)>(stx->stx_ino);
  tmp.st_mode = static_cast<decltype(tmp.st_mode)>(stx->stx_mode);
  tmp.st_nlink = static_cast<decltype(tmp.st_nlink)>(stx->stx_nlink);
  tmp.st_uid = static_cast<decltype(tmp.st_uid)>(stx->stx_uid);
  tmp.st_gid = static_cast<decltype(tmp.st_gid)>(stx->stx_gid);
  tmp.st_rdev = makedev(stx->stx_rdev_major, stx->stx_rdev_minor);
  tmp.st_size = static_cast<decltype(tmp.st_size)>(stx->stx_size);
  tmp.st_blksize = static_cast<decltype(tmp.st_blksize)>(stx->stx_blksize);
  tmp.st_blocks = static_cast<decltype(tmp.st_blocks)>(stx->stx_blocks);
  tmp.st_atim.tv_sec = static_cast<decltype(tmp.st_atim.tv_sec)>(stx->stx_atime.tv_sec);
  tmp.st_atim.tv_nsec = static_cast<decltype(tmp.st_atim.tv_nsec)>(stx->stx_atime.tv_nsec);
  tmp.st_mtim.tv_sec = static_cast<decltype(tmp.st_mtim.tv_sec)>(stx->stx_mtime.tv_sec);
  tmp.st_mtim.tv_nsec = static_cast<decltype(tmp.st_mtim.tv_nsec)>(stx->stx_mtime.tv_nsec);
  tmp.st_ctim.tv_sec = static_cast<decltype(tmp.st_ctim.tv_sec)>(stx->stx_ctime.tv_sec);
  tmp.st_ctim.tv_nsec = static_cast<decltype(tmp.st_ctim.tv_nsec)>(stx->stx_ctime.tv_nsec);
  *st = tmp;
#else
  // statx(2) is Linux-specific: there is no statx buffer to rewrite on other platforms
  (void)data;
  (void)size;
#endif
}

} // namespace ringu::detail
