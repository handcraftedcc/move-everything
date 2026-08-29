#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host/input_module_api_v1.h"

#define PAD_FIRST 68
#define PAD_LAST 99
#define PAD_COUNT 32
#define PAD_COLS 8
#define COLOR_OFF 0
#define COLOR_WHITE 120

typedef enum moveish_layout_t {
    LAYOUT_CHROMATIC = 0,
    LAYOUT_IN_KEY_OCTAVES = 1,
    LAYOUT_IN_KEY_4THS = 2
} moveish_layout_t;

typedef struct moveish_t {
    moveish_layout_t layout;
    int offset;
    int fixed_velocity;
    uint8_t held_note[PAD_COUNT];
    uint8_t held_channel[PAD_COUNT];
    uint8_t held[PAD_COUNT];
} moveish_t;

static const host_input_api_v1_t *g_host = 0;

static int clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int note_class(int note)
{
    int cls = note % 12;
    return cls < 0 ? cls + 12 : cls;
}

static int pos_mod(int v, int mod)
{
    int r = v % mod;
    return r < 0 ? r + mod : r;
}

static int floor_div(int v, int div)
{
    int q = v / div;
    int r = v % div;
    if (r < 0) q--;
    return q;
}

static void lower_compact(const char *src, char *dst, int dst_len)
{
    int j = 0;
    for (int i = 0; src && src[i] && j < dst_len - 1; i++) {
        char c = (char)src[i];
        if (c == ' ' || c == '-' || c == '_' || c == '.') continue;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        dst[j++] = c;
    }
    dst[j] = '\0';
}

static unsigned int mask_from_intervals(const int *intervals, int count)
{
    unsigned int mask = 0;
    for (int i = 0; i < count; i++) mask |= (1u << intervals[i]);
    return mask;
}

static const int *scale_definition(const char *scale_name, int *count)
{
    static const int chromatic[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    static const int major[] = {0, 2, 4, 5, 7, 9, 11};
    static const int natural_minor[] = {0, 2, 3, 5, 7, 8, 10};
    static const int harmonic_minor[] = {0, 2, 3, 5, 7, 8, 11};
    static const int harmonic_major[] = {0, 2, 4, 5, 7, 8, 11};
    static const int melodic_minor[] = {0, 2, 3, 5, 7, 9, 11};
    static const int dorian[] = {0, 2, 3, 5, 7, 9, 10};
    static const int dorian_sharp4[] = {0, 2, 3, 6, 7, 9, 10};
    static const int phrygian[] = {0, 1, 3, 5, 7, 8, 10};
    static const int phrygian_dominant[] = {0, 1, 4, 5, 7, 8, 10};
    static const int lydian[] = {0, 2, 4, 6, 7, 9, 11};
    static const int lydian_augmented[] = {0, 2, 4, 6, 8, 9, 11};
    static const int lydian_dominant[] = {0, 2, 4, 6, 7, 9, 10};
    static const int mixolydian[] = {0, 2, 4, 5, 7, 9, 10};
    static const int locrian[] = {0, 1, 3, 5, 6, 8, 10};
    static const int super_locrian[] = {0, 1, 3, 4, 6, 8, 10};
    static const int major_pentatonic[] = {0, 2, 4, 7, 9};
    static const int minor_pentatonic[] = {0, 3, 5, 7, 10};
    static const int blues[] = {0, 3, 5, 6, 7, 10};
    static const int whole_tone[] = {0, 2, 4, 6, 8, 10};
    static const int whole_half_dim[] = {0, 2, 3, 5, 6, 8, 9, 11};
    static const int half_whole_dim[] = {0, 1, 3, 4, 6, 7, 9, 10};
    static const int eight_tone_spanish[] = {0, 1, 3, 4, 5, 6, 8, 10};
    static const int bhairav[] = {0, 1, 4, 5, 7, 8, 11};
    static const int hirajoshi[] = {0, 2, 3, 7, 8};
    static const int in_sen[] = {0, 1, 5, 7, 10};
    static const int iwato[] = {0, 1, 5, 6, 10};
    static const int kumoi[] = {0, 2, 3, 7, 9};
    static const int pelog_selisir[] = {0, 1, 3, 7, 8};
    static const int pelog_tembung[] = {0, 1, 5, 7, 8};
    static const int hungarian_minor[] = {0, 2, 3, 6, 7, 8, 11};
    static const int messiaen3[] = {0, 2, 3, 4, 6, 7, 8, 10, 11};
    static const int messiaen4[] = {0, 1, 2, 5, 6, 7, 8, 11};
    static const int messiaen5[] = {0, 1, 5, 6, 7, 11};
    static const int messiaen6[] = {0, 2, 4, 5, 6, 8, 10, 11};
    static const int messiaen7[] = {0, 1, 2, 3, 5, 6, 7, 8, 9, 11};

    static const char *captured_move_menu_names =
        "Major, Minor, Dorian, Mixolydian, Lydian, Phrygian, Locrian, "
        "Whole Tone, Half-whole Dim., Whole-half Dim., Minor Blues, "
        "Minor Pentatonic, Major Pentatonic, Harmonic Minor, Harmonic Major, "
        "Dorian #4, Phrygian Dominant, Melodic Minor, Lydian Augmented, "
        "Lydian Dominant, Super Locrian, 8-Tone Spanish, Bhairav, "
        "Hungarian Minor, Hirajoshi, In-Sen, Iwato, Kumoi, Pelog Selisir, "
        "Pelog Tembung, Messiaen 3, Messiaen 4, Messiaen 5, Messiaen 6, "
        "Messiaen 7, C♯/D♭";
    (void)captured_move_menu_names;

    char name[64];
    lower_compact(scale_name, name, sizeof(name));
#define RETURN_SCALE(arr) do { *count = (int)(sizeof(arr) / sizeof((arr)[0])); return (arr); } while (0)
    if (!name[0] || strstr(name, "chromatic")) RETURN_SCALE(chromatic);
    if (strstr(name, "majorpentatonic")) RETURN_SCALE(major_pentatonic);
    if (strstr(name, "minorpentatonic")) RETURN_SCALE(minor_pentatonic);
    if (strstr(name, "harmonicmajor")) RETURN_SCALE(harmonic_major);
    if (strstr(name, "harmonicminor")) RETURN_SCALE(harmonic_minor);
    if (strstr(name, "melodicminor")) RETURN_SCALE(melodic_minor);
    if (strstr(name, "hungarianminor")) RETURN_SCALE(hungarian_minor);
    if (strstr(name, "dorian#4")) RETURN_SCALE(dorian_sharp4);
    if (strstr(name, "phrygiandominant")) RETURN_SCALE(phrygian_dominant);
    if (strstr(name, "lydianaugmented")) RETURN_SCALE(lydian_augmented);
    if (strstr(name, "lydiandominant")) RETURN_SCALE(lydian_dominant);
    if (strstr(name, "superlocrian")) RETURN_SCALE(super_locrian);
    if (strstr(name, "8tonespanish")) RETURN_SCALE(eight_tone_spanish);
    if (strstr(name, "bhairav")) RETURN_SCALE(bhairav);
    if (strstr(name, "minorblues")) RETURN_SCALE(blues);
    if (strstr(name, "blues")) RETURN_SCALE(blues);
    if (strstr(name, "naturalminor") || strstr(name, "minor")) RETURN_SCALE(natural_minor);
    if (strstr(name, "dorian")) RETURN_SCALE(dorian);
    if (strstr(name, "phrygian")) RETURN_SCALE(phrygian);
    if (strstr(name, "lydian")) RETURN_SCALE(lydian);
    if (strstr(name, "mixolydian")) RETURN_SCALE(mixolydian);
    if (strstr(name, "locrian")) RETURN_SCALE(locrian);
    if (strstr(name, "wholetone")) RETURN_SCALE(whole_tone);
    if (strstr(name, "wholehalfdim")) RETURN_SCALE(whole_half_dim);
    if (strstr(name, "halfwholedim")) RETURN_SCALE(half_whole_dim);
    if (strstr(name, "diminished")) RETURN_SCALE(whole_half_dim);
    if (strstr(name, "hirajoshi")) RETURN_SCALE(hirajoshi);
    if (strstr(name, "insen")) RETURN_SCALE(in_sen);
    if (strstr(name, "iwato")) RETURN_SCALE(iwato);
    if (strstr(name, "kumoi")) RETURN_SCALE(kumoi);
    if (strstr(name, "pelogselisir")) RETURN_SCALE(pelog_selisir);
    if (strstr(name, "pelogtembung")) RETURN_SCALE(pelog_tembung);
    if (strstr(name, "messiaen3")) RETURN_SCALE(messiaen3);
    if (strstr(name, "messiaen4")) RETURN_SCALE(messiaen4);
    if (strstr(name, "messiaen5")) RETURN_SCALE(messiaen5);
    if (strstr(name, "messiaen6")) RETURN_SCALE(messiaen6);
    if (strstr(name, "messiaen7")) RETURN_SCALE(messiaen7);
    RETURN_SCALE(major);
#undef RETURN_SCALE
}

static unsigned int scale_mask(const char *scale_name)
{
    int scale_len = 0;
    const int *intervals = scale_definition(scale_name, &scale_len);
    return mask_from_intervals(intervals, scale_len);
}

static int is_in_scale(int note_cls, int root_cls, const char *scale_name)
{
    if (root_cls < 0 || root_cls > 11) return 1;
    int interval = (note_cls - root_cls + 12) % 12;
    return (scale_mask(scale_name) & (1u << interval)) != 0;
}

static int note_from_scale_step(int root_note, const int *intervals, int scale_len, int scale_step)
{
    int octave = floor_div(scale_step, scale_len);
    int degree = pos_mod(scale_step, scale_len);
    return root_note + octave * 12 + intervals[degree];
}

static int root_base_note(const input_context_t *ctx)
{
    int root_cls = (ctx->root_note_class >= 0 && ctx->root_note_class <= 11) ? ctx->root_note_class : 0;
    return 48 + ((ctx->octave_index - 2) * 12) + root_cls;
}

static int pad_to_note(const moveish_t *inst, int pad_note, const input_context_t *ctx)
{
    int idx = pad_note - PAD_FIRST;
    int row = idx / PAD_COLS;
    int col = idx % PAD_COLS;
    int root_note = root_base_note(ctx);

    if (inst->layout == LAYOUT_CHROMATIC) {
        int chromatic_step = row * 5 + col - 3 + inst->offset;
        return root_note + chromatic_step;
    }

    int scale_len = 0;
    const int *intervals = scale_definition(ctx->scale_name, &scale_len);
    int scale_step = 0;
    if (inst->layout == LAYOUT_IN_KEY_OCTAVES) {
        scale_step = row * scale_len + col + inst->offset;
    } else {
        scale_step = row * 3 + col + inst->offset;
    }
    return note_from_scale_step(root_note, intervals, scale_len, scale_step);
}

static int held_note_matches(const moveish_t *inst, int note)
{
    for (int i = 0; i < PAD_COUNT; i++) {
        if (inst->held[i] && inst->held_note[i] == note) return 1;
    }
    return 0;
}

static void draw_leds(const moveish_t *inst, const input_context_t *ctx)
{
    if (!inst || !ctx || !g_host || !g_host->set_pad_led) return;
    int track = clamp_int(ctx->active_track, 0, 3);
    int track_color = g_host->get_track_color ? g_host->get_track_color(g_host->ctx, track) : -1;
    uint8_t root_color = (uint8_t)((track_color > 0) ? track_color : COLOR_WHITE);

    for (int idx = 0; idx < PAD_COUNT; idx++) {
        int note = pad_to_note(inst, PAD_FIRST + idx, ctx);
        uint8_t color = COLOR_OFF;
        if (note >= 0 && note <= 127) {
            int cls = note_class(note);
            if (held_note_matches(inst, note)) {
                color = root_color;
            } else if (ctx->root_note_class >= 0 && cls == ctx->root_note_class) {
                color = root_color;
            } else if (inst->layout != LAYOUT_CHROMATIC || is_in_scale(cls, ctx->root_note_class, ctx->scale_name)) {
                color = COLOR_WHITE;
            }
        }
        g_host->set_pad_led(g_host->ctx, idx, color);
    }
}

static void *create_instance(const char *module_dir, const char *json_defaults)
{
    (void)module_dir;
    (void)json_defaults;
    moveish_t *inst = (moveish_t *)calloc(1, sizeof(moveish_t));
    if (!inst) return 0;
    inst->layout = LAYOUT_CHROMATIC;
    inst->offset = 0;
    inst->fixed_velocity = 0;
    return inst;
}

static void destroy_instance(void *instance)
{
    free(instance);
}

static void set_param(void *instance, const char *key, const char *value)
{
    moveish_t *inst = (moveish_t *)instance;
    if (!inst || !key) return;
    const char *val = value ? value : "";
    if (strcmp(key, "layout") == 0) {
        if (strcmp(val, "1") == 0 ||
            strcmp(val, "In-Key Octaves") == 0 ||
            strcmp(val, "in_key_octaves") == 0) {
            inst->layout = LAYOUT_IN_KEY_OCTAVES;
        } else if (strcmp(val, "2") == 0 ||
                   strcmp(val, "In-Key 4ths") == 0 ||
                   strcmp(val, "in_key_4ths") == 0) {
            inst->layout = LAYOUT_IN_KEY_4THS;
        } else {
            inst->layout = LAYOUT_CHROMATIC;
        }
    } else if (strcmp(key, "offset") == 0) {
        inst->offset = clamp_int(atoi(val), -32, 32);
    } else if (strcmp(key, "velocity") == 0) {
        inst->fixed_velocity = (strcmp(val, "Fixed 100") == 0 || strcmp(val, "fixed") == 0);
    }
}

static int get_param(void *instance, const char *key, char *buf, int buf_len)
{
    moveish_t *inst = (moveish_t *)instance;
    if (!inst || !key || !buf || buf_len <= 0) return -1;
    if (strcmp(key, "layout") == 0) {
        const char *layout = "Chromatic";
        if (inst->layout == LAYOUT_IN_KEY_OCTAVES) layout = "In-Key Octaves";
        if (inst->layout == LAYOUT_IN_KEY_4THS) layout = "In-Key 4ths";
        return snprintf(buf, buf_len, "%s", layout);
    }
    if (strcmp(key, "offset") == 0) {
        return snprintf(buf, buf_len, "%d", inst->offset);
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
    moveish_t *inst = (moveish_t *)instance;
    if (!inst || !in || !ctx || !out) return -1;
    memset(out, 0, sizeof(*out));

    if (in->cable != 0 || in->data1 < PAD_FIRST || in->data1 > PAD_LAST) return 0;
    uint8_t type = in->status & 0xF0;
    if (type != 0x80 && type != 0x90) return 0;

    int idx = in->data1 - PAD_FIRST;
    int channel = clamp_int(ctx->active_track, 0, 15);
    int is_note_on = (type == 0x90 && in->data2 > 0);
    if (!is_note_on && !inst->held[idx]) {
        out->handled = 1;
        out->request_led_redraw = 1;
        return 0;
    }
    int note = is_note_on ? pad_to_note(inst, in->data1, ctx) : inst->held_note[idx];
    if (note < 0 || note > 127) {
        out->handled = 1;
        out->request_led_redraw = 1;
        return 0;
    }

    input_usb_midi_packet_t *pkt = &out->outputs[0];
    pkt->cable = 2;
    pkt->data1 = (uint8_t)note;
    if (is_note_on) {
        uint8_t vel = inst->fixed_velocity ? 100 : in->data2;
        pkt->cin = 0x09;
        pkt->status = (uint8_t)(0x90 | channel);
        pkt->data2 = vel;
        inst->held_note[idx] = (uint8_t)note;
        inst->held_channel[idx] = (uint8_t)channel;
        inst->held[idx] = 1;
    } else {
        uint8_t off_channel = inst->held[idx] ? inst->held_channel[idx] : (uint8_t)channel;
        pkt->cin = 0x08;
        pkt->status = (uint8_t)(0x80 | off_channel);
        pkt->data2 = 0;
        inst->held[idx] = 0;
    }
    out->output_count = 1;
    out->handled = 1;
    out->request_led_redraw = 1;
    return 0;
}

static void on_context_changed(void *instance, const input_context_t *ctx)
{
    moveish_t *inst = (moveish_t *)instance;
    draw_leds(inst, ctx);
}

static void on_all_notes_off(void *instance, const input_context_t *ctx)
{
    moveish_t *inst = (moveish_t *)instance;
    (void)ctx;
    if (!inst) return;
    memset(inst->held, 0, sizeof(inst->held));
}

static input_module_api_v1_t api = {
    .api_version = INPUT_MODULE_API_VERSION,
    .create_instance = create_instance,
    .destroy_instance = destroy_instance,
    .set_param = set_param,
    .get_param = get_param,
    .process_midi = process_midi,
    .on_context_changed = on_context_changed,
    .on_all_notes_off = on_all_notes_off
};

input_module_api_v1_t *schwung_input_module_init_v1(const host_input_api_v1_t *host)
{
    g_host = host;
    (void)g_host;
    return &api;
}
