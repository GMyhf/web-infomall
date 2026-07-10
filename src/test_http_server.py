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
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)

    print("PASS: C++ HTTP framing, gzip/ETag, validation, and pipelining")


if __name__ == "__main__":
    main()
