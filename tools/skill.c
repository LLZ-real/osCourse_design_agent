/*
 * Skill system — progressive-disclosure prompts.
 *
 * At startup, only skill names and one-line descriptions are injected into
 * the system prompt (low token cost). When the LLM decides a skill is
 * relevant, it calls skill_load("<name>") to get the full instruction.
 *
 * Skills are stored at <workdir>/.agent/skills/<name>.json.
 */
#include "config.h"
#include "tools/tools.h"
#include "util.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ── Built-in skill definitions ──────────────────────────────────────── */

typedef struct {
    const char *name;
    const char *desc;
    const char *prompt;
} BuiltinSkill;

static const BuiltinSkill BUILTIN_SKILLS[] = {
    {
        .name = "code-review",
        .desc = "Review code for correctness, bugs, and style issues",
        .prompt =
            "You are reviewing code. Follow these steps:\n"
            "1. Read the file(s) using read_file\n"
            "2. Check for: logic bugs, memory leaks, undefined behavior, "
            "security issues, style violations\n"
            "3. For each issue found, cite the exact line and explain why\n"
            "4. Suggest a fix for each issue\n"
            "5. Summarize: overall quality (1-10), critical issues, "
            "recommendations\n"
            "Format each finding as:\n"
            "  [severity] file:line — description\n"
            "Severity: CRITICAL, MAJOR, MINOR, STYLE",
    },
    {
        .name = "explain",
        .desc = "Explain what a piece of code does in detail",
        .prompt =
            "You are explaining code. Follow these steps:\n"
            "1. Read the file(s) using read_file\n"
            "2. Give a one-sentence summary of what the code does\n"
            "3. Walk through the logic section by section\n"
            "4. Explain any non-obvious patterns, algorithms, or idioms\n"
            "5. Note any assumptions or prerequisites\n"
            "Focus on the WHY, not just the WHAT.",
    },
    {
        .name = "refactor",
        .desc = "Suggest refactoring improvements for code",
        .prompt =
            "You are suggesting refactoring. Follow these steps:\n"
            "1. Read the file(s) using read_file\n"
            "2. Identify: duplicate code, long functions, complex conditions, "
            "tight coupling, naming issues\n"
            "3. For each issue, show the current code and your proposed change\n"
            "4. Explain the benefit (readability, performance, maintainability)\n"
            "5. Prioritize by impact: HIGH, MEDIUM, LOW\n"
            "Only suggest changes that preserve existing behavior.",
    },
};

#define NUM_SKILLS (int)(sizeof(BUILTIN_SKILLS) / sizeof(BUILTIN_SKILLS[0]))

/* ── Ensure skills directory ─────────────────────────────────────────── */

static void ensure_skills_dir(void) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/.agent", g_config.workdir);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/.agent/skills", g_config.workdir);
    mkdir(path, 0755);
}

/* ── Write built-in skills to disk on first access ───────────────────── */

static void seed_skills(void) {
    ensure_skills_dir();
    for (int i = 0; i < NUM_SKILLS; i++) {
        char path[4096];
        snprintf(path, sizeof(path), "%s/.agent/skills/%s.json", g_config.workdir,
                 BUILTIN_SKILLS[i].name);

        FILE *fp = fopen(path, "r");
        if (fp) { fclose(fp); continue; }  /* already exists */

        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "name", BUILTIN_SKILLS[i].name);
        cJSON_AddStringToObject(obj, "desc", BUILTIN_SKILLS[i].desc);
        cJSON_AddStringToObject(obj, "prompt", BUILTIN_SKILLS[i].prompt);
        char *json = cJSON_PrintUnformatted(obj);
        cJSON_Delete(obj);

        fp = fopen(path, "w");
        if (fp) {
            fprintf(fp, "%s\n", json);
            fclose(fp);
        }
        free(json);
    }
}

/* ── Build system-prompt fragment with skill names and descriptions ──── */

char *skill_build_listing(void) {
    seed_skills();

    char dir[4096];
    snprintf(dir, sizeof(dir), "%s/.agent/skills", g_config.workdir);

    size_t total = 0;
    int cap = 16;
    int count = 0;
    char **lines = xmalloc((size_t)cap * sizeof(char *));

    DIR *d = opendir(dir);
    if (d) {
        struct dirent *entry;
        while ((entry = readdir(d)) != NULL) {
            const char *dot = strrchr(entry->d_name, '.');
            if (!dot || strcmp(dot, ".json") != 0) continue;

            char path[4096];
            snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
            FILE *fp = fopen(path, "r");
            if (!fp) continue;

            char line[8192];
            if (fgets(line, sizeof(line), fp)) {
                cJSON *obj = cJSON_Parse(line);
                if (obj) {
                    cJSON *n = cJSON_GetObjectItem(obj, "name");
                    cJSON *d = cJSON_GetObjectItem(obj, "desc");
                    const char *name = n && cJSON_IsString(n) ? n->valuestring : "";
                    const char *desc = d && cJSON_IsString(d) ? d->valuestring : "";
                    char buf[1024];
                    int len = snprintf(buf, sizeof(buf), "  %s — %s", name, desc);

                    if (count >= cap) {
                        cap *= 2;
                        lines = xrealloc(lines, (size_t)cap * sizeof(char *));
                    }
                    lines[count] = xstrdup(buf);
                    total += (size_t)len + 1;
                    count++;
                    cJSON_Delete(obj);
                }
            }
            fclose(fp);
        }
        closedir(d);
    }

    /* Build the listing string. */
    char *out = xmalloc(total + 128);
    int pos = snprintf(out, total + 128, "\nAvailable skills (use skill_load to activate):\n");
    for (int i = 0; i < count; i++) {
        pos += snprintf(out + pos, total + 128 - (size_t)pos, "%s\n", lines[i]);
        free(lines[i]);
    }
    free(lines);

    if (count == 0) {
        free(out);
        return xstrdup("");
    }

    return out;
}

/* ── skill_load tool ─────────────────────────────────────────────────── */

static ToolResult tool_skill_load(cJSON *args);

ToolDef skill_load_def = {
    .name = "skill_load",
    .desc = "Load the full prompt for a skill by name. Call this when you recognize a relevant skill.",
    .param_schema = "{\"type\":\"object\","
                    "\"properties\":{"
                    "\"name\":{\"type\":\"string\","
                    "\"description\":\"Skill name to load\"}"
                    "},\"required\":[\"name\"]}",
    .exec = tool_skill_load,
    .read_only = true,
};

static ToolResult tool_skill_load(cJSON *args) {
    cJSON *name_j = cJSON_GetObjectItem(args, "name");
    if (!name_j || !cJSON_IsString(name_j))
        return (ToolResult){.ok = false, .output = xstrdup("missing 'name' argument")};

    seed_skills();

    char path[4096];
    snprintf(path, sizeof(path), "%s/.agent/skills/%s.json", g_config.workdir,
             name_j->valuestring);

    FILE *fp = fopen(path, "r");
    if (!fp)
        return (ToolResult){.ok = false, .output = xasprintf("skill '%s' not found", name_j->valuestring)};

    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    rewind(fp);
    char *buf = xmalloc((size_t)sz + 1);
    fread(buf, 1, (size_t)sz, fp);
    buf[sz] = '\0';
    fclose(fp);

    cJSON *obj = cJSON_Parse(buf);
    free(buf);
    if (!obj)
        return (ToolResult){.ok = false, .output = xstrdup("failed to parse skill file")};

    cJSON *prompt_j = cJSON_GetObjectItem(obj, "prompt");
    const char *prompt = prompt_j && cJSON_IsString(prompt_j) ? prompt_j->valuestring : "";

    char *result = xasprintf("[Skill: %s]\n%s", name_j->valuestring, prompt);
    cJSON_Delete(obj);

    return (ToolResult){.ok = true, .output = result};
}
