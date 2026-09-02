#!/usr/bin/env python3
import json
import os
import pathlib
import subprocess
import sys
import tempfile


def packet(value):
    body = json.dumps(value, separators=(",", ":")).encode()
    return f"Content-Length: {len(body)}\r\n\r\n".encode() + body


def read_packet(stream):
    length = None
    while True:
        line = stream.readline()
        if not line:
            raise RuntimeError("server closed before response")
        if line == b"\r\n":
            break
        if line.lower().startswith(b"content-length:"):
            length = int(line.split(b":", 1)[1])
    return json.loads(stream.read(length))


binary = pathlib.Path(sys.argv[1]).resolve()
root = pathlib.Path("tests/fixtures/basic").resolve()
uri = (root / "consumer.gd").as_uri()
process = subprocess.Popen(
    [str(binary), "--api", str(root / "extension_api.json")],
    stdin=subprocess.PIPE,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
)

requests = [
    {
        "jsonrpc": "2.0",
        "id": 1,
        "method": "initialize",
        "params": {
            "rootUri": root.as_uri(),
            "workspaceFolders": [{"uri": root.as_uri(), "name": "basic"}],
        },
    },
    {
        "jsonrpc": "2.0",
        "id": 2,
        "method": "textDocument/completion",
        "params": {"textDocument": {"uri": uri}, "position": {"line": 6, "character": 7}},
    },
    {
        "jsonrpc": "2.0",
        "id": 3,
        "method": "gdscript/resolveType",
        "params": {
            "textDocument": {"uri": uri},
            "position": {"line": 6, "character": 4},
            "expression": "local",
        },
    },
    {"jsonrpc": "2.0", "id": 4, "method": "shutdown", "params": {}},
    {"jsonrpc": "2.0", "method": "exit", "params": {}},
]
for request in requests:
    process.stdin.write(packet(request))
process.stdin.flush()

initialize = read_packet(process.stdout)
completion = read_packet(process.stdout)
resolved = read_packet(process.stdout)
shutdown = read_packet(process.stdout)
assert initialize["result"]["capabilities"]["positionEncoding"] == "utf-16"
completion_items = {item["filterText"]: item for item in completion["result"]["items"]}
assert {"own", "count", "label", "reference_method"} <= completion_items.keys()
assert completion_items["label"]["label"] == "label()"
assert completion_items["label"]["insertText"] == "label()"
assert completion_items["label"]["detail"] == ""
assert resolved["result"]["kind"] == "script_class"
assert resolved["result"]["name"] == "ChildThing"
assert shutdown["result"] is None
assert process.wait(timeout=5) == 0

# No --api: the executable must locate its bundled Godot 4.6 metadata and expose
# native members for a project that has never been opened by Godot.
native_root = pathlib.Path("tests/fixtures/native").resolve()
native_uri = (native_root / "main.gd").as_uri()
process = subprocess.Popen(
    [binary.name],
    stdin=subprocess.PIPE,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    env={**os.environ, "PATH": str(binary.parent) + os.pathsep + os.environ.get("PATH", "")},
)
for request in [
    {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"rootUri": native_root.as_uri()}},
    {
        "jsonrpc": "2.0",
        "id": 2,
        "method": "textDocument/completion",
        "params": {"textDocument": {"uri": native_uri}, "position": {"line": 3, "character": 4}},
    },
    {"jsonrpc": "2.0", "id": 3, "method": "shutdown", "params": {}},
    {"jsonrpc": "2.0", "method": "exit", "params": {}},
]:
    process.stdin.write(packet(request))
process.stdin.flush()
read_packet(process.stdout)
native_completion = read_packet(process.stdout)
read_packet(process.stdout)
native_items = {item["filterText"]: item for item in native_completion["result"]["items"]}
assert {"queue_free", "print_tree"} <= native_items.keys()
assert process.wait(timeout=5) == 0

# Diagnostics are available through the LSP 3.17 pull request and are also
# pushed after overlays change. A clean overlay must publish an empty list so
# clients remove errors that came from the disk version.
diagnostic_root = pathlib.Path("tests/fixtures/diagnostics").resolve()
diagnostic_uri = (diagnostic_root / "errors.gd").as_uri()
process = subprocess.Popen(
    [
        str(binary),
        "--project",
        str(diagnostic_root),
        "--api",
        str((root / "extension_api.json").resolve()),
    ],
    stdin=subprocess.PIPE,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
)
# The explicit project remains authoritative even if a fixed-root integration
# sends unrelated initialization metadata.
process.stdin.write(packet({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"rootUri": root.as_uri()}}))
process.stdin.write(
    packet(
        {
            "jsonrpc": "2.0",
            "id": 2,
            "method": "textDocument/diagnostic",
            "params": {"textDocument": {"uri": diagnostic_uri}},
        }
    )
)
process.stdin.flush()
diagnostic_initialize = read_packet(process.stdout)
assert diagnostic_initialize["result"]["capabilities"]["diagnosticProvider"]["interFileDependencies"] is True
pulled = read_packet(process.stdout)
pulled_codes = {item["code"] for item in pulled["result"]["items"]}
assert {"duplicate-symbol", "unknown-type", "type-mismatch"} <= pulled_codes

semantic_uri = (diagnostic_root / "semantic_errors.gd").as_uri()
process.stdin.write(
    packet(
        {
            "jsonrpc": "2.0",
            "id": 3,
            "method": "textDocument/diagnostic",
            "params": {"textDocument": {"uri": semantic_uri}},
        }
    )
)
process.stdin.flush()
semantic_pull = read_packet(process.stdout)
semantic_codes = {item["code"] for item in semantic_pull["result"]["items"]}
assert {
    "undefined-identifier",
    "undefined-function",
    "unknown-member",
    "not-callable",
    "argument-count",
    "argument-type",
    "return-type-mismatch",
    "missing-return-path",
} <= semantic_codes

fixed_source = "extends RefCounted\n\nvar value: int = 1\n"
process.stdin.write(
    packet(
        {
            "jsonrpc": "2.0",
            "method": "textDocument/didOpen",
            "params": {"textDocument": {"uri": diagnostic_uri, "languageId": "gdscript", "version": 4, "text": fixed_source}},
        }
    )
)
process.stdin.write(
    packet(
        {
            "jsonrpc": "2.0",
            "id": 4,
            "method": "textDocument/diagnostic",
            "params": {"textDocument": {"uri": diagnostic_uri}},
        }
    )
)
process.stdin.flush()
clearing_push = None
fixed_pull = None
while clearing_push is None or fixed_pull is None:
    message = read_packet(process.stdout)
    if message.get("method") == "textDocument/publishDiagnostics" and message["params"]["uri"] == diagnostic_uri:
        clearing_push = message
    if message.get("id") == 4:
        fixed_pull = message
assert clearing_push is not None
assert clearing_push["params"]["version"] == 4
assert clearing_push["params"]["diagnostics"] == []
assert fixed_pull["result"]["items"] == []

# A buffer flush immediately followed by completion must not sit behind the
# project-wide diagnostic pass. The edited document may publish first, but
# other documents are deliberately deferred until the idle background scan.
process.stdin.write(
    packet(
        {
            "jsonrpc": "2.0",
            "method": "textDocument/didChange",
            "params": {
                "textDocument": {"uri": diagnostic_uri, "version": 5},
                "contentChanges": [{"text": fixed_source}],
            },
        }
    )
)
process.stdin.write(
    packet(
        {
            "jsonrpc": "2.0",
            "id": 40,
            "method": "textDocument/completion",
            "params": {"textDocument": {"uri": diagnostic_uri}, "position": {"line": 2, "character": 3}},
        }
    )
)
process.stdin.flush()
foreign_diagnostic_before_completion = False
while True:
    message = read_packet(process.stdout)
    if message.get("method") == "textDocument/publishDiagnostics":
        foreign_diagnostic_before_completion |= message["params"]["uri"] != diagnostic_uri
    if message.get("id") == 40:
        assert message["result"]["items"]
        builtin = {item["filterText"]: item for item in message["result"]["items"]}["is_instance_of"]
        assert builtin["label"] == "is_instance_of(\u2026)"
        assert builtin["insertText"] == "is_instance_of("
        break
assert not foreign_diagnostic_before_completion

background_cross_file_diagnostic = None
while background_cross_file_diagnostic is None:
    message = read_packet(process.stdout)
    if (
        message.get("method") == "textDocument/publishDiagnostics"
        and message["params"]["uri"] != diagnostic_uri
    ):
        background_cross_file_diagnostic = message

for request in [
    {"jsonrpc": "2.0", "id": 5, "method": "shutdown", "params": {}},
    {"jsonrpc": "2.0", "method": "exit", "params": {}},
]:
    process.stdin.write(packet(request))
process.stdin.flush()
while True:
    shutdown = read_packet(process.stdout)
    if shutdown.get("id") == 5:
        break
assert shutdown["id"] == 5 and shutdown["result"] is None
assert process.wait(timeout=5) == 0


def initialize_server(params, *, args=(), cwd=None):
    server = subprocess.Popen(
        [str(binary), *map(str, args)],
        cwd=cwd,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    server.stdin.write(packet({"jsonrpc": "2.0", "id": 100, "method": "initialize", "params": params}))
    server.stdin.flush()
    return server, read_packet(server.stdout)


def stop_server(server):
    server.stdin.write(packet({"jsonrpc": "2.0", "id": 101, "method": "shutdown", "params": {}}))
    server.stdin.write(packet({"jsonrpc": "2.0", "method": "exit", "params": {}}))
    server.stdin.flush()
    response = read_packet(server.stdout)
    assert response["id"] == 101 and response["result"] is None
    assert server.wait(timeout=5) == 0


# Older clients may only send rootPath, and clients with no root metadata may
# rely on the server process working directory.
server, response = initialize_server({"rootPath": str(root), "rootUri": None}, cwd="/tmp")
assert response["result"]["serverInfo"]["name"] == "gdscript-lsp"
stop_server(server)

server, response = initialize_server({"rootUri": None, "workspaceFolders": None}, cwd=root)
assert response["result"]["serverInfo"]["name"] == "gdscript-lsp"
stop_server(server)

# File URI decoding must accept the percent escapes emitted for paths with spaces.
with tempfile.TemporaryDirectory(prefix="gdscript lsp ") as temporary:
    linked_root = pathlib.Path(temporary) / "linked project"
    linked_root.symlink_to(root, target_is_directory=True)
    server, response = initialize_server({"rootUri": linked_root.as_uri()}, cwd="/tmp")
    assert response["result"]["serverInfo"]["name"] == "gdscript-lsp"
    stop_server(server)

# A request before initialize gets the standard lifecycle error, without
# preventing a subsequent valid initialization.
server = subprocess.Popen(
    [str(binary)], cwd="/tmp", stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE
)
server.stdin.write(
    packet(
        {
            "jsonrpc": "2.0",
            "id": 102,
            "method": "textDocument/completion",
            "params": {"textDocument": {"uri": uri}, "position": {"line": 0, "character": 0}},
        }
    )
)
server.stdin.write(
    packet({"jsonrpc": "2.0", "id": 103, "method": "initialize", "params": {"rootUri": root.as_uri()}})
)
server.stdin.flush()
assert read_packet(server.stdout)["error"]["code"] == -32002
assert read_packet(server.stdout)["result"]["serverInfo"]["name"] == "gdscript-lsp"
stop_server(server)

# Ambiguous and absent roots fail initialization clearly instead of silently
# selecting a project or exiting before the client receives a response.
server, response = initialize_server(
    {
        "workspaceFolders": [
            {"uri": root.as_uri(), "name": "basic"},
            {"uri": diagnostic_root.as_uri(), "name": "diagnostics"},
        ]
    },
    cwd="/tmp",
)
assert response["error"]["code"] == -32602
assert "multiple Godot project roots" in response["error"]["message"]
server.stdin.write(packet({"jsonrpc": "2.0", "method": "exit", "params": {}}))
server.stdin.flush()
assert server.wait(timeout=5) == 1

with tempfile.TemporaryDirectory() as empty_root:
    server, response = initialize_server({"rootUri": pathlib.Path(empty_root).as_uri()}, cwd=empty_root)
    assert response["error"]["code"] == -32602
    assert "no Godot project found" in response["error"]["message"]
    server.stdin.write(packet({"jsonrpc": "2.0", "method": "exit", "params": {}}))
    server.stdin.flush()
    assert server.wait(timeout=5) == 1

print("lsp smoke test passed")
