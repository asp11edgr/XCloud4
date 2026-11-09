#!/usr/bin/env python3
"""Inventory real stage-producing expressions, then require English coverage.

This is a C-token scan (comments ignored), not a grep of a hand-written case
list. It covers stage assignments, the auth/session publishing helpers, HTTP
CREATE/OPTION helpers, stage copy/format calls, and the two stage tables.
"""
import ast
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
LEX = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[A-Za-z_][A-Za-z_0-9]*|[^\s]', re.S)
STAGE_ARGS = {'set':4, 'publish':4, 'reset_catalog':3, 'reset_recent':3,
              'reset_session':3, 'step':1, 'finish':4, 'recent_finish':4,
              'session_end':4, 'end_with':4, 'session_state':2,
              'CREATE':0, 'OPTION':0, 'PUBLIC_CREATE':0, 'PUBLIC_OPTION':0,
              'connection_stage':1, 'publish_stage':1}
NEUTRAL = {'', 'NetInit', 'SslInit', 'HttpInit', 'HTTPS: %s', '%s (%s)', '%s: %s'}


def tokenize(text):
    return [(m.group(), m.start()) for m in LEX.finditer(text)
            if not m.group().startswith(('/*', '//'))]


def arguments(tokens, start):
    """Split a call, retaining nested expressions and ternary string arms."""
    if start >= len(tokens) or tokens[start][0] != '(':
        return []
    args, current, depth = [], [], 0
    for token in tokens[start + 1:]:
        v = token[0]
        if v == ')' and depth == 0:
            return args + [current]
        if v == ',' and depth == 0:
            args.append(current); current = []; continue
        if v in ('(', '[', '{'): depth += 1
        if v in (')', ']', '}'): depth -= 1
        current.append(token)
    return []


def strings(tokens):
    return [ast.literal_eval(t[0]) for t in tokens if t[0].startswith('"')]


def inventory():
    found = {}
    paths = list((ROOT/'src/auth').glob('*.c')) + [ROOT/'src/core/main.c']
    for path in paths:
        text = path.read_text(encoding='utf-8')
        ts = tokenize(text)

        def remember(items):
            for value in strings(items):
                if value in NEUTRAL or value.startswith(('/', 'https://')):
                    continue
                # Protocol keys in conditional predicates are not visible
                # stage captions. Single-word Spanish stages are included.
                if value in {'ReadyToConnect','Provisioned','Provisioning',
                             'WaitingForResources','Failed','Closed'}:
                    continue
                found.setdefault(value, []).append(path.relative_to(ROOT).as_posix())

        for i, (name, _) in enumerate(ts):
            args = arguments(ts, i+1)
            if name in STAGE_ARGS and len(args) > STAGE_ARGS[name]:
                remember(args[STAGE_ARGS[name]])
            if name in ('copy_text', 'snprintf') and len(args) >= 3 and any(
                    'stage' in tok[0] for tok in args[0]):
                remember(args[2])
            if name == 'stage' and i+1 < len(ts) and ts[i+1][0] == '=':
                end = i+2
                while end < len(ts) and ts[end][0] != ';': end += 1
                remember(ts[i+2:end])
        # Code->caption and offering->caption initializers have no setter.
        for table in ('passport_errors', 'offerings'):
            match = re.search(r'\b'+table+r'\s*\[\s*\]\s*=\s*\{(.*?)\n\};', text, re.S)
            if match:
                for row in re.finditer(r'\{([^{}]*)\}', match.group(1)):
                    values = strings(tokenize(row.group(1)))
                    if values: remember([(json.dumps(values[-1], ensure_ascii=False),0)])
        if path.name == 'device_auth.c':
            match = re.search(r'static const char \*rejection\([^)]*\)\s*\{(.*?)\n\}', text, re.S)
            if match:
                for returned in re.finditer(r'\breturn\s+("(?:\\.|[^"\\])*")', match.group(1)):
                    remember(tokenize(returned.group(1)))
        if path.name == 'http_client.c':
            match = re.search(r'const char \*x4_http_stage\([^)]*\)\s*\{(.*?)\n\}', text, re.S)
            if match: remember(tokenize(match.group(1)))
    return found


def main():
    found = inventory()
    source = (ROOT/'src/ui/lang.c').read_text(encoding='utf-8')
    table = source[source.index('} stage_map[] = {'):]
    pairs = dict((ast.literal_eval(a),ast.literal_eval(b)) for a,b in
                 re.findall(r'\{\s*("(?:\\.|[^"\\])*")\s*,\s*("(?:\\.|[^"\\])*")\s*,',table.split("\n};", 1)[0]))
    # These are format families, translated at display time with their suffix
    # retained. C runtime tests exercise the populated form and HTTPS nesting.
    dynamic = {'Xbox está preparando el juego… %u s'}
    missing = sorted(set(found) - set(pairs) - dynamic)
    if '--list-missing' in sys.argv:
        print(json.dumps(missing,ensure_ascii=False,indent=2));return
    assert found and not missing, 'Stages missing English translations: '+repr(missing)
    assert all(en and es != en for es,en in pairs.items()), 'Empty/identity translation'
    screen = (ROOT/'src/ui/screen.c').read_text(encoding='utf-8')
    assert 'x4_tr_stage(a->stage)' in screen and 'x4_tr_stage(s->stage)' in screen
    print('PASS: English coverage for %d distinct real auth/HTTP/loading stage literals (%d source occurrences).' %
          (len(found),sum(map(len,found.values()))))


if __name__ == '__main__': main()
