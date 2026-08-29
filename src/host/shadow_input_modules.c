/* shadow_input_modules.c - Runtime for pre-native input modules. */

#define _GNU_SOURCE

#include <ctype.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "shadow_input_modules.h"
#include "input_module_api_v1.h"
#include "shadow_led_queue.h"

#define INPUT_TRACK_COUNT 4
#define INPUT_PARAM_SLOTS 32
#define INPUT_MODULE_ID_LEN 64
#define INPUT_PARAM_KEY_LEN 64
#define INPUT_PARAM_VALUE_LEN 128
#define INPUT_MODULE_DIR_LEN 256
#define INPUT_LED_MODE_LEN 24
#define INPUT_SCHEDULED_EVENTS 128

#define MOVE_MODE_NOTE 2
#define MOVE_PAD_NOTE_FIRST 68
#define MOVE_PAD_NOTE_LAST 99
#define CC_UP 55
#define CC_DOWN 54
#define DEFAULT_UI_OCTAVE_INDEX 2
#define MIN_UI_OCTAVE_INDEX 0
#define MAX_UI_OCTAVE_INDEX 8
#define USER_LIBRARY_SETS_DIR "/data/UserData/UserLibrary/Sets"
#define ACTIVE_SET_PATH "/data/UserData/schwung/active_set.txt"
#define SENTRY_DIR "/data/UserData/Sentry"
#define SENTRY_BREADCRUMB_1 "__sentry-breadcrumb1"
#define SENTRY_BREADCRUMB_2 "__sentry-breadcrumb2"
#define SENTRY_PATH_LEN 768
#define SENTRY_READ_LIMIT 8192
#define SENTRY_READ_OVERLAP 512
#define SENTRY_POLL_USEC 250000
#define MAX_SENTRY_WATCH_FILES 64
#define SONG_SET_POLL_USEC 2000000
#define SONG_SET_HEADER_READ_LIMIT 4096
#define SCREENREADER_DUPLICATE_USEC 100000
#define INPUT_PENDING_ROOT_UNCHANGED -2

typedef struct input_param_pair_t {
    char key[INPUT_PARAM_KEY_LEN];
    char value[INPUT_PARAM_VALUE_LEN];
} input_param_pair_t;

typedef struct input_track_runtime_t {
    char module_id[INPUT_MODULE_ID_LEN];
    char module_dir[INPUT_MODULE_DIR_LEN];
    char led_mode[INPUT_LED_MODE_LEN];
    void *handle;
    const input_module_api_v1_t *api;
    void *instance;
    int loaded;
    input_param_pair_t params[INPUT_PARAM_SLOTS];
    int param_count;
    uint8_t generated_notes[16][128];
} input_track_runtime_t;

typedef struct input_scheduled_event_t {
    int active;
    int track;
    uint64_t due_us;
    input_usb_midi_packet_t packet;
} input_scheduled_event_t;

typedef struct move_key_scale_state_t {
    int root_midi_class;
    char root_name[8];
    char scale_name[64];
    uint32_t generation;
} move_key_scale_state_t;

typedef struct sentry_watch_file_t {
    char path[SENTRY_PATH_LEN];
    long offset;
    time_t mtime_sec;
    long mtime_nsec;
    int initialized;
} sentry_watch_file_t;

static shadow_input_host_t g_host;
static input_track_runtime_t g_tracks[INPUT_TRACK_COUNT];
static host_input_api_v1_t g_module_host_api;
static input_scheduled_event_t g_scheduled[INPUT_SCHEDULED_EVENTS];
static pthread_t g_sentry_thread;
static volatile int g_sentry_running = 0;
static uint32_t g_mode_generation = 0;
static uint32_t g_track_generation = 0;
static uint32_t g_octave_generation = 0;
static int g_last_mode = 0;
static int g_last_track = 0;
static int g_last_led_owner_active = 0;
static sentry_watch_file_t g_sentry_files[MAX_SENTRY_WATCH_FILES];
static int g_sentry_initial_scan_done = 0;
static volatile uint32_t g_pending_key_scale_generation = 0;
static uint32_t g_applied_pending_key_scale_generation = 0;
static int g_pending_root_class = INPUT_PENDING_ROOT_UNCHANGED;
static char g_pending_root_name[8];
static char g_pending_scale_name[64];
static char g_last_sentry_scale_stamp[40];
static char g_last_sentry_root_stamp[40];
static char g_current_song_path[SENTRY_PATH_LEN];
static time_t g_song_mtime_sec = 0;
static long g_song_mtime_nsec = 0;
static int g_song_watch_initialized = 0;
static char g_last_screenreader_text[128];
static struct timespec g_last_screenreader_time;
static move_key_scale_state_t g_key_scale = {
    .root_midi_class = -1,
    .root_name = "",
    .scale_name = "",
    .generation = 0
};
static int g_track_octave_index[INPUT_TRACK_COUNT] = {
    DEFAULT_UI_OCTAVE_INDEX,
    DEFAULT_UI_OCTAVE_INDEX,
    DEFAULT_UI_OCTAVE_INDEX,
    DEFAULT_UI_OCTAVE_INDEX
};
static int g_track_color[INPUT_TRACK_COUNT] = { -1, -1, -1, -1 };

static void input_song_file_poll(void);

static uint64_t monotonic_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000ULL);
}

static void input_log(const char *msg)
{
    if (g_host.log) g_host.log(msg);
}

static shadow_control_t *input_control(void)
{
    return (g_host.shadow_control_ptr) ? *g_host.shadow_control_ptr : NULL;
}

static int clamp_track(int track)
{
    if (track < 0) return 0;
    if (track >= INPUT_TRACK_COUNT) return INPUT_TRACK_COUNT - 1;
    return track;
}

static int active_track(void)
{
    shadow_control_t *ctrl = input_control();
    return ctrl ? clamp_track((int)ctrl->selected_slot) : 0;
}

static int current_mode(void)
{
    shadow_control_t *ctrl = input_control();
    return ctrl ? (int)ctrl->move_ui_mode : 0;
}

static int clamp_octave_index(int octave_index)
{
    if (octave_index < MIN_UI_OCTAVE_INDEX) return MIN_UI_OCTAVE_INDEX;
    if (octave_index > MAX_UI_OCTAVE_INDEX) return MAX_UI_OCTAVE_INDEX;
    return octave_index;
}

static int current_octave_index(void)
{
    return g_track_octave_index[active_track()];
}

static void fill_context(input_context_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->active_track = active_track();
    ctx->move_mode = current_mode();
    ctx->root_note_class = g_key_scale.root_midi_class;
    ctx->octave_index = current_octave_index();
    ctx->root_name = g_key_scale.root_name;
    ctx->scale_name = g_key_scale.scale_name;
    ctx->playing = g_host.get_transport_playing ? g_host.get_transport_playing() : 0;
    ctx->bpm = g_host.get_bpm ? g_host.get_bpm() : 120.0;
    ctx->mode_generation = g_mode_generation;
    ctx->track_generation = g_track_generation;
    ctx->key_scale_generation = g_key_scale.generation;
}

static int input_get_active_track(void *ctx)
{
    (void)ctx;
    return active_track();
}

static int input_get_root_note_class(void *ctx)
{
    (void)ctx;
    return g_key_scale.root_midi_class;
}

static int input_get_octave_index(void *ctx)
{
    (void)ctx;
    return current_octave_index();
}

static const char *input_get_root_name(void *ctx)
{
    (void)ctx;
    return g_key_scale.root_name;
}

static const char *input_get_scale_name(void *ctx)
{
    (void)ctx;
    return g_key_scale.scale_name;
}

static int input_get_transport_playing(void *ctx)
{
    (void)ctx;
    return g_host.get_transport_playing ? g_host.get_transport_playing() : 0;
}

static double input_get_transport_bpm(void *ctx)
{
    (void)ctx;
    return g_host.get_bpm ? g_host.get_bpm() : 120.0;
}

static void input_host_log(void *ctx, const char *message)
{
    (void)ctx;
    input_log(message ? message : "input module log");
}

static int input_host_set_pad_led(void *ctx, int pad_index, uint8_t color)
{
    (void)ctx;
    return led_queue_set_input_pad_led(pad_index, color);
}

static int input_host_get_pad_led(void *ctx, int pad_index)
{
    (void)ctx;
    return led_queue_get_input_pad_led(pad_index);
}

static int input_host_get_track_color(void *ctx, int track_index)
{
    (void)ctx;
    if (track_index < 0 || track_index >= INPUT_TRACK_COUNT) return -1;
    return g_track_color[track_index];
}

static void input_pending_barrier(void)
{
    __sync_synchronize();
}

static int validate_packet(const input_usb_midi_packet_t *p)
{
    if (!p) return 0;
    if (p->cable != 2) return 0;
    if (p->status < 0x80) return 0;
    if (p->data1 > 127 || p->data2 > 127) return 0;
    uint8_t type = p->status & 0xF0;
    switch (type) {
        case 0x80: return p->cin == 0x08;
        case 0x90: return p->cin == 0x09;
        case 0xA0: return p->cin == 0x0A;
        case 0xB0: return p->cin == 0x0B;
        case 0xC0: return p->cin == 0x0C;
        case 0xD0: return p->cin == 0x0D;
        case 0xE0: return p->cin == 0x0E;
        default: return 0;
    }
}

static void track_output_note(input_track_runtime_t *track, const input_usb_midi_packet_t *p)
{
    if (!track || !p) return;
    uint8_t type = p->status & 0xF0;
    uint8_t ch = p->status & 0x0F;
    if (type == 0x90 && p->data2 > 0) {
        track->generated_notes[ch][p->data1] = 1;
    } else if (type == 0x80 || (type == 0x90 && p->data2 == 0)) {
        track->generated_notes[ch][p->data1] = 0;
    }
}

static int emit_validated(input_track_runtime_t *track,
                          const input_usb_midi_packet_t *packets,
                          int count)
{
    if (!packets || count <= 0) return 0;
    if (count > INPUT_MODULE_MAX_OUTPUT_PACKETS) count = INPUT_MODULE_MAX_OUTPUT_PACKETS;
    int sent = 0;
    for (int i = 0; i < count; i++) {
        const input_usb_midi_packet_t *p = &packets[i];
        if (!validate_packet(p)) {
            input_log("input module: dropped invalid output packet");
            continue;
        }
        uint8_t msg[4] = {
            (uint8_t)((p->cable << 4) | (p->cin & 0x0F)),
            p->status,
            p->data1,
            p->data2
        };
        if (g_host.emit_midi && g_host.emit_midi(msg, 4) == 4) {
            track_output_note(track, p);
            sent++;
        }
    }
    return sent;
}

static int input_emit_midi(void *ctx,
                           const input_usb_midi_packet_t *packets,
                           int count)
{
    input_track_runtime_t *track = (input_track_runtime_t *)ctx;
    return emit_validated(track, packets, count);
}

static void clear_scheduled_track(int track_num)
{
    for (int i = 0; i < INPUT_SCHEDULED_EVENTS; i++) {
        if (g_scheduled[i].active && g_scheduled[i].track == track_num) {
            g_scheduled[i].active = 0;
        }
    }
}

static int clear_scheduled_track_note(int track_num, int note, int channel)
{
    int cleared = 0;
    for (int i = 0; i < INPUT_SCHEDULED_EVENTS; i++) {
        if (!g_scheduled[i].active || g_scheduled[i].track != track_num) continue;
        const input_usb_midi_packet_t *p = &g_scheduled[i].packet;
        if (note >= 0 && p->data1 != (uint8_t)note) continue;
        if (channel >= 0 && (p->status & 0x0F) != (uint8_t)channel) continue;
        g_scheduled[i].active = 0;
        cleared++;
    }
    return cleared;
}

static int input_schedule_midi(void *ctx,
                               const input_usb_midi_packet_t *packets,
                               int count,
                               uint32_t delay_us)
{
    input_track_runtime_t *track = (input_track_runtime_t *)ctx;
    if (!track || !packets || count <= 0) return 0;
    if (count > INPUT_MODULE_MAX_OUTPUT_PACKETS) count = INPUT_MODULE_MAX_OUTPUT_PACKETS;
    int track_num = (int)(track - g_tracks);
    if (track_num < 0 || track_num >= INPUT_TRACK_COUNT) return 0;

    uint64_t due = monotonic_us() + (uint64_t)delay_us;
    int queued = 0;
    for (int i = 0; i < count; i++) {
        if (!validate_packet(&packets[i])) {
            input_log("input module: dropped invalid scheduled packet");
            continue;
        }
        int slot = -1;
        for (int j = 0; j < INPUT_SCHEDULED_EVENTS; j++) {
            if (!g_scheduled[j].active) {
                slot = j;
                break;
            }
        }
        if (slot < 0) {
            input_log("input module: scheduled MIDI queue full");
            break;
        }
        g_scheduled[slot].active = 1;
        g_scheduled[slot].track = track_num;
        g_scheduled[slot].due_us = due;
        g_scheduled[slot].packet = packets[i];
        queued++;
    }
    return queued;
}

static int input_cancel_scheduled_midi(void *ctx, int note, int channel)
{
    input_track_runtime_t *track = (input_track_runtime_t *)ctx;
    if (!track) return 0;
    int track_num = (int)(track - g_tracks);
    if (track_num < 0 || track_num >= INPUT_TRACK_COUNT) return 0;
    int n = (note >= 0 && note <= 127) ? note : -1;
    int ch = (channel >= 0 && channel < 16) ? channel : -1;
    return clear_scheduled_track_note(track_num, n, ch);
}

static void input_drain_scheduled(void)
{
    uint64_t now = monotonic_us();
    int drained = 0;
    for (int i = 0; i < INPUT_SCHEDULED_EVENTS && drained < 8; i++) {
        if (!g_scheduled[i].active || g_scheduled[i].due_us > now) continue;
        int track_num = g_scheduled[i].track;
        input_usb_midi_packet_t packet = g_scheduled[i].packet;
        g_scheduled[i].active = 0;
        if (track_num >= 0 && track_num < INPUT_TRACK_COUNT) {
            emit_validated(&g_tracks[track_num], &packet, 1);
            drained++;
        }
    }
}

static void input_tick_active_module(void)
{
    input_track_runtime_t *track = &g_tracks[active_track()];
    if (!track->loaded || !track->api || !track->instance || !track->api->on_tick) return;
    input_context_t ctx;
    fill_context(&ctx);
    track->api->on_tick(track->instance, &ctx);
}

static int track_wants_replace_pads(const input_track_runtime_t *track)
{
    return track && track->loaded &&
           strcmp(track->module_id, "native") != 0 &&
           strcmp(track->led_mode, "replace_pads") == 0;
}

static void notify_context_changed(input_track_runtime_t *track)
{
    if (!track || !track->api || !track->instance || !track->api->on_context_changed) return;
    input_context_t ctx;
    fill_context(&ctx);
    track->api->on_context_changed(track->instance, &ctx);
}

static void input_update_led_ownership(void)
{
    int track_num = active_track();
    input_track_runtime_t *track = &g_tracks[track_num];
    int want_owner = (current_mode() == MOVE_MODE_NOTE && track_wants_replace_pads(track));
    if (want_owner != g_last_led_owner_active) {
        led_queue_set_input_pad_owner(want_owner);
        g_last_led_owner_active = want_owner;
        if (want_owner) notify_context_changed(track);
    }
}

static void input_set_track_led_mode(input_track_runtime_t *track, const char *value, int force_refresh)
{
    if (!track) return;
    int was_active = (track == &g_tracks[active_track()]) && g_last_led_owner_active;
    if (value && (strcmp(value, "replace_pads") == 0 || strcmp(value, "Replace Pads") == 0)) {
        snprintf(track->led_mode, sizeof(track->led_mode), "replace_pads");
    } else {
        snprintf(track->led_mode, sizeof(track->led_mode), "native");
    }
    input_update_led_ownership();
    if (force_refresh && track == &g_tracks[active_track()]) {
        if (track_wants_replace_pads(track)) {
            notify_context_changed(track);
        } else if (was_active) {
            led_queue_set_input_pad_owner(0);
            g_last_led_owner_active = 0;
        }
    }
}

static void panic_track(input_track_runtime_t *track)
{
    if (!track) return;
    int track_num = (int)(track - g_tracks);
    if (track_num >= 0 && track_num < INPUT_TRACK_COUNT) clear_scheduled_track(track_num);
    input_context_t ctx;
    fill_context(&ctx);
    if (track->api && track->instance && track->api->on_all_notes_off) {
        track->api->on_all_notes_off(track->instance, &ctx);
    }

    for (int ch = 0; ch < 16; ch++) {
        int had_note = 0;
        for (int note = 0; note < 128; note++) {
            if (!track->generated_notes[ch][note]) continue;
            input_usb_midi_packet_t off = {
                .cin = 0x08,
                .status = (uint8_t)(0x80 | ch),
                .data1 = (uint8_t)note,
                .data2 = 0,
                .cable = 2
            };
            emit_validated(track, &off, 1);
            track->generated_notes[ch][note] = 0;
            had_note = 1;
        }
        if (had_note) {
            input_usb_midi_packet_t all_off = {
                .cin = 0x0B,
                .status = (uint8_t)(0xB0 | ch),
                .data1 = 123,
                .data2 = 0,
                .cable = 2
            };
            emit_validated(track, &all_off, 1);
        }
    }
}

void shadow_input_panic_all(void)
{
    for (int i = 0; i < INPUT_TRACK_COUNT; i++) panic_track(&g_tracks[i]);
}

static void unload_track(input_track_runtime_t *track)
{
    if (!track) return;
    panic_track(track);
    if (track == &g_tracks[active_track()]) {
        led_queue_set_input_pad_owner(0);
        g_last_led_owner_active = 0;
    }
    if (track->api && track->instance && track->api->destroy_instance) {
        track->api->destroy_instance(track->instance);
    }
    if (track->handle) dlclose(track->handle);
    track->handle = NULL;
    track->api = NULL;
    track->instance = NULL;
    track->loaded = 0;
    track->module_dir[0] = '\0';
    snprintf(track->led_mode, sizeof(track->led_mode), "native");
}

static int path_exists(const char *path)
{
    struct stat st;
    return path && stat(path, &st) == 0;
}

static int find_module_dir(const char *module_id, char *out, int out_len)
{
    const char *roots[] = {
        "/data/UserData/schwung/modules/inputs",
        "/data/UserData/schwung/modules/input_modules",
        "/data/UserData/schwung/modules",
        NULL
    };
    for (int i = 0; roots[i]; i++) {
        snprintf(out, out_len, "%s/%s", roots[i], module_id);
        char module_json[INPUT_MODULE_DIR_LEN + 32];
        snprintf(module_json, sizeof(module_json), "%s/module.json", out);
        if (path_exists(module_json)) return 1;
    }
    return 0;
}

static int read_module_led_mode(const char *module_dir, char *out, int out_len)
{
    if (!out || out_len <= 0) return 0;
    snprintf(out, out_len, "native");
    if (!module_dir || !module_dir[0]) return 0;

    char path[INPUT_MODULE_DIR_LEN + 32];
    snprintf(path, sizeof(path), "%s/module.json", module_dir);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char json[8193];
    size_t n = fread(json, 1, sizeof(json) - 1, f);
    fclose(f);
    json[n] = '\0';

    const char *p = strstr(json, "\"led_mode\"");
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '"') return 0;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < out_len - 1) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    if (strcmp(out, "replace_pads") != 0) snprintf(out, out_len, "native");
    return 1;
}

static void apply_stored_params(input_track_runtime_t *track)
{
    if (!track || !track->api || !track->instance || !track->api->set_param) return;
    for (int i = 0; i < track->param_count; i++) {
        if (strcmp(track->params[i].key, "led_mode") == 0) {
            input_set_track_led_mode(track, track->params[i].value, 0);
        }
        track->api->set_param(track->instance, track->params[i].key, track->params[i].value);
    }
}

int shadow_input_set_track_module(int track_index, const char *module_id)
{
    int track_num = clamp_track(track_index);
    input_track_runtime_t *track = &g_tracks[track_num];
    const char *id = (module_id && module_id[0]) ? module_id : "native";

    if (strcmp(track->module_id, id) == 0 && track->loaded) return 1;
    unload_track(track);
    snprintf(track->module_id, sizeof(track->module_id), "%s", id);
    snprintf(track->led_mode, sizeof(track->led_mode), "native");

    if (strcmp(id, "native") == 0) {
        input_update_led_ownership();
        return 1;
    }

    char module_dir[INPUT_MODULE_DIR_LEN];
    if (!find_module_dir(id, module_dir, sizeof(module_dir))) {
        char msg[160];
        snprintf(msg, sizeof(msg), "input module: %s not found; using native passthrough", id);
        input_log(msg);
        return 0;
    }

    char dsp_path[INPUT_MODULE_DIR_LEN + 32];
    snprintf(dsp_path, sizeof(dsp_path), "%s/dsp.so", module_dir);
    if (!path_exists(dsp_path)) {
        char msg[192];
        snprintf(msg, sizeof(msg), "input module: %s has no dsp.so; UI-only passthrough", id);
        input_log(msg);
        return 0;
    }

    void *handle = dlopen(dsp_path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        char msg[224];
        snprintf(msg, sizeof(msg), "input module: dlopen failed for %s: %s", id, dlerror());
        input_log(msg);
        return 0;
    }

    schwung_input_module_init_v1_fn init_fn =
        (schwung_input_module_init_v1_fn)dlsym(handle, SCHWUNG_INPUT_MODULE_INIT_V1_SYMBOL);
    if (!init_fn) {
        char msg[192];
        snprintf(msg, sizeof(msg), "input module: %s missing %s", id, SCHWUNG_INPUT_MODULE_INIT_V1_SYMBOL);
        input_log(msg);
        dlclose(handle);
        return 0;
    }

    g_module_host_api.ctx = track;
    const input_module_api_v1_t *api = init_fn(&g_module_host_api);
    if (!api || api->api_version != INPUT_MODULE_API_VERSION || !api->create_instance || !api->process_midi) {
        char msg[160];
        snprintf(msg, sizeof(msg), "input module: %s returned invalid API", id);
        input_log(msg);
        dlclose(handle);
        return 0;
    }

    void *instance = api->create_instance(module_dir, "{}");
    if (!instance) {
        char msg[160];
        snprintf(msg, sizeof(msg), "input module: %s create_instance failed", id);
        input_log(msg);
        dlclose(handle);
        return 0;
    }

    track->handle = handle;
    track->api = api;
    track->instance = instance;
    track->loaded = 1;
    snprintf(track->module_dir, sizeof(track->module_dir), "%s", module_dir);
    read_module_led_mode(module_dir, track->led_mode, sizeof(track->led_mode));
    apply_stored_params(track);
    input_update_led_ownership();
    if (track_num == active_track()) notify_context_changed(track);

    char msg[160];
    snprintf(msg, sizeof(msg), "input module: track %d loaded %s", track_num + 1, id);
    input_log(msg);
    return 1;
}

int shadow_input_set_track_param(int track_index, const char *key, const char *value)
{
    int track_num = clamp_track(track_index);
    input_track_runtime_t *track = &g_tracks[track_num];
    if (!key || !key[0]) return 0;
    const char *val = value ? value : "";

    int slot = -1;
    for (int i = 0; i < track->param_count; i++) {
        if (strcmp(track->params[i].key, key) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (track->param_count >= INPUT_PARAM_SLOTS) return 0;
        slot = track->param_count++;
        snprintf(track->params[slot].key, sizeof(track->params[slot].key), "%s", key);
    }
    snprintf(track->params[slot].value, sizeof(track->params[slot].value), "%s", val);

    if (strcmp(key, "led_mode") == 0) {
        input_set_track_led_mode(track, val, 1);
    }

    if (track->api && track->instance && track->api->set_param) {
        track->api->set_param(track->instance, key, val);
    }
    if (track_num == active_track()) notify_context_changed(track);
    return 1;
}

int shadow_input_get_track_param(int track_index, const char *key, char *buf, int buf_len)
{
    if (!buf || buf_len <= 0 || !key) return -1;
    int track_num = clamp_track(track_index);
    input_track_runtime_t *track = &g_tracks[track_num];
    if (strcmp(key, "module") == 0 || strcmp(key, "module_id") == 0) {
        return snprintf(buf, buf_len, "%s", track->module_id[0] ? track->module_id : "native");
    }
    if (track->api && track->instance && track->api->get_param) {
        int len = track->api->get_param(track->instance, key, buf, buf_len);
        if (len >= 0) return len;
    }
    for (int i = 0; i < track->param_count; i++) {
        if (strcmp(track->params[i].key, key) == 0) {
            return snprintf(buf, buf_len, "%s", track->params[i].value);
        }
    }
    return -1;
}

static const char *skip_ws(const char *p)
{
    while (p && *p && isspace((unsigned char)*p)) p++;
    return p;
}

static int parse_json_string_after(const char *start, const char *needle, char *out, int out_len)
{
    const char *p = strstr(start, needle);
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    p = skip_ws(p + 1);
    if (!p || *p != '"') return 0;
    p++;
    int n = 0;
    while (*p && *p != '"' && n < out_len - 1) {
        if (*p == '\\' && p[1]) p++;
        out[n++] = *p++;
    }
    out[n] = '\0';
    return n > 0;
}

static int parse_track_number(const char *obj)
{
    const char *p = strstr(obj, "\"track\"");
    if (!p) return -1;
    p = strchr(p, ':');
    if (!p) return -1;
    return atoi(p + 1);
}

static int root_class_from_name(const char *name)
{
    if (!name || !name[0]) return -1;
    char a = (char)toupper((unsigned char)name[0]);
    int base = -1;
    switch (a) {
        case 'C': base = 0; break;
        case 'D': base = 2; break;
        case 'E': base = 4; break;
        case 'F': base = 5; break;
        case 'G': base = 7; break;
        case 'A': base = 9; break;
        case 'B': base = 11; break;
        default: return -1;
    }
    const char *slash = strchr(name, '/');
    int accidental_scan_len = slash ? (int)(slash - name) : 8;
    if (accidental_scan_len < 0) accidental_scan_len = 0;
    if (accidental_scan_len > 16) accidental_scan_len = 16;
    size_t accidental_len = (size_t)accidental_scan_len;
    int has_sharp = memchr(name, '#', accidental_len) ||
                    memmem(name, accidental_len, "♯", strlen("♯"));
    int has_flat = memchr(name, 'b', accidental_len) ||
                   memchr(name, 'B', accidental_len) ||
                   memmem(name, accidental_len, "♭", strlen("♭"));
    if (has_sharp) base++;
    else if (has_flat) base--;
    return (base + 12) % 12;
}

static const char *root_name_from_class(int root)
{
    static const char *names[] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    if (root < 0 || root > 11) return "";
    return names[root];
}

static void compact_lower_string(const char *src, char *dst, int dst_len)
{
    int j = 0;
    for (int i = 0; src && src[i] && j < dst_len - 1; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == ' ' || c == '-' || c == '_') continue;
        dst[j++] = (char)tolower(c);
    }
    dst[j] = '\0';
}

static int parse_json_int_key(const char *json, const char *key, int *out)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (!isdigit((unsigned char)*p) && *p != '-') return 0;
    *out = atoi(p);
    return 1;
}

static int parse_json_string_key(const char *json, const char *key, char *out, int out_len)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    return parse_json_string_after(json, needle, out, out_len);
}

static void set_key_scale_state(int root_class, const char *root_name, const char *scale_name)
{
    char next_root[8] = "";
    char next_scale[64] = "";
    if (root_class >= 0 && root_class < 12) {
        snprintf(next_root, sizeof(next_root), "%s",
                 (root_name && root_name[0]) ? root_name : root_name_from_class(root_class));
    }
    if (scale_name && scale_name[0]) snprintf(next_scale, sizeof(next_scale), "%s", scale_name);

    if (g_key_scale.root_midi_class != root_class ||
        strcmp(g_key_scale.root_name, next_root) != 0 ||
        strcmp(g_key_scale.scale_name, next_scale) != 0) {
        g_key_scale.root_midi_class = root_class;
        snprintf(g_key_scale.root_name, sizeof(g_key_scale.root_name), "%s", next_root);
        snprintf(g_key_scale.scale_name, sizeof(g_key_scale.scale_name), "%s", next_scale);
        g_key_scale.generation++;
    }
}

static void input_queue_key_scale_update(int root_class,
                                         const char *root_name,
                                         const char *scale_name,
                                         const char *source)
{
    if (root_class != INPUT_PENDING_ROOT_UNCHANGED) {
        g_pending_root_class = root_class;
        snprintf(g_pending_root_name, sizeof(g_pending_root_name), "%s",
                 (root_name && root_name[0]) ? root_name : root_name_from_class(root_class));
    }
    if (scale_name && scale_name[0]) {
        snprintf(g_pending_scale_name, sizeof(g_pending_scale_name), "%s", scale_name);
    }
    char msg[160];
    if (root_class != INPUT_PENDING_ROOT_UNCHANGED) {
        snprintf(msg, sizeof(msg), "input module: %s root -> %s",
                 (source && source[0]) ? source : "key/scale",
                 (root_name && root_name[0]) ? root_name : root_name_from_class(root_class));
        input_log(msg);
    }
    if (scale_name && scale_name[0]) {
        snprintf(msg, sizeof(msg), "input module: %s scale -> %s",
                 (source && source[0]) ? source : "key/scale", scale_name);
        input_log(msg);
    }
    input_pending_barrier();
    g_pending_key_scale_generation++;
}

static void input_queue_sentry_key_scale_update(int root_class,
                                                const char *root_name,
                                                const char *scale_name)
{
    if (g_current_song_path[0]) return;
    input_queue_key_scale_update(root_class, root_name, scale_name, "Sentry");
}

static void input_queue_song_key_scale_update(int root_class,
                                              const char *root_name,
                                              const char *scale_name)
{
    input_queue_key_scale_update(root_class, root_name, scale_name, "Song.abl");
}

static void input_queue_screenreader_key_scale_update(int root_class,
                                                      const char *root_name,
                                                      const char *scale_name)
{
    input_queue_key_scale_update(root_class, root_name, scale_name, "ScreenReader");
}

static int input_apply_pending_key_scale(void)
{
    uint32_t pending_generation = g_pending_key_scale_generation;
    if (pending_generation == g_applied_pending_key_scale_generation) return 0;
    input_pending_barrier();

    int root_class = g_pending_root_class;
    char root_name[sizeof(g_pending_root_name)];
    char scale_name[sizeof(g_pending_scale_name)];
    snprintf(root_name, sizeof(root_name), "%s", g_pending_root_name);
    snprintf(scale_name, sizeof(scale_name), "%s", g_pending_scale_name);
    input_pending_barrier();
    if (pending_generation != g_pending_key_scale_generation) return 0;

    int next_root = (root_class == INPUT_PENDING_ROOT_UNCHANGED)
        ? g_key_scale.root_midi_class
        : root_class;
    const char *next_root_name = (root_class == INPUT_PENDING_ROOT_UNCHANGED)
        ? g_key_scale.root_name
        : root_name;
    const char *next_scale_name = scale_name[0] ? scale_name : g_key_scale.scale_name;

    uint32_t before = g_key_scale.generation;
    set_key_scale_state(next_root, next_root_name, next_scale_name);
    g_applied_pending_key_scale_generation = pending_generation;
    if (pending_generation == g_pending_key_scale_generation) {
        g_pending_root_class = INPUT_PENDING_ROOT_UNCHANGED;
        g_pending_root_name[0] = '\0';
        g_pending_scale_name[0] = '\0';
    }
    return before != g_key_scale.generation;
}

static char *trim_line(char *line)
{
    while (line && *line && isspace((unsigned char)*line)) line++;
    if (!line) return line;
    char *end = line + strlen(line);
    while (end > line && isspace((unsigned char)end[-1])) *--end = '\0';
    return line;
}

static int line_is_sentry_field(const char *line, const char *field)
{
    char compact[32];
    compact_lower_string(line, compact, sizeof(compact));
    return strcmp(compact, field) == 0;
}

static const char *sentry_set_to_value(const char *line)
{
    line = skip_ws(line);
    if (!line || strncasecmp(line, "set to ", 7) != 0) return NULL;
    return skip_ws(line + 7);
}

static const char *input_text_set_to_value(const char *text)
{
    if (!text || !text[0]) return NULL;
    const char *p = strcasestr(text, "set to ");
    if (!p) return NULL;
    return skip_ws(p + 7);
}

static int elapsed_usec_since(const struct timespec *before,
                              const struct timespec *after)
{
    if (!before || !after || before->tv_sec == 0) return SCREENREADER_DUPLICATE_USEC + 1;
    long sec = (long)(after->tv_sec - before->tv_sec);
    long nsec = after->tv_nsec - before->tv_nsec;
    return (int)(sec * 1000000L + nsec / 1000L);
}

static int is_duplicate_screenreader_text(const char *text)
{
    if (!text || !text[0]) return 1;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int duplicate = strcmp(g_last_screenreader_text, text) == 0 &&
        elapsed_usec_since(&g_last_screenreader_time, &now) < SCREENREADER_DUPLICATE_USEC;
    snprintf(g_last_screenreader_text, sizeof(g_last_screenreader_text), "%s", text);
    g_last_screenreader_time = now;
    return duplicate;
}

static int parse_screenreader_menu_item_text(const char *text)
{
    if (!text || !text[0]) return 0;
    const char *marker = strstr(text, ". Menu item. ");
    if (!marker) return 0;

    int item = 0;
    int total = 0;
    if (sscanf(marker + strlen(". Menu item. "), "%d of %d", &item, &total) != 2) return 0;

    char value[64];
    int n = (int)(marker - text);
    if (n <= 0 || n >= (int)sizeof(value)) return 0;
    memcpy(value, text, (size_t)n);
    value[n] = '\0';

    /* Native Move screen reader text uses 35 scale items and 12 root items:
       e.g. "Dorian #4. Menu item. 16 of 35" and "C♯/D♭. Menu item. 2 of 12". */
    if (total == 35 && item >= 1 && item <= 35) {
        input_queue_screenreader_key_scale_update(INPUT_PENDING_ROOT_UNCHANGED, NULL, value);
        return 1;
    }
    if (total == 12 && item >= 1 && item <= 12) {
        int root_class = root_class_from_name(value);
        if (root_class >= 0) {
            input_queue_screenreader_key_scale_update(root_class, root_name_from_class(root_class), NULL);
            return 1;
        }
    }
    return 0;
}

static void copy_key_scale_text_value(const char *src, char *out, int out_len)
{
    if (!out || out_len <= 0) return;
    int n = 0;
    for (int i = 0; src && src[i] && n < out_len - 1; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\n' || c == '\r') break;
        out[n++] = (char)c;
    }
    while (n > 0 && isspace((unsigned char)out[n - 1])) n--;
    out[n] = '\0';
}

void shadow_input_update_key_scale_from_text(const char *text)
{
    if (!text || !text[0]) return;
    if (!is_duplicate_screenreader_text(text) && parse_screenreader_menu_item_text(text)) return;

    const char *value = input_text_set_to_value(text);
    if (!value || !value[0]) return;

    if (strcasestr(text, "root note") || strcasestr(text, "root")) {
        char root_name[32];
        copy_key_scale_text_value(value, root_name, sizeof(root_name));
        int root_class = root_class_from_name(root_name);
        if (root_class >= 0) {
            input_queue_sentry_key_scale_update(root_class, root_name_from_class(root_class), NULL);
        }
        return;
    }

    if (strcasestr(text, "scale")) {
        char scale_name[64];
        copy_key_scale_text_value(value, scale_name, sizeof(scale_name));
        if (scale_name[0]) {
            input_queue_sentry_key_scale_update(INPUT_PENDING_ROOT_UNCHANGED, NULL, scale_name);
        }
    }
}

static int copy_sentry_embedded_value(const char *start, char *out, int out_len)
{
    if (!start || !out || out_len <= 0) return 0;
    int n = 0;
    while (*start && n < out_len - 1) {
        unsigned char c = (unsigned char)*start;
        if (c < 32 || c >= 128) {
            if (strncmp(start, "♯", strlen("♯")) == 0 && n < out_len - 4) {
                size_t len = strlen("♯");
                memcpy(out + n, start, len);
                n += (int)len;
                start += len;
                continue;
            }
            if (strncmp(start, "♭", strlen("♭")) == 0 && n < out_len - 4) {
                size_t len = strlen("♭");
                memcpy(out + n, start, len);
                n += (int)len;
                start += len;
                continue;
            }
            break;
        }
        if (*start == '\n' || *start == '\r') break;
        out[n++] = *start++;
    }
    while (n > 0 && isspace((unsigned char)out[n - 1])) n--;
    out[n] = '\0';
    return n > 0;
}

static int extract_sentry_timestamp_before(const char *text,
                                           const char *message,
                                           char *out,
                                           int out_len)
{
    if (!text || !message || !out || out_len <= 0 || message <= text) return 0;
    out[0] = '\0';
    const char *limit = message > text + 256 ? message - 256 : text;
    const char *p = message;
    while (p > limit) {
        p--;
        if (p + 20 >= message) continue;
        if (p[0] != '2' || p[1] != '0' || p[4] != '-' || p[7] != '-' || p[10] != 'T') {
            continue;
        }
        const char *end = p;
        while (end < message && *end && *end != 'Z') end++;
        if (end >= message || *end != 'Z') {
            continue;
        }
        int n = 0;
        while (p <= end && n < out_len - 1) out[n++] = *p++;
        out[n] = '\0';
        return n > 0;
    }
    return 0;
}

static int sentry_stamp_is_new(const char *stamp, char *last_stamp, int last_len)
{
    if (!stamp || !stamp[0]) return 0;
    if (!last_stamp || last_len <= 0) return 1;
    if (last_stamp[0] && strcmp(stamp, last_stamp) <= 0) return 0;
    snprintf(last_stamp, last_len, "%s", stamp);
    return 1;
}

static int sentry_stamp_is_later(const char *stamp, const char *best_stamp)
{
    if (!stamp || !stamp[0]) return !best_stamp || !best_stamp[0];
    if (!best_stamp || !best_stamp[0]) return 1;
    return strcmp(stamp, best_stamp) > 0;
}

static void parse_sentry_embedded_set_to_messages(const char *text)
{
    if (!text || !text[0]) return;
    const char *scale_pattern = "Scale\nset to ";
    const char *root_pattern = "Root note\nset to ";
    char best_scale_stamp[40] = "";
    char best_scale_value[64] = "";
    char best_root_stamp[40] = "";
    char best_root_value[32] = "";

    const char *p = text;
    while ((p = strstr(p, scale_pattern)) != NULL) {
        char value[64];
        char stamp[40] = "";
        const char *start = p + strlen(scale_pattern);
        extract_sentry_timestamp_before(text, p, stamp, sizeof(stamp));
        if (copy_sentry_embedded_value(start, value, sizeof(value))) {
            if (sentry_stamp_is_later(stamp, best_scale_stamp)) {
                snprintf(best_scale_stamp, sizeof(best_scale_stamp), "%s", stamp);
                snprintf(best_scale_value, sizeof(best_scale_value), "%s", value);
            }
        }
        p = start;
    }

    p = text;
    while ((p = strstr(p, root_pattern)) != NULL) {
        char value[32];
        char stamp[40] = "";
        const char *start = p + strlen(root_pattern);
        extract_sentry_timestamp_before(text, p, stamp, sizeof(stamp));
        if (copy_sentry_embedded_value(start, value, sizeof(value))) {
            if (sentry_stamp_is_later(stamp, best_root_stamp)) {
                snprintf(best_root_stamp, sizeof(best_root_stamp), "%s", stamp);
                snprintf(best_root_value, sizeof(best_root_value), "%s", value);
            }
        }
        p = start;
    }

    if (best_scale_value[0] &&
        sentry_stamp_is_new(best_scale_stamp, g_last_sentry_scale_stamp, sizeof(g_last_sentry_scale_stamp))) {
        input_queue_sentry_key_scale_update(INPUT_PENDING_ROOT_UNCHANGED, NULL, best_scale_value);
    }
    if (best_root_value[0] &&
        sentry_stamp_is_new(best_root_stamp, g_last_sentry_root_stamp, sizeof(g_last_sentry_root_stamp))) {
        int root_class = root_class_from_name(best_root_value);
        if (root_class >= 0) {
            input_queue_sentry_key_scale_update(root_class, root_name_from_class(root_class), NULL);
        }
    }
}

static void parse_sentry_key_scale_text(const char *text)
{
    if (!text || !text[0]) return;
    parse_sentry_embedded_set_to_messages(text);

    char copy[SENTRY_READ_LIMIT + SENTRY_READ_OVERLAP + 1];
    snprintf(copy, sizeof(copy), "%s", text);

    /* Native breadcrumbs are line pairs like `Scale` / `set to Minor` and
       `Root note` / `set to C#`. */
    char *save = NULL;
    char *line = strtok_r(copy, "\n", &save);
    while (line) {
        char *field = trim_line(line);
        int is_scale = line_is_sentry_field(field, "scale");
        int is_root = line_is_sentry_field(field, "rootnote") ||
                      line_is_sentry_field(field, "root");
        if (!is_scale && !is_root) {
            line = strtok_r(NULL, "\n", &save);
            continue;
        }

        char *value_line = NULL;
        while ((line = strtok_r(NULL, "\n", &save)) != NULL) {
            value_line = trim_line(line);
            if (value_line[0]) break;
        }
        const char *value = sentry_set_to_value(value_line);
        if (!value || !value[0]) continue;

        if (is_scale) {
            input_queue_sentry_key_scale_update(INPUT_PENDING_ROOT_UNCHANGED, NULL, value);
        } else {
            int root_class = root_class_from_name(value);
            if (root_class >= 0) {
                input_queue_sentry_key_scale_update(root_class, root_name_from_class(root_class), NULL);
            }
        }
    }
}

static int has_suffix(const char *s, const char *suffix)
{
    if (!s || !suffix) return 0;
    size_t s_len = strlen(s);
    size_t suffix_len = strlen(suffix);
    return s_len >= suffix_len && strcmp(s + s_len - suffix_len, suffix) == 0;
}

static long stat_mtime_nsec(const struct stat *st)
{
    if (!st) return 0;
#if defined(__APPLE__)
    return st->st_mtimespec.tv_nsec;
#elif defined(_GNU_SOURCE)
    return st->st_mtim.tv_nsec;
#else
    return 0;
#endif
}

static sentry_watch_file_t *sentry_watch_for_path(const char *path)
{
    if (!path || !path[0]) return NULL;
    sentry_watch_file_t *free_slot = NULL;
    for (int i = 0; i < MAX_SENTRY_WATCH_FILES; i++) {
        if (g_sentry_files[i].initialized && strcmp(g_sentry_files[i].path, path) == 0) {
            return &g_sentry_files[i];
        }
        if (!free_slot && !g_sentry_files[i].initialized) {
            free_slot = &g_sentry_files[i];
        }
    }
    return free_slot;
}

static void read_sentry_breadcrumb_file(sentry_watch_file_t *watch,
                                        const char *path,
                                        int parse_initial_tail)
{
    if (!watch || !path || !path[0]) return;
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        watch->path[0] = '\0';
        watch->offset = 0;
        watch->initialized = 0;
        return;
    }

    int new_watch = !watch->initialized || strcmp(watch->path, path) != 0;
    long oldest_allowed = (long)st.st_size > SENTRY_READ_LIMIT
        ? (long)st.st_size - SENTRY_READ_LIMIT
        : 0;
    if (!watch->initialized || strcmp(watch->path, path) != 0) {
        snprintf(watch->path, sizeof(watch->path), "%s", path);
        watch->offset = parse_initial_tail ? oldest_allowed : (long)st.st_size;
        watch->mtime_sec = st.st_mtime;
        watch->mtime_nsec = stat_mtime_nsec(&st);
        watch->initialized = 1;
        if (!parse_initial_tail) return;
    }

    long file_nsec = stat_mtime_nsec(&st);
    int mtime_changed = st.st_mtime != watch->mtime_sec || file_nsec != watch->mtime_nsec;
    if ((long)st.st_size < watch->offset) watch->offset = 0;
    if (!new_watch && (long)st.st_size <= watch->offset && !mtime_changed) return;

    long start = new_watch
        ? watch->offset
        : (mtime_changed && (long)st.st_size <= watch->offset)
        ? 0
        : (watch->offset > SENTRY_READ_OVERLAP
        ? watch->offset - SENTRY_READ_OVERLAP
        : 0);
    if (start < oldest_allowed) start = oldest_allowed;

    FILE *f = fopen(path, "r");
    if (!f) return;
    if (fseek(f, start, SEEK_SET) != 0) {
        fclose(f);
        return;
    }
    char buf[SENTRY_READ_LIMIT + 1];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    watch->offset = (long)st.st_size;
    watch->mtime_sec = st.st_mtime;
    watch->mtime_nsec = file_nsec;
    if (n == 0) return;
    buf[n] = '\0';
    parse_sentry_key_scale_text(buf);
}

static void input_sentry_poll_run_dir(const char *run_dir, int parse_initial_tail)
{
    if (!run_dir || !run_dir[0]) return;
    const char *files[] = { SENTRY_BREADCRUMB_1, SENTRY_BREADCRUMB_2, NULL };
    for (int i = 0; files[i]; i++) {
        char path[SENTRY_PATH_LEN];
        snprintf(path, sizeof(path), "%s/%s", run_dir, files[i]);
        sentry_watch_file_t *watch = sentry_watch_for_path(path);
        if (watch) read_sentry_breadcrumb_file(watch, path, parse_initial_tail);
    }
}

static void input_sentry_poll(void)
{
    DIR *dir = opendir(SENTRY_DIR);
    if (!dir) return;

    int parse_initial_tail = g_sentry_initial_scan_done;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.' || !has_suffix(entry->d_name, ".run")) continue;
        char run_dir[SENTRY_PATH_LEN];
        snprintf(run_dir, sizeof(run_dir), "%s/%s", SENTRY_DIR, entry->d_name);
        struct stat st;
        if (stat(run_dir, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        input_sentry_poll_run_dir(run_dir, parse_initial_tail);
    }
    closedir(dir);
    g_sentry_initial_scan_done = 1;
}

static void *input_sentry_watcher_main(void *arg)
{
    (void)arg;
    int song_poll_usec = SONG_SET_POLL_USEC;
    while (g_sentry_running) {
        song_poll_usec += SENTRY_POLL_USEC;
        if (song_poll_usec >= SONG_SET_POLL_USEC) {
            song_poll_usec = 0;
            input_song_file_poll();
        }
        usleep(SENTRY_POLL_USEC);
    }
    return NULL;
}

static void input_sentry_watcher_start(void)
{
    if (g_sentry_running) return;
    memset(g_sentry_files, 0, sizeof(g_sentry_files));
    g_sentry_initial_scan_done = 0;
    g_sentry_running = 1;
    if (pthread_create(&g_sentry_thread, NULL, input_sentry_watcher_main, NULL) != 0) {
        g_sentry_running = 0;
        input_log("input module: failed to start Sentry key/scale watcher");
    }
}

static void input_sentry_watcher_stop(void)
{
    if (!g_sentry_running) return;
    g_sentry_running = 0;
    pthread_join(g_sentry_thread, NULL);
}

static int parse_key_scale_values_from_song(const char *json,
                                            int *root_class_out,
                                            char *root_name_out,
                                            int root_name_len,
                                            char *scale_name_out,
                                            int scale_name_len)
{
    int root_class = -1;
    int root_value = -1;
    char root_name[8] = "";
    char scale_name[64] = "";
    const char *root_int_keys[] = {
        "rootNote", "root_note", "rootNoteClass", "root_midi_class", "keyRoot", NULL
    };
    const char *root_string_keys[] = {
        "rootName", "root_name", "rootNoteName", "key", "scaleRoot", NULL
    };
    const char *scale_string_keys[] = {
        "scaleName", "scale_name", "selectedScale", "scale", NULL
    };

    for (int i = 0; root_int_keys[i]; i++) {
        if (parse_json_int_key(json, root_int_keys[i], &root_value)) {
            root_class = ((root_value % 12) + 12) % 12;
            break;
        }
    }
    for (int i = 0; root_string_keys[i] && root_class < 0; i++) {
        if (parse_json_string_key(json, root_string_keys[i], root_name, sizeof(root_name))) {
            root_class = root_class_from_name(root_name);
        }
    }
    for (int i = 0; scale_string_keys[i]; i++) {
        if (parse_json_string_key(json, scale_string_keys[i], scale_name, sizeof(scale_name))) break;
    }
    if (root_class_out) *root_class_out = root_class;
    if (root_name_out && root_name_len > 0) {
        snprintf(root_name_out, root_name_len, "%s",
                 root_class >= 0 ? root_name_from_class(root_class) : root_name);
    }
    if (scale_name_out && scale_name_len > 0) {
        snprintf(scale_name_out, scale_name_len, "%s", scale_name);
    }
    return root_class >= 0 || scale_name[0] != '\0';
}

static void parse_key_scale_from_song(const char *json)
{
    int root_class = -1;
    char root_name[8] = "";
    char scale_name[64] = "";
    parse_key_scale_values_from_song(json, &root_class, root_name, sizeof(root_name),
                                     scale_name, sizeof(scale_name));
    set_key_scale_state(root_class, root_name, scale_name);
}

static void reset_track_colors(void)
{
    for (int i = 0; i < INPUT_TRACK_COUNT; i++) {
        g_track_color[i] = -1;
    }
}

static const char *skip_json_string(const char *p, const char *end)
{
    if (!p || p >= end || *p != '"') return p;
    p++;
    while (p < end && *p) {
        if (*p == '\\' && p + 1 < end) {
            p += 2;
            continue;
        }
        if (*p == '"') return p + 1;
        p++;
    }
    return p;
}

static int parse_direct_int_key(const char *obj_start, const char *obj_end,
                                const char *key, int *out)
{
    if (!obj_start || !obj_end || !key || !out) return 0;
    int depth = 0;
    size_t key_len = strlen(key);
    for (const char *p = obj_start; p < obj_end && *p; p++) {
        if (*p == '"') {
            if (depth == 1 && p + key_len + 2 <= obj_end &&
                p[1 + key_len] == '"' &&
                strncmp(p + 1, key, key_len) == 0) {
                const char *colon = p + key_len + 2;
                while (colon < obj_end && isspace((unsigned char)*colon)) colon++;
                if (colon >= obj_end || *colon != ':') {
                    p = skip_json_string(p, obj_end) - 1;
                    continue;
                }
                const char *val = skip_ws(colon + 1);
                if (!val || val >= obj_end || (!isdigit((unsigned char)*val) && *val != '-')) {
                    p = skip_json_string(p, obj_end) - 1;
                    continue;
                }
                *out = atoi(val);
                return 1;
            }
            p = skip_json_string(p, obj_end) - 1;
            continue;
        }
        if (*p == '{') depth++;
        else if (*p == '}') depth--;
    }
    return 0;
}

static const char *find_matching_json_close(const char *open, char close_char)
{
    if (!open) return NULL;
    char open_char = *open;
    int depth = 0;
    for (const char *p = open; *p; p++) {
        if (*p == '"') {
            p = skip_json_string(p, p + strlen(p));
            if (!p || !*p) return p;
            p--;
            continue;
        }
        if (*p == open_char) depth++;
        else if (*p == close_char) {
            depth--;
            if (depth == 0) return p;
        }
    }
    return NULL;
}

static void parse_track_colors_from_song(const char *json)
{
    reset_track_colors();
    const char *tracks_key = strstr(json, "\"tracks\"");
    if (!tracks_key) return;
    const char *tracks_open = strchr(tracks_key, '[');
    if (!tracks_open) return;
    const char *tracks_close = find_matching_json_close(tracks_open, ']');
    if (!tracks_close) return;

    int track = 0;
    for (const char *p = tracks_open + 1; p < tracks_close && track < INPUT_TRACK_COUNT; p++) {
        if (*p == '"') {
            p = skip_json_string(p, tracks_close) - 1;
            continue;
        }
        if (*p != '{') continue;
        const char *obj_end = find_matching_json_close(p, '}');
        if (!obj_end || obj_end > tracks_close) break;
        int color = -1;
        if (parse_direct_int_key(p, obj_end, "color", &color) && color >= 0 && color <= 127) {
            g_track_color[track] = color;
        }
        track++;
        p = obj_end;
    }
}

static void reset_track_octaves(void)
{
    for (int i = 0; i < INPUT_TRACK_COUNT; i++) {
        g_track_octave_index[i] = DEFAULT_UI_OCTAVE_INDEX;
    }
    g_octave_generation++;
}

static int read_active_set_uuid(char *uuid_out, int uuid_len, char *name_out, int name_len)
{
    if (uuid_out && uuid_len > 0) uuid_out[0] = '\0';
    if (name_out && name_len > 0) name_out[0] = '\0';
    FILE *f = fopen(ACTIVE_SET_PATH, "r");
    if (!f) return 0;
    int ok = 0;
    if (uuid_out && uuid_len > 0 && fgets(uuid_out, uuid_len, f)) {
        char *end = uuid_out + strlen(uuid_out);
        while (end > uuid_out && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ')) *--end = '\0';
        ok = uuid_out[0] != '\0';
    }
    if (name_out && name_len > 0 && fgets(name_out, name_len, f)) {
        char *end = name_out + strlen(name_out);
        while (end > name_out && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ')) *--end = '\0';
    }
    fclose(f);
    return ok;
}

static int uuid_from_state_dir(const char *state_dir, char *uuid_out, int uuid_len)
{
    if (!state_dir || !uuid_out || uuid_len <= 0) return 0;
    const char *marker = strstr(state_dir, "/set_state/");
    if (!marker) return 0;
    const char *uuid = marker + strlen("/set_state/");
    if (!uuid[0]) return 0;
    int n = 0;
    while (uuid[n] && uuid[n] != '/' && n < uuid_len - 1) {
        uuid_out[n] = uuid[n];
        n++;
    }
    uuid_out[n] = '\0';
    return n > 0;
}

static int find_song_path_for_uuid(const char *uuid, const char *set_name, char *out, int out_len)
{
    if (!uuid || !uuid[0] || !out || out_len <= 0) return 0;
    if (set_name && set_name[0]) {
        snprintf(out, out_len, "%s/%s/%s/Song.abl", USER_LIBRARY_SETS_DIR, uuid, set_name);
        if (path_exists(out)) return 1;
    }

    char uuid_dir[512];
    snprintf(uuid_dir, sizeof(uuid_dir), "%s/%s", USER_LIBRARY_SETS_DIR, uuid);
    DIR *dir = opendir(uuid_dir);
    if (!dir) return 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        snprintf(out, out_len, "%s/%s/Song.abl", uuid_dir, entry->d_name);
        if (path_exists(out)) {
            closedir(dir);
            return 1;
        }
    }
    closedir(dir);
    return 0;
}

static void input_song_watch_clear(void)
{
    g_current_song_path[0] = '\0';
    g_song_mtime_sec = 0;
    g_song_mtime_nsec = 0;
    g_song_watch_initialized = 0;
}

static void input_song_watch_set_path(const char *song_path)
{
    if (!song_path || !song_path[0]) {
        input_song_watch_clear();
        return;
    }
    snprintf(g_current_song_path, sizeof(g_current_song_path), "%s", song_path);
    struct stat st;
    if (stat(g_current_song_path, &st) == 0 && S_ISREG(st.st_mode)) {
        g_song_mtime_sec = st.st_mtime;
        g_song_mtime_nsec = stat_mtime_nsec(&st);
        g_song_watch_initialized = 1;
    } else {
        g_song_mtime_sec = 0;
        g_song_mtime_nsec = 0;
        g_song_watch_initialized = 0;
    }
}

static int input_read_song_key_scale_header(const char *song_path,
                                            int *root_class,
                                            char *root_name,
                                            int root_name_len,
                                            char *scale_name,
                                            int scale_name_len)
{
    FILE *f = fopen(song_path, "r");
    if (!f) return 0;
    char buf[SONG_SET_HEADER_READ_LIMIT + 1];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return 0;
    buf[n] = '\0';

    char *tracks = strstr(buf, "\"tracks\"");
    if (tracks) *tracks = '\0';
    return parse_key_scale_values_from_song(buf, root_class, root_name, root_name_len,
                                            scale_name, scale_name_len);
}

static void input_song_file_poll(void)
{
    if (!g_current_song_path[0]) return;
    struct stat st;
    if (stat(g_current_song_path, &st) != 0 || !S_ISREG(st.st_mode)) return;

    long file_nsec = stat_mtime_nsec(&st);
    if (g_song_watch_initialized &&
        st.st_mtime == g_song_mtime_sec &&
        file_nsec == g_song_mtime_nsec) {
        return;
    }

    int root_class = -1;
    char root_name[8] = "";
    char scale_name[64] = "";
    if (!input_read_song_key_scale_header(g_current_song_path,
                                          &root_class,
                                          root_name,
                                          sizeof(root_name),
                                          scale_name,
                                          sizeof(scale_name))) {
        return;
    }

    g_song_mtime_sec = st.st_mtime;
    g_song_mtime_nsec = file_nsec;
    g_song_watch_initialized = 1;
    input_queue_song_key_scale_update(root_class, root_name, scale_name);
}

static void load_track_octaves_from_song(const char *song_path)
{
    reset_track_octaves();
    reset_track_colors();
    FILE *f = fopen(song_path, "r");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 4 * 1024 * 1024) {
        fclose(f);
        return;
    }
    char *json = malloc((size_t)size + 1);
    if (!json) {
        fclose(f);
        return;
    }
    size_t n = fread(json, 1, (size_t)size, f);
    json[n] = '\0';
    fclose(f);

    int track = 0;
    const char *p = json;
    while (track < INPUT_TRACK_COUNT && (p = strstr(p, "\"uiOctaveIndex\"")) != NULL) {
        const char *colon = strchr(p, ':');
        if (!colon) break;
        g_track_octave_index[track] = clamp_octave_index(atoi(colon + 1));
        track++;
        p = colon + 1;
    }
    parse_key_scale_from_song(json);
    parse_track_colors_from_song(json);
    free(json);
    char msg[256];
    snprintf(msg, sizeof(msg), "input module: track octaves [%d,%d,%d,%d] colors [%d,%d,%d,%d] from %s",
             g_track_octave_index[0], g_track_octave_index[1],
             g_track_octave_index[2], g_track_octave_index[3],
             g_track_color[0], g_track_color[1],
             g_track_color[2], g_track_color[3],
             song_path ? song_path : "(none)");
    input_log(msg);
}

static void load_track_octaves_for_state_dir(const char *state_dir)
{
    char uuid[128] = "";
    char set_name[128] = "";
    char song_path[768] = "";
    if (!uuid_from_state_dir(state_dir, uuid, sizeof(uuid))) {
        read_active_set_uuid(uuid, sizeof(uuid), set_name, sizeof(set_name));
    } else {
        char active_uuid[128] = "";
        char active_name[128] = "";
        if (read_active_set_uuid(active_uuid, sizeof(active_uuid), active_name, sizeof(active_name)) &&
            strcmp(active_uuid, uuid) == 0) {
            snprintf(set_name, sizeof(set_name), "%s", active_name);
        }
    }
    if (find_song_path_for_uuid(uuid, set_name, song_path, sizeof(song_path))) {
        load_track_octaves_from_song(song_path);
        input_song_watch_set_path(song_path);
    } else {
        input_song_watch_clear();
        reset_track_octaves();
        reset_track_colors();
        set_key_scale_state(-1, "", "");
    }
}

static void parse_track_params(input_track_runtime_t *track, const char *obj_start, const char *obj_end)
{
    const char *params = strstr(obj_start, "\"params\"");
    if (!params || params > obj_end) return;
    const char *open = strchr(params, '{');
    if (!open || open > obj_end) return;
    const char *close = strchr(open, '}');
    if (!close || close > obj_end) return;

    const char *p = open + 1;
    while (p < close && track->param_count < INPUT_PARAM_SLOTS) {
        p = skip_ws(p);
        if (!p || p >= close || *p == '}') break;
        if (*p != '"') { p++; continue; }
        p++;
        char key[INPUT_PARAM_KEY_LEN] = "";
        int kn = 0;
        while (p < close && *p && *p != '"' && kn < (int)sizeof(key) - 1) key[kn++] = *p++;
        key[kn] = '\0';
        const char *colon = strchr(p, ':');
        if (!colon || colon > close) break;
        p = skip_ws(colon + 1);
        char value[INPUT_PARAM_VALUE_LEN] = "";
        int vn = 0;
        if (*p == '"') {
            p++;
            while (p < close && *p && *p != '"' && vn < (int)sizeof(value) - 1) {
                if (*p == '\\' && p[1]) p++;
                value[vn++] = *p++;
            }
            if (*p == '"') p++;
        } else {
            while (p < close && *p && *p != ',' && *p != '}' &&
                   vn < (int)sizeof(value) - 1) {
                value[vn++] = *p++;
            }
            while (vn > 0 && isspace((unsigned char)value[vn - 1])) vn--;
        }
        value[vn] = '\0';
        if (key[0]) shadow_input_set_track_param((int)(track - g_tracks), key, value);
        const char *comma = strchr(p, ',');
        if (!comma || comma > close) break;
        p = comma + 1;
    }
}

int shadow_input_load_state_dir(const char *state_dir)
{
    load_track_octaves_for_state_dir(state_dir);

    char path[512];
    snprintf(path, sizeof(path), "%s/input_modules.json",
             (state_dir && state_dir[0]) ? state_dir : "/data/UserData/schwung/slot_state");
    FILE *f = fopen(path, "r");
    if (!f) {
        for (int i = 0; i < INPUT_TRACK_COUNT; i++) shadow_input_set_track_module(i, "native");
        input_update_led_ownership();
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 65536) {
        fclose(f);
        return 0;
    }
    char *json = malloc((size_t)size + 1);
    if (!json) {
        fclose(f);
        return 0;
    }
    size_t n = fread(json, 1, (size_t)size, f);
    json[n] = '\0';
    fclose(f);

    for (int i = 0; i < INPUT_TRACK_COUNT; i++) {
        g_tracks[i].param_count = 0;
        shadow_input_set_track_module(i, "native");
    }

    const char *p = json;
    while ((p = strstr(p, "\"track\"")) != NULL) {
        const char *obj_start = p;
        while (obj_start > json && *obj_start != '{') obj_start--;
        const char *obj_end = strchr(p, '}');
        if (!obj_end) break;
        int track_num = parse_track_number(obj_start);
        if (track_num >= 0 && track_num < INPUT_TRACK_COUNT) {
            char module_id[INPUT_MODULE_ID_LEN] = "native";
            parse_json_string_after(obj_start, "\"module\"", module_id, sizeof(module_id));
            g_tracks[track_num].param_count = 0;
            snprintf(g_tracks[track_num].module_id, sizeof(g_tracks[track_num].module_id), "%s", module_id);
            parse_track_params(&g_tracks[track_num], obj_start, obj_end);
            shadow_input_set_track_module(track_num, module_id);
        }
        p = obj_end + 1;
    }
    free(json);
    input_update_led_ownership();
    notify_context_changed(&g_tracks[active_track()]);
    return 1;
}

void shadow_input_process_control_event(const uint8_t usb_packet[4])
{
    if (!usb_packet) return;
    uint8_t cin = usb_packet[0] & 0x0F;
    uint8_t cable = (usb_packet[0] >> 4) & 0x0F;
    uint8_t status = usb_packet[1];
    uint8_t d1 = usb_packet[2];
    uint8_t d2 = usb_packet[3];
    if (cable != 0 || cin != 0x0B || (status & 0xF0) != 0xB0 || d2 == 0) return;
    int track = active_track();
    int next = g_track_octave_index[track];
    if (d1 == CC_UP) {
        next++;
    } else if (d1 == CC_DOWN) {
        next--;
    } else {
        return;
    }
    next = clamp_octave_index(next);
    if (next != g_track_octave_index[track]) {
        g_track_octave_index[track] = next;
        g_octave_generation++;
        char msg[128];
        snprintf(msg, sizeof(msg), "input module: track %d octave -> %d", track + 1, next);
        input_log(msg);
    }
}

void shadow_input_tick_context(void)
{
    int mode = current_mode();
    int track = active_track();
    int changed = 0;
    if (input_apply_pending_key_scale()) {
        changed = 1;
    }
    if (g_last_mode == MOVE_MODE_NOTE && mode != MOVE_MODE_NOTE) {
        shadow_input_panic_all();
    }
    if (mode != g_last_mode) {
        g_mode_generation++;
        g_last_mode = mode;
        changed = 1;
    }
    if (track != g_last_track) {
        if (g_last_led_owner_active) {
            led_queue_set_input_pad_owner(0);
            g_last_led_owner_active = 0;
        }
        shadow_input_panic_all();
        g_track_generation++;
        g_last_track = track;
        changed = 1;
    }
    input_update_led_ownership();
    input_tick_active_module();
    input_drain_scheduled();
    if (changed) notify_context_changed(&g_tracks[track]);
}

int shadow_input_process_pad_event(const uint8_t usb_packet[4])
{
    if (!usb_packet) return 0;
    shadow_input_tick_context();
    if (current_mode() != MOVE_MODE_NOTE) return 0;

    int track_num = active_track();
    input_track_runtime_t *track = &g_tracks[track_num];
    if (!track->loaded || strcmp(track->module_id, "native") == 0 ||
        !track->api || !track->instance || !track->api->process_midi) {
        return 0;
    }

    input_usb_midi_packet_t in = {
        .cin = (uint8_t)(usb_packet[0] & 0x0F),
        .status = usb_packet[1],
        .data1 = usb_packet[2],
        .data2 = usb_packet[3],
        .cable = (uint8_t)((usb_packet[0] >> 4) & 0x0F)
    };
    input_context_t ctx;
    fill_context(&ctx);
    input_process_result_t result;
    memset(&result, 0, sizeof(result));
    int rc = track->api->process_midi(track->instance, &in, &ctx, &result);
    if (rc != 0) {
        input_log("input module: process_midi failed; passing native event");
        return 0;
    }
    emit_validated(track, result.outputs, result.output_count);
    if (result.request_led_redraw) notify_context_changed(track);
    return result.handled ? 1 : 0;
}

void shadow_input_modules_init(const shadow_input_host_t *host)
{
    memset(g_tracks, 0, sizeof(g_tracks));
    if (host) g_host = *host;
    memset(&g_module_host_api, 0, sizeof(g_module_host_api));
    g_module_host_api.emit_midi = input_emit_midi;
    g_module_host_api.schedule_midi = input_schedule_midi;
    g_module_host_api.cancel_scheduled_midi = input_cancel_scheduled_midi;
    g_module_host_api.set_pad_led = input_host_set_pad_led;
    g_module_host_api.get_pad_led = input_host_get_pad_led;
    g_module_host_api.get_track_color = input_host_get_track_color;
    g_module_host_api.get_active_track = input_get_active_track;
    g_module_host_api.get_root_note_class = input_get_root_note_class;
    g_module_host_api.get_octave_index = input_get_octave_index;
    g_module_host_api.get_root_name = input_get_root_name;
    g_module_host_api.get_scale_name = input_get_scale_name;
    g_module_host_api.get_transport_playing = input_get_transport_playing;
    g_module_host_api.get_transport_bpm = input_get_transport_bpm;
    g_module_host_api.log = input_host_log;
    for (int i = 0; i < INPUT_TRACK_COUNT; i++) {
        snprintf(g_tracks[i].module_id, sizeof(g_tracks[i].module_id), "native");
        snprintf(g_tracks[i].led_mode, sizeof(g_tracks[i].led_mode), "native");
    }
    input_song_watch_clear();
    g_last_mode = current_mode();
    g_last_track = active_track();
    input_sentry_watcher_start();
}

void shadow_input_modules_shutdown(void)
{
    input_sentry_watcher_stop();
    input_song_watch_clear();
    shadow_input_panic_all();
    led_queue_set_input_pad_owner(0);
    g_last_led_owner_active = 0;
    for (int i = 0; i < INPUT_TRACK_COUNT; i++) unload_track(&g_tracks[i]);
}
