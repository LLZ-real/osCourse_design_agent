#include "http.h"

#include "util.h"

#include <unistd.h>

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>

int tcp_connect(const char *host, int port, int timeout_sec,
                char *err, size_t err_cap) {
  char port_buf[16];
  snprintf(port_buf, sizeof(port_buf), "%d", port);

  struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
  struct addrinfo *res = NULL;
  if (getaddrinfo(host, port_buf, &hints, &res) != 0 || !res) {
    snprintf(err, err_cap, "cannot resolve host %s", host);
    return -1;
  }

  int fd = socket(res->ai_family, res->ai_socktype, 0);
  if (fd < 0) {
    snprintf(err, err_cap, "socket: %s", strerror(errno));
    freeaddrinfo(res);
    return -1;
  }

  /* Set send/receive timeouts so the whole HTTP request won't hang
     indefinitely.  These are inherited by connect() when we use
     a non-blocking handshake below. */
  struct timeval tv = { .tv_sec = timeout_sec };
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  /* Connect with timeout: put fd in non-blocking mode, then poll. */
  int fl = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, fl | O_NONBLOCK);

  if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
    if (errno != EINPROGRESS) {
      snprintf(err, err_cap, "connect: %s", strerror(errno));
      close(fd);
      freeaddrinfo(res);
      return -1;
    }
    /* Wait for the connection to complete (or timeout). */
    struct pollfd pfd = { .fd = fd, .events = POLLOUT };
    int ret = poll(&pfd, 1, timeout_sec * 1000);
    if (ret <= 0) {
      snprintf(err, err_cap, "connect: %s",
               ret == 0 ? "timed out" : strerror(errno));
      close(fd);
      freeaddrinfo(res);
      return -1;
    }
    /* Check if connect succeeded (no error on the socket). */
    int so_err = 0;
    socklen_t slen = sizeof(so_err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &slen) < 0 || so_err) {
      snprintf(err, err_cap, "connect: %s", so_err ? strerror(so_err) : strerror(errno));
      close(fd);
      freeaddrinfo(res);
      return -1;
    }
  }

  /* Restore blocking mode. */
  fcntl(fd, F_SETFL, fl);

  freeaddrinfo(res);
  return fd;
}

int send_all(int fd, const void *buf, size_t len) {
  const char *p = buf;
  size_t sent = 0;
  while (sent < len) {
    ssize_t n = send(fd, p + sent, len - sent, 0);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    if (n == 0)
      return -1;
    sent += (size_t)n;
  }
  return 0;
}

int recv_all(int fd, int timeout_sec, char **out, size_t *out_len, char *err,
             size_t err_cap) {
  if (timeout_sec > 0) {
    struct timeval tv = {.tv_sec = timeout_sec};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  }

  /* Track total elapsed time so a slow-trickling peer can't keep us alive
     beyond timeout_sec even though each individual recv() finishes within
     the per-call SO_RCVTIMEO. */
  struct timespec deadline = {0, 0};
  if (timeout_sec > 0) {
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += timeout_sec;
  }

  size_t cap = 4096;
  size_t len = 0;
  char *buf = xmalloc(cap);

  for (;;) {
    /* Check total deadline BEFORE each recv, so a slow peer can't
       keep us alive by trickling data just inside SO_RCVTIMEO. */
    if (timeout_sec > 0) {
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      if (now.tv_sec >= deadline.tv_sec) {
        snprintf(err, err_cap, "recv timed out (%ds total)", timeout_sec);
        free(buf);
        return -1;
      }
      /* Shrink SO_RCVTIMEO to the remaining budget so a single
         stuck recv() doesn't wait longer than the total deadline. */
      int left = (int)(deadline.tv_sec - now.tv_sec);
      if (left > 0) {
        struct timeval tv = { .tv_sec = left };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
      }
    }

    if (len + 1 >= cap) {
      cap *= 2;
      buf = xrealloc(buf, cap);
    }
    ssize_t n = recv(fd, buf + len, cap - len - 1, 0);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        snprintf(err, err_cap, "recv timed out (%ds)", timeout_sec);
      else
        snprintf(err, err_cap, "recv: %s", strerror(errno));
      free(buf);
      return -1;
    }
    if (n == 0)
      break;
    len += (size_t)n;
  }

  buf[len] = '\0';
  *out = buf;
  *out_len = len;
  return 0;
}

int http_parse_response(const char *raw, int *status, const char **body) {
  const char *space = strchr(raw, ' ');
  if (!space)
    return -1;
  *status = (int)strtol(space + 1, NULL, 10);

  const char *sep = strstr(raw, "\r\n\r\n");
  if (!sep)
    return -1;
  *body = sep + 4;
  return 0;
}
