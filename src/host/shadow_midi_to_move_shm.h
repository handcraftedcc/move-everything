#ifndef SHADOW_MIDI_TO_MOVE_SHM_H
#define SHADOW_MIDI_TO_MOVE_SHM_H

#include <stddef.h>
#include <stdint.h>

#define SHADOW_MIDI_TO_MOVE_SHM_NAME "/shadow_midi_to_move"
#define SHADOW_MIDI_TO_MOVE_MAGIC 0x534d324du  /* "SM2M" */
#define SHADOW_MIDI_TO_MOVE_VERSION 2u
#define SHADOW_MIDI_TO_MOVE_DEFAULT_CAPACITY 1024u
#define SHADOW_MIDI_TO_MOVE_PACKET_SIZE 4u

/* Monotonic indices: producers reserve via atomic fetch-add on write_idx.
 * Reader advances read_idx after successful injection.
 *
 * Payload area layout:
 *   packets[capacity][4]
 *   ready_tokens[capacity]  (uint32_t token == reserved_idx + 1 when slot is ready)
 */
typedef struct shadow_midi_to_move_t {
    uint32_t magic;
    uint32_t version;
    uint32_t capacity;
    uint32_t write_idx;
    uint32_t read_idx;
    uint8_t data[];
} shadow_midi_to_move_t;

static inline uint8_t *shadow_midi_to_move_packets(shadow_midi_to_move_t *shm)
{
    return shm ? shm->data : NULL;
}

static inline const uint8_t *shadow_midi_to_move_packets_const(const shadow_midi_to_move_t *shm)
{
    return shm ? shm->data : NULL;
}

static inline uint32_t *shadow_midi_to_move_ready(shadow_midi_to_move_t *shm)
{
    if (!shm) return NULL;
    return (uint32_t *)(shm->data + ((size_t)shm->capacity * SHADOW_MIDI_TO_MOVE_PACKET_SIZE));
}

static inline const uint32_t *shadow_midi_to_move_ready_const(const shadow_midi_to_move_t *shm)
{
    if (!shm) return NULL;
    return (const uint32_t *)(shm->data + ((size_t)shm->capacity * SHADOW_MIDI_TO_MOVE_PACKET_SIZE));
}

static inline size_t shadow_midi_to_move_shm_size(uint32_t capacity)
{
    return sizeof(shadow_midi_to_move_t) +
           ((size_t)capacity * SHADOW_MIDI_TO_MOVE_PACKET_SIZE) +
           ((size_t)capacity * sizeof(uint32_t));
}

#endif /* SHADOW_MIDI_TO_MOVE_SHM_H */
