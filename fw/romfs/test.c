/**
 * Copyright (c) 2022-2023 sashz /pdaXrom.org/
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <inttypes.h>
#include "romfs.h"
#include "test_flash.h"

#if defined(__linux__) || defined(__APPLE__)
#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_YELLOW  "\x1b[33m"
#define ANSI_COLOR_CYAN    "\x1b[36m"
#define ANSI_COLOR_MAGENTA "\x1b[35m"
#define ANSI_COLOR_RESET   "\x1b[0m"
#else
#define ANSI_COLOR_RED     ""
#define ANSI_COLOR_GREEN   ""
#define ANSI_COLOR_YELLOW  ""
#define ANSI_COLOR_CYAN    ""
#define ANSI_COLOR_MAGENTA ""
#define ANSI_COLOR_RESET   ""
#endif

static uint8_t *flash_base = NULL;

const int NORMAL_CHUNK_SIZE = 256;
const int NORMAL_CHUNKS_PER_FILE = 20; // Creates 5KB files (256 * 20)
const int SMALL_FILE_SIZE = 1; // For testing file list limits

typedef struct {
    char name[ROMFS_MAX_NAME_LEN];
    uint32_t size;
    int file_index;
} random_fill_entry;

static uint32_t rng_state;
static uint64_t rng_calls;

/* Explicit 32-bit arithmetic gives the same sequence on every host libc. */
static uint32_t test_random(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    rng_calls++;
    return rng_state;
}

static void free_file_list(char **files, int count)
{
    for (int i = 0; i < count; i++) {
        free(files[i]);
    }
    free(files);
}

static bool flash_ok(void)
{
    const test_flash_stats *stats = test_flash_get_stats();
    return stats->rejected == 0 && stats->injected == 0;
}

static bool close_test_file(romfs_file *file)
{
    uint32_t err = romfs_close_file(file);
    if (err != ROMFS_NOERR) {
        fprintf(stderr, "Failed to close %s: %s\n", file->entry.name, romfs_strerror(err));
        return false;
    }
    return true;
}

// Fills a buffer with deterministic dummy data based on an index
static void create_test_data(uint8_t *buffer, size_t size, int file_idx, int chunk_idx)
{
    for (size_t i = 0; i < size; i++) {
        buffer[i] = (uint8_t)((file_idx + chunk_idx + i) & 0xFF);
    }
}

static bool test_service_sector_protection(uint16_t *flash_map, uint32_t map_size)
{
    printf(ANSI_COLOR_YELLOW "\n--- Running Service Sector Protection Test ---\n" ANSI_COLOR_RESET);

    if (!romfs_format()) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to format filesystem for service sector test\n" ANSI_COLOR_RESET);
        return false;
    }

    romfs_entry firmware = {0};
    romfs_entry flashmap = {0};
    if (romfs_get_entry("firmware", &firmware) != ROMFS_NOERR ||
            romfs_get_entry("flashmap", &flashmap) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Cannot read service entries\n" ANSI_COLOR_RESET);
        return false;
    }

    uint32_t first_data_sector = flashmap.start +
                                 (flashmap.size + ROMFS_FLASH_SECTOR - 1) / ROMFS_FLASH_SECTOR;
    uint32_t total_sectors = map_size / sizeof(uint16_t);
    if (first_data_sector >= total_sectors || firmware.size > first_data_sector * ROMFS_FLASH_SECTOR) {
        fprintf(stderr, ANSI_COLOR_RED "Invalid service sector layout\n" ANSI_COLOR_RESET);
        return false;
    }

    for (uint32_t i = 0; i < firmware.size; i++) {
        flash_base[i] = (uint8_t) ((i * 31u + 0x5au) & 0xffu);
    }

    // Simulate an old or damaged flash map that marks firmware and metadata
    // sectors as free. A user file must never be allocated in this range.
    for (uint32_t i = 0; i < first_data_sector; i++) {
        flash_map[i] = 0xffff;
    }

    uint8_t *io_buffer = malloc(ROMFS_FLASH_SECTOR);
    uint8_t *payload = malloc(ROMFS_FLASH_SECTOR);
    if (!io_buffer || !payload) {
        fprintf(stderr, ANSI_COLOR_RED "Allocation failure in service sector test\n" ANSI_COLOR_RESET);
        free(io_buffer);
        free(payload);
        return false;
    }
    memset(payload, 0xa5, ROMFS_FLASH_SECTOR);

    bool success = true;
    romfs_file file = {0};
    if (romfs_create_file("service-guard.bin", &file, ROMFS_MODE_READWRITE, ROMFS_TYPE_MISC,
                          io_buffer) != ROMFS_NOERR ||
            romfs_write_file(payload, ROMFS_FLASH_SECTOR, &file) != ROMFS_FLASH_SECTOR ||
            romfs_close_file(&file) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to write service sector test file: %s\n" ANSI_COLOR_RESET,
                romfs_strerror(file.err));
        success = false;
        goto cleanup;
    }

    if (file.entry.start < first_data_sector) {
        fprintf(stderr, ANSI_COLOR_RED "User file allocated protected sector %u (< %u)\n" ANSI_COLOR_RESET,
                file.entry.start, first_data_sector);
        success = false;
        goto cleanup;
    }

    for (uint32_t i = 0; i < firmware.size; i++) {
        uint8_t expected = (uint8_t) ((i * 31u + 0x5au) & 0xffu);
        if (flash_base[i] != expected) {
            fprintf(stderr, ANSI_COLOR_RED "Firmware changed at offset 0x%08x\n" ANSI_COLOR_RESET, i);
            success = false;
            goto cleanup;
        }
    }

    romfs_file protected_file = {0};
    if (romfs_open_append("firmware", &protected_file, ROMFS_TYPE_MISC, io_buffer) != ROMFS_ERR_OPERATION ||
            romfs_delete("firmware") != ROMFS_ERR_OPERATION ||
            romfs_rename("firmware", "firmware.old") != ROMFS_ERR_OPERATION ||
            romfs_create_file("fake-service", &protected_file, ROMFS_MODE_READWRITE, ROMFS_TYPE_FIRMWARE,
                              io_buffer) != ROMFS_ERR_OPERATION) {
        fprintf(stderr, ANSI_COLOR_RED "Service entry mutation was not rejected\n" ANSI_COLOR_RESET);
        success = false;
    }

cleanup:
    romfs_delete("service-guard.bin");
    free(io_buffer);
    free(payload);
    success = romfs_format() && success;
    return success;
}

// Shuffles an array of strings
static void shuffle_filenames(char **array, size_t n)
{
    if (n > 1) {
        for (size_t i = 0; i < n - 1; i++) {
            size_t j = i + test_random() % (n - i);
            char *t = array[j];
            array[j] = array[i];
            array[i] = t;
        }
    }
}

// Gets a list of all user-created files
static int get_file_list(char ***file_list_out)
{
    *file_list_out = NULL;
    int count = 0;
    int capacity = 128;
    char **files = malloc(capacity * sizeof(*files));
    if (!files) {
        fprintf(stderr, "Failed to allocate file list\n");
        return -1;
    }

    romfs_file file = {0};
    uint32_t err = romfs_list(&file, true);
    while (err == ROMFS_NOERR) {
        if (file.entry.attr.names.type == ROMFS_TYPE_MISC) {
            if (count == capacity) {
                int new_capacity = capacity * 2;
                char **new_files = realloc(files, new_capacity * sizeof(*files));
                if (!new_files) {
                    fprintf(stderr, "Failed to expand file list\n");
                    free_file_list(files, count);
                    return -1;
                }
                files = new_files;
                capacity = new_capacity;
            }
            files[count] = strdup(file.entry.name);
            if (!files[count]) {
                fprintf(stderr, "Failed to copy filename\n");
                free_file_list(files, count);
                return -1;
            }
            count++;
        }
        err = romfs_list(&file, false);
    }
    if (err != ROMFS_ERR_NO_FREE_ENTRIES) {
        fprintf(stderr, "Failed to list files: %s\n", romfs_strerror(err));
        free_file_list(files, count);
        return -1;
    }
    *file_list_out = files;
    return count;
}

// Returns the number of complete files, or -1 on an unexpected failure.
// NO_SPACE / NO_FREE_ENTRIES are expected only in these capacity scenarios.
static int fill_drive(const char *prefix, int max_chunks_per_file, int chunk_size, bool random_size)
{
    int file_idx = 0;
    int result = -1;
    uint8_t *test_data = malloc(chunk_size);
    uint8_t *io_buffer = malloc(ROMFS_FLASH_SECTOR);
    if (!test_data || !io_buffer) {
        fprintf(stderr, "Failed to allocate fill buffers\n");
        goto cleanup;
    }

    while (true) {
        char filename[ROMFS_MAX_NAME_LEN];
        snprintf(filename, sizeof(filename), "%s%04d.dat", prefix, file_idx);
        romfs_file file = {0};
        uint32_t err = romfs_create_file(filename, &file, ROMFS_MODE_READWRITE, ROMFS_TYPE_MISC, io_buffer);
        if (err != ROMFS_NOERR) {
            if (err == ROMFS_ERR_NO_FREE_ENTRIES || err == ROMFS_ERR_NO_SPACE) {
                printf("\nCapacity reached after %d files: %s\n", file_idx, romfs_strerror(err));
                result = file_idx;
            } else {
                fprintf(stderr, "Failed to create %s: %s\n", filename, romfs_strerror(err));
            }
            break;
        }

        printf(ANSI_COLOR_MAGENTA "Creating %s... \r" ANSI_COLOR_RESET, filename);
        fflush(stdout);
        int chunks = random_size ? 1 + test_random() % max_chunks_per_file : max_chunks_per_file;
        bool no_space = false;
        bool write_ok = true;
        for (int j = 0; j < chunks; j++) {
            create_test_data(test_data, chunk_size, file_idx, j);
            uint32_t written = romfs_write_file(test_data, chunk_size, &file);
            if (written != (uint32_t)chunk_size || file.err != ROMFS_NOERR) {
                no_space = file.err == ROMFS_ERR_NO_SPACE && written < (uint32_t)chunk_size;
                write_ok = false;
                if (!no_space) {
                    fprintf(stderr, "Write failed for %s: %u/%d bytes, %s\n",
                            filename, written, chunk_size, romfs_strerror(file.err));
                }
                break;
            }
        }
        uint32_t close_err = romfs_close_file(&file);
        if (close_err != ROMFS_NOERR && !(no_space && close_err == ROMFS_ERR_NO_SPACE)) {
            fprintf(stderr, "Close failed for %s: %s\n", filename, romfs_strerror(close_err));
            break;
        }
        if (!write_ok) {
            if (!no_space) {
                break;
            }
            /* A partial file is outside this complete-file verification set.
             * Current ROMFS may not publish it at all after ENOSPC. */
            err = romfs_delete(filename);
            if (err != ROMFS_NOERR && err != ROMFS_ERR_NO_ENTRY) {
                fprintf(stderr, "Failed to discard %s: %s\n", filename, romfs_strerror(err));
                break;
            }
            printf("\nData space exhausted after %d complete files.\n", file_idx);
            result = file_idx;
            break;
        }
        file_idx++;
    }

cleanup:
    free(io_buffer);
    free(test_data);
    return result;
}

// Verifies the contents of all user files on the drive, works with random sizes.
static bool verify_drive(uint32_t chunk_size)
{
    printf(ANSI_COLOR_CYAN "\nVerifying all files...\n" ANSI_COLOR_RESET);
    bool success = true;
    char** file_list = NULL;
    int file_count = get_file_list(&file_list);
    if (file_count <= 0) {
        if (file_list) {
            free(file_list);
        }
        if (file_count == 0) {
            printf("No files to verify.\n");
            return true;
        }
        return false;
    }

    uint8_t* read_buffer = malloc(chunk_size);
    uint8_t* expected_data = malloc(chunk_size);
    if (!read_buffer || !expected_data) {
        free(read_buffer);
        free(expected_data);
        free_file_list(file_list, file_count);
        fprintf(stderr, "Failed to allocate verification buffers\n");
        return false;
    }

    for (int i = 0; i < file_count; i++) {
        printf(ANSI_COLOR_MAGENTA "Verifying %s... \r" ANSI_COLOR_RESET, file_list[i]);
        fflush(stdout);

        int file_idx = -1;
        if (sscanf(file_list[i], "file%d.dat", &file_idx) != 1) {
            if (sscanf(file_list[i], "rfile%d.dat", &file_idx) != 1) {
                if (sscanf(file_list[i], "ifile%d.dat", &file_idx) != 1) {
                    if (sscanf(file_list[i], "rndfile%d.dat", &file_idx) != 1) {
                        sscanf(file_list[i], "sfile%d.dat", &file_idx);
                    }
                }
            }
        }

        if (file_idx < 0) {
            fprintf(stderr, "Unexpected verification filename: %s\n", file_list[i]);
            success = false;
            continue;
        }
        romfs_file file = {0};
        uint8_t *romfs_io_buffer = malloc(ROMFS_FLASH_SECTOR);
        if (!romfs_io_buffer) {
            fprintf(stderr, "Failed to allocate verification IO buffer\n");
            success = false;
            break;
        }
        if (romfs_open_file(file_list[i], &file, romfs_io_buffer) != ROMFS_NOERR) {
            fprintf(stderr, ANSI_COLOR_RED "\nFailed to open file for verification: %s\n" ANSI_COLOR_RESET, file_list[i]);
            success = false;
            free(romfs_io_buffer);
            continue;
        }

        bool file_ok = true;
        uint32_t total_bytes_read = 0;
        int chunk_idx = 0;
        while(total_bytes_read < file.entry.size) {
            uint32_t remaining_bytes = file.entry.size - total_bytes_read;
            uint32_t bytes_to_read = remaining_bytes > chunk_size ? chunk_size : remaining_bytes;

            uint32_t bytes_read = romfs_read_file(read_buffer, bytes_to_read, &file);
            if (bytes_read != bytes_to_read ||
                    (file.err != ROMFS_NOERR && file.err != ROMFS_ERR_EOF)) {
                fprintf(stderr, ANSI_COLOR_RED "\nRead error on %s, chunk %d. Expected %d, got %d\n" ANSI_COLOR_RESET, file_list[i],
                        chunk_idx, bytes_to_read, bytes_read);
                file_ok = false;
                break;
            }

            create_test_data(expected_data, bytes_to_read, file_idx, chunk_idx);
            if (memcmp(read_buffer, expected_data, bytes_to_read) != 0) {
                fprintf(stderr, ANSI_COLOR_RED "\nData mismatch in %s, chunk %d\n" ANSI_COLOR_RESET, file_list[i], chunk_idx);
                file_ok = false;
                break;
            }
            total_bytes_read += bytes_read;
            chunk_idx++;
        }

        if (romfs_close_file(&file) != ROMFS_NOERR) {
            fprintf(stderr, "Close failed for %s\n", file_list[i]);
            file_ok = false;
        }
        free(romfs_io_buffer);
        if (!file_ok) {
            success = false;
        }
    }

    for(int i=0; i<file_count; i++) {
        free(file_list[i]);
    }
    free(file_list);
    free(read_buffer);
    free(expected_data);

    if(success) {
        printf(ANSI_COLOR_GREEN "\nVerification successful. All %d files are correct.\n" ANSI_COLOR_RESET, file_count);
    } else {
        printf(ANSI_COLOR_RED "\nVerification FAILED.\n" ANSI_COLOR_RESET);
    }

    return success;
}

static bool test_large_io_transfer(void)
{
    printf(ANSI_COLOR_YELLOW "\n--- Running Large I/O Transfer Test (> ROMFS_FLASH_SECTOR) ---\n" ANSI_COLOR_RESET);

    if (!romfs_format()) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to format filesystem for large I/O test\n" ANSI_COLOR_RESET);
        return false;
    }

    const uint32_t large_len = (ROMFS_FLASH_SECTOR * 2) + 123;
    uint8_t *write_data = malloc(large_len);
    uint8_t *read_data = malloc(large_len);
    uint8_t *io_buffer = malloc(ROMFS_FLASH_SECTOR);
    bool success = false;

    if (!write_data || !read_data || !io_buffer) {
        fprintf(stderr, ANSI_COLOR_RED "Allocation failure in large I/O test\n" ANSI_COLOR_RESET);
        goto cleanup;
    }

    for (uint32_t i = 0; i < large_len; i++) {
        write_data[i] = (uint8_t)(i ^ 0x5a);
    }

    romfs_file file;
    if (romfs_create_file("large_test.bin", &file, ROMFS_MODE_READWRITE, ROMFS_TYPE_MISC, io_buffer) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to create large_test.bin: %s\n" ANSI_COLOR_RESET, romfs_strerror(file.err));
        goto cleanup;
    }

    uint32_t written = romfs_write_file(write_data, large_len, &file);
    if (written != large_len || file.err != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Write failed for large_test.bin (wrote %u/%u): %s\n" ANSI_COLOR_RESET,
                written, large_len, romfs_strerror(file.err));
        goto cleanup_close_write;
    }

    if (romfs_close_file(&file) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Close failed for large_test.bin after write: %s\n" ANSI_COLOR_RESET,
                romfs_strerror(file.err));
        goto cleanup_close_write;
    }

    romfs_file reader;
    if (romfs_open_file("large_test.bin", &reader, io_buffer) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to reopen large_test.bin: %s\n" ANSI_COLOR_RESET, romfs_strerror(reader.err));
        goto cleanup_close_write;
    }

    uint32_t read_total = romfs_read_file(read_data, large_len, &reader);
    if (read_total != large_len) {
        fprintf(stderr, ANSI_COLOR_RED "Read failed for large_test.bin (read %u/%u): %s\n" ANSI_COLOR_RESET,
                read_total, large_len, romfs_strerror(reader.err));
        success = close_test_file(&reader) && success;
        goto cleanup_close_write;
    }

    if (reader.err != ROMFS_ERR_EOF) {
        fprintf(stderr, ANSI_COLOR_RED "Unexpected reader.err (%u) after large read\n" ANSI_COLOR_RESET, reader.err);
        success = close_test_file(&reader) && success;
        goto cleanup_close_write;
    }

    if (memcmp(write_data, read_data, large_len) != 0) {
        fprintf(stderr, ANSI_COLOR_RED "Data mismatch after large I/O transfer\n" ANSI_COLOR_RESET);
        success = close_test_file(&reader) && success;
        goto cleanup_close_write;
    }

    if (romfs_close_file(&reader) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Close failed for reader handle: %s\n" ANSI_COLOR_RESET, romfs_strerror(reader.err));
        goto cleanup_close_write;
    }

    success = true;

cleanup_close_write:
    romfs_delete("large_test.bin");

cleanup:
    free(write_data);
    free(read_data);
    free(io_buffer);
    success = romfs_format() && success;
    return success;
}

static bool test_seek_tell(void)
{
    printf(ANSI_COLOR_YELLOW "\n--- Running Seek/Tell Test ---\n" ANSI_COLOR_RESET);

    if (!romfs_format()) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to format filesystem for seek/tell test\n" ANSI_COLOR_RESET);
        return false;
    }

    const uint32_t total_len = ROMFS_FLASH_SECTOR * 3 + 321;
    const size_t read_buf_size = 512;

    uint8_t *pattern = malloc(total_len);
    uint8_t *write_io = malloc(ROMFS_FLASH_SECTOR);
    uint8_t *read_io = malloc(ROMFS_FLASH_SECTOR);
    uint8_t *read_buf = malloc(read_buf_size);
    bool success = true;

    if (!pattern || !write_io || !read_io || !read_buf) {
        fprintf(stderr, ANSI_COLOR_RED "Allocation failure in seek/tell test\n" ANSI_COLOR_RESET);
        success = false;
        goto cleanup;
    }

    for (uint32_t i = 0; i < total_len; i++) {
        pattern[i] = (uint8_t)(i & 0xff);
    }

    romfs_file file;
    if (romfs_create_file("seektest.bin", &file, ROMFS_MODE_READWRITE, ROMFS_TYPE_MISC, write_io) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to create seektest.bin: %s\n" ANSI_COLOR_RESET, romfs_strerror(file.err));
        success = false;
        goto cleanup;
    }

    if (romfs_write_file(pattern, total_len, &file) != total_len || romfs_close_file(&file) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to populate seektest.bin: %s\n" ANSI_COLOR_RESET, romfs_strerror(file.err));
        success = false;
        goto cleanup;
    }

    romfs_file reader;
    if (romfs_open_file("seektest.bin", &reader, read_io) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to open seektest.bin for reading: %s\n" ANSI_COLOR_RESET,
                romfs_strerror(reader.err));
        success = false;
        goto cleanup;
    }

    uint32_t pos = 0;
    if (romfs_tell_file(&reader, &pos) != ROMFS_NOERR || pos != 0) {
        fprintf(stderr, ANSI_COLOR_RED "Initial tell mismatch (%u)\n" ANSI_COLOR_RESET, pos);
        success = false;
        goto cleanup_reader;
    }

    const uint32_t first_seek = ROMFS_FLASH_SECTOR + 123;
    if (romfs_seek_file(&reader, (int32_t)first_seek, SEEK_SET) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "SEEK_SET to %u failed: %s\n" ANSI_COLOR_RESET, first_seek, romfs_strerror(reader.err));
        success = false;
        goto cleanup_reader;
    }
    if (romfs_tell_file(&reader, &pos) != ROMFS_NOERR || pos != first_seek) {
        fprintf(stderr, ANSI_COLOR_RED "Tell after SEEK_SET mismatch (%u)\n" ANSI_COLOR_RESET, pos);
        success = false;
        goto cleanup_reader;
    }

    const uint32_t first_read = 200;
    if (romfs_read_file(read_buf, first_read, &reader) != first_read ||
            memcmp(read_buf, &pattern[first_seek], first_read) != 0) {
        fprintf(stderr, ANSI_COLOR_RED "Data mismatch after SEEK_SET read\n" ANSI_COLOR_RESET);
        success = false;
        goto cleanup_reader;
    }

    uint32_t expected_pos = first_seek + first_read;
    if (romfs_tell_file(&reader, &pos) != ROMFS_NOERR || pos != expected_pos) {
        fprintf(stderr, ANSI_COLOR_RED "Tell after first read mismatch (%u)\n" ANSI_COLOR_RESET, pos);
        success = false;
        goto cleanup_reader;
    }

    if (romfs_seek_file(&reader, -50, SEEK_CUR) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "SEEK_CUR -50 failed: %s\n" ANSI_COLOR_RESET, romfs_strerror(reader.err));
        success = false;
        goto cleanup_reader;
    }
    expected_pos -= 50;
    if (romfs_tell_file(&reader, &pos) != ROMFS_NOERR || pos != expected_pos) {
        fprintf(stderr, ANSI_COLOR_RED "Tell after SEEK_CUR mismatch (%u)\n" ANSI_COLOR_RESET, pos);
        success = false;
        goto cleanup_reader;
    }

    const uint32_t second_read = 100;
    if (romfs_read_file(read_buf, second_read, &reader) != second_read ||
            memcmp(read_buf, &pattern[expected_pos], second_read) != 0) {
        fprintf(stderr, ANSI_COLOR_RED "Data mismatch after SEEK_CUR read\n" ANSI_COLOR_RESET);
        success = false;
        goto cleanup_reader;
    }
    expected_pos += second_read;

    if (romfs_seek_file(&reader, -128, SEEK_END) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "SEEK_END -128 failed: %s\n" ANSI_COLOR_RESET, romfs_strerror(reader.err));
        success = false;
        goto cleanup_reader;
    }
    expected_pos = total_len - 128;
    if (romfs_tell_file(&reader, &pos) != ROMFS_NOERR || pos != expected_pos) {
        fprintf(stderr, ANSI_COLOR_RED "Tell after SEEK_END mismatch (%u)\n" ANSI_COLOR_RESET, pos);
        success = false;
        goto cleanup_reader;
    }

    const uint32_t third_read = 64;
    if (romfs_read_file(read_buf, third_read, &reader) != third_read ||
            memcmp(read_buf, &pattern[expected_pos], third_read) != 0) {
        fprintf(stderr, ANSI_COLOR_RED "Data mismatch after SEEK_END read\n" ANSI_COLOR_RESET);
        success = false;
        goto cleanup_reader;
    }
    expected_pos += third_read;

    if (romfs_seek_file(&reader, 0, SEEK_END) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "SEEK_END 0 failed: %s\n" ANSI_COLOR_RESET, romfs_strerror(reader.err));
        success = false;
        goto cleanup_reader;
    }
    if (romfs_read_file(read_buf, 16, &reader) != 0 || reader.err != ROMFS_ERR_EOF) {
        fprintf(stderr, ANSI_COLOR_RED "EOF read after SEEK_END did not behave as expected\n" ANSI_COLOR_RESET);
        success = false;
        goto cleanup_reader;
    }

cleanup_reader:
    success = close_test_file(&reader) && success;

    if (success) {
        romfs_file empty_file;
        if (romfs_create_file("empty_seek.bin", &file, ROMFS_MODE_READWRITE, ROMFS_TYPE_MISC, write_io) != ROMFS_NOERR) {
            fprintf(stderr, ANSI_COLOR_RED "Failed to create empty_seek.bin: %s\n" ANSI_COLOR_RESET, romfs_strerror(file.err));
            success = false;
        } else if (romfs_close_file(&file) != ROMFS_NOERR) {
            fprintf(stderr, ANSI_COLOR_RED "Failed to close empty_seek.bin after creation: %s\n" ANSI_COLOR_RESET,
                    romfs_strerror(file.err));
            success = false;
        } else if (romfs_open_file("empty_seek.bin", &empty_file, read_io) != ROMFS_NOERR) {
            fprintf(stderr, ANSI_COLOR_RED "Failed to open empty_seek.bin: %s\n" ANSI_COLOR_RESET, romfs_strerror(empty_file.err));
            success = false;
        } else {
            if (romfs_tell_file(&empty_file, &pos) != ROMFS_NOERR || pos != 0) {
                fprintf(stderr, ANSI_COLOR_RED "Empty file tell mismatch\n" ANSI_COLOR_RESET);
                success = false;
            }
            if (success && romfs_seek_file(&empty_file, 0, SEEK_SET) != ROMFS_NOERR) {
                fprintf(stderr, ANSI_COLOR_RED "Empty file SEEK_SET failed\n" ANSI_COLOR_RESET);
                success = false;
            }
            if (success && romfs_seek_file(&empty_file, 0, SEEK_END) != ROMFS_NOERR) {
                fprintf(stderr, ANSI_COLOR_RED "Empty file SEEK_END failed\n" ANSI_COLOR_RESET);
                success = false;
            }
            if (success && romfs_seek_file(&empty_file, 1, SEEK_SET) == ROMFS_NOERR) {
                fprintf(stderr, ANSI_COLOR_RED "Empty file SEEK_SET beyond end unexpectedly succeeded\n" ANSI_COLOR_RESET);
                success = false;
            }
            if (success && romfs_seek_file(&empty_file, -1, SEEK_CUR) == ROMFS_NOERR) {
                fprintf(stderr, ANSI_COLOR_RED "Empty file SEEK_CUR negative unexpectedly succeeded\n" ANSI_COLOR_RESET);
                success = false;
            }
            success = close_test_file(&empty_file) && success;
        }
    }

cleanup:
    success = romfs_format() && success;
    free(pattern);
    free(write_io);
    free(read_io);
    free(read_buf);
    return success;
}

static bool test_append_mode(void)
{
    printf(ANSI_COLOR_YELLOW "\n--- Running Append Mode Test ---\n" ANSI_COLOR_RESET);

    if (!romfs_format()) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to format filesystem for append test\n" ANSI_COLOR_RESET);
        return false;
    }

    const char part1[] = "append-one-";
    const char part2[] = "append-two-extended";
    const size_t part1_len = strlen(part1);
    const size_t part2_len = strlen(part2);
    const size_t big_len = ROMFS_FLASH_SECTOR + 137;
    const size_t total_expected = part1_len + part2_len + big_len;

    uint8_t *io_buffer = malloc(ROMFS_FLASH_SECTOR);
    uint8_t *read_io_buffer = malloc(ROMFS_FLASH_SECTOR);
    uint8_t *big_data = malloc(big_len);
    uint8_t *read_buffer = malloc(total_expected);
    uint8_t *expected = malloc(total_expected);

    if (!io_buffer || !read_io_buffer || !big_data || !read_buffer || !expected) {
        fprintf(stderr, ANSI_COLOR_RED "Allocation failure in append test\n" ANSI_COLOR_RESET);
        free(io_buffer);
        free(read_io_buffer);
        free(big_data);
        free(read_buffer);
        free(expected);
        return false;
    }

    create_test_data(big_data, big_len, 77, 0);

    bool success = true;
    uint32_t pos = 0;
    romfs_file file = {0};
    romfs_file reader = {0};

    /* Step 1: create via append and write first chunk */
    if (romfs_open_append_path("append.bin", &file, ROMFS_TYPE_MISC, io_buffer, false) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to open append.bin for append: %s\n" ANSI_COLOR_RESET, romfs_strerror(file.err));
        success = false;
        goto cleanup;
    }

    if (romfs_write_file(part1, part1_len, &file) != part1_len) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to write first append chunk\n" ANSI_COLOR_RESET);
        success = false;
        goto cleanup_close_file;
    }

    if (romfs_tell_file(&file, &pos) != ROMFS_NOERR || pos != part1_len) {
        fprintf(stderr, ANSI_COLOR_RED "tell mismatch after initial append (got %u, expected %zu)\n" ANSI_COLOR_RESET, pos,
                part1_len);
        success = false;
        goto cleanup_close_file;
    }

cleanup_close_file:
    if (romfs_close_file(&file) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to close append.bin after first write: %s\n" ANSI_COLOR_RESET,
                romfs_strerror(file.err));
        success = false;
    }
    if (!success) {
        goto cleanup;
    }

    /* Verify initial contents */
    memset(&reader, 0, sizeof(reader));
    if (romfs_open_file("append.bin", &reader, read_io_buffer) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to reopen append.bin for verification: %s\n" ANSI_COLOR_RESET,
                romfs_strerror(reader.err));
        success = false;
        goto cleanup;
    }
    if (romfs_read_file(read_buffer, part1_len, &reader) != part1_len ||
            memcmp(read_buffer, part1, part1_len) != 0) {
        fprintf(stderr, ANSI_COLOR_RED "Initial append verification failed\n" ANSI_COLOR_RESET);
        success = false;
    }
    success = close_test_file(&reader) && success;
    if (!success) {
        goto cleanup;
    }

    /* Step 2: append second chunk within same sector */
    memset(&file, 0, sizeof(file));
    if (romfs_open_append_path("append.bin", &file, ROMFS_TYPE_MISC, io_buffer, false) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to reopen append.bin for second append: %s\n" ANSI_COLOR_RESET,
                romfs_strerror(file.err));
        success = false;
        goto cleanup;
    }

    if (romfs_tell_file(&file, &pos) != ROMFS_NOERR || pos != part1_len) {
        fprintf(stderr, ANSI_COLOR_RED "tell mismatch before second append (got %u)\n" ANSI_COLOR_RESET, pos);
        success = false;
        goto cleanup_second_close;
    }

    if (romfs_write_file(part2, part2_len, &file) != part2_len) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to write second append chunk\n" ANSI_COLOR_RESET);
        success = false;
        goto cleanup_second_close;
    }

    if (romfs_tell_file(&file, &pos) != ROMFS_NOERR || pos != part1_len + part2_len) {
        fprintf(stderr, ANSI_COLOR_RED "tell mismatch after second append (got %u, expected %zu)\n" ANSI_COLOR_RESET, pos,
                part1_len + part2_len);
        success = false;
        goto cleanup_second_close;
    }

cleanup_second_close:
    if (romfs_close_file(&file) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to close append.bin after second write: %s\n" ANSI_COLOR_RESET,
                romfs_strerror(file.err));
        success = false;
    }
    if (!success) {
        goto cleanup;
    }

    /* Verify combined content */
    memcpy(expected, part1, part1_len);
    memcpy(expected + part1_len, part2, part2_len);
    memset(&reader, 0, sizeof(reader));
    if (romfs_open_file("append.bin", &reader, read_io_buffer) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to reopen append.bin for second verification: %s\n" ANSI_COLOR_RESET,
                romfs_strerror(reader.err));
        success = false;
        goto cleanup;
    }
    size_t current_size = part1_len + part2_len;
    if (romfs_read_file(read_buffer, current_size, &reader) != current_size ||
            memcmp(read_buffer, expected, current_size) != 0) {
        fprintf(stderr, ANSI_COLOR_RED "Second append verification failed\n" ANSI_COLOR_RESET);
        success = false;
    }
    success = close_test_file(&reader) && success;
    if (!success) {
        goto cleanup;
    }

    /* Step 3: append data crossing sector boundary */
    memset(&file, 0, sizeof(file));
    if (romfs_open_append_path("append.bin", &file, ROMFS_TYPE_MISC, io_buffer, false) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to reopen append.bin for large append: %s\n" ANSI_COLOR_RESET,
                romfs_strerror(file.err));
        success = false;
        goto cleanup;
    }

    if (romfs_tell_file(&file, &pos) != ROMFS_NOERR || pos != current_size) {
        fprintf(stderr, ANSI_COLOR_RED "tell mismatch before large append (got %u, expected %zu)\n" ANSI_COLOR_RESET, pos,
                current_size);
        success = false;
        goto cleanup_large_close;
    }

    if (romfs_write_file(big_data, big_len, &file) != big_len) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to write large append payload\n" ANSI_COLOR_RESET);
        success = false;
        goto cleanup_large_close;
    }

    if (romfs_tell_file(&file, &pos) != ROMFS_NOERR || pos != current_size + big_len) {
        fprintf(stderr, ANSI_COLOR_RED "tell mismatch after large append (got %u, expected %zu)\n" ANSI_COLOR_RESET, pos,
                current_size + big_len);
        success = false;
        goto cleanup_large_close;
    }

cleanup_large_close:
    if (romfs_close_file(&file) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to close append.bin after large append: %s\n" ANSI_COLOR_RESET,
                romfs_strerror(file.err));
        success = false;
    }
    if (!success) {
        goto cleanup;
    }

    /* Verify final content */
    memcpy(expected + current_size, big_data, big_len);
    memset(&reader, 0, sizeof(reader));
    if (romfs_open_file("append.bin", &reader, read_io_buffer) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to open append.bin for final verification: %s\n" ANSI_COLOR_RESET,
                romfs_strerror(reader.err));
        success = false;
        goto cleanup;
    }
    if (romfs_read_file(read_buffer, total_expected, &reader) != total_expected ||
            memcmp(read_buffer, expected, total_expected) != 0) {
        fprintf(stderr, ANSI_COLOR_RED "Final append verification failed\n" ANSI_COLOR_RESET);
        success = false;
    }
    success = close_test_file(&reader) && success;
    if (!success) {
        goto cleanup;
    }

    /* Flat namespace append helper */
    memset(&file, 0, sizeof(file));
    if (romfs_open_append("flat.bin", &file, ROMFS_TYPE_MISC, io_buffer) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to open flat.bin via romfs_open_append: %s\n" ANSI_COLOR_RESET,
                romfs_strerror(file.err));
        success = false;
        goto cleanup;
    }
    if (romfs_write_file(part1, part1_len, &file) != part1_len || romfs_close_file(&file) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to use romfs_open_append in flat mode\n" ANSI_COLOR_RESET);
        success = false;
        goto cleanup;
    }
    if (romfs_open_file("flat.bin", &reader, io_buffer) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to reopen flat.bin after append\n" ANSI_COLOR_RESET);
        success = false;
        goto cleanup;
    }
    if (romfs_read_file(read_buffer, part1_len, &reader) != part1_len ||
            memcmp(read_buffer, part1, part1_len) != 0) {
        fprintf(stderr, ANSI_COLOR_RED "Verification of flat.bin append failed\n" ANSI_COLOR_RESET);
        success = false;
    }
    success = close_test_file(&reader) && success;
    if (!success) {
        goto cleanup;
    }

    /* Ensure create_dirs parameter works */
    memset(&file, 0, sizeof(file));
    if (romfs_open_append_path("logs/session/log.txt", &file, ROMFS_TYPE_MISC, io_buffer, true) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to append with implicit directory creation: %s\n" ANSI_COLOR_RESET,
                romfs_strerror(file.err));
        success = false;
        goto cleanup;
    }
    if (romfs_close_file(&file) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to close log.txt in append test: %s\n" ANSI_COLOR_RESET,
                romfs_strerror(file.err));
        success = false;
        goto cleanup;
    }
    romfs_delete_path("logs/session/log.txt");
    romfs_rmdir_path("logs/session");
    romfs_rmdir_path("logs");

cleanup:
    romfs_delete_path("append.bin");
    romfs_delete("flat.bin");
    success = romfs_format() && success;
    free(io_buffer);
    free(read_io_buffer);
    free(big_data);
    free(read_buffer);
    free(expected);
    return success;
}

static bool test_truncate_api(void)
{
    printf(ANSI_COLOR_YELLOW "\n--- Running Truncate API Test ---\n" ANSI_COLOR_RESET);

    if (!romfs_format()) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to format filesystem for truncate test\n" ANSI_COLOR_RESET);
        return false;
    }

    const uint32_t initial_len = ROMFS_FLASH_SECTOR * 2 + 123;
    const uint32_t shrink_len = ROMFS_FLASH_SECTOR + 77;
    const uint32_t extend_len = ROMFS_FLASH_SECTOR * 3 + 15;

    uint8_t *io_buffer = malloc(ROMFS_FLASH_SECTOR);
    uint8_t *read_io_buffer = malloc(ROMFS_FLASH_SECTOR);
    uint8_t *payload = malloc(initial_len);
    uint8_t *read_buffer = malloc(extend_len);
    if (!io_buffer || !read_io_buffer || !payload || !read_buffer) {
        fprintf(stderr, ANSI_COLOR_RED "Allocation failure in truncate test\n" ANSI_COLOR_RESET);
        free(io_buffer);
        free(read_io_buffer);
        free(payload);
        free(read_buffer);
        return false;
    }

    create_test_data(payload, initial_len, 91, 3);

    bool success = true;
    romfs_file file = {0};
    romfs_file reader = {0};

    if (romfs_create_file("truncate.bin", &file, ROMFS_MODE_READWRITE, ROMFS_TYPE_MISC, io_buffer) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to create truncate.bin: %s\n" ANSI_COLOR_RESET, romfs_strerror(file.err));
        success = false;
        goto cleanup;
    }
    if (romfs_write_file(payload, initial_len, &file) != initial_len ||
            romfs_close_file(&file) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to populate truncate.bin: %s\n" ANSI_COLOR_RESET, romfs_strerror(file.err));
        success = false;
        goto cleanup;
    }

    memset(&file, 0, sizeof(file));
    if (romfs_open_append("truncate.bin", &file, ROMFS_TYPE_MISC, io_buffer) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to open truncate.bin for shrink: %s\n" ANSI_COLOR_RESET,
                romfs_strerror(file.err));
        success = false;
        goto cleanup;
    }
    if (romfs_truncate_file(&file, shrink_len) != ROMFS_NOERR ||
            romfs_close_file(&file) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Shrink truncate failed: %s\n" ANSI_COLOR_RESET, romfs_strerror(file.err));
        success = false;
        goto cleanup;
    }

    if (romfs_open_file("truncate.bin", &reader, read_io_buffer) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to open truncated file: %s\n" ANSI_COLOR_RESET, romfs_strerror(reader.err));
        success = false;
        goto cleanup;
    }
    if (reader.entry.size != shrink_len ||
            romfs_read_file(read_buffer, shrink_len, &reader) != shrink_len ||
            memcmp(read_buffer, payload, shrink_len) != 0 ||
            romfs_read_file(read_buffer, 1, &reader) != 0 ||
            reader.err != ROMFS_ERR_EOF) {
        fprintf(stderr, ANSI_COLOR_RED "Shrink truncate verification failed\n" ANSI_COLOR_RESET);
        success = false;
    }
    success = close_test_file(&reader) && success;
    if (!success) {
        goto cleanup;
    }

    memset(&file, 0, sizeof(file));
    if (romfs_open_append("truncate.bin", &file, ROMFS_TYPE_MISC, io_buffer) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to open truncate.bin for extend: %s\n" ANSI_COLOR_RESET,
                romfs_strerror(file.err));
        success = false;
        goto cleanup;
    }
    if (romfs_truncate_file(&file, extend_len) != ROMFS_NOERR ||
            romfs_close_file(&file) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Extend truncate failed: %s\n" ANSI_COLOR_RESET, romfs_strerror(file.err));
        success = false;
        goto cleanup;
    }

    memset(&reader, 0, sizeof(reader));
    if (romfs_open_file("truncate.bin", &reader, read_io_buffer) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to reopen extended file: %s\n" ANSI_COLOR_RESET, romfs_strerror(reader.err));
        success = false;
        goto cleanup;
    }
    if (reader.entry.size != extend_len ||
            romfs_read_file(read_buffer, extend_len, &reader) != extend_len ||
            memcmp(read_buffer, payload, shrink_len) != 0) {
        fprintf(stderr, ANSI_COLOR_RED "Extend truncate prefix verification failed\n" ANSI_COLOR_RESET);
        success = false;
    }
    for (uint32_t i = shrink_len; success && i < extend_len; i++) {
        if (read_buffer[i] != 0) {
            fprintf(stderr, ANSI_COLOR_RED "Extend truncate did not zero-fill at byte %u\n" ANSI_COLOR_RESET, i);
            success = false;
        }
    }
    success = close_test_file(&reader) && success;
    if (!success) {
        goto cleanup;
    }

    memset(&file, 0, sizeof(file));
    if (romfs_open_append("truncate.bin", &file, ROMFS_TYPE_MISC, io_buffer) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to open truncate.bin for zero truncate: %s\n" ANSI_COLOR_RESET,
                romfs_strerror(file.err));
        success = false;
        goto cleanup;
    }
    if (romfs_truncate_file(&file, 0) != ROMFS_NOERR ||
            romfs_close_file(&file) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Zero truncate failed: %s\n" ANSI_COLOR_RESET, romfs_strerror(file.err));
        success = false;
        goto cleanup;
    }

    memset(&reader, 0, sizeof(reader));
    if (romfs_open_file("truncate.bin", &reader, read_io_buffer) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to reopen zero-truncated file: %s\n" ANSI_COLOR_RESET,
                romfs_strerror(reader.err));
        success = false;
        goto cleanup;
    }
    if (reader.entry.size != 0 || romfs_read_file(read_buffer, 1, &reader) != 0 ||
            reader.err != ROMFS_ERR_EOF) {
        fprintf(stderr, ANSI_COLOR_RED "Zero truncate verification failed\n" ANSI_COLOR_RESET);
        success = false;
    }
    success = close_test_file(&reader) && success;

cleanup:
    romfs_delete("truncate.bin");
    success = romfs_format() && success;
    free(io_buffer);
    free(read_io_buffer);
    free(payload);
    free(read_buffer);
    return success;
}

static bool test_random_write_api(void)
{
    printf(ANSI_COLOR_YELLOW "\n--- Running Random Write API Test ---\n" ANSI_COLOR_RESET);

    if (!romfs_format()) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to format filesystem for random write test\n" ANSI_COLOR_RESET);
        return false;
    }

    const uint32_t initial_len = ROMFS_FLASH_SECTOR * 2 + 123;
    const uint32_t patch_a_offset = 100;
    const uint32_t patch_a_len = 300;
    const uint32_t patch_b_offset = ROMFS_FLASH_SECTOR - 20;
    const uint32_t patch_b_len = 80;
    const uint32_t patch_c_offset = initial_len + 97;
    const uint32_t patch_c_len = 123;
    const uint32_t final_len = patch_c_offset + patch_c_len;

    uint8_t *io_buffer = malloc(ROMFS_FLASH_SECTOR);
    uint8_t *read_io_buffer = malloc(ROMFS_FLASH_SECTOR);
    uint8_t *initial = malloc(initial_len);
    uint8_t *expected = malloc(final_len);
    uint8_t *read_buffer = malloc(final_len);
    uint8_t *patch_a = malloc(patch_a_len);
    uint8_t *patch_b = malloc(patch_b_len);
    uint8_t *patch_c = malloc(patch_c_len);
    if (!io_buffer || !read_io_buffer || !initial || !expected || !read_buffer ||
            !patch_a || !patch_b || !patch_c) {
        fprintf(stderr, ANSI_COLOR_RED "Allocation failure in random write test\n" ANSI_COLOR_RESET);
        free(io_buffer);
        free(read_io_buffer);
        free(initial);
        free(expected);
        free(read_buffer);
        free(patch_a);
        free(patch_b);
        free(patch_c);
        return false;
    }

    create_test_data(initial, initial_len, 41, 1);
    create_test_data(patch_a, patch_a_len, 42, 2);
    create_test_data(patch_b, patch_b_len, 43, 3);
    create_test_data(patch_c, patch_c_len, 44, 4);
    memset(expected, 0, final_len);
    memcpy(expected, initial, initial_len);
    memcpy(&expected[patch_a_offset], patch_a, patch_a_len);
    memcpy(&expected[patch_b_offset], patch_b, patch_b_len);
    memcpy(&expected[patch_c_offset], patch_c, patch_c_len);

    bool success = true;
    romfs_file file = {0};
    romfs_file reader = {0};

    if (romfs_create_file("random-write.bin", &file, ROMFS_MODE_READWRITE, ROMFS_TYPE_MISC, io_buffer) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to create random-write.bin: %s\n" ANSI_COLOR_RESET, romfs_strerror(file.err));
        success = false;
        goto cleanup;
    }
    if (romfs_write_file(initial, initial_len, &file) != initial_len ||
            romfs_close_file(&file) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to populate random-write.bin: %s\n" ANSI_COLOR_RESET, romfs_strerror(file.err));
        success = false;
        goto cleanup;
    }

    memset(&file, 0, sizeof(file));
    if (romfs_open_append("random-write.bin", &file, ROMFS_TYPE_MISC, io_buffer) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to open random-write.bin for writing: %s\n" ANSI_COLOR_RESET,
                romfs_strerror(file.err));
        success = false;
        goto cleanup;
    }

    if (romfs_seek_file(&file, patch_a_offset, SEEK_SET) != ROMFS_NOERR ||
            romfs_write_file(patch_a, patch_a_len, &file) != patch_a_len) {
        fprintf(stderr, ANSI_COLOR_RED "In-sector random write failed: %s\n" ANSI_COLOR_RESET, romfs_strerror(file.err));
        success = false;
        goto cleanup_close_file;
    }

    uint32_t pos = 0;
    if (romfs_tell_file(&file, &pos) != ROMFS_NOERR || pos != patch_a_offset + patch_a_len) {
        fprintf(stderr, ANSI_COLOR_RED "tell mismatch after first random write (%u)\n" ANSI_COLOR_RESET, pos);
        success = false;
        goto cleanup_close_file;
    }

    if (romfs_seek_file(&file, patch_b_offset, SEEK_SET) != ROMFS_NOERR ||
            romfs_write_file(patch_b, patch_b_len, &file) != patch_b_len) {
        fprintf(stderr, ANSI_COLOR_RED "Cross-sector random write failed: %s\n" ANSI_COLOR_RESET, romfs_strerror(file.err));
        success = false;
        goto cleanup_close_file;
    }

    if (romfs_seek_file(&file, patch_c_offset, SEEK_SET) != ROMFS_NOERR ||
            romfs_write_file(patch_c, patch_c_len, &file) != patch_c_len) {
        fprintf(stderr, ANSI_COLOR_RED "Sparse random write failed: %s\n" ANSI_COLOR_RESET, romfs_strerror(file.err));
        success = false;
        goto cleanup_close_file;
    }

cleanup_close_file:
    if (romfs_close_file(&file) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to close random-write.bin: %s\n" ANSI_COLOR_RESET, romfs_strerror(file.err));
        success = false;
    }
    if (!success) {
        goto cleanup;
    }

    if (romfs_open_file("random-write.bin", &reader, read_io_buffer) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to reopen random-write.bin: %s\n" ANSI_COLOR_RESET, romfs_strerror(reader.err));
        success = false;
        goto cleanup;
    }
    if (reader.entry.size != final_len ||
            romfs_read_file(read_buffer, final_len, &reader) != final_len ||
            memcmp(read_buffer, expected, final_len) != 0) {
        fprintf(stderr, ANSI_COLOR_RED "Random write verification failed\n" ANSI_COLOR_RESET);
        success = false;
    }
    success = close_test_file(&reader) && success;

cleanup:
    romfs_delete("random-write.bin");
    success = romfs_format() && success;
    free(io_buffer);
    free(read_io_buffer);
    free(initial);
    free(expected);
    free(read_buffer);
    free(patch_a);
    free(patch_b);
    free(patch_c);
    return success;
}

static bool test_rename_api(void)
{
    printf(ANSI_COLOR_YELLOW "\n--- Running Rename API Test ---\n" ANSI_COLOR_RESET);

    if (!romfs_format()) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to format filesystem for rename test\n" ANSI_COLOR_RESET);
        return false;
    }

    uint8_t *io_buffer = malloc(ROMFS_FLASH_SECTOR);
    uint8_t *read_buffer = malloc(64);
    if (!io_buffer || !read_buffer) {
        fprintf(stderr, ANSI_COLOR_RED "Allocation failure in rename test\n" ANSI_COLOR_RESET);
        free(io_buffer);
        free(read_buffer);
        return false;
    }

    bool success = true;
    bool reader_open = false;
    const uint8_t payload[] = { 0xaa, 0xbb, 0xcc, 0xdd };

    romfs_dir root;
    romfs_dir_root(&root);

    romfs_dir alpha, beta;
    if (romfs_dir_create(&root, "alpha", &alpha) != ROMFS_NOERR ||
            romfs_dir_create(&root, "beta", &beta) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to create test directories for rename test\n" ANSI_COLOR_RESET);
        success = false;
        goto cleanup;
    }

    romfs_file file;
    if (romfs_create_file_in_dir(&alpha, "note.bin", &file, ROMFS_MODE_READWRITE, ROMFS_TYPE_MISC,
                                 io_buffer) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to create initial file for rename test\n" ANSI_COLOR_RESET);
        success = false;
        goto cleanup;
    }

    if (romfs_write_file(payload, sizeof(payload), &file) != sizeof(payload) ||
            romfs_close_file(&file) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to populate note.bin\n" ANSI_COLOR_RESET);
        success = false;
        goto cleanup;
    }

    if (romfs_rename_in_dir(&alpha, "note.bin", &alpha, "memo.bin") != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Rename within directory failed\n" ANSI_COLOR_RESET);
        success = false;
        goto cleanup;
    }

    romfs_file reader;
    if (romfs_open_file_in_dir(&alpha, "memo.bin", &reader, io_buffer) != ROMFS_NOERR ||
            (reader_open = true, romfs_read_file(read_buffer, sizeof(payload), &reader) != sizeof(payload)) ||
            memcmp(read_buffer, payload, sizeof(payload)) != 0) {
        fprintf(stderr, ANSI_COLOR_RED "Verification after rename within directory failed\n" ANSI_COLOR_RESET);
        success = false;
        goto cleanup_reader;
    }

    success = close_test_file(&reader) && success;
    reader_open = false;

    if (romfs_open_file_in_dir(&alpha, "note.bin", &reader, io_buffer) != ROMFS_ERR_NO_ENTRY) {
        fprintf(stderr, ANSI_COLOR_RED "Old name unexpectedly accessible after rename\n" ANSI_COLOR_RESET);
        success = false;
        goto cleanup;
    }

    if (romfs_rename_in_dir(&alpha, "memo.bin", &beta, "memo.bin") != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Rename across directories failed\n" ANSI_COLOR_RESET);
        success = false;
        goto cleanup;
    }

    if (romfs_open_file_in_dir(&beta, "memo.bin", &reader, io_buffer) != ROMFS_NOERR ||
            (reader_open = true, romfs_read_file(read_buffer, sizeof(payload), &reader) != sizeof(payload)) ||
            memcmp(read_buffer, payload, sizeof(payload)) != 0) {
        fprintf(stderr, ANSI_COLOR_RED "Verification after cross-directory rename failed\n" ANSI_COLOR_RESET);
        success = false;
        goto cleanup_reader;
    }
    success = close_test_file(&reader) && success;
    reader_open = false;

    // Conflict detection
    if (romfs_create_file_in_dir(&beta, "other.bin", &file, ROMFS_MODE_READWRITE, ROMFS_TYPE_MISC,
                                 io_buffer) != ROMFS_NOERR ||
            romfs_close_file(&file) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to create conflict file for rename test\n" ANSI_COLOR_RESET);
        success = false;
        goto cleanup;
    }
    uint32_t err = romfs_rename_in_dir(&beta, "memo.bin", &beta, "other.bin");
    if (err != ROMFS_ERR_FILE_EXISTS) {
        fprintf(stderr, ANSI_COLOR_RED "Rename conflict did not report ROMFS_ERR_FILE_EXISTS (got %u)\n" ANSI_COLOR_RESET, err);
        success = false;
        goto cleanup;
    }

    // Path-based rename with directory creation
    err = romfs_rename_path("/beta/memo.bin", "/logs/session/archive.bin", true);
    if (err != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Path rename with create_dirs failed: %s\n" ANSI_COLOR_RESET, romfs_strerror(err));
        success = false;
        goto cleanup;
    }

    if (romfs_open_path("/logs/session/archive.bin", &reader, io_buffer) != ROMFS_NOERR ||
            (reader_open = true, romfs_read_file(read_buffer, sizeof(payload), &reader) != sizeof(payload)) ||
            memcmp(read_buffer, payload, sizeof(payload)) != 0) {
        fprintf(stderr, ANSI_COLOR_RED "Verification after path rename failed\n" ANSI_COLOR_RESET);
        success = false;
        goto cleanup_reader;
    }
    success = close_test_file(&reader) && success;
    reader_open = false;

    // Flat namespace rename helper
    memset(&file, 0, sizeof(file));
    if (romfs_create_file("flat_rename.bin", &file, ROMFS_MODE_READWRITE, ROMFS_TYPE_MISC, io_buffer) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to create flat_rename.bin\n" ANSI_COLOR_RESET);
        success = false;
        goto cleanup;
    }
    if (romfs_write_file(payload, sizeof(payload), &file) != sizeof(payload) ||
            romfs_close_file(&file) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to populate flat_rename.bin\n" ANSI_COLOR_RESET);
        success = false;
        goto cleanup;
    }

    if (romfs_rename("flat_rename.bin", "flat_renamed.bin") != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "romfs_rename failed in flat mode\n" ANSI_COLOR_RESET);
        success = false;
        goto cleanup;
    }

    if (romfs_open_file("flat_renamed.bin", &reader, io_buffer) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to open flat_renamed.bin\n" ANSI_COLOR_RESET);
        success = false;
        goto cleanup;
    }
    if (romfs_read_file(read_buffer, sizeof(payload), &reader) != sizeof(payload) ||
            memcmp(read_buffer, payload, sizeof(payload)) != 0) {
        fprintf(stderr, ANSI_COLOR_RED "Verification of romfs_rename target failed\n" ANSI_COLOR_RESET);
        success = false;
    }
    success = close_test_file(&reader) && success;
    if (!success) {
        goto cleanup;
    }

    if (romfs_open_file("flat_rename.bin", &reader, io_buffer) != ROMFS_ERR_NO_ENTRY) {
        fprintf(stderr, ANSI_COLOR_RED "Old name still accessible after romfs_rename\n" ANSI_COLOR_RESET);
        success = false;
        goto cleanup;
    }

    // Directory rename checks
    romfs_dir project;
    if (romfs_dir_create(&root, "project", &project) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to create project directory\n" ANSI_COLOR_RESET);
        success = false;
        goto cleanup;
    }
    romfs_dir stage;
    if (romfs_dir_create(&project, "stage", &stage) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to create stage directory\n" ANSI_COLOR_RESET);
        success = false;
        goto cleanup;
    }

    if (romfs_rename_in_dir(&project, "stage", &root, "stage_root") != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Directory rename to root failed\n" ANSI_COLOR_RESET);
        success = false;
        goto cleanup;
    }

    romfs_dir stage_root;
    if (romfs_dir_open(&root, "stage_root", &stage_root) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Moved directory not accessible after rename\n" ANSI_COLOR_RESET);
        success = false;
        goto cleanup;
    }

    romfs_dir nested;
    if (romfs_dir_create(&project, "nested", &nested) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to create nested directory\n" ANSI_COLOR_RESET);
        success = false;
        goto cleanup;
    }

    err = romfs_rename_in_dir(&root, "project", &nested, "project");
    if (err != ROMFS_ERR_DIR_INVALID) {
        fprintf(stderr, ANSI_COLOR_RED "Illegal directory move did not fail as expected (err=%u)\n" ANSI_COLOR_RESET, err);
        success = false;
        goto cleanup;
    }

cleanup_reader:
    if (reader_open) {
        success = close_test_file(&reader) && success;
        reader_open = false;
    }

cleanup:
    romfs_delete_path("logs/session/archive.bin");
    romfs_rmdir_path("logs/session");
    romfs_rmdir_path("logs");
    romfs_delete_path("/beta/other.bin");
    romfs_delete("flat_renamed.bin");
    free(io_buffer);
    free(read_buffer);
    success = romfs_format() && success;
    return success;
}

static bool test_random_fill_to_capacity(void)
{
    printf(ANSI_COLOR_YELLOW "\n--- Running Random Fill-to-Capacity Test ---\n" ANSI_COLOR_RESET);

    if (!romfs_format()) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to format filesystem for random fill test\n" ANSI_COLOR_RESET);
        return false;
    }

    uint32_t initial_free = romfs_free();
    if (initial_free == 0) {
        fprintf(stderr, ANSI_COLOR_RED "Filesystem reports zero free space\n" ANSI_COLOR_RESET);
        return false;
    }

    uint32_t max_file_size = initial_free / 16;
    if (max_file_size == 0) {
        max_file_size = 1;
    }

    random_fill_entry *entries = NULL;
    size_t entry_count = 0;
    size_t entry_capacity = 0;

    const uint32_t chunk_cap = ROMFS_FLASH_SECTOR;
    uint8_t *chunk_buffer = malloc(chunk_cap);
    if (!chunk_buffer) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to allocate chunk buffer\n" ANSI_COLOR_RESET);
        return false;
    }

    bool success = true;
    int file_idx = 0;

    while (romfs_free() > 0) {
        char filename[ROMFS_MAX_NAME_LEN];
        snprintf(filename, sizeof(filename), "randfill%05d.bin", file_idx);

        uint32_t file_size = (uint32_t)(test_random() % (max_file_size + 1));

        romfs_file file;
        uint8_t *io_buffer = malloc(ROMFS_FLASH_SECTOR);
        if (!io_buffer) {
            fprintf(stderr, ANSI_COLOR_RED "Failed to allocate IO buffer for %s\n" ANSI_COLOR_RESET, filename);
            success = false;
            break;
        }

        if (romfs_create_file(filename, &file, ROMFS_MODE_READWRITE, ROMFS_TYPE_MISC, io_buffer) != ROMFS_NOERR) {
            if (file.err == ROMFS_ERR_NO_SPACE || file.err == ROMFS_ERR_NO_FREE_ENTRIES) {
                printf(ANSI_COLOR_YELLOW "Stopping creation at %d files (filesystem full)\n" ANSI_COLOR_RESET, file_idx);
            } else {
                fprintf(stderr, ANSI_COLOR_RED "Failed to create %s: %s\n" ANSI_COLOR_RESET, filename, romfs_strerror(file.err));
                success = false;
            }
            free(io_buffer);
            break;
        }

        bool write_ok = true;
        bool no_space = false;
        uint32_t remaining = file_size;
        uint32_t chunk_idx = 0;
        while (remaining > 0) {
            uint32_t chunk = remaining > chunk_cap ? chunk_cap : remaining;
            create_test_data(chunk_buffer, chunk, file_idx, chunk_idx);
            uint32_t written = romfs_write_file(chunk_buffer, chunk, &file);
            if (written != chunk || file.err != ROMFS_NOERR) {
                no_space = file.err == ROMFS_ERR_NO_SPACE && written < chunk;
                if (!no_space) {
                    fprintf(stderr, ANSI_COLOR_RED "Write error on %s: %s\n" ANSI_COLOR_RESET, filename, romfs_strerror(file.err));
                    success = false;
                }
                write_ok = false;
                break;
            }
            remaining -= chunk;
            chunk_idx++;
        }

        uint32_t close_err = romfs_close_file(&file);
        if (close_err != ROMFS_NOERR && !(no_space && close_err == ROMFS_ERR_NO_SPACE)) {
            fprintf(stderr, ANSI_COLOR_RED "Close error on %s: %s\n" ANSI_COLOR_RESET, filename, romfs_strerror(close_err));
            write_ok = false;
            success = false;
        }

        if (!write_ok) {
            uint32_t delete_err = romfs_delete(filename);
            if (delete_err != ROMFS_NOERR && delete_err != ROMFS_ERR_NO_ENTRY) {
                fprintf(stderr, "Failed to discard %s: %s\n", filename, romfs_strerror(delete_err));
                success = false;
            }
            free(io_buffer);
            if (success) {
                printf("Data space exhausted at file %d.\n", file_idx);
            }
            break;
        }

        if (entry_count == entry_capacity) {
            size_t new_capacity = entry_capacity ? entry_capacity * 2 : 64;
            random_fill_entry *new_entries = realloc(entries, new_capacity * sizeof(random_fill_entry));
            if (!new_entries) {
                fprintf(stderr, ANSI_COLOR_RED "Failed to expand metadata list\n" ANSI_COLOR_RESET);
                success = false;
                romfs_delete(filename);
                free(io_buffer);
                break;
            }
            entries = new_entries;
            entry_capacity = new_capacity;
        }

        random_fill_entry *entry = &entries[entry_count++];
        memset(entry, 0, sizeof(*entry));
        strncpy(entry->name, filename, sizeof(entry->name) - 1);
        entry->size = file_size;
        entry->file_index = file_idx;

        free(io_buffer);
        file_idx++;
    }

    if (success) {
        uint8_t *read_buffer = malloc(chunk_cap);
        uint8_t *expected_buffer = malloc(chunk_cap);
        uint8_t *io_buffer = malloc(ROMFS_FLASH_SECTOR);

        if (!read_buffer || !expected_buffer || !io_buffer) {
            fprintf(stderr, ANSI_COLOR_RED "Failed to allocate verification buffers\n" ANSI_COLOR_RESET);
            success = false;
        } else {
            printf(ANSI_COLOR_CYAN "Verifying %zu files written in random fill test...\n" ANSI_COLOR_RESET, entry_count);
            for (size_t i = 0; i < entry_count && success; i++) {
                romfs_file file;
                if (romfs_open_file(entries[i].name, &file, io_buffer) != ROMFS_NOERR) {
                    fprintf(stderr, ANSI_COLOR_RED "Failed to open %s for verification: %s\n" ANSI_COLOR_RESET, entries[i].name,
                            romfs_strerror(file.err));
                    success = false;
                    break;
                }

                if (file.entry.size != entries[i].size) {
                    fprintf(stderr, ANSI_COLOR_RED "Size mismatch for %s (expected %u, got %u)\n" ANSI_COLOR_RESET,
                            entries[i].name, entries[i].size, file.entry.size);
                    success = false;
                }

                uint32_t remaining = entries[i].size;
                uint32_t chunk_idx = 0;
                while (success && remaining > 0) {
                    uint32_t chunk = remaining > chunk_cap ? chunk_cap : remaining;
                    uint32_t read_bytes = romfs_read_file(read_buffer, chunk, &file);
                    if (read_bytes != chunk ||
                            (file.err != ROMFS_NOERR && file.err != ROMFS_ERR_EOF)) {
                        fprintf(stderr, ANSI_COLOR_RED "Read error on %s: expected %u, got %u (%s)\n" ANSI_COLOR_RESET,
                                entries[i].name, chunk, read_bytes, romfs_strerror(file.err));
                        success = false;
                        break;
                    }
                    create_test_data(expected_buffer, chunk, entries[i].file_index, chunk_idx);
                    if (memcmp(read_buffer, expected_buffer, chunk) != 0) {
                        fprintf(stderr, ANSI_COLOR_RED "Data mismatch on %s at chunk %u\n" ANSI_COLOR_RESET,
                                entries[i].name, chunk_idx);
                        success = false;
                        break;
                    }
                    remaining -= chunk;
                    chunk_idx++;
                }

                if (success && entries[i].size == 0) {
                    uint8_t dummy = 0;
                    if (romfs_read_file(&dummy, 1, &file) != 0 || file.err != ROMFS_ERR_EOF) {
                        fprintf(stderr, ANSI_COLOR_RED "Unexpected data when reading zero-length file %s\n" ANSI_COLOR_RESET, entries[i].name);
                        success = false;
                    }
                } else if (success && file.err != ROMFS_ERR_EOF) {
                    fprintf(stderr, ANSI_COLOR_RED "EOF not reported for %s\n" ANSI_COLOR_RESET, entries[i].name);
                    success = false;
                }

                success = close_test_file(&file) && success;
            }

            if (success) {
                printf(ANSI_COLOR_GREEN "Random fill-to-capacity verification succeeded (%zu files).\n" ANSI_COLOR_RESET, entry_count);
            }
        }

        free(read_buffer);
        free(expected_buffer);
        free(io_buffer);
    }

    free(chunk_buffer);
    free(entries);
    success = romfs_format() && success;
    return success;
}

static bool test_directory_api(void)
{
    printf(ANSI_COLOR_YELLOW "\n--- Running Directory API Test ---\n" ANSI_COLOR_RESET);

    if (!romfs_format()) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to format filesystem for directory test\n" ANSI_COLOR_RESET);
        return false;
    }

    uint8_t *io_buffer = malloc(ROMFS_FLASH_SECTOR);
    if (!io_buffer) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to allocate IO buffer for directory test\n" ANSI_COLOR_RESET);
        return false;
    }

    bool success = true;
    const uint8_t payload[] = { 0xde, 0xad, 0xbe, 0xef, 0x01, 0x02, 0x03, 0x04 };

    romfs_dir nintendo_dir;
    if (romfs_mkdir_path("/games/nintendo", true, &nintendo_dir) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to create /games/nintendo directory\n" ANSI_COLOR_RESET);
        success = false;
        goto cleanup;
    }

    romfs_file file;
    if (romfs_create_path("/games/nintendo/zelda.z64", &file, ROMFS_MODE_READWRITE, ROMFS_TYPE_MISC, io_buffer,
                          false) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to create file inside /games/nintendo\n" ANSI_COLOR_RESET);
        success = false;
        goto cleanup;
    }

    if (romfs_write_file(payload, sizeof(payload), &file) != sizeof(payload)) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to write payload to zelda.z64\n" ANSI_COLOR_RESET);
        success = false;
        goto cleanup;
    }

    if (romfs_close_file(&file) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to close zelda.z64 after write\n" ANSI_COLOR_RESET);
        success = false;
        goto cleanup;
    }

    romfs_file read_file;
    if (romfs_open_path("/games/nintendo/zelda.z64", &read_file, io_buffer) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to reopen zelda.z64 for validation\n" ANSI_COLOR_RESET);
        success = false;
        goto cleanup;
    }

    uint8_t readback[sizeof(payload)] = {0};
    if (romfs_read_file(readback, sizeof(readback), &read_file) != sizeof(readback) ||
            memcmp(readback, payload, sizeof(payload)) != 0) {
        fprintf(stderr, ANSI_COLOR_RED "Payload mismatch when reading zelda.z64\n" ANSI_COLOR_RESET);
        success = false;
        goto cleanup;
    }
    success = close_test_file(&read_file) && success;

    if (romfs_dir_remove(&nintendo_dir) != ROMFS_ERR_DIR_NOT_EMPTY) {
        fprintf(stderr, ANSI_COLOR_RED "Directory removal should fail while contents exist\n" ANSI_COLOR_RESET);
        success = false;
        goto cleanup;
    }

    romfs_file list_entry;
    if (romfs_list_dir(&list_entry, true, &nintendo_dir, false) != ROMFS_NOERR ||
            strcmp(list_entry.entry.name, "zelda.z64") != 0) {
        fprintf(stderr, ANSI_COLOR_RED "Directory listing did not return expected file\n" ANSI_COLOR_RESET);
        success = false;
        goto cleanup;
    }

    if (romfs_delete_in_dir(&nintendo_dir, "zelda.z64") != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to delete zelda.z64 via directory API\n" ANSI_COLOR_RESET);
        success = false;
        goto cleanup;
    }

    if (romfs_dir_remove(&nintendo_dir) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to remove empty /games/nintendo directory\n" ANSI_COLOR_RESET);
        success = false;
        goto cleanup;
    }

    romfs_dir root;
    romfs_dir_root(&root);
    romfs_dir games_dir;
    if (romfs_dir_open(&root, "games", &games_dir) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to reopen /games directory\n" ANSI_COLOR_RESET);
        success = false;
        goto cleanup;
    }

    if (romfs_dir_remove(&games_dir) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to remove empty /games directory\n" ANSI_COLOR_RESET);
        success = false;
        goto cleanup;
    }

    romfs_file missing_file;
    if (romfs_open_path("/games/nintendo/zelda.z64", &missing_file, io_buffer) != ROMFS_ERR_NO_ENTRY) {
        fprintf(stderr, ANSI_COLOR_RED "Removed path unexpectedly accessible\n" ANSI_COLOR_RESET);
        success = false;
        goto cleanup;
    }

    if (romfs_create_path("/saves/profile/slot1.bin", &file, ROMFS_MODE_READWRITE, ROMFS_TYPE_MISC, io_buffer,
                          true) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to create file with implicit directories\n" ANSI_COLOR_RESET);
        success = false;
        goto cleanup;
    }

    if (romfs_write_file(payload, sizeof(payload), &file) != sizeof(payload) ||
            romfs_close_file(&file) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to populate slot1.bin\n" ANSI_COLOR_RESET);
        success = false;
        goto cleanup;
    }

    romfs_dir saves_dir;
    if (romfs_dir_open(&root, "saves", &saves_dir) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to open /saves directory\n" ANSI_COLOR_RESET);
        success = false;
        goto cleanup;
    }

    romfs_dir profile_dir;
    if (romfs_dir_open(&saves_dir, "profile", &profile_dir) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to open /saves/profile directory\n" ANSI_COLOR_RESET);
        success = false;
        goto cleanup;
    }

    if (romfs_list_dir(&list_entry, true, &profile_dir, false) != ROMFS_NOERR ||
            strcmp(list_entry.entry.name, "slot1.bin") != 0) {
        fprintf(stderr, ANSI_COLOR_RED "Unexpected contents when listing /saves/profile\n" ANSI_COLOR_RESET);
        success = false;
        goto cleanup;
    }

    if (romfs_delete_in_dir(&profile_dir, "slot1.bin") != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to delete slot1.bin\n" ANSI_COLOR_RESET);
        success = false;
        goto cleanup;
    }

    if (romfs_dir_remove(&profile_dir) != ROMFS_NOERR ||
            romfs_dir_remove(&saves_dir) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to remove /saves hierarchy\n" ANSI_COLOR_RESET);
        success = false;
        goto cleanup;
    }

cleanup:
    free(io_buffer);
    success = romfs_format() && success;
    return success;
}

// Deletes some files, then creates some new ones, repeatedly.
static bool test_interleaved_delete_write(void)
{
    printf(ANSI_COLOR_YELLOW "\n--- Running Interleaved Delete/Write Test (Random Sizes) ---\n" ANSI_COLOR_RESET);
    printf("Pre-filling drive...\n");
    int initial_files = fill_drive("file", NORMAL_CHUNKS_PER_FILE, NORMAL_CHUNK_SIZE, true);
    if (initial_files <= 0 || !verify_drive(NORMAL_CHUNK_SIZE)) {
        return false;
    }

    char **file_list = NULL;
    int file_count = get_file_list(&file_list);
    uint8_t *io_buffer = malloc(ROMFS_FLASH_SECTOR);
    uint8_t *test_data = malloc(NORMAL_CHUNK_SIZE);
    bool success = false;
    if (file_count <= 0 || !io_buffer || !test_data) {
        fprintf(stderr, "Cannot prepare interleaved test\n");
        goto cleanup;
    }

    for (int i = 0; i < initial_files; i++) {
        for (int d = 0; d < 5 && file_count > 0; d++) {
            int idx = test_random() % file_count;
            char *name = file_list[idx];
            printf(ANSI_COLOR_MAGENTA "Deleting %s (%d remaining)... \r" ANSI_COLOR_RESET, name, file_count - 1);
            fflush(stdout);
            if (romfs_delete(name) != ROMFS_NOERR) {
                fprintf(stderr, "Failed to delete %s in interleaved test\n", name);
                goto cleanup;
            }
            free(name);
            file_list[idx] = file_list[--file_count];
        }
        if (!verify_drive(NORMAL_CHUNK_SIZE)) {
            goto cleanup;
        }

        char filename[ROMFS_MAX_NAME_LEN];
        snprintf(filename, sizeof(filename), "ifile%04d.dat", i);
        romfs_file file = {0};
        if (romfs_create_file(filename, &file, ROMFS_MODE_READWRITE, ROMFS_TYPE_MISC, io_buffer) != ROMFS_NOERR) {
            fprintf(stderr, "Failed to create %s: %s\n", filename, romfs_strerror(file.err));
            goto cleanup;
        }
        int chunks = 1 + test_random() % NORMAL_CHUNKS_PER_FILE;
        for (int j = 0; j < chunks; j++) {
            create_test_data(test_data, NORMAL_CHUNK_SIZE, i, j);
            if (romfs_write_file(test_data, NORMAL_CHUNK_SIZE, &file) != (uint32_t)NORMAL_CHUNK_SIZE ||
                    file.err != ROMFS_NOERR) {
                fprintf(stderr, "Failed to write %s: %s\n", filename, romfs_strerror(file.err));
                goto cleanup;
            }
        }
        if (romfs_close_file(&file) != ROMFS_NOERR) {
            fprintf(stderr, "Failed to close %s: %s\n", filename, romfs_strerror(file.err));
            goto cleanup;
        }
    }
    printf("\nInterleaved operations complete. Verifying final state...\n");
    success = verify_drive(NORMAL_CHUNK_SIZE);

cleanup:
    free_file_list(file_list, file_count);
    free(io_buffer);
    free(test_data);
    return success;
}

static bool delete_files(bool random_order, bool verify_between)
{
    char **files = NULL;
    int count = get_file_list(&files);
    if (count < 0) {
        return false;
    }
    if (random_order) {
        shuffle_filenames(files, count);
    }
    bool success = true;
    for (int i = 0; i < count; i++) {
        printf(ANSI_COLOR_MAGENTA "Deleting %s (%d/%d)... \r" ANSI_COLOR_RESET, files[i], i + 1, count);
        fflush(stdout);
        uint32_t err = romfs_delete(files[i]);
        if (err != ROMFS_NOERR) {
            fprintf(stderr, "Failed to delete %s: %s\n", files[i], romfs_strerror(err));
            success = false;
            break;
        }
        if (verify_between && ((i + 1) % 10 == 0 || i + 1 == count) &&
                !verify_drive(NORMAL_CHUNK_SIZE)) {
            /* Keep the name alive until diagnostics and verification finish. */
            fprintf(stderr, "Verification failed after deleting %s\n", files[i]);
            success = false;
            break;
        }
    }
    free_file_list(files, count);
    if (success) {
        printf("\nDeleted %d files. Free space: %u bytes\n", count, romfs_free());
    }
    return success;
}

static bool run_test_suite(uint32_t flash_size_mb, uint32_t seed,
                            test_flash_operation fail_op, uint64_t fail_nth)
{
    printf(ANSI_COLOR_YELLOW "=================================================\n" ANSI_COLOR_RESET);
    printf(ANSI_COLOR_CYAN "      Testing with %uMB Flash Image\n" ANSI_COLOR_RESET, flash_size_mb);
    printf("Seed: %" PRIu32 "\n", seed);
    rng_state = seed ^ (flash_size_mb * 0x9e3779b9u);
    rng_calls = 0;
    bool success = false;
    uint16_t *flash_map = NULL;
    uint8_t *flash_list = NULL;
    const char *stage = "flash initialization";

    if (!test_flash_init(flash_size_mb * ROMFS_MB)) {
        fprintf(stderr, "Cannot allocate %uMB flash image\n", flash_size_mb);
        goto cleanup;
    }
    flash_base = test_flash_data();
    if (fail_nth && !test_flash_fail_on(fail_op, fail_nth)) {
        fprintf(stderr, "Cannot configure flash failure\n");
        goto cleanup;
    }

    uint32_t map_size, list_size;
    romfs_get_buffers_sizes(flash_size_mb * ROMFS_MB, &map_size, &list_size);
    /* Defined contents make a failed mount read safe to diagnose even while
     * current ROMFS ignores callback errors. Stop before using that mount. */
    flash_map = calloc(1, map_size);
    flash_list = calloc(1, list_size);
    if (!flash_map || !flash_list) {
        fprintf(stderr, "Cannot allocate map/list buffers\n");
        goto cleanup;
    }

#define RUN_CHECK(expression) do { \
    stage = #expression; \
    if (!(expression) || !flash_ok()) { \
        goto cleanup; \
    } \
} while (0)

    RUN_CHECK(romfs_start(0x10000, flash_size_mb * ROMFS_MB, flash_map, flash_list));
    RUN_CHECK(test_service_sector_protection(flash_map, map_size));
    RUN_CHECK(test_large_io_transfer());
    RUN_CHECK(test_seek_tell());
    RUN_CHECK(test_append_mode());
    RUN_CHECK(test_truncate_api());
    RUN_CHECK(test_random_write_api());
    RUN_CHECK(test_rename_api());
    RUN_CHECK(test_directory_api());

    printf(ANSI_COLOR_YELLOW "\n--- Running Fill (Fixed Size) / Sequential Delete Test ---\n" ANSI_COLOR_RESET);
    RUN_CHECK(romfs_format());
    RUN_CHECK(fill_drive("file", NORMAL_CHUNKS_PER_FILE, NORMAL_CHUNK_SIZE, false) > 0);
    RUN_CHECK(verify_drive(NORMAL_CHUNK_SIZE));
    RUN_CHECK(delete_files(false, false));

    printf(ANSI_COLOR_YELLOW "\n--- Running Refill (Fixed Size) / Random Delete Test ---\n" ANSI_COLOR_RESET);
    RUN_CHECK(fill_drive("rfile", NORMAL_CHUNKS_PER_FILE, NORMAL_CHUNK_SIZE, false) > 0);
    RUN_CHECK(verify_drive(NORMAL_CHUNK_SIZE));
    RUN_CHECK(delete_files(true, true));

    printf(ANSI_COLOR_YELLOW "\n--- Running Fill (Random Size) / Verify / Delete Test ---\n" ANSI_COLOR_RESET);
    RUN_CHECK(romfs_format());
    RUN_CHECK(fill_drive("rndfile", NORMAL_CHUNKS_PER_FILE * 2, NORMAL_CHUNK_SIZE, true) > 0);
    RUN_CHECK(verify_drive(NORMAL_CHUNK_SIZE));
    RUN_CHECK(delete_files(true, false));

    RUN_CHECK(test_random_fill_to_capacity());
    RUN_CHECK(romfs_format());
    RUN_CHECK(test_interleaved_delete_write());

    printf(ANSI_COLOR_YELLOW "\n--- Running File List Limit Test ---\n" ANSI_COLOR_RESET);
    RUN_CHECK(romfs_format());
    RUN_CHECK(fill_drive("sfile", 1, SMALL_FILE_SIZE, false) == (int)(list_size / sizeof(romfs_entry)) - 3);
    RUN_CHECK(verify_drive(SMALL_FILE_SIZE));
#undef RUN_CHECK

    if (fail_nth) {
        stage = "requested I/O failure was not reached";
        goto cleanup;
    }
    success = true;

cleanup:
    if (!success) {
        fprintf(stderr, "Test for %uMB FAILED at %s\n", flash_size_mb, stage);
    }
    const test_flash_stats *stats = test_flash_get_stats();
    if (stats->first_failure_reason) {
        fprintf(stderr, "Flash %s at 0x%08" PRIx32 ": %s\n",
                test_flash_operation_name(stats->first_failure_op), stats->first_failure_offset,
                stats->first_failure_reason);
    }
    printf("Flash calls: read=%" PRIu64 " erase=%" PRIu64 " write=%" PRIu64
           " rejected=%" PRIu64 " injected=%" PRIu64 "\n",
           stats->calls[TEST_FLASH_READ], stats->calls[TEST_FLASH_ERASE],
           stats->calls[TEST_FLASH_WRITE], stats->rejected, stats->injected);
    printf("Random calls: %" PRIu64 "; state=%" PRIu32 "\n", rng_calls, rng_state);
    printf("[%s] %uMB seed=%" PRIu32 "\n", success ? "PASS" : "FAIL", flash_size_mb, seed);
    free(flash_map);
    free(flash_list);
    test_flash_destroy();
    flash_base = NULL;
    return success;
}

static bool parse_number(const char *text, uint64_t maximum, uint64_t *value)
{
    if (!text || text[0] < '0' || text[0] > '9') {
        return false;
    }
    errno = 0;
    char *end;
    unsigned long long parsed = strtoull(text, &end, 10);
    if (errno == ERANGE || *end || parsed > maximum) {
        return false;
    }
    *value = parsed;
    return true;
}

static void usage(const char *program)
{
    fprintf(stderr, "Usage: %s [--seed N] [--flash-mb N] [--fail-io read:N|erase:N|write:N]\n"
            "Default: seed 1, flash sizes 16/32/64/128/256 MiB.\n"
            "--flash-mb accepts 2..256 MiB; smaller geometries currently expose ROMFS defects.\n"
            "--fail-io fails the Nth callback of that kind per image and makes the run fail.\n",
            program);
}

int main(int argc, char *argv[])
{
    /* Keep merged stdout/stderr diagnostics in execution order. */
    setvbuf(stdout, NULL, _IONBF, 0);
    uint32_t seed = 1;
    uint32_t selected_size = 0;
    uint64_t fail_nth = 0;
    test_flash_operation fail_op = TEST_FLASH_OPERATION_COUNT;
    for (int i = 1; i < argc; i++) {
        uint64_t value;
        if (!strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        }
        if (i + 1 >= argc) {
            usage(argv[0]);
            return 2;
        }
        const char *option = argv[i++];
        if (!strcmp(option, "--seed") && parse_number(argv[i], UINT32_MAX, &value)) {
            seed = (uint32_t)value;
        } else if (!strcmp(option, "--flash-mb") && parse_number(argv[i], 256, &value) && value >= 2) {
            selected_size = (uint32_t)value;
        } else if (!strcmp(option, "--fail-io")) {
            const char *colon = strchr(argv[i], ':');
            fail_op = TEST_FLASH_OPERATION_COUNT;
            if (colon && parse_number(colon + 1, UINT64_MAX, &value) && value != 0) {
                for (test_flash_operation op = TEST_FLASH_READ; op < TEST_FLASH_OPERATION_COUNT; op++) {
                    const char *name = test_flash_operation_name(op);
                    if ((size_t)(colon - argv[i]) == strlen(name) && !strncmp(argv[i], name, strlen(name))) {
                        fail_op = op;
                    }
                }
            }
            if (fail_op == TEST_FLASH_OPERATION_COUNT) {
                usage(argv[0]);
                return 2;
            }
            fail_nth = value;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    const uint32_t sizes[] = {16, 32, 64, 128, 256};
    unsigned passed = 0, failed = 0;
    unsigned count = selected_size ? 1 : sizeof(sizes) / sizeof(sizes[0]);
    for (unsigned i = 0; i < count; i++) {
        if (run_test_suite(selected_size ? selected_size : sizes[i], seed, fail_op, fail_nth)) {
            passed++;
        } else {
            failed++;
        }
    }
    printf("Suites: %u passed, %u failed.\n", passed, failed);
    if (failed) {
        fprintf(stderr, "ROMFS tests FAILED.\n");
        return 1;
    }
    printf(ANSI_COLOR_GREEN "All tests completed successfully!\n" ANSI_COLOR_RESET);
    return 0;
}
