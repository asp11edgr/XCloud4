#!/usr/bin/env python3
"""Run the full production HTTP code with a deterministic native ABI fixture.

No live networking or credentials are used. Compiler, native stub and sanitizers
are host-only; passing does not prove PS4 keep-alive or TLS behavior.
"""
from pathlib import Path
import hashlib
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="x4-public-http-") as directory:
    path = Path(directory)
    (path / "orbis").mkdir()
    (path / "orbis/libkernel.h").write_text(
        "#include <stdint.h>\nuint64_t sceKernelGetProcessTime(void);\nint sceKernelUsleep(unsigned int microseconds);\n", encoding="utf8")
    compiler = shlex.split(os.environ.get("CC", "cc"))
    flags = shlex.split(os.environ.get("CFLAGS", ""))
    binary = path / "public-http-test"
    subprocess.run(compiler + ["-std=gnu2x", "-Wall", "-Wextra", "-Werror", *flags,
        "-DX4_HTTP_TEST", "-pthread", "-I", str(path), str(root / "tests/http_public_transport_test.c"),
        str(root / "src/auth/public_image_policy.c"), str(root / "src/auth/http_wire.c"),
        str(root / "src/auth/ca_bundle.c"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("production HTTP source SHA256:", hashlib.sha256((root / "src/auth/http_client.c").read_bytes()).hexdigest())
