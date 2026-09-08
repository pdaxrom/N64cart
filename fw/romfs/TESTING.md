# Host ROMFS tests

Run these commands from the repository root:

```sh
make -C fw/romfs all
make -C fw/romfs check
```

The default build uses AddressSanitizer and UndefinedBehaviorSanitizer, with
sanitizer recovery disabled. Executables, objects and logs go to the ignored
`build-romfs-tests/` directory. `make check` runs the flash emulator, geometry, chain corruption,
platform callback/USB, and process-level runner checks, including the full ROMFS suite. It requires
Python 3 and Bash in addition to the C compiler.

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
`NO_SPACE` / `NO_FREE_ENTRIES` outcomes; they do not yet require preservation of
a partial file after ENOSPC. Passing this suite is not proof that the defects
identified in the ROMFS review have been fixed.

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
and physical USB/PI smoke tests are separate. General core I/O error propagation
and partial-file preservation remain separate fixes.

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
Open-handle ownership, general I/O failures and partial writes are separate work.

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
suite, even if the core ignores the callback's `false`. A requested injection
that is never reached also fails the run. Future tests for core error handling
can use the emulator API directly and explicitly assert expected failures.

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
turn it into success. The host ROMFS CLI still has known incomplete error exit
codes, pending the separate CLI/I/O fixes. The script therefore compares every
pulled file and detects missing outputs even when that CLI returns zero.

The checks in `test_runner.py` run against fresh generated fixtures under the
build directory. They compare repeated seeds, verify all size results, test
invalid arguments and deliberate failures, and exercise script cleanup after
command errors, corrupted pulls and missing output files. Logs are saved in
`build-romfs-tests/logs/`; generated fixture images are removed afterwards.

These are host tests. No BOARD/REGION is selected, no cartridge is accessed,
and firmware/ROM builds and hardware USB/PI/LED checks remain separate.
