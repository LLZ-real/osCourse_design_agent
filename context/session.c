/*
 * Session — append‑only log of agent conversations on stable storage.
 *
 * Each session is a JSONL file at <workdir>/.agent/session/<name>.log.
 * On a fresh start, replaying the log rebuilds the conversation history
 * so the agent can pick up where it left off.
 */
#include "context/session.h"
#include "util.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ── Internal helpers ─────────────────────────────────────────────────── */

static void ensure_dir(const char *path) {
    char *p = strdup(path);
    for (char *s = p + 1; *s; s++) {
        if (*s == '/') {
            *s = '\0';
            mkdir(p, 0755);
            *s = '/';
        }
    }
    mkdir(p, 0755);
    free(p);
}

static int build_path(char *buf, size_t cap, const char *workdir,
                      const char *name) {
    return snprintf(buf, cap, "%s/.agent/session/%s.log", workdir, name);
}

/* ── Session handle ──────────────────────────────────────────────────── */

struct Session {
    char *name;
    FILE *fp;
};

Session *session_open(const char *workdir, const char *name) {
    if (!workdir || !name || !name[0])
        return NULL;

    char dir[4096];
    snprintf(dir, sizeof(dir), "%s/.agent/session", workdir);
    ensure_dir(dir);

    char path[4096];
    build_path(path, sizeof(path), workdir, name);

    FILE *fp = fopen(path, "a");
    if (!fp)
        return NULL;

    Session *s = malloc(sizeof(*s));
    if (!s) {
        fclose(fp);
        return NULL;
    }
    s->name = strdup(name);
    s->fp = fp;
    return s;
}

void session_append(Session *s, const char *msg_json) {
    if (!s || !s->fp || !msg_json)
        return;
    fprintf(s->fp, "%s\n", msg_json);
    fflush(s->fp);
}

void session_close(Session *s) {
    if (!s)
        return;
    if (s->fp)
        fclose(s->fp);
    free(s->name);
    free(s);
}

/* ── Replay ──────────────────────────────────────────────────────────── */

int session_replay(const char *workdir, const char *name,
                   void (*cb)(void *, const char *), void *user_data) {
    if (!workdir || !name || !cb)
        return 0;

    char path[4096];
    build_path(path, sizeof(path), workdir, name);

    FILE *fp = fopen(path, "r");
    if (!fp)
        return 0;

    int count = 0;
    char line[65536];
    while (fgets(line, sizeof(line), fp)) {
        /* Remove trailing newline. */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n')
            line[len - 1] = '\0';

        /* Skip blank lines and malformed JSON (crash recovery safety). */
        if (line[0] != '{')
            continue;

        cb(user_data, line);
        count++;
    }

    fclose(fp);
    return count;
}

/* ── Listing ─────────────────────────────────────────────────────────── */

char **session_list(const char *workdir, int *out_count) {
    *out_count = 0;

    char dir[4096];
    snprintf(dir, sizeof(dir), "%s/.agent/session", workdir);

    DIR *d = opendir(dir);
    if (!d)
        return NULL;

    int cap = 16;
    int count = 0;
    char **names = xmalloc((size_t)cap * sizeof(char *));

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        /* Only match *.log files. */
        const char *dot = strrchr(entry->d_name, '.');
        if (!dot || strcmp(dot, ".log") != 0)
            continue;

        size_t nlen = (size_t)(dot - entry->d_name);
        if (count >= cap) {
            cap *= 2;
            names = xrealloc(names, (size_t)cap * sizeof(char *));
        }
        names[count] = strndup(entry->d_name, nlen);
        count++;
    }
    closedir(d);

    *out_count = count;
    return names;
}

void session_free_list(char **list, int count) {
    if (!list)
        return;
    for (int i = 0; i < count; i++)
        free(list[i]);
    free(list);
}

/* ── Delete ──────────────────────────────────────────────────────────── */

void session_delete(const char *workdir, const char *name) {
    if (!workdir || !name)
        return;

    char path[4096];
    build_path(path, sizeof(path), workdir, name);
    remove(path);
}
