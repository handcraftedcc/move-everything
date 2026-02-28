#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>

#include "host/plugin_api_v1.h"
#include "host/shadow_midi_to_move.h"

#define DEFAULT_BPM 120
#define MIN_BPM 30
#define MAX_BPM 300

#define DEFAULT_VELOCITY 100
#define MIN_VELOCITY 1
#define MAX_VELOCITY 127

#define DEFAULT_GATE_PERCENT 50
#define MIN_GATE_PERCENT 5
#define MAX_GATE_PERCENT 95

#define STAGE_STEPS 8u
#define TOTAL_PATTERN_STEPS (STAGE_STEPS * 3u)
#define SELF_ECHO_WINDOW_MS 80u
#define SELF_ECHO_RING_SIZE 128u

typedef struct self_echo_event {
    uint64_t timestamp_ms;
    uint8_t status;
    uint8_t d1;
    uint8_t d2;
} self_echo_event_t;

typedef enum source_mode {
    SOURCE_MODE_BOTH = 0,
    SOURCE_MODE_INTERNAL = 1,
    SOURCE_MODE_EXTERNAL = 2
} source_mode_t;

typedef struct midi_inject_test_instance {
    uint32_t received_packets;
    uint32_t emitted_packets;
    uint32_t dropped_packets;
    uint32_t dropped_source;
    uint32_t dropped_system;
    uint32_t dropped_non_note;
    uint32_t dropped_knob_touch;
    uint32_t dropped_queue_full;
    uint32_t pattern_steps;
    uint32_t rx_note_events;
    uint32_t rx_aftertouch_events;

    uint8_t last_status;
    uint8_t last_source;

    int output_channel;              /* -1 = passthrough, 0..15 = force channel */
    int bpm;
    int velocity;
    int gate_percent;
    source_mode_t source_mode;

    uint8_t held_notes[128];
    uint8_t held_count;

    uint8_t trigger_note;
    uint8_t trigger_channel;
    uint8_t running;

    uint32_t step_index;
    int step_samples;
    int gate_samples;
    int samples_to_next_step;
    int samples_to_note_off;         /* -1 when none pending */

    uint8_t sounding_notes[3];
    uint8_t sounding_count;
    uint8_t stop_requested;

    self_echo_event_t echo_ring[SELF_ECHO_RING_SIZE];
    uint32_t echo_ring_write;
    uint32_t dropped_self_echo;
} midi_inject_test_instance_t;

static const host_api_v1_t *g_host = NULL;
static int g_log_fd = -1;
static uint32_t g_instance_count = 0;
static const char *k_log_path = "/data/UserData/move-anything/midi_inject_test.log";
static const char *k_ui_hierarchy_json =
    "{"
        "\"levels\":{"
            "\"root\":{"
                "\"label\":\"MIDI Inject Test\","
                "\"params\":["
                    "{\"key\":\"out_channel\",\"label\":\"Output Ch\"},"
                    "{\"key\":\"bpm\",\"label\":\"BPM\"},"
                    "{\"key\":\"velocity\",\"label\":\"Velocity\"},"
                    "{\"key\":\"gate_percent\",\"label\":\"Gate %\"},"
                    "{\"key\":\"source_mode\",\"label\":\"Input Src\"}"
                "],"
                "\"knobs\":[\"out_channel\",\"bpm\",\"velocity\",\"gate_percent\"]"
            "}"
        "}"
    "}";
static const char *k_chain_params_json =
    "["
        "{\"key\":\"out_channel\",\"name\":\"Output Ch\",\"type\":\"int\",\"min\":0,\"max\":16,\"default\":0,\"step\":1},"
        "{\"key\":\"bpm\",\"name\":\"BPM\",\"type\":\"int\",\"min\":30,\"max\":300,\"default\":120,\"step\":1},"
        "{\"key\":\"velocity\",\"name\":\"Velocity\",\"type\":\"int\",\"min\":1,\"max\":127,\"default\":100,\"step\":1},"
        "{\"key\":\"gate_percent\",\"name\":\"Gate %\",\"type\":\"int\",\"min\":5,\"max\":95,\"default\":50,\"step\":1},"
        "{\"key\":\"source_mode\",\"name\":\"Input Src\",\"type\":\"enum\",\"options\":[\"both\",\"internal\",\"external\"],\"default\":2}"
    "]";

static void midi_inject_log_open(void)
{
    if (g_log_fd >= 0) return;
    g_log_fd = open(k_log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
}

static void midi_inject_log_close(void)
{
    if (g_log_fd >= 0) {
        close(g_log_fd);
        g_log_fd = -1;
    }
}

static void midi_inject_logf(const char *fmt, ...)
{
    if (!fmt) return;
    midi_inject_log_open();
    if (g_log_fd < 0) return;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tmv;
    localtime_r(&ts.tv_sec, &tmv);

    char msg[512];
    int prefix_len = snprintf(msg, sizeof(msg), "%02d:%02d:%02d.%03ld [midi_inject_test] ",
                              tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ts.tv_nsec / 1000000L);
    if (prefix_len < 0 || prefix_len >= (int)sizeof(msg)) return;

    va_list ap;
    va_start(ap, fmt);
    int body_len = vsnprintf(msg + prefix_len, sizeof(msg) - (size_t)prefix_len, fmt, ap);
    va_end(ap);
    if (body_len < 0) return;

    size_t total_len = (size_t)prefix_len + (size_t)body_len;
    if (total_len >= sizeof(msg)) total_len = sizeof(msg) - 1u;

    msg[total_len++] = '\n';
    (void)write(g_log_fd, msg, total_len);
}

static int should_log_sample(uint32_t count, uint32_t interval_mask)
{
    return (count <= 8u) || ((count & interval_mask) == 0u);
}

static uint64_t now_mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000u) + (uint64_t)(ts.tv_nsec / 1000000u);
}

static int clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static const char *source_mode_to_string(source_mode_t mode)
{
    switch (mode) {
        case SOURCE_MODE_INTERNAL: return "internal";
        case SOURCE_MODE_EXTERNAL: return "external";
        default: return "both";
    }
}

static source_mode_t parse_source_mode(const char *val)
{
    if (!val) return SOURCE_MODE_BOTH;
    if (strcmp(val, "internal") == 0) return SOURCE_MODE_INTERNAL;
    if (strcmp(val, "external") == 0) return SOURCE_MODE_EXTERNAL;
    if (strcmp(val, "both") == 0) return SOURCE_MODE_BOTH;

    /* Accept numeric enum index as fallback. */
    int idx = atoi(val);
    if (idx <= 0) return SOURCE_MODE_BOTH;
    if (idx == 1) return SOURCE_MODE_INTERNAL;
    return SOURCE_MODE_EXTERNAL;
}

static int source_is_allowed(const midi_inject_test_instance_t *inst, int source)
{
    if (!inst) return 0;

    switch (inst->source_mode) {
        case SOURCE_MODE_INTERNAL:
            return source == MOVE_MIDI_SOURCE_INTERNAL;
        case SOURCE_MODE_EXTERNAL:
            return source == MOVE_MIDI_SOURCE_EXTERNAL;
        case SOURCE_MODE_BOTH:
        default:
            return (source == MOVE_MIDI_SOURCE_INTERNAL ||
                    source == MOVE_MIDI_SOURCE_EXTERNAL);
    }
}

static void recalc_timing(midi_inject_test_instance_t *inst)
{
    if (!inst) return;

    inst->bpm = clamp_int(inst->bpm, MIN_BPM, MAX_BPM);
    inst->gate_percent = clamp_int(inst->gate_percent, MIN_GATE_PERCENT, MAX_GATE_PERCENT);

    int samples = (MOVE_SAMPLE_RATE * 60) / (inst->bpm * 4); /* 16th notes */
    if (samples < 1) samples = 1;
    inst->step_samples = samples;

    int gate = (samples * inst->gate_percent) / 100;
    if (samples <= 1) {
        gate = 1;
    } else {
        if (gate < 1) gate = 1;
        if (gate >= samples) gate = samples - 1;
    }
    inst->gate_samples = gate;

    if (inst->samples_to_next_step > inst->step_samples) {
        inst->samples_to_next_step = inst->step_samples;
    }
    if (inst->samples_to_note_off > inst->gate_samples) {
        inst->samples_to_note_off = inst->gate_samples;
    }
}

static uint8_t apply_output_channel(const midi_inject_test_instance_t *inst, uint8_t status)
{
    if (!inst) return status;
    if (inst->output_channel < 0 || status >= 0xF0) return status;
    return (uint8_t)((status & 0xF0) | (inst->output_channel & 0x0F));
}

static void remember_emitted_note_event(midi_inject_test_instance_t *inst,
                                        uint8_t status, uint8_t d1, uint8_t d2)
{
    if (!inst) return;
    uint8_t type = (uint8_t)(status & 0xF0);
    if (type != 0x90 && type != 0x80) return;

    uint32_t slot = inst->echo_ring_write % SELF_ECHO_RING_SIZE;
    inst->echo_ring_write++;
    inst->echo_ring[slot].timestamp_ms = now_mono_ms();
    inst->echo_ring[slot].status = status;
    inst->echo_ring[slot].d1 = d1;
    inst->echo_ring[slot].d2 = d2;
}

static int is_recent_self_echo(const midi_inject_test_instance_t *inst,
                               uint8_t status, uint8_t d1, uint8_t d2)
{
    if (!inst) return 0;
    (void)d2;
    uint8_t type = (uint8_t)(status & 0xF0);
    uint8_t ch = (uint8_t)(status & 0x0F);
    uint64_t now = now_mono_ms();
    for (uint32_t i = 0; i < SELF_ECHO_RING_SIZE; i++) {
        const self_echo_event_t *ev = &inst->echo_ring[i];
        if (ev->timestamp_ms == 0) continue;
        if ((uint8_t)(ev->status & 0xF0) != type) continue;
        if ((uint8_t)(ev->status & 0x0F) != ch) continue;
        if (ev->d1 != d1) continue;
        if ((now - ev->timestamp_ms) <= SELF_ECHO_WINDOW_MS) return 1;
    }
    return 0;
}

static int send_usb_packet(midi_inject_test_instance_t *inst,
                           uint8_t cin, uint8_t status, uint8_t d1, uint8_t d2)
{
    if (!inst) return 0;

    uint8_t final_status = apply_output_channel(inst, status);
    if (shadow_midi_to_move_send_usb_packet(cin, final_status, d1, d2)) {
        inst->emitted_packets++;
        remember_emitted_note_event(inst, final_status, d1, d2);
        if (should_log_sample(inst->emitted_packets, 0x3Fu)) {
            midi_inject_logf("tx usb0=0x%02X status=0x%02X d1=%u d2=%u step=%u emitted=%u",
                             cin, final_status, d1, d2, inst->step_index, inst->emitted_packets);
        }
        return 1;
    }

    inst->dropped_packets++;
    inst->dropped_queue_full++;
    if (should_log_sample(inst->dropped_queue_full, 0x1Fu)) {
        midi_inject_logf("drop reason=queue-full status=0x%02X d1=%u d2=%u drop_queue=%u",
                         final_status, d1, d2, inst->dropped_queue_full);
    }
    return 0;
}

static void emit_note_offs(midi_inject_test_instance_t *inst)
{
    if (!inst || inst->sounding_count == 0) return;

    uint8_t status = (uint8_t)(0x80 | (inst->trigger_channel & 0x0F));
    for (uint8_t i = 0; i < inst->sounding_count; i++) {
        send_usb_packet(inst, 0x08, status, inst->sounding_notes[i], 0);
    }

    inst->sounding_count = 0;
    inst->samples_to_note_off = -1;
}

static void choose_next_trigger_note(midi_inject_test_instance_t *inst)
{
    if (!inst || inst->held_count == 0) return;

    for (int note = 0; note < 128; note++) {
        if (inst->held_notes[note]) {
            inst->trigger_note = (uint8_t)note;
            return;
        }
    }
}

static void stop_pattern(midi_inject_test_instance_t *inst, const char *reason)
{
    if (!inst || !inst->running) return;

    inst->running = 0;
    inst->stop_requested = 1;
    inst->step_index = 0;
    inst->samples_to_next_step = 0;
    inst->samples_to_note_off = 0;

    midi_inject_logf("pattern stopped reason=%s held=%u", reason ? reason : "unknown", inst->held_count);
}

static void start_pattern(midi_inject_test_instance_t *inst, uint8_t note, uint8_t channel)
{
    if (!inst) return;

    inst->trigger_note = note;
    inst->trigger_channel = channel;
    inst->step_index = 0;
    inst->samples_to_next_step = 0;  /* emit immediately on next render callback */
    inst->samples_to_note_off = -1;
    inst->running = 1;

    midi_inject_logf("pattern started note=%u ch=%u mode=8x1,8x2,8x3",
                     inst->trigger_note, inst->trigger_channel + 1);
}

static void emit_pattern_step(midi_inject_test_instance_t *inst)
{
    if (!inst || !inst->running) return;

    uint8_t note0 = inst->trigger_note;
    uint8_t note1 = (note0 < 127u) ? (uint8_t)(note0 + 1u) : 127u;
    uint8_t note2 = (note0 < 126u) ? (uint8_t)(note0 + 2u) : 127u;
    uint8_t notes[3] = { note0, note1, note2 };
    uint8_t voice_count;

    if (inst->step_index < STAGE_STEPS) {
        voice_count = 1; /* first 8: single notes */
    } else if (inst->step_index < (STAGE_STEPS * 2u)) {
        voice_count = 2; /* next 8: two-note stack (+1 semitone) */
    } else {
        voice_count = 3; /* final 8: three-note stack (+1, +2 semitones) */
    }

    emit_note_offs(inst); /* enforce distinct note windows */

    uint8_t status = (uint8_t)(0x90 | (inst->trigger_channel & 0x0F));
    inst->sounding_count = voice_count;
    for (uint8_t i = 0; i < voice_count; i++) {
        inst->sounding_notes[i] = notes[i];
        send_usb_packet(inst, 0x09, status, inst->sounding_notes[i], (uint8_t)inst->velocity);
    }

    inst->pattern_steps++;
    if (should_log_sample(inst->pattern_steps, 0x0Fu)) {
        midi_inject_logf("step=%u voices=%u note=%u vel=%d",
                         inst->step_index, voice_count, inst->trigger_note, inst->velocity);
    }

    inst->step_index = (inst->step_index + 1u) % TOTAL_PATTERN_STEPS;
    inst->samples_to_next_step = inst->step_samples;
    inst->samples_to_note_off = inst->gate_samples;
}

static void* create_instance(const char *module_dir, const char *json_defaults)
{
    (void)module_dir;
    (void)json_defaults;

    midi_inject_log_open();
    if (g_instance_count == 0) {
        midi_inject_logf("---------- session start ----------");
    }
    g_instance_count++;

    int opened = shadow_midi_to_move_open();
    midi_inject_test_instance_t *inst = (midi_inject_test_instance_t *)calloc(1, sizeof(midi_inject_test_instance_t));
    if (!inst) {
        midi_inject_logf("instance create failed");
        return NULL;
    }

    inst->output_channel = -1;
    inst->bpm = DEFAULT_BPM;
    inst->velocity = DEFAULT_VELOCITY;
    inst->gate_percent = DEFAULT_GATE_PERCENT;
    inst->source_mode = SOURCE_MODE_EXTERNAL;
    inst->samples_to_note_off = -1;
    recalc_timing(inst);

    midi_inject_logf("instance created (queue=%s, source=%s, out=Thru, bpm=%d)",
                     opened ? "ok" : "fail",
                     source_mode_to_string(inst->source_mode),
                     inst->bpm);
    return inst;
}

static void destroy_instance(void *instance)
{
    midi_inject_test_instance_t *inst = (midi_inject_test_instance_t *)instance;
    if (inst) {
        if (inst->running) {
            stop_pattern(inst, "destroy");
        }
        if (inst->sounding_count > 0) {
            emit_note_offs(inst);
            inst->stop_requested = 0;
        }
        midi_inject_logf("instance destroyed recv=%u emitted=%u drop=%u source=%u system=%u non_note=%u aftertouch=%u note=%u knob_touch=%u self_echo=%u queue=%u pattern_steps=%u",
                         inst->received_packets,
                         inst->emitted_packets,
                         inst->dropped_packets,
                         inst->dropped_source,
                         inst->dropped_system,
                         inst->dropped_non_note,
                         inst->rx_aftertouch_events,
                         inst->rx_note_events,
                         inst->dropped_knob_touch,
                         inst->dropped_self_echo,
                         inst->dropped_queue_full,
                         inst->pattern_steps);
    }
    free(inst);

    if (g_instance_count > 0) g_instance_count--;
    if (g_instance_count == 0) {
        midi_inject_logf("---------- session end ----------");
        midi_inject_log_close();
    }
}

static void render_block(void *instance, int16_t *out_lr, int frames)
{
    midi_inject_test_instance_t *inst = (midi_inject_test_instance_t *)instance;
    memset(out_lr, 0, (size_t)frames * 2u * sizeof(int16_t));
    if (!inst) return;

    if (inst->stop_requested) {
        emit_note_offs(inst);
        inst->stop_requested = 0;
    }

    if (!inst->running) return;

    if (inst->step_samples <= 0 || inst->gate_samples <= 0) recalc_timing(inst);

    int remaining = frames;
    int guard = 0;
    while (remaining > 0 && inst->running && guard < 64) {
        guard++;

        int advance = remaining;
        if (inst->samples_to_next_step < advance) advance = inst->samples_to_next_step;
        if (inst->samples_to_note_off >= 0 && inst->samples_to_note_off < advance) {
            advance = inst->samples_to_note_off;
        }
        if (advance < 0) advance = 0;

        inst->samples_to_next_step -= advance;
        if (inst->samples_to_note_off >= 0) inst->samples_to_note_off -= advance;
        remaining -= advance;

        int handled = 0;
        if (inst->samples_to_note_off == 0) {
            emit_note_offs(inst);
            handled = 1;
        }
        if (inst->samples_to_next_step == 0 && inst->running) {
            emit_pattern_step(inst);
            handled = 1;
        }

        if (!handled && advance == 0) break;
    }
}

static void set_param(void *instance, const char *key, const char *val)
{
    midi_inject_test_instance_t *inst = (midi_inject_test_instance_t *)instance;
    if (!inst || !key || !val) return;

    if (strcmp(key, "out_channel") == 0 || strcmp(key, "preset") == 0) {
        int ch = atoi(val);
        if (ch <= 0) {
            inst->output_channel = -1;
        } else if (ch > 16) {
            inst->output_channel = 15;
        } else {
            inst->output_channel = ch - 1;
        }
        if (inst->output_channel < 0) {
            midi_inject_logf("output channel set to Thru");
        } else {
            midi_inject_logf("output channel set to Ch %d", inst->output_channel + 1);
        }
        return;
    }

    if (strcmp(key, "bpm") == 0) {
        inst->bpm = clamp_int(atoi(val), MIN_BPM, MAX_BPM);
        recalc_timing(inst);
        midi_inject_logf("bpm set to %d", inst->bpm);
        return;
    }

    if (strcmp(key, "velocity") == 0) {
        inst->velocity = clamp_int(atoi(val), MIN_VELOCITY, MAX_VELOCITY);
        midi_inject_logf("velocity set to %d", inst->velocity);
        return;
    }

    if (strcmp(key, "gate_percent") == 0) {
        inst->gate_percent = clamp_int(atoi(val), MIN_GATE_PERCENT, MAX_GATE_PERCENT);
        recalc_timing(inst);
        midi_inject_logf("gate set to %d%%", inst->gate_percent);
        return;
    }

    if (strcmp(key, "source_mode") == 0) {
        inst->source_mode = parse_source_mode(val);
        midi_inject_logf("source mode set to %s", source_mode_to_string(inst->source_mode));
        return;
    }
}

static int get_param(void *instance, const char *key, char *buf, int buf_len)
{
    midi_inject_test_instance_t *inst = (midi_inject_test_instance_t *)instance;
    if (!inst || !key || !buf || buf_len <= 1) return -1;

    if (strcmp(key, "ui_hierarchy") == 0) {
        return snprintf(buf, (size_t)buf_len, "%s", k_ui_hierarchy_json);
    }
    if (strcmp(key, "chain_params") == 0) {
        return snprintf(buf, (size_t)buf_len, "%s", k_chain_params_json);
    }

    if (strcmp(key, "received_packets") == 0) return snprintf(buf, (size_t)buf_len, "%u", inst->received_packets);
    if (strcmp(key, "emitted_packets") == 0) return snprintf(buf, (size_t)buf_len, "%u", inst->emitted_packets);
    if (strcmp(key, "dropped_packets") == 0) return snprintf(buf, (size_t)buf_len, "%u", inst->dropped_packets);
    if (strcmp(key, "dropped_source") == 0) return snprintf(buf, (size_t)buf_len, "%u", inst->dropped_source);
    if (strcmp(key, "dropped_system") == 0) return snprintf(buf, (size_t)buf_len, "%u", inst->dropped_system);
    if (strcmp(key, "dropped_knob_touch") == 0) return snprintf(buf, (size_t)buf_len, "%u", inst->dropped_knob_touch);
    if (strcmp(key, "dropped_self_echo") == 0) return snprintf(buf, (size_t)buf_len, "%u", inst->dropped_self_echo);
    if (strcmp(key, "dropped_queue_full") == 0) return snprintf(buf, (size_t)buf_len, "%u", inst->dropped_queue_full);
    if (strcmp(key, "pattern_steps") == 0) return snprintf(buf, (size_t)buf_len, "%u", inst->pattern_steps);
    if (strcmp(key, "last_status") == 0) return snprintf(buf, (size_t)buf_len, "%u", inst->last_status);
    if (strcmp(key, "last_source") == 0) return snprintf(buf, (size_t)buf_len, "%u", inst->last_source);

    if (strcmp(key, "out_channel") == 0) return snprintf(buf, (size_t)buf_len, "%d", inst->output_channel + 1);
    if (strcmp(key, "preset") == 0) return snprintf(buf, (size_t)buf_len, "%d", inst->output_channel + 1);
    if (strcmp(key, "bpm") == 0) return snprintf(buf, (size_t)buf_len, "%d", inst->bpm);
    if (strcmp(key, "velocity") == 0) return snprintf(buf, (size_t)buf_len, "%d", inst->velocity);
    if (strcmp(key, "gate_percent") == 0) return snprintf(buf, (size_t)buf_len, "%d", inst->gate_percent);
    if (strcmp(key, "source_mode") == 0) return snprintf(buf, (size_t)buf_len, "%s", source_mode_to_string(inst->source_mode));

    if (strcmp(key, "pattern_mode") == 0) return snprintf(buf, (size_t)buf_len, "8x1,8x2,8x3(+1,+2)");

    return -1;
}

static void on_midi(void *instance, const uint8_t *msg, int len, int source)
{
    midi_inject_test_instance_t *inst = (midi_inject_test_instance_t *)instance;
    if (!inst || !msg || len <= 0) return;

    inst->received_packets++;
    inst->last_status = msg[0];
    inst->last_source = (uint8_t)source;

    uint8_t status = msg[0];
    uint8_t d1 = (len > 1) ? msg[1] : 0;
    uint8_t d2 = (len > 2) ? msg[2] : 0;

    uint8_t type = (uint8_t)(status & 0xF0);

    if (!source_is_allowed(inst, source)) {
        inst->dropped_source++;
        if (should_log_sample(inst->dropped_source, 0x3Fu)) {
            midi_inject_logf("drop reason=source src=%d mode=%s status=0x%02X dropped_source=%u",
                             source,
                             source_mode_to_string(inst->source_mode),
                             status,
                             inst->dropped_source);
        }
        return;
    }

    if (status >= 0xF0) {
        inst->dropped_system++;
        if (should_log_sample(inst->dropped_system, 0x7Fu)) {
            midi_inject_logf("drop reason=system status=0x%02X dropped_system=%u",
                             status, inst->dropped_system);
        }
        return;
    }

    if (type != 0x90 && type != 0x80) {
        inst->dropped_non_note++;
        if (type == 0xA0) {
            inst->rx_aftertouch_events++;
            if (should_log_sample(inst->rx_aftertouch_events, 0xFFu)) {
                midi_inject_logf("rx summary aftertouch=%u note=%u held=%u running=%u recv_total=%u",
                                 inst->rx_aftertouch_events,
                                 inst->rx_note_events,
                                 inst->held_count,
                                 inst->running,
                                 inst->received_packets);
            }
        }
        return;
    }

    inst->rx_note_events++;
    if (should_log_sample(inst->rx_note_events, 0x3Fu)) {
        midi_inject_logf("rx note src=%d len=%d status=0x%02X d1=%u d2=%u held=%u running=%u note_total=%u recv_total=%u",
                         source, len, status, d1, d2, inst->held_count, inst->running,
                         inst->rx_note_events, inst->received_packets);
    }

    if (source == MOVE_MIDI_SOURCE_EXTERNAL &&
        is_recent_self_echo(inst, status, d1, d2)) {
        inst->dropped_self_echo++;
        if (should_log_sample(inst->dropped_self_echo, 0x3Fu)) {
            midi_inject_logf("drop reason=self-echo status=0x%02X d1=%u d2=%u dropped_self_echo=%u",
                             status, d1, d2, inst->dropped_self_echo);
        }
        return;
    }

    if (d1 < 10u) {
        inst->dropped_knob_touch++;
        if (should_log_sample(inst->dropped_knob_touch, 0x1Fu)) {
            midi_inject_logf("drop reason=knob-touch note=%u dropped_knob_touch=%u",
                             d1, inst->dropped_knob_touch);
        }
        return;
    }

    /* Normalize note-on velocity 0 to note-off semantics. */
    int note_on = (type == 0x90 && d2 > 0);
    int note_off = (type == 0x80) || (type == 0x90 && d2 == 0);

    if (note_on) {
        if (!inst->held_notes[d1]) {
            inst->held_notes[d1] = 1;
            if (inst->held_count < 127u) inst->held_count++;
        }

        if (!inst->running) {
            start_pattern(inst, d1, (uint8_t)(status & 0x0F));
        }
        return;
    }

    if (note_off) {
        if (inst->held_notes[d1]) {
            inst->held_notes[d1] = 0;
            if (inst->held_count > 0u) inst->held_count--;
        }

        if (inst->held_count == 0) {
            stop_pattern(inst, "all-notes-released");
            return;
        }

        if (inst->running && d1 == inst->trigger_note) {
            choose_next_trigger_note(inst);
            midi_inject_logf("trigger note switched to %u", inst->trigger_note);
        }
    }
}

static plugin_api_v2_t g_api = {
    .api_version = 2,
    .create_instance = create_instance,
    .destroy_instance = destroy_instance,
    .on_midi = on_midi,
    .set_param = set_param,
    .get_param = get_param,
    .render_block = render_block,
};

plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t *host)
{
    g_host = host;
    (void)g_host;  /* Module uses dedicated log file instead of unified debug.log. */
    midi_inject_log_open();
    midi_inject_logf("plugin initialized (v2, hold-gated pattern injector)");
    return &g_api;
}
