#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host/input_module_api_v1.h"

#define PAD_FIRST 68
#define PAD_LAST 99
#define PAD_COUNT 32
#define COLOR_OFF 0
#define COLOR_WHITE 120

typedef struct true_chromatic_t {
    int root_note;
    int fixed_velocity;
    uint8_t held_note[PAD_COUNT];
    uint8_t held_channel[PAD_COUNT];
    uint8_t held[PAD_COUNT];
} true_chromatic_t;

static const host_input_api_v1_t *g_host = 0;

static int clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int pad_to_note(const true_chromatic_t *inst, int pad_note, int octave_index)
{
    int idx = pad_note - PAD_FIRST;
    return inst->root_note + ((octave_index - 2) * 12) + idx;
}

static int note_class(int note)
{
    int cls = note % 12;
    return cls < 0 ? cls + 12 : cls;
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

static unsigned int scale_mask(const char *scale_name)
{
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

    static const char *captured_move_scale_names =
        "Harmonic Major, Dorian #4, Phrygian Dominant, Lydian Augmented, "
        "Lydian Dominant, Super Locrian, 8-Tone Spanish, Bhairav, Kumoi, "
        "Pelog Selisir, Pelog Tembung, Hungarian Minor, Messiaen 3, "
        "Messiaen 4, Messiaen 5, Messiaen 6, Messiaen 7";
    (void)captured_move_scale_names;

    char name[64];
    lower_compact(scale_name, name, sizeof(name));
    if (!name[0] || strstr(name, "chromatic")) return 0x0FFFu;
    if (strstr(name, "majorpentatonic")) return mask_from_intervals(major_pentatonic, 5);
    if (strstr(name, "minorpentatonic")) return mask_from_intervals(minor_pentatonic, 5);
    if (strstr(name, "harmonicmajor")) return mask_from_intervals(harmonic_major, 7);
    if (strstr(name, "harmonicminor")) return mask_from_intervals(harmonic_minor, 7);
    if (strstr(name, "melodicminor")) return mask_from_intervals(melodic_minor, 7);
    if (strstr(name, "hungarianminor")) return mask_from_intervals(hungarian_minor, 7);
    if (strstr(name, "dorian#4")) return mask_from_intervals(dorian_sharp4, 7);
    if (strstr(name, "phrygiandominant")) return mask_from_intervals(phrygian_dominant, 7);
    if (strstr(name, "lydianaugmented")) return mask_from_intervals(lydian_augmented, 7);
    if (strstr(name, "lydiandominant")) return mask_from_intervals(lydian_dominant, 7);
    if (strstr(name, "superlocrian")) return mask_from_intervals(super_locrian, 7);
    if (strstr(name, "8tonespanish")) return mask_from_intervals(eight_tone_spanish, 8);
    if (strstr(name, "bhairav")) return mask_from_intervals(bhairav, 7);
    if (strstr(name, "minorblues")) return mask_from_intervals(blues, 6);
    if (strstr(name, "blues")) return mask_from_intervals(blues, 6);
    if (strstr(name, "naturalminor") || strstr(name, "minor")) return mask_from_intervals(natural_minor, 7);
    if (strstr(name, "dorian")) return mask_from_intervals(dorian, 7);
    if (strstr(name, "phrygian")) return mask_from_intervals(phrygian, 7);
    if (strstr(name, "lydian")) return mask_from_intervals(lydian, 7);
    if (strstr(name, "mixolydian")) return mask_from_intervals(mixolydian, 7);
    if (strstr(name, "locrian")) return mask_from_intervals(locrian, 7);
    if (strstr(name, "wholetone")) return mask_from_intervals(whole_tone, 6);
    if (strstr(name, "wholehalfdim")) return mask_from_intervals(whole_half_dim, 8);
    if (strstr(name, "halfwholedim")) return mask_from_intervals(half_whole_dim, 8);
    if (strstr(name, "diminished")) return mask_from_intervals(whole_half_dim, 8);
    if (strstr(name, "hirajoshi")) return mask_from_intervals(hirajoshi, 5);
    if (strstr(name, "insen")) return mask_from_intervals(in_sen, 5);
    if (strstr(name, "iwato")) return mask_from_intervals(iwato, 5);
    if (strstr(name, "kumoi")) return mask_from_intervals(kumoi, 5);
    if (strstr(name, "pelogselisir")) return mask_from_intervals(pelog_selisir, 5);
    if (strstr(name, "pelogtembung")) return mask_from_intervals(pelog_tembung, 5);
    if (strstr(name, "messiaen3")) return mask_from_intervals(messiaen3, 9);
    if (strstr(name, "messiaen4")) return mask_from_intervals(messiaen4, 8);
    if (strstr(name, "messiaen5")) return mask_from_intervals(messiaen5, 6);
    if (strstr(name, "messiaen6")) return mask_from_intervals(messiaen6, 8);
    if (strstr(name, "messiaen7")) return mask_from_intervals(messiaen7, 10);
    return mask_from_intervals(major, 7);
}

static int is_in_scale(int note_cls, int root_cls, const char *scale_name)
{
    if (root_cls < 0 || root_cls > 11) return 1;
    int interval = (note_cls - root_cls + 12) % 12;
    return (scale_mask(scale_name) & (1u << interval)) != 0;
}

static void draw_leds(const true_chromatic_t *inst, const input_context_t *ctx)
{
    if (!inst || !ctx || !g_host || !g_host->set_pad_led) return;
    int track = clamp_int(ctx->active_track, 0, 3);
    int track_color = g_host->get_track_color ? g_host->get_track_color(g_host->ctx, track) : -1;
    uint8_t root_color = (uint8_t)((track_color > 0) ? track_color : COLOR_WHITE);

    for (int idx = 0; idx < PAD_COUNT; idx++) {
        int note = pad_to_note(inst, PAD_FIRST + idx, ctx->octave_index);
        uint8_t color = COLOR_OFF;
        if (note >= 0 && note <= 127) {
            int cls = note_class(note);
            if (ctx->root_note_class >= 0 && cls == ctx->root_note_class) {
                color = root_color;
            } else if (is_in_scale(cls, ctx->root_note_class, ctx->scale_name)) {
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
    true_chromatic_t *inst = (true_chromatic_t *)calloc(1, sizeof(true_chromatic_t));
    if (!inst) return 0;
    inst->root_note = 48;
    inst->fixed_velocity = 0;
    return inst;
}

static void destroy_instance(void *instance)
{
    free(instance);
}

static void set_param(void *instance, const char *key, const char *value)
{
    true_chromatic_t *inst = (true_chromatic_t *)instance;
    if (!inst || !key) return;
    const char *val = value ? value : "";
    if (strcmp(key, "root_note") == 0) {
        inst->root_note = clamp_int(atoi(val), 0, 127);
    } else if (strcmp(key, "velocity") == 0) {
        inst->fixed_velocity = (strcmp(val, "Fixed 100") == 0 || strcmp(val, "fixed") == 0);
    }
}

static int get_param(void *instance, const char *key, char *buf, int buf_len)
{
    true_chromatic_t *inst = (true_chromatic_t *)instance;
    if (!inst || !key || !buf || buf_len <= 0) return -1;
    if (strcmp(key, "root_note") == 0) {
        return snprintf(buf, buf_len, "%d", inst->root_note);
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
    true_chromatic_t *inst = (true_chromatic_t *)instance;
    if (!inst || !in || !ctx || !out) return -1;
    memset(out, 0, sizeof(*out));

    if (in->cable != 0 || in->data1 < PAD_FIRST || in->data1 > PAD_LAST) return 0;
    uint8_t type = in->status & 0xF0;
    if (type != 0x80 && type != 0x90) return 0;

    int idx = in->data1 - PAD_FIRST;
    int channel = clamp_int(ctx->active_track, 0, 15);
    int is_note_on = (type == 0x90 && in->data2 > 0);
    int note = is_note_on ? pad_to_note(inst, in->data1, ctx->octave_index) : inst->held_note[idx];
    if (note < 0 || note > 127) {
        out->handled = 1;
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
    return 0;
}

static void on_context_changed(void *instance, const input_context_t *ctx)
{
    true_chromatic_t *inst = (true_chromatic_t *)instance;
    draw_leds(inst, ctx);
}

static void on_all_notes_off(void *instance, const input_context_t *ctx)
{
    true_chromatic_t *inst = (true_chromatic_t *)instance;
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
