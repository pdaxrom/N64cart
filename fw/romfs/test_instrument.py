#!/usr/bin/env python3
"""Count traversals and catalog copies in a generated ROMFS core."""

import argparse
import json
import re
from pathlib import Path


def instrument(source: str, name: str, counter: str, load: str =
               "uint32_t next = from_lsb16(flash_map_int[sector]);", expected: int = 1) -> str:
    pattern = re.compile(
        rf"^static\s+(?:bool|uint32_t)\s+{re.escape(name)}\([^;{{]*\)\n\{{.*?^\}}",
        re.MULTILINE | re.DOTALL,
    )
    matches = list(pattern.finditer(source))
    if len(matches) != 1:
        raise ValueError(f"Expected one definition of {name}, found {len(matches)}")
    match = matches[0]
    body = match.group()
    if body.count(load) != expected:
        raise ValueError(f"Expected {expected} instrumentation sites in {name}")
    body = body.replace(load, f"{counter}++; {load}")
    return source[:match.start()] + body + source[match.end():]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path(__file__).with_name("romfs.c"))
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    source = args.source.read_text()
    # Support the preceding core too, for a reproducible before/after comparison.
    validator = "romfs_validate_chain_with_cursor" if "romfs_validate_chain_with_cursor(" in source else "romfs_validate_chain"
    source = instrument(source, validator, "romfs_test_validation_links")
    source = instrument(source, "romfs_sector_at_index", "romfs_test_position_links")
    source = instrument(source, "romfs_find_free_sector", "romfs_test_allocation_checks",
                        "if (flash_map_int[i] == 0xffff) {", 2)
    source = instrument(source, "romfs_list_internal", "romfs_test_catalog_copies",
                        "memmove(file->entry.name, raw_entry->name, ROMFS_MAX_NAME_LEN);")
    prefix = (
        "#include <stdint.h>\n"
        "uint64_t romfs_test_validation_links;\n"
        "uint64_t romfs_test_position_links;\n"
        "uint64_t romfs_test_allocation_checks;\n"
        "uint64_t romfs_test_catalog_copies;\n"
        f"#line 1 {json.dumps(str(args.source.resolve()))}\n"
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(prefix + source)


if __name__ == "__main__":
    main()
