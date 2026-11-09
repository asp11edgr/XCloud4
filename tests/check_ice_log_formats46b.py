#!/usr/bin/env python3
"""Check the emitted ICE log formats in a host fixture or native product."""

import argparse
import re
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--diag-only", action="store_true")
    parser.add_argument("--public", type=int, choices=(0, 1), required=True)
    parser.add_argument("binary", type=Path)
    args = parser.parse_args()
    data = args.binary.read_bytes()
    assert data.startswith(b"\x7fELF"), "Expected an ELF binary"
    formats = set(re.findall(rb"XCloud4: [\x20-\x7e]+", data))

    selected = b"XCloud4: ICE par seleccionado: local=%s/%s remote=%s/%s"
    remote = b"XCloud4: ICE candidato remoto: remote=%s/%s"
    if args.public:
        assert b"%u.%u.x.x" not in data and b"%x:%x::/32" not in data
    else:
        selected += b" local_prefix=%s remote_prefix=%s"
        remote += b" remote_prefix=%s"
        assert b"%u.%u.x.x" in data and b"%x:%x::/32" in data
    assert {f for f in formats if f.startswith(b"XCloud4: ICE par seleccionado: ")} == {selected}
    assert {f for f in formats if f.startswith(b"XCloud4: ICE candidato remoto: ")} == {remote}
    assert not any(f.startswith(b"XCloud4: Servidor de juego (envia video):") for f in formats)
    assert b'"game_server_port"' not in data and b'"game_server_ip"' not in data

    if not args.diag_only:
        sdp = {f for f in formats if f.startswith(b"XCloud4: SDP media index=")}
        # The full SDP shape log is Lab-only in the product.
        if not args.public:
            assert sdp, "Expected the Lab SDP summary format"
        for line in sdp:
            assert b"port_present=%u" in line and b"sctp_present=%u" in line
            assert b" port=" not in line and b" sctp=" not in line and b"midnum=" not in line
        assert b"[campo %s] %s = %.*s" not in data, "Found a raw region-response log format"

    print(f"PASS ICE log format scan edition={'public' if args.public else 'lab'}"
          f" formats={len(formats)}")


if __name__ == "__main__":
    main()
