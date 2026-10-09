#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Export the actual patched build trees without building or changing them.
set -euo pipefail
SCRIPT_PATH=$(realpath -- "${BASH_SOURCE[0]}")
RTC_ROOT=$(realpath -- "${1:-${X4_WEBRTC_ROOT:-$HOME/.local/share/xcloud4/webrtc}}")
PROJECT_ROOT=$(realpath -- "${X4_PROJECT_ROOT:-$HOME/Projects/XCloud4}")
if [[ -n ${X4_SOURCE_VERSION:-} ]]; then
    SOURCE_VERSION=$X4_SOURCE_VERSION
else
    SOURCE_VERSION=$(python3 - "$PROJECT_ROOT/scripts/empaquetar.sh" <<'PY_VERSION'
import pathlib, re, sys
source = pathlib.Path(sys.argv[1]).read_text()
match = re.search(r'''^PACKAGE_VERSION=['"]([0-9]+\.[0-9]+\.[0-9]+)['"]$''', source, re.M)
if not match:
    raise SystemExit('Missing package version; set X4_SOURCE_VERSION explicitly')
print(match[1])
PY_VERSION
)
fi
[[ "$SOURCE_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo 'Invalid source version' >&2; exit 1; }
ARCHIVE_ROOT="XCloud4-$SOURCE_VERSION-dependency-sources"
OUTPUT=$(realpath -m -- "${2:-$RTC_ROOT/$ARCHIVE_ROOT.tar.gz}")
[[ ! -e "$OUTPUT" && ! -L "$OUTPUT" ]] || { echo 'Refusing to replace an existing source snapshot' >&2; exit 1; }
STAGING=$(mktemp -d "$RTC_ROOT/.x4-source-export.XXXXXX")
cleanup() {
    if [[ -n ${STAGING:-} && "$STAGING" == "$RTC_ROOT"/.x4-source-export.* && -d "$STAGING" ]]; then
        rm -rf -- "$STAGING"
    fi
}
trap cleanup EXIT

# SOURCE_DATE_EPOCH fixes metadata for reproducible exports of unchanged files.
EPOCH=${SOURCE_DATE_EPOCH:-$(git -C "$RTC_ROOT/libdatachannel" show -s --format=%ct HEAD)}
[[ "$EPOCH" =~ ^[0-9]+$ ]] || { echo 'Invalid SOURCE_DATE_EPOCH' >&2; exit 1; }
mkdir -p -- "$(dirname -- "$OUTPUT")"

python3 - "$RTC_ROOT" "$PROJECT_ROOT" "$STAGING/$ARCHIVE_ROOT" "$SCRIPT_PATH" "$SOURCE_VERSION" <<'PY'
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys

rtc, project, dest, exporter = map(pathlib.Path, sys.argv[1:5])
version = sys.argv[5]
pins = [
    ('libdatachannel', 'https://github.com/paullouisageneau/libdatachannel.git', 'bdc5ff28e9d3b863144c94a677ecf5bf043aaf15'),
    ('libdatachannel/deps/libjuice', 'https://github.com/paullouisageneau/libjuice.git', 'b89c792e3612faf2f12cf35bcc56857313a06be3'),
    ('libdatachannel/deps/usrsctp', 'https://github.com/paullouisageneau/usrsctp.git', 'fec583d54493f879d2ae44a743423bf8a04371ab'),
    ('libdatachannel/deps/libsrtp', 'https://github.com/cisco/libsrtp.git', 'd33b8ffb1491a0b4b58a206889f09800cf7310ab'),
    ('libdatachannel/deps/json', 'https://github.com/nlohmann/json.git', '55f93686c01528224f448c19128836e7df245f72'),
    ('libdatachannel/deps/plog', 'https://github.com/SergiusTheBest/plog.git', '94899e0b926ac1b0f4750bfbd495167b4a6ae9ef'),
    ('mbedtls', 'https://github.com/Mbed-TLS/mbedtls.git', '068ff080b369adfac81509f9b57b2afabaf82dc5'),
    ('mbedtls/framework', 'https://github.com/Mbed-TLS/mbedtls-framework.git', 'dde0c4a0e448a0552f18817dcea633bb851fd288'),
]
opus_sha = '65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1'
sources = {}
components = []

def git(path, *args):
    return subprocess.check_output(['git', '-C', str(path), *args])

def add(source, relative):
    if not source.is_file() and not source.is_symlink():
        raise SystemExit('Missing required source: ' + str(source))
    key = pathlib.PurePosixPath(relative)
    if key.is_absolute() or '..' in key.parts or '.git' in key.parts:
        raise SystemExit('Unsafe archive path: ' + str(key))
    previous = sources.get(str(key))
    if previous is not None and previous != source:
        raise SystemExit('Conflicting archive path: ' + str(key))
    sources[str(key)] = source

def snapshot(source, check_target=True):
    if source.is_symlink():
        target = os.readlink(source)
        if os.path.isabs(target):
            raise SystemExit('Absolute source symlink is not exportable: ' + str(source))
        if check_target:
            resolved = source.resolve(strict=True)
            if not resolved.is_relative_to(rtc) and not resolved.is_relative_to(project):
                raise SystemExit('Source symlink escapes selected roots: ' + str(source))
        data = b'symlink\0' + os.fsencode(target)
        return {'type': 'symlink', 'target': target, 'sha256': hashlib.sha256(data).hexdigest()}
    digest = hashlib.sha256()
    with source.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return {'type': 'file', 'bytes': source.stat().st_size, 'sha256': digest.hexdigest()}

# Git-tracked files include original public fixtures and notices, but exclude
# generated files and accidental credentials. Each pinned submodule is copied
# explicitly, so the archive works without Git metadata or network checkout.
for name, url, expected in pins:
    tree = rtc / name
    actual = git(tree, 'rev-parse', 'HEAD').decode().strip()
    if actual != expected:
        raise SystemExit('Unexpected dependency revision: ' + name)
    changes = git(tree, 'diff', '--name-only', 'HEAD').decode().splitlines()
    components.append({'path': 'vendor/' + name, 'upstream': url, 'commit': actual,
                       'modified_paths': sorted(changes)})
    for raw in git(tree, 'ls-files', '-z').split(b'\0'):
        if not raw:
            continue
        relative = os.fsdecode(raw)
        source = tree / relative
        if source.is_dir() and not source.is_symlink():
            continue  # submodule directory is exported by its own pin
        if source.exists() or source.is_symlink():
            add(source, 'vendor/' + name + '/' + relative)

# Opus is the official release-tar tree, not a Git checkout. Builds reside in
# the sibling build-opus directory and are never traversed by this exporter.
opus = rtc / 'opus-1.5.2'
if not (opus / 'COPYING').is_file() or not (opus / 'configure').is_file():
    raise SystemExit('Expected official Opus 1.5.2 release source is absent')
if 'PACKAGE_VERSION="1.5.2"' not in (opus / 'package_version').read_text():
    raise SystemExit('Unexpected Opus release version')
components.append({'path': 'vendor/opus-1.5.2', 'release': '1.5.2',
                   'upstream': 'https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz',
                   'official_release_archive_sha256': opus_sha})
for current, dirs, files in os.walk(opus):
    dirs[:] = sorted(d for d in dirs if d != '.git' and d != 'prefix' and not d.startswith('build') and d != '__pycache__')
    for name in sorted(files):
        if name.endswith(('.o', '.a', '.so', '.dll', '.exe', '.elf', '.pkg', '.log', '.tmp', '.pyc')) or name.startswith('.env'):
            continue
        source = pathlib.Path(current) / name
        add(source, 'vendor/opus-1.5.2/' + source.relative_to(opus).as_posix())

for name in ('prepare_port.py', 'build_native.sh', 'openorbis.cmake', 'mbedtls-user-config.h'):
    add(rtc / name, 'scripts/webrtc/' + name)
add(exporter, 'scripts/webrtc/export_source.sh')
for source in sorted((rtc / 'overlay').rglob('*')):
    if source.is_file() or source.is_symlink():
        add(source, 'overlay/' + source.relative_to(rtc / 'overlay').as_posix())
if not any(name.startswith('overlay/') for name in sources):
    raise SystemExit('Actual build overlay is absent')

add(project / 'LICENSE', 'LICENSE')
add(project / 'THIRD_PARTY_NOTICES.md', 'THIRD_PARTY_NOTICES.md')
license_dir = project / 'docs/licenses'
if not license_dir.is_dir():
    raise SystemExit('Project license directory is absent; synchronize it before exporting')
for source in sorted(license_dir.iterdir()):
    if source.is_file():
        add(source, 'docs/licenses/' + source.name)
streaming = project / 'src/streaming'
for name in ('rtc_native.c', 'rtc_native.h', 'rtc_net.c', 'rtc_transport.h'):
    if not (streaming / name).is_file():
        raise SystemExit('Missing native streaming adapter: ' + name)
for source in sorted(streaming.iterdir()):
    if source.is_file() and source.suffix in ('.c', '.h', '.cpp', '.cc', '.cxx', '.hpp'):
        add(source, 'src/streaming/' + source.name)

# Record the exact file bytes first and verify every original after the copy.
# A simultaneous vendor edit makes the export fail instead of producing a
# source archive that does not match the build trees.
before = {name: snapshot(source) for name, source in sorted(sources.items())}
dest.mkdir(parents=True)
for name, source in sorted(sources.items()):
    output = dest / name
    output.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, output, follow_symlinks=False)
    if snapshot(output, check_target=False) != before[name]:
        raise SystemExit('Source changed while copying: ' + name)
for name, source in sorted(sources.items()):
    if snapshot(source) != before[name]:
        raise SystemExit('Source changed during export; retry after edits finish: ' + name)
    output = dest / name
    if output.is_symlink() and not output.resolve(strict=True).is_relative_to(dest):
        raise SystemExit('Exported symlink points outside the archive: ' + name)

manifest = {'format': 1, 'target': 'OpenOrbis PS4 firmware 12.00', 'version': version,
            'purpose': 'Exact patched dependency source snapshot for XCloud4 ' + version + ' development',
            'components': components, 'file_count': len(before),
            'files': [{'path': name, **metadata} for name, metadata in before.items()]}
(dest / 'SOURCE_MANIFEST.json').write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n')
(dest / 'SOURCE_SHA256SUMS').write_text(''.join(metadata['sha256'] + '  ' + name + '\n'
    for name, metadata in before.items() if metadata['type'] == 'file'))
(dest / 'README.md').write_text(f'''# XCloud4 {version} dependency sources

This archive contains the exact patched dependency working trees and native
port configuration captured from the build host. It includes pinned
submodules, original licenses, SDK ABI overlay headers and native streaming
adapters. It does not contain SDK binaries, generated libraries, Git metadata,
build caches, locally generated credentials, account tokens or runtime logs.
Original public upstream test fixtures are retained as source material.

SOURCE_MANIFEST.json records upstream pins, modified paths and every source
file hash. SOURCE_SHA256SUMS can check ordinary extracted files. Native secure
entropy and transport integration live in src/streaming. scripts/webrtc holds
the reproducible port/build configuration; its build script expects an
external OpenOrbis v0.5.4 installation. The full XCloud4 application source is
maintained separately in its repository.

Opus is pinned by the official 1.5.2 release/archive hash. This export records
the bytes of the actual release source tree; it does not run compilation or
tests. Successful export does not establish live video/audio on PS4.

See THIRD_PARTY_NOTICES.md and docs/licenses for applicable licenses and
attribution. Modified MPL sources retain their original license and are
included directly in this archive.
''')
print('Source files:', len(before))
print('Source bytes:', sum(info.get('bytes', 0) for info in before.values()))
for item in components:
    print(item['path'], item.get('commit', item.get('release')))
PY

# GNU tar + gzip -n produce stable ordering, timestamps and gzip metadata.
tar --sort=name --mtime="@$EPOCH" --owner=0 --group=0 --numeric-owner \
    --mode='u+rwX,go+rX,go-w,a-s' \
    --format=posix --pax-option=delete=atime,delete=ctime \
    -C "$STAGING" -cf "$STAGING/source.tar" "$ARCHIVE_ROOT"
gzip -n -1 -c "$STAGING/source.tar" > "$STAGING/source.tar.gz"
mv -- "$STAGING/source.tar.gz" "$OUTPUT"
sha256sum "$OUTPUT"
stat --format='Archive bytes: %s' "$OUTPUT"
