#!/usr/bin/env python3
import json
import pathlib
import subprocess
import sys


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
    [str(binary), "--project", str(root), "--api", str(root / "extension_api.json")],
    stdin=subprocess.PIPE,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
)

requests = [
    {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"rootUri": root.as_uri()}},
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
labels = {item["label"] for item in completion["result"]["items"]}
assert {"own", "count", "label", "reference_method"} <= labels
assert resolved["result"]["kind"] == "script_class"
assert resolved["result"]["name"] == "ChildThing"
assert shutdown["result"] is None
assert process.wait(timeout=5) == 0

# No --api: the executable must locate its bundled Godot 4.6 metadata and expose
# native members for a project that has never been opened by Godot.
native_root = pathlib.Path("tests/fixtures/native").resolve()
native_uri = (native_root / "main.gd").as_uri()
process = subprocess.Popen(
    [str(binary), "--project", str(native_root)],
    stdin=subprocess.PIPE,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
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
native_labels = {item["label"] for item in native_completion["result"]["items"]}
assert {"queue_free", "print_tree"} <= native_labels
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
process.stdin.write(
    packet({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"rootUri": diagnostic_root.as_uri()}})
)
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
while True:
    message = read_packet(process.stdout)
    if message.get("method") == "textDocument/publishDiagnostics" and message["params"]["uri"] == diagnostic_uri:
        clearing_push = message
    if message.get("id") == 4:
        fixed_pull = message
        break
assert clearing_push is not None
assert clearing_push["params"]["version"] == 4
assert clearing_push["params"]["diagnostics"] == []
assert fixed_pull["result"]["items"] == []

for request in [
    {"jsonrpc": "2.0", "id": 5, "method": "shutdown", "params": {}},
    {"jsonrpc": "2.0", "method": "exit", "params": {}},
]:
    process.stdin.write(packet(request))
process.stdin.flush()
shutdown = read_packet(process.stdout)
assert shutdown["id"] == 5 and shutdown["result"] is None
assert process.wait(timeout=5) == 0
print("lsp smoke test passed")
