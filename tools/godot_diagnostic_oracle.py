#!/usr/bin/env python3
"""Compare stable diagnostic categories with Godot's headless parser."""

import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile


def packet(value):
    body = json.dumps(value, separators=(",", ":")).encode()
    return f"Content-Length: {len(body)}\r\n\r\n".encode() + body


def packets(data):
    values = []
    offset = 0
    while offset < len(data):
        header_end = data.index(b"\r\n\r\n", offset)
        header = data[offset:header_end].decode()
        match = re.search(r"(?im)^Content-Length:\s*(\d+)\s*$", header)
        if not match:
            raise RuntimeError("language server response omitted Content-Length")
        length = int(match.group(1))
        body_start = header_end + 4
        values.append(json.loads(data[body_start : body_start + length]))
        offset = body_start + length
    return values


repository = pathlib.Path(__file__).resolve().parent.parent
golden = json.loads((repository / "tests/diagnostic_oracle.json").read_text())
fixture = repository / "tests/fixtures/diagnostics"
lsp = pathlib.Path(sys.argv[1]).resolve()
godot = sys.argv[2] if len(sys.argv) > 2 else os.environ.get("GODOT", "godot")

version = subprocess.run([godot, "--version"], text=True, capture_output=True, check=True).stdout.strip()
if not version.startswith(golden["godot_version_prefix"]):
    raise RuntimeError(f"oracle requires Godot {golden['godot_version_prefix']}x, found {version}")

requests = [{"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"rootUri": fixture.as_uri()}}]
for request_id, case in enumerate(golden["cases"], 2):
    requests.append(
        {
            "jsonrpc": "2.0",
            "id": request_id,
            "method": "textDocument/diagnostic",
            "params": {"textDocument": {"uri": (fixture / case["script"]).as_uri()}},
        }
    )
requests.extend(
    [
        {"jsonrpc": "2.0", "id": 100, "method": "shutdown", "params": {}},
        {"jsonrpc": "2.0", "method": "exit", "params": {}},
    ]
)
server = subprocess.run(
    [str(lsp), "--project", str(fixture), "--api", str(repository / "tests/fixtures/basic/extension_api.json")],
    input=b"".join(packet(request) for request in requests),
    capture_output=True,
    check=True,
)
responses = {value["id"]: value for value in packets(server.stdout) if "id" in value}

with tempfile.TemporaryDirectory(prefix="gdscript-lsp-oracle-") as data_home:
    environment = os.environ.copy()
    environment["XDG_DATA_HOME"] = data_home
    for request_id, case in enumerate(golden["cases"], 2):
        result = subprocess.run(
            [
                godot,
                "--headless",
                "--no-header",
                "--path",
                str(fixture),
                "--check-only",
                "--script",
                "res://" + case["script"],
            ],
            text=True,
            capture_output=True,
            env=environment,
        )
        output = result.stdout + result.stderr
        pattern = case["godot_pattern"]
        if pattern is None:
            assert result.returncode == 0, f"Godot rejected valid case {case['script']}:\n{output}"
        else:
            assert result.returncode != 0, f"Godot accepted invalid case {case['script']}"
            assert re.search(pattern, output), f"Godot output changed for {case['script']}:\n{output}"

        codes = {item["code"] for item in responses[request_id]["result"]["items"]}
        expected = case["lsp_code"]
        if expected is None:
            assert not codes, f"language server rejected valid case {case['script']}: {sorted(codes)}"
        else:
            assert expected in codes, f"language server omitted {expected} for {case['script']}: {sorted(codes)}"
        print(f"ok  {case['script']}: {expected or 'valid'}")

print(f"diagnostic oracle passed against Godot {version}")
