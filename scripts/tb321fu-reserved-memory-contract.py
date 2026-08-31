#!/usr/bin/env python3
"""Validate the TB321FU reserved-memory and ramoops DTB contract."""

from __future__ import annotations

import argparse
import contextlib
import hashlib
import os
from pathlib import Path
import stat
import subprocess
import tarfile
import tempfile
from typing import Iterator


MAX_ARCHIVE_BYTES = 512 * 1024 * 1024
MAX_ARCHIVE_MEMBERS = 200_000
MAX_DTB_BYTES = 16 * 1024 * 1024
MAX_RESERVED_NODES = 256
FDTGET_TIMEOUT_SECONDS = 10
RAMOOPS_NODE = "ramoops@a2000000"
RAMOOPS_START = 0xA2000000
RAMOOPS_SIZE = 0x400000
RAMOOPS_CONSOLE_SIZE = 0x200000
RAMOOPS_PMSG_SIZE = 0x200000


class ContractError(RuntimeError):
    pass


def regular_file(path: Path, maximum: int, label: str) -> os.stat_result:
    if path.is_symlink():
        raise ContractError(f"{label} must not be a symlink: {path}")
    try:
        metadata = path.stat()
    except OSError as error:
        raise ContractError(f"cannot stat {label} {path}: {error}") from error
    if not stat.S_ISREG(metadata.st_mode):
        raise ContractError(f"{label} is not a regular file: {path}")
    if metadata.st_size <= 0 or metadata.st_size > maximum:
        raise ContractError(
            f"{label} size {metadata.st_size} is outside 1..{maximum}: {path}"
        )
    return metadata


@contextlib.contextmanager
def materialize_dtb(archive_or_dtb: Path, member_name: str | None) -> Iterator[Path]:
    if member_name is None:
        regular_file(archive_or_dtb, MAX_DTB_BYTES, "DTB")
        yield archive_or_dtb
        return

    regular_file(archive_or_dtb, MAX_ARCHIVE_BYTES, "archive")
    matches: list[tarfile.TarInfo] = []
    try:
        with tarfile.open(archive_or_dtb, mode="r:gz") as archive:
            for count, member in enumerate(archive, 1):
                if count > MAX_ARCHIVE_MEMBERS:
                    raise ContractError(
                        f"archive exceeds {MAX_ARCHIVE_MEMBERS} members"
                    )
                if member.name == member_name:
                    matches.append(member)
            if len(matches) != 1:
                raise ContractError(
                    f"expected exactly one archive member {member_name!r}, "
                    f"found {len(matches)}"
                )
            member = matches[0]
            if not member.isfile() or member.size <= 0 or member.size > MAX_DTB_BYTES:
                raise ContractError(
                    f"DTB member must be a 1..{MAX_DTB_BYTES} byte regular file"
                )
            source = archive.extractfile(member)
            if source is None:
                raise ContractError(f"cannot read DTB member {member_name!r}")
            payload = source.read(member.size + 1)
            if len(payload) != member.size:
                raise ContractError(
                    f"DTB member length mismatch: expected {member.size}, got {len(payload)}"
                )
    except (OSError, tarfile.TarError) as error:
        raise ContractError(f"cannot read archive {archive_or_dtb}: {error}") from error

    with tempfile.TemporaryDirectory(prefix="tb321fu-dtb-contract-") as temp_root:
        dtb_path = Path(temp_root) / "sm8650-lenovo-tb321fu.dtb"
        dtb_path.write_bytes(payload)
        yield dtb_path


def run_fdtget(*arguments: str, allow_empty: bool = False) -> str:
    try:
        result = subprocess.run(
            ["fdtget", *arguments],
            check=False,
            capture_output=True,
            text=True,
            timeout=FDTGET_TIMEOUT_SECONDS,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise ContractError(f"fdtget failed to execute: {error}") from error
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip() or "no diagnostic"
        raise ContractError(
            f"fdtget {' '.join(arguments)!r} failed ({result.returncode}): {detail}"
        )
    output = result.stdout.strip()
    if not allow_empty and not output:
        raise ContractError(f"fdtget {' '.join(arguments)!r} returned empty output")
    if len(output) > 64 * 1024:
        raise ContractError("fdtget output exceeds 64 KiB")
    return output


def hex_cells(dtb_path: Path, node: str, property_name: str) -> list[int]:
    output = run_fdtget("-t", "x", str(dtb_path), node, property_name)
    try:
        cells = [int(token, 16) for token in output.split()]
    except ValueError as error:
        raise ContractError(
            f"{node}:{property_name} contains a non-hexadecimal cell"
        ) from error
    if any(cell < 0 or cell > 0xFFFFFFFF for cell in cells):
        raise ContractError(f"{node}:{property_name} contains a non-u32 cell")
    return cells


def validate_reserved_memory(dtb_path: Path) -> tuple[int, str]:
    digest = hashlib.sha256(dtb_path.read_bytes()).hexdigest()
    address_cells = hex_cells(dtb_path, "/reserved-memory", "#address-cells")
    size_cells = hex_cells(dtb_path, "/reserved-memory", "#size-cells")
    if address_cells != [2] or size_cells != [2]:
        raise ContractError(
            "reserved-memory must use exactly two address and two size cells"
        )

    child_output = run_fdtget("-l", str(dtb_path), "/reserved-memory")
    children = [line.strip() for line in child_output.splitlines() if line.strip()]
    if not children or len(children) > MAX_RESERVED_NODES:
        raise ContractError(
            f"reserved-memory child count {len(children)} is outside "
            f"1..{MAX_RESERVED_NODES}"
        )
    if len(children) != len(set(children)):
        raise ContractError("reserved-memory contains duplicate child names")

    intervals: list[tuple[int, int, str]] = []
    for child in children:
        node = f"/reserved-memory/{child}"
        cells = hex_cells(dtb_path, node, "reg")
        if len(cells) != 4:
            raise ContractError(f"{node}:reg must contain exactly four cells")
        start = (cells[0] << 32) | cells[1]
        size = (cells[2] << 32) | cells[3]
        end = start + size
        if size == 0 or end > 1 << 64:
            raise ContractError(f"{node}:reg has an invalid size or overflow")
        intervals.append((start, end, child))

    intervals.sort()
    for previous, current in zip(intervals, intervals[1:]):
        if current[0] < previous[1]:
            raise ContractError(
                f"reserved-memory overlap: {previous[2]} and {current[2]}"
            )

    ramoops = [entry for entry in intervals if entry[2] == RAMOOPS_NODE]
    if ramoops != [(RAMOOPS_START, RAMOOPS_START + RAMOOPS_SIZE, RAMOOPS_NODE)]:
        raise ContractError("ramoops start/size does not match the tested package")
    ramoops_path = f"/reserved-memory/{RAMOOPS_NODE}"
    if run_fdtget("-t", "s", str(dtb_path), ramoops_path, "compatible") != "ramoops":
        raise ContractError("ramoops compatible must be exactly 'ramoops'")
    console_size = hex_cells(dtb_path, ramoops_path, "console-size")
    pmsg_size = hex_cells(dtb_path, ramoops_path, "pmsg-size")
    if console_size != [RAMOOPS_CONSOLE_SIZE]:
        raise ContractError("ramoops console-size does not match the tested package")
    if pmsg_size != [RAMOOPS_PMSG_SIZE]:
        raise ContractError("ramoops pmsg-size does not match the tested package")
    if console_size[0] + pmsg_size[0] != RAMOOPS_SIZE:
        raise ContractError("ramoops payload sizes do not fill the reserved range")
    run_fdtget(str(dtb_path), ramoops_path, "no-map", allow_empty=True)

    return len(intervals), digest


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path, help="DTB or .tar.gz containing the DTB")
    parser.add_argument("--tar-member", help="exact regular-file member to validate")
    parser.add_argument("--expected-sha256", help="optional expected DTB SHA-256")
    return parser.parse_args()


def main() -> None:
    arguments = parse_arguments()
    expected_digest = arguments.expected_sha256
    if expected_digest is not None:
        expected_digest = expected_digest.lower()
        if len(expected_digest) != 64 or any(
            character not in "0123456789abcdef" for character in expected_digest
        ):
            raise SystemExit("--expected-sha256 must be 64 hexadecimal characters")
    try:
        with materialize_dtb(arguments.input, arguments.tar_member) as dtb_path:
            interval_count, digest = validate_reserved_memory(dtb_path)
    except ContractError as error:
        raise SystemExit(str(error)) from error
    if expected_digest is not None and digest != expected_digest:
        raise SystemExit(
            f"DTB SHA-256 mismatch: expected {expected_digest}, got {digest}"
        )
    print(
        "TB321FU_RESERVED_MEMORY_CONTRACT=PASS "
        f"intervals={interval_count} sha256={digest}"
    )


if __name__ == "__main__":
    main()
