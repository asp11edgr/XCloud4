#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Apply and verify the PS4 media UDP buffer overlay on the pinned dependencies."""
import pathlib
import subprocess
import sys

PINS = {
    "libdatachannel": "bdc5ff28e9d3b863144c94a677ecf5bf043aaf15",
    "libdatachannel/deps/libjuice": "b89c792e3612faf2f12cf35bcc56857313a06be3",
}


def replace_once(text, before, after, name):
    count = text.count(after)
    if count == 1:
        return text
    if count != 0 or text.count(before) != 1:
        raise SystemExit(f"Unexpected pinned UDP buffer anchor in {name}: {before!r}")
    return text.replace(before, after, 1)


def apply(base):
    base = pathlib.Path(base).resolve()
    for path, pin in PINS.items():
        checkout = base / path
        if (checkout / ".git").exists():
            head = subprocess.check_output(
                ["git", "-C", str(checkout), "rev-parse", "HEAD"], text=True
            ).strip()
            if head != pin:
                raise SystemExit(f"Unexpected UDP buffer patch pin for {path}")

    vendor = base / "libdatachannel"
    juice = "deps/libjuice/src/"
    edits = {}

    def edit(path, before, after):
        file = vendor / path
        text = edits.get(file)
        if text is None:
            text = file.read_text(encoding="utf-8")
        edits[file] = replace_once(text, before, after, path)

    socket_proto = "socket_t udp_create_socket(const udp_socket_config_t *config);"
    edit(juice + "udp.h", socket_proto,
         socket_proto + "\n#ifdef X4_OPENORBIS\n#include <x4_udp_buffer46c.h>\n"
         "socket_t x4_udp_create_media_socket(const udp_socket_config_t *config,\n"
         "                                    x4_udp_buffer_info *buffers);\n#endif")
    original = ("static socket_t create_socket_for_addrinfo(const udp_socket_config_t *config,\n"
                "                                           const struct addrinfo *ai) {")
    edit(juice + "udp.c", original, original[:-3] + ", bool tune_buffers) {")
    original = (
        "\t// Set buffer size up to 1 MiB for performance\n"
        "\tconst sockopt_t buffer_size = 1 * 1024 * 1024;\n"
        "\tsetsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const char *)&buffer_size, sizeof(buffer_size));\n"
        "\tsetsockopt(sock, SOL_SOCKET, SO_SNDBUF, (const char *)&buffer_size, sizeof(buffer_size));"
    )
    edit(juice + "udp.c", original,
         "\tif (!tune_buffers) {\n" + "\n".join("\t" + line for line in original.splitlines()) + "\n\t}")
    edit(juice + "udp.c", "socket_t udp_create_socket(const udp_socket_config_t *config) {",
         "static socket_t create_udp_socket(const udp_socket_config_t *config, bool tune_buffers) {")
    edit(juice + "udp.c", "socket_t sock = create_socket_for_addrinfo(config, ai);",
         "socket_t sock = create_socket_for_addrinfo(config, ai, tune_buffers);")
    anchor = "int udp_recvfrom(socket_t sock, char *buffer, size_t size, addr_record_t *src) {"
    edit(juice + "udp.c", anchor,
         "socket_t udp_create_socket(const udp_socket_config_t *config) {\n"
         "\treturn create_udp_socket(config, false);\n}\n\n"
         "#ifdef X4_OPENORBIS\n"
         "socket_t x4_udp_create_media_socket(const udp_socket_config_t *config,\n"
         "                                    x4_udp_buffer_info *buffers) {\n"
         "\tsocket_t sock = create_udp_socket(config, true);\n"
         "\tif (sock != INVALID_SOCKET)\n"
         "\t\t*buffers = x4_udp_configure_buffers(sock);\n"
         "\treturn sock;\n}\n#endif\n\n" + anchor)

    anchor = '#include "addr.h"'
    edit(juice + "agent.h", anchor,
         "#ifdef X4_OPENORBIS\n#include <x4_udp_buffer46c.h>\n#endif\n" + anchor)
    anchor = "\tvoid *conn_impl;\n"
    edit(juice + "agent.h", anchor,
         anchor + "#ifdef X4_OPENORBIS\n\tx4_udp_buffer_sample x4_udp_buffers;\n#endif\n")
    anchor = "\tagent->config.concurrency_mode = config->concurrency_mode;\n"
    edit(juice + "agent.c", anchor,
         anchor + "#ifdef X4_OPENORBIS\n\tx4_udp_buffer_sample_init(&agent->x4_udp_buffers);\n#endif\n")
    anchor = "int agent_gather_candidates(juice_agent_t *agent) {"
    edit(juice + "agent.c", anchor,
         "#ifdef X4_OPENORBIS\n"
         "int x4_juice_get_udp_buffers(const juice_agent_t *agent, x4_udp_buffer_info *buffers) {\n"
         "\treturn x4_udp_buffer_sample_read(&agent->x4_udp_buffers, buffers);\n"
         "}\n#endif\n\n" + anchor)

    anchor = "\tconn_impl->udp_sock = udp_create_socket(config);"
    edit(juice + "conn_poll.c", anchor,
         "#ifdef X4_OPENORBIS\n"
         "\tx4_udp_buffer_info buffers;\n"
         "\tconn_impl->udp_sock = x4_udp_create_media_socket(config, &buffers);\n"
         "#else\n" + anchor + "\n#endif")
    anchor = "\tagent->conn_impl = conn_impl;\n\treturn 0;"
    edit(juice + "conn_poll.c", anchor,
         "\tagent->conn_impl = conn_impl;\n"
         "#ifdef X4_OPENORBIS\n"
         "\tx4_udp_buffer_sample_publish(&agent->x4_udp_buffers, &buffers);\n"
         "#endif\n\treturn 0;")

    for path, name in (("src/impl/icetransport.hpp", "rtc::impl"),
                       ("include/rtc/peerconnection.hpp", "rtc")):
        anchor = f"namespace {name} {{"
        edit(path, anchor,
             "#ifdef X4_OPENORBIS\nstruct x4_udp_buffer_info;\n#endif\n\n" + anchor)
        anchor = "\tbool getSelectedCandidatePair(Candidate *local, Candidate *remote);\n"
        edit(path, anchor,
             anchor + "#ifdef X4_OPENORBIS\n"
             "\tbool x4GetUdpBuffers(::x4_udp_buffer_info &buffers) const;\n#endif\n")

    anchor = "#if !USE_NICE // libjuice"
    edit("src/impl/icetransport.cpp", anchor,
         anchor + "\n\n#ifdef X4_OPENORBIS\n"
         "extern \"C\" int x4_juice_get_udp_buffers(const juice_agent_t *, ::x4_udp_buffer_info *);\n"
         "bool IceTransport::x4GetUdpBuffers(::x4_udp_buffer_info &buffers) const {\n"
         "\treturn x4_juice_get_udp_buffers(mAgent.get(), &buffers) == 0;\n"
         "}\n#endif")
    anchor = "#else // USE_NICE == 1"
    edit("src/impl/icetransport.cpp", anchor,
         anchor + "\n\n#ifdef X4_OPENORBIS\n"
         "bool IceTransport::x4GetUdpBuffers(::x4_udp_buffer_info &) const { return false; }\n"
         "#endif")
    anchor = "optional<std::chrono::milliseconds> PeerConnection::rtt() {"
    edit("src/peerconnection.cpp", anchor,
         "#ifdef X4_OPENORBIS\n"
         "bool PeerConnection::x4GetUdpBuffers(::x4_udp_buffer_info &buffers) const {\n"
         "\tauto ice = impl()->getIceTransport();\n"
         "\treturn ice && ice->x4GetUdpBuffers(buffers);\n"
         "}\n#endif\n\n" + anchor)

    anchor = '#include "rtc.hpp"'
    edit("src/capi.cpp", anchor,
         "#ifdef X4_OPENORBIS\n#include <x4_udp_buffer46c.h>\n#endif\n" + anchor)
    anchor = "int rtcGetLocalAddress(int pc, char *buffer, int size) {"
    edit("src/capi.cpp", anchor,
         "#ifdef X4_OPENORBIS\n"
         "extern \"C\" int x4_rtc_native_udp_buffers(int pc, int *rcvbuf, int *sndbuf,\n"
         "                                         int *requested, int *attempts) {\n"
         "\tif (!rcvbuf || !sndbuf || !requested || !attempts)\n"
         "\t\treturn RTC_ERR_INVALID;\n"
         "\treturn wrap([&] {\n"
         "\t\tx4_udp_buffer_info buffers;\n"
         "\t\tif (!getPeerConnection(pc)->x4GetUdpBuffers(buffers))\n"
         "\t\t\treturn RTC_ERR_NOT_AVAIL;\n"
         "\t\t*rcvbuf = buffers.rcvbuf;\n"
         "\t\t*sndbuf = buffers.sndbuf;\n"
         "\t\t*requested = buffers.requested;\n"
         "\t\t*attempts = buffers.attempts;\n"
         "\t\treturn RTC_ERR_SUCCESS;\n"
         "\t});\n"
         "}\n#endif\n\n" + anchor)

    overlay = base / "overlay" / "x4_udp_buffer46c.h"
    header = pathlib.Path(__file__).with_name("udp_buffer46c.h").read_bytes()
    for path, text in edits.items():
        if path.read_text(encoding="utf-8") != text:
            path.write_bytes(text.encode("utf-8"))
    overlay.parent.mkdir(parents=True, exist_ok=True)
    if not overlay.exists() or overlay.read_bytes() != header:
        overlay.write_bytes(header)
    print(f"UDP media buffer overlay verified ({len(edits)} pinned source files).")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("Usage: prepare_udp_buffer46c.py <RTC_ROOT>")
    apply(sys.argv[1])
