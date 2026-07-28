#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "retain_store.h"

#include "utils/log.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define RETAIN_MAGIC 0x4D4E5452u /* "RTNM" in a little-endian file */
#define RETAIN_VERSION 1u
#ifdef TEST
#define RETAIN_FLUSH_INTERVAL_NS 1000000L
#else
#define RETAIN_FLUSH_INTERVAL_NS 500000000L
#endif
#define RETAIN_MX_WIDTH 8u

typedef struct
{
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint32_t buffer_size;
    uint32_t payload_size;
    uint32_t payload_crc32;
} retain_file_header_t;

_Static_assert(sizeof(retain_file_header_t) == 20, "retain file header layout changed");

static retain_store_buffers_t g_buffers;
static pthread_t g_worker;
static atomic_bool g_running = false;
static atomic_bool g_stop_requested = false;
static atomic_bool g_dirty = false;
static uint32_t g_last_crc;
static bool g_last_crc_valid;
static char g_retain_path[PATH_MAX] = RETAIN_STORE_FILE;

static size_t payload_size_for(int buffer_size)
{
    if (buffer_size <= 0) {
        return 0;
    }

    return (size_t)buffer_size *
           (RETAIN_MX_WIDTH * sizeof(uint8_t) + sizeof(uint16_t) + sizeof(uint32_t) +
            sizeof(uint64_t));
}

static bool buffers_valid(const retain_store_buffers_t *buffers)
{
    return buffers != NULL && buffers->bool_memory != NULL && buffers->int_memory != NULL &&
           buffers->dint_memory != NULL && buffers->lint_memory != NULL &&
           buffers->buffer_size > 0 && buffers->image_lock != NULL &&
           buffers->image_unlock != NULL;
}

static uint32_t crc32_bytes(const uint8_t *data, size_t size)
{
    uint32_t crc = UINT32_MAX;

    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned int bit = 0; bit < 8; ++bit) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }

    return ~crc;
}

static void encode_payload(const retain_store_buffers_t *buffers, uint8_t *payload)
{
    size_t offset = 0;

    buffers->image_lock();

    for (int i = 0; i < buffers->buffer_size; ++i) {
        for (unsigned int bit = 0; bit < RETAIN_MX_WIDTH; ++bit) {
            IEC_BOOL *ptr = buffers->bool_memory[i][bit];
            payload[offset++] = ptr != NULL ? (uint8_t)(*ptr != 0) : 0u;
        }
    }
    for (int i = 0; i < buffers->buffer_size; ++i) {
        IEC_UINT value = buffers->int_memory[i] != NULL ? *buffers->int_memory[i] : 0u;
        memcpy(payload + offset, &value, sizeof(value));
        offset += sizeof(value);
    }
    for (int i = 0; i < buffers->buffer_size; ++i) {
        IEC_UDINT value = buffers->dint_memory[i] != NULL ? *buffers->dint_memory[i] : 0u;
        memcpy(payload + offset, &value, sizeof(value));
        offset += sizeof(value);
    }
    for (int i = 0; i < buffers->buffer_size; ++i) {
        IEC_ULINT value = buffers->lint_memory[i] != NULL ? *buffers->lint_memory[i] : 0u;
        memcpy(payload + offset, &value, sizeof(value));
        offset += sizeof(value);
    }

    buffers->image_unlock();
}

static void decode_payload(const retain_store_buffers_t *buffers, const uint8_t *payload)
{
    size_t offset = 0;

    buffers->image_lock();

    for (int i = 0; i < buffers->buffer_size; ++i) {
        for (unsigned int bit = 0; bit < RETAIN_MX_WIDTH; ++bit) {
            IEC_BOOL *ptr = buffers->bool_memory[i][bit];
            if (ptr != NULL) {
                *ptr = (IEC_BOOL)(payload[offset] != 0);
            }
            ++offset;
        }
    }
    for (int i = 0; i < buffers->buffer_size; ++i) {
        IEC_UINT value;
        memcpy(&value, payload + offset, sizeof(value));
        offset += sizeof(value);
        if (buffers->int_memory[i] != NULL) {
            *buffers->int_memory[i] = value;
        }
    }
    for (int i = 0; i < buffers->buffer_size; ++i) {
        IEC_UDINT value;
        memcpy(&value, payload + offset, sizeof(value));
        offset += sizeof(value);
        if (buffers->dint_memory[i] != NULL) {
            *buffers->dint_memory[i] = value;
        }
    }
    for (int i = 0; i < buffers->buffer_size; ++i) {
        IEC_ULINT value;
        memcpy(&value, payload + offset, sizeof(value));
        offset += sizeof(value);
        if (buffers->lint_memory[i] != NULL) {
            *buffers->lint_memory[i] = value;
        }
    }

    buffers->image_unlock();
}

static int read_exact(int fd, void *buffer, size_t size)
{
    uint8_t *cursor = buffer;
    size_t total = 0;

    while (total < size) {
        ssize_t count = read(fd, cursor + total, size - total);
        if (count == 0) {
            return -1;
        }
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        total += (size_t)count;
    }

    return 0;
}

static int write_exact(int fd, const void *buffer, size_t size)
{
    const uint8_t *cursor = buffer;
    size_t total = 0;

    while (total < size) {
        ssize_t count = write(fd, cursor + total, size - total);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (count == 0) {
            return -1;
        }
        total += (size_t)count;
    }

    return 0;
}

static int parent_directory(char *directory, size_t size)
{
    int result = snprintf(directory, size, "%s", g_retain_path);
    if (result < 0 || (size_t)result >= size) {
        return -1;
    }

    char *slash = strrchr(directory, '/');
    if (slash == NULL) {
        return -1;
    }
    if (slash == directory) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }
    return 0;
}

static int ensure_parent_directory(void)
{
    char directory[PATH_MAX];
    if (parent_directory(directory, sizeof(directory)) != 0) {
        log_warn("[RETAIN] Invalid snapshot path: %s", g_retain_path);
        return -1;
    }

    if (mkdir(directory, 0755) != 0 && errno != EEXIST) {
        log_warn("[RETAIN] Cannot create %s: %s", directory, strerror(errno));
        return -1;
    }
    return 0;
}

static void sync_parent_directory(void)
{
    char directory[PATH_MAX];
    if (parent_directory(directory, sizeof(directory)) != 0) {
        return;
    }

#ifdef O_DIRECTORY
    int fd = open(directory, O_RDONLY | O_DIRECTORY);
#else
    int fd = open(directory, O_RDONLY);
#endif
    if (fd < 0) {
        log_warn("[RETAIN] Cannot open %s for directory sync: %s", directory, strerror(errno));
        return;
    }
    if (fsync(fd) != 0) {
        log_warn("[RETAIN] Cannot sync directory %s: %s", directory, strerror(errno));
    }
    if (close(fd) != 0) {
        log_warn("[RETAIN] Cannot close directory %s: %s", directory, strerror(errno));
    }
}

static int write_snapshot(const retain_store_buffers_t *buffers)
{
    size_t payload_size = payload_size_for(buffers->buffer_size);
    if (payload_size == 0 || payload_size > UINT32_MAX) {
        log_warn("[RETAIN] Invalid M-area payload size");
        return -1;
    }

    uint8_t *payload = malloc(payload_size);
    if (payload == NULL) {
        log_warn("[RETAIN] Cannot allocate %zu-byte M-area snapshot", payload_size);
        return -1;
    }

    encode_payload(buffers, payload);
    uint32_t crc = crc32_bytes(payload, payload_size);

    if (g_last_crc_valid && crc == g_last_crc && access(g_retain_path, F_OK) == 0) {
        free(payload);
        return 0;
    }

    if (ensure_parent_directory() != 0) {
        free(payload);
        return -1;
    }

    char temporary_path[PATH_MAX];
    int path_length = snprintf(temporary_path, sizeof(temporary_path), "%s.tmp", g_retain_path);
    if (path_length < 0 || (size_t)path_length >= sizeof(temporary_path)) {
        log_warn("[RETAIN] Snapshot path is too long");
        free(payload);
        return -1;
    }

    retain_file_header_t header = {
        .magic = RETAIN_MAGIC,
        .version = RETAIN_VERSION,
        .header_size = (uint16_t)sizeof(retain_file_header_t),
        .buffer_size = (uint32_t)buffers->buffer_size,
        .payload_size = (uint32_t)payload_size,
        .payload_crc32 = crc,
    };

    int fd = open(temporary_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        log_warn("[RETAIN] Cannot open %s: %s", temporary_path, strerror(errno));
        free(payload);
        return -1;
    }

    int result = 0;
    if (write_exact(fd, &header, sizeof(header)) != 0 ||
        write_exact(fd, payload, payload_size) != 0) {
        log_warn("[RETAIN] Cannot write %s: %s", temporary_path, strerror(errno));
        result = -1;
    } else if (fsync(fd) != 0) {
        log_warn("[RETAIN] Cannot sync %s: %s", temporary_path, strerror(errno));
        result = -1;
    }

    if (close(fd) != 0 && result == 0) {
        log_warn("[RETAIN] Cannot close %s: %s", temporary_path, strerror(errno));
        result = -1;
    }

    if (result == 0 && rename(temporary_path, g_retain_path) != 0) {
        log_warn("[RETAIN] Cannot replace %s: %s", g_retain_path, strerror(errno));
        result = -1;
    }

    if (result == 0) {
        sync_parent_directory();
        g_last_crc = crc;
        g_last_crc_valid = true;
        log_debug("[RETAIN] Saved M-area snapshot to %s", g_retain_path);
    } else if (unlink(temporary_path) != 0 && errno != ENOENT) {
        log_warn("[RETAIN] Cannot remove temporary snapshot %s: %s", temporary_path,
                 strerror(errno));
    }

    free(payload);
    return result;
}

int retain_store_restore(const retain_store_buffers_t *buffers)
{
    if (!buffers_valid(buffers)) {
        log_warn("[RETAIN] Cannot restore: invalid M-area buffers");
        return -1;
    }

    g_last_crc_valid = false;
    int fd = open(g_retain_path, O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) {
            return 0;
        }
        log_warn("[RETAIN] Cannot open %s: %s; using default M-area values", g_retain_path,
                 strerror(errno));
        return -1;
    }

    retain_file_header_t header;
    struct stat file_stat;
    int result = -1;
    uint8_t *payload = NULL;

    if (fstat(fd, &file_stat) != 0 || read_exact(fd, &header, sizeof(header)) != 0) {
        log_warn("[RETAIN] Cannot read snapshot header from %s; using default M-area values",
                 g_retain_path);
        goto cleanup;
    }

    size_t expected_payload_size = payload_size_for(buffers->buffer_size);
    off_t expected_file_size = (off_t)sizeof(header) + (off_t)expected_payload_size;
    if (header.magic != RETAIN_MAGIC || header.version != RETAIN_VERSION ||
        header.header_size != sizeof(header) ||
        header.buffer_size != (uint32_t)buffers->buffer_size ||
        header.payload_size != expected_payload_size || file_stat.st_size != expected_file_size) {
        log_warn("[RETAIN] Snapshot layout mismatch in %s; using default M-area values",
                 g_retain_path);
        goto cleanup;
    }

    payload = malloc(expected_payload_size);
    if (payload == NULL) {
        log_warn("[RETAIN] Cannot allocate %zu-byte restore buffer", expected_payload_size);
        goto cleanup;
    }
    if (read_exact(fd, payload, expected_payload_size) != 0) {
        log_warn("[RETAIN] Snapshot payload is truncated in %s; using default M-area values",
                 g_retain_path);
        goto cleanup;
    }

    uint32_t crc = crc32_bytes(payload, expected_payload_size);
    if (crc != header.payload_crc32) {
        log_warn("[RETAIN] Snapshot CRC mismatch in %s; using default M-area values",
                 g_retain_path);
        goto cleanup;
    }

    decode_payload(buffers, payload);
    g_last_crc = crc;
    g_last_crc_valid = true;
    log_info("[RETAIN] Restored M-area snapshot from %s", g_retain_path);
    result = 0;

cleanup:
    free(payload);
    if (close(fd) != 0) {
        log_warn("[RETAIN] Cannot close %s: %s", g_retain_path, strerror(errno));
    }
    return result;
}

static void sleep_until_flush_interval(void)
{
    struct timespec delay = {
        .tv_sec = 0,
        .tv_nsec = RETAIN_FLUSH_INTERVAL_NS,
    };

    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
    }
}

static void *retain_worker(void *unused)
{
    (void)unused;

    while (!atomic_load_explicit(&g_stop_requested, memory_order_acquire)) {
        sleep_until_flush_interval();
        if (atomic_load_explicit(&g_stop_requested, memory_order_acquire)) {
            break;
        }
        if (atomic_exchange_explicit(&g_dirty, false, memory_order_acq_rel)) {
            if (write_snapshot(&g_buffers) != 0) {
                /* Retry a transient storage failure on the next interval. */
                atomic_store_explicit(&g_dirty, true, memory_order_release);
            }
        }
    }

    return NULL;
}

int retain_store_start(const retain_store_buffers_t *buffers)
{
    if (!buffers_valid(buffers)) {
        log_warn("[RETAIN] Cannot start: invalid M-area buffers");
        return -1;
    }
    if (atomic_load_explicit(&g_running, memory_order_acquire)) {
        return 0;
    }

    memcpy(&g_buffers, buffers, sizeof(g_buffers));
    atomic_store_explicit(&g_stop_requested, false, memory_order_release);
    atomic_store_explicit(&g_dirty, false, memory_order_release);

    int result = pthread_create(&g_worker, NULL, retain_worker, NULL);
    if (result != 0) {
        log_warn("[RETAIN] Cannot start persistence worker: %s", strerror(result));
        memset(&g_buffers, 0, sizeof(g_buffers));
        return -1;
    }

    atomic_store_explicit(&g_running, true, memory_order_release);
    log_info("[RETAIN] M-area persistence enabled at %s", g_retain_path);
    return 0;
}

void retain_store_mark_dirty(void)
{
    if (atomic_load_explicit(&g_running, memory_order_relaxed)) {
        atomic_store_explicit(&g_dirty, true, memory_order_release);
    }
}

void retain_store_stop(bool flush_pending)
{
    if (!atomic_exchange_explicit(&g_running, false, memory_order_acq_rel)) {
        return;
    }

    atomic_store_explicit(&g_stop_requested, true, memory_order_release);
    int result = pthread_join(g_worker, NULL);
    if (result != 0) {
        log_warn("[RETAIN] Cannot join persistence worker: %s", strerror(result));
    }

    bool was_dirty = atomic_exchange_explicit(&g_dirty, false, memory_order_acq_rel);
    if (flush_pending && was_dirty && result == 0) {
        (void)write_snapshot(&g_buffers);
    }

    memset(&g_buffers, 0, sizeof(g_buffers));
}

#ifdef TEST
int retain_store_set_path_for_test(const char *path)
{
    if (path == NULL || atomic_load_explicit(&g_running, memory_order_acquire)) {
        return -1;
    }

    int result = snprintf(g_retain_path, sizeof(g_retain_path), "%s", path);
    if (result < 0 || (size_t)result >= sizeof(g_retain_path)) {
        return -1;
    }
    g_last_crc_valid = false;
    return 0;
}
#endif
