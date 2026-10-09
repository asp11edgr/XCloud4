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

# Native POSIX fcntl uses the SDK's BSD O_NONBLOCK flag. The SDK has no
# FIONBIO declaration, so use the standard descriptor operation instead.
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
    text = text.replace('namespace rtc::impl {',
        '#ifdef X4_OPENORBIS\nextern "C" void x4_native_rtc_diagnostic(int, int);\n'
        '#endif\n\nnamespace rtc::impl {', 1)
    text = text.replace('\twhile (count-- > 0)\n\t\tmWorkers.emplace_back(std::bind(&ThreadPool::run, this));',
        '\twhile (count-- > 0) {\n#ifdef X4_OPENORBIS\n'
        '\t\ttry {\n\t\t\tmWorkers.emplace_back(std::bind(&ThreadPool::run, this));\n'
        '\t\t} catch (...) {\n\t\t\t::x4_native_rtc_diagnostic(19, int(mWorkers.size()));\n\t\t\tthrow;\n\t\t}\n'
        '\t\t::x4_native_rtc_diagnostic(18, int(mWorkers.size()));\n'
        '#else\n\t\tmWorkers.emplace_back(std::bind(&ThreadPool::run, this));\n#endif\n\t}')
write_changed(source, text)
print('OpenOrbis dependency overlay prepared:', overlay)
