#!/usr/bin/env python3
"""Informational warm-completion benchmark; intentionally has no pass/fail limit."""

import argparse
import json
import math
import pathlib
import subprocess
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
    if length is None:
        raise RuntimeError("response omitted Content-Length")
    return json.loads(stream.read(length))


def response_for(process, request_id):
    while True:
        message = read_packet(process.stdout)
        if message.get("id") == request_id:
            if "error" in message:
                raise RuntimeError(f"request {request_id} failed: {message['error']}")
            return message["result"]


def source_with_prefix(prefix, title_type="String"):
    return (
        "extends RefCounted\n\n"
        "enum BenchState { IDLE, READY }\n\n"
        "class Product:\n"
        f"\tvar title: {title_type}\n"
        "\tfunc tick() -> void: pass\n\n"
        "class BenchRoot:\n"
        "\tclass Utils:\n"
        "\t\tstatic func some_func(value: int) -> void: pass\n\n"
        "func inspect(target: Product) -> void:\n"
        f"\t{prefix}\n"
    )


def completion_position(source):
    line = source.rstrip("\n").splitlines()[-1]
    return {"line": len(source.splitlines()) - 1, "character": len(line)}


def percentile(values, fraction):
    ordered = sorted(values)
    return ordered[max(0, math.ceil(len(ordered) * fraction) - 1)]


def summary(label, values):
    print(
        f"{label}: p50={percentile(values, 0.50):.2f} ms  "
        f"p95={percentile(values, 0.95):.2f} ms  max={max(values):.2f} ms  n={len(values)}"
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=pathlib.Path)
    parser.add_argument("--project", type=pathlib.Path, required=True)
    parser.add_argument("--api", type=pathlib.Path)
    parser.add_argument("--iterations", type=int, default=100)
    parser.add_argument("--warmup", type=int, default=10)
    parser.add_argument("--semantic-iterations", type=int, default=10)
    args = parser.parse_args()
    if args.iterations < 1 or args.warmup < 0 or args.semantic_iterations < 1:
        parser.error("iterations must be positive and warmup must be non-negative")

    binary = args.binary.resolve()
    project = args.project.resolve()
    command = [str(binary), "--project", str(project)]
    if args.api:
        command.extend(("--api", str(args.api.resolve())))
    process = subprocess.Popen(
        command,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    synthetic_uri = (project / ".gdscript_lsp_benchmark.gd").as_uri()
    request_id = 1
    started = time.perf_counter()
    process.stdin.write(
        packet(
            {
                "jsonrpc": "2.0",
                "id": request_id,
                "method": "initialize",
                "params": {"rootUri": project.as_uri()},
            }
        )
    )
    process.stdin.flush()
    response_for(process, request_id)
    initialize_ms = (time.perf_counter() - started) * 1000.0

    current_source = source_with_prefix("target.")
    process.stdin.write(
        packet(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didOpen",
                "params": {
                    "textDocument": {
                        "uri": synthetic_uri,
                        "languageId": "gdscript",
                        "version": 1,
                        "text": current_source,
                    }
                },
            }
        )
    )

    def complete(source):
        nonlocal request_id
        request_id += 1
        process.stdin.write(
            packet(
                {
                    "jsonrpc": "2.0",
                    "id": request_id,
                    "method": "textDocument/completion",
                    "params": {
                        "textDocument": {"uri": synthetic_uri},
                        "position": completion_position(source),
                    },
                }
            )
        )
        process.stdin.flush()
        return response_for(process, request_id)

    # Let the immediate didOpen diagnostic finish before measuring requests.
    # Otherwise one sample can include unrelated diagnostic CPU/output work.
    time.sleep(0.25)
    for _ in range(args.warmup):
        complete(current_source)

    warm_samples = []
    for _ in range(args.iterations):
        started = time.perf_counter()
        complete(current_source)
        warm_samples.append((time.perf_counter() - started) * 1000.0)

    edit_samples = []
    version = 1
    for index in range(args.iterations):
        current_source = source_with_prefix("target.t" if index % 2 == 0 else "target.ti")
        version += 1
        started = time.perf_counter()
        process.stdin.write(
            packet(
                {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didChange",
                    "params": {
                        "textDocument": {"uri": synthetic_uri, "version": version},
                        "contentChanges": [{"text": current_source}],
                    },
                }
            )
        )
        complete(current_source)
        edit_samples.append((time.perf_counter() - started) * 1000.0)

    chain_samples = []
    chain_expressions = ("BenchRoot.", "BenchRoot.Utils.", "BenchRoot.Utils.some_func(")
    for index in range(args.iterations):
        current_source = source_with_prefix(chain_expressions[index % len(chain_expressions)])
        version += 1
        started = time.perf_counter()
        process.stdin.write(
            packet(
                {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didChange",
                    "params": {
                        "textDocument": {"uri": synthetic_uri, "version": version},
                        "contentChanges": [{"text": current_source}],
                    },
                }
            )
        )
        complete(current_source)
        chain_samples.append((time.perf_counter() - started) * 1000.0)

    semantic_samples = []
    for index in range(args.semantic_iterations):
        current_source = source_with_prefix("target.t", "int" if index % 2 == 0 else "String")
        version += 1
        started = time.perf_counter()
        process.stdin.write(
            packet(
                {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didChange",
                    "params": {
                        "textDocument": {"uri": synthetic_uri, "version": version},
                        "contentChanges": [{"text": current_source}],
                    },
                }
            )
        )
        complete(current_source)
        semantic_samples.append((time.perf_counter() - started) * 1000.0)

    request_id += 1
    process.stdin.write(packet({"jsonrpc": "2.0", "id": request_id, "method": "shutdown", "params": {}}))
    process.stdin.write(packet({"jsonrpc": "2.0", "method": "exit", "params": {}}))
    process.stdin.flush()
    response_for(process, request_id)
    process.wait(timeout=10)

    print(f"project: {project}")
    print(f"initialize/index: {initialize_ms:.2f} ms")
    summary("warm completion", warm_samples)
    summary("body didChange + completion", edit_samples)
    summary("member-chain didChange + completion", chain_samples)
    summary("declaration didChange + completion", semantic_samples)


if __name__ == "__main__":
    main()
