#include "adsb/feed/shm_ring.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

#include "adsb/common/clock.hpp"

namespace adsb::feed {

namespace {

void set_error(std::string* error, const std::string& what) {
  if (error) *error = what;
}

std::string errno_text(const std::string& what) { return what + ": " + std::strerror(errno); }

bool valid_name(const std::string& name) {
  // POSIX: a leading slash and no other; macOS limits names to 31 bytes.
  return name.size() >= 2 && name.size() <= 31 && name[0] == '/' &&
         name.find('/', 1) == std::string::npos;
}

std::uint64_t make_instance() {
  static std::atomic<std::uint64_t> counter{0};
  std::uint64_t x = wall_ns() ^ (static_cast<std::uint64_t>(::getpid()) << 32) ^
                    (counter.fetch_add(1) * 0x9E3779B97F4A7C15ULL);
  // splitmix64 finalizer
  x ^= x >> 30;
  x *= 0xBF58476D1CE4E5B9ULL;
  x ^= x >> 27;
  x *= 0x94D049BB133111EBULL;
  x ^= x >> 31;
  return x == 0 ? 1 : x;
}

}  // namespace

ShmRegion::~ShmRegion() { close(); }

ShmRegion& ShmRegion::operator=(ShmRegion&& o) noexcept {
  if (this != &o) {
    close();
    base_ = std::exchange(o.base_, nullptr);
    bytes_ = std::exchange(o.bytes_, 0);
    owner_ = std::exchange(o.owner_, false);
    huge_ = o.huge_;
    name_ = std::move(o.name_);
    view_ = o.view_;
    o.view_ = RingView{};
  }
  return *this;
}

void ShmRegion::close() noexcept {
  // Remove the name only if it still refers to our ring: after a restart, a
  // new publisher may already have replaced it, and unlinking then would
  // delete the new ring. (The check and the unlink are not atomic; a writer
  // replacing the ring in that instant is the only way to lose the race.)
  if (owner_ && base_ && current_instance(name_) == view_.instance()) unlink(name_);
  if (base_) ::munmap(base_, bytes_);
  base_ = nullptr;
  bytes_ = 0;
  owner_ = false;
  view_ = RingView{};
}

void ShmRegion::unlink(const std::string& name) noexcept { ::shm_unlink(name.c_str()); }

bool ShmRegion::create(const std::string& name, std::uint64_t capacity, const protocol::Session& session,
                       std::uint64_t first_seq, std::string* error) {
  close();
  if (!valid_name(name)) {
    set_error(error, "shm name must be '/name', at most 31 bytes: " + name);
    return false;
  }
  if (capacity < 2 || capacity > kShmMaxSlots || (capacity & (capacity - 1)) != 0) {
    set_error(error, "shm ring capacity must be a power of two in [2, 2^24]");
    return false;
  }
  if (first_seq == 0) {  // seq 0 marks a never-written slot (feed sequences start at 1)
    set_error(error, "shm ring first_seq must be at least 1");
    return false;
  }
  // A previous session's object: readers that still map it keep it alive and
  // see its end-of-session marker (or a writer that stopped writing).
  ::shm_unlink(name.c_str());
  const int fd = ::shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0644);
  if (fd < 0) {
    set_error(error, errno_text("shm_open " + name));
    return false;
  }
  const std::size_t bytes = shm_region_bytes(capacity);
  if (::ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
    set_error(error, errno_text("ftruncate " + name));
    ::close(fd);
    ::shm_unlink(name.c_str());
    return false;
  }
  void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  ::close(fd);
  if (p == MAP_FAILED) {
    set_error(error, errno_text("mmap " + name));
    ::shm_unlink(name.c_str());
    return false;
  }
#if defined(__linux__) && defined(MADV_HUGEPAGE)
  // Transparent huge pages for the slots, if the kernel allows them for shmem
  // (/sys/kernel/mm/transparent_hugepage/shmem_enabled). Best effort.
  huge_ = ::madvise(p, bytes, MADV_HUGEPAGE) == 0;
#endif
  base_ = p;
  bytes_ = bytes;
  owner_ = true;
  name_ = name;
  view_ = RingView(p, capacity);
  view_.format(session, first_seq, make_instance(), static_cast<std::int64_t>(::getpid()));
  return true;
}

bool ShmRegion::open(const std::string& name, std::string* error) {
  close();
  if (!valid_name(name)) {
    set_error(error, "shm name must be '/name', at most 31 bytes: " + name);
    return false;
  }
  const int fd = ::shm_open(name.c_str(), O_RDONLY, 0);
  if (fd < 0) {
    set_error(error, errno_text("shm_open " + name));
    return false;
  }
  struct stat st {};
  if (::fstat(fd, &st) != 0 || st.st_size < static_cast<off_t>(kShmHeaderBytes)) {
    set_error(error, name + ": not a feed ring (too small)");
    ::close(fd);
    return false;
  }
  const auto bytes = static_cast<std::size_t>(st.st_size);
  int flags = MAP_SHARED;
#if defined(__linux__) && defined(MAP_POPULATE)
  flags |= MAP_POPULATE;  // fault the pages in now, not on the first reads
#endif
  void* p = ::mmap(nullptr, bytes, PROT_READ, flags, fd, 0);
  ::close(fd);
  if (p == MAP_FAILED) {
    set_error(error, errno_text("mmap " + name));
    return false;
  }
  const auto* h = static_cast<const ShmHeader*>(p);
  // The magic is stored last with release: seeing it makes the rest of the header visible.
  const bool ok = h->magic.load(std::memory_order_acquire) == kShmMagic && h->version == kShmVersion &&
                  h->slot_bytes == kShmSlotBytes && h->capacity >= 2 && h->capacity <= kShmMaxSlots &&
                  (h->capacity & (h->capacity - 1)) == 0 && shm_region_bytes(h->capacity) <= bytes;
  if (!ok) {
    set_error(error, name + ": not a feed ring (bad header, wrong version, or still being created)");
    ::munmap(p, bytes);
    return false;
  }
  base_ = p;
  bytes_ = bytes;
  owner_ = false;
  name_ = name;
  view_ = RingView(p, h->capacity);
  return true;
}

std::uint64_t ShmRegion::current_instance(const std::string& name) noexcept {
  if (!valid_name(name)) return 0;
  const int fd = ::shm_open(name.c_str(), O_RDONLY, 0);
  if (fd < 0) return 0;
  struct stat st {};
  std::uint64_t id = 0;
  if (::fstat(fd, &st) == 0 && st.st_size >= static_cast<off_t>(kShmHeaderBytes)) {
    void* p = ::mmap(nullptr, kShmHeaderBytes, PROT_READ, MAP_SHARED, fd, 0);
    if (p != MAP_FAILED) {
      const auto* h = static_cast<const ShmHeader*>(p);
      if (h->magic.load(std::memory_order_acquire) == kShmMagic) id = h->instance;
      ::munmap(p, kShmHeaderBytes);
    }
  }
  ::close(fd);
  return id;
}

}  // namespace adsb::feed
