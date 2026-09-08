# Host ROMFS tests

Run these commands from the repository root:

```sh
make -C fw/romfs all
make -C fw/romfs check
```

The default build uses AddressSanitizer and UndefinedBehaviorSanitizer, with
sanitizer recovery disabled. Executables, objects and logs go to the ignored
`build-romfs-tests/` directory. `make check` runs the flash emulator, geometry,
chain corruption, handle ownership, I/O failures, write counts/ordering, newlib bridge, platform callback/USB, and
process-level runner checks, including the full ROMFS suite. It requires
Python 3 and Bash on a POSIX host in addition to the C compiler. CLI tests use
`RLIMIT_FSIZE` to exercise real host write/close failures.

`BUILD_DIR` overrides the output directory; choose a top-level `build-*`
directory. `ROMFS_BIN` overrides the CLI executable used by `test.sh`.
Clean before changing compiler, flags or the CLI's `FLASH` setting. `make clean`
removes the named binaries, objects and dependency files, preserving logs and
scratch images. `SANITIZERS=` can disable instrumentation for a separate build.

## Reproduce a suite run

```sh
build-romfs-tests/test
build-romfs-tests/test --seed 123 --flash-mb 16
build-romfs-tests/test --help
```

The default seed is 1, and the default sizes are 16/32/64/128/256 MiB. The runner
prints the seed, per-size result, flash callback counts, and final PRNG state.
Each size starts with its own seed-derived PRNG state, so selecting one size
reproduces its part of the full run. Randomness uses explicit 32-bit arithmetic
and does not depend on the host libc's `rand()` implementation.

Exit codes for the C runner:

| Code | Meaning |
|---|---|
| 0 | Every selected suite passed; or `--help` was requested |
| 1 | At least one suite failed |
| 2 | Invalid arguments |

The size selector accepts integers from 2 through 256 MiB. The process runner
also runs standalone suites for 2/4/8 MiB. The geometry regression executable
covers 2/4/8/16/32/64/128/256 MiB. Existing randomized capacity tests accept documented
`NO_SPACE` / `NO_FREE_ENTRIES` outcomes and require successful close after ENOSPC.
The I/O and CLI tests additionally require readback of the accepted prefix and
reclamation of every sector after deletion. Remaining performance review items
have separate coverage requirements.

## Geometry and platform guards

```sh
build-romfs-tests/test_geometry
python3 fw/romfs/test_platform.py --build-dir build-romfs-tests
```

The size query does not change the active mount. Flash sizes must be nonzero,
sector-aligned and at most 256 MiB. Mounting additionally requires non-null
buffers (with the map aligned for `uint16_t`), space for metadata after the
32 KiB aligned firmware boundary, and at least one usable data sector.
Callers remain responsible for providing buffers of the queried sizes.
Rejected geometry must cause no I/O and preserve the previous mount and buffers.

Geometry tests check the existing metadata layout, exact free space, allocation
of the last usable sector, ENOSPC, GC reuse and remount/readback. A valid large
file chain is constructed directly and persisted to reach capacity quickly;
this is not a full-device payload write test. Map padding is excluded from
allocation. At 256 MiB, index `65535` stays reserved because `0xffff` remains
the free/invalid sentinel. The last physical sector is untouched by ROMFS file
allocation. The disk format and metadata sizes are unchanged.

`test_platform.py` extracts the current ARM/N64 erase/write callbacks, boundary
functions and USB command handler verbatim into generated includes under the
build directory, then compiles them with `test_platform.c` hardware substitutes.
Extraction fails if a definition cannot be identified uniquely. Tests check
that invalid offsets never reach erase/program or mode/interrupt changes;
metadata and the last physical sector remain accessible through valid raw
commands. They also check low-level failure returns, restoration of N64 mode
and interrupts, early USB rejection, the second check at write completion,
error ACK conversion, and recovery with a following valid command.

The N64 host harness models big-endian field values with explicit byte swaps;
it does not emulate MIPS execution or a USB controller. Full ARM/MIPS builds
and physical USB/PI smoke tests are separate. Core I/O error propagation and
partial-file preservation are exercised by `test_io` and the bridge/CLI tests.

## Corrupted chains

`build-romfs-tests/test_chains` arranges deterministic corrupt images, persists
them and remounts before testing. It also exercises handles opened before the
corruption, dirty write buffers, and live corruption of service links.
Coverage includes invalid start/next indices, map padding, links into service
regions, early termination, missing terminal self-links, cycles, wrong sizes,
arithmetic overflow, and mismatched buffer/chain sectors.

ROMFS validates a complete chain against its size before open/read/seek/map
export and file mutations. A normal empty file uses start `0xffff`; a nonempty
file must have exactly the required number of sectors and a final self-link.
Service files are readable only with their expected geometry and within their
own service range. Invalid chains return `ROMFS_ERR_OPERATION`. Read/write
return byte counts, including zero on rejection, with the error in `file->err`.
Rejected reads and map exports leave output buffers and positions unchanged.

Truncate validates before syncing a dirty buffer or changing the catalog/map.
GC preflights every eligible deleted file before freeing any of them, and
propagates a corrupt-chain error through catalog/sector allocation. If a corrupt
tombstone blocks GC, `romfs_free()` reports only sectors already marked free.
The tests compare the entire image, map and catalog and require zero callbacks
on these rejection paths. They verify that valid GC works after the fixture's
bad link is repaired; production code does not repair corrupt user chains.

Validation uses a bounded walk and no additional bitmap or heap allocation.
It currently adds a full chain walk to each read/write/seek call, so small I/O
calls on large files cost more. Caching with correct invalidation belongs to
the planned performance work. This checks each chain's structure and range,
not ownership of sectors shared by otherwise structurally valid files.

## Handle ownership and names

```sh
build-romfs-tests/test_ownership
build-romfs-tests/test_newlib
```

Successful create/open registers the descriptor at its address. The caller must
keep it alive and stationary until close, and close it on every error path before
leaving scope or freeing it. Copying a descriptor does not create another usable
handle. Reopening or listing into an active descriptor returns `ROMFS_ERR_BUSY`
without overwriting it. No heap allocation or fixed handle-count limit is added
to the core; the registry is an intrusive list. Calls still require external
serialization; this does not make the filesystem thread safe.

New files reserve a catalog slot and name before their first flush/close, so two
unflushed creates cannot claim the same slot. Multiple ordinary readers may
coexist, but a writer excludes other ordinary opens of that file. Delete and
rename reject open files with `ROMFS_ERR_BUSY`. Pending children prevent directory
removal, and open descendants prevent directory rename. File and directory names
share a namespace; invalid components are rejected before automatic parent
creation. Names must have 1–53 bytes, contain no slash, differ from `.` and `..`,
and not start with the empty/deleted markers. This does not repair old images
that already contain duplicate or invalid names.

`romfs_open_read_view` flushes a writer and opens a temporary read-only view.
The writer cannot write/flush/truncate until the view closes. Newlib uses this
for `O_RDWR`, closes the view after each read (including EOF/error), then updates
the owning writer's position. Ordinary conflicts map to `EBUSY`. Closing a file
unregisters it even if flush returns an error; flush itself retains ownership.
A closed descriptor must be reopened before further I/O. Repeated close is a
no-op. Partial-write recovery and flash callback errors are described below.

Directory IDs are released only at deletion; GC must not release an ID now owned
by another directory. Directory handles carry a runtime generation and catalog
index, preventing stale handles/cursors from accessing a replacement even when
both ID and slot are reused. Mount with valid geometry and format invalidate
previous file/non-root directory handles, including on a subsequent I/O failure;
a geometry-rejected mount preserves them.
The in-memory `romfs_file` and `romfs_dir` ABI changed, so clients must be rebuilt.
The on-flash `romfs_entry` layout, map and sentinel values are unchanged.

Ownership tests exercise reverse-order close/remount of two pending writers,
shared readers, stale copies, directory ID/slot reuse through actual catalog GC,
all reserved catalog slots, invalid names, mount invalidation and read views.
Ownership-specific failed opens/closes use invalid buffers/operations; `test_io`
injects flash failures. Rejected ownership operations compare the entire image, map and
catalog and check that no flash callback ran. Corruption tests use actual open
descriptors for live damage, closing each before opening a conflicting writer.

The newlib harness compiles the production `newlib-romfs.c` with minimal libdragon
callback declarations in `test_newlib_stubs/`; it does not copy bridge logic.
Host-only symbol renaming keeps its `rename`/`rmdir` away from host libc calls.
Tests cover alternating `O_RDWR` reads/writes/seeks across sectors, EOF, read
errors, truncate, conflicting opens, preservation of both rename endpoints,
failed-close cleanup and stale directory cursors. Full MIPS builds separately
check the real libdragon headers; no MIPS runtime is emulated here.

## Newlib open flags

The bridge creates files only with `O_CREAT`, including `O_RDONLY | O_CREAT`.
Without it, a missing file or parent returns `ENOENT`. With it, the existing
automatic parent-creation behavior is preserved. `O_CREAT | O_EXCL` rejects both
published and pending names with `EEXIST`, in all three access modes. `O_EXCL`
without `O_CREAT` is ignored. An invalid access mode or read-only `O_TRUNC`
returns `EINVAL` before any filesystem changes.

Existing writable files open through `romfs_open_write_path`, which never creates
a file or parent and starts at offset zero. The low-level `romfs_open_append*`
APIs retain their create-if-missing behavior and initial EOF position. Newlib
handles start at zero, including `O_APPEND`: append seeks to the current EOF
before every write, even after reads, seeks or truncate.

`O_TRUNC` acquires the writer and calls `romfs_truncate_file(..., 0)`, preserving
the catalog slot, name, parent and attributes. `O_APPEND | O_TRUNC` truncates
first. Write opens of read-only/system/reserved or service files return `EACCES`;
directory opens return `EISDIR`. Exclusive creation of an occupied name takes
precedence over these errors. Busy files return `EBUSY` before truncation.

The bridge tests cover 192 flag combinations across existing files, missing
files with an existing parent, and missing parents. They check sizes, contents,
offsets, access failures, slot/attribute preservation and remount readback.
Additional cases cover append after read/seek/truncate, pending exclusive
creation, busy truncation, protected entries and directories. Rejected flag,
access and ownership checks require an unchanged image/map/catalog and zero
flash callbacks. Fault injection checks both metadata sectors for read-only
creation and truncation: failures return `EIO`, preserve that errno during
cleanup, and release handles. These I/O failures can leave creation/truncation
applied in RAM; synchronization and close recovery still follow the I/O contract
below, without rollback or power-loss guarantees.

## I/O errors and partial transfers

`build-romfs-tests/test_io` injects failures into mount reads, data buffer
read/erase/write (including first writes of new sectors), truncate tail I/O, and both sectors of metadata
on a 2 MiB image. Metadata operations cover format, mkdir, delete, rmdir, rename,
file flush and close, including repeated failures before a successful retry.

Each failed flash callback returns `ROMFS_ERR_IO` through the calling operation
(or false from start/format). An erase failure prevents the corresponding write.
Dirty buffers remain dirty until erase and program both succeed. A failed load
invalidates the buffer's cached sector. Allocation reserves a sector in the RAM
map without flash I/O; its buffer is initialized to zero and filled with accepted
data. The first physical write happens at a full buffer or flush/close. Shared
sync must write every changed writer's data before publishing metadata. A failed
new-sector write retains the reservation and dirty buffer for retry; failed close
reclaims unpublished tail allocations as described below. Truncate performs tail
I/O before releasing chain links.

Read returns only successfully read fragments and advances by that count. A
failed callback may have touched its destination, so bytes beyond the returned
count are not valid. Write counts bytes accepted into the file's RAM buffer,
including a full sector whose later flush failed; inspect `file->err` even when
the return is nonzero. Successful flush/close confirms callback completion for
accepted data and metadata. ENOSPC is a short-write condition, not a reason for
close to discard the accepted file. Close reports its own synchronization result.

Capacity tests use a deterministic 32 KiB image to exercise create, append,
overwrite, gap extension and truncate at capacity, then remount/read back the
accepted data, delete the file and reuse all sectors. Zero-filled gap/truncate
extensions can remain partially applied on failure; failed gap filling returns
zero caller-data bytes and preserves the caller's requested position.

`romfs_sync()` synchronizes changed open writers before flushing the shared map
and catalog. A commit must not combine one writer's extended chain with its old
catalog size. Thus closing one file or changing a directory may also synchronize
other open writers and can report their errors. Tests remount after closing only
one of two writers and verify both entries and data. Unchanged writers with an
active read view need no synchronization and retain their ownership rules.

Failed metadata writes retain the pending state. Directory changes may already
exist in RAM when the operation returns an I/O error; call `romfs_sync()` before
remounting to retry them. This is not transaction rollback. Repeated mkdir of an
existing directory also retries synchronization. A failed mount read disables
access to the incomplete mount until another successful start; it cannot restore
old caller-owned buffers that have already been partially overwritten.

Before close, a failed data flush can be retried with the descriptor and its RAM
buffer intact. Close releases the descriptor even on error: for a valid chain it
reclaims tail allocations beyond the entry already in the RAM catalog, preserving
that entry's chain. Catalog/map writes still pending can be retried with
`romfs_sync()`. Invalid chains are never traversed for speculative cleanup.

Newlib tests verify short read/write results, positions, `EIO`/`ENOSPC`, failed
close and partial-file readback. Host CLI tests use a separately compiled 2 MiB
`romfs-small` executable for deterministic capacity tests, input/output failures,
write/close size limits and preservation of the local destination on remote-open
failure. Host CLI exit codes are 0 for success, 1 for operation/I/O failure, and 2
for invalid arguments. Only format may create a missing image; images of the wrong
size are rejected. `list`, `free`, and `pull` preserve image contents and mtime.

N64 save loading aborts ROM launch on read failure or incomplete save length;
open failures other than ENOENT also abort. Save writing checks fclose and logs
success only on successful transfer/flush/close. Its existing policy of deleting
failed save writes remains. Physical cartridge validation of these paths is still
required. The USB/remote CLI and GUI detect partial transfers and report errors.

These checks model callback failures, not power-loss atomicity or reliable driver
detection of every physical flash fault. Failed in-place erase/program can damage
existing data or metadata. Generation snapshots and data copy-on-write remain
deferred in `TODO.md`; the on-flash format is unchanged here.

## Single programming of new sectors

```sh
build-romfs-tests/test_write
build-romfs-tests/test_write --measure
```

The write harness links the same NOR emulator with renamed callbacks and wraps
them to count data and metadata requests separately. `--measure` reports counts
and verifies round-trip contents without requiring the optimized counts, so it
can also measure an older core. The ordinary run asserts one erase/program per
new data sector for sequential creation, including a partial last sector.

Measured on a 16 MiB image (numbers are erase/program pairs for file data):

| Operation | Before deferred allocation | Current |
|---|---:|---:|
| Create 1 MiB | 512 | 256 |
| Create 1 MiB + 17 bytes | 514 | 257 |
| Append 4096 bytes to a 17-byte file | 3 | 2 |
| Overwrite 17 bytes inside a two-sector file | 1 | 1 |
| Append 17 bytes to a one-sector file | 2 | 1 |

Each measured operation additionally writes three metadata sectors in both
versions. Reads remain zero for new sectors; appending to a partial sector and
partial overwrite each read the existing sector once. Repeated flushes or writes
to the same previously stored sector can still require another erase/program.
These counts are not elapsed-time measurements on a cartridge.

Further tests fill and delete a six-sector file to force GC reuse, then check
partial writes, zero-filled gaps and truncate extension. They verify both the
logical file and physical zero bytes beyond EOF. Two pending writers must reserve
different sectors without any I/O. Before each metadata erase/program the wrapper
checks that both data buffers already match flash, including after a failure in
either writer. Gap/truncate retries inject erase/program failures into an old
partial sector or a new sector, retaining offsets, the accepted prefix and zeros.
Existing I/O tests cover ENOSPC and reclamation after a failed close.

## Flash emulator and fault injection

`test_flash.c` supplies the host callbacks used by `test.c`. It checks physical
bounds, sector alignment, non-null buffers and programming without an erase.
It models `1→0` programming and rejects `0→1` transitions. Invalid operations
and injected failures leave both the image and the caller's read buffer
unchanged. This is an operation-level model, not a simulation of interrupted
page programming or power loss.

The emulator permits access to the firmware range within physical flash. That
allows core protection tests to detect erroneous firmware writes instead of
having the emulator silently protect the core. Direct `test_flash_data()`
access is reserved for arranging or inspecting fixtures.

```sh
build-romfs-tests/test_flash_test
build-romfs-tests/test --flash-mb 16 --fail-io read:1
build-romfs-tests/test --flash-mb 16 --fail-io erase:1
build-romfs-tests/test --flash-mb 16 --fail-io write:1
```

The three injected suite runs are expected to exit **1**. The selected callback
fails once at the specified ordinal, counting from image initialization and
including mount I/O. Any rejection or injected failure fails an ordinary
suite. A requested injection that is never reached also fails the run. The
dedicated core I/O tests use the emulator API and assert expected failures.

The API can arm failures relative to the current callback counts.
`test_flash_reset_counters()` preserves image contents and disarms failures;
initialization/destruction also resets the counters and failure settings.

## File round-trip script

```sh
fw/romfs/test.sh build-romfs-tests/scratch.img /path/to/small-samples
```

The supplied image is **formatted**. Use a disposable image outside the input
directory. The script handles ordinary files at the top level, including
hidden files, spaces and literal shell characters in names. It ignores
subdirectories and rejects an empty input set. Source files must stay unchanged
while the script runs. The temporary pull directory is removed on success,
failure and handled signals.

A command failure or `cmp` failure produces a nonzero status; cleanup cannot
turn it into success. In addition to checking CLI exit codes, the script compares
every pulled file and detects missing outputs from a misbehaving CLI wrapper.

The checks in `test_runner.py` run against fresh generated fixtures under the
build directory. They compare repeated seeds, verify all size results, test
invalid arguments and deliberate failures, and exercise script cleanup after
command errors, corrupted pulls and missing output files. Logs are saved in
`build-romfs-tests/logs/`; generated fixture images are removed afterwards.

These are host tests. No BOARD/REGION is selected, no cartridge is accessed,
and firmware/ROM builds and hardware USB/PI/LED checks remain separate.
