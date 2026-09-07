#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "socket.h"
#include "utils.h"

int udp_socket_add_multicast_group(UdpSocket* udp_socket, Address* mcast_addr) {
  int ret = 0;
  struct ip_mreq imreq = {0};
  struct in_addr iaddr = {0};

  imreq.imr_interface.s_addr = INADDR_ANY;
  // IPV4 only
  imreq.imr_multiaddr.s_addr = mcast_addr->sin.sin_addr.s_addr;

  if ((ret = setsockopt(udp_socket->fd, IPPROTO_IP, IP_MULTICAST_IF, &iaddr, sizeof(struct in_addr))) < 0) {
    LOGE("Failed to set IP_MULTICAST_IF: %d", ret);
    return ret;
  }

  if ((ret = setsockopt(udp_socket->fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &imreq, sizeof(struct ip_mreq))) < 0) {
    LOGE("Failed to set IP_ADD_MEMBERSHIP: %d", ret);
    return ret;
  }

  return 0;
}

int udp_socket_open(UdpSocket* udp_socket, int family, int port) {
  int ret;
  int reuse = 1;
  struct sockaddr* sa;
  socklen_t sock_len;

  udp_socket->bind_addr.family = family;
  switch (family) {
    case AF_INET6:
      udp_socket->fd = socket(AF_INET6, SOCK_DGRAM, 0);
      udp_socket->bind_addr.sin6.sin6_family = AF_INET6;
      udp_socket->bind_addr.sin6.sin6_port = htons(port);
      udp_socket->bind_addr.sin6.sin6_addr = in6addr_any;
      udp_socket->bind_addr.port = ntohs(udp_socket->bind_addr.sin6.sin6_port);
      sa = (struct sockaddr*)&udp_socket->bind_addr.sin6;
      sock_len = sizeof(struct sockaddr_in6);
      break;
    case AF_INET:
    default:
      udp_socket->fd = socket(AF_INET, SOCK_DGRAM, 0);
      udp_socket->bind_addr.sin.sin_family = AF_INET;
      udp_socket->bind_addr.sin.sin_port = htons(port);
      udp_socket->bind_addr.sin.sin_addr.s_addr = htonl(INADDR_ANY);
      sa = (struct sockaddr*)&udp_socket->bind_addr.sin;
      sock_len = sizeof(struct sockaddr_in);
      break;
  }

  if (udp_socket->fd < 0) {
    LOGE("Failed to create socket");
    return -1;
  }

  do {
    if ((ret = setsockopt(udp_socket->fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse))) < 0) {
      LOGW("reuse failed. ignore");
    }

    // Receive buffer big enough that a burst -- a multi-hundred-KB keyframe,
    // or an ordinary second of stream arriving while the worker happens to be
    // preempted -- is not dropped before the worker can drain it.
    //
    // SO_RCVBUF alone does NOT do this. The kernel silently clamps it to
    // net.core.rmem_max and still returns success, so the 4 MB this used to
    // ask for quietly yielded 208 KB on the RG353's stock firmware
    // (rmem_max = 212992) and logged nothing. Measured on the device:
    //
    //   SO_RCVBUF      asked 4096 KB -> usable  208 KB
    //   SO_RCVBUFFORCE asked 4096 KB -> usable 4096 KB
    //
    // SO_RCVBUFFORCE is the same option without the clamp. It needs
    // CAP_NET_ADMIN, which we have because the client runs as root here. Try
    // it first, fall back to the clamped form, then read back what was
    // actually granted and say so -- a buffer a twentieth of the requested
    // size is exactly what shows up much later as unexplained loss under load.
    //
    // 1 MB, not the 4 MB that was asked for before: at the 8 Mbit/s this
    // stream runs at, 1 MB is about a second of video, which is already far
    // longer than a packet can be late and still be useful, and roughly three
    // times the largest keyframe. 4 MB would be four seconds of queue -- the
    // jitter buffer discards stale packets so it would not break anything, but
    // there is no case where holding that much helps, and buffering junk in a
    // real-time path is how a stall gets reported as latency instead of loss.
    // 208 KB, by contrast, is genuinely marginal: a 200 ms worker stall
    // overflows it.
    {
      int rcvbuf = 1024 * 1024;
      int got = 0;
      socklen_t got_len = sizeof(got);
      const char* how = "SO_RCVBUF";
      int forced = 0;

      // Guarded on the macro, not just on the call failing: lwIP and other
      // embedded stacks libpeer targets do not define it, and they have no
      // rmem_max to be clamped by either, so the plain option is right there.
#ifdef SO_RCVBUFFORCE
      forced = setsockopt(udp_socket->fd, SOL_SOCKET, SO_RCVBUFFORCE, &rcvbuf, sizeof(rcvbuf)) == 0;
      if (forced)
        how = "SO_RCVBUFFORCE";
#endif
      if (!forced && setsockopt(udp_socket->fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0)
        LOGW("SO_RCVBUF failed. ignore");

      // The kernel reports double what it will actually hold in payload: the
      // other half is its own per-skb bookkeeping allowance.
      if (getsockopt(udp_socket->fd, SOL_SOCKET, SO_RCVBUF, &got, &got_len) == 0) {
        int usable = got / 2;
        if (usable < rcvbuf)
          LOGW("recv buffer: asked %d KB via %s, got %d KB (raise net.core.rmem_max)",
               rcvbuf / 1024, how, usable / 1024);
        else
          LOGI("recv buffer: %d KB via %s", usable / 1024, how);
      }
    }

    if ((ret = bind(udp_socket->fd, sa, sock_len)) < 0) {
      LOGE("Failed to bind socket: %d", ret);
      break;
    }

    if (getsockname(udp_socket->fd, sa, &sock_len) < 0) {
      LOGE("Get socket info failed");
      break;
    }
  } while (0);

  if (ret < 0) {
    udp_socket_close(udp_socket);
    return -1;
  }

  switch (udp_socket->bind_addr.family) {
    case AF_INET6:
      udp_socket->bind_addr.port = ntohs(udp_socket->bind_addr.sin6.sin6_port);
      break;
    case AF_INET:
    default:
      udp_socket->bind_addr.port = ntohs(udp_socket->bind_addr.sin.sin_port);
      break;
  }

  return 0;
}

void udp_socket_close(UdpSocket* udp_socket) {
  if (udp_socket->fd > 0) {
    close(udp_socket->fd);
  }
}

int udp_socket_sendto(UdpSocket* udp_socket, Address* addr, const uint8_t* buf, int len) {
  struct sockaddr* sa;
  socklen_t sock_len;
  int ret = -1;

  if (udp_socket->fd < 0) {
    LOGE("sendto before socket init");
    return -1;
  }

  switch (addr->family) {
    case AF_INET6:
      addr->sin6.sin6_family = AF_INET6;
      sa = (struct sockaddr*)&addr->sin6;
      sock_len = sizeof(struct sockaddr_in6);
      break;
    case AF_INET:
    default:
      addr->sin.sin_family = AF_INET;
      sa = (struct sockaddr*)&addr->sin;
      sock_len = sizeof(struct sockaddr_in);
      break;
  }

  if ((ret = sendto(udp_socket->fd, buf, len, 0, sa, sock_len)) < 0) {
    LOGE("Failed to sendto: %s", strerror(errno));
    return -1;
  }

  return ret;
}

int udp_socket_recvfrom(UdpSocket* udp_socket, Address* addr, uint8_t* buf, int len) {
  struct sockaddr_in6 sin6;
  struct sockaddr_in sin;
  struct sockaddr* sa;
  socklen_t sock_len;
  int ret;

  if (udp_socket->fd < 0) {
    LOGE("recvfrom before socket init");
    return -1;
  }

  switch (udp_socket->bind_addr.family) {
    case AF_INET6:
      sin6.sin6_family = AF_INET6;
      sa = (struct sockaddr*)&sin6;
      sock_len = sizeof(struct sockaddr_in6);
      break;
    case AF_INET:
    default:
      sin.sin_family = AF_INET;
      sa = (struct sockaddr*)&sin;
      sock_len = sizeof(struct sockaddr_in);
      break;
  }

  if ((ret = recvfrom(udp_socket->fd, buf, len, 0, sa, &sock_len)) < 0) {
    LOGE("Failed to recvfrom: %s", strerror(errno));
    return -1;
  }

  if (addr) {
    switch (udp_socket->bind_addr.family) {
      case AF_INET6:
        addr->family = AF_INET6;
        addr->port = htons(sin6.sin6_port);
        memcpy(&addr->sin6, &sin6, sizeof(struct sockaddr_in6));
        break;
      case AF_INET:
      default:
        addr->family = AF_INET;
        addr->port = htons(sin.sin_port);
        memcpy(&addr->sin, &sin, sizeof(struct sockaddr_in));
        break;
    }
  }

  return ret;
}

int tcp_socket_open(TcpSocket* tcp_socket, int family) {
  tcp_socket->bind_addr.family = family;
  switch (family) {
    case AF_INET6:
      tcp_socket->fd = socket(AF_INET6, SOCK_STREAM, 0);
      break;
    case AF_INET:
    default:
      tcp_socket->fd = socket(AF_INET, SOCK_STREAM, 0);
      break;
  }

  if (tcp_socket->fd < 0) {
    LOGE("Failed to create socket");
    return -1;
  }
  return 0;
}

int tcp_socket_connect(TcpSocket* tcp_socket, Address* addr) {
  char addr_string[ADDRSTRLEN];
  int ret;
  struct sockaddr* sa;
  socklen_t sock_len;

  if (tcp_socket->fd < 0) {
    LOGE("Connect before socket init");
    return -1;
  }

  switch (addr->family) {
    case AF_INET6:
      addr->sin6.sin6_family = AF_INET6;
      sa = (struct sockaddr*)&addr->sin6;
      sock_len = sizeof(struct sockaddr_in6);
      break;
    case AF_INET:
    default:
      addr->sin.sin_family = AF_INET;
      sa = (struct sockaddr*)&addr->sin;
      sock_len = sizeof(struct sockaddr_in);
      break;
  }

  addr_to_string(addr, addr_string, sizeof(addr_string));
  LOGI("Connecting to server: %s:%d", addr_string, addr->port);
  if ((ret = connect(tcp_socket->fd, sa, sock_len)) < 0) {
    LOGE("Failed to connect to server");
    return -1;
  }

  LOGI("Server is connected");
  return 0;
}

void tcp_socket_close(TcpSocket* tcp_socket) {
  if (tcp_socket->fd > 0) {
    close(tcp_socket->fd);
  }
}

int tcp_socket_send(TcpSocket* tcp_socket, const uint8_t* buf, int len) {
  int ret;

  if (tcp_socket->fd < 0) {
    LOGE("sendto before socket init");
    return -1;
  }

  ret = send(tcp_socket->fd, buf, len, 0);
  if (ret < 0) {
    LOGE("Failed to send: %s", strerror(errno));
    return -1;
  }
  return ret;
}

int tcp_socket_recv(TcpSocket* tcp_socket, uint8_t* buf, int len) {
  int ret;

  if (tcp_socket->fd < 0) {
    LOGE("recvfrom before socket init");
    return -1;
  }

  ret = recv(tcp_socket->fd, buf, len, 0);
  if (ret < 0) {
    LOGE("Failed to recv: %s", strerror(errno));
    return -1;
  }
  return ret;
}
