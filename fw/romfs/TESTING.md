# Host ROMFS tests

Run these commands from the repository root:

```sh
make -C fw/romfs all
make -C fw/romfs check
```

The default build uses AddressSanitizer and UndefinedBehaviorSanitizer, with
sanitizer recovery disabled. Executables, objects and logs go to the ignored
`build-romfs-tests/` directory. `make check` runs the flash emulator checks and
the process-level runner checks, including the full ROMFS suite. It requires
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

The size selector accepts integers from 2 through 256 MiB. It does not imply
that all geometries work in the current ROMFS implementation. The known small
flash geometry defects and the 256 MiB sentinel/GC defect need separate core
fixes and regression cases. Existing capacity tests accept documented
`NO_SPACE` / `NO_FREE_ENTRIES` outcomes; they do not yet require preservation of
a partial file after ENOSPC. Passing this suite is not proof that the defects
identified in the ROMFS review have been fixed.

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
