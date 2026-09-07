#!/usr/bin/env python3
"""Run production flash callbacks and USB dispatch against host hardware stubs."""

import argparse
from pathlib import Path
import re
import shlex
import subprocess


def extract(path, name):
    source = path.read_text()
    pattern = rf"^(?:static )?(?:inline )?(?:void|bool|uint32_t|uint16_t) {name}\([^;]*?\)\n\{{.*?^\}}"
    found = list(re.finditer(pattern, source, re.MULTILINE | re.DOTALL))
    if len(found) != 1:
        raise RuntimeError(f"Expected one definition of {name} in {path}")
    line = source.count("\n", 0, found[0].start()) + 1
    return f'#line {line} "{path}"\n{found[0].group()}\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--cflags", default="-O1 -g -Wall -Wextra -Wpedantic")
    parser.add_argument("--sanitizers", default="-fsanitize=address,undefined -fno-sanitize-recover=all")
    args = parser.parse_args()
    source = Path(__file__).resolve().parent
    root = source.parent.parent
    for platform, directory in (("arm", "fw"), ("n64", "rom/src")):
        build = args.build_dir.resolve() / ("platform-" + platform)
        build.mkdir(parents=True, exist_ok=True)
        main_file = root / directory / "main.c"
        usb_file = root / directory / "usb/dev_lowlevel.c"
        functions = []
        if platform == "n64":
            functions += [extract(main_file, name) for name in ("flash_access_lock", "flash_access_unlock")]
            functions += [extract(usb_file, name) for name in ("reverser16", "reverser32")]
        functions += [extract(main_file, name) for name in (
            "get_romfs_start_offset", "romfs_flash_sector_writable", "romfs_flash_sector_erase", "romfs_flash_sector_write",
        )]
        functions.append(extract(usb_file, "ep1_out_handler"))
        (build / "platform_functions.inc").write_text("\n".join(functions))
        command = [*shlex.split(args.cc), *shlex.split(args.cflags), *shlex.split(args.sanitizers),
                   "-Wno-unused-function", "-Wno-unused-variable", "-I", str(build),
                   f"-DTEST_N64={int(platform == 'n64')}", str(source / "test_platform.c"),
                   "-o", str(build / "test_platform")]
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=60)
        (build / "build.log").write_text(result.stdout)
        if result.returncode:
            raise RuntimeError(f"{platform}: build failed; see {build / 'build.log'}")
        result = subprocess.run([build / "test_platform"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, timeout=60)
        (build / "test.log").write_text(result.stdout)
        print(result.stdout, end="", flush=True)
        if result.returncode:
            raise RuntimeError(f"{platform}: test failed; see {build / 'test.log'}")


if __name__ == "__main__":
    main()
