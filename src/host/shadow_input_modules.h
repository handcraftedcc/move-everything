/* shadow_input_modules.h - Runtime for pre-native input modules. */

#ifndef SHADOW_INPUT_MODULES_H
#define SHADOW_INPUT_MODULES_H

#include <stdint.h>
#include "shadow_constants.h"

typedef struct shadow_input_host_t {
    void (*log)(const char *msg);
    int (*emit_midi)(const uint8_t *msg, int len);
    float (*get_bpm)(void);
    int (*get_transport_playing)(void);
    shadow_control_t **shadow_control_ptr;
} shadow_input_host_t;

void shadow_input_modules_init(const shadow_input_host_t *host);
void shadow_input_modules_shutdown(void);

int shadow_input_load_state_dir(const char *state_dir);
int shadow_input_set_track_module(int track, const char *module_id);
int shadow_input_set_track_param(int track, const char *key, const char *value);
int shadow_input_get_track_param(int track, const char *key, char *buf, int buf_len);

void shadow_input_tick_context(void);
void shadow_input_panic_all(void);
void shadow_input_update_key_scale_from_text(const char *text);
int shadow_input_process_pad_event(const uint8_t usb_packet[4]);
void shadow_input_process_control_event(const uint8_t usb_packet[4]);

#endif /* SHADOW_INPUT_MODULES_H */
