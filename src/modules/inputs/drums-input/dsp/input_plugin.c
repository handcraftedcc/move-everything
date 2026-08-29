#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "host/input_module_api_v1.h"

#define PAD_FIRST 68
#define PAD_LAST 99
#define PAD_COUNT 32
#define PAD_COLS 8
#define LEFT_COLS 4
#define COLOR_OFF 0
#define COLOR_WHITE 120

typedef enum drums_mode_t {
    MODE_32_DRUMS = 0,
    MODE_16_VELOCITIES = 1,
    MODE_16_RATCHETS = 2
} drums_mode_t;

typedef struct drums_input_t {
    drums_mode_t mode;
    int base_note;
    int fixed_velocity;
    int last_note;
    int ratchet_active;
    int ratchet_pad_idx;
    int ratchet_right_idx;
    int ratchet_note;
    int ratchet_velocity;
    int ratchet_channel;
    uint64_t ratchet_next_cycle_us;
    uint8_t held_note[PAD_COUNT];
    uint8_t held_channel[PAD_COUNT];
    uint8_t held[PAD_COUNT];
} drums_input_t;

static const host_input_api_v1_t *g_host = 0;

static const int ratchet_rates[4] = {6, 8, 12, 16};
static const int ratchet_pattern_lengths[4] = {4, 4, 4, 6};
static const int ratchet_patterns[4][6] = {
    {1, 1, 1, 1, 0, 0},
    {1, 1, 1, 0, 0, 0},
    {1, 1, 0, 0, 0, 0},
    {1, 1, 1, 1, 0, 0}
};

static int clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static uint64_t monotonic_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000ULL);
}

static int pad_index(int pad_note)
{
    return pad_note - PAD_FIRST;
}

static int pad_row(int idx)
{
    return idx / PAD_COLS;
}

static int pad_col(int idx)
{
    return idx % PAD_COLS;
}

static int left16_note(const drums_input_t *inst, int row, int col)
{
    return inst->base_note + row * LEFT_COLS + col;
}

static int pad_to_32_note(const drums_input_t *inst, int idx)
{
    return inst->base_note + idx;
}

static int is_left_pad(int idx)
{
    return pad_col(idx) < LEFT_COLS;
}

static int right_pad_index(int idx)
{
    return pad_row(idx) * LEFT_COLS + (pad_col(idx) - LEFT_COLS);
}

static int velocity_for_pad(int idx)
{
    int v = (idx + 1) * 8;
    return v > 127 ? 127 : v;
}

static input_usb_midi_packet_t note_packet(int note, int velocity, int channel, int on)
{
    input_usb_midi_packet_t p;
    p.cable = 2;
    p.data1 = (uint8_t)clamp_int(note, 0, 127);
    p.data2 = (uint8_t)(on ? clamp_int(velocity, 1, 127) : 0);
    p.cin = on ? 0x09 : 0x08;
    p.status = (uint8_t)((on ? 0x90 : 0x80) | clamp_int(channel, 0, 15));
    return p;
}

static uint32_t note_gate_us(uint32_t step_us)
{
    uint32_t gate = step_us / 2;
    if (gate < 5000) gate = 5000;
    if (gate > 30000) gate = 30000;
    return gate;
}

static void schedule_note_off(int note, int channel, uint32_t delay_us)
{
    if (!g_host || !g_host->schedule_midi) return;
    input_usb_midi_packet_t off = note_packet(note, 0, channel, 0);
    g_host->schedule_midi(g_host->ctx, &off, 1, delay_us);
}

static uint32_t ratchet_step_us_for(const input_context_t *ctx, int right_idx)
{
    int col = right_idx % LEFT_COLS;
    double bpm = ctx->bpm;
    int transport_hint = ctx->playing;
    (void)transport_hint;
    if (bpm < 20.0 || bpm > 999.0) bpm = 120.0;
    return (uint32_t)((60000000.0 / bpm) / (double)ratchet_rates[col]);
}

static uint32_t ratchet_cycle_us_for(const input_context_t *ctx, int right_idx)
{
    int row = right_idx / LEFT_COLS;
    int steps = (row >= 0 && row < 4) ? ratchet_pattern_lengths[row] : 4;
    return ratchet_step_us_for(ctx, right_idx) * (uint32_t)steps;
}

static void schedule_ratchet_cycle(const drums_input_t *inst,
                                   const input_context_t *ctx,
                                   uint64_t cycle_start_us)
{
    if (!inst || !ctx || !g_host || !g_host->schedule_midi || !inst->ratchet_active) return;
    int row = inst->ratchet_right_idx / LEFT_COLS;
    uint32_t step_us = ratchet_step_us_for(ctx, inst->ratchet_right_idx);
    uint32_t gate_us = note_gate_us(step_us);
    uint64_t now = monotonic_us();
    int steps = (row >= 0 && row < 4) ? ratchet_pattern_lengths[row] : 4;
    for (int step = 0; step < steps; step++) {
        if (!ratchet_patterns[row][step]) continue;
        uint64_t on_at = cycle_start_us + step_us * (uint64_t)step;
        uint64_t off_at = on_at + gate_us;
        uint32_t on_delay = (on_at > now) ? (uint32_t)(on_at - now) : 0;
        uint32_t off_delay = (off_at > now) ? (uint32_t)(off_at - now) : 0;
        input_usb_midi_packet_t on = note_packet(inst->ratchet_note, inst->ratchet_velocity, inst->ratchet_channel, 1);
        input_usb_midi_packet_t off = note_packet(inst->ratchet_note, 0, inst->ratchet_channel, 0);
        g_host->schedule_midi(g_host->ctx, &on, 1, on_delay);
        g_host->schedule_midi(g_host->ctx, &off, 1, off_delay);
    }
}

static void cancel_ratchet(drums_input_t *inst, input_process_result_t *out)
{
    if (!inst || !inst->ratchet_active) return;
    if (g_host && g_host->cancel_scheduled_midi) {
        g_host->cancel_scheduled_midi(g_host->ctx, inst->ratchet_note, inst->ratchet_channel);
    }
    input_usb_midi_packet_t off = note_packet(inst->ratchet_note, 0, inst->ratchet_channel, 0);
    if (out && out->output_count < INPUT_MODULE_MAX_OUTPUT_PACKETS) {
        out->outputs[out->output_count++] = off;
    } else if (g_host && g_host->schedule_midi) {
        g_host->schedule_midi(g_host->ctx, &off, 1, 0);
    }
    inst->ratchet_active = 0;
}

static void start_ratchet(drums_input_t *inst,
                          const input_context_t *ctx,
                          int pad_idx,
                          int right_idx,
                          int velocity,
                          int channel)
{
    cancel_ratchet(inst, NULL);
    inst->ratchet_active = 1;
    inst->ratchet_pad_idx = pad_idx;
    inst->ratchet_right_idx = right_idx;
    inst->ratchet_note = inst->last_note;
    inst->ratchet_velocity = velocity;
    inst->ratchet_channel = channel;
    uint64_t now = monotonic_us();
    inst->ratchet_next_cycle_us = now;
    schedule_ratchet_cycle(inst, ctx, inst->ratchet_next_cycle_us);
    inst->ratchet_next_cycle_us += ratchet_cycle_us_for(ctx, right_idx);
}

static void draw_leds(const drums_input_t *inst, const input_context_t *ctx)
{
    if (!inst || !ctx || !g_host || !g_host->set_pad_led) return;
    int track = clamp_int(ctx->active_track, 0, 3);
    int track_color = g_host->get_track_color ? g_host->get_track_color(g_host->ctx, track) : -1;
    uint8_t drum_color = (uint8_t)((track_color > 0) ? track_color : COLOR_WHITE);

    for (int idx = 0; idx < PAD_COUNT; idx++) {
        uint8_t color = COLOR_OFF;
        if (inst->mode == MODE_32_DRUMS) {
            color = drum_color;
        } else if (is_left_pad(idx)) {
            color = drum_color;
        } else {
            color = COLOR_WHITE;
        }
        g_host->set_pad_led(g_host->ctx, idx, color);
    }
}

static void *create_instance(const char *module_dir, const char *json_defaults)
{
    (void)module_dir;
    (void)json_defaults;
    drums_input_t *inst = (drums_input_t *)calloc(1, sizeof(drums_input_t));
    if (!inst) return 0;
    inst->mode = MODE_32_DRUMS;
    inst->base_note = 36;
    inst->fixed_velocity = 0;
    inst->last_note = 36;
    inst->ratchet_pad_idx = -1;
    return inst;
}

static void destroy_instance(void *instance)
{
    free(instance);
}

static void set_param(void *instance, const char *key, const char *value)
{
    drums_input_t *inst = (drums_input_t *)instance;
    if (!inst || !key) return;
    const char *val = value ? value : "";
    if (strcmp(key, "mode") == 0) {
        if (strcmp(val, "1") == 0 || strcmp(val, "16 Velocities") == 0) {
            inst->mode = MODE_16_VELOCITIES;
        } else if (strcmp(val, "2") == 0 || strcmp(val, "16 Ratchets") == 0) {
            inst->mode = MODE_16_RATCHETS;
        } else {
            inst->mode = MODE_32_DRUMS;
        }
    } else if (strcmp(key, "base_note") == 0) {
        inst->base_note = clamp_int(atoi(val), 0, 96);
        inst->last_note = inst->base_note;
    } else if (strcmp(key, "velocity") == 0) {
        inst->fixed_velocity = (strcmp(val, "Fixed 100") == 0 || strcmp(val, "fixed") == 0 || strcmp(val, "1") == 0);
    }
}

static int get_param(void *instance, const char *key, char *buf, int buf_len)
{
    drums_input_t *inst = (drums_input_t *)instance;
    if (!inst || !key || !buf || buf_len <= 0) return -1;
    if (strcmp(key, "mode") == 0) {
        const char *mode = "32 Drums";
        if (inst->mode == MODE_16_VELOCITIES) mode = "16 Velocities";
        if (inst->mode == MODE_16_RATCHETS) mode = "16 Ratchets";
        return snprintf(buf, buf_len, "%s", mode);
    }
    if (strcmp(key, "base_note") == 0) {
        return snprintf(buf, buf_len, "%d", inst->base_note);
    }
    if (strcmp(key, "velocity") == 0) {
        return snprintf(buf, buf_len, "%s", inst->fixed_velocity ? "Fixed 100" : "Pad");
    }
    return -1;
}

static int process_midi(void *instance,
                        const input_usb_midi_packet_t *in,
                        const input_context_t *ctx,
                        input_process_result_t *out)
{
    drums_input_t *inst = (drums_input_t *)instance;
    if (!inst || !in || !ctx || !out) return -1;
    memset(out, 0, sizeof(*out));

    if (in->cable != 0 || in->data1 < PAD_FIRST || in->data1 > PAD_LAST) return 0;
    uint8_t type = in->status & 0xF0;
    if (type != 0x80 && type != 0x90 && type != 0xA0) return 0;

    int idx = pad_index(in->data1);
    int row = pad_row(idx);
    int col = pad_col(idx);
    int channel = clamp_int(ctx->active_track, 0, 15);
    if (type == 0xA0) {
        if (inst->mode == MODE_16_RATCHETS &&
            inst->ratchet_active &&
            inst->ratchet_pad_idx == idx) {
            inst->ratchet_velocity = clamp_int(in->data2, 1, 127);
        }
        out->handled = 1;
        return 0;
    }
    int is_note_on = (type == 0x90 && in->data2 > 0);

    if (inst->mode != MODE_32_DRUMS && !is_left_pad(idx)) {
        if (is_note_on) {
            int right_idx = right_pad_index(idx);
            if (inst->mode == MODE_16_VELOCITIES) {
                int velocity = inst->fixed_velocity ? 100 : velocity_for_pad(right_idx);
                input_usb_midi_packet_t on = note_packet(inst->last_note, velocity, channel, 1);
                out->outputs[0] = on;
                out->output_count = 1;
                schedule_note_off(inst->last_note, channel, 30000);
            } else {
                int velocity = inst->fixed_velocity ? 100 : in->data2;
                start_ratchet(inst, ctx, idx, right_idx, velocity, channel);
            }
        } else if (inst->mode == MODE_16_RATCHETS && inst->ratchet_active && inst->ratchet_pad_idx == idx) {
            cancel_ratchet(inst, out);
        }
        out->handled = 1;
        return 0;
    }

    int note = (inst->mode == MODE_32_DRUMS) ? pad_to_32_note(inst, idx) : left16_note(inst, row, col);
    if (!is_note_on && !inst->held[idx]) {
        out->handled = 1;
        return 0;
    }
    if (!is_note_on) note = inst->held_note[idx];
    if (note < 0 || note > 127) {
        out->handled = 1;
        return 0;
    }

    input_usb_midi_packet_t *pkt = &out->outputs[0];
    if (is_note_on) {
        uint8_t velocity = inst->fixed_velocity ? 100 : in->data2;
        *pkt = note_packet(note, velocity, channel, 1);
        inst->last_note = note;
        inst->held_note[idx] = (uint8_t)note;
        inst->held_channel[idx] = (uint8_t)channel;
        inst->held[idx] = 1;
    } else {
        uint8_t off_channel = inst->held[idx] ? inst->held_channel[idx] : (uint8_t)channel;
        *pkt = note_packet(note, 0, off_channel, 0);
        inst->held[idx] = 0;
    }
    out->output_count = 1;
    out->handled = 1;
    return 0;
}

static void on_context_changed(void *instance, const input_context_t *ctx)
{
    drums_input_t *inst = (drums_input_t *)instance;
    draw_leds(inst, ctx);
}

static void on_all_notes_off(void *instance, const input_context_t *ctx)
{
    drums_input_t *inst = (drums_input_t *)instance;
    (void)ctx;
    if (!inst) return;
    cancel_ratchet(inst, NULL);
    memset(inst->held, 0, sizeof(inst->held));
}

static void on_tick(void *instance, const input_context_t *ctx)
{
    drums_input_t *inst = (drums_input_t *)instance;
    if (!inst || !ctx || !inst->ratchet_active) return;
    uint64_t now = monotonic_us();
    uint32_t lookahead_us = ratchet_cycle_us_for(ctx, inst->ratchet_right_idx);
    if (inst->ratchet_next_cycle_us <= now + lookahead_us) {
        schedule_ratchet_cycle(inst, ctx, inst->ratchet_next_cycle_us);
        inst->ratchet_next_cycle_us += ratchet_cycle_us_for(ctx, inst->ratchet_right_idx);
    }
}

static input_module_api_v1_t api = {
    .api_version = INPUT_MODULE_API_VERSION,
    .create_instance = create_instance,
    .destroy_instance = destroy_instance,
    .set_param = set_param,
    .get_param = get_param,
    .process_midi = process_midi,
    .on_context_changed = on_context_changed,
    .on_all_notes_off = on_all_notes_off,
    .on_tick = on_tick
};

input_module_api_v1_t *schwung_input_module_init_v1(const host_input_api_v1_t *host)
{
    g_host = host;
    return &api;
}
