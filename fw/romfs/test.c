/**
 * Copyright (c) 2022-2023 sashz /pdaXrom.org/
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <inttypes.h>
#include "romfs.h"

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

static uint8_t *memory = NULL;
static uint8_t *flash_base = NULL;

const int NORMAL_CHUNK_SIZE = 256;
const int NORMAL_CHUNKS_PER_FILE = 20; // Creates 5KB files (256 * 20)
const int SMALL_FILE_SIZE = 1; // For testing file list limits

typedef struct {
    char name[ROMFS_MAX_NAME_LEN];
    uint32_t size;
    int file_index;
} random_fill_entry;

bool romfs_flash_sector_erase(uint32_t offset)
{
    memset(&flash_base[offset], 0xff, ROMFS_FLASH_SECTOR);
    return true;
}

bool romfs_flash_sector_write(uint32_t offset, uint8_t *buffer)
{
    memmove(&flash_base[offset], buffer, ROMFS_FLASH_SECTOR);
    return true;
}

bool romfs_flash_sector_read(uint32_t offset, uint8_t *buffer, uint32_t need)
{
    memmove(buffer, &flash_base[offset], need);
    return true;
}

// Fills a buffer with deterministic dummy data based on an index
static void create_test_data(uint8_t *buffer, size_t size, int file_idx, int chunk_idx)
{
    for (size_t i = 0; i < size; i++) {
        buffer[i] = (uint8_t)((file_idx + chunk_idx + i) & 0xFF);
    }
}

// Shuffles an array of strings
static void shuffle_filenames(char **array, size_t n)
{
    if (n > 1) {
        for (size_t i = 0; i < n - 1; i++) {
            size_t j = i + rand() / (RAND_MAX / (n - i) + 1);
            char *t = array[j];
            array[j] = array[i];
            array[i] = t;
        }
    }
}

// Gets a list of all user-created files
static int get_file_list(char*** file_list_out)
{
    int count = 0;
    int capacity = 128;
    char** file_list = malloc(capacity * sizeof(char*));
    if (!file_list) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to allocate memory for file list\n" ANSI_COLOR_RESET);
        return -1;
    }

    romfs_file file;
    if (romfs_list(&file, true) == ROMFS_NOERR) {
        do {
            if (file.entry.attr.names.type == ROMFS_TYPE_MISC) {
                if (count >= capacity) {
                    capacity *= 2;
                    char** new_list = realloc(file_list, capacity * sizeof(char*));
                    if (!new_list) {
                        fprintf(stderr, ANSI_COLOR_RED "Failed to reallocate memory for file list\n" ANSI_COLOR_RESET);
                        for(int i=0; i<count; i++) free(file_list[i]);
                        free(file_list);
                        return -1;
                    }
                    file_list = new_list;
                }
                file_list[count] = strdup(file.entry.name);
                count++;
            }
        } while (romfs_list(&file, false) == ROMFS_NOERR);
    }
    *file_list_out = file_list;
    return count;
}

// Fills the filesystem with files of a given size until no space is left.
static int fill_drive(const char* prefix, int max_chunks_per_file, int chunk_size, bool random_size)
{
    int file_idx = 0;
    uint8_t* test_data = malloc(chunk_size);

    while (true) {
        char filename[ROMFS_MAX_NAME_LEN];
        snprintf(filename, sizeof(filename), "%s%04d.dat", prefix, file_idx);

        romfs_file file;
        uint8_t *romfs_io_buffer = malloc(ROMFS_FLASH_SECTOR);
        if (!romfs_io_buffer) {
            fprintf(stderr, ANSI_COLOR_RED "Failed to allocate IO buffer\n" ANSI_COLOR_RESET);
            free(test_data);
            return -1;
        }

        if (romfs_create_file(filename, &file, ROMFS_MODE_READWRITE, ROMFS_TYPE_MISC, romfs_io_buffer) != ROMFS_NOERR) {
            if (file.err == ROMFS_ERR_NO_FREE_ENTRIES) {
                 printf(ANSI_COLOR_YELLOW "\nCould not create new file: file list is full. Created %d files.\n" ANSI_COLOR_RESET, file_idx);
            } else {
                 printf(ANSI_COLOR_YELLOW "\nCould not create new file entry. Filesystem full. Created %d files.\n" ANSI_COLOR_RESET, file_idx);
            }
            free(romfs_io_buffer);
            break;
        }

        printf(ANSI_COLOR_MAGENTA "Creating %s... \r" ANSI_COLOR_RESET, filename);
        fflush(stdout);

        int chunks_this_file = random_size ? (1 + (rand() % max_chunks_per_file)) : max_chunks_per_file;
        if (chunks_this_file == 0) chunks_this_file = 1;

        bool write_error = false;
        for (int j = 0; j < chunks_this_file; j++) {
            create_test_data(test_data, chunk_size, file_idx, j);
            if (romfs_write_file(test_data, chunk_size, &file) == 0) {
                 if(file.err != ROMFS_ERR_NO_SPACE) {
                    fprintf(stderr, ANSI_COLOR_RED "\nromfs write error on %s: %s\n" ANSI_COLOR_RESET, filename, romfs_strerror(file.err));
                 }
                 write_error = true;
                 break;
            }
        }

        if (romfs_close_file(&file) != ROMFS_NOERR) {
            fprintf(stderr, ANSI_COLOR_RED "\nromfs close error on %s: %s\n" ANSI_COLOR_RESET, filename, romfs_strerror(file.err));
            write_error = true;
        }

        free(romfs_io_buffer);

        if (write_error) {
            romfs_delete(filename);
            printf(ANSI_COLOR_YELLOW "\nFilesystem is full. Created %d files.\n" ANSI_COLOR_RESET, file_idx);
            break;
        }

        file_idx++;
    }
    free(test_data);
    return file_idx;
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

        romfs_file file;
        uint8_t *romfs_io_buffer = malloc(ROMFS_FLASH_SECTOR);
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
            if (bytes_read != bytes_to_read) {
                fprintf(stderr, ANSI_COLOR_RED "\nRead error on %s, chunk %d. Expected %d, got %d\n" ANSI_COLOR_RESET, file_list[i], chunk_idx, bytes_to_read, bytes_read);
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

        free(romfs_io_buffer);
        if (!file_ok) {
            success = false;
        }
    }

    for(int i=0; i<file_count; i++) free(file_list[i]);
    free(file_list);
    free(read_buffer);
    free(expected_data);

    if(success) printf(ANSI_COLOR_GREEN "\nVerification successful. All %d files are correct.\n" ANSI_COLOR_RESET, file_count);
    else printf(ANSI_COLOR_RED "\nVerification FAILED.\n" ANSI_COLOR_RESET);

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
        fprintf(stderr, ANSI_COLOR_RED "Close failed for large_test.bin after write: %s\n" ANSI_COLOR_RESET, romfs_strerror(file.err));
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
        romfs_close_file(&reader);
        goto cleanup_close_write;
    }

    if (reader.err != ROMFS_ERR_EOF) {
        fprintf(stderr, ANSI_COLOR_RED "Unexpected reader.err (%u) after large read\n" ANSI_COLOR_RESET, reader.err);
        romfs_close_file(&reader);
        goto cleanup_close_write;
    }

    if (memcmp(write_data, read_data, large_len) != 0) {
        fprintf(stderr, ANSI_COLOR_RED "Data mismatch after large I/O transfer\n" ANSI_COLOR_RESET);
        romfs_close_file(&reader);
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
    romfs_format();
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
        fprintf(stderr, ANSI_COLOR_RED "Failed to open seektest.bin for reading: %s\n" ANSI_COLOR_RESET, romfs_strerror(reader.err));
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
    romfs_close_file(&reader);

    if (success) {
        romfs_file empty_file;
        if (romfs_create_file("empty_seek.bin", &file, ROMFS_MODE_READWRITE, ROMFS_TYPE_MISC, write_io) != ROMFS_NOERR) {
            fprintf(stderr, ANSI_COLOR_RED "Failed to create empty_seek.bin: %s\n" ANSI_COLOR_RESET, romfs_strerror(file.err));
            success = false;
        } else if (romfs_close_file(&file) != ROMFS_NOERR) {
            fprintf(stderr, ANSI_COLOR_RED "Failed to close empty_seek.bin after creation: %s\n" ANSI_COLOR_RESET, romfs_strerror(file.err));
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
            romfs_close_file(&empty_file);
        }
    }

cleanup:
    romfs_format();
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
        fprintf(stderr, ANSI_COLOR_RED "tell mismatch after initial append (got %u, expected %zu)\n" ANSI_COLOR_RESET, pos, part1_len);
        success = false;
        goto cleanup_close_file;
    }

cleanup_close_file:
    if (romfs_close_file(&file) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to close append.bin after first write: %s\n" ANSI_COLOR_RESET, romfs_strerror(file.err));
        success = false;
    }
    if (!success) {
        goto cleanup;
    }

    /* Verify initial contents */
    memset(&reader, 0, sizeof(reader));
    if (romfs_open_file("append.bin", &reader, read_io_buffer) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to reopen append.bin for verification: %s\n" ANSI_COLOR_RESET, romfs_strerror(reader.err));
        success = false;
        goto cleanup;
    }
    if (romfs_read_file(read_buffer, part1_len, &reader) != part1_len ||
        memcmp(read_buffer, part1, part1_len) != 0) {
        fprintf(stderr, ANSI_COLOR_RED "Initial append verification failed\n" ANSI_COLOR_RESET);
        success = false;
    }
    romfs_close_file(&reader);
    if (!success) {
        goto cleanup;
    }

    /* Step 2: append second chunk within same sector */
    memset(&file, 0, sizeof(file));
    if (romfs_open_append_path("append.bin", &file, ROMFS_TYPE_MISC, io_buffer, false) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to reopen append.bin for second append: %s\n" ANSI_COLOR_RESET, romfs_strerror(file.err));
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
        fprintf(stderr, ANSI_COLOR_RED "tell mismatch after second append (got %u, expected %zu)\n" ANSI_COLOR_RESET, pos, part1_len + part2_len);
        success = false;
        goto cleanup_second_close;
    }

cleanup_second_close:
    if (romfs_close_file(&file) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to close append.bin after second write: %s\n" ANSI_COLOR_RESET, romfs_strerror(file.err));
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
        fprintf(stderr, ANSI_COLOR_RED "Failed to reopen append.bin for second verification: %s\n" ANSI_COLOR_RESET, romfs_strerror(reader.err));
        success = false;
        goto cleanup;
    }
    size_t current_size = part1_len + part2_len;
    if (romfs_read_file(read_buffer, current_size, &reader) != current_size ||
        memcmp(read_buffer, expected, current_size) != 0) {
        fprintf(stderr, ANSI_COLOR_RED "Second append verification failed\n" ANSI_COLOR_RESET);
        success = false;
    }
    romfs_close_file(&reader);
    if (!success) {
        goto cleanup;
    }

    /* Step 3: append data crossing sector boundary */
    memset(&file, 0, sizeof(file));
    if (romfs_open_append_path("append.bin", &file, ROMFS_TYPE_MISC, io_buffer, false) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to reopen append.bin for large append: %s\n" ANSI_COLOR_RESET, romfs_strerror(file.err));
        success = false;
        goto cleanup;
    }

    if (romfs_tell_file(&file, &pos) != ROMFS_NOERR || pos != current_size) {
        fprintf(stderr, ANSI_COLOR_RED "tell mismatch before large append (got %u, expected %zu)\n" ANSI_COLOR_RESET, pos, current_size);
        success = false;
        goto cleanup_large_close;
    }

    if (romfs_write_file(big_data, big_len, &file) != big_len) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to write large append payload\n" ANSI_COLOR_RESET);
        success = false;
        goto cleanup_large_close;
    }

    if (romfs_tell_file(&file, &pos) != ROMFS_NOERR || pos != current_size + big_len) {
        fprintf(stderr, ANSI_COLOR_RED "tell mismatch after large append (got %u, expected %zu)\n" ANSI_COLOR_RESET, pos, current_size + big_len);
        success = false;
        goto cleanup_large_close;
    }

cleanup_large_close:
    if (romfs_close_file(&file) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to close append.bin after large append: %s\n" ANSI_COLOR_RESET, romfs_strerror(file.err));
        success = false;
    }
    if (!success) {
        goto cleanup;
    }

    /* Verify final content */
    memcpy(expected + current_size, big_data, big_len);
    memset(&reader, 0, sizeof(reader));
    if (romfs_open_file("append.bin", &reader, read_io_buffer) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to open append.bin for final verification: %s\n" ANSI_COLOR_RESET, romfs_strerror(reader.err));
        success = false;
        goto cleanup;
    }
    if (romfs_read_file(read_buffer, total_expected, &reader) != total_expected ||
        memcmp(read_buffer, expected, total_expected) != 0) {
        fprintf(stderr, ANSI_COLOR_RED "Final append verification failed\n" ANSI_COLOR_RESET);
        success = false;
    }
    romfs_close_file(&reader);
    if (!success) {
        goto cleanup;
    }

    /* Flat namespace append helper */
    memset(&file, 0, sizeof(file));
    if (romfs_open_append("flat.bin", &file, ROMFS_TYPE_MISC, io_buffer) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to open flat.bin via romfs_open_append: %s\n" ANSI_COLOR_RESET, romfs_strerror(file.err));
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
    romfs_close_file(&reader);
    if (!success) {
        goto cleanup;
    }

    /* Ensure create_dirs parameter works */
    memset(&file, 0, sizeof(file));
    if (romfs_open_append_path("logs/session/log.txt", &file, ROMFS_TYPE_MISC, io_buffer, true) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to append with implicit directory creation: %s\n" ANSI_COLOR_RESET, romfs_strerror(file.err));
        success = false;
        goto cleanup;
    }
    if (romfs_close_file(&file) != ROMFS_NOERR) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to close log.txt in append test: %s\n" ANSI_COLOR_RESET, romfs_strerror(file.err));
        success = false;
        goto cleanup;
    }
    romfs_delete_path("logs/session/log.txt");
    romfs_rmdir_path("logs/session");
    romfs_rmdir_path("logs");

cleanup:
    romfs_delete_path("append.bin");
    romfs_delete("flat.bin");
    romfs_format();
    free(io_buffer);
    free(read_io_buffer);
    free(big_data);
    free(read_buffer);
    free(expected);
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
    if (romfs_create_file_in_dir(&alpha, "note.bin", &file, ROMFS_MODE_READWRITE, ROMFS_TYPE_MISC, io_buffer) != ROMFS_NOERR) {
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

    romfs_close_file(&reader);
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
    romfs_close_file(&reader);
    reader_open = false;

    // Conflict detection
    if (romfs_create_file_in_dir(&beta, "other.bin", &file, ROMFS_MODE_READWRITE, ROMFS_TYPE_MISC, io_buffer) != ROMFS_NOERR ||
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
    romfs_close_file(&reader);
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
    romfs_close_file(&reader);
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
        romfs_close_file(&reader);
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
    romfs_format();
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

        uint32_t file_size = (uint32_t)(rand() % (max_file_size + 1));

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
        uint32_t remaining = file_size;
        uint32_t chunk_idx = 0;
        while (remaining > 0) {
            uint32_t chunk = remaining > chunk_cap ? chunk_cap : remaining;
            create_test_data(chunk_buffer, chunk, file_idx, chunk_idx);
            uint32_t written = romfs_write_file(chunk_buffer, chunk, &file);
            if (written != chunk) {
                if (file.err != ROMFS_ERR_NO_SPACE) {
                    fprintf(stderr, ANSI_COLOR_RED "Write error on %s: %s\n" ANSI_COLOR_RESET, filename, romfs_strerror(file.err));
                }
                write_ok = false;
                break;
            }
            remaining -= chunk;
            chunk_idx++;
        }

        if (write_ok && romfs_close_file(&file) != ROMFS_NOERR) {
            fprintf(stderr, ANSI_COLOR_RED "Close error on %s: %s\n" ANSI_COLOR_RESET, filename, romfs_strerror(file.err));
            write_ok = false;
        }

        if (!write_ok) {
            romfs_delete(filename);
            free(io_buffer);
            printf(ANSI_COLOR_YELLOW "Stopping creation due to write failure (likely full) at file %d\n" ANSI_COLOR_RESET, file_idx);
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
                    fprintf(stderr, ANSI_COLOR_RED "Failed to open %s for verification: %s\n" ANSI_COLOR_RESET, entries[i].name, romfs_strerror(file.err));
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
                    if (read_bytes != chunk) {
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

                romfs_close_file(&file);
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
    romfs_format();
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
    if (romfs_create_path("/games/nintendo/zelda.z64", &file, ROMFS_MODE_READWRITE, ROMFS_TYPE_MISC, io_buffer, false) != ROMFS_NOERR) {
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
    romfs_close_file(&read_file);

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

    if (romfs_create_path("/saves/profile/slot1.bin", &file, ROMFS_MODE_READWRITE, ROMFS_TYPE_MISC, io_buffer, true) != ROMFS_NOERR) {
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
    romfs_format();
    return success;
}

// Deletes some files, then creates some new ones, repeatedly.
static bool test_interleaved_delete_write(void)
{
    printf(ANSI_COLOR_YELLOW "\n--- Running Interleaved Delete/Write Test (Random Sizes) ---\n" ANSI_COLOR_RESET);

    // 1. Fill the drive about halfway to have something to work with.
    printf("Pre-filling drive to 50%% capacity...\n");
    int initial_files = fill_drive("file", NORMAL_CHUNKS_PER_FILE, NORMAL_CHUNK_SIZE, true);
    if (!verify_drive(NORMAL_CHUNK_SIZE)) return false;

    char** file_list = NULL;
    int file_count = get_file_list(&file_list);
    if (file_count == 0) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to create initial files for interleaved test.\n" ANSI_COLOR_RESET);
        return false;
    }

    // 2. Interleave operations
    int files_to_create = initial_files; 
    int new_file_idx = 0;
    const int delete_batch_size = 5;

    for (int i = 0; i < files_to_create; i++) {
        // Delete one or more random files
        if (file_count > 0) {
            for(int d=0; d<delete_batch_size && file_count > 0; d++) {
                int file_to_delete_idx = rand() % file_count;
                char* file_to_delete = file_list[file_to_delete_idx];
                printf(ANSI_COLOR_MAGENTA "Deleting %s (%d remaining)... \r" ANSI_COLOR_RESET, file_to_delete, file_count - 1);
                fflush(stdout);
                if (romfs_delete(file_to_delete) != ROMFS_NOERR) {
                    fprintf(stderr, ANSI_COLOR_RED "\nFailed to delete %s in interleaved test.\n" ANSI_COLOR_RESET, file_to_delete);
                    return false;
                }
                free(file_to_delete);
                // Replace the deleted entry with the last entry
                file_list[file_to_delete_idx] = file_list[file_count - 1];
                file_count--;
            }

            // Verify the state of the filesystem after deletions
            if (!verify_drive(NORMAL_CHUNK_SIZE)) {
                fprintf(stderr, ANSI_COLOR_RED "Verification failed during interleaved test after deletion phase.\n" ANSI_COLOR_RESET);
                return false;
            }
        }

        // Create one new file with random size
        char filename[ROMFS_MAX_NAME_LEN];
        snprintf(filename, sizeof(filename), "ifile%04d.dat", new_file_idx);

        romfs_file file;
        uint8_t *romfs_io_buffer = malloc(ROMFS_FLASH_SECTOR);
        uint8_t* test_data = malloc(NORMAL_CHUNK_SIZE);

        romfs_create_file(filename, &file, ROMFS_MODE_READWRITE, ROMFS_TYPE_MISC, romfs_io_buffer);

        int chunks_this_file = 1 + (rand() % NORMAL_CHUNKS_PER_FILE);
        for (int j = 0; j < chunks_this_file; j++) {
            create_test_data(test_data, NORMAL_CHUNK_SIZE, new_file_idx, j);
            if(romfs_write_file(test_data, NORMAL_CHUNK_SIZE, &file) == 0) {
                // Stop if we run out of space
                break;
            }
        }
        romfs_close_file(&file);
        free(romfs_io_buffer);
        free(test_data);
        new_file_idx++;
    }
    free(file_list);

    printf("\nInterleaved operations complete. Verifying final state...\n");
    return verify_drive(NORMAL_CHUNK_SIZE);
}


// Runs the entire test suite for a given flash size.
static void run_test_suite(uint32_t flash_size_mb)
{
    printf(ANSI_COLOR_YELLOW "=================================================\n" ANSI_COLOR_RESET);
    printf(ANSI_COLOR_CYAN "      Testing with %uMB Flash Image\n" ANSI_COLOR_RESET, flash_size_mb);
    printf(ANSI_COLOR_YELLOW "=================================================\n" ANSI_COLOR_RESET);

    size_t mem_size_bytes = flash_size_mb * ROMFS_MB;
    memory = malloc(mem_size_bytes);
    if (!memory) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to allocate memory for %uMB flash image\n" ANSI_COLOR_RESET, flash_size_mb);
        return;
    }
    memset(memory, 0xff, mem_size_bytes);
    flash_base = memory;

    uint32_t map_size = 0;
    uint32_t list_size = 0;
    romfs_get_buffers_sizes(mem_size_bytes, &map_size, &list_size);

    uint16_t *flash_map = malloc(map_size);
    uint8_t *flash_list = malloc(list_size);
    if (!flash_map || !flash_list) {
        fprintf(stderr, ANSI_COLOR_RED "Failed to allocate memory for map/list\n" ANSI_COLOR_RESET);
        goto cleanup;
    }

    if (!romfs_start(0x10000, mem_size_bytes, flash_map, flash_list)) {
        printf(ANSI_COLOR_RED "Cannot start romfs!\n" ANSI_COLOR_RESET);
        goto cleanup;
    }

    if (!test_large_io_transfer()) {
        goto cleanup;
    }

    if (!test_seek_tell()) {
        goto cleanup;
    }

    if (!test_append_mode()) {
        goto cleanup;
    }

    if (!test_rename_api()) {
        goto cleanup;
    }

    if (!test_directory_api()) {
        goto cleanup;
    }

    // --- Test 1: Fixed Size Fill, Verify, Sequential Delete ---
    printf(ANSI_COLOR_YELLOW "\n--- Running Fill (Fixed Size) / Sequential Delete Test ---\n" ANSI_COLOR_RESET);
    romfs_format();
    printf("Initial free space: %u bytes\n", romfs_free());
    fill_drive("file", NORMAL_CHUNKS_PER_FILE, NORMAL_CHUNK_SIZE, false);
    if (!verify_drive(NORMAL_CHUNK_SIZE)) goto cleanup;

    printf("\nSequentially deleting files...\n");
    char** file_list = NULL;
    int file_count = get_file_list(&file_list);
    if (file_count > 0) {
        for (int i = 0; i < file_count; i++) {
            printf(ANSI_COLOR_MAGENTA "Deleting %s... \r" ANSI_COLOR_RESET, file_list[i]);
            fflush(stdout);
            romfs_delete(file_list[i]);
            free(file_list[i]);
        }
        free(file_list);
        printf("\nSequential deletion complete. %d files deleted.\n", file_count);
    }
    printf("Free space after sequential delete: %u bytes\n", romfs_free());

    // --- Test 2: Fixed Size Refill, Verify, Random Delete with Verification ---
    printf(ANSI_COLOR_YELLOW "\n--- Running Refill (Fixed Size) / Random Delete Test ---\n" ANSI_COLOR_RESET);
    fill_drive("rfile", NORMAL_CHUNKS_PER_FILE, NORMAL_CHUNK_SIZE, false);
    if (!verify_drive(NORMAL_CHUNK_SIZE)) goto cleanup;

    printf("\nRandomly deleting files with verification...\n");
    file_list = NULL;
    file_count = get_file_list(&file_list);
    if (file_count > 0) {
        shuffle_filenames(file_list, file_count);
        int total_files = file_count;
        for (int i = 0; i < total_files; i++) {
            printf(ANSI_COLOR_MAGENTA "Deleting %s (%d/%d)... \r" ANSI_COLOR_RESET, file_list[i], i + 1, total_files);
            fflush(stdout);
            romfs_delete(file_list[i]);
            free(file_list[i]);

            // After every 10 deletions (and for the very last one), verify remaining files
            if ((i + 1) % 10 == 0 || (i + 1) == total_files) {
                if (!verify_drive(NORMAL_CHUNK_SIZE)) {
                    fprintf(stderr, ANSI_COLOR_RED "Verification failed after deleting %s!\n" ANSI_COLOR_RESET, file_list[i]);
                    // To avoid memory leaks on early exit
                    for (int j = i + 1; j < total_files; j++) {
                        free(file_list[j]);
                    }
                    free(file_list);
                    goto cleanup;
                }
            }
        }
        free(file_list);
        printf("\nRandom deletion with verification complete. %d files deleted.\n", total_files);
    }
    printf("Free space after random delete: %u bytes\n", romfs_free());

    // --- Test 3: Random Size Fill, Verify, Random Delete ---
    printf(ANSI_COLOR_YELLOW "\n--- Running Fill (Random Size) / Verify / Delete Test ---\n" ANSI_COLOR_RESET);
    romfs_format();
    fill_drive("rndfile", NORMAL_CHUNKS_PER_FILE * 2, NORMAL_CHUNK_SIZE, true);
    if (!verify_drive(NORMAL_CHUNK_SIZE)) goto cleanup;

    printf("\nRandomly deleting all random-sized files...\n");
    file_list = NULL;
    file_count = get_file_list(&file_list);
    if (file_count > 0) {
        shuffle_filenames(file_list, file_count);
        for (int i = 0; i < file_count; i++) {
            printf(ANSI_COLOR_MAGENTA "Deleting %s... \r" ANSI_COLOR_RESET, file_list[i]);
            fflush(stdout);
            romfs_delete(file_list[i]);
            free(file_list[i]);
        }
        free(file_list);
        printf("\nRandom deletion complete. %d files deleted.\n", file_count);
    }
    printf("Free space after random delete: %u bytes\n", romfs_free());

    if (!test_random_fill_to_capacity()) goto cleanup;


    // --- Test 4: Interleaved Delete and Write ---
    romfs_format();
    if (!test_interleaved_delete_write()) goto cleanup;

    // --- Test 5: File List Limit ---
    printf(ANSI_COLOR_YELLOW "\n--- Running File List Limit Test ---\n" ANSI_COLOR_RESET);
    romfs_format();
    fill_drive("sfile", 1, SMALL_FILE_SIZE, false);
    printf("File list limit test complete.\n");

    printf(ANSI_COLOR_GREEN "\nTest for %uMB complete.\n\n" ANSI_COLOR_RESET, flash_size_mb);

cleanup:
    free(memory);
    free(flash_map);
    free(flash_list);
    memory = NULL;
    flash_base = NULL;
}

int main(int argc, char *argv[])
{
    srand(time(NULL));

    run_test_suite(16);
    run_test_suite(32);
    run_test_suite(64);
    run_test_suite(128);
    run_test_suite(256);

    printf(ANSI_COLOR_GREEN "=================================================\n" ANSI_COLOR_RESET);
    printf(ANSI_COLOR_GREEN " All tests completed successfully!\n" ANSI_COLOR_RESET);
    printf(ANSI_COLOR_GREEN "=================================================\n" ANSI_COLOR_RESET);

    return 0;
}
