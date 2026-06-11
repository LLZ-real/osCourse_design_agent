#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

/*
 * Minimal HTTP/1.1 client primitives — the transport layer.
 *
 * The callers above (llm_client.c) build and parse JSON; this module knows
 * only sockets and HTTP framing. recv_all returns a dynamically-sized buffer
 * so large LLM responses fit.
 */

int tcp_connect(const char *host, int port, char *err, size_t err_cap);

int send_all(int fd, const void *buf, size_t len);

/*
 * Read until the peer closes the connection. *out becomes a heap-allocated
 * NUL-terminated buffer of *out_len bytes; caller frees. timeout_sec applies
 * via SO_RCVTIMEO.
 */
int recv_all(int fd, int timeout_sec, char **out, size_t *out_len, char *err,
             size_t err_cap);

/*
 * Parse an HTTP response in-place. Sets *status and points *body at the
 * start of the message body (inside raw). Returns 0 or -1.
 */
int http_parse_response(const char *raw, int *status, const char **body);

#endif
