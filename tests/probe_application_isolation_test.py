#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Source/path bounds checks; compiler/object proof is an additional VM step."""
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
PROBE_ENTRY = re.compile(
    r'\bint\s+main\s*\(\s*void\s*\)\s*\{\s*'
    r'#if X4_VIDEO_PROBE\s+/\*.*?\*/\s*'
    r'#if X4_PROBE_T27\s+return x4_t27_controller_run\(\);\s*'
    r'#else\s+return x4_probe_controller_run\(\);\s*#endif\s*#else', re.S)

class IsolationTests(unittest.TestCase):
    def test_reports_are_build_specific_and_bounded(self):
        source = (ROOT/'src/core/probe_controller.c').read_text(encoding='utf-8')
        self.assertIn('char path[256]', source)
        backend=(ROOT/'src/video/probe_files.c').read_text(encoding='utf-8')
        self.assertIn('"%s/xcloud4-probe-%.12s%s-%s-%llu.json"', backend)
        self.assertIn('"%s-%s-case"', source)
        self.assertIn('"%s-%s-metrics"', source)
        self.assertEqual(source.count('x4_probe_files_report_path('),5)
        self.assertNotIn('/data/xcloud4-probe-reports', source)
        # Bounded short source and unchanged six-flag build suffix. Full source
        # identity stays inside JSON and logs, not in the long report filename.
        build = 'f'*12 + '-n1-g45-r0-vc1-p1-im'
        for mode in ('paced','maximum-speed'):
            for suffix in ('case','metrics'):
                path=f'/data/xcloud4-probe-{build}-'+'a'*50+f'-{mode}-{suffix}-{2**64-1}.json'
                self.assertLess(len(path.encode())+1,256)
        path=f'/data/xcloud4-probe-{build}-files-{2**64-1}.json'
        self.assertLess(len(path.encode())+1,256)
        path=f'/data/xcloud4-probe-{build}-stacks-{2**64-1}.json'
        self.assertLess(len(path.encode())+1,256)
        path=f'/data/xcloud4-probe-{build}-memory-{2**64-1}.json'
        self.assertLess(len(path.encode())+1,256)

    def test_probe_close_does_not_export_normal_trace(self):
        source=(ROOT/'src/media/live_media.c').read_text(encoding='utf-8')
        block=source[source.index('int x4_live_media_close('):]
        start=block.index('if (m->local_probe) {')
        end=block.index('free(m->au); free(m); return 0;',start)
        self.assertNotIn('x4_trace_dump_file',block[start:end])
        self.assertNotIn('x4_trace_monitor_dump',block[start:end])
        self.assertGreater(block.index('"/data/xcloud4-trace-0731-'),end)

    def test_files_precede_producer_and_openat_is_only_informational(self):
        source=(ROOT/'src/core/probe_controller.c').read_text(encoding='utf-8')
        start=source.index('if (!exit_requested && !moved && (pad.pressed & ORBIS_PAD_BUTTON_CROSS))')
        branch=source[start:]
        self.assertLess(branch.index('if (report_dir() < 0)'),branch.index('x4_live_media_probe_create'))
        backend=(ROOT/'src/video/probe_files.c').read_text(encoding='utf-8')
        header=(ROOT/'src/video/probe_files.h').read_text(encoding='utf-8')
        self.assertIn('#define X4_PROBE_REPORT_ROOT "/data"',header)
        self.assertIn('O_WRONLY | O_CREAT | O_EXCL',backend)
        self.assertNotRegex(backend,r'\b(?:lstat|fstatat|fchmod|sceKernelFchmod)\s*\(')
        # One native openat is retained to measure whether it is blocked. None
        # of file creation, readback, preparation or clip reading depends on it.
        self.assertEqual(len(re.findall(r'\bopenat\s*\(',backend)),1)
        self.assertIn('X4_PROBE_FILES_OPENAT_INFO',backend)

        for name in ('x4_probe_files_prepare','x4_probe_files_report_create',
                     'x4_probe_files_report_open','x4_probe_files_sample_open'):
            match=re.search(r'\b'+name+r'\s*\([^;]*?\)\s*\{',backend,re.S)
            self.assertIsNotNone(match,name)
            depth=1; cursor=match.end()
            while depth and cursor<len(backend):
                depth += (backend[cursor]=='{')-(backend[cursor]=='}')
                cursor += 1
            self.assertNotRegex(backend[match.end():cursor],r'\bopenat\s*\(',name)
        self.assertNotIn('/data/xcloud4-probe-reports',backend)
        self.assertIn('probe-posix-audit: build/xcloud4.elf', (ROOT/'Makefile').read_text(encoding='utf-8'))
        self.assertLess(branch.index('if (!files_checked)'),branch.index('x4_live_media_probe_create'))
        self.assertIn('bool moved = files_view || memory_view',source)
        action=source[source.index('if (!exit_requested && !retained && (pad.pressed & ORBIS_PAD_BUTTON_SQUARE))'):source.index('if (files_view && (pad.pressed & ORBIS_PAD_BUTTON_CIRCLE))')]
        self.assertIn('x4_probe_files_selfcheck',action)
        self.assertNotIn('x4_live_media_probe_create',action)
        self.assertLess(source.index('int case_rc = case_report'), source.index('compatibility_passed[active_memory_choice * X4_PROBE_CASE_COUNT + active_index] = true'))
        self.assertIn('if (compatible && report == 0 && case_rc == 0)',source)

    def test_stack_info_follows_unchanged_file_check_and_is_exported_separately(self):
        source=(ROOT/'src/core/probe_controller.c').read_text(encoding='utf-8')
        action=source[source.index('if (!exit_requested && !retained && (pad.pressed & ORBIS_PAD_BUTTON_SQUARE))'):source.index('if (files_view && (pad.pressed & ORBIS_PAD_BUTTON_CIRCLE))')]
        self.assertLess(action.index('x4_probe_files_selfcheck'), action.index('x4_probe_stack_check'))
        self.assertIn('if (files_checked)',action)
        self.assertIn('stack_check_report(&stack_check, now)',action)
        self.assertIn('files_checked = stack_check.result == 0 && stack_check.report_result == 0',action)
        self.assertIn('x4_probe_metrics_stacks_json(f, metrics)',source)
        self.assertIn('"stacks", now',source)
        self.assertIn('15. INFO pila principal',source)
        self.assertIn('16. INFO hilo de prueba',source)

    def test_diagnostic_is_bounded_and_records_immediate_returns(self):
        header=(ROOT/'src/video/probe_files.h').read_text(encoding='utf-8')
        self.assertIn('raw_return',header)
        self.assertIn('call_errno',header)
        self.assertIn('calls_omitted',header)
        self.assertIn('#define X4_PROBE_FILE_CALL_CAPACITY 64',header)
        self.assertIn('calls[X4_PROBE_FILE_CALL_CAPACITY]',header)
        backend=(ROOT/'src/video/probe_files.c').read_text(encoding='utf-8')
        self.assertIn('errno = 0',backend)
        self.assertIn('X4_PROBE_FILES_OPENAT_INFO',backend)

    def test_explicit_probe_entry_and_no_connection_module(self):
        source=(ROOT/'src/core/main.c').read_text(encoding='utf-8')
        # Both banks return before any normal auth/catalog/network allocation.
        # T27 must remain nested inside the same OFF guard as the older probe.
        entry=PROBE_ENTRY.search(source)
        self.assertIsNotNone(entry,'Both probe entries must be inside X4_VIDEO_PROBE at the start of main')
        self.assertRegex(source,r'#if X4_VIDEO_PROBE && X4_PROBE_T27\s+'
                         r'#include "\.\./video/probe_t27_controller\.h"\s*#endif')
        self.assertEqual(source.count('return x4_t27_controller_run();'),1)
        self.assertEqual(source.count('return x4_probe_controller_run();'),1)
        self.assertNotRegex(source[entry.end():],
                            r'\bx4_(?:t27_controller_run|probe_controller_run)\s*\(')
        make=(ROOT/'Makefile').read_text(encoding='utf-8')
        self.assertIn('X4_VIDEO_PROBE ?= 0',make)
        self.assertIn('X4_PROBE_T27 ?= 0',make)
        self.assertRegex(make,r'ifeq \(\$\(X4_PROBE_T27\),1\)\s+'
                         r'ifneq \(\$\(X4_VIDEO_PROBE\)_\$\(X4_PROBE_T26\),1_1\)\s+'
                         r'\$\(error T27 requires X4_VIDEO_PROBE=1 and X4_PROBE_T26=1\)')
        self.assertIn('X4_VIDEO_CAPACITY_1080 ?= 0',make)
        self.assertFalse((ROOT/'src/network/connection_test.c').exists())
        self.assertNotIn('connection_test',source)

    def test_probe_entry_rejects_missing_off_guard_or_early_allocation(self):
        source=(ROOT/'src/core/main.c').read_text(encoding='utf-8')
        entry=PROBE_ENTRY.search(source)
        self.assertIsNotNone(entry)
        block=entry.group(0)
        # These controls ensure the updated expectation cannot accept either
        # a T27 call in an OFF build or work before the probe-only return.
        for name,mutated in (
            ('missing_outer_off_guard',block.replace('#if X4_VIDEO_PROBE','#if X4_PROBE_T27',1)),
            ('allocation_before_entry',block.replace('#if X4_VIDEO_PROBE',
                'x4_auth_create();\n#if X4_VIDEO_PROBE',1)),
            ('t27_falls_through',block.replace('return x4_t27_controller_run();',
                'x4_t27_controller_run();',1)),
        ):
            with self.subTest(name=name):
                self.assertIsNone(PROBE_ENTRY.search(mutated))

if __name__=='__main__': unittest.main(verbosity=2)
