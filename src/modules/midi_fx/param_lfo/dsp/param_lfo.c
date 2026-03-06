/*
 * Param LFO MIDI FX
 *
 * Publishes control-rate modulation values into chain host's runtime
 * modulation bus without overwriting target base values.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "host/midi_fx_api_v1.h"
#include "host/plugin_api_v1.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Emit modulation at control-rate to reduce expensive per-block target writes. */
#define MOD_EMIT_EVERY_N_BLOCKS 2

typedef enum {
    WAVE_SINE = 0,
    WAVE_TRIANGLE,
    WAVE_SQUARE,
    WAVE_SAW_UP,
    WAVE_RANDOM,
    WAVE_DRUNK
} lfo_waveform_t;

typedef struct {
    lfo_waveform_t waveform;
    float phase;
    float phase_offset;
    float rate_hz;
    float depth;
    float offset;
    int bipolar;
    int enabled;
    int retrigger;
    char target_component[16];
    char target_param[32];
    char source_id[32];
    int modulation_active;
    uint8_t held_notes[128];
    int held_count;
    float random_hold_value;
    float drunk_start_value;
    float drunk_target_value;
    unsigned int block_tick_count;
} param_lfo_instance_t;

static const host_api_v1_t *g_host = NULL;

static float clampf(float value, float min_val, float max_val) {
    if (value < min_val) return min_val;
    if (value > max_val) return max_val;
    return value;
}

static float wrap_phase(float phase) {
    phase -= floorf(phase);
    if (phase < 0.0f) phase += 1.0f;
    return phase;
}

static float rand_bipolar(void) {
    float unit = (float)rand() / (float)RAND_MAX;
    return (unit * 2.0f) - 1.0f;
}

static int json_get_string(const char *json, const char *key, char *out, int out_len) {
    if (!json || !key || !out || out_len < 1) return 0;

    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *pos = strstr(json, search);
    if (!pos) return 0;

    const char *colon = strchr(pos, ':');
    if (!colon) return 0;
    while (*colon && (*colon == ':' || *colon == ' ' || *colon == '\t')) colon++;
    if (*colon != '"') return 0;
    colon++;

    const char *end = strchr(colon, '"');
    if (!end) return 0;

    int len = (int)(end - colon);
    if (len >= out_len) len = out_len - 1;
    strncpy(out, colon, len);
    out[len] = '\0';
    return 1;
}

static int json_get_float(const char *json, const char *key, float *out) {
    if (!json || !key || !out) return 0;

    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *pos = strstr(json, search);
    if (!pos) return 0;

    const char *colon = strchr(pos, ':');
    if (!colon) return 0;
    colon++;
    while (*colon && (*colon == ' ' || *colon == '\t')) colon++;

    char *endptr = NULL;
    float parsed = strtof(colon, &endptr);
    if (endptr == colon) return 0;

    *out = parsed;
    return 1;
}

static int parse_toggle(const char *val, int fallback) {
    if (!val || !val[0]) return fallback;

    if (strcmp(val, "on") == 0 || strcmp(val, "true") == 0 || strcmp(val, "1") == 0) {
        return 1;
    }
    if (strcmp(val, "off") == 0 || strcmp(val, "false") == 0 || strcmp(val, "0") == 0) {
        return 0;
    }
    return fallback;
}

static lfo_waveform_t parse_waveform(const char *val, lfo_waveform_t fallback) {
    if (!val || !val[0]) return fallback;

    if (strcmp(val, "sine") == 0 || strcmp(val, "0") == 0) return WAVE_SINE;
    if (strcmp(val, "triangle") == 0 || strcmp(val, "1") == 0) return WAVE_TRIANGLE;
    if (strcmp(val, "square") == 0 || strcmp(val, "2") == 0) return WAVE_SQUARE;
    if (strcmp(val, "saw_up") == 0 || strcmp(val, "3") == 0 || strcmp(val, "saw") == 0) return WAVE_SAW_UP;
    if (strcmp(val, "random") == 0 || strcmp(val, "4") == 0 || strcmp(val, "rand") == 0) return WAVE_RANDOM;
    if (strcmp(val, "drunk") == 0 || strcmp(val, "5") == 0) return WAVE_DRUNK;

    return fallback;
}

static const char *waveform_to_string(lfo_waveform_t waveform) {
    switch (waveform) {
        case WAVE_TRIANGLE: return "triangle";
        case WAVE_SQUARE: return "square";
        case WAVE_SAW_UP: return "saw_up";
        case WAVE_RANDOM: return "random";
        case WAVE_DRUNK: return "drunk";
        case WAVE_SINE:
        default:
            return "sine";
    }
}

static int parse_bipolar(const char *val, int fallback) {
    if (!val || !val[0]) return fallback;

    if (strcmp(val, "bipolar") == 0 || strcmp(val, "0") == 0) return 1;
    if (strcmp(val, "unipolar") == 0 || strcmp(val, "1") == 0) return 0;

    return fallback;
}

static const char *bipolar_to_string(int bipolar) {
    return bipolar ? "bipolar" : "unipolar";
}

static void reset_phase(param_lfo_instance_t *inst) {
    if (!inst) return;
    inst->phase = 0.0f;
}

static float waveform_rate_multiplier(const param_lfo_instance_t *inst) {
    if (!inst) return 1.0f;

    if (inst->waveform == WAVE_DRUNK) {
        /* Keep low rates familiar, but make full-rate drunk movement much faster. */
        const float norm = clampf(inst->rate_hz / 20.0f, 0.0f, 1.0f);
        return 1.0f + (3.0f * norm);
    }

    return 1.0f;
}

static float compute_lfo_sample(const param_lfo_instance_t *inst) {
    if (!inst) return 0.0f;

    const float phase = wrap_phase(inst->phase + inst->phase_offset);
    switch (inst->waveform) {
        case WAVE_TRIANGLE:
            return 1.0f - 4.0f * fabsf(phase - 0.5f);
        case WAVE_SQUARE:
            return (phase < 0.5f) ? 1.0f : -1.0f;
        case WAVE_SAW_UP:
            return (2.0f * phase) - 1.0f;
        case WAVE_RANDOM:
            return inst->random_hold_value;
        case WAVE_DRUNK:
            return inst->drunk_start_value + (inst->drunk_target_value - inst->drunk_start_value) * phase;
        case WAVE_SINE:
        default:
            return sinf(2.0f * (float)M_PI * phase);
    }
}

static void advance_wave_cycle_state(param_lfo_instance_t *inst) {
    if (!inst) return;

    if (inst->waveform == WAVE_RANDOM) {
        inst->random_hold_value = rand_bipolar();
    } else if (inst->waveform == WAVE_DRUNK) {
        inst->drunk_start_value = inst->drunk_target_value;
        inst->drunk_target_value = clampf(inst->drunk_target_value + rand_bipolar() * 0.5f, -1.0f, 1.0f);
    }
}

static void clear_modulation(param_lfo_instance_t *inst) {
    if (!inst) return;

    if (g_host && g_host->mod_clear_source) {
        g_host->mod_clear_source(g_host->mod_host_ctx, inst->source_id);
    }
    inst->modulation_active = 0;
}

static void clear_held_notes(param_lfo_instance_t *inst) {
    if (!inst) return;
    memset(inst->held_notes, 0, sizeof(inst->held_notes));
    inst->held_count = 0;
}

static void handle_note_on(param_lfo_instance_t *inst, uint8_t note) {
    if (!inst || note >= 128) return;
    if (inst->held_notes[note]) return;

    /* Retrigger only on fresh gate: no notes were previously held. */
    if (inst->retrigger && inst->held_count == 0) {
        reset_phase(inst);
    }

    inst->held_notes[note] = 1;
    inst->held_count++;
}

static void handle_note_off(param_lfo_instance_t *inst, uint8_t note) {
    if (!inst || note >= 128) return;
    if (!inst->held_notes[note]) return;

    inst->held_notes[note] = 0;
    inst->held_count--;
    if (inst->held_count < 0) inst->held_count = 0;
}

static int target_component_valid(const char *component) {
    if (!component || !component[0]) return 0;
    return strcmp(component, "synth") == 0 ||
           strcmp(component, "fx1") == 0 ||
           strcmp(component, "fx2") == 0 ||
           strcmp(component, "midi_fx1") == 0 ||
           strcmp(component, "midi_fx2") == 0;
}

static void* param_lfo_create_instance(const char *module_dir, const char *config_json) {
    (void)module_dir;
    (void)config_json;

    param_lfo_instance_t *inst = calloc(1, sizeof(param_lfo_instance_t));
    if (!inst) return NULL;

    inst->waveform = WAVE_SINE;
    inst->phase = 0.0f;
    inst->phase_offset = 0.0f;
    inst->rate_hz = 1.0f;
    inst->depth = 0.25f;
    inst->offset = 0.0f;
    inst->bipolar = 1;
    inst->enabled = 0;
    inst->retrigger = 0;
    inst->random_hold_value = rand_bipolar();
    inst->drunk_start_value = 0.0f;
    inst->drunk_target_value = rand_bipolar();
    inst->target_component[0] = '\0';
    inst->target_param[0] = '\0';
    snprintf(inst->source_id, sizeof(inst->source_id), "param_lfo_%p", (void *)inst);

    return inst;
}

static void param_lfo_destroy_instance(void *instance) {
    param_lfo_instance_t *inst = (param_lfo_instance_t *)instance;
    if (!inst) return;

    clear_modulation(inst);
    free(inst);
}

static int param_lfo_process_midi(void *instance,
                                  const uint8_t *in_msg, int in_len,
                                  uint8_t out_msgs[][3], int out_lens[],
                                  int max_out) {
    param_lfo_instance_t *inst = (param_lfo_instance_t *)instance;

    if (!in_msg || in_len < 1 || max_out < 1) return 0;

    if (inst) {
        const uint8_t status = in_msg[0];
        const uint8_t type = status & 0xF0;

        /* Transport Start/Stop reset phase for deterministic sync restarts. */
        if (status == 0xFA || status == 0xFC) {
            reset_phase(inst);
            if (status == 0xFC) {
                clear_held_notes(inst);
            }
        }

        /* Track held-note gate for retrigger behavior. */
        if (in_len >= 3) {
            if (type == 0x90 && in_msg[2] > 0) {
                handle_note_on(inst, in_msg[1]);
            } else if (type == 0x80 || (type == 0x90 && in_msg[2] == 0)) {
                handle_note_off(inst, in_msg[1]);
            }
        }
    }

    out_msgs[0][0] = in_msg[0];
    out_msgs[0][1] = in_len > 1 ? in_msg[1] : 0;
    out_msgs[0][2] = in_len > 2 ? in_msg[2] : 0;
    out_lens[0] = in_len;
    return 1;
}

static int param_lfo_tick(void *instance,
                          int frames, int sample_rate,
                          uint8_t out_msgs[][3], int out_lens[],
                          int max_out) {
    (void)out_msgs;
    (void)out_lens;
    (void)max_out;

    param_lfo_instance_t *inst = (param_lfo_instance_t *)instance;
    if (!inst || frames <= 0 || sample_rate <= 0) return 0;

    if (!g_host || !g_host->mod_emit_value) return 0;

    if (!inst->enabled || !target_component_valid(inst->target_component) || !inst->target_param[0]) {
        if (inst->modulation_active) {
            clear_modulation(inst);
        }
        return 0;
    }

    float phase_inc = (inst->rate_hz * waveform_rate_multiplier(inst)) *
                      ((float)frames / (float)sample_rate);
    float new_phase = inst->phase + phase_inc;
    int wraps = (int)floorf(new_phase);
    inst->phase = wrap_phase(new_phase);
    for (int i = 0; i < wraps; i++) {
        advance_wave_cycle_state(inst);
    }

    /* Throttle modulation writes while keeping phase progression full-rate. */
    if ((inst->block_tick_count % MOD_EMIT_EVERY_N_BLOCKS) != 0) {
        inst->block_tick_count++;
        return 0;
    }
    inst->block_tick_count++;

    float sample = compute_lfo_sample(inst);

    int rc = g_host->mod_emit_value(g_host->mod_host_ctx,
                                    inst->source_id,
                                    inst->target_component,
                                    inst->target_param,
                                    sample,
                                    inst->depth,
                                    inst->offset,
                                    inst->bipolar,
                                    1);
    if (rc == 0) {
        inst->modulation_active = 1;
    }

    return 0;
}

static void param_lfo_set_param(void *instance, const char *key, const char *val) {
    param_lfo_instance_t *inst = (param_lfo_instance_t *)instance;
    if (!inst || !key || !val) return;

    if (strcmp(key, "waveform") == 0) {
        inst->waveform = parse_waveform(val, inst->waveform);
    }
    else if (strcmp(key, "rate_hz") == 0) {
        inst->rate_hz = clampf((float)atof(val), 0.01f, 20.0f);
    }
    else if (strcmp(key, "phase") == 0) {
        inst->phase_offset = clampf((float)atof(val), 0.0f, 1.0f);
    }
    else if (strcmp(key, "depth") == 0) {
        inst->depth = clampf((float)atof(val), 0.0f, 1.0f);
    }
    else if (strcmp(key, "offset") == 0) {
        inst->offset = clampf((float)atof(val), -1.0f, 1.0f);
    }
    else if (strcmp(key, "polarity") == 0) {
        inst->bipolar = parse_bipolar(val, inst->bipolar);
    }
    else if (strcmp(key, "retrigger") == 0) {
        inst->retrigger = parse_toggle(val, inst->retrigger);
    }
    else if (strcmp(key, "enable") == 0) {
        int new_enabled = parse_toggle(val, inst->enabled);
        if (!new_enabled && inst->modulation_active) {
            clear_modulation(inst);
        }
        inst->enabled = new_enabled;
    }
    else if (strcmp(key, "target_component") == 0) {
        if (inst->modulation_active) {
            clear_modulation(inst);
        }
        strncpy(inst->target_component, val, sizeof(inst->target_component) - 1);
        inst->target_component[sizeof(inst->target_component) - 1] = '\0';
    }
    else if (strcmp(key, "target_param") == 0) {
        if (inst->modulation_active) {
            clear_modulation(inst);
        }
        strncpy(inst->target_param, val, sizeof(inst->target_param) - 1);
        inst->target_param[sizeof(inst->target_param) - 1] = '\0';
    }
    else if (strcmp(key, "state") == 0) {
        float f = 0.0f;
        char text[64];

        if (json_get_string(val, "waveform", text, sizeof(text))) {
            inst->waveform = parse_waveform(text, inst->waveform);
        }
        if (json_get_float(val, "rate_hz", &f)) {
            inst->rate_hz = clampf(f, 0.01f, 20.0f);
        }
        if (json_get_float(val, "phase", &f)) {
            inst->phase_offset = clampf(f, 0.0f, 1.0f);
        }
        if (json_get_float(val, "depth", &f)) {
            inst->depth = clampf(f, 0.0f, 1.0f);
        }
        if (json_get_float(val, "offset", &f)) {
            inst->offset = clampf(f, -1.0f, 1.0f);
        }
        if (json_get_string(val, "polarity", text, sizeof(text))) {
            inst->bipolar = parse_bipolar(text, inst->bipolar);
        }
        if (json_get_string(val, "retrigger", text, sizeof(text))) {
            inst->retrigger = parse_toggle(text, inst->retrigger);
        }
        if (json_get_string(val, "enable", text, sizeof(text))) {
            int new_enabled = parse_toggle(text, inst->enabled);
            if (!new_enabled && inst->modulation_active) {
                clear_modulation(inst);
            }
            inst->enabled = new_enabled;
        }
        if (json_get_string(val, "target_component", text, sizeof(text))) {
            if (inst->modulation_active) {
                clear_modulation(inst);
            }
            strncpy(inst->target_component, text, sizeof(inst->target_component) - 1);
            inst->target_component[sizeof(inst->target_component) - 1] = '\0';
        }
        if (json_get_string(val, "target_param", text, sizeof(text))) {
            if (inst->modulation_active) {
                clear_modulation(inst);
            }
            strncpy(inst->target_param, text, sizeof(inst->target_param) - 1);
            inst->target_param[sizeof(inst->target_param) - 1] = '\0';
        }
    }
}

static int param_lfo_get_param(void *instance, const char *key, char *buf, int buf_len) {
    param_lfo_instance_t *inst = (param_lfo_instance_t *)instance;
    if (!inst || !key || !buf || buf_len < 1) return -1;

    if (strcmp(key, "waveform") == 0) {
        return snprintf(buf, buf_len, "%s", waveform_to_string(inst->waveform));
    }
    if (strcmp(key, "rate_hz") == 0) {
        return snprintf(buf, buf_len, "%.4f", inst->rate_hz);
    }
    if (strcmp(key, "phase") == 0) {
        return snprintf(buf, buf_len, "%.4f", inst->phase_offset);
    }
    if (strcmp(key, "depth") == 0) {
        return snprintf(buf, buf_len, "%.4f", inst->depth);
    }
    if (strcmp(key, "offset") == 0) {
        return snprintf(buf, buf_len, "%.4f", inst->offset);
    }
    if (strcmp(key, "polarity") == 0) {
        return snprintf(buf, buf_len, "%s", bipolar_to_string(inst->bipolar));
    }
    if (strcmp(key, "retrigger") == 0) {
        return snprintf(buf, buf_len, "%s", inst->retrigger ? "on" : "off");
    }
    if (strcmp(key, "enable") == 0) {
        return snprintf(buf, buf_len, "%s", inst->enabled ? "on" : "off");
    }
    if (strcmp(key, "target_component") == 0) {
        return snprintf(buf, buf_len, "%s", inst->target_component);
    }
    if (strcmp(key, "target_param") == 0) {
        return snprintf(buf, buf_len, "%s", inst->target_param);
    }
    if (strcmp(key, "state") == 0) {
        return snprintf(buf,
                        buf_len,
                        "{\"waveform\":\"%s\",\"rate_hz\":%.6f,\"phase\":%.6f,\"depth\":%.6f,\"offset\":%.6f,\"polarity\":\"%s\",\"retrigger\":\"%s\",\"enable\":\"%s\",\"target_component\":\"%s\",\"target_param\":\"%s\"}",
                        waveform_to_string(inst->waveform),
                        inst->rate_hz,
                        inst->phase_offset,
                        inst->depth,
                        inst->offset,
                        bipolar_to_string(inst->bipolar),
                        inst->retrigger ? "on" : "off",
                        inst->enabled ? "on" : "off",
                        inst->target_component,
                        inst->target_param);
    }
    if (strcmp(key, "chain_params") == 0) {
        const char *params = "["
            "{\"key\":\"waveform\",\"name\":\"Wave\",\"type\":\"enum\",\"options\":[\"sine\",\"triangle\",\"square\",\"saw_up\",\"random\",\"drunk\"],\"default\":0},"
            "{\"key\":\"rate_hz\",\"name\":\"Rate\",\"type\":\"float\",\"min\":0.01,\"max\":20.0,\"default\":1.0,\"step\":0.01},"
            "{\"key\":\"phase\",\"name\":\"Phase\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"default\":0.0,\"step\":0.01},"
            "{\"key\":\"depth\",\"name\":\"Depth\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"default\":0.25,\"step\":0.01},"
            "{\"key\":\"offset\",\"name\":\"Offset\",\"type\":\"float\",\"min\":-1.0,\"max\":1.0,\"default\":0.0,\"step\":0.01},"
            "{\"key\":\"polarity\",\"name\":\"Polarity\",\"type\":\"enum\",\"options\":[\"bipolar\",\"unipolar\"],\"default\":0},"
            "{\"key\":\"retrigger\",\"name\":\"Retrigger\",\"type\":\"enum\",\"options\":[\"off\",\"on\"],\"default\":0},"
            "{\"key\":\"enable\",\"name\":\"Enable\",\"type\":\"enum\",\"options\":[\"off\",\"on\"],\"default\":0},"
            "{\"key\":\"target_component\",\"name\":\"Target Comp\",\"type\":\"enum\",\"options\":[\"synth\",\"fx1\",\"fx2\",\"midi_fx1\",\"midi_fx2\"],\"default\":0},"
            "{\"key\":\"target_param\",\"name\":\"Target Param\",\"type\":\"filepath\",\"default\":\"\"}"
        "]";
        return snprintf(buf, buf_len, "%s", params);
    }

    return -1;
}

static midi_fx_api_v1_t g_api = {
    .api_version = MIDI_FX_API_VERSION,
    .create_instance = param_lfo_create_instance,
    .destroy_instance = param_lfo_destroy_instance,
    .process_midi = param_lfo_process_midi,
    .tick = param_lfo_tick,
    .set_param = param_lfo_set_param,
    .get_param = param_lfo_get_param
};

midi_fx_api_v1_t* move_midi_fx_init(const host_api_v1_t *host) {
    g_host = host;
    return &g_api;
}
