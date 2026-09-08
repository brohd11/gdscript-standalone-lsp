#!/usr/bin/env python3
"""Enforce the Godot 4.6.3 parser-error coverage matrix."""

import argparse
import json
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests'))

from diagnostics.cases import prepare, query
from diagnostics.oracle import check_engine


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('binary', type=pathlib.Path)
    parser.add_argument('--godot')
    args = parser.parse_args()
    binary = args.binary.resolve()
    matrix = json.loads((ROOT / 'tests/diagnostics/parse_errors.json').read_text(encoding='utf-8'))
    if args.godot:
        version = subprocess.run(
            [args.godot, '--version'], capture_output=True, text=True, check=True, timeout=10
        ).stdout.strip()
        expected = matrix['godot_version_prefix']
        assert version.startswith(expected), f'Coverage matrix requires {expected}x; found {version}'

    failures = []
    counts = {'error': 0, 'ok': 0, 'gap': 0}
    for case in matrix['cases']:
        counts[case['expect']] += 1
        try:
            with tempfile.TemporaryDirectory(prefix='gdscript-parse-error-') as temporary:
                directory = pathlib.Path(temporary)
                script = prepare(directory, case)
                diagnostics = query(binary, directory, script)
                errors = [item for item in diagnostics if item.get('severity') == 1]
                if case['expect'] == 'error':
                    assert errors, f"{case['name']}: expected an LSP error, got {diagnostics!r}"
                else:
                    assert not diagnostics, (
                        f"{case['name']}: expected LSP silence for {case['expect']}, got {diagnostics!r}"
                    )
                if args.godot:
                    pattern = re.escape(case['godot']) if case['godot'] is not None else None
                    check_engine(args.godot, directory, script, pattern)
        except (AssertionError, subprocess.SubprocessError) as error:
            failures.append(str(error))

    for failure in failures:
        print(failure, file=sys.stderr)
    print(
        f"Parse-error coverage: {len(matrix['cases'])} cases "
        f"({counts['error']} errors, {counts['gap']} known gaps, {counts['ok']} valid), "
        f"{len(failures)} failures"
    )
    return bool(failures)


if __name__ == '__main__':
    sys.exit(main())
