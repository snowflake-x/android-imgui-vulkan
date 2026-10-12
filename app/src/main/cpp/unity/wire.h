#pragma once
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
namespace vulkanscope::unity {
struct Socket {
  int fd = -1;
  explicit Socket(int f = -1) : fd(f) {}
  ~Socket() {
    if (fd >= 0)
      close(fd);
  }
  Socket(const Socket &) = delete;
};
/** @brief Bound local messages; peer credentials are checked by both endpoints
 * before parsing. */
inline void transfer(int fd, void *data, size_t n, bool write) {
  auto p = static_cast<char *>(data);
  while (n) {
    auto count = write ? send(fd, p, n, MSG_NOSIGNAL) : recv(fd, p, n, 0);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      throw std::runtime_error("Runtime connection closed or timed out; "
                               "execution outcome may be unknown");
    p += count;
    n -= count;
  }
}
inline std::string receive(int fd) {
  uint32_t n = 0;
  transfer(fd, &n, 4, false);
  if (n > 512u * 1024u)
    throw std::runtime_error("Runtime response exceeds 512 KiB");
  std::string s(n, 0);
  transfer(fd, s.data(), n, false);
  return s;
}
inline void transmit(int fd, const std::string &s) {
  if (s.size() > 512u * 1024u)
    throw std::runtime_error("Runtime response exceeds 512 KiB");
  uint32_t n = s.size();
  transfer(fd, &n, 4, true);
  transfer(fd, const_cast<char *>(s.data()), n, true);
}
inline sockaddr_un endpoint(const std::string &name) {
  sockaddr_un a{};
  a.sun_family = AF_UNIX;
  if (name.empty() || name.size() > sizeof(a.sun_path) - 2)
    throw std::runtime_error("Invalid runtime endpoint");
  memcpy(a.sun_path + 1, name.data(), name.size());
  return a;
}
inline void timeout(int fd) {
  timeval t{12, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof(t));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &t, sizeof(t));
}
}
