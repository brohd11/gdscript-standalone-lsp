#!/usr/bin/env python3
import json
import os
import pathlib
import queue
import subprocess
import sys
import tempfile
import threading
import time


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
]
for request in requests:
    process.stdin.write(packet(request))
process.stdin.flush()

initialize = read_packet(process.stdout)
completion = read_packet(process.stdout)
resolved = read_packet(process.stdout)
assert initialize["result"]["capabilities"]["positionEncoding"] == "utf-16"
completion_items = {item["filterText"]: item for item in completion["result"]["items"]}
completion_order = [item["filterText"] for item in completion["result"]["items"]]
assert {"own", "count", "label", "reference_method"} <= completion_items.keys()
assert completion_items["label"]["label"] == "label()"
assert completion_items["label"]["insertText"] == "label()"
assert completion_items["label"]["detail"] == "func"
assert completion_items["label"]["data"]["gdscriptLsp"]["symbolId"].endswith("::label")
assert completion_items["label"]["data"]["gdscriptLsp"]["provider"] == "semantic"
assert completion_order.index("own") < completion_order.index("CHILD_CONSTANT") < completion_order.index("count")
assert completion_order.index("count") < completion_order.index("BASE_CONSTANT") < completion_order.index("reference_method")
sort_ranks = [item["sortText"] for item in completion["result"]["items"]]
assert sort_ranks == sorted(sort_ranks) and len(sort_ranks) == len(set(sort_ranks))
assert resolved["result"]["kind"] == "script_class"
assert resolved["result"]["name"] == "ChildThing"
process.stdin.write(
    packet(
        {
            "jsonrpc": "2.0",
            "id": 4,
            "method": "completionItem/resolve",
            "params": completion_items["label"],
        }
    )
)
process.stdin.write(
    packet(
        {
            "jsonrpc": "2.0",
            "id": 5,
            "method": "gdscript/resolveExpression",
            "params": {
                "textDocument": {"uri": uri},
                "position": {"line": 6, "character": 4},
                "expression": "local",
            },
        }
    )
)
process.stdin.write(packet({"jsonrpc": "2.0", "id": 6, "method": "shutdown", "params": {}}))
process.stdin.write(packet({"jsonrpc": "2.0", "method": "exit", "params": {}}))
process.stdin.flush()
completion_resolve = read_packet(process.stdout)
rich_resolved = read_packet(process.stdout)
shutdown = read_packet(process.stdout)
assert completion_resolve["result"]["detail"] == "func"
assert rich_resolved["result"]["type"]["name"] == "ChildThing"
assert rich_resolved["result"]["origin"]["name"] == "local"
assert rich_resolved["result"]["origin"]["symbolId"].split("@", 1)[0].endswith("::local")
assert rich_resolved["result"]["accessPaths"][0]["preferred"] is True
assert initialize["result"]["capabilities"]["completionProvider"]["resolveProvider"] is True
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

# A buffer flush immediately followed by completion must not sit behind
# dependency diagnostics. Unrelated documents are not revisited.
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

for request in [
    {"jsonrpc": "2.0", "id": 5, "method": "shutdown", "params": {}},
    {"jsonrpc": "2.0", "method": "exit", "params": {}},
]:
    process.stdin.write(packet(request))
process.stdin.flush()
while True:
    shutdown = read_packet(process.stdout)
    if shutdown.get("method") == "textDocument/publishDiagnostics":
        assert shutdown["params"]["uri"] == diagnostic_uri
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
    while response.get("id") != 101:
        response = read_packet(server.stdout)
    assert response["id"] == 101 and response["result"] is None
    assert server.wait(timeout=5) == 0


# Recursive member chains return the final receiver's members. Once member
# access has been recognized, an unresolved receiver must serialize as an
# explicit empty CompletionList rather than falling back to visible scope.
server, response = initialize_server(
    {"rootUri": root.as_uri()}, args=("--api", root / "extension_api.json")
)
assert response["result"]["serverInfo"]["name"] == "gdscript-lsp"
chain_uri = (root / "return_inference.gd").as_uri()
chain_source = (
    (root / "return_inference.gd").read_text()
    + "\n\tNamespace.Factory.make().functions.keys().\n\tins().missing\n"
)
repeated_call_line = len(chain_source.splitlines()) - 2
unresolved_line = len(chain_source.splitlines()) - 1
server.stdin.write(
    packet(
        {
            "jsonrpc": "2.0",
            "method": "textDocument/didOpen",
            "params": {
                "textDocument": {
                    "uri": chain_uri,
                    "languageId": "gdscript",
                    "version": 8,
                    "text": chain_source,
                }
            },
        }
    )
)
for request_id, position in (
    (110, (12, 36)),
    (111, (unresolved_line, 7)),
    (112, (repeated_call_line, 43)),
):
    server.stdin.write(
        packet(
            {
                "jsonrpc": "2.0",
                "id": request_id,
                "method": "textDocument/completion",
                "params": {
                    "textDocument": {"uri": chain_uri},
                    "position": {"line": position[0], "character": position[1]},
                },
            }
        )
    )
server.stdin.flush()
member_completion = None
unresolved_completion = None
repeated_call_completion = None
while member_completion is None or unresolved_completion is None or repeated_call_completion is None:
    message = read_packet(server.stdout)
    if message.get("id") == 110:
        member_completion = message["result"]
    elif message.get("id") == 111:
        unresolved_completion = message["result"]
    elif message.get("id") == 112:
        repeated_call_completion = message["result"]
assert {item["filterText"] for item in member_completion["items"]} >= {"keys", "size"}
assert unresolved_completion == {"isIncomplete": False, "items": []}
assert {item["filterText"] for item in repeated_call_completion["items"]} == {"append_array"}
stop_server(server)


# Regression for the real Gote failure: a leading blank line used to make the
# expected-value scan revisit byte one forever. That stranded later changes,
# diagnostics, cancellation, and shutdown behind the completion request.
server, response = initialize_server(
    {
        "rootUri": root.as_uri(),
        "initializationOptions": {"gdscriptLsp": {"diagnostics": {"pollIntervalMs": 0}}},
    },
    args=("--api", root / "extension_api.json"),
)
assert response["result"]["serverInfo"]["name"] == "gdscript-lsp"
leading_uri = (root / "consumer.gd").as_uri()
leading_source = (
    "\nextends RefCounted\n\nfunc inspect() -> void:\n"
    "\tvar class_obj: Dictionary = {}\n\tvar class = 1\n"
)
messages = queue.Queue()


def read_leading_messages():
    try:
        while True:
            messages.put(read_packet(server.stdout))
    except (EOFError, RuntimeError):
        return


reader = threading.Thread(target=read_leading_messages, daemon=True)
reader.start()


def leading_message(predicate, timeout=2.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            message = messages.get(timeout=max(0.01, deadline - time.monotonic()))
        except queue.Empty:
            break
        if predicate(message):
            return message
    raise AssertionError("timed out waiting for leading-newline LSP response")


server.stdin.write(packet({"jsonrpc": "2.0", "method": "initialized", "params": {}}))
server.stdin.write(
    packet(
        {
            "jsonrpc": "2.0",
            "method": "textDocument/didOpen",
            "params": {
                "textDocument": {
                    "uri": leading_uri,
                    "languageId": "gdscript",
                    "version": 1,
                    "text": leading_source,
                }
            },
        }
    )
)
server.stdin.flush()
initial_diagnostic = leading_message(
    lambda message: message.get("method") == "textDocument/publishDiagnostics"
    and message["params"]["uri"] == leading_uri
    and message["params"].get("version") == 1
)
assert any(item["code"] == "syntax-error" for item in initial_diagnostic["params"]["diagnostics"])

incomplete_source = leading_source.replace("\tvar class = 1\n", "\tc\n")
server.stdin.write(
    packet(
        {
            "jsonrpc": "2.0",
            "method": "textDocument/didChange",
            "params": {
                "textDocument": {"uri": leading_uri, "version": 2},
                "contentChanges": [{"text": incomplete_source}],
            },
        }
    )
)
server.stdin.write(
    packet(
        {
            "jsonrpc": "2.0",
            "id": 130,
            "method": "textDocument/completion",
            "params": {"textDocument": {"uri": leading_uri}, "position": {"line": 5, "character": 2}},
        }
    )
)
server.stdin.flush()
scope_completion = leading_message(lambda message: message.get("id") == 130)
assert "class_obj" in {item["filterText"] for item in scope_completion["result"]["items"]}

valid_source = incomplete_source.replace("\tc\n", "\tclass_obj\n")
server.stdin.write(
    packet(
        {
            "jsonrpc": "2.0",
            "method": "textDocument/didChange",
            "params": {
                "textDocument": {"uri": leading_uri, "version": 3},
                "contentChanges": [{"text": valid_source}],
            },
        }
    )
)
server.stdin.flush()
current_diagnostic = leading_message(
    lambda message: message.get("method") == "textDocument/publishDiagnostics"
    and message["params"]["uri"] == leading_uri
    and message["params"].get("version") == 3
)
assert all('Identifier "c"' not in item["message"] for item in current_diagnostic["params"]["diagnostics"])

member_source = valid_source.replace("\tclass_obj\n", "\tclass_obj.\n")
server.stdin.write(
    packet(
        {
            "jsonrpc": "2.0",
            "method": "textDocument/didChange",
            "params": {
                "textDocument": {"uri": leading_uri, "version": 4},
                "contentChanges": [{"text": member_source}],
            },
        }
    )
)
server.stdin.write(
    packet(
        {
            "jsonrpc": "2.0",
            "id": 131,
            "method": "textDocument/completion",
            "params": {"textDocument": {"uri": leading_uri}, "position": {"line": 5, "character": 11}},
        }
    )
)
server.stdin.flush()
member_completion = leading_message(lambda message: message.get("id") == 131)
assert member_completion["result"]["isIncomplete"] is False

recovered_type_source = (
    "extends RefCounted\n\nfunc inspect() -> void:\n"
    "\tvar class_obj: Dictionary = {}\n"
    "\tvar test_var:\n\t# do not absorb this comment\n"
    "\tvar n = Missing.Type.VALUE\n\tmissing_after\n"
)
server.stdin.write(
    packet(
        {
            "jsonrpc": "2.0",
            "method": "textDocument/didChange",
            "params": {
                "textDocument": {"uri": leading_uri, "version": 5},
                "contentChanges": [{"text": recovered_type_source}],
            },
        }
    )
)
server.stdin.flush()
recovered_type_diagnostic = leading_message(
    lambda message: message.get("method") == "textDocument/publishDiagnostics"
    and message["params"]["uri"] == leading_uri
    and message["params"].get("version") == 5
)
recovered_items = recovered_type_diagnostic["params"]["diagnostics"]
assert any(item["code"] == "syntax-error" and item["message"] == 'Expected type after ":".' for item in recovered_items)
assert not any(item["code"] == "unknown-type" and "do not absorb" in item["message"] for item in recovered_items)
assert any(item["code"] == "undefined-identifier" and "missing_after" in item["message"] for item in recovered_items)

recovered_value_source = (
    "extends RefCounted\n\nfunc inspect() -> void:\n"
    "\tvar class_obj: Dictionary = {}\n\tvar test_var =\n\tmissing_after\n"
)
server.stdin.write(
    packet(
        {
            "jsonrpc": "2.0",
            "method": "textDocument/didChange",
            "params": {
                "textDocument": {"uri": leading_uri, "version": 6},
                "contentChanges": [{"text": recovered_value_source}],
            },
        }
    )
)
server.stdin.flush()
recovered_value_diagnostic = leading_message(
    lambda message: message.get("method") == "textDocument/publishDiagnostics"
    and message["params"]["uri"] == leading_uri
    and message["params"].get("version") == 6
)
assert any(
    item["code"] == "syntax-error" and item["message"] == 'Expected expression after "=".'
    for item in recovered_value_diagnostic["params"]["diagnostics"]
)

post_recovery_source = recovered_value_source.replace("\tvar test_var =\n", "\tvar test_var = {}\n").replace(
    "\tmissing_after\n", "\tclass_obj.k\n"
)
server.stdin.write(
    packet(
        {
            "jsonrpc": "2.0",
            "method": "textDocument/didChange",
            "params": {
                "textDocument": {"uri": leading_uri, "version": 7},
                "contentChanges": [{"text": post_recovery_source}],
            },
        }
    )
)
server.stdin.write(
    packet(
        {
            "jsonrpc": "2.0",
            "id": 132,
            "method": "textDocument/completion",
            "params": {"textDocument": {"uri": leading_uri}, "position": {"line": 5, "character": 12}},
        }
    )
)
server.stdin.flush()
post_recovery_completion = leading_message(lambda message: message.get("id") == 132)
assert "keys" in {item["filterText"] for item in post_recovery_completion["result"]["items"]}, post_recovery_completion

incomplete_comparison_source = (
    "enum EditState { READY, STOPPED }\n\nfunc inspect() -> void:\n"
    "\tvar state = EditState.READY\n\tif state == "
)
server.stdin.write(
    packet(
        {
            "jsonrpc": "2.0",
            "method": "textDocument/didChange",
            "params": {
                "textDocument": {"uri": leading_uri, "version": 8},
                "contentChanges": [{"text": incomplete_comparison_source}],
            },
        }
    )
)
server.stdin.write(
    packet(
        {
            "jsonrpc": "2.0",
            "id": 133,
            "method": "textDocument/completion",
            "params": {"textDocument": {"uri": leading_uri}, "position": {"line": 4, "character": 13}},
        }
    )
)
server.stdin.flush()
comparison_completion = leading_message(lambda message: message.get("id") == 133)
assert {"EditState.READY", "EditState.STOPPED"} <= {
    item["filterText"] for item in comparison_completion["result"]["items"]
}

incomplete_match_source = (
    "enum EditState { READY, STOPPED }\n\nfunc inspect() -> void:\n"
    "\tvar state = EditState.READY\n\tmatch state:\n\t\ts"
)
server.stdin.write(
    packet(
        {
            "jsonrpc": "2.0",
            "method": "textDocument/didChange",
            "params": {
                "textDocument": {"uri": leading_uri, "version": 9},
                "contentChanges": [{"text": incomplete_match_source}],
            },
        }
    )
)
server.stdin.write(
    packet(
        {
            "jsonrpc": "2.0",
            "id": 134,
            "method": "textDocument/completion",
            "params": {"textDocument": {"uri": leading_uri}, "position": {"line": 5, "character": 3}},
        }
    )
)
server.stdin.flush()
match_completion = leading_message(lambda message: message.get("id") == 134)
assert {"EditState.READY", "EditState.STOPPED"} <= {
    item["filterText"] for item in match_completion["result"]["items"]
}

server.stdin.write(packet({"jsonrpc": "2.0", "id": 135, "method": "shutdown", "params": {}}))
server.stdin.flush()
shutdown = leading_message(lambda message: message.get("id") == 135)
assert shutdown["result"] is None
server.stdin.write(packet({"jsonrpc": "2.0", "method": "exit", "params": {}}))
server.stdin.flush()
assert server.wait(timeout=2) == 0


# The portable disk poll discovers scripts created and removed outside the
# client. Adding a global class must invalidate its previously unresolved
# consumers; deleting it must invalidate those consumers through the old graph.
with tempfile.TemporaryDirectory(prefix="gdscript-lsp-poll-") as temporary:
    poll_root = pathlib.Path(temporary)
    (poll_root / "project.godot").write_text('[application]\nconfig/name="Poll fixture"\n')
    consumer_path = poll_root / "consumer.gd"
    consumer_path.write_text("extends RefCounted\n\nvar item: PollType\n")
    poll_server, response = initialize_server(
        {
            "rootUri": poll_root.as_uri(),
            "initializationOptions": {"gdscriptLsp": {"diagnostics": {"pollIntervalMs": 100}}},
        },
        args=("--api", root / "extension_api.json"),
    )
    assert response["result"]["serverInfo"]["name"] == "gdscript-lsp"
    poll_messages = queue.Queue()

    def read_poll_messages():
        try:
            while True:
                poll_messages.put(read_packet(poll_server.stdout))
        except (EOFError, RuntimeError):
            return

    poll_reader = threading.Thread(target=read_poll_messages, daemon=True)
    poll_reader.start()

    def poll_diagnostic(predicate, timeout=4.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                message = poll_messages.get(timeout=max(0.01, deadline - time.monotonic()))
            except queue.Empty:
                break
            if (
                message.get("method") == "textDocument/publishDiagnostics"
                and message["params"]["uri"] == consumer_path.as_uri()
                and predicate(message["params"]["diagnostics"])
            ):
                return message
        raise AssertionError("timed out waiting for dependency-aware polled diagnostics")

    poll_server.stdin.write(packet({"jsonrpc": "2.0", "method": "initialized", "params": {}}))
    poll_server.stdin.flush()
    poll_diagnostic(lambda diagnostics: any(item["code"] == "unknown-type" for item in diagnostics))

    external_type = poll_root / "poll_type.gd"
    external_type.write_text("class_name PollType\nextends RefCounted\n")
    poll_diagnostic(lambda diagnostics: diagnostics == [])

    external_type.unlink()
    poll_diagnostic(lambda diagnostics: any(item["code"] == "unknown-type" for item in diagnostics))

    poll_server.stdin.write(packet({"jsonrpc": "2.0", "id": 140, "method": "shutdown", "params": {}}))
    poll_server.stdin.flush()
    deadline = time.monotonic() + 2
    while True:
        response = poll_messages.get(timeout=max(0.01, deadline - time.monotonic()))
        if response.get("id") == 140:
            break
    assert response["result"] is None
    poll_server.stdin.write(packet({"jsonrpc": "2.0", "method": "exit", "params": {}}))
    poll_server.stdin.flush()
    assert poll_server.wait(timeout=2) == 0


# Completion provider settings use initializationOptions and the standard live
# configuration notification; changing a feature does not require reindexing.
server, response = initialize_server(
    {
        "rootUri": diagnostic_root.as_uri(),
        "initializationOptions": {"gdscriptLsp": {"completion": {"enums": False}}},
    },
    args=("--api", root / "extension_api.json"),
)
assert response["result"]["serverInfo"]["name"] == "gdscript-lsp"
enum_source = (diagnostic_root / "semantic_valid.gd").read_text()
enum_uri = (diagnostic_root / "semantic_valid.gd").as_uri()
enum_line = next(i for i, line in enumerate(enum_source.splitlines()) if "var mode: FileAccess.ModeFlags" in line)
enum_column = enum_source.splitlines()[enum_line].index("= ") + 2
server.stdin.write(
    packet(
        {
            "jsonrpc": "2.0",
            "id": 120,
            "method": "textDocument/completion",
            "params": {"textDocument": {"uri": enum_uri}, "position": {"line": enum_line, "character": enum_column}},
        }
    )
)
server.stdin.flush()
disabled_items = {item["filterText"] for item in read_packet(server.stdout)["result"]["items"]}
assert "FileAccess.WRITE" not in disabled_items
server.stdin.write(
    packet(
        {
            "jsonrpc": "2.0",
            "method": "workspace/didChangeConfiguration",
            "params": {"settings": {"gdscriptLsp": {"completion": {"enums": True}}}},
        }
    )
)
server.stdin.write(
    packet(
        {
            "jsonrpc": "2.0",
            "id": 121,
            "method": "textDocument/completion",
            "params": {"textDocument": {"uri": enum_uri}, "position": {"line": enum_line, "character": enum_column}},
        }
    )
)
server.stdin.flush()
enabled_items = {item["filterText"] for item in read_packet(server.stdout)["result"]["items"]}
assert {"FileAccess.READ", "FileAccess.WRITE"} <= enabled_items
stop_server(server)

# The serialized completion path uses the same nested comparison, member,
# structural-colon, and foreign constructor contexts as the core API.
caret_root = pathlib.Path("tests/fixtures/caret_completion").resolve()
caret_uri = (caret_root / "main.gd").as_uri()
caret_source = (
    "extends RefCounted\n\n"
    "const TF = ContextRoot.Utils.Profile.TimeFunction.TimeScale\n\n"
    "enum LocalState { IDLE, READY }\n"
    "enum OtherState { FIRST, SECOND }\n\n"
    "func inspect() -> void:\n"
    "\tvar n := OtherState.FIRST\n"
    "\tvar em: LocalState\n"
    "\tif (em != LocalState.IDLE) or n == OtherState.FIRST:\n"
    "\t\tpass\n"
    "\tContextRoot.Utils.Profile.TimeFunction.new(\"\", TF.USEC)\n"
    "\tContextRoot.Utils.Profile.TimeFunction.new(\"\", \"bad\")\n"
)


def caret_position(needle):
    offset = caret_source.index(needle) + len(needle)
    before = caret_source[:offset]
    return {"line": before.count("\n"), "character": len(before.rsplit("\n", 1)[-1])}


server, response = initialize_server(
    {"rootUri": caret_root.as_uri()}, args=("--api", root / "extension_api.json")
)
assert response["result"]["serverInfo"]["name"] == "gdscript-lsp"
server.stdin.write(
    packet(
        {
            "jsonrpc": "2.0",
            "method": "textDocument/didOpen",
            "params": {
                "textDocument": {
                    "uri": caret_uri,
                    "languageId": "gdscript",
                    "version": 1,
                    "text": caret_source,
                }
            },
        }
    )
)
for request_id, needle in (
    (140, "n == "),
    (141, 'new("", '),
    (143, "OtherState.FIRST:"),
    (144, "TF."),
):
    server.stdin.write(
        packet(
            {
                "jsonrpc": "2.0",
                "id": request_id,
                "method": "textDocument/completion",
                "params": {"textDocument": {"uri": caret_uri}, "position": caret_position(needle)},
            }
        )
    )
server.stdin.write(
    packet(
        {
            "jsonrpc": "2.0",
            "id": 142,
            "method": "textDocument/diagnostic",
            "params": {"textDocument": {"uri": caret_uri}},
        }
    )
)
server.stdin.flush()
caret_responses = {}
while len(caret_responses) < 5:
    message = read_packet(server.stdout)
    if message.get("id") in (140, 141, 142, 143, 144):
        caret_responses[message["id"]] = message["result"]
comparison_items = {item["filterText"] for item in caret_responses[140]["items"]}
constructor_items = {item["filterText"] for item in caret_responses[141]["items"]}
colon_items = {item["filterText"] for item in caret_responses[143]["items"]}
member_items = {item["filterText"] for item in caret_responses[144]["items"]}
assert "OtherState.FIRST" in comparison_items
assert {"TF.MSEC", "ContextRoot.Utils.Profile.TimeFunction.TimeScale.MSEC"} <= constructor_items
assert not colon_items
assert {"MSEC", "USEC"} <= member_items
assert "TF.MSEC" not in member_items
assert any(item["code"] == "argument-type" for item in caret_responses[142]["items"])
stop_server(server)

# Each portable completion provider is serialized at least once through the
# stdio transport. Exhaustive context behavior remains in the C++ provider
# suite so this section stays a protocol smoke test rather than a duplicate.
provider_root = pathlib.Path("tests/fixtures/completion_providers").resolve()
provider_uri = (provider_root / "main.gd").as_uri()
provider_source = (
    "extends CompletionProviderBase\n\n"
    "enum State { IDLE, READY }\n\n"
    "class Product:\n"
    "\tvar title: String\n"
    "\tvar _private: int\n"
    "\tfunc _init(required: int) -> void: pass\n"
    "\tfunc build() -> void: pass\n\n"
    "func inspect(target: Product) -> void:\n"
    "\tvar state: State = \n"
    "\tvar typed: \n"
    "\tvar product: Product = \n"
    "\ttarget.call(\"\")\n"
    "\tprint(target.)\n"
    "\tif true:\n"
)


def provider_position(needle):
    offset = provider_source.index(needle) + len(needle)
    before = provider_source[:offset]
    return {"line": before.count("\n"), "character": len(before.rsplit("\n", 1)[-1])}


server, response = initialize_server(
    {"rootUri": provider_root.as_uri()}, args=("--api", root / "extension_api.json")
)
assert response["result"]["serverInfo"]["name"] == "gdscript-lsp"
server.stdin.write(
    packet(
        {
            "jsonrpc": "2.0",
            "method": "textDocument/didOpen",
            "params": {
                "textDocument": {
                    "uri": provider_uri,
                    "languageId": "gdscript",
                    "version": 1,
                    "text": provider_source,
                }
            },
        }
    )
)
provider_needles = {
    150: "var state: State = ",
    151: "var typed: ",
    152: "var product: Product = ",
    153: 'target.call("',
    154: "print(target.",
    155: "if true:",
}
for request_id, needle in provider_needles.items():
    server.stdin.write(
        packet(
            {
                "jsonrpc": "2.0",
                "id": request_id,
                "method": "textDocument/completion",
                "params": {
                    "textDocument": {"uri": provider_uri},
                    "position": provider_position(needle),
                },
            }
        )
    )
server.stdin.flush()
provider_responses = {}
while len(provider_responses) < len(provider_needles):
    message = read_packet(server.stdout)
    if message.get("id") in provider_needles:
        provider_responses[message["id"]] = message["result"]


def items_by_filter(response):
    return {item["filterText"]: item for item in response["items"]}


enum_items = items_by_filter(provider_responses[150])
type_items = items_by_filter(provider_responses[151])
constructor_items = items_by_filter(provider_responses[152])
string_items = items_by_filter(provider_responses[153])
private_items = items_by_filter(provider_responses[154])
assert enum_items["State.IDLE"]["data"]["gdscriptLsp"]["provider"] == "enums"
assert type_items["Product"]["data"]["gdscriptLsp"]["provider"] == "extendedTypeHints"
assert constructor_items["Product.new"]["data"]["gdscriptLsp"]["provider"] == "constructors"
assert constructor_items["Product.new"]["insertText"] == "Product.new("
assert string_items["build"]["data"]["gdscriptLsp"]["provider"] == "memberStrings"
assert string_items["build"]["insertText"] == "build"
assert "title" in private_items and "_private" not in private_items
assert provider_responses[155]["items"] == []
stop_server(server)


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
