#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Static T26 lifecycle gate, with actual C preprocessor branch selection.

Host checks deliberately elide includes (not code or conditionals), so SDK
types need not be fabricated to inspect local X4Display declarations. Native
builds compile the original includes. For a full native preprocessor check,
set X4_LIFECYCLE_NATIVE_CPP=1 and X4_LIFECYCLE_CPPFLAGS to the real Make include
flags: the exact same assertions then inspect unmodified product input.
"""
import os
import pathlib
import re
import shlex
import subprocess

ROOT = pathlib.Path(os.environ.get('X4_T26_LIFECYCLE_SOURCE_DIR',
    pathlib.Path(__file__).resolve().parent.parent)).resolve()


def code_only(text):
    pattern = r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\''
    return re.sub(pattern, lambda m: '\n' * m.group(0).count('\n') + ' ', text,
                  flags=re.S)


def function_body(text, name):
    clean = code_only(text)
    match = re.search(r'\bint\s+' + re.escape(name) + r'\s*\(void\)\s*\{', clean)
    assert match, name + ' definition absent'
    start = match.end()
    depth = 1
    for index in range(start, len(clean)):
        depth += (clean[index] == '{') - (clean[index] == '}')
        if not depth:
            return clean[start:index]
    raise AssertionError('unclosed function body')


controller = ROOT / 'src/video/probe_t26_controller.c'
header = ROOT / 'src/video/probe_t26_controller.h'
header_code = code_only(header.read_text())
assert re.search(r'\bx4_t26_controller_run\s*\(void\)\s*'
                 r'__attribute__\s*\(\(\s*noreturn\s*\)\)', header_code), \
    'T26 entry declaration must have noreturn contract'
assert not re.search(r'\breturn\b', function_body(controller.read_text(),
                                                'x4_t26_controller_run')), \
    'T26 entry must have no path that returns to main/libc'

compiler = shlex.split(os.environ.get('CC', 'clang'))
extra = shlex.split(os.environ.get('X4_LIFECYCLE_CPPFLAGS', ''))
native = os.environ.get('X4_LIFECYCLE_NATIVE_CPP') == '1'
configurations = {
    'probe-t26': {'X4_VIDEO_PROBE': 1, 'X4_PROBE_T26': 1, 'X4_PUBLIC': 0,
                  'X4_RUMBLE': 0},
    'lab': {'X4_VIDEO_PROBE': 0, 'X4_PROBE_T26': 0, 'X4_PUBLIC': 0,
            'X4_RUMBLE': 1},
    'public': {'X4_VIDEO_PROBE': 0, 'X4_PROBE_T26': 0, 'X4_PUBLIC': 1,
               'X4_RUMBLE': 1},
}
common = {'X4_STREAM_PROFILE': 1, 'X4_VIDEO_CAPACITY_1080': 1,
          'X4_RECEIVER_NACK': 1, 'X4_NACK_GRACE_MS': 45}
files = [p for p in (ROOT / 'src').rglob('*.c')
         if re.search(r'\bX4Display\s+[A-Za-z_]\w*\b', code_only(p.read_text()))]
assert controller in files and ROOT/'src/core/main.c' in files
summary = []
for name, values in configurations.items():
    flags = ['-D' + key + '=' + str(value) for key, value in {**common, **values}.items()]
    found = []
    for path in files:
        source = path.read_text()
        if native:
            command = compiler + ['-E', '-P', '-x', 'c'] + extra + flags + [str(path)]
            result = subprocess.run(command, cwd=ROOT, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        else:
            source = re.sub(r'^\s*#\s*include\b[^\n]*', '', source, flags=re.M)
            command = compiler + ['-E', '-P', '-x', 'c'] + extra + flags + ['-']
            result = subprocess.run(command, input=source, cwd=ROOT, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        assert result.returncode == 0, (name, path, result.stderr)
        clean = code_only(result.stdout)
        for declaration in re.finditer(r'\bX4Display\s+([A-Za-z_]\w*)\s*([^;]*);', clean):
            variable, initializer = declaration.groups()
            assert re.search(r'=\s*\{[^}]*\.handle\s*=\s*-\s*1\b', initializer), \
                (name, str(path), variable, 'active X4Display lacks .handle=-1')
            found.append(str(path.relative_to(ROOT)) + ':' + variable)
    assert found, (name, 'no active X4Display declaration was examined')
    summary.append((name, found))
for name, found in summary:
    print('PASS preprocessed', name, 'SP1 active display initializers:', ', '.join(found))
print('PASS actual preprocessor branch selection, noreturn declaration and no return in T26 entry; mode=' +
      ('native-includes' if native else 'host-includes-elided'))
