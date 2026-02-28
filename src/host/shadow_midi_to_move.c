#include "shadow_midi_to_move.h"

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "shadow_midi_to_move_shm.h"

static shadow_midi_to_move_t *g_shadow_midi_to_move = NULL;
static uint32_t g_shadow_midi_to_move_capacity = 0;

static int shadow_midi_to_move_validate(shadow_midi_to_move_t *shm, size_t mapped_size)
{
    if (!shm) return 0;
    if (shm->magic != SHADOW_MIDI_TO_MOVE_MAGIC) return 0;
    if (shm->version != SHADOW_MIDI_TO_MOVE_VERSION) return 0;
    if (shm->capacity == 0) return 0;
    if (shadow_midi_to_move_shm_size(shm->capacity) > mapped_size) return 0;
    return 1;
}

int shadow_midi_to_move_open(void)
{
    if (g_shadow_midi_to_move) return 1;

    int fd = shm_open(SHADOW_MIDI_TO_MOVE_SHM_NAME, O_RDWR, 0666);
    if (fd < 0) return 0;

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)sizeof(shadow_midi_to_move_t)) {
        close(fd);
        return 0;
    }

    void *mapped = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (mapped == MAP_FAILED) {
        return 0;
    }

    shadow_midi_to_move_t *shm = (shadow_midi_to_move_t *)mapped;
    if (!shadow_midi_to_move_validate(shm, (size_t)st.st_size)) {
        munmap(mapped, (size_t)st.st_size);
        return 0;
    }

    g_shadow_midi_to_move = shm;
    g_shadow_midi_to_move_capacity = shm->capacity;
    return 1;
}

int shadow_midi_to_move_send_usb_packet(uint8_t p0, uint8_t p1, uint8_t p2, uint8_t p3)
{
    if (!g_shadow_midi_to_move && !shadow_midi_to_move_open()) return 0;

    uint8_t cin = p0 & 0x0F;  /* Shim sets cable nibble to 2 at injection time. */
    uint32_t idx = __atomic_fetch_add(&g_shadow_midi_to_move->write_idx, 1u, __ATOMIC_ACQ_REL);
    uint32_t slot = idx % g_shadow_midi_to_move_capacity;
    uint32_t packet = (uint32_t)cin |
                      ((uint32_t)p1 << 8) |
                      ((uint32_t)p2 << 16) |
                      ((uint32_t)p3 << 24);

    uint32_t *packet_words = (uint32_t *)shadow_midi_to_move_packets(g_shadow_midi_to_move);
    uint32_t *ready_words = shadow_midi_to_move_ready(g_shadow_midi_to_move);
    __atomic_store_n(&packet_words[slot], packet, __ATOMIC_RELEASE);
    __atomic_store_n(&ready_words[slot], idx + 1u, __ATOMIC_RELEASE);

    return 1;
}

int shadow_midi_to_move_note_on(uint8_t channel, uint8_t note, uint8_t velocity)
{
    uint8_t ch = channel & 0x0F;
    return shadow_midi_to_move_send_usb_packet(0x09, (uint8_t)(0x90 | ch), note & 0x7F, velocity & 0x7F);
}

int shadow_midi_to_move_note_off(uint8_t channel, uint8_t note, uint8_t velocity)
{
    uint8_t ch = channel & 0x0F;
    return shadow_midi_to_move_send_usb_packet(0x08, (uint8_t)(0x80 | ch), note & 0x7F, velocity & 0x7F);
}

int shadow_midi_to_move_cc(uint8_t channel, uint8_t cc, uint8_t value)
{
    uint8_t ch = channel & 0x0F;
    return shadow_midi_to_move_send_usb_packet(0x0B, (uint8_t)(0xB0 | ch), cc & 0x7F, value & 0x7F);
}

int shadow_midi_to_move_pitchbend(uint8_t channel, uint16_t value)
{
    uint8_t ch = channel & 0x0F;
    uint16_t bend = value & 0x3FFF;
    return shadow_midi_to_move_send_usb_packet(0x0E, (uint8_t)(0xE0 | ch),
                                               (uint8_t)(bend & 0x7F),
                                               (uint8_t)((bend >> 7) & 0x7F));
}
