"""Run with python3 -m unittest discover -s tests from the Blutter directory."""

import argparse
import os
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import blutter
from dartvm_fetch_build import DartLibInfo


class MetadataCliTests(unittest.TestCase):
    selectors = ['MyHomePage', 'package:app/my page.dart::_State']

    def make_input(self, root, vs=False, selectors=None):
        value = blutter.BlutterInput('app path/libapp.so',
                                    DartLibInfo('3.11.4', 'android', 'arm64'),
                                    str(root), False, vs, False, selectors)
        value.blutter_file = str(root / 'blutter')
        Path(value.blutter_file).touch()
        return value

    def test_native_arguments_and_default(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for selectors in (None, self.selectors):
                with self.subTest(selectors=selectors):
                    value = self.make_input(root, selectors=selectors)
                    with patch.object(blutter.subprocess, 'run') as run:
                        blutter.build_and_run(value)
                    expected = [value.blutter_file, '-i', value.libapp_path,
                                '-o', value.outdir]
                    for name in selectors or []:
                        expected += ['--metadata-class', name]
                    run.assert_called_once_with(expected, check=True)

    def test_visual_studio_arguments_preserve_spaces(self):
        with tempfile.TemporaryDirectory() as directory:
            value = self.make_input(Path(directory), vs=True, selectors=self.selectors)
            with patch.dict(os.environ, {'VSCMD_VER': '17.0'}), \
                    patch.object(blutter, 'find_compat_macro', return_value=[]), \
                    patch.object(blutter.subprocess, 'run') as run:
                blutter.build_and_run(value)
            command = run.call_args.args[0]
            debug_args = next(arg for arg in command if arg.startswith('-DDBG_CMD:STRING='))
            expected = ['-i', value.libapp_path, '-o',
                        os.path.abspath(os.path.join(value.outdir, 'out'))]
            for name in self.selectors:
                expected += ['--metadata-class', name]
            self.assertEqual(debug_args, '-DDBG_CMD:STRING=' + subprocess.list2cmdline(expected))

    def test_directory_apk_and_explicit_version_routes(self):
        flags = (False, False, False, self.selectors)
        with patch.object(blutter, 'find_lib_files', return_value=('app', 'flutter')), \
                patch.object(blutter, 'main2') as main2:
            blutter.main('libs', 'out', *flags)
            main2.assert_called_once_with('app', 'flutter', 'out', *flags)
        with patch.object(blutter, 'extract_libs_from_apk', return_value=('app', 'flutter')), \
                patch.object(blutter, 'main2') as main2:
            blutter.main('app.apk', 'out', *flags)
            main2.assert_called_once_with('app', 'flutter', 'out', *flags)
        with patch.object(blutter, 'get_dart_lib_info',
                          return_value=DartLibInfo('3.11.4', 'android', 'arm64')), \
                patch.object(blutter, 'build_and_run') as build:
            blutter.main2('app', 'flutter', 'out', *flags)
            self.assertEqual(build.call_args.args[0].metadata_classes, self.selectors)
            blutter.main_no_flutter('app', '3.11.4_android_arm64', 'out', *flags)
            self.assertEqual(build.call_args.args[0].metadata_classes, self.selectors)

    def test_cli_parses_repeated_selectors(self):
        class Parsed(Exception):
            pass

        parse_args = argparse.ArgumentParser.parse_args

        def check_args(parser):
            args = parse_args(parser)
            self.assertEqual(args.metadata_class, self.selectors)
            raise Parsed

        for version_args in ([], ['--dart-version', '3.11.4_android_arm64']):
            with self.subTest(version_args=version_args):
                argv = ['blutter.py', 'app', 'out'] + version_args
                for name in self.selectors:
                    argv += ['--metadata-class', name]
                with patch.object(sys, 'argv', argv), \
                        patch.object(argparse.ArgumentParser, 'parse_args', check_args), \
                        self.assertRaises(Parsed):
                    runpy.run_path(blutter.__file__, run_name='__main__')


if __name__ == '__main__':
    unittest.main()
