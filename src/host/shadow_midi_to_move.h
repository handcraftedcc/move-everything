#ifndef SHADOW_MIDI_TO_MOVE_H
#define SHADOW_MIDI_TO_MOVE_H

#include <stdint.h>

/* Open and map /shadow_midi_to_move (lazy, idempotent). */
int shadow_midi_to_move_open(void);

/* Enqueue one USB-MIDI packet (4 bytes). Non-blocking. */
int shadow_midi_to_move_send_usb_packet(uint8_t p0, uint8_t p1, uint8_t p2, uint8_t p3);

/* Convenience helpers: channel is 0-15 in low nibble. */
int shadow_midi_to_move_note_on(uint8_t channel, uint8_t note, uint8_t velocity);
int shadow_midi_to_move_note_off(uint8_t channel, uint8_t note, uint8_t velocity);
int shadow_midi_to_move_cc(uint8_t channel, uint8_t cc, uint8_t value);
int shadow_midi_to_move_pitchbend(uint8_t channel, uint16_t value);

#endif /* SHADOW_MIDI_TO_MOVE_H */
