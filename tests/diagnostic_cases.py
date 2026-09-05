#!/usr/bin/env python3
"""Isolated, exact-set diagnostics and LSP update regression tests.

Sources and expectations are data; no Godot executable is needed for this suite.
"""
import json
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
API = ROOT / 'addons/gdscript_lsp/data/godot-4.6-extension-api.json'


def packet(value):
    body = json.dumps(value).encode()
    return f'Content-Length: {len(body)}\r\n\r\n'.encode() + body


def packets(data):
    result = []
    while data:
        header, data = data.split(b'\r\n\r\n', 1)
        length = int(next(line.split(b':', 1)[1] for line in header.split(b'\r\n') if line.lower().startswith(b'content-length:')))
        result.append(json.loads(data[:length]))
        data = data[length:]
    return result


def project_settings(case):
    settings = '[application]\nconfig/name="Diagnostic case"\n[debug]\ngdscript/warnings/directory_rules={}\n'
    settings += 'gdscript/warnings/enable=' + ('true' if case.get('warnings') else 'false') + '\n'
    return settings + case.get('settings', '')


def prepare(directory, case):
    (directory / 'project.godot').write_text(project_settings(case))
    script = directory / case.get('path', 'main.gd')
    script.parent.mkdir(parents=True, exist_ok=True)
    script.write_text(case['source'])
    for name, source in case.get('files', {}).items():
        path = directory / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(source)
    return script


def query(binary, directory, script, source=None, api=API):
    directory = directory.resolve()
    script = script.resolve()
    uri = script.as_uri()
    requests = [{'jsonrpc': '2.0', 'id': 1, 'method': 'initialize', 'params': {'rootUri': directory.as_uri()}}]
    if source is not None:
        requests.append({'jsonrpc': '2.0', 'method': 'textDocument/didOpen', 'params': {'textDocument': {'uri': uri, 'languageId': 'gdscript', 'version': 1, 'text': source}}})
    requests.extend([
        {'jsonrpc': '2.0', 'id': 2, 'method': 'textDocument/diagnostic', 'params': {'textDocument': {'uri': uri}}},
        {'jsonrpc': '2.0', 'id': 3, 'method': 'shutdown', 'params': {}},
        {'jsonrpc': '2.0', 'method': 'exit', 'params': {}},
    ])
    result = subprocess.run([str(binary), '--api', str(api)], input=b''.join(map(packet, requests)), capture_output=True, timeout=20, check=True)
    return next(item['result']['items'] for item in packets(result.stdout) if item.get('id') == 2)


def assert_diagnostics(case, actual):
    expected = case.get('expected', [])
    if expected is None:
        codes = {d['code'] for d in actual}
        assert set(case['invariants'].get('require', [])) <= codes, (case['name'], actual)
        assert not set(case['invariants'].get('forbid', [])) & codes, (case['name'], actual)
        return
    def key(item):
        return (item['range']['start']['line'], item['range']['start']['character'], item['code'], item['severity'])
    want, got = sorted(expected, key=key), sorted(actual, key=key)
    assert len(want) == len(got), f"{case['name']}: expected {len(want)}, got {len(got)}\n{json.dumps(actual, indent=2)}"
    for a, b in zip(want, got):
        assert key(a) == key(b), f"{case['name']}: expected {a}, got {b}"
        if 'end' in a['range']:
            assert a['range']['end'] == b['range']['end'], f"{case['name']}: incorrect range: {b}"
        assert b['message'].strip(), f"{case['name']}: missing message"


def main():
    binary = pathlib.Path(sys.argv[1]).resolve()
    cases = json.loads((ROOT / 'tests/diagnostic_cases.json').read_text())['cases']
    failures = []
    for case in cases:
        if len(sys.argv) > 2 and sys.argv[2] not in case['name']:
            continue
        try:
            with tempfile.TemporaryDirectory(prefix='gdscript-diagnostics-') as temporary:
                directory = pathlib.Path(temporary)
                script = prepare(directory, case)
                api = ROOT / case['api'] if 'api' in case else API
                if case.get('operator_metadata') is False:
                    metadata = json.loads(API.read_text())
                    metadata['gdscript_lsp_schema'] = 2
                    for builtin in metadata['builtin_classes']:
                        builtin.pop('operators', None)
                    api = directory / 'extension_api.json'
                    api.write_text(json.dumps(metadata))
                assert_diagnostics(case, query(binary, directory, script, api=api))
                if case.get('overlay'):
                    script.write_text('extends Node\n')
                    assert_diagnostics(case, query(binary, directory, script, source=case['source'], api=api))
        except (AssertionError, subprocess.SubprocessError) as error:
            failures.append(str(error))
    for failure in failures:
        print(failure, file=sys.stderr)
    print(f'Diagnostic cases: {len(cases)} cases, {len(failures)} failures')
    return bool(failures)


if __name__ == '__main__':
    sys.exit(main())
