#include "http.h"

#include "util.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

int tcp_connect(const char *host, int port, char *err, size_t err_cap) {
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

  if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
    snprintf(err, err_cap, "connect: %s", strerror(errno));
    close(fd);
    freeaddrinfo(res);
    return -1;
  }

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
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
      snprintf(err, err_cap, "setsockopt: %s", strerror(errno));
      return -1;
    }
  }

  size_t cap = 4096;
  size_t len = 0;
  char *buf = xmalloc(cap);

  for (;;) {
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
