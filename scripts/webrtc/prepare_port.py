#!/usr/bin/env python3
"""Apply explicit, reproducible OpenOrbis ABI overlays to pinned dependencies.

Run on the build host. This never alters the installed SDK. The overlay must
also be used when compiling the XCloud4 transport translation units.
"""
import os
import pathlib
import sys

def write_changed(path, text):
    """Do not invalidate every dependency object when a patch is unchanged."""
    if not path.exists() or path.read_text() != text:
        path.write_text(text)

base = pathlib.Path(sys.argv[1]).resolve()
sdk = pathlib.Path(os.environ['OO_PS4_TOOLCHAIN']).resolve()
overlay = base / 'overlay'
(overlay / 'sys').mkdir(parents=True, exist_ok=True)
(overlay / 'bits').mkdir(exist_ok=True)

socket = (sdk / 'include/sys/socket.h').read_text()
old = 'struct sockaddr_storage {\n\tsa_family_t ss_family;\n\tchar __ss_padding[128-sizeof(long)-sizeof(sa_family_t)];\n\tunsigned long __ss_align;\n};'
new = 'struct sockaddr_storage {\n\tunsigned char ss_len;\n\tsa_family_t ss_family;\n\tchar __ss_padding[128-sizeof(long)-2];\n\tunsigned long __ss_align;\n};'
if old not in socket:
    raise SystemExit('Unexpected SDK sockaddr_storage declaration')
write_changed(overlay / 'sys/socket.h', socket.replace(old, new))

types = (sdk / 'include/bits/alltypes.h').read_text()
# Native FreeBSD attributes are opaque pointers, not musl's four-byte values.
# Eight-byte storage prevents native attr_init writing outside the object.
for typename in ('pthread_mutexattr_t', 'pthread_condattr_t', 'pthread_barrierattr_t'):
    old = 'typedef struct { unsigned __attr; } ' + typename + ';'
    new = 'typedef struct { unsigned long __attr; } ' + typename + ';'
    if old not in types:
        raise SystemExit('Unexpected SDK attribute declaration: ' + typename)
    types = types.replace(old, new)
write_changed(overlay / 'bits/alltypes.h', types)

# FreeBSD's optional thread naming is not exposed by the SDK header set.
# Remove only the optional naming hook, preserving pthread creation/join.
thread = base / 'libdatachannel/deps/libjuice/src/thread.h'
text = thread.read_text()
text = text.replace('#if defined(__FreeBSD__)\n#include <pthread_np.h>',
                    '#if defined(__FreeBSD__) && !defined(X4_OPENORBIS)\n#include <pthread_np.h>')
text = text.replace('#elif defined(__FreeBSD__)\n\tpthread_set_name_np',
                    '#elif defined(__FreeBSD__) && !defined(X4_OPENORBIS)\n\tpthread_set_name_np')
write_changed(thread, text)

random = base / 'libdatachannel/deps/libjuice/src/random.c'
text = random.read_text()
if 'X4_OPENORBIS' not in text:
    text = text.replace('void juice_random(void *buf, size_t size) {',
        'void juice_random(void *buf, size_t size) {\n'
        '#ifdef X4_OPENORBIS\n'
        '\textern void x4_native_random_or_exit(void *, size_t);\n'
        '\tx4_native_random_or_exit(buf, size);\n'
        '\treturn;\n'
        '#endif\n')
write_changed(random, text)

# PS4 kernel socket descriptors reject both F_SETFL and BSD FIONBIO with
# EACCES on the real console. Sony's PlayStation WebKit port and OpenOrbis
# homebrew use setsockopt(SOL_SOCKET, SO_NBIO) on the same POSIX descriptor.
# The application implements and verifies that native operation.
source = base / 'libdatachannel/deps/libjuice/src/socket.h'
text = source.read_text()
legacy_fio_header = ('#include <sys/ioctl.h>\n#ifdef X4_OPENORBIS\n'
    '#ifndef FIONBIO\n#define FIONBIO _IOW(\'f\', 126, int)\n#endif\n#endif')
socket_native_header = ('#include <sys/ioctl.h>\n#ifdef X4_OPENORBIS\n'
    'extern int x4_native_socket_nonblock(int);\n#endif')
if socket_native_header not in text:
    if legacy_fio_header in text:
        text = text.replace(legacy_fio_header,socket_native_header,1)
    else:
        if text.count('#include <sys/ioctl.h>') != 1:
            raise SystemExit('Unexpected pinned libjuice socket ioctl include')
        text = text.replace('#include <sys/ioctl.h>', socket_native_header, 1)
if text.count(socket_native_header) != 1:
    raise SystemExit('Incomplete native socket API declaration')
write_changed(source, text)

# Normalize the original upstream form for the checked migration below.
# Existing 0.7.3 diagnostic sources and the current native form are also
# accepted explicitly; no partially applied replacement is silently used.
for filename in ('tcp.c', 'udp.c'):
    source = base / 'libdatachannel/deps/libjuice/src' / filename
    text = source.read_text()
    text = text.replace('if (ioctlsocket(sock, FIONBIO, &nbio)) {',
        'if (fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK) < 0) {')
    write_changed(source, text)

# AF_CONN SCTP is carried exclusively inside DTLS; native raw IPv4/IPv6 SCTP
# support is disabled in CMake. Use upstream's portable userspace metadata
# structures rather than depending on unavailable FreeBSD kernel headers.
source = base / 'libdatachannel/deps/usrsctp/usrsctplib/user_environment.h'
text = source.read_text().replace('#ifdef __FreeBSD__\n#ifndef _SYS_MUTEX_H_',
                                '#if defined(__FreeBSD__) && !defined(X4_OPENORBIS)\n#ifndef _SYS_MUTEX_H_')
write_changed(source, text)
source = base / 'libdatachannel/deps/usrsctp/usrsctplib/netinet/sctp_os_userspace.h'
text = source.read_text().replace('#if !defined(__FreeBSD__)\nstruct mtx',
                                '#if !defined(__FreeBSD__) || defined(X4_OPENORBIS)\nstruct mtx')
write_changed(source, text)
source = base / 'libdatachannel/deps/usrsctp/usrsctplib/user_environment.c'
text = source.read_text()
if '#if defined(X4_OPENORBIS)' not in text:
    text = text.replace('#if defined(FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION)',
        '#if defined(X4_OPENORBIS)\n'
        'extern void x4_native_random_or_exit(void *, size_t);\n'
        'void init_random(void) {}\n'
        'void finish_random(void) {}\n'
        'void read_random(void *buf, size_t size) { x4_native_random_or_exit(buf, size); }\n'
        '#elif defined(FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION)', 1)
write_changed(source, text)

# Select generic POSIX userspace SCTP compatibility, retaining the real
# FreeBSD target ABI and detected sockaddr length fields. AF_CONN is the
# only SCTP family used; no raw IP/route socket support is compiled.
cmake = base / 'libdatachannel/deps/usrsctp/usrsctplib/CMakeLists.txt'
text = cmake.read_text()
marker = '\nif(CMAKE_C_FLAGS MATCHES "X4_OPENORBIS")\n  target_compile_options(usrsctp PRIVATE -U__FreeBSD__)\nendif()\n'
if marker not in text:
    text += marker
write_changed(cmake, text)
for relative in ('netinet/sctp_os_userspace.h', 'netinet/sctp_userspace.c'):
    source = base / 'libdatachannel/deps/usrsctp/usrsctplib' / relative
    lines = source.read_text().splitlines()
    for index, line in enumerate(lines):
        if line == 'timingsafe_bcmp(const void *, const void *, size_t);' or line == 'timingsafe_bcmp(const void *b1, const void *b2, size_t n)':
            if 'X4_OPENORBIS' not in lines[index - 2]:
                lines[index - 2] += ' || defined(X4_OPENORBIS)'
    write_changed(source, '\n'.join(lines) + '\n')
source = base / 'libdatachannel/deps/usrsctp/usrsctplib/user_socketvar.h'
lines = source.read_text().splitlines()
for index, line in enumerate(lines):
    if line == '#define ERESTART (-1)' and 'X4_OPENORBIS' not in lines[index - 2]:
        lines[index - 2] += ' || defined(X4_OPENORBIS)'
write_changed(source, '\n'.join(lines) + '\n')

source = base / 'libdatachannel/src/impl/utils.cpp'
text = source.read_text()
text = text.replace('#if defined(__FreeBSD__)\n#include <pthread_np.h>',
                    '#if defined(__FreeBSD__) && !defined(X4_OPENORBIS)\n#include <pthread_np.h>')
text = text.replace('#elif defined(__FreeBSD__)\n\tpthread_set_name_np',
                    '#elif defined(__FreeBSD__) && !defined(X4_OPENORBIS)\n\tpthread_set_name_np')
if 'x4_native_random_or_exit' not in text:
    text = text.replace('#include "utils.hpp"',
        '#include "utils.hpp"\n#ifdef X4_OPENORBIS\nextern "C" void x4_native_random_or_exit(void *, size_t);\n#endif')
    text = text.replace('std::seed_seq random_seed() {',
        'std::seed_seq random_seed() {\n#ifdef X4_OPENORBIS\n'
        '\tstd::vector<unsigned int> seed(4);\n'
        '\t::x4_native_random_or_exit(seed.data(), seed.size() * sizeof(unsigned int));\n'
        '\treturn {seed.begin(), seed.end()};\n#else')
    text = text.replace('return {seed.begin(), seed.end()};\n}',
                        'return {seed.begin(), seed.end()};\n#endif\n}')
text = text.replace('return {seed.begin(), seed.end()};',
                    'return std::seed_seq(seed.begin(), seed.end());')
write_changed(source, text)

# SDK executables do not supply the C++ thread-local destructor runtime.
# This URBG draws directly from native secure entropy instead of creating
# a thread_local PRNG; standard distributions retain their range handling.
source = base / 'libdatachannel/src/impl/utils.hpp'
text = source.read_text()
if 'native_random_engine' not in text:
    text = text.replace('namespace rtc::impl::utils {',
        '#ifdef X4_OPENORBIS\n'
        'extern "C" void x4_native_random_or_exit(void *, size_t);\n'
        '#endif\n\nnamespace rtc::impl::utils {')
    marker = '// Return a wrapped thread-local seeded random number generator'
    adapter = ('#ifdef X4_OPENORBIS\n'
               'template <typename Result> struct native_random_engine {\n'
               '\tusing result_type = Result;\n'
               '\tstatic constexpr Result min() { return std::numeric_limits<Result>::min(); }\n'
               '\tstatic constexpr Result max() { return std::numeric_limits<Result>::max(); }\n'
               '\tResult operator()() { Result value; ::x4_native_random_or_exit(&value, sizeof(value)); return value; }\n'
               '\tvoid discard(unsigned long long count) { while (count--) (void)(*this)(); }\n'
               '};\n#endif\n\n')
    text = text.replace(marker, adapter + marker)
    text = text.replace('auto random_engine() {\n',
        'auto random_engine() {\n#ifdef X4_OPENORBIS\n'
        '\treturn native_random_engine<Result>{};\n#else\n')
    text = text.replace('return random_engine_wrapper<Generator, Result>{engine};\n}',
        'return random_engine_wrapper<Generator, Result>{engine};\n#endif\n}')
write_changed(source, text)

# This application generates ephemeral DTLS certificates. Keep disabled
# MbedTLS filesystem APIs out of the optional certificate-file import path;
# callers of that unsupported path receive a real error rather than a stub.
source = base / 'libdatachannel/src/impl/certificate.cpp'
text = source.read_text()
marker = '\tPLOG_DEBUG << "Importing certificate from PEM file (MbedTLS): " << crt_pem_file;'
if marker in text and 'Certificate file import requires MBEDTLS_FS_IO' not in text:
    start = text.index(marker)
    end_marker = '\treturn Certificate(std::move(crt), std::move(pk));\n}'
    end = text.index(end_marker, start)
    body = text[start:end + len(end_marker) - 2]
    replacement = ('#if !defined(MBEDTLS_FS_IO)\n'
                   '\t(void)crt_pem_file; (void)key_pem_file; (void)pass;\n'
                   '\tthrow std::invalid_argument("Certificate file import requires MBEDTLS_FS_IO");\n'
                   '#else\n' + body + '\n#endif\n')
    text = text[:start] + replacement + text[end + len(end_marker) - 2:]
write_changed(source, text)

# Preserve numeric diagnostics from runtime exceptions. Dependency messages
# can contain remote credentials, so do not print exception text or enable
# raw library logging even when diagnosing native initialization failures.
source = base / 'libdatachannel/src/capi.cpp'
text = source.read_text()
if 'x4_native_rtc_diagnostic' not in text:
    text = text.replace('#include <exception>', '#include <exception>\n#include <system_error>\n'
        '#ifdef X4_OPENORBIS\nextern "C" void x4_native_rtc_diagnostic(int, int);\n#endif')
    text = text.replace('} catch (const std::invalid_argument &e) {\n',
        '} catch (const std::invalid_argument &e) {\n#ifdef X4_OPENORBIS\n'
        '\tx4_native_rtc_diagnostic(1, 0);\n#endif\n', 1)
    text = text.replace('} catch (const std::exception &e) {\n',
        '} catch (const std::exception &e) {\n#ifdef X4_OPENORBIS\n'
        '\tif (auto system = dynamic_cast<const std::system_error *>(&e))\n'
        '\t\tx4_native_rtc_diagnostic(2, system->code().value());\n'
        '\telse x4_native_rtc_diagnostic(dynamic_cast<const std::runtime_error *>(&e) ? 3 : 4, 0);\n'
        '#endif\n', 1)
# Preserve explicit source identities and every non-receiver track. An unset
# recvonly source must not become a bare ssrc attribute through the C API.
ssrc_marker = '// XCloud4: omit undeclared sender SSRC for recvonly tracks.'
original_ssrc = (
    '\n\t\tdescription->addSSRC(init->ssrc,\n'
    '\t\t                     init->name ? std::make_optional(string(init->name)) : nullopt,\n'
    '\t\t                     init->msid ? std::make_optional(string(init->msid)) : nullopt,\n'
    '\t\t                     init->trackId ? std::make_optional(string(init->trackId)) : nullopt);\n')
guarded_ssrc = (
    '\n\t\t' + ssrc_marker + '\n'
    '\t\tif (direction != Description::Direction::RecvOnly || init->ssrc != 0 ||\n'
    '\t\t    init->name || init->msid || init->trackId) {\n'
    '\t\t\tdescription->addSSRC(init->ssrc,\n'
    '\t\t\t                     init->name ? std::make_optional(string(init->name)) : nullopt,\n'
    '\t\t\t                     init->msid ? std::make_optional(string(init->msid)) : nullopt,\n'
    '\t\t\t                     init->trackId ? std::make_optional(string(init->trackId)) : nullopt);\n'
    '\t\t}\n')
if ssrc_marker not in text:
    if text.count(original_ssrc) != 1:
        raise SystemExit('Unexpected pinned rtcAddTrackEx SSRC implementation')
    text = text.replace(original_ssrc, guarded_ssrc, 1)
if text.count(ssrc_marker) != 1 or text.count(guarded_ssrc) != 1 or original_ssrc in text:
    raise SystemExit('Incomplete rtcAddTrackEx recvonly SSRC guard')
write_changed(source, text)

source = base / 'libdatachannel/src/impl/tls.cpp'
text = source.read_text()
if 'x4_native_rtc_diagnostic' not in text:
    text = text.replace('#include <time.h>', '#include <time.h>\n#ifdef X4_OPENORBIS\n'
        'extern "C" void x4_native_rtc_diagnostic(int, int);\n#endif')
    text = text.replace('\t\tconst size_t bufferSize = 1024;',
        '#ifdef X4_OPENORBIS\n\t\tx4_native_rtc_diagnostic(5, ret);\n#endif\n'
        '\t\tconst size_t bufferSize = 1024;', 1)
write_changed(source, text)

source = base / 'libdatachannel/src/impl/init.cpp'
text = source.read_text()
if 'x4_native_rtc_diagnostic' not in text:
    text = text.replace('namespace rtc::impl {',
        '#ifdef X4_OPENORBIS\nextern "C" void x4_native_rtc_diagnostic(int, int);\n'
        '#define X4_INIT_EVENT(id,value) ::x4_native_rtc_diagnostic(id,value)\n'
        '#else\n#define X4_INIT_EVENT(id,value) ((void)0)\n#endif\n\nnamespace rtc::impl {', 1)
    text = text.replace('\tThreadPool::Instance().spawn(count);',
        '\tX4_INIT_EVENT(10, int(count));\n\tThreadPool::Instance().spawn(count);\n\tX4_INIT_EVENT(11, 0);')
    text = text.replace('\tmbedtls::init();',
        '\tX4_INIT_EVENT(12, 0);\n\tmbedtls::init();\n\tX4_INIT_EVENT(13, 0);')
    for expression, event in (('SctpTransport::Init();', 14), ('DtlsTransport::Init();', 15),
                               ('DtlsSrtpTransport::Init();', 16), ('IceTransport::Init();', 17)):
        text = text.replace('\t' + expression, '\t' + expression + '\n\tX4_INIT_EVENT(' + str(event) + ', 0);', 1)
write_changed(source, text)

source = base / 'libdatachannel/src/impl/threadpool.cpp'
text = source.read_text()
if 'x4_native_rtc_diagnostic' not in text:
    original_spawn = '\twhile (count-- > 0)\n\t\tmWorkers.emplace_back(std::bind(&ThreadPool::run, this));'
    if original_spawn not in text:
        raise SystemExit('Unexpected pinned ThreadPool::spawn implementation')
    text = text.replace('namespace rtc::impl {',
        '#ifdef X4_OPENORBIS\nextern "C" void x4_native_rtc_diagnostic(int, int);\n'
        '#endif\n\nnamespace rtc::impl {', 1)
    text = text.replace(original_spawn,
        '\twhile (count-- > 0) {\n#ifdef X4_OPENORBIS\n'
        '\t\ttry {\n\t\t\tmWorkers.emplace_back(std::bind(&ThreadPool::run, this));\n'
        '\t\t} catch (...) {\n\t\t\t::x4_native_rtc_diagnostic(19, int(mWorkers.size()));\n\t\t\tthrow;\n\t\t}\n'
        '\t\t::x4_native_rtc_diagnostic(18, int(mWorkers.size()));\n'
        '#else\n\t\tmWorkers.emplace_back(std::bind(&ThreadPool::run, this));\n#endif\n\t}')
for marker in ('::x4_native_rtc_diagnostic(19, int(mWorkers.size()));',
               '::x4_native_rtc_diagnostic(18, int(mWorkers.size()));'):
    if marker not in text:
        raise SystemExit('ThreadPool worker diagnostic was not applied: ' + marker)
write_changed(source, text)

# First-offer diagnostics are fixed numeric stages only. Never print SDP,
# ICE credentials, addresses, certificate bytes or exception messages.
def checked_patch(source, replacements, marker):
    text = source.read_text()
    if marker not in text:
        for original, replacement in replacements:
            if text.count(original) != 1:
                raise SystemExit('Unexpected pinned offer implementation: ' + str(source) + ': ' + original)
            text = text.replace(original, replacement, 1)
    if marker not in text:
        raise SystemExit('Offer diagnostic was not applied: ' + str(source))
    for original, replacement in replacements:
        if replacement not in text:
            raise SystemExit('Incomplete pinned diagnostic patch: ' + str(source) + ': ' + original)
    write_changed(source, text)

source = base / 'libdatachannel/src/peerconnection.cpp'
checked_patch(source, [
    ('namespace rtc {', '#ifdef X4_OPENORBIS\nextern "C" void x4_native_rtc_diagnostic(int, int);\n'
     '#define X4_OFFER_EVENT(id,value) ::x4_native_rtc_diagnostic(id,value)\n'
     '#else\n#define X4_OFFER_EVENT(id,value) ((void)0)\n#endif\n\nnamespace rtc {'),
    ('\tauto iceTransport = impl()->initIceTransport();\n\tif (!iceTransport)\n\t\treturn; // closed\n\n\tif (init.iceUfrag',
     '\tX4_OFFER_EVENT(20, 0);\n\tauto iceTransport = impl()->initIceTransport();\n\tX4_OFFER_EVENT(21, 0);\n'
     '\tif (!iceTransport)\n\t\treturn; // closed\n\n\tif (init.iceUfrag'),
    ('\tDescription local = iceTransport->getLocalDescription(type);',
     '\tDescription local = iceTransport->getLocalDescription(type);\n\tX4_OFFER_EVENT(22, 0);'),
    ('\timpl()->populateLocalDescription(local);',
     '\timpl()->populateLocalDescription(local);\n\tX4_OFFER_EVENT(23, local.mediaCount());'),
    ('\timpl()->processLocalDescription(std::move(local));',
     '\timpl()->processLocalDescription(std::move(local));\n\tX4_OFFER_EVENT(24, 0);'),
    ('\t\ticeTransport->gatherLocalCandidates(impl()->localBundleMid());',
     '\t\tX4_OFFER_EVENT(25, 0);\n\t\ticeTransport->gatherLocalCandidates(impl()->localBundleMid());\n\t\tX4_OFFER_EVENT(26, 0);'),
], 'X4_OFFER_EVENT(26, 0);')

source = base / 'libdatachannel/src/impl/icetransport.cpp'
checked_patch(source, [
    ('namespace rtc::impl {', '#ifdef X4_OPENORBIS\nextern "C" void x4_native_rtc_diagnostic(int, int);\n'
     '#define X4_ICE_EVENT(id,value) ::x4_native_rtc_diagnostic(id,value)\n'
     '#else\n#define X4_ICE_EVENT(id,value) ((void)0)\n#endif\n\nnamespace rtc::impl {'),
    ('\tmAgent = decltype(mAgent)(juice_create(&jconfig), juice_destroy);',
     '\tX4_ICE_EVENT(30, 0);\n\tmAgent = decltype(mAgent)(juice_create(&jconfig), juice_destroy);\n\tX4_ICE_EVENT(31, mAgent ? 0 : -1);'),
    ('\tif (juice_get_local_description(mAgent.get(), sdp, JUICE_MAX_SDP_STRING_LEN) < 0)',
     '\tconst int localResult = juice_get_local_description(mAgent.get(), sdp, JUICE_MAX_SDP_STRING_LEN);\n'
     '\tX4_ICE_EVENT(32, localResult);\n\tif (localResult < 0)'),
    ('\tif (juice_gather_candidates(mAgent.get()) < 0) {',
     '\tconst int gatherResult = juice_gather_candidates(mAgent.get());\n'
     '\tX4_ICE_EVENT(33, gatherResult);\n\tif (gatherResult < 0) {'),
], 'X4_ICE_EVENT(33, gatherResult);')

source = base / 'libdatachannel/src/impl/peerconnection.cpp'
checked_patch(source, [
    ('namespace rtc::impl {', '#include <system_error>\n#ifdef X4_OPENORBIS\n'
     'extern "C" void x4_native_rtc_diagnostic(int, int);\n'
     '#define X4_ICE_EXCEPTION_EVENT(id,value) ::x4_native_rtc_diagnostic(id,value)\n'
     '#else\n#define X4_ICE_EXCEPTION_EVENT(id,value) ((void)0)\n#endif\n\nnamespace rtc::impl {'),
    ('\t} catch (const std::exception &e) {\n\t\tPLOG_ERROR << e.what();\n\t\tchangeState(State::Failed);\n'
     '\t\tthrow std::runtime_error("ICE transport initialization failed");',
     '\t} catch (const std::exception &e) {\n'
     '\t\tif (auto nativeSystem = dynamic_cast<const std::system_error *>(&e)) {\n'
     '\t\t\tX4_ICE_EXCEPTION_EVENT(2, nativeSystem->code().value());\n\t\t\tX4_ICE_EXCEPTION_EVENT(35, 2);\n'
     '\t\t} else X4_ICE_EXCEPTION_EVENT(35, dynamic_cast<const std::runtime_error *>(&e) ? 3 : 4);\n'
     '\t\tPLOG_ERROR << e.what();\n\t\tchangeState(State::Failed);\n'
     '\t\tthrow std::runtime_error("ICE transport initialization failed");'),
], 'X4_ICE_EXCEPTION_EVENT(35, 2);')

source = base / 'libdatachannel/src/impl/certificate.cpp'
checked_patch(source, [
    ('namespace rtc::impl {', '#ifdef X4_OPENORBIS\nextern "C" void x4_native_rtc_diagnostic(int, int);\n'
     '#define X4_CERT_EVENT(id,value) ::x4_native_rtc_diagnostic(id,value)\n'
     '#else\n#define X4_CERT_EVENT(id,value) ((void)0)\n#endif\n\nnamespace rtc::impl {'),
    ('\tPLOG_DEBUG << "Generating certificate (MbedTLS)";',
     '\tPLOG_DEBUG << "Generating certificate (MbedTLS)";\n\tX4_CERT_EVENT(40, 0);'),
    ('\t\tauto now = std::chrono::system_clock::now();',
     '\t\tX4_CERT_EVENT(41, 0);\n\t\tauto now = std::chrono::system_clock::now();'),
    ('\t\tstring notAfter = mbedtls::format_time(now + std::chrono::hours(24 * 365));',
     '\t\tstring notAfter = mbedtls::format_time(now + std::chrono::hours(24 * 365));\n\t\tX4_CERT_EVENT(42, 0);'),
    ('\t\tstd::string name = std::string("O=" + commonName + ",CN=" + commonName);',
     '\t\tX4_CERT_EVENT(43, 0);\n\t\tstd::string name = std::string("O=" + commonName + ",CN=" + commonName);'),
    ('\t\tif (certificateLen <= 0) {', '\t\tX4_CERT_EVENT(44, int(certificateLen));\n\t\tif (certificateLen <= 0) {'),
    ('\treturn Certificate(std::move(crt), std::move(pk));\n}\n\nstd::tuple<shared_ptr<mbedtls_x509_crt>',
     '\tX4_CERT_EVENT(45, 0);\n\treturn Certificate(std::move(crt), std::move(pk));\n}\n\nstd::tuple<shared_ptr<mbedtls_x509_crt>'),
], 'X4_CERT_EVENT(44, int(certificateLen));')

source = base / 'libdatachannel/src/impl/tls.cpp'
checked_patch(source, [
    ('\tif (my_gmtime(t, &g) != 0)\n\t\treturn 0;\n\n\treturn ::strftime(buf, size, format, &g);',
     '\tconst int timeResult = my_gmtime(t, &g);\n#ifdef X4_OPENORBIS\n'
     '\tx4_native_rtc_diagnostic(48, timeResult);\n#endif\n\tif (timeResult != 0)\n\t\treturn 0;\n\n'
     '\tconst size_t formattedSize = ::strftime(buf, size, format, &g);\n#ifdef X4_OPENORBIS\n'
     '\tx4_native_rtc_diagnostic(49, int(formattedSize));\n#endif\n\treturn formattedSize;'),
], 'x4_native_rtc_diagnostic(49, int(formattedSize));')

juice_diagnostic_header = ('#include "log.h"\n#ifdef X4_OPENORBIS\n'
    'extern void x4_native_rtc_diagnostic(int, int);\n'
    '#define X4_JUICE_EVENT(id,value) x4_native_rtc_diagnostic(id,value)\n'
    '#else\n#define X4_JUICE_EVENT(id,value) ((void)0)\n#endif')
source = base / 'libdatachannel/deps/libjuice/src/udp.c'
checked_patch(source, [
    ('#include "log.h"', juice_diagnostic_header),
    ('\tif (sock == INVALID_SOCKET) {\n\t\tJLOG_WARN("UDP socket creation failed, errno=%d", sockerrno);',
     '\tif (sock == INVALID_SOCKET) {\n\t\tX4_JUICE_EVENT(50, sockerrno);\n'
     '\t\tJLOG_WARN("UDP socket creation failed, errno=%d", sockerrno);'),
    ('\t\tJLOG_WARN("UDP socket binding failed, errno=%d", sockerrno);',
     '\t\tX4_JUICE_EVENT(53, sockerrno);\n\t\tJLOG_WARN("UDP socket binding failed, errno=%d", sockerrno);'),
    ('\t\tJLOG_WARN("UDP socket binding failed on port %hu, errno=%d", port, sockerrno);',
     '\t\tX4_JUICE_EVENT(53, sockerrno);\n\t\tJLOG_WARN("UDP socket binding failed on port %hu, errno=%d", port, sockerrno);'),
    ('\t\tJLOG_WARN("UDP socket binding failed on port range %s:[%hu,%hu], errno=%d",',
     '\t\tX4_JUICE_EVENT(53, sockerrno);\n\t\tJLOG_WARN("UDP socket binding failed on port range %s:[%hu,%hu], errno=%d",'),
    ('\tif (getaddrinfo(config->bind_address, "0", &hints, &ai_list) != 0) {',
     '\tconst int bind_address_result = getaddrinfo(config->bind_address, "0", &hints, &ai_list);\n'
     '\tX4_JUICE_EVENT(54, bind_address_result);\n\tif (bind_address_result != 0) {'),
], 'X4_JUICE_EVENT(54, bind_address_result);')

source = base / 'libdatachannel/deps/libjuice/src/agent.c'
checked_patch(source, [
    ('#include "log.h"', juice_diagnostic_header),
    ('\tif (conn_create(agent, &socket_config)) {',
     '\tconst int connection_result = conn_create(agent, &socket_config);\n'
     '\tX4_JUICE_EVENT(55, connection_result);\n\tif (connection_result) {'),
    ('\tint records_count = conn_get_addrs(agent, records, ICE_MAX_CANDIDATES_COUNT - 1);',
     '\tint records_count = conn_get_addrs(agent, records, ICE_MAX_CANDIDATES_COUNT - 1);\n'
     '\tX4_JUICE_EVENT(56, records_count);'),
    ('\t\tint ret = thread_init(&agent->resolver_thread, resolver_thread_entry, agent);',
     '\t\tint ret = thread_init(&agent->resolver_thread, resolver_thread_entry, agent);\n\t\tX4_JUICE_EVENT(57, ret);'),
], 'X4_JUICE_EVENT(57, ret);')

source = base / 'libdatachannel/deps/libjuice/src/conn_poll.c'
checked_patch(source, [
    ('#include "log.h"', juice_diagnostic_header),
    ('\tif (pipe(pipefds)) {',
     '\tconst int pipe_result = pipe(pipefds);\n'
     '\tX4_JUICE_EVENT(59, pipe_result == 0 ? 0 : errno);\n\tif (pipe_result) {'),
    ('\tfcntl(pipefds[0], F_SETFL, O_NONBLOCK);\n\tfcntl(pipefds[1], F_SETFL, O_NONBLOCK);',
     '\tif (fcntl(pipefds[0], F_SETFL, O_NONBLOCK) < 0) X4_JUICE_EVENT(60, errno);\n'
     '\tif (fcntl(pipefds[1], F_SETFL, O_NONBLOCK) < 0) X4_JUICE_EVENT(61, errno);'),
    ('\tint ret = thread_init(&registry_impl->thread, conn_thread_entry, registry);',
     '\tint ret = thread_init(&registry_impl->thread, conn_thread_entry, registry);\n\tX4_JUICE_EVENT(58, ret);'),
], 'X4_JUICE_EVENT(58, ret);')

# Keep the original ioctl operation on other platforms. The native adapter
# requests PS4 SO_NBIO with a four-byte int and confirms the mode through
# getsockopt. A rejected operation remains an error; never use blocking UDP.
for filename in ('udp.c', 'tcp.c'):
    source = base / 'libdatachannel/deps/libjuice/src' / filename
    text = source.read_text()
    if juice_diagnostic_header not in text:
        if text.count('#include "log.h"') != 1:
            raise SystemExit('Unexpected pinned socket diagnostic include: ' + filename)
        text = text.replace('#include "log.h"', juice_diagnostic_header, 1)
    nbio_declaration = '#ifndef X4_OPENORBIS\n\tctl_t nbio = 1;\n#endif'
    if nbio_declaration not in text:
        if text.count('\tctl_t nbio = 1;') != 1:
            raise SystemExit('Unexpected pinned socket mode argument: ' + filename)
        text = text.replace('\tctl_t nbio = 1;',nbio_declaration,1)
    replacement = ('#ifdef X4_OPENORBIS\n\tconst int nonblock_result = x4_native_socket_nonblock(sock);\n'
        '#else\n\tconst int nonblock_result = ioctlsocket(sock, FIONBIO, &nbio);\n#endif\n'
        '\tX4_JUICE_EVENT(64, nonblock_result == 0 ? 0 : sockerrno);\n'
        '\tif (nonblock_result) {')
    if replacement not in text:
        original_forms = (
            '\tif (fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK) < 0) {',
            '\tint existing_flags = fcntl(sock, F_GETFL, 0);\n'
            '\tif (existing_flags < 0) X4_JUICE_EVENT(51, sockerrno);\n'
            '\tif (fcntl(sock, F_SETFL, existing_flags | O_NONBLOCK) < 0) {\n'
            '\t\tX4_JUICE_EVENT(52, sockerrno);',
            '\tconst int nonblock_result = ioctlsocket(sock, FIONBIO, &nbio);\n'
            '\tX4_JUICE_EVENT(64, nonblock_result == 0 ? 0 : sockerrno);\n'
            '\tif (nonblock_result) {',
        )
        matches = [original for original in original_forms if original in text]
        if len(matches) != 1 or text.count(matches[0]) != 1:
            raise SystemExit('Unexpected pinned socket nonblocking implementation: ' + filename)
        text = text.replace(matches[0], replacement, 1)
    if text.count(replacement) != 1:
        raise SystemExit('Incomplete native socket nonblocking patch: ' + filename)
    write_changed(source, text)
# The SDK declares Linux CLOCK_BOOTTIME=7, which the native clock adapter
# cannot serve. This pinned libjuice path therefore returned timestamp zero.
# Use the real monotonic clock for this target; retain every other target's
# original clock selection. Report a clock failure only once per process.
source = base / 'libdatachannel/deps/libjuice/src/timestamp.c'
checked_patch(source, [
    ('#include "timestamp.h"',
     '#include "timestamp.h"\n#ifdef X4_OPENORBIS\n#include <errno.h>\n#include <stdatomic.h>\n'
     'extern void x4_native_rtc_diagnostic(int, int);\n#endif'),
    ('#ifdef CLOCK_BOOTTIME\n\tconst clockid_t clock_id = CLOCK_BOOTTIME;',
     '#if defined(X4_OPENORBIS)\n\t// XCloud4: native monotonic time, not Linux CLOCK_BOOTTIME.\n'
     '\tconst clockid_t clock_id = CLOCK_MONOTONIC;\n#elif defined(CLOCK_BOOTTIME)\n'
     '\tconst clockid_t clock_id = CLOCK_BOOTTIME;'),
    ('\tif (clock_gettime(clock_id, &ts))\n\t\treturn 0;',
     '\tconst int clock_result = clock_gettime(clock_id, &ts);\n\tif (clock_result) {\n'
     '#ifdef X4_OPENORBIS\n\t\tconst int clock_errno = errno;\n'
     '\t\tstatic atomic_flag reported = ATOMIC_FLAG_INIT;\n'
     '\t\tif (!atomic_flag_test_and_set(&reported)) {\n'
     '\t\t\tx4_native_rtc_diagnostic(74, clock_result);\n'
     '\t\t\tx4_native_rtc_diagnostic(75, clock_errno);\n\t\t}\n'
     '\t\terrno = clock_errno;\n#endif\n\t\treturn 0;\n\t}'),
], '// XCloud4: native monotonic time, not Linux CLOCK_BOOTTIME.')

# Sparse numeric failure sites distinguish native socket errors from an ICE
# connectivity timeout. They never emit packets, candidates or credentials.
source = base / 'libdatachannel/deps/libjuice/src/conn_poll.c'
checked_patch(source, [
    ('\t\tJLOG_WARN("UDP socket error");\n\t\tagent_conn_fail(agent);',
     '\t\tJLOG_WARN("UDP socket error");\n\t\tX4_JUICE_EVENT(76, 1);\n'
     '\t\tX4_JUICE_EVENT(77, (int)pfd->revents);\n\t\tagent_conn_fail(agent);'),
    ('\t\t} else {\n\t\t\tagent_conn_fail(agent);\n\t\t\tconn_impl->state = CONN_STATE_FINISHED;',
     '\t\t} else {\n\t\t\tX4_JUICE_EVENT(76, 2);\n\t\t\tX4_JUICE_EVENT(77, -ret);\n'
     '\t\t\tagent_conn_fail(agent);\n\t\t\tconn_impl->state = CONN_STATE_FINISHED;'),
], 'X4_JUICE_EVENT(76, 1);')

source = base / 'libdatachannel/deps/libjuice/src/agent.c'
checked_patch(source, [
    ('\t\tJLOG_WARN("Lost connectivity");\n\t\tagent_change_state(agent, JUICE_STATE_FAILED);',
     '\t\tJLOG_WARN("Lost connectivity");\n\t\tX4_JUICE_EVENT(76, 3);\n'
     '\t\tagent_change_state(agent, JUICE_STATE_FAILED);'),
    ('\t\t\tJLOG_INFO("Connectivity timer expired");\n\t\t\tagent_change_state(agent, JUICE_STATE_FAILED);',
     '\t\t\tJLOG_INFO("Connectivity timer expired");\n\t\t\tX4_JUICE_EVENT(76, 4);\n'
     '\t\t\tagent_change_state(agent, JUICE_STATE_FAILED);'),
    ('\t\tagent->state = state;\n\t\tif (agent->config.cb_state_changed)',
     '\t\tagent->state = state;\n\t\tif (state == JUICE_STATE_FAILED) {\n'
     '\t\t\tX4_JUICE_EVENT(88, agent->candidate_pairs_count);\n'
     '\t\t\tX4_JUICE_EVENT(89, agent->remote.candidates_count);\n'
     '\t\t\tX4_JUICE_EVENT(90, agent->entries_count);\n\t\t}\n'
     '\t\tif (agent->config.cb_state_changed)'),
], 'X4_JUICE_EVENT(76, 4);')

# Preserve all transport states, exceptions and cleanup. The existing native
# MbedTLS check already reports numeric error codes; add the missing phase.
source = base / 'libdatachannel/src/impl/peerconnection.cpp'
checked_patch(source, [
    ('\t\tPLOG_VERBOSE << "Starting DTLS transport";',
     '\t\tPLOG_VERBOSE << "Starting DTLS transport";\n\t\tX4_ICE_EXCEPTION_EVENT(86, 0);'),
    ('\t\treturn emplaceTransport(this, &mDtlsTransport, std::move(transport));',
     '\t\tX4_ICE_EXCEPTION_EVENT(86, 1);\n'
     '\t\treturn emplaceTransport(this, &mDtlsTransport, std::move(transport));'),
    ('\t} catch (const std::exception &e) {\n\t\tPLOG_ERROR << e.what();\n'
     '\t\tchangeState(State::Failed);\n\t\tthrow std::runtime_error("DTLS transport initialization failed");',
     '\t} catch (const std::exception &e) {\n'
     '\t\tif (auto nativeSystem = dynamic_cast<const std::system_error *>(&e)) {\n'
     '\t\t\tX4_ICE_EXCEPTION_EVENT(78, 2);\n'
     '\t\t\tX4_ICE_EXCEPTION_EVENT(79, nativeSystem->code().value());\n'
     '\t\t} else X4_ICE_EXCEPTION_EVENT(78, dynamic_cast<const std::runtime_error *>(&e) ? 3 : 4);\n'
     '\t\tPLOG_ERROR << e.what();\n\t\tchangeState(State::Failed);\n'
     '\t\tthrow std::runtime_error("DTLS transport initialization failed");'),
] + [
    ('case ' + transport + 'Transport::State::' + state + ':',
     'case ' + transport + 'Transport::State::' + state + ':\n'
     '\t\t\t\t\t\tX4_ICE_EXCEPTION_EVENT(' + str(event) + ', static_cast<int>(transportState));')
    for transport, event in (('Dtls', 80), ('Sctp', 87))
    for state in ('Connected', 'Failed', 'Disconnected')
], 'X4_ICE_EXCEPTION_EVENT(78, 2);')

source = base / 'libdatachannel/src/impl/dtlstransport.cpp'
dtls_recv_tail = ('\t} catch (const std::exception &e) {\n\t\tPLOG_ERROR << "DTLS recv: " << e.what();\n\t}\n\n'
                 '\tif (state() == State::Connected) {\n\t\tPLOG_INFO << "DTLS closed";\n'
                 '\t\tchangeState(State::Disconnected);\n\t\trecv(nullptr);\n\t} else {\n'
                 '\t\tPLOG_ERROR << "DTLS handshake failed";\n\t\tchangeState(State::Failed);\n\t}\n}\n\n'
                 'int DtlsTransport::CertificateCallback(void *ctx, mbedtls_x509_crt *crt, int /*depth*/,')
checked_patch(source, [
    ('#include <exception>',
     '#include <exception>\n#include <system_error>\n#ifdef X4_OPENORBIS\n'
     'extern "C" void x4_native_rtc_diagnostic(int, int);\n'
     '#define X4_DTLS_EVENT(id,value) ::x4_native_rtc_diagnostic(id,value)\n'
     '#else\n#define X4_DTLS_EVENT(id,value) ((void)0)\n#endif'),
    (dtls_recv_tail, dtls_recv_tail.replace(
     '\t} catch (const std::exception &e) {\n',
     '\t} catch (const std::exception &e) {\n'
     '\t\tif (auto nativeSystem = dynamic_cast<const std::system_error *>(&e)) {\n'
     '\t\t\tX4_DTLS_EVENT(81, 2);\n\t\t\tX4_DTLS_EVENT(82, nativeSystem->code().value());\n'
     '\t\t} else X4_DTLS_EVENT(81, dynamic_cast<const std::runtime_error *>(&e) ? 3 : 4);\n', 1)),
], 'X4_DTLS_EVENT(81, 2);')

source = base / 'libdatachannel/src/impl/dtlssrtptransport.cpp'
checked_patch(source, [
    ('namespace rtc::impl {',
     '#ifdef X4_OPENORBIS\nextern "C" void x4_native_rtc_diagnostic(int, int);\n'
     '#define X4_SRTP_EVENT(id,value) ::x4_native_rtc_diagnostic(id,value)\n'
     '#else\n#define X4_SRTP_EVENT(id,value) ((void)0)\n#endif\n\nnamespace rtc::impl {'),
    ('\tif (srtp_err_status_t err = srtp_create(&mSrtpIn, nullptr)) {',
     '\tif (srtp_err_status_t err = srtp_create(&mSrtpIn, nullptr)) {\n\t\tX4_SRTP_EVENT(83, static_cast<int>(err));'),
    ('\tif (srtp_err_status_t err = srtp_create(&mSrtpOut, nullptr)) {',
     '\tif (srtp_err_status_t err = srtp_create(&mSrtpOut, nullptr)) {\n\t\tX4_SRTP_EVENT(84, static_cast<int>(err));'),
    ('void DtlsSrtpTransport::postHandshake() {\n\tif (mInitDone)\n\t\treturn;',
     'void DtlsSrtpTransport::postHandshake() {\n\tif (mInitDone)\n\t\treturn;\n\tX4_SRTP_EVENT(85, 0);'),
    ('\tmInitDone = true;', '\tmInitDone = true;\n\tX4_SRTP_EVENT(85, 1);'),
], 'X4_SRTP_EVENT(85, 1);')

print('OpenOrbis dependency overlay prepared:', overlay)
