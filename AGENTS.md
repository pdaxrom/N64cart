# Repository Guidelines

## Project Structure & Module Organization
Firmware sources live in `fw/`, with board support in `fw/board`, USB helpers in `fw/usb`, and the rom-focused filesystem in `fw/romfs`. Hardware collateral sits in `hw/`, documentation in `docs/`, cartridge ROM assets in `rom/`, and host utilities under `utils/`. Keep generated build artifacts confined to `fw/build` or the top-level `build-*` directories and out of version control.

## Build, Test, and Development Commands
Configure Pico firmware from a clean build tree:
```bash
cd fw && mkdir -p build && cd build
cmake .. [-DBOARD=v2|pico|pico-lite] [-DREGION=pal]
make -j
```
Use `make flash` when a UF2 target is available, otherwise drag `n64cart.uf2` onto the RPI-RP2 volume. Build the menu ROM with `cd rom && make` (pass `BOARD=` when targeting non-default hardware). Host-side utilities compile via `cd utils && make [SYSTEM=Windows]`.

## Coding Style & Naming Conventions
C modules use four-space indentation, brace-on-next-line function definitions, and lowercase `snake_case` identifiers for functions and variables (`romfs_flash_sector_write`). Constants and macros stay uppercase (`ROMFS_NOERR`). Group standard headers before project headers and guard optional features with clear `#if defined(...)` blocks. Mirror the layout in `fw/main.c` and `fw/romfs/romfs.c` when introducing new components.

## Testing Guidelines
Regression coverage for the filesystem is managed through `fw/romfs/test.sh`, which formats a temporary image and round-trips sample assets:
```bash
cd fw/romfs
./test.sh test.img samples/
```
Keep sample inputs small and deterministic; add new cases in `fw/romfs/testfile.txt` or adjacent fixtures. For firmware-side changes, exercise hardware smoke tests (USB passthrough, LED control, PI bus transfers) on physical carts before opening a pull request, and document any missing hardware validation.

## Commit & Pull Request Guidelines
Follow the existing imperative, concise commit messages seen in `git log` (e.g., `fix garbage collector performance`). Each commit should be scoped narrowly and build green. Pull requests need: a short summary of the change, testing notes (commands run and hardware targets), linked issues when relevant, and screenshots or serial logs for user-visible behavior. Flag board-specific or PAL-only changes in the description so reviewers can verify on matching hardware.

## Configuration & Safety Notes
Board selection and region flags materially change flash layouts; always restate the chosen `BOARD`/`REGION` options in reviews so collaborators can reproduce your environment. Do not commit generated UF2, ROM, or image files - store them under `build/` and add to `.gitignore` when needed. Secrets or USB serial logs containing personal data must be scrubbed before upload.
