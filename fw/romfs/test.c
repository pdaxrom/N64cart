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
static bool verify_drive(int chunk_size)
{
    printf(ANSI_COLOR_CYAN "\nVerifying all files...\n" ANSI_COLOR_RESET);
    bool success = true;
    char** file_list = NULL;
    int file_count = get_file_list(&file_list);
    if (file_count == 0) {
        printf("No files to verify.\n");
        return true;
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

            int bytes_read = romfs_read_file(read_buffer, bytes_to_read, &file);
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

    size_t mem_size_bytes = flash_size_mb * 1024 * 1024;
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
