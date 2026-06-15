#include "agent/agent.h"
#include "config.h"
#include "context/context.h"
#include "context/session.h"
#include "tools/tools.h"
#include "ui/ui.h"
#include "util.h"

#include <readline/history.h>
#include <readline/readline.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Session helpers ─────────────────────────────────────────────────── */

static char g_loaded_session[256] = "";

static void write_history_to(const MessageList *h, const char *path) {
    if (h->len == 0) return;
    FILE *out = fopen(path, "w");
    if (!out) return;
    for (int i = 0; i < h->len; i++)
        fprintf(out, "%s\n", h->items[i]);
    fclose(out);
}

static void auto_save_on_exit(Agent *a) {
    const MessageList *h = ctx_history(agent_get_context(a));

    /* Save to last.log for quick recovery. */
    char last_path[4096];
    snprintf(last_path, sizeof(last_path), "%s/.agent/session/last.log", g_config.workdir);
    write_history_to(h, last_path);

    /* Also save to a timestamped name so earlier sessions aren't lost. */
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", t);
    char stamped_path[4096];
    snprintf(stamped_path, sizeof(stamped_path), "%s/.agent/session/auto-%s.log",
             g_config.workdir, ts);
    write_history_to(h, stamped_path);

    /* Save back to the loaded named session if any. */
    if (g_loaded_session[0]) {
        char named_path[4096];
        snprintf(named_path, sizeof(named_path), "%s/.agent/session/%s.log",
                 g_config.workdir, g_loaded_session);
        write_history_to(h, named_path);
    }
}

static void replay_push(void *user_data, const char *msg) {
    ctx_push((Context *)user_data, strdup(msg));
}

static void open_fresh_session(Context *ctx) {
    ctx_set_session(ctx, NULL);
    session_delete(g_config.workdir, "auto");
    ctx_clear(ctx);
    Session *s = session_open(g_config.workdir, "auto");
    if (s) ctx_set_session(ctx, s);
}

/* ── Display helpers ─────────────────────────────────────────────────── */

static void print_session_summary(Context *ctx, int total) {
    /* Show up to 10 most recent messages, fully, matching prompt format. */
    const MessageList *h = ctx_history(ctx);
    int start = total > 10 ? total - 10 : 0;

    for (int i = start; i < total; i++) {
        cJSON *msg = cJSON_Parse(h->items[i]);
        if (!msg) continue;
        const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "role"));
        const char *content = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "content"));
        if (!role || !content) { cJSON_Delete(msg); continue; }

        if (strcmp(role, "user") == 0) {
            /* "> content" — same format as the prompt line */
            printf("\033[36m>\033[0m %s\n", content);
        } else if (strcmp(role, "assistant") == 0) {
            /* "  content" — aligned after "> " */
            printf("  %s\n", content);
        }
        /* skip tool/internal messages */
        cJSON_Delete(msg);
    }
    if (start > 0)
        printf("\033[90m... (%d earlier messages)\033[0m\n", start);
}

static int pick_session_name(char *out, size_t out_cap) {
    int count;
    char **list = session_list(g_config.workdir, &count);
    if (!list || count == 0) {
        printf("No saved sessions.\n");
        session_free_list(list, count);
        return -1;
    }

    printf("Sessions:\n");
    for (int i = 0; i < count; i++)
        printf("  \033[36m[%d]\033[0m  %s\n", i, list[i]);

    printf("Enter number (or blank to cancel): ");
    fflush(stdout);

    char line[64];
    if (!fgets(line, sizeof(line), stdin)) {
        session_free_list(list, count);
        return -1;
    }

    int idx = -1;
    if (sscanf(line, "%d", &idx) == 1 && idx >= 0 && idx < count) {
        snprintf(out, out_cap, "%s", list[idx]);
        session_free_list(list, count);
        return 0;
    }

    session_free_list(list, count);
    return -1;
}

/* ── REPL: /session commands ─────────────────────────────────────────── */

static int cmd_session(Agent *a, const char *arg) {
    Context *ctx = agent_get_context(a);

    char cmd[256], name[256] = "";
    int n = sscanf(arg, "%255s %255s", cmd, name);

    if (n < 1) {
        printf("Usage:\n"
               "  /session list              — list saved sessions\n"
               "  /session save <name>       — save a named snapshot\n"
               "  /session load [name]       — load / pick a saved session\n"
               "  /session delete <name>     — delete a saved session\n"
               "  /session new               — discard current session\n");
        return 1;
    }

    if (strcmp(cmd, "list") == 0) {
        int count;
        char **list = session_list(g_config.workdir, &count);
        if (!list || count == 0)
            printf("No saved sessions.\n");
        else
            for (int i = 0; i < count; i++)
                printf("  \033[36m[%d]\033[0m  %s\n", i, list[i]);
        session_free_list(list, count);
        return 1;
    }

    if (strcmp(cmd, "save") == 0 && name[0]) {
        const MessageList *h = ctx_history(ctx);
        if (h->len == 0) { printf("Nothing to save — session is empty.\n"); return 1; }
        char dst[4096];
        snprintf(dst, sizeof(dst), "%s/.agent/session/%s.log", g_config.workdir, name);
        FILE *out = fopen(dst, "w");
        if (!out) { printf("Cannot write %s\n", dst); return 1; }
        for (int i = 0; i < h->len; i++)
            fprintf(out, "%s\n", h->items[i]);
        fclose(out);
        snprintf(g_loaded_session, sizeof(g_loaded_session), "%s", name);
        printf("Saved session as '%s' (%d messages).\n", name, h->len);
        return 1;
    }

    if (strcmp(cmd, "load") == 0) {
        char load_name[256];
        if (name[0]) {
            snprintf(load_name, sizeof(load_name), "%s", name);
        } else {
            if (pick_session_name(load_name, sizeof(load_name)) != 0) {
                printf("Cancelled.\n");
                return 1;
            }
        }

        /* Clear screen for a clean slate. */
        printf("\033[2J\033[H");
        fflush(stdout);

        ctx_set_session(ctx, NULL);
        ctx_clear(ctx);
        /* Replay all messages (0 = no limit) so the agent has full history. */
        int replayed = session_replay(g_config.workdir, load_name, replay_push, ctx,
                                       0);
        printf("Loaded session '\033[36m%s\033[0m' (%d messages).\n", load_name, replayed);
        if (replayed > 0)
            print_session_summary(ctx, replayed);
        Session *s = session_open(g_config.workdir, "auto");
        if (s) ctx_set_session(ctx, s);
        snprintf(g_loaded_session, sizeof(g_loaded_session), "%s", load_name);
        return 1;
    }

    if (strcmp(cmd, "delete") == 0 && name[0]) {
        session_delete(g_config.workdir, name);
        printf("Deleted session '%s'.\n", name);
        return 1;
    }

    if (strcmp(cmd, "new") == 0) {
        open_fresh_session(ctx);
        g_loaded_session[0] = '\0';
        printf("Started a new session.\n");
        return 1;
    }

    printf("Unknown session command: %s\n", cmd);
    return 1;
}

/* ── Main ────────────────────────────────────────────────────────────── */

int main(void) {
    config_init();
    tools_init();
    ui_init();
    ui_start();

    Agent *a = agent_create();
    if (!a) {
        fprintf(stderr, "agent_create failed\n");
        ui_stop();
        return 1;
    }

    /* Start a fresh auto-session for logging. */
    Session *s = session_open(g_config.workdir, "auto");
    if (s)
        ctx_set_session(agent_get_context(a), s);

    ui_banner();

    int rc = 0;

    for (;;) {
        char *input = readline("> ");
        if (!input)
            break;

        if (strcmp(input, "exit") == 0 || strcmp(input, "quit") == 0 ||
            strcmp(input, "q") == 0) {
            auto_save_on_exit(a);
            free(input);
            break;
        }

        if (strncmp(input, "/session", 8) == 0) {
            const char *arg = input + 8;
            while (*arg == ' ') arg++;
            cmd_session(a, arg);
            free(input);
            continue;
        }

        if (input[0] != '\0')
            add_history(input);

        const char *reply = agent_chat(a, input);
        free(input);

        if (reply) {
            ui_idle();
            const char *m = agent_last_metrics(a);
            if (m) printf("%s\n", m);
            printf("%s\n", reply);
        } else {
            rc = 1;
        }
    }

    agent_free(a);
    ui_stop();
    return rc;
}
