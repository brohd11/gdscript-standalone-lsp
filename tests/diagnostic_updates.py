#!/usr/bin/env python3
"""Verify warning settings, source overlays, and push/pull consistency."""
import json
import pathlib
import queue
import subprocess
import sys
import tempfile
import threading
import time

from diagnostic_cases import API, packet


def read(stream):
    length = None
    while True:
        line = stream.readline()
        if not line:
            return None
        if line == b'\r\n':
            return json.loads(stream.read(length))
        if line.lower().startswith(b'content-length:'):
            length = int(line.split(b':', 1)[1])


def main():
    with tempfile.TemporaryDirectory(prefix='gdscript-diagnostic-updates-') as temporary:
        directory = pathlib.Path(temporary).resolve()
        project = directory / 'project.godot'
        project.write_text('[application]\nconfig/name="Diagnostic updates"\n')
        script = directory / 'main.gd'
        clean = 'extends Node\nfunc test() -> void:\n    pass\n'
        unused = 'extends Node\nfunc test() -> void:\n    var unused = 1\n'
        script.write_text(clean)
        uri = script.as_uri()
        process = subprocess.Popen([str(pathlib.Path(sys.argv[1]).resolve()), '--api', str(API)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        messages = queue.Queue()
        def reader():
            while True:
                message = read(process.stdout)
                messages.put(message)
                if message is None:
                    return
        threading.Thread(target=reader, daemon=True).start()
        def send(method, params, request_id=None):
            value = {'jsonrpc': '2.0', 'method': method, 'params': params}
            if request_id is not None:
                value['id'] = request_id
            process.stdin.write(packet(value))
            process.stdin.flush()
        def take(predicate):
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                message = messages.get(timeout=max(0.01, deadline-time.monotonic()))
                assert message is not None, 'Server exited unexpectedly'
                if predicate(message):
                    return message
            raise AssertionError('Timed out waiting for LSP message')
        try:
            send('initialize', {'rootUri': directory.as_uri(), 'initializationOptions': {'gdscriptLsp': {'diagnostics': {'pollIntervalMs': 0}}}}, 1)
            take(lambda m: m.get('id') == 1)
            send('initialized', {})
            send('textDocument/didOpen', {'textDocument': {'uri': uri, 'languageId': 'gdscript', 'version': 1, 'text': unused}})
            request_id = 10
            def verify(codes, severity=2):
                nonlocal request_id
                pushed = take(lambda m: m.get('method') == 'textDocument/publishDiagnostics' and m['params']['uri'] == uri)['params']['diagnostics']
                assert [d['code'] for d in pushed] == codes, pushed
                assert all(d['severity'] == severity for d in pushed), pushed
                request_id += 1
                send('textDocument/diagnostic', {'textDocument': {'uri': uri}}, request_id)
                pulled = take(lambda m: m.get('id') == request_id)['result']['items']
                assert pushed == pulled, (pushed, pulled)
            verify(['unused-variable'])
            project.write_text('[application]\nconfig/name="Diagnostic updates"\n[debug]\ngdscript/warnings/unused_variable=2\n')
            send('workspace/didChangeWatchedFiles', {'changes': [{'uri': project.as_uri(), 'type': 2}]})
            verify(['unused-variable'], 1)
            ignored = unused.replace('    var', '    @warning_ignore("unused_variable")\n    var')
            send('textDocument/didChange', {'textDocument': {'uri': uri, 'version': 2}, 'contentChanges': [{'text': ignored}]})
            verify([])
            send('textDocument/didChange', {'textDocument': {'uri': uri, 'version': 3}, 'contentChanges': [{'text': unused}]})
            verify(['unused-variable'], 1)
            project.write_text('[application]\nconfig/name="Diagnostic updates"\n[debug]\ngdscript/warnings/enable=false\n')
            send('workspace/didChangeWatchedFiles', {'changes': [{'uri': project.as_uri(), 'type': 2}]})
            verify([])
            project.write_text('[application]\nconfig/name="Diagnostic updates"\n')
            send('workspace/didChangeWatchedFiles', {'changes': [{'uri': project.as_uri(), 'type': 2}]})
            verify(['unused-variable'])
            send('textDocument/didClose', {'textDocument': {'uri': uri}})
            verify([])
            unicode_source = 'extends Node\nvar 位置: int = 1\nfunc café() -> int:\n    return 位置\n'
            send('textDocument/didOpen', {'textDocument': {'uri': uri, 'languageId': 'gdscript', 'version': 4, 'text': unicode_source}})
            send('textDocument/definition', {'textDocument': {'uri': uri}, 'position': {'line': 3, 'character': 12}}, 50)
            definitions = take(lambda m: m.get('id') == 50)['result']
            assert definitions and definitions[0]['range']['start'] == {'line': 1, 'character': 4}, definitions
            send('textDocument/documentSymbol', {'textDocument': {'uri': uri}}, 51)
            symbols = take(lambda m: m.get('id') == 51)['result']
            def names(nodes):
                return [node['name'] for node in nodes] + [name for node in nodes for name in names(node.get('children', []))]
            assert {'位置', 'café'} <= set(names(symbols)), symbols
            send('textDocument/completion', {'textDocument': {'uri': uri}, 'position': {'line': 3, 'character': 12}}, 52)
            completion = take(lambda m: m.get('id') == 52)['result']
            items = completion['items'] if isinstance(completion, dict) else completion
            assert any(item.get('filterText', item['label']) == '位置' for item in items), items
            send('shutdown', {}, 99)
            take(lambda m: m.get('id') == 99)
            send('exit', {})
            assert process.wait(timeout=5) == 0
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
    print('Diagnostic updates: overlays, suppression, configuration, and push/pull passed')


if __name__ == '__main__':
    main()
