/* move_mode_watcher.h - Cached Move UI mode watcher. */

#ifndef MOVE_MODE_WATCHER_H
#define MOVE_MODE_WATCHER_H

#include <stdint.h>
#include "shadow_constants.h"

typedef struct move_mode_watcher_host_t {
    void (*log)(const char *msg);
    shadow_control_t **shadow_control_ptr;
} move_mode_watcher_host_t;

void move_mode_watcher_init(const move_mode_watcher_host_t *host);
void move_mode_watcher_start(void);
void move_mode_watcher_stop(void);
uint32_t move_mode_watcher_generation(void);

#endif /* MOVE_MODE_WATCHER_H */
