/* move_mode_watcher.c - Cached Move UI mode watcher. */

#define _GNU_SOURCE

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "move_mode_watcher.h"

#define MOVE_MODE_SESSION 1
#define MOVE_MODE_NOTE 2
#define MOVE_MODE_SET_OVERVIEW 3

static move_mode_watcher_host_t g_host;
static pthread_t g_thread;
static volatile int g_running = 0;
static uint32_t g_generation = 0;
static int g_last_mode = 0;

static const char *candidate_paths[] = {
    "/data/UserData/schwung/move_mode.txt",
    "/data/UserData/.config/Ableton/Move/sentry_breadcrumbs.log",
    "/data/UserData/.local/share/Ableton/Move/sentry_breadcrumbs.log",
    "/data/UserData/Move/sentry-breadcrumbs.log",
    NULL
};

static shadow_control_t *watcher_control(void)
{
    return g_host.shadow_control_ptr ? *g_host.shadow_control_ptr : NULL;
}

static void lower_copy(char *dst, const char *src, int dst_len)
{
    int i = 0;
    for (; src && src[i] && i < dst_len - 1; i++) {
        dst[i] = (char)tolower((unsigned char)src[i]);
    }
    dst[i] = '\0';
}

static int parse_mode_text(const char *text)
{
    char lower[4096];
    lower_copy(lower, text, sizeof(lower));
    const char *p = strrchr(lower, '\n');
    const char *tail = p ? p + 1 : lower;
    if (strstr(tail, "set mainmode") || strstr(tail, "mainmode")) {
        if (strstr(tail, "note")) return MOVE_MODE_NOTE;
        if (strstr(tail, "session")) return MOVE_MODE_SESSION;
        if (strstr(tail, "set overview") || strstr(tail, "sets")) return MOVE_MODE_SET_OVERVIEW;
    }
    if (strstr(tail, "note mode")) return MOVE_MODE_NOTE;
    if (strstr(tail, "session mode")) return MOVE_MODE_SESSION;
    if (strstr(tail, "set overview")) return MOVE_MODE_SET_OVERVIEW;
    return 0;
}

static int read_mode_from_path(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return 0;
    buf[n] = '\0';
    return parse_mode_text(buf);
}

static void publish_mode(int mode)
{
    if (mode <= 0 || mode == g_last_mode) return;
    shadow_control_t *ctrl = watcher_control();
    if (ctrl) ctrl->move_ui_mode = (uint8_t)mode;
    g_last_mode = mode;
    g_generation++;
}

static void *watcher_main(void *arg)
{
    (void)arg;
    while (g_running) {
        for (int i = 0; candidate_paths[i]; i++) {
            int mode = read_mode_from_path(candidate_paths[i]);
            if (mode > 0) {
                publish_mode(mode);
                break;
            }
        }
        usleep(250000);
    }
    return NULL;
}

void move_mode_watcher_init(const move_mode_watcher_host_t *host)
{
    if (host) g_host = *host;
}

void move_mode_watcher_start(void)
{
    if (g_running) return;
    g_running = 1;
    if (pthread_create(&g_thread, NULL, watcher_main, NULL) != 0) {
        g_running = 0;
        if (g_host.log) g_host.log("move_mode_watcher: failed to start thread");
    }
}

void move_mode_watcher_stop(void)
{
    if (!g_running) return;
    g_running = 0;
    pthread_join(g_thread, NULL);
}

uint32_t move_mode_watcher_generation(void)
{
    return g_generation;
}
