/*
 * Schwung Input Module API v1
 *
 * Input modules transform physical Move input before native note handling.
 * Modules receive raw USB-MIDI packets and may emit replacement cable-2 MIDI.
 */

#ifndef SCHWUNG_INPUT_MODULE_API_V1_H
#define SCHWUNG_INPUT_MODULE_API_V1_H

#include <stdint.h>

#define INPUT_MODULE_API_VERSION 2
#define INPUT_MODULE_MAX_OUTPUT_PACKETS 16

typedef enum input_event_type_t {
    INPUT_EVENT_PAD = 0,
    INPUT_EVENT_BUTTON = 1,
    INPUT_EVENT_TRANSPORT = 2,
    INPUT_EVENT_MODE_CHANGED = 3,
    INPUT_EVENT_TRACK_CHANGED = 4,
    INPUT_EVENT_SET_CHANGED = 5,
    INPUT_EVENT_KEY_SCALE_CHANGED = 6
} input_event_type_t;

typedef struct input_usb_midi_packet_t {
    uint8_t cin;
    uint8_t status;
    uint8_t data1;
    uint8_t data2;
    uint8_t cable;
} input_usb_midi_packet_t;

typedef struct input_context_t {
    int active_track;
    int move_mode;
    int root_note_class;
    int octave_index;
    const char *root_name;
    const char *scale_name;
    int playing;
    double bpm;
    uint32_t mode_generation;
    uint32_t track_generation;
    uint32_t key_scale_generation;
} input_context_t;

typedef struct input_process_result_t {
    int handled;
    int output_count;
    input_usb_midi_packet_t outputs[INPUT_MODULE_MAX_OUTPUT_PACKETS];
    int request_led_redraw;
} input_process_result_t;

typedef struct host_input_api_v1 {
    void *ctx;

    int (*emit_midi)(void *ctx,
                     const input_usb_midi_packet_t *packets,
                     int count);

    int (*schedule_midi)(void *ctx,
                         const input_usb_midi_packet_t *packets,
                         int count,
                         uint32_t delay_us);

    int (*cancel_scheduled_midi)(void *ctx,
                                 int note,
                                 int channel);

    int (*set_pad_led)(void *ctx,
                       int pad_index,
                       uint8_t color);

    int (*get_pad_led)(void *ctx,
                       int pad_index);

    int (*get_track_color)(void *ctx,
                           int track_index);

    int (*get_active_track)(void *ctx);

    int (*get_root_note_class)(void *ctx);
    int (*get_octave_index)(void *ctx);
    const char* (*get_root_name)(void *ctx);
    const char* (*get_scale_name)(void *ctx);

    int (*get_transport_playing)(void *ctx);
    double (*get_transport_bpm)(void *ctx);

    void (*log)(void *ctx, const char *message);
} host_input_api_v1_t;

typedef struct input_module_api_v1 {
    int api_version;

    void* (*create_instance)(const char *module_dir,
                             const char *json_defaults);

    void (*destroy_instance)(void *instance);

    void (*set_param)(void *instance,
                      const char *key,
                      const char *value);

    int (*get_param)(void *instance,
                     const char *key,
                     char *buf,
                     int buf_len);

    int (*process_midi)(void *instance,
                        const input_usb_midi_packet_t *in,
                        const input_context_t *ctx,
                        input_process_result_t *out);

    void (*on_context_changed)(void *instance,
                               const input_context_t *ctx);

    void (*on_all_notes_off)(void *instance,
                             const input_context_t *ctx);

    void (*on_tick)(void *instance,
                    const input_context_t *ctx);
} input_module_api_v1_t;

typedef input_module_api_v1_t* (*schwung_input_module_init_v1_fn)(const host_input_api_v1_t *host);

#define SCHWUNG_INPUT_MODULE_INIT_V1_SYMBOL "schwung_input_module_init_v1"

#endif /* SCHWUNG_INPUT_MODULE_API_V1_H */
