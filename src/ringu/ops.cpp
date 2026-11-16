#include <ringu/action.hpp>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include <linux/stat.h>

// Library-side implementations of the small helpers declared in the public
// headers. This translation unit is where the POSIX / Linux types live.

namespace ringu::detail {

std::int32_t lseek_fd(std::int32_t fd, std::int64_t offset, std::int32_t whence) {
  // lseek(2) uses off_t (C ABI boundary)
  const off_t ret = ::lseek(fd, static_cast<off_t>(offset), whence);
  if (ret == static_cast<off_t>(-1)) {
    return -errno;
  }
  return static_cast<std::int32_t>(ret);
}

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

void statx_to_stat(std::byte* data, std::uint64_t size) {
  if (size < static_cast<std::uint64_t>(sizeof(struct statx))) {
    return;
  }
  const auto* stx = reinterpret_cast<const struct statx*>(data);
  auto* st = reinterpret_cast<struct stat*>(data);
  struct stat tmp {};
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
}

} // namespace ringu::detail
