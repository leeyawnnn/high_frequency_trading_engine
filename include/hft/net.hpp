// Thin RAII wrapper over a UDP socket.
//
// This is the simulation's stand-in for a kernel-bypass NIC. On a real HFT box
// the receive path would be DPDK / AF_XDP / Solarflare ef_vi — userspace,
// poll-mode, no per-packet syscall. Here it is ordinary BSD UDP over loopback:
// recv() is a syscall and IS the dominant cost on the receive path. We measure
// around it honestly and call this out in the README.
//
// Setup calls (socket/bind/setsockopt) throw on failure — they run once at
// startup. The hot-path calls (send_to / try_recv) never throw and never
// allocate; try_recv returns -1 on EWOULDBLOCK so the feed loop can spin.
#pragma once

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

namespace hft {

// A UDP destination/bind address (IPv4).
struct Endpoint {
  ::in_addr_t addr;     // network byte order
  std::uint16_t port;   // host byte order

  static Endpoint v4(const char* ip, std::uint16_t port) {
    Endpoint e{};
    if (::inet_pton(AF_INET, ip, &e.addr) != 1)
      throw std::runtime_error(std::string("bad IPv4 address: ") + ip);
    e.port = port;
    return e;
  }

  ::sockaddr_in sockaddr() const {
    ::sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = addr;
    sa.sin_port = htons(port);
    return sa;
  }
};

class UdpSocket {
 public:
  UdpSocket() {
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) throw_errno("socket");
  }
  ~UdpSocket() {
    if (fd_ >= 0) ::close(fd_);
  }

  UdpSocket(UdpSocket&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
  UdpSocket& operator=(UdpSocket&& o) noexcept {
    if (this != &o) {
      if (fd_ >= 0) ::close(fd_);
      fd_ = o.fd_;
      o.fd_ = -1;
    }
    return *this;
  }
  UdpSocket(const UdpSocket&) = delete;
  UdpSocket& operator=(const UdpSocket&) = delete;

  // Create a socket bound to a local port for receiving.
  static UdpSocket bound(std::uint16_t port, const char* bind_addr = "0.0.0.0") {
    UdpSocket s;
    int one = 1;
    ::setsockopt(s.fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    ::sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (::inet_pton(AF_INET, bind_addr, &sa.sin_addr) != 1)
      throw std::runtime_error(std::string("bad bind address: ") + bind_addr);
    if (::bind(s.fd_, reinterpret_cast<::sockaddr*>(&sa), sizeof(sa)) < 0)
      s.throw_errno("bind");
    return s;
  }

  void set_nonblocking(bool on) {
    int flags = ::fcntl(fd_, F_GETFL, 0);
    if (flags < 0) throw_errno("fcntl(F_GETFL)");
    flags = on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    if (::fcntl(fd_, F_SETFL, flags) < 0) throw_errno("fcntl(F_SETFL)");
  }

  void set_recv_buffer(int bytes) {
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes));
  }
  void set_send_buffer(int bytes) {
    ::setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes));
  }

  // Hot-path send. Returns bytes sent, or -1 on error (errno set).
  long send_to(const Endpoint& dst, const void* data, std::size_t len) noexcept {
    ::sockaddr_in sa = dst.sockaddr();
    return ::sendto(fd_, data, len, 0, reinterpret_cast<::sockaddr*>(&sa),
                    sizeof(sa));
  }

  // Hot-path receive. Returns bytes received, 0 for an empty datagram, or
  // -1 when there is nothing to read (EWOULDBLOCK) or on error.
  long try_recv(void* buf, std::size_t len) noexcept {
    return ::recv(fd_, buf, len, 0);
  }

  int fd() const noexcept { return fd_; }

  // The local port this socket is bound to (resolves an ephemeral port chosen
  // when bound to 0). Returns 0 if unbound.
  std::uint16_t local_port() const {
    ::sockaddr_in sa{};
    ::socklen_t len = sizeof(sa);
    if (::getsockname(fd_, reinterpret_cast<::sockaddr*>(&sa), &len) != 0) return 0;
    return ntohs(sa.sin_port);
  }

 private:
  [[noreturn]] void throw_errno(const char* what) {
    throw std::runtime_error(std::string(what) + ": " + std::strerror(errno));
  }
  int fd_ = -1;
};

}  // namespace hft
