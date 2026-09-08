#!/usr/bin/env python3
"""Exercise the test runners as processes, including their failure paths."""

import argparse
import os
from pathlib import Path
import re
import resource
import signal
import subprocess
import tempfile


ANSI = re.compile(r"\x1b\[[0-9;]*m")
SUCCESS = "All tests completed successfully!"
SANITIZER = re.compile(r"AddressSanitizer|LeakSanitizer|UndefinedBehaviorSanitizer|runtime error:")


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("../../build-romfs-tests"))
    args = parser.parse_args()
    build = args.build_dir.resolve()
    source = Path(__file__).resolve().parent
    logs = build / "logs"
    logs.mkdir(parents=True, exist_ok=True)

    def run(label, command, expected=0, env=None, cwd=None, preexec_fn=None):
        try:
            result = subprocess.run(
                list(map(str, command)), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, env=env, cwd=cwd, timeout=300, preexec_fn=preexec_fn,
            )
        except subprocess.TimeoutExpired as error:
            (logs / (label + ".log")).write_bytes(error.stdout or b"")
            raise RuntimeError(f"{label}: timed out; see {logs}") from error
        (logs / (label + ".log")).write_text(result.stdout)
        require(result.returncode == expected,
                f"{label}: exit {result.returncode}, expected {expected}; see {logs / (label + '.log')}")
        require(not SANITIZER.search(result.stdout), f"{label}: sanitizer diagnostic; see {logs}")
        return ANSI.sub("", result.stdout)

    runner = build / "test"
    full = run("suite", [runner])
    require(re.findall(r"\[PASS\] (\d+)MB seed=1", full) == ["16", "32", "64", "128", "256"],
            "suite: not every size completed")
    require("Suites: 5 passed, 0 failed." in full and SUCCESS in full, "suite: wrong summary")
    for size in (2, 4, 8):
        output = run(f"suite-{size}", [runner, "--flash-mb", size])
        require(f"[PASS] {size}MB seed=1" in output and SUCCESS in output, "small flash suite failed")

    single = run("seed-1-16", [runner, "--flash-mb", "16", "--seed", "1"])
    repeated = run("seed-1-16-repeat", [runner, "--flash-mb", "16", "--seed", "1"])
    require(single == repeated, "same seed produced different output")

    def section(output):
        start = output.index("      Testing with 16MB Flash Image")
        end = output.index("[PASS] 16MB seed=1", start) + len("[PASS] 16MB seed=1")
        return output[start:end]

    require(section(full) == section(single), "standalone size differs from the full suite")
    for seed in (0, 4294967295):
        output = run(f"seed-{seed}-16", [runner, "--seed", seed, "--flash-mb", "16"])
        require(f"[PASS] 16MB seed={seed}" in output, "seed was not used")
        require(output != single, "different seed did not change output")
    print("PASS: full suite and deterministic standalone runs", flush=True)

    invalid = [
        ["--seed"], ["--seed", ""], ["--seed", "-1"], ["--seed", "+1"],
        ["--seed", "4294967296"], ["--seed", "18446744073709551616"], ["--seed", "1x"],
        ["--flash-mb", "0"], ["--flash-mb", "1"], ["--flash-mb", "257"],
        ["--flash-mb", "16x"], ["--unknown", "1"], ["--fail-io", "read:0"],
        ["--fail-io", "write:-1"], ["--fail-io", "read:18446744073709551616"],
        ["--fail-io", "unknown:1"], ["--fail-io", "read"], ["--fail-io", "read:1:2"],
    ]
    for i, options in enumerate(invalid):
        output = run(f"invalid-{i}", [runner, *options], expected=2)
        require("Testing with" not in output and SUCCESS not in output, "invalid options ran tests")
    run("help", [runner, "--help"])

    for operation in ("read", "erase", "write"):
        output = run(f"fail-{operation}", [runner, "--flash-mb", "16", "--fail-io", operation + ":1"], expected=1)
        require("injected=1" in output and "[FAIL] 16MB" in output, "injected failure was missed")
        require(SUCCESS not in output and "[PASS]" not in output, "failed run reported success")
    output = run("fail-all", [runner, "--fail-io", "read:1"], expected=1)
    require("Suites: 0 passed, 5 failed." in output and SUCCESS not in output,
            "runner did not aggregate failures across every size")
    output = run("fail-unreached", [runner, "--flash-mb", "16", "--fail-io", "read:18446744073709551615"], expected=1)
    require("requested I/O failure was not reached" in output and SUCCESS not in output,
            "unreached injection silently passed")
    print("PASS: invalid arguments, injected I/O failures and failure summaries", flush=True)

    # Every file and image used here belongs to this temporary test directory.
    with tempfile.TemporaryDirectory(prefix="runner-", dir=build) as work_name:
        work = Path(work_name)
        samples = work / "samples with spaces"
        samples.mkdir()
        fixtures = {
            "binary.bin": bytes(range(256)) * 17,
            "name with spaces.txt": b"spaces in names\n",
            "-leading-dash": b"literal dash\n",
            ".hidden": b"hidden sample\n",
            "$literal;name": b"literal shell characters\n",
            "empty": b"",
        }
        for name, content in fixtures.items():
            (samples / name).write_bytes(content)
        (samples / "ignored-directory").mkdir()
        empty = work / "empty-directory"
        empty.mkdir()
        scratch = work / "scratch image.img"
        temp = work / "temporary output"
        temp.mkdir()
        env = os.environ.copy()
        env.update(ROMFS_BIN=str(build / "romfs"), TMPDIR=str(temp))
        script = source / "test.sh"
        run("shell-syntax", ["bash", "-n", script])
        output = run("round-trip", ["bash", script, scratch, samples], env=env, cwd=work)
        require(f"Round-trip passed ({len(fixtures)} files)." in output, "round-trip summary missing")
        require(not list(temp.iterdir()), "successful round-trip left temporary output")

        wrapper = work / "romfs wrapper.sh"
        wrapper.write_text('''#!/bin/bash
set -euo pipefail
if [[ ${FAIL_COMMAND:-} == "$2" ]]; then exit 23; fi
if [[ ${OMIT_PULL:-0} == 1 && $2 == pull ]]; then exit 0; fi
"$REAL_ROMFS" "$@"
if [[ ${CORRUPT_PULL:-0} == 1 && $2 == pull ]]; then printf 'X' >> "$4"; fi
''')
        wrapper.chmod(0o755)
        wrapped = dict(env, ROMFS_BIN=str(wrapper), REAL_ROMFS=str(build / "romfs"))
        for command in ("format", "push", "pull"):
            output = run("shell-fail-" + command, ["bash", script, scratch, samples], expected=23,
                         env=dict(wrapped, FAIL_COMMAND=command))
            require("Round-trip passed" not in output, "command failure reported success")
            require(not list(temp.iterdir()), "command failure left temporary output")
        output = run("shell-corrupt", ["bash", script, scratch, samples], expected=1,
                     env=dict(wrapped, CORRUPT_PULL="1"))
        require("Round-trip passed" not in output and not list(temp.iterdir()), "cmp failure was masked")
        output = run("shell-missing-pull", ["bash", script, scratch, samples], expected=2,
                     env=dict(wrapped, OMIT_PULL="1"))
        require("Round-trip passed" not in output and not list(temp.iterdir()), "missing output was masked")

        bad_script_args = [[], [scratch], [scratch, work / "missing"], [scratch, empty],
                           [samples / "binary.bin", samples], [scratch, samples, "extra"]]
        for i, options in enumerate(bad_script_args):
            run(f"shell-invalid-{i}", ["bash", script, *options], expected=2, env=env)
        run("shell-no-binary", ["bash", script, scratch, samples], expected=2,
            env=dict(env, ROMFS_BIN=str(work / "missing-executable")))
        require(not list(temp.iterdir()), "argument errors left temporary output")
        for name, content in fixtures.items():
            require((samples / name).read_bytes() == content, f"input fixture changed: {name}")

        cli = build / "romfs-small"
        image = work / "cli.img"
        missing = work / "missing.img"
        for i, options in enumerate([[], [image], [image, "unknown"], [image, "push"], [image, "format", "extra"]]):
            run(f"cli-args-{i}", [cli, *options], expected=2)
        run("cli-missing-image", [cli, missing, "list"], expected=1)
        require(not missing.exists(), "read-only command created a missing image")
        run("cli-format", [cli, image, "format"])
        require(image.stat().st_size == 2 * 1024 * 1024, "wrong small CLI image size")
        run("cli-push", [cli, image, "push", samples / "binary.bin", "data"])
        before = image.read_bytes(), image.stat().st_mtime_ns
        local = work / "cli-pull.bin"
        run("cli-list-root", [cli, image, "list", "/"])
        run("cli-free", [cli, image, "free"])
        run("cli-pull", [cli, image, "pull", "data", local])
        require(local.read_bytes() == fixtures["binary.bin"], "CLI readback differs")
        run("cli-missing-pull", [cli, image, "pull", "absent", local], expected=1)
        require(local.read_bytes() == fixtures["binary.bin"], "failed remote open truncated local output")
        require(before == (image.read_bytes(), image.stat().st_mtime_ns), "read-only CLI changed the image")
        run("cli-output-error", [cli, image, "pull", "data", samples], expected=1)
        run("cli-input-error", [cli, image, "push", samples, "bad-input"], expected=1)
        run("cli-missing-input", [cli, image, "push", missing, "absent"], expected=1)
        run("cli-missing-delete", [cli, image, "delete", "absent"], expected=1)
        run("cli-save-error", [cli, work / "missing-dir" / "image", "format"], expected=1)
        def limit_output_size():
            signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
            resource.setrlimit(resource.RLIMIT_FSIZE, (1024, 1024))

        run("cli-save-write-error", [cli, work / "limited.img", "format"], expected=1,
            preexec_fn=limit_output_size)
        run("cli-pull-close-error", [cli, image, "pull", "data", local], expected=1,
            preexec_fn=limit_output_size)
        for size in (0, 17, 2 * 1024 * 1024 + 1):
            invalid_image = work / "invalid.img"
            invalid_image.write_bytes(b"x" * size)
            run(f"cli-invalid-image-{size}", [cli, invalid_image, "format"], expected=1)
            require(invalid_image.stat().st_size == size, "invalid image was overwritten")
        run("cli-capacity-format", [cli, image, "format"])
        capacity_text = run("cli-capacity", [cli, image, "free"])
        capacity = int(re.search(r"Free space: (\d+) bytes", capacity_text).group(1))
        large = work / "large.bin"
        data = (bytes(range(256)) * ((capacity + 256) // 256))[:capacity + 1]
        large.write_bytes(data)
        run("cli-enospc", [cli, image, "push", large, "partial"], expected=1)
        run("cli-partial-pull", [cli, image, "pull", "partial", local])
        require(local.read_bytes() == data[:capacity], "ENOSPC CLI lost accepted prefix")
        run("cli-partial-delete", [cli, image, "delete", "partial"])
        require(run("cli-reclaimed", [cli, image, "free"]) == capacity_text, "ENOSPC CLI leaked sectors")
    print("PASS: shell round-trip, quoting, error propagation and cleanup", flush=True)
    print("PASS: CLI exit codes, read-only commands, host I/O errors and ENOSPC persistence", flush=True)
    print(f"Runner checks passed. Logs: {logs}")


if __name__ == "__main__":
    main()
