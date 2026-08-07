#!/usr/bin/env python3
"""End-to-end protocol checks for the production C++ HTTP server."""

import gzip
import socket
import subprocess
import tempfile
import time
from pathlib import Path


SRC = Path(__file__).resolve().parent
SAMPLE_DATA = SRC.parent / "sample_data"


def reserve_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def exchange(port, request):
    with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
        sock.sendall(request)
        sock.shutdown(socket.SHUT_WR)
        chunks = []
        while True:
            chunk = sock.recv(65536)
            if not chunk:
                return b"".join(chunks)
            chunks.append(chunk)


def split_response(response):
    head, body = response.split(b"\r\n\r\n", 1)
    headers = {}
    for line in head.split(b"\r\n")[1:]:
        key, value = line.split(b":", 1)
        headers[key.lower()] = value.strip()
    return head, headers, body


def wait_until_ready(proc, port):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"server exited early with status {proc.returncode}")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("server did not become ready")


RATE_LIMIT_MAX = 30  # requests per 5s window, per resolved client key (server.cpp)


def burst(port, xff_values):
    """Fire one request per entry, returning the status codes in order.

    Each request carries its own X-Forwarded-For, so whether they share a
    rate-limit bucket is exactly what the server's hop resolution decides.
    """
    codes = []
    for value in xff_values:
        request = b"GET /ping HTTP/1.1\r\nHost: localhost\r\n"
        if value is not None:
            request += b"X-Forwarded-For: " + value.encode() + b"\r\n"
        request += b"Connection: close\r\n\r\n"
        codes.append(int(exchange(port, request).split()[1]))
    return codes


def start_server(archive, port, extra_args=()):
    proc = subprocess.Popen(
        [str(SRC / "serve"), str(archive / "data"), str(archive / "index"), str(port),
         *extra_args],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    wait_until_ready(proc, port)
    return proc


def stop_server(proc):
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5)


def check_trusted_proxy_hops(archive):
    """T-005: the rate-limit key must follow --trusted-proxy-hops.

    Every assertion here is about *which* address gets billed, so each one is
    written as "these N requests do / do not share a bucket" — the only
    externally observable consequence of picking the wrong entry.
    """
    over = RATE_LIMIT_MAX + 1

    # hops=0: X-Forwarded-For is ignored entirely, so requests claiming distinct
    # client addresses still share the peer's bucket and the burst gets limited.
    port = reserve_port()
    proc = start_server(archive, port, ["--trusted-proxy-hops", "0"])
    try:
        codes = burst(port, [f"10.0.0.{i}" for i in range(over)])
        assert 429 in codes, f"hops=0 must ignore XFF and limit the burst, got {set(codes)}"
    finally:
        stop_server(proc)

    # hops=1 (the default): the last XFF entry is believed, so distinct claimed
    # clients get distinct buckets and nothing is limited.
    port = reserve_port()
    proc = start_server(archive, port)
    try:
        codes = burst(port, [f"10.0.1.{i}" for i in range(over)])
        assert 429 not in codes, f"hops=1 must key on XFF, got {set(codes)}"

        # The security-critical half: with one trusted proxy the client is the
        # entry our proxy appended, i.e. the RIGHTMOST one. A client that
        # prepends a forged address must not escape its bucket. If the server
        # read the leftmost entry instead — the naive reading of "the client
        # comes first in X-Forwarded-For" — every one of these would land in its
        # own bucket and nothing would be limited.
        codes = burst(port, [f"203.0.113.{i}, 10.0.2.7" for i in range(over)])
        assert 429 in codes, (
            "forged XFF prefixes must be skipped: the rightmost entry decides the "
            f"bucket, got {set(codes)}"
        )

        # A chain shorter than the configured hop count, an unparseable entry and
        # an absent header all fall back to the peer address rather than failing
        # open. All three share 127.0.0.1's bucket, so the burst gets limited.
        codes = burst(port, ["not-an-ip", "::1", None] * (over // 3 + 1))
        assert 429 in codes, f"unusable XFF must fall back to the peer, got {set(codes)}"
    finally:
        stop_server(proc)

    # Invalid hop counts are rejected at startup instead of being silently
    # clamped: a typo here disables rate limiting, so it must not start.
    for bad in ["-1", "9", "abc", ""]:
        rejected = subprocess.run(
            [str(SRC / "serve"), str(archive / "data"), str(archive / "index"),
             str(reserve_port()), "--trusted-proxy-hops", bad],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30,
        )
        assert rejected.returncode != 0, f"--trusted-proxy-hops {bad!r} must be rejected"


def main():
    with tempfile.TemporaryDirectory(prefix="web-infomall-http-test-") as tmp:
        archive = Path(tmp) / "archive"
        loaded = subprocess.run(
            [str(SRC / "load"), str(SAMPLE_DATA), str(archive), "--files", "0"],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
        if loaded.returncode != 0:
            raise RuntimeError(f"sample load failed:\n{loaded.stdout}")

        port = reserve_port()
        proc = subprocess.Popen(
            [str(SRC / "serve"), str(archive / "data"),
             str(archive / "index"), str(port)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        try:
            wait_until_ready(proc, port)

            plain = exchange(
                port,
                b"GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
            )
            plain_head, plain_headers, plain_body = split_response(plain)
            assert plain_head.startswith(b"HTTP/1.1 200")
            assert b"Web InfoMall" in plain_body
            assert b"content-encoding" not in plain_headers
            assert int(plain_headers[b"content-length"]) == len(plain_body)

            compressed = exchange(
                port,
                b"GET / HTTP/1.1\r\nHost: localhost\r\nAccept-Encoding: gzip\r\n"
                b"Connection: close\r\n\r\n",
            )
            compressed_head, compressed_headers, compressed_body = split_response(compressed)
            assert compressed_head.startswith(b"HTTP/1.1 200")
            assert compressed_headers[b"content-encoding"] == b"gzip"
            assert gzip.decompress(compressed_body) == plain_body
            assert compressed_headers[b"etag"] != plain_headers[b"etag"]

            etag = compressed_headers[b"etag"]
            not_modified = exchange(
                port,
                b"GET / HTTP/1.1\r\nHost: localhost\r\nAccept-Encoding: gzip\r\n"
                b"If-None-Match: " + etag +
                b"\r\nConnection: close\r\n\r\n",
            )
            nm_head, nm_headers, nm_body = split_response(not_modified)
            assert nm_head.startswith(b"HTTP/1.1 304")
            assert nm_body == b""
            assert nm_headers[b"vary"] == b"Accept-Encoding"

            no_gzip = exchange(
                port,
                b"GET / HTTP/1.1\r\nHost: localhost\r\n"
                b"Accept-Encoding: gzip;q=0, *;q=0\r\nConnection: close\r\n\r\n",
            )
            _, no_gzip_headers, _ = split_response(no_gzip)
            assert b"content-encoding" not in no_gzip_headers

            duplicate_length = exchange(
                port,
                b"GET / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n"
                b"Content-Length: 1\r\nConnection: close\r\n\r\n",
            )
            assert duplicate_length.startswith(b"HTTP/1.1 400")

            transfer_encoding = exchange(
                port,
                b"GET / HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n"
                b"Connection: close\r\n\r\n",
            )
            assert transfer_encoding.startswith(b"HTTP/1.1 400")

            missing_host = exchange(
                port,
                b"GET / HTTP/1.1\r\nConnection: close\r\n\r\n",
            )
            assert missing_host.startswith(b"HTTP/1.1 400")

            pipeline = exchange(
                port,
                b"GET /stats HTTP/1.1\r\nHost: localhost\r\n\r\n"
                b"GET /stats HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
            )
            assert pipeline.count(b"HTTP/1.1 200") == 2
        finally:
            stop_server(proc)

        # Runs on its own server instances: rate-limit state is per-process, and
        # the checks above have already spent part of this window's budget.
        check_trusted_proxy_hops(archive)

    print("PASS: C++ HTTP framing, gzip/ETag, validation, pipelining, and proxy hops")


if __name__ == "__main__":
    main()
