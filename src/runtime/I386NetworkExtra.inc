#include <cstddef>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <net/if.h>
#include <netinet/in.h>
#ifdef __linux__
#include <linux/if_link.h>
#include <netpacket/packet.h>
#endif
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#ifdef __linux__
#include <sys/random.h>
#endif
#include <sys/uio.h>
#include <termios.h>
#include <sys/utsname.h>

namespace {
std::mutex netextra_guest_alloc_mutex;
struct NetExtraOwned { void *host; std::vector<uint32_t> guest_blocks; };
std::map<uint32_t, NetExtraOwned> netextra_addrinfo_roots;
std::map<uint32_t, NetExtraOwned> netextra_ifaddrs_roots;
thread_local std::vector<uint32_t> netextra_hostent_blocks;
thread_local uint32_t netextra_hostent_root = 0;
thread_local std::vector<uint32_t> *netextra_active_blocks = nullptr;
struct NetExtraSignal {
  uint32_t arena_handler = 0;
  uint8_t *arena = nullptr;
  State *state = nullptr;
  RuntimeManager *runtime = nullptr;
  uint32_t guest_flags = 0;
  uint32_t guest_restorer = 0;
  sigset_t guest_mask{};
};
std::mutex netextra_signal_mutex;
std::map<std::pair<RuntimeManager *, int>, NetExtraSignal> netextra_signals;
thread_local volatile sig_atomic_t netextra_pending_signals[NSIG] = {};
thread_local bool netextra_dispatching_signals = false;
thread_local bool netextra_jump_saved_mask = false;
NetExtraSignal netextra_signal_get(RuntimeManager *runtime, int sig) {
  std::lock_guard<std::mutex> lock(netextra_signal_mutex);
  auto it = netextra_signals.find({runtime, sig});
  return it == netextra_signals.end() ? NetExtraSignal{} : it->second;
}
NetExtraSignal netextra_signal_set(RuntimeManager *runtime, int sig, const NetExtraSignal &entry) {
  std::lock_guard<std::mutex> lock(netextra_signal_mutex);
  auto key = std::make_pair(runtime, sig);
  auto it = netextra_signals.find(key);
  NetExtraSignal old = it == netextra_signals.end() ? NetExtraSignal{} : it->second;
  netextra_signals[key] = entry;
  return old;
}
void netextra_signal_trampoline(int number) {
  // Host async handlers only enqueue a coalesced standard signal; guest code runs at a safe point.
  if (number > 0 && number < NSIG) netextra_pending_signals[number] = 1;
}
void netextra_guest_sigset(uint8_t *arena, uint32_t address, sigset_t &result) {
  sigemptyset(&result);
  if (!address) return;
  uint32_t words[32]; memcpy(words, guest(arena, address, sizeof(words)), sizeof(words));
  for (int sig = 1; sig < NSIG && sig <= 1024; ++sig)
    if ((words[(sig - 1) >> 5] >> ((sig - 1) & 31)) & 1) sigaddset(&result, sig);
}
void netextra_store_sigset(uint8_t *arena, uint32_t address, const sigset_t &value) {
  if (!address) return;
  uint32_t words[32] = {};
  for (int sig = 1; sig < NSIG && sig <= 1024; ++sig)
    if (sigismember(&value, sig) == 1) words[(sig - 1) >> 5] |= 1u << ((sig - 1) & 31);
  memcpy(guest(arena, address, sizeof(words)), words, sizeof(words));
}
struct NetExtraGuestSigaction { uint32_t handler, mask[32], flags, restorer; };
static_assert(sizeof(NetExtraGuestSigaction) == 140 &&
              offsetof(NetExtraGuestSigaction, flags) == 132 &&
              offsetof(NetExtraGuestSigaction, restorer) == 136,
              "i386 glibc sigaction ABI mismatch");
uint32_t netextra_alloc(size_t, std::vector<uint32_t> *);
uint32_t netextra_copy_sockaddr(uint8_t *, const sockaddr *, socklen_t, std::vector<uint32_t> *);
uint32_t netextra_alloc(RuntimeManager *, size_t length, size_t = 4) {
  return netextra_alloc(length, netextra_active_blocks);
}
uint32_t netextra_copy_sockaddr(uint8_t *arena, RuntimeManager *runtime,
                                const sockaddr *address, socklen_t length) {
  return netextra_copy_sockaddr(arena, address, length, netextra_active_blocks);
}
uint32_t netextra_alloc(size_t length, std::vector<uint32_t> *blocks) {
  if (length > UINT32_MAX) { errno = ENOMEM; return 0; }
  uint32_t address = allocate(uint32_t(length));
  if (address && blocks) blocks->push_back(address);
  return address;
}



uint32_t netextra_copy_sockaddr(uint8_t *arena, const sockaddr *address, socklen_t length,
                               std::vector<uint32_t> *blocks) {
  if (!address || !length) return 0;
  uint32_t result = netextra_alloc(length, blocks);
  if (result) memcpy(guest(arena, result, length), address, length);
  return result;
}
ssize_t netextra_message(uint8_t *arena, uint32_t descriptor, uint32_t message,
                         int flags, bool receive) {
  uint32_t fields[7]; memcpy(fields, guest(arena, message, sizeof(fields)), sizeof(fields));
  const uint32_t name_address = fields[0], name_capacity = fields[1];
  const uint32_t iov_address = fields[2], iov_count = fields[3];
  const uint32_t control_address = fields[4], control_capacity = fields[5];
  // Ancillary cmsghdr has different i386/host alignment; do not pass or truncate it.
  if (iov_count > 1024 || uint64_t(iov_count) * 8 > MEMORY_ARENA_SIZE ||
      control_capacity) { errno = control_capacity ? EOPNOTSUPP : EINVAL; return -1; }
  sockaddr_storage name{};
  if (name_capacity > sizeof(name)) { errno = EINVAL; return -1; }
  if (name_address && name_capacity) memcpy(&name, guest(arena, name_address, name_capacity), name_capacity);
  std::vector<iovec> vectors(iov_count);
  for (uint32_t i = 0; i < iov_count; ++i) {
    uint32_t pair[2];
    memcpy(pair, guest(arena, iov_address + i * 8, 8), 8);
    vectors[i].iov_base = pair[1] ? guest(arena, pair[0], pair[1]) : nullptr;
    vectors[i].iov_len = pair[1];
  }
  msghdr host{};
  host.msg_name = name_address ? &name : nullptr;
  host.msg_namelen = name_address ? name_capacity : 0;
  host.msg_iov = vectors.empty() ? nullptr : vectors.data();
  host.msg_iovlen = vectors.size();
  host.msg_control = nullptr;
  host.msg_controllen = 0;
  ssize_t result = receive ? recvmsg(int32_t(descriptor), &host, flags)
                           : sendmsg(int32_t(descriptor), &host, flags);
  if (result >= 0 && receive) {
    if (name_address) {
      uint32_t copied = std::min<uint32_t>(name_capacity, host.msg_namelen);
      if (copied) memcpy(guest(arena, name_address, copied), &name, copied);
    }
    fields[1] = host.msg_namelen;
    fields[5] = uint32_t(host.msg_controllen);
    fields[6] = uint32_t(host.msg_flags);
    memcpy(guest(arena, message, sizeof(fields)), fields, sizeof(fields));
  }
  (void)control_address;
  return result;
}
bool netextra_timeval_option(int level, int option) {
  return level == SOL_SOCKET &&
      (option == SO_RCVTIMEO || option == SO_SNDTIMEO);
}
int netextra_setsockopt_guest(uint8_t *arena, int fd, int level, int option,
                              uint32_t value, int32_t length) {
  if (length < 0) { errno = EINVAL; return -1; }
  if (netextra_timeval_option(level, option)) {
    if (length != 8) { errno = EINVAL; return -1; }
    int32_t guest_time[2]; memcpy(guest_time, guest(arena, value, 8), 8);
    timeval host_time{guest_time[0], guest_time[1]};
    return setsockopt(fd, level, option, &host_time, sizeof(host_time));
  }
  return setsockopt(fd, level, option, length ? guest(arena, value, length) : nullptr, length);
}
int netextra_getsockopt_guest(uint8_t *arena, int fd, int level, int option,
                              uint32_t value, uint32_t length_address) {
  uint32_t capacity; memcpy(&capacity, guest(arena, length_address, 4), 4);
  if (netextra_timeval_option(level, option)) {
    timeval host_time{}; socklen_t host_length = sizeof(host_time);
    int result = getsockopt(fd, level, option, &host_time, &host_length);
    if (!result) {
      if (host_time.tv_sec < INT32_MIN || host_time.tv_sec > INT32_MAX) {
        errno = EOVERFLOW; return -1;
      }
      int32_t guest_time[2] = {int32_t(host_time.tv_sec), int32_t(host_time.tv_usec)};
      uint32_t copied = std::min<uint32_t>(capacity, sizeof(guest_time));
      if (copied) memcpy(guest(arena, value, copied), guest_time, copied);
      uint32_t output_length = sizeof(guest_time);
      memcpy(guest(arena, length_address, 4), &output_length, 4);
    }
    return result;
  }
  if (capacity > MEMORY_ARENA_SIZE) { errno = EINVAL; return -1; }
  std::vector<uint8_t> bytes(capacity);
  socklen_t host_length = capacity;
  int result = getsockopt(fd, level, option, capacity ? bytes.data() : nullptr, &host_length);
  if (!result) {
    uint32_t copied = std::min<uint32_t>(capacity, host_length);
    if (copied) memcpy(guest(arena, value, copied), bytes.data(), copied);
    memcpy(guest(arena, length_address, 4), &host_length, 4);
  }
  return result;
}
int netextra_accept4_guest(uint8_t *arena, int listener, uint32_t address,
                           uint32_t length_address, int flags) {
  uint32_t capacity = 0;
  if (address) {
    if (!length_address) { errno = EFAULT; return -1; }
    memcpy(&capacity, guest(arena, length_address, 4), 4);
  }
  sockaddr_storage host_address{}; socklen_t host_length = sizeof(host_address);
#ifdef __linux__
  int fd = accept4(listener, address ? reinterpret_cast<sockaddr *>(&host_address) : nullptr,
                   address ? &host_length : nullptr, flags);
#else
  int fd = accept(listener, address ? reinterpret_cast<sockaddr *>(&host_address) : nullptr,
                  address ? &host_length : nullptr);
  if (fd >= 0 && (flags & SOCK_CLOEXEC) && fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
    int error = errno; close(fd); errno = error; return -1;
  }
  if (fd >= 0 && (flags & SOCK_NONBLOCK) && fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) < 0) {
    int error = errno; close(fd); errno = error; return -1;
  }
#endif
  if (fd >= 0 && address) {
    uint32_t copied = std::min<uint32_t>(capacity, host_length);
    if (copied) memcpy(guest(arena, address, copied), &host_address, copied);
    memcpy(guest(arena, length_address, 4), &host_length, 4);
  }
  return fd;
}
}

IMPORT(socket) { CALL; call.result(socket(int32_t(call.word(0)), int32_t(call.word(1)), int32_t(call.word(2)))); }
IMPORT(bind) {
  CALL;
  uint32_t length = call.word(2);
  if (length > sizeof(sockaddr_storage)) { errno = EINVAL; call.result(-1); return; }
  sockaddr_storage address{};
  if (length) memcpy(&address, guest(arena, call.word(1), length), length);
  call.result(bind(int32_t(call.word(0)), reinterpret_cast<sockaddr *>(&address), length));
}
IMPORT(connect) {
  CALL;
  uint32_t length = call.word(2);
  if (length > sizeof(sockaddr_storage)) { errno = EINVAL; call.result(-1); return; }
  sockaddr_storage address{};
  if (length) memcpy(&address, guest(arena, call.word(1), length), length);
  call.result(connect(int32_t(call.word(0)), reinterpret_cast<sockaddr *>(&address), length));
}
IMPORT(accept) {
  CALL;
  uint32_t address_pointer = call.word(1), length_pointer = call.word(2);
  sockaddr_storage address{};
  socklen_t length = sizeof(address);
  int fd = accept(int32_t(call.word(0)), address_pointer ? reinterpret_cast<sockaddr *>(&address) : nullptr,
                  address_pointer ? &length : nullptr);
  if (fd >= 0 && address_pointer) {
    uint32_t guest_length;
    memcpy(&guest_length, guest(arena, length_pointer, 4), 4);
    const uint32_t copied = std::min<uint32_t>(guest_length, length);
    if (copied) memcpy(guest(arena, address_pointer, copied), &address, copied);
    uint32_t output_length = length;
    memcpy(guest(arena, length_pointer, 4), &output_length, 4);
  }
  call.result(fd);
}
IMPORT(listen) { CALL; call.result(listen(int32_t(call.word(0)), int32_t(call.word(1)))); }
IMPORT(shutdown) { CALL; call.result(shutdown(int32_t(call.word(0)), int32_t(call.word(1)))); }
IMPORT(getsockname) {
  CALL;
  uint32_t length_pointer = call.word(2);
  uint32_t length; memcpy(&length, guest(arena, length_pointer, 4), 4);
  sockaddr_storage address{}; socklen_t host_length = sizeof(address);
  int result = getsockname(int32_t(call.word(0)), reinterpret_cast<sockaddr *>(&address), &host_length);
  if (!result) {
    uint32_t copied = std::min<uint32_t>(length, host_length);
    if (copied) memcpy(guest(arena, call.word(1), copied), &address, copied);
    uint32_t value = host_length; memcpy(guest(arena, length_pointer, 4), &value, 4);
  }
  call.result(result);
}
IMPORT(getpeername) {
  CALL;
  uint32_t length_pointer = call.word(2);
  uint32_t length; memcpy(&length, guest(arena, length_pointer, 4), 4);
  sockaddr_storage address{}; socklen_t host_length = sizeof(address);
  int result = getpeername(int32_t(call.word(0)), reinterpret_cast<sockaddr *>(&address), &host_length);
  if (!result) {
    uint32_t copied = std::min<uint32_t>(length, host_length);
    if (copied) memcpy(guest(arena, call.word(1), copied), &address, copied);
    uint32_t value = host_length; memcpy(guest(arena, length_pointer, 4), &value, 4);
  }
  call.result(result);
}
IMPORT(send) {
  CALL; int32_t length = int32_t(call.word(2));
  if (length < 0) { errno = EINVAL; call.result(-1); return; }
  call.result(send(int32_t(call.word(0)), length ? guest(arena, call.word(1), length) : nullptr,
                   length, call.word(3)));
}
IMPORT(recv) {
  CALL; int32_t length = int32_t(call.word(2));
  if (length < 0) { errno = EINVAL; call.result(-1); return; }
  call.result(recv(int32_t(call.word(0)), length ? guest(arena, call.word(1), length) : nullptr,
                   length, call.word(3)));
}
IMPORT(sendto) {
  CALL; int32_t length = int32_t(call.word(2)); uint32_t address_length = call.word(5);
  if (length < 0 || address_length > sizeof(sockaddr_storage)) { errno = EINVAL; call.result(-1); return; }
  sockaddr_storage address{};
  if (address_length) memcpy(&address, guest(arena, call.word(4), address_length), address_length);
  call.result(sendto(int32_t(call.word(0)), length ? guest(arena, call.word(1), length) : nullptr,
      length, call.word(3), address_length ? reinterpret_cast<sockaddr *>(&address) : nullptr, address_length));
}
IMPORT(recvfrom) {
  CALL; int32_t length = int32_t(call.word(2)); uint32_t address_pointer = call.word(4), length_pointer = call.word(5);
  if (length < 0) { errno = EINVAL; call.result(-1); return; }
  sockaddr_storage address{}; socklen_t host_length = sizeof(address);
  ssize_t result = recvfrom(int32_t(call.word(0)), length ? guest(arena, call.word(1), length) : nullptr,
      length, call.word(3), address_pointer ? reinterpret_cast<sockaddr *>(&address) : nullptr,
      address_pointer ? &host_length : nullptr);
  if (result >= 0 && address_pointer) {
    uint32_t capacity; memcpy(&capacity, guest(arena, length_pointer, 4), 4);
    uint32_t copied = std::min<uint32_t>(capacity, host_length);
    if (copied) memcpy(guest(arena, address_pointer, copied), &address, copied);
    uint32_t value = host_length; memcpy(guest(arena, length_pointer, 4), &value, 4);
  }
  call.result(result);
}
IMPORT(getsockopt) {
  CALL;
  call.result(netextra_getsockopt_guest(arena, int32_t(call.word(0)), int32_t(call.word(1)),
      int32_t(call.word(2)), call.word(3), call.word(4)));
}
IMPORT(setsockopt) {
  CALL;
  call.result(netextra_setsockopt_guest(arena, int32_t(call.word(0)), int32_t(call.word(1)),
      int32_t(call.word(2)), call.word(3), int32_t(call.word(4))));
}
IMPORT(poll) {
  CALL; int32_t count = int32_t(call.word(1));
  if (count < 0 || uint64_t(count) * sizeof(pollfd) > MEMORY_ARENA_SIZE) { errno = EINVAL; call.result(-1); return; }
  std::vector<pollfd> fds(count);
  for (int32_t i = 0; i < count; ++i) {
    uint8_t *p = static_cast<uint8_t *>(guest(arena, call.word(0) + uint32_t(i) * 8, 8));
    memcpy(&fds[i].fd, p, 4); memcpy(&fds[i].events, p + 4, 2); memcpy(&fds[i].revents, p + 6, 2);
  }
  int result = poll(count ? fds.data() : nullptr, count, int32_t(call.word(2)));
  for (int32_t i = 0; i < count; ++i)
    memcpy(guest(arena, call.word(0) + uint32_t(i) * 8 + 6, 2), &fds[i].revents, 2);
  call.result(result);
}
IMPORT(select) {
  CALL;
  int32_t nfds = int32_t(call.word(0));
  if (nfds < 0 || nfds > FD_SETSIZE) { errno = EINVAL; call.result(-1); return; }
  fd_set read_set, write_set, except_set;
  fd_set *r = nullptr, *w = nullptr, *e = nullptr;
  auto load = [&](uint32_t address, fd_set &set, fd_set *&target) {
    if (!address) return;
    target = &set; FD_ZERO(&set);
    for (int fd = 0; fd < nfds; ++fd) {
      uint32_t word; memcpy(&word, guest(arena, address + (fd >> 5) * 4, 4), 4);
      if ((word >> (fd & 31)) & 1) FD_SET(fd, &set);
    }
  };
  load(call.word(1), read_set, r); load(call.word(2), write_set, w); load(call.word(3), except_set, e);
  timeval timeout, *timeout_pointer = nullptr;
  if (call.word(4)) {
    int32_t values[2]; memcpy(values, guest(arena, call.word(4), 8), 8);
    timeout.tv_sec = values[0]; timeout.tv_usec = values[1]; timeout_pointer = &timeout;
  }
  int result = select(nfds, r, w, e, timeout_pointer);
  auto store = [&](uint32_t address, fd_set *set) {
    if (!address) return;
    for (int word_index = 0; word_index < (nfds + 31) / 32; ++word_index) {
      uint32_t bits = 0;
      for (int bit = 0; bit < 32 && word_index * 32 + bit < nfds; ++bit)
        if (FD_ISSET(word_index * 32 + bit, set)) bits |= 1u << bit;
      memcpy(guest(arena, address + word_index * 4, 4), &bits, 4);
    }
  };
  if (result >= 0) { store(call.word(1), r); store(call.word(2), w); store(call.word(3), e); }
  if (timeout_pointer && call.word(4)) { int32_t values[2] = {int32_t(timeout.tv_sec), int32_t(timeout.tv_usec)}; memcpy(guest(arena, call.word(4), 8), values, 8); }
  call.result(result);
}
IMPORT(ioctl) {
  CALL;
  unsigned long request = call.word(1); uint32_t argument = call.word(2);
  unsigned size = (request >> 16) & 0x3fff;
  unsigned direction = (request >> 30) & 3;
  if (!size) {
#ifdef TIOCGWINSZ
    if (request == TIOCGWINSZ) { size = sizeof(struct winsize); direction = 2; }
#endif
#ifdef TIOCSWINSZ
    if (request == TIOCSWINSZ) { size = sizeof(struct winsize); direction = 1; }
#endif
#ifdef FIONREAD
    if (request == FIONREAD) { size = sizeof(int); direction = 2; }
#endif
#ifdef FIONBIO
    if (request == FIONBIO) { size = sizeof(int); direction = 1; }
#endif
#ifdef TCGETS
    if (request == TCGETS) { size = sizeof(struct termios); direction = 2; }
#endif
#ifdef TCSETS
    if (request == TCSETS || request == TCSETSW || request == TCSETSF) {
      size = sizeof(struct termios); direction = 1;
    }
#endif
#ifdef TIOCGPGRP
    if (request == TIOCGPGRP) { size = sizeof(pid_t); direction = 2; }
#endif
#ifdef TIOCSPGRP
    if (request == TIOCSPGRP) { size = sizeof(pid_t); direction = 1; }
#endif
  }
  if (size > MEMORY_ARENA_SIZE) { errno = EINVAL; call.result(-1); return; }
  if (!size || !argument) { call.result(ioctl(int32_t(call.word(0)), request, uintptr_t(argument))); return; }
  std::vector<uint8_t> bytes(size);
  if (direction & 1) memcpy(bytes.data(), guest(arena, argument, size), size);
  int result = ioctl(int32_t(call.word(0)), request, bytes.data());
  if (result >= 0 && (direction & 2)) memcpy(guest(arena, argument, size), bytes.data(), size);
  call.result(result);
}
IMPORT(inet_addr) { CALL; call.result(inet_addr(text(arena, call.word(0)))); }
IMPORT(inet_pton) {
  CALL; int32_t family = call.word(0);
  size_t size = family == AF_INET ? 4 : family == AF_INET6 ? 16 : 0;
  if (!size) { errno = EAFNOSUPPORT; call.result(-1); return; }
  uint8_t result[16];
  int status = inet_pton(family, text(arena, call.word(1)), result);
  if (status == 1) memcpy(guest(arena, call.word(2), size), result, size);
  call.result(status);
}
IMPORT(inet_ntop) {
  CALL; int32_t family = call.word(0);
  size_t size = family == AF_INET ? 4 : family == AF_INET6 ? 16 : 0;
  if (!size) { errno = EAFNOSUPPORT; call.result(0); return; }
  char result[INET6_ADDRSTRLEN];
  if (!inet_ntop(family, guest(arena, call.word(1), size), result, sizeof(result))) { call.result(0); return; }
  uint32_t capacity = call.word(3);
  size_t length = strlen(result) + 1;
  if (length > capacity) { errno = ENOSPC; call.result(0); return; }
  memcpy(guest(arena, call.word(2), length), result, length);
  call.result(call.word(2));
}
IMPORT(gethostname) {
  CALL; uint32_t length = call.word(1);
  if (!length || length > MEMORY_ARENA_SIZE) { errno = EINVAL; call.result(-1); return; }
  std::vector<char> name(length);
  int result = gethostname(name.data(), length);
  if (!result) memcpy(guest(arena, call.word(0), length), name.data(), length);
  call.result(result);
}
IMPORT(gethostbyname) {
  CALL;
  for (uint32_t block : netextra_hostent_blocks) release(block);
  netextra_hostent_blocks.clear(); netextra_hostent_root = 0;
  hostent *host = gethostbyname(text(arena, call.word(0)));
  if (!host) { call.result(0); return; }
  auto *prior_blocks = netextra_active_blocks; netextra_active_blocks = &netextra_hostent_blocks;
  size_t alias_count = 0, address_count = 0;
  if (host->h_aliases) while (host->h_aliases[alias_count]) ++alias_count;
  if (host->h_addr_list) while (host->h_addr_list[address_count]) ++address_count;
  uint32_t root = netextra_alloc(runtime, 20);
  uint32_t name = netextra_alloc(runtime, strlen(host->h_name) + 1);
  uint32_t aliases = netextra_alloc(runtime, (alias_count + 1) * 4);
  uint32_t list = netextra_alloc(runtime, (address_count + 1) * 4);
  bool failed = !root || !name || !aliases || !list || !address_count || host->h_length <= 0;
  std::vector<uint32_t> alias_strings(alias_count), address_values(address_count);
  for (size_t i = 0; i < alias_count && !failed; ++i) {
    alias_strings[i] = netextra_alloc(runtime, strlen(host->h_aliases[i]) + 1);
    if (!alias_strings[i]) { failed = true; break; }
    memcpy(guest(arena, alias_strings[i], strlen(host->h_aliases[i]) + 1),
           host->h_aliases[i], strlen(host->h_aliases[i]) + 1);
  }
  for (size_t i = 0; i < address_count && !failed; ++i) {
    address_values[i] = netextra_alloc(runtime, host->h_length);
    if (!address_values[i]) { failed = true; break; }
    memcpy(guest(arena, address_values[i], host->h_length),
           host->h_addr_list[i], host->h_length);
  }
  if (!failed) {
    memcpy(guest(arena, name, strlen(host->h_name) + 1), host->h_name, strlen(host->h_name) + 1);
    for (size_t i = 0; i < alias_count; ++i) memcpy(guest(arena, aliases + i * 4, 4), &alias_strings[i], 4);
    for (size_t i = 0; i < address_count; ++i) memcpy(guest(arena, list + i * 4, 4), &address_values[i], 4);
    uint32_t null = 0;
    memcpy(guest(arena, aliases + alias_count * 4, 4), &null, 4);
    memcpy(guest(arena, list + address_count * 4, 4), &null, 4);
    uint32_t fields[5] = {name, aliases, uint32_t(host->h_addrtype), uint32_t(host->h_length), list};
    memcpy(guest(arena, root, sizeof(fields)), fields, sizeof(fields));
    netextra_hostent_root = root;
  }
  netextra_active_blocks = prior_blocks;
  if (failed) {
    for (uint32_t block : netextra_hostent_blocks) release(block);
    netextra_hostent_blocks.clear(); errno = ENOMEM; call.result(0); return;
  }
  call.result(netextra_hostent_root);
}
IMPORT(getaddrinfo) {
  CALL; addrinfo hints{}; addrinfo *hints_pointer = nullptr;
  if (!call.word(3)) { errno = EFAULT; call.result(EAI_FAIL); return; }
  if (call.word(2)) {
    uint32_t words[8]; memcpy(words, guest(arena, call.word(2), sizeof(words)), sizeof(words));
    hints.ai_flags = int32_t(words[0]); hints.ai_family = int32_t(words[1]);
    hints.ai_socktype = int32_t(words[2]); hints.ai_protocol = int32_t(words[3]);
    hints_pointer = &hints;
  }
  addrinfo *result = nullptr;
  int status = getaddrinfo(call.word(0) ? text(arena, call.word(0)) : nullptr,
      call.word(1) ? text(arena, call.word(1)) : nullptr, hints_pointer, &result);
  if (status) { call.result(status); return; }
  size_t count = 0; for (addrinfo *p = result; p; p = p->ai_next) ++count;
  if (!count) { freeaddrinfo(result); uint32_t empty = 0; memcpy(guest(arena, call.word(3), 4), &empty, 4); call.result(0); return; }
  std::vector<uint32_t> blocks; auto *prior_blocks = netextra_active_blocks; netextra_active_blocks = &blocks;
  uint32_t *addresses = count ? new uint32_t[count] : nullptr;
  bool failed = false;
  for (size_t i = 0; i < count; ++i) {
    addresses[i] = netextra_alloc(runtime, 32, 4);
    if (!addresses[i]) { failed = true; break; }
  }
  size_t node_index = 0;
  for (addrinfo *p = result; p && !failed; p = p->ai_next, ++node_index) {
    const size_t i = node_index;
    uint32_t address = netextra_copy_sockaddr(arena, p->ai_addr, p->ai_addrlen, &blocks);
    uint32_t canon = 0;
    if (p->ai_canonname) {
      canon = netextra_alloc(runtime, strlen(p->ai_canonname) + 1);
      if (canon) memcpy(guest(arena, canon, strlen(p->ai_canonname) + 1), p->ai_canonname, strlen(p->ai_canonname) + 1);
    }
    if (!address || (p->ai_canonname && !canon)) { failed = true; break; }
    uint32_t fields[8] = {uint32_t(p->ai_flags), uint32_t(p->ai_family), uint32_t(p->ai_socktype), uint32_t(p->ai_protocol), uint32_t(p->ai_addrlen), address, canon, i + 1 < count ? addresses[i + 1] : 0};
    memcpy(guest(arena, addresses[i], sizeof(fields)), fields, sizeof(fields));
  }
  netextra_active_blocks = prior_blocks;
  if (failed) {
    delete[] addresses; freeaddrinfo(result);
    for (uint32_t block : blocks) release(block);
    call.result(EAI_MEMORY); return;
  }
  if (call.word(3)) memcpy(guest(arena, call.word(3), 4), addresses, 4);
  { std::lock_guard<std::mutex> lock(netextra_guest_alloc_mutex);
    netextra_addrinfo_roots[addresses[0]] = {result, std::move(blocks)}; }
  delete[] addresses; call.result(0);
}
IMPORT(freeaddrinfo) {
  CALL; NetExtraOwned owned{};
  { std::lock_guard<std::mutex> lock(netextra_guest_alloc_mutex);
    auto found = netextra_addrinfo_roots.find(call.word(0));
    if (found != netextra_addrinfo_roots.end()) { owned = std::move(found->second); netextra_addrinfo_roots.erase(found); } }
  if (owned.host) freeaddrinfo(static_cast<addrinfo *>(owned.host));
  for (uint32_t block : owned.guest_blocks) release(block);
}
IMPORT(getifaddrs) {
  CALL; ifaddrs *host = nullptr;
  if (!call.word(0)) { errno = EFAULT; call.result(-1); return; }
  int result = getifaddrs(&host);
  if (result) { call.result(result); return; }
  size_t count = 0; for (ifaddrs *p = host; p; p = p->ifa_next) ++count;
  if (!count) { freeifaddrs(host); uint32_t empty = 0; memcpy(guest(arena, call.word(0), 4), &empty, 4); call.result(0); return; }
  std::vector<uint32_t> blocks; auto *prior_blocks = netextra_active_blocks; netextra_active_blocks = &blocks;
  std::vector<uint32_t> nodes(count); size_t i = 0;
  for (; i < count; ++i) { nodes[i] = netextra_alloc(runtime, 28, 4); if (!nodes[i]) break; }
  bool failed = i != count;
  i = 0;
  for (ifaddrs *p = host; p && !failed; p = p->ifa_next, ++i) {
    uint32_t name = netextra_alloc(runtime, strlen(p->ifa_name) + 1);
    if (!name) { failed = true; break; }
    memcpy(guest(arena, name, strlen(p->ifa_name) + 1), p->ifa_name, strlen(p->ifa_name) + 1);
    auto copy_address = [&](sockaddr *a) { return a ? netextra_copy_sockaddr(arena, runtime, a,
        a->sa_family == AF_INET ? sizeof(sockaddr_in) :
        a->sa_family == AF_INET6 ? sizeof(sockaddr_in6) :
#ifdef __linux__
        a->sa_family == AF_PACKET ? sizeof(sockaddr_ll) :
#endif
        sizeof(sockaddr)) : 0; };
    uint32_t address = copy_address(p->ifa_addr), netmask = copy_address(p->ifa_netmask), broad = copy_address(p->ifa_broadaddr);
    uint32_t data = 0;
#ifdef __linux__
    if (p->ifa_data) {
      data = netextra_alloc(runtime, sizeof(struct rtnl_link_stats));
      if (data) memcpy(guest(arena, data, sizeof(struct rtnl_link_stats)), p->ifa_data, sizeof(struct rtnl_link_stats));
    }
    if ((p->ifa_addr && !address) || (p->ifa_netmask && !netmask) ||
        (p->ifa_broadaddr && !broad)
#ifdef __linux__
        || (p->ifa_data && !data)
#endif
        ) { failed = true; break; }
    uint32_t fields[7] = {i + 1 < count ? nodes[i + 1] : 0, name, p->ifa_flags,
                          address, netmask, broad, data};
    memcpy(guest(arena, nodes[i], sizeof(fields)), fields, sizeof(fields));
  }
  netextra_active_blocks = prior_blocks;
  if (failed) {
    freeifaddrs(host);
    for (uint32_t block : blocks) release(block);
    errno = ENOMEM; call.result(-1); return;
  }
  if (call.word(0)) memcpy(guest(arena, call.word(0), 4), &nodes[0], 4);
  { std::lock_guard<std::mutex> lock(netextra_guest_alloc_mutex);
    netextra_ifaddrs_roots[nodes[0]] = {host, std::move(blocks)}; }
  call.result(0);
}
IMPORT(freeifaddrs) {
  CALL; NetExtraOwned owned{};
  { std::lock_guard<std::mutex> lock(netextra_guest_alloc_mutex);
    auto found = netextra_ifaddrs_roots.find(call.word(0));
    if (found != netextra_ifaddrs_roots.end()) { owned = std::move(found->second); netextra_ifaddrs_roots.erase(found); } }
  if (owned.host) freeifaddrs(static_cast<ifaddrs *>(owned.host));
  for (uint32_t block : owned.guest_blocks) release(block);
}
IMPORT(sigemptyset) {
  CALL; uint32_t words[32] = {};
  memcpy(guest(arena, call.word(0), sizeof(words)), words, sizeof(words)); call.result(0);
}
IMPORT(sigaddset) {
  CALL; int32_t sig = int32_t(call.word(1));
  if (sig <= 0 || sig >= NSIG) { errno = EINVAL; call.result(-1); return; }
  uint32_t address = call.word(0), word;
  memcpy(&word, guest(arena, address + uint32_t((sig - 1) >> 5) * 4, 4), 4);
  word |= 1u << ((sig - 1) & 31);
  memcpy(guest(arena, address + uint32_t((sig - 1) >> 5) * 4, 4), &word, 4);
  call.result(0);
}
IMPORT(sigprocmask) {
  CALL; int32_t how = int32_t(call.word(0));
  sigset_t set, old; sigset_t *set_pointer = nullptr;
  if (call.word(1)) { netextra_guest_sigset(arena, call.word(1), set); set_pointer = &set; }
  int result = sigprocmask(how, set_pointer, call.word(2) ? &old : nullptr);
  if (!result && call.word(2)) netextra_store_sigset(arena, call.word(2), old);
  call.result(result);
}
IMPORT(raise) { CALL; call.result(raise(int32_t(call.word(0)))); }
IMPORT(sigaction) {
  CALL; int32_t sig = int32_t(call.word(0));
  if (sig <= 0 || sig >= NSIG) { errno = EINVAL; call.result(-1); return; }
  NetExtraSignal previous = netextra_signal_get(runtime, sig);
  struct sigaction next{}, old{}; struct sigaction *next_pointer = nullptr;
  if (call.word(1)) {
    NetExtraGuestSigaction guest_action;
    memcpy(&guest_action, guest(arena, call.word(1), sizeof(guest_action)), sizeof(guest_action));
    if (guest_action.flags & (SA_SIGINFO | SA_ONSTACK)) { errno = ENOTSUP; call.result(-1); return; }
    sigset_t mask; netextra_guest_sigset(arena, call.word(1) + 4, mask);
    const uint32_t handler = guest_action.handler;
    NetExtraSignal entry{handler, arena, state, runtime, guest_action.flags,
                         guest_action.restorer, mask};
    netextra_signal_set(runtime, sig, entry);
    sigemptyset(&next.sa_mask); next.sa_mask = mask;
    int host_flags = int32_t(guest_action.flags);
#ifdef SA_RESTORER
    host_flags &= ~SA_RESTORER;
#endif
    next.sa_flags = host_flags;
    next.sa_handler = handler == 0 ? SIG_DFL : handler == 1 ? SIG_IGN : netextra_signal_trampoline;
    next_pointer = &next;
  }
  int result = sigaction(sig, next_pointer, call.word(2) ? &old : nullptr);
  if (result && call.word(1)) netextra_signal_set(runtime, sig, previous);
  if (!result && call.word(2)) {
    NetExtraGuestSigaction output{};
    const bool ours = old.sa_handler == netextra_signal_trampoline;
    uint32_t handler = old.sa_handler == SIG_IGN ? 1 : old.sa_handler == SIG_DFL ? 0 :
        ours ? previous.arena_handler : 0;
    output.handler = handler;
    output.flags = ours ? previous.guest_flags : uint32_t(old.sa_flags);
    output.restorer = ours ? previous.guest_restorer : 0;
    memcpy(guest(arena, call.word(2), sizeof(output)), &output, sizeof(output));
    netextra_store_sigset(arena, call.word(2) + 4, old.sa_mask);
  }
  call.result(result);
}
IMPORT(signal) {
  CALL; int32_t sig = int32_t(call.word(0)); uint32_t handler = call.word(1);
  if (sig <= 0 || sig >= NSIG) { errno = EINVAL; call.result(UINT32_MAX); return; }
  NetExtraSignal previous = netextra_signal_get(runtime, sig);
  struct sigaction next{}, old{}; sigemptyset(&next.sa_mask);
  NetExtraSignal entry{handler, arena, state, runtime, SA_RESTART, 0, next.sa_mask};
  netextra_signal_set(runtime, sig, entry);
  next.sa_handler = handler == 0 ? SIG_DFL : handler == 1 ? SIG_IGN : netextra_signal_trampoline;
  next.sa_flags = SA_RESTART;
  if (sigaction(sig, &next, &old)) { netextra_signal_set(runtime, sig, previous); call.result(UINT32_MAX); return; }
  uint32_t old_handler = old.sa_handler == SIG_IGN ? 1 : old.sa_handler == SIG_DFL ? 0 :
      old.sa_handler == netextra_signal_trampoline ? previous.arena_handler : 0;
  call.result(old_handler);
}
IMPORT(syscall) {
  CALL; int32_t number = int32_t(call.word(0)); long result = -1;
  if (number == 102) {
    uint32_t call_number = call.word(1);
    uint32_t args[6] = {};
    const uint32_t argument_count =
        (call_number == 11 || call_number == 12) ? 6 :
        (call_number == 8 || call_number == 18) ? 4 :
        (call_number == 4 || call_number == 13) ? 2 :
        (call_number == 14 || call_number == 15) ? 5 :
        (call_number == 9 || call_number == 10) ? 4 :
        (call_number >= 1 && call_number <= 7) ||
        (call_number >= 16 && call_number <= 17) ? 3 : 0;
    if (argument_count)
      memcpy(args, guest(arena, call.word(2), argument_count * 4), argument_count * 4);
    switch (call_number) {
      case 1: result = socket(int32_t(args[0]), int32_t(args[1]), int32_t(args[2])); break;
      case 2: result = bind(int32_t(args[0]), reinterpret_cast<sockaddr *>(guest(arena, args[1], args[2])), args[2]); break;
      case 3: result = connect(int32_t(args[0]), reinterpret_cast<sockaddr *>(guest(arena, args[1], args[2])), args[2]); break;
      case 4: result = listen(int32_t(args[0]), int32_t(args[1])); break;
      case 8: result = socketpair(int32_t(args[0]), int32_t(args[1]), int32_t(args[2]),
                                  reinterpret_cast<int *>(guest(arena, args[3], 8))); break;
      case 5: {
        uint32_t capacity = 0;
        if (args[2]) memcpy(&capacity, guest(arena, args[2], 4), 4);
        sockaddr_storage address{}; socklen_t length = sizeof(address);
        int fd = accept(int32_t(args[0]), args[1] ? reinterpret_cast<sockaddr *>(&address) : nullptr,
                        args[1] ? &length : nullptr);
        if (fd >= 0 && args[1]) {
          uint32_t copied = std::min<uint32_t>(capacity, length);
          if (copied) memcpy(guest(arena, args[1], copied), &address, copied);
          memcpy(guest(arena, args[2], 4), &length, 4);
        }
        result = fd; break;
      }
      case 6: case 7: {
        uint32_t capacity; memcpy(&capacity, guest(arena, args[2], 4), 4);
        sockaddr_storage address{}; socklen_t length = sizeof(address);
        int status = call_number == 6
            ? getsockname(int32_t(args[0]), reinterpret_cast<sockaddr *>(&address), &length)
            : getpeername(int32_t(args[0]), reinterpret_cast<sockaddr *>(&address), &length);
        if (!status) {
          uint32_t copied = std::min<uint32_t>(capacity, length);
          if (copied) memcpy(guest(arena, args[1], copied), &address, copied);
          memcpy(guest(arena, args[2], 4), &length, 4);
        }
        result = status; break;
      }
      case 9: result = send(int32_t(args[0]), guest(arena, args[1], args[2]), args[2], args[3]); break;
      case 10: result = recv(int32_t(args[0]), guest(arena, args[1], args[2]), args[2], args[3]); break;
      case 11: result = sendto(int32_t(args[0]), guest(arena, args[1], args[2]), args[2], args[3],
          args[4] ? reinterpret_cast<sockaddr *>(guest(arena, args[4], args[5])) : nullptr, args[5]); break;
      case 12: {
        uint32_t capacity = 0;
        if (args[5]) memcpy(&capacity, guest(arena, args[5], 4), 4);
        sockaddr_storage address{}; socklen_t length = sizeof(address);
        ssize_t received = recvfrom(int32_t(args[0]), guest(arena, args[1], args[2]), args[2], args[3],
            args[4] ? reinterpret_cast<sockaddr *>(&address) : nullptr, args[4] ? &length : nullptr);
        if (received >= 0 && args[4]) {
          uint32_t copied = std::min<uint32_t>(capacity, length);
          if (copied) memcpy(guest(arena, args[4], copied), &address, copied);
          memcpy(guest(arena, args[5], 4), &length, 4);
        }
        result = received; break;
      }
      case 13: result = shutdown(int32_t(args[0]), int32_t(args[1])); break;
      case 14: result = netextra_setsockopt_guest(arena, int32_t(args[0]), int32_t(args[1]),
          int32_t(args[2]), args[3], int32_t(args[4])); break;
      case 15: result = netextra_getsockopt_guest(arena, int32_t(args[0]), int32_t(args[1]),
          int32_t(args[2]), args[3], args[4]); break;
      case 16: result = netextra_message(arena, args[0], args[1], int32_t(args[2]), false); break;
      case 17: result = netextra_message(arena, args[0], args[1], int32_t(args[2]), true); break;
      case 18: result = netextra_accept4_guest(arena, int32_t(args[0]), args[1], args[2], int32_t(args[3])); break;
      default: errno = ENOSYS; break;
    }
  } else if (number >= 359 && number <= 373) {
    uint32_t a[6]; for (unsigned i = 0; i < 6; ++i) a[i] = call.word(i + 1);
    switch (number) {
      case 359: result = socket(int32_t(a[0]), int32_t(a[1]), int32_t(a[2])); break;
      case 360: result = socketpair(int32_t(a[0]), int32_t(a[1]), int32_t(a[2]), reinterpret_cast<int *>(guest(arena, a[3], 8))); break;
      case 361: result = bind(int32_t(a[0]), reinterpret_cast<sockaddr *>(guest(arena, a[1], a[2])), a[2]); break;
      case 362: result = connect(int32_t(a[0]), reinterpret_cast<sockaddr *>(guest(arena, a[1], a[2])), a[2]); break;
      case 363: result = listen(int32_t(a[0]), int32_t(a[1])); break;
      case 364: result = netextra_accept4_guest(arena, int32_t(a[0]), a[1], a[2], int32_t(a[3])); break;
      case 365: result = netextra_getsockopt_guest(arena, int32_t(a[0]), int32_t(a[1]),
          int32_t(a[2]), a[3], a[4]); break;
      case 366: result = netextra_setsockopt_guest(arena, int32_t(a[0]), int32_t(a[1]),
          int32_t(a[2]), a[3], int32_t(a[4])); break;
      case 367: case 368: {
        uint32_t capacity; memcpy(&capacity, guest(arena, a[2], 4), 4);
        sockaddr_storage address{}; socklen_t length = sizeof(address);
        int status = number == 367 ? getsockname(int32_t(a[0]), reinterpret_cast<sockaddr *>(&address), &length)
                                  : getpeername(int32_t(a[0]), reinterpret_cast<sockaddr *>(&address), &length);
        if (!status) { uint32_t copied = std::min<uint32_t>(capacity, length); if (copied) memcpy(guest(arena, a[1], copied), &address, copied); memcpy(guest(arena, a[2], 4), &length, 4); }
        result = status; break;
      }
      case 369: result = sendto(int32_t(a[0]), a[2] ? guest(arena, a[1], a[2]) : nullptr, a[2], a[3], a[4] ? reinterpret_cast<sockaddr *>(guest(arena, a[4], a[5])) : nullptr, a[5]); break;
      case 371: {
        uint32_t capacity = 0; if (a[5]) memcpy(&capacity, guest(arena, a[5], 4), 4);
        sockaddr_storage address{}; socklen_t length = sizeof(address);
        ssize_t received = recvfrom(int32_t(a[0]), a[2] ? guest(arena, a[1], a[2]) : nullptr, a[2], a[3], a[4] ? reinterpret_cast<sockaddr *>(&address) : nullptr, a[4] ? &length : nullptr);
        if (received >= 0 && a[4]) { uint32_t copied = std::min<uint32_t>(capacity, length); if (copied) memcpy(guest(arena, a[4], copied), &address, copied); memcpy(guest(arena, a[5], 4), &length, 4); }
        result = received; break;
      }
      case 370: result = netextra_message(arena, a[0], a[1], int32_t(a[2]), false); break;
      case 372: result = netextra_message(arena, a[0], a[1], int32_t(a[2]), true); break;
      case 373: result = shutdown(int32_t(a[0]), int32_t(a[1])); break;
      default: errno = ENOSYS; break;
    }
  } else {
    switch (number) {
      case 1: exit(int32_t(call.word(1))); result = 0; break;
      case 2: errno = ENOSYS; break;
      case 3: result = read(int32_t(call.word(1)), call.word(3) ? guest(arena, call.word(2), call.word(3)) : nullptr, call.word(3)); break;
      case 4: result = write(int32_t(call.word(1)), call.word(3) ? guest(arena, call.word(2), call.word(3)) : nullptr, call.word(3)); break;
      case 6: result = close(int32_t(call.word(1))); break;
      case 20: result = getpid(); break;
      case 24: result = getuid(); break;
      case 47: result = getgid(); break;
      case 49: result = geteuid(); break;
      case 50: result = getegid(); break;
      case 122: result = uname(reinterpret_cast<struct utsname *>(guest(arena, call.word(1), sizeof(struct utsname)))); break;
      case 162: {
        int32_t guest_time[2]; memcpy(guest_time, guest(arena, call.word(1), 8), 8);
        timespec requested{guest_time[0], guest_time[1]}, remaining{};
        int status = nanosleep(&requested, call.word(2) ? &remaining : nullptr);
        if (status && call.word(2)) {
          int32_t output[2] = {int32_t(remaining.tv_sec), int32_t(remaining.tv_nsec)};
          memcpy(guest(arena, call.word(2), 8), output, 8);
        }
        result = status; break;
      }
#ifdef __linux__
      case 355: result = getrandom(call.word(2) ? guest(arena, call.word(1), call.word(2)) : nullptr, call.word(2), call.word(3)); break;
#else
      case 355: errno = ENOSYS; break;
#endif
      default: errno = ENOSYS; break;
    }
  }
  call.result(uint32_t(result));
}
namespace {
uint32_t netextra_mangle_pointer(uint32_t value, uint32_t guard) {
  uint32_t encoded = value ^ guard;
  return (encoded << 9) | (encoded >> 23);
}
uint32_t netextra_unmangle_pointer(uint32_t value, uint32_t guard) {
  uint32_t decoded = (value >> 9) | (value << 23);
  return decoded ^ guard;
}
void netextra_save_jump(uint8_t *arena, State *state, uint32_t env, bool save_mask) {
  uint32_t words[6] = {
      state->gpr.rbx.dword, state->gpr.rsi.dword, state->gpr.rdi.dword,
      state->gpr.rbp.dword, 0, 0};
  uint32_t return_pc;
  memcpy(&return_pc, guest(arena, state->gpr.rsp.dword, 4), 4);
  words[4] = state->gpr.rsp.dword + 4;
  words[5] = return_pc;
  uint32_t pointer_guard = 0;
  if (state->addr.gs_base.dword)
    memcpy(&pointer_guard, guest(arena, state->addr.gs_base.dword + 24, 4), 4);
  words[4] = netextra_mangle_pointer(words[4], pointer_guard);
  words[5] = netextra_mangle_pointer(words[5], pointer_guard);
  memcpy(guest(arena, env, sizeof(words)), words, sizeof(words));
  uint32_t was_saved = save_mask;
  memcpy(guest(arena, env + 24, 4), &was_saved, 4);
  if (save_mask) {
    sigset_t mask;
    if (sigprocmask(SIG_SETMASK, nullptr, &mask)) elfconv_runtime_error("Cannot save i386 signal mask.\n");
    netextra_store_sigset(arena, env + 28, mask);
  }
}
[[noreturn]] void netextra_longjmp(uint8_t *arena, State *state, uint32_t env,
                                  uint32_t value, bool checked) {
  uint32_t words[6]; memcpy(words, guest(arena, env, sizeof(words)), sizeof(words));
  uint32_t pointer_guard = 0;
  if (state->addr.gs_base.dword)
    memcpy(&pointer_guard, guest(arena, state->addr.gs_base.dword + 24, 4), 4);
  const uint32_t stack = netextra_unmangle_pointer(words[4], pointer_guard);
  if (checked && stack < state->gpr.rsp.dword)
    elfconv_runtime_error("invalid i386 longjmp target stack.\n");
  const uint32_t pc = netextra_unmangle_pointer(words[5], pointer_guard);
  // glibc sysdeps/unix/sysv/linux/i386/pointer_guard.h: SAVE uses rol32(ptr ^ guard,9);
  // RESTORE uses ror32(encoded,9)^guard, for both saved ESP and EIP.
  if (stack < NULL_GUARD_SIZE || stack > MEMORY_ARENA_SIZE)
    elfconv_runtime_error("invalid i386 longjmp target stack.\n");
  if (pc < NULL_GUARD_SIZE || pc >= MEMORY_ARENA_SIZE)
    elfconv_runtime_error("invalid i386 longjmp target pc.\n");
  uint32_t mask_saved; memcpy(&mask_saved, guest(arena, env + 24, 4), 4);
  netextra_jump_saved_mask = mask_saved != 0;
  if (mask_saved) {
    sigset_t mask; netextra_guest_sigset(arena, env + 28, mask);
    if (sigprocmask(SIG_SETMASK, &mask, nullptr))
      elfconv_runtime_error("Cannot restore i386 signal mask.\n");
  }
  state->gpr.rbx.dword = words[0];
  state->gpr.rsi.dword = words[1];
  state->gpr.rdi.dword = words[2];
  state->gpr.rbp.dword = words[3];
  state->gpr.rsp.dword = stack;
  state->gpr.rip.dword = pc;
  state->gpr.rax.dword = value ? value : 1;
  throw I386NonlocalJump{};
}
}
IMPORT(_setjmp) { CALL; netextra_save_jump(arena, state, call.word(0), false); call.result(0); }
IMPORT(__sigsetjmp) { CALL; netextra_save_jump(arena, state, call.word(0), call.word(1) != 0); call.result(0); }
IMPORT(longjmp) { CALL; netextra_longjmp(arena, state, call.word(0), call.word(1), false); }
IMPORT(__longjmp_chk) { CALL; netextra_longjmp(arena, state, call.word(0), call.word(1), true); }
// Delivery is deferred from the host trampoline until an import/dispatch safe point.
extern "C" void __ecv_i386_dispatch_pending_signals(uint8_t *arena, State *state,
                                                      RuntimeManager *runtime) {
  if (netextra_dispatching_signals || !state || !runtime) return;
  netextra_dispatching_signals = true;
  struct DispatchGuard {
    ~DispatchGuard() { netextra_dispatching_signals = false; }
  } dispatch_guard;
  for (int sig = 1; sig < NSIG; ++sig) {
    if (!netextra_pending_signals[sig]) continue;
    netextra_pending_signals[sig] = 0;
    NetExtraSignal action = netextra_signal_get(runtime, sig);
    if (!action.arena_handler || action.arena_handler == 1) continue;
    if (action.guest_flags & SA_RESETHAND) {
      NetExtraSignal reset{};
      reset.runtime = runtime;
      netextra_signal_set(runtime, sig, reset);
    }
    sigset_t old_mask;
    if (sigprocmask(SIG_SETMASK, nullptr, &old_mask))
      elfconv_runtime_error("Cannot query signal mask at guest safe point.\n");
    sigset_t callback_mask = action.guest_mask;
    if (!(action.guest_flags & SA_NODEFER)) sigaddset(&callback_mask, sig);
    if (sigprocmask(SIG_BLOCK, &callback_mask, nullptr))
      elfconv_runtime_error("Cannot apply guest signal mask at safe point.\n");
    const State interrupted = *state;
    const uint32_t argument = uint32_t(sig);
    const int saved_errno = errno;
    try {
      invoke(arena, state, runtime, action.arena_handler, &argument, 1);
    } catch (const I386NonlocalJump &) {
      if (!netextra_jump_saved_mask) sigprocmask(SIG_SETMASK, &old_mask, nullptr);
      netextra_jump_saved_mask = false;
      throw;
    } catch (...) {
      *state = interrupted;
      sigprocmask(SIG_SETMASK, &old_mask, nullptr);
      errno = saved_errno;
      throw;
    }
    *state = interrupted;
    if (sigprocmask(SIG_SETMASK, &old_mask, nullptr))
      elfconv_runtime_error("Cannot restore signal mask after guest callback.\n");
    netextra_jump_saved_mask = false;
    errno = saved_errno;
  }
}
