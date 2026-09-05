#!/usr/bin/env python3
"""Deterministic semantic recovery corpus and edit sweeps, core + LSP."""

import argparse
import concurrent.futures
import io
import json
import pathlib
import re
import subprocess


FIXTURE = pathlib.Path(__file__).resolve().parent / "fixtures" / "broken_syntax"
RESOURCE = "res://scenario.gd"
MARKER = re.compile(r"<@([a-z_]+)@>")


def position(source, offset):
    prefix = source[:offset]
    return {"line": prefix.count("\n"), "character": len(prefix.rsplit("\n", 1)[-1].encode("utf-16-le")) // 2}


def unpack_markers(marked):
    source, markers, cursor = "", {}, 0
    for match in MARKER.finditer(marked):
        source += marked[cursor:match.start()]
        assert match[1] not in markers, match[1]
        markers[match[1]] = len(source)
        cursor = match.end()
    return source + marked[cursor:], markers


def snapshot(case, fragment, broken):
    before = ("func before() -> Node:\n\tvar before_local = Node.new()\n"
              "\tbefore_local.<@before@>get_name()\n\treturn before_local\n\n")
    factory = ("func factory() -> Node:\n\tvar factory_local = Node.new()\n\treturn factory_local\n\n")
    after = ("func after() -> Node:\n\tvar after_local = fac<@after_call@>tory()\n"
             "\tafter_local.<@after@>get_name()\n\treturn after_local\n\n")
    if case["family"] == "body":
        damaged = "func damaged():\n\tvar n = fac<@call@>tory()\n\t" + fragment.replace("\n", "\n\t") + "<@edit@>\n\n"
    else:
        damaged = fragment + "<@edit@>\n\n"
    layout = case.get("layout", "middle")
    if layout == "eof":
        marked = before + factory + after + damaged.rstrip("\n")
    elif layout == "backward":
        marked = factory + before + damaged + after
    else:
        marked = before + damaged
        if layout == "neighbors":
            marked += "func another():\n\tvar other = Node.new()\n\tother.\n\n"
        marked += factory + after
    owner = RESOURCE
    if layout == "nested":
        marked = "class Holder:\n" + "\n".join("\t" + line if line else "" for line in marked.split("\n"))
        owner += ".Holder"
    marked = "# 😀 source positions\n" + marked
    if layout == "crlf-spaces":
        marked = marked.replace("\t", "    ").replace("\n", "\r\n")
    source, markers = unpack_markers(marked)
    # An unclosed literal can change the meaning of the apparent suffix.
    # The prefix must remain useful; the suffix is required again after repair.
    ambiguous = broken and case.get("ambiguous_tail")
    factory_visible = not ambiguous or layout in ("eof", "backward")
    after_visible = not ambiguous or layout == "eof"
    names = ["before"] + (["factory"] if factory_visible else []) + (["after"] if after_visible else [])
    probes = []
    for label in ("before", "after"):
        if label == "after" and not after_visible:
            continue
        probes.append({"label": label, "position": position(source, markers[label]),
                       "expression": label + "_local", "type": "Node", "members": ["get_name", "queue_free"]})
    if factory_visible:
        start = source.index("func factory") + len("func ")
        target = {"start": position(source, start), "end": position(source, start + len("factory"))}
        for label in ("call", "after_call"):
            if label in markers and (label != "after_call" or after_visible):
                probes.append({"label": label, "position": position(source, markers[label]),
                               "expression": "factory()", "type": "Node", "target": target, "hover": "factory"})
    edit_probe = {"label": "edit", "position": position(source, markers["edit"]), "members": []}
    if case["family"] == "body" and factory_visible:
        edit_probe.update(expression="n", type="Node")
    if broken and case.get("receiver"):
        edit_probe.update(expression=case["receiver"], type="Node", members=["get_name", "queue_free"])
    probes.append(edit_probe)
    symbols = []
    for name in names:
        start = source.index("func " + name) + len("func ")
        symbols.append({"id": owner + "::" + name, "owner": owner, "return": "Node",
                        "selection": {"start": position(source, start), "end": position(source, start + len(name))},
                        "local": name + "_local"})
    return {"source": source, "probes": probes, "symbols": symbols}


def sequences():
    corpus = json.loads((FIXTURE / "cases.json").read_text())
    cases = []
    for family in ("body", "declaration"):
        for entry in corpus[family]:
            cases.append({**entry, "family": family, "name": family + "/" + entry["name"]})
    variants = {"member-dot", "open-call", "type-annotation", "double-quote", "function-open", "parameter-type", "class-body", "getter"}
    for case in list(cases):
        if case["name"].split("/")[-1] in variants:
            for layout in ("eof", "backward", "nested", "neighbors", "crlf-spaces"):
                cases.append({**case, "layout": layout, "name": case["name"] + "/" + layout})
    # One token deletion at a time, never a byte deletion inside Unicode.
    for seed_index, seed in enumerate(corpus["sweep_seeds"]):
        for index, character in enumerate(seed):
            if character in ".()[]{}:,=\"":
                cases.append({"name": f"sweep/delete/{seed_index}/{index}", "family": "body",
                              "valid": seed, "broken": seed[:index] + seed[index + 1:],
                              "ambiguous_tail": character == '"'})
    for family, seed in (("body", "n.get_child(0).get_name()"),
                         ("declaration", "func damaged(value: Node = null) -> Node: return value")):
        for index in range(1, len(seed)):
            cases.append({"name": f"sweep/type/{family}/{index}", "family": family, "valid": seed, "broken": seed[:index]})
    for case in cases:
        broken = snapshot(case, case["broken"], True)
        valid = snapshot(case, case["valid"], False)
        yield {"name": case["name"], "root": str(FIXTURE), "api": str(FIXTURE / "extension_api.json"),
               "uri": (FIXTURE / "scenario.gd").as_uri(), "snapshots": [broken, valid, broken, valid, broken, valid]}


def packet(value):
    body = json.dumps({"jsonrpc": "2.0", **value}, separators=(",", ":")).encode()
    return f"Content-Length: {len(body)}\r\n\r\n".encode() + body


def run_process(command, data):
    process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        out, err = process.communicate(data, timeout=10)
    except subprocess.TimeoutExpired:
        process.kill()
        out, err = process.communicate()
        raise AssertionError(f"timeout after 10 seconds; stderr={err.decode(errors='replace')}") from None
    assert process.returncode == 0, f"exit={process.returncode}; stderr={err.decode(errors='replace')}"
    return out


def read_responses(output):
    stream, responses = io.BytesIO(output), {}
    while stream.tell() < len(output):
        length = None
        while True:
            line = stream.readline()
            assert line, "truncated LSP headers"
            if line == b"\r\n":
                break
            if line.lower().startswith(b"content-length:"):
                length = int(line.split(b":", 1)[1])
        assert length is not None, "missing Content-Length"
        message = json.loads(stream.read(length))
        if "id" in message:
            assert "error" not in message, message
            responses[message["id"]] = message["result"]
    return responses


def ranged_change(old, new):
    start = 0
    while start < min(len(old), len(new)) and old[start] == new[start]:
        start += 1
    old_end, new_end = len(old), len(new)
    while old_end > start and new_end > start and old[old_end - 1] == new[new_end - 1]:
        old_end -= 1
        new_end -= 1
    return {"range": {"start": position(old, start), "end": position(old, old_end)}, "text": new[start:new_end]}


def flatten(items):
    for item in items:
        yield item
        yield from flatten(item.get("children", []))


def lsp_observations(binary, sequence):
    requests, mappings = [], []

    def request(method, params):
        request_id = len(requests) + 1
        requests.append({"id": request_id, "method": method, "params": params})
        return request_id

    request("initialize", {"rootUri": FIXTURE.as_uri()})
    uri, previous = sequence["uri"], ""
    for version, step in enumerate(sequence["snapshots"], 1):
        source = step["source"]
        if version == 1:
            requests.append({"method": "textDocument/didOpen", "params": {"textDocument": {
                "uri": uri, "languageId": "gdscript", "version": version, "text": source}}})
        else:
            change = {"text": source} if version in (2, 5) else ranged_change(previous, source)
            requests.append({"method": "textDocument/didChange", "params": {
                "textDocument": {"uri": uri, "version": version}, "contentChanges": [change]}})
        probes = []
        for probe in step["probes"]:
            params = {"textDocument": {"uri": uri}, "position": probe["position"]}
            ids = {}
            if "members" in probe:
                ids["completion"] = request("textDocument/completion", params)
            if "expression" in probe:
                ids["type"] = request("gdscript/resolveType", {**params, "expression": probe["expression"]})
            if "target" in probe:
                ids["definition"] = request("textDocument/definition", params)
                ids["hover"] = request("textDocument/hover", params)
            probes.append(ids)
        mappings.append({"probes": probes,
                         "symbols": request("gdscript/documentSymbols", {"textDocument": {"uri": uri}}),
                         "diagnostics": request("textDocument/diagnostic", {"textDocument": {"uri": uri}})})
        previous = source
    shutdown = request("shutdown", {})
    requests.append({"method": "exit", "params": {}})
    responses = read_responses(run_process([str(binary), "--api", sequence["api"]], b"".join(map(packet, requests))))
    assert responses[shutdown] is None
    observations = []
    for mapping in mappings:
        probes = []
        for ids in mapping["probes"]:
            actual = {key: responses[value] for key, value in ids.items()}
            if "completion" in actual:
                actual["completion"] = sorted(item["filterText"] for item in actual["completion"]["items"])
            if "type" in actual:
                actual["type"] = actual["type"]["name"]
            if actual.get("hover"):
                actual["hover"] = {"text": actual["hover"]["contents"]["value"], "range": actual["hover"]["range"]}
            probes.append(actual)
        symbols = [{"id": item["symbolId"], "owner": item["ownerId"], "name": item["name"],
                    "type": item["resolvedType"]["name"], "return": (item.get("returnType") or {}).get("name"),
                    "range": item["range"], "selection": item["selectionRange"]}
                   for item in flatten(responses[mapping["symbols"]]["symbols"])]
        diagnostics = [{key: item[key] for key in ("code", "message", "range")}
                       for item in responses[mapping["diagnostics"]]["items"]]
        observations.append({"probes": probes, "symbols": symbols, "diagnostics": diagnostics})
    return observations


def pos_key(p):
    return p["line"], p["character"]


def validate(sequence, observations, path):
    assert len(observations) == len(sequence["snapshots"])
    for step_index, (step, observed) in enumerate(zip(sequence["snapshots"], observations), 1):
        try:
            assert len(observed["probes"]) == len(step["probes"]), "missing semantic probes"
            for probe, actual in zip(step["probes"], observed["probes"]):
                if "members" in probe:
                    assert set(probe["members"]) <= set(actual["completion"]), (probe["label"], "missing completion", actual)
                if "type" in probe:
                    assert actual["type"] == probe["type"], (probe["label"], "lost type", actual)
                if "target" in probe:
                    assert actual["definition"] == [{"uri": sequence["uri"], "range": probe["target"]}], (probe["label"], "wrong definition", actual)
                    assert actual["hover"] and probe["hover"] in actual["hover"]["text"], (probe["label"], "missing hover", actual)
            for expected in step["symbols"]:
                matches = [s for s in observed["symbols"] if s["id"] == expected["id"]]
                assert len(matches) == 1, ("missing/duplicate symbol", expected["id"], observed["symbols"])
                symbol = matches[0]
                for field in ("owner", "return", "selection"):
                    assert symbol[field] == expected[field], ("corrupt symbol", field, expected, symbol)
                locals_ = [s for s in observed["symbols"] if s["name"] == expected["local"]]
                assert len(locals_) == 1 and locals_[0]["owner"] == expected["id"] and locals_[0]["type"] == "Node", ("lost/leaked local", expected, locals_)
                for diagnostic in observed["diagnostics"]:
                    assert not (pos_key(symbol["range"]["start"]) <= pos_key(diagnostic["range"]["start"]) < pos_key(symbol["range"]["end"])), ("diagnostic leaked into stable function", expected["id"], diagnostic)
            end = position(step["source"], len(step["source"]))
            for item in observed["symbols"] + observed["diagnostics"]:
                r = item["range"]
                assert pos_key(r["start"]) <= pos_key(r["end"]) <= pos_key(end), ("invalid range", item)
            # The repaired document must have exactly the same observations on
            # every revisit, including diagnostics and declaration identities.
            if step_index in (4, 6):
                assert observed == observations[1], "repair failed to restore the valid semantic snapshot"
        except AssertionError as error:
            raise AssertionError(f"{path} step {step_index}: {error}\nsource:\n{step['source']}") from None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("server", type=pathlib.Path)
    parser.add_argument("core", type=pathlib.Path)
    parser.add_argument("--case", default="", help="run names containing this substring")
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    cases = [s for s in sequences() if args.case in s["name"]]
    assert cases, "no matching cases"

    def run(sequence):
        failures = []
        observations = {}
        for path in ("core", "LSP"):
            try:
                actual = (json.loads(run_process([str(args.core.resolve())], json.dumps(sequence).encode())) if path == "core"
                          else lsp_observations(args.server.resolve(), sequence))
                validate(sequence, actual, path)
                observations[path] = actual
            except (AssertionError, KeyError, ValueError) as error:
                detail = str(error)
                if "source:\n" not in detail:
                    detail += "\ninitial source:\n" + sequence["snapshots"][0]["source"]
                failures.append(f"FAIL {sequence['name']} [{path}]: {detail}")
        if len(observations) == 2 and observations["core"] != observations["LSP"]:
            step = next(i for i, pair in enumerate(zip(observations["core"], observations["LSP"])) if pair[0] != pair[1])
            failures.append(f"FAIL {sequence['name']}: core/LSP observations differ at step {step + 1}\n"
                            f"core={observations['core'][step]}\nLSP={observations['LSP'][step]}\n"
                            f"source:\n{sequence['snapshots'][step]['source']}")
        return failures

    failures = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for result in pool.map(run, cases):
            failures.extend(result)
            for failure in result:
                print(failure, flush=True)
    print(f"semantic recovery: {len(cases)} sequences, {len(cases) * 6} snapshots per path, {len(failures)} failures")
    return bool(failures)


if __name__ == "__main__":
    raise SystemExit(main())
