/* Cheap token heuristic for context budgeting. */
#include "context/internal.h"

#include <string.h>

int ctx_estimate_tokens(const char *msg_json) {
  if (!msg_json)
    return 0;
  /* ~4 bytes per token, plus a small overhead for JSON keys. */
  return (int)(strlen(msg_json) / 4) + 10;
}
