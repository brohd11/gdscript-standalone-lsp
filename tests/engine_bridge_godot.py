#!/usr/bin/env python3
"""Compare direct Godot LSP diagnostics with attach and managed bridge modes."""
import json
import pathlib
import socket
import subprocess
import sys
import tempfile
import time

from diagnostic_cases import packet
from engine_bridge import Client, project, read_packet


def connect(port, process):
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        assert process.poll() is None, 'Godot exited before its LSP was ready'
        try:
            return socket.create_connection(('127.0.0.1', port), timeout=1)
        except OSError:
            time.sleep(.1)
    raise AssertionError('Godot LSP startup timed out')


class DirectGodot:
    def __init__(self, connection, root):
        self.connection = connection
        connection.settimeout(10)
        self.stream = connection.makefile('rb')
        self.uri = (root / 'main.gd').as_uri()
        self.version = 0
        self.identifier = 1
        self.send('initialize', {'rootUri': root.as_uri(), 'rootPath': str(root), 'capabilities': {}}, 1)
        while read_packet(self.stream).get('id') != 1:
            pass
        self.send('initialized', {})
        while read_packet(self.stream).get('method') != 'gdscript/capabilities':
            pass

    def send(self, method, params, identifier=None):
        message = {'jsonrpc': '2.0', 'method': method, 'params': params}
        if identifier is not None:
            message['id'] = identifier
        self.connection.sendall(packet(message))

    def diagnostics(self, source):
        self.version += 1
        document = {'uri': self.uri, 'version': self.version}
        if self.version == 1:
            document.update(languageId='gdscript', text=source)
            self.send('textDocument/didOpen', {'textDocument': document})
        else:
            self.send('textDocument/didChange', {'textDocument': document, 'contentChanges': [{'text': source}]})
        self.identifier += 1
        self.send('textDocument/documentSymbol', {'textDocument': {'uri': self.uri}}, self.identifier)
        items = None
        while True:
            message = read_packet(self.stream)
            if message.get('method') == 'textDocument/publishDiagnostics' and message['params']['uri'] == self.uri:
                items = message['params']['diagnostics']
            if message.get('id') == self.identifier:
                assert items is not None
                return items


def stop_process(process):
    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def main():
    binary = pathlib.Path(sys.argv[1]).resolve()
    godot = str(pathlib.Path(sys.argv[2]).resolve())
    with tempfile.TemporaryDirectory(prefix='engine-bridge-godot-') as temporary:
        root = pathlib.Path(temporary).resolve()
        project(root)
        with socket.socket() as reserve:
            reserve.bind(('127.0.0.1', 0))
            port = reserve.getsockname()[1]
        with (root / 'godot.log').open('w') as log:
            process = subprocess.Popen([godot, '--headless', '--editor', '--path', str(root), '--lsp-port', str(port)], stdout=log, stderr=log)
            try:
                connection = connect(port, process)
                direct = DirectGodot(connection, root)
                client = Client(binary, root, arguments=('--godot-lsp-port', str(port)))
                try:
                    client.state('ready')
                    sources = [
                        'extends Node\nfunc test():\n    var unused = 1\n',
                        'extends Node\nfunc test():\n    print(1 / 2)\n',
                        'extends Node\nfunc test():\n    var values: Dictionary[String, int] = {"key": "bad"}\n    print(values)\n',
                        'extends Node\n@unknown_annotation\nvar value = 1\n',
                        'extends Node\nvar values: Array[Array[int]]\n',
                        'extends Node\nfunc test():\n    var value: int = "🙂"\n',
                        'extends Node\nfunc test():\n    @warning_ignore("unused_variable")\n    var unused = 1\n',
                        'extends Node\nfunc test() -> void:\n    pass\n',
                    ]
                    count = 0
                    for version, source in enumerate(sources, 1):
                        expected = direct.diagnostics(source)
                        if version == 1:
                            client.open(source, version)
                        else:
                            client.change(source, version)
                        actual = client.diagnostics()
                        assert actual == expected, (source, expected, actual)
                        count += 1
                    # Direct Godot and the bridge must share the engine's treatment
                    # of an unsaved dependency, even when it differs from disk.
                    dependency = root / 'dependency.gd'
                    dependency.write_text('extends RefCounted\nfunc value() -> int:\n    return 1\n')
                    dependency_uri = dependency.as_uri()
                    overlay = 'extends RefCounted\nfunc value() -> String:\n    return "changed"\n'
                    direct.send('textDocument/didOpen', {'textDocument': {'uri': dependency_uri, 'languageId': 'gdscript', 'version': 1, 'text': overlay}})
                    client.open(overlay, uri=dependency_uri)
                    source = 'extends Node\nfunc test() -> int:\n    return preload("res://dependency.gd").new().value()\n'
                    expected = direct.diagnostics(source)
                    client.change(source, 9)
                    assert client.diagnostics() == expected
                    print('Unsaved dependency characterization:', json.dumps(expected))
                finally:
                    client.close()
                    connection.close()
                assert process.poll() is None, 'Attaching bridge terminated the editor'
            finally:
                stop_process(process)
        # Managed launch, project settings restart, and live overlay replay.
        client = Client(binary, root, arguments=('--godot', godot))
        try:
            state = client.state('ready', timeout=70)
            assert state['engineVersion'], state
            source = 'extends Node\nfunc test():\n    var unused = 1\n'
            client.open(source)
            actual = client.diagnostics()
            assert len(actual) == 1 and actual[0]['severity'] == 2 and isinstance(actual[0]['code'], int), actual
            (root / 'project.godot').write_text('[application]\nconfig/name="Engine bridge tests"\n[debug]\ngdscript/warnings/unused_variable=2\n')
            client.send('workspace/didChangeWatchedFiles', {'changes': [{'uri': (root / 'project.godot').as_uri(), 'type': 2}]})
            client.state('connecting')
            client.state('ready', timeout=70)
            actual = client.diagnostics()
            assert any(item['severity'] == 1 and isinstance(item['code'], int) for item in actual), actual
            assert (root / 'main.gd').read_text() == 'extends Node\n', 'Bridge saved an unsaved buffer'
        finally:
            client.close()
    print(f'Engine bridge Godot: {count} exact diagnostic comparisons, dependency behavior, managed launch/restart passed')


if __name__ == '__main__':
    main()
