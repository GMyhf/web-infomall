#!/usr/bin/env python3
"""End-to-end checks for the archive verification and benchmark tools."""

import struct
import subprocess
import tempfile
from pathlib import Path


SRC = Path(__file__).resolve().parent
SAMPLE_DATA = SRC.parent / "sample_data"


def run(*args):
    return subprocess.run(
        args, cwd=SRC, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, check=False,
    )


def require_success(proc, label):
    if proc.returncode != 0:
        raise RuntimeError(f"{label} failed ({proc.returncode}):\n{proc.stdout}")


def main():
    with tempfile.TemporaryDirectory(prefix="web-infomall-archive-tools-") as tmp:
        archive = Path(tmp) / "archive"
        loaded = run(str(SRC / "load"), str(SAMPLE_DATA), str(archive), "--files", "0")
        require_success(loaded, "sample load")

        verified = run(str(SRC / "verify"), str(archive))
        require_success(verified, "verify on a fresh sample archive")
        if "Result: ALL CHECKS PASSED" not in verified.stdout:
            raise RuntimeError(f"verify did not report a clean archive:\n{verified.stdout}")

        benchmark = run(str(SRC / "bench"), str(archive / "data"), str(archive / "index"), "32")
        require_success(benchmark, "bench on a fresh sample archive")
        if "Queries/sec (QPS)" not in benchmark.stdout:
            raise RuntimeError(f"bench did not report throughput:\n{benchmark.stdout}")

        # MappedShard deliberately accepts legacy calendar-invalid dates so a
        # historical archive can still be served. verify is the separate
        # integrity boundary: it must make this inconsistency visible and fail.
        shard = next((archive / "index").glob("url_*.idx"))
        with shard.open("r+b") as index:
            header = index.read(16)
            _, entries, hosts, _ = struct.unpack("=IIII", header)
            if entries == 0 or hosts == 0:
                raise RuntimeError("sample archive unexpectedly has an empty shard")
            first_entry = 16 + hosts * 40
            index.seek(first_entry + 8)  # UrlIndexEntry.crawl_date
            index.write(struct.pack("=I", 20030229))

        invalid_date = run(str(SRC / "verify"), str(archive))
        if invalid_date.returncode == 0:
            raise RuntimeError(f"verify accepted an invalid index date:\n{invalid_date.stdout}")
        # Pin the severity, not just the text. verify runs a two-tier scheme —
        # ERROR for structural_errors that abort a data-file scan, WARNING for
        # bad_entry_refs that let the scan continue — and the index-side date
        # check is deliberately the second tier. Asserting only the sentence
        # lets the tier drift silently, which is exactly how the record-side
        # branch ended up labelled WARNING while counting as a structural error.
        if "WARNING: Entry" not in invalid_date.stdout:
            raise RuntimeError(f"index-side date issue lost its WARNING tier:\n{invalid_date.stdout}")
        if "has invalid crawl date 20030229" not in invalid_date.stdout:
            raise RuntimeError(f"verify did not report the invalid index date:\n{invalid_date.stdout}")

    check_invalid_record_date()
    check_invalid_record_payload_size()
    print("PASS: archive verify success/failure paths and benchmark smoke test")


def check_invalid_record_date():
    """verify has a *second* date check, on the record header rather than the index.

    Both exist because the relaxation in 99d97d3 removed calendar validation from
    MappedShard::open and named verify as the compensating boundary — but that
    boundary is two separate branches, and pinning only the index one leaves half
    the hedge unguarded. This branch is reachable and fires before the CRC pass
    (a corrupted date reports no CRC mismatch, because the structural check breaks
    out first), so it is testable today.

    Uses its own archive: the index-side case above leaves that one corrupted, and
    a failure has to be attributable to exactly one injected fault.
    """
    with tempfile.TemporaryDirectory(prefix="web-infomall-record-date-") as tmp:
        archive = Path(tmp) / "archive"
        require_success(run(str(SRC / "load"), str(SAMPLE_DATA), str(archive),
                            "--files", "0"), "sample load")

        record = sorted((archive / "data").glob("*/data_*.dat"))[0]
        with record.open("r+b") as data:
            data.seek(8)  # ArticleRecord.crawl_date
            data.write(struct.pack("=I", 20030229))

        result = run(str(SRC / "verify"), str(archive))
        if result.returncode == 0:
            raise RuntimeError(f"verify accepted an invalid record date:\n{result.stdout}")
        # ERROR, matching every other branch in scan_data_file: this one
        # increments structural_errors and breaks out of the file.
        if "ERROR: Record at offset 0 has invalid crawl date 20030229" not in result.stdout:
            raise RuntimeError(f"verify did not report the invalid record date:\n{result.stdout}")


def check_invalid_record_payload_size():
    """A malformed record length must not be conflated with an invalid date."""
    with tempfile.TemporaryDirectory(prefix="web-infomall-record-payload-") as tmp:
        archive = Path(tmp) / "archive"
        require_success(run(str(SRC / "load"), str(SAMPLE_DATA), str(archive),
                            "--files", "0"), "sample load")

        record = sorted((archive / "data").glob("*/data_*.dat"))[0]
        with record.open("r+b") as data:
            data.seek(28)  # ArticleRecord.record_size
            original_size = struct.unpack("=I", data.read(4))[0]
            data.seek(28)
            data.write(struct.pack("=I", original_size + 1))

        result = run(str(SRC / "verify"), str(archive))
        if result.returncode == 0:
            raise RuntimeError(f"verify accepted an invalid record payload size:\n{result.stdout}")
        if "Record payload size" not in result.stdout or "does not match record size" not in result.stdout:
            raise RuntimeError(f"verify did not report the payload mismatch:\n{result.stdout}")


if __name__ == "__main__":
    main()
