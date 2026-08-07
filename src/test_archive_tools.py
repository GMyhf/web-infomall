#!/usr/bin/env python3
"""End-to-end checks for the archive verification and benchmark tools."""

import shutil
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


def fault_copy(base, tmp, name):
    """Copy the pristine archive so each fault is injected into its own tree.

    Copying rather than re-running load is worth an explicit note: `load` takes
    ~45s on this 1000-article sample, and almost all of it is fsync. DataStore
    keeps one data file open at a time and fsyncs the file *and* its directory
    whenever the month changes, while articles arrive in arbitrary date order
    across 111 month directories — about 2178 fsyncs for 1000 records. Loading
    once and copying keeps every assertion below identical while taking ~90s off
    the gate. See T-010 for the loader side of this.
    """
    target = Path(tmp) / name
    shutil.copytree(base, target)
    return target


def main():
    with tempfile.TemporaryDirectory(prefix="web-infomall-archive-tools-") as tmp:
        pristine = Path(tmp) / "pristine"
        loaded = run(str(SRC / "load"), str(SAMPLE_DATA), str(pristine), "--files", "0")
        require_success(loaded, "sample load")
        archive = fault_copy(pristine, tmp, "index-date")

        verified = run(str(SRC / "verify"), str(archive))
        require_success(verified, "verify on a fresh sample archive")
        if "Result: ALL CHECKS PASSED" not in verified.stdout:
            raise RuntimeError(f"verify did not report a clean archive:\n{verified.stdout}")

        # 300 rather than 32: bench is now a correctness check as well as a
        # latency one (T-012), and its power comes from sampling enough URLs that
        # a broken lookup path cannot hide. Cost is negligible — bench does 500
        # queries in ~5ms.
        benchmark = run(str(SRC / "bench"), str(archive / "data"), str(archive / "index"), "300")
        require_success(benchmark, "bench on a fresh sample archive")
        if "Queries/sec (QPS)" not in benchmark.stdout:
            raise RuntimeError(f"bench did not report throughput:\n{benchmark.stdout}")
        # Assert the miss line explicitly rather than leaning on the exit code
        # alone: if the counter were ever dropped, bench would go back to exiting
        # 0 while measuring nothing, and require_success could not tell.
        if "Lookup misses" not in benchmark.stdout:
            raise RuntimeError(f"bench stopped accounting for misses:\n{benchmark.stdout}")
        miss_line = next(l for l in benchmark.stdout.splitlines() if "Lookup misses" in l)
        if miss_line.split("|")[2].strip() != "0":
            raise RuntimeError(f"bench reported lookup misses:\n{benchmark.stdout}")

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

        check_bench_counts_misses(fault_copy(pristine, tmp, "bench-no-data"))
        check_invalid_record_date(fault_copy(pristine, tmp, "record-date"))
        check_invalid_record_payload_size(fault_copy(pristine, tmp, "record-payload"))
    print("PASS: archive verify success/failure paths and benchmark smoke test")


def check_bench_counts_misses(archive):
    """Positive control for bench's miss counter (T-012).

    Asserting "Lookup misses | 0" on a healthy archive proves nothing: gut the
    counter and misses stays 0, the line still prints, and the assertion still
    passes. It is the same vacuous-assertion trap this repo keeps finding, and it
    was in the first version of this check.

    So force misses to exist. Deleting the data files leaves the index intact, so
    every lookup still resolves through the shard and then fails to read its
    record — get_page returns {} and the URL no longer matches. bench must count
    those and exit non-zero. With the counter removed, this check goes red.
    """
    for record in (archive / "data").rglob("*.dat"):
        record.unlink()
    result = run(str(SRC / "bench"), str(archive / "data"), str(archive / "index"), "100")
    if result.returncode == 0:
        raise RuntimeError(f"bench passed an archive whose records are all gone:\n{result.stdout}")
    miss_line = next((l for l in result.stdout.splitlines() if "Lookup misses" in l), "")
    if not miss_line or miss_line.split("|")[2].strip() == "0":
        raise RuntimeError(f"bench did not count the misses it hit:\n{result.stdout}")


def check_invalid_record_date(archive):
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


def check_invalid_record_payload_size(archive):
    """A malformed record length must not be conflated with an invalid date."""
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
