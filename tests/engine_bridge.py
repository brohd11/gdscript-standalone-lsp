#!/usr/bin/env python3
"""Engine bridge integration tests using a controllable upstream LSP."""
import json
import os
import pathlib
import queue
import socket
import subprocess
import sys
import tempfile
import threading
import time

from diagnostics.cases import API, packet

ROOT = pathlib.Path(__file__).resolve().parent.parent


def read_packet(stream):
    length = None
    while True:
        line = stream.readline()
        if not line:
            raise EOFError('LSP connection closed')
        if line == b'\r\n':
            return json.loads(stream.read(length))
        if line.lower().startswith(b'content-length:'):
            length = int(line.split(b':', 1)[1])


class Client:
    def __init__(self, binary, root, engine=None, arguments=()):
        self.process = subprocess.Popen([str(binary), '--api', str(API), *arguments], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.messages = queue.Queue()
        self.backlog = []
        self.history = []
        self.request_id = 100
        self.root = root
        self.uri = (root / 'main.gd').as_uri()
        def reader():
            try:
                while True:
                    self.messages.put(read_packet(self.process.stdout))
            except (EOFError, OSError) as error:
                self.messages.put(error)
        threading.Thread(target=reader, daemon=True).start()
        options = {'gdscriptLsp': {'diagnostics': {'pollIntervalMs': 0}}}
        if engine:
            options['gdscriptLsp']['diagnostics']['engine'] = engine
        self.request('initialize', {'rootUri': root.as_uri(), 'initializationOptions': options})
        self.send('initialized', {})

    def send(self, method, params, request_id=None):
        message = {'jsonrpc': '2.0', 'method': method, 'params': params}
        if request_id is not None:
            message['id'] = request_id
        self.process.stdin.write(packet(message))
        self.process.stdin.flush()

    def wait(self, predicate, timeout=12):
        for i, message in enumerate(self.backlog):
            if predicate(message):
                return self.backlog.pop(i)
        deadline = time.monotonic() + timeout
        while True:
            try:
                message = self.messages.get(timeout=max(.01, deadline - time.monotonic()))
            except queue.Empty:
                raise AssertionError('LSP response timeout; recent messages: ' + repr(self.history[-5:]))
            if isinstance(message, Exception):
                raise message
            self.history.append(message)
            if predicate(message):
                return message
            self.backlog.append(message)

    def request(self, method, params, timeout=12):
        self.request_id += 1
        self.send(method, params, self.request_id)
        response = self.wait(lambda m: m.get('id') == self.request_id, timeout)
        assert 'error' not in response, response
        return response['result']

    def state(self, state, timeout=12):
        return self.wait(lambda m: m.get('method') == 'gdscript/diagnosticBackendChanged'
                         and m['params']['state'] == state, timeout)['params']

    def open(self, source, version=1, uri=None):
        self.send('textDocument/didOpen', {'textDocument': {'uri': uri or self.uri, 'languageId': 'gdscript', 'version': version, 'text': source}})

    def change(self, source, version):
        self.send('textDocument/didChange', {'textDocument': {'uri': self.uri, 'version': version}, 'contentChanges': [{'text': source}]})

    def diagnostics(self):
        return self.request('textDocument/diagnostic', {'textDocument': {'uri': self.uri}})['items']

    def close(self):
        if self.process.poll() is None:
            try:
                self.request('shutdown', {})
                self.send('exit', {})
                assert self.process.wait(timeout=8) == 0
            finally:
                if self.process.poll() is None:
                    self.process.kill()
                    self.process.wait()


def engine_diagnostics(source):
    if '# engine' not in source:
        return []
    return [{'code': 42, 'severity': 2, 'source': 'gdscript', 'message': '(ENGINE_ONLY): ' + source.splitlines()[-1],
             'range': {'start': {'line': 1, 'character': 0}, 'end': {'line': 1, 'character': 2}},
             'tags': [1], 'data': {'opaque': ['engine', 42]},
             'codeDescription': {'href': 'https://example.invalid/engine-warning'}}]


class FakeEngine:
    def __init__(self, wrong_root=False, port=0):
        self.listener = socket.socket()
        self.listener.bind(('127.0.0.1', port))
        self.listener.listen()
        self.listener.settimeout(.1)
        self.port = self.listener.getsockname()[1]
        self.wrong_root = wrong_root
        self.stop = threading.Event()
        self.delayed = threading.Event()
        self.observed = []
        self.errors = []
        self.connections = []
        self.workers = []
        self.thread = threading.Thread(target=self.accept, daemon=True)
        self.thread.start()

    def accept(self):
        while not self.stop.is_set():
            try:
                connection, _ = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            self.connections.append(connection)
            worker = threading.Thread(target=self.serve, args=(connection,), daemon=True)
            self.workers.append(worker)
            worker.start()

    def serve(self, connection):
        def send(message):
            data = packet(message)
            # Every reply exercises fragmented headers and payloads.
            for start, end in ((0, 7), (7, 23), (23, len(data))):
                connection.sendall(data[start:end])
        documents = {}
        stalled = False
        try:
            stream = connection.makefile('rb')
            while not self.stop.is_set():
                message = read_packet(stream)
                self.observed.append(message)
                method = message['method']
                params = message['params']
                if method == 'initialize':
                    if self.wrong_root:
                        send({'jsonrpc': '2.0', 'method': 'gdscript_client/changeWorkspace', 'params': {'path': '/wrong-project'}})
                    send({'jsonrpc': '2.0', 'id': message['id'], 'result': {'capabilities': {'textDocumentSync': {'change': 1}, 'documentSymbolProvider': True}}})
                elif method == 'initialized':
                    send({'jsonrpc': '2.0', 'method': 'gdscript/capabilities', 'params': {'native_classes': []}})
                elif method in ('textDocument/didOpen', 'textDocument/didChange'):
                    document = params['textDocument']
                    uri = document['uri']
                    if method.endswith('didOpen'):
                        assert uri not in documents, 'Duplicate didOpen'
                        source = document['text']
                    else:
                        assert uri in documents, 'didChange before didOpen'
                        assert len(params['contentChanges']) == 1 and 'range' not in params['contentChanges'][0]
                        source = params['contentChanges'][0]['text']
                    documents[uri] = source
                    if '# delay' in source:
                        self.delayed.set()
                        time.sleep(.8)
                    if '# stall' in source:
                        stalled = True
                        self.delayed.set()
                        continue
                    if '# malformed' in source:
                        connection.sendall(b'Content-Length: -1\r\n\r\n')
                        continue
                    send({'jsonrpc': '2.0', 'method': 'textDocument/publishDiagnostics', 'params': {'uri': uri, 'diagnostics': engine_diagnostics(source)}})
                elif method == 'textDocument/documentSymbol' and not stalled:
                    send({'jsonrpc': '2.0', 'id': message['id'], 'result': []})
                elif method == 'textDocument/didClose':
                    assert params['textDocument']['uri'] in documents, 'didClose before didOpen'
                    del documents[params['textDocument']['uri']]
                elif method == 'textDocument/didSave':
                    assert params['text'] == documents[params['textDocument']['uri']]
        except (EOFError, ConnectionError, OSError):
            pass
        except Exception as error:
            self.errors.append(error)

    def close(self):
        self.stop.set()
        self.listener.close()
        for connection in self.connections:
            try:
                connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            connection.close()
        self.thread.join(2)
        for worker in self.workers:
            worker.join(2)
        assert not self.errors, self.errors


def project(root):
    (root / 'project.godot').write_text('[application]\nconfig/name="Engine bridge tests"\n')
    (root / 'main.gd').write_text('extends Node\n')


def test_attach(binary, root):
    engine = FakeEngine()
    client = Client(binary, root, {'mode': 'attach', 'port': engine.port})
    try:
        client.state('ready')
        source = 'extends Node\n# engine initial\n'
        client.open(source)
        expected = engine_diagnostics(source)
        assert client.diagnostics() == expected
        pushed = client.wait(lambda m: m.get('method') == 'textDocument/publishDiagnostics' and m['params'].get('version') == 1 and m['params']['diagnostics'] == expected)
        assert pushed['params']['diagnostics'] == client.diagnostics()
        disk = root / 'unopened.gd'
        disk_source = 'extends Node\n# engine unopened\n'
        disk.write_text(disk_source)
        client.send('workspace/didChangeWatchedFiles', {'changes': [{'uri': disk.as_uri(), 'type': 1}]})
        client.wait(lambda m: m.get('method') == 'textDocument/publishDiagnostics' and m['params']['uri'] == disk.as_uri()
                    and m['params']['diagnostics'] == engine_diagnostics(disk_source))
        disk.unlink()
        client.send('workspace/didChangeWatchedFiles', {'changes': [{'uri': disk.as_uri(), 'type': 3}]})
        client.wait(lambda m: m.get('method') == 'textDocument/publishDiagnostics' and m['params']['uri'] == disk.as_uri() and not m['params']['diagnostics'])
        client.change('extends Node\n# engine old # delay\n', 2)
        assert engine.delayed.wait(3), 'Update was not synchronized'
        started = time.monotonic()
        client.request('textDocument/completion', {'textDocument': {'uri': client.uri}, 'position': {'line': 1, 'character': 0}})
        assert time.monotonic() - started < .6, 'Engine blocked completion'
        current = 'extends Node\n# engine new\n'
        client.change(current, 3)
        assert client.diagnostics() == engine_diagnostics(current)
        assert not any(m.get('method') == 'textDocument/publishDiagnostics' and m['params'].get('version') == 2 for m in client.history), 'Published stale engine result'
        # Frontend ranged edits must become full-text upstream updates.
        client.send('textDocument/didChange', {'textDocument': {'uri': client.uri, 'version': 4}, 'contentChanges': [{'range': {'start': {'line': 1, 'character': 0}, 'end': {'line': 2, 'character': 0}}, 'text': '# clean\n'}]})
        assert client.diagnostics() == []
        client.send('textDocument/didSave', {'textDocument': {'uri': client.uri}})
        assert client.diagnostics() == []
        client.send('textDocument/didClose', {'textDocument': {'uri': client.uri}})
        assert client.diagnostics() == []
        source = 'extends Node\n# engine reopened café 位置\n'
        client.open(source, 5)
        assert client.diagnostics() == engine_diagnostics(source)
        # A second client owns its own upstream buffer state.
        other = Client(binary, root, {'mode': 'attach', 'port': engine.port})
        try:
            other.state('ready')
            other.open('extends Node\n# engine other\n')
            assert other.diagnostics() == engine_diagnostics('extends Node\n# engine other\n')
            assert client.diagnostics() == engine_diagnostics(source)
        finally:
            other.close()
        client.request('gdscript/reconnectDiagnosticEngine', {})
        client.state('ready')
        # Wait for the new engine authority rather than startup fallback.
        deadline = time.monotonic() + 5
        while client.diagnostics() != engine_diagnostics(source):
            assert time.monotonic() < deadline
            time.sleep(.05)
        project_uri = (root / 'project.godot').as_uri()
        client.send('workspace/didChangeWatchedFiles', {'changes': [{'uri': project_uri, 'type': 2}]})
        client.state('reload-required')
        fallback = client.diagnostics()
        assert fallback == [], fallback
        client.request('gdscript/reconnectDiagnosticEngine', {})
        client.state('ready')
        client.change('extends Node\n# stall\n', 6)
        client.send('textDocument/diagnostic', {'textDocument': {'uri': client.uri}}, 900)
        client.send('$/cancelRequest', {'id': 900})
        assert client.wait(lambda m: m.get('id') == 900)['error']['code'] == -32800
        client.state('fallback', timeout=8)
        assert client.diagnostics() == []
        # Remove the stalled text before replay, then check malformed framing.
        client.change('extends Node\n# malformed\n', 7)
        client.state('ready')
        client.state('fallback')
        client.send('workspace/didChangeConfiguration', {'settings': {'gdscriptLsp': {'diagnostics': {'engine': {'mode': 'off'}}}}})
        client.state('off')
        client.change('extends Node\nfunc test():\n    var unused = 1\n', 8)
        assert [d['code'] for d in client.diagnostics()] == ['unused-variable']
        assert any(m['method'] == 'textDocument/didSave' for m in engine.observed)
    finally:
        client.close()
        engine.close()


def test_failures(binary, root):
    engine = FakeEngine(wrong_root=True)
    client = Client(binary, root, {'mode': 'attach', 'port': engine.port})
    try:
        state = client.state('fallback')
        assert 'different project' in state['reason'], state
        assert not any(m['method'] == 'textDocument/didOpen' for m in engine.observed)
    finally:
        client.close()
        engine.close()
    client = Client(binary, root, {'mode': 'launch', 'executable': str(root / 'missing-godot')})
    try:
        client.state('fallback')
        assert client.diagnostics() == []
    finally:
        client.close()
    result = subprocess.run([str(binary), '--godot', 'godot', '--godot-lsp-port', '6005'], capture_output=True, timeout=5)
    assert result.returncode == 2


def test_managed_lifecycle(binary, root):
    if os.name == 'nt':
        return  # Windows process jobs are exercised by the real-engine suite.
    executable = root / 'fake godot'
    executable.write_text('#!' + sys.executable + '\n' + '''
import os, pathlib, subprocess, sys, time
sys.path.insert(0, ''' + repr(str(ROOT / 'tests')) + ''')
from engine_bridge import FakeEngine
if '--version' in sys.argv:
    print('4.6.test.fake')
elif '--help' in sys.argv:
    print('--headless --editor --lsp-port')
else:
    root = pathlib.Path(sys.argv[sys.argv.index('--path') + 1])
    engine = FakeEngine(port=int(sys.argv[sys.argv.index('--lsp-port') + 1]))
    with (root / 'engine-pids.txt').open('a') as stream:
        stream.write(str(os.getpid()) + '\\n')
    if (root / 'spawn-child').exists():
        child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(300)'])
        with (root / 'helper-pids.txt').open('a') as stream:
            stream.write(str(child.pid) + '\\n')
    while True:
        time.sleep(.1)
''')
    executable.chmod(0o755)

    def alive(pid):
        try:
            os.kill(pid, 0)
            return True
        except ProcessLookupError:
            return False

    def wait_dead(pid):
        deadline = time.monotonic() + 5
        while alive(pid):
            assert time.monotonic() < deadline, 'Orphaned engine process ' + str(pid)
            time.sleep(.05)

    for action in ('shutdown', 'terminate', 'crash'):
        if action == 'crash':
            (root / 'spawn-child').touch()
        client = Client(binary, root, arguments=('--godot', str(executable)))
        try:
            client.state('ready')
            pid = int((root / 'engine-pids.txt').read_text().splitlines()[-1])
            source = 'extends Node\n# engine managed\n'
            client.open(source)
            assert client.diagnostics() == engine_diagnostics(source)
            if action == 'terminate':
                client.process.terminate()
                client.process.wait(timeout=8)
            elif action == 'crash':
                helper = int((root / 'helper-pids.txt').read_text().splitlines()[-1])
                os.kill(pid, 9)
                client.state('fallback')
                client.state('ready')
                replacement = int((root / 'engine-pids.txt').read_text().splitlines()[-1])
                assert replacement != pid
                assert client.diagnostics() == engine_diagnostics(source)
                wait_dead(helper)
                client.close()
                wait_dead(replacement)
            else:
                client.close()
            wait_dead(pid)
        finally:
            client.close()


def test_cli_precedence(binary, root):
    engine = FakeEngine()
    client = Client(binary, root, {'mode': 'off'}, arguments=('--godot-lsp-port', str(engine.port)))
    try:
        client.state('ready')
        client.send('workspace/didChangeConfiguration', {'settings': {'gdscriptLsp': {'diagnostics': {'engine': {'mode': 'off'}}}}})
        assert client.request('gdscript/diagnosticBackend', {})['mode'] == 'attach'
        source = 'extends Node\n# engine CLI\n'
        client.open(source)
        assert client.diagnostics() == engine_diagnostics(source)
    finally:
        client.close()
        engine.close()


def main():
    binary = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix='engine-bridge-tests-') as temporary:
        root = pathlib.Path(temporary).resolve()
        project(root)
        test_attach(binary, root)
        test_failures(binary, root)
        test_managed_lifecycle(binary, root)
        test_cli_precedence(binary, root)
    print('Engine bridge: synchronization, authority, isolation, recovery, and fallback passed')


if __name__ == '__main__':
    main()
