#ifndef RETAIN_STORE_H
#define RETAIN_STORE_H

#include <stdbool.h>

#include "../lib/iec_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RETAIN_STORE_DIRECTORY "/var/run/runtime"
#define RETAIN_STORE_FILE RETAIN_STORE_DIRECTORY "/retain_m.bin"

typedef struct
{
    IEC_BOOL *(*bool_memory)[8];
    IEC_UINT **int_memory;
    IEC_UDINT **dint_memory;
    IEC_ULINT **lint_memory;
    int buffer_size;
    void (*image_lock)(void);
    void (*image_unlock)(void);
} retain_store_buffers_t;

/*
 * Restore the M-area snapshot into the supplied image tables.
 *
 * A missing file is a normal first-start condition and returns 0 without
 * logging an error. Invalid or unreadable files are ignored and return -1;
 * callers must continue starting the PLC with its default values.
 */
int retain_store_restore(const retain_store_buffers_t *buffers);

/* Start/stop the non-real-time persistence worker. */
int retain_store_start(const retain_store_buffers_t *buffers);
void retain_store_stop(bool flush_pending);

/*
 * Mark the M-area snapshot dirty. This is safe on the PLC scan path: it only
 * performs atomic loads/stores and never allocates, locks, logs, or performs
 * file I/O. All slow work is done by the worker thread.
 */
void retain_store_mark_dirty(void);

#ifdef TEST
/* Unit-test-only path override; production always uses RETAIN_STORE_FILE. */
int retain_store_set_path_for_test(const char *path);
#endif

#ifdef __cplusplus
}
#endif

#endif /* RETAIN_STORE_H */
