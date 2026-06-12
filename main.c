#include "agent/agent.h"
#include "config.h"
#include "context/context.h"
#include "context/session.h"
#include "tools/tools.h"
#include "ui/ui.h"

#include <readline/history.h>
#include <readline/readline.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Session helpers ─────────────────────────────────────────────────── */

static char g_loaded_session[256] = "";  /* tracks which session was loaded */

static void copy_session_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "r");
    if (!in) return;
    FILE *out = fopen(dst, "w");
    if (!out) { fclose(in); return; }
    char buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        fwrite(buf, 1, n, out);
    fclose(in); fclose(out);
}

static void auto_save_on_exit(void) {
    char auto_path[4096];
    snprintf(auto_path, sizeof(auto_path), "%s/.agent/session/auto.log", g_config.workdir);

    /* Always save to last.log for quick recovery. */
    char last_path[4096];
    snprintf(last_path, sizeof(last_path), "%s/.agent/session/last.log", g_config.workdir);
    copy_session_file(auto_path, last_path);

    /* If a named session was loaded, update it too so new messages are saved back. */
    if (g_loaded_session[0]) {
        char named_path[4096];
        snprintf(named_path, sizeof(named_path), "%s/.agent/session/%s.log",
                 g_config.workdir, g_loaded_session);
        copy_session_file(auto_path, named_path);
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

/* ── REPL: /session commands ─────────────────────────────────────────── */

static int cmd_session(Agent *a, const char *arg) {
    Context *ctx = agent_get_context(a);

    char cmd[256], name[256] = "";
    int n = sscanf(arg, "%255s %255s", cmd, name);

    if (n < 1) {
        printf("Usage:\n"
               "  /session list              — list saved sessions\n"
               "  /session save <name>       — save a named snapshot\n"
               "  /session load <name>       — load a saved session\n"
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
                printf("  %s\n", list[i]);
        session_free_list(list, count);
        return 1;
    }

    if (strcmp(cmd, "save") == 0 && name[0]) {
        char src[4096], dst[4096];
        snprintf(src, sizeof(src), "%s/.agent/session/auto.log", g_config.workdir);
        snprintf(dst, sizeof(dst), "%s/.agent/session/%s.log", g_config.workdir, name);
        FILE *in = fopen(src, "r");
        if (!in) { printf("No active session to save.\n"); return 1; }
        FILE *out = fopen(dst, "w");
        if (!out) { fclose(in); printf("Cannot write %s\n", dst); return 1; }
        char buf[65536]; size_t n;
        int lines = 0;
        while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
            fwrite(buf, 1, n, out);
            for (size_t i = 0; i < n; i++) if (buf[i] == '\n') lines++;
        }
        fclose(in); fclose(out);
        printf("Saved session as '%s' (%d messages).\n", name, lines);
        return 1;
    }

    if (strcmp(cmd, "load") == 0 && name[0]) {
        ctx_set_session(ctx, NULL);
        ctx_clear(ctx);
        int replayed = session_replay(g_config.workdir, name, replay_push, ctx);
        printf("Loaded session '%s' (%d messages).\n", name, replayed);
        Session *s = session_open(g_config.workdir, "auto");
        if (s) ctx_set_session(ctx, s);
        strncpy(g_loaded_session, name, sizeof(g_loaded_session) - 1);
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
            auto_save_on_exit();
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
