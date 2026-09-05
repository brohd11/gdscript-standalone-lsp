#!/usr/bin/env python3
"""Compare isolated diagnostics with a matching headless Godot parser.

Godot can return status zero despite script errors. Script diagnostics, process
failures and startup failures are therefore classified separately. Each warning
is tested by promoting only its category to Error, avoiding debugger mode.
"""
import json
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tests'))
from diagnostic_cases import API, assert_diagnostics, prepare, query


def check_engine(engine, directory, script, pattern):
    result = subprocess.run([engine, '--headless', '--no-header', '--log-file', str(directory / 'oracle.log'),
                             '--path', str(directory), '--check-only', '--script', 'res://' + script.relative_to(directory).as_posix()],
                            text=True, capture_output=True, timeout=20)
    output = result.stdout + result.stderr
    if result.returncode < 0 or 'Program crashed' in output:
        raise AssertionError('Godot crashed:\n' + output)
    script_error = re.search(r'SCRIPT ERROR:', output)
    if not script_error and (result.returncode != 0 or re.search(r'(?m)^ERROR:', output)):
        raise AssertionError('Godot startup/infrastructure failure:\n' + output)
    if pattern is None:
        assert not script_error, 'Godot rejected valid source:\n' + output
    else:
        assert script_error and re.search(pattern, output, re.I), 'Godot diagnostic did not match ' + repr(pattern) + ':\n' + output


def main():
    binary = pathlib.Path(sys.argv[1]).resolve()
    engine = sys.argv[2] if len(sys.argv) > 2 else 'godot'
    version = subprocess.run([engine, '--version'], capture_output=True, text=True, check=True, timeout=10).stdout.strip()
    golden = json.loads((ROOT / 'tests/diagnostic_oracle.json').read_text())
    assert version.startswith(golden['godot_version_prefix']), 'Oracle requires ' + golden['godot_version_prefix'] + 'x; found ' + version
    count = 0
    failures = []
    with tempfile.TemporaryDirectory(prefix='gdscript-oracle-') as temporary:
        base = pathlib.Path(temporary)
        legacy = base / 'legacy'
        shutil.copytree(ROOT / 'tests/fixtures/diagnostics', legacy, ignore=shutil.ignore_patterns('.godot'))
        with (legacy / 'project.godot').open('a') as stream:
            stream.write('\n[debug]\ngdscript/warnings/enable=false\n')
        for case in golden['cases']:
            script = legacy / case['script']
            check_engine(engine, legacy, script, case['godot_pattern'])
            diagnostics = query(binary, legacy, script, api=ROOT / 'tests/fixtures/basic/extension_api.json')
            codes = {d['code'] for d in diagnostics}
            if case['lsp_code'] is None:
                assert not diagnostics, case['script'] + ': unexpected diagnostics ' + repr(diagnostics)
            else:
                assert codes == {case['lsp_code']}, case['script'] + ': unexpected diagnostic codes ' + repr(codes)
            count += 1
        warning_names = re.findall(r'^([A-Z][A-Z_]+)$', (ROOT / 'tests/diagnostic_zoo/warning_codes.txt').read_text(), re.M)
        for case in json.loads((ROOT / 'tests/diagnostic_cases.json').read_text())['cases']:
            if 'godot_pattern' not in case:
                continue
            directory = base / case['name']
            directory.mkdir()
            script = prepare(directory, case)
            # Test the ordinary LSP severity and exact set before engine-specific configuration.
            assert_diagnostics(case, query(binary, directory, script))
            settings = directory / 'project.godot'
            if case.get('godot_warning'):
                extra = '\n'.join('gdscript/warnings/' + name.lower() + '=0' for name in warning_names)
                settings.write_text('[application]\nconfig/name="Warning oracle"\n[debug]\ngdscript/warnings/enable=true\n' + extra + '\ngdscript/warnings/' + case['godot_warning'] + '=2\n')
            try:
                check_engine(engine, directory, script, case['godot_pattern'])
            except (AssertionError, subprocess.SubprocessError) as error:
                failures.append(case['name'] + ': ' + str(error))
            count += 1
    assert not failures, '\n\n'.join(failures)
    print(f'Diagnostic oracle passed: {count} cases against Godot {version}')


if __name__ == '__main__':
    main()
