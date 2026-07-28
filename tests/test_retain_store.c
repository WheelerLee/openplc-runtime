#include "retain_store.h"
#include "unity.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TEST_RETAIN_BUFFER_SIZE 4

static IEC_BOOL mx_values[TEST_RETAIN_BUFFER_SIZE][8];
static IEC_UINT mw_values[TEST_RETAIN_BUFFER_SIZE];
static IEC_UDINT md_values[TEST_RETAIN_BUFFER_SIZE];
static IEC_ULINT ml_values[TEST_RETAIN_BUFFER_SIZE];

static IEC_BOOL *mx_pointers[TEST_RETAIN_BUFFER_SIZE][8];
static IEC_UINT *mw_pointers[TEST_RETAIN_BUFFER_SIZE];
static IEC_UDINT *md_pointers[TEST_RETAIN_BUFFER_SIZE];
static IEC_ULINT *ml_pointers[TEST_RETAIN_BUFFER_SIZE];

static retain_store_buffers_t buffers;
static char snapshot_path[160];
static char temporary_path[176];

static void image_lock_noop(void)
{
}

static void image_unlock_noop(void)
{
}

static void clear_values(void)
{
    memset(mx_values, 0, sizeof(mx_values));
    memset(mw_values, 0, sizeof(mw_values));
    memset(md_values, 0, sizeof(md_values));
    memset(ml_values, 0, sizeof(ml_values));
}

void setUp(void)
{
    clear_values();

    for (int i = 0; i < TEST_RETAIN_BUFFER_SIZE; ++i) {
        for (int bit = 0; bit < 8; ++bit) {
            mx_pointers[i][bit] = &mx_values[i][bit];
        }
        mw_pointers[i] = &mw_values[i];
        md_pointers[i] = &md_values[i];
        ml_pointers[i] = &ml_values[i];
    }

    buffers.bool_memory = mx_pointers;
    buffers.int_memory = mw_pointers;
    buffers.dint_memory = md_pointers;
    buffers.lint_memory = ml_pointers;
    buffers.buffer_size = TEST_RETAIN_BUFFER_SIZE;
    buffers.image_lock = image_lock_noop;
    buffers.image_unlock = image_unlock_noop;

    snprintf(snapshot_path, sizeof(snapshot_path), "/tmp/openplc-retain-%ld.bin",
             (long)getpid());
    snprintf(temporary_path, sizeof(temporary_path), "%s.tmp", snapshot_path);
    unlink(snapshot_path);
    unlink(temporary_path);
    TEST_ASSERT_EQUAL_INT(0, retain_store_set_path_for_test(snapshot_path));
}

void tearDown(void)
{
    retain_store_stop(false);
    unlink(snapshot_path);
    unlink(temporary_path);
}

void test_missing_snapshot_is_a_normal_fresh_start(void)
{
    mw_values[0] = 1234;

    TEST_ASSERT_EQUAL_INT(0, retain_store_restore(&buffers));
    TEST_ASSERT_EQUAL_UINT16(1234, mw_values[0]);
    TEST_ASSERT_EQUAL_INT(-1, access(snapshot_path, F_OK));
}

void test_dirty_snapshot_round_trips_all_m_areas(void)
{
    mx_values[2][5] = 1;
    mw_values[1] = 0x1234;
    md_values[2] = 0x89ABCDEFu;
    ml_values[3] = UINT64_C(0x0123456789ABCDEF);

    TEST_ASSERT_EQUAL_INT(0, retain_store_start(&buffers));
    retain_store_mark_dirty();
    retain_store_stop(true);

    clear_values();
    TEST_ASSERT_EQUAL_INT(0, retain_store_restore(&buffers));

    TEST_ASSERT_EQUAL_UINT8(1, mx_values[2][5]);
    TEST_ASSERT_EQUAL_HEX16(0x1234, mw_values[1]);
    TEST_ASSERT_EQUAL_HEX32(0x89ABCDEFu, md_values[2]);
    TEST_ASSERT_EQUAL_HEX64(UINT64_C(0x0123456789ABCDEF), ml_values[3]);
}

void test_stop_without_dirty_data_does_not_create_snapshot(void)
{
    TEST_ASSERT_EQUAL_INT(0, retain_store_start(&buffers));
    retain_store_stop(true);

    TEST_ASSERT_EQUAL_INT(-1, access(snapshot_path, F_OK));
}

void test_same_crc_does_not_replace_existing_snapshot(void)
{
    mw_values[0] = 42;
    TEST_ASSERT_EQUAL_INT(0, retain_store_start(&buffers));
    retain_store_mark_dirty();
    retain_store_stop(true);

    struct stat before;
    struct stat after;
    TEST_ASSERT_EQUAL_INT(0, stat(snapshot_path, &before));

    TEST_ASSERT_EQUAL_INT(0, retain_store_start(&buffers));
    retain_store_mark_dirty();
    retain_store_stop(true);
    TEST_ASSERT_EQUAL_INT(0, stat(snapshot_path, &after));

    TEST_ASSERT_EQUAL_UINT64((uint64_t)before.st_ino, (uint64_t)after.st_ino);
}

void test_crc_mismatch_is_ignored(void)
{
    ml_values[0] = 99;
    TEST_ASSERT_EQUAL_INT(0, retain_store_start(&buffers));
    retain_store_mark_dirty();
    retain_store_stop(true);

    int fd = open(snapshot_path, O_RDWR);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
    TEST_ASSERT_NOT_EQUAL(-1, lseek(fd, -1, SEEK_END));
    uint8_t corrupt = 0xA5;
    TEST_ASSERT_EQUAL_INT(1, write(fd, &corrupt, sizeof(corrupt)));
    close(fd);

    clear_values();
    TEST_ASSERT_EQUAL_INT(-1, retain_store_restore(&buffers));
    TEST_ASSERT_EQUAL_UINT64(0, ml_values[0]);
}
