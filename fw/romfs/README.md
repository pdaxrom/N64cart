# ROMFS API Overview

The ROMFS layer provides a lightweight flash-backed filesystem for the N64 cart firmware, exposing a C API that maps files and pseudo-directories onto the SPI flash. This README summarises the available calls and shows how to integrate them into firmware or host tools.

- [Core Concepts](#core-concepts)
- [Initialization](#initialization)
- [Filesystem Queries](#filesystem-queries)
- [Directory Navigation](#directory-navigation)
- [File Operations](#file-operations)
- [Flash Access Primitives](#flash-access-primitives)
- [Error Handling](#error-handling)
- [Example: Creating and Reading a File](#example-creating-and-reading-a-file)
- [Example: Recursively Listing the ROMFS Tree](#example-recursively-listing-the-romfs-tree)

## Core Concepts

ROMFS stores a flat table of `romfs_entry` records that describe files and pseudo-directories. Each entry contains:

| Field | Purpose |
|---|---|
| `name`, `attr` | ASCII name (max 53 chars) and packed mode/type bits |
| `start`, `size` | First sector in the flash map chain and file size (bytes) |

Entries can represent either files or pseudo-directories. Each directory receives an ID (`attr.names.current`) that children reference via `attr.names.parent`, allowing the API to efficiently walk or filter the flat entry table without nested structures. The ROMFS core keeps a lookup table of directory IDs so obtaining a `romfs_dir` handle is O(1), and helpers such as `romfs_dir_open_path` or `romfs_create_path` split incoming paths on `/`, create intermediate directories if requested, and reject `.`/`..` segments to keep the tree well-defined. Callers that prefer a flat namespace can keep passing bare filenames (the root directory is implied), while directory-aware code can scope operations using `romfs_dir` handles or path-based helpers.

Every entry is backed by a 4 KiB sector map (`flash_map_int`) and a list block (`flash_list_int`). The API reads these blocks into RAM and writes them back when changes occur.

## Initialization

```c
#include "romfs.h"

uint32_t map_size, list_size;
romfs_get_buffers_sizes(flash_bytes, &map_size, &list_size);

uint16_t *flash_map = malloc(map_size);
uint8_t  *flash_list = malloc(list_size);

if (!romfs_start(flash_base_offset, flash_bytes, flash_map, flash_list)) {
    // handle error
}
```

`romfs_start` must run before any other call. It loads the list/map into RAM and rebuilds internal directory indices.

## Filesystem Queries

- `uint32_t romfs_free(void);` - returns available bytes.
- `uint32_t romfs_list(romfs_file *entry, bool first);` - iterates over all entries (deprecated for directory-aware apps; use `romfs_list_dir` instead).
- `const char *romfs_strerror(uint32_t err);` - converts ROMFS error codes into strings.

The `romfs_file` struct acts both as an iterator and an open file handle. Fields of interest:

| Field | Meaning |
|---|---|
| `entry` | Snapshot of `romfs_entry` data (name, size, attributes) |
| `nentry` | Index in the entry list |
| `err` | Last operation result (`ROMFS_NOERR`, `ROMFS_ERR_*`) |

## Directory Navigation

ROMFS supports up to 16 pseudo-directories using the `romfs_dir` handle:

```c
romfs_dir root, subdir;
romfs_dir_root(&root);

if (romfs_dir_open(&root, "games", &subdir) == ROMFS_NOERR) {
    // `subdir` now refers to /games
}

// Create nested directories, allocating IDs on demand
romfs_dir saves;
romfs_dir_create(&root, "saves", &saves);
```

Directory-aware listing is performed with `romfs_list_dir`:

```c
romfs_file entry = {0};
if (romfs_list_dir(&entry, true, &subdir, true) == ROMFS_NOERR) {
    do {
        bool is_dir = entry.entry.attr.names.type == ROMFS_TYPE_DIR;
        printf("%s%s\n", entry.entry.name, is_dir ? "/" : "");
    } while (romfs_list_dir(&entry, false, &subdir, true) == ROMFS_NOERR);
}
```

Helper routines:

| Function | Description |
|---|---|
| `romfs_dir_root` | Returns the root directory handle (`/`) |
| `romfs_dir_open` | Resolves a single child (no `/`) beneath a parent |
| `romfs_dir_open_path` | Walks a slash-separated path (e.g. `games/pal`) |
| `romfs_dir_create` | Creates a new directory entry (fails if it exists) |
| `romfs_dir_remove` | Deletes an empty directory by handle |
| `romfs_mkdir_path` | Convenience for building paths (`create_parents` makes intermediate directories) |
| `romfs_rmdir_path` | Removes an empty directory by path |

## File Operations

Open/create via handle-based functions:

```c
uint8_t io_buffer[ROMFS_FLASH_SECTOR];

romfs_file fh;
if (romfs_create_path("games/mario.n64", &fh,
        ROMFS_MODE_READWRITE, ROMFS_TYPE_MISC,
        io_buffer, true) == ROMFS_NOERR) {
    // Use romfs_write_file to append data (partial blocks go through io_buffer)
}
```

- `romfs_create_file` / `romfs_create_file_in_dir` / `romfs_create_path`
- `romfs_open_file` / `romfs_open_file_in_dir` / `romfs_open_path`
- `romfs_open_append` / `romfs_open_append_in_dir` / `romfs_open_append_path`
- `romfs_rename` / `romfs_rename_in_dir` / `romfs_rename_path`
- `romfs_delete` / `romfs_delete_in_dir` / `romfs_delete_path`
- `romfs_seek_file` / `romfs_tell_file`

Append handles behave like `fopen("a")`: the file is created if missing, the write cursor starts at the current end, and partially filled sectors are rewritten in-place before new sectors are chained on.

Rename helpers let you move files or directories between parents while updating their names in one shot. When renaming directories, the API prevents moving a folder into its own subtree.

Once open:

| Function | Behaviour |
|---|---|
| `romfs_write_file(const void *buffer, uint32_t size, romfs_file *file)` | Writes up to 4 KiB at a time, buffering partial blocks |
| `romfs_read_file(void *buffer, uint32_t size, romfs_file *file)` | Reads up to 4 KiB; sets `file->err` to `ROMFS_ERR_EOF` on completion |
| `romfs_read_map_table(uint16_t *map, uint32_t count, romfs_file *file)` | Retrieves the chain of sectors used by a file |
| `romfs_close_file(romfs_file *file)` | Flushes any pending write buffers |
| `romfs_seek_file(romfs_file *file, int32_t offset, int whence)` | Repositions a read handle relative to `SEEK_SET`, `SEEK_CUR`, or `SEEK_END`; bounds-checks against the current file size |
| `romfs_tell_file(romfs_file *file, uint32_t *position)` | Reports the logical cursor for the next read (`read_offset` for read handles, flushed bytes for writers) |

Files opened for write require a sector-sized scratch buffer (`io_buffer`). When writes cannot be satisfied (disk full, buffer missing, etc.), the API automatically unlinks any new sectors to leave ROMFS consistent.

## Flash Access Primitives

ROMFS never touches the hardware directly. Instead, the platform must provide blocking implementations of the three flash hooks declared in `romfs.h`. All offsets are absolute byte addresses within the SPI flash (the same values you pass to `romfs_start`), and each function returns `true` on success to keep internal state machines moving.

| Function | Purpose |
|---|---|
| `bool romfs_flash_sector_read(uint32_t offset, uint8_t *buffer, uint32_t need)` | Copies `need` bytes starting at `offset` into `buffer`. Reads may span less than a full sector to satisfy tail fragments, but the ROMFS core only requests ranges contained inside a single 4 KiB sector. |
| `bool romfs_flash_sector_write(uint32_t offset, uint8_t *buffer)` | Programs one `ROMFS_FLASH_SECTOR`-sized block at `offset`. The buffer must already contain the fully prepared sector image; ROMFS handles read/modify cycles in RAM before calling into this hook. |
| `bool romfs_flash_sector_erase(uint32_t offset)` | Erases a single 4 KiB sector aligned to `ROMFS_FLASH_SECTOR`. Implementations must block until the device reports the sector is blank (typically all `0xff`). |

The ROMFS formatter and metadata loader call these hooks during `romfs_start` and `romfs_format`, and file I/O uses them whenever sectors roll over. Hardware ports usually wrap lower-level flash drivers or USB bridge commands:

```c
bool romfs_flash_sector_write(uint32_t offset, uint8_t *buffer)
{
    flash_wait_ready();
    flash_cs_assert();
    flash_program_sector(offset, buffer, ROMFS_FLASH_SECTOR);
    flash_cs_release();
    return flash_check_status() == FLASH_OK;
}
```

Returning `false` from any primitive propagates `ROMFS_ERR_OPERATION` to the caller, keeping higher layers aware of transport failures or protection faults.

## Error Handling

Common error codes:

| Code | Meaning |
|---|---|
| `ROMFS_NOERR` | Success |
| `ROMFS_ERR_NO_IO_BUFFER` | Missing sector buffer for read/write |
| `ROMFS_ERR_NO_ENTRY` | Entry not found |
| `ROMFS_ERR_NO_FREE_ENTRIES` | Entry table exhausted |
| `ROMFS_ERR_NO_SPACE` | Flash has no free sectors |
| `ROMFS_ERR_FILE_EXISTS` | Creating a file that already exists |
| `ROMFS_ERR_BUFFER_TOO_SMALL` | Provided map/table buffers are too small |
| `ROMFS_ERR_DIR_LIMIT` | Out of directory IDs (max 16) |
| `ROMFS_ERR_DIR_NOT_EMPTY` | Attempted to remove a non-empty directory |

Inspect `romfs_file.err` or return values after every call. Convert to text with `romfs_strerror`.

## Example: Creating and Reading a File

```c
uint8_t io_buffer[ROMFS_FLASH_SECTOR];
romfs_file fh;

// Create nested directories and file
romfs_mkdir_path("saves/profile", true, NULL);
if (romfs_create_path("saves/profile/slot1.bin",
        &fh, ROMFS_MODE_READWRITE, ROMFS_TYPE_MISC,
        io_buffer, false) == ROMFS_NOERR) {
    const char payload[] = "hello romfs";
    romfs_write_file(payload, sizeof(payload), &fh);
    romfs_close_file(&fh);
}

// Later: open for reading
if (romfs_open_path("saves/profile/slot1.bin", &fh, io_buffer) == ROMFS_NOERR) {
    char buffer[32];
    if (romfs_read_file(buffer, sizeof(buffer), &fh) > 0 &&
        fh.err == ROMFS_ERR_EOF) {
        // buffer contains the payload
    }
}
```

## Example: Recursively Listing the ROMFS Tree

```c
static void list_dir(const romfs_dir *dir, const char *prefix)
{
    romfs_file entry = {0};
    if (romfs_list_dir(&entry, true, dir, true) != ROMFS_NOERR) {
        return;
    }

    do {
        bool is_dir = entry.entry.attr.names.type == ROMFS_TYPE_DIR;
        printf("%s%s%s\n", prefix, entry.entry.name, is_dir ? "/" : "");

        if (is_dir) {
            romfs_dir child;
            if (romfs_dir_open(dir, entry.entry.name, &child) == ROMFS_NOERR) {
                char next[256];
                snprintf(next, sizeof(next), "%s%s/", prefix, entry.entry.name);
                list_dir(&child, next);
            }
        }
    } while (romfs_list_dir(&entry, false, dir, true) == ROMFS_NOERR);
}

romfs_dir root;
romfs_dir_root(&root);
list_dir(&root, "/");
```

This produces a tree-style listing similar to:

```
/games/
/games/mario.n64
/saves/
/saves/profile/
/saves/profile/slot1.bin
```

---

For more examples, see:

- `utils/usb-romfs.c` - host utility supporting directory creation and recursive uploads
- `newlib-romfs.c` - registers `romfs:/` with newlib via `attach_filesystem`, letting firmware use `fopen`, `mkdir`, `chdir`, etc. without touching the raw API

The ROMFS layer is intentionally minimal; feel free to extend this README as new helpers or workflows are introduced. Bugs and pull requests are welcome!
