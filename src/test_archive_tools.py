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
        if "has invalid crawl date 20030229" not in invalid_date.stdout:
            raise RuntimeError(f"verify did not report the invalid index date:\n{invalid_date.stdout}")

    print("PASS: archive verify success/failure paths and benchmark smoke test")


if __name__ == "__main__":
    main()
