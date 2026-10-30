// asset_mmap.cpp — read-only mmap; see header.
#include "engine/assets/asset_mmap.hpp"

#include <utility>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace engine::assets {

MappedFile::MappedFile(MappedFile&& o) noexcept
#if defined(_WIN32)
    : file_handle_(o.file_handle_),
      map_handle_(o.map_handle_),
#else
    : fd_(o.fd_),
#endif
      data_(o.data_),
      size_(o.size_),
      mtime_ns_(o.mtime_ns_) {
  o.data_ = nullptr;
  o.size_ = 0;
#if defined(_WIN32)
  o.file_handle_ = nullptr;
  o.map_handle_ = nullptr;
#else
  o.fd_ = -1;
#endif
}

auto MappedFile::operator=(MappedFile&& o) noexcept -> MappedFile& {
  if (this != &o) {
    unmap();
#if defined(_WIN32)
    file_handle_ = o.file_handle_;
    map_handle_ = o.map_handle_;
    o.file_handle_ = nullptr;
    o.map_handle_ = nullptr;
#else
    fd_ = o.fd_;
    o.fd_ = -1;
#endif
    data_ = o.data_;
    size_ = o.size_;
    mtime_ns_ = o.mtime_ns_;
    o.data_ = nullptr;
    o.size_ = 0;
  }
  return *this;
}

MappedFile::~MappedFile() noexcept { unmap(); }

auto MappedFile::map(const std::string& path) noexcept -> bool {
  unmap();
#if defined(_WIN32)
  HANDLE fh = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (fh == INVALID_HANDLE_VALUE) {
    return false;
  }
  // File size via GetFileSize (DWORD halves). Sonar cpp:S6232 flags ANY member access
  // on LARGE_INTEGER (it is a union: LowPart/HighPart share storage with
  // QuadPart), including the innocent `sz.QuadPart` read after the API
  // fills the union. Route around the union entirely: query the size with
  // GetFileSize (DWORD halves, no union) and compose arithmetically.
  DWORD size_hi = 0;
  DWORD size_lo = GetFileSize(fh, &size_hi);
  if (size_lo == INVALID_FILE_SIZE && GetLastError() != NO_ERROR) {
    CloseHandle(fh);
    return false;
  }
  std::uint64_t file_size =
      (static_cast<std::uint64_t>(size_hi) << 32) | size_lo;
  if (file_size == 0) {
    CloseHandle(fh);
    return false;
  }
  HANDLE mh = CreateFileMappingA(fh, nullptr, PAGE_READONLY, size_hi, size_lo,
                                 nullptr);
  if (mh == nullptr) {
    CloseHandle(fh);
    return false;
  }
  const void* p = MapViewOfFile(mh, FILE_MAP_READ, 0, 0,
                                static_cast<SIZE_T>(file_size));
  if (p == nullptr) {
    CloseHandle(mh);
    CloseHandle(fh);
    return false;
  }
  FILETIME wt{};
  std::uint64_t mtime = 0;
  if (GetFileTime(fh, nullptr, nullptr, &wt)) {
    mtime = (static_cast<std::uint64_t>(wt.dwHighDateTime) << 32) |
            static_cast<std::uint64_t>(wt.dwLowDateTime);
    mtime = mtime * 100ULL - 11644473600000000000ULL;  // 100ns -> ns epoch
  }
  file_handle_ = fh;
  map_handle_ = mh;
  data_ = static_cast<const std::uint8_t*>(p);
  size_ = static_cast<std::size_t>(file_size);
  mtime_ns_ = mtime;
  return true;
#else
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return false;
  }
  struct stat st {};
  if (::fstat(fd, &st) != 0 || st.st_size <= 0) {
    ::close(fd);
    return false;
  }
  void* p = ::mmap(nullptr, static_cast<std::size_t>(st.st_size), PROT_READ,
                   MAP_PRIVATE, fd, 0);
  if (p == MAP_FAILED) {
    ::close(fd);
    return false;
  }
  // Advise sequential read: the header parsers scan once, front to back.
  ::madvise(p, static_cast<std::size_t>(st.st_size), MADV_SEQUENTIAL);
  data_ = static_cast<const std::uint8_t*>(p);
  size_ = static_cast<std::size_t>(st.st_size);
#if defined(__APPLE__)
  mtime_ns_ = static_cast<std::uint64_t>(st.st_mtimespec.tv_sec) * 1000000000ULL +
              static_cast<std::uint64_t>(st.st_mtimespec.tv_nsec);
#else
  mtime_ns_ = static_cast<std::uint64_t>(st.st_mtim.tv_sec) * 1000000000ULL +
              static_cast<std::uint64_t>(st.st_mtim.tv_nsec);
#endif
  // POSIX maps survive close; release the fd now so we never leak it.
  ::close(fd);
  fd_ = -1;
  return true;
#endif
}

auto MappedFile::unmap() noexcept -> void {
  if (data_ == nullptr) {
    return;
  }
#if defined(_WIN32)
  UnmapViewOfFile(data_);
  if (map_handle_ != nullptr) {
    CloseHandle(map_handle_);
    map_handle_ = nullptr;
  }
  if (file_handle_ != nullptr) {
    CloseHandle(file_handle_);
    file_handle_ = nullptr;
  }
#else
  ::munmap(const_cast<std::uint8_t*>(data_), size_);
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
#endif
  data_ = nullptr;
  size_ = 0;
  mtime_ns_ = 0;
}

}  // namespace engine::assets
